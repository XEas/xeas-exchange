# Order Book Core (Milestone 1) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use **superpowers:subagent-driven-development** (recommended) or **superpowers:executing-plans** to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Each task is self-contained: write the failing test, RUN it and observe the stated failure (RED), write the implementation, RUN again and observe pass (GREEN), commit. Do not skip RED steps. Do not deviate from the code given here.

## Context

xeas-exchange (see `DESIGN.md`) is a single-threaded order book reconstruction engine in C++. This plan implements **Milestone 1: the book core** — a pure, deterministic, venue-agnostic L3 (market-by-order) limit order book state machine — exactly per the approved spec. The repo currently contains only docs; this is a greenfield C++20/CMake/GoogleTest build. The book consumes canonical `Event`s (`Add`/`Cancel`/`Delete`/`Execute`/`Replace`), maintains exact book state with strict time priority, exposes a query API (BBO, level size, order lookup), and provides an out-of-band `check_invariants()` checker. Milestone 3 will swap internals behind this same API, so the API here is final.

**Goal:** Implement the canonical `Event` struct, the `Book` type with `apply()` semantics for all five event types, the query API, `check_invariants()`, and a full event-tape unit-test suite.

**Spec (this plan implements it EXACTLY):** `docs/superpowers/specs/2026-08-15-order-book-core-design.md`
**Project design:** `DESIGN.md`

**Architecture:**

- `include/xeas/event.h` — `Event`, `Side`, `EventType`, type aliases (`Price` = `int64_t`, `Qty` = `uint32_t`, `OrderId` = `uint64_t`, `Timestamp` = `uint64_t`). All fixed-width integers; no floating point.
- `include/xeas/book.h` — `Book` class; free functions `apply(Book&, const Event&)` and `check_invariants(const Book&)`; `BookError`; query result structs `BBO` and `OrderInfo`.
- `src/book.cpp` — implementation.
- `tests/book_test.cpp` — event-tape unit tests (single test file per spec §7).
- Internals (baseline, correctness-first per spec §3 — Milestone 3 swaps these behind the same API):
  - Bids: `std::map<Price, Level, std::greater<>>`; Asks: `std::map<Price, Level, std::less<>>` (best price is `begin()` on both).
  - `Level` = `std::list<Order>` FIFO (front = oldest, strict time priority; stable iterators) + cached `int64_t total_shares` maintained incrementally.
  - Order index: `std::unordered_map<OrderId, OrderHandle>` with `OrderHandle {Side side, Price price, Level* level, std::list<Order>::iterator it}` — `Level*` and the list iterator are stable; `price` is used only for the O(log levels) map erase when a level empties. O(1)-amortized order lookup per spec.
  - `Order` in the list stores only `{OrderId id, Qty remaining}` — side/price live in the handle/level key.
- The book never matches. A crossed incoming `Add` is stored as given; only `check_invariants()` flags it.
- `Execute` has the exact same book effect as `Cancel`. `Delete` ignores `quantity`/`price`. `Replace` atomically removes the old order and enqueues the new one at the **back** of its (possibly new) level on the **same side** — time priority lost.

**Tech Stack:** C++20, CMake ≥ 3.24, GoogleTest **v1.17.0** via `FetchContent`, ctest. Library target `xeas_core`, test target `book_test`.

**Design decisions baked in (all spec-conformant):**
1. `check_invariants(const Book&)` **returns** `std::vector<std::string>` of violation descriptions (empty = consistent) rather than throwing — spec §6 leaves this to the plan; returning lets fuzz/replay drivers log all violations at once. Tests assert emptiness.
2. Level FIFO container: `std::list<Order>` (stable iterators for the handle).
3. Order index handle as described in Architecture above.
4. `Order` = `{OrderId id, Qty remaining}` only.
5. BBO query returns `std::optional<BBO>` with `struct BBO { Price price; std::int64_t total_shares; }`.
6. `BookError` derives from `std::runtime_error` and carries the offending `Event` plus a reason string (spec §4).
7. `apply()` validates everything **before** mutating (spec only requires "state unspecified after throw"; pre-validation is cheap and makes tests deterministic). Tests must NOT assert post-throw state beyond what pre-validation trivially guarantees.

**One deliberate structural choice:** `include/xeas/book.h` is written once, in its complete final form, in Task 2 (Task 1 uses a minimal placeholder). It contains only declarations (plus the trivially header-only `BookError`); every declared behavior is still implemented test-first in its own task. This keeps signatures byte-identical across all tasks.

## Global Constraints (from spec — apply to every task)

- **C++20**, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`).
- **No floating point anywhere.** Prices are `int64_t` ticks; quantities `uint32_t`; aggregates `int64_t`.
- **Fixed-width integer types** (`<cstdint>`) for all event/book scalars.
- **No matching, no I/O, no clocks, no threads** in the core. `Book` is single-instrument, single-threaded.
- `apply(Book&, const Event&)` is a **free function returning `void`** (pure state mutation).
- Queries are `const`, allocation-free, O(1) or O(log levels). (`find_order` returning `std::optional<OrderInfo>` by value is allocation-free and permitted.)
- `check_invariants()` is **never called inside `apply()`** — the hot path stays clean.
- **No empty levels ever**; zero-remaining orders removed immediately.
- `timestamp` is carried, never interpreted.
- Library target name is exactly **`xeas_core`**.
- Baseline `std::map` structures only — no optimization in this milestone (Milestone 3 swaps internals behind this API).
- Strict TDD: never write implementation before seeing the task's tests fail. Commit after every green task.
- All shell commands below run from the repository root `/Users/glebmokeev/xeas-exchange`. Build directory is `build/`.

**Test execution conventions:**
- Configure (once, Task 1; re-runs automatically on CMake changes): `cmake -S . -B build`
- Build: `cmake --build build`
- Full suite: `ctest --test-dir build --output-on-failure`
- Single task's tests: `./build/book_test --gtest_filter='<Suite>.*'`
- First configure downloads GoogleTest from GitHub (network required).

**Before Task 1:**

```bash
# Save this plan into the repo (superpowers convention), then branch:
mkdir -p docs/superpowers/plans
cp /Users/glebmokeev/.claude/plans/implement-026-08-15-order-book-core-desi-concurrent-otter.md \
   docs/superpowers/plans/2026-08-16-order-book-core.md
git checkout -b feature/order-book-core
```

---

## Task 1 — Project skeleton: CMake, `event.h`, minimal `Book`, sanity test

**Files**
- Create: `CMakeLists.txt`
- Create: `.gitignore`
- Create: `include/xeas/event.h`
- Create: `include/xeas/book.h` (minimal placeholder — fully replaced in Task 2)
- Create: `src/book.cpp` (placeholder TU — fully replaced in Task 2)
- Test: `tests/book_test.cpp` (created here; appended to in every later task)

**Interfaces**
- Produces: `xeas::Price`, `xeas::Qty`, `xeas::OrderId`, `xeas::Timestamp` aliases; `enum class xeas::Side : std::uint8_t { Bid, Ask }`; `enum class xeas::EventType : std::uint8_t { Add, Cancel, Delete, Execute, Replace }`; `struct xeas::Event`; empty `class xeas::Book`.
- Consumes: nothing (greenfield).

### Steps

- [ ] 1. Write the test and build files FIRST so the build failure is the RED step. Create `tests/book_test.cpp` with exactly:

```cpp
#include "xeas/book.h"
#include "xeas/event.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace {

using namespace xeas;

// ---------------------------------------------------------------------------
// Task 1: skeleton sanity
// ---------------------------------------------------------------------------

TEST(Skeleton, EventFieldsRoundTrip) {
    Event e;
    e.type = EventType::Add;
    e.timestamp = 34'200'000'000'000ULL;  // 09:30 in ns since midnight; carried, never interpreted
    e.order_id = 42;
    e.side = Side::Bid;
    e.price = 1'234'500;  // ticks (ITCH fixed point x10,000)
    e.quantity = 100;
    e.new_order_id = 0;

    EXPECT_EQ(e.type, EventType::Add);
    EXPECT_EQ(e.timestamp, 34'200'000'000'000ULL);
    EXPECT_EQ(e.order_id, 42u);
    EXPECT_EQ(e.side, Side::Bid);
    EXPECT_EQ(e.price, 1'234'500);
    EXPECT_EQ(e.quantity, 100u);
    EXPECT_EQ(e.new_order_id, 0u);
}

TEST(Skeleton, TypeAliasesAreFixedWidth) {
    static_assert(std::is_same_v<Price, std::int64_t>);
    static_assert(std::is_same_v<Qty, std::uint32_t>);
    static_assert(std::is_same_v<OrderId, std::uint64_t>);
    static_assert(std::is_same_v<Timestamp, std::uint64_t>);
    SUCCEED();
}

TEST(Skeleton, BookDefaultConstructs) {
    Book book;
    (void)book;
    SUCCEED();
}

}  // namespace
```

- [ ] 2. Create `CMakeLists.txt` with exactly:

```cmake
cmake_minimum_required(VERSION 3.24)
project(xeas_exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- core library -----------------------------------------------------------
add_library(xeas_core src/book.cpp)
target_include_directories(xeas_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(xeas_core PUBLIC cxx_std_20)
if(NOT MSVC)
  target_compile_options(xeas_core PRIVATE -Wall -Wextra)
endif()

# --- tests -------------------------------------------------------------------
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)
# For Windows: prevent overriding the parent project's compiler/linker settings.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(book_test tests/book_test.cpp)
target_link_libraries(book_test PRIVATE xeas_core GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(book_test PRIVATE -Wall -Wextra)
endif()

include(GoogleTest)
gtest_discover_tests(book_test)
```

- [ ] 3. Create `.gitignore` with exactly:

```
build/
.DS_Store
.idea/
```

- [ ] 4. RED — configure and build; observe failure because the headers do not exist yet:

```bash
cmake -S . -B build && cmake --build build
```

Expected failure: compilation of `tests/book_test.cpp` (or `src/book.cpp`) aborts with a fatal error similar to `'xeas/book.h' file not found`. (The configure step itself succeeds and downloads GoogleTest v1.17.0; only the build fails. If configure fails on `src/book.cpp` missing — "Cannot find source file" — that is also a valid RED; proceed.)

- [ ] 5. GREEN part 1 — create `include/xeas/event.h` with exactly:

```cpp
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
```

- [ ] 6. GREEN part 2 — create `include/xeas/book.h` (minimal placeholder; Task 2 replaces this file entirely) with exactly:

```cpp
#pragma once

#include "xeas/event.h"

namespace xeas {

class Book {};

}  // namespace xeas
```

- [ ] 7. GREEN part 3 — create `src/book.cpp` (placeholder; Task 2 replaces this file entirely) with exactly:

```cpp
#include "xeas/book.h"

// Implementation arrives in later tasks; this translation unit anchors xeas_core.
```

(Note: on macOS, `ranlib` may warn that this object file has no symbols. The warning is harmless and disappears in Task 2.)

- [ ] 8. Build and run; observe all 3 tests pass:

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 3`.

- [ ] 9. Commit:

```bash
git add CMakeLists.txt .gitignore include src tests docs/superpowers/plans
git commit -m "Task 1: project skeleton (CMake, event.h, minimal Book, sanity tests)" -m "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2 — `apply(Add)` + full query API (final `book.h`)

**Files**
- Modify: `include/xeas/book.h` (replace entire file with its **final** form — never modified again)
- Modify: `src/book.cpp` (replace entire file)
- Test: `tests/book_test.cpp` (append)

**Interfaces**
- Produces (all in namespace `xeas`, all final for the milestone):
  - `class BookError : public std::runtime_error` — ctor `BookError(const Event& event, const std::string& reason)`; accessor `const Event& event() const noexcept`
  - `struct BBO { Price price; std::int64_t total_shares; };`
  - `struct OrderInfo { Side side; Price price; Qty remaining_qty; };`
  - `class Book` queries: `std::optional<BBO> best_bid() const;` `std::optional<BBO> best_ask() const;` `std::int64_t size_at(Side side, Price price) const;` `std::optional<OrderInfo> find_order(OrderId order_id) const;` `std::size_t order_count() const;` `std::size_t level_count(Side side) const;`
  - `void apply(Book& book, const Event& event);`
  - `std::vector<std::string> check_invariants(const Book& book);` (declared here; defined in Task 3)
  - Test-only friend `struct BookTestPeer` (defined in the test file)
- Consumes: `xeas/event.h` from Task 1.
- Implemented in this task: the six queries, private `Book::insert_order`, and the `Add` branch of `apply()`. `Cancel`/`Execute`/`Delete`/`Replace` branches are no-ops until Tasks 4–6; `apply()` validation arrives in Task 7; `check_invariants` and `Book::erase_order` are declared but not yet defined (not referenced, so the build links).

### Steps

- [ ] 1. RED — first add these includes at the top of `tests/book_test.cpp`, after the existing `#include <type_traits>` line:

```cpp
#include <string>
#include <string_view>
#include <vector>
```

Then append to `tests/book_test.cpp`, **inside** the anonymous namespace (i.e., insert immediately before the final closing `}  // namespace` line — note the appended block deliberately closes the anonymous namespace, opens `namespace xeas` for the test peer, and reopens an anonymous namespace which the file's original closing brace then closes), exactly:

```cpp
// ---------------------------------------------------------------------------
// Test helpers: event factories
// ---------------------------------------------------------------------------

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

}  // namespace

namespace xeas {

// Test-only backdoor (friend of Book) used to observe FIFO queue order and,
// from Task 3 on, to corrupt internal state so the invariant checker can be
// genuinely tested. Never used by production code.
struct BookTestPeer {
    static std::vector<OrderId> level_order_ids(const Book& b, Side side, Price price) {
        std::vector<OrderId> ids;
        const Book::Level* level = nullptr;
        if (side == Side::Bid) {
            const auto it = b.bids_.find(price);
            if (it != b.bids_.end()) level = &it->second;
        } else {
            const auto it = b.asks_.find(price);
            if (it != b.asks_.end()) level = &it->second;
        }
        if (level != nullptr) {
            for (const auto& order : level->orders) ids.push_back(order.id);
        }
        return ids;
    }
};

}  // namespace xeas

namespace {

using xeas::BookTestPeer;

// ---------------------------------------------------------------------------
// Task 2: Add + queries
// ---------------------------------------------------------------------------

TEST(AddAndQueries, EmptyBookQueries) {
    Book book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 0);
    EXPECT_EQ(book.size_at(Side::Ask, 1'000'000), 0);
    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.level_count(Side::Bid), 0u);
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
}

TEST(AddAndQueries, SingleBidAdd) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));

    const auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->price, 1'000'000);
    EXPECT_EQ(bb->total_shares, 100);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 100);
    EXPECT_EQ(book.size_at(Side::Bid, 999'999), 0);

    const auto info = book.find_order(1);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->side, Side::Bid);
    EXPECT_EQ(info->price, 1'000'000);
    EXPECT_EQ(info->remaining_qty, 100u);

    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
}

TEST(AddAndQueries, AggregatesLevelsAndBestPriceOrdering) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));  // same level: aggregates
    apply(book, make_add(3, Side::Bid, 990'000, 25));    // worse bid (lower)
    apply(book, make_add(4, Side::Ask, 1'010'000, 75));
    apply(book, make_add(5, Side::Ask, 1'020'000, 10));  // worse ask (higher)

    const auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->price, 1'000'000);  // highest bid is best
    EXPECT_EQ(bb->total_shares, 150);

    const auto ba = book.best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(ba->price, 1'010'000);  // lowest ask is best
    EXPECT_EQ(ba->total_shares, 75);

    EXPECT_EQ(book.size_at(Side::Bid, 990'000), 25);
    EXPECT_EQ(book.size_at(Side::Ask, 1'020'000), 10);
    EXPECT_EQ(book.order_count(), 5u);
    EXPECT_EQ(book.level_count(Side::Bid), 2u);
    EXPECT_EQ(book.level_count(Side::Ask), 2u);
}

TEST(AddAndQueries, TimePriorityFifoEnqueueAtBack) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 10));
    apply(book, make_add(2, Side::Ask, 1'010'000, 20));
    apply(book, make_add(3, Side::Ask, 1'010'000, 30));
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{1, 2, 3}));
}
```

- [ ] 2. RED — build and observe compile failure:

```bash
cmake --build build
```

Expected failure: compile errors in `tests/book_test.cpp` such as `no member named 'best_bid' in 'xeas::Book'` and `use of undeclared identifier 'apply'` (the Task 1 placeholder `Book` is empty).

- [ ] 3. GREEN part 1 — replace the **entire contents** of `include/xeas/book.h` with its final form:

```cpp
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
```

- [ ] 4. GREEN part 2 — replace the **entire contents** of `src/book.cpp` with:

```cpp
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
```

- [ ] 5. GREEN — build and run:

```bash
cmake --build build && ./build/book_test --gtest_filter='AddAndQueries.*:Skeleton.*'
```

Expected: 7 tests pass. Then run the full suite:

```bash
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 7`.

- [ ] 6. Commit:

```bash
git add include/xeas/book.h src/book.cpp tests/book_test.cpp
git commit -m "Task 2: Add events and full query API" -m "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3 — `check_invariants()`: all five spec §6 checks

**Files**
- Modify: `src/book.cpp` (append one function)
- Test: `tests/book_test.cpp` (extend `BookTestPeer`, append tests)

**Interfaces**
- Produces: definition of `std::vector<std::string> check_invariants(const Book& book);` (declared in Task 2's header). Checks, in spec §6 order: (1) best bid < best ask when both sides non-empty; (2) each level's cached `total_shares` equals the sum of its orders' remaining shares; (3) no empty levels, no zero-quantity orders; (4) levels strictly sorted per side (descending bids, ascending asks); (5) order index and levels agree exactly (every level order is indexed with a handle pointing at exactly that node, and the counts match both ways).
- Violation message contract (tests match these substrings — keep them stable): `"crossed"`, `"total_shares"`, `"is empty"`, `"zero-quantity"`, `"missing from index"`, `"disagrees"`, `"index size"`, `"not strictly sorted"`.
- Consumes: `Book` internals via friendship; test corruption via `BookTestPeer`.
- Note: invariant 4 cannot be violated through `std::map` (keys are immutable and sorted by construction); the check exists so Milestone 3's swapped internals are still verified. It is exercised only in the passing direction here.

### Steps

- [ ] 1. RED — in `tests/book_test.cpp`, replace the **entire** `struct BookTestPeer { ... };` definition (inside `namespace xeas`) with:

```cpp
// Test-only backdoor (friend of Book) used to observe FIFO queue order and to
// corrupt internal state so the invariant checker can be genuinely tested.
// Never used by production code.
struct BookTestPeer {
    static std::vector<OrderId> level_order_ids(const Book& b, Side side, Price price) {
        std::vector<OrderId> ids;
        const Book::Level* level = nullptr;
        if (side == Side::Bid) {
            const auto it = b.bids_.find(price);
            if (it != b.bids_.end()) level = &it->second;
        } else {
            const auto it = b.asks_.find(price);
            if (it != b.asks_.end()) level = &it->second;
        }
        if (level != nullptr) {
            for (const auto& order : level->orders) ids.push_back(order.id);
        }
        return ids;
    }

    // --- corruption helpers (invariant-checker tests only) ---
    static void corrupt_total_shares(Book& b, Side side, Price price, std::int64_t v) {
        if (side == Side::Bid) {
            b.bids_.at(price).total_shares = v;
        } else {
            b.asks_.at(price).total_shares = v;
        }
    }

    static void add_empty_level(Book& b, Side side, Price price) {
        if (side == Side::Bid) {
            b.bids_[price];
        } else {
            b.asks_[price];
        }
    }

    static void zero_order_qty(Book& b, OrderId id) {
        b.orders_.at(id).it->remaining = 0;
    }

    static void drop_from_index(Book& b, OrderId id) {
        b.orders_.erase(id);
    }
};
```

- [ ] 2. RED — append to `tests/book_test.cpp`, inside the **second** anonymous namespace (immediately before its closing `}  // namespace` at end of file), exactly:

```cpp
// ---------------------------------------------------------------------------
// Task 3: check_invariants
// ---------------------------------------------------------------------------

testing::AssertionResult book_consistent(const Book& book) {
    const auto violations = check_invariants(book);
    if (violations.empty()) {
        return testing::AssertionSuccess();
    }
    auto result = testing::AssertionFailure();
    for (const auto& v : violations) {
        result << v << "; ";
    }
    return result;
}

bool any_contains(const std::vector<std::string>& violations, std::string_view needle) {
    for (const auto& v : violations) {
        if (v.find(needle) != std::string::npos) return true;
    }
    return false;
}

TEST(Invariants, EmptyBookHasNoViolations) {
    Book book;
    EXPECT_TRUE(book_consistent(book));
}

TEST(Invariants, ConsistentMultiLevelBookHasNoViolations) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    apply(book, make_add(3, Side::Bid, 990'000, 25));
    apply(book, make_add(4, Side::Ask, 1'010'000, 75));
    apply(book, make_add(5, Side::Ask, 1'020'000, 10));
    EXPECT_TRUE(book_consistent(book));
}

TEST(Invariants, CrossedBookIsStoredAsGivenAndFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));  // crosses the bid
    // apply() does NOT match or reject: both orders rest in the book as given.
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(book.size_at(Side::Bid, 1'010'000), 100);
    EXPECT_EQ(book.size_at(Side::Ask, 1'000'000), 100);
    // Only check_invariants flags it.
    EXPECT_TRUE(any_contains(check_invariants(book), "crossed"));
}

TEST(Invariants, LockedBookIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));  // locked: bid == ask
    EXPECT_TRUE(any_contains(check_invariants(book), "crossed"));
}

TEST(Invariants, CorruptedCachedTotalSharesIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::corrupt_total_shares(book, Side::Bid, 1'000'000, 999);
    EXPECT_TRUE(any_contains(check_invariants(book), "total_shares"));
}

TEST(Invariants, EmptyLevelIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    BookTestPeer::add_empty_level(book, Side::Ask, 1'020'000);
    EXPECT_TRUE(any_contains(check_invariants(book), "is empty"));
}

TEST(Invariants, ZeroQuantityOrderIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    BookTestPeer::zero_order_qty(book, 1);
    EXPECT_TRUE(any_contains(check_invariants(book), "zero-quantity"));
}

TEST(Invariants, IndexLevelDisagreementIsFlagged) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    BookTestPeer::drop_from_index(book, 2);
    const auto violations = check_invariants(book);
    EXPECT_TRUE(any_contains(violations, "missing from index"));
    EXPECT_TRUE(any_contains(violations, "index size"));
}
```

- [ ] 3. RED — build and observe link failure:

```bash
cmake --build build
```

Expected failure: linker error for the test binary, e.g. `Undefined symbols ... xeas::check_invariants(xeas::Book const&)` (declared in the header since Task 2, never defined).

- [ ] 4. GREEN — append to `src/book.cpp`, inside `namespace xeas` (immediately before the final `}  // namespace xeas` line), exactly:

```cpp
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
```

- [ ] 5. GREEN — build and run:

```bash
cmake --build build && ./build/book_test --gtest_filter='Invariants.*'
```

Expected: 8 tests pass. Full suite:

```bash
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 15`.

- [ ] 6. Commit:

```bash
git add src/book.cpp tests/book_test.cpp
git commit -m "Task 3: check_invariants consistency checker (all 5 spec invariants)" -m "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4 — `Cancel` + `Execute` (identical book effect)

**Files**
- Modify: `src/book.cpp` (add `Book::erase_order` definition; replace `apply()`)
- Test: `tests/book_test.cpp` (append)

**Interfaces**
- Produces: `Cancel`/`Execute` semantics in `apply()` — reduce the order's remaining shares by `event.quantity`; reaching zero removes the order immediately; removing a level's last order removes the level immediately. `Execute` shares the exact code path with `Cancel` (spec: same book effect; distinct type kept for later trade stats only). Also produces `void Book::erase_order(OrderIndex::iterator index_it)` (declared in Task 2's header).
- Consumes: `Book::insert_order`, order index, `check_invariants` (in tests, via `book_consistent` from Task 3).
- No validation yet (Task 7): tests here feed only valid events.

### Steps

- [ ] 1. RED — append to `tests/book_test.cpp`, inside the second anonymous namespace (immediately before its closing `}  // namespace` at end of file), exactly:

```cpp
// ---------------------------------------------------------------------------
// Task 4: Cancel + Execute
// ---------------------------------------------------------------------------

Event make_cancel(OrderId id, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Cancel;
    e.timestamp = ts;
    e.order_id = id;
    e.quantity = qty;
    return e;
}

Event make_execute(OrderId id, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Execute;
    e.timestamp = ts;
    e.order_id = id;
    e.quantity = qty;
    return e;
}

TEST(CancelExecute, PartialCancelReducesOrderAndLevel) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_cancel(1, 30));

    const auto info = book.find_order(1);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->remaining_qty, 70u);
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 70);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(CancelExecute, FullCancelRemovesOrderKeepsLevelWithOthers) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    apply(book, make_cancel(1, 100));  // reaches zero: removed immediately

    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 50);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Bid, 1'000'000),
              (std::vector<OrderId>{2}));
    EXPECT_TRUE(book_consistent(book));
}

TEST(CancelExecute, CancelLastOrderRemovesLevel) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    apply(book, make_cancel(1, 100));

    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.level_count(Side::Ask), 0u);  // no empty levels ever
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 0);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(CancelExecute, MidQueueRemovalPreservesFifoOfOthers) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 10));
    apply(book, make_add(2, Side::Ask, 1'010'000, 20));
    apply(book, make_add(3, Side::Ask, 1'010'000, 30));
    apply(book, make_cancel(2, 20));

    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 40);
    EXPECT_TRUE(book_consistent(book));
}

TEST(CancelExecute, ExecuteHasSameBookEffectAsCancel) {
    Book cancel_book;
    Book exec_book;
    for (Book* b : {&cancel_book, &exec_book}) {
        apply(*b, make_add(1, Side::Ask, 1'010'000, 100));
        apply(*b, make_add(2, Side::Ask, 1'010'000, 50));
    }
    apply(cancel_book, make_cancel(1, 40));
    apply(exec_book, make_execute(1, 40));  // execution price never affects book state

    EXPECT_EQ(cancel_book.size_at(Side::Ask, 1'010'000),
              exec_book.size_at(Side::Ask, 1'010'000));
    EXPECT_EQ(cancel_book.find_order(1)->remaining_qty,
              exec_book.find_order(1)->remaining_qty);
    EXPECT_EQ(cancel_book.order_count(), exec_book.order_count());
    EXPECT_EQ(cancel_book.level_count(Side::Ask), exec_book.level_count(Side::Ask));
    EXPECT_TRUE(book_consistent(exec_book));
}

TEST(CancelExecute, PartialThenFullExecuteRemovesFrontOrder) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'010'000, 50));

    apply(book, make_execute(1, 40));
    EXPECT_EQ(book.find_order(1)->remaining_qty, 60u);
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 110);
    EXPECT_TRUE(book_consistent(book));

    apply(book, make_execute(1, 60));
    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{2}));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 50);
    EXPECT_TRUE(book_consistent(book));
}
```

- [ ] 2. RED — build and run; observe assertion failures (Cancel/Execute are still no-ops):

```bash
cmake --build build && ./build/book_test --gtest_filter='CancelExecute.*'
```

Expected failure: all 6 `CancelExecute` tests fail with value mismatches, e.g. `Expected equality of these values: info->remaining_qty ... 70u` but actual `100`.

- [ ] 3. GREEN part 1 — in `src/book.cpp`, add the `erase_order` definition immediately after the `Book::insert_order` definition:

```cpp
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
```

- [ ] 4. GREEN part 2 — in `src/book.cpp`, replace the **entire** `void apply(Book& book, const Event& event) { ... }` function with:

```cpp
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
    case EventType::Delete:
        break;  // implemented in Task 5
    case EventType::Replace:
        break;  // implemented in Task 6
    }
}
```

- [ ] 5. GREEN — build and run:

```bash
cmake --build build && ./build/book_test --gtest_filter='CancelExecute.*'
```

Expected: 6 tests pass. Full suite:

```bash
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 21`.

- [ ] 6. Commit:

```bash
git add src/book.cpp tests/book_test.cpp
git commit -m "Task 4: Cancel and Execute events (shared removal path, level cleanup)" -m "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 5 — `Delete`

**Files**
- Modify: `src/book.cpp` (replace the `Delete` case in `apply()`)
- Test: `tests/book_test.cpp` (append)

**Interfaces**
- Produces: `Delete` semantics in `apply()` — remove the order entirely whatever its remaining quantity; `event.quantity` and `event.price` are ignored.
- Consumes: `Book::erase_order` from Task 4.

### Steps

- [ ] 1. RED — append to `tests/book_test.cpp`, inside the second anonymous namespace (before its closing `}  // namespace`), exactly:

```cpp
// ---------------------------------------------------------------------------
// Task 5: Delete
// ---------------------------------------------------------------------------

Event make_delete(OrderId id, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Delete;
    e.timestamp = ts;
    e.order_id = id;
    return e;
}

TEST(DeleteEvent, DeleteRemovesOrderWithRemainingShares) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 1'000'000, 50));
    apply(book, make_delete(1));  // 100 shares still remaining: removed anyway

    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 50);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(DeleteEvent, DeleteIgnoresQuantityAndPrice) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));

    Event e;
    e.type = EventType::Delete;
    e.order_id = 1;
    e.quantity = 999'999;  // ignored
    e.price = -5;          // ignored (price validation applies to Add/Replace only)
    apply(book, e);

    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(DeleteEvent, DeleteLastOrderRemovesLevelAndUpdatesBbo) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 990'000, 25));
    apply(book, make_delete(1));

    const auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->price, 990'000);  // BBO fell to the next level
    EXPECT_EQ(bb->total_shares, 25);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 0);
    EXPECT_TRUE(book_consistent(book));
}
```

- [ ] 2. RED — build and run; observe failures (Delete is a no-op):

```bash
cmake --build build && ./build/book_test --gtest_filter='DeleteEvent.*'
```

Expected failure: all 3 tests fail, e.g. `Value of: book.find_order(1).has_value()  Actual: true  Expected: false`.

- [ ] 3. GREEN — in `src/book.cpp`, inside `apply()`, replace the two lines

```cpp
    case EventType::Delete:
        break;  // implemented in Task 5
```

with:

```cpp
    case EventType::Delete: {
        // Remove entirely, whatever the remaining quantity; event.quantity and
        // event.price are ignored by definition (spec section 2).
        const auto index_it = book.orders_.find(event.order_id);
        book.erase_order(index_it);
        break;
    }
```

- [ ] 4. GREEN — build and run:

```bash
cmake --build build && ./build/book_test --gtest_filter='DeleteEvent.*'
```

Expected: 3 tests pass. Full suite:

```bash
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 24`.

- [ ] 5. Commit:

```bash
git add src/book.cpp tests/book_test.cpp
git commit -m "Task 5: Delete events (unconditional removal, quantity/price ignored)" -m "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 6 — `Replace`

**Files**
- Modify: `src/book.cpp` (replace the `Replace` case in `apply()`)
- Test: `tests/book_test.cpp` (append)

**Interfaces**
- Produces: `Replace` semantics in `apply()` — atomically remove order `order_id` and insert order `new_order_id` with `event.quantity` shares at `event.price`, on the **same side as the removed order** (resolved via lookup — `event.side` is meaningless on Replace), at the **back** of its (possibly new) level's queue. Time priority is lost even when the price is unchanged (ITCH semantics). Quantity is the event's quantity, never carried over.
- Consumes: `Book::erase_order`, `Book::insert_order`.

### Steps

- [ ] 1. RED — append to `tests/book_test.cpp`, inside the second anonymous namespace (before its closing `}  // namespace`), exactly:

```cpp
// ---------------------------------------------------------------------------
// Task 6: Replace
// ---------------------------------------------------------------------------

Event make_replace(OrderId id, OrderId new_id, Price price, Qty qty, Timestamp ts = 0) {
    Event e;
    e.type = EventType::Replace;
    e.timestamp = ts;
    e.order_id = id;
    // Deliberately leaves e.side at its default: the book must resolve the
    // side from the replaced order, never from the event.
    e.price = price;
    e.quantity = qty;
    e.new_order_id = new_id;
    return e;
}

TEST(ReplaceEvent, SamePriceLosesTimePriority) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    apply(book, make_add(2, Side::Ask, 1'010'000, 50));
    apply(book, make_replace(1, 10, 1'010'000, 100));  // same price, same qty

    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{2, 10}));  // re-enqueued at the back
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 150);
    EXPECT_TRUE(book_consistent(book));
}

TEST(ReplaceEvent, PriceMoveCreatesAndRemovesLevels) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));  // alone at its level
    apply(book, make_add(2, Side::Bid, 990'000, 25));
    apply(book, make_replace(1, 10, 980'000, 40));

    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 0);  // old level removed
    EXPECT_EQ(book.size_at(Side::Bid, 980'000), 40);   // new level created
    const auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->price, 990'000);
    EXPECT_EQ(book.level_count(Side::Bid), 2u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(ReplaceEvent, KeepsSideFromBookNotEvent) {
    Book book;
    apply(book, make_add(1, Side::Ask, 1'010'000, 100));
    // make_replace leaves event.side == Side::Bid (default); the book must
    // still keep the order on the Ask side.
    apply(book, make_replace(1, 10, 1'020'000, 60));

    const auto info = book.find_order(10);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->side, Side::Ask);
    EXPECT_EQ(info->price, 1'020'000);
    EXPECT_EQ(info->remaining_qty, 60u);  // event quantity, not carried over
    EXPECT_EQ(book.level_count(Side::Bid), 0u);
    EXPECT_TRUE(book_consistent(book));
}

TEST(ReplaceEvent, OldIdGoneNewIdPresentCountsStable) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_replace(1, 2, 1'000'000, 70));

    EXPECT_FALSE(book.find_order(1).has_value());
    ASSERT_TRUE(book.find_order(2).has_value());
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 70);
    EXPECT_TRUE(book_consistent(book));
}
```

- [ ] 2. RED — build and run; observe failures (Replace is a no-op):

```bash
cmake --build build && ./build/book_test --gtest_filter='ReplaceEvent.*'
```

Expected failure: all 4 tests fail, e.g. FIFO expectation `{2, 10}` vs actual `{1, 2}`.

- [ ] 3. GREEN — in `src/book.cpp`, inside `apply()`, replace the two lines

```cpp
    case EventType::Replace:
        break;  // implemented in Task 6
```

with:

```cpp
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
```

- [ ] 4. GREEN — build and run:

```bash
cmake --build build && ./build/book_test --gtest_filter='ReplaceEvent.*'
```

Expected: 4 tests pass. Full suite:

```bash
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 28`.

- [ ] 5. Commit:

```bash
git add src/book.cpp tests/book_test.cpp
git commit -m "Task 6: Replace events (atomic remove+insert, same side, priority loss)" -m "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 7 — Strict error policy: `BookError` on every spec §4 condition

**Files**
- Modify: `src/book.cpp` (replace `apply()` with the final validated version)
- Test: `tests/book_test.cpp` (append)

**Interfaces**
- Produces: full spec §4 validation in `apply()`, throwing `xeas::BookError` (defined header-only since Task 2) carrying the offending `Event` and a reason string. Conditions, each with its own test:
  - `Cancel`/`Delete`/`Execute`/`Replace` referencing an unknown `order_id`
  - `Add` with an `order_id` that already exists
  - `Replace` with a `new_order_id` that already exists, **including `new_order_id == order_id`**
  - `Cancel`/`Execute` with `quantity` of zero, or exceeding the order's remaining shares
  - `Add`/`Replace` with zero `quantity` or non-positive `price`
- Per design decision 7, every branch validates **all** conditions before mutating anything.
- Consumes: everything from Tasks 2–6.

### Steps

- [ ] 1. RED — append to `tests/book_test.cpp`, inside the second anonymous namespace (before its closing `}  // namespace`), exactly:

```cpp
// ---------------------------------------------------------------------------
// Task 7: strict error policy (spec section 4) — one test per bullet
// ---------------------------------------------------------------------------

TEST(ErrorPolicy, CancelUnknownOrderThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_cancel(999, 10)), BookError);
}

TEST(ErrorPolicy, ExecuteUnknownOrderThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_execute(999, 10)), BookError);
}

TEST(ErrorPolicy, DeleteUnknownOrderThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_delete(999)), BookError);
}

TEST(ErrorPolicy, ReplaceUnknownOrderThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_replace(999, 1000, 1'000'000, 10)), BookError);
}

TEST(ErrorPolicy, AddDuplicateOrderIdThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_add(1, Side::Ask, 1'010'000, 50)), BookError);
}

TEST(ErrorPolicy, ReplaceExistingNewOrderIdThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Bid, 990'000, 50));
    EXPECT_THROW(apply(book, make_replace(1, 2, 1'000'000, 10)), BookError);
}

TEST(ErrorPolicy, ReplaceNewOrderIdEqualsOrderIdThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_replace(1, 1, 1'000'000, 10)), BookError);
}

TEST(ErrorPolicy, CancelZeroQuantityThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_cancel(1, 0)), BookError);
}

TEST(ErrorPolicy, ExecuteZeroQuantityThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_execute(1, 0)), BookError);
}

TEST(ErrorPolicy, CancelQuantityExceedingRemainingThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_cancel(1, 101)), BookError);
}

TEST(ErrorPolicy, ExecuteQuantityExceedingRemainingThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_execute(1, 60));  // remaining 40
    EXPECT_THROW(apply(book, make_execute(1, 41)), BookError);
}

TEST(ErrorPolicy, AddZeroQuantityThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_add(1, Side::Bid, 1'000'000, 0)), BookError);
}

TEST(ErrorPolicy, AddZeroPriceThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_add(1, Side::Bid, 0, 100)), BookError);
}

TEST(ErrorPolicy, AddNegativePriceThrows) {
    Book book;
    EXPECT_THROW(apply(book, make_add(1, Side::Bid, -1'000'000, 100)), BookError);
}

TEST(ErrorPolicy, ReplaceZeroQuantityThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_replace(1, 2, 1'000'000, 0)), BookError);
}

TEST(ErrorPolicy, ReplaceNonPositivePriceThrows) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_THROW(apply(book, make_replace(1, 2, 0, 10)), BookError);
    EXPECT_THROW(apply(book, make_replace(1, 2, -7, 10)), BookError);
}

TEST(ErrorPolicy, BookErrorCarriesOffendingEventAndReason) {
    Book book;
    const Event bad = make_cancel(777, 10);
    try {
        apply(book, bad);
        FAIL() << "expected BookError";
    } catch (const BookError& err) {
        EXPECT_EQ(err.event().type, EventType::Cancel);
        EXPECT_EQ(err.event().order_id, 777u);
        EXPECT_EQ(err.event().quantity, 10u);
        EXPECT_NE(std::string(err.what()), "");
    }
}
```

- [ ] 2. RED — build and run; observe failures:

```bash
cmake --build build && ./build/book_test --gtest_filter='ErrorPolicy.*'
```

Expected failure: tests fail with `Expected: ... throws an exception of type BookError. Actual: it throws nothing` — **or the test binary crashes** on the unknown-id cases (the unvalidated implementation dereferences an end iterator, which is undefined behavior). Either outcome is a valid RED; do not attempt to fix by any means other than step 3.

- [ ] 3. GREEN — in `src/book.cpp`, replace the **entire** `void apply(Book& book, const Event& event) { ... }` function with the final validated version:

```cpp
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
        auto& h = index_it->second;
        if (event.quantity > h.it->remaining) {
            throw BookError(event, std::string(name) + ": quantity exceeds remaining shares");
        }
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
        // All validation passed above; neither call below can fail.
        const Side side = index_it->second.side;
        book.erase_order(index_it);
        book.insert_order(side, event.price, event.new_order_id, event.quantity);
        break;
    }
    }
}
```

- [ ] 4. GREEN — build and run:

```bash
cmake --build build && ./build/book_test --gtest_filter='ErrorPolicy.*'
```

Expected: 17 tests pass. Full suite (proves validation broke no happy path):

```bash
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 45`.

- [ ] 5. Commit:

```bash
git add src/book.cpp tests/book_test.cpp
git commit -m "Task 7: strict error policy - BookError on all malformed events" -m "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 8 — Event-tape scenario tests (spec §7 test strategy)

**Files**
- Test: `tests/book_test.cpp` (append; no production code changes)

**Interfaces**
- Produces: scripted event-tape tests asserting queries **and** `check_invariants()` emptiness after **every** event, covering: full add/cancel/delete lifecycle; partial executes sweeping a level in FIFO order; replace with price move and priority loss; level creation/removal; BBO updates on both sides.
- Consumes: the complete `apply()`/query/invariant API from Tasks 2–7.

### Steps

- [ ] 1. Append to `tests/book_test.cpp`, inside the second anonymous namespace (before its closing `}  // namespace`), exactly:

```cpp
// ---------------------------------------------------------------------------
// Task 8: event-tape scenarios — queries AND consistency after EVERY event
// ---------------------------------------------------------------------------

// Applies the event and asserts full-book consistency at the call site
// (a macro so ASSERT aborts the enclosing TEST, not a helper function).
#define APPLY_CHECKED(book, event)                \
    do {                                          \
        apply((book), (event));                   \
        ASSERT_TRUE(book_consistent((book)));     \
    } while (0)

TEST(EventTapes, AddCancelDeleteLifecycle) {
    Book book;

    APPLY_CHECKED(book, make_add(1, Side::Bid, 1'000'000, 100, 1));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);
    EXPECT_EQ(book.order_count(), 1u);

    APPLY_CHECKED(book, make_add(2, Side::Ask, 1'010'000, 80, 2));
    EXPECT_EQ(book.best_ask()->price, 1'010'000);
    EXPECT_EQ(book.best_ask()->total_shares, 80);

    APPLY_CHECKED(book, make_cancel(1, 25, 3));
    EXPECT_EQ(book.find_order(1)->remaining_qty, 75u);
    EXPECT_EQ(book.best_bid()->total_shares, 75);

    APPLY_CHECKED(book, make_execute(2, 30, 4));
    EXPECT_EQ(book.find_order(2)->remaining_qty, 50u);
    EXPECT_EQ(book.best_ask()->total_shares, 50);

    APPLY_CHECKED(book, make_delete(1, 5));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.level_count(Side::Bid), 0u);

    APPLY_CHECKED(book, make_execute(2, 50, 6));
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
}

TEST(EventTapes, PartialExecutesSweepLevelInFifoOrder) {
    Book book;

    APPLY_CHECKED(book, make_add(1, Side::Ask, 1'010'000, 100, 1));
    APPLY_CHECKED(book, make_add(2, Side::Ask, 1'010'000, 50, 2));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 150);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{1, 2}));

    APPLY_CHECKED(book, make_execute(1, 40, 3));
    EXPECT_EQ(book.find_order(1)->remaining_qty, 60u);
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 110);

    APPLY_CHECKED(book, make_execute(1, 60, 4));
    EXPECT_FALSE(book.find_order(1).has_value());
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Ask, 1'010'000),
              (std::vector<OrderId>{2}));
    EXPECT_EQ(book.size_at(Side::Ask, 1'010'000), 50);

    APPLY_CHECKED(book, make_execute(2, 50, 5));
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(EventTapes, ReplacePriceMoveAndPriorityLoss) {
    Book book;

    APPLY_CHECKED(book, make_add(1, Side::Bid, 1'000'000, 100, 1));
    APPLY_CHECKED(book, make_add(2, Side::Bid, 1'000'000, 50, 2));
    APPLY_CHECKED(book, make_add(3, Side::Bid, 990'000, 30, 3));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);
    EXPECT_EQ(book.best_bid()->total_shares, 150);

    // Price move down: order 1 leaves the best level, joins 990'000 at the back.
    APPLY_CHECKED(book, make_replace(1, 4, 990'000, 100, 4));
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 50);
    EXPECT_EQ(book.size_at(Side::Bid, 990'000), 130);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Bid, 990'000),
              (std::vector<OrderId>{3, 4}));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);

    // Replacing the last order at the best level removes that level; BBO falls.
    APPLY_CHECKED(book, make_replace(2, 5, 990'000, 25, 5));
    EXPECT_EQ(book.size_at(Side::Bid, 1'000'000), 0);
    EXPECT_EQ(book.level_count(Side::Bid), 1u);
    EXPECT_EQ(book.best_bid()->price, 990'000);
    EXPECT_EQ(book.best_bid()->total_shares, 155);
    EXPECT_EQ(BookTestPeer::level_order_ids(book, Side::Bid, 990'000),
              (std::vector<OrderId>{3, 4, 5}));
    EXPECT_EQ(book.order_count(), 3u);
}

TEST(EventTapes, LevelChurnAndBboUpdatesBothSides) {
    Book book;

    APPLY_CHECKED(book, make_add(1, Side::Bid, 990'000, 10, 1));
    EXPECT_EQ(book.best_bid()->price, 990'000);

    APPLY_CHECKED(book, make_add(2, Side::Bid, 1'000'000, 20, 2));
    EXPECT_EQ(book.best_bid()->price, 1'000'000);  // better bid becomes BBO
    EXPECT_EQ(book.level_count(Side::Bid), 2u);

    APPLY_CHECKED(book, make_add(3, Side::Ask, 1'020'000, 30, 3));
    EXPECT_EQ(book.best_ask()->price, 1'020'000);

    APPLY_CHECKED(book, make_add(4, Side::Ask, 1'010'000, 40, 4));
    EXPECT_EQ(book.best_ask()->price, 1'010'000);  // better ask becomes BBO
    EXPECT_EQ(book.level_count(Side::Ask), 2u);

    APPLY_CHECKED(book, make_delete(2, 5));
    EXPECT_EQ(book.best_bid()->price, 990'000);  // BBO falls back
    EXPECT_EQ(book.level_count(Side::Bid), 1u);

    APPLY_CHECKED(book, make_execute(4, 40, 6));
    EXPECT_EQ(book.best_ask()->price, 1'020'000);  // BBO rises back
    EXPECT_EQ(book.level_count(Side::Ask), 1u);

    APPLY_CHECKED(book, make_cancel(3, 5, 7));
    EXPECT_EQ(book.best_ask()->total_shares, 25);

    APPLY_CHECKED(book, make_delete(3, 8));
    EXPECT_FALSE(book.best_ask().has_value());

    APPLY_CHECKED(book, make_delete(1, 9));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.level_count(Side::Bid), 0u);
    EXPECT_EQ(book.level_count(Side::Ask), 0u);
}
```

- [ ] 2. RED-check — build and run the new tests:

```bash
cmake --build build && ./build/book_test --gtest_filter='EventTapes.*'
```

Expected: **all 4 pass immediately** — this task adds integration coverage over already-implemented behavior, so a genuine RED is not expected. If ANY of these tests fails, that is a real bug in Tasks 2–7: STOP, use superpowers:systematic-debugging, and fix the implementation (not the test) before proceeding.

- [ ] 3. Full suite:

```bash
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 49`.

- [ ] 4. Commit:

```bash
git add tests/book_test.cpp
git commit -m "Task 8: event-tape scenario tests with per-event invariant checks" -m "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Verification (run after all tasks)

From the repository root `/Users/glebmokeev/xeas-exchange`, on branch `feature/order-book-core`:

```bash
rm -rf build
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected output:
1. Configure succeeds, printing the CMake generate step and fetching/configuring GoogleTest v1.17.0 (network download on the clean configure).
2. Build compiles `src/book.cpp` and `tests/book_test.cpp` with **zero warnings** under `-Wall -Wextra` and links `libxeas_core` and `book_test`.
3. ctest ends with exactly:
   ```
   100% tests passed, 0 tests failed out of 49
   ```
   (49 = 3 Skeleton + 4 AddAndQueries + 8 Invariants + 6 CancelExecute + 3 DeleteEvent + 4 ReplaceEvent + 17 ErrorPolicy + 4 EventTapes.)

Also verify directly:

```bash
./build/book_test
```

Expected: `[  PASSED  ] 49 tests.`

Spec-conformance checklist (reviewer sanity pass):
- [ ] File layout matches spec §7 exactly (`include/xeas/event.h`, `include/xeas/book.h`, `src/book.cpp`, `tests/book_test.cpp`, `CMakeLists.txt`; library target `xeas_core`).
- [ ] All `Event` fields fixed-width integers; no floating point anywhere in the tree (`grep -rn "float\|double" include src` returns nothing).
- [ ] `apply` is a free `void` function; queries are `const`; `check_invariants` is never called from `apply`.
- [ ] Bids `std::map<Price, Level, std::greater<>>`, asks `std::map<Price, Level, std::less<>>`; cached `total_shares`; `unordered_map` order index.
- [ ] Execute == Cancel book effect; Delete ignores quantity/price; Replace keeps side, loses priority; crossed Adds stored as-is and flagged only by `check_invariants`.
- [ ] All eight §4 error bullets throw `BookError` carrying the event and a reason.
- [ ] git log shows 8 task commits on `feature/order-book-core`.

### Critical Files for Implementation
- `docs/superpowers/specs/2026-08-15-order-book-core-design.md` (the spec)
- `include/xeas/event.h`, `include/xeas/book.h`, `src/book.cpp`, `tests/book_test.cpp`, `CMakeLists.txt` (all created by this plan)
