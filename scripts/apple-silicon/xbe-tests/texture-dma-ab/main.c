/*
 * texture-dma-ab — texture-stage DMA channel selector (A vs B) v0.1
 *                  Tier-1 NV2A diag XBE.
 *
 * NV2A feature exercised: §E.14 (texture DMA selector).
 * NV097 methods:           SET_TEXTURE_FORMAT (CONTEXT_DMA field),
 *                          SET_CONTEXT_DMA_A / _B (read from pgraph
 *                          state set by pbkit init; not overridden).
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests (v0.1 narrow scope) -------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.16 (post-Codex-finding-#8): a
 * 2-cell left/right grid where both cells point at the SAME texture
 * data but bind it via DIFFERENT FORMAT_CONTEXT_DMA selector values
 * (left = 0; right = 2). xemu's `pgraph.c:2679-2680` only promotes
 * `CONTEXT_DMA == 2` to internal `dma_select=true` (which then maps
 * to `pg->dma_b` in `texture.c:101`); other CONTEXT_DMA values map
 * to channel A. So the field encoding is 0=A, 2=B (not the natural
 * 0/1).
 *
 * v0.1 is a SMOKE TEST UNDER PBKIT-DEFAULT ALIASING (Codex review
 * 2026-05-21). Under nxdk pbkit's default boot setup
 * (`nxdk/lib/pbkit/pbkit.c::pb_init`), both `SET_CONTEXT_DMA_A` and
 * `SET_CONTEXT_DMA_B` are programmed to point at the same RAMIN
 * object (#3), which resolves to base 0 of VRAM. So even with the
 * correct selector value (=2), the two halves sample from the SAME
 * underlying VRAM region and the test cannot distinguish a routing
 * regression that swaps DMA A and DMA B internally.
 *
 * v0.1 IS useful as:
 *   - A non-zero CONTEXT_DMA field round-trip test: that writing
 *     CONTEXT_DMA=2 to NV097_SET_TEXTURE_FORMAT doesn't crash the
 *     texture-bind path and produces RED output (i.e. doesn't
 *     accidentally null out the texture pointer).
 *   - A documentation oracle: the source + manifest record the
 *     correct field encoding (0=A, 2=B) and the pbkit-aliasing
 *     caveat that makes proper A-vs-B testing require RAMIN setup.
 *
 * v0.1 is NOT useful as a regression gate for:
 *   - Per-channel base-address translation (pbkit aliases the
 *     channels). Tracked as v0.2 follow-up + task #18.
 *   - Selector mask alignment in `pgraph.c:SET_TEXTURE_FORMAT`
 *     (false-pass risk under aliasing).
 *
 * v0.2 needs (deferred):
 *   - `xbed_dma` helper that writes a custom DMA object to RAMIN
 *     with `base != 0`.
 *   - Two separate texture allocations at distinct VRAM offsets.
 *   - Update pgraph's DMA-A handle (or DMA-B handle) at runtime
 *     via NV097_SET_CONTEXT_DMA_A/_B with the new RAMIN handle.
 *   - Identical texture content at both VRAM bases.
 *   - Both halves rendering the same color proves the per-channel
 *     base-address translation works correctly.
 *
 * --- Expected output ------------------------------------------------
 *
 * 640x480 framebuffer. Left half (x 0..319) renders an SZ_A8R8G8B8
 * texture sampled via DMA A. Right half (x 320..639) renders the
 * SAME texture data via DMA B. Both halves show the same per-half
 * solid color (RED at full saturation = 0xFF in R channel, 0 in
 * G/B channels). The split point at x = 320 should NOT be visible
 * (no color discontinuity).
 *
 * Test texture data: 1x1 SZ_A8R8G8B8 = single ARGB pixel = RED
 * (0xFFFF0000 little-endian DWORD = bytes 0x00, 0x00, 0xFF, 0xFF for
 * BGRA). The texture's tiny dimensions keep the swizzle layout
 * trivial (single texel, no Z-order to apply). Catches the DMA
 * selector decode without depending on intra-texel swizzle
 * correctness (task #16 captures that gap for SZ_A8R8G8B8 today).
 *
 * --- Reproducibility ------------------------------------------------
 *
 * Pure deterministic; texture set up once, same draw every frame.
 * Byte-identical across two cold runs.
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
#define HALF_W (WIN_W / 2)

#define VERTS_PER_QUAD 6
#define CELLS          2
#define VERTS_TOTAL    (CELLS * VERTS_PER_QUAD)

#define SZ_A8R8G8B8_CODE 0x06

typedef struct {
    float pos[3];
    float tex[4];   /* w=1 to neutralize 2D_PROJECTIVE divide */
    float col[4];
} __attribute__((packed)) TexVertex;

static TexVertex   s_verts[VERTS_TOTAL];
static TexVertex  *s_alloc_verts;
static uint8_t    *s_tex_vram;
#define TEX_W 1
#define TEX_H 1
/* 1x1 SZ_A8R8G8B8 = 4 bytes of BGRA. RED pixel. */
#define TEX_BYTES 4

static inline void mk_vert(TexVertex *v, int x_w, int y_w,
                           float u, float vc)
{
    v->pos[0]  = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1]  = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2]  = 0.5f;
    v->tex[0]  = u;
    v->tex[1]  = vc;
    v->tex[2]  = 0.0f;
    v->tex[3]  = 1.0f;
    v->col[0]  = 1.0f;
    v->col[1]  = 1.0f;
    v->col[2]  = 1.0f;
    v->col[3]  = 1.0f;
}

static void emit_quad(TexVertex *out, int x0, int y0, int x1, int y1)
{
    /* Constant UV across all 6 verts; 1x1 texture so any UV samples
     * the single texel. */
    const float u = 0.5f;
    const float v = 0.5f;
    mk_vert(&out[0], x0, y0, u, v);
    mk_vert(&out[1], x1, y0, u, v);
    mk_vert(&out[2], x1, y1, u, v);
    mk_vert(&out[3], x0, y0, u, v);
    mk_vert(&out[4], x1, y1, u, v);
    mk_vert(&out[5], x0, y1, u, v);
}

static void build_geometry(void)
{
    /* Left half: x 0..HALF_W; right half: HALF_W..WIN_W. Each full
     * height. */
    emit_quad(&s_verts[0],                  0,        0, HALF_W,  WIN_H);
    emit_quad(&s_verts[VERTS_PER_QUAD],  HALF_W,      0, WIN_W,   WIN_H);
}

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

/* dma_channel: raw 2-bit value written to NV097_SET_TEXTURE_FORMAT
 * CONTEXT_DMA. xemu's pgraph_set_texture_format treats only value
 * == 2 as selecting DMA B; 0/1/3 map to DMA A. */
static void bind_texture_via_channel(uint32_t dma_channel)
{
    XbedTextureStage0 params;
    xbed_texture_init_argb8888_defaults(&params);
    params.vram_addr        = s_tex_vram;
    params.width            = TEX_W;
    params.height           = TEX_H;
    params.color_format     = SZ_A8R8G8B8_CODE;
    params.base_size_u_log2 = 0;  /* log2(1) */
    params.base_size_v_log2 = 0;
    params.mipmap_levels    = 1;
    params.pitch_bytes      = 0;
    params.min_filter       = 1;  /* NEAREST */
    params.mag_filter       = 1;  /* NEAREST */
    params.wrap_s           = 3;  /* CLAMP_TO_EDGE */
    params.wrap_t           = 3;  /* CLAMP_TO_EDGE */
    params.dma_channel      = dma_channel;  /* 0 = A, 1 = B */
    xbed_texture_bind_stage0(&params);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    /* Left half: DMA A. */
    bind_texture_via_channel(0);
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                     0, VERTS_PER_QUAD);

    /* Right half: DMA B selector. Per
     * `hw/xbox/nv2a/pgraph/pgraph.c:2679-2680`, only CONTEXT_DMA == 2
     * promotes the internal `dma_select` to 1 (DMA B); other values
     * (including 1) map to A. So the field encoding is 0=A, 2=B. */
    bind_texture_via_channel(2);
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES,
                     VERTS_PER_QUAD, VERTS_PER_QUAD);
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("texture-dma-ab v0.1\n");

    xbed_set_default_render_state();
    xbed_load_textured_shaders();

    /* Allocate 1 page (4KB) for the texture; we only use 4 bytes. */
    s_tex_vram = (uint8_t *)MmAllocateContiguousMemoryEx(
        0x1000, 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_tex_vram) {
        debugPrint("MmAllocateContiguousMemoryEx (tex) failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    /* SZ_A8R8G8B8 stored on little-endian as B G R A bytes per pixel.
     * RED = (R=255, G=0, B=0, A=255). */
    s_tex_vram[0] = 0x00;  /* B */
    s_tex_vram[1] = 0x00;  /* G */
    s_tex_vram[2] = 0xFF;  /* R */
    s_tex_vram[3] = 0xFF;  /* A */

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
        "D:\\texture-dma-ab-capture.bin",
        "D:\\texture-dma-ab-done.txt",
        "texture-dma-ab");
    return 0;
}
