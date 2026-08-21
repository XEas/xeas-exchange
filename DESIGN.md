# xeas-exchange — Design

## What this is

A single-threaded, high-performance **order book reconstruction engine** in C++.

It rebuilds the full limit order book of an exchange by consuming a stream of
order-level events (add, cancel, replace, execute) and maintaining an exact
in-memory replica of the book at every point in time. Primary data source:
historical **NASDAQ TotalView-ITCH** files, replayed from disk.

**System type:** an event-driven state machine / event-sourcing consumer.
The book is pure state; the event stream is the single source of truth;
replaying the same events always produces the same book (deterministic).

## Scope

**In scope**
- Full-depth L3 book (market-by-order): per-price FIFO queues with time priority
- Binary ITCH decoder for historical replay from file
- Canonical internal `Event` struct — the book core is venue-agnostic
- Integer arithmetic throughout (price ticks, share lots — no floating point)
- Invariant checking (best bid < best ask, level totals consistent, no empty levels)
- Throughput benchmark on full-day replays, with optimization pass
  (flat tick-indexed price array, intrusive per-level lists, order memory pool)

**Out of scope (for now)**
- Live network feeds, snapshot/resync protocol (sequencer) — possible later phase
- Matching engine / order entry — this system consumes events, it does not match
- Multi-threading — one instrument's stream is sequential by nature

## Architecture

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

## Milestones

1. **Book core** — data structures + `apply()`, driven by unit-test event tapes
2. **ITCH replay** — decode a real trading day from file, replay through the book
3. **Benchmark & optimize** — measure msgs/sec, then swap in fast data structures
   with before/after numbers
4. **(Optional) Live mode** — sequencer + WebSocket feed reusing the same core

## Key decisions

- Prices and quantities are integers everywhere (ITCH prices are ×10,000 fixed-point)
- The normalized `Event` struct is the hard boundary between feed handlers and the book
- Correctness first: slow `std::map` baseline before any optimization, so gains are measured
- Testing: hand-written event tapes → invariant fuzzing → golden replay of a full real day
