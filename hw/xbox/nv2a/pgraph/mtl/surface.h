/*
 * NV2A PGRAPH Metal renderer — surface manager (slice M2 + M5.9).
 *
 * C-callable interface to the Metal-side surface manager. M5.9 (2026-05-03)
 * promotes the M2-era single-slot manager to a per-VRAM-address cache that
 * mirrors the relevant subset of `vk/surface.c`: entries keyed by
 * `vram_addr`+`size`, persistent across binding changes, and looked up
 * by the CRTC publish path via `pgraph_mtl_surface_get_within`.
 *
 * The implementation is split:
 *   - `surface.mm` owns the QTAILQ-equivalent doubly-linked-list,
 *     MTLTexture + MSAA companion lifetime, and the upload-from-VRAM
 *     blit. It does NOT include nv2a_int.h.
 *   - `renderer.c` calls into the cache from `pgraph_mtl_surface_update`
 *     and `pgraph_mtl_get_framebuffer_surface`. Per-target headers are
 *     visible here only.
 *
 * The legacy single-slot accessors (`pgraph_mtl_surface_ensure_color` /
 * `_ensure_depth`, `pgraph_mtl_surface_get_color_texture`, etc.) are kept
 * as thin wrappers over the new cache so the existing draw / clear paths
 * keep compiling unchanged. The "currently bound" pointer is now a
 * pointer into the cache rather than a private statics.
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

/* M5.9: drop every cache entry. Called from `pgraph_mtl_surface_flush`
 * so paired GL/Metal renderer-restart flows are byte-identical. */
void pgraph_mtl_surface_cache_flush(void);

/*
 * Bind a color or depth surface keyed by VRAM address. M5.9: replaces
 * the M2-era pgraph_mtl_surface_ensure_color/_depth. The cache
 * promotes a freshly-bound (vram_addr, dimensions, format) into a
 * persistent entry; subsequent re-binds of the same vram_addr at the
 * same shape return the existing entry instead of releasing+reallocating
 * the underlying MTLTexture.
 *
 * `vram_addr` is the absolute VRAM offset of the surface's first pixel.
 * `size` is the in-VRAM byte footprint (`pitch * height` rounded up
 * to width*bytes_per_pixel).
 * `pitch` is the row stride in VRAM bytes (matches `pg->surface_*.pitch`).
 * `vram_ptr` is `d->vram_ptr` (or NULL to skip the upload-from-VRAM
 * step); the cache uses it to upload the guest's pixel data into the
 * MTLTexture on first allocation so the freshly-bound RT picks up the
 * caller's prior CPU writes. The pointer remains valid for the
 * lifetime of the renderer; the cache only reads from `vram_ptr +
 * vram_addr` synchronously inside the call.
 *
 * Returns true on success (an entry was bound, possibly newly-created),
 * false on failure (heap exhausted, invalid arguments).
 */
bool pgraph_mtl_surface_bind_color(uint32_t vram_addr, uint32_t size,
                                   uint32_t width, uint32_t height,
                                   uint32_t pitch,
                                   uint32_t nv097_color_format,
                                   const uint8_t *vram_ptr);
bool pgraph_mtl_surface_bind_depth(uint32_t vram_addr, uint32_t size,
                                   uint32_t width, uint32_t height,
                                   uint32_t pitch,
                                   uint32_t nv097_zeta_format,
                                   const uint8_t *vram_ptr);

/* Lookup helpers. Mirror vk/surface.c::pgraph_vk_surface_get and
 * pgraph_vk_surface_get_within. The returned pointer is owned by the
 * cache; do not release. NULL when nothing is bound at the given
 * address. The "_within" variant returns the surface whose VRAM range
 * contains `vram_addr` (used by the CRTC publish path; the CRTC start
 * may not be exactly the surface's `vram_addr` if `line_offset != 0`). */
void *pgraph_mtl_surface_get_metal_texture_at(uint32_t vram_addr);
void *pgraph_mtl_surface_get_metal_texture_within(uint32_t vram_addr,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  uint32_t *out_format);

/*
 * Legacy ensure-color/-depth wrappers. Kept so the M2-era clear path
 * keeps working; the M5.9 cache binds a "synthetic" entry at vram_addr
 * 0 if no bind_color/bind_depth has been called yet. Set vram_addr=0
 * means "use whatever the renderer most recently bound for this aspect"
 * — i.e. the legacy single-slot semantics.
 *
 * Either width or height being 0 leaves the binding untouched.
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
 * Returns 1 if a front framebuffer texture is available, else 0.
 * Matches the int return type of PGRAPHRenderer.ops.get_framebuffer_surface.
 */
int pgraph_mtl_surface_has_front_framebuffer(void);

/*
 * Side-channel accessor for the compositor in ui/xemu-metal.mm. Returns
 * the current front framebuffer as id<MTLTexture> cast to void*, or
 * NULL if no surface has been published yet.
 *
 * M5.9: the published front-fb is the surface that the NV2A CRTC says
 * is the active framebuffer (looked up via `d->pcrtc.start +
 * line_offset` against the surface cache). Falls back to the most
 * recently bound color surface if no CRTC-pointed surface exists yet.
 */
void *pgraph_mtl_get_framebuffer_metal_texture(void);

/*
 * The surface manager's framebuffer-texture pointer is sampled by the
 * compositor in ui/xemu-metal.mm at present time. After present, the
 * compositor calls this to release any per-frame references. For M2 the
 * surface is long-lived (no in-flight tracking), so this is a no-op.
 */
void pgraph_mtl_release_framebuffer_metal_texture(void);

/* M5.9: publish the surface at `vram_addr` as the front-fb. Called
 * from `pgraph_mtl_get_framebuffer_surface` (renderer.c) which already
 * has access to NV2AState. The surface manager itself does not include
 * nv2a_int.h, so renderer.c does the CRTC math and hands the result
 * down via this call. Returns true if a matching cache entry was
 * found and published; false otherwise (the front-fb is unchanged).
 *
 * Bumps the `METAL_FRONT_FB_PUBLISHES` counter when the resolved
 * texture differs from the last published value, and emits a
 * `xemu-perf: metal_front_fb_publish vram_addr=0x.. width=W height=H
 * format=FMT reason=<reason>` line on the same condition.
 *
 * `reason` is a short literal ("crtc", "clear", "ensure", "bind") used
 * for the diagnostic log; the cache keeps a copy of the pointer so
 * repeated publishes for the same texture are deduped to one line.
 */
bool pgraph_mtl_surface_publish_front_fb(uint32_t vram_addr,
                                         const char *reason);

/*
 * Accessors for the currently-bound color / depth render targets.
 */
void *pgraph_mtl_surface_get_color_texture(void);
void *pgraph_mtl_surface_get_depth_texture(void);
uint32_t pgraph_mtl_surface_get_color_format(void);
uint32_t pgraph_mtl_surface_get_depth_format(void);
uint32_t pgraph_mtl_surface_get_width(void);
uint32_t pgraph_mtl_surface_get_height(void);

/*
 * M11: configure the per-renderer MSAA sample count.
 */
void pgraph_mtl_surface_set_msaa_sample_count(uint32_t sample_count);

/*
 * M11: returns the effective MSAA sample count (1 = off).
 */
uint32_t pgraph_mtl_surface_get_msaa_sample_count(void);

/*
 * M11: accessor for the MSAA companion color/depth textures.
 */
void *pgraph_mtl_surface_get_msaa_color_texture(void);
void *pgraph_mtl_surface_get_msaa_depth_texture(void);

/*
 * M11: counter accessors. Always-on atomics.
 */
uint64_t pgraph_mtl_surface_msaa_resolve_count(void);
uint64_t pgraph_mtl_surface_msaa_resolve_us_total(void);

/*
 * Diagnostic counters surfaced by extract-perf-summary.sh.
 */
uint64_t pgraph_mtl_surface_clear_count(void);

/* M5.9: counters. */
uint64_t pgraph_mtl_surface_front_fb_publishes(void);
uint64_t pgraph_mtl_surface_cache_entries(void);

/*
 * M5.9-followup-A (2026-05-03): NV097_IMAGE_BLIT GPU-side surface copy.
 *
 * Look up `src_vram_addr` and `dst_vram_addr` in the per-VRAM cache; if
 * both resolve to MTLTextures with matching pixel format, encode a
 * MTLBlitCommandEncoder copyFromTexture from the src rect to the dst
 * rect. If formats mismatch (or either entry is missing), invalidate the
 * destination cache entry so the next bind reallocates with a fresh
 * upload from VRAM — the caller is expected to have already updated
 * guest VRAM via the CPU-side memcpy path (mirroring vk/gl's blit).
 *
 * `src_x` / `src_y` / `dst_x` / `dst_y` / `width` / `height` are in
 * GUEST 1x pixel space; the cache scales them by the surface entry's
 * (texture_dim / vram_dim) ratio to address the host-scaled MTLTexture
 * (surface_scale_factor=2 means texture is 2x the VRAM dims).
 *
 * Returns true if a GPU-side blit was issued; false if the path fell
 * back to invalidate-on-mismatch (or both src/dst missing). Bumps the
 * METAL_IMAGE_BLITS counter on a successful GPU blit.
 *
 * The caller MUST have already called pgraph_mtl_draw_flush_open_pass
 * so any in-flight render encoder against either texture is committed.
 */
bool pgraph_mtl_surface_blit_copy(uint32_t src_vram_addr,
                                  uint32_t dst_vram_addr,
                                  uint32_t src_x, uint32_t src_y,
                                  uint32_t dst_x, uint32_t dst_y,
                                  uint32_t width, uint32_t height);

/* M5.9-followup-A: counter accessor. Always-on atomic. */
uint64_t pgraph_mtl_surface_image_blits(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_SURFACE_H */
