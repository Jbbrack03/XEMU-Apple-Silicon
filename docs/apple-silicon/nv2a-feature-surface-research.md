# NV2A Rendering Pipeline — Feature Surface Research Catalog

Last updated: 2026-05-05 (post-Codex review revisions).
Scope: rendering pipeline only (vertex pipeline → primitive assembly →
rasterization → pixel pipeline → ROP → render targets → display). Audio,
USB, AV signal, and networking subsystems are out of scope per the
2026-05-05 direction-setting conversation.

## Errata — 2026-05-05 Codex review

Independent Codex-CLI review (read-only, ChatGPT auth) returned MAJOR
ISSUES verdict and surfaced eight findings. All have been applied to
this document. Summary:

1. **HIGH** — `NV097_GET_REPORT` was overstated as a general
   self-validation mechanism. Source confirms it is **Z-pass-pixel-count
   only** (`hw/xbox/nv2a/pgraph/pgraph.c:2629-2635` asserts
   `type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT`); the Metal
   renderer's `pgraph_mtl_get_report` writes a literal **0**
   regardless of actual pixel count
   (`hw/xbox/nv2a/pgraph/mtl/renderer.c:1917-1920`); GL has explicit
   FIXME limitations for MSAA / clears / clipping
   (`hw/xbox/nv2a/pgraph/gl/reports.c:38`). The self-validation
   contract has been rewritten — see §6 below — to use CPU-side VRAM
   readback as the primary mechanism.
2. **HIGH** — The original CRTC / front-fb-fallback XBE design used
   Z-pass count to validate scanout; that's a false-negative on Metal
   today. Redesigned to use RT-as-texture sampling and CPU-side VRAM
   readback (§6).
3. **MEDIUM** — §E.1 incorrectly claimed W1 and W2 "have the same
   texture-format set." They don't: W1 lists `0x2F LU_IMAGE_DEPTH_X8_Y24_FLOAT`
   (`texture.c:65`) that W2 doesn't enumerate symbolically; W2 lists
   `0x16 LU_IMAGE_R8B8` (`nv_regs.h:554`) that W1 doesn't. The §E.1
   table now has explicit per-witness columns.
4. **MEDIUM** — §3 mixed true disagreements with gaps and agreements.
   #8 (primitive enum) is full agreement; #10 (sRGB) is agreement;
   #12 (logic ops) is W1-only. §3 is now split into "true
   disagreements," "single-witness gaps," and "known xemu
   implementation limitations."
5. **MEDIUM** — Texture DMA selector (DMA A vs DMA B) was missing.
   `SET_CONTEXT_DMA_A/B` set `pg->dma_a/b` (`pgraph.c:1056`);
   `SET_TEXTURE_FORMAT_CONTEXT_DMA` selects the channel
   (`pgraph.c:2675`); texture/palette lookup maps through both
   (`texture.c:92, 153`). §E.11 now covers this and a DMA-A-vs-B
   diagnostic XBE is added to the build priority.
6. **MEDIUM** — §C.7 conflated point sprite, point parameter, and
   point smoothing. xemu's fragment-side `point_sprite` flag is
   currently derived from `POINTSMOOTHENABLE`
   (`glsl/psh.c:102`) — that's *not* the same as the D3D8
   `POINTSPRITEENABLE` source. The three concepts are now
   distinguished separately and the true Xbox point-sprite enable
   source is marked unresolved.
7. **LOW** — §6 promotion contract required GL cross-validation as
   blocking. Some XBEs are *expected* to fail GL (e.g., logic ops:
   GL map is commented out in `gl/constants.h:86`), so blocking
   GL would reject useful oracle XBEs. GL/Cxbx cross-validation is
   now advisory; promotion gates on the mathematical oracle plus
   self-validation path; known-renderer failures are recorded in
   the manifest.
8. **LOW** — Catalog itself was not in the Codex-validate cadence.
   Now added (§8).

The Codex review also raised three open questions; the project's
answers are carried in §6.7. Original Codex transcript is not
preserved in-tree; project rule #15 hook validation marker not
written because this was a `plan`-mode review of a doc, not
`changes` mode.

## 0. Why this document exists

The 2026-05-05 SC2 Metal canonical-recipe replay produced clean perf
counters (`METAL_PIPELINE_TRANSLATED_FAILED=0`,
`METAL_PIPELINE_FALLBACKS=0`, `post_load_avg_fps=41.84`) but the user's
live-test observation reported severe visual bugs (top-half mirrored
into bottom half, missing floor, wrong colors). Single-renderer
counters and short still-image strips did not flag the divergence. The
GL renderer is ~85% correct (per the user) so a paired Metal-vs-GL
diff cannot serve as a correctness oracle either — it is at most a
divergence detector.

The decision was to build a diagnostic-XBE library targeting the NV2A
rendering pipeline feature-by-feature, with each XBE designed so its
correct visual output is mathematically derivable and (where possible)
self-validated by **CPU-side VRAM readback** (the XBE runs in the
guest; it has direct memory-mapped access to its own VRAM via
`MmGetPhysicalAddress` / pbkit framebuffer pointers, and can decode
rendered pixels and report PASS/FAIL via `pb_print` text overlay).
RT-as-texture sampling is the secondary mechanism for cases where
CPU readback is unwieldy. `NV097_GET_REPORT` is **not** suitable as
a general self-validation mechanism — it is Z-pass-only and the
Metal renderer currently always returns 0 (see §6 contract for full
detail). Each XBE becomes its own oracle. To do that without
guessing, we first need a complete and source-cited catalog of the
NV2A rendering pipeline feature surface — which is this document.

This catalog is the input to the next deliverable, `diagnostic-xbe-plan.md`,
which will translate the catalog into a per-XBE design with the
correctness contract baked in.

## 1. Witnesses

Three independent research streams contributed to this catalog. Each
claim below cites the witness(es) that support it. Where two
witnesses contradict, both are quoted.

| Tag | Witness | Type |
|---|---|---|
| **W1** | xemu source (`hw/xbox/nv2a/`) | First-party, internal |
| **W2** | nxdk + pbkit (`/Users/jbbrack03/XEMU_MacOS/nxdk/`) | Homebrew SDK; Xbox-side runtime |
| **W3** | External docs (xboxdevwiki, archived MS D3D8 ref, apitrace dxsdk d3d8 header, Khronos NV_register_combiners + NV_texture_shader specs, Cxbx-Reloaded source/issues, envytools, Beyond3D, Wikipedia) | Independent third parties |

All three streams ran in parallel and did not share intermediate
results before producing their respective catalogs. The synthesis
below cross-references them.

## 2. Architecture overview

Cited claims:

- NV2A is the NV20-family ("Kelvin") graphics processor; the Xbox
  graphics object class is `NV_KELVIN_PRIMITIVE = 0x0097`, hence the
  method-namespace prefix `NV097_*`. (W1: `nv2a_regs.h:844`. W3:
  xboxdevwiki NV2A page; Beyond3D — "Faster GeForce3 with one major
  addition: a second vertex shader pipeline.")
- HW revisions: XGPU (1.0), XGPU S (1.1–1.4), XGPU-B (1.6); xboxdevwiki
  notes "very little known about the difference between the
  revisions." (W3: xboxdevwiki.) The fork treats the GPU as
  feature-uniform across these revs.
- Pixel back-end: 4 pixel pipelines × 2 texture units per pipeline; "4
  textures per pass" (Wikipedia / Beyond3D).
- Theoretical fillrate: 932 Mpix/s; 1864 Mtex/s (W3: Wikipedia).
- AA modes marketed: Quincunx, supersample, multisample (W3:
  Wikipedia / Beyond3D); concrete encoding flags below in §C.4 / §H.
- xemu names the vertex unit "Cheops" (W1:
  `nv2a_regs.h:318` = `NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START`) and the
  lighting context "Zoser" (W1: `nv2a_regs.h:1361`).
- PGRAPH = MMIO at offset 0x400000 on NV20:NV40 (W3: envytools).
- PCRTC scanout register documentation in envytools is "Todo: write
  me" (W3: gap; W1 provides the actual register layout — see §I).

Structural counts that constrain the diagnostic XBE library:

| Quantity | Value | Witness |
|---|---|---|
| Vertex shader instruction slots | 136 | W1 (`nv2a_regs.h:1510` `NV2A_MAX_TRANSFORM_PROGRAM_LENGTH`); W3 (xboxdevwiki) |
| Vertex shader instruction width | 4 dwords (16 bytes) | W1 (`vsh_regs.h:83` `VSH_TOKEN_SIZE`); W3 |
| Vertex shader constants | 192 (two banks of 96, D3D's c[0] at index 96) | W1 (`nv2a_regs.h:1511`, `vsh_regs.h:85`); W3 (xboxdevwiki) |
| Vertex shader temps | 12 (R0..R11) | W3; W1 implicit via translator |
| Vertex shader inputs | 16 (`v[0..15]`) | W1, W2, W3 — full agreement |
| Vertex shader outputs | 11 (`HPOS, COL0, COL1, FOGC, PSIZ, BFC0, BFC1, TEX0..TEX3`) | W1 (`vsh_regs.h:94-97`); W3 |
| Vertex attribute formats | 6 (UB_D3D, S1, F, UB_OGL, S32K, CMP) | W1 (`pgraph.c:2564-2590`); W2 (`nv_regs.h:481-490`); W3 implicit |
| Vertex attribute slots | 16 | W1 (`nv2a_regs.h:1507`); W2 (`nv_objects.h:1148`) |
| Lights | 8 | W1 (`nv2a_regs.h:1512`); W3 |
| Texture units | 4 | W1 (`nv2a_regs.h:1508`); W2; W3 |
| Texture format codes (catalogued) | 42 (union); 41 attested by W1, 41 attested by W2 — see §E.1 for per-witness divergences (0x16 W2-only, 0x2F W1-only) | W1 (`texture.c:26-79`, `kelvin_color_format_info_map[66]`); W2 (`nv_regs.h:537-579`) |
| Combiner stages | 8 + 1 final | W1 (`glsl/psh.h:44-45`); W2 (`nv_regs.h:665-683`); W3 |
| Texture-shader stage modes | 19 (5-bit field; see §D.8) | W1 (`psh_regs.h:31-53`); W3 (xboxdevwiki "16 from NONE to DOTPRODUCT" — a slight undercount) |
| Primitive types | 11 (incl. `_OP_END`, points, lines, line loop/strip, tris, tri strip/fan, quads, quad strip, polygon) | W1 (`nv2a_regs.h:1160-1170`); W2 (`nv_regs.h:505-516`); W3 (community-derived, paywalled mirror) |
| Window clip rectangles | 8 (Xbox-specific) | W1 (`nv2a_regs.h:618-637`); W2 (`nv_regs.h:221-222`) |
| Render-target color formats | 10 | W1 (`nv2a_regs.h:870-880`); W2; W3 |
| RT depth formats | 2 (Z16, Z24S8 — fixed *or* float at xemu's discretion) | W1 (`nv2a_regs.h:881-883`); W3 |
| MRT support | None — a single color + a single zeta binding | W1 (`gl/surface.c:200,381,415`); W2 (no MRT API surface in pbkit); W3 |
| Method dispatch table size | ~200 entries | W1 (`methods.h.inc`) |
| Max draw arrays count tracked | 1250 ranges/draw | W1 (`pgraph.h:238-239`) |
| Max batch length (inline elements / array) | 0x07FFFF | W1 (`pgraph.h:229`, `nv2a_regs.h:1505`) |

---

## A. Vertex pipeline

### A.1 Programmable vertex shader ("Cheops")

The Xbox vertex pipeline is a programmable transform stage based on
NV_vertex_program / vs.1.1 with Xbox-specific extensions. Source: W3
(xboxdevwiki NV2A/Vertex_Shader page; Khronos NV_vertex_program spec)
+ W1 (translator and decoder in `glsl/vsh-prog.c`, `vsh_regs.h`).

**Program upload mechanism (W1, W2):**

- `NV097_SET_TRANSFORM_PROGRAM` (0x00000B00, 32-dword burst) writes
  instruction tokens into `pg->program_data[136][4]` at the load-pointer
  position. (`nv2a_regs.h:1090`, `pgraph.h:189`, handler
  `pgraph.c:2140`.)
- `NV097_SET_TRANSFORM_PROGRAM_LOAD` (0x00001E9C) sets the per-burst
  load cursor. (`nv2a_regs.h:1311`.)
- `NV097_SET_TRANSFORM_PROGRAM_START` (0x00001EA0) sets execution
  start. (`nv2a_regs.h:1312`.)
- `NV097_SET_TRANSFORM_EXECUTION_MODE` (0x00001E94, fields `MODE` and
  `RANGE_MODE`) selects fixed-function vs programmable, plus the
  privileged-vs-user range. (`nv2a_regs.h:1307-1309`.)
- `NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN` (0x00001E98) enables
  read-write constant bank vs read-only. (W2: `nv_regs.h:768`.)

**Constants (W1, W2, W3):**

- `NV097_SET_TRANSFORM_CONSTANT` (0x00000B80, 32-dword) writes a
  constant slot (4 floats per slot). (W1: `nv2a_regs.h:1091`.)
- `NV097_SET_TRANSFORM_CONSTANT_LOAD` (0x00001EA4) sets the
  constant-write cursor. (W1: `nv2a_regs.h:1313`.)
- D3D `c[0]` corresponds to NV2A constant index 96
  (`VSH_D3DSCM_CORRECTION = 96`). (W1: `vsh_regs.h:85`. W3: "Microsoft
  exposed the 96 additional constant registers in D3D shaders through
  c[-96] to c[-1].")
- 192 constants total. (W1: `nv2a_regs.h:1511`; W3: xboxdevwiki.)

**Vertex state shader (XVSS) launch (W1):**

- `NV097_LAUNCH_TRANSFORM_PROGRAM` (0x00001E90); CPU-emulator path
  `nv2a_vsh_emu_execute_track_context_writes` at `pgraph.c:3130`.
- `NV097_SET_TRANSFORM_DATA` (0x00001E80, 4 dwords) buffered into
  `pg->vertex_state_shader_v0[4]` (`pgraph.h:188`).
- Vertex state shaders feed `v[0]` from `LAUNCH_DATA` registers
  (W3: 0x1E80/0x1E84/0x1E88/0x1E8C for X/Y/Z/W).

**Shader version tags (W1):**

| Tag | Value | Meaning |
|---|---|---|
| `VSH_VERSION_VS` | 0xF078 | Unofficial vs.1.1 |
| `VSH_VERSION_XVS` | 0x2078 | Xbox vertex shader |
| `VSH_VERSION_XVSS` | 0x7378 | Xbox vertex state shader |
| `VSH_VERSION_XVSW` | 0x7778 | Xbox vertex read/write shader |

(`vsh_regs.h:72-81`.)

**Instruction set (W1, W2, W3):**

Vertex shader instructions are 4 little-endian dwords. Each
instruction packs both an MAC (vector) op and an ILU (scalar) op that
execute in parallel. Decoder fields enumerated in
`vsh_regs.h:139-181`.

- **Vector / MAC ops** — 14 codes: `NOP, MOV, MUL, ADD, MAD, DP3, DPH,
  DP4, DST, MIN, MAX, SLT, SGE, ARL`. (W1: `vsh_regs.h:115-130`. W2:
  `nv20_shader.h:13-34`. W3: xboxdevwiki list.)
- **Scalar / ILU ops** — 8 codes: `NOP, MOV, RCP, RCC, RSQ, EXP, LOG,
  LIT`. (W1: `vsh_regs.h:104-113`. W2; W3.)
- **Output muxing** — `OMUX_MAC` / `OMUX_ILU` (W1: `vsh_regs.h:99-102`).
  Output target `OUTPUT_C` (constant register file write) vs `OUTPUT_O`
  (output register) (W1: `vsh_regs.h:94-97`).
- **Source register types** — `PARAM_R` (temp), `PARAM_V` (input
  attribute), `PARAM_C` (constant) (W1: `vsh_regs.h:87-92`. W2:
  `nv20_shader.h:39-42`).
- **Per-component swizzle** — 2 bits per component (X/Y/Z/W). (W1, W2,
  W3.)
- **Negate flag** — bit 14 of the source word. (W2: `nv20_shader.h`.)
- **Address register** — `A0.x`, set only via `ARL`. (W3.)
- **R12 = oPos mirror** — output `o[HPOS]` is mirrored into temp `R12`
  and can be used as a source operand. (W3: xboxdevwiki — "R12 is
  mirrored as oPos and can be used as source operand.") This is
  Xbox-specific; emulators that don't model the alias break titles
  that read back the position they just wrote.
- **DPH semantics** — `dst = s1.x*s2.x + s1.y*s2.y + s1.z*s2.z + s2.w`
  (homogenous dot). (W3.)
- **RCC quirk** — clamps reciprocal output, with Xbox-specific signed-
  zero handling: "rounds -0.0 to the negative range and 0.0 to the
  positive range." (W3.)
- **Final-instruction flag** — `NV20_VP_INST_LAST_INST` (W2:
  `nv20_shader.h:65-86`).

**ARL emulation pitfall** (W3, xemu issue #2362):
xemu currently uses `int(floor(src + 0.001))` to bias for Xbox's
"specify rounding" behavior. This *over-corrects* in Midtown Madness
3 where input ≈58.999996 becomes 59 instead of 58, producing mesh
corruption. Any diagnostic XBE that exercises ARL must explicitly
test these boundary cases. The exact Xbox rounding rule is not
externally documented (W3: gap).

### A.2 Fixed-function transform pipeline

The "fixed-function" path is implemented in xemu as a *generated*
vertex shader (W1: `glsl/vsh-ff.c`) — there is no separate hardware
fixed-function block at the renderer's view; the configuration
registers steer the generator.

**Top-level FFP state (W1):** `FixedFunctionVshState` struct
(`glsl/vsh.h:30-43`) carries:

- `normalization` — normalize normals after transform.
- `texture_matrix_enable[4]` — per-stage texture-matrix application.
- `texgen[4][4]` — per-stage, per-(S/T/R/Q)-channel texgen mode.
- `foggen` — fog generation source.
- `skinning` — bone-blend mode (off, 2, 2G, 3, 3G, 4, 4G).
- `lighting` — global lighting enable.
- `light[NV2A_MAX_LIGHTS=8]` — per-light type (off / infinite / local
  / spot).
- Material color sources — diffuse / specular / ambient / emissive
  per-vertex vs material.
- `local_eye` — local viewer vs infinite viewer for specular.

**Matrices (W1, W2):**

| Matrix | NV097 method | Range |
|---|---|---|
| Modelview (4 of them; per-skin-bone) | `SET_MODEL_VIEW_MATRIX` (0x0480) | 64 dwords |
| Inverse modelview | `SET_INVERSE_MODEL_VIEW_MATRIX` (0x0580) | 64 dwords |
| Projection | `SET_PROJECTION_MATRIX` (0x0440) | 16 dwords |
| Composite (Xbox-specific MVP) | `SET_COMPOSITE_MATRIX` (0x0680) | 16 dwords |
| Texture matrix per-stage | `SET_TEXTURE_MATRIX` (0x06C0) | 64 dwords |
| Texture matrix enable | `SET_TEXTURE_MATRIX_ENABLE` (0x0420) | 4 |

(W1: `nv2a_regs.h:1059-1066`. W2: `nv_regs.h:373-377`. Helpers:
`pb_push_transposed_matrix` / `pb_push_4x4_matrix` /
`pb_push_4x3_matrix` (W2: `pbkit_pushbuffer.h:128-135`).)

**Lighting state (W1, W2, W3):**

- Global enable: `NV097_SET_LIGHTING_ENABLE` (0x0314).
- Per-light type mask: `NV097_SET_LIGHT_ENABLE_MASK` (0x03BC) →
  `NV_PGRAPH_CSV0_D_LIGHTS` mask. Values per light: 0=OFF / 1=INFINITE
  / 2=LOCAL / 3=SPOT. (W1: `nv2a_regs.h:287-292`. W2.)
- Per-light parameters: ambient/diffuse/specular colors at
  0x1000/0x100C/0x1018; local-range (0x1024); infinite-half-vector
  (0x1028); infinite-direction (0x1034); spot-falloff (0x1040);
  spot-direction (0x104C); local-position (0x105C); local-attenuation
  (0x1068). Back-face equivalents at 0x0C00.. (W1, W2.)
- Two-side: `SET_LIGHT_TWO_SIDE_ENABLE` (0x17C4) (W2).
- Light control flags: `NV097_SET_LIGHT_CONTROL` (0x0294) —
  `INCLUDE_SPECULAR`, `SEPARATE_SPECULAR`, `LOCALEYE` (bit 16),
  `ALPHA_FROM_MATERIAL_SPECULAR` (bit 17). (W1: `nv2a_regs.h:905-908`.
  W2: `nv_regs.h:169-174`.)

**Material state (W1, W2):**

- Emission: `SET_MATERIAL_EMISSION` (0x03A8) → `ltctxa[CM_COL]`.
- Alpha: `SET_MATERIAL_ALPHA` (0x03B4); back-face `SET_BACK_MATERIAL_ALPHA`
  (0x17AC).
- Color material source: `SET_COLOR_MATERIAL` (0x0298) selects whether
  the per-vertex color or the material constant feeds ambient/diffuse/
  specular/emission. Enum `MaterialColorSource { MATERIAL, DIFFUSE,
  SPECULAR }` (W1: `vsh_regs.h:205-209`).
- Specular: `SET_SPECULAR_ENABLE` (0x03B8) → `NV_PGRAPH_CSV0_C_SPECULAR_ENABLE`.
  Specular params: `SET_SPECULAR_PARAMS` (0x09E0, 6 dwords); back
  variant at 0x1E28. (W1: `pgraph.h:213-216`.)
- Specular fog factor: `SET_SPECULAR_FOG_FACTOR` (0x1E20).
- Scene ambient: `SET_SCENE_AMBIENT_COLOR` (0x0A10).

**Eye state (W1, W2):**

- `SET_EYE_POSITION` (0x0A50, 4 dwords).
- `SET_EYE_DIRECTION` (0x17E0, 3 dwords).
- `SET_EYE_VECTOR` (0x181C) — register `NV_PGRAPH_EYEVEC0/1/2`.

**Fog (W1, W2, W3):**

- `SET_FOG_ENABLE` (0x02A4).
- `SET_FOG_MODE` (0x029C) — values: `_LINEAR=0x2601, _EXP=0x800,
  _EXP2=0x801, _EXP_ABS=0x802, _EXP2_ABS=0x803, _LINEAR_ABS=0x804`.
  (W2: `nv_regs.h:201-207`.)
- `SET_FOG_GEN_MODE` (0x02A0).
- `SET_FOG_COLOR` (0x02A8).
- `SET_FOG_PARAMS` (0x09C0).
- `SET_FOG_PLANE` (0x09D0).
- `SET_FOG_COORD` (0x1698).

D3D8 fog (W3 cross-reference): `D3DFOGMODE { NONE=0, EXP=1, EXP2=2,
LINEAR=3 }`. Per-pixel range-based fog "Not currently supported by
any hardware" per archived MS doc, but Xbox NV2A's `o[FOGC]` from the
vertex shader feeds the fog computation; emulators replicating in a
fragment shader must interpolate fog factor explicitly.

**Texgen modes (W1, W2):**

- `SET_TEXGEN_S/T/R/Q` (0x3C0/4/8/C, 16-stage range = 4 stages × 4
  channels). Values: `DISABLE=0, EYE_LINEAR=0x2400, OBJECT_LINEAR=0x2401,
  SPHERE_MAP=0x2402, REFLECTION_MAP=0x8512, NORMAL_MAP=0x8511`. (W1:
  `nv2a_regs.h:1050-1055`. W2.)
- `SET_TEXGEN_PLANE_S/T/R/Q` (0x840/50/60/70, 256 dwords total = 4
  stages × 4 channels × 4 plane components).
- `SET_TEXGEN_VIEW_MODEL` (0x09CC) — `LOCAL_VIEWER=0` /
  `INFINITE_VIEWER=1`.

**Skinning / vertex blend (W1, W2, W3):**

- `SET_SKIN_MODE` (0x0328) — values: `OFF, 2G, 2, 3G, 3, 4G, 4`. (W1:
  `nv2a_regs.h:944-951`, `vsh_regs.h:61-69`.)
- Per-vertex weights: `SET_WEIGHT1F` (0x169C) / `SET_WEIGHT2F` /
  `SET_WEIGHT3F` / `SET_WEIGHT4F` (0x16C0). Inline values into vertex
  attribute slot 1 (`NV2A_VERTEX_ATTR_WEIGHT`).
- D3D8 surface (W3): `D3DRS_VERTEXBLEND=151`,
  `D3DRS_INDEXEDVERTEXBLENDENABLE=167`. **Xbox enum values for
  VERTEXBLEND are NOT identical to PC D3D8/D3D9** (Cxbx-Reloaded issue
  #2091). Emulator bug class.

**Normalization (W1):** `SET_NORMALIZATION_ENABLE` (0x03A4). Routes to
`NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE`.

**FFP-state-not-externally-documented:** xboxdevwiki's
`NV2A/Fixed_Function_Pipeline` page is a stub (Lighting / Skinning
headers only, last edit 2018-12-29). W1 is the authoritative witness
for the FFP register layout; W3 contributes only the D3D8-API surface.

### A.3 Vertex attribute formats

`NV097_SET_VERTEX_DATA_ARRAY_FORMAT` (0x00001760, 16-slot range)
packs three sub-fields (W1: `nv2a_regs.h:1137`; W2: `nv_regs.h:481-490`):

- `_TYPE` (4 bits at LSB): one of 6 enumerated formats.
- `_SIZE` (4 bits): component count.
- `_STRIDE` (24 bits): byte stride.

The 6 formats (W1: `pgraph.c:2564-2590`, `vertex.c:32-83`; W2:
`nv_regs.h:481-490`; W3 via Cxbx):

| Code | Symbolic | Bytes | Notes |
|---|---|---|---|
| 0 | `UB_D3D` | 1/comp (4 comps fixed) | **BGRA byte order** for D3D color; asserts count==4. |
| 1 | `S1` | 2 | Signed 16-bit, normalized to [-1, 1] (clamped at -1 per `vertex.c:42`). |
| 2 | `F` | 4 | IEEE 32-bit float. |
| 4 | `UB_OGL` | 1 | Unsigned byte, normalized [0, 1]; OpenGL-byte-order RGBA. |
| 5 | `S32K` | 2 | Signed 16-bit, **un-normalized** (raw integer cast to float). |
| 6 | `CMP` | 4 | **Xbox-specific**: 3 signed normalized comps packed in 32 bits as (11,11,10); count==1 mandatory. |

(Type 3 is unallocated.)

**Two distinct UB encodings** (UB_D3D vs UB_OGL) is one of the
emulation pitfalls W3 specifically calls out: "Normalized unsigned
byte (D3D): bytes arranged as ZYXW (BGRA) vs (GL): bytes arranged as
XYZW (RGBA)" — different per attribute in the same vertex stream.

**CMP format pitfall (W1, W3):** the (11,11,10) packed signed format
is decoded CPU-side in xemu (`vertex.c:56-75`); on GL it triggers the
`needs_conversion` integer-attrib path (`gl/vertex.c:124-130`). The
Metal renderer's M5.8 slice added an explicit decoder for this format
because the shader-side `compressed_attrs` path was unreliable. Any
diagnostic XBE exercising CMP needs to verify both paths.

### A.4 Vertex attribute slots

16 slots total; `NV2A_VERTEXSHADER_ATTRIBUTES = 16`. (W1:
`nv2a_regs.h:1507`.) Slot semantics:

| Index | Name (W1 / W2) |
|---|---|
| 0 | POSITION |
| 1 | WEIGHT (skinning) |
| 2 | NORMAL |
| 3 | DIFFUSE |
| 4 | SPECULAR |
| 5 | FOG |
| 6 | POINT_SIZE (W1) / generic ATTR6 (W2) |
| 7 | BACK_DIFFUSE (W1) / generic ATTR7 (W2) |
| 8 | BACK_SPECULAR (W1) / TEX0 (W2) |
| 9-12 | TEXTURE0..3 (W1) / TEX1-TEX4 (W2 — off-by-one shift) |
| 13-15 | RESERVED1..3 (W1) / TEX5-TEX7 (W2) |

**Disagreement between W1 and W2 on slot semantics 6-15.** W1 has
`POINT_SIZE` at slot 6, `BACK_DIFFUSE`/`BACK_SPECULAR` at 7-8, and
`TEXTURE0..3` at 9-12. W2 has slots 6-7 generic and `TEX0..TEX7` at
8-15. xemu (W1) is the authoritative witness because it implements
the actual decoded behavior; nxdk (W2) appears to use a generic
"any slot can be any thing" mapping convention. Diagnostic XBEs
should follow W1's mapping.

**Per-attribute state (W1):** `VertexAttribute` struct
(`pgraph.h:42-61`) tracks `dma_select`, `offset`,
`inline_array_offset`, `inline_value[4]`, `format`, `size`, `count`,
`stride`, `needs_conversion`, `inline_buffer`, `inline_buffer_populated`.

**Attribute offsets:** `NV097_SET_VERTEX_DATA_ARRAY_OFFSET` (0x00001720).
High bit selects between `dma_vertex_a` and `dma_vertex_b` (W1:
`pgraph.c:2603`).

### A.5 Vertex weighting / matrix palette / skinning

See A.2 (`SET_SKIN_MODE`, `SET_WEIGHTNF`). The matrix palette is
implicit in the 4 modelview matrices (`SET_MODEL_VIEW_MATRIX`
0x0480, 4×16 dwords). (W1, W2.)

### A.6 Z perspective / Z format / clip range

- **Z perspective** (W-buffer instead of Z): `SET_CONTROL0`
  bit 16 (`Z_PERSPECTIVE_ENABLE`). (W1: `nv2a_regs.h:904`.) Captured in
  `VshState::z_perspective` and `PshState::z_perspective`
  (`glsl/vsh.h:71`, `glsl/psh.h:71`).
- **Z format** (fixed vs float): `SET_CONTROL0` bit 12 (`Z_FORMAT`).
  (W1: `nv2a_regs.h:903`; W3: xboxdevwiki "depth buffer can be
  configured to be fixed point or floating point" — Xbox-specific; PC
  D3D8 has no float-depth.)
- **Z clip range**: `SET_CLIP_MIN` (0x0394), `SET_CLIP_MAX` (0x0398).
  Registers `NV_PGRAPH_ZCLIPMIN` / `_ZCLIPMAX`. (W1, W2.)
- **Z min/max behavior**: `SET_ZMIN_MAX_CONTROL` (0x1D78) — values
  `_CULL_NEAR_FAR / _ZCLAMP_CULL / _ZCLAMP_CLAMP / _CULL_IGNORE_W`.
  (W1: `nv2a_regs.h:1270-1273`. W2: `nv_regs.h:626-630`.)

---

## B. Primitive assembly

### B.1 Primitive types — `NV097_SET_BEGIN_END` (0x000017FC)

11 primitive types accepted (W1: `nv2a_regs.h:1160-1170`; W2:
`nv_regs.h:505-516`; W3: community-derived via paywalled NGEmu mirror —
flag as least-authoritative witness):

| Value | Symbol |
|---|---|
| 0x00 | `_OP_END` (terminator) |
| 0x01 | `_OP_POINTS` |
| 0x02 | `_OP_LINES` |
| 0x03 | `_OP_LINE_LOOP` |
| 0x04 | `_OP_LINE_STRIP` |
| 0x05 | `_OP_TRIANGLES` |
| 0x06 | `_OP_TRIANGLE_STRIP` |
| 0x07 | `_OP_TRIANGLE_FAN` |
| 0x08 | `_OP_QUADS` |
| 0x09 | `_OP_QUAD_STRIP` |
| 0x0A | `_OP_POLYGON` |

**Xbox-specific**: `QUADS` / `QUAD_STRIP` / `POLYGON` have no
direct OpenGL/Vulkan/Metal equivalents. xemu's GL backend expands them
via geometry shaders; the Apple Silicon fork adds a `XEMU_NATIVE_QUAD`
CPU-expansion path. (W1: `gl/shaders.c:34-70`.)

### B.2 Index / element formats

- **16-bit indices** — `NV097_ARRAY_ELEMENT16` (0x00001800), 2 indices
  packed per dword. (W1: `nv2a_regs.h:1171`. W2.)
- **32-bit indices** — `NV097_ARRAY_ELEMENT32` (0x00001808). (W1, W2.)
- Per-batch element buffer: `pg->inline_elements[NV2A_MAX_BATCH_LENGTH=0x07FFFF]`
  (W1).

### B.3 Four ingestion paths

The Xbox graphics command stream supports four distinct vertex
ingestion paths, all converging at the `flush_draw` op of the
`PGRAPHRenderer` dispatch table. (W1, W2, W3.)

1. **Inline buffer (per-attribute method writes).** Methods like
   `SET_VERTEX3F` (0x1500), `SET_VERTEX4F` (0x1518),
   `SET_NORMAL3F`/`3S`, `SET_DIFFUSE_COLOR4F`/`3F`/`4UB`,
   `SET_SPECULAR_COLOR{4F,3F,4UB}`, `SET_TEXCOORD{0..3}_{2F,4F,2S,4S}`
   (0x1590-0x1630), `SET_FOG_COORD` (0x1698), `SET_WEIGHT{1..4}F`
   (0x169C-0x16C0). Generic per-slot variants:
   `SET_VERTEX_DATA2F_M` (0x1880), `_4F_M` (0x1A00), `_2S` (0x1900),
   `_4UB` (0x1940), `_4S_M` (0x1980). Pushes one vertex's attributes
   into accumulators; `pgraph_finish_inline_buffer_vertex` (W1:
   `vertex.c:121`) flushes. (W1: `nv2a_regs.h:1092-1135`,
   `nv2a_regs.h:1178-1182`. W2.)
2. **Inline elements (indexed).** `NV097_ARRAY_ELEMENT16/32` push
   indices into `pg->inline_elements`; vertex data fetched from per-
   attribute DMA streams configured via `SET_VERTEX_DATA_ARRAY_OFFSET`
   + `SET_VERTEX_DATA_ARRAY_FORMAT`.
3. **Inline arrays (interleaved push).** `NV097_INLINE_ARRAY`
   (0x00001818) pushes raw vertex stream dwords into
   `pg->inline_array[NV2A_MAX_BATCH_LENGTH]`; GL packs and binds per-
   attribute via `pgraph_gl_bind_inline_array` (W1:
   `gl/vertex.c:198-233`). Used with `NV2A_SUPPRESS_COMMAND_INCREMENT`
   (bit 30 of the method header, W2: `nv_regs.h:25`).
4. **Indexed draw arrays.** `NV097_DRAW_ARRAYS` (0x00001810) packs
   `START_INDEX` (24 bits) and `COUNT` (8 bits); `pg->draw_arrays_start[1250]`
   / `_count[1250]` accumulate ranges; coalesced to `glMultiDrawArrays`
   on GL when contiguous, else expanded to `inline_elements`
   (W1: `pgraph.c:2803-2822`, `gl/draw.c:905-1061`).

### B.4 GL primitive-mode mapping (xemu specifics)

`hw/xbox/nv2a/pgraph/gl/shaders.c:34-70` (W1):

- `POINTS → GL_POINTS`, `LINES → GL_LINES`, `LINE_LOOP → GL_LINE_LOOP`,
  `LINE_STRIP → GL_LINE_STRIP`, `TRIANGLES → GL_TRIANGLES`,
  `TRIANGLE_STRIP → GL_TRIANGLE_STRIP`, `TRIANGLE_FAN → GL_TRIANGLE_FAN`.
- `QUADS → GL_LINES_ADJACENCY` + GS (4-vert in, 2-tri out) **OR**
  `GL_TRIANGLES` if `XEMU_NATIVE_QUAD` is on.
- `QUAD_STRIP → GL_LINE_STRIP_ADJACENCY` + GS **OR** `GL_TRIANGLES`
  (native quad).
- `POLYGON → GL_TRIANGLE_FAN` (fill mode) or `GL_LINE_LOOP` (line
  mode).

The Metal renderer follows similar logic but with M5.8 CPU-decoded
attribute streams instead of GS.

---

## C. Rasterization

### C.1 Polygon mode (per face)

- `NV097_SET_FRONT_POLYGON_MODE` (0x0000038C). Values: `V_POINT=0x1B00`,
  `V_LINE=0x1B01`, `V_FILL=0x1B02`. (W1: `nv2a_regs.h:1026-1029`. W2.)
- `NV097_SET_BACK_POLYGON_MODE` (0x00000390). Same values.
- Routed via `NV_PGRAPH_SETUPRASTER_FRONTFACEMODE` (mask 0x3) and
  `_BACKFACEMODE` (mask 0xC). Enum `ShaderPolygonMode` (`vsh_regs.h:199-203`).
- D3D8 cross-reference (W3): `D3DFILLMODE { POINT=1, WIREFRAME=2,
  SOLID=3 }`.

### C.2 Cull / winding

- Cull enable: `NV097_SET_CULL_FACE_ENABLE` (0x00000308). (W1, W2.)
- Cull face: `NV097_SET_CULL_FACE` (0x0000039C). Values: `V_FRONT=0x404,
  V_BACK=0x405, V_FRONT_AND_BACK=0x408`. (W1: `nv2a_regs.h:1033-1036`.)
- Front-face winding: `NV097_SET_FRONT_FACE` (0x000003A0). Values:
  `V_CW=0x900, V_CCW=0x901`.
- D3D8 cross-reference (W3): `D3DCULL { NONE=1, CW=2, CCW=3 }` —
  default CCW.
- **xemu GL note (W1):** winding inverted because clip-space Y is
  flipped (`gl/draw.c:643-646`); this is a fork-specific behavior and
  is *not* a hardware property.

### C.3 Polygon offset / depth bias

- Per-mode enable: `SET_POLY_OFFSET_POINT_ENABLE` (0x0330),
  `_LINE_ENABLE` (0x0334), `_FILL_ENABLE` (0x0338). (W1, W2.)
- Scale factor: `SET_POLYGON_OFFSET_SCALE_FACTOR` (0x0384).
- Bias: `SET_POLYGON_OFFSET_BIAS` (0x0388).
- Registers: `NV_PGRAPH_ZOFFSETBIAS` (0x1AA4), `NV_PGRAPH_ZOFFSETFACTOR`
  (0x1AA8). (W1: `nv2a_regs.h:644-645`.)
- D3D8 cross-reference (W3): `D3DRS_ZBIAS=47` is integer 0..16 — *not*
  the slope-scale model PC D3D9+ uses. Xbox NV2A uses the
  scale+bias form (matches PC D3D9). Emulators that map ZBIAS via
  D3D11/Vulkan slope-scale need empirical tuning (Cxbx uses a
  multiplier; xemu has multiple commits retuning Z handling).
- **xemu special case (W1):** GL renderer disables `GL_POLYGON_OFFSET_*`
  and computes the offset in geometry/fragment shader explicitly
  (`gl/draw.c:648-651`). The Apple Silicon fork's `XEMU_NATIVE_TRI_DEPTH`
  flag derives depth/slope in the fragment shader — making polygon
  offset tests an extra-important diagnostic target.

### C.4 Multisampling

- Global enable: `NV097_SET_ANTI_ALIASING_CONTROL` (0x00001D7C) bit 0.
  Register `NV_PGRAPH_ANTIALIASING_ENABLE`. (W1: `nv2a_regs.h:1274-1275`,
  357-358. W2.)
- Per-surface AA mode: `NV097_SET_SURFACE_FORMAT_ANTI_ALIASING`
  (mask 0x0000F000) (W1: `nv2a_regs.h:887-890`; W2 confirms via
  `NV20_TCL_PRIMITIVE_3D_MULTISAMPLE`):
  - `CENTER_1` (0): no MSAA, 1 sample.
  - `CENTER_CORNER_2` (1): 2× MSAA (width × 2 internal allocation).
  - `SQUARE_OFFSET_4` (2): 4× MSAA (width × 2, height × 2).
- Captured in `pg->surface_shape.anti_aliasing` (W1: `surface.h:32`);
  applied by `pgraph_apply_anti_aliasing_factor` (`pgraph.h:364-382`).
- D3D8 cross-reference (W3): `D3DRS_MULTISAMPLEANTIALIAS=161` (BOOL,
  default TRUE), `D3DRS_MULTISAMPLEMASK=162` (default 0xFFFFFFFF).
- **W3 documentation gap**: the specific `NV097_SET_ANTI_ALIASING_CONTROL`
  bit-encoding for "Quincunx" mode is not externally documented.
  W1 implements it via the surface AA mode; this catalog defers to W1.

### C.5 Smooth vs flat shading + provoking vertex

- Shade model: `NV097_SET_SHADE_MODE` (0x0000037C). Values:
  `V_FLAT=0x1D00, V_SMOOTH=0x1D01`. (W1: `nv2a_regs.h:1021-1023`.)
- Provoking vertex: `NV097_SET_FLAT_SHADE_OP` (0x000009FC) /
  `NV097_SET_PROVOKING_VERTEX`. Values: `LAST=0, FIRST=1`. (W1:
  `nv2a_regs.h:1077-1079`. W2: `nv_regs.h:387-389`.)
- State: `pg->smooth_shading`, `pg->first_vertex_is_provoking`
  (`pgraph.h:204-205`).
- **xemu GL note (W1):** forces `glProvokingVertex(GL_FIRST_VERTEX_CONVENTION)`
  to match Vulkan default (`gl/draw.c:667-668`).
- Smoothing toggles: `SET_POINT_SMOOTH_ENABLE` (0x031C),
  `_LINE_SMOOTH_ENABLE` (0x0320), `_POLY_SMOOTH_ENABLE` (0x0324). (W1:
  `nv2a_regs.h:941-943`. W2.)
- D3D8 cross-reference (W3): `D3DSHADEMODE { FLAT=1, GOURAUD=2,
  PHONG=3 }`. PHONG is enumerated but unimplemented in D3D8.

### C.6 Edge flags / line stipple

- **Edge flag**: `NV097_SET_EDGE_FLAG` (0x000016BC). (W2:
  `nv_regs.h:452`.) **W1 finding**: no method-handler dispatch found
  for edge flags in `methods.h.inc`. **Documentation disagreement**:
  W2 lists the method, W1 does not appear to handle it. Likely a
  no-op in xemu — diagnostic XBE for edge flags should be flagged as
  potentially-unsupported.
- **Line stipple**: `NV097_SET_STIPPLE_ENABLE` (0x147C),
  `NV097_SET_STIPPLE_PATERN_0` (0x1480, 32 dwords). (W2:
  `nv_regs.h:447-449`.) **W1 finding**: no method-handler dispatch
  found. Same disagreement as edge flag — likely a no-op in xemu.
- D3D8 cross-reference (W3): `D3DRS_LINEPATTERN=10` plus
  `D3DRS_LASTPIXEL=16` ("FALSE to enable drawing the last pixel in a
  line or triangle") — both relate to line rasterization rules.

### C.7 Point sprites, point parameters, point smoothing, point size

These are **three distinct features** that the catalog originally
conflated. Codex review pointed out that xemu's `PshState::point_sprite`
flag is currently derived from `POINTSMOOTHENABLE`
(`glsl/psh.c:102`), not from a true `POINTSPRITEENABLE` source. The
true Xbox-side enable for point-sprite *texture-coordinate replacement*
(D3D8's `POINTSPRITEENABLE` semantics) is **unresolved** from these
three witnesses; W3 mentions D3D8 enums but no source attests the
NV097 method. Diagnostic XBE for point sprites must explicitly verify
which combination of flags actually triggers texture-coordinate
replacement on real Xbox.

**C.7.1 Point size (always-on per-vertex):**
- `NV097_SET_POINT_SIZE` (0x0000043C), max 0x1FF (W1: `nv2a_regs.h:498`).

**C.7.2 Point parameters / size attenuation:**
- Enable: `NV097_SET_POINT_PARAMS_ENABLE` (0x00000318) (W1:
  `nv2a_regs.h:940`); register `NV_PGRAPH_CSV0_D_POINTPARAMSENABLE`
  + `NV_PGRAPH_CONTROL_3_POINTPARAMSENABLE`.
- Params (8 floats): `NV097_SET_POINT_PARAMS` (0x00000A30); xemu
  buffers `pg->point_params[8]` (`pgraph.h:218`).
- xemu wires this into the **vertex shader** for size attenuation
  (`glsl/vsh.c:131`) — controls quadratic distance attenuation
  A + B·d + C·d², not texture-coordinate replacement.
- D3D8 cross-reference (W3): `D3DRS_POINTSCALEENABLE=157`,
  `D3DRS_POINTSCALE_A/B/C` (158-160), `D3DRS_POINTSIZE_MIN/MAX`
  (155, 166).

**C.7.3 Point smoothing (per-pixel coverage):**
- Enable: `NV097_SET_POINT_SMOOTH_ENABLE` (0x031C) (W1, W2).
- Register `NV_PGRAPH_SETUPRASTER_POINTSMOOTH`.
- xemu currently maps this to `PshState::point_sprite`
  (`glsl/psh.c:102`) — that is **not** the same semantic as D3D8's
  `POINTSPRITEENABLE` (texture-coordinate replacement). This is
  either an xemu impl quirk or an undocumented Xbox shorthand;
  diagnostic XBE should clarify.

**C.7.4 True point-sprite (texture-coordinate replacement):**
- D3D8 cross-reference (W3): `D3DRS_POINTSPRITEENABLE=156`. Archived
  MS doc says "Not supported in Windows CE" but Xbox NV2A is widely
  understood to support it.
- **Xbox-side enable source: UNRESOLVED.** No witness in this
  research pass attests which NV097 method or `NV_PGRAPH_*` register
  flips texture-coord replacement on for point primitives. Could be
  a bit in `SET_CONTROL0` or an undocumented combiner-stage flag.
- Diagnostic XBE for point sprites must be designed around this
  uncertainty: render points with each candidate flag combination,
  inspect texture-coordinate output via fragment shader, identify
  which flag controls replacement.

### C.8 Window clip — Xbox-specific 8-rect scissor

- Type: `NV097_SET_WINDOW_CLIP_TYPE` (0x000002B4) — inclusive vs
  exclusive. (W1.)
- 8 horizontal rects: `NV097_SET_WINDOW_CLIP_HORIZONTAL` (0x2C0).
- 8 vertical rects: `_VERTICAL` (0x2E0).
- Registers `NV_PGRAPH_WINDOWCLIPX0..7` and `_Y0..7` (W1:
  `nv2a_regs.h:618-637`).
- State: `PshState::window_clip_exclusive` (W1: `glsl/psh.h:67`).
- This is **Xbox-specific**; PC D3D8 has only one scissor rect.

---

## D. Pixel pipeline / Register combiners

NV2A has no programmable pixel shader in the SM2.0+ sense. Pixel
shading is configured as 8 register-combiner stages plus a final
combiner, with an Xbox-specific 4-stage texture-shader program that
selects per-stage texture-mode behaviour. (W1, W2, W3.)

### D.1 Combiner control

- `NV097_SET_COMBINER_CONTROL` (0x00001E60). (W1, W2.)
- Register `NV_PGRAPH_COMBINECTL`.
- Carries `MUX_LSB` / `MUX_MSB` (R0.a-mux selector) and
  `SAME_C0` / `UNIQUE_C0` / `SAME_C1` / `UNIQUE_C1` flags. (W1:
  `psh_regs.h:91-101`.)
- Number of stages: derived from combiner control; up to 8.

**NV2A-specific deviation from NV_register_combiners (W3):**
`FACTOR_SAME_FACTOR_ALL` causes "constant-colors for all other stages
are taken from the very first stage. But on NV2A, the final-combiner
does always have unique constants (even using FACTOR#_SAME_FACTOR_ALL)
from all other stages."

### D.2 Per-stage ICW / OCW

| Word | NV097 method | Stages | Register |
|---|---|---|---|
| Color ICW (input) | `SET_COMBINER_COLOR_ICW` (0x00000AC0) | 8 | `NV_PGRAPH_COMBINECOLORI0..` |
| Color OCW (output) | `SET_COMBINER_COLOR_OCW` (0x00001E40) | 8 | `NV_PGRAPH_COMBINECOLORO0..` |
| Alpha ICW | `SET_COMBINER_ALPHA_ICW` (0x00000260) | 8 | `NV_PGRAPH_COMBINEALPHAI0..` |
| Alpha OCW | `SET_COMBINER_ALPHA_OCW` (0x00000AA0) | 8 | `NV_PGRAPH_COMBINEALPHAO0..` |
| Per-stage C0 | `SET_COMBINER_FACTOR0` (0x00000A60) | 8 | `NV_PGRAPH_COMBINEFACTOR0` |
| Per-stage C1 | `SET_COMBINER_FACTOR1` (0x00000A80) | 8 | `NV_PGRAPH_COMBINEFACTOR1` |
| Final spec/fog CW0 | `SET_COMBINER_SPECULAR_FOG_CW0` (0x288) | — | `NV_PGRAPH_COMBINESPECFOG0` |
| Final spec/fog CW1 | `SET_COMBINER_SPECULAR_FOG_CW1` (0x28C) | — | `NV_PGRAPH_COMBINESPECFOG1` |

(W1, W2 — full agreement.)

### D.3 Inputs (`enum PS_REGISTER`, W1: `psh_regs.h:67-89`)

| Code | Register | Notes |
|---|---|---|
| 0x00 | `ZERO` / `DISCARD` | Same index — writes are discarded, reads return zero. (W3 confirms; *NV2A-specific* deviation from NV_register_combiners.) |
| 0x01 | `C0` | Per-stage constant 0 |
| 0x02 | `C1` | Per-stage constant 1 |
| 0x03 | `FOG` | Fog factor |
| 0x04 | `V0` (DIFFUSE) | Per-vertex diffuse |
| 0x05 | `V1` (SPECULAR) | Per-vertex specular |
| 0x08-0x0B | `T0`-`T3` | Texture stage outputs |
| 0x0C-0x0D | `R0`-`R1` | Combiner temp registers |
| 0x0E | `V1R0_SUM` | Special final-combiner input |
| 0x0F | `EF_PROD` | AB·CD product, available as input |

Pseudo-constants `ONE`, `NEGATIVE_ONE`, `ONE_HALF`, `NEGATIVE_ONE_HALF`
derived via input mappings (D.4).

### D.4 Input mappings (`enum PS_INPUTMAPPING`, W1: `psh_regs.h:55-65`)

| Code | Mapping | Formula |
|---|---|---|
| 0x00 | `UNSIGNED_IDENTITY` | max(0, x) |
| 0x20 | `UNSIGNED_INVERT` | 1 - max(0, x) |
| 0x40 | `EXPAND_NORMAL` | 2·max(0, x) - 1 |
| 0x60 | `EXPAND_NEGATE` | 1 - 2·max(0, x) |
| 0x80 | `HALFBIAS_NORMAL` | max(0, x) - 0.5 |
| 0xA0 | `HALFBIAS_NEGATE` | 0.5 - max(0, x) |
| 0xC0 | `SIGNED_IDENTITY` | x (no clamp) |
| 0xE0 | `SIGNED_NEGATE` | -x |

The first two are the only ones legal in the final combiner.

### D.5 Output operations (`enum PS_COMBINEROUTPUT`, W1: `psh_regs.h:103-124`)

Per-stage RGB/ALPHA output produces three results from inputs A/B/C/D:

- **AB**: multiply *or* `AB_DOT_PRODUCT` (RGB only, code 0x02).
- **CD**: multiply *or* `CD_DOT_PRODUCT` (RGB only, code 0x01).
- **Third output**: `AB_CD_SUM` (0x00) *or* `AB_CD_MUX(R0.a)` (0x04).

**Output scale modifiers**: `IDENTITY=0x00`, `BIAS=0x08` (y=x-0.5),
`SHIFTLEFT_1=0x10` (×2), `SHIFTLEFT_1_BIAS=0x18` ((x-0.5)×2),
`SHIFTLEFT_2=0x20` (×4), `SHIFTRIGHT_1=0x30` (÷2).

**Special RGB-only flags**: `AB_BLUE_TO_ALPHA=0x80`,
`CD_BLUE_TO_ALPHA=0x40` — write the blue result of A/B (or C/D)
computations to the alpha channel of the RGB output register
(W3: confirmed Xbox-specific deviation).

### D.6 Channel selector (`enum PS_CHANNEL`, W1: `psh_regs.h:126-131`)

- `RGB=0x00` / `BLUE=0x00` (alpha-source mode).
- `ALPHA=0x10` (use alpha component as RGB or alpha source).

**NV2A-specific deviation (W3):** "Single ALPHA flag for swizzle: 0
⇒ `.rgb`/`.b`, 1 ⇒ `.aaa`/`.a`. Different from NV_register_combiners
where each swizzle has its own constant."

### D.7 Final combiner

- Settings (`enum PS_FINALCOMBINERSETTING`, W1: `psh_regs.h:134-141`):
  `CLAMP_SUM=0x80` (V1+R0 clamp), `COMPLEMENT_V1=0x40`,
  `COMPLEMENT_R0=0x20`.
- Final inputs A/B/C/D/E/F/G driven by `final_inputs_0`/`final_inputs_1`
  in `PshState` (W1: `glsl/psh.h:41-42`).
- Specific Xbox final-combiner inputs (W2): `REG_SPECLIT=0xE`,
  `REG_EF_PROD=0xF`.
- Final formula (W3, NV_register_combiners): `RGB_out = AB +
  (1-A)·C + D` and `alpha_out = G`. NV2A's specific path computes:
  `OUT = D + (E·F)·(1-G) + G·V1`-style; xemu translation in `glsl/psh.c`.
- **NV2A-specific deviation (W3):** "MUX (final combiner): if
  spare0_alpha ≥ 0.5, performs C·D, otherwise A·B."

### D.8 Texture stage program — Xbox-specific shader stages

`NV097_SET_SHADER_STAGE_PROGRAM` (0x00001E70) — 5-bit field per stage
× 4 stages (W1: `nv2a_regs.h:1302`, `psh_regs.h:31-53`; W2:
`nv_regs.h:694-747`; W3 confirms 16-modes).

**xemu (W1) lists 19 modes; xboxdevwiki (W3) lists "16 from NONE to
DOTPRODUCT" — slight undercount on W3. The xemu list is more
complete:**

| Value | Mode | Valid stages |
|---|---|---|
| 0x00 | NONE | 0,1,2,3 |
| 0x01 | PROJECT2D | 0,1,2,3 |
| 0x02 | PROJECT3D | 0,1,2,3 |
| 0x03 | CUBEMAP | 0,1,2,3 |
| 0x04 | PASSTHRU | 0,1,2,3 |
| 0x05 | CLIPPLANE | 0,1,2,3 |
| 0x06 | BUMPENVMAP | 1,2,3 |
| 0x07 | BUMPENVMAP_LUM | 1,2,3 |
| 0x08 | BRDF | 2,3 |
| 0x09 | DOT_ST | 2,3 |
| 0x0A | DOT_ZW | 2,3 |
| 0x0B | DOT_RFLCT_DIFF | 2 |
| 0x0C | DOT_RFLCT_SPEC | 3 |
| 0x0D | DOT_STR_3D | 3 |
| 0x0E | DOT_STR_CUBE | 3 |
| 0x0F | DPNDNT_AR | 1,2,3 |
| 0x10 | DPNDNT_GB | 1,2,3 |
| 0x11 | DOTPRODUCT | 1,2 |
| 0x12 | DOT_RFLCT_SPEC_CONST | 3 |

**This is Xbox-specific beyond NV20** — bump-environment, BRDF,
dot-product reflection mapping, and dependent-texture chains all
driven from this register.

### D.9 Other shader-stage state

- `NV097_SET_SHADER_OTHER_STAGE_INPUT` (0x00001E78) (W1, W2:
  `nv_regs.h:749-758`).
- `NV097_SET_DOT_RGBMAPPING` (0x00001E74). Values (W1:
  `psh_regs.h:143-153`): `ZERO_TO_ONE`, `MINUS1_TO_1_D3D`,
  `MINUS1_TO_1_GL`, `MINUS1_TO_1`, `HILO_1`, `HILO_HEMISPHERE_D3D`,
  `HILO_HEMISPHERE_GL`, `HILO_HEMISPHERE`.
- `NV097_SET_SHADER_CLIP_PLANE_MODE` (0x000017F8) — user-defined clip
  plane via the texture stage `CLIPPLANE` mode.

### D.10 Combiner texture-coord state (in PshState, W1: `glsl/psh.h:48-62`)

- `rect_tex[4]` — rectangular (non-pow-2) texture flag per stage.
- `snorm_tex[4]` — per-channel signed-vs-unsigned interpretation
  active.
- `tex_x8y24[4]` — depth-as-texture format flag.
- `dim_tex[4]` — texture dimensionality (1D/2D/3D).
- `tex_cubemap[4]` — cubemap flag.
- `compare_mode[4][4]` — per-stage, per-channel compare mode.
- `alphakill[4]` — alpha-kill enable.
- `colorkey_mode[4]` — color-key mode.
- `conv_tex[4]` — convolution filter mode (Quincunx, Gaussian).
- `border_logical_size[4][3]` / `border_inv_real_size[4][3]` — border
  geometry.
- `shadow_map[4]` — shadow-map mode.
- `shadow_depth_func` — shadow-map comparison function.

### D.11 Shadow / depth-shadow comparison

- Func: `NV097_SET_SHADOW_DEPTH_FUNC` (0x00001E6C). Values 0..7
  (NEVER..ALWAYS). (W1: `nv2a_regs.h:1293-1301`. W2.)
- Z-slope threshold: `NV097_SET_SHADOW_ZSLOPE_THRESHOLD` (0x00001E68).

### D.12 Color key / alpha kill — Xbox texture-side

- Per-stage color key: `NV097_SET_COLOR_KEY_COLOR` (0x00000AE0, 4
  stages). (W1: `nv2a_regs.h:1088`.)
- Mode enum: `PS_COLORKEYMODE { NONE, KILL_ALPHA, KILL_COLOR_AND_ALPHA,
  DISCARD }` (W1: `psh_regs.h:155-160`).
- Color-key mode register: `NV_PGRAPH_TEXCTL0_0_COLORKEYMODE`.
- Alpha kill: `NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN`.

### D.13 Bump-environment mapping

- Matrix: `NV097_SET_TEXTURE_SET_BUMP_ENV_MAT` (0x00001B28, range 4
  stages × 4 floats). Registers `NV_PGRAPH_BUMPMAT00/01/10/11`.
- Scale: `NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE` (0x1B38).
  `NV_PGRAPH_BUMPSCALE1`.
- Offset: `NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET` (0x1B3C).
  `NV_PGRAPH_BUMPOFFSET1`.
- PshUniform decls: `bumpMat`, `bumpOffset`, `bumpScale`
  (W1: `glsl/psh.h:81-95`).
- D3D8 cross-reference (W3): `D3DTSS_BUMPENVMAT00/01/10/11` (TSS
  values 7-10), `D3DTSS_BUMPENVLSCALE` (22), `D3DTSS_BUMPENVLOFFSET`
  (23), used in conjunction with `D3DTOP_BUMPENVMAP=22` and
  `D3DTOP_BUMPENVMAPLUMINANCE=23`. Requires NV2A texture shader
  programs (D.8 mode 6/7), not just register combiners.

---

## E. Texture system

`NV2A_MAX_TEXTURES = 4` stages.

### E.1 Texture formats (full enumeration)

W1 (`texture.c:26-79`, `kelvin_color_format_info_map[66]`) and W2
(`nv_regs.h:537-579`) enumerate symbolic format codes; W3 confirms
the format families and named pitfalls. The two source-code witnesses
do **not** enumerate identical sets — see "Per-witness disagreement"
column. The union of all codes attested by at least one witness is
listed below; bpp is bytes per pixel for non-compressed, "block" for
compressed.

| Code | Symbolic | bpp | Layout | W1 | W2 | Notes |
|---|---|---|---|:-:|:-:|---|
| 0x00 | `SZ_Y8` | 1 | swizzled | ✓ | ✓ | luminance |
| 0x01 | `SZ_AY8` | 1 | swizzled | ✓ | ✓ | combined A and Y |
| 0x02 | `SZ_A1R5G5B5` | 2 | swizzled | ✓ | ✓ | |
| 0x03 | `SZ_X1R5G5B5` | 2 | swizzled | ✓ | ✓ | unused bit |
| 0x04 | `SZ_A4R4G4B4` | 2 | swizzled | ✓ | ✓ | |
| 0x05 | `SZ_R5G6B5` | 2 | swizzled | ✓ | ✓ | |
| 0x06 | `SZ_A8R8G8B8` | 4 | swizzled | ✓ | ✓ | |
| 0x07 | `SZ_X8R8G8B8` | 4 | swizzled | ✓ | ✓ | unused channel |
| 0x0B | `SZ_I8_A8R8G8B8` | 1 | swizzled | ✓ | ✓ | **palettized P8** → ARGB |
| 0x0C | `L_DXT1_A1R5G5B5` | block | linear | ✓ | ✓ | DXT1 / S3TC |
| 0x0E | `L_DXT23_A8R8G8B8` | block | linear | ✓ | ✓ | DXT3 |
| 0x0F | `L_DXT45_A8R8G8B8` | block | linear | ✓ | ✓ | DXT5 |
| 0x10 | `LU_IMAGE_A1R5G5B5` | 2 | linear | ✓ | ✓ | |
| 0x11 | `LU_IMAGE_R5G6B5` | 2 | linear | ✓ | ✓ | |
| 0x12 | `LU_IMAGE_A8R8G8B8` | 4 | linear | ✓ | ✓ | |
| 0x13 | `LU_IMAGE_Y8` | 1 | linear | ✓ | ✓ | |
| 0x16 | `LU_IMAGE_R8B8` | 2 | linear | — | ✓ | **W2-only** — `nv_regs.h:554`. xemu may not handle. |
| 0x17 | `LU_IMAGE_G8B8` | 2 | linear | ✓ | ✓ | xemu issue #320: MechAssault 2 |
| 0x19 | `SZ_A8` | 1 | swizzled | ✓ | ✓ | |
| 0x1A | `SZ_A8Y8` | 2 | swizzled | ✓ | ✓ | |
| 0x1B | `LU_IMAGE_AY8` | 1 | linear | ✓ | ✓ | |
| 0x1C | `LU_IMAGE_X1R5G5B5` | 2 | linear | ✓ | ✓ | |
| 0x1D | `LU_IMAGE_A4R4G4B4` | 2 | linear | ✓ | ✓ | |
| 0x1E | `LU_IMAGE_X8R8G8B8` | 4 | linear | ✓ | ✓ | |
| 0x1F | `LU_IMAGE_A8` | 1 | linear | ✓ | ✓ | |
| 0x20 | `LU_IMAGE_A8Y8` | 2 | linear | ✓ | ✓ | |
| 0x24 | `LC_IMAGE_CR8YB8CB8YA8` | 2 | linear | ✓ | ✓ | YUY2 (4:2:2 YUV) |
| 0x25 | `LC_IMAGE_YB8CR8YA8CB8` | 2 | linear | ✓ | ✓ | UYVY; xemu issue #320: MotoGP, NHL Hitz |
| 0x27 | `SZ_R6G5B5` | 2 | swizzled | ✓ | ✓ | |
| 0x28 | `SZ_G8B8` | 2 | swizzled | ✓ | ✓ | |
| 0x29 | `SZ_R8B8` | 2 | swizzled | ✓ | ✓ | |
| 0x2C | `SZ_DEPTH_Y16_FIXED` | 2 | swizzled | ✓ | ✓ | depth-as-texture; xemu issue #320: Just Cause |
| 0x2E | `LU_IMAGE_DEPTH_X8_Y24_FIXED` | 4 | linear | ✓ | ✓ | depth-as-texture |
| 0x2F | `LU_IMAGE_DEPTH_X8_Y24_FLOAT` | 4 | linear | ✓ | — | **W1-only** — `nv2a_regs.h:1224`, `texture.c:65`. nxdk pbkit doesn't expose. |
| 0x30 | `LU_IMAGE_DEPTH_Y16_FIXED` | 2 | linear | ✓ | ✓ | depth-as-texture |
| 0x31 | `LU_IMAGE_DEPTH_Y16_FLOAT` | 2 | linear | ✓ | ✓ | depth-as-texture; xemu issue #320: FireBlade, Backyard Wrestling |
| 0x35 | `LU_IMAGE_Y16` | 2 | linear | ✓ | ✓ | |
| 0x3A | `SZ_A8B8G8R8` | 4 | swizzled | ✓ | ✓ | channel-swapped |
| 0x3B | `SZ_B8G8R8A8` | 4 | swizzled | ✓ | ✓ | channel-swapped; xemu issue #320: Unreal Championship 2 |
| 0x3C | `SZ_R8G8B8A8` | 4 | swizzled | ✓ | ✓ | channel-swapped |
| 0x3F | `LU_IMAGE_A8B8G8R8` | 4 | linear | ✓ | ✓ | |
| 0x40 | `LU_IMAGE_B8G8R8A8` | 4 | linear | ✓ | ✓ | |
| 0x41 | `LU_IMAGE_R8G8B8A8` | 4 | linear | ✓ | ✓ | |

**Total**: 42 distinct codes attested. **W1 attests 41**; **W2
attests 41**; the two single-witness codes (0x16 W2-only, 0x2F
W1-only) are real divergences and the diagnostic XBE library should
explicitly cover both — code 0x16 to confirm xemu handles it at all,
code 0x2F to confirm nxdk programs that bypass the SDK enum can hit
the format.

**Format-family prefix conventions:**

- `SZ_*` — Xbox **swizzled** (Z-order / Morton) tile format.
- `LU_*` — **linear unswizzled** (pitched, aligned).
- `LC_*` — **linear chroma-subsampled** (YUV variants).
- `L_*` — **linear S3TC compressed**.

**Known emulation pitfalls (W3):** xemu issue #320 enumerates
historically-unimplemented texture-format codes that ship in real
games:

- 0x17 G8B8 (MechAssault 2)
- 0x2C SZ_DEPTH_Y16_FIXED (Just Cause)
- 0x25 UYVY (MotoGP, NHL Hitz)
- 0x31 LU_IMAGE_DEPTH_Y16_FLOAT (FireBlade, Backyard Wrestling)
- 0x3B B8G8R8A8 (Unreal Championship 2)

Diagnostic XBE coverage should explicitly target all 41 codes; a
reduced suite at minimum should cover the families ({SZ, LU, LC, L}
× {ARGB, palettized, depth-as-texture, channel-swapped, YUV,
compressed}).

### E.2 Compressed texture formats

DXT1 (code 0x0C): 8-byte blocks. DXT3 (0x0E) and DXT5 (0x0F): 16-byte
blocks. (W1: `texture.c:188-199`. W2. W3: D3D8 D3DFORMAT FOURCC for
DXT1/2/3/4/5; Xbox supports DXT1/3/5.)

- xemu has its own software decoder at `pgraph/s3tc.c` / `s3tc.h`.
- GL passes through as `GL_COMPRESSED_RGBA_S3TC_DXT{1,3,5}_EXT`
  (W1: `gl/constants.h:182-187`).
- Compressed-format check helper: `pgraph_is_texture_format_compressed`
  (`pgraph.h:341-346`).

### E.3 Filter modes

`NV097_SET_TEXTURE_FILTER` (0x00001B14) packs (W1: `nv2a_regs.h:1245-1252`):

- `MIPMAP_LOD_BIAS` (low 13 bits, signed 13-bit / 256 fixed point —
  helper `pgraph_convert_lod_bias_to_float` at `texture.h:69-76`).
- `MIN` (bits 16-23): min filter.
- `MAG` (bits 24-31): mag filter.
- `ASIGNED`/`RSIGNED`/`GSIGNED`/`BSIGNED` (bits 28-31): per-channel
  signed-vs-unsigned interpretation.

**Min-filter values (W1: `nv2a_regs.h:567-573`):**

| Value | Symbolic | Behavior |
|---|---|---|
| 1 | `BOX_LOD0` | nearest, no mips |
| 2 | `TENT_LOD0` | linear, no mips |
| 3 | `BOX_NEARESTLOD` | nearest mip / nearest tap |
| 4 | `TENT_NEARESTLOD` | nearest mip / linear tap |
| 5 | `BOX_TENT_LOD` | linear mip / nearest tap |
| 6 | `TENT_TENT_LOD` | linear mip / linear tap (trilinear) |
| 7 | `CONVOLUTION_2D_LOD0` | convolution kernel mode |

**Mag-filter values (verified from W1's GL map, `gl/constants.h:40-46`):**

- 1 = nearest
- 2 = linear
- 4 = convolution (treated as linear, FIXME)

**Convolution kernel (W1: `nv2a_regs.h:563-565`, `psh_regs.h:184-188`):**

- `QUINCUNX = 1`
- `GAUSSIAN_3 = 2`

**Anisotropy (W1: `nv2a_regs.h:547`):** `NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY`
mask 0x30 — 2-bit field giving up to 4× anisotropic filtering per
stage. (W3: Wikipedia confirms NV2A supports anisotropic.)

D3D8 cross-reference (W3): `D3DTEXTUREFILTERTYPE { NONE=0, POINT=1,
LINEAR=2, ANISOTROPIC=3, FLATCUBIC=4, GAUSSIANCUBIC=5 }`. NV2A's
QUINCUNX / GAUSSIAN_3 modes don't have direct D3D8 names but are
related to FLATCUBIC/GAUSSIANCUBIC.

### E.4 Wrap modes

`NV097_SET_TEXTURE_ADDRESS` (0x00001B08), per (U,V,P,Q) channel via
`NV_PGRAPH_TEXADDRESS0_ADDRU/ADDRV/ADDRP` (W1: `nv2a_regs.h:529-540`):

| Value | Mode |
|---|---|
| 1 | `WRAP` (repeat) |
| 2 | `MIRROR` |
| 3 | `CLAMP_TO_EDGE` |
| 4 | `BORDER` |
| 5 | `CLAMP_OGL` (true `GL_CLAMP`) |

GL map at `gl/constants.h:48-55` — note xemu maps OpenGL's "true
CLAMP" to `GL_CLAMP_TO_EDGE` (approximate).

D3D8 cross-reference (W3): `D3DTEXTUREADDRESS { WRAP=1, MIRROR=2,
CLAMP=3, BORDER=4, MIRRORONCE=5 }`. NV2A's MIRROR is full mirror
(not D3D8's MIRRORONCE — once reflected then clamp); diagnostic XBE
must explicitly test which behavior is implemented.

Per-channel cylinder-wrap toggles: `_WRAP_U`, `_WRAP_V`, `_WRAP_P`,
`_WRAP_Q` (W1: `nv2a_regs.h:535-540`, W2: `nv_objects.h:1187-1191`).

### E.5 LOD / mipmap selection

- LOD bias: `NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS` mask 0x00001FFF;
  fixed-point sign-extended 13-bit / 256.
- Min/max LOD clamp: `NV097_SET_TEXTURE_CONTROL0` packs
  `MIN_LOD_CLAMP` (mask 0x3FFC0000) and `MAX_LOD_CLAMP` (mask
  0x0003FFC0).
- Mipmap level count: `NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS` mask
  0x000F0000.
- Stage enable: `NV097_SET_TEXTURE_CONTROL0_ENABLE` bit 30.

(W1: `nv2a_regs.h:548-549, 562, 1234, 1239-1242`. W2.)

### E.6 Cubemap

- Enable: `NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE` bit 2.
- Face alignment: `NV2A_CUBEMAP_FACE_ALIGNMENT = 128`. (W1.)

### E.7 3D textures

- Dimensionality: `NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY` mask
  0x000000F0. Values 1=1D, 2=2D, 3=3D.
- Base depth: `NV097_SET_TEXTURE_FORMAT_BASE_SIZE_P` mask 0xF0000000.
- Length math at `texture.c:166-219`. (W1.)

### E.8 Palettized — Xbox-specific

- Format `SZ_I8_A8R8G8B8` (0x0B): 8-bit indices into RGBA palette.
- Palette config: `NV097_SET_TEXTURE_PALETTE` (0x00001B20). Fields:
  - `CONTEXT_DMA` (1 bit): DMA channel select.
  - `LENGTH` (2 bits): 256/128/64/32 entries.
  - `OFFSET`: VRAM offset of palette.
- Length helper: `pgraph_get_texture_palette_phys_addr_length`
  (`texture.c:128-164`).

(W1, W2.)

**Pitfall (W3):** Cxbx-Reloaded comment — "P8, R8B8 and others are
not available on host Direct3D and are converted into ARGB."
Conversion is mandatory on hosts without paletted/luminance support;
emulators must redo the conversion per-frame if the palette changes
(via the palette-update path).

### E.9 Swizzled vs linear texture layouts

- `linear` flag in `BasicColorFormatInfo` (W1: `texture.h:47-51`).
  `LU_*`/`LC_*`/`L_*` formats are linear; `SZ_*` formats are Xbox
  swizzled.
- Software swizzle / unswizzle: `swizzle_box`/`unswizzle_box`
  (`pgraph/swizzle.h:26-35`); rect helpers `swizzle_rect`/`unswizzle_rect`
  (`swizzle.h:46-66`); SIMD multiversioned implementation
  (`swizzle.c:180-181`).

**Pitfall (W3):** Z-order interleaves x/y bits. **Mipmap chain follows
swizzle order, not linear-derived offset** — level-N base is
`level0_base + swizzle(level_offset)`, not `level0_base + sum(level_sizes)`.
nv2a-trace ships a `Texture.py` decoder; multiple xemu commits (e.g.
`f0abe3c4`) repeatedly fix swizzle handling for R8B8/G8B8 and unusual
format codes. Diagnostic XBEs must include a swizzle test that
explicitly walks each mip level and verifies expected pixel content.

### E.10 Texture coord generation (texgen)

See A.2 for the per-stage / per-channel texgen modes. Diagnostic
coverage needs all 6 modes (DISABLE / EYE_LINEAR / OBJECT_LINEAR /
SPHERE_MAP / REFLECTION_MAP / NORMAL_MAP) per stage.

### E.11 Multi-texturing

- 4 stages. Stage active query: `pgraph_is_texture_stage_active(pg,
  stage)` extracts a 5-bit mode from `NV_PGRAPH_SHADERPROG`
  (W1: `pgraph.h:327-332`).
- Stage enable query: `pgraph_is_texture_enabled(pg, idx)` reads
  `NV_PGRAPH_TEXCTL0_0_ENABLE` (`pgraph.h:334-339`).
- Stage-to-combiner mapping is implicit through `T0..T3` register
  inputs in combiner equations.

Per-stage register sets (W1):

| Method | Per-stage offset | Base address |
|---|---|---|
| `SET_TEXTURE_OFFSET` | +0x00 | 0x1B00 |
| `SET_TEXTURE_FORMAT` | +0x04 | 0x1B04 |
| `SET_TEXTURE_ADDRESS` | +0x08 | 0x1B08 |
| `SET_TEXTURE_CONTROL0` | +0x0C | 0x1B0C |
| `SET_TEXTURE_CONTROL1` | +0x10 | 0x1B10 |
| `SET_TEXTURE_FILTER` | +0x14 | 0x1B14 |
| `SET_TEXTURE_IMAGE_RECT` | +0x1C | 0x1B1C |
| `SET_TEXTURE_PALETTE` | +0x20 | 0x1B20 |
| `SET_TEXTURE_BORDER_COLOR` | +0x24 | 0x1B24 |
| Bumpenv mat / scale / offset | +0x28..+0x3C | 0x1B28..0x1B3C |

(Each per-stage offset increments by 0x40 between stages — base
addresses for stage N = 0x1B00 + N×0x40.)

### E.12 Border color

- Per-stage: `NV097_SET_TEXTURE_BORDER_COLOR` (0x00001B24).
- Border source: `NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE` bit 3 —
  `TEXTURE=0` / `COLOR=1`.
- PshUniforms: `border_logical_size[4][3]` /
  `border_inv_real_size[4][3]`.

### E.13 Texture image rect / pitch

- Linear textures: `NV097_SET_TEXTURE_IMAGE_RECT` (0x00001B1C) packs
  `WIDTH` (mask 0xFFFF0000) and `HEIGHT` (mask 0x0000FFFF).
- Image pitch: `NV097_SET_TEXTURE_CONTROL1_IMAGE_PITCH` (mask
  0xFFFF0000) — register `NV_PGRAPH_TEXCTL1_0_IMAGE_PITCH`.

(W1, W2.)

### E.14 Texture DMA selector (DMA A vs DMA B)

Codex review surfaced this section was missing. Texture image data
(and palettes) are fetched from one of two DMA channels per stage,
selected by `NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA` (low 2 bits of
`NV097_SET_TEXTURE_FORMAT`):

- `NV097_SET_CONTEXT_DMA_A` (0x0000018C) → `pg->dma_a` (W1:
  `pgraph.c:1056`).
- `NV097_SET_CONTEXT_DMA_B` (0x00000190) → `pg->dma_b`.
- Per-stage selector bits in `SET_TEXTURE_FORMAT` low 2 bits select
  between channel A (0) and channel B (1) (W1: `pgraph.c:2675`).
- Texture/palette base-address resolution maps the offset through
  the selected DMA channel's translation
  (W1: `texture.c:92` for image, `texture.c:153` for palette).

**Why this matters:** xemu's texture-binding code paths through DMA
A and DMA B are nominally equivalent but hit different address-
translation code. A diagnostic XBE that binds the same texture
data through DMA A vs DMA B and verifies pixel-identical output
catches address-space bugs that single-DMA tests miss. The full
test sweep should also exercise the DMA-channel switch mid-frame
(e.g., stage 0 DMA A, stage 1 DMA B in the same draw).

This is a candidate for inclusion in the priority XBE list (added
to §6 build priority as XBE #16).

---

## F. Color / Blend / Output merger (ROP)

### F.1 Blend

- Enable: `NV097_SET_BLEND_ENABLE` (0x00000304). (W1, W2.)
- Source factor: `NV097_SET_BLEND_FUNC_SFACTOR` (0x00000344). 16
  values:

  | Value | Symbolic |
  |---|---|
  | 0x0000 | `ZERO` |
  | 0x0001 | `ONE` |
  | 0x0300 | `SRC_COLOR` |
  | 0x0301 | `ONE_MINUS_SRC_COLOR` |
  | 0x0302 | `SRC_ALPHA` |
  | 0x0303 | `ONE_MINUS_SRC_ALPHA` |
  | 0x0304 | `DST_ALPHA` |
  | 0x0305 | `ONE_MINUS_DST_ALPHA` |
  | 0x0306 | `DST_COLOR` |
  | 0x0307 | `ONE_MINUS_DST_COLOR` |
  | 0x0308 | `SRC_ALPHA_SATURATE` |
  | 0x8001 | `CONSTANT_COLOR` |
  | 0x8002 | `ONE_MINUS_CONSTANT_COLOR` |
  | 0x8003 | `CONSTANT_ALPHA` |
  | 0x8004 | `ONE_MINUS_CONSTANT_ALPHA` |

  (W1, W2.)
- Dest factor: `NV097_SET_BLEND_FUNC_DFACTOR` (0x00000348) — same 16
  values. (W1: `nv2a_regs.h:974-989`. W2.)
- Blend constant: `NV097_SET_BLEND_COLOR` (0x0000034C). Register
  `NV_PGRAPH_BLENDCOLOR`.
- Blend equation: `NV097_SET_BLEND_EQUATION` (0x00000350). 7 values:

  | Value | Symbolic |
  |---|---|
  | 0x8006 | `FUNC_ADD` |
  | 0x8007 | `MIN` |
  | 0x8008 | `MAX` |
  | 0x800A | `FUNC_SUBTRACT` |
  | 0x800B | `FUNC_REVERSE_SUBTRACT` |
  | 0xF005 | `FUNC_REVERSE_SUBTRACT_SIGNED` (Xbox-specific) |
  | 0xF006 | `FUNC_ADD_SIGNED` (Xbox-specific) |

  (W1: `nv2a_regs.h:992-998`. W2.)

D3D8 cross-reference (W3):
- `D3DBLEND { ZERO=1, ONE=2, SRCCOLOR=3, INVSRCCOLOR=4, SRCALPHA=5,
  INVSRCALPHA=6, DESTALPHA=7, INVDESTALPHA=8, DESTCOLOR=9, INVDESTCOLOR=10,
  SRCALPHASAT=11, BOTHSRCALPHA=12, BOTHINVSRCALPHA=13 }`.
- `D3DBLENDOP { ADD=1, SUBTRACT=2, REVSUBTRACT=3, MIN=4, MAX=5 }`.
- **Xbox quirk (W3):** `BOTHSRCALPHA` and `BOTHINVSRCALPHA` are D3D7-
  era holdovers; PC D3D9 dropped `BOTHINVSRCALPHA`. Emulators
  targeting modern APIs must split into two passes or refactor.

### F.2 Alpha test

- Enable: `NV097_SET_ALPHA_TEST_ENABLE` (0x00000300).
- Func: `NV097_SET_ALPHA_FUNC` (0x0000033C). Enum `PshAlphaFunc`:
  `NEVER=0, LESS=1, EQUAL=2, LEQUAL=3, GREATER=4, NOTEQUAL=5,
  GEQUAL=6, ALWAYS=7`. (W1.)
- Reference: `NV097_SET_ALPHA_REF` (0x00000340). 8-bit ref.

D3D8 cross-reference (W3): `D3DCMPFUNC { NEVER=1, LESS=2, EQUAL=3,
LESSEQUAL=4, GREATER=5, NOTEQUAL=6, GREATEREQUAL=7, ALWAYS=8 }` —
1-indexed vs NV2A's 0-indexed; emulator must remap.

### F.3 Color masks (per-channel)

`NV097_SET_COLOR_MASK` (0x00000358) — per-channel bits:

- `BLUE_WRITE_ENABLE` bit 0
- `GREEN_WRITE_ENABLE` bit 8
- `RED_WRITE_ENABLE` bit 16
- `ALPHA_WRITE_ENABLE` bit 24

(W1, W2.)

D3D8 cross-reference (W3): `D3DRS_COLORWRITEENABLE=168`. Common
emulator bug: ignoring the alpha bit when host RT has no alpha
channel.

### F.4 Logic ops

- Enable: `NV097_SET_LOGIC_OP_ENABLE` (0x000017BC).
- Op: `NV097_SET_LOGIC_OP` (0x000017C0). (W1, W2.)

**xemu note (W1):** GL renderer's logic-op map at `gl/constants.h:86-105`
is **commented out** — logic-op state is captured but not actually
programmed on GL. **Likely regression risk for titles that use logic
ops.** Diagnostic XBE for logic ops should be high priority — it'll
likely flag a real renderer gap on first run.

### F.5 Dithering

- Enable: `NV097_SET_DITHER_ENABLE` (0x00000310). (W1, W2.)
- GL applies via `glEnable(GL_DITHER)` (W1: `gl/draw.c:707-712`).

D3D8 cross-reference (W3): `D3DRS_DITHERENABLE=26`, default FALSE.
Xbox NV2A has hardware dither; on a 16bpp surface the dither pattern
is observable. Ignoring dither produces visible banding in low-bpp
gradients (e.g., Xbox dashboard).

### F.6 sRGB

- **No NV097_* method handler observed for sRGB color-space
  conversion.** RT formats are linear-only on the Xbox-side; xemu
  does not appear to track sRGB state. (W1: gap. W2: not exposed.
  W3: not enumerated.) Diagnostic XBE for sRGB is unnecessary unless
  evidence of usage emerges.

### F.7 Render-target color formats

`NV097_SET_SURFACE_FORMAT_COLOR` (mask 0x0000000F) — 10 codes:

| Code | Format |
|---|---|
| 0x01 | `LE_X1R5G5B5_Z1R5G5B5` |
| 0x02 | `LE_X1R5G5B5_O1R5G5B5` |
| 0x03 | `LE_R5G6B5` |
| 0x04 | `LE_X8R8G8B8_Z8R8G8B8` |
| 0x05 | `LE_X8R8G8B8_O8R8G8B8` |
| 0x06 | `LE_X1A7R8G8B8_Z1A7R8G8B8` |
| 0x07 | `LE_X1A7R8G8B8_O1A7R8G8B8` |
| 0x08 | `LE_A8R8G8B8` |
| 0x09 | `LE_B8` |
| 0x0A | `LE_G8B8` |

(W1: `nv2a_regs.h:870-880`. W2. W3: confirmed.)

**Z vs O suffixes (W3):** "Z" = zero-fill / undefined unused bits;
"O" = original (preserved) unused bits. Xbox-specific. Emulators
"frequently get this wrong on first pass."

**xemu GL note (W1):** GL map at `gl/constants.h:287-302` only
supports a subset (X1R5G5B5/R5G6B5/X8R8G8B8/A8R8G8B8/B8/G8B8); the
`LE_X1A7R8G8B8_*` variants and the `_Z*`/`_O*` distinctions are
**not separately implemented** in GL. Likely treated as A8R8G8B8 /
X8R8G8B8.

**Surfaces "*not suitable for displaying*" (W3):** the `B8` and
`G8B8` color formats — CRTC scanout cannot decode them; titles may
still RENDER into them as transient targets.

---

## G. Depth / Stencil

### G.1 Depth test

- Enable: `NV097_SET_DEPTH_TEST_ENABLE` (0x0000030C).
- Func: `NV097_SET_DEPTH_FUNC` (0x00000354). 8 values 0-7
  (NEVER..ALWAYS).
- Write enable: `NV097_SET_DEPTH_MASK` (0x0000035C).
- Helper: `pgraph_zeta_write_enabled` (W1: `pgraph.h:357-362`).

(W1, W2.)

### G.2 Z vs W buffer

- `NV097_SET_CONTROL0` bit 12 = `Z_FORMAT` (0=fixed, 1=float).
- `NV097_SET_CONTROL0` bit 16 = `Z_PERSPECTIVE_ENABLE` (W-buffer).
- Zeta surface format: `NV097_SET_SURFACE_FORMAT_ZETA` mask 0xF0:
  - `Z16 = 1`
  - `Z24S8 = 2`
- xemu maps both fixed and float zeta in
  `kelvin_surface_zeta_float_format_gl_map[]` and
  `_fixed_format_gl_map[]`. (W1: `gl/constants.h:304-320`.)
- **Xbox-specific (W3):** float-Z on Xbox; PC D3D8 has only fixed-Z.

**xemu FIXME:** `Z24S8 floating-point` mode is emulated via fixed-
point on GL (`gl/constants.h:308`).

### G.3 Depth bounds

- **No separate "depth bounds test" enumerated in NV2A.** NV2A uses
  `SET_CLIP_MIN`/`SET_CLIP_MAX` plus `SET_ZMIN_MAX_CONTROL` (clamp/cull
  selector). (W1, W2: gap. W3: D3D8 has no depth-bounds test either.)

### G.4 Stencil

- Enable: `NV097_SET_STENCIL_TEST_ENABLE` (0x0000032C).
- Func: `NV097_SET_STENCIL_FUNC` (0x00000364). NEVER..ALWAYS.
- Reference: `NV097_SET_STENCIL_FUNC_REF` (0x00000368).
- Read mask: `NV097_SET_STENCIL_FUNC_MASK` (0x0000036C).
- Write mask: `NV097_SET_STENCIL_MASK` (0x00000360).
- Stencil-write enable: `NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE`
  bit 0; also `NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE` bit 25.
- Ops: `NV097_SET_STENCIL_OP_FAIL` (0x370), `_ZFAIL` (0x374),
  `_ZPASS` (0x378). 8 values:
  - `KEEP=0x1E00`
  - `ZERO=0`
  - `REPLACE=0x1E01`
  - `INCRSAT=0x1E02`
  - `DECRSAT=0x1E03`
  - `INVERT=0x150A`
  - `INCR=0x8507`
  - `DECR=0x8508`
- GL map: `gl/constants.h:136-146`.

(W1, W2, W3 — full agreement.)

D3D8 cross-reference (W3): `D3DSTENCILOP { KEEP=1, ZERO=2, REPLACE=3,
INCRSAT=4, DECRSAT=5, INVERT=6, INCR=7, DECR=8 }`.

### G.5 Z compression

- Register `NV_PGRAPH_ZCOMPRESSOCCLUDE` (0x1A84). (W1.)
- "Z Buffer is compressed losslessly up to 4:1, 2:1 in practice"
  (W3: Beyond3D / xboxdevwiki).
- xemu does not appear to model the underlying compression scheme
  pixel-perfect. Diagnostic XBE for Z-compression boundary cases is
  low priority unless a specific bug surfaces.

### G.6 Depth-only / stencil-only RTs

- Surface zeta independent of color binding via `NV097_SET_SURFACE_FORMAT_ZETA`
  + `NV097_SET_SURFACE_ZETA_OFFSET` (0x00000214).
- Independent DMA selectors `pg->dma_color`, `pg->dma_zeta`. (W1.)

---

## H. Render targets / Surface / Framebuffer

### H.1 Surface state

- `Surface` struct (W1: `pgraph.h:63-70`): `draw_dirty`, `buffer_dirty`,
  `write_enabled_cache`, `pitch`, `offset`.
- `SurfaceShape` struct (W1: `surface.h:25-33`): `z_format`,
  `color_format`, `zeta_format`, `log_width`, `log_height`,
  `clip_x/y`, `clip_width/height`, `anti_aliasing`.
- Two simultaneous bindings: `pg->surface_color`, `pg->surface_zeta`.

### H.2 MRT

**Not supported on NV2A.** xemu GL hardcodes a single
`GL_COLOR_ATTACHMENT0` (W1: `gl/surface.c:200,381,415`); only
color/zeta dual binding exists. (W2: no MRT API in pbkit; W3:
Wikipedia / Beyond3D.)

### H.3 Surface VRAM addressing / pitch / tiling

- `NV097_SET_SURFACE_PITCH` (0x0000020C) packs `_COLOR` (mask 0xFFFF)
  and `_ZETA` (mask 0xFFFF0000).
- `NV097_SET_SURFACE_COLOR_OFFSET` (0x00000210),
  `_ZETA_OFFSET` (0x00000214).
- `NV097_SET_SURFACE_FORMAT_TYPE` mask 0xF00: `PITCH=0x1`, `SWIZZLE=0x2`.
- DMA selector: `NV097_SET_CONTEXT_DMA_COLOR` (0x00000194), `_ZETA`
  (0x00000198).
- `NV097_SET_SURFACE_FORMAT_WIDTH` / `_HEIGHT` (mask 0x00FF0000 /
  0xFF000000) — log2 of width/height (so up to 2^255, but practically
  bounded).
- Tile-format hardware tracker: `NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(i)`
  for 8 GPU tiles (`NV_NUM_GPU_TILES = 8`).

(W1: `nv2a_regs.h:60, 722-727, 884-897`. W2. W3.)

### H.4 Surface clip (sub-region of bound RT)

- `NV097_SET_SURFACE_CLIP_HORIZONTAL` (0x00000200): `X` (mask 0xFFFF),
  `WIDTH` (mask 0xFFFF0000).
- `NV097_SET_SURFACE_CLIP_VERTICAL` (0x00000204): `Y`, `HEIGHT`.

(W1, W2.)

### H.5 Surface-to-texture (RT-as-texture)

The render-target surface and the texture-stage texture share DMA /
VRAM addressing. The renderer ops include `surface_update`
(`pgraph.h:130`) which downloads the GPU-side RT contents back to
VRAM so a subsequent texture-bind for the same address sees current
pixels. xemu GL: `pgraph_gl_surface_update`. xemu Metal: dirty-
tracking per `MtlSurfaceBinding` cache.

**Pitfall (W3):** xemu PR #2168 — "Upscale rendertargets and depth
surfaces consistently"; emulators scaling internal-resolution must
scale BOTH color and depth at the same factor or readbacks corrupt.

### H.6 Surface-to-VRAM blits

`NV_IMAGE_BLIT` class 0x9F:
- `SET_OBJECT` 0x0
- `SET_CONTEXT_SURFACES` 0x019C
- `SET_OPERATION` 0x02FC — values `BLEND_AND=2`, `SRCCOPY=3`
- `CONTROL_POINT_IN/OUT` 0x300/0x304
- `SIZE` 0x308

`NV_CONTEXT_SURFACES_2D` class 0x62:
- `SET_COLOR_FORMAT` 0x300 — values `LE_Y8=0x01`, `LE_R5G6B5=0x04`,
  `LE_X8R8G8B8_Z8R8G8B8=0x06`, `LE_X8R8G8B8=0x07`, `LE_A8R8G8B8=0x0A`,
  `LE_Y32=0x0B`
- Plus pitch + offsets

(W1: `nv2a_regs.h:818-841`. W2: `nv_objects.h:24-47, 689-696, 698-717,
734-749, 833-840` — confirms additional 2D classes:
`NV04_SWIZZLED_SURFACE`, `NV20_SWIZZLED_SURFACE`,
`NV05_SCALED_IMAGE_FROM_MEMORY`, `NV_MEMORY_TO_MEMORY_FORMAT`.)

### H.7 CRTC publish / page flip / vsync

- `NV097_FLIP_STALL` (0x00000130). Triggers `flip_stall` op +
  `surface_update(false, true, true)` to publish back-buffer.
- `NV097_FLIP_INCREMENT_WRITE` (0x0000012C) — bumps WRITE_3D modulo
  counter (tracked by the `NV2A_PRESENT_HEARTBEAT` counter in this
  fork).
- `NV097_SET_FLIP_READ` (0x00000120), `_WRITE` (0x00000124),
  `_MODULO` (0x00000128).
- VBlank IRQ source: `NV_PCRTC_INTR_0_VBLANK`,
  `NV_PCRTC_INTR_EN_0_VBLANK`.
- Front-buffer base: `NV_PCRTC_START` 0x0800, `NV_PCRTC_CONFIG` 0x0804,
  `NV_PCRTC_RASTER` 0x0808.

(W1: `nv2a_regs.h:653-658, 848-852`. W2.)

**Pitfall (W3):** "FLIP_STALL ... updates the surface and then enters
a loop that waits for the read and write pointers in the surface
registers to match, blocking until the flip operation completes." The
stall is on the **GPU FIFO side** (it stalls PFIFO command processing),
not on the CPU side. CPU continues — but the next NV097 method the
CPU pushes won't fire until the PFIFO catches up. Misimplementing as
a CPU-side spin is a classic refactoring trap.

**The PGR2/Crimson "front-fb fallback" class of bugs** (in this fork's
existing docs) maps onto this: titles abandon the CRTC-pointed surface
mid-run and draw to a different VRAM address; faithful CRTC publish
requires either VRAM read-back or back-buffer propagation. Diagnostic
XBE that explicitly tests "draw to surface A, switch to B, never go
back to A" exercises this exact path.

---

## I. Display / Output

### I.1 PCRTC scanout

- `NV_PCRTC_START` (front-buffer base address), `_CONFIG`, `_RASTER`.
- VBlank IRQ: `NV_PCRTC_INTR_0_VBLANK` / `_INTR_EN_0_VBLANK`.

(W1: `nv2a_regs.h:653-658`. W3: envytools — page is "Todo: write me",
W1 is authoritative.)

### I.2 Gamma / video DAC

- `NV_PRMDIO` block — alias of VGA palette. `NV_USER_DAC_WRITE_MODE_ADDRESS`
  0x3C8, `NV_USER_DAC_PALETTE_DATA` 0x3C9.
- `NV_PRAMDAC` block (RAMDAC + cursor + PLL): `NV_PRAMDAC_NVPLL_COEFF`,
  `_MPLL_COEFF`, `_VPLL_COEFF`.
- `pb_set_gamma_ramp(const PB_GAMMA_RAMP*)` (W2: `pbkit_gamma.h:24`)
  writes `NV_USER_DAC_PALETTE_DATA` directly.

(W1, W2.)

### I.3 Display modes / refresh rates

Flat-panel registers: `NV_PRAMDAC_FP_VDISPLAY_END` (0x800),
`_VCRTC` (0x808), `_VSYNC_END` (0x810), `_VVALID_END` (0x818),
`_HDISPLAY_END` (0x820), `_HCRTC` (0x828), `_HVALID_END` (0x838).

Interlace mode: `NV_PRMCIO_INTERLACE_MODE` 0x39, value `_DISABLED=0xFF`.

Mode-set API (Xbox-side, W2): `XVideoSetMode(width, height, bpp, refresh)`
in `<hal/video.h>`. Refresh constants: `REFRESH_DEFAULT`.

(W1: `nv2a_regs.h:661-662, 757-763`. W2.)

### I.4 Video overlay (`NV_PVIDEO`) — out of pure 3D scope

Mentioned for completeness: `NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8`,
point-in/out, color key. Not part of the diagnostic XBE library scope.

(W1: `nv2a_regs.h:683-704`.)

---

## J. State / Control

### J.1 Viewport

- `NV097_SET_VIEWPORT_OFFSET` (0x00000A20, 4 dwords).
- `NV097_SET_VIEWPORT_SCALE` (0x00000AF0, 4 dwords).
- Form: `pos = pos·scale + offset` (no separate min/max registers).

xemu GL applies via `glViewport(0, 0, vp_width, vp_height)` after
`pgraph_apply_scaling_factor` (W1: `gl/draw.c:734-737`).

(W1, W2.)

### J.2 Scissor / clip rect

- Surface clip (H.4) is the primary scissor-equivalent.
- Window clip (C.8) is the 8-rect Xbox-specific variant.

### J.3 Clip planes (user-defined)

- `NV097_SET_SHADER_CLIP_PLANE_MODE` (0x000017F8). Register
  `NV_PGRAPH_SHADERCLIPMODE`.
- Implementation: encoded as a texture-stage `CLIPPLANE` mode (D.8
  mode 0x05).
- PshUniform `clipRange`, `clipRegion[8]` bridge into fragment shader.

(W1.)

D3D8 cross-reference (W3): `D3DRS_CLIPPING=136` (default TRUE),
`D3DRS_CLIPPLANEENABLE=152` (bitmask of 6 planes — Xbox supports more
via texture-stage encoding).

### J.4 State block / state push

- xemu's NV2A "state" is `PGRAPHState::regs_[0x2000]` flat register
  file (W1: `pgraph.h:242`) with per-dword dirty bitmap (`regs_dirty`).
- Accessed via `pgraph_reg_r`/`pgraph_reg_w`.
- Method dispatch goes through `pgraph_method` with the table from
  `methods.h.inc` (~200 entries).
- The Xbox D3D8 state-block concept is **not** modeled at the hardware
  level; D3D8 state pushes translate into individual NV097 method
  dispatches at the guest driver level.

(W1.)

### J.5 Clear surface

- `NV097_CLEAR_SURFACE` (0x00001D94): `Z` bit 0, `STENCIL` bit 1,
  `COLOR` mask 0xF0 (`R/G/B/A` bits 4-7).
- Clear color: `NV097_SET_COLOR_CLEAR_VALUE` (0x1D90).
- Clear zstencil: `NV097_SET_ZSTENCIL_CLEAR_VALUE` (0x1D8C).
- Clear rect: `NV097_SET_CLEAR_RECT_HORIZONTAL` (0x1D98), `_VERTICAL`
  (0x1D9C).
- Color decode helper: `pgraph_get_clear_color` (W1:
  `pgraph.c:3187+`).

pbkit helpers (W2):
- `pb_erase_depth_stencil_buffer(x, y, w, h)` — must be called once
  per frame or 1/3 perf is lost (depth-tile compression invariant).
- `pb_fill(x, y, w, h, color)` — color rect clear.
- `pb_set_depth_stencil_buffer_region(format, depth, stencil, x, y,
  w, h)` — parameterized.

(W1, W2.)

---

## K. NV2A-/Xbox-specific oddities

These do not fit cleanly into A-J because they are either Xbox-specific
HW behavior or xemu-specific renderer infrastructure. The diagnostic
XBE library should plan tests for these.

### K.1 Quad / quad-strip / polygon expansion via geometry shader

OpenGL/Vulkan/Metal have no native QUAD primitive. xemu's GL backend
uses geometry shaders to expand:

- `QUADS → triangles` via `GL_LINES_ADJACENCY` input (4 verts) + GS emit (2 tris).
- `QUAD_STRIP → GL_LINE_STRIP_ADJACENCY` + GS emit.
- `POLYGON` in fill mode → fans (`GL_TRIANGLE_FAN`) or `GL_LINE_LOOP`
  (line mode).

GS state: `GeomState` (`glsl/geom.h:28-41`); generator
`pgraph_glsl_gen_geom`. GL-specific dispatch `gl/shaders.c:34-70`.
Diagonal selection / triangle rotation tracked in
`GPUProperties::geom_shader_winding { tri, tri_strip0, tri_strip1,
tri_fan }` — GL queries the GPU to pick the correct expansion winding
(`gl/gpuprops.c`).

(W1.)

### K.2 Native triangle-depth / native quad bypasses (Apple Silicon fork)

To avoid the GS pass on Apple's GL-on-Metal (and to enable the Metal
renderer):

- `XEMU_NATIVE_TRI_DEPTH=1` (PshState `native_tri_depth`,
  `glsl/psh.h:72`) — triangle depth + polygon-slope derived in the
  fragment shader from `gl_FragCoord` (no GS).
- `XEMU_NATIVE_QUAD=1` (PshState `native_quad`, `glsl/psh.h:73`) —
  CPU expands quads to triangles via `gl_native_quad_index_buffer`
  (`gl/vertex.c:275`).

Eligibility: `pgraph_glsl_native_tri_depth_supported` /
`pgraph_glsl_native_quad_supported` (`glsl/geom.h:52-60`).

These are default-on Apple Silicon flags (per project rule #11) and
must be regression-gated by a diagnostic XBE that exercises the
specific path each replaces.

### K.3 Surface scaling / supersampling

- `pg->surface_scale_factor` (W1: `pgraph.h:266`); applied via
  `pgraph_apply_scaling_factor` (`pgraph.h:384-390`).
- User-facing knobs: `XEMU_DISPLAY_SCALE` and the saved
  `display.quality.surface_scale` (default 2 on Apple Silicon system
  builds — 1080p-class internal resolution).

### K.4 Anti-aliasing factor → host width/height multiplier

`pgraph_apply_anti_aliasing_factor` (W1: `pgraph.h:364-382`)
translates the surface AA mode (none / 2× / 4× SQUARE) to width/height
multipliers — Xbox MSAA is encoded by allocating a larger backing
surface and then resolving down at present.

### K.5 Z-pass pixel-count occlusion query

- `NV097_CLEAR_REPORT_VALUE` (0x000017C8) with `TYPE_ZPASS_PIXEL_CNT=1`.
- `NV097_SET_ZPASS_PIXEL_COUNT_ENABLE` (0x000017CC).
- `NV097_GET_REPORT` (0x000017D0): `OFFSET` (mask 0xFFFFFF) and
  `TYPE_ZPASS_PIXEL_CNT=1`.
- Writeback via `pgraph_write_zpass_pixel_cnt_report`.
- Renderer ops: `clear_report_value`, `get_report`,
  `process_pending_reports`.

(W1: `nv2a_regs.h:1149-1156`. W2: `nv_regs.h:495-502`. W3 confirms NV2A
has occlusion query but bit-format not externally documented.)

**Limitations affecting diagnostic-XBE use** (Codex review):

- The method handler asserts `type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT`
  (`pgraph.c:2629-2635`) — **no other report types are supported**.
  Z-pass is the *only* on-GPU value the report mechanism returns.
- The Metal renderer's `pgraph_mtl_get_report` writes a literal
  zero unconditionally (`mtl/renderer.c:1917-1920`). Any Z-pass
  XBE will FAIL on Metal until that's implemented.
- The GL renderer has explicit FIXME limitations for MSAA, clear,
  and clipping interactions (`gl/reports.c:38`).

**Therefore**: `NV097_GET_REPORT` is **not** suitable as the
*primary* self-validation mechanism for any XBE that's expected to
validate Metal output. See §6.1 for the revised contract; the
primary mechanism is **CPU-side VRAM readback**.

The original framing here (Z-pass count as "the on-GPU readback
path that lets an XBE check its own results") was wrong — corrected
2026-05-05 post-Codex review.

### K.6 Semaphore / synchronization

- `NV097_SET_CONTEXT_DMA_SEMAPHORE` 0x1A4.
- `NV097_SET_SEMAPHORE_OFFSET` 0x1D6C.
- `NV097_BACK_END_WRITE_SEMAPHORE_RELEASE` 0x1D70.
- `NV097_NO_OPERATION` 0x100.
- `NV097_WAIT_FOR_IDLE` 0x110.

(W1, W2.)

### K.7 DMA object types

- `NV_DMA_FROM_MEMORY_CLASS` 0x02
- `NV_DMA_TO_MEMORY_CLASS` 0x03
- `NV_DMA_IN_MEMORY_CLASS` 0x3D

Selectors: `dma_*` for vertex A/B, color, zeta, state, notifies,
semaphore, report.

(W1.)

### K.8 Renderer dispatch table (xemu-specific)

`PGRAPHRenderer` ops (W1: `pgraph.h:108-135`) — single set of function
pointers bridging NV2A method dispatch to GL/Metal/Vulkan/Null
backends:

`early_context_init`, `init`, `finalize`, `clear_report_value`,
`clear_surface`, `draw_begin`, `draw_end`, `flip_stall`, `flush_draw`,
`get_report`, `image_blit`, `pre_savevm_trigger`, `pre_savevm_wait`,
`pre_shutdown_trigger`, `pre_shutdown_wait`, `process_pending`,
`process_pending_reports`, `surface_flush`, `surface_update`,
`set_surface_scale_factor`, `get_surface_scale_factor`,
`get_framebuffer_surface`, `get_gpu_properties`.

Backend selected via `XEMU_RENDERER` env-var bridge (Apple Silicon
fork). Renderer state union in `pgraph.h:270-274`.

### K.9 Dirty-VRAM tracking

- `pg->regs_dirty` bitmap — per-dword PGRAPH register-modification.
- Per-attribute `pg->compressed_attrs`, `uniform_attrs`, `swizzle_attrs`
  masks.
- Per-texture `pg->texture_dirty[NV2A_MAX_TEXTURES]`.
- Per-`vsh_constants[]` `vsh_constants_dirty[]`.
- Per-`ltctxa/b/c1` dirty arrays.
- `pg->program_data_dirty`.
- Metal renderer extends with VRAM-side surface-dirty tracking via
  `MtlSurfaceBinding::dirty_vram` atomic + access callbacks.

(W1: `pgraph.h:170-243`.)

### K.10 RDI access (Cheops internal RAM)

Side-channel access to vertex-context RAM:

- `NV_PGRAPH_RDI_INDEX` (with `_ADDRESS` and `_SELECT` sub-fields).
- `NV_PGRAPH_RDI_DATA`.
- `RDI_INDEX_VTX_CONSTANTS0` 0x17, `RDI_INDEX_VTX_CONSTANTS1` 0xCC.

`NV_PGRAPH_RDI_DATA` reads auto-increment `RDI_INDEX_ADDRESS` — so
this register's lock-free fast-read path is unsafe.

(W1.)

### K.11 Renderer switch / runtime renderer change

State machine `renderer_switch_phase` in `PGRAPHState`
(`pgraph.h:259-264`) with phases `IDLE`/`STARTED`/`CPU_WAITING` and
`renderer_switch_complete` event — supports runtime switching between
GL/Vulkan/Metal/Null without restarting the VM.

(W1.)

### K.12 Async shader compile worker

Apple Silicon fork: third GL context `g_nv2a_context_shader_compile`
runs `glLinkProgram` off the renderer critical path; renderer marks
bindings PENDING and skips the draw if the binding is still
compiling.

(W1: `gl/shaders.c`.)

### K.13 Inline buffer attribute backing

Each `VertexAttribute` allocates `inline_buffer = g_malloc(NV2A_MAX_BATCH_LENGTH
* sizeof(float[4]))` lazily; per-attribute float buffers populated only
when an inline-data method writes.

(W1: `pgraph.c:362-365`, `vertex.c:105-119`.)

### K.14 Pattern / chroma-key context

`NV_CONTEXT_PATTERN` class 0x44 with `SET_MONOCHROME_COLOR0` 0x310;
`NV_PGRAPH_PATT_COLOR0`.

(W1.)

### K.15 Beta blend constant context

`NV_BETA` class 0x12 with `SET_BETA` 0x300; `BetaState` struct in
pgraph.

(W1.)

### K.16 Notifier IRQ surface

`NV_PGRAPH_INTR_*` interrupt sources: `NOTIFY`, `MISSING_HW`,
`TLB_PRESENT_DMA_R/_W/_TEX_A/_B/_VTX`, `CONTEXT_SWITCH`, `STATE3D`,
`BUFFER_NOTIFY`, `ERROR`, `SINGLE_STEP`. Used by guest driver for
async-completion / page-fault recovery.

(W1.)

### K.17 Channel / subchannel structure

- `NV2A_NUM_CHANNELS = 32`.
- `NV2A_NUM_SUBCHANNELS = 8`.
- `NV2A_CACHE1_SIZE = 128`.
- 3D path uses `NV_KELVIN_PRIMITIVE = 0x97`.

(W1, W2.)

---

## 3. Cross-witness disagreements, gaps, and known limitations

The original §3 conflated three categories. Codex review surfaced
that #8, #10 are full agreement (not disagreements), #4 mistakenly
blanked W2, and #12 is a single-witness observation. Reorganized
into 3a/3b/3c below.

### 3a. True cross-witness disagreements

These are explicit conflicts where two witnesses provide
contradictory information. Each is a diagnostic-XBE target — the
XBE itself is the experimental tiebreaker.

| # | Topic | W1 says | W2 says | W3 says | Diagnostic action |
|---|---|---|---|---|---|
| 3a.1 | Vertex attribute slot 6-15 mapping | POINT_SIZE/BACK_DIFF/BACK_SPEC/TEX0-3/RES1-3 (`nv2a_regs.h:1469-1484`) | Generic ATTR6/ATTR7 + TEX0-7 at 8-15 (`nv_objects.h:1149-1164`) | — | XBE writes a unique known constant to each slot 6-15; vertex shader emits each slot in turn; fragment shader colorizes by slot index; expected output is the W1 mapping per `pgraph.c:2444`. (Codex confirmed W1 maps TEX0 to attr 9.) |
| 3a.2 | Edge flags | No method handler in `methods.h.inc` (likely no-op) | `NV097_SET_EDGE_FLAG` 0x16BC enumerated | D3D8 has no `D3DRS_EDGEFLAG` (though it's an FFP feature) | XBE writes edge flags; expected output (per real Xbox semantics) shows specific edges suppressed in line/wireframe mode. Confirms whether xemu silently no-ops. |
| 3a.3 | Line stipple | No method handler | `NV097_SET_STIPPLE_ENABLE` 0x147C, pattern 0x1480 enumerated | `D3DRS_LINEPATTERN=10` enumerated | Same as 3a.2 — XBE renders stippled line; visible pattern indicates support, solid line indicates no-op. |
| 3a.4 | Texture-shader stage mode count | 19 modes (`psh_regs.h:31-53`) | 19 modes attested via per-stage enum (`nv_regs.h:694-747`); Codex correction — W2 does enumerate, original §3 incorrectly blanked W2 | "16 from NONE to DOTPRODUCT" (xboxdevwiki) | W3 undercounts by 3 (BUMPENVMAP_LUM, DPNDNT_AR/GB, DOT_RFLCT_*); trust W1+W2 (full agreement on 19). XBE iterates all 19 valid (stage, mode) pairs. |
| 3a.5 | Vertex shader ARL bias | `floor(src + 0.001)` (xemu impl) | nxdk supplies VS instruction encoder, no opinion on ARL semantics | xemu issue #2362 demonstrates xemu's bias over-corrects in Midtown Madness 3 | XBE constructs ARL-input vector with values near integer boundaries; vertex shader uses `c[a0+N]` indexed read; fragment shader displays decoded constant index. CPU readback compares against expected per real Xbox semantics (which xemu issue #2362 says is "specify rounding" — exact mode unresolved externally). |
| 3a.6 | NV2A_SET_ANTI_ALIASING_CONTROL bit-encoding | Implemented per `surface_shape.anti_aliasing`, AA modes 0/1/2 (`surface.h:32`) | Bit 0 = MSAA enable + bits 31:16 sample-mask via `NV20_TCL_PRIMITIVE_3D_MULTISAMPLE` (`nv_objects.h:1213`) | "no external page enumerates this" | XBE configures each AA mode; renders edge-on quad; CPU samples adjacent pixels along diagonal; expected output shows AA edge gradients differ per mode. |
| 3a.7 | Z24S8 floating-point depth | xemu emulates via fixed-point (`gl/constants.h:308` FIXME) | nxdk pbkit doesn't expose float Z toggle | xboxdevwiki: float depth is supported on Xbox | XBE renders coplanar geometry near far-plane and near-plane in float-Z mode; samples depth-as-texture; CPU readback verifies precision distribution differs from fixed-point case. |

### 3b. Single-witness observations (not disagreements)

Items where only one witness has a position; the others are silent
rather than contradicting. Treat as "load-bearing on that one
witness" — verify with a diagnostic XBE if relevant.

| # | Topic | Witness | Claim |
|---|---|---|---|
| 3b.1 | EDGEANTIALIAS supported on Xbox | W1, fork commit `mborgerson/xemu@a34cab6` | Xbox supports EDGEANTIALIAS despite PC archived MS doc saying "Not supported in Windows CE." W1 trumps W3 here because the W3 source is from PC/CE D3D8, not Xbox D3D8. |
| 3b.2 | NV2A signed texture format sign-extension | W1 (code path exists), W3 (xqemu PR #36 fix) | "default-zero unsigned interpretation produces wrong dot3 bumpmaps." Diagnostic XBE writes signed values, samples, verifies sign-extension. |
| 3b.3 | LOGIC_OP captured but not programmed on xemu GL | W1 only | `gl/constants.h:86` map is commented out. **xemu impl limitation, not a disagreement.** XBE for logic ops is high-priority because it'll likely show as broken on GL renderer (and possibly on Metal). |
| 3b.4 | xemu GS expansion of QUADS / QUAD_STRIP / POLYGON | W1 only | xemu's renderer-side strategy; not part of NV2A semantics. Diagnostic XBE that exercises native `_OP_QUADS` regression-gates xemu's expansion path against the closed-default-on `XEMU_NATIVE_QUAD` flag. |
| 3b.5 | Apple Silicon fork-specific flags | W1 only | `XEMU_NATIVE_TRI_DEPTH`, `XEMU_NATIVE_QUAD`, `MtlSurfaceBinding` cache, etc. Fork-specific impl details, regression-gated by their own diagnostic XBEs (§6). |

### 3c. Cross-witness agreement (high confidence)

These are *not* disagreements — listed for completeness because the
original §3 included them as "disagreements" by mistake.

| # | Topic | Agreement |
|---|---|---|
| 3c.1 | NV2A primitive enum (POINTS=1..POLYGON=10) | W1 (`nv2a_regs.h:1160`) and W2 (`nv_regs.h:506`) match exactly. W3 is community-derived but consistent. **High confidence.** |
| 3c.2 | sRGB support absent | All three witnesses: no NV097 method, not exposed in pbkit, not enumerated externally. **No diagnostic XBE needed.** |
| 3c.3 | EDGEANTIALIAS / POINTSPRITEENABLE / PATCHEDGESTYLE PC-doc-disagree-with-Xbox | W3 archived MS doc says "Not supported in Windows CE"; the same W3 secondary sources confirm Xbox support. W1 confirms via implementation. **W3 doc disagreement is internal to W3** — Xbox-specific behavior is well-attested. |

## 4. Documentation gaps where only one witness attests

Items below are attested by one stream only. The diagnostic XBE
library should help confirm or refute them when relevant.

- W1-only: `MtlSurfaceBinding` cache structure, `XEMU_NATIVE_*` flag
  semantics, `pg->surface_scale_factor` (Apple Silicon fork
  particulars).
- W1-only: Compete texture-shader mode enum (19 modes); W3 undercounts.
- W2-only: `NV097_INLINE_ARRAY` 0x1818 bit-30 idiom; W1 implements
  but W2 has the canonical comment.
- W3-only: NV2A "Faster GeForce3 with one major addition: a second
  vertex shader pipeline" — architecture descriptor.
- W3-only: Beyond3D / Wikipedia fillrate numbers (932 Mpix/s,
  1864 Mtex/s).
- W3-only: Texture-format codes that historically broke specific
  retail titles (xemu issue #320).
- W3-only: ARL emulation bug in xemu (issue #2362) details.
- W3-only: PCRTC envytools page is stub.
- W3-only: NV097_SET_ANTI_ALIASING_CONTROL bit encoding gap.
- W3-only: Hi-Z / Z-compression invalidation rules.
- W3-only: Xbox-specific X_D3DTSS / X_D3DRS extension numeric values
  shift across XDK revisions.
- W3-only: NV097_GET_REPORT bit-format and which counters are
  addressable (Z-pass, Z-cull, primitive-count).

## 5. Known emulation pitfalls (catalogued)

Cross-referenced from W3 + W1 known-issues. Each becomes a
diagnostic-XBE candidate.

| Pitfall | Class | Source |
|---|---|---|
| ARL bias over-correction in MM3 | Vertex shader | W3: xemu issue #2362 |
| R12 = oPos mirror not modeled | Vertex shader | W3: xboxdevwiki |
| Two distinct UB encodings (UB_D3D vs UB_OGL) | Vertex attrib | W1, W3 |
| CMP packed (11,11,10) format decode | Vertex attrib | W1 (M5.8 slice) |
| Mipmap chain in swizzle order, not linear-derived | Texture | W3 |
| P8/R8B8/luminance format conversion to ARGB | Texture | W3: Cxbx |
| Signed texture format sign-extension (Q8W8V8U8/V8U8/V16U16/W11V11U10) | Texture | W3: xqemu PR #36 |
| BUMPENVMAP / BUMPENVMAPLUMINANCE require texture-shader programs | Pixel | W3: Cxbx |
| BOTHSRCALPHA / BOTHINVSRCALPHA D3D7-era holdovers | Blend | W3 |
| Logic-op map commented out on GL | ROP | W1 |
| Z24S8 floating-point emulated via fixed-point | Depth | W1 FIXME |
| Window-clip 8-rect Xbox-specific scissor | Rasterization | W1, W2 |
| Native QUAD / QUAD_STRIP / POLYGON have no host equivalent | Primitive | W1 |
| Front-fb fallback class (PGR2/Crimson) — surface abandoned mid-run | Render target | This fork's docs |
| FLIP_STALL on FIFO side, not CPU side | Sync | W3 |
| EDGEANTIALIAS / POINTSPRITEENABLE / PATCHEDGESTYLE PC-doc disagree with Xbox | API | W3 |
| ZBIAS integer-vs-slope-scale interpretation | Depth | W3 |
| MSAA resolve semantics not externally documented | RT | W3 gap |
| Hi-Z compression invalidation rules | Depth | W3 gap |
| sRGB support absent | Color | All three |
| ColorWriteEnable per-channel including alpha | ROP | W3 |
| Dither on 16bpp visible | ROP | W3 |
| LASTPIXEL semantics (line endpoint inclusion) | Rasterization | W3 |
| POINTSCALEENABLE world-vs-screen units quirk | Rasterization | W3 |
| Vertex skinning enum values shift PC vs Xbox | Vertex | W3: Cxbx issue #2091 |

## 6. Implications for the diagnostic-XBE library

A complete diagnostic library will produce roughly 60-80 XBEs
covering the catalog above. This section was substantively revised
post-Codex review — the original self-validation contract overstated
`NV097_GET_REPORT` and several proposed XBEs depended on it.

### 6.1 Self-validation contract (revised)

Each XBE picks one of these self-validation mechanisms in priority
order. The XBE source-file header MUST declare which it uses and
why others were not chosen.

**Primary — CPU-side VRAM readback.** The XBE runs in the Xbox
guest. It has direct memory access to its own VRAM via
`MmGetPhysicalAddress` and pbkit framebuffer pointers. After
issuing `pb_finished()` and waiting for GPU completion via
`pb_wait_until_gr_not_busy()`, the XBE reads the rendered
pixels straight out of the back-buffer (or a render-target VRAM
offset) with a plain memcpy / pointer dereference, decodes the
expected color/depth/stencil per the test's math, and renders
`PASS` or `FAIL [diagnostic info]` via `pb_print` text overlay.

This works on any renderer (GL, Metal, future Vulkan), is
mathematically deterministic, and is independent of any external
oracle. It is the **default** mechanism.

**Secondary — RT-as-texture sampling.** When the XBE needs to test
something that's awkward to read CPU-side (e.g., MSAA-resolved
output, depth-as-texture, palette-decoded texel), the XBE binds the
just-rendered render target as a texture in a follow-up draw, samples
the relevant texel, and outputs it as a solid-color block in a
known screen position. CPU-side readback (mechanism #1) then
verifies that block.

**Tertiary — `NV097_GET_REPORT` Z-pass count.** Useful only for
**Z-pass-only** liveness (e.g., "did exactly N pixels pass depth
test?"). Limitations:

- The method asserts `type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT`
  (`hw/xbox/nv2a/pgraph/pgraph.c:2629-2635`); no other report types
  are dispatched.
- xemu **Metal renderer's `pgraph_mtl_get_report` writes a literal
  zero unconditionally** (`hw/xbox/nv2a/pgraph/mtl/renderer.c:1917-1920`).
  Z-pass count XBEs will FAIL on Metal until that's implemented.
- xemu GL has FIXME limitations for MSAA, clear, and clipping
  interactions (`hw/xbox/nv2a/pgraph/gl/reports.c:38`).

Conclusion: do NOT use `NV097_GET_REPORT` as the *primary*
mechanism for any XBE that's expected to validate Metal output.
Use it only as a *secondary* liveness counter alongside CPU-side
VRAM readback.

**Skipped — visual inspection only.** Where neither CPU readback
nor RT sampling is feasible (e.g., display-side gamma test where
the only meaningful comparison is "does this pixel look right on
the user's monitor"), the XBE renders side-by-side expected vs
actual reference patterns and asks the operator to visually
confirm. The XBE's source-file header must explicitly note that
no on-GPU self-check is possible and explain why.

### 6.2 Build priority

Priority ordering for build (tied to bugs already observed and to
the closed default-on flags that must be regression-gated):

1. **Mirror / viewport / scissor** — XBE renders single white
   pixel at known position (e.g., `(640, 100)` on a black
   `1280×960` back-buffer). CPU reads back the entire
   back-buffer; expected: exactly one white pixel at that
   coordinate, every other pixel exactly the clear color. Tests:
   vertex transform correctness, viewport offset/scale, surface
   clip, window clip rect 0. **Catches SC2's top-mirrored-to-
   bottom symptom directly** — if the renderer mirrors top to
   bottom, the readback finds a second white pixel at `(640, 860)`
   and the XBE renders `FAIL: mirror at (640, 860)`.
2. **Color channel** — XBE renders R/G/B/W in fixed-position
   quadrants. CPU reads back; expected: each quadrant is exactly
   that solid color (e.g., top-left `0xFF0000FF` ARGB). Tests: RT
   format A8R8G8B8, channel swizzle, color write enable. **Catches
   SC2 "wrong colors" symptom.**
3. **Depth / floor coverage** — XBE renders a labeled checkered
   ground plane viewed at fixed camera with z-test. CPU reads back
   each grid cell's center pixel; expected: predefined per-cell
   color. Tests: depth function, depth write enable, surface cache
   for zeta, `native_tri_depth` path. **Catches SC2 "floor
   disappearing" symptom.**
4. **CRTC publish race / front-fb fallback** — XBE allocates
   surface A at known VRAM addr, draws known content, switches the
   CRTC publish to addr A; then allocates surface B at a different
   VRAM addr, switches to drawing exclusively to B, calls
   `NV097_FLIP_STALL` to publish. The XBE itself then reads VRAM
   at the published addr (via the CRTC `NV_PCRTC_START` register
   value) and verifies it points at A's known content (or B's, if
   the CRTC publish is updated; both outcomes are valid behaviors
   to surface, so the XBE reports which one occurred rather than
   PASS/FAIL). Tests: front-fb fallback policy, CRTC publish
   path, surface-to-VRAM coherency. **Catches PGR2/Crimson class
   directly.** **Self-validation: CPU-side VRAM readback** (NOT
   `NV097_GET_REPORT` — that path returns 0 on Metal).
5. **Native quad / native tri-depth** — XBE renders `OP_QUADS` and
   `OP_TRIANGLES` with provoking-vertex first vs last. CPU readback
   verifies expected pixel coverage and per-pixel color. Tests: GS
   expansion path on plain GL, native-quad CPU expansion path,
   `native_tri_depth` fragment-shader depth derivation, flat-shading
   with provoking vertex. Regression-gates the closed default-on
   flags `XEMU_NATIVE_QUAD` and `XEMU_NATIVE_TRI_DEPTH`.
6. **Vertex format — CMP packed (11,11,10)** — XBE writes a known
   vector in CMP format, reads via vertex shader, outputs as solid
   color. CPU readback verifies the decoded color matches the math.
   Tests: M5.8 Metal CPU decoder, GL `needs_conversion` integer-
   attrib path.
7. **Texture format sweep** — XBE renders one texel-per-format from
   the 42-code union (E.1). Each format gets a fixed input VRAM
   pattern; output region is the decoded color or pattern. CPU
   readback verifies each format's region against expected. Tests
   the W1-only `0x2F LU_IMAGE_DEPTH_X8_Y24_FLOAT`, the W2-only
   `0x16 LU_IMAGE_R8B8`, and every format xemu issue #320
   historically broke.
8. **Swizzled vs linear texture layouts** — XBE writes a known mip
   chain in swizzled vs linear layout, samples each level, CPU
   readback verifies mip-level offset math.
9. **Blend / alpha test / color mask** — XBE renders overlapping
   quads with each blend-factor combination; CPU readback verifies
   each region's color matches the blend math.
10. **Stencil ops** — XBE writes stencil values, performs each of
    the 8 ops, then renders a quad with the stencil read back into
    color via stencil-as-texture (or via stencil-test pass/fail
    coloring); CPU readback verifies. (Mechanism #2: RT-as-texture
    is the natural fit since stencil isn't directly memcpy-able.)
11. **Texture filter / wrap modes** — XBE samples a UV-overrun quad
    with each wrap mode; CPU readback per-mode pixel comparison.
12. **Register combiner basic ops** — XBE configures a single
    combiner stage with each input / mapping / output; CPU readback
    verifies the output color matches the math. (No
    `NV097_GET_REPORT` dependency.)
13. **Texture-shader stage 19 modes** (D.8) — XBE iterates each
    valid (stage, mode) pair; CPU readback verifies expected output
    per mode.
14. **Logic ops** — XBE renders with each logic op against known
    dest; CPU readback verifies the logic-op result. **Likely flags
    an immediate GL regression** (W1 finding: GL map commented
    out). Will also stress whether Metal implements logic ops.
15. **MSAA / AA factor** — XBE renders edge-on geometry at fixed
    angles, per AA mode (none/2×/4×); CPU readback samples adjacent
    pixels along the edge to detect AA gradient.
16. **Texture DMA selector A vs B** — XBE binds the same texture
    data through DMA channel A and DMA channel B in successive
    frames. CPU readback compares; expected: pixel-identical. Tests
    DMA-channel address-translation parity (see §E.14).

Subsequent priorities cover: CRTC publish via `NV097_FLIP_STALL`
sequencing, vertex shader instruction-set per-op (each MAC op + each
ILU op individually, especially ARL boundary cases per §3a.5),
fixed-function lighting modes (8-light combinations), fog modes
(linear/exp/exp2/exp_abs/exp2_abs/linear_abs), texgen modes,
skinning modes, shadow-map / depth-shadow comparison, color key /
alpha kill, palettized texture, cube map, 3D texture, compressed
(DXT1/3/5), bumpenvmap, anisotropic filtering, multi-texturing
stage combinations, point sprite (resolving §C.7.4 unresolved
question), edge flags / line stipple (resolving §3a.2 / §3a.3).

### 6.3 Per-XBE correctness contract

Each XBE follows the correctness contract described in the companion
plan doc (`diagnostic-xbe-plan.md`):

- **Math-derivable expected output** documented in source-file
  header. The header derives the correct output from first
  principles (projection math, NV2A method semantics, channel
  format encoding) so a reviewer can audit "does this XBE actually
  test what it claims" without running it.
- **Cited NV097 method(s)**, xemu source location, and W2/W3
  citation per claim. No undocumented behavior reliance unless
  the XBE itself is the experimental probe (§3a items).
- **Self-validation per §6.1**, in priority order: CPU-side VRAM
  readback first, RT-as-texture second, `NV097_GET_REPORT`
  Z-pass-only third, visual-only as last resort with explicit
  rationale.
- **On-screen PASS/FAIL banner** via `pb_print` with diagnostic
  info on FAIL (e.g., "FAIL: pixel at (640, 100) was 0xFF00FFFF
  expected 0xFFFFFFFF").
- **Cross-renderer behavior recorded, not gating.** Per Codex
  finding, GL/Cxbx-Reloaded comparison is **advisory** — known-
  renderer failures (e.g., GL logic-op map commented out) are
  recorded in the XBE manifest as expected-fail-on-renderer-X
  rather than blocking promotion. Promotion gates on the
  mathematical oracle plus self-validation passing on at least one
  renderer.
- **Self-review the design (rule #15)** before nxdk source is written.
  Claude owns checks-and-balances directly; there is no external
  validator.
- **Reproducibility check** — two cold runs must produce
  byte-identical output (no timing leaks, no uninitialized
  memory).

### 6.4 Catalog itself in the validation cadence

Per Codex review, this catalog is the source of truth for the XBE
plan; if the catalog has a witness-attribution error or a missed
feature, every XBE downstream inherits the bug. Therefore:

- **Catalog diffs get a deliberate self-review pass (rule #15)** before
  expansion of the XBE plan they motivate.
- The original creation of this catalog (2026-05-05) was
  Codex-validated in `plan` mode and returned MAJOR ISSUES; the
  fixes are this revision. Subsequent meaningful catalog updates
  (new feature classes discovered, new pitfalls surfaced) repeat
  the self-review step.

### 6.5 Note on `NV097_GET_REPORT` Metal stub

The Metal-side `pgraph_mtl_get_report` (`mtl/renderer.c:1917-1920`)
currently writes 0 unconditionally. **Implementing it correctly is
itself a worthwhile fork slice** — it would unblock any future
diagnostic XBE that wants to use Z-pass count as a secondary
liveness check, and it would make titles that use occlusion queries
(if any) render correctly on Metal. Filed as a candidate task; not
on the diagnostic-XBE-library critical path (CPU-side VRAM readback
covers all current correctness-validation needs).

### 6.6 Note on point-sprite Xbox-side enable source

§C.7.4 left unresolved which NV097 method or `NV_PGRAPH_*` register
flips Xbox texture-coordinate replacement on for point primitives.
The diagnostic XBE for point sprites should be designed as an
**experimental probe** — render points with each candidate flag
combination, inspect texture-coordinate output via fragment shader,
identify which combination triggers replacement. Treat the result
as the resolution of §C.7.4 and update both the catalog and the
XBE design once known.

### 6.7 Codex-review open questions and project answers

Codex review surfaced three open questions. Project answers:

1. **Should Metal `NV097_GET_REPORT` be implemented before any XBE
   depends on Z-pass self-validation?** No — the revised
   self-validation contract (§6.1) makes CPU-side VRAM readback
   the primary mechanism, so no XBE should *need* Z-pass count.
   Filing a Metal `NV097_GET_REPORT` implementation as a
   candidate slice anyway because (a) titles that use occlusion
   queries on Metal will silently render wrong, and (b) it
   restores parity with GL.
2. **What is the intended authoritative fallback for W3-only
   hardware behavior: real Xbox capture, nv2a-trace, Cxbx, or
   math-only?** Math-only by default; the XBE's correct output is
   derivable from the catalog's NV097 method semantics + W1's
   implementation. nv2a-trace is the next fallback (it can capture
   the actual NV097 method stream from a homebrew XBE running on
   xemu and confirm the methods are dispatched as expected). Real
   Xbox capture is left as out-of-scope unless the user can
   provide it. Cxbx-Reloaded is advisory only (see §6.3).
3. **Should known GL failures be allowed as expected failures in
   the diagnostic library manifest?** Yes — see §6.3
   "Cross-renderer behavior recorded, not gating."

## 7. Sources / witnesses

### W1 — xemu source

All paths under `/Users/jbbrack03/XEMU_MacOS/xemu-fork/`:
- `hw/xbox/nv2a/nv2a_regs.h` (1522 lines) — NV097 / NV_PGRAPH register
  definitions.
- `hw/xbox/nv2a/pgraph/pgraph.h` (421 lines) — pipeline state, types.
- `hw/xbox/nv2a/pgraph/methods.h.inc` (204 lines) — method dispatch
  table.
- `hw/xbox/nv2a/pgraph/psh_regs.h` (190 lines) — pixel shader / register
  combiner state.
- `hw/xbox/nv2a/pgraph/vsh_regs.h` (211 lines) — vertex shader state.
- `hw/xbox/nv2a/pgraph/surface.h`.
- `hw/xbox/nv2a/pgraph/texture.{h,c}` — texture state machine, format
  table.
- `hw/xbox/nv2a/pgraph/vertex.c` — CPU-side per-element NV2A
  vertex-attribute decoder.
- `hw/xbox/nv2a/pgraph/glsl/{psh,vsh}.h` — translator state structs.
- `hw/xbox/nv2a/pgraph/gl/constants.h` — GL-side enum maps.
- `hw/xbox/nv2a/pgraph/gl/shaders.c` — GL primitive-mode mapping.
- `hw/xbox/nv2a/pgraph/swizzle.{h,c}` — Xbox tile swizzle.

### W2 — nxdk + pbkit

All paths under `/Users/jbbrack03/XEMU_MacOS/nxdk/`:
- `lib/pbkit/pbkit.{h,c}` — driver core.
- `lib/pbkit/nv_regs.h` — register definitions (xboxdevwiki-derived).
- `lib/pbkit/nv_objects.h` — DMA channels, contextual objects.
- `lib/pbkit/nv20_shader.h` — vertex shader instruction set.
- `lib/pbkit/pbkit_draw.{h,c}` — clear primitives.
- `lib/pbkit/pbkit_pushbuffer.{h,c}` — push-buffer macros.
- `lib/pbkit/pbkit_gamma.{h,c}` — DAC palette.
- `lib/pbkit/pbkit_print.h` — text overlay.
- `samples/triangle/`, `samples/mesh/`, `samples/gamma/`, `samples/hello/`
  — code-template references.

Plus the existing fork's
`xemu-fork/scripts/apple-silicon/xbe-tests/flat-tri-depth/main.c` —
existing diagnostic XBE template.

### W3 — External documentation

Primary:
- [xboxdevwiki — NV2A](https://xboxdevwiki.net/NV2A) and subpages
  (`/Vertex_Shader`, `/Pixel_Combiner`, `/Surface_Formats`,
  `/Vertex_attributes`, `/Fixed_Function_Pipeline`).
- [Microsoft Learn — D3D8 D3DRENDERSTATETYPE archived](https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms886344(v=msdn.10)).
- [apitrace dxsdk — d3d8types.h](https://github.com/apitrace/dxsdk/blob/master/Include/d3d8types.h).
- [Khronos NV_register_combiners spec](https://registry.khronos.org/OpenGL/extensions/NV/NV_register_combiners.txt).
- [Khronos NV_texture_shader spec](https://registry.khronos.org/OpenGL/extensions/NV/NV_texture_shader.txt).
- [envytools NV20 PGRAPH](https://envytools.readthedocs.io/en/latest/hw/graph/kelvin/pgraph.html).

Secondary:
- [Cxbx-Reloaded source / issues](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded).
- [xemu issue #320 (texture-format gaps)](https://github.com/xemu-project/xemu/issues/320).
- [xemu issue #2362 (ARL bias)](https://github.com/xemu-project/xemu/issues/2362).
- [Beyond3D NV2A features thread](https://forum.beyond3d.com/threads/xboxs-nv2a-which-of-these-features-does-support.47215/).
- [Wikipedia Xbox technical specifications](https://en.wikipedia.org/wiki/Xbox_technical_specifications).
- [XboxDev nv2a-trace](https://github.com/XboxDev/nv2a-trace).
- [abaire nv2a_vsh_cpu](https://github.com/abaire/nv2a_vsh_cpu).
- [Fabian Giesen — Texture tiling and swizzling](https://fgiesen.wordpress.com/2011/01/17/texture-tiling-and-swizzling/).
- [Blueshogun — Pixel Shader support blog (Dec 2009)](http://shogun3d-cxbx.blogspot.com/2009/12/pixel-shader-support-more-like-re.html).

## 8. How this catalog is consumed

The next deliverable, `diagnostic-xbe-plan.md`, will translate this
catalog into a per-XBE design with the correctness contract baked in
(see §6.3). Each XBE entry in that plan will reference back to the
section above that describes the feature it tests. This catalog
itself does NOT prescribe the test design — it is the reference
material.

When new NV2A features are discovered (e.g., additional texture
format codes, undocumented method behaviors), this catalog gets a
new entry citing the witness; only after that does the XBE plan get
extended to cover it. The catalog is the single source of truth for
"what is the rendering pipeline."

### 8.1 Validation cadence (per Codex review)

This catalog is the source of truth that drives all downstream XBE
designs. Witness-attribution errors here propagate. Therefore:

Claude owns checks-and-balances directly per rule #15; there is no
external validator.

1. **Initial creation**: self-reviewed, fixes applied (this
   revision, 2026-05-05).
2. **Substantive revisions**: any catalog change that adds a feature
   class, retracts a witness claim, or changes a self-validation
   recommendation gets a deliberate self-review pass (rule #15)
   before the diagnostic XBE plan that depends on it is updated.
3. **Diagnostic XBE plan**: `diagnostic-xbe-plan.md` itself gets a
   self-review pass (rule #15) before any nxdk source is written.
4. **First-build XBEs**: self-reviewed (rule #15) after nxdk source
   lands but before the XBE is promoted to a regression gate.

Trivial catalog edits (typos, citation-line-number tweaks) skip
the cadence per project rule #15's "trivial work skips the
self-review automatically" clause.
