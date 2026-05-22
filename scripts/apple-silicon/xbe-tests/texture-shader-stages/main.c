/*
 * texture-shader-stages — NV2A SHADER_STAGE_PROGRAM (D.8) dispatch Tier-1
 * diag XBE.  v0.2.
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
 * --- v0.2 scope --------------------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.13: "Iterate each valid (stage,
 * mode) pair from the 19-mode enum; per-mode signature pattern."
 * v0.2 expands v0.1's 4x2 grid to a 4x3 grid by adding a third row
 * that bypasses the t0 path entirely. The new row is a bisect cell
 * that distinguishes a renderer-side SHADER_STAGE_PROGRAM dispatch
 * bug from an XBE-side combiner / state-machine bug (per the v0.1
 * manifest's `expected_fail_notes` next-session experiment):
 *
 *   - Stage 0 only.
 *   - Three modes/sources tested:
 *     row 0: SHADER_STAGE_PROGRAM=PASS_THROUGH (0x04), combiner A=T0.
 *     row 1: SHADER_STAGE_PROGRAM=PROGRAM_NONE  (0x00), combiner A=T0.
 *     row 2: SHADER_STAGE_PROGRAM=PROGRAM_NONE  (0x00), combiner A=V0.
 *   - The other 17 modes deferred to v0.3+ (see "Does NOT catch" below).
 *
 * Why these two SHADER_STAGE_PROGRAM modes: per
 * `hw/xbox/nv2a/pgraph/glsl/psh.c:1170-1227`,
 *
 *     case PS_TEXTUREMODES_NONE:
 *         t0 = vec4(0.0, 0.0, 0.0, 1.0);
 *     case PS_TEXTUREMODES_PASSTHRU:
 *         t0 = pT0;   // the interpolated TEXCOORD0 attribute
 *
 * Both modes produce t0 deterministically from data that is either a
 * constant or a vertex-stream attribute — no actual texture sample is
 * read for either mode. This makes them the only two modes whose
 * expected output is byte-exactly predictable without modeling the
 * full sampler / filter / wrap state machine.
 *
 * Why the v0.2 row-2 V0/DIFFUSE-source bisect: per the v0.1 manifest's
 * `expected_fail_notes`, row 2 keeps SHADER_STAGE_PROGRAM at NONE
 * (same as row 1) but rewires the combiner stage-0 A_SOURCE to V0
 * (PS_REGISTER_V0 = 0x04, the interpolated vertex DIFFUSE attribute
 * at slot 3). The per-cell DIFFUSE attribute encodes (R,G,B,1).
 * Combiner stage 0 routes A=V0 -> R0 -> fragColor, bypassing t0
 * entirely. If row 2 renders the expected per-cell colors while
 * row 0 renders black, the failure localizes to the renderer's
 * PASS_THROUGH path (the t0 / pT0 chain or the
 * SHADER_STAGE_PROGRAM PASS_THROUGH dispatch); the XBE-side
 * combiner / vertex-attrib / state-machine plumbing is proven
 * sound by the working row 2 cells. If row 2 also renders black,
 * the failure is broader (combiner program never reaches the
 * fragment stage; or every per-cell draw is silently dropped).
 *
 * Catches that v0.2 detects:
 *   - SHADER_STAGE_PROGRAM 5-bit field mis-decoded at stage 0 — any
 *     wrong mode produces output that visibly differs from BOTH the
 *     PASS_THROUGH expectation (texcoord-as-color) AND the NONE
 *     expectation (solid BLACK). e.g. a renderer that always treats
 *     stage 0 as PROJECT2D would sample the dummy magenta texture and
 *     paint MAGENTA everywhere.
 *   - PASS_THROUGH collapsed to NONE: row 0 renders BLACK instead of
 *     R/G/B/W from TEXCOORD0.
 *   - NONE collapsed to PASS_THROUGH: row 1 renders the per-cell
 *     TEXCOORD0 color instead of BLACK.
 *   - TEXCOORD0 not propagating from VS to PS: row 0 renders a
 *     uniform color across all 4 cells instead of the 4 distinct
 *     R/G/B/W signatures, because the fragment pT0 input never
 *     receives the per-vertex TEXCOORD0 attribute.
 *   - The texture-stage "enabled" gate (psh.c:142-148) flipping
 *     incorrectly: if stage 0 is treated as disabled, the renderer
 *     forces stage_program=NONE, which makes the PASS_THROUGH row
 *     render as BLACK and falsely-matches the NONE row.
 *   - v0.2-specific: bisect between renderer-side PASS_THROUGH-path
 *     bug and XBE-side combiner / state-machine bug. Row 2 bypasses
 *     the t0 / pT0 chain entirely (A_SOURCE=V0 / DIFFUSE), so it
 *     proves the combiner / vertex-attrib / state-machine plumbing
 *     is sound when row 2 renders correctly. If row 0 stays BLACK
 *     while row 2 renders R/G/B/W, the regression is localized to
 *     the t0 or PASS_THROUGH dispatch path in the renderer.
 *
 * --- Geometry layout --------------------------------------------------
 *
 * 640x480 framebuffer. 4 cols × 3 rows = 12 cells; cell_w=160, cell_h=160.
 *
 *   col 0      col 1     col 2     col 3
 *      |         |         |         |
 *   y=0   +---------+---------+---------+---------+
 *         |  RED    |  GREEN  |  BLUE   |  WHITE  |  Row 0 = PASS_THROUGH, A=T0
 *   y=160 +---------+---------+---------+---------+
 *         |  BLACK  |  BLACK  |  BLACK  |  BLACK  |  Row 1 = NONE,         A=T0
 *   y=320 +---------+---------+---------+---------+
 *         |  RED    |  GREEN  |  BLUE   |  WHITE  |  Row 2 = NONE,         A=V0 (bisect)
 *   y=480 +---------+---------+---------+---------+
 *
 * Row 0 (PASS_THROUGH, A=T0): TEXCOORD0 attribute set to the cell's
 *                       target (R, G, B, 1.0) on all 4 vertices of
 *                       the cell's quad. PASS_THROUGH copies
 *                       pT0 -> t0 unchanged, and the combiner routes
 *                       t0 -> R0 -> fragColor. DIFFUSE held white.
 *
 * Row 1 (PROGRAM_NONE, A=T0): TEXCOORD0 attribute set to the same
 *                       per-cell colors as row 0 — the input is
 *                       irrelevant for PROGRAM_NONE since the
 *                       renderer hard-codes t0 = vec4(0, 0, 0, 1).
 *                       All 4 cells render solid BLACK. Identical
 *                       TEXCOORD0 input across rows 0 and 1
 *                       guarantees that any per-column color
 *                       difference between those rows is caused by
 *                       the SHADER_STAGE_PROGRAM mode dispatch,
 *                       not by attribute interpolation. DIFFUSE
 *                       held white.
 *
 * Row 2 (PROGRAM_NONE, A=V0 — bisect): TEXCOORD0 ignored (held the
 *                       same per-cell colors for vertex-attrib
 *                       symmetry; combiner now reads DIFFUSE
 *                       instead). DIFFUSE attribute set to the
 *                       cell's target (R, G, B, 1.0). Combiner
 *                       stage-0 A_SOURCE rewired to V0 (DIFFUSE);
 *                       routes v0 -> R0 -> fragColor. Expected
 *                       output mirrors row 0. The combiner program
 *                       and DIFFUSE attribute path bypass the
 *                       SHADER_STAGE_PROGRAM PASS_THROUGH dispatch
 *                       entirely; this isolates "renderer-side
 *                       PASS_THROUGH bug" from "XBE-side combiner /
 *                       state-machine / vertex-attrib bug" (per the
 *                       v0.1 manifest's `expected_fail_notes`
 *                       next-session experiment).
 *
 * --- Math derivation per cell ----------------------------------------
 *
 * The shared combiner program (identical across all 8 cells) routes
 * stage-0 output to fragColor:
 *
 *   COLOR ICW stage 0:
 *     A = t0.rgb (T0 source, UNSIGNED_IDENTITY mapping)
 *     B = (1 - ZERO) = 1            (UNSIGNED_INVERT of ZERO)
 *     C = 0, D = 0
 *
 *   COLOR OCW stage 0:
 *     AB_DST = R0, CD_DST = DISCARD, SUM_DST = DISCARD
 *     AB_DOT = 0, CD_DOT = 0, MUX = 0, OP = IDENTITY
 *
 *     -> R0.rgb = clamp(t0.rgb * 1, -1, 1) = clamp(t0.rgb, -1, 1)
 *
 *   Alpha stage 0: zeroed; final-combiner G provides fragColor.a.
 *
 *   FINAL combiner CW0:
 *     A = ZERO, B = ZERO, C = ZERO, D = R0
 *     -> fragColor.rgb = R0 + mix(C, B, A) = R0 + 0 = R0
 *
 *   FINAL combiner CW1:
 *     E = ZERO, F = ZERO, G = DIFFUSE.a (with G_ALPHA=1)
 *     -> fragColor.a = G = DIFFUSE.a = 1.0  (vertex DIFFUSE = white)
 *
 *   fragColor (post-framebuffer-clamp [0,1] and quantize to 8 bit per
 *   channel):
 *
 *     Row 0 (PASS_THROUGH, A=T0; t0 = pT0):
 *       cell 0: pT0=(1,0,0,1) -> (255,   0,   0, 255)  RED
 *       cell 1: pT0=(0,1,0,1) -> (  0, 255,   0, 255)  GREEN
 *       cell 2: pT0=(0,0,1,1) -> (  0,   0, 255, 255)  BLUE
 *       cell 3: pT0=(1,1,1,1) -> (255, 255, 255, 255)  WHITE
 *
 *     Row 1 (PROGRAM_NONE, A=T0; t0 = (0,0,0,1)):
 *       cells 4..7: (0, 0, 0, 255)  BLACK
 *
 *     Row 2 (PROGRAM_NONE, A=V0; combiner reads DIFFUSE):
 *       cell  8: v0=(1,0,0,1) -> (255,   0,   0, 255)  RED
 *       cell  9: v0=(0,1,0,1) -> (  0, 255,   0, 255)  GREEN
 *       cell 10: v0=(0,0,1,1) -> (  0,   0, 255, 255)  BLUE
 *       cell 11: v0=(1,1,1,1) -> (255, 255, 255, 255)  WHITE
 *
 * All target colors are saturated 0/255 cube corners; the
 * float -> framebuffer round-to-nearest-byte path produces the
 * byte-exact 8-bit values above for each cell interior. The manifest
 * applies `compare_overrides.max_changed_pct = 5.0` /
 * `min_signal_match_pct = 95.0` only to absorb (a) sub-pixel rasterizer
 * differences along the 4 inter-cell vertical edges and the 2 horizontal
 * row-boundary lines, and (b) the harness's `frame_quality_score` having
 * to pick the best post-XBE-load frame from the screenshot sequence; the
 * per-channel threshold remains the harness default (16) and is not
 * relaxed by this XBE.
 *
 * --- Texture stage binding (required for both modes) ------------------
 *
 * Per psh.c:142-148, the renderer overrides SHADER_STAGE_PROGRAM to
 * `NONE` whenever a stage is not "enabled" (i.e. stage texture binding
 * not active OR NV_PGRAPH_TEXCTL0_0_ENABLE is clear). To exercise the
 * dispatch path itself, BOTH the PASS_THROUGH cells AND the NONE cells
 * must have stage 0 enabled with a valid texture binding. v0.1 binds
 * a 4x4 LU_IMAGE_A8R8G8B8 magenta-filled dummy texture; the texture
 * data is never sampled by either mode but the binding must exist so
 * the renderer keeps the caller-supplied stage_program (vs. forcing
 * NONE). If the binding ever becomes a regression source — e.g. if a
 * sample accidentally reaches fragColor — every PASS_THROUGH cell
 * would render MAGENTA instead of its target color, which the harness
 * catches loudly.
 *
 * The dummy texture sets `BORDER_SOURCE_BIT = 1` (BORDER_SOURCE_COLOR
 * default per the 2026-05-22 xbed_texture.c fix); this leaves
 * `border_logical_size[i][0] == 0.0f` so the PASS_THROUGH assert
 * (psh.c:1225) does not trip.
 *
 * --- Does NOT catch (deferred to v0.3+) ------------------------------
 *
 *   - The other 17 of 19 modes (PROJECT2D, PROJECT3D, CUBEMAP,
 *     CLIPPLANE, BUMPENVMAP*, BRDF, DOT_ST, DOT_ZW, DOT_RFLCT_*,
 *     DOT_STR_*, DPNDNT_AR, DPNDNT_GB, DOTPRODUCT,
 *     DOT_RFLCT_SPEC_CONST).
 *   - Multi-stage chaining (stage 1 reading t0 from stage 0; stage 2
 *     reading t0+t1; etc.). Required for BUMPENVMAP*, DOT_*, DPNDNT_*,
 *     DOTPRODUCT modes.
 *   - SET_SHADER_OTHER_STAGE_INPUT field meaning per stage; v0.2 sets
 *     this register but does not exercise its semantics.
 *   - PROGRAM_NONE on a stage with DISABLED stage 0 binding (which
 *     would degenerate to the same `NONE` output — by design indistin-
 *     guishable; tracked as "this case is by construction equal to the
 *     ENABLED+NONE case" rather than a separate test).
 *   - Validating that stage_program at stages 1..3 is also honored
 *     (v0.2 holds stages 1..3 at PROGRAM_NONE for all cells; per
 *     psh.c:142-148 the renderer also clears their bits because those
 *     stages are explicitly disabled via xbed_texture_disable_*).
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
#define GRID_ROWS 3
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define CELL_W (WIN_W / GRID_COLS)   /* 160 */
#define CELL_H (WIN_H / GRID_ROWS)   /* 160 */

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
 * Only the two sources used by v0.2 cells are named here; full table
 * lives in `psh_regs.h`. */
#define ICW_A_SOURCE_V0    0x4  /* PS_REGISTER_V0 (DIFFUSE) */
#define ICW_A_SOURCE_T0    0x8  /* PS_REGISTER_T0 */

typedef struct {
    uint8_t  stage0_mode;     /* SHADER_STAGE_PROGRAM mode for stage 0 */
    float    tex0[4];         /* per-cell TEXCOORD0 RGBA */
    float    diffuse[4];      /* per-cell DIFFUSE RGBA (vertex slot 3) */
} ShaderStageCell;

static const ShaderStageCell k_cells[GRID_CELLS] = {
    /* Row 0 — PASS_THROUGH + combiner A=T0. TEXCOORD0 is the cell's
     *          target color; DIFFUSE held white (combiner ignores it). */
    { MODE_PASS_THROUGH, { 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 0  RED   */
    { MODE_PASS_THROUGH, { 0.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 1  GREEN */
    { MODE_PASS_THROUGH, { 0.0f, 0.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 2  BLUE  */
    { MODE_PASS_THROUGH, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 3  WHITE */
    /* Row 1 — PROGRAM_NONE + combiner A=T0. TEXCOORD0 mirrors row 0
     *          so any per-row delta is attributable solely to the
     *          SHADER_STAGE_PROGRAM mode dispatch; DIFFUSE held white. */
    { MODE_PROGRAM_NONE, { 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 4  BLACK */
    { MODE_PROGRAM_NONE, { 0.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 5  BLACK */
    { MODE_PROGRAM_NONE, { 0.0f, 0.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 6  BLACK */
    { MODE_PROGRAM_NONE, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 7  BLACK */
    /* Row 2 (v0.2 bisect) — PROGRAM_NONE + combiner A=V0. TEXCOORD0
     *          held identical to row 1 to keep the vertex-attrib path
     *          symmetric across rows; DIFFUSE drives the per-cell color
     *          via the rewired combiner A_SOURCE. */
    { MODE_PROGRAM_NONE, { 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } }, /* 8  RED   */
    { MODE_PROGRAM_NONE, { 0.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } }, /* 9  GREEN */
    { MODE_PROGRAM_NONE, { 0.0f, 0.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }, /* 10 BLUE  */
    { MODE_PROGRAM_NONE, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } }, /* 11 WHITE */
};

typedef struct {
    float pos[3];
    float tex[4];   /* TEXCOORD0 */
    float col[4];   /* DIFFUSE (white in rows 0/1; per-cell colors in row 2) */
} __attribute__((packed)) TsVertex;

#define VERTS_PER_QUAD 6
#define VERTS_TOTAL (GRID_CELLS * VERTS_PER_QUAD)

static TsVertex   s_verts[VERTS_TOTAL];
static TsVertex  *s_alloc_verts;
static void      *s_tex_vram;     /* shared dummy texture for all cells */

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
    /* DIFFUSE drives row 2's combiner A_SOURCE=V0 path; for rows 0/1
     * it's held white and the final-combiner G_SOURCE = DIFFUSE.a path
     * still supplies fragColor.a = 1.0 (the .a channel is 1.0 in every
     * row). */
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
    /* Magenta (A=FF, R=FF, G=00, B=FF) encoded for LU_IMAGE_A8R8G8B8:
     * memory bytes (low addr -> high) are B, G, R, A, so the 32-bit
     * little-endian uint32 = 0xFFFF00FF. Magenta is conspicuous if
     * any accidental sample reaches fragColor. */
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
    /* DIFFUSE (slot 3): Float4 — always white. */
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(TsVertex), &s_alloc_verts[0].col[0]);
}

/* Push the shared combiner program for the given ICW stage-0 A_SOURCE
 * (T0 or V0). Stage 0: A * 1 -> R0 (color); alpha stage 0 zeroed.
 * Final combiner: D = R0, G = DIFFUSE.a -> fragColor = (R0.rgb, 1.0). */
static void program_combiners_with_a_source(uint32_t a_source)
{
    /* COLOR ICW stage 0. A_SOURCE = configurable (T0 for rows 0/1,
     * V0 = DIFFUSE for row 2's bisect), A_MAP = UNSIGNED_IDENTITY
     * (0); B_SOURCE = ZERO (0x0), B_MAP = UNSIGNED_INVERT (0x1) -> B = 1. */
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

    /* COLOR OCW stage 0. AB_DST = R0 (0x4); CD_DST/SUM_DST = DISCARD;
     * MUX/DOT off; OP = NOSHIFT (identity). */
    const uint32_t ocw_color =
          TS_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST,        0x4)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST,        0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST,       0x0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE,    0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | TS_MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT);

    /* ALPHA ICW stage 0 — all ZERO (no alpha contribution from stage 0). */
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

    /* CONTROL: 1 stage active; SAME_FACTOR_ALL (C0/C1 unused here). */
    const uint32_t control =
          TS_MASK(NV097_SET_COMBINER_CONTROL_FACTOR0,
                  NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | TS_MASK(NV097_SET_COMBINER_CONTROL_FACTOR1,
                  NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | TS_MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 1);

    /* FINAL combiner CW0: A=B=C=ZERO so mix(C,B,A) = 0; D = R0
     * (0xC) so fragColor.rgb = R0. */
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
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE,  0x0C) /* R0 */
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA,   0)
        | TS_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0);

    /* FINAL combiner CW1: G = DIFFUSE.a so fragColor.a = 1.0. */
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
 * remain PROGRAM_NONE; they're also explicitly disabled via
 * xbed_texture_bind_stage0()'s tail (stages 1..3 ENABLE bit cleared). */
static void program_stage_program_for_cell(int idx)
{
    const uint32_t stage0_mode = k_cells[idx].stage0_mode;

    const uint32_t stage_program =
          TS_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, stage0_mode)
        | TS_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE1, MODE_PROGRAM_NONE)
        | TS_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE2, MODE_PROGRAM_NONE)
        | TS_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE3, MODE_PROGRAM_NONE);

    /* OTHER_STAGE_INPUT not meaningful for stage 0 mode dispatch
     * (per psh.c:1641 input_tex[0] is hard-coded to -1), but reset to
     * a known state (all-zero) so any prior XBE's residual setup
     * doesn't leak in. */
    const uint32_t other_input =
          TS_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE1, 0)
        | TS_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE2, 0)
        | TS_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE3, 0);

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, other_input);
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,     stage_program);
    pb_end(p);
}

/* Per-cell stage 0 binding — same dummy magenta texture for every
 * cell; only the SHADER_STAGE_PROGRAM mode varies across cells. */
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

#define ROW01_CELL_COUNT (2 * GRID_COLS)  /* rows 0 + 1 = 8 cells   */
#define ROW2_CELL_COUNT  GRID_COLS         /* row 2          = 4 cells */

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    /* Stage 0 texture binding — identical across all 12 cells (the
     * texture is never sampled by PASS_THROUGH or NONE, but the bind
     * must be active so the renderer doesn't override stage_program
     * to NONE due to a disabled stage). Push once per frame. */
    bind_dummy_stage0();

    /* Rows 0 + 1 — combiner A_SOURCE = T0. Push once for the 8 cells
     * of rows 0 (PASS_THROUGH) and 1 (PROGRAM_NONE). */
    program_combiners_with_a_source(ICW_A_SOURCE_T0);
    for (int idx = 0; idx < ROW01_CELL_COUNT; idx++) {
        program_stage_program_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }

    /* Row 2 — combiner A_SOURCE = V0 (DIFFUSE) bisect cells. Push the
     * combiner once for the 4 row-2 cells; SHADER_STAGE_PROGRAM stays
     * at PROGRAM_NONE (already set by the row-1 tail). */
    program_combiners_with_a_source(ICW_A_SOURCE_V0);
    for (int idx = ROW01_CELL_COUNT; idx < GRID_CELLS; idx++) {
        program_stage_program_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }

    xbed_texture_disable_all_stages();
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("texture-shader-stages v0.2\n");

    xbed_set_default_render_state();
    xbed_load_textured_shaders();

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
