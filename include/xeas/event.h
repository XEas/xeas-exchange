#pragma once

#include <cstdint>

namespace xeas {

// Fixed-width scalar aliases — integers everywhere, no floating point.
using Price = std::int64_t;       // price in ticks (ITCH: fixed point x10,000)
using Qty = std::uint32_t;        // shares
using OrderId = std::uint64_t;    // unique per instrument per day
using Timestamp = std::uint64_t;  // ns since midnight; carried through, never interpreted

enum class Side : std::uint8_t { Bid, Ask };

enum class EventType : std::uint8_t { Add, Cancel, Delete, Execute, Replace };

// Canonical event — the hard boundary between feed handlers and the book.
struct Event {
    EventType type{EventType::Add};
    Timestamp timestamp{0};
    OrderId order_id{0};
    Side side{Side::Bid};     // meaningful on Add only; other types resolve side via order lookup
    Price price{0};           // meaningful on Add and Replace
    Qty quantity{0};          // meaning depends on type
    OrderId new_order_id{0};  // Replace only: id of the replacement order
};

}  // namespace xeas
