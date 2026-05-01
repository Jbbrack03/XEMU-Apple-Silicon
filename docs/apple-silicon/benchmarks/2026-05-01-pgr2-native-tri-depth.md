# PGR2 Native Triangle-Depth Replay Comparison

Date: 2026-05-01

## Purpose

Replay the recorded PGR2 retail gameplay route with `XEMU_NATIVE_TRI_DEPTH=1`
to measure how much of the severe Apple OpenGL geometry-shader bottleneck is
removed when the completed triangle-family fill replacement is enabled, and to
classify the remaining geometry-shader pressure that gates the next slice.

The baseline reference is the 2026-05-01 PGR2 capture
(`benchmark-runs/20260501-094823-pgr2`, 11.53 FPS / 11.67 post-load).

## Route

- Game: Project Gotham Racing 2
- Disc: `Test_Games/PGR2.xiso.iso` (mounted from
  `/Volumes/Final Cut Pro Libraries/Projects/XEMU_MacOS/`)
- Prepared HDD source: `benchmark-runs/profile-prep/xbox_hdd.qcow2`
- Replay script: `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv`
- Run: `benchmark-runs/20260501-104158-pgr2`
- Duration: 300 seconds
- Build commit: `56a0e3c9832e401d83ddde1ec59aad2e69b77670`
  (apple-silicon-performance branch)

## Command

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

## Perf Summary

From `scripts/apple-silicon/extract-perf-summary.sh
benchmark-runs/20260501-104158-pgr2`:

- Intervals: 274
- Timed intervals: 273
- Final intervals: 1
- Average FPS: 21.71
- Average MSPF: 2.27
- Post-load skip: 5
- Post-load intervals: 268
- Post-load average FPS: 21.40
- Post-load average MSPF: 1.96

Geometry-shader counters:

- Module gen: 2
- Program gen: 63
- Bind: 142,785 (notdirty: 0)
- Draw: 177,272
  - Line: 0
  - Triangle: 0
  - Quad: 177,272
  - Other: 0

Native triangle-depth counters:

- Draw: 10,644,394
- Candidate: 10,644,394 (smooth: 10,644,394; flat-first: 0; flat-nonfirst: 0)
- Fallback: 0
- Z-perspective: 10,201,204
- Linear-Z: 443,190
- Polygon-offset: 1,070,516
- Smooth-shaded: 10,644,394
- Flat-first: 0

Dispatch breakdown:

- BEGIN/END pairs: 11,091,872
- Draw arrays: 240,235
- Inline elements: 10,560,700
- Inline arrays: 360
- Inline buffers: 283,701
- Shader gen: 335
- Shader binds: 1,484,318

## Comparison To Baseline

| Metric | Baseline (no flag) | XEMU_NATIVE_TRI_DEPTH=1 | Delta |
| --- | --- | --- | --- |
| Avg FPS | 11.53 | 21.71 | +10.18 (+88%) |
| Post-load avg FPS | 11.67 | 21.40 | +9.73 (+83%) |
| GS triangle draws | 1,477,734 | 0 | -100% |
| GS quad draws | 38,785 | 177,272 | +357% |
| GS line draws | 0 | 0 | unchanged |
| Native tri-depth draws | n/a | 10,644,394 | new |

The +357% quad-family count is not a regression: the baseline route ran at
11.5 FPS so it produced fewer post-load intervals of work in the same wall-
clock window. Per-second the native-flag run is rendering more total frames,
which scales every counter. The relevant comparison is *what fraction of the
remaining GPU work is quad-family versus everything else*, and in this run it
is the entirety of the geometry-shader work (177,272 of 177,272).

## Conclusion

- `XEMU_NATIVE_TRI_DEPTH=1` removes the entire triangle-family geometry-shader
  cost from PGR2 retail gameplay (1.48M GS triangle draws → 0).
- The native triangle-depth path covers 100% of the triangle-fill draws as
  smooth-shaded candidates with zero fallbacks. There is no flat-shaded
  triangle activity in this scene, so the flat-first eligibility rule is not
  exercised here.
- FPS roughly doubles (11.67 → 21.40 post-load), which is a large win, but
  PGR2 still falls 8.6 FPS short of the sustained-30-FPS gameplay floor.
- The remaining geometry-shader pressure is **entirely quad-family**: 177,272
  draws across PGR2's gameplay window. There are zero remaining
  triangle-family, line-family, or "other" geometry-shader draws.
- Next implementation slice: replace the quad/quad-strip geometry-shader path
  for the smooth-shaded fill case with native expansion to triangles, derive
  depth and polygon-slope offset in the fragment shader the same way the
  triangle path does, and add per-subtype counters
  (`GEOM_SHADER_DRAW_QUAD_LIST`, `GEOM_SHADER_DRAW_QUAD_STRIP`,
  `NATIVE_QUAD_DRAW_*`, `NATIVE_QUAD_FALLBACK_*`) to confirm coverage.

## Notes

- Visual smoke check was not part of this run because the native-tri-depth
  visual comparison is already on file
  (`docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md`).
  A new paired comparison should be produced once the quad bypass lands so
  baseline + native-tri-depth + native-tri-depth + native-quad have a
  same-build cross-check.
- The packaged-app validator regression gate
  (`scripts/apple-silicon/validate-native-tri-depth.sh --run 20`) passed in
  the prior session at `benchmark-runs/20260430-210159-flat-tri-depth` and
  is not re-run here because no triangle code changed.
