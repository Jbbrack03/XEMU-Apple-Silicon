# Handoff Summary

- Cycle: 2026-05-22, cycle 11 (§H.6 IMAGE_BLIT XBE — second
  second-wave Gate 2 slice).
- Owner: Claude Code (resumed bounded session).
- Outcome: **Exit Option B — bounded partial closed cleanly.**

## What shipped

### `xbe-tests/image-blit/` v0.2

Tier-2 diagnostic XBE covering §H.6 in
`nv2a-feature-surface-research.md`. 4x2 grid, 8 cells, single
LE_A8R8G8B8 color format. NV062 + NV09F per-cell `(in_x, in_y,
out_x, out_y, width, height)` sweep. CPU-side per-pixel oracle
compares dst VRAM (via `pb_agp_access`) against the math-derived
expected source pixel; per-cell verdict encoded as a 160×240
green/red dashboard cell.

### Root-cause fix (channels 9/11 → 3/4)

Original v0.1 routed `NV062_SET_CONTEXT_DMA_IMAGE_SOURCE/DESTIN`
through pbkit handles **9** and **11** (carry-forward
assumption). On Metal that asserted at
`hw/xbox/nv2a/pgraph/mtl/blit.c:170` because
`pb_init → pb_target_back_buffer → set_draw_buffer()`
(`nxdk/lib/pbkit/pbkit.c:1611-1668, 3260`) reprograms PRAMIN for
those channels to point at the back buffer with
`limit = height * pitch - 1` (= 0x0012BFFF for 640×480
LE_A8R8G8B8). Verified empirically with qemu
`-trace events=nv2a_dma_map`: the sDmaObject9 entry shows
`addr=0x03BD4000 limit=0x0012BFFF` immediately before the crash.

v0.2 switches to handles **3** and **4** — pbkit creates them
with `base=0, Limit=MAXRAM` at `pbkit.c:2643,2645` and never
reprograms them after `pb_init` returns. Channels 9 and 11 are
pbkit-reserved scratch DMAs for the back/front buffer aperture,
NOT general-purpose RAM channels after `pb_init`. NV062 does not
validate the DMA channel class, so handles 3 (CLASS_3D) and 4
(CLASS_3) are both legal NV062 source/dest channels.

v0.2 also conservatively replaces `HighestAcceptableAddress =
0x3FFB000` with `MAXRAM` in the XBE's three
`MmAllocateContiguousMemoryEx` sites (matches pbkit pattern at
`pbkit.c:2297-2305`). This was a disproved hypothesis from
earlier this cycle but is kept because it's strictly conservative.

### Verdict on Metal (v0.2)

**Bounded partial — 3 of 8 cells PASS byte-correct; 5 fail.**

- `changed_pixels_pct = 62.7083` (≫ 3.0 gate)
- `signal_match_pct = 37.5000` (≪ 97.0 gate)
- captured frame: `image-blit.0124.png`
- evidence: `benchmark-runs/xbe-harness-20260522-075241/image-blit/metal/`

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

Manifest declares `expected_fail_renderers = ["metal"]` with the
full notes pointing at the decision-log cycle 11 entry.

### Important: METAL_IMAGE_BLITS=0 is expected for this XBE

The counter only increments after the OPTIONAL GPU-side cached-
texture copy path (`surface.mm:3322`). For this XBE — which uses
never-rendered VRAM buffers — the surface cache stays empty so
`pgraph_mtl_surface_blit_copy` returns Path C at
`surface.mm:3218-3222` without incrementing. The CPU memcpy at
`mtl/blit.c:215-221` is the load-bearing path and runs
unconditionally. Do NOT chase this counter as a suspect in cycle
12.

### M15 default-on Gate 2

| §  | XBE | status (post-cycle 11) |
|----|-----|------------------------|
| E.13 | per-format pitch + image-rect alignment | **MET** (cycle 10) |
| H.6  | IMAGE_BLIT | **PARTIAL** — 3/8 cells green (this cycle) |
| G.5  | Z compression boundary | unstarted |
| —    | RT-as-texture sampling (PGR2 late-stage-0) | unstarted |

Gate 2 reads **1 MET + 1 PARTIAL of 4**. M15 default-on overall
remains **NOT MET** pending §H.6 full close + the remaining two
slices + Gate 3 retail re-verification.

## Why the next slice could be §H.6 finalize OR §G.5

§H.6 finalize is the cheapest continuation — the failure pattern
is already narrowed (5 specific cells, all width != 8 OR width = 8
but `(in_x, in_y) > (4, 4)`). The investigation list in
decision-log cycle 11 entry plus claude-status §Residual hypothesis
gives the next session a concrete starting point.

§G.5 (Z compression boundary) is the next strictly-unstarted Gate 2
slice and could proceed in parallel.

## Validation / review state

- Codex validation on v0.2: **MAJOR ISSUES** (stale README +
  manifest + claude-status drift around the METAL_IMAGE_BLITS=0
  expectation). All three findings adopted in-session:
  README + manifest fully updated to reflect channel 3/4 + 3/8
  PASS state; `expected_fail_renderers = ["metal"]` set;
  counter-zero expectation documented in README, manifest notes,
  and claude-status.
- Codex marker for the v0.2 fingerprint will be written before
  commit per rule #15.
- Doc sync: COMPLETE (handoff.md cycle 11 banner; decision-log.md
  cycle 11 entry; orchestration-state including this file +
  claude-status + current-cycle + validation-status; image-blit
  README + manifest).

## Residual investigation for cycle 12

1. Re-run image-blit on **GL** — if identical 5-cell pattern, bug
   is in shared blit copy/clip math (`gl/blit.c:123-187`,
   `mtl/blit.c:181-233`, `vk/blit.c:127-191`); if different,
   Metal-specific.
2. Inspect `nv_clip_gpu_tile_blit` (`nv2a.c:89-107`) against the
   runtime PFB tile register state at blit time. UnleashX may
   have set up tiles whose limits intersect our dst buffer
   allocations.
3. Diff `mtl/blit.c` against `gl/`+`vk/` siblings for any
   accidental drift since slice M5.9-followup-A.
4. Add a per-cell first-mismatch debug encode in the FAIL cell
   color so the residual mismatch pixel reveals itself in the
   captured PNG without requiring host-side trace.

## Why this is shipped as bounded partial (Option B)

- Original v0.1 blocker (assertion crash) is fully resolved with
  qemu-trace-grounded root cause + targeted fix.
- The 5-cell residual failure mode is NOT yet grounded; multiple
  plausible hypotheses (shared blit path, tile clipping, XBE
  oracle) each merit a focused investigation in a separate
  bounded slice.
- Committing now lets the channel 9/11 → 3/4 finding land
  durably (decision-log + handoff + manifest) and unblocks
  parallel work on §G.5 / RT-as-texture if cycle 12 doesn't
  immediately tackle §H.6 finalize.
- Manifest declares Metal `expected_fail` with explicit notes so
  the matrix runner treats this as known-not-green rather than
  a regression.
