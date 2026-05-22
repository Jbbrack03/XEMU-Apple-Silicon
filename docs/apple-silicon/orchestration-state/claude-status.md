# Claude Status

- Objective: §H.6 IMAGE_BLIT XBE — second second-wave Gate 2 slice.
- Status: **OPTION B — bounded partial closed cleanly (this session).**
- Slice target: `xbe-tests/image-blit/` Tier-2 NV2A diag XBE.

## What landed this session

### Original v0.1 failure (carry-forward from prior bounded session)

Assertion crash at `mtl/blit.c:170 source_offset < source_dma_len`
on the FIRST IMAGE_BLIT. `METAL_IMAGE_BLITS = 0`. Artifact:
`benchmark-runs/xbe-harness-20260522-071449/`.

### Root cause (grounded via qemu trace, NOT speculation)

`pb_init` calls `pb_target_back_buffer() → set_draw_buffer()`
(`nxdk/lib/pbkit/pbkit.c:1611-1668, called at 3260`). That
function **reprograms PRAMIN for `pb_DmaChID9Inst` and
`pb_DmaChID11Inst`** to:
- `addr = framebuffer_base`
- `limit = height * pitch - 1` (= `0x0012BFFF` for 640×480 LE_A8R8G8B8)

Verified empirically with `-trace events=nv2a_dma_map`: the
PRAMIN entry for sDmaObject9 shows
`addr=0x03BD4000 limit=0x0012BFFF` immediately before the
assertion. The original carry-forward design that "channels 9
and 11 are MAXRAM-mapped" was a misread of pbkit — `pb_create_dma_ctx`
does create them with Limit=MAXRAM, but `pb_init`'s tail
reprograms them via `set_draw_buffer()` before returning.

### Fix applied (v0.2)

`scripts/apple-silicon/xbe-tests/image-blit/nv2a_regs_image_blit.h`:
- `IMAGE_BLIT_DMA_HANDLE_SRC: 9 → 3` (sDmaObject3, base=0, Limit=MAXRAM,
  untouched by `set_draw_buffer`)
- `IMAGE_BLIT_DMA_HANDLE_DST: 11 → 4` (sDmaObject4, base=0, Limit=MAXRAM,
  untouched by `set_draw_buffer`)

`scripts/apple-silicon/xbe-tests/image-blit/main.c`:
- 3 `MmAllocateContiguousMemoryEx` sites: `0x3FFB000 → MAXRAM`
  (conservative; matches `pbkit.c:2297-2305`'s own pattern).
- Top-of-file doc comment updated to reflect channel 3/4 choice +
  `set_draw_buffer` reprogram trap.

### Result on Metal (v0.2)

`benchmark-runs/xbe-harness-20260522-075241/image-blit/metal/`:
- **No assertion crash** (xemu runs to completion).
- 278 frames captured (vs 66 in the crashing run).
- `signal_match_pct = 37.5000` (3 of 8 cells PASS byte-correct).
- `changed_pixels_pct = 62.7083`.
- Best frame: `image-blit.0124.png`.

Per-cell verdict:

| Cell | In(x,y) | Out(x,y) | W×H   | Verdict     |
|------|---------|----------|-------|-------------|
| 0    | (0,0)   | (0,0)    | 8×8   | **PASS**    |
| 1    | (0,0)   | (0,0)    | 16×16 | FAIL        |
| 2    | (0,0)   | (0,0)    | 32×32 | FAIL        |
| 3    | (8,8)   | (0,0)    | 8×8   | FAIL        |
| 4    | (0,0)   | (4,4)    | 8×8   | **PASS**    |
| 5    | (4,4)   | (8,8)    | 8×8   | **PASS**    |
| 6    | (0,0)   | (0,0)    | 1×16  | FAIL        |
| 7    | (0,0)   | (0,0)    | 16×1  | FAIL        |

PASS envelope: `width == height == 8 AND (in_x, in_y) ≤ (4, 4)`.

### Important: METAL_IMAGE_BLITS=0 is the expected steady state

The counter only increments after the OPTIONAL GPU-side cached-
texture copy path (`surface.mm:3322`). For this XBE, src/dst are
fresh VRAM never bound as render targets, so the surface cache
stays empty and `pgraph_mtl_surface_blit_copy` returns Path C
at `surface.mm:3218-3222` without incrementing. The CPU memcpy at
`mtl/blit.c:215-221` is the load-bearing path and runs
unconditionally. **`METAL_IMAGE_BLITS = 0` does NOT mean the
blit failed to dispatch.** Next session: do not chase this
counter as a suspect.

### Manifest changes

`scripts/apple-silicon/xbe-tests/image-blit/manifest.json`:
- `expected_fail_renderers = ["metal"]` (was empty).
- `expected_fail_notes` field added with full per-cell verdict
  + pointer to decision-log cycle 11 entry.
- Title bumped to "(v0.2 — 8-cell rect/offset sweep, bounded
  partial Metal)".
- Purpose paragraph updated with channel 3/4 rationale.

Matrix runner now treats v0.2 as known-not-green on Metal rather
than a regression.

### Codex validation (rule #15)

v0.2 → MAJOR ISSUES (cycle 11):

1. **HIGH**: README/manifest were stale — still documented 9/11
   and "shipped cleanly". **Adopted**: README and manifest fully
   updated to reflect channel 3/4 + bounded-partial 3/8 PASS state.

2. **MEDIUM**: `expected_fail_renderers` empty; status doc
   mistakenly treated METAL_IMAGE_BLITS=0 as suspect when it is
   the expected steady state. **Adopted**: manifest set to
   `["metal"]`; claude-status (this file), README, and decision-log
   all now document the counter-zero expectation.

3. **OPEN QUESTION** (Codex): is the bug shared-blit-math
   (`mtl/blit.c:181-233` etc.) or `nv_clip_gpu_tile_blit`?
   **Deferred to next slice** — investigation list logged in the
   decision-log cycle 11 entry + this file's "Residual" section.

## Residual hypothesis (next bounded slice)

The 5/8 failure pattern looks like a shared-blit-path issue
(`mtl/blit.c:181-233`, mirrored in `gl/blit.c:123-187` and
`vk/blit.c:127-191`) or tile-limit clipping via
`nv_clip_gpu_tile_blit` (`nv2a.c:89-107`) against PFB tile
registers inherited from the chainloading UnleashX dashboard. The
XBE-side oracle (`main.c:262-378`) was inspected (this session)
and is internally consistent: the same `source_pixel_at()` drives
fill + verify; the inside-rect / outside-rect math correctly
re-derives the expected source pixel from (in_x, in_y, out_x,
out_y, x, y).

Investigation steps for cycle 12 (NOT executed this session):
1. Re-run image-blit on **GL** — if identical 5-cell pattern,
   bug is in shared blit copy/clip math; if different,
   Metal-specific.
2. Inspect `nv_clip_gpu_tile_blit` against the runtime PFB tile
   register state at blit time (UnleashX may have set up tiles).
3. Diff `mtl/blit.c` against `gl/blit.c` and `vk/blit.c`
   siblings for any drift.
4. Add a per-cell first-mismatch debug encode to the dashboard
   (e.g. encode the first mismatched (x, y) and got/expected
   into the FAIL cell color) so the offending pixel reveals
   itself in the captured PNG without needing host-side trace.

## Evidence preserved

- v0.1 crash run:
  `benchmark-runs/xbe-harness-20260522-071449/`
  (assertion at blit.c:170; 9/11 handles).
- MAXRAM-only attempt (still 9/11, still crashed):
  `benchmark-runs/xbe-harness-20260522-073448/`
  (proves the buffer-placement hypothesis was wrong).
- v0.2 standalone qemu trace:
  `/tmp/image-blit-trace/xemu.log`
  (proves sDmaObject9's PRAMIN limit was reprogrammed to 0x0012BFFF).
- v0.2 bounded-partial run:
  `benchmark-runs/xbe-harness-20260522-075241/`
  (3/8 PASS, signal_match_pct=37.5000).

## M15 default-on Gate 2 status (post-cycle 11)

- §E.13 per-format pitch + image-rect alignment — **MET** (cycle 10).
- §H.6 IMAGE_BLIT — **PARTIAL** (this cycle; 3/8 cells green).
- §G.5 Z compression boundary — still unstarted.
- RT-as-texture sampling XBE — still unstarted.

Second-wave coverage: **1 MET + 1 PARTIAL of 4**.
