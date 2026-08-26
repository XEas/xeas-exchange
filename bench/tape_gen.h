#pragma once

// TapeGen (spec §6): a seeded generator of valid-by-construction event tapes.
// Maintains its own shadow set of live orders (flat vectors, swap-remove) so
// Cancels never over-cancel, Deletes/Replaces always target live ids, and ids
// are unique. Crossing is allowed — the book stores crossed as given. Prices
// random-walk around a mid, with occasional band-edge prices (exercising band
// growth) and rare far-out prices (exercising overflow and overflow-is-best).
// Header-only; shared by book_bench and the fuzz/differential suites. Never
// linked into production targets.

#include "xeas/event.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace xeas {

struct TapeGenOptions {
    std::uint64_t seed = 1;
    Price mid = 1'000'000;             // $100.00
    int far_price_ppm = 100;           // out-of-band outliers, parts per million
    // per-mille event mix (sums to 1000):
    int add = 400, del = 250, execute = 150, cancel = 100, replace = 100;
    // price shape (steady-state defaults; deep/crossed workloads override):
    Price walk_step = 50;              // max per-event random-walk drift of the mid
    Price offset_max = 2'000;          // max distance of a new price from the mid
};

class TapeGen {
public:
    explicit TapeGen(TapeGenOptions opts) : opts_(opts), rng_(opts.seed), mid_(opts.mid) {}

    // Change the mix/price shape mid-tape (bench workload phases). The RNG,
    // live-order set, id counter, and price walk carry over so the tape stays
    // valid across the switch. The seed is fixed at construction.
    void set_options(const TapeGenOptions& opts) {
        const std::uint64_t seed = opts_.seed;
        opts_ = opts;
        opts_.seed = seed;
    }

    Event next() {
        Event e;
        e.timestamp = ++now_;
        int pick = dist(0, 999);
        EventType type = EventType::Add;   // nothing live: everything falls back to Add
        if (!ids_.empty()) {
            if ((pick -= opts_.add) < 0) type = EventType::Add;
            else if ((pick -= opts_.del) < 0) type = EventType::Delete;
            else if ((pick -= opts_.execute) < 0) type = EventType::Execute;
            else if ((pick -= opts_.cancel) < 0) type = EventType::Cancel;
            else type = EventType::Replace;
        }
        switch (type) {
        case EventType::Add: {
            e.type = EventType::Add;
            e.order_id = next_id_++;
            e.side = dist(0, 1) == 0 ? Side::Bid : Side::Ask;
            e.price = gen_price(e.side);
            e.quantity = static_cast<Qty>(dist(1, 1'000));
            ids_.push_back(e.order_id);
            remaining_.push_back(e.quantity);
            break;
        }
        case EventType::Delete: {
            const std::size_t v = victim();
            e.type = EventType::Delete;
            e.order_id = ids_[v];
            remove_live(v);
            break;
        }
        case EventType::Cancel:
        case EventType::Execute: {
            const std::size_t v = victim();
            e.type = type;
            e.order_id = ids_[v];
            e.quantity = static_cast<Qty>(dist(1, static_cast<int>(remaining_[v])));
            if (e.quantity == remaining_[v]) {
                remove_live(v);
            } else {
                remaining_[v] -= e.quantity;
            }
            break;
        }
        case EventType::Replace: {
            const std::size_t v = victim();
            e.type = EventType::Replace;
            e.order_id = ids_[v];
            e.new_order_id = next_id_++;
            // e.side stays defaulted: the book resolves side from the old order.
            e.price = gen_price(dist(0, 1) == 0 ? Side::Bid : Side::Ask);
            e.quantity = static_cast<Qty>(dist(1, 1'000));
            remove_live(v);
            ids_.push_back(e.new_order_id);
            remaining_.push_back(e.quantity);
            break;
        }
        }
        return e;
    }

    const std::vector<OrderId>& live_order_ids() const { return ids_; }

private:
    int dist(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }

    std::size_t victim() {
        return static_cast<std::size_t>(dist(0, static_cast<int>(ids_.size()) - 1));
    }

    void remove_live(std::size_t v) {   // O(1) swap-remove
        ids_[v] = ids_.back();
        ids_.pop_back();
        remaining_[v] = remaining_.back();
        remaining_.pop_back();
    }

    Price gen_price(Side side) {
        if (opts_.far_price_ppm > 0 && dist(0, 999'999) < opts_.far_price_ppm) {
            // Far outliers, guaranteed outside any band window around the
            // opts_.mid anchor neighborhood (kMaxBand is 262,144 ticks).
            constexpr Price kFarSpan = 400'000;
            const Price jitter = dist(0, 1'000);
            if (opts_.mid > kFarSpan + 1'000 && dist(0, 1) == 0) {
                return opts_.mid - kFarSpan - jitter;   // the lone-far-bid case
            }
            return opts_.mid + kFarSpan + jitter;
        }
        mid_ += dist(-static_cast<int>(opts_.walk_step), static_cast<int>(opts_.walk_step));
        if (mid_ < opts_.offset_max + 1) mid_ = opts_.offset_max + 1;  // keep prices positive
        Price offset = dist(0, static_cast<int>(opts_.offset_max));
        if (dist(0, 1'999) == 0) {
            offset = dist(0, 100'000);   // occasional band-edge price: exercise growth
        }
        const Price price = side == Side::Bid ? mid_ - offset : mid_ + offset;
        return std::max<Price>(1, price);
    }

    TapeGenOptions opts_;
    std::mt19937_64 rng_;
    Price mid_;
    Timestamp now_ = 0;
    OrderId next_id_ = 1;
    std::vector<OrderId> ids_;        // live order ids (parallel to remaining_)
    std::vector<Qty> remaining_;
};

}  // namespace xeas
