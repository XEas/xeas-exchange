#include "xeas/replay.h"

#include <chrono>
#include <cstddef>
#include <utility>

namespace xeas {

namespace {

std::string format_replay_error(std::uint16_t locate, const std::string& symbol,
                                const std::vector<std::string>& violations,
                                std::uint64_t message_index) {
    std::string msg = "structural invariant violation in " + symbol +
                      " (locate " + std::to_string(locate) + ") at message " +
                      std::to_string(message_index) + ":";
    for (const auto& v : violations) msg += "\n  - " + v;
    return msg;
}

// Prefix produced by check_invariants() for the one finding that is data
// reality (halts, opening/closing crosses), not a bug — see src/book.cpp.
constexpr const char* kCrossedPrefix = "book crossed or locked";

}  // namespace

ReplayError::ReplayError(std::uint16_t locate, std::string symbol,
                         std::vector<std::string> violations,
                         std::uint64_t message_index)
    : std::runtime_error(format_replay_error(locate, symbol, violations, message_index)),
      locate_(locate),
      symbol_(std::move(symbol)),
      violations_(std::move(violations)),
      message_index_(message_index) {}

void replay_stream(ItchReader& reader, BookRouter& router,
                   const ReplayOptions& opts, ReplayStats& stats,
                   std::ostream& log) {
    const auto start = std::chrono::steady_clock::now();
    const auto stamp = [&] {
        stats.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                .count();
        stats.bytes_consumed = reader.bytes_consumed();
        stats.books_created = router.book_count();
    };

    // Per-locate crossed state for the O(1) probe on the touched book only.
    std::vector<bool> crossed_now(65'536, false);
    std::vector<bool> crossed_ever(65'536, false);
    std::vector<bool> crossed_logged(65'536, false);

    std::int64_t live_orders = 0;
    Timestamp last_timestamp = 0;
    bool have_timestamp = false;

    // Sweep every book. "book crossed or locked" findings are data reality:
    // logged on first occurrence per book, never fatal. Anything else is a
    // structural bug: throw ReplayError (spec section 4).
    const auto sweep = [&] {
        ++stats.invariant_sweeps;
        router.for_each_book([&](std::uint16_t locate, const Book& book) {
            std::vector<std::string> violations = check_invariants(book);
            bool crossed = false;
            std::vector<std::string> structural;
            for (auto& v : violations) {
                if (v.rfind(kCrossedPrefix, 0) == 0) {
                    crossed = true;
                } else {
                    structural.push_back(std::move(v));
                }
            }
            if (crossed && !crossed_logged[locate]) {
                crossed_logged[locate] = true;
                log << "note: " << router.symbol_or_locate(locate)
                    << " crossed or locked at timestamp " << last_timestamp << "\n";
            }
            if (!structural.empty()) {
                throw ReplayError(locate, router.symbol_or_locate(locate),
                                  std::move(structural), stats.messages_total);
            }
        });
    };

    try {
        while (const auto frame = reader.next()) {
            const char type = std::to_integer<char>(frame->data[0]);
            ++stats.messages_total;
            ++stats.count_by_type[static_cast<unsigned char>(type)];

            if (type == 'R') {
                StockDirectoryEntry entry;
                try {
                    entry = decode_stock_directory(frame->data, frame->size);
                } catch (const FeedError& e) {
                    // Re-throw with the position only this layer knows.
                    throw FeedError(e.reason(),
                                    reader.bytes_consumed() - frame->size - 2,
                                    stats.messages_total - 1, e.message_type());
                }
                router.set_symbol(entry.stock_locate, std::move(entry.symbol));
            } else if (is_book_message(type)) {
                BookMessage msg;
                try {
                    msg = decode_book_message(frame->data, frame->size);
                } catch (const FeedError& e) {
                    throw FeedError(e.reason(),
                                    reader.bytes_consumed() - frame->size - 2,
                                    stats.messages_total - 1, e.message_type());
                }

                if (have_timestamp && msg.event.timestamp < last_timestamp) {
                    ++stats.timestamp_regressions;  // reported, never fatal
                }
                last_timestamp = msg.event.timestamp;
                have_timestamp = true;

                Book& book = router.book_for(msg.stock_locate);
                const auto before = static_cast<std::int64_t>(book.order_count());
                apply(book, msg.event);  // BookError propagates (abort)
                ++stats.events_applied;

                live_orders += static_cast<std::int64_t>(book.order_count()) - before;
                if (live_orders > 0 &&
                    static_cast<std::uint64_t>(live_orders) > stats.live_orders_peak) {
                    stats.live_orders_peak = static_cast<std::uint64_t>(live_orders);
                }

                // O(1) crossed probe on the touched book only: count an
                // episode on each false->true transition.
                const bool crossed = is_crossed(book);
                if (crossed && !crossed_now[msg.stock_locate]) {
                    ++stats.crossed_episodes;
                    if (!crossed_ever[msg.stock_locate]) {
                        crossed_ever[msg.stock_locate] = true;
                        ++stats.crossed_symbols;
                    }
                }
                crossed_now[msg.stock_locate] = crossed;
            }
            // All other well-framed types: already counted; skipping is free.

            if (opts.invariant_sweep_every != 0 &&
                stats.messages_total % opts.invariant_sweep_every == 0) {
                sweep();
            }
            if (opts.progress_every != 0 &&
                stats.messages_total % opts.progress_every == 0) {
                stamp();
                log << "progress: " << stats.messages_total << " messages, "
                    << stats.events_applied << " events, "
                    << router.book_count() << " books, " << live_orders
                    << " live orders\n";
            }
        }
        sweep();  // final sweep at EOF, always
        stats.live_orders_final =
            live_orders > 0 ? static_cast<std::uint64_t>(live_orders) : 0;
        stamp();
    } catch (...) {
        stats.live_orders_final =
            live_orders > 0 ? static_cast<std::uint64_t>(live_orders) : 0;
        stamp();
        throw;  // stats out-param keeps everything counted so far
    }
}

}  // namespace xeas
