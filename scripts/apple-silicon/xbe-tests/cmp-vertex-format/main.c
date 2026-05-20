/*
 * cmp-vertex-format — packed (11,11,10) CMP decoder oracle (Tier-1 NV2A diag XBE).
 *
 * NV2A feature exercised: §A.3 (vertex attribute formats — CMP). The
 *                          streamed-array decoder path (DRAW_ARRAYS
 *                          from VRAM), NOT inline-element. §3b.5
 *                          covers the inline path separately.
 * NV097 methods:           SET_VERTEX_DATA_ARRAY_FORMAT (TYPE_CMP for
 *                          attribute 3 = DIFFUSE; TYPE_F for attribute
 *                          0 = POSITION), SET_VERTEX_DATA_ARRAY_OFFSET,
 *                          SET_BEGIN_END(TRIANGLES), DRAW_ARRAYS,
 *                          CLEAR_SURFACE (via pb_fill).
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.6, this is the oracle for the
 * NV2A CMP vertex attribute format -- 3 signed-normalized components
 * packed into a single 32-bit word with bit layout (LSB first):
 *
 *   X bits 0-10  (11-bit signed), normalized by 1023.
 *   Y bits 11-21 (11-bit signed), normalized by 1023.
 *   Z bits 22-31 (10-bit signed), normalized by 511.
 *
 * The decoded value is clamped to [-1, +1] in each renderer's
 * decoder path (see `pgraph/vertex.c:56-75`, `pgraph/mtl/vertex.c:130-157`
 * for the CPU paths; `pgraph/glsl/vsh.c:203-208` for the GL GLSL path).
 *
 * The test emits 8 cells, each filled with a different CMP-encoded
 * value at one of the 8 ±1 corners of the unit cube. The DIFFUSE
 * attribute (#3) carries the CMP value; the passthrough VS routes
 * DIFFUSE -> COLOR; the pixel shader emits COLOR; the framebuffer
 * clamps [0, 1] (so any decoded component of -1 lands as 0 = BLACK).
 *
 * 8 corners of the RGB cube -- saturated 0/255 outputs only:
 *
 *   Row 0: (+1,+1,+1)=WHITE   (+1,+1,-1)=YELLOW   (+1,-1,+1)=MAGENTA  (-1,+1,+1)=CYAN
 *   Row 1: (+1,-1,-1)=RED     (-1,+1,-1)=GREEN    (-1,-1,+1)=BLUE     (-1,-1,-1)=BLACK
 *
 * --- CMP encoding worked example (matches XBE-side k_cmp_normals) ---
 *
 * For normal = (+1, +1, +1):
 *   X = 1023 = 0x3FF (max +ve 11-bit signed). Bits 0-10.
 *   Y = 1023 = 0x3FF shifted left 11.
 *   Z =  511 = 0x1FF shifted left 22.
 *   Encoded word = 0x3FF | (0x3FF << 11) | (0x1FF << 22) = 0x7FDFFBFF.
 *
 * For normal = (-1, -1, -1):
 *   X = -1024 (min 11-bit signed) = 0x400 in 11-bit two's complement.
 *   Y = -1024 = 0x400 shifted left 11.
 *   Z =  -512 = 0x200 in 10-bit two's complement, shifted left 22.
 *   Encoded word = 0x400 | (0x400 << 11) | (0x200 << 22) = 0x80200400.
 *   Decoded: -1024/1023 = -1.001 clamped to -1; -512/511 = -1.002 clamped
 *   to -1. Output color clamped to (0, 0, 0) = BLACK.
 *
 * Mixed signs follow the same encoding rule: positive component bits
 * are 0x3FF (X/Y) or 0x1FF (Z); negative component bits are 0x400 (X/Y)
 * or 0x200 (Z). The XBE's `k_cmp_normals` constant table carries all
 * 8 pre-encoded words so the in-frame draw needs no per-frame math.
 *
 * --- Why no (normal+1)*0.5 projection ------------------------------
 *
 * The §4.6 spec describes the test as "VS projects normal to color via
 * (normal+1)*0.5; sample expected color per encoded input; tolerance
 * ±1 LSB." That projection avoids ambiguity between negative and zero
 * normal components in the output color. This XBE instead chooses the
 * 8 ±1-corner encodings which produce saturated 0/255 outputs after
 * the framebuffer's natural [0, 1] clamp -- no projection needed and
 * no ±1 LSB tolerance required. The result is byte-exact across
 * renderers regardless of any display-side gamma table (same lesson
 * as depth-floor v0.2). Intermediate-normal coverage with the
 * (normal+1)*0.5 projection is filed as a second-wave follow-up XBE.
 *
 * --- Why OP_TRIANGLES, not OP_QUADS --------------------------------
 *
 * The §4.5 `native-quad-tri-depth` XBE proved Metal does not yet
 * support FLAT-shaded OP_QUADS (task #13 tracks the renderer fix).
 * This XBE uses OP_TRIANGLES + SMOOTH shading to avoid that gap and
 * keep the test focused on the CMP decoder. Each cell is two
 * triangles in the A-B-C / A-C-D diagonalization; all 6 verts carry
 * the same CMP value so smooth interpolation produces a uniform cell.
 *
 * --- Geometry layout ------------------------------------------------
 *
 * WIN_W=640, WIN_H=480. 4 cols x 2 rows = 8 cells; cell_w=160, cell_h=240.
 *
 *   col*160       (col+1)*160
 *      |             |
 *   y=0  +-----+-----+-----+-----+
 *        | W   | Y   | M   | C   |  (row 0)
 * y=240  +-----+-----+-----+-----+
 *        | R   | G   | B   | K   |  (row 1)
 *   y=480+-----+-----+-----+-----+
 *
 *
 * --- Math derivation (640x480 X8R8G8B8 front buffer) ----------------
 *
 * For pixel (x, y) in [0, 640) x [0, 480):
 *   col = x / 160
 *   row = y / 240
 *   normal = k_cmp_decoded[row * 4 + col]
 *   expected_rgb = clamp(normal, [0, 1]) * 255  (component-wise)
 *
 * pb_fill(0, 0, W, H, BG) clears to opaque black 0xFF000000 before
 * any draw. Cell quads cover every pixel exactly (4*160=640, 2*240=480).
 *
 * Catches (endpoint-only oracle limits documented):
 *   - CMP bitfield range errors (wrong X/Y/Z bit widths or shift
 *     offsets): would produce wrong cell colors at multiple corners
 *     because a misshifted field reads neighboring components' bits.
 *   - Sign-extension bug: the (-1, -1, -1) BLACK cell would decode
 *     positively without sign-extend (large positive numbers) and
 *     the output would saturate to non-BLACK after framebuffer clamp.
 *   - Component-ordering bug (X and Z swapped, etc.): would produce
 *     wrong cell colors at the asymmetric corners (e.g. RED ↔ BLUE).
 *   - GL: GLSL shader-side bitfieldExtract path
 *     (pgraph/glsl/vsh.c:203-208) -- streamed CMP attributes go
 *     through this on GL.
 *   - Metal: CPU-side decoder in `pgraph/mtl/vertex.c:130-157` --
 *     streamed CMP attributes are pre-decoded to Float3 on Metal.
 *
 * Does NOT catch (fundamental byte-quantization limits, not test
 * design):
 *   - Sub-LSB divisor errors. 1023 vs 1024 divisor at the maximum
 *     positive encoding (1023): 1023/1023=1.0 vs 1023/1024=0.999,
 *     ~0.25 LSB difference -- invisible at 8-bit framebuffer
 *     quantization.
 *   - Missing decoder-side clamp of the slightly-out-of-range -1024/
 *     1023 ≈ -1.001 and -512/511 ≈ -1.002 values: the framebuffer's
 *     [0, 1] clamp produces 0 regardless of whether the decoder
 *     clamped to -1 first.
 *
 * Intermediate-normal coverage with a custom VS implementing
 * (normal+1)*0.5 + ±1 LSB tolerance (per the original §4.6 spec) is
 * filed as a second-wave follow-up that would catch a small-scale
 * mid-range error not visible at the ±1 corners.
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

#define GRID_COLS 4
#define GRID_ROWS 2
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define CELL_W (WIN_W / GRID_COLS)
#define CELL_H (WIN_H / GRID_ROWS)

#define TRI_VERTS_PER_CELL 6
#define TRI_VERTS_TOTAL    (GRID_CELLS * TRI_VERTS_PER_CELL)

/* Vertex layout: POSITION (Float3) at slot 0, DIFFUSE (CMP packed
 * uint32) at slot 3. Note the DIFFUSE is a single uint32 per vertex
 * because CMP encodes all 3 components in one word (TYPE_CMP requires
 * size=1, see hw/xbox/nv2a/pgraph/gl/vertex.c:124-129 + assert(count==1)).
 *
 * Stride includes both attributes' worth of bytes -- the vertex puller
 * fetches POSITION from offset 0, DIFFUSE from offset 12, with stride
 * 16. */
typedef struct {
    float    pos[3];        /* 12 bytes */
    uint32_t cmp_diffuse;   /* 4 bytes — packed (11,11,10) signed-normalized */
} __attribute__((packed)) CmpVertex;

/* CMP encoding helpers. Each component is signed in its respective
 * field width (11-bit X/Y, 10-bit Z) and stored in two's complement.
 * +1 → max positive (1023 for X/Y, 511 for Z); -1 → min negative
 * (1024 → 0x400 for X/Y, 512 → 0x200 for Z; these decode to <-1 and
 * are clamped to -1 by the decoder). */
#define CMP_PACK(x11, y11, z10) \
    (((uint32_t)(x11) & 0x7FFu)         | \
     (((uint32_t)(y11) & 0x7FFu) << 11) | \
     (((uint32_t)(z10) & 0x3FFu) << 22))

/* 8 CMP-encoded normals at the ±1 corners of the unit cube.
 * Per-cell, indexed by row * 4 + col. */
static const uint32_t k_cmp_normals[GRID_CELLS] = {
    /* Row 0 */
    CMP_PACK(0x3FF, 0x3FF, 0x1FF),  /* (+1, +1, +1) → WHITE   */
    CMP_PACK(0x3FF, 0x3FF, 0x200),  /* (+1, +1, -1) → YELLOW  */
    CMP_PACK(0x3FF, 0x400, 0x1FF),  /* (+1, -1, +1) → MAGENTA */
    CMP_PACK(0x400, 0x3FF, 0x1FF),  /* (-1, +1, +1) → CYAN    */
    /* Row 1 */
    CMP_PACK(0x3FF, 0x400, 0x200),  /* (+1, -1, -1) → RED     */
    CMP_PACK(0x400, 0x3FF, 0x200),  /* (-1, +1, -1) → GREEN   */
    CMP_PACK(0x400, 0x400, 0x1FF),  /* (-1, -1, +1) → BLUE    */
    CMP_PACK(0x400, 0x400, 0x200),  /* (-1, -1, -1) → BLACK   */
};

/* Window (x_w, y_w) → clip (cx, cy). Same formula as depth-floor /
 * native-quad-tri-depth. */
static inline void mk_pos(float pos[3], int x_w, int y_w)
{
    pos[0] = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    pos[1] = 1.0f - (float)y_w / (float)(WIN_H / 2);
    pos[2] = 0.5f;
}

/* Emit one cell as two triangles in A-B-C / A-C-D winding. All 6
 * verts carry the SAME CMP-encoded diffuse value so smooth
 * interpolation across the cell yields a uniform color. */
static void emit_cell_tris(CmpVertex *out, int x0, int y0, int x1, int y1,
                           uint32_t cmp)
{
    /* Triangle 1: TL, TR, BR. */
    mk_pos(out[0].pos, x0, y0); out[0].cmp_diffuse = cmp;
    mk_pos(out[1].pos, x1, y0); out[1].cmp_diffuse = cmp;
    mk_pos(out[2].pos, x1, y1); out[2].cmp_diffuse = cmp;
    /* Triangle 2: TL, BR, BL. */
    mk_pos(out[3].pos, x0, y0); out[3].cmp_diffuse = cmp;
    mk_pos(out[4].pos, x1, y1); out[4].cmp_diffuse = cmp;
    mk_pos(out[5].pos, x0, y1); out[5].cmp_diffuse = cmp;
}

static CmpVertex s_verts[TRI_VERTS_TOTAL];
static CmpVertex *s_alloc_verts;

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
            emit_cell_tris(s_verts + q, x0, y0, x1, y1,
                           k_cmp_normals[idx]);
            q += TRI_VERTS_PER_CELL;
        }
    }
}

static void enforce_diag_state(void)
{
    uint32_t *p = pb_begin();
    /* Smooth shading: all 4 corners of each cell are the same color so
     * the interpolation is trivially uniform; using SMOOTH avoids any
     * provoking-vertex selection variable. */
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    /* FILL both faces; depth off (no overdraw to test). */
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
    pb_end(p);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();

    xbed_clear_all_attribs_to_float();
    /* Position (slot 0): TYPE_F, 3 components, stride = sizeof(CmpVertex). */
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(CmpVertex), &s_alloc_verts[0].pos[0]);
    /* Diffuse (slot 3): TYPE_CMP, count=1 (per the renderer's
     * assert(attr->count == 1) at gl/vertex.c:127). Each vertex
     * carries one packed uint32. Stride still sizeof(CmpVertex). */
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP, 1,
        sizeof(CmpVertex), &s_alloc_verts[0].cmp_diffuse);

    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, TRI_VERTS_TOTAL);
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("cmp-vertex-format v0.1\n");

    xbed_set_default_render_state();
    enforce_diag_state();
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
        "D:\\cmp-vertex-format-capture.bin",
        "D:\\cmp-vertex-format-done.txt",
        "cmp-vertex-format");
    return 0;
}
