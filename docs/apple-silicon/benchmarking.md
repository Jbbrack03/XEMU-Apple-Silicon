# Benchmarking Plan

Last updated: 2026-05-01

## Benchmarking Rules

1. Every performance claim needs a before/after capture.
2. Record the exact commit, renderer, macOS version, hardware, settings, game,
   scene, and duration.
3. Separate FPS, frame pacing, shader compilation stalls, CPU time, and GPU time
   when possible.
4. Keep correctness screenshots next to performance measurements.
5. Do not compare across unrelated emulator settings.

## Local Machine

Record these for every run:

- `sw_vers`
- `uname -m`
- `sysctl -n machdep.cpu.brand_string` when available
- `system_profiler SPDisplaysDataType`
- xemu commit hash
- renderer and feature flags

Current observed environment:

- Architecture: `arm64`
- macOS: `26.4.1`
- Build: `25E253`
- CPU: `Apple M3 Ultra`
- GPU: `Apple M3 Ultra`
- GPU cores: `60`
- Metal support: `Metal 4`
- Display: `SONY TV *30`, 3360 x 1890, 60 Hz

Current baseline build:

- Build command: `./build.sh -a arm64`
- Result: succeeds
- App bundle: `dist/xemu.app`
- Executable: `dist/xemu.app/Contents/MacOS/xemu`
- Executable architecture: Mach-O 64-bit arm64
- Startup renderer: Apple OpenGL-on-Metal
  - GL vendor: `Apple`
  - GL renderer: `Apple M3 Ultra`
  - GL version: `4.1 Metal - 90.5`
- Baseline setup session file: `docs/apple-silicon/benchmarks/2026-04-29-baseline.md`
- Baseline metrics session file: `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`
- Native triangle-depth validation file:
  `docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md`

## Local Assets

Known local test games:

- Crimson Skies: `/Users/jbbrack03/XEMU_MacOS/Test_Games/Crimson skies.xiso.iso`
- Rainbow Six 3: `/Users/jbbrack03/XEMU_MacOS/Test_Games/Rainbow Six 3.xiso.iso`
- Project Gotham Racing 2:
  `/Volumes/Final Cut Pro Libraries/Projects/XEMU_MacOS/Test_Games/PGR2.xiso.iso`

Known emulator files:

- MCPX: `/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/mcpx/mcpx_1.0.bin`
- BIOS: `/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/bios/Complex_4627.bin`
- HDD: `/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/hdd/xbox_hdd.qcow2`

## Initial Matrix

| ID | Build | Renderer | Game | Scene | Duration | Metrics |
| --- | --- | --- | --- | --- | --- | --- |
| B0 | local arm64 baseline | OpenGL | Crimson Skies | boot + first playable scene | 150s | complete: FPS and pacing |
| B1 | local arm64 baseline | OpenGL | Rainbow Six 3 | boot + first playable scene | 190s | complete: FPS and pacing |
| B2 | geometry counter baseline | OpenGL | Crimson Skies | `crimson_scene_b0` snapshot | 30s | complete: FPS, pacing, geometry counters |
| B3 | geometry counter baseline | OpenGL | Rainbow Six 3 | `rainbow_scene_b1_nothumb` snapshot | 30s | complete: FPS, pacing, geometry counters |
| D1 | simplified triangle depth diagnostic | OpenGL | Rainbow Six 3 | `rainbow_scene_b1_nothumb` snapshot | 30s | complete: no improvement over B3 |
| D2 | skip triangle geometry diagnostic | OpenGL | Rainbow Six 3 | `rainbow_scene_b1_nothumb` snapshot | 30s | complete: large frame-time improvement, intentionally incorrect |
| D3 | native triangle-depth path | OpenGL | Rainbow Six 3 | `rainbow_scene_b1_nothumb` snapshot | 30s | complete: zero geometry draws, faster than B3; later validated by D17/P1/P2 |
| D4 | native triangle-depth path | OpenGL | Crimson Skies | `crimson_scene_b0` snapshot | 30s | complete: zero geometry draws, cross-check scene |
| D5 | native triangle-depth path, line-safe skip | OpenGL | Rainbow Six 3 | `rainbow_scene_b1_nothumb` snapshot | 30s | complete: historical Rainbow comparison point |
| D6/D7 | native triangle-depth path, line-safe skip | OpenGL | Crimson Skies | `crimson_scene_b0` snapshot | 30s | complete: cross-check repeated because D6 showed slower pacing |
| D8/D10 | native triangle-depth path, flat-shading-safe skip | OpenGL | Rainbow Six 3 | `rainbow_scene_b1_nothumb` snapshot | 30s | complete: repeated zero-fallback native path confirmation |
| D17 | flat triangle-depth XBE | OpenGL | flat-tri-depth nxdk test | generated XISO | 20-22s | complete: final perf flush captures the flat split; reusable validator passed in `20260430-210159` |
| P1/P2 | paired native triangle-depth comparisons | OpenGL | Rainbow Six 3 / Crimson Skies | saved snapshots | 16s | complete: same-build baseline/native proof for current opt-in triangle-fill category |
| V0 | Vulkan-over-Metal prototype | MoltenVK | Crimson Skies | same as B0 | 120s | feature support, FPS, correctness |
| V1 | Vulkan-over-Metal prototype | KosmicKrisp | Crimson Skies | same as B0 | 120s | feature support, FPS, correctness |
| M0 | Metal prototype | Metal | Crimson Skies | same as B0 | 120s | FPS, pacing, correctness |

## Retail Gameplay Targets

Performance target floor:

- Sustained 30 FPS in gameplay for all benchmark titles, matching original Xbox
  hardware expectations.
- 60 FPS is desirable when possible, but not the minimum stability/performance
  bar for this Apple Silicon work.

Current observed retail behavior before recorded gameplay routes:

| Game | Smooth Areas | Problem Areas |
| --- | --- | --- |
| Project Gotham Racing 2 | Opening cinematics and main menu are smooth. | Gameplay is very choppy in both video and audio. |
| Crimson Skies | Opening cinematics and main menu are smooth. | Gameplay does not sustain the proper framerate, feels laggy, specific animations such as holding `Y` for plane acceleration are choppy, and audio occasionally skips. |
| Rainbow Six 3 | Opening cinematics and main menu are smooth. Gameplay is smooth while stationary. | Framerate drops substantially once character movement starts, gameplay becomes choppy, and audio occasionally skips. |

Recorded gameplay routes should prioritize the problem areas above rather than
only reaching menus or cinematics. PGR2 is the strongest current stress case for
severe video/audio pacing collapse. Crimson Skies is useful for sustained
near-target gameplay with animation/audio glitches. Rainbow Six 3 is useful for
movement-triggered gameplay slowdown and visual artifact coverage.

Current matrix status:

- B0/B1 build, launch, scripted route, and log-based FPS/frame-pacing
  measurement setup are complete.
- B0/B1 baseline metrics were recorded on 2026-04-30:
  - B0 Crimson Skies: 139 intervals, average 29.93 FPS, tail-60 average 30.98
    FPS.
  - B1 Rainbow Six 3: 174 intervals, average 26.45 FPS, tail-60 average 30.98
    FPS.
- Snapshot scene-entry runs were verified:
  - `crimson_scene_b0`: QMP/HMP restore average 29.52 FPS over 28 intervals.
  - `rainbow_scene_b1_nothumb`: QMP/HMP restore average 29.22 FPS over 28
    intervals.
- B2/B3 geometry-counter scene-entry runs were recorded on 2026-04-30:
  - B2 Crimson Skies: 28 intervals, average 29.70 FPS, post-load average
    30.98 FPS, 25,202 geometry-backed draws.
  - B3 Rainbow Six 3: 27 intervals, average 29.23 FPS, post-load average
    30.97 FPS, 149,961 geometry-backed draws.
- D1 tested `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` on Rainbow Six 3:
  - 25 intervals, average 28.24 FPS, post-load average 30.72 FPS.
  - Result: no improvement over B3; next diagnostics should target
    geometry-shader dispatch or driver-level timing.
- D2 tested `XEMU_DIAG_SKIP_TRI_GEOM=1` on Rainbow Six 3:
  - 27 intervals, average 29.93 FPS, post-load average 30.96 FPS.
  - Post-load average frame time fell to 6.38 ms and geometry draw counters
    dropped to zero.
  - Result: strong diagnostic evidence that the GL geometry-shader dispatch
    path or Apple driver behavior is the bottleneck.
- D3 tested `XEMU_DIAG_NATIVE_TRI_DEPTH=1` on Rainbow Six 3:
  - 27 intervals, average 29.49 FPS, post-load average 30.96 FPS.
  - Post-load average frame time was 8.10 ms and geometry draw counters
    dropped to zero.
  - Result at the time: first promising prototype for a GL triangle path that
    avoids geometry-shader dispatch. Later D17/P1/P2 work validated the current
    opt-in triangle-family fill category.
- D4 cross-checked `XEMU_DIAG_NATIVE_TRI_DEPTH=1` on Crimson Skies:
  - 28 intervals, average 31.15 FPS, post-load average 30.98 FPS.
  - Post-load average frame time was 18.97 ms and geometry draw counters
    dropped to zero.
  - One previous Crimson D3 attempt crashed before QMP in Apple's OpenGL
    texture-upload worker path, but the immediate rerun completed.
- D5 reran `XEMU_DIAG_NATIVE_TRI_DEPTH=1` on Rainbow Six 3 after narrowing the
  diagnostic geometry skip so line primitives stay on their normal geometry
  path:
  - 28 intervals, average 30.89 FPS, post-load average 30.99 FPS.
  - Post-load average frame time was 6.35 ms and geometry draw counters
    remained zero.
  - Result: use D5 as a historical Rainbow comparison point. Use P1/P2 for
    current same-build baseline/native evidence.
- D6/D7 reran the same line-safe native triangle-depth path on Crimson
  Skies:
  - D6: 28 intervals, average 31.37 FPS, post-load 30.98 FPS / 27.53 MSPF.
  - D7: 28 intervals, average 31.26 FPS, post-load 30.98 FPS / 20.30 MSPF.
  - Both runs kept geometry draw counters at zero, including line counters.
  - Result: Crimson has more frame-time variance here; keep using Rainbow Six 3
    as the primary geometry-dispatch comparison.
  - Current runs should use the stable `XEMU_NATIVE_TRI_DEPTH=1` spelling; the
    older diagnostic spelling remains a compatibility alias for these records.
- D8/D10 reran Rainbow Six 3 after tightening the native triangle-depth
  eligibility rule for flat shading:
  - D8: 28 intervals, average 30.99 FPS, post-load 30.99 FPS / 6.47 MSPF,
    197,212 native triangle-depth draws, zero fallbacks.
  - D10: 28 intervals, average 30.93 FPS, post-load 30.98 FPS / 6.78 MSPF,
    192,776 native triangle-depth draws, zero fallbacks.
  - Result: the current snapshot keeps all measured triangle-family fill draws
    on the native path without falling back to geometry shaders.
- D17 added and validated a generated nxdk XBE for flat-shaded triangle
  coverage:
  - Source: `scripts/apple-silicon/xbe-tests/flat-tri-depth/`
  - ISO: `scripts/apple-silicon/xbe-tests/flat-tri-depth/flat-tri-depth.iso`
  - Manual copy: `/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso`
  - Initial runs: `benchmark-runs/20260430-141057-flat-tri-depth`,
    `benchmark-runs/20260430-141439-flat-tri-depth`
  - Initial trace: `benchmark-runs/20260430-141331-flat-tri-trace`
  - Rebuilt/debug runs: `benchmark-runs/20260430-143952-flat-tri-depth`,
    `benchmark-runs/20260430-144128-flat-tri-depth`,
    `benchmark-runs/20260430-144451-flat-tri-depth`,
    `benchmark-runs/20260430-152714-flat-tri-depth`,
    `benchmark-runs/20260430-152952-flat-tri-depth`
  - First passing run: `benchmark-runs/20260430-153555-flat-tri-depth`
  - Reusable validator:
    `scripts/apple-silicon/validate-native-tri-depth.sh --run 20`
  - Fresh packaged-app validation:
    `benchmark-runs/20260430-210159-flat-tri-depth`
  - Result: the XBE boots and sends `NV097_SET_SHADE_MODE` flat plus first/last
    `NV097_SET_PROVOKING_VERTEX`. Capped native-tri-depth tracing showed live
    PGRAPH state and bound shader state both become flat-first. The earlier
    all-smooth summaries missed the short flat tail; after adding a graceful
    final perf flush, the first passing run reports 480
    `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, 304
    `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`, and 304 `GEOM_SHADER_DRAW_TRI`.
    The fresh packaged-app validator run reports 422 flat-first native draws,
    240 flat-nonfirst fallbacks, and 240 triangle-family geometry-shader draws.
- P1/P2 paired same-build native triangle-depth comparisons are recorded in
  `docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md`:
  - P1 Rainbow Six 3: 23.10 baseline MSPF versus 6.83 native MSPF, with 79,775
    baseline geometry draws and 0 native-run geometry draws.
  - P2 Crimson Skies: 29.47 baseline MSPF versus 17.87 native MSPF, with 20,041
    baseline geometry draws and 0 native-run geometry draws.
  - Result: triangle-family fill replacement is complete for the current opt-in
    category; defaulting still needs broader retail coverage.
- B0/B1 scripted setup routes remain available:
  - B0 reaches the Crimson Skies rendered in-engine sequence.
  - B1 reaches the Rainbow Six 3 Hereford mission loading screen.
- V0/V1/M0 should wait until the next geometry-shader-removal slice is chosen.
  Triangle-family fill is validated for current opt-in testing. The next
  benchmark task is to measure or create coverage for line primitives,
  quad/quad-strip expansion, polygon fill, and nonfill triangle modes, then use
  the paired comparison harness once a concrete replacement path exists.

## Metrics To Add To xemu

Add logging or HUD counters for:

- Renderer backend name. Basic startup logging is now present when
  `XEMU_PERF_LOG=1`.
- GPU/driver/platform feature summary.
- Shader compile count and compile time by stage. Geometry shader module and
  geometry-backed program generation counts are now logged.
- Pipeline cache hits/misses.
- Number of draw calls per frame.
- Number of geometry-shader-path draws per frame until removed. Geometry-backed
  draw counters split line, triangle, quad, and other primitive families.
- Texture upload/download bytes per frame.
- Surface readback count and bytes.
- Explicit sync/wait count per frame.
- CPU frame time and GPU frame time if available.

## Tools

Initial tools:

- xemu HUD/statistics if available.
- `scripts/apple-silicon/run-benchmark.sh` for scripted-input benchmark runs.
- Computer Use screenshots for live route verification when running from Codex.
- macOS Activity Monitor / `powermetrics` for high-level CPU/GPU pressure.
- Xcode Instruments:
  - Time Profiler
  - Metal System Trace for Metal or Vulkan-over-Metal paths
- Metal frame capture for native Metal or MoltenVK/KosmicKrisp paths.
- Console logs for `MTLCompilerService` failures.

Potential later tools:

- RenderDoc for non-macOS comparisons.
- GFXReconstruct for Vulkan capture if compatible with selected driver path.

Automation notes:

- `XEMU_SCRIPTED_INPUT` enables a fork-local scripted controller input source.
- `XEMU_PERF_LOG=1` enables `xemu-perf:` interval summaries in `xemu.log`; the
  benchmark launcher enables this by default.
- Current starter routes live in `scripts/apple-silicon/input-scripts/`.
- Named VM snapshot save/restore is available through QMP/HMP:
  - `XEMU_BENCH_SAVEVM_AT`
  - `XEMU_BENCH_SAVEVM_TAG`
  - `XEMU_BENCH_LOADVM_TAG`
  - `XEMU_BENCH_HDD_SOURCE`
- Use `XEMU_BENCH_SCREENSHOT_BACKEND=none` when tuning routes from Codex; QMP
  framebuffer capture has crashed Apple OpenGL-on-Metal, and macOS
  `screencapture` may not have display access in this environment.
- `scripts/apple-silicon/extract-perf-summary.sh` summarizes a run directory or
  `xemu.log` into overall/post-load FPS and the key geometry/native counters.
- `scripts/apple-silicon/compare-screenshots.py` crops two captured screenshots
  and prints basic image-difference metrics for visual smoke checks.
- See `automation.md` before adding new benchmark routes.

## Output Format

Create one markdown file per benchmark session:

`docs/apple-silicon/benchmarks/YYYY-MM-DD-short-name.md`

Each file should include:

- Commit hash.
- Build command.
- Launch command or config.
- Machine details.
- Game and scene.
- Screenshots or capture names.
- Table of metrics.
- Notes on visible correctness issues.
- Conclusion.
