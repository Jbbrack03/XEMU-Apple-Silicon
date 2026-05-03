/*
 * NV2A PGRAPH Metal renderer — surface manager (slice M2).
 *
 * C-callable interface to a minimal surface manager that owns the
 * current color render target, current depth render target, and the
 * "front" framebuffer texture that the present compositor blits to the
 * CAMetalLayer drawable.
 *
 * Compared with vk/surface.c (1760 lines), the M2 surface manager is
 * intentionally minimal:
 *   - One color binding + one depth binding at a time (no surface cache
 *     across guest VRAM bindings yet — M3 starts wiring draws and at
 *     that point we'll need a per-vram-addr cache).
 *   - clear_surface decodes the NV097_CLEAR_SURFACE_* mask and runs a
 *     single render pass with MTLLoadActionClear / no draws.
 *   - The "front" framebuffer pointer is published so the compositor in
 *     ui/xemu-metal.mm can read it via a side-channel accessor.
 *
 * Per docs/apple-silicon/metal-renderer-plan.md slice M2 the gate is
 * "a game that issues only clear_surface (no drawing) shows the cleared
 * color in the window".
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_SURFACE_H
#define HW_XBOX_NV2A_PGRAPH_MTL_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize / finalize. Init must run after pgraph_mtl_heap_init().
 */
bool pgraph_mtl_surface_init(void);
void pgraph_mtl_surface_finalize(void);

/*
 * Ensure a color RT and (optionally) a depth RT exist with the given
 * dimensions. Reallocates from the heap when the shape changes. Either
 * width or height being 0 leaves the binding untouched (matches the
 * common "unconfigured surface" case).
 *
 * Format arguments are NV097_SET_SURFACE_FORMAT_COLOR_* /
 * NV097_SET_SURFACE_FORMAT_ZETA_* values from nv2a_regs.h. The surface
 * manager translates them to MTLPixelFormat internally so renderer.c
 * (a .c file without Metal headers) does not need MTLPixelFormat.
 *
 * Called by pgraph_mtl_clear_surface and (later) pgraph_mtl_draw_begin.
 */
void pgraph_mtl_surface_ensure_color(uint32_t width, uint32_t height,
                                     uint32_t nv097_color_format);
void pgraph_mtl_surface_ensure_depth(uint32_t width, uint32_t height,
                                     uint32_t nv097_zeta_format);

/*
 * Encode a clear pass into a fresh command buffer and commit it.
 *
 * write_color: clear the color binding with rgba (each channel 0..1f).
 * write_zeta:  clear the depth binding with depth (0..1f).
 * If a binding is nil, that aspect of the clear is silently skipped.
 */
void pgraph_mtl_surface_clear(bool write_color, const float rgba[4],
                              bool write_zeta, float depth);

/*
 * After a successful clear, the color binding becomes the current
 * "front" framebuffer that the compositor reads. This is a stand-in for
 * the per-VRAM-addr surface cache + sync_pending machinery the GL
 * renderer uses; M2 does not implement that yet.
 *
 * Returns 1 if a front framebuffer texture is available, else 0.
 * Matches the int return type of PGRAPHRenderer.ops.get_framebuffer_surface.
 */
int pgraph_mtl_surface_has_front_framebuffer(void);

/*
 * Side-channel accessor for the compositor in ui/xemu-metal.mm. Returns
 * the current front framebuffer as id<MTLTexture> cast to void*, or
 * NULL if no surface has been created yet.
 *
 * The texture is owned by the surface manager; the caller must NOT
 * release it. The caller must use the texture only on the same Metal
 * device that allocated it (the global one from xemu-metal.mm).
 *
 * This accessor exists because PGRAPHRenderer.ops.get_framebuffer_surface
 * has the type `int (*)(NV2AState *)` (the GL impl returns a GLuint).
 * Returning an id<MTLTexture> as int is not portable, so M2 publishes
 * the texture via this side-channel and the int op returns 1/0.
 *
 * Decision-log entry: "2026-05-02: Metal slice M2 — clear-only surface
 * manager + side-channel framebuffer texture accessor".
 */
void *pgraph_mtl_get_framebuffer_metal_texture(void);

/*
 * The surface manager's framebuffer-texture pointer is sampled by the
 * compositor in ui/xemu-metal.mm at present time. After present, the
 * compositor calls this to release any per-frame references. For M2 the
 * surface is long-lived (no in-flight tracking), so this is a no-op.
 * Future slices may use it for fence/release pairing.
 */
void pgraph_mtl_release_framebuffer_metal_texture(void);

/*
 * Accessors for the currently-bound color / depth render targets. Used
 * by the draw module (mtl/draw.mm) to build a render-pass descriptor
 * without round-tripping through renderer.c. Returns NULL if no
 * binding is active.
 *
 * The dimensions and pixel formats are stored alongside the textures
 * so the draw module can construct a viewport / pipeline-cache key.
 *
 * Pixel format is returned as MTLPixelFormat cast to uint32_t (matches
 * the heap.h convention).
 */
void *pgraph_mtl_surface_get_color_texture(void);
void *pgraph_mtl_surface_get_depth_texture(void);
uint32_t pgraph_mtl_surface_get_color_format(void);
uint32_t pgraph_mtl_surface_get_depth_format(void);
uint32_t pgraph_mtl_surface_get_width(void);
uint32_t pgraph_mtl_surface_get_height(void);

/*
 * M11: configure the per-renderer MSAA sample count. Called once at
 * pgraph_mtl_init after the env var has been parsed and clamped to
 * the device's `supportsTextureSampleCount:` reply. `sample_count`
 * must be 1 (off), 2, 4, or 8. Calling this with the same value as
 * the current configuration is a no-op; any change clears the MSAA
 * companion bindings so they are re-allocated against the new count.
 */
void pgraph_mtl_surface_set_msaa_sample_count(uint32_t sample_count);

/*
 * M11: returns the effective MSAA sample count (1 = off). Used by
 * draw.mm and state.c to drive the render-pass `storeAction` and the
 * pipeline `rasterSampleCount` consistently with the surface
 * manager's allocations.
 */
uint32_t pgraph_mtl_surface_get_msaa_sample_count(void);

/*
 * M11: accessor for the MSAA companion color/depth textures. These
 * are memoryless multisample textures that pair 1:1 with the
 * single-sample color/depth bindings; they are the render-pass
 * `texture` while the single-sample bindings are the
 * `resolveTexture`. Returns NULL if MSAA is disabled or the
 * companion texture has not yet been allocated.
 */
void *pgraph_mtl_surface_get_msaa_color_texture(void);
void *pgraph_mtl_surface_get_msaa_depth_texture(void);

/*
 * M11: counter accessors. Always-on atomics; costs nothing when MSAA
 * is off (no resolve happens, the increment is skipped). Surfaced via
 * extract-perf-summary.sh.
 */
uint64_t pgraph_mtl_surface_msaa_resolve_count(void);
uint64_t pgraph_mtl_surface_msaa_resolve_us_total(void);

/*
 * Diagnostic counters surfaced by extract-perf-summary.sh. These are
 * always-on atomics and have negligible cost.
 */
uint64_t pgraph_mtl_surface_clear_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_SURFACE_H */
