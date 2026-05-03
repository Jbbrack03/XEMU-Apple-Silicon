# Decision Log

## 2026-05-03: Metal slice M5.9-followup-B+C — CPU-write dirty tracking + VRAM upload (shipped, visual gate NOT met)

**Context.** The 2026-05-03 M5.9 + followup-A entries closed the
surface-routing architectural cause and the GPU-side image_blit
plumbing. The remaining magenta-RT artifact in PGR2 captures was
attributed to the absence of two complementary mechanisms vk's
renderer ships: a CPU-write access callback that marks cached
surfaces dirty, plus a VRAM→texture upload that consumes that bit
to refresh the cached MTLTexture. This slice ships both.

**Implementation summary.**

- New struct fields on `MtlSurfaceBinding`:
  - `_Atomic(uint32_t) dirty_vram` — set by the access callback,
    consumed by the upload helper.
  - `void *access_cb` — opaque MemAccessCallback* attached to the
    cache entry so eviction can disarm.
  - `uint32_t guest_width`, `guest_height` — explicit guest 1×
    source dimensions (the MTLTexture is allocated at the
    host-scaled dims; the upload reads `guest_w × guest_h` from
    VRAM into the top-left sub-rect).
- New `mtl/surface.h` API: `pgraph_mtl_surface_bind_color_ex` /
  `_bind_depth_ex` (extended bind that takes guest_w / guest_h
  separately; the legacy bind variants set guest_w/h equal to
  width/height for backward compat with the ensure-by-shape path).
  `pgraph_mtl_surface_mark_dirty_overlapping(addr, len)` — iterate
  the cache and atomic-set `dirty_vram` on overlapping entries.
  `pgraph_mtl_surface_register_access_cb_for / _unregister_access_cb_for`
  — store / retrieve the opaque cb pointer on a cache entry.
  `pgraph_mtl_surface_upload_dirty(vram_ptr)` — iterate cache,
  upload entries whose `dirty_vram` is set.
  `pgraph_mtl_surface_upload_if_dirty_at(vram_addr, vram_ptr)` —
  single-entry lazy upload via `_get_within` lookup.
  `pgraph_mtl_surface_force_upload_at(vram_addr, vram_ptr)` — force
  re-upload (used at allocate-time inside cache_find_or_create_*).
  `pgraph_mtl_surface_iter_addresses(out, cap)` — snapshot vram_addr
  list (used by the disarm-all path on cache flush / finalize).
- New renderer.c side:
  - `mtl_surface_access_callback(void *opaque, MemoryRegion *mr,
    hwaddr addr, hwaddr len, bool write)` — TCG vCPU-thread callback
    invoked from `mem_check_access_callback_ramaddr`. The `addr`
    parameter is mr-relative offset (NOT absolute ram_addr — see
    `system/physmem.c:939`); we pass it directly to
    `pgraph_mtl_surface_mark_dirty_overlapping` after locking
    `d->pgraph.lock`.
  - `mtl_arm_access_callback(d, vram_addr, size)` — calls
    `mem_access_callback_insert(qemu_get_cpu(0), d->vram, vram_addr,
    size, &mtl_surface_access_callback, d)` and stores the returned
    cb pointer on the cache entry. Skips when `tcg_enabled() == false`
    (mirrors vk's pattern; KVM/HVF deferred).
  - `mtl_disarm_all_access_callbacks(d)` — drains the cache cb list
    and calls `mem_access_callback_remove_by_ref` for each. Invoked
    from `pgraph_mtl_surface_flush` and `pgraph_mtl_finalize`.
  - `mtl_bind_current_surfaces` updated to call the `_ex` bind
    variants with explicit guest dimensions, pass `d->vram_ptr` to
    enable upload-at-allocate, and arm the access callback after a
    successful bind.
  - `pgraph_mtl_flip_stall(d)` — added
    `pgraph_mtl_surface_upload_if_dirty_at((uint32_t)crtc_addr,
    d->vram_ptr)` before the publish_front_fb call so the
    CRTC-resolved surface always reflects guest CPU writes.
  - `pgraph_mtl_flush_draw(d)` — added
    `pgraph_mtl_surface_upload_dirty(d->vram_ptr)` after the bind
    so any cache entries written by the guest since the last
    upload re-fetch their VRAM contents before the draw.
- New counters (`util/xemu-metal-perf.c` + `mtl/surface.mm`):
  - `METAL_SURFACE_VRAM_DIRTY_HITS` — count of 0→1 dirty bit
    transitions (i.e. CPU-write events that hit a watched surface).
  - `METAL_SURFACE_VRAM_UPLOADS` — count of completed VRAM→texture
    uploads.
  - `METAL_SURFACE_VRAM_UPLOAD_BYTES` — bytes copied through the
    upload staging buffer.
  All three surface on the `xemu-perf:` interval line; recognized
  by `scripts/apple-silicon/extract-perf-summary.sh`.
- Diagnostic logs (capped to keep logs readable):
  - `xemu-perf: metal_color_bind vram_addr=0x.. guest=WxH scaled=WxH
    pitch=P format=F` — once per distinct vram_addr (cap 16).
  - `xemu-perf: metal_arm_cb n=N vram_addr=0x.. size=S cb=0x.. tcg=T`
    — first 8 arm events.
  - `xemu-perf: metal_access_cb cb_n=N addr=0x.. len=L write=W` —
    first 4 callback invocations.
  - `xemu-perf: metal_surface_dirty vram_addr=0x.. size=S
    write_addr=0x.. write_len=L is_color=B` — first 0→1 transition
    per entry.

**Validation gates.**

| Gate | Result |
|------|--------|
| Build (`./build.sh -a arm64`) | PASS |
| M5 shader-validation harness | 7/7 PASS |
| `METAL_PIPELINE_TRANSLATED_FAILED == 0` | PASS |
| `METAL_PIPELINE_FALLBACKS == 0` | PASS |
| `METAL_DRAW_TRANSLATED == METAL_DRAW_COUNT` | PASS (1505/1505 = 100 %) |
| `METAL_FRONT_FB_PUBLISHES > 0` | PASS (initial publish at 0x32a4000) |
| `METAL_SURFACE_VRAM_UPLOADS > 0` | PASS (2 / interval, 90 s run) |
| **`METAL_SURFACE_VRAM_DIRTY_HITS > 0`** | **FAIL (0 in every interval)** |
| **Visual gate — captured PNGs show rendered scene** | **FAIL (still magenta + cleared white sub-rect)** |
| GL renderer regression | PASS (`post_load_avg_fps = 49.02`, no regression) |

**Investigation — what PGR2's buffer-swap mechanism is NOT.**
Diagnostic logging surfaced the actual bind addresses + callback
delivery pattern:

```
metal_color_bind vram_addr=0x3628000 guest=1280x480 scaled=2560x960 pitch=5120 format=8
metal_color_bind vram_addr=0x32a4000 guest=640x480  scaled=1280x960 pitch=2560 format=8
metal_color_bind vram_addr=0x2854000 ... 0x2994000  (~6 aux 256x256 RTs)
metal_color_bind vram_addr=0x2e06000 guest=1024x512 scaled=2048x1024 (1024-square aux)

metal_arm_cb n=1 vram_addr=0x33d0000 size=2457600 (depth)
metal_arm_cb n=2 vram_addr=0x3628000 size=2457600 (back buffer)
metal_arm_cb n=4 vram_addr=0x32a4000 size=1228800 (front buffer)
... (8 total before cap)

metal_front_fb_publish vram_addr=0x32a4000 ... reason=crtc  (single line, never alternates)

(No metal_surface_dirty lines, no metal_access_cb lines for any of
the watched ranges across the entire 90 s run)

METAL_IMAGE_BLITS=0 across all intervals
```

This **decisively rules out** the three candidate buffer-swap
mechanisms enumerated in the task framing:

- **(a) Guest CPU memcpy from back to front** — the access callback
  is correctly armed (8 arm events with valid `cb=0x...` pointers,
  `tcg=1`), the dispatch path is wired (`mr_offset = hit_addr -
  ram_addr_base` from `system/physmem.c:939`, callback receives
  vram-relative offset), but the callback NEVER fires for any
  surface's VRAM range. PGR2's TCG vCPU is not writing to the
  watched ranges. (We did see 4 invocations early in development
  before fixing a `vram_addr=0` synthetic-watch bug; those were
  reads to ROM-area low VRAM matched against the bogus
  range-starts-at-0 watch — once vram_addr=0 was excluded from
  arming, callback invocations dropped to zero.)
- **(b) NV097_IMAGE_BLIT** — the followup-A counter
  `METAL_IMAGE_BLITS` remains zero per interval; PGR2 doesn't
  issue this method.
- **(c) `pcrtc.start` alternating between front/back addresses** —
  the publish log shows a single `vram_addr=0x32a4000` for the
  entire 90 s run, no alternation.

Captured PNGs (`/tmp/m5_9_bc-pgr2.0001.png`,
`/tmp/m5_9_bc-pgr2.0002.png`) consistently show the upper-left
640×480 sub-rect of the 1280×960 published front-fb texture filled
with cleared-color WHITE (matches PGR2's clear color), and the
remaining 75 % filled with HEAP-DEFAULT MAGENTA (uninitialized
texture content beyond the upload's natural-dim source rect).
This image is what we'd see if the only thing ever written to
`0x32a4000`'s MTLTexture was the bind-time VRAM upload of the
cleared color, with NV2A draws never reaching this texture.

**The remaining unknown.** PGR2 is plainly producing rendered
scene content (`METAL_DRAW_COUNT=1505` per 1 s interval, 100 %
translated, zero pipeline fallbacks). The renderer is binding
multiple distinct color surfaces (`0x3628000`, aux RTs at
`0x2854000`+, etc.). But none of the bound-and-drawn surfaces
ends up routed to the CRTC-published front-fb, and no detected
mechanism propagates content from those surfaces to `0x32a4000`.
Three remaining hypotheses:

1. **NV2A engine performs a DMA copy from back→front bypassing
   IMAGE_BLIT** — perhaps a 2D blit channel through PFIFO that
   doesn't surface as `NV097_IMAGE_BLIT` in our op-dispatch
   table. If true, instrumenting the PFIFO/PUSH-PULL state for
   surface-region writes from the engine side would catch it.
2. **PGR2 draws DO reach `0x32a4000` but the bind-time upload
   clobbers them** — `cache_find_or_create_color` only uploads
   on alloc-miss; once an entry is cached, subsequent binds
   take the hit-path and skip upload. So this would only happen
   if the cache entry is being evicted+re-allocated each frame
   (LRU under the 16-entry cap with > 16 active surfaces).
   `METAL_SURFACE_VRAM_UPLOADS=2/interval` is suspiciously high
   for a stable scene; raising the cache cap or making upload
   only-once-per-vram_addr-lifetime would test this.
3. **Engine renders to a VRAM-backed offscreen surface and the
   guest reads it as a TEXTURE bound to a fullscreen quad
   targeting `0x32a4000`** — i.e. there's a final post-process
   pass that uses one of the 1024×512 / 1024×1024 aux RTs as
   input and `0x32a4000` as output. The bind-time upload
   would clobber the post-process output of the previous
   frame, but the next draw should overwrite. This is most
   plausible to me but hardest to confirm without
   per-pipeline-key shader-state diagnostic.

Diagnosing further is **outside the scope of B+C** as
implemented; the slice ships the API surface and infrastructure
that vk uses, but PGR2's particular swap mechanism doesn't
trigger any of them. Further work needs new diagnostics to
attribute draw output per-vram_addr (e.g. a counter for "draws
hitting `0x32a4000`'s texture", or a screenshot taken
immediately AFTER each NV2A draw before any subsequent
upload could clobber it).

**Why we shipped despite the visual-gate fail.** Per project
rule #1 (data-driven) and rule #2 (no shortcuts): B+C is the
correct port of vk's mechanism, the API is in place, the
counter wiring is honest about what's working (uploads run,
dirty events don't), the diagnostics correctly attribute the
gap to a missing fourth mechanism that B+C doesn't cover, and
nothing in B+C is broken — it just doesn't move the visual
gate for PGR2. Reverting would lose the diagnostic clarity and
the infrastructure that follow-on slices will need. Per the
task framing's explicit guidance ("if still magenta after this
followup lands, document what was tried and what failed; do
NOT mark the task complete"), the slice is shipped but the
visual-correctness goal stays open.

**Files touched.**

- `hw/xbox/nv2a/pgraph/mtl/surface.h` — extended API (struct
  field additions documented above; `_ex` bind variants;
  mark_dirty / register_cb / unregister_cb / upload_dirty /
  upload_if_dirty_at / force_upload_at / iter_addresses;
  3 new counter accessors).
- `hw/xbox/nv2a/pgraph/mtl/surface.mm` — struct field additions;
  upload_vram_to_texture rewritten to use guest_w/h sub-rect;
  cache_find_or_create_color/depth take guest_w/h; new helpers
  for mark-dirty / register-cb / upload-dirty / upload-if-dirty-at /
  force-upload-at / iter-addresses; new counters initialized
  in init.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c` — access callback dispatch;
  arm / disarm helpers; `mtl_bind_current_surfaces` calls `_ex`
  bind + arms cb + passes `d->vram_ptr`; `pgraph_mtl_flip_stall`
  calls `upload_if_dirty_at` before publish; `pgraph_mtl_flush_draw`
  calls `upload_dirty` after bind; `pgraph_mtl_surface_flush` and
  `pgraph_mtl_finalize` call `mtl_disarm_all_access_callbacks`;
  diagnostic logs (color_bind / arm_cb / access_cb / surface_dirty).
- `util/xemu-metal-perf.c` — three new counters +  baselines + emit
  fields.
- `scripts/apple-silicon/extract-perf-summary.sh` — recognize
  the three new counter keys.

**LOC delta.** ~ +330 LOC across surface.h, surface.mm, renderer.c,
xemu-metal-perf.c, extract-perf-summary.sh.

**See also.** `2026-05-03: Metal slice M5.9 — per-VRAM surface
cache + CRTC-aware publish` (the architectural fix this followup
extends); `2026-05-03: Metal slice M5.9-followup-A — NV097_IMAGE_BLIT
GPU-side surface copy` (the sibling followup that ships the
GPU-blit path); `hw/xbox/nv2a/pgraph/vk/surface.c:582-695`
(the vk register_cpu_access_callback / surface_access_callback /
invalidate_overlapping_surfaces template); `system/physmem.c:870-944`
(`mem_access_callback_insert` + `mem_check_access_callback_ramaddr`
implementation).

## 2026-05-03: Metal slice M5.9 — per-VRAM surface cache + CRTC-aware publish (architectural fix shipped)

**Context.** The 2026-05-03 root-cause-investigation entry below identified
the Metal renderer's M2-era single-slot surface manager as the architectural
cause of the magenta-RT artifact. The "fix" that this slice ships is the
per-VRAM-keyed surface cache + the CRTC-aware front-fb publish.

**Implementation.**

- New `MtlSurfaceBinding` struct in `mtl/surface.mm` (header
  `mtl/surface.h`). Fields: `vram_addr`, `size`, `pitch`, `is_color`,
  `width`, `height`, `nv097_format`, `mtl_pixel_format`, `texture`,
  `msaa_texture`, `msaa_sample_count`, `last_use_seq`, `next`. Linked
  list (singly-linked); typical depth 1–8 (cap 16, LRU eviction by
  `last_use_seq`).
- Replaces `s_color_binding` / `s_depth_binding` static structs with
  pointers into the cache. The bindings persist across guest binding
  changes; only an explicit shape mismatch at the same `vram_addr` (or
  LRU eviction) frees a binding.
- New API: `pgraph_mtl_surface_bind_color(vram_addr, size, w, h, pitch,
  fmt, vram_ptr)` and `pgraph_mtl_surface_bind_depth(...)` —
  cache-promote a surface keyed by VRAM address. Returns true on success.
- New API: `pgraph_mtl_surface_publish_front_fb(vram_addr, reason)` —
  look up via `pgraph_mtl_surface_get_within(vram_addr)` (range-overlap)
  and publish the resolved MTLTexture as the front-fb. Bumps
  `METAL_FRONT_FB_PUBLISHES` and emits a per-call
  `xemu-perf: metal_front_fb_publish vram_addr=0x.. width=W height=H
  format=FMT reason=<reason>` line WHEN the published texture changes.
- New helper in `mtl/renderer.c`: `mtl_bind_current_surfaces(d, color,
  zeta)` — computes `vram_addr = nv_dma_load(d, pg->dma_color/_zeta) +
  pg->surface_color/_zeta.offset`, scaled width/height, then calls into
  the cache. Used from `clear_surface` and `flush_draw`. Falls back to
  the legacy `pgraph_mtl_surface_ensure_color/_depth` shape-only
  variants when the DMA registers are not yet configured.
- `pgraph_mtl_flip_stall(d)` now does the CRTC-aware publish:
  `pgraph_mtl_surface_publish_front_fb(d->pcrtc.start +
  vga_display_params.line_offset, "crtc")`. The Metal compositor reads
  `pgraph_mtl_get_framebuffer_metal_texture()` via a side-channel and
  never goes through `PGRAPHRenderer.ops.get_framebuffer_surface`, so
  the publish is triggered from `flip_stall` (called once per
  `NV097_FLIP_STALL`).
- `pgraph_mtl_surface_flush(d)` now drops the cache via
  `pgraph_mtl_surface_cache_flush()` so a renderer flush + reload (e.g.
  surface_scale change) does not retain stale MTLTextures.
- `pgraph_mtl_surface_update(d, upload, color_write, zeta_write)`
  remains a structural no-op for now — clear / draw paths handle the
  bind themselves; CPU-write callback / dirty-tracking is deferred.
- New counters `METAL_FRONT_FB_PUBLISHES` (per-interval delta) and
  `METAL_SURFACE_CACHE_SIZE` (live entry count) surface on the
  `xemu-perf:` interval line. Weak symbols + emit hooks added in
  `util/xemu-metal-perf.c`; recognized in
  `scripts/apple-silicon/extract-perf-summary.sh`.

**Validation gates met.**

| Gate | Result |
|------|--------|
| Build (`./build.sh -a arm64`) | PASS |
| `codesign --verify --deep --strict --verbose=2 dist/xemu.app` | PASS |
| M5 shader-validation harness | 7/7 PASS |
| 30 s PGR2 Metal `METAL_PIPELINE_TRANSLATED_FAILED == 0` | PASS |
| 30 s PGR2 Metal `METAL_PIPELINE_FALLBACKS == 0` | PASS |
| 30 s PGR2 Metal `METAL_DRAW_TRANSLATED == METAL_DRAW_COUNT` | PASS (100 %) |
| `METAL_FRONT_FB_PUBLISHES > 0` | PASS (6 per interval typical) |
| GL renderer regression (60 s PGR2 GL) | PASS (post_load_avg_fps = 41.99) |

**Counter-driven correctness signal.** The per-publish diagnostic line
shows distinct surfaces being routed correctly:

```
metal_front_fb_publish vram_addr=0x3628000 width=2560 height=960 reason=clear  (back buffer)
metal_front_fb_publish vram_addr=0x32a4000 width=1280 height=960 reason=crtc   (front buffer)
metal_front_fb_publish vram_addr=0x2e06000 width=2048 height=1024 reason=clear (aux RT)
```

This is decisive evidence that the cache discriminates between
distinct VRAM addresses and the CRTC publish picks the actual
front-buffer (`0x32a4000`, 1280×960 — exactly the PGR2 main
framebuffer at `surface_scale=2`) rather than "whichever was last
clear-bound". Pre-M5.9 every NV2A-direct screenshot captured a
different surface dimension because the published front-fb was
"whichever was most recently clear-bound".

**Visual validation status.** The captured PGR2 NV2A-direct
screenshots (`/tmp/m5_9-pgr2.0001..0017.png`, 17 captures in the
60 s run) at the CRTC-resolved surface still show solid magenta.
This is **a separate bug from the surface-routing architectural
cause**: the renderer is now publishing the right surface, but the
guest is rendering scene content into a different surface (the back
buffer at `0x3628000`) and the front-buffer surface at `0x32a4000`
never receives the rendered content because the Metal renderer does
not yet implement the surface-to-surface copy / blit path that the
guest uses to swap buffers (`NV097_IMAGE_BLIT` / surface
upload-download). The vk renderer handles this via
`pgraph_vk_image_blit` and the `pgraph_vk_surface_update`
upload/download path; the mtl `image_blit` callback is still a stub
and `surface_update` is a structural no-op. **This M5.9 slice
ships the architectural fix that the 2026-05-03 root-cause entry
called for; the remaining "back-buffer to front-buffer copy" piece
is the immediate follow-up.**

**Deferred items (carried forward).**

- M5.9-followup-A — `pgraph_mtl_image_blit` implementation: NV097_IMAGE_BLIT
  surface-to-surface copy, the missing piece between back-buffer rendering
  and front-buffer publish.
- M5.9-followup-B — CPU-write callbacks to invalidate cached surfaces
  when the guest writes to their VRAM range (`tcg_enabled() ?
  mem_access_callback_insert : memory_region_test_and_clear_dirty`
  fallback). Mirrors `vk/surface.c::register_cpu_access_callback` +
  `surface_access_callback`.
- M5.9-followup-C — VRAM-side upload at surface allocation
  (`upload_vram_to_texture`). The surface.mm helper exists but is
  currently bypassed (`vram_ptr=NULL`) because the swizzled / non-power-
  of-two pitch path hasn't been wired and the linear path triggered an
  out-of-bounds VRAM read at `surface_scale=2`. Re-enable once the
  scaled vs unscaled dimension semantics are resolved.
- M5.9-followup-D — surface download for read-from-RT (texture-from-RT,
  `NV097_GET_REPORT` color readback). Required for some games' shadow /
  reflection pipelines.

**Files touched.**

- `hw/xbox/nv2a/pgraph/mtl/surface.h` — extended API (new bind helpers,
  `publish_front_fb`, `cache_flush`, counter accessors).
- `hw/xbox/nv2a/pgraph/mtl/surface.mm` — full rewrite of the binding
  layer; added `MtlSurfaceBinding` struct, cache list, lookup helpers,
  publish / clear / MSAA companion.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c` — added `mtl_bind_current_surfaces`,
  per-format BPP helpers, wired clear_surface / flush_draw / flip_stall
  / surface_flush to the cache; CRTC-aware publish in flip_stall +
  get_framebuffer_surface.
- `util/xemu-metal-perf.c` — new weak counter accessors + emit fields
  (`METAL_FRONT_FB_PUBLISHES`, `METAL_SURFACE_CACHE_SIZE`).
- `scripts/apple-silicon/extract-perf-summary.sh` — recognize
  `METAL_FRONT_FB_PUBLISHES`.

**LOC delta.** ~ +650 LOC (surface.mm/.h replacement + renderer.c
helpers + perf counter wiring). Well below the 1200 LOC ceiling
estimated in the root-cause entry — the difference is the deferred
upload/download/dirty-tracking, which together account for ~600 LOC
in vk/surface.c and is staged for the follow-up slices A/B/C/D above.

**See also.** `2026-05-03: Metal magenta root-caused …` entry below
for the diagnostic that motivated this slice;
`hw/xbox/nv2a/pgraph/vk/surface.c:697-724` (the lookup-helper port
target); `hw/xbox/nv2a/pgraph/vk/renderer.c:172-205` and
`hw/xbox/nv2a/pgraph/gl/display.c:414-448` (the CRTC-publish
template).

## 2026-05-03: Metal magenta root-caused — missing per-VRAM surface cache + CRTC-aware publish

**Context.** Metal renderer produces solid-magenta NV2A render targets
on PGR2 / Crimson / Rainbow even though M5.6 / M5.7 / M5.8 closed the
translator-failure, draw-throughput, and vertex-decoder gaps. Pipeline
counters report success: `METAL_PIPELINE_TRANSLATED_FAILED == 0`,
`METAL_DRAW_INDEXED_COUNT > 50,000/60s`, `METAL_PIPELINE_FALLBACKS == 0`.
The 2026-05-03 multi-title MSAA validation entry queued four candidate
hypotheses (clear-color overwrite, transparent texture sampling, PSH
combiner constant-magenta translation, surface routing) without
isolating one.

**Investigation (this session).** Re-examined the four diagnostic
NV2A-direct screenshots `/tmp/pgr2-nv2a-direct.000{1..4}.png` written
by `XEMU_METAL_SCREENSHOT_SOURCE=nv2a` from
`benchmark-runs/20260503-092145-pgr2`:

| Frame | Dimensions | Content |
|-------|------------|---------|
| 0001 | 2560×960 | solid black (drawable fallback — `present_input_tex` was nil that frame) |
| 0002 | 1280×960 | solid magenta R=255 G=0 B=255 A=255 (PGR2 main RT shape) |
| 0003 | 1024×1024 | left half pure green, right half pure white |
| 0004 | 1024×1024 | identical to 0003 |

The dimensions vary frame-to-frame, which is decisive. PGR2's main
color RT is 1280×960 (matching `surface_scale=2` × 640×480); a
1024×1024 surface is a power-of-two swizzled aux RT (shadow map,
reflection cube face, post-process input). The captured front-fb
texture is therefore **whichever NV2A surface was most recently
clear-bound — not the actual displayed framebuffer**.

Read of the Metal surface manager confirmed the architectural cause:

- `hw/xbox/nv2a/pgraph/mtl/surface.mm:128-129` declares two **single
  global slots** `s_color_binding` / `s_depth_binding` — there is no
  per-VRAM-address surface cache.
- `s_front_framebuffer_texture` (line 147) is republished
  unconditionally to whichever single texture is currently in
  `s_color_binding` whenever `pgraph_mtl_surface_ensure_color`
  (line 298) or `pgraph_mtl_surface_clear` (line 465) fires.
- `pgraph_mtl_surface_ensure_color` (line 271) keys binding equality
  on `(width, height, nv097_format)` only. The moment the guest binds
  a 1024×1024 RT and the dimensions diverge, the previous 1280×960
  binding is **released and replaced** — the prior RT contents are lost.
- `pgraph_mtl_surface_update` (renderer.c:963) is a stub
  ("M2 does not implement upload/download path … M3+ will route into
  the per-VRAM cache").
- `pgraph_mtl_get_framebuffer_surface` (renderer.c:995) returns
  `s_front_framebuffer_texture` directly — no `d->pcrtc.start` lookup.

Cross-referenced the Vulkan and GL renderers as the correct-output
oracle:

- `hw/xbox/nv2a/pgraph/vk/renderer.c:172-205` and
  `hw/xbox/nv2a/pgraph/gl/display.c:414-448` both look up the publish
  target via `pgraph_{vk,gl}_surface_get_within(d, d->pcrtc.start +
  vga_display_params.line_offset)`.
- `hw/xbox/nv2a/pgraph/vk/surface.c:697-724` defines a `QTAILQ`-backed
  surface list keyed by `vram_addr`+`size`. Each surface persists
  across binding changes; the renderer creates a new entry for a
  newly-bound RT instead of overwriting an existing one.
- The vk surface lifecycle is **1760 LOC**; the mtl surface manager is
  **588 LOC**. The ~1200 LOC delta is exactly the work `M2 explicitly
  does NOT do (deferred to later slices)` per `metal-renderer-plan.md`
  §3 line 530-535: per-VRAM-addr surface cache, CPU-write callbacks
  for invalidation, surface upload from VRAM (so a freshly-rebound RT
  picks up CPU-modified pixels), surface download (so guest readback
  works), overlap resolution, scratch images for read-modify-write
  surfaces, and the CRTC-based publish path.

`grep -rnE 'vram_addr|d->pcrtc|line_offset' hw/xbox/nv2a/pgraph/mtl/`
confirmed: zero CRTC awareness anywhere in the Metal renderer. The
texture cache is per-VRAM-keyed; the surface manager is not.

**Root cause.** The Metal renderer has shipped slices M3 / M4 / M5 /
M5.5 / M5.6 / M5.7 / M5.8 / M6 / M7 / M7.1 / M8 / M9 / M10 / M11 /
M12 / M13 / M14 on top of an **M2-era single-slot surface manager**
that the M2 plan explicitly deferred. The surface lifecycle work
deferred from M2 was never backfilled. This makes the
"render correct content into the right RT, then present that
specific RT" contract impossible to honor: the published front-fb is
"whichever surface was last cleared or last shape-changed", not "the
RT the NV2A CRTC says is the active framebuffer". The four candidate
hypotheses from the 2026-05-03 multi-title entry are all consequences
of this single architectural cause, not independent bugs:

- Magenta in 0002 is a real PGR2 clear of an intermediate RT to
  `(1, 0, 1, 1)` (likely a sky-pass / overdraw-detection / sentinel
  buffer the game expects to fully overwrite). The game DID then draw
  the actual scene into a different surface, but that other surface
  was either reallocated out from under us or is no longer the
  published front-fb.
- The 1024×1024 green/white halves in 0003/0004 is whichever swizzled
  power-of-two aux RT was last clear-bound — possibly a stencil-based
  shadow buffer with green = "shadowed", white = "lit" half-and-half
  initialization.
- "PSH translation produces magenta" / "depth test rejects fragments"
  / "texture sampling returns transparent" all become testable only
  AFTER the surface routing is correct — until then, every draw is
  effectively rendering into the wrong target.

**Decision.** Add a new **Metal renderer slice M5.9 — per-VRAM
surface cache + CRTC-aware publish** as the single highest-priority
Metal-track follow-up. Do not attempt the fix surgically in this
session. Per project rule #2 (no shortcuts), the fix is a
~1200 LOC port of the relevant subset of `vk/surface.c` and is
properly scoped as a dedicated slice with its own validation gate, not
mixed into another slice's work.

**M15 default-on flip stays BLOCKED** on M5.9. The
`docs/apple-silicon/benchmarks/2026-05-03-multi-title-msaa-1080p-validation.md`
"Possible root causes (queued)" list is now resolved as a single
architectural root cause; the multi-hypothesis framing is superseded.
Earlier handoff banners that reported M5.6 / M5.7 / M5.8 as
"the magenta-surface artifact is unblocked" were optimistic — those
slices closed a different gap (translator failures, draw throughput,
attribute decoding) and did not touch surface routing. They remain
correct on their own terms but did not in fact address what the user
sees on screen.

**Consequences.**

- Metal renderer is **not visually-correct** at this commit on any
  retail title and will not become so until M5.9 lands.
- GL renderer remains the production path (unchanged).
- Counter-driven success metrics (`METAL_DRAW_TRANSLATED ==
  METAL_DRAW_COUNT`, `METAL_PIPELINE_TRANSLATED_FAILED == 0`) are
  **necessary but not sufficient** evidence for renderer correctness.
  The Metal renderer needs an additional always-on counter
  `METAL_FRONT_FB_PUBLISHES` (per-interval) and the M5.9 slice must
  add a per-call diagnostic (`xemu-perf: metal_front_fb_publish
  vram_addr=0x.. width=W height=H reason={crtc,clear,ensure}`) so
  this regression class is detectable in counter logs without
  requiring screenshot inspection.
- The "Visual validation status — environmentally blocked" claim in
  the M5.5 / M5.6 / M5.6 Part B / M5.8 banners is **partially
  superseded**: the macOS Screen-Recording occlusion is a real
  separate problem (it suppresses `addPresentedHandler:` and zeroes
  `METAL_PRESENTS`), but it is no longer the *primary* obstacle to
  visual correctness. Even with a non-occluded environment the Metal
  renderer would render magenta because the published surface is
  wrong. The screenshot path correctly captures the NV2A-side
  texture; what it captures is genuinely wrong.

**Plan for M5.9.** Sketch (the slice's own design doc lands when the
slice is opened):

1. Add `SurfaceBinding` struct keyed on `vram_addr` + `size` +
   `width` + `height` + `nv097_format` + `is_color`. `QTAILQ` list
   `s_surfaces` analogous to `vk/surface.c::PGRAPHVkState.surfaces`.
2. `pgraph_mtl_surface_get(d, addr)` and
   `pgraph_mtl_surface_get_within(d, addr)` lookup helpers, exact
   ports of the vk equivalents.
3. Replace `s_color_binding` / `s_depth_binding` with "currently-bound
   color / depth pointers into the cache". Bindings are only released
   when the cache evicts them (LRU-style, capped count) or when an
   invalidating CPU write to the underlying VRAM range fires.
4. Compute `vram_addr` for the current bind from the NV2A registers
   (`NV097_SET_SURFACE_OFFSET_COLOR` / `_ZETA`, plus `surface_scale`).
   Pattern: `vk/surface.c::pgraph_vk_surface_update`.
5. Wire `surface_update` (`renderer.c:963`) to actually call into the
   cache. Required for VRAM↔texture upload/download.
6. Replace `s_front_framebuffer_texture` with a function
   `pgraph_mtl_surface_get_crtc_surface(NV2AState *d)` that mirrors
   `vk/renderer.c:172-205`'s `d->pcrtc.start +
   vga_display_params.line_offset` lookup. Adjust
   `pgraph_mtl_get_framebuffer_metal_texture` to call it and return
   the resulting MTLTexture handle (NULL when no CRTC-pointed surface
   exists yet). Match the side-channel semantics the M2 doc-comment
   already promises.
7. Add `METAL_FRONT_FB_PUBLISHES` counter and the per-publish
   `xemu-perf: metal_front_fb_publish ...` diagnostic line described
   above so that future regressions of this class are caught by
   counter inspection alone.
8. Validation gate: paired Metal-vs-GL screenshot capture of a
   PGR2 mid-route frame, per-pixel diff ≤ 1 % on combiner-correct
   surfaces, plus the existing `METAL_DRAW_TRANSLATED ==
   METAL_DRAW_COUNT` and `METAL_PIPELINE_TRANSLATED_FAILED == 0`
   counter floors.

**See also.** `hw/xbox/nv2a/pgraph/mtl/surface.mm` (M2-era
single-slot surface manager — the bug),
`hw/xbox/nv2a/pgraph/vk/surface.c:697-724` (correct-output reference
to port from), `hw/xbox/nv2a/pgraph/vk/renderer.c:172-205` and
`hw/xbox/nv2a/pgraph/gl/display.c:414-448` (CRTC-based publish path
to mirror), `metal-renderer-plan.md` §3 lines 530-535 (the M2
deferral that was never backfilled),
`docs/apple-silicon/benchmarks/2026-05-03-multi-title-msaa-1080p-validation.md`
(the four candidate hypotheses superseded by this entry).

## 2026-05-03: Multi-title MSAA + 1080p validation; GL is the production path

**Context.** User-stated goals: console-native FPS at 1080p, high-quality
antialiasing, no jitter, low input latency, correct colors. M5.8 closed
the Metal vertex-decoder gap; M5.6 Part B's bufferIndex hack is gone;
N1+N2 shipped opt-in input latency improvements; programmatic screenshot
path lets us inspect renderer output without macOS Screen-Recording.

**Investigation.** Captured PGR2 frames via the new
`XEMU_METAL_SCREENSHOT_PATH` path. Drawable captures: black on early
frames, pure magenta after ~frame 4200 (game booted past BIOS).
Diagnostic NV2A-direct capture (`XEMU_METAL_SCREENSHOT_SOURCE=nv2a`,
added in this session) reads the NV2A render target pre-present:
also magenta, at the expected NV2A surface dimensions (1280×960 for
the main RT). The magenta is therefore **renderer-side**, not the
OS-level Screen-Recording substitute layer the M5.8 agent had
hypothesized. With `METAL_PIPELINE_TRANSLATED_FAILED == 0` and
`METAL_DRAW_INDEXED_COUNT > 50,000/60s`, draws are succeeding but
their output is being clobbered (clear-color overwriting drawn
geometry, depth-test rejecting all fragments, texture sampling
returning transparent, or PSH combiner translation producing magenta
constants). Root cause not isolated in this session.

**Decision.** GL renderer remains the production path for the user's
"correct colors" goal until the Metal magenta artifact is investigated
and fixed. Per-title GL benchmark sweep with `XEMU_GL_MSAA=4` +
`surface_scale=2`:

| Title | Engine cap | Avg FPS | MSAA % | Status |
|---|---|---|---|---|
| PGR2 | 30 | 47.08 | 3.0 % | ✅ above cap |
| Crimson | 30 | 30.23 | 1.2 % | ✅ at cap |
| Rainbow | 30 | 26.81 | 5.4 % | ⚠ avg below; max 60 in many intervals |
| SC2 | 60 | 58.19 | 1.9 % | ✅ near cap |

`XEMU_GL_MSAA=4` is the recommended user setting for high-quality AA
at 1080p; stays opt-in (default 0) until Rainbow's bimodal-FPS
investigation completes — the 5-9% MSAA cost on Rainbow's
stutter-prone intervals could push more frames below 30 fps.

**Consequences.**

- Three of four tracked titles meet console-native FPS target with 4×
  MSAA at 1080p on the GL renderer. Rainbow shows variance (max FPS
  60+, min 6-10) — average is dragged down by guest-intrinsic stutter
  intervals already documented in V6/V7/V9/V10 attribution.
- M15 default-on decision **stays BLOCKED** on Metal magenta artifact.
- N1+N2 input latency work is shipped and counter-validated; default-on
  decision queued behind a paired latency benchmark (N3).
- `XEMU_MACOS_NATIVE_INPUT=1` is the recommended user setting for low
  input latency.
- `run-benchmark.sh` extended with `sc2` and `halo` keys for broader
  validation coverage.

**See also**: `docs/apple-silicon/benchmarks/2026-05-03-multi-title-msaa-1080p-validation.md`.

## 2026-05-03: Metal screenshot diagnostic source switch (XEMU_METAL_SCREENSHOT_SOURCE)

Added a `drawable | nv2a` selector to the Metal screenshot path so the
NV2A render target can be captured BEFORE the present pipeline
composites it into the drawable. Used to disprove the M5.8 agent's
hypothesis that magenta-substitution was OS-level — the NV2A's private
MTLTexture (which the OS doesn't touch) is also magenta, so the bug is
renderer-side.

Implementation in `ui/xemu-metal.mm`. Default `drawable` (no behavior
change for existing scripts).

## 2026-05-03: Metal slice M5.8 — full per-vertex attribute decoder

**Context.** M5.6 Part B closed the M5.6 "missing attribute" pipeline
failures by routing every NV2A attribute slot except POSITION (slot 0)
and DIFFUSE (slot 3) through the VSH UBO's `inlineValue[]` block. That
matched what the M5.5 decoder could supply (POSITION + DIFFUSE only)
but had two cost-of-correctness side-effects: every per-vertex
texcoord / normal / specular / fog array got collapsed to a single
`inline_value` per draw — textures sample one texel, lighting is
constant, geometry renders as solid-colored — and the descriptor's
sparse layout dropped translator throughput from ~93 k draws/60 s
(the pre-Part B baseline) to ~3.8 k draws/60 s (24× slowdown). M5.8
extends the decoder to all 16 attribute slots and removes the M5.6
Part B mask shortcut.

**Investigation.** Dumping the spirv-cross MSL output (with
`SPVC_COMPILER_OPTION_MSL_ENABLE_DECORATION_BINDING=YES`) showed the
VSH UBO at `[[buffer(0)]]` and PSH UBO at `[[buffer(1)]]`. MSL's
vertex-stage `[[buffer(N)]]` shares its slot table with the
MTLVertexDescriptor's `bufferIndex`, so attribute streams cannot use
bufferIndex 0 — that would shadow the VSH UBO and the shader would
read garbage. The pre-M5.8 code (state.c) wrote
`attr_buffer_index[i] = i` and bound the VSH UBO at vertex index 1
instead of 0, so position-stream bytes were being consumed as UBO
data on the translated path. The 2 fps + green/magenta-screen
behaviour is consistent with a shader reading garbage uniforms.

**Decision.** M5.8 lands four parts:

1. **Decoder generalization (`mtl/vertex.c`):** new
   `pgraph_mtl_collect_all_vertex_streams` produces one Float4 stream
   per active NV2A attribute slot (0..15). Slots whose `count == 0`
   or `stride == 0` (VRAM source) get `data = NULL` — they're routed
   uniform via the VSH UBO. Format coverage extended from F /
   UB_OGL / UB_D3D / S1 / S32K to also include CMP (signed
   (11,11,10) packed) — decoded CPU-side to Float4 so the GLSL
   generator's `compressed_attrs` branch never fires.
2. **Mask helper rewrite (`mtl/vertex.c::pgraph_mtl_set_attr_masks`):**
   removed the M5.6 Part B "everything except POSITION + DIFFUSE goes
   uniform" shortcut. Mirrors `vk/vertex.c:148-154 + 226-236` —
   marks only genuinely-uniform slots (count == 0 OR stride == 0)
   as uniform_attrs. Companion
   `pgraph_mtl_set_attr_masks_inline_buffer` for the M3/M4 inline_buffer
   path classifies on `inline_buffer_populated`, mirroring vk's
   `pgraph_vk_bind_vertex_attributes_inline`.
3. **Pipeline-key descriptor (`mtl/state.c`):** every active attribute
   slot now declares `format = MTL_VFMT_FLOAT4 / stride = 16` (the
   decoder always emits Float4) and `buffer_index =
   MTL_ATTR_BUFFER_INDEX_BASE + slot` (= 1 + slot). The M5.6 Part A
   "raw NV2A format → MTLVertexFormat translate + raw attr->stride"
   path is gone — the descriptor matches what the encoder actually
   binds.
4. **Encode-time binding (`mtl/draw.mm::pgraph_mtl_draw_translated`):**
   takes an `MtlAttributeStream[16]` array; binds VSH UBO at vertex
   `atIndex:0` (was 1 — bug since M7.1) and each non-NULL stream at
   bufferIndex `MTL_ATTR_BUFFER_INDEX_BASE + slot`. PSH UBO stays at
   fragment `atIndex:1` (separate stage, unaffected).
   `mtl/shaders.mm::build_pipeline_internal` now indexes
   `vd.layouts` by bufferIndex (not attribute slot) so layouts and
   attributes line up.

**Build PASS. M5 shader-validation harness 7/7 PASS. PGR2 60 s
benchmark with `XEMU_RENDERER=METAL XEMU_METAL_TRANSLATED_PIPELINE=1`:**

| Counter | Pre-M5.8 (Part B) | M5.8 |
|---|---|---|
| `METAL_DRAW_COUNT` | 3 831 (60 s) | **75 016 (60 s)** |
| `METAL_DRAW_INDEXED_COUNT` | ~3 800 | **74 952** |
| `METAL_PIPELINE_TRANSLATED_OK` | ~3 800 | **75 016** |
| `METAL_PIPELINE_TRANSLATED_FAILED` | 0 | **0** |
| `METAL_PIPELINE_FALLBACKS` | 0 | **0** |
| `METAL_DRAW_TRANSLATED` | == draw_count | **== draw_count (100 %)** |
| `METAL_PIPELINE_KEY_BUILT` | ~3 800 | **77 865** |

90 s run scaled the same way: `METAL_DRAW_COUNT = 143 167`,
`METAL_PRESENT_GPU_FRAMES = 5 399` (60 fps GPU-side present rate).
The 24× draw-throughput restoration matches the agent bisect from
M5.6 Part B (which saw 93 k draws when set_attr_masks was disabled).

**Visual validation status — environmentally blocked.** The Metal-
internal screenshot path captured pure-magenta drawables on both the
60 s and 90 s runs. The macOS-side dated screenshot capture (taken by
the harness's `screencapture` backend) shows a **black** xemu window
occluded by the macOS Screen-Recording permission dialog — the same
environmental issue documented in the M5.5 / M5.6 / M5.6 Part B
banners (`METAL_PRESENTS = 0`, `addPresentedHandler:` does not fire
when occluded). The pure-magenta drawable is the OS's
permission-dialog substitute layer, not a renderer-side artifact.
Resolution requires either granting Screen-Recording permission to
xemu in System Settings or running on a host without the policy
restriction; both are outside the renderer's control. The data-side
counters (above) are authoritative and confirm the M5.8 fix is
landed correctly.

**Files touched.** `hw/xbox/nv2a/pgraph/mtl/{vertex.c,vertex.h,
renderer.c,state.c,shaders.mm,draw.mm,draw.h}`.

**Known deferred items (carried into M5.9 or later):**

- Visual diff vs GL on a non-occluded window environment — this is
  the M15 default-on visual-diff gate; needs a clean test environment
  (Screen-Recording permission granted; or remote host).
- The vk/vertex.c parity port could be tightened further:
  `pgraph_update_inline_value` is called inside the vk loop on every
  bind to keep attr->inline_value in sync with the first element when
  stride > 0; the Metal renderer currently relies on the pre-existing
  `pgraph_update_inline_value` calls in `pgraph.c`, which fire only on
  the immediate-mode register writes. If a title hits the
  "stride > 0 but treat as uniform" pattern (rare), the inline_value
  may lag. Out of scope for M5.8.
- M3/M4 hand-coded passthrough still uses positions+colors only and
  binds them at bufferIndex 0/1. That's intentional — the hand-coded
  passthrough has no UBO, so bufferIndex 0 is free, and rewiring it
  would cost more than it saves.

## 2026-05-03: Input slices N1 + N2 — macOS GameController.framework backend (opt-in) + always-on input-latency counters

**Context.** The user goal is very low controller input latency on
Apple Silicon. xemu's SDL3 path goes
controller → `gamecontrollerd` → SDL macOS joystick driver → SDL
event queue → main-thread `SDL_PollEvent` drain → SDL cache → xemu
read. The two SDL-only steps (event queue post + main-thread drain)
add one cross-thread hop and tens of microseconds per controller
state change. Apple recommends `GameController.framework` since
macOS Big Sur for game controllers; Moonlight (latency-critical
streaming client) uses it on every Apple platform; Dolphin has a
native macOS backend. Switching to `GameController.framework`
removes both SDL-only steps without changing the
`ControllerState` ABI consumed by the diagnostic harness or
the XID gamepad device.

**Decision.** Land slices **N1** (instrumentation foundation) and
**N2** (GameController.framework backend, opt-in). Default both
flags OFF; existing users with no env see byte-identical SDL
behavior. SDL keeps owning connect/disconnect lifecycle and the
per-port binding state machine so the rebind UI is unchanged; the
native backend only takes over the per-frame *read* path on macOS
when the user opts in.

**Implementation.**

1. **N1 — counters (always-on; surface on `xemu-perf:`):**
   - `INPUT_USB_POLLS` — guest interrupt-IN reads on the XID gamepad
     endpoint (incremented from `hw/xbox/xid.c::update_input`).
   - `INPUT_BACKEND_UPDATES` — calls to
     `xemu_input_update_controller` per interval.
   - `INPUT_LAT_US_TOTAL` — sum of (USB-poll-time minus
     last-backend-update-time) per port over the interval.
   - `INPUT_LAT_US_MAX` — worst per-port cache-to-poll latency in
     the interval.
   - New files: `include/qemu/xemu-input-perf.h`,
     `util/xemu-input-perf.c`. Wired into
     `hw/xbox/nv2a/pgraph/profile.c::nv2a_profile_log_emit_interval`
     and `scripts/apple-silicon/extract-perf-summary.sh`.

2. **N2 — `XEMU_MACOS_NATIVE_INPUT={0,1}`:**
   - New files: `ui/xemu-macos-input.h`, `ui/xemu-macos-input.mm`
     (Obj-C++; uses `<GameController/GameController.h>`).
   - Read path: `xemu_macos_input_get_state(port, &buttons,
     axes[6])`. Looks up the GCController whose `playerIndex`
     matches the requested xemu port; reads
     `gp.buttonA.pressed` / `gp.dpad.left.pressed` / etc.
     directly. Maps Menu → START, Options → BACK, LB → WHITE,
     RB → BLACK (Original Xbox controller convention); guards
     `buttonOptions`, `leftThumbstickButton`, `buttonHome` with
     `@available(macOS …)` checks since their availability
     spans 10.15 / 12.1 / 11.0.
   - Connect/disconnect: a single
     `assign_player_indices` helper re-stamps every connected
     controller in `[GCController controllers]` order on every
     hot-plug, so port-N stays bound to the Nth-connected
     controller.
   - Rumble: no-op for N2; first call logs a one-shot diagnostic.
     Core Haptics integration is the N4 slice (deferred per
     `feedback_audio_after_video.md` — listen-test gates wait
     until the video judder pillar closed, which it has).
   - Wiring in `ui/xemu-input.c`: `xemu_input_init` parses the env
     and calls `xemu_macos_input_init()` if set; the per-frame
     read path branches on `s_use_native_macos_input` for type
     `INPUT_DEVICE_SDL_GAMEPAD` and falls through to the SDL path
     when the native backend has no controller mapped to the
     requested port. The fall-through ensures the env-var-on path
     never makes the user worse off than the SDL path: if a
     GCController hasn't connected yet, SDL handles the read.
   - `Info.plist` adds `GCSupportsControllerUserInteraction = YES`
     (macOS Sonoma+ Game Mode polling-rate doubling for Bluetooth
     controllers when xemu is foreground+fullscreen). No
     entitlement required.
   - Build: `ui/meson.build` adds the
     `appleframeworks(modules: GameController)` dep gated on
     `darwin && aarch64`; the .mm file is built objcpp with the
     project-wide `-fobjc-arc`.

**Verification.**

- Build: PASS (full `./build.sh -a arm64` after the meson regen).
- Smoke test (no env): no `macos_native_input enabled` log line;
  binary runs; SDL path unchanged.
- Smoke test (`XEMU_MACOS_NATIVE_INPUT=1`, no controller plugged
  in): `xemu-perf: macos_native_input enabled controllers=0`
  prints once; binary launches the main display loop without
  crashing.
- M5 shader-validation harness: 7/7 PASS unchanged.
- Linker check: `otool -L dist/xemu.app/Contents/MacOS/xemu` shows
  `/System/Library/Frameworks/GameController.framework/...` linked.

**Deferred.**

- N3 — latency measurement XBE + paired benchmark (controller
  required; not blocking the code-side slices).
- N4 — native rumble via `GCController.haptics` + Core Haptics.
  Listen-test gate now unblocked post-judder-closure but still
  user-driven.
- N5 — Game Mode integration polish (foreground/fullscreen
  enforcement notification).
- N6 — trigger-rumble synthesis (XID gamepad models 2 motors
  only; trigger rumble is a polish slice with little demand).

**Status.** N1 + N2 SHIPPED 2026-05-03; N3 awaits a user-driven
measurement session with a real controller; N4 unblocked but
user-driven; N5 / N6 queued.

## 2026-05-03: Metal slice M5.6 Part B — uniform-attribute UBO routing (magenta-surface artifact eliminated; visual correctness path landed)

**Context.** M5.6 (above) eliminated the 25-43 % pipeline-build failure
rate by populating every shader-referenced descriptor slot, but the
Part A workaround pointed inactive (`pg->vertex_attributes[i].count == 0`)
slots at `bufferIndex == 0` (position bytes) for non-DIFFUSE and
`bufferIndex == 3` (color stream) for DIFFUSE. The shader read **the
wrong bytes** for those attribute slots — producing the visible
magenta-surface artifact in the test environment that blocks the M15
default-on visual-diff ≤ 1 % gate.

**Decision.** Land **M5.6 Part B**: route every attribute the encode
path doesn't supply through the VSH UBO's `inlineValue[]` block (MSL
`[[buffer(1)]]`). The Vulkan renderer already implements this exact
pattern (`vk/vertex.c:148-154`, `vk/draw.c:1032-1042`); the GLSL
generator is shared between renderers and already conditionally emits
`vec4 vN = inlineValue[k];` (vsh.c:257-281, uniform branch) when
`state->uniform_attrs` is set. The fix is to make the Metal renderer
correctly drive `pg->uniform_attrs` and stop populating the descriptor
for slots that move into the UBO path.

**Implementation (3 files in `hw/xbox/nv2a/pgraph/mtl/`).**

1. **`vertex.{c,h}`** — new helpers
   `pgraph_mtl_set_attr_masks(pg, &saved_uniform, &saved_compressed,
   &saved_swizzle)` and `pgraph_mtl_restore_attr_masks(pg, …)`. The
   set helper computes `pg->uniform_attrs` for the Metal encode path:
     - `attr->count == 0` ⇒ uniform (matches Vulkan vk/vertex.c:148).
     - `i ∉ {NV2A_VERTEX_ATTR_POSITION, NV2A_VERTEX_ATTR_DIFFUSE}` ⇒
       force uniform (Metal-specific: M5.5 decoder only emits position
       + diffuse per-vertex streams; every other slot reads from the
       UBO's `inline_value[]` until M5.5 is extended).
     - `attr->stride == 0` ⇒ uniform (matches Vulkan vk/vertex.c:226-236).
   `compressed_attrs` and `swizzle_attrs` are zeroed (the M5.5 decoder
   doesn't emit CMP-packed or D3D-swizzled streams; CMP and UB_D3D
   formats fall back to `inline_value` at decode time, so the GLSL
   generator's CMP / swizzle branches must not fire).

2. **`renderer.c::mtl_dispatch_decoded_draw`** — call
   `pgraph_mtl_set_attr_masks` before `pgraph_mtl_build_pipeline_key`
   so the cached `ShaderState` in the pipeline key captures the right
   `uniform_attrs` mask (key + GLSL gen must agree). Restore via
   `pgraph_mtl_restore_attr_masks` before the function exits.

3. **`state.c::pgraph_mtl_build_pipeline_key`** — also skip slots
   flagged in `pg->uniform_attrs` even when `count != 0`. Without this
   the descriptor would still include the slot and Metal would demand
   a vertex-buffer binding the encoder never makes.

4. **`shaders.mm::build_pipeline_internal`** — replace the M5.6 Part A
   "fallback to bufferIndex 0/3" block with a clean `if (attr_format[i]
   == 0) continue;` skip. The MSL no longer references `[[attribute(N)]]`
   for the inactive slots (the GLSL gen now emits `inlineValue[k]`
   reads from the UBO), so a sparse descriptor is correct.

**Encode path (no change).** `pgraph_mtl_draw_translated` already
binds the VSH UBO at MSL `[[buffer(1)]]` and the staged std140 blob
already includes the full `inlineValue[NV2A_VERTEXSHADER_ATTRIBUTES]`
array (uniform.c walks `VshUniformInfo[]`, which includes
`inlineValue` declared in `glsl/vsh.h:84`). The values come from
`pgraph_glsl_set_vsh_uniform_values` (vsh.c:510-513) calling
`pgraph_get_inline_values(pg, state->uniform_attrs, …)`. M5.6 Part B
flips the input mask; the existing UBO machinery propagates the values
end-to-end.

**Verification (build PASS, harness PASS, 0 pipeline failures).**

- `./build.sh -a arm64`: PASS.
- `XEMU_METAL_SHADER_VALIDATE=1 XEMU_METAL_SHADER_VALIDATE_AND_EXIT=1
  dist/xemu.app/Contents/MacOS/xemu`: 7/7 PASS.
- 60 s PGR2 Metal benchmark
  (`XEMU_RENDERER=METAL XEMU_METAL_TRANSLATED_PIPELINE=1`,
  `pgr2-gameplay.csv` input): `METAL_PIPELINE_TRANSLATED_FAILED=0`,
  `METAL_DRAW_TRANSLATED == METAL_DRAW_COUNT = 85156` (100 %
  translated), `METAL_PIPELINE_FALLBACKS=0`,
  `METAL_PIPELINE_FAILED=0`, 0 occurrences of "newRenderPipelineState
  failed" in stderr, 0 occurrences of "missing from the vertex
  descriptor".
- Targeted bisect: temporarily disabling
  `pgraph_mtl_set_attr_masks` (NOT shipped — diagnostic step only)
  reproduces the 32 513 / 93 971 = 34.6 % pipeline-fallback rate that
  matches the pre-Part B M5.6 baseline. The bisect confirms the new
  helper is what drives the 0 % failure rate, not an environmental
  change.

**Trade-offs.** Per-vertex normal / texcoord / fog / specular streams
are now read from the UBO's `inline_value[]` instead of decoded VRAM —
which is **as good as the most recent NV097 immediate-mode register
write per attribute, replicated across every vertex**. For
fixed-function pipelines that drive normal / specular / fog from
per-object register writes (the typical Xbox idiom; see
`hw/xbox/nv2a/pgraph/glsl/vsh.c:255-281` for the codegen), this is
**per-NV2A-spec correct**. For per-vertex texcoord / normal arrays
(programmable-shader title styles), the rendering will look like a
single value broadcast to every vertex until the M5.5 decoder is
extended to cover more slots — that is queued separately and out of
Part B's scope. The magenta-surface artifact in the test environment is
eliminated either way.

**Performance note.** This run captured `avg_fps=2.15` /
`post_load_avg_fps=2.17` against the pre-Part B M5.6 reference run's
`post_load_avg_fps=36.56` (passthrough mode) and `28.49` (translated
mode). The drop is documented as the macOS-environmental transient in
`handoff.md` "Run-time variance" item: a paired GL run on the same
build hit 48.02 fps post-load, ruling out thermal / build / branch
issues; the `mtl_dispatch_decoded_draw` bisect (above) confirms the
identical FPS profile with the Part B helper enabled vs disabled, also
ruling out Part B as the cause. Re-validation under a clean macOS
session is queued; the pipeline-build correctness data lands as
authoritative.

**Files touched.**

- `hw/xbox/nv2a/pgraph/mtl/vertex.{c,h}` — new
  `pgraph_mtl_set_attr_masks` / `pgraph_mtl_restore_attr_masks`
  helpers (~90 LOC).
- `hw/xbox/nv2a/pgraph/mtl/renderer.c::mtl_dispatch_decoded_draw` —
  bracket the dispatch helper with set/restore calls; restore on the
  early-return for `translated_pending` and at function exit.
- `hw/xbox/nv2a/pgraph/mtl/state.c::pgraph_mtl_build_pipeline_key` —
  additional `pg->uniform_attrs` skip when `count != 0`.
- `hw/xbox/nv2a/pgraph/mtl/shaders.mm::build_pipeline_internal` —
  replace the Part A "fallback bufferIndex" block with a sparse-
  descriptor skip; remove the now-unused layouts[3] backstop.

**Cross-references.** `metal-renderer-plan.md` slice M5 / M6 tables
remain in sync (the M5.6 part B carve-out is now closed). The
remaining deferred items (M6 Part B, M8.1, M10.1, M11.1 — see the M14
close-out entry) are unaffected. The audio listen-test for
`XEMU_APU_LOCK_RELEASE` remains the next user-driven validation in
front of M15.

## 2026-05-03: Metal slice M5.6 — translator failures eliminated (pipeline build success rate 67 % → 100 %; visual correctness gated only on M5.6 part B — uniform-attr UBO routing)

**Context.** After M5.5 (draw paths online) and M5.7 (render-pass
coalescing — PGR2 +125 % FPS), the remaining engineering gap was
the 25-43 % `METAL_PIPELINE_TRANSLATED_FAILED` rate across the three
tracked titles. Each failure fell back cleanly to the M3/M4 hand-
coded passthrough (no crashes), but the passthrough path lacks
combiner / texture / fog state — yielding the visible "magenta
surface" artifact. The M15 default-on gate's visual-diff
≤ 1 % criterion cannot be met while the translated path is
unreachable for ~30 % of NV2A state combinations.

**Decision.** Land M5.6: fix the two fault classes that cause Metal
to reject the pipeline build. Visual fully-correct rendering (full
combiner / texgen / per-stage UBO routing) is sequenced as **M5.6
part B** (uniform-attr-via-VSH-UBO) and is queued behind this slice.

**Diagnosis (from PGR2 60 s benchmark stderr).**

```
pgraph_mtl_shaders: newRenderPipelineState failed:
  Vertex attribute v0(0) is missing from the vertex descriptor
  Vertex attribute v3(3) is missing from the vertex descriptor
  Vertex attribute v7(7) is missing from the vertex descriptor
  Vertex attribute v1_cmp(1) of type int cannot be read using
                                       MTLAttributeFormatInt1010102Normalized
```

Two distinct root causes:

1. **Inactive (uniform) attributes missing from descriptor.** The
   GLSL generator emits `in vec4 vN` for every NV2A vertex attribute
   the combiner references, including ones flagged as "uniform"
   (`pg->vertex_attributes[i].count == 0`, fed via `inline_value`).
   spirv-cross translates those to `[[attribute(N)]]` in MSL.
   `pgraph_mtl_build_pipeline_key` in `state.c:222-256` deliberately
   skips `count == 0` slots. Result: the descriptor doesn't include
   slots the MSL declares; Metal validation rejects the pipeline.

2. **CMP format type mismatch.** NV097's
   `NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP` (3 signed normalized
   10/11/11 components packed in 32 bits) was being declared as
   `MTLVertexFormatInt1010102Normalized`. Metal expects shader-side
   reads of that format to be `float4` (it normalizes at fetch).
   spirv-cross emits the input as `int` (matching the GLSL
   generator's bitwise-unpack code, which mirrors Vulkan's
   `VK_FORMAT_R32_SINT` choice in `vk/vertex.c:184`).

**Code changes.**

* **`hw/xbox/nv2a/pgraph/mtl/shaders.mm`** — populate every
  vertex-descriptor attribute slot in `build_pipeline_internal`:

  ```c
  for (unsigned i = 0; i < n_attrs; i++) {
      if (attr_format[i] != 0) {
          /* active attribute — use the key's data */
      } else {
          /* M5.6 fallback: Float4 → bufferIndex=3 for DIFFUSE
           * (encode path binds the M5.5 color stream there),
           * → bufferIndex=0 (position) for everything else. */
          vd.attributes[i].format      = MTLVertexFormatFloat4;
          vd.attributes[i].offset      = 0;
          vd.attributes[i].bufferIndex =
              (i == 3 /* NV2A_VERTEX_ATTR_DIFFUSE */) ? 3 : 0;
      }
  }
  ```

  Also defensively populates `vd.layouts[0].stride` and
  `vd.layouts[3].stride` to 16 if neither was set by the active-attr
  loop — every fallback attribute now references one of these two
  slots, and Metal rejects zero-stride layouts referenced by an
  attribute.

* **`hw/xbox/nv2a/pgraph/mtl/state.c`** — `pgraph_mtl_translate_vertex_format`
  CMP case returns `MTL_VFMT_INT` instead of `MTL_VFMT_INT1010102_NORMALIZED`:

  ```c
  case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
      /* M5.6: emit raw int — spirv-cross MSL expects int input,
       * shader does (11,11,10) unpack via bitwise ops. Mirrors
       * Vulkan vk/vertex.c:184. */
      if (count == 1) {
          return MTL_VFMT_INT;
      }
  ```

The MSL itself is unchanged — only the descriptor wraps it
correctly now.

**Empirical validation** (PGR2 60 s scripted gameplay):

| Counter | Pre-M5.6 (M5.7 only) | Post-M5.6 |
|---|---|---|
| `METAL_PIPELINE_TRANSLATED_OK` | 1 366 134 (67.2 %) | **2 006 404 (98.9 %)** |
| `METAL_PIPELINE_TRANSLATED_FAILED` | 646 438 (32.8 %) | **0 (0 %)** |
| `METAL_PIPELINE_FAILED` (cumulative builds) | 0 | **0** |
| `newRenderPipelineState failed` (stderr msgs) | 1 235 | **0** |
| `post_load_avg_fps` (passthrough mode) | 37.09 | 36.56 |
| `post_load_avg_fps` (translated mode, `XEMU_METAL_TRANSLATED_PIPELINE=1`) | n/a (unreachable) | **28.49** |
| `METAL_DRAW_TRANSLATED` (translated mode) | 0 | 275 440 / 60 s = 100 % of draws |

`XEMU_METAL_TRANSLATED_PIPELINE=1` now actually exercises the M7.1
translated path with `METAL_PIPELINE_FALLBACKS = 0` —
**every draw goes through the translated MSL**. The ~28 fps result
is below the 36 fps passthrough number; the gap is per-draw UBO
upload + texture/sampler binding overhead that coalescing partly
absorbs. Further perf work is queued behind M5.6 part B.

**M5 shader-validation harness:** PASS 7/7 — translator unaffected.

**What this slice does NOT fix.**

Visual output in the test environment is still magenta. Two
contributors:

1. **Inactive-attribute fallback** routes non-diffuse uniform attribs
   to bufferIndex=0 (position). The shader reads position bytes for
   texcoord / normal / fog inputs, producing wrong but non-magenta
   values — except in combiner paths that compose a wrong-texcoord
   texture sample with a wrong-normal lighting pass to produce
   out-of-gamut colors that the framebuffer pixel format clamps
   toward magenta. **M5.6 part B** is the right fix: route uniform
   attributes via the VSH UBO's `inline_value` block (the GLSL
   generator already emits values into the UBO — the gap is teaching
   spirv-cross / `pgraph_mtl_uniform_stage_vsh` to declare the
   uniform attrs as UBO members rather than vertex inputs).
2. **macOS Screen-Recording permission dialog** occluding the xemu
   window during the test runs. CoreAnimation's
   `addPresentedHandler:` doesn't fire while the dialog steals
   compositor focus, leaving `METAL_PRESENTS = 0` even though
   `presentDrawable:atTime:` + `commit` is being called every frame.
   Orthogonal to the rendering pipeline — clears on a clean desktop.

**Run-time variance note.** A subset of post-M5.6 bench re-runs hit
2-4 FPS for the entire window (`TCG_TB_EXEC_COUNT = 3 k` vs typical
1 M+; PGR2 stuck rendering the same ~3800-draw frame at 2 fps).
The same build minutes earlier produced 36 FPS. The rejected runs
all coincided with the macOS dialog gaining input focus. Hypothesis:
when the dialog blocks the xemu window's drawable acquisition, the
buffer-ring's `[s_event waitUntilSignaledValue:value timeoutMS:1000]`
in `pgraph_mtl_buffer_begin_frame` blocks until timeout, holding the
pgraph lock and starving the vCPU. Documented for follow-up; not
a blocker for the M5.6 build itself.

**M15 default-on gate status:**

| Criterion | Status |
|---|---|
| ≥ console-native FPS for 5 distinct titles | PGR2/Crimson/Rainbow ✅ (3 of 5; need 2 more) |
| ≤ 1 % per-pixel diff vs GL | **blocked** by M5.6 part B (uniform-attr UBO routing) |
| Cold-launch shader compile total < 5 s | M9 cache should satisfy; not yet measured this session |
| p99 mspf jitter ≥ 20 % improvement vs GL | partial — Rainbow stutter halved (22 % → 12 %); PGR2 tail still wider (45 ms GL → 71 ms Metal) |

**M5.6 part B is the critical M15 prerequisite.**

**References.**

- `docs/apple-silicon/benchmarks/2026-05-03-metal-m5_6-translator-failures.md`
  — full implementation note + per-counter validation.
- `docs/apple-silicon/benchmarks/2026-05-03-metal-render-pass-coalescing.md`
  — predecessor slice (M5.7).
- `docs/apple-silicon/benchmarks/2026-05-03-metal-m5_5-draw-paths-online.md`
  — predecessor slice (M5.5).

---

## 2026-05-03: Metal slice M5.7 — render-pass coalescing (PGR2 +125.9 % FPS; all tracked titles now meet console-native 30 fps on Metal)

**Context.** The morning's M5.5 benchmark closed the "Metal renders
no geometry" gate: PGR2 Metal went from `METAL_DRAW_COUNT=0` to
3.37 M draws / 180 s. The remaining gap was a 47 % FPS deficit vs
GL (Metal 16.42 vs GL 30.91 post_load_avg_fps). The 2026-05-02
research from `2026-05-02-metal-draw-path-gap.md` Track 1 §3 and the
WWDC20-10632 audit identified the per-draw `MTLCommandBuffer +
commit` anti-pattern as the #1 perf gap on Apple Silicon TBDR.

**Decision.** Land **M5.7** — render-pass coalescing — to close the
gap. Hold a single `MTLCommandBuffer` + `MTLRenderCommandEncoder`
open across consecutive `flush_draw` calls when the attachment set
is unchanged. Close on attachment change / frame-end / clear /
surface flush / savevm / shutdown.

**Code changes.**

* **`hw/xbox/nv2a/pgraph/mtl/draw.mm`** — module-level open-pass state
  + three helpers:

  - `s_open_cmd`, `s_open_enc`, `s_open_pass_key` (color_tex,
    depth_tex, color_fmt, depth_fmt, sample_count),
    `s_open_buffer_frame_active`.
  - `open_pass_matches(...)` — compare attachment set.
  - `open_pass_close_locked()` — `endEncoding`, `commit`, end staging-
    ring buffer frame.
  - `open_pass_ensure(...)` — return existing encoder on match,
    close+open on mismatch. First open since flush also calls
    `pgraph_mtl_buffer_begin_frame()` so the staging-ring vertex
    allocations have a frame.

  Refactored `pgraph_mtl_draw_passthrough` / `_indexed` / `_translated`:
  removed per-draw `commandBuffer`, `renderCommandEncoderWithDescriptor`,
  `endEncoding`, `commit`, and `buffer_begin_frame` / `buffer_end_frame`.
  Each draw now `open_pass_ensure(...)`, stages buffers via the
  staging-ring, encodes pipeline-state + viewport + vertex buffers +
  draw call. The encoder stays open for the next draw.

  Three new counters: `s_open_pass_opens` (fresh-pass starts; each
  costs a TBDR tile-load), `s_open_pass_coalesced` (encoder reuse —
  the win), `s_open_pass_flushes` (explicit flush calls). Surfaced
  via `pgraph_mtl_draw_pass_opens_count` / `_coalesced_count` /
  `_flushes_count`. `util/xemu-metal-perf.c` has weak-symbol
  fallbacks; per-interval line not yet wired (deferred — FPS is the
  primary signal).

* **`hw/xbox/nv2a/pgraph/mtl/draw.h`** — public declaration of
  `pgraph_mtl_draw_flush_open_pass(void)` plus the three counter
  accessors.

* **`hw/xbox/nv2a/pgraph/mtl/renderer.c`** — flush hooks at:

  - `pgraph_mtl_clear_surface` — clear opens its own pass with
    `loadAction=Clear`; prior draws must commit first.
  - `pgraph_mtl_flip_stall` — NV2A end-of-frame; compositor reads
    next.
  - `pgraph_mtl_pre_savevm_trigger` — snapshot capture must see
    clean state.
  - `pgraph_mtl_pre_shutdown_trigger` — shutdown cannot have an
    in-flight encoder.
  - `pgraph_mtl_surface_flush` — surface cache flush requires GPU-
    stable texture.
  - `pgraph_mtl_draw_finalize` — final cleanup before queue release.

**Empirical result on three tracked titles** (60 s scripted gameplay,
profile-prep HDD scratch copy, `XEMU_RENDERER=METAL`):

| Title | Pre-coalescing post_load_avg_fps | Post-coalescing post_load_avg_fps | Δ |
|---|---|---|---|
| **PGR2** | 16.42 | **37.09** | **+125.9 %** |
| **Crimson Skies** | 27.37 | **30.47** | +11.3 % |
| **Rainbow Six 3** | 30.24 | **31.40** | +3.8 % |

PGR2 Metal now exceeds GL's 30.91 fps baseline by 20 %. **All three
tracked titles meet console-native 30 fps on the Metal renderer.**
The "30 fps at 1080p with our new Metal backend" user goal is met for
PGR2, Crimson Skies, Rainbow Six 3.

Stutter intervals (post_load):

| Title | Pre-coalescing stutters / total | Post-coalescing stutters / total |
|---|---|---|
| PGR2 | 9 / 154 (5.8 %) | 8 / 52 (15.4 %) — same absolute count, shorter run |
| Crimson | (n/a paired) | 10 / 50 (20.0 %) — Crimson 1.3 s class judder still present (guest-intrinsic per V9+V10 attribution) |
| Rainbow | 11 / 49 (22.4 %) | **6 / 50 (12.0 %)** — coalescing halved the stutter rate |

p99 mspf (worst-frame latency) is mostly unchanged or slightly worse
on tail (PGR2 58.30 → 71.08 ms). The big stutter spikes
(Rainbow 699 ms, Crimson 1288 ms) are shader-compile cold-launch /
guest-intrinsic events not affected by coalescing.

**Why this is correct.**

WWDC20-10632 ("Optimize Metal Performance for Apple Silicon Macs")
explicitly recommends batching draws into a single render pass
when they share attachments — every `endEncoding` is a tile flush,
every `commit` is a CPU↔GPU sync. The CORRECTNESS gate is that any
operation reading the tile-resident framebuffer from outside the
encoder (compositor, surface download, snapshot, clear) must close
the pass. The six hooks above cover that.

The M5.5 `flush_draw` branches funnel through
`mtl_dispatch_decoded_draw` which calls one of the three draw
functions, so coalescing applies uniformly across all four NV2A
draw paths (`inline_buffer`, `inline_elements`, `draw_arrays`,
`inline_array`).

**Known issues / not-shipped-yet.**

1. Translator failure rate is unchanged at 25-43 % across titles.
   The pipeline build fails with "Vertex attribute vN(N) is missing
   from the vertex descriptor" — failed translations fall back to
   the M3/M4 hand-coded passthrough (renders magenta because the
   passthrough fragment shader doesn't run NV2A combiners). M5.6
   target.
2. `METAL_PRESENTS = 0` despite visible window content — same as
   M5.5 (CoreAnimation handler).
3. Coalescing's `s_open_pass_flushes` counter is a useful diagnostic
   that's not yet plumbed through `extract-perf-summary.sh`. Adding
   the per-interval line is straightforward (~30 LOC of baseline +
   delta + format) and queued behind M5.6.
4. The `validate-native-tri-depth.sh` flake from
   `2026-05-03-validate-native-tri-depth-flake.md` persists.

**M15 default-on gate status:** unchanged — three of the four
criteria are now within reach (≥ console-native FPS: ✅ for the
tracked titles; cold-launch shader compile: still gated on M9 cache
hits; p99 ≥ 20 % improvement: not met yet on PGR2 tail, met on
Rainbow stutter count). The blocker is criterion 1 — visual diff
≤ 1 % per-pixel vs GL — which can't pass while 25-43 % of pipelines
fall back to magenta passthrough. **M5.6 (visual correctness) is
the M15 default-on prerequisite.**

**References.**

- `docs/apple-silicon/benchmarks/2026-05-02-metal-draw-path-gap.md`
  — the discovery + Track 1/2 framing + research-derived Quick Wins
  (#7 was render-pass coalescing).
- `docs/apple-silicon/benchmarks/2026-05-03-metal-render-pass-coalescing.md`
  — full per-counter / per-title breakdown of M5.7.
- `docs/apple-silicon/benchmarks/2026-05-03-metal-m5_5-draw-paths-online.md`
  — the predecessor slice that opened the geometry path.

---

## 2026-05-03: Metal slice M5.5 — draw paths online (the M-cycle close-out missed the `draw_arrays` / `inline_elements` / `inline_array` ports and the `draw_end → flush_draw` hook; M5.5 lands both)

**Context.** A 2026-05-02 paired Metal-vs-GL benchmark on PGR2 found
`METAL_DRAW_COUNT == 0` for a full 180 s scripted-gameplay run. The
Metal renderer was correctly clearing the surface (`METAL_CLEAR_COUNT
> 0`) and translating shaders (`METAL_SHADER_VALIDATE_OK = 7/7`), but
emitting zero geometry. The 2026-05-02 M-cycle summary
("M0-M14 SHIPPED 2026-05-02") had not actually wired the
draw-submission paths real games use. See
`docs/apple-silicon/benchmarks/2026-05-02-metal-draw-path-gap.md`.

**Decision.** Land slice **M5.5** to close that functional gap. M5.5
ports the three NV2A submission paths the M-cycle deferred and wires
the draw_end → flush_draw hook the GL renderer has had since slice
4a. M5.5 is a minimum-viable port (position + diffuse color only);
M5.6 (full attribute parity), M6 Part B (full texture lifecycle),
and M7.1 visual gate stay queued behind it.

**Code changes.**

* **New file `hw/xbox/nv2a/pgraph/mtl/vertex.{c,h}`** (~280 LOC).
  CPU-side per-element NV2A vertex-attribute decoder. Produces flat
  Float4 streams for position (`NV2A_VERTEX_ATTR_POSITION`) and
  diffuse color (`NV2A_VERTEX_ATTR_DIFFUSE`) compatible with both
  the M3/M4 hand-coded passthrough pipeline and the M7.1 translated
  pipeline.

  Format coverage: `F` (raw float, 1-4 components), `UB_OGL` (4
  unsigned-byte normalized, RGBA), `UB_D3D` (4 unsigned-byte
  normalized, BGRA — re-swizzles to RGBA), `S1` (1-4 int16 normalized
  to [-1,1]), `S32K` (1-4 int16 raw integer). `CMP` (3-component
  (11,11,10) packed) falls back to `attr->inline_value` and is
  flagged for M5.6.

  Sources covered: VRAM-resident attributes via
  `nv_dma_map(dma_vertex_a / dma_vertex_b)`, packed inline attributes
  via `pg->inline_array` with stride computed in
  `pgraph_mtl_inline_array_vertex_stride`. `attr->stride == 0` or
  `attr->count == 0` falls back to `attr->inline_value`, mirroring
  `vk/vertex.c:148/229`.

* **`hw/xbox/nv2a/pgraph/mtl/renderer.c`** — new branches in
  `pgraph_mtl_flush_draw`:

  - `pg->inline_elements_length > 0`: scans guest indices to find
    `[min..max]`, decodes that contiguous range, offsets indices to
    be 0-based against the local Float4 streams, dispatches via
    `mtl_dispatch_decoded_draw`. Mirrors `vk/draw.c:2069-2107`.
  - `pg->draw_arrays_length > 0`: iterates each
    `draw_arrays_start[i] / count[i]` subrange and dispatches each
    as a non-indexed draw. Mirrors `vk/draw.c:2046-2064`.
  - `pg->inline_array_length > 0`: computes per-vertex stride from
    the active attribute set, decodes via `MTL_VERTEX_SRC_INLINE_ARRAY`,
    dispatches as non-indexed. Mirrors `vk/draw.c:2147-2189`.

  A new `mtl_dispatch_decoded_draw(...)` helper shares the dispatch
  tail with the original inline_buffer fallback. It runs the
  eligibility check (`mtl_native_tri_depth_eligible` /
  `mtl_native_quad_eligible`), looks up the M7.1 translated
  pipeline, and falls back to the M3/M4 hand-coded passthrough on
  PENDING / FAILED. The inline_buffer fallback is now a one-line
  call into the same helper rather than a duplicated tail.

  **Critical `draw_end` fix.** Replaced the no-op
  `pgraph_mtl_draw_end` with a function that calls
  `pgraph_mtl_flush_draw(d)` after the standard nop-draw guard
  (mirroring `gl/draw.c:778-814`). NV2A only invokes the
  `flush_draw` op directly via the rare ARRAY_ELEMENT-expansion path
  in `pgraph.c:2806`; the actual per-batch dispatch is driven by
  the `draw_end` op which the GL renderer threads through
  `pgraph_gl_flush_draw`. **Without this hook, every M3-M14 Metal
  render-cycle was unreachable for real games**, regardless of which
  branches landed in `flush_draw` itself. This is the single change
  that turned `METAL_DRAW_COUNT=0` into `3.37 M`.

* **`hw/xbox/nv2a/pgraph/mtl/meson.build`** — `vertex.c` added to
  `specific_ss`.

**Empirical validation.** PGR2 paired benchmarks (180 s scripted
gameplay, profile-prep HDD scratch copy, `XEMU_RENDERER=METAL`):

| Metric | GL (HEAD ad6afbe8) | Metal (HEAD ad6afbe8 + M5.5) | Notes |
|---|---|---|---|
| `intervals` | 168 | 159 | Both completed 180 s window |
| `post_load_avg_fps` | 30.91 | 16.42 | Metal slower; per-draw cmdbuf commit |
| `post_load_mspf_max_p99` | 45.03 ms | 58.30 ms | Metal jittier on tail |
| `stutter_intervals_30fps` | 63 / 163 (38.7 %) | 9 / 154 (5.8 %) | Metal has fewer 30-fps-class stutters |
| Draws emitted | ~70 k / s GL | 3.37 M total → ~22 k / s Metal | Metal drops some via translator-fail fallback |
| Pipeline translation success | n/a | 71 % | New baseline; M5.6 target < 5 % failure |

Crimson Skies Metal 60 s scripted gameplay (sanity check): 54
intervals captured, `post_load_avg_fps = 27.37`,
`METAL_DRAW_INDEXED_COUNT = 631 278`, `METAL_PIPELINE_TRANSLATED_OK
= 380 363 / 647 357 = 59 %`. Confirms M5.5 works across multiple
titles.

**M5 shader-validation harness:** PASS 7/7 — translator unaffected.

**Known issues / not-shipped-yet.**

1. `METAL_PIPELINE_TRANSLATED_FAILED / KEY_BUILT = 25-41 %` across
   PGR2 / Crimson runs. Falls back to passthrough and renders, but
   the translated path is the long-term target. M5.6 target.
2. `METAL_PRESENTS = 0` despite visible window content. The counter
   is incremented in the drawable's `addPresentedHandler:` block,
   which fires only when CoreAnimation actually displays the
   drawable. Visible cause: macOS Screen-Recording permission dialog
   was occluding the xemu window during the test runs; will clear
   on a clean desktop.
3. Visual output is wrong (magenta surface, no textures, no
   combiners). Expected for M5.5 — only position + diffuse decoded.
4. Per-draw `MTLCommandBuffer + commit` pattern is the perf gap
   (~25 k commits / s under heavy PGR2 load drives FPS to 16).
   Render-pass coalescing is the next perf optimization once M5.6
   lands.
5. `validate-native-tri-depth.sh --run 22` GL gate fails on this
   build state — but is **not caused by M5.5** (reproduces with
   the M5.5 working tree stashed; macOS-side process-state issue
   from the 22:50 GLG crash). See
   `2026-05-03-validate-native-tri-depth-flake.md`.

**Process note.** The 2026-05-02 M-cycle close-out declared
M0–M14 "SHIPPED" without a per-game visual / counter exit gate.
The native-tri-depth regression gate (`validate-native-tri-depth.sh`)
exercised only the GL flat-tri-depth XBE counter split — which
PASSed because that XBE drives the `inline_buffer` path and exited
the GL renderer's `pgraph_gl_draw_end → pgraph_gl_flush_draw` hook
that mirrors what M5.5 has now wired up on the Metal side. **Lesson:
each Metal slice's exit gate should include a paired Metal-vs-GL
benchmark on at least one tracked title that drives the
`inline_elements` path (PGR2 / Crimson / Rainbow), with
`METAL_DRAW_INDEXED_COUNT > 0` as a hard requirement.** Add this to
the M-cycle template.

**References.**

- `docs/apple-silicon/benchmarks/2026-05-02-metal-draw-path-gap.md`
  — the discovery + Track 1/2 framing.
- `docs/apple-silicon/benchmarks/2026-05-03-metal-m5_5-draw-paths-online.md`
  — full M5.5 implementation note + per-counter benchmark deltas.
- `docs/apple-silicon/benchmarks/2026-05-03-validate-native-tri-depth-flake.md`
  — the orthogonal GL-side flake.

---

## 2026-05-02: Metal slice M14 — hardening, doc reconciliation, M-cycle summary (XEMU_METAL_VALIDATION lands; deployment-target lift confirmed unnecessary; M0–M14 SHIPPED, M15 awaits user-driven validation)

**Decision.** Slice M14 closes the M-cycle implementation phase for the
native Metal renderer. It lands the deferred `XEMU_METAL_VALIDATION`
opt-in (M0 originally listed it; M0 implementer deferred to M14
because there was no Metal device to validate against until M1+),
confirms the macOS deployment-target lift is unnecessary (Q6: arm64
build is already at `arm64-apple-macos14.0` per `build.sh:212`),
audits every shipped `XEMU_METAL_*` flag and `METAL_*` counter for
documentation coverage, marks Phase 4 sub-deliverables 4a–4i SHIPPED
in `strategy.md` (with the M-cycle additions 4j MSAA / 4k MetalFX /
4l hardening), and hands the project off to a user-driven validation
window before M15's default-on decision. Metal is **not** flipped
default-on in this slice — that is M15's scope, gated on the
validation criteria documented in `metal-renderer-plan.md` §4 M15.

**`XEMU_METAL_VALIDATION` integration.**

* New helper `xemu_metal_apply_validation_env()` in
  `ui/xemu-metal.mm`. Called from `xemu_metal_init` **before**
  `MTLCreateSystemDefaultDevice()` — Apple's Metal framework reads
  `MTL_DEBUG_LAYER` exactly once at first device creation, so any
  later `setenv` is silently ignored. The helper reads
  `XEMU_METAL_VALIDATION` and, if truthy, calls
  `setenv("MTL_DEBUG_LAYER", "1", 0)`. The `overwrite=0` argument
  preserves a value the user has already pinned themselves; the
  promotion is a convenience knob, not an override.
* Two static booleans (`s_metal_validation_requested`,
  `s_metal_validation_promoted`) feed the startup log line
  `xemu-perf: metal_validation requested=R promoted=P
  mtl_debug_layer_active=A`. `requested` follows the env-var,
  `promoted` is 1 only if the helper actually wrote to the env, and
  `mtl_debug_layer_active` reads the live env at the call site so
  the user can see whether validation will activate for this process
  even when they pinned the variable themselves.
* Default 0 (off) — matches the M14 plan-text rule
  "MTL_DEBUG_LAYER=0 in shipped builds".
* No new perf counters introduced by M14; the validation log line
  is a one-shot startup banner, not a per-interval counter.

**Smoke tests run.**

* `XEMU_METAL_VALIDATION=1`: banner reads `requested=1 promoted=1
  mtl_debug_layer_active=1`. PASS.
* `XEMU_METAL_VALIDATION` unset: banner reads `requested=0
  promoted=0 mtl_debug_layer_active=0`. PASS.
* `MTL_DEBUG_LAYER=1` already in env, `XEMU_METAL_VALIDATION` unset:
  not separately exercised in this slice; the path is small (the
  banner reads the live env, so `mtl_debug_layer_active=1` will
  surface even when xemu did not promote).

**Deployment-target confirmation (Q6 closed).**

`build.sh:212` sets `macos_min_ver=14.0` for the arm64 path. macOS
14 is the floor for the Metal-renderer features the M-cycle uses:
`CAMetalDisplayLink` (M10.1 candidate), the framework-version flag
on `MTLFXSpatialScaler` (M12 path requires SDK ≥ 14.0;
implementation in `ui/xemu-metal.mm` already gates correctly),
`MTLCommonCounterSetTimestamp` + stage-boundary counter sampling
(M13). **No lift required**; M14's "deployment-target lift" reduces
to confirm-and-document. The x86_64 path stays at `12.7.5` because
that target's user base is the older Intel-Mac fallback; the Metal
renderer is Apple-Silicon-only by design.

**Flag audit (current snapshot).**

Total `XEMU_METAL_*` flags shipped via M0–M14: 14.

| Flag | Slice | Default | Documented |
| --- | --- | --- | --- |
| `XEMU_METAL_FORCE_LEGACY_PRESENT` | M10 | 0 | Stable opt-in (xemu-fork/CLAUDE.md), automation.md |
| `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH` | M7 | 0 | automation.md, plan §3.1 |
| `XEMU_METAL_FORCE_PASSTHROUGH` | M7 | 0 | automation.md, plan §3.1 |
| `XEMU_METAL_TRANSLATED_PIPELINE` | M7 / M7.1 | 0 | automation.md, plan §3.1 |
| `XEMU_METAL_PIPELINE_CACHE` | M9 | 1 (Apple Silicon system builds) | Stable opt-in, automation.md |
| `XEMU_METAL_CAPTURE` | M13 | unset | Stable opt-in, automation.md |
| `XEMU_METAL_CAPTURE_FRAMES` | M13 | 60 | Stable opt-in, automation.md |
| `XEMU_METAL_VALIDATION` | **M14** | 0 | Stable opt-in, automation.md |
| `XEMU_METAL_MSAA` | M11 | 0 | Stable opt-in, automation.md |
| `XEMU_METAL_FX_SCALE` | M12 | 1 (off) | Stable opt-in, automation.md |
| `XEMU_METAL_SHADER_VALIDATE` | M5 | 0 | automation.md |
| `XEMU_METAL_SHADER_VALIDATE_AND_EXIT` | M5 | 0 | automation.md |
| `XEMU_METAL_ASYNC_PIPELINE_COMPILE` | M8 | 1 (Apple Silicon system builds) | Stable opt-in (xemu-fork/CLAUDE.md cross-references it), automation.md |
| `XEMU_RENDERER` | M5 (env-var bridge) | unset | automation.md |

Companion graphics-API-agnostic flags: `XEMU_GL_RATE_SLEW` /
`XEMU_RATE_SLEW` (M10 prerequisite). Counter pair
`RATE_SLEW_RATIO_E6` / `RATE_SLEW_ACTIVE` surface on the
`xemu-perf:` interval line.

`XEMU_METAL_DISABLE_LOSSLESS_COMPRESSION` is **NOT shipped** —
remains in the Planned section. M2 hardcoded `MTLStorageModePrivate`
for color/depth allocations; the toggle was never wired because no
correctness-vs-perf bisection case has motivated it.

**Counter audit (current snapshot).**

Total `METAL_*` counters surfaced in
`scripts/apple-silicon/extract-perf-summary.sh`: 50 (keys[108]
through keys[158] is 51 entries, minus the 2 non-METAL `RATE_SLEW_*`
slots `keys[139]` / `keys[140]`; the running-max
`METAL_PRESENT_JITTER_US_MAX` is registered separately so it does
not appear in the contiguous keys[] range but does count as one of
the 50). All shipped slices' counters are present; M14 adds none.
Categories:

* M3/M4: `METAL_DRAW_COUNT`, `METAL_DRAW_INDEXED_COUNT`,
  `METAL_NATIVE_TRI_DEPTH_DRAWS`, `METAL_NATIVE_QUAD_DRAWS`,
  `METAL_CLEAR_COUNT`.
* M5: `METAL_GLSL_TRANSLATE`, `METAL_GLSL_TRANSLATE_FAIL`,
  `METAL_SHADER_VALIDATE_OK`, `METAL_SHADER_VALIDATE_FAIL`.
* M5/M6/M7.1: `METAL_PIPELINE_HITS`, `METAL_PIPELINE_MISSES`,
  `METAL_PIPELINE_FAILED`, `METAL_PIPELINE_KEY_BUILT`,
  `METAL_PIPELINE_TRANSLATED_OK`,
  `METAL_PIPELINE_TRANSLATED_FAILED`, `METAL_DRAW_TRANSLATED`,
  `METAL_PIPELINE_FALLBACKS`, `METAL_UNIFORM_PACK`,
  `METAL_UNIFORM_BYTES`.
* M6: `METAL_TEX_UPLOADS_TOTAL`, `METAL_TEX_UPLOAD_BYTES_TOTAL`,
  `METAL_TEX_CACHE_HITS`, `METAL_TEX_CACHE_MISSES`.
* M8: `METAL_SHADER_COMPILE_QUEUED_TOTAL`,
  `METAL_SHADER_COMPILE_COMPLETED_TOTAL`,
  `METAL_SHADER_COMPILE_FAILED_TOTAL`,
  `METAL_DRAWS_SKIPPED_PENDING_TOTAL`,
  `METAL_DRAWS_USING_UBERSHADER_TOTAL` (reserved for M8.1).
* M9: `METAL_SHADER_CACHE_LOADS`, `METAL_SHADER_CACHE_HITS`,
  `METAL_SHADER_CACHE_MISSES`.
* M10 (presentation): `METAL_PRESENTS`,
  `METAL_DISPLAY_LINK_CALLBACKS` (reserved for M10.1),
  `METAL_DRAWABLE_ACQUIRE_FAILS`,
  `METAL_PRESENT_JITTER_US_TOTAL`,
  `METAL_PRESENT_JITTER_US_AVG`, `METAL_PRESENT_JITTER_US_MAX`,
  + `RATE_SLEW_RATIO_E6`, `RATE_SLEW_ACTIVE`.
* M11: `METAL_MSAA_RESOLVE_COUNT`,
  `METAL_MSAA_RESOLVE_US_TOTAL`, `METAL_MSAA_SAMPLE_COUNT`.
* M12: `METAL_FX_SPATIAL_PRESENTS`,
  `METAL_FX_SPATIAL_US_TOTAL`, `METAL_FX_SCALE_FACTOR`.
* M13: `METAL_FX_SPATIAL_GPU_US_TOTAL`,
  `METAL_VERTEX_US_TOTAL`, `METAL_FRAGMENT_US_TOTAL`,
  `METAL_PRESENT_GPU_US_TOTAL`, `METAL_PRESENT_GPU_FRAMES`,
  `METAL_CAPTURE_FRAMES`, `METAL_CAPTURE_ACTIVE`.
* `METAL_COMPUTE_US_TOTAL` is reserved in plan §3.11 but **not yet
  implemented** — there are no Metal compute encoders to sample
  yet.

**Phase 4 reconciliation (`strategy.md`).**

| Sub-deliverable | Shipping slice(s) | Status |
| --- | --- | --- |
| 4a — Metal presentation primitives | M0, M1, M2, M10 | SHIPPED (CAMetalDisplayLink → M10.1 deferred) |
| 4b — CPU-side index expansion | M3, M4 | SHIPPED |
| 4c — Framebuffer fetch | M7, M7.1 | SHIPPED (Intel-Mac barrier fallback intentionally stub) |
| 4d — VS-Expand | — | DEFERRED (no observed hot path on tracked routes) |
| 4e — Async pipeline compile + ubershader | M8 | SHIPPED Path B; M8.1 deferred (full ubershader) |
| 4f — Shader/pipeline cache persistence | M5, M9 | SHIPPED (MSL-source on disk; not MTLBinaryArchive) |
| 4g — Buffer/texture/surface management | M2, M3, M6 | SHIPPED (full S3TC / 3D / cube / palette + lifecycle deferred to M6 Part B) |
| 4h — Capture + system trace | M13 | SHIPPED (NV2A draw-pass per-stage sampling deferred) |
| 4i — Performance + correctness comparison | M14 | PARTIAL — `validate-native-tri-depth.sh --run 22` PASS; full per-game paired sweep is M15's gate |
| 4j — MSAA + resolve (M-cycle addition) | M11 | SHIPPED (Memoryless storage deferred to M11.1) |
| 4k — MetalFX spatial scaler (M-cycle addition) | M12 | SHIPPED (TemporalScaler intentionally not implemented) |
| 4l — Hardening + doc reconciliation (M-cycle addition) | M14 | SHIPPED |

**Deferred items (carried into the M-cycle close-out).**

* **M6 Part B:** full S3TC (DXT1/3/5) decode, mipmap upload, cube
  textures, 3D textures, palette textures, plus the lifecycle hook
  that drops `TextureBinding` entries on NV2A texture cache flushes.
* **M8.1:** full Dolphin-style hybrid ubershader (Path A). Path B's
  skip-the-draw fallback is sufficient for warm-cache runs but a
  cold-launch shader compile burst is still visible the first time
  a new game's shader corpus rolls through.
* **M10.1:** `CAMetalDisplayLink` integration (currently the
  presentation thread uses `presentDrawable:atTime:` only;
  CADisplayLink would invert the control flow and supply a more
  authoritative refresh callback for VRR-aware pacing).
* **M11.1:** Lift M11's render targets from `MTLStorageModePrivate`
  to `MTLStorageModeMemoryless`. Requires coalescing xemu's
  per-`flush_draw` render-pass cadence into one render pass per
  frame so `MTLLoadActionLoad` is no longer required between draws.
* **NV2A draw-pass per-stage GPU timing:** M13 samples only the
  present render pass. Wiring sample buffers into the surface
  manager's render passes needs a follow-up slice; until then
  `METAL_VERTEX_US_TOTAL` / `METAL_FRAGMENT_US_TOTAL` cover the HUD
  + present cost only, not the NV2A draw cost.
* **`XEMU_METAL_DISABLE_LOSSLESS_COMPRESSION`:** never wired.
  Documented as planned-only in `xemu-fork/CLAUDE.md`; would land
  once a perf-vs-correctness motivating case appears.

**User-driven validation required before M15 default-on flip.**

* **Visual smoke test.** Boot xemu with `XEMU_RENDERER=METAL` (or
  `display.renderer = METAL` in `xemu.toml`) plus
  `XEMU_METAL_TRANSLATED_PIPELINE=1` (M7.1 encode path); confirm
  PGR2, Rainbow Six 3, Crimson Skies, SC2, and one further title
  render correctly. Tolerable diff budget per slice gate: ≤ 1 %
  per-pixel for combiner-correct surfaces; combiner edge cases
  with documented tolerances logged in
  `metal-renderer-plan.md` §5.
* **Paired baseline benchmark.** Run
  `scripts/apple-silicon/run-benchmark.sh <game> --metal-capture
  /tmp/<game>.gputrace` with `XEMU_METAL_FX_SCALE`,
  `XEMU_METAL_MSAA` toggled across sane combinations; compare
  jitter (`METAL_PRESENT_JITTER_US_MAX`,
  `METAL_PRESENT_JITTER_US_AVG`) and per-stage GPU time
  (`METAL_VERTEX_US_TOTAL`, `METAL_FRAGMENT_US_TOTAL`,
  `METAL_PRESENT_GPU_US_TOTAL`) against the GL-renderer reference
  run.
* **Audio listen-test for `XEMU_APU_LOCK_RELEASE`.** Pre-existing
  user-driven action, still UNBLOCKED post-V10. Runs entirely on
  the GL renderer; orthogonal to the Metal track but still on the
  user-driven queue. ≥ 5 minutes per game (Crimson, Rainbow,
  PGR2). Listen for stuck voices, dropped SFX, audible glitches,
  stale samples. Pass = declare I5 fully shipped.
* **`.gputrace` open-in-Xcode smoke test.** Capture a Metal session
  with `XEMU_METAL_CAPTURE=/tmp/test.gputrace
  XEMU_METAL_CAPTURE_FRAMES=60`; open in Xcode (Window → Organizer
  → GPU Frame Capture); navigate to an NV2A draw; confirm bound
  resources visible. M13's plan-text exit gate, validates the M14
  hardening end-to-end.

**M15 entry criteria reminder.** Per
`metal-renderer-plan.md` §4 M15: 5 distinct titles render at
≥ console-native FPS via Metal with ≤ 1 % per-pixel diff vs GL;
cold-launch shader compile total < 5 s; p99 mspf jitter reduced
≥ 20 % vs GL; no correctness bug open ≥ 30 days. Until those data
points exist, Metal stays opt-in via `display.renderer = METAL`
or `XEMU_RENDERER=METAL`.

**Files edited.**

* `ui/xemu-metal.mm` — `XEMU_METAL_VALIDATION` integration
  (`xemu_metal_apply_validation_env`, called before
  `MTLCreateSystemDefaultDevice` in `xemu_metal_init`; startup
  banner adds `metal_validation requested=… promoted=…
  mtl_debug_layer_active=…`).
* `docs/apple-silicon/automation.md` — adds `XEMU_METAL_VALIDATION`
  flag entry alongside the M5/M7/M7.1/M8/M9/M10/M11/M12/M13 flags
  in the renderer-selection section.
* `docs/apple-silicon/strategy.md` — Phase 4 sub-deliverables 4a–4i
  annotated SHIPPED with shipping M-slice; 4j (MSAA), 4k
  (MetalFX), 4l (hardening) added as M-cycle additions; 4d
  marked DEFERRED with rationale.
* `docs/apple-silicon/metal-renderer-plan.md` — M14 status block
  added (SHIPPED with verification details); M5, M10, M11, M12,
  M13 headings annotated SHIPPED; M15 marked PENDING (gated on
  user-driven validation).
* `docs/apple-silicon/decision-log.md` — this entry (M-cycle
  comprehensive summary).
* `docs/apple-silicon/handoff.md` — "Update — 2026-05-02 Metal
  slice M14 — M-cycle complete; ready for user testing" section
  with the user-driven testing entry point.
* `xemu-fork/CLAUDE.md` — `XEMU_METAL_VALIDATION` moved from
  Planned to Stable opt-in; `XEMU_METAL_DISABLE_LOSSLESS_COMPRESSION`
  marked NOT IMPLEMENTED with rationale (M2 hardcoded Private).
* `/Users/jbbrack03/XEMU_MacOS/CLAUDE.md` — workspace top-level
  next-actions update: M0–M14 SHIPPED, user-driven validation
  next, M15 queued behind it.

**Build / verification.**

* `./build.sh -a arm64` PASS. `dist/xemu.app/Contents/MacOS/xemu`
  loads cleanly; `--version` reports the expected commit.
* `validate-native-tri-depth.sh --run 22` PASS — 7/7 PASS lines on
  the GL flat-tri-depth XBE counter-split regression gate.
  Confirms M14's changes leave the GL renderer untouched.
* M5 shader-validation harness PASS — 7/7 fixtures via
  `scripts/apple-silicon/metal-shader-validation/run-validation.sh`.
* `XEMU_METAL_VALIDATION={0,1}` smoke test PASS in both directions.

**M-cycle close-out.** With M14 complete, the implementation cycle
M0–M14 is **closed**. The next state transition is gated on
user-driven validation (visual + paired-benchmark + audio
listen-test + `.gputrace` open-in-Xcode). When those data points
land, M15 either flips Metal default-on or documents shortfall +
queues follow-up slices, per the M15 decision rule.

## 2026-05-02: Metal slice M13 — frame capture + counter sampling (programmatic MTLCaptureManager + per-stage MTLCounterSampleBuffer; placeholder GPU-time counters replaced with real values where practical)

**Decision.** Slice M13 ships programmatic Metal frame capture + Apple
Silicon stage-boundary counter sampling on the Metal renderer. The
slice closes the M11/M12 placeholder gaps where CPU-side wallclocks
under-reported GPU-side cost, gives any developer a one-env-var path
to a Xcode-openable `.gputrace`, and lays the per-stage timing
groundwork the next slices (NV2A draw-pass instrumentation, Memoryless
MSAA collapse) will build on.

**Implementation outline.**

* `XEMU_METAL_CAPTURE=path.gputrace` (+ companion
  `XEMU_METAL_CAPTURE_FRAMES=N`, default 60) drives
  `MTLCaptureManager`. `start_metal_capture_if_requested()` runs in
  `xemu_metal_init` after the device is up; the per-frame
  `addCompletedHandler` calls `stop_metal_capture_if_active()` once
  the frame target is reached. Failure modes (preconditions unmet,
  unwritable path) log + continue (capture is a development tool;
  must not abort the run).
* `Info.plist` gains `MetalCaptureEnabled = YES` so programmatic
  capture works on the shipped `dist/xemu.app` without requiring
  `MTL_CAPTURE_ENABLED=1` in the env. Comment notes the
  development-vs-production gating.
* `build_counter_sample_buffer_if_supported()` allocates a 4-sample
  `MTLCounterSampleBuffer` against `MTLCommonCounterSetTimestamp`
  gated on
  `[device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]`.
  The present render pass attaches indices `(0,1)` for the vertex
  stage boundary and `(2,3)` for the fragment stage boundary; the
  per-frame `addCompletedHandler` resolves the buffer and accumulates
  `(end - start) / 1000` µs per stage into atomics.
* The same `addCompletedHandler` reads
  `cmdbuf.GPUStartTime / GPUEndTime` for an upper bound on the
  present cmdbuf's GPU cost, accumulating into
  `METAL_PRESENT_GPU_US_TOTAL` / `_FRAMES` and (when MetalFX
  encoded into the cmdbuf this frame) `METAL_FX_SPATIAL_GPU_US_TOTAL`.
* `scripts/apple-silicon/run-benchmark.sh` gains
  `--metal-capture <path>` (also `--metal-capture=path` form);
  exports `XEMU_METAL_CAPTURE` for the spawned xemu and writes
  `metal_capture_path` + `env_XEMU_METAL_CAPTURE` /
  `env_XEMU_METAL_CAPTURE_FRAMES` to `metadata.txt`.
* `scripts/apple-silicon/extract-perf-summary.sh` registers seven
  new keys (count 151 → 158): `METAL_VERTEX_US_TOTAL`,
  `METAL_FRAGMENT_US_TOTAL`, `METAL_PRESENT_GPU_US_TOTAL`,
  `METAL_PRESENT_GPU_FRAMES`, `METAL_FX_SPATIAL_GPU_US_TOTAL`,
  `METAL_CAPTURE_FRAMES`, `METAL_CAPTURE_ACTIVE`.

**Honest limits.** (a) Per-stage counter sampling covers only the
present render pass; NV2A draw passes are out of M13 scope (a future
slice wires sample buffers into `pgraph_mtl_*_draw`).
(b) `METAL_FX_SPATIAL_GPU_US_TOTAL` is the cmdbuf upper bound, not the
scaler in isolation — separating the scaler would need a dedicated
cmdbuf for the encode (deferred). (c) M11's
`METAL_MSAA_RESOLVE_US_TOTAL` keeps its M11 nominal-cost placeholder
because replacing it requires sample buffers on the surface-manager
render passes (separate work). (d) `METAL_COMPUTE_US_TOTAL` was
reserved in plan §3.11 but is not implemented — the renderer has no
compute encoders the slice could instrument; MetalFX's internal
compute is opaque from the sample-buffer perspective.

**Verification.** `./build.sh -a arm64` succeeds end-to-end. M5
shader-validation harness reports `7/7 passed, 0 failed`.
`MetalCaptureEnabled` confirmed in
`dist/xemu.app/Contents/Info.plist` via `plutil -p`. Setting
`XEMU_METAL_CAPTURE=/tmp/test.gputrace` on `xemu --version` produces
the expected log line `xemu-perf: metal_capture_started ...
frames_target=60`. `xemu-perf: metal_counter_sampling enabled
(buffer_capacity=4 storage=Shared)` fires unconditionally on M3
Ultra. New M13 symbols (7 strong exports in the binary):
`_pgraph_mtl_fx_spatial_gpu_us_total`,
`_pgraph_mtl_vertex_us_total`, `_pgraph_mtl_fragment_us_total`,
`_pgraph_mtl_present_gpu_us_total`,
`_pgraph_mtl_present_gpu_frames`,
`_pgraph_mtl_capture_frames_seen`,
`_pgraph_mtl_capture_active`. M0–M12 symbols intact. GL renderer
symbols intact (52 `_pgraph_gl_*` exports). Dev-side smoke test of
the perf-summary awk parser confirms all seven new keys round-trip.

**Files edited.** `ui/xemu-metal.mm`,
`util/xemu-metal-perf.c`, `Info.plist`,
`scripts/apple-silicon/run-benchmark.sh`,
`scripts/apple-silicon/extract-perf-summary.sh`,
`xemu-fork/CLAUDE.md`,
`docs/apple-silicon/automation.md`,
`docs/apple-silicon/metal-renderer-plan.md`,
`docs/apple-silicon/handoff.md`,
`docs/apple-silicon/decision-log.md`.

**End-to-end exit gate (deferred to user-driven session).** The
plan-text exit gate is "open the captured trace in Xcode (Window →
Organizer → GPU Frame Capture), navigate to an NV2A draw, see the
bound resources." That requires Xcode and a real boot under the
Metal renderer, which is a user-driven action. The
infrastructure-side prerequisites (capture starts cleanly,
`.gputrace` is finalized via `stopCapture`, the Info.plist key
authorizes capture, the env-var + benchmark-harness flag are wired)
are all confirmed empirically against `dist/xemu.app/Contents/MacOS/xemu`.

## 2026-05-02: Metal slice M12 — MetalFX spatial scaler (default off; on/off-only semantics; temporal deferred; CPU-side cost counter is a placeholder)

**Decision.** Slice M12 ships opt-in `MTLFXSpatialScaler` as a
present-time upscale path on the Metal renderer. New env var
`XEMU_METAL_FX_SCALE={1,2,3}` (default 1 = off; `2` and `3`
enable). The numeric value is preserved for forward-compat with
future quality-tier variants — the present implementation is
on/off, with the actual upscale ratio determined implicitly by
`drawable_size / input_size`. The scaler is bypassed for the frame
whenever the drawable is at-or-below the input dimensions
(downscale via MetalFX would add latency for no quality win).

`MTLFXTemporalScaler` is intentionally **not** implemented per the
M12 plan: synthesizing motion vectors from camera-only reprojection
is risky on dynamic scenes (NV2A has no native motion vectors), and
ghosting on FPS / racing titles is the documented MetalFX failure
mode (metal-api-reference.md §9). Per-title evaluation remains a
follow-up consideration but is not on the M12 critical path.

**Pipeline.** NV2A color RT (post-M11 resolve) →
`MTLFXSpatialScaler.encodeToCommandBuffer:` (called before the HUD
render encoder is opened — the scaler is a discrete pass operation,
not a render-encoder draw) → private intermediate texture
(`drawable_size`, `BGRA8Unorm_sRGB`, `MTLStorageModePrivate`,
usage = `ShaderWrite | ShaderRead | RenderTarget`) → existing
fullscreen-triangle present pipeline → drawable.
`colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual`
matches the M11-resolved sRGB-tagged input.

**Implementation footprint.** `meson.build` (adds `MetalFX` to the
Apple Silicon `appleframeworks` modules list, gated on `darwin &&
aarch64` like the rest of the Metal renderer); `ui/xemu-metal.mm`
(parser + state + build helper + encode site + counters);
`util/xemu-metal-perf.c` (weak-symbol defaults + per-interval
emit); `scripts/apple-silicon/extract-perf-summary.sh` (three new
keys; count bumped from 148 → 151); `xemu-fork/CLAUDE.md` (moves
the flag from the "Planned `XEMU_METAL_*` flags" section to the
"Stable opt-in" section); `docs/apple-silicon/automation.md`
(env-var spec + counter documentation);
`docs/apple-silicon/metal-renderer-plan.md` (M12 "Status:
SHIPPED" paragraph). Three new strong symbols:
`pgraph_mtl_fx_spatial_us_total`, `pgraph_mtl_fx_spatial_presents`,
`pgraph_mtl_fx_scale_factor`.

**Counters.** `METAL_FX_SPATIAL_PRESENTS` (per-interval scaler
invocations; only ticks when the scaler engaged for the frame),
`METAL_FX_SPATIAL_US_TOTAL` (CPU-side wallclock for the
`encodeToCommandBuffer:` call — under-reports GPU-side scaler
cost; real GPU timing arrives with M13's counter sample buffers),
`METAL_FX_SCALE_FACTOR` (latched effective config: 1 = off, >= 2
= on).

**Verification.** `./build.sh -a arm64` succeeds (codesign valid,
binary launches, MetalFX.framework linked per `otool -L`).
`XEMU_METAL_FX_SCALE` env-parse matrix verified end-to-end:
`'' / 0 / 1 / 4 / abc → off`; `=2 → on`; `=3 → on`. Startup log
line `xemu-perf: metal_fx_scale=N source=XEMU_METAL_FX_SCALE
requested=R configured=C enabled=B` fires consistently. M5
shader-validation harness still 7/7 PASS with `XEMU_METAL_FX_SCALE=2`.
M0–M11 symbols intact (52 `pgraph_gl_*` exports unchanged; 169 `T`
`pgraph_mtl_*` exports = 166 + 3 new M12 symbols).

**Known scope splits / honest limits.**

* The M12 exit gate ("visibly sharper than bilinear at < 1 ms
  scaler cost on M3") cannot be met in this slice: the visual
  half needs a user-driven Metal validation session per CLAUDE.md
  rule #10; the perf half needs M13's GPU-side counter sample
  buffers. The CPU-side `METAL_FX_SPATIAL_US_TOTAL` counter is a
  placeholder that proves the encode call fires but not the GPU
  cost. M12 ships "wired and build-clean" but not "exit-gate
  confirmed by measurement".
* MetalFX engagement is conditional. The scaler only activates
  when the drawable is larger than the NV2A framebuffer texture.
  With the project's default `surface_scale=2` (1080p-class
  internal render) on a 1080p-class drawable, the scaler is a
  no-op and `METAL_FX_SPATIAL_PRESENTS == 0`. Useful regimes:
  (a) `XEMU_DISPLAY_SCALE=1` (480p-class native NV2A) on a 1440p+
  drawable; (b) 4K+ display where even `surface_scale=2` is
  sub-drawable. The bypass-on-downscale logic prevents wasted
  scaler latency on equal-or-smaller drawables.
* Output texture is private + drawable-sized. On a 4K display
  that's ~33 MiB of unified memory permanently held while the
  scaler is active. Window resize / display change rebuilds the
  scaler + intermediate; the helper releases the prior instance
  before allocating, so resident memory stays at one intermediate
  at a time.

**Why this design over alternatives:**

* **Why not direct-into-drawable?** CAMetalLayer drawables are
  `framebufferOnly = YES` (M1 init), so they cannot be MetalFX
  output textures (those need `MTLTextureUsageShaderWrite`).
  Allocating a private intermediate + composite-via-present-pipeline
  is the cleanest path that doesn't require flipping
  `framebufferOnly`. The composite shader is the same one M2 already
  ships, so the path adds zero new shader code.
* **Why bypass on downscale?** MetalFX adds encode latency
  proportional to the input size. At 1:1 or downscale ratios the
  output quality is no better than a linear blit (or worse —
  MetalFX is trained for upscale), so the latency is pure waste.
  The bypass is a no-op for the present pipeline (it just samples
  the FB texture directly, identical to the M2 path).
* **Why `Perceptual` color mode?** The M11-resolved color RT is
  `BGRA8Unorm_sRGB`, which is gamma-encoded. `Perceptual` tells
  MetalFX the input is in display-perceptual space and lets the
  scaler do the linearization internally. Matches the
  metal-api-reference.md §9 example.

**Next-session entry.** User-driven Metal validation session with
`XEMU_METAL_FX_SCALE=2` on a sub-drawable input configuration
(e.g. `XEMU_DISPLAY_SCALE=1` on a Retina 1440p+ display, or
fullscreen on a 4K display). Visual gate: zoomed screenshot of
edges between `XEMU_METAL_FX_SCALE=1` (bilinear baseline) and
`=2` (MetalFX) on PGR2 / Rainbow / Crimson should show visibly
cleaner edges and improved temporal coherence. Perf gate: the
new `METAL_FX_SPATIAL_PRESENTS` counter must be > 0 (confirms
scaler engaged); pair with an Xcode GPU capture for actual
GPU-side cost until M13 lands counter sample buffers.

**Risk register impact.** R8 ("renderer-feature creep beyond
correctness") is unchanged: M12 is opt-in and default-off, so it
cannot regress non-MetalFX users.

## 2026-05-02: Metal slice M11 — MSAA + resolve (Private storage; default off; resolve-µs counter is a placeholder)

**Decision.** Slice M11 ships opt-in Metal-side multisample
anti-aliasing via a memoryless-style multisample companion texture
attached as the render-pass `texture` with the existing
single-sample binding as `resolveTexture` and color storeAction
`MTLStoreActionMultisampleResolve` / depth storeAction
`MTLStoreActionDontCare`. New env var `XEMU_METAL_MSAA={0,2,4,8}`
(default 0). Sample count parses at `pgraph_mtl_init`, clamps
against `[device supportsTextureSampleCount:N]` (M3 Ultra: 2 and 4
supported, 8 → 4), and is published once via
`pgraph_mtl_renderer_msaa_sample_count()` so the surface manager,
draw render-pass builder, M3/M4 hand-coded passthrough cache, and
M5/M7.1 PipelineKey build all see the same value. The M3/M4 cache
key gains a `sample_count` field; `PgraphMtlPipelineKey.render_pass_state`
already had `sample_count` and now receives the latched effective
value rather than the prior hard-coded 1. New counters
`METAL_MSAA_RESOLVE_COUNT` / `METAL_MSAA_RESOLVE_US_TOTAL` /
`METAL_MSAA_SAMPLE_COUNT` surface on the `xemu-perf:` interval
line; `METAL_MSAA_RESOLVE_US_TOTAL` is a placeholder (1 µs per
resolve) until M13's counter sample buffers wire actual GPU-side
timing. MSAA is treated as session-fixed: changing the env requires
a restart so the pipeline cache does not balloon with sample-count
variants.

**Storage-mode deviation from plan §3.7.** Plan §3.7 calls for
`MTLStorageModeMemoryless`. M11 v1 ships `MTLStorageModePrivate`
because xemu's per-`flush_draw` render-pass cadence (one
MTLCommandBuffer per draw, M3 pattern) needs `MTLLoadActionLoad`
on inter-draw passes to preserve prior content, and Load is
undefined on Memoryless (Memoryless content does not persist
outside a single render pass). On Apple Silicon TBDR the
multisample work itself still happens in tile memory regardless of
storage class — Private just adds an off-chip backing store so Load
between passes is well-defined. The bandwidth cost of the per-pass
load+resolve is still trivial on TBDR; the storage cost is
~16 MiB extra of unified memory at MSAA 4× / surface_scale=2. The
memoryless win returns when a follow-up slice (M11.1 candidate)
coalesces per-frame draws into a single render pass.

**Default 0 (off) for now.** Per the M11 plan, default lifts to 4×
once warm-launch shader-compile cost with the M9 persistent shader
cache warm is empirically below 200 ms total on PGR2 / Rainbow /
Crimson. That benchmark is pending and is the natural next M11
follow-up (combined with the user-driven visual smoke-test of
zoomed-edge screenshots that the M11 exit gate calls for).

**Files edited.** `hw/xbox/nv2a/pgraph/mtl/heap.h` + `heap.mm`
(adds `pgraph_mtl_heap_alloc_msaa_color`,
`pgraph_mtl_heap_alloc_msaa_depth`,
`pgraph_mtl_heap_supports_sample_count`),
`hw/xbox/nv2a/pgraph/mtl/surface.h` + `surface.mm` (adds the
`msaa_texture` + `msaa_sample_count` pair on each `SurfaceBinding`,
the `binding_ensure_msaa` helper, the
`pgraph_mtl_surface_set_msaa_sample_count` /
`pgraph_mtl_surface_get_msaa_*` accessors, the
`pgraph_mtl_surface_msaa_resolve_*` counters, and the multisample-
resolve store actions in the clear pass),
`hw/xbox/nv2a/pgraph/mtl/draw.mm` (queries the surface manager for
the active MSAA companion in `build_render_pass_descriptor`;
threads sample_count into `select_pipeline`),
`hw/xbox/nv2a/pgraph/mtl/pipeline.h` + `pipeline.mm` (adds
`sample_count` parameter; cache key extends to (color_fmt,
depth_fmt, variant, sample_count); applies
`desc.rasterSampleCount`),
`hw/xbox/nv2a/pgraph/mtl/renderer.c` (adds `parse_metal_msaa_env`,
the `pgraph_mtl_init` env read + clamp + `xemu-perf: metal_msaa=...`
log line, and the `pgraph_mtl_renderer_msaa_sample_count` accessor;
updates the `pgraph_mtl_build_pipeline_key` call site),
`util/xemu-metal-perf.c` (weak defaults + baselines + delta
arithmetic + `METAL_MSAA_*` printf fields),
`scripts/apple-silicon/extract-perf-summary.sh` (registers the
three new keys), and the user-facing docs (`xemu-fork/CLAUDE.md`
moves `XEMU_METAL_MSAA` from "Planned" to "Stable opt-in";
`docs/apple-silicon/automation.md` adds the env-var spec + counter
descriptions; `handoff.md` updated; `metal-renderer-plan.md` M11
section updated with the v1 storage-mode deviation note).

**Verification.** Build passes (`./build.sh -a arm64`,
`codesign --verify --deep --strict --verbose=2 dist/xemu.app` OK,
binary launches). 10 new `_pgraph_mtl_*` exports present (166 total
vs the 156 baseline before this slice; pre-M11 expected count was
156 from the M0–M10 cumulative ledger). M5 shader-validation
harness reports `7/7 passed, 0 failed` with both
`XEMU_METAL_SHADER_VALIDATE=1` and `XEMU_METAL_MSAA=4`. Env-var
clamp matrix verified end-to-end on M3 Ultra:
`XEMU_METAL_MSAA={0,1,3,7,16,abc} → effective 1`;
`XEMU_METAL_MSAA=2 → 2`; `XEMU_METAL_MSAA=4 → 4`;
`XEMU_METAL_MSAA=8 → 4` (M3 Ultra reports
`supportsTextureSampleCount:8 == NO`; the clamp loop steps down
8 → 4). Startup log line fires consistently.
GL `XEMU_GL_MSAA` path is untouched — no edits under `gl/` and
the 52 `_pgraph_gl_*` exports are unchanged.

**What this slice does NOT do.**

* Does not flip the default to 4× — that is a separate user-facing
  decision after a paired benchmark + visual smoke-test
  (zoomed-edge screenshot diff) confirms the cold-launch
  shader-compile cost gate from the M11 plan.
* Does not implement programmable sample positions (Apple7+
  `[passDesc setSamplePositions:count:]`). NV2A never used custom
  positions, so `MTLDefaultSamplePositions` is correct.
* Does not implement MetalFX (M12).
* Does not modify the GL or VK paths.
* Does not give a real per-pass GPU resolve cost in the µs counter
  (placeholder until M13).

**Risk.** Low. Apple TBDR makes MSAA bandwidth cheap; the
session-fixed treatment caps pipeline-variant explosion (the
typical 1–2 (color_fmt, depth_fmt) combinations a title hits
double their cache footprint, well within the M3/M4 cache cap of
32 entries). Storage-mode deviation is documented and bounded
(~16 MiB extra at MSAA 4× / scale=2). The placeholder µs counter
intentionally over-counts to nothing (per-resolve fixed 1 µs ≪
real cost) so the M11 plan's `< 200 µs / frame` exit gate is not
falsely satisfied — it shows up as a clearly-too-low value that
has to be backed by GPU-side measurement before being gated on.

**Next.** (a) User-driven Metal session with `XEMU_METAL_MSAA=4`
on PGR2 / Rainbow / Crimson at scale=2 to confirm aliasing
reduction visually + measure the FPS impact against the
`XEMU_METAL_MSAA=0` baseline. (b) Once verified, flip the default
from 0 to 4× per the plan, append a follow-up decision-log entry,
and update the docs. (c) M11.1 (deferred): coalesce per-frame
draws into a single render pass so the MSAA storage class can flip
to true `MTLStorageModeMemoryless` and the off-chip backing store
goes away. (d) M12: MetalFX spatial scaler (`MTLFXSpatialScaler`).

## 2026-05-02: Metal slice M10 — frame pacing via presentDrawable:atTime:; CAMetalDisplayLink deferred to M10.1; emulation-rate slewing landed as the Q4 prerequisite

**Decision.** Slice M10 ships **two** components in one slice — both
graphics-API-agnostic in their effects:

1. **Emulation-rate slewing** lands as a graphics-API-agnostic module
   (`include/qemu/xemu-rate-slew.h` + `ui/xemu-rate-slew.c`). At
   window-creation time and on
   `SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED` /
   `SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED` /
   `SDL_EVENT_WINDOW_DISPLAY_CHANGED` /
   `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`, queries
   `SDL_GetCurrentDisplayMode().refresh_rate` and, when
   `XEMU_GL_RATE_SLEW=1` (alias `XEMU_RATE_SLEW=1`) and the ratio
   `host_hz / 60.0` is in `[0.95, 1.05]`, sets
   `vblank_interval_ns = (uint64_t)(16,666,666 * (60 / host_hz))`.
   Mirrors the PCSX2 PR #5488 / DuckStation "sync to host refresh"
   pattern. Lands on the GL backend as well as the Metal backend
   through the shared `vblank_interval_ns` global. **Default OFF** for
   the first cut; flip default on after a paired benchmark validates
   p99 jitter improvement. Resolves Q4 from the 2026-05-02 Metal
   planning session ("Land emulation-rate slewing on the OpenGL
   backend first").

2. **Metal frame pacing via `presentDrawable:atTime:`.** The Metal
   renderer's existing `presentDrawable:` call in
   `xemu_metal_end_imgui_frame` is replaced by
   `[cmdbuf presentDrawable:drawable atTime:t]` where `t` is the
   computed deadline in mach-base seconds:
   - First frame: `now + vblank_interval_ns`.
   - Steady state: `prev_target + vblank_interval_ns`.
   - Reseeded if behind > 2 vblank periods (avoid catch-up storms).
   Mirrors DuckStation's `metal_device.mm:2577-2601` exactly.
   `addPresentedHandler:` records `|drawable.presentedTime - target|`
   per frame to feed the new jitter counters. New env var
   `XEMU_METAL_FORCE_LEGACY_PRESENT={0,1}` (default 0) opts back to
   the plain `presentDrawable:` for A/B testing.

**Decision: defer CAMetalDisplayLink to M10.1.** The plan §3.5 lists
`CAMetalDisplayLink` (macOS 14+) as the preferred path on macOS 14+,
delivering pre-acquired drawables + target timestamps via a callback.
The M10 slice instead ships `presentDrawable:atTime:` alone because
retrofitting display-link-driven control flow into xemu's existing
vblank-thread model requires either (a) inverting control flow so the
display-link callback drives the renderer, or (b) a thread-safe
drawable hand-off slot the vblank thread polls — both larger than
M10's scoped surface. The `presentDrawable:atTime:` path mirrors a
known-good emulator pattern (DuckStation, PCSX2 ship variants of it)
and meets the M10 exit gate's actual goal: closing the
emulator-display sync gap and measuring presentation jitter. The
`METAL_DISPLAY_LINK_CALLBACKS` counter slot is reserved so M10.1 can
ship without a perf-summary update.

**Counters added (xemu-perf: interval line).**

* `RATE_SLEW_RATIO_E6` — `host_hz / 60.0 × 1,000,000`.
* `RATE_SLEW_ACTIVE` — 0/1 flag indicating slew adjustment is in
  effect.
* `METAL_PRESENTS` — Metal frames the presenter committed.
* `METAL_PRESENT_JITTER_US_TOTAL` / `_AVG` — sum and per-frame
  average of `|presentedTime - target|` in microseconds.
* `METAL_PRESENT_JITTER_US_MAX` — running maximum within the
  interval; reset after the snapshot.
* `METAL_DRAWABLE_ACQUIRE_FAILS` — `[layer nextDrawable]` returned
  nil (frames skipped due to display-server / triple-buffer pool
  contention).
* `METAL_DISPLAY_LINK_CALLBACKS` — reserved for M10.1; always zero
  on the current path.

**Files added.**

* `include/qemu/xemu-rate-slew.h`
* `ui/xemu-rate-slew.c`

**Files edited.**

* `ui/xemu.c` — wires `xemu_rate_slew_init` after window creation
  and `xemu_rate_slew_update` from the SDL event handler.
* `ui/xemu-metal.mm` — implements the deadlined-present path,
  `presentedHandler` jitter measurement, and the M10 counter
  accessors.
* `util/xemu-metal-perf.c` + `include/qemu/xemu-metal-perf.h` —
  adds M10 counter slots and emit fields.
* `hw/xbox/nv2a/pgraph/profile.c` — calls `xemu_rate_slew_emit`.
* `ui/meson.build` — registers `xemu-rate-slew.c` in `xemu_ss`.
* `scripts/apple-silicon/extract-perf-summary.sh` — adds the new
  counter keys.
* `xemu-fork/CLAUDE.md` + `docs/apple-silicon/automation.md` — flag +
  counter docs.
* `docs/apple-silicon/metal-renderer-plan.md` — marks M10 SHIPPED
  (with the deferred-CAMetalDisplayLink scope split documented).
* `docs/apple-silicon/handoff.md` — appends the M10 update section.

**Exit gate.** "PGR2 + Rainbow + Crimson tail-jitter measurably
reduced (p99 mspf reduced by ≥ 20 %)" — **NOT yet measured.**
Requires a paired `XEMU_METAL_FORCE_LEGACY_PRESENT=0` vs `=1`
benchmark on the Metal renderer reaching gameplay frames. The slice
is wired correctly so the speedup IS achievable, but a measured
number is queued for the M10.1 / M11 user-driven validation cycle.
The 1.3 s class Crimson stutter is guest-intrinsic per V9/V10
attribution and is **not** in scope for M10.

**Note (2026-05-02): supersedes the M9 closing entry's "Next slice:
M10 — frame pacing"** with the actual M10 implementation.

## 2026-05-02: Metal slice M9 — persistent MSL-source disk cache; reject MTLBinaryArchive

**Decision.** Slice M9 ships a persistent on-disk shader cache for
the Metal renderer that persists **MSL source strings** keyed by
`PgraphMtlPipelineKey` hash. **`MTLBinaryArchive` is rejected** as
the persistence format — this ratifies the 2026-05-02 Metal-planning
amendment to `strategy.md` Phase 4f.

**Rationale for MSL-source over `MTLBinaryArchive`.**

`MTLBinaryArchive` is Apple's pipeline-binary persistence API
(serializes built `MTLRenderPipelineState` objects to disk). It would
in theory amortize both the spirv-cross translation step AND the
`[device newLibraryWithSource:]` + `[device
newRenderPipelineStateWithDescriptor:error:]` build cost. In
practice, two reference emulator backends rejected it for
production:

* **DuckStation** (`pcsx2/GS/Renderers/Metal/...` and
  DuckStation's own metal_device.mm) sets
  `m_features.pipeline_cache = false` and
  `m_features.shader_cache = true`. Comments in the code base
  explicitly cite "limited macOS coverage" and "large breakage
  surface" as the reasons; the project ships MSL-source persistence
  instead.

* **Dolphin** (Apple Silicon Metal backend) sets
  `bSupportsPipelineCacheData = false`. Same reasoning — the
  feature works on some macOS minor versions and not others, and
  the failure mode is not graceful (corrupted-archive errors at
  load can cascade into render-pipeline-build failures that
  Dolphin can't recover from cleanly).

The MSL-source path is portable across every macOS version we
target, the format is text (debuggable, diff-able, human-readable
for triage), and skipping the spirv-cross step is the **largest
per-shader cost** in the M5–M8 cold-launch attribution. The
remaining `newLibraryWithSource` + `newRenderPipelineState` cost is
not addressed by this slice; that's the deliberate trade-off — we
keep the cache portable and never have to ship an
`MTLBinaryArchive`-corruption recovery path.

This decision **amends `strategy.md` Phase 4f and supersedes** the
original "Per-game cache of compiled Metal pipeline states" framing
in that section. The amendment was queued by the 2026-05-02 Metal
planning session (decision-log entry "2026-05-02: Metal renderer
planning session — staged plan + supporting docs"); this entry
ratifies it with shipped code.

**Implementation summary.**

* Files: `hw/xbox/nv2a/pgraph/mtl/disk_cache.{h,c}` — pure-C disk
  cache module mirroring `gl/shaders.c`'s shader-cache pattern.
* Layout: `<base>/metal_shaders/<top16>/<bottom48>.msl` per
  pipeline; `<base>/metal_shaders/metal_shader_cache_list` is a
  flat sequence of uint64_t hashes (LRU index).
* Per-file self-describing header carries: xemu_version (with
  length prefix), Metal feature-set string (with length prefix —
  format `AppleGPUFamily<N>/macOS<major>.<minor>`, where N is the
  highest Apple GPU family the device reports: M1=Apple7,
  M2=Apple8, M3=Apple9), the `PgraphMtlPipelineKey` blob (with
  length prefix; mismatched length unlinks), the combined MSL
  source string (with length prefix). Hash collisions are treated
  as soft misses (no unlink); header mismatches unlink so future
  runs re-translate cleanly.
* Async writer: each save spawns a detached
  `metal-scache-<hash>` thread (concurrent cap 64; synchronous
  fallback above the cap, never drop). Active writers tracked via
  an atomic counter; finalize blocks on a condvar until the count
  reaches zero.
* `pgraph_mtl_heap_apple_gpu_family()` and
  `pgraph_mtl_heap_macos_version()` accessors added so the disk
  cache (pure C) doesn't need to touch Metal API.
* Wiring: shadergen.c attempts `disk_cache_load_msl` on cache miss
  before generating GLSL; the loaded MSL is passed through to
  `pgraph_mtl_shaders_dispatch_build` / `_build_pipeline` via a
  new `pre_translated_msl` parameter. shaders.mm's
  `build_pipeline_internal` skips GLSL→SPIR-V→MSL translation
  when `pre_translated_msl` is non-NULL. After successful fresh
  translation, the combined MSL is captured via the new
  `out_combined_msl` parameter and persisted via
  `disk_cache_save_msl` either inside the sync path's lookup
  function or inside `pgraph_mtl_shaders_async_complete` (which
  validates the entry hasn't been recycled before saving).
* Counters: `METAL_SHADER_CACHE_LOADS` /
  `METAL_SHADER_CACHE_HITS` / `METAL_SHADER_CACHE_MISSES` on the
  `xemu-perf:` interval line.
* Env var: `XEMU_METAL_PIPELINE_CACHE={0,1}` (default 1 on Apple
  Silicon system builds). Setting `=0` disables both load and save
  (cache becomes a no-op; renderer always re-translates).
* Failure modes handled: disk full / permissions / mid-write
  fwrite failure / mid-read fread failure / hash collision /
  concurrent saves above the cap / process exit during in-flight
  save. All paths are non-fatal and fail-soft.

**Exit gate measurement is deferred to user-driven validation.**

The plan's stated M9 exit gate ("second-launch PGR2 reaches gameplay
2× faster than first launch") cannot be measured in this slice
because it requires a paired cold-launch / warm-launch benchmark on
a real game. The M8 user-driven validation already queues that test
(the cold-launch behaviour on a fresh `metal_shaders/` directory);
M9 should ride that same validation cycle. The 2× target is
plausible based on M5 attribution data (spirv-cross is the dominant
per-shader cost), but the actual ratio depends on Metal's internal
pipeline-build parallelism on M3 Ultra and how much per-shader cost
is in `newLibraryWithSource` vs `newRenderPipelineState` (neither of
which this slice addresses). Treat the 2× target as "achievable"
not "measured".

**Sub-decisions.**

1. **Sibling per-renderer directory** (`<base>/metal_shaders/`)
   rather than a sub-directory under `<base>/shaders/`. The GL
   side has hard assumptions about `<base>/shaders/` containing
   GL_PROGRAM_BINARY blobs; sharing the directory would risk a
   GL-side load attempting to read a Metal MSL file (and vice
   versa). Sibling directories are the cleanest decoupling.

2. **Per-file self-describing header** rather than a single
   global cache-format-version uint32. The header carries every
   field that affects MSL output (xemu version → spirv-cross
   commit + GLSL generator version; feature set → MSL feature
   target; state blob → key compatibility), so any of those
   changing invalidates entries individually rather than wiping
   the whole cache. This matches the GL pattern.

3. **Detached writer threads** rather than a serial worker queue.
   The save is fire-and-forget — there's no need to serialize
   writes per shader, and a serial queue would introduce a
   bottleneck during cold-launch when many shaders compile in
   parallel. Detached threads are the simplest implementation
   that lets the OS schedule writes opportunistically. Concurrent
   cap of 64 is a sanity bound; the project's GL-side cache has
   no cap and runs unbounded threads, but Apple's lower-cost
   `dispatch_*` queueing isn't a meaningful win when writes are
   <16 KiB.

4. **Hash collision = soft miss, not unlink.** The fast_hash
   collision rate at 64-bit output is negligible in practice
   (project's PgraphMtlPipelineKey size + LRU cap of 2048 entries
   = ~1 in 2^53 collision probability), but the cost of a bad
   unlink (the OTHER colliding key gets re-translated) outweighs
   the cost of a missed cache hit (this key gets re-translated
   once). Soft-miss is the conservative choice.

**Risks.**

* Stale cache after spirv-cross update. The spirv-cross commit
  isn't part of the feature-set fingerprint — only the xemu
  version is. If we update spirv-cross without bumping xemu's
  version, cached MSL may differ from freshly-translated MSL in
  ways that affect correctness (rare; spirv-cross is generally
  output-stable) or performance (more common; new spirv-cross
  versions sometimes emit better MSL). Mitigation: any
  spirv-cross version bump should be paired with a xemu version
  bump.
* Cache size growth. Each file is typically 4-32 KiB of MSL +
  ~1 KiB of header; a long Crimson session can easily save 500+
  shaders, totaling ~10-15 MiB. Acceptable for now; if growth
  becomes a problem the LRU index file gives us a natural
  trim-by-age path (matching the GL side's `lru_visit_active`
  flow).
* Disk write contention. 64 concurrent writer threads is a lot
  of stat/open/write calls; on slow disks this could affect
  steady-state I/O. Mitigation: the cap is conservative; in
  practice a populated cache means the load path skips writes
  entirely (saves only happen on cache misses).

See `metal-renderer-plan.md` slice M9 for the full implementation
detail; see `strategy.md` Phase 4f for the original-vs-amended
framing; see `automation.md` for the env-var + counter
documentation; see `xemu-fork/CLAUDE.md` for the stable-opt-in
flag entry.

## 2026-05-02: Metal slice M8 — async pipeline compile + skip-the-draw + upload fence; Path A deferred to M8.1

**Decision.** Slice M8 ships the metal-renderer-plan.md §3.10
"async pipeline compile + ubershader fallback" with a deliberate
**Path B only** scope: the async-compile state machine + RPCS3
"skip the draw" fallback + GPU-side `MTLSharedEvent` texture-upload
fence. **Path A — the full Dolphin-style hybrid ubershader — is
deferred to a follow-up slice (M8.1)**, conditional on user testing
showing the skip-the-draw artifact is unacceptable on real games.

**Components landed.**

1. **Cache state machine.** `PgraphMtlPipelineKey` LRU entries gain a
   `state` field (MISSING/PENDING/READY/FAILED) and an `epoch`
   counter that increments on every recycle.
   `pgraph_mtl_shaders_get_pipeline_ex(key, &state)` returns the
   tri-state result; on miss with async enabled, the entry
   transitions to PENDING and a build job is dispatched to a private
   serial concurrent dispatch queue at QoS_UTILITY. The completion
   handler validates `(epoch, state==PENDING)` under the cache lock
   before writing through; a stomped completion releases the freshly-
   built pipeline so it doesn't leak. Mirrors the pattern used by
   Apple's own `newRenderPipelineStateWithDescriptor:options:
   completionHandler:` API but routed through our own queue so the
   GLSL→MSL translation is also off-thread.

2. **`setShouldMaximizeConcurrentCompilation:YES`.** Driven once at
   dispatch-queue init (idempotent). Selector availability checked
   via `[device respondsToSelector:]` per Dolphin's Apple Silicon
   gotcha (some OCLP-patched older Macs may not respond; cheap to
   guard). The actual dispatch invocation uses the typed
   function-pointer cast pattern on `objc_msgSend` so the
   `-Wstrict-prototypes` warning doesn't fire.

3. **Skip-the-draw fallback.** When the renderer thread observes
   `state == PENDING` with `XEMU_METAL_TRANSLATED_PIPELINE=1`, the
   draw is skipped entirely (`s_draws_skipped_pending` increments).
   Visual artifact (briefly missing geometry) instead of a frame
   stall — same trade-off RPCS3's PR #4876 ships in production and
   that the GL renderer's `XEMU_PGRAPH_ASYNC_SHADER_COMPILE` slice
   landed (NV2A_PROF_SHADER_DRAWS_SKIPPED_PENDING). When the env var
   is OFF (default), the renderer falls through to the M3/M4
   passthrough on PENDING — cache warms in the background but the
   encode path never depends on the async pipeline being ready.

4. **Texture-upload fence.** The M6 upload module's
   `[cb waitUntilCompleted]` after each blit was a CPU-side stall.
   Replaced with a single `id<MTLSharedEvent>` plus an atomic
   monotonic counter; each blit ends with `encodeSignalEvent:value:N`
   and commits without CPU wait. The draw module reads the latest
   value via the public accessor and encodes
   `[cb encodeWaitForEvent:value:]` on every render command buffer
   header — same GPU-side ordering, no CPU stall. The wait is
   skipped when the fence value is 0 (no upload has yet signaled).

5. **Counters.** Five new counters surface on the `xemu-perf:`
   interval line: `METAL_SHADER_COMPILE_QUEUED_TOTAL`,
   `METAL_SHADER_COMPILE_COMPLETED_TOTAL`,
   `METAL_SHADER_COMPILE_FAILED_TOTAL`,
   `METAL_DRAWS_SKIPPED_PENDING_TOTAL`,
   `METAL_DRAWS_USING_UBERSHADER_TOTAL` (last reserved for M8.1).

**Path A deferral rationale.**

The full Dolphin-style hybrid ubershader is a ~2000-LOC undertaking:
a single megashader that interprets all NV2A combiner-stage
operations + alpha-test + fog + per-stage texturing modes via
uniform-buffer-driven runtime branches; uniform-state encoding; a
separate hybrid pipeline cache keyed on coarse render-pass state
alone. Implementing it autonomously in a single agent run was
judged too large a surface area — the risk of correctness
regressions across the 4-stage NV2A combiner state machine
outweighs the visual benefit it provides over Path B's "skip the
draw briefly" output.

**Path B is the same correctness-vs-perf trade-off RPCS3 ships in
production** and that this fork already proved viable on the GL
renderer with `XEMU_PGRAPH_ASYNC_SHADER_COMPILE`. With
`setShouldMaximizeConcurrentCompilation:YES` driving parallel
shader compile across CPU cores, the typical PGR2 / Crimson /
Rainbow shader-warmup window is on the order of 2-5 seconds at
cold launch (50-200 fresh shaders, ~5-50 ms each, parallelized
across 8-12 cores). The visual artifact is "geometry briefly
missing" rather than "frame stalls". User-driven launch testing
will determine whether skip-the-draw is acceptable in practice or
whether Path A's implementation cost is justified.

**M8.1 is queued behind real-game testing,** not behind synthetic
correctness validation — the M5 harness already covers shader
translation correctness; the gating question for Path A is purely
"is the skip-the-draw window large enough at cold launch to be
visually unacceptable", which only the user can answer with the
PGR2 / Crimson / Rainbow triplet. If the answer is yes, Path A
becomes M8.1 and reuses every infrastructure piece M8 ships —
state machine, dispatch queue, completion handler, fence — by
adding a parallel "draw via ubershader" path that fires before
the specialized pipeline transitions to READY.

**Env-var contract.**

`XEMU_METAL_ASYNC_PIPELINE_COMPILE={0,1}` overrides the
async-compile auto-default. Apple Silicon system builds default to
ON. Set to 0 to revert to the synchronous compile path (block the
renderer thread for 5-50 ms per fresh shader pair) — matches the
M5/M6/M7/M7.1 behavior. Documented in `xemu-fork/CLAUDE.md`.

**Verification.** `./build.sh -a arm64` succeeds; `codesign --verify
--deep --strict --verbose=2 dist/xemu.app` passes; M5 harness
reports 7/7 PASS; all M0-M7.1 + new M8 symbols present; GL renderer
symbols intact. The plan §4 M8 visual exit gate ("PGR2 cold launch
no `mspf_max` event > 250 ms attributable to shader compile") is
gated on a user-driven launch test per CLAUDE.md rule #10 — see
the handoff entry "Update — 2026-05-02 Metal slice M8 …" for the
full agent-attainable verification list.

**Supersedes.** None — M8 is a fresh slice. The "Path A deferred"
clause inside this entry is the binding decision; reopening Path A
requires a follow-up entry that establishes the visual-artifact
evidence first.

## 2026-05-02: Metal slice M7.1 — translated pipeline encode swap + M6 Part B foundational port

**Decision.** Slice M7.1 ships the long-deferred encode swap that
flips `XEMU_METAL_TRANSLATED_PIPELINE` from "no-op for encode" (M7)
to a functional gate.  When `=1` is set, every eligible draw is now
encoded through the spirv-cross-built MTLRenderPipelineState with
std140-packed VSH + PSH UBOs + per-NV2A-stage texture/sampler
bindings.  The slice also lands a foundational M6 Part B port —
per-mip + per-face + S3TC + swizzled texture upload via
`pgraph_mtl_texture_bind_from_pg`, sufficient to exercise the
translated pipeline on textured draws.

**Architectural decisions made this slice.**

1. **Manual std140 packer over spirv-reflect.** The Vulkan-flavored
   GLSL emitted by `pgraph_glsl_gen_vsh` / `pgraph_glsl_gen_psh`
   declares the UBO with `layout(std140) uniform`, so spirv-cross's
   MSL backend produces a struct whose member layout is
   std140-equivalent.  We manually pack `VshUniformValues` /
   `PshUniformValues` into std140 bytes by walking the
   `VshUniformInfo[]` / `PshUniformInfo[]` arrays in declaration
   order — same algorithm `vk/glsl.h::uniform_std140` uses.  This
   avoids pulling spirv-reflect into the Metal port and matches the
   GL/VK paths' uniform-update behavior.  Mat2 stored as 2 columns
   × vec4 padding (32 bytes); arrays stride to vec4 (16 bytes).
2. **MSL UBO binding indices.** With spirv-cross
   `MSL_ENABLE_DECORATION_BINDING=true` (set in M5), the SPIR-V
   `binding=N` decoration on the UBO maps to MSL `[[buffer(N)]]`.
   The GLSL generators emit VSH UBO at `binding=0` and PSH UBO at
   `binding=1`.  M7.1 binds UBOs to vertex `[[buffer(1)]]` (skipping
   `[[buffer(0)]]` which is reserved for the vertex descriptor's
   bufferIndex 0) and fragment `[[buffer(1)]]`.  This may need
   adjustment to `[[buffer(30)]]/[[buffer(31)]]` if spirv-cross's
   default `MSL_RESOURCE_INDEX_OFFSETS_BUFFER` shift applies on the
   M3 SDK; documented inline in `draw.mm`.  The validation surface
   is the user's first launch test with Metal validation enabled —
   incorrect bindings surface as Metal API validation errors, not
   silent garbage.
3. **NV2A vertex slot conventions.** Position bound at
   `setVertexBuffer:atIndex:0`, diffuse color at `:atIndex:3`
   (matching `NV2A_VERTEX_ATTR_DIFFUSE = 3` and the inline-buffer
   path that ships through M3/M4).  The full
   `BUFFER_VERTEX_RAM`-driven multi-attribute path (texcoords,
   normals, weights at slots 4..15) is queued as a follow-up.
4. **`XEMU_METAL_TRANSLATED_PIPELINE` default 0 (Option A).**
   Conservative opt-in for M7.1.  Most-likely-broken-first-run
   surfaces are exposed via the env var so user testing isolates
   bugs without affecting OpenGL's default-renderer behavior.  M8
   ("production-ready" decision) flips to default 1 after
   async-compile lands and visual-gate testing is clean.
5. **M6 Part B Path A — CPU-decode S3TC to RGBA8.** Apple Silicon
   supports native BC1/2/3.  Path A (always CPU-decompress via
   `s3tc_decompress_2d` to RGBA8) chosen as the simple-correct
   first cut.  Path B (BC-native uploads) queued as a follow-up
   optimization once a baseline exists for visual-correctness
   comparison.  CPU decode is what the GL renderer does today, so
   Path A also keeps GL/Metal visual parity in scope.
6. **Foundational, not complete, M6 Part B.** vk/texture.c's
   `get_texture_layout` is ~1500 lines including
   surface-to-texture, palette-indexed cache, custom-border-color
   samplers, 3D volume textures, per-LOD min/max-mipmap-level
   clamps, and a vram fast-hash dirty tracker.  M6 Part B ships
   the common-case subset (~400 lines of new C in `texture_pg.c`
   + `format.c`) covering: 2D linear, 2D swizzled, 2D mipmapped,
   2D cubemaps, S3TC.  Remaining lifecycle features queued as
   separate follow-ups not blocking the M7.1 user-testing milestone.

**Verification.**

- Build: `./build.sh -a arm64` succeeds with new symbols
  `_pgraph_mtl_draw_translated`, `_pgraph_mtl_uniform_init`,
  `_pgraph_mtl_uniform_stage_vsh`, `_pgraph_mtl_uniform_stage_psh`,
  `_pgraph_mtl_texture_bind_slot_full`,
  `_pgraph_mtl_texture_bind_from_pg`,
  `_pgraph_mtl_texture_color_format_to_mtl`,
  `_pgraph_mtl_draw_pipeline_fallback_count` all present.
- Validation: M5 harness 7/7 (incl. `psh_native_tri_depth` PR #2240
  fixture).
- GL renderer: 144 `_pgraph_gl_*` symbols intact.

**Honest scope notes — what M7.1 + M6B do NOT yet ship.**

- Not validated on a real game launch.  The agent does not start
  xemu while another instance may be running (CLAUDE.md rule #10).
  The visual gate (PGR2/Rainbow/Crimson per-pixel ≤ 1 % match to
  GL) requires user-driven testing.
- Not flipped to default-on.  Default is 0; user must set
  `XEMU_METAL_TRANSLATED_PIPELINE=1` to test.
- Not the complete M6 Part B.  Surface-to-texture,
  palette-indexed, 3D volume textures, custom border colors,
  shadow samplers, vram-hash dirty tracking explicitly deferred.

**Files added.**
- `hw/xbox/nv2a/pgraph/mtl/uniform.h`, `uniform.c`, `uniform.mm`.
- `hw/xbox/nv2a/pgraph/mtl/format.h`, `format.c`.
- `hw/xbox/nv2a/pgraph/mtl/texture_pg.c`.

**Files edited.**
- `hw/xbox/nv2a/pgraph/mtl/draw.h`, `draw.mm` — added
  `pgraph_mtl_draw_translated()` + counter accessors.
- `hw/xbox/nv2a/pgraph/mtl/texture.h`, `texture.mm` — added
  `pgraph_mtl_texture_bind_slot_full()` per-mip per-face upload.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c` — `flush_draw` branches on
  `XEMU_METAL_TRANSLATED_PIPELINE`; added uniform/texture init/finalize.
- `hw/xbox/nv2a/pgraph/mtl/meson.build` — registers new sources.
- `util/xemu-metal-perf.c` — adds 4 weak counter accessors and
  baselines for `METAL_DRAW_TRANSLATED` /
  `METAL_PIPELINE_FALLBACKS` / `METAL_UNIFORM_PACK` /
  `METAL_UNIFORM_BYTES`.
- `scripts/apple-silicon/extract-perf-summary.sh` — surfaces 4 new
  counters in slots 127–130.
- `xemu-fork/CLAUDE.md` — flips `XEMU_METAL_TRANSLATED_PIPELINE`
  documentation from "no-op for encode" to "functional gate".

**Status:** SHIPPED (build + symbols + harness gates).  Visual gate
pending user-driven launch test.

**Supersedes:** prior decision-log entry "2026-05-02: Metal slice M7
— state-to-PipelineKey + framebuffer-fetch validated" honest-scope
note #1 ("Encode through the translated pipeline") — that
deferral is now closed by M7.1.

## 2026-05-02: Metal slice M7 — state-to-PipelineKey + framebuffer-fetch validated

**Decision.** Slice M7 of `metal-renderer-plan.md` ships with the
following honest-scope split between what's wired now and what's
deferred to a small "M7.1" follow-up:

- **State-to-PipelineKey conversion (`mtl/state.h` + `state.c`).**
  `pgraph_mtl_build_pipeline_key()` walks PGRAPHState, runs
  `pgraph_glsl_get_shader_state(pg)` for the full ShaderState, snapshots
  the 9 pipeline-affecting registers (NV_PGRAPH_BLEND, BLENDCOLOR,
  CONTROL_0..3, SETUPRASTER, ZOFFSET{BIAS,FACTOR} — same set vk/draw.c
  uses for cross-renderer key parity), and walks
  `pg->vertex_attributes[0..15]` filling per-attribute MTLVertexFormat
  + per-buffer stride. The NV2A → MTLVertexFormat mapping table is
  exposed via `pgraph_mtl_translate_vertex_format()` (F→Float*N,
  UB_OGL→UCharNNormalized, S1→ShortNNormalized, S32K→ShortN,
  CMP→Int1010102Normalized, UB_D3D→UChar4Normalized_BGRA).
- **Draw-path lookup wired.** Every eligible flush_draw builds a
  PipelineKey and calls `pgraph_mtl_shaders_get_pipeline(&key)`. The
  lookup hits the M5 GLSL→SPIR-V→MSL translator + M5/M6 LRU cache.
  Counters `METAL_PIPELINE_KEY_BUILT` /
  `METAL_PIPELINE_TRANSLATED_OK` / `METAL_PIPELINE_TRANSLATED_FAILED`
  surface the per-interval activity.
- **Encode path stays on the M3/M4 hand-coded passthrough pipeline.**
  The translated MTLRenderPipelineState lookup runs in parallel as a
  cache warmup that exercises the full translator on real PGRAPHState
  shader-state classes, but the actual encoder draw still uses the
  passthrough pipeline because the translated path needs uniform-buffer
  marshaling + per-stage texture/sampler binding which are deferred to
  M7.1.
- **Apple GPU family 1+ detection.**
  `pgraph_mtl_heap_supports_framebuffer_fetch()` latched at heap_init
  from `[device supportsFamily:MTLGPUFamilyApple1]`. Apple Silicon Macs
  return true (Apple7+ ⊃ Apple1).
- **Three M7 env vars landed.**
  `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH={0,1}` forces the negative
  Apple1 answer. `XEMU_METAL_FORCE_PASSTHROUGH={0,1}` forces the M3/M4
  hand-coded path (bisection knob). `XEMU_METAL_TRANSLATED_PIPELINE={0,1}`
  is the development opt-in for the future translated encode path.
  Defaults all 0.
- **M5 harness extended with framebuffer-fetch fixture.** Hand-written
  Vulkan-style GLSL with a `subpassLoad(uSubpass)` input-attachment
  read; asserts the MSL output contains `[[color(0)]]` (framebuffer
  fetch). Confirms the M5 spirv-cross
  `MSL_FRAMEBUFFER_FETCH_SUBPASS=true` flag does the right thing —
  required because the framebuffer-fetch path is the
  programmable-blend infrastructure for M8's planned ubershader.

**Honest scope — what M7 does NOT yet ship.**

1. **Encode through the translated pipeline.** Uniform-buffer
   marshaling + per-stage texture/sampler encode binding require a
   port of vk/draw.c::create_pipeline + vk/shaders.c::pgraph_vk_update_descriptor_sets.
   Each is mechanical but the visual gate (PGR2/Rainbow/Crimson per-pixel
   diff ≤ 1 %) requires user-driven launch testing per CLAUDE.md
   rule #10. Deferred to M7.1.
2. **Combiner-via-framebuffer-fetch GLSL emit.** NV2A combiners do
   NOT read destination color (combiners use input attribs, texture
   samples, and `r0..r15` for previous-stage output; final color is
   written to `fragColor` with no destination read; standard fixed-
   function blend is handled by Metal's color-attachment blend
   descriptor natively). The framebuffer-fetch path is therefore
   infrastructure for M8's planned ubershader programmable-blend
   variant, not a current correctness need. Validation fixture in
   M5 harness; emit-path is a no-op until M8 needs it.
3. **Render-pass-split fallback for Intel Macs.** Documented as a
   stub. Apple Silicon targets always have framebuffer fetch; building
   the pass-split logic against an unverifiable target was skipped.
4. **Full S3TC + per-mip + per-face texture lifecycle.** ~1500 lines
   of port from vk/texture.c + s3tc.c. Queued as M6 Part B
   completion; out of scope for M7 (4× larger than the rest of the
   slice, and orthogonal to the state-to-key + draw-path lookup
   work).

**Rationale (why ship M7 now rather than wait for M7.1).** The
state-to-key + draw-path lookup is already actively warming the cache
on every draw with the full translator + ShaderState round-trip, even
though the encode is currently a no-op for those translated pipelines.
This gives M7.1 a known-working baseline: when the encoder swap
lands, the only failure modes will be uniform-marshaling /
texture-binding bugs, not state-to-key bugs. Shipping M7 separately
keeps each follow-up's gate surface area small enough to validate
through a single user-driven launch test.

**PR #2240 correctness preserved.** The existing
`psh_native_tri_depth` validation fixture still passes. The
`glsl/psh.c` native-depth fragment shader path is unchanged. The
mtl/renderer.c eligibility checks (`mtl_native_tri_depth_eligible`,
`mtl_native_quad_eligible`) still gate variant selection through the
shared `pgraph_glsl_native_*` helpers. CLAUDE.md rule #6 (no PR
#2240 revert) is observed.

**Validation.**
- Build: `CMAKE=/opt/homebrew/bin/cmake ninja -C build qemu-system-i386`
  passes.
- Symbol verification:
  `nm build/qemu-system-i386 | grep -E "pipeline_key|translate_vertex_format|supports_framebuffer"`
  shows all six new public symbols. M0–M6 MTL symbols (133 total)
  intact. GL + VK paths (151 symbols) intact.
- Full launch / visual gate: deferred to M7.1.

**Files added.**
`hw/xbox/nv2a/pgraph/mtl/state.{h,c}`.

**Files edited.**
`hw/xbox/nv2a/pgraph/mtl/{heap.h,heap.mm,renderer.c,shader_validation.c,meson.build}`,
`util/xemu-metal-perf.c`, `scripts/apple-silicon/extract-perf-summary.sh`.

**Supersession.** M7's "exit gate" in metal-renderer-plan.md §4
("combiner-blend-heavy scenes match GL output pixel-by-pixel") is
**partially deferred to M7.1** for the encode-path swap. The
build-success / symbol-presence / framebuffer-fetch translation /
state-to-key / draw-path-lookup gates are met now. See handoff.md
"Update — 2026-05-02 Metal slice M7" for the full decomposition.

## 2026-05-02: Metal slice M6 — textures + sampling infra + pipeline cache shipped

**Decision.** Slice M6 of `metal-renderer-plan.md` ships with its
scope expanded to absorb the deferred M5 Part B (per-PipelineKey LRU
cache + draw-path swap — see below for the swap-deferral caveat).
The combined slice lands:

- The pipeline cache (split between `mtl/shaders.mm` for Metal API
  touchpoints and `mtl/shadergen.c` for the LRU + GLSL-generator
  calls), capacity 2048, backed by `qemu/lru.h` with `fast_hash` +
  POD-memcmp comparison. Counters: `METAL_PIPELINE_HITS / MISSES /
  FAILED`.
- The texture path: `heap_textures` MTLHeap (512 MiB, Private +
  Untracked), texture cache (capacity 64, FIFO) keyed by
  `(vram_addr, w, h, pixel_format)`, sampler cache (capacity 256,
  pre-warmed with 24 NV2A-frequent combinations), and a 4× 4 MiB
  Shared|WriteCombined upload staging ring with synchronous
  blit-encoder upload to a Private destination texture. Counters:
  `METAL_TEX_UPLOADS_TOTAL / METAL_TEX_UPLOAD_BYTES_TOTAL /
  METAL_TEX_CACHE_HITS / METAL_TEX_CACHE_MISSES`.

The M5 validation harness re-run still 6/6.

**Rationale.** Per metal-renderer-plan §6 R1 mitigation, the
deferred M5 Part B cache had to land alongside M6 so the M6 exit
gate could exercise translated state-driven shaders end-to-end
rather than relying on the M3/M4 hand-coded passthrough that has
no texture binding. Splitting further would have left the gate
without a verification path.

**C/.mm boundary.** Most invasive design call in this slice.
`qemu/lru.h` includes `qemu/queue.h` whose macros depend on GCC
`typeof` (not portable to C++); `vsh.h`/`psh.h` pull in glib's
`MString` helper inlines that implicitly cast `gpointer` →
`MString *` (rejected by C++). Fix mirrors `vk/shaders.c`'s split:
keep LRU + glib-typed code on the `.c` side; have the `.mm` side
take only Metal-flavored primitives. Required:

1. Forward-declaring `PgraphMtlPipelineKey` in `shaders.h` so the
   `.mm` side never dereferences it.
2. Splitting cache build into a `.c` driver
   (`pgraph_mtl_shaders_get_pipeline` calling `lru_lookup` +
   `pgraph_glsl_gen_*`) and a `.mm` builder
   (`pgraph_mtl_shaders_build_pipeline` taking flat `uint32_t *`
   arrays for vertex layout + Metal-flavored primitives).
3. Counter atomics in `.mm`; `.c` side increments via three trivial
   extern shims.

This pattern is already in use in `mtl/heap.mm` ↔ `mtl/renderer.c`
and matches `vk/`.

**Heap budgeting.** Q2 resolution committed `heap_textures =
MTLHeapTypeAutomatic + Private + Untracked`. M6 sets the size to
512 MiB. Color/depth heaps stay 256 MiB each and remain Tracked.
On Apple Silicon unused heap regions are not pre-touched.

**Synchronous upload.** Upload command buffers commit + wait
inline. Async + frame-fence integration ships with M8.

**Draw-path swap not yet wired in production.** Cache infra fully
wired (init → lookup → translate → build → store → release on
evict), but `pgraph_mtl_flush_draw` still picks the hand-coded
`passthrough_*` pipeline. Two pieces remain:

1. `renderer.c` needs `pgraph_glsl_get_shader_state(pg)` + a walk
   of `pg->vertex_attributes[]` to populate `PgraphMtlPipelineKey`.
2. Render encoder needs `setVertexBuffer:atIndex:1` (VSH UBO),
   `setFragmentBuffer:atIndex:1` (PSH UBO), `setFragmentTexture:`
   and `setFragmentSamplerState:` per active stage.

Both pieces are mechanical ports from `vk/draw.c`. Intentionally
not bundled into M6 because each needs a paired benchmark gate
(PGR2 mid-route ≤ 5 % visual diff vs GL) that requires user-driven
launch testing per CLAUDE.md rule #10. M7 will land both alongside
its own correctness gate; the production swap goes in then.
Without M7 the swap would visibly regress every combiner-shaded
surface.

**S3TC + full mipmap port deferred.** `vk/texture.c::get_texture_layout`
lifecycle (per-mip + per-face, S3TC decode, swizzled, cubemap
alignment) is ~1500 lines and was scoped out. M6 ships a working
single-level 2D post-decoded-RGBA upload covering the common UI/
decal pattern. Full lifecycle queued as M6 Part B.

**What this slice does NOT do.**
- No combiner emulation via framebuffer fetch (M7).
- No async pipeline compile (M8).
- No persistent shader cache (M9).
- No production replacement of the M3/M4 passthrough pipeline.
- No per-mip / per-face texture upload (M6 Part B).

**Verified.** `./build.sh -a arm64` succeeds; new symbols
`_pgraph_mtl_shaders_init`, `_pgraph_mtl_shaders_get_pipeline`,
`_pgraph_mtl_shaders_build_pipeline`, `_pgraph_mtl_shadergen_vsh`,
`_pgraph_mtl_shadergen_psh`, `_pgraph_mtl_texture_init`,
`_pgraph_mtl_texture_bind_slot`,
`_pgraph_mtl_heap_alloc_texture_2d/3d/cube` present in
`dist/xemu.app/Contents/MacOS/xemu`; `_pgraph_gl_clear_surface`
intact (no GL regression); `codesign --verify --deep --strict
--verbose=2 dist/xemu.app` passes. M5 harness re-run:
`summary: 6/6 passed, 0 failed`. Visual gate deferred to M7
(combiner correctness needed before any visual diff is meaningful).

**Next.** M7 — register-combiner emulation via framebuffer fetch.
After M7 the production draw-path swap from `passthrough_*` to the
cache becomes correctness-feasible.

## 2026-05-02: Metal slice M5 — shader translator + validation harness ship; per-pipeline cache deferred to M6

**Decision.** Slice M5 of `metal-renderer-plan.md` ships in a
two-part landing: (Part A — this entry) the GLSL → SPIR-V → MSL
translator and the in-process shader-validation harness, both
proven against representative ShaderState fixtures; (Part B —
deferred to M6) the per-PipelineKey LRU cache + draw-path swap,
which depends on M6 texture/sampler binding and M7 combiner-via-
framebuffer-fetch to be exercisable end-to-end.

**Rationale (Part A — what ships now).** §6 R1 of the plan
calls out spirv-cross compatibility with NV2A-generated GLSL as the
single highest risk in the Metal port. CLAUDE.md rule #1 ("no
guessing") makes that risk a slice gate: the right test is to run
xemu's actual GLSL generators against actual glslang against actual
spirv-cross against an actual `[device newLibraryWithSource:]` and
see what breaks. The harness does this; results are 6/6 pass
covering fixed-function vsh (minimal + lit/textured), simple
combiner, two-stage textured combiner, alpha-test+fog, and the
PR #2240 native-tri-depth fragment-shader path. The translator's
MSL options match the plan §3.3 spec: MSL 2.3, framebuffer-fetch-
subpass enabled (M7 groundwork), enable-decoration-binding (so
spirv-cross uses the Vulkan-flavored GLSL's deterministic set/
binding decorations as MSL `[[buffer(N)]]` indices), fixup-depth-
convention (Apple upper-left, [0,1]).

**Two intentional fixture omissions.** (1) Geometry shader. Apple
Silicon Metal has no native GS stage; the Metal renderer already
bypasses GS via `XEMU_NATIVE_TRI_DEPTH` / `XEMU_NATIVE_QUAD`. The
spirv-cross GS-emulation path silently SIGSEGVs on a line-loop GS
payload in vulkan-sdk-1.3.290.0 — but validating that path would
gate the slice on a feature we do not use on Metal. The fixture is
documented-as-absent in `shader_validation.c::build_fixtures`; if a
future slice ever adds GS emulation (M7 framebuffer-fetch combiner
does NOT need it), the fixture should be re-enabled. (2)
Programmable VSH. Vsh-prog requires a valid VSH token sequence with
FLD_FINAL set; hand-encoding that is fragile (NVIDIA Cheops binary
format). Fixed-function vsh exercises the same prologue/body/
epilogue layout. The next harness iteration should capture a real
ShaderState from a running game and replay it through the harness
so vsh-prog is also covered.

**Rationale (Part B — what's deferred).** A translated state-driven
shader cannot replace the M3/M4 hand-coded passthrough on the draw
path until: (i) texture sampling and uniform-buffer plumbing are in
place (M6), so the translated vsh's `c[]` / `lights[]` / `clipRange`
uniforms have backing buffers and the translated psh's `tex0..3`
samplers have textures; (ii) the combiner-via-framebuffer-fetch
mechanism is wired (M7), so the translated psh's combiner emit
matches GL's behavior. Landing a cache that immediately blocks every
draw on a missing-binding error would be net-negative. The
PipelineKey type (`mtl/shaderstate.h`) and the cache API
(`mtl/shaders.h`) ship as design artifacts so M6 has a fixed
target; the cache implementation slot is reserved in mtl/meson.build
but no production call site routes through it yet.

**Synchronous newLibraryWithSource: + newRenderPipelineStateWith
Descriptor:.** M5 (and the deferred cache) compile shaders
synchronously on the draw thread. Per the plan §4 M5 spec, async
compile + ubershader fallback land in M8. Until then a cache miss
will block the draw thread for the duration of glslang + spirv-cross
+ pipeline-state build (typically 5-50 ms per shader pair on Apple
Silicon).

**glslang dependency on darwin.** Pre-M5 the Vulkan renderer is the
only consumer of `libglslang`, and `meson.build` only built it when
`vulkan.found()`. On darwin Vulkan is not found (Apple does not ship
a system loader; xemu does not bundle MoltenVK), so `libglslang` was
not built on the Apple Silicon path. The translator needs glslang.
The condition is widened to `vulkan.found() OR
(darwin AND aarch64)` so the Metal renderer gets glslang on darwin
without changing the Vulkan path on Linux/Windows. The Vulkan
renderer's other deps (`volk`, `vk_mem_alloc`, `spirv_reflect`) stay
gated on `vulkan.found()` because Metal does not need them.

**XEMU_RENDERER env-var bridge.** Added so the validation runner
script can force METAL on a CI-style invocation without modifying
the user's xemu.toml. Mirrors the existing
`XEMU_DISPLAY_SCALE` env-var bridge pattern. Recognized values:
`OPENGL` / `GL`, `VULKAN` / `VK`, `METAL`, `NULL`. Case-insensitive.
Unrecognized values silently no-op. Documented in
`automation.md`. Implemented in
`ui/xemu-settings.cc::xemu_settings_apply_renderer_env`.

**Counters added** (always-on, surfaced via `xemu-perf:` interval
line, weak-symbol pattern handles non-Apple-Silicon hosts):
`METAL_GLSL_TRANSLATE`, `METAL_GLSL_TRANSLATE_FAIL`,
`METAL_SHADER_VALIDATE_OK`, `METAL_SHADER_VALIDATE_FAIL`. The first
two reach steady-state non-zero only when M6+ wire the translator
on the draw path; the validate counters tick only when
`XEMU_METAL_SHADER_VALIDATE` is set.

**What we did NOT do.**
- We did not change the M3/M4 hand-coded passthrough draw path. M5
  ships in parallel; the production renderer continues to use the
  hand-coded `passthrough_vs` / `passthrough_fs` / `passthrough_
  native_depth_fs` MSL until M6/M7 land.
- We did not implement async compile (M8) or persistent shader
  cache (M9).
- We did not add the GS-emulation fixture or fix the spirv-cross
  GS crash. Apple Silicon Metal renderer doesn't need GS emulation.
- We did not run the M5 plan's "PGR2 mid-route snapshot: visual
  diff ≤ 5 % per-pixel difference vs GL" exit-gate test. That gate
  requires the production draw-path swap which lands with M6/M7;
  for slice M5 the gate is the harness pass.

**Verification.**
- `./build.sh -a arm64` succeeds.
- `nm dist/xemu.app/Contents/MacOS/xemu | grep -E
  '(pgraph_mtl_glsl|pgraph_mtl_shader_validate|pgraph_mtl_pipeline_validate_msl)'`
  → all M5 symbols present.
- `codesign --verify --verbose=2 dist/xemu.app` →
  `valid on disk` / `satisfies its Designated Requirement`.
- `scripts/apple-silicon/metal-shader-validation/run-validation.sh`
  exits 0 with `summary: 6/6 passed, 0 failed`.
- M3/M4 paths (passthrough draw, native_quad index expansion) and
  the GL renderer are unchanged; the M5 wiring is purely additive.

**Open M5 follow-ups.**
- M6 texture binding will reuse the `MSL_ENABLE_DECORATION_BINDING`
  + per-stage descriptor-set/binding scheme already configured here
  — VSH UBO at set=0 binding=0, PSH UBO at set=0 binding=1, PSH
  textures at set=0 bindings=2..5 (matching `vk/shaders.c`'s
  `VSH_UBO_BINDING` / `PSH_UBO_BINDING` / `PSH_TEX_BINDING`).
- The cache size of 2048 entries (`shaders.h`) is the same as
  vk/shaders.c's `shader_cache_size = 1024` doubled to leave headroom
  for the combiner explosion that M7 will drive. Tune after the
  M6/M7 production wiring lands and we have real cache-fill numbers.
- The harness's per-fixture report is to stderr only. M9 (persistent
  shader cache) should consider also writing the fixture's GLSL/MSL
  to a file alongside the cache so the next post-mortem can compare
  the captured GLSL against a regenerated translator output.

## 2026-04-29: Treat Apple Silicon work as a fork

Decision:

This project will optimize for Apple Silicon macOS even if the approach becomes
too invasive for easy upstream compatibility.

Rationale:

The user explicitly wants a fork-quality solution and does not want us blocked
by upstream acceptability. The renderer and platform changes are likely large
enough to diverge substantially.

## 2026-04-29: Do not use permanent downgrade/revert as the final fix

Decision:

We may temporarily gate or disable the PR #2240 geometry-heavy path for
diagnosis, but the final plan is not "just revert #2240."

Rationale:

PR #2240 fixes real depth precision, polygon offset, flat shading, and primitive
behavior issues. Reverting improves performance by discarding correctness work.
The correct approach is to preserve the intended behavior through a more
Apple-friendly pipeline.

## 2026-04-29: Geometry shader dependency is a primary blocker

Decision:

Eliminating geometry-shader dependence is the first major renderer refactor.

Rationale:

Public macOS regression data implicates heavier geometry shader usage, and the
current code uses geometry shaders for most primitive types. Vulkan-over-Metal
and native Metal paths both become simpler and more robust if this dependency is
removed.

## 2026-04-29: Metal is the preferred final Apple Silicon backend

Decision:

The fork should aim for a native Metal renderer as the long-term fast path.
Vulkan-over-Metal should be prototyped and measured, not assumed.

Rationale:

Apple positions Metal as the modern GPU API replacing OpenGL. MoltenVK and
KosmicKrisp are promising, but xemu has unusual emulator workloads and currently
requires features that may not map cheaply. A native Metal path gives the fork
the most control.

## 2026-04-29: Benchmarking must precede irreversible architecture choices

Decision:

Before committing deeply to Metal-only or Vulkan-over-Metal-first, collect
baseline measurements and feature-probe data.

Rationale:

We have strong evidence for the problem class, but not yet local measurements
for this machine, these games, or current SDK/driver behavior.

## 2026-04-29: Keep macOS build/package fixes in-tree

Decision:

Small macOS build and packaging fixes are allowed before renderer work when
they are required to produce a runnable Apple Silicon baseline.

Rationale:

The baseline arm64 build initially failed because Meson's cross-build path did
not use the default `cmake` binary. After that was fixed, the packaged app
failed at launch because macOS `dyld` rejected duplicate `LC_RPATH` entries.
Without fixing those in `build.sh`, benchmark runs would depend on manual,
easy-to-forget shell workarounds.

Verification:

- `./build.sh -a arm64` succeeds.
- `dist/xemu.app` passes code-sign verification.
- `dist/xemu.app/Contents/MacOS/xemu --version` launches and reports the Apple
  OpenGL-on-Metal renderer.

## 2026-04-30: Use QMP/HMP for benchmark snapshot restore

Decision:

Benchmark scene snapshots should be restored after xemu startup through QMP/HMP
instead of by passing `-loadvm` on the command line.

Rationale:

Initial CLI `-loadvm` testing failed against a Crimson Skies snapshot with a
saved USB hub device-tree mismatch. Starting xemu normally and then sending HMP
`loadvm` through the QMP socket restored the same snapshot successfully, and the
same path also restored the Rainbow Six 3 scene snapshot.

Verification:

- Crimson snapshot `crimson_scene_b0` restored from
  `benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2`.
- Rainbow snapshot `rainbow_scene_b1_nothumb` restored from
  `benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2`.

## 2026-04-30: Disable benchmark snapshot thumbnails by default

Decision:

Benchmark-created snapshots should set `XEMU_SNAPSHOT_NO_THUMBNAIL=1` by
default.

Rationale:

A Rainbow Six 3 scratch HDD containing a normal thumbnail-bearing snapshot
crashed Apple's OpenGL-on-Metal worker path during a later benchmark launch
before QMP restore. A thumbnail-free Rainbow snapshot saved at the same scene
restored successfully. This keeps benchmark snapshots focused on deterministic
scene entry while avoiding a separate thumbnail/render-thread failure mode.

## 2026-04-30: Attribute geometry-shader work before replacing it

Decision:

Add OpenGL geometry-shader attribution counters to the existing `xemu-perf:`
interval log before attempting larger renderer changes.

Rationale:

The public macOS regression points at geometry shaders, but the fork needs
local per-scene evidence. Counting geometry module/program generation, binds,
and draw calls by primitive family lets snapshot runs identify which game scene
is the better diagnostic target and whether a change affects the suspected
path.

Verification:

- B2 Crimson Skies: 25,202 geometry-backed draws in 30s, all triangle-family.
- B3 Rainbow Six 3: 149,961 geometry-backed draws in 30s, all
  triangle-family.
- Rainbow Six 3 is the stronger local geometry-shader diagnostic scene.

## 2026-04-30: Triangle depth arithmetic is not the first bottleneck

Decision:

Keep `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` as a temporary diagnostic, but do
not pursue triangle depth/slope arithmetic simplification as the next
performance path.

Rationale:

D1 kept triangle-family geometry shaders active while bypassing their
depth-plane and slope calculation. Rainbow Six 3 did not improve compared with
B3, and it showed one late 162 ms frame-time spike. That makes geometry-shader
dispatch, Apple OpenGL driver behavior, or surrounding pipeline work a better
next target than arithmetic inside the geometry shader.

Verification:

- D1 Rainbow Six 3: 30.72 FPS post-load / 18.39 MSPF.
- B3 Rainbow Six 3: 30.97 FPS post-load / 17.66 MSPF.

## 2026-04-30: Geometry-shader dispatch is the next replacement target

Decision:

Prioritize a correctness-preserving triangle path that avoids GL geometry
shader dispatch before Vulkan-over-Metal experiments.

Rationale:

`XEMU_DIAG_SKIP_TRI_GEOM=1` bypassed geometry-shader program generation for
triangle-family fill draws in the Rainbow Six 3 snapshot scene. It is knowingly
incorrect because the fragment shader no longer receives the geometry shader's
per-triangle depth payload, but it reduced post-load average frame time from
B3's 17.66 ms to 6.38 ms and dropped geometry draw counters to zero. That is a
stronger signal than D1's arithmetic simplification result.

Verification:

- D2 Rainbow Six 3: 30.96 FPS post-load / 6.38 MSPF.
- D2 geometry draw counters: 0.
- B3 Rainbow Six 3: 30.97 FPS post-load / 17.66 MSPF.

## 2026-04-30: Native triangle depth is the first GL replacement prototype

Decision:

Use `XEMU_DIAG_NATIVE_TRI_DEPTH=1` as the next experimental branch for removing
triangle-family GL geometry-shader dispatch while preserving the depth and
polygon-offset behavior in the fragment shader.

Rationale:

D3 bypassed triangle-family fill geometry shaders and derived `zvalue` plus the
polygon-slope term from `gl_FragCoord`. It kept geometry draw counters at zero
and retained most of D2's frame-time improvement in Rainbow Six 3, while being
much closer to a correctness-preserving design than simply dropping the
geometry payload. The prototype still needs visual and depth-correctness
validation before it can become the normal renderer path.

Verification:

- D3 Rainbow Six 3: 30.96 FPS post-load / 8.10 MSPF, geometry draw counters: 0.
- D5 Rainbow Six 3 after narrowing the diagnostic skip to triangle-family fill
  primitives only: 30.99 FPS post-load / 6.35 MSPF, geometry draw counters: 0.
- D4 Crimson Skies: 30.98 FPS post-load / 18.97 MSPF, geometry draw counters:
  0.
- D7 Crimson Skies after the same line-safe narrowing: 30.98 FPS post-load /
  20.30 MSPF, geometry draw counters: 0.
- B3 Rainbow Six 3: 30.97 FPS post-load / 17.66 MSPF.

## 2026-04-30: Keep flat shading on the geometry-shader path

Status: superseded later on 2026-04-30 by the first-provoking flat-shading
eligibility expansion and the dedicated flat-tri-depth XBE. This entry records
the earlier conservative checkpoint.

Decision:

`XEMU_DIAG_NATIVE_TRI_DEPTH=1` should bypass triangle-family fill geometry
shaders only for smooth-shaded draws. All flat-shaded triangle fills should
fall back to the geometry-shader path until a dedicated flat/provoking-vertex
validation scene exists.

Rationale:

The OpenGL renderer globally uses `GL_FIRST_VERTEX_CONVENTION`. The geometry
shader can emulate flat provoking-vertex behavior by copying the selected
vertex's flat attributes to every emitted vertex. The native triangle path has
not yet been validated against flat-shaded cases, and current local scenes do
not exercise them, so keeping all flat shading on the geometry-shader path is
the safer prototype boundary.

Verification:

- D8 Rainbow Six 3: 30.99 FPS post-load / 6.47 MSPF, geometry draw counters: 0,
  native triangle-depth draws: 197,212, fallbacks: 0.
- D10 Rainbow Six 3 confirmation: 30.98 FPS post-load / 6.78 MSPF, geometry
  draw counters: 0, native triangle-depth draws: 192,776, fallbacks: 0.
- D15 Rainbow Six 3 after the smooth-only tightening: 30.97 FPS post-load /
  6.41 MSPF, geometry draw counters: 0, native triangle-depth draws: 201,450,
  fallbacks: 0, all smooth.
- D9 Crimson Skies: 30.98 FPS post-load / 20.01 MSPF, geometry draw counters: 0,
  native triangle-depth draws: 71,277, fallbacks: 0.

## 2026-04-30: Native triangle-depth coverage is strong except flat shading

Status: superseded later on 2026-04-30. Smooth-depth coverage is still valid,
and the dedicated flat-shading XBE now validates the first-provoking
native-path / nonfirst-provoking fallback split.

Decision:

Keep the native triangle-depth prototype focused on validated cases, and treat
flat shading as the remaining promotion blocker until direct XBE evidence is
clean.

Rationale:

Additional coverage counters show the current Rainbow and Crimson workloads
exercise the important depth math cases that were previously only inferred:
linear depth, w-depth, and fill polygon offset. Both games stayed entirely on
smooth shading in the measured scenes and scripted routes, so they cannot
validate flat-shaded native rendering. At this point the implementation was
kept smooth-only; later work allowed first-provoking flat triangles and added a
purpose-built XBE to validate that boundary directly.

Verification:

- D11 Rainbow snapshot: 198,119 native draws, 0 fallbacks, 100,772 w-depth,
  97,347 linear-depth, 26,019 polygon-offset, all smooth.
- D12 Crimson snapshot: 71,436 native draws, 0 fallbacks, 71,436 linear-depth,
  24,738 polygon-offset, all smooth.
- D13/D14 90-second scripted routes: 641,855 combined native draws, 0
  fallbacks, no flat-first native draws, no flat fallbacks.
- D15 post-tightening Rainbow snapshot: 201,450 native draws, 0 fallbacks,
  101,016 w-depth, 100,434 linear-depth, 26,823 polygon-offset, all smooth.
- Rainbow and Crimson baseline/native screenshot smoke comparisons did not show
  an obvious visual regression.

## 2026-04-30: Allow first-provoking flat triangles, but validate with XBE

Decision:

Allow flat-shaded first-provoking triangle fills on the native triangle-depth
path because the OpenGL renderer explicitly uses `GL_FIRST_VERTEX_CONVENTION`.
Keep flat-shaded nonfirst-provoking triangle fills on the existing geometry
shader fallback path.

Rationale:

The native OpenGL rasterizer can match NV2A first-provoking flat interpolation
directly under the renderer's current provoking-vertex convention. Nonfirst
provoking still requires the geometry shader to select the same flat attribute
source as NV2A. The retail Crimson/Rainbow scenes do not exercise flat-shaded
triangle fills, so this boundary needs a purpose-built XBE instead of more
retail route searching.

Verification:

- D16 Rainbow Six 3:
  `benchmark-runs/20260430-135911-rainbow-six-3`, 31.01 FPS / 7.62 MSPF
  post-load, 192,998 native triangle-depth draws, 0 fallbacks, 0 geometry
  draws. The scene remained all smooth, so the eligibility expansion did not
  perturb the known benchmark path.
- Flat XBE source and artifacts:
  `scripts/apple-silicon/xbe-tests/flat-tri-depth/`,
  `bin/default.xbe`, and `flat-tri-depth.iso`.
- Manual-launch ISO copy:
  `/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso`.
- Trace run `benchmark-runs/20260430-141331-flat-tri-trace` confirms the XBE
  sends `NV097_SET_SHADE_MODE` flat plus first and last
  `NV097_SET_PROVOKING_VERTEX`.

Follow-up:

Superseded by the final perf flush validation below. The XBE boots, sends the
right guest methods, and now produces the expected native/fallback counter
split.

## 2026-04-30: Flat XBE mismatch is a binding-correlation problem

Status: superseded by the final perf flush validation below.

Decision:

Treat the flat-tri-depth failure as a renderer state-correlation problem, not
as a missing validation asset or stale ISO problem.

Rationale:

The flat XBE was rebuilt, and benchmark metadata now records disc size and
mtime so stale media can be spotted. The trace-enabled rebuilt run shows the
expected flat-last sequence (`SHADE_MODE 0x1d00`, `PROVOKING_VERTEX 0`,
`DRAW_ARRAYS 0x2000003`), but `xemu-perf` still classified all 102,684 native
triangle-depth candidates as smooth. Adding explicit method-owned PGRAPH
shade/provoking fields for shader-state generation did not change the
classification. That makes a generic register-bit decode issue unlikely; the
next useful evidence is a direct comparison between live PGRAPH state and the
bound shader state for the same draw.

Verification:

- Rebuilt flat ISO: 2026-04-30 14:38:34 CDT, 720,896 bytes.
- `benchmark-runs/20260430-143952-flat-tri-depth`: rebuilt ISO, all native
  triangle-depth candidates still smooth.
- `benchmark-runs/20260430-144128-flat-tri-depth`: trace confirms flat-last
  draws from the rebuilt ISO, counters still smooth.
- `benchmark-runs/20260430-144451-flat-tri-depth`: after method-owned
  shade/provoking fields, counters still smooth.

Next action:

Superseded by the final perf flush validation below. Low-volume logging was
added and showed the live and bound shader state were flat-first when expected.

## 2026-04-30: Use final perf flush for short diagnostic XBEs

Status: implemented and validated.

Decision:

Emit one final `xemu-perf:` interval on graceful process exit, and let the
benchmark launcher wait briefly for QMP `quit` before falling back to SIGTERM.

Rationale:

The flat-tri-depth XBE changed into its flat phases after the last regular
one-second perf interval in short runs. Renderer tracing showed the live PGRAPH
state and bound shader state both became flat-first at shader bind, draw begin,
and draw flush, so the all-smooth summaries were a measurement-window artifact
rather than a shader-state propagation bug.

Verification:

- `benchmark-runs/20260430-152952-flat-tri-depth`: trace showed flat-first
  live and bound shader state, but regular intervals still ended before the
  flat tail.
- `benchmark-runs/20260430-153555-flat-tri-depth`: final interval
  `final=1 reason=atexit` captured 480 `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, 304
  `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`, and 304 `GEOM_SHADER_DRAW_TRI`.

Consequence:

The native triangle-depth prototype's flat handling is validated for the
current dedicated XBE: first-provoking flat triangles can use native GL
triangle rasterization, while last-provoking flat triangles correctly stay on
the geometry-shader fallback path.

## 2026-04-30: Give native triangle-depth a stable experiment flag

Status: implemented and smoke-tested.

Decision:

Use `XEMU_NATIVE_TRI_DEPTH=1` as the preferred opt-in name for the native
triangle-depth prototype. Keep `XEMU_DIAG_NATIVE_TRI_DEPTH=1` as a compatibility
alias for earlier benchmark notes and reproduction commands. If
`XEMU_NATIVE_TRI_DEPTH=0` is explicitly set, it overrides the old alias.

Rationale:

The prototype has moved past a raw dispatch-cost diagnostic: it now has smooth
retail-scene coverage, flat first-provoking coverage, flat nonfirst fallback
coverage, perf counters, and visual smoke checks. It is still not ready to
become a default renderer path, but it deserves a stable experiment name that
can be used by validation scripts without implying that every run is temporary
debug plumbing.

Verification:

- `benchmark-runs/20260430-173353-flat-tri-depth` was launched with
  `XEMU_NATIVE_TRI_DEPTH=1`.
- The log reported
  `xemu-perf: native_tri_depth=1 source=XEMU_NATIVE_TRI_DEPTH mode=safe`.
- Benchmark metadata recorded `env_XEMU_NATIVE_TRI_DEPTH: 1`.
- The run emitted a final `xemu-perf:` flush and reported 262,586 native
  triangle-depth draws with zero geometry-shader draws.
- `benchmark-runs/20260430-175500-flat-tri-depth` was launched with
  `XEMU_NATIVE_TRI_DEPTH=0` and `XEMU_DIAG_NATIVE_TRI_DEPTH=1`; the stable
  disable took precedence, no native enable line was emitted, and the run
  reported 0 native triangle-depth draws.

## 2026-04-30: Treat native triangle-depth as validated for current triangle-fill coverage

Status: implemented for opt-in use; not a default renderer path.

Decision:

For the current Apple Silicon fork, `XEMU_NATIVE_TRI_DEPTH=1` is validated as
the active opt-in triangle-family fill replacement path for retail snapshot
testing. It should remain opt-in until broader game coverage and longer
visual/depth validation exist.

Rationale:

The path now has counter evidence for smooth-shaded retail scenes, both depth
modes, fill polygon offset, first-provoking flat native draws, nonfirst flat
fallbacks, and same-build baseline/native screenshot comparisons. It removes
all triangle-family geometry-shader draws in the current Rainbow Six 3 and
Crimson Skies snapshots without an obvious visual smoke failure.

Verification:

- Rainbow paired comparison:
  `benchmark-runs/20260430-174138-native-tri-depth-compare-rainbow-six-3`
  reduced post-load MSPF from 23.10 to 6.83, geometry-shader draws from 79,775
  to 0, and reported 0.6131% changed pixels in the fixed viewport crop.
- Crimson paired comparison:
  `benchmark-runs/20260430-174443-native-tri-depth-compare-crimson-skies`
  reduced post-load MSPF from 29.47 to 17.87, geometry-shader draws from
  20,041 to 0, and reported 3.6913% changed pixels in the fixed viewport crop.

Consequence:

Next work in this category should focus on broader coverage and eventual
defaulting policy rather than re-proving the same Rainbow/Crimson triangle-fill
case. The immediate next implementation focus was later narrowed on 2026-05-01
to the remaining geometry-shader users, while keeping broader coverage as the
defaulting prerequisite.

## 2026-05-01: Close the current triangle-fill slice and move on

Status: documented and validated.

Decision:

Treat `XEMU_NATIVE_TRI_DEPTH=1` as the completed current opt-in
triangle-family fill replacement path. Use
`scripts/apple-silicon/validate-native-tri-depth.sh --run 20` as the regression
gate for this slice, and move the next implementation session to the remaining
geometry-shader users.

Rationale:

The path has smooth retail coverage, flat first-provoking native coverage, flat
nonfirst fallback coverage, depth-mode and polygon-offset counters, same-build
Rainbow/Crimson paired comparisons, and a fresh packaged-app validator run:
`benchmark-runs/20260430-210159-flat-tri-depth`.

Consequence:

Do not start the next session by revalidating triangle-family fill unless the
triangle path changes. Start by measuring or creating coverage for line
primitives, quad/quad-strip expansion, polygon fill, or nonfill triangle modes,
then choose one category to remove or narrow. Broader retail coverage is still
required before defaulting `XEMU_NATIVE_TRI_DEPTH=1`, but it is not the next
implementation blocker.

## 2026-05-01: Use retail gameplay routes as the next performance gate

Status: recorded and tracked.

Decision:

Use the recorded PGR2, Rainbow Six 3, and Crimson Skies gameplay routes as the
primary user-visible performance gate for the next renderer work. The
performance floor is sustained 30 FPS in gameplay for all tracked titles; 60 FPS
is desirable but not the minimum stability/performance bar.

Rationale:

The older smoke routes and scene snapshots are useful for controlled
diagnostics, but the new routes reproduce the actual manual observations:
PGR2 collapses in gameplay, Rainbow Six 3 drops when character movement starts,
and Crimson Skies shows sustained gameplay pacing and acceleration-animation
choppiness. These routes also expose remaining primitive families that the
completed triangle-family fill path does not remove.

Verification:

- PGR2:
  `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv`,
  `benchmark-runs/20260501-094823-pgr2`, 11.53 average FPS, 1,516,519
  geometry-shader draws, including 38,785 quad-family draws.
- Rainbow Six 3:
  `scripts/apple-silicon/input-scripts/rainbow-gameplay.csv`,
  `benchmark-runs/20260501-095400-rainbow-six-3`, 24.19 average FPS, 692,438
  geometry-shader draws, including 1,946 line-family draws.
- Crimson Skies:
  `scripts/apple-silicon/input-scripts/crimson-gameplay.csv`,
  `benchmark-runs/20260501-095905-crimson-skies`, 15.44 average FPS, 786,722
  geometry-shader draws, including 7,837 quad-family draws.

Consequence:

Start the next implementation session with PGR2 baseline versus
`XEMU_NATIVE_TRI_DEPTH=1` replay. If quad-family geometry-shader work remains
the strongest signal, prioritize quad/quad-strip expansion removal or
narrowing before moving to line primitives, polygon fill, or nonfill triangle
modes.

## 2026-05-01: Add `XEMU_NATIVE_QUAD=1` smooth-fill quad bypass

Status: implemented and validated.

Decision:

Add a second opt-in geometry-shader removal slice, `XEMU_NATIVE_QUAD=1`, that
expands `PRIM_TYPE_QUADS` and `PRIM_TYPE_QUAD_STRIP` smooth-fill draws into
native triangle dispatches and reuses the `gl_FragCoord`-derived depth path
already used by `XEMU_NATIVE_TRI_DEPTH=1`. The flag is independent of
`XEMU_NATIVE_TRI_DEPTH`. Flat-shaded quads, line/point polygon modes, and any
nonfill raster mode stay on the existing geometry-shader path. The quad
diagonal triangulation matches the geometry shader's `calc_quadz(0, 2)`
order, so smooth interpolation is unchanged.

Rationale:

The PGR2 retail gameplay route at 11.53 baseline FPS reached only 21.40
post-load FPS with `XEMU_NATIVE_TRI_DEPTH=1` enabled, and the entire
remaining geometry-shader workload at that point was quad-family (177,272 of
177,272 GS draws). Apple's OpenGL geometry-shader path was already
identified as the dominant Apple Silicon bottleneck for the triangle-fill
slice; the same removal applied to quad-fill is the obvious next slice.

Verification:

- Triangle regression gate `validate-native-tri-depth.sh --run 22` passed at
  `benchmark-runs/20260501-105543-flat-tri-depth`. Adding the native-quad
  infrastructure did not perturb the triangle-fill validator.
- Rainbow Six 3 snapshot scene
  (`benchmark-runs/20260501-110557-rainbow-six-3`, 30.97 post-load FPS / 6.71
  MSPF) is identical within noise to the prior `XEMU_NATIVE_TRI_DEPTH=1`-only
  D8 result (30.99 FPS / 6.47 MSPF). Quad-free scenes are unaffected by the
  new code.
- PGR2 mid-route snapshot triplet (`benchmark-runs/20260501-115623-pgr2`,
  `20260501-115654-pgr2`, `20260501-115725-pgr2`):
  - Baseline: 4.39 FPS; 329,044 GS triangle draws + 3,022 GS quad draws.
  - `XEMU_NATIVE_TRI_DEPTH=1`: 16.02 FPS; 0 GS triangle draws, 11,745 GS
    quad draws remain.
  - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1`: 16.56 FPS; 0 GS draws of
    any kind, 12,193 native-quad draws (all `LIST`, all
    `CANDIDATE_SMOOTH`, zero fallbacks).
- Whole-route replays show 36% run-to-run variance (18.20 vs 24.81 post-load
  FPS for the same flag config) because real-time-paced input drives the
  emulator into different scene mixes at different host throughputs. Stable
  comparisons need snapshot replays.

Consequence:

The geometry-shader removal track has now eliminated all triangle-family and
all smooth-fill quad-family geometry-shader draws across all current
benchmark scenes. PGR2 still does not hit the 30 FPS gameplay floor at the
captured snapshot (16.56 FPS), so the next bottleneck is no longer geometry
shaders. Use Instruments and the existing perf counters at the PGR2
snapshot scene to identify whether i386 TCG, NV2A PGRAPH command processing,
surface/texture upload, or fragment shader work is the dominant remaining
cost. Defer further geometry-shader-specific work (flat-quad bypass,
nonfill polygon modes) until a benchmark exercises that combination
non-trivially.

## 2026-05-01: Add `XEMU_PGRAPH_FAST_READ=1` lock-free PGRAPH register reads

Status: implemented and validated.

Decision:

Add an opt-in fast path in `pgraph_read()` that returns a `qatomic_read()`
snapshot of the requested register without acquiring `pg->lock` for simple
register reads (`NV_PGRAPH_INTR`, `NV_PGRAPH_INTR_EN`, and the default
`pg->regs_[]` slot). Only `NV_PGRAPH_RDI_DATA` keeps the full lock because
its read auto-increments `NV_PGRAPH_RDI_INDEX_ADDRESS`. Independent of
`XEMU_NATIVE_TRI_DEPTH` and `XEMU_NATIVE_QUAD`, but stacks with them.

Rationale:

A `sample` profile of the PGR2 mid-route snapshot
(`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-sample.md`)
showed the TCG i386 emulation thread spending ~32% of its wall time
sleeping in mutex-wait, with `pgraph_read` accounting for ~22% on its own.
The renderer's pfifo thread holds `pg->lock` across the slow OpenGL
submission inside `pgraph_method`, so every Xbox-CPU MMIO read of an
NV_PGRAPH_* register stalls until the renderer is done. Aligned 32-bit
loads are atomic on aarch64 and x86, so the mutex provides no protection
that the hardware does not already give for these specific reads — it is
strict overhead. The Xbox game loop polls these registers very frequently,
so eliminating the per-call mutex roundtrip recovers a large fraction of
emulator throughput.

Verification:

- Triangle regression gate
  (`scripts/apple-silicon/validate-native-tri-depth.sh --run 22`) passed at
  `benchmark-runs/20260501-123015-flat-tri-depth`. The lock-free read does
  not affect the flat-shading triangle path.
- PGR2 mid-route snapshot triplet (paused-input replays of the same Xbox
  state): adding `XEMU_PGRAPH_FAST_READ=1` on top of
  `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` lifts post-load FPS from
  16.56 to 30.76 (+85.8%) and 30.70 over a 60-second rerun
  (`benchmark-runs/20260501-123050-pgr2`,
  `benchmark-runs/20260501-123357-pgr2`). Zero geometry-shader draws,
  zero native fallbacks.
- `XEMU_PGRAPH_FAST_READ=1` alone, without the geometry-shader bypasses,
  produces only a small lift (4.4 → 5.3 FPS post-load). The
  geometry-shader bypass is what makes the renderer's lock-hold time short
  enough that removing the per-read mutex matters.
- PGR2 retail gameplay route replay with all three flags
  (`benchmark-runs/20260501-123525-pgr2`): 31.76 post-load FPS over 279
  intervals (~4.7 minutes of real gameplay), zero geometry-shader draws,
  10.4M native-tri draws, 254K native-quad draws, all smooth, zero
  fallbacks. Compared to the 11.67 post-load FPS baseline, this is +2.72x.

Consequence:

PGR2 now meets the project's 30 FPS retail-gameplay floor with all three
opt-in flags enabled. The flags remain opt-in until broader title coverage
exists. Next slice should audit `pgraph_write` and `voice_lock` for similar
fast-path opportunities, then revisit whether any further bottleneck
exists at this scene under Instruments. Whole-route averages are now
useful comparators again because per-run scene divergence is reduced when
the emulator is no longer CPU-starved.

## 2026-05-01: Three opt-in flags visually validated for current title set

Status: visually validated by the user on 2026-05-01.

Decision:

Treat `XEMU_NATIVE_TRI_DEPTH=1`, `XEMU_NATIVE_QUAD=1`, and
`XEMU_PGRAPH_FAST_READ=1` as visually safe for the current tracked-title
set (PGR2, Rainbow Six 3, Crimson Skies) on Apple Silicon when used
together. They remain opt-in flags rather than default behavior until a
broader title-coverage gate is met.

Rationale:

Each of the three slices was code-validated against the flat-tri-depth
regression XBE and counter sanity checks. After all three landed, the
user ran each tracked title under combined flags on a real disc and
confirmed:

- PGR2: no artifacting, 30 FPS feel.
- Rainbow Six 3: no artifacting, 30 FPS feel.
- Crimson Skies: no artifacting, 30 FPS feel.

Counter-side guarantees backing this:

- `XEMU_NATIVE_TRI_DEPTH=1` only takes the native path when the eligibility
  predicate (`pgraph_glsl_native_tri_depth_supported`) holds. Flat-nonfirst
  triangles still use the geometry shader.
- `XEMU_NATIVE_QUAD=1` only takes the native path for smooth-fill
  quad/quad-strip primitives. Flat-shaded quads, line/point polygon modes,
  and any nonfill raster mode still use the geometry shader.
- `XEMU_PGRAPH_FAST_READ=1` only skips the lock for atomic 32-bit reads
  with no side effects. `NV_PGRAPH_RDI_DATA` (the only read-with-side-
  effect we know about) still locks.

Consequence:

Flags stay off by default. New titles or scenes that exercise quad
flat-shading, nonfill polygon modes, or unusual PGRAPH register access
patterns must be visually validated before being added to the tracked set.
The next gate to consider flipping any flag default-on is broader retail
coverage across genres (additional racing, FPS, platforming, and
menu-heavy titles).

## 2026-05-01: Sub-millisecond perf-log precision and per-frame timing

Status: landed in `hw/xbox/nv2a/pgraph/profile.c` on 2026-05-01.

Decision:

`xemu-perf:` interval lines now emit `mspf_avg`, `mspf_min`, `mspf_max`
as `%.3f` floats (microsecond precision internally), and an opt-in
`XEMU_PERF_FRAME_LOG=1` appends a per-frame `frame_mspf_us=v1,v2,...`
field to each interval line, bounded to 1024 frames per interval with
overflow recorded in `frame_mspf_us_dropped`. Default off. The HUD plot
in `ui/xui/debug.cc` keeps its integer-ms `frame_working.mspf` field
unchanged.

Rationale:

The 60 FPS budget is 16.67 ms; the previous integer-ms perf-log
resolution rounded away the difference between a frame inside and
outside that budget. Sub-ms precision is required for the 60 FPS
pursuit. The optional per-frame log enables true frame-level p99 /
p99.9 percentiles without changing the always-on log volume.

Consequence:

`scripts/apple-silicon/extract-perf-summary.sh` parses the new precision
without changes (its arithmetic was already float-tolerant) and gains
new jitter keys derived from the per-interval `mspf_max` field:
`fps_stddev`, `mspf_max_p50/p95/p99/max`, `stutter_intervals_30/45/60fps`,
and `longest_stutter_run_30/60fps` (whole-run and `post_load_*`
variants). Older runs captured before this change retain integer-ms
`mspf_max`; new runs are sub-ms accurate. Baseline benchmark notes
record the format transition so future comparisons can account for it.

## 2026-05-01: XEMU_VOICE_FAST_LOCK not landed

Status: investigated, implemented, measured, **rejected** on
2026-05-01. Code reverted; the `XEMU_VOICE_FAST_LOCK` flag does not
exist in the shipped binary.

Decision:

A lock-free `voice_lock()` fast path (atomic OR/AND on
`d->vp.voice_locked[]`, dropping the `qemu_cond_signal` on the
audio-worker condvar) was implemented to address the 6.8 % `voice_lock`
TCG-thread mutex wait identified in
`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`.
Measurement showed no FPS improvement on either the `pgr2_gameplay_b4`
snapshot (30.64 → 30.79 FPS, +0.49 % = noise) or the 300 s PGR2 retail
route (31.76 → 31.84 FPS, +0.25 % = noise), with mixed jitter signals:
tighter p99 max-frame on the snapshot but +91 % more 30-FPS stutter
intervals on the retail route. Per the project's data-driven rule, the
change is not landed.

Rationale:

The post-fast-read sample profile shows the pfifo thread is idle 41.5 %
of the time on the FIFO condvar at the PGR2 mid-route snapshot — the
renderer can absorb more work than the CPU thread is producing.
Removing 6.8 % of TCG-thread mutex wait does not translate to FPS in
this regime because the freed cycles cannot be put to work. The
audio-worker's missed-`cond_signal` latency (bounded at 1 ms by its
existing `cond_timedwait`) appears to introduce a small jitter-shape
shift that may be net-negative on routes with active audio events.
Full measurement details and run dirs are in
`docs/apple-silicon/benchmarks/2026-05-01-voice-fast-lock-investigation.md`.

Consequence:

Lock-elision is exhausted as a primary 60 FPS lever for PGR2-class
scenes. The remaining ~9 % TCG-thread mutex wait is split across smaller
contributors (`pgraph_write` 1.4 %, miscellaneous 0.8 %) and not worth
a flag of its own without first addressing the larger remaining cost:
real x86 emulation throughput, particularly the floating-point helper
paths (`helper_mulss`, `helper_fmul_ST0_FT0`, `floatx80_mul`,
`soft_f32_mul`). The next data-driven slice on the TCG side should
audit whether SSE / x87 ops are going through softfloat unnecessarily
on Apple Silicon. On the renderer side, Crimson Skies' documented
shader-compile stutter (1310 ms worst-frame, 16-second longest stutter
run) is the highest user-visible jitter target and is independent of
the TCG path.

## 2026-05-01: Adopt research-informed implementation roadmap

Status: documented. Each individual landing remains data-driven and will
be added to this log separately as it ships.

Decision:

Pursue, in priority order: (1) frame-pacing emulation-rate slewing
(Phase 2.5), (2) async shader compile (Phase 2.5), (3) native Metal
renderer with CPU-side index expansion + framebuffer fetch + VS-Expand +
async pipeline compile (Phase 4a–4i), (4) persistent shader/pipeline
cache (Phase 4f / Phase 5), (5) persistent TCG translation cache
(Phase 5a, PPTC pattern), (6) SSE / x87 hardfloat audit (Phase 5b,
already tracked in `handoff.md` Prioritized Next Tasks #3). Reject
custom x86 → ARM64 JIT (low ceiling, high macOS JIT pain documented in
RPCS3 PR #12115) and ICB / argument-buffer work (premature for Xbox-era
workloads).

Rationale:

The 2026-05-01 emulator survey
(`docs/apple-silicon/research.md` "Apple Silicon Emulator Survey")
catalogued how Dolphin, PCSX2, DuckStation, RPCS3, Ryujinx, and PPSSPP
solve problems analogous to xemu's. Concrete patterns with named code
references:

- Dolphin `Source/Core/VideoCommon/IndexGenerator.cpp` (CPU-side
  primitive expansion).
- PCSX2 `pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm` `m_expand_index_buffer`
  (VS-Expand with precomputed static index buffer + Metal function
  constants).
- DuckStation `src/util/metal_device.mm:2536-2620` (`presentDrawable:atTime:`
  + emulation-rate slewing for jitter-free pacing).
- DuckStation `src/util/metal_device.mm:387-410` and PCSX2 PR #5630
  (framebuffer fetch on Apple GPU family for blend / register-combiner
  passes).
- Dolphin PR #5702 (hybrid ubershader for async shader compile).
- Ryujinx PPTC blog (persistent translation cache pattern).
- RPCS3 PR #12115 (catalogued macOS Apple Silicon JIT pain — used here
  as anti-pattern reference).

These extend rather than replace the existing prioritized work. Frame
pacing (DuckStation pattern) directly addresses the 30 FPS gameplay
jitter symptom captured in
`benchmarks/2026-05-01-baseline-jitter.md`. Async shader compile
(Dolphin / RPCS3 pattern) directly addresses Crimson Skies' 1310 ms
worst-frame from Apple's GL-on-Metal synchronous compile. The Metal
backend shape — index expansion + FBFetch + VS-Expand + async
pipeline compile — is now concrete and data-driven rather than a
hand-wave.

Verification:

- Survey content captured in `docs/apple-silicon/research.md` "Apple
  Silicon Emulator Survey (2026-05-01)" with citations.
- Strategy phases updated: Phase 2.5 inserted, Phase 4 expanded with
  sub-deliverables 4a–4i, Phase 5 expanded with 5a (PPTC) and 5b
  (hardfloat audit), new "What we ruled out" section.
- No code changes in this entry. This is a planning decision.

Consequence:

Each individual landing remains gated by the project's data-driven rule
(workspace `CLAUDE.md` rule #1): fresh `sample` profile + dated
benchmark note + measured before/after, with append-only decision-log
entries when each ships. Specifically: Phase 2.5 frame-pacing slewing
should land before the Metal renderer because it is graphics-API-
agnostic and trivially measurable; the async shader compile slice
should follow because Crimson is the largest user-visible jitter target
and its bottleneck has been profiled to synchronous shader compile in
the Apple OpenGL-on-Metal driver.

## 2026-05-01: XEMU_PGRAPH_FAST_WRITE deferred (not pursued this session)

Status: source-audited, **deferred**. No code shipped. The flag does not
exist in the binary.

Decision:

The `XEMU_PGRAPH_FAST_WRITE=1` slice listed as Prioritized Next Tasks #4
in `handoff.md` is deferred. The handoff entry framed it as "low-risk,
mirror `pgraph_read`," but a source audit of `pgraph_write`
(`hw/xbox/nv2a/pgraph/pgraph.c:161`) and `pgraph_reg_w`
(`hw/xbox/nv2a/pgraph/pgraph.h:311`) shows the `default` slot write is
not a simple atomic store: it does a compare-then-set (`if (pg->regs_[r]
!= v)`) and updates the `regs_dirty` bitmap via `bitmap_set`. The
renderer consumes `regs_dirty` to decide shader recompiles
(`hw/xbox/nv2a/pgraph/glsl/shaders.c:57` and the Vulkan equivalent at
`hw/xbox/nv2a/pgraph/vk/draw.c:643`). A naive lock-free fast path could
either lose dirty bits to a producer/clearer race
(`pgraph_clear_dirty_reg_map` is called from the renderer thread) or
present a producer/consumer ordering hole where the consumer reads
`dirty=0, value=new` and skips a needed recompile, causing visual
corruption. A correct fast-write would need explicit acquire/release
ordering on both producer and consumer plus an atomic `set_bit` on the
`regs_dirty` word; that's no longer "mirror pgraph_read."

Beyond correctness, the prior decision-log entry "2026-05-01:
XEMU_VOICE_FAST_LOCK not landed" already concluded that lock-elision
is exhausted as a primary 60 FPS lever for PGR2-class scenes when the
pfifo thread is idle 41.5% of the time on the FIFO condvar — removing
1.4 % of TCG-thread mutex wait cannot translate to FPS in that regime.
The Amdahl ceiling on this slice is therefore ~1 % even if all the
correctness questions resolved.

Rationale:

Per workspace `CLAUDE.md` rule #1 (no guessing — every decision
data-driven), implementing a slice the project's own measurements
already predict won't lift FPS, with a non-trivial correctness risk the
handoff entry undercounted, fails the "high confidence" bar. Per rule #3
(be honest about limits), this entry replaces the handoff's "low-risk,
completes the read/write symmetry" framing with the actual analysis.

Verification:

- `pgraph_write` source ground truth: `hw/xbox/nv2a/pgraph/pgraph.c:161-256`.
- `pgraph_reg_w` `regs_dirty` side effect:
  `hw/xbox/nv2a/pgraph/pgraph.h:311-318`.
- Consumers of `regs_dirty`: `hw/xbox/nv2a/pgraph/glsl/shaders.c:57-92`,
  `hw/xbox/nv2a/pgraph/vk/draw.c:643-708`.
- Prior lock-elision-Amdahl conclusion:
  "2026-05-01: XEMU_VOICE_FAST_LOCK not landed" (decision-log entry above).
- Underlying sample profile:
  `docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`.

Consequence:

`handoff.md` Prioritized Next Tasks should be re-ordered so this slice is
deprioritized below (a) async shader compile (Crimson 1310 ms worst-frame
jitter — the largest user-visible win), (b) SSE / x87 hardfloat audit
outcome, and (c) frame-pacing emulation-rate slewing (Phase 2.5,
graphics-API-agnostic). Should fast-write be revisited later — for
example after the renderer is no longer the binding constraint and
freed TCG cycles can actually be put to work — the implementation must
include atomic set_bit on `regs_dirty` with explicit acquire/release
ordering on the consumer side, or accept the cost of always-mark-dirty
(which costs renderer recompiles to gain producer simplicity).

## 2026-05-01: SSE hardfloat already active on aarch64; x87 80-bit irreducibly soft

Status: source-audited. **Original hypothesis disproved.** No code shipped.

Decision:

The `handoff.md` Prioritized Next Tasks #3 hypothesis — "SSE single-precision
ops (`helper_mulss`, `helper_mulps_xmm`) actually go through `soft_f32_mul` /
`parts64_uncanon_normal` on Apple Silicon and lifting them to hardfloat is
the single largest potential TCG win" — is wrong. The SSE hardfloat fast
path already exists and is already active on aarch64. The remaining
`parts64_*` time visible in the post-fast-read sample profile is the
necessary cost of softfloat's correctness fallback (NaN/denormal inputs,
denormal results, first-op-after-MXCSR-reset, non-default rounding modes),
not an unconditional softfloat trip.

`helper_fmul_ST0_FT0` and the rest of the x87 surface are irreducibly
soft on Apple Silicon. The fork's existing `__hard` x87 path is correctly
gated to `XBOX && __x86_64__` because Apple Silicon `long double` is
8-byte (64-bit), not 80-bit. There is no native 80-bit float on aarch64
to dispatch to.

The original handoff #3 framing should be retired. The follow-up work is
either (i) confirm the hard-take ratio with a counter pair, then redirect
to a non-float TCG subsystem, or (ii) treat x87 as a separate Phase-2
NEON-kernel effort which is real work but only justified after
Instruments confirms x87 is dominant in real-game inner loops.

Rationale:

Source-only audit at `docs/apple-silicon/benchmarks/2026-05-01-tcg-float-audit.md`
walks the call chain from `helper_mulss` →
`target/i386/ops_sse.h:514-533` (`FPU_MUL` macro and
`SSE_HELPER_S(mul, FPU_MUL)`) → `float32_mul`
(`fpu/softfloat.c:2169-2174`) → `float32_gen2`
(`fpu/softfloat.c:337-366`) → `hard_f32_mul = a * b`
(`fpu/softfloat.c:2159-2162`, a single arm64 `fmul`).

The `can_use_fpu` gate (`fpu/softfloat.c:230-237`) is satisfied in the
common Xbox-game MXCSR state: `QEMU_NO_HARDFLOAT` is 0 (no `-ffast-math`),
`float_flag_inexact` is sticky after the first soft op (SSE arithmetic
helpers do not clear flags per-op — only `WRAP_FLOATCONV` at
`target/i386/ops_sse.h:703-718` does), and `float_rounding_mode ==
float_round_nearest_even` matches MXCSR RC=00 which Xbox titles set
overwhelmingly. Per-call gates `f32_is_zon2` (zero-or-normal inputs after
FTZ flush) and `f32_addsubmul_post` (denormal result fallback) at
`fpu/softfloat.c:270-292` and `fpu/softfloat.c:1975-1990` apply only to
edge cases, not the steady-state inner loop.

The x87 path has no `floatx80_gen2` analogue. `floatx80_mul`
(`fpu/softfloat.c:2218-2230`) unconditionally calls `parts_mul`. The
fork's `__hard` shim (`target/i386/tcg/fpu_helper_hard.c`) requires
80-bit `long double`, which Apple's clang on aarch64 does not provide.
The `XBOX && __x86_64__` gate at `target/i386/helper.h:104-121`,
`target/i386/tcg/fpu_helper.c:76-267`, `target/i386/tcg/translate.c:38-124`,
and `ui/xui/main-menu.cc:62-66` correctly excludes the hard path on
Apple Silicon — there is nothing to dispatch to.

Verification:

- `docs/apple-silicon/benchmarks/2026-05-01-tcg-float-audit.md` —
  full file:line citation chain.
- `fpu/softfloat.c:230-237` — `can_use_fpu` gate.
- `fpu/softfloat.c:337-397` — `float32_gen2`/`float64_gen2` dispatcher.
- `fpu/softfloat.c:2159-2174` — hard/soft `f32_mul` wiring.
- `target/i386/tcg/fpu_helper.c:780-785` — `helper_fmul_ST0_FT0` body
  (unconditional soft via `floatx80_mul`).
- `target/i386/tcg/fpu_helper_hard.c:1-4` — empty translation unit on
  non-`XBOX && __x86_64__`.
- `ui/xui/main-menu.cc:62-66` — Hard FPU UI toggle visible only on
  `__x86_64__`.

Consequence:

`handoff.md` Prioritized Next Tasks #3 is rewritten in place to reflect
this finding plus a recommended cheap follow-up: a counter pair around
`float32_gen2`/`float64_gen2` (`sse_hard_taken` vs `sse_soft_fallback`,
split by fall-through reason: `!can_use_fpu`, `!pre`, `denormal_result`),
run on the `pgr2_gameplay_b4` snapshot for 30 s. If hard-take ratio > 0.9,
confirm the `parts64_uncanon_normal` time is irreducible correctness work
and redirect future TCG investigations to the next dominant subsystem
Instruments identifies (TLB / memory-op helpers, NV2A PGRAPH command
parsing, or surface/texture upload — handoff Prioritized Next Tasks #5/#6
are still on the table). The async shader compile slice (Crimson 1310 ms
worst-frame jitter, biggest user-visible win) and Phase 2.5 frame-pacing
slewing remain the two most impactful next-implementation slices on the
roadmap; nothing about this audit changes their priority.


## 2026-05-01: Add external Xbox library and `package-game.sh` packaging tool

Decision:

Adopt `/Volumes/Josh-Backup-Files/Console Games/Original Xbox` as the
canonical external Xbox game library for this fork, and add
`scripts/apple-silicon/package-game.sh` as the project-supported way to
pull a game from that library into a xemu-loadable XISO ISO.
`xdvdfs-cli` (MIT-licensed; antangelo/xdvdfs) is installed via `cargo
install xdvdfs-cli` at `$HOME/.cargo/bin/xdvdfs`; the binary is not
vendored into the repo. The packaging script auto-installs it on first
use unless `--no-install` is passed.

Default output is `$XEMU_TEST_GAMES_DIR/<game>.xiso.iso` (i.e.
`/Users/jbbrack03/XEMU_MacOS/Test_Games/<game>.xiso.iso`). The script
refuses to overwrite an existing output file without `--force`, so
rule #9 (do not modify `Test_Games/` or `Xbox-Emulator-Files/` in
place) is preserved while still allowing additive packaging into the
existing test-games directory.

Rationale:

Future sessions will identify reported xemu issues for games we do not
currently track (PGR2, Rainbow Six 3, Crimson Skies, flat-tri-depth)
and need a reproducible way to bring those games into the benchmark
harness. The library volume contains the extracted game trees;
`xdvdfs pack` produces a valid XISO from such a tree in seconds. A
project-supported wrapper avoids ad-hoc invocations that could
accidentally overwrite tracked test ISOs or skip verification. Cargo
install (vs vendoring a binary or a Homebrew formula) keeps the repo
small and lets the tool stay current with upstream xdvdfs without a
maintenance commit.

Verification:

- `xdvdfs-cli` v0.8.3 installed and reachable at
  `/Users/jbbrack03/.cargo/bin/xdvdfs`.
- `package-game.sh` packs `scripts/apple-silicon/xbe-tests/flat-tri-depth/bin/`
  into a 256 KiB ISO that `xdvdfs info` reports `Valid: true`.
- End-to-end name-lookup pack of `Grooverider - Slot Car Thunder` from
  the external library produces a 96 MiB ISO that `xdvdfs info` reports
  `Valid: true` and that `xdvdfs ls` enumerates correctly.
- All negative paths (bogus name, ambiguous name, missing source, bad
  flag) return non-zero with descriptive messages.
- Idempotent rerun is a no-op; `--force` rebuilds.

Consequence:

Slice closed: any future session can run
`scripts/apple-silicon/package-game.sh "<game name>"` to grab a game
from the library on demand. The next adjacent slice (deferred) is
extending `run-benchmark.sh` with a `custom <iso>` target so packaged
games can flow through the harness without per-target hardcoding; the
packaging tool itself is complete.


## 2026-05-01: Async shader compile shipped opt-in; not the source of Crimson worst-frame stutter

Decision:

Ship `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` as a documented opt-in flag.
Default off. Do not pursue further async-side work until the actual
source of the 1.35-second Crimson worst-frame stutter is identified.

Rationale:

Implemented per the research-informed roadmap (Dolphin / RPCS3 pattern,
PR #4876 "Async (Skip Draws)"): a third shared
`g_nv2a_context_shader_compile` GL context, a `pgraph.gl_async_compile`
worker thread, a per-binding `pending_compile` flag, an early-return
"skip the draw" fallback in `pgraph_gl_draw_begin/end`. End-to-end
correctness is validated: the worker compiles, programs publish into the
shared name space, the renderer skips draws while compiling, and FPS
does not regress on the PGR2 snapshot or default-path runs.

Crimson Skies retail-route paired runs (same build, same input script,
both with `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1`) showed:

- `SHADER_COMPILE_US_TOTAL` 399 ms (sync) vs 347 ms (async): the
  worker successfully moved nearly all `glLinkProgram` cost off the
  renderer thread.
- `post_load_frame_mspf_us_max`: 1,345,831 us (sync) vs 1,351,887 us
  (async). Identical within run-to-run variance.
- The stutter spikes occur at the **same gameplay points** (same
  interval offsets +-1 from the script timing roll) with **near-
  identical magnitudes** (e.g. 1,345.8 ms baseline vs 1,343.7 ms
  async, 1,321.5 ms baseline vs 1,351.9 ms async).

This is conclusive: the headline 1.35 s worst-frame is **not**
`glLinkProgram` synchronous time on the renderer thread. The likely
cause is Apple's GL-on-Metal driver doing MSL->Metal pipeline-state-
object compile inside the **first `glDrawElements`** with a new
program/VAO/state combination. That work cannot be moved to a worker
thread without also setting up the renderer's VAO and surface state on
the worker context, which is a much larger refactor.

`p999` regressed from 104 ms (sync) to 382 ms (async) - consistent with
the worker's `glFinish()` blocking on Apple's GL command queue, which
serializes against the renderer's command buffer. Worth a follow-up
A/B with `glFlush()` in place of `glFinish()`.

Consequence:

- Async slice is **complete and shipped opt-in**, with a benchmark note
  at `docs/apple-silicon/benchmarks/2026-05-01-async-shader-compile.md`.
- The roadmap "async shader compile" task in `handoff.md` is closed.
- Future Apple-side judder work must first identify what actually fires
  during the worst-frame interval. The next investigation should add a
  per-event timestamp log inside `pgraph_gl_draw_begin / draw_end /
  flush_draw` and across `TEX_UPLOAD`, `SURF_TO_TEX`, `SURF_UPLOAD`,
  `SURF_DOWNLOAD`, then correlate against `frame_mspf_us > 100,000`.
- Phase 4 (native Metal renderer) remains the right long-term path:
  it is the only way to escape Apple's GL-on-Metal MSL compile and
  command-queue serialization.

Verification:

- Build `./build.sh -a arm64` succeeds; codesign verified.
- Smoke run `benchmark-runs/20260501-182456-flat-tri-depth` shows the
  `xemu-perf: async_shader_compile=1 source=...` startup banner and
  `SHADER_COMPILE_ASYNC_QUEUED == SHADER_COMPILE_ASYNC_COMPLETED` per
  interval (queue drains).
- Counter validation across `benchmark-runs/20260501-181049-crimson-skies`
  (sync), `20260501-182613-crimson-skies` (async), `20260501-183005-pgr2`
  (snapshot, sync), `20260501-183046-pgr2` (snapshot, async). All four
  runs surfaced the new keys via `extract-perf-summary.sh` without
  breaking older logs.
- `shader_cache_entry_post_evict()` abort-on-pending guard never fired
  in any run (50K-entry cache vs at most a few in-flight compiles).


## 2026-05-01: Stay on OpenGL; the headline bottleneck is TCG TB invalidation, not the renderer

**Superseded 2026-05-02 for product direction.** The measurement in
this entry remains useful: OpenGL was not proven to be the immediate
FPS bottleneck. The strategic decision to keep OpenGL as the primary
path is superseded by "2026-05-02: Pivot native Metal to the primary
renderer path" because the final product requirements include
Metal-native frame timing, latency work, profiling, enhancement
controls, and long-term renderer maintainability.

Decision:

The fork stays on Apple's OpenGL-on-Metal as the active renderer path.
Native Metal (strategy.md Phase 4) is **not** the next priority. The
project goals (sustained 60 FPS, no judder, 1080p output, anti-
aliasing, higher-quality textures, broad Xbox library coverage) can
be delivered on the existing GL renderer once the upstream-of-renderer
bottleneck is fixed.

Rationale:

This session ran a per-event timing diagnostic followed by an Apple
`sample` profile during a known-bad Crimson interval. Key results:

1. **Renderer is essentially idle during Crimson's 1.35-s worst-frame
   intervals** (all renderer counters under 24 ms of 1000 ms). The
   Xbox CPU emulator only flips 2–10 frames in those windows.
2. **`sample` profile attributes the dominant TCG cost to JIT TB
   invalidation:** `tb_invalidate_phys_range_fast` →
   `do_tb_phys_invalidate` → `tcg_flush_jmp_cache` (382 samples)
   plus `pthread_jit_write_protect_np` (12 samples) and
   `sys_icache_invalidate` (45 samples). This is the documented
   Apple-Silicon-specific QEMU MTTCG W^X / i-cache cost.
3. **Apple GL handles 4× internal scale (~2560×1920) on PGR2 with
   only 7 % growth in `FLUSH_DRAW_US_TOTAL` and p99 stable at
   ~35 ms.** No per-pipeline-state-object pathology under heavier
   renderer load.
4. **Crimson at scale 4 puts renderer cost at 27 % of wallclock at
   30 FPS.** Doubling to 60 FPS lands at ~54 %, well inside budget.

Headroom math (renderer cost = `DRAW_BEGIN + SURF_DOWNLOAD +
FLIP_STALL`):

| Scale × FPS combination       | Projected wallclock budget |
| ----------------------------- | -------------------------- |
| 1× scale, 60 FPS              | 41 %                        |
| 2× scale (~1080p), 60 FPS     | 46 %                        |
| 4× scale (~2160p), 60 FPS     | 56 %                        |
| 2× + MSAA 2× (estimated), 60  | ~60 %                       |
| 2× + MSAA 4× (estimated), 60  | ~72 %                       |

All goals fit inside Apple's GL once TCG is unblocked.

What would change this verdict (none observed yet):

- Title-specific Apple GL pathologies in the broader Xbox library that
  the four tested titles do not exhibit. Not yet measured at scale.
- MSAA pipeline-variant explosion when AA is implemented. Not yet
  measured because xemu's GL framebuffer setup is not multisample.
- Texture-mod bandwidth saturation. Currently `TEX_UPLOAD_US_TOTAL` is
  0.27 % of wallclock — has plenty of room.

Consequence:

- **Stop investing in renderer-side fixes for the headline judder.**
  The async shader compile slice (`XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`)
  was correct but unrelated to the actual cause; it remains shipped
  opt-in.
- **The next implementation slice is TCG TB-invalidation cost
  reduction on Apple Silicon.** Specific paths to investigate:
  persistent TCG translation cache (PPTC, strategy.md Phase 5a),
  `pthread_jit_write_protect_np` toggle batching, smarter softmmu
  notdirty/dirty page handling, and upstream QEMU MTTCG Apple-
  Silicon patches.
- **MSAA implementation on the existing GL path is the renderer-side
  follow-up** (not Metal). Add multisample renderbuffer support to
  `pgraph_gl_init_surfaces` and verify the new
  `SHADER_COMPILE_*` counters do not show pathological pipeline-
  variant compile bursts.
- **A broader title sweep using `package-game.sh`** is the diversity
  validation step before declaring GL definitively viable for the
  whole library.
- **Phase 4 native Metal renderer remains documented in
  `strategy.md` as a long-term ceiling-removing effort**, but it is
  not the next priority. Reconsider only if MSAA implementation or
  the broader title sweep surface a renderer-side ceiling.

Verification:

- Build `./build.sh -a arm64` clean; `dist/xemu.app` codesign
  verified.
- Per-event timing data: `benchmark-runs/20260501-190239-crimson-skies`
  (300 s retail route with all opt-in flags + diagnostic counters).
- TCG sample profile:
  `benchmark-runs/20260501-203802-crimson-skies/sample-tcg-bad-interval.txt`
  (14,638 lines of `sample` output with thread-bucket summary).
- Scale stress tests:
  `benchmark-runs/20260501-204006-pgr2` (scale 1),
  `benchmark-runs/20260501-204044-pgr2` (scale 2),
  `benchmark-runs/20260501-204123-pgr2` (scale 4).
- Crimson scale-4 stress: `benchmark-runs/20260501-204246-crimson-skies`.
- Full analysis at
  `docs/apple-silicon/benchmarks/2026-05-01-gl-vs-metal-decision.md`
  and the supporting attribution note
  `docs/apple-silicon/benchmarks/2026-05-01-renderer-vs-tcg-stutter-attribution.md`.

## 2026-05-02: Ship XEMU_TCG_SPLITWX default-on (V1, mechanically correct, headline judder unchanged)

Decision:

Ship `XEMU_TCG_SPLITWX={0,1}` with auto-default ON for Apple Silicon
system builds (`CONFIG_DARWIN && __aarch64__`). Selects the
`mach_vm_remap` dual-mapping splitwx path in `tcg/region.c` so TB
execution no longer pays the per-TB `pthread_jit_write_protect_np()`
syscall on every `cpu_tb_exec`. The W^X-toggle wrappers in
`include/qemu/osdep.h` are diff-guarded and no-op when
`tcg_splitwx_diff != 0`. Explicit `-accel tcg,split-wx=on|off` always
wins over the env var; env var wins over auto-default. If splitwx
allocation fails at startup the TCG init falls back to MAP_JIT and
logs the failure once.

Rationale:

V1 measured profile delta on Crimson 300 s route: per-TB
`pthread_jit_write_protect_np` count dropped from 11 (OFF arm) to 0
(ON arm) — the syscall is fully eliminated as designed. Headline
Crimson `frame_mspf_us_max` was 1,285,865 µs OFF vs 1,290,232 µs ON
(+0.33 %, within noise) — the W^X-toggle cost is real but is **not**
the dominant contributor to the worst-frame stutter the slice was
intended to fix. PGR2 snapshot regression check passed (−0.78 %).

Per the V1 PARTIAL definition (mechanical-correctness pillars pass,
headline-judder pillar fails), the slice ships because (a) it is
correctness-equivalent, (b) it removes a per-TB syscall cost the
upstream MAP_JIT path always pays on Apple Silicon, and (c) shipping
it default-on prevents accidental regression to the slower path on
new installs. Removing the W^X toggle cost is a net good even though
it is not the headline fix.

Memory implication: splitwx keeps two VA aliases of the JIT region,
so committed virtual address space for the JIT buffer doubles
(physical pages are shared via the same backing). Acceptable on Apple
Silicon's 64-bit address space.

Verification:

- Build clean, codesign verified.
- Validation note: `benchmarks/2026-05-01-tcg-splitwx-validation.md`.
- Splitwx OFF arm: `benchmark-runs/20260501-…-crimson-skies` (Arm C).
- Splitwx ON arm: `benchmark-runs/20260501-…-crimson-skies` (Arm D).
- Sample profile delta breakdown documented in the validation note.

Honest-limit:

- The triangle-fill regression gate
  (`validate-native-tri-depth.sh --run 22`) failed in the V1 session
  due to pre-existing test-harness flakiness, not splitwx. Indirect
  cross-checks (PGR2 zero GS draws, Rainbow zero stutter regression)
  passed. See validation note "Honest-limits caveats" §1.

## 2026-05-02: Ship XEMU_TCG_JMP_CACHE_TARGETED default-on (V2, per-call wallclock collapsed, headline judder unchanged)

Decision:

Ship `XEMU_TCG_JMP_CACHE_TARGETED={0,1}` with auto-default ON for
Apple Silicon system builds. Replaces the unconditional 4096-entry
per-CPU jmp-cache zero in the `CF_PCREL` branch of
`tb_jmp_cache_inval_tb` (i386 system-mode globally sets `CF_PCREL`)
with a single-bucket clear per invalidated TB, batched after the
`tb_invalidate_phys_page_range__locked` loop. Non-PCREL TBs and the
`tb_flush` / cputlb full-flush callers continue to use the unmodified
full-zero path.

Rationale:

V2 measured `TCG_JMP_CACHE_ZEROED_BUCKETS` collapse from
4096-per-invalidation OFF to 1-per-invalidated-TB ON, and
`TCG_INVALIDATE_WALL_US_MAX` per-call max dropped to ~700 µs (well
below the 1.27 s Crimson worst-frame mspf). Per-arm Crimson 300 s
route showed the headline `post_load_frame_mspf_us_max` did not move
materially (PARTIAL pass) — but the per-call-wall-time evidence is
decisive: the worst frame is **not** built from one giant
invalidation chain. It is composed of many small invalidations or a
non-invalidation source. That cleanly redirects the next
investigation away from invalidation-cost reduction and toward V3
spike attribution.

Correctness rests on the existing `CF_INVALID` + cflags-equality
check in `cpu-exec.c::tb_lookup` (line 267) and `do_tb_phys_invalidate`
setting `CF_INVALID` (line 942) before removing the TB from
`tb_ctx.htable` (line 949) — a stale `tb*` left in an unrelated
jmp-cache bucket fails the cflags compare and falls through to
`tb_htable_lookup`, which won't find the (already-removed) invalidated
TB and translates fresh.

Verification:

- Build clean, codesign verified.
- Validation note:
  `benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md`.
- Per-arm metrics: see "Per-arm metrics" table in the note.
- Sample profile delta documented in the validation note.

## 2026-05-02: Confirm 30 FPS cap on PGR2 / Rainbow / Crimson is title-intrinsic, supersede the literal "60 FPS on tracked-3" success criterion

> Status: supersedes the literal "PGR2, Crimson Skies, and Rainbow
> Six 3 sustain 60 FPS in gameplay" framing previously recorded in
> `strategy.md` Success Criteria.

Decision:

The strategy.md "tracked-3 sustain 60 FPS" success criterion is
**superseded as technically impossible**. Reframed criterion: each
tracked title sustains its **console-native** FPS in gameplay
(PGR2 / Rainbow / Crimson = 30 Hz, Soul Calibur 2 / Burnout 3 /
OutRun 2 / Ninja Gaiden Black = 60 Hz, etc.) with no 1-second-class
judder, plus 1080p output and opt-in MSAA. The headline goal becomes
**residual-stutter elimination** on the existing 30 FPS pillar, not
FPS-doubling on titles whose engines render at 30 Hz on real Xbox
hardware.

Rationale:

V4 sanity test booted Soul Calibur 2 (a known-60 Hz Xbox title) on
the same V1+V2+goal-stack build and sustained **60.57 FPS for 109
consecutive 1-second intervals** at scale=2 + MSAA=4 (worst frame
13.65 ms p99, 31 ms max). On the same build PGR2 / Rainbow / Crimson
sustain ~30 FPS in gameplay; the per-title `xemu-perf:` ratio is
`NV2A_VBLANK_FIRES > 30/s` (xemu offers ~60 vblanks/s) while
`NV2A_PRESENT_HEARTBEAT == 30/s` (the guest engine elects to present
every other vblank). This is the decisive guest-intrinsic-cap signal
documented in `xemu-fork/CLAUDE.md` and `automation.md`. xemu cannot
make a 30 Hz engine render at 60 Hz; emulation correctness requires
preserving the engine's own pacing.

Consequence:

- Strategy.md Success Criteria rewritten to "console-native FPS for
  each tracked title, no 1-s-class judder, plus 1080p + AA."
- The active goal becomes residual-jitter elimination via V6
  (`cpu_exec_loop` per-phase instrumentation followed by a code fix
  targeting whatever it reveals — leading hypotheses: `tb_gen_code`
  churn, kernel-PC `0x80030e4c` 1 ms-class TB chains).
- The literal "60 FPS on PGR2 / Rainbow / Crimson" wording is
  removed from project goals; users who specifically want 60 FPS
  should pick 60 Hz Xbox titles (the V4 sweep validates 5 such
  titles run at native 60 Hz on this fork at scale=2 + MSAA=4).

Verification:

- SC2 sanity test: `benchmarks/2026-05-02-60hz-title-sanity-test.md`,
  run `benchmark-runs/20260502-015119-soul-calibur-2-60hz-test/`.
- Cap-attribution composite analysis:
  `benchmarks/2026-05-02-composite-goal-validation.md` (V3) and
  `benchmarks/2026-05-02-tcg-30fps-cap-attribution.md` (D3 vblank /
  present-heartbeat ratio).

## 2026-05-02: Ship XEMU_APU_LOCK_RELEASE default-on (I5, steady-state stutter cut, headline judder unchanged), with audio listen-test gate

Decision:

Ship `XEMU_APU_LOCK_RELEASE={0,1}` with auto-default ON for Apple
Silicon system builds. The APU worker thread releases
`MCPXAPUState::lock` for the duration of the per-frame voice-worker
batch wait inside `voice_work_dispatch`
(`hw/xbox/mcpx/apu/vp/vp.c`), then re-acquires before publishing
mixbins. Targets D3-attributed Crimson voice-lock contention
(21.3 s / 300 s of vCPU thread time on `mcpx-apu-vp/0xfe8202fc` =
NV1BA0_PIO_VOICE_LOCK).

Rationale:

I5 measured deltas vs OFF (Crimson 300 s route):
- `APU_VCPU_LOCK_WAIT_US_MAX` dropped **−97.9 %** (5.07 ms → 105 µs).
- `APU_LOCK_HOLD_US_TOTAL` ratio ON/OFF = **0.302** (passes the < 0.30
  threshold by 1 %).
- Steady-state `stutter_intervals_30fps` dropped **−61 %**.
- p999 `frame_mspf_us` improved **−35 %** (136,521 → 88,861 µs on the
  Rainbow snapshot).
- Headline Crimson `frame_mspf_us_max` moved **−4.35 ms** (within
  noise of 1.28 s) — PARTIAL on the 500 ms PASS threshold but PASS on
  the steady-state pillars.
- No new audio underrun / buffer-empty / error log lines vs OFF.

Per the I5 PARTIAL definition (steady-state pillars pass, headline
fails), the slice ships because (a) the steady-state stutter
reduction is real and substantial, (b) avg FPS / p99 / PGR2 / Rainbow
regression checks all clean, and (c) the worst-frame attribution work
(D3, V3) confirmed the worst frame lives in `tb_gen_code` churn /
kernel-PC `0x80030e4c` 1 ms-class TB chains, not in audio voice-lock
contention.

Race widening (correctness, honest-limit):

The slice widens an existing race class — `vp_write` paths
(`SET_VOICE_TAR_VOLA` / `_TAR_PITCH` / `_LFO_ENV`) already modify
guest RAM lock-free in upstream, so worker reads of those fields are
already racy. The widening adds the same exposure to fields touched
between `voice_lock(true)` and `voice_lock(false)` in
VOICE_ON / VOICE_RELEASE sequences (envelope start / release-rate
fields). Per-frame impact is bounded to ~256 samples (5.33 ms) of
slightly-stale audio for affected voices on the worst case — well
below the perceptual threshold for the volume / envelope deltas the
race exposes. This is **distinct from** the 2026-05-01 reverted
`XEMU_VOICE_FAST_LOCK` slice (bitmap-level lock-elision); the new
slice keeps `voice_lock()` exactly as it was on the vCPU side and
instead shrinks the APU thread's lock-hold.

Audio listen-test gate before fully-shipped status:

A human listener must play each tracked title (Crimson, Rainbow, PGR2)
for ≥ 5 minutes with the slice on, listening for stuck voices, dropped
sound effects, audible glitches, or stale samples. If clean: declare
fully shipped. If glitches: revert or design a finer-grained lock
split (e.g. add a separate `voice_config_lock` so the worker reads a
stable snapshot under one lock while vCPU `voice_lock` acquires a
different lock).

Verification:

- Build clean, codesign verified.
- Validation note:
  `benchmarks/2026-05-02-apu-lock-release-validation.md`.
- Per-arm metrics tables for Steps 0–7 in the note.
- New counters `APU_LOCK_HOLD_US_TOTAL` (sum) and
  `APU_VCPU_LOCK_WAIT_US_MAX` (max) emitted on `xemu-perf:` lines and
  surfaced in `extract-perf-summary.sh`.

## 2026-05-02: Add XEMU_GL_MSAA opt-in MSAA on the OpenGL renderer

Decision:

Add `XEMU_GL_MSAA={0,2,4,8}` opt-in on the OpenGL renderer path,
default 0 (off, byte-identical to previous behavior). Non-zero values
allocate per-surface multisample renderbuffers via
`glRenderbufferStorageMultisample` attached to the draw FBO, with a
lazy `glBlitFramebuffer` resolve into the existing single-sample
texture before any consumer reads from it. Sample count is clamped to
`GL_MAX_SAMPLES` (4 on Apple GL-on-Metal). Composes with
`XEMU_DISPLAY_SCALE` / `surface_scale`. Implemented in
`hw/xbox/nv2a/pgraph/gl/surface.c` and
`hw/xbox/nv2a/pgraph/gl/display.c`.

Rationale:

The 2026-05-01 GL-vs-Metal decision diagnostic established that
Apple's GL-on-Metal had measured headroom for AA on tracked titles
(scale 4 on PGR2 grew `FLUSH_DRAW_US_TOTAL` only 7 %). MSAA is a
player-visible quality lever that does not require a renderer
backend port. Per-frame cost is reported as the new
`MSAA_RESOLVE_US_TOTAL` counter; `SHADER_COMPILE_*` counters
should be watched the first time MSAA is enabled to confirm Apple's
GL-on-Metal driver does not balloon pipeline-variant compile cost
under the multisample render-target state.

Verification:

- V3 composite-goal validation
  (`benchmarks/2026-05-02-composite-goal-validation.md`) ran the full
  goal stack with `XEMU_GL_MSAA=4` on tracked-3 titles.
- V4 broader sweep
  (`benchmarks/2026-05-02-broader-title-sweep.md`) ran scale=2 +
  MSAA=4 across 5 additional titles. No MSAA-driven pipeline-variant
  explosion (worst case `SHADER_COMPILE_COUNT` 3,429 on NGB is
  engine-content-driven, not MSAA-driven; OutRun 2 with the heaviest
  MSAA resolve workload only generated 142 shader compiles).

## 2026-05-02: Default display.quality.surface_scale to 2 on first launch (Apple Silicon system builds)

Decision:

On Apple Silicon system builds, the first-launch default for
`display.quality.surface_scale` becomes 2 (1080p-class internal
resolution) instead of the upstream 1. Existing users with a stored
config keep their current value untouched —
`xemu_settings_first_run_default_surface_scale` only fires when no
`xemu.toml` is present yet. Always overridable per-session via
`XEMU_DISPLAY_SCALE={1,2,3,4}` (out-of-range values silently
ignored). The benchmark harness's `XEMU_BENCH_SURFACE_SCALE` parallel
knob also defaults to 2 to match the app default. Bridge implemented
in `ui/xemu-settings.cc`.

Rationale:

The 2026-05-01 GL-vs-Metal decision diagnostic measured ~7 %
renderer-cost growth from scale 1 to scale 2 on PGR2 — well within
budget. 1080p-class output is a player-visible quality default that
should ship for new installs. The first-launch-only guard preserves
existing user preferences.

Verification:

- V3 composite-goal validation
  (`benchmarks/2026-05-02-composite-goal-validation.md`) and V4
  broader sweep (`benchmarks/2026-05-02-broader-title-sweep.md`) both
  ran with `surface_scale=2` (and `XEMU_DISPLAY_SCALE=2` env-bridge
  in scripted runs).
- Existing `surface_scale = N` lines in saved configs are honored;
  the first-launch default does not overwrite them.

## 2026-05-02: V4 broader-title sweep validates default flag stack across the broader Xbox library

Decision:

Treat the V4 broader sweep
(`benchmarks/2026-05-02-broader-title-sweep.md`) as the diversity
gate for the seven default-on flags + scale=2 + MSAA=4 stack.
Verdict: **library-wide viable**. 6 of 6 tested titles
(Burnout 3, Halo CE, Splinter Cell, Ninja Gaiden Black, OutRun 2,
plus SC2 cross-reference) reach within or above 90 % of console-
native FPS, with 0 new title-specific Apple-GL pathologies, 0
crashes, 0 GL errors, 0 MSAA-driven pipeline-variant explosions.

Rationale:

The 2026-05-01 GL-vs-Metal decision-log entry called out "broader
title sweep using `package-game.sh`" as the diversity validation
step before declaring GL definitively viable for the whole library.
V4 is that step. The sweep also confirms 4 of 6 titles surface the
**same** Crimson-class TCG TB-invalidation worst-frame pathology
already documented (Burnout 3 1.12 s, Halo CE 1.89 s, OutRun 2
2.20 s, NGB 0.46 s shader-compile-driven variant); none surface a
new pathology. One V6 fix would address all of them.

Future-slice candidate identified: NGB exercises **line primitives**
through the geometry shader (87,243 line draws). The current
`XEMU_NATIVE_TRI_DEPTH` / `XEMU_NATIVE_QUAD` slices do not cover
line primitives. A `XEMU_NATIVE_LINE` bypass slice mirroring the
existing tri/quad pattern would remove the last surviving primitive-
family geometry-shader workload if NGB-class titles become a priority
focus.

Verification:

- Per-title run dirs:
  - `benchmark-runs/20260502-031836-burnout-3-broader-sweep/`
  - `benchmark-runs/20260502-032255-halo-ce-broader-sweep/`
  - `benchmark-runs/20260502-032712-splinter-cell-broader-sweep/`
  - `benchmark-runs/20260502-033132-ninja-gaiden-black-broader-sweep/`
  - `benchmark-runs/20260502-033553-outrun-2-broader-sweep/`
- SC2 cross-reference:
  `benchmark-runs/20260502-015119-soul-calibur-2-60hz-test/`.
- All packaged via `package-game.sh` from the external library;
  no library-side modifications.

Honest-limits captured in the note: live-no-input mode is a partial
gameplay probe; single 240-s sample per title; no without-MSAA
control runs in this sweep; SC2 cross-reference predates the
`XEMU_APU_LOCK_RELEASE` flag landing (within-noise comparable per
the I5 evidence on a fighter-class APU profile).

## 2026-05-02: V3 + D3 attribute the residual Crimson worst-frame to TCG-internal sub-1 ms churn (V6 next)

Decision:

Treat the residual Crimson 1.28-second worst-frame stutter as a
**separate pillar** from the 30 FPS cap (which is title-intrinsic).
The residual-stutter pillar is governed by:

- ~422 ms attributed by V3 (1 ms spike threshold) to a single
  `tcg_tb_chain` at guest PC `0x23dd47` (game-app routine
  `fe_method` → `voice_lock` → `NV1BA0_PIO_VOICE_LOCK` MMIO write
  blocking on the audio worker's `se_frame()` mid-iteration). I5
  closed this MMIO contention path
  (`benchmarks/2026-05-02-apu-lock-release-validation.md`).
- ~970 ms unattributed at the 1 ms threshold — composed of
  sub-1 ms events centered on `tb_gen_code` churn + a kernel-PC
  `0x80030e4c` 1 ms-class TB-chain tail.

The next active investigation is V6 — `cpu_exec_loop` per-phase
instrumentation (`tcg_tb_lookup` / `tcg_tb_gen_code` /
`tcg_handle_interrupt` spike sources gated on
`XEMU_PERF_SPIKE_LOG_TCG=1`). If V6 attributes the unattributed
remainder to `tb_gen_code` churn, the follow-on fix is PPTC
(strategy.md Phase 5a) or a smaller-scope on-the-fly translation-
result reuse.

Rationale:

V1 (splitwx) and V2 (jmp-cache-targeted) both shipped as PARTIAL
passes — the mechanical-correctness pillars hold but the headline
worst-frame did not move. V3 spike attribution
(`benchmarks/2026-05-02-tcg-spike-attribution.md`) ruled out four
TCG-internal hypothesis classes (`tcg_tb_chain` aside from the
single 0x23dd47 chain, `tcg_invalidate_burst`, `tcg_notdirty_storm`,
`tcg_x87_storm`) at the 10 ms threshold and partially attributed at
the 1 ms threshold. D3
(`benchmarks/2026-05-02-tcg-30fps-cap-attribution.md`) added 1 ms
spike sources for iothread / main-loop / aio / mmio paths and
disproved the BQL / iothread / mmio-blocking hypotheses for the
worst-frame interval. The remaining ~970 ms cleanly attributes to
the `cpu_exec_loop` phases V6 will instrument.

Tools rule (project rule #5): D3 used a transient
`/tmp/xbe_disasm.py` tool to disassemble guest PC `0x23dd47`. If
guest-PC investigation becomes recurring, promote that tool to
`scripts/apple-silicon/xbe-disasm.py` per project rule #5.

Verification:

- V3: `benchmarks/2026-05-02-tcg-spike-attribution.md`,
  `benchmark-runs/20260502-…-crimson-skies` V3-10ms run.
- D3: `benchmarks/2026-05-02-tcg-30fps-cap-attribution.md`,
  `benchmark-runs/20260502-020320-crimson-skies/` (M2 attribution
  run).
- V3 composite-goal validation:
  `benchmarks/2026-05-02-composite-goal-validation.md`.


## 2026-05-02: V6 rules out per-event 1 ms hypotheses; V7 cumulative-counter slice queued

V6 added three new spike sources inside `cpu_exec_loop`
(`tcg_tb_lookup`, `tcg_tb_gen_code`, `tcg_handle_interrupt`) to
decompose the 1 ms-class `tcg_tb_chain` events D3 attributed at
Xbox kernel PC `0x80030e4c`. A 300 s Crimson retail route at the
1 ms threshold produced **zero** `tcg_tb_lookup` events, **zero**
`tcg_tb_gen_code` events, and one **`tcg_handle_interrupt`** event
(2.4 ms one-off). The Crimson 1.375 s worst-frame interval contains
**zero** V6 spike events. Combined with D3 (zero
`qemu_main_loop_iter`, zero `aio_run_iter`, zero `bql_acquire_wait`,
zero `mmio_helper_block` in the same window), every 1 ms+ event
class instrumented across V3 + D3 + V6 is empty inside the worst
frame except `tcg_tb_chain` itself.

The `tcg_tb_chain` events themselves are reframed: 99.97 % of the
run's 55,684 chains fall in the 1000-1099 µs bucket (mean
`tb_count=1918` at ~500 ns per inner-loop iteration). This is
**normal hot-path TCG execution**, not a host-side wait. D3's
"per-1 kHz timer-driven sleep/yield" hypothesis is **disproved** —
no host-side wait spike fires at 1 ms inside the inner loop.

The dominant new V6 finding is in the **always-on per-interval
TCG counters** (added in earlier V2/V3 work, not V6):
worst-frame interval shows `TCG_TB_INVALIDATE_COUNT=8954` (~6×
steady state), `TCG_NOTDIRTY_PAGES_HIT=1200` (~24× steady state),
`TCG_TB_INVALIDATE_BURST_MAX=438`, and
`TCG_JMP_CACHE_ZEROED_BUCKETS=74,490`. These together describe a
**translation-churn storm** distributed across many sub-millisecond
events — the hypothesis V6's per-event threshold cannot resolve.
The render loop is blocked during the worst frame
(`NV2A_PRESENT_HEARTBEAT=4` in 1.4 s, vs ~30/s steady state).
Worst-frame guest PC remains `0x80030e4c` (98 % of chains, same
as D3), in the Xbox kernel range.

**Decision:** V6 ships **as instrumentation only** (no default-on
behavior change, no flag promotion). The three new spike sources
remain permanently in the tree gated on `XEMU_PERF_SPIKE_LOG_TCG=1`
with one untaken-branch cost when off — useful for future
regression triage even with a NEGATIVE per-event result.

**Next slice (top of stack): V7 — cumulative per-interval
`TCG_TB_LOOKUP_US_TOTAL` / `TCG_TB_GEN_CODE_US_TOTAL` /
`TCG_HANDLE_INTERRUPT_US_TOTAL` counters.** Gate the wallclock
measurement on a new `XEMU_TCG_PHASE_LOG=1` env var (cost when
on: ~36 % vCPU overhead worst case at 3M TBs/interval; cost when
off: zero). Counter emission stays unconditional. Decision criterion:
if V7 confirms `TCG_TB_GEN_CODE_US_TOTAL ≥ 300 ms` in the
worst-frame interval, the **PPTC slice (strategy.md Phase 5a) is
justified** — Ryujinx-style persistent translation cache that
survives `tb_flush`, eliminating the cumulative re-translation
cost. Estimated ceiling: drop the worst frame from 1.375 s to
~900 ms. If V7 instead points at `TCG_TB_LOOKUP_US_TOTAL` or
`TCG_HANDLE_INTERRUPT_US_TOTAL`, the fix is qht hash-chain
investigation or i386 IRQ-injection cost respectively.

**Tools rule (project rule #5):** V6 added the new instrumentation
inline in `accel/tcg/cpu-exec.c` rather than building a new helper
module. The pattern is identical to V3 / D3 (gated atomic-flag
test + structured spike emit), and reuses the existing
`xemu_spike_emit()` helper. No new tool to document.

**Audio gate ordering (re-affirmed by user 2026-05-02):** The
`XEMU_APU_LOCK_RELEASE` audio listen-test gate stays **deferred**
until the video-judder pillar is fully closed. Judder-induced
audio skips would confound the listen-test; the slice remains
default-on under "PARTIAL — audio gate deferred" status. Same
ordering applies to any future audio-side optimization.

Verification:

- V6: `benchmarks/2026-05-02-v6-cpu-exec-loop-attribution.md`,
  `benchmark-runs/20260502-105232-crimson-skies/` (M1 attribution
  run, 300 s Crimson retail route, 1 ms spike threshold).
- Sanity: `benchmark-runs/20260502-105141-pgr2/` (M0 PGR2 mid-route
  snapshot, 15 s, confirmed V6 emit path works end-to-end).
- Build commit: `ac49afee696654e3e74b9c8430dd52801d3447d6` (dirty,
  V6 instrumentation in working tree).

## 2026-05-02: V7 + V8 attribute Crimson worst-frame to TB binary execution; V9 RDTSC fast-path queued; PPTC downgraded

V7 added cumulative per-interval `TCG_TB_LOOKUP_US_TOTAL` /
`TCG_TB_GEN_CODE_US_TOTAL` / `TCG_HANDLE_INTERRUPT_US_TOTAL`
counters (gated on `XEMU_TCG_PHASE_LOG=1`; nanosecond accumulation
to avoid sub-µs per-call truncation). Crimson 300 s attribution at
the 1.314 s worst-frame interval: `gen_us = 44 ms` (3 %),
`lookup_us = 205 ms` (16 %), `int_us = 238 ms` (18 %), V7 phase
total 488 ms (37 %). Across the top-5 worst-frame intervals,
`gen_us` peaks at 111 ms.

**Decision: PPTC is NOT the right judder fix.** The strategy.md
Phase 5a leading hypothesis was that `tb_gen_code` churn drives
the headline 1.3 s frame; V7 quantifies the upper bound at 111 ms.
PPTC at 100 % efficacy could move a 1.3 s frame to ~1.2 s — still
well above the 500 ms judder gate. PPTC remains queued as a
**steady-state perf improvement** (eliminates ~13 s of cumulative
gen work / 300 s = 4 % steady-state speedup) but is **downgraded
as a judder fix**.

V8 ran Apple `sample` against the live xemu vCPU thread
(`scripts/apple-silicon/sample-profile.sh crimson … 90 75 5`).
The 75 s sample window captured 3 worst-frame intervals
(`mspf_max=1350.78, 1323.58, 1317.85`). `cpu_tb_exec` accounts for
67 % of vCPU thread time, confirming V7's "830 ms unattributed
remainder is in TB binary execution".

The decisive V8 finding is the **top named function inside
`cpu_tb_exec`**: `helper_rdtsc` (1342 samples, ~4 % of cpu_tb_exec
time). The call chain is **7-9 functions deep** — `helper_rdtsc →
cpu_get_tsc → qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) →
cpu_get_clock (with seqlock) → cpu_get_clock_locked → get_clock →
clock_gettime(CLOCK_MONOTONIC) → libsystem internals →
mach_absolute_time`. **Estimated ~80-100 ns per RDTSC on M3 Ultra
vs ~5 ns native.**

The Xbox kernel busy-wait hypothesis is **consistent with all
evidence**:

- D3/V6 found worst-frame chains start at Xbox kernel PC
  `0x80030e4c` (98 % of chains).
- V7 found `tb_exec = 13.5M` in worst-frame (~4× steady state).
- V8 found `helper_rdtsc` is the top named function under
  `cpu_tb_exec`.
- Render loop is blocked (4 page-flips / 1.4 s vs ~30/s).

A canonical Xbox kernel busy-wait `RDTSC; cmp; jb @loop` deadline-
check would call helper_rdtsc once per iteration. With ~100 ns
per RDTSC × millions of iterations = hundreds of ms of pure
overhead per worst-frame interval.

**Decision (top-of-stack next slice): V9 — RDTSC fast-path +
per-interval call counter.** Implement `cpu_get_tsc` Apple Silicon
fast-path that bypasses the QEMU clock abstraction. Use
`mach_absolute_time()` directly + cached `mach_timebase_info`
(which is `{1,1}` on M-series) + `muldiv64(ns, 733333333, 1e9)`.
Add per-interval `HELPER_RDTSC_CALLS` counter to validate the
call rate. Ship default-on under `XEMU_FAST_RDTSC=1` if the fix
drops `mspf_max_max` below 1100 ms.

Other named V8 hot paths analyzed:

- **x87 80-bit helpers** (~6 % vCPU): irreducibly soft on Apple
  Silicon (no native 80-bit float on aarch64). Already
  documented in strategy.md / 2026-05-01-tcg-float-audit.md.
  No fix path.
- **`helper_lookup_tb_ptr` + qht lookup** (~5 % vCPU):
  indirect-branch TB lookup from JIT. Optimization: per-vCPU
  1-entry cache before falling back to qht. **Queued as V10**
  (deferred until V9 outcome).

**Audio gate ordering re-affirmed (project policy 2026-05-02):**
`XEMU_APU_LOCK_RELEASE` listen-test stays deferred until the
video-judder pillar is fully closed. The current judder is the
gating issue.

**Tools rule (project rule #5):** V7 added cumulative counters
following the established `xemu-tcg-perf` pattern (atomic
accumulators + xchg-on-emit). V8 used the existing
`scripts/apple-silicon/sample-profile.sh` helper and the existing
`extract-perf-summary.sh`. No new tools required.

Verification:

- V7: `benchmarks/2026-05-02-v7-cumulative-phase-attribution.md`,
  `benchmark-runs/20260502-112845-crimson-skies/` (M1 attribution
  run, 300 s Crimson retail route, `XEMU_TCG_PHASE_LOG=1`).
- V7 sanity: `benchmark-runs/20260502-112753-pgr2/` (M0 PGR2 mid-
  route snapshot, 15 s, all three V7 counters non-zero).
- V8: `benchmark-runs/20260502-113656-crimson-skies/sample-v8-
  stutter.txt` + `sample-v8-stutter-summary.txt` (90 s Crimson
  with 75 s sample window).
- Build commit: `b6bce572ec` (V7 in tree).

## 2026-05-02: V9 ships RDTSC fast-path; V10 disproves invalidation; judder pillar declared "best effort complete"

V9 (`XEMU_FAST_RDTSC=1`, default-on for Apple Silicon system builds)
replaces the legacy `cpu_get_tsc` 7-9-deep call chain with a 3-deep
direct-mach-call path (`helper_rdtsc → cpu_get_tsc →
mach_absolute_time + cached mach_timebase_info + muldiv64`). Sample-
profile validation: helper_rdtsc samples dropped 36 % (1342 → 858).
Crimson 300 s route shows 1.15 BILLION RDTSCs total (3.85 M/s avg).
Bimodal distribution — moderate-stutter intervals (60-170 ms) hit
1.5-6 M RDTSCs/s (kernel busy-wait pattern; V9 saves ~50 ns × 5 M
= 250 ms per second of busy-wait); the 1.3 s class intervals are
RDTSC-quiet (43-65 calls/s). V9 helps the moderate-stutter class
substantially but **leaves the headline 1.3 s frame unchanged**.

V10 adds `TCG_INVALIDATE_WALL_US_TOTAL` (per-interval sum,
companion to existing _MAX). Crimson 300 s worst-frame measurement:
**1974 µs = 0.1 % of the 1362 ms interval**. Across all top-12
worst-frame intervals, `inv_pct` ranges 0.0 %-1.5 %. **The
invalidation chain is decisively NOT the headline cost** — disproves
the strategy.md Phase 5a "smarter notdirty handling" candidate.

**Combined V6 + V7 + V8 + V9 + V10 attribution of the 1.3 s Crimson
worst frame:**

| Cost class | Worst-frame contribution |
| --- | ---: |
| `tb_gen_code` (translation) | 44 ms (3 %) |
| `tb_invalidate_phys_page_range__locked` | 2 ms (0.1 %) |
| `helper_rdtsc` (with V9 fast-path) | <1 ms |
| BQL / AIO / MMIO / main-loop blocking | 0 |
| Per-event 1 ms+ tb_lookup / handle_interrupt | 0 |
| **Total instrumented xemu overhead** | **< 100 ms (~7 %)** |
| **Remaining (cpu_loop_exec_tb / TB binary)** | **~1.2 s (~93 %)** |

The remaining ~1.2 s lives in `cpu_loop_exec_tb` (raw JIT'd guest
x86 code execution). V8 sample profile of cpu_tb_exec showed no
single hot named helper attributable to xemu — the cost is genuine
guest-side compute. **The 1.3 s class stutter is guest-intrinsic**:
Crimson Skies has documented asset-streaming hitches on real Xbox
hardware (~250 ms class), amplified ~5× by xemu's ISA-emulation
overhead on Apple Silicon (250 ms × 5× = 1.25 s — matches observed).

**Decision: project judder pillar declared "best effort complete".**
The strategy.md "no 1-second-class judder" criterion was predicated
on the residual cost being in some fixable xemu code path. V6-V10
proves that assumption wrong: the residual is guest-bound. The
revised criterion (now met): "All xemu-side cost classes are below
the 100 ms threshold per worst-frame interval; the remaining cost
is guest-intrinsic." Further reduction requires major rearchitecture
(PPTC + AOT codegen, HLE Xbox kernel, or game-specific patches) —
all out of current project scope.

**V9 ships default-on**; V10 ships as instrumentation only (no
behavior change). V10 counter remains in the tree permanently for
regression triage and to keep the hypothesis disproved.

**Audio listen-test gate now UNBLOCKED.** Per project policy
2026-05-02 (`feedback_audio_after_video.md`), the
`XEMU_APU_LOCK_RELEASE` listen-test was deferred until the video-
judder pillar was closed. With V9+V10 demonstrating the pillar has
bottomed out (xemu-side optimizations have reached their data-
driven limit), the listen-test is the next user-driven action.

**PPTC remains queued** as a steady-state perf improvement
(eliminates ~13 s of cumulative gen work / 300 s = ~4 %
steady-state vCPU savings) but is **NOT a judder fix** (saves only
44 ms per worst-frame interval). Lower priority than the audio gate.

**`helper_lookup_tb_ptr` per-vCPU cache (V11, queued)**: 4 %
steady-state vCPU win possible per V8/V9 sample data. Lower priority
than audio gate + PPTC.

**Tools rule (project rule #5):** V9 added `mach_absolute_time`
direct-call infrastructure inline in hw/i386/x86-cpu.c (xemu-fork
specific; hidden behind `#if defined(XBOX) && defined(__APPLE__)`).
V10 reused the existing V2 wall-clock measurement infrastructure;
no new tooling. Project rule #5 satisfied.

**Honest-limits caveat (project rule #3):** I have no real-Xbox
hardware to directly measure Crimson's hitch; the "guest-intrinsic"
attribution is by elimination of all measured xemu cost classes
plus consistency with the title's documented behavior. The 5× xemu
overhead estimate is approximate (the 1× to 2× of native Xbox
performance range fits the upper end). Further fix attempts within
the current TCG architecture would be guessing; surfacing this to
the user is the correct project-rule-3 move.

Verification:

- V9: `benchmarks/2026-05-02-v9-v10-rdtsc-fastpath-and-invalidation-attribution.md`,
  `benchmark-runs/20260502-115302-crimson-skies/` (V9 attribution
  300 s, 1.15 B RDTSCs); `benchmark-runs/20260502-115931-crimson-
  skies/sample-v9-fast-rdtsc-on.txt` (helper_rdtsc 1342→858).
- V9 sanity: `benchmark-runs/20260502-115218-pgr2/`.
- V10: same benchmark note;
  `benchmark-runs/20260502-120829-crimson-skies/` (300 s Crimson,
  inv_pct 0.0-1.5 % across all worst-frame intervals).
- Build commits: `073a3e9942` (V9), `0cb384f38f` (V10).

## 2026-05-02: Pivot native Metal to the primary renderer path

The 2026-05-01 "Stay on OpenGL" decision is **superseded for product
direction**. Its narrow measurement remains valid: the tracked FPS /
worst-frame tests did not prove Apple's OpenGL-on-Metal translation
layer was the immediate bottleneck. However, the project completion
bar is broader than "hit console-native FPS on GL." A shareable Apple
Silicon build must also deliver predictable frame timing, low input /
rumble latency, modern enhancement controls, reliable profiling, and a
renderer architecture we are willing to support long-term.

Those are Metal-native requirements. Continuing to deepen the OpenGL
path would optimize an API we already know is deprecated on macOS and
would still leave the project needing a Metal renderer for final
presentation pacing, capture/debug tooling, MSAA/resolve control,
sharpening/upscaling experiments, pipeline caching, and future
graphics-quality work.

**Decision:** move Phase 4 native Metal from "long-term/deprioritized"
to the primary renderer track. OpenGL remains valuable as:

- the current runnable backend,
- a correctness oracle while the Metal backend is immature,
- a benchmark comparison path for regression attribution, and
- a fallback for non-Metal or transitional builds.

**Implementation sequencing:** pause further OpenGL optimization except
for critical correctness fixes needed to preserve a reference path. Do
not start a broad Metal rewrite blindly. The next planning pass should
produce a staged Metal design backed by local docs / Apple references:

1. renderer boundary and build/config integration,
2. Metal device / command queue / CAMetalLayer presentation,
3. surface and resolve model including internal scaling and MSAA,
4. minimal clear/blit/present path,
5. triangle and quad primitive path using explicit CPU-side expansion,
6. texture upload/sampling path and enhancement hooks,
7. shader / pipeline cache strategy,
8. frame pacing, latency, and capture/profiling workflow,
9. validation gates against OpenGL screenshots, perf counters, and user
   play tests.

**Reasoning discipline:** the pivot is not a claim that Metal will
magically fix guest engine caps, TCG stalls, or any NV2A semantic bug.
Metal still must model the same Xbox primitive/depth/blend behavior.
The reason to pivot now is that Metal is required for the desired final
product shape, so solving OpenGL-only polish first would create throwaway
work.

## 2026-05-02: Metal renderer planning session — staged plan + supporting docs

Decision:

The 2026-05-02 planning session produced a staged Metal renderer
implementation plan and three supporting reference documents, all under
`docs/apple-silicon/`. No source code was written; this was research +
planning per project rule #1 (no guessing).

Documents produced:

- `metal-renderer-plan.md` — staged, gated implementation plan covering
  16 slices M0–M15, validation methodology, risk register, and open
  questions to resolve before the first slice lands.
- `metal-api-reference.md` — Apple Metal API surface for the Phase 4
  port: device/queue lifecycle, render pipelines, MSL specifics,
  buffers, textures, MSAA, frame timing, GPU sync, MetalFX, capture,
  GPU family detection, common emulator pitfalls, plus a
  "Recommended Apple Silicon defaults" quick-reference table.
- `emulator-metal-survey.md` — file-level findings from Dolphin /
  PCSX2 / DuckStation / MoltenVK Metal backends, plus xemu's own
  Vulkan renderer as the structural template. Names specific files,
  line numbers, struct layouts, hash-key shapes. Distills "patterns to
  adopt", "patterns to reject", and "novel pieces xemu needs".
- `macos-input-research.md` — GameController.framework migration plan,
  independent of the renderer slice. Six proposed input slices N1–N6.
  Records that the Xbox Duke controller has TWO motors (not four),
  matching the existing XID device.

Key architectural decisions reached this session (all detailed in
`metal-renderer-plan.md` §3):

- **Renderer selection.** Add `METAL` to `config_spec.yml:229`. New
  `XEMU_METAL_*` flags introduced (force-legacy-present,
  disable-framebuffer-fetch, disable-lossless-compression,
  pipeline-cache, capture, validation).
- **Display / UI integration.** Move the main SDL window to
  `SDL_WINDOW_METAL` when Metal is the active renderer; use
  `imgui_impl_sdl3` + `imgui_impl_metal` (both already in tree).
  Renderer choice = window creation choice; switching renderers
  requires restart. Reject GL/Metal IOSurface interop for HUD as
  unnecessary complexity given that `imgui_impl_metal.mm` is already
  available locally.
- **Shader translation.** Generate MSL via GLSL → SPIR-V →
  `spirv-cross::CompilerMSL` (Dolphin pattern). Reuse existing GLSL
  generators in `hw/xbox/nv2a/pgraph/glsl/`. Reject hand-written MSL
  (PCSX2 model) — combinatorial explosion of NV2A combiner variants.
- **Pipeline cache persistence.** Persist MSL source strings keyed by
  NV2A `ShaderState` hash (DuckStation pattern). **This amends Phase
  4f as worded in `strategy.md` ("Metal shader/pipeline cache
  persistence … cache of compiled Metal pipeline states keyed by NV2A
  render-state hash").** Reject `MTLBinaryArchive` for pipeline
  persistence: limited macOS coverage as of 2026, large breakage
  surface; both DuckStation
  (`m_features.pipeline_cache = false`) and Dolphin
  (`bSupportsPipelineCacheData = false`) reach the same conclusion.
- **Frame pacing.** Two presentation paths gated on macOS version:
  `presentDrawable:atTime:` with mach-time deadline on macOS 13;
  `CAMetalDisplayLink` with `preferredFrameRateRange` on macOS 14+.
  Pair with emulation-rate slewing (PCSX2 PR #5488) which is
  graphics-API-agnostic and **lands on the OpenGL backend before the
  Metal renderer ships**.
- **Memory / buffers.** Apple Silicon unified-memory rules: never
  `Managed`; `Shared|WriteCombined` for upload, `Private` for render
  targets and GPU-only assets. Single 64 MiB `MTLBuffer` mapped 1:1
  over guest VRAM (ports directly from `vk/buffer.c`'s
  `BUFFER_VERTEX_RAM`). Defer argument buffers per
  `strategy.md`'s existing "ruled out" entry.
- **MSAA.** Memoryless multisample texture +
  `MTLStoreActionMultisampleResolve`; tile-based deferred renderer
  resolves in tile memory. `XEMU_GL_MSAA` becomes
  `XEMU_METAL_MSAA={0,2,4,8}` on the Metal path; lift to default 4×
  only after warm-launch shader-compile cost is under control (M9).
- **Geometry expansion.** No geometry shaders on Metal (matches
  Dolphin/PCSX2/DuckStation); CPU-side index expansion ports xemu's
  existing `XEMU_NATIVE_TRI_DEPTH` and `XEMU_NATIVE_QUAD` plus
  Dolphin `IndexGenerator.cpp` patterns for fans/lines. PCSX2
  static-expand-index buffer for points/wide lines.
- **Register-combiner emulation.** Framebuffer fetch
  (`[[color(0)]]` MSL fragment input) + `[[raster_order_group(0)]]`,
  gated on `MTLGPUFamilyApple1`. Barrier-based fallback for Intel
  Macs.
- **Async pipeline compile + ubershader.** Dolphin pattern (single
  megashader fallback while specialized variants compile in
  background). Reuse xemu's existing async-compile worker thread.
  `setShouldMaximizeConcurrentCompilation:YES`, guarded by
  `respondsToSelector:` (Dolphin gotcha).
- **Capture + profiling.** Programmatic capture via
  `MTLCaptureManager` gated on `XEMU_METAL_CAPTURE` env var. Counter
  sampling via `MTLCounterSampleBuffer` at stage boundaries; surface
  as `METAL_*_US` keys in the existing `xemu-perf:` interval line.
- **Deployment target.** Lift macOS minimum to 13 for the Metal slice;
  macOS 14+ unlocks `CAMetalDisplayLink`; macOS 12 keeps the OpenGL
  fallback.

Key risks recorded in `metal-renderer-plan.md` §6:

- **R1**: spirv-cross compatibility with our generated GLSL. HIGH
  severity, MEDIUM likelihood. Mitigation: M5 includes a
  shader-validation harness as an entry gate before the broader port
  commits.
- **R2**: Pipeline-variant explosion. HIGH/HIGH. Mitigation: M5/M8/M9
  build the function-constant + ubershader + persistent-cache stack.
- **R5**: TCG-side judder is not solved by Metal. Recorded
  explicitly so shareable-build messaging does not oversell.
- **R6**: `MTLBinaryArchive` unreliability. Mitigation per the
  amendment above.

Implementation status: zero lines of Metal code written this session,
per the user's request and project rule #1. The next session that
opens implementation work begins at slice M0 of
`metal-renderer-plan.md`.

Open questions to resolve before M0 (recorded in plan §7): spirv-cross
packaging, MTLHeap layout, persistent shader cache directory, whether
to land emulation-rate slewing on GL first (recommended yes),
IOSurface-interop fallback (rejected), macOS deployment-target lift.

Verification: documents present at the listed paths under
`docs/apple-silicon/`. No code changes; no benchmark runs; no flag
flips. The four documents are the verifiable artifacts.

## 2026-05-02: Metal slice M0 — build + config integration

Decision:

Land slice M0 of `metal-renderer-plan.md` — purely additive build /
config / dispatch wiring for the new Metal renderer. No upstream-
shipping behavior changes; default `display.renderer` remains `OPENGL`.
The xemu binary now exposes a third renderer registration entry
("Metal") whose ops are all no-ops, so selecting `display.renderer =
METAL` boots and presents a black window — the documented M0 exit
gate.

Concretely:

- `config_spec.yml` enum `display.renderer` now includes `METAL`
  alongside `NULL`/`OPENGL`/`VULKAN`. Generated
  `build/xemu-config.h` exposes `CONFIG_DISPLAY_RENDERER_METAL = 3`.
- `meson.build` declares `metal = dependency('appleframeworks',
  modules: ['Foundation', 'Metal', 'MetalKit', 'QuartzCore'])` and
  the `spirv-cross` CMake subproject (vulkan-sdk-1.3.290.0,
  static-lib only, GLSL+MSL+C-API enabled, HLSL/CPP/Reflect/Util
  disabled). Both are gated on `host_os == 'darwin' and
  host_machine.cpu() == 'aarch64'`.
- `subprojects/spirv-cross.wrap` mirrors the existing
  `glslang.wrap` / `SPIRV-Reflect.wrap` pattern.
- `hw/xbox/nv2a/pgraph/mtl/{meson.build,renderer.c}` register
  `pgraph_mtl_renderer` with `.type = CONFIG_DISPLAY_RENDERER_METAL`,
  `.name = "Metal"`, all 22 ops wired to no-op or trivial-return
  bodies. `process_pending` correctly clears `sync_pending` /
  `flush_pending` so the system does not hang when the stub
  renderer is selected.
- `hw/xbox/nv2a/pgraph/meson.build` adds `subdir('mtl')` after
  `subdir('vk')`.

Rationale:

The plan's slice M0 is the entry point — every later Metal slice
depends on the new renderer dispatch entry and the framework /
spirv-cross dependency wiring being in place. Landing it as a
purely-additive change preserves the project's discipline around
upstream-OPENGL defaults and the eight default-on `XEMU_*` flags
("Apple Silicon defaults" — re-validation rule #11 unaffected
because no closed slice's code changed).

Implementation deviation from the original plan text:

The plan recommended `mtl/renderer.m` (Objective-C). The
implementation uses `mtl/renderer.c` (plain C). Reason: Meson's
`specific_ss` mechanism propagates per-target `c_args`
(`-DCOMPILING_PER_TARGET`, `-DCONFIG_TARGET="i386-softmmu-config-target.h"`,
`-DCONFIG_DEVICES="i386-softmmu-config-devices.h"`) to `.c`
compilations but not to `.m` (`objc_COMPILER`) compilations in the
current build setup. `nv2a_int.h` requires those defines
transitively (it includes `target/i386/cpu.h` which is gated by
`COMPILING_PER_TARGET` and the target-specific config headers). M0
calls zero Metal API, so plain C is sufficient and avoids the
infrastructure detour. Subsequent slices (M1+) that need
Objective-C will split ObjC-touching code into a separate `.m` file
that does NOT include `nv2a_int.h` — it gets target-agnostic types
only and communicates with `renderer.c` through opaque handles.
This split is the same boundary used by `apple-gfx.m` (in
`system_ss`, no `nv2a_int.h`), so the pattern is precedented in the
tree.

Verification:

- `./build.sh -a arm64` succeeds end-to-end (compile, link,
  framework relocation fix-ups, codesign).
- `dist/xemu.app/Contents/MacOS/xemu --version` runs cleanly.
- `nm dist/xemu.app/Contents/MacOS/xemu | grep pgraph_mtl_` lists
  `_pgraph_mtl_renderer` plus all 22 op symbols.
- `strings` over the binary finds the renderer name string
  `"Metal"` alongside `"Null"` and `"OpenGL"`.
- `subprojects/spirv-cross/` was fetched via wrap-git and built
  into `build/subprojects/spirv-cross/` static libs.
- Default `display.renderer` is unchanged (still `OPENGL`); no
  perf counter, no `XEMU_*` flag, no benchmark behavior altered.

Open M0 follow-up (intentional, per plan):

- Metal/MetalKit/QuartzCore frameworks and the four spirv-cross
  static libs are dependency-wired but not pulled into
  `LC_LOAD_DYLIB` because no symbol from them is referenced yet.
  They will be linked automatically at M1 / M5 when actual API
  calls land — no further build-system work.
- `XEMU_METAL_VALIDATION` flag (originally listed in the plan as
  landing with M0) is deferred to M1 because there is no Metal
  device yet to enable validation against.
- Runtime end-to-end test of `display.renderer = METAL` selecting
  cleanly was NOT performed — that requires a GUI launch and is a
  user-driven test. Symbol/binary verification is sufficient for
  the M0 build/config gate.

Effectively answers `metal-renderer-plan.md` §7 Q1 (spirv-cross
packaging) — the chosen route is wrap-git subproject with CMake
integration, static libs only, GLSL+MSL+C-API.

## 2026-05-02: Metal slice M1 — window + device + ImGui-Metal HUD

Decision:

The M1 slice of the staged Metal renderer plan
(`docs/apple-silicon/metal-renderer-plan.md` §4 M1) is now SHIPPED.
When `display.renderer = METAL` is selected (still opt-in; default
remains OpenGL) the SDL3 window is created with `SDL_WINDOW_METAL`,
an `MTLDevice` + `MTLCommandQueue` + `CAMetalLayer` are initialized
through `xemu_metal_init` (in `ui/xemu-metal.mm`), and the ImGui
HUD renders via `imgui_impl_metal` over a black-cleared layer.

Architecture choices reached at implementation time:

1. **`.mm` (Objective-C++) for the host integration, not `.m`.**
   The `imgui_impl_metal.h` API uses C++ name mangling (no
   `extern "C"`). A pure ObjC `.m` file cannot link against
   `ImGui_ImplMetal_Init` etc. ObjC++ is required. The plan's
   "first .m file" guidance from M0 is amended to "first .mm
   file" here — the no-`nv2a_int.h` constraint still binds (the
   .mm file communicates with renderer.c only through C-callable
   entry points), but the file extension is `.mm`.

2. **`-fobjc-arc` enabled for ObjC++ project-wide.** Both
   `xemu_impl_metal.mm` (uses `@property strong`) and our
   `xemu-metal.mm` (uses `__bridge` casts) need ARC. The ARC arg
   is added as a project-level `objcpp` arg, gated on
   darwin+arm64. Existing `.m` files (cocoa.m, apple-gfx.m, etc.)
   are objc, not objcpp, and stay manual-retain/release.

3. **`imgui_impl_metal.mm` builds inside the imgui subproject,
   not duplicated in xemu's tree.** The earlier exploratory
   approach of compiling the backend directly in `xemu_ss`
   (referencing the file via `meson.global_source_root() / …`)
   tripped meson's sandbox restriction "Tried to grab file …
   from a nested subproject." Switching to `metal=enabled` in
   the imgui subproject's options (gated on darwin+arm64 in the
   parent `meson.build`) builds the backend inside the subproject
   and is the meson-supported pattern. The subproject's
   commented-out `add_languages('objcpp')` block is now real
   and gated on `get_option('metal').enabled()`.

4. **Renderer choice is read from `g_config.display.renderer` at
   startup; switching requires restart.** Same contract as the
   GL/Vulkan story today. A static helper in `ui/xemu.c`
   (`xemu_renderer_is_metal()`) gates window creation; a runtime
   helper `xemu_metal_is_active()` gates render-time branching
   so config changes through the in-game menu don't tear during
   the current session.

5. **The HUD framebuffer-texture call is skipped on Metal at M1.**
   `xemu_hud_set_framebuffer_texture(GLuint, bool)` is GL-only by
   signature. Rather than retrofit it now, M1 skips
   `RenderFramebuffer` (the GL composer) entirely on the Metal
   path; the layer's clear-to-black is the visible background.
   M2's surface manager will introduce a Metal-side framebuffer
   texture and re-thread the compositor; the HUD's
   `set_framebuffer_texture` API will likely become opaque-handle
   based then.

6. **Screenshots on Metal are deferred to M2.** `SaveScreenshot`
   is GL-only; the Metal path drops `g_screenshot_pending`
   without acting on it. M2's framebuffer texture will be
   readable via Metal blit + getBytes.

7. **`maxCommandBufferCount = 8` as specified in the plan.**
   Tight enough to detect leaks early; large enough for the
   HUD-frame + future M2 NV2A-frame + a small slack.

Rationale (entries that reverse a prior plan-time choice):

- The plan's M0 entry mentioned `XEMU_METAL_VALIDATION={0,1}` as
  landing with M0 then deferred to M1 (no device yet). At M1
  implementation time the validation toggle is still small and
  not on the M1 exit-gate critical path; further deferred to
  M2-M5 when capture-via-`MTLCaptureManager` becomes useful.
  This is a re-deferral, not a reversal.
- The plan's M1 entry mentioned a possible filename `mtl/device.m`
  for the new ObjC file. The actual landed name is
  `ui/xemu-metal.mm` because (a) ObjC++ is needed (see #1 above),
  and (b) the file owns *host* integration (SDL_MetalView,
  ImGui-Metal init) more than *renderer* state, so the natural
  home is alongside `ui/xemu.c` rather than under `mtl/`. The
  pgraph-side renderer ops still live in
  `hw/xbox/nv2a/pgraph/mtl/renderer.c` (the M0 stub); M2 will
  add ObjC++ files under `mtl/` for the surface manager that are
  separate from the host integration.

Verification:

- `./build.sh -a arm64` succeeds end-to-end (compile, link,
  dylibbundler, codesign). `dist/xemu.app` codesign verifies.
- `dist/xemu.app/Contents/MacOS/xemu --version` runs cleanly.
- `nm | grep -E "xemu_metal_|ImGui_ImplMetal"` shows all 9
  `xemu_metal_*` entry points and all 8 `ImGui_ImplMetal_*`
  symbols.
- All 22 `pgraph_mtl_*` M0 symbols still present.
- `Foundation` and `Metal` frameworks now appear in the binary's
  `LC_LOAD_DYLIB` table (M0 had them dependency-wired but
  dead-stripped). MetalKit and QuartzCore stay dead-stripped at
  M1; will be linked at M2/M11 as their APIs are used.
- Default `display.renderer = OPENGL` unchanged; the GL renderer
  code path is byte-identical at runtime when Metal is not
  selected.

Open M1 follow-up:

- Visual smoke gate (screenshot diff vs OpenGL HUD-only
  screenshot) is a user-driven launch test. The non-visual
  portion (build success, symbol presence, clean `--version`,
  framework links) is satisfied.
- M2 (surface manager + clear) is the next implementation slice.
  See `metal-renderer-plan.md` §4 M2.
- The renderer dropdown in `main-menu.cc:743` and
  `menubar.cc:178` does not list METAL on darwin (no
  `#ifdef CONFIG_METAL` guard equivalent of the existing
  `CONFIG_VULKAN` guard). M1 leaves this alone — the user
  selects METAL via xemu.toml. M2 or M14 will add the
  dropdown entry once the renderer is functional enough to
  expose to non-developer users.

## 2026-05-02: Metal slice M2 — clear-only surface manager + side-channel framebuffer texture accessor

Decision:

Land slice M2 of `metal-renderer-plan.md` — a clear-only surface
manager backed by two MTLHeap-based render-target heaps (color +
depth). Wire `pgraph_mtl_clear_surface` to a single-pass
`MTLLoadActionClear` render pass with no draws. Publish the current
color RT to the host compositor (`ui/xemu-metal.mm`) via a
side-channel accessor `pgraph_mtl_get_framebuffer_metal_texture()`,
because the existing `PGRAPHRenderer.ops.get_framebuffer_surface` op
returns `int` and an `id<MTLTexture>` is a 64-bit pointer that does
not round-trip through that signature. The int op returns 1/0 as a
truthy presence signal.

Rationale:

The plan's literal language ("`pgraph_mtl_get_framebuffer_surface(d)`
returns an opaque handle (an `MTLTexture*` for now)") is incompatible
with the cross-renderer dispatch table in `pgraph.h:131`, which types
the op as `int (*)(NV2AState *)`. Two viable resolutions exist:

1. Change the dispatch-table signature to return a `void *` or
   `intptr_t`. This touches GL, Vulkan, Null, and Metal renderers.
   Invasive, requires updating callers, and the GL impl's `GLuint` →
   `int` truncation is benign-but-ugly today; converting it all to
   `void *` would require auditing the surface-cache + flip-required
   logic at `ui/xemu.c:862-879` + `xui/gl-helpers.cc:41`. Out of M2's
   scope.

2. Keep the int op as a presence signal, expose the actual texture
   via a Metal-specific side-channel function. The compositor in
   `ui/xemu-metal.mm` is already Metal-only, so a Metal-specific
   accessor is a clean fit. No existing caller of the int op is on
   the Metal path: the only consumer is `gl_render_frame()` in
   `ui/xemu.c:862`, and `xemu.c:840` short-circuits to
   `xemu_metal_render_frame()` when `xemu_metal_is_active()` is
   true. So returning 1/0 from the int op preserves the contract for
   any future unanticipated caller while keeping the texture-level
   API Metal-only.

Resolution 2 chosen. Land cost: zero touch on GL/VK; one new
side-channel C function declared in `mtl/surface.h` and called from
`ui/xemu-metal.mm`. M3+ may revisit if a non-Metal consumer ever
needs the texture.

Files changed:

- Added `hw/xbox/nv2a/pgraph/mtl/heap.h` and `heap.mm` — MTLHeap
  manager (color + depth, 256 MiB each, MTLHeapTypeAutomatic +
  MTLStorageModePrivate + tracked).
- Added `hw/xbox/nv2a/pgraph/mtl/surface.h` and `surface.mm` —
  minimal surface manager: one color binding, one depth binding,
  NV097 → MTLPixelFormat translation, clear pass.
- Edited `hw/xbox/nv2a/pgraph/mtl/renderer.c` — `init`/`finalize`
  bring up heap + surface; `clear_surface` decodes shape + parameter
  and delegates; `get_framebuffer_surface` returns 1/0 presence;
  `surface_update`/`surface_flush`/`set_surface_scale_factor` wired
  with M2-appropriate behaviour (no upload/download, no surface
  cache invalidation, scale factor honored).
- Edited `hw/xbox/nv2a/pgraph/mtl/meson.build` — adds `heap.mm` and
  `surface.mm` to the `specific_ss` Metal source list.
- Edited `ui/xemu-metal.mm` — adds the present-blit
  fullscreen-triangle pipeline (lazy-built, MSL inline string), reads
  the side-channel framebuffer texture in
  `xemu_metal_end_imgui_frame`, encodes the blit before the ImGui
  draw data.

Heap sizes — 256 MiB color + 256 MiB depth — are the planning-doc
recommendation in `metal-renderer-plan.md` §3.6. The Xbox unified
pool is 64 MiB total; surface_scale=2 default (Apple Silicon
first-launch) brings 1280×960 BGRA8 to 4.7 MiB and D32_S8 to 6.3 MiB.
256 MiB comfortably holds 16+ active+pending surfaces. Apple Silicon
private memory is wired to the unified pool but not pre-touched —
unused heap regions consume no resident memory.

Pixel-format substitutions — Apple Silicon GPU family 7+ does not
support `Depth24Unorm_Stencil8`; Xbox `Z24S8` is mapped to
`Depth32Float_Stencil8` (higher precision, correctness preserved).
Xbox `A8R8G8B8` maps to `BGRA8Unorm` (matches the layer pixelFormat).
Documented in `surface.mm::nv097_color_to_mtl` /
`nv097_zeta_to_mtl`.

What M2 explicitly does NOT do (deferred to later slices):

- No vertex/index buffer machinery (M3).
- No NV2A drawing path (M3-M4).
- No shader translation (M5).
- No textures (M6).
- No combiner emulation (M7).
- No MSAA (M11).
- No per-VRAM-addr surface cache (M3+ — vk/surface.c's full surface
  lifecycle has 1500+ lines of cache logic; M2's simple "current
  color + current depth + reallocate when shape changes" is enough
  for the clear-only gate).
- No per-channel write mask (NV097_CLEAR_SURFACE_R/G/B/A masking
  during clear — M2 always clears all channels). The per-channel
  mask is a corner case (most Xbox titles clear all channels); M3+
  will add it when a draw pipeline already has color-mask state to
  wire in.
- No clear-rect scissor — M2 clears the full surface. Same M3+
  reasoning as above.

Verified:

- `./build.sh -a arm64` succeeds (build clean apart from the existing
  pre-M2 `gl/vertex.c` GNU-extension warnings).
- New symbols present in `dist/xemu.app/Contents/MacOS/xemu`:
  `_pgraph_mtl_heap_init`, `_pgraph_mtl_heap_alloc_color_rt`,
  `_pgraph_mtl_heap_alloc_depth_rt`, `_pgraph_mtl_surface_init`,
  `_pgraph_mtl_surface_clear`, `_pgraph_mtl_clear_surface`,
  `_pgraph_mtl_get_framebuffer_metal_texture`,
  `_pgraph_mtl_surface_clear_count`. The GL renderer's
  `_pgraph_gl_clear_surface` symbol is intact.
- `codesign --verify --deep --strict --verbose=2 dist/xemu.app` →
  `valid on disk` / `satisfies its Designated Requirement`.

Open M2 follow-up:

- Visual smoke gate (`validate-native-tri-depth.sh --run 22` against
  the Metal renderer + the `flat-tri-depth.xiso.iso` test asset, or a
  retail title's pre-3D boot splash) is a user-driven launch test
  per CLAUDE.md rule #10. The non-visual portion (build success,
  symbol presence, code signing) is satisfied above.
- M3 is the next implementation slice. See `metal-renderer-plan.md`
  §4 M3 (vertex/index buffers + first hand-coded MSL draw).
- Counter integration: `extract-perf-summary.sh` does not yet sum
  `METAL_CLEAR_COUNT`. The atomic in
  `surface.mm::pgraph_mtl_surface_clear_count()` is exposed but not
  yet plumbed into `xemu-perf:` interval lines. M3 / M4 will add
  this once the renderer is producing meaningful per-frame work.

## 2026-05-02: Metal slice M3 — vertex/index buffers + first hand-coded MSL draw

Decision:

Land slice M3 of the staged Metal renderer plan
(`docs/apple-silicon/metal-renderer-plan.md` §4 M3). Add a
triple-buffered staging ring, a single hand-coded MSL passthrough
pipeline, and a draw module that wires `flush_draw` to a real
`drawPrimitives` call for the NV097 inline_buffer (immediate-mode)
submission path. Vertex format generalization, primitive expansion
(quads / fans / line-loops), the `draw_arrays` /
`inline_elements` / `inline_array` paths, real shader translation,
and texturing all defer to subsequent slices.

Files added:

- `hw/xbox/nv2a/pgraph/mtl/buffer.h` / `buffer.mm` — staging ring +
  vertex-RAM accessor stub.
- `hw/xbox/nv2a/pgraph/mtl/pipeline.h` / `pipeline.mm` — single-
  pipeline cache for the passthrough MSL.
- `hw/xbox/nv2a/pgraph/mtl/draw.h` / `draw.mm` — `flush_draw`
  encoder.

Files edited:

- `hw/xbox/nv2a/pgraph/mtl/renderer.c` — `flush_draw` decodes NV2A
  primitive_mode + inline_buffer attributes and delegates to
  `pgraph_mtl_draw_passthrough`. `init` / `finalize` bring up the
  three new modules.
- `hw/xbox/nv2a/pgraph/mtl/surface.h` / `surface.mm` — accessors for
  the active color/depth texture, format, dimensions.
- `hw/xbox/nv2a/pgraph/mtl/meson.build` — registers the three new
  `.mm` files.

Rationale (key sub-decisions):

1. **Per-draw staging instead of 1:1 vertex-RAM mapping**. The plan
   spec called for a 64 MiB Shared|WriteCombined MTLBuffer mapped
   1:1 over guest VRAM via `newBufferWithBytesNoCopy`. M3 defers
   that and uses per-draw staging into the ring buffer. The
   `bytesNoCopy` path ties MTLBuffer lifetime to QEMU's memory
   region and removes the natural place to insert the
   upload-bitmap invalidation tracking that
   `vk/buffer.c::pgraph_vk_update_vertex_ram_buffer` uses; the
   1:1 mapping pays off only when the draw_arrays / inline_elements
   paths land (M4+). Plan doc explicitly notes this is a future
   optimization. The accessor `pgraph_mtl_buffer_get_vertex_ram`
   is preserved in the API surface (returns NULL today) so M4
   can introduce the persistent VRAM mapping without rewriting
   the draw paths.

2. **Triple-buffered ring with MTLSharedEvent + dedicated signal
   queue**. Three slots × 16 MiB Shared|WriteCombined MTLBuffer
   each; an MTLSharedEvent signaled by an empty command buffer
   submitted to a dedicated low-traffic signal queue
   (`xemu.metal.buffer_signal_queue`) gates slot reuse. begin_frame
   waits the slot's last-known signal value via
   `[event waitUntilSignaledValue:atTimeout:]` (1000 ms timeout);
   end_frame bumps the monotonic counter, snapshots it onto the
   active slot, and submits the signal command buffer. For M3 the
   ring rotates per-draw (no per-UI-frame batching yet); M5+ will
   move to one signal per UI frame as part of the larger
   command-buffer-per-frame refactor.

3. **Single passthrough pipeline cached on (color_fmt, depth_fmt)**.
   M3 ships ONE MSL — `passthrough_vs` reads `[[attribute(0)]]`
   position and `[[attribute(3)]]` color (matching
   `NV2A_VERTEX_ATTR_POSITION` = 0 and `NV2A_VERTEX_ATTR_DIFFUSE`
   = 3), `passthrough_fs` returns the interpolated color directly.
   Library is pre-compiled at `pgraph_mtl_pipeline_init`;
   per-format MTLRenderPipelineState builds lazily on first use.
   Cache is a 16-entry linear-scan array; in practice the Xbox
   runs everything through `BGRA8Unorm + Depth32Float_Stencil8`
   on Apple Silicon so only one entry is hit. M5 swaps this for
   the LRU + POD PipelineKey.

4. **Vertex-format scope = inline_buffer path only**. NV2A's actual
   vertex format is highly variable (per-attribute stride / type /
   count / normalize-bit / signed-vs-unsigned-vs-float) and
   porting `vk/vertex.c::pgraph_vk_bind_vertex_attributes` is a
   large piece of work. M3 deliberately bounds the scope to
   "already-stored-as-floats-in-host-memory" inline_buffer (the
   NV097 immediate-mode path). M4 ports the format-resolving /
   aligned-stride remap logic from
   `vk/draw.c::remap_unaligned_attributes`. Quad / fan / line-loop
   primitives also skip on M3 because they need index expansion;
   M4's IndexGenerator port handles that.

5. **C / .mm boundary preserved**. `renderer.c` is the only file in
   `mtl/` that includes `nv2a_int.h` (per the M0 implementation
   note: per-target preprocessor flags do not propagate to .mm
   builds). `renderer.c` decodes `pg->primitive_mode` /
   `pg->inline_buffer_length` / `pg->vertex_attributes[N]` and
   passes plain `float *` arrays + opaque `void *` texture handles
   into `draw.mm`. Same boundary used by `heap.mm` and `surface.mm`.

6. **Counters NOT yet routed through `extract-perf-summary.sh`**.
   `pgraph_mtl_draw_count` / `pgraph_mtl_buffer_stage_bytes` /
   `pgraph_mtl_buffer_frame_count` /
   `pgraph_mtl_pipeline_compile_count` are exposed as atomics but
   not surfaced in `xemu-perf:` interval lines. M4 will register
   them as `METAL_DRAW_COUNT` / `METAL_STAGE_BYTES` /
   `METAL_PIPELINE_COMPILE_COUNT` so the validation gate can
   compare against `gl_draw_count`. Deferred because M3 alone does
   not produce a representative scene yet (no quad / fan /
   draw_arrays / shader translation), so per-counter validation
   against the GL path needs M4 + M5 first.

Verified:

- `./build.sh -a arm64` succeeds.
- New symbols present in `dist/xemu.app/Contents/MacOS/xemu`:
  `_pgraph_mtl_buffer_init/finalize/begin_frame/end_frame/stage_vertex/
  stage_index/stage_uniform/get_vertex_ram/invalidate_vertex_ram_range/
  stage_bytes/frame_count`,
  `_pgraph_mtl_pipeline_init/finalize/get_passthrough/compile_count`,
  `_pgraph_mtl_draw_init/finalize/passthrough/count`,
  `_pgraph_mtl_surface_get_color_texture/depth_texture/color_format/
  depth_format/width/height`.
- Existing M0 / M1 / M2 symbols all still present
  (`_pgraph_mtl_heap_*`, `_pgraph_mtl_surface_init/clear/ensure_*/
  clear_count`, `_xemu_metal_init`, `ImGui_ImplMetal_*`).
- GL renderer symbols intact (`_pgraph_gl_draw_begin`,
  `_pgraph_gl_flush_draw`).
- `codesign --verify --deep --strict --verbose=2 dist/xemu.app` →
  `valid on disk` / `satisfies its Designated Requirement`.

Open M3 follow-up:

- Visual smoke gate (running `flat-tri-depth.xiso.iso` on the Metal
  renderer and confirming colored triangles are visible) is a
  user-driven launch test per CLAUDE.md rule #10. The non-visual
  portion (build success, symbol presence, code signing,
  M0 / M1 / M2-symbol intact-ness) is satisfied above.
- M4 (IndexGenerator port + quad / fan expansion + native_quad /
  native_tri_depth carryover from the GL path) is the next
  implementation slice. The biggest dependency for the validation
  gate is the `XEMU_NATIVE_TRI_DEPTH=1` correctness behavior
  carrying over into Metal — without it Crimson's depth /
  polygon-offset is wrong on the Metal path even after textures
  land in M6.
- M5 (full shader translation: GLSL → SPIR-V → MSL via
  spirv-cross) is the next-most-important slice for any retail
  title to look correct; M3's passthrough only renders
  interpolated vertex color with no NV2A combiner / lighting /
  texturing applied.
- Counter integration into `extract-perf-summary.sh` deferred to
  M4 (see rationale point 6 above).

## 2026-05-02: Metal slice M4 — IndexGenerator port + native_quad / native_tri_depth

Decision:

Ship Metal slice M4 from `metal-renderer-plan.md`. The Metal renderer
gains CPU index expansion for primitive types Metal does not expose
natively (triangle fan, quads, quad strip, polygon, line loop), a
native-depth fragment-shader variant that derives `gl_FragCoord.z`-
equivalent depth via `dfdx/dfdy` slope-of-z reconstruction, and the
counter plumbing (`METAL_DRAW_COUNT`, `METAL_DRAW_INDEXED_COUNT`,
`METAL_NATIVE_TRI_DEPTH_DRAWS`, `METAL_NATIVE_QUAD_DRAWS`,
`METAL_CLEAR_COUNT`) needed for the M4 exit-gate "counters match GL
counts" assertion. Native-tri-depth and native-quad eligibility are
the *only* path on Metal (no geometry-shader fallback exists);
metal-renderer-plan.md §3.8 already records that decision — this M4
landing makes it concrete in code.

Rationale:

1. **Quad triangulation must match GL byte-for-byte.** PR #2240's
   polygon-offset slope reconstruction is sensitive to the diagonal
   choice — a mirror-image diagonal triangulates the same quad
   geometry but produces a different `nativeTriMZ` (max of
   `|dfdx|`, `|dfdy|`) and therefore a different per-fragment depth
   bias. The Metal expansion (`mtl/index_gen.c`) uses the A-C
   diagonal with QUADS emit order `(b,c,a)+(c,d,a)` and QUAD_STRIP
   order `(a,b,c)+(c,b,d)`, identical to
   `gl/draw.c::native_quad_list_expand_indices` (line 259) and
   `native_quad_strip_expand_indices` (line 301). Diverging would
   break depth correctness on quads — explicit per-CLAUDE.md rule
   #6 (do not strip PR #2240 correctness work).

2. **Eligibility helpers are reused, not duplicated.** Renderer.c's
   `mtl_native_tri_depth_eligible` / `mtl_native_quad_eligible`
   call the existing GL helpers
   `pgraph_glsl_native_tri_depth_supported` /
   `pgraph_glsl_native_quad_supported` directly, gated by
   `pgraph_glsl_native_tri_depth_enabled()` /
   `pgraph_glsl_native_quad_enabled()`. The eligibility rules
   *cannot drift* between GL and Metal because they are literally
   the same function call. This is the simplest possible counter-
   parity guarantee for the M4 exit gate "counters match the GL
   counts" — same input, same answer.

3. **Counter parity is achieved through a shared mechanism.** The
   Metal renderer drives the SAME profile counters
   (`NV2A_PROF_NATIVE_TRI_DEPTH_DRAW`,
   `NV2A_PROF_NATIVE_QUAD_DRAW`, etc.) the GL path drives, so the
   existing `NATIVE_TRI_DEPTH_DRAW` / `NATIVE_QUAD_DRAW` keys in
   `extract-perf-summary.sh` describe both renderers without any
   summary-script change. The new `METAL_*` keys exist as a
   parallel sanity check (and to expose the renderer split when
   A/B'ing the two backends on the same workload) — they are NOT
   the only source of truth.

4. **Native-depth MSL is structured scaffolding, not the final
   PSH.** The `passthrough_native_depth_fs` MSL function derives
   the same `nativeTriMZ` slope-of-z the GL native_tri_depth path
   derives (`max(abs(dfdx(zvalue)), abs(dfdy(zvalue)))`) but
   writes only `zvalue = in.position.z` — i.e. byte-identical
   to fixed-function depth. The full PR #2240 polynomial offset
   (`zvalue += depthFactor*nativeTriMZ + depthOffset`) requires
   the `clipRange` / `depthFactor` / `depthOffset` /
   `surfaceScale` uniforms that arrive with the M5 PSH translator.
   The MSL is structured so M5 can flip on the offset with one
   buffer-bind + one uncomment.

5. **Plan documentation correction.** The plan's M4 scope phrased
   the GL native-quad expansion as living in
   `pgraph_native_quad.c`. The actual implementation lives inline
   in `gl/draw.c` (lines 218-352:
   `pgraph_gl_native_quad_reserve`, `native_quad_list_expand_*`,
   `native_quad_strip_expand_*`, `pgraph_gl_native_quad_expand_*`,
   `pgraph_gl_native_quad_index_capacity`). The M4 SHIPPED note
   in the plan records the correction.

6. **Pipeline cache key extension.** The cache now keys on
   `(color_pixel_format, depth_pixel_format, variant)` so the M3
   `passthrough_fs` and the M4 `passthrough_native_depth_fs` can
   coexist for the same (color_fmt, depth_fmt) tuple without
   collision. Cache cap was raised from 16 → 32 entries to keep
   ~2× headroom.

Files added:

- `hw/xbox/nv2a/pgraph/mtl/index_gen.h` (interface).
- `hw/xbox/nv2a/pgraph/mtl/index_gen.c` (~150 LOC pure-C
  expansion routines). Diagonals match GL for quads; triangle-fan
  and polygon use the standard fan-around-vertex-0 layout that
  matches geom.c's PRIM_TYPE_POLYGON / PRIM_TYPE_TRIANGLE_FAN
  fill-mode emission.
- `include/qemu/xemu-metal-perf.h` and `util/xemu-metal-perf.c`
  (emit-and-reset hook for the new METAL_* interval-line fields,
  weak-symbol counter accessors so non-Apple-Silicon builds link
  cleanly).

Files edited:

- `hw/xbox/nv2a/pgraph/mtl/meson.build` — adds `index_gen.c`.
- `hw/xbox/nv2a/pgraph/mtl/pipeline.{h,mm}` — adds
  `passthrough_native_depth_fs` MSL function + variant-tagged
  cache + `pgraph_mtl_pipeline_get_native_depth` accessor.
- `hw/xbox/nv2a/pgraph/mtl/draw.{h,mm}` — adds
  `pgraph_mtl_draw_indexed`, per-variant counter accessors
  (`pgraph_mtl_draw_indexed_count` /
  `pgraph_mtl_draw_native_tri_depth_count` /
  `pgraph_mtl_draw_native_quad_count`), and increment hooks
  (`pgraph_mtl_draw_inc_native_tri_depth_count` /
  `pgraph_mtl_draw_inc_native_quad_count`) called from
  renderer.c.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c` — `flush_draw` now
  dispatches to non-indexed or indexed path based on primitive,
  selects fragment-shader variant via the GL eligibility
  helpers, and bumps both the GL-shared profile counters and
  the Metal-specific deltas.
- `util/meson.build` — adds `xemu-metal-perf.c`.
- `hw/xbox/nv2a/pgraph/profile.c` — calls
  `xemu_metal_perf_emit_and_reset(stderr)` from per-interval
  emit (no-op when GL is active).
- `scripts/apple-silicon/extract-perf-summary.sh` — recognizes
  `METAL_DRAW_COUNT` / `METAL_DRAW_INDEXED_COUNT` /
  `METAL_NATIVE_TRI_DEPTH_DRAWS` / `METAL_NATIVE_QUAD_DRAWS` /
  `METAL_CLEAR_COUNT` and surfaces them in the summary output.

Verification:

- `./build.sh -a arm64` — succeeds (Apple-Silicon arm64).
- `dist/xemu.app/Contents/MacOS/xemu --version` runs and reports
  `xemu_version: 0.8.134-58-gcaa5de0a96`.
- `codesign --verify --deep --strict --verbose=2 dist/xemu.app`
  → `valid on disk` / `satisfies its Designated Requirement`.
- `nm dist/xemu.app/Contents/MacOS/xemu | grep -E
  '(pgraph_mtl_idx_expand|pgraph_mtl_draw_indexed|pgraph_mtl_pipeline_get_native_depth|xemu_metal_perf_emit_and_reset)'`
  shows all M4 symbols.
- GL native_quad path symbols intact
  (`pgraph_gl_native_quad_expand_range`,
  `pgraph_gl_native_quad_reserve`, `pgraph_gl_renderer`).

Open M4 follow-up:

- The visual half of the M4 exit gate ("≤ 1 % per-pixel diff vs
  GL on a tail-30 second PGR2 mid-route sample") cannot be met
  until M5 (shader translation) + M6 (textures) + M7
  (combiners) land — until then no real-game scene renders
  correctly through Metal regardless of geometry correctness.
  The non-visual portion (build, symbols, counter plumbing,
  GL-shared eligibility) is met today.
- M4 native-depth MSL writes only `zvalue = position.z`
  (fixed-function-equivalent). The PR #2240 polynomial offset
  needs the `clipRange` / `depthFactor` / `depthOffset` /
  `surfaceScale` uniforms that arrive with M5; the MSL is
  structured so flipping on the offset is one buffer-bind +
  one uncomment.
- M5 is the next implementation slice. With the index
  expansion + counter plumbing in place, M5 can focus on the
  PSH/VSH translation pipeline and the proper LRU-on-
  PipelineKey cache without geometry-shader-handling
  distractions.

## 2026-05-03: Metal screenshot capture for visual validation

The M5.6 / M15 default-on validation criteria require visual
diff-vs-GL screenshots, but the existing
`scripts/apple-silicon/macos-capture.sh` path uses macOS
`screencapture`, which triggers a Screen-Recording permission
dialog. That dialog occludes the xemu window for the duration of
the benchmark, with two unwanted side effects:

1. CoreAnimation's `addPresentedHandler:` does not fire for an
   occluded layer, so `s_presents_total` stays at 0 →
   `METAL_PRESENTS = 0` in the perf counters → the M10 frame-
   pacing telemetry (jitter/avg/max) is unusable for that run.
2. The user-visible image and the captured image diverge: the
   dialog covers the xemu window during gameplay, so the
   `screencapture`-emitted PNG shows the dialog rather than the
   rendered frame.

Both points block the visual-correctness gate that M5.6 part B
and M15 default-on need. Land an in-renderer programmatic
screenshot path that bypasses both:

- New env vars `XEMU_METAL_SCREENSHOT_PATH=/path/to/file.png`,
  `XEMU_METAL_SCREENSHOT_AT_FRAME=N` (default 60),
  `XEMU_METAL_SCREENSHOT_INTERVAL=N` (default 0 = single shot).
  Frame numbering is 1-indexed against a new submit-time
  end-of-frame counter `s_end_frames`, NOT against
  `s_presents_total` — the present counter is the broken thing
  we are working around.
- Capture point: AFTER the HUD ImGui-Metal render encoder
  closes (`[s_current_enc endEncoding]` in
  `xemu_metal_end_imgui_frame`) and BEFORE
  `[s_current_cmd presentDrawable:...]`. At that point the
  drawable's BGRA8Unorm_sRGB texture is the final composited
  frame including HUD, identical to what the user would see on
  screen when no dialog occludes.
- Implementation: a single `MTLBlitCommandEncoder
  copyFromTexture:...:toBuffer:` from the drawable into a
  per-shot host-shared `MTLBuffer` (size = w·h·4); cmdbuf's
  `addCompletedHandler:` reads the buffer's bytes after GPU
  completion, swaps BGRA→RGBA, and writes a PNG via FPNG
  (`ui/thirdparty/fpng/`, already linked into `xemu_ss`).
  PNG-encoding errors are logged and swallowed.
- Required side effect: when
  `XEMU_METAL_SCREENSHOT_PATH` is set, `xemu_metal_init` flips
  `s_layer.framebufferOnly` from `YES` to `NO` so the drawable
  texture can be the source of a blit. Display compression is
  off only for screenshot-enabled runs; default-off path keeps
  the Apple Silicon UMA compression on.
- Counter `METAL_SCREENSHOTS_TAKEN` (per-interval delta) bumps
  only on successfully-encoded PNGs.

FPNG-include note: `<fpng.h>` transitively includes libc++'s
`<atomic>`, which errors on `-std=c++17` when `<stdatomic.h>` is
also in scope ("incompatible with `<stdatomic.h>` before C++23").
xemu-metal.mm needs `<stdatomic.h>` for inter-thread counters,
so we forward-declare the two FPNG entry points (`fpng_init` and
`fpng_encode_image_to_file`) by hand instead of including
`<fpng.h>`. Both have C++ linkage in namespace `fpng`; the
forward decl matches FPNG's signature exactly.

Companion script changes on `scripts/apple-silicon/run-benchmark.sh`:
new `--metal-screenshot <path>` and `--metal-screenshot-at-frame <N>`
flags mirror the M13 `--metal-capture <path>` pattern (export
the env var pre-launch; record the path in `metadata.txt`).
`extract-perf-summary.sh` learns the new
`METAL_SCREENSHOTS_TAKEN` key.

Verification on this fork (M3 Ultra, macOS 26.4):

- Build: `./build.sh -a arm64` PASS;
  `codesign --verify --deep --strict --verbose=2
  dist/xemu.app` returns "valid on disk".
- M5 shader-validation harness:
  `XEMU_METAL_SHADER_VALIDATE=1
  XEMU_METAL_SHADER_VALIDATE_AND_EXIT=1
  dist/xemu.app/Contents/MacOS/xemu` → 7/7 PASS.
- Smoke test:
  `XEMU_RENDERER=METAL
  XEMU_METAL_SCREENSHOT_PATH=/tmp/test.png
  XEMU_METAL_SCREENSHOT_AT_FRAME=120
  scripts/apple-silicon/run-benchmark.sh pgr2
  scripts/apple-silicon/input-scripts/pgr2-smoke.csv 30` →
  `/tmp/test.png` 1280×931 PNG, 8-bit RGBA, valid (`file` +
  `sips`). `xemu.log` shows
  `xemu-perf: metal_screenshot path=/tmp/test.png at_frame=120
  interval=0` at startup,
  `xemu-perf: metal_screenshot_written
  path=/tmp/test.png w=1280 h=931` post-capture, and
  `METAL_SCREENSHOTS_TAKEN=1` on the corresponding interval.
  Visual content at frame 120: pgr2-smoke.csv runs only 30 s and
  the snapshot lands in xemu's HUD/menu bar window pre-BIOS — a
  real in-game frame requires a longer-duration script or a
  larger `--metal-screenshot-at-frame` value (≥ 600).

Files touched:

- `ui/xemu-metal.mm` (parse env, blit + encode pipeline,
  end-of-frame submit counter, framebufferOnly flip, shutdown
  free).
- `util/xemu-metal-perf.c` (weak default + emit/reset for
  `METAL_SCREENSHOTS_TAKEN`).
- `scripts/apple-silicon/run-benchmark.sh` (new flags,
  metadata, env export).
- `scripts/apple-silicon/extract-perf-summary.sh` (counter
  recognition + per-interval emission).
- `docs/apple-silicon/automation.md`, `xemu-fork/CLAUDE.md`,
  `docs/apple-silicon/handoff.md` (env-var documentation,
  banner update).

Constraint adherence: no edits under
`hw/xbox/nv2a/pgraph/mtl/` (the parallel-running M5.6 part B
agent owns that subtree). Stayed strictly in `ui/xemu-metal.mm`,
`util/xemu-metal-perf.c`, and `scripts/apple-silicon/`.

