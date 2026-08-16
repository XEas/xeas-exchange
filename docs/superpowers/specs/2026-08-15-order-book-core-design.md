# Order Book Core — Design Spec

**Date:** 2026-08-15
**Milestone:** 1 (see `DESIGN.md`)
**Status:** Approved decisions from design Q&A; ready for implementation planning.

## 1. Purpose & scope

The book core is a pure, deterministic, venue-agnostic **L3 (market-by-order) limit
order book** state machine. It consumes canonical `Event`s and maintains the exact
book state; replaying the same events always produces the same book.

**In scope**
- The canonical `Event` struct (the hard boundary between feed handlers and the book)
- The `Book` type: data structures and `apply()` semantics
- Query API (BBO, level size, order lookup)
- `check_invariants()` (out-of-band consistency checker)
- Project skeleton: C++20, CMake, GoogleTest; unit-test event tapes

**Out of scope** (later milestones)
- ITCH decoder and normalizer (Milestone 2) — the ITCH mapping below is informative only
- Symbol routing — a `Book` is strictly single-instrument; a router outside the core
  dispatches events to one `Book` per symbol
- Benchmarks and optimized data structures (Milestone 3) — the API in this spec must
  not change when the internals are swapped
- Live feeds, matching, multi-threading

## 2. Canonical `Event`

All fields are fixed-width integers. No floating point anywhere.

| Field | Type | Meaning |
|---|---|---|
| `type` | enum | `Add`, `Cancel`, `Delete`, `Execute`, `Replace` |
| `timestamp` | `uint64_t` | Nanoseconds since midnight. Carried through, never interpreted by the book. |
| `order_id` | `uint64_t` | Subject order. Unique per instrument per day. |
| `side` | enum `Bid`/`Ask` | Meaningful on `Add` only; other types resolve side via order lookup. |
| `price` | `int64_t` | Price in ticks (ITCH: fixed point ×10,000). Meaningful on `Add` and `Replace`. |
| `quantity` | `uint32_t` | Shares. Meaning depends on `type` (see semantics table). |
| `new_order_id` | `uint64_t` | `Replace` only: the id of the replacement order. |

Event type semantics:

| Type | Semantics |
|---|---|
| `Add` | New resting order of `quantity` shares at `price` on `side`. Enqueued at the back of its price level (time priority). |
| `Cancel` | Reduce the order's remaining shares by `quantity`. Reaching zero removes the order. |
| `Delete` | Remove the order entirely, whatever its remaining quantity. `quantity`/`price` ignored. |
| `Execute` | Same book effect as `Cancel` (shares come off the front-resting order); kept distinct because it is a trade, not a pull — useful for later stats. Execution price does not affect book state. |
| `Replace` | Atomically remove order `order_id` and insert a new order `new_order_id` with `quantity` shares at `price`, on the **same side**, at the **back** of its (possibly new) level's queue — time priority is lost, matching ITCH semantics. |

**ITCH mapping (informative, owned by the Milestone 2 normalizer):**
`A`/`F` → Add, `X` → Cancel, `D` → Delete, `E`/`C` → Execute, `U` → Replace.
This mapping exists here only to show the `Event` struct is sufficient for ITCH replay.

## 3. `Book` semantics & baseline structures

A `Book` is single-instrument and single-threaded. Structure:

- Two sides (bid, ask), each an ordered collection of **price levels**, best price first.
- Each level: a FIFO queue of orders (strict time priority) plus a **cached aggregate
  size** maintained incrementally on every mutation.
- A global order index for O(1)-amortized lookup by `order_id`.

Baseline (correctness-first, per `DESIGN.md` — measure before optimizing):

- `std::map<Price, Level, std::greater<>>` for bids, `std::map<Price, Level, std::less<>>` for asks
- `Level` = FIFO container of orders + `int64_t total_shares`
- `std::unordered_map<OrderId, OrderHandle>` where the handle locates the order in its
  level in O(1) (side, level iterator, position handle)

Structural rules:

- An order whose remaining quantity reaches zero is removed immediately.
- A level whose last order is removed is removed immediately — **no empty levels exist**.
- The book does not match: a crossed incoming `Add` is stored as given (crossing is an
  invariant violation surfaced by `check_invariants()`, not handled by `apply()`).
  With well-formed exchange data this does not occur.

The public API is `apply(Book&, const Event&)` (pure state mutation, returns `void`)
plus the queries in §5. Milestone 3 swaps internals (flat tick-indexed array, intrusive
lists, memory pool) behind this same API.

## 4. Error policy — strict

`apply()` throws `BookError` (carrying the offending event and a reason) on:

- `Cancel`/`Delete`/`Execute`/`Replace` referencing an unknown `order_id`
- `Add` with an `order_id` that already exists; `Replace` with a `new_order_id` that
  already exists (including `new_order_id == order_id`)
- `Cancel`/`Execute` with `quantity` exceeding the order's remaining shares, or zero
- `Add`/`Replace` with zero `quantity` or non-positive `price`

After a throw, book state is unspecified; the replay driver must abort.

**Rationale:** replay of a well-formed historical ITCH day is deterministic — any
violation means a bug in the decoder, normalizer, or book. Fail loud, fix the bug.

## 5. Query API

| Query | Returns |
|---|---|
| `best_bid()` / `best_ask()` | `std::optional<{price, total_shares}>` — empty if side is empty |
| `size_at(side, price)` | Aggregate shares at that level; `0` if no such level |
| `find_order(order_id)` | `std::optional<{side, price, remaining_qty}>` |
| `order_count()` | Total live orders |
| `level_count(side)` | Number of price levels on a side |

Queries are `const`, allocation-free, and O(1) or O(log levels).

## 6. Invariants — `check_invariants(const Book&)`

A standalone function (not called inside `apply()` — the hot path stays clean) that
walks the entire book and verifies:

1. Best bid < best ask when both sides are non-empty (book not crossed/locked)
2. Every level's cached `total_shares` equals the sum of its orders' remaining shares
3. No empty levels; no zero-quantity orders
4. Levels on each side are strictly sorted (descending bids, ascending asks)
5. The order index and the levels agree exactly: every indexed order is present in
   exactly one level, and every order in a level is indexed (same count both ways)

Returns a result listing violations (or throws — implementation's choice, decided in
the implementation plan). Called by unit tests after every event, by fuzz tests, and
periodically during full-day replay.

## 7. Project skeleton & testing

```
include/xeas/event.h      — Event, Side, enums, type aliases (Price, Qty, OrderId)
include/xeas/book.h       — Book, apply(), queries, check_invariants(), BookError
src/book.cpp              — implementation
tests/book_test.cpp       — event-tape unit tests
CMakeLists.txt            — C++20; library target `xeas_core`; GoogleTest via FetchContent
```

**Test strategy** (per `DESIGN.md`, TDD):

1. **Hand-written event tapes** — small scripted sequences asserting queries and
   `check_invariants()` after every event: add/cancel/delete lifecycles, partial
   executes, replace (price move, priority loss), level creation/removal, BBO updates.
2. **Error-policy tests** — every §4 condition throws `BookError`.
3. Later (out of this spec): invariant fuzzing with random valid tapes, then golden
   replay of a full real ITCH day (Milestone 2).
