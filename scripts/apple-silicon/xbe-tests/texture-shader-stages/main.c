/*
 * texture-shader-stages — NV2A SHADER_STAGE_PROGRAM (D.8) dispatch Tier-1
 * diag XBE.  v0.3.
 *
 * NV2A feature exercised: §D.8 (texture stage program — Xbox-specific
 *                          shader stages, 19-mode 5-bit-per-stage field).
 * NV097 methods exercised: SET_SHADER_STAGE_PROGRAM (0x1E70),
 *                          SET_SHADER_OTHER_STAGE_INPUT (0x1E78),
 *                          SET_TEXTURE_* (stage 0 binding via lib),
 *                          SET_COMBINER_* + SET_COMBINER_SPECULAR_FOG_*.
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- v0.3 scope --------------------------------------------------------
 *
 * v0.3 expands v0.2's 4x3 grid to a 4x4 grid (CELL_H=120px) to add two
 * new bisect experiments targeting the two surviving candidates from
 * v0.2's DIFFUSE-source bisect (see manifest.json `expected_fail_notes`
 * and orchestration-state/claude-status.md cycle 7):
 *
 *   (a) textured-shader state-machine interaction: xbed_load_textured_shaders()
 *       sets SHADER_STAGE_PROGRAM=2D_PROJECTIVE (via xbed_tex_ps.inl);
 *       the XBE's per-cell NONE/PASS_THROUGH overrides may not be honored
 *       on Metal when the textured-shader state is active.
 *   (b) combiner-rewrite ignored under textured-shader state: the
 *       row1->row2 A_SOURCE switch (T0->V0) may be silently dropped, so
 *       row 2's DIFFUSE-source bypass never reaches the fragment combiner.
 *
 * v0.3 rows:
 *
 *   row 0: PASS_THROUGH + combiner A=T0, textured shaders  → R/G/B/W
 *   row 1: NONE + combiner A=T0, textured shaders          → BLACK
 *   row 2: NONE + sentinel combiner (A=INVERT(ZERO)=1.0),  → WHITE×4
 *           textured shaders — completely independent of T0, V0,
 *           or any shader-stage program. Tests: "does any combiner
 *           output reach the framebuffer under textured-shader setup?"
 *   row 3: NONE + combiner A=V0/DIFFUSE, DEFAULT shaders   → R/G/B/W
 *           (mid-frame xbed_load_default_shaders switch).
 *           Tests: "does the V0/DIFFUSE combiner path work when the
 *           textured-shader state machine is not involved?"
 *
 * Diagnostic interpretation (all 4 outcome combinations):
 *
 *   sentinel(r2) PASS + control(r3) PASS:
 *     Combiner output reaches framebuffer under textured-shader setup;
 *     V0/DIFFUSE path works with default shaders. The textured-shader
 *     state machine specifically breaks the V0/DIFFUSE and/or PASS_THROUGH
 *     dispatch paths (candidate a is the lead suspect; candidate b may
 *     also be partially involved).
 *
 *   sentinel(r2) PASS + control(r3) FAIL:
 *     Combiner IS reached under textured-shader setup; but V0/DIFFUSE
 *     returns 0 or is mis-attributed even with default shaders.
 *     Candidate (b) is ruled in; candidate (a) ruled out as the sole
 *     cause. Points to a V0/DIFFUSE vertex-attribute read bug independent
 *     of shader setup.
 *
 *   sentinel(r2) FAIL + control(r3) PASS:
 *     Draws under textured-shader state are silently discarded or the
 *     combiner output is suppressed for the textured-shader pipeline.
 *     Candidate (a) is the clear culprit; V0/DIFFUSE path works fine
 *     once the textured-shader state is not in effect.
 *
 *   sentinel(r2) FAIL + control(r3) FAIL:
 *     SHADER_STAGE_PROGRAM override path is broken for all rows
 *     regardless of shader setup. Very fundamental issue — the
 *     per-cell NONE write is silently dropped before the fragment
 *     stage reaches the combiner.
 *
 * --- Geometry layout --------------------------------------------------
 *
 * 640x480 framebuffer. 4 cols × 4 rows = 16 cells; cell_w=160, cell_h=120.
 *
 *   col 0      col 1     col 2     col 3
 *      |         |         |         |
 *   y=0   +---------+---------+---------+---------+
 *         |  RED    |  GREEN  |  BLUE   |  WHITE  |  Row 0 = PASS_THROUGH, A=T0 (textured)
 *   y=120 +---------+---------+---------+---------+
 *         |  BLACK  |  BLACK  |  BLACK  |  BLACK  |  Row 1 = NONE, A=T0 (textured)
 *   y=240 +---------+---------+---------+---------+
 *         |  WHITE  |  WHITE  |  WHITE  |  WHITE  |  Row 2 = NONE, sentinel A=INVERT(0)=1.0 (textured)
 *   y=360 +---------+---------+---------+---------+
 *         |  RED    |  GREEN  |  BLUE   |  WHITE  |  Row 3 = NONE, A=V0/DIFFUSE (DEFAULT shaders)
 *   y=480 +---------+---------+---------+---------+
 *
 * Row 0 (PASS_THROUGH, A=T0, textured): TEXCOORD0 = (R,G,B,1) per cell;
 *   PASS_THROUGH copies pT0 -> t0; combiner A=T0 -> R0 -> fragColor.
 *   Expected: RED, GREEN, BLUE, WHITE.
 *
 * Row 1 (PROGRAM_NONE, A=T0, textured): same TEXCOORD0 as row 0;
 *   t0 = (0,0,0,1) regardless; combiner A=T0=0 -> R0=0 -> BLACK.
 *   Expected: BLACK x 4.
 *
 * Row 2 (PROGRAM_NONE, sentinel, textured): TEXCOORD0 = per-cell colors
 *   (symmetric with other rows; combiner ignores it). DIFFUSE = (1,1,1,1).
 *   Combiner: A = INVERT(ZERO) = 1.0, B = INVERT(ZERO) = 1.0;
 *   R0 = A*B + C*D = 1.0 -> fragColor = WHITE.
 *   Completely independent of T0, V0, shader-stage program, or texture state.
 *   Expected: WHITE x 4.
 *
 * Row 3 (PROGRAM_NONE, A=V0/DIFFUSE, DEFAULT shaders): mid-frame call to
 *   xbed_load_default_shaders() switches the VS/PS; per-cell DIFFUSE =
 *   (R,G,B,1); combiner A=V0=DIFFUSE -> R0 -> fragColor. Same combiner
 *   program as v0.2 row 2, but now under default (non-textured) shaders.
 *   Expected: RED, GREEN, BLUE, WHITE.
 *
 * --- Math derivation per cell ----------------------------------------
 *
 * Row 0 (PASS_THROUGH, A=T0):
 *   t0 = pT0 = TEXCOORD0; combiner A*1 -> V0; FINAL D=V0 -> fragColor.rgb.
 *   cell 0: pT0=(1,0,0,1) -> (255, 0, 0, 255) RED
 *   cell 1: pT0=(0,1,0,1) -> (0, 255, 0, 255) GREEN
 *   cell 2: pT0=(0,0,1,1) -> (0, 0, 255, 255) BLUE
 *   cell 3: pT0=(1,1,1,1) -> (255,255,255,255) WHITE
 *
 * Row 1 (PROGRAM_NONE, A=T0):
 *   t0 = (0,0,0,1); V0 = 0 -> BLACK. Cells 4..7: (0,0,0,255).
 *
 * Row 2 (sentinel, A=INVERT(ZERO)):
 *   A = 1.0, B = 1.0, V0 = 1.0 -> (255,255,255,255) WHITE. Cells 8..11.
 *
 * Row 3 (PROGRAM_NONE, A=V0, default shaders):
 *   v0 = DIFFUSE; combiner A*1 -> V0; FINAL D=V0 -> fragColor.rgb.
 *   cell 12: v0=(1,0,0,1) -> (255, 0, 0, 255) RED
 *   cell 13: v0=(0,1,0,1) -> (0, 255, 0, 255) GREEN
 *   cell 14: v0=(0,0,1,1) -> (0, 0, 255, 255) BLUE
 *   cell 15: v0=(1,1,1,1) -> (255,255,255,255) WHITE
 *
 * --- Texture stage binding -------------------------------------------
 *
 * Stage 0 bound to a dummy 4x4 magenta texture for ALL 16 cells.
 * Required so the psh.c:142-148 gate does not override SHADER_STAGE_PROGRAM
 * to NONE due to a disabled stage. The texture is never actually sampled
 * by NONE, PASS_THROUGH, or the sentinel combiner. Any accidental sample
 * reaching fragColor would paint MAGENTA instead of the target color.
 *
 * --- Shader switching mid-frame --------------------------------------
 *
 * xbed_load_textured_shaders() is called at the START of render_one()
 * so rows 0-2 (textured context) begin each frame in the correct state.
 * xbed_load_default_shaders() is called immediately before row 3's draws
 * to switch to the default (non-textured) VS/PS. The textured-shader
 * state remains in effect when render_one() is next invoked.
 *
 * --- Does NOT catch (deferred to v0.4+) ------------------------------
 *
 *   - The 17 remaining modes (PROJECT2D, PROJECT3D, CUBEMAP, CLIPPLANE,
 *     BUMPENVMAP*, BRDF, DOT_*, DPNDNT_*, DOTPRODUCT,
 *     DOT_RFLCT_SPEC_CONST).
 *   - Multi-stage chaining (stage 1 reading t0 from stage 0).
 *   - SET_SHADER_OTHER_STAGE_INPUT field semantics.
 *
 * --- Reproducibility -------------------------------------------------
 *
 * Pure deterministic pattern; no banner, no counter, no per-frame
 * variation. Byte-identical across two cold runs.
 */
#include "xbed_capture.h"
#include "xbed_runtime.h"
#include "xbed_texture.h"

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

/* Dummy texture is 4x4 magenta (LU_IMAGE_A8R8G8B8). Not sampled by
 * PASSTHRU or NONE, but the binding must exist so the renderer doesn't
 * override SHADER_STAGE_PROGRAM to NONE due to a disabled stage. */
#define TEX_W 4
#define TEX_H 4
#define TEX_BPP 4
#define TEX_SIZE_BYTES (TEX_W * TEX_H * TEX_BPP)

/* SHADER_STAGE_PROGRAM mode codes (5-bit per stage in the 32-bit
 * register; stage0 is bits 0..4). See `psh_regs.h::PS_TEXTUREMODES`. */
#define MODE_PROGRAM_NONE  0x00
#define MODE_PASS_THROUGH  0x04

/* Combiner ICW A_SOURCE register codes — see `psh_regs.h::PS_REGISTER_*`.
 * Only the sources used by v0.3 cells are named here. */
#define ICW_A_SOURCE_V0    0x4  /* PS_REGISTER_V0 (DIFFUSE) */
#define ICW_A_SOURCE_T0    0x8  /* PS_REGISTER_T0 */

typedef struct {
    uint8_t  stage0_mode;     /* SHADER_STAGE_PROGRAM mode for stage 0 */
    float    tex0[4];         /* per-cell TEXCOORD0 RGBA */
    float    diffuse[4];      /* per-cell DIFFUSE RGBA (vertex slot 3) */
} ShaderStageCell;

static const ShaderStageCell k_cells[GRID_CELLS] = {
    /* Row 0 — PASS_THROUGH + combiner A=T0, textured shaders. */
    { MODE_PASS_THROUGH, { 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  0 RED   */
    { MODE_PASS_THROUGH, { 0.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  1 GREEN */
    { MODE_PASS_THROUGH, { 0.0f, 0.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  2 BLUE  */
    { MODE_PASS_THROUGH, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  3 WHITE */
    /* Row 1 — PROGRAM_NONE + combiner A=T0, textured shaders. */
    { MODE_PROGRAM_NONE, { 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  4 BLACK */
    { MODE_PROGRAM_NONE, { 0.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  5 BLACK */
    { MODE_PROGRAM_NONE, { 0.0f, 0.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  6 BLACK */
    { MODE_PROGRAM_NONE, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  7 BLACK */
    /* Row 2 — PROGRAM_NONE + sentinel combiner, textured shaders.
     * TEXCOORD0 held per-col for symmetry; DIFFUSE.a=1 for final-combiner
     * G=DIFFUSE.a. The sentinel combiner ignores both T0 and V0. */
    { MODE_PROGRAM_NONE, { 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  8 WHITE sentinel */
    { MODE_PROGRAM_NONE, { 0.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /*  9 WHITE sentinel */
    { MODE_PROGRAM_NONE, { 0.0f, 0.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 10 WHITE sentinel */
    { MODE_PROGRAM_NONE, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 11 WHITE sentinel */
    /* Row 3 — PROGRAM_NONE + combiner A=V0, DEFAULT shaders (control).
     * DIFFUSE drives the per-cell color via the combiner A_SOURCE=V0 path,
     * bypassing the textured-shader state machine entirely. */
    { MODE_PROGRAM_NONE, { 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } }, /* 12 RED   control */
    { MODE_PROGRAM_NONE, { 0.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } }, /* 13 GREEN control */
    { MODE_PROGRAM_NONE, { 0.0f, 0.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }, /* 14 BLUE  control */
    { MODE_PROGRAM_NONE, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 15 WHITE control */
};

typedef struct {
    float pos[3];
    float tex[4];   /* TEXCOORD0 */
    float col[4];   /* DIFFUSE */
} __attribute__((packed)) TsVertex;

#define VERTS_PER_QUAD 6
#define VERTS_TOTAL (GRID_CELLS * VERTS_PER_QUAD)

static TsVertex   s_verts[VERTS_TOTAL];
static TsVertex  *s_alloc_verts;
static void      *s_tex_vram;

/* Manually-built MASK helper that mirrors lib/ps.inl. */
#define TS_MASK(FIELD, VALUE) \
    (((VALUE) << (__builtin_ffs(FIELD) - 1)) & (FIELD))

static inline void mk_vert(TsVertex *v, int x_w, int y_w,
                           const float tex0[4], const float diffuse[4])
{
    v->pos[0]   = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1]   = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2]   = 0.5f;
    v->tex[0]   = tex0[0];
    v->tex[1]   = tex0[1];
    v->tex[2]   = tex0[2];
    v->tex[3]   = tex0[3];
    v->col[0]   = diffuse[0];
    v->col[1]   = diffuse[1];
    v->col[2]   = diffuse[2];
    v->col[3]   = diffuse[3];
}

static void emit_quad(TsVertex *out, int x0, int y0, int x1, int y1,
                      const float tex0[4], const float diffuse[4])
{
    mk_vert(&out[0], x0, y0, tex0, diffuse);
    mk_vert(&out[1], x1, y0, tex0, diffuse);
    mk_vert(&out[2], x1, y1, tex0, diffuse);
    mk_vert(&out[3], x0, y0, tex0, diffuse);
    mk_vert(&out[4], x1, y1, tex0, diffuse);
    mk_vert(&out[5], x0, y1, tex0, diffuse);
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
            emit_quad(&s_verts[idx * VERTS_PER_QUAD], x0, y0, x1, y1,
                      k_cells[idx].tex0, k_cells[idx].diffuse);
        }
    }
}

static void fill_dummy_texture(void *vram)
{
    /* Magenta (A=FF, R=FF, G=00, B=FF) in LU_IMAGE_A8R8G8B8 byte order:
     * 32-bit LE = 0xFFFF00FF. Conspicuous if any sample leaks to fragColor. */
    uint32_t *p = (uint32_t *)vram;
    for (int i = 0; i < TEX_W * TEX_H; i++) {
        p[i] = 0xFFFF00FFu;
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

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    /* POSITION (slot 0): Float3. */
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(TsVertex), &s_alloc_verts[0].pos[0]);
    /* TEX0 (slot 9): Float4 — full RGBA passes through into pT0. */
    xbed_set_attrib_pointer(
        9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(TsVertex), &s_alloc_verts[0].tex[0]);
    /* DIFFUSE (slot 3): Float4. */
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(TsVertex), &s_alloc_verts[0].col[0]);
}

/* Push the shared combiner program for the given ICW stage-0 A_SOURCE
 * (T0 or V0). Stage 0: A * 1 -> V0 (OCW AB_DST=0x4=PS_REGISTER_V0);
 * alpha stage 0 zeroed. Final combiner: D = V0, G = V0.a -> fragColor. */
static void program_combiners_with_a_source(uint32_t a_source)
{
    /* COLOR ICW stage 0. A_SOURCE = configurable, A_MAP = UNSIGNED_IDENTITY;
     * B_SOURCE = ZERO, B_MAP = UNSIGNED_INVERT (1) -> B = 1.0. */
    const uint32_t icw_color =
          TS_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, a_source)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA,  0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA,  0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP,    1)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA,  0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA,  0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP,    0);

    /* COLOR OCW stage 0. AB_DST = V0 (0x4); OCW destinations use the
     * PS_REGISTER encoding, so 0x4 = PS_REGISTER_V0, same as the working
     * ps.inl and xbed_tex_ps.inl. FINAL CW0 D_SOURCE reads from V0. */
    const uint32_t ocw_color =
          TS_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST,        0x4)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST,        0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST,       0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE,    0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT);

    /* ALPHA ICW stage 0 — all ZERO. */
    const uint32_t icw_alpha =
          TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA,  1)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA,  1)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA,  1)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA,  1)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP,    0);

    /* ALPHA OCW stage 0 — all DISCARD. */
    const uint32_t ocw_alpha =
          TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST,     0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_CD_DST,     0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST,    0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, 0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, NV097_SET_COMBINER_ALPHA_OCW_OP_NOSHIFT);

    /* CONTROL: 1 stage active. */
    const uint32_t control =
          TS_MASK(NV097_SET_COMBINER_CONTROL_FACTOR0,
                  NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | TS_MASK(NV097_SET_COMBINER_CONTROL_FACTOR1,
                  NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | TS_MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 1);

    /* FINAL combiner CW0: D = V0 (0x4) -> fragColor.rgb = V0.
     * OCW AB_DST=0x4 wrote A_SOURCE result into V0; FINAL reads V0.
     * Matches ps.inl and xbed_tex_ps.inl which both use D_SOURCE=0x4. */
    const uint32_t final_cw0 =
          TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE,  0x04) /* V0 */
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0);

    /* FINAL combiner CW1: G = V0.a (= DIFFUSE.a) -> fragColor.a = 1.0. */
    const uint32_t final_cw1 =
          TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE,  0x04) /* V0.a */
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA,   1)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_SPECULAR_CLAMP, 0);

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4, icw_color);
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4, ocw_color);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4, icw_alpha);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 0 * 4, ocw_alpha);
    p = pb_push1(p, NV097_SET_COMBINER_CONTROL,           control);
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,  final_cw0);
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,  final_cw1);
    pb_end(p);
}

/* Sentinel combiner: A = INVERT(ZERO) = 1.0, B = INVERT(ZERO) = 1.0.
 * R0 = A*B + C*D = 1.0*1.0 + 0 = 1.0 -> (255,255,255) WHITE.
 * Independent of T0, V0, TEXCOORD0, SHADER_STAGE_PROGRAM, texture bindings.
 * If draws under textured-shader setup produce WHITE, the combiner IS
 * executing and prior V0/T0 failures are about those inputs returning 0.
 * If draws produce BLACK, the draw is being silently discarded before the
 * combiner executes. */
static void program_combiners_sentinel(void)
{
    /* COLOR ICW: A=INVERT(ZERO)=1.0, B=INVERT(ZERO)=1.0; C=D=0.
     * A_SOURCE=ZERO(0x0), A_MAP=UNSIGNED_INVERT(1) -> A=1.0.
     * B_SOURCE=ZERO(0x0), B_MAP=UNSIGNED_INVERT(1) -> B=1.0. */
    const uint32_t icw_color =
          TS_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA,  0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP,    1)  /* UNSIGNED_INVERT */
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA,  0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP,    1)  /* UNSIGNED_INVERT */
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA,  0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA,  0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP,    0);

    /* COLOR OCW: AB_DST = V0 (0x4). OCW 0x4 = PS_REGISTER_V0. */
    const uint32_t ocw_color =
          TS_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST,        0x4)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST,        0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST,       0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE,    0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT);

    /* ALPHA ICW — all ZERO (no alpha combiner contribution). */
    const uint32_t icw_alpha =
          TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA,  1)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA,  1)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA,  1)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP,    0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA,  1)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP,    0);

    /* ALPHA OCW — all DISCARD. */
    const uint32_t ocw_alpha =
          TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST,     0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_CD_DST,     0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST,    0x0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, 0)
        | TS_MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, NV097_SET_COMBINER_ALPHA_OCW_OP_NOSHIFT);

    /* CONTROL: 1 stage active. */
    const uint32_t control =
          TS_MASK(NV097_SET_COMBINER_CONTROL_FACTOR0,
                  NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | TS_MASK(NV097_SET_COMBINER_CONTROL_FACTOR1,
                  NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | TS_MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 1);

    /* FINAL combiner CW0: D = V0 (0x4) -> fragColor.rgb = 1.0 = WHITE.
     * OCW wrote INVERT(ZERO)*INVERT(ZERO)=1.0 into V0; FINAL reads V0. */
    const uint32_t final_cw0 =
          TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE,  0x04) /* V0 */
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0);

    /* FINAL combiner CW1: G = DIFFUSE.a -> fragColor.a = 1.0. */
    const uint32_t final_cw1 =
          TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_SOURCE,  0x00)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE,  0x04) /* DIFFUSE */
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA,   1)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_INVERSE, 0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_SPECULAR_CLAMP, 0);

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4, icw_color);
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4, ocw_color);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4, icw_alpha);
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 0 * 4, ocw_alpha);
    p = pb_push1(p, NV097_SET_COMBINER_CONTROL,           control);
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,  final_cw0);
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,  final_cw1);
    pb_end(p);
}

/* Push SHADER_STAGE_PROGRAM for the cell's stage-0 mode. Stages 1..3
 * remain PROGRAM_NONE. */
static void program_stage_program_for_cell(int idx)
{
    const uint32_t stage0_mode = k_cells[idx].stage0_mode;

    const uint32_t stage_program =
          TS_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, stage0_mode)
        | TS_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE1, MODE_PROGRAM_NONE)
        | TS_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE2, MODE_PROGRAM_NONE)
        | TS_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE3, MODE_PROGRAM_NONE);

    const uint32_t other_input =
          TS_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE1, 0)
        | TS_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE2, 0)
        | TS_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE3, 0);

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, other_input);
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,     stage_program);
    pb_end(p);
}

static void bind_dummy_stage0(void)
{
    XbedTextureStage0 params;
    xbed_texture_init_argb8888_defaults(&params);
    params.vram_addr   = s_tex_vram;
    params.width       = TEX_W;
    params.height      = TEX_H;
    params.pitch_bytes = TEX_W * TEX_BPP;
    xbed_texture_bind_stage0(&params);
}

#define ROW01_CELL_COUNT  (2 * GRID_COLS)  /* rows 0+1 = 8 cells  */
#define ROW012_CELL_COUNT (3 * GRID_COLS)  /* rows 0+1+2 = 12 cells */

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    /* Rows 0-2 use the textured-shader context. Load textured shaders at
     * the top of every frame so mid-frame state from the prior frame's
     * row-3 default-shader switch does not persist. */
    xbed_load_textured_shaders();

    /* Stage 0 binding — same dummy magenta texture for all 16 cells.
     * Required so psh.c:142-148 does not override SHADER_STAGE_PROGRAM
     * to NONE due to a disabled stage. */
    bind_dummy_stage0();

    /* Rows 0 + 1 — textured shaders, combiner A=T0. */
    program_combiners_with_a_source(ICW_A_SOURCE_T0);
    for (int idx = 0; idx < ROW01_CELL_COUNT; idx++) {
        program_stage_program_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }

    /* Row 2 — textured shaders, sentinel combiner (A=B=INVERT(ZERO)=1.0).
     * Expected WHITE regardless of T0, V0, or texture state. */
    program_combiners_sentinel();
    for (int idx = ROW01_CELL_COUNT; idx < ROW012_CELL_COUNT; idx++) {
        program_stage_program_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }

    /* Row 3 — DEFAULT shaders + combiner A=V0/DIFFUSE (control bisect).
     * Mid-frame shader switch isolates the textured-shader state machine
     * from the V0/DIFFUSE combiner path. */
    xbed_load_default_shaders();
    program_combiners_with_a_source(ICW_A_SOURCE_V0);
    for (int idx = ROW012_CELL_COUNT; idx < GRID_CELLS; idx++) {
        program_stage_program_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }

    xbed_texture_disable_all_stages();
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("texture-shader-stages v0.3\n");

    xbed_set_default_render_state();

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

    s_tex_vram = MmAllocateContiguousMemoryEx(
        TEX_SIZE_BYTES, 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_tex_vram) {
        debugPrint("MmAllocateContiguousMemoryEx (tex) failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    fill_dummy_texture(s_tex_vram);

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\texture-shader-stages-capture.bin",
        "D:\\texture-shader-stages-done.txt",
        "texture-shader-stages");
    return 0;
}
