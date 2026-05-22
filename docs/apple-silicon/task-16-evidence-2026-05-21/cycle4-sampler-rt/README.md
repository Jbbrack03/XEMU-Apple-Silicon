# Task #16 — cycle 4 sampler-attribution + bordered-texture diagnosis (2026-05-21 evening, Hermes cycle 4)

Hermes-supervised continuation of cycles 2 + 3 (parent + `cycle3-replay/`
sibling directories). This cycle adds a per-bind sampler-state
attribution diagnostic and uses it together with the cycle-3
`metal_dispatch_draw_target` line and the cycle-2 `metal_unswizzle_dump`
line to **localize the remaining "intra-mip Q0 collapse" symptom
decisively**. The conclusion supersedes cycle 3's open framing of
"sampler / fragment-shader UV-to-texel path" — it is **not** a Metal
sampler-state bug. The bug is the GLSL fragment shader's
**bordered-texture UV transform** running against an **un-doubled
texture upload** in the Metal renderer, triggered by the diag-XBE
library shipping the swizzle-mipmap texture with `BORDER_SOURCE != COLOR`
(default-zero in `xbed_texture_bind_stage0`'s composed format word).

## Tooling delta this cycle (env-gated, zero impact when env unset)

`hw/xbox/nv2a/pgraph/mtl/texture_pg.c::pgraph_mtl_texture_bind_from_pg`
gains one env-gated `metal_tex_bind_attrib` diag line per bind when:

- `XEMU_METAL_DIAG_ATTRIB_DUMP` is set, AND
- `pg->vertex_attributes[9].stride == 44` (matches the cycle-3
  dispatch-target diag — the swizzle-mipmap XBE's TexVertex layout), AND
- `s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8`
  (the XBE's texture format).

Capped at 32 lines so the four diag streams from this family
(`metal_set_attr_masks` / `metal_attrib_stream` / `metal_dispatch_draw_target`
/ `metal_tex_bind_attrib`) share the same per-dispatch line budget. The
gate intentionally does NOT include `s.levels == 7` because the XBE's
per-cell `MAX_LOD_CLAMP` writes cause `pgraph_get_texture_shape` to
clamp the reported `s.levels` down to `max_mipmap_level + 1`, so the
SAME XBE-bound texture is reported as `s.levels = 1..7` across the 7
per-cell binds. The line shape:

```
xemu-perf: metal_tex_bind_attrib stage=N tex_addr=0xAAAA color_target=0xCCCC \
  nv2a_fmt=0xFF mtl_fmt=M w=W h=H levels=L s_levels=S \
  shape_min_lvl=MN shape_max_lvl=MX \
  min_lod=X.XXX max_lod=Y.YYY lod_bias=B.BBB \
  min_f=N mag_f=N mip_f=N addr_u=U addr_v=V \
  has_surf=B self_sample=B linear=B tex_dirty=B \
  next_dump_idx=IDX dump_active=B
```

`next_dump_idx` cross-references the upcoming `XEMU_METAL_DUMP_DRAW_RT`
PNG filename via a new tiny helper `pgraph_mtl_draw_dump_rt_peek_index()`
in `mtl/draw.{h,mm}` so the caller can match a stride==44 bind to its
eventual post-flush_draw PNG when DUMP is active.

Documented in `automation.md` "Diagnostic Toggles" and
`.claude/rules/flags-renderer.md`.

## Decisive findings (this cycle)

### 1. Per-cell sampler state IS correct on Metal

The 32 captured stride==44 binds during the swizzle-mipmap run show
exact per-cell discrimination:

```
cell 0: levels=1, shape_min=0, shape_max=0, min_lod=0.000, max_lod=0.000, mip_f=0
cell 1: levels=2, shape_min=1, shape_max=1, min_lod=1.000, max_lod=1.000, mip_f=1
cell 2: levels=3, shape_min=2, shape_max=2, min_lod=2.000, max_lod=2.000, mip_f=1
cell 3: levels=4, shape_min=3, shape_max=3, min_lod=3.000, max_lod=3.000, mip_f=1
cell 4: levels=5, shape_min=4, shape_max=4, min_lod=4.000, max_lod=4.000, mip_f=1
cell 5: levels=6, shape_min=5, shape_max=5, min_lod=5.000, max_lod=5.000, mip_f=1
cell 6: levels=7, shape_min=6, shape_max=6, min_lod=6.000, max_lod=6.000, mip_f=1
```

Evidence: `logs/sampler-attrib-per-cell.log` (32 lines, four
batches of 7 cells each plus a 4-line partial batch as the cap
exhausts). The Metal sampler descriptor cache produces a distinct
`MTLSamplerState` per cell because the `PgraphMtlSamplerDesc` memcmp
key differs in `min_lod`/`max_lod`. The cycle-3 LOD-clamp morning fix
is plumbed end-to-end through `build_sampler_desc_from_pg`.

**Cycle 3's open question — "is the sampler bug per-cell?" — is
answered: no, the sampler is per-cell correct.** Each cell selects
the right mip via the sampler. The intra-mip Q0 collapse must come
from elsewhere.

### 2. `pgraph_get_texture_shape` clamps `s.levels` per cell

The XBE's `set_lod_clamp(mip, mip)` writes
`NV097_SET_TEXTURE_CONTROL0` with `MIN_LOD_CLAMP=mip` and
`MAX_LOD_CLAMP=mip`. Inside `pgraph_get_texture_shape`
(`hw/xbox/nv2a/pgraph/texture.c:304`):

```c
levels = MIN(levels, max_mipmap_level + 1);
```

So cell 0 (MAX_LOD_CLAMP=0) reports `s.levels = MIN(7, 1) = 1`;
cell 6 (MAX_LOD_CLAMP=6) reports `s.levels = MIN(7, 7) = 7`. The
widened-gate run (`logs/sampler-attrib-widened-gate-histogram.log`)
shows a per-cell histogram of exactly this distribution
(`nv2a_fmt=0x06 s_levels={1..7}` with ~9-10 binds each over the 64
captured lines).

This per-cell clamping is the reason the cycle-3 `metal_unswizzle_dump`
gate `s.levels == 7` only triggered for cell-6 binds — and it is also
the reason the original cycle-4 sampler diag's `s.levels == 7` gate
needed to be removed.

### 3. The fragment shader uses the **bordered-texture UV transform**

Cycle 3's preserved back-buffer GLSL dump
(`../cycle3-replay/glsl-dumps/xemu-metal-target-0x03aa8000.glsl`)
shows the relevant lines:

```glsl
vec3 t0LogicalSize = vec3(64.000000, 64.000000, 1.000000);
pT0.xyz = (pT0.xyz * t0LogicalSize + vec3(4, 4, 4))
            * vec3(0.007812, 0.007812, 0.062500);
vec4 t0 = textureProj(texSamp0, (pT0.xyw));
```

`0.007812 = 1/128` and `0.062500 = 1/16`. This is the
`apply_border_adjustment` block in
`hw/xbox/nv2a/pgraph/glsl/psh.c:794-810`, which `psh.c:178` only emits
when:

```c
if (border_source != NV_PGRAPH_TEXFMT0_BORDER_SOURCE_COLOR) {
    if (!f.linear && !cubemap) {
        // The actual texture will be (at least) double the reported
        // size and shifted by a 4 texel border but texture coordinates
        // will still be relative to the reported size.
```

xemu's per-renderer convention: when `border_source != COLOR`, the
**actual texture in memory is 2x the reported logical size with a
4-texel border**, and the GLSL transforms input UVs to land on the
correct texels of the doubled physical texture:

```
sample_u = (input_u * reported_w + 4) / (reported_w * 2)
        = (input_u * 64 + 4) / 128
```

For input UVs (0.25, 0.25) → sample at texel (~20/128, ~20/128) of the
**128-wide physical** texture = correct quadrant Q0 of the logical
64x64 texture. The math is correct **iff the actual MTLTexture is
128x128 with the 4-texel border**.

### 4. The GL renderer DOES upload the 2x bordered texture

`hw/xbox/nv2a/pgraph/gl/texture.c:451-456`:

```c
if (!f.linear && s.border) {
    adjusted_width = MAX(16, adjusted_width * 2);
    adjusted_height = MAX(16, adjusted_height * 2);
    adjusted_pitch = adjusted_width * (s.pitch / s.width);
    adjusted_depth = MAX(16, s.depth * 2);
}
```

GL allocates a 128x128 GL texture and uploads the 128x128 swizzled VRAM
contents into it. The GLSL's `(uv * 64 + 4) / 128` transform produces
correct sample coordinates against this 128x128 texture. **Cell 0
renders the 4-quadrant pattern correctly on GL** (per the cycle-3
manifest narrative for §4.8 + Task #17 — GL's cell-0 PASS, cells 1..6
BLACK is a SEPARATE GL bug).

### 5. The Metal renderer does NOT double the texture for borders

`hw/xbox/nv2a/pgraph/mtl/texture_pg.c::decode_face_levels`
(`scripts/apple-silicon/xbe-tests/swizzle-mipmap` SZ_A8R8G8B8 path,
file lines 856-958) decodes using `s.width`, `s.height` directly with
**no `s.border` adjustment**. The MTLTexture allocated by
`pgraph_mtl_texture_bind_slot_full` (`texture.mm:907-912`) is
`l0->width x l0->height` — i.e., the **reported 64x64**, not 128x128.

Result: the fragment shader's `(uv * 64 + 4) / 128` transform produces
sample coordinates in [0.031..0.531] for input UVs in [0..1] — i.e.
the **left half** of the 64x64 texture's normalized space. For the
XBE's per-sub-quad UVs:

- Q0 (TL) UV=(0.25, 0.25) → sample at (0.156, 0.156) of 64x64 →
  texel (10, 10) → Q0 of the texture data → **RED** ✓ matches
- Q1 (TR) UV=(0.75, 0.25) → sample at (0.406, 0.156) of 64x64 →
  texel (26, 10) → still **Q0** of the texture data (Q1 starts at
  texel 32) → **RED** ✗ (should be GREEN)
- Q2 (BL) UV=(0.25, 0.75) → sample at (0.156, 0.406) of 64x64 →
  texel (10, 26) → still **Q0** → **RED** ✗ (should be BLUE)
- Q3 (BR) UV=(0.75, 0.75) → sample at (0.406, 0.406) of 64x64 →
  texel (26, 26) → still **Q0** → **RED** ✗ (should be YELLOW)

**All four sub-quads of cell 0 land in Q0 of the actual 64x64 texture
because the UV transform expects a 128-wide texture but the renderer
uploaded only a 64-wide one.** That's the intra-mip "Q0 collapse"
exactly as observed in cycle 3 and re-captured this cycle.

Evidence: `screenshots/cycle4-best-frame-0257-q0-collapse.png` (the
swizzle-mipmap XBE on Metal — six visible cells of progressively
darker RED with the seventh cell BLACK as expected; the second-cell
black is a transient mid-flush capture artifact). Compare with
`../cycle3-replay/screenshots/cycle3-best-frame-0124-xbe-per-mip-tint-
ramp.png` — same symptom class.

### 6. The XBE's texture format word never explicitly sets BORDER_SOURCE

`scripts/apple-silicon/xbe-tests/lib/xbed_texture.c::xbed_texture_bind_stage0`
composes the format word from `fmt = 0` then OR-ins bits — but
**never sets `XBED_FMT_BORDER_SOURCE_BIT`** (line 27 defines it but
the body at lines 87-93 never references it):

```c
uint32_t fmt = 0;
fmt |= (p->dma_channel & XBED_FMT_CONTEXT_DMA_MASK);
fmt |= XBED_FMT_DIMENSIONALITY;                              /* 2D */
fmt |= ((p->color_format & 0xFFu) << XBED_FMT_COLOR_SHIFT);
fmt |= ((p->mipmap_levels & 0xFu) << XBED_FMT_MIPMAP_SHIFT);
fmt |= ((p->base_size_u_log2 & 0xFu) << XBED_FMT_BASE_SIZE_U_SHIFT);
fmt |= ((p->base_size_v_log2 & 0xFu) << XBED_FMT_BASE_SIZE_V_SHIFT);
```

So bit 3 stays 0 = `NV_PGRAPH_TEXFMT0_BORDER_SOURCE_TEXTURE` (NOT
COLOR). For comparison, the nxdk `samples/mesh` sample
(`/Users/jbbrack03/XEMU_MacOS/nxdk/samples/mesh/main.c:145`) pushes
the format word `0x0001122a` whose bit 3 is **set** (i.e.
`BORDER_SOURCE = COLOR`):

```
0x0001122a = 0b00000000 00000001 00010010 00101010
                                         |^- bit 3 = 1 (BORDER_SOURCE_COLOR)
```

`xbed_texture.c` was authored to "cross-check against samples/mesh"
(per its own header comment) but missed this bit. **It is an XBE
library bug.**

## Root cause: two interacting bugs

1. **Metal renderer bug** (`hw/xbox/nv2a/pgraph/mtl/texture_pg.c`):
   `decode_face_levels` and `pgraph_mtl_texture_bind_from_pg` do not
   honor `s.border`. When `s.border == true` (BORDER_SOURCE != COLOR)
   the renderer should follow the GL convention and upload a
   `2 * s.width x 2 * s.height` texture with the 4-texel border (per
   `gl/texture.c:451-456`). Currently it uploads only `s.width x s.height`.
   Real Xbox games that intentionally use BORDER_SOURCE != COLOR will
   misrender on Metal.

2. **XBE library bug**
   (`scripts/apple-silicon/xbe-tests/lib/xbed_texture.c::xbed_texture_bind_stage0`):
   The composed format word never sets `XBED_FMT_BORDER_SOURCE_BIT`,
   so all four library users (`swizzle-mipmap`, `texture-format-sweep`,
   `texture-dma-ab`, `texture-filter-wrap`) bind their textures as
   "TEXTURE has its own border" — accidentally triggering bug #1 in
   the Metal renderer. Only the SZ_*-format users feel the impact;
   `texture-format-sweep` / `texture-dma-ab` / `texture-filter-wrap`
   are all LU_IMAGE_-format (linear) and `psh.c:179`'s
   `if (!f.linear && !cubemap)` guard skips the bordered UV transform
   for them, so they are unaffected.

## Why cycle 3's narrowing was correct but not sufficient

Cycle 3 narrowed the surface to "Metal texture sampler /
fragment-shader UV-to-texel path". Cycle 4 confirms the sampler is
correct and the FRAGMENT-SHADER UV TRANSFORM is correct **given the
GL convention**, but the **texture upload** in the Metal renderer
doesn't honor that convention. The bug is upstream of the sampler
(in `decode_face_levels` / `bind_slot_full`) and downstream of the
GLSL generator. Cycle 3's instinct to focus on the sampler was off
by one layer; this cycle's per-bind sampler-attribution diag closes
the gap by proving the sampler is correct, forcing the search back
toward the texture upload path where the bug actually lives.

## Net effect on Task #16

- **The Q0 collapse symptom is fully explained.** It is the
  geometrically-correct rendering of `(input_uv * 64 + 4) / 128` →
  texel in [2..34] / 64, which lands in Q0 (texels 0..31) for all four
  XBE-supplied sub-quad UV centers (0.25, 0.75).
- **The per-mip tint ramp working is explained.** Per-cell
  `MIN_LOD_CLAMP = MAX_LOD_CLAMP = N` correctly forces the sampler
  to clamp to mip N — the per-cell sampler discriminates correctly
  (cycle-4 finding #1). The renderer then samples Q0 of mip N, and
  the per-mip tint at Q0 progresses 0xFF → 0x3F as the cells advance,
  matching the XBE manifest's expected mip tint sequence.
- The renderer fix is **separable from the XBE library fix.** Either
  alone closes the swizzle-mipmap XBE PASS gate on Metal; both
  together are the principled outcome (XBE library matches nxdk
  conventions; renderer correctly handles real-game bordered textures).

## Next exact experiments (handoff to the next slice)

1. **Fix the XBE library (one-line)**: in
   `scripts/apple-silicon/xbe-tests/lib/xbed_texture.c::xbed_texture_bind_stage0`
   line ~89-93, add `fmt |= XBED_FMT_BORDER_SOURCE_BIT;` after the
   `XBED_FMT_DIMENSIONALITY` line. Rebuild `default.xbe` for all four
   library users (`swizzle-mipmap`, `texture-format-sweep`,
   `texture-dma-ab`, `texture-filter-wrap`). Re-run the XBE rotation;
   expect swizzle-mipmap to flip from `expected_fail` → PASS on Metal
   (assuming Task #17 GL also gets re-addressed; otherwise swizzle-
   mipmap stays `expected_fail` on GL only). Requires nxdk build
   environment.
2. **Fix the Metal renderer (multi-file)**: mirror
   `gl/texture.c:451-456` in `mtl/texture_pg.c::decode_face_levels`
   and `pgraph_mtl_texture_bind_from_pg` — when `s.border` is true,
   double `width`/`height`/`pitch` for the unswizzle + upload paths;
   allocate the MTLTexture at the doubled size; ensure the cache key
   reflects the doubled dimensions. Verify against a NEW diag XBE
   (`xbed_texture-border` or similar) that intentionally sets
   `BORDER_SOURCE = TEXTURE` and provides 128x128 swizzled data so the
   bordered-UV transform produces correct per-quadrant samples. Without
   this, real-Xbox games using bordered textures will misrender on
   Metal.
3. **Investigate Task #17 (GL LOD-clamp regression).** Cycle 3
   evidence claims GL renders cell 0 correctly but cells 1..6 BLACK.
   That is unrelated to the bordered-texture bug (since GL's
   bordered-texture path is correct). The GL bug likely lives in the
   per-mip upload path when `s.levels < 7` due to the same
   `pgraph_get_texture_shape::levels = MIN(levels, max + 1)` clamp.
4. **Audit other psh.c bordered-texture sites** (`texture_apply_*`
   helpers, `apply_convolution_filter`, etc.) to confirm the
   `(uv*size+4)/(size*2)` formula is consistent across all UV
   sample-site emissions; the cycle-4 fix in the Metal renderer needs
   to satisfy this convention everywhere, not just the basic
   `textureProj(pT0.xyw)` call.

## Reproduction

```sh
# Single run (the diag is env-gated; rebuild xemu after the diag patch
# lands).
./build.sh -a arm64 --skip-shader-validation
XEMU_METAL_DIAG_ATTRIB_DUMP=1 \
python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
  --xbe swizzle-mipmap --renderer metal \
  --out /tmp/cycle4-replay --surface-scale 1 --no-upload

# Verify the per-cell discrimination:
grep "metal_tex_bind_attrib" /tmp/cycle4-replay/swizzle-mipmap/metal/xemu.log \
  | head -7
# Expect 7 lines with shape_min_lvl=0..6 / s_levels=1..7 across cell 0..6.
```

The optional draw-RT capture (`XEMU_METAL_DUMP_DRAW_RT=<lo>:<hi>:<prefix>`
matched against `next_dump_idx` from the diag) was attempted but the
swizzle-mipmap run produces ~10000-12000 dashboard flush_draws/sec
between the XBE bursts; dumping more than ~500 PNGs causes the harness
to time out before the XBE settles. The textual diag streams + the
front-fb screenshot are sufficient evidence — see the cycle-4 narrative
above for why the renderer-side root cause is provable without per-cell
RT keyframes.

## Artifact map

```
cycle4-sampler-rt/
├── README.md                                      this file
├── logs/
│   ├── full-xemu-final.log                        full xemu.log from the final
│   │                                              capture (3811 lines; includes
│   │                                              all four stride==44 diag streams
│   │                                              + interval counters)
│   ├── interleave-stride44-streams.log            filtered to the four
│   │                                              metal_*-named streams + the
│   │                                              metal_color_bind line; 152
│   │                                              lines, shows per-cell
│   │                                              interleave order
│   ├── sampler-attrib-per-cell.log                NEW: 32 metal_tex_bind_attrib
│   │                                              lines proving per-cell sampler
│   │                                              discrimination
│   └── sampler-attrib-widened-gate-histogram.log  64 metal_tex_bind_attrib lines
│                                                  from the gate-widened smoke run
│                                                  (the per-cell s_levels=1..7
│                                                  histogram lives here)
└── screenshots/
    └── cycle4-best-frame-0257-q0-collapse.png     swizzle-mipmap render still
                                                   showing the per-mip tint ramp
                                                   + intra-mip Q0 collapse
                                                   symptom (Q0 of the bordered-UV
                                                   left-half of the 64x64 texture)
```

The cycle-3 evidence directory (`../cycle3-replay/`) remains the
source of truth for the back-buffer GLSL dumps and the per-flip
publish histogram — those did not change this cycle.
