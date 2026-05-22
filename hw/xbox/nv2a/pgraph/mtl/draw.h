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

#include "vertex.h"  /* MtlAttributeStream */

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
 * M7.1 + M5.8: encode a draw through a translated MSL pipeline.
 *
 * The caller has built a `PgraphMtlPipelineKey`, looked up the
 * resulting MTLRenderPipelineState via
 * `pgraph_mtl_shaders_get_pipeline`, and staged uniform / texture
 * bindings via the uniform.h / texture.h APIs.
 *
 *   pipeline_state    : id<MTLRenderPipelineState> from the cache.
 *   attr_streams[i]   : per-NV2A-slot Float4 stream (M5.8). NULL data
 *                       == slot is uniform; the encoder leaves the
 *                       slot's bufferIndex unbound and the shader
 *                       reads via the VSH UBO's inlineValue[] block.
 *   n_attr_streams    : count of valid entries in attr_streams (typically
 *                       MTL_VERTEX_NUM_ATTRIBUTES = 16).
 *   vsh_ubo / vsh_ubo_offset / psh_ubo / psh_ubo_offset : id<MTLBuffer>
 *                                                        + byte offset
 *                                                        for the
 *                                                        translated
 *                                                        VSH/PSH UBOs.
 *   stage_textures[i] / stage_samplers[i] : per-fragment-stage texture
 *       and sampler bindings (NV2A_MAX_TEXTURES = 4 stages). Either
 *       may be NULL — the encoder will skip unbound slots.
 *
 * Buffer-index layout (vertex stage):
 *   [[buffer(0)]] = VSH UBO (spirv-cross pins the binding=0 UBO here).
 *   [[buffer(1+N)]] = attribute slot N stream (N in 0..15) =
 *     bufferIndex (MTL_ATTR_BUFFER_INDEX_BASE + N).
 */
void pgraph_mtl_draw_translated(void *pipeline_state,
                                const MtlAttributeStream *attr_streams,
                                unsigned int n_attr_streams,
                                unsigned int vertex_count,
                                const uint32_t *indices,
                                unsigned int index_count,
                                uint32_t mtl_primitive,
                                unsigned int viewport_w,
                                unsigned int viewport_h,
                                void *surface_color,
                                void *surface_depth,
                                uint32_t depth_fmt,
                                uint32_t blend_color,
                                uint32_t control_0,
                                uint32_t control_1,
                                uint32_t control_2,
                                uint32_t setup_raster,
                                uint32_t scissor_x,
                                uint32_t scissor_y,
                                uint32_t scissor_w,
                                uint32_t scissor_h,
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

/* M5.5+: render-pass coalescing.
 *
 * The draw module now holds a single MTLCommandBuffer +
 * MTLRenderCommandEncoder open across consecutive flush_draw calls
 * with identical attachment sets. Callers MUST invoke this flush
 * before any operation that depends on the surface texture being
 * stable on the GPU, including:
 *   - flip_stall (NV2A signaled end of frame; compositor reads next).
 *   - clear_surface (clear opens its own pass with loadAction=Clear).
 *   - get_framebuffer_surface (compositor sees the texture).
 *   - savevm / shutdown / surface_flush.
 */
void pgraph_mtl_draw_flush_open_pass(void);

/* Coalescing telemetry. */
uint64_t pgraph_mtl_draw_pass_opens_count(void);
uint64_t pgraph_mtl_draw_pass_coalesced_count(void);
uint64_t pgraph_mtl_draw_pass_flushes_count(void);

/*
 * M5.10 (2026-05-03): expose the texture pointers currently captured
 * inside the coalesced open render pass so the surface cache's eviction
 * / destroy paths can pin them (avoid deallocating an MTLTexture that
 * a queued render encoder still references). Returns NULL via the out
 * parameters when no pass is open. Both out parameters are required;
 * pass NULL for either to skip its read.
 *
 * Accessing the open-pass key is read-only; the underlying state is
 * only mutated under the renderer-thread invariant. Callers under
 * pgraph.lock are safe.
 *
 * M5.10 also adds a draw-queue completion fence: after every draw
 * command-buffer commit, the draw queue signals s_draw_done_event with
 * a monotonic value. Render-queue consumers (surface downloads, blit
 * encoder reads of draw-target textures) call
 * pgraph_mtl_draw_get_done_event_state(&event, &value) and then
 * `[cmdbuf encodeWaitForEvent:event value:value]` on their own
 * command-buffer to ensure prior draw-queue commits have committed
 * before the cross-queue read fires.
 */
void pgraph_mtl_draw_get_open_pass_textures(void **out_color,
                                            void **out_depth);
void pgraph_mtl_draw_get_done_event_state(void **out_event,
                                          uint64_t *out_value);

/*
 * W4 (2026-05-04): per-draw color RT dump.
 *
 * Parses XEMU_METAL_DUMP_DRAW_RT=START:END:PREFIX exactly once at
 * pgraph_mtl_init time. When set, every per-flush_draw invocation
 * whose 0-indexed cumulative-per-RUN counter falls within [START,END]
 * has its bound color render target snapshotted as a PNG at
 * `<PREFIX>.<index_padded_6>.png`. Empty / unset / malformed values
 * disable the dump (zero hot-path cost — one global load + branch).
 *
 * The dump is asynchronous: the open render pass is closed (so the
 * post-MSAA-resolve color texture is the source of truth), then a
 * blit-encoder copies the texture into a host-shared MTLBuffer, and
 * the cmdbuf's addCompletedHandler writes the PNG via FPNG. The
 * renderer thread does not block.
 *
 * Counter: METAL_DRAW_RT_DUMPS (per-interval delta).
 */
void pgraph_mtl_draw_dump_rt_init(void);
void pgraph_mtl_draw_dump_rt_after_flush_draw(void *color_texture);
uint64_t pgraph_mtl_draw_rt_dumps_count(void);

/* True iff XEMU_METAL_DUMP_DRAW_RT was parsed into a usable config at
 * init time. Used by the per-flush_draw wrapper in renderer.c to gate
 * the open-pass flush + color-texture lookup that the dump path needs;
 * when dumping is off the wrapper must NOT close the coalesced pass
 * after every guest draw or the M5.7 coalescing optimization
 * regresses (PGR2/Rainbow draw their scene to a render target whose
 * MSAA companion gets dropped on every spurious pass close, leaving
 * the present pipeline reading uninitialized magenta). */
bool pgraph_mtl_draw_dump_rt_active(void);

/* 2026-05-21 (task #16, cycle 4) — peek the index that the NEXT
 * `pgraph_mtl_draw_dump_rt_after_flush_draw` call will assign to its
 * PNG filename. Returns 0 when XEMU_METAL_DUMP_DRAW_RT is not active
 * (the caller should also check `pgraph_mtl_draw_dump_rt_active()` to
 * disambiguate "not active" from "next index happens to be 0"). Used
 * by sampler-attribution diagnostics in texture_pg.c to cross-reference
 * a stride==44 bind to its eventual draw-RT PNG filename.
 */
uint64_t pgraph_mtl_draw_dump_rt_peek_index(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_DRAW_H */
