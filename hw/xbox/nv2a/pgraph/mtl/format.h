/*
 * NV2A PGRAPH Metal renderer — NV2A→MTLPixelFormat tables (slice M7.1 / M6 Part B).
 *
 * Centralizes the texture/surface format mapping. Returns MTLPixelFormat
 * values cast to uint32_t so this header stays Metal-API-free
 * (per-target .c builds do not propagate Metal headers).
 *
 * For texture color formats the table covers:
 *   - A8R8G8B8 / X8R8G8B8 → BGRA8Unorm (NV2A's ARGB8888 maps to BGRA8 with
 *     red/blue swap handled by spirv-cross's component_map; on Metal we
 *     CPU-decode via pgraph_convert_texture_data and upload as RGBA8).
 *   - R5G6B5 → CPU-converted to RGBA8 (B5G6R5Unorm is supported by Apple
 *     Silicon but NV2A uses ARGB ordering — easier to convert).
 *   - L8 / A8 / AL8 → RGBA8 via CPU conversion.
 *   - DXT1/3/5 → CPU-decompressed to RGBA8 (Path A; per-Slice
 *     M6-Part-B decision).
 *   - YUV / YCrCb → CPU-converted to BGRA.
 *
 * MTLPixelFormat enum values from <Metal/MTLPixelFormat.h>:
 *   Invalid           = 0
 *   R8Unorm           = 10
 *   RG8Unorm          = 30
 *   RGBA8Unorm        = 70
 *   BGRA8Unorm        = 80
 *   R5G6B5Unorm       = 40   (deprecated; ABGR4Unorm at 41 is the modern path)
 *   ABGR4Unorm        = 41
 *   ...
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_FORMAT_H
#define HW_XBOX_NV2A_PGRAPH_MTL_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MTLPixelFormat values used by the texture upload path. */
#define PGRAPH_MTL_PIXEL_FORMAT_INVALID      0
#define PGRAPH_MTL_PIXEL_FORMAT_R8UNORM      10
#define PGRAPH_MTL_PIXEL_FORMAT_RG8UNORM     30
#define PGRAPH_MTL_PIXEL_FORMAT_RGBA8UNORM   70
#define PGRAPH_MTL_PIXEL_FORMAT_BGRA8UNORM   80

/* Translate an NV2A texture color_format value to the MTLPixelFormat
 * the upload path produces. The CPU conversion / decompression /
 * unswizzle (vk pattern) always produces RGBA8 / BGRA8, so the table
 * is short. Returns 0 for unmapped formats. */
uint32_t pgraph_mtl_texture_color_format_to_mtl(unsigned int nv2a_color_format,
                                                bool *out_is_compressed,
                                                bool *out_is_indexed,
                                                bool *out_needs_unswizzle);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_FORMAT_H */
