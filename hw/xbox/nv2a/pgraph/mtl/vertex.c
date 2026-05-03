/*
 * NV2A PGRAPH Metal renderer — vertex-attribute decode (slice M5.5).
 *
 * Apple Silicon performance fork. The Metal renderer's M3/M4 inline-
 * buffer path covers only the immediate-mode NV097 BEGIN/END vertex
 * stream. Real games (PGR2, Crimson Skies, Rainbow Six 3, Halo, etc.)
 * almost exclusively use the draw_arrays / inline_elements /
 * inline_array submission paths, which were short-circuited at
 * renderer.c:462-465. M5.5 lands a CPU-side decoder that reads the
 * relevant range of guest VRAM (or pg->inline_array), decodes each
 * attribute's per-element bytes via its declared NV2A format, and
 * produces flat Float4 streams compatible with the existing
 * pgraph_mtl_draw_passthrough / pgraph_mtl_draw_indexed entry points.
 *
 * Scope (M5.5 minimum-viable):
 *   - Position (NV2A_VERTEX_ATTR_POSITION) and diffuse color
 *     (NV2A_VERTEX_ATTR_DIFFUSE) attributes only — produces what the
 *     existing M3/M4 passthrough pipeline accepts.
 *   - Format support: F (raw float), UB_OGL / UB_D3D (4 ubytes
 *     normalized), S1 (1-4 int16 normalized), S32K (1-4 int16 not
 *     normalized).
 *   - inline_value fallback when stride==0 or count==0.
 *
 * Out of scope (deferred to M5.6 / M7.1 routing):
 *   - Other attributes (normal, fog, texcoords, etc.) — those need the
 *     translated MSL pipeline + std140 VSH/PSH UBOs to be useful.
 *   - CMP (3-component (11,11,10) packed) — extremely rare in the
 *     tracked titles; falls back to inline_value if encountered.
 *   - VRAM page-tracking dirty cache. We re-read the relevant VRAM
 *     range for every flush_draw. The existing surface-cache memory
 *     is host-mapped, so reads are cheap; correctness comes first.
 *
 * The .c extension is intentional — same reason as renderer.c:
 * meson's per-target c_args (-DCOMPILING_PER_TARGET / -DCONFIG_TARGET
 * / -DCONFIG_DEVICES) propagate to .c compiles but not .m / .mm. This
 * file includes nv2a_int.h and produces float-only host buffers; no
 * Metal API surface here.
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
 * format (currently CMP); the caller should fall back to inline_value.
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
    case MTL_NV097_FMT_CMP:
        /* (11, 11, 10) packed signed normalized. Rare on tracked
         * titles; fall back to inline_value. M5.6 to extend. */
        return false;
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
 *
 * attr->count == 0 OR attr->stride == 0 fall back to attr->inline_value
 * (uniform attribute semantics, mirrors vk/vertex.c:148/229).
 */
static void mtl_decode_attribute_element(NV2AState *d,
                                         VertexAttribute *attr,
                                         enum MtlVertexSource source,
                                         unsigned int inline_stride,
                                         uint32_t idx,
                                         float out[4])
{
    if (attr->count == 0 || (attr->stride == 0 &&
                              source != MTL_VERTEX_SRC_INLINE_ARRAY)) {
        /* Uniform attribute — same value for every vertex. */
        out[0] = attr->inline_value[0];
        out[1] = attr->inline_value[1];
        out[2] = attr->inline_value[2];
        out[3] = attr->inline_value[3];
        return;
    }

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

/*
 * Collect a contiguous range of position + diffuse-color values into
 * caller-allocated Float4 streams.
 *
 * - `min_element` / `num_elements`: the range to decode. For
 *   draw_arrays this spans [draw_arrays_min_start ..
 *   draw_arrays_max_count]; for inline_elements it spans
 *   [min(indices) .. max(indices)]. For inline_array it spans
 *   [0 .. index_count].
 * - `positions_out` and `colors_out` must be sized to
 *   num_elements * 4 floats each.
 * - Element index 0 in the output corresponds to guest element
 *   `min_element`; the caller must offset its index buffer
 *   accordingly when issuing indexed draws.
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
        mtl_decode_attribute_element(d, pos_attr, source, inline_stride,
                                     guest_idx, &positions_out[i * 4]);
        mtl_decode_attribute_element(d, col_attr, source, inline_stride,
                                     guest_idx, &colors_out[i * 4]);
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
