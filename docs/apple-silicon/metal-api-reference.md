# Native Metal Renderer for xemu — API Reference for Phase 4 Planning

Last updated: 2026-05-02

This document is the API-surface companion to `strategy.md` Phase 4 (4a–4i). It
does not re-derive the architectural decisions in the strategy doc; it spells
out the concrete Metal API contracts, recommended emulator patterns, Apple-
Silicon-specific gotchas, and reference URLs for each topic. Audience:
engineers building the NV2A-to-Metal translation layer.

Conventions: `[device supportsFamily:MTLGPUFamilyAppleN]` is shortened to
"Apple N+". M3 Ultra (the project's primary target) is Apple 9.

---

## 1. Device + Queue Lifecycle, SDL3/CAMetalLayer Integration

**API surface.** `MTLCreateSystemDefaultDevice()` returns the auto-selected
`MTLDevice`. On Apple Silicon there is exactly one GPU, so multi-adapter
enumeration via `MTLCopyAllDevices()` is a no-op concern. Queues are created
with `[device newCommandQueue]` (default capacity 64 in-flight buffers) or
`[device newCommandQueueWithMaxCommandBufferCount:N]`. Queues are thread-safe;
command buffers are not (one CPU thread per buffer at a time). Command buffers
are created with `[queue commandBuffer]` (auto-released-on-commit) or
`[queue commandBufferWithUnretainedReferences]` (caller keeps all referenced
resources alive — useful when ownership is already centrally tracked).

**Recommended pattern for xemu.** One `MTLDevice` (cache the result of
`MTLCreateSystemDefaultDevice()` at NV2A renderer init). One graphics
`MTLCommandQueue` with `maxCommandBufferCount = 8` — the renderer thread
submits at most 1–2 buffers per guest frame at console FPS. A second
compute/blit queue is only justified if we ever do parallel async-compute
texture decode; for the first Metal slice, do not split. One command buffer
per emulated frame is the right granularity (matches the existing `surface.c`
flush boundary). Split into multiple buffers only when a sub-frame
`MTLEvent` signal back to the CPU is needed before the frame ends.

**SDL3 + CAMetalLayer integration.** xemu already creates an SDL3 window
(`ui/xemu.c:1037`). The path is:

```c
SDL_MetalView mv = SDL_Metal_CreateView(window);     // SDL3 (also SDL2 ≥ 2.0.12)
CAMetalLayer *layer = (CAMetalLayer *)SDL_Metal_GetLayer(mv);
layer.device = device;                               // SDL does NOT set this
layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;   // or RGBA16Float for HDR
layer.framebufferOnly = YES;                         // unless reading drawables
layer.maximumDrawableCount = 3;                      // 3=throughput, 2=latency
layer.displaySyncEnabled = YES;                      // NO = tearing/uncapped
```

`SDL_Metal_GetLayer` returns the unretained `CAMetalLayer`; do not release it.
Pair `SDL_Metal_CreateView` with `SDL_Metal_DestroyView` at shutdown. The main
window must be created with `SDL_WINDOW_METAL` instead of the current
`SDL_WINDOW_OPENGL` flag (`ui/xemu.c:1034`).

**Threading.** Drawable acquisition (`[layer nextDrawable]`) blocks if
`maximumDrawableCount` are in flight; never call it from the iothread.
Recording and committing must both happen on a single thread per command
buffer, but multiple buffers can be recorded in parallel on different
threads. For xemu, do all Metal work on the renderer thread; the iothread
only signals via an `MTLSharedEvent` (see §8). `[layer nextDrawable]` may
return nil after a 1-second timeout — handle and skip the frame rather
than asserting.

**Gotchas.** `MTLCreateSystemDefaultDevice` is `nil` in headless processes;
guard. Re-creating queues per frame is a profiler red flag. Command buffers
retain their command queue, command queues retain their device — drop them
in reverse order at shutdown.

References:
- [MTLCommandQueue](https://developer.apple.com/documentation/metal/mtlcommandqueue)
- [newCommandQueueWithMaxCommandBufferCount:](https://developer.apple.com/documentation/metal/mtldevice/1433433-newcommandqueuewithmaxcommandbuf)
- [SDL2 SDL_Metal_CreateView](https://wiki.libsdl.org/SDL2/SDL_Metal_CreateView)
- [Command Organization and Execution Model](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Cmd-Submiss/Cmd-Submiss.html)

---

## 2. Render Pipelines (MTLRenderPipelineState)

**API surface.** `MTLRenderPipelineDescriptor` fields that matter for NV2A:

- `vertexFunction`, `fragmentFunction` — `id<MTLFunction>` from
  `[library newFunctionWithName:]` or specialized via
  `newFunctionWithName:constantValues:error:`.
- `vertexDescriptor` — `MTLVertexDescriptor` with up to 31 attributes;
  layouts pick stride and `MTLVertexStepFunction` (PerVertex / PerInstance /
  Constant). Required when the vertex shader uses `[[stage_in]]`.
- `colorAttachments[i]` — pixel format, blend equations
  (`rgbBlendOperation` / `alphaBlendOperation`), source/dest factors,
  `writeMask`, `blendingEnabled`.
- `depthAttachmentPixelFormat`, `stencilAttachmentPixelFormat` — must match
  the `MTLRenderPassDescriptor` actually used.
- `rasterSampleCount` — MSAA sample count; must match the multisample
  render-target.
- `inputPrimitiveTopology` —
  `MTLPrimitiveTopologyClassPoint`/`Line`/`Triangle`/`Unspecified`. Set
  explicitly for 4f-style pipeline-cache hashing.
- `alphaToCoverageEnabled`, `alphaToOneEnabled`, `rasterizationEnabled`.

Build via `[device newRenderPipelineStateWithDescriptor:error:]` (sync,
blocks the calling thread on shader compile) or
`newRenderPipelineStateWithDescriptor:completionHandler:` (async — the only
acceptable form on the renderer thread). Metal 3 also accepts
`MTLPipelineOption` flags including `MTLPipelineOptionFailOnBinaryArchiveMiss`
to enforce cache hits.

**Function constants.** `MTLFunctionConstantValues` is the right
specialization tool for NV2A's combinatorial render-state explosion
(combiner stages, alpha test, fog mode, two-sided lighting). Set named
constants at pipeline build time:

```objc
MTLFunctionConstantValues *fc = [MTLFunctionConstantValues new];
bool alpha_test = !!nv2a_state.alpha_test_enabled;
[fc setConstantValue:&alpha_test type:MTLDataTypeBool withName:@"kAlphaTest"];
uint32_t fog_mode = nv2a_state.fog_mode;
[fc setConstantValue:&fog_mode type:MTLDataTypeUInt withName:@"kFogMode"];
id<MTLFunction> frag = [lib newFunctionWithName:@"nv2a_frag"
                              constantValues:fc error:&err];
```

In MSL: `constant bool kAlphaTest [[function_constant(0)]];`. The MSL
compiler dead-strips branches gated on a function constant, so one source
yields many specialized binaries.

**Pipeline cache (Phase 4f).** `MTLBinaryArchive` is the persistence
mechanism. Pattern: open or create the archive at startup, set
`descriptor.binaryArchives = @[archive]` before each pipeline build. After
successful builds, call
`[archive addRenderPipelineFunctionsWithDescriptor:error:]`, then
`[archive serializeToURL:error:]` at clean shutdown (or periodically). On
the next run, the archive is consulted before MTLCompiler is invoked.
WWDC22-10102 introduced the offline `metal-tt` toolchain so a metallib can
be shipped pre-warmed.

**MTLLibrary serialization.** Compile `.metal` source offline with
`metal -c foo.metal -o foo.air` then `metallib foo.air -o foo.metallib`,
ship the `metallib` in the app bundle, load with
`[device newLibraryWithURL:error:]`. Online compile from source
(`newLibraryWithSource:options:error:`) is acceptable for runtime-generated
NV2A shaders but each compile costs 5–50 ms wall-clock and dispatches to
the out-of-process `MTLCompilerService` — see §12.

**Gotcha.** Pipeline state is hashed by descriptor identity. Tiny variations
(a different `colorAttachments[0].writeMask` for a depth-only prepass, an
unset `inputPrimitiveTopology`, a `rasterSampleCount` mismatch with the
pass) all force fresh compiles. The 4f cache must hash at the *NV2A
render-state* level, not the Metal-descriptor level, or it will thrash.
Also: do not destroy MTLRenderPipelineState objects between frames — keep
them in a hash table keyed by NV2A state hash.

References:
- [MTLRenderPipelineDescriptor](https://developer.apple.com/documentation/metal/mtlrenderpipelinedescriptor)
- [MTLFunctionConstantValues](https://developer.apple.com/documentation/metal/mtlfunctionconstantvalues)
- [MTLBinaryArchive](https://developer.apple.com/documentation/metal/mtlbinaryarchive)
- WWDC20-10615 "Build GPU binaries with Metal"
- WWDC21-10229 "Discover compilation workflows in Metal"
- WWDC22-10102 "Target and optimize GPU binaries with Metal 3"

---

## 3. MSL Shader Compilation

**API surface.** Three roads to a `MTLLibrary`: (1) ship a precompiled
`.metallib` and load with `newLibraryWithURL:`; (2) compile MSL at runtime
with `[device newLibraryWithSource:options:error:]` (sync) or
`newLibraryWithSource:options:completionHandler:` (async); (3) reconstitute
from a serialized `MTLBinaryArchive`. `MTLCompileOptions` controls
`languageVersion` (`MTLLanguageVersion3_2`), `fastMathEnabled`,
`preserveInvariance`, preprocessor macros.

**Online compile cost on Apple GPUs.** The `newLibrary*Source*` path is
forwarded to the out-of-process `MTLCompilerService` daemon; cold compiles
run 5–50 ms per shader. xemu issue #1466 documents the worst case:
MTLCompilerService has crashed during heavy first-launch shader bursts.
Mitigations: (a) async compile + ubershader fallback (Phase 4e, mirroring
Dolphin's `bSupportsBackgroundCompiling`); (b) ship a base `.metallib` of
common shaders pre-built; (c) persist `MTLBinaryArchive` between sessions
(4f).

**Pipeline-variant explosion sources for NV2A.** Register-combiner stage
count + per-stage operand selection × alpha test (8 funcs × ref) × fog (3
modes) × two-sided lighting × number of texture units enabled ×
point-sprite / line-stipple / polygon-mode. Naive emit-per-state explodes
into thousands of shaders.

**Reduction strategy.** Use function constants (§2) for low-cardinality
booleans (alpha test on/off, fog enabled, two-sided lighting). Use uniform
branches keyed off `device int *kNV2AState [[buffer(N)]]` for
high-cardinality but rarely-changing selection (combiner operand indices).
The MSL spec explicitly notes that function-constant-gated dead code is
stripped at specialization time, so a 6-function-constant pipeline does
not pay runtime branch cost. Match Dolphin and PPSSPP by keying the
per-game cache on a 64-bit hash of the *normalized* NV2A state, not on
raw register dumps.

**MSL specifics for NV2A.**

- Buffer binding: `device const Vert *v [[buffer(0)]]`. Use
  `constant T &cb [[buffer(N)]]` for read-only small uniforms — the
  `constant` address space is in the constant buffer cache, faster than
  `device`.
- Vertex inputs: `[[stage_in]]` with `[[attribute(N)]]` for NV2A's fixed
  vertex stream layouts. For VS-Expand point-sprite/wide-line emulation
  (Phase 4d), drop `[[stage_in]]` and use `vertex_id` + `instance_id` to
  index into a `MTLStorageModePrivate` index buffer.
- Fragment color: `float4 c [[color(0)]]` as fragment output. The same
  attribute as fragment *input* with `[[color(0)]]` becomes framebuffer
  fetch (§5).
- Depth output: `[[depth(less)]]`, `[[depth(greater)]]`, or `[[depth(any)]]`
  to declare conservative direction; required when the fragment shader
  writes `gl_FragDepth`-equivalent. Use `[[depth(less)]]` for the
  Metal-side native-tri-depth path so HiZ is preserved.
- `discard_fragment()` for alpha-test failures. Free on Apple GPU
  (no early-Z penalty before discard, because TBDR runs the shader after
  visibility).
- `[[raster_order_group(0)]]` on a fragment-input `[[color(0)]]` makes
  blending sequential per pixel — the right tool for NV2A combiner blends
  that don't fit Metal fixed-function (§5).

```metal
#include <metal_stdlib>
using namespace metal;
constant bool kAlphaTest [[function_constant(0)]];
constant uint kFogMode   [[function_constant(1)]];

struct V2F { float4 pos [[position]]; float4 col; float fog; };
struct FragOut { float4 color [[color(0)]]; float depth [[depth(less)]]; };

fragment FragOut nv2a_frag(V2F in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]],
                           float4 fb_in [[color(0), raster_order_group(0)]])
{
    float4 c = in.col * tex.sample(smp, /* uv */ float2(0));
    if (kAlphaTest && c.a < 0.5) discard_fragment();
    if (kFogMode == 1) c.rgb = mix(c.rgb, float3(0.5), in.fog);
    // Custom blend that doesn't fit MTL fixed-function:
    c.rgb = c.rgb * fb_in.rgb;
    return { c, in.pos.z };
}
```

References:
- [MTLCompileOptions](https://developer.apple.com/documentation/metal/mtlcompileoptions)
- [xemu#1466 MTLCompilerService](https://github.com/xemu-project/xemu/issues/1466)

---

## 4. Buffers and Memory

**API surface.** `MTLResourceOptions` combines a storage mode + CPU cache
mode + hazard tracking mode:

- Storage modes: `Shared` (CPU+GPU coherent in unified memory), `Private`
  (GPU-only; host upload via blit), `Managed` (separate CPU/GPU copies —
  *do not use on Apple Silicon*), `Memoryless` (tile memory only; textures
  only).
- CPU cache: `MTLResourceCPUCacheModeDefaultCache` or
  `MTLResourceCPUCacheModeWriteCombined`. Write-combined for upload-only
  buffers (CPU writes, never reads).
- Hazard tracking: `MTLResourceHazardTrackingModeUntracked` skips Metal's
  automatic dependency tracking; required when doing own fence/event
  synchronization, and a small CPU win.

**Apple Silicon implications.** Unified memory means `Shared` is essentially
"give me the bytes" with no host upload cost — the GPU reads directly.
*Never use Managed on Apple Silicon.* Dolphin removed its
`bUseUnifiedMemory` toggle (PR #10754) for exactly this reason. `Private`
is still meaningful: it enables hardware *lossless compression* of
textures (color, depth, ASTC) which `Shared` does not, because the CPU
side would see compressed data.

**Recommended pattern for xemu.**

- **NV2A guest memory mirror.** A single large `Shared` `MTLBuffer`
  aliasing the guest RAM region, write-combined CPU cache. The TCG thread
  already has the bytes; mapping them as Metal-readable is the cheapest
  path.
- **Streaming uniform/staging ring.** Triple-buffered (3 × frame slot)
  `Shared` ring of ~16 MB. Write into slot N for frame N, fence slot N+3
  with the per-frame `MTLSharedEvent`. `frame_idx % 3` selects the slot;
  the renderer thread writes, command buffer `addCompletedHandler` signals
  event value=`frame_idx`, the writer waits-on event ≥ `frame_idx-2`
  before reusing the slot.
- **Per-surface render targets.** `Private`. Lossless compression buys
  ~30% of bandwidth back at zero cost.
- **MTLHeap** for textures. Two heaps: one `MTLHeapTypeAutomatic` `Private`
  for non-aliasable assets (textures, vertex buffers); a second for
  transient render-target aliasing (call `[texture makeAliasable]` after a
  surface goes out of use). Per Apple docs, do *not* mix MSAA and
  depth-compressed textures with normal color textures in the same heap —
  separate them. NV2A texture sizes are small (often 256×256 or 512×512),
  so suballocating from a 64 MB or 128 MB heap eliminates per-texture
  allocation syscall cost.

**Argument buffers.** Tellusim's
[Metal MDI study](https://tellusim.com/metal-mdi/) showed argument buffers +
ICB win only when there are many small draws. NV2A draws are small but only
1k–30k per frame — the per-draw ObjC bind cost is not the bottleneck.
*Defer argument buffers until profiling proves CPU encoding overhead.*
The strategy doc's "ruled-out" section reaches the same conclusion.

```objc
id<MTLBuffer> ring = [device newBufferWithLength:16*1024*1024
    options:MTLResourceStorageModeShared|MTLResourceCPUCacheModeWriteCombined
           |MTLResourceHazardTrackingModeUntracked];
id<MTLHeap> texHeap;
{
  MTLHeapDescriptor *d = [MTLHeapDescriptor new];
  d.size = 128*1024*1024;
  d.storageMode = MTLStorageModePrivate;
  d.type = MTLHeapTypeAutomatic;
  texHeap = [device newHeapWithDescriptor:d];
}
```

References:
- [MTLStorageMode](https://developer.apple.com/documentation/metal/mtlstoragemode)
- [Choosing a resource storage mode for Apple GPUs](https://developer.apple.com/documentation/metal/choosing-a-resource-storage-mode-for-apple-gpus)
- [MTLHeap](https://developer.apple.com/documentation/metal/mtlheap)
- WWDC22-10101 "Go bindless with Metal 3"

---

## 5. Textures + Sampling

**API surface.** `MTLTextureDescriptor` (factory:
`texture2DDescriptorWithPixelFormat:width:height:mipmapped:`) has
`pixelFormat`, `width/height/depth`, `mipmapLevelCount`, `arrayLength`,
`sampleCount`, `usage` (`ShaderRead | RenderTarget | ShaderWrite |
PixelFormatView`), `storageMode`. Build with `[device newTextureWithDescriptor:]`
or `[heap newTextureWithDescriptor:]`. Texture views:
`[tex newTextureViewWithPixelFormat:textureType:levels:slices:]`. Set
`usage |= MTLTextureUsagePixelFormatView` at original creation time, otherwise
views fail. `MTLSamplerState` is built from `MTLSamplerDescriptor` (filters,
addressing, lod clamp, compare function).

**NV2A-relevant pixel formats.**
- Color: `BGRA8Unorm` / `BGRA8Unorm_sRGB` (drawable), `RGBA8Unorm`,
  `RGBA16Float` (HDR composition / MetalFX), `R8Unorm`.
- Depth/stencil: `Depth32Float_Stencil8` (universal). `Depth16Unorm`
  matches NV2A 16-bit depth modes. **Do not use `Depth24Unorm_Stencil8`** —
  Apple Silicon does not support it; the format silently substitutes to
  `Depth32Float_Stencil8`, surprising ports.
- Compressed: BC1–7 are *supported on Apple Silicon* (Apple7+) for Xbox-era
  DXT data.

**Lossless compression.** Apple GPUs apply lossless compression to color
render targets, MSAA, and depth automatically — *only when storage mode is
`Private`*. `Shared` textures bypass compression (CPU would see compressed
bytes). Practical implication: NV2A surfaces (color RT, depth RT,
intermediate render-to-texture) all live in `Private`; only the final
present texture and any "read back to guest memory" surfaces need a
`Shared` staging path.

**Sampler caching.** Sampler creation is cheap; cache keyed on the NV2A
texture-stage state: address mode × filter × LOD bias × compare-func.
Expect ≤ 64 unique samplers per game; pre-build them all at PGRAPH init.

**Texture-to-texture moves.** `MTLBlitCommandEncoder` for
copy/clear/mipgen/buffer-to-texture. NV2A surface-to-surface and
surface-to-texture flows map directly:

```objc
id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
[blit copyFromTexture:src toTexture:dst];          // full-image copy
[blit generateMipmapsForTexture:dst];              // hardware mip gen
[blit synchronizeResource:sharedTex];              // no-op on Apple Silicon
[blit endEncoding];
```

**Framebuffer fetch (Phase 4c).** This is the right NV2A-register-combiner
emulation lever. On Apple GPUs (Apple1+) the fragment shader can read the
current framebuffer pixel by declaring `[[color(0)]]` as an *input*
parameter. Combine with `[[raster_order_group(0)]]` for ordered access
when overlapping primitives need deterministic blending. Runtime check:
`[device supportsFamily:MTLGPUFamilyApple1]` — true on every Apple Silicon
Mac (M1+), false on Intel + AMD discrete macOS. Build the Apple-fast path
and a barrier-based fallback (extra render pass) for Intel Macs as Phase
4c specifies.

References:
- [MTLPixelFormat](https://developer.apple.com/documentation/metal/mtlpixelformat)
- [Tailor your apps for Apple GPUs and TBDR](https://developer.apple.com/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering)
- [areRasterOrderGroupsSupported](https://developer.apple.com/documentation/metal/mtldevice/arerasterordergroupssupported)
- Tech Talk 605 "Metal 2 on A11 - Raster Order Groups"
- DuckStation `metal_device.mm:387-410`; PCSX2 PR #5630

---

## 6. MSAA + Resolve

**API surface.** `MTLRenderPassDescriptor.colorAttachments[i]` controls
per-attachment load/store: `loadAction` ∈ {Load, Clear, DontCare};
`storeAction` ∈ {Store, MultisampleResolve, StoreAndMultisampleResolve,
DontCare}. The MSAA color attachment uses `texture` (multisample) and
`resolveTexture` (single-sample destination). Pipeline `rasterSampleCount`
must equal the attachment's `sampleCount`. Programmable sample positions
are set on the pass with `[passDesc setSamplePositions:count:]` (Apple7+
— `[device areProgrammableSamplePositionsSupported]`).

**Recommended pattern (the Apple-GPU MSAA recipe).**

1. Create the multisample color texture as `MTLStorageModeMemoryless`.
   TBDR keeps multisample data in tile memory only — never spilled to
   system memory.
2. Set `loadAction = MTLLoadActionClear`,
   `storeAction = MTLStoreActionMultisampleResolve` (or
   `StoreAndMultisampleResolve` if also retaining the multisample copy —
   almost never).
3. The `resolveTexture` is the single-sample `MTLStorageModePrivate` color
   render target. The resolve happens during tile-flush, on chip, with no
   off-chip bandwidth.
4. The pipeline state's `rasterSampleCount` must match (e.g. 4).

```objc
MTLTextureDescriptor *msd = [MTLTextureDescriptor
    texture2DDescriptorWithPixelFormat:fmt width:W height:H mipmapped:NO];
msd.textureType = MTLTextureType2DMultisample;
msd.sampleCount = 4;
msd.storageMode = MTLStorageModeMemoryless;
msd.usage = MTLTextureUsageRenderTarget;
id<MTLTexture> ms = [device newTextureWithDescriptor:msd];

MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
pass.colorAttachments[0].texture        = ms;
pass.colorAttachments[0].resolveTexture = colorRT;
pass.colorAttachments[0].loadAction     = MTLLoadActionClear;
pass.colorAttachments[0].clearColor     = MTLClearColorMake(0,0,0,1);
pass.colorAttachments[0].storeAction    = MTLStoreActionMultisampleResolve;
```

**Why MSAA is "free" on Apple GPUs.** TBDR retains a small N×N tile
(e.g. 32×32) entirely in on-chip SRAM during the fragment phase. MSAA
expands per-pixel sample count from 1 to 4, but only inside tile memory.
The resolve is a tile-local 4→1 reduction at flush, also in tile memory.
*No multisample bytes are ever stored to DRAM.* `Memoryless` storage mode
encodes this contract. Programmable sample positions (Apple7+, available
on M1+) let custom 4× or 8× sample patterns. NV2A never used custom sample
positions; standard `MTLDefaultSamplePositions` are correct.

**Depth/stencil MSAA.** Same pattern: depth attachment is multisample
memoryless; if resolved depth is needed (e.g. for MetalFX, see §9), use
`MTLStoreActionMultisampleResolve` with
`depthResolveFilter = MTLMultisampleDepthResolveFilterMin` (or Max/Sample0).
Per-MSAA-fragment-shader execution is enabled by adding `[[color(0)]]` to
a fragment input or by `setSampleMask:`. Avoid unless NV2A behavior
requires it — per-sample shading is 4× the fragment cost.

References:
- [MTLStoreAction.multisampleResolve](https://developer.apple.com/documentation/metal/mtlstoreaction/mtlstoreactionmultisampleresolve)
- [MTLStorageMode.memoryless](https://developer.apple.com/documentation/metal/mtlstoragemode/memoryless)
- WWDC20-10602 "Harness Apple GPUs with Metal"

---

## 7. Frame Timing + Presentation

**API surface (in priority order).**

1. **`CAMetalDisplayLink`** (macOS 14+ / Metal 3+, WWDC23). The modern
   path. Fires a callback per upcoming refresh with a
   `CAMetalDisplayLinkUpdate` containing `drawable` (pre-acquired),
   `targetPresentationTimestamp`, and `targetTimestamp`. Properties:
   `preferredFrameRateRange` (min/max/preferred), `paused`. *VRR-aware* on
   ProMotion / external Apple displays.
2. **`CAMetalLayer.presentsWithTransaction`** + `[cmdBuf presentDrawable:atTime:]`
   and `[cmdBuf presentDrawable:afterMinimumDuration:]`. The DuckStation
   pattern (`metal_device.mm:2536-2620`): compute `target = mach_absolute_time
   + nominal_frame_period`, present at that absolute time. Works back to
   macOS 10.15.4.
3. **`addPresentedHandler:`** on the drawable. Callback fires with the
   actual presented timestamp; use it to measure presentation jitter and
   update an EWMA frame budget.

**Drawable count and latency.** `CAMetalLayer.maximumDrawableCount` defaults
to 3. Set to 2 for a low-latency mode at the cost of GPU/CPU parallelism.
Set to 3 when the renderer can sustain console FPS; the extra slot absorbs
input-thread or shader-compile hiccups.

**Tearing / uncapped.** Set `CAMetalLayer.displaySyncEnabled = NO` to
disable v-sync on present. Useful for benchmark mode and uncapped-FPS
inspection.

**Mach-absolute deadline pattern (DuckStation).**

```objc
mach_timebase_info_data_t tb; mach_timebase_info(&tb);
uint64_t now = mach_absolute_time();
uint64_t target_ns_from_mach = (uint64_t)now * tb.numer / tb.denom + 16666667;
CFTimeInterval target_s = (CFTimeInterval)target_ns_from_mach / 1e9;
[cmdBuf presentDrawable:drawable atTime:target_s];
```

**Recommended pattern for xemu.** Phase 4a says "use CAMetalDisplayLink for
VRR-aware presentation pacing." Concrete recipe:

- Use `CAMetalDisplayLink` when `@available(macOS 14, *)`; fall back to
  the `presentDrawable:atTime:` mach-time pattern on macOS 13.
- Set `preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 60)` —
  honors guest 30 Hz titles and allows 60+ where supported. Update
  preferred when the guest VBLANK rate changes.
- Pair with the Phase 2.5 emulation-rate slewing — slew the guest clock by
  ≤ 1 % to lock to the actual presentation timestamps reported by
  `addPresentedHandler:`. This eliminates the PGR2 / Rainbow tail-jitter
  from emulation-clock drift.
- `maximumDrawableCount = 3` for production; flip to 2 only when the user
  explicitly opts into a low-latency toggle.

References:
- [CAMetalDisplayLink](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink)
- [present(_:afterMinimumDuration:)](https://developer.apple.com/documentation/metal/mtlcommandbuffer/2806849-present)
- DuckStation `metal_device.mm:2536-2620`
- WWDC23-10125 "Bring your game to Mac, Part 3: Render with Metal"

---

## 8. GPU Synchronization

**API surface.**

- **`MTLFence`** — intra-command-buffer, intra-encoder-boundary. Created
  with `[device newFence]`. Used as `[encoder updateFence:f afterStages:s]`
  and `[encoder waitForFence:f beforeStages:s]`. Cannot cross queue
  boundaries.
- **`MTLEvent`** — intra-device, can cross queue boundaries. Created with
  `[device newEvent]`. Encode `[cmdBuf encodeSignalEvent:e value:v]` and
  `[cmdBuf encodeWaitForEvent:e value:v]`. Per-monotonic-counter semantics.
  CPU cannot wait on `MTLEvent`.
- **`MTLSharedEvent`** — cross-process / cross-device-capable; CPU *can*
  wait via `[event waitUntilSignaledValue:timeoutMS:]` or asynchronous
  `[event notifyListener:atValue:block:]` with an `MTLSharedEventListener`.
  The right primitive for renderer-thread-↔-iothread coordination in xemu.

**Encoder-boundary memory barriers.** Within a single render encoder,
`[renderEnc memoryBarrierWithScope:after:before:]` enforces RAW/WAW
ordering between draws. **Apple GPUs disable after-fragment barriers in
render passes** — Metal validation rejects them. The right pattern for
cross-pass dependencies is *splitting the pass* and relying on Metal's
automatic hazard tracking, or using a `MTLFence` between encoders.
Compute-encoder barriers are different: `memoryBarrierWithResources:` or
`memoryBarrierWithScope:` (allowed mid-encoder for compute).

**Apple-tile-render barriers.** When a render pass writes-then-reads the
same color attachment via framebuffer fetch (§5), no explicit barrier is
needed because raster_order_group enforces per-pixel ordering. Cross-tile
dependencies require a render-pass split.

**Recommended pattern for xemu.**

- One `MTLSharedEvent`, with an incrementing 64-bit counter, used as the
  "frame N completed" signal. Iothread waits on N-2 before reusing the
  staging ring slot for frame N. CPU-side wait via the listener
  (background queue) avoids blocking the iothread.
- Within a frame, rely on Metal's automatic hazard tracking for `Shared`
  resources, and explicit `MTLFence` only for
  `MTLResourceHazardTrackingModeUntracked` ring buffer slices when
  profiling proves the global-tracking cost is the bottleneck.
- Avoid `MTLEvent` unless multi-queue appears; for a single-queue
  renderer it adds API surface without value.

```objc
id<MTLSharedEvent> frameEvent = [device newSharedEvent];
__block uint64_t signalValue = 1;

[cmdBuf encodeSignalEvent:frameEvent value:signalValue];
MTLSharedEventListener *l = [[MTLSharedEventListener alloc] init];
[frameEvent notifyListener:l
                   atValue:signalValue
                     block:^(id<MTLSharedEvent> e, uint64_t v) {
    nv2a_renderer_frame_done_async(v);
}];
[cmdBuf commit];
signalValue++;
```

References:
- [About synchronization events](https://developer.apple.com/documentation/metal/about-synchronization-events)
- [MTLSharedEvent](https://developer.apple.com/documentation/metal/mtlsharedevent)
- [Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)

---

## 9. MetalFX

**API surface.**

- **`MTLFXSpatialScaler`** (macOS 13+). Inputs: low-res input texture,
  output texture. No motion vectors required. Lightweight — typically
  ~0.5–1 ms on M1.
- **`MTLFXTemporalScaler`** (macOS 13+). Inputs: low-res color, depth
  (resolved if MSAA), motion vectors (low-res, 16-bit signed), exposure
  (optional). Outputs: high-res color. Requires the engine to apply
  *jittered* sub-pixel offsets per frame and to feed the same jitter to
  the scaler. Roughly 2–4 ms on M1, less on M3+. Significantly higher
  quality than spatial.
- **`MTLFXTemporalDenoisedScaler`** (macOS 14+). Ray-traced denoising;
  not relevant.

Both scalers encode onto an `MTLCommandBuffer` like a compute pass via
`[scaler encodeToCommandBuffer:]`.

**For an emulator with no native motion vectors.** NV2A had no concept of
motion vectors. Using the temporal scaler requires *synthesizing* them,
which on a fixed-camera title is feasible (re-project last frame's
screen-space depth using the title's view-matrix delta) but on
dynamic-camera + dynamic-object scenes is hard and produces ghosting.
Recommendation:

1. **Phase 4 v1: spatial only.** `MTLFXSpatialScaler` for an
   upscale-from-720p-to-1440p path. Drop-in, no NV2A pipeline changes,
   low CPU/GPU cost, visibly better than bilinear blit.
2. **Phase 4 v2 experiment: temporal with synthetic vectors.** Only for
   titles where camera-only re-projection works (cockpit-cam racers —
   Crimson Skies, PGR2 might be acceptable). Needs jitter injection.
3. **Don't ship temporal as the default.** The risk of ghosting on FPS
   games like Rainbow Six 3 is high.

**Apple's recommended integration order.**

```
Render at low res (with jitter for temporal) → Resolve MSAA →
MTLFXScaler (temporal needs depth+motion) → Optional sharpening pass →
Tone-map / SDR↔HDR → Present to drawable
```

The scaler must run *before* tone-mapping and *after* MSAA resolve. Output
should be in linear color space; convert to sRGB at present.

```objc
MTLFXSpatialScalerDescriptor *sd = [MTLFXSpatialScalerDescriptor new];
sd.colorTextureFormat = MTLPixelFormatRGBA16Float;
sd.outputTextureFormat = MTLPixelFormatBGRA8Unorm_sRGB;
sd.inputWidth = 1280;  sd.inputHeight = 720;
sd.outputWidth = 2560; sd.outputHeight = 1440;
sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
id<MTLFXSpatialScaler> s = [sd newSpatialScalerWithDevice:device];
s.colorTexture = lowResColor;
s.outputTexture = highResColor;
[s encodeToCommandBuffer:cmdBuf];
```

References:
- [MTLFXSpatialScaler](https://developer.apple.com/documentation/metalfx/mtlfxspatialscaler)
- [MTLFXTemporalScaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscaler)
- WWDC22-10103 "Boost performance with MetalFX Upscaling"

---

## 10. Frame Capture + Profiling

**API surface.**

- **`MTLCaptureManager`** (`+sharedCaptureManager`). Methods:
  `startCaptureWithDescriptor:error:`, `stopCapture`,
  `supportsDestination:`. Destinations:
  `MTLCaptureDestinationDeveloperTools` (Xcode),
  `MTLCaptureDestinationGPUTraceDocument` (`.gputrace` file).
- **`MTLCaptureScope`** —
  `[manager newCaptureScopeWithDevice:]` or `newCaptureScopeWithCommandQueue:`.
  `beginScope`/`endScope` define the capture window. Set
  `manager.defaultCaptureScope = scope` so Cmd-T in Xcode captures the
  right slice.
- **`MTLCounterSet` / `MTLCounterSampleBuffer`** (macOS 11+). Per-stage
  timing on Apple Silicon; per-draw on Intel/AMD. Sample at
  `MTLCounterSamplingPointAtStageBoundary` (Apple),
  `MTLCounterSamplingPointAtDrawBoundary` (Intel/AMD), `AtBlitBoundary`,
  `AtDispatchBoundary`. Allocate with
  `[device newCounterSampleBufferWithDescriptor:]`, sample with
  `[encoder sampleCountersInBuffer:atSampleIndex:withBarrier:]`, resolve
  with `[buffer resolveCounterRange:]`. Timestamps in nanoseconds;
  calibrate against CPU time with `[device sampleTimestamps:gpuTimestamp:]`.
- **Metal Performance HUD** — runtime overlay. Enable via
  `MTL_HUD_ENABLED=1` env (per-process) or per-app entitlement.
- **Instruments "Metal System Trace"** template — full-system view, ties
  Metal commands to GPU power state, thermals, CPU↔GPU handoffs.

**Capture-by-default for benchmark runs (Phase 4h).** Start a capture
scope at PGRAPH init bound to the snapshot scenes (PGR2 mid-route,
Rainbow scene-entry). Trigger via env var:

```objc
if (getenv("XEMU_METAL_CAPTURE")) {
    MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
    MTLCaptureDescriptor *desc = [MTLCaptureDescriptor new];
    desc.captureObject = device;
    desc.destination = MTLCaptureDestinationGPUTraceDocument;
    desc.outputURL = [NSURL fileURLWithPath:@(getenv("XEMU_METAL_CAPTURE"))];
    [mgr startCaptureWithDescriptor:desc error:&err];
    /* run for N frames */
    [mgr stopCapture];
}
```

**For the existing benchmark harness:** add a `--metal-capture <path>` flag
to `run-benchmark.sh` that exports `XEMU_METAL_CAPTURE`. Add a
`metal_capture_path` field to per-run logs. The `.gputrace` files open in
Xcode (Window → Organizer → GPU Frame Capture) and are reproducible across
runs.

**For continuous profiling without Xcode:** counter sample buffers feed
the existing `xemu-perf:` log line. Add `METAL_VERTEX_US`,
`METAL_FRAGMENT_US`, `METAL_COMPUTE_US` per-interval keys, paralleling the
existing `MSAA_RESOLVE_US_TOTAL`.

**Required entitlement.** Programmatic capture requires
`MetalCaptureEnabled = YES` in `Info.plist` (or environment
`MTL_CAPTURE_ENABLED=1`); by default capture is disabled in non-development
builds. Document this in `automation.md` when the slice lands.

References:
- [MTLCaptureManager](https://developer.apple.com/documentation/metal/mtlcapturemanager)
- [Capturing a Metal workload programmatically](https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-programmatically)
- [MTLCounterSampleBuffer](https://developer.apple.com/documentation/metal/mtlcountersamplebuffer)

---

## 11. GPU Family Detection and Gating

**API surface.** `[device supportsFamily:MTLGPUFamilyAppleN]` returns
BOOL. Older `[device supportsFeatureSet:]` is deprecated; do not use.
Direct property checks for specific features:

- `device.areRasterOrderGroupsSupported` — required for raster_order_group
  (§5).
- `device.areProgrammableSamplePositionsSupported` — Apple7+ programmable
  MSAA positions.
- `device.argumentBuffersSupport` — Tier1 or Tier2. Apple6+ is Tier2.
- `device.supportsFunctionPointers`, `device.supportsDynamicLibraries`.
- `device.maxBufferLength`, `device.maxThreadgroupMemoryLength`.
- `device.supportsFamily:MTLGPUFamilyMetal3` — composite check.

**GPU family map (relevant rows).**

| Family | Hardware |
|---|---|
| Apple1 | A7 — first iOS Metal-capable. *Baseline* for framebuffer fetch / tile shading. On Mac, "true" means Apple GPU (not Intel/AMD). |
| Apple7 | A14 / **M1 / M1 Pro / M1 Max / M1 Ultra**. Programmable sample positions, ray tracing, ICB Tier2. |
| Apple8 | A15/A16 / **M2 / M2 Pro / M2 Max / M2 Ultra**. |
| Apple9 | A17 Pro / **M3 / M3 Pro / M3 Max / M3 Ultra / M4**. Hardware-accelerated mesh shading, dynamic caching. |

**Gating recipe for xemu.**

```objc
typedef struct {
    BOOL is_apple_silicon;        // supportsFamily:Apple1
    BOOL programmable_sample_pos; // supportsFamily:Apple7
    BOOL framebuffer_fetch;       // == is_apple_silicon
    BOOL raster_order_groups;     // areRasterOrderGroupsSupported
    BOOL metal3;                  // supportsFamily:Metal3
    BOOL apple9;                  // M3+
} MetalCaps;
MetalCaps c = {0};
c.is_apple_silicon = [device supportsFamily:MTLGPUFamilyApple1];
c.programmable_sample_pos = [device supportsFamily:MTLGPUFamilyApple7];
c.framebuffer_fetch = c.is_apple_silicon;
c.raster_order_groups = device.areRasterOrderGroupsSupported;
c.metal3 = [device supportsFamily:MTLGPUFamilyMetal3];
c.apple9 = [device supportsFamily:MTLGPUFamilyApple9];
```

Log all of the above at renderer init (Phase 1 deliverable).

**Deployment target.** `CAMetalDisplayLink` requires macOS 14. MetalFX
requires macOS 13. Metal 3 (`MTLBinaryArchive` improvements, fast
resource loading, function stitching) requires macOS 13+. xemu's macOS
deployment target should be lifted to macOS 13 minimum for the Metal
slice — the project's primary targets (M1 / M3 Ultra Macs) all run
macOS 13+. macOS 12 builds keep the OpenGL fallback.

References:
- [MTLGPUFamily](https://developer.apple.com/documentation/metal/mtlgpufamily)
- [Detecting GPU features and Metal software versions](https://developer.apple.com/documentation/metal/gpu_devices_and_work_submission/detecting_gpu_features_and_metal_software_versions)
- [Metal Feature Set Tables PDF](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)

---

## 12. Common Emulator-Specific Pitfalls

**MTLCompilerService stalls (xemu#1466).** Synchronous shader compile hits
an out-of-process daemon; on heavy bursts (e.g. first scene transition in
Halo 2) it has crashed. Mitigations: async compile + ubershader fallback
(Phase 4e); ship pre-warmed `metallib` (Phase 4f); `MTLBinaryArchive`
per-game cache. **Never call sync `newRenderPipelineStateWithDescriptor:error:`
on the renderer thread for an unknown NV2A state — always async +
ubershader.**

**Pipeline thrash from descriptor identity.** Tiny variations (mismatched
`inputPrimitiveTopology`, color attachment count,
`colorAttachments[N].writeMask`, even `pixelFormat` differing from the
actual pass) force fresh compiles. Fix: hash at the *NV2A* level (4f), not
descriptor level. Normalize the NV2A state hash (sort combiner stages,
mask-out don't-care bits) before lookup. Track cache hit/miss with
counters (`METAL_PIPELINE_HITS`, `METAL_PIPELINE_MISSES`).

**Command buffer abandonment cost.** `[cmdBuf commit]` on a buffer with no
encoded work is cheap; `[cmdBuf enqueue]` then never committing leaks the
queue slot. Always commit (or release without enqueue) on the error path.
Detect long-running buffers with `addScheduledHandler:` +
`addCompletedHandler:` time delta.

**Metal API Validation overhead in debug.** `MTL_DEBUG_LAYER=1` (or
running in Xcode) enables full validation; it is *significant* CPU
overhead (often 1.5–3× per encode call). Apps measurably faster outside
Xcode. Configure: ship release builds with `MTL_DEBUG_LAYER=0`; enable
selectively with `MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS=1` etc. when
chasing a specific bug. Document this in `automation.md` under the
Metal slice.

**Implicit GPU↔CPU sync from `Managed` mode on Apple Silicon.** Macros that
"just work" on discrete GPUs become traps on Apple Silicon. The `Managed`
mode triggers an explicit `[buffer didModifyRange:]` requirement and a
CPU↔GPU copy. **On Apple Silicon, never use `Managed`.** Use `Shared`
everywhere a `Managed` buffer would have gone on a discrete GPU. Match
Dolphin's `bUseUnifiedMemory` removal (PR #10754).

**Pipeline state cache thrashing.** If the cache evicts based on LRU
under memory pressure, the renderer re-compiles next frame. Don't evict
pipeline states — they are a few KB each; thousands cost <50 MB. Only
evict on per-game cache invalidation (game-state hash change).

**Drawable starvation.** `[layer nextDrawable]` blocks for up to 1 second
when `maximumDrawableCount` are in flight. If the renderer thread blocks
here, the iothread also blocks (TCG thread → main loop → SDL events).
Always call `nextDrawable` *only* on the renderer thread and *after* there
is actual work to encode. Have a "skip frame" path if it returns nil.

**Argument-buffer trap.** Tellusim's MDI study: argument buffers + ICB
win for many small draws but lose for larger draws. Don't use them by
default — only after profiling proves CPU encode overhead.

**Texture compression and Shared mode.** Setting `usage |= ShaderRead` on
a `Shared` texture *disables* lossless compression. If a texture is
GPU-only, use `Private`.

**Fragment-after-fragment barriers.** Apple GPUs disable these in render
passes. The Metal validation layer rejects them. If fragment-stage A must
complete before fragment-stage B reads, *split the render pass* — or use
framebuffer fetch + raster order groups for the in-pass dependency.

References:
- [Validating your app's Metal API usage](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-api-usage/)
- [xemu#1466](https://github.com/xemu-project/xemu/issues/1466)
- [Tellusim Metal MDI](https://tellusim.com/metal-mdi/)
- Dolphin PR #10754; RPCS3 PR #4876

---

## Key Risks

- **Shader-translation IR design is the single highest-risk slice.**
  GLSL→MSL via SPIR-V Cross is the obvious path but the existing GLSL
  emitter in `hw/xbox/nv2a/pgraph/glsl/` was hand-tuned to GL semantics;
  some constructs (per-primitive depth, polygon-offset slope,
  geometry-shader correctness work in PR #2240) need explicit MSL
  equivalents not naïve translation. Plan for a small fork-specific IR.
- **Pipeline-variant explosion.** NV2A's combinatorial state space
  (combiners × alpha test × fog × textures × point/line/poly mode) can
  produce thousands of unique pipeline states per game. Without
  function-constant specialization + a persistent `MTLBinaryArchive`
  cache, first-launch shader compile bursts will reproduce or worsen
  the OpenGL-on-Metal path's MTLCompilerService pain (xemu#1466).
- **MetalFX temporal upscaling without genuine motion vectors.**
  Synthesizing motion vectors from camera-only reprojection is risky on
  dynamic-camera + dynamic-object titles; ghosting is the failure mode.
  Spatial-only is the safe default; treat temporal as an experiment per
  title.
- **Framebuffer-fetch path divergence.** Apple GPU + raster_order_group
  is the fast path for combiner-style blends; Intel-Mac fallback
  (barrier-based render-pass split) is a second code path with its own
  correctness surface. Phase 4c gating must cover both.
- **`CAMetalDisplayLink` requires macOS 14.** Earlier macOS users get
  the older `presentDrawable:atTime:` path. Two presentation pipelines
  is a maintenance cost.
- **TCG-side judder is not solved by Metal.** strategy.md and
  decision-log are explicit: the 1.3 s class Crimson worst-frame is
  guest-intrinsic + TCG-overhead; Metal will improve presentation
  smoothness and latency but cannot erase guest-engine or TCG-only
  stalls. Don't oversell Metal in shareable-build messaging.
- **MSAA pipeline-variant interaction.** Each `rasterSampleCount` value
  is a distinct pipeline. Shipping `XEMU_GL_MSAA={2,4,8}` translated to
  Metal means up to 3× the pipeline cache size if MSAA is changed at
  runtime. Mitigate by treating MSAA as a session-fixed option (require
  restart to change) OR by accepting the cache size cost.

---

## Recommended Apple Silicon Defaults (quick reference)

| Setting | Value | Rationale |
|---|---|---|
| `CAMetalLayer.pixelFormat` | `BGRA8Unorm_sRGB` | Matches macOS compositor |
| `CAMetalLayer.framebufferOnly` | `YES` | Compositor optimizations |
| `CAMetalLayer.maximumDrawableCount` | `3` (default), `2` for low-latency | 3 = throughput; 2 = 1 frame less latency |
| `CAMetalLayer.displaySyncEnabled` | `YES` (default), `NO` for benchmark | Tearing path only for benchmarking |
| Presentation API | `CAMetalDisplayLink` (macOS 14+), `presentDrawable:atTime:` fallback | VRR-aware |
| `preferredFrameRateRange` | `(30, 120, guest_native)` | Honor 30 Hz title cap |
| `MTLCommandQueue` count | 1 graphics queue | NV2A draw count doesn't justify multi-queue |
| `maxCommandBufferCount` | 8 | 1–2 buffers per emulated frame |
| Guest memory mirror buffer | `Shared` + `WriteCombined` + `Untracked` | Direct GPU read of guest RAM |
| Streaming uniform ring | `Shared`, 16 MB, triple-buffered, `Untracked` | Per-frame uniforms |
| Surface (color/depth) RT | `Private` | Hardware lossless compression |
| MSAA color attachment | `Memoryless`, `MSAA texture type`, `sampleCount=4` | Zero off-chip MSAA bandwidth |
| MSAA store action | `MTLStoreActionMultisampleResolve` | Default; never `Store` for MSAA |
| Depth/stencil format | `Depth32Float_Stencil8` | Universal Apple Silicon support |
| Texture allocation | `MTLHeap`, two heaps | Suballocation |
| Pipeline build | Async (`completionHandler:`) only | Sync compile blocks renderer thread |
| Pipeline specialization | `MTLFunctionConstantValues` for low-cardinality booleans | One MSL source, many specialized binaries |
| Pipeline cache | `MTLBinaryArchive`, per-game, persisted | Phase 4f |
| Synchronization (renderer↔iothread) | `MTLSharedEvent` + `MTLSharedEventListener` | CPU async wait/notify |
| Synchronization (intra-frame) | Metal automatic hazard tracking; `MTLFence` only when `Untracked` | Cheaper than manual events |
| Framebuffer fetch (combiner blends) | `[[color(0)]]` input + `[[raster_order_group(0)]]` | Apple1+ fast path |
| MetalFX | `MTLFXSpatialScaler` default; temporal opt-in per title | Spatial is safe drop-in |
| Argument buffers | **Defer** | Wins only at >> NV2A draw count |
| Frame capture | `MTLCaptureManager` programmatic, env-gated `XEMU_METAL_CAPTURE=path.gputrace` | Phase 4h |
| Counter sampling | `MTLCounterSampleBuffer` at stage boundaries | Surface as `METAL_*_US` perf keys |
| Metal API validation | `MTL_DEBUG_LAYER=0` in shipped builds | 1.5–3× CPU overhead when on |
| Deployment target | macOS 13 (Metal 3 + MetalFX); macOS 14 for `CAMetalDisplayLink`; OpenGL fallback for older | M1+/M3 Ultra targets |
| Storage mode never used | `Managed` | Apple Silicon trap — always `Shared` |

---

## Sources

- Apple Developer Documentation:
  [MTLDevice](https://developer.apple.com/documentation/metal/mtldevice),
  [MTLCommandQueue](https://developer.apple.com/documentation/metal/mtlcommandqueue),
  [MTLRenderPipelineDescriptor](https://developer.apple.com/documentation/metal/mtlrenderpipelinedescriptor),
  [MTLFunctionConstantValues](https://developer.apple.com/documentation/metal/mtlfunctionconstantvalues),
  [MTLBinaryArchive](https://developer.apple.com/documentation/metal/mtlbinaryarchive),
  [MTLStorageMode](https://developer.apple.com/documentation/metal/mtlstoragemode),
  [Choosing a resource storage mode for Apple GPUs](https://developer.apple.com/documentation/metal/choosing-a-resource-storage-mode-for-apple-gpus),
  [MTLHeap](https://developer.apple.com/documentation/metal/mtlheap),
  [MTLPixelFormat](https://developer.apple.com/documentation/metal/mtlpixelformat),
  [MTLStoreAction](https://developer.apple.com/documentation/metal/mtlstoreaction),
  [MTLStorageMode.memoryless](https://developer.apple.com/documentation/metal/mtlstoragemode/memoryless),
  [Tailor your apps for Apple GPUs and TBDR](https://developer.apple.com/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering),
  [areRasterOrderGroupsSupported](https://developer.apple.com/documentation/metal/mtldevice/arerasterordergroupssupported),
  [CAMetalLayer](https://developer.apple.com/documentation/quartzcore/cametallayer),
  [CAMetalDisplayLink](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink),
  [About synchronization events](https://developer.apple.com/documentation/metal/about-synchronization-events),
  [MTLSharedEvent](https://developer.apple.com/documentation/metal/mtlsharedevent),
  [MTLFXSpatialScaler](https://developer.apple.com/documentation/metalfx/mtlfxspatialscaler),
  [MTLFXTemporalScaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscaler),
  [MTLCaptureManager](https://developer.apple.com/documentation/metal/mtlcapturemanager),
  [MTLCounterSampleBuffer](https://developer.apple.com/documentation/metal/mtlcountersamplebuffer),
  [MTLGPUFamily](https://developer.apple.com/documentation/metal/mtlgpufamily),
  [Metal Feature Set Tables PDF](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf),
  [Validating your app's Metal API usage](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-api-usage/).
- WWDC: WWDC20-10602 "Harness Apple GPUs with Metal", WWDC20-10615 "Build
  GPU binaries with Metal", WWDC21-10157 "Discover Metal debugging,
  profiling, and asset creation tools", WWDC21-10229 "Discover compilation
  workflows in Metal", WWDC22-10101 "Go bindless with Metal 3", WWDC22-10102
  "Target and optimize GPU binaries with Metal 3", WWDC22-10103 "Boost
  performance with MetalFX Upscaling", WWDC23-10125 "Bring your game to
  Mac, Part 3: Render with Metal", Tech Talk 605 "Metal 2 on A11 — Raster
  Order Groups", Tech Talk 10001 "Explore Live GPU Profiling with Metal
  Counters".
- SDL: [SDL2 SDL_Metal_CreateView](https://wiki.libsdl.org/SDL2/SDL_Metal_CreateView).
- Third-party: [Tellusim Metal MDI](https://tellusim.com/metal-mdi/),
  DuckStation `metal_device.mm:387-410, 2536-2620`,
  Dolphin PR #10754, PCSX2 PR #5630, RPCS3 PR #4876.
- xemu: [xemu#1466 MTLCompilerService](https://github.com/xemu-project/xemu/issues/1466),
  `strategy.md` Phase 4 (4a–4i),
  `decision-log.md` "2026-05-02: Pivot native Metal".
