/*
 * NV2A PGRAPH Metal renderer — texture upload + sampling (slice M6).
 *
 * Public C-callable API that hides the Metal-specific texture caching
 * + sampler caching machinery. Designed in the same style as
 * mtl/surface.h: opaque void* texture handles + uint32 format codes,
 * so the per-target .c side (renderer.c) can call into it without
 * pulling Metal headers.
 *
 * Per the M6 plan:
 *   - TextureBinding wraps id<MTLTexture> + id<MTLSamplerState>.
 *   - Upload: stage in Shared|WriteCombined ring slice, blit-encode
 *     into a Private id<MTLTexture> on heap_textures (lossless
 *     compression implicit).
 *   - Sampler cache: small finite set of NV2A texture-stage states;
 *     samplers are pre-built per unique (filter, addr_u, addr_v,
 *     addr_w, max_anisotropy, lod_bias) tuple at first use and reused.
 *   - S3TC: decode through xemu's existing CPU path
 *     (hw/xbox/nv2a/pgraph/s3tc.c) to RGBA8 before upload.
 *
 * NOTE — current scope: M6 ships with linear / non-swizzled / non-
 * cube-map / single-level RGBA texture upload as the primary path
 * (covers the common "UI texture, decal, basic surface" patterns).
 * Swizzled formats, mipmaps, cube maps, 3D textures, and YUV/intensity
 * formats are routed through the same cache + sampler path but use the
 * existing CPU conversion routine (`pgraph_convert_texture_data`) to
 * produce RGBA8 or BGRA8 staging data before upload. The full per-mip
 * + per-face vk/texture.c-equivalent layout-builder lands in M6 Part B
 * (visual gate driven, not blocking the cache + draw-swap milestone).
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_TEXTURE_H
#define HW_XBOX_NV2A_PGRAPH_MTL_TEXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle. Init pre-builds the sampler cache and brings up the upload
 * staging ring. Must run after pgraph_mtl_heap_init() — texture
 * allocations come out of heap_textures. */
bool pgraph_mtl_texture_init(void);
void pgraph_mtl_texture_finalize(void);

/*
 * Slot-based binding API.
 *
 * Each NV2A pixel-shader stage has a texture slot (0 .. NV2A_MAX_TEXTURES-1).
 * The renderer.c side calls pgraph_mtl_texture_bind_slot() per active
 * stage before pgraph_mtl_shaders_get_pipeline + the draw encode. The
 * .mm draw layer reads the bound texture/sampler via the accessors
 * below and binds them to the fragment encoder.
 *
 * `width` / `height` describe the source texture dimensions in texels.
 * `bytes_per_row` is the source row stride (post-decode if S3TC).
 * `mtl_pixel_format` is the MTLPixelFormat the source data is in
 * (typically MTLPixelFormatBGRA8Unorm = 80 or RGBA8Unorm = 70).
 *
 * The binding stays valid across draws until either:
 *   - pgraph_mtl_texture_bind_slot is called again for the same slot, or
 *   - pgraph_mtl_texture_unbind_slot is called, or
 *   - the texture cache evicts the entry under heap pressure.
 */
typedef struct PgraphMtlSamplerDesc {
    /* MTLSamplerMinMagFilter casts. 0 = Nearest, 1 = Linear. */
    uint32_t min_filter;
    uint32_t mag_filter;
    /* MTLSamplerMipFilter cast. 0 = NotMipmapped, 1 = Nearest, 2 = Linear. */
    uint32_t mip_filter;
    /* MTLSamplerAddressMode casts per axis. */
    uint32_t addr_u;
    uint32_t addr_v;
    uint32_t addr_w;
    /* Max anisotropy (1..16). */
    uint32_t max_anisotropy;
    float    lod_bias;
    float    min_lod;
    float    max_lod;
    /* Border color (MTLSamplerBorderColor cast); only honored when an
     * address mode is ClampToBorderColor. */
    uint32_t border_color;
} PgraphMtlSamplerDesc;

/*
 * Upload + bind a texture to `stage`. Returns true on success.
 *
 * The host-side `data` pointer's `size` bytes are blit-uploaded into a
 * Private id<MTLTexture> allocated from heap_textures. `data` may be
 * freed after this call returns — the staging ring takes a copy. The
 * upload command buffer is committed before this function returns
 * (synchronous), matching the M3 buffer ring's behavior.
 *
 * The sampler is looked up (or built and cached) from `sampler` and
 * bound to the same stage.
 *
 * If `vram_phys_addr` is non-zero it serves as an identity for the
 * texture cache; subsequent calls with the same vram address + shape
 * + sampler will hit the cache and skip re-uploading.
 */
bool pgraph_mtl_texture_bind_slot(int stage,
                                  uint64_t vram_phys_addr,
                                  uint32_t mtl_pixel_format,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t bytes_per_row,
                                  const void *data,
                                  size_t      size,
                                  const PgraphMtlSamplerDesc *sampler);

/* Mark a stage as "no texture bound". Subsequent draws will not bind a
 * texture/sampler to that fragment slot. */
void pgraph_mtl_texture_unbind_slot(int stage);
void pgraph_mtl_texture_invalidate_addr(uint64_t vram_phys_addr);
void pgraph_mtl_texture_invalidate_range(uint64_t vram_phys_addr,
                                         uint64_t byte_length);

/* Accessors used by the .mm draw layer to encode bindings. Return
 * void* casts of id<MTLTexture> / id<MTLSamplerState>; the cache
 * retains them so the caller MUST NOT release. */
void *pgraph_mtl_texture_get_metal_texture(int stage);
void *pgraph_mtl_texture_get_sampler_state(int stage);
float pgraph_mtl_texture_get_stage_scale(int stage);
bool pgraph_mtl_texture_stage_uses_external_surface(int stage);

/* Default sampler used for stages that need an MSL sampler binding
 * (some translated PSH variants reference all 4 sampler slots even when
 * a stage has no texture). Returns the same shared id<MTLSamplerState>
 * across calls. */
void *pgraph_mtl_texture_get_default_sampler(void);

/*
 * M6 Part B: extended slot binding with per-mip + per-face + cubemap
 * support. The single-level legacy `pgraph_mtl_texture_bind_slot`
 * remains the simple-call entry point. This extended form takes:
 *
 *   - levels        : number of mip levels (1 .. 16). For cubemaps this
 *                     applies to every face.
 *   - num_faces     : 1 (2D) or 6 (cubemap).
 *   - is_cubemap    : if true, the texture is allocated as MTLTextureTypeCube.
 *   - per_level     : pointer to `levels * num_faces` PgraphMtlTextureLevel
 *                     entries. Order: face-major, level-minor — i.e.
 *                     per_level[face * levels + level].
 *
 * All level data must be host-side bytes (RGBA8 or BGRA8 per
 * `mtl_pixel_format`). The caller is responsible for any CPU
 * conversion / decompression / unswizzle before invoking.
 */
typedef struct PgraphMtlTextureLevel {
    uint32_t    width;
    uint32_t    height;
    uint32_t    bytes_per_row;
    const void *data;
    size_t      data_size;
} PgraphMtlTextureLevel;

bool pgraph_mtl_texture_bind_slot_full(int stage,
                                       uint64_t vram_phys_addr,
                                       uint64_t source_byte_length,
                                       uint32_t mtl_pixel_format,
                                       bool is_cubemap,
                                       uint32_t num_faces,
                                       uint32_t levels,
                                       const PgraphMtlTextureLevel *per_level,
                                       const PgraphMtlSamplerDesc *sampler);

bool pgraph_mtl_texture_bind_slot_external(int stage,
                                           void *texture,
                                           float scale,
                                           const PgraphMtlSamplerDesc *sampler);

/*
 * Convenience: walk the current PGRAPHState texture stage `stage`
 * and bind it. The implementation pulls the shape via
 * pgraph_get_texture_shape, decompresses S3TC / unswizzles via the
 * existing CPU paths (s3tc.c / swizzle.c), and uploads via
 * pgraph_mtl_texture_bind_slot_full.
 *
 * Returns true if a texture was bound; false if the slot is disabled
 * or the format is unsupported (in which case the unbind path runs).
 *
 * Implementation lives in texture_pg.c (per-target compile).
 */
typedef struct PGRAPHState PGRAPHState;
bool pgraph_mtl_texture_bind_from_pg(PGRAPHState *pg, int stage);

/* Counters (atomics). */
uint64_t pgraph_mtl_texture_uploads_count(void);
uint64_t pgraph_mtl_texture_upload_bytes(void);
uint64_t pgraph_mtl_texture_cache_hits(void);
uint64_t pgraph_mtl_texture_cache_misses(void);
uint64_t pgraph_mtl_texture_sampler_cache_size(void);

/*
 * M8 — texture-upload fence.
 *
 * The M6 upload path used `[cb waitUntilCompleted]` after every blit,
 * which blocks the renderer thread until the GPU finishes copying the
 * staging buffer into the destination texture. That is correctness-
 * by-CPU-stall: the upload finishes before the draw encoder runs, so
 * the draw never reads a half-uploaded texture.
 *
 * M8 replaces the CPU stall with a GPU-side fence: the upload command
 * buffer signals `s_upload_fence_event` with a monotonically
 * increasing value, and the draw command buffer encodes a wait on the
 * latest value before its render encoder runs. Same semantic
 * ordering, no CPU stall.
 *
 * `pgraph_mtl_texture_get_upload_fence_event` returns the
 * id<MTLSharedEvent> as void* (caller MUST NOT release).
 * `pgraph_mtl_texture_get_upload_fence_value` returns the latest
 * signaled value to wait on. These are read by the draw layer
 * immediately before encoding a draw command buffer; the encode
 * captures the value, and the draw's `[cb encodeWaitForEvent:value:]`
 * gates GPU execution on the upload's completion. */
void    *pgraph_mtl_texture_get_upload_fence_event(void);
uint64_t pgraph_mtl_texture_get_upload_fence_value(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_TEXTURE_H */
