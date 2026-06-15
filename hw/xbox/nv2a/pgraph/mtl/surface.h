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
#include <stdio.h>

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

/*
 * M5.9-followup-C (2026-05-03): bind_color_ex / bind_depth_ex —
 * extended bind that takes explicit GUEST 1× dimensions. The MTLTexture
 * is allocated at the host-scaled dims (caller passes those as
 * `width`/`height`), but the VRAM-side upload reads `guest_width ×
 * guest_height` pixels into the top-left sub-rect. The plain bind
 * variants above set `guest_*` equal to `width`/`height` for backward
 * compat with the legacy ensure-by-shape path; new call sites should
 * use the `_ex` variants and pass distinct values.
 *
 * Returns true on success.
 */
bool pgraph_mtl_surface_bind_color_ex(uint32_t vram_addr, uint32_t size,
                                      uint32_t width, uint32_t height,
                                      uint32_t guest_width,
                                      uint32_t guest_height,
                                      uint32_t pitch,
                                      uint32_t nv097_color_format,
                                      const uint8_t *vram_ptr,
                                      uint32_t clip_x, uint32_t clip_y,
                                      uint32_t clip_w, uint32_t clip_h,
                                      uint32_t scissor_x, uint32_t scissor_y,
                                      uint32_t scissor_w, uint32_t scissor_h);
bool pgraph_mtl_surface_bind_depth_ex(uint32_t vram_addr, uint32_t size,
                                      uint32_t width, uint32_t height,
                                      uint32_t guest_width,
                                      uint32_t guest_height,
                                      uint32_t pitch,
                                      uint32_t nv097_zeta_format,
                                      const uint8_t *vram_ptr,
                                      uint32_t clip_x, uint32_t clip_y,
                                      uint32_t clip_w, uint32_t clip_h,
                                      uint32_t scissor_x, uint32_t scissor_y,
                                      uint32_t scissor_w, uint32_t scissor_h);

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
bool pgraph_mtl_surface_get_color_surface_info_at(uint32_t vram_addr,
                                                  void **out_texture,
                                                  uint32_t *out_width,
                                                  uint32_t *out_height,
                                                  uint32_t *out_guest_width,
                                                  uint32_t *out_guest_height,
                                                  uint32_t *out_pitch,
                                                  uint32_t *out_format);
bool pgraph_mtl_surface_get_color_surface_info_for(uint32_t vram_addr,
                                                   uint32_t guest_width,
                                                   uint32_t guest_height,
                                                   uint32_t pitch,
                                                   void **out_texture,
                                                   uint32_t *out_width,
                                                   uint32_t *out_height,
                                                   uint32_t *out_guest_width,
                                                   uint32_t *out_guest_height,
                                                   uint32_t *out_pitch,
                                                   uint32_t *out_format);
bool pgraph_mtl_surface_has_other_color_shape(uint32_t vram_addr,
                                              uint32_t guest_width,
                                              uint32_t guest_height,
                                              uint32_t pitch);

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
 * write_color:   clear the color binding with rgba (each channel 0..1f).
 * write_depth:   clear the depth aspect of the depth binding with depth.
 * write_stencil: clear the stencil aspect (low 8 bits of stencil). When
 *                the bound depth format lacks a stencil aspect this is
 *                silently ignored. Independently gated from write_depth
 *                so NV097_CLEAR_SURFACE_Z and _STENCIL are honored
 *                separately, mirroring GL's `glClear(GL_DEPTH_BUFFER_BIT)`
 *                vs `glClear(GL_STENCIL_BUFFER_BIT)` semantics in
 *                `gl/draw.c::pgraph_gl_clear_surface`.
 * If a binding is nil, that aspect of the clear is silently skipped.
 */
void pgraph_mtl_surface_clear(bool write_color, const float rgba[4],
                              bool write_depth, float depth,
                              bool write_stencil, int stencil);

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
void pgraph_mtl_release_framebuffer_metal_texture(void *texture);

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
                                         uint32_t crtc_addr,
                                         const char *reason);
bool pgraph_mtl_surface_publish_display_front_fb(uint32_t vram_addr,
                                                 uint32_t display_width,
                                                 uint32_t display_height,
                                                 uint32_t vga_line_offset,
                                                 const char *reason);

/* T2 (2026-05-12 evening): lightweight publish for the per-host-refresh
 * code path (xemu_metal_render_frame → nv2a_get_framebuffer_surface →
 * pgraph_mtl_get_framebuffer_surface). Behaves like
 * `pgraph_mtl_surface_publish_front_fb` BUT unconditionally takes the
 * non-snapshot path — no GPU copy, no `waitUntilCompleted`. The atomic
 * texture-pointer store is the only side effect. Safe because the
 * compositor at xemu-metal.mm:1410 reads the pointer and uses it in a
 * Metal render pass on the same `s_render_queue`, which serializes the
 * NV2A draws and the compositor present ops naturally.
 *
 * The full `publish_front_fb` path (snapshot-or-not gated on
 * `XEMU_METAL_PRESENT_SNAPSHOT`) is preserved for the once-per-guest-
 * flip `pgraph_mtl_flip_stall` caller, where a stable snapshot is
 * still valuable for screenshot/capture diagnostics. */
bool pgraph_mtl_surface_publish_front_fb_pointer_only(uint32_t vram_addr,
                                                      const char *reason);

/* M2 diagnostic (2026-06-04): set the flip-stall ordinal for the
 * publish diagnostic log. Called from pgraph_mtl_flip_stall before
 * any publish path so pgraph_mtl_surface_publish_front_fb and
 * pgraph_mtl_surface_publish_display_front_fb can log the ordinal. */
void pgraph_mtl_surface_set_flip_ordinal(uint64_t ordinal);

/* M2 diagnostic (2026-06-04): return the vram_addr of the last
 * published front-fb surface (the CRTC address used at publish
 * time). Used by pgraph_mtl_get_framebuffer_metal_texture for
 * correlation between publish and lookup. */
uint32_t pgraph_mtl_surface_get_last_publish_vram_addr(void);
/* M2 diagnostic (2026-06-04): CRTC address of last published front-fb. */
uint32_t pgraph_mtl_surface_get_last_publish_crtc_addr(void);

/*
 * Accessors for the currently-bound color / depth render targets.
 */
void *pgraph_mtl_surface_get_color_texture(void);
void *pgraph_mtl_surface_get_depth_texture(void);
uint32_t pgraph_mtl_surface_get_color_format(void);
uint32_t pgraph_mtl_surface_get_depth_format(void);
uint32_t pgraph_mtl_surface_get_width(void);
uint32_t pgraph_mtl_surface_get_height(void);
/* 2026-05-03 magenta-RT investigation: vram_addr of the currently-bound
 * color/depth render target. Returns 0 if no binding exists OR if the
 * active binding came from the legacy ensure-by-shape fallback (which
 * does not know its VRAM address). Used by the per-vram_addr
 * `metal_draw_target` diagnostic counter in mtl/renderer.c so we can
 * see WHICH cached SurfaceBinding is the actual draw destination. */
uint32_t pgraph_mtl_surface_get_color_vram_addr(void);
uint32_t pgraph_mtl_surface_get_depth_vram_addr(void);
uint32_t pgraph_mtl_surface_get_depth_dirty(void);

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

/* 2026-05-20: cross-sibling sync counters.
 *
 * `_sibling_syncs` increments per executed sibling-sync GPU blit
 * (a same-VRAM same-pitch same-format same-aspect sibling with fresher
 * content was found and copied into the about-to-be-rebound entry).
 * `_sibling_sync_skips` increments when a sync was considered but no
 * fresher sibling existed (the common case once the cache stabilizes).
 *
 * Gated by env flag XEMU_METAL_RTT_SIBLING_SYNC (default on). */
uint64_t pgraph_mtl_surface_sibling_syncs(void);
uint64_t pgraph_mtl_surface_sibling_sync_skips(void);

/* Tool 1 (2026-05-19): structured per-flip JSONL surface-graph dump.
 *
 * Emits one "flip" header line followed by one "binding" line per
 * cache entry to `out`. Designed for consumption by
 * `scripts/apple-silicon/surface-graph-analyze.py`. The publish-source
 * metadata in the flip header is taken from the SELECTED source
 * binding at last publish time (Codex review 2026-05-19, finding #1) —
 * the published texture object itself may be a composed display
 * texture, snapshot, or the binding's own e->texture depending on the
 * publish path. Caller MUST hold `pg->lock` so the singly-linked cache
 * list does not mutate while we walk it (the flip_stall hook already
 * runs under pg->lock per T2, commit 3ae76a327c).
 *
 * `flip_ordinal` is the monotonic flip-stall counter maintained by the
 * caller; embedded into every emitted line so the analyzer can group.
 * `reason` is a short literal stored in the flip header (e.g.,
 * "flip_stall", "manual"). */
void pgraph_mtl_surface_dump_graph_jsonl(FILE *out, const char *reason,
                                         uint64_t flip_ordinal);

/* Tool 1 (2026-05-19): cumulative graph-dump counter; surfaced via
 * METAL_SURFACE_GRAPH_DUMPS in `extract-perf-summary.sh`. */
uint64_t pgraph_mtl_surface_graph_dumps(void);
/* 2026-05-03 magenta-RT diagnostic: monotonic count of cache entries
 * destroyed + recreated due to shape mismatch on same-vram_addr rebind.
 * Each shape-mismatch destroy clobbers all previously rendered content
 * for that vram_addr, so a non-zero rate explains "draws hit but
 * screenshots show fresh texture content". */
uint64_t pgraph_mtl_surface_recreate_shape_mismatch(void);

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

/*
 * M5.9-followup-B+C (2026-05-03): CPU-write dirty tracking + VRAM upload.
 *
 * The cache supports lazy upload from guest VRAM into the cached
 * MTLTexture. Two trigger paths:
 *   - On bind / re-bind: if the entry was just created (or its
 *     dirty_vram bit is set), upload from `vram_ptr + vram_addr`.
 *   - Before publish-front-fb: if the resolved entry's dirty_vram bit
 *     is set, upload before bumping the published-texture pointer.
 *
 * The dirty_vram bit is set by `pgraph_mtl_surface_mark_dirty_overlapping`
 * which is invoked from the CPU-write access callback registered in
 * `mtl/renderer.c` via `mem_access_callback_insert` (TCG path) or via
 * `memory_region_test_and_clear_dirty` polled at bind time (KVM/HVF
 * path; mirrors vk/surface.c::update_surface_part).
 *
 * `pgraph_mtl_surface_register_access_cb_for(vram_addr, cb)` and
 * `_unregister_access_cb_for` attach an opaque `MemAccessCallback*`
 * (typed as void* here so surface.mm doesn't include cpu.h) to the
 * cache entry so the tear-down on eviction can remove it.
 */
void pgraph_mtl_surface_mark_dirty_overlapping(uint32_t addr, uint32_t len);
void pgraph_mtl_surface_register_access_cb_for(uint32_t vram_addr, void *cb);
void pgraph_mtl_surface_unregister_access_cb_for(uint32_t vram_addr,
                                                 void **out_cb);
/*
 * Iterate every cache entry; for each entry whose dirty_vram bit is
 * set OR is_color but never-uploaded, upload `vram_ptr + entry.vram_addr`
 * into the cached MTLTexture. Returns the number of uploads performed.
 * The caller (renderer.c) supplies a `vram_ptr` valid for the lifetime
 * of this call.
 */
unsigned int pgraph_mtl_surface_upload_dirty(const uint8_t *vram_ptr);

/*
 * Lazy single-entry upload: if the entry at `vram_addr` is dirty (or
 * never-initialized), upload from `vram_ptr + vram_addr`. Used by the
 * publish-front-fb path so the published surface always reflects the
 * latest guest CPU writes when CPU-writes are the swap mechanism.
 */
void pgraph_mtl_surface_upload_if_dirty_at(uint32_t vram_addr,
                                           const uint8_t *vram_ptr);

/*
 * Force-upload an entry. Used at first cache-allocate to seed the
 * texture with whatever is in VRAM (the BIOS framebuffer, the menu
 * surface, etc.). Mirrors vk's "upload at bind when dirty" pattern.
 */
void pgraph_mtl_surface_force_upload_at(uint32_t vram_addr,
                                        const uint8_t *vram_ptr);

/*
 * Snapshot helpers — number of cache entries currently allocated and
 * whether `vram_addr` is present. Used by renderer.c for the
 * registration / unregistration loop. The cache itself is the
 * single-source-of-truth for which addresses have callbacks attached;
 * use these to drive the registration side-effects.
 */
unsigned int pgraph_mtl_surface_iter_addresses(uint32_t *out, unsigned int cap);

/* M5.10 codex finding (HIGH severity, 2026-05-03): companion of
 * `_iter_addresses` that also yields each entry's `size` field. Used
 * by the KVM/HVF parity polling in `pgraph_mtl_surface_update` so
 * `memory_region_test_and_clear_dirty` covers the full surface range
 * — a 4 KB conservative probe missed guest writes outside the first
 * page on multi-MB framebuffers. Both `out_addrs` and `out_sizes` are
 * required; pass parallel arrays of length `cap`. Returns the number
 * of entries written. */
unsigned int pgraph_mtl_surface_iter_address_size(uint32_t *out_addrs,
                                                  uint32_t *out_sizes,
                                                  unsigned int cap);

/* M5.10 experimental fallback (2026-05-03): publish the
 * dominant per-frame color draw target as the front-fb. See
 * `surface.mm::pgraph_mtl_surface_publish_latest_draw_fallback` for
 * the full rationale. Gated by `XEMU_METAL_FRONT_FB_FALLBACK=1` at
 * the renderer level. */
void pgraph_mtl_surface_note_color_draw(void *texture, bool color_write);
bool pgraph_mtl_surface_publish_latest_draw_fallback(uint32_t display_width,
                                                     uint32_t display_height,
                                                     uint32_t crtc_vram_addr);

/* M5.9-followup-B+C: counter accessors. Always-on atomics. */
uint64_t pgraph_mtl_surface_vram_dirty_hits(void);
uint64_t pgraph_mtl_surface_vram_uploads(void);
uint64_t pgraph_mtl_surface_vram_upload_bytes(void);

/*
 * M5.10 (2026-05-03): VRAM-coherent surface download.
 *
 * Mirrors `vk/surface.c::pgraph_vk_surface_download_if_dirty` and the
 * gl-side `pgraph_gl_surface_download_if_dirty`. The download path
 * writes rendered MTLTexture pixels back to guest VRAM at the surface's
 * `vram_addr`. The CRTC publish path then reads VRAM at
 * `pcrtc.start + line_offset` and uploads from VRAM into the published
 * texture — closing the back→front-buffer gap for AAA Xbox titles
 * whose draw target differs from the displayed framebuffer.
 *
 * `pgraph_mtl_surface_set_draw_dirty_color/_depth(true)` is called at
 * the end of each `flush_draw` / `clear_surface` to flag the bound
 * surface as having GPU-side rendered pixels. The download paths
 * gate on this bit; only dirty surfaces incur the GPU→VRAM copy.
 *
 * The `download_if_dirty_at(vram_addr, vram_ptr_base)` form looks up
 * via `cache_get_within` so a CRTC publish at a non-zero line_offset
 * still finds the surface that contains it; `download_dirty_all` walks
 * every cache entry and calls `download_if_dirty` on each.
 *
 * `download_in_range_if_dirty(start, len, vram_ptr_base)` is the
 * IMAGE_BLIT / texture-bind hook: any cached surface that overlaps the
 * given VRAM range gets a GPU→VRAM download before the caller reads
 * the affected guest memory.
 *
 * The downloads are SYNCHRONOUS — internally the path uses a
 * MTLBlitCommandEncoder that copies texture pixels into a Shared
 * MTLBuffer, waits for the blit's command-buffer to complete, then
 * memcpy's the buffer contents into `vram_ptr + vram_addr`. On Apple
 * Silicon UMA the GPU→Shared copy is a barrier+coherence sync rather
 * than a real memory copy. Caller must hold `d->pgraph.lock` (or the
 * renderer-thread invariant equivalent).
 *
 * Counters `METAL_SURFACE_DOWNLOADS` (per-interval delta) and
 * `METAL_SURFACE_DOWNLOAD_BYTES` surface on the `xemu-perf:` interval
 * line.
 */
void pgraph_mtl_surface_set_draw_dirty_color(void);
void pgraph_mtl_surface_set_draw_dirty_depth(void);

/* Callback type for "I just downloaded `byte_size` bytes starting at
 * `vram_addr`". Used by the renderer.c wrapper to issue
 * `memory_region_set_client_dirty(... DIRTY_MEMORY_VGA |
 *  DIRTY_MEMORY_NV2A_TEX ...)` from inside the .c file (the .mm file
 * cannot include the QEMU memory headers). `opaque` is whatever the
 * caller passes in (typically NV2AState*).
 *
 * Called synchronously from inside the download path while the
 * caller's stack is live; do not retain the pointer. */
typedef void (*PgraphMtlSurfaceDownloadCb)(void *opaque,
                                           uint32_t vram_addr,
                                           uint32_t byte_size);

void pgraph_mtl_surface_download_if_dirty_at(uint32_t vram_addr,
                                             uint8_t *vram_ptr_base,
                                             PgraphMtlSurfaceDownloadCb cb,
                                             void *cb_opaque);
void pgraph_mtl_surface_download_dirty_all(uint8_t *vram_ptr_base,
                                           PgraphMtlSurfaceDownloadCb cb,
                                           void *cb_opaque);
void pgraph_mtl_surface_download_in_range_if_dirty(uint32_t start,
                                                   uint32_t len,
                                                   uint8_t *vram_ptr_base,
                                                   PgraphMtlSurfaceDownloadCb cb,
                                                   void *cb_opaque);

uint64_t pgraph_mtl_surface_downloads(void);
uint64_t pgraph_mtl_surface_download_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_SURFACE_H */
