/*
 * NV2A PGRAPH Metal renderer — heap manager (slice M2).
 *
 * Implements the C-callable interface declared in heap.h on top of
 * MTLHeap. Two private heaps (color RTs, depth RTs) are pre-allocated
 * at init; allocations are sub-allocated from those heaps.
 *
 * This file does NOT include hw/xbox/nv2a/nv2a_int.h — per
 * docs/apple-silicon/metal-renderer-plan.md slice M1's "Implementation
 * note" the per-target c_args (-DCOMPILING_PER_TARGET / -DCONFIG_TARGET
 * / -DCONFIG_DEVICES) that nv2a_int.h transitively requires do not
 * propagate to .mm / objcpp_COMPILER builds. The .mm file communicates
 * with the rest of the renderer through opaque handles only; the C-side
 * (renderer.c) does the target-specific NV2A work.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "heap.h"

#include <stdio.h>
#include <stdlib.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* Re-declare the small bit of host integration we need from
 * ui/xemu-metal.h. Including that header here would pull in
 * <SDL3/SDL.h> which is not required for the heap module. */
extern "C" void *xemu_metal_get_device(void);

/* Heap sizes. The Xbox graphics memory budget is small (the NV2A
 * shares the 64 MiB unified pool with the CPU), and a typical Xbox
 * game keeps ≤ 8 simultaneous color surfaces (front/back buffer +
 * a few render-to-texture targets) at ≤ 720p-equivalent. Per
 * docs/apple-silicon/metal-renderer-plan.md §3.6 the upstream
 * Vulkan renderer's surface cache caps invalid surfaces at 10 entries
 * (`num_invalid_surfaces_to_keep` in vk/surface.c:32) and active
 * surfaces are tightly bounded by guest behaviour.
 *
 * The renderer also opts the user into surface_scale=2 by default
 * on Apple Silicon (1080p-class internal resolution). At scale 2,
 * an A8R8G8B8 1280×960 surface costs 4.7 MiB; D24S8 1280×960 costs
 * 4.7 MiB. A budget that comfortably holds ~16 active + invalid
 * surfaces at scale-2 1080p-class is well under 256 MiB.
 *
 * 256 MiB per heap is the planning-doc recommendation and matches
 * other emulator Metal backends (Dolphin StreamBuffer ~64 MiB +
 * texture heap several hundred MiB; PCSX2 similar). It is generous
 * but private memory on Apple Silicon is wired to the unified pool,
 * not "wasted" — unused regions are not pre-touched. */
static constexpr NSUInteger HEAP_COLOR_RTS_SIZE = 256ULL * 1024ULL * 1024ULL;
static constexpr NSUInteger HEAP_DEPTH_RTS_SIZE = 256ULL * 1024ULL * 1024ULL;
/* Textures heap (M6). Original Xbox VRAM is 64 MiB shared with the CPU,
 * but xemu's renderer textures can include per-mip + per-face allocations
 * plus surface_scale=2 upscaled copies in some flows. 512 MiB
 * comfortably absorbs the working set — Dolphin's texture heap budget
 * is in the same ballpark for its medium tier. Private + untracked +
 * Automatic placement enables lossless compression on Apple GPU 7+. */
static constexpr NSUInteger HEAP_TEXTURES_SIZE  = 512ULL * 1024ULL * 1024ULL;

static id<MTLHeap> s_heap_color_rts = nil;
static id<MTLHeap> s_heap_depth_rts = nil;
static id<MTLHeap> s_heap_textures  = nil;
static bool        s_heap_initialized = false;
static bool        s_supports_framebuffer_fetch = false;
/* M9: Apple GPU family + macOS version, latched at init. */
static uint32_t    s_apple_gpu_family = 0;
static uint32_t    s_macos_version    = 0;

bool pgraph_mtl_heap_init(void)
{
    if (s_heap_initialized) {
        return true;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil) {
        fprintf(stderr,
                "pgraph_mtl_heap_init: no Metal device (xemu_metal_init "
                "must run first)\n");
        return false;
    }

    @autoreleasepool {
        MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
        /* Tracked + automatic placement — the Apple Silicon-recommended
         * default for render targets per metal-api-reference.md. The
         * driver tracks hazards between encoders that read/write into
         * the heap, which we want for color/depth RT ping-pong. */
        desc.type        = MTLHeapTypeAutomatic;
        desc.storageMode = MTLStorageModePrivate;
        desc.hazardTrackingMode = MTLHazardTrackingModeTracked;
        desc.cpuCacheMode = MTLCPUCacheModeDefaultCache;

        desc.size = HEAP_COLOR_RTS_SIZE;
        s_heap_color_rts = [device newHeapWithDescriptor:desc];
        if (s_heap_color_rts == nil) {
            fprintf(stderr,
                    "pgraph_mtl_heap_init: failed to allocate "
                    "color-RT heap (%llu bytes)\n",
                    (unsigned long long)HEAP_COLOR_RTS_SIZE);
            return false;
        }
        s_heap_color_rts.label = @"xemu.metal.heap.color_rts";

        desc.size = HEAP_DEPTH_RTS_SIZE;
        s_heap_depth_rts = [device newHeapWithDescriptor:desc];
        if (s_heap_depth_rts == nil) {
            fprintf(stderr,
                    "pgraph_mtl_heap_init: failed to allocate "
                    "depth-RT heap (%llu bytes)\n",
                    (unsigned long long)HEAP_DEPTH_RTS_SIZE);
            s_heap_color_rts = nil;
            return false;
        }
        s_heap_depth_rts.label = @"xemu.metal.heap.depth_rts";

        /* Textures heap (M6). Untracked: the upload sequence is
         * blit-encoder-into-Private with explicit cmd-buffer ordering;
         * the driver's automatic hazard tracking would only add
         * overhead for read-after-write that we already serialize. */
        desc.size = HEAP_TEXTURES_SIZE;
        desc.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        s_heap_textures = [device newHeapWithDescriptor:desc];
        if (s_heap_textures == nil) {
            fprintf(stderr,
                    "pgraph_mtl_heap_init: failed to allocate "
                    "textures heap (%llu bytes)\n",
                    (unsigned long long)HEAP_TEXTURES_SIZE);
            s_heap_color_rts = nil;
            s_heap_depth_rts = nil;
            return false;
        }
        s_heap_textures.label = @"xemu.metal.heap.textures";
    }

    /* M7: Apple GPU family 1+ detection. Latched once. Apple Silicon
     * Macs report Apple7+ which is a superset of Apple1; Intel Macs
     * return false. The optional XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH
     * flag forces the negative answer for testing the fallback path. */
    bool supports_fb_fetch = false;
    if ([device respondsToSelector:@selector(supportsFamily:)]) {
        supports_fb_fetch = [device supportsFamily:MTLGPUFamilyApple1];
    }
    const char *force_off = getenv("XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH");
    if (force_off && force_off[0] && force_off[0] != '0') {
        supports_fb_fetch = false;
    }
    s_supports_framebuffer_fetch = supports_fb_fetch;

    /* M9: latch the highest Apple GPU family the device reports + the
     * running macOS major.minor. Probed once; both feed the disk
     * shader-cache feature-set fingerprint. The probe walks Apple9 →
     * Apple1 in descending order so Apple Silicon Macs report their
     * actual family rather than Apple1 (the looser superset). Intel
     * Macs (no Apple family) leave s_apple_gpu_family at 0. */
    uint32_t apple_family = 0;
    if ([device respondsToSelector:@selector(supportsFamily:)]) {
        const struct {
            uint32_t        n;
            MTLGPUFamily    f;
        } probes[] = {
#if defined(MAC_OS_VERSION_13_0) || (defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000)
            { 9, MTLGPUFamilyApple9 },
#endif
            { 8, MTLGPUFamilyApple8 },
            { 7, MTLGPUFamilyApple7 },
            { 6, MTLGPUFamilyApple6 },
            { 5, MTLGPUFamilyApple5 },
            { 4, MTLGPUFamilyApple4 },
            { 3, MTLGPUFamilyApple3 },
            { 2, MTLGPUFamilyApple2 },
            { 1, MTLGPUFamilyApple1 },
        };
        for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            if ([device supportsFamily:probes[i].f]) {
                apple_family = probes[i].n;
                break;
            }
        }
    }
    s_apple_gpu_family = apple_family;

    NSOperatingSystemVersion ov =
        [[NSProcessInfo processInfo] operatingSystemVersion];
    s_macos_version = ((uint32_t)ov.majorVersion << 16) |
                      ((uint32_t)ov.minorVersion & 0xFFFF);

    s_heap_initialized = true;
    fprintf(stderr,
            "pgraph-mtl-heap: color_rts=%llu MiB depth_rts=%llu MiB "
            "textures=%llu MiB "
            "type=Automatic storage=Private color/depth=tracked "
            "textures=untracked apple1_framebuffer_fetch=%d\n",
            (unsigned long long)(HEAP_COLOR_RTS_SIZE >> 20),
            (unsigned long long)(HEAP_DEPTH_RTS_SIZE >> 20),
            (unsigned long long)(HEAP_TEXTURES_SIZE >> 20),
            s_supports_framebuffer_fetch ? 1 : 0);
    return true;
}

bool pgraph_mtl_heap_supports_framebuffer_fetch(void)
{
    return s_supports_framebuffer_fetch;
}

uint32_t pgraph_mtl_heap_apple_gpu_family(void)
{
    return s_apple_gpu_family;
}

uint32_t pgraph_mtl_heap_macos_version(void)
{
    return s_macos_version;
}

void pgraph_mtl_heap_finalize(void)
{
    if (!s_heap_initialized) {
        return;
    }
    s_heap_color_rts = nil;
    s_heap_depth_rts = nil;
    s_heap_textures  = nil;
    s_heap_initialized = false;
    s_supports_framebuffer_fetch = false;
    s_apple_gpu_family = 0;
    s_macos_version    = 0;
}

static void *alloc_rt(id<MTLHeap> heap, uint32_t width, uint32_t height,
                      uint32_t pixel_format, MTLTextureUsage usage,
                      const char *label_prefix)
{
    if (heap == nil) {
        return NULL;
    }
    if (width == 0 || height == 0) {
        return NULL;
    }

    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            (MTLPixelFormat)pixel_format
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.storageMode      = MTLStorageModePrivate;
    desc.usage            = usage;
    desc.textureType      = MTLTextureType2D;
    desc.sampleCount      = 1;
    desc.mipmapLevelCount = 1;
    desc.arrayLength      = 1;

    id<MTLTexture> tex = [heap newTextureWithDescriptor:desc];
    if (tex == nil) {
        fprintf(stderr,
                "pgraph-mtl-heap: %s alloc failed: %ux%u fmt=%u "
                "(heap full or unsupported format)\n",
                label_prefix, width, height, pixel_format);
        return NULL;
    }
    tex.label = [NSString stringWithFormat:@"%s_%ux%u_fmt%u",
                          label_prefix, width, height, pixel_format];

    /* Hand the +1 retained reference to the C side as a void*. The
     * caller must balance with pgraph_mtl_heap_release_texture(). */
    return (__bridge_retained void *)tex;
}

void *pgraph_mtl_heap_alloc_color_rt(uint32_t width, uint32_t height,
                                     uint32_t pixel_format)
{
    /* Color RTs need RenderTarget; ShaderRead is needed so the present
     * compositor and texture-fetch paths (M6+) can sample them. */
    return alloc_rt(s_heap_color_rts, width, height, pixel_format,
                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                    "color_rt");
}

void *pgraph_mtl_heap_alloc_depth_rt(uint32_t width, uint32_t height,
                                     uint32_t pixel_format)
{
    /* Depth RTs need RenderTarget; ShaderRead supports later
     * depth-as-texture reads. */
    return alloc_rt(s_heap_depth_rts, width, height, pixel_format,
                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                    "depth_rt");
}

void pgraph_mtl_heap_release_texture(void *texture)
{
    if (texture == NULL) {
        return;
    }
    /* Convert back to id<MTLTexture> with -1 retain count; ARC drops it
     * when this scope exits. The texture's dealloc returns the heap
     * region to MTLHeapTypeAutomatic's free list automatically. */
    id<MTLTexture> tex = (__bridge_transfer id<MTLTexture>)texture;
    (void)tex;
}

uint64_t pgraph_mtl_heap_color_rts_size(void)
{
    return s_heap_initialized ? (uint64_t)HEAP_COLOR_RTS_SIZE : 0;
}

uint64_t pgraph_mtl_heap_depth_rts_size(void)
{
    return s_heap_initialized ? (uint64_t)HEAP_DEPTH_RTS_SIZE : 0;
}

uint64_t pgraph_mtl_heap_textures_size(void)
{
    return s_heap_initialized ? (uint64_t)HEAP_TEXTURES_SIZE : 0;
}

/* -------- M11 memoryless MSAA allocators -------- */

/* MSAA targets are allocated out-of-heap (the existing color/depth
 * heaps are sub-allocators for single-sample Private RTs; mixing in
 * MSAA textures with different sample-count attribute would
 * complicate the heap). On Apple Silicon's TBDR the multisample
 * work always happens in tile memory regardless of storage class;
 * the storage class only controls whether tile contents are written
 * out to a backing store at pass end.
 *
 * **Storage-mode note (M11 v1).** Plan §3.7 calls for Memoryless,
 * which is optimal when every render pass starts from Clear. xemu's
 * Metal renderer issues a separate render pass per `flush_draw`,
 * and inter-draw passes use MTLLoadActionLoad — undefined on
 * Memoryless. M11 v1 ships Private to keep correctness in the
 * many-passes-per-frame case; the memoryless win returns when a
 * future slice coalesces per-frame draws into one render pass. */
static void *alloc_msaa_target(uint32_t width, uint32_t height,
                               uint32_t pixel_format,
                               uint32_t sample_count,
                               MTLTextureUsage usage,
                               const char *label_prefix)
{
    if (width == 0 || height == 0 || sample_count <= 1) {
        return NULL;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil) {
        return NULL;
    }
    if (![device supportsTextureSampleCount:sample_count]) {
        fprintf(stderr,
                "pgraph-mtl-heap: %s sample_count=%u unsupported by device\n",
                label_prefix, sample_count);
        return NULL;
    }

    @autoreleasepool {
        MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
        desc.textureType      = MTLTextureType2DMultisample;
        desc.pixelFormat      = (MTLPixelFormat)pixel_format;
        desc.width            = width;
        desc.height           = height;
        desc.depth            = 1;
        desc.mipmapLevelCount = 1;
        desc.arrayLength      = 1;
        desc.sampleCount      = sample_count;
        /* Private + RenderTarget. The Memoryless optimization
         * requires every render pass to start from a Clear (see the
         * storage-mode note above); xemu's per-draw render passes
         * need Load to preserve prior content, which is undefined on
         * Memoryless. Private gives well-defined Load semantics at
         * the cost of an off-chip backing store; on TBDR the
         * MSAA work itself still lives in tile memory until pass end. */
        desc.storageMode      = MTLStorageModePrivate;
        desc.usage            = usage;

        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        if (tex == nil) {
            fprintf(stderr,
                    "pgraph-mtl-heap: %s memoryless alloc failed: "
                    "%ux%u fmt=%u samples=%u\n",
                    label_prefix, width, height, pixel_format,
                    sample_count);
            return NULL;
        }
        tex.label = [NSString
            stringWithFormat:@"%s_msaa%u_%ux%u_fmt%u",
                             label_prefix, sample_count,
                             width, height, pixel_format];
        return (__bridge_retained void *)tex;
    }
}

void *pgraph_mtl_heap_alloc_msaa_color(uint32_t width, uint32_t height,
                                       uint32_t pixel_format,
                                       uint32_t sample_count)
{
    return alloc_msaa_target(width, height, pixel_format, sample_count,
                             MTLTextureUsageRenderTarget,
                             "msaa_color");
}

void *pgraph_mtl_heap_alloc_msaa_depth(uint32_t width, uint32_t height,
                                       uint32_t pixel_format,
                                       uint32_t sample_count)
{
    return alloc_msaa_target(width, height, pixel_format, sample_count,
                             MTLTextureUsageRenderTarget,
                             "msaa_depth");
}

bool pgraph_mtl_heap_supports_sample_count(uint32_t sample_count)
{
    if (sample_count <= 1) {
        return true;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (device == nil) {
        return false;
    }
    return [device supportsTextureSampleCount:sample_count];
}

/* -------- M6 texture-heap allocation helpers -------- */

static void *alloc_texture_descriptor(MTLTextureDescriptor *desc,
                                      const char *label_prefix,
                                      uint32_t pixel_format,
                                      uint32_t width, uint32_t height)
{
    if (s_heap_textures == nil || desc == nil) {
        return NULL;
    }
    desc.storageMode = MTLStorageModePrivate;
    desc.usage       = MTLTextureUsageShaderRead;

    id<MTLTexture> tex = [s_heap_textures newTextureWithDescriptor:desc];
    if (tex == nil) {
        fprintf(stderr,
                "pgraph-mtl-heap: %s alloc failed: %ux%u fmt=%u "
                "(heap full or unsupported format)\n",
                label_prefix, width, height, pixel_format);
        return NULL;
    }
    tex.label = [NSString stringWithFormat:@"%s_%ux%u_fmt%u",
                          label_prefix, width, height, pixel_format];
    return (__bridge_retained void *)tex;
}

void *pgraph_mtl_heap_alloc_texture_2d(uint32_t width, uint32_t height,
                                       uint32_t levels,
                                       uint32_t pixel_format)
{
    if (width == 0 || height == 0 || levels == 0) {
        return NULL;
    }
    @autoreleasepool {
        MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
        desc.textureType      = MTLTextureType2D;
        desc.pixelFormat      = (MTLPixelFormat)pixel_format;
        desc.width            = width;
        desc.height           = height;
        desc.depth            = 1;
        desc.mipmapLevelCount = levels;
        desc.arrayLength      = 1;
        desc.sampleCount      = 1;
        return alloc_texture_descriptor(desc, "tex2d", pixel_format,
                                        width, height);
    }
}

void *pgraph_mtl_heap_alloc_texture_cube(uint32_t edge,
                                         uint32_t levels,
                                         uint32_t pixel_format)
{
    if (edge == 0 || levels == 0) {
        return NULL;
    }
    @autoreleasepool {
        MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
        desc.textureType      = MTLTextureTypeCube;
        desc.pixelFormat      = (MTLPixelFormat)pixel_format;
        desc.width            = edge;
        desc.height           = edge;
        desc.depth            = 1;
        desc.mipmapLevelCount = levels;
        desc.arrayLength      = 1;
        desc.sampleCount      = 1;
        return alloc_texture_descriptor(desc, "texcube", pixel_format,
                                        edge, edge);
    }
}

void *pgraph_mtl_heap_alloc_texture_3d(uint32_t width, uint32_t height,
                                       uint32_t depth, uint32_t levels,
                                       uint32_t pixel_format)
{
    if (width == 0 || height == 0 || depth == 0 || levels == 0) {
        return NULL;
    }
    @autoreleasepool {
        MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
        desc.textureType      = MTLTextureType3D;
        desc.pixelFormat      = (MTLPixelFormat)pixel_format;
        desc.width            = width;
        desc.height           = height;
        desc.depth            = depth;
        desc.mipmapLevelCount = levels;
        desc.arrayLength      = 1;
        desc.sampleCount      = 1;
        return alloc_texture_descriptor(desc, "tex3d", pixel_format,
                                        width, height);
    }
}
