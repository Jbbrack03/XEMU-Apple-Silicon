/*
 * logic-ops — 16 NV2A color logic-op oracle (Tier-1 NV2A diag XBE).
 *
 * NV2A feature exercised: §F.4 (raster-op color logic ops), §3b.3 (op
 *                          enum codes), §H.5 (clear for known DST).
 * NV097 methods:           SET_LOGIC_OP_ENABLE, SET_LOGIC_OP,
 *                          CLEAR_SURFACE (to paint DST), DRAW_ARRAYS
 *                          (per-cell SRC quad), SET_BEGIN_END.
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical; will PASS) +
 *                          math-derived (audit).
 *
 * --- What this XBE tests (and what it documents as unsupported) ----
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.14, this is the spec oracle for
 * the 16 NV2A color-logic ops. The XBE renders a 4x4 grid where each
 * cell:
 *
 *   1. Inherits a known DST color from the back-buffer clear
 *      (0xFF808080, mid-gray uniform: R=G=B=0x80).
 *   2. Issues `SET_LOGIC_OP_ENABLE=1`, `SET_LOGIC_OP=<op>`.
 *   3. Renders a quad with a known SRC color (R=0x40, G=0xC0, B=0x80,
 *      A=0xFF) which the ROP combines with the DST per the op.
 *
 * Expected per-channel: `result = src <op> dst`, computed bitwise per
 * channel. The 16 cells exercise the full GL/D3D logic-op enum:
 *
 *   CLEAR(0), AND(1), AND_REV(2), COPY(3), AND_INV(4), NOOP(5), XOR(6),
 *   OR(7), NOR(8), EQUIV(9), INVERT(10), OR_REV(11), COPY_INV(12),
 *   OR_INV(13), NAND(14), SET(15)
 *
 * --- Renderer-support state (Codex 2026-05-20 confirmed) ----------
 *
 * Neither the GL nor Metal renderer applies SET_LOGIC_OP_ENABLE /
 * SET_LOGIC_OP -- both treat the rasterizer as COPY regardless of the
 * NV2A method values. Vulkan backend hard-codes `VK_LOGIC_OP_COPY`
 * (`hw/xbox/nv2a/pgraph/vk/draw.c:559/917`,
 *  `hw/xbox/nv2a/pgraph/vk/gpuprops.c:183`); GL/Metal don't even read
 * the LOGIC_OP method (`hw/xbox/nv2a/pgraph/pgraph.c:2607/2613` set
 * the pg field but no renderer-side consumer exists). Both renderers
 * therefore produce the SRC color in every cell.
 *
 * `manifest.json::expected_fail_renderers = ["metal", "gl"]` flags
 * this as a known regression target. The harness records the cells
 * as `expected_fail` (not `fail`) so the rotation isn't gated on the
 * missing feature; the XBE serves as a SPEC for the per-op behavior
 * each renderer needs when logic-op support is implemented. Real
 * Xbox hardware is expected to PASS (no renderer override; the NV2A
 * ROP applies the op natively).
 *
 * --- Math derivation (640x480 X8R8G8B8 front buffer) ---------------
 *
 *   col = x / 160 (4 cols)
 *   row = y / 120 (4 rows)
 *   op = row * 4 + col
 *   dst_rgb = (0x80, 0x80, 0x80)  (uniform mid-gray clear)
 *   src_rgb = (0x40, 0xC0, 0x80)
 *   expected_rgb[ch] = LOGIC_OP_TABLE[op](src_rgb[ch], dst_rgb[ch])
 *
 * See expected.py for the per-op math table and per-cell expected
 * pixel computation.
 *
 * --- Geometry layout ------------------------------------------------
 *
 * 4x4 grid; cell_w=160, cell_h=120.
 *
 *   Row 0:  CLEAR    AND     AND_REV   COPY
 *   Row 1:  AND_INV  NOOP    XOR       OR
 *   Row 2:  NOR      EQUIV   INVERT    OR_REV
 *   Row 3:  COPY_INV OR_INV  NAND      SET
 *
 * Each cell is rendered as two triangles (6 verts), SMOOTH-shaded,
 * uniform SRC color across all 6 verts. Issued sequentially per
 * cell so the LOGIC_OP method push lands between cells.
 *
 * --- Reproducibility ------------------------------------------------
 *
 * Pure deterministic pattern; no banner, no counter, no per-frame
 * variation. Byte-identical-after-mask across two cold runs (modulo
 * boundary AA from retina downsample, covered by compare_overrides).
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

/* OpenGL/D3D logic-op enum values; NV2A uses the same. */
#define LOGIC_OP_CLEAR        0x1500
#define LOGIC_OP_AND          0x1501
#define LOGIC_OP_AND_REVERSE  0x1502
#define LOGIC_OP_COPY         0x1503
#define LOGIC_OP_AND_INVERTED 0x1504
#define LOGIC_OP_NOOP         0x1505
#define LOGIC_OP_XOR          0x1506
#define LOGIC_OP_OR           0x1507
#define LOGIC_OP_NOR          0x1508
#define LOGIC_OP_EQUIV        0x1509
#define LOGIC_OP_INVERT       0x150A
#define LOGIC_OP_OR_REVERSE   0x150B
#define LOGIC_OP_COPY_INVERTED 0x150C
#define LOGIC_OP_OR_INVERTED  0x150D
#define LOGIC_OP_NAND         0x150E
#define LOGIC_OP_SET          0x150F

static const uint32_t k_ops[GRID_CELLS] = {
    LOGIC_OP_CLEAR, LOGIC_OP_AND,           LOGIC_OP_AND_REVERSE,  LOGIC_OP_COPY,
    LOGIC_OP_AND_INVERTED, LOGIC_OP_NOOP,   LOGIC_OP_XOR,          LOGIC_OP_OR,
    LOGIC_OP_NOR,          LOGIC_OP_EQUIV,  LOGIC_OP_INVERT,       LOGIC_OP_OR_REVERSE,
    LOGIC_OP_COPY_INVERTED, LOGIC_OP_OR_INVERTED, LOGIC_OP_NAND,   LOGIC_OP_SET,
};

/* SRC color: (R=0x40, G=0xC0, B=0x80, A=0xFF). Chosen so all 4 bit
 * patterns 0/1 appear across the three channels paired with DST=0x80:
 *   R: src=01000000 dst=10000000  → AND=0,  OR=11000000, XOR=11000000.
 *   G: src=11000000 dst=10000000  → AND=10000000, OR=11000000, XOR=01000000.
 *   B: src=10000000 dst=10000000  → AND=10000000, OR=10000000, XOR=00000000.
 * Three different bit-pair shapes (01, 11, 10) per channel against a
 * fixed dst gives the math-derived oracle full discrimination
 * between any two distinct logic ops. */
#define SRC_R 0.25098f   /* 0x40 / 255 */
#define SRC_G 0.75294f   /* 0xC0 / 255 */
#define SRC_B 0.50196f   /* 0x80 / 255 */

#define TRI_VERTS_PER_QUAD 6
typedef struct {
    float pos[3];
    float color[3];
} __attribute__((packed)) ColoredVertex;

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

static void build_quad(int x0, int y0, int x1, int y1)
{
    mk_vert(&s_quad_verts[0], x0, y0, SRC_R, SRC_G, SRC_B);
    mk_vert(&s_quad_verts[1], x1, y0, SRC_R, SRC_G, SRC_B);
    mk_vert(&s_quad_verts[2], x1, y1, SRC_R, SRC_G, SRC_B);
    mk_vert(&s_quad_verts[3], x0, y0, SRC_R, SRC_G, SRC_B);
    mk_vert(&s_quad_verts[4], x1, y1, SRC_R, SRC_G, SRC_B);
    mk_vert(&s_quad_verts[5], x0, y1, SRC_R, SRC_G, SRC_B);
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
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    p = pb_push1(p, NV097_SET_LOGIC_OP_ENABLE, 1);
    pb_end(p);
}

static void set_logic_op(uint32_t op)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_LOGIC_OP, op);
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

static void render_cell(int col, int row, uint32_t op)
{
    const int x0 = col * CELL_W;
    const int y0 = row * CELL_H;
    const int x1 = x0 + CELL_W;
    const int y1 = y0 + CELL_H;
    set_logic_op(op);
    build_quad(x0, y0, x1, y1);
    memcpy(s_alloc_verts, s_quad_verts, sizeof(s_quad_verts));
    bind_attribs();
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, TRI_VERTS_PER_QUAD);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    /* DST = uniform mid-gray (R=G=B=0x80). The logic op combines this
     * with each cell's SRC via the per-cell op. */
    xbed_clear_color_argb(0xFF808080);
    xbed_load_viewport_matrix();
    enforce_common_state();

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            render_cell(col, row, k_ops[idx]);
        }
    }
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("logic-ops v0.1\n");

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
        "D:\\logic-ops-capture.bin",
        "D:\\logic-ops-done.txt",
        "logic-ops");
    return 0;
}
