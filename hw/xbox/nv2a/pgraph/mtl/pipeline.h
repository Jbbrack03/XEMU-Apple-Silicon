/*
 * NV2A PGRAPH Metal renderer — pipeline cache (slice M3).
 *
 * For M3 only ONE pipeline exists: a hand-coded MSL passthrough that
 * takes float4 position + float4 color (NV2A vertex attributes 0 and 3,
 * matching the GL renderer's binding numbers) and writes the
 * interpolated vertex color to a single color attachment. No texturing,
 * no register combiners, no depth/stencil state, no blend.
 *
 * The pipeline cache key for M3 is the (color_pixel_format,
 * depth_pixel_format) tuple. Each unique format combination produces a
 * separate pipeline; in practice the Xbox runs everything through
 * BGRA8Unorm + Depth32Float_Stencil8 on Apple Silicon so only one
 * entry is hit at runtime.
 *
 * M5 will replace this with the full PipelineKey + LRU. M3's
 * representation is intentionally minimal — a tiny array scan with
 * lazy MSL compile.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_PIPELINE_H
#define HW_XBOX_NV2A_PGRAPH_MTL_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pgraph_mtl_pipeline_init(void);
void pgraph_mtl_pipeline_finalize(void);

/*
 * Look up or lazily build the M3 passthrough pipeline. Returns an
 * id<MTLRenderPipelineState> as void*, or NULL on compile failure.
 *
 * color_pixel_format / depth_pixel_format are MTLPixelFormat values
 * cast to uint32_t. depth_pixel_format may be 0 to indicate no depth
 * attachment.
 *
 * sample_count: MSAA sample count. 1 means non-MSAA. M11 caches per
 * (color_fmt, depth_fmt, sample_count, variant) so the same shader +
 * formats can serve both non-MSAA and MSAA-enabled draws when MSAA
 * is reconfigured (although in practice MSAA is session-fixed).
 *
 * Counter: every call to this function that produces a new pipeline
 * is logged via pgraph_mtl_pipeline_compile_count().
 */
void *pgraph_mtl_pipeline_get_passthrough(uint32_t color_pixel_format,
                                          uint32_t depth_pixel_format,
                                          uint32_t sample_count);

/*
 * M4 native-depth variant.
 *
 * Look up or lazily build a passthrough pipeline whose fragment shader
 * additionally writes a derived depth value via [[depth(any)]],
 * reproducing the GL fragment-shader path's
 * `gl_FragCoord.z`-based zvalue derivation under the assumption
 * `XEMU_NATIVE_TRI_DEPTH`/`XEMU_NATIVE_QUAD` semantics
 * (linear-Z mode; z-perspective is M5 territory once the full PSH
 * comes online and `clipRange` / `depthFactor` / `depthOffset`
 * uniforms are routed in).
 *
 * For M4 the pipeline is keyed on (color_pixel_format,
 * depth_pixel_format) just like the passthrough variant; the cached
 * entry is independent of the passthrough entry (different fragment
 * function).
 */
void *pgraph_mtl_pipeline_get_native_depth(uint32_t color_pixel_format,
                                           uint32_t depth_pixel_format,
                                           uint32_t sample_count);

uint64_t pgraph_mtl_pipeline_compile_count(void);

/*
 * Slice M5 — synchronous MSL → MTLLibrary compile via [device
 * newLibraryWithSource:options:error:]. Returns 1 on success, 0 on
 * failure; on failure writes a malloc'd UTF-8 error string to
 * *out_error if non-NULL (caller frees with free()).
 *
 * This is a thin wrapper used by the shader-validation harness AND
 * by the upcoming PipelineKey-driven draw cache (mtl/shaders.mm).
 * The library object is intentionally not returned by this entry
 * point — the validation harness only needs to confirm acceptance,
 * not retain the library. The pipeline cache's library plumbing
 * lives in shaders.mm.
 */
int pgraph_mtl_pipeline_validate_msl(const char *msl_source,
                                     char **out_error);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_PIPELINE_H */
