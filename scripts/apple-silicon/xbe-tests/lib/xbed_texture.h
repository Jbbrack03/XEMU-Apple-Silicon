/*
 * xbed_texture — shared texture-stage helpers for Tier-1 diag XBEs.
 *
 * Closes the infrastructure gap that blocked §4.7/4.8/4.11/4.13/4.16
 * texture-cluster XBEs (per `diagnostic-xbe-plan.md` v2 §4 + handoff
 * 2026-05-20 evening). Wraps the pbkit-level NV097 texture state
 * machine + the pixel-shader binding so individual texture XBEs only
 * have to express their per-format / per-filter / per-wrap case.
 *
 * --- Design contract --------------------------------------------------
 *
 * - Texture data lives in caller-allocated VRAM (the diag XBE owns
 *   `MmAllocateContiguousMemoryEx`-backed storage). xbed_texture
 *   does NOT allocate VRAM; it accepts a populated buffer pointer
 *   so the XBE can pre-populate test patterns deterministically at
 *   XBE init time and re-use them every frame.
 *
 * - Only stage 0 is bound by xbed_texture_bind_stage0(). Stages
 *   1..3 are explicitly disabled so a stale stage-N state from a
 *   prior XBE in the rotation can't leak in.
 *
 * - Format-related fields (color enum, base_size_u/v, mipmap_levels,
 *   pitch, image_rect) are caller-supplied. xbed_texture does not
 *   guess them from width/height because swizzled (SZ_) formats
 *   require log2 powers-of-two while linear (LU_IMAGE_) formats use
 *   plain pixel counts -- the test author has to choose.
 *
 * - Filter and wrap modes are caller-supplied via raw NV097
 *   enums so the same helper covers both nearest/linear and
 *   the four supported wrap modes (CLAMP_TO_EDGE / CLAMP / WRAP /
 *   MIRROR / BORDER per the NV097_SET_TEXTURE_ADDRESS encoding).
 *
 * - The combiner / pixel-shader stage is set up via the new
 *   `xbed_load_textured_shaders()` entry in xbed_runtime: the
 *   stage 0 sample is routed to COLOR. Tests that need a custom
 *   combiner program load their own shaders instead and call only
 *   the bind helper here.
 */
#ifndef XBED_TEXTURE_H
#define XBED_TEXTURE_H

#include <stdint.h>

/* --- Texture stage 0 binding ---------------------------------------- */

/* Per-test parameters for binding a single texture at stage 0. All
 * fields are caller-supplied; xbed_texture does not derive any
 * (see header for rationale). */
typedef struct {
    /* VRAM pointer to texture data. Must be contiguous-allocated
     * via MmAllocateContiguousMemoryEx; xbed_texture does NOT
     * allocate or free this. */
    void     *vram_addr;

    /* Pixel dimensions of the texture mip 0. For swizzled SZ_
     * formats these MUST be powers of two and consistent with
     * base_size_u/v (which carry log2 of the dimensions). For
     * linear LU_IMAGE_ formats these are plain pixel counts. */
    uint32_t  width;
    uint32_t  height;

    /* Raw NV097_SET_TEXTURE_FORMAT_COLOR_* enum (8-bit value).
     * E.g. NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 (0x12). */
    uint32_t  color_format;

    /* For swizzled (SZ_) formats: log2 of width / height. For
     * linear formats these are ignored; xbed_texture passes them
     * as zero in NV097_SET_TEXTURE_FORMAT. */
    uint32_t  base_size_u_log2;
    uint32_t  base_size_v_log2;

    /* Number of mipmap levels (>= 1). Most diag XBEs ship with 1. */
    uint32_t  mipmap_levels;

    /* For linear formats: row pitch in bytes. For swizzled formats
     * this is ignored (the layout is implicit). */
    uint32_t  pitch_bytes;

    /* Raw NV097_SET_TEXTURE_FILTER_{MIN,MAG} enum. Default
     * NEAREST/NEAREST so byte-exact comparison is possible; set to
     * LINEAR/LINEAR for filter-mode tests. */
    uint32_t  min_filter;
    uint32_t  mag_filter;

    /* Wrap modes for S / T axes (NV097_SET_TEXTURE_ADDRESS U/V).
     * 1 = WRAP, 2 = MIRROR, 3 = CLAMP_TO_EDGE, 4 = BORDER,
     * 5 = CLAMP. Defaults to CLAMP_TO_EDGE for the simplest case. */
    uint32_t  wrap_s;
    uint32_t  wrap_t;

    /* DMA channel for the texture (0 = NV_DMA_A typical;
     * 2 = NV_DMA_B). Stored in NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA. */
    uint32_t  dma_channel;
} XbedTextureStage0;

/* Bind the parameters to stage 0 and explicitly disable stages 1..3.
 * Caller is responsible for ensuring no other code touches stage 0
 * between this call and the subsequent draw. */
void xbed_texture_bind_stage0(const XbedTextureStage0 *params);

/* Disable all 4 texture stages (clear-state helper for between-cell
 * setups when a test renders some cells with texturing and others
 * without). */
void xbed_texture_disable_all_stages(void);

/* --- Convenience builders ------------------------------------------- */

/* Populate `params` with defaults suitable for a linear A8R8G8B8
 * texture: NEAREST/NEAREST filter, CLAMP_TO_EDGE/CLAMP_TO_EDGE wrap,
 * mipmap_levels=1, dma_channel=0, base_size_u/v_log2=0, pitch =
 * width * 4. Caller still needs to set `vram_addr`, `width`, and
 * `height` before calling xbed_texture_bind_stage0(). */
void xbed_texture_init_argb8888_defaults(XbedTextureStage0 *params);

#endif /* XBED_TEXTURE_H */
