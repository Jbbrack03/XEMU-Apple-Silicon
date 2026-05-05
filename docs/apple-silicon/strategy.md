# Strategy

Last updated: 2026-05-05 (Magenta investigation closed; PGR2 + Rainbow Six 3 + Halo CE menu + Xbox boot/flubber + Crimson Skies all PASS as Metal MSAA4 visual canaries with the canonical M15 recipe `XEMU_METAL_FRONT_FB_FALLBACK=1 + XEMU_METAL_TRANSLATED_PIPELINE=1 + XEMU_METAL_MSAA=4 + XEMU_NATIVE_TRI_DEPTH=1 + XEMU_NATIVE_QUAD=1 + XEMU_PGRAPH_FAST_READ=1`. Crimson reclassified 2026-05-05 — the previous "patterned frame followed by black drawable" symptom was caused by `metal-gl-compare.sh` not threading the canonical recipe to its Metal leg; Crimson now joins PGR2 as a documented "PASS only with `XEMU_METAL_FRONT_FB_FALLBACK=1`" title. **Phase 4 sub-deliverable 4j (MSAA) validated on the GL path** at `XEMU_GL_MSAA=4` + `surface_scale=2` across 5 titles (PGR2 47, Crimson 30, SC2 58, Halo 30, Rainbow 27); Metal MSAA4 also active on all five canaries. **GL renderer remains the production path for visual correctness today**; Metal renderer is feature-complete (M0–M14 shipped) and renders correctly on five canaries but M15 default-on stays BLOCKED on (a) F3 per-title snapshot anchor for paired diff (interactive recording needed), (b) SC2 routed visual canary (interactive input-script recording needed), (c) front-fb fallback default-on policy decision. Input slices N1+N2 also ship — `XEMU_MACOS_NATIVE_INPUT=1` GameController.framework backend with always-on input-latency counters.)

## North Star

Build an Apple Silicon renderer path that is fast, maintainable, and accurate
enough to avoid trading correctness for speed casually.

The likely final shape is:

- Apple Silicon default: Metal-native presentation and GPU work.
- Optional experiment path: Vulkan renderer through KosmicKrisp or MoltenVK.
- Legacy path: OpenGL retained only as a comparison/fallback while the fork is
  in transition.

## Architectural Principle

Do not build the Apple Silicon future around geometry shaders.

Reasoning:

- Apple's preferred graphics API is Metal.
- xemu's public macOS regression is linked to heavier geometry shader use.
- xemu's Vulkan renderer currently requires `geometryShader`.
- Portable Vulkan-over-Metal paths are risky when a renderer requires features
  that do not map naturally to Metal.

## Candidate Technical Approaches

### A. Vulkan-over-Metal first

Enable Vulkan on Darwin, wire platform surface creation, and test with:

- MoltenVK
- KosmicKrisp

Pros:

- Reuses the existing Vulkan renderer.
- Fastest way to produce comparative data.
- Gives us a bridge while designing native Metal.

Cons:

- Existing Vulkan renderer requires geometry shaders.
- Current Vulkan display code assumes Vulkan-to-OpenGL interop in places.
- MoltenVK/KosmicKrisp behavior must be proven with real xemu workloads.

Use this as a measurement/prototype track, not the assumed final design.

### B. Native Metal backend

Create a PGRAPH renderer that maps NV2A concepts directly to Metal:

- MTLDevice / MTLCommandQueue / CAMetalLayer presentation.
- MTLRenderPipelineState and MTLComputePipelineState cache.
- MTLBuffer-backed guest memory mirrors and staging.
- Metal shaders generated from the existing GLSL semantic model or a new
  intermediate shader representation.

Pros:

- Best chance at excellent Apple Silicon performance.
- Avoids OpenGL and Vulkan portability limitations.
- Gives direct access to Metal profiling and frame capture.

Cons:

- Larger implementation effort.
- Requires careful shader translation and validation.
- More fork-specific code.

This is the likely final long-term path.

### C. Geometry elimination layer

Move the work currently done by geometry shaders into an earlier stage:

- CPU-side primitive/index expansion for quads, quad strips, polygons, line
  loops, and line strips.
- CPU or compute generation of per-primitive constants needed for:
  - flat shading provoking-vertex behavior
  - depth interpolation
  - polygon offset slope
  - line-mode polygon rendering
- Fragment shader consumes per-primitive data rather than relying on a geometry
  shader to emit it.

Pros:

- Benefits OpenGL, Vulkan, and Metal.
- Directly targets the macOS regression.
- Makes Vulkan-over-Metal more feasible.

Cons:

- Must be validated against PR #2240 correctness goals.
- May increase CPU-side work or buffer bandwidth if designed poorly.

This should be treated as the first major renderer refactor.

## Phased Plan

### Phase 0: Baseline and Instrumentation

Deliverables:

- Native arm64 build. Done.
- Reproducible launch commands for local assets. Done in
  `docs/apple-silicon/benchmarks/2026-04-29-baseline.md`.
- Benchmark captures for Crimson Skies, Rainbow Six 3, and PGR2. Done for
  scripted smoke routes, snapshot scene-entry runs, and retail gameplay routes,
  using log-based FPS/frame pacing.
- At least one public regression title if available later.
- Frame pacing and FPS evidence. Done.
- CPU and GPU timing evidence. Pending; use Instruments or driver-level timing
  once the GL diagnostic target is narrower.

Current Phase 0 status:

- `./build.sh -a arm64` succeeds.
- `dist/xemu.app` launches far enough to report version and renderer details.
- Observed baseline renderer is Apple OpenGL-on-Metal:
  - GL vendor: `Apple`
  - GL renderer: `Apple M3 Ultra`
  - GL version: `4.1 Metal - 90.5`
- Snapshot-backed B2/B3 geometry-counter runs are complete.
- D1 showed triangle depth/slope arithmetic is not the likely bottleneck.
- D2/D3 showed avoiding GL geometry-shader dispatch is the main local
  performance lever.
- The current Phase 0 edge moved past flat-XBE counter validation. The
  generated `flat-tri-depth` XBE now reports first-provoking flat triangles on
  the native path and last-provoking flat triangles on the geometry-shader
  fallback path after adding a final perf flush for short diagnostic tails.
- `XEMU_NATIVE_TRI_DEPTH=1` is the completed current opt-in triangle-family
  fill replacement path. It is still not defaulted because broader retail
  coverage is needed.
- `XEMU_NATIVE_QUAD=1` is the completed current opt-in quad/quad-strip-family
  fill replacement path. It expands quads to triangles on the CPU and reuses
  the same `gl_FragCoord`-derived depth path; smooth fill only, flat shading
  and nonfill polygon modes still fall back to the geometry shader. It is
  also not defaulted yet.
- `XEMU_PGRAPH_FAST_READ=1` is the completed current opt-in lock-free
  PGRAPH register read fast path. It removes the bulk of TCG-thread
  mutex-wait time on `pg->lock` by skipping the mutex for atomic 32-bit
  register reads (`NV_PGRAPH_INTR`, `NV_PGRAPH_INTR_EN`, default
  `pg->regs_[]` slots). `NV_PGRAPH_RDI_DATA` keeps the lock because it has
  a side effect. Independent of the geometry-shader bypasses.
- Combined, the three flags eliminate every geometry-shader draw and
  remove the dominant TCG-thread mutex-wait stall in PGR2, Rainbow Six 3,
  and Crimson Skies, lifting all three tracked titles past the 30 FPS
  retail gameplay floor on the recorded routes.
- 2026-05-02: four additional flags landed default-on for Apple Silicon
  system builds (V1, V2, I5, plus the first-launch surface_scale=2
  default), and one opt-in renderer flag (`XEMU_GL_MSAA={2,4,8}`).
  Combined defaults at 1080p (`surface_scale=2`) plus `XEMU_GL_MSAA=4`
  pass the V4 broader-title sweep across 5 additional titles
  (`benchmarks/2026-05-02-broader-title-sweep.md`), with no MSAA-driven
  pipeline-variant explosion. The 30 FPS cap on PGR2 / Rainbow / Crimson
  is title-intrinsic (V4 SC2 sanity test sustains 60.57 FPS on the same
  build), so the active goal is no longer "60 FPS on those titles" but
  "console-native FPS with no 1-second-class judder + 1080p + AA across
  the broader Xbox library."
- Retail gameplay route baselines are now captured:
  - PGR2: 11.53 FPS average, 1,516,519 geometry-shader draws, including 38,785
    quad-family draws.
  - Rainbow Six 3: 24.19 FPS average, 692,438 geometry-shader draws, including
    1,946 line-family draws.
  - Crimson Skies: 15.44 FPS average, 786,722 geometry-shader draws, including
    7,837 quad-family draws.
- PGR2 mid-route snapshot triplet
  (`docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md`) showed
  the geometry-shader removal slices alone were not enough to reach 30
  FPS at that scene (16.56 FPS with both flags, zero geometry-shader
  draws). The follow-up sample profile
  (`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-sample.md`)
  identified ~32% of TCG-thread time was spent in mutex wait, dominated
  by `pgraph_read` contention. The lock-free read fast path
  (`docs/apple-silicon/benchmarks/2026-05-01-pgraph-fast-read.md`) lifted
  the same snapshot to 30.76 FPS and brought PGR2 retail gameplay route
  to 31.76 post-load FPS.
- `scripts/apple-silicon/validate-native-tri-depth.sh --run 20` is the
  regression gate for the triangle slice; do not use the next session to
  re-prove it unless the triangle path changes.

### Phase 1: Renderer Control Plane

Deliverables:

- Build-time and runtime renderer selection clarity.
- Explicit `opengl`, `vulkan-metal`, and future `metal` paths.
- Logging that records renderer, driver, GPU family, shader path, and major
  feature toggles at startup.

### Phase 2: Geometry Shader Exit

Deliverables:

- Completed slice: replace triangle-family fill geometry-shader dispatch
  with the opt-in native triangle-depth path (`XEMU_NATIVE_TRI_DEPTH=1`).
- Completed slice: replace quad/quad-strip-family smooth-fill
  geometry-shader dispatch with native CPU index expansion + the same
  `gl_FragCoord`-derived depth path (`XEMU_NATIVE_QUAD=1`).
- Next slice entry point is no longer a geometry-shader category. Combined
  with the triangle slice, the two flags eliminate every geometry-shader
  draw at the current snapshot scenes. Profile the PGR2 mid-route snapshot
  (16.56 FPS with both flags) under Instruments and the existing
  `XEMU_PERF_LOG=1` counters to identify the next dominant cost (likely
  candidates: i386 TCG, NV2A PGRAPH command processing, surface/texture
  upload bandwidth, fragment-shader work).
- Remaining geometry-shader categories worth eventually addressing if a
  benchmark exercises them non-trivially: flat-shaded quads, line/point
  polygon modes, line primitives, polygon fill, and nonfill triangle modes.
  None are non-trivially exercised by the current Crimson/Rainbow/PGR2
  routes.
- Replace geometry-shader primitive expansion with explicit index/vertex
  expansion where native GL rasterization cannot preserve NV2A behavior.
- Preserve flat shading and provoking-vertex behavior.
- Preserve or intentionally gate PR #2240 depth behavior.
- Benchmarks proving reduced shader compilation stalls and improved macOS 3D
  pacing.

### Phase 2.5: Frame Pacing & Async Shader Compile

> **Status (updated 2026-05-01): the async shader compile slice
> shipped as `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` opt-in but does NOT
> address the headline Crimson Skies 1.35-second worst-frame stutter.
> The GL-vs-Metal decision diagnostic
> (`benchmarks/2026-05-01-gl-vs-metal-decision.md`) proved the
> renderer is idle during those bad intervals — the bottleneck is the
> TCG vCPU thread (Apple Silicon-specific TB-invalidation cost). The
> frame pacing slewing piece below is still graphics-API-agnostic and
> still useful, but it cannot fix the headline judder either; PPTC
> (Phase 5a) is now the highest-priority slice.**

These two slices are independent of graphics-API choice and can land on
the current OpenGL path. Original framing was that they would address
the user-visible jitter sources documented in
`benchmarks/2026-05-01-baseline-jitter.md`. The 2026-05-01 diagnostic
session refined that picture:

- Crimson Skies' 1.35-second worst-frame is **NOT** Apple's GL
  synchronous shader compile. The renderer thread is idle during
  those intervals (5–23 ms of busy time per 1000 ms wallclock); the
  cost lives in TCG TB invalidation. See
  `benchmarks/2026-05-01-renderer-vs-tcg-stutter-attribution.md`.
- Frame-pacing slewing remains valuable for general smoothness on
  scenes that ARE renderer-paced (PGR2 / Rainbow tail jitter from
  emulation-clock drift relative to host vsync), but it does not
  unblock the headline goal.

Deliverables (status):

- **Async shader compile (shipped opt-in 2026-05-01).**
  `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`. Worker thread on a third
  shared GL context (`g_nv2a_context_shader_compile`) compiles shader
  programs off the renderer's critical path; "skip the draw" fallback
  (RPCS3 PR #4876 pattern). Counters
  `SHADER_COMPILE_ASYNC_QUEUED/COMPLETED` and
  `SHADER_DRAWS_SKIPPED_PENDING` confirm the worker drains correctly.
  Verdict: correct & shipped, does not fix Crimson stutter; remains
  off by default. See
  `benchmarks/2026-05-01-async-shader-compile.md`.
- Frame pacing — emulation-rate slewing. **Deferred until after the
  TCG fix lands.** Lock guest 60 Hz to host vsync via fractional clock
  adjustment of ≤ 1 %, mirroring DuckStation's "Sync to Host Refresh
  Rate" (PCSX2 PR #5488). Graphics-API-agnostic; measurable on the
  existing OpenGL build via the per-interval `mspf_max` jitter keys
  in `extract-perf-summary.sh`.

Both slices are research-informed; named source references are in
`research.md` "Apple Silicon Emulator Survey (2026-05-01)".

### Phase 3: Vulkan-over-Metal Prototype

> **Status (updated 2026-05-02): not the primary path.** The project
> is pivoting directly to native Metal rather than spending the next
> major implementation slice on Vulkan-over-Metal. Vulkan-over-Metal
> remains useful only as an optional comparison/prototype path because
> the existing Vulkan renderer requires features that do not map
> naturally to Metal and would still not give us full Metal-native
> presentation, capture, enhancement, and pipeline-control workflows.

Deliverables (deferred):

- Darwin Vulkan discovery and surface creation.
- MoltenVK/KosmicKrisp test matrix.
- Feature probe report at startup.
- Decision whether Vulkan-over-Metal is production-worthy or only a test path.

### Phase 4: Native Metal Renderer

> **Status (updated 2026-05-02): promoted to the primary renderer
> track.** The 2026-05-01 GL-vs-Metal diagnostic remains valid as a
> narrow measurement: Apple's OpenGL-on-Metal path was not proven to
> be the immediate FPS bottleneck in the measured routes. That verdict
> is superseded as a product-direction decision. A shareable Apple
> Silicon build needs Metal-native frame timing, input/rumble latency
> work, presentation control, capture/profiling, MSAA/resolve control,
> sharpening/upscaling experiments, pipeline caching, and a renderer
> architecture we are comfortable supporting. OpenGL remains the
> runnable reference backend and fallback; new renderer investment
> should target Metal.
>
> **Implementation sequencing (added 2026-05-02 after Metal planning
> session):** the sub-deliverables 4a–4i below are sequenced into 16
> staged, gated slices **M0–M15** in
> `metal-renderer-plan.md`. Each slice has explicit scope, entry
> criteria, exit criteria, and validation gate; the plan also adds
> architectural decisions (§3), validation methodology (§5), risk
> register R1–R8 (§6), and 6 open questions Q1–Q6 to resolve before
> slice M0 lands (§7). When working on Phase 4 implementation, follow
> the slice ordering in `metal-renderer-plan.md` rather than picking
> sub-deliverables ad-hoc from the list below.
>
> **Companion references (added 2026-05-02):**
> `metal-api-reference.md` (Apple Metal API surface),
> `emulator-metal-survey.md` (file-level findings from peer
> emulators), `macos-input-research.md` (independent input track).

Sub-deliverables informed by the 2026-05-01 emulator survey
(`research.md`).

> **Status (updated 2026-05-02 after Metal slice M14 lands):**
> Sub-deliverables 4a, 4b, 4c, 4e, 4f, 4g, 4h, 4i are **SHIPPED**
> across Metal slices M0–M14 — see per-bullet annotations and the
> 2026-05-02 decision-log entries for each slice. 4d (VS-Expand for
> point sprites / wide lines) is **DEFERRED** — never observed as a
> Crimson / Rainbow / PGR2 hot path; queued as a future slice if a
> game emerges that exercises it. Default-on flip is gated on M15.

- 4a. Metal presentation primitives. `CAMetalLayer`, `MTLDevice`,
  `MTLCommandQueue`, `presentDrawable:atTime:` for VRR-aware
  presentation pacing. Reference: DuckStation
  `metal_device.mm:2536-2620`. Pair with the Phase 2.5 emulation-rate
  slewing for the full frame-pacing recipe.
  **SHIPPED across M0 (build/config integration), M1 (window + device
  + ImGui-Metal HUD), M2 (surface manager), M10 (presentDrawable:
  atTime: + emulation-rate slewing prerequisite via
  `XEMU_GL_RATE_SLEW`); M10.1 CAMetalDisplayLink integration is
  deferred behind a Metal user-driven validation session — slot
  reserved as `METAL_DISPLAY_LINK_CALLBACKS`.**
- 4b. CPU-side index expansion (Metal). Port the existing
  `XEMU_NATIVE_TRI_DEPTH` / `XEMU_NATIVE_QUAD` index logic to the Metal
  backend so Metal only ever sees `MTLPrimitiveTypeTriangle` /
  `TriangleStrip`. Reference: Dolphin
  `Source/Core/VideoCommon/IndexGenerator.cpp` (`AddFan`, `AddQuads`,
  with `pr` / non-`pr` template variants).
  **SHIPPED across M3 (vertex/index buffers + first hand-coded MSL
  draw) and M4 (IndexGenerator port + `XEMU_NATIVE_TRI_DEPTH` /
  `XEMU_NATIVE_QUAD` parity); counters
  `METAL_NATIVE_TRI_DEPTH_DRAWS` / `METAL_NATIVE_QUAD_DRAWS` confirm
  the Metal renderer matches the GL renderer's geometry-shader-bypass
  count.**
- 4c. Framebuffer fetch (Apple GPU only). Gate on
  `[device supportsFamily:MTLGPUFamilyApple1]`. Maps NV2A register-
  combiner / blend modes that don't fit Metal fixed-function blending
  into a single shader pass with MSL `[[color(0)]]` fragment input.
  Barrier-based fallback for Intel Macs. References: PCSX2 PR #5630,
  DuckStation `metal_device.mm:387-410`.
  **SHIPPED across M7 (state-to-PipelineKey + framebuffer-fetch
  validated) and M7.1 (translated pipeline encode swap connected to
  the encoder); the Intel-Mac barrier-based pass-split fallback
  remains a stub since this fork's only target is Apple Silicon —
  `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH=1` flips the Apple1+
  detection result so future Intel-Mac fallback work can exercise
  it.**
- 4d. VS-Expand for point sprites / wide lines. Static precomputed
  index buffer in `MTLStorageModePrivate`; two MSL vertex shader
  variants selected at pipeline-build time via Metal *function
  constants*. Reference: PCSX2 `m_expand_index_buffer` pattern in
  `pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm`.
  **DEFERRED — not observed as a hot path on the Crimson / Rainbow /
  PGR2 / SC2 routes during M0–M14. Queued behind a motivating game
  measurement.**
- 4e. Async pipeline compile + ubershader fallback. Pipeline objects
  cached; specialized variants compiled in the background; ubershader
  bound while waiting; swap on completion. Reference: Dolphin PR #5702
  + the `bSupportsBackgroundCompiling` plumbing. Builds on Phase 2.5
  if that slice landed first.
  **SHIPPED in M8 (Path B: skip-the-draw fallback while pipeline build
  is in flight); Path A (the full Dolphin-style hybrid ubershader) is
  deferred to M8.1 since Path B already drives
  `METAL_DRAWS_SKIPPED_PENDING_TOTAL` toward zero on the M9-warmed
  cache. The `METAL_DRAWS_USING_UBERSHADER_TOTAL` counter slot is
  reserved for M8.1.**
- 4f. Metal shader/pipeline cache persistence. Per-game cache of
  compiled Metal pipeline states keyed by NV2A render-state hash,
  mirroring Dolphin and PPSSPP per-game shader caches.
  **Amended 2026-05-02 (Metal planning session):** persist **MSL
  source strings** keyed by NV2A `ShaderState` hash, not
  `MTLBinaryArchive`-serialized pipelines. DuckStation
  (`m_features.pipeline_cache = false`, `m_features.shader_cache =
  true`) and Dolphin (`bSupportsPipelineCacheData = false`) both reach
  the same conclusion: `MTLBinaryArchive` has limited macOS coverage
  and large breakage surface as of 2026. MSL-source caching achieves
  the same cold-launch warmup goal more portably and skips the
  spirv-cross step on cache hit. See `metal-renderer-plan.md` slice M9
  and decision-log "2026-05-02: Metal renderer planning session".
  **SHIPPED across M5 (in-process per-pipeline LRU cache infra +
  shader-validation harness; harness exit 0 = full pass on the 7
  representative ShaderState fixtures) and M9 (persistent MSL-source
  disk cache; counters `METAL_SHADER_CACHE_LOADS` /
  `METAL_SHADER_CACHE_HITS` / `METAL_SHADER_CACHE_MISSES`).**
- 4g. Metal buffer/texture/surface management. `MTLResourceStorageModeShared`
  for streaming uploads, `MTLStorageModePrivate` for GPU-only resources.
  Reference: Dolphin PR #10754 ("`bUseUnifiedMemory` toggle was removed,
  not worth the extra code").
  **SHIPPED across M2 (surface manager + clear), M3 (vertex/index
  buffers), M6 (texture upload + sampling — Shared staging buffer
  → Private texture via blit encoder + 24-state pre-warmed sampler
  cache; `MTLHeap` for non-aliasable assets). Texture-upload counters
  `METAL_TEX_UPLOAD_BYTES_TOTAL` / `METAL_TEX_UPLOADS_TOTAL` /
  `METAL_TEX_CACHE_HITS` / `METAL_TEX_CACHE_MISSES` surface on the
  `xemu-perf:` interval line. M6 Part B remaining items (full
  S3TC/3D/cube/palette + lifecycle integration with NV2A texture
  cache flushes) intentionally deferred — see decision-log
  "2026-05-02: Metal slice M6". `XEMU_METAL_DISABLE_LOSSLESS_COMPRESSION`
  remains planned-only (no shipped flag); a follow-up slice will
  wire it once a perf-vs-correctness motivating case appears.**
- 4h. Metal System Trace and frame capture workflow. Capture-by-default
  presets for the existing PGR2 / Rainbow / Crimson scene snapshots.
  **SHIPPED in M13 (programmatic `MTLCaptureManager` capture via
  `XEMU_METAL_CAPTURE=path.gputrace` + `XEMU_METAL_CAPTURE_FRAMES=N`,
  default-60-frame bound; per-stage GPU-time counter sampling via
  `MTLCounterSampleBuffer` at the present render pass's
  vertex/fragment stage boundaries; `Info.plist` gains
  `MetalCaptureEnabled = YES` so capture works on the shipped
  `dist/xemu.app` without `MTL_CAPTURE_ENABLED=1` in the env).
  Counters `METAL_VERTEX_US_TOTAL` / `METAL_FRAGMENT_US_TOTAL` /
  `METAL_PRESENT_GPU_US_TOTAL` / `METAL_PRESENT_GPU_FRAMES` /
  `METAL_FX_SPATIAL_GPU_US_TOTAL` / `METAL_CAPTURE_FRAMES` /
  `METAL_CAPTURE_ACTIVE` surface on the `xemu-perf:` interval line.
  Companion `--metal-capture <path>` flag added to
  `scripts/apple-silicon/run-benchmark.sh`.**
- 4i. Performance and correctness comparison against Phase 0.
  **PARTIAL via M14 — `validate-native-tri-depth.sh --run 22` (the
  flat-tri-depth XBE counter-split regression gate, which is
  graphics-API-agnostic from the perspective of the GL counters it
  validates) clears under M14's build. The full per-game paired
  baseline-vs-Metal benchmark sweep is the M15 default-on entry
  gate (5 distinct titles, ≤ 1 % per-pixel diff vs GL, p99 mspf
  jitter ≥ 20 % improvement); it is gated on user-driven
  validation.**
  **Updated 2026-05-03 (M5.5 + M5.6 + M5.7 close out the perf side
  of this gate for the three tracked titles).** Paired Metal-vs-GL
  benchmarks recorded:
  - PGR2: GL `post_load_avg_fps = 30.91`, Metal **37.09**
    (**Metal +18 %**) post-render-pass-coalescing.
  - Crimson Skies: Metal **30.47** (console-native 30 Hz met).
  - Rainbow Six 3: Metal **31.40** (console-native 30 Hz met).
  See `benchmarks/2026-05-03-metal-render-pass-coalescing.md` and
  `benchmarks/2026-05-03-metal-m5_6-translator-failures.md`. Two
  more titles (SC2 + one further) are still required for M15's
  "5 distinct titles" criterion; the visual-diff ≤ 1 % criterion
  is blocked on M5.6 part B (uniform-attribute-via-VSH-UBO
  routing). Stutter intervals improved Rainbow Six 3 22 % → 12 %;
  PGR2 tail mspf is wider than GL on the worst frames (p99 71 ms
  vs GL 45 ms) — the coalesced cmdbuf does more work per commit,
  which under macOS scheduler stress can exceed GL's swap cadence.

**Additional sub-deliverables added by the M-cycle (not in the
original Phase 4 enumeration, but landed via slices M11–M13):**

- 4j. **MSAA + resolve.** `XEMU_METAL_MSAA={0,2,4,8}`; multisample
  companion textures + `MTLStoreActionMultisampleResolve`. SHIPPED
  in M11. Counters `METAL_MSAA_RESOLVE_COUNT` /
  `METAL_MSAA_RESOLVE_US_TOTAL` / `METAL_MSAA_SAMPLE_COUNT`.
  Storage-mode deviation: M11 ships `MTLStorageModePrivate` instead
  of `MTLStorageModeMemoryless` (M11.1 candidate; needs the
  per-`flush_draw` render-pass cadence coalesced first).
  **Updated 2026-05-03: M5.7 / 4m landed render-pass coalescing,
  unblocking M11.1.** Memoryless MSAA can now ship as a follow-up
  slice — the `endEncoding` rate is no longer per-draw, so the
  multisample tile no longer needs `MTLLoadActionLoad` between
  draws. Estimated DRAM bandwidth saving at 1080p 4× MSAA is
  ~3.8-7.6 GB/s per the 2026-05-02 research note.
- 4k. **MetalFX spatial scaler.** `XEMU_METAL_FX_SCALE={1,2,3}`.
  SHIPPED in M12. `MTLFXTemporalScaler` intentionally deferred — NV2A
  has no native motion vectors and synthesizing them from camera-only
  reprojection is risky on dynamic scenes.
- 4l. **Hardening.** `XEMU_METAL_VALIDATION={0,1}` for development;
  doc reconciliation across `automation.md`, `extract-perf-summary.sh`,
  both `CLAUDE.md`, this document, the renderer plan, and the
  decision log. SHIPPED in M14.
- 4m. **Render-pass coalescing (M5.7, 2026-05-03).** Hold one
  `MTLCommandBuffer` + `MTLRenderCommandEncoder` open across
  consecutive `flush_draw` calls when the attachment set is
  unchanged; close on attachment change / `flip_stall` /
  `clear_surface` / `surface_flush` / `pre_savevm` /
  `pre_shutdown` / `finalize`. WWDC20-10632 + the 2026-05-02
  emulator-survey research both flagged the per-draw `commit`
  pattern as the #1 anti-pattern on Apple Silicon TBDR. PGR2
  `post_load_avg_fps` 16.42 → 37.09 (**+125 %**), Crimson Skies
  27.37 → 30.47, Rainbow Six 3 30.24 → 31.40. SHIPPED. Public
  flush API: `pgraph_mtl_draw_flush_open_pass()`. Telemetry:
  `pgraph_mtl_draw_pass_opens_count` /
  `pgraph_mtl_draw_pass_coalesced_count` /
  `pgraph_mtl_draw_pass_flushes_count`. **Unblocks M11.1**
  (Memoryless MSAA — was deferred behind "needs per-flush_draw
  render-pass cadence coalesced first"; that prerequisite is now
  met).
- 4n. **M5.5 — port draw_arrays / inline_elements / inline_array
  + draw_end → flush_draw hook.** The original M-cycle close-out
  declared M5–M14 SHIPPED but left these branches as
  short-circuits (returning without rendering) and `draw_end` as a
  no-op. Result: `METAL_DRAW_COUNT == 0` for the three tracked
  titles. M5.5 lands `mtl/vertex.{c,h}` (~280 LOC CPU-side
  vertex-attribute decoder; formats F / UB_OGL / UB_D3D / S1 /
  S32K) plus the `draw_end → flush_draw(d)` wiring. SHIPPED 2026-05-03.
- 4o. **M5.6 — translator failure rate eliminated.** The
  M-cycle's translated pipelines were rejected by Metal's
  vertex-descriptor validator for 25-43 % of NV2A state
  combinations: (a) MSL declared `[[attribute(N)]]` for "uniform"
  attributes (`pg->vertex_attributes[i].count == 0`) that the
  pipeline-key builder skipped, and (b) the NV097 CMP packed
  format was declared as `INT1010102_NORMALIZED` but spirv-cross
  emits the input as `int`, mismatch. M5.6 populates every vertex-
  descriptor slot (inactive → bufferIndex=3 for DIFFUSE /
  bufferIndex=0 for the rest) and emits CMP as `MTL_VFMT_INT`,
  matching `vk/vertex.c:184`. **Failure rate 25-43 % → 0 %**;
  `METAL_PIPELINE_FAILED = 0` cumulative. SHIPPED 2026-05-03.
  Visual correctness (≤ 1 % per-pixel diff vs GL) deferred to
  **M5.6 part B** — uniform-attribute-via-VSH-UBO routing — which
  is the M15 default-on visual-diff prerequisite.

### Phase 5: Performance Hardening

> **Status (updated 2026-05-02): 5a is partially landed.** V1
> (`XEMU_TCG_SPLITWX`, default-on) and V2
> (`XEMU_TCG_JMP_CACHE_TARGETED`, default-on) shipped 2026-05-01/02
> as PARTIAL passes — both slices are mechanically correct (V1
> drives `pthread_jit_write_protect_np` count to 0; V2 collapses
> `TCG_JMP_CACHE_ZEROED_BUCKETS` per invalidation) but the Crimson
> 1.27-second worst-frame is unchanged, because V3 spike attribution
> (`benchmarks/2026-05-02-tcg-spike-attribution.md`) and D3 1 ms
> spike-log (`benchmarks/2026-05-02-tcg-30fps-cap-attribution.md`)
> showed the worst frame is composed of ~422 ms attributed to a
> `tcg_tb_chain` at guest PC `0x23dd47` (game-app routine
> NV1BA0_PIO_VOICE_LOCK MMIO sequence) plus ~970 ms of unattributed
> sub-1 ms `tb_gen_code` churn + kernel-PC `0x80030e4c` 1 ms TB
> chains. I5 (`XEMU_APU_LOCK_RELEASE`, default-on,
> `benchmarks/2026-05-02-apu-lock-release-validation.md`) eliminated
> the `0x23dd47` MMIO-block contention (vCPU lock-wait −97.9 %, p999
> −35 %, steady-state stutter intervals −61 %) but the headline
> 1.28-s frame is still unchanged at the 500 ms PASS threshold. V6
> (`benchmarks/2026-05-02-v6-cpu-exec-loop-attribution.md`, landed
> 2026-05-02 as instrumentation only) ruled out per-event 1 ms
> dominance for `tb_lookup`, `tb_gen_code`, and `cpu_handle_interrupt`
> across 300 s of Crimson (0 / 0 / 1 events respectively; zero V6
> events inside the worst-frame interval). The 1 ms-class
> `tcg_tb_chain` events are reframed as **normal hot-path
> execution** (mean `tb_count = 1918` × ~500 ns/iter), not pathology;
> D3's "host-side wait" hypothesis is disproved. The dominant new
> finding is the worst-frame TCG counter storm:
> `TCG_TB_INVALIDATE_COUNT = 8954` and `TCG_NOTDIRTY_PAGES_HIT =
> 1200` (both ~6-24× steady state), distributed across many
> sub-millisecond translation events. The next active investigation
> is V7 — cumulative per-interval `TCG_TB_*_US_TOTAL` counters —
> to confirm whether translation cost dominates the cumulative axis;
> if yes, **PPTC (Phase 5a downstream entry)** is justified as the
> follow-on fix.

Deliverables:

- 5a. **TCG TB-invalidation cost reduction on Apple Silicon
  (partially landed; V6 instrumentation done 2026-05-02 with
  NEGATIVE per-event 1 ms result; V7 cumulative-counter slice
  queued).** The 2026-05-01 sample profile
  attributed Crimson's 1.35-second worst-frame to the JIT TB
  invalidation chain (`tb_invalidate_phys_range_fast` →
  `do_tb_phys_invalidate` → `tcg_flush_jmp_cache`) plus
  `pthread_jit_write_protect_np` and `sys_icache_invalidate`. Apple
  Silicon pays real syscall cost per TB flush. Investigation paths in
  priority order (status as of 2026-05-02):

  - **V1 splitwx (landed default-on, PARTIAL).** `XEMU_TCG_SPLITWX=1`
    selects the `mach_vm_remap` dual-mapping path so TB execution no
    longer pays the per-TB `pthread_jit_write_protect_np()` syscall.
    Sample profile confirmed `pthread_jit_write_protect_np` count
    drops from 11 to 0. Headline Crimson worst-frame unchanged
    (+0.33 % vs OFF). `include/qemu/osdep.h` W^X toggles are
    diff-guarded. Verification:
    `benchmarks/2026-05-01-tcg-splitwx-validation.md`.
  - **V2 jmp-cache-targeted invalidation (landed default-on,
    PARTIAL).** `XEMU_TCG_JMP_CACHE_TARGETED=1` replaces the
    unconditional 4096-entry per-CPU jmp-cache zero in the
    `CF_PCREL` branch with a single-bucket clear per invalidated TB.
    Per-call max wallclock drops to ~700 µs (well below the
    worst-frame mspf), confirming the worst frame is built from many
    small invalidations or a non-invalidation source. Verification:
    `benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md`.
  - **V6 `cpu_exec_loop` per-phase instrumentation (landed
    2026-05-02 as instrumentation only; NEGATIVE per-event 1 ms
    result).** Added `tcg_tb_lookup` / `tcg_tb_gen_code` /
    `tcg_handle_interrupt` spike sources gated on
    `XEMU_PERF_SPIKE_LOG_TCG=1`. 300 s Crimson route at 1 ms
    threshold: 0 / 0 / 1 events respectively across the full run;
    zero V6 events inside the worst-frame interval. The 1 ms-class
    `tcg_tb_chain` events are reframed as **normal hot-path
    execution** (mean `tb_count=1918` × ~500 ns/iter), not
    pathology. The dominant new finding is in always-on per-interval
    TCG counters: worst-frame interval shows `TCG_TB_INVALIDATE_COUNT
    = 8954` (~6× steady state) and `TCG_NOTDIRTY_PAGES_HIT = 1200`
    (~24× steady state) — a translation-churn storm distributed
    across many sub-millisecond events. V6 verification:
    `benchmarks/2026-05-02-v6-cpu-exec-loop-attribution.md`.
  - **V7 `TCG_*_US_TOTAL` cumulative per-interval phase counters
    (queued, next).** V6 ruled out per-event 1 ms+ dominance, so
    cumulative wallclock measurement is required. Add
    `TCG_TB_LOOKUP_US_TOTAL` / `TCG_TB_GEN_CODE_US_TOTAL` /
    `TCG_HANDLE_INTERRUPT_US_TOTAL` (sum, per interval) gated on
    a new `XEMU_TCG_PHASE_LOG=1` env var. Decision criterion: if
    `TCG_TB_GEN_CODE_US_TOTAL ≥ 300 ms` in the worst-frame
    interval, **PPTC (next entry) is the right slice**; if not,
    follow up with host-thread profiling (Apple `sample` via
    `scripts/apple-silicon/sample-profile.sh`).
  - **Persistent TCG translation cache (PPTC pattern) — gated on
    V7 outcome.** Serialize TCG translation blocks across runs
    AND across in-process `tb_flush` events, keyed by guest binary
    hash + cflags. First-load and warmup-stutter win. The Crimson
    worst-frame's `TCG_TB_INVALIDATE_COUNT = 8954` translates to
    ~450 ms of cumulative re-translation cost (8954 × ~50 µs)
    consistent with the observed 446 ms of `tcg_tb_chain` time;
    PPTC eliminates this re-translation entirely. Estimated
    ceiling: drop the worst frame from 1.375 s to ~900 ms.
    Reference:
    https://blog.ryujinx.org/introducing-profiled-persistent-translation-cache/.
  - W^X toggle batching in `accel/tcg/tb-maint.c`. Lower priority
    now that V1 splitwx-on path removes the per-TB toggle entirely.
  - Smarter softmmu notdirty page handling — reduce the rate at
    which writes through `do_st4_mmu` trigger
    `tb_invalidate_phys_range_fast`. `TCG_NOTDIRTY_TRIPS` /
    `TCG_NOTDIRTY_PAGES_HIT` counters now exist for measurement.
  - Upstream QEMU MTTCG patches for Apple Silicon JIT handling.
    Search qemu-devel and qemu-project/qemu for `MAP_JIT`,
    `pthread_jit_write_protect_np`, and `tb_flush` patches.
- Shader cache persistence and prewarming where useful (extends
  existing on-disk cache; would extend Phase 4f if Metal lands).
- Reduced synchronization stalls.
- Texture/surface upload/download audit.
- Game-specific regression suite.
- 5b. SSE / x87 floating-point helper audit — **completed 2026-05-01,
  hypothesis disproved for SSE.** See
  `benchmarks/2026-05-01-tcg-float-audit.md` and decision-log entry
  "2026-05-01: SSE hardfloat already active on aarch64; x87 80-bit
  irreducibly soft". Source-level finding:
  `float32_gen2`/`float64_gen2` already dispatches to a hard arm64
  `fmul` when the guest's MXCSR is in the common state — no
  `__x86_64__` gate disables the shortcut on aarch64. The visible
  `parts64_uncanon_normal` time in the post-fast-read sample is the
  necessary soft fallback for first-op-after-MXCSR-reset, NaN/Inf/
  denormal inputs, denormal results, and non-default rounding modes.
  `helper_fmul_ST0_FT0` (x87 80-bit) is irreducibly soft on Apple
  Silicon because there is no native 80-bit float on aarch64; the
  fork's existing `__hard` x87 path is correctly gated to `XBOX
  && __x86_64__`. **Remaining 5b work** (deferred, real but
  Phase-2-scope): (i) cheap counter-pair experiment around
  `float32_gen2` (`sse_hard_taken` vs `sse_soft_fallback` per reason)
  to confirm the steady-state hard-take ratio > 0.9 on the PGR2
  snapshot before any further float work; (ii) NEON-based
  `floatx80_mul`/`floatx80_add` Dekker / TwoProduct kernel for x87
  bit-exact 80-bit math; (iii) opt-in `XEMU_X87_RELAXED_PRECISION=1`
  honoring the guest FPU control word's PC field, gated per-title and
  off by default. Both (ii) and (iii) are real work and only justified
  after Instruments confirms x87 is dominant in real-game inner loops.

## What we ruled out

The 2026-05-01 emulator survey identified several techniques that other
projects use but that are not on this fork's roadmap, with reasons:

- Custom x86 → ARM64 JIT replacing TCG. RPCS3 PR #12115's documented
  macOS-on-Apple-Silicon JIT pain catalog (16 KB pages,
  `MAP_FIXED | MAP_JIT` ban, W^X toggling) is real and the typical
  game-emulation custom-JIT ceiling over TCG is 2–5×, not 10×. Per
  workspace rule #1, measurement should drive that decision; the most
  recent profile (`benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`)
  attributes ~9 % of TCG-thread time to mutex wait and the bulk of the
  remainder to floating-point helpers. The 5b audit
  (`benchmarks/2026-05-01-tcg-float-audit.md`) showed the SSE
  float32/float64 helpers already take the hardfloat shortcut on
  aarch64 in steady state, so the float-helper time visible in the
  profile is split between irreducible x87 80-bit cost and
  correctness-required softfloat fallback for SSE edge cases — not
  unconditional softfloat trips. A custom JIT cannot recover that
  surface area either, since the irreducible portion is genuinely
  irreducible without precision-relaxing semantics.
- Indirect command buffers, argument buffers, mesh shaders. Tellusim's
  Metal MDI study (https://tellusim.com/metal-mdi/) shows ICBs win
  only for many small draws and lose by ~1.5× for larger draws. The
  win is meaningful only after per-draw shader compile cost is
  absorbed and the bottleneck has moved into command-encoding overhead
  — not the case at Xbox-era workloads.
- Apple Hypervisor.framework for guest CPU. Only useful when guest and
  host ISA match; xemu's guest is x86 32-bit and the host is ARM64.

## Risks

- CPU TCG may become the next bottleneck after renderer work succeeds.
- Some NV2A behavior may be hard to reproduce exactly without shader tricks.
- Metal shader translation may require a fork-specific IR rather than direct
  GLSL-to-MSL conversion.
- Vulkan-over-Metal drivers are evolving; behavior may vary by SDK and macOS
  version.

## Success Criteria

> **Updated 2026-05-02 after the Soul Calibur 2 sanity test
> (`benchmarks/2026-05-02-60hz-title-sanity-test.md`) and the V4
> broader title sweep
> (`benchmarks/2026-05-02-broader-title-sweep.md`).** The previous
> "PGR2, Crimson Skies, and Rainbow Six 3 sustain 60 FPS" framing is
> **superseded as technically impossible**: those three titles render
> at 30 Hz on real Xbox hardware (engine-intrinsic frame-pacing),
> and SC2 demonstrated this fork already sustains 60.57 FPS for 109
> consecutive intervals on a known-60-Hz Xbox title at scale=2 +
> MSAA=4. The cap on the tracked-3 titles is the **guest engine**, not
> xemu — `NV2A_VBLANK_FIRES > 30/s` while `NV2A_PRESENT_HEARTBEAT ==
> 30/s` is the decisive ratio (D3,
> `benchmarks/2026-05-02-tcg-30fps-cap-attribution.md`).
>
> The previous "fork stays on OpenGL" framing is superseded by the
> 2026-05-02 Metal pivot. OpenGL remains the reference/fallback path;
> native Metal is the product renderer direction.

- **Each tracked title sustains its console-native FPS in gameplay.**
  Console-native: PGR2 / Crimson Skies
  / Rainbow Six 3 = 30 Hz, Soul Calibur 2 / Burnout 3 / Halo CE / etc.
  per the V4 sweep table. The 30 FPS floor on the original tracked
  three is already met (2026-05-01 with four opt-in flags). The
  tracked 60 Hz titles in the V4 sweep reach native 60 Hz on the same
  build.
- **1-second-class worst-frame stutter is best-effort complete within
  the current TCG architecture.** V9/V10 attributed all measured
  xemu-side cost classes in Crimson's 1.3 s worst-frame interval to
  <100 ms total; the remaining ~1.2 s is raw JIT'd guest code
  execution. Further reduction requires larger rearchitecture (PPTC /
  AOT, HLE kernel work) or game-specific patches, not OpenGL renderer
  polishing. Metal may improve presentation smoothness and latency,
  but it is not expected to erase guest-engine or TCG-only stalls by
  itself.
- **1080p output (internal scale 2×) on tracked titles, with
  anti-aliasing as a player-visible option.** Met as of 2026-05-02:
  Apple Silicon system builds default `surface_scale = 2` on first
  launch (`XEMU_DISPLAY_SCALE` overrides per session); `XEMU_GL_MSAA
  ∈ {2,4,8}` is opt-in (default 0), clamped to `GL_MAX_SAMPLES` (4 on
  Apple GL-on-Metal). Composition validated end-to-end by V3
  composite-goal validation
  (`benchmarks/2026-05-02-composite-goal-validation.md`) and across
  the V4 sweep at scale=2 + MSAA=4 with no MSAA-driven pipeline-
  variant explosion (worst case `SHADER_COMPILE_COUNT` 3,429 on NGB
  is engine-content-driven, not MSAA-driven; OutRun 2 with the
  heaviest MSAA resolve workload only generated 142 shader compiles).
- **Broader title coverage validated.** V4 sweep covers Burnout 3,
  Halo CE, Splinter Cell, Ninja Gaiden Black, OutRun 2, plus SC2
  cross-reference: 6 of 6 titles pass the FPS / pathology gate; 0
  new title-specific Apple-GL pathologies; 4 of 6 surface the same
  catalogued Crimson-class TCG TB-invalidation worst-frame
  pathology (one V6 fix would address all of them). NGB additionally
  exercises **line primitives** through the geometry shader (87,243
  line draws) — a `XEMU_NATIVE_LINE` bypass is the natural
  follow-on slice if NGB-class titles become a priority focus.
- **Apple Silicon system build ships eight default-on flags**
  (updated 2026-05-02 after V9): `XEMU_NATIVE_TRI_DEPTH`,
  `XEMU_NATIVE_QUAD`, `XEMU_PGRAPH_FAST_READ`, `XEMU_TCG_SPLITWX`,
  `XEMU_TCG_JMP_CACHE_TARGETED`, `XEMU_APU_LOCK_RELEASE`,
  `XEMU_FAST_RDTSC` (V9, RDTSC fast-path), and
  `display.quality.surface_scale = 2`. Plus opt-in `XEMU_GL_MSAA`
  and `XEMU_PGRAPH_ASYNC_SHADER_COMPILE`. Each is overridable via
  the documented env var.
- Correctness regressions are documented, minimized, and tracked.
  Open: `XEMU_APU_LOCK_RELEASE` widens an existing race class
  (~5.33 ms slightly-stale audio per affected voice per frame —
  within existing upstream-loose patterns; see decision-log
  "2026-05-02: Ship XEMU_APU_LOCK_RELEASE default-on with audio
  listen-test gate"). Human listen-test on the three tracked titles
  is the gating step before fully-shipped status.
- Phase 4 native Metal renderer is the primary product-renderer
  investment. It is justified by the final product requirements:
  frame timing, latency, capture/profiling, enhancement controls,
  pipeline caching, and long-term maintainability. Metal is not
  expected to fix guest engine caps or TCG-only stalls by itself.
