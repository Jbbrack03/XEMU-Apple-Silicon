/*
 * NV2A PGRAPH Metal renderer — vertex-attribute decode (slice M5.5 + M5.8).
 *
 * Public API for the CPU-side decoder. See vertex.c for the rationale
 * + format coverage.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_VERTEX_H
#define HW_XBOX_NV2A_PGRAPH_MTL_VERTEX_H

#include <stddef.h>
#include <stdint.h>

/* Forward decls; the .c side includes nv2a_int.h, callers (renderer.c)
 * already do. */
struct NV2AState;
struct PGRAPHState;

#ifdef __cplusplus
extern "C" {
#endif

/* M5.8 buffer-index layout for the translated MSL pipeline.
 *
 * spirv-cross emits the VSH UBO at `[[buffer(0)]]` (because the
 * Vulkan-flavored GLSL generator sets `layout(binding=0)` on
 * VshUniforms, and we run with SPVC_COMPILER_OPTION_MSL_ENABLE_DECORATION_BINDING).
 * MSL's vertex-stage `[[buffer(N)]]` table is shared with the
 * MTLVertexDescriptor's `bufferIndex` slots, so per-vertex attribute
 * streams must NOT use bufferIndex 0 — that would shadow the UBO and
 * the shader would read garbage.
 *
 * We shift attribute streams to `MTL_ATTR_BUFFER_INDEX_BASE + slot`,
 * starting at 1. PSH_UBO is a fragment-stage binding (separate buffer
 * table), so it does not constrain the vertex-stage layout. The 16
 * attribute slots span bufferIndex [1..16], which fits within Apple
 * Silicon's `MTLBufferLayoutDescriptor` count (31).
 */
#define MTL_ATTR_BUFFER_INDEX_BASE 1

enum MtlVertexSource {
    MTL_VERTEX_SRC_VRAM         = 0, /* draw_arrays / inline_elements */
    MTL_VERTEX_SRC_INLINE_ARRAY = 1, /* inline_array (NV097_INLINE_ARRAY) */
};

/* M5.8: per-attribute Float4 stream. NULL data == slot is uniform or
 * inactive (caller should NOT bind a buffer at this slot's bufferIndex
 * — the GLSL generator routes the slot through the VSH UBO's
 * inlineValue[] block instead).
 */
typedef struct MtlAttributeStream {
    float *data;       /* num_elements * 4 floats; NULL when inactive/uniform */
    size_t bytes;      /* sizeof(float) * 4 * num_elements */
} MtlAttributeStream;

#define MTL_VERTEX_NUM_ATTRIBUTES 16  /* Mirrors NV2A_VERTEXSHADER_ATTRIBUTES. */

/*
 * (Legacy M5.5 entry point.) Decode a contiguous range of guest vertex
 * elements into Float4 position + Float4 diffuse-color streams. Kept so
 * the M3/M4 inline_buffer fallback path doesn't have to be rewritten;
 * new callers should use `pgraph_mtl_collect_all_vertex_streams`.
 */
void pgraph_mtl_collect_vertex_streams(struct NV2AState *d,
                                       enum MtlVertexSource source,
                                       unsigned int inline_stride,
                                       uint32_t min_element,
                                       uint32_t num_elements,
                                       float *positions_out,
                                       float *colors_out);

/*
 * M5.8: Decode all NV2A_VERTEXSHADER_ATTRIBUTES (16) vertex-attribute
 * slots into per-slot Float4 streams.
 *
 * For each attribute slot `i`:
 *   - If the slot is genuinely uniform (count == 0, or stride == 0
 *     for VRAM source) — `streams_out[i].data = NULL` and the caller
 *     should leave bufferIndex unbound (the shader reads via the VSH
 *     UBO's inlineValue[] block).
 *   - Otherwise — `streams_out[i].data` is a malloc'd buffer of
 *     `num_elements * 4 * sizeof(float)`, decoded element-by-element
 *     via the format mapping in vertex.c. Caller frees with
 *     `pgraph_mtl_free_attribute_streams`.
 *
 * Mirrors vk/vertex.c::pgraph_vk_bind_vertex_attributes — same
 * uniform-vs-stream classification.
 */
void pgraph_mtl_collect_all_vertex_streams(struct NV2AState *d,
                                           enum MtlVertexSource source,
                                           unsigned int inline_stride,
                                           uint32_t min_element,
                                           uint32_t num_elements,
                                           MtlAttributeStream streams_out[
                                               MTL_VERTEX_NUM_ATTRIBUTES]);

/* Free every stream's `data` and zero the array. Safe to call on
 * partially-populated arrays. */
void pgraph_mtl_free_attribute_streams(MtlAttributeStream streams[
                                           MTL_VERTEX_NUM_ATTRIBUTES]);

/* M5.8: gather per-attribute Float4 streams from
 * `attr->inline_buffer` (the M3/M4 NV097 immediate-mode path). For each
 * slot that has `inline_buffer_populated == true`, `streams_out[i].data`
 * points to attr->inline_buffer (NOT a copy — caller must not free it;
 * the free helper is no-op for these). Inactive slots are left NULL.
 *
 * Returns true on success (slot 0 = POSITION must be populated for any
 * draw to make sense; if not, returns false and the caller should skip
 * the draw).
 */
struct PGRAPHState; /* fwd-redeclare */
typedef struct MtlInlineBufferStreams {
    MtlAttributeStream streams[MTL_VERTEX_NUM_ATTRIBUTES];
} MtlInlineBufferStreams;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compute / publish the per-vertex stride for the inline_array path.
 * `vertex_stride` returns the stride in bytes; the corresponding
 * `update_offsets` writes each VertexAttribute->inline_array_offset
 * so the decoder knows where each attribute's bytes live within the
 * packed inline_array buffer.
 *
 * Mirrors vk/draw.c:2155-2171.
 */
unsigned int pgraph_mtl_inline_array_vertex_stride(struct PGRAPHState *pg);
void         pgraph_mtl_inline_array_update_offsets(struct PGRAPHState *pg);

/*
 * M5.8 (replaces the M5.6 Part B variant): compute pg->uniform_attrs /
 * compressed_attrs / swizzle_attrs for the Metal renderer. Mirrors
 * vk/vertex.c:148-154 + 226-236 — only attribute slots that the
 * guest left genuinely uniform (count == 0, or stride == 0 for VRAM)
 * are routed through the VSH UBO's inlineValue[] block. Per-vertex
 * texcoord/normal/etc. arrays now stream from the M5.8 decoder, so
 * the M5.6 Part B "everything except POSITION + DIFFUSE goes uniform"
 * shortcut is removed.
 *
 * compressed_attrs and swizzle_attrs are zeroed because the M5.8
 * decoder normalizes every output to Float4 — the GLSL generator's
 * CMP/swizzle code paths must not fire (the shader would otherwise
 * try to decompress a value that's already decompressed).
 *
 * Returns the previous values via the out-params so the caller can
 * restore them after the draw (preserving cross-renderer invariants —
 * pg->uniform_attrs is shared state).
 */
void pgraph_mtl_set_attr_masks(struct PGRAPHState *pg,
                               uint16_t *prev_uniform_attrs,
                               uint16_t *prev_compressed_attrs,
                               uint16_t *prev_swizzle_attrs);

/*
 * Restore the previous attribute mask values saved by
 * pgraph_mtl_set_attr_masks.
 */
void pgraph_mtl_restore_attr_masks(struct PGRAPHState *pg,
                                   uint16_t prev_uniform_attrs,
                                   uint16_t prev_compressed_attrs,
                                   uint16_t prev_swizzle_attrs);

/*
 * M5.8: variant of pgraph_mtl_set_attr_masks for the M3/M4
 * inline_buffer fallback path. Marks every slot whose
 * `attr->inline_buffer_populated == false` as uniform. Mirrors
 * vk/vertex.c::pgraph_vk_bind_vertex_attributes_inline.
 */
void pgraph_mtl_set_attr_masks_inline_buffer(struct PGRAPHState *pg,
                                             uint16_t *prev_uniform_attrs,
                                             uint16_t *prev_compressed_attrs,
                                             uint16_t *prev_swizzle_attrs);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_VERTEX_H */
