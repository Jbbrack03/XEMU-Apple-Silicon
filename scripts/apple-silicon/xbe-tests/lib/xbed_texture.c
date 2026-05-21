/*
 * xbed_texture — texture-stage helpers implementation.
 *
 * See xbed_texture.h for the API contract. This module is NOT included
 * by lib.mk yet -- it ships as an unused module for the next session
 * to wire up + author the §4.7/4.8/4.11/4.13/4.16 texture-cluster XBEs
 * on top. The pbkit register methods + bit layouts have been
 * cross-checked against:
 *   - `hw/xbox/nv2a/nv2a_regs.h` (NV097_SET_TEXTURE_* canonical defs)
 *   - `/Users/jbbrack03/XEMU_MacOS/nxdk/lib/pbkit/nv_regs.h` (matching
 *     pbkit-side header)
 *   - `/Users/jbbrack03/XEMU_MacOS/nxdk/samples/mesh/main.c:139-163`
 *     (live texture stage-0 binding sequence used by the nxdk mesh
 *     sample; same pattern we mirror here)
 */
#include "xbed_texture.h"

#include <pbkit/pbkit.h>
#include <stdint.h>

/* NV097 SET_TEXTURE_FORMAT bit layout (mask, shift) reproduced from
 * nv2a_regs.h. We compose the format word here rather than using
 * pbkit's macros so the test author can see exactly which bits we
 * touch -- diag XBEs must be auditable from the source. */
#define XBED_FMT_CONTEXT_DMA_MASK   0x00000003u
#define XBED_FMT_CUBEMAP_ENABLE_BIT (1u << 2)
#define XBED_FMT_BORDER_SOURCE_BIT  (1u << 3)
#define XBED_FMT_DIMENSIONALITY     (2u << 4)  /* 2D = 2 */
#define XBED_FMT_COLOR_SHIFT        8
#define XBED_FMT_MIPMAP_SHIFT       16
#define XBED_FMT_BASE_SIZE_U_SHIFT  20
#define XBED_FMT_BASE_SIZE_V_SHIFT  24
#define XBED_FMT_BASE_SIZE_P_SHIFT  28

/* NV097 SET_TEXTURE_ADDRESS bit layout. We only touch U and V (S, T)
 * here; cylinder-wrap bits remain 0; P (R-axis for 3D / cube) is set
 * to CLAMP_TO_EDGE for safety. */
#define XBED_ADDR_U_SHIFT           0
#define XBED_ADDR_V_SHIFT           8
#define XBED_ADDR_P_SHIFT           16
#define XBED_ADDR_CLAMP_TO_EDGE     3u

/* NV097 SET_TEXTURE_FILTER bit layout. We set MIN (bits 16-19) and
 * MAG (bits 24-27). LOD bias remains 0; aniso bias remains 0. The
 * sign bits (ASIGNED/RSIGNED/GSIGNED/BSIGNED) are left 0 (unsigned
 * texture interpretation). */
#define XBED_FILT_MIN_SHIFT         16
#define XBED_FILT_MAG_SHIFT         24

/* NV097 SET_TEXTURE_CONTROL0 bit layout. ENABLE is bit 30. LOD clamps
 * we leave wide-open at min=0, max=0xFFF (the field is 12-bit). */
#define XBED_CTRL0_ENABLE_BIT       (1u << 30)
#define XBED_CTRL0_MIN_LOD_SHIFT    18
#define XBED_CTRL0_MAX_LOD_SHIFT    6
#define XBED_CTRL0_MAX_LOD_OPEN     0xFFFu

/* NV097 SET_TEXTURE_IMAGE_RECT bit layout: (width << 16) | height. */

void xbed_texture_init_argb8888_defaults(XbedTextureStage0 *params)
{
    if (params == 0) return;
    /* Caller fills vram_addr, width, height. */
    params->color_format     = 0x12;  /* LU_IMAGE_A8R8G8B8 */
    params->base_size_u_log2 = 0;
    params->base_size_v_log2 = 0;
    params->mipmap_levels    = 1;
    params->pitch_bytes      = 0;     /* caller sets to width * 4 */
    params->min_filter       = 1;     /* NEAREST */
    params->mag_filter       = 1;     /* NEAREST */
    params->wrap_s           = XBED_ADDR_CLAMP_TO_EDGE;
    params->wrap_t           = XBED_ADDR_CLAMP_TO_EDGE;
    params->dma_channel      = 0;
}

void xbed_texture_bind_stage0(const XbedTextureStage0 *p)
{
    if (p == 0) return;
    /* The texture offset is the low 28 bits of the VRAM-physical
     * address. The xbox is always running KSEG0-cached at
     * 0x80000000+; the NV2A wants the physical address which is the
     * VRAM offset directly. MmAllocateContiguousMemoryEx returns
     * cacheable pointers; `& 0x03ffffff` masks to the 64 MB VRAM
     * window, matching nxdk samples/mesh's pattern at line 145. */
    uint32_t offset = ((uint32_t)(uintptr_t)p->vram_addr) & 0x03ffffffu;

    /* Compose TEXTURE_FORMAT word. */
    uint32_t fmt = 0;
    fmt |= (p->dma_channel & XBED_FMT_CONTEXT_DMA_MASK);
    fmt |= XBED_FMT_DIMENSIONALITY;                              /* 2D */
    fmt |= ((p->color_format & 0xFFu) << XBED_FMT_COLOR_SHIFT);
    fmt |= ((p->mipmap_levels & 0xFu) << XBED_FMT_MIPMAP_SHIFT);
    fmt |= ((p->base_size_u_log2 & 0xFu) << XBED_FMT_BASE_SIZE_U_SHIFT);
    fmt |= ((p->base_size_v_log2 & 0xFu) << XBED_FMT_BASE_SIZE_V_SHIFT);
    /* BASE_SIZE_P = 0 (2D only). */

    /* Compose TEXTURE_ADDRESS word. */
    uint32_t addr = 0;
    addr |= ((p->wrap_s & 0xFu) << XBED_ADDR_U_SHIFT);
    addr |= ((p->wrap_t & 0xFu) << XBED_ADDR_V_SHIFT);
    addr |= (XBED_ADDR_CLAMP_TO_EDGE << XBED_ADDR_P_SHIFT);

    /* Compose TEXTURE_FILTER word. */
    uint32_t filt = 0;
    filt |= ((p->min_filter & 0xFu) << XBED_FILT_MIN_SHIFT);
    filt |= ((p->mag_filter & 0xFu) << XBED_FILT_MAG_SHIFT);

    /* Compose TEXTURE_CONTROL0 word. */
    uint32_t ctrl0 = 0;
    ctrl0 |= XBED_CTRL0_ENABLE_BIT;
    ctrl0 |= (XBED_CTRL0_MAX_LOD_OPEN << XBED_CTRL0_MAX_LOD_SHIFT);

    /* Compose TEXTURE_CONTROL1 word: bits 16-31 = image pitch. Only
     * meaningful for linear LU_IMAGE_ formats. */
    uint32_t ctrl1 = (p->pitch_bytes << 16);

    /* Compose IMAGE_RECT word. */
    uint32_t rect = ((p->width & 0xFFFFu) << 16) | (p->height & 0xFFFFu);

    /* Emit the stage-0 setup as a single pb_begin / pb_end pair so
     * the command buffer fragment is atomic from the NV2A's
     * perspective. */
    uint32_t *cmd = pb_begin();
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_OFFSET,     offset);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_FORMAT,     fmt);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_ADDRESS,    addr);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL0,   ctrl0);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL1,   ctrl1);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_FILTER,     filt);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_IMAGE_RECT, rect);
    pb_end(cmd);

    /* Stages 1..3 disabled to prevent stale state from a previous
     * XBE in the rotation leaking through. */
    uint32_t off_ctrl0 = 0;  /* ENABLE bit clear */
    cmd = pb_begin();
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL0 + (1 << 6), off_ctrl0);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL0 + (2 << 6), off_ctrl0);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL0 + (3 << 6), off_ctrl0);
    pb_end(cmd);
}

void xbed_texture_disable_all_stages(void)
{
    uint32_t off_ctrl0 = 0;
    uint32_t *cmd = pb_begin();
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL0 + (0 << 6), off_ctrl0);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL0 + (1 << 6), off_ctrl0);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL0 + (2 << 6), off_ctrl0);
    cmd = pb_push1(cmd, NV097_SET_TEXTURE_CONTROL0 + (3 << 6), off_ctrl0);
    pb_end(cmd);
}
