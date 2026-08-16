#include "xeas/book.h"
#include "xeas/event.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace {

using namespace xeas;

// ---------------------------------------------------------------------------
// Task 1: skeleton sanity
// ---------------------------------------------------------------------------

TEST(Skeleton, EventFieldsRoundTrip) {
    Event e;
    e.type = EventType::Add;
    e.timestamp = 34'200'000'000'000ULL;  // 09:30 in ns since midnight; carried, never interpreted
    e.order_id = 42;
    e.side = Side::Bid;
    e.price = 1'234'500;  // ticks (ITCH fixed point x10,000)
    e.quantity = 100;
    e.new_order_id = 0;

    EXPECT_EQ(e.type, EventType::Add);
    EXPECT_EQ(e.timestamp, 34'200'000'000'000ULL);
    EXPECT_EQ(e.order_id, 42u);
    EXPECT_EQ(e.side, Side::Bid);
    EXPECT_EQ(e.price, 1'234'500);
    EXPECT_EQ(e.quantity, 100u);
    EXPECT_EQ(e.new_order_id, 0u);
}

TEST(Skeleton, TypeAliasesAreFixedWidth) {
    static_assert(std::is_same_v<Price, std::int64_t>);
    static_assert(std::is_same_v<Qty, std::uint32_t>);
    static_assert(std::is_same_v<OrderId, std::uint64_t>);
    static_assert(std::is_same_v<Timestamp, std::uint64_t>);
    SUCCEED();
}

TEST(Skeleton, BookDefaultConstructs) {
    Book book;
    (void)book;
    SUCCEED();
}

}  // namespace
