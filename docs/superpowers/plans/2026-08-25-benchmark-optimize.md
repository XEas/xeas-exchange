# Benchmark & Optimize (Milestone 3) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use **superpowers:subagent-driven-development** (recommended) or **superpowers:executing-plans** to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Each task is self-contained: write the failing test, RUN it and observe the stated failure (RED), write the implementation, RUN again and observe pass (GREEN), commit. Do not skip RED steps. Do not deviate from the code given here.

**Goal:** Make the book fast, with proof — a book-only benchmark (`book_bench`), a frozen `BaselineBook` oracle, seeded fuzz/differential suites (`TapeGen`), then three internal optimizations landed as separately measured phases behind the byte-identical public API of `book.h`.

**Architecture:** `bench/` gains a frozen copy of the Milestone 1/2 book (`xeas_baseline`, linked only by bench + fuzz binaries), a header-only tape generator, and the `book_bench` CLI. `Book`'s private section is rewritten three times, each phase ending with all tests green, fuzz differential green, and a `BENCH.md` row: (2) pooled orders + intrusive per-level FIFO lists, (3) banded flat tick-indexed price array + two-level occupancy bitmaps + overflow map, (4) flat open-addressing order index.

**Tech Stack:** C++20, CMake ≥ 3.24, GoogleTest v1.17.0 via FetchContent, ctest. New targets: `xeas_baseline` (static lib), `book_bench` (executable), `book_fuzz_test` (test binary).

**Spec (this plan implements it EXACTLY):** `docs/superpowers/specs/2026-08-24-benchmark-optimize-design.md`
**Project design:** `DESIGN.md`

## Global Constraints (from spec — apply to every task)

- The **public section of `include/xeas/book.h` is byte-identical**: signatures, `const` queries (allocation-free, O(1) or O(log levels)), `BookError` conditions, validation order, post-throw-unspecified rule. Only the private section, includes, and `src/book.cpp` change.
- All 108 existing tests pass with **at most mechanical `BookTestPeer` edits; zero changes to any test that uses only the public API** (one sanctioned exception: `corrupt_handle_price` → `corrupt_handle_level`, spec §4).
- No invariant checks, asserts, or stats inside `apply()` — `check_invariants` stays strictly out-of-band.
- No new dependencies (no Google Benchmark), no threads, no SIMD, no mmap. Timing is never asserted in tests.
- `BaselineBook` is FROZEN — never linked into `xeas_core`, `xeas_feed`, or `itch_replay`; never "improved".
- Layering grep stays clean: `src/book.cpp`, `include/xeas/book.h`, `bench/` include no itch/router/replay headers.
- No optimization lands without its `BENCH.md` before/after row and a green fuzz differential.
- `getrusage` platform code lives only in `main` files (`itch_replay_main.cpp`, `book_bench_main.cpp`), never in libraries.
- Strict TDD; commit after every green task. All commands run from repo root `/Users/glebmokeev/xeas-exchange`.
- C++20, no extensions, `-Wall -Wextra` clean on non-MSVC.

**Design decisions baked in (spec-conformant; ambiguities resolved here):**
1. `TapeGenOptions` gains two price-shape fields beyond the spec sketch (`walk_step`, `offset_max`) — the spec's "configurable" price walk; the deep and crossed-heavy workloads need them. `TapeGen` gains `set_options()` (mix change mid-tape for bench workload phases; seed fixed at construction).
2. `FlatSide::best` stores the **slot offset** of the best in-band level (not the level index) — comparisons become purely positional; the level index is `slots[best] - 1`.
3. Pools expose tiny const accessors (`capacity()`, `free_head()`, `operator[]`) and befriend `BookTestPeer` — needed by `check_invariants` (invariant 8) and corruption tests; the classes are nested in `Book`'s private section, so nothing leaks.
4. `OrderIndex` gains private `home()`/`rehash()` members and initial capacity 1,024; `check_invariants` and `BookTestPeer` are friends (probe self-check + unit tests). Growth trigger `(size_+1)*10 > capacity*7` ≈ max load 0.7.
5. Band anchor = the first Add on that side (trivially "the first in-band Add"). Band grows iff `union(current range, new price)` spans ≤ `kMaxBand`; otherwise the price goes to overflow. Growth migrates any overflow entries the wider band now covers (invariant 7).
6. `for_each_level` takes `std::function` (out-of-band only — checker + test peer), avoiding template-linkage traps. Bitmap scan helpers are file-local in `book.cpp`.
7. `book_bench` pre-generates the tape (spec: "generate a tape, apply it in a timed batch loop"), so its peak RSS includes the tape (~48 B/event, identical across impls) — documented in `BENCH.md`; per-impl RSS comparisons use separate `--impl new` / `--impl baseline` runs; absolute footprint comes from the macro table. Median = element at index `R/2` of the sorted repeats.
8. Test suites exceed the spec's ~28 sketch (TapeGen unit tests, OrderIndex unit tests, baseline smoke — ~40 new tests total). `mt19937_64` is seeded and deterministic per platform; `uniform_int_distribution` sequences are implementation-defined, so recorded numbers are same-machine reproducible (all the spec requires).
9. Two `BookTestPeer` definitions exist — one in `tests/book_test.cpp`, one in `tests/book_fuzz_test.cpp`. They live in different binaries, so no ODR issue.

**Test execution conventions:**
- Configure: `cmake -S . -B build` — Build: `cmake --build build`
- Full suite: `ctest --test-dir build --output-on-failure`
- One suite: `./build/book_fuzz_test --gtest_filter='Pool.*'`
- Bench (numbers of record): `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release && ./build-release/book_bench ...`

**Before Task 1:**

```bash
gh pr merge 5 --merge          # doc-only spec PR; then:
git checkout main && git pull
git checkout -b feature/benchmark-optimize
git add docs/superpowers/plans/2026-08-25-benchmark-optimize.md
git commit -m "docs: add benchmark & optimize implementation plan (Milestone 3)"
git push -u origin feature/benchmark-optimize
```

---

### Task 1: `BaselineBook` — frozen oracle + `xeas_baseline` + smoke tests

**Files:**
- Create: `bench/baseline_book.h`, `bench/baseline_book.cpp` (mechanical transform of frozen sources)
- Create: `tests/book_fuzz_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces produced:** `class xeas::BaselineBook` with the exact public queries of `Book`, plus free `apply(BaselineBook&, const Event&)`, `check_invariants(const BaselineBook&)`, `is_crossed(const BaselineBook&)`.

- [ ] **Step 1: Write the failing tests** — create `tests/book_fuzz_test.cpp`:

```cpp
#include "baseline_book.h"
#include "xeas/book.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace xeas;

// --- event factories (mirrors tests/book_test.cpp) --------------------------

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

Event make_cancel(OrderId id, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Cancel;
    e.timestamp = ts;
    e.order_id = id;
    e.quantity = qty;
    return e;
}

Event make_delete(OrderId id, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Delete;
    e.timestamp = ts;
    e.order_id = id;
    return e;
}

bool any_contains(const std::vector<std::string>& violations, std::string_view needle) {
    for (const auto& v : violations) {
        if (v.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// BaselineSmoke: the frozen oracle behaves like the Milestone 1/2 book
// ---------------------------------------------------------------------------

TEST(BaselineSmoke, AddQueryDeleteLifecycle) {
    BaselineBook book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Ask, 1'010'000, 50));
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(book.best_bid()->price, 1'000'000);
    EXPECT_EQ(book.best_ask()->price, 1'010'000);
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 100);
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(book.level_count(Side::Ask), 1u);
    ASSERT_TRUE(book.find_order(1).has_value());
    EXPECT_EQ(book.find_order(1)->remaining_qty, 100u);
    EXPECT_TRUE(check_invariants(book).empty());
    apply(book, make_delete(1));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BaselineSmoke, ThrowsBookErrorOnMalformedEvents) {
    BaselineBook book;
    EXPECT_THROW(apply(book, make_cancel(999, 10)), BookError);
    EXPECT_THROW(apply(book, make_add(1, Side::Bid, 0, 100)), BookError);
}

TEST(BaselineSmoke, CrossedBookStoredAsGivenAndFlagged) {
    BaselineBook book;
    apply(book, make_add(1, Side::Bid, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));
    EXPECT_TRUE(is_crossed(book));
    EXPECT_TRUE(any_contains(check_invariants(book), "crossed"));
}

}  // namespace
```

- [ ] **Step 2: Wire CMake** — in `CMakeLists.txt`, after the `itch_replay` block insert:

```cmake
# --- baseline oracle (Milestone 3) --------------------------------------------
# FROZEN copy of the Milestone 1/2 book internals: reference oracle for the
# bench and the fuzz differential. Linked ONLY by book_bench and book_fuzz_test
# — never into xeas_core, xeas_feed, or itch_replay.
add_library(xeas_baseline bench/baseline_book.cpp)
target_include_directories(xeas_baseline PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/bench)
target_compile_features(xeas_baseline PUBLIC cxx_std_20)
if(NOT MSVC)
  target_compile_options(xeas_baseline PRIVATE -Wall -Wextra)
endif()
```

and after the `replay_test` block:

```cmake
add_executable(book_fuzz_test tests/book_fuzz_test.cpp)
target_link_libraries(book_fuzz_test PRIVATE xeas_core xeas_baseline GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(book_fuzz_test PRIVATE -Wall -Wextra)
endif()
```

and add `gtest_discover_tests(book_fuzz_test)` next to the other discover calls.

- [ ] **Step 3: RED** — Run: `cmake -S . -B build && cmake --build build`
Expected: FAIL — `bench/baseline_book.cpp` / `baseline_book.h` do not exist.

- [ ] **Step 4: Create `bench/baseline_book.h`** (exact content):

```cpp
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
```

- [ ] **Step 5: Create `bench/baseline_book.cpp` by mechanical transform** of the CURRENT (still-Milestone-1) `src/book.cpp` — this must happen BEFORE Task 6 rewrites it:

```bash
perl -pe 's/\bBook\b/BaselineBook/g; s{xeas/book\.h}{baseline_book.h}' \
    src/book.cpp > bench/baseline_book.cpp
```

Then prepend this line at the very top of `bench/baseline_book.cpp`:

```cpp
// FROZEN copy of Milestone 1/2 internals — reference oracle, do not improve.
```

Verify the transform: `grep -n 'BaselineBookError\|\bBook\b' bench/baseline_book.cpp` must print nothing (word-boundary rename leaves `BookError`, `BBO`, `OrderInfo` untouched; every bare `Book` became `BaselineBook`).

- [ ] **Step 6: GREEN** — Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all 108 existing tests + 3 `BaselineSmoke.*` pass.

- [ ] **Step 7: Commit**

```bash
git add bench/baseline_book.h bench/baseline_book.cpp tests/book_fuzz_test.cpp CMakeLists.txt
git commit -m "feat: BaselineBook - frozen Milestone 1/2 oracle as xeas_baseline"
```

### Task 2: `TapeGen` — seeded, valid-by-construction event tapes

**Files:**
- Create: `bench/tape_gen.h` (header-only)
- Modify: `tests/book_fuzz_test.cpp` (add TapeGen suite)

**Interfaces produced:** `xeas::TapeGenOptions{seed, mid, far_price_ppm, add, del, execute, cancel, replace, walk_step, offset_max}`, `xeas::TapeGen{TapeGen(TapeGenOptions), set_options(const TapeGenOptions&), Event next(), const std::vector<OrderId>& live_order_ids() const}`.

- [ ] **Step 1: Write the failing tests** — append to `tests/book_fuzz_test.cpp` (inside the anonymous namespace, after `BaselineSmoke`), and add `#include "tape_gen.h"` under the `baseline_book.h` include:

```cpp
// ---------------------------------------------------------------------------
// TapeGen: determinism + validity of the generator itself
// ---------------------------------------------------------------------------

TEST(TapeGen, SameSeedProducesIdenticalTapes) {
    TapeGenOptions opts;
    opts.seed = 42;
    TapeGen a(opts);
    TapeGen b(opts);
    for (int i = 0; i < 10'000; ++i) {
        const Event ea = a.next();
        const Event eb = b.next();
        ASSERT_EQ(ea.type, eb.type) << "event " << i;
        ASSERT_EQ(ea.order_id, eb.order_id) << "event " << i;
        ASSERT_EQ(ea.side, eb.side) << "event " << i;
        ASSERT_EQ(ea.price, eb.price) << "event " << i;
        ASSERT_EQ(ea.quantity, eb.quantity) << "event " << i;
        ASSERT_EQ(ea.new_order_id, eb.new_order_id) << "event " << i;
    }
}

TEST(TapeGen, ValidByConstructionAgainstBaseline) {
    TapeGenOptions opts;
    opts.seed = 1;
    TapeGen gen(opts);
    BaselineBook book;
    for (int i = 0; i < 50'000; ++i) {
        ASSERT_NO_THROW(apply(book, gen.next())) << "event " << i;
    }
    EXPECT_EQ(book.order_count(), gen.live_order_ids().size());
}

TEST(TapeGen, FallsBackToAddWhenNothingIsLive) {
    TapeGenOptions opts;
    opts.seed = 3;
    opts.add = 0;
    opts.del = 1'000;
    opts.execute = opts.cancel = opts.replace = 0;
    TapeGen gen(opts);
    BaselineBook book;
    const Event first = gen.next();
    EXPECT_EQ(first.type, EventType::Add);  // nothing live: must fall back
    apply(book, first);
    for (int i = 0; i < 200; ++i) {
        apply(book, gen.next());
        ASSERT_LE(book.order_count(), 1u);  // add/delete strictly alternate
    }
}

TEST(TapeGen, FarPricesAppearAtConfiguredRate) {
    TapeGenOptions opts;
    opts.seed = 5;
    opts.far_price_ppm = 100'000;  // 10%: outliers must show up fast
    TapeGen gen(opts);
    int far_seen = 0;
    for (int i = 0; i < 2'000; ++i) {
        const Event e = gen.next();
        if ((e.type == EventType::Add || e.type == EventType::Replace) &&
            (e.price >= opts.mid + 400'000 || e.price <= opts.mid - 400'000)) {
            ++far_seen;
        }
    }
    EXPECT_GT(far_seen, 0);
}
```

- [ ] **Step 2: RED** — Run: `cmake --build build`
Expected: FAIL — `tape_gen.h` not found.

- [ ] **Step 3: Create `bench/tape_gen.h`** (exact content):

```cpp
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
```

- [ ] **Step 4: GREEN** — Run: `cmake --build build && ./build/book_fuzz_test --gtest_filter='TapeGen.*'`
Expected: 4 tests pass. Then full `ctest --test-dir build --output-on-failure` — everything green.

- [ ] **Step 5: Commit**

```bash
git add bench/tape_gen.h tests/book_fuzz_test.cpp
git commit -m "feat: TapeGen - seeded valid-by-construction event tape generator"
```

### Task 3: Differential harness + `FuzzDifferential` suite

With `Book` still on Milestone 1 internals, `Book`-vs-`BaselineBook` runs baseline-vs-baseline in effect — this task validates the harness itself (spec §7 phase 1).

**Files:**
- Modify: `tests/book_fuzz_test.cpp` (harness helpers + 7 tests)

**Interfaces produced (test-internal, reused by every later phase):** `books_agree(book, oracle, prices, ids) -> AssertionResult`, `crossed_only(violations) -> AssertionResult`, `run_differential<NewBook, OracleBook>(opts, events, check_every)`.

- [ ] **Step 1: Write the harness + tests** — append inside the anonymous namespace:

```cpp
// ---------------------------------------------------------------------------
// Differential protocol (spec §6): apply each event to both books; compare
// queries every K events and exhaustively at the end; require the new book's
// invariants to be empty or crossed-only.
// ---------------------------------------------------------------------------

template <class B>
std::optional<BBO> best_of(const B& book, Side side) {
    return side == Side::Bid ? book.best_bid() : book.best_ask();
}

template <class NewBook, class OracleBook>
testing::AssertionResult books_agree(const NewBook& book, const OracleBook& oracle,
                                     const std::vector<Price>& prices,
                                     const std::vector<OrderId>& ids) {
    for (const Side side : {Side::Bid, Side::Ask}) {
        const char* name = side == Side::Bid ? "bid" : "ask";
        const auto a = best_of(book, side);
        const auto b = best_of(oracle, side);
        if (a.has_value() != b.has_value()) {
            return testing::AssertionFailure() << "best " << name << " presence differs";
        }
        if (a.has_value() && (a->price != b->price || a->total_shares != b->total_shares)) {
            return testing::AssertionFailure()
                   << "best " << name << " differs: " << a->price << "/" << a->total_shares
                   << " vs " << b->price << "/" << b->total_shares;
        }
        if (book.level_count(side) != oracle.level_count(side)) {
            return testing::AssertionFailure()
                   << name << " level_count differs: " << book.level_count(side) << " vs "
                   << oracle.level_count(side);
        }
        for (const Price price : prices) {
            if (book.size_at(side, price) != oracle.size_at(side, price)) {
                return testing::AssertionFailure()
                       << name << " size_at " << price << " differs: "
                       << book.size_at(side, price) << " vs " << oracle.size_at(side, price);
            }
        }
    }
    if (book.order_count() != oracle.order_count()) {
        return testing::AssertionFailure() << "order_count differs: " << book.order_count()
                                           << " vs " << oracle.order_count();
    }
    for (const OrderId id : ids) {
        const auto a = book.find_order(id);
        const auto b = oracle.find_order(id);
        if (a.has_value() != b.has_value()) {
            return testing::AssertionFailure() << "find_order presence differs for id " << id;
        }
        if (a.has_value() && (a->side != b->side || a->price != b->price ||
                              a->remaining_qty != b->remaining_qty)) {
            return testing::AssertionFailure() << "find_order differs for id " << id;
        }
    }
    return testing::AssertionSuccess();
}

testing::AssertionResult crossed_only(const std::vector<std::string>& violations) {
    for (const auto& v : violations) {
        if (v.find("crossed") == std::string::npos) {
            return testing::AssertionFailure() << "non-crossed invariant violation: " << v;
        }
    }
    return testing::AssertionSuccess();
}

template <class NewBook, class OracleBook>
void run_differential(const TapeGenOptions& opts, std::size_t events, std::size_t check_every) {
    NewBook book;
    OracleBook oracle;
    TapeGen gen(opts);
    std::vector<Price> touched;       // every Add/Replace price, for the final sweep
    std::array<Price, 16> recent{};   // ring of recently touched prices
    std::size_t recent_n = 0;
    for (std::size_t i = 0; i < events; ++i) {
        const Event e = gen.next();
        apply(book, e);
        apply(oracle, e);
        if (e.type == EventType::Add || e.type == EventType::Replace) {
            touched.push_back(e.price);
            recent[recent_n++ % recent.size()] = e.price;
        }
        if ((i + 1) % check_every == 0) {
            const std::vector<Price> prices(
                recent.begin(),
                recent.begin() + static_cast<std::ptrdiff_t>(std::min(recent_n, recent.size())));
            std::vector<OrderId> sample;
            const auto& live = gen.live_order_ids();
            for (std::size_t k = 0; k < live.size(); k += 97) sample.push_back(live[k]);
            ASSERT_TRUE(books_agree(book, oracle, prices, sample)) << "after event " << i + 1;
            ASSERT_TRUE(crossed_only(check_invariants(book))) << "after event " << i + 1;
        }
    }
    // Final sweeps: every live order, every touched price.
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    ASSERT_TRUE(books_agree(book, oracle, touched, gen.live_order_ids())) << "final sweep";
    ASSERT_TRUE(crossed_only(check_invariants(book))) << "final sweep";
}

// ---------------------------------------------------------------------------
// FuzzDifferential: Book vs the frozen BaselineBook oracle
// ---------------------------------------------------------------------------

TEST(FuzzDifferential, DefaultMixAgainstBaseline) {
    TapeGenOptions opts;
    opts.seed = 1;
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, ReplaceHeavyMix) {
    TapeGenOptions opts;
    opts.seed = 2;
    opts.add = 300; opts.del = 100; opts.execute = 100; opts.cancel = 100; opts.replace = 400;
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, FarPriceHeavyMix) {
    TapeGenOptions opts;
    opts.seed = 3;
    opts.far_price_ppm = 20'000;   // 2% far-out prices: overflow and overflow-is-best
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, HighChurnDrainsToEmpty) {
    TapeGenOptions opts;
    opts.seed = 4;
    opts.add = 250; opts.del = 450; opts.execute = 200; opts.cancel = 50; opts.replace = 50;
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, CrossedHeavyMix) {
    TapeGenOptions opts;
    opts.seed = 5;
    opts.offset_max = 2;   // both sides hug the mid: constant locking/crossing
    opts.walk_step = 5;
    run_differential<Book, BaselineBook>(opts, 100'000, 4'096);
}

TEST(FuzzDifferential, MultiSeedSweep) {
    for (std::uint64_t seed = 2; seed <= 11; ++seed) {
        TapeGenOptions opts;
        opts.seed = seed;
        run_differential<Book, BaselineBook>(opts, 20'000, 4'096);
        if (::testing::Test::HasFatalFailure()) FAIL() << "seed " << seed;
    }
}

TEST(FuzzDifferential, BaselineAgreesWithItself) {
    // Harness self-validation: the oracle differentially compared to itself.
    TapeGenOptions opts;
    opts.seed = 1;
    run_differential<BaselineBook, BaselineBook>(opts, 50'000, 4'096);
}
```

- [ ] **Step 2: GREEN** — Run: `cmake --build build && ./build/book_fuzz_test --gtest_filter='FuzzDifferential.*'`
Expected: 7 tests pass (Book still has baseline internals — this proves the harness, not the optimizations). Then full ctest green.

- [ ] **Step 3: Commit**

```bash
git add tests/book_fuzz_test.cpp
git commit -m "test: differential fuzz harness - Book vs frozen BaselineBook oracle"
```

### Task 4: Bench workloads + `book_bench` CLI + `BenchSmoke`

**Files:**
- Modify: `bench/tape_gen.h` (append workload section)
- Create: `bench/book_bench_main.cpp`
- Modify: `tests/book_fuzz_test.cpp` (BenchSmoke suite), `CMakeLists.txt` (book_bench target)

**Interfaces produced:** `enum class xeas::Workload { Steady, Insert, Deep }`, `struct xeas::WorkloadTape { std::vector<Event> warm, timed; }`, `xeas::make_workload_tape(Workload, seed, warmup_events, timed_events) -> WorkloadTape`, `kSteadyWarmOrders = 100'000`. Executable `book_bench`.

- [ ] **Step 1: Write the failing tests** — append to `tests/book_fuzz_test.cpp`:

```cpp
// ---------------------------------------------------------------------------
// BenchSmoke: each bench workload runs untimed and leaves a consistent book
// ---------------------------------------------------------------------------

TEST(BenchSmoke, WorkloadsPassInvariantsUntimed) {
    for (const Workload w : {Workload::Steady, Workload::Insert, Workload::Deep}) {
        const WorkloadTape tape = make_workload_tape(w, 1, 1'000, 10'000);
        Book book;
        for (const Event& e : tape.warm) apply(book, e);
        for (const Event& e : tape.timed) apply(book, e);
        EXPECT_TRUE(crossed_only(check_invariants(book)))
            << "workload " << static_cast<int>(w);
        EXPECT_GT(book.order_count() + tape.timed.size(), 10'000u);
    }
}

TEST(BenchSmoke, InsertWorkloadDrainsTheBook) {
    const WorkloadTape tape = make_workload_tape(Workload::Insert, 1, 0, 10'000);
    Book book;
    for (const Event& e : tape.timed) apply(book, e);
    EXPECT_EQ(book.order_count(), 0u);   // adds then deletes: exactly drains
}
```

- [ ] **Step 2: RED** — Run: `cmake --build build`
Expected: FAIL — `Workload` / `make_workload_tape` undeclared.

- [ ] **Step 3: Append the workload section to `bench/tape_gen.h`** (before the closing `}  // namespace xeas`):

```cpp
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
```

- [ ] **Step 4: GREEN (smoke)** — Run: `cmake --build build && ./build/book_fuzz_test --gtest_filter='BenchSmoke.*'`
Expected: 2 tests pass.

- [ ] **Step 5: Create `bench/book_bench_main.cpp`** (exact content):

```cpp
// book_bench [--workload steady|insert|deep|all] [--events N] [--warmup N]
//            [--seed S] [--repeat R] [--impl new|baseline|both]
//
// Book-only microbenchmark (spec §2): deterministic TapeGen workloads applied
// in a timed steady_clock batch loop; reports the median and min of Mmsg/s
// over R repeats, ns/event, and peak RSS. Links xeas_core and xeas_baseline
// only — no feed, no I/O. Numbers of record come from a Release build; see
// BENCH.md for methodology (RSS here includes the pre-generated tape, which is
// identical across impls — compare impls with separate --impl runs).

#include "baseline_book.h"
#include "tape_gen.h"
#include "xeas/book.h"

#include <sys/resource.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace xeas;

int usage() {
    std::cerr
        << "usage: book_bench [--workload steady|insert|deep|all] [--events N]\n"
           "                  [--warmup N] [--seed S] [--repeat R] [--impl new|baseline|both]\n"
           "  --workload  synthetic workload(s) to run (default all)\n"
           "  --events    timed events per run (default 10000000)\n"
           "  --warmup    untimed mix events before timing, steady/deep only (default 1000000)\n"
           "  --seed      TapeGen seed, printed with every line (default 1)\n"
           "  --repeat    repeats per (workload, impl); median and min reported (default 5)\n"
           "  --impl      which book implementation(s) to run (default both)\n";
    return 1;
}

bool parse_count(const char* s, std::uint64_t& out) {
    if (s == nullptr || s[0] == '\0') return false;
    if (s[0] == '-' || s[0] == '+') return false;
    char* end;
    errno = 0;
    std::uint64_t val = std::strtoull(s, &end, 10);
    if (errno == ERANGE) return false;
    if (*end != '\0') return false;
    out = val;
    return true;
}

// ru_maxrss is bytes on macOS and KiB on Linux — normalized to bytes here.
std::uint64_t peak_rss_bytes() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
}

template <class B>
double run_once_seconds(const WorkloadTape& tape) {
    B book;
    for (const Event& e : tape.warm) apply(book, e);
    const auto t0 = std::chrono::steady_clock::now();
    for (const Event& e : tape.timed) apply(book, e);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

struct ImplResult {
    double median_mmsgs = 0.0;
    double min_mmsgs = 0.0;
};

template <class B>
ImplResult run_impl(const WorkloadTape& tape, std::uint64_t repeats) {
    std::vector<double> mmsgs;
    for (std::uint64_t r = 0; r < repeats; ++r) {
        const double seconds = run_once_seconds<B>(tape);
        mmsgs.push_back(static_cast<double>(tape.timed.size()) / seconds / 1e6);
    }
    std::sort(mmsgs.begin(), mmsgs.end());
    return ImplResult{mmsgs[mmsgs.size() / 2], mmsgs.front()};
}

void report(std::string_view workload, std::string_view impl, const WorkloadTape& tape,
            std::uint64_t seed, std::uint64_t repeats, const ImplResult& r) {
    std::cout << "workload=" << workload << " impl=" << impl
              << " events=" << tape.timed.size() << " warm=" << tape.warm.size()
              << " seed=" << seed << " repeats=" << repeats << std::fixed
              << std::setprecision(2) << " median=" << r.median_mmsgs
              << " Mmsg/s min=" << r.min_mmsgs << " Mmsg/s ns_per_event="
              << std::setprecision(1) << 1000.0 / r.median_mmsgs
              << " peak_rss_mib=" << peak_rss_bytes() / (1024 * 1024) << "\n"
              << std::defaultfloat;
}

}  // namespace

int main(int argc, char** argv) {
    const std::pair<std::string_view, Workload> all[] = {{"steady", Workload::Steady},
                                                         {"insert", Workload::Insert},
                                                         {"deep", Workload::Deep}};
    std::string_view workload_arg = "all";
    std::string_view impl = "both";
    std::uint64_t events = 10'000'000;
    std::uint64_t warmup = 1'000'000;
    std::uint64_t seed = 1;
    std::uint64_t repeats = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--workload" && i + 1 < argc) {
            workload_arg = argv[++i];
        } else if (arg == "--impl" && i + 1 < argc) {
            impl = argv[++i];
        } else if (arg == "--events" && i + 1 < argc) {
            if (!parse_count(argv[++i], events)) return usage();
        } else if (arg == "--warmup" && i + 1 < argc) {
            if (!parse_count(argv[++i], warmup)) return usage();
        } else if (arg == "--seed" && i + 1 < argc) {
            if (!parse_count(argv[++i], seed)) return usage();
        } else if (arg == "--repeat" && i + 1 < argc) {
            if (!parse_count(argv[++i], repeats)) return usage();
        } else {
            return usage();
        }
    }
    if (impl != "new" && impl != "baseline" && impl != "both") return usage();
    if (events == 0 || repeats == 0) return usage();
    bool matched = false;
    for (const auto& [name, workload] : all) {
        if (workload_arg != "all" && workload_arg != name) continue;
        matched = true;
        const WorkloadTape tape = make_workload_tape(workload, seed, warmup, events);
        ImplResult baseline_result;
        if (impl != "new") {
            baseline_result = run_impl<BaselineBook>(tape, repeats);
            report(name, "baseline", tape, seed, repeats, baseline_result);
        }
        if (impl != "baseline") {
            const ImplResult new_result = run_impl<Book>(tape, repeats);
            report(name, "new", tape, seed, repeats, new_result);
            if (impl == "both") {
                std::cout << "workload=" << name << " ratio new/baseline=" << std::fixed
                          << std::setprecision(2)
                          << new_result.median_mmsgs / baseline_result.median_mmsgs << "x\n"
                          << std::defaultfloat;
            }
        }
    }
    if (!matched) return usage();
    return 0;
}
```

- [ ] **Step 6: Wire CMake** — after the `xeas_baseline` block in `CMakeLists.txt`:

```cmake
# --- book_bench (Milestone 3) --------------------------------------------------
# Book-only: links xeas_core + xeas_baseline, NOT xeas_feed — cashing the
# Milestone 2 promise that "benchmarks can link book-only".
add_executable(book_bench bench/book_bench_main.cpp)
target_link_libraries(book_bench PRIVATE xeas_core xeas_baseline)
if(NOT MSVC)
  target_compile_options(book_bench PRIVATE -Wall -Wextra)
endif()
```

- [ ] **Step 7: GREEN** — Run:

```bash
cmake -S . -B build && cmake --build build
./build/book_bench --events 100000 --warmup 10000 --repeat 2
./build/book_bench --workload bogus 2>&1 | head -2   # expect usage, exit 1
ctest --test-dir build --output-on-failure
```

Expected: six result lines + three ratio lines (ratio ≈ 1.0x — identical internals), usage on bad args, full suite green.

- [ ] **Step 8: Commit**

```bash
git add bench/tape_gen.h bench/book_bench_main.cpp tests/book_fuzz_test.cpp CMakeLists.txt
git commit -m "feat: book_bench - deterministic book-only microbenchmark CLI"
```

---

### Task 5: Peak-RSS line in `itch_replay` + `BENCH.md` + phase-1 baseline numbers

**Files:**
- Modify: `src/itch_replay_main.cpp` (additive only)
- Create: `BENCH.md`

- [ ] **Step 1: Add the peak-RSS line** to `src/itch_replay_main.cpp`. Add `#include <sys/resource.h>` after the xeas includes; add this helper inside the anonymous namespace (before `print_stats`):

```cpp
// ru_maxrss is bytes on macOS and KiB on Linux — normalized to bytes here.
// Platform code stays out of the libraries (spec §2): this lives in main only.
std::uint64_t peak_rss_bytes() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
}
```

and at the end of `print_stats` (after the throughput block) append:

```cpp
    out << "  peak RSS            " << (peak_rss_bytes() / (1024 * 1024)) << " MiB\n";
```

- [ ] **Step 2: Verify** — Run: `cmake --build build && ./build/itch_replay 2>&1 | head -3` (usage still prints; no file needed). Build is `-Wall -Wextra` clean. All tests still green (`ctest --test-dir build`).

- [ ] **Step 3: Create `BENCH.md`** at the repo root:

```markdown
# BENCH — Milestone 3 numbers & methodology

Every number here is reproducible from its own row: fixed seed, event count,
and the commands below.

## Rules

- **Release build** (must be stated — the project's default build type is empty):
  `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release`
- Median of 5 repeats (min also recorded), single-threaded, quiet machine;
  note CPU frequency scaling / power state next to the machine line.
- `book_bench` peak RSS includes the pre-generated event tape (~48 B/event),
  identical across impls: compare impls with separate `--impl new` /
  `--impl baseline` runs; absolute footprint lives in the macro table.
- Macro runs use a real uncompressed `*.NASDAQ_ITCH50` day (never checked in),
  same file and machine before/after each phase.

## Machine

- (filled at first recording) CPU, RAM, OS, compiler `c++ --version`, frequency-scaling note.

## Microbenchmark: `book_bench`

Command of record, per phase:
`./build-release/book_bench --workload all --events 10000000 --warmup 1000000 --seed 1 --repeat 5`
plus per-impl RSS runs with `--impl new` and `--impl baseline`.

| phase | workload | impl | Mmsg/s median | Mmsg/s min | ns/event | speedup vs baseline | peak RSS MiB |
|---|---|---|---|---|---|---|---|
| 1 (harness, baseline internals) | steady | baseline | _pending_ | | | 1.00x | |

## Macrobenchmark: full-day `itch_replay`

Command: `./build-release/itch_replay <day.NASDAQ_ITCH50> --quiet`

| phase | file | msgs/s | MiB/s | peak RSS MiB | live orders peak |
|---|---|---|---|---|---|
| 1 | _pending: requires local day file_ | | | | |

**Book-vs-feed split (spec §2):** compare `book_bench steady` Mmsg/s with
end-to-end `itch_replay` msgs/s. Close ⇒ the book dominates and feed-side
optimization stays out of scope; feed-dominant ⇒ a finding for a future
milestone, not license to widen this one.

## Band constants (phase 3 tuning)

`kInitialBandTicks = 4096`, `kMaxBandTicks = 262144` — provisional, named in
`src/book.cpp`. Finalized from a real-day per-symbol price-range histogram
(share of a symbol's quoted range covered by a 262,144-tick window around its
first quote); recorded here when measured.
```

- [ ] **Step 4: Record phase-1 numbers** — Run:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
./build-release/book_bench --workload all --events 10000000 --warmup 1000000 --seed 1 --repeat 5
./build-release/book_bench --workload steady --impl baseline --events 10000000 --warmup 1000000 --seed 1 --repeat 5
./build-release/book_bench --workload steady --impl new --events 10000000 --warmup 1000000 --seed 1 --repeat 5
```

Fill the machine header and replace the micro table's pending row with actual phase-1 rows (3 workloads × 2 impls; ratios ≈ 1.0x). **Manual (skip if no local day file):** run the macro command on a real day and fill the phase-1 macro row.

- [ ] **Step 5: Commit**

```bash
git add src/itch_replay_main.cpp BENCH.md
git commit -m "feat: itch_replay peak-RSS line; BENCH.md with phase-1 baseline numbers"
```

### Task 6: Phase 2 — order pool + intrusive per-level FIFO lists

Level lookup stays `std::map` (now `Price -> u32` level index) and the order index stays `std::unordered_map` (handle shrinks to 8 bytes). Orders move into a pooled slab addressed by u32 indices. `apply()` keeps its exact shape: same validation order, same `BookError` messages, all validation before any mutation.

The regression suite IS this task's test: 108 existing tests (with mechanical peer edits) + the full fuzz differential.

**Files:**
- Modify: `include/xeas/book.h` (private section + includes ONLY — public section byte-identical)
- Modify: `src/book.cpp` (full rewrite, given below)
- Modify: `tests/book_test.cpp` (BookTestPeer bodies + the one sanctioned test edit)

**Interfaces produced (used by Tasks 7, 9, 11):** `Book::kNone`, `Book::OrderNode{id, remaining, prev, next}`, `Book::Level{head, tail, total_shares, price, side}`, `Book::OrderPool` / `Book::LevelPool` (`allocate`, `release`, `operator[]`, `capacity()`, `free_head()`), `Book::OrderHandle{node, level}`, private `level_index(Side, Price) -> u32`, `create_level(Side, Price) -> u32`, `for_each_level(Side, const std::function<void(std::uint32_t, const Level&)>&) const`.

- [ ] **Step 1: Replace the private section of `include/xeas/book.h`.** Remove `#include <list>` from the includes (everything else stays). Replace everything from `private:` to the closing `};` of `class Book` with:

```cpp
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
```

- [ ] **Step 2: Rewrite `src/book.cpp`** — full replacement content:

```cpp
#include "xeas/book.h"

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

// --- queries -----------------------------------------------------------------

std::optional<BBO> Book::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    const auto& [price, idx] = *bids_.begin();
    return BBO{price, level_pool_[idx].total_shares};
}

std::optional<BBO> Book::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    const auto& [price, idx] = *asks_.begin();
    return BBO{price, level_pool_[idx].total_shares};
}

std::uint32_t Book::level_index(Side side, Price price) const {
    if (side == Side::Bid) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? kNone : it->second;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? kNone : it->second;
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
    return side == Side::Bid ? bids_.size() : asks_.size();
}

// --- mutation helpers ----------------------------------------------------------

std::uint32_t Book::create_level(Side side, Price price) {
    const std::uint32_t idx = level_pool_.allocate(price, side);
    if (side == Side::Bid) {
        bids_.emplace(price, idx);
    } else {
        asks_.emplace(price, idx);
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
            bids_.erase(level.price);
        } else {
            asks_.erase(level.price);
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
        OrderNode& node = book.order_pool_[index_it->second.node];
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
        for (const auto& entry : bids_) fn(entry.second, level_pool_[entry.second]);
    } else {
        for (const auto& entry : asks_) fn(entry.second, level_pool_[entry.second]);
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
```

- [ ] **Step 3: Rewrite `BookTestPeer` in `tests/book_test.cpp`** (bodies only — same test intents). Replace the whole `struct BookTestPeer { ... };` with:

```cpp
struct BookTestPeer {
    static std::vector<OrderId> level_order_ids(const Book& b, Side side, Price price) {
        std::vector<OrderId> ids;
        const std::uint32_t idx = b.level_index(side, price);
        if (idx == Book::kNone) return ids;
        for (std::uint32_t cur = b.level_pool_[idx].head; cur != Book::kNone;
             cur = b.order_pool_[cur].next) {
            ids.push_back(b.order_pool_[cur].id);
        }
        return ids;
    }

    // --- corruption helpers (invariant-checker tests only) ---
    static void corrupt_total_shares(Book& b, Side side, Price price, std::int64_t v) {
        b.level_pool_[b.level_index(side, price)].total_shares = v;
    }

    static void add_empty_level(Book& b, Side side, Price price) {
        b.create_level(side, price);
    }

    static void zero_order_qty(Book& b, OrderId id) {
        b.order_pool_[b.orders_.at(id).node].remaining = 0;
    }

    static void drop_from_index(Book& b, OrderId id) {
        b.orders_.erase(id);
    }

    static void corrupt_handle_level(Book& b, OrderId id, Side side, Price price) {
        b.orders_.at(id).level = b.level_index(side, price);
    }
};
```

- [ ] **Step 4: The one sanctioned test edit** (spec §4: `corrupt_handle_price` → `corrupt_handle_level`; the handle no longer stores a price, so the corruption points it at another real level). Replace `TEST(Invariants, IndexHandleDisagreementIsFlagged)` with:

```cpp
TEST(Invariants, IndexHandleDisagreementIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 990'000, 50));
    BookTestPeer::corrupt_handle_level(book, 1, Side::Bid, 990'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "disagrees"));
}
```

- [ ] **Step 5: GREEN** — Run:

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: ALL tests green — the 108 (57 book + 36 itch + 15 replay) plus every fuzz suite. The differential now genuinely compares new internals against the frozen oracle. Also verify the public section is untouched: `git diff include/xeas/book.h` must show changes only in the includes and below `private:`.

- [ ] **Step 6: Commit**

```bash
git add include/xeas/book.h src/book.cpp tests/book_test.cpp
git commit -m "feat: Book phase 2 - pooled order slab + intrusive per-level FIFO lists"
```

### Task 7: `Pool` suite + phase-2 `InvariantsV2` corruptions

**Files:**
- Modify: `tests/book_fuzz_test.cpp` (fuzz-file `BookTestPeer` + Pool suite + 3 InvariantsV2 tests)

- [ ] **Step 1: Write the failing tests.** First add the fuzz-file peer — after the includes in `tests/book_fuzz_test.cpp`, BEFORE the anonymous namespace, insert:

```cpp
namespace xeas {

// Test-only backdoor (friend of Book) for the fuzz binary. A sibling of the
// peer in book_test.cpp — the two live in different binaries, so no ODR issue.
struct BookTestPeer {
    static std::uint32_t node_index(const Book& b, OrderId id) {
        return b.orders_.at(id).node;
    }
    static std::size_t order_pool_capacity(const Book& b) { return b.order_pool_.capacity(); }
    static std::size_t level_pool_capacity(const Book& b) { return b.level_pool_.capacity(); }
    static std::vector<OrderId> level_order_ids(const Book& b, Side side, Price price) {
        std::vector<OrderId> ids;
        const std::uint32_t idx = b.level_index(side, price);
        if (idx == Book::kNone) return ids;
        for (std::uint32_t cur = b.level_pool_[idx].head; cur != Book::kNone;
             cur = b.order_pool_[cur].next) {
            ids.push_back(b.order_pool_[cur].id);
        }
        return ids;
    }

    // --- corruption helpers ---
    static void set_order_free_head(Book& b, OrderId id) {   // node free AND in a level
        b.order_pool_.free_head_ = b.orders_.at(id).node;
    }
    static void set_level_free_head(Book& b, Side side, Price price) {
        b.level_pool_.free_head_ = b.level_index(side, price);
    }
    static void break_prev_link(Book& b, OrderId id) {   // corrupt a NON-head node
        b.order_pool_[b.orders_.at(id).node].prev = Book::kNone;
    }
    static void corrupt_handle_level(Book& b, OrderId id, Side side, Price price) {
        b.orders_.at(id).level = b.level_index(side, price);
    }
};

}  // namespace xeas
```

then add `using xeas::BookTestPeer;` at the top of the anonymous namespace, and append the suites:

```cpp
// ---------------------------------------------------------------------------
// Pool: slab allocate/release/reuse, index stability, freelist honesty
// ---------------------------------------------------------------------------

TEST(Pool, ReleaseThenReallocateReusesTheSlot) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    const std::uint32_t first_slot = BookTestPeer::node_index(book, 1);
    apply(book, make_delete(1));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    EXPECT_EQ(BookTestPeer::node_index(book, 2), first_slot);
    EXPECT_EQ(BookTestPeer::order_pool_capacity(book), 1u);
}

TEST(Pool, CapacityStableUnderChurn) {
    Book book;
    for (int round = 0; round < 3; ++round) {
        for (OrderId id = 1; id <= 100; ++id) {
            apply(book, make_add(id + static_cast<OrderId>(round) * 1'000, Side::Bid,
                                 1'000'000 - static_cast<Price>(id % 10) * 100, 10));
        }
        for (OrderId id = 1; id <= 100; ++id) {
            apply(book, make_delete(id + static_cast<OrderId>(round) * 1'000));
        }
    }
    EXPECT_EQ(BookTestPeer::order_pool_capacity(book), 100u);   // slots recycled
    EXPECT_EQ(BookTestPeer::level_pool_capacity(book), 10u);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(Pool, HandlesStableAcrossSlabGrowth) {
    Book book;
    for (OrderId id = 1; id <= 5'000; ++id) {   // many vector reallocations
        apply(book, make_add(id, Side::Bid, 1'000'000 - static_cast<Price>(id % 50), 10));
    }
    for (const OrderId id : {OrderId{1}, OrderId{2'500}, OrderId{5'000}}) {
        const auto info = book.find_order(id);
        ASSERT_TRUE(info.has_value()) << id;
        EXPECT_EQ(info->price, 1'000'000 - static_cast<Price>(id % 50));
        EXPECT_EQ(info->remaining_qty, 10u);
    }
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(Pool, FreelistCorruptionIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 10));
    apply(book, make_add(2, Side::Ask, 1'010'000, 20));
    BookTestPeer::set_order_free_head(book, 2);
    EXPECT_TRUE(any_contains(check_invariants(book), "freelist"));
}

TEST(Pool, FifoSurvivesHeavyChurn) {
    Book book;
    for (OrderId id = 1; id <= 50; ++id) {
        apply(book, make_add(id, Side::Ask, 1'010'000, 10));
    }
    for (OrderId id = 1; id <= 50; id += 2) {
        apply(book, make_delete(id));
    }
    std::vector<OrderId> expected;
    for (OrderId id = 2; id <= 50; id += 2) expected.push_back(id);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000), expected);
    apply(book, make_add(51, Side::Ask, 1'010'000, 10));
    expected.push_back(51);   // new order enqueues at the back
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000), expected);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(Pool, LevelPoolReusesReleasedLevels) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_delete(1));                          // level released
    apply(book, make_add(2, Side::Bid, 990'000, 50));     // different price, same slot
    EXPECT_EQ(BookTestPeer::level_pool_capacity(book), 1u);
    EXPECT_EQ(book.best_bid()->price, 990'000);
    EXPECT_TRUE(check_invariants(book).empty());
}

// ---------------------------------------------------------------------------
// InvariantsV2 (phase-2 subset): one corruption per new structural invariant
// ---------------------------------------------------------------------------

TEST(InvariantsV2, BrokenPrevNextReciprocityIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));   // id 2 is NOT the head
    BookTestPeer::break_prev_link(book, 2);
    EXPECT_TRUE(any_contains(check_invariants(book), "reciprocal"));
}

TEST(InvariantsV2, NodeOnFreelistAndInLevelIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::set_level_free_head(book, Side::Bid, 1'000'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "freelist"));
}

TEST(InvariantsV2, WrongLevelHandleIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'020'000, 50));
    BookTestPeer::corrupt_handle_level(book, 1, Side::Ask, 1'020'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "disagrees"));
}
```

- [ ] **Step 2: RED** — Run: `cmake --build build`
Expected: FAIL — `BookTestPeer` members reference the new internals; if anything is misnamed the compile fails here (with a correct Task 6, this compiles and the tests pass immediately — that is the acceptable "green-on-first-run" for corruption tests; if any of them passes VACUOUSLY, i.e. the checker misses the corruption, the EXPECT fails and that is the RED to fix in `check_invariants`).

- [ ] **Step 3: GREEN** — Run: `cmake --build build && ./build/book_fuzz_test --gtest_filter='Pool.*:InvariantsV2.*'` then full ctest.
Expected: 9 new tests pass; suite green.

- [ ] **Step 4: Commit**

```bash
git add tests/book_fuzz_test.cpp
git commit -m "test: Pool suite + phase-2 structural invariant corruptions"
```

---

### Task 8: Record phase-2 numbers

- [ ] **Step 1:** Rebuild Release and run the commands of record (same as Task 5 Step 4). Append `phase 2` rows to the `BENCH.md` micro table (3 workloads × 2 impls, ratio column from the `--impl both` output). **Manual (skip if no day file):** re-run the macro command, append the phase-2 macro row.

- [ ] **Step 2: Commit**

```bash
git add BENCH.md
git commit -m "docs: BENCH.md phase-2 numbers - order pool + intrusive lists"
```

### Task 9: Phase 3 — banded flat price array + two-level bitmaps + overflow map

Replaces the per-side `std::map` with `FlatSide`: a lazily-anchored band of `u32` slots (level index + 1), a per-tick occupancy bitmap with a one-bit-per-word summary, a cached best offset, and an exact `std::map` overflow for far prices. Pools, index, and `apply()` are untouched.

**Files:**
- Modify: `include/xeas/book.h` (private section), `src/book.cpp`
- Modify: `tests/book_fuzz_test.cpp` (peer band accessors + BandGrowth suite)

**Interfaces produced (used by Task 10):** `Book::FlatSide<Compare>{lo, slots, words, summary, best, overflow, level_count}`, `Book::BidSide`/`Book::AskSide`, members `bids_`/`asks_` of those types; `check_invariants` gains invariants 6–7 with messages containing `"bitmap disagrees"`, `"summary disagrees"`, `"cached best is stale"`, `"overflow contains in-band price"`.

- [ ] **Step 1: Write the failing tests.** Add band accessors to the fuzz-file `BookTestPeer`:

```cpp
    // --- band accessors (phase 3) ---
    static Price band_lo(const Book& b, Side side) {
        return side == Side::Bid ? b.bids_.lo : b.asks_.lo;
    }
    static std::size_t band_ticks(const Book& b, Side side) {
        return side == Side::Bid ? b.bids_.slots.size() : b.asks_.slots.size();
    }
    static bool in_overflow(const Book& b, Side side, Price price) {
        return side == Side::Bid ? b.bids_.overflow.count(price) != 0
                                 : b.asks_.overflow.count(price) != 0;
    }
    static std::vector<Price> ordered_prices(const Book& b, Side side) {
        std::vector<Price> prices;
        b.for_each_level(side, [&](std::uint32_t, const Book::Level& level) {
            prices.push_back(level.price);
        });
        return prices;
    }
```

and append the suite:

```cpp
// ---------------------------------------------------------------------------
// BandGrowth: anchor, geometric growth, overflow, merged best-first order
// ---------------------------------------------------------------------------

TEST(BandGrowth, FirstAddAnchorsTheBand) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 100));
    const std::size_t ticks = BookTestPeer::band_ticks(book, Side::Ask);
    EXPECT_GT(ticks, 0u);
    EXPECT_EQ(BookTestPeer::band_lo(book, Side::Ask),
              1'000'000 - static_cast<Price>(ticks / 2));   // centered on the first add
    EXPECT_FALSE(BookTestPeer::in_overflow(book, Side::Ask, 1'000'000));
    EXPECT_EQ(book.best_ask()->price, 1'000'000);
    EXPECT_EQ(BookTestPeer::band_ticks(book, Side::Bid), 0u);   // lazy per side
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, OutOfBandAddGrowsTheBandAndPreservesQueries) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 100));
    const std::size_t before = BookTestPeer::band_ticks(book, Side::Ask);
    apply(book, make_add(2, Side::Ask, 1'003'000, 50));   // past the initial hi edge
    EXPECT_GT(BookTestPeer::band_ticks(book, Side::Ask), before);
    EXPECT_FALSE(BookTestPeer::in_overflow(book, Side::Ask, 1'003'000));
    EXPECT_EQ(book.best_ask()->price, 1'000'000);
    EXPECT_EQ(book.size_at(Side::Ask, 1'000'000), 100);
    EXPECT_EQ(book.size_at(Side::Ask, 1'003'000), 50);
    EXPECT_EQ(book.level_count(Side::Ask), 2u);
    EXPECT_EQ(book.find_order(1)->price, 1'000'000);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, FarPriceLandsInOverflowAndCanBeBest) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 100));
    apply(book, make_add(2, Side::Ask, 100, 25));   // ~1M ticks away: overflow
    EXPECT_TRUE(BookTestPeer::in_overflow(book, Side::Ask, 100));
    EXPECT_EQ(book.size_at(Side::Ask, 100), 25);
    EXPECT_EQ(book.best_ask()->price, 100);         // the lone $0.01 ask is the BBO
    EXPECT_EQ(book.level_count(Side::Ask), 2u);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, LoneOverflowOrderIsTheBbo) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 5'000'000, 25));   // far above: overflow bid
    EXPECT_EQ(book.best_bid()->price, 5'000'000);
    apply(book, make_delete(2));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);
    apply(book, make_delete(1));
    apply(book, make_add(3, Side::Bid, 5'000'000, 10));   // band empty, overflow only
    EXPECT_EQ(book.best_bid()->price, 5'000'000);
    EXPECT_EQ(book.best_bid()->total_shares, 10);
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, BandAndOverflowInterleaveInSortedOrder) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 10));   // anchors the band
    apply(book, make_add(2, Side::Ask, 1'000'100, 10));   // in band
    apply(book, make_add(3, Side::Ask, 100, 10));         // overflow, below band
    apply(book, make_add(4, Side::Ask, 5'000'000, 10));   // overflow, above band
    EXPECT_EQ(BookTestPeer::ordered_prices(book, Side::Ask),
              (std::vector<Price>{100, 1'000'000, 1'000'100, 5'000'000}));
    apply(book, make_add(5, Side::Bid, 900'000, 10));
    apply(book, make_add(6, Side::Bid, 400'000, 10));     // out-of-band low bid: grows or overflows
    const auto bid_prices = BookTestPeer::ordered_prices(book, Side::Bid);
    EXPECT_EQ(bid_prices, (std::vector<Price>{900'000, 400'000}));   // descending
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, NextBestScanCrossesWordAndSummaryBoundaries) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'000'000, 10));
    apply(book, make_add(2, Side::Ask, 1'000'100, 10));   // different bitmap word
    apply(book, make_delete(1));
    EXPECT_EQ(book.best_ask()->price, 1'000'100);          // scan crossed a word
    apply(book, make_add(3, Side::Ask, 1'200'000, 10));    // grows the band wide
    apply(book, make_delete(2));
    EXPECT_EQ(book.best_ask()->price, 1'200'000);          // scan crossed summary words
    EXPECT_TRUE(check_invariants(book).empty());
}

TEST(BandGrowth, GrowthWhileCrossedKeepsBothSidesRight) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));   // crossed, stored as given
    apply(book, make_add(3, Side::Ask, 1'003'500, 50));    // out-of-band: grow while crossed
    EXPECT_TRUE(is_crossed(book));
    EXPECT_EQ(book.best_ask()->price, 1'000'000);
    EXPECT_EQ(book.best_bid()->price, 1'010'000);
    EXPECT_EQ(book.size_at(Side::Ask, 1'003'500), 50);
    EXPECT_TRUE(crossed_only(check_invariants(book)));
}
```

- [ ] **Step 2: RED** — Run: `cmake --build build`
Expected: FAIL — `bids_.lo` etc. do not exist on the phase-2 `Book`.

- [ ] **Step 3: Update the private section of `include/xeas/book.h`.** Replace the block from `using BidMap = ...` through `using OrderIndexMap = ...` with:

```cpp
    template <class Compare>           // std::greater<> bids, std::less<> asks
    struct FlatSide {
        Price lo = 0;                             // band = [lo, lo + slots.size())
        std::vector<std::uint32_t> slots;         // level index + 1; 0 = empty
        std::vector<std::uint64_t> words;         // occupancy, 64 ticks per word
        std::vector<std::uint64_t> summary;       // one bit per word
        std::uint32_t best = kNone;               // slot OFFSET of the best in-band level
        std::map<Price, std::uint32_t, Compare> overflow;  // far prices, exact
        std::size_t level_count = 0;              // band + overflow
    };
    using BidSide = FlatSide<std::greater<>>;
    using AskSide = FlatSide<std::less<>>;
    using OrderIndexMap = std::unordered_map<OrderId, OrderHandle>;

    // Band/overflow operations. Declared here, defined and only instantiated
    // in book.cpp.
    template <class Compare>
    static void band_anchor(FlatSide<Compare>& s, Price price);
    template <class Compare>
    static bool band_grow_to_cover(FlatSide<Compare>& s, Price price);
    template <class Compare>
    static std::uint32_t side_find(const FlatSide<Compare>& s, Price price);
    template <class Compare>
    static void side_insert(FlatSide<Compare>& s, Price price, std::uint32_t level_idx);
    template <class Compare>
    static void side_erase(FlatSide<Compare>& s, Price price);
    template <class Compare>
    static std::optional<std::pair<Price, std::uint32_t>> side_best(const FlatSide<Compare>& s);
    template <class Compare>
    void side_for_each(const FlatSide<Compare>& s,
                       const std::function<void(std::uint32_t, const Level&)>& fn) const;
```

and change the members `BidMap bids_; AskMap asks_;` to `BidSide bids_; AskSide asks_;`. Add `#include <utility>` to the header includes (for `std::pair`). Everything else — public section, pools, helpers, `OrderIndexMap orders_` — stays.

- [ ] **Step 4: Update `src/book.cpp`.** Four edits, complete code below. `apply()`, both pools, `find_order`, `order_count`, and `insert_order` are NOT touched.

**(a)** After `#include "xeas/book.h"` add:

```cpp
#include <algorithm>
#include <bit>
```

and this anonymous namespace before `namespace xeas {`:

```cpp
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
```

**(b)** After the pool definitions, add the band/overflow member templates:

```cpp
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
```

**(c)** Replace these five functions (bodies only, signatures unchanged):

```cpp
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

std::size_t Book::level_count(Side side) const {
    return side == Side::Bid ? bids_.level_count : asks_.level_count;
}

std::uint32_t Book::create_level(Side side, Price price) {
    const std::uint32_t idx = level_pool_.allocate(price, side);
    if (side == Side::Bid) {
        side_insert(bids_, price, idx);
    } else {
        side_insert(asks_, price, idx);
    }
    return idx;
}
```

in `Book::erase_order`, replace the map-erase block inside `if (level.head == kNone)` with:

```cpp
        if (level.side == Side::Bid) {
            side_erase(bids_, level.price);
        } else {
            side_erase(asks_, level.price);
        }
        level_pool_.release(h.level);
```

and replace `Book::for_each_level` with:

```cpp
void Book::for_each_level(
    Side side, const std::function<void(std::uint32_t, const Level&)>& fn) const {
    if (side == Side::Bid) {
        side_for_each(bids_, fn);
    } else {
        side_for_each(asks_, fn);
    }
}
```

**(d)** In `check_invariants`, insert invariants 6–7 immediately before the freelist section (after the `orders_in_levels` size check):

```cpp
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
```

(`<type_traits>` comes in transitively; if the build complains, add `#include <type_traits>`.)

- [ ] **Step 5: GREEN** — Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: everything green — the 108, all fuzz suites (the differential now hammers band growth and overflow via `FarPriceHeavyMix`/`Deep` paths), and the 7 new `BandGrowth.*`. Also `git diff include/xeas/book.h` still shows no public-section change.

- [ ] **Step 6: Commit**

```bash
git add include/xeas/book.h src/book.cpp tests/book_fuzz_test.cpp
git commit -m "feat: Book phase 3 - banded flat price array, occupancy bitmaps, overflow map"
```

---

### Task 10: Phase-3 `InvariantsV2` corruptions + phase-3 numbers + band tuning

**Files:**
- Modify: `tests/book_fuzz_test.cpp`, `BENCH.md`

- [ ] **Step 1: Write the failing tests.** Add to the fuzz-file `BookTestPeer`:

```cpp
    // --- phase-3 corruption helpers ---
    template <class FlatSideT>
    static void flip_tick_bit_in(FlatSideT& s, Price price) {
        const auto off = static_cast<std::size_t>(price - s.lo);
        s.words[off >> 6] ^= (1ULL << (off & 63));
    }
    static void flip_tick_bit(Book& b, Side side, Price price) {
        if (side == Side::Bid) flip_tick_bit_in(b.bids_, price);
        else flip_tick_bit_in(b.asks_, price);
    }
    static void clear_best_cache(Book& b, Side side) {
        (side == Side::Bid ? b.bids_.best : b.asks_.best) = Book::kNone;
    }
    static void inject_overflow_in_band(Book& b, Side side, Price price) {
        if (side == Side::Bid) b.bids_.overflow.emplace(price, 0u);
        else b.asks_.overflow.emplace(price, 0u);
    }
```

and the tests:

```cpp
TEST(InvariantsV2, BitmapSlotDisagreementIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::flip_tick_bit(book, Side::Bid, 1'000'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "bitmap disagrees"));
}

TEST(InvariantsV2, StaleCachedBestIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    BookTestPeer::clear_best_cache(book, Side::Ask);
    EXPECT_TRUE(any_contains(check_invariants(book), "cached best"));
}

TEST(InvariantsV2, InBandPriceInOverflowIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::inject_overflow_in_band(book, Side::Bid, 1'000'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "overflow contains in-band price"));
}
```

- [ ] **Step 2: RED → GREEN** — Run: `cmake --build build && ./build/book_fuzz_test --gtest_filter='InvariantsV2.*'` (a compile failure or a vacuous pass is the RED to fix), then full ctest green.

- [ ] **Step 3: Record phase-3 numbers** — same commands of record; append `phase 3` rows to `BENCH.md` (micro; macro row manual if a day file exists). **Band-constant tuning (manual, requires a day file — otherwise record "provisional, no day file available" in BENCH.md):** compute a per-symbol price-range histogram of the day (throwaway script over the replayed data, not committed) and adjust `kInitialBandTicks`/`kMaxBandTicks` in `src/book.cpp` if the histogram warrants it; document the decision in BENCH.md's band-constants section. If constants change, re-run the bench and ctest.

- [ ] **Step 4: Commit**

```bash
git add tests/book_fuzz_test.cpp BENCH.md src/book.cpp
git commit -m "test: phase-3 invariant corruptions; docs: BENCH.md phase-3 numbers"
```

### Task 11: Phase 4 — flat open-addressing `OrderIndex`

Replaces `std::unordered_map<OrderId, OrderHandle>` with a flat power-of-2 table: linear probing, Fibonacci-mixed key (`id * 0x9E3779B97F4A7C15` — ITCH order references are near-sequential, identity hashing would cluster catastrophically), max load ~0.7, backward-shift deletion (no tombstones), empty slot ⇔ `handle.node == kNone` (no reserved `OrderId`).

**Files:**
- Modify: `include/xeas/book.h`, `src/book.cpp`
- Modify: `tests/book_fuzz_test.cpp` (peer index helpers + OrderIndexUnit suite)
- Modify: `tests/book_test.cpp` (mechanical peer edit: `orders_.at(id)` → `*b.orders_.find(id)`)

- [ ] **Step 1: Write the failing tests.** Add to the fuzz-file `BookTestPeer`:

```cpp
    // --- OrderIndex unit-test access (phase 4) ---
    static Book::OrderIndex make_index() { return {}; }
    static void index_insert(Book::OrderIndex& idx, OrderId id, std::uint32_t node) {
        idx.insert(id, Book::OrderHandle{node, 0});
    }
    static std::uint32_t index_find_node(const Book::OrderIndex& idx, OrderId id) {
        const auto* h = idx.find(id);
        return h == nullptr ? Book::kNone : h->node;
    }
    static void index_erase(Book::OrderIndex& idx, OrderId id) { idx.erase(id); }
    static std::size_t index_size(const Book::OrderIndex& idx) { return idx.size(); }
    static constexpr std::uint32_t none() { return Book::kNone; }
```

add `#include <unordered_map>` and `#include <random>` to the fuzz file's includes, and append the suite (note `auto` — the nested type's name is private, its objects are not):

```cpp
// ---------------------------------------------------------------------------
// OrderIndexUnit: open addressing, Fibonacci mix, backward-shift erase
// ---------------------------------------------------------------------------

TEST(OrderIndexUnit, MatchesReferenceMapUnderChurn) {
    auto idx = BookTestPeer::make_index();
    std::unordered_map<OrderId, std::uint32_t> reference;
    std::vector<OrderId> live;
    std::mt19937_64 rng(7);
    OrderId next_id = 1;
    for (int i = 0; i < 200'000; ++i) {
        if (live.empty() || rng() % 100 < 55) {
            const OrderId id = next_id++;   // near-sequential, like ITCH refs
            BookTestPeer::index_insert(idx, id, static_cast<std::uint32_t>(id));
            reference.emplace(id, static_cast<std::uint32_t>(id));
            live.push_back(id);
        } else {
            const std::size_t v = rng() % live.size();
            const OrderId id = live[v];
            live[v] = live.back();
            live.pop_back();
            BookTestPeer::index_erase(idx, id);
            reference.erase(id);
        }
    }
    ASSERT_EQ(BookTestPeer::index_size(idx), reference.size());
    for (const auto& [id, node] : reference) {
        ASSERT_EQ(BookTestPeer::index_find_node(idx, id), node) << "id " << id;
    }
    for (OrderId id = next_id; id < next_id + 1'000; ++id) {
        ASSERT_EQ(BookTestPeer::index_find_node(idx, id), BookTestPeer::none());
    }
}

TEST(OrderIndexUnit, BackwardShiftKeepsClustersFindable) {
    auto idx = BookTestPeer::make_index();
    for (OrderId id = 1; id <= 3'000; ++id) {   // forces several rehashes
        BookTestPeer::index_insert(idx, id, static_cast<std::uint32_t>(id));
    }
    for (OrderId id = 2; id <= 3'000; id += 2) {
        BookTestPeer::index_erase(idx, id);
    }
    for (OrderId id = 1; id <= 3'000; ++id) {
        if (id % 2 == 1) {
            ASSERT_EQ(BookTestPeer::index_find_node(idx, id), static_cast<std::uint32_t>(id));
        } else {
            ASSERT_EQ(BookTestPeer::index_find_node(idx, id), BookTestPeer::none());
        }
    }
    EXPECT_EQ(BookTestPeer::index_size(idx), 1'500u);
}

TEST(OrderIndexUnit, SizeTracksInsertsAndErases) {
    auto idx = BookTestPeer::make_index();
    EXPECT_EQ(BookTestPeer::index_size(idx), 0u);
    EXPECT_EQ(BookTestPeer::index_find_node(idx, 1), BookTestPeer::none());  // empty table
    BookTestPeer::index_insert(idx, 1, 10);
    BookTestPeer::index_insert(idx, 2, 20);
    EXPECT_EQ(BookTestPeer::index_size(idx), 2u);
    EXPECT_EQ(BookTestPeer::index_find_node(idx, 1), 10u);
    BookTestPeer::index_erase(idx, 1);
    EXPECT_EQ(BookTestPeer::index_size(idx), 1u);
    EXPECT_EQ(BookTestPeer::index_find_node(idx, 1), BookTestPeer::none());
    EXPECT_EQ(BookTestPeer::index_find_node(idx, 2), 20u);
}
```

- [ ] **Step 2: RED** — Run: `cmake --build build`
Expected: FAIL — `Book::OrderIndex` does not exist yet.

- [ ] **Step 3: Update `include/xeas/book.h`.** Remove `#include <unordered_map>`. Replace the line `using OrderIndexMap = std::unordered_map<OrderId, OrderHandle>;` with:

```cpp
    class OrderIndex {                 // flat open addressing, linear probe,
    public:                            // Fibonacci-mixed key, backward-shift erase
        OrderHandle* find(OrderId id);
        const OrderHandle* find(OrderId id) const;
        void insert(OrderId id, OrderHandle h);   // precondition: id absent
        void erase(OrderId id);                   // precondition: id present
        std::size_t size() const noexcept { return size_; }
    private:
        struct Slot { OrderId id; OrderHandle h; };  // empty <=> h.node == kNone
        std::size_t home(OrderId id) const;
        void rehash(std::size_t new_capacity);
        std::vector<Slot> slots_;      // power-of-2 capacity; max load ~0.7
        std::size_t size_ = 0;
        friend std::vector<std::string> check_invariants(const Book& book);
        friend struct BookTestPeer;
    };
```

Change the private helper `void erase_order(OrderIndexMap::iterator index_it);` to `void erase_order(OrderId order_id, OrderHandle h);` and the member `OrderIndexMap orders_;` to `OrderIndex orders_;`.

- [ ] **Step 4: Update `src/book.cpp`.**

**(a)** Add to the anonymous namespace:

```cpp
constexpr std::size_t kIndexInitialCapacity = 1'024;
constexpr std::uint64_t kFibMix = 0x9E37'79B9'7F4A'7C15ULL;
```

**(b)** Add the `OrderIndex` definitions (after the pools):

```cpp
// --- order index (spec §3.3) -----------------------------------------------------

std::size_t Book::OrderIndex::home(OrderId id) const {
    // Fibonacci mix, top bits: near-sequential ITCH order references spread
    // uniformly; identity hashing would cluster catastrophically.
    return static_cast<std::size_t>(
        (id * kFibMix) >> (64 - std::countr_zero(slots_.size())));
}

OrderHandle* Book::OrderIndex::find(OrderId id) {
    if (slots_.empty()) {
        return nullptr;
    }
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = home(id);
    while (slots_[i].h.node != kNone) {
        if (slots_[i].id == id) {
            return &slots_[i].h;
        }
        i = (i + 1) & mask;
    }
    return nullptr;
}

const OrderHandle* Book::OrderIndex::find(OrderId id) const {
    return const_cast<OrderIndex*>(this)->find(id);
}

void Book::OrderIndex::insert(OrderId id, OrderHandle h) {
    if (slots_.empty()) {
        rehash(kIndexInitialCapacity);
    } else if ((size_ + 1) * 10 > slots_.size() * 7) {   // max load ~0.7
        rehash(slots_.size() * 2);
    }
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = home(id);
    while (slots_[i].h.node != kNone) {
        i = (i + 1) & mask;
    }
    slots_[i] = Slot{id, h};
    ++size_;
}

void Book::OrderIndex::rehash(std::size_t new_capacity) {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(new_capacity, Slot{0, OrderHandle{kNone, kNone}});
    for (const Slot& s : old) {
        if (s.h.node == kNone) continue;
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = home(s.id);
        while (slots_[i].h.node != kNone) {
            i = (i + 1) & mask;
        }
        slots_[i] = s;
    }
}

void Book::OrderIndex::erase(OrderId id) {
    if (slots_.empty()) {
        return;   // precondition violated upstream; erase of absent id is a no-op
    }
    const std::size_t mask = slots_.size() - 1;
    std::size_t hole = home(id);
    while (slots_[hole].h.node != kNone && slots_[hole].id != id) {
        hole = (hole + 1) & mask;
    }
    if (slots_[hole].h.node == kNone) {
        return;
    }
    // Backward-shift deletion (no tombstones): any later cluster member whose
    // probe path crosses the hole moves back into it.
    std::size_t j = hole;
    for (;;) {
        j = (j + 1) & mask;
        if (slots_[j].h.node == kNone) break;
        const std::size_t h = home(slots_[j].id);
        if (((j - h) & mask) >= ((j - hole) & mask)) {
            slots_[hole] = slots_[j];
            hole = j;
        }
    }
    slots_[hole].h.node = kNone;
    --size_;
}
```

(`std::countr_zero` needs `<bit>` — already included since phase 3.)

**(c)** Change `insert_order`'s last line from `orders_.emplace(order_id, OrderHandle{node_idx, level_idx});` to `orders_.insert(order_id, OrderHandle{node_idx, level_idx});`, and `find_order`'s lookup to:

```cpp
std::optional<OrderInfo> Book::find_order(OrderId order_id) const {
    const OrderHandle* h = orders_.find(order_id);
    if (h == nullptr) {
        return std::nullopt;
    }
    const OrderNode& node = order_pool_[h->node];
    const Level& level = level_pool_[h->level];
    return OrderInfo{level.side, level.price, node.remaining};
}
```

**(d)** Rewrite `erase_order` with the new signature (body otherwise identical to phase 3 — the handle arrives by value, the final line erases by id):

```cpp
void Book::erase_order(OrderId order_id, OrderHandle h) {
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
        if (level.side == Side::Bid) {
            side_erase(bids_, level.price);
        } else {
            side_erase(asks_, level.price);
        }
        level_pool_.release(h.level);
    }
    orders_.erase(order_id);
}
```

**(e)** Update `apply()` lookups (validation order and messages unchanged):
- Add: `if (book.orders_.find(event.order_id) != nullptr) { throw ... }`
- Cancel/Execute:

```cpp
        OrderHandle* handle = book.orders_.find(event.order_id);
        if (handle == nullptr) {
            throw BookError(event, std::string(name) + ": unknown order_id");
        }
        if (event.quantity == 0) {
            throw BookError(event, std::string(name) + ": zero quantity");
        }
        OrderNode& node = book.order_pool_[handle->node];
        if (event.quantity > node.remaining) {
            throw BookError(event, std::string(name) + ": quantity exceeds remaining shares");
        }
        if (event.quantity == node.remaining) {
            book.erase_order(event.order_id, *handle);  // reaches zero: removed immediately
        } else {
            node.remaining -= event.quantity;
            book.level_pool_[handle->level].total_shares -=
                static_cast<std::int64_t>(event.quantity);
        }
```

- Delete:

```cpp
        const OrderHandle* handle = book.orders_.find(event.order_id);
        if (handle == nullptr) {
            throw BookError(event, "Delete: unknown order_id");
        }
        book.erase_order(event.order_id, *handle);
```

- Replace (side resolved before the erase invalidates the pointer):

```cpp
        const OrderHandle* handle = book.orders_.find(event.order_id);
        if (handle == nullptr) {
            throw BookError(event, "Replace: unknown order_id");
        }
        if (event.new_order_id == event.order_id ||
            book.orders_.find(event.new_order_id) != nullptr) {
            throw BookError(event, "Replace: new_order_id already exists");
        }
        if (event.quantity == 0) {
            throw BookError(event, "Replace: zero quantity");
        }
        if (event.price <= 0) {
            throw BookError(event, "Replace: non-positive price");
        }
        const Side side = book.level_pool_[handle->level].side;
        book.erase_order(event.order_id, *handle);
        book.insert_order(side, event.price, event.new_order_id, event.quantity);
```

**(f)** In `check_invariants`: replace the `orders_.find` iterator usage in the chain walk with the pointer form:

```cpp
                const Book::OrderHandle* handle = book.orders_.find(node.id);
                if (handle == nullptr) {
                    violations.push_back("order " + std::to_string(node.id) + " in " + name +
                                         " level " + std::to_string(price) +
                                         " missing from index");
                } else if (handle->node != cur || handle->level != level_idx) {
                    violations.push_back("index entry for order " + std::to_string(node.id) +
                                         " disagrees with its level");
                }
```

and add the probe self-check right after the `orders_in_levels` size comparison:

```cpp
    // Phase-4 index self-check: every occupied slot must be reachable from its
    // home position (backward-shift erase keeps probe chains gap-free).
    std::size_t index_entries = 0;
    for (const auto& slot : book.orders_.slots_) {
        if (slot.h.node == Book::kNone) continue;
        ++index_entries;
        if (book.orders_.find(slot.id) != &slot.h) {
            violations.push_back("index probe chain broken for order " +
                                 std::to_string(slot.id));
        }
    }
    if (index_entries != book.orders_.size()) {
        violations.push_back("index occupied slots " + std::to_string(index_entries) +
                             " != index size " + std::to_string(book.orders_.size()));
    }
```

- [ ] **Step 5: Mechanical peer edits.** In BOTH `tests/book_test.cpp` and `tests/book_fuzz_test.cpp`, replace every `b.orders_.at(id)` with `(*b.orders_.find(id))` and every `b.orders_.erase(id)` (in `drop_from_index`) stays as-is (`OrderIndex::erase(OrderId)` has the same spelling). Concretely, e.g. `zero_order_qty` becomes:

```cpp
    static void zero_order_qty(Book& b, OrderId id) {
        b.order_pool_[b.orders_.find(id)->node].remaining = 0;
    }
```

(same pattern for `node_index`, `corrupt_handle_level`, `set_order_free_head`).

- [ ] **Step 6: GREEN** — Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: everything green — 108 + all fuzz suites (the differential's 100k-event churn now exercises rehash and backward shift constantly) + 3 `OrderIndexUnit.*`. `git diff include/xeas/book.h`: public section still untouched; `<unordered_map>` gone from the header.

- [ ] **Step 7: Commit**

```bash
git add include/xeas/book.h src/book.cpp tests/book_test.cpp tests/book_fuzz_test.cpp
git commit -m "feat: Book phase 4 - flat open-addressing OrderIndex with backward-shift erase"
```

---

### Task 12: Final numbers, README line, verification sweep

**Files:**
- Modify: `BENCH.md`, `README.md`

- [ ] **Step 1: Record phase-4 numbers** — commands of record; append `phase 4` micro rows. **Manual (day file):** final macro run with peak RSS; fill the book-vs-feed split conclusion in `BENCH.md` (compare `book_bench steady` Mmsg/s vs `itch_replay` msgs/s).

- [ ] **Step 2: README** — add one summary line + link under the Milestone section, e.g.:

```markdown
- **Milestone 3 — benchmark & optimize:** pooled intrusive levels, banded
  bitmap price array, open-addressing order index behind the frozen `book.h`
  API; before/after numbers per optimization in [BENCH.md](BENCH.md).
```

(adjust wording/placement to the README's existing milestone list style; include the flagship steady-workload speedup once measured).

- [ ] **Step 3: Verification sweep** — run and confirm each:

```bash
# 1. Full suite, both build types
ctest --test-dir build --output-on-failure
cmake --build build-release && ctest --test-dir build-release --output-on-failure
# 2. Public API frozen: only includes + private section changed since main
git diff main -- include/xeas/book.h
# 3. Layering: no ITCH knowledge in the core or bench
grep -rn 'itch\|router\|replay' include/xeas/book.h src/book.cpp bench/ && echo LEAK || echo clean
# 4. Baseline isolation: xeas_baseline linked only by book_bench and book_fuzz_test
grep -n 'xeas_baseline' CMakeLists.txt
# 5. Bench CLI contract
./build-release/book_bench --workload steady --events 1000000 --warmup 100000 --repeat 3
./build-release/book_bench --events abc ; echo "exit=$?"   # usage, exit 1
```

- [ ] **Step 4: Commit + PR**

```bash
git add BENCH.md README.md
git commit -m "docs: BENCH.md phase-4 numbers; README Milestone 3 summary"
git push -u origin feature/benchmark-optimize
gh pr create --base main --title "Milestone 3: benchmark & optimize — pooled levels, banded bitmap price array, flat order index" --body "..."
```

---

## Verification (end-to-end)

1. `ctest` green in Debug-ish (`build/`) and Release (`build-release/`) — 146 tests (108 existing + 38 new).
2. `FuzzDifferential` green at every phase boundary — the frozen oracle agrees with the optimized book on every query, 100k+ events × 7 mixes × 10 extra seeds.
3. `BENCH.md` has micro rows for phases 1–4 (each optimization measured individually) and the ratio column shows the cumulative speedup; macro rows filled or explicitly marked pending-no-day-file.
4. Public API frozen (`git diff main -- include/xeas/book.h` shows includes + private section only); layering grep clean; `xeas_baseline` never linked into production targets.
5. `book_bench` and `itch_replay` both print peak RSS; `itch_replay` output is otherwise unchanged.

## Task summary

| # | Task | New tests |
|---|---|---|
| 1 | BaselineBook + xeas_baseline | 3 BaselineSmoke |
| 2 | TapeGen | 4 TapeGen |
| 3 | Differential harness | 7 FuzzDifferential |
| 4 | Workloads + book_bench | 2 BenchSmoke |
| 5 | RSS line + BENCH.md phase 1 | — |
| 6 | Phase 2: pools + intrusive FIFOs | (108 regression) |
| 7 | Pool suite + InvariantsV2 (2) | 6 + 3 |
| 8 | BENCH.md phase 2 | — |
| 9 | Phase 3: FlatSide | 7 BandGrowth |
| 10 | InvariantsV2 (3) + BENCH.md phase 3 | 3 |
| 11 | Phase 4: OrderIndex | 3 OrderIndexUnit |
| 12 | Final numbers + README | — |







