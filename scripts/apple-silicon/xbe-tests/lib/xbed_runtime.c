/*
 * xbed_runtime — implementation. See xbed_runtime.h for the contract.
 *
 * Source-of-truth for the boilerplate is the existing
 * `flat-tri-depth/main.c` (already known-good on real Xbox + xemu).
 * This lib factors that pattern out so each new diag XBE only writes
 * test-specific render code.
 */
#include "xbed_runtime.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define XBED_MASK(mask, val) (((val) << (ffs(mask) - 1)) & (mask))

static int  s_back_w = 0;
static int  s_back_h = 0;
static float s_viewport[4][4];

static void s_compute_viewport(int width, int height)
{
    /* Same formula as `nxdk/samples/triangle/main.c::matrix_viewport`.
     * Maps clip-space [-1,1] to window coords (origin top-left, Y down,
     * Z in [0, 65536]). */
    memset(s_viewport, 0, sizeof(s_viewport));
    s_viewport[0][0] = (float)width / 2.0f;
    s_viewport[1][1] = (float)height / -2.0f;
    s_viewport[2][2] = 65536.0f - 0.0f;
    s_viewport[3][3] = 1.0f;
    s_viewport[3][0] = (float)width / 2.0f;
    s_viewport[3][1] = (float)height / 2.0f;
    s_viewport[3][2] = 0.0f;
}

xbed_status_t xbed_init(int width, int height)
{
    XVideoSetMode(width, height, 32, REFRESH_DEFAULT);

    int rc = pb_init();
    if (rc != 0) {
        debugPrint("xbed_init: pb_init -> %d\n", rc);
        Sleep(2000);
        return XBED_FAIL_INIT;
    }

    pb_show_front_screen();

    s_back_w = (int)pb_back_buffer_width();
    s_back_h = (int)pb_back_buffer_height();
    s_compute_viewport(s_back_w, s_back_h);
    return XBED_OK;
}

void xbed_shutdown(void)
{
    pb_show_debug_screen();
    pb_kill();
}

int xbed_back_buffer_width(void)  { return s_back_w; }
int xbed_back_buffer_height(void) { return s_back_h; }

void xbed_frame_begin(void)
{
    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();
}

void xbed_frame_end_and_swap(void)
{
    /* Flush GPU, then wait for the swap to schedule. Mirrors the
     * triangle sample's tail end. */
    while (pb_busy()) { /* spin */ }
    while (pb_finished()) { /* spin until ready to swap */ }
}

void xbed_clear_color_argb(uint32_t argb)
{
    pb_fill(0, 0, s_back_w, s_back_h, argb);
}

void xbed_set_default_render_state(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BLEND_ENABLE,        0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE,   0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE,    0);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE,   0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK,          0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_SHADE_MODEL,
                 NV097_SET_SHADE_MODEL_SMOOTH);
    pb_end(p);
}

void xbed_load_viewport_matrix(void)
{
    uint32_t *p = pb_begin();

    p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96);
    pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, 16);
    memcpy(p, s_viewport, sizeof(s_viewport));
    p += 16;

    pb_end(p);
}

void xbed_load_default_shaders(void)
{
    /* The lib provides the same passthrough shader pair the
     * triangle sample uses: clip-space POSITION × viewport-matrix → window
     * coords; DIFFUSE → COLOR. The .inl files are produced by the
     * Cg compiler against `lib/vs.vs.cg` and `lib/ps.ps.cg`. */
    static const uint32_t vs_program[] = {
#include "vs.inl"
    };
    uint32_t *p;
    int i;

    p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 XBED_MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE,
                           NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM)
                 | XBED_MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
                             NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
    pb_end(p);

    p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    pb_end(p);

    for (i = 0; i < (int)(sizeof(vs_program) / 16); i++) {
        p = pb_begin();
        pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
        memcpy(p, &vs_program[i * 4], 4 * 4);
        p += 4;
        pb_end(p);
    }

    p = pb_begin();
#include "ps.inl"
    pb_end(p);
}

void xbed_clear_all_attribs_to_float(void)
{
    uint32_t *p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (int i = 0; i < 16; i++) {
        *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
    }
    pb_end(p);
}

void xbed_set_attrib_pointer(unsigned index, unsigned format,
                             unsigned size, unsigned stride,
                             const void *data)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
                 XBED_MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE,   format)
                 | XBED_MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE,   size)
                 | XBED_MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4,
                 (uint32_t)data & 0x03ffffff);
    pb_end(p);
}

void xbed_draw_arrays(unsigned mode, int start, int count)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, mode);
    p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
                 XBED_MASK(NV097_DRAW_ARRAYS_COUNT,        count - 1)
                 | XBED_MASK(NV097_DRAW_ARRAYS_START_INDEX, start));
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}
