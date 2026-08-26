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

namespace {

using namespace xeas;

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

}  // namespace
