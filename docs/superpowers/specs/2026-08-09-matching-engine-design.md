# XEAS Exchange — Matching Engine & Order Book Design

**Date:** 2026-08-09
**Status:** Approved

## Overview

A single-instrument matching engine with an order book, written in C++20, delivered as a library plus a small CLI demo. The engine accepts limit and market orders, matches them under price-time priority, supports partial fills, and cancels orders in O(1). Unit tests use GoogleTest; the build system is CMake.

## Decisions

| Topic | Decision |
|---|---|
| Scope | Library + CLI demo |
| Instruments | Single instrument (one order book, no symbol field) |
| Price/quantity representation | 64-bit integers (price in ticks, quantity in whole units) |
| Market order remainder | Filled as far as possible, remainder cancelled (never rests) |
| Fill reporting | `submit_*` returns the list of trades it caused |
| Threading | Single-threaded, no locks |
| Order IDs | Engine-assigned, monotonically increasing from 1 |
| Book storage | `std::map` of price → level (sorted tree) |
| Book queries | Best bid/ask and depth snapshot |
| CLI input | Interactive stdin and script file, same parser |
| Toolchain | C++20, CMake, GoogleTest |

## Core Types (`types.h`)

- `Price` — `int64_t`, in ticks. Must be > 0 for limit orders.
- `Quantity` — `int64_t`. Must be > 0.
- `OrderId` — `uint64_t`, assigned by the engine starting at 1. `0` is never a valid ID.
- `Side` — enum: `Buy`, `Sell`.
- `Trade` — `{ OrderId maker_id, OrderId taker_id, Price price, Quantity quantity }`. The *maker* is the resting order; the *taker* is the incoming order. The trade price is always the maker's price.
- `Order` — internal representation: `{ OrderId id, Side side, Price price, Quantity remaining }`.

## Components

### OrderBook (`orderbook.h/.cpp`)

Pure storage; contains no matching rules. Internal structure:

- Bids: `std::map<Price, Level, std::greater<Price>>` — best (highest) bid first.
- Asks: `std::map<Price, Level, std::less<Price>>` — best (lowest) ask first.
- `Level`: FIFO queue of orders at one price (`std::list<Order>`), preserving arrival order.
- Cancellation index: `std::unordered_map<OrderId, locator>` where the locator holds the side, the level, and the `std::list` iterator for the order — giving O(1) removal (list iterators remain valid under insertion/erasure elsewhere).

Operations:

- `add(Order)` — insert a resting order at the back of its price level; index it.
- `remove(OrderId) → bool` — O(1) cancel; erases the level if it becomes empty; `false` if the ID is unknown.
- Best-price access for the matching loop: peek the front order of the best opposite level; reduce or pop it as fills occur (index updated on full fill).
- `best_bid() / best_ask() → std::optional<Price>` — empty when that side has no orders.
- `depth(Side) → vector<{Price, Quantity}>` — total resting quantity per price level, best price first.

### MatchingEngine (`matching_engine.h/.cpp`)

Owns the `OrderBook`, assigns order IDs, and implements matching semantics. Public API:

- `submit_limit(Side, Price, Quantity) → SubmitResult`
- `submit_market(Side, Quantity) → SubmitResult`
- `cancel(OrderId) → bool`
- `best_bid()`, `best_ask()`, `depth(Side)` — passed through from the book.

`SubmitResult` = `{ OrderId id, std::vector<Trade> trades, bool accepted }`. Rejected orders have `accepted == false`, `id == 0`, and no trades. Every accepted order gets an ID, including market orders and limit orders that fill completely (their ID simply never appears on the book).

## Matching Semantics

Price-time priority throughout:

1. **Incoming limit buy at price P:** while remaining quantity > 0 and `best_ask() <= P`, trade against the front (oldest) order of the best ask level. Trade quantity = min of the two remaining quantities; trade price = the resting order's price. Fully filled resting orders pop off the level. If the incoming order still has quantity when no ask crosses, it rests on the bid side at P. Limit sells mirror this against bids.
2. **Incoming market order:** same loop with no price bound — it consumes the best opposite levels until filled or that side is empty. Any remainder is cancelled; a market order never rests on the book.
3. **Partial fills:** every trade decrements both orders' remaining quantity. A resting order stays on the book, keeping its time priority, until its remaining quantity reaches zero or it is cancelled.
4. **Determinism:** single-threaded, orders processed strictly in submission sequence; matching is fully deterministic.

## Error Handling

- `quantity <= 0` (any order) or `price <= 0` (limit order): rejected — `accepted == false`, book untouched.
- `cancel` of an unknown, fully filled, or already-cancelled ID: returns `false`. Not an error condition.
- Market order against an empty opposite side: accepted, zero trades, nothing rests.
- No exceptions in the public API; all failure modes are expressed in return values.

## CLI Demo (`cli/main.cpp`)

Thin wrapper over `MatchingEngine`; contains no matching logic. Commands, one per line:

```
buy  limit  <price> <qty>    sell limit  <price> <qty>
buy  market <qty>            sell market <qty>
cancel <id>
book                         # print depth snapshot, both sides
help
quit
```

After each order the CLI prints the assigned ID and any trades; `book` prints the depth snapshot. With a filename argument it executes the file's commands and exits; with no argument it reads stdin interactively. Same parser for both. Unrecognized or malformed lines print an error and are skipped. The parser lives in a separate translation unit (`cli/parser.h/.cpp`) so it is unit-testable.

## Testing (GoogleTest)

- **`orderbook_test.cpp`** — structural: add/remove, FIFO order within a level, empty-level cleanup, cancellation index correctness, `best_bid/best_ask/depth` on empty and populated books.
- **`matching_engine_test.cpp`** — behavioral:
  - Limit orders rest when they don't cross; cross when they do; trade at maker price.
  - Price priority: better-priced resting orders fill first.
  - Time priority: FIFO at equal price.
  - Partial fills: incoming larger than resting, resting larger than incoming, fills spanning multiple levels.
  - Market orders: full fill, partial fill with remainder cancelled, empty-book no-op.
  - Cancellation: resting order, partially filled order, unknown ID → `false`, cancelled order no longer matches.
  - Validation: zero/negative quantity and price rejected.
- **`cli_parser_test.cpp`** — command parsing, including malformed input.

## Project Layout

```
CMakeLists.txt
src/        types.h, orderbook.h, orderbook.cpp,
            matching_engine.h, matching_engine.cpp
cli/        main.cpp, parser.h, parser.cpp
tests/      orderbook_test.cpp, matching_engine_test.cpp, cli_parser_test.cpp
```

GoogleTest is fetched via CMake `FetchContent`. Targets: `xeas_engine` (library), `xeas_cli` (executable), `xeas_tests` (test runner).

## Out of Scope (deliberately)

Multiple symbols, caller-provided order IDs, time-in-force flags (IOC/FOK/GTC), order modification/replace, thread safety, persistence, networking. All can be layered on later without redesigning the core.
