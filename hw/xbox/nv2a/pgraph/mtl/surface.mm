/*
 * NV2A PGRAPH Metal renderer — surface manager implementation
 * (slice M2 + slice M5.9 — per-VRAM surface cache + CRTC-aware publish).
 *
 * M5.9 (2026-05-03): the M2-era single-slot manager (s_color_binding /
 * s_depth_binding) is replaced by a per-VRAM-address cache that mirrors
 * vk/surface.c's QTAILQ-of-SurfaceBindings. Each entry persists across
 * binding changes; lookup by vram_addr (`pgraph_mtl_surface_get_at`) and
 * range-overlap (`pgraph_mtl_surface_get_within`) match the vk
 * equivalents. The "currently bound" color / depth pointers are now
 * cache pointers, not private statics.
 *
 * VRAM-to-MTLTexture upload happens at first allocation via the heap-
 * staging buffer used by the texture manager. We do NOT yet wire CPU-
 * write callbacks (vk/surface.c::register_cpu_access_callback) — guest
 * writes to a bound surface's VRAM range will not trigger
 * re-upload until the next bind. This is sufficient for the magenta-
 * artifact root-cause fix (the published front-fb must be the right RT
 * for each frame); the deeper dirty-tracking is a follow-up.
 *
 * This file does NOT include hw/xbox/nv2a/nv2a_int.h — same target
 * preprocessor-flag boundary as heap.mm and ui/xemu-metal.mm.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "surface.h"
#include "heap.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" void *xemu_metal_get_device(void);

/* NV097 surface format constants (mirrored from nv2a_regs.h). We can't
 * include nv2a_int.h or nv2a_regs.h here because the .mm build does not
 * see the per-target preprocessor flags those headers transitively
 * require. Keep these in sync with nv2a_regs.h:871-883. */
enum {
    NV097_COLOR_X1R5G5B5_Z1R5G5B5 = 0x01,
    NV097_COLOR_X1R5G5B5_O1R5G5B5 = 0x02,
    NV097_COLOR_R5G6B5            = 0x03,
    NV097_COLOR_X8R8G8B8_Z8R8G8B8 = 0x04,
    NV097_COLOR_X8R8G8B8_O8R8G8B8 = 0x05,
    NV097_COLOR_X1A7R8G8B8_Z1A7R8G8B8 = 0x06,
    NV097_COLOR_X1A7R8G8B8_O1A7R8G8B8 = 0x07,
    NV097_COLOR_A8R8G8B8          = 0x08,
    NV097_COLOR_B8                = 0x09,
    NV097_COLOR_G8B8              = 0x0A,
};
enum {
    NV097_ZETA_Z16   = 1,
    NV097_ZETA_Z24S8 = 2,
};

static MTLPixelFormat nv097_color_to_mtl(uint32_t nv097)
{
    switch (nv097) {
    case NV097_COLOR_X1R5G5B5_Z1R5G5B5:
    case NV097_COLOR_X1R5G5B5_O1R5G5B5:
        return MTLPixelFormatBGR5A1Unorm;
    case NV097_COLOR_R5G6B5:
        return MTLPixelFormatB5G6R5Unorm;
    case NV097_COLOR_X8R8G8B8_Z8R8G8B8:
    case NV097_COLOR_X8R8G8B8_O8R8G8B8:
    case NV097_COLOR_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_COLOR_X1A7R8G8B8_O1A7R8G8B8:
    case NV097_COLOR_A8R8G8B8:
        return MTLPixelFormatBGRA8Unorm;
    case NV097_COLOR_B8:
        return MTLPixelFormatR8Unorm;
    case NV097_COLOR_G8B8:
        return MTLPixelFormatRG8Unorm;
    default:
        return MTLPixelFormatBGRA8Unorm;
    }
}

static MTLPixelFormat nv097_zeta_to_mtl(uint32_t nv097)
{
    switch (nv097) {
    case NV097_ZETA_Z16:
        return MTLPixelFormatDepth16Unorm;
    case NV097_ZETA_Z24S8:
        return MTLPixelFormatDepth32Float_Stencil8;
    default:
        return MTLPixelFormatDepth32Float;
    }
}

static unsigned int color_bytes_per_pixel(uint32_t nv097)
{
    switch (nv097) {
    case NV097_COLOR_X1R5G5B5_Z1R5G5B5:
    case NV097_COLOR_X1R5G5B5_O1R5G5B5:
    case NV097_COLOR_R5G6B5:
        return 2;
    case NV097_COLOR_X8R8G8B8_Z8R8G8B8:
    case NV097_COLOR_X8R8G8B8_O8R8G8B8:
    case NV097_COLOR_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_COLOR_X1A7R8G8B8_O1A7R8G8B8:
    case NV097_COLOR_A8R8G8B8:
        return 4;
    case NV097_COLOR_B8:
        return 1;
    case NV097_COLOR_G8B8:
        return 2;
    default:
        return 4;
    }
}

/* ---------------------------------------------------------------- */

static id<MTLCommandQueue> s_render_queue = nil;

/* M5.9 SurfaceBinding cache entry — a Metal-side analog of
 * vk/renderer.h::SurfaceBinding. Linked into a singly-linked list
 * keyed by vram_addr; the list is small (typically 1-8 entries) so
 * linear scan is fine. */
typedef struct MtlSurfaceBinding {
    /* Cache key — VRAM range. */
    uint32_t vram_addr;
    uint32_t size;
    uint32_t pitch;
    bool     is_color;

    /* Shape. The scaled (host-texture) dimensions match the actual
     * MTLTexture; the guest dimensions are needed for the VRAM upload
     * source rectangle (we read GUEST_W × GUEST_H pixels from VRAM
     * into the top-left of the host-scaled texture). */
    uint32_t width;          /* host-texture (scaled) width */
    uint32_t height;         /* host-texture (scaled) height */
    uint32_t guest_width;    /* M5.9-followup-C: 1× source width */
    uint32_t guest_height;   /* M5.9-followup-C: 1× source height */
    uint32_t nv097_format;
    uint32_t mtl_pixel_format;

    /* Allocations. */
    void    *texture;          /* +1 retained MTLTexture handle */
    void    *msaa_texture;     /* M11 companion (out-of-heap) */
    uint32_t msaa_sample_count;

    uint64_t last_use_seq;     /* monotonic, for LRU eviction */

    /* M5.9-followup-B (2026-05-03): CPU-write dirty tracking. The
     * `dirty_vram` flag is set by the access-callback (registered in
     * renderer.c) when the guest writes to the surface's VRAM range.
     * `access_cb` is the opaque MemAccessCallback* returned from
     * mem_access_callback_insert; surface.mm holds it as void* to keep
     * the .mm boundary clean. The renderer.c side owns the registration
     * + unregistration plumbing. */
    _Atomic(uint32_t) dirty_vram;
    void              *access_cb;

    struct MtlSurfaceBinding *next;
} MtlSurfaceBinding;

/* Cap to avoid runaway. The vk renderer uses
 * num_invalid_surfaces_to_keep=10 as the soft cap on stale entries.
 * Apple Silicon GPUs have plenty of VRAM but heap_color_rts has a
 * fixed budget at heap_init; cap aggressively. */
static const unsigned int kMaxCacheEntries = 16;

static MtlSurfaceBinding *s_cache_head = NULL;
static unsigned int       s_cache_size = 0;
static uint64_t           s_use_seq    = 0;

/* Currently-bound pointers into the cache. May be NULL between
 * frames or while the cache is empty. */
static MtlSurfaceBinding *s_color_binding = NULL;
static MtlSurfaceBinding *s_depth_binding = NULL;

/* M11: effective MSAA sample count (1 = off). */
static uint32_t s_msaa_sample_count = 1;

/* M11: counters. */
static _Atomic(uint64_t) s_msaa_resolve_count    = 0;
static _Atomic(uint64_t) s_msaa_resolve_us_total = 0;

/* The "front" framebuffer texture published to the compositor. */
static _Atomic(void *) s_front_framebuffer_texture = nullptr;

/* Diagnostic counters. */
static _Atomic(uint64_t) s_clear_count          = 0;
static _Atomic(uint64_t) s_front_fb_publishes   = 0;
/* M5.9-followup-A: GPU-side image_blit copies issued. */
static _Atomic(uint64_t) s_image_blits          = 0;
/* M5.9-followup-B+C (2026-05-03): CPU-write dirty tracking + VRAM upload. */
static _Atomic(uint64_t) s_vram_dirty_hits      = 0;
static _Atomic(uint64_t) s_vram_uploads         = 0;
static _Atomic(uint64_t) s_vram_upload_bytes    = 0;

static bool s_initialized = false;

/* ---------------------------------------------------------------- */

static void msaa_release(MtlSurfaceBinding *b)
{
    if (b->msaa_texture) {
        pgraph_mtl_heap_release_texture(b->msaa_texture);
        b->msaa_texture = NULL;
    }
    b->msaa_sample_count = 0;
}

static void msaa_ensure(MtlSurfaceBinding *b)
{
    if (s_msaa_sample_count <= 1 || b->texture == NULL) {
        msaa_release(b);
        return;
    }
    if (b->msaa_texture != NULL &&
        b->msaa_sample_count == s_msaa_sample_count) {
        return;
    }
    msaa_release(b);

    void *tex = b->is_color
        ? pgraph_mtl_heap_alloc_msaa_color(b->width, b->height,
                                           b->mtl_pixel_format,
                                           s_msaa_sample_count)
        : pgraph_mtl_heap_alloc_msaa_depth(b->width, b->height,
                                           b->mtl_pixel_format,
                                           s_msaa_sample_count);
    if (tex != NULL) {
        b->msaa_texture       = tex;
        b->msaa_sample_count  = s_msaa_sample_count;
    }
}

static void binding_destroy(MtlSurfaceBinding *b)
{
    msaa_release(b);
    if (b->texture) {
        pgraph_mtl_heap_release_texture(b->texture);
        b->texture = NULL;
    }
    free(b);
}

static MtlSurfaceBinding *cache_get_at(uint32_t vram_addr)
{
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->vram_addr == vram_addr) {
            return e;
        }
    }
    return NULL;
}

/* "_within" lookup: returns the surface whose [vram_addr, vram_addr+size)
 * range contains `addr`. Mirrors vk/surface.c::pgraph_vk_surface_get_within
 * line 711-724. Used by the CRTC publish path — `d->pcrtc.start +
 * line_offset` may not be exactly the surface's vram_addr if line_offset
 * is non-zero or the CRTC points into a sub-rectangle. */
static MtlSurfaceBinding *cache_get_within(uint32_t addr)
{
    /* Prefer an exact-vram_addr color match first; that's the common
     * case and always wins over a "contains addr" depth surface. */
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->is_color && e->vram_addr == addr) {
            return e;
        }
    }
    /* Range-overlap; prefer color over depth since the CRTC scans color. */
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->is_color &&
            addr >= e->vram_addr &&
            addr <  e->vram_addr + e->size) {
            return e;
        }
    }
    /* No color match — fall back to any matching surface. */
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (addr >= e->vram_addr &&
            addr <  e->vram_addr + e->size) {
            return e;
        }
    }
    return NULL;
}

static void cache_unlink(MtlSurfaceBinding *target)
{
    if (s_cache_head == target) {
        s_cache_head = target->next;
    } else {
        for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
            if (e->next == target) {
                e->next = target->next;
                break;
            }
        }
    }
    target->next = NULL;
    if (s_cache_size > 0) {
        s_cache_size--;
    }
}

static void cache_insert_head(MtlSurfaceBinding *e)
{
    e->next = s_cache_head;
    s_cache_head = e;
    s_cache_size++;
}

/* Evict by LRU on `last_use_seq` until <= kMaxCacheEntries-1 entries
 * remain. Skips currently-bound entries; if those alone exceed the
 * cap we raise the soft limit silently (not worth thrashing). */
static void cache_evict_lru(void)
{
    while (s_cache_size >= kMaxCacheEntries) {
        MtlSurfaceBinding *oldest = NULL;
        for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
            if (e == s_color_binding || e == s_depth_binding) {
                continue;
            }
            if (oldest == NULL || e->last_use_seq < oldest->last_use_seq) {
                oldest = e;
            }
        }
        if (oldest == NULL) {
            return; /* everything is bound — bail */
        }
        cache_unlink(oldest);
        binding_destroy(oldest);
    }
}

/* Free every cache entry. Called from the shutdown / surface_flush
 * path. Drops s_color_binding / s_depth_binding too — they alias into
 * the cache. */
static void cache_drop_all(void)
{
    s_color_binding = NULL;
    s_depth_binding = NULL;
    while (s_cache_head) {
        MtlSurfaceBinding *e = s_cache_head;
        s_cache_head = e->next;
        binding_destroy(e);
    }
    s_cache_size = 0;
}

/* ---------------------------------------------------------------- */
/* VRAM → MTLTexture upload.
 *
 * Uses a Shared MTLBuffer staged once per upload, then a blit encoder
 * copy to the Private destination texture. Synchronous: the wait keeps
 * the upload simple and the heap layout deterministic; it's only run at
 * surface-allocation time (once per (vram_addr, size) seen, not per
 * frame). For the production path the cache makes most surfaces hit
 * after the first call.
 *
 * `vram_ptr + vram_addr` is read directly. The cache trusts that the
 * caller passed a valid pointer + size; pitch may differ from
 * width*bytes_per_pixel for power-of-two swizzled surfaces, in which
 * case we copy the natural-pitch slice. M5.9 ships LINEAR-only upload
 * — swizzled/cube/3D path is deferred. */
static void upload_vram_to_texture(MtlSurfaceBinding *b,
                                   const uint8_t *vram_ptr)
{
    if (vram_ptr == NULL || b->texture == NULL ||
        b->width == 0 || b->height == 0) {
        return;
    }
    if (!b->is_color) {
        /* Depth-stencil VRAM upload is non-trivial (D32S8 vs Xbox's
         * D24S8 layout). Skip; the renderer's clear / first depth-write
         * will populate the texture before any draw reads from it. */
        return;
    }

    unsigned int bpp = color_bytes_per_pixel(b->nv097_format);
    if (bpp == 0) {
        return;
    }
    /* Source rectangle is GUEST 1× dimensions. */
    uint32_t guest_w = b->guest_width  ? b->guest_width  : b->width;
    uint32_t guest_h = b->guest_height ? b->guest_height : b->height;
    /* Clamp the destination rect to the host MTLTexture extent — for
     * surface_scale_factor=2 the texture is 2× the VRAM, so a 1×
     * upload fits with room to spare. If somehow the texture is
     * smaller than the guest source, clamp. */
    uint32_t dst_w = guest_w;
    uint32_t dst_h = guest_h;
    if (dst_w > b->width)  dst_w = b->width;
    if (dst_h > b->height) dst_h = b->height;
    if (dst_w == 0 || dst_h == 0) {
        return;
    }

    size_t row_bytes = (size_t)dst_w * bpp;
    size_t copy_size = row_bytes * dst_h;
    if (copy_size == 0) {
        return;
    }

    @autoreleasepool {
        id<MTLDevice> device =
            (__bridge id<MTLDevice>)xemu_metal_get_device();
        if (device == nil) {
            return;
        }
        id<MTLBuffer> stage = [device
            newBufferWithLength:copy_size
                        options:MTLResourceStorageModeShared];
        if (stage == nil) {
            return;
        }
        const uint8_t *src = vram_ptr + b->vram_addr;
        size_t src_pitch = b->pitch != 0 ? (size_t)b->pitch : row_bytes;
        if (src_pitch == row_bytes) {
            memcpy(stage.contents, src, copy_size);
        } else {
            uint8_t *dst = (uint8_t *)stage.contents;
            for (uint32_t y = 0; y < dst_h; y++) {
                memcpy(dst + y * row_bytes,
                       src + y * src_pitch,
                       row_bytes);
            }
        }

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.surface_upload";
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        blit.label = @"xemu.metal.surface_upload_blit";
        id<MTLTexture> tex = (__bridge id<MTLTexture>)b->texture;
        [blit copyFromBuffer:stage
                sourceOffset:0
           sourceBytesPerRow:row_bytes
         sourceBytesPerImage:copy_size
                  sourceSize:MTLSizeMake(dst_w, dst_h, 1)
                   toTexture:tex
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [cmd commit];
        /* Metal's queue ordering ensures the upload completes before
         * any subsequent draw command buffer that uses the texture
         * (both submitted on s_render_queue or the draw queue with
         * consistent submit-order semantics). */
        atomic_fetch_add(&s_vram_uploads, 1);
        atomic_fetch_add(&s_vram_upload_bytes, (uint64_t)copy_size);
        atomic_store(&b->dirty_vram, (uint32_t)0);
    }
}

/* ---------------------------------------------------------------- */

bool pgraph_mtl_surface_init(void)
{
    if (s_initialized) {
        return true;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil) {
        fprintf(stderr,
                "pgraph_mtl_surface_init: no Metal device; "
                "xemu_metal_init must run first\n");
        return false;
    }

    s_render_queue = [device newCommandQueueWithMaxCommandBufferCount:8];
    if (s_render_queue == nil) {
        fprintf(stderr,
                "pgraph_mtl_surface_init: newCommandQueue failed\n");
        return false;
    }
    s_render_queue.label = @"xemu.metal.render_queue";

    s_cache_head = NULL;
    s_cache_size = 0;
    s_color_binding = NULL;
    s_depth_binding = NULL;
    s_use_seq = 0;
    atomic_store(&s_front_framebuffer_texture, (void *)NULL);
    atomic_store(&s_clear_count, (uint64_t)0);
    atomic_store(&s_front_fb_publishes, (uint64_t)0);
    atomic_store(&s_image_blits, (uint64_t)0);
    atomic_store(&s_vram_dirty_hits, (uint64_t)0);
    atomic_store(&s_vram_uploads, (uint64_t)0);
    atomic_store(&s_vram_upload_bytes, (uint64_t)0);

    s_initialized = true;
    return true;
}

void pgraph_mtl_surface_finalize(void)
{
    if (!s_initialized) {
        return;
    }

    atomic_store(&s_front_framebuffer_texture, (void *)NULL);
    cache_drop_all();
    s_render_queue = nil;
    s_initialized = false;
}

void pgraph_mtl_surface_cache_flush(void)
{
    if (!s_initialized) {
        return;
    }
    atomic_store(&s_front_framebuffer_texture, (void *)NULL);
    cache_drop_all();
}

/* ---------------------------------------------------------------- */

static MtlSurfaceBinding *
cache_find_or_create_color(uint32_t vram_addr, uint32_t size,
                           uint32_t width, uint32_t height,
                           uint32_t guest_width, uint32_t guest_height,
                           uint32_t pitch, uint32_t nv097_color_format,
                           const uint8_t *vram_ptr)
{
    if (width == 0 || height == 0) {
        return NULL;
    }

    MtlSurfaceBinding *e = cache_get_at(vram_addr);
    if (e != NULL && e->is_color &&
        e->width == width && e->height == height &&
        e->nv097_format == nv097_color_format) {
        /* Cache hit — keep guest dims fresh in case the caller resized. */
        if (guest_width  != 0) e->guest_width  = guest_width;
        if (guest_height != 0) e->guest_height = guest_height;
        e->last_use_seq = ++s_use_seq;
        return e;
    }
    /* Existing entry but shape changed — release and recreate. */
    if (e != NULL) {
        cache_unlink(e);
        binding_destroy(e);
    }

    cache_evict_lru();

    MTLPixelFormat mtl_fmt = nv097_color_to_mtl(nv097_color_format);
    void *tex = pgraph_mtl_heap_alloc_color_rt(width, height,
                                               (uint32_t)mtl_fmt);
    if (tex == NULL) {
        return NULL;
    }

    e = (MtlSurfaceBinding *)calloc(1, sizeof(MtlSurfaceBinding));
    if (!e) {
        pgraph_mtl_heap_release_texture(tex);
        return NULL;
    }
    e->vram_addr        = vram_addr;
    e->size             = size > 0 ? size : ((uint32_t)pitch * height);
    e->pitch            = pitch;
    e->is_color         = true;
    e->width            = width;
    e->height           = height;
    e->guest_width      = guest_width  ? guest_width  : width;
    e->guest_height     = guest_height ? guest_height : height;
    e->nv097_format     = nv097_color_format;
    e->mtl_pixel_format = (uint32_t)mtl_fmt;
    e->texture          = tex;
    e->last_use_seq     = ++s_use_seq;
    atomic_store(&e->dirty_vram, (uint32_t)0);
    e->access_cb        = NULL;

    msaa_ensure(e);
    upload_vram_to_texture(e, vram_ptr);
    cache_insert_head(e);
    return e;
}

static MtlSurfaceBinding *
cache_find_or_create_depth(uint32_t vram_addr, uint32_t size,
                           uint32_t width, uint32_t height,
                           uint32_t guest_width, uint32_t guest_height,
                           uint32_t pitch, uint32_t nv097_zeta_format,
                           const uint8_t *vram_ptr)
{
    if (width == 0 || height == 0) {
        return NULL;
    }

    MtlSurfaceBinding *e = cache_get_at(vram_addr);
    if (e != NULL && !e->is_color &&
        e->width == width && e->height == height &&
        e->nv097_format == nv097_zeta_format) {
        if (guest_width  != 0) e->guest_width  = guest_width;
        if (guest_height != 0) e->guest_height = guest_height;
        e->last_use_seq = ++s_use_seq;
        return e;
    }
    if (e != NULL) {
        cache_unlink(e);
        binding_destroy(e);
    }

    cache_evict_lru();

    MTLPixelFormat mtl_fmt = nv097_zeta_to_mtl(nv097_zeta_format);
    void *tex = pgraph_mtl_heap_alloc_depth_rt(width, height,
                                               (uint32_t)mtl_fmt);
    if (tex == NULL) {
        return NULL;
    }

    e = (MtlSurfaceBinding *)calloc(1, sizeof(MtlSurfaceBinding));
    if (!e) {
        pgraph_mtl_heap_release_texture(tex);
        return NULL;
    }
    e->vram_addr        = vram_addr;
    e->size             = size > 0 ? size : ((uint32_t)pitch * height);
    e->pitch            = pitch;
    e->is_color         = false;
    e->width            = width;
    e->height           = height;
    e->guest_width      = guest_width  ? guest_width  : width;
    e->guest_height     = guest_height ? guest_height : height;
    e->nv097_format     = nv097_zeta_format;
    e->mtl_pixel_format = (uint32_t)mtl_fmt;
    e->texture          = tex;
    e->last_use_seq     = ++s_use_seq;
    atomic_store(&e->dirty_vram, (uint32_t)0);
    e->access_cb        = NULL;

    msaa_ensure(e);
    /* upload_vram_to_texture skips depth (see the function comment). */
    upload_vram_to_texture(e, vram_ptr);
    cache_insert_head(e);
    return e;
}

bool pgraph_mtl_surface_bind_color(uint32_t vram_addr, uint32_t size,
                                   uint32_t width, uint32_t height,
                                   uint32_t pitch,
                                   uint32_t nv097_color_format,
                                   const uint8_t *vram_ptr)
{
    if (!s_initialized || width == 0 || height == 0) {
        return false;
    }
    MtlSurfaceBinding *e = cache_find_or_create_color(
        vram_addr, size, width, height, /*guest_w=*/0, /*guest_h=*/0,
        pitch, nv097_color_format, vram_ptr);
    if (e == NULL) {
        return false;
    }
    s_color_binding = e;
    return true;
}

bool pgraph_mtl_surface_bind_depth(uint32_t vram_addr, uint32_t size,
                                   uint32_t width, uint32_t height,
                                   uint32_t pitch,
                                   uint32_t nv097_zeta_format,
                                   const uint8_t *vram_ptr)
{
    if (!s_initialized || width == 0 || height == 0) {
        return false;
    }
    MtlSurfaceBinding *e = cache_find_or_create_depth(
        vram_addr, size, width, height, /*guest_w=*/0, /*guest_h=*/0,
        pitch, nv097_zeta_format, vram_ptr);
    if (e == NULL) {
        return false;
    }
    s_depth_binding = e;
    return true;
}

bool pgraph_mtl_surface_bind_color_ex(uint32_t vram_addr, uint32_t size,
                                      uint32_t width, uint32_t height,
                                      uint32_t guest_width,
                                      uint32_t guest_height,
                                      uint32_t pitch,
                                      uint32_t nv097_color_format,
                                      const uint8_t *vram_ptr)
{
    if (!s_initialized || width == 0 || height == 0) {
        return false;
    }
    MtlSurfaceBinding *e = cache_find_or_create_color(
        vram_addr, size, width, height, guest_width, guest_height,
        pitch, nv097_color_format, vram_ptr);
    if (e == NULL) {
        return false;
    }
    s_color_binding = e;
    return true;
}

bool pgraph_mtl_surface_bind_depth_ex(uint32_t vram_addr, uint32_t size,
                                      uint32_t width, uint32_t height,
                                      uint32_t guest_width,
                                      uint32_t guest_height,
                                      uint32_t pitch,
                                      uint32_t nv097_zeta_format,
                                      const uint8_t *vram_ptr)
{
    if (!s_initialized || width == 0 || height == 0) {
        return false;
    }
    MtlSurfaceBinding *e = cache_find_or_create_depth(
        vram_addr, size, width, height, guest_width, guest_height,
        pitch, nv097_zeta_format, vram_ptr);
    if (e == NULL) {
        return false;
    }
    s_depth_binding = e;
    return true;
}

void *pgraph_mtl_surface_get_metal_texture_at(uint32_t vram_addr)
{
    if (!s_initialized) return NULL;
    MtlSurfaceBinding *e = cache_get_at(vram_addr);
    return e ? e->texture : NULL;
}

void *pgraph_mtl_surface_get_metal_texture_within(uint32_t vram_addr,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  uint32_t *out_format)
{
    if (!s_initialized) return NULL;
    MtlSurfaceBinding *e = cache_get_within(vram_addr);
    if (!e) {
        if (out_width)  *out_width  = 0;
        if (out_height) *out_height = 0;
        if (out_format) *out_format = 0;
        return NULL;
    }
    if (out_width)  *out_width  = e->width;
    if (out_height) *out_height = e->height;
    if (out_format) *out_format = e->nv097_format;
    return e->texture;
}

/* ---------------------------------------------------------------- */
/* Legacy ensure-color/-depth wrappers. Used by the M2-era clear path
 * that does NOT have a vram_addr (e.g. when no NV097_SET_SURFACE_OFFSET
 * has fired yet). Promote to a synthetic vram_addr=0 entry so the cache
 * still owns the binding. */

void pgraph_mtl_surface_ensure_color(uint32_t width, uint32_t height,
                                     uint32_t nv097_color_format)
{
    if (!s_initialized || width == 0 || height == 0) {
        return;
    }
    /* If the renderer has called bind_color first this frame,
     * s_color_binding already points at the right entry; only
     * synthesize a vram_addr=0 entry when nothing is bound yet. */
    if (s_color_binding != NULL &&
        s_color_binding->is_color &&
        s_color_binding->width == width &&
        s_color_binding->height == height &&
        s_color_binding->nv097_format == nv097_color_format) {
        return;
    }
    if (s_color_binding != NULL) {
        /* Existing binding has different shape — fall through to
         * find_or_create which will release+reallocate. */
    }
    MtlSurfaceBinding *e = cache_find_or_create_color(
        s_color_binding ? s_color_binding->vram_addr : 0,
        0, width, height, 0, 0, 0, nv097_color_format, NULL);
    if (e != NULL) {
        s_color_binding = e;
    }
}

void pgraph_mtl_surface_ensure_depth(uint32_t width, uint32_t height,
                                     uint32_t nv097_zeta_format)
{
    if (!s_initialized || width == 0 || height == 0) {
        return;
    }
    if (s_depth_binding != NULL &&
        !s_depth_binding->is_color &&
        s_depth_binding->width == width &&
        s_depth_binding->height == height &&
        s_depth_binding->nv097_format == nv097_zeta_format) {
        return;
    }
    MtlSurfaceBinding *e = cache_find_or_create_depth(
        s_depth_binding ? s_depth_binding->vram_addr : 0,
        0, width, height, 0, 0, 0, nv097_zeta_format, NULL);
    if (e != NULL) {
        s_depth_binding = e;
    }
}

/* ---------------------------------------------------------------- */

bool pgraph_mtl_surface_publish_front_fb(uint32_t vram_addr,
                                         const char *reason)
{
    if (!s_initialized) {
        return false;
    }
    MtlSurfaceBinding *e = cache_get_within(vram_addr);
    if (e == NULL) {
        return false;
    }

    void *prev = atomic_load(&s_front_framebuffer_texture);
    if (prev == e->texture) {
        return true;
    }
    atomic_store(&s_front_framebuffer_texture, e->texture);
    atomic_fetch_add(&s_front_fb_publishes, 1);
    e->last_use_seq = ++s_use_seq;
    fprintf(stderr,
            "xemu-perf: metal_front_fb_publish vram_addr=0x%x "
            "width=%u height=%u format=%u reason=%s\n",
            (unsigned)e->vram_addr, e->width, e->height, e->nv097_format,
            reason ? reason : "?");
    return true;
}

/* M5.9-followup-A (2026-05-03): the M5.9-era publish_color_binding()
 * helper used to be called from `pgraph_mtl_surface_clear` to publish
 * every cleared color surface as the front-fb. That stopgap is now
 * removed (see the comment in `pgraph_mtl_surface_clear`); the front-fb
 * is published exclusively by `pgraph_mtl_surface_publish_front_fb`,
 * which is invoked from `pgraph_mtl_flip_stall` with the CRTC-pointed
 * vram_addr. The helper is gone to keep the publish path single-source. */

/* ---------------------------------------------------------------- */

void pgraph_mtl_surface_clear(bool write_color, const float rgba[4],
                              bool write_zeta, float depth)
{
    if (!s_initialized) {
        return;
    }

    bool have_color_target = write_color &&
                             s_color_binding != NULL &&
                             s_color_binding->texture != NULL;
    bool have_depth_target = write_zeta &&
                             s_depth_binding != NULL &&
                             s_depth_binding->texture != NULL;
    if (!have_color_target && !have_depth_target) {
        return;
    }

    /* 2026-05-03 diagnostic — capped log of surface-clear operations to
     * identify what color the guest is clearing to (magenta artifact
     * investigation). Limited to first 32 calls to keep the log
     * bounded; controlled by XEMU_METAL_DIAG_CLEAR=1. */
    static _Atomic uint32_t s_clear_diag_count = 0;
    if (have_color_target && getenv("XEMU_METAL_DIAG_CLEAR") &&
        atomic_load(&s_clear_diag_count) < 32) {
        atomic_fetch_add(&s_clear_diag_count, 1);
        fprintf(stderr,
                "xemu-perf: metal_surface_clear vram_addr=0x%x "
                "rgba=(%.3f,%.3f,%.3f,%.3f) write_zeta=%d\n",
                (unsigned)s_color_binding->vram_addr,
                rgba[0], rgba[1], rgba[2], rgba[3],
                write_zeta ? 1 : 0);
    }

    bool have_msaa_color =
        have_color_target && s_color_binding->msaa_texture != NULL &&
        s_color_binding->msaa_sample_count > 1;
    bool have_msaa_depth =
        have_depth_target && s_depth_binding->msaa_texture != NULL &&
        s_depth_binding->msaa_sample_count > 1;
    bool resolved = false;

    @autoreleasepool {
        MTLRenderPassDescriptor *desc =
            [MTLRenderPassDescriptor renderPassDescriptor];

        if (have_color_target) {
            id<MTLTexture> tex =
                (__bridge id<MTLTexture>)s_color_binding->texture;
            if (have_msaa_color) {
                id<MTLTexture> ms =
                    (__bridge id<MTLTexture>)s_color_binding->msaa_texture;
                desc.colorAttachments[0].texture        = ms;
                desc.colorAttachments[0].resolveTexture = tex;
                desc.colorAttachments[0].loadAction     = MTLLoadActionClear;
                desc.colorAttachments[0].storeAction    =
                    MTLStoreActionMultisampleResolve;
                resolved = true;
            } else {
                desc.colorAttachments[0].texture     = tex;
                desc.colorAttachments[0].loadAction  = MTLLoadActionClear;
                desc.colorAttachments[0].storeAction = MTLStoreActionStore;
            }
            desc.colorAttachments[0].clearColor  =
                MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
        }

        if (have_depth_target) {
            id<MTLTexture> tex =
                (__bridge id<MTLTexture>)s_depth_binding->texture;
            if (have_msaa_depth) {
                id<MTLTexture> ms =
                    (__bridge id<MTLTexture>)s_depth_binding->msaa_texture;
                desc.depthAttachment.texture     = ms;
                desc.depthAttachment.loadAction  = MTLLoadActionClear;
                desc.depthAttachment.storeAction = MTLStoreActionDontCare;
            } else {
                desc.depthAttachment.texture     = tex;
                desc.depthAttachment.loadAction  = MTLLoadActionClear;
                desc.depthAttachment.storeAction = MTLStoreActionStore;
            }
            desc.depthAttachment.clearDepth  = depth;

            MTLPixelFormat fmt =
                (MTLPixelFormat)s_depth_binding->mtl_pixel_format;
            if (fmt == MTLPixelFormatDepth24Unorm_Stencil8 ||
                fmt == MTLPixelFormatDepth32Float_Stencil8 ||
                fmt == MTLPixelFormatStencil8) {
                if (have_msaa_depth) {
                    desc.stencilAttachment.texture     =
                        desc.depthAttachment.texture;
                    desc.stencilAttachment.loadAction  = MTLLoadActionClear;
                    desc.stencilAttachment.storeAction = MTLStoreActionDontCare;
                } else {
                    desc.stencilAttachment.texture     = tex;
                    desc.stencilAttachment.loadAction  = MTLLoadActionClear;
                    desc.stencilAttachment.storeAction = MTLStoreActionStore;
                }
                desc.stencilAttachment.clearStencil = 0;
            }
        }

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.clear";

        id<MTLRenderCommandEncoder> enc =
            [cmd renderCommandEncoderWithDescriptor:desc];
        enc.label = @"xemu.metal.clear_enc";
        [enc endEncoding];
        [cmd commit];
    }

    atomic_fetch_add(&s_clear_count, 1);
    if (resolved) {
        atomic_fetch_add(&s_msaa_resolve_count, 1);
        atomic_fetch_add(&s_msaa_resolve_us_total, (uint64_t)1);
    }

    if (have_color_target) {
        s_color_binding->last_use_seq = ++s_use_seq;
        /* M5.9-followup-A (2026-05-03): do NOT publish the cleared
         * surface as the front-fb. M5.9 published-on-clear as a stopgap
         * so the SDL window shows *something* before the first
         * NV097_FLIP_STALL — but on a steady-state per-frame cadence
         * the render path issues several clears (back buffer + Z buffer
         * + aux RTs) per frame, each of which would clobber the
         * CRTC-published front-fb pointer set at flip_stall.
         *
         * The result was that the compositor's
         * pgraph_mtl_get_framebuffer_metal_texture() race-read
         * whichever surface had been cleared most recently, not the
         * CRTC-pointed surface — surfacing as a wrong-dimension
         * (back-buffer-shaped) texture in the captured screenshots.
         *
         * We publish ONLY at flip_stall (renderer.c:pgraph_mtl_flip_stall
         * → pgraph_mtl_surface_publish_front_fb(crtc_addr, "crtc")) and
         * on the explicit get_framebuffer_surface call from the
         * compositor. If no flip_stall has fired yet (very early boot
         * before the first guest swap) the front-fb pointer remains
         * NULL and the compositor falls back to its blank drawable —
         * better than displaying an arbitrary cleared depth buffer. */
    }
}

/* ---------------------------------------------------------------- */

int pgraph_mtl_surface_has_front_framebuffer(void)
{
    return atomic_load(&s_front_framebuffer_texture) != NULL ? 1 : 0;
}

void *pgraph_mtl_get_framebuffer_metal_texture(void)
{
    return atomic_load(&s_front_framebuffer_texture);
}

void pgraph_mtl_release_framebuffer_metal_texture(void)
{
    /* No per-frame release needed; the cache owns texture lifetime. */
}

uint64_t pgraph_mtl_surface_clear_count(void)
{
    return atomic_load(&s_clear_count);
}

uint64_t pgraph_mtl_surface_front_fb_publishes(void)
{
    return atomic_load(&s_front_fb_publishes);
}

uint64_t pgraph_mtl_surface_cache_entries(void)
{
    return s_initialized ? (uint64_t)s_cache_size : 0;
}

/* ---------------------------------------------------------------- */

void *pgraph_mtl_surface_get_color_texture(void)
{
    if (!s_initialized || s_color_binding == NULL) return NULL;
    return s_color_binding->texture;
}

void *pgraph_mtl_surface_get_depth_texture(void)
{
    if (!s_initialized || s_depth_binding == NULL) return NULL;
    return s_depth_binding->texture;
}

uint32_t pgraph_mtl_surface_get_color_format(void)
{
    if (!s_initialized || s_color_binding == NULL) return 0;
    return s_color_binding->mtl_pixel_format;
}

uint32_t pgraph_mtl_surface_get_depth_format(void)
{
    if (!s_initialized || s_depth_binding == NULL) return 0;
    return s_depth_binding->mtl_pixel_format;
}

uint32_t pgraph_mtl_surface_get_width(void)
{
    if (!s_initialized) {
        return 0;
    }
    if (s_color_binding != NULL) return s_color_binding->width;
    if (s_depth_binding != NULL) return s_depth_binding->width;
    return 0;
}

uint32_t pgraph_mtl_surface_get_height(void)
{
    if (!s_initialized) {
        return 0;
    }
    if (s_color_binding != NULL) return s_color_binding->height;
    if (s_depth_binding != NULL) return s_depth_binding->height;
    return 0;
}

/* -------- M11 MSAA accessors -------- */

void pgraph_mtl_surface_set_msaa_sample_count(uint32_t sample_count)
{
    if (sample_count == 0) {
        sample_count = 1;
    }
    if (s_msaa_sample_count == sample_count) {
        return;
    }
    s_msaa_sample_count = sample_count;
    if (s_initialized) {
        /* Re-allocate every cached binding's MSAA companion. */
        for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
            msaa_ensure(e);
        }
    }
}

uint32_t pgraph_mtl_surface_get_msaa_sample_count(void)
{
    return s_msaa_sample_count;
}

void *pgraph_mtl_surface_get_msaa_color_texture(void)
{
    if (!s_initialized || s_msaa_sample_count <= 1) {
        return NULL;
    }
    if (s_color_binding == NULL) return NULL;
    return s_color_binding->msaa_texture;
}

void *pgraph_mtl_surface_get_msaa_depth_texture(void)
{
    if (!s_initialized || s_msaa_sample_count <= 1) {
        return NULL;
    }
    if (s_depth_binding == NULL) return NULL;
    return s_depth_binding->msaa_texture;
}

uint64_t pgraph_mtl_surface_msaa_resolve_count(void)
{
    return atomic_load(&s_msaa_resolve_count);
}

uint64_t pgraph_mtl_surface_msaa_resolve_us_total(void)
{
    return atomic_load(&s_msaa_resolve_us_total);
}

/* ---------------------------------------------------------------- */
/* M5.9-followup-A — NV097_IMAGE_BLIT GPU-side surface copy.
 *
 * The CPU-side memcpy in mtl/blit.c keeps guest VRAM correct (matching
 * vk/gl). This GPU-side blit additionally propagates the source pixels
 * into the destination MTLTexture so the per-VRAM cache's resolved
 * texture (which is what the CRTC publish path ultimately reads) shows
 * the rendered scene content rather than a stale clear color.
 *
 * Path A — formats match: encode a copyFromTexture rect-to-rect blit on
 *   a fresh command buffer.
 * Path B — formats mismatch (or src not in cache): invalidate the dst
 *   cache entry. The next bind_color at dst_vram_addr will allocate a
 *   fresh MTLTexture and upload-from-VRAM picks up the CPU-side memcpy
 *   the caller just wrote.
 * Path C — dst not in cache either: nothing to do. The next bind at
 *   dst_vram_addr will create + upload from VRAM (already fresh).
 */

uint64_t pgraph_mtl_surface_image_blits(void)
{
    return atomic_load(&s_image_blits);
}

/* ---------------------------------------------------------------- */
/* M5.9-followup-B+C — CPU-write dirty tracking + VRAM upload.
 *
 * The mark-dirty path is invoked from the CPU-write access callback
 * registered in renderer.c. The upload paths are invoked at bind time
 * (via the existing `upload_vram_to_texture` call inside cache_find_or_create_*)
 * and from the publish-front-fb path so the published surface always
 * reflects the latest guest CPU writes when CPU-writes are the
 * inter-buffer-copy mechanism.
 *
 * All entry-list iteration here happens under the same external lock
 * that protects the cache (the renderer-ops dispatch holds
 * `d->pgraph.lock`; the access callback acquires it before calling
 * into mark_dirty_overlapping). The atomic _Atomic(uint32_t)
 * dirty_vram bit is the cross-thread synchronization primitive
 * between callback and bind-time check.
 */

uint64_t pgraph_mtl_surface_vram_dirty_hits(void)
{
    return atomic_load(&s_vram_dirty_hits);
}

uint64_t pgraph_mtl_surface_vram_uploads(void)
{
    return atomic_load(&s_vram_uploads);
}

uint64_t pgraph_mtl_surface_vram_upload_bytes(void)
{
    return atomic_load(&s_vram_upload_bytes);
}

void pgraph_mtl_surface_mark_dirty_overlapping(uint32_t addr, uint32_t len)
{
    if (!s_initialized || len == 0) {
        return;
    }
    uint32_t range_end = addr + len;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        uint32_t ent_end = e->vram_addr + e->size;
        bool overlaps = !(e->vram_addr >= range_end || addr >= ent_end);
        if (!overlaps) {
            continue;
        }
        uint32_t prev = atomic_exchange(&e->dirty_vram, (uint32_t)1);
        if (prev == 0) {
            atomic_fetch_add(&s_vram_dirty_hits, 1);
            /* One-shot diagnostic per dirty event. Capped natively by
             * dirty_vram only being 0→1 once until consumed. */
            fprintf(stderr,
                    "xemu-perf: metal_surface_dirty vram_addr=0x%x "
                    "size=%u write_addr=0x%x write_len=%u "
                    "is_color=%d\n",
                    (unsigned)e->vram_addr, e->size,
                    (unsigned)addr, (unsigned)len,
                    (int)e->is_color);
        }
    }
}

void pgraph_mtl_surface_register_access_cb_for(uint32_t vram_addr, void *cb)
{
    if (!s_initialized) {
        return;
    }
    MtlSurfaceBinding *e = cache_get_at(vram_addr);
    if (e != NULL) {
        e->access_cb = cb;
    }
}

void pgraph_mtl_surface_unregister_access_cb_for(uint32_t vram_addr,
                                                 void **out_cb)
{
    if (out_cb) {
        *out_cb = NULL;
    }
    if (!s_initialized) {
        return;
    }
    MtlSurfaceBinding *e = cache_get_at(vram_addr);
    if (e != NULL) {
        if (out_cb) {
            *out_cb = e->access_cb;
        }
        e->access_cb = NULL;
    }
}

unsigned int pgraph_mtl_surface_upload_dirty(const uint8_t *vram_ptr)
{
    if (!s_initialized || vram_ptr == NULL) {
        return 0;
    }
    unsigned int n = 0;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (atomic_load(&e->dirty_vram)) {
            upload_vram_to_texture(e, vram_ptr);
            n++;
        }
    }
    return n;
}

void pgraph_mtl_surface_upload_if_dirty_at(uint32_t vram_addr,
                                           const uint8_t *vram_ptr)
{
    if (!s_initialized || vram_ptr == NULL) {
        return;
    }
    /* Use the within-range lookup so a CRTC publish at a non-zero
     * line_offset still resolves to the surface that contains it. */
    MtlSurfaceBinding *e = cache_get_within(vram_addr);
    if (e == NULL) {
        return;
    }
    if (atomic_load(&e->dirty_vram)) {
        upload_vram_to_texture(e, vram_ptr);
    }
}

void pgraph_mtl_surface_force_upload_at(uint32_t vram_addr,
                                        const uint8_t *vram_ptr)
{
    if (!s_initialized || vram_ptr == NULL) {
        return;
    }
    MtlSurfaceBinding *e = cache_get_within(vram_addr);
    if (e == NULL) {
        return;
    }
    upload_vram_to_texture(e, vram_ptr);
}

unsigned int pgraph_mtl_surface_iter_addresses(uint32_t *out, unsigned int cap)
{
    if (!s_initialized || out == NULL || cap == 0) {
        return 0;
    }
    unsigned int n = 0;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL && n < cap;
         e = e->next) {
        out[n++] = e->vram_addr;
    }
    return n;
}

/* Compute the effective host-space rectangle for a given guest-space
 * (x, y, w, h) on the MTLTexture of binding `b`. The MTLTexture is
 * allocated at the SCALED dimensions (surface_scale_factor=2 -> 2x VRAM)
 * but the guest writes blit coordinates in 1x space. For cache entries
 * that were bound at the scaled dims (the common path through
 * mtl_bind_current_surfaces) we scale by texture_dim / 1.
 *
 * We do not have a separate "guest dim" field on the binding — the
 * b->width/height ARE the texture dims. So if the caller passed guest
 * rect (x=0, y=0, w=640, h=480) and the binding is 1280x960, we infer
 * the scale from b->width / known_guest_width. To keep this robust
 * without storing the guest dim explicitly we use the simpler rule:
 * if width/height matches the binding 1:1 we pass through; otherwise
 * we proportionally scale. */
static void scale_rect_for_binding(MtlSurfaceBinding *b,
                                   uint32_t guest_max_w,
                                   uint32_t guest_max_h,
                                   uint32_t *x, uint32_t *y,
                                   uint32_t *w, uint32_t *h)
{
    if (b == NULL || guest_max_w == 0 || guest_max_h == 0) {
        return;
    }
    /* Scale factor is texture_dim / guest_dim, rounded down. The
     * guest_max is the larger of (caller width, caller x+w) so a
     * partial-rect blit infers the scale from the surface footprint
     * not the rect alone. */
    uint32_t sx = b->width >= guest_max_w
        ? b->width / (guest_max_w ? guest_max_w : 1) : 1;
    uint32_t sy = b->height >= guest_max_h
        ? b->height / (guest_max_h ? guest_max_h : 1) : 1;
    if (sx == 0) sx = 1;
    if (sy == 0) sy = 1;
    *x *= sx;
    *y *= sy;
    *w *= sx;
    *h *= sy;
    /* Clamp to texture extent in case of rounding. */
    if (*x >= b->width)  *x = b->width  ? b->width  - 1 : 0;
    if (*y >= b->height) *y = b->height ? b->height - 1 : 0;
    if (*x + *w > b->width)  *w = b->width  - *x;
    if (*y + *h > b->height) *h = b->height - *y;
}

bool pgraph_mtl_surface_blit_copy(uint32_t src_vram_addr,
                                  uint32_t dst_vram_addr,
                                  uint32_t src_x, uint32_t src_y,
                                  uint32_t dst_x, uint32_t dst_y,
                                  uint32_t width, uint32_t height)
{
    if (!s_initialized || width == 0 || height == 0) {
        return false;
    }

    MtlSurfaceBinding *src = cache_get_within(src_vram_addr);
    MtlSurfaceBinding *dst = cache_get_within(dst_vram_addr);

    /* Path C: neither in cache (or only dst) -> nothing GPU-side. The
     * caller's memcpy already updated VRAM; future bind picks it up. */
    if (src == NULL && dst == NULL) {
        return false;
    }
    /* Only-src (no dst): invalidating doesn't apply (no entry to drop).
     * Future bind on dst_vram_addr will do a fresh upload. Done. */
    if (src == NULL) {
        return false;
    }

    /* Path B: src exists but dst either missing or format/aspect mismatch.
     * Drop the dst entry so the next bind reallocates with fresh upload-
     * from-VRAM (the caller's memcpy is the authoritative state). */
    bool format_match =
        dst != NULL &&
        dst->is_color == src->is_color &&
        dst->mtl_pixel_format == src->mtl_pixel_format;

    if (!format_match) {
        if (dst != NULL) {
            /* Don't evict if dst is the actively-bound color/depth — that
             * would yank the texture out from a draw in progress. The
             * caller drained the open pass via flush_open_pass; if dst is
             * still bound for the next draw, mtl_bind_current_surfaces
             * will rebuild it on demand. Safe to drop. */
            cache_unlink(dst);
            if (dst == s_color_binding) s_color_binding = NULL;
            if (dst == s_depth_binding) s_depth_binding = NULL;
            /* Also clear the front-fb pointer if it referenced this dst. */
            if (atomic_load(&s_front_framebuffer_texture) == dst->texture) {
                atomic_store(&s_front_framebuffer_texture, (void *)NULL);
            }
            binding_destroy(dst);
        }
        return false;
    }

    /* Path A: GPU-side rect-to-rect copy. */

    /* Translate the guest-space rect to host MTLTexture-space rects.
     * The blit width/height arrives as guest 1x; surfaces are allocated
     * at scaled dims. We use the larger of (rect right edge, surface
     * footprint dim) to infer the scale.
     *
     * Conservative path: if width/height already match the surface
     * dimensions exactly we don't scale at all. */
    uint32_t s_x = src_x, s_y = src_y, s_w = width, s_h = height;
    uint32_t d_x = dst_x, d_y = dst_y, d_w = width, d_h = height;

    /* Heuristic: if either rect plus its origin fits inside surface dims
     * already, leave as-is (caller passed host-space coords). Otherwise
     * scale. The blit is most often "copy the entire RT to the front",
     * so we mainly hit the fits-inside fast path with origin (0,0). */
    if (src_x + width > src->width || src_y + height > src->height) {
        scale_rect_for_binding(src,
                               src_x + width, src_y + height,
                               &s_x, &s_y, &s_w, &s_h);
    }
    if (dst_x + width > dst->width || dst_y + height > dst->height) {
        scale_rect_for_binding(dst,
                               dst_x + width, dst_y + height,
                               &d_x, &d_y, &d_w, &d_h);
    }

    /* Final clamp — the source-rect dims must equal the dest-rect dims
     * for copyFromTexture (rect-to-rect, no scaling). Use the smaller of
     * the two if they diverged after scaling/clamping. */
    uint32_t copy_w = s_w < d_w ? s_w : d_w;
    uint32_t copy_h = s_h < d_h ? s_h : d_h;
    if (copy_w == 0 || copy_h == 0) {
        return false;
    }

    @autoreleasepool {
        id<MTLTexture> src_tex = (__bridge id<MTLTexture>)src->texture;
        id<MTLTexture> dst_tex = (__bridge id<MTLTexture>)dst->texture;
        if (src_tex == nil || dst_tex == nil) {
            return false;
        }

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.image_blit";
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        blit.label = @"xemu.metal.image_blit_enc";
        [blit copyFromTexture:src_tex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(s_x, s_y, 0)
                   sourceSize:MTLSizeMake(copy_w, copy_h, 1)
                    toTexture:dst_tex
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(d_x, d_y, 0)];
        [blit endEncoding];
        [cmd commit];

        /* Mark dst entry recently-used to keep it from LRU eviction. */
        dst->last_use_seq = ++s_use_seq;
        src->last_use_seq = ++s_use_seq;
    }

    atomic_fetch_add(&s_image_blits, 1);

    /* If the destination is the front-fb (CRTC published) surface, the
     * cache pointer is unchanged — the texture has been updated in
     * place. The compositor reads the same atomic pointer next present,
     * which now contains the blitted scene content. */

    return true;
}
