# Benchmark Automation

Last updated: 2026-05-01 (game-packaging-tool session)

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
scripts/apple-silicon/run-benchmark.sh pgr2
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

## Recording Input

Physical controller input can be recorded into the same CSV format used by the
scripted-input replay path:

```sh
scripts/apple-silicon/record-input.sh pgr2 300
```

The recording wrapper launches the selected game with port 1 open for a real
controller, records controller changes to `recorded-input.csv` in the run
directory, and still captures normal benchmark metadata/logs. Pass an explicit
output path when a stable route file should be overwritten directly:

```sh
scripts/apple-silicon/record-input.sh pgr2 300 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv
```

The recorder is controlled by:

- `XEMU_RECORD_INPUT`: output CSV path.
- `XEMU_RECORD_INPUT_PORT`: one-based controller port, default `1`.
- `XEMU_RECORD_INPUT_AXIS_DELTA`: minimum analog-axis change to record,
  default `1024`.
- `XEMU_RECORD_INPUT_AXIS_DEADZONE`: small neutral-axis values treated as zero,
  default `256`.

Replay a captured route with the normal launcher:

```sh
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

## Captured Retail Gameplay Routes

The current profile-prepared retail gameplay routes are:

| Game | Route File | Benchmark Note | Baseline Capture |
| --- | --- | --- | --- |
| PGR2 | `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv` | `docs/apple-silicon/benchmarks/2026-05-01-pgr2-gameplay-route.md` | `benchmark-runs/20260501-094823-pgr2` |
| Rainbow Six 3 | `scripts/apple-silicon/input-scripts/rainbow-gameplay.csv` | `docs/apple-silicon/benchmarks/2026-05-01-rainbow-gameplay-route.md` | `benchmark-runs/20260501-095400-rainbow-six-3` |
| Crimson Skies | `scripts/apple-silicon/input-scripts/crimson-gameplay.csv` | `docs/apple-silicon/benchmarks/2026-05-01-crimson-gameplay-route.md` | `benchmark-runs/20260501-095905-crimson-skies` |

Replay them from the prepared profile HDD:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/rainbow-gameplay.csv 300
```

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/crimson-gameplay.csv 300
```

For opt-in triangle-family fill regression checks, add
`XEMU_NATIVE_TRI_DEPTH=1`. For opt-in quad/quad-strip-family fill bypass on
top of that, also add `XEMU_NATIVE_QUAD=1`. For lock-free PGRAPH register
reads, also add `XEMU_PGRAPH_FAST_READ=1`. All three flags are independent
and stack:

- `XEMU_NATIVE_TRI_DEPTH=1` removes the triangle-family geometry shader.
  Flat-first triangles stay native, flat-nonfirst triangle strips and fans
  fall back to the geometry shader.
- `XEMU_NATIVE_QUAD=1` removes the quad/quad-strip geometry shader for
  smooth-shaded fill draws by expanding quads to native triangles on the CPU
  and reusing the same `gl_FragCoord`-derived depth/slope path the triangle
  bypass uses. Flat-shaded quads, line/point polygon modes, and any nonfill
  raster mode fall back to the geometry shader.
- `XEMU_PGRAPH_FAST_READ=1` skips `pg->lock` for simple PGRAPH register
  reads (atomic 32-bit loads of `NV_PGRAPH_INTR`, `NV_PGRAPH_INTR_EN`, and
  the default `pg->regs_[]` slot). Removes the bulk of TCG-thread
  mutex-wait time when the renderer is holding the lock during a draw.
  `NV_PGRAPH_RDI_DATA` still locks because the read has a side effect.
  Biggest gain shows up when combined with the geometry-shader bypasses,
  because the renderer's lock-hold time is what creates contention in the
  first place.

## Profile Setup Runs

Before recording real benchmark routes, use live setup runs to create or select
game profiles without recording those menu actions:

```sh
scripts/apple-silicon/live-setup.sh pgr2 600
scripts/apple-silicon/live-setup.sh crimson 600
scripts/apple-silicon/live-setup.sh rainbow 600
```

These runs use a persistent copied HDD image at:

```text
benchmark-runs/profile-prep/xbox_hdd.qcow2
```

Use that prepared HDD as the source for later recorded or replayed benchmark
runs:

```sh
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/record-input.sh pgr2 300 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv
```

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
sub-millisecond `mspf_avg` / `mspf_min` / `mspf_max` (microsecond
precision internally, emitted with `%.3f`), and nonzero NV2A counters.
The OpenGL path also reports geometry-shader attribution counters:

At graceful process exit, xemu emits one final partial interval with
`final=1`. The benchmark launcher waits briefly for QMP `quit` before falling
back to termination so this final counter flush can run.

Optional per-frame timing log: `XEMU_PERF_FRAME_LOG=1` appends a
`frame_mspf_us=v1,v2,...` field to each interval line, recording the
microsecond mspf of every frame in that interval (bounded to 1024 frames
per interval; overflow recorded in `frame_mspf_us_dropped`). Default
off; enable when frame-level p99 / p99.9 percentiles are needed.

`scripts/apple-silicon/extract-perf-summary.sh` derives jitter metrics
from `mspf_max` per interval:
- `fps_stddev`, `mspf_max_p50/p95/p99/max`, `mspf_avg_max`.
- `stutter_intervals_30fps/45fps/60fps` — count of intervals with at
  least one frame > 33.3 / 22.2 / 16.7 ms.
- `longest_stutter_run_30fps/60fps` — longest contiguous run.
Each metric is also emitted with the `post_load_` prefix over the
post-load window (default skip first 5 intervals).

When the run was captured with `XEMU_PERF_FRAME_LOG=1`, the summary
also emits true frame-level metrics derived from the per-frame
`frame_mspf_us=...` field, aggregated across all timed intervals
(`final=1` partial-exit interval excluded, matching the timing
averages):
- `frame_mspf_us_p50`, `frame_mspf_us_p95`, `frame_mspf_us_p99`,
  `frame_mspf_us_p999`, `frame_mspf_us_max` — per-frame mspf
  percentiles in microseconds (nearest-rank).
- `frame_mspf_us_count` — total frame samples aggregated.
- `frame_mspf_us_dropped_total` — sum of `frame_mspf_us_dropped`
  across intervals (frames over the 1024/interval cap).
- `stutter_frames_30fps/45fps/60fps` — count of individual frames
  with mspf > 33.3 / 22.2 / 16.7 ms (33300 / 22200 / 16700 us).
Each frame-level metric is also emitted with the `post_load_` prefix.
Logs without `frame_mspf_us=` produce none of these keys.

`scripts/apple-silicon/sample-profile.sh GAME INPUT_CSV BENCH_SECONDS
[SAMPLE_DURATION] [WARMUP] [LABEL]` is the autonomous helper for
attaching Apple `sample` to a live xemu process. Runs the benchmark in
the background, polls for the run dir / xemu pid, sleeps WARMUP, then
calls `sample` for SAMPLE_DURATION seconds. Output written to
`<run-dir>/sample-<LABEL>.txt` with a thread-bucket summary alongside.

`scripts/apple-silicon/compare-runs.sh BASELINE CANDIDATE [SKIP]` prints
a side-by-side jitter+FPS comparison with verdicts (regression /
improvement / noise). `NOISE_PCT` env var overrides the default 3 %
threshold. Exit code reflects whether the candidate regresses.

- `GEOM_SHADER_MODULE_GEN`: new geometry shader modules compiled.
- `GEOM_SHADER_PROGRAM_GEN`: new linked programs that attach a geometry shader.
- `GEOM_SHADER_BIND` / `GEOM_SHADER_BIND_NOTDIRTY`: geometry-backed shader
  program binds or reused binds.
- `GEOM_SHADER_DRAW`: draws using a geometry-backed shader program.
- `GEOM_SHADER_DRAW_LINE`, `GEOM_SHADER_DRAW_TRI`, `GEOM_SHADER_DRAW_QUAD`,
  `GEOM_SHADER_DRAW_OTHER`: geometry-backed draws grouped by primitive family.
- `GEOM_SHADER_DRAW_QUAD_LIST`, `GEOM_SHADER_DRAW_QUAD_STRIP`: quad-family GS
  draws split into PRIM_TYPE_QUADS (4-vertex independent) and
  PRIM_TYPE_QUAD_STRIP (2-vertex incremental) so the native quad bypass
  coverage can be reasoned about by subtype.
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
- `NATIVE_QUAD_DRAW` / `NATIVE_QUAD_FALLBACK`: native quad-family bypass
  attempts that drew natively or fell back to the geometry shader.
- `NATIVE_QUAD_DRAW_LIST` / `NATIVE_QUAD_DRAW_STRIP`: native quad draws split
  by primitive subtype (PRIM_TYPE_QUADS vs PRIM_TYPE_QUAD_STRIP).
- `NATIVE_QUAD_CANDIDATE`, `NATIVE_QUAD_CANDIDATE_SMOOTH`,
  `NATIVE_QUAD_CANDIDATE_FLAT`: eligibility classification before the
  draw/fallback split. Smooth-shaded fill quads currently take the native
  path; everything else falls back.
- `NATIVE_QUAD_FALLBACK_FLAT` / `NATIVE_QUAD_FALLBACK_NONFILL`: reason for
  staying on the geometry shader when XEMU_NATIVE_QUAD=1 was set.
- `NATIVE_QUAD_DRAW_ZPERSPECTIVE` / `NATIVE_QUAD_DRAW_LINEAR_Z`: native quad
  draws split by depth mode.
- `NATIVE_QUAD_DRAW_POLY_OFFSET`: native quad draws with fill polygon offset
  enabled.
- `SHADER_COMPILE_COUNT`: number of GL program compile/link events per
  interval (cold-compile path `generate_shaders()` plus disk-cache
  `pgraph_gl_shader_load_from_memory()`). Cache hits do not count.
- `SHADER_COMPILE_US_TOTAL`: total microseconds spent in the GL program
  compile/link path per interval. On Apple's GL-on-Metal driver this is
  the synchronous GLSL→MSL translation cost the renderer thread blocks on
  inside `glLinkProgram` / `glProgramBinary`. Divide by
  `SHADER_COMPILE_COUNT` for per-event cost. This is the direct
  attribution counter for shader-compile-driven jitter — pair with
  `frame_mspf_us_max` and the longest-stutter-run keys to confirm the
  Crimson/Rainbow worst-frame source. With
  `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`, the timing accumulates on the
  worker thread (clock time), not the renderer thread, so this counter
  no longer maps 1:1 to renderer-thread block time.
- `SHADER_COMPILE_ASYNC_QUEUED`: per interval, number of shader compile
  requests the renderer dispatched to the async worker. Only nonzero
  with `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`.
- `SHADER_COMPILE_ASYNC_COMPLETED`: per interval, number of compile
  requests the worker finished and published. Cumulative
  `QUEUED - COMPLETED` is the live queue depth.
- `SHADER_DRAWS_SKIPPED_PENDING`: per interval, number of `draw_begin /
  draw_end` calls that were skipped because the requested shader
  binding was still being compiled by the async worker. Each skipped
  draw is one frame of pop-in for that geometry. The count should
  drop sharply once the working set of shader variants is warm.

Async shader compile (opt-in flag `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`):

- Spawns a single `pgraph.gl_async_compile` worker thread bound to a
  shared `g_nv2a_context_shader_compile` GL context. The worker pulls
  bindings off `PGRAPHGLState::compile_queue` and runs
  `generate_shaders()` (compile + link + glFinish) without blocking the
  renderer.
- The renderer's `pgraph_gl_bind_shaders` enqueues a compile request
  the first time it sees a new shader state hash, sets
  `r->shader_skip_draw=true`, and the corresponding draw is skipped via
  early-returns in `pgraph_gl_draw_begin` / `pgraph_gl_draw_end`.
  Subsequent bind calls on the same hash also skip until the worker
  publishes `binding->initialized=true`.
- LRU eviction of a binding with `pending_compile=true` would race the
  worker; `shader_cache_entry_post_evict` aborts in that case. The
  shader cache is 50K entries and worst-case queue depth is small, so
  this is documented as a guardrail, not an expected path.
- Trade-off: visible pop-in for the first few frames after a new shader
  appears, in exchange for never blocking the renderer on synchronous
  GLSL→MSL compile. Matches the RPCS3 `Async (Skip Draws)` pattern.
  Aimed at the Crimson Skies / Rainbow Six 3 worst-frame stutter
  documented in the post-fast-read jitter analysis.

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

## Game Library Packaging

`scripts/apple-silicon/package-game.sh` packages an extracted Original Xbox
game directory into a XISO ISO that xemu can load. It wraps
[xdvdfs](https://github.com/antangelo/xdvdfs) (`xdvdfs pack`) with
project-aware defaults so Claude can pull a game from the external library
on demand for stress-testing reported xemu issues against this build.

External library default location:

```text
/Volumes/Josh-Backup-Files/Console Games/Original Xbox
```

The library is organized into three letter-range subdirectories
(`XBOX HDD ready (#-I)`, `XBOX HDD ready (J-Q)`, `XBOX HDD ready (R-Z)`)
plus a `DLC/` directory. Each game is an extracted Xbox game tree
(`default.xbe` plus subdirectories), exactly what `xdvdfs pack` consumes.

Usage:

```sh
# List games (optionally filtered)
scripts/apple-silicon/package-game.sh --list
scripts/apple-silicon/package-game.sh --list "rainbow"

# Pack by name (case-insensitive substring match against folder names)
scripts/apple-silicon/package-game.sh "Grooverider - Slot Car Thunder"

# Pack with explicit source / output overrides
scripts/apple-silicon/package-game.sh \
  --source "/path/to/extracted/game" \
  --output Test_Games/some-game.xiso.iso
```

Default output location is `$XEMU_TEST_GAMES_DIR/<game>.xiso.iso` (which
defaults to `/Users/jbbrack03/XEMU_MacOS/Test_Games`). The launcher
`run-benchmark.sh` only knows about its four hardcoded targets
(`crimson`, `rainbow`, `pgr2`, `flat-tri-depth`); for an arbitrary
packaged game, pass the disc path through `XEMU_BENCH_EXTRA_QEMU_ARGS`
or invoke xemu directly with `-dvd_path` until the launcher learns a
`custom` target.

Behavior:

- Refuses to overwrite an existing output ISO unless `--force` is passed.
  This honors workspace rule #9 (do not modify `Test_Games/` in place).
- Writes a `<output>.meta.txt` sidecar with source path, byte size, pack
  duration, xdvdfs version, and a UTC timestamp.
- Verifies the produced ISO with `xdvdfs info` (skip via `--no-verify`).
- Auto-installs xdvdfs via `cargo install xdvdfs-cli --root
  $XEMU_XDVDFS_INSTALL_ROOT` (default `$HOME/.cargo`) when the binary is
  missing. Pass `--no-install` to disable.

Environment variables:

- `XEMU_GAME_LIBRARY` — external library root override.
- `XEMU_TEST_GAMES_DIR` — default output directory override.
- `XEMU_XDVDFS_BIN` — xdvdfs binary path override.
- `XEMU_XDVDFS_INSTALL_ROOT` — `cargo install --root` target for the
  autoinstall path.

Exit codes: `0` success (or already-packed no-op), `1` runtime/IO error,
`2` usage error or ambiguous name match.

The tool reads the external library only; it never modifies it.

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

1. Use the recorded retail gameplay routes as the main replay targets for the
   next session, starting with PGR2 baseline versus `XEMU_NATIVE_TRI_DEPTH=1`.
2. Use `scripts/apple-silicon/native-tri-depth-compare.sh` for paired
   baseline/native checks only when future changes could affect the completed
   triangle-family fill path.
3. Use `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` only for targeted flat-path
   debugging; the flat XBE counter split is already validated.
4. Keep the smoke routes as boot/profile/menu setup before refreshing snapshots.
5. For the next geometry-shader exit slice, use PGR2 to confirm quad-family
   pressure first. Keep Rainbow Six 3 for line-family coverage and Crimson
   Skies for sustained flight/acceleration cross-checks.
