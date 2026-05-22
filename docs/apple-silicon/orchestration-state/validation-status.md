# Validation Status

- Current slice: cycle 11 §H.6 IMAGE_BLIT XBE (image-blit v0.2).
- Validation state: **Bounded partial — 3 of 8 cells PASS
  byte-correct on Metal; 5 of 8 FAIL. Original assertion crash
  resolved.**
- Repo baseline: working tree on commit `bd27467eca` (cycle 10
  closure) + new diag XBE directory + doc-sync diffs.
- Slice scope: ship the §H.6 IMAGE_BLIT XBE; reuse `xbed_lib`
  4x2-grid pattern; do not touch shared lib or renderer source.

## Gate status (M15 default-on)

- **Gate 1 — first-wave XBE saturation:** **MET.** 17 PASS on
  Metal + 1 expected_fail SPEC (unchanged this cycle).
- **Gate 2 — second-wave retail-implicated feature coverage:**
  **1 MET + 1 PARTIAL of 4** (E.13 met cycle 10; H.6 partial this
  cycle; G.5 + RT-as-texture still unstarted).
- **Gate 3 — retail-title canary re-verification after XBE-library
  green:** **NOT MET.** Blocked on Gate 2 (1 PARTIAL + 2 unstarted
  remaining).
- **Gate 4 — no correctness bug ≥30 days:** **MET.**
- **Codex validation:** v0.2 raised MAJOR ISSUES (stale README +
  manifest + claude-status drift around METAL_IMAGE_BLITS=0
  expectation). All three findings adopted in-session.
- **Doc sync:** **COMPLETE** in this cycle.

## Compare statistics (Metal, v0.2)

- `changed_pixels_pct = 62.7083` (≫ 3.0 gate — bounded partial)
- `signal_match_pct = 37.5000` (≪ 97.0 gate — 3 of 8 cells green)
- captured frame: `image-blit.0124.png`
- best_frame_count = 278
- evidence:
  `benchmark-runs/xbe-harness-20260522-075241/image-blit/metal/`

## Per-cell verdict (Metal, v0.2)

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

## Original v0.1 failure (now resolved)

Crash at `hw/xbox/nv2a/pgraph/mtl/blit.c:170`
(`source_offset < source_dma_len`) on the FIRST IMAGE_BLIT.
Confirmed via qemu `-trace events=nv2a_dma_map` that
`pb_init → pb_target_back_buffer → set_draw_buffer()` reprograms
PRAMIN for `sDmaObject9` and `sDmaObject11` to point at the back
buffer with `limit = height * pitch - 1` (= 0x0012BFFF). Channels
9 and 11 are pbkit-reserved scratch DMAs, NOT general-purpose RAM.
v0.2 switches to handles 3 and 4 (untouched by `set_draw_buffer`,
both base=0 / Limit=MAXRAM). Crash is gone in v0.2.

## Residual (next slice)

5/8 failure pattern looks like shared-blit-path bug
(`mtl/blit.c:181-233` mirrored in gl/vk siblings) or tile-limit
clipping via `nv_clip_gpu_tile_blit` (`nv2a.c:89-107`) against
PFB tile registers inherited from UnleashX. XBE-side oracle
(`main.c:262-378`) inspected and internally consistent.
Investigation checklist in decision-log cycle 11 entry +
claude-status §Residual hypothesis.

## Known open items

- M15 default-on remains gated on the residual §H.6 cells turning
  green + the remaining 2 of 4 Gate 2 slices + Gate 3 retail
  re-verification.
- task #17 (GL LOD-clamp regression) remains outside this gate.
- Real-Xbox reference promotion for image-blit deferred until the
  next hardware bring-up window.

## Cycle exit verdict

**Option B — bounded partial.** §H.6 IMAGE_BLIT XBE shipped with
the channel 9/11 → 3/4 root cause fully grounded (qemu trace
evidence preserved). 3/8 cells PASS byte-correct on Metal. 5/8
documented as expected_fail in manifest with the next-session
investigation list logged in decision-log cycle 11.
