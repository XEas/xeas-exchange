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

// --- apply ---------------------------------------------------------------------

void apply(Book& book, const Event& event) {
    switch (event.type) {
    case EventType::Add:
        book.insert_order(event.side, event.price, event.order_id, event.quantity);
        break;
    case EventType::Cancel:
    case EventType::Execute:
        break;  // implemented in Task 4
    case EventType::Delete:
        break;  // implemented in Task 5
    case EventType::Replace:
        break;  // implemented in Task 6
    }
}

}  // namespace xeas
