#pragma once

// FROZEN copy of Milestone 1/2 internals — reference oracle, do not improve.
//
// This is the std::map / std::list / std::unordered_map book exactly as it
// shipped in Milestone 1 (plus Milestone 2's is_crossed), renamed BaselineBook.
// It exists so every Milestone 3+ internal rewrite can be A/B benchmarked and
// differentially fuzzed against the original, in the same binary. Linked only
// by book_bench and book_fuzz_test; never by production targets. Kept forever.

#include "xeas/book.h"  // BookError, BBO, OrderInfo, Event aliases

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xeas {

class BaselineBook {
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

    friend void apply(BaselineBook& book, const Event& event);
    friend std::vector<std::string> check_invariants(const BaselineBook& book);
};

// Same contracts as the Book overloads (see xeas/book.h).
void apply(BaselineBook& book, const Event& event);
std::vector<std::string> check_invariants(const BaselineBook& book);
bool is_crossed(const BaselineBook& book);

}  // namespace xeas
