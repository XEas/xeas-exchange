# ITCH Replay (Milestone 2) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use **superpowers:subagent-driven-development** (chosen by the user) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Each task is self-contained: write the failing test, RUN it and observe the stated failure (RED), write the implementation, RUN again and observe pass (GREEN), commit. Do not skip RED steps. Do not deviate from the code given here.

## Context

xeas-exchange (see `DESIGN.md`) is a single-threaded order book reconstruction engine in C++20. Milestone 1 (merged to `main` via PR #3) delivered the frozen book core: canonical `Event` (`include/xeas/event.h`), `Book` + `apply()` + `check_invariants()` + `BookError` (`include/xeas/book.h`, `src/book.cpp`), 52 tests.

This plan implements **Milestone 2: ITCH replay** — decode a full NASDAQ TotalView-ITCH 5.0 historical day from disk and replay it through one `Book` per symbol. Everything sits *in front of* the frozen book core, except one additive function (`is_crossed`).

**Goal:** `ItchReader` (framing), `decode_book_message`/`decode_stock_directory` (fused decoder+normalizer), `BookRouter` (one lazily-created `Book` per stock locate), `replay_stream` (driver with stats + invariant sweeps), the `itch_replay` CLI, and a synthetic-binary-fixture test suite (~54 new tests). No data files checked in.

**Spec (this plan implements it EXACTLY):** `docs/superpowers/specs/2026-08-18-itch-replay-design.md`
**Project design:** `DESIGN.md` (amended by Task 11)

**Architecture:**

```
ITCH file → ItchReader (framing) → decode_book_message → Event → BookRouter → Book
                                          └── decode_stock_directory ('R') → symbol names
                     replay_stream drives the loop, counts stats, sweeps invariants
```

- `include/xeas/itch.h` + `src/itch.cpp` — `FeedError`, `FramedMessage`, `ItchReader`, `is_book_message`, `decode_book_message`, `decode_stock_directory`. The ONLY code in the repo that knows ITCH byte layouts (fused decoder+normalizer per spec §2 rationale).
- `include/xeas/router.h` + `src/router.cpp` — `BookRouter`: 65,536-slot `std::vector<std::unique_ptr<Book>>` (O(1) routing by locate, pointer-stable books) + parallel symbol-name vector.
- `include/xeas/replay.h` + `src/replay.cpp` — `ReplayOptions`, `ReplayStats`, `ReplayError`, `replay_stream()` (library function so it is testable; the CLI is a thin shell).
- `src/itch_replay_main.cpp` — CLI: argv parsing, exception → exit-code mapping, stats printing.
- `tests/itch_fixture.h` — `ItchBuilder`: test-only binary fixture builder, an independently written inverse of the decoder.
- `tests/itch_test.cpp` — Classify / Fixture / Errors / Framing / Decode / Router suites.
- `tests/replay_test.cpp` — Replay / Differential suites.
- One additive change to the frozen core: `bool is_crossed(const Book&)` in `book.h`/`book.cpp` (O(1), via existing public queries — no new friend).

**Design decisions baked in (spec-conformant; ambiguities resolved here):**
1. `FeedError` gets a `reason()` accessor (raw reason, no position baked in) in addition to the spec's accessors. The decode functions have no stream position, so they throw with `byte_offset = 0, message_index = 0` and the type byte set; `replay_stream` catches and **re-throws enriched** with the reader's position (frame start = `bytes_consumed() - 2 - body_size`). Layering preserved: decoder knows form, driver knows position.
2. `ItchReader` clamps `buffer_size` up to `2 + 512` bytes (one max frame must fit whole, since `next()` returns views). Straddle tests therefore use tapes longer than 514 bytes, not sub-frame buffers.
3. The `0x0000` sentinel: its 2 bytes count in `bytes_consumed`, it does NOT count in `messages_framed` (it has no type byte), and it is terminal — bytes after it are ignored, subsequent `next()` returns `nullopt`.
4. `timestamp_regressions` is tracked across decoded book events (the only messages whose timestamps we decode). Equal timestamps are not regressions.
5. `invariant_sweep_every = 0` means final-sweep-only (the final EOF sweep always runs). `progress_every = 0` means silent.
6. `live_orders_peak/final` tracked incrementally via `order_count()` delta on the touched book (signed accumulator).
7. Crossed classification: the per-message O(1) probe (`is_crossed` on the touched book vs a per-locate was-crossed bit) counts `crossed_episodes` (false→true transitions) and `crossed_symbols` (distinct locates ever crossed). Sweeps classify `check_invariants` findings by the prefix `"book crossed or locked"` (exact prefix produced by Milestone 1's checker at `src/book.cpp:175`): those are logged once per book (`symbol + last event timestamp`) and never fatal; any other finding throws `ReplayError`.
8. A structural `ReplayError` cannot be triggered through well-formed replay (by design), so replay tests cover sweep *cadence* and crossed *classification*; `ReplayError` itself is tested via direct construction (checker correctness is Milestone 1's job, per spec §8.5).
9. CLI "symbols named" is reported as directory entries = `count_by_type['R']` (router API is frozen per spec; no extra accessor added).
10. Unopenable file maps to exit 1 (per spec §7) even though it throws `FeedError` — the CLI constructs the reader in its own try-block.

**Tech Stack:** C++20, CMake ≥ 3.24, GoogleTest v1.17.0 via `FetchContent`, ctest. New library target `xeas_feed` (links `PUBLIC xeas_core`), executable `itch_replay`, test binaries `itch_test`, `replay_test`.

## Global Constraints (from spec — apply to every task)

- **C++20**, no compiler extensions; `-Wall -Wextra` clean on non-MSVC.
- **No floating point** except `ReplayStats::elapsed_seconds` and CLI throughput printing (spec-mandated fields).
- All integers in ITCH are **big-endian**; prices are 4-byte fixed point ×10,000 widened to `int64_t`; timestamps are 6-byte ns-since-midnight widened to `uint64_t`.
- Namespace `xeas` for production code; `xeas::test` for the fixture builder. Plain structs, free functions where no state is owned, strict errors via exceptions carrying context.
- The book core API is **frozen**: Milestone 2 may only ADD `is_crossed` — never modify `Event`, `Book`, `apply`, `check_invariants`, `BookError`.
- `Event.side` is meaningful on `Add` only; `U` (Replace) carries no side byte and the book resolves side from the old order. **Do not "fix" this.**
- The decoder does NOT pre-validate semantics — form belongs to the decoder, meaning belongs to the book.
- Unknown well-framed message types are counted and skipped — forward compatibility, not an error.
- Frame length prefix > 512 → `FeedError` (largest real ITCH 5.0 body is 50 bytes).
- Layering at link level: `xeas_core` never sees feed or I/O code.
- No data files or golden files checked into the repo; tests use synthetic binary fixtures only.
- Strict TDD: never write implementation before seeing the task's tests fail. Commit after every green task.
- All shell commands run from the repo root `/Users/glebmokeev/xeas-exchange`. Build dir `build/`.

**Test execution conventions:**
- Configure (re-runs automatically on CMake changes): `cmake -S . -B build`
- Build: `cmake --build build`
- Full suite: `ctest --test-dir build --output-on-failure`
- One suite directly: `./build/itch_test --gtest_filter='Framing.*'`

**Before Task 1:**

```bash
git checkout main && git pull
git checkout -b feature/itch-replay
# Save this plan into the repo (superpowers convention):
cp /Users/glebmokeev/.claude/plans/execute-on-docs-superpowers-specs-2026-0-wild-backus.md \
   docs/superpowers/plans/2026-08-19-itch-replay.md
git add docs/superpowers/plans/2026-08-19-itch-replay.md
git commit -m "docs: add ITCH replay implementation plan (Milestone 2)"
git push -u origin feature/itch-replay
```

**Push policy (user requirement): push after EVERY task commit — commit and push as you go, never batch pushes at the end.**

---

## ITCH 5.0 byte-layout reference (used by Tasks 2 and 4 — the ONLY two places that may know these offsets)

All integers big-endian. Common body header: `type` (1 byte, offset 0), `stock_locate` (u16, offset 1), `tracking_number` (u16, offset 3, ignored), `timestamp` (6 bytes, offset 5). Per-type fields start at offset 11:

| Type | Body | Fields from offset 11 |
|---|---|---|
| `A` Add Order | 36 | order ref u64@11, side char@19 (`'B'`/`'S'`), shares u32@20, stock 8 alpha@24, price u32@32 |
| `F` Add Order MPID | 40 | as `A`, plus attribution 4 alpha@36 (dropped) |
| `X` Order Cancel | 23 | order ref u64@11, canceled shares u32@19 |
| `D` Order Delete | 19 | order ref u64@11 |
| `E` Order Executed | 31 | order ref u64@11, executed shares u32@19, match u64@23 (ignored) |
| `C` Executed w/ Price | 36 | as `E`, plus printable char@31 (ignored), price u32@32 (ignored) |
| `U` Order Replace | 35 | old ref u64@11, new ref u64@19, shares u32@27, price u32@31 |
| `R` Stock Directory | 39 | stock 8 alpha@11 (space-padded); remaining 20 bytes of directory attributes ignored |

---

## Task 1 — `is_crossed(const Book&)` (additive book-core function)

**Files:**
- Modify: `include/xeas/book.h` (add one declaration after `check_invariants` at line 102)
- Modify: `src/book.cpp` (add one definition at the end, before the closing namespace)
- Test: `tests/book_test.cpp` (append a new `IsCrossed` suite)

**Interfaces:**
- Consumes: `Book`, `apply()`, `Event` factories — `tests/book_test.cpp` already has `Event make_add(OrderId id, Side side, Price price, Qty qty, Timestamp ts = 0)` in its anonymous namespace (line 58).
- Produces: `bool is_crossed(const Book& book);` in namespace `xeas` — Tasks 7–8 call it per message.

- [ ] **Step 1: Write the failing tests** — append to the end of the anonymous-namespace test section of `tests/book_test.cpp` (after the last existing TEST, keeping `make_add` visible; if the file's anonymous namespace closed earlier, place these in the trailing test area where other `TEST`s live and use fully qualified helpers as the existing tests do):

```cpp
// ---------------------------------------------------------------------------
// Milestone 2 additive: is_crossed() — O(1) crossed/locked probe
// ---------------------------------------------------------------------------

TEST(IsCrossed, EmptyBookIsNotCrossed) {
    Book book;
    EXPECT_FALSE(is_crossed(book));
}

TEST(IsCrossed, OneSidedBookIsNotCrossed) {
    Book bids_only;
    apply(bids_only, make_add(1, Side::Bid, 1'000'000, 100));
    EXPECT_FALSE(is_crossed(bids_only));

    Book asks_only;
    apply(asks_only, make_add(2, Side::Ask, 1'000'000, 100));
    EXPECT_FALSE(is_crossed(asks_only));
}

TEST(IsCrossed, NormalSpreadIsNotCrossed) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'100, 100));
    EXPECT_FALSE(is_crossed(book));
}

TEST(IsCrossed, LockedBookIsCrossed) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'000, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));
    EXPECT_TRUE(is_crossed(book));
}

TEST(IsCrossed, CrossedBookIsCrossed) {
    Book book;
    apply(book, make_add(1, Side::Bid, 1'000'100, 100));
    apply(book, make_add(2, Side::Ask, 1'000'000, 100));
    EXPECT_TRUE(is_crossed(book));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/book_test --gtest_filter='IsCrossed.*'`
Expected: BUILD FAILURE — `is_crossed` not declared. (A compile error IS the red state here.)

- [ ] **Step 3: Declare and implement.** In `include/xeas/book.h`, immediately after the `check_invariants` declaration (line 102):

```cpp
// True iff both sides are non-empty and best bid >= best ask (locked or
// crossed). O(1). Milestone 2's replay driver probes this after every event
// to classify crossed/locked books as data reality (halts, opening/closing
// crosses) without string-matching check_invariants() output.
bool is_crossed(const Book& book);
```

In `src/book.cpp`, before the closing `}  // namespace xeas`:

```cpp
// --- is_crossed ------------------------------------------------------------

bool is_crossed(const Book& book) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    return bid.has_value() && ask.has_value() && bid->price >= ask->price;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build && ./build/book_test`
Expected: all 57 tests PASS (52 existing + 5 new).

- [ ] **Step 5: Commit**

```bash
git add include/xeas/book.h src/book.cpp tests/book_test.cpp
git commit -m "feat: add is_crossed() O(1) probe to book core (Milestone 2 additive)"
git push
```

---

## Task 2 — `xeas_feed` skeleton: complete `itch.h`, `ItchBuilder` fixture, CMake targets

**Files:**
- Create: `include/xeas/itch.h` (COMPLETE final form — later tasks implement, never re-declare)
- Create: `src/itch.cpp` (initially just includes the header)
- Create: `tests/itch_fixture.h` (complete `ItchBuilder`)
- Create: `tests/itch_test.cpp` (Classify / Fixture / Errors suites)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `xeas::Event` from `include/xeas/event.h` (fields: `type`, `timestamp`, `order_id`, `side`, `price`, `quantity`, `new_order_id`).
- Produces (for Tasks 3–10): everything declared in `itch.h` below, and `xeas::test::ItchBuilder` with methods `add_order`, `add_order_mpid`, `order_cancel`, `order_delete`, `order_executed`, `order_executed_price`, `order_replace`, `stock_directory`, `other`, `raw`, `sentinel`, `truncate_last`, `bytes()`, `stream()`.

- [ ] **Step 1: Write `include/xeas/itch.h` in its complete final form:**

```cpp
#pragma once

#include "xeas/event.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace xeas {

// Thrown on any feed-level failure: unopenable file, truncated stream,
// implausible frame length, malformed body (spec section 5). Feed errors
// happen before an Event exists, so the context is positional: byte offset of
// the frame start, 0-based message index, and the type byte ('\0' if not
// known). The decode_* functions have no stream position; they throw with
// offset and index 0, and replay_stream() re-throws enriched with the
// reader's position.
class FeedError : public std::runtime_error {
public:
    FeedError(std::string reason, std::uint64_t byte_offset,
              std::uint64_t message_index, char message_type = '\0')
        : std::runtime_error(reason + " (byte offset " + std::to_string(byte_offset) +
                             ", message index " + std::to_string(message_index) +
                             (message_type != '\0'
                                  ? std::string(", type '") + message_type + "'"
                                  : std::string()) +
                             ")"),
          reason_(std::move(reason)),
          byte_offset_(byte_offset),
          message_index_(message_index),
          message_type_(message_type) {}

    const std::string& reason() const noexcept { return reason_; }  // no position
    std::uint64_t byte_offset() const noexcept { return byte_offset_; }
    std::uint64_t message_index() const noexcept { return message_index_; }
    char message_type() const noexcept { return message_type_; }

private:
    std::string reason_;
    std::uint64_t byte_offset_;
    std::uint64_t message_index_;
    char message_type_;
};

// View into ItchReader's internal buffer; valid only until the next next().
struct FramedMessage {
    const std::byte* data;  // data[0] is the message type byte
    std::size_t size;       // body size in bytes (the u16 length prefix's value)
};

// Framing reader for the ITCH 5.0 historical dump ("BinaryFILE"): a flat
// sequence of [u16 big-endian length][body] frames (spec section 2). Plain
// buffered sequential reads; next() returns zero-copy views into the buffer;
// a frame straddling the buffer end is compacted to the front before
// refilling. buffer_size is clamped up so one maximum frame always fits.
class ItchReader {
public:
    // Throws FeedError if the file cannot be opened.
    explicit ItchReader(const std::filesystem::path& path,
                        std::size_t buffer_size = 1 << 20);
    // For tests: reads from a caller-owned stream (e.g. std::istringstream).
    explicit ItchReader(std::istream& in, std::size_t buffer_size = 1 << 20);

    // Next frame, or nullopt at clean EOF (end of data at a frame boundary,
    // or a 0x0000 sentinel length — terminal, bytes after it are ignored).
    // Throws FeedError on truncation or an implausible (> 512) length.
    std::optional<FramedMessage> next();

    std::uint64_t bytes_consumed() const noexcept { return bytes_consumed_; }
    std::uint64_t messages_framed() const noexcept { return messages_framed_; }

private:
    void refill();  // compact the unread tail to the front, then read more
    std::size_t available() const noexcept { return fill_ - pos_; }

    std::ifstream owned_file_;  // used by the path constructor only
    std::istream* in_;          // points at owned_file_ or the caller's stream
    std::vector<std::byte> buffer_;
    std::size_t pos_ = 0;       // next unread byte in buffer_
    std::size_t fill_ = 0;      // one past the last valid byte in buffer_
    bool eof_ = false;          // underlying stream is exhausted
    bool done_ = false;         // 0x0000 sentinel seen
    std::uint64_t bytes_consumed_ = 0;
    std::uint64_t messages_framed_ = 0;
};

// The seven message types that mutate a book (spec section 2 table).
constexpr bool is_book_message(char type) noexcept {
    return type == 'A' || type == 'F' || type == 'X' || type == 'D' ||
           type == 'E' || type == 'C' || type == 'U';
}

struct BookMessage {
    std::uint16_t stock_locate;
    Event event;
};

// Fused decoder+normalizer: one framed body (data[0] = type byte, size = the
// body length) to a canonical Event. A non-book type throws. Throws FeedError
// on wrong body length for the type or a side byte that is not 'B'/'S'. Does
// NOT validate semantics (unknown ids, over-execute, ...): form belongs here,
// meaning belongs to the book.
BookMessage decode_book_message(const std::byte* data, std::size_t size);

struct StockDirectoryEntry {
    std::uint16_t stock_locate;
    std::string symbol;  // trailing spaces trimmed
};

// Decodes an 'R' (Stock Directory) body; only locate and symbol are kept.
StockDirectoryEntry decode_stock_directory(const std::byte* data, std::size_t size);

}  // namespace xeas
```

- [ ] **Step 2: Write `src/itch.cpp` (placeholder TU so the library links; Tasks 3–5 fill it):**

```cpp
#include "xeas/itch.h"

// ItchReader and the decode functions are implemented in Milestone 2
// Tasks 3-5 (test-first). This TU exists so xeas_feed builds from Task 2 on.
```

- [ ] **Step 3: Write `tests/itch_fixture.h` (complete):**

```cpp
#pragma once

// Test-only ITCH 5.0 binary fixture builder. Written as an INDEPENDENT
// inverse of the decoder, straight from the ITCH 5.0 field tables — never
// call decoder helpers here, or round-trip tests lose their teeth.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace xeas::test {

class ItchBuilder {
public:
    // 'A' Add Order, body 36.
    ItchBuilder& add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                           char side, std::uint32_t shares, const std::string& stock,
                           std::uint32_t price) {
        begin(36, 'A', locate, ts);
        u64(ref); ch(side); u32(shares); alpha(stock, 8); u32(price);
        return *this;
    }

    // 'F' Add Order with MPID attribution, body 40.
    ItchBuilder& add_order_mpid(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                                char side, std::uint32_t shares, const std::string& stock,
                                std::uint32_t price, const std::string& mpid) {
        begin(40, 'F', locate, ts);
        u64(ref); ch(side); u32(shares); alpha(stock, 8); u32(price); alpha(mpid, 4);
        return *this;
    }

    // 'X' Order Cancel (partial), body 23.
    ItchBuilder& order_cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                              std::uint32_t canceled_shares) {
        begin(23, 'X', locate, ts);
        u64(ref); u32(canceled_shares);
        return *this;
    }

    // 'D' Order Delete, body 19.
    ItchBuilder& order_delete(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
        begin(19, 'D', locate, ts);
        u64(ref);
        return *this;
    }

    // 'E' Order Executed, body 31.
    ItchBuilder& order_executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                                std::uint32_t executed_shares, std::uint64_t match) {
        begin(31, 'E', locate, ts);
        u64(ref); u32(executed_shares); u64(match);
        return *this;
    }

    // 'C' Order Executed with Price, body 36.
    ItchBuilder& order_executed_price(std::uint16_t locate, std::uint64_t ts,
                                      std::uint64_t ref, std::uint32_t executed_shares,
                                      std::uint64_t match, char printable,
                                      std::uint32_t exec_price) {
        begin(36, 'C', locate, ts);
        u64(ref); u32(executed_shares); u64(match); ch(printable); u32(exec_price);
        return *this;
    }

    // 'U' Order Replace, body 35.
    ItchBuilder& order_replace(std::uint16_t locate, std::uint64_t ts,
                               std::uint64_t old_ref, std::uint64_t new_ref,
                               std::uint32_t shares, std::uint32_t price) {
        begin(35, 'U', locate, ts);
        u64(old_ref); u64(new_ref); u32(shares); u32(price);
        return *this;
    }

    // 'R' Stock Directory, body 39. Only locate and symbol matter downstream;
    // the remaining directory attributes get plausible constants.
    ItchBuilder& stock_directory(std::uint16_t locate, std::uint64_t ts,
                                 const std::string& symbol) {
        begin(39, 'R', locate, ts);
        alpha(symbol, 8);
        ch(' ');       // market category
        ch(' ');       // financial status indicator
        u32(100);      // round lot size
        ch('N');       // round lots only
        ch(' ');       // issue classification
        alpha("", 2);  // issue sub-type
        ch('P');       // authenticity
        ch(' ');       // short sale threshold indicator
        ch(' ');       // IPO flag
        ch(' ');       // LULD reference price tier
        ch('N');       // ETP flag
        u32(0);        // ETP leverage factor
        ch('N');       // inverse indicator
        return *this;
    }

    // Any other well-framed type: common header + the given tail bytes.
    ItchBuilder& other(char type, std::uint16_t locate, std::uint64_t ts,
                       const std::string& tail) {
        begin(static_cast<std::uint16_t>(11 + tail.size()), type, locate, ts);
        out_.append(tail);
        return *this;
    }

    // Malformed-input helpers.
    ItchBuilder& raw(const std::string& bytes) { out_.append(bytes); return *this; }
    ItchBuilder& sentinel() { ch('\0'); ch('\0'); return *this; }  // 0x0000 length
    ItchBuilder& truncate_last(std::size_t n) {
        out_.resize(out_.size() >= n ? out_.size() - n : 0);
        return *this;
    }

    const std::string& bytes() const { return out_; }
    std::istringstream stream() const {
        return std::istringstream(out_, std::ios::binary);
    }

private:
    void begin(std::uint16_t body_len, char type, std::uint16_t locate,
               std::uint64_t ts) {
        u16(body_len);  // frame length prefix
        ch(type);
        u16(locate);
        u16(0);         // tracking number (ignored by the decoder)
        u48(ts);
    }

    void ch(char c) { out_.push_back(c); }
    void u16(std::uint16_t v) { be(v, 2); }
    void u32(std::uint32_t v) { be(v, 4); }
    void u48(std::uint64_t v) { be(v, 6); }
    void u64(std::uint64_t v) { be(v, 8); }
    void be(std::uint64_t v, int bytes) {  // big-endian, most significant first
        for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
            out_.push_back(static_cast<char>((v >> shift) & 0xFF));
        }
    }
    void alpha(const std::string& s, std::size_t width) {  // left-just., space-padded
        out_.append(s, 0, std::min(s.size(), width));
        for (std::size_t i = s.size(); i < width; ++i) out_.push_back(' ');
    }

    std::string out_;
};

}  // namespace xeas::test
```

- [ ] **Step 4: Write `tests/itch_test.cpp` with the Classify / Fixture / Errors suites:**

```cpp
#include "xeas/itch.h"

#include "itch_fixture.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace {

using namespace xeas;
using xeas::test::ItchBuilder;

// ---------------------------------------------------------------------------
// Classify: is_book_message()
// ---------------------------------------------------------------------------

TEST(Classify, BookMessageTable) {
    for (char t : {'A', 'F', 'X', 'D', 'E', 'C', 'U'}) {
        EXPECT_TRUE(is_book_message(t)) << "type " << t;
    }
    for (char t : {'R', 'S', 'H', 'P', 'Q', 'I', 'B', 'a', 'u', '\0'}) {
        EXPECT_FALSE(is_book_message(t)) << "type " << t;
    }
}

// ---------------------------------------------------------------------------
// Errors: FeedError context
// ---------------------------------------------------------------------------

TEST(Errors, FeedErrorCarriesContext) {
    const FeedError e("truncated message body", 1234, 56, 'A');
    EXPECT_EQ(e.reason(), "truncated message body");
    EXPECT_EQ(e.byte_offset(), 1234u);
    EXPECT_EQ(e.message_index(), 56u);
    EXPECT_EQ(e.message_type(), 'A');
    const std::string what = e.what();
    EXPECT_NE(what.find("truncated message body"), std::string::npos);
    EXPECT_NE(what.find("1234"), std::string::npos);
    EXPECT_NE(what.find("'A'"), std::string::npos);
}

TEST(Errors, FeedErrorOmitsTypeWhenUnknown) {
    const FeedError e("truncated length prefix", 10, 2);
    EXPECT_EQ(e.message_type(), '\0');
    EXPECT_EQ(std::string(e.what()).find("type"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Fixture: the builder is the decoder's independently written inverse.
// Golden-bytes test: every byte of one 'A' frame checked against the ITCH
// 5.0 field table by hand.
// ---------------------------------------------------------------------------

TEST(Fixture, AddOrderGoldenBytes) {
    ItchBuilder b;
    b.add_order(0x0102, 0x030405060708ULL, 0x1122334455667788ULL, 'B', 200,
                "SPY", 1'234'500);
    const unsigned char expected[] = {
        0x00, 0x24,                                      // length 36
        0x41,                                            // 'A'
        0x01, 0x02,                                      // stock locate
        0x00, 0x00,                                      // tracking number
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08,              // timestamp (48-bit)
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,  // order reference
        0x42,                                            // side 'B'
        0x00, 0x00, 0x00, 0xC8,                          // shares 200
        0x53, 0x50, 0x59, 0x20, 0x20, 0x20, 0x20, 0x20,  // "SPY     "
        0x00, 0x12, 0xD6, 0x44,                          // price 1234500
    };
    ASSERT_EQ(b.bytes().size(), sizeof(expected));
    for (std::size_t i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(static_cast<unsigned char>(b.bytes()[i]), expected[i])
            << "byte " << i;
    }
}

TEST(Fixture, FrameSizesMatchSpecTable) {
    // 2-byte prefix + fixed body length per type (spec section 2 table).
    EXPECT_EQ(ItchBuilder().add_order(1, 0, 1, 'B', 1, "X", 1).bytes().size(), 38u);
    EXPECT_EQ(ItchBuilder().add_order_mpid(1, 0, 1, 'B', 1, "X", 1, "MPID").bytes().size(), 42u);
    EXPECT_EQ(ItchBuilder().order_cancel(1, 0, 1, 1).bytes().size(), 25u);
    EXPECT_EQ(ItchBuilder().order_delete(1, 0, 1).bytes().size(), 21u);
    EXPECT_EQ(ItchBuilder().order_executed(1, 0, 1, 1, 1).bytes().size(), 33u);
    EXPECT_EQ(ItchBuilder().order_executed_price(1, 0, 1, 1, 1, 'Y', 1).bytes().size(), 38u);
    EXPECT_EQ(ItchBuilder().order_replace(1, 0, 1, 2, 1, 1).bytes().size(), 37u);
    EXPECT_EQ(ItchBuilder().stock_directory(1, 0, "AAPL").bytes().size(), 41u);
}

TEST(Fixture, RawSentinelAndTruncate) {
    ItchBuilder b;
    b.order_delete(1, 0, 42).sentinel().raw(std::string("\xAB\xCD", 2));
    EXPECT_EQ(b.bytes().size(), 21u + 2u + 2u);
    b.truncate_last(4);
    EXPECT_EQ(b.bytes().size(), 21u);
}

}  // namespace
```

- [ ] **Step 5: Add the feed targets to `CMakeLists.txt`.** After the `xeas_core` block (line 14), insert:

```cmake
# --- feed library (Milestone 2) ----------------------------------------------
# Separate target so layering is enforced at link level: the book core never
# sees feed or I/O code, and Milestone 3 benchmarks can link book-only.
add_library(xeas_feed src/itch.cpp)
target_link_libraries(xeas_feed PUBLIC xeas_core)
if(NOT MSVC)
  target_compile_options(xeas_feed PRIVATE -Wall -Wextra)
endif()
```

After the `book_test` block (line 33), insert:

```cmake
add_executable(itch_test tests/itch_test.cpp)
target_link_libraries(itch_test PRIVATE xeas_feed GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(itch_test PRIVATE -Wall -Wextra)
endif()
```

And after `gtest_discover_tests(book_test)` (line 36), add:

```cmake
gtest_discover_tests(itch_test)
```

- [ ] **Step 6: Build and run — verify GREEN** (these suites exercise header-only + fixture code, so they pass once they compile; the RED/GREEN cycle for behavior starts in Task 3):

Run: `cmake -S . -B build && cmake --build build && ./build/itch_test`
Expected: 5 tests PASS (`Classify.*`, `Errors.*`, `Fixture.*`). Also `./build/book_test` still passes.

- [ ] **Step 7: Commit**

```bash
git add include/xeas/itch.h src/itch.cpp tests/itch_fixture.h tests/itch_test.cpp CMakeLists.txt
git commit -m "feat: xeas_feed skeleton - itch.h interface, ItchBuilder fixture, itch_test target"
git push
```

---

## Task 3 — `ItchReader`: framing

**Files:**
- Modify: `src/itch.cpp` (implement `ItchReader`)
- Test: `tests/itch_test.cpp` (append `Framing` suite)

**Interfaces:**
- Consumes: `ItchReader`, `FramedMessage`, `FeedError` exactly as declared in `include/xeas/itch.h` (Task 2); `ItchBuilder` from `tests/itch_fixture.h`.
- Produces: a working `ItchReader::next()` used by every later task. Framing semantics: `nullopt` at clean EOF; `0x0000` length = terminal sentinel (2 bytes counted in `bytes_consumed`, not in `messages_framed`); truncated prefix/body and length > 512 throw `FeedError` with the byte offset of the frame start and the 0-based message index.

- [ ] **Step 1: Append the failing `Framing` suite to `tests/itch_test.cpp`** (inside the existing anonymous namespace, before its closing brace). Add `#include <filesystem>` and `#include <fstream>` to the test file's includes:

```cpp
// ---------------------------------------------------------------------------
// Framing: ItchReader
// ---------------------------------------------------------------------------

TEST(Framing, EmptyStreamIsCleanEof) {
    ItchBuilder b;
    auto in = b.stream();
    ItchReader reader(in);
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.bytes_consumed(), 0u);
    EXPECT_EQ(reader.messages_framed(), 0u);
}

TEST(Framing, SingleFrameRoundTrip) {
    ItchBuilder b;
    b.add_order(1, 1000, 42, 'B', 100, "AAPL", 500'000);
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->size, 36u);
    EXPECT_EQ(std::to_integer<char>(frame->data[0]), 'A');
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.bytes_consumed(), 38u);
    EXPECT_EQ(reader.messages_framed(), 1u);
}

TEST(Framing, MultipleFramesInOrder) {
    ItchBuilder b;
    b.stock_directory(1, 100, "AAPL");
    b.add_order(1, 200, 42, 'B', 100, "AAPL", 500'000);
    b.order_delete(1, 300, 42);
    auto in = b.stream();
    ItchReader reader(in);
    const char expected_types[] = {'R', 'A', 'D'};
    const std::size_t expected_sizes[] = {39, 36, 19};
    for (int i = 0; i < 3; ++i) {
        const auto frame = reader.next();
        ASSERT_TRUE(frame.has_value()) << "frame " << i;
        EXPECT_EQ(std::to_integer<char>(frame->data[0]), expected_types[i]);
        EXPECT_EQ(frame->size, expected_sizes[i]);
    }
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.bytes_consumed(), 41u + 38u + 21u);
    EXPECT_EQ(reader.messages_framed(), 3u);
}

TEST(Framing, SentinelIsCleanEofAndTerminal) {
    ItchBuilder b;
    b.order_delete(1, 1000, 42).sentinel();
    b.add_order(1, 2000, 43, 'B', 100, "AAPL", 500'000);  // after sentinel: ignored
    auto in = b.stream();
    ItchReader reader(in);
    ASSERT_TRUE(reader.next().has_value());
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_FALSE(reader.next().has_value());  // stays EOF
    EXPECT_EQ(reader.messages_framed(), 1u);
    EXPECT_EQ(reader.bytes_consumed(), 21u + 2u);  // frame + sentinel bytes
}

TEST(Framing, TinyBufferForcesStraddle) {
    ItchBuilder b;
    for (std::uint64_t i = 0; i < 40; ++i) {
        b.add_order(1, 1000 + i, 100 + i, 'B', 10, "AAPL", 500'000);
    }
    auto in = b.stream();
    // buffer_size is clamped up to one max frame (2 + 512 = 514 bytes);
    // 40 x 38-byte frames = 1520 bytes forces refills with mid-frame
    // compaction (38 does not divide 514).
    ItchReader reader(in, 1);
    std::uint64_t n = 0;
    while (const auto frame = reader.next()) {
        EXPECT_EQ(frame->size, 36u);
        EXPECT_EQ(std::to_integer<char>(frame->data[0]), 'A');
        ++n;
    }
    EXPECT_EQ(n, 40u);
    EXPECT_EQ(reader.messages_framed(), 40u);
    EXPECT_EQ(reader.bytes_consumed(), 40u * 38u);
}

TEST(Framing, TruncatedPrefixThrowsWithPosition) {
    ItchBuilder b;
    b.order_delete(1, 1000, 42).raw(std::string("\x00", 1));  // lone prefix byte
    auto in = b.stream();
    ItchReader reader(in);
    ASSERT_TRUE(reader.next().has_value());
    try {
        reader.next();
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.reason(), "truncated length prefix");
        EXPECT_EQ(e.byte_offset(), 21u);  // one delete frame consumed
        EXPECT_EQ(e.message_index(), 1u);
        EXPECT_EQ(e.message_type(), '\0');
    }
}

TEST(Framing, TruncatedBodyThrowsWithPosition) {
    ItchBuilder b;
    b.add_order(1, 1000, 42, 'B', 100, "AAPL", 500'000).truncate_last(5);
    auto in = b.stream();
    ItchReader reader(in);
    try {
        reader.next();
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.reason(), "truncated message body");
        EXPECT_EQ(e.byte_offset(), 0u);
        EXPECT_EQ(e.message_index(), 0u);
        EXPECT_EQ(e.message_type(), 'A');  // the type byte was readable
    }
}

TEST(Framing, ImplausibleLengthThrows) {
    ItchBuilder b;
    b.raw(std::string("\x02\x01", 2));  // length 513 > 512
    auto in = b.stream();
    ItchReader reader(in);
    try {
        reader.next();
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.reason(), "implausible frame length 513");
        EXPECT_EQ(e.byte_offset(), 0u);
        EXPECT_EQ(e.message_index(), 0u);
    }
}

TEST(Framing, FileConstructorReadsAndMissingFileThrows) {
    ItchBuilder b;
    b.order_delete(7, 1000, 42);
    const auto path = std::filesystem::temp_directory_path() / "xeas_itch_framing_test.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(b.bytes().data(), static_cast<std::streamsize>(b.bytes().size()));
    }
    {
        ItchReader reader(path);
        const auto frame = reader.next();
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(std::to_integer<char>(frame->data[0]), 'D');
        EXPECT_FALSE(reader.next().has_value());
    }
    std::filesystem::remove(path);
    EXPECT_THROW(ItchReader(std::filesystem::path("/nonexistent/xeas.itch")), FeedError);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build build`
Expected: LINK FAILURE — undefined symbols for `ItchReader::ItchReader`, `ItchReader::next`, `ItchReader::refill`. (Link error IS the red state.)

- [ ] **Step 3: Implement `ItchReader` in `src/itch.cpp`** (replace the placeholder comment; keep the `#include "xeas/itch.h"`):

```cpp
#include "xeas/itch.h"

#include <algorithm>
#include <cstring>

namespace xeas {

namespace {

// Largest real ITCH 5.0 body is 50 bytes; anything past 512 means desynced,
// MoldUDP, or garbage input — fail fast instead of silently skipping
// gigabytes (spec section 5).
constexpr std::size_t kMaxBody = 512;

std::uint16_t read_u16(const std::byte* p) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(p[0]) << 8) |
                                      std::to_integer<std::uint16_t>(p[1]));
}

}  // namespace

ItchReader::ItchReader(const std::filesystem::path& path, std::size_t buffer_size)
    : owned_file_(path, std::ios::binary),
      in_(&owned_file_),
      // The buffer must hold one whole frame: next() returns views into it.
      buffer_(std::max(buffer_size, 2 + kMaxBody)) {
    if (!owned_file_.is_open()) {
        throw FeedError("cannot open file: " + path.string(), 0, 0);
    }
}

ItchReader::ItchReader(std::istream& in, std::size_t buffer_size)
    : in_(&in), buffer_(std::max(buffer_size, 2 + kMaxBody)) {}

void ItchReader::refill() {
    if (eof_) return;
    // Compact: move the unread tail to the front to make room. Bodies are
    // <= 512 bytes, so this is trivial and rare.
    if (pos_ > 0) {
        const std::size_t tail = fill_ - pos_;
        std::memmove(buffer_.data(), buffer_.data() + pos_, tail);
        pos_ = 0;
        fill_ = tail;
    }
    while (fill_ < buffer_.size() && !eof_) {
        in_->read(reinterpret_cast<char*>(buffer_.data() + fill_),
                  static_cast<std::streamsize>(buffer_.size() - fill_));
        const std::size_t got = static_cast<std::size_t>(in_->gcount());
        fill_ += got;
        if (in_->eof() || got == 0) eof_ = true;
    }
    // eofbit/failbit at end-of-data is expected; badbit is a hard I/O error.
    if (in_->bad()) {
        throw FeedError("stream read error", bytes_consumed_, messages_framed_);
    }
}

std::optional<FramedMessage> ItchReader::next() {
    if (done_) return std::nullopt;
    if (available() < 2) refill();
    if (available() == 0) return std::nullopt;  // clean EOF at a frame boundary
    if (available() < 2) {
        throw FeedError("truncated length prefix", bytes_consumed_, messages_framed_);
    }
    const std::uint16_t length = read_u16(buffer_.data() + pos_);
    if (length == 0) {
        // 0x0000 sentinel: clean end-of-stream marker at the tail of some
        // NASDAQ dumps. Consume it and stop; anything after it is ignored.
        pos_ += 2;
        bytes_consumed_ += 2;
        done_ = true;
        return std::nullopt;
    }
    if (length > kMaxBody) {
        throw FeedError("implausible frame length " + std::to_string(length),
                        bytes_consumed_, messages_framed_);
    }
    if (available() < 2u + length) refill();
    if (available() < 2u + length) {
        const char type =
            available() >= 3 ? std::to_integer<char>(buffer_[pos_ + 2]) : '\0';
        throw FeedError("truncated message body", bytes_consumed_,
                        messages_framed_, type);
    }
    const FramedMessage frame{buffer_.data() + pos_ + 2, length};
    pos_ += 2u + length;
    bytes_consumed_ += 2u + length;
    ++messages_framed_;
    return frame;
}

}  // namespace xeas
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build build && ./build/itch_test --gtest_filter='Framing.*'`
Expected: 9 tests PASS. Then `./build/itch_test` — all 14 PASS.

- [ ] **Step 5: Commit**

```bash
git add src/itch.cpp tests/itch_test.cpp
git commit -m "feat: ItchReader - buffered framing with straddle compaction, sentinel, strict truncation errors"
git push
```

---

## Task 4 — `decode_book_message`: fused decoder+normalizer

**Files:**
- Modify: `src/itch.cpp` (add decode helpers + `decode_book_message`)
- Test: `tests/itch_test.cpp` (append `Decode` suite)

**Interfaces:**
- Consumes: `ItchReader` (Task 3), `ItchBuilder` (Task 2), `Event`/`EventType`/`Side` from `xeas/event.h`.
- Produces: `BookMessage decode_book_message(const std::byte* data, std::size_t size)` per the declaration in `itch.h`. Mapping (spec §2 table): `A`/`F` → `Add` (MPID dropped), `X` → `Cancel`, `D` → `Delete`, `E`/`C` → `Execute` (printable flag and execution price dropped), `U` → `Replace` (populates `new_order_id`; `side` left at its default — the book resolves side from the old order). Throws `FeedError(reason, 0, 0, type)` on wrong body length or a side byte that is not `'B'`/`'S'`, and on a non-book type.

- [ ] **Step 1: Append the failing `Decode` suite to `tests/itch_test.cpp`** (inside the anonymous namespace). First add this helper right after the `using` declarations at the top of the namespace:

```cpp
// Frames one builder message through the real reader and decodes it.
BookMessage decode_one(const ItchBuilder& b) {
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    EXPECT_TRUE(frame.has_value());
    return decode_book_message(frame->data, frame->size);
}
```

Then the tests:

```cpp
// ---------------------------------------------------------------------------
// Decode: decode_book_message — one test per type, EVERY Event field asserted
// ---------------------------------------------------------------------------

TEST(Decode, AddOrderDecodesEveryField) {
    ItchBuilder b;
    b.add_order(0xBEEF, 0xFFEEDDCCBBAAULL, 0x0102030405060708ULL, 'B', 250,
                "AAPL", 1'234'500);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 0xBEEF);
    EXPECT_EQ(m.event.type, EventType::Add);
    EXPECT_EQ(m.event.timestamp, 0xFFEEDDCCBBAAULL);  // full 48-bit assembly
    EXPECT_EQ(m.event.order_id, 0x0102030405060708ULL);
    EXPECT_EQ(m.event.side, Side::Bid);
    EXPECT_EQ(m.event.price, 1'234'500);
    EXPECT_EQ(m.event.quantity, 250u);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, AddOrderSellSideMapsToAsk) {
    ItchBuilder b;
    b.add_order(1, 100, 42, 'S', 10, "AAPL", 500'000);
    EXPECT_EQ(decode_one(b).event.side, Side::Ask);
}

TEST(Decode, AddOrderMpidDropsAttribution) {
    ItchBuilder b;
    b.add_order_mpid(7, 200, 43, 'B', 20, "MSFT", 3'000'000, "NSDQ");
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 7);
    EXPECT_EQ(m.event.type, EventType::Add);  // same Event as 'A'
    EXPECT_EQ(m.event.timestamp, 200u);
    EXPECT_EQ(m.event.order_id, 43u);
    EXPECT_EQ(m.event.side, Side::Bid);
    EXPECT_EQ(m.event.price, 3'000'000);
    EXPECT_EQ(m.event.quantity, 20u);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, CancelDecodes) {
    ItchBuilder b;
    b.order_cancel(3, 300, 44, 15);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 3);
    EXPECT_EQ(m.event.type, EventType::Cancel);
    EXPECT_EQ(m.event.timestamp, 300u);
    EXPECT_EQ(m.event.order_id, 44u);
    EXPECT_EQ(m.event.quantity, 15u);
    EXPECT_EQ(m.event.price, 0);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, DeleteDecodes) {
    ItchBuilder b;
    b.order_delete(4, 400, 45);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 4);
    EXPECT_EQ(m.event.type, EventType::Delete);
    EXPECT_EQ(m.event.timestamp, 400u);
    EXPECT_EQ(m.event.order_id, 45u);
    EXPECT_EQ(m.event.quantity, 0u);
    EXPECT_EQ(m.event.price, 0);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, ExecutedDecodes) {
    ItchBuilder b;
    b.order_executed(5, 500, 46, 30, 0xDEADBEEFULL);  // match number ignored
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 5);
    EXPECT_EQ(m.event.type, EventType::Execute);
    EXPECT_EQ(m.event.timestamp, 500u);
    EXPECT_EQ(m.event.order_id, 46u);
    EXPECT_EQ(m.event.quantity, 30u);
    EXPECT_EQ(m.event.price, 0);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, ExecutedWithPriceIgnoresPrintableAndPrice) {
    ItchBuilder b;
    b.order_executed_price(6, 600, 47, 40, 99, 'N', 9'999'999);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.event.type, EventType::Execute);
    EXPECT_EQ(m.event.order_id, 47u);
    EXPECT_EQ(m.event.quantity, 40u);
    EXPECT_EQ(m.event.price, 0);  // execution price never affects the book
}

TEST(Decode, ReplaceDecodes) {
    ItchBuilder b;
    b.order_replace(8, 800, 48, 49, 60, 2'000'000);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 8);
    EXPECT_EQ(m.event.type, EventType::Replace);
    EXPECT_EQ(m.event.timestamp, 800u);
    EXPECT_EQ(m.event.order_id, 48u);
    EXPECT_EQ(m.event.new_order_id, 49u);
    EXPECT_EQ(m.event.quantity, 60u);
    EXPECT_EQ(m.event.price, 2'000'000);
    // 'U' carries no side byte; Event.side stays at its default and the book
    // resolves side from the old order. Do not assert side here.
}

TEST(Decode, TimestampWidensFull48Bits) {
    ItchBuilder b;
    b.order_delete(1, 0xFFFFFFFFFFFFULL, 42);  // max 6-byte value
    EXPECT_EQ(decode_one(b).event.timestamp, 0xFFFFFFFFFFFFULL);
}

TEST(Decode, WrongBodyLengthThrowsForEveryType) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order_mpid(1, 100, 2, 'B', 10, "AAPL", 1'000'000, "NSDQ");
    b.order_cancel(1, 100, 1, 5);
    b.order_delete(1, 100, 1);
    b.order_executed(1, 100, 1, 5, 900);
    b.order_executed_price(1, 100, 1, 5, 900, 'Y', 1'000'000);
    b.order_replace(1, 100, 1, 2, 10, 1'000'000);
    auto in = b.stream();
    ItchReader reader(in);
    int checked = 0;
    while (const auto frame = reader.next()) {
        const char type = std::to_integer<char>(frame->data[0]);
        try {
            decode_book_message(frame->data, frame->size - 1);
            FAIL() << "expected FeedError for type " << type;
        } catch (const FeedError& e) {
            EXPECT_EQ(e.message_type(), type);
            EXPECT_NE(e.reason().find("body length"), std::string::npos);
        }
        ++checked;
    }
    EXPECT_EQ(checked, 7);
}

TEST(Decode, BadSideByteThrows) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'Q', 10, "AAPL", 1'000'000);  // side neither B nor S
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    try {
        decode_book_message(frame->data, frame->size);
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.message_type(), 'A');
        EXPECT_NE(e.reason().find("side"), std::string::npos);
    }
}

TEST(Decode, NonBookTypeThrows) {
    ItchBuilder b;
    b.stock_directory(1, 100, "AAPL");
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_THROW(decode_book_message(frame->data, frame->size), FeedError);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build build`
Expected: LINK FAILURE — undefined symbol `decode_book_message`.

- [ ] **Step 3: Implement.** In `src/itch.cpp`, extend the anonymous namespace with the wide readers and add the decode function after `ItchReader::next()`:

```cpp
// (in the anonymous namespace, after read_u16)
std::uint32_t read_u32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | std::to_integer<std::uint32_t>(p[i]);
    return v;
}

std::uint64_t read_u48(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
    return v;
}

std::uint64_t read_u64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
    return v;
}

// Fixed body lengths per the ITCH 5.0 spec; 0 = not a book message.
std::size_t expected_body_length(char type) {
    switch (type) {
    case 'A': return 36;
    case 'F': return 40;
    case 'X': return 23;
    case 'D': return 19;
    case 'E': return 31;
    case 'C': return 36;
    case 'U': return 35;
    default: return 0;
    }
}

Side decode_side(std::byte b, char type) {
    const char c = std::to_integer<char>(b);
    if (c == 'B') return Side::Bid;
    if (c == 'S') return Side::Ask;
    throw FeedError(std::string("bad side byte '") + c + "'", 0, 0, type);
}
```

```cpp
// (in namespace xeas, after ItchReader::next())

BookMessage decode_book_message(const std::byte* data, std::size_t size) {
    const char type = std::to_integer<char>(data[0]);
    const std::size_t expected = expected_body_length(type);
    if (expected == 0) {
        throw FeedError("not a book message", 0, 0, type);
    }
    if (size != expected) {
        throw FeedError("wrong body length " + std::to_string(size) +
                            " (expected " + std::to_string(expected) + ")",
                        0, 0, type);
    }
    BookMessage out;
    out.stock_locate = read_u16(data + 1);
    // data + 3: tracking number — MoldUDP diagnostics, ignored.
    Event& e = out.event;
    e.timestamp = read_u48(data + 5);
    switch (type) {
    case 'A':
    case 'F':  // MPID attribution at data + 36 is dropped
        e.type = EventType::Add;
        e.order_id = read_u64(data + 11);
        e.side = decode_side(data[19], type);
        e.quantity = read_u32(data + 20);
        e.price = static_cast<Price>(read_u32(data + 32));
        break;
    case 'X':
        e.type = EventType::Cancel;
        e.order_id = read_u64(data + 11);
        e.quantity = read_u32(data + 19);
        break;
    case 'D':
        e.type = EventType::Delete;
        e.order_id = read_u64(data + 11);
        break;
    case 'E':
    case 'C':  // 'C' printable flag and execution price never affect the book
        e.type = EventType::Execute;
        e.order_id = read_u64(data + 11);
        e.quantity = read_u32(data + 19);
        break;
    case 'U':
        // No side byte: the book resolves side from the old order (spec §2).
        e.type = EventType::Replace;
        e.order_id = read_u64(data + 11);
        e.new_order_id = read_u64(data + 19);
        e.quantity = read_u32(data + 27);
        e.price = static_cast<Price>(read_u32(data + 31));
        break;
    }
    return out;
}
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build build && ./build/itch_test --gtest_filter='Decode.*'`
Expected: 12 tests PASS. Then full `./build/itch_test` — 26 PASS.

- [ ] **Step 5: Commit**

```bash
git add src/itch.cpp tests/itch_test.cpp
git commit -m "feat: decode_book_message - fused ITCH decoder+normalizer for A/F/X/D/E/C/U"
git push
```

---

## Task 5 — `decode_stock_directory`

**Files:**
- Modify: `src/itch.cpp` (add `decode_stock_directory`)
- Test: `tests/itch_test.cpp` (append to `Decode` suite)

**Interfaces:**
- Consumes: `read_u16` helper and reader/builder from earlier tasks (same file).
- Produces: `StockDirectoryEntry decode_stock_directory(const std::byte* data, std::size_t size)` — locate + symbol with trailing spaces trimmed; throws `FeedError` on wrong type byte or size ≠ 39.

- [ ] **Step 1: Append failing tests to `tests/itch_test.cpp`:**

```cpp
// ---------------------------------------------------------------------------
// Decode: decode_stock_directory ('R')
// ---------------------------------------------------------------------------

TEST(Decode, StockDirectoryTrimsPadding) {
    ItchBuilder b;
    b.stock_directory(42, 100, "ZVZZT");  // padded to "ZVZZT   " on the wire
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    const StockDirectoryEntry entry =
        decode_stock_directory(frame->data, frame->size);
    EXPECT_EQ(entry.stock_locate, 42);
    EXPECT_EQ(entry.symbol, "ZVZZT");
}

TEST(Decode, StockDirectoryFullWidthSymbolKept) {
    ItchBuilder b;
    b.stock_directory(1, 100, "ABCDEFGH");
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(decode_stock_directory(frame->data, frame->size).symbol, "ABCDEFGH");
}

TEST(Decode, StockDirectoryWrongLengthOrTypeThrows) {
    ItchBuilder b;
    b.stock_directory(1, 100, "AAPL");
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_THROW(decode_stock_directory(frame->data, frame->size - 1), FeedError);

    ItchBuilder b2;
    b2.order_delete(1, 100, 42);  // wrong type byte
    auto in2 = b2.stream();
    ItchReader reader2(in2);
    const auto frame2 = reader2.next();
    ASSERT_TRUE(frame2.has_value());
    EXPECT_THROW(decode_stock_directory(frame2->data, frame2->size), FeedError);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build build`
Expected: LINK FAILURE — undefined symbol `decode_stock_directory`.

- [ ] **Step 3: Implement in `src/itch.cpp`** (after `decode_book_message`):

```cpp
StockDirectoryEntry decode_stock_directory(const std::byte* data, std::size_t size) {
    const char type = std::to_integer<char>(data[0]);
    if (type != 'R') {
        throw FeedError("not a stock directory message", 0, 0, type);
    }
    if (size != 39) {
        throw FeedError("wrong body length " + std::to_string(size) +
                            " (expected 39)",
                        0, 0, 'R');
    }
    StockDirectoryEntry out;
    out.stock_locate = read_u16(data + 1);
    std::string symbol(reinterpret_cast<const char*>(data + 11), 8);
    while (!symbol.empty() && symbol.back() == ' ') symbol.pop_back();
    out.symbol = std::move(symbol);
    return out;
}
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build build && ./build/itch_test`
Expected: all 29 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/itch.cpp tests/itch_test.cpp
git commit -m "feat: decode_stock_directory - locate to symbol names for reporting"
git push
```

---

## Task 6 — `BookRouter`

**Files:**
- Create: `include/xeas/router.h`
- Create: `src/router.cpp`
- Modify: `CMakeLists.txt` (add `src/router.cpp` to `xeas_feed`)
- Test: `tests/itch_test.cpp` (append `Router` suite; add `#include "xeas/router.h"`)

**Interfaces:**
- Consumes: `Book`, `apply()` from `xeas/book.h`.
- Produces (used by Tasks 7–10): class `BookRouter` with exactly: `BookRouter()`, `Book& book_for(std::uint16_t)`, `const Book* find(std::uint16_t) const`, `void set_symbol(std::uint16_t, std::string)`, `std::string symbol_or_locate(std::uint16_t) const` (returns the symbol, or `"locate N"`), `std::size_t book_count() const noexcept`, `void for_each_book(const std::function<void(std::uint16_t, const Book&)>&) const` (ascending locate order).

- [ ] **Step 1: Write `include/xeas/router.h` (complete):**

```cpp
#pragma once

#include "xeas/book.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xeas {

// One Book per stock locate. Locates are dense, day-scoped u16s, so a flat
// 65,536-slot vector gives O(1) routing with no hashing; heap-allocated books
// give pointer stability for free. Books are created lazily on first use.
// Symbol names (from 'R' messages) are cosmetic, may arrive in any order or
// never; reports fall back to "locate N".
class BookRouter {
public:
    BookRouter();

    Book& book_for(std::uint16_t stock_locate);          // lazily creates
    const Book* find(std::uint16_t stock_locate) const;  // nullptr if never seen
    void set_symbol(std::uint16_t locate, std::string symbol);
    std::string symbol_or_locate(std::uint16_t locate) const;  // "AAPL" or "locate 42"
    std::size_t book_count() const noexcept { return book_count_; }
    // Visits every created book in ascending locate order.
    void for_each_book(const std::function<void(std::uint16_t, const Book&)>&) const;

private:
    static constexpr std::size_t kLocateSlots = 65'536;  // full u16 range

    std::vector<std::unique_ptr<Book>> books_;
    std::vector<std::string> symbols_;
    std::size_t book_count_ = 0;
};

}  // namespace xeas
```

- [ ] **Step 2: Append the failing `Router` suite to `tests/itch_test.cpp`** (add `#include "xeas/router.h"` at the top; the suite needs an event factory — add it next to `decode_one`):

```cpp
Event make_add_event(OrderId id, Side side, Price price, Qty qty) {
    Event e;
    e.type = EventType::Add;
    e.order_id = id;
    e.side = side;
    e.price = price;
    e.quantity = qty;
    return e;
}
```

```cpp
// ---------------------------------------------------------------------------
// Router: BookRouter
// ---------------------------------------------------------------------------

TEST(Router, LazyCreation) {
    BookRouter router;
    EXPECT_EQ(router.find(5), nullptr);
    EXPECT_EQ(router.book_count(), 0u);
    Book& book = router.book_for(5);
    EXPECT_EQ(router.find(5), &book);
    EXPECT_EQ(router.book_count(), 1u);
}

TEST(Router, SameLocateSameBook) {
    BookRouter router;
    Book& a = router.book_for(7);
    Book& b = router.book_for(7);
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(router.book_count(), 1u);
}

TEST(Router, DistinctLocatesAreIsolated) {
    BookRouter router;
    apply(router.book_for(1), make_add_event(10, Side::Bid, 1'000'000, 100));
    EXPECT_EQ(router.book_for(1).order_count(), 1u);
    EXPECT_EQ(router.book_for(2).order_count(), 0u);
    EXPECT_EQ(router.book_count(), 2u);
}

TEST(Router, SymbolOrLocateFallsBackThenNames) {
    BookRouter router;
    EXPECT_EQ(router.symbol_or_locate(42), "locate 42");
    router.set_symbol(42, "AAPL");
    EXPECT_EQ(router.symbol_or_locate(42), "AAPL");
    // Symbols are independent of book creation ('R' needs no ordering).
    EXPECT_EQ(router.find(42), nullptr);
}

TEST(Router, ExtremeLocatesWork) {
    BookRouter router;
    router.book_for(0);
    router.book_for(65'535);
    EXPECT_NE(router.find(0), nullptr);
    EXPECT_NE(router.find(65'535), nullptr);
    EXPECT_EQ(router.book_count(), 2u);
}

TEST(Router, ForEachBookVisitsAllInLocateOrder) {
    BookRouter router;
    router.book_for(30);
    router.book_for(10);
    router.book_for(20);
    std::vector<std::uint16_t> visited;
    router.for_each_book(
        [&](std::uint16_t locate, const Book&) { visited.push_back(locate); });
    EXPECT_EQ(visited, (std::vector<std::uint16_t>{10, 20, 30}));
}
```

- [ ] **Step 3: Run to verify RED**

Run: `cmake --build build`
Expected: LINK FAILURE — undefined `BookRouter` symbols (header exists; `src/router.cpp` not yet written/compiled).

- [ ] **Step 4: Implement `src/router.cpp`:**

```cpp
#include "xeas/router.h"

namespace xeas {

BookRouter::BookRouter() : books_(kLocateSlots), symbols_(kLocateSlots) {}

Book& BookRouter::book_for(std::uint16_t stock_locate) {
    auto& slot = books_[stock_locate];
    if (!slot) {
        slot = std::make_unique<Book>();
        ++book_count_;
    }
    return *slot;
}

const Book* BookRouter::find(std::uint16_t stock_locate) const {
    return books_[stock_locate].get();
}

void BookRouter::set_symbol(std::uint16_t locate, std::string symbol) {
    symbols_[locate] = std::move(symbol);
}

std::string BookRouter::symbol_or_locate(std::uint16_t locate) const {
    const std::string& symbol = symbols_[locate];
    return symbol.empty() ? "locate " + std::to_string(locate) : symbol;
}

void BookRouter::for_each_book(
    const std::function<void(std::uint16_t, const Book&)>& fn) const {
    for (std::size_t locate = 0; locate < books_.size(); ++locate) {
        if (books_[locate]) {
            fn(static_cast<std::uint16_t>(locate), *books_[locate]);
        }
    }
}

}  // namespace xeas
```

And in `CMakeLists.txt` change the `xeas_feed` line to:

```cmake
add_library(xeas_feed src/itch.cpp src/router.cpp)
```

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build build && ./build/itch_test`
Expected: all 35 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/xeas/router.h src/router.cpp tests/itch_test.cpp CMakeLists.txt
git commit -m "feat: BookRouter - O(1) locate routing, lazy books, symbol names"
git push
```

---

## Task 7 — `replay_stream`: driver core (happy path, stats, cadence)

**Files:**
- Create: `include/xeas/replay.h` (COMPLETE final form)
- Create: `src/replay.cpp` (complete — Task 8 only adds tests)
- Create: `tests/replay_test.cpp` (Replay suite, happy-path half)
- Modify: `CMakeLists.txt` (add `src/replay.cpp` to `xeas_feed`; add `replay_test` target)

**Interfaces:**
- Consumes: `ItchReader`, `decode_book_message`, `decode_stock_directory`, `is_book_message`, `FeedError` (itch.h); `BookRouter` (router.h); `Book`, `apply`, `check_invariants`, `is_crossed`, `BookError` (book.h); `ItchBuilder` (tests/itch_fixture.h).
- Produces: `ReplayOptions`, `ReplayStats`, `ReplayError`, `void replay_stream(ItchReader&, BookRouter&, const ReplayOptions&, ReplayStats&, std::ostream& log)` — used by Task 8 tests, Task 9, and the Task 10 CLI.

- [ ] **Step 1: Write `include/xeas/replay.h` (complete):**

```cpp
#pragma once

#include "xeas/book.h"
#include "xeas/itch.h"
#include "xeas/router.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xeas {

// Thrown when an invariant sweep finds a structural violation (level sums,
// empty levels, index mismatches, ...) — a bug somewhere, never data reality.
// Crossed/locked findings are classified as reportable and never reach here.
class ReplayError : public std::runtime_error {
public:
    ReplayError(std::uint16_t locate, std::string symbol,
                std::vector<std::string> violations, std::uint64_t message_index);

    std::uint16_t locate() const noexcept { return locate_; }
    const std::string& symbol() const noexcept { return symbol_; }
    const std::vector<std::string>& violations() const noexcept { return violations_; }
    std::uint64_t message_index() const noexcept { return message_index_; }

private:
    std::uint16_t locate_;
    std::string symbol_;
    std::vector<std::string> violations_;
    std::uint64_t message_index_;
};

struct ReplayOptions {
    std::uint64_t invariant_sweep_every = 25'000'000;  // messages between sweeps; 0 = final only
    std::uint64_t progress_every = 10'000'000;         // messages between lines; 0 = silent
};

struct ReplayStats {
    std::uint64_t messages_total = 0;
    std::array<std::uint64_t, 256> count_by_type{};  // indexed by type byte
    std::uint64_t events_applied = 0;
    std::uint64_t bytes_consumed = 0;
    std::uint64_t books_created = 0;
    std::uint64_t crossed_episodes = 0;    // false->true transitions
    std::uint64_t crossed_symbols = 0;     // distinct books ever crossed
    std::uint64_t timestamp_regressions = 0;
    std::uint64_t live_orders_peak = 0;
    std::uint64_t live_orders_final = 0;
    std::uint64_t invariant_sweeps = 0;
    double elapsed_seconds = 0.0;
};

// Replays every frame from the reader through per-locate books. stats is an
// out-parameter (not a return value) so that on an abort the caller still has
// everything counted up to the failure point — the single most useful
// debugging artifact. FeedError / BookError / ReplayError all propagate;
// byte/book/elapsed counters are stamped even then.
void replay_stream(ItchReader& reader, BookRouter& router,
                   const ReplayOptions& opts, ReplayStats& stats,
                   std::ostream& log);

}  // namespace xeas
```

- [ ] **Step 2: Write the failing happy-path tests in `tests/replay_test.cpp`:**

```cpp
#include "xeas/replay.h"

#include "itch_fixture.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace xeas;
using xeas::test::ItchBuilder;

// Runs a builder tape through replay_stream. Default options: final sweep
// only, silent. Errors propagate to the caller; stats survive via out-param.
ReplayStats run(const ItchBuilder& b, BookRouter& router,
                ReplayOptions opts = ReplayOptions{0, 0},
                std::ostream* log = nullptr) {
    std::ostringstream sink;
    auto in = b.stream();
    ItchReader reader(in);
    ReplayStats stats;
    replay_stream(reader, router, opts, stats, log != nullptr ? *log : sink);
    return stats;
}

// ---------------------------------------------------------------------------
// Replay: driver semantics
// ---------------------------------------------------------------------------

TEST(Replay, MultiSymbolHappyTapeFinalState) {
    ItchBuilder b;
    b.stock_directory(1, 100, "AAPL");
    b.stock_directory(2, 100, "MSFT");
    b.other('S', 0, 100, "O");  // system event: counted, skipped
    b.add_order(1, 200, 10, 'B', 100, "AAPL", 1'500'000);
    b.add_order(1, 210, 11, 'S', 80, "AAPL", 1'501'000);
    b.add_order(2, 220, 20, 'B', 50, "MSFT", 3'000'000);
    b.order_executed(1, 230, 11, 30, 9001);           // ask 11 -> 50 left
    b.order_cancel(1, 240, 10, 40);                   // bid 10 -> 60 left
    b.order_replace(2, 250, 20, 21, 70, 3'010'000);   // MSFT bid moves
    b.order_delete(1, 260, 11);                       // AAPL ask gone
    b.add_order_mpid(2, 270, 22, 'S', 10, "MSFT", 3'020'000, "NSDQ");

    BookRouter router;
    const ReplayStats stats = run(b, router);

    EXPECT_EQ(stats.messages_total, 11u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('R')], 2u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('S')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('A')], 3u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('F')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('E')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('X')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('U')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('D')], 1u);
    EXPECT_EQ(stats.events_applied, 8u);
    EXPECT_EQ(stats.books_created, 2u);
    EXPECT_EQ(stats.bytes_consumed, b.bytes().size());
    EXPECT_EQ(stats.invariant_sweeps, 1u);  // final sweep only
    EXPECT_EQ(stats.timestamp_regressions, 0u);
    EXPECT_EQ(stats.crossed_episodes, 0u);

    const Book* aapl = router.find(1);
    ASSERT_NE(aapl, nullptr);
    ASSERT_TRUE(aapl->best_bid().has_value());
    EXPECT_EQ(aapl->best_bid()->price, 1'500'000);
    EXPECT_EQ(aapl->best_bid()->total_shares, 60);
    EXPECT_FALSE(aapl->best_ask().has_value());
    EXPECT_EQ(aapl->order_count(), 1u);

    const Book* msft = router.find(2);
    ASSERT_NE(msft, nullptr);
    ASSERT_TRUE(msft->best_bid().has_value());
    EXPECT_EQ(msft->best_bid()->price, 3'010'000);
    EXPECT_EQ(msft->best_bid()->total_shares, 70);
    ASSERT_TRUE(msft->best_ask().has_value());
    EXPECT_EQ(msft->best_ask()->price, 3'020'000);
    EXPECT_EQ(msft->order_count(), 2u);

    EXPECT_EQ(router.symbol_or_locate(1), "AAPL");
    EXPECT_EQ(router.symbol_or_locate(2), "MSFT");
}

TEST(Replay, UnknownTypesCountedAndSkipped) {
    ItchBuilder b;
    b.other('P', 1, 100, std::string(33, 'x'));  // Trade (non-cross), real type
    b.other('z', 1, 110, "??");                  // unknown future type
    b.other('z', 1, 120, "??");
    BookRouter router;
    const ReplayStats stats = run(b, router);
    EXPECT_EQ(stats.messages_total, 3u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('P')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('z')], 2u);
    EXPECT_EQ(stats.events_applied, 0u);
    EXPECT_EQ(stats.books_created, 0u);
}

TEST(Replay, LiveOrdersPeakAndFinal) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 110, 2, 'B', 10, "AAPL", 1'000'100);
    b.add_order(1, 120, 3, 'S', 10, "AAPL", 1'000'700);
    b.order_delete(1, 130, 1);
    b.order_delete(1, 140, 3);
    BookRouter router;
    const ReplayStats stats = run(b, router);
    EXPECT_EQ(stats.live_orders_peak, 3u);
    EXPECT_EQ(stats.live_orders_final, 1u);
}

TEST(Replay, TimestampRegressionsCountedNotFatal) {
    ItchBuilder b;
    b.add_order(1, 200, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 150, 2, 'B', 10, "AAPL", 1'000'100);  // regression
    b.add_order(1, 150, 3, 'B', 10, "AAPL", 1'000'200);  // equal: NOT a regression
    b.add_order(1, 100, 4, 'B', 10, "AAPL", 1'000'300);  // regression
    BookRouter router;
    const ReplayStats stats = run(b, router);
    EXPECT_EQ(stats.timestamp_regressions, 2u);
    EXPECT_EQ(stats.events_applied, 4u);  // never fatal
}

TEST(Replay, SweepCadencePlusFinalSweep) {
    ItchBuilder b;
    for (std::uint64_t i = 0; i < 5; ++i) {
        b.add_order(1, 100 + i, 1 + i, 'B', 10, "AAPL",
                    static_cast<std::uint32_t>(1'000'000 - i * 100));
    }
    BookRouter router;
    const ReplayStats stats = run(b, router, ReplayOptions{2, 0});
    EXPECT_EQ(stats.invariant_sweeps, 3u);  // after msgs 2 and 4, plus final
}

TEST(Replay, ProgressLinesAndQuiet) {
    ItchBuilder b;
    for (std::uint64_t i = 0; i < 5; ++i) {
        b.add_order(1, 100 + i, 1 + i, 'B', 10, "AAPL", 1'000'000);
    }
    BookRouter router1;
    std::ostringstream log1;
    run(b, router1, ReplayOptions{0, 2}, &log1);
    std::size_t lines = 0;
    for (char c : log1.str()) lines += (c == '\n') ? 1 : 0;
    EXPECT_EQ(lines, 2u);  // after msgs 2 and 4
    EXPECT_NE(log1.str().find("progress"), std::string::npos);

    BookRouter router2;
    std::ostringstream log2;
    run(b, router2, ReplayOptions{0, 0}, &log2);
    EXPECT_TRUE(log2.str().empty());  // 0 = silent
}

}  // namespace
```

- [ ] **Step 3: Add CMake targets and verify RED.** In `CMakeLists.txt`, change the `xeas_feed` line to:

```cmake
add_library(xeas_feed src/itch.cpp src/router.cpp src/replay.cpp)
```

After the `itch_test` block add:

```cmake
add_executable(replay_test tests/replay_test.cpp)
target_link_libraries(replay_test PRIVATE xeas_feed GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(replay_test PRIVATE -Wall -Wextra)
endif()
```

And next to the other `gtest_discover_tests` calls: `gtest_discover_tests(replay_test)`.

Create `src/replay.cpp` containing only `#include "xeas/replay.h"` for the RED build.

Run: `cmake -S . -B build && cmake --build build`
Expected: LINK FAILURE — undefined `replay_stream` and `ReplayError::ReplayError`.

- [ ] **Step 4: Implement `src/replay.cpp` (complete):**

```cpp
#include "xeas/replay.h"

#include <chrono>
#include <cstddef>
#include <utility>

namespace xeas {

namespace {

std::string format_replay_error(std::uint16_t locate, const std::string& symbol,
                                const std::vector<std::string>& violations,
                                std::uint64_t message_index) {
    std::string msg = "structural invariant violation in " + symbol +
                      " (locate " + std::to_string(locate) + ") at message " +
                      std::to_string(message_index) + ":";
    for (const auto& v : violations) msg += "\n  - " + v;
    return msg;
}

// Prefix produced by check_invariants() for the one finding that is data
// reality (halts, opening/closing crosses), not a bug — see src/book.cpp.
constexpr const char* kCrossedPrefix = "book crossed or locked";

}  // namespace

ReplayError::ReplayError(std::uint16_t locate, std::string symbol,
                         std::vector<std::string> violations,
                         std::uint64_t message_index)
    : std::runtime_error(format_replay_error(locate, symbol, violations, message_index)),
      locate_(locate),
      symbol_(std::move(symbol)),
      violations_(std::move(violations)),
      message_index_(message_index) {}

void replay_stream(ItchReader& reader, BookRouter& router,
                   const ReplayOptions& opts, ReplayStats& stats,
                   std::ostream& log) {
    const auto start = std::chrono::steady_clock::now();
    const auto stamp = [&] {
        stats.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                .count();
        stats.bytes_consumed = reader.bytes_consumed();
        stats.books_created = router.book_count();
    };

    // Per-locate crossed state for the O(1) probe on the touched book only.
    std::vector<bool> crossed_now(65'536, false);
    std::vector<bool> crossed_ever(65'536, false);
    std::vector<bool> crossed_logged(65'536, false);

    std::int64_t live_orders = 0;
    Timestamp last_timestamp = 0;
    bool have_timestamp = false;

    // Sweep every book. "book crossed or locked" findings are data reality:
    // logged on first occurrence per book, never fatal. Anything else is a
    // structural bug: throw ReplayError (spec section 4).
    const auto sweep = [&] {
        ++stats.invariant_sweeps;
        router.for_each_book([&](std::uint16_t locate, const Book& book) {
            std::vector<std::string> violations = check_invariants(book);
            bool crossed = false;
            std::vector<std::string> structural;
            for (auto& v : violations) {
                if (v.rfind(kCrossedPrefix, 0) == 0) {
                    crossed = true;
                } else {
                    structural.push_back(std::move(v));
                }
            }
            if (crossed && !crossed_logged[locate]) {
                crossed_logged[locate] = true;
                log << "note: " << router.symbol_or_locate(locate)
                    << " crossed or locked at timestamp " << last_timestamp << "\n";
            }
            if (!structural.empty()) {
                throw ReplayError(locate, router.symbol_or_locate(locate),
                                  std::move(structural), stats.messages_total);
            }
        });
    };

    try {
        while (const auto frame = reader.next()) {
            const char type = std::to_integer<char>(frame->data[0]);
            ++stats.messages_total;
            ++stats.count_by_type[static_cast<unsigned char>(type)];

            if (type == 'R') {
                StockDirectoryEntry entry;
                try {
                    entry = decode_stock_directory(frame->data, frame->size);
                } catch (const FeedError& e) {
                    // Re-throw with the position only this layer knows.
                    throw FeedError(e.reason(),
                                    reader.bytes_consumed() - frame->size - 2,
                                    stats.messages_total - 1, e.message_type());
                }
                router.set_symbol(entry.stock_locate, std::move(entry.symbol));
            } else if (is_book_message(type)) {
                BookMessage msg;
                try {
                    msg = decode_book_message(frame->data, frame->size);
                } catch (const FeedError& e) {
                    throw FeedError(e.reason(),
                                    reader.bytes_consumed() - frame->size - 2,
                                    stats.messages_total - 1, e.message_type());
                }

                if (have_timestamp && msg.event.timestamp < last_timestamp) {
                    ++stats.timestamp_regressions;  // reported, never fatal
                }
                last_timestamp = msg.event.timestamp;
                have_timestamp = true;

                Book& book = router.book_for(msg.stock_locate);
                const auto before = static_cast<std::int64_t>(book.order_count());
                apply(book, msg.event);  // BookError propagates (abort)
                ++stats.events_applied;

                live_orders += static_cast<std::int64_t>(book.order_count()) - before;
                if (live_orders > 0 &&
                    static_cast<std::uint64_t>(live_orders) > stats.live_orders_peak) {
                    stats.live_orders_peak = static_cast<std::uint64_t>(live_orders);
                }

                // O(1) crossed probe on the touched book only: count an
                // episode on each false->true transition.
                const bool crossed = is_crossed(book);
                if (crossed && !crossed_now[msg.stock_locate]) {
                    ++stats.crossed_episodes;
                    if (!crossed_ever[msg.stock_locate]) {
                        crossed_ever[msg.stock_locate] = true;
                        ++stats.crossed_symbols;
                    }
                }
                crossed_now[msg.stock_locate] = crossed;
            }
            // All other well-framed types: already counted; skipping is free.

            if (opts.invariant_sweep_every != 0 &&
                stats.messages_total % opts.invariant_sweep_every == 0) {
                sweep();
            }
            if (opts.progress_every != 0 &&
                stats.messages_total % opts.progress_every == 0) {
                stamp();
                log << "progress: " << stats.messages_total << " messages, "
                    << stats.events_applied << " events, "
                    << router.book_count() << " books, " << live_orders
                    << " live orders\n";
            }
        }
        sweep();  // final sweep at EOF, always
        stats.live_orders_final =
            live_orders > 0 ? static_cast<std::uint64_t>(live_orders) : 0;
        stamp();
    } catch (...) {
        stats.live_orders_final =
            live_orders > 0 ? static_cast<std::uint64_t>(live_orders) : 0;
        stamp();
        throw;  // stats out-param keeps everything counted so far
    }
}

}  // namespace xeas
```

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build build && ./build/replay_test`
Expected: 6 tests PASS. `ctest --test-dir build --output-on-failure` — everything passes.

- [ ] **Step 6: Commit**

```bash
git add include/xeas/replay.h src/replay.cpp tests/replay_test.cpp CMakeLists.txt
git commit -m "feat: replay_stream - driver with stats, sweeps, crossed probe, progress"
git push
```

---

## Task 8 — Replay error paths & crossed classification

**Files:**
- Test: `tests/replay_test.cpp` (append; implementation already exists from Task 7 — these tests verify its error/crossed behavior and may reveal bugs to fix in `src/replay.cpp`)

**Interfaces:**
- Consumes: the `run()` helper defined at the top of `tests/replay_test.cpp` (Task 7): `ReplayStats run(const ItchBuilder&, BookRouter&, ReplayOptions = {0,0}, std::ostream* = nullptr)`. Also `FeedError` (`reason()`, `byte_offset()`, `message_index()`, `message_type()`), `BookError`, `ReplayError` accessors.
- Produces: verified abort semantics for Tasks 9–10.

- [ ] **Step 1: Append the failing tests** (inside the anonymous namespace of `tests/replay_test.cpp`):

```cpp
// ---------------------------------------------------------------------------
// Replay: crossed classification and abort semantics
// ---------------------------------------------------------------------------

TEST(Replay, CrossedTapeCompletesAndCountsEpisodes) {
    ItchBuilder b;
    b.stock_directory(1, 50, "AAPL");
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 110, 2, 'S', 10, "AAPL", 1'005'000);  // normal spread
    b.add_order(1, 120, 3, 'S', 10, "AAPL", 995'000);    // crossed: episode 1
    b.order_delete(1, 130, 3);                           // uncrossed
    b.add_order(1, 140, 4, 'S', 10, "AAPL", 1'000'000);  // locked: episode 2
    BookRouter router;
    std::ostringstream log;
    // Sweep after every message: the crossed book is seen by sweeps too —
    // logged once, never fatal.
    const ReplayStats stats = run(b, router, ReplayOptions{1, 0}, &log);
    EXPECT_EQ(stats.crossed_episodes, 2u);
    EXPECT_EQ(stats.crossed_symbols, 1u);
    EXPECT_EQ(stats.events_applied, 5u);  // completed without throwing
    const std::string text = log.str();
    EXPECT_NE(text.find("AAPL"), std::string::npos);
    EXPECT_NE(text.find("crossed"), std::string::npos);
    EXPECT_EQ(text.find("crossed"), text.rfind("crossed"));  // logged exactly once
}

TEST(Replay, CrossedThenUncrossedFinalSweepStaysQuiet) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 110, 2, 'S', 10, "AAPL", 995'000);  // crossed
    b.order_delete(1, 120, 2);                         // uncrossed before EOF
    BookRouter router;
    std::ostringstream log;
    const ReplayStats stats = run(b, router, ReplayOptions{0, 0}, &log);
    EXPECT_EQ(stats.crossed_episodes, 1u);   // probe caught it mid-tape
    EXPECT_EQ(stats.crossed_symbols, 1u);
    EXPECT_TRUE(log.str().empty());          // final sweep saw a clean book
}

TEST(Replay, BookErrorPropagatesWithStatsIntact) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.order_cancel(1, 110, 999, 5);  // unknown order id -> BookError
    b.add_order(1, 120, 2, 'B', 10, "AAPL", 1'000'000);  // never reached
    BookRouter router;
    auto in = b.stream();
    ItchReader reader(in);
    ReplayStats stats;
    std::ostringstream log;
    const ReplayOptions opts{0, 0};
    EXPECT_THROW(replay_stream(reader, router, opts, stats, log), BookError);
    EXPECT_EQ(stats.messages_total, 2u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('A')], 1u);
    EXPECT_EQ(stats.count_by_type[static_cast<unsigned char>('X')], 1u);
    EXPECT_EQ(stats.events_applied, 1u);
    EXPECT_EQ(stats.books_created, 1u);
    EXPECT_EQ(stats.live_orders_final, 1u);
    EXPECT_GT(stats.bytes_consumed, 0u);
}

TEST(Replay, TruncatedStreamMidTapeThrowsFeedError) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order(1, 110, 2, 'B', 10, "AAPL", 1'000'100);
    b.truncate_last(4);
    BookRouter router;
    auto in = b.stream();
    ItchReader reader(in);
    ReplayStats stats;
    std::ostringstream log;
    const ReplayOptions opts{0, 0};
    EXPECT_THROW(replay_stream(reader, router, opts, stats, log), FeedError);
    EXPECT_EQ(stats.messages_total, 1u);  // first message processed fine
    EXPECT_EQ(stats.events_applied, 1u);
}

TEST(Replay, DecodeErrorEnrichedWithStreamPosition) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);   // frame 0: 38 bytes
    b.add_order(1, 110, 2, 'Q', 10, "AAPL", 1'000'100);   // bad side byte
    BookRouter router;
    auto in = b.stream();
    ItchReader reader(in);
    ReplayStats stats;
    std::ostringstream log;
    const ReplayOptions opts{0, 0};
    try {
        replay_stream(reader, router, opts, stats, log);
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.byte_offset(), 38u);   // start of the offending frame
        EXPECT_EQ(e.message_index(), 1u);
        EXPECT_EQ(e.message_type(), 'A');
        EXPECT_NE(e.reason().find("side"), std::string::npos);
    }
}

TEST(Replay, ReplayErrorCarriesContext) {
    // A structural violation cannot be produced through well-formed replay
    // (that is the point); checker correctness is Milestone 1's job. Verify
    // the exception type's contract directly.
    const ReplayError e(42, "AAPL",
                        {"bid level 100 is empty", "order index size 2 != orders present in levels 1"},
                        1234);
    EXPECT_EQ(e.locate(), 42);
    EXPECT_EQ(e.symbol(), "AAPL");
    EXPECT_EQ(e.violations().size(), 2u);
    EXPECT_EQ(e.message_index(), 1234u);
    const std::string what = e.what();
    EXPECT_NE(what.find("AAPL"), std::string::npos);
    EXPECT_NE(what.find("42"), std::string::npos);
    EXPECT_NE(what.find("1234"), std::string::npos);
    EXPECT_NE(what.find("bid level 100 is empty"), std::string::npos);
    EXPECT_NE(what.find("order index size"), std::string::npos);
}
```

- [ ] **Step 2: Run to verify RED-or-GREEN honestly**

Run: `cmake --build build && ./build/replay_test`
Expected: these tests exercise code written in Task 7, so some may already pass — that is fine (they are contract locks, and Task 7's implementation was written against this plan). Any FAILURE means a real Task 7 bug: fix `src/replay.cpp` until green, and note the fix in the commit message.

- [ ] **Step 3: Run the full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass (book_test 57, itch_test 35, replay_test 12).

- [ ] **Step 4: Commit**

```bash
git add tests/replay_test.cpp src/replay.cpp
git commit -m "test: replay abort semantics, crossed classification, error enrichment"
git push
```

---

## Task 9 — Differential suite: binary tape vs direct events

**Files:**
- Test: `tests/replay_test.cpp` (append `Differential` suite)

**Interfaces:**
- Consumes: `run()` helper (Task 7), `apply()`, all `Book` queries, `ItchBuilder`.
- Produces: the end-to-end check with zero real data (spec §8.6).

- [ ] **Step 1: Append helpers + failing tests** (inside the anonymous namespace). Helpers first:

```cpp
// ---------------------------------------------------------------------------
// Differential: the same logical tape expressed twice — binary via
// ItchBuilder -> replay_stream, and as Events applied to hand-routed Books.
// Every query must agree across all books.
// ---------------------------------------------------------------------------

struct DirectEvent {
    std::uint16_t locate;
    Event event;
};

Event ev(EventType type, Timestamp ts, OrderId id, Side side, Price price,
         Qty qty, OrderId new_id = 0) {
    Event e;
    e.type = type;
    e.timestamp = ts;
    e.order_id = id;
    e.side = side;
    e.price = price;
    e.quantity = qty;
    e.new_order_id = new_id;
    return e;
}

std::map<std::uint16_t, Book> apply_direct(const std::vector<DirectEvent>& tape) {
    std::map<std::uint16_t, Book> books;
    for (const auto& [locate, event] : tape) apply(books[locate], event);
    return books;
}

void expect_books_agree(const BookRouter& router,
                        const std::map<std::uint16_t, Book>& direct,
                        const std::vector<Price>& prices,
                        const std::vector<OrderId>& order_ids) {
    EXPECT_EQ(router.book_count(), direct.size());
    for (const auto& [locate, expected] : direct) {
        const Book* actual = router.find(locate);
        ASSERT_NE(actual, nullptr) << "locate " << locate;
        EXPECT_EQ(actual->order_count(), expected.order_count()) << "locate " << locate;
        EXPECT_EQ(actual->level_count(Side::Bid), expected.level_count(Side::Bid));
        EXPECT_EQ(actual->level_count(Side::Ask), expected.level_count(Side::Ask));

        const auto bb_a = actual->best_bid();
        const auto bb_e = expected.best_bid();
        ASSERT_EQ(bb_a.has_value(), bb_e.has_value()) << "locate " << locate;
        if (bb_a.has_value()) {
            EXPECT_EQ(bb_a->price, bb_e->price);
            EXPECT_EQ(bb_a->total_shares, bb_e->total_shares);
        }
        const auto ba_a = actual->best_ask();
        const auto ba_e = expected.best_ask();
        ASSERT_EQ(ba_a.has_value(), ba_e.has_value()) << "locate " << locate;
        if (ba_a.has_value()) {
            EXPECT_EQ(ba_a->price, ba_e->price);
            EXPECT_EQ(ba_a->total_shares, ba_e->total_shares);
        }
        for (const Price p : prices) {
            EXPECT_EQ(actual->size_at(Side::Bid, p), expected.size_at(Side::Bid, p))
                << "locate " << locate << " bid " << p;
            EXPECT_EQ(actual->size_at(Side::Ask, p), expected.size_at(Side::Ask, p))
                << "locate " << locate << " ask " << p;
        }
        for (const OrderId id : order_ids) {
            const auto oa = actual->find_order(id);
            const auto oe = expected.find_order(id);
            ASSERT_EQ(oa.has_value(), oe.has_value()) << "order " << id;
            if (oa.has_value()) {
                EXPECT_EQ(oa->side, oe->side) << "order " << id;
                EXPECT_EQ(oa->price, oe->price) << "order " << id;
                EXPECT_EQ(oa->remaining_qty, oe->remaining_qty) << "order " << id;
            }
        }
    }
}
```

Then the three tests:

```cpp
TEST(Differential, TwoSymbolMixedTapeAgrees) {
    ItchBuilder b;
    std::vector<DirectEvent> direct;

    b.stock_directory(1, 50, "AAPL");  // no direct counterpart (not an Event)
    b.add_order(1, 100, 10, 'B', 100, "AAPL", 1'500'000);
    direct.push_back({1, ev(EventType::Add, 100, 10, Side::Bid, 1'500'000, 100)});
    b.add_order(1, 110, 11, 'S', 80, "AAPL", 1'501'000);
    direct.push_back({1, ev(EventType::Add, 110, 11, Side::Ask, 1'501'000, 80)});
    b.add_order(2, 120, 20, 'B', 50, "MSFT", 3'000'000);
    direct.push_back({2, ev(EventType::Add, 120, 20, Side::Bid, 3'000'000, 50)});
    b.order_executed(1, 130, 11, 30, 7001);
    direct.push_back({1, ev(EventType::Execute, 130, 11, Side::Bid, 0, 30)});
    b.order_cancel(1, 140, 10, 40);
    direct.push_back({1, ev(EventType::Cancel, 140, 10, Side::Bid, 0, 40)});
    b.order_delete(1, 150, 11);
    direct.push_back({1, ev(EventType::Delete, 150, 11, Side::Bid, 0, 0)});
    b.add_order_mpid(2, 160, 22, 'S', 10, "MSFT", 3'020'000, "NSDQ");
    direct.push_back({2, ev(EventType::Add, 160, 22, Side::Ask, 3'020'000, 10)});

    BookRouter router;
    run(b, router);
    expect_books_agree(router, apply_direct(direct),
                       {1'500'000, 1'501'000, 3'000'000, 3'020'000},
                       {10, 11, 20, 22});
}

TEST(Differential, ReplaceChainTapeAgrees) {
    ItchBuilder b;
    std::vector<DirectEvent> direct;

    b.add_order(3, 100, 1, 'S', 100, "ZVZZT", 2'000'000);
    direct.push_back({3, ev(EventType::Add, 100, 1, Side::Ask, 2'000'000, 100)});
    b.order_replace(3, 110, 1, 2, 90, 2'001'000);
    direct.push_back({3, ev(EventType::Replace, 110, 1, Side::Bid, 2'001'000, 90, 2)});
    b.order_executed_price(3, 120, 2, 40, 8001, 'Y', 2'001'000);
    direct.push_back({3, ev(EventType::Execute, 120, 2, Side::Bid, 0, 40)});
    b.order_replace(3, 130, 2, 3, 50, 1'999'000);
    direct.push_back({3, ev(EventType::Replace, 130, 2, Side::Bid, 1'999'000, 50, 3)});
    // Note: event.side on Replace/Execute is not meaningful; the book resolves
    // side from the referenced order — both paths must agree regardless.

    BookRouter router;
    run(b, router);
    expect_books_agree(router, apply_direct(direct),
                       {2'000'000, 2'001'000, 1'999'000}, {1, 2, 3});
}

TEST(Differential, CrossedTapeAgrees) {
    ItchBuilder b;
    std::vector<DirectEvent> direct;

    b.add_order(4, 100, 1, 'B', 10, "TEST", 1'000'000);
    direct.push_back({4, ev(EventType::Add, 100, 1, Side::Bid, 1'000'000, 10)});
    b.add_order(4, 110, 2, 'S', 10, "TEST", 995'000);  // crossed — stored as given
    direct.push_back({4, ev(EventType::Add, 110, 2, Side::Ask, 995'000, 10)});

    BookRouter router;
    const ReplayStats stats = run(b, router);
    EXPECT_EQ(stats.crossed_episodes, 1u);
    expect_books_agree(router, apply_direct(direct), {1'000'000, 995'000}, {1, 2});
}
```

- [ ] **Step 2: Run to verify**

Run: `cmake --build build && ./build/replay_test --gtest_filter='Differential.*'`
Expected: 3 tests PASS (these lock end-to-end agreement; a failure means a decoder or driver bug — debug before proceeding, do not weaken the assertions).

- [ ] **Step 3: Full suite, then commit**

Run: `ctest --test-dir build --output-on-failure` — all pass.

```bash
git add tests/replay_test.cpp
git commit -m "test: differential suite - binary tape vs direct events agree on every query"
git push
```

---

## Task 10 — `itch_replay` CLI

**Files:**
- Create: `src/itch_replay_main.cpp`
- Modify: `CMakeLists.txt` (add the executable)

**Interfaces:**
- Consumes: everything from `itch.h`, `router.h`, `replay.h`, plus `BookError` from `book.h`.
- Produces: `itch_replay <file> [--sweep-every N] [--progress N] [--quiet]`. Exit codes: 0 success (crossed findings included), 1 usage or unopenable file, 2 `FeedError`, 3 `BookError`, 4 `ReplayError`. On 2–4: full exception context + stats-so-far to stderr. No unit tests — its logic lives in `replay_stream` (already covered); smoke-tested below.

- [ ] **Step 1: Write `src/itch_replay_main.cpp` (complete):**

```cpp
// itch_replay <file> [--sweep-every N] [--progress N] [--quiet]
//
// Thin shell around replay_stream(): argv parsing, exception -> exit-code
// mapping, stats printing. All replay logic lives (and is tested) in
// xeas_feed; this file is smoke-tested manually against a real day.

#include "xeas/book.h"
#include "xeas/itch.h"
#include "xeas/replay.h"
#include "xeas/router.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

using namespace xeas;

int usage() {
    std::cerr << "usage: itch_replay <file> [--sweep-every N] [--progress N] [--quiet]\n"
                 "  <file>: an uncompressed .NASDAQ_ITCH50 file (gunzip *.gz first)\n"
                 "  --sweep-every N   messages between invariant sweeps (default 25000000)\n"
                 "  --progress N      messages between progress lines (default 10000000)\n"
                 "  --quiet           no progress lines\n";
    return 1;
}

constexpr struct { char type; const char* name; } kTypeNames[] = {
    {'S', "System Event"},
    {'R', "Stock Directory"},
    {'H', "Stock Trading Action"},
    {'Y', "Reg SHO Restriction"},
    {'L', "Market Participant Position"},
    {'V', "MWCB Decline Level"},
    {'W', "MWCB Status"},
    {'K', "IPO Quoting Period Update"},
    {'J', "LULD Auction Collar"},
    {'h', "Operational Halt"},
    {'A', "Add Order"},
    {'F', "Add Order (MPID)"},
    {'E', "Order Executed"},
    {'C', "Order Executed with Price"},
    {'X', "Order Cancel"},
    {'D', "Order Delete"},
    {'U', "Order Replace"},
    {'P', "Trade (non-cross)"},
    {'Q', "Cross Trade"},
    {'B', "Broken Trade"},
    {'I', "Net Order Imbalance"},
    {'N', "Retail Price Improvement"},
    {'O', "Direct Listing with Capital Raise"},
};

const char* type_name(char type) {
    for (const auto& entry : kTypeNames) {
        if (entry.type == type) return entry.name;
    }
    return nullptr;
}

void print_stats(const ReplayStats& stats, const BookRouter& router,
                 std::ostream& out) {
    out << "--- message counts by type ---\n";
    for (int t = 0; t < 256; ++t) {
        if (stats.count_by_type[t] == 0) continue;
        const char c = static_cast<char>(t);
        const char* name = type_name(c);
        out << "  '" << c << "'  " << stats.count_by_type[t] << "  "
            << (name != nullptr ? name : "(unknown type)") << "\n";
    }
    std::uint64_t final_orders = 0;
    std::uint64_t final_levels = 0;
    router.for_each_book([&](std::uint16_t, const Book& book) {
        final_orders += book.order_count();
        final_levels += book.level_count(Side::Bid) + book.level_count(Side::Ask);
    });
    out << "--- totals ---\n"
        << "  messages            " << stats.messages_total << "\n"
        << "  bytes               " << stats.bytes_consumed << "\n"
        << "  events applied      " << stats.events_applied << "\n"
        << "  books created       " << stats.books_created << "\n"
        << "  directory entries   "
        << stats.count_by_type[static_cast<unsigned char>('R')] << "\n"
        << "  crossed episodes    " << stats.crossed_episodes << "\n"
        << "  crossed symbols     " << stats.crossed_symbols << "\n"
        << "  ts regressions      " << stats.timestamp_regressions << "\n"
        << "  live orders peak    " << stats.live_orders_peak << "\n"
        << "  live orders final   " << final_orders << "\n"
        << "  price levels final  " << final_levels << "\n"
        << "  invariant sweeps    " << stats.invariant_sweeps << "\n"
        << "  elapsed             " << stats.elapsed_seconds << " s\n";
    if (stats.elapsed_seconds > 0.0) {
        out << "  throughput          "
            << static_cast<std::uint64_t>(
                   static_cast<double>(stats.messages_total) / stats.elapsed_seconds)
            << " msgs/s, "
            << (static_cast<double>(stats.bytes_consumed) / (1024.0 * 1024.0)) /
                   stats.elapsed_seconds
            << " MiB/s\n";
    }
}

void print_book_error(const BookError& e, std::ostream& out) {
    const Event& ev = e.event();
    out << "book error: " << e.what() << "\n"
        << "  event: type=" << static_cast<int>(ev.type)
        << " timestamp=" << ev.timestamp << " order_id=" << ev.order_id
        << " side=" << (ev.side == Side::Bid ? "Bid" : "Ask")
        << " price=" << ev.price << " quantity=" << ev.quantity
        << " new_order_id=" << ev.new_order_id << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path file;
    bool have_file = false;
    ReplayOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--quiet") {
            opts.progress_every = 0;
        } else if (arg == "--sweep-every" && i + 1 < argc) {
            opts.invariant_sweep_every = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--progress" && i + 1 < argc) {
            opts.progress_every = std::strtoull(argv[++i], nullptr, 10);
        } else if (!arg.empty() && arg.front() == '-') {
            return usage();
        } else if (!have_file) {
            file = argv[i];
            have_file = true;
        } else {
            return usage();
        }
    }
    if (!have_file) return usage();

    std::optional<ItchReader> reader;
    try {
        reader.emplace(file);
    } catch (const FeedError& e) {
        // Unopenable file is a usage-level failure: exit 1, not 2 (spec §7).
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    BookRouter router;
    ReplayStats stats;
    int exit_code = 0;
    try {
        replay_stream(*reader, router, opts, stats, std::cerr);
    } catch (const FeedError& e) {
        std::cerr << "feed error: " << e.what() << "\n";
        exit_code = 2;
    } catch (const BookError& e) {
        print_book_error(e, std::cerr);
        exit_code = 3;
    } catch (const ReplayError& e) {
        std::cerr << "replay error: " << e.what() << "\n";
        exit_code = 4;
    }
    // Stats-so-far are the most useful debugging artifact: print them even
    // (especially) on an abort.
    print_stats(stats, router, exit_code == 0 ? std::cout : std::cerr);
    return exit_code;
}
```

- [ ] **Step 2: Add the executable to `CMakeLists.txt`** (after the `xeas_feed` block):

```cmake
add_executable(itch_replay src/itch_replay_main.cpp)
target_link_libraries(itch_replay PRIVATE xeas_feed)
if(NOT MSVC)
  target_compile_options(itch_replay PRIVATE -Wall -Wextra)
endif()
```

- [ ] **Step 3: Build and smoke-test** (run each; check output AND `echo $?`):

```bash
cmake -S . -B build && cmake --build build

./build/itch_replay; echo "exit=$?"
# Expected: usage text on stderr, exit=1

./build/itch_replay /nonexistent/file.itch; echo "exit=$?"
# Expected: "error: cannot open file ...", exit=1

printf '\x00\x00' > /tmp/xeas_sentinel.itch
./build/itch_replay /tmp/xeas_sentinel.itch --quiet; echo "exit=$?"
# Expected: stats block with 0 messages, 2 bytes, exit=0

printf '\x00\x13D\x00\x01\x00\x00\x00\x00\x00\x00\x00\x64\x00\x00\x00\x00\x00\x00\x00\x2a' > /tmp/xeas_orphan.itch
./build/itch_replay /tmp/xeas_orphan.itch --quiet; echo "exit=$?"
# One 'D' (delete order 42) with no prior add -> BookError.
# Expected: "book error: Delete: unknown order_id" + event fields + stats on stderr, exit=3

rm /tmp/xeas_sentinel.itch /tmp/xeas_orphan.itch
```

- [ ] **Step 4: Full suite still green**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/itch_replay_main.cpp CMakeLists.txt
git commit -m "feat: itch_replay CLI - args, exit-code mapping, stats printing"
git push
```

---

## Task 11 — Docs: DESIGN.md amendment, README Milestone 2

**Files:**
- Modify: `DESIGN.md` (architecture diagram + decoder/normalizer bullets, per spec §2 rationale)
- Modify: `README.md` (status table, Milestone 2 section, layout, expected test count)

**Interfaces:** none (docs only).

- [ ] **Step 1: Amend `DESIGN.md`.** Replace the diagram (line 35) and the two bullets under it (lines 38–39) with:

```markdown
```
ITCH file → [Decoder/Normalizer] → Event → [Book core] → queries / stats
```

- **Decoder/Normalizer** — fused: for ITCH the normalization is nearly the
  identity (`'B'/'S'` → `Side`, drop MPID, widen price/timestamp), so a typed
  per-message struct layer would be pure transcription. The split survives as
  a knowledge boundary: `src/itch.cpp` is the only code that knows ITCH byte
  layouts, and its output is the canonical `Event`. A future second venue adds
  its own `decode_*` producing the same `Event`.
- **Book core** — pure `apply(Book&, Event)`; no I/O, no clocks, fully testable
```

(The `- **Book core**` bullet already exists — keep it; only the diagram and the two decoder/normalizer bullets change.)

- [ ] **Step 2: Update `README.md`:**
  - Status table: `2. ITCH replay — decode and replay a real trading day | ✅ done`.
  - After the "What's here (Milestone 1)" section, add:

```markdown
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
```

  - Update the expected test line in Build & test to the real total from Step 3 (roughly `104 tests`: 57 + 35 + 12 differential/replay — use the number ctest prints).
  - Layout section: add the new files:

```
include/xeas/itch.h    FeedError, FramedMessage, ItchReader, decode functions
include/xeas/router.h  BookRouter (one Book per stock locate)
include/xeas/replay.h  ReplayOptions, ReplayStats, ReplayError, replay_stream()
src/itch.cpp           framing + fused ITCH decoder/normalizer
src/router.cpp         locate routing
src/replay.cpp         replay driver
src/itch_replay_main.cpp  the itch_replay CLI
tests/itch_fixture.h   ItchBuilder — synthetic ITCH binary fixtures
tests/itch_test.cpp    Framing / Decode / Router suites
tests/replay_test.cpp  Replay / Differential suites
```

- [ ] **Step 3: Final verification**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: `100% tests passed`. Record the exact total and make sure README states it.

- [ ] **Step 4: Commit**

```bash
git add DESIGN.md README.md
git commit -m "docs: amend DESIGN.md to fused decoder/normalizer; README Milestone 2"
git push
```

---

## Verification (end-to-end, after all tasks)

1. `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` — 100% pass, zero warnings from `-Wall -Wextra` on the new targets.
2. CLI smoke matrix from Task 10 Step 3 — exit codes 1/1/0/3 as stated.
3. Layering check: `xeas_core` must not link or include anything from `itch.h`/`router.h`/`replay.h` — `grep -rn 'itch\.h\|router\.h\|replay\.h' src/book.cpp include/xeas/book.h include/xeas/event.h` returns nothing.
4. Optional (user-supplied data, not part of CI): `gunzip` a real `*.NASDAQ_ITCH50.gz` day, run `./build/itch_replay <file>`, record the stats block once as the manual golden baseline (spec §7). Expect crossed episodes > 0 around halts/crosses and exit 0.

## Execution Handoff

Plan approved for **Subagent-Driven** execution (user's choice): use **superpowers:subagent-driven-development** — fresh subagent per task, two-stage review between tasks. Remember the "Before Task 1" block (branch off updated `main`, copy this plan to `docs/superpowers/plans/2026-08-19-itch-replay.md`, commit it) before dispatching Task 1.
