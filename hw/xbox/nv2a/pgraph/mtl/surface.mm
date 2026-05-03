/*
 * NV2A PGRAPH Metal renderer — surface manager implementation (slice M2).
 *
 * Minimal port of vk/surface.c's clear-only path. See surface.h for the
 * scope contract.
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
        /* The Xbox X1R5G5B5 layout is _BGR5A1 in Metal terms. */
        return MTLPixelFormatBGR5A1Unorm;
    case NV097_COLOR_R5G6B5:
        return MTLPixelFormatB5G6R5Unorm;
    case NV097_COLOR_X8R8G8B8_Z8R8G8B8:
    case NV097_COLOR_X8R8G8B8_O8R8G8B8:
    case NV097_COLOR_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_COLOR_X1A7R8G8B8_O1A7R8G8B8:
    case NV097_COLOR_A8R8G8B8:
        /* Xbox A8R8G8B8 in memory order is BGRA when read as little-endian
         * by Metal. Use plain BGRA8Unorm; the present compositor will
         * handle gamma if needed (no _sRGB on the RT — clear values are
         * delivered in linear space and gamma is applied at display
         * time, matching the GL renderer's behaviour). */
        return MTLPixelFormatBGRA8Unorm;
    case NV097_COLOR_B8:
        return MTLPixelFormatR8Unorm;
    case NV097_COLOR_G8B8:
        return MTLPixelFormatRG8Unorm;
    default:
        /* Default to BGRA8 — the most common Xbox color format. M2 will
         * not exercise unusual formats. */
        return MTLPixelFormatBGRA8Unorm;
    }
}

static MTLPixelFormat nv097_zeta_to_mtl(uint32_t nv097)
{
    switch (nv097) {
    case NV097_ZETA_Z16:
        /* MTLPixelFormatDepth16Unorm is supported on all Apple Silicon
         * GPUs. */
        return MTLPixelFormatDepth16Unorm;
    case NV097_ZETA_Z24S8:
        /* Apple Silicon does NOT support Depth24Unorm_Stencil8 (Apple
         * GPU family 7+ only exposes Depth32Float_Stencil8 for combined
         * depth+stencil). Use that as the substitute; precision is
         * higher, not lower, so correctness is preserved. See
         * docs/apple-silicon/metal-api-reference.md. */
        return MTLPixelFormatDepth32Float_Stencil8;
    default:
        return MTLPixelFormatDepth32Float;
    }
}

/* The MTLCommandQueue lives in ui/xemu-metal.mm. The surface manager
 * needs to enqueue a clear command buffer; we add a dedicated "render"
 * command queue here so the surface manager's command buffers are
 * ordered independently of the HUD's per-frame command buffer. (A
 * single MTLDevice supports many command queues; per Apple's
 * Programming Guide a queue per logical work stream is the
 * recommended pattern.)
 *
 * For M2 we only need clear passes; draws (M3) and texture uploads
 * (M6) will reuse the same queue. */
static id<MTLCommandQueue> s_render_queue = nil;

/* Currently bound color RT and depth RT. Re-allocated when shape
 * changes. M2 stores them as raw fields; M3+ will introduce a per-VRAM
 * surface cache analogous to vk/surface.c::PGRAPHVkState.surfaces. */
typedef struct {
    void    *texture;          /* +1 retained handle owned by the heap */
    uint32_t width;
    uint32_t height;
    uint32_t nv097_format;     /* what the caller requested */
    uint32_t mtl_pixel_format; /* MTLPixelFormat actually allocated */
    /* M11: optional memoryless multisample companion texture. Owned
     * out-of-heap (MTLStorageModeMemoryless). NULL when MSAA is off
     * or when the companion has not yet been allocated for this
     * binding's shape. The companion's pixel_format always matches
     * the resolve target's `mtl_pixel_format`. */
    void    *msaa_texture;
    uint32_t msaa_sample_count;
} SurfaceBinding;

static SurfaceBinding s_color_binding;
static SurfaceBinding s_depth_binding;

/* M11: effective MSAA sample count (1 = off; 2/4/8 when on). Set once
 * at pgraph_mtl_init from the env-var-derived clamped value. The
 * surface manager uses this to decide whether to allocate a
 * memoryless multisample companion texture for each binding. */
static uint32_t s_msaa_sample_count = 1;

/* M11: counters. Always-on atomics. */
static _Atomic(uint64_t) s_msaa_resolve_count    = 0;
static _Atomic(uint64_t) s_msaa_resolve_us_total = 0;

/* The "front" framebuffer texture published to the compositor. For M2
 * this aliases s_color_binding.texture — when a clear lands on the
 * color binding, that becomes the visible framebuffer. M3+ will
 * de-alias these (a clear-without-draw should still be visible, but
 * the front fb may differ from the active draw target once
 * render-to-texture is supported). */
static _Atomic(void *) s_front_framebuffer_texture = nullptr;

/* Diagnostic counters. */
static _Atomic(uint64_t) s_clear_count = 0;

static bool s_initialized = false;

/* ---------------------------------------------------------------- */

static bool binding_equals(const SurfaceBinding *b, uint32_t w, uint32_t h,
                           uint32_t nv097_fmt)
{
    return b->texture != NULL &&
           b->width == w && b->height == h && b->nv097_format == nv097_fmt;
}

static void binding_release_msaa(SurfaceBinding *b)
{
    if (b->msaa_texture) {
        pgraph_mtl_heap_release_texture(b->msaa_texture);
        b->msaa_texture = NULL;
    }
    b->msaa_sample_count = 0;
}

static void binding_release(SurfaceBinding *b)
{
    binding_release_msaa(b);
    if (b->texture) {
        pgraph_mtl_heap_release_texture(b->texture);
        b->texture = NULL;
    }
    b->width = 0;
    b->height = 0;
    b->nv097_format = 0;
    b->mtl_pixel_format = 0;
}

/* M11: ensure a memoryless multisample companion exists for this
 * binding when MSAA is enabled and the binding has a real
 * single-sample texture. Re-allocated when shape, format, or sample
 * count changes. is_color selects the heap accessor (color vs depth)
 * but the storage class is the same — out-of-heap memoryless. */
static void binding_ensure_msaa(SurfaceBinding *b, bool is_color)
{
    if (s_msaa_sample_count <= 1 || b->texture == NULL) {
        binding_release_msaa(b);
        return;
    }
    if (b->msaa_texture != NULL &&
        b->msaa_sample_count == s_msaa_sample_count) {
        return; /* still valid for this shape + count */
    }
    binding_release_msaa(b);

    void *tex = is_color
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

    /* maxCommandBufferCount=8 mirrors the host queue (xemu-metal.mm).
     * Surface clear passes are short and rarely in-flight more than 1-2
     * at a time. */
    s_render_queue = [device newCommandQueueWithMaxCommandBufferCount:8];
    if (s_render_queue == nil) {
        fprintf(stderr,
                "pgraph_mtl_surface_init: newCommandQueue failed\n");
        return false;
    }
    s_render_queue.label = @"xemu.metal.render_queue";

    memset(&s_color_binding, 0, sizeof(s_color_binding));
    memset(&s_depth_binding, 0, sizeof(s_depth_binding));
    atomic_store(&s_front_framebuffer_texture, (void *)NULL);
    atomic_store(&s_clear_count, (uint64_t)0);

    s_initialized = true;
    return true;
}

void pgraph_mtl_surface_finalize(void)
{
    if (!s_initialized) {
        return;
    }

    /* Drop the front-fb pointer before releasing the underlying
     * texture. The compositor checks this pointer atomically; once it
     * reads NULL it falls back to the black background. */
    atomic_store(&s_front_framebuffer_texture, (void *)NULL);

    binding_release(&s_color_binding);
    binding_release(&s_depth_binding);

    s_render_queue = nil;
    s_initialized = false;
}

/* ---------------------------------------------------------------- */

void pgraph_mtl_surface_ensure_color(uint32_t width, uint32_t height,
                                     uint32_t nv097_color_format)
{
    if (!s_initialized || width == 0 || height == 0) {
        return;
    }
    if (binding_equals(&s_color_binding, width, height, nv097_color_format)) {
        return;
    }

    MTLPixelFormat mtl_fmt = nv097_color_to_mtl(nv097_color_format);

    binding_release(&s_color_binding);
    s_color_binding.texture =
        pgraph_mtl_heap_alloc_color_rt(width, height, (uint32_t)mtl_fmt);
    if (s_color_binding.texture) {
        s_color_binding.width = width;
        s_color_binding.height = height;
        s_color_binding.nv097_format = nv097_color_format;
        s_color_binding.mtl_pixel_format = (uint32_t)mtl_fmt;
        /* M11: pair the single-sample target with a memoryless
         * multisample companion when MSAA is enabled. The single-
         * sample texture remains the resolveTexture / front-fb. */
        binding_ensure_msaa(&s_color_binding, /*is_color=*/true);
    }

    /* Republish the front-fb pointer to the new texture. */
    atomic_store(&s_front_framebuffer_texture, s_color_binding.texture);
}

void pgraph_mtl_surface_ensure_depth(uint32_t width, uint32_t height,
                                     uint32_t nv097_zeta_format)
{
    if (!s_initialized || width == 0 || height == 0) {
        return;
    }
    if (binding_equals(&s_depth_binding, width, height, nv097_zeta_format)) {
        return;
    }

    MTLPixelFormat mtl_fmt = nv097_zeta_to_mtl(nv097_zeta_format);

    binding_release(&s_depth_binding);
    s_depth_binding.texture =
        pgraph_mtl_heap_alloc_depth_rt(width, height, (uint32_t)mtl_fmt);
    if (s_depth_binding.texture) {
        s_depth_binding.width = width;
        s_depth_binding.height = height;
        s_depth_binding.nv097_format = nv097_zeta_format;
        s_depth_binding.mtl_pixel_format = (uint32_t)mtl_fmt;
        /* M11: memoryless multisample depth companion. */
        binding_ensure_msaa(&s_depth_binding, /*is_color=*/false);
    }
}

/* ---------------------------------------------------------------- */

void pgraph_mtl_surface_clear(bool write_color, const float rgba[4],
                              bool write_zeta, float depth)
{
    if (!s_initialized) {
        return;
    }

    bool have_color_target = write_color && s_color_binding.texture != NULL;
    bool have_depth_target = write_zeta && s_depth_binding.texture != NULL;
    if (!have_color_target && !have_depth_target) {
        /* Nothing to clear. Don't burn a command buffer. */
        return;
    }

    /* M11: when MSAA is enabled the render-pass target is the
     * memoryless multisample companion; the single-sample binding
     * becomes the resolveTexture and the storeAction switches to
     * MTLStoreActionMultisampleResolve. The clear loadAction still
     * fires against the multisample target — Metal expands the clear
     * across all samples. The resolve then writes the (here trivial,
     * since no draws ran) per-sample-averaged single-sample copy back
     * into the binding texture, where the present compositor reads
     * it. */
    bool have_msaa_color =
        have_color_target && s_color_binding.msaa_texture != NULL &&
        s_color_binding.msaa_sample_count > 1;
    bool have_msaa_depth =
        have_depth_target && s_depth_binding.msaa_texture != NULL &&
        s_depth_binding.msaa_sample_count > 1;
    bool resolved = false;

    @autoreleasepool {
        MTLRenderPassDescriptor *desc =
            [MTLRenderPassDescriptor renderPassDescriptor];

        if (have_color_target) {
            id<MTLTexture> tex =
                (__bridge id<MTLTexture>)s_color_binding.texture;
            if (have_msaa_color) {
                id<MTLTexture> ms =
                    (__bridge id<MTLTexture>)s_color_binding.msaa_texture;
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
                (__bridge id<MTLTexture>)s_depth_binding.texture;
            if (have_msaa_depth) {
                /* MSAA depth: render into the memoryless multisample
                 * depth target; we do NOT resolve depth in the clear
                 * pass — the post-resolve depth is unused (the
                 * compositor only samples color). MTLStoreActionDontCare
                 * is the lowest-cost option on TBDR; the multisample
                 * depth content vanishes with the tile. */
                id<MTLTexture> ms =
                    (__bridge id<MTLTexture>)s_depth_binding.msaa_texture;
                desc.depthAttachment.texture     = ms;
                desc.depthAttachment.loadAction  = MTLLoadActionClear;
                desc.depthAttachment.storeAction = MTLStoreActionDontCare;
            } else {
                desc.depthAttachment.texture     = tex;
                desc.depthAttachment.loadAction  = MTLLoadActionClear;
                desc.depthAttachment.storeAction = MTLStoreActionStore;
            }
            desc.depthAttachment.clearDepth  = depth;

            /* Stencil clear only emitted when the format includes
             * stencil; the common Z16 / D32Float paths skip this. The
             * NV097_CLEAR_SURFACE_STENCIL bit is decoded upstream and
             * reflected in `write_zeta` — for M2 we don't yet route the
             * stencil value separately, so leave the stencil store
             * action untouched (MTLStoreActionDontCare) on
             * depth-only formats. */
            MTLPixelFormat fmt =
                (MTLPixelFormat)s_depth_binding.mtl_pixel_format;
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

        /* If we have a depth target but no color target, MTLRenderPass
         * still needs an attachment — depthAttachment alone is fine,
         * Metal accepts a depth-only pass. Conversely a color-only
         * pass is fine. */

        id<MTLCommandBuffer> cmd = [s_render_queue commandBuffer];
        cmd.label = @"xemu.metal.clear";

        id<MTLRenderCommandEncoder> enc =
            [cmd renderCommandEncoderWithDescriptor:desc];
        enc.label = @"xemu.metal.clear_enc";
        /* Empty render pass — the loadAction does the work. */
        [enc endEncoding];
        [cmd commit];
    }

    atomic_fetch_add(&s_clear_count, 1);
    if (resolved) {
        atomic_fetch_add(&s_msaa_resolve_count, 1);
        /* The clear-only resolve is in-tile and effectively free on
         * Apple Silicon (it expands the cleared sample value to the
         * single-sample destination). We attribute the per-clear
         * cost to the resolve counter at a fixed nominal value (1 µs)
         * rather than measuring per-call clock — the real cost is
         * already attributed to the surface clear, and per-call CPU
         * timing here would dwarf the actual GPU resolve work. M13's
         * counter-sample-buffer integration will replace this
         * placeholder with GPU-side timing. */
        atomic_fetch_add(&s_msaa_resolve_us_total, (uint64_t)1);
    }

    /* After a clear the color binding is fresh content; publish it. */
    if (have_color_target) {
        atomic_store(&s_front_framebuffer_texture,
                     s_color_binding.texture);
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
    /* For M2 the front-fb texture is long-lived (lives in the heap and
     * is reused frame-to-frame); no per-call release is needed. M3+
     * may extend this once in-flight tracking lands. */
}

uint64_t pgraph_mtl_surface_clear_count(void)
{
    return atomic_load(&s_clear_count);
}

/* ---------------------------------------------------------------- */

void *pgraph_mtl_surface_get_color_texture(void)
{
    return s_initialized ? s_color_binding.texture : NULL;
}

void *pgraph_mtl_surface_get_depth_texture(void)
{
    return s_initialized ? s_depth_binding.texture : NULL;
}

uint32_t pgraph_mtl_surface_get_color_format(void)
{
    return s_initialized ? s_color_binding.mtl_pixel_format : 0;
}

uint32_t pgraph_mtl_surface_get_depth_format(void)
{
    return s_initialized ? s_depth_binding.mtl_pixel_format : 0;
}

uint32_t pgraph_mtl_surface_get_width(void)
{
    if (!s_initialized) {
        return 0;
    }
    if (s_color_binding.texture != NULL) {
        return s_color_binding.width;
    }
    return s_depth_binding.width;
}

uint32_t pgraph_mtl_surface_get_height(void)
{
    if (!s_initialized) {
        return 0;
    }
    if (s_color_binding.texture != NULL) {
        return s_color_binding.height;
    }
    return s_depth_binding.height;
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
    /* Re-allocate companion bindings against the new count. If the
     * binding has not yet been created (first clear/draw not landed
     * yet), the ensure call is a no-op until the underlying
     * single-sample texture exists. */
    if (s_initialized) {
        binding_ensure_msaa(&s_color_binding, /*is_color=*/true);
        binding_ensure_msaa(&s_depth_binding, /*is_color=*/false);
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
    return s_color_binding.msaa_texture;
}

void *pgraph_mtl_surface_get_msaa_depth_texture(void)
{
    if (!s_initialized || s_msaa_sample_count <= 1) {
        return NULL;
    }
    return s_depth_binding.msaa_texture;
}

uint64_t pgraph_mtl_surface_msaa_resolve_count(void)
{
    return atomic_load(&s_msaa_resolve_count);
}

uint64_t pgraph_mtl_surface_msaa_resolve_us_total(void)
{
    return atomic_load(&s_msaa_resolve_us_total);
}
