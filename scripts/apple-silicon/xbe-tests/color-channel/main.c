/*
 * color-channel — RT format and channel ordering oracle (Tier-1).
 *
 * NV2A feature exercised: §F.7 (RT color format),
 *                          §F.3 (color write masks / write order),
 *                          §A.3 (DIFFUSE attribute decode)
 * NV097 methods:           SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET (POSITION
 *                          slot 0 + DIFFUSE slot 3, both TYPE_F),
 *                          DRAW_ARRAYS, SET_BEGIN_END(TRIANGLES)
 * Self-validation tier:    1 (host-side capture; math-derived oracle)
 * Oracle priority:         real-xbox (canonical) + math-derived (audit)
 *
 * Math derivation (640x480 X8R8G8B8 front buffer):
 *
 *   Render four full-height vertical strips, each 160 px wide,
 *   coloring TL/TR/BL/BR (left-to-right):
 *     col [  0, 160) → red    (R=255, G=0,   B=0)
 *     col [160, 320) → green  (R=0,   G=255, B=0)
 *     col [320, 480) → blue   (R=0,   G=0,   B=255)
 *     col [480, 640) → white  (R=255, G=255, B=255)
 *
 *   Expected per-pixel output:
 *     pixel (0..159, *)   = 0xFFFF0000
 *     pixel (160..319, *) = 0xFF00FF00
 *     pixel (320..479, *) = 0xFF0000FF
 *     pixel (480..639, *) = 0xFFFFFFFF
 *
 *   Each strip is a quad (two triangles, OP_TRIANGLES) with vertices
 *   at integer pixel corners. Under the d3d top-left fill convention
 *   NV2A inherits, the strips tile cleanly without overlap or gaps.
 *
 *   This test catches the SC2 "wrong colors" symptom: a renderer that
 *   swaps R/B (BGRA-vs-RGBA mismatch in the front-buffer publish path
 *   or in the DIFFUSE → COLOR fixed-function passthrough) would show
 *   the strips as blue / green / red / white instead of
 *   red / green / blue / white.
 *
 * Reproducibility: byte-identical-after-mask across two cold runs.
 * Pure deterministic pattern, no banner, no timestamps.
 *
 * Future extension: a sibling `color-channel-d3d` XBE will repeat
 * this with TYPE_UB_D3D attribute format to validate format decode.
 * This XBE only validates the F (4×float) path.
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

/* Map window-coord (x, y) to clip-space (cx, cy). The lib's viewport
 * matrix maps cx → (cx+1)*320, cy → (1-cy)*240, so:
 *   cx = x/320 - 1
 *   cy = 1 - y/240
 *
 * For a strip spanning x in [x0, x1] and y in [0, 480]:
 *   cx0 = x0/320 - 1, cx1 = x1/320 - 1
 *   cy_top = 1 - 0/240 = 1
 *   cy_bot = 1 - 480/240 = -1
 *
 * Six vertices per strip (two triangles via OP_TRIANGLES). */
#define V_TL(x_w, y_w, r, g, b) {{(float)(x_w)/320.0f - 1.0f, 1.0f - (float)(y_w)/240.0f, 0.0f}, {r, g, b}}

static const ColoredVertex k_verts[4 * 6] = {
    /* Strip 0: red,    cols [0, 160) */
    V_TL(  0,   0, 1.0f, 0.0f, 0.0f),
    V_TL(160,   0, 1.0f, 0.0f, 0.0f),
    V_TL(160, 480, 1.0f, 0.0f, 0.0f),
    V_TL(  0,   0, 1.0f, 0.0f, 0.0f),
    V_TL(160, 480, 1.0f, 0.0f, 0.0f),
    V_TL(  0, 480, 1.0f, 0.0f, 0.0f),

    /* Strip 1: green,  cols [160, 320) */
    V_TL(160,   0, 0.0f, 1.0f, 0.0f),
    V_TL(320,   0, 0.0f, 1.0f, 0.0f),
    V_TL(320, 480, 0.0f, 1.0f, 0.0f),
    V_TL(160,   0, 0.0f, 1.0f, 0.0f),
    V_TL(320, 480, 0.0f, 1.0f, 0.0f),
    V_TL(160, 480, 0.0f, 1.0f, 0.0f),

    /* Strip 2: blue,   cols [320, 480) */
    V_TL(320,   0, 0.0f, 0.0f, 1.0f),
    V_TL(480,   0, 0.0f, 0.0f, 1.0f),
    V_TL(480, 480, 0.0f, 0.0f, 1.0f),
    V_TL(320,   0, 0.0f, 0.0f, 1.0f),
    V_TL(480, 480, 0.0f, 0.0f, 1.0f),
    V_TL(320, 480, 0.0f, 0.0f, 1.0f),

    /* Strip 3: white,  cols [480, 640) */
    V_TL(480,   0, 1.0f, 1.0f, 1.0f),
    V_TL(640,   0, 1.0f, 1.0f, 1.0f),
    V_TL(640, 480, 1.0f, 1.0f, 1.0f),
    V_TL(480,   0, 1.0f, 1.0f, 1.0f),
    V_TL(640, 480, 1.0f, 1.0f, 1.0f),
    V_TL(480, 480, 1.0f, 1.0f, 1.0f),
};

static ColoredVertex *s_alloc_vertices;

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;
    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_vertices[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_vertices[0].color[0]);
    /* All 24 vertices in one draw call — strips don't overlap so
     * order-of-rasterization doesn't matter. */
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, 24);
}

int main(void)
{
    if (xbed_init(640, 480) != XBED_OK) return 1;

    debugPrint("color-channel v0.1\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    s_alloc_vertices = MmAllocateContiguousMemoryEx(
        sizeof(k_verts), 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_alloc_vertices) {
        debugPrint("MmAllocateContiguousMemoryEx failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    memcpy(s_alloc_vertices, k_verts, sizeof(k_verts));

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\color-channel-capture.bin",
        "D:\\color-channel-done.txt",
        "color-channel");
    return 0;
}
