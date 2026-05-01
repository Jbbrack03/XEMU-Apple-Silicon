# Benchmark Automation

Last updated: 2026-05-01

This fork has a small scripted-input harness for repeatable Apple Silicon
benchmark runs. It is opt-in and does not affect normal xemu launches.

## Scripted Input

Set `XEMU_SCRIPTED_INPUT` to a CSV file before launching xemu:

```sh
XEMU_SCRIPTED_INPUT=scripts/apple-silicon/input-scripts/crimson-skies-smoke.csv \
XEMU_SCRIPTED_INPUT_PORT=1 \
dist/xemu.app/Contents/MacOS/xemu ...
```

The CSV format is:

```csv
time_ms,control,value
8000,start,1
8200,start,0
12000,a,1
12200,a,0
```

Button values are `0` or `1`. Axis values are signed 16-bit controller values
from `-32768` to `32767`.

Supported buttons:

- `a`, `b`, `x`, `y`
- `start`, `back`
- `white`, `black`
- `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`
- `lstick_btn`, `rstick_btn`, `guide`

Supported axes:

- `ltrigger`, `rtrigger`
- `lstick_x`, `lstick_y`
- `rstick_x`, `rstick_y`

## Benchmark Launcher

Run one of the local benchmark targets:

```sh
scripts/apple-silicon/run-benchmark.sh crimson
scripts/apple-silicon/run-benchmark.sh rainbow
scripts/apple-silicon/run-benchmark.sh flat-tri-depth
```

Optional arguments:

```sh
scripts/apple-silicon/run-benchmark.sh crimson path/to/input.csv 240
```

Each run creates a directory under `benchmark-runs/` containing:

- `metadata.txt`: machine, commit, game, and input script details.
- `xemu.toml`: per-run config pointing at the scratch HDD and selected disc.
- `xemu.log`: xemu stdout/stderr, including scripted-input load status.
- `capture.log`: QMP screenshot capture status.
- `snapshot.log`: QMP/HMP `savevm` or `loadvm` status when requested.
- `screenshots/`: periodic PNG captures.
- `xbox_hdd.qcow2`: APFS-cloned scratch HDD copy when available.
- `qmp.sock`: QMP socket while the run is active.

The launcher intentionally uses a scratch HDD copy so benchmark navigation does
not mutate the source HDD image.

Set `XEMU_BENCH_EXTRA_QEMU_ARGS` when a benchmark run needs an extra xemu/QEMU
argument such as a trace selector:

```sh
XEMU_BENCH_EXTRA_QEMU_ARGS='-trace nv2a_pgraph_method' \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

This is intended for simple diagnostic arguments. The value is appended as
shell words by the launcher.

## Flat Triangle-Depth XBE

A dedicated nxdk test lives at
`scripts/apple-silicon/xbe-tests/flat-tri-depth/`. It builds:

- `bin/default.xbe`
- `flat-tri-depth.iso`

Build it with:

```sh
NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk \
PATH=/Users/jbbrack03/XEMU_MacOS/nxdk/bin:/opt/homebrew/Cellar/lld@19/19.1.7/bin:/opt/homebrew/opt/llvm/bin:$PATH \
make -C scripts/apple-silicon/xbe-tests/flat-tri-depth
```

The ISO is also copied to
`/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso` for manual
launches. The app alternates every 240 frames between:

- first-provoking flat-shaded triangles, expected to hit
  `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`.
- last-provoking flat-shaded triangles, expected to hit
  `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`.

Current validation status: the XBE boots, the rebuilt ISO timestamp is recorded
in benchmark metadata, and the native triangle-depth path now splits the two
flat phases as intended. Run
`benchmark-runs/20260430-153555-flat-tri-depth` produced 480
`NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST` draws for first-provoking flat triangles and
304 `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` fallbacks with 304
`GEOM_SHADER_DRAW_TRI` draws for last-provoking flat triangles.

The pass/fail wrapper is:

```sh
scripts/apple-silicon/validate-native-tri-depth.sh --run 20
```

It launches the flat XBE with `XEMU_NATIVE_TRI_DEPTH=1`, disables the older
diagnostic triangle flags, extracts the perf summary, and checks that
flat-first triangles draw natively while flat-nonfirst triangles still match
the geometry-shader fallback count. A fresh packaged-app run,
`benchmark-runs/20260430-210159-flat-tri-depth`, passed with 422 flat-first
native draws, 240 flat-nonfirst fallbacks, and 240 triangle-family geometry
shader draws.

Run it through the benchmark harness with:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 20
```

Run the trace-enabled variant only when debugging the flat native/fallback
split:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_PERF_LOG_INTERVAL_MS=1000 \
XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1 \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

Pass criteria for the flat XBE:

- first-provoking flat phases produce nonzero
  `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`.
- last-provoking flat phases produce nonzero
  `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` and corresponding
  `GEOM_SHADER_DRAW_TRI` activity.
- the run includes a final `xemu-perf:` interval with `final=1`, so short test
  tails are included in the counter summary.

The launcher refuses to start when another `dist/xemu.app/Contents/MacOS/xemu`
process is already running. This avoids mixing windows, QMP sockets, and save
state from overlapping benchmark runs. Close xemu first, or set
`XEMU_BENCH_ALLOW_EXISTING=1` only for manual experiments.

Screenshot cadence can be adjusted with environment variables:

```sh
XEMU_BENCH_SCREENSHOT_INTERVAL=5 \
XEMU_BENCH_SCREENSHOT_START_DELAY=2 \
scripts/apple-silicon/run-benchmark.sh crimson
```

Screenshot backend defaults to macOS `screencapture`, which avoids touching the
guest OpenGL framebuffer:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=macos scripts/apple-silicon/run-benchmark.sh crimson
XEMU_BENCH_SCREENSHOT_BACKEND=none scripts/apple-silicon/run-benchmark.sh crimson
```

`XEMU_BENCH_SCREENSHOT_BACKEND=qmp` is available for experiments, but it has
crashed Apple's OpenGL-on-Metal path during early testing and should not be the
baseline capture method yet.

Frame pacing and NV2A profile summaries are enabled by the launcher through
`XEMU_PERF_LOG=1`. The summary interval defaults to one second and can be
changed with:

```sh
XEMU_PERF_LOG_INTERVAL_MS=500 scripts/apple-silicon/run-benchmark.sh crimson
```

The resulting `xemu.log` contains `xemu-perf:` lines with interval FPS,
average/min/max frame time, and nonzero NV2A counters. The OpenGL path also
reports geometry-shader attribution counters:

At graceful process exit, xemu emits one final partial interval with
`final=1`. The benchmark launcher waits briefly for QMP `quit` before falling
back to termination so this final counter flush can run.

- `GEOM_SHADER_MODULE_GEN`: new geometry shader modules compiled.
- `GEOM_SHADER_PROGRAM_GEN`: new linked programs that attach a geometry shader.
- `GEOM_SHADER_BIND` / `GEOM_SHADER_BIND_NOTDIRTY`: geometry-backed shader
  program binds or reused binds.
- `GEOM_SHADER_DRAW`: draws using a geometry-backed shader program.
- `GEOM_SHADER_DRAW_LINE`, `GEOM_SHADER_DRAW_TRI`, `GEOM_SHADER_DRAW_QUAD`,
  `GEOM_SHADER_DRAW_OTHER`: geometry-backed draws grouped by primitive family.
- `NATIVE_TRI_DEPTH_DRAW` / `NATIVE_TRI_DEPTH_FALLBACK`: native triangle-depth
  replacement attempts that drew natively or fell back to geometry shaders.
- `NATIVE_TRI_DEPTH_CANDIDATE`, `NATIVE_TRI_DEPTH_CANDIDATE_SMOOTH`,
  `NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST`, and
  `NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST`: eligibility classification before
  the draw/fallback split. These are the most direct counters for flat XBE
  validation and future native-triangle correctness checks.
- `NATIVE_TRI_DEPTH_DRAW_ZPERSPECTIVE` / `NATIVE_TRI_DEPTH_DRAW_LINEAR_Z`:
  native draws split by depth mode.
- `NATIVE_TRI_DEPTH_DRAW_POLY_OFFSET`: native draws with fill polygon offset
  enabled.
- `NATIVE_TRI_DEPTH_DRAW_SMOOTH` / `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`: native
  draws split by shading/provoking-vertex coverage. Flat-shaded draws are
  allowed on the native path only when NV2A is using first-provoking-vertex
  mode, which matches the OpenGL path's `GL_FIRST_VERTEX_CONVENTION`.
- `NATIVE_TRI_DEPTH_FALLBACK_FLAT` / `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`:
  native-path attempts that stayed on geometry shaders because flat shading is
  still outside the validated native path.

Summarize a completed run with:

```sh
scripts/apple-silicon/extract-perf-summary.sh benchmark-runs/20260430-114511-rainbow-six-3
```

The summary reports overall averages, post-load averages with the first five
intervals skipped, geometry-shader counters, native triangle-depth counters,
and common draw-path counters.

When a final `final=1` interval exists, its counters are included in totals but
its partial-exit FPS/frame-time values are excluded from timing averages.

Compare two captured screenshots over a fixed xemu viewport crop with:

```sh
scripts/apple-silicon/compare-screenshots.py \
  benchmark-runs/20260430-115436-rainbow-six-3/screenshots/001-010000ms.png \
  benchmark-runs/20260430-115335-rainbow-six-3/screenshots/001-010001ms.png \
  --crop 641,209,1278,957 \
  --out-dir benchmark-runs/20260430-115436-rainbow-six-3/visual-compare-native-10s
```

The comparison helper saves cropped baseline/candidate images, an amplified
diff image, and prints mean absolute error, RMS error, maximum channel error,
and changed-pixel percentage. Use this only as a visual smoke check; it does
not prove depth correctness.

Run a paired baseline/native snapshot comparison with:

```sh
scripts/apple-silicon/native-tri-depth-compare.sh \
  rainbow benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
  rainbow_scene_b1_nothumb 16
```

The paired runner launches a baseline run with native triangle depth disabled,
then a candidate run with `XEMU_NATIVE_TRI_DEPTH=1`. It writes launcher logs,
perf summaries, metadata, and a cropped screenshot comparison under a
`benchmark-runs/*-native-tri-depth-compare-*` report directory. Override
`XEMU_NATIVE_TRI_DEPTH_COMPARE_SCREENSHOT_INDEX`, screenshot cadence,
`XEMU_NATIVE_TRI_DEPTH_COMPARE_RETRIES`, or pass `none` as the final crop
argument for perf-only comparisons. Retries default to two attempts per side to
absorb the nondeterministic Apple OpenGL texture-upload startup crash seen in
Crimson Skies.

## Diagnostic Toggles

`XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` keeps triangle-family geometry shaders in
use, but bypasses their depth-plane and slope calculation. This is intentionally
not a correctness path. It is a temporary diagnostic for separating "geometry
shader exists" overhead from "geometry shader does expensive depth math"
overhead. The flag is included in the geometry shader state key, so diagnostic
shader programs do not reuse normal shader-cache entries.

`XEMU_DIAG_SKIP_TRI_GEOM=1` bypasses geometry-shader program generation for
triangle-family fill draws and lets OpenGL draw native triangles directly. This
is intentionally not a correctness path: the fragment shader then receives the
per-vertex depth payload through normal/flat interpolation rather than the
geometry shader's per-triangle payload. It is a temporary diagnostic for
measuring the cost of geometry-shader dispatch and Apple OpenGL driver behavior.
Line primitives remain on the normal geometry-shader path.

`XEMU_NATIVE_TRI_DEPTH=1` also bypasses triangle-family fill geometry
shaders, but changes the fragment shader to derive depth and polygon-slope
offset from native GL rasterization state. This is the completed current opt-in
triangle-family fill replacement path. Like the skip diagnostic, it leaves line
primitives on the normal geometry-shader path. Flat-shaded triangle fills
remain conservative:
first-provoking draws can use the native path, while nonfirst-provoking draws
still fall back to the geometry shader. Set `XEMU_NATIVE_TRI_DEPTH=0` to disable
the path explicitly; that preferred spelling wins even if the compatibility
alias is also set.

`XEMU_DIAG_NATIVE_TRI_DEPTH=1` remains accepted as a compatibility alias for
older benchmark notes, but new runs should prefer `XEMU_NATIVE_TRI_DEPTH=1`.

`XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` enables capped native-triangle state
logging for flat-path debugging. It records shade/provoking method state plus
the live PGRAPH fields and bound shader state at shader bind, draw begin, and
draw flush. Leave it off for timing runs.

## Snapshot Runs

The launcher can create and restore named VM snapshots through the QMP socket
using QEMU's HMP `savevm` and `loadvm` commands.

Create a snapshot after the scripted route reaches the target scene:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_SAVEVM_AT=120 \
XEMU_BENCH_SAVEVM_TAG=crimson_scene_b0 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/crimson-skies-smoke.csv 140
```

Restore that snapshot from the snapshot-bearing scratch HDD:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=crimson_scene_b0 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Snapshot restore is intentionally done after xemu startup through QMP/HMP.
Initial CLI `-loadvm` testing failed with a saved device-tree mismatch around
the emulated USB hub.

Benchmark runs set `XEMU_SNAPSHOT_NO_THUMBNAIL=1` by default. This skips
snapshot thumbnail capture, because one Rainbow Six 3 snapshot launch crashed
Apple's OpenGL-on-Metal worker path while a thumbnail-bearing snapshot was
present on the scratch HDD.

## Current Limitations

- The Crimson Skies route reaches pilot registration, accepts a generated name,
  and advances into the rendered in-engine sequence.
- The Rainbow Six 3 route reaches the default `CHAVEZ` profile, selects
  Campaign, and advances to the Hereford mission loading screen.
- The smoke routes remain useful for setup, and named VM snapshots now provide
  shorter repeatable scene-entry runs.
- The harness drives xemu's controller abstraction, not QEMU's generic
  `send-key` path.
- FPS/frame-pacing extraction is available from `xemu-perf:` log lines.

## Next Automation Steps

1. Use `scripts/apple-silicon/native-tri-depth-compare.sh` for paired
   baseline/native checks only when future changes could affect the completed
   triangle-family fill path.
2. Use `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` only for targeted flat-path
   debugging; the flat XBE counter split is already validated.
3. Keep the smoke routes as boot/profile/menu setup before refreshing snapshots.
4. For the next geometry-shader exit slice, extend counters and paired
   comparison commands before changing line primitives, quad/quad-strip
   expansion, polygon fill, or nonfill triangle modes.
