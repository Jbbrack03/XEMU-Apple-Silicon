# Apple Silicon Performance Fork

Last updated: 2026-05-04 (Metal boot/flubber + surface/RTT follow-up.
PGR2 has clean Metal menu/logo/text/color output with the translated
pipeline and front-fb fallback; Rainbow Six 3 loading-screen output is
also clean. The previously reported green/wireframe failure was the
Xbox boot/flubber animation, not in-game Crimson Skies, and that boot
canary now renders shaded geometry and glow without texture blobs.
**The user's stated 30/60 FPS at 1080p / high-quality AA /
correct-colors goals remain met today via the GL renderer** with
`XEMU_GL_MSAA=4` + `surface_scale=2`; Metal remains opt-in /
experimental until the broader visual-diff/gameplay gate passes. Input
slices N1+N2 also shipped via opt-in `XEMU_MACOS_NATIVE_INPUT=1`
GameController.framework backend.)

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
  - Metal product path: native Metal is now the primary renderer
    direction for frame timing, latency work, capture/profiling,
    MSAA/resolve control, enhancement hooks, pipeline caching, and
    maintainability. OpenGL remains the runnable reference/fallback.
  - Residual-stutter pillar: V9/V10 declared the Crimson 1.3 s class
    judder best-effort complete within the current TCG architecture;
    remaining improvement requires larger rearchitecture or
    game-specific work, not more OpenGL renderer polish.
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
- `metal-renderer-plan.md`: **(2026-05-04)** staged Metal renderer
  implementation plan, slices M0–M15, validation gates, risk register,
  open questions, and current M5.x follow-up / M15 blocker status.
  Read after `handoff.md` when working on the Metal port.
- `metal-api-reference.md`: **(2026-05-02)** Apple Metal API surface
  reference for Phase 4 implementation — device/queue, render pipelines,
  MSL specifics, buffers/textures, MSAA, frame timing, sync, MetalFX,
  capture, GPU family detection, common emulator pitfalls, and a
  "Recommended Apple Silicon defaults" quick-reference table.
- `emulator-metal-survey.md`: **(2026-05-02)** file-level findings
  from Dolphin / PCSX2 / DuckStation / MoltenVK Metal backends and
  xemu's own Vulkan renderer as the structural template; "12 Patterns
  to Steal" / "5 Anti-Patterns to Avoid".
- `macos-input-research.md`: **(2026-05-02)** GameController.framework
  migration plan with proposed input slices N1–N6, independent of the
  renderer track.

## Next Session Start

**Read `handoff.md` first.** Its top 2026-05-04 section is the
authoritative current-state briefing and lists the next-action
priority. The summary below is for orientation only — `handoff.md`
wins when the two diverge.

**Current state (2026-05-04, post PGR2 Metal surface/RTT fix).**

- **Eight default-on Apple Silicon flags ship**: `XEMU_NATIVE_TRI_DEPTH`,
  `XEMU_NATIVE_QUAD`, `XEMU_PGRAPH_FAST_READ`, `XEMU_TCG_SPLITWX`,
  `XEMU_TCG_JMP_CACHE_TARGETED`, `XEMU_APU_LOCK_RELEASE`
  (PARTIAL — audio listen-test now UNBLOCKED), `XEMU_FAST_RDTSC` (V9,
  −36 % helper_rdtsc cost), plus `display.quality.surface_scale = 2`
  first-launch default.
- **Opt-in GL renderer flags**: `XEMU_GL_MSAA={2,4,8}` (default 0;
  clamped to `GL_MAX_SAMPLES`, 4 on Apple GL-on-Metal),
  `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` (default off; correct &
  shipped, does NOT fix headline judder),
  `XEMU_GL_RATE_SLEW`/`XEMU_RATE_SLEW` (M10 prerequisite, default off).
- **Metal renderer slices M0–M14 SHIPPED 2026-05-02, with M5.x
  correctness follow-ups continuing through 2026-05-04.** The Metal
  renderer is opt-in via `XEMU_RENDERER=METAL` (or
  `display.renderer = METAL` in `xemu.toml`). 13 `XEMU_METAL_*` flags
  + the `XEMU_RENDERER` env-var bridge (14 total Metal-track flags),
  50 `METAL_*` performance counters, plus the 2 graphics-API-agnostic
  `RATE_SLEW_*` counters surface on the `xemu-perf:` interval line.
  PGR2 and Rainbow Six 3 visual canaries are clean; the Xbox
  boot/flubber canary is also clean after the Metal front-face fix.
  Default renderer remains OpenGL; M15 (default-on flip) is BLOCKED on
  a broader Metal-vs-GL visual-diff and gameplay gate rather than this
  specific boot-animation failure.
- **Judder pillar declared "best effort complete" (2026-05-02 after
  V9 + V10).** All xemu-side cost classes < 100 ms (~7 %) of the
  Crimson 1.3 s worst-frame interval; remaining ~93 % is raw JIT'd
  guest x86 code execution. The 1.3 s class stutter is
  guest-intrinsic, amplified ~5× by xemu's TCG ISA-emulation
  overhead. Further reduction requires major rearchitecture
  (PPTC + AOT codegen, HLE Xbox kernel, or game-specific patches),
  all out of current scope.
- **30 FPS cap on PGR2 / Rainbow / Crimson is title-intrinsic**
  (SC2 sanity test sustains 60.57 FPS on same build). The literal
  "60 FPS on PGR2/Rainbow/Crimson" goal is technically impossible.

**Next-action priority.**

1. **Run the broader Metal-vs-GL gameplay gate.** Include PGR2,
   Rainbow Six 3, Crimson Skies gameplay after the boot animation, SC2,
   and one further title with paired screenshots/FPS/jitter/input
   counters before revisiting M15 default-on.
2. **Keep the green canaries green after each Metal fix.** PGR2:
   `benchmark-runs/20260504-092708-pgr2` /
   `benchmark-runs/visual-checks/pgr2-post-oob-f900.png`. Rainbow:
   `benchmark-runs/20260504-092750-rainbow-six-3` /
   `benchmark-runs/visual-checks/rainbow-post-oob-f600.png`.
   Boot/flubber: `benchmark-runs/20260504-092824-crimson-skies` /
   `benchmark-runs/visual-checks/boot-post-oob-f300.png`. Crimson
   gameplay stability:
   `benchmark-runs/20260504-092403-crimson-skies`.
3. **Only then revisit M15 default-on.** Outcome from the paired
   gameplay and visual-diff sweep feeds the M15 decision.
4. **Track B (Audio listen-test for `XEMU_APU_LOCK_RELEASE`, still
   UNBLOCKED, GL-side, orthogonal to Metal):** A human listener
   plays Crimson, Rainbow, PGR2 for ≥ 5 minutes each with the slice
   on. If clean: declare I5 fully shipped. If glitches: revert or
   design finer-grained lock split.

**Implementation candidates (lower priority than user-driven
validation):** M8.1 full hybrid ubershader (Path A), M10.1
CAMetalDisplayLink integration, M11.1 memoryless MSAA storage, M6
Part B remaining items (full S3TC/3D/cube/palette + lifecycle
hook), NV2A draw-pass per-stage GPU timing.

**Steady-state perf candidates (lower priority than A or B):**
PPTC (strategy.md Phase 5a — Ryujinx pattern; ~4 % vCPU savings;
saves only ~44 ms in the headline worst-frame so does NOT close the
judder gap). V11 (`helper_lookup_tb_ptr` per-vCPU cache; ~4 %
steady-state win).

**Diagnostic infrastructure** (for community measurement on any
Apple Silicon Mac): per-subsystem timing counters
(`BIND_TEXTURES_US_TOTAL`, `TEX_UPLOAD_US_TOTAL`,
`SURF_TO_TEX_US_TOTAL`, `SURF_UPLOAD_US_TOTAL`,
`SURF_DOWNLOAD_US_TOTAL`, `FLUSH_DRAW_US_TOTAL`,
`DRAW_BEGIN_US_TOTAL`, `FLIP_STALL_US_TOTAL`,
`FLIP_STALL_GLFINISH_US_TOTAL`); TCG hot-path counters
(`TCG_TB_EXEC_COUNT`, `TCG_TB_INVALIDATE_COUNT`,
`TCG_NOTDIRTY_TRIPS`, `TCG_NOTDIRTY_PAGES_HIT`,
`TCG_TB_INVALIDATE_BURST_MAX`, `TCG_JMP_CACHE_ZEROED_BUCKETS`,
`TCG_INVALIDATE_WALL_US_MAX`, `TCG_INVALIDATE_WALL_US_TOTAL` (V10),
`TCG_TB_LOOKUP_US_TOTAL` / `TCG_TB_GEN_CODE_US_TOTAL` /
`TCG_HANDLE_INTERRUPT_US_TOTAL` (V7), `HELPER_RDTSC_CALLS` (V9));
APU counters (`APU_LOCK_HOLD_US_TOTAL`,
`APU_VCPU_LOCK_WAIT_US_MAX`); display pacing counters
(`NV2A_VBLANK_FIRES`, `NV2A_PRESENT_HEARTBEAT`,
`NV2A_FLIP_STALL_WRITES`, `XEMU_GL_SWAPS`); MSAA cost
(`MSAA_RESOLVE_US_TOTAL`); per-event spike log
(`XEMU_PERF_SPIKE_LOG=1`, `XEMU_PERF_SPIKE_LOG_TCG=1`); per-frame
mspf log (`XEMU_PERF_FRAME_LOG=1`); cumulative TCG-phase log
(`XEMU_TCG_PHASE_LOG=1`); renderer-load A/B knob
(`XEMU_BENCH_SURFACE_SCALE=N`).

## First Principle

Every meaningful change needs a benchmark before/after and a correctness check.
When data is missing, gather it before committing to architecture.
