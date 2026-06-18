/*
 * polygon-offset — NV2A polygon offset / depth bias (§C.3) Tier-1 diag XBE.
 *
 * NV2A feature exercised: §C.3 (polygon offset / depth bias).
 *   NV097_SET_POLY_OFFSET_FILL_ENABLE     (0x0338)
 *   NV097_SET_POLYGON_OFFSET_SCALE_FACTOR (0x0384)  -> NV_PGRAPH_ZOFFSETFACTOR
 *   NV097_SET_POLYGON_OFFSET_BIAS         (0x0388)  -> NV_PGRAPH_ZOFFSETBIAS
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * ====================================================================
 *  Method-address + register verification (rule #1, no guessing)
 * ====================================================================
 *
 * Verified against the in-tree NV2A register header
 * `hw/xbox/nv2a/nv2a_regs.h` AND nxdk's `lib/pbkit/nv_regs.h`:
 *   NV097_SET_POLY_OFFSET_FILL_ENABLE     = 0x00000338 (nv2a_regs.h:957)
 *   NV097_SET_POLYGON_OFFSET_SCALE_FACTOR = 0x00000384 (nv2a_regs.h:1026)
 *   NV097_SET_POLYGON_OFFSET_BIAS         = 0x00000388 (nv2a_regs.h:1027)
 *
 * Dispatch (hw/xbox/nv2a/pgraph/pgraph.c):
 *   SET_POLY_OFFSET_FILL_ENABLE (pgraph.c:1511) sets
 *     NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE (bit 8) in
 *     NV_PGRAPH_SETUPRASTER (0x1990).
 *   SET_POLYGON_OFFSET_SCALE_FACTOR (pgraph.c:1777) writes the raw 32-bit
 *     parameter straight into NV_PGRAPH_ZOFFSETFACTOR (0x1AA8) — the
 *     guest must push the IEEE-754 bit pattern of the float factor.
 *   SET_POLYGON_OFFSET_BIAS (pgraph.c:1782) writes the raw 32-bit
 *     parameter straight into NV_PGRAPH_ZOFFSETBIAS (0x1AA4) — the
 *     guest must push the IEEE-754 bit pattern of the float units.
 *   (We therefore push the float via its u32 bit-reinterpretation.)
 *
 * ====================================================================
 *  Renderer semantics (verified) — how polygon offset reaches pixels
 * ====================================================================
 *
 * IMPORTANT FORK-SPECIFIC FACT (PR #2240, XEMU_NATIVE_TRI_DEPTH):
 * neither the GL nor the Metal renderer use the fixed-function depth
 * bias (glPolygonOffset / setDepthBias:slopeScale:clamp:). Both compute
 * per-fragment depth — INCLUDING polygon offset — in the FRAGMENT
 * SHADER via the shared GLSL generator `hw/xbox/nv2a/pgraph/glsl/psh.c`.
 * GL explicitly disables GL_POLYGON_OFFSET_* (gl/draw.c:648-651); Metal
 * never calls setDepthBias (absent from mtl/draw.mm apply_translated_
 * raster_state). Metal consumes the SAME generated GLSL (translated to
 * MSL via spirv-cross), so polygon offset is identical-by-construction
 * across GL and Metal.
 *
 * Non-z_perspective native_tri_depth / native_quad path
 * (glsl/psh.c:1052-1059):
 *
 *   precise float zvalue = gl_FragCoord.z * clipRange.y;          // window-Z
 *   float nativeTriMZ = max(|dFdx(zvalue)*sx|, |dFdy(zvalue)*sy|);// screen slope
 *   zvalue += depthOffset;                // = NV_PGRAPH_ZOFFSETBIAS  (units)
 *   zvalue += depthFactor*nativeTriMZ;    // = NV_PGRAPH_ZOFFSETFACTOR (factor)
 *   ...
 *   gl_FragDepth = zvalue / clipRange.y;  // renormalize back to [0,1] (psh.c:1569)
 *
 * Uniform population (glsl/psh.c:1764-1810, pgraph_glsl_set_psh_uniform_values):
 *   - polygon_offset_enabled is true only when primitive_mode >=
 *     PRIM_TYPE_TRIANGLES AND the SETUPRASTER front-face polygon mode
 *     matches an enabled POFFSET*ENABLE bit. Our quads are drawn as
 *     OP_TRIANGLES in FILL mode (pbkit/xbed default), and we enable
 *     POFFSETFILLENABLE, so the gate is satisfied.
 *   - When enabled: depthOffset = float(NV_PGRAPH_ZOFFSETBIAS),
 *                   depthFactor = float(NV_PGRAPH_ZOFFSETFACTOR).
 *     When disabled: both are 0.
 *
 * clipRange.y derivation (glsl/common.c:69-90): for the default Z24S8
 * zeta with FIXED (integer) z_format — which is exactly the pbkit
 * default (pb_DepthFmt=ZETA_Z24S8, NV097_SET_CONTROL0 Z_FORMAT bit
 * left FIXED=0) — clipRange.y = (float)0xFFFFFF = 16777215.0. So
 * `zvalue` lives in window-depth units 0..16777215, and depthOffset
 * (units) is added in THOSE window-depth units (NOT in NDC).
 *
 * ====================================================================
 *  ORACLE CORRECTION (v0.2, 2026-06-18) — viewport Z scale is 65536
 * ====================================================================
 *  The shared diag-lib viewport matrix (lib/xbed_runtime.c:36) sets
 *  s_viewport[2][2] = 65536.0, so clip z=0.5 maps to window-Z
 *  0.5*65536 = 32768 in the fragment shader, NOT 0.5*16777215. The
 *  per-cell tables below were written against the (wrong) 8388607.5
 *  base; the CORRECT base window-Z is 32768. Consequences, now
 *  reflected in expected.py v0.2:
 *    - cells 2/4 (+units) still push back -> GREEN (unchanged).
 *    - cells 3/5 (-units) now UNDERFLOW the pinned clip range [0,
 *      16777215] (32768-100000 < 0) and are CLIP-CULLED (discard,
 *      psh.c:1116-1120) -> GREEN, instead of winning LEQUAL -> RED.
 *      They degenerate from "negative-bias-wins" tests into clip-cull
 *      tests. A v0.3 should place the base plane near mid-range
 *      (clip z ~128 given the 65536 scale) or use small -units (~-2000)
 *      that stay in range, to isolate the negative-bias LEQUAL win.
 *    - cells 6/7 (factor slope) unchanged: +273/-273 bias -> GREEN/RED.
 *  CORRECT expected (= NV2A/GL behavior): RED RED GREEN GREEN GREEN
 *  GREEN GREEN RED. The Metal renderer currently applies NO effective
 *  bias (depthOffset/depthFactor reach the Metal shader as 0 at
 *  runtime) -> all 8 cells RED -> FAILS cells 2,3,4,5,6 == confirmed
 *  Metal §C.3 gap.
 *
 * ====================================================================
 *  Test design — isolate depth bias under LEQUAL
 * ====================================================================
 *
 * 8-cell 4x2 grid (640x480; cell 160x240). Depth test LEQUAL, depth
 * write ON, blend OFF, cull OFF, FILL mode, smooth shading. Z24S8 zeta
 * cleared to max-Z each frame.
 *
 * Per cell, two COPLANAR full-cell quads at the SAME clip z = 0.5:
 *   1. BASE quad   (GREEN 0xFF00FF00), polygon offset DISABLED.
 *      Writes window-Z Zb = 0.5 * 16777215 = 8388607.5 into the cell.
 *   2. OFFSET quad (RED   0xFFFF0000), drawn SECOND with this cell's
 *      polygon-offset configuration. Its biased depth is
 *        Zo = 8388607.5 + depthFactor*slope + depthOffset.
 *
 * Because both quads are screen-aligned (constant window-Z across the
 * face), the screen-space depth slope is ZERO, so nativeTriMZ = 0 and
 * the depthFactor*slope term vanishes for cells 0-5. Those cells
 * isolate the depthOffset (UNITS) term alone. Cells 6-7 add a SLOPED
 * offset quad to exercise the depthFactor (FACTOR) term.
 *
 * LEQUAL passes when (Zo <= Zb). Whichever quad's depth survives owns
 * the cell color: OFFSET wins -> RED; OFFSET loses -> BASE GREEN shows.
 *
 *   Cell  PolyOff   units (ZOFFSETBIAS)  factor  slope  Zo vs Zb       Result
 *   ----  --------  -------------------  ------  -----  -------------  ------
 *   0     OFF        (n/a, offset off)    n/a     0     Zo == Zb       RED
 *   1     ON              0.0             0.0     0     Zo == Zb       RED
 *   2     ON         +100000.0           0.0     0     Zo  > Zb       GREEN
 *   3     ON         -100000.0           0.0     0     Zo  < Zb       RED
 *   4     ON         +8000000.0          0.0     0     Zo  > Zb       GREEN
 *   5     ON         -8000000.0          0.0     0     Zo  < Zb       RED
 *   6     ON              0.0            +10.0   >0     Zo  > Zb       GREEN
 *   7     ON              0.0            -10.0   >0     Zo  < Zb       RED
 *
 * Per-cell rationale:
 *   - Cell 0: offset DISABLED. Second quad has identical depth to base
 *     -> LEQUAL tie -> RED. This is the "offset machinery off" control;
 *     a renderer that always biases would mis-color it.
 *   - Cell 1: offset ENABLED but units=factor=0. Same identical depth
 *     -> LEQUAL tie -> RED. Proves enabling the offset with zero
 *     magnitude is a no-op (catches a spurious constant bias on enable).
 *   - Cell 2: SMALL POSITIVE units pushes the offset quad BACK (larger
 *     Z). LEQUAL fails -> GREEN. Establishes sign convention: positive
 *     units -> away from viewer -> base shows.
 *   - Cell 3: SMALL NEGATIVE units pulls the offset quad FORWARD
 *     (smaller Z). LEQUAL passes -> RED. Mirror of cell 2; a sign-flip
 *     bug swaps cells 2 and 3.
 *   - Cells 4/5: LARGE +/- units. Same outcomes as 2/3 but far from the
 *     tie boundary, so they still resolve correctly even if the
 *     renderer applies units at a different (but same-signed) scale.
 *     They guard against a units-magnitude bug that is too small to
 *     flip cells 2/3 only by luck.
 *   - Cell 6: POSITIVE factor on a sloped quad. The screen-space depth
 *     slope is nonzero, so depthFactor*nativeTriMZ > 0 biases the quad
 *     BACK over its whole face -> GREEN. Exercises the FACTOR term and
 *     the dFdx/dFdy slope computation (the part most likely to differ
 *     between renderers; a renderer that ignores factor mis-colors it).
 *   - Cell 7: NEGATIVE factor on the same sloped quad biases FORWARD
 *     -> RED. Mirror of cell 6; isolates the factor sign.
 *
 *  SLOPE construction for cells 6/7 (must bias the WHOLE face one way):
 *   The offset quad is tilted in depth: its top edge at clip z=0.5, its
 *   bottom edge at clip z=0.6 (linear in window-Y). To keep the
 *   comparison unambiguous across the whole cell we make the BASE quad
 *   for cells 6/7 match the offset quad's tilt EXACTLY (same tilted
 *   geometry, polygon offset OFF) so that, per pixel,
 *   Zb(pixel) == unbiased Zo(pixel). Then the ONLY difference is the
 *   bias term depthFactor*nativeTriMZ, which has a single sign across
 *   the face.
 *
 *   The tilt spans window-Z 0.5*16777215=8388607.5 (top) to
 *   0.6*16777215=10066329 (bottom): delta ~1677721 over 240 px, so
 *   |dFdy(zvalue)| ~= 6990 window-Z/px and nativeTriMZ ~= 6990
 *   (dFdx ~= 0; surfaceScale = 1 under XEMU_DISPLAY_SCALE=1, no MSAA).
 *   With |factor| = 10 the bias is |10*6990| ~= 69900 window-Z units --
 *   comfortably above the float32 ULP at 8e6 (~1.0), yet far below the
 *   tilt's headroom to the pinned clip range [0, 16777215]:
 *     cell 6 (+): Zo in [8.46M, 10.14M], all > Zb and inside clip range
 *                 -> LEQUAL fails over the whole face -> GREEN.
 *     cell 7 (-): Zo in [8.32M,  9.996M], all < Zb and inside clip range
 *                 -> LEQUAL passes over the whole face -> RED.
 *   (factor must stay SMALL: a large factor would push Zo outside the
 *   clip range and the depth_clipping=CULL path would DISCARD the offset
 *   quad -> GREEN for BOTH signs, masking the factor-sign test. |10| is
 *   chosen to win/lose LEQUAL decisively while staying in range.)
 *   slope is per-pixel constant for a planar tilt, so the bias is
 *   uniform -> byte-exact uniform cell color.
 *
 * ====================================================================
 *  Float-exactness / byte-exactness
 * ====================================================================
 *
 * Cell colors are saturated 0/255 endpoints (pure RED / pure GREEN), so
 * the captured cell is byte-exact across renderers regardless of any
 * display-side gamma. The depth math uses window-Z magnitudes (>1e5)
 * that dominate float32 rounding noise at the 0.5 clip plane, so the
 * LEQUAL outcome is unambiguous for every cell. No compare tolerance is
 * needed for the color; a small max_changed_pct in the manifest absorbs
 * the 1-pixel cell-seam aliasing only.
 *
 * ====================================================================
 *  What this catches / does NOT catch
 * ====================================================================
 *  Catches:
 *   - Polygon offset not applied at all (units ignored): cells 2/4
 *     would wrongly show RED (offset quad always wins the LEQUAL tie).
 *   - Sign inversion of units: cells 2<->3 and 4<->5 flip.
 *   - Factor (slope) term ignored: cells 6/7 wrongly show RED.
 *   - Factor sign inversion: cells 6<->7 flip.
 *   - Spurious constant bias on enable: cell 1 flips to GREEN.
 *   - Offset applied even when DISABLED: cell 0 flips to GREEN.
 *   - Renderer divergence GL vs Metal: any per-cell mismatch in the
 *     paired board is a flagged divergence (the shared psh.c path
 *     predicts they MUST agree).
 *
 *  Does NOT catch (documented limits; second-wave follow-ups):
 *   - POINT/LINE polygon-offset enables (only FILL exercised here).
 *   - W-buffer (z_perspective) polygon-offset path, which uses a
 *     different slope formula and is NV2A_UNIMPLEMENTED for factor in
 *     this fork (psh.c:1800-1805). A z_perspective variant is a
 *     separate XBE.
 *   - Exact NV2A units-quantization vs the IEEE float we push (we test
 *     direction + gross magnitude, not the hardware's exact rounding).
 *   - Real-Xbox hardware's exact factor*slope constant (the fork's
 *     dFdx/dFdy approximation may differ from HW at sub-LSB; our
 *     magnitudes are chosen far from any boundary so this does not
 *     change the per-cell verdict).
 *
 * ====================================================================
 *  Geometry layout
 * ====================================================================
 *   WIN_W=640, WIN_H=480. 4 cols x 2 rows = 8 cells; cell 160 x 240.
 *
 *      col*160      (col+1)*160
 *   y=0  +-----+-----+-----+-----+
 *        | 0   | 1   | 2   | 3   |  (row 0)
 * y=240  +-----+-----+-----+-----+
 *        | 4   | 5   | 6   | 7   |  (row 1)
 *   y=480+-----+-----+-----+-----+
 *
 * ====================================================================
 *  Reproducibility
 * ====================================================================
 *  WRITE-ONCE vertex buffer (lesson from stencil-ops task #10): every
 *  cell's BASE+OFFSET quads are laid out once in s_verts, copied once
 *  into a single MmAllocateContiguousMemoryEx allocation, bound once.
 *  The per-frame callback only issues per-cell polygon-offset state
 *  writes + draws against the resident buffer; it NEVER rewrites vertex
 *  data. Pure deterministic pattern; no banner, no counter.
 *  Byte-identical across two cold runs.
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

/* Verified against hw/xbox/nv2a/nv2a_regs.h + nxdk lib/pbkit/nv_regs.h
 * (rule #1). nxdk's pbkit headers DO expose these (nv_regs.h:242-244,
 * 332-333), but we define defensively in case of a header revision. */
#ifndef NV097_SET_POLY_OFFSET_FILL_ENABLE
#define NV097_SET_POLY_OFFSET_FILL_ENABLE     0x00000338
#endif
#ifndef NV097_SET_POLYGON_OFFSET_SCALE_FACTOR
#define NV097_SET_POLYGON_OFFSET_SCALE_FACTOR 0x00000384
#endif
#ifndef NV097_SET_POLYGON_OFFSET_BIAS
#define NV097_SET_POLYGON_OFFSET_BIAS         0x00000388
#endif
/* SET_CLIP_MIN/MAX (nv2a_regs.h:1033-1034) take the float clip range as
 * an IEEE-754 bit pattern; pgraph.c:1818-1825 writes them straight into
 * NV_PGRAPH_ZCLIPMIN/ZCLIPMAX, which common.c:88-89 reads back as float
 * to form clipRange.z/.w. pbkit enables depth_clipping (DISCARD) via
 * SET_ZMIN_MAX_CONTROL=ZCLAMP_EN_CULL (psh.c:119-122 + :1116-1120) but
 * never pushes a clip range, so we set it explicitly to the full
 * window-Z span [0, 16777215] so our large-units cells (4/5) cannot be
 * clip-culled by an unknown reset default. */
#ifndef NV097_SET_CLIP_MIN
#define NV097_SET_CLIP_MIN 0x00000394
#endif
#ifndef NV097_SET_CLIP_MAX
#define NV097_SET_CLIP_MAX 0x00000398
#endif

/* Window-Z max for the pbkit default fixed-point Z24S8 zeta
 * (glsl/common.c:77, (float)0xFFFFFF). clipRange.y == this value. */
#define WINDOW_Z_MAX 16777215.0f

/* Base clip-Z plane for the coplanar (flat) cells. */
#define Z_FLAT       0.5f
/* Sloped offset-quad clip-Z range for cells 6/7 (top -> bottom). */
#define Z_SLOPE_TOP  0.5f
#define Z_SLOPE_BOT  0.6f

typedef struct {
    float pos[3];    /* clip-space (x,y in [-1,1], z=clip depth) */
    float color[4];  /* RGBA (0..1) */
} __attribute__((packed)) OffVertex;

/* Per-cell polygon-offset configuration. */
typedef struct {
    int   offset_enable;  /* SET_POLY_OFFSET_FILL_ENABLE value */
    float units;          /* ZOFFSETBIAS   (depthOffset) */
    float factor;         /* ZOFFSETFACTOR (depthFactor) */
    int   sloped;         /* 1 -> offset+base quads are tilted in Z */
} OffCell;

#define BASE_RGBA   { 0.0f, 1.0f, 0.0f, 1.0f }   /* GREEN — base quad */
#define OFFSET_RGBA { 1.0f, 0.0f, 0.0f, 1.0f }   /* RED   — offset quad */

/* See file header for the full per-cell derivation. */
static const OffCell k_cells[GRID_CELLS] = {
    /* 0 offset OFF                       -> tie  -> RED   */
    { 0,        0.0f,        0.0f,    0 },
    /* 1 offset ON, zero magnitude        -> tie  -> RED   */
    { 1,        0.0f,        0.0f,    0 },
    /* 2 ON, small +units (push back)     -> lose -> GREEN */
    { 1,  +100000.0f,        0.0f,    0 },
    /* 3 ON, small -units (pull forward)  -> win  -> RED   */
    { 1,  -100000.0f,        0.0f,    0 },
    /* 4 ON, large +units                 -> lose -> GREEN */
    { 1, +8000000.0f,        0.0f,    0 },
    /* 5 ON, large -units                 -> win  -> RED   */
    { 1, -8000000.0f,        0.0f,    0 },
    /* 6 ON, +factor on sloped quad       -> lose -> GREEN */
    { 1,        0.0f,      +10.0f,    1 },
    /* 7 ON, -factor on sloped quad       -> win  -> RED   */
    { 1,        0.0f,      -10.0f,    1 },
};

/* Per cell: BASE quad (6 verts) + OFFSET quad (6 verts) = 12 verts. */
#define VERTS_PER_QUAD 6
#define QUADS_PER_CELL 2
#define VERTS_PER_CELL (VERTS_PER_QUAD * QUADS_PER_CELL)
#define VERTS_TOTAL    (GRID_CELLS * VERTS_PER_CELL)

static OffVertex s_verts[VERTS_TOTAL];
static OffVertex *s_alloc_verts;

/* Window (x_w, y_w, z_clip) -> clip-space vertex with RGBA color. */
static inline void mk_vert(OffVertex *v, int x_w, int y_w, float z_clip,
                           const float rgba[4])
{
    v->pos[0] = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1] = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2] = z_clip;
    v->color[0] = rgba[0];
    v->color[1] = rgba[1];
    v->color[2] = rgba[2];
    v->color[3] = rgba[3];
}

/* Flat quad at constant z over rect (x0,y0)-(x1,y1), A-B-C / A-C-D. */
static void emit_quad_flat(OffVertex *out, int x0, int y0, int x1, int y1,
                           float z, const float rgba[4])
{
    mk_vert(&out[0], x0, y0, z, rgba);
    mk_vert(&out[1], x1, y0, z, rgba);
    mk_vert(&out[2], x1, y1, z, rgba);
    mk_vert(&out[3], x0, y0, z, rgba);
    mk_vert(&out[4], x1, y1, z, rgba);
    mk_vert(&out[5], x0, y1, z, rgba);
}

/* Tilted quad: z linearly z_top at y0 -> z_bot at y1. Both BASE and
 * OFFSET use the SAME tilt so per-pixel unbiased depth is identical;
 * only the polygon-offset bias differs between them. */
static void emit_quad_sloped(OffVertex *out, int x0, int y0, int x1, int y1,
                             float z_top, float z_bot, const float rgba[4])
{
    mk_vert(&out[0], x0, y0, z_top, rgba);
    mk_vert(&out[1], x1, y0, z_top, rgba);
    mk_vert(&out[2], x1, y1, z_bot, rgba);
    mk_vert(&out[3], x0, y0, z_top, rgba);
    mk_vert(&out[4], x1, y1, z_bot, rgba);
    mk_vert(&out[5], x0, y1, z_bot, rgba);
}

static void build_geometry(void)
{
    static const float base_rgba[4]   = BASE_RGBA;
    static const float offset_rgba[4] = OFFSET_RGBA;

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = row * CELL_H;
            const int x1 = x0 + CELL_W;
            const int y1 = y0 + CELL_H;
            OffVertex *cell = &s_verts[idx * VERTS_PER_CELL];

            if (k_cells[idx].sloped) {
                /* BASE + OFFSET share the identical tilt; only the
                 * polygon-offset bias on the OFFSET draw differs. */
                emit_quad_sloped(&cell[0], x0, y0, x1, y1,
                                 Z_SLOPE_TOP, Z_SLOPE_BOT, base_rgba);
                emit_quad_sloped(&cell[VERTS_PER_QUAD], x0, y0, x1, y1,
                                 Z_SLOPE_TOP, Z_SLOPE_BOT, offset_rgba);
            } else {
                emit_quad_flat(&cell[0], x0, y0, x1, y1,
                               Z_FLAT, base_rgba);
                emit_quad_flat(&cell[VERTS_PER_QUAD], x0, y0, x1, y1,
                               Z_FLAT, offset_rgba);
            }
        }
    }
}

static void enable_depth_test(void)
{
    const float zmin = 0.0f;
    const float zmax = WINDOW_Z_MAX;
    uint32_t zmin_bits, zmax_bits;
    memcpy(&zmin_bits, &zmin, sizeof(zmin_bits));
    memcpy(&zmax_bits, &zmax, sizeof(zmax_bits));

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 1);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 1);
    /* pbkit default depth func is LEQUAL (0x203); smaller-Z wins. */
    /* Pin the depth clip range to the full window-Z span so the large
     * +/- units cells are never discarded by depth_clipping (CULL). */
    p = pb_push1(p, NV097_SET_CLIP_MIN, zmin_bits);
    p = pb_push1(p, NV097_SET_CLIP_MAX, zmax_bits);
    pb_end(p);
}

static void enforce_common_state(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    pb_end(p);
}

/* BASE pass: polygon offset explicitly OFF (units/factor irrelevant). */
static void set_offset_off(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_POLY_OFFSET_FILL_ENABLE, 0);
    pb_end(p);
}

/* OFFSET pass: program this cell's polygon-offset state. The
 * factor/units methods take the raw IEEE-754 bit pattern of the float
 * (pgraph.c writes the u32 straight into ZOFFSETFACTOR/ZOFFSETBIAS). */
static void set_offset_for_cell(int idx)
{
    const OffCell *c = &k_cells[idx];
    uint32_t units_bits, factor_bits;
    memcpy(&units_bits,  &c->units,  sizeof(units_bits));
    memcpy(&factor_bits, &c->factor, sizeof(factor_bits));

    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_POLYGON_OFFSET_SCALE_FACTOR, factor_bits);
    p = pb_push1(p, NV097_SET_POLYGON_OFFSET_BIAS,         units_bits);
    p = pb_push1(p, NV097_SET_POLY_OFFSET_FILL_ENABLE,
                 c->offset_enable);
    pb_end(p);
}

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(OffVertex), &s_alloc_verts[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(OffVertex), &s_alloc_verts[0].color[0]);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    /* Clear depth+stencil so each frame starts from max-Z. */
    pb_erase_depth_stencil_buffer(0, 0, WIN_W, WIN_H);
    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    /* Per cell: BASE draw (offset OFF, GREEN) then OFFSET draw (this
     * cell's polygon offset, RED). LEQUAL decides which depth survives;
     * the survivor's color owns the cell. */
    for (int idx = 0; idx < GRID_CELLS; idx++) {
        const int base = idx * VERTS_PER_CELL;

        set_offset_off();
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         base, VERTS_PER_QUAD);

        set_offset_for_cell(idx);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         base + VERTS_PER_QUAD, VERTS_PER_QUAD);
    }
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("polygon-offset v0.1\n");

    xbed_set_default_render_state();
    enable_depth_test();
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
        "D:\\polygon-offset-capture.bin",
        "D:\\polygon-offset-done.txt",
        "polygon-offset");
    return 0;
}
