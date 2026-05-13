# Benchmarking Plan

Last updated: 2026-05-12 evening (added T1/T2 boot-animation temporal
baseline rows; M15 evidence bundle remains incomplete pending tracked-
title reruns under T2). Use
`scripts/apple-silicon/m15-bundle-status.py` before new Metal/default-on
claims; 2026-05-11 evening result is
`verdict=incomplete ok=6 fail=5 missing=4` (after the same-day
m15-gameplay-* discovery extension and the front-fb fallback policy
decision-log entry). The oracle-side and stable retail trio evidence is
green, but title-level paired gameplay Metal-vs-GL validation is not: the
latest PGR2/Rainbow paired passes are capture/static canaries only, Crimson
paired diff still fails (`changed_pct=14.7560`), and the 2026-05-11 evening
PGR2 capture-source diagnostic decisively ruled out the capture path,
confirming the bug is in Metal's multi-RT compositing path (see
`docs/apple-silicon/benchmarks/2026-05-11-pgr2-metal-render-path-diagnostic.md`).
SC2/Halo/Rainbow gameplay diffs are missing, PGR2/Rainbow/Crimson p99
jitter gates fail, and cold shader compile proof is missing. The front-fb
fallback policy is now resolved (stays opt-in pending the multi-RT
compositing fix). `metal-gl-compare.sh --trigger
flip` now uses GL `XEMU_GL_SCREENSHOT_PATH` plus Metal
`XEMU_METAL_SCREENSHOT_SOURCE=nv2a` for cleaner paired PNGs, but production
visual evidence still requires controller-driven gameplay sequences, multiple
content-aligned keyframes, and visible GL/Metal/oracle triptychs. GL renderer
with `XEMU_GL_MSAA=4` +
`surface_scale=2` remains the production configuration meeting the user's
stated 30/60 FPS at 1080p goals. Input-latency counters
`INPUT_USB_POLLS` / `INPUT_BACKEND_UPDATES` / `INPUT_LAT_US_TOTAL` /
`INPUT_LAT_US_MAX` remain always-on; `XEMU_MACOS_NATIVE_INPUT=1` is the
opt-in GameController.framework backend.

For the next M15 benchmark session, use the explicit PGR2 recipe in
`handoff.md`, but first inspect the failed PGR2 diagnostic in
`benchmark-runs/m15-gameplay-pgr2-windowgl-20260511-182317/diagnostic-relaxed-align/`.
The immediate benchmark question is whether the Metal NV2A screenshot source is
wrong or the live Metal renderer is genuinely missing PGR2 profile/menu
background content. Do not log a gameplay visual PASS from a single
flip-trigger/static frame or from a relaxed-align diagnostic.

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

### 2026-05-02 multi-slice session entries

> The "V"-prefixed labels in this session refer to **validation
> slices** for default-on flag candidates, not the older
> Vulkan-over-Metal V0/V1/M0 cells above. Both naming conventions
> are kept; the V1/V2/V3/V4/V5 entries below are unrelated to V0/V1.

| ID | Slice / target | Renderer | Game / scene | Duration | Verdict | Note |
| --- | --- | --- | --- | --- | --- | --- |
| V1 | `XEMU_TCG_SPLITWX` validation | OpenGL | Crimson 300s + PGR2 snapshot regression | 300s + 30s | PARTIAL (mechanical PASS; headline FAIL) | `benchmarks/2026-05-01-tcg-splitwx-validation.md` |
| V2 | `XEMU_TCG_JMP_CACHE_TARGETED` validation | OpenGL | Crimson 300s + PGR2 snapshot regression | 300s + 30s | PARTIAL (per-call wallclock collapsed; headline FAIL) | `benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md` |
| V3 | TCG / renderer per-event spike attribution | OpenGL | Crimson 300s @ 10ms threshold | 300s | PARTIAL ATTRIBUTION (~10% of worst frame attributed) | `benchmarks/2026-05-02-tcg-spike-attribution.md` |
| V3-composite | Composite goal stack (4 flags + scale=2 + MSAA=4) | OpenGL | PGR2 / Rainbow / Crimson | 300s each | 30 FPS cap NOT renderer-bound; reframe needed | `benchmarks/2026-05-02-composite-goal-validation.md` |
| V4-SC2 | 60Hz title sanity test | OpenGL | Soul Calibur 2, attract demo | 240s | PASS — 60.57 FPS sustained 109 intervals | `benchmarks/2026-05-02-60hz-title-sanity-test.md` |
| V4-sweep | Broader-title library sweep | OpenGL | Burnout 3, Halo CE, Splinter Cell, NGB, OutRun 2 | 240s each | PASS — library-wide viable | `benchmarks/2026-05-02-broader-title-sweep.md` |
| D2 | TCG 30 FPS cap + worst-frame attribution | OpenGL | Crimson 300s, 1ms spike threshold | 300s | Attributed 422ms / 1386ms; 963ms unattributed | `benchmarks/2026-05-02-tcg-30fps-cap-attribution.md` |
| I5 | `XEMU_APU_LOCK_RELEASE` validation | OpenGL | Crimson 300s + PGR2 / Rainbow snapshot | 300s + 30s + 30s | PARTIAL (steady-state PASS; headline FAIL) | `benchmarks/2026-05-02-apu-lock-release-validation.md` |

### 2026-05-04 Metal visual canary entries

| ID | Slice / target | Renderer | Game / scene | Duration | Verdict | Note |
| --- | --- | --- | --- | --- | --- | --- |
| M5.11-PGR2 | Surface/RTT + A8 linear texture-view fix | Metal | PGR2 menu/logo/text canary | frame 900 screenshot | PASS | `benchmarks/2026-05-04-metal-pgr2-surface-rtt-validation.md` |
| M5.11-R6 | Surface/RTT regression canary | Metal | Rainbow Six 3 loading screen | frame 600 screenshot | PASS | `benchmarks/2026-05-04-metal-pgr2-surface-rtt-validation.md` |
| M5.12-BOOT | Front-face + cubemap-border canary | Metal | Xbox boot/flubber animation | frame 300 screenshot | PASS | `benchmark-runs/20260504-092824-crimson-skies`, `benchmark-runs/visual-checks/boot-post-oob-f300.png` |
| M5.12-CS-STAB | Texture-DMA OOB + invalid-stage shader hardening | Metal | Crimson Skies gameplay route | frame 1800 screenshot + 70s run | STABILITY PASS / VISUAL PENDING | `benchmark-runs/20260504-092403-crimson-skies`; screenshot lands on black transition/loading output |
| M5.13-PGR2-TEX | Texture dirty semantics + shader-dot fix + Metal CPU wall counters | Metal | PGR2 gameplay route | 150s | ATTRIBUTED: shader fallbacks 0, texture bind CPU bottleneck | `benchmarks/2026-05-04-metal-pgr2-texture-bind-attribution.md`, `benchmark-runs/20260504-132806-pgr2` |

### 2026-05-05 Metal visual canary + harness entries

| ID | Slice / target | Renderer | Game / scene | Duration | Verdict | Note |
| --- | --- | --- | --- | --- | --- | --- |
| M5.14-CS-VISUAL | Crimson Metal "blocker" reclassified as config | Metal | Crimson Skies main menu (canonical recipe) | 90s | PASS — sustained ~30 FPS; tarot-card menu rendered correctly | `benchmarks/2026-05-05-crimson-config-not-renderer-bug.md`, `benchmark-runs/20260505-104139-crimson-skies` |
| W3-RERUN | Counter-mode regression gate post-atexit-skip fix | Metal | All four canaries (PGR2 / Rainbow / Halo / boot) | ~6 min total | 4/4 PASS | `benchmark-runs/20260505-110002-canary-regress` |
| F3-CRIMSON | Per-title snapshot anchor proof-of-concept | GL+Metal | Crimson menu via `crimson-canary` snapshot | 30s | snapshot save+load PASS same-renderer; cross-renderer Metal-saved → GL-loaded SIGSEGV | `benchmark-runs/profile-prep/crimson-canary.qcow2`, `benchmark-runs/20260505-114034-metal-gl-compare-crimson` |

### 2026-05-12 boot-animation temporal baseline + T2 fix

| ID | Slice / target | Renderer | Game / scene | Duration | Verdict | Note |
| --- | --- | --- | --- | --- | --- | --- |
| T1-METAL-NV2A | Boot animation PNG-every-frame (SOURCE=nv2a, FRONT_FB_FALLBACK=0) | Metal | Xbox BIOS animation + flat-tri-depth.xbe | 18s | **FAIL** — 1036/1066 frames solid magenta | `benchmark-runs/20260512T200701Z-boot-metal-temporal/`; user-reported "green blobs" baseline |
| T1-METAL-DRAW | Boot animation (SOURCE=drawable, FRONT_FB_FALLBACK=0) | Metal | same | 18s | **FAIL** — 709/1070 frames solid magenta | `benchmark-runs/20260512T200800Z-boot-metal-drawable/` |
| T1-METAL-FRONTFB | Boot animation (SOURCE=drawable, FRONT_FB_FALLBACK=1; the M15 eval recipe) | Metal | same | 18s | **FAIL** — green-blob noise; blink rate 1.06/sec (17× GL) | `benchmark-runs/20260512T201000Z-boot-metal-frontfb/` |
| T1-GL-REF | Boot animation reference | GL | same | 18s | PASS — orderly BIOS animation + flat-tri-depth red/cyan triangle | `benchmark-runs/20260512T200844Z-boot-gl-temporal/`, ffmpeg AVFoundation capture |
| T1-PAIRED | Temporal-flicker analyzer paired output | Metal vs GL | aggregated | n/a | analysis artifact | `benchmark-runs/20260512T201100Z-boot-temporal-analysis/`, drove the methodology-change decision-log entry 2026-05-12 (evening) |
| T2-METAL-FIX | Post-fix boot animation (per-host-refresh publish + pg->lock) | Metal | same | 18s | PARTIAL PASS — 525 content frames (vs 0 pre-fix); flat-tri-depth renders correctly; BIOS animation still solid magenta pending VGA fallback (M5.13/M18) | `benchmark-runs/20260513T030000Z-boot-metal-T2v4-locked/`, see decision-log 2026-05-12 (evening 2), commits `ca35b96562` + `3ae76a327c`, `benchmarks/2026-05-12-metal-boot-animation-temporal-baseline.md` |


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

Current recorded retail gameplay routes:

| ID | Game | Route File | Capture Run | Avg FPS | Post-load Avg FPS | Geometry-shader Draws | Primitive Coverage |
| --- | --- | --- | --- | --- | --- | --- | --- |
| R1 | Project Gotham Racing 2 | `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv` | `benchmark-runs/20260501-094823-pgr2` | 11.53 | 11.67 | 1,516,519 | 1,477,734 triangle, 38,785 quad |
| R2 | Rainbow Six 3 | `scripts/apple-silicon/input-scripts/rainbow-gameplay.csv` | `benchmark-runs/20260501-095400-rainbow-six-3` | 24.19 | 24.76 | 692,438 | 690,492 triangle, 1,946 line |
| R3 | Crimson Skies | `scripts/apple-silicon/input-scripts/crimson-gameplay.csv` | `benchmark-runs/20260501-095905-crimson-skies` | 15.44 | 15.80 | 786,722 | 778,885 triangle, 7,837 quad |

Use PGR2 as the first retail route for the next geometry-shader-removal slice:
it is furthest below the 30 FPS target and exposes quad-family geometry-shader
activity during real gameplay. Use Rainbow Six 3 to keep line-family coverage
visible, and Crimson Skies as the sustained flight/acceleration cross-check.

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
    as the primary snapshot-level triangle-family comparison. Use PGR2 first
    for current retail gameplay geometry-dispatch work.
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
- 2026-05-01 PGR2 native-tri-depth route replay
  (`docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-tri-depth.md`):
  baseline 11.53 → 21.40 post-load FPS with `XEMU_NATIVE_TRI_DEPTH=1`. The
  remaining geometry-shader work was 100% quad-family (177,272 of 177,272
  GS draws), motivating the quad bypass.
- 2026-05-01 PGR2 native-quad implementation and snapshot triplet
  (`docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md`):
  - `XEMU_NATIVE_QUAD=1` is the second completed geometry-shader bypass
    slice; smooth-fill quads/quad-strips dispatch via CPU-expanded triangle
    indices and reuse the `gl_FragCoord`-derived depth path.
  - Triangle regression gate
    (`scripts/apple-silicon/validate-native-tri-depth.sh --run 22`) passed at
    `benchmark-runs/20260501-105543-flat-tri-depth`.
  - Rainbow Six 3 snapshot scene with both flags reports 30.97 post-load
    FPS / 6.71 MSPF — identical within noise to D8.
  - PGR2 mid-route snapshot triplet (`pgr2_gameplay_b4` from
    `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`):
    - Baseline 4.39 FPS, 332,066 GS draws.
    - `XEMU_NATIVE_TRI_DEPTH=1` 16.02 FPS, 11,745 GS quad draws remain.
    - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` 16.56 FPS, zero GS draws,
      12,193 native-quad draws all `LIST` and `CANDIDATE_SMOOTH` with zero
      fallbacks.
  - Whole-route PGR2 averages are not stable across runs (36% variance
    between two same-config runs), so the snapshot triplet is the trusted
    comparison. Use the stable `XEMU_NATIVE_TRI_DEPTH=1` and
    `XEMU_NATIVE_QUAD=1` spellings together for the full geometry-shader
    bypass.
- 2026-05-01 per-frame mspf retail-route capture
  (`docs/apple-silicon/benchmarks/2026-05-01-frame-log-retail-routes.md`):
  all three R1/R2/R3 routes were re-run under
  `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1
  XEMU_PERF_LOG=1 XEMU_PERF_FRAME_LOG=1` for 300 s each so every
  per-interval `xemu-perf:` line carries `frame_mspf_us=v1,v2,...` for
  future frame-level p99/p99.9 work. Run dirs:
  `benchmark-runs/20260501-173435-pgr2`,
  `benchmark-runs/20260501-173959-rainbow-six-3`,
  `benchmark-runs/20260501-174514-crimson-skies`. Per-interval
  post-load summaries: PGR2 32.07 FPS / 117.84 ms max-frame; Rainbow
  30.16 FPS / 717.18 ms max-frame; Crimson 30.43 FPS / 1375.50 ms
  max-frame and 16-interval longest 30 FPS stutter run. Crimson's
  worst-frame distribution matches the Apple GL-on-Metal synchronous-
  shader-compile fingerprint already documented in
  `docs/apple-silicon/benchmarks/2026-05-01-baseline-jitter.md`. The
  data is the input for a future `extract-perf-summary.sh` extension
  that parses `frame_mspf_us=` to compute true frame-level percentiles;
  no emulator code change is required for that follow-up.
- 2026-05-01 TCG SSE/x87 hardfloat audit
  (`docs/apple-silicon/benchmarks/2026-05-01-tcg-float-audit.md`): pure
  source audit, no benchmark runs. Conclusion: SSE float32/float64
  helpers already take the hardfloat shortcut on aarch64
  (`fpu/softfloat.c:337-397`); the visible `parts64_uncanon_normal`
  time in `2026-05-01-pgr2-bottleneck-postfast.md` is the necessary
  soft fallback for first-op-after-MXCSR-reset, NaN/Inf/denormal
  inputs, denormal results, and non-default rounding modes. x87 80-bit
  is irreducibly soft on Apple Silicon. Recommended cheap follow-up:
  add an `sse_hard_taken`/`sse_soft_fallback` counter pair around
  `float32_gen2`/`float64_gen2` and run on the `pgr2_gameplay_b4`
  snapshot for 30 s; if hard-take ratio > 0.9, redirect away from
  float-helper work.
- V0/V1/M0 should wait until the next non-geometry-shader bottleneck for
  PGR2 is identified. With both bypass slices on, the snapshot scene is
  16.56 FPS and there is no geometry-shader work left to remove. The next
  benchmark task is to profile that snapshot under Instruments and the
  existing `XEMU_PERF_LOG=1` counters to find the dominant remaining cost
  before any further renderer slice.

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
  draw counters split line, triangle, quad, and other primitive families,
  with quad further split into list/strip subtypes
  (`GEOM_SHADER_DRAW_QUAD_LIST` / `GEOM_SHADER_DRAW_QUAD_STRIP`).
- Number of native-bypass draws per frame
  (`NATIVE_TRI_DEPTH_DRAW`, `NATIVE_QUAD_DRAW` and per-subtype/depth
  splits) plus eligibility/fallback breakdowns
  (`NATIVE_TRI_DEPTH_FALLBACK_*`, `NATIVE_QUAD_FALLBACK_*`).
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
