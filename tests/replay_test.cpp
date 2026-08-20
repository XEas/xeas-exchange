#include "xeas/replay.h"

#include "itch_fixture.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace xeas;
using xeas::test::ItchBuilder;

// Runs a builder tape through replay_stream. Default options: final sweep
// only, silent. Errors propagate to the caller; stats survive via out-param.
ReplayStats run(const ItchBuilder& b, BookRouter& router,
                ReplayOptions opts = ReplayOptions{0, 0},
                std::ostream* log = nullptr) {
    std::ostringstream sink;
    auto in = b.stream();
    ItchReader reader(in);
    ReplayStats stats;
    replay_stream(reader, router, opts, stats, log != nullptr ? *log : sink);
    return stats;
}

// ---------------------------------------------------------------------------
// Replay: driver semantics
// ---------------------------------------------------------------------------

TEST(Replay, MultiSymbolHappyTapeFinalState) {
    ItchBuilder b;
    b.stock_directory(1, 100, "AAPL");
    b.stock_directory(2, 100, "MSFT");
    b.other('S', 0, 100, "O");  // system event: counted, skipped
    b.add_order(1, 200, 10, 'B', 100, "AAPL", 1'500'000);
    b.add_order(1, 210, 11, 'S', 80, "AAPL", 1'501'000);
    b.add_order(2, 220, 20, 'B', 50, "MSFT", 3'000'000);
    b.order_executed(1, 230, 11, 30, 9001);           // ask 11 -> 50 left
    b.order_cancel(1, 240, 10, 40);                   // bid 10 -> 60 left
    b.order_replace(2, 250, 20, 21, 70, 3'010'000);   // MSFT bid moves
    b.order_delete(1, 260, 11);                       // AAPL ask gone
    b.add_order_mpid(2, 270, 22, 'S', 10, "MSFT", 3'020'000, "NSDQ");

    BookRouter router;
    const ReplayStats stats = run(b, router);

    EXPECT_EQ(stats.messages_total, 11u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('R')], 2u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('S')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('A')], 3u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('F')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('E')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('X')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('U')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('D')], 1u);
    EXPECT_EQ(stats.events_applied, 8u);
    EXPECT_EQ(stats.books_created, 2u);
    EXPECT_EQ(stats.bytes_consumed, b.bytes().size());
    EXPECT_EQ(stats.invariant_sweeps, 1u);  // final sweep only
    EXPECT_EQ(stats.timestamp_regressions, 0u);
    EXPECT_EQ(stats.crossed_episodes, 0u);

    const Book* aapl = router.find(1);
    ASSERT_NE(aapl, nullptr);
    ASSERT_TRUE(aapl->best_bid().has_value());
    EXPECT_EQ(aapl->best_bid()->price, 1'500'000);
    EXPECT_EQ(aapl->best_bid()->total_shares, 60);
    EXPECT_FALSE(aapl->best_ask().has_value());
    EXPECT_EQ(aapl->order_count(), 1u);

    const Book* msft = router.find(2);
    ASSERT_NE(msft, nullptr);
    ASSERT_TRUE(msft->best_bid().has_value());
    EXPECT_EQ(msft->best_bid()->price, 3'010'000);
    EXPECT_EQ(msft->best_bid()->total_shares, 70);
    ASSERT_TRUE(msft->best_ask().has_value());
    EXPECT_EQ(msft->best_ask()->price, 3'020'000);
    EXPECT_EQ(msft->order_count(), 2u);

    EXPECT_EQ(router.symbol_or_locate(1), "AAPL");
    EXPECT_EQ(router.symbol_or_locate(2), "MSFT");
}

TEST(Replay, UnknownTypesCountedAndSkipped) {
    ItchBuilder b;
    b.other('P', 1, 100, std::string(33, 'x'));  // Trade (non-cross), real type
    b.other('z', 1, 110, "??");                  // unknown future type
    b.other('z', 1, 120, "??");
    BookRouter router;
    const ReplayStats stats = run(b, router);
    EXPECT_EQ(stats.messages_total, 3u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('P')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('z')], 2u);
    EXPECT_EQ(stats.events_applied, 0u);
    EXPECT_EQ(stats.books_created, 0u);
}

TEST(Replay, LiveOrdersPeakAndFinal) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 110, 2, 'B', 10, "AAPL", 1'000'100);
    b.add_order(1, 120, 3, 'S', 10, "AAPL", 1'000'700);
    b.order_delete(1, 130, 1);
    b.order_delete(1, 140, 3);
    BookRouter router;
    const ReplayStats stats = run(b, router);
    EXPECT_EQ(stats.live_orders_peak, 3u);
    EXPECT_EQ(stats.live_orders_final, 1u);
}

TEST(Replay, TimestampRegressionsCountedNotFatal) {
    ItchBuilder b;
    b.add_order(1, 200, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 150, 2, 'B', 10, "AAPL", 1'000'100);  // regression
    b.add_order(1, 150, 3, 'B', 10, "AAPL", 1'000'200);  // equal: NOT a regression
    b.add_order(1, 100, 4, 'B', 10, "AAPL", 1'000'300);  // regression
    BookRouter router;
    const ReplayStats stats = run(b, router);
    EXPECT_EQ(stats.timestamp_regressions, 2u);
    EXPECT_EQ(stats.events_applied, 4u);  // never fatal
}

TEST(Replay, SweepCadencePlusFinalSweep) {
    ItchBuilder b;
    for (std::uint64_t i = 0; i < 5; ++i) {
        b.add_order(1, 100 + i, 1 + i, 'B', 10, "AAPL",
                    static_cast<std::uint32_t>(1'000'000 - i * 100));
    }
    BookRouter router;
    const ReplayStats stats = run(b, router, ReplayOptions{2, 0});
    EXPECT_EQ(stats.invariant_sweeps, 3u);  // after msgs 2 and 4, plus final
}

TEST(Replay, ProgressLinesAndQuiet) {
    ItchBuilder b;
    for (std::uint64_t i = 0; i < 5; ++i) {
        b.add_order(1, 100 + i, 1 + i, 'B', 10, "AAPL", 1'000'000);
    }
    BookRouter router1;
    std::ostringstream log1;
    run(b, router1, ReplayOptions{0, 2}, &log1);
    std::size_t lines = 0;
    for (char c : log1.str()) lines += (c == '\n') ? 1 : 0;
    EXPECT_EQ(lines, 2u);  // after msgs 2 and 4
    EXPECT_NE(log1.str().find("progress"), std::string::npos);

    BookRouter router2;
    std::ostringstream log2;
    run(b, router2, ReplayOptions{0, 0}, &log2);
    EXPECT_TRUE(log2.str().empty());  // 0 = silent
}

// ---------------------------------------------------------------------------
// Replay: crossed classification and abort semantics
// ---------------------------------------------------------------------------

TEST(Replay, CrossedTapeCompletesAndCountsEpisodes) {
    ItchBuilder b;
    b.stock_directory(1, 50, "AAPL");
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 110, 2, 'S', 10, "AAPL", 1'005'000);  // normal spread
    b.add_order(1, 120, 3, 'S', 10, "AAPL", 995'000);    // crossed: episode 1
    b.order_delete(1, 130, 3);                           // uncrossed
    b.add_order(1, 140, 4, 'S', 10, "AAPL", 1'000'000);  // locked: episode 2
    BookRouter router;
    std::ostringstream log;
    // Sweep after every message: the crossed book is seen by sweeps too —
    // logged once, never fatal.
    const ReplayStats stats = run(b, router, ReplayOptions{1, 0}, &log);
    EXPECT_EQ(stats.crossed_episodes, 2u);
    EXPECT_EQ(stats.crossed_symbols, 1u);
    EXPECT_EQ(stats.events_applied, 5u);  // completed without throwing
    const std::string text = log.str();
    EXPECT_NE(text.find("AAPL"), std::string::npos);
    EXPECT_NE(text.find("crossed"), std::string::npos);
    EXPECT_EQ(text.find("crossed"), text.rfind("crossed"));  // logged exactly once
}

TEST(Replay, CrossedThenUncrossedFinalSweepStaysQuiet) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 110, 2, 'S', 10, "AAPL", 995'000);  // crossed
    b.order_delete(1, 120, 2);                         // uncrossed before EOF
    BookRouter router;
    std::ostringstream log;
    const ReplayStats stats = run(b, router, ReplayOptions{0, 0}, &log);
    EXPECT_EQ(stats.crossed_episodes, 1u);   // probe caught it mid-tape
    EXPECT_EQ(stats.crossed_symbols, 1u);
    EXPECT_TRUE(log.str().empty());          // final sweep saw a clean book
}

TEST(Replay, BookErrorPropagatesWithStatsIntact) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.order_cancel(1, 110, 999, 5);  // unknown order id -> BookError
    b.add_order(1, 120, 2, 'B', 10, "AAPL", 1'000'000);  // never reached
    BookRouter router;
    auto in = b.stream();
    ItchReader reader(in);
    ReplayStats stats;
    std::ostringstream log;
    const ReplayOptions opts{0, 0};
    EXPECT_THROW(replay_stream(reader, router, opts, stats, log), BookError);
    EXPECT_EQ(stats.messages_total, 2u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('A')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('X')], 1u);
    EXPECT_EQ(stats.events_applied, 1u);
    EXPECT_EQ(stats.books_created, 1u);
    EXPECT_EQ(stats.live_orders_final, 1u);
    EXPECT_GT(stats.bytes_consumed, 0u);
}

TEST(Replay, TruncatedStreamMidTapeThrowsFeedError) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 110, 2, 'B', 10, "AAPL", 1'000'100);
    b.truncate_last(4);
    BookRouter router;
    auto in = b.stream();
    ItchReader reader(in);
    ReplayStats stats;
    std::ostringstream log;
    const ReplayOptions opts{0, 0};
    EXPECT_THROW(replay_stream(reader, router, opts, stats, log), FeedError);
    EXPECT_EQ(stats.messages_total, 1u);  // first message processed fine
    EXPECT_EQ(stats.events_applied, 1u);
}

TEST(Replay, DecodeErrorEnrichedWithStreamPosition) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);   // frame 0: 38 bytes
    b.add_order(1, 110, 2, 'Q', 10, "AAPL", 1'000'100);   // bad side byte
    BookRouter router;
    auto in = b.stream();
    ItchReader reader(in);
    ReplayStats stats;
    std::ostringstream log;
    const ReplayOptions opts{0, 0};
    try {
        replay_stream(reader, router, opts, stats, log);
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.byte_offset(), 38u);   // start of the offending frame
        EXPECT_EQ(e.message_index(), 1u);
        EXPECT_EQ(e.message_type(), 'A');
        EXPECT_NE(e.reason().find("side"), std::string::npos);
    }
}

TEST(Replay, ReplayErrorCarriesContext) {
    // A structural violation cannot be produced through well-formed replay
    // (that is the point); checker correctness is Milestone 1's job. Verify
    // the exception type's contract directly.
    const ReplayError e(42, "AAPL",
                        {"bid level 100 is empty", "order index size 2 != orders present in levels 1"},
                        1234);
    EXPECT_EQ(e.locate(), 42);
    EXPECT_EQ(e.symbol(), "AAPL");
    EXPECT_EQ(e.violations().size(), 2u);
    EXPECT_EQ(e.message_index(), 1234u);
    const std::string what = e.what();
    EXPECT_NE(what.find("AAPL"), std::string::npos);
    EXPECT_NE(what.find("42"), std::string::npos);
    EXPECT_NE(what.find("1234"), std::string::npos);
    EXPECT_NE(what.find("bid level 100 is empty"), std::string::npos);
    EXPECT_NE(what.find("order index size"), std::string::npos);
}

}  // namespace
