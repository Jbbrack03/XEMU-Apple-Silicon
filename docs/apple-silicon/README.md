# Apple Silicon Performance Fork

Last updated: 2026-05-19 night (Apple-aligned Metal workflow adopted in
the canonical docs; tooling slice shipped; retail Xbox oracle available
again after the post-repaste thermal recheck; the late PGR2
same-VRAM linear-alias bridge now uses `copy-alias` instead of a
VRAM round-trip; Metal default-on is still blocked). The retail Xbox
oracle is production-ready
for the stable trio: Crimson Skies, Rainbow Six 3, and PGR2 all have live
`retail-oracle-workflow.py` proofs with `workflow.json` `status=ok`, and
the hardware is back in service after the May 19 repaste validation in
`benchmarks/2026-05-19-retail-oracle-post-repaste-thermal-check.md`.
Soul Calibur 2 is still deferred as a retail-oracle production gate on the
current Xbox image because repeated real-hardware return/IGR proofs leave
the console offline, but it remains useful for emulator-side renderer and
performance validation.

Before any M15/default-on claim, run:

```sh
./scripts/apple-silicon/m15-bundle-status.py
```

The 2026-05-11 evening result is `verdict=incomplete ok=6 fail=5 missing=4`
(after the same-day m15-gameplay-* discovery extension and the front-fb
fallback policy decision-log entry).
Oracle-side evidence is green, but the title-level Metal-vs-GL bundle is
not closed: the 2026-05-11 PGR2/Rainbow paired passes only proved cleaner
capture/static-canary comparison (black boot-ish PGR2 and Rainbow loading
screen), not gameplay visual parity. M15 requires matched gameplay keyframes
from controller routes, aligned by visual content rather than timestamp and
reviewed as GL/Metal/oracle triptychs where possible. Crimson's older paired
diff still fails (`changed_pct=14.7560`). A 2026-05-11 evening PGR2 gameplay
evidence attempt also failed: after fixing the GL leg to strict xemu-window
capture and adding source-specific crops to
`m15-gameplay-visual-compare.py`, the relaxed diagnostic still showed
85.4635..100.0000% changed pixels and Metal NV2A profile/menu captures
missing the GL/oracle background detail. SC2/Halo/Rainbow gameplay paired
diffs are missing, PGR2/Rainbow/Crimson p99 jitter gates fail, cold
shader compile proof is missing, and the front-fb fallback policy is
resolved to opt-in (not default-on). The current app build still does not
expose QMP/HMP `screendump`;
`metal-gl-compare.sh --trigger flip` uses the GL renderer's
`XEMU_GL_SCREENSHOT_PATH` path and Metal `XEMU_METAL_SCREENSHOT_SOURCE=nv2a`.

Next session should start in `docs/apple-silicon/handoff.md` at
"START HERE NEXT SESSION — M15 bundle closure". The first concrete task is no
longer a capture-source check or the old linear alias-to-VRAM bridge: the
May 19 follow-up reruns confirmed a real host-refresh publish overwrite bug,
kept that fix, rejected the tempting `0x3b58000` display-shape publish
heuristic, replaced the late same-VRAM linear alias bind with
`path=copy-alias`, and still failed local GL-vs-Metal gameplay compare.
Start from
`benchmarks/2026-05-19-pgr2-snapshot-publish-and-rtt-followup.md` and debug
the copied late stage-0 RTT content/format/use-site for `0x3c84000` in the
Metal path; retail-oracle PGR2 gameplay work remains deferred until local
content alignment improves.

Project is still in Metal Phase 1/2 closure work. **The user's stated
30/60 FPS at 1080p / high-quality AA / correct-colors goals remain met
today via the GL renderer** with `XEMU_GL_MSAA=4` + `surface_scale=2`;
Metal remains opt-in until the M15 evidence bundle is complete and green.

## Apple-aligned Metal workflow

When a session touches the native Metal renderer, the project now follows
Apple's documented migration/debug/profiling loop rather than an ad hoc
"title symptom first" loop:

1. Reproduce on a stable scene or route.
2. Turn on validation first (`XEMU_METAL_VALIDATION=1`; use shader
   validation / post-build fixture validation where relevant).
3. Capture the failing frame or short sequence with Xcode GPU capture
   (`.gputrace`) and use Instruments / Metal System Trace to classify the
   issue as correctness, CPU, GPU, or CPU/GPU overlap.
4. Use project-specific tools (paired GL/Metal diffs, per-draw RT dumps,
   oracle triptychs, temporal capture) as reproducer/oracle layers around
   Apple's tools, not as substitutes for them.
5. Only optimize after the bottleneck is measured. Re-measure after every
   meaningful fix.

`metal-porting-workflow.md` is the canonical playbook for this loop.

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
- `retail-title-patching-strategy.md`: current retail-game oracle plan for
  per-title XBE patches, including the autonomous dashboard-return requirement.
- `metal-porting-workflow.md`: **(2026-05-04)** canonical operating
  playbook for the native Metal renderer port. Five-phase model
  (build & boot → translation correctness → visual parity → perf
  parity → default-on), Apple-aligned daily loop for the active phase
  (validate → capture → classify → optimize → re-measure), tools
  index, triage flowchart, phase exit-gate procedures, triangulation
  appendix. Read after `handoff.md` when starting a Metal-track
  session.
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
- `nv2a-feature-surface-research.md`: **(2026-05-05, Codex-revised
  2026-05-06)** comprehensive feature catalog of the NV2A rendering
  pipeline assembled from three independent witnesses (xemu source,
  nxdk + pbkit, external docs). Foundation for the diagnostic-XBE
  library. Read when designing or auditing diagnostic XBEs.
- `diagnostic-xbe-plan.md`: **(2026-05-06 v2 — supersedes v1
  d57742ef47 which was Codex-flagged BLOCKING)** implementation plan
  for the diagnostic-XBE library. Self-validation tiers (host-side
  capture primary, guest-side VRAM readback escape hatch), shared
  infrastructure, manifest schema, per-XBE specs for the first 16
  priority XBEs, build sequence. Read when implementing the
  diagnostic-XBE library. **Phase 3.0 (`pipeline-smoke`) shipped
  2026-05-06 and proves the chainload-and-back orchestrator
  plumbing; Tier-1 NV2A-pipeline XBEs (mirror / color-channel /
  depth-floor per §4.1–§4.3) are next.**
- `real-xbox-oracle-feasibility.md`: **(2026-05-06)** feasibility
  research for using a real OpenXenium-modded Original Xbox as a
  hardware oracle. Original architecture (XBDM debug kernel +
  PrometheOS) was superseded same day after the iND-BiOS revision
  blocked the leaked `xbdm.dll` path; pivot to a custom nxdk
  oracle agent shipped Phase 1+2+3.0 against the project Xbox.
  Read when planning Xbox-side work or when reviewing why the
  XBDM-leg architecture was superseded.

In-tree oracle artifacts (added 2026-05-06):

- `scripts/apple-silicon/xbe-tests/oracle-agent/` — nxdk XBE,
  the persistent network-listening oracle agent. TCP 9001.
  Phase 1+2 shipped: info / eeprom / mem.read / mem.write /
  nv2a.read / nv2a.write / vram.read / screenshot / runxbe /
  unsafe.enable / reboot / bye / help.
- `scripts/apple-silicon/xbe-tests/eeprom-dump/` — nxdk XBE,
  one-shot 256-byte EEPROM capture (raw + decrypted info file).
- `scripts/apple-silicon/xbe-tests/pipeline-smoke/` — **Phase
  3.0** Tier-4 diag XBE that validates the orchestrator
  pipeline end-to-end via a CPU-painted single-pixel oracle.
  Real Tier-1 NV2A-pipeline diag XBEs build on top of the same
  XOSS-capture-then-reboot skeleton.
- `scripts/apple-silicon/oracle-client.py` — Mac-side Python
  class + CLI wrapping the agent's TCP-9001 protocol.
- `scripts/apple-silicon/oracle-orchestrator.py` — Mac-side
  pipeline driver: status / ensure-agent / capture / run-diag
  / validate. `run-diag` is the autonomous chainload-and-back
  cycle for diagnostic XBEs.
- `scripts/apple-silicon/xbox-ftp-mirror.py` — Python recursive
  FTP mirror with SHA-256 manifest; used for Tier-1 backups.
- `docs/apple-silicon/xbox-real-references/<xbe-id>/*.png` —
  canonical real-Xbox reference frames captured via
  `oracle-orchestrator.py capture` after a diag-XBE run.

## Next Session Start

**Read `handoff.md` first.** Its TOP-OF-STACK 2026-05-06
(Phase 3.0 PASS) banner is the authoritative current-state
briefing and lists the next-action priority. The summary below
is for orientation only — `handoff.md` wins when the two
diverge.

**Current state (2026-05-06 — Phase 3.0 PASS).**

- **Real-Xbox oracle pipeline operational end-to-end.** The
  `oracle-orchestrator.py run-diag` cycle (ensure-agent →
  pre-screenshot → runxbe → wait FTP back → pull artifacts →
  relaunch agent → post-screenshot) is proven against the
  project Xbox via the `pipeline-smoke` Tier-4 diag XBE.
  Captured framebuffer SHA-256
  `66f1f332f0bec182be06a53447221047af250ca708bb3525ee842821197e34b4`,
  byte-for-byte identical across run-2 + run-3 + the math-derived
  expected. Three commits: `c2274310fc` (Phase 2 land),
  `abac6b5017` (orchestrator FTP except-clause fix),
  `aae0138565` (Phase 3.0 pipeline-smoke).
- **Two orchestrator bugs fixed in flight:** (a) FTP except
  clause used `(OSError, ftplib.all_errors)` which Python
  rejects (the second is a tuple) — fixed via module-level
  `_FTP_ERRORS` tuple; (b) `run_diag` was relaunching the agent
  BEFORE pulling FTP artifacts, but the agent suspends XBMC's
  FTP server — reordered to pull-then-relaunch.
- **Phase 3.0 caveat:** `pipeline-smoke` is Tier-4 (CPU-painted
  framebuffer; no NV2A pgraph). It validates orchestrator
  plumbing, NOT renderer behavior. The SC2 visual symptoms
  (top-mirrored, wrong colors, missing floor) require Tier-1
  NV2A-pipeline diag XBEs — that's Phase 3.1+ work.

**Next-action priority (2026-05-06).**

1. **Phase 3.1 — first Tier-1 NV2A diag XBE.** Build `mirror`
   per `diagnostic-xbe-plan.md` v2 §4.1: VS path through
   pgraph, single-pixel triangle at guest coord (320, 50),
   surface scale 1, opaque-black back-buffer. Same XOSS-
   capture-then-reboot skeleton pipeline-smoke established;
   `oracle-orchestrator.py run-diag` is already proven and
   ready to drive it.
2. **Phase 3.2 — `color-channel` and `depth-floor`** per §4.2
   and §4.3.
3. **Wire Tier-1 diag XBEs into the M15 visual gate.** Add a
   harness step that runs each through xemu-GL + xemu-Metal +
   real Xbox; per-(renderer, flag-recipe) PASS/FAIL via
   `oracle-orchestrator.py validate` (which threads
   `--crop --out-dir --threshold` into `compare-screenshots.py`).
4. **(Optional) Build the shared `xbe-tests/lib/` skeleton**
   per `diagnostic-xbe-plan.md` §3.1 once 3+ XBEs share
   pbkit/banner/capture boilerplate.

**Earlier banner — 2026-05-05 (preserved verbatim for
empirical audit trail; now superseded by the Phase 3.0 PASS
state above).**

- **W3 regression gate is operational autonomously.** Run
  `./scripts/apple-silicon/metal-canary-regress.sh` (defaults to
  counter mode) for a 6-minute PASS/FAIL verdict on all four canaries
  (pgr2 / rainbow / halo / boot). Catches PSH/VSH translator failures,
  M5.7 coalescing collapse, drawable starvation, present-path silence
  without depending on pixel-perfect golds. Use after every Metal
  renderer change.
- **Workflow tooling status: D1 / W1 / W2 / W3 (counters) / W4 / F1 /
  F2 / VFR / skills / hooks all operational.** W5 BLOCKED (MoltenVK).
- **Three recorded gameplay scripts already exist**:
  `pgr2-gameplay.csv` (33k lines), `rainbow-gameplay.csv` (61k lines),
  `crimson-gameplay.csv` (26k lines). PGR2 and Rainbow render real
  game content via Metal under their existing scripts. Crimson's
  script reaches the gameplay state (stability PASS, counters clean)
  but Metal renders the resulting frames as a single patterned frame
  followed by a black drawable — a renderer correctness bug that
  needs diagnosis via W4 per-draw RT dump (Metal vs GL on matched
  draw indices). Only **SC2 lacks a recorded input script**; the
  existing `noop.csv` reaches only boot/flubber.
- **Remaining open work for M15 default-on**: (a) diagnose +
  fix the Crimson Metal visual route via W4 + W2 (autonomous);
  (b) record SC2 via `record-input.sh` (interactive); (c) run the
  full 5-title paired Metal-vs-GL diff under the M15 gate; (d)
  decide front-fb fallback policy.

**Current state (2026-05-04, post PGR2 Metal surface/RTT fix — preserved
for empirical audit trail).**

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

**Next-action priority (2026-05-05).**

1. **(autonomous) Run paired Metal-vs-GL diff for PGR2, Rainbow,
   Crimson** using the existing recorded scripts:
   ```
   for title in pgr2 rainbow crimson; do
     ./scripts/apple-silicon/metal-gl-compare.sh "$title" \
       --input scripts/apple-silicon/input-scripts/${title}-gameplay.csv \
       --trigger flip --trigger-ordinal 30 --threshold 1.0
   done
   ```
   Use F1 flip-stall trigger for paired-frame alignment. PGR2 and
   Rainbow are expected to be near-pass (modulo color/gamma deltas);
   Crimson is expected to FAIL because the Metal gameplay route
   currently renders one patterned frame followed by a black
   drawable.
2. **(autonomous) Diagnose the Crimson black-drawable visual
   regression** via W4 per-draw RT dump on both renderers:
   ```
   XEMU_RENDERER=METAL XEMU_METAL_DUMP_DRAW_RT=0:200:/tmp/mtl_crimson \
     ./scripts/apple-silicon/run-benchmark.sh crimson \
       scripts/apple-silicon/input-scripts/crimson-gameplay.csv 60
   XEMU_RENDERER=GL XEMU_GL_DUMP_DRAW_RT=0:200:/tmp/gl_crimson \
     ./scripts/apple-silicon/run-benchmark.sh crimson \
       scripts/apple-silicon/input-scripts/crimson-gameplay.csv 60
   ```
   Compare matched draw indices to localise the NV2A semantics bug.
3. **(INTERACTIVE) Record SC2 input script** via
   `./scripts/apple-silicon/record-input.sh sc2`. The only title
   without a recorded route. Optionally pair with a QMP snapshot at
   a stable visual frame.
4. **Run `metal-canary-regress.sh --mode counters` after every Metal
   renderer change** to catch translator failures, coalescing
   collapse, drawable starvation (~6-minute autonomous loop;
   "post-change smoke" per `metal-porting-workflow.md` §3.5).
5. **After Crimson visual is fixed and SC2 has a script**, run the
   full 5-title M15 default-on visual gate (PGR2 / Rainbow / Crimson
   / SC2 + one broader-sweep title). ≤1% per-pixel diff vs GL on
   all five = visual gate met. Combine with FPS / p99 jitter
   validation.
6. **Decide whether to flip `XEMU_METAL_FRONT_FB_FALLBACK` default
   to ON** after the wider title sweep characterizes which title
   classes benefit / regress. Currently default OFF; project rule #2
   prefers a faithful CRTC publish path over shipping a known visual
   bug class.
7. **Track B (Audio listen-test for `XEMU_APU_LOCK_RELEASE`, still
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
