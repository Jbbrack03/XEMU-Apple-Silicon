/*
 * swizzle-mipmap — SZ_A8R8G8B8 mip-chain offset + per-mip LOD-clamped
 *                  sampling Tier-1 NV2A diag XBE.
 *
 * NV2A feature exercised: §E.5 (LOD / mipmap selection -- MIN_LOD_CLAMP
 *                         + MAX_LOD_CLAMP enforcement), §E.9 (swizzled
 *                         texture layouts), §E.1 (SZ_A8R8G8B8 format).
 * NV097 methods:           SET_TEXTURE_OFFSET, SET_TEXTURE_FORMAT,
 *                          SET_TEXTURE_CONTROL0 (MIN_LOD_CLAMP +
 *                          MAX_LOD_CLAMP fields), SET_TEXTURE_CONTROL1,
 *                          SET_TEXTURE_FILTER, SET_TEXTURE_IMAGE_RECT,
 *                          SET_TEXTURE_ADDRESS, SET_SHADER_STAGE_PROGRAM.
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.8: a 64x64 base SZ_A8R8G8B8
 * texture with a 7-level mip chain (levels 64x64, 32x32, 16x16,
 * 8x8, 4x4, 2x2, 1x1). The texture is bound ONCE per draw with
 * MIPMAP_LEVELS=7 -- the renderer is responsible for walking the
 * mip chain to find each level's base offset. Per cell, only the
 * MIN_LOD_CLAMP / MAX_LOD_CLAMP fields in SET_TEXTURE_CONTROL0
 * vary; both are set to the same value N so the sampler is forced
 * to fetch mip N regardless of UV derivatives.
 *
 * Each mip is filled with a UNIQUE PRE-SWIZZLED non-uniform 2x2
 * pattern (4-quadrant grid of distinct colors). The discriminator
 * per cell is:
 *   - Which mip got fetched (LOD clamp respected?).
 *   - Which intra-mip texel got sampled (swizzle decode correct?
 *     Wrong swizzle masks → wrong quadrant colors).
 *
 * Cells 0..6 sample the 4 quadrants of mip N respectively. Each cell
 * is split into a 2x2 sub-grid showing the 4 quadrant colors of mip
 * N -- the cell-as-painted matches mip N's pre-swizzled 2x2 pattern.
 *
 * Cell 7 is unused (no draw issued; retains BLACK clear).
 *
 * This addresses both Codex findings from the v0.1 review
 * (2026-05-21):
 *
 *   1. Single-bind / MIPMAP_LEVELS > 1: actually exercises xemu's
 *      mip-chain offset traversal (was bypassed by v0.1's per-cell
 *      MIPMAP_LEVELS=1 rebind).
 *   2. Non-uniform pre-swizzled mip data: discriminates intra-level
 *      address mapping (was masked by v0.1's solid-color fill).
 *
 * Depends on the 2026-05-21 Metal renderer fix in
 * mtl/texture_pg.c::build_sampler_desc_from_pg that maps
 * TextureShape.min_mipmap_level / max_mipmap_level to MTLSamplerDesc
 * lodMinClamp / lodMaxClamp. Pre-fix, all cells would have sampled
 * mip 0 because Metal hardcoded min_lod=0 / max_lod=levels-1
 * regardless of guest writes.
 *
 * --- What this XBE does NOT test (deferred, second-wave) ------------
 *
 *   - GPU's automatic LOD selection from screen-space derivatives.
 *     We pin LOD via clamps; auto-selection is covered by
 *     `swizzle-mipmap-lod-auto` second-wave XBE.
 *   - LINEAR / TENT_TENT_LOD trilinear filtering across mips.
 *     We use NEAREST mipmap + NEAREST tap filter.
 *   - LU_IMAGE_ (linear) + mips: real NV2A and xemu both reject
 *     linear+mip per `gl/texture.c:749-755`; not a renderer gap.
 *   - Real-Xbox swizzled-order mip chain offset divergence: catalog
 *     warning at `nv2a-feature-surface-research.md:1164-1170`
 *     suggests real hardware may compute level offset via
 *     `swizzle(level_offset)`. xemu uses linear sum
 *     (`hw/xbox/nv2a/pgraph/texture.c:178-186`). If real Xbox
 *     diverges, the XBE will FAIL on real-xbox / PASS on xemu --
 *     that is itself a useful oracle signal (resolve via renderer
 *     fix or doc the divergence).
 *
 * --- Mip data + swizzle ---------------------------------------------
 *
 * For each mip level, the 4 quadrants get 4 distinct colors:
 *   Q0 (TL): red-ish
 *   Q1 (TR): green-ish
 *   Q2 (BL): blue-ish
 *   Q3 (BR): yellow-ish
 *
 * "red-ish" etc. are tinted per mip so a wrong-mip selection
 * shows the wrong shade of red (instead of cleanly black). Mip 0
 * Q0 = (255,0,0) saturated; mip 1 Q0 = (223,0,0) at tint 0xDF;
 * mip 2 0xBF, mip 3 0x9F, mip 4 0x7F, mip 5 0x5F, mip 6 0x3F
 * (=> Q0 ramps 255 / 223 / 191 / 159 / 127 / 95 / 63). Same
 * scaling applied to G/B/Y channels.
 *
 * For mips >= 2 (4x4 and smaller), the 2x2 quadrant scheme reduces
 * to fewer texels per quadrant -- mip 5 is 2x2 = 1 texel per
 * quadrant; mip 6 is 1x1 = all 4 "quadrants" collapse to a single
 * texel (we use Q0's color for that texel). Cells sampling smaller
 * mips show smaller-but-still-distinct patterns.
 *
 * Pre-swizzle: the CPU writes mip data into VRAM using the NV2A
 * Z-order swizzle. For each linear (x, y) pixel, the byte offset
 * in swizzled storage is `swizzle_offset(x, y, w, h)`. We replicate
 * the same masks xemu's `swizzle_box` uses (see
 * `hw/xbox/nv2a/pgraph/swizzle.c:48-70`).
 *
 * --- Layout ---------------------------------------------------------
 *
 * 4-col × 2-row grid. WIN = 640x480; cell_w = 160, cell_h = 240.
 * Per cell, the 2x2 quadrant pattern is rendered by drawing 4 small
 * quads (one per quadrant). Each small quad samples a fixed UV
 * (the center of one of the 4 quadrants in normalized UV space).
 *
 * Cell layout (col, row):
 *   Row 0: mip0 mip1 mip2 mip3
 *   Row 1: mip4 mip5 mip6 unused
 *
 * Within each cell, the 4 quadrants paint the 4-tone pattern.
 *
 * --- Catches --------------------------------------------------------
 *
 *   - Metal renderer ignores MIN/MAX_LOD_CLAMP (pre-fix behavior):
 *     all cells show mip 0's bright pattern; cells 1..6 fail.
 *   - Mip-chain offset off-by-one: cell N shows mip N-1 or N+1's
 *     shade. Tints differ by 32 LSB per channel per mip (well above
 *     the harness threshold of 16).
 *   - SZ_A8R8G8B8 intra-mip swizzle mis-addressing: quadrant
 *     positions mismatch. Each quadrant has a distinct color so any
 *     swap is visible.
 *   - SZ_A8R8G8B8 channel mis-route at any base size: tint
 *     channels swap (R<->B, etc.).
 *   - mip 6 (1x1) corner case: degenerate dims should still produce
 *     mip 0 Q0's tint -- if the renderer asserts on 1x1 swizzle
 *     decode, this cell fails.
 *
 * --- Reproducibility ------------------------------------------------
 *
 * Pure deterministic pattern; texture is filled once at XBE init
 * and the same draw is issued every frame. Byte-identical across
 * two cold runs.
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

#define BASE_TEX_W 64
#define BASE_TEX_H 64
#define BASE_TEX_LOG2 6
#define MIP_LEVELS 7

/* Per-cell: 4 quadrant sub-quads; each sub-quad is 6 vertices
 * (two triangles). Cell 7 has no quads but we still reserve the
 * vertex space for layout symmetry; the render loop skips draws
 * for cell 7. */
#define QUADS_PER_CELL 4
#define VERTS_PER_QUAD 6
#define VERTS_PER_CELL (QUADS_PER_CELL * VERTS_PER_QUAD)
#define VERTS_TOTAL    (GRID_CELLS * VERTS_PER_CELL)

#define SZ_A8R8G8B8_CODE 0x06

/* Cell -> mip level. -1 = unused cell. */
static const int k_cell_mip[GRID_CELLS] = {
    0, 1, 2, 3,
    4, 5, 6, -1,
};

/* Base RGB for each quadrant; tints scale per mip level. */
static const uint8_t k_quad_base_rgb[4][3] = {
    {0xFF, 0x00, 0x00},  /* Q0: red    */
    {0x00, 0xFF, 0x00},  /* Q1: green  */
    {0x00, 0x00, 0xFF},  /* Q2: blue   */
    {0xFF, 0xFF, 0x00},  /* Q3: yellow */
};

/* Per-mip tint factor (0xFF -> 0x3F = 255 -> 63 in 32-LSB steps).
 * 6 intervals of 32 LSB each, all well above the harness per-channel
 * threshold of 16 so any off-by-one mip selection produces a
 * pixel-compare failure. */
static uint8_t k_mip_tint[MIP_LEVELS] = {
    0xFF, 0xDF, 0xBF, 0x9F, 0x7F, 0x5F, 0x3F,
};

typedef struct {
    float pos[3];
    /* TEXCOORD0 as a 4-component vector. The textured PS uses
     * SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE which samples
     * the texture at (u/w, v/w). We pass w = 1.0 so the projective
     * divide collapses to a plain 2D sample at (u, v). Without an
     * explicit w (and with stride/size set to 2), the GPU reads
     * whatever bytes happen to be at the next vec slot, which makes
     * the divide unpredictable and silently collapses all sample
     * positions to the same texel. Caught by the v0.2 round of
     * Metal-side validation 2026-05-21. */
    float tex[4];
    float col[4];
} __attribute__((packed)) TexVertex;

static TexVertex  s_verts[VERTS_TOTAL];
static TexVertex *s_alloc_verts;
static void      *s_tex_vram;
static uint32_t   s_mip_offsets[MIP_LEVELS];
static uint32_t   s_mip_total_bytes;

/* --- Swizzle helpers (mirror hw/xbox/nv2a/pgraph/swizzle.c) -------- */

static void generate_swizzle_masks_2d(unsigned int width,
                                      unsigned int height,
                                      uint32_t *mask_x,
                                      uint32_t *mask_y)
{
    uint32_t x = 0, y = 0;
    uint32_t bit = 1;
    uint32_t mask_bit = 1;
    int done;
    do {
        done = 1;
        if (bit < width)  { x |= mask_bit; mask_bit <<= 1; done = 0; }
        if (bit < height) { y |= mask_bit; mask_bit <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    *mask_x = x;
    *mask_y = y;
}

/* Walk linear (x, y) and write each pixel at the matching swizzled
 * offset. Mirrors swizzle_box_internal's address-walk pattern. */
static void swizzle_fill(uint8_t *dst, unsigned int w, unsigned int h,
                         const uint8_t *(*pixel_for)(unsigned int x,
                                                     unsigned int y,
                                                     void *user),
                         void *user)
{
    if (w == 1 && h == 1) {
        const uint8_t *src = pixel_for(0, 0, user);
        dst[0] = src[0]; dst[1] = src[1];
        dst[2] = src[2]; dst[3] = src[3];
        return;
    }
    uint32_t mask_x, mask_y;
    generate_swizzle_masks_2d(w, h, &mask_x, &mask_y);
    int off_y = 0;
    for (unsigned int y = 0; y < h; y++) {
        int off_x = 0;
        for (unsigned int x = 0; x < w; x++) {
            const uint8_t *src = pixel_for(x, y, user);
            uint8_t *p = dst + (off_y + off_x) * 4;
            p[0] = src[0];
            p[1] = src[1];
            p[2] = src[2];
            p[3] = src[3];
            off_x = (off_x - mask_x) & mask_x;
        }
        off_y = (off_y - mask_y) & mask_y;
    }
}

/* Per-pixel color lookup: pick the quadrant the (x, y) lands in and
 * return the tinted RGBA in BGRA byte order (NV2A SZ_A8R8G8B8 stores
 * pixels as A:R:G:B 32-bit big-endian within each DWORD, which on
 * little-endian Xbox memory lays out as B G R A). */
typedef struct {
    unsigned int w, h;
    uint8_t      tint;
    uint8_t      bgra[16];  /* pre-computed BGRA bytes for 4 quadrants */
} MipFillCtx;

static const uint8_t *quad_pixel_for(unsigned int x, unsigned int y,
                                     void *user)
{
    MipFillCtx *ctx = (MipFillCtx *)user;
    unsigned int half_w = ctx->w >> 1;
    unsigned int half_h = ctx->h >> 1;
    if (half_w == 0) half_w = 1;
    if (half_h == 0) half_h = 1;
    unsigned int qx = (x >= half_w) ? 1 : 0;
    unsigned int qy = (y >= half_h) ? 1 : 0;
    unsigned int quad = qy * 2 + qx;
    return &ctx->bgra[quad * 4];
}

static void apply_tint(uint8_t out_bgra[4], const uint8_t base_rgb[3],
                       uint8_t tint, uint8_t alpha)
{
    /* Apply tint as a per-channel multiplicative factor 8x8 -> 16,
     * then top-byte. Equivalent to (channel * tint) / 255 with
     * banker's rounding. Mirrors what a (255,0,0)*0xDF -> (0xDF,0,0)
     * gives, which is what the math oracle expects. */
    uint32_t r = ((uint32_t)base_rgb[0] * (uint32_t)tint + 127) / 255;
    uint32_t g = ((uint32_t)base_rgb[1] * (uint32_t)tint + 127) / 255;
    uint32_t b = ((uint32_t)base_rgb[2] * (uint32_t)tint + 127) / 255;
    out_bgra[0] = (uint8_t)b;
    out_bgra[1] = (uint8_t)g;
    out_bgra[2] = (uint8_t)r;
    out_bgra[3] = alpha;
}

static void compute_mip_layout(void)
{
    uint32_t offset = 0;
    uint32_t w = BASE_TEX_W, h = BASE_TEX_H;
    for (int level = 0; level < MIP_LEVELS; level++) {
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        s_mip_offsets[level] = offset;
        offset += w * h * 4;
        w /= 2;
        h /= 2;
    }
    s_mip_total_bytes = offset;
}

static void fill_mip_data(void)
{
    uint8_t *base = (uint8_t *)s_tex_vram;
    uint32_t w = BASE_TEX_W, h = BASE_TEX_H;
    for (int level = 0; level < MIP_LEVELS; level++) {
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        MipFillCtx ctx;
        ctx.w = w;
        ctx.h = h;
        ctx.tint = k_mip_tint[level];
        for (int q = 0; q < 4; q++) {
            apply_tint(&ctx.bgra[q * 4], k_quad_base_rgb[q], ctx.tint,
                       0xFF);
        }
        swizzle_fill(base + s_mip_offsets[level], w, h,
                     quad_pixel_for, &ctx);
        w /= 2;
        h /= 2;
    }
}

/* --- Vertex builders ---------------------------------------------- */

static inline void mk_vert(TexVertex *v, int x_w, int y_w,
                           float u, float vc)
{
    v->pos[0]  = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1]  = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2]  = 0.5f;
    v->tex[0]  = u;
    v->tex[1]  = vc;
    v->tex[2]  = 0.0f;   /* r-axis unused for 2D textures */
    v->tex[3]  = 1.0f;   /* w = 1 -> projective divide is no-op */
    v->col[0]  = 1.0f;
    v->col[1]  = 1.0f;
    v->col[2]  = 1.0f;
    v->col[3]  = 1.0f;
}

/* Emit two triangles covering (x0..x1, y0..y1) sampling a fixed
 * quadrant of the texture. UVs are normalized; (u_c, v_c) is the
 * quadrant center in [0..1]. */
static void emit_quad_uv(TexVertex *out, int x0, int y0, int x1, int y1,
                         float u_c, float v_c)
{
    mk_vert(&out[0], x0, y0, u_c, v_c);
    mk_vert(&out[1], x1, y0, u_c, v_c);
    mk_vert(&out[2], x1, y1, u_c, v_c);
    mk_vert(&out[3], x0, y0, u_c, v_c);
    mk_vert(&out[4], x1, y1, u_c, v_c);
    mk_vert(&out[5], x0, y1, u_c, v_c);
}

static void build_geometry(void)
{
    /* Per-quadrant UV centers in [0..1]. Q0 = top-left -> (0.25,0.25);
     * Q1 = top-right -> (0.75,0.25); Q2 = bottom-left -> (0.25,0.75);
     * Q3 = bottom-right -> (0.75,0.75). NEAREST filter rounds to the
     * nearest texel, which for mips with w >= 2 lands within the
     * intended quadrant. For mip 6 (1x1) any UV samples the single
     * texel (= Q0's tinted color); the cell paints all 4 quadrants
     * the same color which is the expected behavior. */
    static const float q_uv[4][2] = {
        {0.25f, 0.25f},  /* Q0 TL */
        {0.75f, 0.25f},  /* Q1 TR */
        {0.25f, 0.75f},  /* Q2 BL */
        {0.75f, 0.75f},  /* Q3 BR */
    };
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx     = row * GRID_COLS + col;
            const int cell_x0 = col * CELL_W;
            const int cell_y0 = row * CELL_H;
            const int half_w  = CELL_W / 2;
            const int half_h  = CELL_H / 2;
            for (int q = 0; q < 4; q++) {
                const int qx = q & 1;
                const int qy = q >> 1;
                const int x0 = cell_x0 + qx * half_w;
                const int y0 = cell_y0 + qy * half_h;
                const int x1 = x0 + half_w;
                const int y1 = y0 + half_h;
                emit_quad_uv(
                    &s_verts[idx * VERTS_PER_CELL + q * VERTS_PER_QUAD],
                    x0, y0, x1, y1, q_uv[q][0], q_uv[q][1]);
            }
        }
    }
}

/* --- NV097 plumbing ---------------------------------------------- */

static void enforce_common_state(void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE,   0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK,          0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE,        0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE,   0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE,    0);
    pb_end(p);
}

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(TexVertex), &s_alloc_verts[0].pos[0]);
    xbed_set_attrib_pointer(
        9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(TexVertex), &s_alloc_verts[0].tex[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(TexVertex), &s_alloc_verts[0].col[0]);
}

/* Set the full-mip texture binding once. Per-cell variation goes only
 * through SET_TEXTURE_CONTROL0 LOD-clamp updates below. */
static void bind_full_mipchain(void)
{
    XbedTextureStage0 params;
    xbed_texture_init_argb8888_defaults(&params);
    params.vram_addr        = s_tex_vram;
    params.width            = BASE_TEX_W;
    params.height           = BASE_TEX_H;
    params.color_format     = SZ_A8R8G8B8_CODE;
    params.base_size_u_log2 = BASE_TEX_LOG2;
    params.base_size_v_log2 = BASE_TEX_LOG2;
    params.mipmap_levels    = MIP_LEVELS;
    params.pitch_bytes      = 0;  /* unused for swizzled */
    /* Use BOX_NEARESTLOD (filter mode 3) so the sampler picks
     * exactly one mip level (no inter-mip interpolation) per the
     * LOD clamps. Within the chosen mip, NEAREST tap. */
    params.min_filter       = 3;  /* BOX_NEARESTLOD */
    params.mag_filter       = 1;  /* NEAREST */
    params.wrap_s           = 3;  /* CLAMP_TO_EDGE */
    params.wrap_t           = 3;  /* CLAMP_TO_EDGE */
    params.dma_channel      = 0;
    xbed_texture_bind_stage0(&params);
}

/* SET_TEXTURE_CONTROL0 bit layout (mirrored from nv_regs.h /
 * xbed_texture.c so we can rewrite just the LOD clamp fields):
 *   bit 30        ENABLE
 *   bits 18..29   MIN_LOD_CLAMP (12-bit raw integer level on xemu)
 *   bits  6..17   MAX_LOD_CLAMP (12-bit raw integer level on xemu)
 *
 * On xemu both fields are interpreted as plain integer mip levels
 * via `GET_MASK` in `pgraph_get_texture_shape`. We write the level
 * number directly. (Real-Xbox may use fixed-point; tracked at the
 * top-of-file caveats. The Metal renderer fix landed 2026-05-21
 * propagates these to MTLSamplerDescriptor.lodMin/Max Clamp.) */
#define LOD_ENABLE_BIT          (1u << 30)
#define LOD_MIN_SHIFT           18
#define LOD_MAX_SHIFT           6
#define LOD_FIELD_MASK          0xFFFu

static void set_lod_clamp(int min_level, int max_level)
{
    uint32_t ctl0 = LOD_ENABLE_BIT;
    ctl0 |= ((uint32_t)(min_level & LOD_FIELD_MASK)) << LOD_MIN_SHIFT;
    ctl0 |= ((uint32_t)(max_level & LOD_FIELD_MASK)) << LOD_MAX_SHIFT;
    uint32_t *p = pb_begin();
    /* Stage 0 control0: base register, +0 stage offset. */
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, ctl0);
    pb_end(p);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();
    bind_full_mipchain();

    for (int idx = 0; idx < GRID_CELLS; idx++) {
        const int mip = k_cell_mip[idx];
        if (mip < 0) continue;
        set_lod_clamp(mip, mip);
        xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                         idx * VERTS_PER_CELL, VERTS_PER_CELL);
    }
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("swizzle-mipmap v0.2\n");

    xbed_set_default_render_state();
    xbed_load_textured_shaders();

    compute_mip_layout();
    uint32_t alloc_bytes = (s_mip_total_bytes + 0xFFFu) & ~0xFFFu;

    s_tex_vram = MmAllocateContiguousMemoryEx(
        alloc_bytes, 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_tex_vram) {
        debugPrint("MmAllocateContiguousMemoryEx (tex) failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    fill_mip_data();

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

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\swizzle-mipmap-capture.bin",
        "D:\\swizzle-mipmap-done.txt",
        "swizzle-mipmap");
    return 0;
}
