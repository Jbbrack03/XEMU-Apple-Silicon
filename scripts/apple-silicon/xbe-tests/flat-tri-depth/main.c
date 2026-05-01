/*
 * Minimal NV2A flat-shading diagnostic for xemu's native triangle-depth path.
 *
 * The app alternates every 240 frames between a first-vertex flat-shaded
 * triangle, which should be eligible for the native path, and a last-vertex
 * flat-shaded triangle, which should continue to require the geometry-shader
 * fallback.
 */
#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

#define MASK(mask, val) (((val) << (ffs(mask) - 1)) & (mask))

typedef struct {
    float pos[3];
    float color[3];
} __attribute__((packed)) ColoredVertex;

static const ColoredVertex kVerts[] = {
    /* First-provoking flat-shaded triangle: should render red. */
    {{-0.88f, -0.78f, 1.0f}, {1.0f, 0.0f, 0.0f}},
    {{-0.08f, -0.78f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{-0.48f,  0.78f, 1.0f}, {0.0f, 0.0f, 1.0f}},

    /* Last-provoking flat-shaded triangle: should render cyan. */
    {{ 0.08f, -0.78f, 1.0f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.88f, -0.78f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.48f,  0.78f, 1.0f}, {0.0f, 1.0f, 1.0f}},
};

static ColoredVertex *alloc_vertices;
static float m_viewport[4][4];

static void matrix_viewport(float out[4][4], float x, float y, float width,
                            float height, float z_min, float z_max);
static void init_shader(void);
static void init_render_state(void);
static void set_shader_constants(void);
static void set_vertex_arrays(void);
static void set_attrib_pointer(unsigned int index, unsigned int format,
                               unsigned int size, unsigned int stride,
                               const void *data);
static void draw_arrays(unsigned int mode, int start, int count);
static void draw_flat_triangle_phase(uint32_t frame);

int main(void)
{
    int status;
    int width;
    int height;
    uint32_t frame;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    status = pb_init();
    if (status) {
        Sleep(2000);
        return 1;
    }

    pb_show_front_screen();

    width = pb_back_buffer_width();
    height = pb_back_buffer_height();

    init_render_state();
    init_shader();

    alloc_vertices = MmAllocateContiguousMemoryEx(
        sizeof(kVerts), 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    memcpy(alloc_vertices, kVerts, sizeof(kVerts));
    matrix_viewport(m_viewport, 0, 0, width, height, 0, 65536.0f);
    frame = 0;

    for (;;) {
        pb_wait_for_vbl();
        pb_reset();
        pb_target_back_buffer();

        pb_erase_depth_stencil_buffer(0, 0, width, height);
        pb_fill(0, 0, width, height, 0x00000000);

        while (pb_busy()) {
        }

        set_shader_constants();
        set_vertex_arrays();
        draw_flat_triangle_phase(frame++);

        while (pb_busy()) {
        }

        while (pb_finished()) {
        }
    }

    return 0;
}

static void matrix_viewport(float out[4][4], float x, float y, float width,
                            float height, float z_min, float z_max)
{
    memset(out, 0, 4 * 4 * sizeof(float));
    out[0][0] = width / 2.0f;
    out[1][1] = height / -2.0f;
    out[2][2] = z_max - z_min;
    out[3][3] = 1.0f;
    out[3][0] = x + width / 2.0f;
    out[3][1] = y + height / 2.0f;
    out[3][2] = z_min;
}

static void init_shader(void)
{
    uint32_t vs_program[] = {
#include "vs.inl"
    };
    uint32_t *p;
    int i;

    p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE,
                      NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                     MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
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

static void init_render_state(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_FLAT);
    pb_end(p);
}

static void set_shader_constants(void)
{
    uint32_t *p = pb_begin();

    p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96);
    pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, 16);
    memcpy(p, m_viewport, 16 * 4);
    p += 16;

    pb_end(p);
}

static void set_vertex_arrays(void)
{
    uint32_t *p;
    int i;

    p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (i = 0; i < 16; i++) {
        *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
    }
    pb_end(p);

    set_attrib_pointer(0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
                       sizeof(ColoredVertex), &alloc_vertices[0].pos[0]);
    set_attrib_pointer(3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
                       sizeof(ColoredVertex), &alloc_vertices[0].color[0]);
}

static void set_attrib_pointer(unsigned int index, unsigned int format,
                               unsigned int size, unsigned int stride,
                               const void *data)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, format) |
                     MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size) |
                     MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4,
                 (uint32_t)data & 0x03ffffff);
    pb_end(p);
}

static void draw_arrays(unsigned int mode, int start, int count)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, mode);
    p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
                 MASK(NV097_DRAW_ARRAYS_COUNT, count - 1) |
                     MASK(NV097_DRAW_ARRAYS_START_INDEX, start));
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}

static void draw_flat_triangle_phase(uint32_t frame)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_FLAT);

    if ((frame / 240) & 1) {
        p = pb_push1(p, NV097_SET_FLAT_SHADE_OP,
                     NV097_SET_FLAT_SHADE_OP_VERTEX_LAST);
        pb_end(p);
        draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 3, 3);
    } else {
        p = pb_push1(p, NV097_SET_FLAT_SHADE_OP,
                     NV097_SET_FLAT_SHADE_OP_VERTEX_FIRST);
        pb_end(p);
        draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, 3);
    }
}
