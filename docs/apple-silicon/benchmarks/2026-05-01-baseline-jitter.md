# Baseline Jitter Measurement (Post-Fast-Read, Pre-60-FPS-Pursuit)

Date: 2026-05-01

## Purpose

The 30 FPS gameplay floor is now met for all three tracked titles with the
three completed opt-in flags
(`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`). The
60 FPS pursuit needs a measured baseline of *both* sustained FPS and frame
jitter so future slices can be evaluated for regressions in either
dimension.

This note records that baseline by re-analyzing the existing 300 s retail
gameplay route runs from the fast-read landing. No new runs were captured
because the binary at the same commit produced those logs.

## Tooling Added In This Slice

`scripts/apple-silicon/extract-perf-summary.sh` now emits jitter metrics
derived from the existing per-interval `mspf_min`/`mspf_avg`/`mspf_max`
fields:

- `fps_stddev` — per-interval FPS stddev across the run (and post-load).
- `mspf_max_p50/p95/p99/max` — percentile of per-interval worst-frame MSPF.
- `mspf_avg_max` — worst per-interval mean MSPF.
- `stutter_intervals_30fps` — count of intervals where `mspf_max > 33.3`.
- `stutter_intervals_45fps` — count where `mspf_max > 22.2`.
- `stutter_intervals_60fps` — count where `mspf_max > 16.7`.
- `longest_stutter_run_30fps` / `longest_stutter_run_60fps` — longest
  contiguous run of intervals over those thresholds.

Both whole-run and post-load (`post_load_*`) variants are emitted. A
post-load skip of 5 intervals is used by default to elide load-fade frames.

## Measurement Granularity Note

The original `xemu-perf:` log emitted `mspf_min`/`mspf_max` as integer
milliseconds. A subsequent code change in this same slice promoted internal
mspf tracking to microseconds and emits `mspf_avg`/`mspf_min`/`mspf_max`
with sub-ms precision (`%.3f`) so the 60 FPS budget (16.67 ms) is
resolvable. The baseline runs analyzed below were captured before that
change so their `mspf_max` values are still integer-ms; future runs will be
sub-ms accurate.

The optional `XEMU_PERF_FRAME_LOG=1` toggle was added at the same time. It
appends a per-frame `frame_mspf_us=v1,v2,...` field to interval lines,
bounded at 1024 frames per interval (overflow recorded in
`frame_mspf_us_dropped`). Default off; enable when true frame-level
percentiles are needed.

## Baseline Metrics — 300 s Retail Routes (All Three Flags On)

| Title | post_load fps | fps stddev | mspf p50 | p95 | p99 | max | stutter ints (30 FPS) | longest stutter run | bottleneck class |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| PGR2 (`benchmark-runs/20260501-123525-pgr2`) | 31.76 | 5.69 | 29 | 36 | 38 | 117 | 36 | 3 s | CPU/lock-bound |
| Rainbow Six 3 (`benchmark-runs/20260501-124102-rainbow-six-3`) | 30.55 | 5.25 | 7 | 63 | **139** | **694** | 31 | 4 s | CPU/lock-bound |
| Crimson Skies (`benchmark-runs/20260501-124625-crimson-skies`) | 30.54 | 4.37 | 33 | 78 | **892** | **1310** | **127** | **16 s** | renderer-bound |

(All units ms. mspf percentiles are over per-interval `mspf_max`. "Stutter
ints (30 FPS)" is intervals where the worst frame in that second exceeded
33.3 ms; "longest stutter run" is the longest contiguous run of such
intervals.)

### Bottleneck Class Derivation

The `avg_mspf` field in `xemu-perf:` is *render time per frame* (time the
renderer actually spent), not total frame budget (= 1000 / fps). Comparing
the two reveals where wall-time is spent:

| Title | avg_mspf (renderer) | 1000/fps (total budget) | renderer % | non-renderer % |
| --- | --- | --- | --- | --- |
| PGR2 | 12.34 | 31.49 | 39 % | 61 % |
| Rainbow | 6.89 | 32.74 | 21 % | 79 % |
| Crimson | 29.68 | 32.74 | **91 %** | 9 % |

PGR2 and Rainbow have most of the per-frame wall time outside the
renderer — that is consistent with the
`pgr2-bottleneck-sample` finding that the TCG i386 thread is the
critical-path consumer when the renderer is fast. Crimson, by contrast,
spends 91 % of every frame budget inside the renderer and is currently
GPU/draw-bound rather than CPU/lock-bound. This means the next lock-elision
slice will *not* lift Crimson; Crimson needs a renderer-side win.

### Crimson Stutter Source

Per-interval drilldown of the Crimson run's worst stutters:

| Interval | fps | mspf_max | TEX_UPLOAD | SURF_DOWNLOAD | SURF_TO_TEX | SHADER_GEN | SHADER_BIND | tri_draws |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 9  | 6.51  | 1249 | 3   | 1   | 9   | 2   | 19   | 20    |
| 10 | 4.57  | 1184 | 1   | 3   | 9   | 1   | 27   | 27    |
| 11 | 30.79 | 160  | 46  | 0   | 31  | 4   | 141  | 161   |
| 15 | 2.23  | **1310** | 3 | 2 | 3 | 0 | 7 | 7 |
| 16 | 9.29  | 401  | 0   | 1   | 12  | 0   | 36   | 36    |
| 17 | 15.86 | 505  | 1   | 0   | 16  | 1   | 36   | 36    |
| 20 | 5.92  | 892  | 69  | 0   | 6   | 20  | 256  | 935   |
| 21 | 23.08 | 316  | 29  | 6   | 25  | 7   | 2502 | 9368  |

Every stutter interval coincides with non-zero `SHADER_GEN`,
`SURF_TO_TEX`, or `TEX_UPLOAD`. Interval 15 is the most diagnostic single
data point: 7 triangle draws over 1.3 seconds (≈ 187 ms per draw average)
with `SURF_DOWNLOAD=2` and `SURF_TO_TEX=3`. That is not a draw-throughput
problem; it is one of the surface or shader pipeline operations stalling
the pfifo thread.

This is consistent with Apple's OpenGL-on-Metal driver compiling shaders
synchronously inside `glDrawElements` (well-documented behavior in macOS GL
emulators). The `xemu-perf:` `SHADER_GEN` counter reports xemu's *own*
GLSL-source generation, but the host-side compile of GLSL → Metal happens
opaquely inside Apple's GL driver during the next draw that uses the
program. Both events plausibly contribute.

## Implications For The 60 FPS Pursuit

1. **PGR2 and Rainbow (CPU/lock-bound)**: the next lock-elision slices
   (`pgraph_write` fast path, `voice_lock` fast path,
   release-lock-during-GL) should help both. Expected lift is bounded
   by what those locks contribute — known to be 6.9 % `voice_lock` and
   2.7 % `pgraph_write` from the pre-fast-read sample profile, plus
   whatever `pg->lock` contributes during GL submission. A fresh sample
   profile is the next required data point.
2. **Crimson (renderer-bound)**: lock-elision slices will do *little*
   for Crimson because the renderer is already saturating frame budget.
   The work for Crimson is renderer-side: shader-compile stutter
   reduction, surface upload optimization, or eventually the native
   Metal renderer track.
3. **Tail jitter**: All three titles have meaningful p99 frame times.
   The user-visible "no jitter" bar means we should drive
   `mspf_max_p99` to ≤ 22 ms (45 FPS-budget margin) and ideally
   ≤ 16.67 ms for sustained 60 FPS. The current p99 is 38 / 139 / 892
   ms for PGR2 / Rainbow / Crimson. Crimson's tail is the worst by
   orders of magnitude and is unlikely to collapse without addressing
   the synchronous shader-compile path.

## Followups

1. Capture a fresh `sample` profile of the `pgr2_gameplay_b4` snapshot
   with all three flags on (post-fast-read). Identify the new dominant
   cost. Document under
   `docs/apple-silicon/benchmarks/<date>-pgr2-bottleneck-postfast.md`.
2. Re-run the 300 s retail routes once the perf-log microsecond change is
   built so jitter percentiles have sub-ms precision.
3. Flip `XEMU_PERF_FRAME_LOG=1` for at least one route per title to
   capture true per-frame mspf distributions. Extend
   `extract-perf-summary.sh` to compute frame-level p99 / p99.9 from the
   `frame_mspf_us=` field when present.
4. Decide ordering of next slices in light of the bottleneck classes:
   `XEMU_PGRAPH_FAST_WRITE` first (smallest, applies to PGR2/Rainbow),
   then `XEMU_VOICE_FAST_LOCK`, then
   `XEMU_PGRAPH_RELEASE_LOCK_DURING_GL`, then start the
   shader-compile-stutter slice for Crimson.
