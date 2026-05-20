# 2026-05-11 — PGR2 Metal Render-Path Diagnostic (Capture-Source Ruled Out)

> Update 2026-05-19 late evening: this note still stands as the point where
> capture-source error was ruled out, but the active next-step framing is now
> narrower. See `2026-05-19-pgr2-snapshot-publish-and-rtt-followup.md`: keep
> the host-refresh publish preservation fix, reject the `0x3b58000`
> display-shape heuristic, and treat late stage-0 `0x3c84000` RTT sampling as
> the live blocker.

Goal: Resolve handoff "Next engineering steps" #1 — decide whether
`XEMU_METAL_SCREENSHOT_SOURCE=nv2a` is sampling the wrong published
texture or whether live Metal rendering is missing the PGR2 profile/menu
background. Then form a falsifiable next-step hypothesis.

## Conclusion

The capture source is **not** the bug. Metal is genuinely failing to
present the PGR2 profile/menu background imagery. The CRTC-pointed
surface holds stale boot-state contents, and the dominant-draw fallback
surface holds non-image data when sampled. The current
`XEMU_METAL_FRONT_FB_FALLBACK=1` mechanism cannot bridge PGR2's
profile-screen scene rendering on its own.

PGR2 paired gameplay visual parity remains FAIL. This established that the
problem was deeper than screenshot-source selection. The later 2026-05-19
follow-up further narrowed the active fix from general "multi-RT compositing
investigation" to RTT/render-target-as-texture correctness around late
stage-0 sampling of `0x3c84000`.

## Method

Three sequential single-capture runs, identical env to the failed
2026-05-11 gameplay-evidence attempt except for `XEMU_METAL_SCREENSHOT_SOURCE`:

| Run | Source                       | Out dir                                                                |
|-----|------------------------------|------------------------------------------------------------------------|
| A   | `drawable` (composited)      | `benchmark-runs/m15-gameplay-pgr2-drawable-20260511-202348/metal/`     |
| B   | `vram:0x32a4000` (CRTC)      | `benchmark-runs/m15-pgr2-vramdump-32a4000-20260511-203756/metal/`      |
| C   | `vram:0x3628000` (wide RT)   | `benchmark-runs/m15-pgr2-vramdump-3628000-20260511-204029/metal/`      |

Each run: 120s or 90s PGR2 with `pgr2-gameplay.csv` route,
`XEMU_RENDERER=METAL XEMU_METAL_TRANSLATED_PIPELINE=1
XEMU_METAL_FRONT_FB_FALLBACK=1 XEMU_METAL_MSAA=4`, opt-in flags on.
Screenshot every 60 frames. Capture environment identical across runs
so frame-to-frame correspondence is meaningful.

Reference: GL strict-window capture at `benchmark-runs/20260511-182317-pgr2/screenshots/`.

## Findings

### A (drawable) — frame 50 == B (NV2A) — frame 50

Both Metal captures at the same input-route position show the
PROFILESELECT UI overlay on a flat-gray background, identical content.
Identical means the capture source is not where information is lost —
the rendered/composited scene genuinely does not contain the cityscape
GL renders.

This rules out the "wrong texture sampled by `nv2a` source" hypothesis
from the prior session.

### vram:0x32a4000 (CRTC-pointed front-fb)

`benchmark-runs/m15-pgr2-vramdump-32a4000-20260511-203756/metal/vram32a4.0035.png`
shows fuchsia/magenta with an upside-down "Microsoft" logo across the
entire run. The contents do not change frame-to-frame.

Interpretation: PGR2 binds 0x32a4000 only briefly during boot (the
`metal_draw_target` interval logs show ~30–38 draws per interval going
there, vs thousands going elsewhere), then abandons it. The texture
retains the late-boot state. CRTC-strict publish would always show
this; the front-fb fallback was added precisely to bypass it.

The Y-mirrored "Microsoft" text is a separate suspected issue
(NV2A Y-axis vs Metal Y-axis convention during boot-time draws) and is
out of scope for this diagnostic.

### vram:0x3628000 (wide 1280×480 back-buffer)

`benchmark-runs/m15-pgr2-vramdump-3628000-20260511-204029/metal/vram3628.0035.png`
shows tiled horizontal banding with red/teal/magenta colour noise and
no coherent image structure.

Interpretation: either (a) the surface contains data that doesn't
correspond to a normal RGBA image (raw memory, multiple sub-buffers
side-by-side, intermediate post-process content), or (b) the surface
format/swizzle in the Metal texture doesn't match how the scene was
written.

Either way: 0x3628000 is not the "final composited image" surface that
the display compositor could publish to produce the PGR2 background.

## Surface map (from `metal_color_bind` + `metal_draw_target_first`)

| vram_addr   | Guest dims   | Format        | Draws/interval | Role hypothesis              |
|-------------|--------------|---------------|----------------|-------------------------------|
| 0x32a4000   | 640×480      | 8 (A8R8G8B8)  | ~30–38         | CRTC-pointed, abandoned       |
| 0x3628000   | 1280×480     | 8 (A8R8G8B8)  | ~8K–14K        | Wide intermediate RT          |
| 0x2c06000   | 1024×512     | 8 (A8R8G8B8)  | ~6K–9K         | Intermediate RT               |
| 0x2e06000   | 1024×512     | 8 (A8R8G8B8)  | ~6K–7K         | Intermediate RT               |
| 0x2454000   | 1024×1024    | 8 (A8R8G8B8)  | tracked        | Large RT (env-map?)           |
| 0x3c84000   | 640×480      | 4 (X8R8G8B8)  | tracked        | Possible final back-buffer    |
| 0x3b58000   | 640×480      | 4 (X8R8G8B8)  | tracked        | Possible final back-buffer    |
| 0x2854…0x2994 | 256×256 ×6 | 8 (A8R8G8B8)  | ~433/interval ea | Sprite RTTs                |
| 0x37f0000…  | 128×32, 64×16 | 8 (A8R8G8B8) | tracked       | Tiny RTs (HUD elements?)      |
| vram_addr=0 | 512×512      | 3 (R5G6B5)    | ~12K–15K       | Mystery R5G6B5 RT (likely an animated/skybox source) |

The 640×480 format-4 surfaces (0x3c84000 / 0x3b58000) are plausible
candidates for the real final composite back-buffer. The current
fallback mechanism selects whichever surface has the highest per-frame
draw count, which biases toward 0x3628000 / vram_addr=0; the format-4
surfaces are not picked even though they may carry the composite.

## Perf evidence (Metal drawable run)

`benchmark-runs/20260511-202348-pgr2/perf-summary.txt`:

- `post_load_avg_fps=49.51` (intervals=117)
- `METAL_DRAW_COUNT=285,803` (100% translated, 0 fallbacks)
- `METAL_PIPELINE_TRANSLATED_FAILED=0`, `METAL_PIPELINE_FALLBACKS=0`
- `METAL_TEX_CACHE_HITS=234,701`, `METAL_TEX_CACHE_MISSES=346`
- `METAL_FRONT_FB_PUBLISHES=5,723` (= per-frame publish OK)
- `METAL_SURFACE_VRAM_UPLOADS=20`, `METAL_SURFACE_VRAM_DIRTY_HITS=4`
- `METAL_DRAW_PASS_COALESCED=273,579 / METAL_DRAW_PASS_OPENS=12,224`
  (95.7 % coalesced — M5.7 healthy)

Renderer plumbing is green; the failure is at the higher-level
"which surface contains the title's intended scene" question, not at
shader translation, texture upload, or pipeline build.

## Falsifiable hypothesis recorded for future Metal work

PGR2's profile-screen background composition uses a multi-RT pipeline
on the original NV2A: render-to-texture into one or more intermediate
surfaces, then a final draw or blit step composes them into a 640×480
format-4 surface (0x3c84000 / 0x3b58000) that the CRTC scans. The
current Metal renderer:

1. Binds and renders to each intermediate surface (eight+ distinct
   vram_addrs observed).
2. Has no awareness of the final-composite step — neither which
   surface receives the composited image nor which earlier
   intermediates need to be sampled as textures during that step.
3. Falls back to publishing whichever surface has the highest
   per-frame draw count, which is an intermediate RT, not the
   final composite.

Possible fixes (deferred):

- Track NV097_IMAGE_BLIT to find where the final composite goes
  (`METAL_IMAGE_BLITS=0` in this run — the blit op is not firing
  for this title, so the composite is probably a draw, not a blit).
- Detect the 640×480 format-4 surface 0x3c84000 / 0x3b58000 by
  shape-match and prefer it for publish over wider/larger RTs.
- Implement a "publish-by-CRTC-shape-match" mode that looks for a
  cached surface whose dimensions equal the display dimensions
  (640×480) rather than the CRTC vram_addr or dominant-draw count.
- Investigate whether the surface-as-texture path correctly aliases
  intermediate RTs when the title samples them in the final-composite
  draw — `texture.mm:294` returns NULL for vram_addr=0 in the
  texture cache, but the surface-as-texture fast path at
  texture_pg.c:1183 should handle this; verify whether
  `has_compatible_surface` is true for the affected stages.

These need careful design — the wrong heuristic would regress titles
that are currently green under the dominant-draw fallback.

## Status against M15 bundle

The bundle moved through two intermediate states during this session:

1. **After the m15-bundle-status.py discovery extension** (intermediate,
   superseded later in same session by step 2):
   `verdict=incomplete ok=5 fail=5 missing=5` — the MISSING for PGR2
   gameplay was correctly surfaced as FAIL via the new discovery.
2. **After the front-fb fallback policy decision-log entry**
   ("2026-05-11 (evening 2)") + the
   `m15-bundle-status.py` decision-log marker check:
   `verdict=incomplete ok=6 fail=5 missing=4` — one MISSING item
   (policy) converted to OK (resolved as opt-in pending multi-RT
   compositing fix).

State (2) is the final same-session result. `fail=5` reflects:

- Crimson paired diff visual (existing)
- PGR2 / Rainbow / Crimson p99 jitter (existing)
- PGR2 paired gameplay visual now correctly surfaced as FAIL (was
  MISSING before the discovery fix)

`missing=4` items are: Rainbow / SC2 / Halo paired gameplay diffs and
cold shader compile proof.

## Project rule compliance

- Rule #1 (no guessing): every conclusion rests on a captured image or
  a logged counter from `benchmark-runs/`.
- Rule #11 (no re-validation of closed flags): the eight default-on
  Apple Silicon flags are not touched; the diagnostic uses
  `XEMU_METAL_FRONT_FB_FALLBACK` and `XEMU_METAL_SCREENSHOT_SOURCE`
  which are diagnostic/opt-in flags.
- Rule #4 (no doc drift): this note and the matching decision-log
  entry land together; handoff.md will be reconciled in the same
  session.
