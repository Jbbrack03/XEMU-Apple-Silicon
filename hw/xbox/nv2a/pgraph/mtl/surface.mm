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

/* M5.10: cross-module accessors for the open-pass + draw-done fence.
 * Defined in mtl/draw.mm. Surface.mm cannot include draw.h because
 * draw.h pulls in vertex.h which depends on per-target preprocessor
 * flags this .mm file does not see. */
extern "C" void pgraph_mtl_draw_get_open_pass_textures(void **out_color,
                                                       void **out_depth);
extern "C" void pgraph_mtl_draw_get_done_event_state(void **out_event,
                                                     uint64_t *out_value);
extern "C" void pgraph_mtl_draw_flush_open_pass(void);

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

    /* M5.10 (2026-05-03): GPU-side draw dirty tracking. Set after a
     * flush_draw / clear_surface lands on this binding's MTLTexture;
     * cleared by `download_*` once pixels have been written back to
     * guest VRAM. Mirrors `vk/renderer.h::SurfaceBinding.draw_dirty`. */
    _Atomic(uint32_t) draw_dirty;

    struct MtlSurfaceBinding *next;
} MtlSurfaceBinding;

/* Cap to avoid runaway. The vk renderer uses
 * num_invalid_surfaces_to_keep=10 as the soft cap on stale entries.
 * Apple Silicon GPUs have plenty of VRAM but heap_color_rts has a
 * fixed budget at heap_init; cap aggressively. */
/* 2026-05-03 magenta-RT fix: raised from 16 to 32 because PGR2 has at
 * least 11 distinct color render targets + ~4 depth surfaces + the
 * synthetic vram_addr=0 ensure-by-shape entries — at 16 the cap is
 * always reached and LRU eviction kicks out infrequently-bound entries
 * (notably the published front-fb) while the back-buffer + aux RTs hog
 * the cache. 32 gives PGR2 (and similar AAA Xbox titles) margin to
 * avoid eviction during steady-state gameplay. Each entry is small
 * (struct + MTLTexture + maybe an MSAA companion) — 32 entries is on
 * the order of tens of MB on Apple Silicon UMA, well within budget. */
static const unsigned int kMaxCacheEntries = 32;

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
/* M5.10 (2026-05-03): VRAM-coherent surface download (GPU → VRAM). */
static _Atomic(uint64_t) s_surface_downloads    = 0;
static _Atomic(uint64_t) s_surface_download_bytes = 0;
/* 2026-05-03 magenta-RT diagnostic: count cache entries destroyed +
 * recreated due to shape mismatch on a same-vram_addr rebind. */
static _Atomic(uint64_t) s_recreate_shape_mismatch = 0;

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

/* 2026-05-03 magenta-RT fix: color/depth-filtered lookups so a
 * same-vram_addr color binding and depth binding don't collide on
 * cache_get_at and destroy each other every bind. The legacy
 * ensure-by-shape paths use vram_addr=0 as a sentinel, and PGR2 has at
 * least one real color surface that resolves to vram_addr=0 too — both
 * collide with the depth bindings keyed at 0. Splitting the lookup by
 * is_color lets the cache hold one color + one depth entry per
 * vram_addr safely (matching how the vk renderer treats them as
 * orthogonal). The destroy-and-recreate fall-through in
 * cache_find_or_create_color/_depth is now properly limited to a real
 * shape change of the SAME aspect, never a color-vs-depth confusion. */
static MtlSurfaceBinding *cache_get_at_color(uint32_t vram_addr)
{
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->vram_addr == vram_addr && e->is_color) {
            return e;
        }
    }
    return NULL;
}

static MtlSurfaceBinding *cache_get_at_depth(uint32_t vram_addr)
{
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->vram_addr == vram_addr && !e->is_color) {
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
 * remain. Skips currently-bound entries AND the entry whose texture is
 * the currently-published front-fb (so the compositor never reads from
 * a freed MTLTexture). If only pinned entries remain we raise the soft
 * limit silently (not worth thrashing).
 *
 * 2026-05-03 magenta-RT fix: front-fb pin added because PGR2 binds the
 * back-buffer for rendering (which makes back-buffer = s_color_binding)
 * while the front-fb at pcrtc.start (the displayed framebuffer) is not
 * currently bound and was eligible for eviction. The publish dedupe in
 * pgraph_mtl_surface_publish_front_fb returns early when the published
 * texture is already current and DOES NOT bump last_use_seq, so a
 * stably-published front-fb's LRU score grew stale and the cache was
 * happy to evict it — destroying the texture the compositor is showing.
 * Pinning fixes that without changing the publish path. */
static void cache_evict_lru(void)
{
    while (s_cache_size >= kMaxCacheEntries) {
        MtlSurfaceBinding *oldest = NULL;
        void *front_tex = atomic_load(&s_front_framebuffer_texture);
        /* M5.10: also pin the open-pass color/depth textures so a
         * mid-pass eviction does not yank an attached MTLTexture out
         * from under the still-encoding render command buffer. */
        void *open_color = NULL, *open_depth = NULL;
        pgraph_mtl_draw_get_open_pass_textures(&open_color, &open_depth);
        for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
            if (e == s_color_binding || e == s_depth_binding) {
                continue;
            }
            if (front_tex != NULL && e->texture == front_tex) {
                continue;
            }
            if (open_color != NULL && e->texture == open_color) {
                continue;
            }
            if (open_depth != NULL && e->texture == open_depth) {
                continue;
            }
            if (oldest == NULL || e->last_use_seq < oldest->last_use_seq) {
                oldest = e;
            }
        }
        if (oldest == NULL) {
            return; /* everything is pinned — bail */
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
/* M5.10 (2026-05-03): GPU → VRAM surface download.
 *
 * The download path mirrors `vk/surface.c::download_surface_to_buffer`
 * but uses Metal's MTLBlitCommandEncoder copyFromTexture:toBuffer:.
 * The destination is a Shared MTLBuffer; on Apple Silicon UMA the
 * post-`waitUntilCompleted` `[buffer contents]` view is coherent with
 * the GPU writes (the blit encoder runs through the GPU's tile-store
 * path back to system memory).
 *
 * Caveats:
 *   - Color formats only for the M5.10 v1. Depth-stencil download is
 *     more involved (D32S8 vs Xbox's D24S8 packing) and is deferred —
 *     PGR2 / Crimson / Rainbow do NOT depend on depth readback for
 *     normal display.
 *   - Source rect is GUEST 1× dimensions (b->guest_width / _height).
 *     The MTLTexture is allocated at host-scaled dimensions
 *     (surface_scale_factor=2 → 2x); the upper-left 1× sub-rect is
 *     where the guest VRAM layout maps.
 *   - The blit + waitUntilCompleted is synchronous from the caller's
 *     perspective: this matches vk's `pgraph_vk_finish` before the
 *     download. Frequency is bounded — the download fires only when
 *     the surface is `draw_dirty == 1` AND a consumer (publish path,
 *     image_blit src read, eviction) requests it.
 *
 * Locking: the caller holds either pgraph.lock (during a renderer-ops
 * dispatch) or the renderer-thread invariant. The download performs
 * `[cmdBuffer waitUntilCompleted]` outside any other lock. */
/* Returns true iff bytes were actually written to vram_ptr_base +
 * vram_addr. Callers gate `memory_region_set_client_dirty` and the
 * caller-side draw_dirty bookkeeping on the return value so unsupported
 * cases (depth, scaled-non-1×, format unknown) don't spuriously mark
 * VRAM dirty for downstream consumers. */
static bool download_surface_to_vram(MtlSurfaceBinding *b,
                                     uint8_t *vram_ptr_base)
{
    if (vram_ptr_base == NULL || b == NULL || b->texture == NULL) {
        return false;
    }
    if (!b->is_color) {
        /* Depth download deferred. Do NOT clear draw_dirty — leave it
         * set so a future slice that adds depth-stencil download
         * support picks up the still-pending download. Callers that
         * walk the cache will keep skipping this entry until then. */
        return false;
    }
    if (b->width == 0 || b->height == 0) {
        return false;
    }

    unsigned int bpp = color_bytes_per_pixel(b->nv097_format);
    if (bpp == 0) {
        return false;
    }

    uint32_t guest_w = b->guest_width  ? b->guest_width  : b->width;
    uint32_t guest_h = b->guest_height ? b->guest_height : b->height;
    /* M5.10 codex finding (HIGH severity, 2026-05-03):
     *
     * The MTLTexture is allocated at host-scaled dims (surface_scale=2
     * gives a 2× texture). A `copyFromTexture:sourceOrigin:sourceSize:`
     * blit reads PIXELS at the requested rect — it cannot downsample.
     * If we naively read `(guest_w, guest_h)` from `(0, 0)` the
     * destination buffer ends up holding the upper-left guest-sized
     * crop of the host-scaled image, not the visible-frame content
     * scaled to guest dims. Writing that cropped buffer to VRAM at
     * `b->vram_addr` would corrupt downstream consumers (CRTC scan-out
     * in particular).
     *
     * The proper fix is a downsample render pass (vk does this with
     * `vkCmdBlitImage` + `VK_FILTER_LINEAR` against an `image_scratch`
     * 1× target). Out of scope for the M5.10 MVP.
     *
     * Defensive behavior here: skip the download whenever the texture
     * is host-scaled (`b->width != guest_w` or `b->height != guest_h`),
     * leave `draw_dirty` set, and return false. Callers see the entry
     * remains dirty; downstream consumers that need pixel-coherent
     * VRAM at this address will still see stale guest VRAM, but that
     * is preferable to writing a corrupted crop.
     *
     * The Apple Silicon default `surface_scale = 2` triggers this skip
     * universally on PGR2 / Rainbow / Crimson / SC2. To exercise the
     * download path for development, set
     * `XEMU_DISPLAY_SCALE=1 XEMU_METAL_FRONT_FB_DOWNLOAD=1`; otherwise
     * the path is structurally inert until the downsample slice lands. */
    if (b->width != guest_w || b->height != guest_h) {
        return false;
    }
    uint32_t src_w = guest_w;
    uint32_t src_h = guest_h;
    if (src_w > b->width)  src_w = b->width;
    if (src_h > b->height) src_h = b->height;
    if (src_w == 0 || src_h == 0) {
        return false;
    }

    size_t row_bytes = (size_t)src_w * bpp;
    size_t copy_size = row_bytes * src_h;
    if (copy_size == 0) {
        return;
    }

    /* The render-queue blit-encoder reads from a draw-target texture
     * that may be the destination of pending render-encoder work on
     * s_draw_queue. Drain the open pass first — committing prior draws
     * — and then `encodeWaitForEvent:` against the latest draw-done
     * value so cross-queue ordering is enforced even when MoltenVK /
     * future Metal versions weaken implicit ordering. The drain on
     * the same thread is sufficient to preserve correctness; the
     * fence is belt-and-suspenders. */
    pgraph_mtl_draw_flush_open_pass();
    void     *event_handle = NULL;
    uint64_t  event_value  = 0;
    pgraph_mtl_draw_get_done_event_state(&event_handle, &event_value);

    bool succeeded = false;
    @autoreleasepool {
        id<MTLDevice> device =
            (__bridge id<MTLDevice>)xemu_metal_get_device();
        if (device == nil) {
            return false;
        }
        id<MTLBuffer> stage = [device
            newBufferWithLength:copy_size
                        options:MTLResourceStorageModeShared];
        if (stage == nil) {
            return false;
        }
        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.surface_download";
        if (event_handle != NULL && event_value > 0) {
            id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
            [cmd encodeWaitForEvent:ev value:event_value];
        }

        id<MTLTexture> tex = (__bridge id<MTLTexture>)b->texture;
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        blit.label = @"xemu.metal.surface_download_blit";
        [blit copyFromTexture:tex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(src_w, src_h, 1)
                     toBuffer:stage
            destinationOffset:0
       destinationBytesPerRow:row_bytes
     destinationBytesPerImage:copy_size];
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        /* Copy the staging buffer into guest VRAM at the source pitch.
         * If pitch != natural, mirror the pattern in the upload helper. */
        uint8_t       *dst       = vram_ptr_base + b->vram_addr;
        size_t         dst_pitch = b->pitch != 0 ? (size_t)b->pitch : row_bytes;
        const uint8_t *src       = (const uint8_t *)stage.contents;
        if (dst_pitch == row_bytes) {
            memcpy(dst, src, copy_size);
        } else {
            for (uint32_t y = 0; y < src_h; y++) {
                memcpy(dst + y * dst_pitch,
                       src + y * row_bytes,
                       row_bytes);
            }
        }
        atomic_fetch_add(&s_surface_downloads, 1);
        atomic_fetch_add(&s_surface_download_bytes, (uint64_t)copy_size);
        succeeded = true;
    }

    if (succeeded) {
        atomic_store(&b->draw_dirty, (uint32_t)0);
        /* The freshly-downloaded VRAM IS the latest texture content; clear
         * dirty_vram so the next bind-time upload doesn't redundantly copy
         * the same bytes back to the texture (the texture already has them).
         * The CPU-write callback will re-set dirty_vram if the guest writes
         * to this range after the download. */
        atomic_store(&b->dirty_vram, (uint32_t)0);
    }
    return succeeded;
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
    atomic_store(&s_surface_downloads, (uint64_t)0);
    atomic_store(&s_surface_download_bytes, (uint64_t)0);

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

    /* 2026-05-03 magenta-RT fix: color-only lookup so a same-vram_addr
     * depth binding doesn't trigger a destroy-and-recreate of this
     * color slot. */
    MtlSurfaceBinding *e = cache_get_at_color(vram_addr);
    if (e != NULL &&
        e->width == width && e->height == height &&
        e->nv097_format == nv097_color_format) {
        /* Cache hit — keep guest dims fresh in case the caller resized. */
        if (guest_width  != 0) e->guest_width  = guest_width;
        if (guest_height != 0) e->guest_height = guest_height;
        e->last_use_seq = ++s_use_seq;
        return e;
    }
    /* Existing entry but shape changed — release and recreate.
     *
     * 2026-05-03 magenta-RT diagnostic: bump a counter and emit a bounded
     * log line whenever we recreate due to shape mismatch. PGR2 may bind
     * the same vram_addr at multiple distinct shapes (e.g. a 1280×480
     * supersampled back-buffer reconfigured as 640×480 mid-frame). Each
     * recreate destroys all previously rendered content for that
     * vram_addr — explaining "draws hit but screenshots show fresh
     * texture content".
     *
     * Counter accumulates monotonically; xemu-metal-perf.c reads it as
     * METAL_SURFACE_RECREATE_SHAPE_MISMATCH per interval. Diagnostic log
     * line is rate-limited to first 16 events. */
    if (e != NULL) {
        atomic_fetch_add(&s_recreate_shape_mismatch, 1);
        static _Atomic uint32_t s_recreate_log_count = 0;
        if (atomic_load(&s_recreate_log_count) < 16) {
            atomic_fetch_add(&s_recreate_log_count, 1);
            fprintf(stderr,
                    "xemu-perf: metal_surface_recreate vram_addr=0x%x "
                    "old=%ux%u/fmt%u new=%ux%u/fmt%u (color)\n",
                    (unsigned)vram_addr,
                    e->width, e->height, e->nv097_format,
                    width, height, nv097_color_format);
        }
        /* 2026-05-03 magenta-RT fix (codex-validate finding): if the
         * entry being destroyed is the currently-published front-fb,
         * clear the publish pointer first so the compositor doesn't
         * dereference a freed MTLTexture. Symmetric with the LRU
         * front-fb pin in cache_evict_lru — that pin protects against
         * eviction; this clear protects against shape-change destroy.
         * Without this, a guest-side surface reconfiguration of the
         * displayed buffer would reproduce the heap-default magenta
         * class artifact via a different mechanism. */
        if (atomic_load(&s_front_framebuffer_texture) == e->texture) {
            atomic_store(&s_front_framebuffer_texture, (void *)NULL);
        }
        /* M5.10: if the open render pass currently references this
         * texture, drain it before binding_destroy releases the
         * MTLTexture out from under the encoder. */
        void *open_color_tex = NULL, *open_depth_tex = NULL;
        pgraph_mtl_draw_get_open_pass_textures(&open_color_tex,
                                               &open_depth_tex);
        if (e->texture == open_color_tex || e->texture == open_depth_tex) {
            pgraph_mtl_draw_flush_open_pass();
        }
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
    atomic_store(&e->draw_dirty, (uint32_t)0);
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

    /* 2026-05-03 magenta-RT fix: depth-only lookup. Symmetric with the
     * color-only filter above. */
    MtlSurfaceBinding *e = cache_get_at_depth(vram_addr);
    if (e != NULL &&
        e->width == width && e->height == height &&
        e->nv097_format == nv097_zeta_format) {
        if (guest_width  != 0) e->guest_width  = guest_width;
        if (guest_height != 0) e->guest_height = guest_height;
        e->last_use_seq = ++s_use_seq;
        return e;
    }
    if (e != NULL) {
        atomic_fetch_add(&s_recreate_shape_mismatch, 1);
        static _Atomic uint32_t s_recreate_log_count_d = 0;
        if (atomic_load(&s_recreate_log_count_d) < 16) {
            atomic_fetch_add(&s_recreate_log_count_d, 1);
            fprintf(stderr,
                    "xemu-perf: metal_surface_recreate vram_addr=0x%x "
                    "old=%ux%u/fmt%u new=%ux%u/fmt%u (depth)\n",
                    (unsigned)vram_addr,
                    e->width, e->height, e->nv097_format,
                    width, height, nv097_zeta_format);
        }
        /* M5.10: open-pass drain symmetric with the color path. */
        void *open_color_tex = NULL, *open_depth_tex = NULL;
        pgraph_mtl_draw_get_open_pass_textures(&open_color_tex,
                                               &open_depth_tex);
        if (e->texture == open_color_tex || e->texture == open_depth_tex) {
            pgraph_mtl_draw_flush_open_pass();
        }
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
    atomic_store(&e->draw_dirty, (uint32_t)0);
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

    /* 2026-05-03 magenta-RT fix: bump last_use_seq before the dedupe
     * check so a stably-published front-fb keeps a fresh LRU score and
     * is never picked as the eviction victim while the compositor is
     * actively reading it. The front-fb-pin guard in cache_evict_lru is
     * the primary protection; this is a belt-and-suspenders refresh. */
    e->last_use_seq = ++s_use_seq;
    void *prev = atomic_load(&s_front_framebuffer_texture);
    if (prev == e->texture) {
        return true;
    }
    atomic_store(&s_front_framebuffer_texture, e->texture);
    atomic_fetch_add(&s_front_fb_publishes, 1);
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

uint32_t pgraph_mtl_surface_get_color_vram_addr(void)
{
    if (!s_initialized || s_color_binding == NULL) return 0;
    return s_color_binding->vram_addr;
}

uint32_t pgraph_mtl_surface_get_depth_vram_addr(void)
{
    if (!s_initialized || s_depth_binding == NULL) return 0;
    return s_depth_binding->vram_addr;
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

uint64_t pgraph_mtl_surface_recreate_shape_mismatch(void)
{
    return atomic_load(&s_recreate_shape_mismatch);
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

unsigned int pgraph_mtl_surface_iter_address_size(uint32_t *out_addrs,
                                                  uint32_t *out_sizes,
                                                  unsigned int cap)
{
    if (!s_initialized || out_addrs == NULL || out_sizes == NULL ||
        cap == 0) {
        return 0;
    }
    unsigned int n = 0;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL && n < cap;
         e = e->next) {
        out_addrs[n] = e->vram_addr;
        out_sizes[n] = (uint32_t)e->size;
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------- */
/* M5.10 (2026-05-03): set-draw-dirty + download API. */

void pgraph_mtl_surface_set_draw_dirty_color(void)
{
    if (!s_initialized || s_color_binding == NULL) {
        return;
    }
    atomic_store(&s_color_binding->draw_dirty, (uint32_t)1);
}

void pgraph_mtl_surface_set_draw_dirty_depth(void)
{
    if (!s_initialized || s_depth_binding == NULL) {
        return;
    }
    atomic_store(&s_depth_binding->draw_dirty, (uint32_t)1);
}

/* Helper: download a single entry, then invoke the QEMU-side dirty
 * callback with the VRAM range that was written. */
static void download_and_notify(MtlSurfaceBinding *e,
                                uint8_t *vram_ptr_base,
                                PgraphMtlSurfaceDownloadCb cb,
                                void *cb_opaque)
{
    if (!atomic_load(&e->draw_dirty) || e->texture == NULL) {
        return;
    }
    /* Compute the byte range we're about to overwrite in VRAM. Mirror
     * the natural-row formula used by the upload helper so the caller's
     * QEMU dirty mark covers exactly the bytes we touched. */
    unsigned int bpp = e->is_color
        ? color_bytes_per_pixel(e->nv097_format)
        : 4;
    uint32_t guest_w = e->guest_width  ? e->guest_width  : e->width;
    uint32_t guest_h = e->guest_height ? e->guest_height : e->height;
    if (guest_w > e->width)  guest_w = e->width;
    if (guest_h > e->height) guest_h = e->height;
    size_t row_bytes = (size_t)guest_w * (bpp ? bpp : 4);
    size_t pitch     = e->pitch != 0 ? (size_t)e->pitch : row_bytes;
    size_t byte_size = pitch * guest_h;
    if (byte_size > e->size) {
        byte_size = e->size;
    }

    bool wrote = download_surface_to_vram(e, vram_ptr_base);

    if (wrote && cb != NULL && byte_size > 0) {
        /* The download path wrote real bytes to vram. Invoke the QEMU
         * dirty-mark callback so downstream consumers (display, NV2A
         * texture cache) see the new bytes. Callback is skipped when
         * the download was a no-op (depth, scaled-non-1×, format
         * unknown) — see download_surface_to_vram for the gates. */
        cb(cb_opaque, e->vram_addr, (uint32_t)byte_size);
    }
}

void pgraph_mtl_surface_download_if_dirty_at(uint32_t vram_addr,
                                             uint8_t *vram_ptr_base,
                                             PgraphMtlSurfaceDownloadCb cb,
                                             void *cb_opaque)
{
    if (!s_initialized || vram_ptr_base == NULL) {
        return;
    }
    MtlSurfaceBinding *e = cache_get_within(vram_addr);
    if (e == NULL) {
        return;
    }
    download_and_notify(e, vram_ptr_base, cb, cb_opaque);
}

void pgraph_mtl_surface_download_dirty_all(uint8_t *vram_ptr_base,
                                           PgraphMtlSurfaceDownloadCb cb,
                                           void *cb_opaque)
{
    if (!s_initialized || vram_ptr_base == NULL) {
        return;
    }
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        download_and_notify(e, vram_ptr_base, cb, cb_opaque);
    }
}

void pgraph_mtl_surface_download_in_range_if_dirty(uint32_t start,
                                                   uint32_t len,
                                                   uint8_t *vram_ptr_base,
                                                   PgraphMtlSurfaceDownloadCb cb,
                                                   void *cb_opaque)
{
    if (!s_initialized || vram_ptr_base == NULL || len == 0) {
        return;
    }
    uint32_t range_end = start + len;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        uint32_t ent_end = e->vram_addr + e->size;
        bool overlaps = !(e->vram_addr >= range_end || start >= ent_end);
        if (!overlaps) {
            continue;
        }
        download_and_notify(e, vram_ptr_base, cb, cb_opaque);
    }
}

uint64_t pgraph_mtl_surface_downloads(void)
{
    return atomic_load(&s_surface_downloads);
}

uint64_t pgraph_mtl_surface_download_bytes(void)
{
    return atomic_load(&s_surface_download_bytes);
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
