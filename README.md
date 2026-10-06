# xeas-exchange

A single-threaded **limit order book reconstruction engine** in C++20. It
replays NASDAQ TotalView-ITCH 5.0 order-level events (add, cancel, delete,
execute, replace) and maintains an exact, full-depth replica of every book at
every point in the day.

The book is pure state and the event stream is the only source of truth, so
replaying the same events always produces the same book.

**Headline numbers** (Apple M5, Release build, details in [BENCH.md](BENCH.md)):

| | |
|---|---|
| Full trading day replayed | 268.7M messages, 8,892 books, in 38.0 s |
| End-to-end throughput | 7.08M msgs/s, 1.62x the baseline internals |
| Book core alone | 25.8M events/s (38.7 ns/event), 5.29x the baseline |
| Tests | 147 across unit, differential, and fuzz suites |

## Quick start

Requires CMake 3.24 or newer and a C++20 compiler. The first configure
downloads GoogleTest v1.17.0, so it needs network access.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The project sets no default build type, so pass `-DCMAKE_BUILD_TYPE=Release`
for anything you intend to time.

### Replay a trading day

NASDAQ publishes sample ITCH days at `emi.nasdaq.com/ITCH/Nasdaq ITCH/`.
Decompress one first, since `itch_replay` reads the raw `.NASDAQ_ITCH50` file.

```bash
gunzip 12302019.NASDAQ_ITCH50.gz
./build/itch_replay 12302019.NASDAQ_ITCH50 --quiet --sweep-every 0
```

| Flag | Meaning | Default |
|---|---|---|
| `--sweep-every N` | Messages between full invariant sweeps (`0` runs only the final sweep) | 25,000,000 |
| `--progress N` | Messages between progress lines | 10,000,000 |
| `--quiet` | Suppress progress lines | off |

It prints message counts by type, then totals: events applied, books created,
crossed episodes, peak live orders, elapsed time, throughput, and peak RSS.
Stats gathered so far are printed even when the run fails.

| Exit code | Meaning |
|---|---|
| 0 | Success |
| 1 | Usage error or unopenable file |
| 2 | Feed error (truncation, malformed frame or body) |
| 3 | Book error (malformed event) |
| 4 | Replay error (structural invariant violation) |

### Run the microbenchmark

```bash
./build/book_bench --workload all --events 10000000 --warmup 1000000 --seed 1 --repeat 5
```

`book_bench` drives the book directly with seeded synthetic tapes, with no
feed code linked in. Workloads are `steady`, `insert`, and `deep`; `--impl
new|baseline|both` selects which implementation runs.

## Architecture

```
ITCH file ──▶ ItchReader ──▶ decode ──▶ Event ──▶ BookRouter ──▶ Book ──▶ queries / stats
              (framing)      (normalize)          (per locate)   (apply)
```

| Component | Files | Role |
|---|---|---|
| `Event` | `include/xeas/event.h` | Canonical event struct and the hard boundary between feed handlers and the book. Integer ticks (ITCH fixed point ×10,000), no floating point. |
| `Book` | `include/xeas/book.h`, `src/book.cpp` | Single-instrument L3 (market-by-order) book with strict FIFO time priority within a level. Mutated only through `apply(Book&, const Event&)`. |
| ITCH feed | `include/xeas/itch.h`, `src/itch.cpp` | Frame reader for `[u16 length][body]` big-endian dumps, fused with the decoder. `A F X D E C U` become events, `R` feeds the locate-to-symbol directory, and everything else is counted and skipped. |
| `BookRouter` | `include/xeas/router.h`, `src/router.cpp` | One lazily created `Book` per stock locate in a flat 65,536-slot table. |
| Replay driver | `include/xeas/replay.h`, `src/replay.cpp` | `replay_stream()` with per-type stats, live-order tracking, timestamp-regression counting, and periodic invariant sweeps. |

Layering is enforced at link time: `xeas_core` (the book) never sees feed or
I/O code, and `xeas_feed` builds on top of it.

### Book internals

The public `book.h` API was frozen after the first milestone and the internals
were swapped behind it, one measured step at a time:

- **Order pool with intrusive lists** for per-level FIFO queues, replacing
  `std::list` nodes.
- **Banded flat price array with bitmaps** for O(1) level access near the
  touch, with an exact `std::map` overflow for far-out prices.
- **Open-addressing order index** for id lookup, the largest single win.

Queries (`best_bid()`, `best_ask()`, `size_at()`, `find_order()`,
`order_count()`, `level_count()`) are `const` and allocation-free.

### Error policy

Malformed input fails loudly. Replay of well-formed data is deterministic, so
a violation means a bug upstream.

- `BookError` for a malformed event (unknown id, duplicate id, over-cancel,
  zero quantity), carrying the offending event.
- `FeedError` for truncation, implausible lengths, and malformed bodies.
- `ReplayError` for structural invariant violations.

Crossed and locked books are treated as data reality (halts, auction crosses):
counted and reported, never fatal.

`check_invariants(book)` is an out-of-band consistency checker (book not
crossed, aggregates match, no empty levels, sides sorted, index and levels
agree). It never runs on the hot path.

## Performance

Measured on an Apple M5 in a Release build, single-threaded, median of 5.

**Book core** (`book_bench`, 10M events, optimized vs. baseline internals):

| Workload | Baseline Mmsg/s | Optimized Mmsg/s | Speedup |
|---|---|---|---|
| `steady` | 4.89 | 25.83 | 5.29x |
| `insert` | 5.55 | 27.30 | 4.92x |
| `deep` | 2.97 | 6.04 | 2.04x |

**Full day** (`itch_replay` on NASDAQ's Dec 30, 2019 sample, 7.69 GiB,
invariant sweeps off):

| Internals | Wall time | msgs/s | Peak RSS |
|---|---|---|---|
| Baseline (`std::map` + `std::list`) | 61.7 s | 4.36M | 272 MiB |
| Optimized | 38.0 s | 7.08M | ~5 GiB |

Two findings from the full-day run, both documented in [BENCH.md](BENCH.md):

- **The feed side dominates wall time.** The book accounts for roughly a
  quarter of end-to-end time; framing, decode, and routing take the rest.
- **Peak RSS regressed about 18x.** Each book carries fixed-capacity
  structures sized for one hot book, and a real day multiplies that floor
  across 8,892 books. Per-book sizing is the open follow-up.

With the default periodic invariant sweeps enabled, the optimized internals
measure 0.86x the baseline end-to-end, because a sweep over the band arrays is
more expensive than one over node-based structures. BENCH.md has the full
methodology, per-phase numbers, and caveats.

## Testing

147 GoogleTest cases across four binaries:

| Binary | Tests | Covers |
|---|---|---|
| `book_test` | 57 | Event tapes, queries, error policy, invariants |
| `itch_test` | 36 | Framing, decode, routing, on synthetic binary fixtures |
| `replay_test` | 15 | Replay driver, plus binary-vs-direct-`Event` differential runs |
| `book_fuzz_test` | 39 | Seeded differential fuzzing against the frozen baseline, plus pool, band, and order-index unit tests |

No market data is checked in. Feed tests build ITCH binaries in memory with
`tests/itch_fixture.h`, and the fuzz suites use the seeded `TapeGen` in
`bench/tape_gen.h`. The original `std::map`/`std::list` book is kept as
`BaselineBook` (`xeas_baseline`), an oracle that every rewrite of the
internals must agree with.

```bash
./build/book_test --gtest_filter='EventTapes.*'   # run a subset directly
```

## Project layout

```
include/xeas/     public headers: event, book, itch, router, replay
src/              book core, ITCH feed, replay driver, itch_replay CLI
bench/            book_bench, TapeGen, frozen BaselineBook oracle
tests/            GoogleTest suites and the ITCH fixture builder
docs/superpowers/ per-milestone design specs and implementation plans
DESIGN.md         scope, architecture, key decisions
BENCH.md          benchmark methodology and full results
```

## Status

| Milestone | State |
|---|---|
| 1. Book core: data structures, `apply()`, queries, invariant checker | Done |
| 2. ITCH replay: decode and replay a real trading day | Done |
| 3. Benchmark and optimize: measured swap to fast internals | Done |
| 4. Live mode: sequencer and network feed on the same core | Planned, optional |

Out of scope: matching and order entry (this system consumes events, it does
not match) and multi-threading.
