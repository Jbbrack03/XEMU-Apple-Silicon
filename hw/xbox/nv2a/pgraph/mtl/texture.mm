/*
 * NV2A PGRAPH Metal renderer — texture implementation (slice M6).
 *
 * See texture.h for the public contract. The implementation owns:
 *
 *   1. A texture cache keyed by (vram_phys_addr, shape) → id<MTLTexture>.
 *      Cache uses a small linear-scan array (NV2A_MAX_TEXTURES * 8 = 32
 *      entries) sized so the typical Xbox-game texture-binding-set fits;
 *      eviction is FIFO.
 *
 *   2. A sampler cache keyed by PgraphMtlSamplerDesc (POD, memcmp).
 *      Pre-warmed at init with the most common NV2A combinations
 *      (4 filter combos × 3 addr modes × 2 mip modes = ~24 samplers);
 *      additional combinations build on demand. Capacity 256 — well
 *      above the cardinality of the NV2A texture-stage state space.
 *
 *   3. A Shared|WriteCombined upload staging ring (1 MiB × 4 slots,
 *      independent of mtl/buffer.mm's draw staging ring so the upload
 *      path doesn't compete with draw staging). Each
 *      pgraph_mtl_texture_bind_slot call allocates from the current
 *      slot, blit-encodes the upload to the destination Private texture,
 *      and commits.
 *
 *   4. Per-stage current-binding pointers, surfaced via the accessors
 *      so the .mm draw layer can encode setFragmentTexture: /
 *      setFragmentSamplerState:.
 *
 * Counters:
 *   METAL_TEX_UPLOADS_TOTAL, METAL_TEX_UPLOAD_BYTES_TOTAL,
 *   METAL_TEX_CACHE_HITS, METAL_TEX_CACHE_MISSES.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "texture.h"
#include "heap.h"

#include <stdatomic.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" void *xemu_metal_get_device(void);
extern "C" void pgraph_mtl_draw_get_done_event_state(void **out_event,
                                                     uint64_t *out_value);

/* NV2A_MAX_TEXTURES is 4 in pgraph.h; mirror locally to keep this .mm
 * file's compile independent of nv2a_int.h. */
#define MTL_TEX_MAX_STAGES 4

/* -------- internal types -------- */

typedef struct {
    bool       in_use;
    uint64_t   vram_addr;
    uint64_t   byte_length;
    uint32_t   width;
    uint32_t   height;
    uint32_t   pixel_format;
    bool       is_cubemap;
    uint32_t   levels;
    void      *texture;  /* id<MTLTexture> retained */
} TextureCacheEntry;

typedef struct {
    bool                  in_use;
    PgraphMtlSamplerDesc  desc;
    void                 *state;  /* id<MTLSamplerState> retained */
} SamplerCacheEntry;

#define MTL_TEX_CACHE_CAP     512
#define MTL_SAMPLER_CACHE_CAP 256

static TextureCacheEntry s_tex_cache[MTL_TEX_CACHE_CAP];
static int               s_tex_cache_next_evict = 0;
static int               s_tex_cache_size = 0;
static int               s_tex_cache_limit = 0;

static SamplerCacheEntry s_smp_cache[MTL_SAMPLER_CACHE_CAP];
static int               s_smp_cache_size = 0;

static void *s_default_sampler = NULL;  /* id<MTLSamplerState> retained */

/* Per-stage current bindings. The draw layer reads these directly. */
static void *s_stage_texture[MTL_TEX_MAX_STAGES];
static void *s_stage_sampler[MTL_TEX_MAX_STAGES];
static float s_stage_scale[MTL_TEX_MAX_STAGES];
static bool  s_stage_external_surface[MTL_TEX_MAX_STAGES];
static bool  s_stage_rect_tex[MTL_TEX_MAX_STAGES];

/* Upload-ring staging buffers. Independent from mtl/buffer.mm's draw
 * ring so the upload encoder doesn't contend for the same backing. */
#define MTL_TEX_UPLOAD_SLOTS    4
#define MTL_TEX_UPLOAD_SLOT_SZ  (4 * 1024 * 1024)
static id<MTLBuffer> s_upload_slots[MTL_TEX_UPLOAD_SLOTS];
static int           s_upload_slot_idx = 0;
static size_t        s_upload_slot_off = 0;

static id<MTLDevice>       s_device;
static id<MTLCommandQueue> s_upload_queue;
static bool                s_init = false;

/* M8: upload fence. The blit command buffer signals
 * s_upload_fence_event with the value taken from s_upload_fence_value
 * (incremented atomically per signal). The draw layer reads the
 * latest value via the public accessor and encodes a wait on the
 * draw command buffer. */
static id<MTLSharedEvent>  s_upload_fence_event;
static _Atomic uint64_t    s_upload_fence_value = 0;

static _Atomic uint64_t s_uploads = 0;
static _Atomic uint64_t s_upload_bytes = 0;
static _Atomic uint64_t s_upload_us_total = 0;
static _Atomic uint64_t s_cache_hits = 0;
static _Atomic uint64_t s_cache_miss = 0;

static inline int64_t mtl_now_us(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    __uint128_t ns = (__uint128_t)mach_absolute_time() * tb.numer / tb.denom;
    return (int64_t)(ns / 1000);
}

static inline void mtl_add_elapsed_us(_Atomic uint64_t *counter,
                                      int64_t start_us)
{
    int64_t elapsed = mtl_now_us() - start_us;
    if (elapsed > 0) {
        atomic_fetch_add(counter, (uint64_t)elapsed);
    }
}

/* -------- sampler cache -------- */

static bool sampler_desc_equal(const PgraphMtlSamplerDesc *a,
                               const PgraphMtlSamplerDesc *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void *build_sampler(const PgraphMtlSamplerDesc *d)
{
    @autoreleasepool {
        MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
        desc.minFilter   = (MTLSamplerMinMagFilter)d->min_filter;
        desc.magFilter   = (MTLSamplerMinMagFilter)d->mag_filter;
        desc.mipFilter   = (MTLSamplerMipFilter)d->mip_filter;
        desc.sAddressMode = (MTLSamplerAddressMode)d->addr_u;
        desc.tAddressMode = (MTLSamplerAddressMode)d->addr_v;
        desc.rAddressMode = (MTLSamplerAddressMode)d->addr_w;
        if (d->max_anisotropy >= 1) {
            desc.maxAnisotropy = d->max_anisotropy;
        }
        desc.lodMinClamp = d->min_lod;
        desc.lodMaxClamp = (d->max_lod > 0.0f) ? d->max_lod : FLT_MAX;
        desc.normalizedCoordinates = YES;
        if (d->addr_u == MTLSamplerAddressModeClampToBorderColor ||
            d->addr_v == MTLSamplerAddressModeClampToBorderColor ||
            d->addr_w == MTLSamplerAddressModeClampToBorderColor) {
            desc.borderColor = (MTLSamplerBorderColor)d->border_color;
        }
        desc.label = @"xemu.metal.sampler";

        id<MTLSamplerState> ss =
            [s_device newSamplerStateWithDescriptor:desc];
        if (ss == nil) {
            return NULL;
        }
        return (__bridge_retained void *)ss;
    }
}

static void *get_sampler(const PgraphMtlSamplerDesc *d)
{
    for (int i = 0; i < s_smp_cache_size; i++) {
        if (s_smp_cache[i].in_use && sampler_desc_equal(&s_smp_cache[i].desc, d)) {
            return s_smp_cache[i].state;
        }
    }
    if (s_smp_cache_size >= MTL_SAMPLER_CACHE_CAP) {
        /* Should not happen — NV2A sampler-state cardinality is well
         * below 256. Fall back to the default sampler. */
        fprintf(stderr,
                "pgraph_mtl_texture: sampler cache full; "
                "using default sampler\n");
        return s_default_sampler;
    }
    void *st = build_sampler(d);
    if (st == NULL) {
        return s_default_sampler;
    }
    int idx = s_smp_cache_size++;
    s_smp_cache[idx].in_use = true;
    s_smp_cache[idx].desc   = *d;
    s_smp_cache[idx].state  = st;
    return st;
}

static void prewarm_sampler_cache(void)
{
    /* Pre-build the most-frequent NV2A combinations so the first draw
     * doesn't pay 4× sampler-build cost. The set chosen mirrors the
     * NV097 default state plus the variants observed in PGR2 / Rainbow
     * / Crimson texture-stage logs. */
    static const uint32_t filters[] = {
        MTLSamplerMinMagFilterNearest,
        MTLSamplerMinMagFilterLinear,
    };
    static const uint32_t mip_modes[] = {
        MTLSamplerMipFilterNotMipmapped,
        MTLSamplerMipFilterNearest,
        MTLSamplerMipFilterLinear,
    };
    static const uint32_t addr_modes[] = {
        MTLSamplerAddressModeRepeat,
        MTLSamplerAddressModeMirrorRepeat,
        MTLSamplerAddressModeClampToEdge,
        MTLSamplerAddressModeClampToZero,
    };

    for (size_t fi = 0; fi < sizeof(filters)/sizeof(filters[0]); fi++) {
        for (size_t mi = 0; mi < sizeof(mip_modes)/sizeof(mip_modes[0]); mi++) {
            for (size_t ai = 0; ai < sizeof(addr_modes)/sizeof(addr_modes[0]); ai++) {
                PgraphMtlSamplerDesc d;
                memset(&d, 0, sizeof(d));
                d.min_filter = filters[fi];
                d.mag_filter = filters[fi];
                d.mip_filter = mip_modes[mi];
                d.addr_u = d.addr_v = d.addr_w = addr_modes[ai];
                d.max_anisotropy = 1;
                d.min_lod = 0.0f;
                d.max_lod = 0.0f;
                (void)get_sampler(&d);
            }
        }
    }
}

/* -------- texture cache -------- */

static int texture_cache_limit(void)
{
    if (s_tex_cache_limit > 0) {
        return s_tex_cache_limit;
    }

    int limit = 256;
    const char *e = getenv("XEMU_METAL_TEX_CACHE_CAP");
    if (e != NULL && e[0] != '\0') {
        char *endp = NULL;
        long parsed = strtol(e, &endp, 10);
        if (endp != e && *endp == '\0') {
            if (parsed < 16) {
                parsed = 16;
            } else if (parsed > MTL_TEX_CACHE_CAP) {
                parsed = MTL_TEX_CACHE_CAP;
            }
            limit = (int)parsed;
        }
    }

    s_tex_cache_limit = limit;
    return limit;
}

static void release_tex_entry(TextureCacheEntry *e)
{
    if (e->texture) {
        pgraph_mtl_heap_release_texture(e->texture);
        e->texture = NULL;
    }
    e->in_use = false;
    e->vram_addr = 0;
    e->byte_length = 0;
    e->width = 0;
    e->height = 0;
    e->pixel_format = 0;
    e->is_cubemap = false;
    e->levels = 0;
}

static TextureCacheEntry *find_tex_entry(uint64_t vram_addr,
                                         uint64_t byte_length,
                                         uint32_t width, uint32_t height,
                                         uint32_t pixel_format,
                                         bool is_cubemap,
                                         uint32_t levels)
{
    if (vram_addr == 0) {
        return NULL;
    }
    int limit = texture_cache_limit();
    for (int i = 0; i < limit; i++) {
        TextureCacheEntry *e = &s_tex_cache[i];
        if (e->in_use &&
            e->vram_addr    == vram_addr &&
            e->byte_length  == byte_length &&
            e->width        == width &&
            e->height       == height &&
            e->pixel_format == pixel_format &&
            e->is_cubemap   == is_cubemap &&
            e->levels       == levels) {
            return e;
        }
    }
    return NULL;
}

static TextureCacheEntry *insert_tex_entry(uint64_t vram_addr,
                                           uint64_t byte_length,
                                           uint32_t width, uint32_t height,
                                           uint32_t pixel_format,
                                           bool is_cubemap,
                                           uint32_t levels,
                                           void *texture)
{
    /* Find a free slot first. */
    int limit = texture_cache_limit();
    for (int i = 0; i < limit; i++) {
        if (!s_tex_cache[i].in_use) {
            s_tex_cache[i].in_use       = true;
            s_tex_cache[i].vram_addr    = vram_addr;
            s_tex_cache[i].byte_length  = byte_length;
            s_tex_cache[i].width        = width;
            s_tex_cache[i].height       = height;
            s_tex_cache[i].pixel_format = pixel_format;
            s_tex_cache[i].is_cubemap   = is_cubemap;
            s_tex_cache[i].levels       = levels;
            s_tex_cache[i].texture      = texture;
            if (s_tex_cache_size < limit) {
                s_tex_cache_size++;
            }
            return &s_tex_cache[i];
        }
    }
    /* All slots in use — evict the next FIFO target. */
    int idx = s_tex_cache_next_evict % limit;
    s_tex_cache_next_evict = (s_tex_cache_next_evict + 1) % limit;
    release_tex_entry(&s_tex_cache[idx]);
    s_tex_cache[idx].in_use       = true;
    s_tex_cache[idx].vram_addr    = vram_addr;
    s_tex_cache[idx].byte_length  = byte_length;
    s_tex_cache[idx].width        = width;
    s_tex_cache[idx].height       = height;
    s_tex_cache[idx].pixel_format = pixel_format;
    s_tex_cache[idx].is_cubemap   = is_cubemap;
    s_tex_cache[idx].levels       = levels;
    s_tex_cache[idx].texture      = texture;
    return &s_tex_cache[idx];
}

/* -------- upload ring -------- */

static bool stage_upload_bytes(const void *data, size_t size,
                               id<MTLBuffer> *out_buf, size_t *out_off)
{
    if (size > MTL_TEX_UPLOAD_SLOT_SZ) {
        return false;
    }
    /* Align to 16 bytes for safety; Metal typically requires 4. */
    s_upload_slot_off = (s_upload_slot_off + 15) & ~((size_t)15);
    if (s_upload_slot_off + size > MTL_TEX_UPLOAD_SLOT_SZ) {
        s_upload_slot_idx = (s_upload_slot_idx + 1) % MTL_TEX_UPLOAD_SLOTS;
        s_upload_slot_off = 0;
    }
    id<MTLBuffer> buf = s_upload_slots[s_upload_slot_idx];
    void *dst = (uint8_t *)buf.contents + s_upload_slot_off;
    memcpy(dst, data, size);
    *out_buf = buf;
    *out_off = s_upload_slot_off;
    s_upload_slot_off += size;
    return true;
}

/* -------- public API -------- */

bool pgraph_mtl_texture_init(void)
{
    if (s_init) {
        return true;
    }
    s_device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (s_device == nil) {
        fprintf(stderr, "pgraph_mtl_texture_init: no Metal device\n");
        return false;
    }
    s_upload_queue = [s_device newCommandQueueWithMaxCommandBufferCount:8];
    if (s_upload_queue == nil) {
        return false;
    }
    s_upload_queue.label = @"xemu.metal.tex_upload_queue";

    /* M8: shared event used to fence draws against in-flight uploads.
     * Value 0 = nothing signaled yet; the first signal is value 1. */
    s_upload_fence_event = [s_device newSharedEvent];
    if (s_upload_fence_event == nil) {
        s_upload_queue = nil;
        return false;
    }
    s_upload_fence_event.label = @"xemu.metal.tex_upload_fence";
    atomic_store(&s_upload_fence_value, (uint64_t)0);

    @autoreleasepool {
        for (int i = 0; i < MTL_TEX_UPLOAD_SLOTS; i++) {
            id<MTLBuffer> b = [s_device
                newBufferWithLength:MTL_TEX_UPLOAD_SLOT_SZ
                            options:MTLResourceStorageModeShared |
                                    MTLResourceCPUCacheModeWriteCombined];
            if (b == nil) {
                for (int j = 0; j < i; j++) {
                    s_upload_slots[j] = nil;
                }
                return false;
            }
            b.label = [NSString stringWithFormat:@"xemu.metal.tex_upload_%d", i];
            s_upload_slots[i] = b;
        }
    }

    /* Build the default sampler — point-sampled, clamp-to-edge. */
    @autoreleasepool {
        MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
        desc.minFilter   = MTLSamplerMinMagFilterLinear;
        desc.magFilter   = MTLSamplerMinMagFilterLinear;
        desc.mipFilter   = MTLSamplerMipFilterNotMipmapped;
        desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        desc.rAddressMode = MTLSamplerAddressModeClampToEdge;
        desc.label = @"xemu.metal.default_sampler";
        id<MTLSamplerState> ss =
            [s_device newSamplerStateWithDescriptor:desc];
        if (ss == nil) {
            return false;
        }
        s_default_sampler = (__bridge_retained void *)ss;
    }

    memset(s_tex_cache, 0, sizeof(s_tex_cache));
    memset(s_smp_cache, 0, sizeof(s_smp_cache));
    memset(s_stage_texture, 0, sizeof(s_stage_texture));
    memset(s_stage_sampler, 0, sizeof(s_stage_sampler));
    memset(s_stage_external_surface, 0, sizeof(s_stage_external_surface));
    memset(s_stage_rect_tex, 0, sizeof(s_stage_rect_tex));
    for (int i = 0; i < MTL_TEX_MAX_STAGES; i++) {
        s_stage_scale[i] = 1.0f;
    }
    s_tex_cache_size = 0;
    s_tex_cache_next_evict = 0;
    s_tex_cache_limit = 0;
    s_smp_cache_size = 0;
    s_upload_slot_idx = 0;
    s_upload_slot_off = 0;

    prewarm_sampler_cache();

    atomic_store(&s_uploads, (uint64_t)0);
    atomic_store(&s_upload_bytes, (uint64_t)0);
    atomic_store(&s_upload_us_total, (uint64_t)0);
    atomic_store(&s_cache_hits, (uint64_t)0);
    atomic_store(&s_cache_miss, (uint64_t)0);
    s_init = true;
    return true;
}

void pgraph_mtl_texture_finalize(void)
{
    if (!s_init) {
        return;
    }
    int limit = texture_cache_limit();
    for (int i = 0; i < limit; i++) {
        release_tex_entry(&s_tex_cache[i]);
    }
    s_tex_cache_size = 0;
    for (int i = 0; i < s_smp_cache_size; i++) {
        if (s_smp_cache[i].state) {
            id<MTLSamplerState> ss =
                (__bridge_transfer id<MTLSamplerState>)s_smp_cache[i].state;
            (void)ss;
            s_smp_cache[i].state = NULL;
            s_smp_cache[i].in_use = false;
        }
    }
    s_smp_cache_size = 0;
    if (s_default_sampler) {
        id<MTLSamplerState> ss =
            (__bridge_transfer id<MTLSamplerState>)s_default_sampler;
        (void)ss;
        s_default_sampler = NULL;
    }
    for (int i = 0; i < MTL_TEX_UPLOAD_SLOTS; i++) {
        s_upload_slots[i] = nil;
    }
    s_upload_fence_event = nil;
    s_upload_queue = nil;
    s_device = nil;
    memset(s_stage_texture, 0, sizeof(s_stage_texture));
    memset(s_stage_sampler, 0, sizeof(s_stage_sampler));
    memset(s_stage_external_surface, 0, sizeof(s_stage_external_surface));
    memset(s_stage_rect_tex, 0, sizeof(s_stage_rect_tex));
    for (int i = 0; i < MTL_TEX_MAX_STAGES; i++) {
        s_stage_scale[i] = 1.0f;
    }
    s_init = false;
}

/* -------- M8: upload fence accessors -------- */

extern "C" void *pgraph_mtl_texture_get_upload_fence_event(void)
{
    return (__bridge void *)s_upload_fence_event;
}

extern "C" uint64_t pgraph_mtl_texture_get_upload_fence_value(void)
{
    return atomic_load(&s_upload_fence_value);
}

bool pgraph_mtl_texture_bind_slot(int stage,
                                  uint64_t vram_phys_addr,
                                  uint32_t mtl_pixel_format,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t bytes_per_row,
                                  const void *data,
                                  size_t      size,
                                  const PgraphMtlSamplerDesc *sampler)
{
    if (!s_init || stage < 0 || stage >= MTL_TEX_MAX_STAGES) {
        return false;
    }
    if (width == 0 || height == 0 || data == NULL || size == 0) {
        return false;
    }

    /* Sampler lookup / build first; cheap and always succeeds. */
    void *smp_state = s_default_sampler;
    if (sampler) {
        smp_state = get_sampler(sampler);
        if (smp_state == NULL) {
            smp_state = s_default_sampler;
        }
    }
    s_stage_sampler[stage] = smp_state;
    s_stage_scale[stage] = 1.0f;
    s_stage_external_surface[stage] = false;
    s_stage_rect_tex[stage] = false;

    /* Texture cache lookup. */
    TextureCacheEntry *cached = find_tex_entry(vram_phys_addr,
                                               (uint64_t)size,
                                               width, height,
                                               mtl_pixel_format,
                                               false, 1);
    if (cached) {
        atomic_fetch_add(&s_cache_hits, 1);
        s_stage_texture[stage] = cached->texture;
        return true;
    }
    atomic_fetch_add(&s_cache_miss, 1);

    /* Allocate a Private destination texture from heap_textures. */
    void *dst_handle = pgraph_mtl_heap_alloc_texture_2d(
        width, height, /*levels=*/1, mtl_pixel_format);
    if (dst_handle == NULL) {
        return false;
    }

    /* Stage + blit-encode upload. */
    int64_t upload_start_us = mtl_now_us();
    @autoreleasepool {
        id<MTLBuffer> stage_buf = nil;
        size_t stage_off = 0;
        if (!stage_upload_bytes(data, size, &stage_buf, &stage_off)) {
            pgraph_mtl_heap_release_texture(dst_handle);
            return false;
        }
        id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_handle;

        id<MTLCommandBuffer> cb = [s_upload_queue commandBuffer];
        cb.label = @"xemu.metal.tex_upload_cb";
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        blit.label = @"xemu.metal.tex_upload_blit";

        [blit copyFromBuffer:stage_buf
                sourceOffset:stage_off
           sourceBytesPerRow:bytes_per_row
         sourceBytesPerImage:size
                  sourceSize:MTLSizeMake(width, height, 1)
                   toTexture:dst
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];

        /* M8: signal the upload fence after the blit completes. The
         * draw layer reads the latest value from
         * pgraph_mtl_texture_get_upload_fence_value() and encodes a
         * wait on its draw command buffer — gating GPU draw execution
         * on this upload's completion without blocking the renderer
         * thread on the CPU side. */
        uint64_t signal_value =
            atomic_fetch_add(&s_upload_fence_value, 1) + 1;
        [cb encodeSignalEvent:s_upload_fence_event value:signal_value];
        [cb commit];
        /* No CPU-side wait — the draw queue's encodeWaitForEvent
         * provides correct ordering on the GPU. */
    }

    /* Insert into cache; bind to stage. */
    TextureCacheEntry *e = insert_tex_entry(vram_phys_addr,
                                            (uint64_t)size,
                                            width, height,
                                            mtl_pixel_format,
                                            false, 1,
                                            dst_handle);
    s_stage_texture[stage] = e->texture;

    atomic_fetch_add(&s_uploads, 1);
    atomic_fetch_add(&s_upload_bytes, (uint64_t)size);
    mtl_add_elapsed_us(&s_upload_us_total, upload_start_us);
    return true;
}

void pgraph_mtl_texture_unbind_slot(int stage)
{
    if (stage < 0 || stage >= MTL_TEX_MAX_STAGES) {
        return;
    }
    s_stage_texture[stage] = NULL;
    s_stage_sampler[stage] = NULL;
    s_stage_scale[stage] = 1.0f;
    s_stage_external_surface[stage] = false;
}

void pgraph_mtl_texture_invalidate_addr(uint64_t vram_phys_addr)
{
    if (!s_init) {
        return;
    }
    int limit = texture_cache_limit();
    for (int i = 0; i < limit; i++) {
        TextureCacheEntry *e = &s_tex_cache[i];
        if (!e->in_use || e->vram_addr != vram_phys_addr) {
            continue;
        }
        void *old_texture = e->texture;
        for (int stage = 0; stage < MTL_TEX_MAX_STAGES; stage++) {
            if (s_stage_texture[stage] == old_texture) {
                s_stage_texture[stage] = NULL;
                s_stage_scale[stage] = 1.0f;
                s_stage_external_surface[stage] = false;
                s_stage_rect_tex[stage] = false;
            }
        }
        release_tex_entry(e);
        if (s_tex_cache_size > 0) {
            s_tex_cache_size--;
        }
    }
}

void pgraph_mtl_texture_invalidate_range(uint64_t vram_phys_addr,
                                         uint64_t byte_length)
{
    if (!s_init || byte_length == 0) {
        return;
    }
    uint64_t end = vram_phys_addr + byte_length;
    if (end < vram_phys_addr) {
        end = UINT64_MAX;
    }
    int limit = texture_cache_limit();
    for (int i = 0; i < limit; i++) {
        TextureCacheEntry *e = &s_tex_cache[i];
        if (!e->in_use || e->vram_addr == 0) {
            continue;
        }
        uint64_t entry_len = e->byte_length;
        if (entry_len == 0) {
            entry_len = (uint64_t)e->width * e->height * 4;
        }
        uint64_t entry_end = e->vram_addr + entry_len;
        if (entry_end < e->vram_addr) {
            entry_end = UINT64_MAX;
        }
        if (e->vram_addr >= end || vram_phys_addr >= entry_end) {
            continue;
        }
        void *old_texture = e->texture;
        for (int stage = 0; stage < MTL_TEX_MAX_STAGES; stage++) {
            if (s_stage_texture[stage] == old_texture) {
                s_stage_texture[stage] = NULL;
                s_stage_scale[stage] = 1.0f;
                s_stage_external_surface[stage] = false;
                s_stage_rect_tex[stage] = false;
            }
        }
        release_tex_entry(e);
        if (s_tex_cache_size > 0) {
            s_tex_cache_size--;
        }
    }
}

bool pgraph_mtl_texture_bind_slot_external(int stage,
                                           void *texture,
                                           float scale,
                                           bool use_rect_tex,
                                           const PgraphMtlSamplerDesc *sampler)
{
    if (!s_init || stage < 0 || stage >= MTL_TEX_MAX_STAGES ||
        texture == NULL) {
        return false;
    }

    void *smp_state = s_default_sampler;
    if (sampler) {
        smp_state = get_sampler(sampler);
        if (smp_state == NULL) {
            smp_state = s_default_sampler;
        }
    }

    s_stage_texture[stage] = texture;
    s_stage_sampler[stage] = smp_state;
    s_stage_scale[stage] = (scale > 0.0f) ? scale : 1.0f;
    s_stage_external_surface[stage] = true;
    s_stage_rect_tex[stage] = use_rect_tex;
    atomic_fetch_add(&s_cache_hits, 1);
    return true;
}

bool pgraph_mtl_texture_bind_slot_surface_copy(
    int stage,
    uint64_t vram_phys_addr,
    uint64_t source_byte_length,
    uint32_t mtl_pixel_format,
    uint32_t width,
    uint32_t height,
    void *source_texture,
    float scale,
    bool use_rect_tex,
    const PgraphMtlSamplerDesc *sampler)
{
    if (!s_init || stage < 0 || stage >= MTL_TEX_MAX_STAGES ||
        width == 0 || height == 0 || source_texture == NULL) {
        return false;
    }

    void *smp_state = s_default_sampler;
    if (sampler) {
        smp_state = get_sampler(sampler);
        if (smp_state == NULL) {
            smp_state = s_default_sampler;
        }
    }
    s_stage_sampler[stage] = smp_state;
    s_stage_scale[stage] = (scale > 0.0f) ? scale : 1.0f;
    s_stage_external_surface[stage] = false;
    s_stage_rect_tex[stage] = use_rect_tex;

    TextureCacheEntry *cached = find_tex_entry(vram_phys_addr,
                                               source_byte_length,
                                               width, height,
                                               mtl_pixel_format,
                                               false, 1);
    void *dst_handle = cached ? cached->texture : NULL;
    if (cached != NULL) {
        atomic_fetch_add(&s_cache_hits, 1);
    } else {
        atomic_fetch_add(&s_cache_miss, 1);
        dst_handle = pgraph_mtl_heap_alloc_texture_2d(width, height,
                                                      /*levels=*/1,
                                                      mtl_pixel_format);
        if (dst_handle == NULL) {
            return false;
        }
    }

    @autoreleasepool {
        void *event_handle = NULL;
        uint64_t event_value = 0;
        pgraph_mtl_draw_get_done_event_state(&event_handle, &event_value);

        id<MTLTexture> src = (__bridge id<MTLTexture>)source_texture;
        id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_handle;
        if (src == nil || dst == nil) {
            if (cached == NULL) {
                pgraph_mtl_heap_release_texture(dst_handle);
            }
            return false;
        }

        id<MTLCommandBuffer> cb = [s_upload_queue commandBuffer];
        cb.label = @"xemu.metal.tex_surface_copy_cb";
        if (event_handle != NULL && event_value > 0) {
            id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
            [cb encodeWaitForEvent:ev value:event_value];
        }

        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        blit.label = @"xemu.metal.tex_surface_copy_blit";
        [blit copyFromTexture:src
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(width, height, 1)
                    toTexture:dst
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];

        uint64_t signal_value =
            atomic_fetch_add(&s_upload_fence_value, 1) + 1;
        [cb encodeSignalEvent:s_upload_fence_event value:signal_value];
        [cb commit];
    }

    if (cached == NULL) {
        TextureCacheEntry *e = insert_tex_entry(vram_phys_addr,
                                                source_byte_length,
                                                width, height,
                                                mtl_pixel_format,
                                                false, 1,
                                                dst_handle);
        s_stage_texture[stage] = e->texture;
    } else {
        s_stage_texture[stage] = cached->texture;
    }
    return true;
}

/* -------- M6 Part B: per-mip + per-face full upload -------- */

bool pgraph_mtl_texture_bind_slot_full(int stage,
                                       uint64_t vram_phys_addr,
                                       uint64_t source_byte_length,
                                       uint32_t mtl_pixel_format,
                                       bool is_cubemap,
                                       uint32_t num_faces,
                                       uint32_t levels,
                                       const PgraphMtlTextureLevel *per_level,
                                       const PgraphMtlSamplerDesc *sampler)
{
    if (!s_init || stage < 0 || stage >= MTL_TEX_MAX_STAGES) {
        return false;
    }
    if (per_level == NULL || levels == 0 || num_faces == 0) {
        return false;
    }
    if (is_cubemap && num_faces != 6) {
        return false;
    }

    /* Sampler first. */
    void *smp_state = s_default_sampler;
    if (sampler) {
        smp_state = get_sampler(sampler);
        if (smp_state == NULL) {
            smp_state = s_default_sampler;
        }
    }
    s_stage_sampler[stage] = smp_state;
    s_stage_scale[stage] = 1.0f;
    s_stage_external_surface[stage] = false;
    s_stage_rect_tex[stage] = false;

    /* Cache lookup uses level-0 face-0 dimensions as a proxy. */
    const PgraphMtlTextureLevel *l0 = &per_level[0];
    TextureCacheEntry *cached = find_tex_entry(vram_phys_addr,
                                               source_byte_length,
                                               l0->width, l0->height,
                                               mtl_pixel_format,
                                               is_cubemap, levels);
    if (cached) {
        atomic_fetch_add(&s_cache_hits, 1);
        s_stage_texture[stage] = cached->texture;
        return true;
    }
    atomic_fetch_add(&s_cache_miss, 1);

    /* Allocate the destination texture: 2D, cube, with mips as needed. */
    void *dst_handle = NULL;
    if (is_cubemap) {
        dst_handle = pgraph_mtl_heap_alloc_texture_cube(
            l0->width, levels, mtl_pixel_format);
    } else {
        dst_handle = pgraph_mtl_heap_alloc_texture_2d(
            l0->width, l0->height, levels, mtl_pixel_format);
    }
    if (dst_handle == NULL) {
        return false;
    }

    /* Stage all level data, then issue the blit. */
    int64_t upload_start_us = mtl_now_us();
    @autoreleasepool {
        id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_handle;

        id<MTLCommandBuffer> cb = [s_upload_queue commandBuffer];
        cb.label = @"xemu.metal.tex_upload_cb_full";
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        blit.label = @"xemu.metal.tex_upload_blit_full";

        bool any_uploaded = false;
        size_t total_bytes = 0;
        for (uint32_t face = 0; face < num_faces; face++) {
            for (uint32_t level = 0; level < levels; level++) {
                const PgraphMtlTextureLevel *L =
                    &per_level[face * levels + level];
                if (L->data == NULL || L->data_size == 0 ||
                    L->width == 0 || L->height == 0) {
                    continue;
                }

                id<MTLBuffer> stage_buf = nil;
                size_t stage_off = 0;
                if (!stage_upload_bytes(L->data, L->data_size,
                                        &stage_buf, &stage_off)) {
                    /* Bail out — partial upload is OK because the cache
                     * entry is not inserted yet; the texture handle is
                     * released below. */
                    [blit endEncoding];
                    pgraph_mtl_heap_release_texture(dst_handle);
                    return false;
                }

                [blit copyFromBuffer:stage_buf
                        sourceOffset:stage_off
                   sourceBytesPerRow:L->bytes_per_row
                 sourceBytesPerImage:L->data_size
                          sourceSize:MTLSizeMake(L->width, L->height, 1)
                           toTexture:dst
                    destinationSlice:face
                    destinationLevel:level
                   destinationOrigin:MTLOriginMake(0, 0, 0)];
                any_uploaded = true;
                total_bytes += L->data_size;
            }
        }
        [blit endEncoding];

        /* M8: signal the upload fence — see bind_slot above for the
         * full rationale. The draw layer reads
         * pgraph_mtl_texture_get_upload_fence_value() and waits on the
         * signal in its render command buffer. */
        uint64_t signal_value =
            atomic_fetch_add(&s_upload_fence_value, 1) + 1;
        [cb encodeSignalEvent:s_upload_fence_event value:signal_value];
        [cb commit];

        if (!any_uploaded) {
            pgraph_mtl_heap_release_texture(dst_handle);
            return false;
        }
        atomic_fetch_add(&s_uploads, 1);
        atomic_fetch_add(&s_upload_bytes, (uint64_t)total_bytes);
        mtl_add_elapsed_us(&s_upload_us_total, upload_start_us);
    }

    TextureCacheEntry *e = insert_tex_entry(vram_phys_addr,
                                            source_byte_length,
                                            l0->width, l0->height,
                                            mtl_pixel_format,
                                            is_cubemap, levels,
                                            dst_handle);
    s_stage_texture[stage] = e->texture;
    return true;
}

bool pgraph_mtl_texture_bind_slot_cached_full(
    int stage,
    uint64_t vram_phys_addr,
    uint64_t source_byte_length,
    uint32_t mtl_pixel_format,
    bool is_cubemap,
    uint32_t levels,
    uint32_t width,
    uint32_t height,
    const PgraphMtlSamplerDesc *sampler)
{
    if (!s_init || stage < 0 || stage >= MTL_TEX_MAX_STAGES ||
        width == 0 || height == 0 || levels == 0) {
        return false;
    }

    TextureCacheEntry *cached = find_tex_entry(vram_phys_addr,
                                               source_byte_length,
                                               width, height,
                                               mtl_pixel_format,
                                               is_cubemap, levels);
    if (cached == NULL) {
        return false;
    }

    void *smp_state = s_default_sampler;
    if (sampler) {
        smp_state = get_sampler(sampler);
        if (smp_state == NULL) {
            smp_state = s_default_sampler;
        }
    }
    s_stage_sampler[stage] = smp_state;
    s_stage_scale[stage] = 1.0f;
    s_stage_external_surface[stage] = false;
    s_stage_rect_tex[stage] = false;

    atomic_fetch_add(&s_cache_hits, 1);
    s_stage_texture[stage] = cached->texture;
    return true;
}

void *pgraph_mtl_texture_get_metal_texture(int stage)
{
    if (stage < 0 || stage >= MTL_TEX_MAX_STAGES) {
        return NULL;
    }
    return s_stage_texture[stage];
}

void *pgraph_mtl_texture_get_sampler_state(int stage)
{
    if (stage < 0 || stage >= MTL_TEX_MAX_STAGES) {
        return NULL;
    }
    return s_stage_sampler[stage];
}

float pgraph_mtl_texture_get_stage_scale(int stage)
{
    if (stage < 0 || stage >= MTL_TEX_MAX_STAGES) {
        return 1.0f;
    }
    return s_stage_scale[stage] > 0.0f ? s_stage_scale[stage] : 1.0f;
}

bool pgraph_mtl_texture_stage_uses_external_surface(int stage)
{
    if (stage < 0 || stage >= MTL_TEX_MAX_STAGES) {
        return false;
    }
    return s_stage_external_surface[stage];
}

bool pgraph_mtl_texture_stage_uses_rect_tex(int stage)
{
    if (stage < 0 || stage >= MTL_TEX_MAX_STAGES) {
        return false;
    }
    return s_stage_rect_tex[stage];
}

void *pgraph_mtl_texture_get_default_sampler(void)
{
    return s_default_sampler;
}

uint64_t pgraph_mtl_texture_uploads_count(void)
{
    return atomic_load(&s_uploads);
}

uint64_t pgraph_mtl_texture_upload_bytes(void)
{
    return atomic_load(&s_upload_bytes);
}

extern "C" uint64_t pgraph_mtl_texture_upload_us_total(void)
{
    return atomic_load(&s_upload_us_total);
}

uint64_t pgraph_mtl_texture_cache_hits(void)
{
    return atomic_load(&s_cache_hits);
}

uint64_t pgraph_mtl_texture_cache_misses(void)
{
    return atomic_load(&s_cache_miss);
}

uint64_t pgraph_mtl_texture_sampler_cache_size(void)
{
    return (uint64_t)s_smp_cache_size;
}
