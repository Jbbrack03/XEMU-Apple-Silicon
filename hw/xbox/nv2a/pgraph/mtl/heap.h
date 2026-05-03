/*
 * NV2A PGRAPH Metal renderer — heap manager (slice M2).
 *
 * C-callable interface to a small set of MTLHeap-backed allocators for
 * NV2A render targets and (eventually) sampled textures. The Metal
 * implementation lives in heap.mm; this header only exposes opaque
 * void* texture handles so it can be safely included from .c sources
 * that compile without -fobjc-arc and without Metal headers.
 *
 * Per docs/apple-silicon/metal-renderer-plan.md §3.6 ("Memory: heap +
 * untracked + memoryless"), three heaps + two out-of-heap classes are
 * planned:
 *   - heap_color_rts (MTLHeapTypeAutomatic, MTLStorageModePrivate, tracked)
 *   - heap_depth_rts (MTLHeapTypeAutomatic, MTLStorageModePrivate, tracked)
 *   - heap_textures  (MTLHeapTypeAutomatic, MTLStorageModePrivate, untracked) — M6
 *   - MSAA targets   (out-of-heap, memoryless)                              — M11
 *   - Per-frame staging (out-of-heap, shared/write-combined ring)           — M3
 *
 * M2 only brings up the two render-target heaps. The texture heap and
 * staging buffers come in M6 and M3 respectively.
 *
 * Pixel format is passed as NSUInteger (matches MTLPixelFormat's
 * underlying type) to keep this header pure-C. The implementation casts
 * back to MTLPixelFormat at the boundary.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_HEAP_H
#define HW_XBOX_NV2A_PGRAPH_MTL_HEAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the render-target heaps. Returns true on success, false if
 * the device is unavailable or the heaps could not be created.
 *
 * Must be called after xemu_metal_init() — the heaps are allocated from
 * the process-wide MTLDevice owned by ui/xemu-metal.mm.
 */
bool pgraph_mtl_heap_init(void);

/*
 * Tear down the heaps. Releases the MTLHeap references; any outstanding
 * MTLTexture handles allocated from those heaps must be released by the
 * caller first. Safe to call if init failed.
 */
void pgraph_mtl_heap_finalize(void);

/*
 * Allocate a 2D color render target from heap_color_rts. Returns an
 * id<MTLTexture> cast to void*, or NULL if the heap is full.
 *
 * The returned texture is +1 retained under ARC; the caller must
 * release via pgraph_mtl_heap_release_texture() to reclaim the heap
 * region (MTLHeapTypeAutomatic auto-reclaims on dealloc).
 *
 * pixel_format is an MTLPixelFormat value cast to uint32_t.
 * Recommended Apple Silicon defaults for color RTs (from
 * docs/apple-silicon/metal-api-reference.md):
 *   MTLPixelFormatBGRA8Unorm           = 80
 *   MTLPixelFormatBGRA8Unorm_sRGB      = 81
 */
void *pgraph_mtl_heap_alloc_color_rt(uint32_t width, uint32_t height,
                                     uint32_t pixel_format);

/*
 * Allocate a 2D depth render target from heap_depth_rts. Returns an
 * id<MTLTexture> cast to void*, or NULL if the heap is full.
 *
 * Recommended Apple Silicon depth formats:
 *   MTLPixelFormatDepth32Float          = 252
 *   MTLPixelFormatDepth24Unorm_Stencil8 = 255 (NOT supported on Apple
 *                                              Silicon; use Depth32Float_Stencil8)
 *   MTLPixelFormatDepth32Float_Stencil8 = 260
 */
void *pgraph_mtl_heap_alloc_depth_rt(uint32_t width, uint32_t height,
                                     uint32_t pixel_format);

/*
 * Allocate a multisample 2D color render target (M11).
 *
 * Returned out-of-heap (it is NOT sub-allocated from heap_color_rts).
 *
 * **Storage-mode note (M11 v1).** The plan §3.7 calls for
 * `MTLStorageModeMemoryless` so the multisample data lives only in
 * tile memory. That is the optimal storage class on Apple Silicon's
 * TBDR architecture, but it requires every render pass to start
 * from a Clear (memoryless content is undefined at pass-begin).
 * xemu's Metal renderer currently issues a separate render pass per
 * `flush_draw` (one MTLCommandBuffer per draw, M3 pattern). Inter-
 * draw passes need MTLLoadActionLoad to preserve prior draws — and
 * Load is undefined on Memoryless. To keep correctness in the
 * many-passes-per-frame case, M11 ships with
 * `MTLStorageModePrivate` for the MSAA target. The resolve still
 * uses MTLStoreActionMultisampleResolve into the single-sample
 * target; on Apple Silicon's TBDR the actual MSAA work happens in
 * tile memory regardless of the storage class — Private just adds
 * a backing store so Load between passes is well-defined. The
 * memoryless win is recovered when a future slice coalesces draws
 * into a single render pass per frame (M11.1 candidate).
 *
 * `sample_count` must be one of the GPU-supported counts (typically
 * 2 / 4 / 8). The caller is responsible for clamping to
 * `[device supportsTextureSampleCount:N]` first.
 *
 * Pixel format must match the resolve target's pixel format — Metal's
 * `MTLStoreActionMultisampleResolve` requires a format match between
 * the multisample source and single-sample destination.
 *
 * Returns +1 retained void* (id<MTLTexture>); release with
 * pgraph_mtl_heap_release_texture().
 */
void *pgraph_mtl_heap_alloc_msaa_color(uint32_t width, uint32_t height,
                                       uint32_t pixel_format,
                                       uint32_t sample_count);

/*
 * Allocate a multisample 2D depth render target (M11).
 *
 * Same out-of-heap, Private-storage contract as the color variant
 * (see the storage-mode note above). M11 uses
 * `MTLStoreActionDontCare` on the depth resolve (the resolved
 * single-sample depth is not consumed by the present compositor) so
 * no separate resolve target is required for depth.
 */
void *pgraph_mtl_heap_alloc_msaa_depth(uint32_t width, uint32_t height,
                                       uint32_t pixel_format,
                                       uint32_t sample_count);

/*
 * Returns true if the active MTLDevice supports the requested
 * multisample sample count for renderable textures. Wraps
 * `[device supportsTextureSampleCount:N]`.
 */
bool pgraph_mtl_heap_supports_sample_count(uint32_t sample_count);

/*
 * Allocate a sampled 2D texture from heap_textures (M6).
 *
 * Backed by `MTLHeapTypeAutomatic` + `Private` storage + untracked
 * (we manage hazards through the explicit blit-encoder upload sequence
 * into a Private texture). Lossless compression is available because
 * the storage is Private and not view-aliased.
 *
 * `levels` is the mipmap level count (1 for single-level uploads).
 * `usage` follows MTLTextureUsage — pass MTLTextureUsageShaderRead for
 * sampled textures; M6 does not yet support render-to-texture into
 * heap_textures.
 *
 * Returns +1 retained void* (id<MTLTexture>); release with
 * pgraph_mtl_heap_release_texture().
 */
void *pgraph_mtl_heap_alloc_texture_2d(uint32_t width, uint32_t height,
                                       uint32_t levels,
                                       uint32_t pixel_format);

/*
 * Allocate a cube texture from heap_textures.
 */
void *pgraph_mtl_heap_alloc_texture_cube(uint32_t edge,
                                         uint32_t levels,
                                         uint32_t pixel_format);

/*
 * Allocate a 3D texture from heap_textures.
 */
void *pgraph_mtl_heap_alloc_texture_3d(uint32_t width, uint32_t height,
                                       uint32_t depth, uint32_t levels,
                                       uint32_t pixel_format);

/*
 * Release a texture obtained from one of the alloc functions.
 * Decrements the ARC reference; when it hits zero the texture's
 * dealloc reclaims the heap region under MTLHeapTypeAutomatic.
 *
 * Safe to call with NULL.
 */
void pgraph_mtl_heap_release_texture(void *texture);

/*
 * Diagnostic accessors (used for logging only). Returns 0 if the heap
 * is not initialized.
 */
uint64_t pgraph_mtl_heap_color_rts_size(void);
uint64_t pgraph_mtl_heap_depth_rts_size(void);
uint64_t pgraph_mtl_heap_textures_size(void);

/*
 * Apple GPU family 1+ detection (M7).
 *
 * Returns true if `[device supportsFamily:MTLGPUFamilyApple1]` succeeds.
 * Apple Silicon Macs (M1/M2/M3+) all report Apple7+, which is a
 * superset of Apple1 — so this returns true on every Apple Silicon
 * target.  Intel Macs return false; this is the gate for the
 * framebuffer-fetch combiner path (`[[color(0)]]` MSL fragment input
 * + `[[raster_order_group(0)]]`).  When false, the renderer falls
 * back to a render-pass split path.
 *
 * `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH=1` forces this to return false
 * for testing the fallback path on Apple Silicon hardware.
 *
 * Latched at heap_init; stable for the renderer's lifetime.
 */
bool pgraph_mtl_heap_supports_framebuffer_fetch(void);

/*
 * Highest Apple GPU family supported by the active MTLDevice (M9).
 *
 * Returns the family number (Apple1 = 1, Apple7 = 7, Apple8 = 8,
 * Apple9 = 9) or 0 if no Apple-family is reported (Intel Mac, or
 * device unavailable). Apple Silicon Macs report Apple7+ (M1
 * = Apple7, M2 = Apple8, M3 = Apple9, …). The disk shader cache
 * uses this to invalidate cached MSL on hardware change so a cache
 * built on M1 isn't replayed on M3 (where new MSL features are
 * available and the spirv-cross output may differ).
 *
 * Latched at heap_init; stable for the renderer's lifetime.
 */
uint32_t pgraph_mtl_heap_apple_gpu_family(void);

/*
 * macOS major.minor version of the running OS (M9). Returned packed
 * as `(major << 16) | minor`; zero if NSProcessInfo is unavailable.
 * Used to fingerprint the disk shader cache: a cache built on macOS
 * 14 cannot be replayed safely on macOS 13 (Apple's Metal compiler
 * outputs differ across major versions; spirv-cross may also widen).
 */
uint32_t pgraph_mtl_heap_macos_version(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_HEAP_H */
