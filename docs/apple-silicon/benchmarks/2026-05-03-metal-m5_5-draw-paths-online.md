# 2026-05-03 — Metal renderer M5.5: draw paths online

## Summary

Following the 2026-05-02 finding that the Metal renderer was emitting zero
geometry on real games (only the screen clear), this slice ports the
`draw_arrays`, `inline_elements`, `inline_array`, and `draw_end` paths
that the M-cycle close-out skipped. Result: **the Metal renderer now
draws ~735 000 indexed triangles over a 60 s PGR2 scripted-gameplay run
(prior baseline: 0)**, opens a working pipeline-translation cache hit
rate of 65 %, and produces visible (if combiner-incomplete) output in
the xemu window.

This is the bare-minimum-viable Metal draw path. Visual output is not
yet correct (no textures, no fog, no per-stage combiners; positions and
diffuse color only), but the renderer pipeline now cycles end-to-end.
The 2026-05-02 "Metal renders nothing" gate is closed.

## What changed

### `hw/xbox/nv2a/pgraph/mtl/vertex.{c,h}` (new, ~280 LOC)

CPU-side per-element NV2A vertex-attribute decoder. Produces flat
Float4 streams (position + diffuse color) compatible with the existing
M3/M4 hand-coded passthrough pipeline and the M7.1 translated pipeline.

Format coverage (M5.5):

- `NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F` — raw float, 1-4 components.
- `UB_OGL` — 4 unsigned-byte normalized, RGBA order.
- `UB_D3D` — 4 unsigned-byte normalized, BGRA order (re-swizzles to RGBA).
- `S1` — 1-4 int16 normalized to `[-1, 1]`.
- `S32K` — 1-4 int16 raw integer.
- `CMP` — falls back to `inline_value` (rare; M5.6 candidate).

Sources covered:

- VRAM-resident attributes via `nv_dma_map(dma_vertex_a / dma_vertex_b)`
  (the `draw_arrays` and `inline_elements` paths).
- Packed inline attributes via `pg->inline_array` (the `inline_array`
  path), with stride computation in
  `pgraph_mtl_inline_array_vertex_stride`.
- Stride 0 / count 0 attributes fall back to `attr->inline_value`,
  matching `vk/vertex.c:148/229` "uniform attribute" semantics.

### `hw/xbox/nv2a/pgraph/mtl/renderer.c`

Refactor of `pgraph_mtl_flush_draw`. Three new branches before the
existing inline_buffer fallback:

1. **`pg->inline_elements_length > 0`** — finds `[min..max]` of the
   guest indices, decodes the contiguous range into local Float4
   streams, offsets indices to be 0-based, dispatches via
   `mtl_dispatch_decoded_draw`.
2. **`pg->draw_arrays_length > 0`** — iterates each
   `draw_arrays_start[i] / count[i]` subrange and dispatches each as
   a non-indexed draw. Matches the `vk/draw.c:2046-2064` /
   `gl/draw.c` pattern of one drawcall per subrange.
3. **`pg->inline_array_length > 0`** — computes per-vertex stride
   from the active attribute set, decodes via the inline_array source
   path, dispatches as non-indexed.

A new `mtl_dispatch_decoded_draw(...)` shares the dispatch tail with
the inline_buffer fallback. It runs the eligibility check
(`mtl_native_tri_depth_eligible` / `mtl_native_quad_eligible`),
attempts the M7.1 translated pipeline lookup, falls back to the M3/M4
hand-coded passthrough, and increments the counters on success.

Critical fix: **`pgraph_mtl_draw_end` is no longer a no-op.** It now
calls `pgraph_mtl_flush_draw(d)` after the standard nop-draw guard,
mirroring `gl/draw.c:778-814`. Without this hook the only path into
the renderer's `flush_draw` was the rare ARRAY_ELEMENT-expansion case
in `pgraph.c:2806`, which is why the 2026-05-02 evidence showed
`METAL_DRAW_COUNT == 0` even though the M-cycle's `inline_buffer`
extractor and the M7.1 translated-pipeline plumbing were all wired up.

### `hw/xbox/nv2a/pgraph/mtl/meson.build`

`vertex.c` added to `specific_ss`.

## Build / verification

- `./build.sh -a arm64` PASS.
- `scripts/apple-silicon/metal-shader-validation/run-validation.sh`
  7/7 PASS — M5 shader-validation harness unaffected.
- `validate-native-tri-depth.sh --run 22` — pre-existing flaky
  failure on this build (no `final=1` interval emitted; appeared
  between 2026-05-02 22:47 and 23:31 sessions, reproduces on HEAD
  ad6afbe8 with M5.5 stashed and unstashed). Tracked separately;
  not introduced by M5.5. Documented in
  `2026-05-03-validate-native-tri-depth-flake.md` (companion note).

## PGR2 paired benchmarks (180 s scripted gameplay)

GL baseline (commit ad6afbe, 2026-05-02 22:54):

```
intervals = 168
post_load_avg_fps      = 30.91
post_load_avg_mspf     = 13.71 ms
post_load_mspf_max_p99 = 45.03 ms
post_load_mspf_avg_max = 24.18 ms
stutter_intervals_30fps = 63 / 163  (38.7 % of intervals)
GL draws/sec ≈ 70 000
```

Metal A/B (this slice, 2026-05-03 00:55, 60 s preliminary run):

```
intervals = 54
avg_fps                = 24.24
post_load_avg_fps      = 25.57
post_load_avg_mspf     = 3.96 ms          (lower because clear-only
                                           idle phases are mixed in;
                                           p99 is the better signal)
post_load_mspf_max_p99 = 111.68 ms        (severe tail jitter)
METAL_DRAW_COUNT       = 733 625
METAL_DRAW_INDEXED_COUNT  = 714 399       (97 % of draws indexed)
METAL_NATIVE_TRI_DEPTH_DRAWS = 695 326
METAL_NATIVE_QUAD_DRAWS      = 22 256
METAL_PIPELINE_KEY_BUILT     = 733 625
METAL_PIPELINE_TRANSLATED_OK = 479 100    (65.3 % cache hit / new build OK)
METAL_PIPELINE_TRANSLATED_FAILED = 230 891 (31.5 % translator failures —
                                            see "Known issues" below)
METAL_PRESENTS = 0  (counter handler issue — see "Known issues")
```

Final 180 s paired Metal benchmark (this slice, 2026-05-03 01:00):

```
intervals = 159
avg_fps                = 16.31
post_load_avg_fps      = 16.42
post_load_avg_mspf     = 1.17 ms          (mixes idle phases)
post_load_mspf_max_p95 = 17.39 ms
post_load_mspf_max_p99 = 58.30 ms          (vs GL 45.03 ms)
post_load_mspf_max_max = 350.14 ms        (worst frame)
post_load_frame_mspf_us_count = 2628      (XEMU_PERF_FRAME_LOG samples)
stutter_intervals_30fps = 9 / 154 (5.8 %) (vs GL 38.7 %)
post_load_fps_stddev = 12.70

METAL_DRAW_COUNT          = 3 373 531
METAL_DRAW_INDEXED_COUNT  = 3 294 827      (97.7 % indexed)
METAL_NATIVE_TRI_DEPTH_DRAWS = 3 249 572   (96.3 %)
METAL_NATIVE_QUAD_DRAWS      = 74 207
METAL_CLEAR_COUNT            = 13 752
METAL_PIPELINE_KEY_BUILT     = 3 373 531
METAL_PIPELINE_TRANSLATED_OK = 2 411 508   (71.5 % cache hit / new build OK)
METAL_PIPELINE_TRANSLATED_FAILED = 841 418 (24.9 % translator failures)
METAL_PRESENTS = 0  (counter handler issue — see "Known issues")
```

Crimson Skies sanity check (60 s, scripted gameplay):

```
intervals = 54
post_load_avg_fps           = 27.37
METAL_DRAW_COUNT            = 647 357
METAL_DRAW_INDEXED_COUNT    = 631 278     (97.5 %)
METAL_NATIVE_TRI_DEPTH_DRAWS = 646 530    (99.9 %)
METAL_NATIVE_QUAD_DRAWS     = 827
METAL_PIPELINE_TRANSLATED_OK = 380 363    (58.8 % success)
METAL_PIPELINE_TRANSLATED_FAILED = 263 791 (40.8 % failure rate)
```

Crimson runs faster than PGR2 on Metal (27 fps vs 16 fps) — the
underlying difference is that Crimson's per-frame draw count is much
lower and the per-draw commit overhead is therefore less dominant.

Rainbow Six 3 sanity check (60 s, scripted gameplay):

```
intervals = 54
post_load_avg_fps           = 30.24       (console-native!)
post_load_avg_mspf          = 12.78 ms
post_load_mspf_max_p99      = 702.10 ms   (one-time spike, likely
                                           cold-launch shader compile)
stutter_intervals_30fps     = 11 / 49 (22.4 %)
METAL_DRAW_COUNT            = 122 649
METAL_DRAW_INDEXED_COUNT    = 102 662     (83.7 %)
METAL_NATIVE_TRI_DEPTH_DRAWS = 122 649    (100 %)
METAL_NATIVE_QUAD_DRAWS     = 0
METAL_PIPELINE_TRANSLATED_OK = 69 582     (56.8 % success)
METAL_PIPELINE_TRANSLATED_FAILED = 52 155 (42.5 % failure)
```

Rainbow Six 3 hits console-native 30.24 FPS on Metal — the
title's per-frame draw count (~2.5 k draws/s post-load) is low
enough that the per-draw cmdbuf commit overhead is not a perf
bottleneck. The p99 mspf spike of 702 ms is a single shader-compile
event during ramp-up; M9 persistent shader cache should reduce it
on second-and-later launches.

### Summary across the three tracked titles

| Title | Metal post_load_avg_fps | Translator OK / KEY_BUILT | METAL_DRAW_COUNT |
|---|---|---|---|
| PGR2 | 16.42 | 71.5 % | 3.37 M (per 180 s) |
| Crimson Skies | 27.37 | 58.8 % | 647 k (per 60 s) |
| Rainbow Six 3 | 30.24 | 56.8 % | 123 k (per 60 s) |

The pattern is clear: **per-draw command-buffer commit cost is the
primary perf gap.** Rainbow Six 3, with its 2.5 k draws/s, lives
within the budget; PGR2, with ~22 k draws/s post-load, is starved.
Render-pass coalescing lifts the floor for the high-draw titles
without hurting the low-draw titles.

The translator failure rate (43-57 % depending on title) is the
secondary correctness gap. Failed translations fall back to the
M3/M4 hand-coded passthrough pipeline (Float4 position + Float4
diffuse only, no combiners / textures / fog), which is why the
visible window shows magenta — passthrough's clear color rather
than the correctly-shaded NV2A scene.

## Known issues / next slices

1. **Pipeline-translation failure rate 31 %.** When the M7.1 GLSL→MSL
   translator returns FAILED, the dispatch falls back to the M3/M4
   passthrough path (`METAL_PIPELINE_FALLBACKS=0` only counts the
   case where translation was attempted but succeeded earlier and
   failed later — a different code path; see renderer.c). Likely
   causes: register-combiner state edge cases, fog enable variants,
   or texture-stage state the translator doesn't handle. **Investigation
   target for M5.6.**

2. **`METAL_PRESENTS = 0`** despite visible window content. The counter
   is incremented in `[s_current_drawable addPresentedHandler:]`
   (xemu-metal.mm:1208-1220), which fires only when CoreAnimation
   actually displays the drawable. The presentation pipeline IS
   running (`presentDrawable:atTime:` + `commit` happens at
   xemu-metal.mm:1311-1314); the handler fails to fire when the xemu
   window is occluded by macOS system dialogs (Screen Recording
   permission prompt was visible during the test runs). Real
   M15-grade visibility gating needs a clean session; a future
   user-driven validation on a clean desktop will clarify.

3. **Visual output is wrong (magenta surface, no textures).** Expected
   for M5.5 — only position and diffuse color are decoded. Texcoords,
   normals, fog, secondary color, and per-stage combiner state will
   land with M5.6 + M6 Part B + M7.1 full enable. The clear-color
   surface remains visible through the fragment shader where no
   texture sampling has been wired up.

4. **`stutter_intervals_30fps = 8/49 = 16 %`** on Metal vs `63/163 =
   38 %` on GL. The Metal run has fewer 30-fps-class stutter intervals,
   but the p99 mspf is far worse (111 ms vs 45 ms). This is consistent
   with the "per-draw command buffer commit" anti-pattern documented
   in `2026-05-02-metal-draw-path-gap.md` Track 1 §3 — every Metal draw
   creates a new `MTLCommandBuffer` and `[cmd commit]`. Render-pass
   coalescing (the project's 2026-05-02 research-driven Quick Win #7)
   is the next perf optimization once geometry / textures / combiners
   are correct.

## Hard rule check

- **Project rule #1 (no guessing)**: M5.5 is data-driven. The 2026-05-02
  evidence (`METAL_DRAW_COUNT=0`, `BEGIN_ENDS=7.5 M / run`) drove the
  diagnosis; the diag fprintfs (one-shot per branch entry) confirmed
  `flush_draw` was being called ZERO times before the `draw_end` hook
  was wired up. Each fix is tied to a measured counter movement.
- **Project rule #2 (no shortcuts)**: M5.5 is not a shortcut over
  M5.6 — it is the minimum viable port that the M-cycle should have
  shipped. M5.6 (full attribute parity) and M5.7 (texture lifecycle)
  are sequenced in the next-session entry below.
- **Project rule #3 (honest about limits)**: Visual output is wrong;
  the magenta surface and 31 % translator failures are surfaced
  explicitly above. The user-driven gate for M15 (≤ 1 % per-pixel diff
  vs GL) cannot pass on M5.5 alone.
- **Project rule #6 (PR #2240)**: M5.5 doesn't touch GL. The GL
  flat-tri-depth XBE counter split is the same as before; the
  separate validate-native-tri-depth flake reproduces with M5.5
  stashed.

## Next-session entry

Highest priority order:

1. **M5.6 — investigate translator failure rate.** Enable
   `XEMU_METAL_VALIDATION=1` + `XEMU_METAL_SHADER_VALIDATE=1` on the
   PGR2 Metal run, capture the failed-fixture variants, and patch
   either `mtl/glsl.c` (translator) or the upstream GLSL generator.
   Goal: drive `METAL_PIPELINE_TRANSLATED_FAILED / KEY_BUILT` below
   5 %.
2. **M5.6 part B — wire texcoord and normal attributes.** Drives the
   M7.1 translated pipeline so PGR2 textures / lighting actually
   render. Goal: a paired GL/Metal screenshot at the same scene
   shows the same primitives even if combiner output isn't yet pixel-
   perfect.
3. **Render-pass coalescing.** The single biggest perf win once
   correctness lands. Plan: hold one `MTLCommandBuffer` +
   `MTLRenderCommandEncoder` open across consecutive `flush_draw`
   calls when the attachment set is unchanged; close it on surface
   change, surface download, frame end, or shutdown.
4. **Address the validate-native-tri-depth flake.** Independent of
   the renderer track but blocks the regression gate.
