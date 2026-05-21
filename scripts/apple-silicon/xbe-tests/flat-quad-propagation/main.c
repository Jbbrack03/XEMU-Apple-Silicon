/*
 * flat-quad-propagation -- Metal FLAT-shaded OP_QUADS regression gate
 *                         (Tier-1 NV2A diag XBE).
 *
 * NV2A feature exercised: §B.1 (primitive assembly OP_QUADS), §C.5
 *                          (polygon fill), §K.1 (per-fragment color),
 *                          §3a.1 (provoking vertex / flat-shade).
 * NV097 methods:           SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET,
 *                          SET_BEGIN_END(QUADS), DRAW_ARRAYS,
 *                          CLEAR_SURFACE (via pb_fill), SET_SHADE_MODEL=FLAT,
 *                          SET_FLAT_SHADE_OP=VERTEX_LAST (NV2A default
 *                          for OP_QUADS: vertex 3 always provoking
 *                          regardless of FLAT_SHADE_OP setting per NV2A
 *                          rule -- but we set VERTEX_LAST explicitly for
 *                          doc clarity), SET_FRONT/BACK_POLYGON_MODE
 *                          (=FILL throughout).
 * Self-validation tier:    1 (host-side capture + counter assertion).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Task #13 regression gate. Metal renderer has no native geometry-
 * shader stage; the GL renderer's GS-driven manual flat-color
 * propagation (copying vertex 3's DIFFUSE across vertices 0/1/2 of
 * each quad before triangulation) is not available. Without CPU-side
 * propagation in mtl/vertex.c::pgraph_mtl_propagate_flat_quad_colors,
 * FLAT-shaded OP_QUADS render BLACK on Metal because the A-C diagonal
 * triangulation puts vertex 0/1/2 first in Metal's [[flat]]
 * interpolation (first-vertex convention) -- and vertex 0/1/2 carry
 * the BLACK distractor color in this XBE while vertex 3 carries the
 * expected color.
 *
 * The XBE renders a 4×2 grid of 8 cells via a single OP_QUADS draw
 * (32 vertices). For each cell:
 *   - Vertex 0 (TL): BLACK distractor
 *   - Vertex 1 (TR): BLACK distractor
 *   - Vertex 2 (BR): BLACK distractor
 *   - Vertex 3 (BL): EXPECTED color (NV2A LAST-vertex provoking)
 *
 * NV2A's OP_QUADS rule: vertex 3 is ALWAYS the provoking vertex for
 * flat shading, regardless of SET_FLAT_SHADE_OP (which only affects
 * OP_TRIANGLES / OP_TRIANGLE_STRIP / OP_TRIANGLE_FAN). See xemu's
 * pgraph state: pg->first_vertex_is_provoking is FALSE by default
 * which makes vertex 3 (last) provoking for QUADS.
 *
 * Cell layout (col, row):
 *
 *   Row 0: RED      GREEN    BLUE     WHITE
 *   Row 1: YELLOW   CYAN     MAGENTA  RED (repeat; pure 0/255 only)
 *
 * On Metal WITHOUT task #13's CPU propagation: every cell renders
 * BLACK (the rasterizer interpolates the BLACK distractor uniformly
 * because Metal's [[flat]] picks vertex 0/1/2 of each tri).
 *
 * On Metal WITH task #13's CPU propagation: every cell renders its
 * EXPECTED color because vertex 3's color was replicated across
 * vertices 0/1/2 in the streams before the draw.
 *
 * --- Counter assertion -----------------------------------------------
 *
 * Manifest declares `required_counters_min.metal.
 * METAL_FLAT_QUAD_PROPAGATIONS >= 100`. This is the per-mode counter
 * registered in mtl/renderer.c (via xemu-perf interval emission) that
 * fires once per dispatch where the CPU propagation path engaged.
 * This counter PROVES that the propagation path was actually
 * exercised on Metal -- not that some unrelated code happened to
 * render the right colors.
 *
 * --- Catches ---------------------------------------------------------
 *
 *   - Metal renderer ships without task #13: all cells BLACK.
 *   - Task #13 regresses (propagation function broken, wrong slots,
 *     etc.): cell colors mis-match, or BLACK distractor leaks through.
 *   - Provoking vertex flipped from LAST to FIRST: cells render BLACK
 *     distractor (because vertex 0 carries BLACK).
 *   - Counter assertion fails: propagation path didn't engage (e.g.
 *     someone disabled the !smooth_shading guard in
 *     mtl_dispatch_decoded_draw).
 *
 * --- Does NOT catch (deferred) ---------------------------------------
 *
 *   - OP_QUAD_STRIP variant: separate follow-up XBE.
 *   - SET_FLAT_SHADE_OP=VERTEX_FIRST + OP_QUADS: NV2A rule says
 *     OP_QUADS still uses vertex 3, but the GL renderer's GS treats
 *     it differently. v0.1 narrows to the common case.
 *   - First-provoking flat quads (NV2A first_vertex_is_provoking=true,
 *     rare): different code path; deferred.
 *
 * --- Reproducibility ------------------------------------------------
 *
 * Pure deterministic 4×2 grid of pure 0/255-component colors.
 * Byte-identical across two cold runs.
 */
#include "xbed_capture.h"
#include "xbed_runtime.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define WIN_W 640
#define WIN_H 480

#define GRID_COLS 4
#define GRID_ROWS 2
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define CELL_W (WIN_W / GRID_COLS)   /* 160 */
#define CELL_H (WIN_H / GRID_ROWS)   /* 240 */

#define VERTS_PER_QUAD 4
#define VERTS_TOTAL (GRID_CELLS * VERTS_PER_QUAD)

typedef struct {
    float pos[3];
    float color[3];
} __attribute__((packed)) ColoredVertex;

/* 8 cell colors. Pure 0/255 components -- survive display gamma
 * byte-exact (per depth-floor v0.2 / native-quad-tri-depth lesson:
 * non-saturated colors are gamma-corrected at scan-out and miss
 * byte-equality by 1-60 LSB per channel). All 8 colors are pure
 * corners of the RGB cube. */
static const float k_cell_rgb[GRID_CELLS][3] = {
    {1.0f, 0.0f, 0.0f},  /* 0 RED    */
    {0.0f, 1.0f, 0.0f},  /* 1 GREEN  */
    {0.0f, 0.0f, 1.0f},  /* 2 BLUE   */
    {1.0f, 1.0f, 1.0f},  /* 3 WHITE  */
    {1.0f, 1.0f, 0.0f},  /* 4 YELLOW  */
    {0.0f, 1.0f, 1.0f},  /* 5 CYAN    */
    {1.0f, 0.0f, 1.0f},  /* 6 MAGENTA */
    /* 7th unique color: RED is the only repeat (the cube has 7 non-
     * black saturated corners; cell 7 repeats RED for the 8th cell.
     * Any single-cell regression still shows up because the
     * distractor BLACK would dominate if propagation failed). */
    {1.0f, 0.0f, 0.0f},  /* 7 RED (repeat) */
};

#define DISTRACTOR_R 0.0f
#define DISTRACTOR_G 0.0f
#define DISTRACTOR_B 0.0f

#define CELL_Z 0.5f

static ColoredVertex s_verts[VERTS_TOTAL];
static ColoredVertex *s_alloc_verts;

static inline void mk_vert(ColoredVertex *v, int x_w, int y_w,
                           float r, float g, float b)
{
    v->pos[0] = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1] = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2] = CELL_Z;
    v->color[0] = r;
    v->color[1] = g;
    v->color[2] = b;
}

/* Emit one FLAT quad in OP_QUADS winding (TL, TR, BR, BL).
 * Vertices 0/1/2 (TL, TR, BR) carry the BLACK distractor.
 * Vertex 3 (BL) carries the EXPECTED color (LAST-vertex provoking). */
static void emit_quad_flat(ColoredVertex *out, int x0, int y0,
                           int x1, int y1, float r, float g, float b)
{
    /* TL = vertex 0 = distractor */
    mk_vert(out + 0, x0, y0, DISTRACTOR_R, DISTRACTOR_G, DISTRACTOR_B);
    /* TR = vertex 1 = distractor */
    mk_vert(out + 1, x1, y0, DISTRACTOR_R, DISTRACTOR_G, DISTRACTOR_B);
    /* BR = vertex 2 = distractor */
    mk_vert(out + 2, x1, y1, DISTRACTOR_R, DISTRACTOR_G, DISTRACTOR_B);
    /* BL = vertex 3 = expected color (provoking) */
    mk_vert(out + 3, x0, y1, r, g, b);
}

static void build_geometry(void)
{
    int q = 0;
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = row * CELL_H;
            const int x1 = x0 + CELL_W;
            const int y1 = y0 + CELL_H;
            emit_quad_flat(s_verts + q, x0, y0, x1, y1,
                           k_cell_rgb[idx][0],
                           k_cell_rgb[idx][1],
                           k_cell_rgb[idx][2]);
            q += VERTS_PER_QUAD;
        }
    }
}

static void enforce_common_state(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    /* FLAT shading with NV2A's default LAST-vertex provoking for
     * OP_QUADS. Note: NV2A's OP_QUADS rule is that vertex 3 is ALWAYS
     * the provoking vertex regardless of SET_FLAT_SHADE_OP (which only
     * applies to OP_TRIANGLES family). We set VERTEX_LAST explicitly
     * here for doc clarity. */
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_FLAT);
    p = pb_push1(p, NV097_SET_FLAT_SHADE_OP,
                 NV097_SET_FLAT_SHADE_OP_VERTEX_LAST);
    pb_end(p);
}

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_verts[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_verts[0].color[0]);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_QUADS, 0, VERTS_TOTAL);
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("flat-quad-propagation v0.1\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    build_geometry();

    s_alloc_verts = MmAllocateContiguousMemoryEx(
        sizeof(s_verts), 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_alloc_verts) {
        debugPrint("MmAllocateContiguousMemoryEx (verts) failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    memcpy(s_alloc_verts, s_verts, sizeof(s_verts));

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\flat-quad-propagation-capture.bin",
        "D:\\flat-quad-propagation-done.txt",
        "flat-quad-propagation");
    return 0;
}
