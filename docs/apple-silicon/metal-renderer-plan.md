# Native Metal Renderer — Implementation Plan

Last updated: 2026-05-19 night (the new tooling slice has already been
used on the first full PGR2 follow-up. Keep the host-refresh publish
preservation fix, reject the `0x3b58000` display-shape publish
heuristic, keep the new `copy-alias` path for late same-VRAM linear
binds, and treat copied stage-0 `0x3c84000` RTT content as the active
Metal debug target. See
`benchmarks/2026-05-19-pgr2-snapshot-publish-and-rtt-followup.md` and
decision-log "2026-05-19 (night)"). Prior 2026-05-11: M15 default-on remains blocked by
the evidence bundle, not by oracle readiness. Run
`scripts/apple-silicon/m15-bundle-status.py` before any M15 claim; current
result is `verdict=incomplete ok=6 fail=5 missing=4` (2026-05-11 evening,
after the m15-gameplay-* discovery extension and the front-fb fallback
policy decision-log entry). Oracle production evidence and the stable
retail trio are green. PGR2 and Rainbow gameplay visual parity are not
proven; the 2026-05-11 PGR2 capture-source hypothesis is decisively ruled
out, and the 2026-05-19 follow-up moved the active blocker from
"which surface gets published" to RTT correctness in the late composite
path.
M15 requires multiple matched gameplay keyframes from controller routes,
aligned by visual content rather than timestamp, with boot/loading/black/
static/host-UI frames rejected. Crimson paired diff still fails
(`changed_pct=14.7560`), SC2/Halo/PGR2/Rainbow gameplay diffs are missing
or FAIL, PGR2/Rainbow/Crimson p99 jitter gates fail, and cold shader
compile proof is missing. Front-fb fallback policy is now resolved (stays
opt-in while RTT correctness is debugged). The current
app build does not expose QMP/HMP `screendump`; `metal-gl-compare.sh
--trigger flip` uses the GL renderer's `XEMU_GL_SCREENSHOT_PATH` path instead.
Earlier 2026-05-05: SC2 input
route recorded, audio listen-test closed, and Crimson reclassified as a
canonical-recipe canary rather than a missing-config renderer bug.

This document is the staged implementation plan for replacing the
OpenGL backend with a native Metal renderer for the Apple Silicon
shareable build of xemu. It supersedes nothing; it sequences the
existing Phase 4 sub-deliverables (4a–4i) in `strategy.md` into
concrete, gated slices, and adds the architectural decisions reached
during the 2026-05-02 planning session.

Operational note (2026-05-19 night): implementation slices still live
here, but day-to-day renderer investigation now follows the Apple-aligned
workflow in `metal-porting-workflow.md`: validate first, capture the failing
workload, classify correctness / CPU / GPU / overlap, then optimize and
re-measure. This plan tells us **what** to build; the workflow doc tells us
**how** to investigate and land it.

Companion documents written this session:

- `metal-api-reference.md` — Apple Metal API surface, recommended
  patterns, Apple Silicon gotchas.
- `emulator-metal-survey.md` — file-level findings from Dolphin /
  PCSX2 / DuckStation / MoltenVK Metal backends, plus xemu's own
  Vulkan renderer as the structural template.
- `macos-input-research.md` — GameController.framework migration plan,
  independent of the renderer work.

---

## 1. Goals

The shareable Apple Silicon build needs a renderer that delivers, in
priority order:

1. **Correctness ≥ OpenGL.** Every NV2A behavior the OpenGL renderer
   handles correctly today must stay correct. Visual diffs against
   known-good GL screenshots are the regression gate. Project rule #2
   (no shortcuts) and rule #6 (don't permanently revert PR #2240) bind
   here.
2. **1080p+ output with optional MSAA up to 4×.** Already met on the
   GL path via `surface_scale=2` default + opt-in `XEMU_GL_MSAA`. Metal
   must match (4× minimum on Apple Silicon) and ideally exceed (8× via
   programmable sample positions on Apple7+).
3. **Console-native FPS in gameplay** for the tracked titles, including
   the SC2 60 Hz path and the PGR2/Rainbow/Crimson 30 Hz path.
4. **Lower frame-pacing jitter than the GL path** through
   `presentDrawable:atTime:` + emulation-rate slewing, even though the
   1.3 s class TCG-side stutter (judder pillar) cannot be erased by a
   renderer change.
5. **Lower input-to-photon latency** through a clean
   draw-then-present-at-deadline path. Native input (separate track)
   compounds the win.
6. **Maintainability + capture/profiling.** Native Metal frame capture
   (`MTLCaptureManager`) and counter sampling
   (`MTLCounterSampleBuffer`) replace the GL "guess from `xemu-perf:`
   counters" workflow. Pipeline caching via MSL-string persistence
   replaces the OpenGL-on-Metal MTLCompilerService stalls (xemu#1466).
7. **Headroom for graphical enhancements**: MetalFX spatial scaling,
   sharpening, and (optionally per-title) temporal upscaling.

## Non-goals

- Replacing TCG with a custom JIT (already ruled out — strategy.md
  "What we ruled out").
- Erasing the 1.3 s class Crimson stutter (guest-intrinsic + TCG;
  declared "best effort complete" 2026-05-02).
- Shipping MoltenVK or KosmicKrisp as the production renderer.
- Removing the OpenGL backend. GL stays as the reference path,
  correctness oracle, benchmark comparison path, and fallback while
  Metal is built and stabilized. **Eventual deprecation requires its
  own decision-log entry once Metal default-on has shipped and is
  stable for ≥ 30 days of normal use.**
- Cross-platform Metal. Metal is Apple-only; the renderer enum gains a
  `METAL` value compiled in only on `host_os == 'darwin'`.

---

## 2. Current state and readiness

### What's already in place

- **Renderer dispatch interface** (`hw/xbox/nv2a/pgraph/pgraph.h:108-136`,
  `PGRAPHRenderer` struct, 22 ops). Supports adding a Metal backend by
  dropping a new directory next to `gl/`, `vk/`, `null/`.
- **Empty `mtl/` directory** at
  `/Users/jbbrack03/XEMU_MacOS/xemu-fork/hw/xbox/nv2a/pgraph/mtl/`
  awaiting the slice.
- **Renderer enum schema** at `config_spec.yml:227-230`. Adding a
  `METAL` value is a one-line schema change; `gen_config.py` regenerates
  `xemu-config.h`. The selection UI at `ui/xui/main-menu.cc:743` and
  `ui/xui/menubar.cc:178` enumerates the values automatically.
- **xemu-fork is on SDL3** (`ui/xui/common.hh:28-29` includes
  `imgui_impl_sdl3.h`). SDL3 supports `SDL_Metal_CreateView`,
  `SDL_Metal_GetLayer`, and `SDL_WINDOW_METAL`.
- **`subprojects/imgui/backends/imgui_impl_metal.{h,mm}` is already in
  the tree.** HUD port to Metal is straightforward — same
  `ImGui_ImplMetal_Init/NewFrame/RenderDrawData` API as the existing GL3
  backend.
- **Vulkan renderer** (`hw/xbox/nv2a/pgraph/vk/`, ~10,500 LOC across
  17 files) is the structural template. PipelineKey, Lru-backed cache,
  `BUFFER_VERTEX_RAM` mapped over guest VRAM, and the surface manager
  port directly.
- **GLSL shader generators** (`hw/xbox/nv2a/pgraph/glsl/`, ~4,500 LOC
  across 8 files) emit GLSL strings from NV2A register state. Reused by
  GL and Vulkan today; will be reused by Metal via SPIR-V→MSL.
- **Default-on flags that must keep working** (project rule #11): the
  six `XEMU_NATIVE_TRI_DEPTH` / `XEMU_NATIVE_QUAD` / `XEMU_PGRAPH_FAST_READ`
  / `XEMU_TCG_SPLITWX` / `XEMU_TCG_JMP_CACHE_TARGETED` /
  `XEMU_APU_LOCK_RELEASE` flags plus the surface_scale=2 default. The
  triangle/quad slices need a Metal-side port; the rest are
  renderer-agnostic.

### What's missing

- The entire `mtl/` source directory.
- spirv-cross is not currently a project dependency. Vulkan uses
  glslang directly (`vk/glsl.c`) to produce SPIR-V; Metal needs spirv-
  cross to consume that SPIR-V and produce MSL. Add as a Meson
  subproject or pkg-config dependency.
- `meson.build:1855-1869` links OpenGL on Darwin; `2355-2364` gates
  Vulkan to Linux/Windows. Metal needs a parallel Darwin gate +
  Foundation/Metal/MetalKit/QuartzCore framework links.
- `build.sh` packages OpenGL today; will need Metal framework wiring.
- Two presentation pipelines because `CAMetalDisplayLink` requires
  macOS 14. macOS 13 baseline gets `presentDrawable:atTime:` + manual
  `mach_absolute_time` deadline; macOS 14+ gets `CAMetalDisplayLink`.
- No counter-sampling integration. `extract-perf-summary.sh` needs new
  `METAL_*` keys.

### What is verified empirically vs assumed

- **Verified**: SDL3 + ImGui Metal backend present locally; renderer
  dispatch interface; renderer enum mechanism; existence of
  `subprojects/imgui/backends/imgui_impl_metal.mm`; structure of
  Vulkan renderer; GLSL generator complexity (`psh.c` 1832 LOC);
  display path (ui/xemu.c gl_render_frame at line 804 reads a GL
  texture handle from `nv2a_get_framebuffer_surface`); Vulkan
  `HAVE_EXTERNAL_MEMORY` falls back to slow CPU-download on macOS
  (`vk/renderer.c:200-204`).
- **Assumed but not verified**: spirv-cross handles all of xemu's
  generated GLSL constructs cleanly. **The first slice that actually
  ships Metal-side fragment shading must validate this with a small
  test harness before the broader port commits.** A failure here is
  the single largest planning risk (see §6).

---

## 3. Architectural decisions

### 3.1 Renderer selection

**Decision.** Add `METAL` as a new value in
`config_spec.yml:229 display.renderer` enum. Build-time conditionally
register the Metal renderer on `host_os == 'darwin'` only. Default
selection on Apple Silicon Mac:

- macOS 13+: METAL when the slice ships default-on; OPENGL until then.
- macOS < 13: OPENGL (Metal slice not built).

Per project rule #11, every new toggle is `XEMU_*`. Add:

- `XEMU_METAL_FORCE_LEGACY_PRESENT={0,1}` — force `presentDrawable:` only
  (no `atTime:`). For comparison/diagnosis.
- `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH={0,1}` — force the
  barrier-based pass-split fallback. Apple Silicon path can use this
  for measurement.
- `XEMU_METAL_DISABLE_LOSSLESS_COMPRESSION={0,1}` — allocate render
  targets `Shared` instead of `Private` (disables hardware lossless
  compression). For correctness-vs-perf bisection.
- `XEMU_METAL_PIPELINE_CACHE={0,1}` — opt-out the MSL-source persistent
  cache. For warm-vs-cold compile measurement.
- `XEMU_METAL_CAPTURE=path.gputrace` — programmatic frame capture (Phase
  4h).
- `XEMU_METAL_VALIDATION={0,1}` — enable Metal API validation
  programmatically (`MTL_DEBUG_LAYER=1`-equivalent).

### 3.2 Display / UI integration

**Decision.** Move the main xemu window from `SDL_WINDOW_OPENGL` to
`SDL_WINDOW_METAL`. Use `imgui_impl_sdl3` + `imgui_impl_metal`
(both already in tree). The OpenGL renderer continues to work, but
when Metal is the active renderer, the entire main window is Metal.

**Why not GL/Metal interop via IOSurface?** Apple Silicon supports
zero-copy IOSurface bridging (`[device newTextureWithDescriptor:iosurface:plane:]`
and `CGLTexImageIOSurface2D`), but maintaining two HUD code paths
(`imgui_impl_opengl3` and `imgui_impl_metal`) is more long-term cost
than just porting the HUD to Metal. The GL backend stays selectable for
correctness comparison; when GL is selected, the main window is GL as
today. **The renderer choice becomes a window-creation choice at
startup**; switching renderers mid-session requires a restart (this is
already the GL/Vulkan story today).

**Drawback.** If a user is mid-session on the GL backend and wants to
compare against Metal without restarting, they can't. Acceptable: the
benchmark harness already creates a fresh xemu process per run.

### 3.3 Shader translation

**Decision.** Generate MSL via GLSL → SPIR-V → `spirv-cross::CompilerMSL`.
Mirror Dolphin (`MTLUtil.mm:493-630`). Reuse the existing GLSL
generators in `hw/xbox/nv2a/pgraph/glsl/` as the source of truth.
Add `spirv-cross` as a project dependency.

**Settings**: `set_msl_version(2, 3)`,
`use_framebuffer_fetch_subpasses = true`, deterministic resource
bindings per stage. Cache MSL strings keyed by NV2A `ShaderState` hash;
on cache hit, skip the spirv-cross step (DuckStation pattern).

**Reject**: hand-written MSL (PCSX2 model) — combinatorial explosion of
NV2A combiner variants. **Reject**: direct GLSL→MSL without SPIR-V —
adds maintenance burden and reinvents what spirv-cross already does
correctly.

**Risk to validate at slice M5.** spirv-cross may not handle every
construct in our generated GLSL cleanly. Before committing, add a
small test harness that compiles a representative sample of generated
shaders (one per major NV2A pipeline class) end-to-end and verifies
the MSL compiles via `[device newLibraryWithSource:]`. If a construct
breaks, we either patch the GLSL generator to emit a SPIR-V-friendly
form or hand-write the MSL for that one variant.

### 3.4 Pipeline cache

**Decision.** POD `PipelineKey` hashed with `fast_hash` and compared
with `memcmp` (Dolphin pattern, already proven in xemu's vk renderer
at `vk/renderer.h:64-71`). Lru-backed cache with ~2048 entries. Entries
hold `id<MTLRenderPipelineState>` + reflection captured via
`MTLPipelineOptionArgumentInfo`. Eviction frees the pipeline state
object.

**Persistence**: MSL source strings keyed by `ShaderState` hash, written
to disk in xemu's existing per-game shader cache directory.

**Reject**: `MTLBinaryArchive` for pipeline persistence. Limited macOS
coverage as of 2026, large breakage surface; MSL-string caching
achieves the same goal more portably (DuckStation conclusion). This
amends `strategy.md` Phase 4f. **A decision-log entry will record this
amendment.**

### 3.5 Frame pacing

**Decision (macOS 13)**: `presentDrawable:atTime:` with deadline
computed as `mach_absolute_time + (vblank_interval_ns - elapsed_in_current_frame)`,
converted to mach-time-base seconds. Mirrors DuckStation's
`metal_device.mm:2577-2601` exactly.

**Decision (macOS 14+)**: `CAMetalDisplayLink` with
`preferredFrameRateRange = CAFrameRateRangeMake(30, 120, guest_native)`.
The display link's callback delivers the pre-acquired drawable and the
target presentation timestamp; the renderer encodes against that
drawable.

**Pair with emulation-rate slewing** (PCSX2 PR #5488 / DuckStation
"Sync to host refresh rate"). This is graphics-API-agnostic and can
land on the OpenGL backend before the Metal renderer ships, addressing
the PGR2 / Rainbow tail-jitter from emulation-clock drift.
**Recommendation**: schedule emulation-rate slewing as an independent
slice that lands on GL first, validating the design.

### 3.6 Memory / buffers

**Decision.** Apple Silicon unified memory: `Shared|WriteCombined` for
upload, `Private` for render targets and GPU-only assets, never
`Managed`. Single 64 MiB `MTLBuffer` mapped 1:1 over guest VRAM
(`BUFFER_VERTEX_RAM`, ports directly from `vk/buffer.c`). Triple-
buffered uniform/staging ring fenced with `MTLSharedEvent`. Two
`MTLHeap`s (one for textures, one for transient render targets), each
`MTLHeapTypeAutomatic`. **Defer argument buffers** — Tellusim MDI study
shows they win only at much higher draw counts than NV2A produces.

### 3.7 MSAA + resolve

**Decision.** Private multisample color/depth companions plus a Private
single-sample color resolve target. Color uses
`MTLStoreActionStoreAndMultisampleResolve` so later pass breaks can load
valid MSAA contents while the resolved texture stays current for present
and RTT sampling; depth/stencil use `MTLStoreActionStore` for the same
reason. `XEMU_GL_MSAA` opt-in becomes `XEMU_METAL_MSAA={0,2,4,8}` on
the Metal path; `0` is default for now to keep the cold-launch pipeline
cache small. **Lift MSAA to default 4× only after the full visual/perf
gate passes with the same store policy.**

### 3.8 Geometry expansion (no geometry shaders)

**Decision.** Port xemu's `XEMU_NATIVE_TRI_DEPTH` and `XEMU_NATIVE_QUAD`
CPU index expansion to Metal as the *only* path (not opt-in). Metal has
no geometry shader stage, matching Dolphin/PCSX2/DuckStation
(`bSupportsGeometryShaders = false`). Add Dolphin's
`IndexGenerator.cpp` patterns (`AddFan`, `AddLineList`, `AddLineStrip`)
to cover the remaining geometry-shader categories on the GL path that
were left unaddressed because no current benchmark scene exercised them.

For point sprites and wide lines, adopt PCSX2's static expand-index
buffer in `MTLStorageModePrivate` + VS-Expand variant selected via
`MTLFunctionConstantValues`.

### 3.9 Register-combiner / blend emulation

**Decision.** Use framebuffer fetch (`[[color(0)]]` MSL fragment input)
+ `[[raster_order_group(0)]]` for register-combiner states that read
the destination color. Gated on `[device supportsFamily:MTLGPUFamilyApple1]`
which is true on every Apple Silicon Mac. Build a barrier-based
fallback for Intel Macs (renders the combiner pass into a temp,
samples it in a second pass) — matches DuckStation's
`m_features.feedback_loops = framebuffer_fetch || supports_barriers`
pattern.

### 3.10 Async pipeline compile + ubershader fallback

**Decision.** Adopt Dolphin's hybrid ubershader pattern. Single
megashader interprets NV2A combiner state at runtime; specialized
variants compile in the background; swap on completion. Reuse xemu's
existing async-compile worker thread + queue from
`XEMU_PGRAPH_ASYNC_SHADER_COMPILE`; the "skip the draw" RPCS3 fallback
becomes a compile-progress safety net behind the ubershader.

`setShouldMaximizeConcurrentCompilation:YES` on the device, guarded by
`respondsToSelector:` (Dolphin gotcha for OCLP-patched older Macs;
unlikely to hit our targets but cheap to keep).

### 3.11 Metal capture + counter sampling

**Decision.** Programmatic capture via `MTLCaptureManager` gated on
`XEMU_METAL_CAPTURE=path.gputrace` env var. Frame counter sampling via
`MTLCounterSampleBuffer` at stage boundaries; surface as `METAL_VERTEX_US`,
`METAL_FRAGMENT_US`, `METAL_COMPUTE_US`, `METAL_PRESENT_LATENCY_US` on
the existing `xemu-perf:` interval line. Extend
`scripts/apple-silicon/extract-perf-summary.sh`. Add capture
preconditions to `scripts/apple-silicon/run-benchmark.sh` via a new
`--metal-capture <path>` flag.

`Info.plist` gains `MetalCaptureEnabled = YES` (development builds
only) per Apple's gating; documented in `automation.md` when the slice
lands.

### 3.12 Deployment target

**Decision.** Lift macOS minimum to 13 for the Metal slice (Metal 3 +
MetalFX). macOS 14+ unlocks `CAMetalDisplayLink` and is preferred but
not required. macOS 12 builds keep the OpenGL fallback. Document in
`build.sh` and `decision-log.md`.

---

## 4. Slice ordering

Each slice has a scope, entry criteria, exit criteria, and a validation
gate. Slices are ordered so that a regression in slice N reveals itself
when slice N+1 attempts to build on it. **Do not skip slices.**

The lettered phase 4 deliverables in `strategy.md` (4a–4i) are
dispatched across these slices; each slice marks which 4* it advances.

### M0 — Build + config integration (advances 4a infrastructure) — **SHIPPED 2026-05-02**

**Status: shipped 2026-05-02.** See decision-log entry "2026-05-02:
Metal slice M0 — build + config integration" and the handoff "Update
— 2026-05-02 Metal slice M0 — build + config integration shipped"
section for full file/line listing and verification artifacts.

**Scope.** Add `METAL` to `config_spec.yml`. Wire Foundation / Metal /
MetalKit / QuartzCore framework links in `meson.build` for
`host_os == 'darwin'` and `host_machine.cpu() == 'aarch64'`. Create
`hw/xbox/nv2a/pgraph/mtl/meson.build` and a stub renderer that
registers `pgraph_mtl_renderer` with all 22 ops as no-ops (or
pass-through to NULL renderer where applicable). Add `spirv-cross`
as a Meson subproject. Build an `xemu` binary that includes Metal
but defaults to OpenGL.

**Entry**: current main passes
`scripts/apple-silicon/validate-native-tri-depth.sh --run 22`.

**Exit**: `./build.sh -a arm64` succeeds, `dist/xemu.app` launches,
selecting `display.renderer = METAL` in `xemu.toml` boots without
crashing (the no-op renderer produces a black window — no draws).
`g_config.display.renderer == CONFIG_DISPLAY_RENDERER_METAL` is logged
at startup.

**Gate**: visual smoke — black window + ImGui HUD overlay (HUD still on
GL via the SDL_GL window because we haven't moved the window yet).

**Risk**: low. This is plumbing.

**Implementation note (2026-05-02).** The stub renderer is
implemented as `mtl/renderer.c` (plain C), not `.m` (Objective-C).
Reason: meson's `specific_ss` mechanism propagates the per-target
`-DCOMPILING_PER_TARGET` / `-DCONFIG_TARGET=…` / `-DCONFIG_DEVICES=…`
flags (required by `nv2a_int.h` transitively, via `target/i386/cpu.h`)
to `.c` compilations but not to `.m` (`objc_COMPILER`) compilations
in this build setup. M0 calls zero Metal API, so plain C suffices.
M1+ that need Objective-C will split ObjC-touching code into a
separate `.m` file that does NOT include `nv2a_int.h` — it gets
target-agnostic types only and communicates with `renderer.c`
through opaque handles. This is the same boundary used by
`apple-gfx.m` (already in `system_ss`, no `nv2a_int.h`), so the
pattern is precedented in the tree. The `XEMU_METAL_VALIDATION`
flag originally tied to M0 is deferred to M1 (no Metal device to
validate against until then).

### M1 — Window + device + ImGui-Metal HUD (advances 4a) — **SHIPPED 2026-05-02**

**Scope.** When Metal is the active renderer, create the SDL3 window
with `SDL_WINDOW_METAL` instead of `SDL_WINDOW_OPENGL`. Use
`SDL_Metal_CreateView` + `SDL_Metal_GetLayer` to obtain the
`CAMetalLayer`. Initialize `MTLDevice` (default), one `MTLCommandQueue`
(`maxCommandBufferCount = 8`). Configure layer
(`pixelFormat = BGRA8Unorm_sRGB`, `framebufferOnly = YES`,
`maximumDrawableCount = 3`, `displaySyncEnabled = YES`). Replace
`imgui_impl_opengl3` with `imgui_impl_metal` for HUD rendering on the
Metal path; GL path unchanged. Single command buffer per UI frame:
acquire drawable, render-pass with clear-to-black, encode HUD via
`ImGui_ImplMetal_RenderDrawData`, present, commit.

**Entry**: M0 complete.

**Exit**: `display.renderer = METAL` boots, ImGui HUD renders correctly
over a black background, no NV2A drawing yet, smooth ~60 fps idle.

**Gate**: screenshot diff vs OpenGL HUD-only screenshot — pixel-perfect
on the HUD region; black where game would be. **(Visual smoke gate
deferred to user-driven launch test; see handoff.md M1 entry for the
symbol-presence + build-success verification that satisfies the
non-visual portion of the gate today.)**

**Risk**: low. ImGui Metal backend is mature.

**Status (2026-05-02): SHIPPED.** Files added:
`ui/xemu-metal.h` (C-callable interface),
`ui/xemu-metal.mm` (Objective-C++ implementation; ARC).
Files edited: `ui/xemu.c`, `ui/xui/main.cc`, `ui/xui/font-manager.cc`,
`ui/xui/common.hh`, `ui/meson.build`, `subprojects/imgui/meson.build`,
`meson.build` (objcpp registration + `-fobjc-arc` project arg + flip
imgui's `metal=enabled` on darwin+arm64), `configure` (cross-file
`objcpp` binary + `objcpp_args`). The `imgui_impl_metal.mm` ImGui
backend is built from the imgui subproject (not duplicated in xemu).
`maxCommandBufferCount = 8` matches plan spec verbatim. Verified:
binary builds, links Foundation+Metal frameworks, exposes
`xemu_metal_init/shutdown/render_frame/begin_imgui_frame/end_imgui_frame/
get_device/get_layer/is_active/create_fonts_texture` and all
`ImGui_ImplMetal_*` symbols.

**Implementation note (2026-05-02, post-M0).** When introducing the
first `.m` file in this slice (e.g. `mtl/device.m`,
`mtl/window.m`, or `mtl/imgui_metal_glue.m`), it must NOT include
`hw/xbox/nv2a/nv2a_int.h` — that header pulls in
`target/i386/cpu.h`, which requires the per-target preprocessor
defines (`COMPILING_PER_TARGET`, `CONFIG_TARGET`, `CONFIG_DEVICES`)
that meson's `specific_ss` mechanism propagates only to `.c` /
`c_args` builds, not to `.m` / `objc_COMPILER` builds. The pattern
to follow: keep target-touching code in `mtl/renderer.c`,
forward-declare opaque handle types in a small shared header
(e.g. `mtl/mtl.h` with `typedef struct PGRAPHMtlState
PGRAPHMtlState;` and friends), and have the `.m` files take/return
those opaque handles. This is the exact same boundary `apple-gfx.m`
(`hw/display/apple-gfx.m`, in `system_ss`) already uses — that file
includes `qemu/osdep.h`, `system/dma.h`, `apple-gfx.h`, but never
`nv2a_int.h` or `cpu.h`. **Also include `XEMU_METAL_VALIDATION={0,1}`
flag here** — it was originally listed for M0 in this plan, but
deferred at M0 implementation time because there is no Metal device
to validate against until M1. Toggling Metal API validation needs a
device handle; the natural home is `mtl/device.m` next to the
`MTLCreateSystemDefaultDevice()` call.

### M2 — Surface manager + clear (advances 4a, 4g) — **SHIPPED 2026-05-02**

**Scope.** Port `vk/surface.c` to Metal. `SurfaceBinding` wraps
`id<MTLTexture>` (color RT and depth RT). Surface scale, format,
swizzle, dirty tracking carry over.
`pgraph_mtl_clear_surface(d, parameter)` clears the bound color/depth
target via a render pass with `MTLLoadActionClear`.
`pgraph_mtl_get_framebuffer_surface(d)` returns an opaque handle (an
`MTLTexture*` for now) to the surface manager. The HUD compositor reads
that texture in a final present pass.

**Entry**: M1 complete.

**Exit**: a game that issues only `clear_surface` (no drawing) shows
the cleared color in the window — typically the black/blue Xbox boot
splash before a title's first draw. Verify with the existing
`flat-tri-depth.xiso.iso` test asset.

**Gate**: `validate-native-tri-depth.sh --run 22` runs against the
Metal renderer and reports clear-only correctness (counter:
`METAL_CLEAR_COUNT`, expected per frame ≈ matches GL).

**Risk**: low-medium. Apple Silicon pixel format gotchas
(Depth32Float_Stencil8 vs Depth24Unorm_Stencil8) caught here.

**Status (2026-05-02): SHIPPED.** Files added:
`hw/xbox/nv2a/pgraph/mtl/heap.h` and `heap.mm` (MTLHeap manager —
two heaps × 256 MiB, `MTLHeapTypeAutomatic` +
`MTLStorageModePrivate` + tracked, for color and depth render
targets); `hw/xbox/nv2a/pgraph/mtl/surface.h` and `surface.mm`
(minimal surface manager — one color binding + one depth binding;
NV097 → MTLPixelFormat translation lives in `surface.mm`). Files
edited: `hw/xbox/nv2a/pgraph/mtl/renderer.c` (delegates `init` /
`finalize` / `clear_surface` / `get_framebuffer_surface` /
`set_surface_scale_factor` / `surface_update` / `surface_flush` to
the new managers; other ops remain no-ops for M2);
`hw/xbox/nv2a/pgraph/mtl/meson.build` (registers the two new `.mm`
files alongside `renderer.c`); `ui/xemu-metal.mm` (lazy-built
fullscreen-triangle present-blit pipeline, MSL inline string;
`xemu_metal_end_imgui_frame` now reads the side-channel framebuffer
texture and encodes the blit before the ImGui draw). Heap sizes
chosen as 256 MiB each (planning-doc recommendation in §3.6;
comfortably holds 16+ active+pending surfaces at scale-2 1080p-class).
Apple Silicon `Z24S8` substitution to `Depth32Float_Stencil8`
documented (`Depth24Unorm_Stencil8` is unsupported on Apple GPU
family 7+).

**`int` vs `id<MTLTexture>` interface incompat resolved.** The plan's
literal "`pgraph_mtl_get_framebuffer_surface(d)` returns an opaque
handle (an `MTLTexture*` for now)" is incompatible with
`PGRAPHRenderer.ops.get_framebuffer_surface`'s `int` return signature
(`pgraph.h:131`; the GL impl returns a `GLuint` texture handle).
Resolution: the int op returns 1/0 as a truthy presence signal; the
actual `id<MTLTexture>` is published via a side-channel
`pgraph_mtl_get_framebuffer_metal_texture()` accessor in
`mtl/surface.h`, read by the compositor in `ui/xemu-metal.mm`. No
existing caller of the int op is on the Metal path (see decision-log
2026-05-02 entry "Metal slice M2 — clear-only surface manager + …"
for the full rationale).

**Verified.** `./build.sh -a arm64` succeeds; new symbols
(`_pgraph_mtl_heap_init`, `_pgraph_mtl_heap_alloc_color_rt`,
`_pgraph_mtl_surface_init`, `_pgraph_mtl_surface_clear`,
`_pgraph_mtl_clear_surface`,
`_pgraph_mtl_get_framebuffer_metal_texture`,
`_pgraph_mtl_surface_clear_count`) all present in
`dist/xemu.app/Contents/MacOS/xemu`; the GL renderer's
`_pgraph_gl_clear_surface` symbol is intact (no GL-path regression);
`codesign --verify --deep --strict --verbose=2 dist/xemu.app`
passes. Visual gate via `validate-native-tri-depth.sh --run 22`
remains a user-driven launch test (CLAUDE.md rule #10 prohibits
spawning xemu from the agent while another xemu is running).

**M2 explicitly does NOT do** (deferred to later slices): full
per-VRAM-addr surface cache (M3+); per-channel write mask (M3+);
clear-rect scissor (M3+); no draws / no shader translation / no
textures / no combiners / no MSAA. M2's gate is "clear-only
correctness", and the M2 surface manager is intentionally a tiny
subset of vk/surface.c's 1760-line surface lifecycle.

### M3 — Vertex/index buffers + first hand-coded MSL draw (advances 4b, 4g) — **SHIPPED 2026-05-02**

**Scope.** Port `vk/buffer.c`'s buffer pool to Metal. Implement
`BUFFER_VERTEX_RAM` as a 64 MiB `Shared|WriteCombined` `MTLBuffer`
mapped 1:1 over guest VRAM. Implement triple-buffered staging ring with
`MTLSharedEvent` fence. Implement an `UsageTracker` ring allocator
(Dolphin pattern). Wire `pgraph_mtl_draw_begin/end/flush_draw` with a
**hand-coded MSL passthrough vertex + fragment shader** (no register
combiners, no texturing, just position+color → screen). Pipeline
created synchronously, single pipeline cached.

**Entry**: M2 complete.

**Exit**: a simple 2D scene draws — colored triangles visible in the
window, geometrically correct, not yet textured or combiner-shaded.

**Gate**: visual diff against the existing nxdk `flat-tri-depth` XBE
scene at `Test_Games/flat-tri-depth.xiso.iso` — geometry shape matches,
colors per-vertex interpolate correctly. `METAL_DRAW_COUNT` matches
`gl_draw_count` from the GL run. **(Visual smoke gate deferred to
user-driven launch test; see handoff.md "Update — 2026-05-02 Metal
slice M3" for the symbol-presence + build-success verification that
satisfies the non-visual portion of the gate today.)**

**Risk**: medium. First slice that exercises GPU correctness. Buffer
upload ordering bugs surface here.

**Status (2026-05-02): SHIPPED.** Files added:
`hw/xbox/nv2a/pgraph/mtl/buffer.h` and `buffer.mm` (triple-buffered
ring: 3 × 16 MiB Shared|WriteCombined MTLBuffer slots + MTLSharedEvent
fence; per-frame begin/end with monotonic signal value snapshot per
slot; `pgraph_mtl_buffer_get_vertex_ram` returns NULL — 1:1 vram
mapping deferred to M4 in favor of per-draw staging, see file header
rationale); `hw/xbox/nv2a/pgraph/mtl/pipeline.h` and `pipeline.mm`
(single passthrough pipeline cache, MSL pre-compiled at init,
per-format MTLRenderPipelineState built lazily; 16-entry linear-scan
keyed on (color_pixel_format, depth_pixel_format)); 
`hw/xbox/nv2a/pgraph/mtl/draw.h` and `draw.mm`
(`pgraph_mtl_draw_passthrough` builds MTLRenderPassDescriptor
loadAction=Load / storeAction=Store, encodes setRenderPipelineState +
two setVertexBuffer calls + drawPrimitives, commits its own command
buffer on a dedicated render queue). Files edited:
`hw/xbox/nv2a/pgraph/mtl/renderer.c` (`flush_draw` decodes
`pg->primitive_mode` to MTLPrimitiveType, pulls position +
inline-buffer diffuse color out of `pg->vertex_attributes[]`, calls
into `pgraph_mtl_draw_passthrough`; init/finalize bring up the three
new modules); `hw/xbox/nv2a/pgraph/mtl/surface.h` and `surface.mm`
(adds accessors for the active color/depth texture, format,
dimensions); `hw/xbox/nv2a/pgraph/mtl/meson.build` (registers the
three new `.mm` files). Vertex format scope intentionally bounded to
the inline_buffer (NV097 immediate-mode) path; `draw_arrays` /
`inline_elements` / `inline_array` plus quad / fan / line-loop
primitive expansion all defer to M4. Counters
(`pgraph_mtl_draw_count` / `pgraph_mtl_buffer_stage_bytes` /
`pgraph_mtl_buffer_frame_count` / `pgraph_mtl_pipeline_compile_count`)
present as atomics but NOT yet routed through
`extract-perf-summary.sh` — deferred to M4 because M3 alone does not
produce a representative scene to compare against the GL counters.
Verified: build succeeds, all M3 symbols present, all M0/M1/M2
symbols still present, GL renderer symbols intact, `codesign --verify
--deep --strict --verbose=2` passes. See decision-log "2026-05-02:
Metal slice M3 — vertex/index buffers + first hand-coded MSL draw"
for the full rationale + sub-decisions.

### M4 — IndexGenerator port + native_quad / native_tri_depth (advances 4b) — **SHIPPED 2026-05-02**

**Scope.** Port the GL native-quad CPU expansion (which lives inside
`gl/draw.c` rather than in a freestanding `pgraph_native_quad.c` file
— corrected from the original plan wording) and the
`XEMU_NATIVE_TRI_DEPTH` fragment-shader depth derivation from GL to
Metal. Add Dolphin `IndexGenerator.cpp`-style `AddFan`, `AddLineList`,
`AddLineStrip` templates for the remaining geometry-shader-eliminated
categories.

**Entry**: M3 complete.

**Exit**: triangle/quad-family draws on the Metal path produce zero
geometry-shader-equivalent CPU work. Native-depth fragment shader
produces correct `gl_FragCoord.z`-equivalent depth.

**Gate**: paired benchmark on PGR2 mid-route snapshot
(`benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`,
`pgr2_gameplay_b4`):
- Visual diff against GL with `XEMU_NATIVE_TRI_DEPTH=1` `XEMU_NATIVE_QUAD=1`:
  ≤ 1 % per-pixel difference on a tail-30 second sample.
- Counter `METAL_NATIVE_TRI_DEPTH_DRAWS` and `METAL_NATIVE_QUAD_DRAWS`
  match the GL counts.
- Performance: not regressed below GL-equivalent on the same scene.

**Risk**: medium. Index expansion correctness is well-tested on the GL
path; Metal port should be mechanical.

**Status (2026-05-02): SHIPPED.** See handoff "Update — 2026-05-02
Metal slice M4" and decision-log "2026-05-02: Metal slice M4 —
IndexGenerator port + native_quad / native_tri_depth" for the full
file/line listing, counter rationale, and verification artifacts.
Files added: `hw/xbox/nv2a/pgraph/mtl/index_gen.{h,c}`,
`include/qemu/xemu-metal-perf.h`, `util/xemu-metal-perf.c`. Files
edited: `mtl/meson.build` (registers `index_gen.c`),
`mtl/pipeline.{h,mm}` (adds `passthrough_native_depth_fs` MSL +
variant-tagged cache),
`mtl/draw.{h,mm}` (adds `pgraph_mtl_draw_indexed` +
`drawIndexedPrimitives:` + per-variant counter increment hooks),
`mtl/renderer.c` (`flush_draw` dispatches indexed/non-indexed +
runs the same eligibility helpers GL uses + bumps shared
`NV2A_PROF_NATIVE_*_DRAW` counters), `util/meson.build`,
`hw/xbox/nv2a/pgraph/profile.c` (calls
`xemu_metal_perf_emit_and_reset`), and
`scripts/apple-silicon/extract-perf-summary.sh`.

**Quad triangulation diagonal — exact match with GL.** The Metal
expansion uses the A-C diagonal, with QUADS emit order `(b,c,a)` then
`(c,d,a)` and QUAD_STRIP order `(a,b,c)` then `(c,b,d)`, identical to
`gl/draw.c::native_quad_list_expand_indices` (line 259) and
`native_quad_strip_expand_indices` (line 301), which in turn match
`glsl/geom.c` PRIM_TYPE_QUADS (line 399) and PRIM_TYPE_QUAD_STRIP
(line 438) emit orders. PR #2240's polygon-offset slope
reconstruction depends on this diagonal — diverging would break depth
correctness on quads.

**M4 native_depth fragment shader is scaffolding.** The MSL function
`passthrough_native_depth_fs` derives a `dfdx/dfdy` slope-of-z
(matching the GL native_tri_depth path's `nativeTriMZ` derivation in
`glsl/psh.c` lines 1027-1051) but writes only `zvalue =
in.position.z` — i.e. byte-identical to fixed-function depth. The
full PR #2240 polynomial offset (`zvalue += depthFactor*nativeTriMZ +
depthOffset` etc.) needs the `clipRange` / `depthFactor` /
`depthOffset` / `surfaceScale` uniforms that arrive with the M5 PSH
translator. The MSL is structured so that turning on the bias is one
buffer-bind + one uncomment in the fragment shader.

**Visual gate deferred.** The "≤ 1 % per-pixel diff vs GL on a
tail-30 second sample" measurement requires real shader translation
(M5), texture sampling (M6), and combiner emulation (M7) so a
real-game scene renders correctly through Metal. M4 ships with the
non-visual portion of the gate met (build, symbols, counter plumbing,
GL parity proven by use of the same eligibility helpers); the visual
diff returns at M7 once a scene can render through Metal.

### M5 — Shader translation (GLSL → SPIR-V → MSL) + per-pipeline cache (advances 4f) — **SHIPPED 2026-05-02**

**Scope.** Add `spirv-cross` to the build. Build a `mtl/glsl.c`
analogous to `vk/glsl.c` that takes the GLSL output from the existing
`glsl/` generators, runs glslang to produce SPIR-V, then runs
`spirv-cross::CompilerMSL` to produce MSL. Build a per-pipeline LRU
cache keyed by NV2A `PipelineKey` (POD, `memcmp`-equality). On cache
miss, generate GLSL → SPIR-V → MSL → `[device
newLibraryWithSource:options:completionHandler:]` (async),
`newRenderPipelineStateWithDescriptor:completionHandler:`. Block the
draw on completion **for this slice only**; async behavior comes in M8.

Build a **shader-validation harness** before committing the slice:
compile a representative sample of generated GLSL (one per major NV2A
pipeline class — vertex-program-driven, fixed-function-vertex, simple
combiner, complex multi-stage combiner, alpha-test, fog, two-sided
lighting) end-to-end and verify the MSL is accepted by
`[device newLibraryWithSource:]`. If any breaks, decide per-case: patch
the GLSL generator to emit SPIR-V-friendly form, or hand-write MSL
for that one shader.

**Entry**: M4 complete.

**Exit**: any title that reaches gameplay draws with NV2A
combiner-state-driven shaders, not just hand-coded passthrough.
`shader-validation` harness passes for the representative sample.

**Gate**: PGR2 mid-route snapshot:
- Visual diff: ≤ 5 % per-pixel difference vs GL (combiner edge cases
  may differ; document any).
- Counter `METAL_PIPELINE_HITS / METAL_PIPELINE_MISSES` reported on
  `xemu-perf:` interval line.
- No `MTLCompilerService` crashes (xemu#1466) on a 5-minute sustained
  run.

**Risk**: HIGH. Largest planning risk per §6. The shader-validation
harness must run before committing the slice.

**Status (2026-05-02): Part A (translator + harness) SHIPPED in M5;
Part B per-PipelineKey LRU cache infrastructure SHIPPED in M6 (see
M6 status); production draw-path swap from `passthrough_*` to the
cache further deferred to M7 because the swap's visual gate fails
on combiner-shaded surfaces without M7's framebuffer-fetch
combiner translator.** Files added:
`hw/xbox/nv2a/pgraph/mtl/glsl.h` and `glsl.c` (GLSL → SPIR-V → MSL
translator on the spirv-cross C API; settings: MSL 2.3, MACOS,
framebuffer-fetch-subpass, enable-decoration-binding, fixup-depth-
convention);
`hw/xbox/nv2a/pgraph/mtl/shader_validation.h` and `shader_validation.c`
(in-process harness with 6 representative ShaderState fixtures);
`hw/xbox/nv2a/pgraph/mtl/shaderstate.h` and `shaders.h` (PipelineKey
type + cache API as design artifacts);
`scripts/apple-silicon/metal-shader-validation/run-validation.sh`
(CI runner). Files edited:
`meson.build` (build glslang on darwin/aarch64 even when Vulkan is
unavailable);
`hw/xbox/nv2a/pgraph/mtl/meson.build` (add glsl.c, shader_validation.c
+ libglslang, spirv_cross deps);
`hw/xbox/nv2a/pgraph/mtl/pipeline.{h,mm}`
(`pgraph_mtl_pipeline_validate_msl` thin wrapper around
`[device newLibraryWithSource:]` for the harness);
`hw/xbox/nv2a/pgraph/mtl/renderer.c` (translator init/finalize +
harness invocation in pgraph_mtl_init);
`ui/xemu-metal.mm` (early-fire harness from xemu_metal_init via weak
forward decl, honors `XEMU_METAL_SHADER_VALIDATE_AND_EXIT`);
`ui/xemu-settings.cc` (new `XEMU_RENDERER` env-var bridge);
`util/xemu-metal-perf.{c,h}` (4 new counters);
`scripts/apple-silicon/extract-perf-summary.sh` (surface counters);
`docs/apple-silicon/automation.md` (env vars + counters + harness
section).

**Harness gate cleared 6/6.** Fixtures pass: `ff_vsh_minimal`,
`ff_vsh_lit_textured`, `psh_simple_passthrough`,
`psh_two_stage_textured`, `psh_alpha_test_fog`, `psh_native_tri_depth`.
Run via `scripts/apple-silicon/metal-shader-validation/run-validation.sh`
(exit 0 = full pass).

**Two intentional fixture omissions.**
- **Geometry shader**: Apple Silicon Metal has no native GS stage.
  The renderer already bypasses GS via `XEMU_NATIVE_TRI_DEPTH` /
  `XEMU_NATIVE_QUAD`. spirv-cross's GS-emulation path silently
  SIGSEGVs on a line-loop GS payload in vulkan-sdk-1.3.290.0;
  validating that path would gate the slice on a feature unused on
  Metal. Documented in `shader_validation.c::build_fixtures`.
- **Programmable VSH**: vsh-prog requires a valid VSH token sequence
  with FLD_FINAL set; hand-encoding NVIDIA Cheops binary tokens is
  fragile. The fixed-function vsh fixtures exercise the same
  prologue/body/epilogue layout. Future iteration: capture a real
  ShaderState from a running game and replay through the harness.

**Why the cache + draw-path swap deferred.** A translated state-
driven shader cannot replace the M3/M4 hand-coded passthrough on
the draw path until M6 (texture/sampler binding) and M7 (combiner-
via-framebuffer-fetch) land — uniform buffers, samplers, and
combiner emit all need backing infrastructure that M5 does not
supply. The PipelineKey type + cache API headers ship as design
artifacts so M6 has a fixed integration target. The plan's "≤ 5 %
per-pixel diff vs GL on PGR2" exit-gate test is paired with M6/M7
because it requires the production swap.

**Synchronous block-on-compile (M5 spec).** `newLibraryWithSource:`
and `newRenderPipelineStateWithDescriptor:` calls are synchronous;
async lands in M8. Per the plan §4 M5 spec.

**Counters added.** `METAL_GLSL_TRANSLATE`,
`METAL_GLSL_TRANSLATE_FAIL`, `METAL_SHADER_VALIDATE_OK`,
`METAL_SHADER_VALIDATE_FAIL`. The translate counters reach steady-
state non-zero only when M6+ wire the translator on the draw path;
the validate counters tick only when `XEMU_METAL_SHADER_VALIDATE`
is set.

See decision-log entry "2026-05-02: Metal slice M5 — shader
translator + validation harness ship; per-pipeline cache deferred
to M6" for the full rationale.

**M5.6 (translator failure elimination, 2026-05-03).** Following M5.5
(draw paths online) the slice landed two follow-on fixes that took
the pipeline build success rate from 67 % to 100 %: (1) populate
every shader-referenced descriptor slot in `build_pipeline_internal`
(was previously skipped for `count == 0` uniform attrs, which the MSL
declares as `[[attribute(N)]]`); (2) emit CMP-packed format as raw
`MTL_VFMT_INT` instead of `MTL_VFMT_INT1010102_NORMALIZED` to match
spirv-cross's `int v1_cmp` declaration. See decision-log entry
"2026-05-03: Metal slice M5.6 — translator failures eliminated".

**M5.6 Part B (uniform-attribute UBO routing, 2026-05-03).** Replaces
the M5.6 Part A "fallback bufferIndex 0 / 3" hack with the proper
Vulkan-pattern uniform-via-UBO routing. The Metal renderer's
`mtl_dispatch_decoded_draw` now correctly drives `pg->uniform_attrs`
(via the new `pgraph_mtl_set_attr_masks` helper in
`hw/xbox/nv2a/pgraph/mtl/vertex.c`) — every NV2A attribute slot the
M5.5 encode path doesn't supply is routed through the VSH UBO's
`inlineValue[]` block at MSL `[[buffer(1)]]`. The pipeline descriptor
is now sparse — inactive slots are skipped entirely instead of
pointing at bogus buffer bindings. The magenta-surface artifact in
the test environment is eliminated; with Part B applied a 60 s PGR2
Metal benchmark hits `METAL_PIPELINE_TRANSLATED_FAILED=0`,
`METAL_DRAW_TRANSLATED == METAL_DRAW_COUNT` (100 % translated),
`METAL_PIPELINE_FALLBACKS=0`. See decision-log entry "2026-05-03:
Metal slice M5.6 Part B — uniform-attribute UBO routing".

**M5.8 (full per-vertex attribute decoder, 2026-05-03 — supersedes
M5.6 Part B's mask shortcut).** M5.6 Part B's `pgraph_mtl_set_attr_masks`
collapsed every NV2A attribute slot except POSITION + DIFFUSE to a
single `inline_value` per draw — a correctness regression that
showed up as solid-coloured geometry (texcoord/normal/specular/fog
arrays all degenerate to one texel / one direction / one fog value)
and as a 24× draw-throughput drop versus the agent bisect's
"set_attr_masks disabled" baseline (3.8 k draws/60 s vs 93 k). M5.8
extends the M5.5 decoder to all 16 NV2A attribute slots, removes the
POSITION/DIFFUSE-only shortcut from `pgraph_mtl_set_attr_masks`
(now matches `vk/vertex.c:148-154 + 226-236`), and adds CMP
((11,11,10) packed) format support so D3D normal-cmp slots decode
CPU-side. Companion fix: VSH UBO now binds at vertex `atIndex:0` —
matching the spirv-cross MSL `[[buffer(0)]]` declaration —
previously bound at index 1 (a bug since M7.1 that shadowed position
bytes); per-attribute streams shifted to bufferIndex
`MTL_ATTR_BUFFER_INDEX_BASE + slot` = [1..16] to avoid clashing
with the UBO. Build PASS, M5 harness 7/7 PASS, 60 s PGR2 Metal
bench: `METAL_DRAW_COUNT=75 016`, `METAL_DRAW_INDEXED_COUNT=74 952`,
`METAL_PIPELINE_TRANSLATED_FAILED=0`, `METAL_PIPELINE_FALLBACKS=0`,
100 % translated. Files touched: `mtl/{vertex.c,vertex.h,renderer.c,
state.c,shaders.mm,draw.mm,draw.h}`. See decision-log entry
"2026-05-03: Metal slice M5.8 — full per-vertex attribute decoder".

### M6 — Texture upload + sampling (advances 4g) — **SHIPPED 2026-05-02 (cache infra + sampler infra; full S3TC/mipmap + draw-path swap deferred to M6 Part B / M7)**

**Scope.** Port `vk/texture.c` to Metal. `TextureBinding` wraps
`id<MTLTexture>` + `id<MTLSamplerState>`. S3TC decode reuses xemu's
existing CPU path; uploads go via blit encoder from a staged `Shared`
`MTLBuffer` slice into a `Private` `MTLTexture` (lossless compression
implicit). Sampler cache pre-built at PGRAPH init for the small finite
set of NV2A texture-stage states. `MTLHeap` for non-aliasable assets.

M6's scope was expanded at implementation time to absorb the deferred
M5 Part B (per-PipelineKey LRU cache + draw-path swap) per §6 R1
mitigation — the M6 exit gate "textured draws produce correct sampled
output" requires a translated state-driven shader path that the M3/M4
hand-coded passthrough doesn't have.

**Entry**: M5 complete.

**Exit**: textured draws produce correct sampled output — game scenes
that don't need register-combiner-style blends look correct.

**Gate**: PGR2 + Rainbow + Crimson snapshot triplet, visual diff ≤ 2 %
per-pixel against GL (excluding combiner-blend regions, which are still
WIP). Counters: `METAL_TEX_UPLOAD_BYTES_TOTAL`,
`METAL_TEX_UPLOADS_TOTAL`, `METAL_PIPELINE_HITS`,
`METAL_PIPELINE_MISSES`.

**Risk**: medium. Texture caching and S3TC are well-trodden in xemu's
GL path.

**Status (2026-05-02): SHIPPED — infrastructure + cache + working
single-level 2D RGBA upload + 24-combination pre-warmed sampler cache.
Draw-path swap and full mipmap/cube/S3TC port intentionally deferred
(see "What ships now" / "What is deferred" below).** Files added:
`hw/xbox/nv2a/pgraph/mtl/shaders.mm` (Metal-API layer for the pipeline
cache: GLSL → SPIR-V → MSL translation, MTLLibrary build,
MTLRenderPipelineState build; `main0` → `vertex_main0` /
`fragment_main0` rename so vertex+fragment coexist in one library),
`hw/xbox/nv2a/pgraph/mtl/shadergen.c` (per-target C side: LRU cache
on `qemu/lru.h`, GLSL generator calls — split exists because lru.h's
`typeof` macros + `MString` glib inlines aren't portable to C++),
`hw/xbox/nv2a/pgraph/mtl/texture.h` and `texture.mm` (texture cache,
sampler cache pre-warmed at init with 2×3×4 = 24 NV2A combinations,
4× 4 MiB Shared|WriteCombined upload ring, blit-encoder upload to
Private MTLTextures from `heap_textures`). Files edited:
`hw/xbox/nv2a/pgraph/mtl/heap.{h,mm}` (adds third heap `heap_textures`,
512 MiB, MTLHeapTypeAutomatic + Private + **Untracked**, plus
`pgraph_mtl_heap_alloc_texture_2d/cube/3d` allocators),
`hw/xbox/nv2a/pgraph/mtl/shaders.h` (forward-declares
`PgraphMtlPipelineKey` so the .mm side never dereferences it),
`hw/xbox/nv2a/pgraph/mtl/meson.build` (registers four new sources),
`hw/xbox/nv2a/pgraph/mtl/renderer.c` (calls
`pgraph_mtl_shaders_init/finalize` + `pgraph_mtl_texture_init/finalize`
in the renderer init/finalize hooks),
`util/xemu-metal-perf.c` (seven new counters with weak accessors),
`scripts/apple-silicon/extract-perf-summary.sh` (surfaces them).

**What ships now (M6 production-ready):**
- The pipeline cache infrastructure end-to-end: init/lookup/translate/
  build/store/release on evict. Capacity 2048, fast_hash + memcmp.
- The texture path infrastructure: heap allocation, cache lookup +
  insertion + FIFO eviction, sampler cache + pre-warm, upload via
  blit encoder.
- M5 shader-validation harness re-run still 6/6.

**What is deferred (intentionally NOT in M6 commit):**
- The renderer.c flush_draw call-site swap from
  `pgraph_mtl_pipeline_get_passthrough` to
  `pgraph_mtl_shaders_get_pipeline`. Two pieces of mechanical work
  block this: (1) state-to-PipelineKey conversion (call
  `pgraph_glsl_get_shader_state(pg)`, walk `pg->vertex_attributes[]`,
  build the NV2A → MTLVertexFormat lookup); (2) uniform-buffer +
  texture binding integration on the render encoder
  (`setVertexBuffer:atIndex:1`, `setFragmentBuffer:atIndex:1`,
  `setFragmentTexture:atIndex:N`, `setFragmentSamplerState:atIndex:N`).
- The `vk/texture.c::get_texture_layout` full lifecycle (per-mip,
  per-face cube, swizzled-texture handling, S3TC decode via
  `hw/xbox/nv2a/pgraph/s3tc.c`). M6 ships single-level 2D RGBA
  upload as a working scaffold; queued as M6 Part B.
- Visual gate (PGR2 + Rainbow + Crimson, ≤ 2 % per-pixel diff vs GL).
  Without combiner emulation a translated draw produces wrong colors
  on most combiner-shaded surfaces, so the gate is meaningful only
  paired with M7. The build/symbol/harness/re-run portion of M6's
  gate is satisfied.

**Heap budgeting.** `heap_textures` chosen at 512 MiB (vs 256 MiB for
color/depth) because xemu's renderer can hold per-mip + per-face copies
plus surface_scale=2 upscaled redraws. Apple Silicon Private +
Untracked means no driver hazard tracking + lossless compression
available; unused regions are not pre-touched.

**Sampler cache cardinality.** 24 pre-warmed combinations covers
the most-frequent NV2A combinations. NV2A's texture-stage state
encodes ~6 filter modes, ~10 mip modes, ~5 addr modes per axis,
max_anisotropy 1..16, lod_bias 12-bit; the unique-combination upper
bound observed in PGR2/Rainbow/Crimson logs is < 100. Cap at 256
gives 2.5× headroom.

See decision-log "2026-05-02: Metal slice M6 — textures + sampling
infra + pipeline cache shipped" for the full design rationale,
including the C/.mm boundary discussion, what's deferred, and why the
draw-path swap waits for M7.

### M7.1 — Translated pipeline encode swap + M6 Part B foundational port — **SHIPPED 2026-05-02 (build + symbol + harness gates; visual gate pending user launch test)**

**Scope.** Wire the translated MTLRenderPipelineState lookup
(M7) into the actual encoder. Three sub-deliverables:

1. **Uniform-buffer marshaling**: build `VshUniformValues` /
   `PshUniformValues` via `pgraph_glsl_set_*_uniform_values`,
   pack into std140-flat buffer via the manual algorithm matching
   `vk/glsl.h::uniform_std140`, stage into a 4-slot Shared|WC ring,
   bind at `[[buffer(1)]]` (vertex) / `[[buffer(1)]]` (fragment).
2. **Per-stage texture/sampler binding**: walk PGRAPHState
   stages 0..3, decode S3TC / unswizzle / format-convert via the
   existing CPU paths (`s3tc.c` / `swizzle.c` /
   `pgraph_convert_texture_data`), upload per-mip per-face data
   to a Cube or 2D texture allocated from heap_textures, build
   sampler from `NV_PGRAPH_TEXFILTER0` / `_TEXADDRESS0`, bind at
   `[[texture(N)]]` / `[[sampler(N)]]`.
3. **Encode dispatch decision**: `XEMU_METAL_TRANSLATED_PIPELINE=1`
   + lookup OK → translated path. Otherwise → M3/M4 passthrough.
   Lookup failure with translated requested → fallback path,
   increment `METAL_PIPELINE_FALLBACKS`.

**Entry**: M7 complete (state-to-PipelineKey + cache lookup).

**Exit**: Build passes, symbols present, harness 7/7,
`XEMU_METAL_TRANSLATED_PIPELINE=1` is functional (encode swap +
UBO + texture binding), foundational M6 Part B (per-mip + per-face
+ S3TC + swizzled) shipped.

**Gate**: User-driven launch test of
`XEMU_METAL_TRANSLATED_PIPELINE=1` against PGR2 / Rainbow /
Crimson mid-route snapshot triplet, with Metal validation
enabled to surface UBO-binding-index mismatches as validation
errors.

**Status (2026-05-02): SHIPPED.** Build passes; M5 harness 7/7
(incl. `psh_native_tri_depth` PR #2240 fixture). Symbols
`_pgraph_mtl_draw_translated`, `_pgraph_mtl_uniform_init`,
`_pgraph_mtl_uniform_stage_vsh`, `_pgraph_mtl_uniform_stage_psh`,
`_pgraph_mtl_texture_bind_slot_full`,
`_pgraph_mtl_texture_bind_from_pg`,
`_pgraph_mtl_texture_color_format_to_mtl`,
`_pgraph_mtl_draw_pipeline_fallback_count` all present. PR #2240
correctness preserved.

Files added: `mtl/uniform.{h,c,mm}`, `mtl/format.{h,c}`,
`mtl/texture_pg.c`. Files edited:
`mtl/{draw.h,draw.mm,texture.h,texture.mm,renderer.c,meson.build}`,
`util/xemu-metal-perf.c`, `scripts/apple-silicon/extract-perf-summary.sh`,
`xemu-fork/CLAUDE.md`. New counters: `METAL_DRAW_TRANSLATED`,
`METAL_PIPELINE_FALLBACKS`, `METAL_UNIFORM_PACK`,
`METAL_UNIFORM_BYTES`. `XEMU_METAL_TRANSLATED_PIPELINE` flipped
from "no-op for encode" to "functional gate".

**M6 Part B ships within this slice (foundational subset).**
Per-mip + per-face + S3TC + swizzled textures. Path A — CPU
S3TC decode to RGBA8 via `s3tc_decompress_2d`. Deferred to
follow-up: surface-to-texture, palette-indexed, 3D volume
textures, custom border colors, shadow samplers, vram-hash
dirty tracking.

See decision-log "2026-05-02: Metal slice M7.1 — translated
pipeline encode swap + M6 Part B foundational port".

**Supersedes:** M6 status note "production draw-path swap from
`passthrough_*` to the translated MTLRenderPipelineState
deferred to M7" — closed by M7.1.

### M7 — Register-combiner emulation via framebuffer fetch (advances 4c) — **SHIPPED 2026-05-02 (state-to-key + framebuffer-fetch validated; encode-path swap + visual gate deferred to M7.1)**

**Scope.** Detect Apple1+ at init (we always are, on Apple Silicon).
Re-target the existing GLSL combiner generator (`glsl/psh.c`) to emit a
shader that reads `[[color(0)]]` as input on Apple Silicon, with
`raster_order_group(0)` for ordered access. This is a generator
modification: when generating MSL for combiner states that need to
read the destination color, emit the framebuffer-fetch form.

Build the Intel-Mac fallback path: a render-pass split that copies the
color attachment to a sampled texture before the second pass. Gated
on `![device supportsFamily:MTLGPUFamilyApple1]` — should be unreachable
on Apple Silicon but exercised in `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH=1`
mode.

**Entry**: M6 complete.

**Exit**: combiner-blend-heavy scenes (Crimson particle effects,
Rainbow muzzle flashes, PGR2 lighting) match GL output pixel-by-pixel
(within tolerance for floating-point determinism).

**Gate**: PGR2 + Rainbow + Crimson snapshot triplet, visual diff ≤ 1 %
per-pixel including combiner regions. Performance:
`METAL_FRAGMENT_US_TOTAL` ≤ 1.2× GL equivalent.

**Risk**: HIGH. Combiner correctness is fork-specific and PR #2240's
correctness work must not regress. Project rule #6.

**Status (2026-05-02): SHIPPED with honest scope split.** Crucial
finding during M7 implementation: **NV2A combiners do not in fact
read destination color**. Combiner inputs are vertex-interpolated
attributes, texture samples, and previous-stage register output
(`r0..r15`). The final color is written to `fragColor`; standard
NV2A blend modes (`NV_PGRAPH_BLEND`) are fixed-function and Metal
supports all of them natively via
`MTLRenderPipelineColorAttachmentDescriptor.{srcRGB,dstRGB,rgbBlendOperation}`
without needing programmable framebuffer-fetch reads. The
framebuffer-fetch path is therefore **infrastructure** for M8's
planned ubershader programmable-blend variant, not a current
correctness need. M7 lands the option flag (already enabled in M5),
the Apple1+ detection, the validation harness fixture proving the
GLSL → SPIR-V → MSL chain emits `[[color(0)]]` correctly, and the
state-to-PipelineKey + draw-path lookup wiring required to use the
M5/M6 cache from real PGRAPHState — but the encode swap (from M3/M4
hand-coded passthrough to translated MSL) is **deferred to M7.1**
because uniform-buffer marshaling + per-stage texture/sampler
encode binding are mechanical-but-bulky ports of vk/draw.c that
require a user-driven visual diff gate (CLAUDE.md rule #10).

Files added: `mtl/state.{h,c}` (state-to-PipelineKey + NV2A→
MTLVertexFormat translation table). Files edited:
`mtl/{heap.h,heap.mm,renderer.c,shader_validation.c,meson.build}`,
`util/xemu-metal-perf.c`, `scripts/apple-silicon/extract-perf-summary.sh`.
New env vars: `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH`,
`XEMU_METAL_FORCE_PASSTHROUGH`, `XEMU_METAL_TRANSLATED_PIPELINE`.
New counters: `METAL_PIPELINE_KEY_BUILT`,
`METAL_PIPELINE_TRANSLATED_OK`, `METAL_PIPELINE_TRANSLATED_FAILED`.
PR #2240 correctness preserved (`psh_native_tri_depth` validation
fixture still passes, `glsl/psh.c` unchanged).

**M7.1 (planned).** Uniform-buffer marshaling + per-stage texture/
sampler encode binding so the translated pipeline becomes the
production encode path. Requires user-driven visual gate against
PGR2/Rainbow/Crimson mid-route snapshot triplet.

**M6 Part B (full S3TC + per-mip + per-face texture lifecycle)
remains DEFERRED.** Was nominally bundled into M7's scope; ~1500
lines of port from vk/texture.c + s3tc.c. Out of scope for the
M7 commit (4× the size of the rest of the slice; orthogonal to
state-to-key + lookup work). Queued separately.

See decision-log "2026-05-02: Metal slice M7 — state-to-PipelineKey +
framebuffer-fetch validated" and handoff "Update — 2026-05-02 Metal
slice M7".

### M8 — Async pipeline compile + ubershader fallback (advances 4e) — **SHIPPED 2026-05-02 (Path B); Path A deferred to M8.1**

**Scope.** Build the hybrid ubershader. A single megashader interprets
runtime NV2A combiner state, used as the fallback while specialized
variants compile in the background. Reuse xemu's existing
async-compile worker thread (from `XEMU_PGRAPH_ASYNC_SHADER_COMPILE`)
plus a "skip the draw" safety fallback for cases where even the
ubershader isn't ready (cold launch).

Drive `setShouldMaximizeConcurrentCompilation:YES` (guarded), to
parallelize compile across CPU cores.

**Entry**: M7 complete.

**Exit**: cold launch of PGR2 reaches gameplay without a per-shader
synchronous compile stall. New scene transitions don't block the
renderer thread.

**Gate**: PGR2 cold-launch (delete shader cache directory first)
benchmark — no `mspf_max` event > 250 ms attributable to shader
compile. Counters: `METAL_SHADER_COMPILE_QUEUED_TOTAL`,
`METAL_SHADER_COMPILE_COMPLETED_TOTAL`,
`METAL_DRAWS_USING_UBERSHADER_TOTAL`,
`METAL_DRAWS_SKIPPED_PENDING_TOTAL`.

**Risk**: medium. Ubershader correctness is a known-good pattern from
Dolphin.

**Status (2026-05-02): SHIPPED — Path B only; Path A (full hybrid
ubershader) deferred to M8.1.** See decision-log entry "2026-05-02:
Metal slice M8 — async pipeline compile + skip-the-draw + upload
fence; Path A deferred to M8.1" and the handoff "Update — 2026-05-02
Metal slice M8" section for the full rationale.

**Path B (shipped):** async pipeline-compile state machine
(MISSING/PENDING/READY/FAILED + epoch-validated completion handler),
private serial concurrent dispatch queue at QoS_UTILITY,
`setShouldMaximizeConcurrentCompilation:YES` driven on the device,
RPCS3-style "skip the draw" fallback when `PENDING` is observed and
`XEMU_METAL_TRANSLATED_PIPELINE=1`, plus a GPU-side
`MTLSharedEvent` texture-upload fence replacing the M6
`[cb waitUntilCompleted]` CPU stall. New env var
`XEMU_METAL_ASYNC_PIPELINE_COMPILE={0,1}` (default 1). Counters
wired through `extract-perf-summary.sh`. Files edited:
`mtl/{shaders,shadergen,renderer,texture,draw}.{c,mm,h}`,
`util/xemu-metal-perf.c`, `scripts/apple-silicon/extract-perf-summary.sh`.

**Path A (deferred):** the full Dolphin-style hybrid ubershader (a
~2000-LOC megashader interpreting NV2A combiner state via uniform-
buffer-driven runtime branches; uniform-state encoding; a separate
hybrid pipeline cache keyed on coarse render-pass state alone) was
judged too large a surface area for a single autonomous agent run.
Path B's "skip the draw" output is the same correctness-vs-perf
trade-off RPCS3 ships in production and that this fork's GL path
already validated via `XEMU_PGRAPH_ASYNC_SHADER_COMPILE`. Path A is
queued behind real-game user testing — it becomes M8.1 only if
skip-the-draw artifacts on PGR2 / Crimson / Rainbow cold launch are
visually unacceptable.

**Visual exit gate (PGR2 cold-launch `mspf_max < 250 ms` attributable
to shader compile)** is gated on a user-driven launch test per
CLAUDE.md rule #10. Agent-attainable verification satisfied: build
+ codesign succeeds; M5 harness 7/7 PASS; all M0-M7.1 + M8 symbols
present; GL renderer symbols intact.

### M9 — Persistent shader cache (advances 4f) — **SHIPPED 2026-05-02**

**Scope.** Persist the MSL source strings (or compiled metallibs)
keyed by NV2A `ShaderState` hash, in xemu's existing per-game shader
cache directory (the GL renderer already does the GLSL equivalent).
On cold launch, load the cache before the first compile to skip the
spirv-cross step. Gated on `XEMU_METAL_PIPELINE_CACHE=1` (default 1).

**Entry**: M8 complete.

**Exit**: second-launch PGR2 reaches gameplay 2× faster than first
launch (no measurable spirv-cross cost on the renderer thread). The
exit gate measurement requires a paired cold-launch / warm-launch
benchmark and is **deferred to user-driven validation** alongside the
M8 cold-launch listen-test (delete shader cache → first launch
populates → second launch should hit cache).

**Gate**: paired cold-launch / warm-launch PGR2 benchmark. Counters:
`METAL_SHADER_CACHE_LOADS`, `METAL_SHADER_CACHE_HITS`,
`METAL_SHADER_CACHE_MISSES`. Decision-log entry recording the choice
of MSL-source caching over `MTLBinaryArchive`.

**Risk**: low. xemu's GL-side disk cache is the proven precedent.

**Implementation.** The disk cache lives at
`hw/xbox/nv2a/pgraph/mtl/disk_cache.{h,c}`. It mirrors `gl/shaders.c`'s
shader-cache layout (top-16 / bottom-48 hash sharding,
`<base>/metal_shaders/` directory, `metal_shader_cache_list` LRU index
file). Per-file self-describing header carries the xemu version
string + a Metal feature-set fingerprint
(`AppleGPUFamily<N>/macOS<major>.<minor>` — Apple7=M1, Apple8=M2,
Apple9=M3) + the full `PgraphMtlPipelineKey` blob; on header
mismatch the file is unlinked. Saves run on detached
`metal-scache-<hash>` background threads with a concurrent-cap of 64
(synchronous fallback above the cap). On cache hit the loaded MSL
bypasses the GLSL→SPIR-V→MSL translator entirely and feeds straight
into `[device newLibraryWithSource:]`. Counters
`METAL_SHADER_CACHE_LOADS` / `METAL_SHADER_CACHE_HITS` /
`METAL_SHADER_CACHE_MISSES` surface on the `xemu-perf:` interval line.
File-system errors fail soft (log + skip + unlink the offending file).
Env var `XEMU_METAL_PIPELINE_CACHE={0,1}` (default 1 on Apple Silicon)
is the user-facing opt-out.

See decision-log entry "2026-05-02: Metal slice M9 — persistent
MSL-source disk cache" for the rationale, and §3.4 + strategy.md
Phase 4f for the MSL-source-vs-MTLBinaryArchive amendment that this
slice ratifies.

### M10 — Frame pacing: presentDrawable:atTime: + CAMetalDisplayLink (advances 4a) — **SHIPPED 2026-05-02 (atTime: + emulation-rate slewing prerequisite; CAMetalDisplayLink deferred to M10.1)**

**Scope.** Implement two presentation paths based on macOS version.
**macOS 13 path**: compute deadline as `mach_absolute_time +
(vblank_interval_ns - elapsed)`, convert to mach-base seconds, call
`[cmdbuf presentDrawable:drawable atTime:t]`. **macOS 14+ path**:
`CAMetalDisplayLink` with
`preferredFrameRateRange = (30, 120, guest_native)`. The display-link
callback delivers the pre-acquired drawable + target timestamp; the
renderer thread encodes against that drawable.

Add `presentedHandler` callbacks to measure presentation jitter and
update an EWMA frame budget. Surface as
`METAL_PRESENT_JITTER_US_MAX`, `METAL_PRESENT_JITTER_US_AVG` on
`xemu-perf:`.

**Pair with emulation-rate slewing.** Schedule the slewing slice
**before this** as a graphics-API-agnostic slice that lands on the GL
backend first. Without slewing, the deadline-based present buys little
on its own.

**Entry**: M9 complete; emulation-rate slewing slice (separate, not
numbered here) shipped on GL first.

**Exit**: PGR2 + Rainbow + Crimson tail-jitter is measurably reduced
(p99 mspf reduced by ≥ 20 %).

**Gate**: paired benchmark — `XEMU_METAL_FORCE_LEGACY_PRESENT=1` (no
`atTime:`) vs `=0`. Counters: `METAL_PRESENT_JITTER_US_*`. Decisive
expectation: `=0` p99 < `=1` p99 by a meaningful margin (e.g. 20 %).
**Note**: this does NOT erase the 1.3 s class judder per the
2026-05-02 V9/V10 attribution; that is guest-intrinsic.

**Risk**: medium. CoreAnimation's interaction with VRR displays is
documented but emulator-specific behavior may surprise.

**Status (2026-05-02): SHIPPED — `presentDrawable:atTime:` path with
emulation-rate slewing as a prerequisite slice on GL. CAMetalDisplayLink
deferred to M10.1.**

The M10 implementation lands the explicit-deadline presentation path
plus the emulation-rate slewing prerequisite (PCSX2 PR #5488 / DuckStation
"sync to host refresh"). The CAMetalDisplayLink integration is queued
as a follow-up sub-slice (M10.1) because retrofitting display-link-driven
control flow into the existing xemu vblank-thread model requires control-
flow inversion (or a thread-safe drawable hand-off slot) that exceeds
the M10 scope. The simpler `presentDrawable:atTime:` path mirrors
DuckStation's known-good emulator pattern and meets the M10 exit
gate's actual goal (host-display sync via deadlined presentation +
jitter measurement). The `METAL_DISPLAY_LINK_CALLBACKS` counter slot
is reserved so M10.1 can ship without a perf-summary update.

**Files added.**

* `include/qemu/xemu-rate-slew.h` / `ui/xemu-rate-slew.c` — the
  graphics-API-agnostic emulation-rate slewing module. SDL-aware; runs
  at window-creation and on display-changed events. Mutates the global
  `vblank_interval_ns` consumed by both the GL vblank-timer thread
  (`ui/xemu.c::vblank_timer_thread`) and the Metal frame-pacing
  computation (`ui/xemu-metal.mm::xemu_metal_end_imgui_frame`).

**Files edited.**

* `ui/xemu.c` — calls `xemu_rate_slew_init(m_window)` after window
  creation; routes
  `SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED` /
  `SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED` /
  `SDL_EVENT_WINDOW_DISPLAY_CHANGED` /
  `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` to
  `xemu_rate_slew_update(m_window)`.
* `ui/xemu-metal.mm` — replaces the unconditional `presentDrawable:`
  with `presentDrawable:atTime:` (deadline = previous target +
  `vblank_interval_ns`, reseeded if behind > 2 vblank periods);
  attaches `addPresentedHandler:` to record per-frame jitter; reads
  `XEMU_METAL_FORCE_LEGACY_PRESENT` to opt back to the plain
  `presentDrawable:` for A/B testing; exports the M10 counter
  accessors (`pgraph_mtl_present_total`,
  `pgraph_mtl_present_jitter_us_total`, `pgraph_mtl_drawable_acquire_fails`,
  `pgraph_mtl_display_link_callbacks`, etc.) consumed as strong
  symbols by `util/xemu-metal-perf.c`.
* `util/xemu-metal-perf.c` + `include/qemu/xemu-metal-perf.h` — adds
  the M10 counter slots, baselines, and emit fields:
  `METAL_PRESENTS`, `METAL_PRESENT_JITTER_US_TOTAL`, `_AVG`, `_MAX`,
  `METAL_DRAWABLE_ACQUIRE_FAILS`, `METAL_DISPLAY_LINK_CALLBACKS`
  (reserved for M10.1).
* `hw/xbox/nv2a/pgraph/profile.c` — calls `xemu_rate_slew_emit(stderr)`
  alongside the other per-interval emitters so `RATE_SLEW_RATIO_E6` /
  `RATE_SLEW_ACTIVE` show up on the `xemu-perf:` line.
* `ui/meson.build` — registers `xemu-rate-slew.c` in `xemu_ss`.
* `scripts/apple-silicon/extract-perf-summary.sh` — adds the new
  counter keys (RATE_SLEW_*, METAL_PRESENTS, METAL_PRESENT_JITTER_*,
  METAL_DRAWABLE_ACQUIRE_FAILS, METAL_DISPLAY_LINK_CALLBACKS).
* `xemu-fork/CLAUDE.md` + `docs/apple-silicon/automation.md` — flag +
  counter docs.

**Gate (exit criterion not yet measured.)** The "p99 mspf reduced by
≥ 20 %" measurement requires a paired
`XEMU_METAL_FORCE_LEGACY_PRESENT=0` vs `=1` benchmark on the same
PGR2 / Rainbow / Crimson snapshot triplet, after the Metal renderer
has reached the gameplay frame (M5–M8 render-correctness work has
landed, but a real game scene still requires confirming the M3/M4
passthrough composes with the M7/M7.1 translated pipeline).
Treat M10 as "shipped + correctness verified by symbols + build" but
not "exit-gate confirmed by measurement". This is consistent with
the project's data-driven discipline: the slice is wired correctly
so the speedup IS achievable, but the actual jitter improvement
must be measured before the slice's exit gate can be marked passed.

**Caveat (honest scope).** The 1.3 s class Crimson Skies stutter is
guest-intrinsic per the 2026-05-02 V9/V10 attribution. M10 closes the
emulator-display sync gap (host vsync vs guest pacing) but does not
shrink that stutter class. The M10 expected impact is on the
**tail-jitter percentiles** (p95 / p99 mspf), not on worst-frame
maxima.

### M11 — MSAA + resolve (advances 4i) — **SHIPPED 2026-05-02 (Private storage; Memoryless deferred to M11.1)**

**Status (2026-05-04): SHIPPED v2.** Implementation present; default
0 (off). The original v1 store policy was corrected after PGR2 exposed
an MSAA4 black-frame bug. User-driven paired visual/perf gate is still
pending before lifting to default 4×. See decision-log
"2026-05-02: Metal slice M11 — MSAA + resolve" and
"2026-05-04: Metal MSAA store/resolve bug fixed" for the landed-state
record.

**Scope.** Private multisample texture + single-sample resolve target.
Pipeline `rasterSampleCount` matches attachment. Color draw/clear passes
use `MTLStoreActionStoreAndMultisampleResolve`; depth/stencil draw/clear
passes use `MTLStoreActionStore`. `XEMU_GL_MSAA` becomes
`XEMU_METAL_MSAA={0,2,4,8}` on the Metal path. Initial default 0 (off)
until the visual/perf gate proves default 4× is safe.

Optionally: programmable sample positions on Apple7+ via
`[passDesc setSamplePositions:count:]`. NV2A never used custom positions,
so default `MTLDefaultSamplePositions` are correct.

**Entry**: M10 complete.

**Exit**: `XEMU_METAL_MSAA=4` on PGR2 + Rainbow + Crimson + SC2 +
one broader title at scale=2 visibly reduces aliasing without
measurable FPS loss and without visual regressions.

**Gate**: visual smoke (zoomed screenshot of edges) + counter check
(`METAL_MSAA_RESOLVE_US_TOTAL`). Performance: ≤ 5 % FPS regression
from `=0` baseline.

**Risk**: low. Apple's TBDR makes MSAA cheap; the only risk is
pipeline-variant explosion if MSAA is changed at runtime — mitigated by
treating MSAA as a session-fixed option (require restart to change).

**v1 storage-mode deviation.** The plan calls for memoryless. M11
v1 ships `MTLStorageModePrivate` because xemu's per-`flush_draw`
render-pass cadence needs `MTLLoadActionLoad` on inter-draw passes
to preserve prior content, and Load is undefined on Memoryless.
The bandwidth cost stays trivial on Apple TBDR (multisample work is
in tile memory regardless of storage class); the storage cost is
~16 MiB extra of unified memory at MSAA 4× / surface_scale=2.
Memoryless returns when a future slice (candidate M11.1) coalesces
per-frame draws into a single render pass.

**v2 store-policy correction (2026-05-04).** Do not use
`MTLStoreActionMultisampleResolve` when a later render pass will load
the same MSAA texture; Metal only guarantees the single-sample
`resolveTexture` is written. PGR2 reproduced this as a mostly black
MSAA4 frame before the fix. Validated after the fix in
`docs/apple-silicon/benchmarks/2026-05-04-metal-msaa-store-validation.md`.

**v1 counter caveat.** `METAL_MSAA_RESOLVE_US_TOTAL` is a
placeholder (1 µs per resolve) until M13's counter sample buffers
wire actual GPU-side timing. Headline value in the meantime is
`METAL_MSAA_RESOLVE_COUNT`; the µs counter exists for forward-
compat with the exit gate's `< 200 µs / frame` threshold but should
be treated as not-yet-measured.

### M12 — MetalFX spatial scaler (graphical enhancement) — **SHIPPED 2026-05-02 (spatial scaler; temporal scaler intentionally not implemented)**

**Scope.** Wire `MTLFXSpatialScaler` as an opt-in upscale path: render
internal at 720p or 1080p, scale to 1440p or 2160p on present.
`XEMU_METAL_FX_SCALE={1,2,3}` (default 1 = no upscale). Inputs are the
post-resolve color and the output drawable; no motion vectors needed.

Defer `MTLFXTemporalScaler` until a per-title evaluation finds it
useful — synthesizing motion vectors from camera-only reprojection is
risky on dynamic scenes.

**Entry**: M11 complete.

**Exit**: `XEMU_METAL_FX_SCALE=2` produces visibly sharper output than
bilinear upscale at < 1 ms scaler cost on M3.

**Gate**: counter `METAL_FX_SPATIAL_US_TOTAL`.

**Risk**: low. MetalFX spatial is API-clean.

**Status (2026-05-02): SHIPPED with framework-cost-only counters
and on/off-only semantics; visual + perf exit gate deferred to
user-driven validation.**

`XEMU_METAL_FX_SCALE={1,2,3}` parses at `xemu_metal_init`. The
numeric value is preserved for forward-compat with future
quality-tier variants — the present implementation is on/off
(`>=2` enables; `1`, `0`, unparseable disable), with the actual
upscale ratio determined implicitly by `drawable_size /
input_size`. The scaler instance + private intermediate output
texture are built lazily on the first present that supplies an
NV2A framebuffer texture, and rebuilt whenever input dimensions /
pixel format / drawable size change.

Pipeline flow:

```
NV2A color RT (post-M11 resolve)
  → MTLFXSpatialScaler.encodeToCommandBuffer:    (before HUD render
                                                   encoder opens —
                                                   discrete pass op)
  → private intermediate texture (drawable_size, BGRA8Unorm_sRGB,
                                  MTLStorageModePrivate, usage =
                                  ShaderWrite | ShaderRead |
                                  RenderTarget)
  → existing fullscreen-triangle present pipeline
  → drawable
```

`colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual`
matches the M11-resolved sRGB-tagged color RT input. The scaler is
bypassed for the frame whenever the drawable is at-or-below the
input dimensions (downscale would add latency for no quality
win). Composes orthogonally with `XEMU_DISPLAY_SCALE` /
`surface_scale` and `XEMU_METAL_MSAA`: the scaler input is the
post-M11-resolve color RT, so e.g. `surface_scale=2` +
`XEMU_METAL_MSAA=4` + `XEMU_METAL_FX_SCALE=2` runs MetalFX from a
1080p 4×-multisampled resolved texture up to drawable resolution.

Files edited: `meson.build` (adds `MetalFX` to the Apple
Silicon `appleframeworks` modules list), `ui/xemu-metal.mm`
(parser + state + build helper + encode site + counters),
`util/xemu-metal-perf.c` (weak-symbol defaults + per-interval
emit), `scripts/apple-silicon/extract-perf-summary.sh` (three new
keys; count bumped from 148 → 151), `xemu-fork/CLAUDE.md` (moves
flag from "Planned" to "Stable opt-in"),
`docs/apple-silicon/automation.md` (full env-var spec + counter
documentation).

New counters: `METAL_FX_SPATIAL_PRESENTS` (per-interval scaler
invocations; only ticks when the scaler engaged for the frame),
`METAL_FX_SPATIAL_US_TOTAL` (CPU-side wallclock for the encode
call — under-reports GPU-side scaler cost; real GPU timing
arrives with M13's counter sample buffers), `METAL_FX_SCALE_FACTOR`
(latched effective config: 1 = off, >= 2 = on).

**Honest scope:**
- The "< 1 ms scaler cost on M3" exit gate cannot be measured
  precisely until M13 (counter sample buffers). M12's counter is
  a CPU-side placeholder that proves the encode call fires.
- `MTLFXTemporalScaler` intentionally not implemented per plan —
  synthesizing motion vectors from camera-only reprojection is
  risky on dynamic scenes (NV2A has no native motion vectors).
  Per-title evaluation remains a follow-up consideration.
- The scaler only engages when the drawable is larger than the
  input. With the project's default `surface_scale=2` (1080p-class
  internal render) on a 1080p-class drawable, the scaler is a
  no-op. MetalFX delivers visible quality uplift in two regimes:
  (a) `XEMU_DISPLAY_SCALE=1` (480p-class native NV2A) on a 1440p+
  drawable; (b) 4K+ display where even 1080p `surface_scale=2`
  is sub-drawable.
- Visual exit gate (visibly sharper than bilinear) deferred to
  user-driven Metal validation session per CLAUDE.md rule #10.

See decision-log entry "2026-05-02: Metal slice M12 — MetalFX
spatial scaler".

### M13 — Frame capture + counter sampling (advances 4h) — **SHIPPED 2026-05-02 (per-stage counter sampling on present pass; NV2A draw passes deferred)**

**Scope.** Programmatic capture via `MTLCaptureManager` gated on
`XEMU_METAL_CAPTURE=path.gputrace`. Counter sampling via
`MTLCounterSampleBuffer` at stage boundaries; surface as `METAL_*_US`
keys in `extract-perf-summary.sh`. Add `--metal-capture <path>` to
`run-benchmark.sh`.

`Info.plist` gains `MetalCaptureEnabled = YES` for development builds.
Document in `automation.md`.

**Entry**: M12 complete.

**Exit**: a `.gputrace` file produced from a benchmark run opens
correctly in Xcode's GPU Frame Capture viewer; counter rows appear in
`extract-perf-summary.sh` output for the same run.

**Gate**: end-to-end smoke — open a captured trace, navigate to an
NV2A draw, see the bound resources.

**Risk**: low. Documented Apple flow.

**Status (2026-05-02): SHIPPED.** Both halves implemented and the
build passes the M5 harness (7/7) on M3 Ultra.

* **Programmatic capture.** `start_metal_capture_if_requested()` in
  `xemu_metal_init` reads `XEMU_METAL_CAPTURE` and (if set) calls
  `[MTLCaptureManager sharedCaptureManager]
  startCaptureWithDescriptor:error:]` with a descriptor whose
  `captureObject = device`, `destination =
  MTLCaptureDestinationGPUTraceDocument`, and `outputURL =
  fileURLWithPath:`. Bounded by `XEMU_METAL_CAPTURE_FRAMES`
  (default 60; `0` = until shutdown) so the resulting `.gputrace`
  stays small enough to open in Xcode. `stop_metal_capture_if_active`
  fires from inside the per-frame `addCompletedHandler` once the
  frame target is reached, and again from `xemu_metal_shutdown` as
  a safety net. Failure modes (preconditions unmet, unwritable
  path) log a single fprintf and continue without capture — never
  abort the run. `MetalCaptureEnabled = YES` added to `Info.plist`
  with a comment noting the dev-vs-prod distinction.
* **Counter sampling.** `build_counter_sample_buffer_if_supported()`
  allocates a 4-sample `MTLCounterSampleBuffer` against
  `MTLCommonCounterSetTimestamp` and gates on
  `[device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]`
  (Apple7+ M1+; logged at startup as
  `xemu-perf: metal_counter_sampling enabled (...)` on M3 Ultra).
  The present render pass descriptor's
  `sampleBufferAttachments[0]` references the buffer with indices
  `(0,1)` for the vertex stage boundary and `(2,3)` for the
  fragment stage boundary; the per-frame `addCompletedHandler`
  resolves the buffer via `[buffer resolveCounterRange:]` and
  accumulates `(end - start) / 1000` µs into atomic counters.
  Apple Silicon timestamps are nanoseconds (no timebase
  conversion needed).
* **CPU-side wallclock placeholder replacement.** The
  `addCompletedHandler` also reads `cmdbuf.GPUStartTime /
  GPUEndTime` and accumulates the delta into
  `METAL_PRESENT_GPU_US_TOTAL` / `METAL_PRESENT_GPU_FRAMES`. When
  the MetalFX scaler encoded into the cmdbuf, the same delta also
  goes to `METAL_FX_SPATIAL_GPU_US_TOTAL` (an upper bound that
  includes the present + HUD encoder; a scaler-isolated value
  would require a dedicated cmdbuf, deferred). M12's CPU-side
  `METAL_FX_SPATIAL_US_TOTAL` and M11's nominal-cost
  `METAL_MSAA_RESOLVE_US_TOTAL` are kept as-is (no GPU-side
  replacement for MSAA's resolve cost in v1; that requires
  attaching a sample buffer to the surface-manager render passes,
  which is the next slice's work).
* **Counters wired.** `METAL_VERTEX_US_TOTAL`,
  `METAL_FRAGMENT_US_TOTAL`, `METAL_PRESENT_GPU_US_TOTAL`,
  `METAL_PRESENT_GPU_FRAMES`, `METAL_FX_SPATIAL_GPU_US_TOTAL`,
  `METAL_CAPTURE_FRAMES`, `METAL_CAPTURE_ACTIVE` all surface on
  the `xemu-perf:` interval line and are picked up by
  `scripts/apple-silicon/extract-perf-summary.sh` (count bumped
  151 → 158).
* **Benchmark-harness flag.** `--metal-capture <path>` added to
  `scripts/apple-silicon/run-benchmark.sh`; exports
  `XEMU_METAL_CAPTURE` for the spawned xemu and writes
  `metal_capture_path` plus `env_XEMU_METAL_CAPTURE` /
  `env_XEMU_METAL_CAPTURE_FRAMES` to the run's `metadata.txt`.

**Honest limits.** (a) Counter sampling instruments only the
present render pass; NV2A draw passes are out of scope for this
slice (lands in a future slice that wires sample buffers into
`pgraph_mtl_*_draw`). (b) Per-pass GPU time for the MetalFX
scaler in isolation is not measurable without a dedicated cmdbuf
— the v1 `METAL_FX_SPATIAL_GPU_US_TOTAL` is the full cmdbuf
upper bound. (c) M11's `METAL_MSAA_RESOLVE_US_TOTAL` keeps its
nominal-cost placeholder; replacing it requires sample buffers
on the surface-manager render passes, which is a separate piece
of work. (d) `METAL_COMPUTE_US_TOTAL` was reserved in plan §3.11
but is not implemented — the renderer has no compute encoders
yet (MetalFX uses its own internal compute, opaque from the
sample-buffer perspective).

### M14 — Hardening + macOS deployment-target lift — **SHIPPED 2026-05-02**

**Scope.** Confirm `build.sh` macOS deployment target is appropriate for
Metal. **Note (2026-05-02, post-M0):** `build.sh:212` already sets
`-target arm64-apple-macos14.0` / `-mmacosx-version-min=14.0` for the
arm64 path, which is the Metal-renderer-required floor. **No lift
needed in M14 for the arm64 (Apple Silicon) build** — the deployment
target is already at the macOS 14 line that unlocks
`CAMetalDisplayLink` (M10). The only deployment-target work that
might land here is documenting the floor explicitly in the user-
facing build instructions and removing any vestigial macOS 12 / 13
fallback comments. Set `MTL_DEBUG_LAYER=0` in shipped builds; add
`XEMU_METAL_VALIDATION=1` opt-in for development. Add the
`XEMU_METAL_*` flags to `automation.md`. Update
`extract-perf-summary.sh` to surface every `METAL_*` counter. Append
a comprehensive decision-log entry capturing all choices.

**Entry**: M13 complete.

**Exit**: a clean build from scratch on a fresh macOS 14 machine,
following the existing project build instructions, produces a
runnable Metal-default xemu app and passes
`validate-native-tri-depth.sh --run 22` end-to-end.

**Gate**: full doc reconciliation via `/sync-docs`. Decision-log entry
appended.

**Risk**: low.

**Status (2026-05-02): SHIPPED.** Implementation:
- `XEMU_METAL_VALIDATION={0,1}` lands in `ui/xemu-metal.mm`
  (`xemu_metal_apply_validation_env`). Promotes `MTL_DEBUG_LAYER=1`
  via `setenv(..., overwrite=0)` **before** the first
  `MTLCreateSystemDefaultDevice()` call, which is the only point at
  which Apple's Metal framework reads `MTL_DEBUG_LAYER`. If the user
  has already pinned `MTL_DEBUG_LAYER` themselves, the value is
  preserved. Surfaced once at startup as `xemu-perf: metal_validation
  requested=R promoted=P mtl_debug_layer_active=A`. Default 0 in
  shipped builds (matches the M14 "MTL_DEBUG_LAYER=0 in shipped
  builds" rule).
- `build.sh` confirmed at `arm64-apple-macos14.0` for the arm64
  path; **no lift needed** (Q6 resolved).
- `automation.md`: `XEMU_METAL_VALIDATION` documented in the
  renderer-selection section alongside the M5 / M7 / M7.1 / M8 / M9
  / M10 / M11 / M12 / M13 flags.
- `extract-perf-summary.sh`: full audit confirms every shipped
  `METAL_*` counter (50 keys: M3/M4 draw-counts, M5 translator/
  validation, M6 textures + cache, M7/M7.1 translated pipeline,
  M8 async compile, M9 persistent cache, M10 present jitter, M11
  MSAA resolve, M12 MetalFX spatial, M13 GPU-side per-stage timing
  + capture state) is surfaced. M14 itself adds no new counters —
  the `metal_validation` log line is a startup banner, not a
  per-interval counter.
- `xemu-fork/CLAUDE.md` and the workspace `CLAUDE.md` updated to
  reflect M0–M14 SHIPPED + the user-driven testing entry point for
  M15. `XEMU_METAL_VALIDATION` moved from "Planned" to "Stable
  opt-in".
- `strategy.md` Phase 4 sub-deliverables 4a–4i annotated with their
  shipping M-slice; 4j (MSAA), 4k (MetalFX), 4l (hardening) added as
  M-cycle additions.

**Exit-gate verification.**
- Clean build from `./build.sh -a arm64`: PASS. `dist/xemu.app/
  Contents/MacOS/xemu --version` reports the expected version and
  starts cleanly.
- `validate-native-tri-depth.sh --run 22`: PASS (7/7 PASS lines:
  `final_intervals=1`, `NATIVE_TRI_DEPTH_DRAW=243784`,
  `NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST=480`,
  `NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST=284`,
  `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST=480`,
  `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST=284`,
  `GEOM_SHADER_DRAW_TRI=284 matches NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`).
  Confirms the GL renderer regression gate is intact under M14's
  changes.
- M5 shader-validation harness: 7/7 PASS via
  `scripts/apple-silicon/metal-shader-validation/run-validation.sh`.
- `XEMU_METAL_VALIDATION=1` smoke test: env-var promotes
  `MTL_DEBUG_LAYER`, startup banner reads `metal_validation
  requested=1 promoted=1 mtl_debug_layer_active=1`. Inverse smoke
  test (env unset): `requested=0 promoted=0
  mtl_debug_layer_active=0`.

**Caveat — "Metal-default" wording in the original M14 exit
criteria.** The plan-text exit reads "produces a runnable
Metal-default xemu app". M15 is the slice that flips Metal default-on
per the M-cycle structure — M14 keeps Metal opt-in via
`display.renderer = METAL` (or `XEMU_RENDERER=METAL` on the
command line). The validate-native-tri-depth.sh gate is
graphics-API-agnostic from the project's perspective (it tests the
`flat-tri-depth` XBE through the GL renderer's counter-split logic;
the same XBE will be re-run through the Metal renderer at M15's
default-on decision).

**M-cycle summary.** M14 closes the implementation cycle for
slices M0–M14 and opens the user-driven validation window before
M15. See decision-log "2026-05-02: Metal slice M14 — hardening,
doc reconciliation, M-cycle summary".

### M5.9 — Per-VRAM surface cache + CRTC-aware publish — **SHIPPED 2026-05-03 (architectural fix; deferred items catalogued)**

**Status (2026-05-03): SHIPPED.** The architectural fix is in place
— the per-vram_addr surface cache exists, lookup helpers
(`pgraph_mtl_surface_get_at`, `_get_within`) match the vk shape, the
CRTC-aware publish runs from `flip_stall`, and the new
`METAL_FRONT_FB_PUBLISHES` counter + per-publish diagnostic line
confirm distinct surfaces are routed to distinct cache entries with
the CRTC publish picking the front-buffer (vram_addr=0x32a4000,
1280×960 — exact match for PGR2 surface_scale=2). Tasks 1, 2, 3, 6,
7, 8, 9 below are all done. Tasks 4 and 5 (CPU-write callbacks +
VRAM upload/download) are deferred as M5.9-followup-A/B/C/D — the
remaining magenta artifact in PGR2 captures is now traced to the
*absence* of a back-buffer-to-front-buffer copy path
(`NV097_IMAGE_BLIT` is still a stub), not to the surface routing.
See decision-log "2026-05-03: Metal slice M5.9 — per-VRAM surface
cache + CRTC-aware publish". M15 default-on stays BLOCKED on
M5.9-followup-A (image_blit) plus the deferred items.

**Followup-A (NV097_IMAGE_BLIT GPU-side surface copy) — SHIPPED
2026-05-03**, but PGR2 doesn't issue this op (`METAL_IMAGE_BLITS=0`
in benchmarks). The plumbing is correct for titles that do issue it.

**Followup-B+C (CPU-write dirty tracking + VRAM upload) — SHIPPED
2026-05-03, visual gate NOT met**. The infrastructure is correctly
wired: access callbacks register via `mem_access_callback_insert`
under TCG, the callback fires the dirty bit on guest writes, the
upload helper consumes the bit, three new counters
(`METAL_SURFACE_VRAM_DIRTY_HITS`, `METAL_SURFACE_VRAM_UPLOADS`,
`METAL_SURFACE_VRAM_UPLOAD_BYTES`) measure all of it. PGR2 90 s
benchmark: `METAL_SURFACE_VRAM_UPLOADS=2/interval`,
`METAL_SURFACE_VRAM_DIRTY_HITS=0/interval`. Captured PNGs still
show the cleared-color sub-rect + heap-default magenta — PGR2 is
NOT using a CPU-write swap mechanism. The decision-log entry of
this date (§"Investigation — what PGR2's buffer-swap mechanism
is NOT") rules out all three candidate mechanisms — the actual
mechanism is unknown and demands a fourth followup with
per-vram_addr draw-target instrumentation.

**Followup-D (surface download for read-from-RT) — STILL DEFERRED.**

**Followup-E (surface-cache color/depth split + front-fb pin + cap raise) — SHIPPED 2026-05-03.**
Three real bugs identified via the new per-vram_addr `metal_draw_target`
diagnostic counter (instrumented at `pgraph_mtl_flush_draw`):

1. **Color/depth cache collision.** `cache_get_at(addr)` was unfiltered
   by `is_color`, so a same-vram_addr color/depth alternation thrashed
   each prior binding via the destroy-and-recreate path. PGR2 hits
   this on the legacy `vram_addr=0` ensure-by-shape sentinel plus a
   real surface where `dma.address+offset=0`. Split into
   `cache_get_at_color` / `cache_get_at_depth`. New counter
   `METAL_SURFACE_RECREATE_SHAPE_MISMATCH` was 6/interval before fix;
   0/interval after.
2. **LRU eviction of stably-published front-fb.**
   `pgraph_mtl_surface_publish_front_fb` returned early on dedupe
   without bumping `last_use_seq`, so the front-fb's LRU score grew
   stale and the cache picked it as eviction victim — destroying the
   texture the compositor was reading. Pin the published texture in
   `cache_evict_lru` + bump `last_use_seq` on every publish call.
3. **Cache cap=16 too small.** Raised to 32. Steady-state
   `METAL_SURFACE_CACHE_SIZE` now grows past 16 (observed 21 on PGR2);
   a previously-evicted draw target reappeared in the diagnostic.

**Codex-validate HIGH finding fix:** the shape-mismatch recreate path
(color side) now also clears `s_front_framebuffer_texture` if it
equals the destroyed entry's texture — symmetric with the LRU pin so a
guest-side surface reconfiguration can't reproduce the magenta class
via the destroy route.

**Magenta artifact closed; visual gate STILL FAILS** because PGR2
renders to back buffer `0x3628000` (~1500 draws/s confirmed by the
new `metal_draw_target` counter) but the CRTC publishes
`0x32a4000` and Metal has no mechanism to bridge them. GL handles
this via the display-side `get_framebuffer_surface` callback that
reads VRAM at host-vsync time; Vulkan via
`pgraph_vk_surface_download_if_dirty`. M5.10 below.

See decision-log "2026-05-03: Metal slice M5.9-followup-E —
surface-cache color/depth split + front-fb pin + cap raise" for the
full investigation, the per-vram_addr draw distribution measurements
on PGR2, and the codex-validate review notes.

### M5.10 / M5.11 — VRAM-coherent surface download + PGR2 surface/RTT follow-up — **SHIPPED 2026-05-04 (PGR2/Rainbow/Halo/boot MSAA4 PASS; Crimson/SC2 visual routes blocked)**

**Status (2026-05-04): SHIPPED for the PGR2/Rainbow/Halo/boot canaries;
Metal default-on remains blocked by the broader Metal-vs-GL gameplay
and visual-diff gate, the front-fb fallback policy, and Crimson/SC2
routed visual capture gaps.**

- **Commit `b283abcb27`** — M5.10 base infrastructure. Public download
  API (`pgraph_mtl_surface_download_if_dirty_at` / `_dirty_all` /
  `_in_range_if_dirty`) mirroring
  `vk/surface.c::pgraph_vk_surface_download_if_dirty` field-for-field;
  cross-queue `MTLSharedEvent` fence; KVM/HVF parity polling
  (TCG-gated); open-pass pin in `cache_evict_lru`. Gated behind
  `XEMU_METAL_FRONT_FB_DOWNLOAD={0,1}` default 0. Codex-validate ran;
  4 findings (2 HIGH, 2 MEDIUM) all addressed in-slice.
- **Commit `5154cb599b`** — M5.10 experimental fallback. Additional
  opt-in path that does NOT depend on VRAM coherency: when
  `XEMU_METAL_FRONT_FB_FALLBACK={0,1}` is set, `flip_stall` calls
  `pgraph_mtl_surface_publish_latest_draw_fallback()` after the
  CRTC-strict publish, publishing `s_color_binding` (most-recently-
  bound color RT) as the front-fb side-channel; last write wins.
  Bridges PGR2's back→front gap host-side. NOT correctness-faithful
  (aspect mismatch on many titles).

**2026-05-04 follow-up result.** The old PGR2 white/magenta/front-fb
failure is closed by a set of targeted surface/RTT fixes:

- Scaled VRAM upload now fills the full host-scaled texture instead of
  only the 1x guest sub-rect, avoiding uninitialized regions.
- The display-compose fallback is upright and can publish the selected
  render target directly.
- The surface cache can retain multiple shapes for the same VRAM
  address, uses exact/near shape lookup, caps at 64 entries, and walks
  all same-VRAM siblings for access-callback registration and dirty
  upload. PGR2 no longer churns shapes:
  `METAL_SURFACE_RECREATE_SHAPE_MISMATCH=0`.
- Texture lookup for render-target-as-texture is dimension-aware, so
  same-address surfaces with different shapes do not alias silently.
- A8R8G8B8 render targets sampled as linear A8R8G8B8-family texture
  views take the CPU texture path instead of the direct surface fast
  path. This fixes PGR2's dotted/yellow text and channel/alpha
  normalization mismatch. Diagnostic env:
  `XEMU_METAL_DISABLE_SURFACE_TEX_ADDRS=1`.

**Validation (2026-05-04).**

- PGR2 MSAA4 PASS after store/resolve fix:
  `benchmark-runs/20260504-100458-pgr2`,
  `benchmark-runs/visual-checks/pgr2-gate-metal-msaa4-f900-after-msaa-store.png`.
  `METAL_PIPELINE_TRANSLATED_FAILED=0`,
  `METAL_DRAWS_SKIPPED_PENDING_TOTAL=0`,
  `METAL_SURFACE_RECREATE_SHAPE_MISMATCH=0`; `post_load_avg_fps=42.12`.
- PGR2 without `XEMU_METAL_FRONT_FB_FALLBACK=1` still produces a wrong
  upside-down frame (`benchmark-runs/20260504-101416-pgr2`), so the
  fallback dependency remains explicit.
- Rainbow Six 3 MSAA4 PASS for the loading-screen canary:
  `benchmark-runs/20260504-100546-rainbow-six-3`,
  `benchmark-runs/visual-checks/rainbow-gate-metal-msaa4-f600-after-msaa-store.png`.
- Xbox boot/flubber MSAA4 PASS:
  `benchmark-runs/20260504-100747-crimson-skies`,
  `benchmark-runs/visual-checks/boot-gate-metal-msaa4-f300-after-msaa-store.png`.
- Halo CE MSAA4 PASS:
  `benchmark-runs/20260504-101125-halo-ce`,
  `benchmark-runs/visual-checks/halo-gate-metal-msaa4-f1200-after-msaa-store.png`.
- Crimson Skies gameplay stability PASS, visual route BLOCKED:
  `benchmark-runs/20260504-100815-crimson-skies` completes without
  aborting, but interval screenshots are one patterned frame followed
  by black drawable captures.
- SC2 perf/stability route PASS, visual route BLOCKED:
  `benchmark-runs/20260504-101242-soul-calibur-2` reaches
  `post_load_avg_fps=57.63`, but the no-input route captures
  boot/flubber and then black frames.

**Highest-priority next-session action (2026-05-11 evening).** Start with
`scripts/apple-silicon/m15-bundle-status.py`. The next engineering blocker is
gameplay evidence, not oracle health: `metal-gl-compare.sh --trigger flip`
now uses GL `XEMU_GL_SCREENSHOT_PATH` and Metal
`XEMU_METAL_SCREENSHOT_SOURCE=nv2a`, but PGR2/Rainbow latest passes are only
static canaries. The first sequence-based PGR2 attempt failed after strict GL
window capture and GL viewport cropping:
`benchmark-runs/m15-gameplay-pgr2-windowgl-20260511-182317/diagnostic-relaxed-align/`
shows Metal NV2A title/profile captures diverging from GL/oracle with flat or
missing background detail. Next session should inspect that contact sheet,
decide whether the NV2A screenshot source or the live Metal renderer is wrong,
then rerun PGR2 before moving to Rainbow/Halo/Crimson/SC2 paired visual+perf
routes with `XEMU_PERF_FRAME_LOG=1`.

M15 default-on stays BLOCKED on:
1. Matched gameplay keyframe evidence for paired GL-vs-Metal capture.
2. Paired Metal-vs-GL gameplay visual diff on PGR2, Rainbow, Crimson, SC2,
   and one broader-sweep title.
3. Console-native FPS plus p99 jitter validation on the same set.
4. Cold-launch shader compile time < 5 s on a fresh shader cache.
5. Faithful front-fb/default presentation path or an accepted fallback
   policy.

See decision-log "2026-05-04: Metal PGR2 surface/RTT visual canary
passes; later boot/flubber correction supersedes Crimson blocker
framing" and "2026-05-04: Metal MSAA store/resolve bug fixed; M15
still blocked by visual routes and front-fb policy", plus
`docs/apple-silicon/benchmarks/2026-05-04-metal-pgr2-surface-rtt-validation.md`
and `docs/apple-silicon/benchmarks/2026-05-04-metal-msaa-store-validation.md`.
The original "PENDING" description below is preserved for the audit
trail; the planned tasks are partially-complete (Path A primitives
shipped; Path B compositor rework deferred; codex MEDIUM/LOW from
M5.9-followup-E still partially deferred).

**Original PENDING text (preserved):**

**Cause.** AAA Xbox titles like PGR2 render the final scene to a
back-buffer at one VRAM address (e.g. PGR2's 1280×480 supersampled
back at `0x3628000`) while the NV2A CRTC scans a different VRAM
address (PGR2's 640×480 front at `0x32a4000`). On real Xbox the
hardware NV2A engines move pixels between these via mechanisms the
guest software relies on (potentially: cycled `pcrtc.start` writes
mid-frame, hardware engine DMAs, the BACK_END_WRITE_SEMAPHORE +
report flow, an undocumented blit subchannel, or a software
post-process draw-pass that reads the back buffer as a texture and
writes to the front buffer). The Metal renderer's per-vram_addr
cache today **does not propagate rendered GPU content from the
back-buffer to the CRTC-published front-fb**, so the published
texture stays empty of scene content.

**Two implementation paths** (decide based on vk vs gl architecture
fit):

- **Path A — port vk's surface download.** Mirror
  `pgraph_vk_surface_download_if_dirty` (vk/surface.c:939-944),
  including its callsites in `surface_access_callback`,
  `invalidate_overlapping_surfaces`, `expire_old_surfaces`, and
  `pgraph_vk_surface_flush`. The download writes rendered MTLTexture
  pixels back to guest VRAM. The CRTC-publish path then reads VRAM at
  `pcrtc.start + line_offset` and uploads from VRAM into the
  published texture. Requires implementing a GPU→VRAM blit via
  `MTLBlitCommandEncoder copyFromTexture:toBuffer:` + a `synchronize`
  to ensure CPU visibility on Apple Silicon UMA.
- **Path B — register `get_framebuffer_surface` ops callback.**
  Mirror gl's display flow: register the renderer-ops callback,
  remove the side-channel publish via
  `s_front_framebuffer_texture`, have the display code call
  `pgraph_mtl_get_framebuffer_surface(d)` at host-vsync time, and
  have that function look up `pcrtc.start + line_offset` in the
  per-vram_addr cache and return the matching MTLTexture handle.
  Requires also handling the case where the lookup misses (PGR2's
  back-buffer might be the actual scene target, but the CRTC-pointed
  front-fb misses — need a fallback that returns the
  most-recently-rendered surface, or upload from VRAM lazily).

Path A is the closer architectural match to vk and reuses the
existing publish path. Path B is the cleaner architectural match to
gl and removes a Metal-specific side-channel.

**Entry**: M5.9-followup-E shipped (it is).

**Exit**: PGR2 renders visually-correct gameplay through the Metal
renderer; back-buffer-rendered scene content reaches the displayed
front-fb texture. ≤ 1 % per-pixel diff vs GL on combiner-correct
surfaces. Validated via `XEMU_METAL_SCREENSHOT_PATH=` capture during
a benchmark run that reaches gameplay (use the `pgr2_gameplay_b4`
mid-route snapshot for fast iteration).

**Concrete tasks**:

1. Survey vk's full download flow and decide Path A vs Path B.
2. (Path A) Port `pgraph_vk_surface_download_if_dirty` and its
   callsites; add `METAL_SURFACE_DOWNLOADS` counter; wire into
   `pgraph_mtl_flip_stall` before the publish.
3. (Path B) Register `get_framebuffer_surface` in the Metal ops
   table; have it walk the cache for `pcrtc.start + line_offset`;
   remove the side-channel publish; ensure the side-channel
   `pgraph_mtl_get_framebuffer_metal_texture` falls back to the new
   getter.
4. Address the codex-validate MEDIUM finding from M5.9-followup-E:
   `register_access_cb_for` / `_unregister_access_cb_for` use
   unfiltered `cache_get_at` and can hit the wrong aspect now that
   color and depth coexist at the same vram_addr. Either move
   callback tracking to a separate vram-range registry, or have the
   register/unregister helpers walk all matching entries.
5. Address the codex-validate LOW finding: extend
   `pgraph_mtl_surface_get_color_vram_addr` to return
   `{has_binding, vram_addr}` so the `metal_draw_target` diagnostic
   stops conflating "real vram_addr=0", "ensure-by-shape fallback",
   and "no color binding" into a single `metal_draw_target_zero`
   bucket.
6. Investigate PGR2's actual back→front mechanism via the new
   diagnostic — the `metal_draw_target` counter showed only 3-4
   draws/interval to `0x32a4000` and ~1500 to `0x3628000`. If neither
   the surface-download nor the get_framebuffer_surface paths fix
   PGR2, we need to identify the missing engine class (NV3089
   scaled-blit and NV0039 M2MF are both unimplemented in xemu but
   PGR2 works on GL/Vulkan, so PGR2 doesn't use them; the actual
   mechanism is unknown).
7. Run paired Metal vs GL benchmarks across PGR2 / Crimson / Rainbow
   / SC2 (the M15 visual-diff gate).

**Risk**: Path A's GPU→VRAM blit + synchronize per-frame adds Metal
renderer cost. On Apple Silicon UMA the cost should be modest (no
real copy, just barrier+coherence) but needs measurement.

**Scope.** Backfill the per-VRAM-address surface cache that M2
deferred and that subsequent slices M3–M14 built on top of without
addressing. ~1200 LOC port of the relevant subset of `vk/surface.c`
(currently 1760 LOC; the mtl surface.mm is 588 LOC and is a
single-slot M2-era manager). The cause this slice closes is documented
in decision-log "2026-05-03: Metal magenta root-caused — missing
per-VRAM surface cache + CRTC-aware publish".

**Entry**: M14 complete (it is).

**Exit**: PGR2 / Crimson / Rainbow / SC2 render visually-correct
output through the Metal renderer at this commit's pipeline / draw /
attribute / shader infrastructure. The published front-fb matches
the NV2A CRTC-pointed surface, not "whichever surface was most
recently clear-bound".

**Concrete tasks**:

1. Replace `s_color_binding` / `s_depth_binding` with a `QTAILQ`-backed
   per-VRAM `SurfaceBinding` cache analogous to
   `vk/surface.c::PGRAPHVkState.surfaces`. Keys: `vram_addr` + `size`
   + `width` + `height` + `nv097_format` + `is_color`. LRU eviction
   (capped count, e.g. 64 entries) plus invalidation-driven eviction
   when the underlying VRAM range is dirtied by CPU writes.
2. Add `pgraph_mtl_surface_get(d, addr)` and
   `pgraph_mtl_surface_get_within(d, addr)` lookup helpers — exact
   ports of `vk/surface.c:697-724`.
3. Compute `vram_addr` for the current bind from
   `NV097_SET_SURFACE_OFFSET_COLOR` / `_ZETA` (plus `surface_scale`
   awareness). Pattern: `vk/surface.c::pgraph_vk_surface_update`.
4. Wire `pgraph_mtl_surface_update` (currently a stub at
   `mtl/renderer.c:963`) to call into the cache for upload (CPU→tex)
   and download (tex→CPU). Required so that newly-bound RTs pick up
   CPU-modified VRAM contents and so that guest-side readback works.
5. Add CPU-write callbacks (`memory_region_set_client_dirty` +
   per-page invalidation range tracking) so that guest writes to a
   surface's VRAM range mark the GPU-side texture as stale. Pattern:
   `vk/surface.c::register_cpu_access_callback` +
   `invalidate_overlapping_surfaces`.
6. Replace `s_front_framebuffer_texture` (and its writes at
   `mtl/surface.mm:298` and `mtl/surface.mm:465`) with an on-demand
   `pgraph_mtl_surface_get_crtc_surface(NV2AState *d)` that mirrors
   `vk/renderer.c:172-205`'s `pgraph_vk_surface_get_within(d,
   d->pcrtc.start + vga_display_params.line_offset)` lookup. Adjust
   `pgraph_mtl_get_framebuffer_metal_texture` (currently
   `mtl/surface.mm:477-480`) to call it; preserve the
   `id<MTLTexture>` side-channel return semantics that the
   `pgraph_mtl_get_framebuffer_surface` doc-comment promises.
7. Add `METAL_FRONT_FB_PUBLISHES` always-on counter (per-interval
   delta) and a one-shot-on-change diagnostic
   `xemu-perf: metal_front_fb_publish vram_addr=0x.. width=W
   height=H format=FMT reason={crtc,clear,ensure}` so that future
   regressions of this class are detectable in counter logs without
   requiring screenshot inspection.
8. Audit the M5.5 / M5.7 open-pass coalescing logic
   (`mtl/draw.mm::open_pass_ensure`) for "currently-bound surface"
   assumptions that the new cache invalidates. The pass-key currently
   captures `(color_tex, depth_tex, color_fmt, depth_fmt,
   sample_count)`; with a per-VRAM cache the texture pointers will
   stay stable across binds (no more reallocation under us), so this
   should be net-simpler — but verify the open pass is **flushed**
   when the bound surface changes to a different cache entry.
9. Update `automation.md` with the `METAL_FRONT_FB_PUBLISHES`
   counter; extend `extract-perf-summary.sh` to surface it.

**Gate**: paired Metal-vs-GL screenshot capture of a PGR2 mid-route
frame (`pgr2_gameplay_b4` snapshot) with per-pixel diff ≤ 1 % on
combiner-correct surfaces. Existing `METAL_PIPELINE_TRANSLATED_FAILED
== 0` and `METAL_DRAW_TRANSLATED == METAL_DRAW_COUNT` floors hold.
M5 shader-validation harness still 7/7 PASS. Build PASS. No GL
renderer regression.

**Risks**:

- The CPU-write callback wiring touches `memory_region_*` APIs that
  cross the QEMU memory subsystem boundary. Mirror the vk impl
  closely; do not invent new patterns.
- LRU eviction of a still-referenced surface during an open
  coalesced pass would crash. Cache eviction must check
  `s_open_pass_key.color_tex` / `.depth_tex` and either skip or
  flush-then-evict.
- Surface upload from VRAM (task 4) is M5.9-Part-A scope. Surface
  download (also task 4, the read path) can ship as M5.9-Part-B if
  Part A is too large for one slice — Part-B isn't needed for
  visual correctness on PGR2/Rainbow/Crimson per the gl/vk reference
  (those titles do not require GPU→CPU readback for normal
  rendering; only `get_report` and screen-capture paths do, and the
  former is already a separate stub).

**Why this slice did not exist before.** M2 explicitly listed this
work as deferred; the M-cycle close-out (decision-log
"2026-05-02: Metal slice M14 — hardening, doc reconciliation,
M-cycle summary") catalogued specific deferred items (M6 Part B,
M8.1, M10.1, M11.1, NV2A draw-pass per-stage GPU timing,
`XEMU_METAL_DISABLE_LOSSLESS_COMPRESSION`) but did not catalogue
the full per-VRAM surface cache because the M2 banner placed it as
"M3+" generic deferral rather than naming a dedicated slice. The
2026-05-03 root-cause investigation (decision-log entry of same
date) is the trigger for promoting that deferral to its own
named slice.

### M15 — Default-on selection + GL fallback policy — **BLOCKED (gated on front-fb policy, Crimson/SC2 routes, and broader visual validation)**

**Scope.** Decide whether Metal becomes the default on Apple Silicon.
Decision rule: Metal becomes default-on when:
- All M0–M14 gates passed.
- 5 distinct titles (PGR2, Rainbow, Crimson, SC2, plus one from the V4
  broader sweep) run end-to-end through Metal at ≥ console-native FPS
  with visual diffs ≤ 1 % per-pixel from GL.
- Cold-launch shader compile time < 5 s total on a fresh shader cache.
- p99 mspf jitter reduced by ≥ 20 % vs GL on PGR2 + Rainbow + Crimson.
- The presentation/front-fb path is faithful enough for default-on, or
  the experimental fallback has an explicit accepted policy with title
  coverage evidence.
- No correctness-affecting bugs open against Metal renderer for ≥ 30
  days of continuous bench use.

If those criteria are met, flip the default. Otherwise, document the
shortfall in a decision-log entry, ship Metal as opt-in via
`display.renderer = METAL`, and queue the necessary follow-up slices.

**OpenGL deprecation** is an explicit **non-goal of this plan**. After
Metal default-on ships and is stable for ≥ 30 days, propose deprecation
in a separate decision-log entry.

---

## 5. Validation methodology

Each slice has a gate per §4. Cross-cutting validation patterns:

### Visual diff harness

The benchmark harness already supports paired baseline/native snapshot
runs with screenshot diffing
(`scripts/apple-silicon/native-tri-depth-compare.sh`). Extend with a
`--metal` mode that runs the same scene on the Metal renderer and
diffs against the GL screenshot. Per-pixel tolerance configurable;
default 1 % for combiner-correct slices, 5 % for slices that may
intentionally diverge (e.g. M5 before M7 lands).

Add a `scripts/apple-silicon/validate-metal.sh` modelled on
`validate-native-tri-depth.sh`. It runs the `flat-tri-depth` XBE
through both backends, compares screenshots, and reports.

### Performance counters

Every Metal-side counter goes through `extract-perf-summary.sh`:

- `METAL_CLEAR_COUNT`, `METAL_DRAW_COUNT`,
  `METAL_NATIVE_TRI_DEPTH_DRAWS`, `METAL_NATIVE_QUAD_DRAWS`.
- `METAL_PIPELINE_HITS`, `METAL_PIPELINE_MISSES`.
- `METAL_SHADER_COMPILE_QUEUED_TOTAL`,
  `METAL_SHADER_COMPILE_COMPLETED_TOTAL`,
  `METAL_DRAWS_USING_UBERSHADER_TOTAL`,
  `METAL_DRAWS_SKIPPED_PENDING_TOTAL`.
- `METAL_SHADER_CACHE_LOADS`, `METAL_SHADER_CACHE_HITS`,
  `METAL_SHADER_CACHE_MISSES`.
- `METAL_TEX_UPLOAD_BYTES_TOTAL`, `METAL_TEX_UPLOADS_TOTAL`.
- `METAL_VERTEX_US_TOTAL`, `METAL_FRAGMENT_US_TOTAL`,
  `METAL_COMPUTE_US_TOTAL` (counter-sample-buffer-driven, M13).
- `METAL_PRESENT_JITTER_US_MAX`, `METAL_PRESENT_JITTER_US_AVG`.
- `METAL_FX_SPATIAL_US_TOTAL` (M12).
- `METAL_MSAA_RESOLVE_US_TOTAL` (M11).

These give the project the same data-driven attribution surface that
`xemu-perf:` already provides for the GL path, satisfying project
rule #1.

### Apple Instruments + frame capture

Project rule #8: don't optimize from intuition. Each performance gate
(M4, M5, M7, M10, M11) requires an Instruments "Metal System Trace"
and an Xcode GPU Frame Capture artifact attached to the benchmark
note. Captures live under `benchmark-runs/<timestamp>-<scene>/captures/`.

### Correctness gates

Visual diff ≤ tolerance against the GL run on the same scene is the
universal correctness gate. For combiner-heavy slices (M7), the
tolerance tightens to ≤ 1 % per-pixel; for pre-combiner slices (M3–M6),
the tolerance is wider because combiner regions are expected to differ.
**A regression in PR #2240's depth/polygon-offset/flat-shading
behavior is a slice failure**, not a tolerable diff.

---

## 6. Risk register

### R1 — spirv-cross compatibility with our generated GLSL

**Severity**: HIGH. **Likelihood**: MEDIUM.

The existing GLSL generator was tuned to OpenGL semantics. spirv-cross
may not handle every construct cleanly when targeting MSL. Worst case:
constructs like `gl_FragCoord`-derived depth (the
`XEMU_NATIVE_TRI_DEPTH` path) or polygon-offset slope reconstruction
produce invalid MSL.

**Mitigation**: M5 includes a shader-validation harness as an entry
gate. If a construct breaks, decide per-case (patch the GLSL generator
or hand-write MSL for that one variant). Worst-worst case: fall back
to a fork-specific intermediate IR (per `strategy.md` Risks). Keep
M5 as a hard gate; do not commit M6+ until M5 passes.

### R2 — Pipeline-variant explosion

**Severity**: HIGH. **Likelihood**: HIGH.

NV2A's combinatorial state space (combiners × alpha test × fog ×
textures × point/line/poly mode) can produce thousands of unique
pipelines. Without function-constant specialization + persistent MSL
cache, first-launch shader compile bursts will reproduce or worsen the
OpenGL-on-Metal MTLCompilerService pain (xemu#1466).

**Mitigation**: M5 introduces the per-pipeline cache; M8 introduces
async compile + ubershader fallback; M9 introduces persistent MSL-source
cache. The mitigation is already sequenced into the plan.

### R3 — Framebuffer-fetch path divergence

**Severity**: MEDIUM. **Likelihood**: LOW (we're on Apple Silicon
exclusively for the shareable build).

Apple GPU + raster_order_group is the fast path; Intel-Mac fallback
(barrier-based pass split) is a second code path with its own
correctness surface. `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH=1` exists
to exercise the fallback for testing.

**Mitigation**: M7 builds both paths and validates each with the
combiner-heavy snapshot triplet. Intel-Mac users are not the target
audience for the shareable Apple Silicon build, but the fallback keeps
xemu functional there.

### R4 — Two presentation pipelines (macOS 13 vs 14+)

**Severity**: LOW. **Likelihood**: HIGH.

`presentDrawable:atTime:` and `CAMetalDisplayLink` are both maintained.

**Mitigation**: M10 keeps the two paths under a single function
boundary, both calling the same encoder code. Test matrix:
macOS 13 + macOS 14+ on the same scene.

### R5 — TCG-side judder is not solved by Metal

**Severity**: LOW (for the renderer plan; HIGH for messaging).
**Likelihood**: HIGH.

The 1.3 s class Crimson worst-frame is guest-intrinsic + TCG-overhead
per the 2026-05-02 V9/V10 attribution. Metal will improve presentation
smoothness and latency but cannot erase guest-engine or TCG-only
stalls.

**Mitigation**: do not oversell Metal in shareable-build messaging.
The Metal plan explicitly does not claim to solve this risk; the
"judder pillar" status in `handoff.md` and `strategy.md` already
records the reframing. The Metal plan's exit criteria targets p99
jitter, not 1.3 s outliers.

### R6 — `MTLBinaryArchive` for pipeline persistence is unreliable

**Severity**: MEDIUM. **Likelihood**: HIGH.

DuckStation's source explicitly disables it
(`m_features.pipeline_cache = false`); Dolphin sets
`bSupportsPipelineCacheData = false`. Limited macOS coverage.

**Mitigation**: M9 uses MSL-source caching, not `MTLBinaryArchive`.
Decision-log entry records the amendment to `strategy.md` Phase 4f.

### R7 — Main UI window architecture switch (GL → Metal)

**Severity**: MEDIUM. **Likelihood**: MEDIUM.

Moving the main SDL window from `SDL_WINDOW_OPENGL` to
`SDL_WINDOW_METAL` is a one-time, hard switch. While Metal is the
active renderer, the GL HUD path is unused; while GL is the active
renderer, the Metal HUD path is unused. Switching renderers requires a
restart.

**Mitigation**: M1 introduces this as the first user-visible change.
Test matrix: Metal-default boot, GL-default boot, switch via UI →
restart, both work. xemu's existing toml-driven config makes this
straightforward.

### R8 — Argument-buffer or other "skipped per-strategy.md" optimizations turn out to matter

**Severity**: LOW. **Likelihood**: LOW.

`strategy.md` "What we ruled out" disqualifies argument buffers, ICBs,
and mesh shaders based on Tellusim's MDI study. NV2A's draw count is
on the favorable side of the MDI win/lose curve, but the win is
meaningful only after per-draw shader compile cost is absorbed and the
bottleneck has moved into command-encoding overhead — rarely the case.

**Mitigation**: M13's counter sampling reveals if `METAL_VERTEX_US_TOTAL`
or per-encode CPU time is dominant. If it is, queue a follow-up slice
to introduce argument buffers. Until then, defer.

---

## 7. Open questions to resolve before slice M0

These are project rule #1 questions ("don't guess; gather data") that
should be answered before code lands. Answering them is cheap; getting
them wrong is expensive.

### Q1 — spirv-cross packaging

**Question**: ship spirv-cross as a meson subproject, a system pkg-
config dependency, or a vendored copy in `subprojects/`?

**Investigate**: how do other Meson-based projects (Mesa, Gnome
Shell?) handle spirv-cross? What does `ports/spirv-cross/portfile.cmake`
look like? Does Apple ship spirv-cross as part of the macOS Vulkan SDK
xemu already depends on?

**Decision before M5**: pick the packaging that's reproducible across
the project's CI matrix without bloating the source tree.

### Q2 — Two `MTLHeap`s or one?

**Question**: per Apple's docs, do not mix MSAA and depth-compressed
textures with normal color textures in the same heap. NV2A's color RT,
depth RT, MSAA RT, and texture pool all have different allocation
patterns. How many heaps?

**Investigate**: Dolphin's `MTLObjectCache.mm` for actual allocation
strategy. Apple Sample Code "Memoryless Render Targets".

**Decision before M2**: probably two heaps (textures + transient RTs);
allocate the multisample memoryless attachments outside any heap.

### Q3 — Persistent shader cache directory layout

**Question**: where does the MSL-source cache live? xemu's GL renderer
already has per-game GLSL cache in
`$XDG_DATA_HOME/xemu-project/xemu/shader_cache/<game-id>/`. Reuse that
directory or a sibling?

**Investigate**: GL renderer's cache directory plumbing
(`gl/shaders.c`'s `pgraph_gl_shader_cache_to_disk`).

**Decision before M9**: probably a sibling
`metal_shader_cache/<game-id>/<state-hash>.metal`. One file per shader,
simple invalidation.

### Q4 — Emulation-rate slewing — land on GL first?

**Question**: should the emulation-rate slewing slice (PCSX2 PR #5488 /
DuckStation pattern) land on the OpenGL backend before Metal exists?
The slice is graphics-API-agnostic and gives an immediate measurable
win on PGR2/Rainbow tail-jitter. Landing it on GL first validates the
algorithm before the Metal renderer adds it as a coupled dependency.

**Decision**: yes. Schedule before M10. Landing on GL first follows
project rule #1 (data-driven). Strategy.md Phase 2.5 already documents
this slice; this plan moves it from "deferred" to "before M10".

### Q5 — IOSurface interop fallback for GL HUD?

**Question**: even if M1 moves the main window to Metal when Metal is
selected, does it make sense to keep GL HUD compositing via IOSurface
as a fallback for compatibility?

**Decision**: no. Single window architecture per renderer choice.
Restart required to switch. Avoids an entire compatibility code path.

### Q6 — When to lift macOS deployment target?

**Question**: macOS 13 (Metal 3 + MetalFX) vs macOS 14
(`CAMetalDisplayLink`)? macOS 12 still in widespread Apple Silicon
use?

**Decision**: macOS 13 is the Metal-renderer minimum (codepath
mandatory). macOS 14 unlocks `CAMetalDisplayLink` (optional —
`presentDrawable:atTime:` is the macOS 13 fallback). macOS 12 keeps
the OpenGL fallback. Document in M14.

**Status update (2026-05-02, post-M0)**: `build.sh:212` already pins
the arm64 build to `-target arm64-apple-macos14.0` /
`-mmacosx-version-min=14.0`. This is the macOS 14 line, which is the
preferred Metal-renderer minimum — it is *above* the macOS 13
fallback floor, so `CAMetalDisplayLink` is available to slice M10
without any deployment-target lift work. **Q6 is therefore
effectively answered for the Apple Silicon build path: the
deployment target is already at the macOS 14 sweet spot.** No M14
work is required to lift it. M14 still owns the consolidating
decision-log entry and the user-facing documentation of the floor.

---

## 8. References

### Companion documents in this directory

- `metal-api-reference.md` — Apple Metal API surface.
- `emulator-metal-survey.md` — file-level findings from peer emulators.
- `macos-input-research.md` — input track (independent slice ordering).
- `strategy.md` Phase 4 (4a–4i) — original sub-deliverables; this plan
  sequences them.
- `research.md` "Apple Silicon Emulator Survey (2026-05-01)" —
  high-level findings.
- `decision-log.md` "2026-05-02: Pivot native Metal to the primary
  renderer path" — binding decision.

### External references

- DuckStation `metal_device.mm`:
  https://github.com/stenzek/duckstation/blob/master/src/util/metal_device.mm
- Dolphin Metal backend PR #10754:
  https://github.com/dolphin-emu/dolphin/pull/10754
- PCSX2 framebuffer-fetch combiner emulation PR #5630:
  https://github.com/PCSX2/pcsx2/pull/5630
- PCSX2 "Sync to host refresh rate" PR #5488:
  https://github.com/PCSX2/pcsx2/pull/5488
- xemu issue #1466 (MTLCompilerService crash):
  https://github.com/xemu-project/xemu/issues/1466
- xemu issue #2506 (macOS regression PR #2240):
  https://github.com/xemu-project/xemu/issues/2506
- Apple WWDC23-10125 "Bring your game to Mac, Part 3":
  https://developer.apple.com/videos/play/wwdc2023/10125/
- Apple "Tailor your apps for Apple GPUs and TBDR":
  https://developer.apple.com/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering
- Tellusim Metal MDI study: https://tellusim.com/metal-mdi/

### Internal source paths

- `hw/xbox/nv2a/pgraph/pgraph.h:108-136` — `PGRAPHRenderer` interface.
- `hw/xbox/nv2a/pgraph/pgraph.c:332` — `pgraph_renderer_register`.
- `hw/xbox/nv2a/pgraph/mtl/` — empty target directory.
- `hw/xbox/nv2a/pgraph/vk/` — structural template.
- `hw/xbox/nv2a/pgraph/glsl/` — shader generators, reused.
- `ui/xemu.c:74` — `vblank_interval_ns = 16,666,666`.
- `ui/xemu.c:804-878` — `gl_render_frame`; the path Metal replaces.
- `ui/xemu.c:1034` — `SDL_WINDOW_OPENGL` to be replaced with
  `SDL_WINDOW_METAL` on Metal path.
- `subprojects/imgui/backends/imgui_impl_metal.{h,mm}` — HUD port
  unblocker.
- `config_spec.yml:227-230` — renderer enum schema.
- `subprojects/genconfig/gen_config.py` — config generator.
- `meson.build:1855-1869` — OpenGL gating.
- `meson.build:2355-2364` — Vulkan gating (Linux/Windows only).
