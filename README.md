# xeas-exchange

A single-threaded, high-performance **order book reconstruction engine** in C++20.

It rebuilds the full limit order book of an exchange by consuming a stream of
order-level events (add, cancel, replace, execute) and maintaining an exact
in-memory replica of the book at every point in time. Primary data source:
historical **NASDAQ TotalView-ITCH** files, replayed from disk.

The book is pure state, the event stream is the single source of truth, and
replaying the same events always produces the same book — a deterministic,
event-sourced state machine. See [DESIGN.md](DESIGN.md) for the full design.

## Status

| Milestone | State |
|---|---|
| 1. Book core — data structures, `apply()`, queries, invariant checker | ✅ done |
| 2. ITCH replay — decode and replay a real trading day | planned |
| 3. Benchmark & optimize — measured swap to fast internals | planned |
| 4. (Optional) live mode | planned |

## What's here (Milestone 1)

- `include/xeas/event.h` — the canonical `Event` struct (`Add` / `Cancel` /
  `Delete` / `Execute` / `Replace`), the hard boundary between feed handlers
  and the book. Fixed-width integers everywhere; prices in ticks
  (ITCH fixed point ×10,000); no floating point.
- `include/xeas/book.h`, `src/book.cpp` — the single-instrument **L3
  (market-by-order)** `Book`: price levels best-first per side, strict FIFO
  time priority within a level, cached level aggregates, O(1)-amortized
  order lookup. Mutated only via the free function `apply(Book&, const Event&)`.
- **Queries** — `best_bid()` / `best_ask()`, `size_at()`, `find_order()`,
  `order_count()`, `level_count()`; all `const`, allocation-free,
  O(1) or O(log levels).
- **Strict error policy** — any malformed event (unknown id, duplicate id,
  over-cancel, zero quantity, …) throws `BookError` carrying the offending
  event. Replay of well-formed data is deterministic, so a violation means a
  bug upstream: fail loud.
- **`check_invariants(book)`** — out-of-band consistency checker (book not
  crossed, aggregates match, no empty levels, sides sorted, index and levels
  agree). Never called on the hot path; unit tests call it after every event.

Current internals are the deliberately simple, correctness-first baseline
(`std::map` + `std::list`); Milestone 3 swaps them behind the same API with
before/after numbers.

## Build & test

Requires CMake ≥ 3.24 and a C++20 compiler. The first configure fetches
GoogleTest v1.17.0 (network needed).

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 52`.

```bash
./build/book_test --gtest_filter='EventTapes.*'   # run a subset directly
```

## Layout

```
include/xeas/event.h   Event, Side, EventType, type aliases (Price, Qty, OrderId)
include/xeas/book.h    Book, apply(), queries, check_invariants(), BookError
src/book.cpp           implementation
tests/book_test.cpp    event-tape unit tests (52 tests)
docs/superpowers/      design spec and executed implementation plan
```
