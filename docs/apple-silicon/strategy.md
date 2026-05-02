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

> **Status (updated 2026-05-01): deprioritized.** The GL-vs-Metal
> decision diagnostic showed Apple's GL has measured headroom for
> 60 FPS at 1080p (and even 4×-scale) on tracked titles. Vulkan-over-
> Metal evaluation no longer has a forcing function. Reconsider only
> if MSAA-on-GL or broader-title coverage surface a renderer-side
> ceiling.

Deliverables (deferred):

- Darwin Vulkan discovery and surface creation.
- MoltenVK/KosmicKrisp test matrix.
- Feature probe report at startup.
- Decision whether Vulkan-over-Metal is production-worthy or only a test path.

### Phase 4: Native Metal Renderer

> **Status (updated 2026-05-01): deprioritized — not the next
> implementation slice.** The GL-vs-Metal decision diagnostic
> (`benchmarks/2026-05-01-gl-vs-metal-decision.md`) demonstrated that
> Apple's GL-on-Metal is not the gating constraint for any project
> goal: PGR2 snapshot at 4× internal scale (~2560×1920) showed only
> 7 % growth in `FLUSH_DRAW_US_TOTAL` and stable p99. The renderer
> has measured headroom for 60 FPS at 1080p plus AA on tracked
> titles. Phase 4 remains the long-term ceiling-removing path
> (cleaner code, Apple-extension access, framebuffer-fetch-class
> features unreachable from GL or MoltenVK), but it is not what
> blocks the project's stated goals. Reconsider when MSAA-on-GL or
> the broader-title sweep surface a renderer-side ceiling, or when
> the long-term code-quality / future-proofing case becomes the next
> highest-leverage investment.

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

> **Status (updated 2026-05-01): 5a (PPTC + Apple Silicon TCG TB-
> invalidation cost) is now the highest-priority active slice.** The
> GL-vs-Metal decision diagnostic moved this from "future hardening"
> to "the only thing that gates every project goal" — see decision-
> log entry "2026-05-01: Stay on OpenGL; the headline bottleneck is
> TCG TB invalidation, not the renderer."

Deliverables:

- 5a. **TCG TB-invalidation cost reduction on Apple Silicon (active).**
  The 2026-05-01 sample profile attributed Crimson's 1.35-second
  worst-frame to the JIT TB invalidation chain
  (`tb_invalidate_phys_range_fast` → `do_tb_phys_invalidate` →
  `tcg_flush_jmp_cache`) plus `pthread_jit_write_protect_np` and
  `sys_icache_invalidate`. Apple Silicon pays real syscall cost per
  TB flush. Investigation paths in priority order:
  - Persistent TCG translation cache (PPTC pattern). Serialize TCG
    translation blocks across runs, keyed by guest binary hash.
    First-load and warmup-stutter win. Reference:
    https://blog.ryujinx.org/introducing-profiled-persistent-translation-cache/.
  - W^X toggle batching in `accel/tcg/tb-maint.c`. Apple recommends
    batching writes under a single `pthread_jit_write_protect_np`
    flip; xemu likely toggles per invalidation. Investigate whether
    the invalidation burst can be wrapped in a single toggle.
  - Smarter softmmu notdirty page handling — reduce the rate at
    which writes through `do_st4_mmu` trigger
    `tb_invalidate_phys_range_fast`. The Xbox CPU emulator may
    currently treat all writes through softmmu as potentially-
    invalidating.
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

> **Updated 2026-05-01 to match the GL-vs-Metal verdict.** The fork
> stays on OpenGL as the active renderer. "Defaults to non-OpenGL"
> was based on an incorrect bottleneck attribution.

- **TCG TB-invalidation cost on Apple Silicon is reduced** so that
  Crimson Skies-class self-modifying-code scenes do not produce
  1-second-class worst-frame stalls.
- **PGR2, Crimson Skies, and Rainbow Six 3 sustain 60 FPS in
  gameplay** at native Xbox internal resolution after the TCG fix.
  30 FPS floor is already met (2026-05-01).
- **1080p output (internal scale 2×) at sustained 60 FPS** on
  tracked titles, with anti-aliasing as a player-visible option.
- **Apple GL renderer pipeline-variant cost remains non-pathological
  under MSAA enable** (validated by `SHADER_COMPILE_*` counters
  during a stress test that we have not yet run).
- **Broader title coverage** — at least 5 titles from the external
  Xbox library exhibit no Apple-GL pathologies that the tracked
  three did not.
- Correctness regressions are documented, minimized, and tracked.
- Phase 4 native Metal renderer becomes a Phase-2 quality
  investment, justified by code-base health or by a measured
  renderer-side ceiling — not by the headline judder, which Metal
  would not address.
