/*
 * combiner-basic — single-stage NV2A register-combiner ops Tier-1 diag XBE.
 *
 * NV2A feature exercised: §D.1 (combiner control), §D.2 (color ICW/OCW),
 *                         §D.4 (input mappings), §D.5 (output scale
 *                         modifiers), §D.7 (final combiner).
 * NV097 methods:           SET_COMBINER_CONTROL, SET_COMBINER_COLOR_ICW,
 *                          SET_COMBINER_COLOR_OCW, SET_COMBINER_ALPHA_ICW,
 *                          SET_COMBINER_ALPHA_OCW, SET_COMBINER_SPECULAR_
 *                          FOG_CW0, SET_COMBINER_SPECULAR_FOG_CW1.
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.12: a 16-cell matrix of (input
 * mapping × output scale modifier) at fixed input/op; expected color
 * per cell from the combiner equation.
 *
 * Configuration shared across all 16 cells:
 *
 *   Single combiner stage (stage 0), color path only. Combiner equation:
 *
 *     A = map(DIFFUSE.rgb)         (mapping varies per column)
 *     B = UNSIGNED_INVERT(ZERO) = 1
 *     C = UNSIGNED_IDENTITY(ZERO) = 0
 *     D = UNSIGNED_IDENTITY(ZERO) = 0
 *
 *     AB raw = A * B = mapped(DIFFUSE)
 *     CD raw = C * D = 0
 *     SUM raw = AB + CD = mapped(DIFFUSE)
 *
 *   OCW: AB_DST = DISCARD, CD_DST = DISCARD, SUM_DST = R0, OP = scale
 *        modifier (varies per row), MUX_ENABLE = 0, AB_DOT = 0, CD_DOT = 0.
 *
 *   R0 = clamp(scale_op(SUM_raw), -1, 1)        (per xemu psh.c clamp)
 *
 *   Final combiner:
 *     A = ZERO, B = ZERO, C = ZERO  (so mix(C, B, A) = 0)
 *     D = R0                        (so fragColor.rgb = R0 + 0 = R0)
 *     E = ZERO, F = ZERO
 *     G = DIFFUSE.a (alpha output passthrough, kept = 1.0)
 *
 *   fragColor.rgb = clamp(R0, 0, 1)             (framebuffer clamp)
 *   fragColor.a   = 1.0
 *
 * Per-cell variation:
 *
 *   - Column 0 (UI):  A_MAP = UNSIGNED_IDENTITY  (0x00) -> A = c
 *   - Column 1 (II):  A_MAP = UNSIGNED_INVERT    (0x20) -> A = 1 - c
 *   - Column 2 (EN):  A_MAP = EXPAND_NORMAL      (0x40) -> A = 2c - 1
 *   - Column 3 (EE):  A_MAP = EXPAND_NEGATE      (0x60) -> A = 1 - 2c
 *
 *   - Row 0 (ID):  OP = IDENTITY        (0x00) -> y = x
 *   - Row 1 (SL1): OP = SHIFTLEFT_1     (0x10) -> y = 2x
 *   - Row 2 (SL2): OP = SHIFTLEFT_2     (0x20) -> y = 4x
 *   - Row 3 (NB):  OP = NOSHIFT_BIAS    (0x08) -> y = x - 0.5
 *
 * Skipped from the catalog (D.4 + D.5, deferred to second wave):
 *
 *   - HALFBIAS_NORMAL / HALFBIAS_NEGATE input mappings (rare; less
 *     discriminative when paired with the scale row).
 *   - SIGNED_IDENTITY / SIGNED_NEGATE input mappings (DIFFUSE always
 *     non-negative; SIGNED_IDENTITY is identical to UNSIGNED_IDENTITY
 *     and SIGNED_NEGATE produces only non-positive values that the
 *     framebuffer clamp collapses to BLACK — covered as a corner case
 *     by second-wave coverage XBEs).
 *   - SHIFTLEFT_1_BIAS / SHIFTRIGHT_1 output scale modifiers (deferred
 *     to second wave; the four selected here give the strongest
 *     mapping-vs-scale discrimination per cell while still hitting
 *     the BIAS + asymmetric-magnitude cases).
 *
 * --- Math derivation per cell ----------------------------------------
 *
 * Fixed DIFFUSE = (0.25, 0.5, 0.75, 1.0). Different per channel so a
 * single grid populates 3 independent channels of evidence per cell.
 *
 * Stage 1: mapping (per column, per channel):
 *
 *                R=0.25  G=0.5   B=0.75
 *   UI:          0.25    0.5     0.75
 *   II:          0.75    0.5     0.25
 *   EN:         -0.5     0.0     0.5
 *   EE:          0.5     0.0    -0.5
 *
 * Stage 2: scale (per row, then clamp to [-1, 1] per xemu psh.c):
 *
 *   ID  (y = x):
 *     UI -> ( 0.25,  0.5,   0.75 )
 *     II -> ( 0.75,  0.5,   0.25 )
 *     EN -> (-0.5,   0.0,   0.5  )  [no clamp needed]
 *     EE -> ( 0.5,   0.0,  -0.5  )
 *
 *   SL1 (y = 2x), clamp [-1,1]:
 *     UI -> ( 0.5,   1.0,   1.0  )  [B clamped from 1.5]
 *     II -> ( 1.0,   1.0,   0.5  )  [R clamped from 1.5]
 *     EN -> (-1.0,   0.0,   1.0  )
 *     EE -> ( 1.0,   0.0,  -1.0  )
 *
 *   SL2 (y = 4x), clamp [-1,1]:
 *     UI -> ( 1.0,   1.0,   1.0  )  [G,B clamped]
 *     II -> ( 1.0,   1.0,   1.0  )  [R,G clamped]
 *     EN -> (-1.0,   0.0,   1.0  )  [R,B clamped from -2/2]
 *     EE -> ( 1.0,   0.0,  -1.0  )  [R,B clamped]
 *
 *   NB  (y = x - 0.5):
 *     UI -> (-0.25,  0.0,   0.25 )
 *     II -> ( 0.25,  0.0,  -0.25 )
 *     EN -> (-1.0,  -0.5,   0.0  )
 *     EE -> ( 0.0,  -0.5,  -1.0  )
 *
 * Stage 3: framebuffer clamp [0,1] and quantize to 8-bit per channel:
 *
 *                R    G    B
 *   ID/UI:       64   128  191
 *   ID/II:       191  128  64
 *   ID/EN:       0    0    128
 *   ID/EE:       128  0    0
 *
 *   SL1/UI:      128  255  255
 *   SL1/II:      255  255  128
 *   SL1/EN:      0    0    255
 *   SL1/EE:      255  0    0
 *
 *   SL2/UI:      255  255  255
 *   SL2/II:      255  255  255
 *   SL2/EN:      0    0    255
 *   SL2/EE:      255  0    0
 *
 *   NB/UI:       0    0    64
 *   NB/II:       64   0    0
 *   NB/EN:       0    0    0
 *   NB/EE:       0    0    0
 *
 * Mid-tones (64, 128, 191) are byte-exact in the raw framebuffer
 * (xemu writes 8-bit RGBA8 from the GLSL/Metal fragment shader; the
 * agent + harness PNG capture is raw, no display gamma applied).
 * The mapping psh.c -> framebuffer is float -> round-to-nearest-byte:
 *   0.25 * 255 = 63.75  -> 64 (round)
 *   0.5  * 255 = 127.5  -> 128 (round-half-to-even or up)
 *   0.75 * 255 = 191.25 -> 191
 * Float-precision drift (1-2 LSB) is well within the harness's
 * default threshold = 16 per channel. No `compare_overrides` needed.
 *
 * --- Catches --------------------------------------------------------
 *
 *   - Wrong A_MAP: column 0 (UI) would not produce DIFFUSE; column 1
 *     (II) would not produce (1 - DIFFUSE).
 *   - Sign-flip on EN vs EE (or vice versa): swapping the two columns
 *     produces visibly inverted cells (clamp turns sign-flips into
 *     all-BLACK vs all-color).
 *   - Wrong scale OP: any row swap produces a different mid-tone
 *     pattern; clamp behavior in SL1/SL2 rows rules out OP_NOSHIFT
 *     being treated as OP_SHIFTLEFT.
 *   - Final combiner D_SOURCE != R0: cells would render with the
 *     wrong source (e.g. DIFFUSE -> (64, 128, 191) for every cell).
 *
 * --- Does NOT catch (documented limits, second-wave follow-ups) -----
 *
 *   - Multi-stage chaining (stage 1 reads R0 from stage 0): covered
 *     by `combiner-multi-stage` second-wave XBE.
 *   - HALFBIAS / SIGNED mappings: see "Skipped" above.
 *   - SHIFTLEFT_1_BIAS / SHIFTRIGHT_1 scales: see "Skipped" above.
 *   - DOT_PRODUCT operations (AB_DOT / CD_DOT): covered by
 *     `combiner-dot-product` second-wave XBE.
 *   - BLUE_TO_ALPHA flags: covered by `combiner-blue-to-alpha`.
 *   - MUX path (muxsum_op = AB_CD_MUX, OCW MUX_ENABLE = 1): not
 *     exercised here -- every cell has MUX_ENABLE = 0, so a SUM-vs-
 *     MUX regression on the MUX_ENABLE field decoding is NOT caught.
 *     Covered by second-wave `combiner-mux` XBE which sets
 *     MUX_ENABLE = 1 and exercises the AB/CD selection via an
 *     explicit alpha-stage R0.a write.
 *   - Missing [-1, 1] clamp in psh.c add_stage_code: clamp removal
 *     would change R0 to hold the unclamped value, but no stage 1
 *     reads R0 in this XBE; the SUM->framebuffer path collapses
 *     out-of-range values via the [0, 1] framebuffer clamp before
 *     capture, so a clamp regression is undetectable here. Covered
 *     by second-wave `combiner-multi-stage` where a downstream
 *     stage reads R0 with an EXPAND mapping that exposes the
 *     unclamped magnitude.
 *   - Alpha combiner path: covered by `combiner-alpha`.
 *   - C0/C1 per-stage constants: covered by `combiner-constants`.
 *   - Final-combiner specific formula corners (E*F product, SPECLIT,
 *     EF_PROD input, COMPLEMENT_V1/R0, CLAMP_SUM): covered by
 *     `final-combiner-formula`.
 *
 * --- Geometry layout ------------------------------------------------
 *
 * WIN_W=640, WIN_H=480. 4 cols × 4 rows = 16 cells; cell_w=160, cell_h=120.
 *
 *   col*160       (col+1)*160
 *      |             |
 *   y=0   +--+--+--+--+
 *         |UI|II|EN|EE|  ID
 *   y=120 +--+--+--+--+
 *         |UI|II|EN|EE|  SL1
 *   y=240 +--+--+--+--+
 *         |UI|II|EN|EE|  SL2
 *   y=360 +--+--+--+--+
 *         |UI|II|EN|EE|  NB
 *   y=480 +--+--+--+--+
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
#define GRID_ROWS 4
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define CELL_W (WIN_W / GRID_COLS)   /* 160 */
#define CELL_H (WIN_H / GRID_ROWS)   /* 120 */

#define VERTS_PER_QUAD 6  /* two triangles A-B-C / A-C-D */
#define VERTS_TOTAL    (GRID_CELLS * VERTS_PER_QUAD)

typedef struct {
    float pos[3];     /* clip-space, z=0.5 */
    float color[4];   /* RGBA (0..1); fixed across all cells */
} __attribute__((packed)) CombVertex;

/* Per-cell (mapping, scale) tuple. Field naming mirrors the catalog
 * names in `nv2a-feature-surface-research.md` §D. */
typedef struct {
    uint32_t a_map;     /* PS_INPUTMAPPING_* shifted (0x00 / 0x20 / 0x40 / 0x60) */
    uint32_t op;        /* SET_COMBINER_COLOR_OCW_OP_* (0/2/4/1 in encoded space) */
} CombCell;

/* The A_MAP value is a 3-bit code in the ICW (PS_INPUTMAPPING_* /
 * 0x20). xemu's nv_regs.h encodes it as 0..7 in the *_MAP field.
 * NV097_SET_COMBINER_COLOR_ICW_A_MAP value codes:
 *   0 UNSIGNED_IDENTITY
 *   1 UNSIGNED_INVERT
 *   2 EXPAND_NORMAL
 *   3 EXPAND_NEGATE
 *   4 HALFBIAS_NORMAL
 *   5 HALFBIAS_NEGATE
 *   6 SIGNED_IDENTITY
 *   7 SIGNED_NEGATE
 *
 * The 3-bit value lives in the *_MAP field of the ICW DWORD. The
 * pbkit MASK() helper bit-aligns the value into its mask, so we pass
 * the raw 0..7 code. */
#define MAP_UI 0
#define MAP_II 1
#define MAP_EN 2
#define MAP_EE 3

/* OCW_OP encodings from nv_regs.h:
 *   0 NOSHIFT      (identity)
 *   1 NOSHIFT_BIAS (y = x - 0.5)
 *   2 SHIFTLEFTBY1 (y = 2x)
 *   3 SHIFTLEFTBY1_BIAS
 *   4 SHIFTLEFTBY2 (y = 4x)
 *   6 SHIFTRIGHTBY1
 */
#define OP_ID  0
#define OP_NB  1
#define OP_SL1 2
#define OP_SL2 4

static const CombCell k_cells[GRID_CELLS] = {
    /* Row 0 (ID  / NOSHIFT)   */
    { MAP_UI, OP_ID  }, { MAP_II, OP_ID  }, { MAP_EN, OP_ID  }, { MAP_EE, OP_ID  },
    /* Row 1 (SL1 / SHIFTLEFT_1) */
    { MAP_UI, OP_SL1 }, { MAP_II, OP_SL1 }, { MAP_EN, OP_SL1 }, { MAP_EE, OP_SL1 },
    /* Row 2 (SL2 / SHIFTLEFT_2) */
    { MAP_UI, OP_SL2 }, { MAP_II, OP_SL2 }, { MAP_EN, OP_SL2 }, { MAP_EE, OP_SL2 },
    /* Row 3 (NB  / NOSHIFT_BIAS) */
    { MAP_UI, OP_NB  }, { MAP_II, OP_NB  }, { MAP_EN, OP_NB  }, { MAP_EE, OP_NB  },
};

/* Fixed DIFFUSE for every vertex. Chosen so each channel exercises a
 * distinct input value (0.25 / 0.5 / 0.75) and alpha = 1.0 so the
 * default alpha-output path keeps the framebuffer alpha at 255. */
#define DIFFUSE_R 0.25f
#define DIFFUSE_G 0.50f
#define DIFFUSE_B 0.75f
#define DIFFUSE_A 1.00f

static CombVertex   s_verts[VERTS_TOTAL];
static CombVertex  *s_alloc_verts;

static inline void mk_vert(CombVertex *v, int x_w, int y_w)
{
    v->pos[0]   = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1]   = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2]   = 0.5f;
    v->color[0] = DIFFUSE_R;
    v->color[1] = DIFFUSE_G;
    v->color[2] = DIFFUSE_B;
    v->color[3] = DIFFUSE_A;
}

/* Emit two triangles covering the rect (x0,y0)-(x1,y1) into out[0..5]
 * using A-B-C / A-C-D winding. */
static void emit_quad(CombVertex *out, int x0, int y0, int x1, int y1)
{
    mk_vert(&out[0], x0, y0);
    mk_vert(&out[1], x1, y0);
    mk_vert(&out[2], x1, y1);
    mk_vert(&out[3], x0, y0);
    mk_vert(&out[4], x1, y1);
    mk_vert(&out[5], x0, y1);
}

static void build_geometry(void)
{
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0  = col * CELL_W;
            const int y0  = row * CELL_H;
            const int x1  = x0 + CELL_W;
            const int y1  = y0 + CELL_H;
            emit_quad(&s_verts[idx * VERTS_PER_QUAD], x0, y0, x1, y1);
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
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE,   0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK,          0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE,        0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE,   0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE,    0);
    pb_end(p);
}

/* Manually-built MASK helper that mirrors the one used by lib/ps.inl.
 * Each FIELD is a bitmask; VALUE is shifted into FIELD's LSB. */
#define COMB_MASK(FIELD, VALUE) \
    (((VALUE) << (__builtin_ffs(FIELD) - 1)) & (FIELD))

/* Push the combiner program for a single cell. Color stage 0 routes:
 *
 *     A = (DIFFUSE.rgb)  with per-cell map; ALPHA-source flag = 0 (use rgb)
 *     B = UNSIGNED_INVERT(ZERO) = 1   (ALPHA-source flag = 0)
 *     C = UNSIGNED_IDENTITY(ZERO) = 0 (ALPHA-source flag = 0)
 *     D = UNSIGNED_IDENTITY(ZERO) = 0 (ALPHA-source flag = 0)
 *
 *     OCW: AB_DST = DISCARD, CD_DST = DISCARD, SUM_DST = R0 (0xC),
 *          OP = per-cell scale, MUX_ENABLE = 0, AB_DOT = 0, CD_DOT = 0.
 *
 * The alpha stage 0 ICW/OCW are left at the lib's passthrough defaults
 * (no alpha writes per stage; final-combiner G_SOURCE = DIFFUSE
 * supplies fragColor.a = 1.0).
 *
 * Combiner control sets ITERATION_COUNT = 1 (only stage 0 active).
 * Final-combiner CW0 D_SOURCE = R0 routes our stage-0 SUM_DST to
 * fragColor.rgb; the rest of the final inputs zero out.
 */
static void program_combiner_for_cell(int idx)
{
    const uint32_t a_map_code = k_cells[idx].a_map;
    const uint32_t op_code    = k_cells[idx].op;

    /* COLOR ICW stage 0. ALPHA flag = 0 means "read RGB" for that input.
     * SOURCE = 0x04 (DIFFUSE) for A; SOURCE = 0x00 (ZERO) for B/C/D.
     * MAP codes per the constants near the top of this file. */
    const uint32_t icw_color =
          COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x04)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA,  0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP,    a_map_code)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x00)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA,  0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP,    1) /* UNSIGNED_INVERT -> 1 */
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x00)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA,  0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP,    0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x00)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA,  0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP,    0);

    /* COLOR OCW stage 0. SUM_DST = R0 (0xC); AB/CD DISCARD; OP varies. */
    const uint32_t ocw_color =
          COMB_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST,        0x0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST,        0x0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST,       0xC)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE,    0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | COMB_MASK(NV097_SET_COMBINER_COLOR_OCW_OP,            op_code);

    /* CONTROL: 1 stage active; SAME_FACTOR_ALL for C0/C1 (unused here). */
    const uint32_t control =
          COMB_MASK(NV097_SET_COMBINER_CONTROL_FACTOR0,
                    NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | COMB_MASK(NV097_SET_COMBINER_CONTROL_FACTOR1,
                    NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | COMB_MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 1);

    /* FINAL combiner CW0: A=B=C=ZERO so mix(C,B,A) = 0; D = R0 so
     * fragColor.rgb = R0. Per psh.c add_final_stage_code:
     *   fragColor.rgb = D + mix(C, B, A)
     *   fragColor.a   = G
     */
    const uint32_t final_cw0 =
          COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE,  0x00)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA,   0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_INVERSE, 0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE,  0x00)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_ALPHA,   0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_INVERSE, 0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE,  0x00)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_ALPHA,   0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_INVERSE, 0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE,  0x0C) /* R0 */
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA,   0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0);

    /* FINAL combiner CW1: G = DIFFUSE.a so fragColor.a = 1.0. */
    const uint32_t final_cw1 =
          COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_SOURCE,  0x00)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_ALPHA,   0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_INVERSE, 0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_SOURCE,  0x00)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_ALPHA,   0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_INVERSE, 0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE,  0x04) /* DIFFUSE */
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA,   1)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_INVERSE, 0)
        | COMB_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_SPECULAR_CLAMP, 0);

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4, icw_color);
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4, ocw_color);
    p = pb_push1(p, NV097_SET_COMBINER_CONTROL,           control);
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,  final_cw0);
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,  final_cw1);
    pb_end(p);
}

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(CombVertex), &s_alloc_verts[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(CombVertex), &s_alloc_verts[0].color[0]);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    for (int idx = 0; idx < GRID_CELLS; idx++) {
        program_combiner_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("combiner-basic v0.1\n");

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
        "D:\\combiner-basic-capture.bin",
        "D:\\combiner-basic-done.txt",
        "combiner-basic");
    return 0;
}
