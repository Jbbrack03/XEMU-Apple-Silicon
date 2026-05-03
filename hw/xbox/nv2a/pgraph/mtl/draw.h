/*
 * NV2A PGRAPH Metal renderer — draw machinery (slice M3).
 *
 * For M3 the draw module accepts pre-decoded vertex data (position +
 * color) from renderer.c. renderer.c is the only file that includes
 * nv2a_int.h; it pulls the inline_buffer / inline_value arrays out of
 * PGRAPHState and hands plain float arrays to draw.mm. This keeps
 * NV2A's per-target preprocessor flags from leaking into the .mm
 * compile.
 *
 * M3 only handles the inline_buffer path (NV097 immediate-mode vertex
 * submission). draw_arrays / inline_elements / inline_array land in
 * M4. Vertex format generalization (anything other than float4 / 4
 * components) lands in M5.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_DRAW_H
#define HW_XBOX_NV2A_PGRAPH_MTL_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pgraph_mtl_draw_init(void);
void pgraph_mtl_draw_finalize(void);

/*
 * Pipeline variant selection. Mirrors mtl/pipeline.mm's variant enum
 * but exposed here so renderer.c (no Metal headers) can pick which
 * fragment shader to run without including Metal/MTLPixelFormat
 * machinery.
 *
 *   PASSTHROUGH  : M3 default — no depth write.
 *   NATIVE_DEPTH : M4 — fragment shader derives depth analogously to
 *                  GL's XEMU_NATIVE_TRI_DEPTH / XEMU_NATIVE_QUAD path.
 *                  Used for triangle-family fill draws and quads/
 *                  quad-strips that pass the GL native-path eligibility
 *                  rules. The actual eligibility check is done in
 *                  renderer.c via the existing pgraph_glsl_native_*
 *                  helpers.
 */
enum {
    MTL_DRAW_VARIANT_PASSTHROUGH = 0,
    MTL_DRAW_VARIANT_NATIVE_DEPTH = 1,
};

/*
 * Encode a passthrough draw with the supplied per-vertex data.
 *
 * positions      — pointer to vertex_count × 4 floats (xyzw per vertex)
 * colors         — pointer to vertex_count × 4 floats (rgba per vertex)
 * vertex_count   — number of vertices
 * mtl_primitive  — MTLPrimitiveType cast to uint32_t (4 = Triangle,
 *                  3 = TriangleStrip, 1 = Line, 0 = Point, etc.).
 *                  See draw.mm primitive_mode_from_nv097().
 * variant        — MTL_DRAW_VARIANT_*. Selects which fragment shader
 *                  to run.
 * viewport_w/h   — render-target dimensions for the viewport.
 * surface_color  — opaque MTLTexture* (color RT). May be NULL only if
 *                  surface_depth is non-NULL — Metal needs at least
 *                  one attachment.
 * surface_depth  — opaque MTLTexture* (depth RT) or NULL.
 * depth_fmt      — MTLPixelFormat of depth (0 if no depth).
 *
 * Increments METAL_DRAW_COUNT on success.
 */
void pgraph_mtl_draw_passthrough(const float *positions,
                                 const float *colors,
                                 unsigned int vertex_count,
                                 uint32_t mtl_primitive,
                                 uint32_t variant,
                                 unsigned int viewport_w,
                                 unsigned int viewport_h,
                                 void *surface_color,
                                 void *surface_depth,
                                 uint32_t color_fmt,
                                 uint32_t depth_fmt);

/*
 * Encode an indexed draw. Same vertex layout as
 * `pgraph_mtl_draw_passthrough` but the GPU consumes
 * `index_count` uint32 indices from `indices` (host pointer; copied
 * into a staging slot internally).
 *
 * Used by M4 for primitive types Metal does not expose natively
 * (triangle fan, quad list, quad strip, line loop, polygon). The
 * caller is responsible for choosing the right `mtl_primitive` for the
 * expanded index list — typically 3 (Triangle) for the geometry
 * expansions and 1 (Line) for line_loop.
 */
void pgraph_mtl_draw_indexed(const float *positions,
                             const float *colors,
                             unsigned int vertex_count,
                             const uint32_t *indices,
                             unsigned int index_count,
                             uint32_t mtl_primitive,
                             uint32_t variant,
                             unsigned int viewport_w,
                             unsigned int viewport_h,
                             void *surface_color,
                             void *surface_depth,
                             uint32_t color_fmt,
                             uint32_t depth_fmt);

/*
 * M7.1: encode a draw through a translated MSL pipeline.
 *
 * The caller has built a `PgraphMtlPipelineKey`, looked up the
 * resulting MTLRenderPipelineState via
 * `pgraph_mtl_shaders_get_pipeline`, and staged uniform / texture
 * bindings via the uniform.h / texture.h APIs.
 *
 *   pipeline_state    : id<MTLRenderPipelineState> from the cache.
 *   vsh_ubo / vsh_ubo_offset / psh_ubo / psh_ubo_offset : id<MTLBuffer>
 *                                                        + byte offset
 *                                                        for the
 *                                                        translated
 *                                                        VSH/PSH UBOs.
 *   stage_textures[i] / stage_samplers[i] : per-fragment-stage texture
 *       and sampler bindings (NV2A_MAX_TEXTURES = 4 stages). Either
 *       may be NULL — the encoder will skip unbound slots.
 *
 * Vertex source: position + color, same shape as the M3/M4 path. M7.1
 * keeps the inline_buffer-driven Float4 vertex layout; the
 * BUFFER_VERTEX_RAM port lands later when format-resolving lands.
 */
void pgraph_mtl_draw_translated(void *pipeline_state,
                                const float *positions,
                                const float *colors,
                                unsigned int vertex_count,
                                const uint32_t *indices,
                                unsigned int index_count,
                                uint32_t mtl_primitive,
                                unsigned int viewport_w,
                                unsigned int viewport_h,
                                void *surface_color,
                                void *surface_depth,
                                uint32_t depth_fmt,
                                void *vsh_ubo,
                                size_t vsh_ubo_offset,
                                size_t vsh_ubo_size,
                                void *psh_ubo,
                                size_t psh_ubo_offset,
                                size_t psh_ubo_size,
                                void *const stage_textures[4],
                                void *const stage_samplers[4]);

uint64_t pgraph_mtl_draw_count(void);
uint64_t pgraph_mtl_draw_indexed_count(void);
uint64_t pgraph_mtl_draw_native_tri_depth_count(void);
uint64_t pgraph_mtl_draw_native_quad_count(void);
uint64_t pgraph_mtl_draw_translated_count(void);
uint64_t pgraph_mtl_draw_pipeline_fallback_count(void);
void     pgraph_mtl_draw_inc_pipeline_fallback_count(void);

/*
 * Renderer.c calls these after a successful native-depth indexed draw
 * — it has the NV2A primitive_mode necessary to distinguish triangle-
 * family (native_tri_depth) from quad-family (native_quad).
 * Decoupling the increment from draw.mm keeps NV2A primitive enums
 * out of the Metal-API-bound .mm file. The counters parallel the GL
 * profile counters NATIVE_TRI_DEPTH_DRAW and NATIVE_QUAD_DRAW (gl/
 * draw.c::pgraph_gl_profile_native_tri_depth_draw /
 * pgraph_gl_profile_native_quad_draw); they are mutually exclusive.
 */
void pgraph_mtl_draw_inc_native_tri_depth_count(void);
void pgraph_mtl_draw_inc_native_quad_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_DRAW_H */
