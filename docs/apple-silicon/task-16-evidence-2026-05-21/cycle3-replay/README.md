# Task #16 — cycle 3 replay evidence (2026-05-21 evening, Hermes cycle 3)

Hermes-supervised continuation of cycle 2 (`../` parent directory). This
cycle resolves the **render-target ambiguity** the cycle 2 banner left
open and replays the `0x032a4000` front-buffer GLSL dump that cycle 2
lost.

## Tooling delta this cycle

`hw/xbox/nv2a/pgraph/mtl/renderer.c::mtl_dispatch_decoded_draw` gains
one env-gated `metal_dispatch_draw_target` diag line per dispatch when
`XEMU_METAL_DIAG_ATTRIB_DUMP=1` AND `pg->vertex_attributes[9].stride
== 44` (same gate as the cycle-2 `metal_set_attr_masks` diag), cap 32
lines, zero impact when the env is unset. Line shape:

```
xemu-perf: metal_dispatch_draw_target color_addr=0xCCCCCCCC depth_addr=0xDDDDDDDD \
  uniform_attrs=0xfdf6 vcount=N icount=M prim=P color_fmt=0xF depth_fmt=0xF \
  v0=1 v3=1 v9=1 native_tri=1 native_quad=0
```

Pairs one-to-one with the existing `metal_set_attr_masks
uniform_attrs=0xfdf6 ...` line because they share the same gate and cap.

Documented in `automation.md` "Diagnostic Toggles" and
`.claude/rules/flags-renderer.md`.

## Reproduction

```sh
# A: capture per-dispatch draw-target attribution + back-buffer GLSL dumps
XEMU_METAL_DIAG_ATTRIB_DUMP=1 \
XEMU_METAL_DUMP_TARGET_SHADER=stride44 \
XEMU_METAL_DUMP_TARGET_SHADER_DIR=docs/apple-silicon/task-16-evidence-2026-05-21/cycle3-replay/glsl-dumps \
python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
  --xbe swizzle-mipmap --renderer metal \
  --out docs/apple-silicon/task-16-evidence-2026-05-21/cycle3-replay/harness-out \
  --surface-scale 1 --no-upload

# B: capture front-buffer 0x032a4000 GLSL dump + publish stream
XEMU_METAL_DIAG_ATTRIB_DUMP=1 \
XEMU_METAL_DIAG_PUBLISH=1 \
XEMU_METAL_DUMP_TARGET_SHADER=0x032a4000 \
XEMU_METAL_DUMP_TARGET_SHADER_DIR=docs/apple-silicon/task-16-evidence-2026-05-21/cycle3-replay/front-buffer-dump \
python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
  --xbe swizzle-mipmap --renderer metal \
  --out docs/apple-silicon/task-16-evidence-2026-05-21/cycle3-replay/front-buffer-dump/harness-out \
  --surface-scale 1 --no-upload
```

## Decisive findings (this cycle)

1. **The swizzle-mipmap XBE's stride==44 `xbed_draw_arrays(TRIANGLES,
   cell*24, 24)` draws all render to back-buffer-class color targets
   `0x03aa8000`, `0x03bd4000`, `0x03d00000` cycling round-robin per
   frame.** Confirmed by 32 consecutive
   `metal_dispatch_draw_target color_addr=0x03{aa8|bd4|d00}000
   depth_addr=0x0397c000 uniform_attrs=0xfdf6 vcount=24 icount=0
   prim=5 color_fmt=0x50 depth_fmt=0x104 v0=1 v3=1 v9=1 native_tri=1
   native_quad=0` lines. Each line interleaves 1:1 with the
   cycle-2-shipped `metal_set_attr_masks uniform_attrs=0xfdf6
   [9]c=4,s=44` line, proving they describe the same dispatch.
   Evidence: `logs/dispatch-draw-target-stride44.log`,
   `logs/set-attr-masks-stride44.log`.

2. **The XBE NEVER renders to `0x032a4000` (the canonical "front
   buffer" address that the prior `0x032a4000`-class shader dump
   referenced).** Zero of the 32 stride==44 dispatches target it.
   Interval `metal_draw_target vram_addr=0x32a4000 count=...` lines
   show `0x032a4000` receives ~30-35 flush_draws/sec — flat, dashboard-
   class cadence consistent with a non-XBE pipeline, not the XBE's
   7-cells-per-frame burst rhythm.

3. **The fresh `0x032a4000` GLSL dump (replayed this cycle, addressing
   cycle 2's lost capture) has EVERY vertex slot routed through
   `inlineValue[N]`** — no `layout(location = N) in vec4 vN` for any
   slot. I.e. `uniform_attrs == 0xFFFF`, not the
   cycle-2-narrative's `0xFDFE` (which was already inconsistent with
   the XBE's bind sequence). Evidence:
   `glsl-dumps/xemu-metal-target-0x032a4000.glsl`. This is a uniform-
   only blit / publish-class draw, not the XBE's pipeline. The cycle 2
   narrative of "front-buffer pipeline with `v3 = inlineValue[2]`
   while `v9` streaming" was capturing a transient mid-stream pipeline
   state for an unrelated draw to `0x032a4000`; "latest dump wins" per-
   target means cycle 2's snapshot was just a different non-XBE draw.
   Either way, the XBE does not own `0x032a4000`.

4. **Front-buffer publish path under `XEMU_METAL_FRONT_FB_FALLBACK=1`
   (the xbe-harness canonical Metal recipe) overwhelmingly selects
   dashboard surfaces, NOT the XBE's back buffer.** 427 publishes
   observed over the run. Target histogram: 376 → `0x3628000`
   (dashboard compositor), 26 → `0x2c06000`, 18 → `0x2e06000`, 3 →
   `0x03aa8000` (an XBE back buffer), 3 → `0x2994000`, 1 →
   `0x2454000`. Reason breakdown: 426 `fallback-dominant-draw` + 1
   `fallback-current-binding` (the first publish in the run, to
   `0x3628000`). The `0x2454000` / `0x2994000` outliers are transient
   non-XBE pipelines (4/427 ≈ 0.9%). Evidence:
   `logs/front-fb-publish.log`.

5. **The captured "best frame" screenshot (`screenshots/cycle3-best-
   frame-0124-xbe-per-mip-tint-ramp.png`) IS the XBE's actual output**
   — 4-column × 2-row grid with per-mip RED tint ramp (cell 0 bright
   red → cell 6 darkest red, cell 7 black). Each cell is a uniform
   color across all 4 sub-quads — i.e. intra-mip Q0 collapse (Q0=red
   sampled for all 4 quadrants) with per-mip LOD-clamp ramp working.
   This is exactly the manifest's documented `expected_fail_notes`
   symptom. The cycle-2 banner's "corner-tinted gradient covering the
   full surface" description (frame 0138 in the cycle-2 capture) was
   capturing a different surface entirely — the cycle-3 frame 0138
   screenshot (`screenshots/cycle3-frame-0138-dashboard-noise.png`)
   shows green-on-black dashboard / VGA-direct noise, the M5.13 / M18
   VGA-direct deferred bug, not the XBE.

6. **CPU-side unswizzled texture buffer is correct** for every mip:
   `metal_unswizzle_dump w=64 Q0=(B00 G00 Rff Aff) Q1=(B00 Gff R00
   Aff) Q2=(Bff G00 R00 Aff) Q3=(B00 Gff Rff Aff)` and the same
   pattern with per-mip-tinted intensity down to `w=2 h=2`. Evidence:
   `logs/unswizzle-dump-quadrants.log`. Combined with the cycle-2
   `metal_attrib_stream slot=9 ... count=4 stride=44` proof that the
   per-vertex UVs reach the renderer correctly, this localizes the
   bug to the **Metal texture sampler / fragment-shader UV-to-texel
   path**, not the vertex pipeline.

## Net effect on the open Task #16 narrative

The cycle-2 banner left two competing hypotheses open:
- (a) XBE renders to back buffer; the screenshot publishes a stale
      front-buffer pipeline's output.
- (b) XBE renders to front buffer directly through some path the
      diagnostics didn't catch.

This cycle decisively confirms (a) and rules out (b). Further, the
"front-buffer pipeline" the cycle-2 narrative pointed at (`0x032a4000`
with `v3 = inlineValue[2]` while `v9` streaming) is **not actually
mid-blit on the XBE's content** — `0x032a4000`'s current pipeline is
fully uniform-only, and the XBE's content is in `0x03aa8000` /
`0x03bd4000` / `0x03d00000`.

The actual Task #16 symptom — intra-mip Q0 collapse with per-mip ramp
working — is captured correctly in `screenshots/cycle3-best-frame-
0124-xbe-per-mip-tint-ramp.png`. CPU-side vertex stream, CPU-side
unswizzled texture, vertex descriptor, render target, color/depth
format, and compiled pipeline state are all proven correct upstream of
the bug. The remaining surface area is the **sampler / fragment shader
UV-to-texel path** inside `pgraph_mtl_texture_*` and the GLSL `pT0.xyw`
projection logic seen in the staged GLSL dumps.

## Next exact experiments (handoff to the next slice)

1. **Per-cell sampler-state attribution.** Add an env-gated diag in
   `pgraph_mtl_texture_bind_from_pg` (or the `build_sampler_desc_from_pg`
   path that already shipped 2026-05-21 morning) that, gated on the
   same stride==44 dispatch heuristic, logs the resolved
   `min_lod_clamp` / `max_lod_clamp` / `mip_filter` / `mag_filter` /
   `mip_levels` / `texture_base_vram_addr` per draw. Confirm the
   Metal sampler descriptor honors per-cell MIN_LOD_CLAMP rewrites
   (LOD-clamp fix landed morning of 2026-05-21).

2. **Per-cell sampled-color readback.** Use `XEMU_METAL_DUMP_DRAW_RT`
   to dump the color RT after each per-cell draw. If the RT contains
   the per-quadrant Q0/Q1/Q2/Q3 pattern but the displayed front-fb
   shows uniform Q0 only, the bug is in the publish/compose path. If
   the RT itself contains only Q0 per cell, the bug is in the
   sampler.

3. **GLSL `textureProj(texSamp0, pT0.xyw)` audit.** Verify the
   generated GLSL preserves per-vertex `pT0.xy` through to the
   fragment shader's projective sample. The XBE binds 4 distinct UV
   centers per cell; if the rasterized fragments don't see distinct
   `pT0.xy` values, the bug is upstream of the sampler (rasterizer /
   fragment-interpolation path).

4. **Confirm `XEMU_METAL_FRONT_FB_FALLBACK=1` is acceptable for this
   XBE.** The "dominant-draw" fallback's 0.7% capture rate of XBE
   content is enough for the harness's best-frame selector to land
   the XBE's render, but a future XBE with tighter draw timing might
   miss entirely. Worth considering an XBE-specific
   `metal_canonical_overrides` setting `XEMU_METAL_FRONT_FB_FALLBACK=0`
   so the publish goes through CRTC resolution.

## Artifact map

```
cycle3-replay/
├── README.md                              this file
├── glsl-dumps/
│   ├── xemu-metal-target-0x032a4000.glsl  (replayed front-buffer dump, all uniform — from run B)
│   ├── xemu-metal-target-0x03aa8000.glsl  (XBE back buffer A — from run A)
│   ├── xemu-metal-target-0x03bd4000.glsl  (XBE back buffer B — from run A)
│   └── xemu-metal-target-0x03d00000.glsl  (XBE back buffer C — from run A)
├── logs/
│   ├── dispatch-draw-target-stride44.log  (NEW: 32× per-dispatch attribution)
│   ├── set-attr-masks-stride44.log        (cycle-2 diag, replayed)
│   ├── attrib-stream-slot9-stride44.log   (cycle-2 diag, replayed)
│   ├── front-fb-publish.log               (publish target histogram source — from run B)
│   ├── draw-target-aggregates.log         (per-interval flush_draw counts — from run B)
│   └── unswizzle-dump-quadrants.log       (cycle-2 diag, proves CPU correct)
└── screenshots/
    ├── cycle3-best-frame-0124-xbe-per-mip-tint-ramp.png  (XBE actual output — frame 0124 from run B)
    └── cycle3-frame-0138-dashboard-noise.png             (non-XBE noise frame — frame 0138 from run B)
```

Note: the raw harness-out directories (~2 GB of full xemu.log + 279
PNG screenshots per run) are NOT checked in. They are reproducible
verbatim from the two commands in the "Reproduction" section above.
The curated logs and screenshots above are derived from them.
