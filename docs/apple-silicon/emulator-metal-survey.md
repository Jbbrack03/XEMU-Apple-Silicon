# Emulator Metal Backend Survey — Source-level findings

Last updated: 2026-05-02

This document is the source-level companion to the high-level "Apple Silicon
Emulator Survey (2026-05-01)" section of `research.md`. It records the
specific files, function names, hash-key shapes, and dispatch patterns we
intend to adopt or reject when we build xemu's native Metal backend.

It supersedes nothing in `research.md`; it goes one level deeper.

---

## A. Dolphin (`Source/Core/VideoBackends/Metal/`)

Native Metal backend introduced in PR #10754 (TellowKrinkle, 2022). Layered
over the `VideoCommon` abstract device (`AbstractGfx`, `AbstractPipeline`,
`AbstractTexture`, `AbstractFramebuffer`, `VertexManagerBase`, `PerfQuery`).

### File layout

```
MTLMain.mm             VideoBackend::Initialize() entry; device/queue/layer
MTLObjectCache.h/.mm   depth-stencil cache; sampler cache; render-pipeline cache
MTLStateTracker.h/.mm  encoder/state batching; upload buffer pools; FlushEncoders
MTLGfx.h/.mm           AbstractGfx; texture/framebuffer; BindBackbuffer/PresentBackbuffer
MTLPipeline.h/.mm      MTLRenderPipelineState wrapper + reflection
MTLShader.h/.mm        MTLLibrary/MTLFunction wrapper (19 LOC)
MTLTexture.h/.mm       AbstractTexture / Framebuffer
MTLVertexFormat.h/.mm  MTLVertexDescriptor for vertex layout
MTLVertexManager.h/.mm vertex/index uploads via StateTracker
MTLBoundingBox.h/.mm   bounding-box compute pre-pass
MTLPerfQuery.h/.mm     visibility-result counters
MTLUtil.h/.mm          backend feature probing; SPIR-V→MSL translation
MRCHelpers.h           manual ARC RAII for ObjC++
```

### Key patterns

- **`Internal::PipelineID` POD hashed with `memcmp`** (`MTLObjectCache.mm:266-365`).
  Packed struct of vertex layout + shader pointers + blend state +
  framebuffer state. `operator<` and `operator==` are `memcmp`. Storage
  is `std::map<PipelineID, StoredPipeline>`.
- **`GetOrCreatePipeline` cv-wait pattern** (`MTLObjectCache.mm:506-531`).
  Lock; lookup; if found return; else if pending wait on cv; else insert
  sentinel, drop lock, compile, retake lock, fill, notify_all. Renderer
  thread blocks at most once per unique pipeline.
- **`MTLPipelineOptionArgumentInfo` reflection** captured at compile time
  so the state tracker skips `setFragmentTextures:` for unused slots.
- **`setShouldMaximizeConcurrentCompilation:YES`** (`MTLMain.mm:108-115`),
  guarded by `respondsToSelector:` because OCLP-patched older Macs crash
  on the selector. **The single most important Apple Silicon
  shader-compile parallelism knob (macOS 13.3+).**
- **One render command buffer per emulated frame batch; one render
  encoder per framebuffer change.** `BeginRenderPass`
  (`MTLStateTracker.mm:313`) opens encoder lazily; ends on framebuffer
  change. Multiple draws batched in same encoder. State applied lazily
  via dirty-bitmask in `PrepareRender`.
- **`BufferPair` pool with `UsageTracker` ring allocator**
  (`MTLStateTracker.mm:139-238`). cpubuffer is
  `Shared|WriteCombined`; gpubuffer is `Private|HazardTrackingUntracked`.
  On Apple Silicon (`unified_memory == YES`), gpubuffer is omitted —
  same Shared buffer used for both. **Dolphin removed `bUseUnifiedMemory`
  toggle in PR #10754** because separate code paths weren't worth it.
- **SPIR-V→MSL via spirv-cross** (`MTLUtil.mm:493-630`). glslang →
  SPIR-V 1.5 → spirv-cross `CompilerMSL`. `set_msl_version(2,3)`,
  `use_framebuffer_fetch_subpasses = true`. Bindings are deterministic
  per stage so they're known at backend-generation time.
- **CPU-side `IndexGenerator`** (`Source/Core/VideoCommon/IndexGenerator.cpp`).
  `AddFan<pr>`, `AddQuads<pr>`, `AddLineList`, `AddLineStrip`. Two
  compile-time variants (`<true>`/`<false>` for primitive-restart) emit
  triangle strips with `0xFFFF` separators or triangle lists. **Direct
  template for xemu's Metal-side `XEMU_NATIVE_QUAD` / `XEMU_NATIVE_TRI_DEPTH`
  port.**
- **Hybrid ubershader fallback** (`VideoCommon/ShaderCache.cpp`). A
  single megashader interprets TEV state at runtime while the per-pipeline
  specialized variant compiles in the background. xemu's
  `XEMU_PGRAPH_ASYNC_SHADER_COMPILE` (RPCS3 PR #4876 "skip the draw"
  pattern) is more aggressive — for Metal, adopt the cv-wait with a
  short timeout, falling back to "skip the draw" for late shaders.
- **`m_features.bSupportsPipelineCacheData = false`** (`MTLUtil.mm:72`).
  Metal has no portable serializable pipeline-cache blob; ubershader
  fallback hides cold-launch compile cost.

### Frame pacing (Dolphin)

`MTLGfx::PresentBackbuffer` (`MTLGfx.mm:464-487`) does NOT use
`presentDrawable:atTime:`. It uses `presentDrawable:` directly when vsync
is on, or `addScheduledHandler:` deferred present otherwise. xemu should
adopt **DuckStation's** pattern instead (see §C below).

`m_layer setDisplaySyncEnabled:bVSyncActive` toggles vsync. Default
`maximumDrawableCount = 3`.

### Apple Silicon gotchas (Dolphin)

1. Pipeline `rasterSampleCount` must match attached texture sample count
   exactly; enforced in `PipelineID::framebuffer.samples`.
2. Driver-bug enums in `DriverDetails` (`BUG_INVERTED_IS_HELPER`,
   `BUG_BROKEN_SUBGROUP_OPS_WITH_DISCARD`) — Apple Silicon clean here.
3. `MTLDepthClipModeClamp` requires Apple4+ (`MTLUtil.mm:264`); M1 = Apple7
   so xemu is fine.

---

## B. PCSX2 (`pcsx2/GS/Renderers/Metal/`)

Native Metal by TellowKrinkle, sibling project to Dolphin's Metal backend
but separate codebase. PCSX2 has its own `GSDevice` virtual base.

### File layout

```
GSDeviceMTL.h/.mm       device init; pipeline cache; encoder
GSMTLDeviceInfo.h/.mm   gpu-family probing → m_dev.features
GSMTLShaderCommon.h     MSL bridging
GSMTLSharedHeader.h     header included by both C++ and .metal
GSTextureMTL.h/.mm      concrete texture type
*.metal                 hand-written MSL kernels (cas/convert/fxaa/interlace/
                        merge/misc/present/tfx) compiled to metallib at build
```

### Key patterns

- **Static expand-index buffer** (`GSDeviceMTL.mm:1003`).
  `m_expand_index_buffer = CreatePrivateBufferWithContent(...,
  EXPAND_BUFFER_SIZE, GenerateExpansionIndexBuffer)`. Built once at init in
  `MTLStorageModePrivate`, bound as the index buffer for point-sprite /
  wide-line draws. `GenerateExpansionIndexBuffer` lives in
  `pcsx2/GS/Renderers/Common/GSDevice.cpp` (shared with DX/OGL backends).
  **Direct template for xemu's NV2A point-sprite emulation.**
- **Function-constant specialization**
  (`MRESetHWPipelineState`, `mm:1903-2039`). `MTLFunctionConstantValues`
  with 50+ constants set per fragment-shader variant; `MTLFunction` is
  fetched via `[m_dev.shaders newFunctionWithName:@"ps_main"
  constantValues:m_fn_constants error:&err]`. Each combination compiles
  to a fresh MSL kernel; parallelizes across CPU cores when
  `setShouldMaximizeConcurrentCompilation:` is on.
- **Two-level pipeline cache** (`m_hw_vs[VSSelector::key]`,
  `m_hw_ps[pssel]`, `m_hw_pipeline[fullsel]`). `PSSelector` is 96-bit POD,
  hashed with `std::hash<u64>` of `(key_hi, key_lo)`, equality by `memcmp`.
- **`unified_memory` feature toggle.** When set,
  `MTLResourceStorageModeShared|MTLResourceCPUCacheModeWriteCombined` for
  upload, no separate gpu-side buffer. `m_draw_sync_fence = [dev newFence]`
  used only when manual upload is in use.
- **Pre-compiled `.metal` files** shipped as a `metallib`, loaded with
  `[device newLibraryWithURL:]`. Function-constant specialization at
  runtime. **This architectural choice is incompatible with NV2A's
  combiner state space**; xemu must use Dolphin's spirv-cross model.
- **Framebuffer fetch with explicit Apple-family gating**
  (`GSMTLDeviceInfo.mm:149-155`). MSL 2.3 + `[device
  supportsFamily:MTLGPUFamilyApple1]`. Apple Silicon always supports it.

### Anti-pattern (PCSX2)

- **`SpinManager`** (`mm:351-373, 396, 503`) runs a no-op compute kernel
  to keep Apple's GPU clock high between submits. **On Apple Silicon's
  unified-power package, this directly steals power from the CPU and
  hurts thermals.** Reject for xemu.

### Frame pacing (PCSX2)

PCSX2 uses `presentDrawable:` (not `atTime:`). Their "Sync to Host
Refresh Rate" (PR #5488) operates at the emulation-rate level — nudging
GS frametime by ≤ 1 % to align with measured host refresh interval.
**Graphics-API-agnostic**; can land on xemu's GL backend before Metal
exists.

---

## C. DuckStation (`src/util/metal_device.{h,mm}`)

One ~2769-LOC `metal_device.mm`. Native Metal sibling of D3D11/12, OpenGL,
and Vulkan backends.

### Key patterns

- **`presentDrawable:atTime:` with mach-absolute deadline**
  (`metal_device.mm:2577-2601`):

  ```objc
  Timer::Value current_time;
  if (present_time != 0 && (current_time = Timer::GetCurrentValue()) < present_time) {
    const u64 mach_ns =
        CocoaTools::ConvertMachTimeBaseToNanoseconds(mach_absolute_time());
    const double mach_present_time =
        static_cast<double>(mach_ns + (present_time - current_time)) / 1e9;
    [m_render_cmdbuf presentDrawable:m_layer_drawable atTime:mach_present_time];
  } else {
    [m_render_cmdbuf presentDrawable:m_layer_drawable];
  }
  ```

  `m_features.timed_present = true` (`mm:410`). Combined with
  emulation-rate slewing (the same shape as PCSX2's PR #5488), this is
  **the proven Apple Silicon recipe for jitter-free pacing without VRR.**
  Highest-leverage adoption for xemu.

- **`m_features.feedback_loops = framebuffer_fetch || supports_barriers`**
  (`mm:387-410`). For sampling a render target while it's bound. NV2A
  games occasionally do this; **xemu must support it.** Apple1+ uses
  framebuffer fetch; Intel-Mac fallback uses barrier-based pass split.

- **`m_features.pipeline_cache = false`** (`mm:412`). DuckStation builds
  pipelines synchronously and doesn't try to persist them; PS1 has too
  few variants for that to matter. xemu's NV2A pipeline space is much
  larger; we **cannot** adopt this approach.

- **`m_features.shader_cache = true`** (`mm:411`). Persists MSL source
  (or compiled metallib) keyed by GLSL hash, so cold launches don't pay
  the spirv-cross cost. **This is the right pipeline-persistence answer
  for xemu** rather than `MTLBinaryArchive`.

- **`prefer_unused_textures = true`** — DuckStation pools `MTLTexture`s
  rather than freeing/reallocating to avoid the Metal driver's hidden
  allocation cost.

---

## D. MoltenVK (`MoltenVK/MoltenVK/`)

Translation layer; not a renderer architecture for us. Useful only as a
window into how Metal handles Vulkan idioms.

### Findings

- **Triangle-fan emulation via compute pre-pass**
  (`MVKCmdDraw.mm:177-214, 967-1066`). Allocates a temp index buffer of
  `_vertexCount * sizeof(uint32_t)`, fills it linearly, submits as
  indexed indirect. For triangle-fan-with-indices, a compute shader
  emits `(0, i, i+1)` triplets. **Adds barrier + compute dispatch per
  draw.** xemu's CPU-side `XEMU_NATIVE_QUAD` index expansion is cheaper
  for our geometry counts; reject the compute pre-pass.

- **Argument buffers** (`MVKDescriptorSet.{h,mm}`). MVK packs Vulkan
  descriptors into Metal argument buffers when the device supports
  `MTLArgumentBuffersTier2` (Apple Silicon Tier 2 universal). For NV2A
  with at most 4 textures + ~10 uniform-buffer slots per draw, plain
  `setFragmentTextures:withRange:` (Dolphin's pattern) is simpler.

### KosmicKrisp

Source not on a public repo as of 2025-12 alpha. No source-level
findings; conformance and feature claims from `lunarg.com`. Treat as a
measurement track only.

---

## E. xemu's existing Vulkan renderer (`hw/xbox/nv2a/pgraph/vk/`)

Structurally the closest precedent for what the Metal backend will look
like. ~10,500 LOC across 17 files.

### Renderer-interface dispatch (`hw/xbox/nv2a/pgraph/pgraph.h:108-136`)

22 ops, registered via `__attribute__((constructor))` calling
`pgraph_renderer_register(&pgraph_*_renderer)`. Three backends live in
the tree today (`null/`, `gl/`, `vk/`), each ~1–10k LOC, plus the empty
`mtl/` directory awaiting this slice.

### Init sequence (`vk/renderer.c:36-67`)

```c
pgraph_vk_init_instance(pg, errp);          // VkInstance, VkDevice, VmaAllocator
pgraph_vk_init_command_buffers(pg);
pgraph_vk_init_buffers(d);
pgraph_vk_init_surfaces(pg);
pgraph_vk_init_shaders(pg);
pgraph_vk_init_pipelines(pg);
pgraph_vk_init_textures(pg);
pgraph_vk_init_reports(pg);
pgraph_vk_init_compute(pg);
pgraph_vk_init_display(pg);
pgraph_vk_update_vertex_ram_buffer(...);
pgraph_vk_determine_gpu_properties(d);
```

The Metal init mirrors this almost one-for-one (no `init_compute` needed
in the first slice; Apple7+ doesn't need a compute pre-pass for
geometry).

### `PipelineKey` (`vk/renderer.h:64-71`)

Already a POD struct hashed/compared with `memcmp`-equivalent equality.
Direct template for the Metal `PipelineKey`.

### `BUFFER_VERTEX_RAM` (vk/renderer.h:83-96, vk/buffer.c)

64 MiB buffer mapped 1:1 over guest VRAM. Vertex DMA reads point into
this buffer at the same offset the guest sees. **Ports beautifully to
Metal `Shared` mode**: a single `MTLBuffer` of size 64 MiB, mapped into
the QEMU address space, all guest writes cache-coherent to the GPU. The
`uploaded_bitmap` (`renderer.h:376`) tracks which 4 KiB pages need an
explicit barrier before GPU read.

### Display / GL interop (`vk/display.c:1093 LOC`)

Uses `gloffscreen` + GL interop to re-import the Vulkan-rendered texture
into the OpenGL ImGui context. **`HAVE_EXTERNAL_MEMORY` is the gating
macro**; on Linux this uses `VK_KHR_external_memory_fd`, on Windows
`VK_KHR_external_memory_win32`, on macOS there is no equivalent and the
fallback is a slow CPU download (`vk/renderer.c:200-204`).

For Metal, the equivalent is **`IOSurfaceRef`** which is natively
bridgeable to both Metal (`[device newTextureWithDescriptor:iosurface:plane:]`)
and the GL ImGui draw layer (`CGLTexImageIOSurface2D`). This may unblock
zero-copy interop cleanly. Alternative: move the entire main UI window
to Metal, using `imgui_impl_metal.mm` (already present in
`subprojects/imgui/backends/`).

---

## Synthesis: what xemu should adopt

### Adopt directly

1. **POD `PipelineKey` + `fast_hash` + `memcmp` equality.** xemu vk
   already uses this. Carry over.
2. **`Lru`-backed pipeline cache, ~2048 entries, LRU eviction.** xemu vk
   already uses this. Carry over.
3. **One render command buffer per frame batch; one render encoder per
   framebuffer; lazy state apply via dirty bitmask.** Dolphin pattern.
4. **`BufferPair`-style upload pool with `UsageTracker` ring allocator;
   `Shared|WriteCombined` cpubuffer; on Apple Silicon, no separate
   Private gpubuffer.**
5. **`presentDrawable:atTime:` with `mach_absolute_time +
   (vblank_interval_ns - elapsed)` deadline.** DuckStation. Pair with
   PCSX2's emulation-rate slewing.
6. **Static expand-index buffer for points / wide lines.** PCSX2.
7. **CPU-side `IndexGenerator` for triangle fans / quads / line strips.**
   Dolphin. xemu already has the GL-side equivalent in
   `pgraph_native_quad`; reuse the same expansion routine.
8. **MSL function-constant specialization for VS/PS variants.** PCSX2.
9. **Framebuffer fetch `[[color(0)]]` for register-combiner reads of
   destination color.** Dolphin/PCSX2/DuckStation. Gated on
   `MTLGPUFamilyApple1`.
10. **`MTLPipelineOptionArgumentInfo` reflection.** Dolphin. Skip unused
    binds.
11. **`setShouldMaximizeConcurrentCompilation:YES`**, guarded by
    `respondsToSelector:`. Dolphin. Single most important Apple Silicon
    knob.
12. **MSL-source cache keyed by `ShaderState` hash for cold-launch
    warmup.** DuckStation. Avoids the spirv-cross step on subsequent
    runs.

### Reject

1. **`MTLBinaryArchive` for pipeline persistence.** Limited macOS
   coverage as of 2026; large breakage surface. MSL-string caching
   achieves the same goal more portably. (Note: this contradicts the
   Phase 4f sub-deliverable as currently worded in `strategy.md`. The
   plan should be amended to say "MSL-source cache keyed by NV2A state
   hash" not "Metal pipeline state cache via `MTLBinaryArchive`.")
2. **PCSX2's `SpinManager`.** Steals CPU power on unified-package Apple
   Silicon.
3. **Compute pre-pass for triangle-fan emulation.** CPU-side index
   expansion is cheaper for NV2A's geometry counts.
4. **Hand-written MSL.** Combinatorial explosion of NV2A combiner
   variants. Generate from GLSL.
5. **MoltenVK / KosmicKrisp as the shipping renderer.** 20–30 % CPU
   overhead vs native Metal in Dolphin's measurements; FBFetch and
   pacing benefits exposed indirectly. Measurement track only.
6. **DuckStation's "no pipeline cache".** Fine for PS1; NV2A has too
   many variants.

### Novel pieces xemu Metal needs (no reference emulator has them)

1. **NV2A register combiners → MSL fragment shader generation.** Existing
   GLSL combiner generator (`hw/xbox/nv2a/pgraph/glsl/psh.c:1832 LOC`)
   produces GLSL; the Metal path generates GLSL → SPIR-V → MSL via
   spirv-cross at first; profile against direct MSL generation later if
   spirv-cross compile cost dominates.
2. **NV2A vertex programs (NVidia 0x4097 ISA, 136-token register-based).**
   Existing GLSL generator continues to drive both backends; vertex
   format is fixed (16 attributes), so `[[stage_in]]` with
   `MTLVertexDescriptor` works cleanly.
3. **Xbox-style fixed-function blend modes outside `MTLBlendFactor`.**
   Use framebuffer fetch + raster_order_group for register-combiner
   states that read destination color.
4. **Xbox memory aliasing of FB and texture in same draw.** Use
   framebuffer fetch (Apple1+) or `useResource:usage:stages:` with
   barriers (Intel-Mac fallback).
5. **NV097_GET_REPORT visibility-result emulation.**
   `MTLVisibilityResultMode = Counting` on the encoder + one `MTLBuffer`
   for results. Drop-in replacement for the existing `vk/reports.c`.
6. **`vblank_interval_ns`-driven 60 Hz timer + `presentDrawable:atTime:`
   integration.** Two implementation options:
   - Add a new `void (*present_at)(NV2AState *d, uint64_t deadline_mach_ns)`
     op to `PGRAPHRenderer`. Cleaner; exposes to GL backend too.
   - Compute deadline inside `flip_stall` of the Metal renderer.
     Fewer touch points.
   The plan picks the first option in the slice ordering.
7. **ImGui main UI integration.** xemu uses
   `imgui_impl_sdl3` + `imgui_impl_opengl3` today; the Metal path uses
   `imgui_impl_sdl3` + `imgui_impl_metal` (both already in
   `subprojects/imgui/backends/`). The main SDL3 window is created with
   `SDL_WINDOW_METAL` instead of `SDL_WINDOW_OPENGL` (`ui/xemu.c:1034`).

---

## Reference URLs

- Dolphin Metal backend, PR #10754:
  https://github.com/dolphin-emu/dolphin/pull/10754
- Dolphin progress report, July/August 2022:
  https://dolphin-emu.org/blog/2022/09/13/dolphin-progress-report-july-and-august-2022/
- PCSX2 framebuffer-fetch combiner emulation, PR #5630:
  https://github.com/PCSX2/pcsx2/pull/5630
- PCSX2 "Sync to host refresh rate", PR #5488:
  https://github.com/PCSX2/pcsx2/pull/5488
- DuckStation source:
  https://github.com/stenzek/duckstation/blob/master/src/util/metal_device.mm
- RPCS3 async shader compile (skip-the-draw), PR #4876:
  https://github.com/RPCS3/rpcs3/pull/4876
- MoltenVK source: https://github.com/KhronosGroup/MoltenVK
- Tellusim Metal MDI study: https://tellusim.com/metal-mdi/

---

## Research gaps

1. **KosmicKrisp source not on a public GitHub repo as of 2025-12 alpha.**
   No source-level verification of any LunarG implementation claims.
2. **Dolphin's `Source/Core/VideoCommon/AsyncShaderCompiler.cpp` and
   `ShaderCache.cpp` were not deep-read line-by-line.** Recommend reading
   before implementing the Metal-side async compile worker, especially
   `WaitForAsyncCompiler` semantics.
