# 2026-05-03 — Metal slice M5.9-followup-E surface-cache fixes

## Summary

Three real bugs in the Metal renderer's surface manager fixed via a
new per-vram_addr `metal_draw_target` diagnostic counter at
`pgraph_mtl_flush_draw`. The counter exposed exactly which cached
SurfaceBinding receives each draw — the decisive measurement queued
in the prior decision-log "front/back/aux RTs all ruled out as scene
targets" entry.

**Net result**: the magenta heap-default artifact in the
CRTC-published front-fb is closed. The visual gate **still fails**
because PGR2's rendered scene lives in the back buffer at
`0x3628000` while the CRTC publishes the front buffer at
`0x32a4000`, and the Metal renderer has no mechanism to bridge
them. New slice **M5.10** queued to address this.

## Setup

- Working tree: `apple-silicon-performance` branch at HEAD
  `0109deb649` (pre-this-slice).
- Build: `./build.sh -a arm64`, PASS.
- Host: Apple M3 Ultra, macOS 26.4.1.
- Game: PGR2 cold-boot via `pgr2-gameplay.csv` scripted input.
- Renderer: `XEMU_RENDERER=METAL XEMU_METAL_TRANSLATED_PIPELINE=1`.
- Perf log: `XEMU_PERF_LOG=1 XEMU_PERF_LOG_INTERVAL_MS=2000`.

## What landed

### Diagnostic instrumentation (lands first)

- `mtl/renderer.c::mtl_draw_target_bump(uint32_t vram_addr)` —
  bounded per-vram_addr counter table (cap 32 distinct addresses +
  overflow + zero-vram_addr buckets). Bumped from
  `pgraph_mtl_flush_draw` after `mtl_bind_current_surfaces`. Reads
  the bound color binding's vram_addr via the new
  `pgraph_mtl_surface_get_color_vram_addr()` getter (with
  symmetric `_get_depth_vram_addr` companion).
- `mtl/renderer.c::pgraph_mtl_draw_target_emit_interval(FILE *)` —
  called from `nv2a_profile_log_emit_interval` AFTER the main
  `xemu-perf:` line is closed. Emits zero-or-more
  `xemu-perf: metal_draw_target vram_addr=0x.. count=N` lines per
  interval, plus a one-shot `metal_draw_target_first` at first
  sighting. Resets per-interval counts on emit; the slot table is
  preserved across resets.
- `mtl/surface.mm::pgraph_mtl_surface_recreate_shape_mismatch()` —
  monotonic counter; bumped each time
  `cache_find_or_create_color/_depth` destroys an existing
  same-vram_addr cache entry to recreate it under a different shape.
  Bounded log line `xemu-perf: metal_surface_recreate vram_addr=0x..
  old=WxH/fmtN new=WxH/fmtN (color|depth)` for the first 16 events.
  Surfaced as `METAL_SURFACE_RECREATE_SHAPE_MISMATCH`.

### PGR2 60 s benchmark — the empirical evidence

Steady-state per-2 s-interval draw target distribution (run
`benchmark-runs/20260503-161331-pgr2`):

| vram_addr | dimensions | role | draws/interval |
|---|---|---|---|
| `0x3628000` | 1280×480 (2560×960 host-scaled) | back buffer | 908–1776 |
| `0x2c06000` | 1024×512 aux | env/reflection | 434–1302 |
| `0x2e06000` | 1024×512 aux | env/reflection | 434–868 |
| `0x32a4000` | 640×480 (1280×960 host-scaled) | front buffer (CRTC) | **3–4** |
| `0x0` | 512×512 R5G6B5 (1024×1024) | aux RT | 1197–1995 |

Plus six 256×256 aux RTs at 0x2854000–0x2994000.

**Conclusion from the counter**: PGR2's scene-rendering goes to the
back buffer at `0x3628000`, not to the CRTC-published front buffer
at `0x32a4000`. The front buffer receives only 3–4 draws/interval —
HUD elements at most, not a per-frame composite (which would be
~30 draws/interval at 30 FPS). PGR2 must use SOME mechanism to move
the scene from back to front, but `NV097_IMAGE_BLIT` count is 0
(prior diagnostic), CPU memcpy dirty-events are 0 (followup-B+C),
and `pcrtc.start` is stable (followup-A). The actual mechanism is
still unknown.

### Three surface-manager bugs the counter exposed

1. **Color/depth cache collision on `vram_addr=0x0`** —
   `metal_surface_recreate` log showed 16 alternating recreate events
   between `1280×960/fmt2 (color)` and `1024×1024/fmt3 (depth)` at
   `vram_addr=0x0`. PGR2 binds both a color RT and a depth RT at
   `vram_addr=0` (the legacy ensure path uses 0 as a sentinel; PGR2
   also has a real surface where `dma.address+offset=0`). The
   `cache_get_at` lookup was unfiltered by aspect — each alternating
   bind found an entry of the wrong aspect, fell through the
   destroy-and-recreate path, and clobbered the prior binding.
   **Fix**: split into `cache_get_at_color` and `cache_get_at_depth`.
   `METAL_SURFACE_RECREATE_SHAPE_MISMATCH` drops 6/interval → 0.

2. **LRU eviction of stably-published front-fb** — PGR2 binds the
   back buffer for rendering most of the time;
   `s_color_binding = back_buffer`. The front-fb at `0x32a4000` is
   no longer `s_color_binding`, so it's eligible for eviction. The
   `pgraph_mtl_surface_publish_front_fb` dedupe path returned early
   without bumping `last_use_seq` when the published texture was
   already current — so a stably-published front-fb's LRU score
   went stale and the cache happily picked it as eviction victim,
   destroying the texture the compositor was reading. The
   compositor then read a freed `id<MTLTexture>` and showed
   heap-default magenta. **Fix**: bump `last_use_seq` on every
   publish call (before the dedupe check), AND add explicit
   front-fb pin to `cache_evict_lru` that skips the entry whose
   texture matches `s_front_framebuffer_texture`.

3. **Cache cap=16 too small** — `kMaxCacheEntries = 16` was always
   saturated on PGR2 (10+ color RTs + depth surfaces + ensure-by-shape
   entries). LRU was thrashing real surfaces. **Fix**: raise cap to
   32. After raise, `METAL_SURFACE_CACHE_SIZE` grows past 16
   (observed 17/19/21 with PGR2 boot-phase activity), and a new
   `metal_draw_target_first` slot 10 entry (`0x2454000`) appeared
   that the previous cap was hiding.

### Codex-validate review (rule #15)

Verdict: **MAJOR ISSUES**.

- **HIGH (fixed in-slice).** The shape-mismatch recreate path in
  `cache_find_or_create_color` could destroy the currently-published
  front-fb's MTLTexture without clearing
  `s_front_framebuffer_texture`. The LRU pin protects against
  eviction; this path was an unprotected second route to the same
  artifact. Fix: clear `s_front_framebuffer_texture` if it equals
  the entry's texture before `binding_destroy(e)`. Color side only —
  the publish path operates exclusively on color bindings, so depth
  recreate never matches the front-fb.
- **MEDIUM (deferred to M5.10 prerequisite work).**
  `register_access_cb_for` / `_unregister_access_cb_for` key on
  unfiltered `cache_get_at(vram_addr)`. Now that color and depth
  entries can co-exist at the same vram_addr, these accessors can
  hit the wrong entry. Refactor to a separate vram-range registry,
  or have register/unregister helpers walk all matching entries.
  Not load-bearing for this slice's user-visible behavior (PGR2
  dirty-tracking already showed 0 hits).
- **LOW (deferred).** The `metal_draw_target_zero` bucket conflates
  three distinct sources: real `vram_addr=0` color RT,
  ensure-by-shape fallback, and "no color binding". Plan: extend
  the getter to return `{has_binding, vram_addr}`.

## Validation gates

| Gate | Result |
|------|--------|
| Build (`./build.sh -a arm64`) | PASS |
| `METAL_PIPELINE_TRANSLATED_FAILED == 0` | PASS |
| `METAL_PIPELINE_FALLBACKS == 0` | PASS |
| `METAL_DRAW_TRANSLATED == METAL_DRAW_COUNT` | PASS (100 % translated) |
| `METAL_SURFACE_RECREATE_SHAPE_MISMATCH > 0` before fix | PASS (6/interval) |
| `METAL_SURFACE_RECREATE_SHAPE_MISMATCH == 0` after fix | PASS |
| `METAL_SURFACE_CACHE_SIZE > 16` after cap raise | PASS (observed 21) |
| GL renderer regression | NOT REGRESSED (60 fps PGR2 GL run unchanged; all changes mtl/-only or weak-symbol-gated) |
| codex-validate review | RAN, HIGH finding fixed in-slice, MEDIUM/LOW deferred |
| **Visual gate — captured PNGs show rendered scene** | **STILL FAIL.** PGR2's published front-fb at `0x32a4000` no longer shows magenta heap-default content (the LRU eviction bug is closed) but is still empty of the actual scene. PGR2 renders to back buffer; CRTC publishes front buffer; no Metal-side mechanism propagates rendered GPU content to the CRTC-pointed front-fb. M5.10 queued. |

## Files touched

- `hw/xbox/nv2a/pgraph/mtl/renderer.c` — added the per-vram_addr
  draw-target table + bump + emit; mtl_draw_target_bump call site in
  `pgraph_mtl_flush_draw` after surface bind. +130 LOC.
- `hw/xbox/nv2a/pgraph/mtl/surface.{h,mm}` — color/depth-filtered
  cache lookups, vram_addr getters for color/depth bindings,
  shape-mismatch counter, front-fb pin in `cache_evict_lru`,
  `last_use_seq` bump on publish, cap raised from 16 to 32, codex
  follow-up clear of `s_front_framebuffer_texture` in shape-mismatch
  recreate path. +14 / +152 LOC.
- `util/xemu-metal-perf.c` — `METAL_SURFACE_RECREATE_SHAPE_MISMATCH`
  baseline + delta + emission; weak symbol fallback for
  `pgraph_mtl_draw_target_emit_interval`. +33 LOC.
- `hw/xbox/nv2a/pgraph/profile.c` — call
  `pgraph_mtl_draw_target_emit_interval` immediately AFTER the main
  interval-line newline. +10 LOC.
- `scripts/apple-silicon/extract-perf-summary.sh` — recognize the new
  counter key.
- `docs/apple-silicon/{handoff,decision-log,automation,metal-renderer-plan}.md`
  + `xemu-fork/CLAUDE.md` + this benchmark note — doc reconciliation.

LOC delta: +590 / -15 across 8 files (including docs).

## Next session

Open slice **M5.10 — VRAM-coherent surface download** (or
alternatively: register a `get_framebuffer_surface` ops callback for
the Metal renderer that mirrors GL's display flow). See
`docs/apple-silicon/metal-renderer-plan.md` §M5.10 for the two
implementation paths and the concrete task list. M15 default-on
stays BLOCKED on M5.10.

Run with the `pgr2_gameplay_b4` mid-route snapshot
(`benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`) for fast
iteration to actual gameplay state — PGR2 cold-boot is too slow on
the current Metal path (~9 fps cold-boot in this session) to reach
gameplay in a reasonable benchmark window.
