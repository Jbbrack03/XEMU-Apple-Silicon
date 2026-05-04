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
#include "uniform.h"
#include "vertex.h"  /* MtlAttributeStream + MTL_ATTR_BUFFER_INDEX_BASE */

#include "hw/xbox/nv2a/nv2a_regs.h"

#include <stdatomic.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* W4 (2026-05-04): per-draw color RT dump (XEMU_METAL_DUMP_DRAW_RT).
 * fpng.h pulls in <vector>; libc++'s <atomic> conflicts with the C11
 * <stdatomic.h> we already need. Forward-declare the two FPNG entry
 * points by hand — same pattern as ui/xemu-metal.mm's screenshot path.
 */
namespace fpng {
    void fpng_init();
    bool fpng_encode_image_to_file(const char *pFilename,
                                   const void *pImage,
                                   uint32_t w,
                                   uint32_t h,
                                   uint32_t num_chans,
                                   uint32_t flags);
}

extern "C" void *xemu_metal_get_device(void);

/* M8: gate the draw command buffer on the latest texture upload by
 * encoding a wait on the shared upload fence. Cheap on Apple Silicon
 * and serves as the GPU-side replacement for the M6
 * `[cb waitUntilCompleted]` in mtl/texture.mm. Skipped when the
 * fence value is 0 — no upload has yet signaled, so there is
 * nothing to wait on. */
static inline void mtl_draw_wait_upload_fence(id<MTLCommandBuffer> cb,
                                              uint64_t value)
{
    void *event_handle = pgraph_mtl_texture_get_upload_fence_event();
    if (event_handle == NULL) {
        return;
    }
    if (value == 0) {
        return;
    }
    id<MTLEvent> ev = (__bridge id<MTLEvent>)event_handle;
    [cb encodeWaitForEvent:ev value:value];
}

static id<MTLDevice>       s_device;
static id<MTLCommandQueue> s_draw_queue;
static bool                s_initialized = false;

/* M5.10 (2026-05-03): cross-queue MTLSharedEvent fence.
 *
 * The Metal renderer uses two distinct command queues: `s_draw_queue`
 * for render-encoder draws (this file) and `s_render_queue` in
 * surface.mm for blit-encoder uploads / downloads / clears. Cross-queue
 * `commit` ordering is NOT synchronous — a blit-encoder copy on
 * `s_render_queue` that reads a draw-target texture written on
 * `s_draw_queue` can race the still-pending draw commit unless an
 * explicit fence forces wait-before-read ordering.
 *
 * `s_draw_done_event` is signaled with monotonically increasing values
 * after every draw command-buffer commit (inside open_pass_close_locked).
 * Cross-queue readers fetch the latest value via
 * pgraph_mtl_draw_get_done_event_state and `encodeWaitForEvent:` on
 * their own command buffer to ensure prior draws have completed
 * before the cross-queue read fires.
 *
 * MTLSharedEvent (not MTLEvent) is required because cross-queue waits
 * are implemented internally as cross-process signals; the Shared
 * variant is the supported pattern per the Metal docs. */
static id<MTLSharedEvent> s_draw_done_event = nil;
static _Atomic(uint64_t)  s_draw_done_value = 0;
static _Atomic(uint64_t)   s_draw_count = 0;
static _Atomic(uint64_t)   s_draw_indexed_count = 0;
static _Atomic(uint64_t)   s_draw_native_tri_depth_count = 0;
static _Atomic(uint64_t)   s_draw_native_quad_count = 0;
static _Atomic(uint64_t)   s_draw_translated_count = 0;
static _Atomic(uint64_t)   s_draw_pipeline_fallback_count = 0;
static _Atomic(uint64_t)   s_draw_encode_us_total = 0;
static _Atomic(uint64_t)   s_open_pass_flush_us_total = 0;

/* W4 (2026-05-04): per-draw color RT dump. See draw.h for env-var
 * semantics. The dump is gated on `s_dump_enabled` (loaded once at
 * init); when off the entire after-flush_draw helper short-circuits
 * to one global load + branch. The cumulative draw counter
 * (`s_dump_draw_index`) is bumped UNCONDITIONALLY when init parsed a
 * valid config; this matches Mesa/RADV debug-dump semantics where the
 * draw index is the cumulative-per-RUN count irrespective of whether
 * the current draw is in range. */
static bool        s_dump_enabled = false;
static uint64_t    s_dump_start = 0;
static uint64_t    s_dump_end = 0;
static char       *s_dump_prefix = NULL;
static _Atomic(uint64_t) s_dump_draw_index = 0;
static _Atomic(uint64_t) s_dump_log_emitted = 0;
static _Atomic(uint64_t) s_dump_count = 0;
static bool        s_dump_fpng_inited = false;

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

typedef struct MtlDepthStencilCacheEntry {
    uint32_t control_0;
    uint32_t control_1;
    uint32_t control_2;
    bool     has_depth_attachment;
    bool     has_stencil_attachment;
    id<MTLDepthStencilState> state;
} MtlDepthStencilCacheEntry;

static MtlDepthStencilCacheEntry s_depth_stencil_cache[64];
static unsigned int s_depth_stencil_cache_count = 0;

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
static uint64_t s_open_upload_fence_value = 0;
static bool s_open_buffer_frame_active = false;

/* Per-interval coalescing telemetry (surfaced via accessors at file
 * end; xemu-metal-perf.c emits as METAL_PASS_OPENS / _COALESCED). */
static _Atomic(uint64_t) s_open_pass_opens     = 0;
static _Atomic(uint64_t) s_open_pass_coalesced = 0;
static _Atomic(uint64_t) s_open_pass_flushes   = 0;
static bool s_wait_for_open_pass_close = false;

static void argb_pack32_to_rgba_float(uint32_t argb, float rgba[4])
{
    rgba[0] = (float)((argb >> 16) & 0xff) / 255.0f;
    rgba[1] = (float)((argb >> 8) & 0xff) / 255.0f;
    rgba[2] = (float)(argb & 0xff) / 255.0f;
    rgba[3] = (float)((argb >> 24) & 0xff) / 255.0f;
}

static inline uint32_t mtl_get_mask(uint32_t v, uint32_t mask)
{
    return mask ? ((v & mask) >> __builtin_ctz(mask)) : 0;
}

static bool mtl_env_flag_enabled(const char *name)
{
    const char *e = getenv(name);
    return e != NULL && e[0] != '\0' && e[0] != '0';
}

static const MTLCompareFunction pgraph_compare_mtl_map[8] = {
    MTLCompareFunctionNever,
    MTLCompareFunctionLess,
    MTLCompareFunctionEqual,
    MTLCompareFunctionLessEqual,
    MTLCompareFunctionGreater,
    MTLCompareFunctionNotEqual,
    MTLCompareFunctionGreaterEqual,
    MTLCompareFunctionAlways,
};

static const MTLStencilOperation pgraph_stencil_op_mtl_map[9] = {
    MTLStencilOperationKeep,
    MTLStencilOperationKeep,
    MTLStencilOperationZero,
    MTLStencilOperationReplace,
    MTLStencilOperationIncrementClamp,
    MTLStencilOperationDecrementClamp,
    MTLStencilOperationInvert,
    MTLStencilOperationIncrementWrap,
    MTLStencilOperationDecrementWrap,
};

static const MTLCullMode pgraph_cull_mode_mtl_map[4] = {
    MTLCullModeNone,
    MTLCullModeFront,
    MTLCullModeBack,
    MTLCullModeBack,
};

static bool mtl_format_has_stencil(uint32_t depth_fmt)
{
    MTLPixelFormat fmt = (MTLPixelFormat)depth_fmt;
    return fmt == MTLPixelFormatDepth24Unorm_Stencil8 ||
           fmt == MTLPixelFormatDepth32Float_Stencil8 ||
           fmt == MTLPixelFormatStencil8;
}

static id<MTLDepthStencilState>
get_depth_stencil_state(uint32_t control_0, uint32_t control_1,
                        uint32_t control_2, bool has_depth_attachment,
                        bool has_stencil_attachment)
{
    for (unsigned int i = 0; i < s_depth_stencil_cache_count; i++) {
        MtlDepthStencilCacheEntry *e = &s_depth_stencil_cache[i];
        if (e->control_0 == control_0 &&
            e->control_1 == control_1 &&
            e->control_2 == control_2 &&
            e->has_depth_attachment == has_depth_attachment &&
            e->has_stencil_attachment == has_stencil_attachment) {
            return e->state;
        }
    }

    bool force_no_depth_stencil =
        mtl_env_flag_enabled("XEMU_METAL_DEBUG_DISABLE_DEPTH_STENCIL");
    bool depth_test =
        has_depth_attachment &&
        !force_no_depth_stencil &&
        (control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) != 0;
    bool depth_write =
        has_depth_attachment &&
        !force_no_depth_stencil &&
        (control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE) != 0;
    bool stencil_test =
        has_stencil_attachment &&
        !force_no_depth_stencil &&
        (control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE) != 0;

    MTLDepthStencilDescriptor *desc = [MTLDepthStencilDescriptor new];
    desc.depthCompareFunction = MTLCompareFunctionAlways;
    desc.depthWriteEnabled = depth_write ? YES : NO;
    if (depth_test) {
        uint32_t func =
            mtl_get_mask(control_0, NV_PGRAPH_CONTROL_0_ZFUNC) & 7u;
        desc.depthCompareFunction = pgraph_compare_mtl_map[func];
    }

    if (stencil_test) {
        uint32_t func =
            mtl_get_mask(control_1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC) & 7u;
        uint32_t op_fail =
            mtl_get_mask(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail =
            mtl_get_mask(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass =
            mtl_get_mask(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
        stencil.stencilCompareFunction = pgraph_compare_mtl_map[func];
        stencil.stencilFailureOperation =
            pgraph_stencil_op_mtl_map[op_fail < 9 ? op_fail : 1];
        stencil.depthFailureOperation =
            pgraph_stencil_op_mtl_map[op_zfail < 9 ? op_zfail : 1];
        stencil.depthStencilPassOperation =
            pgraph_stencil_op_mtl_map[op_zpass < 9 ? op_zpass : 1];
        stencil.readMask =
            mtl_get_mask(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        stencil.writeMask =
            mtl_get_mask(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        desc.frontFaceStencil = stencil;
        desc.backFaceStencil = stencil;
    }

    id<MTLDepthStencilState> state =
        [s_device newDepthStencilStateWithDescriptor:desc];
    if (state == nil) {
        return nil;
    }

    MtlDepthStencilCacheEntry *slot = NULL;
    if (s_depth_stencil_cache_count <
        sizeof(s_depth_stencil_cache) / sizeof(s_depth_stencil_cache[0])) {
        slot = &s_depth_stencil_cache[s_depth_stencil_cache_count++];
    } else {
        slot = &s_depth_stencil_cache[0];
    }
    slot->control_0 = control_0;
    slot->control_1 = control_1;
    slot->control_2 = control_2;
    slot->has_depth_attachment = has_depth_attachment;
    slot->has_stencil_attachment = has_stencil_attachment;
    slot->state = state;
    return state;
}

static void apply_translated_raster_state(id<MTLRenderCommandEncoder> enc,
                                          uint32_t viewport_w,
                                          uint32_t viewport_h,
                                          uint32_t depth_fmt,
                                          void *surface_depth,
                                          uint32_t control_0,
                                          uint32_t control_1,
                                          uint32_t control_2,
                                          uint32_t setup_raster,
                                          uint32_t scissor_x,
                                          uint32_t scissor_y,
                                          uint32_t scissor_w,
                                          uint32_t scissor_h)
{
    if (!mtl_env_flag_enabled("XEMU_METAL_DEBUG_DISABLE_CULL") &&
        (setup_raster & NV_PGRAPH_SETUPRASTER_CULLENABLE) != 0) {
        uint32_t cull =
            mtl_get_mask(setup_raster, NV_PGRAPH_SETUPRASTER_CULLCTRL);
        [enc setCullMode:pgraph_cull_mode_mtl_map[cull < 4 ? cull : 0]];
    } else {
        [enc setCullMode:MTLCullModeNone];
    }

    /* Match GL's effective winding for the generated vertex shaders. The
     * Metal SPIR-V->MSL path fixes depth convention, but it does not flip
     * clip-space Y, so using Vulkan's front-face mapping culls the boot
     * animation's translucent glow quads. */
    [enc setFrontFacingWinding:
        (setup_raster & NV_PGRAPH_SETUPRASTER_FRONTFACE)
            ? MTLWindingClockwise
            : MTLWindingCounterClockwise];

    bool has_depth = surface_depth != NULL;
    bool has_stencil = has_depth && mtl_format_has_stencil(depth_fmt);
    id<MTLDepthStencilState> ds =
        get_depth_stencil_state(control_0, control_1, control_2,
                                has_depth, has_stencil);
    if (ds != nil) {
        [enc setDepthStencilState:ds];
    }
    if (has_stencil) {
        uint32_t ref =
            mtl_get_mask(control_1, NV_PGRAPH_CONTROL_1_STENCIL_REF);
        [enc setStencilReferenceValue:ref];
    }

    if (mtl_env_flag_enabled("XEMU_METAL_DEBUG_DISABLE_SCISSOR") ||
        scissor_w == 0 || scissor_h == 0) {
        scissor_x = 0;
        scissor_y = 0;
        scissor_w = viewport_w;
        scissor_h = viewport_h;
    }
    if (scissor_x > viewport_w) scissor_x = viewport_w;
    if (scissor_y > viewport_h) scissor_y = viewport_h;
    if (scissor_x + scissor_w > viewport_w) {
        scissor_w = viewport_w - scissor_x;
    }
    if (scissor_y + scissor_h > viewport_h) {
        scissor_h = viewport_h - scissor_y;
    }
    if (scissor_w == 0) scissor_w = 1;
    if (scissor_h == 0) scissor_h = 1;

    MTLScissorRect scissor = {
        .x = (NSUInteger)scissor_x,
        .y = (NSUInteger)scissor_y,
        .width = (NSUInteger)scissor_w,
        .height = (NSUInteger)scissor_h,
    };
    [enc setScissorRect:scissor];
}

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

    /* M5.10: shared event for cross-queue draw-completion ordering.
     * If event creation fails the fence becomes a no-op; the renderer
     * still works but cross-queue reads of draw targets may race the
     * pending draws. This is unlikely on modern Apple Silicon. */
    s_draw_done_event = [s_device newSharedEvent];
    if (s_draw_done_event == nil) {
        fprintf(stderr,
                "pgraph_mtl_draw_init: newSharedEvent failed; "
                "cross-queue download fence is disabled\n");
    } else {
        s_draw_done_event.label = @"xemu.metal.draw_done";
    }
    atomic_store(&s_draw_done_value, (uint64_t)0);

    atomic_store(&s_draw_count, (uint64_t)0);
    atomic_store(&s_draw_indexed_count, (uint64_t)0);
    atomic_store(&s_draw_native_tri_depth_count, (uint64_t)0);
    atomic_store(&s_draw_native_quad_count, (uint64_t)0);
    atomic_store(&s_draw_translated_count, (uint64_t)0);
    atomic_store(&s_draw_pipeline_fallback_count, (uint64_t)0);
    atomic_store(&s_draw_encode_us_total, (uint64_t)0);
    atomic_store(&s_open_pass_flush_us_total, (uint64_t)0);
    atomic_store(&s_open_pass_opens, (uint64_t)0);
    atomic_store(&s_open_pass_coalesced, (uint64_t)0);
    atomic_store(&s_open_pass_flushes, (uint64_t)0);
    s_depth_stencil_cache_count = 0;
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
    s_wait_for_open_pass_close = true;
    pgraph_mtl_draw_flush_open_pass();
    s_wait_for_open_pass_close = false;
    for (unsigned int i = 0; i < s_depth_stencil_cache_count; i++) {
        s_depth_stencil_cache[i].state = nil;
    }
    s_depth_stencil_cache_count = 0;
    s_draw_done_event = nil;
    s_draw_queue = nil;
    s_device = nil;
    s_initialized = false;
}

/* M5.10: open-pass texture accessors. Used by the surface cache's
 * eviction / shape-mismatch destroy paths to skip / drain when the
 * candidate eviction texture is still referenced by the open render
 * encoder. NULL out parameters mean "no pass open" (key is zeroed).
 *
 * Reads from s_open_pass_key are unsynchronized but the renderer-
 * thread invariant (caller holds pgraph.lock during cache ops) is
 * the same as the writer invariant — no race. */
extern "C" void pgraph_mtl_draw_get_open_pass_textures(void **out_color,
                                                       void **out_depth)
{
    if (out_color) {
        *out_color = s_open_enc != nil ? s_open_pass_key.color_tex : NULL;
    }
    if (out_depth) {
        *out_depth = s_open_enc != nil ? s_open_pass_key.depth_tex : NULL;
    }
}

extern "C" void pgraph_mtl_draw_get_done_event_state(void **out_event,
                                                     uint64_t *out_value)
{
    if (out_event) {
        *out_event = (__bridge void *)s_draw_done_event;
    }
    if (out_value) {
        *out_value = atomic_load(&s_draw_done_value);
    }
}

/* -------- shared encode setup -------- */

static MTLRenderPassDescriptor *
build_render_pass_descriptor(void *surface_color, void *surface_depth,
                             uint32_t depth_fmt)
{
    MTLRenderPassDescriptor *desc =
        [MTLRenderPassDescriptor renderPassDescriptor];

    /* M11: query the MSAA state once. When the surface manager has a
     * multisample companion bound for the active color/depth target,
     * the render pass's `texture` becomes the multisample companion and
     * the single-sample target becomes the `resolveTexture`. Color uses
     * StoreAndMultisampleResolve so the MSAA contents survive pass
     * breaks while the resolved single-sample copy stays current for
     * subsequent passes, RTT sampling, and present. Depth/stencil must
     * also be stored because later passes load the same MSAA attachment. */
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
            /* Keep the multisample attachment alive across coalesced-pass
             * breaks and also update the single-sample resolve target for
             * present / RTT sampling. MTLStoreActionMultisampleResolve only
             * guarantees the resolveTexture is written; the MSAA texture may
             * be discarded, so a later MTLLoadActionLoad can see stale black. */
            desc.colorAttachments[0].loadAction  = MTLLoadActionLoad;
            desc.colorAttachments[0].storeAction =
                MTLStoreActionStoreAndMultisampleResolve;
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
            desc.depthAttachment.storeAction = MTLStoreActionStore;
            if (dfmt == MTLPixelFormatDepth32Float_Stencil8 ||
                dfmt == MTLPixelFormatDepth24Unorm_Stencil8 ||
                dfmt == MTLPixelFormatStencil8) {
                desc.stencilAttachment.texture     = ms;
                desc.stencilAttachment.loadAction  = MTLLoadActionLoad;
                desc.stencilAttachment.storeAction = MTLStoreActionStore;
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
        /* M5.10: signal the cross-queue fence so render-queue blits
         * (surface downloads, blit_copy reads of draw-target textures)
         * can `encodeWaitForEvent:` on this monotonically increasing
         * value to ensure the draw is queued for completion before
         * their copy reads start.
         *
         * The signal is appended to the command buffer BEFORE commit;
         * Metal guarantees the encoded signal fires in submission order
         * after the prior render-encoder work for this command buffer.
         */
        if (s_draw_done_event != nil) {
            uint64_t v = atomic_fetch_add(&s_draw_done_value, 1) + 1;
            [s_open_cmd encodeSignalEvent:s_draw_done_event value:v];
        }
        [s_open_cmd commit];
        if (s_wait_for_open_pass_close) {
            [s_open_cmd waitUntilCompleted];
        }
        s_open_cmd = nil;
    }
    if (s_open_buffer_frame_active) {
        pgraph_mtl_buffer_end_frame();
        s_open_buffer_frame_active = false;
    }
    memset(&s_open_pass_key, 0, sizeof(s_open_pass_key));
    s_open_upload_fence_value = 0;
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
    uint64_t upload_fence_value = pgraph_mtl_texture_get_upload_fence_value();
    if (open_pass_matches(color_tex, depth_tex, color_fmt, depth_fmt,
                          sample_count) &&
        s_open_upload_fence_value == upload_fence_value) {
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
        mtl_draw_wait_upload_fence(s_open_cmd, upload_fence_value);
        s_open_upload_fence_value = upload_fence_value;

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
    int64_t start_us = mtl_now_us();
    if (s_open_enc != nil || s_open_cmd != nil ||
        s_open_buffer_frame_active) {
        atomic_fetch_add(&s_open_pass_flushes, 1);
    }
    open_pass_close_locked();
    mtl_add_elapsed_us(&s_open_pass_flush_us_total, start_us);
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

/* -------- M7.1 / M5.8: translated-pipeline encode -------- */

void pgraph_mtl_draw_translated(void *pipeline_state,
                                const MtlAttributeStream *attr_streams,
                                unsigned int n_attr_streams,
                                unsigned int vertex_count,
                                const uint32_t *indices,
                                unsigned int index_count,
                                uint32_t mtl_primitive,
                                unsigned int viewport_w,
                                unsigned int viewport_h,
                                void *surface_color,
                                void *surface_depth,
                                uint32_t depth_fmt,
                                uint32_t blend_color,
                                uint32_t control_0,
                                uint32_t control_1,
                                uint32_t control_2,
                                uint32_t setup_raster,
                                uint32_t scissor_x,
                                uint32_t scissor_y,
                                uint32_t scissor_w,
                                uint32_t scissor_h,
                                void *vsh_ubo,
                                size_t vsh_ubo_offset,
                                size_t vsh_ubo_size,
                                void *psh_ubo,
                                size_t psh_ubo_offset,
                                size_t psh_ubo_size,
                                void *const stage_textures[4],
                                void *const stage_samplers[4])
{
    int64_t start_us = mtl_now_us();
    if (!s_initialized || pipeline_state == NULL ||
        vertex_count == 0 || attr_streams == NULL || n_attr_streams == 0) {
        return;
    }
    if (surface_color == NULL && surface_depth == NULL) {
        return;
    }
    bool indexed = (indices != NULL && index_count > 0);

    /* Slot 0 (POSITION) is required — without per-vertex positions
     * there's nothing to draw. Bail if the caller didn't supply it. */
    if (attr_streams[0].data == NULL) {
        return;
    }

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

    /* Stage every active attribute stream into the ring; record the
     * resulting (buffer, offset) for binding below. */
    void   *attr_bufs[MTL_VERTEX_NUM_ATTRIBUTES];
    size_t  attr_offs[MTL_VERTEX_NUM_ATTRIBUTES];
    for (unsigned i = 0; i < MTL_VERTEX_NUM_ATTRIBUTES; i++) {
        attr_bufs[i] = NULL;
        attr_offs[i] = 0;
    }
    unsigned int max_streams = (n_attr_streams < MTL_VERTEX_NUM_ATTRIBUTES)
                                   ? n_attr_streams
                                   : MTL_VERTEX_NUM_ATTRIBUTES;
    for (unsigned i = 0; i < max_streams; i++) {
        if (attr_streams[i].data == NULL || attr_streams[i].bytes == 0) {
            continue;
        }
        if (!pgraph_mtl_buffer_stage_vertex(attr_streams[i].data,
                                            attr_streams[i].bytes,
                                            &attr_bufs[i], &attr_offs[i])) {
            return;
        }
    }

    void *idx_buf = NULL;
    size_t idx_off = 0;
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
    apply_translated_raster_state(enc, viewport_w, viewport_h,
                                  depth_fmt, surface_depth,
                                  control_0, control_1, control_2,
                                  setup_raster,
                                  scissor_x, scissor_y,
                                  scissor_w, scissor_h);
    float blend_rgba[4];
    argb_pack32_to_rgba_float(blend_color, blend_rgba);
    [enc setBlendColorRed:blend_rgba[0]
                    green:blend_rgba[1]
                     blue:blend_rgba[2]
                    alpha:blend_rgba[3]];

    MTLViewport vp = (MTLViewport){
        .originX = 0.0,
        .originY = 0.0,
        .width   = (double)viewport_w,
        .height  = (double)viewport_h,
        .znear   = 0.0,
        .zfar    = 1.0,
    };
    [enc setViewport:vp];

    /* M5.8 buffer-index layout:
     *   - Vertex stage `[[buffer(0)]]` = VSH UBO (spirv-cross emits
     *     `constant VshUniforms& _NN [[buffer(0)]]` because the GLSL
     *     generator declares the UBO with `layout(binding=0)`).
     *   - Vertex attribute streams = bufferIndex
     *     (MTL_ATTR_BUFFER_INDEX_BASE + slot) = [1..16]. State.c sets
     *     vd.attributes[N].bufferIndex to the same value; the
     *     vertex-stage buffer table treats the vertex descriptor's
     *     bufferIndex slots and `[[buffer(N)]]` as the same namespace,
     *     so the offset MUST start past the UBO.
     *   - Fragment stage `[[buffer(1)]]` = PSH UBO.
     */
    if (vsh_ubo != NULL && vsh_ubo_size > 0) {
        id<MTLBuffer> ub = (__bridge id<MTLBuffer>)vsh_ubo;
        [enc setVertexBuffer:ub offset:vsh_ubo_offset atIndex:0];
    }
    for (unsigned i = 0; i < max_streams; i++) {
        if (attr_bufs[i] == NULL) {
            continue;
        }
        unsigned int bi = MTL_ATTR_BUFFER_INDEX_BASE + i;
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)attr_bufs[i];
        [enc setVertexBuffer:b offset:attr_offs[i] atIndex:bi];
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
                [enc setFragmentTexture:t
                                atIndex:PGRAPH_MTL_PSH_TEX_BINDING_INDEX + i];
            }
        }
    }
    if (stage_samplers != NULL) {
        for (int i = 0; i < 4; i++) {
            if (stage_samplers[i] != NULL) {
                id<MTLSamplerState> ss =
                    (__bridge id<MTLSamplerState>)stage_samplers[i];
                [enc setFragmentSamplerState:ss
                                      atIndex:PGRAPH_MTL_PSH_TEX_BINDING_INDEX + i];
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

    bool color_write =
        (control_0 & (NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)) != 0;
    pgraph_mtl_surface_note_color_draw(surface_color, color_write);
    atomic_fetch_add(&s_draw_count, 1);
    if (indexed) {
        atomic_fetch_add(&s_draw_indexed_count, 1);
    }
    atomic_fetch_add(&s_draw_translated_count, 1);
    mtl_add_elapsed_us(&s_draw_encode_us_total, start_us);
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
extern "C" uint64_t pgraph_mtl_draw_encode_us_total(void)
{
    return atomic_load(&s_draw_encode_us_total);
}
extern "C" uint64_t pgraph_mtl_draw_open_pass_flush_us_total(void)
{
    return atomic_load(&s_open_pass_flush_us_total);
}

/* W4 (2026-05-04): parse XEMU_METAL_DUMP_DRAW_RT once. Format
 * `START:END:PREFIX`. Anything malformed leaves the dump disabled
 * with zero hot-path cost (one global load + branch). */
extern "C" void pgraph_mtl_draw_dump_rt_init(void)
{
    s_dump_enabled = false;
    if (s_dump_prefix != NULL) {
        free(s_dump_prefix);
        s_dump_prefix = NULL;
    }
    atomic_store(&s_dump_draw_index, (uint64_t)0);
    atomic_store(&s_dump_log_emitted, (uint64_t)0);

    const char *env = getenv("XEMU_METAL_DUMP_DRAW_RT");
    if (env == NULL || env[0] == '\0') {
        return;
    }
    const char *first_colon = strchr(env, ':');
    if (first_colon == NULL || first_colon == env) {
        fprintf(stderr,
                "xemu-perf: metal_dump_draw_rt malformed (missing first ':') "
                "value=%s\n",
                env);
        return;
    }
    const char *second_colon = strchr(first_colon + 1, ':');
    if (second_colon == NULL || second_colon == first_colon + 1 ||
        second_colon[1] == '\0') {
        fprintf(stderr,
                "xemu-perf: metal_dump_draw_rt malformed (missing second "
                "':' or empty prefix) value=%s\n",
                env);
        return;
    }

    char *endp = NULL;
    unsigned long long start = strtoull(env, &endp, 10);
    if (endp != first_colon) {
        fprintf(stderr,
                "xemu-perf: metal_dump_draw_rt START is not a number "
                "value=%s\n",
                env);
        return;
    }
    unsigned long long end = strtoull(first_colon + 1, &endp, 10);
    if (endp != second_colon) {
        fprintf(stderr,
                "xemu-perf: metal_dump_draw_rt END is not a number "
                "value=%s\n",
                env);
        return;
    }
    if (end < start) {
        fprintf(stderr,
                "xemu-perf: metal_dump_draw_rt END(%llu) < START(%llu); "
                "disabled\n",
                end, start);
        return;
    }
    s_dump_start = (uint64_t)start;
    s_dump_end = (uint64_t)end;
    s_dump_prefix = strdup(second_colon + 1);
    if (s_dump_prefix == NULL) {
        return;
    }
    s_dump_enabled = true;
    fprintf(stderr,
            "xemu-perf: metal_dump_draw_rt enabled start=%llu end=%llu "
            "prefix=%s\n",
            (unsigned long long)s_dump_start,
            (unsigned long long)s_dump_end,
            s_dump_prefix);
}

/* W4 (2026-05-04): swap BGRA → RGBA in place. FPNG expects RGBA byte
 * order; Metal color RTs are typically BGRA8Unorm / BGRA8Unorm_sRGB on
 * Apple Silicon. Per-pixel scalar swap — runs once-per-dump on the
 * cmdbuf completion handler off the renderer thread. */
static void mtl_dump_swap_bgra_to_rgba(uint8_t *p, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t b = p[i * 4 + 0];
        uint8_t r = p[i * 4 + 2];
        p[i * 4 + 0] = r;
        p[i * 4 + 2] = b;
    }
}

/* W4 (2026-05-04): the post-flush_draw hook.
 *
 * Bumps the cumulative-per-RUN draw index unconditionally when a parsed
 * config is active. When the bumped index is in [start, end] and a
 * non-NULL color RT was supplied, opens a blit encoder, copies the
 * texture into a host-shared MTLBuffer, and queues an
 * addCompletedHandler that BGRA→RGBA swaps and writes the PNG via
 * FPNG. Errors are logged once and swallowed.
 *
 * Caller contract: invoke AFTER pgraph_mtl_draw_flush_open_pass() so
 * the post-MSAA-resolve color texture is what the blit reads, NOT the
 * multisample companion.
 */
extern "C" void pgraph_mtl_draw_dump_rt_after_flush_draw(void *color_texture)
{
    if (!s_dump_enabled) {
        return;
    }

    /* 0-indexed; pre-increment so the first flush_draw is index 0. */
    uint64_t idx = atomic_fetch_add(&s_dump_draw_index, 1);
    if (idx < s_dump_start || idx > s_dump_end) {
        return;
    }
    if (color_texture == NULL || s_device == nil || s_draw_queue == nil) {
        return;
    }

    @autoreleasepool {
        id<MTLTexture> tex = (__bridge id<MTLTexture>)color_texture;
        NSUInteger w = tex.width;
        NSUInteger h = tex.height;
        if (w == 0 || h == 0) {
            return;
        }
        size_t bytes_per_row = (size_t)w * 4u;
        size_t total_bytes   = bytes_per_row * (size_t)h;

        id<MTLBuffer> readback =
            [s_device newBufferWithLength:total_bytes
                                  options:MTLResourceStorageModeShared];
        if (readback == nil) {
            fprintf(stderr,
                    "xemu-perf: metal_draw_rt_dump readback alloc failed "
                    "idx=%llu size=%zu\n",
                    (unsigned long long)idx, total_bytes);
            return;
        }
        readback.label = @"xemu.metal.draw_rt_dump_readback";

        id<MTLCommandBuffer> cmd = [s_draw_queue commandBuffer];
        cmd.label = @"xemu.metal.draw_rt_dump";
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        blit.label = @"xemu.metal.draw_rt_dump_blit";
        [blit copyFromTexture:tex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                     toBuffer:readback
            destinationOffset:0
       destinationBytesPerRow:bytes_per_row
     destinationBytesPerImage:total_bytes];
        [blit endEncoding];

        /* Build "<prefix>.<idx_padded6>.png". */
        size_t plen = strlen(s_dump_prefix);
        size_t fname_len = plen + 32;
        char *fname = (char *)malloc(fname_len);
        if (fname == NULL) {
            return;
        }
        snprintf(fname, fname_len, "%s.%06llu.png",
                 s_dump_prefix, (unsigned long long)idx);

        uint32_t shot_w = (uint32_t)w;
        uint32_t shot_h = (uint32_t)h;
        uint64_t shot_idx = idx;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> /*cb*/) {
            uint8_t *bytes = (uint8_t *)[readback contents];
            if (bytes != NULL && fname != NULL) {
                if (!s_dump_fpng_inited) {
                    fpng::fpng_init();
                    s_dump_fpng_inited = true;
                }
                mtl_dump_swap_bgra_to_rgba(bytes,
                                           (size_t)shot_w * (size_t)shot_h);
                bool ok = fpng::fpng_encode_image_to_file(
                    fname, bytes, shot_w, shot_h, 4, 0);
                if (ok) {
                    atomic_fetch_add(&s_dump_count, 1);
                    /* Rate-limit the console line: first 5 dumps emit a
                     * one-line confirmation; further dumps land silently
                     * (METAL_DRAW_RT_DUMPS reflects the running total). */
                    uint64_t n =
                        atomic_fetch_add(&s_dump_log_emitted, 1) + 1;
                    if (n <= 5) {
                        fprintf(stderr,
                                "xemu-perf: metal_draw_rt_dump idx=%llu "
                                "path=%s w=%u h=%u\n",
                                (unsigned long long)shot_idx,
                                fname,
                                (unsigned)shot_w,
                                (unsigned)shot_h);
                    }
                } else {
                    fprintf(stderr,
                            "xemu-perf: metal_draw_rt_dump fpng_encode "
                            "failed idx=%llu path=%s\n",
                            (unsigned long long)shot_idx, fname);
                }
            }
            if (fname != NULL) {
                free(fname);
            }
        }];
        [cmd commit];
    }
}

extern "C" uint64_t pgraph_mtl_draw_rt_dumps_count(void)
{
    return atomic_load(&s_dump_count);
}
