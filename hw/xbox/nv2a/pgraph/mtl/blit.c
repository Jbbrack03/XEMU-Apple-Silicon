/*
 * NV2A PGRAPH Metal renderer — NV097_IMAGE_BLIT handler
 * (slice M5.9-followup-A, 2026-05-03).
 *
 * Mirrors hw/xbox/nv2a/pgraph/vk/blit.c and gl/blit.c structurally:
 *   1. Compute source / destination VRAM pointers via nv_dma_map.
 *   2. Memcpy the source rect into the destination rect on host VRAM —
 *      this is the correctness oracle and matches the vk/gl blit
 *      authoritative path. Guest-side reads of the destination region
 *      after the blit see the right pixels.
 *   3. Mark the destination range dirty so other consumers (texture
 *      cache, VGA scan-out for the legacy display path) re-fetch.
 *
 * Step 4 (Metal-specific): also issue a GPU-side rect-to-rect copy
 * between MTLTextures so the surface cache's resolved texture (which
 * the CRTC publish reads at flip_stall) reflects the rendered scene
 * content. This is what the M5.9 cache wants in addition to the CPU
 * memcpy: vk/gl run the inverse flow (download dirty surface, memcpy,
 * mark upload_pending; next bind re-uploads). M5.9 has VRAM upload on
 * cache-allocate but no CPU-write callback yet, so we copy on the GPU
 * directly when both src and dst are cache-resident with matching
 * pixel format. Format mismatch invalidates the dst cache entry so the
 * next bind uploads the freshly-memcpy'd VRAM contents.
 *
 * The file extension is .c (not .m / .mm) so the per-target preprocessor
 * flags (-DCOMPILING_PER_TARGET, etc.) propagate via specific_ss; the
 * file can include nv2a_int.h for the renderer-side state machine and
 * for the DMA / memory-region helpers. ObjC/Metal API usage stays in
 * surface.mm behind the C-callable surface.h interface.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"

#include "draw.h"
#include "surface.h"

static void perform_blit_cpu(int operation, uint8_t *source, uint8_t *dest,
                             size_t width, size_t height, size_t width_bytes,
                             size_t source_pitch, size_t dest_pitch,
                             BetaState *beta)
{
    /* Mirror of vk/blit.c::perform_blit and gl/blit.c::perform_blit.
     * Kept literal so any future correctness fix in those files is a
     * trivial port across. */
    if (operation == NV09F_SET_OPERATION_SRCCOPY) {
        for (unsigned int y = 0; y < height; y++) {
            memmove(dest, source, width_bytes);
            source += source_pitch;
            dest += dest_pitch;
        }
    } else if (operation == NV09F_SET_OPERATION_BLEND_AND) {
        uint32_t max_beta_mult = 0x7f80;
        uint32_t beta_mult = beta->beta >> 16;
        uint32_t inv_beta_mult = max_beta_mult - beta_mult;

        for (unsigned int y = 0; y < height; y++) {
            uint8_t *s = source;
            uint8_t *d = dest;
            for (unsigned int x = 0; x < width; x++) {
                for (unsigned int ch = 0; ch < 3; ch++) {
                    uint32_t a = s[x * 4 + ch] * beta_mult;
                    uint32_t b = d[x * 4 + ch] * inv_beta_mult;
                    d[x * 4 + ch] = (a + b) / max_beta_mult;
                }
            }
            source += source_pitch;
            dest += dest_pitch;
        }
    } else {
        fprintf(stderr, "Unknown blit operation: 0x%x\n", operation);
        assert(false && "Unknown blit operation");
    }
}

static void patch_alpha(uint8_t *dest, size_t width_pixels, size_t height,
                        size_t dest_pitch, uint8_t alpha_val)
{
    for (unsigned int y = 0; y < height; y++) {
        uint8_t *d = dest;
        for (unsigned int x = 0; x < width_pixels; x++) {
            d[x * 4 + 3] = alpha_val;
        }
        dest += dest_pitch;
    }
}

void pgraph_mtl_image_blit(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    ContextSurfaces2DState *context_surfaces = &pg->context_surfaces_2d;
    ImageBlitState *image_blit = &pg->image_blit;
    BetaState *beta = &pg->beta;

    /* Drain any open coalesced render pass before we read or modify
     * MTLTexture contents. The vk path's surface_update(false, true,
     * true) handles the same concern via its render-pass scoping; mtl
     * needs the explicit flush since open_pass coalescing crosses
     * arbitrary draws. */
    pgraph_mtl_draw_flush_open_pass();

    /* The vk/gl renderers call surface_update at this point to download
     * any dirty surface back to VRAM so the CPU memcpy below sees fresh
     * pixels. For mtl, surface_update is a structural no-op (M5.9
     * deferred VRAM download to followup-B) — but the M5.9 cache
     * promotes the on-cache MTLTexture as the authoritative state. The
     * GPU-side copy below propagates the texture-side rendered content
     * directly to the destination texture, sidestepping the VRAM
     * round-trip. */

    assert(context_surfaces->object_instance == image_blit->context_surfaces);

    unsigned int bytes_per_pixel;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        bytes_per_pixel = 1;
        break;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        bytes_per_pixel = 2;
        break;
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_Y32:
        bytes_per_pixel = 4;
        break;
    default:
        fprintf(stderr, "Unknown blit surface format: 0x%x\n",
                context_surfaces->color_format);
        assert(false);
        break;
    }

    hwaddr source_dma_len;
    uint8_t *source = (uint8_t *)nv_dma_map(
        d, context_surfaces->dma_image_source, &source_dma_len);
    assert(context_surfaces->source_offset < source_dma_len);
    source += context_surfaces->source_offset;
    hwaddr source_addr = source - d->vram_ptr;

    hwaddr dest_dma_len;
    uint8_t *dest = (uint8_t *)nv_dma_map(d, context_surfaces->dma_image_dest,
                                          &dest_dma_len);
    assert(context_surfaces->dest_offset < dest_dma_len);
    dest += context_surfaces->dest_offset;
    hwaddr dest_addr = dest - d->vram_ptr;

    hwaddr source_offset = image_blit->in_y * context_surfaces->source_pitch +
                           image_blit->in_x * bytes_per_pixel;
    hwaddr dest_offset = image_blit->out_y * context_surfaces->dest_pitch +
                         image_blit->out_x * bytes_per_pixel;

    size_t max_row_pixels =
        MIN(context_surfaces->source_pitch, context_surfaces->dest_pitch) /
        bytes_per_pixel;
    size_t row_pixels = MIN(max_row_pixels, image_blit->width);

    hwaddr dest_size = (image_blit->height - 1) * context_surfaces->dest_pitch +
                       image_blit->width * bytes_per_pixel;

    uint8_t *source_row = source + source_offset;
    uint8_t *dest_row = dest + dest_offset;
    size_t row_bytes = row_pixels * bytes_per_pixel;

    size_t adjusted_height = image_blit->height;
    size_t leftover_bytes = 0;

    hwaddr clipped_dest_size =
        nv_clip_gpu_tile_blit(d, dest_addr + dest_offset, dest_size);

    if (clipped_dest_size < dest_size) {
        adjusted_height = clipped_dest_size / context_surfaces->dest_pitch;
        size_t consumed_bytes = adjusted_height * context_surfaces->dest_pitch;

        leftover_bytes = clipped_dest_size - consumed_bytes;
    }

    NV2A_DPRINTF("  blit 0x%tx -> 0x%tx (Size: %llu, Clipped Height: %zu)\n",
                 source_addr, dest_addr,
                 (unsigned long long)dest_size, adjusted_height);

    /* CPU-side memcpy — keeps guest VRAM correct (vk/gl parity). */
    if (adjusted_height > 0) {
        perform_blit_cpu(image_blit->operation, source_row, dest_row,
                         row_pixels, adjusted_height, row_bytes,
                         context_surfaces->source_pitch,
                         context_surfaces->dest_pitch, beta);
    }

    if (leftover_bytes > 0) {
        uint8_t *src =
            source_row + adjusted_height * context_surfaces->source_pitch;
        uint8_t *dst_leftover =
            dest_row + adjusted_height * context_surfaces->dest_pitch;

        perform_blit_cpu(image_blit->operation, src, dst_leftover,
                         leftover_bytes / bytes_per_pixel, 1, leftover_bytes,
                         context_surfaces->source_pitch,
                         context_surfaces->dest_pitch, beta);
    }

    bool needs_alpha_patching;
    uint8_t alpha_override;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0xff;
        break;
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0;
        break;
    default:
        needs_alpha_patching = false;
        alpha_override = 0;
    }

    if (needs_alpha_patching) {
        if (adjusted_height > 0) {
            patch_alpha(dest_row, row_pixels, adjusted_height,
                        context_surfaces->dest_pitch, alpha_override);
        }

        if (leftover_bytes > 0) {
            uint8_t *dst_leftover =
                dest_row + adjusted_height * context_surfaces->dest_pitch;
            patch_alpha(dst_leftover, leftover_bytes / 4, 1, 0, alpha_override);
        }
    }

    /* GPU-side blit between MTLTextures — propagate the rendered scene
     * pixels from the source surface (typically the back buffer) to the
     * destination surface (typically the front buffer). This is what
     * lets the CRTC publish at flip_stall pick up content rather than
     * the stale clear color. The function looks up both src/dst by
     * vram_addr in the per-VRAM cache; mismatched / missing entries
     * fall through to invalidation so the next bind reallocates with a
     * fresh upload-from-VRAM (the memcpy above is the source of truth).
     *
     * Cache lookup is by the SURFACE BASE vram_addr (source_addr /
     * dest_addr — these are the DMA-mapped surface's first byte in
     * guest VRAM, not yet offset by in_x/in_y). The rect coordinates
     * (in_x, in_y) / (out_x, out_y) are passed in guest 1x pixel space;
     * the surface manager scales them to host MTLTexture pixel space
     * internally using the cached binding's allocated dimensions. */
    pgraph_mtl_surface_blit_copy(
        (uint32_t)source_addr,
        (uint32_t)dest_addr,
        image_blit->in_x, image_blit->in_y,
        image_blit->out_x, image_blit->out_y,
        image_blit->width, (uint32_t)adjusted_height);

    dest_addr += dest_offset;
    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_NV2A_TEX);
}
