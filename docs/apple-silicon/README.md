# Apple Silicon Performance Fork

Last updated: 2026-05-02

This directory tracks the Apple Silicon performance fork. The fork goal is not
to preserve upstream compatibility at all costs. The goal is to make xemu run
well on Apple Silicon macOS machines through data-driven renderer and platform
work.

## Goal

Deliver excellent Apple Silicon performance without sacrificing emulator
correctness blindly. Performance changes must be measured, and rendering
changes must be validated against known game behavior, existing xemu behavior,
and available NV2A test evidence.

## Working Hypothesis

The current macOS performance problem is primarily graphics-backend related:

- macOS builds link Apple's deprecated OpenGL framework.
- The Vulkan renderer is not enabled for Darwin in the current Meson logic.
- The renderer increasingly depends on geometry shaders for ordinary Xbox
  primitive handling and depth behavior.
- Public xemu issue data ties a severe macOS 3D-performance regression to the
  geometry-shader-heavy depth precision work merged in PR #2240.

CPU emulation still matters because Apple Silicon runs the Xbox x86 CPU through
QEMU TCG rather than an x86 hardware virtualization path, but the current
visible regressions point first at the renderer.

## Non-Goals

- Do not paper over the problem by just downgrading permanently.
- Do not target Rosetta or Intel macOS as the primary performance path.
- Do not rely on deprecated macOS OpenGL for the final fast path.
- Do not accept "higher FPS but obviously wrong rendering" as success.

## Current Fork Baseline

- Source checkout: `/Users/jbbrack03/XEMU_MacOS/xemu-fork`
- Working branch: `apple-silicon-performance`
- Baseline upstream commit: `ebe34071b7fec3b6187248c57708fdd6cc3a8b97`
- Native arm64 baseline build:
  - command: `./build.sh -a arm64`
  - result: succeeds
  - app bundle: `dist/xemu.app`
  - executable: `dist/xemu.app/Contents/MacOS/xemu`
- Startup renderer observed from the baseline app:
  - GL vendor: `Apple`
  - GL renderer: `Apple M3 Ultra`
  - GL version: `4.1 Metal - 90.5`
- Baseline session file:
  - `docs/apple-silicon/benchmarks/2026-04-29-baseline.md`
- Automation smoke session file:
  - `docs/apple-silicon/benchmarks/2026-04-30-automation-smoke.md`
- Baseline metrics and snapshot smoke file:
  - `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`
- Native triangle-depth validation file:
  - `docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md`
- Retail gameplay route captures:
  - PGR2:
    `docs/apple-silicon/benchmarks/2026-05-01-pgr2-gameplay-route.md`
  - Rainbow Six 3:
    `docs/apple-silicon/benchmarks/2026-05-01-rainbow-gameplay-route.md`
  - Crimson Skies:
    `docs/apple-silicon/benchmarks/2026-05-01-crimson-gameplay-route.md`
- Current retail performance target (updated 2026-05-02 after the
  Soul Calibur 2 sanity test):
  - Floor: sustained **console-native** FPS in gameplay for each
    tracked title. PGR2 / Crimson Skies / Rainbow Six 3 are 30 Hz
    Xbox engines (cap is title-intrinsic, confirmed by `NV2A_VBLANK_FIRES
    > 30/s` while `NV2A_PRESENT_HEARTBEAT == 30/s`); 30 FPS floor met
    2026-05-01.
  - Quality: 1080p (`surface_scale=2`, default on first launch on
    Apple Silicon), opt-in MSAA up to 4× via `XEMU_GL_MSAA`.
  - Residual-stutter pillar: eliminate 1-second-class worst-frame
    judder. Currently open: V6 — `cpu_exec_loop` per-phase
    instrumentation to attribute the residual ~970 ms of unattributed
    sub-1 ms `tb_gen_code` churn + kernel-PC `0x80030e4c` 1 ms-class
    TB chains. See benchmarks/2026-05-02-apu-lock-release-validation.md
    "Recommended next action #3".
  - 60 FPS-capable titles (Soul Calibur 2, Burnout 3, OutRun 2, Ninja
    Gaiden Black, etc.) reach native 60 Hz at scale=2 + MSAA=4 per
    the V4 broader sweep.
- Current retail gameplay baselines:
  - PGR2 gameplay route: 11.53 FPS average, 11.67 FPS post-load,
    1,516,519 geometry-shader draws, including 38,785 quad-family draws.
  - Rainbow Six 3 gameplay route: 24.19 FPS average, 24.76 FPS post-load,
    692,438 geometry-shader draws, including 1,946 line-family draws.
  - Crimson Skies gameplay route: 15.44 FPS average, 15.80 FPS post-load,
    786,722 geometry-shader draws, including 7,837 quad-family draws.
- Earlier scripted smoke captures:
  - Crimson Skies: scripted route reaches rendered in-engine sequence.
  - Rainbow Six 3: scripted route reaches Hereford mission loading.
- Baseline FPS/frame-pacing metrics:
  - Crimson Skies B0: tail-60 average 30.98 FPS, 27.27 MSPF
  - Rainbow Six 3 B1: tail-60 average 30.98 FPS, 18.23 MSPF
- Verified snapshot-backed entries:
  - Crimson Skies: `crimson_scene_b0`
  - Rainbow Six 3: `rainbow_scene_b1_nothumb`
- Geometry-shader attribution metrics:
  - Crimson Skies B2: 25,202 geometry-backed draws in a 30s snapshot run, all
    triangle-family.
  - Rainbow Six 3 B3: 149,961 geometry-backed draws in a 30s snapshot run, all
    triangle-family.
- Current renderer status:
  - `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` did not improve Rainbow Six 3.
  - `XEMU_DIAG_SKIP_TRI_GEOM=1` dropped Rainbow Six 3 geometry draw counters to
    zero and reduced post-load MSPF from 17.66 to 6.38, while remaining a
    correctness-breaking diagnostic.
  - `XEMU_NATIVE_TRI_DEPTH=1` is the completed current opt-in replacement path
    for triangle-family fill draws:
    it keeps native GL triangle draws, derives depth/slope in the fragment
    shader, drops geometry draw counters to zero, and measured Rainbow Six 3 at
    6.47 to 6.78 post-load MSPF in flat-shading-safe reruns. It allows
    flat-shaded first-provoking triangle fills on the native path, matching the
    OpenGL renderer's `GL_FIRST_VERTEX_CONVENTION`, and still falls back to the
    geometry shader when flat shading requires a non-first provoking vertex.
    `XEMU_DIAG_NATIVE_TRI_DEPTH=1` is still accepted as a compatibility alias
    for older notes.
  - Follow-up coverage counters show the local Rainbow scene exercises native
    smooth-shaded w-depth, linear depth, and fill polygon offset. Crimson
    exercises native smooth-shaded linear depth and fill polygon offset. Visual
    smoke checks in both games did not show an obvious regression.
  - A dedicated nxdk flat-shading XBE now validates the flat split. Run
    `benchmark-runs/20260430-153555-flat-tri-depth` reports 480 flat-first
    native draws and 304 flat-nonfirst geometry-shader fallbacks. The reusable
    validator is `scripts/apple-silicon/validate-native-tri-depth.sh --run 20`;
    the fresh packaged-app run `benchmark-runs/20260430-210159-flat-tri-depth`
    passed with 422 flat-first native draws and 240 flat-nonfirst fallbacks.
  - The earlier all-smooth flat-XBE summaries were a benchmark-window artifact.
    xemu now emits a final `xemu-perf:` counter interval on graceful exit, and
    the benchmark launcher waits for QMP `quit` before falling back to
    termination.
  - Same-build paired comparisons are recorded for the current opt-in path:
    Rainbow Six 3 improved from 23.10 to 6.83 post-load MSPF with 0 native-run
    geometry draws, and Crimson Skies improved from 29.47 to 17.87 post-load
    MSPF with 0 native-run geometry draws.
  - `XEMU_NATIVE_TRI_DEPTH=0` explicitly disables the path and overrides the
    old compatibility alias when both are present.
  - Next renderer work should not re-prove triangle-family fill. It should
    start from the new retail gameplay routes, especially PGR2, and remove or
    narrow one of the remaining geometry-shader users: quad/quad-strip
    expansion, line primitives, polygon fill, or nonfill triangle modes.
- Local test assets:
  - `/Users/jbbrack03/XEMU_MacOS/Test_Games/Crimson skies.xiso.iso`
  - `/Users/jbbrack03/XEMU_MacOS/Test_Games/Rainbow Six 3.xiso.iso`
  - `/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso`
  - `/Volumes/Final Cut Pro Libraries/Projects/XEMU_MacOS/Test_Games/PGR2.xiso.iso`
  - `/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/bios/Complex_4627.bin`
  - `/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/mcpx/mcpx_1.0.bin`
  - `/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/hdd/xbox_hdd.qcow2`
  - prepared profile HDD for route replay:
    `/Users/jbbrack03/XEMU_MacOS/xemu-fork/benchmark-runs/profile-prep/xbox_hdd.qcow2`

## Documentation Map

- `research.md`: evidence gathered so far, with source links and local code
  references.
- `strategy.md`: proposed technical architecture and phased work.
- `benchmarking.md`: measurement plan and benchmark matrix.
- `automation.md`: scripted-input benchmark harness and launcher usage.
- `benchmarks/`: dated benchmark session notes and run templates.
- `decision-log.md`: dated decisions and rationale.
- `handoff.md`: current state and next-session checklist.

## Next Session Start

Start in `handoff.md`, section `Next Session Checklist` (top of the
2026-05-02 update block). The important state is:

- **Seven default-on flags ship on Apple Silicon system builds**
  (overridable via env vars):
  - `XEMU_NATIVE_TRI_DEPTH`, `XEMU_NATIVE_QUAD`,
    `XEMU_PGRAPH_FAST_READ` — geometry-shader bypasses + lock-free
    PGRAPH register reads (closed 2026-05-01).
  - `XEMU_TCG_SPLITWX` — splitwx on (V1, 2026-05-02).
  - `XEMU_TCG_JMP_CACHE_TARGETED` — per-page targeted jmp-cache
    invalidation (V2, 2026-05-02).
  - `XEMU_APU_LOCK_RELEASE` — APU worker releases d->lock during
    voice-worker batch wait (I5, 2026-05-02). **Has an audio
    listen-test gate before fully-shipped status.**
  - `display.quality.surface_scale = 2` — 1080p first-launch
    default; existing user configs preserved.
- **One opt-in renderer flag**: `XEMU_GL_MSAA={2,4,8}` (default 0;
  clamped to `GL_MAX_SAMPLES`, 4 on Apple GL-on-Metal).
- **One opt-in additional flag**: `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`
  (correct & shipped, does NOT fix the headline judder; default off).
- **Strategic verdict (2026-05-01, decisive): stay on OpenGL.** The
  GL-vs-Metal decision diagnostic
  (`benchmarks/2026-05-01-gl-vs-metal-decision.md`) showed Apple's
  GL has measured headroom for AA / 1080p on tracked titles. Native
  Metal is not the next priority.
- **30 FPS cap on PGR2 / Rainbow / Crimson is title-intrinsic
  (2026-05-02 SC2 sanity test).** Soul Calibur 2 sustains 60.57 FPS
  on the same build/flag stack. The literal "60 FPS on
  PGR2/Rainbow/Crimson" goal is technically impossible — those
  titles' engines render at 30 Hz on real Xbox hardware. See
  decision-log "2026-05-02: Confirm 30 FPS cap … is title-intrinsic
  …".
- **Highest-priority next task: V6 `cpu_exec_loop` per-phase
  instrumentation.** Attribute the residual ~970 ms of unattributed
  sub-1 ms `tb_gen_code` churn + kernel-PC `0x80030e4c` 1 ms-class
  TB chains in the Crimson 1.28-s worst frame. Leading follow-on
  fix: PPTC (strategy.md Phase 5a). See decision-log "2026-05-02: V3
  + D3 attribute the residual Crimson worst-frame to TCG-internal
  sub-1 ms churn (V6 next)".
- **V4 broader-title sweep validates default flag stack across the
  broader Xbox library.** 6 of 6 titles pass; 0 new pathologies; 4
  surface the same catalogued Crimson-class TCG TB-invalidation
  worst-frame pathology — one V6 fix would address them all. See
  `benchmarks/2026-05-02-broader-title-sweep.md`.
- Diagnostic infrastructure for community measurement on any Apple
  Silicon Mac: per-subsystem timing counters
  (`BIND_TEXTURES_US_TOTAL`, `TEX_UPLOAD_US_TOTAL`,
  `SURF_TO_TEX_US_TOTAL`, `SURF_UPLOAD_US_TOTAL`,
  `SURF_DOWNLOAD_US_TOTAL`, `FLUSH_DRAW_US_TOTAL`,
  `DRAW_BEGIN_US_TOTAL`, `FLIP_STALL_US_TOTAL`,
  `FLIP_STALL_GLFINISH_US_TOTAL`); TCG hot-path counters
  (`TCG_TB_EXEC_COUNT`, `TCG_TB_INVALIDATE_COUNT`,
  `TCG_NOTDIRTY_TRIPS`, `TCG_NOTDIRTY_PAGES_HIT`,
  `TCG_TB_INVALIDATE_BURST_MAX`, `TCG_JMP_CACHE_ZEROED_BUCKETS`,
  `TCG_INVALIDATE_WALL_US_MAX`); APU counters
  (`APU_LOCK_HOLD_US_TOTAL`, `APU_VCPU_LOCK_WAIT_US_MAX`); display
  pacing counters (`NV2A_VBLANK_FIRES`, `NV2A_PRESENT_HEARTBEAT`,
  `NV2A_FLIP_STALL_WRITES`, `XEMU_GL_SWAPS`); MSAA cost
  (`MSAA_RESOLVE_US_TOTAL`); per-event spike log
  (`XEMU_PERF_SPIKE_LOG=1`, `XEMU_PERF_SPIKE_LOG_TCG=1`); and
  renderer-load A/B knob (`XEMU_BENCH_SURFACE_SCALE=N`).

## First Principle

Every meaningful change needs a benchmark before/after and a correctness check.
When data is missing, gather it before committing to architecture.
