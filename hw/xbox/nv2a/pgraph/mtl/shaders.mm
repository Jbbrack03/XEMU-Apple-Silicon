/*
 * NV2A PGRAPH Metal renderer — pipeline-build Metal API layer
 * (slice M6; M8 — async pipeline compile + setShouldMaximizeConcurrentCompilation).
 *
 * The .mm side owns the Metal API touchpoints: GLSL→MSL translation,
 * MTLLibrary build, MTLRenderPipelineState build, and resource release.
 * The .c side (shadergen.c) owns the LRU cache machinery and the GLSL
 * generator calls (which need per-target headers).
 *
 * The split exists because qemu/lru.h relies on qemu/queue.h's typeof
 * macros, which aren't portable to C++ / .mm; and because vsh.h/psh.h
 * pull in glib types that don't compile cleanly in C++ via the
 * MString helper inlines. Keeping the LRU + glib-typed code on the .c
 * side is the standard pattern in this codebase (see vk/shaders.c).
 *
 * The .mm-side public surface receives only Metal-flavored primitives
 * (color/depth pixel formats, sample count, vertex descriptor arrays);
 * the full PgraphMtlPipelineKey never crosses the boundary.
 *
 * M8 additions:
 *   - `setShouldMaximizeConcurrentCompilation:YES` on the device at
 *     dispatch-queue init time (guarded by `respondsToSelector:` per
 *     Dolphin's Apple Silicon gotcha).
 *   - A private serial dispatch queue (QoS = utility) for async
 *     GLSL→MSL + library + pipeline build. Multiple compiles in flight
 *     concurrently because Metal's `newRenderPipelineState…` blocks
 *     internally and dispatches its own threads, so even a serial
 *     queue gets parallel compile across CPU cores when
 *     setShouldMaximizeConcurrentCompilation is enabled.
 *   - The queue uses dispatch_async with a refcounted in-flight
 *     counter so finalize can wait for drain before tearing down the
 *     cache.
 *
 * Counters declared in shaders.h are defined here as atomics; the .c
 * side increments them via the *_inc_* extern calls.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "shaders.h"
#include "glsl.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#import <objc/message.h>

extern "C" void *xemu_metal_get_device(void);

/* The C side's completion callback. Runs on the dispatch worker thread
 * — it takes the cache lock internally. M9 adds `combined_msl` so the
 * completion handler can persist the freshly-translated MSL to the
 * disk cache (when the build came from GLSL translation, not from a
 * cache hit). The completion handler takes ownership of `combined_msl`
 * and frees it. */
extern "C" void pgraph_mtl_shaders_async_complete(void *ctx, bool ok,
                                                  void *pipeline_state,
                                                  void *library,
                                                  char *combined_msl);

/* -------- counters -------- */

static _Atomic uint64_t s_hits   = 0;
static _Atomic uint64_t s_miss   = 0;
static _Atomic uint64_t s_failed = 0;
static _Atomic uint64_t s_compile_queued    = 0;
static _Atomic uint64_t s_compile_completed = 0;
static _Atomic uint64_t s_compile_async_failed = 0;

extern "C" uint64_t pgraph_mtl_shaders_pipeline_hits(void)
{
    return atomic_load(&s_hits);
}
extern "C" uint64_t pgraph_mtl_shaders_pipeline_misses(void)
{
    return atomic_load(&s_miss);
}
extern "C" uint64_t pgraph_mtl_shaders_pipeline_failed(void)
{
    return atomic_load(&s_failed);
}
extern "C" uint64_t pgraph_mtl_shaders_compile_queued(void)
{
    return atomic_load(&s_compile_queued);
}
extern "C" uint64_t pgraph_mtl_shaders_compile_completed(void)
{
    return atomic_load(&s_compile_completed);
}
extern "C" uint64_t pgraph_mtl_shaders_compile_async_failed(void)
{
    return atomic_load(&s_compile_async_failed);
}

extern "C" void pgraph_mtl_shaders_inc_hits(void)
{
    atomic_fetch_add(&s_hits, 1);
}
extern "C" void pgraph_mtl_shaders_inc_misses(void)
{
    atomic_fetch_add(&s_miss, 1);
}
extern "C" void pgraph_mtl_shaders_inc_failed(void)
{
    atomic_fetch_add(&s_failed, 1);
}
extern "C" void pgraph_mtl_shaders_inc_compile_queued(void)
{
    atomic_fetch_add(&s_compile_queued, 1);
}
extern "C" void pgraph_mtl_shaders_inc_compile_completed(void)
{
    atomic_fetch_add(&s_compile_completed, 1);
}
extern "C" void pgraph_mtl_shaders_inc_compile_async_failed(void)
{
    atomic_fetch_add(&s_compile_async_failed, 1);
}

/* -------- string helpers -------- */

static char *str_replace(const char *str, const char *needle,
                         const char *repl)
{
    size_t nlen = strlen(needle);
    size_t rlen = strlen(repl);
    size_t cap  = strlen(str) + 1;
    char *out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }
    out[0] = '\0';
    size_t out_len = 0;

    const char *p = str;
    while (*p) {
        const char *m = strstr(p, needle);
        if (!m) {
            size_t tail = strlen(p);
            if (out_len + tail + 1 > cap) {
                cap = out_len + tail + 1;
                char *nb = (char *)realloc(out, cap);
                if (!nb) { free(out); return NULL; }
                out = nb;
            }
            memcpy(out + out_len, p, tail + 1);
            out_len += tail;
            break;
        }
        size_t pre = (size_t)(m - p);
        if (out_len + pre + rlen + 1 > cap) {
            cap = (out_len + pre + rlen + 1) * 2;
            char *nb = (char *)realloc(out, cap);
            if (!nb) { free(out); return NULL; }
            out = nb;
        }
        memcpy(out + out_len, p, pre);
        out_len += pre;
        memcpy(out + out_len, repl, rlen);
        out_len += rlen;
        out[out_len] = '\0';
        p = m + nlen;
    }
    return out;
}

static char *combine_msl(const char *vsh_msl, const char *psh_msl)
{
    const char *p = psh_msl;
    while (*p) {
        if (strncmp(p, "#include", 8) == 0 ||
            strncmp(p, "#pragma",  7) == 0 ||
            strncmp(p, "using namespace", 15) == 0) {
            const char *nl = strchr(p, '\n');
            if (!nl) break;
            p = nl + 1;
            continue;
        }
        if (*p == '\n' || *p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }
        break;
    }

    size_t vlen = strlen(vsh_msl);
    size_t plen = strlen(p);
    char *out = (char *)malloc(vlen + plen + 64);
    if (!out) {
        return NULL;
    }
    snprintf(out, vlen + plen + 64,
             "%s\n/* --- fragment stage --- */\n%s\n", vsh_msl, p);
    return out;
}

static char *build_combined_msl(const char *vsh_msl, const char *psh_msl)
{
    char *vsh_renamed = str_replace(vsh_msl, "main0", "vertex_main0");
    if (!vsh_renamed) {
        return NULL;
    }
    char *psh_renamed = str_replace(psh_msl, "main0", "fragment_main0");
    if (!psh_renamed) {
        free(vsh_renamed);
        return NULL;
    }
    char *combined = combine_msl(vsh_renamed, psh_renamed);
    free(vsh_renamed);
    free(psh_renamed);
    return combined;
}

/* -------- core build helper (shared sync + async) -------- */

/* M9: build_pipeline_internal accepts an optional pre-translated combined
 * MSL string. When non-NULL, the GLSL→SPIR-V→MSL translation step is
 * skipped — used by the disk-cache hit path so cold-launch shaders
 * compile straight from cached MSL.
 *
 * `out_combined_msl`: optional out-pointer; on success returns a malloc'd
 * copy of the combined MSL source the build used (post-rename, post-
 * combine). Caller owns and must free with `free()`. NULL means "don't
 * report it back". Used by the disk-cache save path.
 */
static bool build_pipeline_internal(const char *vsh_glsl,
                                    const char *psh_glsl,
                                    const char *pre_translated_msl,
                                    uint32_t color_format,
                                    uint32_t depth_format,
                                    uint32_t sample_count,
                                    unsigned n_attrs,
                                    const uint32_t *attr_format,
                                    const uint32_t *attr_offset,
                                    const uint32_t *attr_buffer_index,
                                    unsigned n_bufs,
                                    const uint32_t *buf_stride,
                                    const uint32_t *buf_step_function,
                                    const uint32_t *buf_step_rate,
                                    void **out_pipeline_state,
                                    void **out_library,
                                    char **out_combined_msl)
{
    if (out_pipeline_state) *out_pipeline_state = NULL;
    if (out_library)        *out_library        = NULL;
    if (out_combined_msl)   *out_combined_msl   = NULL;

    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil) {
        return false;
    }

    char *combined = NULL;
    if (pre_translated_msl != NULL && pre_translated_msl[0] != '\0') {
        /* Disk-cache hit path. Duplicate so the lifetime matches the
         * translator path's malloc'd `combined`. */
        combined = strdup(pre_translated_msl);
        if (!combined) {
            return false;
        }
    } else {
        if (!vsh_glsl || !psh_glsl) {
            return false;
        }

        char *vsh_err = NULL;
        char *psh_err = NULL;
        char *vsh_msl = pgraph_mtl_glsl_translate_to_msl(
            PGRAPH_MTL_GLSL_STAGE_VERTEX, vsh_glsl, &vsh_err);
        char *psh_msl = pgraph_mtl_glsl_translate_to_msl(
            PGRAPH_MTL_GLSL_STAGE_FRAGMENT, psh_glsl, &psh_err);

        if (!vsh_msl || !psh_msl) {
            fprintf(stderr,
                    "pgraph_mtl_shaders: translate failed (vsh=%p psh=%p): "
                    "vsh_err=%s psh_err=%s\n",
                    vsh_msl, psh_msl,
                    vsh_err ? vsh_err : "(none)",
                    psh_err ? psh_err : "(none)");
            if (vsh_err) free(vsh_err);
            if (psh_err) free(psh_err);
            if (vsh_msl) free(vsh_msl);
            if (psh_msl) free(psh_msl);
            return false;
        }
        if (vsh_err) free(vsh_err);
        if (psh_err) free(psh_err);

        combined = build_combined_msl(vsh_msl, psh_msl);
        free(vsh_msl);
        free(psh_msl);
        if (!combined) {
            return false;
        }
    }

    /* Optionally hand the combined MSL string back to the caller for
     * disk-cache persistence. The caller takes ownership of the
     * malloc'd copy. */
    if (out_combined_msl) {
        *out_combined_msl = strdup(combined);
        /* If the dup fails, swallow it — disk-cache miss is non-fatal. */
    }

    @autoreleasepool {
        NSString *src = [NSString stringWithUTF8String:combined];
        free(combined);
        if (src == nil) {
            if (out_combined_msl && *out_combined_msl) {
                free(*out_combined_msl);
                *out_combined_msl = NULL;
            }
            return false;
        }

        NSError *err = nil;
        MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
        opts.languageVersion = MTLLanguageVersion2_3;

        id<MTLLibrary> lib = [device newLibraryWithSource:src
                                                  options:opts
                                                    error:&err];
        if (lib == nil) {
            fprintf(stderr,
                    "pgraph_mtl_shaders: newLibraryWithSource failed: %s\n",
                    [[err localizedDescription] UTF8String] ?: "(unknown)");
            if (out_combined_msl && *out_combined_msl) {
                free(*out_combined_msl);
                *out_combined_msl = NULL;
            }
            return false;
        }
        lib.label = @"xemu.metal.shader_lib";

        id<MTLFunction> vfunc =
            [lib newFunctionWithName:@"vertex_main0"];
        id<MTLFunction> ffunc =
            [lib newFunctionWithName:@"fragment_main0"];
        if (vfunc == nil || ffunc == nil) {
            fprintf(stderr,
                    "pgraph_mtl_shaders: function lookup failed "
                    "(vfunc=%p ffunc=%p)\n", vfunc, ffunc);
            if (out_combined_msl && *out_combined_msl) {
                free(*out_combined_msl);
                *out_combined_msl = NULL;
            }
            return false;
        }

        MTLRenderPipelineDescriptor *desc =
            [[MTLRenderPipelineDescriptor alloc] init];
        desc.label            = @"xemu.metal.translated_pipeline";
        desc.vertexFunction   = vfunc;
        desc.fragmentFunction = ffunc;

        if (color_format != 0) {
            desc.colorAttachments[0].pixelFormat = (MTLPixelFormat)color_format;
            desc.colorAttachments[0].blendingEnabled = NO;
            desc.colorAttachments[0].writeMask       = MTLColorWriteMaskAll;
        }
        if (depth_format != 0) {
            MTLPixelFormat dfmt = (MTLPixelFormat)depth_format;
            desc.depthAttachmentPixelFormat = dfmt;
            if (dfmt == MTLPixelFormatDepth32Float_Stencil8 ||
                dfmt == MTLPixelFormatDepth24Unorm_Stencil8) {
                desc.stencilAttachmentPixelFormat = dfmt;
            }
        }
        if (sample_count > 1) {
            desc.rasterSampleCount = sample_count;
        }

        MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];

        /* M5.8: only populate attribute slots the encode path actually
         * binds. Inactive slots (attr_format == 0) are left unset —
         * the GLSL generator emits `vec4 vN = inlineValue[k];` for
         * them (reading from the VSH UBO at MSL [[buffer(0)]]) rather
         * than `[[attribute(N)]]`. See
         * hw/xbox/nv2a/pgraph/mtl/vertex.c::pgraph_mtl_set_attr_masks
         * (drives pg->uniform_attrs) and
         * hw/xbox/nv2a/pgraph/glsl/vsh.c:257-281 (uniform branch).
         *
         * BufferIndex layout (M5.8): the VSH UBO occupies MSL
         * `[[buffer(0)]]` so per-vertex attribute streams use buffer
         * indices starting at MTL_ATTR_BUFFER_INDEX_BASE = 1. State.c
         * encodes that shift into attr_buffer_index[i]; this builder
         * just consumes whatever bufferIndex the key carries.
         */
        for (unsigned i = 0; i < n_attrs; i++) {
            if (attr_format[i] == 0) {
                continue;
            }
            vd.attributes[i].format =
                (MTLVertexFormat)attr_format[i];
            vd.attributes[i].offset      = attr_offset[i];
            vd.attributes[i].bufferIndex = attr_buffer_index[i];
        }

        /* M5.8: layouts are indexed by bufferIndex (not attribute
         * slot). For each active attribute slot i, the layout for
         * `attr_buffer_index[i]` carries the stride / step-function. */
        for (unsigned i = 0; i < n_bufs; i++) {
            if (buf_stride[i] == 0) {
                continue;
            }
            uint32_t bi = attr_buffer_index[i];
            vd.layouts[bi].stride       = buf_stride[i];
            vd.layouts[bi].stepFunction =
                (MTLVertexStepFunction)buf_step_function[i];
            vd.layouts[bi].stepRate     = buf_step_rate[i];
        }
        desc.vertexDescriptor = vd;

        NSError *pserr = nil;
        id<MTLRenderPipelineState> ps =
            [device newRenderPipelineStateWithDescriptor:desc error:&pserr];
        if (ps == nil) {
            fprintf(stderr,
                    "pgraph_mtl_shaders: newRenderPipelineState failed: %s\n",
                    [[pserr localizedDescription] UTF8String] ?: "(unknown)");
            if (out_combined_msl && *out_combined_msl) {
                free(*out_combined_msl);
                *out_combined_msl = NULL;
            }
            return false;
        }

        if (out_library)        *out_library        = (__bridge_retained void *)lib;
        if (out_pipeline_state) *out_pipeline_state = (__bridge_retained void *)ps;
    }
    return true;
}

/*
 * Public synchronous Metal-build helper. Called from shadergen.c on
 * cache miss when async is disabled (XEMU_METAL_ASYNC_PIPELINE_COMPILE=0)
 * or as the slow-path fallback. M9 adds two new parameters:
 *   - `pre_translated_msl`: optional pre-translated combined MSL source
 *     (post-rename, post-combine). When non-NULL, the GLSL→SPIR-V→MSL
 *     step is skipped — disk-cache hit path.
 *   - `out_combined_msl`: optional out-pointer for the combined MSL the
 *     build used. Caller frees with `free()`. Used to persist a fresh
 *     translation to the disk cache.
 */
extern "C" bool
pgraph_mtl_shaders_build_pipeline(const char *vsh_glsl,
                                  const char *psh_glsl,
                                  const char *pre_translated_msl,
                                  uint32_t color_format,
                                  uint32_t depth_format,
                                  uint32_t sample_count,
                                  unsigned n_attrs,
                                  const uint32_t *attr_format,
                                  const uint32_t *attr_offset,
                                  const uint32_t *attr_buffer_index,
                                  unsigned n_bufs,
                                  const uint32_t *buf_stride,
                                  const uint32_t *buf_step_function,
                                  const uint32_t *buf_step_rate,
                                  void **out_pipeline_state,
                                  void **out_library,
                                  char **out_combined_msl)
{
    return build_pipeline_internal(vsh_glsl, psh_glsl, pre_translated_msl,
                                   color_format, depth_format, sample_count,
                                   n_attrs, attr_format, attr_offset,
                                   attr_buffer_index,
                                   n_bufs, buf_stride, buf_step_function,
                                   buf_step_rate,
                                   out_pipeline_state, out_library,
                                   out_combined_msl);
}

extern "C" void
pgraph_mtl_shaders_release_pipeline(void *pipeline_state, void *library)
{
    if (pipeline_state) {
        id<MTLRenderPipelineState> ps =
            (__bridge_transfer id<MTLRenderPipelineState>)pipeline_state;
        (void)ps;
    }
    if (library) {
        id<MTLLibrary> lib = (__bridge_transfer id<MTLLibrary>)library;
        (void)lib;
    }
}

/* -------- M8: async pipeline compile dispatch queue -------- */

static dispatch_queue_t  s_compile_queue       = nullptr;
static dispatch_group_t  s_compile_group       = nullptr;
static bool              s_dispatch_active     = false;

extern "C" bool pgraph_mtl_shaders_init_dispatch_queue(void)
{
    if (s_dispatch_active) {
        return true;
    }

    /* Drive `setShouldMaximizeConcurrentCompilation:YES` on the device.
     * Per docs/apple-silicon/metal-renderer-plan.md §3.10 + Dolphin's
     * Apple Silicon gotcha, guard with respondsToSelector: in case
     * we end up running on an OCLP-patched older Mac. The Apple
     * Silicon Mac targets we care about always respond.
     *
     * The selector setShouldMaximizeConcurrentCompilation: lives on
     * MTLDevice; it tells Metal to use as many CPU cores as possible
     * for shader compilation rather than serializing on a single
     * helper thread. Idempotent and cheap. */
    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device != nil) {
        SEL sel = @selector(setShouldMaximizeConcurrentCompilation:);
        if ([device respondsToSelector:sel]) {
            ((void (*)(id, SEL, BOOL))objc_msgSend)(device, sel, YES);
        }
    }

    /* Concurrent dispatch queue at QoS_UTILITY. Multiple compiles run in
     * parallel. Metal's newRenderPipelineState…/newLibraryWithSource:
     * are themselves multi-threaded internally with
     * setShouldMaximizeConcurrentCompilation:YES, so a single concurrent
     * queue with N tasks effectively saturates available cores. */
    s_compile_queue = dispatch_queue_create(
        "app.xemu.metal.pipeline_compile",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT,
                                                QOS_CLASS_UTILITY, 0));
    if (s_compile_queue == nullptr) {
        return false;
    }
    s_compile_group = dispatch_group_create();
    if (s_compile_group == nullptr) {
        s_compile_queue = nullptr;
        return false;
    }
    s_dispatch_active = true;
    return true;
}

extern "C" void pgraph_mtl_shaders_finalize_dispatch_queue(void)
{
    if (!s_dispatch_active) {
        return;
    }
    /* Wait for all in-flight compiles to land before tearing down the
     * cache machinery in the .c side. */
    dispatch_group_wait(s_compile_group, DISPATCH_TIME_FOREVER);
    s_compile_group = nullptr;
    s_compile_queue = nullptr;
    s_dispatch_active = false;
}

/*
 * Public async-build entry called from shadergen.c on cache miss when
 * async compile is enabled. Owns the heap-allocated input strings +
 * arrays — frees them after the build completes.
 *
 * M9: `pre_translated_msl` (optional, owned by callee) — when non-NULL,
 * the GLSL→MSL translation step is skipped and this MSL is used
 * directly. Disk-cache hit path. The build's combined MSL is reported
 * back to the C side via `pgraph_mtl_shaders_async_complete`'s new
 * `combined_msl` parameter so it can be persisted to disk on a fresh
 * translation.
 */
extern "C" void
pgraph_mtl_shaders_dispatch_build(void *ctx,
                                  char *vsh_glsl,
                                  char *psh_glsl,
                                  char *pre_translated_msl,
                                  uint32_t color_format,
                                  uint32_t depth_format,
                                  uint32_t sample_count,
                                  unsigned n_attrs,
                                  uint32_t *attr_format,
                                  uint32_t *attr_offset,
                                  uint32_t *attr_buffer_index,
                                  unsigned n_bufs,
                                  uint32_t *buf_stride,
                                  uint32_t *buf_step_function,
                                  uint32_t *buf_step_rate)
{
    if (!s_dispatch_active || s_compile_queue == nullptr) {
        /* Async unavailable — run synchronously on the caller's
         * thread. This path keeps correctness if init_dispatch_queue
         * failed; the cold-launch performance benefit is lost but the
         * rendering result is the same. */
        void *ps = NULL, *lib = NULL;
        char *combined_msl = NULL;
        bool ok = build_pipeline_internal(
            vsh_glsl, psh_glsl, pre_translated_msl,
            color_format, depth_format, sample_count,
            n_attrs, attr_format, attr_offset, attr_buffer_index,
            n_bufs, buf_stride, buf_step_function, buf_step_rate,
            &ps, &lib,
            /* Only request combined MSL on fresh translation. */
            (pre_translated_msl != NULL) ? NULL : &combined_msl);
        pgraph_mtl_shaders_async_complete(ctx, ok, ps, lib, combined_msl);
        g_free(vsh_glsl);
        g_free(psh_glsl);
        g_free(pre_translated_msl);
        g_free(attr_format);
        g_free(attr_offset);
        g_free(attr_buffer_index);
        g_free(buf_stride);
        g_free(buf_step_function);
        g_free(buf_step_rate);
        return;
    }

    /* Capture by value into a heap-allocated job; the block below takes
     * ownership of all the heap pointers. */
    typedef struct AsyncJob {
        void     *ctx;
        char     *vsh_glsl;
        char     *psh_glsl;
        char     *pre_translated_msl;
        uint32_t  color_format;
        uint32_t  depth_format;
        uint32_t  sample_count;
        unsigned  n_attrs;
        uint32_t *attr_format;
        uint32_t *attr_offset;
        uint32_t *attr_buffer_index;
        unsigned  n_bufs;
        uint32_t *buf_stride;
        uint32_t *buf_step_function;
        uint32_t *buf_step_rate;
    } AsyncJob;

    AsyncJob *job = (AsyncJob *)malloc(sizeof(AsyncJob));
    if (job == nullptr) {
        /* OOM: synchronous fallback. */
        void *ps = NULL, *lib = NULL;
        char *combined_msl = NULL;
        bool ok = build_pipeline_internal(
            vsh_glsl, psh_glsl, pre_translated_msl,
            color_format, depth_format, sample_count,
            n_attrs, attr_format, attr_offset, attr_buffer_index,
            n_bufs, buf_stride, buf_step_function, buf_step_rate,
            &ps, &lib,
            (pre_translated_msl != NULL) ? NULL : &combined_msl);
        pgraph_mtl_shaders_async_complete(ctx, ok, ps, lib, combined_msl);
        g_free(vsh_glsl);
        g_free(psh_glsl);
        g_free(pre_translated_msl);
        g_free(attr_format);
        g_free(attr_offset);
        g_free(attr_buffer_index);
        g_free(buf_stride);
        g_free(buf_step_function);
        g_free(buf_step_rate);
        return;
    }
    job->ctx = ctx;
    job->vsh_glsl = vsh_glsl;
    job->psh_glsl = psh_glsl;
    job->pre_translated_msl = pre_translated_msl;
    job->color_format = color_format;
    job->depth_format = depth_format;
    job->sample_count = sample_count;
    job->n_attrs = n_attrs;
    job->attr_format = attr_format;
    job->attr_offset = attr_offset;
    job->attr_buffer_index = attr_buffer_index;
    job->n_bufs = n_bufs;
    job->buf_stride = buf_stride;
    job->buf_step_function = buf_step_function;
    job->buf_step_rate = buf_step_rate;

    dispatch_group_async(s_compile_group, s_compile_queue, ^{
        void *ps = NULL, *lib = NULL;
        char *combined_msl = NULL;
        bool ok = build_pipeline_internal(
            job->vsh_glsl, job->psh_glsl, job->pre_translated_msl,
            job->color_format, job->depth_format, job->sample_count,
            job->n_attrs, job->attr_format, job->attr_offset,
            job->attr_buffer_index,
            job->n_bufs, job->buf_stride, job->buf_step_function,
            job->buf_step_rate,
            &ps, &lib,
            (job->pre_translated_msl != NULL) ? NULL : &combined_msl);
        pgraph_mtl_shaders_async_complete(job->ctx, ok, ps, lib,
                                          combined_msl);
        g_free(job->vsh_glsl);
        g_free(job->psh_glsl);
        g_free(job->pre_translated_msl);
        g_free(job->attr_format);
        g_free(job->attr_offset);
        g_free(job->attr_buffer_index);
        g_free(job->buf_stride);
        g_free(job->buf_step_function);
        g_free(job->buf_step_rate);
        free(job);
    });
}
