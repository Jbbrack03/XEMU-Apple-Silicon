# Metal Porting Workflow

Last updated: 2026-05-04. This is the canonical operating playbook for
the native Metal renderer port. It supersedes nothing — `metal-renderer-plan.md`
remains the slice-level implementation plan (M0–M15), `handoff.md`
remains the per-session current-state pointer, and `decision-log.md`
remains the append-only record of binding decisions. This document
sits one level above those: it captures the **phased process** by
which we move the Metal renderer from "almost-shipping infrastructure"
to "default-on production renderer", and prescribes the daily loop a
session should follow inside each phase.

This document was added 2026-05-04 alongside the parallel automation
slices D1 (this doc) and W1 / W2 / W3 / W4 / W5 (auto-on validation,
paired diff harness, canary regression gate, per-draw RT dump,
MoltenVK triangulation backend). Forward-language references to those
slices throughout this document mean "introduced 2026-05-04 in slice
W*" — see `handoff.md` for the current implementation status of each.

---

## 1. Status banner

**Phase pointer: Phase 1 — Translation Correctness, ACTIVE.**

Date: 2026-05-04. Last canary state pulled from `handoff.md`:

- PGR2 MSAA4: **PASS** as a static-canary
  (`benchmark-runs/20260504-100458-pgr2`,
  `benchmark-runs/visual-checks/pgr2-gate-metal-msaa4-f900-after-msaa-store.png`).
  PGR2 still requires `XEMU_METAL_FRONT_FB_FALLBACK=1` to publish a
  correct frame; without the fallback the captured drawable is
  upside-down/wrong
  (`benchmark-runs/20260504-101416-pgr2`).
- Rainbow Six 3 loading-screen MSAA4: **PASS**
  (`benchmark-runs/20260504-100546-rainbow-six-3`,
  `benchmark-runs/visual-checks/rainbow-gate-metal-msaa4-f600-after-msaa-store.png`).
- Halo CE menu MSAA4: **PASS**
  (`benchmark-runs/20260504-101125-halo-ce`,
  `benchmark-runs/visual-checks/halo-gate-metal-msaa4-f1200-after-msaa-store.png`).
- Xbox boot/flubber MSAA4: **PASS**
  (`benchmark-runs/20260504-100747-crimson-skies`,
  `benchmark-runs/visual-checks/boot-gate-metal-msaa4-f300-after-msaa-store.png`).
- Crimson Skies gameplay visual route: **BLOCKED** — the route
  produces one patterned frame followed by black drawable captures
  (`benchmark-runs/20260504-100815-crimson-skies`); Visual Flight
  Recorder quantifies an existing failed Crimson sequence as 92.31 %
  black frames after one patterned frame.
- Soul Calibur 2 visual route: **BLOCKED** — counters look strong
  (`post_load_avg_fps=57.63` in
  `benchmark-runs/20260504-101242-soul-calibur-2`) but the no-input
  route captures only boot/flubber and then black; Visual Flight
  Recorder quantifies an existing SC2 sequence as 87.50 % black after
  boot/flubber.
- M5 shader-translation harness: **7/7 PASS** via
  `scripts/apple-silicon/metal-shader-validation/run-validation.sh`.

The user-stated 30/60 FPS at 1080p / high-quality AA / correct-colors
goals **remain met today via the GL renderer** with `XEMU_GL_MSAA=4`
+ `surface_scale=2` + `XEMU_MACOS_NATIVE_INPUT=1`. Metal is opt-in
until M15 default-on flips, and M15 stays BLOCKED on the items
catalogued in `handoff.md` and `metal-renderer-plan.md` §4 M15.

---

## 2. Phase model

The Metal port is organized into five sequential phases. Each phase
has a single dominant question; do not start the next phase until the
current phase's exit gate is met. A phase regression while a later
phase is active sends the team back to the failing phase; do not paper
over a translation-correctness failure with a performance fix.

### Phase 0 — Build & boot — DONE

**Dominant question.** Can a Metal-renderer xemu build, link, sign,
and reach a HUD over a black drawable on Apple Silicon?

**Current state.** DONE. M0–M14 SHIPPED 2026-05-02; the empty `mtl/`
directory was filled with the eleven core source pairs
(`renderer.{c,h}`, `heap.{h,mm}`, `surface.{h,mm}`, `buffer.{h,mm}`,
`pipeline.{h,mm}`, `shaders.mm`, `shadergen.c`, `texture.{h,mm}`,
`draw.{h,mm}`, `glsl.{c,h}`, `shader_validation.{c,h}`, `vertex.{c,h}`,
`state.c`, `index_gen.{c,h}`, `blit.c`); spirv-cross + glslang are
build dependencies; `XEMU_RENDERER=METAL` selects Metal at xemu
startup; `dist/xemu.app` codesigns and launches.

**Entry criteria.** A pre-existing baseline xemu build that passes
`./build.sh -a arm64` and signs cleanly.

**Exit gate.** `./build.sh -a arm64` PASS, `codesign --verify --deep
--strict --verbose=2 dist/xemu.app` PASS, `XEMU_RENDERER=METAL` boots
to a HUD overlay over a black drawable. M5 shader-translation harness
runs end-to-end and reports a numeric pass/fail.

**Key tools.** `./build.sh -a arm64`, `codesign`, `validate-native-tri-depth.sh`
as a Phase 0 sanity gate (it tests the GL path but proves the build
hasn't regressed the eight default-on flags).

### Phase 1 — Translation correctness — ACTIVE

**Dominant question.** For each tracked title, does the Metal
renderer produce a visually-correct rendered frame on every
benchmarked route?

**Current state.** ACTIVE. PGR2 / Rainbow / Halo / boot canaries
PASS; Crimson gameplay route and SC2 no-input route produce black
drawable captures; M5 shader harness PASS; Metal validation layer is
opt-in via `XEMU_METAL_VALIDATION=1` (W1 will make it auto-on for the
benchmark harness when `XEMU_RENDERER=METAL`).

**Entry criteria.** Phase 0 exit gate met.

**Exit gate.** Every tracked title (PGR2, Rainbow Six 3, Crimson
Skies, Soul Calibur 2, plus one broader-sweep title from V4)
produces a non-black, route-correct, MSAA4 visual capture under
Metal at the same scripted-input route used for the GL baseline. M5
shader-translation harness 7/7 PASS. `METAL_PIPELINE_TRANSLATED_FAILED
== 0` and `METAL_DRAWS_SKIPPED_PENDING_TOTAL == 0` across the full
route. No MTLValidation API errors fired during the run.

**Key tools.** `XEMU_METAL_VALIDATION=1` (M14 — auto-on for dev runs
via W1), `XEMU_METAL_SHADER_VALIDATE=1` (M5), the M5 shader harness
script, `XEMU_BENCH_VISUAL_ANALYSIS=1` (Visual Flight Recorder),
`XEMU_METAL_DUMP_DRAW_RT` (W4 — per-draw color RT dump for
first-divergent-draw isolation), `metal-gl-compare.sh` (W2 — paired
Metal-vs-GL diff at matched intervals), MoltenVK triangulation
backend (W5 — three-way Metal/GL/Vulkan-over-Metal cross-check;
HEDGE: blocks on MoltenVK GS support per the W5 risk note).

### Phase 2 — Visual parity gate

**Dominant question.** Is Metal's output within the documented
per-pixel tolerance of GL's output, frame-for-frame, on the same
scene?

**Current state.** Pending. The PGR2 / Rainbow / Halo / boot static
canaries are positively diffed by hand against known-good GL
references (recorded in `handoff.md`'s screenshot links), but no
automated paired-interval per-pixel diff has run yet across the full
gameplay routes for Crimson and SC2.

**Entry criteria.** Phase 1 exit gate met for at least PGR2 + Rainbow
+ Crimson + SC2 + one broader-sweep title.

**Exit gate.** Paired Metal-vs-GL screenshot capture at matched
intervals on each of PGR2, Rainbow, Crimson, SC2, and one broader-
sweep title produces ≤ 1 % per-pixel difference on combiner-correct
surfaces. `metal-renderer-plan.md` §5 documents combiner-edge
tolerances; intentional small diffs against those tolerances are
recorded in the per-title benchmark note for that run, not silently
absorbed.

**Key tools.** `metal-gl-compare.sh` (W2), `metal-canary-regress.sh`
(W3 — the green canary set runs as a regression gate after every
Metal change), Visual Flight Recorder (storyboard + black-frame
quantification on each captured route).

### Phase 3 — Performance parity & polish

**Dominant question.** Does Metal hit the perf and pacing bar that
justifies a default-on flip?

**Current state.** Partially measured. PGR2 Metal post-load
`avg_fps=42.12` at MSAA4 (`benchmark-runs/20260504-100458-pgr2`);
GL baseline at the same `XEMU_GL_MSAA=4` + `surface_scale=2` settings
delivers ~47 fps on PGR2 per
`benchmarks/2026-05-03-multi-title-msaa-1080p-validation.md`. Metal's
texture-bind CPU time was identified as a bottleneck (see
`benchmarks/2026-05-04-metal-pgr2-texture-bind-attribution.md`); an
early cached-texture bind path landed but a paired Metal-vs-GL p99
mspf-jitter measurement has not been run yet.

**Entry criteria.** Phase 2 exit gate met on the same five-title set.

**Exit gate.** Metal hits ≥ console-native FPS for every tracked
title (30 Hz for PGR2/Rainbow/Crimson, 60 Hz for SC2) on the same
scripted route used for GL. p99 mspf jitter on PGR2 + Rainbow +
Crimson improves by ≥ 20 % vs the GL baseline. Cold-launch shader
compile total < 5 s on a fresh `metal_shaders/` cache directory.

**Key tools.** `XEMU_METAL_CAPTURE` (M13 frame capture +
`MTLCounterSampleBuffer` per-stage GPU timing — `METAL_VERTEX_US_TOTAL`
/ `METAL_FRAGMENT_US_TOTAL` / `METAL_PRESENT_GPU_US_TOTAL`),
`extract-perf-summary.sh` (per-interval `METAL_*` counter rollup),
`XEMU_METAL_PIPELINE_CACHE` (M9 — toggle warm-vs-cold paired
benchmarks), Apple Instruments "Metal System Trace" (project rule #8;
attached under `benchmark-runs/<run>/captures/`).

### Phase 4 — Default-on / shipping

**Dominant question.** Is Metal correct-and-fast enough to flip the
default-renderer selector, and does the pre-warmed pipeline cache
ship cleanly?

**Current state.** BLOCKED. M15 is gated on Phase 1, 2, and 3 exit
gates plus a documented front-fb fallback policy (today PGR2 needs
`XEMU_METAL_FRONT_FB_FALLBACK=1`; either the CRTC-strict path is
made faithful, or the fallback is documented as the accepted
default).

**Entry criteria.** Phase 3 exit gate met. No correctness-affecting
Metal bug open ≥ 30 days.

**Exit gate.** `display.renderer = METAL` is the Apple Silicon
first-launch default. A pre-warmed `metal_shaders/` cache ships
inside `dist/xemu.app/Contents/Resources/` (or a documented
download-on-first-launch mechanism) so cold-launch shader compile
cost is ≤ 1 s for the tracked-title corpus. The OpenGL renderer
remains selectable as the correctness oracle. A `decision-log.md`
entry records the M15 default-on flip with the validation artifacts.

**Key tools.** `metal-renderer-plan.md` §4 M15 (the canonical
acceptance criteria), `handoff.md` (the state pointer that tracks
"30 days of stable continuous bench use"), the cross-title perf
suite (Phase 3 tooling).

---

## 3. Daily loop for the active phase (Phase 1)

This is the literal command-by-command sequence a session should
follow when working on Metal correctness. Adapt the canary name and
the screenshot interval to the symptom being investigated; the shape
of the loop stays fixed.

### 3.1 Validation-on build

Always work against a build with `XEMU_METAL_VALIDATION=1` set so
Metal's API-validation layer is active. W1 promotes this to auto-on
for `XEMU_RENDERER=METAL` runs through `run-benchmark.sh`; until the
W1 slice merges, set the env var explicitly:

```sh
./build.sh -a arm64
codesign --verify --deep --strict --verbose=2 dist/xemu.app
```

```sh
XEMU_METAL_VALIDATION=1 \
XEMU_METAL_SHADER_VALIDATE=1 \
scripts/apple-silicon/metal-shader-validation/run-validation.sh
```

The shader-validation runner exits 0 only when all fixtures pass.
Treat any non-zero exit as a Phase 1 regression and bisect before
running gameplay routes.

### 3.2 Paired diff for the suspected failing canary

Use `metal-gl-compare.sh` (W2 — introduced 2026-05-04) when
investigating a specific title:

```sh
scripts/apple-silicon/metal-gl-compare.sh \
  --title pgr2 \
  --route scripts/apple-silicon/input-scripts/pgr2-gameplay.csv \
  --duration 90 \
  --interval 30 \
  --tolerance 0.01
```

The harness runs the same scripted route under GL and Metal at
matched screenshot intervals, runs Visual Flight Recorder over each
output sequence, and writes a side-by-side diff under
`benchmark-runs/<timestamp>-<title>-metal-gl-compare/`. Per-pixel
diff above the tolerance becomes the failing-frame index that
Section 3.3's per-draw RT dump targets.

When `metal-gl-compare.sh` is not yet available in your branch, the
fallback is two manually-paired `run-benchmark.sh` invocations with
`XEMU_BENCH_VISUAL_ANALYSIS=1` and matching
`XEMU_METAL_SCREENSHOT_INTERVAL` / `XEMU_BENCH_SCREENSHOT_INTERVAL`
values, then `python3 scripts/apple-silicon/visual-flight-recorder.py
--run-dir <run>` over each.

### 3.3 Per-draw RT dump for triage

When a paired diff localizes a failing frame, the next question is
"which NV2A draw inside that frame is the first divergence". The
W4 slice (introduced 2026-05-04) lands `XEMU_METAL_DUMP_DRAW_RT` and
`XEMU_GL_DUMP_DRAW_RT` env vars that snapshot the bound color render
target to a PNG after each draw across a configurable range:

```sh
XEMU_RENDERER=METAL \
XEMU_METAL_TRANSLATED_PIPELINE=1 \
XEMU_METAL_VALIDATION=1 \
XEMU_METAL_DUMP_DRAW_RT=900:950:/tmp/pgr2-metal-d \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 60
```

```sh
XEMU_GL_DUMP_DRAW_RT=900:950:/tmp/pgr2-gl-d \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 60
```

Compare the per-draw PNG sequences with `compare` from ImageMagick or
the existing diff utilities in
`scripts/apple-silicon/native-tri-depth-compare.sh`. The first
draw-index where the two sequences diverge is the failing draw — go
read its NV2A method dispatch in `xemu.log` and the corresponding
state in the Metal pipeline-key builder
(`hw/xbox/nv2a/pgraph/mtl/state.c`).

The format `START:END:PREFIX` writes
`<PREFIX>.draw<NNNN>.<frame>.png` for draws indexed `[START, END)`.
Keep the range tight to bound disk usage — a single Metal frame can
issue 100+ draws at PGR2 gameplay rates.

### 3.4 MoltenVK triangulation when stuck

If Metal-vs-GL diverges and the root cause is ambiguous (it could be
either path's bug), a third backend triangulates. W5 (introduced
2026-05-04) wires `XEMU_RENDERER=VULKAN` on Apple Silicon via
MoltenVK so the same scripted route runs through xemu's existing
Vulkan renderer on top of MoltenVK's Vulkan-on-Metal translation:

```sh
XEMU_RENDERER=VULKAN \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 60
```

If the MoltenVK output matches GL but Metal does not, the bug is on
xemu's Metal-renderer side (most common). If the MoltenVK output
matches Metal but neither matches GL, the bug is in xemu's Vulkan
renderer or in MoltenVK's translation. If MoltenVK matches Metal and
GL agrees with both at the failing frame index, the failing frame
is mis-aligned between runs — go re-run with deterministic input
timing.

**HEDGE: W5 may be BLOCKED.** xemu's Vulkan renderer relies on
geometry-shader features that MoltenVK does not implement on Apple
Silicon (specifically `VK_EXT_geometry_shader` is unsupported on
MoltenVK 1.3.x for Apple7+). If `XEMU_RENDERER=VULKAN` aborts at
init or hits a translator failure on every frame, the W5 slice will
land in BLOCKED state — see `handoff.md` for the current W5 status
and the fallback path below.

When W5 is BLOCKED, fall back to:

1. **Per-draw RT dump (Section 3.3)** to localize the failing draw
   from Metal-vs-GL alone. The first divergent draw is usually
   identifiable without a third oracle.
2. **Xcode `.gputrace` capture** of the failing Metal frame:

   ```sh
   XEMU_METAL_CAPTURE=/tmp/pgr2-fail.gputrace \
   XEMU_METAL_CAPTURE_FRAMES=5 \
   scripts/apple-silicon/run-benchmark.sh pgr2 \
     scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 30
   ```

   Open in Xcode (Window → Organizer → GPU Frame Capture), navigate
   to the failing draw index identified in Section 3.3, inspect
   bound resources and shader inputs.
3. **Read the GL renderer's behavior** at the same draw index in
   `hw/xbox/nv2a/pgraph/gl/draw.c` and the corresponding Metal
   path in `hw/xbox/nv2a/pgraph/mtl/draw.mm`. The most common
   Phase 1 bug class is a missing per-state branch on the Metal
   side that GL handles correctly.

### 3.5 Update docs and re-run canaries

Phase 1 sessions that change Metal renderer code must:

1. After every Metal renderer change, run
   `scripts/apple-silicon/metal-canary-regress.sh` as the post-change
   smoke. Expected outcome: all four canaries (PGR2 / Rainbow / Halo /
   boot) PASS at the default 1 % per-pixel-changed threshold. This is
   the post-change smoke tool the W3 slice (introduced 2026-05-04)
   ships:

   ```sh
   scripts/apple-silicon/metal-canary-regress.sh
   ```

   This runs PGR2 / Rainbow / Halo / boot through Metal at MSAA4
   under the established green-canary env recipe (verbatim from the
   `handoff.md` "PGR2 PASS" bullet) and diffs each captured screenshot
   against the recorded gold PNG under `benchmark-runs/visual-checks/`
   via `compare-screenshots.py`. Per project rule #11 the script
   ASSERTS the closed default-on Apple Silicon flags still produce
   the gold PNG; it does NOT re-validate them. Non-zero exit blocks
   the commit. Exit-code semantics match `metal-gl-compare.sh`: 0 PASS,
   1 FAIL on the visual diff, 2 INFRA-FAIL.

   For a focused re-run after a localized change, use
   `--canary <name>` (`pgr2` | `rainbow` | `halo` | `boot`).
   See `automation.md` "Canary regression gate (W3, 2026-05-04)" for
   the embedded canary table and per-canary frame ordinals.

2. Update `handoff.md` if the canary state changed, append a
   benchmark note under `docs/apple-silicon/benchmarks/<date>-*.md`,
   and run `/sync-docs` before ending the session
   (project rule #4).

3. Run `/codex-validate changes` per project rule #15 if the
   uncommitted Metal-side diff exceeds 30 lines.

---

## 4. Tools index

This section catalogues every Metal-relevant flag, script, and counter
the workflow consumes, mapped to the phase that uses it. Stable
opt-in flags are reproduced verbatim from `xemu-fork/CLAUDE.md` "Stable
opt-in"; new tools introduced 2026-05-04 are flagged as such with a
forward reference to `handoff.md` for current implementation status.

### 4.1 Renderer-selection flags

- `XEMU_RENDERER={GL,METAL,VULKAN}` (M5 — environment-variable bridge
  for the `display.renderer` config setting; phases 1, 2, 3, 4).
  `VULKAN` on Apple Silicon goes through MoltenVK once W5 lands.

### 4.2 Translation-correctness flags (Phase 1)

- `XEMU_METAL_VALIDATION={0,1}` (M14, 2026-05-02; phase 1) — opt-in
  Metal API validation layer; promotes `MTL_DEBUG_LAYER=1` before the
  first `MTLCreateSystemDefaultDevice()` call. Default 0. **W1
  (2026-05-04) auto-promotes this to 1** when `run-benchmark.sh`
  detects `XEMU_RENDERER=METAL` and the user has not pinned
  `XEMU_METAL_VALIDATION` themselves; opt out with the new
  `--metal-no-validate` flag.
- `XEMU_METAL_HUD={0,1}` (W1, **introduced 2026-05-04**; phase 1) —
  promotes `MTL_HUD_ENABLED=1` for Apple's Metal Performance HUD
  overlay. Auto-on for benchmark-harness Metal runs unless the
  `--metal-no-hud` flag is passed. See `handoff.md` for current
  implementation status.
- `XEMU_METAL_SHADER_VALIDATE={0,1,strict,2}` (M5, 2026-05-02;
  phase 1) — runs the in-process M5 shader-translation harness at
  `xemu_metal_init`. Counters `METAL_SHADER_VALIDATE_OK` /
  `METAL_SHADER_VALIDATE_FAIL` / `METAL_GLSL_TRANSLATE` /
  `METAL_GLSL_TRANSLATE_FAIL`.
- `XEMU_METAL_SHADER_VALIDATE_AND_EXIT={0,1}` (M5, 2026-05-02;
  phase 1) — CI-runner companion that exits with status 0/1 after
  the harness reports.
- `XEMU_METAL_TRANSLATED_PIPELINE={0,1}` (M7.1, 2026-05-02; phase 1)
  — flips the production draw path from M3/M4 hand-coded passthrough
  to the spirv-cross-built MTLRenderPipelineState path. Default 0;
  most Metal correctness work runs with this set to 1.
- `XEMU_METAL_FORCE_PASSTHROUGH={0,1}` (M7, 2026-05-02; phase 1) —
  bisection knob that forces every draw onto the M3/M4 hand-coded
  passthrough pipeline.
- `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH={0,1}` (M7, 2026-05-02;
  phase 1) — forces `pgraph_mtl_heap_supports_framebuffer_fetch()`
  to return false for fallback exercise.
- `XEMU_METAL_FRONT_FB_FALLBACK={0,1}` (M5.10 experimental,
  2026-05-03; phase 1) — opt-in fallback that publishes the
  selected color render target as the front-fb after the CRTC
  publish. Required for PGR2 today; NOT correctness-faithful for
  titles that legitimately use both surfaces.
- `XEMU_METAL_FRONT_FB_DOWNLOAD={0,1}` (M5.10, 2026-05-03; phase 1
  / 2) — opt-in GPU→VRAM surface-download path mirroring
  `vk/surface.c::pgraph_vk_surface_download_if_dirty`. Default 0
  (off) due to historical cold-boot perf cost.
- `XEMU_METAL_DISABLE_SURFACE_TEX_ADDRS={0,1}` (2026-05-04
  diagnostic; phase 1) — disables direct render-target-as-texture
  lookup by VRAM address; forces the CPU texture path. Useful for
  isolating surface-texture aliasing and channel/alpha
  normalization issues.
- `XEMU_METAL_DUMP_DRAW_RT=START:END:PREFIX` (W4, **introduced
  2026-05-04**; phase 1) — per-draw color RT snapshot for the
  Metal renderer. Writes `<PREFIX>.draw<NNNN>.<frame>.png` for
  draws in the half-open index range. See `handoff.md` for the
  current implementation status.
- `XEMU_GL_DUMP_DRAW_RT=START:END:PREFIX` (W4, **introduced
  2026-05-04**; phase 1) — same per-draw color RT snapshot on the
  GL renderer; intended for paired use with `XEMU_METAL_DUMP_DRAW_RT`
  to do first-divergent-draw isolation.

### 4.3 Diagnostic / capture flags (Phase 1, 2, 3)

- `XEMU_METAL_DIAG_CLEAR={0,1}` (2026-05-03; phase 1) — diagnostic
  logger for `pgraph_mtl_surface_clear`. Emits up to 32 one-line
  `xemu-perf: metal_surface_clear vram_addr=0x.. rgba=(R,G,B,A)
  write_zeta=N` records.
- `XEMU_METAL_SCREENSHOT_PATH=/path/to/file.png` (2026-05-03;
  phase 1, 2) — programmatic PNG screenshot of the final composited
  drawable, encoded inside the Metal renderer (no `screencapture`,
  no Screen-Recording dialog).
- `XEMU_METAL_SCREENSHOT_AT_FRAME=N` (2026-05-03; phase 1, 2) —
  frame number to fire `XEMU_METAL_SCREENSHOT_PATH` at.
- `XEMU_METAL_SCREENSHOT_INTERVAL=N` (2026-05-03; phase 1, 2) —
  repeat interval; writes `<base>.0001.png`, `<base>.0002.png`, …
- `XEMU_METAL_SCREENSHOT_SOURCE={drawable,nv2a,vram:0xADDR}`
  (2026-05-03; phase 1) — selects which texture the screenshot
  path captures.
- `XEMU_METAL_CAPTURE=path.gputrace` (M13, 2026-05-02; phase 1, 3)
  — programmatic `MTLCaptureManager` frame capture.
- `XEMU_METAL_CAPTURE_FRAMES=N` (M13, 2026-05-02; phase 1, 3) —
  frame-count bound for `XEMU_METAL_CAPTURE`. Default 60.

### 4.4 Performance flags (Phase 3)

- `XEMU_METAL_PIPELINE_CACHE={0,1}` (M9, 2026-05-02; phase 3) —
  persistent MSL-source disk cache. Default ON on Apple Silicon.
- `XEMU_METAL_ASYNC_PIPELINE_COMPILE={0,1}` (M8, 2026-05-02;
  phase 3) — async pipeline compile worker. Default ON on Apple
  Silicon.
- `XEMU_METAL_FORCE_LEGACY_PRESENT={0,1}` (M10, 2026-05-02;
  phase 3) — forces `presentDrawable:` only (no `atTime:`).
- `XEMU_METAL_MSAA={0,2,4,8}` (M11, 2026-05-02; phase 1, 2, 3) —
  Metal-side MSAA. Default 0; canaries currently run with 4.
- `XEMU_METAL_FX_SCALE={1,2,3}` (M12, 2026-05-02; phase 3) —
  `MTLFXSpatialScaler` upscale path.
- `XEMU_GL_RATE_SLEW={0,1}` / `XEMU_RATE_SLEW={0,1}` (M10
  prerequisite, 2026-05-02; phase 3) — emulation-rate slewing on
  the host display refresh rate.

### 4.5 Composition flags (used at all phases)

- `XEMU_DISPLAY_SCALE={1,2,3,4}` — overrides the loaded
  `display.quality.surface_scale`. Default 2 on Apple Silicon
  first-launch.
- `XEMU_GL_MSAA={0,2,4,8}` — GL-side MSAA, used as the GL baseline
  for paired diffs.
- `XEMU_NATIVE_TRI_DEPTH=1`, `XEMU_NATIVE_QUAD=1`,
  `XEMU_PGRAPH_FAST_READ=1` — three of the eight default-on Apple
  Silicon flags. Always on; toggling them is a regression check, not
  a Metal-investigation tool.

### 4.6 Scripts

- `scripts/apple-silicon/run-benchmark.sh` — primary launcher. New
  flags 2026-05-04 (W1): `--metal-no-validate` (opt-out of W1
  auto-on `XEMU_METAL_VALIDATION`) and `--metal-no-hud` (opt-out
  of W1 auto-on `XEMU_METAL_HUD`). Existing Metal-related flags:
  `--metal-capture <path>`, `--metal-screenshot <path>`,
  `--metal-screenshot-at-frame <N>`.
- `scripts/apple-silicon/metal-shader-validation/run-validation.sh`
  (M5, 2026-05-02) — runs the in-process shader-translation harness
  and exits 0/1.
- `scripts/apple-silicon/metal-gl-compare.sh` (W2, **introduced
  2026-05-04**; phases 1, 2) — paired Metal-vs-GL diff harness
  modelled on `native-tri-depth-compare.sh`. See `handoff.md` for
  current implementation status.
- `scripts/apple-silicon/metal-canary-regress.sh` (W3, **introduced
  2026-05-04**; phases 1, 2) — runs the green canary set
  (PGR2/Rainbow/Halo/boot) and gates on per-pixel diff against the
  recorded `benchmark-runs/visual-checks/` baselines. Depends on
  W2's diff harness. See `handoff.md` for current implementation
  status.
- `scripts/apple-silicon/visual-flight-recorder.py` (2026-05-04) —
  bounded visual timeline analyzer for PNG sequences and short
  videos. Produces `visual-summary.json`, `timeline.csv`,
  `storyboard.jpg`, and selected `keyframes/`.
- `scripts/apple-silicon/extract-perf-summary.sh` — per-interval
  `xemu-perf:` rollup. Surfaces 50 `METAL_*` counters as of M14
  plus the 2 graphics-API-agnostic `RATE_SLEW_*` counters.
- `scripts/apple-silicon/native-tri-depth-compare.sh` — paired
  baseline/native snapshot comparison with screenshot diffing.
  Pattern that `metal-gl-compare.sh` (W2) follows.
- `scripts/apple-silicon/validate-native-tri-depth.sh --run 22` —
  Phase 0 sanity gate; tests the GL path and proves the eight
  default-on flags still behave correctly.

### 4.7 Counters

The full set of 50 `METAL_*` counters is documented in
`xemu-fork/CLAUDE.md` "Stable opt-in" and `automation.md`. The
phase-relevant subset:

- **Phase 1 (correctness floors):**
  `METAL_PIPELINE_TRANSLATED_FAILED` (must == 0),
  `METAL_DRAWS_SKIPPED_PENDING_TOTAL` (must == 0 once shader cache
  is warm), `METAL_PIPELINE_FALLBACKS` (must == 0 once translated
  pipeline is the production path),
  `METAL_SHADER_VALIDATE_OK` / `_FAIL`,
  `METAL_GLSL_TRANSLATE` / `_FAIL`,
  `METAL_DRAW_TRANSLATED == METAL_DRAW_COUNT` (100 % translated),
  `METAL_SURFACE_RECREATE_SHAPE_MISMATCH` (must == 0).
- **Phase 2 (visual parity, surface routing):**
  `METAL_FRONT_FB_PUBLISHES`, `METAL_SURFACE_VRAM_DIRTY_HITS`,
  `METAL_SURFACE_VRAM_UPLOADS`, `METAL_SURFACE_VRAM_UPLOAD_BYTES`,
  `METAL_SURFACE_DOWNLOADS`, `METAL_SURFACE_DOWNLOAD_BYTES`,
  `METAL_IMAGE_BLITS`, `METAL_MSAA_RESOLVE_COUNT`.
- **Phase 3 (perf parity):**
  `METAL_VERTEX_US_TOTAL`, `METAL_FRAGMENT_US_TOTAL`,
  `METAL_PRESENT_GPU_US_TOTAL` / `_FRAMES`,
  `METAL_FX_SPATIAL_GPU_US_TOTAL`,
  `METAL_TEX_BIND_US_TOTAL`, `METAL_DRAW_ENCODE_US_TOTAL`,
  `METAL_PRESENT_JITTER_US_TOTAL` / `_AVG` / `_MAX`,
  `METAL_DRAWABLE_ACQUIRE_FAILS`,
  `METAL_SHADER_COMPILE_QUEUED_TOTAL` / `_COMPLETED_TOTAL` /
  `_FAILED_TOTAL`,
  `METAL_SHADER_CACHE_LOADS` / `_HITS` / `_MISSES`.
- **Phase 4 (default-on monitoring):** the full set; the M15 gate
  uses `extract-perf-summary.sh` rollups across the
  five-title corpus.

---

## 5. Triage flowchart

This section maps observed symptoms to the next diagnostic and the
next tool. The flow is "narrow before measuring" — do not capture a
`.gputrace` for a build that doesn't even launch.

### 5.1 Metal renderer crashes / Metal validation API error

**Symptom.** xemu aborts with a Metal validation assertion (e.g.
`Drawable presented multiple times in same command buffer`,
`MTLCommandBuffer status was committed and not yet completed`,
`Texture descriptor pixelFormat`-mismatch), or `xemu.log` contains
`MTL_DEBUG_LAYER` complaint lines.

**First diagnostic.** Confirm `XEMU_METAL_VALIDATION=1` is the source
(check the startup banner `xemu-perf: metal_validation requested=R
promoted=P mtl_debug_layer_active=A` — A should be 1).

**Next tool.** Reproduce with `XEMU_METAL_CAPTURE=/tmp/crash.gputrace
XEMU_METAL_CAPTURE_FRAMES=5` so the failing frame's command buffer
state is captured. Open in Xcode (Window → Organizer → GPU Frame
Capture). The validation error message identifies the offending
encoder + draw index; navigate there in the trace to inspect bound
resources.

**If the crash is at startup.** The shader-translation harness is the
right next step: `XEMU_METAL_SHADER_VALIDATE=strict` aborts at the
first failing fixture, isolating the regression to a single shader
class.

### 5.2 Visual difference vs GL (Crimson/SC2-class black-frame)

**Symptom.** Captured Metal screenshots are black or show one
patterned frame followed by black. Counters look clean
(`METAL_PIPELINE_TRANSLATED_FAILED=0`, draws are happening) but the
displayed frame has no scene content.

**First diagnostic.** Run Visual Flight Recorder over the captured
sequence:

```sh
scripts/apple-silicon/visual-flight-recorder.py \
  --run-dir benchmark-runs/<run>
```

Read `visual-analysis/visual-summary.json` to confirm the frames are
genuinely black (not a capture-side artifact). For PGR2-class
"renders to back buffer that never reaches CRTC" symptoms,
`benchmark-runs/<run>/xemu.log` will contain
`metal_front_fb_publish vram_addr=0x.. reason={crtc,clear,
fallback-latest-draw}` lines that identify which surface is being
published.

**Next tool.** Toggle `XEMU_METAL_FRONT_FB_FALLBACK=1` if not already
set. If the fallback fixes the frame, the CRTC-strict publish path
is missing the back→front bridge for that title; file under
`metal-renderer-plan.md` §4 M5.10 follow-up. If the fallback does
not fix it, run the per-draw RT dump (Section 3.3) to see whether
draws are reaching the bound color RT at all, or whether they are
being routed to a different VRAM address.

The `XEMU_METAL_SCREENSHOT_SOURCE=vram:0xADDR` mode captures any
specific cached `MtlSurfaceBinding` by VRAM address, useful when the
diagnostic counters identify a surface but the published front-fb
does not reflect it.

### 5.3 Performance regression vs previous Metal run

**Symptom.** `post_load_avg_fps` drops from a recorded baseline
without an obvious correctness change.

**First diagnostic.** Compare per-interval counters from
`extract-perf-summary.sh` against the baseline run. Look for: (a)
spikes in `METAL_TEX_BIND_US_TOTAL` (texture-bind CPU bottleneck —
historical PGR2 regression class); (b) `METAL_PIPELINE_FALLBACKS` >
0 (translated-pipeline path missing a state branch); (c) sustained
`METAL_DRAWS_SKIPPED_PENDING_TOTAL` > 0 (async pipeline compile
backlog — usually a cold-cache symptom).

**Next tool.** `XEMU_METAL_CAPTURE` for a representative slow frame,
plus an Apple Instruments "Metal System Trace" attached to the
benchmark note (project rule #8). For shader-cache vs cold-launch
attribution, run paired `XEMU_METAL_PIPELINE_CACHE=1` vs `=0`
benchmarks.

### 5.4 Shader translation failure / spirv-cross error

**Symptom.** `METAL_GLSL_TRANSLATE_FAIL > 0` or
`METAL_PIPELINE_TRANSLATED_FAILED > 0` in the perf summary;
`xemu.log` contains spirv-cross or `[device newLibraryWithSource:]`
diagnostic lines.

**First diagnostic.** Run the M5 shader-translation harness:

```sh
scripts/apple-silicon/metal-shader-validation/run-validation.sh
```

Non-zero exit identifies the failing fixture class. If the harness
passes but live runs still fail, the failing shader is a real-game
shader the harness does not exercise — capture the failing
`ShaderState` via `XEMU_METAL_DUMP_SHADER_STATE` (planned diagnostic
flag — file a follow-up if absent) and add a fixture mirroring it.

**Next tool.** spirv-cross has a small CLI (`spirv-cross`) that takes
a SPIR-V binary on disk and emits MSL; running it on the captured
SPIR-V in isolation prevents misattribution between glslang's
GLSL→SPIR-V step and spirv-cross's SPIR-V→MSL step. The patch
decision per `metal-renderer-plan.md` §3.3 is: patch the GLSL
generator to emit a SPIR-V-friendly form, or hand-write the MSL for
that one variant.

### 5.5 First-divergent-draw isolation between Metal and GL

**Symptom.** Both Metal and GL produce visible frames; the visible
frames disagree at some draw inside the frame.

**First diagnostic.** Run Section 3.3's per-draw RT dump on both
backends with matching index ranges. Step through the resulting PNG
pairs in lockstep until the first divergence.

**Next tool.** Once the failing draw is identified, capture an
Xcode `.gputrace` of the failing Metal frame and locate that draw.
Inspect the bound vertex/fragment buffers, samplers, render-target
formats, and the compiled MSL. Cross-check against the
`hw/xbox/nv2a/pgraph/gl/draw.c` path for the same NV2A method
sequence. The most common bug class is a missing per-state branch
on the Metal side that GL handles correctly.

If the per-draw RT dump shows the divergence is multi-frame (e.g.
Metal "remembers" stale state across frames), check the M5.7
open-pass coalescing logic and the M11 MSAA store/resolve policy
— both have historically produced multi-frame diffs.

### 5.6 Surface cache / front-fb fallback question

**Symptom.** Specific titles need `XEMU_METAL_FRONT_FB_FALLBACK=1`
or `XEMU_METAL_FRONT_FB_DOWNLOAD=1` to render correctly; others
break under the same flag.

**First diagnostic.** Read `xemu.log` for the
`metal_front_fb_publish` lines and the `metal_draw_target` per-vram
counter (introduced in M5.9-followup-E). The pair tells you which
VRAM address is being published as the front-fb and which VRAM
address is receiving the bulk of the draws. If they disagree
(PGR2's classic 0x32a4000 vs 0x3628000 split), the title is using
a back→front mechanism the Metal renderer does not yet bridge.

**Next tool.** The M5.10 surface-download path
(`XEMU_METAL_FRONT_FB_DOWNLOAD=1`) is the correctness-faithful
solution; it is default-off due to historical cold-boot perf cost.
For titles that work under fallback but not under download, the
mechanism is likely a hardware blit class the Metal renderer's
`pgraph_mtl_image_blit` stub does not implement (NV3089 scaled-blit
or NV0039 M2MF are candidates, but PGR2 specifically does not use
either per `METAL_IMAGE_BLITS=0`).

When neither flag fixes the title, the fix lives in the surface
manager (`hw/xbox/nv2a/pgraph/mtl/surface.{h,mm}`) — the M5.10 plan
in `metal-renderer-plan.md` Path A vs Path B is the canonical
sequencing choice.

---

## 6. Phase exit-gate procedures

Each phase exit is a concrete, runnable procedure. Do not declare a
phase done by inspection; run the procedure and attach the artifacts
to a benchmark note.

### 6.1 Phase 0 → Phase 1 transition

**Procedure.**

```sh
./build.sh -a arm64
codesign --verify --deep --strict --verbose=2 dist/xemu.app
dist/xemu.app/Contents/MacOS/xemu --version
scripts/apple-silicon/validate-native-tri-depth.sh --run 22
scripts/apple-silicon/metal-shader-validation/run-validation.sh
```

**Acceptance.** All four commands exit zero. The `--version` output
identifies the build. `validate-native-tri-depth.sh` reports 7/7
PASS lines. The shader-validation runner reports a numeric pass
count matching the fixture count (7/7 today).

**Artifact.** Append a benchmark note under
`docs/apple-silicon/benchmarks/<date>-phase0-exit.md` with the
command outputs and the build commit hash.

### 6.2 Phase 1 → Phase 2 transition

**Procedure.**

```sh
scripts/apple-silicon/metal-canary-regress.sh
```

(Until W3 lands, run each canary by hand:)

```sh
for title in pgr2 rainbow halo crimson; do
  XEMU_RENDERER=METAL \
  XEMU_METAL_TRANSLATED_PIPELINE=1 \
  XEMU_METAL_VALIDATION=1 \
  XEMU_METAL_FRONT_FB_FALLBACK=1 \
  XEMU_METAL_MSAA=4 \
  XEMU_BENCH_VISUAL_ANALYSIS=1 \
  scripts/apple-silicon/run-benchmark.sh "$title" 90
done
```

Then for each of PGR2 / Rainbow / Crimson / SC2 / one broader-sweep
title, run `metal-gl-compare.sh` (W2) at the gameplay route used
for the GL baseline.

**Acceptance.** Every title produces a non-black, route-correct
visual capture. `METAL_PIPELINE_TRANSLATED_FAILED == 0` and
`METAL_DRAWS_SKIPPED_PENDING_TOTAL == 0` across the full route.
No MTLValidation API errors fired (confirmed by `xemu.log` grep).
The Visual Flight Recorder summary for each route reports
`black_frame_pct` below the per-title threshold documented in the
canary baseline (PGR2/Rainbow/Halo/boot < 5 %; Crimson and SC2
require their threshold to be set in the same session that closes
their visual route).

**Artifact.** A dated benchmark note pointing at each of the five
runs and recording the per-title acceptance state.

### 6.3 Phase 2 → Phase 3 transition

**Procedure.** For each of the five titles, run `metal-gl-compare.sh`
at the matched-interval setting; record the per-pixel diff
percentage on combiner-correct surfaces.

**Acceptance.** ≤ 1 % per-pixel difference on combiner-correct
surfaces for each title. Combiner-edge tolerances per
`metal-renderer-plan.md` §5 are documented in the run's benchmark
note (not silently absorbed).

**Artifact.** A dated benchmark note with per-title diff
percentages, per-frame-bucket histograms, and the artifacts under
`benchmark-runs/<run>/visual-analysis/`.

### 6.4 Phase 3 → Phase 4 transition

**Procedure.** Paired Metal-vs-GL benchmarks for PGR2, Rainbow Six 3,
Crimson Skies, Soul Calibur 2, and one V4 broader-sweep title. Each
pair: same scripted-input route, same duration, same `surface_scale`,
matched `XEMU_GL_MSAA` / `XEMU_METAL_MSAA`. Capture
`extract-perf-summary.sh` rollups for each.

```sh
# Cold-launch shader compile timing (from a clean cache directory):
rm -rf "$HOME/Library/Application Support/xemu-project/xemu/metal_shaders/<game-id>"
time scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 30
```

**Acceptance.** Metal hits ≥ console-native FPS for every title
(30 Hz for PGR2/Rainbow/Crimson, 60 Hz for SC2). p99 mspf jitter on
PGR2 + Rainbow + Crimson improves by ≥ 20 % vs GL baseline.
Cold-launch shader compile total < 5 s on a fresh cache.

**Artifact.** A dated benchmark note with paired counter rollups,
the cold-launch compile timing, and the p99 mspf comparison.

### 6.5 Phase 4 (M15 default-on flip)

**Procedure.** Per `metal-renderer-plan.md` §4 M15: confirm Phase 0
+ 1 + 2 + 3 exit gates met across the same five-title set; confirm
no correctness-affecting Metal bug has been open ≥ 30 days; flip
`config_spec.yml`'s `display.renderer` first-launch default to
`METAL` on Apple Silicon; ship the pre-warmed `metal_shaders/`
cache.

**Acceptance.** The flip lands as a single commit on
`apple-silicon-performance`; the next clean-build first-launch
selects Metal automatically; the GL renderer remains selectable
through `display.renderer = OPENGL`. A `decision-log.md` entry
records the flip with a pointer to the validation artifacts.

**Artifact.** Decision-log entry plus a `handoff.md` update marking
"M15 SHIPPED".

---

## 7. Triangulation appendix

A "triangulation" exercise combines three independent oracles to
localize a hard correctness bug to a single NV2A command. The
oracles, in order of authority:

1. **GL renderer** — the project's correctness reference; behavior is
   well-trodden through retail-game testing on this fork.
2. **MoltenVK (`XEMU_RENDERER=VULKAN` on Apple Silicon)** —
   independent Vulkan-on-Metal translation. Disagreement between GL
   and MoltenVK localizes the bug to either xemu's Vulkan renderer
   (rare) or MoltenVK's translation (rare). Agreement between GL and
   MoltenVK is the strongest "Metal is the bug" signal.
3. **Metal renderer (`XEMU_RENDERER=METAL`)** — the path under test.

Combine the oracles with two diagnostic lenses:

- **Per-draw RT dump** (`XEMU_METAL_DUMP_DRAW_RT` and
  `XEMU_GL_DUMP_DRAW_RT`, W4) — narrows the failing frame to a
  failing draw index.
- **Xcode `.gputrace` capture** (`XEMU_METAL_CAPTURE`, M13) —
  inspect bound resources and shader inputs at the failing draw.

### 7.1 The full triangulation procedure

For a Crimson-class "Metal goes black after one frame" bug:

```sh
# Run the same scripted route through all three backends.
for renderer in GL VULKAN METAL; do
  XEMU_RENDERER=$renderer \
  XEMU_BENCH_VISUAL_ANALYSIS=1 \
  XEMU_BENCH_SCREENSHOT_INTERVAL=15 \
  scripts/apple-silicon/run-benchmark.sh crimson \
    scripts/apple-silicon/input-scripts/crimson-gameplay.csv 60
done
```

Compare the three Visual Flight Recorder summaries. The expected
matrix in a Phase 1 bug:

| GL    | MoltenVK | Metal | Diagnosis                               |
|-------|----------|-------|-----------------------------------------|
| green | green    | black | Metal renderer bug. Most common. Go to per-draw dump. |
| green | black    | black | xemu Vulkan renderer + Metal both buggy at same NV2A path; very unlikely. Or MoltenVK is dropping a feature both backends rely on. |
| green | black    | green | Vulkan-only or MoltenVK-only; ignore for Metal work. |
| black | green    | green | GL is the regression. Bisect GL.        |
| green | green    | green | Frame-alignment artifact in capture; re-run with deterministic input. |

If the matrix points at Metal:

```sh
# Find the failing-frame index from Visual Flight Recorder.
# Then dump per-draw color RT for the same range under GL and Metal.
XEMU_RENDERER=METAL \
XEMU_METAL_DUMP_DRAW_RT=120:180:/tmp/crimson-metal-d \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/crimson-gameplay.csv 30

XEMU_GL_DUMP_DRAW_RT=120:180:/tmp/crimson-gl-d \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/crimson-gameplay.csv 30
```

Step through the per-draw PNG pairs to find the first divergence.
Capture the failing frame in Xcode:

```sh
XEMU_RENDERER=METAL \
XEMU_METAL_CAPTURE=/tmp/crimson-fail.gputrace \
XEMU_METAL_CAPTURE_FRAMES=5 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/crimson-gameplay.csv 30
```

Open `/tmp/crimson-fail.gputrace` in Xcode (Window → Organizer → GPU
Frame Capture). Navigate to the failing draw index identified from
the per-draw dump. Inspect bound vertex / fragment buffers, samplers,
render-target formats, and the compiled MSL. Cross-check the same
NV2A method dispatch in `xemu.log` against the GL renderer's path in
`hw/xbox/nv2a/pgraph/gl/draw.c` for any state branch the Metal path
is missing.

### 7.2 Fallback when MoltenVK is blocked

xemu's Vulkan renderer relies on `VK_EXT_geometry_shader`, which is
not implemented on MoltenVK 1.3.x for Apple7+ devices. If the W5
slice lands in BLOCKED state (handoff.md will say so), the
triangulation reduces to:

1. **Per-draw RT dump under GL and Metal** (Section 3.3) for
   first-divergent-draw identification.
2. **Xcode `.gputrace`** of the failing Metal frame (Section 3.4).
3. **Read GL vs Metal renderer code at the failing NV2A method** to
   find the missing state branch.

This three-step sequence localizes the majority of Phase 1 bugs
without the third backend. The MoltenVK oracle's value is highest
when the diagnosis is ambiguous (neither GL nor Metal looks
obviously wrong); when one backend is clearly producing a
black/garbled frame, that frame is its own root-cause signal and
the triangulation is mostly redundant.

The W5 BLOCKED state does not block Phase 1 progress; it only
removes one corroborating tool.

---

## 8. Cross-references

- `handoff.md` — current Metal canary state, latest screenshots,
  current-session next-action priority. Always read first; this
  document points at the workflow, `handoff.md` points at the work.
- `metal-renderer-plan.md` — slice-level implementation plan
  (M0–M15), risk register, open questions. The "what to build";
  this document is the "how to operate".
- `decision-log.md` — append-only record of binding decisions and
  their supersession history. The 2026-05-04 entry "Adopt formal
  Metal porting workflow" introduces this document.
- `automation.md` — full reference for benchmark harness, scripted
  input, snapshot workflow, perf-counter list. The phase tools in
  Section 4 above are sourced from here.
- `emulator-metal-survey.md` — peer-emulator structural patterns
  (Dolphin, PCSX2, DuckStation, MoltenVK). Useful when a Phase 1
  bug looks like a known peer-emulator issue.
- `xemu-fork/CLAUDE.md` "Stable opt-in" — the authoritative flag
  list. Section 4 above mirrors but does not replace it.
- `xemu-fork/CLAUDE.md` "Working rules" — project rules #1
  (no guessing), #4 (no doc drift), #5 (build tools when blocked),
  #15 (Codex-validate triggers) directly bind the workflow above.
