/*
 * mirror — pixel-position oracle (Tier-1 NV2A diag XBE).
 *
 * NV2A feature exercised: §A.6 (Z perspective), §C.8 (rasterization
 *                          fill convention), §J.1, §J.2 (front-buffer
 *                          orientation / Y-axis convention)
 * NV097 methods:           SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET,
 *                          DRAW_ARRAYS, SET_BEGIN_END(TRIANGLES)
 * Self-validation tier:    1 (host-side capture; math-derived oracle)
 * Oracle priority:         real-xbox (canonical) + math-derived (audit)
 *
 * Math derivation (640x480 X8R8G8B8 front buffer):
 *
 *   - Background: pb_fill the back buffer to 0xFF000000 (opaque
 *     black). After the post-frame swap the front buffer is opaque
 *     black everywhere except the one rendered triangle pair.
 *
 *   - Foreground: a 4x4 white (0xFFFFFFFF) square at top-left pixel
 *     (318, 48) covering pixels (318..321, 48..51). Choosing a
 *     4x4 patch instead of a literal "single pixel" eliminates
 *     ambiguity around sub-pixel rasterization rules: a quad
 *     bordered by integer pixel corners (318, 48)-(322, 52) covers
 *     exactly the half-open rectangle [318, 322) x [48, 52) under
 *     the d3d top-left fill convention NV2A inherits, regardless of
 *     which renderer (real Xbox / xemu-GL / xemu-Metal) is doing
 *     the rasterization.
 *
 *   - Quad → two triangles via OP_TRIANGLES at clip-space coords:
 *       v0 (318, 48) → cx = -2/320, cy = 1 - 48/240 = 0.8
 *       v1 (322, 48) → cx = +2/320, cy = 0.8
 *       v2 (322, 52) → cx = +2/320, cy = 1 - 52/240 = 188/240
 *       v3 (318, 52) → cx = -2/320, cy = 188/240
 *     Triangles: (v0, v1, v2) + (v0, v2, v3). Diffuse (1,1,1) for
 *     all six vertices. The lib's passthrough VS multiplies by the
 *     standard window-coord viewport matrix loaded by
 *     xbed_load_viewport_matrix(); the lib's passthrough PS returns
 *     the interpolated DIFFUSE.
 *
 *   - Mirror-bug detection (informational; harness is the authority):
 *     a Y-mirror bug in the renderer or front-buffer publisher
 *     would draw the white patch at row (480 - 1 - 50) = 429 instead
 *     of row 50. The expected output has those rows BLACK; FAIL
 *     case shows them WHITE.
 *
 * Reproducibility: byte-identical-after-mask across two cold runs;
 * no banner, no timestamps, no frame counter; pure deterministic
 * pattern.
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

/* Back buffer is 640x480; the lib's viewport matrix maps clip-space
 * (cx, cy) → window (x, y) via x=(cx+1)*320, y=(1-cy)*240.
 * Vertices computed for a 4x4 square at window (318..321, 48..51). */
static const ColoredVertex k_verts[6] = {
    /* tri 1: v0, v1, v2 */
    {{ -2.0f / 320.0f,  192.0f / 240.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }}, /* v0 (318, 48) */
    {{ +2.0f / 320.0f,  192.0f / 240.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }}, /* v1 (322, 48) */
    {{ +2.0f / 320.0f,  188.0f / 240.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }}, /* v2 (322, 52) */
    /* tri 2: v0, v2, v3 */
    {{ -2.0f / 320.0f,  192.0f / 240.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }}, /* v0 dup */
    {{ +2.0f / 320.0f,  188.0f / 240.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }}, /* v2 dup */
    {{ -2.0f / 320.0f,  188.0f / 240.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }}, /* v3 (318, 52) */
};

static ColoredVertex *s_alloc_vertices;

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;
    xbed_clear_color_argb(0xFF000000); /* opaque black background */
    xbed_load_viewport_matrix();
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_vertices[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_vertices[0].color[0]);
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, 6);
}

int main(void)
{
    if (xbed_init(640, 480) != XBED_OK) return 1;

    debugPrint("mirror v0.1\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    /* Allocate the vertex array in PAGE_WRITECOMBINE memory so the
     * GPU's vertex puller sees the data without an explicit cache
     * flush. Same pattern flat-tri-depth uses. */
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

    /* Render the same pattern N times so the host (xemu) has plenty
     * of frames to land its in-renderer screenshot capture, then
     * write capture.bin + reboot so real-Xbox FTP-collect works. */
    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\mirror-capture.bin",
        "D:\\mirror-done.txt",
        "mirror");
    return 0;
}
