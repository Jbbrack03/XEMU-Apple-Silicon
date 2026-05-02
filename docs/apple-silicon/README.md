# Apple Silicon Performance Fork

Last updated: 2026-05-01

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
- Current retail performance target:
  - Floor: sustained 30 FPS in gameplay for all tracked titles.
  - Stretch: 60 FPS where possible.
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

Start in `handoff.md`, section `Next Session Checklist`. The important state is:

- Do not spend the next session revalidating `XEMU_NATIVE_TRI_DEPTH=1`,
  `XEMU_NATIVE_QUAD=1`, or `XEMU_PGRAPH_FAST_READ=1` unless their
  underlying code paths change. All three flags are landed, visually
  validated, and verified to meet the 30 FPS gameplay floor on PGR2 /
  Rainbow Six 3 / Crimson Skies.
- A fourth opt-in flag landed 2026-05-01:
  `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` (worker-thread shader compile
  via a third shared GL context, RPCS3 PR #4876 "skip the draw"
  pattern). It is **correct and shipped opt-in** but does **NOT**
  fix Crimson Skies' 1.35-second worst-frame stutter. Default off.
  See `benchmarks/2026-05-01-async-shader-compile.md`.
- Strategic verdict (2026-05-01, decisive): **stay on OpenGL.** The
  GL-vs-Metal decision diagnostic
  (`benchmarks/2026-05-01-gl-vs-metal-decision.md`) showed Apple's
  GL has measured headroom for 60 FPS at 1080p (and even at 4×-scale
  internal resolution) on tracked titles. The headline 1.35-second
  Crimson stutter is in the **TCG vCPU thread**, not the renderer —
  Apple Silicon-specific QEMU MTTCG TB-invalidation cost
  (`tb_invalidate_phys_range_fast` → `do_tb_phys_invalidate` plus
  `pthread_jit_write_protect_np` and `sys_icache_invalidate`).
  Native Metal would not address it.
- **Highest-priority next task: TCG TB-invalidation cost reduction
  on Apple Silicon (strategy.md Phase 5a).** PPTC, W^X-toggle
  batching, smarter softmmu notdirty handling, or upstream QEMU
  patches. Everything else (MSAA-on-GL, broader title sweep, frame
  pacing) is downstream.
- Diagnostic infrastructure for community measurement on any Apple
  Silicon Mac: per-subsystem timing counters
  (`BIND_TEXTURES_US_TOTAL`, `TEX_UPLOAD_US_TOTAL`,
  `SURF_TO_TEX_US_TOTAL`, `SURF_UPLOAD_US_TOTAL`,
  `SURF_DOWNLOAD_US_TOTAL`, `FLUSH_DRAW_US_TOTAL`,
  `DRAW_BEGIN_US_TOTAL`, `FLIP_STALL_US_TOTAL`,
  `FLIP_STALL_GLFINISH_US_TOTAL`), per-event spike log
  (`XEMU_PERF_SPIKE_LOG=1`), and renderer-load A/B knob
  (`XEMU_BENCH_SURFACE_SCALE=N`).

## First Principle

Every meaningful change needs a benchmark before/after and a correctness check.
When data is missing, gather it before committing to architecture.
