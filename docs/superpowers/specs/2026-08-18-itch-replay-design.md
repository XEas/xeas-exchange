# ITCH Replay — Design Spec

**Date:** 2026-08-18
**Milestone:** 2 (see `DESIGN.md`)
**Status:** Approved decisions from design Q&A; ready for implementation planning.

## 1. Purpose & scope

Milestone 2 makes the Milestone 1 book core consume real data: decode a full
NASDAQ TotalView-ITCH 5.0 historical day from disk and replay it through one
`Book` per symbol, verifying invariants along the way. The book core API is
frozen; everything here sits in front of it.

**In scope**
- Framing reader for the ITCH historical file format (`ItchReader`)
- Fused ITCH decoder+normalizer producing canonical `Event`s (`decode_book_message`)
- Symbol router: one `Book` per stock locate (`BookRouter`)
- Replay driver with stats and invariant sweeps (`replay_stream`)
- `itch_replay <file>` CLI
- One additive book-core function: `bool is_crossed(const Book&)`
- Synthetic-binary-fixture test suite; no data files checked in

**Out of scope** (later milestones or never)
- Gzip input — NASDAQ distributes `*.NASDAQ_ITCH50.gz`; the user runs `gunzip`
  first. The CLI usage text says so.
- MoldUDP64 / live network feeds (Milestone 4) — this spec covers the historical
  BinaryFILE dump format only
- Partial-day files — Cancels/Executes reference earlier Adds, so input must be a
  complete day from byte 0; a mid-day start correctly aborts on unknown order ids
- Benchmarks and optimized internals (Milestone 3)

**Scale expectations (informative):** a busy day is ~300–450M messages, ~13 GB
uncompressed. With the Milestone 1 baseline internals, expect roughly 1–4 GiB
peak RSS at a few million concurrent live orders. That footprint is Milestone 3's
target, not a leak.

## 2. ITCH 5.0 file & message format

The historical dump ("BinaryFILE") is a flat sequence of `[u16 length][body]`
frames. The length is big-endian, as is every integer field in every body. A
`0x0000` length is a clean end-of-stream sentinel (present at the tail of some
NASDAQ dumps), counted in stats — not an error.

Every body starts with a common header: message type (1 byte), stock locate
(`u16`), tracking number (`u16`, ignored — MoldUDP diagnostics), timestamp
(6 bytes, ns since midnight, widened to the `Event`'s `u64`). Prices are 4-byte
fixed point ×10,000 — exactly the Milestone 1 `Price` tick convention, widened
to `int64_t`.

Decoded message set (body lengths are fixed per type and validated):

| Type | Body | Meaning | → `Event` |
|---|---|---|---|
| `A` | 36 | Add Order | `Add` |
| `F` | 40 | Add Order with MPID | `Add` (attribution dropped) |
| `X` | 23 | Order Cancel (partial) | `Cancel` |
| `D` | 19 | Order Delete | `Delete` |
| `E` | 31 | Order Executed | `Execute` |
| `C` | 36 | Order Executed with Price | `Execute` (printable flag and execution price do not affect the book) |
| `U` | 35 | Order Replace | `Replace` (`new_order_id`, new shares, new price) |
| `R` | 39 | Stock Directory | not an `Event`; feeds locate→symbol names for reporting |

`U` carries no side byte; the book resolves side from the old order — this
matches Milestone 1, where `Event.side` is meaningful on `Add` only. Do not
"fix" this.

All other well-framed types (`S`, `H`, `P`, `Q`, `I`, …) are counted by type
byte and skipped — the length prefix makes skipping free, and unknown future
types are handled identically (forward compatibility, not an error).

**Rationale (fused decode+normalize):** `DESIGN.md` draws `Decoder → Normalizer`
as two boxes. For ITCH the normalizer is nearly the identity (`'B'/'S'` → `Side`,
drop MPID, drop printable flag, widen price/timestamp), so a typed per-message
struct layer would be pure transcription: two names for the same bytes, double
the tests, an extra copy on what becomes Milestone 3's hot path. The split
survives as a *knowledge boundary* — `itch.cpp` is the only code in the repo
that knows ITCH byte layouts, and its output is the canonical `Event`. A future
second venue adds its own `decode_*` producing the same `Event`. This amends
`DESIGN.md`'s diagram to `ITCH file → [Decoder/Normalizer] → Event → Book`.

## 3. Components & interfaces

New files: `include/xeas/itch.h` + `src/itch.cpp`, `include/xeas/router.h` +
`src/router.cpp`, `include/xeas/replay.h` + `src/replay.cpp`,
`src/itch_replay_main.cpp`. All in namespace `xeas`, matching Milestone 1's
style: plain structs, free functions where no state is owned, strict errors via
exceptions carrying context.

**Framing & decoding (`itch.h`):**

```cpp
struct FramedMessage {            // view into the reader's buffer;
    const std::byte* data;        // valid only until the next next()
    std::size_t size;             // body size; data[0] is the type byte
};

class ItchReader {
public:
    explicit ItchReader(const std::filesystem::path& path,
                        std::size_t buffer_size = 1 << 20);  // throws FeedError
    explicit ItchReader(std::istream& in,
                        std::size_t buffer_size = 1 << 20);  // for tests
    std::optional<FramedMessage> next();  // nullopt at clean EOF; throws FeedError
    std::uint64_t bytes_consumed() const noexcept;
    std::uint64_t messages_framed() const noexcept;
};

constexpr bool is_book_message(char type) noexcept;   // A F X D E C U

struct BookMessage { std::uint16_t stock_locate; Event event; };
BookMessage decode_book_message(const std::byte* data, std::size_t size);

struct StockDirectoryEntry { std::uint16_t stock_locate; std::string symbol; };
StockDirectoryEntry decode_stock_directory(const std::byte* data, std::size_t size);
```

**Router (`router.h`):**

```cpp
class BookRouter {
public:
    Book& book_for(std::uint16_t stock_locate);          // lazily creates
    const Book* find(std::uint16_t stock_locate) const;  // nullptr if never seen
    void set_symbol(std::uint16_t locate, std::string symbol);   // from 'R'
    std::string symbol_or_locate(std::uint16_t locate) const;    // "AAPL" or "locate 42"
    std::size_t book_count() const noexcept;
    void for_each_book(const std::function<void(std::uint16_t, const Book&)>&) const;
};
```

Internals: a 65,536-slot `std::vector<std::unique_ptr<Book>>` (locate is `u16`;
locates are dense, day-scoped small integers) plus a parallel symbol-name
vector. O(1) routing, no hashing; heap-allocated books give pointer stability
for free. No global order-id→locate map is needed: every ITCH message carries
its own stock locate, and order references are unique day-wide. There is no
ordering dependency on `R` messages — books are created lazily by locate, and
symbol names are cosmetic (reports fall back to `"locate N"`).

**Replay driver (`replay.h`)** — a library function so it is testable; the CLI
is a thin shell around it:

```cpp
struct ReplayOptions {
    std::uint64_t invariant_sweep_every = 25'000'000;  // messages between sweeps
    std::uint64_t progress_every = 10'000'000;         // 0 = silent
};

struct ReplayStats {
    std::uint64_t messages_total = 0;
    std::array<std::uint64_t, 256> count_by_type{};    // indexed by type byte
    std::uint64_t events_applied = 0;
    std::uint64_t bytes_consumed = 0;
    std::uint64_t books_created = 0;
    std::uint64_t crossed_episodes = 0;                // false→true transitions
    std::uint64_t crossed_symbols = 0;                 // distinct books ever crossed
    std::uint64_t timestamp_regressions = 0;
    std::uint64_t live_orders_peak = 0;
    std::uint64_t live_orders_final = 0;
    std::uint64_t invariant_sweeps = 0;
    double elapsed_seconds = 0.0;
};

void replay_stream(ItchReader& reader, BookRouter& router,
                   const ReplayOptions& opts, ReplayStats& stats,
                   std::ostream& log);
```

**Rationale:** `stats` is an out-parameter, not a return value, so that on an
abort the CLI still prints everything counted up to the failure point — the
single most useful debugging artifact.

## 4. Replay driver semantics

Per framed message:

1. Count `count_by_type[type]`, bytes, total.
2. `R` → `decode_stock_directory` → `router.set_symbol`.
3. Book message → `decode_book_message` → `router.book_for(locate)` →
   `apply(book, event)`; then an O(1) crossed probe on the touched book only:
   `is_crossed(book)` against a per-locate was-crossed bit, counting an episode
   on each false→true transition.
4. Track live orders incrementally (`order_count()` delta on the touched book)
   for `live_orders_peak`.
5. A timestamp lower than its predecessor increments `timestamp_regressions` —
   reported, never fatal (the book never interprets timestamps).
6. Anything else: skipped (already counted).

**Invariant sweeps:** every `invariant_sweep_every` messages plus one final
sweep at EOF, running `check_invariants` on every book. Findings prefixed
`"book crossed or locked"` are counted and logged (symbol + timestamp on first
occurrence per book) — **reportable, not fatal**. Any other violation throws
`ReplayError` naming locate, symbol, and all violation strings.

**Rationale:** real ITCH days legitimately produce crossed/locked books around
trading halts and the opening/closing crosses. This refines the Milestone 1
spec's §3 assumption ("with well-formed exchange data this does not occur"):
`check_invariants` itself is unchanged, but the replay driver must classify the
crossed finding as data reality, not a bug. Structural violations (level sums,
empty levels, index mismatches) still mean a bug somewhere — fail loud.

To keep that classification O(1) on the hot path and untied to violation prose,
Milestone 2 adds one function to the book core (`book.h`/`book.cpp`, additive
only): `bool is_crossed(const Book&)` — true iff both sides are non-empty and
best bid ≥ best ask.

## 5. Error policy — strict, layered

Three exception types, all deriving from `std::runtime_error`, each carrying
the context available at its layer:

| Stage | Condition | Throws |
|---|---|---|
| Reader | file unopenable | `FeedError` |
| Reader | EOF inside a length prefix or body | `FeedError` (truncated) |
| Reader | length prefix > 512 | `FeedError` (implausible length — the largest real ITCH 5.0 body is 50 bytes; this fails fast on desynced, MoldUDP, or garbage input instead of silently skipping gigabytes) |
| Decoder | wrong body length for a known type | `FeedError` |
| Decoder | side byte not `'B'`/`'S'` | `FeedError` |
| Decoder | unknown well-framed type | **not an error** — counted and skipped |
| Book | all semantic violations (unknown/duplicate id, over-execute, …) | `BookError`, unchanged from Milestone 1 |
| Driver | structural invariant violation in a sweep | `ReplayError` |

```cpp
class FeedError : public std::runtime_error {   // itch.h
public:
    FeedError(std::string reason, std::uint64_t byte_offset,
              std::uint64_t message_index, char message_type /* '\0' if n/a */);
    // accessors: byte_offset(), message_index(), message_type()
};

class ReplayError : public std::runtime_error { // replay.h
public:
    ReplayError(std::uint16_t locate, std::string symbol,
                std::vector<std::string> violations, std::uint64_t message_index);
    // accessors
};
```

**Rationale:** `BookError` carries an `Event`; feed errors happen before an
`Event` exists and their useful context is positional (byte offset, message
index, type byte). Different payloads, different layers, shared base so the CLI
catch is uniform. The decoder does **not** pre-validate semantics — form belongs
to the decoder, meaning belongs to the book. All three exception types abort
the replay (stats printed first), per the Milestone 1 rule: after a throw, book
state is unspecified.

## 6. Streaming strategy

Plain buffered sequential reads: one `std::vector<std::byte>` buffer (1 MiB
default); `next()` returns zero-copy views into it; a frame straddling the
buffer end is `memmove`d to the front before refilling (bodies are ≤ 50 bytes,
so compaction is trivial and rare). No mmap — a sequential scan through a small
buffer saturates any SSD via OS readahead, and mmap buys nothing but SIGBUS
handling. Correctness-first; Milestone 3 measures before optimizing. The
`buffer_size` constructor parameter exists so tests can force the straddle path
with a tiny buffer.

## 7. CLI

```
itch_replay <file> [--sweep-every N] [--progress N] [--quiet]
```

Input is an uncompressed `.NASDAQ_ITCH50` file (gunzip first). Output: per-type
message count table (known types labeled), totals, books created / symbols
named, events applied, crossed episodes and distinct crossed symbols, timestamp
regressions, final live orders and levels (via `for_each_book`), elapsed wall
time, msgs/sec and MiB/s.

Exit codes: `0` success (crossed findings included), `1` usage or unopenable
file, `2` `FeedError`, `3` `BookError`, `4` `ReplayError`. On 2–4, print the
exception's full context (offset, message index, type, symbol, offending
`Event` fields for `BookError`) plus stats-so-far to stderr.

Golden replay of a real day is a manual workflow this milestone: run the CLI on
a user-supplied file, record the stats block once, compare on later runs. No
data files or golden files are checked into the repo.

## 8. Project skeleton & testing

```
include/xeas/itch.h        — FeedError, FramedMessage, ItchReader, decode functions
include/xeas/router.h      — BookRouter
include/xeas/replay.h      — ReplayOptions, ReplayStats, ReplayError, replay_stream()
src/itch.cpp, src/router.cpp, src/replay.cpp
src/itch_replay_main.cpp   — CLI (thin: argv, catch, print, exit code)
tests/itch_fixture.h       — test-only binary fixture builder (ItchBuilder)
tests/itch_test.cpp        — Framing / Decode / Router suites
tests/replay_test.cpp      — Replay / Differential suites
```

CMake: a second library `add_library(xeas_feed src/itch.cpp src/router.cpp
src/replay.cpp)` linking `PUBLIC xeas_core`, same warning flags; executable
`itch_replay`; test binaries `itch_test` and `replay_test` via
`gtest_discover_tests`.

**Rationale:** a separate `xeas_feed` target enforces the layering at link
level — the book core never sees feed or I/O code, and Milestone 3 benchmarks
can link book-only.

**Test strategy** (TDD, ~40 tests, mirroring Milestone 1's tape style):

1. **Fixture builder** — `ItchBuilder` in `tests/itch_fixture.h`: one method
   per message type (`add_order`, `execute`, `replace`, `stock_directory`, …)
   writing big-endian fields with automatic length prefixes, plus `raw()` and
   `truncate_last(n)` for malformed frames; `bytes()` feeds an
   `std::istringstream` to `ItchReader`. The builder is an independently
   written inverse of the decoder — that is what gives round-trip tests teeth.
2. **`Framing`** (~8) — empty stream; single/multiple frames; straddle with a
   tiny `buffer_size`; truncated prefix/body throw with correct offsets;
   `0x0000` sentinel is clean EOF; implausible length throws; counters exact.
3. **`Decode`** (~15) — one test per book type asserting *every* `Event` field
   (48-bit timestamp assembly, big-endian refs, side mapping, price widening,
   `F` drops MPID, `C` ignores printable+price, `U` populates `new_order_id`);
   wrong body length per type throws; bad side byte throws; `R` trims padding;
   `is_book_message` classification table.
4. **`Router`** (~5) — lazy creation; same locate → same `Book&`; distinct
   locates isolated; `symbol_or_locate` before/after `R`; `book_count`.
5. **`Replay`** (~12) — multi-symbol happy tape (final state via router
   queries, exact per-type counts); unknown types counted and skipped; crossed
   tape completes without throwing and counts episodes (cross–uncross–cross =
   2); `BookError` propagates with stats intact up to the failure; truncated
   stream mid-tape throws `FeedError`; sweep cadence via a tiny
   `invariant_sweep_every` (checker correctness itself is Milestone 1's job);
   timestamp regressions; `live_orders_peak`.
6. **`Differential`** (~3) — the end-to-end check with zero real data: express
   the same logical tape twice, once as binary via `ItchBuilder` →
   `replay_stream`, once as a `std::vector<Event>` applied directly to
   hand-routed `Book`s; every query (`best_bid/ask`, `size_at`, `order_count`,
   `level_count`, `find_order`) must agree across all books.

The CLI is smoke-tested manually against a real day; its logic lives in
`replay_stream`, which the suites above cover.
