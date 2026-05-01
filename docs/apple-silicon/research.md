# Research Notes

Last updated: 2026-05-01

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

## Current Conclusions

1. The final performance path should be Metal-first or Vulkan-over-Metal with a
   serious fallback plan.
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
   now the quick regression gate for that slice. Defaulting still needs broader
   retail coverage, and the next geometry-shader removal session should measure
   or create coverage for line primitives, quad/quad-strip expansion, polygon
   fill, or nonfill triangle modes before changing them.
