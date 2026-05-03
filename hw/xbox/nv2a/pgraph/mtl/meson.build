if metal.found()

# Apple Silicon performance fork — Metal renderer (slice M2 +).
#
# renderer.c: NV2A-side renderer ops; the only file in this subdir that
#   includes hw/xbox/nv2a/nv2a_int.h. Compiled with the per-target
#   c_args propagated by `specific_ss` (-DCOMPILING_PER_TARGET, etc.).
#
# heap.mm, surface.mm: ObjC++ files that touch the Metal API and own
#   the MTLHeap / MTLTexture / MTLCommandQueue lifecycles. They do NOT
#   include nv2a_int.h — the per-target c_args do not propagate to
#   .mm / objcpp_COMPILER builds. They communicate with renderer.c
#   through opaque void*/uint32 handles in heap.h / surface.h. Same
#   boundary as ui/xemu-metal.mm and hw/display/apple-gfx.m.
#
# -fobjc-arc is set as a project-wide objcpp arg in meson.build for
# darwin+arm64; both .mm files use ARC idioms.

# M5 (slice 2026-05-02): GLSL → SPIR-V → MSL translator + shader
# validation harness. The translator (glsl.c) only depends on glslang
# + spirv-cross headers, no nv2a_int.h. The harness (shader_validation.c)
# depends on the per-target glsl/ headers (vsh.h / psh.h / geom.h) which
# pull in pgraph_regs / pgraph.h, so it MUST be in specific_ss. That
# also means the harness uses the per-target build's spirv-cross /
# glslang dependency wiring.
specific_ss.add([metal, libglslang, spirv_cross, files(
	'renderer.c',
	'index_gen.c',
	'disk_cache.c',
	'glsl.c',
	'shader_validation.c',
	'shadergen.c',
	'state.c',
	'format.c',
	'texture_pg.c',
	'uniform.c',
	'buffer.mm',
	'draw.mm',
	'heap.mm',
	'pipeline.mm',
	'shaders.mm',
	'surface.mm',
	'texture.mm',
	'uniform.mm',
	)])

endif
