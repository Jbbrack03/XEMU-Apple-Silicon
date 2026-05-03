/*
 * NV2A PGRAPH Metal renderer — draw machinery implementation
 * (slice M3 — passthrough, slice M4 — indexed + native-depth variant).
 *
 * Each draw runs as its own command buffer on a dedicated render queue
 * — no per-frame batching yet. The buffer ring's begin/end_frame are
 * called around the encode so the staged data is gated on the GPU's
 * consumption marker.
 *
 * M4 additions:
 *   - `pgraph_mtl_draw_indexed` — uses the staging-ring index helper
 *     to upload a uint32 index list and encodes
 *     `drawIndexedPrimitives:`. Used by renderer.c for the geometry
 *     expansions (triangle_fan, quads, quad_strip, polygon, line_loop).
 *   - `variant` plumbing — selects between the passthrough fragment
 *     shader and the native-depth fragment shader (which writes a
 *     derived depth value).
 *   - Per-variant counters (METAL_NATIVE_TRI_DEPTH_DRAWS /
 *     METAL_NATIVE_QUAD_DRAWS / METAL_DRAW_INDEXED_COUNT) drive the
 *     M4 exit gate "counters match GL counts".
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "draw.h"
#include "buffer.h"
#include "pipeline.h"
#include "surface.h"
#include "texture.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" void *xemu_metal_get_device(void);

/* M8: gate the draw command buffer on the latest texture upload by
 * encoding a wait on the shared upload fence. Cheap on Apple Silicon
 * and serves as the GPU-side replacement for the M6
 * `[cb waitUntilCompleted]` in mtl/texture.mm. Skipped when the
 * fence value is 0 — no upload has yet signaled, so there is
 * nothing to wait on. */
static inline void mtl_draw_wait_upload_fence(id<MTLCommandBuffer> cb)
{
    void *event_handle = pgraph_mtl_texture_get_upload_fence_event();
    if (event_handle == NULL) {
        return;
    }
    uint64_t value = pgraph_mtl_texture_get_upload_fence_value();
    if (value == 0) {
        return;
    }
    id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
    [cb encodeWaitForEvent:ev value:value];
}

static id<MTLDevice>       s_device;
static id<MTLCommandQueue> s_draw_queue;
static bool                s_initialized = false;
static _Atomic(uint64_t)   s_draw_count = 0;
static _Atomic(uint64_t)   s_draw_indexed_count = 0;
static _Atomic(uint64_t)   s_draw_native_tri_depth_count = 0;
static _Atomic(uint64_t)   s_draw_native_quad_count = 0;
static _Atomic(uint64_t)   s_draw_translated_count = 0;
static _Atomic(uint64_t)   s_draw_pipeline_fallback_count = 0;

/* M5.5+ render-pass coalescing.
 *
 * Apple Silicon's TBDR makes every render-pass start expensive (tile-
 * load) and every store expensive (tile-store). The pre-coalescing
 * Metal renderer opened a fresh `MTLCommandBuffer` + render-encoder +
 * `commit` for each NV2A flush_draw — at PGR2's ~22 k draws/s, that
 * was ~22 k cmdbuf commits/s, each forcing a CPU↔GPU sync. The
 * 2026-05-03 paired Metal/GL bench measured PGR2 at 16 fps vs GL 31.
 *
 * Coalescing rule (per WWDC20-10632 + emulator-survey research):
 * hold one cmdbuf+encoder open across consecutive draws when the
 * attachment set is unchanged. Close on attachment change, frame-end
 * (flip_stall), clear, surface flush, or shutdown.
 *
 * The "open pass" state is single-instance because the renderer is
 * driven by the iothread / NV2A worker. Every draw entry is a single
 * caller mutating these statics under the implicit pgraph lock.
 *
 * Functions:
 *   open_pass_ensure(...)  — ensure an encoder is open matching the
 *                             passed attachments; close+reopen on
 *                             mismatch.
 *   pgraph_mtl_draw_flush_open_pass() — close+commit any open pass.
 */
static id<MTLCommandBuffer>       s_open_cmd = nil;
static id<MTLRenderCommandEncoder> s_open_enc = nil;
static struct {
    void     *color_tex;
    void     *depth_tex;
    uint32_t  color_fmt;
    uint32_t  depth_fmt;
    uint32_t  sample_count;
} s_open_pass_key;
static bool s_open_buffer_frame_active = false;

/* Per-interval coalescing telemetry (surfaced via accessors at file
 * end; xemu-metal-perf.c emits as METAL_PASS_OPENS / _COALESCED). */
static _Atomic(uint64_t) s_open_pass_opens     = 0;
static _Atomic(uint64_t) s_open_pass_coalesced = 0;
static _Atomic(uint64_t) s_open_pass_flushes   = 0;

bool pgraph_mtl_draw_init(void)
{
    if (s_initialized) {
        return true;
    }

    s_device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (s_device == nil) {
        fprintf(stderr,
                "pgraph_mtl_draw_init: no Metal device\n");
        return false;
    }

    s_draw_queue = [s_device newCommandQueueWithMaxCommandBufferCount:8];
    if (s_draw_queue == nil) {
        fprintf(stderr,
                "pgraph_mtl_draw_init: newCommandQueue failed\n");
        return false;
    }
    s_draw_queue.label = @"xemu.metal.draw_queue";

    atomic_store(&s_draw_count, (uint64_t)0);
    atomic_store(&s_draw_indexed_count, (uint64_t)0);
    atomic_store(&s_draw_native_tri_depth_count, (uint64_t)0);
    atomic_store(&s_draw_native_quad_count, (uint64_t)0);
    atomic_store(&s_draw_translated_count, (uint64_t)0);
    atomic_store(&s_draw_pipeline_fallback_count, (uint64_t)0);
    s_initialized = true;
    return true;
}

void pgraph_mtl_draw_finalize(void)
{
    if (!s_initialized) {
        return;
    }
    /* M5.5+: drain any open coalesced pass before tearing down the
     * queue. Calling endEncoding/commit on a queue that's about to
     * release would otherwise leak GPU work. */
    extern void pgraph_mtl_draw_flush_open_pass(void);
    pgraph_mtl_draw_flush_open_pass();
    s_draw_queue = nil;
    s_device = nil;
    s_initialized = false;
}

/* -------- shared encode setup -------- */

static MTLRenderPassDescriptor *
build_render_pass_descriptor(void *surface_color, void *surface_depth,
                             uint32_t depth_fmt)
{
    MTLRenderPassDescriptor *desc =
        [MTLRenderPassDescriptor renderPassDescriptor];

    /* M11: query the MSAA state once. When the surface manager has a
     * memoryless multisample companion bound for the active color/
     * depth target, the render pass's `texture` becomes the
     * multisample companion and the single-sample target becomes the
     * `resolveTexture`. The store action becomes
     * MTLStoreActionMultisampleResolve for color (so the resolved
     * single-sample copy is what subsequent passes / the present
     * compositor read). For depth we use MTLStoreActionDontCare on
     * the multisample target — Apple Silicon's TBDR keeps it in
     * tile memory and the post-resolve depth is not consumed by the
     * compositor. */
    uint32_t samples = pgraph_mtl_surface_get_msaa_sample_count();
    void *msaa_color_handle = NULL;
    void *msaa_depth_handle = NULL;
    if (samples > 1) {
        if (surface_color != NULL &&
            surface_color == pgraph_mtl_surface_get_color_texture()) {
            msaa_color_handle = pgraph_mtl_surface_get_msaa_color_texture();
        }
        if (surface_depth != NULL &&
            surface_depth == pgraph_mtl_surface_get_depth_texture()) {
            msaa_depth_handle = pgraph_mtl_surface_get_msaa_depth_texture();
        }
    }

    if (surface_color != NULL) {
        id<MTLTexture> tex = (__bridge id<MTLTexture>)surface_color;
        if (msaa_color_handle != NULL) {
            id<MTLTexture> ms =
                (__bridge id<MTLTexture>)msaa_color_handle;
            desc.colorAttachments[0].texture        = ms;
            desc.colorAttachments[0].resolveTexture = tex;
            /* Load existing single-sample contents into the
             * multisample target. Metal expands the single-sample
             * source across all samples for the load. The
             * MultisampleResolve store action then collapses it back
             * to the single-sample resolveTexture. */
            desc.colorAttachments[0].loadAction  = MTLLoadActionLoad;
            desc.colorAttachments[0].storeAction =
                MTLStoreActionMultisampleResolve;
        } else {
            desc.colorAttachments[0].texture     = tex;
            /* Load existing contents — the prior clear / draw is the
             * source of truth. M3/M4 do NOT clear in the draw pass. */
            desc.colorAttachments[0].loadAction  = MTLLoadActionLoad;
            desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        }
    }

    if (surface_depth != NULL) {
        id<MTLTexture> dtex = (__bridge id<MTLTexture>)surface_depth;
        MTLPixelFormat dfmt = (MTLPixelFormat)depth_fmt;
        if (msaa_depth_handle != NULL) {
            id<MTLTexture> ms =
                (__bridge id<MTLTexture>)msaa_depth_handle;
            desc.depthAttachment.texture     = ms;
            desc.depthAttachment.loadAction  = MTLLoadActionLoad;
            desc.depthAttachment.storeAction = MTLStoreActionDontCare;
            if (dfmt == MTLPixelFormatDepth32Float_Stencil8 ||
                dfmt == MTLPixelFormatDepth24Unorm_Stencil8 ||
                dfmt == MTLPixelFormatStencil8) {
                desc.stencilAttachment.texture     = ms;
                desc.stencilAttachment.loadAction  = MTLLoadActionLoad;
                desc.stencilAttachment.storeAction = MTLStoreActionDontCare;
            }
        } else {
            desc.depthAttachment.texture     = dtex;
            desc.depthAttachment.loadAction  = MTLLoadActionLoad;
            desc.depthAttachment.storeAction = MTLStoreActionStore;
            /* Stencil aspect for combined formats. */
            if (dfmt == MTLPixelFormatDepth32Float_Stencil8 ||
                dfmt == MTLPixelFormatDepth24Unorm_Stencil8 ||
                dfmt == MTLPixelFormatStencil8) {
                desc.stencilAttachment.texture     = dtex;
                desc.stencilAttachment.loadAction  = MTLLoadActionLoad;
                desc.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        }
    }

    return desc;
}

static void *select_pipeline(uint32_t variant, uint32_t color_fmt,
                             uint32_t depth_fmt)
{
    /* M11: pipeline rasterSampleCount must match the active render
     * pass's MSAA sample count. Query the surface manager (the
     * render-pass descriptor builder above does the same — keep
     * sources in lock-step). */
    uint32_t samples = pgraph_mtl_surface_get_msaa_sample_count();
    if (variant == MTL_DRAW_VARIANT_NATIVE_DEPTH) {
        return pgraph_mtl_pipeline_get_native_depth(color_fmt, depth_fmt,
                                                    samples);
    }
    return pgraph_mtl_pipeline_get_passthrough(color_fmt, depth_fmt,
                                                samples);
}

/* -------- M5.5+ open-pass helpers -------- */

static bool open_pass_matches(void *color_tex, void *depth_tex,
                              uint32_t color_fmt, uint32_t depth_fmt,
                              uint32_t sample_count)
{
    if (s_open_enc == nil) return false;
    return s_open_pass_key.color_tex   == color_tex &&
           s_open_pass_key.depth_tex   == depth_tex &&
           s_open_pass_key.color_fmt   == color_fmt &&
           s_open_pass_key.depth_fmt   == depth_fmt &&
           s_open_pass_key.sample_count == sample_count;
}

static void open_pass_close_locked(void)
{
    if (s_open_enc != nil) {
        [s_open_enc endEncoding];
        s_open_enc = nil;
    }
    if (s_open_cmd != nil) {
        [s_open_cmd commit];
        s_open_cmd = nil;
    }
    if (s_open_buffer_frame_active) {
        pgraph_mtl_buffer_end_frame();
        s_open_buffer_frame_active = false;
    }
    memset(&s_open_pass_key, 0, sizeof(s_open_pass_key));
}

/* Ensure an open render encoder matching the supplied attachment set.
 * Returns the encoder (caller does not retain). On mismatch, the
 * existing pass is committed and a fresh one opened. The first call
 * since flush also calls pgraph_mtl_buffer_begin_frame() so the
 * staging-ring has a valid frame for vertex stage allocations. */
static id<MTLRenderCommandEncoder>
open_pass_ensure(void *color_tex, void *depth_tex,
                 uint32_t color_fmt, uint32_t depth_fmt,
                 uint32_t sample_count)
{
    if (open_pass_matches(color_tex, depth_tex, color_fmt, depth_fmt,
                          sample_count)) {
        atomic_fetch_add(&s_open_pass_coalesced, 1);
        return s_open_enc;
    }

    open_pass_close_locked();

    if (!s_open_buffer_frame_active) {
        pgraph_mtl_buffer_begin_frame();
        s_open_buffer_frame_active = true;
    }

    @autoreleasepool {
        MTLRenderPassDescriptor *desc =
            build_render_pass_descriptor(color_tex, depth_tex, depth_fmt);

        s_open_cmd = [s_draw_queue commandBuffer];
        s_open_cmd.label = @"xemu.metal.coalesced_draw";
        mtl_draw_wait_upload_fence(s_open_cmd);

        s_open_enc = [s_open_cmd renderCommandEncoderWithDescriptor:desc];
        s_open_enc.label = @"xemu.metal.coalesced_enc";
    }

    s_open_pass_key.color_tex    = color_tex;
    s_open_pass_key.depth_tex    = depth_tex;
    s_open_pass_key.color_fmt    = color_fmt;
    s_open_pass_key.depth_fmt    = depth_fmt;
    s_open_pass_key.sample_count = sample_count;

    atomic_fetch_add(&s_open_pass_opens, 1);
    return s_open_enc;
}

/* External entry point — invoked from renderer.c at flip_stall, before
 * clear_surface, before shutdown, and (eventually) before the
 * compositor reads the surface texture for present. */
extern "C" void pgraph_mtl_draw_flush_open_pass(void)
{
    if (s_open_enc != nil || s_open_cmd != nil ||
        s_open_buffer_frame_active) {
        atomic_fetch_add(&s_open_pass_flushes, 1);
    }
    open_pass_close_locked();
}

/* -------- non-indexed (M3) -------- */

void pgraph_mtl_draw_passthrough(const float *positions,
                                 const float *colors,
                                 unsigned int vertex_count,
                                 uint32_t mtl_primitive,
                                 uint32_t variant,
                                 unsigned int viewport_w,
                                 unsigned int viewport_h,
                                 void *surface_color,
                                 void *surface_depth,
                                 uint32_t color_fmt,
                                 uint32_t depth_fmt)
{
    if (!s_initialized || vertex_count == 0 ||
        positions == NULL || colors == NULL) {
        return;
    }
    if (surface_color == NULL && surface_depth == NULL) {
        return;
    }

    void *ps_handle = select_pipeline(variant, color_fmt, depth_fmt);
    if (ps_handle == NULL) {
        return;
    }

    /* M5.5+: open-pass helpers manage the cmdbuf + buffer-frame
     * lifetime. begin_frame is called inside open_pass_ensure when a
     * new pass actually opens; nothing to do here. */
    uint32_t samples = pgraph_mtl_surface_get_msaa_sample_count();
    id<MTLRenderCommandEncoder> enc = open_pass_ensure(
        surface_color, surface_depth, color_fmt, depth_fmt, samples);
    if (enc == nil) {
        return;
    }

    void *pos_buf = NULL, *col_buf = NULL;
    size_t pos_off = 0, col_off = 0;
    size_t pos_size = (size_t)vertex_count * 4 * sizeof(float);
    size_t col_size = (size_t)vertex_count * 4 * sizeof(float);

    if (!pgraph_mtl_buffer_stage_vertex(positions, pos_size,
                                        &pos_buf, &pos_off) ||
        !pgraph_mtl_buffer_stage_vertex(colors, col_size,
                                        &col_buf, &col_off)) {
        return;
    }

    id<MTLRenderPipelineState> ps =
        (__bridge id<MTLRenderPipelineState>)ps_handle;
    [enc setRenderPipelineState:ps];

    MTLViewport vp = (MTLViewport){
        .originX = 0.0,
        .originY = 0.0,
        .width   = (double)viewport_w,
        .height  = (double)viewport_h,
        .znear   = 0.0,
        .zfar    = 1.0,
    };
    [enc setViewport:vp];

    id<MTLBuffer> pbuf = (__bridge id<MTLBuffer>)pos_buf;
    id<MTLBuffer> cbuf = (__bridge id<MTLBuffer>)col_buf;
    [enc setVertexBuffer:pbuf offset:pos_off atIndex:0];
    [enc setVertexBuffer:cbuf offset:col_off atIndex:1];

    MTLPrimitiveType prim = (MTLPrimitiveType)mtl_primitive;
    [enc drawPrimitives:prim
            vertexStart:0
            vertexCount:vertex_count];

    atomic_fetch_add(&s_draw_count, 1);
}

/* -------- indexed (M4) -------- */

void pgraph_mtl_draw_indexed(const float *positions,
                             const float *colors,
                             unsigned int vertex_count,
                             const uint32_t *indices,
                             unsigned int index_count,
                             uint32_t mtl_primitive,
                             uint32_t variant,
                             unsigned int viewport_w,
                             unsigned int viewport_h,
                             void *surface_color,
                             void *surface_depth,
                             uint32_t color_fmt,
                             uint32_t depth_fmt)
{
    if (!s_initialized || vertex_count == 0 || index_count == 0 ||
        positions == NULL || colors == NULL || indices == NULL) {
        return;
    }
    if (surface_color == NULL && surface_depth == NULL) {
        return;
    }

    void *ps_handle = select_pipeline(variant, color_fmt, depth_fmt);
    if (ps_handle == NULL) {
        return;
    }

    /* M5.5+: open-pass coalescing — see passthrough path. */
    uint32_t samples = pgraph_mtl_surface_get_msaa_sample_count();
    id<MTLRenderCommandEncoder> enc = open_pass_ensure(
        surface_color, surface_depth, color_fmt, depth_fmt, samples);
    if (enc == nil) {
        return;
    }

    void *pos_buf = NULL, *col_buf = NULL, *idx_buf = NULL;
    size_t pos_off = 0, col_off = 0, idx_off = 0;
    size_t pos_size = (size_t)vertex_count * 4 * sizeof(float);
    size_t col_size = (size_t)vertex_count * 4 * sizeof(float);
    size_t idx_size = (size_t)index_count * sizeof(uint32_t);

    if (!pgraph_mtl_buffer_stage_vertex(positions, pos_size,
                                        &pos_buf, &pos_off) ||
        !pgraph_mtl_buffer_stage_vertex(colors, col_size,
                                        &col_buf, &col_off) ||
        !pgraph_mtl_buffer_stage_index(indices, idx_size,
                                       &idx_buf, &idx_off)) {
        return;
    }

    id<MTLRenderPipelineState> ps =
        (__bridge id<MTLRenderPipelineState>)ps_handle;
    [enc setRenderPipelineState:ps];

    MTLViewport vp = (MTLViewport){
        .originX = 0.0,
        .originY = 0.0,
        .width   = (double)viewport_w,
        .height  = (double)viewport_h,
        .znear   = 0.0,
        .zfar    = 1.0,
    };
    [enc setViewport:vp];

    id<MTLBuffer> pbuf = (__bridge id<MTLBuffer>)pos_buf;
    id<MTLBuffer> cbuf = (__bridge id<MTLBuffer>)col_buf;
    id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)idx_buf;
    [enc setVertexBuffer:pbuf offset:pos_off atIndex:0];
    [enc setVertexBuffer:cbuf offset:col_off atIndex:1];

    MTLPrimitiveType prim = (MTLPrimitiveType)mtl_primitive;
    [enc drawIndexedPrimitives:prim
                    indexCount:index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:ibuf
             indexBufferOffset:idx_off];

    atomic_fetch_add(&s_draw_count, 1);
    atomic_fetch_add(&s_draw_indexed_count, 1);
}

uint64_t pgraph_mtl_draw_count(void)
{
    return atomic_load(&s_draw_count);
}

uint64_t pgraph_mtl_draw_indexed_count(void)
{
    return atomic_load(&s_draw_indexed_count);
}

uint64_t pgraph_mtl_draw_native_tri_depth_count(void)
{
    return atomic_load(&s_draw_native_tri_depth_count);
}

uint64_t pgraph_mtl_draw_native_quad_count(void)
{
    return atomic_load(&s_draw_native_quad_count);
}

/* Renderer.c bumps the right native_* counter from the call site
 * because it has the NV2A primitive_mode info necessary to
 * distinguish native_tri_depth (triangle family) from native_quad
 * (quad family). Keeping the increment hooks separate avoids leaking
 * primitive-mode knowledge into draw.mm. The counters intentionally
 * parallel the GL profile counters NATIVE_TRI_DEPTH_DRAW and
 * NATIVE_QUAD_DRAW; they are mutually exclusive (a draw is either
 * triangle-family or quad-family, never both). */
extern "C" void pgraph_mtl_draw_inc_native_tri_depth_count(void)
{
    atomic_fetch_add(&s_draw_native_tri_depth_count, 1);
}

extern "C" void pgraph_mtl_draw_inc_native_quad_count(void)
{
    atomic_fetch_add(&s_draw_native_quad_count, 1);
}

/* -------- M7.1: translated-pipeline encode -------- */

void pgraph_mtl_draw_translated(void *pipeline_state,
                                const float *positions,
                                const float *colors,
                                unsigned int vertex_count,
                                const uint32_t *indices,
                                unsigned int index_count,
                                uint32_t mtl_primitive,
                                unsigned int viewport_w,
                                unsigned int viewport_h,
                                void *surface_color,
                                void *surface_depth,
                                uint32_t depth_fmt,
                                void *vsh_ubo,
                                size_t vsh_ubo_offset,
                                size_t vsh_ubo_size,
                                void *psh_ubo,
                                size_t psh_ubo_offset,
                                size_t psh_ubo_size,
                                void *const stage_textures[4],
                                void *const stage_samplers[4])
{
    if (!s_initialized || pipeline_state == NULL ||
        vertex_count == 0 || positions == NULL || colors == NULL) {
        return;
    }
    if (surface_color == NULL && surface_depth == NULL) {
        return;
    }
    bool indexed = (indices != NULL && index_count > 0);

    /* M5.5+: open-pass coalescing — see passthrough path. We do NOT
     * pass color_fmt here because the translated path is invoked
     * with the surface manager's effective color format already
     * baked into the pipeline_state; we still query it from the
     * surface manager so the pass key matches the M3/M4 pass key
     * when both flow through the same underlying surface. */
    uint32_t color_fmt = pgraph_mtl_surface_get_color_format();
    if (surface_color == NULL) {
        color_fmt = 0;
    }
    uint32_t samples = pgraph_mtl_surface_get_msaa_sample_count();
    id<MTLRenderCommandEncoder> enc = open_pass_ensure(
        surface_color, surface_depth, color_fmt, depth_fmt, samples);
    if (enc == nil) {
        return;
    }

    void *pos_buf = NULL, *col_buf = NULL, *idx_buf = NULL;
    size_t pos_off = 0, col_off = 0, idx_off = 0;
    size_t pos_size = (size_t)vertex_count * 4 * sizeof(float);
    size_t col_size = (size_t)vertex_count * 4 * sizeof(float);

    if (!pgraph_mtl_buffer_stage_vertex(positions, pos_size, &pos_buf, &pos_off) ||
        !pgraph_mtl_buffer_stage_vertex(colors, col_size, &col_buf, &col_off)) {
        return;
    }

    if (indexed) {
        size_t idx_size = (size_t)index_count * sizeof(uint32_t);
        if (!pgraph_mtl_buffer_stage_index(indices, idx_size,
                                           &idx_buf, &idx_off)) {
            return;
        }
    }

    id<MTLRenderPipelineState> ps =
        (__bridge id<MTLRenderPipelineState>)pipeline_state;
    [enc setRenderPipelineState:ps];

    MTLViewport vp = (MTLViewport){
        .originX = 0.0,
        .originY = 0.0,
        .width   = (double)viewport_w,
        .height  = (double)viewport_h,
        .znear   = 0.0,
        .zfar    = 1.0,
    };
    [enc setViewport:vp];

    /* Bind position at slot 0 and diffuse color at slot 3 to match
     * the NV2A_VERTEX_ATTR_DIFFUSE buffer_index established by the
     * pipeline key (see state.c). */
    id<MTLBuffer> pbuf = (__bridge id<MTLBuffer>)pos_buf;
    id<MTLBuffer> cbuf = (__bridge id<MTLBuffer>)col_buf;
    [enc setVertexBuffer:pbuf offset:pos_off atIndex:0];
    [enc setVertexBuffer:cbuf offset:col_off atIndex:3];

    /* UBOs at the spirv-cross emitted [[buffer(N)]] indices. */
    if (vsh_ubo != NULL && vsh_ubo_size > 0) {
        id<MTLBuffer> ub = (__bridge id<MTLBuffer>)vsh_ubo;
        [enc setVertexBuffer:ub offset:vsh_ubo_offset atIndex:1];
    }
    if (psh_ubo != NULL && psh_ubo_size > 0) {
        id<MTLBuffer> ub = (__bridge id<MTLBuffer>)psh_ubo;
        [enc setFragmentBuffer:ub offset:psh_ubo_offset atIndex:1];
    }

    if (stage_textures != NULL) {
        for (int i = 0; i < 4; i++) {
            if (stage_textures[i] != NULL) {
                id<MTLTexture> t =
                    (__bridge id<MTLTexture>)stage_textures[i];
                [enc setFragmentTexture:t atIndex:i];
            }
        }
    }
    if (stage_samplers != NULL) {
        for (int i = 0; i < 4; i++) {
            if (stage_samplers[i] != NULL) {
                id<MTLSamplerState> ss =
                    (__bridge id<MTLSamplerState>)stage_samplers[i];
                [enc setFragmentSamplerState:ss atIndex:i];
            }
        }
    }

    MTLPrimitiveType prim = (MTLPrimitiveType)mtl_primitive;
    if (indexed) {
        id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)idx_buf;
        [enc drawIndexedPrimitives:prim
                        indexCount:index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:ibuf
                 indexBufferOffset:idx_off];
    } else {
        [enc drawPrimitives:prim
                vertexStart:0
                vertexCount:vertex_count];
    }

    atomic_fetch_add(&s_draw_count, 1);
    if (indexed) {
        atomic_fetch_add(&s_draw_indexed_count, 1);
    }
    atomic_fetch_add(&s_draw_translated_count, 1);
}

uint64_t pgraph_mtl_draw_translated_count(void)
{
    return atomic_load(&s_draw_translated_count);
}

uint64_t pgraph_mtl_draw_pipeline_fallback_count(void)
{
    return atomic_load(&s_draw_pipeline_fallback_count);
}

extern "C" void pgraph_mtl_draw_inc_pipeline_fallback_count(void)
{
    atomic_fetch_add(&s_draw_pipeline_fallback_count, 1);
}

/* M5.5+: coalescing counters. Surfaced to extract-perf-summary.sh
 * via util/xemu-metal-perf.c. */
extern "C" uint64_t pgraph_mtl_draw_pass_opens_count(void)
{
    return atomic_load(&s_open_pass_opens);
}
extern "C" uint64_t pgraph_mtl_draw_pass_coalesced_count(void)
{
    return atomic_load(&s_open_pass_coalesced);
}
extern "C" uint64_t pgraph_mtl_draw_pass_flushes_count(void)
{
    return atomic_load(&s_open_pass_flushes);
}
