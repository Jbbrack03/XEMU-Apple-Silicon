/*
 * NV2A PGRAPH Metal renderer — vertex-attribute decode (slice M5.5
 * + M5.8 — full per-vertex attribute decoder for every NV2A slot).
 *
 * Apple Silicon performance fork. The Metal renderer's M3/M4 inline-
 * buffer path covers only the immediate-mode NV097 BEGIN/END vertex
 * stream. Real games (PGR2, Crimson Skies, Rainbow Six 3, Halo, etc.)
 * almost exclusively use the draw_arrays / inline_elements /
 * inline_array submission paths.
 *
 * M5.5 landed a CPU-side decoder that produced flat Float4 streams for
 * just POSITION + DIFFUSE. M5.6 Part B closed the resulting "missing
 * attribute" pipeline failures by routing everything else through the
 * VSH UBO's `inlineValue[]` block — that traded translator-build
 * correctness for the shader-side "every per-vertex texcoord / normal
 * is a single inline_value" approximation. The visible result was solid
 * green-screen rendering on PGR2 and a 24× drop in draw throughput.
 *
 * M5.8 lands the full per-attribute decoder, mirroring vk/vertex.c.
 * For each NV2A attribute slot:
 *   - count == 0  OR (stride == 0 for VRAM source) → uniform attr;
 *     leave NULL output, GLSL emits inlineValue[k]
 *   - otherwise → decode the slot's full per-element bytes into a
 *     Float4 stream. Caller binds each non-NULL stream at its
 *     bufferIndex.
 *
 * Format coverage (extended from M5.5): F (raw float), UB_OGL / UB_D3D
 * (4 ubytes normalized), S1 (1-4 int16 normalized), S32K (1-4 int16
 * not normalized), CMP (signed (11,11,10) packed).
 *
 * The .c extension is intentional — same reason as renderer.c:
 * meson's per-target c_args propagate to .c compiles but not .m / .mm.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"

#include "vertex.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* NV097 vertex-data-array format types. Mirrors nv2a_regs.h:1138-1144. */
#define MTL_NV097_FMT_UB_D3D 0
#define MTL_NV097_FMT_S1     1
#define MTL_NV097_FMT_F      2
#define MTL_NV097_FMT_UB_OGL 4
#define MTL_NV097_FMT_S32K   5
#define MTL_NV097_FMT_CMP    6

/* Decode one element's bytes (format-aware) into a Float4.
 *
 * `bytes` points to the per-element data; `count` is attr->count
 * (1-4); `format` is one of the MTL_NV097_FMT_* values.
 *
 * Missing components default to {x, y, z, w} = {0, 0, 0, 1} — the
 * standard "promote to homogeneous" semantics shared with the GL
 * renderer (see gl/vertex.c) and Direct3D 8.
 *
 * Returns true on a successful decode. False indicates an unsupported
 * format; the caller should fall back to inline_value.
 */
static bool mtl_decode_one_element(const uint8_t *bytes, unsigned int count,
                                   unsigned int format, float out[4])
{
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;

    switch (format) {
    case MTL_NV097_FMT_F: {
        const float *src = (const float *)bytes;
        for (unsigned int c = 0; c < count && c < 4; c++) {
            out[c] = src[c];
        }
        return true;
    }
    case MTL_NV097_FMT_UB_OGL: {
        /* RGBA order, normalized [0, 1]. */
        for (unsigned int c = 0; c < count && c < 4; c++) {
            out[c] = (float)bytes[c] * (1.0f / 255.0f);
        }
        return true;
    }
    case MTL_NV097_FMT_UB_D3D: {
        /* BGRA byte order in memory; emit as RGBA. */
        if (count == 4) {
            out[0] = (float)bytes[2] * (1.0f / 255.0f); /* R from B-slot */
            out[1] = (float)bytes[1] * (1.0f / 255.0f); /* G */
            out[2] = (float)bytes[0] * (1.0f / 255.0f); /* B from R-slot */
            out[3] = (float)bytes[3] * (1.0f / 255.0f); /* A */
        } else {
            /* Non-4 D3D-ubyte is unusual; treat as OGL. */
            for (unsigned int c = 0; c < count && c < 4; c++) {
                out[c] = (float)bytes[c] * (1.0f / 255.0f);
            }
        }
        return true;
    }
    case MTL_NV097_FMT_S1: {
        /* int16 normalized [-1, 1]. */
        const int16_t *src = (const int16_t *)bytes;
        for (unsigned int c = 0; c < count && c < 4; c++) {
            int16_t v = src[c];
            out[c] = (float)v * (1.0f / 32767.0f);
            if (out[c] < -1.0f) {
                out[c] = -1.0f;
            }
        }
        return true;
    }
    case MTL_NV097_FMT_S32K: {
        /* int16 not normalized — used for vertex weights / generic
         * integers. We emit the raw integer as a float, matching the
         * GL renderer's semantics. */
        const int16_t *src = (const int16_t *)bytes;
        for (unsigned int c = 0; c < count && c < 4; c++) {
            out[c] = (float)src[c];
        }
        return true;
    }
    case MTL_NV097_FMT_CMP: {
        /* CMP — 3 signed-normalized components packed into 32 bits as
         * (11, 11, 10). NV2A always uses count == 1 with size == 4; the
         * shader-side decompress_11_11_10() does the unpack on Vulkan
         * via VK_FORMAT_R32_SINT input — but on Metal we already
         * decompress here so the GLSL generator's compressed_attrs
         * branch never fires.
         *
         * Bit layout (LSB-first per the NV2A docs): X is bits [0..10]
         * (11 bits), Y is bits [11..21] (11 bits), Z is bits [22..31]
         * (10 bits). Each signed-normalized: divide by 1023 (X/Y) or
         * 511 (Z) and clamp to [-1, 1].
         */
        const uint32_t *src = (const uint32_t *)bytes;
        uint32_t v = *src;
        /* Sign-extend the 11-bit / 10-bit fields by left-shifting and
         * arithmetic-shifting back. */
        int32_t x = (int32_t)(v << 21) >> 21;       /* bits 0..10  → 11-bit signed */
        int32_t y = (int32_t)(v <<  10) >> 21;      /* bits 11..21 → 11-bit signed */
        int32_t z = (int32_t)(v) >> 22;             /* bits 22..31 → 10-bit signed */
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        if (out[0] < -1.0f) out[0] = -1.0f;
        if (out[1] < -1.0f) out[1] = -1.0f;
        if (out[2] < -1.0f) out[2] = -1.0f;
        return true;
    }
    default:
        return false;
    }
}

/*
 * Decode attribute `attr` for one element index `idx`.
 *
 * Source data location:
 *   - inline_array path (`source == MTL_VERTEX_SRC_INLINE_ARRAY`):
 *     pg->inline_array, offset attr->inline_array_offset, stride =
 *     `inline_stride` (the per-vertex width passed from the caller).
 *   - VRAM path: nv_dma_map(dma_vertex_a / dma_vertex_b) + attr->offset,
 *     stride = attr->stride.
 */
static void mtl_decode_attribute_element(NV2AState *d,
                                         VertexAttribute *attr,
                                         enum MtlVertexSource source,
                                         unsigned int inline_stride,
                                         uint32_t idx,
                                         float out[4])
{
    const uint8_t *base = NULL;
    size_t stride = 0;

    if (source == MTL_VERTEX_SRC_INLINE_ARRAY) {
        if (d->pgraph.inline_array == NULL) {
            out[0] = attr->inline_value[0];
            out[1] = attr->inline_value[1];
            out[2] = attr->inline_value[2];
            out[3] = attr->inline_value[3];
            return;
        }
        base = (const uint8_t *)d->pgraph.inline_array +
               attr->inline_array_offset;
        stride = inline_stride;
    } else {
        hwaddr dma_len = 0;
        uint8_t *dma_ptr = (uint8_t *)nv_dma_map(
            d, attr->dma_select ? d->pgraph.dma_vertex_b
                                : d->pgraph.dma_vertex_a,
            &dma_len);
        if (dma_ptr == NULL || attr->offset >= dma_len) {
            out[0] = attr->inline_value[0];
            out[1] = attr->inline_value[1];
            out[2] = attr->inline_value[2];
            out[3] = attr->inline_value[3];
            return;
        }
        base = dma_ptr + attr->offset;
        stride = attr->stride;
    }

    const uint8_t *element = base + (size_t)idx * stride;
    if (!mtl_decode_one_element(element, attr->count, attr->format, out)) {
        out[0] = attr->inline_value[0];
        out[1] = attr->inline_value[1];
        out[2] = attr->inline_value[2];
        out[3] = attr->inline_value[3];
    }
}

/* Helper: classify whether a slot has an active per-vertex stream
 * (i.e. NOT uniform). Mirrors vk/vertex.c:148-154 + 226-236.
 *
 * For the inline_array source, stride is supplied by the caller (the
 * sum of all enabled attrs' size*count); attrs with count!=0 always
 * stream from inline_array regardless of attr->stride (which is 0
 * for inline_array — it has no meaning there).
 */
static bool mtl_attr_is_streaming(const VertexAttribute *attr,
                                  enum MtlVertexSource source)
{
    if (attr->count == 0) {
        return false;
    }
    if (source == MTL_VERTEX_SRC_VRAM && attr->stride == 0) {
        return false;
    }
    return true;
}

/*
 * Legacy M5.5 entry point — POSITION + DIFFUSE only. Retained for the
 * inline_buffer fallback in renderer.c (which currently still uses the
 * 2-stream M3/M4 hand-coded passthrough pipeline).
 */
void pgraph_mtl_collect_vertex_streams(NV2AState *d,
                                       enum MtlVertexSource source,
                                       unsigned int inline_stride,
                                       uint32_t min_element,
                                       uint32_t num_elements,
                                       float *positions_out,
                                       float *colors_out)
{
    PGRAPHState *pg = &d->pgraph;
    VertexAttribute *pos_attr = &pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION];
    VertexAttribute *col_attr = &pg->vertex_attributes[NV2A_VERTEX_ATTR_DIFFUSE];

    for (uint32_t i = 0; i < num_elements; i++) {
        uint32_t guest_idx = min_element + i;
        if (mtl_attr_is_streaming(pos_attr, source)) {
            mtl_decode_attribute_element(d, pos_attr, source, inline_stride,
                                         guest_idx, &positions_out[i * 4]);
        } else {
            positions_out[i * 4 + 0] = pos_attr->inline_value[0];
            positions_out[i * 4 + 1] = pos_attr->inline_value[1];
            positions_out[i * 4 + 2] = pos_attr->inline_value[2];
            positions_out[i * 4 + 3] = pos_attr->inline_value[3];
        }
        if (mtl_attr_is_streaming(col_attr, source)) {
            mtl_decode_attribute_element(d, col_attr, source, inline_stride,
                                         guest_idx, &colors_out[i * 4]);
        } else {
            colors_out[i * 4 + 0] = col_attr->inline_value[0];
            colors_out[i * 4 + 1] = col_attr->inline_value[1];
            colors_out[i * 4 + 2] = col_attr->inline_value[2];
            colors_out[i * 4 + 3] = col_attr->inline_value[3];
        }
    }
}

/*
 * M5.8: full per-attribute decoder. Allocates a Float4 stream for every
 * slot that the guest fed via a vertex array; leaves NULL for slots
 * that should route through the VSH UBO's inlineValue[] (uniform attrs).
 *
 * Caller frees with `pgraph_mtl_free_attribute_streams`.
 */
void pgraph_mtl_collect_all_vertex_streams(NV2AState *d,
                                           enum MtlVertexSource source,
                                           unsigned int inline_stride,
                                           uint32_t min_element,
                                           uint32_t num_elements,
                                           MtlAttributeStream streams_out[
                                               MTL_VERTEX_NUM_ATTRIBUTES])
{
    PGRAPHState *pg = &d->pgraph;

    for (int i = 0; i < MTL_VERTEX_NUM_ATTRIBUTES; i++) {
        streams_out[i].data = NULL;
        streams_out[i].bytes = 0;
    }

    if (num_elements == 0) {
        return;
    }

    for (int i = 0; i < MTL_VERTEX_NUM_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (!mtl_attr_is_streaming(attr, source)) {
            continue;
        }

        size_t bytes = (size_t)num_elements * 4 * sizeof(float);
        float *out = (float *)g_malloc(bytes);
        if (out == NULL) {
            /* OOM: leave NULL so the caller treats this slot as uniform.
             * The shader still gets attr->inline_value via the GLSL
             * generator's fallback. */
            continue;
        }

        for (uint32_t k = 0; k < num_elements; k++) {
            uint32_t guest_idx = min_element + k;
            mtl_decode_attribute_element(d, attr, source, inline_stride,
                                         guest_idx, &out[k * 4]);
        }

        streams_out[i].data = out;
        streams_out[i].bytes = bytes;
    }
}

void pgraph_mtl_free_attribute_streams(MtlAttributeStream streams[
                                           MTL_VERTEX_NUM_ATTRIBUTES])
{
    for (int i = 0; i < MTL_VERTEX_NUM_ATTRIBUTES; i++) {
        if (streams[i].data != NULL) {
            g_free(streams[i].data);
            streams[i].data = NULL;
            streams[i].bytes = 0;
        }
    }
}

/* Compute the per-vertex stride for the inline_array path by summing
 * each enabled attribute's bytes (size * count, padded to alignment).
 * Returns 0 on failure.
 *
 * Matches vk/draw.c:2155-2171 (inline_array branch). */
unsigned int pgraph_mtl_inline_array_vertex_stride(PGRAPHState *pg)
{
    unsigned int offset = 0;
    for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->count == 0) {
            continue;
        }
        if (attr->size > 0) {
            unsigned int round = attr->size;
            offset = ((offset + round - 1) / round) * round;
        }
        offset += attr->size * attr->count;
        if (attr->size > 0) {
            unsigned int round = attr->size;
            offset = ((offset + round - 1) / round) * round;
        }
    }
    return offset;
}

/* Update each enabled attribute's inline_array_offset for the
 * inline_array path, matching the layout we'll decode against. */
void pgraph_mtl_inline_array_update_offsets(PGRAPHState *pg)
{
    unsigned int offset = 0;
    for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->count == 0) {
            continue;
        }
        if (attr->size > 0) {
            unsigned int round = attr->size;
            offset = ((offset + round - 1) / round) * round;
        }
        attr->inline_array_offset = offset;
        offset += attr->size * attr->count;
        if (attr->size > 0) {
            unsigned int round = attr->size;
            offset = ((offset + round - 1) / round) * round;
        }
    }
}

/* M5.8: see vertex.h for the rationale.
 *
 * Mirrors vk/vertex.c:148-154 + 226-236. The M5.6 Part B "everything
 * except POSITION + DIFFUSE goes uniform" shortcut is removed — the
 * M5.8 decoder produces real per-vertex streams for every active
 * slot, so the GLSL generator should emit `[[attribute(N)]]` reads
 * for them (`uniform_attrs` bit clear).
 *
 * compressed_attrs and swizzle_attrs are zeroed because the M5.8
 * decoder normalizes every output to Float4 — the GLSL generator's
 * CMP-decompress / D3D-swizzle code paths must not fire (the data is
 * already decompressed / unswizzled at decode time).
 */
void pgraph_mtl_set_attr_masks(PGRAPHState *pg,
                               uint16_t *prev_uniform_attrs,
                               uint16_t *prev_compressed_attrs,
                               uint16_t *prev_swizzle_attrs)
{
    if (prev_uniform_attrs) {
        *prev_uniform_attrs = pg->uniform_attrs;
    }
    if (prev_compressed_attrs) {
        *prev_compressed_attrs = pg->compressed_attrs;
    }
    if (prev_swizzle_attrs) {
        *prev_swizzle_attrs = pg->swizzle_attrs;
    }

    uint16_t uniform_mask = 0;

    for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];

        /* count == 0 → guest never bound an array; it's feeding the
         * value via NV097_SET_VERTEX_DATA*F immediate registers, which
         * already route through pgraph_update_inline_value into
         * attr->inline_value[]. Mirrors vk/vertex.c:148-154. */
        if (attr->count == 0) {
            uniform_mask |= (uint16_t)(1u << i);
            continue;
        }

        /* stride == 0 with count != 0 is the "all vertices use first
         * element" pattern; the attribute is effectively uniform. We
         * route via the inline_value (which the upstream NV2A code
         * keeps in sync with the first-element bytes via
         * pgraph_update_inline_value). Mirrors vk/vertex.c:226-236. */
        if (attr->stride == 0) {
            uniform_mask |= (uint16_t)(1u << i);
            continue;
        }
    }

    pg->uniform_attrs    = uniform_mask;
    pg->compressed_attrs = 0;
    pg->swizzle_attrs    = 0;
}

void pgraph_mtl_set_attr_masks_inline_buffer(PGRAPHState *pg,
                                             uint16_t *prev_uniform_attrs,
                                             uint16_t *prev_compressed_attrs,
                                             uint16_t *prev_swizzle_attrs)
{
    if (prev_uniform_attrs) {
        *prev_uniform_attrs = pg->uniform_attrs;
    }
    if (prev_compressed_attrs) {
        *prev_compressed_attrs = pg->compressed_attrs;
    }
    if (prev_swizzle_attrs) {
        *prev_swizzle_attrs = pg->swizzle_attrs;
    }

    uint16_t uniform_mask = 0;
    for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (!attr->inline_buffer_populated) {
            uniform_mask |= (uint16_t)(1u << i);
        }
    }
    pg->uniform_attrs    = uniform_mask;
    pg->compressed_attrs = 0;
    pg->swizzle_attrs    = 0;
}

void pgraph_mtl_restore_attr_masks(PGRAPHState *pg,
                                   uint16_t prev_uniform_attrs,
                                   uint16_t prev_compressed_attrs,
                                   uint16_t prev_swizzle_attrs)
{
    pg->uniform_attrs    = prev_uniform_attrs;
    pg->compressed_attrs = prev_compressed_attrs;
    pg->swizzle_attrs    = prev_swizzle_attrs;
}
