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
  between 3.31 and 4.80 Mmsg/s across invocations of differing command forms
  (the combined `--workload all` run vs the standalone per-impl run)); see
  footnote below the micro table.

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
[^min]: The phase-1 `new` rows showed a `min=0.01 Mmsg/s` outlier (one repeat
~1000x slower than the median) across several invocations. Phase-1 `new` shares byte-identical internals with
`baseline`, so the stall is not code-path-specific, and the outlier never
recurred in phases 2-4 (see [^pwr2..4]). Medians are robust to it.
[^pwr2]: Phase-2 rows were measured on battery power at 100% (`pmset -g
batt`: "Now drawing from 'Battery Power'", discharging), vs. 87% for phase
1's measurements — a different point on the same (non-AC, non-quiet-machine)
power state, noted per the Machine section's caveat above. No `min=0.01`
outliers were observed in the phase-2 runs (unlike phase 1's
`new`/steady min, see [^min]).
[^pwr3]: Phase-3 rows were measured on battery power, 95% draining to 94%
across the three runs (`pmset -g batt`: "Now drawing from 'Battery Power'",
discharging) — the same non-AC, non-quiet-machine caveat as phases 1-2, and
close to phase 2's starting point rather than phase 1's. No `min=0.01`
outliers were observed in the phase-3 runs.
[^pwr4]: Phase-4 rows were measured on battery power, 91% draining to 90%
across the three runs (`pmset -g batt`: "Now drawing from 'Battery Power'",
discharging) — the same non-AC, non-quiet-machine caveat as phases 1-3. No
`min=0.01` outliers were observed in the phase-4 runs; all
`min` values tracked their `median` closely (worst case `insert`/new,
22.78 vs. 27.30 Mmsg/s).

## Macrobenchmark: full-day `itch_replay`

Command: `./build-release/itch_replay <day.NASDAQ_ITCH50> --quiet`

Day file: `12302019.NASDAQ_ITCH50` — NASDAQ's public sample day (Dec 30,
2019) from `emi.nasdaq.com/ITCH/Nasdaq ITCH/`, 7.69 GiB uncompressed
(gzip-CRC verified; the server's `.md5sum` link for this file 404s),
268,744,780 messages, 263,241,937 events applied, 8,892 books created. Never
checked in, per the Rules. Both rows were measured **retroactively on
2026-08-28, after the Milestone 3 merge**, on this same file and machine:
phase 1 is commit `1d9b7b0` (baseline internals) rebuilt in Release in a
worktree; phase 4 is merged `main` (`e2603c3`). Median of 5, min recorded.
Battery power throughout, 93% draining to 88%.

| phase | file | msgs/s median (min) | MiB/s | peak RSS MiB | live orders peak |
|---|---|---|---|---|---|
| 1 (baseline internals) | 12302019 | 3,696,533 (139,887 [^macmin]) | 108.2 | 272 | 1,924,078 |
| 4 (merged main) | 12302019 | 3,185,598 (2,920,105) | 93.3 | 4,994 [^rssmac] | 1,924,078 |

[^macmin]: Two of the five phase-1 repeats were slow outliers (174.7 s and
1921.2 s elapsed vs. the ~72 s cluster of the other three), so the recorded
phase-1 min is an outlier rather than a representative run. Medians are
robust to it.
[^rssmac]: Median of the five repeats (range 4,736–5,102). Phase-1 RSS was
272 MiB in every repeat.

**Default invariant sweeps dominate phase 4's wall time.** The command of
record inherits `--sweep-every 25000000` → 11 sweeps over the day, and a
sweep is far more expensive on the phase-4 internals: `check_invariants`
scans each book's band arrays and bitmaps, and there are 8,892 books —
~4.2 s per sweep, vs. ~1.0 s per sweep for baseline's node-based structures.
With `--sweep-every 0` (final sweep only; one clean run each): phase 4 replays the day in **38.0 s (7,078,660 msgs/s, 207.3
MiB/s)** vs. phase 1's **61.7 s (4,357,999 msgs/s, 127.6 MiB/s)** —
**1.62x**. So the with-sweep table above under-reports the engine: sweeps
are an out-of-band
debugging aid (never on the hot path), and with them enabled phase 4
measures 0.86x vs. baseline end-to-end; without them, 1.62x. Both numbers
are real consequences of the command of record; the no-sweep pair is the
fair engine comparison.

**Peak RSS regression (finding):** 272 MiB (baseline) → ~5 GiB (phase 4),
~18x. Each Book carries fixed-capacity structures — band slot arrays
(4,096 ticks initial, ×2 growth to 262,144, never shrinking), a 1,024-entry
minimum open-addressing index, pooled order/level slabs — sized for the
single hot book of `book_bench`, and the real day multiplies that per-book
floor by 8,892 books (~575 KiB average per book). Correct, but a real cost
the micro table never shows; per-book sizing (smaller initial band/index,
slab sharing across books) is a candidate for a future milestone.

**Book-vs-feed split (spec §2):** feed-dominant. Phase-4 no-sweep
end-to-end is 141 ns/message; `book_bench steady` (phase 4, new) measures
the book at 38.7 ns/event — even charging every applied event at the micro
rate, the book accounts for roughly a quarter of end-to-end wall time, and
the feed side (framing, decode, routing, stats) for the rest. Per the spec,
that is a finding for a future milestone, not license to widen this one.
(Caveat: the micro rate comes from the synthetic single-book `steady`
workload, so the split is an estimate, not a profile.)

## Band constants (phase 3 tuning)

`kInitialBandTicks = 4096`, `kMaxBandTicks = 262144` — **still provisional**.
These constants were set without real-day data and are left unchanged in
`src/book.cpp`. A day file is now available locally (the macro section's
`12302019.NASDAQ_ITCH50`), but the tuning itself remains open: compute the
per-symbol quoted-range histogram (share of a symbol's range covered by a
262,144-tick window around its first quote) and revisit both constants
together with the re-anchor question below. The macro RSS finding above adds
a second input to that tuning: the initial band size is also a per-book
memory floor paid 8,892 times.

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
