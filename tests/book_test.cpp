#include "xeas/book.h"
#include "xeas/event.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

#include <string>
#include <string_view>
#include <vector>

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

// ---------------------------------------------------------------------------
// Test helpers: event factories
// ---------------------------------------------------------------------------

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

}  // namespace

namespace xeas {

// Test-only backdoor (friend of Book) used to observe FIFO queue order and to
// corrupt internal state so the invariant checker can be genuinely tested.
// Never used by production code.
struct BookTestPeer {
    static std::vector<OrderId> level_order_ids(const Book& b, Side side, Price price) {
        std::vector<OrderId> ids;
        const Book::Level* level = nullptr;
        if (side == Side::Bid) {
            const auto it = b.bids_.find(price);
            if (it != b.bids_.end()) level = &it->second;
        } else {
            const auto it = b.asks_.find(price);
            if (it != b.asks_.end()) level = &it->second;
        }
        if (level != nullptr) {
            for (const auto& order : level->orders) ids.push_back(order.id);
        }
        return ids;
    }

    // --- corruption helpers (invariant-checker tests only) ---
    static void corrupt_total_shares(Book& b, Side side, Price price, std::int64_t v) {
        if (side == Side::Bid) {
            b.bids_.at(price).total_shares = v;
        } else {
            b.asks_.at(price).total_shares = v;
        }
    }

    static void add_empty_level(Book& b, Side side, Price price) {
        if (side == Side::Bid) {
            b.bids_[price];
        } else {
            b.asks_[price];
        }
    }

    static void zero_order_qty(Book& b, OrderId id) {
        b.orders_.at(id).it->remaining = 0;
    }

    static void drop_from_index(Book& b, OrderId id) {
        b.orders_.erase(id);
    }

    static void corrupt_handle_price(Book& b, OrderId id, Price wrong_price) {
        b.orders_.at(id).price = wrong_price;
    }
};

}  // namespace xeas

namespace {

using xeas::BookTestPeer;

// ---------------------------------------------------------------------------
// Task 2: Add + queries
// ---------------------------------------------------------------------------

TEST(AddAndQueries, EmptyBookQueries) {
    Book book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 0);
    EXPECT_EQ(book.size_at(Side::Ask, 1'000'000), 0);
    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.level_count(Side::Bid), 0u);
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
}

TEST(AddAndQueries, SingleBidAdd) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));

    const auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->price, 1'000'000);
    EXPECT_EQ(bb->total_shares, 100);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 100);
    EXPECT_EQ(book.size_at(Side::Bid, 999'999), 0);

    const auto info = book.find_order(1);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->side, Side::Bid);
    EXPECT_EQ(info->price, 1'000'000);
    EXPECT_EQ(info->remaining_qty, 100u);

    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
}

TEST(AddAndQueries, AggregatesLevelsAndBestPriceOrdering) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));  // same level: aggregates
    apply(book, make_add(3, Side::Bid, 990'000, 25));    // worse bid (lower)
    apply(book, make_add(4, Side::Ask, 1'010'000, 75));
    apply(book, make_add(5, Side::Ask, 1'020'000, 10));  // worse ask (higher)

    const auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->price, 1'000'000);  // highest bid is best
    EXPECT_EQ(bb->total_shares, 150);

    const auto ba = book.best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(ba->price, 1'010'000);  // lowest ask is best
    EXPECT_EQ(ba->total_shares, 75);

    EXPECT_EQ(book.size_at(Side::Bid, 990'000), 25);
    EXPECT_EQ(book.size_at(Side::Ask, 1'020'000), 10);
    EXPECT_EQ(book.order_count(), 5u);
    EXPECT_EQ(book.level_count(Side::Bid), 2u);
    EXPECT_EQ(book.level_count(Side::Ask), 2u);
}

TEST(AddAndQueries, TimePriorityFifoEnqueueAtBack) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 10));
    apply(book, make_add(2, Side::Ask, 1'010'000, 20));
    apply(book, make_add(3, Side::Ask, 1'010'000, 30));
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{1, 2, 3}));
}

// ---------------------------------------------------------------------------
// Task 3: check_invariants
// ---------------------------------------------------------------------------

testing::AssertionResult book_consistent(const Book& book) {
    const auto violations = check_invariants(book);
    if (violations.empty()) {
        return testing::AssertionSuccess();
    }
    auto result = testing::AssertionFailure();
    for (const auto& v : violations) {
        result << v << "; ";
    }
    return result;
}

bool any_contains(const std::vector<std::string>& violations, std::string_view needle) {
    for (const auto& v : violations) {
        if (v.find(needle) != std::string::npos) return true;
    }
    return false;
}

TEST(Invariants, EmptyBookHasNoViolations) {
    Book book;
    EXPECT_TRUE(book_consistent(book));
}

TEST(Invariants, ConsistentMultiLevelBookHasNoViolations) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    apply(book, make_add(3, Side::Bid, 990'000, 25));
    apply(book, make_add(4, Side::Ask, 1'010'000, 75));
    apply(book, make_add(5, Side::Ask, 1'020'000, 10));
    EXPECT_TRUE(book_consistent(book));
}

TEST(Invariants, CrossedBookIsStoredAsGivenAndFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));  // crosses the bid
    // apply() does NOT match or reject: both orders rest in the book as given.
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(book.size_at(Side::Bid, 1'010'000), 100);
    EXPECT_EQ(book.size_at(Side::Ask, 1'000'000), 100);
    // Only check_invariants flags it.
    EXPECT_TRUE(any_contains(check_invariants(book), "crossed"));
}

TEST(Invariants, LockedBookIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));  // locked: bid == ask
    EXPECT_TRUE(any_contains(check_invariants(book), "crossed"));
}

TEST(Invariants, CorruptedCachedTotalSharesIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::corrupt_total_shares(book, Side::Bid, 1'000'000, 999);
    EXPECT_TRUE(any_contains(check_invariants(book), "total_shares"));
}

TEST(Invariants, EmptyLevelIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    BookTestPeer::add_empty_level(book, Side::Ask, 1'020'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "is empty"));
}

TEST(Invariants, ZeroQuantityOrderIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::zero_order_qty(book, 1);
    EXPECT_TRUE(any_contains(check_invariants(book), "zero-quantity"));
}

TEST(Invariants, IndexLevelDisagreementIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    BookTestPeer::drop_from_index(book, 2);
    const auto violations = check_invariants(book);
    EXPECT_TRUE(any_contains(violations, "missing from index"));
    EXPECT_TRUE(any_contains(violations, "index size"));
}

TEST(Invariants, IndexHandleDisagreementIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::corrupt_handle_price(book, 1, 999'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "disagrees"));
}

// ---------------------------------------------------------------------------
// Task 4: Cancel + Execute
// ---------------------------------------------------------------------------

Event make_cancel(OrderId id, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Cancel;
    e.timestamp = ts;
    e.order_id = id;
    e.quantity = qty;
    return e;
}

Event make_execute(OrderId id, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Execute;
    e.timestamp = ts;
    e.order_id = id;
    e.quantity = qty;
    return e;
}

TEST(CancelExecute, PartialCancelReducesOrderAndLevel) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_cancel(1, 30));

    const auto info = book.find_order(1);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->remaining_qty, 70u);
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 70);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(CancelExecute, FullCancelRemovesOrderKeepsLevelWithOthers) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    apply(book, make_cancel(1, 100));  // reaches zero: removed immediately

    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 50);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Bid, 1'000'000),
              (std::vector<OrderId>{2}));
    EXPECT_TRUE(book_consistent(book));
}

TEST(CancelExecute, CancelLastOrderRemovesLevel) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    apply(book, make_cancel(1, 100));

    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.level_count(Side::Ask), 0u);  // no empty levels ever
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 0);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(CancelExecute, MidQueueRemovalPreservesFifoOfOthers) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 10));
    apply(book, make_add(2, Side::Ask, 1'010'000, 20));
    apply(book, make_add(3, Side::Ask, 1'010'000, 30));
    apply(book, make_cancel(2, 20));

    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 40);
    EXPECT_TRUE(book_consistent(book));
}

TEST(CancelExecute, ExecuteHasSameBookEffectAsCancel) {
    Book cancel_book;
    Book exec_book;
    for (Book* b : {&cancel_book, &exec_book}) {
        apply(*b, make_add(1, Side::Ask, 1'010'000, 100));
        apply(*b, make_add(2, Side::Ask, 1'010'000, 50));
    }
    apply(cancel_book, make_cancel(1, 40));
    apply(exec_book, make_execute(1, 40));  // execution price never affects book state

    EXPECT_EQ(cancel_book.size_at(Side::Ask, 1'010'000),
              exec_book.size_at(Side::Ask, 1'010'000));
    EXPECT_EQ(cancel_book.find_order(1)->remaining_qty,
              exec_book.find_order(1)->remaining_qty);
    EXPECT_EQ(cancel_book.order_count(), exec_book.order_count());
    EXPECT_EQ(cancel_book.level_count(Side::Ask), exec_book.level_count(Side::Ask));
    EXPECT_TRUE(book_consistent(exec_book));
}

TEST(CancelExecute, PartialThenFullExecuteRemovesFrontOrder) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'010'000, 50));

    apply(book, make_execute(1, 40));
    EXPECT_EQ(book.find_order(1)->remaining_qty, 60u);
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 110);
    EXPECT_TRUE(book_consistent(book));

    apply(book, make_execute(1, 60));
    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{2}));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 50);
    EXPECT_TRUE(book_consistent(book));
}

// ---------------------------------------------------------------------------
// Task 5: Delete
// ---------------------------------------------------------------------------

Event make_delete(OrderId id, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Delete;
    e.timestamp = ts;
    e.order_id = id;
    return e;
}

TEST(DeleteEvent, DeleteRemovesOrderWithRemainingShares) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    apply(book, make_delete(1));  // 100 shares still remaining: removed anyway

    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 50);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(DeleteEvent, DeleteIgnoresQuantityAndPrice) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));

    Event e;
    e.type = EventType::Delete;
    e.order_id = 1;
    e.quantity = 999'999;  // ignored
    e.price = -5;          // ignored (price validation applies to Add/Replace only)
    apply(book, e);

    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(DeleteEvent, DeleteLastOrderRemovesLevelAndUpdatesBbo) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 990'000, 25));
    apply(book, make_delete(1));

    const auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->price, 990'000);  // BBO fell to the next level
    EXPECT_EQ(bb->total_shares, 25);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 0);
    EXPECT_TRUE(book_consistent(book));
}

// ---------------------------------------------------------------------------
// Task 6: Replace
// ---------------------------------------------------------------------------

Event make_replace(OrderId id, OrderId new_id, Price price, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Replace;
    e.timestamp = ts;
    e.order_id = id;
    // Deliberately leaves e.side at its default: the book must resolve the
    // side from the replaced order, never from the event.
    e.price = price;
    e.quantity = qty;
    e.new_order_id = new_id;
    return e;
}

TEST(ReplaceEvent, SamePriceLosesTimePriority) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'010'000, 50));
    apply(book, make_replace(1, 10, 1'010'000, 100));  // same price, same qty

    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{2, 10}));  // re-enqueued at the back
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 150);
    EXPECT_TRUE(book_consistent(book));
}

TEST(ReplaceEvent, PriceMoveCreatesAndRemovesLevels) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));  // alone at its level
    apply(book, make_add(2, Side::Bid, 990'000, 25));
    apply(book, make_replace(1, 10, 980'000, 40));

    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 0);  // old level removed
    EXPECT_EQ(book.size_at(Side::Bid, 980'000), 40);   // new level created
    const auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->price, 990'000);
    EXPECT_EQ(book.level_count(Side::Bid), 2u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(ReplaceEvent, KeepsSideFromBookNotEvent) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    // make_replace leaves event.side == Side::Bid (default); the book must
    // still keep the order on the Ask side.
    apply(book, make_replace(1, 10, 1'020'000, 60));

    const auto info = book.find_order(10);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->side, Side::Ask);
    EXPECT_EQ(info->price, 1'020'000);
    EXPECT_EQ(info->remaining_qty, 60u);  // event quantity, not carried over
    EXPECT_EQ(book.level_count(Side::Bid), 0u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(ReplaceEvent, OldIdGoneNewIdPresentCountsStable) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_replace(1, 2, 1'000'000, 70));

    EXPECT_FALSE(book.find_order(1).has_value());
    ASSERT_TRUE(book.find_order(2).has_value());
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 70);
    EXPECT_TRUE(book_consistent(book));
}

// ---------------------------------------------------------------------------
// Task 7: strict error policy (spec section 4) — one test per bullet
// ---------------------------------------------------------------------------

TEST(ErrorPolicy, CancelUnknownOrderThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_cancel(999, 10)), BookError);
}

TEST(ErrorPolicy, ExecuteUnknownOrderThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_execute(999, 10)), BookError);
}

TEST(ErrorPolicy, DeleteUnknownOrderThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_delete(999)), BookError);
}

TEST(ErrorPolicy, ReplaceUnknownOrderThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_replace(999, 1000, 1'000'000, 10)), BookError);
}

TEST(ErrorPolicy, AddDuplicateOrderIdThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_add(1, Side::Ask, 1'010'000, 50)), BookError);
}

TEST(ErrorPolicy, ReplaceExistingNewOrderIdThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 990'000, 50));
    EXPECT_THROW(apply(book, make_replace(1, 2, 1'000'000, 10)), BookError);
}

TEST(ErrorPolicy, ReplaceNewOrderIdEqualsOrderIdThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_replace(1, 1, 1'000'000, 10)), BookError);
}

TEST(ErrorPolicy, CancelZeroQuantityThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_cancel(1, 0)), BookError);
}

TEST(ErrorPolicy, ExecuteZeroQuantityThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_execute(1, 0)), BookError);
}

TEST(ErrorPolicy, CancelQuantityExceedingRemainingThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_cancel(1, 101)), BookError);
}

TEST(ErrorPolicy, ExecuteQuantityExceedingRemainingThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_execute(1, 60));  // remaining 40
    EXPECT_THROW(apply(book, make_execute(1, 41)), BookError);
}

TEST(ErrorPolicy, AddZeroQuantityThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_add(1, Side::Bid, 1'000'000, 0)), BookError);
}

TEST(ErrorPolicy, AddZeroPriceThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_add(1, Side::Bid, 0, 100)), BookError);
}

TEST(ErrorPolicy, AddNegativePriceThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_add(1, Side::Bid, -1'000'000, 100)), BookError);
}

TEST(ErrorPolicy, ReplaceZeroQuantityThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_replace(1, 2, 1'000'000, 0)), BookError);
}

TEST(ErrorPolicy, ReplaceNonPositivePriceThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_replace(1, 2, 0, 10)), BookError);
    EXPECT_THROW(apply(book, make_replace(1, 2, -7, 10)), BookError);
}

TEST(ErrorPolicy, BookErrorCarriesOffendingEventAndReason) {
    Book book;
    const Event bad = make_cancel(777, 10);
    try {
        apply(book, bad);
        FAIL() << "expected BookError";
    } catch (const BookError& err) {
        EXPECT_EQ(err.event().type, EventType::Cancel);
        EXPECT_EQ(err.event().order_id, 777u);
        EXPECT_EQ(err.event().quantity, 10u);
        EXPECT_NE(std::string(err.what()), "");
    }
}

// ---------------------------------------------------------------------------
// Task 8: event-tape scenarios — queries AND consistency after EVERY event
// ---------------------------------------------------------------------------

// Applies the event and asserts full-book consistency at the call site
// (a macro so ASSERT aborts the enclosing TEST, not a helper function).
#define APPLY_CHECKED(book, event)                \
    do {                                          \
        apply((book), (event));                   \
        ASSERT_TRUE(book_consistent((book)));     \
    } while (0)

TEST(EventTapes, AddCancelDeleteLifecycle) {
    Book book;

    APPLY_CHECKED(book, make_add(1, Side::Bid, 1'000'000, 100, 1));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);
    EXPECT_EQ(book.order_count(), 1u);

    APPLY_CHECKED(book, make_add(2, Side::Ask, 1'010'000, 80, 2));
    EXPECT_EQ(book.best_ask()->price, 1'010'000);
    EXPECT_EQ(book.best_ask()->total_shares, 80);

    APPLY_CHECKED(book, make_cancel(1, 25, 3));
    EXPECT_EQ(book.find_order(1)->remaining_qty, 75u);
    EXPECT_EQ(book.best_bid()->total_shares, 75);

    APPLY_CHECKED(book, make_execute(2, 30, 4));
    EXPECT_EQ(book.find_order(2)->remaining_qty, 50u);
    EXPECT_EQ(book.best_ask()->total_shares, 50);

    APPLY_CHECKED(book, make_delete(1, 5));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.level_count(Side::Bid), 0u);

    APPLY_CHECKED(book, make_execute(2, 50, 6));
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
}

TEST(EventTapes, PartialExecutesSweepLevelInFifoOrder) {
    Book book;

    APPLY_CHECKED(book, make_add(1, Side::Ask, 1'010'000, 100, 1));
    APPLY_CHECKED(book, make_add(2, Side::Ask, 1'010'000, 50, 2));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 150);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{1, 2}));

    APPLY_CHECKED(book, make_execute(1, 40, 3));
    EXPECT_EQ(book.find_order(1)->remaining_qty, 60u);
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 110);

    APPLY_CHECKED(book, make_execute(1, 60, 4));
    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{2}));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 50);

    APPLY_CHECKED(book, make_execute(2, 50, 5));
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(EventTapes, ReplacePriceMoveAndPriorityLoss) {
    Book book;

    APPLY_CHECKED(book, make_add(1, Side::Bid, 1'000'000, 100, 1));
    APPLY_CHECKED(book, make_add(2, Side::Bid, 1'000'000, 50, 2));
    APPLY_CHECKED(book, make_add(3, Side::Bid, 990'000, 30, 3));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);
    EXPECT_EQ(book.best_bid()->total_shares, 150);

    // Price move down: order 1 leaves the best level, joins 990'000 at the back.
    APPLY_CHECKED(book, make_replace(1, 4, 990'000, 100, 4));
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 50);
    EXPECT_EQ(book.size_at(Side::Bid, 990'000), 130);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Bid, 990'000),
              (std::vector<OrderId>{3, 4}));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);

    // Replacing the last order at the best level removes that level; BBO falls.
    APPLY_CHECKED(book, make_replace(2, 5, 990'000, 25, 5));
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 0);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_EQ(book.best_bid()->price, 990'000);
    EXPECT_EQ(book.best_bid()->total_shares, 155);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Bid, 990'000),
              (std::vector<OrderId>{3, 4, 5}));
    EXPECT_EQ(book.order_count(), 3u);
}

TEST(EventTapes, LevelChurnAndBboUpdatesBothSides) {
    Book book;

    APPLY_CHECKED(book, make_add(1, Side::Bid, 990'000, 10, 1));
    EXPECT_EQ(book.best_bid()->price, 990'000);

    APPLY_CHECKED(book, make_add(2, Side::Bid, 1'000'000, 20, 2));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);  // better bid becomes BBO
    EXPECT_EQ(book.level_count(Side::Bid), 2u);

    APPLY_CHECKED(book, make_add(3, Side::Ask, 1'020'000, 30, 3));
    EXPECT_EQ(book.best_ask()->price, 1'020'000);

    APPLY_CHECKED(book, make_add(4, Side::Ask, 1'010'000, 40, 4));
    EXPECT_EQ(book.best_ask()->price, 1'010'000);  // better ask becomes BBO
    EXPECT_EQ(book.level_count(Side::Ask), 2u);

    APPLY_CHECKED(book, make_delete(2, 5));
    EXPECT_EQ(book.best_bid()->price, 990'000);  // BBO falls back
    EXPECT_EQ(book.level_count(Side::Bid), 1u);

    APPLY_CHECKED(book, make_execute(4, 40, 6));
    EXPECT_EQ(book.best_ask()->price, 1'020'000);  // BBO rises back
    EXPECT_EQ(book.level_count(Side::Ask), 1u);

    APPLY_CHECKED(book, make_cancel(3, 5, 7));
    EXPECT_EQ(book.best_ask()->total_shares, 25);

    APPLY_CHECKED(book, make_delete(3, 8));
    EXPECT_FALSE(book.best_ask().has_value());

    APPLY_CHECKED(book, make_delete(1, 9));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.level_count(Side::Bid), 0u);
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
}

#undef APPLY_CHECKED

// ---------------------------------------------------------------------------
// Final-review additions: fail-loud on unknown type; replace alone at level
// ---------------------------------------------------------------------------

TEST(ErrorPolicy, UnknownEventTypeThrows) {
    Book book;
    Event e;
    e.type = static_cast<EventType>(99);
    e.order_id = 1;
    EXPECT_THROW(apply(book, e), BookError);
}

TEST(ReplaceEvent, SamePriceAloneAtLevelRecreatesLevel) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));  // alone at its level
    apply(book, make_replace(1, 2, 1'010'000, 60));  // erase drops the level; insert recreates it

    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{2}));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 60);
    EXPECT_EQ(book.level_count(Side::Ask), 1u);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_TRUE(book_consistent(book));
}

}  // namespace
