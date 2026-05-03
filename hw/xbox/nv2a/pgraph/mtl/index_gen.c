/*
 * NV2A PGRAPH Metal renderer — index generator implementation (slice M4).
 *
 * Pure C; no Metal API surface. See index_gen.h for the contract and
 * the diagonal-choice rationale that ties this file to PR #2240's
 * polygon-offset depth correctness.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "index_gen.h"

/* -------- capacity helpers -------- */

size_t pgraph_mtl_idx_capacity_triangle_fan(size_t vertex_count)
{
    if (vertex_count < 3) {
        return 0;
    }
    return (vertex_count - 2) * 3;
}

size_t pgraph_mtl_idx_capacity_quads(size_t vertex_count)
{
    return (vertex_count / 4) * 6;
}

size_t pgraph_mtl_idx_capacity_quad_strip(size_t vertex_count)
{
    if (vertex_count < 4) {
        return 0;
    }
    return ((vertex_count - 2) / 2) * 6;
}

size_t pgraph_mtl_idx_capacity_line_strip(size_t vertex_count)
{
    if (vertex_count < 2) {
        return 0;
    }
    return (vertex_count - 1) * 2;
}

size_t pgraph_mtl_idx_capacity_line_loop(size_t vertex_count)
{
    if (vertex_count < 2) {
        return 0;
    }
    return vertex_count * 2;
}

size_t pgraph_mtl_idx_capacity_polygon(size_t vertex_count)
{
    if (vertex_count < 3) {
        return 0;
    }
    return (vertex_count - 2) * 3;
}

/* -------- expansion -------- */

size_t pgraph_mtl_idx_expand_triangle_fan(uint32_t *out,
                                          size_t out_capacity_indices,
                                          size_t vertex_count)
{
    size_t needed = pgraph_mtl_idx_capacity_triangle_fan(vertex_count);
    if (needed == 0 || out == NULL || out_capacity_indices < needed) {
        return 0;
    }
    /* Triangle fan: each triangle shares vertex 0 and consecutive vertex
     * pair (i+1, i+2). Matches Dolphin IndexGenerator AddFan. */
    size_t triangles = vertex_count - 2;
    for (size_t i = 0; i < triangles; i++) {
        out[i * 3 + 0] = 0;
        out[i * 3 + 1] = (uint32_t)(i + 1);
        out[i * 3 + 2] = (uint32_t)(i + 2);
    }
    return triangles * 3;
}

size_t pgraph_mtl_idx_expand_quads(uint32_t *out,
                                   size_t out_capacity_indices,
                                   size_t vertex_count)
{
    size_t needed = pgraph_mtl_idx_capacity_quads(vertex_count);
    if (needed == 0 || out == NULL || out_capacity_indices < needed) {
        return 0;
    }
    /*
     * Diagonal A-C, matching gl/draw.c::native_quad_list_expand_indices
     * with input range [0, vertex_count): for each quad with vertices
     * (a, b, c, d) at offsets (4i, 4i+1, 4i+2, 4i+3) emit
     *     tri 0 = (b, c, a)
     *     tri 1 = (c, d, a)
     * which mirrors geom.c PRIM_TYPE_QUADS POLY_MODE_FILL emission of
     * (1,2,0) + (2,3,0). PR #2240's polygon-offset slope reconstruction
     * uses this exact diagonal — do not change.
     */
    size_t quads = vertex_count / 4;
    for (size_t i = 0; i < quads; i++) {
        uint32_t a = (uint32_t)(4 * i + 0);
        uint32_t b = (uint32_t)(4 * i + 1);
        uint32_t c = (uint32_t)(4 * i + 2);
        uint32_t d = (uint32_t)(4 * i + 3);
        out[i * 6 + 0] = b;
        out[i * 6 + 1] = c;
        out[i * 6 + 2] = a;
        out[i * 6 + 3] = c;
        out[i * 6 + 4] = d;
        out[i * 6 + 5] = a;
    }
    return quads * 6;
}

size_t pgraph_mtl_idx_expand_quad_strip(uint32_t *out,
                                        size_t out_capacity_indices,
                                        size_t vertex_count)
{
    size_t needed = pgraph_mtl_idx_capacity_quad_strip(vertex_count);
    if (needed == 0 || out == NULL || out_capacity_indices < needed) {
        return 0;
    }
    /*
     * Matches gl/draw.c::native_quad_strip_expand_indices: window of
     * (a, b, c, d) at offsets (2i, 2i+1, 2i+2, 2i+3) emits
     *     tri 0 = (a, b, c)
     *     tri 1 = (c, b, d)
     * which mirrors geom.c PRIM_TYPE_QUAD_STRIP POLY_MODE_FILL
     * emission of (0,1,2) + (2,1,3) for even-indexed primitives (the
     * geom shader skips odd primitive IDs; the input range here is the
     * already-deduplicated quad-strip vertex stream, so all windows
     * advance by 2 verts).
     */
    size_t quads = (vertex_count - 2) / 2;
    for (size_t i = 0; i < quads; i++) {
        uint32_t a = (uint32_t)(2 * i + 0);
        uint32_t b = (uint32_t)(2 * i + 1);
        uint32_t c = (uint32_t)(2 * i + 2);
        uint32_t d = (uint32_t)(2 * i + 3);
        out[i * 6 + 0] = a;
        out[i * 6 + 1] = b;
        out[i * 6 + 2] = c;
        out[i * 6 + 3] = c;
        out[i * 6 + 4] = b;
        out[i * 6 + 5] = d;
    }
    return quads * 6;
}

size_t pgraph_mtl_idx_expand_polygon(uint32_t *out,
                                     size_t out_capacity_indices,
                                     size_t vertex_count)
{
    /* PRIM_TYPE_POLYGON in POLY_MODE_FILL fans around vertex 0 — geom.c
     * emits the same triangle layout as PRIM_TYPE_TRIANGLE_FAN for the
     * fill case (lines/points are deferred). Match it directly. */
    size_t needed = pgraph_mtl_idx_capacity_polygon(vertex_count);
    if (needed == 0 || out == NULL || out_capacity_indices < needed) {
        return 0;
    }
    size_t triangles = vertex_count - 2;
    for (size_t i = 0; i < triangles; i++) {
        out[i * 3 + 0] = 0;
        out[i * 3 + 1] = (uint32_t)(i + 1);
        out[i * 3 + 2] = (uint32_t)(i + 2);
    }
    return triangles * 3;
}

size_t pgraph_mtl_idx_expand_line_strip(uint32_t *out,
                                        size_t out_capacity_indices,
                                        size_t vertex_count)
{
    size_t needed = pgraph_mtl_idx_capacity_line_strip(vertex_count);
    if (needed == 0 || out == NULL || out_capacity_indices < needed) {
        return 0;
    }
    size_t segments = vertex_count - 1;
    for (size_t i = 0; i < segments; i++) {
        out[i * 2 + 0] = (uint32_t)i;
        out[i * 2 + 1] = (uint32_t)(i + 1);
    }
    return segments * 2;
}

size_t pgraph_mtl_idx_expand_line_loop(uint32_t *out,
                                       size_t out_capacity_indices,
                                       size_t vertex_count)
{
    size_t needed = pgraph_mtl_idx_capacity_line_loop(vertex_count);
    if (needed == 0 || out == NULL || out_capacity_indices < needed) {
        return 0;
    }
    /* line_loop = line_strip + closing segment back to vertex 0. */
    size_t segments = vertex_count;
    for (size_t i = 0; i < segments - 1; i++) {
        out[i * 2 + 0] = (uint32_t)i;
        out[i * 2 + 1] = (uint32_t)(i + 1);
    }
    out[(segments - 1) * 2 + 0] = (uint32_t)(vertex_count - 1);
    out[(segments - 1) * 2 + 1] = 0;
    return segments * 2;
}
