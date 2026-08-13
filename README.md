# xeas-exchange

An order book reconstruction engine, built in C++.

Rebuilds and maintains the state of an exchange's limit order book from a snapshot and a stream of incremental change events (order adds, cancels, modifications, and executions).

## How it works

1. Load an initial book snapshot.
2. Apply incremental events in sequence order to keep a local replica of the book in sync.
3. Detect sequence gaps and resynchronize from a fresh snapshot when messages are lost.

## Features

- Full-depth order book (bids and asks, sorted by price level)
- Integer tick/lot arithmetic — no floating point in book state
- O(1) order lookup for cancels and modifications
- Deterministic replay: the same event log always reconstructs the same book

## Use cases

- Live market data consumption for trading systems
- Historical replay and backtesting from recorded event logs
- State recovery and auditing for matching engines
