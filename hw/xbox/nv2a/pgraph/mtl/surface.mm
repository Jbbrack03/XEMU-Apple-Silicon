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

#include "qemu/osdep.h"
#include "surface.h"
#include "heap.h"

#include <stdatomic.h>
#include <mach/mach_time.h>
#include <pthread.h>
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

/* Task #14 residual fix (cross-queue clear→draw fence). Mirrors
 * s_draw_done_event in draw.mm: every clear command-buffer commit
 * signals this event with a monotonically increasing value, and
 * the draw queue's open_pass_ensure waits on the latest signaled
 * value before opening a new render pass. Required because clears
 * run on s_render_queue and draws run on s_draw_queue -- on Apple
 * Silicon those queues' commits execute in submission order WITHIN
 * a queue but NOT across queues without an explicit fence.
 *
 * Without this fence the stencil-ops XBE caught the race: per-cell
 * clears would commit on s_render_queue but the per-cell op_pass
 * draws on s_draw_queue could execute first, reading stale stencil
 * values from before the clear -- producing non-deterministic
 * 1-5/8 cells PASS on the §4.10 stencil-ops XBE (handoff 2026-05-20
 * task #14 residual). */
static id<MTLSharedEvent> s_clear_done_event = nil;
static _Atomic(uint64_t)  s_clear_done_value = 0;

extern "C" void pgraph_mtl_surface_get_clear_done_event_state(
    void **out_event, uint64_t *out_value)
{
    if (out_event) {
        *out_event = (__bridge void *)s_clear_done_event;
    }
    if (out_value) {
        *out_value = atomic_load(&s_clear_done_value);
    }
}

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

    /* Counts color-writing draws since the previous fallback publish.
     * Used by the experimental front-fb fallback to publish the target
     * that received actual scene work instead of whichever RT happened
     * to be bound last before flip_stall. Depth/stencil-only draws are
     * intentionally excluded because they can dominate early frame work
     * while leaving the color target at its clear color. */
    uint32_t frame_draw_count;

    /* 2026-05-19 surface-graph diag (Tool 1): monotonic seq snapshot of
     * the most recent color-writing draw that hit this binding. Mirrors
     * `last_use_seq` but only updates on color-write, so the graph
     * analyzer can rank "most-recently-rendered-to" candidates per flip
     * without depending on `frame_draw_count` which only resets on the
     * fallback publish path. Zero means "never color-drawn". */
    uint64_t last_color_draw_seq;

    /* 2026-05-20: depth-write freshness signal — bumped on every
     * `set_draw_dirty_depth()` invocation. Mirrors `last_color_draw_seq`
     * for depth attachments so the cross-sibling sync can identify the
     * most-recently-drawn depth sibling at a given vram_addr without
     * conflating with `last_use_seq` (which is bumped on every cache
     * lookup hit, including the bind that triggers the sync). Zero means
     * "never depth-drawn". */
    uint64_t last_depth_draw_seq;

    struct MtlSurfaceBinding *next;
} MtlSurfaceBinding;

/* Cap to avoid runaway. PGR2 and other late-era titles reuse the same
 * VRAM address for several distinct render-target shapes; the cache must
 * retain those siblings instead of destroying one shape when another is
 * rebound. 64 entries is still comfortably inside the 256 MiB RT heap for
 * the observed 1080p-class scale-2 working sets, while leaving enough
 * room for color/depth shape aliases and stale RTTs. */
static const unsigned int kMaxCacheEntries = 64;

static MtlSurfaceBinding *s_cache_head = NULL;
static unsigned int       s_cache_size = 0;
static uint64_t           s_use_seq    = 0;

/* Currently-bound pointers into the cache. May be NULL between
 * frames or while the cache is empty. */
static MtlSurfaceBinding *s_color_binding = NULL;
static MtlSurfaceBinding *s_depth_binding = NULL;
static MtlSurfaceBinding *s_fallback_draw_candidate = NULL;
static uint32_t           s_fallback_draw_candidate_count = 0;

/* M11: effective MSAA sample count (1 = off). */
static uint32_t s_msaa_sample_count = 1;

/* M11: counters. */
static _Atomic(uint64_t) s_msaa_resolve_count    = 0;
static _Atomic(uint64_t) s_msaa_resolve_us_total = 0;

/* 2026-05-20 (M5.12 / M17 followup): cross-sibling-sync counters.
 *
 * The renderer cache may hold multiple MtlSurfaceBinding entries at the
 * same vram_addr+pitch+nv097_format with different shapes (the "alias
 * sibling" pattern PGR2 hits late in the route: a 1278x442 sibling and a
 * 1280x480 sibling at vram_addr=0x3c84000 alternate per frame). Each
 * sibling owns a distinct MTLTexture, so a draw to sibling A is invisible
 * to a subsequent sample of sibling B even though the guest views both as
 * the same physical Xbox surface. The cross-sibling sync issues a
 * GPU-blit copy from the freshest sibling's textures (single-sample +
 * MSAA companion) into the target sibling's textures whenever a bind
 * transitions between same-VRAM siblings, so the next draws / samples
 * see the predecessor's content already in place.
 *
 * `s_sibling_sync_count` increments per sync event; `s_sibling_sync_skip`
 * increments when a sync is requested but no fresher sibling exists.
 * Gated by env flag `XEMU_METAL_RTT_SIBLING_SYNC` (default on). */
static _Atomic(uint64_t) s_sibling_sync_count = 0;
static _Atomic(uint64_t) s_sibling_sync_skip  = 0;

/* The "front" framebuffer texture published to the compositor. */
static _Atomic(void *) s_front_framebuffer_texture = nullptr;
static id<MTLTexture> s_front_snapshot_texture = nil;
static pthread_mutex_t s_front_framebuffer_lock = PTHREAD_MUTEX_INITIALIZER;

static id<MTLTexture> s_display_textures[3] = { nil, nil, nil };
static uint32_t        s_display_texture_index = 0;
static id<MTLRenderPipelineState> s_display_pipeline = nil;
static id<MTLSamplerState>        s_display_sampler = nil;

typedef struct MtlDisplayUniforms {
    float display_size[2];
    float line_offset;
    float _pad;
} MtlDisplayUniforms;

static NSString *const k_display_msl =
    @"#include <metal_stdlib>\n"
    @"using namespace metal;\n"
    @"struct VOut {\n"
    @"    float4 pos [[position]];\n"
    @"};\n"
    @"struct DisplayUniforms {\n"
    @"    float2 display_size;\n"
    @"    float line_offset;\n"
    @"    float _pad;\n"
    @"};\n"
    @"vertex VOut xemu_display_vs(uint vid [[vertex_id]]) {\n"
    @"    float2 p = float2((vid == 2) ? 3.0 : -1.0,\n"
    @"                      (vid == 0) ? -3.0 :  1.0);\n"
    @"    VOut o;\n"
    @"    o.pos = float4(p, 0.0, 1.0);\n"
    @"    return o;\n"
    @"}\n"
    @"fragment float4 xemu_display_fs(VOut in [[stage_in]],\n"
    @"                                texture2d<float> tex [[texture(0)]],\n"
    @"                                sampler samp [[sampler(0)]],\n"
    @"                                constant DisplayUniforms &u [[buffer(0)]]) {\n"
    @"    float2 tex_coord = in.pos.xy / u.display_size;\n"
    @"    float rel = u.display_size.y / float(tex.get_height()) / u.line_offset;\n"
    @"    tex_coord.y = rel * (1.0 - tex_coord.y);\n"
    @"    return tex.sample(samp, tex_coord);\n"
    @"}\n";

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
static _Atomic(uint64_t) s_surface_download_us_total = 0;

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
/* 2026-05-03 magenta-RT diagnostic: count cache entries destroyed +
 * recreated due to shape mismatch on a same-vram_addr rebind. */
static _Atomic(uint64_t) s_recreate_shape_mismatch = 0;

/* 2026-05-19 surface-graph diag (Tool 1). Captures the SOURCE binding
 * selected at publish time (not the texture the compositor sees). For
 * the display-compose path the published texture is a freshly
 * synthesized `dst`, so the dump cannot infer "which cache entry was
 * the publish source" by pointer match on s_front_framebuffer_texture.
 * These statics are written under s_front_framebuffer_lock alongside
 * the atomic_store of s_front_framebuffer_texture so the dump path
 * can re-read them under the same lock without tearing. */
static _Atomic(uint64_t) s_graph_dumps = 0;
static uint32_t          s_last_publish_source_vram_addr  = 0;
static void             *s_last_publish_source_texture    = NULL;
static void             *s_last_publish_published_texture = NULL;
static const char       *s_last_publish_kind              = NULL;
static const char       *s_last_publish_reason            = NULL;
static uint64_t          s_last_publish_seq               = 0;

static bool s_initialized = false;

/* Forward decl so publish paths can record source info under the
 * front-fb lock. */
static void record_publish_source_locked(MtlSurfaceBinding *e,
                                         void *published_texture,
                                         const char *kind,
                                         const char *reason);

/* ---------------------------------------------------------------- */

static bool present_snapshot_enabled(void)
{
    const char *e = getenv("XEMU_METAL_PRESENT_SNAPSHOT");
    return !(e && e[0] == '0');
}

/* Tool 1 (2026-05-19): record the SOURCE binding selected at publish
 * time so the graph dump can identify it independently of whichever
 * texture object the compositor ends up sampling (which may be a
 * composed `dst`, a snapshot, or the binding's own e->texture
 * depending on the publish path). Caller must hold
 * s_front_framebuffer_lock — fields are read under the same lock
 * from the dump path so the snapshot is internally consistent. */
static void record_publish_source_locked(MtlSurfaceBinding *e,
                                         void *published_texture,
                                         const char *kind,
                                         const char *reason)
{
    if (e == NULL) {
        return;
    }
    s_last_publish_source_vram_addr  = e->vram_addr;
    s_last_publish_source_texture    = e->texture;
    s_last_publish_published_texture = published_texture;
    s_last_publish_kind              = kind ? kind : "?";
    s_last_publish_reason            = reason ? reason : "?";
    s_last_publish_seq               = ++s_use_seq;
}

static void release_display_textures(void)
{
    for (unsigned int i = 0; i < 3; i++) {
        s_display_textures[i] = nil;
    }
    s_display_texture_index = 0;
}

static bool build_display_pipeline_if_needed(void)
{
    if (s_display_pipeline != nil && s_display_sampler != nil) {
        return true;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil) {
        return false;
    }

    NSError *err = nil;
    id<MTLLibrary> lib =
        [device newLibraryWithSource:k_display_msl options:nil error:&err];
    if (lib == nil) {
        fprintf(stderr,
                "xemu-metal: display-pipeline MSL compile failed: %s\n",
                [[err localizedDescription] UTF8String] ?: "(unknown)");
        return false;
    }

    id<MTLFunction> vs = [lib newFunctionWithName:@"xemu_display_vs"];
    id<MTLFunction> fs = [lib newFunctionWithName:@"xemu_display_fs"];

    MTLRenderPipelineDescriptor *desc =
        [[MTLRenderPipelineDescriptor alloc] init];
    desc.label = @"xemu.metal.display";
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    desc.colorAttachments[0].blendingEnabled = NO;

    s_display_pipeline =
        [device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (s_display_pipeline == nil) {
        fprintf(stderr,
                "xemu-metal: display-pipeline build failed: %s\n",
                [[err localizedDescription] UTF8String] ?: "(unknown)");
        return false;
    }

    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.label = @"xemu.metal.display_sampler";
    s_display_sampler = [device newSamplerStateWithDescriptor:sd];

    return s_display_sampler != nil;
}

static id<MTLTexture> ensure_display_texture(uint32_t width,
                                             uint32_t height)
{
    if (width == 0 || height == 0) {
        return nil;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil) {
        return nil;
    }

    s_display_texture_index = (s_display_texture_index + 1) % 3;
    id<MTLTexture> tex = s_display_textures[s_display_texture_index];
    if (tex != nil &&
        tex.width == width &&
        tex.height == height &&
        tex.pixelFormat == MTLPixelFormatBGRA8Unorm) {
        return tex;
    }

    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    tex = [device newTextureWithDescriptor:desc];
    if (tex != nil) {
        tex.label = @"xemu.metal.display";
    }
    s_display_textures[s_display_texture_index] = tex;
    return tex;
}

static bool front_snapshot_matches(id<MTLTexture> src)
{
    return s_front_snapshot_texture != nil &&
           s_front_snapshot_texture.width == src.width &&
           s_front_snapshot_texture.height == src.height &&
           s_front_snapshot_texture.pixelFormat == src.pixelFormat;
}

static bool ensure_front_snapshot_texture(id<MTLTexture> src)
{
    pthread_mutex_lock(&s_front_framebuffer_lock);
    if (front_snapshot_matches(src)) {
        pthread_mutex_unlock(&s_front_framebuffer_lock);
        return true;
    }

    void *old_snapshot = (__bridge void *)s_front_snapshot_texture;
    if (old_snapshot != NULL &&
        atomic_load(&s_front_framebuffer_texture) == old_snapshot) {
        atomic_store(&s_front_framebuffer_texture, (void *)NULL);
    }
    s_front_snapshot_texture = nil;

    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil || src == nil || src.width == 0 || src.height == 0) {
        pthread_mutex_unlock(&s_front_framebuffer_lock);
        return false;
    }

    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                                           width:src.width
                                                          height:src.height
                                                       mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageShaderRead;
    s_front_snapshot_texture = [device newTextureWithDescriptor:desc];
    if (s_front_snapshot_texture != nil) {
        s_front_snapshot_texture.label = @"xemu.metal.front_snapshot";
    }
    bool ok = s_front_snapshot_texture != nil;
    pthread_mutex_unlock(&s_front_framebuffer_lock);
    return ok;
}

static bool publish_front_texture(MtlSurfaceBinding *e, const char *reason)
{
    if (e == NULL || e->texture == NULL) {
        return false;
    }

    e->last_use_seq = ++s_use_seq;

    if (!present_snapshot_enabled()) {
        pthread_mutex_lock(&s_front_framebuffer_lock);
        void *prev = atomic_load(&s_front_framebuffer_texture);
        if (prev == e->texture) {
            pthread_mutex_unlock(&s_front_framebuffer_lock);
            return true;
        }
        atomic_store(&s_front_framebuffer_texture, e->texture);
        record_publish_source_locked(e, e->texture,
                                     "front-texture", reason);
        pthread_mutex_unlock(&s_front_framebuffer_lock);
        atomic_fetch_add(&s_front_fb_publishes, 1);
        fprintf(stderr,
                "xemu-perf: metal_front_fb_publish vram_addr=0x%x "
                "width=%u height=%u format=%u reason=%s\n",
                (unsigned)e->vram_addr, e->width, e->height,
                e->nv097_format, reason ? reason : "?");
        return true;
    }

    pgraph_mtl_draw_flush_open_pass();

    bool copied = false;
    @autoreleasepool {
        id<MTLTexture> src = (__bridge id<MTLTexture>)e->texture;
        if (!ensure_front_snapshot_texture(src)) {
            return false;
        }

        void     *event_handle = NULL;
        uint64_t  event_value  = 0;
        pgraph_mtl_draw_get_done_event_state(&event_handle, &event_value);

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.front_snapshot_copy";
        if (event_handle != NULL && event_value > 0) {
            id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
            [cmd encodeWaitForEvent:ev value:event_value];
        }

        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        blit.label = @"xemu.metal.front_snapshot_blit";
        [blit copyFromTexture:src
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(src.width, src.height, 1)
                    toTexture:s_front_snapshot_texture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        copied = (cmd.status == MTLCommandBufferStatusCompleted);
    }

    if (!copied) {
        return false;
    }

    pthread_mutex_lock(&s_front_framebuffer_lock);
    atomic_store(&s_front_framebuffer_texture,
                 (__bridge void *)s_front_snapshot_texture);
    record_publish_source_locked(e,
                                 (__bridge void *)s_front_snapshot_texture,
                                 "front-snapshot", reason);
    pthread_mutex_unlock(&s_front_framebuffer_lock);
    atomic_fetch_add(&s_front_fb_publishes, 1);
    if (getenv("XEMU_METAL_DIAG_PUBLISH")) {
        fprintf(stderr,
                "xemu-perf: metal_front_fb_publish vram_addr=0x%x "
                "width=%u height=%u format=%u reason=%s snapshot=1\n",
                (unsigned)e->vram_addr, e->width, e->height,
                e->nv097_format, reason ? reason : "?");
    }
    return true;
}

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

static void fallback_draw_reset(void)
{
    s_fallback_draw_candidate = NULL;
    s_fallback_draw_candidate_count = 0;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        e->frame_draw_count = 0;
    }
}

static void binding_destroy(MtlSurfaceBinding *b)
{
    if (b == s_fallback_draw_candidate) {
        s_fallback_draw_candidate = NULL;
        s_fallback_draw_candidate_count = 0;
    }
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
    if (s_color_binding != NULL &&
        s_color_binding->is_color &&
        s_color_binding->vram_addr == vram_addr) {
        return s_color_binding;
    }
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->vram_addr == vram_addr && e->is_color) {
            return e;
        }
    }
    return NULL;
}

static MtlSurfaceBinding *cache_get_at_depth(uint32_t vram_addr)
{
    if (s_depth_binding != NULL &&
        !s_depth_binding->is_color &&
        s_depth_binding->vram_addr == vram_addr) {
        return s_depth_binding;
    }
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->vram_addr == vram_addr && !e->is_color) {
            return e;
        }
    }
    return NULL;
}

/* Forward decl — implementation lives near the M5.10 download API
 * because it shares the s_render_queue + draw-done fence machinery. */
static void sync_color_siblings_into(MtlSurfaceBinding *target);
static void sync_depth_siblings_into(MtlSurfaceBinding *target);

static bool binding_shape_compatible(MtlSurfaceBinding *e, bool is_color,
                                     uint32_t vram_addr,
                                     uint32_t width, uint32_t height,
                                     uint32_t pitch, uint32_t format)
{
    if (e == NULL || e->is_color != is_color ||
        e->vram_addr != vram_addr ||
        e->pitch != pitch ||
        e->nv097_format != format ||
        e->width < width || e->height < height) {
        return false;
    }
    return (e->width - width) <= 4 && (e->height - height) <= 4;
}

static MtlSurfaceBinding *
cache_get_shape(bool is_color, uint32_t vram_addr,
                uint32_t width, uint32_t height,
                uint32_t pitch, uint32_t format)
{
    MtlSurfaceBinding *best = NULL;
    uint64_t best_extra_area = UINT64_MAX;

    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (!binding_shape_compatible(e, is_color, vram_addr,
                                      width, height, pitch, format)) {
            continue;
        }
        if (e->width == width && e->height == height) {
            return e;
        }
        uint64_t area = (uint64_t)e->width * e->height;
        uint64_t want = (uint64_t)width * height;
        uint64_t extra = area > want ? area - want : 0;
        if (best == NULL || extra < best_extra_area ||
            (extra == best_extra_area && e->last_use_seq > best->last_use_seq)) {
            best = e;
            best_extra_area = extra;
        }
    }
    return best;
}

static MtlSurfaceBinding *
cache_get_color_for_guest(uint32_t vram_addr,
                          uint32_t guest_width, uint32_t guest_height,
                          uint32_t pitch)
{
    MtlSurfaceBinding *best = NULL;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (!e->is_color || e->vram_addr != vram_addr) {
            continue;
        }
        uint32_t ew = e->guest_width ? e->guest_width : e->width;
        uint32_t eh = e->guest_height ? e->guest_height : e->height;
        if (ew != guest_width || eh != guest_height) {
            continue;
        }
        if (pitch != 0 && e->pitch != pitch) {
            continue;
        }
        if (best == NULL || e->last_use_seq > best->last_use_seq) {
            best = e;
        }
    }
    return best;
}

static MtlSurfaceBinding *cache_get_by_texture(void *texture)
{
    if (texture == NULL) {
        return NULL;
    }
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->is_color && e->texture == texture) {
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
    fallback_draw_reset();
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
    if (guest_w > b->width)  guest_w = b->width;
    if (guest_h > b->height) guest_h = b->height;
    /* Fill the whole host texture. Earlier M5.9 upload code copied only
     * the 1× guest rectangle into the upper-left of a scaled render
     * target; the present path samples the full texture, so the untouched
     * region stayed heap-default magenta. Expanding here mirrors how the
     * draw path renders into scaled surfaces and keeps CPU-updated front
     * buffers presentable at surface_scale > 1. */
    uint32_t dst_w = b->width;
    uint32_t dst_h = b->height;
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
        size_t guest_row_bytes = (size_t)guest_w * bpp;
        size_t src_pitch =
            b->pitch != 0 ? (size_t)b->pitch : guest_row_bytes;
        uint8_t *dst = (uint8_t *)stage.contents;
        if (dst_w == guest_w && dst_h == guest_h &&
            src_pitch == row_bytes) {
            memcpy(dst, src, copy_size);
        } else if (dst_w == guest_w && dst_h == guest_h) {
            for (uint32_t y = 0; y < dst_h; y++) {
                memcpy(dst + (size_t)y * row_bytes,
                       src + (size_t)y * src_pitch,
                       row_bytes);
            }
        } else if (dst_w % guest_w == 0 && dst_h % guest_h == 0) {
            uint32_t scale_x = dst_w / guest_w;
            uint32_t scale_y = dst_h / guest_h;
            for (uint32_t sy = 0; sy < guest_h; sy++) {
                const uint8_t *src_row = src + (size_t)sy * src_pitch;
                uint8_t *first_dst_row =
                    dst + (size_t)sy * scale_y * row_bytes;
                for (uint32_t sx = 0; sx < guest_w; sx++) {
                    const uint8_t *src_px = src_row + (size_t)sx * bpp;
                    uint8_t *dst_px =
                        first_dst_row + (size_t)sx * scale_x * bpp;
                    for (uint32_t rx = 0; rx < scale_x; rx++) {
                        memcpy(dst_px + (size_t)rx * bpp, src_px, bpp);
                    }
                }
                for (uint32_t ry = 1; ry < scale_y; ry++) {
                    memcpy(first_dst_row + (size_t)ry * row_bytes,
                           first_dst_row,
                           row_bytes);
                }
            }
        } else {
            for (uint32_t y = 0; y < dst_h; y++) {
                uint32_t sy = (uint32_t)(((uint64_t)y * guest_h) / dst_h);
                const uint8_t *src_row = src + (size_t)sy * src_pitch;
                uint8_t *dst_row = dst + (size_t)y * row_bytes;
                for (uint32_t x = 0; x < dst_w; x++) {
                    uint32_t sx =
                        (uint32_t)(((uint64_t)x * guest_w) / dst_w);
                    memcpy(dst_row + (size_t)x * bpp,
                           src_row + (size_t)sx * bpp,
                           bpp);
                }
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
    int64_t download_start_us = mtl_now_us();
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
    if (guest_w == 0 || guest_h == 0 || guest_w > b->width ||
        guest_h > b->height) {
        return false;
    }

    uint32_t scale_x = b->width / guest_w;
    uint32_t scale_y = b->height / guest_h;
    bool scaled = (b->width != guest_w || b->height != guest_h);
    if (scaled && (scale_x == 0 || scale_y == 0 ||
                   scale_x * guest_w != b->width ||
                   scale_y * guest_h != b->height)) {
        return false;
    }

    uint32_t src_w = scaled ? b->width : guest_w;
    uint32_t src_h = scaled ? b->height : guest_h;
    size_t src_row_bytes = (size_t)src_w * bpp;
    size_t src_copy_size = src_row_bytes * src_h;
    size_t row_bytes = (size_t)guest_w * bpp;
    size_t copy_size = row_bytes * guest_h;
    if (copy_size == 0) {
        return false;
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
            newBufferWithLength:src_copy_size
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
       destinationBytesPerRow:src_row_bytes
     destinationBytesPerImage:src_copy_size];
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        /* Copy the staging buffer into guest VRAM at the source pitch.
         * If pitch != natural, mirror the pattern in the upload helper. */
        uint8_t       *dst       = vram_ptr_base + b->vram_addr;
        size_t         dst_pitch = b->pitch != 0 ? (size_t)b->pitch : row_bytes;
        const uint8_t *src       = (const uint8_t *)stage.contents;
        if (!scaled && dst_pitch == row_bytes) {
            memcpy(dst, src, copy_size);
        } else if (scaled) {
            for (uint32_t y = 0; y < guest_h; y++) {
                const uint8_t *src_row = src + (size_t)y * scale_y * src_row_bytes;
                uint8_t *dst_row = dst + (size_t)y * dst_pitch;
                for (uint32_t x = 0; x < guest_w; x++) {
                    memcpy(dst_row + (size_t)x * bpp,
                           src_row + (size_t)x * scale_x * bpp,
                           bpp);
                }
            }
        } else {
            for (uint32_t y = 0; y < guest_h; y++) {
                memcpy(dst + y * dst_pitch,
                       src + y * row_bytes,
                       row_bytes);
            }
        }
        atomic_fetch_add(&s_surface_downloads, 1);
        atomic_fetch_add(&s_surface_download_bytes, (uint64_t)copy_size);
        mtl_add_elapsed_us(&s_surface_download_us_total, download_start_us);
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

    /* Task #14 residual fix: cross-queue clear→draw fence event. */
    s_clear_done_event = [device newSharedEvent];
    if (s_clear_done_event != nil) {
        s_clear_done_event.label = @"xemu.metal.clear_done";
    }

    s_cache_head = NULL;
    s_cache_size = 0;
    s_color_binding = NULL;
    s_depth_binding = NULL;
    s_fallback_draw_candidate = NULL;
    s_fallback_draw_candidate_count = 0;
    s_use_seq = 0;
    s_front_snapshot_texture = nil;
    release_display_textures();
    s_display_pipeline = nil;
    s_display_sampler = nil;
    atomic_store(&s_front_framebuffer_texture, (void *)NULL);
    atomic_store(&s_clear_count, (uint64_t)0);
    atomic_store(&s_front_fb_publishes, (uint64_t)0);
    atomic_store(&s_image_blits, (uint64_t)0);
    atomic_store(&s_vram_dirty_hits, (uint64_t)0);
    atomic_store(&s_vram_uploads, (uint64_t)0);
    atomic_store(&s_vram_upload_bytes, (uint64_t)0);
    atomic_store(&s_surface_downloads, (uint64_t)0);
    atomic_store(&s_surface_download_bytes, (uint64_t)0);
    atomic_store(&s_surface_download_us_total, (uint64_t)0);

    s_initialized = true;
    return true;
}

void pgraph_mtl_surface_finalize(void)
{
    if (!s_initialized) {
        return;
    }

    pthread_mutex_lock(&s_front_framebuffer_lock);
    atomic_store(&s_front_framebuffer_texture, (void *)NULL);
    s_front_snapshot_texture = nil;
    pthread_mutex_unlock(&s_front_framebuffer_lock);
    release_display_textures();
    s_display_pipeline = nil;
    s_display_sampler = nil;
    cache_drop_all();
    s_render_queue = nil;
    s_initialized = false;
}

void pgraph_mtl_surface_cache_flush(void)
{
    if (!s_initialized) {
        return;
    }
    pthread_mutex_lock(&s_front_framebuffer_lock);
    atomic_store(&s_front_framebuffer_texture, (void *)NULL);
    s_front_snapshot_texture = nil;
    pthread_mutex_unlock(&s_front_framebuffer_lock);
    release_display_textures();
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

    MtlSurfaceBinding *e = cache_get_shape(true, vram_addr, width, height,
                                           pitch, nv097_color_format);
    if (e != NULL) {
        /* Cache hit. Match GL/Vulkan's non-strict compatibility rule only
         * for barely-larger same-format shapes. Substantially different
         * shapes at the same VRAM address remain separate siblings so
         * render-to-texture sampling and fallback publish do not alias the
         * wrong texture. */
        if (e->width == width && e->height == height) {
            if (guest_width  != 0) e->guest_width  = guest_width;
            if (guest_height != 0) e->guest_height = guest_height;
            if (size > e->size) e->size = size;
        }
        e->last_use_seq = ++s_use_seq;
        return e;
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
    e->size             = size > 0 ? size :
                          ((uint32_t)pitch *
                           (guest_height ? guest_height : height));
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

    MtlSurfaceBinding *e = cache_get_shape(false, vram_addr, width, height,
                                           pitch, nv097_zeta_format);
    if (e != NULL) {
        if (e->width == width && e->height == height) {
            if (guest_width  != 0) e->guest_width  = guest_width;
            if (guest_height != 0) e->guest_height = guest_height;
            if (size > e->size) e->size = size;
        }
        e->last_use_seq = ++s_use_seq;
        return e;
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
    e->size             = size > 0 ? size :
                          ((uint32_t)pitch *
                           (guest_height ? guest_height : height));
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
    /* 2026-05-20: if another same-VRAM same-pitch same-format color
     * sibling has fresher content (different clip-rect alias), copy
     * its texture contents into this binding before the next draws
     * land. Closes the PGR2 late-route RTT divergence at 0x3c84000
     * caused by separate MTLTextures per clip-rect sibling holding
     * disjoint subsets of the same physical Xbox surface. */
    sync_color_siblings_into(e);
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
    /* 2026-05-20: cross-sibling sync — depth-attachment companion of
     * the color-side fix. Uses `last_depth_draw_seq` (bumped by
     * set_draw_dirty_depth) instead of `last_use_seq` because the
     * latter is already bumped by the cache_find_or_create_depth call
     * above. */
    sync_depth_siblings_into(e);
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
    /* 2026-05-20: cross-sibling sync — see bind_color() for the
     * full rationale. The `_ex` path is the production callsite from
     * mtl_bind_current_surfaces; non-ex retained for the legacy
     * single-slot wrappers. */
    sync_color_siblings_into(e);
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
    /* 2026-05-20: cross-sibling sync — depth-attachment companion. */
    sync_depth_siblings_into(e);
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

bool pgraph_mtl_surface_get_color_surface_info_at(uint32_t vram_addr,
                                                  void **out_texture,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  uint32_t *out_guest_width,
                                                  uint32_t *out_guest_height,
                                                  uint32_t *out_pitch,
                                                  uint32_t *out_format)
{
    if (!s_initialized) {
        return false;
    }
    MtlSurfaceBinding *e = cache_get_at_color(vram_addr);
    if (e == NULL || e->texture == NULL) {
        return false;
    }
    if (out_texture) {
        *out_texture = e->texture;
    }
    if (out_width) {
        *out_width = e->width;
    }
    if (out_height) {
        *out_height = e->height;
    }
    if (out_guest_width) {
        *out_guest_width = e->guest_width ? e->guest_width : e->width;
    }
    if (out_guest_height) {
        *out_guest_height = e->guest_height ? e->guest_height : e->height;
    }
    if (out_pitch) {
        *out_pitch = e->pitch;
    }
    if (out_format) {
        *out_format = e->nv097_format;
    }
    return true;
}

bool pgraph_mtl_surface_get_color_surface_info_for(uint32_t vram_addr,
                                                   uint32_t guest_width,
                                                   uint32_t guest_height,
                                                   uint32_t pitch,
                                                   void **out_texture,
                                                   uint32_t *out_width,
                                                   uint32_t *out_height,
                                                   uint32_t *out_guest_width,
                                                   uint32_t *out_guest_height,
                                                   uint32_t *out_pitch,
                                                   uint32_t *out_format)
{
    if (!s_initialized || guest_width == 0 || guest_height == 0) {
        return false;
    }
    MtlSurfaceBinding *e =
        cache_get_color_for_guest(vram_addr, guest_width, guest_height,
                                  pitch);
    if (e == NULL || e->texture == NULL) {
        return false;
    }
    if (out_texture) {
        *out_texture = e->texture;
    }
    if (out_width) {
        *out_width = e->width;
    }
    if (out_height) {
        *out_height = e->height;
    }
    if (out_guest_width) {
        *out_guest_width = e->guest_width ? e->guest_width : e->width;
    }
    if (out_guest_height) {
        *out_guest_height = e->guest_height ? e->guest_height : e->height;
    }
    if (out_pitch) {
        *out_pitch = e->pitch;
    }
    if (out_format) {
        *out_format = e->nv097_format;
    }
    e->last_use_seq = ++s_use_seq;
    return true;
}

bool pgraph_mtl_surface_has_other_color_shape(uint32_t vram_addr,
                                              uint32_t guest_width,
                                              uint32_t guest_height,
                                              uint32_t pitch)
{
    if (!s_initialized || guest_width == 0 || guest_height == 0) {
        return false;
    }

    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (!e->is_color || e->vram_addr != vram_addr) {
            continue;
        }

        uint32_t ew = e->guest_width ? e->guest_width : e->width;
        uint32_t eh = e->guest_height ? e->guest_height : e->height;
        bool same_shape = (ew == guest_width && eh == guest_height &&
                           (pitch == 0 || e->pitch == pitch));
        if (!same_shape) {
            return true;
        }
    }

    return false;
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

    return publish_front_texture(e, reason);
}

/* T2 (2026-05-12 evening): lightweight per-host-refresh publish. */
bool pgraph_mtl_surface_publish_front_fb_pointer_only(uint32_t vram_addr,
                                                      const char *reason)
{
    if (!s_initialized) {
        return false;
    }
    MtlSurfaceBinding *e = cache_get_within(vram_addr);
    if (e == NULL || e->texture == NULL) {
        return false;
    }

    /* Bump last_use_seq so LRU eviction doesn't reclaim the surface that
     * the compositor is about to sample. Matches the bookkeeping inside
     * publish_front_texture's non-snapshot path. */
    e->last_use_seq = ++s_use_seq;

    pthread_mutex_lock(&s_front_framebuffer_lock);
    void *prev = atomic_load(&s_front_framebuffer_texture);
    if (prev == e->texture) {
        pthread_mutex_unlock(&s_front_framebuffer_lock);
        return true;
    }
    atomic_store(&s_front_framebuffer_texture, e->texture);
    record_publish_source_locked(e, e->texture, "pointer-only", reason);
    pthread_mutex_unlock(&s_front_framebuffer_lock);
    atomic_fetch_add(&s_front_fb_publishes, 1);
    if (getenv("XEMU_METAL_DIAG_PUBLISH")) {
        fprintf(stderr,
                "xemu-perf: metal_front_fb_publish vram_addr=0x%x "
                "width=%u height=%u format=%u reason=%s pointer_only=1\n",
                (unsigned)e->vram_addr, e->width, e->height,
                e->nv097_format, reason ? reason : "?");
    }
    return true;
}

static bool publish_display_binding_front_fb(MtlSurfaceBinding *e,
                                             uint32_t display_width,
                                             uint32_t display_height,
                                             uint32_t vga_line_offset,
                                             const char *reason)
{
    if (!s_initialized || e == NULL || e->texture == NULL || !e->is_color ||
        display_width == 0 || display_height == 0) {
        return false;
    }

    if (!build_display_pipeline_if_needed()) {
        return publish_front_texture(e, reason);
    }

    id<MTLTexture> dst = ensure_display_texture(display_width, display_height);
    if (dst == nil) {
        return publish_front_texture(e, reason);
    }

    float line_offset = 1.0f;
    if (vga_line_offset != 0) {
        line_offset = (float)e->pitch / (float)vga_line_offset;
        if (line_offset <= 0.0f) {
            line_offset = 1.0f;
        }
    }

    pgraph_mtl_draw_flush_open_pass();
    void     *event_handle = NULL;
    uint64_t  event_value  = 0;
    pgraph_mtl_draw_get_done_event_state(&event_handle, &event_value);

    bool rendered = false;
    @autoreleasepool {
        MTLRenderPassDescriptor *desc =
            [MTLRenderPassDescriptor renderPassDescriptor];
        desc.colorAttachments[0].texture = dst;
        desc.colorAttachments[0].loadAction = MTLLoadActionClear;
        desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        desc.colorAttachments[0].clearColor =
            MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.display_compose";
        if (event_handle != NULL && event_value > 0) {
            id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
            [cmd encodeWaitForEvent:ev value:event_value];
        }

        id<MTLRenderCommandEncoder> enc =
            [cmd renderCommandEncoderWithDescriptor:desc];
        enc.label = @"xemu.metal.display_compose_enc";
        MTLViewport vp = {
            0.0, 0.0,
            (double)display_width, (double)display_height,
            0.0, 1.0
        };
        [enc setViewport:vp];
        MTLScissorRect sc = { 0, 0, display_width, display_height };
        [enc setScissorRect:sc];
        [enc setRenderPipelineState:s_display_pipeline];
        [enc setFragmentTexture:(__bridge id<MTLTexture>)e->texture atIndex:0];
        [enc setFragmentSamplerState:s_display_sampler atIndex:0];
        MtlDisplayUniforms u = {
            { (float)display_width, (float)display_height },
            line_offset,
            0.0f
        };
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        rendered = (cmd.status == MTLCommandBufferStatusCompleted);
    }

    if (!rendered) {
        return false;
    }

    e->last_use_seq = ++s_use_seq;
    pthread_mutex_lock(&s_front_framebuffer_lock);
    atomic_store(&s_front_framebuffer_texture, (__bridge void *)dst);
    record_publish_source_locked(e, (__bridge void *)dst,
                                 "display-compose", reason);
    pthread_mutex_unlock(&s_front_framebuffer_lock);
    atomic_fetch_add(&s_front_fb_publishes, 1);
    if (getenv("XEMU_METAL_DIAG_PUBLISH")) {
        fprintf(stderr,
                "xemu-perf: metal_front_fb_publish vram_addr=0x%x "
                "width=%u height=%u source_width=%u source_height=%u "
                "format=%u line_offset=%.3f reason=%s display=1\n",
                (unsigned)e->vram_addr, display_width, display_height,
                e->width, e->height, e->nv097_format, line_offset,
                reason ? reason : "?");
    }
    return true;
}

bool pgraph_mtl_surface_publish_display_front_fb(uint32_t vram_addr,
                                                 uint32_t display_width,
                                                 uint32_t display_height,
                                                 uint32_t vga_line_offset,
                                                 const char *reason)
{
    if (!s_initialized) {
        return false;
    }
    MtlSurfaceBinding *e = cache_get_within(vram_addr);
    return publish_display_binding_front_fb(e, display_width, display_height,
                                            vga_line_offset, reason);
}

void pgraph_mtl_surface_note_color_draw(void *texture, bool color_write)
{
    if (!s_initialized || texture == NULL || !color_write) {
        return;
    }
    MtlSurfaceBinding *e = cache_get_by_texture(texture);
    if (e == NULL) {
        return;
    }

    uint32_t n = ++e->frame_draw_count;
    /* Tool 1 (2026-05-19): timestamp the most-recent color-write on this
     * binding using the shared monotonic seq counter. The graph analyzer
     * uses this to rank "most-recently-rendered-to" candidates per flip
     * — frame_draw_count alone is cumulative and only resets on the
     * fallback-publish path (Codex review 2026-05-19, finding #2). */
    e->last_color_draw_seq = ++s_use_seq;
    if (s_fallback_draw_candidate == NULL ||
        n > s_fallback_draw_candidate_count ||
        (n == s_fallback_draw_candidate_count && e == s_color_binding)) {
        s_fallback_draw_candidate = e;
        s_fallback_draw_candidate_count = n;
    }
}

/* M5.10 experimental fallback (2026-05-03): publish the dominant
 * per-frame color draw target as the front-fb, regardless of CRTC
 * address. Use case: titles like PGR2 where the CRTC-pointed surface
 * gets only a few draws while the actual rendered scene goes to a back
 * buffer at a different VRAM address. The fallback now tracks actual
 * draw destinations instead of using s_color_binding, since the last
 * bound target before flip_stall may be a black front/CRTC surface.
 *
 * This is NOT correctness-faithful (the back buffer may have a
 * different aspect ratio than the front, and if the title uses
 * legitimate front/back surfaces they will be wrong), so the path is
 * gated behind XEMU_METAL_FRONT_FB_FALLBACK=1. The compositor gets
 * whatever was last bound for drawing. Aspect mismatch is preserved
 * (the present pipeline scales the texture to drawable extent
 * regardless of input dims).
 *
 * Returns true if a publish landed. Bumps METAL_FRONT_FB_PUBLISHES with
 * reason="fallback-dominant-draw". */
bool pgraph_mtl_surface_publish_latest_draw_fallback(uint32_t display_width,
                                                     uint32_t display_height,
                                                     uint32_t crtc_vram_addr)
{
    if (!s_initialized) {
        return false;
    }
    MtlSurfaceBinding *e = s_fallback_draw_candidate;
    const char *reason = "fallback-dominant-draw";
    if (e == NULL || e->texture == NULL) {
        e = s_color_binding;
        reason = "fallback-current-binding";
    }
    if (e == NULL || e->texture == NULL) {
        fallback_draw_reset();
        return false;
    }
    uint32_t selected_count = s_fallback_draw_candidate_count;
    if (getenv("XEMU_METAL_DIAG_PUBLISH")) {
        fprintf(stderr,
                "xemu-perf: metal_front_fb_fallback_candidate "
                "vram_addr=0x%x width=%u height=%u format=%u "
                "color_draws=%u reason=%s%s\n",
                (unsigned)e->vram_addr, e->width, e->height,
                e->nv097_format, (unsigned)selected_count, reason,
                (e == s_color_binding && s_fallback_draw_candidate == NULL)
                    ? " source=current-binding" : "");
    }
    uint32_t publish_width = display_width ? display_width : e->width;
    uint32_t publish_height = display_height ? display_height : e->height;
    bool ok = publish_display_binding_front_fb(e, publish_width,
                                               publish_height, e->pitch,
                                               reason);
    fallback_draw_reset();
    return ok;
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
                              bool write_depth, float depth,
                              bool write_stencil, int stencil)
{
    if (!s_initialized) {
        return;
    }

    /* Need a depth binding only when at least one zeta aspect is being
     * cleared. The two aspects are independently gated below: if only
     * depth is asked for, we DO NOT attach the stencil aspect to the
     * render pass (Metal would otherwise clear stencil too) and vice
     * versa. Mirrors gl/draw.c's per-bit glClear semantics. Per Codex
     * 2026-05-20 finding #1 -- the previous "write_zeta = Z|S"
     * collapse always cleared both aspects when only one was requested. */
    bool have_color_target = write_color &&
                             s_color_binding != NULL &&
                             s_color_binding->texture != NULL;
    bool have_depth_target = (write_depth || write_stencil) &&
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
                "rgba=(%.3f,%.3f,%.3f,%.3f) write_depth=%d write_stencil=%d\n",
                (unsigned)s_color_binding->vram_addr,
                rgba[0], rgba[1], rgba[2], rgba[3],
                write_depth ? 1 : 0, write_stencil ? 1 : 0);
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
                    MTLStoreActionStoreAndMultisampleResolve;
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
            /* Per-aspect attach (Codex 2026-05-20 finding #1): only
             * configure the depth attachment when NV097_CLEAR_SURFACE_Z
             * was requested, and only configure the stencil attachment
             * when NV097_CLEAR_SURFACE_STENCIL was requested. Mirrors
             * gl/draw.c::pgraph_gl_clear_surface, which gates
             * glClearDepth/glClearStencil on the corresponding bits
             * independently. Unconfigured aspects keep their texture
             * contents (Metal preserves the unattached aspect of a
             * combined depth+stencil texture across the render pass). */
            id<MTLTexture> tex =
                (__bridge id<MTLTexture>)s_depth_binding->texture;
            id<MTLTexture> ms = have_msaa_depth ?
                (__bridge id<MTLTexture>)s_depth_binding->msaa_texture :
                nil;

            if (write_depth) {
                desc.depthAttachment.texture     = ms ? ms : tex;
                desc.depthAttachment.loadAction  = MTLLoadActionClear;
                desc.depthAttachment.storeAction = MTLStoreActionStore;
                desc.depthAttachment.clearDepth  = depth;
            }

            MTLPixelFormat fmt =
                (MTLPixelFormat)s_depth_binding->mtl_pixel_format;
            bool format_has_stencil =
                (fmt == MTLPixelFormatDepth24Unorm_Stencil8 ||
                 fmt == MTLPixelFormatDepth32Float_Stencil8 ||
                 fmt == MTLPixelFormatStencil8);
            if (write_stencil && format_has_stencil) {
                /* Honor the decoded stencil clear value from
                 * pgraph_get_clear_depth_stencil_value. Hardcoding 0
                 * here broke the §4.10 stencil-ops XBE on Metal (task
                 * #14): the XBE clears stencil to 0x80 per cell before
                 * each op pass, but Metal's clear ignored the value so
                 * every cell's op started from stencil=0. Mirrors
                 * gl/draw.c's glClearStencil(gl_clear_stencil) call. */
                desc.stencilAttachment.texture     = ms ? ms : tex;
                desc.stencilAttachment.loadAction  = MTLLoadActionClear;
                desc.stencilAttachment.storeAction = MTLStoreActionStore;
                desc.stencilAttachment.clearStencil = (uint32_t)stencil;
            }
        }

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.clear";

        /* Task #14 residual fix: clears run on s_render_queue while
         * draws run on s_draw_queue. Without a cross-queue fence the
         * GPU can execute this clear BEFORE prior draws on s_draw_queue
         * complete -- which would clobber the depth/stencil values
         * those prior draws wrote and produce non-deterministic
         * stencil-test results. Wait on s_draw_done_event before the
         * render-encoder configures the attachments. Mirrors the same
         * fence pattern used by surface downloads / blits in this
         * file (encodeWaitForEvent against the draw queue's latest
         * signal value via pgraph_mtl_draw_get_done_event_state). */
        void *event_handle = NULL;
        uint64_t event_value = 0;
        pgraph_mtl_draw_get_done_event_state(&event_handle, &event_value);
        if (event_handle != NULL && event_value > 0) {
            id<MTLSharedEvent> ev =
                (__bridge id<MTLSharedEvent>)event_handle;
            [cmd encodeWaitForEvent:ev value:event_value];
        }

        id<MTLRenderCommandEncoder> enc =
            [cmd renderCommandEncoderWithDescriptor:desc];
        enc.label = @"xemu.metal.clear_enc";
        [enc endEncoding];

        /* Task #14 residual fix: signal the clear-done event so the
         * draw queue's next open_pass_ensure waits for this clear's
         * load_action_Clear to complete before its loadAction=Load
         * picks up the (now-cleared) depth/stencil contents. Mirrors
         * the s_draw_done_event signal in draw.mm
         * open_pass_close_locked. */
        if (s_clear_done_event != nil) {
            uint64_t v = atomic_fetch_add(&s_clear_done_value, 1) + 1;
            [cmd encodeSignalEvent:s_clear_done_event value:v];
        }

        [cmd commit];

        /* Task #14 residual fix: synchronously wait for the clear
         * to complete on the GPU before returning. The encoded
         * encodeWaitForEvent fence above is in place for future
         * follow-up, but in practice it does NOT serialize the
         * clear-before-draw ordering on Apple Silicon as expected
         * (validated by stencil-ops XBE: with only the fence,
         * 1-3/8 cells PASS deterministic; with [cmd waitUntilCompleted]
         * added here, 8/8 cells PASS). The perf cost is bounded:
         * retail games issue ~2-4 clears per frame so the host-side
         * CPU stall is sub-ms per frame. Opt-out via the env var
         * XEMU_METAL_NO_CLEAR_SYNC for diagnostic/perf-comparison.
         * Counterpart fences (signal s_clear_done_event +
         * mtl_draw_wait_clear_fence in draw.mm) remain in place
         * as a soft guarantee in case the wait-for-completion is
         * later removed. */
        if (!getenv("XEMU_METAL_NO_CLEAR_SYNC")) {
            [cmd waitUntilCompleted];
        }
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
    if (!s_initialized) {
        return NULL;
    }

    void *retained = NULL;
    pthread_mutex_lock(&s_front_framebuffer_lock);
    void *raw = atomic_load(&s_front_framebuffer_texture);
    if (raw != NULL) {
        id<MTLTexture> tex = (__bridge id<MTLTexture>)raw;
        retained = (__bridge_retained void *)tex;
    }
    pthread_mutex_unlock(&s_front_framebuffer_lock);
    return retained;
}

void pgraph_mtl_release_framebuffer_metal_texture(void *texture)
{
    if (texture == NULL) {
        return;
    }
    id<MTLTexture> tex = (__bridge_transfer id<MTLTexture>)texture;
    (void)tex;
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

uint64_t pgraph_mtl_surface_graph_dumps(void)
{
    return atomic_load(&s_graph_dumps);
}

/* Tool 1 (2026-05-19): structured per-flip dump of every cache binding.
 *
 * Emits one JSONL "flip" header line followed by one JSONL "binding"
 * line per cache entry. Caller MUST hold `pg->lock` so the singly-
 * linked s_cache_head list does not mutate while we walk it; the
 * flip_stall hook in renderer.c already runs under that lock per
 * T2 (commit 3ae76a327c). We additionally take the front-fb mutex
 * briefly to snapshot s_last_publish_* fields so the publish-source
 * info is internally consistent with the surface state. */
void pgraph_mtl_surface_dump_graph_jsonl(FILE *out,
                                         const char *reason,
                                         uint64_t flip_ordinal)
{
    if (!s_initialized || out == NULL) {
        return;
    }

    /* Snapshot publish state under the front-fb lock. */
    uint32_t    last_src_vram_addr  = 0;
    void       *last_src_texture    = NULL;
    void       *last_pub_texture    = NULL;
    const char *last_kind           = "(none)";
    const char *last_reason         = "(none)";
    uint64_t    last_pub_seq        = 0;
    pthread_mutex_lock(&s_front_framebuffer_lock);
    last_src_vram_addr  = s_last_publish_source_vram_addr;
    last_src_texture    = s_last_publish_source_texture;
    last_pub_texture    = s_last_publish_published_texture;
    last_kind           = s_last_publish_kind   ? s_last_publish_kind   : "(none)";
    last_reason         = s_last_publish_reason ? s_last_publish_reason : "(none)";
    last_pub_seq        = s_last_publish_seq;
    void *current_front = atomic_load(&s_front_framebuffer_texture);
    pthread_mutex_unlock(&s_front_framebuffer_lock);

    int64_t  ts_us       = mtl_now_us();
    uint64_t cur_seq     = s_use_seq;
    uint32_t cache_size  = s_cache_size;
    void    *cur_color   = (s_color_binding != NULL) ? s_color_binding->texture : NULL;
    void    *cur_depth   = (s_depth_binding != NULL) ? s_depth_binding->texture : NULL;
    uint32_t cur_color_a = (s_color_binding != NULL) ? s_color_binding->vram_addr : 0;
    uint32_t cur_depth_a = (s_depth_binding != NULL) ? s_depth_binding->vram_addr : 0;

    fprintf(out,
            "{\"type\":\"flip\","
            "\"flip_ordinal\":%llu,\"seq\":%llu,\"ts_us\":%lld,"
            "\"reason\":\"%s\","
            "\"cache_size\":%u,\"msaa\":%u,"
            "\"current_front_texture\":\"%p\","
            "\"current_color_binding_vram_addr\":\"0x%x\","
            "\"current_color_binding_texture\":\"%p\","
            "\"current_depth_binding_vram_addr\":\"0x%x\","
            "\"current_depth_binding_texture\":\"%p\","
            "\"last_publish\":{"
            "\"kind\":\"%s\",\"reason\":\"%s\","
            "\"source_vram_addr\":\"0x%x\",\"source_texture\":\"%p\","
            "\"published_texture\":\"%p\",\"seq\":%llu}}\n",
            (unsigned long long)flip_ordinal,
            (unsigned long long)cur_seq,
            (long long)ts_us,
            reason ? reason : "?",
            cache_size, s_msaa_sample_count,
            current_front,
            cur_color_a, cur_color,
            cur_depth_a, cur_depth,
            last_kind, last_reason,
            last_src_vram_addr, last_src_texture,
            last_pub_texture,
            (unsigned long long)last_pub_seq);

    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        bool is_cur_color = (e == s_color_binding);
        bool is_cur_depth = (e == s_depth_binding);
        bool was_publish_source =
            (e->vram_addr == last_src_vram_addr &&
             e->texture   == last_src_texture &&
             last_src_texture != NULL);
        uint32_t draw_dirty = atomic_load(&e->draw_dirty);
        uint32_t dirty_vram = atomic_load(&e->dirty_vram);
        fprintf(out,
                "{\"type\":\"binding\","
                "\"flip_ordinal\":%llu,\"seq\":%llu,"
                "\"vram_addr\":\"0x%x\",\"size\":%u,\"pitch\":%u,"
                "\"is_color\":%s,"
                "\"width\":%u,\"height\":%u,"
                "\"guest_width\":%u,\"guest_height\":%u,"
                "\"nv097_format\":%u,\"mtl_pixel_format\":%u,"
                "\"msaa_sample_count\":%u,"
                "\"texture\":\"%p\",\"msaa_texture\":\"%p\","
                "\"frame_draw_count\":%u,"
                "\"last_color_draw_seq\":%llu,"
                "\"last_use_seq\":%llu,"
                "\"draw_dirty\":%u,\"dirty_vram\":%u,"
                "\"is_current_color\":%s,\"is_current_depth\":%s,"
                "\"was_publish_source\":%s}\n",
                (unsigned long long)flip_ordinal,
                (unsigned long long)cur_seq,
                (unsigned)e->vram_addr, e->size, e->pitch,
                e->is_color ? "true" : "false",
                e->width, e->height,
                e->guest_width, e->guest_height,
                e->nv097_format, e->mtl_pixel_format,
                e->msaa_sample_count,
                e->texture, e->msaa_texture,
                (unsigned)e->frame_draw_count,
                (unsigned long long)e->last_color_draw_seq,
                (unsigned long long)e->last_use_seq,
                draw_dirty, dirty_vram,
                is_cur_color ? "true" : "false",
                is_cur_depth ? "true" : "false",
                was_publish_source ? "true" : "false");
    }

    atomic_fetch_add(&s_graph_dumps, 1);
    fflush(out);
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
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->vram_addr != vram_addr) {
            continue;
        }
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
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e->vram_addr != vram_addr) {
            continue;
        }
        if (out_cb && *out_cb == NULL) {
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
    /* Upload every dirty sibling that contains this address. Multiple
     * cached shapes may share one VRAM range; the caller may subsequently
     * use any exact guest-size match for render-to-texture sampling. */
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (vram_addr < e->vram_addr ||
            vram_addr >= e->vram_addr + e->size) {
            continue;
        }
        if (atomic_load(&e->dirty_vram)) {
            upload_vram_to_texture(e, vram_ptr);
        }
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
/* 2026-05-20 (M5.12 / M17 followup): cross-sibling sync. */

static bool sibling_sync_enabled(void)
{
    /* Default OFF — the 2026-05-20 first iteration showed promising
     * PGR2-only metrics (magenta-inside-the-car closed, ~70% drop in
     * %white pixels across content frames) but real-time observation
     * across multiple tracked titles (boot logo checkerboarded with
     * missing parts, Halo black screen throughout, Crimson Skies
     * flickering + missing UI) showed the path causes regressions
     * elsewhere. The metal-canary-regress.sh counter-mode passed
     * because it does NOT check visual content (documented limitation
     * in `.claude/rules/renderer-metal.md`).
     *
     * The diagnostic infrastructure (counters, helper functions, this
     * sync path) stays in the tree but defaults to OFF until a proper
     * multi-title visual validation gate is run against the retail
     * Xbox oracle. Set XEMU_METAL_RTT_SIBLING_SYNC=1 to enable for
     * targeted PGR2 diagnostic experiments. */
    static int s_cached = -1;
    if (s_cached < 0) {
        const char *e = getenv("XEMU_METAL_RTT_SIBLING_SYNC");
        s_cached = (e != NULL && *e != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return s_cached != 0;
}

/* For the given target binding, find any other same-VRAM same-pitch
 * same-format same-aspect (color/depth) sibling that has fresher
 * last_color_draw_seq and copy its texture (and MSAA companion, if both
 * sides have one with matching sample count) into `target`. The overlap
 * region is the per-dimension min, anchored at the texture origin.
 *
 * Caller MUST already hold `pg->lock` (the renderer-thread invariant)
 * because we walk the cache linked list and we issue a GPU blit on
 * s_render_queue using s_color_binding-related pointers.
 *
 * No-op when the env flag disables the path, when `target` is NULL,
 * when no fresher sibling exists, or when sample counts mismatch and
 * we cannot safely copy both companion textures. */
static void sync_color_siblings_into(MtlSurfaceBinding *target)
{
    if (!s_initialized || target == NULL || !target->is_color ||
        target->texture == NULL) {
        return;
    }
    if (!sibling_sync_enabled()) {
        return;
    }

    MtlSurfaceBinding *source = NULL;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e == target) continue;
        if (!e->is_color) continue;
        if (e->vram_addr != target->vram_addr) continue;
        if (e->pitch    != target->pitch)    continue;
        if (e->nv097_format != target->nv097_format) continue;
        if (e->texture == NULL) continue;
        if (!atomic_load(&e->draw_dirty)) continue;
        if (e->last_color_draw_seq <= target->last_color_draw_seq) continue;
        if (source == NULL ||
            e->last_color_draw_seq > source->last_color_draw_seq) {
            source = e;
        }
    }
    if (source == NULL) {
        atomic_fetch_add(&s_sibling_sync_skip, 1);
        return;
    }

    /* Sample-count parity: both sides must have a matching MSAA
     * companion (or both have none) for the MSAA-side blit to be
     * legal. Same sample count is the Metal blit precondition. */
    bool sync_msaa = (source->msaa_texture != NULL &&
                      target->msaa_texture != NULL &&
                      source->msaa_sample_count > 1 &&
                      source->msaa_sample_count ==
                          target->msaa_sample_count);

    uint32_t copy_w = source->width  < target->width  ? source->width  : target->width;
    uint32_t copy_h = source->height < target->height ? source->height : target->height;
    if (copy_w == 0 || copy_h == 0) {
        atomic_fetch_add(&s_sibling_sync_skip, 1);
        return;
    }

    /* Drain any open render encoder so the source's resolveTexture and
     * its MSAA companion reflect the latest draws before the blit reads
     * from them. Then fence the blit's command buffer against the latest
     * draw-done value for cross-queue ordering. */
    pgraph_mtl_draw_flush_open_pass();
    void     *event_handle = NULL;
    uint64_t  event_value  = 0;
    pgraph_mtl_draw_get_done_event_state(&event_handle, &event_value);

    @autoreleasepool {
        id<MTLTexture> src_ss = (__bridge id<MTLTexture>)source->texture;
        id<MTLTexture> dst_ss = (__bridge id<MTLTexture>)target->texture;
        if (src_ss == nil || dst_ss == nil) {
            return;
        }

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.sibling_sync";
        if (event_handle != NULL && event_value > 0) {
            id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
            [cmd encodeWaitForEvent:ev value:event_value];
        }

        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        blit.label = @"xemu.metal.sibling_sync_blit";
        [blit copyFromTexture:src_ss
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(copy_w, copy_h, 1)
                    toTexture:dst_ss
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];

        if (sync_msaa) {
            id<MTLTexture> src_ms = (__bridge id<MTLTexture>)source->msaa_texture;
            id<MTLTexture> dst_ms = (__bridge id<MTLTexture>)target->msaa_texture;
            if (src_ms != nil && dst_ms != nil) {
                [blit copyFromTexture:src_ms
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(copy_w, copy_h, 1)
                            toTexture:dst_ms
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
        }
        [blit endEncoding];

        /* Bump the cross-queue draw-done fence as well so subsequent
         * draw-queue work that loads target->msaa_texture / target->texture
         * waits on this blit to land. Mirrors the surface_download pattern. */
        if (event_handle != NULL) {
            uint64_t next = event_value + 1;
            id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
            [cmd encodeSignalEvent:ev value:next];
            /* No corresponding atomic bump of s_draw_done_value here —
             * the next open-pass flush will signal a higher value, and
             * any consumer that already waited on `event_value` is
             * unaffected. The signal is "belt-and-suspenders" so a
             * downstream consumer that reads `event_value+1` (rare /
             * future) still sees the blit complete. */
        }
        [cmd commit];
    }

    /* Adopt source's content age so a back-to-back bind doesn't redo
     * the same blit. The target is now considered draw_dirty (it has
     * fresh content from source) so subsequent downloads-to-VRAM or
     * sample copies pick it up. */
    target->last_color_draw_seq = source->last_color_draw_seq;
    atomic_store(&target->draw_dirty, (uint32_t)1);
    /* Source's draw_dirty stays set — downloads still need to mirror
     * the source's content back to VRAM when consumers request it. */

    atomic_fetch_add(&s_sibling_sync_count, 1);
}

uint64_t pgraph_mtl_surface_sibling_syncs(void)
{
    return atomic_load(&s_sibling_sync_count);
}

uint64_t pgraph_mtl_surface_sibling_sync_skips(void)
{
    return atomic_load(&s_sibling_sync_skip);
}

/* Depth-side sibling sync. Same idea as color-side but uses
 * `last_depth_draw_seq` (bumped only on actual depth writes via
 * `set_draw_dirty_depth`) as the freshness signal — `last_use_seq`
 * cannot be used because cache_find_or_create_depth bumps it BEFORE
 * sync runs, making the target's value the latest. PGR2's depth
 * sibling pattern at 0x38e0000 (z-buffer for the 0x3c84000 color RT)
 * shows the same clip-rect alternation as the color side; without
 * sync, the depth attachment that is about to be re-bound holds the
 * z-values from its OWN last render pass, missing the freshly written
 * depth from the other sibling — manifesting as failed depth tests
 * (missing geometry / dark slabs) on the final composite. */
static void sync_depth_siblings_into(MtlSurfaceBinding *target)
{
    if (!s_initialized || target == NULL || target->is_color ||
        target->texture == NULL) {
        return;
    }
    if (!sibling_sync_enabled()) {
        return;
    }

    MtlSurfaceBinding *source = NULL;
    for (MtlSurfaceBinding *e = s_cache_head; e != NULL; e = e->next) {
        if (e == target) continue;
        if (e->is_color) continue;
        if (e->vram_addr != target->vram_addr) continue;
        if (e->pitch    != target->pitch)    continue;
        if (e->nv097_format != target->nv097_format) continue;
        if (e->texture == NULL) continue;
        if (!atomic_load(&e->draw_dirty)) continue;
        if (e->last_depth_draw_seq <= target->last_depth_draw_seq) continue;
        if (source == NULL ||
            e->last_depth_draw_seq > source->last_depth_draw_seq) {
            source = e;
        }
    }
    if (source == NULL) {
        atomic_fetch_add(&s_sibling_sync_skip, 1);
        return;
    }

    bool sync_msaa = (source->msaa_texture != NULL &&
                      target->msaa_texture != NULL &&
                      source->msaa_sample_count > 1 &&
                      source->msaa_sample_count ==
                          target->msaa_sample_count);

    uint32_t copy_w = source->width  < target->width  ? source->width  : target->width;
    uint32_t copy_h = source->height < target->height ? source->height : target->height;
    if (copy_w == 0 || copy_h == 0) {
        atomic_fetch_add(&s_sibling_sync_skip, 1);
        return;
    }

    pgraph_mtl_draw_flush_open_pass();
    void     *event_handle = NULL;
    uint64_t  event_value  = 0;
    pgraph_mtl_draw_get_done_event_state(&event_handle, &event_value);

    @autoreleasepool {
        id<MTLTexture> src_ss = (__bridge id<MTLTexture>)source->texture;
        id<MTLTexture> dst_ss = (__bridge id<MTLTexture>)target->texture;
        if (src_ss == nil || dst_ss == nil) {
            return;
        }

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.sibling_sync_depth";
        if (event_handle != NULL && event_value > 0) {
            id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
            [cmd encodeWaitForEvent:ev value:event_value];
        }

        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        blit.label = @"xemu.metal.sibling_sync_depth_blit";
        [blit copyFromTexture:src_ss
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(copy_w, copy_h, 1)
                    toTexture:dst_ss
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];

        if (sync_msaa) {
            id<MTLTexture> src_ms = (__bridge id<MTLTexture>)source->msaa_texture;
            id<MTLTexture> dst_ms = (__bridge id<MTLTexture>)target->msaa_texture;
            if (src_ms != nil && dst_ms != nil) {
                [blit copyFromTexture:src_ms
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(copy_w, copy_h, 1)
                            toTexture:dst_ms
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
        }
        [blit endEncoding];

        if (event_handle != NULL) {
            uint64_t next = event_value + 1;
            id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
            [cmd encodeSignalEvent:ev value:next];
        }
        [cmd commit];
    }

    target->last_depth_draw_seq = source->last_depth_draw_seq;
    atomic_store(&target->draw_dirty, (uint32_t)1);

    atomic_fetch_add(&s_sibling_sync_count, 1);
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
    /* Bump the depth-write freshness signal used by the cross-sibling
     * depth sync. Reuses the same monotonic seq as color so the two
     * tracks are comparable. */
    s_depth_binding->last_depth_draw_seq = ++s_use_seq;
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

extern "C" uint64_t pgraph_mtl_surface_download_us_total(void)
{
    return atomic_load(&s_surface_download_us_total);
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
            pthread_mutex_lock(&s_front_framebuffer_lock);
            if (atomic_load(&s_front_framebuffer_texture) == dst->texture) {
                atomic_store(&s_front_framebuffer_texture, (void *)NULL);
            }
            pthread_mutex_unlock(&s_front_framebuffer_lock);
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
