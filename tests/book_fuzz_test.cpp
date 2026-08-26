#include "baseline_book.h"
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

}  // namespace
