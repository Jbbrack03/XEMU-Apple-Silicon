# Strategy

Last updated: 2026-05-01

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

These two slices are independent of graphics-API choice and can land on
the current OpenGL path before Phase 3 or Phase 4. They address the
user-visible jitter sources documented in
`benchmarks/2026-05-01-baseline-jitter.md`:

- Crimson Skies' 1310 ms worst-frame from synchronous shader compilation
  in Apple's GL-on-Metal driver (P99 = 892 ms, longest stutter run = 16
  s). Identified as the highest user-visible jitter cost.
- PGR2 / Rainbow tail jitter from emulation-clock drift relative to host
  vsync.

Deliverables:

- Frame pacing — emulation-rate slewing. Lock guest 60 Hz to host vsync
  via fractional clock adjustment of ≤ 1 %, mirroring DuckStation's
  "Sync to Host Refresh Rate" (PCSX2 PR #5488). Graphics-API-agnostic;
  measurable on the existing OpenGL build via the per-interval
  `mspf_max` jitter keys in `extract-perf-summary.sh`.
- Async shader compile (`XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`). Worker
  thread compiles shader programs off the renderer's critical path;
  placeholder/ubershader bind while waiting; swap on completion. Pattern
  from Dolphin's hybrid ubershader (PR #5702) and RPCS3's 2018 async
  shader pipeline. Verification gate: Crimson route's `SHADER_GEN` /
  `SURF_TO_TEX` / `TEX_UPLOAD` co-occurring stutter intervals dropping
  by ≥ 80 %.

Both slices are research-informed; named source references are in
`research.md` "Apple Silicon Emulator Survey (2026-05-01)".

### Phase 3: Vulkan-over-Metal Prototype

Deliverables:

- Darwin Vulkan discovery and surface creation.
- MoltenVK/KosmicKrisp test matrix.
- Feature probe report at startup.
- Decision whether Vulkan-over-Metal is production-worthy or only a test path.

### Phase 4: Native Metal Renderer

Sub-deliverables informed by the 2026-05-01 emulator survey
(`research.md`):

- 4a. Metal presentation primitives. `CAMetalLayer`, `MTLDevice`,
  `MTLCommandQueue`, `presentDrawable:atTime:` for VRR-aware
  presentation pacing. Reference: DuckStation
  `metal_device.mm:2536-2620`. Pair with the Phase 2.5 emulation-rate
  slewing for the full frame-pacing recipe.
- 4b. CPU-side index expansion (Metal). Port the existing
  `XEMU_NATIVE_TRI_DEPTH` / `XEMU_NATIVE_QUAD` index logic to the Metal
  backend so Metal only ever sees `MTLPrimitiveTypeTriangle` /
  `TriangleStrip`. Reference: Dolphin
  `Source/Core/VideoCommon/IndexGenerator.cpp` (`AddFan`, `AddQuads`,
  with `pr` / non-`pr` template variants).
- 4c. Framebuffer fetch (Apple GPU only). Gate on
  `[device supportsFamily:MTLGPUFamilyApple1]`. Maps NV2A register-
  combiner / blend modes that don't fit Metal fixed-function blending
  into a single shader pass with MSL `[[color(0)]]` fragment input.
  Barrier-based fallback for Intel Macs. References: PCSX2 PR #5630,
  DuckStation `metal_device.mm:387-410`.
- 4d. VS-Expand for point sprites / wide lines. Static precomputed
  index buffer in `MTLStorageModePrivate`; two MSL vertex shader
  variants selected at pipeline-build time via Metal *function
  constants*. Reference: PCSX2 `m_expand_index_buffer` pattern in
  `pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm`.
- 4e. Async pipeline compile + ubershader fallback. Pipeline objects
  cached; specialized variants compiled in the background; ubershader
  bound while waiting; swap on completion. Reference: Dolphin PR #5702
  + the `bSupportsBackgroundCompiling` plumbing. Builds on Phase 2.5
  if that slice landed first.
- 4f. Metal shader/pipeline cache persistence. Per-game cache of
  compiled Metal pipeline states keyed by NV2A render-state hash,
  mirroring Dolphin and PPSSPP per-game shader caches.
- 4g. Metal buffer/texture/surface management. `MTLResourceStorageModeShared`
  for streaming uploads, `MTLStorageModePrivate` for GPU-only resources.
  Reference: Dolphin PR #10754 ("`bUseUnifiedMemory` toggle was removed,
  not worth the extra code").
- 4h. Metal System Trace and frame capture workflow. Capture-by-default
  presets for the existing PGR2 / Rainbow / Crimson scene snapshots.
- 4i. Performance and correctness comparison against Phase 0.

### Phase 5: Performance Hardening

Deliverables:

- Shader cache persistence and prewarming where useful (extends Phase 4f).
- Reduced synchronization stalls.
- Texture/surface upload/download audit.
- Game-specific regression suite.
- 5a. Persistent TCG translation cache (PPTC pattern). Serialize TCG
  translation blocks across runs, keyed by guest binary hash.
  First-load win only; will not help steady-state PGR2 / Rainbow /
  Crimson. Reference:
  https://blog.ryujinx.org/introducing-profiled-persistent-translation-cache/.
- 5b. SSE / x87 floating-point helper audit. Already tracked in
  `handoff.md` Prioritized Next Tasks #3. If SSE float32 ops go through
  `soft_f32_mul` while NEON float32 is available, lift to hardfloat.
  Largest potential TCG win on Apple Silicon per
  `benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`.

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
  remainder to floating-point helpers — the SSE / x87 hardfloat audit
  (5b) is a much cheaper way to address the same surface area.
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

- Apple Silicon build defaults to a non-OpenGL fast path.
- Known macOS 3D regression scenes recover frame pacing.
- Shader compilation stalls are measurable and substantially reduced.
- PGR2, Crimson Skies, and Rainbow Six 3 sustain at least 30 FPS in gameplay;
  60 FPS is desirable but not the floor for stability/performance.
- Correctness regressions are documented, minimized, and tracked.
