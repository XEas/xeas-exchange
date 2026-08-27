# BENCH — Milestone 3 numbers & methodology

Every number here is reproducible from its own row: fixed seed, event count,
and the commands below.

## Rules

- **Release build** (must be stated — the project's default build type is empty):
  `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release`
- Median of 5 repeats (min also recorded), single-threaded, quiet machine;
  note CPU frequency scaling / power state next to the machine line.
- `book_bench` peak RSS includes the pre-generated event tape (~48 B/event),
  identical across impls: compare impls with separate `--impl new` /
  `--impl baseline` runs; absolute footprint lives in the macro table.
- Macro runs use a real uncompressed `*.NASDAQ_ITCH50` day (never checked in),
  same file and machine before/after each phase.

## Machine

- CPU: Apple M5 (`sysctl -n machdep.cpu.brand_string`)
- RAM: 24 GiB (`sysctl -n hw.memsize` = 25769803776 bytes)
- OS: macOS 26.4 (`sw_vers -productVersion`)
- Compiler: Apple clang version 21.0.0 (clang-2100.1.1.101) (`c++ --version`)
- Power/frequency note: Apple Silicon has no user-visible frequency-scaling
  control. The machine was measured **on battery power** (`pmset -g batt`
  reported "Now drawing from 'Battery Power'", 87%, discharging) rather than
  AC — not the quiet-machine ideal. Run-to-run median variance was visibly
  higher than expected as a result (e.g. `steady`/baseline median moved
  between 3.31 and 4.80 Mmsg/s across repeated invocations of the same
  command); see footnote below the micro table.

## Microbenchmark: `book_bench`

Command of record, per phase:
`./build-release/book_bench --workload all --events 10000000 --warmup 1000000 --seed 1 --repeat 5`
plus per-impl RSS runs with `--impl new` and `--impl baseline`.

| phase | workload | impl | Mmsg/s median | Mmsg/s min | ns/event | speedup vs baseline | peak RSS MiB |
|---|---|---|---|---|---|---|---|
| 1 (harness, baseline internals) | steady | baseline | 4.80 | 3.48 | 208.5 | 1.00x | 819 [^rss] |
| 1 (harness, baseline internals) | steady | new | 3.64 | 0.01 [^min] | 274.8 | 0.76x | 817 [^rss] |
| 1 (harness, baseline internals) | insert | baseline | 3.67 | 3.42 | 272.4 | 1.00x | 1316 [^comb] |
| 1 (harness, baseline internals) | insert | new | 3.64 | 0.01 [^min] | 274.8 | 0.99x | 1512 [^comb] |
| 1 (harness, baseline internals) | deep | baseline | 2.02 | 1.82 | 495.9 | 1.00x | 1512 [^comb] |
| 1 (harness, baseline internals) | deep | new | 2.03 | 0.01 [^min] | 492.9 | 1.01x | 1512 [^comb] |
| 2 (order pool + intrusive lists) [^pwr2] | steady | baseline | 4.34 | 4.02 | 230.3 | 1.00x | 820 [^rss] |
| 2 (order pool + intrusive lists) | steady | new | 6.03 | 5.47 | 165.9 | 1.39x | 782 [^rss] |
| 2 (order pool + intrusive lists) | insert | baseline | 5.37 | 5.32 | 186.1 | 1.00x | 1289 [^comb] |
| 2 (order pool + intrusive lists) | insert | new | 6.27 | 6.10 | 159.4 | 1.17x | 1556 [^comb] |
| 2 (order pool + intrusive lists) | deep | baseline | 2.66 | 2.09 | 376.1 | 1.00x | 1558 [^comb] |
| 2 (order pool + intrusive lists) | deep | new | 2.73 | 2.56 | 365.8 | 1.03x | 1558 [^comb] |

[^rss]: `steady` peak RSS is from the separate, clean per-impl runs
(`--impl baseline` / `--impl new`, run in isolation) so the ~48 B/event tape
footprint isn't shared with the other impl's process.
[^comb]: `insert`/`deep` peak RSS is read off the combined `--workload all`
run (both impls share one process's peak, one workload at a time within that
process) and is therefore an **upper bound**, not a clean per-impl figure;
the `steady` row is the one clean comparison.
[^min]: The `new` impl showed a `min=0.01 Mmsg/s` outlier repeatably across
independent invocations (with and without `caffeinate -i`, standalone and
combined runs) while `baseline` never did on the same commands. This
persisted under `caffeinate`, so it is not machine-sleep noise; it looks like
a real one-repeat-in-five stall specific to the `new` code path (consistent
with `new`'s higher peak RSS — plausibly a first-touch page-fault /
allocator-growth cost). Since phase-1 impls share the same internals per the
milestone plan, this is flagged as a measurement finding to revisit once the
impls diverge, not something papered over here.
[^pwr2]: Phase-2 rows were measured on battery power at 100% (`pmset -g
batt`: "Now drawing from 'Battery Power'", discharging), vs. 87% for phase
1's measurements — a different point on the same (non-AC, non-quiet-machine)
power state, noted per the Machine section's caveat above. No `min=0.01`
sleep-artifact outliers were observed in the phase-2 runs (unlike phase 1's
`new`/steady min, see [^min]).

## Macrobenchmark: full-day `itch_replay`

Command: `./build-release/itch_replay <day.NASDAQ_ITCH50> --quiet`

| phase | file | msgs/s | MiB/s | peak RSS MiB | live orders peak |
|---|---|---|---|---|---|
| 1 | _pending: requires local day file_ | | | | |

**Book-vs-feed split (spec §2):** compare `book_bench steady` Mmsg/s with
end-to-end `itch_replay` msgs/s. Close ⇒ the book dominates and feed-side
optimization stays out of scope; feed-dominant ⇒ a finding for a future
milestone, not license to widen this one.

## Band constants (phase 3 tuning)

`kInitialBandTicks = 4096`, `kMaxBandTicks = 262144` — provisional, named in
`src/book.cpp`. Finalized from a real-day per-symbol price-range histogram
(share of a symbol's quoted range covered by a 262,144-tick window around its
first quote); recorded here when measured.
