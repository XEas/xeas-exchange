#pragma once

#include "xeas/event.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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
    static constexpr std::uint32_t kNone = 0xFFFF'FFFF;

    struct OrderNode {                 // pool slab entry, addressed by u32 index
        OrderId id;
        Qty remaining;
        std::uint32_t prev, next;      // intrusive FIFO links; kNone at the ends
    };                                 // 24 bytes

    struct Level {                     // level pool entry
        std::uint32_t head = kNone;    // front = oldest (highest time priority)
        std::uint32_t tail = kNone;
        std::int64_t total_shares = 0; // cached aggregate, maintained incrementally
        Price price;                   // a level knows where it lives, so the
        Side side;                     // handle no longer carries side/price
    };

    class OrderPool {                  // freelist over a growing vector slab;
    public:                            // u32 indices survive vector growth
        std::uint32_t allocate(OrderId id, Qty qty);
        void release(std::uint32_t idx);
        OrderNode& operator[](std::uint32_t idx) { return nodes_[idx]; }
        const OrderNode& operator[](std::uint32_t idx) const { return nodes_[idx]; }
        std::size_t capacity() const { return nodes_.size(); }
        std::uint32_t free_head() const { return free_head_; }
    private:
        std::vector<OrderNode> nodes_;
        std::uint32_t free_head_ = kNone;   // freelist threaded through next
        friend struct BookTestPeer;
    };

    class LevelPool {                  // identical pattern over Level
    public:
        std::uint32_t allocate(Price price, Side side);
        void release(std::uint32_t idx);
        Level& operator[](std::uint32_t idx) { return levels_[idx]; }
        const Level& operator[](std::uint32_t idx) const { return levels_[idx]; }
        std::size_t capacity() const { return levels_.size(); }
        std::uint32_t free_head() const { return free_head_; }
    private:
        std::vector<Level> levels_;
        std::uint32_t free_head_ = kNone;   // freelist threaded through head
        friend struct BookTestPeer;
    };

    struct OrderHandle {               // 8 bytes (was Side+Price+ptr+iterator)
        std::uint32_t node;
        std::uint32_t level;
    };

    using BidMap = std::map<Price, std::uint32_t, std::greater<>>;  // price -> level index
    using AskMap = std::map<Price, std::uint32_t, std::less<>>;
    using OrderIndexMap = std::unordered_map<OrderId, OrderHandle>;

    // kNone if the side has no level at this price.
    std::uint32_t level_index(Side side, Price price) const;
    // Allocate a level and install it in the side's price structure.
    std::uint32_t create_level(Side side, Price price);
    // Create the level if needed, enqueue at the back (time priority), index the order.
    void insert_order(Side side, Price price, OrderId order_id, Qty quantity);
    // Unlink the order from its level, drop the level if it emptied, erase from the index.
    void erase_order(OrderIndexMap::iterator index_it);
    // The single ordered walker (strict best-first). Out-of-band only: used by
    // check_invariants and BookTestPeer, never on the hot path.
    void for_each_level(Side side,
                        const std::function<void(std::uint32_t, const Level&)>& fn) const;

    OrderPool order_pool_;
    LevelPool level_pool_;
    BidMap bids_;
    AskMap asks_;
    OrderIndexMap orders_;

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

// True iff both sides are non-empty and best bid >= best ask (locked or
// crossed). O(1). Milestone 2's replay driver probes this after every event
// to classify crossed/locked books as data reality (halts, opening/closing
// crosses) without string-matching check_invariants() output.
bool is_crossed(const Book& book);

}  // namespace xeas
