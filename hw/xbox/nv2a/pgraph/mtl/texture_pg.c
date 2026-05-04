/*
 * NV2A PGRAPH Metal renderer — texture lifecycle (PGRAPHState side).
 * Slice M6 Part B (foundational port).
 *
 * Walks PGRAPHState to compute the texture upload + sampler descriptor
 * for a given stage, decoding S3TC / unswizzling / format-converting on
 * the CPU via the existing routines (s3tc.c / swizzle.c /
 * pgraph_convert_texture_data), and hands the per-mip per-face data to
 * `pgraph_mtl_texture_bind_slot_full`.
 *
 * This is the per-target side that includes nv2a_int.h and the texture
 * helper headers; the Metal-API side stays in texture.mm and the cache
 * backend.
 *
 * Mirrors the structural pattern of vk/texture.c::get_texture_layout +
 * upload_texture_image, condensed for the Metal cache's "bind by
 * vram_addr" identity scheme.
 *
 * SCOPE NOTE: this is a foundational port, not the complete vk/texture.c
 * lifecycle. The full vk implementation (~1500 lines including
 * surface-to-texture, palette caching, scaling, custom border colors)
 * is queued as the rest of M6 Part B and tracked in handoff.md. What
 * this file ships:
 *   - 2D linear / swizzled textures (post-CPU-convert).
 *   - 2D mipmapped textures (per-level).
 *   - 2D cubemap textures (6 faces × N levels).
 *   - DXT1/3/5 via CPU s3tc decompress.
 *   - NV2A sampler-state translation: filter / addr / max-anisotropy.
 *
 * What it deliberately omits (deferred):
 *   - Surface-to-texture (render target rebinding).
 *   - 3D volume textures (NV2A dimensionality == 3).
 *   - Border texel preservation in cubemaps.
 *   - Per-LOD min/max clamp via min_mipmap_level / max_mipmap_level.
 *   - Shadow / depth-compare samplers (NV_PGRAPH_TEXFILTER0 special
 *     modes).
 *   - Texture-data hash dirty tracking; we use vram_phys_addr only for
 *     the cache key, so re-uploads happen on every cache miss.  Vk
 *     adds an in-vram fast_hash to detect modifications without
 *     evicting; that lands in the next slice.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "hw/xbox/nv2a/pgraph/texture.h"

#include "draw.h"
#include "format.h"
#include "surface.h"
#include "texture.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* MTLSamplerMinMagFilter values (cast to uint32_t):
 *   Nearest = 0, Linear = 1
 * MTLSamplerMipFilter:
 *   NotMipmapped = 0, Nearest = 1, Linear = 2
 * MTLSamplerAddressMode:
 *   ClampToEdge = 0, MirrorClampToEdge = 1, Repeat = 2,
 *   MirrorRepeat = 3, ClampToZero = 4, ClampToBorderColor = 5,
 *   MirrorClampToZero = 6
 */
enum {
    MTL_SAMP_FILTER_NEAREST = 0,
    MTL_SAMP_FILTER_LINEAR  = 1,
    MTL_SAMP_MIP_NONE       = 0,
    MTL_SAMP_MIP_NEAREST    = 1,
    MTL_SAMP_MIP_LINEAR     = 2,
    MTL_SAMP_ADDR_CLAMP_TO_EDGE         = 0,
    MTL_SAMP_ADDR_MIRROR_CLAMP_TO_EDGE  = 1,
    MTL_SAMP_ADDR_REPEAT                = 2,
    MTL_SAMP_ADDR_MIRROR_REPEAT         = 3,
    MTL_SAMP_ADDR_CLAMP_TO_ZERO         = 4,
    MTL_SAMP_ADDR_CLAMP_TO_BORDER_COLOR = 5,
};

static void mtl_after_texture_surface_download(void *opaque,
                                               uint32_t vram_addr,
                                               uint32_t byte_size)
{
    NV2AState *d = (NV2AState *)opaque;
    if (d == NULL || d->vram == NULL || byte_size == 0) {
        return;
    }
    memory_region_set_client_dirty(d->vram, (hwaddr)vram_addr,
                                   (hwaddr)byte_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, (hwaddr)vram_addr,
                                   (hwaddr)byte_size,
                                   DIRTY_MEMORY_NV2A_TEX);
}

static bool texture_range_dirty(NV2AState *d, hwaddr addr, size_t size)
{
    if (d == NULL || d->vram == NULL || size == 0) {
        return false;
    }
    hwaddr vram_size = memory_region_size(d->vram);
    if (addr >= vram_size) {
        return false;
    }
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    if (end > vram_size) {
        end = vram_size;
    }
    if (end <= addr) {
        return false;
    }
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

static bool mtl_disable_surface_texture_fastpath(void)
{
    const char *e = getenv("XEMU_METAL_DISABLE_SURFACE_TEX");
    return e != NULL && e[0] != '\0' && e[0] != '0';
}

static bool mtl_disable_surface_texture_addr(uint32_t addr)
{
    const char *e = getenv("XEMU_METAL_DISABLE_SURFACE_TEX_ADDRS");
    if (e == NULL || e[0] == '\0') {
        return false;
    }
    const char *p = e;
    while (*p != '\0') {
        while (*p == ',' || *p == ';' || *p == ':' || *p == ' ' ||
               *p == '\t' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *endp = NULL;
        unsigned long v = strtoul(p, &endp, 0);
        if (endp == p) {
            p++;
            continue;
        }
        if ((uint32_t)v == addr) {
            return true;
        }
        p = endp;
    }
    return false;
}

static bool mtl_diag_tex_bind_enabled(void)
{
    const char *e = getenv("XEMU_METAL_DIAG_TEX_BIND");
    return e != NULL && e[0] != '\0' && e[0] != '0';
}

static bool mtl_diag_tex_bind_all_targets(void)
{
    const char *e = getenv("XEMU_METAL_DIAG_TEX_BIND_ALL");
    return e != NULL && e[0] != '\0' && e[0] != '0';
}

static uint32_t translate_addr_mode(uint32_t nv2a_mode)
{
    /* NV_PGRAPH_TEXADDRESS0_ADDRU values:
     *   1 = wrap, 2 = mirror, 3 = clamp-to-edge, 4 = border, 5 = clamp-to-edge-OGL,
     *   6 = mirror-once (clamp), ...
     * Map conservatively. */
    switch (nv2a_mode) {
    case 1: return MTL_SAMP_ADDR_REPEAT;
    case 2: return MTL_SAMP_ADDR_MIRROR_REPEAT;
    case 3: return MTL_SAMP_ADDR_CLAMP_TO_EDGE;
    case 4: return MTL_SAMP_ADDR_CLAMP_TO_BORDER_COLOR;
    case 5: return MTL_SAMP_ADDR_CLAMP_TO_EDGE;
    case 6: return MTL_SAMP_ADDR_MIRROR_CLAMP_TO_EDGE;
    default:
        return MTL_SAMP_ADDR_CLAMP_TO_EDGE;
    }
}

static uint32_t translate_min_filter(uint32_t nv2a_min)
{
    /* NV_PGRAPH_TEXFILTER0_MIN encodings (subset):
     *  1 = box_lod0       — point, no mipmap
     *  2 = tent_lod0      — linear, no mipmap
     *  3 = box_nearestlod — point + mip-nearest
     *  4 = tent_nearestlod — linear + mip-nearest
     *  5 = box_tentlod    — point + mip-linear
     *  6 = tent_tentlod   — linear + mip-linear
     *  7 = convolution_2d_lod0 — convolution; we approximate as linear
     */
    switch (nv2a_min) {
    case 1: return MTL_SAMP_FILTER_NEAREST;
    case 2: return MTL_SAMP_FILTER_LINEAR;
    case 3: return MTL_SAMP_FILTER_NEAREST;
    case 4: return MTL_SAMP_FILTER_LINEAR;
    case 5: return MTL_SAMP_FILTER_NEAREST;
    case 6: return MTL_SAMP_FILTER_LINEAR;
    default:
        return MTL_SAMP_FILTER_LINEAR;
    }
}

static uint32_t translate_mip_filter(uint32_t nv2a_min, bool single_level)
{
    if (single_level) {
        return MTL_SAMP_MIP_NONE;
    }
    switch (nv2a_min) {
    case 1: case 2: case 7:
        return MTL_SAMP_MIP_NONE;
    case 3: case 4:
        return MTL_SAMP_MIP_NEAREST;
    case 5: case 6:
        return MTL_SAMP_MIP_LINEAR;
    default:
        return MTL_SAMP_MIP_NONE;
    }
}

static enum S3TC_DECOMPRESS_FORMAT
s3tc_format_for_kelvin(unsigned int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    }
}

static inline uint8_t expand_4_to_8(uint32_t v)
{
    return (uint8_t)((v << 4) | v);
}

static inline uint8_t expand_5_to_8(uint32_t v)
{
    return (uint8_t)((v << 3) | (v >> 2));
}

static inline uint8_t expand_6_to_8(uint32_t v)
{
    return (uint8_t)((v << 2) | (v >> 4));
}

static inline uint16_t load_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint8_t *mtl_convert_texture_data_bgra8(TextureShape s,
                                               const uint8_t *data,
                                               unsigned int width,
                                               unsigned int height,
                                               unsigned int row_pitch,
                                               size_t *converted_size)
{
    if (data == NULL || converted_size == NULL) {
        return NULL;
    }

    size_t size = (size_t)width * height * 4;
    uint8_t *out = (uint8_t *)g_malloc(size);
    uint8_t *dst = out;

    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *src = data + (size_t)y * row_pitch;
        for (unsigned int x = 0; x < width; x++, dst += 4) {
            switch (s.color_format) {
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8: {
                uint8_t v = src[x];
                dst[0] = v;
                dst[1] = v;
                dst[2] = v;
                dst[3] = 255;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8: {
                uint8_t v = src[x];
                dst[0] = v;
                dst[1] = v;
                dst[2] = v;
                dst[3] = v;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8: {
                uint8_t a = src[x];
                dst[0] = 255;
                dst[1] = 255;
                dst[2] = 255;
                dst[3] = a;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8: {
                const uint8_t *p = src + (size_t)x * 2;
                uint8_t yv = p[0];
                dst[0] = yv;
                dst[1] = yv;
                dst[2] = yv;
                dst[3] = p[1];
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5: {
                uint16_t p = load_le16(src + (size_t)x * 2);
                dst[0] = expand_5_to_8(p & 0x1f);
                dst[1] = expand_6_to_8((p >> 5) & 0x3f);
                dst[2] = expand_5_to_8((p >> 11) & 0x1f);
                dst[3] = 255;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5: {
                uint16_t p = load_le16(src + (size_t)x * 2);
                dst[0] = expand_5_to_8(p & 0x1f);
                dst[1] = expand_5_to_8((p >> 5) & 0x1f);
                dst[2] = expand_5_to_8((p >> 10) & 0x1f);
                if (s.color_format ==
                        NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5 ||
                    s.color_format ==
                        NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5) {
                    dst[3] = 255;
                } else {
                    dst[3] = (p & 0x8000) ? 255 : 0;
                }
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4: {
                uint16_t p = load_le16(src + (size_t)x * 2);
                dst[0] = expand_4_to_8(p & 0x0f);
                dst[1] = expand_4_to_8((p >> 4) & 0x0f);
                dst[2] = expand_4_to_8((p >> 8) & 0x0f);
                dst[3] = expand_4_to_8((p >> 12) & 0x0f);
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8: {
                const uint8_t *p = src + (size_t)x * 4;
                dst[0] = p[0];
                dst[1] = p[1];
                dst[2] = p[2];
                dst[3] = 255;
                break;
            }
            default:
                g_free(out);
                return NULL;
            }
        }
    }

    *converted_size = size;
    return out;
}

static bool surface_texture_compatible(uint32_t surface_fmt,
                                       const TextureShape *shape)
{
    if (shape == NULL || shape->cubemap || shape->levels > 1 ||
        shape->dimensionality != 2) {
        return false;
    }

    uint32_t texture_fmt = shape->color_format;
    switch (surface_fmt) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        return texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        return texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5 ||
               texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        return texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8 ||
               texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        return texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 ||
               texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8 ||
               texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 ||
               texture_fmt ==
               NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    default:
        return false;
    }
}

static bool surface_texture_needs_cpu_path(uint32_t surface_fmt,
                                           const TextureShape *shape)
{
    if (shape == NULL) {
        return true;
    }

    if (surface_fmt != NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8) {
        return false;
    }

    switch (shape->color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
        /* These linear A8R8G8B8-family texture views rely on the normal
         * texture upload path's channel/alpha normalization. Sampling the
         * render target directly exposes Metal's BGRA view and produces
         * alpha-mask artifacts in PGR2's menu text. Swizzled A8R8G8B8 RTTs
         * remain eligible; they are heavily used for the menu panels. */
        return true;
    default:
        return false;
    }
}

static bool surface_texture_pitch_compatible(uint32_t surface_pitch,
                                             const TextureShape *shape)
{
    if (shape == NULL) {
        return false;
    }

    BasicColorFormatInfo f =
        kelvin_color_format_info_map[shape->color_format];
    if (!f.linear) {
        return true;
    }

    return surface_pitch == 0 || surface_pitch == shape->pitch;
}

#define MTL_TEX_MAX_LEVELS 16
#define MTL_TEX_MAX_FACES  6

/* Decode all levels for a single face; appends to `out_levels[base..]`.
 * `*data_ptr` is advanced past consumed bytes.
 *
 * Each appended PgraphMtlTextureLevel has decoded_data malloc'd by us
 * (free with g_free); the caller frees after the upload completes.
 *
 * Returns true on success. */
static bool decode_face_levels(PGRAPHState *pg, TextureShape s,
                               const uint8_t *vram_base,
                               const uint8_t *palette_data,
                               size_t face_byte_offset,
                               size_t face_size,
                               PgraphMtlTextureLevel *out_levels,
                               unsigned int base_index,
                               unsigned int max_levels,
                               unsigned int *out_decoded_count)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed = pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int width = s.width;
    unsigned int height = s.height;
    const uint8_t *p = vram_base + face_byte_offset;
    const uint8_t *p_end = p + face_size;

    unsigned int levels_to_decode = s.levels;
    if (levels_to_decode > max_levels) levels_to_decode = max_levels;
    if (levels_to_decode > MTL_TEX_MAX_LEVELS) levels_to_decode = MTL_TEX_MAX_LEVELS;

    *out_decoded_count = 0;

    for (unsigned int level = 0; level < levels_to_decode; level++) {
        unsigned int w = width  ? width  : 1;
        unsigned int h = height ? height : 1;

        if (is_compressed) {
            unsigned int physical_width  = (w + 3) & ~3u;
            unsigned int physical_height = (h + 3) & ~3u;
            unsigned int block_size =
                (s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5)
                ? 8 : 16;
            size_t blocks = ((size_t)physical_width / 4) *
                            ((size_t)physical_height / 4);
            size_t consumed = blocks * block_size;
            if (p + consumed > p_end) {
                return false;
            }

            uint8_t *rgba = s3tc_decompress_2d(s3tc_format_for_kelvin(s.color_format),
                                               p, w, h);
            if (!rgba) {
                return false;
            }
            out_levels[base_index + level].width         = w;
            out_levels[base_index + level].height        = h;
            out_levels[base_index + level].bytes_per_row = w * 4;
            out_levels[base_index + level].data          = rgba;
            out_levels[base_index + level].data_size     = (size_t)w * h * 4;
            p += consumed;
        } else if (f.linear) {
            /* Linear texture path — pgraph_convert_texture_data does
             * the format conversion. This path only ever has a single
             * level (assertion in vk/texture.c). */
            unsigned int pitch = s.pitch ? s.pitch : (w * f.bytes_per_pixel);
            size_t consumed = (size_t)pitch * h;
            if (p + consumed > p_end) {
                return false;
            }

            size_t converted_size = 0;
            uint8_t *converted = pgraph_convert_texture_data(
                s, p, palette_data, w, h, 1, pitch, 0, &converted_size);
            if (!converted) {
                converted = mtl_convert_texture_data_bgra8(
                    s, p, w, h, pitch, &converted_size);
            }
            if (!converted &&
                s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
                return false;
            }
            if (!converted) {
                size_t dst_stride = (size_t)w * f.bytes_per_pixel;
                converted_size = dst_stride * h;
                converted = (uint8_t *)g_malloc(converted_size);
                for (unsigned int y = 0; y < h; y++) {
                    memcpy(converted + y * dst_stride,
                           p + y * pitch, dst_stride);
                }
            }
            out_levels[base_index + level].width         = w;
            out_levels[base_index + level].height        = h;
            out_levels[base_index + level].bytes_per_row =
                (uint32_t)((converted_size / h) ? (converted_size / h) : (w * 4));
            out_levels[base_index + level].data          = converted;
            out_levels[base_index + level].data_size     = converted_size;
            p += consumed;
        } else {
            /* Swizzled path — unswizzle into a temp buffer, then
             * format-convert. */
            unsigned int pitch = w * f.bytes_per_pixel;
            size_t swizzled_size = (size_t)pitch * h;
            if (p + swizzled_size > p_end) {
                return false;
            }
            uint8_t *unswizzled = (uint8_t *)g_malloc(swizzled_size);
            unswizzle_rect(p, w, h, unswizzled, pitch, f.bytes_per_pixel);

            size_t converted_size = 0;
            uint8_t *converted = pgraph_convert_texture_data(
                s, unswizzled, palette_data, w, h, 1, pitch, 0,
                &converted_size);
            if (!converted) {
                converted = mtl_convert_texture_data_bgra8(
                    s, unswizzled, w, h, pitch, &converted_size);
            }
            if (converted) {
                g_free(unswizzled);
            } else if (s.color_format ==
                       NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
                g_free(unswizzled);
                return false;
            } else {
                converted = unswizzled;
                converted_size = swizzled_size;
            }
            out_levels[base_index + level].width         = w;
            out_levels[base_index + level].height        = h;
            out_levels[base_index + level].bytes_per_row =
                (uint32_t)((converted_size / h) ? (converted_size / h) : (w * 4));
            out_levels[base_index + level].data          = converted;
            out_levels[base_index + level].data_size     = converted_size;
            p += swizzled_size;
        }

        (*out_decoded_count)++;

        if (width  > 1) width  /= 2;
        if (height > 1) height /= 2;
    }
    return true;
}

bool pgraph_mtl_texture_bind_from_pg(PGRAPHState *pg, int stage)
{
    if (pg == NULL || stage < 0 || stage >= NV2A_MAX_TEXTURES) {
        return false;
    }

    /* Stage-disabled? unbind. */
    uint32_t ctl_0 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + stage * 4);
    if (!GET_MASK(ctl_0, NV_PGRAPH_TEXCTL0_0_ENABLE)) {
        pgraph_mtl_texture_unbind_slot(stage);
        return false;
    }

    TextureShape s = pgraph_get_texture_shape(pg, stage);
    bool is_compressed = false;
    bool is_indexed = false;
    bool needs_unswizzle = false;
    uint32_t mtl_fmt = pgraph_mtl_texture_color_format_to_mtl(
        s.color_format, &is_compressed, &is_indexed, &needs_unswizzle);
    if (mtl_fmt == 0) {
        /* Unsupported format — leave the slot whatever it currently is
         * (likely the default-sampler-only state). */
        pgraph_mtl_texture_unbind_slot(stage);
        return false;
    }
    /* Volume textures (3D dimensionality == 3) deferred. */
    if (s.dimensionality > 2) {
        pgraph_mtl_texture_unbind_slot(stage);
        return false;
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    hwaddr offset = pgraph_get_texture_phys_addr(pg, stage);
    size_t texture_length = pgraph_get_texture_length(pg, &s);
    const uint8_t *vram = (const uint8_t *)d->vram_ptr;
    const uint8_t *palette_data = NULL;
    size_t palette_size = 0;
    hwaddr palette_offset = 0;
    if (is_indexed) {
        palette_offset =
            pgraph_get_texture_palette_phys_addr_length(pg, stage,
                                                        &palette_size);
        if (palette_size == 0) {
            pgraph_mtl_texture_unbind_slot(stage);
            return false;
        }
        palette_data = vram + palette_offset;
        /* The current Metal texture cache key does not include the palette
         * address/content hash. Refresh paletted uploads on bind so palette
         * animation or reuse cannot silently hold stale colors. */
        pgraph_mtl_texture_invalidate_addr((uint64_t)offset);
    }

    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    /* Linear textures must have levels == 1 per NV2A spec (vk asserts
     * the same). */
    unsigned int levels = f.linear ? 1 : s.levels;
    if (levels == 0) levels = 1;
    if (levels > MTL_TEX_MAX_LEVELS) levels = MTL_TEX_MAX_LEVELS;

    unsigned int faces = s.cubemap ? 6 : 1;

    void *surface_tex = NULL;
    uint32_t surface_w = 0, surface_h = 0;
    uint32_t guest_w = 0, guest_h = 0;
    uint32_t surface_pitch = 0, surface_fmt = 0;
    bool has_compatible_surface =
        pgraph_mtl_surface_get_color_surface_info_for(
            (uint32_t)offset, s.width, s.height, /*pitch=*/0,
            &surface_tex, &surface_w, &surface_h,
            &guest_w, &guest_h, &surface_pitch, &surface_fmt) &&
        surface_texture_compatible(surface_fmt, &s) &&
        !surface_texture_needs_cpu_path(surface_fmt, &s) &&
        guest_w == s.width &&
        guest_h == s.height &&
        surface_texture_pitch_compatible(surface_pitch, &s) &&
        !mtl_disable_surface_texture_fastpath() &&
        !mtl_disable_surface_texture_addr((uint32_t)offset);
    bool self_sample =
        has_compatible_surface &&
        pgraph_mtl_surface_get_color_vram_addr() == (uint32_t)offset;

    uint32_t color_target = pgraph_mtl_surface_get_color_vram_addr();
    if (mtl_diag_tex_bind_enabled() &&
        (mtl_diag_tex_bind_all_targets() || color_target == 0x32a4000 ||
         color_target == 0x3628000 || color_target == 0x2e06000 ||
         color_target == 0x2c06000)) {
        typedef struct MtlTexBindDiagCount {
            uint32_t target;
            unsigned int count;
        } MtlTexBindDiagCount;
        static MtlTexBindDiagCount s_diag_counts[16];
        static unsigned int s_diag_used = 0;
        unsigned int *count = NULL;
        for (unsigned int i = 0; i < s_diag_used; i++) {
            if (s_diag_counts[i].target == color_target) {
                count = &s_diag_counts[i].count;
                break;
            }
        }
        if (count == NULL && s_diag_used < 16) {
            s_diag_counts[s_diag_used].target = color_target;
            s_diag_counts[s_diag_used].count = 0;
            count = &s_diag_counts[s_diag_used].count;
            s_diag_used++;
        }
        if (count != NULL && *count < 64) {
            (*count)++;
            fprintf(stderr,
                    "xemu-perf: metal_tex_bind_diag target=0x%x "
                    "stage=%d tex=0x%llx dim=%u size=%ux%u pitch=%u "
                    "fmt=%u mtl_fmt=%u levels=%u linear=%d "
                    "surf=%d self=%d surf_guest=%ux%u "
                    "surf_scaled=%ux%u surf_pitch=%u surf_fmt=%u "
                    "prim=%u ctl0=0x%08x\n",
                    (unsigned)color_target, stage,
                    (unsigned long long)offset,
                    (unsigned)s.dimensionality, s.width, s.height,
                    s.pitch, s.color_format, mtl_fmt, levels,
                    f.linear ? 1 : 0,
                    has_compatible_surface ? 1 : 0,
                    self_sample ? 1 : 0,
                    guest_w, guest_h, surface_w, surface_h,
                    surface_pitch, surface_fmt,
                    (unsigned)pg->primitive_mode, (unsigned)ctl_0);
        }
    }
    if (self_sample) {
        pgraph_mtl_draw_flush_open_pass();
        pgraph_mtl_surface_download_if_dirty_at((uint32_t)offset,
                                                d->vram_ptr,
                                                NULL, NULL);
        pgraph_mtl_texture_invalidate_addr((uint64_t)offset);
        if (getenv("XEMU_METAL_DIAG_SURFACE_TEX")) {
            fprintf(stderr,
                    "xemu-perf: metal_surface_self_sample stage=%d "
                    "vram_addr=0x%llx guest=%ux%u scaled=%ux%u\n",
                    stage, (unsigned long long)offset,
                    guest_w, guest_h, surface_w, surface_h);
        }
    }

    if (!has_compatible_surface || self_sample) {
        pgraph_mtl_surface_download_in_range_if_dirty(
            (uint32_t)offset, (uint32_t)texture_length, d->vram_ptr,
            mtl_after_texture_surface_download, d);

        bool possibly_dirty = pg->texture_dirty[stage] ||
            texture_range_dirty(d, offset, texture_length);
        if (palette_size != 0) {
            possibly_dirty |= texture_range_dirty(d, palette_offset,
                                                  palette_size);
        }
        if (possibly_dirty) {
            pgraph_mtl_texture_invalidate_range((uint64_t)offset,
                                                (uint64_t)texture_length);
        }
    }

    PgraphMtlTextureLevel decoded[MTL_TEX_MAX_FACES * MTL_TEX_MAX_LEVELS];
    memset(decoded, 0, sizeof(decoded));

    /* Compute face byte size for cubemaps via the same formula as vk. */
    size_t face_size = 0;
    if (faces > 1) {
        unsigned int w = s.width, h = s.height;
        if (is_compressed) {
            unsigned int block_size =
                (s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5)
                ? 8 : 16;
            for (unsigned int level = 0; level < s.levels; level++) {
                unsigned int pw = (w + 3) & ~3u;
                unsigned int ph = (h + 3) & ~3u;
                face_size += ((size_t)pw / 4) * ((size_t)ph / 4) * block_size;
                if (w > 1) w /= 2;
                if (h > 1) h /= 2;
            }
        } else {
            for (unsigned int level = 0; level < s.levels; level++) {
                unsigned int lw = w ? w : 1;
                unsigned int lh = h ? h : 1;
                face_size += (size_t)lw * lh * f.bytes_per_pixel;
                if (w > 1) w /= 2;
                if (h > 1) h /= 2;
            }
        }
        /* NV2A_CUBEMAP_FACE_ALIGNMENT not used here; the per-face data
         * layout is contiguous in the assumption used for the texture
         * cache. Borders / alignment are deferred. */
    }

    bool ok = true;
    for (unsigned int face = 0; face < faces; face++) {
        unsigned int decoded_count = 0;
        ok &= decode_face_levels(pg, s, vram, palette_data,
                                 (size_t)offset + face * face_size,
                                 face_size ? face_size : (1ull << 30),
                                 decoded, face * levels, levels,
                                 &decoded_count);
        if (!ok) break;
        if (decoded_count != levels) {
            /* Pad missing levels with zero entries — the upload skips
             * empty entries gracefully. */
        }
    }
    if (!ok) {
        for (size_t i = 0; i < sizeof(decoded)/sizeof(decoded[0]); i++) {
            if (decoded[i].data) g_free((void *)decoded[i].data);
        }
        pgraph_mtl_texture_unbind_slot(stage);
        return false;
    }

    /* Build the sampler descriptor from NV2A regs. */
    uint32_t filter   = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + stage * 4);
    uint32_t address  = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4);
    uint32_t border_argb = pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + stage * 4);

    PgraphMtlSamplerDesc sd;
    memset(&sd, 0, sizeof(sd));

    unsigned int min_f = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    unsigned int mag_f = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    sd.min_filter = translate_min_filter(min_f);
    sd.mag_filter = translate_min_filter(mag_f);
    sd.mip_filter = translate_mip_filter(min_f, levels == 1);

    sd.addr_u = translate_addr_mode(GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU));
    sd.addr_v = translate_addr_mode(GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV));
    sd.addr_w = translate_addr_mode(GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP));

    sd.max_anisotropy = 1;
    sd.lod_bias = 0.0f;
    sd.min_lod = 0.0f;
    sd.max_lod = (levels > 1) ? (float)(levels - 1) : 0.0f;
    sd.border_color = 0; /* TransparentBlack — custom border deferred */
    (void)border_argb;

    if (has_compatible_surface && !self_sample) {
        float scale = 1.0f;
        if (guest_w != 0 && surface_w >= guest_w) {
            scale = (float)surface_w / (float)guest_w;
        }

        pgraph_mtl_draw_flush_open_pass();
        pgraph_mtl_surface_upload_if_dirty_at((uint32_t)offset,
                                              d->vram_ptr);
        bool bound = pgraph_mtl_texture_bind_slot_external(stage, surface_tex,
                                                           scale, &sd);
        if (bound && getenv("XEMU_METAL_DIAG_SURFACE_TEX")) {
            fprintf(stderr,
                    "xemu-perf: metal_surface_texture stage=%d "
                    "vram_addr=0x%llx guest=%ux%u scaled=%ux%u "
                    "surface_fmt=%u texture_fmt=%u scale=%.3f\n",
                    stage, (unsigned long long)offset,
                    guest_w, guest_h, surface_w, surface_h,
                    surface_fmt, s.color_format, scale);
        }
        pg->texture_dirty[stage] = false;
        return bound;
    }

    bool bound = pgraph_mtl_texture_bind_slot_full(stage, (uint64_t)offset,
                                                   (uint64_t)texture_length,
                                                   mtl_fmt, s.cubemap,
                                                   faces, levels,
                                                   decoded, &sd);

    /* Free decoded buffers — the upload has already memcpy'd them into
     * the staging ring. */
    for (size_t i = 0; i < sizeof(decoded)/sizeof(decoded[0]); i++) {
        if (decoded[i].data) g_free((void *)decoded[i].data);
    }

    pg->texture_dirty[stage] = false;
    return bound;
}
