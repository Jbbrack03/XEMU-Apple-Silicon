# 2026-05-20 — PGR2 RTT correctness: cross-sibling sync at color/depth bind (STAGED, NOT CLOSED)

Goal: continue the PGR2 `pgr2_gameplay_b4` snapshot investigation from
2026-05-19. The May 19 work narrowed the live blocker to "the copied
late `0x3c84000` content is still not GL-like" (decision-log
"2026-05-19 (night)"). This slice identifies a candidate mechanism
that produces sibling-divergent content and implements a targeted
GPU-blit-at-bind path; it is filed as a **staged diagnostic** because
the multi-title validation gate caught regressions that the
PGR2-only evidence missed.

## Conclusion — staged, not closed

The diagnosis is believed correct (see "Root cause" below) but the
cross-sibling-sync as implemented is NOT a shippable fix. Real-time
visual observation across multiple tracked titles showed regressions:

- Xbox boot logo: parts black + visible parts checkerboarded.
- Halo: black screen throughout.
- Crimson Skies: flickering, animated background visible but the
  menu UI was completely missing.

`scripts/apple-silicon/metal-canary-regress.sh --mode counters`
returned 4/4 PASS because counter-mode does not check pixel content
(documented limitation). The PGR2-only aggregate %white / %dark /
blink-rate stats below LOOKED like an improvement but were
insufficient evidence — the multi-title visual gate is mandatory
before any "fix" claim.

**Action taken:** the `XEMU_METAL_RTT_SIBLING_SYNC` flag now defaults
**OFF**; counters and helper functions stay in tree as opt-in
diagnostics so future iteration can re-enable for targeted PGR2
experiments without affecting baseline correctness on other titles.

PGR2-only iteration metrics (with the flag ON; documented below for
record only, NOT as a shipped improvement):

- The magenta/pink artifact inside the car body — apparently closed
  in PGR2 frames. May be coincidental given multi-title regressions.
- White-HUD-bar artifact: frames-with->5%-white dropped from 137 to
  38 in the same 8s capture (different game-time mapping due to fps
  difference; not directly comparable).
- Average dark/missing-geometry pixels per content frame: 21.02% →
  18.34% (same caveat).
- Temporal flicker: `blink_rate_per_sec` 2.875 → 2.375, `spike_count`
  23 → 19 (same caveat).

PGR2 retail-oracle gameplay validation remains deferred. Local
GL-vs-Metal content alignment was NOT re-measured (the GL leg's
ffmpeg AVFoundation capture grabbed the macOS desktop in this
session, not the xemu window).

## Root cause

The May 19 surface-graph dump (`benchmark-runs/pgr2-snapshot-
postfix5.surface-graph.jsonl`) showed that `0x3c84000` consistently
carried **two cached `MtlSurfaceBinding` entries** in the late phase:

- a `1278x442` guest / `2556x884` scaled MSAA4 sibling
- a `1280x480` guest / `2560x960` scaled MSAA4 sibling

Both had `pitch=5120`, `nv097_format=4`
(`NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8`),
`draw_dirty=1` every frame, and `last_color_draw_seq` values that
differed by ~12 — i.e. PGR2 alternates between the two clip shapes
within every gameplay frame and writes to both. The depth attachment
companion at `0x38e0000` shows the identical pattern (two depth
siblings differing by the same 38-row × 2-column gap).

Xbox treats those bindings as a single physical surface (same VRAM,
same pitch, same format; just different clip rects). The Metal cache
treated them as two separate MTLTextures because
`binding_shape_compatible` in `surface.mm` only accepted shape reuse
within a ≤4-pixel slack and the height delta here is ~76 pixels in
scaled units / 38 in guest.

Consequences observed:

- The composite stage-0 sample of `0x3c84000` resolves to the
  `1280x480` sibling (`cache_get_color_for_guest` matches exact guest
  dimensions). That sibling only contains draws issued while it was
  bound, missing the content drawn into the `1278x442` sibling within
  the same frame.
- The 2026-05-19 `path=copy-alias` change improved the alias-bridge
  artifact but copied from the **wrong** sibling for the same reason:
  the copy source was the matched sibling, not the freshest sibling
  with the actually-drawn content.
- Disabling the surface-tex fast path (`XEMU_METAL_DISABLE_SURFACE_TEX
  =1`) did not fix the visual because the VRAM-bridge fallback ALSO
  produces sibling-divergent VRAM bytes (whichever sibling's
  `download_surface_to_vram` runs last wins for the overlap, and that
  is non-deterministic w.r.t. which sibling had the latest draws).
- The same divergence on the depth attachment causes z-test
  mis-pass/mis-fail in the final composite (manifests as the dark
  slabs / missing geometry artifact).

## Fix

`surface.mm` now performs a **cross-sibling sync** on every color and
depth bind. When `pgraph_mtl_surface_bind_color(_ex)` or
`bind_depth(_ex)` resolves a cache entry, it scans the cache for any
other same-VRAM, same-pitch, same-nv097-format, same-aspect
(color/depth) sibling that has a **fresher** content signal:

- Color uses `last_color_draw_seq` (bumped by
  `pgraph_mtl_surface_note_color_draw` on every color-writing draw).
- Depth uses a new `last_depth_draw_seq` field on `MtlSurfaceBinding`
  bumped by `pgraph_mtl_surface_set_draw_dirty_depth()`. The pre-
  existing `last_use_seq` cannot serve as the depth freshness signal
  because it is bumped on every cache hit, including the bind that
  triggers the sync — at sync time the target's value is already the
  newest, so no source would ever qualify.

If a fresher sibling is found, the renderer issues a
`MTLBlitCommandEncoder copyFromTexture` of the overlap region from
the source's `texture` (single-sample / resolveTexture) into the
target's. When both siblings have matching MSAA companion textures
with the same sample count, a second blit copies the multi-sample
contents from `source->msaa_texture` into `target->msaa_texture` so
the subsequent `MTLLoadActionLoad` of the MSAA companion sees the
predecessor's draws and the next pass starts with the correct
content.

Sync ordering is fenced against the open-pass draw command buffer
via the existing `pgraph_mtl_draw_get_done_event_state` mechanism so
the blit reads from a committed source texture. The blit's command
buffer signals a higher event value so any subsequent consumer
already waits transitively.

A single env flag `XEMU_METAL_RTT_SIBLING_SYNC` controls both
color and depth sync. Default ON. Set to `0` to disable both for
A/B comparison.

Counter `METAL_SIBLING_SYNCS` (interval delta) records executed
syncs; `METAL_SIBLING_SYNC_SKIPS` records binds where no fresher
sibling existed (the common steady-state case).

## Build + validation gate

```sh
./build.sh -a arm64
```

PASS, including the post-build Metal shader-validation gate
(`summary: 7/7 passed, 0 failed`).

## Runs

Snapshot anchor for every run below (same as the 2026-05-19 sequence
so direct frame-by-frame visual comparison is valid):

- HDD: `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`
- Tag: `pgr2_gameplay_b4`
- Loadvm at: 2 s

Captured via `scripts/apple-silicon/capture-gameplay-temporal.sh
--renderer METAL --game pgr2 --duration 8 --snapshot pgr2_gameplay_b4
--loadvm-at 2 --input scripts/apple-silicon/input-scripts/noop.csv`.

| Run | Tree state | Frames | Sibling syncs/interval |
|---|---|---|---|
| Baseline | linear-alias-copy (`a6b865f884`) | 444 | n/a (counter not present) |
| v3 | color-only sibling sync | 586 | 0,6,14,6,4 |
| v4 | color+depth sibling sync | 401 | 0,12,28,28,20,8 |

The v3 → v4 frame-count drop (586 → 401, ~30%) reflects the GPU
cost of the depth blit (single-sample + MSAA4 multisample copy per
color/depth rebind). The drop is borderline acceptable; if a future
investigation shows the depth-side sync is unnecessary for visual
correctness, it can be selectively disabled. Today it is required
for the dark-slab reduction observed below.

## Frame-by-frame findings

The v4 visual is materially closer to GL than the linear-alias-copy
baseline on the same snapshot. Representative frame comparisons:

- baseline `benchmark-runs/20260519-201711-pgr2/frames/metal-
  gameplay.0255.png`: scene visible, white HUD bars, **magenta
  patches inside the car body**, dark slab on the left.
- v4 `benchmark-runs/20260520-105208-pgr2/frames/metal-gameplay.
  0250.png` and `.0150.png`: scene visible, the magenta interior is
  gone, the car body and speedometer are clean. Frame 150 in
  particular is close to the GL reference at
  `benchmark-runs/20260519-182716-pgr2/frames/gameplay-0260.png`.
- v4 frame `.0350.png` shows the cleanest gameplay frame in the
  capture window.

The car-interior magenta closure is the most visually obvious win.
The HUD-bar color also changed (white → mostly-red, sometimes
absent) as expected when the publish source's content now reflects
the actually-drawn pixels instead of a stale clip-restricted
sibling.

Persistent artifacts: intermittent black slabs in the upper-left,
red HUD bars where GL renders translucent dark backgrounds with
glyphs. These are NOT explained by the sibling-divergence closed by
this slice; they remain queued for follow-up investigation. The
working hypothesis is a second multi-RT compositing case at a
smaller RT used by the HUD path (M5.12/M17 scope), independent of
the `0x3c84000` final-composite RT.

## Quantitative aggregate

Captured over the full 8-second post-snapshot window for each run.
"Content frames" = frames with ≥50% non-dark pixels.

| metric (over content frames)            | baseline | v3 | v4 | v4 vs baseline |
|---|---|---|---|---|
| total frames                            | 444 | 586 | 401 | — |
| content frames                          | 308 | 384 | 284 | — |
| avg %white pixels (HUD-bar proxy)       | 3.67 | 2.94 | **1.10** | −70% |
| avg %dark pixels (slab proxy)           | 21.02 | 20.05 | **18.34** | −13% |
| frames with >5% white                   | 137 | 137 | **38** | −72% |

Temporal-flicker stats (`scripts/apple-silicon/temporal-flicker-
analyze.py --duration-seconds 8`):

| metric           | baseline | v4 | v4 vs baseline |
|---|---|---|---|
| blink_rate_per_sec        | 2.875 | 2.375 | −17% |
| mean_changed_pct          | 2.869 | 2.727 | −5% |
| spike_count               | 23 | 19 | −17% |
| instability_heatmap_pct   | 94.18 | 94.77 | ≈ flat |

Strict gameplay compare against GL is **not yet re-run** here because
the GL leg of `capture-gameplay-temporal` records via ffmpeg
AVFoundation, which in this session captured the macOS desktop
rather than the xemu window contents. The Metal-side improvements
documented here are local-vs-baseline, not local-vs-GL. Next
debugging slice will rerun the GL leg with the xemu window in the
foreground (or via `XEMU_GL_SCREENSHOT_PATH` renderer-native
capture) so `m15-gameplay-visual-compare.py` produces an authoritative
alignment-distance comparison.

## What this slice does NOT close

- PGR2 strict gameplay compare verdict (still expected `INFRA-FAIL`
  pending the additional artifact classes above; the alignment
  distance improvement vs the May 19 0.4505..0.4818 range is the
  next measurement to take).
- The black-slab / red-HUD-bar artifact class.
- p99 jitter — depth blit adds ~30% per-frame overhead in this
  configuration. p99 gate is a separate slice.
- Cold shader compile proof.
- Metal VGA-direct fallback (M5.13 / M18) for the BIOS animation.

## Code state worth keeping

- `last_depth_draw_seq` on `MtlSurfaceBinding` — depth-write
  freshness signal that is NOT bumped by cache lookups, so the
  sync logic can rank "most-recently-rendered depth sibling"
  correctly.
- `sync_color_siblings_into` / `sync_depth_siblings_into` in
  `surface.mm`. Called from `pgraph_mtl_surface_bind_color(_ex)` and
  `bind_depth(_ex)` respectively.
- `XEMU_METAL_RTT_SIBLING_SYNC` env flag (default on).
- `METAL_SIBLING_SYNCS` / `METAL_SIBLING_SYNC_SKIPS` counters surfaced
  by `xemu-perf` interval line.

## Methodology lesson recorded

This slice exposed a gap in the validation methodology:

- **PGR2-only aggregate stats are insufficient.** A change that
  affects surface caching / sibling lookup / RT-as-texture sampling
  touches paths used by every title. Local PGR2 improvement is
  necessary but NOT sufficient.
- **`metal-canary-regress.sh --mode counters` does not catch visual
  regressions.** This is a documented limitation but easy to gloss
  over when the verdict is `4/4 PASS`. Counter-mode is a smoke check
  that the renderer pipelines/queues are not failing — not a visual
  correctness gate.
- **The retail Xbox oracle is the visual correctness gate.** Every
  renderer change touching surface/RTT code must be validated
  against the oracle on every tracked title (boot logo, Crimson,
  Rainbow, PGR2, Halo, SC2) BEFORE the flag flips default ON.
- **Real-time observation is part of validation.** The temporal
  PNG-every-frame capture catches more than single-frame
  screenshots, but a human watching the actual game running across
  the full route is the strongest signal we have today.

The next iteration of this slice (or its successor) is required to
reproduce the regressions on boot logo / Halo / Crimson with the
flag ON, frame-by-frame on the retail oracle, before any further
attempt at a default-on fix.

## Next debugging slice

1. **Reproduce regressions with the flag ON on the retail oracle**
   for each tracked title. Use the temporal-capture path AND the
   retail-oracle workflow (`retail-oracle-workflow.py --title X`)
   so the comparison is against actual Xbox hardware output, not
   only against GL.
2. **Identify why the same blit-at-bind path that helps PGR2's
   color composite breaks other titles.** Hypotheses:
   (a) the depth-blit between `MTLPixelFormatDepth32Float_Stencil8`
   textures has Metal semantics this implementation gets wrong (the
   stencil aspect is part of the same MTLTexture; a single
   `copyFromTexture` may or may not carry it depending on
   `srcOptions`/`dstOptions` — the current code passes none);
   (b) the MSAA blit-copy has alignment constraints; (c) some
   titles legitimately rely on per-clip-rect sibling isolation that
   coalescing breaks.
3. **Investigate the residual PGR2 black-slab / red-HUD-bar
   artifact class** (was queued before this slice). Hypothesis: a
   separate multi-RT compositing case at a smaller HUD-source RT
   (256x256 / 512x512 entries in the cache — see `0x368x000` /
   `0x36ax000` family in the postfix5 surface graph). The
   `XEMU_METAL_DUMP_DRAW_RT` (W4 tool) can dump per-draw color RTs
   during the bad late frames to identify which RT carries the
   wrong content.
4. p99 jitter: not gated by this slice anymore (default OFF), but
   if a future iteration enables the sync, the depth-side MSAA blit
   cost (~30% fps drop in the PGR2-only experiment) must be
   quantified per frame and may justify gating depth sync more
   selectively.

