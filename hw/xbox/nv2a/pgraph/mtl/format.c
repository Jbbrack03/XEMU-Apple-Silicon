/*
 * NV2A PGRAPH Metal renderer — NV2A→MTLPixelFormat tables (slice M7.1 / M6 Part B).
 *
 * See format.h for the public contract.
 *
 * The CPU conversion path (pgraph_convert_texture_data /
 * s3tc_decompress_2d / unswizzle_rect) returns RGBA8 / BGRA8 for every
 * NV2A color format the uploader handles, so the host-side
 * MTLPixelFormat is one of two values in practice. The flags signal
 * which CPU pre-processing the upload path needs.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"

#include "format.h"

#include <stdbool.h>
#include <stdint.h>

uint32_t pgraph_mtl_texture_color_format_to_mtl(unsigned int nv2a_color_format,
                                                bool *out_is_compressed,
                                                bool *out_is_indexed,
                                                bool *out_needs_unswizzle)
{
    if (out_is_compressed)   *out_is_compressed = false;
    if (out_is_indexed)      *out_is_indexed    = false;
    if (out_needs_unswizzle) *out_needs_unswizzle = false;

    switch (nv2a_color_format) {
    /* DXT compressed — CPU-decoded by s3tc_decompress_2d to RGBA8. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        if (out_is_compressed) *out_is_compressed = true;
        return PGRAPH_MTL_PIXEL_FORMAT_RGBA8UNORM;

    /* Linear ARGB8888-class — pgraph_convert_texture_data returns BGRA. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;

    /* Linear permuted-channel 32-bit formats. The §4.7
     * texture-format-sweep XBE caught these as missing on 2026-05-20
     * late evening: without a mapping entry the format hit the
     * INVALID branch and Metal couldn't sample correctly. The actual
     * per-byte channel decode is handled by mtl_convert_texture_data_
     * bgra8 in texture_pg.c (cases at lines 463-489). Adding the
     * mapping here makes those converter cases reachable. Linear
     * variants only -- the swizzled SZ_A8B8G8R8 / SZ_B8G8R8A8 /
     * SZ_R8G8B8A8 do exist but aren't exercised by v0.1, add them
     * when an XBE covers swizzled permutations. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;

    /* Swizzled ARGB8888-class — needs unswizzle then BGRA upload. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
        if (out_needs_unswizzle) *out_needs_unswizzle = true;
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;

    /* Swizzled permuted-channel 32-bit formats. Same conversion path
     * as the linear variants above, plus unswizzle. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
        if (out_needs_unswizzle) *out_needs_unswizzle = true;
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;

    /* R5G6B5 (linear / swizzled) — pgraph_convert_texture_data returns
     * BGRA8. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
        if (out_needs_unswizzle) *out_needs_unswizzle = true;
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;

    /* A8 / L8 — pgraph_convert_texture_data returns BGRA8 (replicated). */
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
        if (nv2a_color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8 ||
            nv2a_color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8) {
            if (out_needs_unswizzle) *out_needs_unswizzle = true;
        }
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;

    /* Indexed — palette decode produces BGRA8. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8:
        if (out_needs_unswizzle) *out_needs_unswizzle = true;
        if (out_is_indexed)      *out_is_indexed = true;
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;

    /* AY8 / A1R5G5B5 / A4R4G4B4 / R6G5B5 etc — pgraph_convert routes
     * them all to BGRA8 host-side. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5:
        return PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM;

    default:
        return PGRAPH_MTL_PIXEL_FORMAT_INVALID;
    }
}
