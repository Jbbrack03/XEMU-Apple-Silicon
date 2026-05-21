/*
 * texture-filter-wrap — UV wrap/clamp + NEAREST filter oracle
 * (Tier-1 NV2A diag XBE).
 *
 * NV2A feature exercised: §E.3 (texture address modes / wrap),
 *                          §E.4 (NEAREST texture filter).
 * NV097 methods exercised: SET_TEXTURE_OFFSET, SET_TEXTURE_FORMAT,
 *                          SET_TEXTURE_ADDRESS (varies per cell),
 *                          SET_TEXTURE_CONTROL0/CONTROL1,
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
 * Per `diagnostic-xbe-plan.md` v2 §4.11. v0.1 narrows scope to
 * NEAREST filter + 2 wrap modes (CLAMP_TO_EDGE, WRAP) to keep
 * byte-exact comparison feasible. LINEAR filter and the other wrap
 * modes (MIRROR, CLAMP, BORDER) are second-wave follow-ups -- LINEAR
 * produces sub-byte fractional outputs that require
 * compare_overrides tolerance + a more careful oracle.
 *
 * The test uses a 4x4 quadrant texture with 4 solid-color regions:
 *   Texels (0..1, 0..1) = RED      (top-left quadrant)
 *   Texels (2..3, 0..1) = GREEN    (top-right quadrant)
 *   Texels (0..1, 2..3) = BLUE     (bottom-left quadrant)
 *   Texels (2..3, 2..3) = WHITE    (bottom-right quadrant)
 *
 * Each cell uses CONSTANT UV at all four vertices so the entire
 * cell renders as the single sampled texel's color. Per-cell UV
 * is chosen far from quadrant boundaries so NEAREST filter
 * deterministically picks one quadrant. The wrap mode is set per
 * cell via xbed_texture_bind_stage0 params.
 *
 * Cell layout. NV2A linear (LU_IMAGE_) textures use TEXEL-UNIT
 * (unnormalized) UV coordinates: U=0.5 picks texel column 0; U=2.5
 * picks texel column 2; U=2.5+TEX_W=6.5 picks texel column 2 after
 * WRAP. (The renderer's norm0() divides UVs by texSize before
 * sampling; this matches the nxdk mesh sample's UV convention.)
 *
 *   Cell  Wrap            UV              Expected texel  Color
 *   ----  --------------  --------------  --------------  -----
 *   0     CLAMP_TO_EDGE   (0.5, 0.5)      (0, 0)          RED
 *   1     CLAMP_TO_EDGE   (2.5, 0.5)      (2, 0)          GREEN
 *   2     CLAMP_TO_EDGE   (0.5, 2.5)      (0, 2)          BLUE
 *   3     CLAMP_TO_EDGE   (2.5, 2.5)      (2, 2)          WHITE
 *   4     WRAP            (4.5, 0.5)      (0, 0)          RED
 *   5     WRAP            (6.5, 0.5)      (2, 0)          GREEN
 *   6     WRAP            (4.5, 2.5)      (0, 2)          BLUE
 *   7     WRAP            (6.5, 2.5)      (2, 2)          WHITE
 *
 * Both rows render the same R/G/B/W pattern -- row 0 via direct
 * in-range UV sampling (CLAMP_TO_EDGE is a no-op for in-range UV),
 * row 1 via UV offset by TEX_W=4 in U axis and wrap=WRAP. If WRAP is
 * mis-implemented (e.g. treated as CLAMP_TO_EDGE), row 1 would
 * clamp to U=3.5 → col 3 which is GREEN/WHITE, exposing the bug.
 *
 * --- Catches --------------------------------------------------------
 *
 *   - Wrap mode WRAP completely ignored: row 1 renders the right
 *     edge of the texture (GREEN/WHITE) instead of wrapping to
 *     the left edge (RED/BLUE).
 *   - Wrap mode WRAP applied to wrong axis: U vs V swapped would
 *     wrap V at the same cells, showing R G R G instead of R G B W.
 *   - NEAREST filter ignored / falling back to LINEAR: cells would
 *     show blurred colors instead of saturated 0/255.
 *   - Texture stage bind failure: all cells BLACK.
 *
 * --- Does NOT catch (deferred to second wave) -----------------------
 *
 *   - LINEAR filter gradients: requires compare_overrides tolerance
 *     and sub-texel oracle. Separate XBE.
 *   - MIRROR / CLAMP / BORDER wrap modes: each needs its own
 *     verification cells; v0.1 prioritized the most-common WRAP +
 *     CLAMP_TO_EDGE pair used by retail Xbox titles.
 *   - CYLINDER wrap modes (cylindrical UV mapping).
 *   - Anisotropic filtering.
 *   - Multi-mipmap wrap behavior.
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

/* 4x4 quadrant texture, A8R8G8B8. Texels in memory order
 * (low addr -> high) for a uint32 0xAARRGGBB are (B, G, R, A). */
#define TEX_W 4
#define TEX_H 4
#define TEX_BPP 4
#define TEX_SIZE_BYTES (TEX_W * TEX_H * TEX_BPP)

/* Wrap mode enums (NV097_SET_TEXTURE_ADDRESS U/V). */
#define WRAP_REPEAT         1
#define WRAP_MIRROR         2
#define WRAP_CLAMP_TO_EDGE  3

/* NEAREST filter enum (NV097_SET_TEXTURE_FILTER MIN/MAG). */
#define FILTER_NEAREST 1

typedef struct {
    float    uv[2];
    uint32_t wrap;   /* applied to BOTH S and T */
    uint8_t  expected_rgba[4];
} Cell;

/* NV2A LU_IMAGE_ textures use TEXEL-UNIT (unnormalized) UV coords,
 * matching the NV2A texture-shader pipeline (PS_TEXTUREMODES_2D_PROJECTIVE
 * + the renderer's norm0() = coord / texSize divisor; see glsl/psh.c).
 * The nxdk mesh sample confirms this convention (its UVs are integer
 * pixel positions like (44, 143)). The TEX_W=4 texture is sampled at
 * (0.5, 0.5) for texel (0,0) and (2.5, 0.5) for texel (2,0) etc.; the
 * +TEX_W=+4 offset for the WRAP row sends sampling back to the same
 * texels via SET_TEXTURE_ADDRESS=WRAP. */
static const Cell k_cells[GRID_CELLS] = {
    /* Row 0 -- CLAMP_TO_EDGE with in-range UVs sampling each
     * quadrant directly. */
    { { 0.5f, 0.5f }, WRAP_CLAMP_TO_EDGE, { 0xFF, 0x00, 0x00, 0xFF } }, /* RED   texel(0,0) */
    { { 2.5f, 0.5f }, WRAP_CLAMP_TO_EDGE, { 0x00, 0xFF, 0x00, 0xFF } }, /* GREEN texel(2,0) */
    { { 0.5f, 2.5f }, WRAP_CLAMP_TO_EDGE, { 0x00, 0x00, 0xFF, 0xFF } }, /* BLUE  texel(0,2) */
    { { 2.5f, 2.5f }, WRAP_CLAMP_TO_EDGE, { 0xFF, 0xFF, 0xFF, 0xFF } }, /* WHITE texel(2,2) */
    /* Row 1 -- WRAP with UV offset by +TEX_W=+4 in U axis; the wrap
     * brings sampling back to the same texels as row 0. */
    { { 4.5f, 0.5f }, WRAP_REPEAT,        { 0xFF, 0x00, 0x00, 0xFF } }, /* RED   wrapped */
    { { 6.5f, 0.5f }, WRAP_REPEAT,        { 0x00, 0xFF, 0x00, 0xFF } }, /* GREEN wrapped */
    { { 4.5f, 2.5f }, WRAP_REPEAT,        { 0x00, 0x00, 0xFF, 0xFF } }, /* BLUE  wrapped */
    { { 6.5f, 2.5f }, WRAP_REPEAT,        { 0xFF, 0xFF, 0xFF, 0xFF } }, /* WHITE wrapped */
};

static void *s_tex_vram;

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
    /* All 4 verts of a cell carry the SAME UV so smooth
     * interpolation yields constant UV across the cell -- the
     * NEAREST filter then picks a single texel for the whole
     * cell. */
    v->tex[0] = u;
    v->tex[1] = vc;
    v->col[0] = 1.0f;
    v->col[1] = 1.0f;
    v->col[2] = 1.0f;
    v->col[3] = 1.0f;
}

static void emit_cell_quad(TexVertex *out, int x0, int y0, int x1, int y1,
                           float u, float vc)
{
    mk_vert(&out[0], x0, y0, u, vc);
    mk_vert(&out[1], x1, y0, u, vc);
    mk_vert(&out[2], x1, y1, u, vc);
    mk_vert(&out[3], x0, y0, u, vc);
    mk_vert(&out[4], x1, y1, u, vc);
    mk_vert(&out[5], x0, y1, u, vc);
}

static void build_geometry(void)
{
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int x0 = col * CELL_W;
            const int y0 = row * CELL_H;
            emit_cell_quad(&s_verts[idx * VERTS_PER_QUAD],
                           x0, y0, x0 + CELL_W, y0 + CELL_H,
                           k_cells[idx].uv[0], k_cells[idx].uv[1]);
        }
    }
}

/* Encode A8R8G8B8 pixel from logical RGBA bytes. Memory layout
 * (low -> high) = (B, G, R, A). */
static uint32_t encode_argb(const uint8_t rgba[4])
{
    return ((uint32_t)rgba[3] << 24) | ((uint32_t)rgba[0] << 16) |
           ((uint32_t)rgba[1] <<  8) |  (uint32_t)rgba[2];
}

/* Fill the 4x4 texture with the 4 quadrant pattern. */
static void fill_quadrant_texture(void *vram)
{
    static const uint8_t QUAD[4][4] = {
        { 0xFF, 0x00, 0x00, 0xFF }, /* RED   (TL) */
        { 0x00, 0xFF, 0x00, 0xFF }, /* GREEN (TR) */
        { 0x00, 0x00, 0xFF, 0xFF }, /* BLUE  (BL) */
        { 0xFF, 0xFF, 0xFF, 0xFF }, /* WHITE (BR) */
    };
    uint32_t *p = (uint32_t *)vram;
    for (int y = 0; y < TEX_H; y++) {
        for (int x = 0; x < TEX_W; x++) {
            int qx = (x < TEX_W / 2) ? 0 : 1;
            int qy = (y < TEX_H / 2) ? 0 : 1;
            int q  = qy * 2 + qx;  /* 0=TL, 1=TR, 2=BL, 3=BR */
            p[y * TEX_W + x] = encode_argb(QUAD[q]);
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
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(TexVertex), &s_alloc_verts[0].pos[0]);
    xbed_set_attrib_pointer(
        9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2,
        sizeof(TexVertex), &s_alloc_verts[0].tex[0]);
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
        params.vram_addr    = s_tex_vram;
        params.width        = TEX_W;
        params.height       = TEX_H;
        params.color_format = 0x12;  /* LU_IMAGE_A8R8G8B8 */
        params.pitch_bytes  = TEX_W * TEX_BPP;
        params.min_filter   = FILTER_NEAREST;
        params.mag_filter   = FILTER_NEAREST;
        params.wrap_s       = k_cells[idx].wrap;
        params.wrap_t       = k_cells[idx].wrap;
        xbed_texture_bind_stage0(&params);

        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_QUAD, VERTS_PER_QUAD);
    }

    xbed_texture_disable_all_stages();
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("texture-filter-wrap v0.1\n");

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

    s_tex_vram = MmAllocateContiguousMemoryEx(
        TEX_SIZE_BYTES, 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_tex_vram) {
        debugPrint("MmAllocateContiguousMemoryEx (tex) failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    fill_quadrant_texture(s_tex_vram);

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\texture-filter-wrap-capture.bin",
        "D:\\texture-filter-wrap-done.txt",
        "texture-filter-wrap");
    return 0;
}
