/*
 * NV2A PGRAPH Metal renderer — pipeline cache implementation (slice M3).
 *
 * See pipeline.h for the public contract. M3 ships a single hand-coded
 * MSL passthrough pipeline that's compiled on first use and cached per
 * (color_format, depth_format) tuple.
 *
 * The vertex layout matches what mtl/draw.mm's flush_draw stages:
 *   buffer 0: tightly-packed float4 position per vertex (16 B stride)
 *   buffer 1: tightly-packed float4 color per vertex (16 B stride)
 *
 * Two separate buffers (rather than interleaved into one) match the
 * NV2A "one buffer per attribute" convention used by the GL/VK
 * renderers — each attribute has its own VBO. M3 staging emits two
 * blobs back-to-back into the active ring slot.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "pipeline.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" void *xemu_metal_get_device(void);

/* The passthrough MSL — exact source from
 * docs/apple-silicon/metal-renderer-plan.md slice M3 spec.
 *
 * M4 extension: a `passthrough_native_depth_fs` variant that writes a
 * depth output via `[[depth(any)]]`, reproducing the structure of the
 * GL fragment-shader native-tri-depth path
 * (glsl/psh.c lines 1027-1051 — `XEMU_NATIVE_TRI_DEPTH` /
 * `XEMU_NATIVE_QUAD` branch). MSL's `[[position]]` input is already in
 * window-relative coordinates with `position.z` equal to OpenGL's
 * `gl_FragCoord.z` in linear-Z mode, so the depth derivation
 * `dfdx/dfdy(position.z)` mirrors GL's
 * `nativeTriMZ = max(abs(dFdx(zvalue) * surfaceScale.x),
 *                    abs(dFdy(zvalue) * surfaceScale.y))`.
 *
 * The full polynomial offset (`zvalue += depthFactor * nativeTriMZ`)
 * requires the `clipRange` / `depthFactor` / `depthOffset` /
 * `surfaceScale` uniforms that land with the real PSH translation in
 * M5. For M4 we ship the depth-slope derivation skeleton with neutral
 * values (depthFactor = 0, depthOffset = 0, surfaceScale = 1) so the
 * output equals `gl_FragCoord.z` exactly — i.e. byte-identical to the
 * pre-PR-#2240 fixed-function depth — and the pipeline-state plumbing
 * (counter increments, fragment function selection) is exercised end
 * to end. M5 wires the uniforms in and the same MSL function picks up
 * the full PR #2240 behavior with one buffer-bind change.
 */
static NSString *const k_passthrough_msl =
    @"#include <metal_stdlib>\n"
    @"using namespace metal;\n"
    @"\n"
    @"struct VertexIn {\n"
    @"    float4 position [[attribute(0)]];\n"
    @"    float4 color    [[attribute(3)]];\n"
    @"};\n"
    @"\n"
    @"struct VertexOut {\n"
    @"    float4 position [[position]];\n"
    @"    float4 color;\n"
    @"};\n"
    @"\n"
    @"struct FragOut {\n"
    @"    float4 color [[color(0)]];\n"
    @"    float depth  [[depth(any)]];\n"
    @"};\n"
    @"\n"
    @"vertex VertexOut passthrough_vs(VertexIn in [[stage_in]]) {\n"
    @"    VertexOut out;\n"
    @"    out.position = in.position;\n"
    @"    out.color = in.color;\n"
    @"    return out;\n"
    @"}\n"
    @"\n"
    @"fragment float4 passthrough_fs(VertexOut in [[stage_in]]) {\n"
    @"    return in.color;\n"
    @"}\n"
    @"\n"
    @"// M4: native-depth fragment shader. Derives per-fragment depth\n"
    @"// slope analogously to the GL native_tri_depth path in\n"
    @"// glsl/psh.c. M4 hard-codes neutral depth bias values; M5 will\n"
    @"// route in the real clipRange/depthFactor/depthOffset uniforms.\n"
    @"fragment FragOut passthrough_native_depth_fs(VertexOut in [[stage_in]]) {\n"
    @"    FragOut out;\n"
    @"    out.color = in.color;\n"
    @"    // gl_FragCoord.z equivalent. position.z in MSL is already in\n"
    @"    // [0, 1] window-space depth on Apple Silicon, matching GL's\n"
    @"    // gl_FragCoord.z under glDepthRange(0, 1).\n"
    @"    float zvalue = in.position.z;\n"
    @"    // Slope-of-z derivation: matches GL's\n"
    @"    //   nativeTriMZ = max(abs(dFdx(zvalue) * surfaceScale.x),\n"
    @"    //                     abs(dFdy(zvalue) * surfaceScale.y));\n"
    @"    // surfaceScale is hard-wired to 1 in M4. The result is\n"
    @"    // computed but discarded under the M4 neutral bias — this is\n"
    @"    // intentional scaffolding so M5 can flip on the real bias\n"
    @"    // without changing this MSL function.\n"
    @"    float nativeTriMZ = max(abs(dfdx(zvalue)),\n"
    @"                            abs(dfdy(zvalue)));\n"
    @"    (void)nativeTriMZ;\n"
    @"    out.depth = zvalue;\n"
    @"    return out;\n"
    @"}\n";

/* Pipeline cache. M3 caches at most a handful of (color_fmt, depth_fmt)
 * combinations — typically just one for any given Xbox title — so a
 * tiny linear-scan array is more than enough. M5 swaps this for an
 * LruCache on PipelineKey.
 *
 * M4: the cache key adds a `variant` tag so the M3 passthrough_fs and
 * the M4 passthrough_native_depth_fs can coexist without colliding.
 * `MTL_PIPELINE_VARIANT_PASSTHROUGH` matches the M3 default (no depth
 * write); `MTL_PIPELINE_VARIANT_NATIVE_DEPTH` writes depth via
 * `[[depth(any)]]`. */
typedef enum {
    MTL_PIPELINE_VARIANT_PASSTHROUGH = 0,
    MTL_PIPELINE_VARIANT_NATIVE_DEPTH = 1,
} PipelineVariant;

typedef struct {
    uint32_t color_fmt;
    uint32_t depth_fmt;
    uint32_t variant;
    uint32_t sample_count; /* M11: MSAA sample count; 1 = non-MSAA */
    void    *pipeline_state;  /* id<MTLRenderPipelineState> retained */
} CacheEntry;

#define MTL_PIPELINE_CACHE_CAP 32
static CacheEntry s_cache[MTL_PIPELINE_CACHE_CAP];
static int        s_cache_size;

static id<MTLLibrary>  s_passthrough_lib;
static id<MTLFunction> s_passthrough_vs;
static id<MTLFunction> s_passthrough_fs;
static id<MTLFunction> s_passthrough_native_depth_fs;
static id<MTLDevice>   s_device;

static bool s_initialized = false;
static _Atomic(uint64_t) s_compile_count = 0;

/* -------- lifecycle -------- */

bool pgraph_mtl_pipeline_init(void)
{
    if (s_initialized) {
        return true;
    }

    s_device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (s_device == nil) {
        fprintf(stderr,
                "pgraph_mtl_pipeline_init: no Metal device; "
                "xemu_metal_init must run first\n");
        return false;
    }

    /* Pre-compile the MSL once at init. The library is reused across
     * every (color, depth) format pair; only the
     * MTLRenderPipelineState differs per pair. */
    NSError *err = nil;
    s_passthrough_lib = [s_device newLibraryWithSource:k_passthrough_msl
                                               options:nil
                                                 error:&err];
    if (s_passthrough_lib == nil) {
        fprintf(stderr,
                "pgraph_mtl_pipeline_init: passthrough MSL compile failed: %s\n",
                [[err localizedDescription] UTF8String] ?: "(unknown)");
        return false;
    }
    s_passthrough_lib.label = @"xemu.metal.passthrough_lib";

    s_passthrough_vs = [s_passthrough_lib newFunctionWithName:@"passthrough_vs"];
    s_passthrough_fs = [s_passthrough_lib newFunctionWithName:@"passthrough_fs"];
    s_passthrough_native_depth_fs =
        [s_passthrough_lib newFunctionWithName:@"passthrough_native_depth_fs"];

    if (s_passthrough_vs == nil || s_passthrough_fs == nil ||
        s_passthrough_native_depth_fs == nil) {
        fprintf(stderr,
                "pgraph_mtl_pipeline_init: passthrough function lookup failed "
                "(vs=%p fs=%p native_depth_fs=%p)\n",
                s_passthrough_vs, s_passthrough_fs,
                s_passthrough_native_depth_fs);
        s_passthrough_lib = nil;
        s_passthrough_vs = nil;
        s_passthrough_fs = nil;
        s_passthrough_native_depth_fs = nil;
        return false;
    }

    for (int i = 0; i < MTL_PIPELINE_CACHE_CAP; i++) {
        s_cache[i].color_fmt = 0;
        s_cache[i].depth_fmt = 0;
        s_cache[i].variant = 0;
        s_cache[i].sample_count = 0;
        s_cache[i].pipeline_state = NULL;
    }
    s_cache_size = 0;
    atomic_store(&s_compile_count, (uint64_t)0);

    s_initialized = true;
    return true;
}

void pgraph_mtl_pipeline_finalize(void)
{
    if (!s_initialized) {
        return;
    }

    for (int i = 0; i < s_cache_size; i++) {
        if (s_cache[i].pipeline_state) {
            /* ARC owns the +1 retain via __bridge_transfer when we
             * stored it. Releasing here returns to balanced count. */
            id<MTLRenderPipelineState> ps =
                (__bridge_transfer id<MTLRenderPipelineState>)
                    s_cache[i].pipeline_state;
            (void)ps;
            s_cache[i].pipeline_state = NULL;
        }
        s_cache[i].color_fmt = 0;
        s_cache[i].depth_fmt = 0;
    }
    s_cache_size = 0;

    s_passthrough_vs = nil;
    s_passthrough_fs = nil;
    s_passthrough_native_depth_fs = nil;
    s_passthrough_lib = nil;
    s_device = nil;
    s_initialized = false;
}

/* -------- cache lookup / build -------- */

static void *build_pipeline(uint32_t color_fmt, uint32_t depth_fmt,
                            PipelineVariant variant,
                            uint32_t sample_count)
{
    @autoreleasepool {
        MTLRenderPipelineDescriptor *desc =
            [[MTLRenderPipelineDescriptor alloc] init];
        const char *variant_label =
            (variant == MTL_PIPELINE_VARIANT_NATIVE_DEPTH)
                ? "native_depth"
                : "passthrough";
        desc.label = [NSString
            stringWithFormat:@"xemu.metal.%s_c%u_d%u_s%u",
                             variant_label, color_fmt, depth_fmt,
                             sample_count];
        desc.vertexFunction   = s_passthrough_vs;
        desc.fragmentFunction =
            (variant == MTL_PIPELINE_VARIANT_NATIVE_DEPTH)
                ? s_passthrough_native_depth_fs
                : s_passthrough_fs;

        /* Color attachment 0. */
        desc.colorAttachments[0].pixelFormat =
            (MTLPixelFormat)color_fmt;
        desc.colorAttachments[0].blendingEnabled = NO;
        desc.colorAttachments[0].writeMask = MTLColorWriteMaskAll;

        /* Depth attachment if requested. */
        if (depth_fmt != 0) {
            desc.depthAttachmentPixelFormat = (MTLPixelFormat)depth_fmt;
            /* Stencil format follows the same texture for combined
             * formats. The two combined depth/stencil formats Apple
             * Silicon supports are Depth32Float_Stencil8 (260) and
             * Depth24Unorm_Stencil8 (255 — Mac only, not iOS). */
            if (depth_fmt == (uint32_t)MTLPixelFormatDepth32Float_Stencil8 ||
                depth_fmt == (uint32_t)MTLPixelFormatDepth24Unorm_Stencil8) {
                desc.stencilAttachmentPixelFormat = (MTLPixelFormat)depth_fmt;
            }
        }

        /* M11: MSAA. The pipeline's rasterSampleCount must match the
         * render-pass attachment's sampleCount or Metal rejects the
         * draw. Setting it to 1 (or omitting) is equivalent. */
        if (sample_count > 1) {
            desc.rasterSampleCount = sample_count;
        }

        /* Vertex descriptor: two buffers, one attribute each. */
        MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];
        /* Attribute 0 — position, in buffer 0. */
        vd.attributes[0].format      = MTLVertexFormatFloat4;
        vd.attributes[0].offset      = 0;
        vd.attributes[0].bufferIndex = 0;
        /* Attribute 3 — color, in buffer 1. */
        vd.attributes[3].format      = MTLVertexFormatFloat4;
        vd.attributes[3].offset      = 0;
        vd.attributes[3].bufferIndex = 1;
        vd.layouts[0].stride       = 16;
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        vd.layouts[0].stepRate     = 1;
        vd.layouts[1].stride       = 16;
        vd.layouts[1].stepFunction = MTLVertexStepFunctionPerVertex;
        vd.layouts[1].stepRate     = 1;
        desc.vertexDescriptor = vd;

        NSError *err = nil;
        id<MTLRenderPipelineState> ps =
            [s_device newRenderPipelineStateWithDescriptor:desc error:&err];
        if (ps == nil) {
            fprintf(stderr,
                    "pgraph_mtl_pipeline: %s build (c=%u d=%u) "
                    "failed: %s\n",
                    variant_label, color_fmt, depth_fmt,
                    [[err localizedDescription] UTF8String] ?: "(unknown)");
            return NULL;
        }

        atomic_fetch_add(&s_compile_count, 1);
        /* +1 retain transferred to ARC; we'll release in finalize. */
        return (__bridge_retained void *)ps;
    }
}

static void *pipeline_get_variant(uint32_t color_pixel_format,
                                  uint32_t depth_pixel_format,
                                  PipelineVariant variant,
                                  uint32_t sample_count)
{
    if (!s_initialized) {
        return NULL;
    }
    if (sample_count == 0) {
        sample_count = 1;
    }

    /* Linear scan — cache size is bounded by MTL_PIPELINE_CACHE_CAP. */
    for (int i = 0; i < s_cache_size; i++) {
        if (s_cache[i].color_fmt    == color_pixel_format &&
            s_cache[i].depth_fmt    == depth_pixel_format &&
            s_cache[i].variant      == (uint32_t)variant &&
            s_cache[i].sample_count == sample_count) {
            return s_cache[i].pipeline_state;
        }
    }

    if (s_cache_size >= MTL_PIPELINE_CACHE_CAP) {
        fprintf(stderr,
                "pgraph_mtl_pipeline: cache full (%d entries); "
                "variant=%d (c=%u d=%u s=%u) skipped\n",
                s_cache_size, (int)variant,
                color_pixel_format, depth_pixel_format, sample_count);
        return NULL;
    }

    void *ps = build_pipeline(color_pixel_format, depth_pixel_format,
                              variant, sample_count);
    if (ps == NULL) {
        return NULL;
    }

    s_cache[s_cache_size].color_fmt    = color_pixel_format;
    s_cache[s_cache_size].depth_fmt    = depth_pixel_format;
    s_cache[s_cache_size].variant      = (uint32_t)variant;
    s_cache[s_cache_size].sample_count = sample_count;
    s_cache[s_cache_size].pipeline_state = ps;
    s_cache_size++;

    return ps;
}

void *pgraph_mtl_pipeline_get_passthrough(uint32_t color_pixel_format,
                                          uint32_t depth_pixel_format,
                                          uint32_t sample_count)
{
    return pipeline_get_variant(color_pixel_format, depth_pixel_format,
                                MTL_PIPELINE_VARIANT_PASSTHROUGH,
                                sample_count);
}

void *pgraph_mtl_pipeline_get_native_depth(uint32_t color_pixel_format,
                                           uint32_t depth_pixel_format,
                                           uint32_t sample_count)
{
    return pipeline_get_variant(color_pixel_format, depth_pixel_format,
                                MTL_PIPELINE_VARIANT_NATIVE_DEPTH,
                                sample_count);
}

uint64_t pgraph_mtl_pipeline_compile_count(void)
{
    return atomic_load(&s_compile_count);
}

/* -------- M5 MSL validation hook -------- */

extern "C" int pgraph_mtl_pipeline_validate_msl(const char *msl_source,
                                                char **out_error)
{
    if (out_error) {
        *out_error = NULL;
    }
    if (msl_source == NULL) {
        if (out_error) {
            *out_error = strdup("null msl_source");
        }
        return 0;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil) {
        if (out_error) {
            *out_error = strdup("no Metal device");
        }
        return 0;
    }

    @autoreleasepool {
        NSString *src = [NSString stringWithUTF8String:msl_source];
        if (src == nil) {
            if (out_error) {
                *out_error = strdup("UTF-8 conversion failed");
            }
            return 0;
        }
        NSError *err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:src
                                                  options:nil
                                                    error:&err];
        if (lib == nil) {
            if (out_error) {
                NSString *desc = [err localizedDescription];
                const char *cstr = desc ? [desc UTF8String] : "(unknown)";
                *out_error = strdup(cstr ? cstr : "(unknown)");
            }
            return 0;
        }
        /* Library is autoreleased; we don't retain it. */
        (void)lib;
    }
    return 1;
}
