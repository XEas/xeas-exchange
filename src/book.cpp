#include "xeas/book.h"

#include <algorithm>
#include <bit>

namespace {

// Provisional band tuning (spec §3.1): finalized as named-constant tuning from
// a real-day price-range histogram during phase 3 — see BENCH.md.
constexpr std::size_t kInitialBandTicks = 4'096;
constexpr std::size_t kMaxBandTicks = 262'144;

// Mirrors Book::kNone (asserted equal in band_anchor).
constexpr std::uint32_t kNoOffset = 0xFFFF'FFFFu;

inline void set_occupancy(std::vector<std::uint64_t>& words,
                          std::vector<std::uint64_t>& summary, std::size_t off) {
    words[off >> 6] |= (1ULL << (off & 63));
    summary[off >> 12] |= (1ULL << ((off >> 6) & 63));
}

inline void clear_occupancy(std::vector<std::uint64_t>& words,
                            std::vector<std::uint64_t>& summary, std::size_t off) {
    words[off >> 6] &= ~(1ULL << (off & 63));
    if (words[off >> 6] == 0) {
        summary[off >> 12] &= ~(1ULL << ((off >> 6) & 63));
    }
}

inline std::uint32_t top_bit(std::uint64_t w) {   // position of the highest set bit; w != 0
    return 63u - static_cast<std::uint32_t>(std::countl_zero(w));
}

// Best occupied tick offset over the whole band; kNoOffset if the band is empty.
// Highest = true scans for the highest offset (bids), false for the lowest (asks).
template <bool Highest>
std::uint32_t scan_all(const std::vector<std::uint64_t>& words,
                       const std::vector<std::uint64_t>& summary) {
    if constexpr (Highest) {
        for (std::size_t sw = summary.size(); sw-- > 0;) {
            if (summary[sw] == 0) continue;
            const std::size_t word = sw * 64 + top_bit(summary[sw]);
            return static_cast<std::uint32_t>(word * 64 + top_bit(words[word]));
        }
    } else {
        for (std::size_t sw = 0; sw < summary.size(); ++sw) {
            if (summary[sw] == 0) continue;
            const std::size_t word =
                sw * 64 + static_cast<std::size_t>(std::countr_zero(summary[sw]));
            return static_cast<std::uint32_t>(
                word * 64 + static_cast<std::size_t>(std::countr_zero(words[word])));
        }
    }
    return kNoOffset;
}

// Best occupied tick offset strictly worse than `from` (below it when Highest,
// above it otherwise); kNoOffset if none. Word remainder first, then summary.
template <bool Highest>
std::uint32_t scan_next(const std::vector<std::uint64_t>& words,
                        const std::vector<std::uint64_t>& summary, std::uint32_t from) {
    const std::size_t w = from >> 6;
    if constexpr (Highest) {
        const std::uint64_t in_word = words[w] & ((1ULL << (from & 63)) - 1);
        if (in_word != 0) return static_cast<std::uint32_t>(w * 64 + top_bit(in_word));
        std::size_t sw = w >> 6;
        std::uint64_t in_summary = summary[sw] & ((1ULL << (w & 63)) - 1);
        for (;;) {
            if (in_summary != 0) {
                const std::size_t word = sw * 64 + top_bit(in_summary);
                return static_cast<std::uint32_t>(word * 64 + top_bit(words[word]));
            }
            if (sw == 0) return kNoOffset;
            in_summary = summary[--sw];
        }
    } else {
        const std::uint64_t in_word = words[w] & ~((2ULL << (from & 63)) - 1);
        if (in_word != 0)
            return static_cast<std::uint32_t>(
                w * 64 + static_cast<std::size_t>(std::countr_zero(in_word)));
        std::size_t sw = w >> 6;
        std::uint64_t in_summary = summary[sw] & ~((2ULL << (w & 63)) - 1);
        for (;;) {
            if (in_summary != 0) {
                const std::size_t word =
                    sw * 64 + static_cast<std::size_t>(std::countr_zero(in_summary));
                return static_cast<std::uint32_t>(
                    word * 64 + static_cast<std::size_t>(std::countr_zero(words[word])));
            }
            if (++sw == summary.size()) return kNoOffset;
            in_summary = summary[sw];
        }
    }
}

}  // namespace

namespace xeas {

// --- pools -------------------------------------------------------------------

std::uint32_t Book::OrderPool::allocate(OrderId id, Qty qty) {
    std::uint32_t idx;
    if (free_head_ != kNone) {
        idx = free_head_;
        free_head_ = nodes_[idx].next;   // freelist is threaded through next
    } else {
        idx = static_cast<std::uint32_t>(nodes_.size());
        nodes_.emplace_back();
    }
    OrderNode& node = nodes_[idx];
    node.id = id;
    node.remaining = qty;
    node.prev = kNone;
    node.next = kNone;
    return idx;
}

void Book::OrderPool::release(std::uint32_t idx) {
    nodes_[idx].next = free_head_;
    nodes_[idx].prev = kNone;
    free_head_ = idx;
}

std::uint32_t Book::LevelPool::allocate(Price price, Side side) {
    std::uint32_t idx;
    if (free_head_ != kNone) {
        idx = free_head_;
        free_head_ = levels_[idx].head;  // freelist is threaded through head
    } else {
        idx = static_cast<std::uint32_t>(levels_.size());
        levels_.emplace_back();
    }
    Level& level = levels_[idx];
    level.head = kNone;
    level.tail = kNone;
    level.total_shares = 0;
    level.price = price;
    level.side = side;
    return idx;
}

void Book::LevelPool::release(std::uint32_t idx) {
    levels_[idx].head = free_head_;
    levels_[idx].tail = kNone;
    free_head_ = idx;
}

// --- banded flat price structure (spec §3.1) -----------------------------------

template <class Compare>
void Book::band_anchor(FlatSide<Compare>& s, Price price) {
    static_assert(kNoOffset == Book::kNone);
    // Created kInitialBandTicks wide, centered on the first Add on this side.
    s.lo = std::max<Price>(1, price - static_cast<Price>(kInitialBandTicks / 2));
    s.slots.assign(kInitialBandTicks, 0);
    s.words.assign(kInitialBandTicks / 64, 0);
    s.summary.assign((kInitialBandTicks / 64 + 63) / 64, 0);
    s.best = kNone;
}

template <class Compare>
bool Book::band_grow_to_cover(FlatSide<Compare>& s, Price price) {
    const Price cur_lo = s.lo;
    const Price cur_hi = cur_lo + static_cast<Price>(s.slots.size());   // exclusive
    const Price need_lo = std::min(cur_lo, price);
    const Price need_hi = std::max(cur_hi, price + 1);                  // exclusive
    const auto span = static_cast<std::size_t>(need_hi - need_lo);
    if (span > kMaxBandTicks) {
        return false;   // outside the maximum window around the anchor: overflow
    }
    std::size_t new_size = s.slots.size();
    while (new_size < span) new_size *= 2;   // ×2 growth; caps at kMaxBandTicks exactly
    const Price pad = static_cast<Price>((new_size - span) / 2);
    const Price new_lo = std::max<Price>(1, need_lo - pad);   // re-anchor with padding

    std::vector<std::uint32_t> new_slots(new_size, 0);
    std::copy(s.slots.begin(), s.slots.end(),
              new_slots.begin() + static_cast<std::ptrdiff_t>(cur_lo - new_lo));
    s.slots = std::move(new_slots);
    s.lo = new_lo;

    // Migrate overflow entries the wider band now covers (invariant 7: no price
    // may be present in both).
    for (auto it = s.overflow.begin(); it != s.overflow.end();) {
        if (it->first >= new_lo && it->first < new_lo + static_cast<Price>(new_size)) {
            s.slots[static_cast<std::size_t>(it->first - new_lo)] = it->second + 1;
            it = s.overflow.erase(it);
        } else {
            ++it;
        }
    }

    // Rebuild occupancy and the cached best from the new slots (rare O(band)).
    s.words.assign(new_size / 64, 0);
    s.summary.assign((new_size / 64 + 63) / 64, 0);
    for (std::size_t off = 0; off < new_size; ++off) {
        if (s.slots[off] != 0) set_occupancy(s.words, s.summary, off);
    }
    s.best = scan_all<std::is_same_v<Compare, std::greater<>>>(s.words, s.summary);
    return true;
}

template <class Compare>
std::uint32_t Book::side_find(const FlatSide<Compare>& s, Price price) {
    if (!s.slots.empty() && price >= s.lo &&
        price < s.lo + static_cast<Price>(s.slots.size())) {
        const std::uint32_t v = s.slots[static_cast<std::size_t>(price - s.lo)];
        return v == 0 ? kNone : v - 1;
    }
    const auto it = s.overflow.find(price);
    return it == s.overflow.end() ? kNone : it->second;
}

template <class Compare>
void Book::side_insert(FlatSide<Compare>& s, Price price, std::uint32_t level_idx) {
    ++s.level_count;
    if (s.slots.empty()) {
        band_anchor(s, price);
    } else if (price < s.lo || price >= s.lo + static_cast<Price>(s.slots.size())) {
        if (!band_grow_to_cover(s, price)) {
            s.overflow.emplace(price, level_idx);
            return;
        }
    }
    const auto off = static_cast<std::uint32_t>(price - s.lo);
    s.slots[off] = level_idx + 1;
    set_occupancy(s.words, s.summary, off);
    constexpr bool highest = std::is_same_v<Compare, std::greater<>>;
    if (s.best == kNone || (highest ? off > s.best : off < s.best)) {
        s.best = off;
    }
}

template <class Compare>
void Book::side_erase(FlatSide<Compare>& s, Price price) {
    --s.level_count;
    if (!s.slots.empty() && price >= s.lo &&
        price < s.lo + static_cast<Price>(s.slots.size())) {
        const auto off = static_cast<std::uint32_t>(price - s.lo);
        if (s.slots[off] != 0) {
            s.slots[off] = 0;
            clear_occupancy(s.words, s.summary, off);
            if (s.best == off) {
                constexpr bool highest = std::is_same_v<Compare, std::greater<>>;
                s.best = scan_next<highest>(s.words, s.summary, off);
            }
            return;
        }
    }
    s.overflow.erase(price);
}

template <class Compare>
std::optional<std::pair<Price, std::uint32_t>> Book::side_best(const FlatSide<Compare>& s) {
    std::optional<std::pair<Price, std::uint32_t>> best;
    if (s.best != kNone) {
        best = std::make_pair(s.lo + static_cast<Price>(s.best), s.slots[s.best] - 1);
    }
    if (!s.overflow.empty()) {
        const auto& [price, idx] = *s.overflow.begin();
        // An overflow order CAN be the best (a lone far bid/ask) — still O(1).
        if (!best.has_value() || Compare{}(price, best->first)) {
            best = std::make_pair(price, idx);
        }
    }
    return best;
}

template <class Compare>
void Book::side_for_each(const FlatSide<Compare>& s,
                         const std::function<void(std::uint32_t, const Level&)>& fn) const {
    constexpr bool highest = std::is_same_v<Compare, std::greater<>>;
    // Fresh scans, deliberately ignoring the best cache: the walk must reflect
    // the real structure so invariant 6 can catch a stale cache.
    std::uint32_t off = scan_all<highest>(s.words, s.summary);
    auto it = s.overflow.begin();
    while (off != kNone || it != s.overflow.end()) {
        const bool band_first =
            off != kNone && (it == s.overflow.end() ||
                             Compare{}(s.lo + static_cast<Price>(off), it->first));
        if (band_first) {
            const std::uint32_t idx = s.slots[off] - 1;
            fn(idx, level_pool_[idx]);
            off = scan_next<highest>(s.words, s.summary, off);
        } else {
            fn(it->second, level_pool_[it->second]);
            ++it;
        }
    }
}

// --- queries -----------------------------------------------------------------

std::optional<BBO> Book::best_bid() const {
    const auto best = side_best(bids_);
    if (!best.has_value()) {
        return std::nullopt;
    }
    return BBO{best->first, level_pool_[best->second].total_shares};
}

std::optional<BBO> Book::best_ask() const {
    const auto best = side_best(asks_);
    if (!best.has_value()) {
        return std::nullopt;
    }
    return BBO{best->first, level_pool_[best->second].total_shares};
}

std::uint32_t Book::level_index(Side side, Price price) const {
    return side == Side::Bid ? side_find(bids_, price) : side_find(asks_, price);
}

std::int64_t Book::size_at(Side side, Price price) const {
    const std::uint32_t idx = level_index(side, price);
    return idx == kNone ? 0 : level_pool_[idx].total_shares;
}

std::optional<OrderInfo> Book::find_order(OrderId order_id) const {
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return std::nullopt;
    }
    const OrderNode& node = order_pool_[it->second.node];
    const Level& level = level_pool_[it->second.level];
    return OrderInfo{level.side, level.price, node.remaining};
}

std::size_t Book::order_count() const {
    return orders_.size();
}

std::size_t Book::level_count(Side side) const {
    return side == Side::Bid ? bids_.level_count : asks_.level_count;
}

// --- mutation helpers ----------------------------------------------------------

std::uint32_t Book::create_level(Side side, Price price) {
    const std::uint32_t idx = level_pool_.allocate(price, side);
    if (side == Side::Bid) {
        side_insert(bids_, price, idx);
    } else {
        side_insert(asks_, price, idx);
    }
    return idx;
}

void Book::insert_order(Side side, Price price, OrderId order_id, Qty quantity) {
    std::uint32_t level_idx = level_index(side, price);
    if (level_idx == kNone) {
        level_idx = create_level(side, price);
    }
    const std::uint32_t node_idx = order_pool_.allocate(order_id, quantity);
    Level& level = level_pool_[level_idx];   // refs taken AFTER all allocations
    order_pool_[node_idx].prev = level.tail;
    if (level.tail != kNone) {
        order_pool_[level.tail].next = node_idx;
    } else {
        level.head = node_idx;
    }
    level.tail = node_idx;
    level.total_shares += static_cast<std::int64_t>(quantity);
    orders_.emplace(order_id, OrderHandle{node_idx, level_idx});
}

void Book::erase_order(OrderIndexMap::iterator index_it) {
    const OrderHandle h = index_it->second;
    const OrderNode node = order_pool_[h.node];  // copy: the slot is released below
    Level& level = level_pool_[h.level];
    level.total_shares -= static_cast<std::int64_t>(node.remaining);
    if (node.prev != kNone) {
        order_pool_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kNone) {
        order_pool_[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }
    order_pool_.release(h.node);
    if (level.head == kNone) {
        // No empty levels ever: drop the level immediately (O(log levels)).
        if (level.side == Side::Bid) {
            side_erase(bids_, level.price);
        } else {
            side_erase(asks_, level.price);
        }
        level_pool_.release(h.level);
    }
    orders_.erase(index_it);
}

// --- apply ---------------------------------------------------------------------

void apply(Book& book, const Event& event) {
    // Strict error policy (spec section 4): replay of well-formed data is
    // deterministic, so any violation is a bug upstream — fail loud.
    // Every branch validates fully BEFORE mutating anything.
    switch (event.type) {
    case EventType::Add: {
        if (event.quantity == 0) {
            throw BookError(event, "Add: zero quantity");
        }
        if (event.price <= 0) {
            throw BookError(event, "Add: non-positive price");
        }
        if (book.orders_.count(event.order_id) != 0) {
            throw BookError(event, "Add: order_id already exists");
        }
        book.insert_order(event.side, event.price, event.order_id, event.quantity);
        break;
    }
    case EventType::Cancel:
    case EventType::Execute: {
        // Identical book effect (spec section 2); Execute is distinct only for
        // later trade stats. Execution price never affects book state.
        const char* const name = (event.type == EventType::Cancel) ? "Cancel" : "Execute";
        const auto index_it = book.orders_.find(event.order_id);
        if (index_it == book.orders_.end()) {
            throw BookError(event, std::string(name) + ": unknown order_id");
        }
        if (event.quantity == 0) {
            throw BookError(event, std::string(name) + ": zero quantity");
        }
        Book::OrderNode& node = book.order_pool_[index_it->second.node];
        if (event.quantity > node.remaining) {
            throw BookError(event, std::string(name) + ": quantity exceeds remaining shares");
        }
        if (event.quantity == node.remaining) {
            book.erase_order(index_it);  // reaches zero: removed immediately
        } else {
            node.remaining -= event.quantity;
            book.level_pool_[index_it->second.level].total_shares -=
                static_cast<std::int64_t>(event.quantity);
        }
        break;
    }
    case EventType::Delete: {
        // Remove entirely, whatever the remaining quantity; event.quantity and
        // event.price are ignored by definition (spec section 2).
        const auto index_it = book.orders_.find(event.order_id);
        if (index_it == book.orders_.end()) {
            throw BookError(event, "Delete: unknown order_id");
        }
        book.erase_order(index_it);
        break;
    }
    case EventType::Replace: {
        const auto index_it = book.orders_.find(event.order_id);
        if (index_it == book.orders_.end()) {
            throw BookError(event, "Replace: unknown order_id");
        }
        // order_id is live, so the count() check alone also catches
        // new_order_id == order_id; the explicit test documents the spec.
        if (event.new_order_id == event.order_id ||
            book.orders_.count(event.new_order_id) != 0) {
            throw BookError(event, "Replace: new_order_id already exists");
        }
        if (event.quantity == 0) {
            throw BookError(event, "Replace: zero quantity");
        }
        if (event.price <= 0) {
            throw BookError(event, "Replace: non-positive price");
        }
        // Atomic remove + insert on the SAME side (resolved from the replaced
        // order, never from event.side), at the BACK of the (possibly new)
        // level's queue — time priority is lost (ITCH semantics).
        // All validation passed above; neither call below can fail validation
        // (allocation failure aside — post-throw state is unspecified, spec section 4).
        const Side side = book.level_pool_[index_it->second.level].side;
        book.erase_order(index_it);
        book.insert_order(side, event.price, event.new_order_id, event.quantity);
        break;
    }
    default:
        // Fail loud on any value outside the five canonical types (spec
        // section 4 philosophy): it means a decoder/normalizer bug upstream.
        throw BookError(event, "unknown event type");
    }
}

// --- for_each_level --------------------------------------------------------------
// Strict best-first merged walk. Out of band only (checker + test peer).

void Book::for_each_level(
    Side side, const std::function<void(std::uint32_t, const Level&)>& fn) const {
    if (side == Side::Bid) {
        side_for_each(bids_, fn);
    } else {
        side_for_each(asks_, fn);
    }
}

// --- check_invariants ------------------------------------------------------------
// Out of band: walks the whole book; never called from apply(). Invariants 1-5
// from the Milestone 1 spec §6 verbatim, iterated via for_each_level so level
// sortedness is verified against the real walk order, plus invariant 8
// (intrusive links + freelists) from the Milestone 3 spec §4.

std::vector<std::string> check_invariants(const Book& book) {
    std::vector<std::string> violations;

    // Invariant 1: best bid < best ask when both sides are non-empty.
    const auto best_bid = book.best_bid();
    const auto best_ask = book.best_ask();
    if (best_bid.has_value() && best_ask.has_value() && best_bid->price >= best_ask->price) {
        violations.push_back("book crossed or locked: best bid " +
                             std::to_string(best_bid->price) + " >= best ask " +
                             std::to_string(best_ask->price));
    }

    std::size_t orders_in_levels = 0;
    // Liveness maps for invariant 8: what the level walk reaches...
    std::vector<char> node_in_level(book.order_pool_.capacity(), 0);
    std::vector<char> level_in_side(book.level_pool_.capacity(), 0);

    const auto check_side = [&](Side side, const char* name) {
        std::optional<Price> prev_price;
        std::size_t levels_seen = 0;
        book.for_each_level(side, [&](std::uint32_t level_idx, const Book::Level& level) {
            ++levels_seen;
            const Price price = level.price;
            if (level_in_side[level_idx] != 0) {
                violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                     " appears twice in the price structure");
            }
            level_in_side[level_idx] = 1;
            if (level.side != side) {
                violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                     " stored on the wrong side");
            }
            // Invariant 3 (levels): no empty levels.
            if (level.head == Book::kNone) {
                violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                     " is empty");
            }
            // Invariant 4: strictly sorted best-first (descending bids, ascending asks).
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

            // Invariant 8: the intrusive chain is well-formed (reciprocal links,
            // consistent head/tail, no cycles, no node in two levels).
            std::int64_t sum = 0;
            std::uint32_t prev = Book::kNone;
            std::uint32_t cur = level.head;
            std::size_t chain_len = 0;
            while (cur != Book::kNone) {
                if (cur >= book.order_pool_.capacity()) {
                    violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                         " chain index out of range");
                    break;
                }
                if (++chain_len > book.order_pool_.capacity()) {
                    violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                         " intrusive chain does not terminate (cycle)");
                    break;
                }
                const Book::OrderNode& node = book.order_pool_[cur];
                if (node.prev != prev) {
                    violations.push_back("order " + std::to_string(node.id) +
                                         " prev/next links not reciprocal at " + name +
                                         " level " + std::to_string(price));
                }
                if (node_in_level[cur] != 0) {
                    violations.push_back("order node " + std::to_string(cur) +
                                         " linked into more than one level");
                }
                node_in_level[cur] = 1;
                // Invariant 3 (orders): no zero-quantity orders.
                if (node.remaining == 0) {
                    violations.push_back("zero-quantity order " + std::to_string(node.id) +
                                         " at " + std::string(name) + " level " +
                                         std::to_string(price));
                }
                sum += static_cast<std::int64_t>(node.remaining);
                // Invariant 5 (level -> index): every order in a level is indexed,
                // and its handle points at exactly this node in this level.
                const auto idx = book.orders_.find(node.id);
                if (idx == book.orders_.end()) {
                    violations.push_back("order " + std::to_string(node.id) + " in " + name +
                                         " level " + std::to_string(price) +
                                         " missing from index");
                } else if (idx->second.node != cur || idx->second.level != level_idx) {
                    violations.push_back("index entry for order " + std::to_string(node.id) +
                                         " disagrees with its level");
                }
                ++orders_in_levels;
                prev = cur;
                cur = node.next;
            }
            if (level.tail != prev) {
                violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                     " tail does not match the last chain node");
            }
            // Invariant 2: cached aggregate equals the sum of remaining shares.
            if (sum != level.total_shares) {
                violations.push_back(std::string(name) + " level " + std::to_string(price) +
                                     " cached total_shares " +
                                     std::to_string(level.total_shares) +
                                     " != sum of orders " + std::to_string(sum));
            }
        });
        if (levels_seen != book.level_count(side)) {
            violations.push_back(std::string(name) + " level count " +
                                 std::to_string(book.level_count(side)) +
                                 " != levels walked " + std::to_string(levels_seen));
        }
    };

    check_side(Side::Bid, "bid");
    check_side(Side::Ask, "ask");

    // Invariant 5 (index -> levels): same count both ways. Combined with the
    // per-order handle check above this makes the agreement exact.
    if (orders_in_levels != book.orders_.size()) {
        violations.push_back("order index size " + std::to_string(book.orders_.size()) +
                             " != orders present in levels " +
                             std::to_string(orders_in_levels));
    }

    // Invariant 6: bitmaps agree with slots, summary agrees with words, and the
    // cached best agrees with a fresh scan. Invariant 7: the overflow map holds
    // only out-of-band prices; level_count = band levels + overflow levels.
    const auto check_flat = [&](const auto& s, const char* name) {
        std::size_t occupied = 0;
        for (std::size_t off = 0; off < s.slots.size(); ++off) {
            const bool has_level = s.slots[off] != 0;
            const bool bit = ((s.words[off >> 6] >> (off & 63)) & 1) != 0;
            if (has_level != bit) {
                violations.push_back(std::string(name) +
                                     " bitmap disagrees with slots at tick offset " +
                                     std::to_string(off));
            }
            if (has_level) {
                ++occupied;
                const auto& level = book.level_pool_[s.slots[off] - 1];
                if (level.price != s.lo + static_cast<Price>(off)) {
                    violations.push_back(std::string(name) + " band slot at offset " +
                                         std::to_string(off) +
                                         " points at a level with the wrong price");
                }
            }
        }
        for (std::size_t w = 0; w < s.words.size(); ++w) {
            const bool bit = ((s.summary[w >> 6] >> (w & 63)) & 1) != 0;
            if ((s.words[w] != 0) != bit) {
                violations.push_back(std::string(name) +
                                     " summary disagrees with occupancy words at word " +
                                     std::to_string(w));
            }
        }
        constexpr bool highest = std::is_same_v<std::decay_t<decltype(s)>, Book::BidSide>;
        if (s.best != scan_all<highest>(s.words, s.summary)) {
            violations.push_back(std::string(name) + " cached best is stale");
        }
        for (const auto& [price, idx] : s.overflow) {
            (void)idx;
            if (!s.slots.empty() && price >= s.lo &&
                price < s.lo + static_cast<Price>(s.slots.size())) {
                violations.push_back(std::string(name) + " overflow contains in-band price " +
                                     std::to_string(price));
            }
        }
        if (occupied + s.overflow.size() != s.level_count) {
            violations.push_back(std::string(name) + " level_count " +
                                 std::to_string(s.level_count) + " != band levels " +
                                 std::to_string(occupied) + " + overflow levels " +
                                 std::to_string(s.overflow.size()));
        }
    };
    check_flat(book.bids_, "bid");
    check_flat(book.asks_, "ask");

    // Invariant 8 (freelists): free and in-use sets partition each pool —
    // nothing on a freelist is reachable from a level, nothing leaks.
    const auto check_freelist = [&](const char* pool_name, std::size_t capacity,
                                    std::uint32_t free_head, const std::vector<char>& in_use,
                                    const auto& next_of) {
        std::vector<char> seen(capacity, 0);
        std::size_t free_count = 0;
        std::uint32_t cur = free_head;
        while (cur != Book::kNone) {
            if (cur >= capacity || seen[cur] != 0) {
                violations.push_back(std::string(pool_name) +
                                     " freelist is malformed (cycle or bad index)");
                break;
            }
            seen[cur] = 1;
            ++free_count;
            if (in_use[cur] != 0) {
                violations.push_back(std::string(pool_name) + " entry " + std::to_string(cur) +
                                     " is both on the freelist and in a level");
            }
            cur = next_of(cur);
        }
        std::size_t used = 0;
        for (const char c : in_use) used += static_cast<std::size_t>(c);
        // (overlap is reported above; only report a true leak, avoiding underflow)
        if (used + free_count < capacity) {
            violations.push_back(std::string(pool_name) + " leak: " +
                                 std::to_string(capacity - used - free_count) +
                                 " entries neither free nor in use");
        }
    };
    check_freelist("order pool", book.order_pool_.capacity(), book.order_pool_.free_head(),
                   node_in_level,
                   [&](std::uint32_t i) { return book.order_pool_[i].next; });
    check_freelist("level pool", book.level_pool_.capacity(), book.level_pool_.free_head(),
                   level_in_side,
                   [&](std::uint32_t i) { return book.level_pool_[i].head; });

    return violations;
}

// --- is_crossed ------------------------------------------------------------

bool is_crossed(const Book& book) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    return bid.has_value() && ask.has_value() && bid->price >= ask->price;
}

}  // namespace xeas
