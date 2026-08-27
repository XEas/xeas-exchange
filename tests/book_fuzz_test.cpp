#include "baseline_book.h"
#include "tape_gen.h"
#include "xeas/book.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xeas {

// Test-only backdoor (friend of Book) for the fuzz binary. A sibling of the
// peer in book_test.cpp — the two live in different binaries, so no ODR issue.
struct BookTestPeer {
    static std::uint32_t node_index(const Book& b, OrderId id) {
        return b.orders_.at(id).node;
    }
    static std::size_t order_pool_capacity(const Book& b) { return b.order_pool_.capacity(); }
    static std::size_t level_pool_capacity(const Book& b) { return b.level_pool_.capacity(); }
    static std::vector<OrderId> level_order_ids(const Book& b, Side side, Price price) {
        std::vector<OrderId> ids;
        const std::uint32_t idx = b.level_index(side, price);
        if (idx == Book::kNone) return ids;
        for (std::uint32_t cur = b.level_pool_[idx].head; cur != Book::kNone;
             cur = b.order_pool_[cur].next) {
            ids.push_back(b.order_pool_[cur].id);
        }
        return ids;
    }

    // --- corruption helpers ---
    static void set_order_free_head(Book& b, OrderId id) {   // node free AND in a level
        b.order_pool_.free_head_ = b.orders_.at(id).node;
    }
    static void set_level_free_head(Book& b, Side side, Price price) {
        b.level_pool_.free_head_ = b.level_index(side, price);
    }
    static void break_prev_link(Book& b, OrderId id) {   // corrupt a NON-head node
        b.order_pool_[b.orders_.at(id).node].prev = Book::kNone;
    }
    static void corrupt_handle_level(Book& b, OrderId id, Side side, Price price) {
        b.orders_.at(id).level = b.level_index(side, price);
    }

    // --- band accessors (phase 3) ---
    static Price band_lo(const Book& b, Side side) {
        return side == Side::Bid ? b.bids_.lo : b.asks_.lo;
    }
    static std::size_t band_ticks(const Book& b, Side side) {
        return side == Side::Bid ? b.bids_.slots.size() : b.asks_.slots.size();
    }
    static bool in_overflow(const Book& b, Side side, Price price) {
        return side == Side::Bid ? b.bids_.overflow.count(price) != 0
                                 : b.asks_.overflow.count(price) != 0;
    }
    static std::vector<Price> ordered_prices(const Book& b, Side side) {
        std::vector<Price> prices;
        b.for_each_level(side, [&](std::uint32_t, const Book::Level& level) {
            prices.push_back(level.price);
        });
        return prices;
    }

    // --- phase-3 corruption helpers ---
    template <class FlatSideT>
    static void flip_tick_bit_in(FlatSideT& s, Price price) {
        const auto off = static_cast<std::size_t>(price - s.lo);
        s.words[off >> 6] ^= (1ULL << (off & 63));
    }
    static void flip_tick_bit(Book& b, Side side, Price price) {
        if (side == Side::Bid) flip_tick_bit_in(b.bids_, price);
        else flip_tick_bit_in(b.asks_, price);
    }
    static void clear_best_cache(Book& b, Side side) {
        (side == Side::Bid ? b.bids_.best : b.asks_.best) = Book::kNone;
    }
    static void inject_overflow_in_band(Book& b, Side side, Price price) {
        if (side == Side::Bid) b.bids_.overflow.emplace(price, 0u);
        else b.asks_.overflow.emplace(price, 0u);
    }
};

}  // namespace xeas

namespace {

using namespace xeas;
using xeas::BookTestPeer;

// --- event factories (mirrors tests/book_test.cpp) --------------------------

Event make_add(OrderId id, Side side, Price price, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Add;
    e.timestamp = ts;
    e.order_id = id;
    e.side = side;
    e.price = price;
    e.quantity = qty;
    return e;
}

Event make_cancel(OrderId id, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Cancel;
    e.timestamp = ts;
    e.order_id = id;
    e.quantity = qty;
    return e;
}

Event make_delete(OrderId id, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Delete;
    e.timestamp = ts;
    e.order_id = id;
    return e;
}

bool any_contains(const std::vector<std::string>& violations, std::string_view needle) {
    for (const auto& v : violations) {
        if (v.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// BaselineSmoke: the frozen oracle behaves like the Milestone 1/2 book
// ---------------------------------------------------------------------------

TEST(BaselineSmoke, AddQueryDeleteLifecycle) {
    BaselineBook book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Ask, 1'010'000, 50));
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(book.best_bid()->price, 1'000'000);
    EXPECT_EQ(book.best_ask()->price, 1'010'000);
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 100);
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(book.level_count(Side::Ask), 1u);
    ASSERT_TRUE(book.find_order(1).has_value());
    EXPECT_EQ(book.find_order(1)->remaining_qty, 100u);
    EXPECT_TRUE(check_invariants(book).empty());
    apply(book, make_delete(1));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BaselineSmoke, ThrowsBookErrorOnMalformedEvents) {
    BaselineBook book;
    EXPECT_THROW(apply(book, make_cancel(999, 10)), BookError);
    EXPECT_THROW(apply(book, make_add(1, Side::Bid, 0, 100)), BookError);
}

TEST(BaselineSmoke, CrossedBookStoredAsGivenAndFlagged) {
    BaselineBook book;
    apply(book, make_add(1, Side::Bid, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));
    EXPECT_TRUE(is_crossed(book));
    EXPECT_TRUE(any_contains(check_invariants(book), "crossed"));
}

// ---------------------------------------------------------------------------
// TapeGen: determinism + validity of the generator itself
// ---------------------------------------------------------------------------

TEST(TapeGen, SameSeedProducesIdenticalTapes) {
    TapeGenOptions opts;
    opts.seed = 42;
    TapeGen a(opts);
    TapeGen b(opts);
    for (int i = 0; i < 10'000; ++i) {
        const Event ea = a.next();
        const Event eb = b.next();
        ASSERT_EQ(ea.type, eb.type) << "event " << i;
        ASSERT_EQ(ea.order_id, eb.order_id) << "event " << i;
        ASSERT_EQ(ea.side, eb.side) << "event " << i;
        ASSERT_EQ(ea.price, eb.price) << "event " << i;
        ASSERT_EQ(ea.quantity, eb.quantity) << "event " << i;
        ASSERT_EQ(ea.new_order_id, eb.new_order_id) << "event " << i;
    }
}

TEST(TapeGen, ValidByConstructionAgainstBaseline) {
    TapeGenOptions opts;
    opts.seed = 1;
    TapeGen gen(opts);
    BaselineBook book;
    for (int i = 0; i < 50'000; ++i) {
        ASSERT_NO_THROW(apply(book, gen.next())) << "event " << i;
    }
    EXPECT_EQ(book.order_count(), gen.live_order_ids().size());
}

TEST(TapeGen, FallsBackToAddWhenNothingIsLive) {
    TapeGenOptions opts;
    opts.seed = 3;
    opts.add = 0;
    opts.del = 1'000;
    opts.execute = opts.cancel = opts.replace = 0;
    TapeGen gen(opts);
    BaselineBook book;
    const Event first = gen.next();
    EXPECT_EQ(first.type, EventType::Add);  // nothing live: must fall back
    apply(book, first);
    for (int i = 0; i < 200; ++i) {
        apply(book, gen.next());
        ASSERT_LE(book.order_count(), 1u);  // add/delete strictly alternate
    }
}

TEST(TapeGen, FarPricesAppearAtConfiguredRate) {
    TapeGenOptions opts;
    opts.seed = 5;
    opts.far_price_ppm = 100'000;  // 10%: outliers must show up fast
    TapeGen gen(opts);
    int far_seen = 0;
    for (int i = 0; i < 2'000; ++i) {
        const Event e = gen.next();
        if ((e.type == EventType::Add || e.type == EventType::Replace) &&
            (e.price >= opts.mid + 400'000 || e.price <= opts.mid - 400'000)) {
            ++far_seen;
        }
    }
    EXPECT_GT(far_seen, 0);
}

// ---------------------------------------------------------------------------
// Differential protocol (spec §6): apply each event to both books; compare
// queries every K events and exhaustively at the end; require the new book's
// invariants to be empty or crossed-only.
// ---------------------------------------------------------------------------

template <class B>
std::optional<BBO> best_of(const B& book, Side side) {
    return side == Side::Bid ? book.best_bid() : book.best_ask();
}

template <class NewBook, class OracleBook>
testing::AssertionResult books_agree(const NewBook& book, const OracleBook& oracle,
                                     const std::vector<Price>& prices,
                                     const std::vector<OrderId>& ids) {
    for (const Side side : {Side::Bid, Side::Ask}) {
        const char* name = side == Side::Bid ? "bid" : "ask";
        const auto a = best_of(book, side);
        const auto b = best_of(oracle, side);
        if (a.has_value() != b.has_value()) {
            return testing::AssertionFailure() << "best " << name << " presence differs";
        }
        if (a.has_value() && (a->price != b->price || a->total_shares != b->total_shares)) {
            return testing::AssertionFailure()
                   << "best " << name << " differs: " << a->price << "/" << a->total_shares
                   << " vs " << b->price << "/" << b->total_shares;
        }
        if (book.level_count(side) != oracle.level_count(side)) {
            return testing::AssertionFailure()
                   << name << " level_count differs: " << book.level_count(side) << " vs "
                   << oracle.level_count(side);
        }
        for (const Price price : prices) {
            if (book.size_at(side, price) != oracle.size_at(side, price)) {
                return testing::AssertionFailure()
                       << name << " size_at " << price << " differs: "
                       << book.size_at(side, price) << " vs " << oracle.size_at(side, price);
            }
        }
    }
    if (book.order_count() != oracle.order_count()) {
        return testing::AssertionFailure() << "order_count differs: " << book.order_count()
                                           << " vs " << oracle.order_count();
    }
    for (const OrderId id : ids) {
        const auto a = book.find_order(id);
        const auto b = oracle.find_order(id);
        if (a.has_value() != b.has_value()) {
            return testing::AssertionFailure() << "find_order presence differs for id " << id;
        }
        if (a.has_value() && (a->side != b->side || a->price != b->price ||
                              a->remaining_qty != b->remaining_qty)) {
            return testing::AssertionFailure() << "find_order differs for id " << id;
        }
    }
    return testing::AssertionSuccess();
}

testing::AssertionResult crossed_only(const std::vector<std::string>& violations) {
    for (const auto& v : violations) {
        if (v.find("crossed") == std::string::npos) {
            return testing::AssertionFailure() << "non-crossed invariant violation: " << v;
        }
    }
    return testing::AssertionSuccess();
}

template <class NewBook, class OracleBook>
void run_differential(const TapeGenOptions& opts, std::size_t events, std::size_t check_every) {
    NewBook book;
    OracleBook oracle;
    TapeGen gen(opts);
    std::vector<Price> touched;       // every Add/Replace price, for the final sweep
    std::array<Price, 16> recent{};   // ring of recently touched prices
    std::size_t recent_n = 0;
    for (std::size_t i = 0; i < events; ++i) {
        const Event e = gen.next();
        apply(book, e);
        apply(oracle, e);
        if (e.type == EventType::Add || e.type == EventType::Replace) {
            touched.push_back(e.price);
            recent[recent_n++ % recent.size()] = e.price;
        }
        if ((i + 1) % check_every == 0) {
            const std::vector<Price> prices(
                recent.begin(),
                recent.begin() + static_cast<std::ptrdiff_t>(std::min(recent_n, recent.size())));
            std::vector<OrderId> sample;
            const auto& live = gen.live_order_ids();
            for (std::size_t k = 0; k < live.size(); k += 97) sample.push_back(live[k]);
            ASSERT_TRUE(books_agree(book, oracle, prices, sample)) << "after event " << i + 1;
            ASSERT_TRUE(crossed_only(check_invariants(book))) << "after event " << i + 1;
        }
    }
    // Final sweeps: every live order, every touched price.
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    ASSERT_TRUE(books_agree(book, oracle, touched, gen.live_order_ids())) << "final sweep";
    ASSERT_TRUE(crossed_only(check_invariants(book))) << "final sweep";
}

// ---------------------------------------------------------------------------
// FuzzDifferential: Book vs the frozen BaselineBook oracle
// ---------------------------------------------------------------------------

TEST(FuzzDifferential, DefaultMixAgainstBaseline) {
    TapeGenOptions opts;
    opts.seed = 1;
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, ReplaceHeavyMix) {
    TapeGenOptions opts;
    opts.seed = 2;
    opts.add = 300; opts.del = 100; opts.execute = 100; opts.cancel = 100; opts.replace = 400;
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, FarPriceHeavyMix) {
    TapeGenOptions opts;
    opts.seed = 3;
    opts.far_price_ppm = 20'000;   // 2% far-out prices: overflow and overflow-is-best
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, HighChurnDrainsToEmpty) {
    TapeGenOptions opts;
    opts.seed = 4;
    opts.add = 250; opts.del = 450; opts.execute = 200; opts.cancel = 50; opts.replace = 50;
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, CrossedHeavyMix) {
    TapeGenOptions opts;
    opts.seed = 5;
    opts.offset_max = 2;   // both sides hug the mid: constant locking/crossing
    opts.walk_step = 5;
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, MultiSeedSweep) {
    for (std::uint64_t seed = 2; seed <= 11; ++seed) {
        TapeGenOptions opts;
        opts.seed = seed;
        run_differential<Book, BaselineBook>(opts, 20'000, 4'096);
        if (::testing::Test::HasFatalFailure()) FAIL() << "seed " << seed;
    }
}

TEST(FuzzDifferential, BaselineAgreesWithItself) {
    // Harness self-validation: the oracle differentially compared to itself.
    TapeGenOptions opts;
    opts.seed = 1;
    run_differential<BaselineBook, BaselineBook>(opts, 50'000, 4'096);
}

// ---------------------------------------------------------------------------
// BenchSmoke: each bench workload runs untimed and leaves a consistent book
// ---------------------------------------------------------------------------

TEST(BenchSmoke, WorkloadsPassInvariantsUntimed) {
    for (const Workload w : {Workload::Steady, Workload::Insert, Workload::Deep}) {
        const WorkloadTape tape = make_workload_tape(w, 1, 1'000, 10'000);
        Book book;
        for (const Event& e : tape.warm) apply(book, e);
        for (const Event& e : tape.timed) apply(book, e);
        EXPECT_TRUE(crossed_only(check_invariants(book)))
            << "workload " << static_cast<int>(w);
        EXPECT_GE(book.order_count() + tape.timed.size(), 10'000u);
    }
}

TEST(BenchSmoke, InsertWorkloadDrainsTheBook) {
    const WorkloadTape tape = make_workload_tape(Workload::Insert, 1, 0, 10'000);
    Book book;
    for (const Event& e : tape.timed) apply(book, e);
    EXPECT_EQ(book.order_count(), 0u);   // adds then deletes: exactly drains
}

// ---------------------------------------------------------------------------
// Pool: slab allocate/release/reuse, index stability, freelist honesty
// ---------------------------------------------------------------------------

TEST(Pool, ReleaseThenReallocateReusesTheSlot) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    const std::uint32_t first_slot = BookTestPeer::node_index(book, 1);
    apply(book, make_delete(1));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    EXPECT_EQ(BookTestPeer::node_index(book, 2), first_slot);
    EXPECT_EQ(BookTestPeer::order_pool_capacity(book), 1u);
}

TEST(Pool, CapacityStableUnderChurn) {
    Book book;
    for (int round = 0; round < 3; ++round) {
        for (OrderId id = 1; id <= 100; ++id) {
            apply(book, make_add(id + static_cast<OrderId>(round) * 1'000, Side::Bid,
                                 1'000'000 - static_cast<Price>(id % 10) * 100, 10));
        }
        for (OrderId id = 1; id <= 100; ++id) {
            apply(book, make_delete(id + static_cast<OrderId>(round) * 1'000));
        }
    }
    EXPECT_EQ(BookTestPeer::order_pool_capacity(book), 100u);   // slots recycled
    EXPECT_EQ(BookTestPeer::level_pool_capacity(book), 10u);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(Pool, HandlesStableAcrossSlabGrowth) {
    Book book;
    for (OrderId id = 1; id <= 5'000; ++id) {   // many vector reallocations
        apply(book, make_add(id, Side::Bid, 1'000'000 - static_cast<Price>(id % 50), 10));
    }
    for (const OrderId id : {OrderId{1}, OrderId{2'500}, OrderId{5'000}}) {
        const auto info = book.find_order(id);
        ASSERT_TRUE(info.has_value()) << id;
        EXPECT_EQ(info->price, 1'000'000 - static_cast<Price>(id % 50));
        EXPECT_EQ(info->remaining_qty, 10u);
    }
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(Pool, FreelistCorruptionIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 10));
    apply(book, make_add(2, Side::Ask, 1'010'000, 20));
    BookTestPeer::set_order_free_head(book, 2);
    EXPECT_TRUE(any_contains(check_invariants(book), "freelist"));
}

TEST(Pool, FifoSurvivesHeavyChurn) {
    Book book;
    for (OrderId id = 1; id <= 50; ++id) {
        apply(book, make_add(id, Side::Ask, 1'010'000, 10));
    }
    for (OrderId id = 1; id <= 50; id += 2) {
        apply(book, make_delete(id));
    }
    std::vector<OrderId> expected;
    for (OrderId id = 2; id <= 50; id += 2) expected.push_back(id);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000), expected);
    apply(book, make_add(51, Side::Ask, 1'010'000, 10));
    expected.push_back(51);   // new order enqueues at the back
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000), expected);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(Pool, LevelPoolReusesReleasedLevels) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_delete(1));                          // level released
    apply(book, make_add(2, Side::Bid, 990'000, 50));     // different price, same slot
    EXPECT_EQ(BookTestPeer::level_pool_capacity(book), 1u);
    EXPECT_EQ(book.best_bid()->price, 990'000);
    EXPECT_TRUE(check_invariants(book).empty());
}

// ---------------------------------------------------------------------------
// InvariantsV2 (phase-2 subset): one corruption per new structural invariant
// ---------------------------------------------------------------------------

TEST(InvariantsV2, BrokenPrevNextReciprocityIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));   // id 2 is NOT the head
    BookTestPeer::break_prev_link(book, 2);
    EXPECT_TRUE(any_contains(check_invariants(book), "reciprocal"));
}

TEST(InvariantsV2, LevelOnFreelistWhileLiveIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::set_level_free_head(book, Side::Bid, 1'000'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "freelist"));
}

TEST(InvariantsV2, WrongLevelHandleIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'020'000, 50));
    BookTestPeer::corrupt_handle_level(book, 1, Side::Ask, 1'020'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "disagrees"));
}

// ---------------------------------------------------------------------------
// BandGrowth: anchor, geometric growth, overflow, merged best-first order
// ---------------------------------------------------------------------------

TEST(BandGrowth, FirstAddAnchorsTheBand) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 100));
    const std::size_t ticks = BookTestPeer::band_ticks(book, Side::Ask);
    EXPECT_GT(ticks, 0u);
    EXPECT_EQ(BookTestPeer::band_lo(book, Side::Ask),
              1'000'000 - static_cast<Price>(ticks / 2));   // centered on the first add
    EXPECT_FALSE(BookTestPeer::in_overflow(book, Side::Ask, 1'000'000));
    EXPECT_EQ(book.best_ask()->price, 1'000'000);
    EXPECT_EQ(BookTestPeer::band_ticks(book, Side::Bid), 0u);   // lazy per side
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, OutOfBandAddGrowsTheBandAndPreservesQueries) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 100));
    const std::size_t before = BookTestPeer::band_ticks(book, Side::Ask);
    apply(book, make_add(2, Side::Ask, 1'003'000, 50));   // past the initial hi edge
    EXPECT_GT(BookTestPeer::band_ticks(book, Side::Ask), before);
    EXPECT_FALSE(BookTestPeer::in_overflow(book, Side::Ask, 1'003'000));
    EXPECT_EQ(book.best_ask()->price, 1'000'000);
    EXPECT_EQ(book.size_at(Side::Ask, 1'000'000), 100);
    EXPECT_EQ(book.size_at(Side::Ask, 1'003'000), 50);
    EXPECT_EQ(book.level_count(Side::Ask), 2u);
    EXPECT_EQ(book.find_order(1)->price, 1'000'000);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, FarPriceLandsInOverflowAndCanBeBest) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 100));
    apply(book, make_add(2, Side::Ask, 100, 25));   // ~1M ticks away: overflow
    EXPECT_TRUE(BookTestPeer::in_overflow(book, Side::Ask, 100));
    EXPECT_EQ(book.size_at(Side::Ask, 100), 25);
    EXPECT_EQ(book.best_ask()->price, 100);         // the lone $0.01 ask is the BBO
    EXPECT_EQ(book.level_count(Side::Ask), 2u);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, LoneOverflowOrderIsTheBbo) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 5'000'000, 25));   // far above: overflow bid
    EXPECT_EQ(book.best_bid()->price, 5'000'000);
    apply(book, make_delete(2));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);
    apply(book, make_delete(1));
    apply(book, make_add(3, Side::Bid, 5'000'000, 10));   // band empty, overflow only
    EXPECT_EQ(book.best_bid()->price, 5'000'000);
    EXPECT_EQ(book.best_bid()->total_shares, 10);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, BandAndOverflowInterleaveInSortedOrder) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 10));   // anchors the band
    apply(book, make_add(2, Side::Ask, 1'000'100, 10));   // in band
    apply(book, make_add(3, Side::Ask, 100, 10));         // overflow, below band
    apply(book, make_add(4, Side::Ask, 5'000'000, 10));   // overflow, above band
    EXPECT_EQ(BookTestPeer::ordered_prices(book, Side::Ask),
              (std::vector<Price>{100, 1'000'000, 1'000'100, 5'000'000}));
    apply(book, make_add(5, Side::Bid, 900'000, 10));
    apply(book, make_add(6, Side::Bid, 400'000, 10));     // out-of-band low bid: grows or overflows
    const auto bid_prices = BookTestPeer::ordered_prices(book, Side::Bid);
    EXPECT_EQ(bid_prices, (std::vector<Price>{900'000, 400'000}));   // descending
    EXPECT_TRUE(crossed_only(check_invariants(book)));  // Note: brief says .empty() but book is crossed
}

TEST(BandGrowth, NextBestScanCrossesWordAndSummaryBoundaries) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 10));
    apply(book, make_add(2, Side::Ask, 1'000'100, 10));   // different bitmap word
    apply(book, make_delete(1));
    EXPECT_EQ(book.best_ask()->price, 1'000'100);          // scan crossed a word
    apply(book, make_add(3, Side::Ask, 1'200'000, 10));    // grows the band wide
    apply(book, make_delete(2));
    EXPECT_EQ(book.best_ask()->price, 1'200'000);          // scan crossed summary words
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, GrowthWhileCrossedKeepsBothSidesRight) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));   // crossed, stored as given
    apply(book, make_add(3, Side::Ask, 1'003'500, 50));    // out-of-band: grow while crossed
    EXPECT_TRUE(is_crossed(book));
    EXPECT_EQ(book.best_ask()->price, 1'000'000);
    EXPECT_EQ(book.best_bid()->price, 1'010'000);
    EXPECT_EQ(book.size_at(Side::Ask, 1'003'500), 50);
    EXPECT_TRUE(crossed_only(check_invariants(book)));
}

TEST(InvariantsV2, BitmapSlotDisagreementIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::flip_tick_bit(book, Side::Bid, 1'000'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "bitmap disagrees"));
}

TEST(InvariantsV2, StaleCachedBestIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    BookTestPeer::clear_best_cache(book, Side::Ask);
    EXPECT_TRUE(any_contains(check_invariants(book), "cached best"));
}

TEST(InvariantsV2, InBandPriceInOverflowIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::inject_overflow_in_band(book, Side::Bid, 1'000'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "overflow contains in-band price"));
}

}  // namespace
