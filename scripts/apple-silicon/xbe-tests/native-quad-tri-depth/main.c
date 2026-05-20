/*
 * native-quad-tri-depth — GS-bypass regression gate (Tier-1 NV2A diag XBE).
 *
 * NV2A feature exercised: §B.1 (primitive assembly), §C.5 (polygon fill),
 *                          §K.1/K.2 (per-fragment color), §H.5 (clear),
 *                          §3a.1 (provoking vertex / flat-shade op).
 * NV097 methods:           SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET,
 *                          SET_BEGIN_END(QUADS, TRIANGLES), DRAW_ARRAYS,
 *                          CLEAR_SURFACE (via pb_fill), SET_SHADE_MODEL
 *                          (toggles SMOOTH ↔ FLAT for the bottom-half
 *                          row-2 stripe only), SET_FLAT_SHADE_OP
 *                          (VERTEX_FIRST for the FLAT triangle stripe),
 *                          SET_FRONT/BACK_POLYGON_MODE (=FILL throughout).
 * Self-validation tier:    1 (host-side capture + counter assertion).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests (Codex-revised 2026-05-20 evening, late) ----
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.5, this is the regression gate for
 * the two closed default-on Apple Silicon flags `XEMU_NATIVE_QUAD` and
 * `XEMU_NATIVE_TRI_DEPTH`. Both flags bypass the geometry shader and
 * derive per-fragment depth in the fragment shader instead.
 *
 * The XBE renders three stripe-passes per frame:
 *
 *   PASS 1 (TOP    half y in [0,   240), OP_QUADS,     SMOOTH)
 *     - 12 cells via OP_QUADS, SHADE_MODEL_SMOOTH. Engages NATIVE_QUAD
 *       on Metal/GL when the flag is on (`glsl/geom.c:162-197` —
 *       requires SMOOTH + FILL on both faces + a QUADS / QUAD_STRIP
 *       primitive). Every cell's 4 vertices carry the same expected
 *       color so smooth interpolation produces a uniform cell.
 *
 *   PASS 2 (BOTTOM half y in [240, 400), OP_TRIANGLES, SMOOTH)
 *     - 8 cells via OP_TRIANGLES, SHADE_MODEL_SMOOTH. Engages
 *       NATIVE_TRI_DEPTH (per `glsl/geom.c:135-160` — eligible for
 *       smooth-shaded triangle primitives in FILL mode). All 6 verts
 *       per cell carry the same expected color.
 *
 *   PASS 3 (BOTTOM half y in [400, 480), OP_TRIANGLES, FLAT, FLAT_SHADE_OP=VERTEX_FIRST)
 *     - 4 cells via OP_TRIANGLES, SHADE_MODEL_FLAT + FLAT_SHADE_OP=
 *       VERTEX_FIRST. Engages NATIVE_TRI_DEPTH's first-provoking
 *       branch (per `glsl/geom.c:156`: returns true for FLAT only when
 *       `first_vertex_is_provoking`). Per-cell: TL=EXPECTED,
 *       TR/BR/BL=BLACK distractor. Each emitted triangle's vertex 0 =
 *       TL provides the cell color via the manual flat-propagation
 *       path in NATIVE_TRI_DEPTH.
 *
 * The math-derived expected output is the SAME 4×3 grid in both
 * halves: each cell takes the same saturated 0/255-RGB color
 * regardless of which pass produced it. Top-half-vs-bottom-half
 * byte-equality is the pixel gate.
 *
 * --- Why no FLAT-quad stripe (originally proposed, then removed) ----
 *
 * An earlier revision of this XBE (committed and Codex-validated
 * 2026-05-20 evening, late) also rendered a FLAT-shaded OP_QUADS
 * stripe in the top half's row 2 to test that NV2A's quad rule
 * (vertex 3 always provoking, independent of FLAT_SHADE_OP) was
 * honored across the GS-fallback path. The first run on Metal
 * caught a real correctness gap: Metal renders FLAT-shaded
 * OP_QUADS as all BLACK because Metal has no native geometry-
 * shader stage (`shader_validation.c:206-228`,
 * `state.h:29-32` explicitly: "flat-non-first-provoking are not
 * exercised through the Metal port") and the renderer does not
 * yet do CPU-side flat-color propagation for the quad-family.
 *
 * Rather than gate this XBE on a known-deferred renderer feature,
 * the FLAT-quad coverage is filed as a follow-up XBE
 * (`flat-quad-propagation`, second-wave) plus a tracked Metal-
 * renderer slice to implement CPU-side flat-color propagation for
 * OP_QUADS/QUAD_STRIP when SHADE_MODEL_FLAT is active. See
 * decision-log "2026-05-20 (evening, late): native-quad-tri-depth
 * XBE caught Metal FLAT-quad gap" for the trail.
 *
 * --- What this XBE catches today -------------------------------------
 *
 *   - SMOOTH stripe top half vs bottom half mismatch: NATIVE_QUAD's
 *     CPU triangulation rasterizes different pixels than
 *     NATIVE_TRI_DEPTH's reference, OR NATIVE_TRI_DEPTH's per-fragment
 *     depth output corrupts the color path, OR the smooth-shading
 *     attribute interpolation diverges between the two paths.
 *   - FLAT-tri V_FIRST stripe wrong color: NATIVE_TRI_DEPTH's
 *     first-vertex-provoking path selected the wrong vertex (would
 *     show BLACK distractor instead of EXPECTED).
 *   - Silent bypass fall-through to GS: caught by the
 *     manifest-declared `required_counters_min` (NATIVE_QUAD_DRAW for
 *     the SMOOTH-quad path, plus the per-stripe-specific
 *     NATIVE_TRI_DEPTH_DRAW_SMOOTH and NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST
 *     for the two triangle stripes -- the aggregate
 *     NATIVE_TRI_DEPTH_DRAW / METAL_NATIVE_TRI_DEPTH_DRAWS is
 *     intentionally NOT asserted because PASS 2 SMOOTH alone would
 *     trivially satisfy any aggregate threshold and mask a PASS 3
 *     FLAT_FIRST regression -- Codex 2026-05-20 finding).
 *     Each counter must accumulate >= 100 (well below the ~300
 *     draws this XBE emits per counter per renderer over its
 *     300-frame loop).
 *
 * --- Why saturated 0/255 colors --------------------------------------
 *
 * Per the depth-floor v0.2 lesson (decision-log 2026-05-06): the Xbox
 * kernel sets a display-side gamma table at video init, so intermediate
 * per-cell colors get gamma-correct'd at scan-out and the captured PNG
 * differs from the math-derived expected by ~1-3 LSB per channel. Pure
 * 0/255 components survive any gamma table byte-exact. The 6 cell
 * colors (RED/GREEN/BLUE/WHITE/YELLOW/CYAN) plus the BLACK distractor
 * are all pure corners of the RGB cube.
 *
 * --- Geometry layout --------------------------------------------------
 *
 * WIN_W=640, WIN_H=480. 4 cols (each 160 px wide). Per half: 3 rows
 * (each 80 px tall). 12 cells per half, 24 cells total.
 *
 * Top-half cell (col, row): x = col*160, y = row*80.
 * Bottom-half cell (col, row): same x, +240 to y.
 *
 * Cell colors are identical per-position in both halves. Per row:
 *
 *   Row 0: RED      GREEN    BLUE     WHITE
 *   Row 1: YELLOW   CYAN     RED      GREEN
 *   Row 2: BLUE     WHITE    YELLOW   CYAN
 *
 * --- Math derivation (640x480 X8R8G8B8 front buffer) ------------------
 *
 * For pixel (x, y) in [0,640) x [0,480):
 *   col = x / 160
 *   row = (y < 240) ? (y / 80) : ((y - 240) / 80)
 *   expected_argb = CELL_COLORS[row * 4 + col]
 *
 * pb_fill(0, 0, W, H, BG) clears the back buffer to opaque black
 * 0xFF000000 before any draw. The 3 stripe-passes paint exactly each
 * half of the screen.
 *
 * Reproducibility: byte-identical-after-mask across two cold runs.
 * Pure deterministic pattern, no banner, no counter, no per-frame
 * variation.
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
#define HALF_H (WIN_H / 2)     /* 240 */

#define CELL_W (WIN_W / 4)     /* 160 */
#define CELL_H (HALF_H / 3)    /* 80; half-height divided into 3 rows */

#define GRID_COLS         4
#define GRID_ROWS         3
#define SMOOTH_TRI_ROWS   2  /* bottom half rows 0-1 are SMOOTH OP_TRIANGLES */
#define FLAT_TRI_ROWS     1  /* bottom half row   2  is  FLAT  OP_TRIANGLES */

#define ALL_CELLS_PER_HALF   (GRID_ROWS * GRID_COLS)             /* 12 */
#define SMOOTH_TRI_CELLS     (SMOOTH_TRI_ROWS * GRID_COLS)       /* 8 */
#define FLAT_TRI_CELLS       (FLAT_TRI_ROWS * GRID_COLS)         /* 4 */

#define QUAD_VERTS_PER_CELL  4
#define TRI_VERTS_PER_CELL   6  /* two triangles, A-B-C / A-C-D */

#define QUAD_VERTS_TOTAL    (ALL_CELLS_PER_HALF * QUAD_VERTS_PER_CELL) /* 48 */
#define SMOOTH_TRI_VERTS    (SMOOTH_TRI_CELLS   * TRI_VERTS_PER_CELL)  /* 48 */
#define FLAT_TRI_VERTS      (FLAT_TRI_CELLS     * TRI_VERTS_PER_CELL)  /* 24 */

typedef struct {
    float pos[3];
    float color[3];
} __attribute__((packed)) ColoredVertex;

/* 12 cell colors. Pure 0/255 components -- survive display gamma.
 * Identical color positions for both halves. Indexed as row * 4 + col. */
static const float k_cell_rgb[GRID_ROWS * GRID_COLS][3] = {
    {1.0f, 0.0f, 0.0f},  /* (0,0) RED     */
    {0.0f, 1.0f, 0.0f},  /* (0,1) GREEN   */
    {0.0f, 0.0f, 1.0f},  /* (0,2) BLUE    */
    {1.0f, 1.0f, 1.0f},  /* (0,3) WHITE   */
    {1.0f, 1.0f, 0.0f},  /* (1,0) YELLOW  */
    {0.0f, 1.0f, 1.0f},  /* (1,1) CYAN    */
    {1.0f, 0.0f, 0.0f},  /* (1,2) RED     */
    {0.0f, 1.0f, 0.0f},  /* (1,3) GREEN   */
    {0.0f, 0.0f, 1.0f},  /* (2,0) BLUE    */
    {1.0f, 1.0f, 1.0f},  /* (2,1) WHITE   */
    {1.0f, 1.0f, 0.0f},  /* (2,2) YELLOW  */
    {0.0f, 1.0f, 1.0f},  /* (2,3) CYAN    */
};

/* FLAT-stripe non-provoking vertices carry opaque black so a
 * flat-shade-vertex-selection bug shows up as the cell rendering BLACK
 * instead of the expected color. */
#define DISTRACTOR_R 0.0f
#define DISTRACTOR_G 0.0f
#define DISTRACTOR_B 0.0f

#define CELL_Z 0.5f

/* Window (x_w, y_w) -> clip (cx, cy). Same formula as depth-floor. */
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

/* Emit one SMOOTH quad as 4 verts in OP_QUADS winding (TL, TR, BR, BL)
 * with every vertex carrying the cell's expected color. Smooth
 * interpolation across constant-color verts produces a uniform cell. */
static void emit_quad_smooth(ColoredVertex *out, int x0, int y0,
                             int x1, int y1, float r, float g, float b)
{
    mk_vert(out + 0, x0, y0, r, g, b);  /* TL */
    mk_vert(out + 1, x1, y0, r, g, b);  /* TR */
    mk_vert(out + 2, x1, y1, r, g, b);  /* BR */
    mk_vert(out + 3, x0, y1, r, g, b);  /* BL */
}

/* Emit one SMOOTH quad as 6 verts (two triangles) in the same A-B-C /
 * A-C-D diagonalization the native_quad path uses internally per
 * glsl/geom.c:176-178. All verts carry the same expected color. */
static void emit_quad_as_tris_smooth(ColoredVertex *out, int x0, int y0,
                                     int x1, int y1,
                                     float r, float g, float b)
{
    /* Triangle 1: TL, TR, BR (A, B, C). */
    mk_vert(out + 0, x0, y0, r, g, b);
    mk_vert(out + 1, x1, y0, r, g, b);
    mk_vert(out + 2, x1, y1, r, g, b);
    /* Triangle 2: TL, BR, BL (A, C, D). */
    mk_vert(out + 3, x0, y0, r, g, b);
    mk_vert(out + 4, x1, y1, r, g, b);
    mk_vert(out + 5, x0, y1, r, g, b);
}

/* Emit one FLAT quad as 6 verts (two triangles). With
 * FLAT_SHADE_OP=VERTEX_FIRST the provoking vertex of each triangle is
 * its vertex 0. We use the A-B-C / A-C-D diagonalization where vertex
 * 0 of each triangle is TL. Both tris get their expected color via
 * TL=EXPECTED; the other two vertices in each tri carry BLACK
 * distractor. */
static void emit_quad_as_tris_flat(ColoredVertex *out, int x0, int y0,
                                   int x1, int y1,
                                   float r, float g, float b)
{
    /* Triangle 1: TL=EXPECTED (provoking), TR=DISTRACTOR, BR=DISTRACTOR. */
    mk_vert(out + 0, x0, y0, r, g, b);
    mk_vert(out + 1, x1, y0,
            DISTRACTOR_R, DISTRACTOR_G, DISTRACTOR_B);
    mk_vert(out + 2, x1, y1,
            DISTRACTOR_R, DISTRACTOR_G, DISTRACTOR_B);
    /* Triangle 2: TL=EXPECTED (provoking), BR=DISTRACTOR, BL=DISTRACTOR. */
    mk_vert(out + 3, x0, y0, r, g, b);
    mk_vert(out + 4, x1, y1,
            DISTRACTOR_R, DISTRACTOR_G, DISTRACTOR_B);
    mk_vert(out + 5, x0, y1,
            DISTRACTOR_R, DISTRACTOR_G, DISTRACTOR_B);
}

/* Per-cell vertex buffers. Stored on .bss; copied into PAGE_WRITECOMBINE
 * GPU-readable memory in main() before the render loop. */
static ColoredVertex s_smooth_quad[QUAD_VERTS_TOTAL];
static ColoredVertex s_smooth_tri[SMOOTH_TRI_VERTS];
static ColoredVertex s_flat_tri[FLAT_TRI_VERTS];

/* PAGE_WRITECOMBINE-mapped copies (GPU-visible). */
static ColoredVertex *s_alloc_smooth_quad;
static ColoredVertex *s_alloc_smooth_tri;
static ColoredVertex *s_alloc_flat_tri;

static void build_geometry(void)
{
    int q;

    /* --- TOP HALF: OP_QUADS SMOOTH for all 12 cells --- */
    q = 0;
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = row * CELL_H;
            const int x1 = x0 + CELL_W;
            const int y1 = y0 + CELL_H;
            emit_quad_smooth(s_smooth_quad + q, x0, y0, x1, y1,
                             k_cell_rgb[idx][0],
                             k_cell_rgb[idx][1],
                             k_cell_rgb[idx][2]);
            q += QUAD_VERTS_PER_CELL;
        }
    }

    /* --- BOTTOM HALF: OP_TRIANGLES SMOOTH for rows 0-1 (8 cells) --- */
    q = 0;
    for (int row = 0; row < SMOOTH_TRI_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = HALF_H + row * CELL_H;
            const int x1 = x0 + CELL_W;
            const int y1 = y0 + CELL_H;
            emit_quad_as_tris_smooth(s_smooth_tri + q, x0, y0, x1, y1,
                                     k_cell_rgb[idx][0],
                                     k_cell_rgb[idx][1],
                                     k_cell_rgb[idx][2]);
            q += TRI_VERTS_PER_CELL;
        }
    }

    /* --- BOTTOM HALF: OP_TRIANGLES FLAT_FIRST for row 2 (4 cells) --- */
    q = 0;
    for (int row = SMOOTH_TRI_ROWS; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = HALF_H + row * CELL_H;
            const int x1 = x0 + CELL_W;
            const int y1 = y0 + CELL_H;
            emit_quad_as_tris_flat(s_flat_tri + q, x0, y0, x1, y1,
                                   k_cell_rgb[idx][0],
                                   k_cell_rgb[idx][1],
                                   k_cell_rgb[idx][2]);
            q += TRI_VERTS_PER_CELL;
        }
    }
}

/* Set common diag state we don't toggle between passes. */
static void enforce_common_state(void)
{
    uint32_t *p = pb_begin();
    /* Both faces FILL -- required for both native_quad and
     * native_tri_depth (glsl/geom.c:141-144 and 181-184). */
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    /* Depth test off -- both halves are at the same z=0.5 and don't
     * overlap in screen space, so depth-test ordering is irrelevant.
     * Removing the test variable means a NATIVE_TRI_DEPTH-only
     * regression that mis-computes per-fragment z still shows up via
     * the color path the two flags share. */
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    /* No blend, no alpha test, no stencil. xbed_set_default_render_state
     * already does this; re-asserted here to keep the test self-
     * documenting. */
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    pb_end(p);
}

static void set_shade_smooth(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADE_MODEL,
                 NV097_SET_SHADE_MODEL_SMOOTH);
    pb_end(p);
}

static void set_shade_flat_v_first(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADE_MODEL,
                 NV097_SET_SHADE_MODEL_FLAT);
    /* For OP_TRIANGLES under FLAT: VERTEX_FIRST picks vertex 0 of each
     * tri as the provoking vertex; matches native_tri_depth's
     * first_vertex_is_provoking branch (glsl/geom.c:156). */
    p = pb_push1(p, NV097_SET_FLAT_SHADE_OP,
                 NV097_SET_FLAT_SHADE_OP_VERTEX_FIRST);
    pb_end(p);
}

static void bind_attribs(const ColoredVertex *base)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &base[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &base[0].color[0]);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    /* Clear back buffer to opaque black so any uncovered pixel is
     * well-defined. 4 * 160 = 640 (x) and 3 * 80 = 240 (y per half)
     * align cleanly -- no uncovered pixels are expected, but defending
     * against geometry-emission off-by-one is cheap. */
    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();

    /* --- PASS 1: TOP HALF SMOOTH OP_QUADS (12 cells) --- */
    set_shade_smooth();
    bind_attribs(s_alloc_smooth_quad);
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_QUADS, 0, QUAD_VERTS_TOTAL);

    /* --- PASS 2: BOTTOM HALF SMOOTH OP_TRIANGLES (rows 0-1, 8 cells) --- */
    set_shade_smooth();
    bind_attribs(s_alloc_smooth_tri);
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, SMOOTH_TRI_VERTS);

    /* --- PASS 3: BOTTOM HALF FLAT OP_TRIANGLES (row 2, 4 cells) --- */
    set_shade_flat_v_first();
    bind_attribs(s_alloc_flat_tri);
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, FLAT_TRI_VERTS);
}

static ColoredVertex *alloc_vc(size_t bytes, const char *label)
{
    ColoredVertex *p = MmAllocateContiguousMemoryEx(
        bytes, 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!p) {
        debugPrint("MmAllocateContiguousMemoryEx (%s) failed\n", label);
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
    }
    return p;
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("native-quad-tri-depth v0.3 (no FLAT-quad stripe)\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    build_geometry();

    s_alloc_smooth_quad = alloc_vc(sizeof(s_smooth_quad), "smooth_quad");
    if (!s_alloc_smooth_quad) return 1;
    memcpy(s_alloc_smooth_quad, s_smooth_quad, sizeof(s_smooth_quad));

    s_alloc_smooth_tri = alloc_vc(sizeof(s_smooth_tri), "smooth_tri");
    if (!s_alloc_smooth_tri) return 1;
    memcpy(s_alloc_smooth_tri, s_smooth_tri, sizeof(s_smooth_tri));

    s_alloc_flat_tri = alloc_vc(sizeof(s_flat_tri), "flat_tri");
    if (!s_alloc_flat_tri) return 1;
    memcpy(s_alloc_flat_tri, s_flat_tri, sizeof(s_flat_tri));

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\native-quad-tri-depth-capture.bin",
        "D:\\native-quad-tri-depth-done.txt",
        "native-quad-tri-depth");
    return 0;
}
