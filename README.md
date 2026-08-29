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
| 2. ITCH replay — decode and replay a real trading day | ✅ done |
| 3. Benchmark & optimize — measured swap to fast internals | ✅ done |
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

## What's here (Milestone 2)

- `include/xeas/itch.h`, `src/itch.cpp` — framing reader for the ITCH 5.0
  historical dump (`[u16 length][body]` frames, big-endian) and the fused
  decoder+normalizer: `A F X D E C U` become canonical `Event`s, `R` feeds
  locate→symbol names, everything else is counted and skipped. Strict
  `FeedError` on truncation, implausible lengths, malformed bodies.
- `include/xeas/router.h`, `src/router.cpp` — `BookRouter`: one lazily-created
  `Book` per stock locate (flat 65,536-slot table, O(1) routing).
- `include/xeas/replay.h`, `src/replay.cpp` — `replay_stream()`: the replay
  driver with per-type stats, live-order tracking, timestamp-regression
  counting, periodic invariant sweeps. Crossed/locked books are classified as
  data reality (halts, auction crosses) — counted, reported, never fatal;
  structural violations throw `ReplayError`.
- `itch_replay` CLI — `itch_replay <file> [--sweep-every N] [--progress N]
  [--quiet]` on an uncompressed `.NASDAQ_ITCH50` file (gunzip first). Exit
  codes: 0 ok, 1 usage/unopenable, 2 feed error, 3 book error, 4 replay error;
  stats-so-far always printed.
- Tests use synthetic binary fixtures (`tests/itch_fixture.h`); no data files
  are checked in. A differential suite replays the same logical tape as binary
  and as direct `Event`s and requires every book query to agree.

## What's here (Milestone 3)

- **Milestone 3 — benchmark & optimize:** pooled intrusive levels, banded
  bitmap price array, open-addressing order index behind the frozen `book.h`
  API; before/after numbers per optimization in [BENCH.md](BENCH.md). Measured
  steady-state replay workload ~5.29x over the Milestone 1 baseline book
  (`book_bench --workload steady`, phase 4, `new` vs. `baseline`); a
  `BaselineBook` oracle (`xeas_baseline`) and a differential fuzz harness keep
  every rewrite honest against the original `std::map`/`std::list`
  internals. The full-day macro run (NASDAQ's public Dec 30, 2019 sample
  day, 268.7M messages) measures **1.62x end-to-end** (7.08 vs. 4.36 Mmsg/s,
  invariant sweeps off) and shows the feed side, not the book, dominating
  wall time — plus an ~18x peak-RSS regression from per-book fixed-capacity
  structures multiplied across 8,892 books. Numbers, the sweep-cost
  decomposition, and both findings are in `BENCH.md`.

## Build & test

Requires CMake ≥ 3.24 and a C++20 compiler. The first configure fetches
GoogleTest v1.17.0 (network needed).

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 108`.

```bash
./build/book_test --gtest_filter='EventTapes.*'   # run a subset directly
```

## Layout

```
include/xeas/event.h    Event, Side, EventType, type aliases (Price, Qty, OrderId)
include/xeas/book.h     Book, apply(), queries, check_invariants(), BookError
include/xeas/itch.h     FeedError, FramedMessage, ItchReader, decode functions
include/xeas/router.h   BookRouter (one Book per stock locate)
include/xeas/replay.h   ReplayOptions, ReplayStats, ReplayError, replay_stream()
src/book.cpp            implementation
src/itch.cpp            framing + fused ITCH decoder/normalizer
src/router.cpp          locate routing
src/replay.cpp          replay driver
src/itch_replay_main.cpp  the itch_replay CLI
tests/book_test.cpp     event-tape unit tests (57 tests)
tests/itch_fixture.h    ItchBuilder — synthetic ITCH binary fixtures
tests/itch_test.cpp     Framing / Decode / Router suites (36 tests)
tests/replay_test.cpp   Replay / Differential suites (15 tests)
docs/superpowers/       design spec and executed implementation plan
```
