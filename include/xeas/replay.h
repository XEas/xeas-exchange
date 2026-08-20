#pragma once

#include "xeas/book.h"
#include "xeas/itch.h"
#include "xeas/router.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xeas {

// Thrown when an invariant sweep finds a structural violation (level sums,
// empty levels, index mismatches, ...) — a bug somewhere, never data reality.
// Crossed/locked findings are classified as reportable and never reach here.
class ReplayError : public std::runtime_error {
public:
    ReplayError(std::uint16_t locate, std::string symbol,
                std::vector<std::string> violations, std::uint64_t message_index);

    std::uint16_t locate() const noexcept { return locate_; }
    const std::string& symbol() const noexcept { return symbol_; }
    const std::vector<std::string>& violations() const noexcept { return violations_; }
    std::uint64_t message_index() const noexcept { return message_index_; }

private:
    std::uint16_t locate_;
    std::string symbol_;
    std::vector<std::string> violations_;
    std::uint64_t message_index_;
};

struct ReplayOptions {
    std::uint64_t invariant_sweep_every = 25'000'000;  // messages between sweeps; 0 = final only
    std::uint64_t progress_every = 10'000'000;         // messages between lines; 0 = silent
};

struct ReplayStats {
    std::uint64_t messages_total = 0;
    std::array<std::uint64_t, 256> count_by_type{};  // indexed by type byte
    std::uint64_t events_applied = 0;
    std::uint64_t bytes_consumed = 0;
    std::uint64_t books_created = 0;
    std::uint64_t crossed_episodes = 0;    // false->true transitions
    std::uint64_t crossed_symbols = 0;     // distinct books ever crossed
    std::uint64_t timestamp_regressions = 0;
    std::uint64_t live_orders_peak = 0;
    std::uint64_t live_orders_final = 0;
    std::uint64_t invariant_sweeps = 0;
    double elapsed_seconds = 0.0;
};

// Replays every frame from the reader through per-locate books. stats is an
// out-parameter (not a return value) so that on an abort the caller still has
// everything counted up to the failure point — the single most useful
// debugging artifact. FeedError / BookError / ReplayError all propagate;
// byte/book/elapsed counters are stamped even then.
void replay_stream(ItchReader& reader, BookRouter& router,
                   const ReplayOptions& opts, ReplayStats& stats,
                   std::ostream& log);

}  // namespace xeas
