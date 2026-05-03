/*
 * NV2A PGRAPH Metal renderer — vertex-attribute decode (slice M5.5).
 *
 * Public API for the CPU-side decoder. See vertex.c for the rationale
 * + format coverage.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_VERTEX_H
#define HW_XBOX_NV2A_PGRAPH_MTL_VERTEX_H

#include <stdint.h>

/* Forward decls; the .c side includes nv2a_int.h, callers (renderer.c)
 * already do. */
struct NV2AState;
struct PGRAPHState;

#ifdef __cplusplus
extern "C" {
#endif

enum MtlVertexSource {
    MTL_VERTEX_SRC_VRAM         = 0, /* draw_arrays / inline_elements */
    MTL_VERTEX_SRC_INLINE_ARRAY = 1, /* inline_array (NV097_INLINE_ARRAY) */
};

/*
 * Decode a contiguous range of guest vertex elements into Float4
 * position + Float4 diffuse-color streams. See vertex.c:140-160 for
 * scope.
 *
 * positions_out / colors_out must each be sized to num_elements * 4
 * floats. min_element is the first guest-element index to decode;
 * output element 0 corresponds to guest element min_element.
 */
void pgraph_mtl_collect_vertex_streams(struct NV2AState *d,
                                       enum MtlVertexSource source,
                                       unsigned int inline_stride,
                                       uint32_t min_element,
                                       uint32_t num_elements,
                                       float *positions_out,
                                       float *colors_out);

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

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_VERTEX_H */
