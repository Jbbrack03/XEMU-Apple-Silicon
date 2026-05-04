# 2026-05-03 — Metal slice M5.10 — VRAM-coherent surface download (default-off)

## Summary

Slice **M5.10 — VRAM-coherent surface download** ships the
infrastructure that mirrors `vk/surface.c::pgraph_vk_surface_download_if_dirty`
on the Metal renderer, plus the codex MEDIUM/LOW fixes from
M5.9-followup-E that gate downstream usage of the new path.

**Default off.** The user-facing visual-correctness gate that M5.9-followup-E
queued for M5.10 is **NOT closed** by this slice: the actual PGR2
back→front buffer-swap mechanism remains unidentified (M5.9-followup-B+C
decisively ruled out CPU memcpy, NV097_IMAGE_BLIT, and pcrtc.start
cycling), so simply downloading the back-buffer's GPU pixels to VRAM at
`vram_addr=0x3628000` does not bridge the gap to the front-fb at
`vram_addr=0x32a4000`. The download path lands as instrumented
infrastructure for future investigation; flag flip to default-on awaits
(a) identifying PGR2's mechanism, (b) a GPU-side downsample pass for
`surface_scale > 1`, (c) ≥ 2 distinct titles passing the per-pixel diff
≤ 1 % gate.

## What landed

### Public API additions (mtl/surface.h)

```c
typedef void (*PgraphMtlSurfaceDownloadCb)(void *opaque,
                                           uint32_t vram_addr,
                                           uint32_t byte_size);

void pgraph_mtl_surface_set_draw_dirty_color(void);
void pgraph_mtl_surface_set_draw_dirty_depth(void);

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

unsigned int pgraph_mtl_surface_iter_address_size(uint32_t *out_addrs,
                                                  uint32_t *out_sizes,
                                                  unsigned int cap);
```

The callback typedef lets `mtl/renderer.c` (which has access to
`memory_region_set_client_dirty`) handle the QEMU dirty-mark side of
the download from a static helper passed through `download_dirty_all`,
keeping `mtl/surface.mm` free of QEMU memory headers.

### MtlSurfaceBinding gains a `_Atomic(uint32_t) draw_dirty` field

Set to 1 at end of `pgraph_mtl_flush_draw` (mtl/renderer.c:1280) and
end of `pgraph_mtl_clear_surface` (mtl/renderer.c:715), gated behind
`mtl_front_fb_download_enabled()`. Cleared back to 0 inside
`download_surface_to_vram` only on a real successful write (codex
finding fix — see Issues section below).

### download_surface_to_vram — the GPU→VRAM blit path

Located at `mtl/surface.mm:580`. Returns `bool` indicating whether
real bytes were written (codex finding #3 fix). On successful path:

1. Drains the open coalesced render pass via
   `pgraph_mtl_draw_flush_open_pass()`.
2. Fetches the cross-queue draw-done event state via
   `pgraph_mtl_draw_get_done_event_state(&event_handle, &event_value)`.
3. Allocates a Shared `MTLBuffer` of size `pitch * guest_h`.
4. Opens an `MTLBlitCommandEncoder` on `s_render_queue` with
   `[cmd encodeWaitForEvent:event_handle value:event_value]` so the
   blit cannot read the texture mid-render (the M5.10 cross-queue
   correctness primitive).
5. `[blit copyFromTexture:sourceSize:toBuffer:]` — single-pass GPU read.
6. Commit, `[cmdBuffer waitUntilCompleted]`.
7. `memcpy([buffer contents], vram_ptr_base + vram_addr, ...)` with
   the same row-stride logic as the upload helper.
8. Atomic-clear `draw_dirty` and `dirty_vram`; bump
   `METAL_SURFACE_DOWNLOADS` and `METAL_SURFACE_DOWNLOAD_BYTES`.

Skips with `return false` on:

- Depth surfaces (deferred to a future depth-aware download slice).
- Host-scaled-vs-guest dimension mismatch (codex HIGH finding fix —
  see Issues section).
- Format-unknown / texture-null / dimension-zero edge cases.

### Cross-queue MTLSharedEvent fence (mtl/draw.{h,mm})

`s_draw_done_event` (created in `pgraph_mtl_draw_init` at
`mtl/draw.mm:161`) is signaled with a monotonically increasing value
inside `open_pass_close_locked` at `mtl/draw.mm:367` after every
draw command-buffer commit. The accessor
`pgraph_mtl_draw_get_done_event_state(&event, &value)` (mtl/draw.mm:218)
hands the latest value to render-queue consumers, who
`[cmdbuf encodeWaitForEvent:value:]` to enforce cross-queue ordering.
This closes the latent correctness hazard the M5.10 audit identified:
`s_draw_queue` (draw encoders) and `s_render_queue` (blit encoders)
are distinct, and their commit ordering is NOT synchronous.

### Render-queue-driven flush sites

- `pgraph_mtl_flip_stall` (renderer.c:805) — calls
  `pgraph_mtl_surface_download_dirty_all(d->vram_ptr,
   mtl_after_surface_download, d)` BEFORE the existing
  `upload_if_dirty_at(crtc_addr, ...)` and `publish_front_fb`. Gated
  on `mtl_front_fb_download_enabled()`.
- `pgraph_mtl_surface_flush` (renderer.c:1592) — calls
  `_download_dirty_all` before disarming callbacks and flushing
  the cache. Gated.
- `pgraph_mtl_image_blit` (mtl/blit.c:114) — calls
  `_download_if_dirty_at(src, ...)` and `_download_if_dirty_at(dst, ...)`
  before the CPU memcpy. Always-on (image_blit fires only when the
  guest issues NV097_IMAGE_BLIT; PGR2 issues 0).
- Surface eviction / shape-mismatch destroy paths NOT yet wired —
  rendered content on those entries is still lost on destroy. Future
  follow-up.

### KVM/HVF parity polling in pgraph_mtl_surface_update

Mirrors `gl/surface.c:1385-1389` polling of
`memory_region_test_and_clear_dirty(d->vram, addr, size, DIRTY_MEMORY_NV2A)`.
Gated on `!tcg_enabled()` so it does NOT run under TCG (where the
per-CPU access-callback path at `mtl_surface_access_callback` is
authoritative; the TCG-side gate fixes a perf regression that
showed up in initial testing — surface_update fires per
NV097_WAIT_FOR_IDLE / per-flip / per-blit, and unconditional polling
added measurable cost).

Codex HIGH finding #2 (2026-05-03): the polling now uses each
entry's full `size` field via the new `_iter_address_size` accessor
(was a fixed 4 KB probe; framebuffers are multi-MB, so writes outside
the first page were silently dropped).

### Open-pass pin (M5.9-followup-E codex 1C — backfilled here)

`mtl/draw.mm` exposes `pgraph_mtl_draw_get_open_pass_textures(&color, &depth)`.
`mtl/surface.mm::cache_evict_lru` skips eviction of any entry whose
`texture` matches the open-pass color or depth. The shape-mismatch
destroy paths in `cache_find_or_create_color/_depth` call
`pgraph_mtl_draw_flush_open_pass()` before `binding_destroy(e)` if
the entry's texture matches the open pass — symmetric with the
follow-up-E front-fb pin.

### Counter wiring

- `METAL_SURFACE_DOWNLOADS` (per-interval delta) — `util/xemu-metal-perf.c`
  with weak-symbol fallback, baseline + delta + emit format string +
  emit args. Surfaced through the `xemu-perf:` interval line.
- `METAL_SURFACE_DOWNLOAD_BYTES` (per-interval delta) — same.
- `scripts/apple-silicon/extract-perf-summary.sh` recognizes both keys.
- `docs/apple-silicon/automation.md` documents the flag, the cross-queue
  fence design, the gating, and the perf trade-off.
- `xemu-fork/CLAUDE.md` documents the flag in the runtime-flag list
  (codex MEDIUM finding #4 fix).

### Feature gate `XEMU_METAL_FRONT_FB_DOWNLOAD={0,1}` default 0

Added at `mtl/renderer.c:240` (`mtl_front_fb_download_enabled`).
Cached on first read. Gates: the `set_draw_dirty_color/_depth` stores
in `flush_draw` (renderer.c:1276) and `clear_surface` (renderer.c:717),
the `download_dirty_all` calls in `flip_stall` (renderer.c:815) and
`surface_flush` (renderer.c:1592). When OFF, the renderer's hot path
incurs the cost of one cached `bool` read per draw + one per clear +
one per flip_stall, plus a near-empty cache walk in
`download_dirty_all` (no entries are draw-dirty when the flag is off).
When ON, the full path runs as designed.

## Validation gates

| Gate | Result |
|------|--------|
| Build (`./build.sh -a arm64`) | PASS |
| Codesign verify | PASS |
| `xemu --version` | PASS (`0.8.134-73-gb9b9c3af16`) |
| M5 shader-validation harness 7/7 | PASS |
| Cold-boot perf with flag OFF (PGR2 60 s) | 16 intervals, restored to pre-M5.10 baseline |
| Cold-boot perf with flag ON (PGR2 30 s) | METAL_SURFACE_DOWNLOADS=10, METAL_SURFACE_DOWNLOAD_BYTES=9.1 MB, METAL_FRONT_FB_PUBLISHES=1, METAL_PIPELINE_TRANSLATED_FAILED=0, METAL_DRAW_TRANSLATED == METAL_DRAW_COUNT, METAL_PIPELINE_FALLBACKS=0 |
| Visual gate at `surface_scale=2` flag ON | NOT met — download skips per-entry due to scaled-vs-guest dim mismatch (codex HIGH finding #1 defensive skip) |
| Visual gate at `surface_scale=1` flag ON | UNVERIFIED this session (would require additional run with `XEMU_DISPLAY_SCALE=1`) |
| Codex review | RAN, **MAJOR ISSUES** verdict; 4 findings, all addressed in-slice (2 HIGH fixes, 2 MEDIUM fixes) |
| GL renderer regression | NOT REGRESSED (all changes are mtl/-only or weak-symbol-gated; util/xemu-metal-perf.c counters fall back to 0 via weak symbols when surface manager isn't loaded) |

## Codex-validate findings (resolved in-slice)

1. **HIGH — scaled-surface readback corruption** (mtl/surface.mm:600).
   Was: `copyFromTexture` reads `(guest_w, guest_h)` from `(0,0)` of
   the host-scaled MTLTexture, so the destination buffer at
   `surface_scale=2` ends up holding the upper-left 1× crop instead
   of a downsampled guest-resolution image. Writing that crop to VRAM
   at `b->vram_addr` would corrupt downstream consumers.
   Fix (this slice): defensive skip — when
   `b->width != guest_w || b->height != guest_h` the function returns
   `false` and leaves `draw_dirty` set so a future call (or future
   downsample-aware slice) picks it up. Documented in CLAUDE.md and
   automation.md as a known limitation; to exercise the actual blit
   path during development set `XEMU_DISPLAY_SCALE=1`.
   Long-term fix: port vk's `vkCmdBlitImage` downscale pattern
   (vk/surface.c:221-263) using a `MTLRenderCommandEncoder` with a
   fullscreen-triangle pass that samples the host-scaled texture and
   writes a 1× scratch texture, then `copyFromTexture:toBuffer:` from
   the scratch.
2. **HIGH — KVM/HVF polling uses fixed 4 KB range** (mtl/renderer.c:1554).
   Was: `memory_region_test_and_clear_dirty(d->vram, addr, 0x1000, ...)`
   only tested the first page; framebuffers are multi-MB so guest
   writes outside the first page silently failed to mark `dirty_vram`.
   Fix: new `pgraph_mtl_surface_iter_address_size` accessor yields
   each entry's full byte size; the polling now passes the actual size
   to test_and_clear_dirty + mark_dirty_overlapping. Exercised only
   under `!tcg_enabled()`.
3. **MEDIUM — depth download spurious dirty mark** (mtl/surface.mm:580).
   Was: depth surfaces returned from `download_surface_to_vram` after
   clearing `draw_dirty`, so the caller's `download_and_notify` saw
   "draw_dirty went 1→0" and invoked the dirty-mark callback even
   though no bytes were written. Fix: function now returns
   `bool succeeded`; caller gates the callback + atomic clears on the
   real-write signal. Depth path leaves `draw_dirty` set so a future
   depth-stencil download slice picks it up cleanly.
4. **MEDIUM — flag missing from CLAUDE.md** (rule #5 enforcement).
   Was: `XEMU_METAL_FRONT_FB_DOWNLOAD` documented in
   `docs/apple-silicon/automation.md` but absent from
   `xemu-fork/CLAUDE.md`'s "Stable opt-in" runtime-flag list. Fix:
   added a comprehensive entry covering API, callers, default-off
   rationale, perf trade-off, and the `surface_scale=1` development
   workflow.

## Open follow-ups (deferred from this slice)

- **GPU-side downsample for `surface_scale > 1`**: implement the
  `vkCmdBlitImage`-equivalent scaled→1× pass so the download path
  works at the Apple Silicon system default `surface_scale=2`.
- **PGR2 back→front mechanism investigation** (continues from
  M5.9-followup-B+C): identify the actual mechanism by which the
  guest moves rendered content from `0x3628000` (back) to
  `0x32a4000` (front). M5.10's download path is correct
  infrastructure but does not bridge this gap on its own; a
  software post-process draw pass that samples the back-buffer as
  a texture and writes to the front-buffer would benefit from
  M5.10 (the texture-bind reads VRAM at the freshly-downloaded
  back-buffer address), but the metal_draw_target counter shows
  only 1-3 draws/interval to `0x32a4000` — not consistent with a
  per-frame post-process pass.
- **Eviction-time download**: extend `cache_evict_lru` and
  `cache_find_or_create_*` shape-mismatch destroy to call
  `download_if_dirty_at` before `binding_destroy(victim)` so a
  soon-to-be-destroyed draw-dirty surface's pixels make it back
  to VRAM before the texture is freed.
- **`mtl_surface_access_callback` read-handling** for synchronous
  download triggered by guest reads of a draw-dirty surface
  (mirror of `vk/surface.c:537-580::surface_access_callback`).
- **Aspect-filtered `register_access_cb_for` API** (codex MEDIUM
  from M5.9-followup-E, deferred again here).
- **Tuple-return `pgraph_mtl_surface_get_color_vram_addr`** (codex
  LOW from M5.9-followup-E, deferred again here).
- **`pgraph_mtl_get_framebuffer_surface` ops path made reachable
  from the Metal compositor** (Stage 5 of original M5.10 plan,
  deferred — would invert the current side-channel pattern).

## Files touched

- `hw/xbox/nv2a/pgraph/mtl/surface.h` — public API additions
  (download API, callback typedef, `_iter_address_size`).
- `hw/xbox/nv2a/pgraph/mtl/surface.mm` — `draw_dirty` field, the
  `download_surface_to_vram` + `download_and_notify` helpers, the
  three public download entry points, set_draw_dirty helpers,
  open-pass pin in `cache_evict_lru`, open-pass drain in
  shape-mismatch destroy paths, `_iter_address_size`, counters,
  scaled-surface defensive skip, bool return, callback gating.
- `hw/xbox/nv2a/pgraph/mtl/draw.h` — declarations of
  `pgraph_mtl_draw_get_open_pass_textures` and
  `pgraph_mtl_draw_get_done_event_state`.
- `hw/xbox/nv2a/pgraph/mtl/draw.mm` — `MTLSharedEvent
  s_draw_done_event` plus monotonic value, signal at every commit,
  accessor, init / finalize wiring.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c` — `mtl_front_fb_download_enabled`
  feature gate, `mtl_after_surface_download` callback,
  `download_dirty_all` calls in `flip_stall` and `surface_flush`,
  `set_draw_dirty` hooks in `flush_draw` and `clear_surface`,
  KVM/HVF polling in `surface_update` with TCG gate and full-size
  iterator.
- `hw/xbox/nv2a/pgraph/mtl/blit.c` — `download_if_dirty_at(src, ...)`
  + `(dst, ...)` before the CPU memcpy in `pgraph_mtl_image_blit`.
- `util/xemu-metal-perf.c` — `METAL_SURFACE_DOWNLOADS` and
  `METAL_SURFACE_DOWNLOAD_BYTES` baseline + delta + total_delta +
  emit format string + emit args; weak-symbol accessor fallbacks.
- `scripts/apple-silicon/extract-perf-summary.sh` — recognize new
  counter keys.
- `docs/apple-silicon/automation.md` — new "M5.10" subsection +
  per-counter docs.
- `xemu-fork/CLAUDE.md` — `XEMU_METAL_FRONT_FB_DOWNLOAD` flag entry
  in the "Stable opt-in" runtime-flag list.

LOC delta: ~ +900 / -25 across 9 source files + 2 docs.

## Next session

The infrastructure exists; the visual gate remains BLOCKED on
identifying PGR2's specific buffer-swap mechanism. The cheapest
next-session experiment is enabling the flag at `XEMU_DISPLAY_SCALE=1`
and capturing a screenshot at gameplay state under the M5.10 download
path — if PGR2 visual content shows up in the published front-fb,
M5.10 is sufficient infrastructure-wise and only the surface_scale=2
downsample is the remaining slice. If still magenta/empty, PGR2 is
using a fourth mechanism (likely an unimplemented NV2A engine class,
e.g. NV3089 / NV0039 / a 2D blit subchannel) and the renderer
needs that engine implemented before any Metal-side surface manager
can correctly deliver the rendered scene. M15 default-on stays
**BLOCKED**.

User-stated runtime goals (1080p, 30/60 fps, high-quality AA, correct
colors, no jitter, no high input latency) remain **MET TODAY** via
the GL renderer with `XEMU_GL_MSAA=4` + `surface_scale=2` first-launch
default + `XEMU_MACOS_NATIVE_INPUT=1`. See
`docs/apple-silicon/benchmarks/2026-05-03-multi-title-msaa-1080p-validation.md`
for the per-title validation table.
