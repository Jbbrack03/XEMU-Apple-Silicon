# 2026-05-01 — Per-frame mspf retail-route capture (PGR2 / Rainbow / Crimson)

## Purpose

Re-run the existing 300 s retail gameplay routes once each with
`XEMU_PERF_FRAME_LOG=1` enabled so every per-interval `xemu-perf:` line
carries a `frame_mspf_us=v1,v2,...` field with microsecond-precision
per-frame timings. Per `handoff.md` Prioritized Next Tasks #1, this is
the data needed to compute true frame-level p99 / p99.9 percentiles in
a future `extract-perf-summary.sh` extension. No code changes were
required for this capture; sub-millisecond perf precision and the
opt-in per-frame log already landed in commit `904659733a`.

This note records the captured run dirs and the per-interval jitter
metrics the existing summary script can already derive, so future
work can compare frame-level p99 against the per-interval values
captured here.

## Build and run conditions

- Binary commit: `4d40d40f46` (rebuilt this session via `./build.sh -a
  arm64`, which is required because the previously installed
  `dist/xemu.app` was at `e63a41433a` — predates the
  `XEMU_PERF_FRAME_LOG=1` feature; runs against that older binary would
  silently produce no `frame_mspf_us` field).
- Renderer: Apple GL-on-Metal (`GL_VENDOR: Apple`, `GL_RENDERER: Apple
  M3 Ultra`, `GL_VERSION: 4.1 Metal - 90.5`). No Vulkan.
- Flags applied per-run: `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1
  XEMU_PGRAPH_FAST_READ=1 XEMU_PERF_LOG=1 XEMU_PERF_FRAME_LOG=1`.
  These three opt-in performance flags are the current visually-validated
  set per decision-log "2026-05-01: Three opt-in flags visually
  validated for current title set".
- Harness: `scripts/apple-silicon/run-benchmark.sh <game>
  <input-csv> 300` with `XEMU_BENCH_SCREENSHOT_BACKEND=none` and the
  shared profile-prepared HDD source
  `benchmark-runs/profile-prep/xbox_hdd.qcow2`.
- Input scripts: `scripts/apple-silicon/input-scripts/{pgr2,rainbow,crimson}-gameplay.csv`
  — the same retail gameplay routes recorded on 2026-05-01.
- Three runs were sequential per project rule #9 (do not start xemu
  while another xemu is running). Total wall clock: ~12 minutes (boot +
  300 s + teardown for each of three titles).

## Run dirs

- PGR2: `benchmark-runs/20260501-173435-pgr2`
- Rainbow Six 3: `benchmark-runs/20260501-173959-rainbow-six-3`
- Crimson Skies: `benchmark-runs/20260501-174514-crimson-skies`

Each `xemu.log` carries one `frame_mspf_us=` field per interval line
(286 PGR2 / 281 Rainbow / 284 Crimson intervals with frame data; the
first ~5 intervals per run lack the field because no frames had ticked
through the renderer yet at perf-log start).

## Per-interval summary (existing extract-perf-summary.sh)

These are derived from the per-interval `mspf_max` field, not yet from
the new `frame_mspf_us=` per-frame field. They are recorded here for
later side-by-side with the per-frame extension.

| Title | post_load_avg_fps | post_load_avg_mspf | fps_stddev | mspf_max_p50 | mspf_max_p95 | mspf_max_p99 | mspf_max_max | stutter_30fps | longest_stutter_30fps |
|-------|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| PGR2  | 32.07 | 16.41 | 6.118 | 29.91 | 35.83 | 37.87 | 117.84 | 53 | 3 |
| Rainbow Six 3 | 30.16 | 6.48 | 5.569 | 6.79 | 29.19 | 89.86 | 717.18 | 9 | 2 |
| Crimson Skies | 30.43 | 29.16 | 4.165 | 33.43 | 74.55 | 458.09 | 1375.50 | 144 | 16 |

Units: FPS for `*_fps`; ms for all `mspf_*`; counts for `stutter_*`;
intervals (≈ seconds) for `longest_stutter_*`. `*_max` is the worst
single per-interval `mspf_max` observed in the post-load window; this
is the per-interval high-water mark, not the worst single frame.

## Observations

- **PGR2 still hits the 30 FPS gameplay floor** at 32.07 post-load FPS,
  consistent with the prior `2026-05-01-pgraph-fast-read.md` 31.76 FPS
  result. Variance across same-flag PGR2 routes remains real-time-paced
  and scene-dependent (see `2026-05-01-pgr2-native-quad.md` for the
  prior 36 % spread between same-config runs); the 32.07 figure is
  within the band already documented and is not a regression.
- **Rainbow Six 3 also clears 30 FPS** (30.16 post-load) but exhibits a
  much worse tail than PGR2: `mspf_max_max=717 ms` with `p99=89.86 ms`.
  This matches the Apple GL-on-Metal synchronous-shader-compile
  fingerprint already documented in `2026-05-01-baseline-jitter.md`
  (Rainbow p99 139 ms / max 694 ms there; the difference is
  scene-dependent re-compile timing, not a new bug).
- **Crimson Skies shows the same worst-frame profile as previously
  documented**: `mspf_max_max=1375 ms`, `p99=458 ms`,
  `longest_stutter_run_30fps=16 intervals`. This is the same Apple
  GL-on-Metal synchronous-shader-compile pattern documented in
  `2026-05-01-baseline-jitter.md` (1310 ms worst-frame, 16 s longest
  stutter run). Crimson is the highest-leverage target for the
  async-shader-compile slice (handoff Prioritized Next Tasks #2).
- **Per-route 30 FPS stutter intervals**: 53 PGR2 / 9 Rainbow / 144
  Crimson. Crimson dominates the stutter-interval count by 2.7× over
  PGR2, consistent with its outlier `mspf_max` distribution.

These per-interval stats line up with the existing
`2026-05-01-baseline-jitter.md` entries (which were captured before
sub-millisecond perf precision landed). Re-checking with
microsecond-precision per-interval `mspf_max` values does not change
the qualitative picture: Crimson is the runaway worst; Rainbow has a
bad tail; PGR2 has a heavier mid-band but a tame tail.

## What is still TODO

`scripts/apple-silicon/extract-perf-summary.sh` does not yet parse
`frame_mspf_us=`. The percentile arithmetic (lines 80-132) operates on
the per-interval `mspf_max` field. Frame-level p99 / p99.9 require:

1. Add a parser that splits each `frame_mspf_us=v1,v2,...` field into a
   global flat list across all intervals (subject to the 1024-frame
   per-interval cap recorded in `frame_mspf_us_dropped`, currently 0
   for all three routes here).
2. Sort and emit `frame_mspf_us_p50`, `_p95`, `_p99`, `_p999`, `_max`,
   plus `_stutter_frames_30/45/60fps` and `_longest_frame_run_*`. Both
   whole-run and `post_load_*` variants.
3. Optionally emit a frame-time histogram for jitter shape inspection.

This is the follow-up the handoff calls out and is a pure
post-processing extension — no emulator code changes. The data captured
in this session is the input.

## Cross-references

- `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`
  (B0–D17 series, integer-ms perf precision).
- `docs/apple-silicon/benchmarks/2026-05-01-baseline-jitter.md`
  (per-interval jitter metrics introduced; sample profiles framed).
- `docs/apple-silicon/benchmarks/2026-05-01-pgraph-fast-read.md`
  (PGR2 31.76 FPS post-load with all three flags on).
- `docs/apple-silicon/benchmarks/2026-05-01-tcg-float-audit.md`
  (this-session research finding: SSE hardfloat already active on
  aarch64; x87 80-bit irreducibly soft).
- `handoff.md` Prioritized Next Tasks (#1 captured by this note; #2
  async shader compile remains the highest user-visible lift).

## Honesty notes

- These three runs were on the same machine (Apple M3 Ultra, macOS
  26.4.1), same external PGR2 disc volume, same scratch HDD source, in
  a single session. They do not characterize machine-to-machine
  variance.
- Whole-route averages remain partly real-time-paced; per-run scene
  divergence is reduced compared to the pre-fast-read regime but not
  eliminated. For stable comparisons of code changes, prefer the
  `pgr2_gameplay_b4` snapshot triplet documented in
  `2026-05-01-pgr2-native-quad.md` and `2026-05-01-pgraph-fast-read.md`.
- Rainbow's worst-frame here (717 ms) is materially better than the
  prior 694 ms baseline-jitter capture and the absolute worst-case
  Crimson tail (1375 ms vs 1310 ms) is materially worse, but both are
  inside the documented bounds for Apple-GL synchronous shader compile
  and should not be read as regressions or improvements without paired
  same-build comparisons (which is the role of `compare-runs.sh`).
