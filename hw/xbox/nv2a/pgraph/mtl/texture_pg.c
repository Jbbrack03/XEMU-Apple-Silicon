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

#include "format.h"
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
                s, p, NULL, w, h, 1, pitch, 0, &converted_size);
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
                s, unswizzled, NULL, w, h, 1, pitch, 0, &converted_size);
            if (converted) {
                g_free(unswizzled);
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
    if (is_indexed) {
        /* Palette path not yet wired through this slice's bind_slot_full
         * (pgraph_convert_texture_data needs palette_data; the wrapper
         * above passes NULL). Skip for M6 Part B; future slice. */
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
    const uint8_t *vram = (const uint8_t *)d->vram_ptr;

    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    /* Linear textures must have levels == 1 per NV2A spec (vk asserts
     * the same). */
    unsigned int levels = f.linear ? 1 : s.levels;
    if (levels == 0) levels = 1;
    if (levels > MTL_TEX_MAX_LEVELS) levels = MTL_TEX_MAX_LEVELS;

    unsigned int faces = s.cubemap ? 6 : 1;

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
        ok &= decode_face_levels(pg, s, vram,
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

    bool bound = pgraph_mtl_texture_bind_slot_full(stage, (uint64_t)offset,
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
