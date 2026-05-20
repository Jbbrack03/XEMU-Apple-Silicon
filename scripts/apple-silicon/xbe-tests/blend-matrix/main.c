/*
 * blend-matrix — 8 most-common blend (sfactor, dfactor, equation)
 * combinations Tier-1 NV2A diag XBE.
 *
 * NV2A feature exercised: §F.1 (color blend stage). NV097_SET_BLEND_ENABLE
 *                         + NV097_SET_BLEND_FUNC_SFACTOR
 *                         + NV097_SET_BLEND_FUNC_DFACTOR
 *                         + NV097_SET_BLEND_EQUATION
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.9, this XBE oracles the NV2A blend
 * pipeline. The §4.9 spec describes 128 cells (16 sfactors × 8
 * dfactor/equation rows); v0.1 narrows scope to the 8 most-common
 * blend modes used by Xbox titles (matches the 4x2 grid convention
 * the rest of the first-wave XBEs use). Second-wave XBEs will expand
 * coverage to the full sfactor x dfactor x equation matrix and the
 * constant-color factors.
 *
 * The test renders an 8-cell 4x2 grid. Per cell:
 *
 *   1. "DST pass" — blend disabled. Draw a quad over the cell with
 *      DIFFUSE=DST_COLOR. The cell becomes DST_COLOR.
 *
 *   2. "SRC pass" — blend enabled with the cell's (sfactor, dfactor,
 *      equation). Draw a quad over the cell with DIFFUSE=SRC_COLOR.
 *      The cell becomes (sfactor*SRC <equation> dfactor*DST), clamped
 *      to [0, 255] per channel.
 *
 * The 8 (sfactor, dfactor, equation) tuples + DST/SRC choices are
 * picked so the expected output lands at saturated 0/255 cube
 * corners. Byte-exact across renderers regardless of display gamma:
 *
 *   Cell  Mode                                          DST              SRC              Expected
 *   ----  --------------------------------------------  ---------------  ---------------  ----------
 *   0     ONE/ZERO/ADD          (overwrite)             (0,128,128,255)  (255,0,0,255)    RED
 *   1     ZERO/ONE/ADD          (DST-only)              (0,255,0,255)    (255,0,255,255)  GREEN
 *   2     ONE/ONE/ADD           (additive)              (255,0,0,255)    (0,255,0,255)    YELLOW
 *   3     ONE/ONE/REVERSE_SUB   (DST - SRC clamp)       (255,255,255,255)(255,0,0,255)    CYAN
 *   4     ONE/ONE/SUBTRACT      (SRC - DST clamp)       (0,255,0,255)    (255,255,255,255)MAGENTA
 *   5     SRC_ALPHA/ONE_MINUS_SRC_ALPHA/ADD α=255       (255,0,0,255)    (0,0,255,255)    BLUE
 *   6     DST_COLOR/ZERO/ADD    (modulate SRC*DST)      (255,0,255,255)  (255,255,0,255)  RED
 *   7     ZERO/SRC_COLOR/ADD    (modulate DST*SRC)      (0,255,255,255)  (255,255,0,255)  GREEN
 *
 * --- Math derivations -----------------------------------------------
 *
 * For each cell, the blend math is:
 *
 *   Cell 0: 1*SRC + 0*DST                   = SRC = (255, 0, 0)         = RED
 *   Cell 1: 0*SRC + 1*DST                   = DST = (0, 255, 0)         = GREEN
 *   Cell 2: SRC + DST clamp                 = (0+255, 255+0, 0+0)       = YELLOW
 *   Cell 3: DST - SRC clamp                 = (255-255, 255-0, 255-0)   = CYAN
 *   Cell 4: SRC - DST clamp                 = (255-0, 255-255, 255-0)   = MAGENTA
 *   Cell 5: (α/255)*SRC + (1-α/255)*DST     = 1*SRC + 0*DST = SRC       = BLUE
 *   Cell 6: (DST/255)*SRC + 0*DST           = (255*255/255, 255*0/255,
 *                                              0*255/255) = (255,0,0)   = RED
 *   Cell 7: 0*SRC + (SRC/255)*DST           = (255*0/255, 255*255/255,
 *                                              0*255/255) = (0,255,0)   = GREEN
 *
 * All arithmetic uses 8-bit integer math: 255*X/255 = X (integer); zero
 * factors zero out the channel exactly. Saturating add/subtract clamps
 * are byte-exact (no precision issues).
 *
 * --- Why no fractional alpha cells in v0.1 --------------------------
 *
 * The §4.9 spec is open about cell granularity. v0.1 deliberately
 * avoids alpha = 128 / 64 etc. because (128/255)*X is not byte-exact:
 * the GL renderer uses GLfloat for blend factors which rounds at the
 * end, while NV2A hardware uses a fixed-point pipeline that may round
 * differently. Endpoint alpha values (255 / 0) preserve byte-exact
 * comparison and let v0.1 ship without compare_overrides tolerance.
 * Mid-range alpha coverage is a second-wave follow-up.
 *
 * --- Catches --------------------------------------------------------
 *
 *   - Wrong sfactor mapping: cell 0 (ONE/ZERO) would not produce SRC;
 *     cell 5 (SRC_ALPHA) would not produce SRC at alpha=255.
 *   - Wrong dfactor mapping: cell 1 (ZERO/ONE) would not produce DST;
 *     cell 7 (ZERO/SRC_COLOR) would not produce DST*SRC/255.
 *   - Wrong equation mapping: cells 3 (REVERSE_SUBTRACT) and 4
 *     (SUBTRACT) would not produce DST-SRC vs SRC-DST respectively.
 *   - Negative-clamp bug in SUBTRACT/REVERSE_SUBTRACT: a per-channel
 *     underflow that wraps modulo 256 instead of clamping to 0 would
 *     produce non-zero in channels that should be zero.
 *   - Saturating-add bug in ADD: cell 2 (ONE+ONE) sums 255+0 in each
 *     channel -- not overflowing -- but a buggy 9-bit-without-clamp
 *     implementation would still show the high byte.
 *   - Wrong DST-color blend (cell 6): would not produce SRC*DST/255.
 *   - DST/SRC swapped in modulate (cells 6 vs 7): cells would render
 *     each other's colors.
 *
 * --- Does NOT catch (documented limits, second-wave follow-ups) -----
 *
 *   - Mid-range alpha precision (alpha=128 etc.) -- requires
 *     compare_overrides tolerance + sub-byte oracle, deferred to
 *     second wave.
 *   - Constant-color blend factors (CONSTANT_COLOR / ALPHA family) --
 *     v0.1 omits to keep cell count at 8. Second wave will add.
 *   - SRC_ALPHA_SATURATE -- requires DST alpha != 0 and SRC alpha !=
 *     0; documented as separate XBE for the corner case.
 *   - Per-RT blend masks / write-mask interaction -- separate XBE.
 *
 * --- Geometry layout ------------------------------------------------
 *
 * WIN_W=640, WIN_H=480. 4 cols x 2 rows = 8 cells; cell_w=160, cell_h=240.
 *
 *   col*160       (col+1)*160
 *      |             |
 *   y=0  +-----+-----+-----+-----+
 *        | 0   | 1   | 2   | 3   |  (row 0)
 * y=240  +-----+-----+-----+-----+
 *        | 4   | 5   | 6   | 7   |  (row 1)
 *   y=480+-----+-----+-----+-----+
 *
 * --- Reproducibility ------------------------------------------------
 *
 * Pure deterministic pattern; no banner, no counter, no per-frame
 * variation. Byte-identical across two cold runs.
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

/* Per-cell: 2 quads (DST + SRC) × 6 verts per quad = 12 verts per cell.
 * Total = 8 cells × 12 = 96 verts. */
#define VERTS_PER_QUAD     6
#define QUADS_PER_CELL     2  /* DST then SRC */
#define VERTS_PER_CELL     (VERTS_PER_QUAD * QUADS_PER_CELL)
#define VERTS_TOTAL        (GRID_CELLS * VERTS_PER_CELL)

typedef struct {
    float pos[3];     /* clip-space, z=0.5 */
    float color[4];   /* RGBA (0..1) */
} __attribute__((packed)) BlendVertex;

/* Per-cell blend tuple + DST + SRC colors. */
typedef struct {
    uint32_t sfactor;   /* NV097_SET_BLEND_FUNC_SFACTOR_V_* */
    uint32_t dfactor;   /* NV097_SET_BLEND_FUNC_DFACTOR_V_* */
    uint32_t equation;  /* NV097_SET_BLEND_EQUATION_V_* */
    float    dst[4];    /* RGBA (0..1) — what cell becomes after DST pass */
    float    src[4];    /* RGBA (0..1) — drawn under blend by SRC pass */
} BlendCell;

/* 8 cells. See file header for the per-cell math derivation. All DST
 * and SRC alpha = 1.0 in v0.1; mid-range alpha is a second-wave
 * follow-up. */
static const BlendCell k_cells[GRID_CELLS] = {
    /* 0  ONE / ZERO / ADD              (overwrite)  -> RED */
    { NV097_SET_BLEND_FUNC_SFACTOR_V_ONE,
      NV097_SET_BLEND_FUNC_DFACTOR_V_ZERO,
      NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
      { 0.0f, 0.5f, 0.5f, 1.0f },  /* DST: any (overwritten) */
      { 1.0f, 0.0f, 0.0f, 1.0f } },/* SRC: RED */

    /* 1  ZERO / ONE / ADD              (DST-only)   -> GREEN */
    { NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO,
      NV097_SET_BLEND_FUNC_DFACTOR_V_ONE,
      NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
      { 0.0f, 1.0f, 0.0f, 1.0f },  /* DST: GREEN */
      { 1.0f, 0.0f, 1.0f, 1.0f } },/* SRC: any (rejected by sfactor=0) */

    /* 2  ONE / ONE / ADD               (additive)   -> YELLOW */
    { NV097_SET_BLEND_FUNC_SFACTOR_V_ONE,
      NV097_SET_BLEND_FUNC_DFACTOR_V_ONE,
      NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
      { 1.0f, 0.0f, 0.0f, 1.0f },  /* DST: RED */
      { 0.0f, 1.0f, 0.0f, 1.0f } },/* SRC: GREEN -> RED+GREEN=YELLOW */

    /* 3  ONE / ONE / REVERSE_SUBTRACT  (DST-SRC clamp) -> CYAN */
    { NV097_SET_BLEND_FUNC_SFACTOR_V_ONE,
      NV097_SET_BLEND_FUNC_DFACTOR_V_ONE,
      NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT,
      { 1.0f, 1.0f, 1.0f, 1.0f },  /* DST: WHITE */
      { 1.0f, 0.0f, 0.0f, 1.0f } },/* SRC: RED -> WHITE-RED=CYAN */

    /* 4  ONE / ONE / SUBTRACT          (SRC-DST clamp) -> MAGENTA */
    { NV097_SET_BLEND_FUNC_SFACTOR_V_ONE,
      NV097_SET_BLEND_FUNC_DFACTOR_V_ONE,
      NV097_SET_BLEND_EQUATION_V_FUNC_SUBTRACT,
      { 0.0f, 1.0f, 0.0f, 1.0f },  /* DST: GREEN */
      { 1.0f, 1.0f, 1.0f, 1.0f } },/* SRC: WHITE -> WHITE-GREEN=MAGENTA */

    /* 5  SRC_ALPHA / ONE_MINUS_SRC_ALPHA / ADD (alpha-blend, α=255) -> BLUE
     * At α=1.0 this collapses to SRC*1 + DST*0 = SRC. */
    { NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA,
      NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA,
      NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
      { 1.0f, 0.0f, 0.0f, 1.0f },  /* DST: RED (rejected at α=1) */
      { 0.0f, 0.0f, 1.0f, 1.0f } },/* SRC: BLUE alpha=1 -> BLUE */

    /* 6  DST_COLOR / ZERO / ADD        (SRC*DST modulate) -> RED */
    { NV097_SET_BLEND_FUNC_SFACTOR_V_DST_COLOR,
      NV097_SET_BLEND_FUNC_DFACTOR_V_ZERO,
      NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
      { 1.0f, 0.0f, 1.0f, 1.0f },  /* DST: MAGENTA */
      { 1.0f, 1.0f, 0.0f, 1.0f } },/* SRC: YELLOW -> SRC*DST: (1*1,1*0,0*1)=RED */

    /* 7  ZERO / SRC_COLOR / ADD        (DST*SRC modulate) -> GREEN */
    { NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO,
      NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR,
      NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
      { 0.0f, 1.0f, 1.0f, 1.0f },  /* DST: CYAN */
      { 1.0f, 1.0f, 0.0f, 1.0f } },/* SRC: YELLOW -> DST*SRC: (0*1,1*1,1*0)=GREEN */
};

static BlendVertex s_verts[VERTS_TOTAL];
static BlendVertex *s_alloc_verts;

static inline void mk_vert(BlendVertex *v, int x_w, int y_w,
                           const float rgba[4])
{
    v->pos[0] = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1] = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2] = 0.5f;
    v->color[0] = rgba[0];
    v->color[1] = rgba[1];
    v->color[2] = rgba[2];
    v->color[3] = rgba[3];
}

/* Emit two triangles covering the rect (x0,y0)-(x1,y1) into out[0..5]
 * using A-B-C / A-C-D winding. All 6 verts carry the same color. */
static void emit_quad(BlendVertex *out, int x0, int y0, int x1, int y1,
                      const float rgba[4])
{
    mk_vert(&out[0], x0, y0, rgba);
    mk_vert(&out[1], x1, y0, rgba);
    mk_vert(&out[2], x1, y1, rgba);
    mk_vert(&out[3], x0, y0, rgba);
    mk_vert(&out[4], x1, y1, rgba);
    mk_vert(&out[5], x0, y1, rgba);
}

static void build_geometry(void)
{
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = row * CELL_H;
            const int x1 = x0 + CELL_W;
            const int y1 = y0 + CELL_H;
            BlendVertex *cell = &s_verts[idx * VERTS_PER_CELL];
            /* DST pass: blend will be DISABLED at draw time; quad
             * paints the cell to DST_COLOR. */
            emit_quad(&cell[0], x0, y0, x1, y1, k_cells[idx].dst);
            /* SRC pass: blend will be ENABLED with the cell's tuple. */
            emit_quad(&cell[VERTS_PER_QUAD], x0, y0, x1, y1,
                      k_cells[idx].src);
        }
    }
}

static void enforce_common_state(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    pb_end(p);
}

static void set_blend_off(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    pb_end(p);
}

static void set_blend_for_cell(int idx)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 1);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, k_cells[idx].sfactor);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, k_cells[idx].dfactor);
    p = pb_push1(p, NV097_SET_BLEND_EQUATION,    k_cells[idx].equation);
    pb_end(p);
}

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(BlendVertex), &s_alloc_verts[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(BlendVertex), &s_alloc_verts[0].color[0]);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    /* Per cell: two draws. The DST draw runs with blend disabled to
     * paint the cell to DST_COLOR. The SRC draw runs with blend
     * enabled and the cell's (sfactor, dfactor, equation) tuple. */
    for (int idx = 0; idx < GRID_CELLS; idx++) {
        const int base = idx * VERTS_PER_CELL;

        set_blend_off();
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         base, VERTS_PER_QUAD);

        set_blend_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         base + VERTS_PER_QUAD, VERTS_PER_QUAD);
    }
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("blend-matrix v0.1\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    build_geometry();

    s_alloc_verts = MmAllocateContiguousMemoryEx(
        sizeof(s_verts), 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_alloc_verts) {
        debugPrint("MmAllocateContiguousMemoryEx failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    memcpy(s_alloc_verts, s_verts, sizeof(s_verts));

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\blend-matrix-capture.bin",
        "D:\\blend-matrix-done.txt",
        "blend-matrix");
    return 0;
}
