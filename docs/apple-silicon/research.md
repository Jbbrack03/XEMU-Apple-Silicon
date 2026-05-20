# Research Notes

Last updated: 2026-05-19 night

## Local Source Findings

### Baseline arm64 build now succeeds

`./build.sh -a arm64` succeeds on the local Apple Silicon machine and produces:

- `build/qemu-system-i386`
- `dist/xemu.app`
- `dist/xemu.app/Contents/MacOS/xemu`

Both `build/qemu-system-i386` and the bundled `xemu` executable are Mach-O
64-bit arm64 binaries.

Launch smoke test:

- `dist/xemu.app/Contents/MacOS/xemu --version`

Observed renderer details:

- GL vendor: `Apple`
- GL renderer: `Apple M3 Ultra`
- GL version: `4.1 Metal - 90.5`
- GLSL version: `4.10`

Implication: Phase 0 has a runnable native arm64 OpenGL baseline. The remaining
Phase 0 work is gameplay measurement, not build enablement.

### macOS build/package fixes are required for a runnable baseline

Two local build/package issues were found and fixed in `build.sh`:

- Meson treats the Darwin arm64 build as a cross build, so default CMake
  discovery was rejected for the `nv2a_vsh_cpu` CMake subproject. `build.sh`
  now exports `CMAKE` when `cmake` is available on `PATH`.
  - `build.sh:241-244`
- `dylibbundler` can leave duplicate `LC_RPATH` entries for
  `@executable_path/../Libraries/arm64/`. macOS 26.4.1 `dyld` rejects the
  duplicate at launch. `build.sh` now removes duplicate app rpaths before the
  final code-sign step.
  - `build.sh:59-64`

Implication: the baseline build can now be reproduced without manual shell
workarounds.

### macOS packages `qemu-system-i386`

`build.sh` copies `build/qemu-system-i386` into the macOS app bundle:

- `build.sh:25-33`

The build target is explicitly `i386-softmmu`:

- `build.sh:275-280`

Implication: xemu's Xbox CPU path is QEMU i386 system emulation. On Apple
Silicon, this cannot use x86 host virtualization because the host architecture
is arm64.

### Apple Silicon HVF is not an i386 accelerator here

Meson only maps HVF to `aarch64-softmmu` on aarch64 hosts:

- `meson.build:338-341`

Implication: Apple Silicon can use HVF for arm64 guests, not for xemu's i386
Xbox guest. CPU emulation is QEMU TCG.

### macOS OpenGL is the only enabled native renderer path

Current Meson logic links OpenGL on Darwin:

- `meson.build:1855-1869`

Current Meson logic only enables Vulkan for Windows and Linux:

- `meson.build:2355-2364`

The renderer selection prefers OpenGL before Vulkan:

- `hw/xbox/nv2a/pgraph/pgraph.c:254-264`

Implication: a stock macOS build will use OpenGL unless we change the platform
backend.

### Geometry shader use is broad

`pgraph_glsl_need_geom()` returns true for nearly every non-point primitive:

- lines, line loops, line strips
- triangles, triangle strips, triangle fans
- quads, quad strips
- polygons

Reference:

- `hw/xbox/nv2a/pgraph/glsl/geom.c:83-110`

The generated geometry shader is explicitly used for deprecated primitive
support and vertex ordering:

- `hw/xbox/nv2a/pgraph/glsl/geom.c:296-323`

The Vulkan renderer currently requires `geometryShader`:

- `hw/xbox/nv2a/pgraph/vk/instance.c:472-498`

Implication: simply enabling Vulkan on macOS is not sufficient if the selected
Vulkan-on-Metal implementation does not support this feature well.

### xemu launch path forces the custom display backend

xemu constructs a QEMU launch with:

- `-machine xbox,...,kernel-irqchip=off`
- `-display xemu`

References:

- `system/vl.c:3019-3023`
- `system/vl.c:3097-3098`

Implication: our display/backend changes should start in the xemu display and
NV2A PGRAPH renderer path, not generic QEMU display code first.

## Public xemu Issue Evidence

### macOS regression after PR #2240

Issue: https://github.com/xemu-project/xemu/issues/2506

Reported behavior:

- Menus and videos can remain fine.
- 3D content such as garages and gameplay runs at reduced speed or poor pacing.
- Reproduced on M2 Max and M1 Pro.
- Regression starts after `0.8.108-2-g54e4580efc`; later versions exhibit the
  issue.

Important comments:

- A collaborator identified PR #2240 as the source of the regression.
- A later comment names heavier geometry-shader use as the likely suspect.
- The same comment says the real fix is to rework xemu to avoid geometry
  shaders, which would also make MoltenVK possible.

### PR #2240: depth precision and polygon offset slope factor

PR: https://github.com/xemu-project/xemu/pull/2240

Merged: 2025-10-24

Important facts:

- It improves depth precision and polygon offset behavior.
- It adds/reworks geometry shader paths for depth interpolation, polygon line
  mode, flat shading, and vertex ordering.
- The author noted geometry shaders add stop-the-world pauses when shaders
  compile.
- Later in the thread, a user reported this PR introduced the severe macOS
  regression tracked in #2506.

Implication: PR #2240 fixed real correctness issues, so reverting it wholesale
is not a principled final solution. We need to preserve the correctness intent
while moving work out of geometry shaders or away from Apple's OpenGL path.

Local diagnostic update, 2026-04-30:

- B2/B3 snapshot runs added geometry-shader attribution counters.
- Crimson Skies B2 issued 25,202 geometry-backed draws in 30s.
- Rainbow Six 3 B3 issued 149,961 geometry-backed draws in 30s.
- D1 enabled `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1`, which keeps
  triangle-family geometry shaders active but bypasses their depth-plane/slope
  math.
- D1 did not improve Rainbow Six 3 over B3.

Retrospective implication: the productive diagnostics are those that remove or
bypass geometry-shader dispatch without losing the geometry shader's
correctness behavior, rather than those that only simplify arithmetic inside
triangle geometry shaders. D2/D3 later confirmed this direction.

### macOS OpenGL/Metal compiler failure report

Issue: https://github.com/xemu-project/xemu/issues/1466

Reported behavior:

- `MTLCompilerService` crash on macOS with Halo 2.
- Poor performance and missing effects.
- Tested on Apple M2.

Maintainer comment:

- Visual issues were already known elsewhere.
- Performance was expected at that time.

Implication: macOS graphics behavior is a known weak point, not an isolated
game issue.

## Platform Research

### Apple: Metal supersedes OpenGL/OpenGL ES/OpenCL

Source: https://developer.apple.com/la/videos/play/wwdc2019/611/

Apple describes Metal as the modern GPU foundation on Apple platforms,
superseding OpenGL, OpenGL ES, and OpenCL.

Implication: excellent Apple Silicon performance should not depend on Apple's
OpenGL implementation.

### Apple Silicon Metal capabilities

Source: https://developer.apple.com/metal/capabilities/

The Metal Feature Set Tables list Apple Silicon GPU families and Metal 3/4
support. M1-series maps to Apple family 7, M2-series to Apple family 8, M3/M4
to Apple family 9, M5 to Apple family 10.

Implication: a Metal backend can target a modern, bounded family of Apple GPUs
instead of a broad legacy OpenGL matrix.

### MoltenVK

Source: https://github.com/KhronosGroup/MoltenVK

MoltenVK is a Vulkan 1.4 implementation layered over Metal and supports Apple
Silicon. It is a portability implementation, not identical to native Vulkan.

Implication: MoltenVK may be useful, but only if xemu's Vulkan feature demands
fit MoltenVK's supported feature set and performance characteristics.

### KosmicKrisp

Sources:

- https://www.lunarg.com/lunarg-achieves-vulkan-1-3-conformance-with-kosmickrisp-on-apple-silicon/
- https://www.lunarg.com/lunarg-releases-vulkan-sdk-1-4-335-0/

LunarG says KosmicKrisp is a Vulkan-to-Metal driver for Apple Silicon. It
passed Vulkan 1.3 conformance in October 2025, and an alpha was integrated into
the macOS Vulkan SDK in December 2025. LunarG also cautions that conformance is
the baseline and real workload testing remains necessary.

Implication: KosmicKrisp is worth testing as a data point, but not assumed as
the final answer without profiling and compatibility evidence.

### QEMU TCG

Source: https://www.qemu.org/docs/master/devel/tcg.html

QEMU describes TCG as its dynamic translation backend, translating guest code
to the host instruction set.

Source: https://www.qemu.org/docs/master/system/introduction.html

QEMU documents accelerators and notes that default TCG is purely emulated; a
hardware accelerator must be selected to use hardware virtualization.

Implication: CPU performance is bounded by translated x86 execution on arm64,
but the public macOS regression data points first at GPU/renderer work.

### QEMU Apple Silicon x86 acceleration request

Source: https://gitlab.com/qemu-project/qemu/-/issues/2295

QEMU has an upstream request to improve x86/x86_64 emulation on Apple Silicon,
including exploiting Apple Silicon x86 memory model features and possibly
Rosetta-related paths.

Implication: there may be future CPU-side opportunities, but they are outside
the immediate renderer-focused fork work.

## Apple Silicon Emulator Survey (2026-05-01)

This section catalogues techniques used by other emulators that achieve
excellent performance on Apple Silicon, with specific source references
and named code paths. Captured 2026-05-01 as the evidence base for the
research-informed roadmap in `strategy.md` Phase 2.5 and Phase 4.

> **See also (2026-05-02):** `emulator-metal-survey.md` is the
> source-level companion to this section. Where this section gives
> high-level summaries, the survey doc names specific files, line
> numbers, struct layouts, and hash-key shapes for Dolphin / PCSX2 /
> DuckStation / MoltenVK / xemu's own Vulkan renderer. Read the
> survey before starting Metal implementation slices that touch
> pipeline cache (M5, M9), buffer management (M3, M6), or presentation
> (M10).

### Dolphin (GameCube/Wii) — native Metal backend

Native Metal backend (TellowKrinkle, PR #10754, 2022); MoltenVK kept as a
separate Vulkan path. Reported 23–30 % gains on specific titles vs
MoltenVK, ~14 W → ~2 W on a 4× IR Skyward Sword scene, and
previously-unplayable titles (Metroid Prime 3) becoming playable.

Sources:

- https://dolphin-emu.org/blog/2022/09/13/dolphin-progress-report-july-and-august-2022/
- https://github.com/dolphin-emu/dolphin/pull/10754

CPU-side primitive expansion in
`Source/Core/VideoCommon/IndexGenerator.cpp` (`AddFan`, `AddQuads`,
`AddLineList`, `AddLineStrip`). Two compile-time variants (`pr` =
primitive-restart enabled). Triangle fans and quads are emitted as
triangle strips (with primitive restart) or triangle lists. The Metal
backend declares `bSupportsGeometryShaders = false` and only ever sees
`MTLPrimitiveTypeTriangle` / `TriangleStrip`
(`Source/Core/VideoBackends/Metal/MTLUtil.mm`).

VS-Expand pattern handles point sprites / wide lines without a geometry
shader: index encodes (logical_vertex_id << 2 | corner_id), the vertex
shader expands one logical vertex into four corners.
`bSupportsVSLinePointExpand = true` in the Metal backend.

Hybrid ubershader (PR #5702): a single megashader interprets TEV state at
runtime as a fallback while the per-pipeline specialized variant compiles
in the background. The ubershader runs on a new scene (no stutter); the
specialized variant swaps in when ready. The Metal backend uses
`bSupportsBackgroundCompiling = true`. Framebuffer fetch is used for
almost all blend unit configurations in the ubershaders on Apple GPUs.

`bUseUnifiedMemory` toggle was removed during PR #10754 ("not worth the
extra code"). The backend uses `MTLResourceStorageModeShared` for
streaming and `MTLStorageModePrivate` for GPU-only resources, treating
Apple Silicon and discrete GPUs uniformly. Implication: unified-memory
wins are mostly automatic if the renderer doesn't unconditionally stage
through a private-only path; no separate Apple Silicon code path is
needed.

PowerPC-to-host JIT is hand-written ARM64 (`JitArm64`); the 2022 progress
report attributed measurable Metal-throughput gains to an unrelated
AArch64 JIT optimization, indicating the host CPU JIT is independently
dominant in this workload.

Implication for xemu: `IndexGenerator.cpp` is the closest direct template
for the Metal-side equivalent of the existing `XEMU_NATIVE_QUAD` /
`XEMU_NATIVE_TRI_DEPTH` slices. Hybrid ubershader is the model for the
async shader compile path that addresses Crimson's documented 1310 ms
worst-frame from synchronous Apple GL-on-Metal compile.

### PCSX2 (PS2) — native Metal backend

Native Metal (TellowKrinkle), `pcsx2/GS/Renderers/Metal/`. Built
specifically to exploit Apple GPU features.

VS Expand (`GSDeviceMTL.mm`):

- `m_features.vs_expand = true`, `point_expand = true`,
  `line_expand = false` (lines via VS).
- A precomputed expand index buffer is built once at init in private GPU
  memory (`m_expand_index_buffer`, label "Point/Sprite Expand Indices")
  and bound to the VS instead of the input assembler.
- Two vertex shader variants (`vs_main` vs `vs_main_expand`) selected by
  Metal *function constants* at pipeline build time, so the compiler
  specializes.

Framebuffer fetch via `[device supportsFamily:MTLGPUFamilyApple1]`. Maps
PS2 "ultra blending" (blend modes that don't fit Metal fixed-function
blending) into a single shader pass with MSL `[[color(0)]]` fragment
input. PR #5630 reports 2.5×–9.75× wins on Intel iGPUs; the same shape
is used by DuckStation.

Memory: `MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined`
for upload buffers, `MTLStorageModePrivate` for GPU-only,
`MTLResourceHazardTrackingModeUntracked` for explicitly fenced buffers
(`m_draw_sync_fence`, `m_spin_fence`).

"Sync to host refresh rate" (PR #5488) — same emulation-rate slewing
pattern as DuckStation.

Sources:

- https://github.com/PCSX2/pcsx2/pull/5630
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm
- https://github.com/PCSX2/pcsx2/pull/5488

Implication for xemu: the static expand-index buffer + function-constant
specialisation pattern is directly applicable to NV2A point sprites and
any wide-line emulation. Framebuffer fetch is high-leverage for NV2A
register-combiner fragment work that does not map cleanly to Metal
fixed-function blending.

### DuckStation (PS1) — native Metal backend

Native Metal (`src/util/metal_device.mm`), plus D3D11/12, OpenGL, Vulkan.
ARM64 macOS universal binary.

Frame pacing: `m_features.timed_present = true`. `MetalDevice::EndPresent`
(`src/util/metal_device.mm:2536-2620`) converts a presentation deadline
to mach absolute time and calls `[m_render_cmdbuf presentDrawable:atTime:]`.
Combined with their "Sync to Host Refresh Rate" option (which nudges
emulation rate by ≤ 1 % to align with monitor refresh), this produces
jitter-free pacing without VRR. The combination — emulation-rate slewing
+ `presentDrawable:atTime:` — is the proven Apple Silicon recipe for
stable pacing.

Framebuffer fetch on `[device supportsFamily:MTLGPUFamilyApple1]`
(`src/util/metal_device.mm:387-410`); `feedback_loops` flag is
`framebuffer_fetch || supports_barriers` — Apple Silicon always picks
FBFetch.

Custom ARM64 dynarec for MIPS R3000A. Software rasterizer is
multi-threaded and vectorized; hardware backend uses a single render
thread (Metal command-buffer recording is already cheap).

Source: https://github.com/stenzek/duckstation/blob/master/src/util/metal_device.mm

Implication for xemu: this is the textbook fix for the "30 FPS with
jitter" symptom. The `presentDrawable:atTime:` piece is Metal-specific;
the emulation-rate slewing piece is graphics-API-agnostic and can land
on the current OpenGL path before the Metal renderer exists.

### PPSSPP (PSP)

Vulkan via MoltenVK on macOS/iOS; no native Metal backend yet (hrydgard
has stated one *may* be written — unverified). PSP GPU is simple enough
that MoltenVK overhead is tolerable.

Custom MIPS-to-ARM64 JIT (`JitArm64`) plus an IR-based JIT: PSP MIPS →
PPSSPP IR → ARM64. The IR layer survives platforms where W^X JIT is
restricted (iOS without entitlement) by running as an interpreter of the
IR. On Apple Silicon macOS where JIT is permitted, the IR backend is at
parity or slightly faster than the direct ARM64 dynarec because the IR
allows additional optimization passes.

Per-game shader cache files in user dir.

Source: https://www.ppsspp.org/docs/development/developer-tools/

Implication for xemu: the per-game shader/pipeline cache pattern is a
low-risk addition independent of API choice; useful as a Phase 4 / Phase
5 deliverable even before async compile lands.

### RPCS3 (PS3)

Vulkan via MoltenVK on macOS. No native Metal.

ARM64 port reuses the existing x86-64 IR and runs an IR-transform pass
that rewrites x86-isms (e.g. `pshufb`, x86-style atomics) into
ARM64-friendly LLVM IR before LLVM's backend. Most optimization work
targeting x86 also benefits ARM64. PRs #12115, #12338.

macOS JIT pain catalog from PR #12115 (directly applicable if xemu ever
extends TCG protections):

- Apple bans `MAP_FIXED | MAP_JIT` overwriting an existing mmap;
  workaround is `munmap` + `mmap` at same address.
- W^X enforcement requires `pthread_jit_write_protect_np()` toggling
  around code emission.
- 16 KB page size on Apple Silicon breaks fine-grained memory unmapping.
- Default to RW permissions on Apple+ARM64 mmap, then transition to RX
  before execution.

Recent SPU work added AArch64 SDOT/UDOT NEON intrinsics for SPU SIMD
workloads. Reported PR #12338 numbers on M1 Pro vs real PS3: SPU integer
~3× faster; SPU spinlock ~35× *slower* (atomic-heavy workload on emulated
cache-line semantics is the main remaining hot spot — contention, not raw
arithmetic).

Async shader compile (2018) decouples decompile/recompile/link from the
render thread; missing shaders cause graphics pop-in until compiled (no
ubershader fallback like Dolphin). Multi-threaded compilation across CPU
cores.

Sources:

- https://blog.rpcs3.net/2024/12/09/introducing-rpcs3-for-arm64/
- https://rpcs3.net/blog/2018/08/08/eliminating-stutter-with-asynchronous-shader-implementation/
- https://github.com/RPCS3/rpcs3/pull/12115
- https://github.com/RPCS3/rpcs3/pull/12338

Implication for xemu: the macOS JIT pain catalog is a documented
anti-pattern list — read PR #12115 before any custom-JIT work. RPCS3's
IR-transform model is the cheaper template if a JIT change ever does
become necessary; see `strategy.md` "What we ruled out" for why a custom
x86→ARM64 JIT is not on the current roadmap.

**Direct relevance to xemu's headline judder (added 2026-05-01).** The
2026-05-01 sample profile during a known-bad Crimson interval
(`benchmarks/2026-05-01-renderer-vs-tcg-stutter-attribution.md`,
`benchmarks/2026-05-01-gl-vs-metal-decision.md`) attributed the
1.35-second worst-frame to xemu's existing TCG hitting exactly this
catalog: `tb_invalidate_phys_range_fast` →
`do_tb_phys_invalidate` → `tcg_flush_jmp_cache` plus
`pthread_jit_write_protect_np` (W^X toggle) and
`sys_icache_invalidate`. Each TB invalidation pays real syscall cost
on Apple Silicon. The fix lives on the TCG side — strategy.md Phase
5a is now the highest-priority active slice — and the natural
reference for what to investigate is Ryujinx's PPTC pattern below
plus the RPCS3 PR #12115 anti-pattern list above.

### Ryujinx (Switch)

Vulkan via MoltenVK on macOS. (Project DMCA'd by Nintendo October 2024;
before that, native Metal was discussed but never shipped.)

Switch ARMv8 → host ARMv8 path uses Apple's `Hypervisor.framework` to run
guest ARM64 directly on the host CPU at near-zero overhead, dropping
into emulation only for syscall HLE and MMIO. Not applicable to xemu
(Xbox is x86 32-bit, not ARM).

PPTC (Profiled Persistent Translation Cache): translation cache survives
across runs; stored as a flat file keyed by guest binary hash. Second
launch is faster, third+ launch is fastest because profile-guided
re-translation has now run.

Sources:

- https://blog.ryujinx.org/the-impossible-port-macos/
- https://blog.ryujinx.org/introducing-profiled-persistent-translation-cache/

Implication for xemu: the PPTC pattern is directly applicable —
persisting QEMU TCG translation blocks between runs would speed
first-load. Steady-state perf unchanged.

### Transversal techniques

Geometry-shader emulation on Metal — three implementation strategies:

1. CPU-side index expansion (Dolphin's `IndexGenerator.cpp`). Cheap,
   robust, no shader-side work. Cost is increased index-buffer
   bandwidth. Best for primitives the guest issues in modest counts
   (quads, fans). Already applied to xemu's OpenGL path via
   `XEMU_NATIVE_QUAD` / `XEMU_NATIVE_TRI_DEPTH`.
2. VS-Expand with bit-packed indices + precomputed static index buffer
   (Dolphin and PCSX2). Best for point sprites and wide-line emulation
   where a single guest vertex must become 4 corners. Avoids index-
   bandwidth cost of (1).
3. Compute pre-pass (MoltenVK approach for triangle fans). Most flexible
   but adds a barrier. MoltenVK uses this internally now that it
   supports `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN` (issues #229, #1419) —
   meaning if xemu stays on MoltenVK, triangle fans Just Work, with a
   hidden compute dispatch cost.

MoltenVK vs native Metal — measured gap: Dolphin 2022 progress report
+23 % Skyward Sword, +30 % Rogue Leader, ~14 W → ~2 W on a 4× IR scene.
Direction is consistent across reports; magnitude is platform/title-
specific and likely shifted as MoltenVK improved. Pattern: native Metal
saves 20–30 % CPU-bound and unblocks features (FBFetch directly
accessible, no MoltenVK-introduced stalls).

KosmicKrisp vs MoltenVK: KosmicKrisp reached Vulkan 1.3 conformance
October 2025 and MoltenVK feature parity February 2026. Apple-Silicon-
only, requires Metal 4. Early benchmarks "competitive". **No public
emulator-specific A/B benchmarks exist as of 2026-05-01.** Performance
claims are unverified per workspace rule #6.

Sources:

- https://www.lunarg.com/the-state-of-vulkan-on-apple-jan-2026/
- https://www.phoronix.com/news/KosmicKrisp-Parity

Frame pacing recipe:

- `[drawable presentAtTime:]` with a deadline computed from the host
  display's expected next vsync (DuckStation
  `metal_device.mm:2585-2589`).
- Pair with emulation-rate slewing (DuckStation/PCSX2 "Sync to Host
  Refresh Rate") to lock guest 60 Hz to host 60 Hz, eliminating the
  fractional drift that causes a hitch every few minutes.
- `CAMetalDisplayLink` (Metal 3+) is Apple's modern replacement for
  `CADisplayLink`; on a 120 Hz display, requesting
  `preferredFramesPerSecond = 60` produces stable pacing automatically
  (WWDC21 talk 10147,
  https://developer.apple.com/videos/play/wwdc2021/10147/).
- Triple buffering of `CAMetalLayer` drawables
  (`MTLLayer.maximumDrawableCount = 3`) is the default; leave alone
  unless latency-critical.

TCG vs custom JIT — measured ceilings: Linaro/academic measurement
(https://arxiv.org/html/2501.03427v1) shows direct binary translation
without an IR can be up to 35× faster than QEMU TCG on tight loops;
typical real-workload speedups for game emulation are 2–5×. Custom JITs
in Dolphin (`JitArm64`), DuckStation (MIPS→ARM64), and PPSSPP routinely
measure 2–4× over interpreters; TCG sits roughly halfway between an
interpreter and a custom JIT for x86 guests. The ceiling on TCG for a
733 MHz Pentium III guest is real but not dramatic — performance is left
on the table, but not 10×.

Indirect command buffers / argument buffers: Tellusim measurements
(https://tellusim.com/metal-mdi/) — on Apple Silicon, ICBs win for many
small draws (< 200 prims/draw) and lose by ~1.5× for larger draws.
Xbox-era games with thousands of small NV2A draw calls per frame are the
favourable case, but the win is meaningful only after per-draw shader
compile cost is absorbed and the bottleneck has moved into command-
encoding overhead — rarely the case at 30 FPS gameplay. Premature for
xemu's current bottleneck profile.

## Current Conclusions

1. **Updated 2026-05-01 (binding) and 2026-05-02 (broader-sweep
   confirmation): the active renderer path is Apple OpenGL.** The
   GL-vs-Metal decision diagnostic
   (`benchmarks/2026-05-01-gl-vs-metal-decision.md`) showed Apple
   GL-on-Metal has measured headroom for AA / 1080p on tracked
   titles, and the V4 broader sweep
   (`benchmarks/2026-05-02-broader-title-sweep.md`) confirmed no
   new title-specific Apple-GL pathologies surface across 5
   additional titles. Native Metal (strategy.md Phase 4) remains
   documented as a long-term ceiling-removing investment but is
   **not** the next priority. (Original 2026-04-29 framing —
   "Metal-first or Vulkan-over-Metal" — superseded by the 2026-05-01
   decision-log entry "Stay on OpenGL …".)
2. Geometry-shader dependency is the key blocker. It hurts macOS OpenGL, makes
   MoltenVK uncertain, and is directly implicated in the current macOS 3D
   regression.
3. We should not permanently revert PR #2240. It encodes real depth and
   primitive correctness work. We should relocate the work into a macOS-friendly
   pipeline.
4. Data collection comes before architecture lock-in: baseline OpenGL, Vulkan
   via MoltenVK if enabled, Vulkan via KosmicKrisp if available, and native
   Metal prototypes should be measured.
5. The local arm64 baseline, scripted-input harness, snapshot scene-entry
   benchmarks, geometry counters, native triangle-depth diagnostics, paired
   comparison harness, and generated `flat-tri-depth` XBE are now in place. The
   evidence now points at GL geometry-shader dispatch or Apple driver behavior
   as the local bottleneck. The dedicated flat XBE and paired Rainbow/Crimson
   runs validate `XEMU_NATIVE_TRI_DEPTH=1` for the current opt-in
   triangle-family fill category. `validate-native-tri-depth.sh --run 20` is
   now the quick regression gate for that slice. The 2026-05-01 PGR2, Rainbow
   Six 3, and Crimson Skies gameplay routes add the next retail coverage layer.
   `XEMU_NATIVE_QUAD=1` is the second completed bypass slice, removing the
   smooth-fill quad/quad-strip-family geometry-shader cost via CPU index
   expansion to triangles plus the same `gl_FragCoord`-derived depth path the
   triangle slice introduced. With both flags the snapshot scenes for PGR2,
   Rainbow Six 3, and Crimson Skies now have zero geometry-shader draws of
   any kind, but the captured PGR2 mid-route snapshot still only reaches
   16.56 FPS — the remaining gap to the 30 FPS gameplay floor is not
   geometry-shader work. The next session should profile that scene under
   Instruments and `XEMU_PERF_LOG=1` counters to find the new dominant cost
   before further renderer slices.
6. The 2026-05-01 emulator survey above adds named patterns and source
   references for the work ahead: Dolphin's `IndexGenerator.cpp` for
   Metal-side primitive expansion, PCSX2's `m_expand_index_buffer` for
   point/wide-line VS-Expand, DuckStation's `presentDrawable:atTime:` +
   emulation-rate slewing for jitter-free pacing, Dolphin's hybrid
   ubershader for async shader compile (the highest-leverage fix for
   Crimson's documented 1310 ms shader-compile stutter), Ryujinx's PPTC
   for persistent TCG translation cache, and RPCS3 PR #12115 as a
   documented anti-pattern list for any future custom-JIT work. The
   findings extend rather than replace the existing prioritized work —
   see `strategy.md` Phase 2.5 and Phase 4 for the implementation
   roadmap, and `decision-log.md` "2026-05-01: Adopt research-informed
   implementation roadmap" for the binding decision.

## 2026-05-19 research pass — Apple's documented Metal workflow

Apple's current guidance for Metal migration and optimization is more
structured than "port code, then tune by feel." The common shape across the
docs is:

1. **Port incrementally.** Keep the legacy renderer alive as a reference path
   while Metal comes up, and migrate in stages rather than replacing
   everything blindly.
2. **Validate correctness first.** Turn on API validation and shader
   validation before interpreting performance behavior.
3. **Capture the workload.** Use Xcode GPU Frame Capture / `.gputrace` to
   inspect the failing frame or short sequence directly.
4. **Classify the bottleneck.** Use Instruments Game Performance / Metal
   System Trace plus the Metal debugger to decide whether the problem is CPU,
   GPU, overlap/pacing, or pure correctness.
5. **Optimize with a measure → analyze → improve → re-measure loop.**
   Apple's game-performance docs present this as the primary optimization
   discipline, not as an optional best practice.
6. **Use runtime feature detection.** Query `supportsFamily` and device
   properties rather than keying behavior off GPU names or assumptions.
7. **Respect Apple GPU render-pass semantics.** Load/store actions, resource
   hazards, invariant positions, and synchronization rules are correctness
   issues on Apple GPUs, not optional polish items.

Implication for this fork: the project-specific GL/Metal/oracle tools are
still justified, but they should sit around Xcode and Instruments rather than
replacing them. Our paired diffs, per-draw RT dumps, temporal capture, and
oracle triptychs are strongest when they act as reproducer/oracle layers over
Apple's first-party validation/capture/profiling loop.

Primary Apple sources reviewed 2026-05-19:

- https://developer.apple.com/documentation/apple-silicon/porting-your-metal-code-to-apple-silicon
- https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-in-xcode
- https://developer.apple.com/documentation/xcode/analyzing-the-performance-of-your-metal-app/
- https://developer.apple.com/documentation/xcode/analyzing-apple-gpu-performance-using-counter-statistics
- https://developer.apple.com/documentation/xcode/analyzing-apple-gpu-performance-using-a-visual-timeline/
- https://developer.apple.com/documentation/xcode/optimizing-gpu-performance
- https://developer.apple.com/documentation/metal/improving-your-games-graphics-performance-and-settings
- https://developer.apple.com/documentation/metal/mixing-metal-and-opengl-rendering-in-a-view
- https://developer.apple.com/metal/capabilities/
