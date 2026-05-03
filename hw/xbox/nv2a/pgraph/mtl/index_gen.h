/*
 * NV2A PGRAPH Metal renderer — index generator (slice M4).
 *
 * CPU-side index expansion for primitive types Metal does not expose
 * natively (triangle fans, line loops, quads, quad strips, polygons).
 * Mirrors the Dolphin VideoCommon/IndexGenerator.cpp pattern of
 * "rewrite the index buffer so the GPU draws triangle/line lists".
 *
 * The quad triangulation matches the GL native_quad path's diagonal
 * choice (gl/draw.c::native_quad_list_expand_indices and
 * native_quad_strip_expand_indices), which in turn matches the
 * geometry-shader emit order in glsl/geom.c::PRIM_TYPE_QUADS /
 * QUAD_STRIP — i.e. diagonal A-C of each quad. PR #2240's
 * polygon-offset slope reconstruction depends on this diagonal choice;
 * deviating from it would break depth correctness on quads.
 *
 * All functions return the number of output indices written. Callers
 * are responsible for sizing `out` according to the matching
 * pgraph_mtl_idx_capacity_* helper or the inline formula in each
 * function's contract:
 *
 *   - triangle_fan: vertex_count >= 3 → (vertex_count - 2) * 3 indices
 *   - quads:        vertex_count / 4 quads → (vertex_count / 4) * 6
 *   - quad_strip:   vertex_count >= 4 → ((vertex_count - 2) / 2) * 6
 *   - line_strip:   vertex_count >= 2 → (vertex_count - 1) * 2
 *   - line_loop:    vertex_count >= 2 → vertex_count * 2
 *   - polygon:      vertex_count >= 3 → (vertex_count - 2) * 3
 *
 * Indices are uint32_t. The expansion is contiguous-vertex-range based
 * (no gather from an existing index list); for the inline_buffer path
 * the input vertex range is always [0, vertex_count) and these helpers
 * cover that case directly. Index-list expansion (for inline_elements)
 * lands when M5 ports the inline_elements / draw_arrays paths.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_INDEX_GEN_H
#define HW_XBOX_NV2A_PGRAPH_MTL_INDEX_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Capacity helpers — bytes-of-uint32 needed to hold the full expansion
 * for `vertex_count` input vertices. Returns 0 when the input is too
 * small to produce any output (e.g. quads with < 4 verts).
 */
size_t pgraph_mtl_idx_capacity_triangle_fan(size_t vertex_count);
size_t pgraph_mtl_idx_capacity_quads(size_t vertex_count);
size_t pgraph_mtl_idx_capacity_quad_strip(size_t vertex_count);
size_t pgraph_mtl_idx_capacity_line_strip(size_t vertex_count);
size_t pgraph_mtl_idx_capacity_line_loop(size_t vertex_count);
size_t pgraph_mtl_idx_capacity_polygon(size_t vertex_count);

/*
 * Expand a contiguous vertex range [0, vertex_count) into triangle-list
 * indices. Returns the number of indices written, or 0 if the input is
 * too small.
 *
 * Triangle fan (vertex_count - 2 triangles):
 *   tri[i] = (0, i + 1, i + 2)
 *
 * Quads (vertex_count / 4 quads, diagonal A-C, matching geom.c order
 *        (1,2,0) + (2,3,0) — see gl/draw.c::native_quad_list_expand_indices):
 *   quad i with verts (a, b, c, d):
 *     tri 0 = (b, c, a)
 *     tri 1 = (c, d, a)
 *
 * Quad strip ((vertex_count - 2) / 2 quads, matching geom.c order
 *             (0,1,2) + (2,1,3) — see gl/draw.c::native_quad_strip_expand_indices):
 *   window i (verts a, b, c, d):
 *     tri 0 = (a, b, c)
 *     tri 1 = (c, b, d)
 *
 * Polygon (vertex_count - 2 triangles, fan triangulation around vertex 0
 *          matching geom.c PRIM_TYPE_POLYGON in POLY_MODE_FILL):
 *   tri[i] = (0, i + 1, i + 2)
 */
size_t pgraph_mtl_idx_expand_triangle_fan(uint32_t *out,
                                          size_t out_capacity_indices,
                                          size_t vertex_count);
size_t pgraph_mtl_idx_expand_quads(uint32_t *out,
                                   size_t out_capacity_indices,
                                   size_t vertex_count);
size_t pgraph_mtl_idx_expand_quad_strip(uint32_t *out,
                                        size_t out_capacity_indices,
                                        size_t vertex_count);
size_t pgraph_mtl_idx_expand_polygon(uint32_t *out,
                                     size_t out_capacity_indices,
                                     size_t vertex_count);

/*
 * Expand a contiguous vertex range into line-list indices.
 *
 * line_strip (vertex_count - 1 segments):
 *   line[i] = (i, i + 1)
 *
 * line_loop (vertex_count segments — closes back to vertex 0):
 *   line[i]                = (i, i + 1)   for i in [0, vertex_count - 1)
 *   line[vertex_count - 1] = (vertex_count - 1, 0)
 */
size_t pgraph_mtl_idx_expand_line_strip(uint32_t *out,
                                        size_t out_capacity_indices,
                                        size_t vertex_count);
size_t pgraph_mtl_idx_expand_line_loop(uint32_t *out,
                                       size_t out_capacity_indices,
                                       size_t vertex_count);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_INDEX_GEN_H */
