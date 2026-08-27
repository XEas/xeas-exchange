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
| 3 (banded flat array + bitmaps + overflow) [^pwr3] | steady | baseline | 4.71 | 3.85 | 212.3 | 1.00x | 820 [^rss] |
| 3 (banded flat array + bitmaps + overflow) | steady | new | 6.71 | 5.73 | 149.0 | 1.42x | 775 [^rss] |
| 3 (banded flat array + bitmaps + overflow) | insert | baseline | 5.49 | 5.20 | 182.1 | 1.00x | 1225 [^comb] |
| 3 (banded flat array + bitmaps + overflow) | insert | new | 10.66 | 10.36 | 93.8 | 1.94x | 1354 [^comb] |
| 3 (banded flat array + bitmaps + overflow) | deep | baseline | 2.75 | 2.10 | 363.8 | 1.00x | 1397 [^comb] |
| 3 (banded flat array + bitmaps + overflow) | deep | new | 3.48 | 3.19 | 287.6 | 1.26x | 1397 [^comb] |
| 4 (open-addressing order index) [^pwr4] | steady | baseline | 4.89 | 4.69 | 204.7 | 1.00x | 820 [^rss] |
| 4 (open-addressing order index) | steady | new | 25.83 | 24.28 | 38.7 | 5.29x | 820 [^rss] |
| 4 (open-addressing order index) | insert | baseline | 5.55 | 5.32 | 180.1 | 1.00x | 1249 [^comb] |
| 4 (open-addressing order index) | insert | new | 27.30 | 22.78 | 36.6 | 4.92x | 1619 [^comb] |
| 4 (open-addressing order index) | deep | baseline | 2.97 | 2.95 | 337.3 | 1.00x | 1619 [^comb] |
| 4 (open-addressing order index) | deep | new | 6.04 | 4.45 | 165.5 | 2.04x | 1619 [^comb] |

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
[^pwr3]: Phase-3 rows were measured on battery power, 95% draining to 94%
across the three runs (`pmset -g batt`: "Now drawing from 'Battery Power'",
discharging) — the same non-AC, non-quiet-machine caveat as phases 1-2, and
close to phase 2's starting point rather than phase 1's. No `min=0.01`
sleep-artifact outliers were observed in the phase-3 runs.
[^pwr4]: Phase-4 rows were measured on battery power, 91% draining to 90%
across the three runs (`pmset -g batt`: "Now drawing from 'Battery Power'",
discharging) — the same non-AC, non-quiet-machine caveat as phases 1-3. No
`min=0.01` sleep-artifact outliers were observed in the phase-4 runs; all
`min` values tracked their `median` closely (worst case `insert`/new,
22.78 vs. 27.30 Mmsg/s).

## Macrobenchmark: full-day `itch_replay`

Command: `./build-release/itch_replay <day.NASDAQ_ITCH50> --quiet`

| phase | file | msgs/s | MiB/s | peak RSS MiB | live orders peak |
|---|---|---|---|---|---|
| 1 | _pending: requires local day file_ | | | | |

**Book-vs-feed split (spec §2):** compare `book_bench steady` Mmsg/s with
end-to-end `itch_replay` msgs/s. Close ⇒ the book dominates and feed-side
optimization stays out of scope; feed-dominant ⇒ a finding for a future
milestone, not license to widen this one.

**Status (through phase 4):** still pending. No real, uncompressed
`*.NASDAQ_ITCH50` day file is available in this environment, so there is no
macro row to fill and no book-vs-feed split to compute — that comparison
awaits the same day file noted in the phase-1 macro row above. No numbers are
invented here in its place.

## Band constants (phase 3 tuning)

`kInitialBandTicks = 4096`, `kMaxBandTicks = 262144` — **still provisional**.
No real-day `*.NASDAQ_ITCH50` file is available in this environment, so there
is no per-symbol price-range histogram to tune against, and these constants
are left unchanged in `src/book.cpp` for this task. They remain a named,
documented estimate, not a measured one. When a day file becomes available,
compute the per-symbol quoted-range histogram (share of a symbol's range
covered by a 262,144-tick window around its first quote) and revisit both
constants together with the re-anchor question below.

**Review finding, spec-accepted as documentation-only for this phase:** the
band anchors on the first add for a side (`band_anchor`, `kInitialBandTicks`
wide, centered on that first price) and it **never re-anchors relative to
current trading activity** — the only thing that ever moves `lo` afterward is
`band_grow_to_cover`, which re-centers the window to cover a specific
out-of-band price *while growing toward it*, capped at `kMaxBandTicks`. Once
the window has grown to the full `kMaxBandTicks` (262,144 ticks) width, that
growth path is exhausted: any further out-of-band price makes
`band_grow_to_cover` return `false` (span would exceed the cap), the price
goes to `overflow`, and — critically — nothing thereafter ever recenters
`lo` again. The window's position is then pinned for the rest of the day,
regardless of where trading subsequently moves.

The failure mode this creates: a single historical outlier seen early on a
side — a fat-finger print, an opening-cross artifact, a halt/reopen far from
the prior close — can force the window to jump straight to `kMaxBandTicks`
in one growth step (doubling-to-cover-span picks whatever size is needed),
anchored around that outlier before the symbol's real intraday range is
known. From then on the window is pinned at that size and position. As the
day's actual mid subsequently drifts away from that early, spurious anchor,
prices near the current mid can fall outside the pinned `[lo, lo +
262,144)` window — up to the full window width, a bit over $26 at the
project's ×10,000 fixed-point scale (262,144 ticks ÷ 10,000 = $26.2144), if
the outlier happened to sit near one edge of the eventual window rather than
its center — sending all new levels near the current mid to the `overflow`
`std::map` instead of the O(1) band path. Correctness is unaffected either
way (`overflow` is exact and fully checked by `check_invariants`, including
by the new `InvariantsV2.InBandPriceInOverflowIsFlagged` test added in this
phase); only the speed characteristic degrades from array/bitmap O(1) to
`std::map` O(log n) for the affected side, for the remainder of the day.

The candidate mitigation is **re-anchor-on-drain**: when a side's band goes
fully empty (no occupied in-band slot; `overflow` may still be non-empty),
recenter `lo` around the current best price instead of leaving the stale,
possibly outlier-pinned window in place. This is not implemented here — it
needs the same real-day histogram data to judge whether drain-driven
re-anchoring is worth its own cost (a full slots/words rebuild, same
mechanism as `band_grow_to_cover`'s existing rebuild) versus simply accepting
the overflow-map fallback as a rare, correctness-preserving degradation. Left
as a follow-up to evaluate once a day file and histogram are available, not a
phase-3 code change.
