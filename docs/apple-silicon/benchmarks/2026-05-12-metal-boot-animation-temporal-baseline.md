# 2026-05-12 — Metal Boot Animation Temporal Baseline (User Report Reproduced)

Goal: Convert the user's verbal observation ("the Metal renderer renders
the Xbox boot animation as just green blobs") into reproducible
quantitative evidence, decide whether the existing single-frame canary
methodology can be trusted for M15 default-on gating, and produce a
flicker-aware analysis tool that the rest of the M15 evidence path can
adopt.

## Conclusion

The user's report is **directly reproduced** and is much worse than the
PASS canaries in the M15 bundle suggest. The Metal renderer in all three
tested configurations fails to display the Xbox BIOS boot animation:

- `XEMU_METAL_FRONT_FB_FALLBACK=0` + `XEMU_METAL_SCREENSHOT_SOURCE=nv2a`
  (the default canary-style capture): **97% of 1066 frames are solid
  magenta** with a longest-stable-run of 1036 frames. The BIOS
  animation is invisible.
- `XEMU_METAL_FRONT_FB_FALLBACK=0` + `XEMU_METAL_SCREENSHOT_SOURCE=drawable`
  (what the user actually sees when running Metal without the fallback):
  **66% of 1070 frames are solid magenta**. Black-to-magenta is the
  entire visible behavior.
- `XEMU_METAL_FRONT_FB_FALLBACK=1` + `XEMU_METAL_SCREENSHOT_SOURCE=drawable`
  (the **M15 eval recipe** in `metal-renderer-plan.md`, and what the user
  is running when they describe "green blobs"): renders chaotic green
  fragments and dotted noise where the Xbox logo should be. Mean adjacent-
  frame `changed_pct` is 1.70 (vs 0.81 GL), spike count is 19 (vs 1 GL),
  blink rate is 1.06/sec (vs 0.06/sec GL) — **17× the temporal
  instability of the GL reference**.

GL renders the boot animation correctly: green orb → Xbox device with
rotating ring → black "XBOX" title → flat-tri-depth diagnostic XBE.

The single-frame canary methodology (PGR2 f900 PASS, Rainbow f600 PASS,
Halo f1200 PASS, Crimson 90s sustained-30 FPS PASS) is **provably
insufficient** to validate Metal renderer health. Single-frame sampling
can land on a moment where the Metal output happens to contain the
correct frame (or contains a deceptively plausible green fragment that
matches the expected color palette of an Xbox boot screen) while the
surrounding 60+ frames are magenta, blank, or noise.

## What this means for M15

`m15-bundle-status.py` currently reports `verdict=incomplete ok=6 fail=5
missing=4`. The `ok=6` block includes the static MSAA4 canary PASSes for
PGR2/Rainbow/Halo/Crimson. **Those PASSes are not evidence of renderer
correctness** — they are evidence that at one specific flip ordinal, a
single frame did not differ catastrophically from a hand-picked GL
reference taken at the same flip ordinal. The temporal data immediately
adjacent to that flip ordinal is uninspected.

The M15 default-on criterion in `metal-renderer-plan.md` §4 says "5
distinct titles render at ≥ console-native FPS via Metal with ≤ 1%
per-pixel diff vs GL." The methodology required to back that criterion
must include temporal sampling, not just point-in-time sampling. This
benchmark proposes that the gameplay-evidence path
(`m15-gameplay-visual-compare.py`) AND the canary path be augmented with
the new `temporal-flicker-analyze.py` tool before any M15 default-on flip
is allowed.

## Method

Three sequential 18-second Metal boot captures plus one GL reference,
each with PNG-every-frame output (Metal via renderer-native
`XEMU_METAL_SCREENSHOT_PATH/AT_FRAME=1/INTERVAL=1`, GL via parallel
`ffmpeg avfoundation` capture of the macOS desktop at 60 fps).

| Run                         | Renderer | SOURCE   | FRONT_FB_FALLBACK | Frames | Dir                                                     |
|-----------------------------|----------|----------|-------------------|-------:|---------------------------------------------------------|
| A (Metal default capture)   | METAL    | nv2a     | 0                 | 1066   | `benchmark-runs/20260512T200701Z-boot-metal-temporal/`  |
| B (Metal user-visible)      | METAL    | drawable | 0                 | 1070   | `benchmark-runs/20260512T200800Z-boot-metal-drawable/`  |
| C (Metal M15 eval recipe)   | METAL    | drawable | 1                 | 1065   | `benchmark-runs/20260512T201000Z-boot-metal-frontfb/`   |
| D (GL reference)            | GL       | (window) | (n/a)             | 1033   | `benchmark-runs/20260512T200844Z-boot-gl-temporal/`     |

Workload: launches xemu with no game disc selected (Test_Games
flat-tri-depth.iso as placeholder media — disc is irrelevant during
BIOS animation). Capture window covers the first 18 s, which on real
hardware is approximately 8 s BIOS animation + ~5 s dashboard transition
+ ~5 s of flat-tri-depth.xbe.

Common environment: `XEMU_RENDERER` toggled per leg,
`XEMU_NATIVE_TRI_DEPTH=1`, `XEMU_NATIVE_QUAD=1`, `XEMU_PGRAPH_FAST_READ=1`,
`XEMU_METAL_MSAA=4`, `XEMU_METAL_TRANSLATED_PIPELINE=1` (Metal legs only),
`XEMU_PERF_LOG=1`, `XEMU_PERF_FRAME_LOG=1`, surface_scale=2.

## Headline metrics (from `temporal-flicker-analyze.py`)

Captured at `benchmark-runs/20260512T201100Z-boot-temporal-analysis/`.
Spike threshold = adjacent-frame `changed_pct >= 35%` OR `MAE >= 20`.

| Metric                                     | A: metal nv2a | B: metal drawable | C: **metal frontfb** | D: GL  |
|--------------------------------------------|--------------:|------------------:|---------------------:|-------:|
| mean adj-frame `changed_pct`               | 0.09          | 0.09              | **1.70**             | 0.81   |
| mean adj-frame MAE                         | 0.16          | 0.16              | 0.77                 | 0.25   |
| spike count over 18 s                      | 1             | 1                 | **19**               | 1      |
| blink rate per sec                         | 0.06          | 0.06              | **1.06**             | 0.06   |
| solid frame count                          | 1036          | 709               | 1                    | 0      |
| longest stable run (consec solid frames)   | 1036          | 709               | 1                    | 0      |
| solid color breakdown                      | `magenta:1036`| `magenta:709`     | `black:1`            | `{}`   |

GL produces zero solid frames; its instability stems from the orderly
BIOS animation (orb growth, ring rotation, logo reveal). Metal-nv2a and
Metal-drawable both saturate to solid magenta (the fuchsia init-color of
the CRTC-pointed front-fb / drawable when nothing is published — same
class of failure as the PGR2 `vram:0x32a4000` finding documented in
`2026-05-11-pgr2-metal-render-path-diagnostic.md`). Metal-frontfb is the
"green blobs" case the user described — content does appear, but it is
noisy, fragmented, and 17× more temporally unstable than GL.

## Side-by-side storyboards

- GL storyboard:
  `benchmark-runs/20260512T201100Z-boot-temporal-analysis/gl/storyboard-leg.jpg`
- Metal frontfb storyboard:
  `benchmark-runs/20260512T201100Z-boot-temporal-analysis/metal-frontfb/storyboard-leg.jpg`
- Metal nv2a heat map (uniform red across the boot region — every pixel
  swings from black to magenta exactly once):
  `benchmark-runs/20260512T201100Z-boot-temporal-analysis/metal-temporal/heatmap-leg.png`

## Why the existing canary PASSes were never going to catch this

`metal-canary-regress.sh --mode counters` checks `METAL_DRAW_COUNT > 0`,
`METAL_PIPELINE_TRANSLATED_FAILED == 0`, `METAL_DRAWABLE_ACQUIRE_FAILS ==
0`, etc. **All of those passed during the Metal-A capture** — see
`benchmark-runs/20260512T200701Z-boot-metal-temporal/xemu.log`:

```
METAL_DRAW_COUNT=534 METAL_PIPELINE_TRANSLATED_OK=534
METAL_PIPELINE_FALLBACKS=0 METAL_DRAWABLE_ACQUIRE_FAILS=0
METAL_PRESENTS=562 METAL_FRONT_FB_PUBLISHES=5
```

The renderer thinks it is healthy. 562 presents fired. 534 draws
succeeded. Only `METAL_FRONT_FB_PUBLISHES=5` hints that the rendered
content is not reaching display. The `metal-canary-regress.sh
--mode counters` gate at `metal-canary-regress.sh:456` does check
`METAL_FRONT_FB_PUBLISHES > 0`, but **the threshold is too weak**:
5 publishes over an 18 s capture (~558 vblanks delivered) trivially
passes `> 0` while still leaving 99% of frames missing publish.
The gate needs a minimum-publish-rate (publishes / interval-seconds,
or publishes / METAL_PRESENTS) threshold rather than the current
binary `> 0` check.

The MSAA4 PNG canary at PGR2 f900 (`benchmark-runs/20260504-100458-pgr2/
visual-checks/pgr2-gate-metal-msaa4-f900-after-msaa-store.png`) passed
because at flip ordinal 900 the title's CRTC publish happened to coincide
with the screenshot trigger. The frames before and after were not
inspected.

## New tooling shipped this session

### `scripts/apple-silicon/capture-boot-temporal.sh`

Boot-animation-specific PNG-every-frame capture harness. Takes
`--renderer GL|METAL`, manages scratch HDD, launches xemu directly
(no scripted input), captures frames per renderer's native every-frame
path. macOS UNIX-socket path length limit (104 bytes) is honored by
parking the QMP socket under `/tmp/` with a symlink in the run-dir for
traceability.

### `scripts/apple-silicon/temporal-flicker-analyze.py`

Standalone PNG-sequence analyzer. Single-leg or paired Metal-vs-GL.
Emits:

- Per-leg `summary.json`: mean adjacent-frame changed_pct, MAE, spike
  count, blink rate, solid-frame breakdown by color, longest stable run,
  per-pixel instability heat map percentage.
- Heat map PNG (red-orange channel mapped to per-pixel stddev).
- Storyboard JPG (8 evenly spread + 2 worst-spike frames).
- Blink-reel directory (top-N spike frames preserved at full resolution).
- Human-readable `report.md`.

Crop semantics support asymmetric `--metal-crop`/`--gl-crop` for the
common case where Metal renderer-native captures are 1280×960 (NV2A
surface) and GL ffmpeg AVFoundation captures are 2560×1440 (full desktop).

## Outstanding hypothesis to falsify next

The fuchsia/magenta in Metal-A and Metal-B matches the
`vram:0x32a4000` (CRTC-pointed surface) findings from the PGR2 multi-RT
diagnostic. Confirming hypothesis: Metal initializes the front-fb /
drawable to the fuchsia clear color (or it inherits an earlier-cleared
texture), and most NV2A draws land on intermediate RTs that are never
composited to the display path because Metal cannot identify which
intermediate RT holds the "final" content. The "M5.12/M17 future Metal
slice" already exists in the decision log for PGR2 — this benchmark
extends the scope: the same bug class also breaks the **BIOS boot
animation**, which has no PGR2-specific multi-RT pipeline and is far
simpler than a 3D racing game. Whatever the renderer is missing, it is
missing it at a fundamental NV2A composition level, not at a
game-specific surface layout level.

## Followups

1. **Methodology change**: add `temporal-flicker-analyze.py` to the M15
   gameplay evidence path. `m15-bundle-status.py` should consume the
   per-leg `summary.json` and refuse to PASS any title whose Metal
   `blink_rate_per_sec` is more than 2× the GL reference.
2. **Apply the analyzer to the four canary titles**. Crimson is the
   cheapest first target (existing 90 s gameplay route + Metal-vs-GL
   paired harness).
3. **Investigate the magenta init-color**. Likely an
   `MTLRenderPassDescriptor.colorAttachments[0].clearColor` set to
   fuchsia at first-frame init; if the front-fb publish path never
   over-writes that, every "no publish" frame leaks the clear color.
4. **Demote the static MSAA4 canary PASSes** in the M15 gate to "smoke
   smoke" status. They remain useful as a fast first-line check but no
   longer constitute renderer-correctness evidence.
