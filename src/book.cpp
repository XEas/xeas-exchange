#include "xeas/book.h"

#include <iterator>

namespace xeas {

// --- queries -----------------------------------------------------------------

std::optional<BBO> Book::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    const auto& [price, level] = *bids_.begin();
    return BBO{price, level.total_shares};
}

std::optional<BBO> Book::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    const auto& [price, level] = *asks_.begin();
    return BBO{price, level.total_shares};
}

std::int64_t Book::size_at(Side side, Price price) const {
    if (side == Side::Bid) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_shares;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_shares;
}

std::optional<OrderInfo> Book::find_order(OrderId order_id) const {
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return std::nullopt;
    }
    const OrderHandle& h = it->second;
    return OrderInfo{h.side, h.price, h.it->remaining};
}

std::size_t Book::order_count() const {
    return orders_.size();
}

std::size_t Book::level_count(Side side) const {
    return side == Side::Bid ? bids_.size() : asks_.size();
}

// --- mutation helpers ----------------------------------------------------------

void Book::insert_order(Side side, Price price, OrderId order_id, Qty quantity) {
    Level& level = (side == Side::Bid) ? bids_[price] : asks_[price];
    level.orders.push_back(Order{order_id, quantity});
    const auto pos = std::prev(level.orders.end());
    level.total_shares += static_cast<std::int64_t>(quantity);
    orders_.emplace(order_id, OrderHandle{side, price, &level, pos});
}

void Book::erase_order(OrderIndex::iterator index_it) {
    OrderHandle& h = index_it->second;
    h.level->total_shares -= static_cast<std::int64_t>(h.it->remaining);
    h.level->orders.erase(h.it);
    if (h.level->orders.empty()) {
        // No empty levels ever: drop the level immediately (O(log levels)).
        if (h.side == Side::Bid) {
            bids_.erase(h.price);
        } else {
            asks_.erase(h.price);
        }
    }
    orders_.erase(index_it);
}

// --- apply ---------------------------------------------------------------------

void apply(Book& book, const Event& event) {
    switch (event.type) {
    case EventType::Add:
        book.insert_order(event.side, event.price, event.order_id, event.quantity);
        break;
    case EventType::Cancel:
    case EventType::Execute: {
        // Identical book effect (spec section 2): shares come off the resting order.
        // Execute is kept distinct only for later trade stats; execution price
        // never affects book state.
        const auto index_it = book.orders_.find(event.order_id);
        auto& h = index_it->second;
        if (event.quantity == h.it->remaining) {
            book.erase_order(index_it);  // reaches zero: removed immediately
        } else {
            h.it->remaining -= event.quantity;
            h.level->total_shares -= static_cast<std::int64_t>(event.quantity);
        }
        break;
    }
    case EventType::Delete: {
        // Remove entirely, whatever the remaining quantity; event.quantity and
        // event.price are ignored by definition (spec section 2).
        const auto index_it = book.orders_.find(event.order_id);
        book.erase_order(index_it);
        break;
    }
    case EventType::Replace: {
        // Atomic remove + insert on the SAME side (resolved from the replaced
        // order, never from event.side), at the BACK of the (possibly new)
        // level's queue — time priority is lost (ITCH semantics).
        const auto index_it = book.orders_.find(event.order_id);
        const Side side = index_it->second.side;
        book.erase_order(index_it);
        book.insert_order(side, event.price, event.new_order_id, event.quantity);
        break;
    }
    }
}

// --- check_invariants ------------------------------------------------------------
// Out of band: walks the whole book; never called from apply().

std::vector<std::string> check_invariants(const Book& book) {
    std::vector<std::string> violations;

    // Invariant 1: best bid < best ask when both sides are non-empty.
    if (!book.bids_.empty() && !book.asks_.empty()) {
        const Price best_bid = book.bids_.begin()->first;
        const Price best_ask = book.asks_.begin()->first;
        if (best_bid >= best_ask) {
            violations.push_back("book crossed or locked: best bid " + std::to_string(best_bid) +
                                 " >= best ask " + std::to_string(best_ask));
        }
    }

    std::size_t orders_in_levels = 0;

    const auto check_side = [&](const auto& levels, Side side, const char* name) {
        std::optional<Price> prev_price;
        for (const auto& [price, level] : levels) {
            // Invariant 3 (levels): no empty levels.
            if (level.orders.empty()) {
                violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                     " is empty");
            }
            // Invariant 4: strictly sorted best-first (descending bids, ascending asks).
            // Guaranteed by std::map today; kept so Milestone 3 internals stay honest.
            if (prev_price.has_value()) {
                const bool ordered =
                    (side == Side::Bid) ? (*prev_price > price) : (*prev_price < price);
                if (!ordered) {
                    violations.push_back(std::string(name) +
                                         " levels not strictly sorted at price " +
                                         std::to_string(price));
                }
            }
            prev_price = price;

            std::int64_t sum = 0;
            for (const auto& order : level.orders) {
                // Invariant 3 (orders): no zero-quantity orders.
                if (order.remaining == 0) {
                    violations.push_back("zero-quantity order " + std::to_string(order.id) +
                                         " at " + std::string(name) + " level " +
                                         std::to_string(price));
                }
                sum += static_cast<std::int64_t>(order.remaining);

                // Invariant 5 (level -> index): every order in a level is indexed,
                // and its handle points at exactly this node on this side/price.
                const auto idx = book.orders_.find(order.id);
                if (idx == book.orders_.end()) {
                    violations.push_back("order " + std::to_string(order.id) + " in " + name +
                                         " level " + std::to_string(price) +
                                         " missing from index");
                } else {
                    const auto& h = idx->second;
                    if (h.side != side || h.price != price || &*h.it != &order) {
                        violations.push_back("index entry for order " + std::to_string(order.id) +
                                             " disagrees with its level");
                    }
                }
                ++orders_in_levels;
            }

            // Invariant 2: cached aggregate equals the sum of remaining shares.
            if (sum != level.total_shares) {
                violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                     " cached total_shares " +
                                     std::to_string(level.total_shares) +
                                     " != sum of orders " + std::to_string(sum));
            }
        }
    };

    check_side(book.bids_, Side::Bid, "bid");
    check_side(book.asks_, Side::Ask, "ask");

    // Invariant 5 (index -> levels): same count both ways. Combined with the
    // per-order handle check above this makes the agreement exact.
    if (orders_in_levels != book.orders_.size()) {
        violations.push_back("order index size " + std::to_string(book.orders_.size()) +
                             " != orders present in levels " +
                             std::to_string(orders_in_levels));
    }

    return violations;
}

}  // namespace xeas
