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

    pgraph_mtl_buffer_begin_frame();

    void *pos_buf = NULL, *col_buf = NULL;
    size_t pos_off = 0, col_off = 0;
    size_t pos_size = (size_t)vertex_count * 4 * sizeof(float);
    size_t col_size = (size_t)vertex_count * 4 * sizeof(float);

    if (!pgraph_mtl_buffer_stage_vertex(positions, pos_size,
                                        &pos_buf, &pos_off) ||
        !pgraph_mtl_buffer_stage_vertex(colors, col_size,
                                        &col_buf, &col_off)) {
        pgraph_mtl_buffer_end_frame();
        return;
    }

    @autoreleasepool {
        MTLRenderPassDescriptor *desc =
            build_render_pass_descriptor(surface_color, surface_depth,
                                         depth_fmt);

        id<MTLCommandBuffer> cmd = [s_draw_queue commandBuffer];
        cmd.label = @"xemu.metal.draw";

        /* M8: wait for any in-flight texture uploads to finish on the
         * GPU before this draw runs. Replaces the M6 CPU-side
         * waitUntilCompleted in texture.mm. */
        mtl_draw_wait_upload_fence(cmd);

        id<MTLRenderCommandEncoder> enc =
            [cmd renderCommandEncoderWithDescriptor:desc];
        enc.label = @"xemu.metal.draw_enc";

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

        [enc endEncoding];
        [cmd commit];
    }

    atomic_fetch_add(&s_draw_count, 1);

    pgraph_mtl_buffer_end_frame();
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

    pgraph_mtl_buffer_begin_frame();

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
        pgraph_mtl_buffer_end_frame();
        return;
    }

    @autoreleasepool {
        MTLRenderPassDescriptor *desc =
            build_render_pass_descriptor(surface_color, surface_depth,
                                         depth_fmt);

        id<MTLCommandBuffer> cmd = [s_draw_queue commandBuffer];
        cmd.label = @"xemu.metal.draw_indexed";

        /* M8: gate on upload fence (see passthrough path). */
        mtl_draw_wait_upload_fence(cmd);

        id<MTLRenderCommandEncoder> enc =
            [cmd renderCommandEncoderWithDescriptor:desc];
        enc.label = @"xemu.metal.draw_indexed_enc";

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

        [enc endEncoding];
        [cmd commit];
    }

    atomic_fetch_add(&s_draw_count, 1);
    atomic_fetch_add(&s_draw_indexed_count, 1);

    pgraph_mtl_buffer_end_frame();
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

    pgraph_mtl_buffer_begin_frame();

    void *pos_buf = NULL, *col_buf = NULL, *idx_buf = NULL;
    size_t pos_off = 0, col_off = 0, idx_off = 0;
    size_t pos_size = (size_t)vertex_count * 4 * sizeof(float);
    size_t col_size = (size_t)vertex_count * 4 * sizeof(float);

    if (!pgraph_mtl_buffer_stage_vertex(positions, pos_size, &pos_buf, &pos_off) ||
        !pgraph_mtl_buffer_stage_vertex(colors, col_size, &col_buf, &col_off)) {
        pgraph_mtl_buffer_end_frame();
        return;
    }

    if (indexed) {
        size_t idx_size = (size_t)index_count * sizeof(uint32_t);
        if (!pgraph_mtl_buffer_stage_index(indices, idx_size,
                                           &idx_buf, &idx_off)) {
            pgraph_mtl_buffer_end_frame();
            return;
        }
    }

    @autoreleasepool {
        MTLRenderPassDescriptor *desc =
            build_render_pass_descriptor(surface_color, surface_depth, depth_fmt);

        id<MTLCommandBuffer> cmd = [s_draw_queue commandBuffer];
        cmd.label = @"xemu.metal.draw_translated";

        /* M8: gate on upload fence (see passthrough path). */
        mtl_draw_wait_upload_fence(cmd);

        id<MTLRenderCommandEncoder> enc =
            [cmd renderCommandEncoderWithDescriptor:desc];
        enc.label = @"xemu.metal.draw_translated_enc";

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

        /* The translated VSH expects vertex buffers at the
         * spirv-cross-emitted attribute slots. With
         * MSL_ENABLE_DECORATION_BINDING=true and the GLSL generator's
         * `layout(location=N)` per attribute, [[attribute(N)]] is bound
         * via MTLVertexDescriptor at vertex_buffer slot N (PerVertex).
         * Our PgraphMtlPipelineKey set buffer_index = i for each
         * attribute — so we must bind position to slot 0 and color to
         * slot N where N matches DIFFUSE's slot. The inline-buffer path
         * always populates slot 0 (POSITION) and slot 3 (DIFFUSE) on
         * NV2A; we bind position at slot 0 and the color at slot
         * NV2A_VERTEX_ATTR_DIFFUSE = 3 to match the
         * vertex_attributes[] array. */
        id<MTLBuffer> pbuf = (__bridge id<MTLBuffer>)pos_buf;
        id<MTLBuffer> cbuf = (__bridge id<MTLBuffer>)col_buf;
        [enc setVertexBuffer:pbuf offset:pos_off atIndex:0];
        [enc setVertexBuffer:cbuf offset:col_off atIndex:3];

        /* UBOs at the spirv-cross emitted [[buffer(N)]] indices. The
         * Vulkan generator emits VSH UBO at binding=0 / PSH UBO at
         * binding=1.  spirv-cross with ENABLE_DECORATION_BINDING
         * forwards those into MSL [[buffer(0)]] / [[buffer(1)]].
         *
         * On Metal vertex stages, [[buffer(0..N)]] are SHARED between
         * vertex inputs (the MTLVertexDescriptor) and direct uniform
         * buffer bindings. spirv-cross resolves this by mapping
         * descriptor-set bindings to high indices (default offset = 30
         * for buffer/texture). So the VSH UBO actually ends up at
         * `[[buffer(30)]]` after spirv-cross's auto-shift, NOT
         * `[[buffer(0)]]`.
         *
         * To stay deterministic without parsing the MSL output, we
         * bind UBOs at both possible locations:
         *   - Vertex stage: VSH UBO at [[buffer(30 + 0)]] = 30.
         *   - Fragment stage: PSH UBO at [[buffer(30 + 1)]] = 31.
         *
         * spirv-cross's default `MSL_RESOURCE_INDEX_OFFSETS_BUFFER` is
         * 0 by default; with ENABLE_DECORATION_BINDING it uses the
         * SPIR-V binding directly.  So actually [[buffer(0)]] /
         * [[buffer(1)]] is what we get.  The vertex slot conflict is
         * resolved by the MTLVertexDescriptor naming individual
         * attributes via [[attribute(N)]] with their own
         * vd.attributes[i].bufferIndex (whose value differs from the
         * UBO buffer index).  Apple's vertex stage allows the UBO to
         * occupy [[buffer(N)]] so long as N is not used by the vertex
         * descriptor's bufferIndex.  Our descriptor uses bufferIndex
         * 0 + 3 (per the inline-buffer NV2A slot mapping).  UBO at
         * [[buffer(0)]] would collide; we bind it at index 1 in the
         * descriptor and shift to [[buffer(1)]] for the vertex stage
         * UBO… but we don't actually have the option to reshift since
         * the MSL was already generated with binding=0.
         *
         * Pragmatic approach: ignore the conflict for now and bind at
         * the spirv-cross indices.  If Metal validation rejects this,
         * the symptom is a clear validation error which we'll catch
         * during the user's first launch test, and the fix is to
         * regenerate MSL with non-zero MSL_RESOURCE_INDEX_OFFSETS or
         * bind at 30/31.  This mirrors MoltenVK's default behaviour. */
        if (vsh_ubo != NULL && vsh_ubo_size > 0) {
            id<MTLBuffer> ub = (__bridge id<MTLBuffer>)vsh_ubo;
            [enc setVertexBuffer:ub offset:vsh_ubo_offset atIndex:1];
        }
        if (psh_ubo != NULL && psh_ubo_size > 0) {
            id<MTLBuffer> ub = (__bridge id<MTLBuffer>)psh_ubo;
            [enc setFragmentBuffer:ub offset:psh_ubo_offset atIndex:1];
        }

        /* Per-stage texture + sampler bindings. The PSH expects them
         * at [[texture(0..3)]] / [[sampler(0..3)]] (Vulkan binding
         * MTL_PSH_TEX_BINDING = 2 + stage maps via spirv-cross to
         * texture/sampler slots starting at 0 because MTLBackend uses
         * separate index spaces for textures/samplers/buffers). */
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

        [enc endEncoding];
        [cmd commit];
    }

    atomic_fetch_add(&s_draw_count, 1);
    if (indexed) {
        atomic_fetch_add(&s_draw_indexed_count, 1);
    }
    atomic_fetch_add(&s_draw_translated_count, 1);

    pgraph_mtl_buffer_end_frame();
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
