#pragma once

#include "xeas/event.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace xeas {

// Thrown by apply() on any malformed event (spec section 4: strict error
// policy). Carries the offending event and a human-readable reason.
// After a throw, book state is unspecified; the replay driver must abort.
class BookError : public std::runtime_error {
public:
    BookError(const Event& event, const std::string& reason)
        : std::runtime_error(reason), event_(event) {}

    const Event& event() const noexcept { return event_; }

private:
    Event event_;
};

// Best price level snapshot returned by best_bid()/best_ask().
struct BBO {
    Price price;
    std::int64_t total_shares;
};

// Snapshot of a live order returned by find_order().
struct OrderInfo {
    Side side;
    Price price;
    Qty remaining_qty;
};

// Single-instrument, single-threaded L3 book. Pure state: no I/O, no clocks,
// no matching. Mutated exclusively via apply(); inspected via const queries.
class Book {
public:
    // Queries: const, allocation-free, O(1) or O(log levels).
    std::optional<BBO> best_bid() const;
    std::optional<BBO> best_ask() const;
    std::int64_t size_at(Side side, Price price) const;   // 0 if no such level
    std::optional<OrderInfo> find_order(OrderId order_id) const;
    std::size_t order_count() const;                      // total live orders
    std::size_t level_count(Side side) const;

private:
    struct Order {
        OrderId id;
        Qty remaining;
    };

    struct Level {
        std::list<Order> orders;       // FIFO: front = oldest (highest time priority)
        std::int64_t total_shares{0};  // cached aggregate, maintained incrementally
    };

    struct OrderHandle {
        Side side;
        Price price;                    // key for the O(log levels) map erase when the level empties
        Level* level;                   // stable: std::map nodes never move
        std::list<Order>::iterator it;  // stable: std::list iterators never invalidate
    };

    using BidMap = std::map<Price, Level, std::greater<>>;  // begin() = highest bid
    using AskMap = std::map<Price, Level, std::less<>>;     // begin() = lowest ask
    using OrderIndex = std::unordered_map<OrderId, OrderHandle>;

    // Create the level if needed, enqueue at the back (time priority), index the order.
    void insert_order(Side side, Price price, OrderId order_id, Qty quantity);
    // Unlink the order from its level, drop the level if it emptied, erase from the index.
    void erase_order(OrderIndex::iterator index_it);

    BidMap bids_;
    AskMap asks_;
    OrderIndex orders_;

    friend void apply(Book& book, const Event& event);
    friend std::vector<std::string> check_invariants(const Book& book);
    friend struct BookTestPeer;  // test-only backdoor for FIFO/invariant tests
};

// Applies one canonical event to the book (pure state mutation).
// Throws BookError on malformed events (spec section 4); all validation
// happens before any mutation, but callers must still treat post-throw
// state as unspecified and abort replay.
void apply(Book& book, const Event& event);

// Out-of-band consistency checker (never called inside apply(); the hot path
// stays clean). Walks the entire book and returns a description of every
// violated invariant; an empty vector means the book is consistent.
std::vector<std::string> check_invariants(const Book& book);

}  // namespace xeas
