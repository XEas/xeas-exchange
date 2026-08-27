// itch_replay <file> [--sweep-every N] [--progress N] [--quiet]
//
// Thin shell around replay_stream(): argv parsing, exception -> exit-code
// mapping, stats printing. All replay logic lives (and is tested) in
// xeas_feed; this file is smoke-tested manually against a real day.

#include "xeas/book.h"
#include "xeas/itch.h"
#include "xeas/replay.h"
#include "xeas/router.h"

#include <sys/resource.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

using namespace xeas;

int usage() {
    std::cerr << "usage: itch_replay <file> [--sweep-every N] [--progress N] [--quiet]\n"
                 "  <file>: an uncompressed .NASDAQ_ITCH50 file (gunzip *.gz first)\n"
                 "  --sweep-every N   messages between invariant sweeps (default 25000000)\n"
                 "  --progress N      messages between progress lines (default 10000000)\n"
                 "  --quiet           no progress lines\n";
    return 1;
}

bool parse_count(const char* s, std::uint64_t& out) {
    if (s == nullptr || s[0] == '\0') return false;
    if (s[0] == '-' || s[0] == '+') return false;
    char* end;
    errno = 0;
    std::uint64_t val = std::strtoull(s, &end, 10);
    if (errno == ERANGE) return false;
    if (*end != '\0') return false;
    out = val;
    return true;
}

constexpr struct { char type; const char* name; } kTypeNames[] = {
    {'S', "System Event"},
    {'R', "Stock Directory"},
    {'H', "Stock Trading Action"},
    {'Y', "Reg SHO Restriction"},
    {'L', "Market Participant Position"},
    {'V', "MWCB Decline Level"},
    {'W', "MWCB Status"},
    {'K', "IPO Quoting Period Update"},
    {'J', "LULD Auction Collar"},
    {'h', "Operational Halt"},
    {'A', "Add Order"},
    {'F', "Add Order (MPID)"},
    {'E', "Order Executed"},
    {'C', "Order Executed with Price"},
    {'X', "Order Cancel"},
    {'D', "Order Delete"},
    {'U', "Order Replace"},
    {'P', "Trade (non-cross)"},
    {'Q', "Cross Trade"},
    {'B', "Broken Trade"},
    {'I', "Net Order Imbalance"},
    {'N', "Retail Price Improvement"},
    {'O', "Direct Listing with Capital Raise"},
};

const char* type_name(char type) {
    for (const auto& entry : kTypeNames) {
        if (entry.type == type) return entry.name;
    }
    return nullptr;
}

// ru_maxrss is bytes on macOS and KiB on Linux — normalized to bytes here.
// Platform code stays out of the libraries (spec §2): this lives in main only.
std::uint64_t peak_rss_bytes() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
}

void print_stats(const ReplayStats& stats, const BookRouter& router,
                 std::ostream& out) {
    out << "--- message counts by type ---\n";
    for (int t = 0; t < 256; ++t) {
        if (stats.count_by_type[t] == 0) continue;
        const char c = static_cast<char>(t);
        const char* name = type_name(c);
        out << "  '" << c << "'  " << stats.count_by_type[t] << "  "
            << (name != nullptr ? name : "(unknown type)") << "\n";
    }
    std::uint64_t final_orders = 0;
    std::uint64_t final_levels = 0;
    router.for_each_book([&](std::uint16_t, const Book& book) {
        final_orders += book.order_count();
        final_levels += book.level_count(Side::Bid) + book.level_count(Side::Ask);
    });
    out << "--- totals ---\n"
        << "  messages            " << stats.messages_total << "\n"
        << "  bytes               " << stats.bytes_consumed << "\n"
        << "  events applied      " << stats.events_applied << "\n"
        << "  books created       " << stats.books_created << "\n"
        << "  directory entries   "
        << stats.count_by_type[static_cast<unsigned char>('R')] << "\n"
        << "  crossed episodes    " << stats.crossed_episodes << "\n"
        << "  crossed symbols     " << stats.crossed_symbols << "\n"
        << "  ts regressions      " << stats.timestamp_regressions << "\n"
        << "  live orders peak    " << stats.live_orders_peak << "\n"
        << "  live orders final   " << final_orders << "\n"
        << "  price levels final  " << final_levels << "\n"
        << "  invariant sweeps    " << stats.invariant_sweeps << "\n"
        << "  elapsed             " << stats.elapsed_seconds << " s\n";
    if (stats.elapsed_seconds > 0.0) {
        out << "  throughput          "
            << static_cast<std::uint64_t>(
                   static_cast<double>(stats.messages_total) / stats.elapsed_seconds)
            << " msgs/s, "
            << (static_cast<double>(stats.bytes_consumed) / (1024.0 * 1024.0)) /
                   stats.elapsed_seconds
            << " MiB/s\n";
    }
    out << "  peak RSS            " << (peak_rss_bytes() / (1024 * 1024)) << " MiB\n";
}

void print_book_error(const BookError& e, std::ostream& out) {
    const Event& ev = e.event();
    out << "book error: " << e.what() << "\n"
        << "  event: type=" << static_cast<int>(ev.type)
        << " timestamp=" << ev.timestamp << " order_id=" << ev.order_id
        << " side=" << (ev.side == Side::Bid ? "Bid" : "Ask")
        << " price=" << ev.price << " quantity=" << ev.quantity
        << " new_order_id=" << ev.new_order_id << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path file;
    bool have_file = false;
    ReplayOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--quiet") {
            opts.progress_every = 0;
        } else if (arg == "--sweep-every" && i + 1 < argc) {
            if (!parse_count(argv[++i], opts.invariant_sweep_every)) return usage();
        } else if (arg == "--progress" && i + 1 < argc) {
            if (!parse_count(argv[++i], opts.progress_every)) return usage();
        } else if (!arg.empty() && arg.front() == '-') {
            return usage();
        } else if (!have_file) {
            file = argv[i];
            have_file = true;
        } else {
            return usage();
        }
    }
    if (!have_file) return usage();

    std::optional<ItchReader> reader;
    try {
        reader.emplace(file);
    } catch (const FeedError& e) {
        // Unopenable file is a usage-level failure: exit 1, not 2 (spec §7).
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    BookRouter router;
    ReplayStats stats;
    int exit_code = 0;
    try {
        replay_stream(*reader, router, opts, stats, std::cerr);
    } catch (const FeedError& e) {
        std::cerr << "feed error: " << e.what() << "\n";
        exit_code = 2;
    } catch (const BookError& e) {
        print_book_error(e, std::cerr);
        exit_code = 3;
    } catch (const ReplayError& e) {
        std::cerr << "replay error: " << e.what() << "\n";
        exit_code = 4;
    }
    // Stats-so-far are the most useful debugging artifact: print them even
    // (especially) on an abort.
    print_stats(stats, router, exit_code == 0 ? std::cout : std::cerr);
    return exit_code;
}
