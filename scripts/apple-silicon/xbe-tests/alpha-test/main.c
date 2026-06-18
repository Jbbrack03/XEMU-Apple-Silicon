/*
 * alpha-test — 8 NV2A alpha-test (func, ref) combinations Tier-1 NV2A
 * diag XBE.
 *
 * NV2A feature exercised: §F.2 (alpha test stage).
 *                         NV097_SET_ALPHA_TEST_ENABLE (0x0300)
 *                         + NV097_SET_ALPHA_FUNC      (0x033C)
 *                         + NV097_SET_ALPHA_REF       (0x0340)
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- Method-address + enum verification (rule #1, no guessing) -------
 *
 * Verified against the in-tree NV2A register header
 * `hw/xbox/nv2a/nv2a_regs.h`:
 *   NV097_SET_ALPHA_TEST_ENABLE = 0x00000300  (line 936)
 *   NV097_SET_ALPHA_FUNC        = 0x0000033C  (line 958)
 *   NV097_SET_ALPHA_REF         = 0x00000340  (line 959)
 *
 * The ALPHA_FUNC parameter is the *raw 0-indexed* PshAlphaFunc enum.
 * `pgraph.c:DEF_METHOD(NV097, SET_ALPHA_FUNC)` writes (parameter & 0xF)
 * straight into NV_PGRAPH_CONTROL_0_ALPHAFUNC with NO value remap
 * (unlike the blend-factor methods, which switch-map SDK V_* tokens).
 * `glsl/psh.c:99` then reads that field back verbatim as
 * `enum PshAlphaFunc`. The enum is defined 0-indexed in
 * `hw/xbox/nv2a/pgraph/psh_regs.h:162`:
 *
 *   NEVER=0, LESS=1, EQUAL=2, LEQUAL=3, GREATER=4,
 *   NOTEQUAL=5, GEQUAL=6, ALWAYS=7
 *
 * There is therefore NO NV097_SET_ALPHA_FUNC_V_* SDK token in the
 * header (grep confirms none); we push the raw integer index directly.
 * NV097_SET_ALPHA_REF takes the raw 8-bit reference value
 * (NV_PGRAPH_CONTROL_0_ALPHAREF mask = 0x000000FF).
 *
 * --- Renderer comparison semantics (verified) -----------------------
 *
 * `glsl/psh.c:1510-1530` emits the alpha-test discard:
 *   - alpha_func == ALWAYS  -> no test emitted (always passes)
 *   - alpha_func == NEVER   -> unconditional `discard;`
 *   - otherwise:
 *       int fragAlpha = int(round(fragColor.a * 255.0));
 *       if (!(fragAlpha <OP> alphaRef)) discard;
 *     where <OP> is the C operator for the func and `alphaRef` is the
 *     raw 8-bit ALPHAREF value (psh.c:1700-1704, NOT normalized to
 *     [0,1]). So the comparison is integer fragAlpha <OP> ref. This is
 *     exactly the NV2A alpha-test semantics: the incoming fragment
 *     alpha (0..255) is compared to the 8-bit reference with the
 *     selected function; fail -> fragment discarded (no framebuffer
 *     write, so the underlying DST color survives).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2, this XBE oracles the NV2A alpha-test
 * ROP gate. The test renders an 8-cell 4x2 grid. Per cell:
 *
 *   1. "BG pass" — alpha test DISABLED. Draw a BLUE quad
 *      (0xFF0000FF; opaque blue) over the cell. The cell becomes BLUE.
 *
 *   2. "FG pass" — alpha test ENABLED with the cell's (func, ref).
 *      Draw a RED quad (0xFF0000RR base color RED, opaque RGB) with a
 *      chosen per-cell alpha A over the cell. If the alpha test PASSES
 *      the RED quad is written -> cell becomes RED. If it FAILS the
 *      RED quad is discarded -> cell stays BLUE.
 *
 * Blend / depth / stencil stay OFF (xbed_set_default_render_state), so
 * alpha test is the ONLY variable. The foreground RGB is opaque RED;
 * only its ALPHA channel drives the test. Captured cell color is RED
 * (pass) or BLUE (discard).
 *
 * ref = 0x80 (128) for every cell. Expected = pass->RED, discard->BLUE:
 *
 *   Cell  Func          A(alpha)  Comparison (fragAlpha OP ref)  Result
 *   ----  ------------  --------  -----------------------------  ------
 *   0     NEVER  (0)    0xFF=255  unconditional discard          BLUE
 *   1     ALWAYS (7)    0x00=0    no test (always pass)          RED
 *   2     LESS   (1)    0x40=64   64  < 128  = true              RED
 *   3     LESS   (1)    0xC0=192  192 < 128  = false             BLUE
 *   4     GEQUAL (6)    0x80=128  128 >= 128 = true              RED
 *   5     GREATER(4)    0x80=128  128 >  128 = false             BLUE
 *   6     EQUAL  (2)    0x80=128  128 == 128 = true              RED
 *   7     NOTEQUAL(5)   0x80=128  128 != 128 = false             BLUE
 *
 * --- Why these cells -------------------------------------------------
 *
 *   - Cells 0/1 anchor the two special-cased funcs (NEVER discard,
 *     ALWAYS no-op) which the renderer handles outside the operator
 *     switch.
 *   - Cells 2/3 are the clearly-above / clearly-below LESS pair: they
 *     verify the func decodes to "<" and that the test direction is
 *     fragAlpha-relative-to-ref (not ref-relative-to-fragAlpha).
 *   - Cells 4-7 are the A == ref boundary discriminators. With A == ref
 *     == 128, GEQUAL passes (>= true), GREATER fails (> false),
 *     EQUAL passes (== true), NOTEQUAL fails (!= false). These catch
 *     the classic off-by-ones: >= vs > and == vs != confusion, AND a
 *     wrong 0-indexed func decode (e.g. an off-by-one in the enum
 *     mapping would swap a passing func for a failing neighbor).
 *
 * --- Float-alpha exactness ------------------------------------------
 *
 * Vertex alpha is uploaded as a float A/255.0. The renderer recovers
 * the integer fragment alpha via round(fragColor.a * 255.0). For
 * A in {0, 64, 128, 192, 255}, A/255.0 round-trips exactly through
 * round(x*255.0) back to A (these are within 0.5 ULP of the intended
 * integer after multiply), so the integer comparison is byte-exact.
 * The diffuse alpha reaches fragColor.a unmodified through the default
 * passthrough PS (lib/ps.ps.cg returns I.color directly; alpha test is
 * applied to that fragment alpha). No mid-range fractional alpha is
 * used, so no compare tolerance is needed for the alpha math itself.
 *
 * --- Catches --------------------------------------------------------
 *
 *   - Wrong/absent alpha-test enable plumbing: every "discard" cell
 *     (0,3,5,7) would wrongly show RED.
 *   - 0-indexed func mis-decode: any off-by-one in the func->operator
 *     map flips at least one of the boundary cells 4-7.
 *   - >= vs > swap: cells 4 (GEQUAL pass) and 5 (GREATER fail) flip.
 *   - == vs != swap: cells 6 (EQUAL pass) and 7 (NOTEQUAL fail) flip.
 *   - ref normalization bug (treating ref as [0,1] float, or
 *     /255 vs *255): boundary cells 4-7 mis-resolve at the A==ref edge.
 *   - Inverted discard sense (discard-on-pass): every cell flips.
 *
 * --- Does NOT catch (documented limits, second-wave follow-ups) -----
 *
 *   - LEQUAL (3): omitted to keep the cell count at 8; LESS + GEQUAL +
 *     GREATER already bracket the ordered comparisons. A second-wave
 *     variant can add LEQUAL and additional ref values.
 *   - Non-128 ref sweep and mid-range fractional alpha (fixed-point
 *     rounding at A != {0,64,128,192,255}).
 *   - Alpha-test interaction with blend / multisample alpha-to-coverage
 *     — separate XBEs.
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
 * WRITE-ONCE vertex buffer (lesson from stencil-ops task #10): all
 * cells' BG+FG quads are laid out once in s_verts, copied once into a
 * single MmAllocateContiguousMemoryEx allocation, bound once. The
 * per-frame render callback only issues per-cell state writes (alpha
 * test enable/func/ref) + draws against the resident buffer; it NEVER
 * rewrites vertex data. Pure deterministic pattern; no banner, no
 * counter. Byte-identical across two cold runs.
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

/* Verified against hw/xbox/nv2a/nv2a_regs.h (rule #1). nxdk's pbkit
 * headers expose NV097_SET_ALPHA_TEST_ENABLE; NV097_SET_ALPHA_FUNC and
 * NV097_SET_ALPHA_REF are defined defensively below in case the local
 * nxdk header revision omits them. The addresses match the in-tree
 * register header exactly. */
#ifndef NV097_SET_ALPHA_TEST_ENABLE
#define NV097_SET_ALPHA_TEST_ENABLE 0x00000300
#endif
#ifndef NV097_SET_ALPHA_FUNC
#define NV097_SET_ALPHA_FUNC        0x0000033C
#endif
#ifndef NV097_SET_ALPHA_REF
#define NV097_SET_ALPHA_REF         0x00000340
#endif

/* 0-indexed PshAlphaFunc (hw/xbox/nv2a/pgraph/psh_regs.h:162).
 * The method takes this raw integer; no SDK V_* remap exists. */
#define ALPHAFUNC_NEVER    0
#define ALPHAFUNC_LESS     1
#define ALPHAFUNC_EQUAL    2
#define ALPHAFUNC_LEQUAL   3
#define ALPHAFUNC_GREATER  4
#define ALPHAFUNC_NOTEQUAL 5
#define ALPHAFUNC_GEQUAL   6
#define ALPHAFUNC_ALWAYS   7

#define ALPHA_REF 0x80  /* 128, shared by all cells */

typedef struct {
    float pos[3];     /* clip-space, z=0.5 */
    float color[4];   /* RGBA (0..1) */
} __attribute__((packed)) AlphaVertex;

/* Per-cell alpha-test tuple. bg is the alpha-test-OFF background quad
 * color; fg is the alpha-test-ON foreground quad color (RGB opaque RED;
 * fg[3] = the per-cell alpha A that drives the test). */
typedef struct {
    uint32_t func;     /* 0-indexed PshAlphaFunc */
    uint8_t  ref;      /* 8-bit reference */
    float    fg_alpha; /* A/255 — fragment alpha under test */
} AlphaCell;

/* BLUE background, RED foreground. Captured cell = RED (test pass) or
 * BLUE (test discard). See file header for the per-cell derivation. */
#define BG_RGB { 0.0f, 0.0f, 1.0f }   /* BLUE */
#define FG_RGB { 1.0f, 0.0f, 0.0f }   /* RED  */

static const AlphaCell k_cells[GRID_CELLS] = {
    /* 0 NEVER,   A=0xFF -> unconditional discard -> BLUE */
    { ALPHAFUNC_NEVER,    ALPHA_REF, 0xFF / 255.0f },
    /* 1 ALWAYS,  A=0x00 -> always pass            -> RED  */
    { ALPHAFUNC_ALWAYS,   ALPHA_REF, 0x00 / 255.0f },
    /* 2 LESS,    A=0x40 -> 64  < 128 true         -> RED  */
    { ALPHAFUNC_LESS,     ALPHA_REF, 0x40 / 255.0f },
    /* 3 LESS,    A=0xC0 -> 192 < 128 false        -> BLUE */
    { ALPHAFUNC_LESS,     ALPHA_REF, 0xC0 / 255.0f },
    /* 4 GEQUAL,  A=0x80 -> 128 >= 128 true        -> RED  */
    { ALPHAFUNC_GEQUAL,   ALPHA_REF, 0x80 / 255.0f },
    /* 5 GREATER, A=0x80 -> 128 >  128 false       -> BLUE */
    { ALPHAFUNC_GREATER,  ALPHA_REF, 0x80 / 255.0f },
    /* 6 EQUAL,   A=0x80 -> 128 == 128 true        -> RED  */
    { ALPHAFUNC_EQUAL,    ALPHA_REF, 0x80 / 255.0f },
    /* 7 NOTEQUAL,A=0x80 -> 128 != 128 false       -> BLUE */
    { ALPHAFUNC_NOTEQUAL, ALPHA_REF, 0x80 / 255.0f },
};

/* Per-cell: 2 quads (BG + FG) × 6 verts per quad = 12 verts per cell.
 * Total = 8 cells × 12 = 96 verts. */
#define VERTS_PER_QUAD     6
#define QUADS_PER_CELL     2  /* BG then FG */
#define VERTS_PER_CELL     (VERTS_PER_QUAD * QUADS_PER_CELL)
#define VERTS_TOTAL        (GRID_CELLS * VERTS_PER_CELL)

static AlphaVertex s_verts[VERTS_TOTAL];
static AlphaVertex *s_alloc_verts;

static inline void mk_vert(AlphaVertex *v, int x_w, int y_w,
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
static void emit_quad(AlphaVertex *out, int x0, int y0, int x1, int y1,
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
    static const float bg_rgb[3] = BG_RGB;
    static const float fg_rgb[3] = FG_RGB;
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = row * CELL_H;
            const int x1 = x0 + CELL_W;
            const int y1 = y0 + CELL_H;
            AlphaVertex *cell = &s_verts[idx * VERTS_PER_CELL];

            /* BG quad: opaque BLUE, alpha test OFF at draw time. */
            const float bg_rgba[4] = { bg_rgb[0], bg_rgb[1], bg_rgb[2],
                                       1.0f };
            emit_quad(&cell[0], x0, y0, x1, y1, bg_rgba);

            /* FG quad: RED with the cell's per-cell alpha A; alpha
             * test ON at draw time. */
            const float fg_rgba[4] = { fg_rgb[0], fg_rgb[1], fg_rgb[2],
                                       k_cells[idx].fg_alpha };
            emit_quad(&cell[VERTS_PER_QUAD], x0, y0, x1, y1, fg_rgba);
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
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    pb_end(p);
}

/* Background pass: alpha test explicitly OFF, so the BLUE quad always
 * writes regardless of any prior cell's alpha-test state. */
static void set_alpha_off(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    pb_end(p);
}

/* Foreground pass: alpha test ON with this cell's func + ref. */
static void set_alpha_for_cell(int idx)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 1);
    p = pb_push1(p, NV097_SET_ALPHA_FUNC, k_cells[idx].func);
    p = pb_push1(p, NV097_SET_ALPHA_REF,  k_cells[idx].ref);
    pb_end(p);
}

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(AlphaVertex), &s_alloc_verts[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(AlphaVertex), &s_alloc_verts[0].color[0]);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    /* Per cell: two draws against the resident write-once vertex
     * buffer. The BG draw runs with alpha test disabled (paints BLUE).
     * The FG draw runs with alpha test enabled and the cell's
     * (func, ref) — RED survives only if the test passes. */
    for (int idx = 0; idx < GRID_CELLS; idx++) {
        const int base = idx * VERTS_PER_CELL;

        set_alpha_off();
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         base, VERTS_PER_QUAD);

        set_alpha_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         base + VERTS_PER_QUAD, VERTS_PER_QUAD);
    }
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("alpha-test v0.1\n");

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
        "D:\\alpha-test-capture.bin",
        "D:\\alpha-test-done.txt",
        "alpha-test");
    return 0;
}
