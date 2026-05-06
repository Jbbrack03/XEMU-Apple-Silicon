/*
 * depth-floor — depth test + native_tri_depth oracle (Tier-1).
 *
 * NV2A feature exercised: §G.1 (depth test enable), §G.6 (depth write
 *                          mask), §K.2 (depth-test pass/fail visibility),
 *                          §H.5 (depth-buffer clear)
 * NV097 methods:           SET_DEPTH_TEST_ENABLE, SET_DEPTH_MASK,
 *                          SET_DEPTH_FUNC (pbkit default LEQUAL),
 *                          SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET,
 *                          DRAW_ARRAYS, SET_BEGIN_END(TRIANGLES),
 *                          pb_erase_depth_stencil_buffer
 * Self-validation tier:    1 (host-side capture; math-derived oracle)
 * Oracle priority:         real-xbox (canonical) + math-derived (audit)
 *
 * Math derivation (640x480 X8R8G8B8 front buffer):
 *
 *   The XBE renders TWO full-screen-aligned quads in order:
 *
 *     1. FLOOR — full screen (640x480) at clip z=0.5, color
 *        white = (R=1, G=1, B=1).
 *
 *     2. WALL  — bottom half (window y in [240, 480]) at clip z=0.0
 *        (closer than floor), color blue = (R=0, G=0, B=1).
 *
 *   With LEQUAL depth test enabled, the wall (z=0.0) WINS over the
 *   floor (z=0.5) wherever both are drawn. Expected per-pixel output:
 *
 *     y in [0,   240) → 0xFFFFFFFF (white) — floor visible
 *     y in [240, 480) → 0xFF0000FF (blue)  — wall wins
 *
 *   All values are 0 or 255 — no intermediate per-cell colors — so
 *   the result is byte-exact across renderers regardless of any
 *   display-side gamma table the BIOS or kernel sets at video init.
 *
 *   Catches:
 *     - Depth test disabled → wall (drawn last) wins everywhere = all
 *       blue (FAIL).
 *     - Depth func reversed (GEQUAL instead of LEQUAL) → floor wins
 *       everywhere = all white (FAIL).
 *     - Y-mirror in renderer or front-buffer publisher → blue half
 *       lands at top instead of bottom (FAIL).
 *     - Native_tri_depth path producing wrong per-fragment Z → wall
 *       loses to floor in the bottom half (no blue visible) (FAIL).
 *
 * Reproducibility: byte-identical-after-mask across runs. Pure
 * deterministic pattern, no banner.
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

typedef struct {
    float pos[3];
    float color[3];
} __attribute__((packed)) ColoredVertex;

#define WIN_W 640
#define WIN_H 480

/* Window (x_w, y_w, z_clip) → clip (cx, cy, z_clip). */
static inline void mk_vert(ColoredVertex *v, int x_w, int y_w, float z_clip,
                           float r, float g, float b)
{
    v->pos[0] = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1] = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2] = z_clip;
    v->color[0] = r;
    v->color[1] = g;
    v->color[2] = b;
}

static void mk_quad(ColoredVertex *out, int x0, int y0, int x1, int y1,
                    float z_clip, float r, float g, float b)
{
    /* Two triangles, OP_TRIANGLES — six verts. */
    mk_vert(out + 0, x0, y0, z_clip, r, g, b);
    mk_vert(out + 1, x1, y0, z_clip, r, g, b);
    mk_vert(out + 2, x1, y1, z_clip, r, g, b);
    mk_vert(out + 3, x0, y0, z_clip, r, g, b);
    mk_vert(out + 4, x1, y1, z_clip, r, g, b);
    mk_vert(out + 5, x0, y1, z_clip, r, g, b);
}

#define FLOOR_VERTS 6
#define WALL_VERTS  6

static ColoredVertex s_verts[FLOOR_VERTS + WALL_VERTS];
static ColoredVertex *s_alloc_vertices;

static void build_geometry(void)
{
    int idx = 0;
    /* Floor — full screen at z=0.5, white. */
    mk_quad(s_verts + idx, 0, 0, WIN_W, WIN_H, 0.5f, 1.0f, 1.0f, 1.0f);
    idx += FLOOR_VERTS;
    /* Wall — bottom half at z=0.0 (closer), blue. */
    mk_quad(s_verts + idx, 0, WIN_H / 2, WIN_W, WIN_H, 0.0f, 0.0f, 0.0f, 1.0f);
    idx += WALL_VERTS;
    (void)idx;
}

static void enable_depth_test(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 1);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 1);
    /* pbkit default depth func is LEQUAL (0x203); smaller-Z wins. */
    pb_end(p);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;
    /* Clear depth+stencil first so the test starts from a max-Z state. */
    pb_erase_depth_stencil_buffer(0, 0, WIN_W, WIN_H);
    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_vertices[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_vertices[0].color[0]);
    /* Floor first (z=0.5), wall second (z=0.0). With LEQUAL the wall
     * wins where both draw. Single draw call covering both. */
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0,
                     FLOOR_VERTS + WALL_VERTS);
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("depth-floor v0.2 (saturated-colors)\n");

    xbed_set_default_render_state();
    enable_depth_test();
    xbed_load_default_shaders();

    build_geometry();
    s_alloc_vertices = MmAllocateContiguousMemoryEx(
        sizeof(s_verts), 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_alloc_vertices) {
        debugPrint("MmAllocateContiguousMemoryEx failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    memcpy(s_alloc_vertices, s_verts, sizeof(s_verts));

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\depth-floor-capture.bin",
        "D:\\depth-floor-done.txt",
        "depth-floor");
    return 0;
}
