# image-blit

Tier-2 diagnostic XBE covering §H.6 (`NV_IMAGE_BLIT` class 0x9F +
`NV_CONTEXT_SURFACES_2D` class 0x62, SRCCOPY operation) in
`docs/apple-silicon/nv2a-feature-surface-research.md`. Second slice
of the second-wave M15-default-on Gate-2 coverage list (after
`texture-pitch-alignment` shipped 2026-05-22 cycle 10).

**Current status (v0.3, 2026-05-22 cycle 12): bounded partial,
root cause materially narrowed.** Same 3-of-8 PASS verdict on
Metal as v0.2, but cycle 12 added a per-cell first-mismatch
diagnostic encoding (4-quad sub-rect layout per cell) that
decoded the residual: got = sentinel `0xff808080`, expected =
RED `0xffff0000`, first mismatch at `(mx=0, my=0)` for every
FAIL cell. A transient xemu-side fprintf in
`hw/xbox/nv2a/pgraph/mtl/blit.c` (since reverted) proved that
the **renderer's CPU memcpy writes the correct RED bytes for
ALL 8 cells**. A cached-read control experiment (oracle reading
via `s_dst_vram[idx]` directly instead of `pb_agp_access`) made
the result strictly WORSE — only cell 4 PASSed. Conclusion: the
residual 5-cell mismatch is a **guest-side CPU cache coherency
issue on the VRAM read-back path**, not a renderer bug. See the
"Status" section + decision-log cycle 12 entry for evidence.

## What it tests

8 `NV_IMAGE_BLIT` calls, one per dashboard cell. Single source
buffer (32×32 LE_A8R8G8B8 in VRAM, CPU-painted with four 16×16
quadrants: RED top-left, GREEN top-right, BLUE bottom-left, WHITE
bottom-right). Eight independent destination buffers (64×64 each,
sentinel-gray `0xFF808080` pre-fill). Per-cell `(in_x, in_y, out_x,
out_y, width, height)` sweep:

| Cell | In(x,y) | Out(x,y) | W×H   | Probes                                  |
|------|---------|----------|-------|-----------------------------------------|
| 0    | (0,0)   | (0,0)    | 8×8   | smallest contiguous rect (RED only)     |
| 1    | (0,0)   | (0,0)    | 16×16 | full RED quadrant                       |
| 2    | (0,0)   | (0,0)    | 32×32 | all 4 quadrants                         |
| 3    | (8,8)   | (0,0)    | 8×8   | `in_x` / `in_y` honored (RED only)      |
| 4    | (0,0)   | (4,4)    | 8×8   | `out_x` / `out_y` honored               |
| 5    | (4,4)   | (8,8)    | 8×8   | both offsets non-zero (RED only)        |
| 6    | (0,0)   | (0,0)    | 1×16  | degenerate width (single-pixel column)  |
| 7    | (0,0)   | (0,0)    | 16×1  | degenerate height (single-pixel row)    |

After each blit, the XBE calls `pb_wait_until_gr_not_busy()`, reads
destination VRAM via `pb_agp_access()` (cache-coherent linear
remap), and verifies every pixel inside the declared destination
rect equals the corresponding source pixel (SRCCOPY semantics) and
every pixel outside the rect remains sentinel gray.

The per-cell verdict is encoded as a 160×240 dashboard rectangle:

- PASS: solid green `0xFF00FF00`.
- FAIL (v0.3): a 2×2 sub-rect with TL = solid red FAIL banner,
  TR = GOT pixel color, BL = EXPECTED pixel color, BR = encoded
  first-mismatch `(mx, my)`. See "Cycle-12 v0.3 diagnostic
  encoding" below for the decoder. (v0.2 historical: a single
  solid red `0xFFFF0000` rectangle.)

## Expected output

Math-derived (when all 8 blits PASS):

```
Row 0 (y 0..239):    GREEN GREEN GREEN GREEN
Row 1 (y 240..479):  GREEN GREEN GREEN GREEN
```

Pure 0/255 RGB components survive display gamma byte-exact (same
property `texture-format-sweep` / `texture-pitch-alignment` rely
on for their cube-corner palettes).

A single failed blit turns one 160×240 cell from green to red and
trips both the `max_changed_pct = 3.0` gate (12.5% of frame changed)
and the `min_signal_match_pct = 97.0` gate.

## What it catches

- `in_x` / `in_y` ignored: cell 3 / cell 5 read the wrong source
  quadrant → dst pixels mismatch RED → FAIL.
- `out_x` / `out_y` ignored: cell 4 / cell 5 write the wrong dst
  position → sentinel remains where rect should be / RED appears
  where sentinel should remain → FAIL.
- `width` / `height` ignored or rounded: cells 6 / 7 (single-pixel
  dimensions) catch any "round to ≥1 row/column" regression by
  showing RED in pixels that should be sentinel, or sentinel where
  RED should be.
- SRCCOPY broken (e.g. silently blending, alpha-patching, masking):
  every cell's pixel oracle catches a byte difference immediately.
- `LE_A8R8G8B8` byte layout broken: cells 1 / 2 show wrong colors.
- Complete IMAGE_BLIT failure: all 8 cells stay sentinel → all 8
  oracle checks FAIL → all 8 cells render red.

## What it does NOT catch (deferred / separate XBEs)

- `BLEND_AND` / non-`SRCCOPY` operations (Beta-channel modulation).
- `LE_R5G6B5` / `LE_Y8` / `LE_X8R8G8B8_Z8R8G8B8` color formats.
- Source / destination overlap inside one buffer.
- GPU-to-GPU MTLTexture-cache propagation: this XBE intentionally
  uses never-rendered VRAM buffers so the `pgraph_mtl_image_blit`
  CPU-memcpy fast path (`mtl/blit.c:217`) is the path under test.
  A separate slice would cover the cached-surface propagation
  path (`pgraph_mtl_surface_blit_copy`, `mtl/blit.c:279`).

## Push sequence (per cell)

```
SUBCH_4 (NV_CONTEXT_SURFACES_2D 0x62):
  SET_OBJECT                 = 17   (pbkit sGrObject17, class 0x62)
  SET_CONTEXT_DMA_IMAGE_SOURCE = 3   (pbkit sDmaObject3; base=0, Limit=MAXRAM)
  SET_CONTEXT_DMA_IMAGE_DESTIN = 4   (pbkit sDmaObject4; base=0, Limit=MAXRAM)
  SET_COLOR_FORMAT           = LE_A8R8G8B8 (0x0A)
  SET_PITCH                  = src_pitch | (dst_pitch << 16)
  SET_OFFSET_SOURCE          = phys(source_buffer)
  SET_OFFSET_DESTIN          = phys(dest_buffer[cell])

SUBCH_3 (NV_IMAGE_BLIT 0x9F):
  SET_OBJECT                 = 16   (pbkit sGrObject16, class 0x9F)
  SET_CONTEXT_SURFACES       = 17   (resolves to same instance as NV062 SET_OBJECT)
  SET_OPERATION              = SRCCOPY (3)
  CONTROL_POINT_IN           = in_x  | (in_y  << 16)
  CONTROL_POINT_OUT          = out_x | (out_y << 16)
  SIZE                       = width | (height << 16)   <-- triggers blit
```

Every SET_OBJECT (method 0) and context-DMA method (`[0x180, 0x200)`)
is RAMHT-resolved by PFIFO (`pfifo.c:572`). pbkit defaults
`NV_PFIFO_RAMHT_SIZE = 0` (4 KiB table) so the lookup hash must
satisfy `hash * 8 < 4096`. Large D3D-runtime handles
(`0x14d00 / 0x14d10 / 0x11120 / 0x11170`, taken from
`benchmark-runs/20260430-141331-flat-tri-trace/xemu-trace.log`)
overflow that bound and trip the `pfifo.c:578` assertion. We instead
point at pbkit's pre-registered small ChannelIDs (3/4/16/17), which
hash within range; channels 3/4 carry base=0, Limit=MAXRAM and
pbkit leaves them untouched after `pb_init` returns. **Channels 9
and 11 are intentionally NOT used** — see `nv2a_regs_image_blit.h`
for the full rationale and the `set_draw_buffer()` reprogram trap.

`pgraph_mtl_image_blit` (`hw/xbox/nv2a/pgraph/mtl/blit.c:93`) asserts
`context_surfaces->object_instance == image_blit->context_surfaces`
(line 144). After RAMHT lookup both fields receive `entry.instance`;
routing both NV062_SET_OBJECT and NV09F_SET_CONTEXT_SURFACES through
handle 17 makes them equal.

## Tier-2 contract

Per `diagnostic-xbe-plan.md` §2.1, Tier-2 XBEs use guest-side VRAM
readback as their authoritative oracle. The host-side capture
(green/red dashboard) is the visible projection of the per-cell
CPU oracle into a form the host harness can compare. The CPU
oracle is load-bearing; the dashboard is bookkeeping.

The Metal renderer's `pgraph_mtl_image_blit` performs a CPU-side
`memcpy` of guest VRAM (`mtl/blit.c:217`) regardless of whether the
src/dst buffers are also surface-cached as MTLTextures. Because
this XBE allocates fresh VRAM buffers that are never used as
render targets, the surface cache never holds entries for them and
the CPU memcpy is the only path that writes dst VRAM. That makes
the test independent of the `XEMU_METAL_FRONT_FB_DOWNLOAD` flag
(Tier-2 prerequisite for surface-cached XBEs per §2.1).

**Note on `METAL_IMAGE_BLITS` counter:** the counter increments
only on the GPU-side cached-texture copy path
(`pgraph_mtl_surface_blit_copy`, `surface.mm:3322`). For this XBE,
which uses never-rendered VRAM buffers, the surface cache is empty
for src/dst so Path C returns false (`surface.mm:3218-3222`)
without incrementing the counter. `METAL_IMAGE_BLITS = 0` is the
EXPECTED steady-state value for this XBE and does NOT indicate the
blit failed to dispatch. The CPU memcpy at `mtl/blit.c:215-221`
is the load-bearing path and runs unconditionally.

## Build + run

```sh
# build (Mac-side):
cd scripts/apple-silicon/xbe-tests/image-blit
make

# run on xemu-Metal via the production harness (explicit --xbe flag
# because Tier-2 XBEs are excluded from the default `run` filter):
scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
    --xbe image-blit --renderer metal
```

## Status

**v0.3 — BOUNDED PARTIAL on Metal (2026-05-22 cycle 12).** Same
per-cell verdict as v0.2 (3 of 8 PASS, 5 FAIL), but the residual
root cause is now materially narrowed by the cycle-12
first-mismatch diagnostic encoding + cached-read control
experiment + xemu-side per-blit fprintf.

### Cycle-12 v0.3 diagnostic encoding

When a cell FAILs, its dashboard cell is now split into a 2×2
sub-rect layout instead of solid red:

- **TL (red)**: FAIL banner — still solid red, retains the
  visual gestalt of "this cell failed."
- **TR**: GOT pixel value as a solid color — the exact ARGB
  the oracle saw at `dst[mx, my]`.
- **BL**: EXPECTED pixel value as a solid color — what the
  math-derived oracle expected.
- **BR**: `(mx, my)` encoded as a pure ARGB color where R and G
  are bucket-of-32 (`R = (mx & 7) * 32`, `G = (my & 7) * 32`;
  values in `{0, 32, 64, …, 224}` so Apple GL-on-Metal gamma
  cannot corrupt the low 3 bits), and B packs the upper 3 bits
  of mx/my as a nibble pair (`B = ((mx >> 3) << 4) | (my >> 3)`;
  values up to `0x33` for mx, my ∈ [0, 31]). Decoder:
  `mx = R / 32 + ((B >> 4) & 7) * 8`,
  `my = G / 32 + (B & 7) * 8`.

Frame 0124 of
`benchmark-runs/xbe-harness-20260522-090124/image-blit/metal/
screenshots/` decodes to: every FAIL cell shows
TR=`0xff808080` (sentinel), BL=`0xffff0000` (red),
BR=`(mx=0, my=0)`. So the FIRST byte of the dst (offset 0) is
sentinel where it should be red.

### Cycle-12 xemu fprintf evidence (since reverted)

A transient `fprintf(stderr, "xemu-perf: image_blit_cell …")` in
`hw/xbox/nv2a/pgraph/mtl/blit.c` logged the local
`source_offset` / `dest_offset` / `dest_size` / `clipped_dest_size`
/ `adjusted_height` / `leftover_bytes` per blit invocation, plus
the pre/post first-pixel dword at `dest_row`. Evidence in
`benchmark-runs/xbe-harness-20260522-090729/image-blit/metal/
xemu.log` shows:

- All eight cell blits fire.
- No tile clipping engages (`clipped == dest_size` for every
  cell).
- `dst_pre = 0xff808080, dst_post = 0xffff0000` for every cell —
  the host-side memcpy writes the correct RED bytes at the right
  host pointer.

### Cycle-12 cached-read control experiment

A v0.3 variant flipped `oracle_check_cell` to read via the
cached virtual pointer (`s_dst_vram[idx]`) instead of
`pb_agp_access(s_dst_vram[idx])`. Frame 0124 of
`benchmark-runs/xbe-harness-20260522-091452/` decodes to: only
cell 4 PASSes; cells 0/1/2/3/5/6/7 all FAIL with the same
"first mismatch at (0,0), got=sentinel, expected=RED" signature.
The cached read is **strictly less fresh** than the AGP-aliased
read — direct evidence that the residual lies in the guest CPU's
read-back cache coherency, not in the renderer.

### Disproved cycle-11 hypotheses

- Shared blit math bug (`mtl/blit.c:181-233` etc.) — DISPROVED.
- Tile-limit clipping via `nv_clip_gpu_tile_blit` — DISPROVED.
- Surface-cache download corruption — DISPROVED.

### v0.2 historical context preserved below.

Original v0.1 routed the NV062 source/destin DMA via pbkit handles
9 and 11. On Metal that asserted at
`hw/xbox/nv2a/pgraph/mtl/blit.c:170`
(`source_offset < source_dma_len`) on the FIRST blit because
pbkit's `pb_init` → `pb_target_back_buffer()` → `set_draw_buffer()`
(`nxdk/lib/pbkit/pbkit.c:1611-1668`, called at `pbkit.c:3260`)
**reprograms** PRAMIN for channels 9 and 11 to point at the back
buffer with `limit = height*pitch-1` (= 0x0012BFFF for 640×480
LE_A8R8G8B8). Verified empirically with
`-trace events=nv2a_dma_map`: sDmaObject9's instance entry shows
`addr=0x03BD4000 limit=0x0012BFFF` (not `addr=0, limit=MAXRAM`).

v0.2 switches `IMAGE_BLIT_DMA_HANDLE_SRC = 3` and
`IMAGE_BLIT_DMA_HANDLE_DST = 4`. pbkit creates these with
`base=0, Limit=MAXRAM` at `pbkit.c:2643,2645` and never reprograms
them after `pb_init` returns. The assertion is **resolved** —
xemu no longer crashes when the XBE runs.

After the fix the dashboard renders 3 of 8 cells PASS on Metal:

| Cell | In(x,y) | Out(x,y) | W×H   | Verdict (Metal, v0.2)        |
|------|---------|----------|-------|------------------------------|
| 0    | (0,0)   | (0,0)    | 8×8   | **PASS** (solid green)        |
| 1    | (0,0)   | (0,0)    | 16×16 | FAIL (v0.3: 2×2 diag layout) |
| 2    | (0,0)   | (0,0)    | 32×32 | FAIL (v0.3: 2×2 diag layout) |
| 3    | (8,8)   | (0,0)    | 8×8   | FAIL (v0.3: 2×2 diag layout) |
| 4    | (0,0)   | (4,4)    | 8×8   | **PASS** (solid green)        |
| 5    | (4,4)   | (8,8)    | 8×8   | **PASS** (solid green)        |
| 6    | (0,0)   | (0,0)    | 1×16  | FAIL (v0.3: 2×2 diag layout) |
| 7    | (0,0)   | (0,0)    | 16×1  | FAIL (v0.3: 2×2 diag layout) |

Best frame:
`benchmark-runs/xbe-harness-20260522-075241/image-blit/metal/screenshots/image-blit.0124.png`.
`signal_match_pct = 37.5000`, `changed_pixels_pct = 62.7083`.
PASS cells all have `width == height == 8` AND
`(in_x, in_y) ≤ (4, 4)`. FAIL cells fall outside that envelope.

The manifest declares Metal `expected_fail` for this slice so the
matrix runner treats the partial verdict as known-not-green rather
than a regression.

**Residual hypothesis (next bounded slice):** the 5/8 failure
pattern looks like a shared-blit path bug (`mtl/blit.c:181-233`,
also `gl/blit.c:123-187` and `vk/blit.c:127-191` by mirror), or
tile-limit clipping via `nv_clip_gpu_tile_blit` (`nv2a.c:89-107`)
against PFB tile registers inherited from UnleashX. The XBE-side
oracle (`main.c:262-378`) was inspected and looks internally
consistent — the same `source_pixel_at()` drives both the source
paint and the per-pixel verify, so an XBE-side bug would not
explain why some cells pass byte-exact. Next session should:
1. re-run the same XBE on GL to see if the failure pattern is
   identical (proves shared-blit-code bug) or different (proves
   Metal-specific);
2. inspect `nv_clip_gpu_tile_blit` against the runtime PFB tile
   state at blit time;
3. diff blit.c against gl/vk siblings.

Real-Xbox reference promotion deferred to the same hardware
bring-up window as the other Tier-1/2 XBEs.
