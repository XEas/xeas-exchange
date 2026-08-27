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

// --- bench workloads (spec §2), shared by book_bench and BenchSmoke -----------

enum class Workload { Steady, Insert, Deep };

struct WorkloadTape {
    std::vector<Event> warm;    // applied untimed (book warm-up)
    std::vector<Event> timed;   // the measured batch
};

inline constexpr std::size_t kSteadyWarmOrders = 100'000;

inline WorkloadTape make_workload_tape(Workload workload, std::uint64_t seed,
                                       std::size_t warmup_events,
                                       std::size_t timed_events) {
    WorkloadTape tape;
    TapeGenOptions mix;   // ITCH-like steady mix (the defaults)
    mix.seed = seed;

    TapeGenOptions adds_only = mix;
    adds_only.add = 1'000;
    adds_only.del = adds_only.execute = adds_only.cancel = adds_only.replace = 0;
    adds_only.far_price_ppm = 0;

    switch (workload) {
    case Workload::Steady: {
        // Warm the book to ~100k live orders, then an ITCH-like mix around a
        // random-walking mid: the real replay hot path — the flagship number.
        TapeGen gen(adds_only);
        tape.warm.reserve(kSteadyWarmOrders + warmup_events);
        for (std::size_t i = 0; i < kSteadyWarmOrders; ++i) tape.warm.push_back(gen.next());
        gen.set_options(mix);
        for (std::size_t i = 0; i < warmup_events; ++i) tape.warm.push_back(gen.next());
        tape.timed.reserve(timed_events);
        for (std::size_t i = 0; i < timed_events; ++i) tape.timed.push_back(gen.next());
        break;
    }
    case Workload::Insert: {
        // Adds spread across many levels, then delete them all: level
        // create/destroy and price-structure insert pressure. No warm phase;
        // warmup_events is ignored — the churn IS the workload.
        TapeGenOptions wide_adds = adds_only;
        wide_adds.offset_max = 50'000;
        TapeGen gen(wide_adds);
        const std::size_t adds = (timed_events + 1) / 2;
        tape.timed.reserve(timed_events);
        for (std::size_t i = 0; i < adds; ++i) tape.timed.push_back(gen.next());
        TapeGenOptions deletes_only = wide_adds;
        deletes_only.add = 0;
        deletes_only.del = 1'000;
        gen.set_options(deletes_only);
        for (std::size_t i = adds; i < timed_events; ++i) tape.timed.push_back(gen.next());
        break;
    }
    case Workload::Deep: {
        // Wide price range with occasional far-out prices: band growth, bitmap
        // next-best scans, the overflow path.
        TapeGenOptions deep = mix;
        deep.walk_step = 500;
        deep.offset_max = 20'000;
        deep.far_price_ppm = 2'000;
        TapeGenOptions deep_adds = adds_only;
        deep_adds.walk_step = deep.walk_step;
        deep_adds.offset_max = deep.offset_max;
        TapeGen gen(deep_adds);
        tape.warm.reserve(kSteadyWarmOrders + warmup_events);
        for (std::size_t i = 0; i < kSteadyWarmOrders; ++i) tape.warm.push_back(gen.next());
        gen.set_options(deep);
        for (std::size_t i = 0; i < warmup_events; ++i) tape.warm.push_back(gen.next());
        tape.timed.reserve(timed_events);
        for (std::size_t i = 0; i < timed_events; ++i) tape.timed.push_back(gen.next());
        break;
    }
    }
    return tape;
}

}  // namespace xeas
