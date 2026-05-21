/*
 * texture-format-sweep — texture color-format decode oracle (Tier-1
 * NV2A diag XBE).
 *
 * NV2A feature exercised: §E.1 (texture color formats). The §4.7
 * spec calls for full 42-code coverage; v0.1 narrows to 4 linear
 * 32-bit-per-pixel formats that share the LU_IMAGE_ family but
 * differ in channel ordering -- this verifies the renderer's
 * channel-decode logic with the minimum cell count compatible with
 * the rest of the first-wave 4x2-grid convention. Second-wave will
 * expand to the swizzled SZ_ formats, packed 16-bit formats, DXT
 * compressed formats, depth-as-texture formats, and the YUV /
 * special formats.
 *
 * NV097 methods exercised: SET_TEXTURE_OFFSET, SET_TEXTURE_FORMAT
 *                          (varies per cell), SET_TEXTURE_ADDRESS,
 *                          SET_TEXTURE_CONTROL0, SET_TEXTURE_CONTROL1,
 *                          SET_TEXTURE_FILTER, SET_TEXTURE_IMAGE_RECT;
 *                          SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET (slots
 *                          0 POSITION, 9 TEX0); SET_BEGIN_END,
 *                          DRAW_ARRAYS.
 *
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.7. The test renders an 8-cell
 * 4x2 grid. Each cell:
 *
 *   1. Owns a 4x4 texture in a specific format containing a uniform
 *      color (cube-corner endpoint, no AA needed). The data layout
 *      is format-specific:
 *        - LU_IMAGE_A8R8G8B8 (0x12): little-endian uint32 0xAARRGGBB
 *          → memory bytes (B, G, R, A) per pixel.
 *        - LU_IMAGE_X8R8G8B8 (0x07): same byte layout, alpha ignored.
 *        - LU_IMAGE_A8B8G8R8 (0x3F): little-endian uint32 0xAABBGGRR
 *          → memory bytes (R, G, B, A).
 *        - LU_IMAGE_B8G8R8A8 (0x40): little-endian uint32 0xBBGGRRAA
 *          → memory bytes (A, R, G, B).
 *
 *   2. Binds stage 0 to the cell's texture (xbed_texture_bind_stage0)
 *      with NEAREST/NEAREST filter + CLAMP_TO_EDGE wrap.
 *
 *   3. Draws a quad covering the cell area with TEX0 mapped to the
 *      texture's [0, 1] UV range. The textured PS samples and writes
 *      the sampled color modulated by DIFFUSE = white (no modulation
 *      effect).
 *
 * v0.1 maps 4 of the 8 cells (the linear 32-bit ARGB/ABGR/BGRA/XRGB
 * family). The remaining 4 cells repeat the same pattern with the
 * same A8R8G8B8 format at different colors so the rotation has 8
 * cells of signal; second-wave will replace those repeat cells with
 * additional formats.
 *
 * Cell layout:
 *
 *   Cell  Format             Logical Color   Expected RGB
 *   ----  -----------------  --------------  ------------
 *   0     LU_IMAGE_A8R8G8B8  RED   1,0,0     (255, 0, 0)
 *   1     LU_IMAGE_X8R8G8B8  GREEN 0,1,0     (0, 255, 0)
 *   2     LU_IMAGE_A8B8G8R8  BLUE  0,0,1     (0, 0, 255)
 *   3     LU_IMAGE_B8G8R8A8  WHITE 1,1,1     (255, 255, 255)
 *   4     LU_IMAGE_A8R8G8B8  YELLOW 1,1,0    (255, 255, 0)
 *   5     LU_IMAGE_A8R8G8B8  CYAN  0,1,1     (0, 255, 255)
 *   6     LU_IMAGE_A8R8G8B8  MAGENTA 1,0,1   (255, 0, 255)
 *   7     LU_IMAGE_A8R8G8B8  RED  1,0,0      (255, 0, 0)
 *
 * --- Catches --------------------------------------------------------
 *
 *   - Wrong channel order in any of the 4 LU_IMAGE_ formats: the
 *     specific cell renders with channels permuted (e.g. cell 2
 *     should be BLUE; if A8B8G8R8 decoder swaps R↔B it would render
 *     RED).
 *   - Alpha-vs-no-alpha confusion (A8R8G8B8 vs X8R8G8B8): cell 1
 *     X8R8G8B8 has alpha bits set in the source but expects opaque
 *     GREEN; the renderer mistakenly using the source alpha would
 *     blend toward the framebuffer background.
 *   - Texture-stage binding completely broken: all 8 cells render
 *     as BLACK (no texture sample reaches the PS).
 *   - Texture sampler ignored: cells render solid texture-bg color
 *     (typically 0).
 *
 * --- Does NOT catch (deferred to second wave) -----------------------
 *
 *   - Swizzled SZ_ formats and the swizzle-vs-linear distinction
 *     -- separate XBE §4.8 swizzle-mipmap.
 *   - 16-bit packed formats (R5G6B5, A1R5G5B5, A4R4G4B4): rounding
 *     errors that v0.1's byte-exact gate cannot tolerate.
 *   - DXT1/3/5 compressed formats.
 *   - Depth-as-texture formats.
 *   - YUV (LC_IMAGE_*) formats.
 *
 * --- Reproducibility ------------------------------------------------
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
#define GRID_ROWS 2
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define CELL_W (WIN_W / GRID_COLS)   /* 160 */
#define CELL_H (WIN_H / GRID_ROWS)   /* 240 */

/* Texture dimensions. 4x4 is large enough to land on a stable
 * sample center across NEAREST filter and free from sub-texel
 * AA on integer-grid quads. */
#define TEX_W 4
#define TEX_H 4
#define TEX_BPP 4  /* 4 bytes per pixel for all 32-bit formats here */
#define TEX_SIZE_BYTES (TEX_W * TEX_H * TEX_BPP)

/* Texture format codes (subset). See nv2a_regs.h
 * NV097_SET_TEXTURE_FORMAT_COLOR_*. */
#define FMT_LU_A8R8G8B8 0x12
#define FMT_LU_X8R8G8B8 0x07
#define FMT_LU_A8B8G8R8 0x3F
#define FMT_LU_B8G8R8A8 0x40

typedef struct {
    uint32_t format;
    uint8_t  rgba[4];   /* logical R, G, B, A target (0..255) */
} TexCell;

static const TexCell k_cells[GRID_CELLS] = {
    /* Row 0 -- format-diversity cells. */
    { FMT_LU_A8R8G8B8, { 255,   0,   0, 255 } },  /* RED      */
    { FMT_LU_X8R8G8B8, {   0, 255,   0, 255 } },  /* GREEN    */
    { FMT_LU_A8B8G8R8, {   0,   0, 255, 255 } },  /* BLUE     */
    { FMT_LU_B8G8R8A8, { 255, 255, 255, 255 } },  /* WHITE    */
    /* Row 1 -- repeat with ARGB at 4 more cube corners so the
     * frame has 8 cells of signal for the harness's signal-match
     * gate. Replace with additional formats in second wave. */
    { FMT_LU_A8R8G8B8, { 255, 255,   0, 255 } },  /* YELLOW   */
    { FMT_LU_A8R8G8B8, {   0, 255, 255, 255 } },  /* CYAN     */
    { FMT_LU_A8R8G8B8, { 255,   0, 255, 255 } },  /* MAGENTA  */
    { FMT_LU_A8R8G8B8, { 255,   0,   0, 255 } },  /* RED (re) */
};

/* Per-cell texture data buffers (allocated in VRAM via
 * MmAllocateContiguousMemoryEx). */
static void *s_tex_vram[GRID_CELLS];

typedef struct {
    float pos[3];
    float tex[2];
    float col[4];
} __attribute__((packed)) TexVertex;

#define VERTS_PER_QUAD 6
#define VERTS_TOTAL (GRID_CELLS * VERTS_PER_QUAD)

static TexVertex s_verts[VERTS_TOTAL];
static TexVertex *s_alloc_verts;

static inline void mk_vert(TexVertex *v, int x_w, int y_w,
                           float u, float vc)
{
    v->pos[0] = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1] = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2] = 0.5f;
    v->tex[0] = u;
    v->tex[1] = vc;
    /* White DIFFUSE so the textured PS sample passes through
     * un-modulated. */
    v->col[0] = 1.0f;
    v->col[1] = 1.0f;
    v->col[2] = 1.0f;
    v->col[3] = 1.0f;
}

static void emit_cell_quad(TexVertex *out, int x0, int y0, int x1, int y1)
{
    mk_vert(&out[0], x0, y0, 0.0f, 0.0f);
    mk_vert(&out[1], x1, y0, 1.0f, 0.0f);
    mk_vert(&out[2], x1, y1, 1.0f, 1.0f);
    mk_vert(&out[3], x0, y0, 0.0f, 0.0f);
    mk_vert(&out[4], x1, y1, 1.0f, 1.0f);
    mk_vert(&out[5], x0, y1, 0.0f, 1.0f);
}

static void build_geometry(void)
{
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = row * CELL_H;
            emit_cell_quad(&s_verts[idx * VERTS_PER_QUAD],
                           x0, y0, x0 + CELL_W, y0 + CELL_H);
        }
    }
}

/* Encode (R, G, B, A) into a single 32-bit pixel for the given
 * format. Byte order matches the format's NV2A memory layout
 * (see header comment block for the byte-by-byte description). */
static uint32_t encode_pixel(uint32_t format, const uint8_t rgba[4])
{
    uint8_t r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
    switch (format) {
    case FMT_LU_A8R8G8B8:
        /* Memory bytes (low addr → high): B, G, R, A. */
        return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
               ((uint32_t)g <<  8) |  (uint32_t)b;
    case FMT_LU_X8R8G8B8:
        /* Same byte layout; the X (alpha) bits are unused. */
        return ((uint32_t)0xFF << 24) | ((uint32_t)r << 16) |
               ((uint32_t)g <<  8) |  (uint32_t)b;
    case FMT_LU_A8B8G8R8:
        /* Memory bytes: R, G, B, A. */
        return ((uint32_t)a << 24) | ((uint32_t)b << 16) |
               ((uint32_t)g <<  8) |  (uint32_t)r;
    case FMT_LU_B8G8R8A8:
        /* Memory bytes: A, R, G, B. */
        return ((uint32_t)b << 24) | ((uint32_t)g << 16) |
               ((uint32_t)r <<  8) |  (uint32_t)a;
    default:
        /* Unsupported format -- write magenta so the cell stands
         * out in the captured frame. */
        return 0xFFFF00FF;
    }
}

static void fill_texture(uint32_t format, const uint8_t rgba[4], void *vram)
{
    uint32_t pixel = encode_pixel(format, rgba);
    uint32_t *p = (uint32_t *)vram;
    for (int i = 0; i < TEX_W * TEX_H; i++) {
        p[i] = pixel;
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
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    pb_end(p);
}

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    /* POSITION (slot 0): Float3. */
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(TexVertex), &s_alloc_verts[0].pos[0]);
    /* TEX0 (slot 9): Float2. */
    xbed_set_attrib_pointer(
        9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2,
        sizeof(TexVertex), &s_alloc_verts[0].tex[0]);
    /* DIFFUSE (slot 3): Float4 -- white. */
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(TexVertex), &s_alloc_verts[0].col[0]);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    for (int idx = 0; idx < GRID_CELLS; idx++) {
        XbedTextureStage0 params;
        xbed_texture_init_argb8888_defaults(&params);
        params.vram_addr    = s_tex_vram[idx];
        params.width        = TEX_W;
        params.height       = TEX_H;
        params.color_format = k_cells[idx].format;
        params.pitch_bytes  = TEX_W * TEX_BPP;
        xbed_texture_bind_stage0(&params);

        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }

    xbed_texture_disable_all_stages();
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("texture-format-sweep v0.1\n");

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

    for (int idx = 0; idx < GRID_CELLS; idx++) {
        s_tex_vram[idx] = MmAllocateContiguousMemoryEx(
            TEX_SIZE_BYTES, 0, 0x3ffb000, 0,
            PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!s_tex_vram[idx]) {
            debugPrint("MmAllocateContiguousMemoryEx (tex %d) failed\n", idx);
            Sleep(2000);
            HalReturnToFirmware(HalRebootRoutine);
            return 1;
        }
        fill_texture(k_cells[idx].format, k_cells[idx].rgba,
                     s_tex_vram[idx]);
    }

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\texture-format-sweep-capture.bin",
        "D:\\texture-format-sweep-done.txt",
        "texture-format-sweep");
    return 0;
}
