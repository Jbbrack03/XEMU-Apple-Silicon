/*
 * texture-pitch-alignment — per-format pitch + image-rect alignment
 * oracle (Tier-1 NV2A diag XBE; covers §E.13 in
 * `docs/apple-silicon/nv2a-feature-surface-research.md`).
 *
 * NV2A feature exercised: §E.13 (linear texture row pitch +
 * IMAGE_RECT.width / .height). The test sweeps a uniform-color
 * LU_IMAGE_A8R8G8B8 texture under 8 different combinations of
 * (active width, active height, row pitch). Each cell's VRAM
 * allocation is sized to `pitch * (height + EXTRA_PAD_ROWS)`
 * bytes -- the trailing `EXTRA_PAD_ROWS` physical rows of
 * each per-cell buffer sit beneath the active rectangle so that
 * any sampler that reads past the declared `IMAGE_RECT.height`
 * (e.g. silently rounding height up to a power of two) lands in
 * pre-filled sentinel-gray rather than uninitialised memory. The
 * full allocation is pre-filled with sentinel gray (0xFF808080)
 * BEFORE the per-row active span is overwritten with the cell's
 * target color. Only the per-row leading `width * bytes_per_pixel`
 * bytes of the first `height` rows are overwritten -- the per-row
 * tail (between `width*bpp` and `pitch`), the per-row padding to
 * the right of every active row, AND the entire trailing
 * `EXTRA_PAD_ROWS` rows below the active rectangle remain
 * sentinel-gray.
 *
 * If the renderer honors both registers correctly:
 *   - SET_TEXTURE_IMAGE_RECT.width / .height define the active
 *     sampling region (W cols, H rows).
 *   - SET_TEXTURE_CONTROL1.IMAGE_PITCH defines the per-row byte
 *     stride from row N to row N+1.
 *
 * Sampling u,v in [0, 1] with NEAREST filter + CLAMP_TO_EDGE wrap
 * yields texel coordinates in [0, W) x [0, H). Every such texel
 * lives in the active region, which is uniformly target-colored,
 * so the cell renders as a solid target-color rectangle.
 *
 * If the renderer ignores pitch and assumes `width * bpp` row
 * stride, the row-1+ sample reads ` (sentinel) | (target) ` bytes
 * from the row-0 padding tail, producing visible gray contamination
 * in the cell.
 *
 * If the renderer ignores IMAGE_RECT.width and instead uses some
 * other dimension (e.g. derived from a power-of-two assumption),
 * the sampling step on u in [0,1] lands outside the active span
 * and reads sentinel bytes.
 *
 * If the renderer ignores IMAGE_RECT.height (e.g. silently rounds
 * up to the next power of two of height), sampling at v in [0,1]
 * with the wrong logical height steps into the trailing
 * EXTRA_PAD_ROWS physical rows -- which are deliberately
 * sentinel-grey -- and the cell visibly degrades. The wrong-height
 * read is safe (always within the allocation) because
 * EXTRA_PAD_ROWS >= next_pow2(height) - height for every cell.
 *
 * NV097 methods exercised: SET_TEXTURE_OFFSET, SET_TEXTURE_FORMAT,
 *                          SET_TEXTURE_ADDRESS, SET_TEXTURE_CONTROL0,
 *                          SET_TEXTURE_CONTROL1 (varies per cell --
 *                          this is the key register under test),
 *                          SET_TEXTURE_FILTER,
 *                          SET_TEXTURE_IMAGE_RECT (varies per cell --
 *                          the second key register under test);
 *                          SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET
 *                          (slots 0 POSITION, 3 DIFFUSE, 9 TEX0);
 *                          SET_BEGIN_END, DRAW_ARRAYS.
 *
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- Cell layout ----------------------------------------------------
 *
 * 4x2 screen grid (160x240 per cell). Every cell uses the
 * LU_IMAGE_A8R8G8B8 format (0x12) and a uniform cube-corner target
 * color. The math-derived expected output is therefore the same 4x2
 * cube-corner grid as `texture-format-sweep`, but the mechanism
 * tested is the linear pitch + image-rect register pair, not the
 * format-decoder family.
 *
 * Allocation per cell = pitch * (height + EXTRA_PAD_ROWS).
 * Active region = first `width` texels of the first `height`
 * rows. Sentinel-gray fills everything else.
 *
 *   Cell  Color    Active  Pitch  Variation
 *   ----  -------  ------  -----  -----------------------------------------
 *   0     RED      4 x 4    16     Baseline: pitch == w * bpp
 *   1     GREEN    4 x 4    64     Pitch 4x w*bpp
 *   2     BLUE     8 x 4    64     Wider active, same pitch
 *   3     WHITE    5 x 3    32     Non-power-of-two dims (small)
 *   4     YELLOW   4 x 4    20     Smallest non-baseline pitch (w*bpp + 4)
 *   5     CYAN     4 x 4   128     Pitch 8x w*bpp (very large)
 *   6     MAGENTA  7 x 5    64     Odd width with non-aligned pitch
 *   7     RED      3 x 3    32     Tiny active, oversized pitch
 *
 * Cells 0 + 7 both render RED (matches `texture-format-sweep`'s
 * grid so the same math oracle applies). Cell 0 is the only true
 * baseline (`pitch == width * bpp`) -- every other cell carries
 * at least 4 bytes of per-row padding, so a "pitch silently
 * treated as width*bpp" regression would flip cells 1-7 to
 * sentinel-gray while cell 0 remained intact. Cell 4 is the
 * smallest such padded case (4 bytes padding per row).
 *
 * --- Catches --------------------------------------------------------
 *
 *   - Row-stride bug: any cell with pitch > width * bpp picks up
 *     sentinel gray in rows 1+ if the renderer assumes width*bpp
 *     row stride.
 *   - IMAGE_RECT.width / .height ignored: cells with non-power-of-
 *     two dimensions (3/5/7) read past the active span and pick up
 *     sentinel.
 *   - Complete linear-texture binding failure: all 8 cells render
 *     as black or sentinel.
 *
 * --- Does NOT catch (deferred / separate XBEs) ----------------------
 *
 *   - Swizzled SZ_ formats (no row pitch register; uses Morton
 *     curve) -- see `swizzle-mipmap`.
 *   - Format-channel decode bugs -- `texture-format-sweep` already
 *     covers that.
 *   - DMA channel A vs B -- `texture-dma-ab` covers that.
 *   - Border-source / wrap modes -- `texture-filter-wrap` covers
 *     CLAMP_TO_EDGE / WRAP / MIRROR / BORDER, but does so at
 *     baseline pitch.
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

#define BPP 4  /* LU_IMAGE_A8R8G8B8 = 4 bytes per pixel */

/* Extra physical rows added beneath each cell's active rectangle.
 * These rows are pre-filled with sentinel-gray and never written
 * with target color. They give the test a SAFE read target if the
 * renderer silently sampling at v in [0,1] with a wrong height
 * larger than `IMAGE_RECT.height` (the most likely failure mode is
 * rounding up to next_pow2(height)). The value 8 covers every cell
 * here -- max(next_pow2(h) - h) across the 8 cells is 3 (cell 6
 * height=5 -> next_pow2=8), so 8 extra rows is a comfortable
 * envelope. */
#define EXTRA_PAD_ROWS 8

/* Texture format code for the whole sweep. The sister XBE
 * `texture-format-sweep` already covers cross-format channel decode
 * at baseline pitch; here we want a single well-understood
 * format so the only variable under test is the pitch +
 * image-rect register pair. */
#define FMT_LU_A8R8G8B8 0x12

typedef struct {
    uint32_t width;       /* IMAGE_RECT.width */
    uint32_t height;      /* IMAGE_RECT.height */
    uint32_t pitch_bytes; /* TEXCTL1.IMAGE_PITCH (bytes per row) */
    uint8_t  rgba[4];     /* logical R, G, B, A target (0..255) */
} TexCell;

static const TexCell k_cells[GRID_CELLS] = {
    /* Row 0 -- format-diversity cells. */
    { 4, 4,  16, { 255,   0,   0, 255 } },  /* 0 RED       baseline pitch */
    { 4, 4,  64, {   0, 255,   0, 255 } },  /* 1 GREEN     4x oversized   */
    { 8, 4,  64, {   0,   0, 255, 255 } },  /* 2 BLUE      wider active   */
    { 5, 3,  32, { 255, 255, 255, 255 } },  /* 3 WHITE     non-pow2 dims  */
    /* Row 1 */
    { 4, 4,  20, { 255, 255,   0, 255 } },  /* 4 YELLOW    pitch w*bpp+4  */
    { 4, 4, 128, {   0, 255, 255, 255 } },  /* 5 CYAN      8x oversized   */
    { 7, 5,  64, { 255,   0, 255, 255 } },  /* 6 MAGENTA   odd w + pitch  */
    { 3, 3,  32, { 255,   0,   0, 255 } },  /* 7 RED       tiny + padded  */
};

/* Sentinel color filling the per-cell VRAM allocation BEFORE the
 * active rectangle is written. Chosen as a non-cube-corner gray so
 * (a) it does NOT match any cell's target color and (b) a regression
 * that leaks sentinel bytes through the sampler will produce a
 * visually obvious gray contamination in the framebuffer.
 *
 * LU_IMAGE_A8R8G8B8 memory bytes (low addr -> high) are (B, G, R, A).
 * Sentinel RGBA = (0x80, 0x80, 0x80, 0xFF) -> memory (0x80, 0x80,
 * 0x80, 0xFF). */
#define SENTINEL_PIXEL 0xFF808080u

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

/* Pack target RGBA into a single 32-bit LU_IMAGE_A8R8G8B8 pixel.
 * Memory bytes (low addr -> high): B, G, R, A. */
static inline uint32_t pack_argb8888(const uint8_t rgba[4])
{
    return ((uint32_t)rgba[3] << 24) | ((uint32_t)rgba[0] << 16) |
           ((uint32_t)rgba[1] <<  8) |  (uint32_t)rgba[2];
}

/* Fill the per-cell VRAM allocation with the sentinel pattern then
 * overwrite ONLY the first `width * BPP` bytes of each of the
 * first `height` rows with the cell's target pixel. Bytes between
 * `width * BPP` and `pitch_bytes` per row, and bytes beyond
 * `height * pitch_bytes`, stay sentinel-gray. */
static void fill_pitch_texture(const TexCell *cell, void *vram,
                               size_t alloc_size)
{
    /* Step 1: sentinel-fill the entire allocation as uint32. */
    {
        uint32_t *p = (uint32_t *)vram;
        size_t count = alloc_size / sizeof(uint32_t);
        for (size_t i = 0; i < count; i++) {
            p[i] = SENTINEL_PIXEL;
        }
    }
    /* Step 2: overwrite the per-row active span. */
    const uint32_t target = pack_argb8888(cell->rgba);
    for (uint32_t row = 0; row < cell->height; row++) {
        uint32_t *row_base = (uint32_t *)((uint8_t *)vram +
                                          row * cell->pitch_bytes);
        for (uint32_t col = 0; col < cell->width; col++) {
            row_base[col] = target;
        }
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
        params.width        = k_cells[idx].width;
        params.height       = k_cells[idx].height;
        params.color_format = FMT_LU_A8R8G8B8;
        params.pitch_bytes  = k_cells[idx].pitch_bytes;
        xbed_texture_bind_stage0(&params);

        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }

    xbed_texture_disable_all_stages();
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("texture-pitch-alignment v0.2\n");

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
        const TexCell *cell = &k_cells[idx];
        /* Allocation = pitch * (height + EXTRA_PAD_ROWS) so a
         * wrong-height sampler that reads up to next_pow2(height)
         * rows lands in pre-filled sentinel rather than past the
         * allocation. */
        size_t alloc_size = (size_t)cell->pitch_bytes *
                            (cell->height + EXTRA_PAD_ROWS);
        s_tex_vram[idx] = MmAllocateContiguousMemoryEx(
            alloc_size, 0, 0x3ffb000, 0,
            PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!s_tex_vram[idx]) {
            debugPrint("MmAllocateContiguousMemoryEx (tex %d) failed\n", idx);
            Sleep(2000);
            HalReturnToFirmware(HalRebootRoutine);
            return 1;
        }
        fill_pitch_texture(cell, s_tex_vram[idx], alloc_size);
    }

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\texture-pitch-alignment-capture.bin",
        "D:\\texture-pitch-alignment-done.txt",
        "texture-pitch-alignment");
    return 0;
}
