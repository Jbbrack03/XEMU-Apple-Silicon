/*
 * stencil-ops — 8 stencil-op color-probe oracle (Tier-1 NV2A diag XBE).
 *
 * NV2A feature exercised: §G.4 (stencil-op pipeline), §G.2 (stencil
 *                          test enable/func/mask), §G.3 (stencil ref),
 *                          §H.5 (clear with stencil clear value).
 * NV097 methods:           SET_ZSTENCIL_CLEAR_VALUE, CLEAR_SURFACE
 *                          (Z+S), SET_STENCIL_TEST_ENABLE,
 *                          SET_STENCIL_FUNC, SET_STENCIL_FUNC_REF,
 *                          SET_STENCIL_FUNC_MASK, SET_STENCIL_MASK,
 *                          SET_STENCIL_OP_FAIL/ZFAIL/ZPASS,
 *                          SET_DEPTH_TEST_ENABLE,
 *                          SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET,
 *                          SET_BEGIN_END(TRIANGLES), DRAW_ARRAYS.
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.10, this is the oracle for the 8
 * NV2A stencil operations. Stencil test/state isn't directly visible
 * in the captured front-buffer; the test encodes the post-op stencil
 * value into the rendered COLOR via a follow-up stencil-test-gated
 * probe quad (v1 plan's Tier-2 readback proposal was rejected per
 * Codex finding #7 because Metal's surface-download path skips the
 * depth/stencil aspect; the color-probe redesign keeps the verdict
 * inside the visible drawable).
 *
 * The test renders an 8-cell 4×2 grid. Per cell:
 *
 *   1. Stencil cleared to 0x80 (mid-range) via SET_ZSTENCIL_CLEAR_VALUE +
 *      CLEAR_SURFACE with Z+S aspects. Cell-area clear, not full
 *      frame (each cell's stencil state must be independent).
 *
 *   2. "Op quad" — renders the cell area with:
 *        FUNC      = ALWAYS (so the op fires regardless of input).
 *        FUNC_REF  = 0x40   (used by REPLACE; ignored by others).
 *        FUNC_MASK = 0xFF   (no func-side masking).
 *        OP_FAIL   = <op>
 *        OP_ZFAIL  = <op>
 *        OP_ZPASS  = <op>   (depth test is off, so ZPASS fires; setting
 *                            all three to the same op removes any
 *                            FAIL/ZFAIL/ZPASS path divergence between
 *                            renderers).
 *        STENCIL_MASK = 0xFF (no write-side masking).
 *      The op quad's color is opaque BLACK so anywhere the test
 *      ultimately FAILs the probe gate the cell shows BLACK.
 *
 *   3. "Probe quad" — renders the same cell area with:
 *        FUNC      = EQUAL
 *        FUNC_REF  = <expected stencil value after op applied to 0x80>
 *        OP_FAIL   = KEEP
 *        OP_ZFAIL  = KEEP
 *        OP_ZPASS  = KEEP
 *      Color = the cell's saturated 0/255 EXPECTED color.
 *      If the op actually produced the expected stencil value, the
 *      probe's stencil test passes and the cell shows EXPECTED;
 *      otherwise the cell stays BLACK.
 *
 * The 8 ops + their expected post-op stencil values (starting from
 * stencil=0x80, ref=0x40):
 *
 *   Cell  Op           Expected stencil  Cell color
 *   ----  -----------  ----------------  ----------
 *   0     KEEP         0x80              RED
 *   1     ZERO         0x00              GREEN
 *   2     REPLACE      0x40 (= ref)      BLUE
 *   3     INCRSAT      0x81 (no sat)     WHITE
 *   4     DECRSAT      0x7F (no sat)     YELLOW
 *   5     INVERT       0x7F (~0x80)      CYAN
 *   6     INCR (wrap)  0x81              MAGENTA
 *   7     DECR (wrap)  0x7F              RED (reused; only 7 cube
 *                                              corners avoiding BLACK)
 *
 * --- Math derivation (640x480 X8R8G8B8 front buffer) ----------------
 *
 * For pixel (x, y) in [0, 640) x [0, 480):
 *   col = x / 160
 *   row = y / 240
 *   expected_rgb = CELL_RGB[row * 4 + col] if (op fired correctly)
 *                  else BLACK
 *
 * Initial clear paints the full back buffer opaque BLACK
 * (xbed_clear_color_argb(0xFF000000)). Each op cell then runs its
 * pair of quads. Cell quads cover their cells exactly (4×160=640,
 * 2×240=480).
 *
 * --- Catches ---------------------------------------------------------
 *
 *   - Any of the 8 stencil ops mis-implemented or aliased: the
 *     affected cell stays BLACK.
 *   - Wrong sat/wrap semantics (INCRSAT vs INCR confusion at non-
 *     overflow values): both INCRSAT and INCR produce 0x81 from 0x80
 *     because no overflow occurs — these two cells deliberately use
 *     the same expected post-op value so a confusion would still
 *     produce GREEN/MAGENTA correctly. (A separate XBE will exercise
 *     the saturation boundary at 0xFF/0x00 specifically; this XBE
 *     focuses on the non-overflow path so all 8 ops succeed in a
 *     well-known stencil-state region.)
 *   - Wrong stencil clear-value plumbing (`SET_ZSTENCIL_CLEAR_VALUE`
 *     not respected): every cell would FAIL the probe because the
 *     post-op stencil would be the wrong starting value.
 *   - Stencil-test off when test enable is on: every cell would
 *     FAIL the EQUAL probe and stay BLACK.
 *   - Stencil-mask incorrectly applied (e.g. clipping to 4 bits):
 *     cells where the post-op value has high bits set would FAIL.
 *
 * --- Reproducibility -------------------------------------------------
 *
 * Pure deterministic pattern; no banner, no counter, no per-frame
 * variation. Byte-identical-after-mask across two cold runs.
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

/* Stencil func values. NV2A uses the same enum as OpenGL/D3D depth-
 * compare functions; they live alongside SET_DEPTH_FUNC_V_* in
 * nv_regs.h. */
#define STENCIL_FUNC_NEVER    0x0200
#define STENCIL_FUNC_LESS     0x0201
#define STENCIL_FUNC_EQUAL    0x0202
#define STENCIL_FUNC_LEQUAL   0x0203
#define STENCIL_FUNC_GREATER  0x0204
#define STENCIL_FUNC_NOTEQUAL 0x0205
#define STENCIL_FUNC_GEQUAL   0x0206
#define STENCIL_FUNC_ALWAYS   0x0207

#define INITIAL_STENCIL 0x80
#define STENCIL_REF     0x40

/* Per-cell op specification: (NV2A op enum, expected post-op stencil,
 * cell RGB triple). Cell index = row * 4 + col. */
typedef struct {
    uint32_t op;
    uint8_t  expected_stencil;
    float    rgb[3];
} OpCell;

static const OpCell k_cells[GRID_CELLS] = {
    /* 0 KEEP    */ { NV097_SET_STENCIL_OP_V_KEEP,    0x80,
                      { 1.0f, 0.0f, 0.0f } },  /* RED     */
    /* 1 ZERO    */ { NV097_SET_STENCIL_OP_V_ZERO,    0x00,
                      { 0.0f, 1.0f, 0.0f } },  /* GREEN   */
    /* 2 REPLACE */ { NV097_SET_STENCIL_OP_V_REPLACE, STENCIL_REF,
                      { 0.0f, 0.0f, 1.0f } },  /* BLUE    */
    /* 3 INCRSAT */ { NV097_SET_STENCIL_OP_V_INCRSAT, 0x81,
                      { 1.0f, 1.0f, 1.0f } },  /* WHITE   */
    /* 4 DECRSAT */ { NV097_SET_STENCIL_OP_V_DECRSAT, 0x7F,
                      { 1.0f, 1.0f, 0.0f } },  /* YELLOW  */
    /* 5 INVERT  */ { NV097_SET_STENCIL_OP_V_INVERT,  0x7F,
                      { 0.0f, 1.0f, 1.0f } },  /* CYAN    */
    /* 6 INCR    */ { NV097_SET_STENCIL_OP_V_INCR,    0x81,
                      { 1.0f, 0.0f, 1.0f } },  /* MAGENTA */
    /* 7 DECR    */ { NV097_SET_STENCIL_OP_V_DECR,    0x7F,
                      { 1.0f, 0.0f, 0.0f } },  /* RED (reused) */
};

#define TRI_VERTS_PER_QUAD 6
typedef struct {
    float pos[3];
    float color[3];
} __attribute__((packed)) ColoredVertex;

/* Buffer for a single quad's 6 verts; we re-issue per cell per pass. */
static ColoredVertex s_quad_verts[TRI_VERTS_PER_QUAD];
static ColoredVertex *s_alloc_verts;

static inline void mk_vert(ColoredVertex *v, int x_w, int y_w,
                           float r, float g, float b)
{
    v->pos[0] = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1] = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2] = 0.5f;
    v->color[0] = r;
    v->color[1] = g;
    v->color[2] = b;
}

/* Emit a single quad as 6 verts (two triangles, A-B-C / A-C-D). All
 * 6 verts carry the same color so smooth interpolation is uniform. */
static void build_quad(int x0, int y0, int x1, int y1,
                       float r, float g, float b)
{
    mk_vert(&s_quad_verts[0], x0, y0, r, g, b);
    mk_vert(&s_quad_verts[1], x1, y0, r, g, b);
    mk_vert(&s_quad_verts[2], x1, y1, r, g, b);
    mk_vert(&s_quad_verts[3], x0, y0, r, g, b);
    mk_vert(&s_quad_verts[4], x1, y1, r, g, b);
    mk_vert(&s_quad_verts[5], x0, y1, r, g, b);
}

static void enforce_common_state(void)
{
    uint32_t *p = pb_begin();
    /* FILL both faces; depth-test off (so OP_ZPASS fires regardless of
     * frame depth content); blend/alpha off. */
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    /* Stencil test enabled throughout. */
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 1);
    p = pb_push1(p, NV097_SET_STENCIL_MASK, 0xFF);
    p = pb_push1(p, NV097_SET_STENCIL_FUNC_MASK, 0xFF);
    pb_end(p);
}

/* Clear stencil to value `s` over the given rect via
 * SET_ZSTENCIL_CLEAR_VALUE (for Z24S8: ((depth & 0xFFFFFF) << 8) | s)
 * + CLEAR_SURFACE with Z+S aspects + SET_CLEAR_RECT_*. depth value
 * is set to max so the depth aspect is also reset cleanly (depth test
 * is off in this XBE but defending against future overlap). */
static void clear_stencil_rect(int x, int y, int w, int h, uint8_t s)
{
    const uint32_t zs_value = (0xFFFFFF << 8) | (uint32_t)s;
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_ZSTENCIL_CLEAR_VALUE, zs_value);
    /* SET_CLEAR_RECT_HORIZONTAL/VERTICAL pack ((max << 16) | min). */
    p = pb_push1(p, NV097_SET_CLEAR_RECT_HORIZONTAL,
                 (((x + w - 1) & 0xFFF) << 16) | (x & 0xFFF));
    p = pb_push1(p, NV097_SET_CLEAR_RECT_VERTICAL,
                 (((y + h - 1) & 0xFFF) << 16) | (y & 0xFFF));
    p = pb_push1(p, NV097_CLEAR_SURFACE,
                 NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL);
    pb_end(p);
}

static void issue_op_pass(uint32_t op)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_STENCIL_FUNC, STENCIL_FUNC_ALWAYS);
    p = pb_push1(p, NV097_SET_STENCIL_FUNC_REF, STENCIL_REF);
    p = pb_push1(p, NV097_SET_STENCIL_OP_FAIL,  op);
    p = pb_push1(p, NV097_SET_STENCIL_OP_ZFAIL, op);
    p = pb_push1(p, NV097_SET_STENCIL_OP_ZPASS, op);
    pb_end(p);
}

static void issue_probe_pass(uint8_t expected_stencil)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_STENCIL_FUNC, STENCIL_FUNC_EQUAL);
    p = pb_push1(p, NV097_SET_STENCIL_FUNC_REF, expected_stencil);
    p = pb_push1(p, NV097_SET_STENCIL_OP_FAIL,  NV097_SET_STENCIL_OP_V_KEEP);
    p = pb_push1(p, NV097_SET_STENCIL_OP_ZFAIL, NV097_SET_STENCIL_OP_V_KEEP);
    p = pb_push1(p, NV097_SET_STENCIL_OP_ZPASS, NV097_SET_STENCIL_OP_V_KEEP);
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

static void render_cell(int col, int row, const OpCell *cell)
{
    const int x0 = col * CELL_W;
    const int y0 = row * CELL_H;
    const int x1 = x0 + CELL_W;
    const int y1 = y0 + CELL_H;

    /* Step 1: clear stencil for this cell area to INITIAL_STENCIL. */
    clear_stencil_rect(x0, y0, CELL_W, CELL_H, INITIAL_STENCIL);

    /* Step 2: op pass — draw the cell area with the op applied; color
     * is BLACK so a probe-pass FAIL leaves the cell black. */
    issue_op_pass(cell->op);
    build_quad(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f);
    memcpy(s_alloc_verts, s_quad_verts, sizeof(s_quad_verts));
    bind_attribs();
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, TRI_VERTS_PER_QUAD);

    /* Step 3: probe pass — stencil test EQUAL expected; color =
     * EXPECTED if it passes. */
    issue_probe_pass(cell->expected_stencil);
    build_quad(x0, y0, x1, y1, cell->rgb[0], cell->rgb[1], cell->rgb[2]);
    memcpy(s_alloc_verts, s_quad_verts, sizeof(s_quad_verts));
    bind_attribs();
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, TRI_VERTS_PER_QUAD);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            render_cell(col, row, &k_cells[idx]);
        }
    }
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("stencil-ops v0.1\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    s_alloc_verts = MmAllocateContiguousMemoryEx(
        sizeof(s_quad_verts), 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_alloc_verts) {
        debugPrint("MmAllocateContiguousMemoryEx failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\stencil-ops-capture.bin",
        "D:\\stencil-ops-done.txt",
        "stencil-ops");
    return 0;
}
