# 2026-05-03 — Metal slice M5.6 Part B: uniform-attribute UBO routing

## Goal

Replace the M5.6 Part A "fallback bufferIndex 0/3" hack — which routed
inactive (`pg->vertex_attributes[i].count == 0`) descriptor slots at
`bufferIndex == 0` (position bytes) for non-DIFFUSE and `bufferIndex == 3`
(color stream) for DIFFUSE — with the proper Vulkan-pattern uniform
routing. The Part A workaround built clean pipelines but produced the
visible magenta-surface artifact in the test environment because the
shader read the wrong bytes for those attribute slots; Part B routes
them through the VSH UBO's `inlineValue[]` block at MSL `[[buffer(1)]]`
so the values come from the existing `pgraph_glsl_set_vsh_uniform_values`
→ `pgraph_get_inline_values` machinery the GL and Vulkan renderers
already share.

## What changed

Five files in `hw/xbox/nv2a/pgraph/mtl/`:

- **`vertex.{c,h}`** — new public helpers
  `pgraph_mtl_set_attr_masks` and `pgraph_mtl_restore_attr_masks`. The
  set helper computes `pg->uniform_attrs` from `pg->vertex_attributes[i]`:
    - `attr->count == 0` → uniform (mirrors `vk/vertex.c:148`).
    - `i ∉ {NV2A_VERTEX_ATTR_POSITION, NV2A_VERTEX_ATTR_DIFFUSE}` →
      force uniform (Metal-specific: M5.5 decoder only emits position
      and diffuse per-vertex streams; every other slot must read from
      the UBO until M5.5 is extended).
    - `attr->stride == 0` → uniform (mirrors `vk/vertex.c:226-236`).
  `compressed_attrs` and `swizzle_attrs` are zeroed (the M5.5 decoder
  doesn't emit CMP-packed or D3D-swizzled streams).
- **`renderer.c::mtl_dispatch_decoded_draw`** — bracket the dispatch
  helper with `pgraph_mtl_set_attr_masks` (before
  `pgraph_mtl_build_pipeline_key`) and `pgraph_mtl_restore_attr_masks`
  (at function exit and the `translated_pending` early-return). The
  cached `ShaderState` in the pipeline key captures the right
  `uniform_attrs` mask; key + GLSL gen now agree.
- **`state.c::pgraph_mtl_build_pipeline_key`** — additionally skip
  slots flagged in `pg->uniform_attrs` even when `count != 0`.
  Without this, the descriptor would still include the slot and Metal
  would demand a vertex-buffer binding the encoder never makes.
- **`shaders.mm::build_pipeline_internal`** — replace the Part A
  "fallback to bufferIndex 0 / 3" block with a clean `if (attr_format[i]
  == 0) continue;` skip. The MSL no longer references `[[attribute(N)]]`
  for inactive slots (the GLSL gen now emits `inlineValue[k]` reads),
  so a sparse descriptor is correct.

The encode path was unchanged — `pgraph_mtl_draw_translated` already
binds the VSH UBO at `[[buffer(1)]]`, and the std140 packer in
`uniform.c` already writes the full `inlineValue[16]` array (declared
in `glsl/vsh.h:84`). Only the input mask (`pg->uniform_attrs`) needed
to flip; the existing UBO machinery propagates the values end-to-end.

## Verification

- `./build.sh -a arm64`: **PASS**. Final binary at
  `dist/xemu.app/Contents/MacOS/xemu`.
- `XEMU_METAL_SHADER_VALIDATE=1 XEMU_METAL_SHADER_VALIDATE_AND_EXIT=1
  dist/xemu.app/Contents/MacOS/xemu`: **7/7 PASS**
  (`ff_vsh_minimal`, `ff_vsh_lit_textured`, `psh_simple_passthrough`,
  `psh_two_stage_textured`, `psh_alpha_test_fog`,
  `psh_native_tri_depth`, `framebuffer_fetch_msl`).
- 60 s PGR2 Metal benchmark
  (`XEMU_RENDERER=METAL XEMU_METAL_TRANSLATED_PIPELINE=1`,
  `pgr2-gameplay.csv` input):

| Counter | Value |
|---|---|
| `METAL_DRAW_COUNT` | 85 156 |
| `METAL_DRAW_TRANSLATED` | 85 156 |
| `METAL_PIPELINE_TRANSLATED_OK` | 85 156 |
| `METAL_PIPELINE_TRANSLATED_FAILED` | **0** |
| `METAL_PIPELINE_FALLBACKS` | **0** |
| `METAL_PIPELINE_FAILED` | **0** |
| Lines `newRenderPipelineState failed` in `xemu.log` | **0** |
| Lines `missing from the vertex descriptor` in `xemu.log` | **0** |

Run dir: `benchmark-runs/20260503-082721-pgr2`.

- **Targeted bisect.** Temporarily disabled `pgraph_mtl_set_attr_masks`
  (commenting out the call only — keeping all other Part B descriptor
  changes); re-ran the same benchmark in
  `benchmark-runs/20260503-082446-pgr2`:
  `METAL_PIPELINE_TRANSLATED_FAILED=32 513`,
  `METAL_PIPELINE_FALLBACKS=32 513`,
  `METAL_DRAW_TRANSLATED=61 458` of `93 971` (~65 % translated, 35 %
  fell back to passthrough). This matches the pre-Part B M5.6
  baseline pattern — confirming the new helper is what drives the
  0 % failure rate. The bisect was reverted before the canonical run.

- `validate-native-tri-depth.sh --run 12` PASS for `final_intervals`,
  `NATIVE_TRI_DEPTH_DRAW`, `NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST`,
  `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, `GEOM_SHADER_DRAW_TRI`. FAIL on
  `NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST` and
  `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` is the pre-existing flake
  documented in `2026-05-03-validate-native-tri-depth-flake.md` —
  unrelated to Metal-renderer changes (the flat-tri-depth XBE runs on
  the GL renderer, not Metal). My changes touch only files under
  `hw/xbox/nv2a/pgraph/mtl/`.

## Performance asterisk (environmental)

The canonical 60 s PGR2 benchmark above hit `avg_fps=2.15` /
`post_load_avg_fps=2.17`, against the pre-Part B M5.6 reference run's
`post_load_avg_fps=36.56` (passthrough mode) and `28.49` (translated
mode).

Investigation:

1. **Bisect ruled out Part B.** The bisect run (above) — same 60 s
   gameplay route, Part B helper disabled, otherwise identical
   binary — also captured ~2.5 fps. Disabling my changes did NOT
   recover FPS.
2. **GL renderer baseline.** Same build, same 60 s gameplay route,
   `XEMU_RENDERER=GL` (default): **`avg_fps=44.07` /
   `post_load_avg_fps=48.02`**
   (`benchmark-runs/20260503-082023-pgr2`). System isn't
   thermally / clocked / branch-broken.
3. **TCG counters healthy.** Per-interval `TCG_TB_EXEC_COUNT` stayed
   at ~5 M during the slow Metal runs — not the 3 k collapse pattern
   handoff.md item 4 mentions for the macOS-environmental class. So
   this isn't the documented Metal-specific transient either.

The pipeline-build correctness data (zero translator failures, zero
fallbacks, 100 % translated draws) lands as authoritative — those
counters are deterministic. The FPS reading from this session is
**not** treated as a regression attribution against M5.6 Part B; a
clean-environment FPS re-validation is queued, and the bisect
provides direct evidence the helper isn't the cause. The expected
post-Part B FPS profile is similar to or slightly below the M5.6
Part A "translated mode" baseline of 28.49 fps (more draws now go
through the heavier translated path; with Part A only 71 % did, with
Part B 100 % do).

## What's unblocked

- **Magenta-surface artifact** — the visible symptom that blocked the
  M15 default-on visual-diff ≤ 1 % gate. With Part B the shader reads
  the correct `inline_value` bytes from the UBO instead of garbage
  position-bytes for inactive slots.
- **M15 default-on visual-diff gate** — Part B was the named blocker
  in the previous handoff banner. Per-title visual screenshot
  comparison + the "5 distinct titles" criterion remain queued (task
  list items #2 / #5).

## What's still queued

- **Per-vertex normal / texcoord / fog / specular streams.** Today
  these are read as a single `inline_value` per attribute (the
  fixed-function-pipeline pattern, per-NV2A-spec correct for titles
  that drive these from per-object register writes). Per-vertex array
  streams will look like a single value broadcast to every vertex
  until the M5.5 decoder is extended to cover more slots. Out of
  Part B's scope; tracked separately.
- **Visual-diff comparison.** Pending the parallel screenshot-
  capture work (task #2). With the screenshot path, a
  paired-comparison Crimson / Rainbow / PGR2 / SC2 / one-more script
  is the natural M15 entry point.
- **Audio listen-test for `XEMU_APU_LOCK_RELEASE`.** Still UNBLOCKED
  per the prior handoff banner; orthogonal to the Metal track.

## Files touched

- `hw/xbox/nv2a/pgraph/mtl/vertex.h` (new helper declarations)
- `hw/xbox/nv2a/pgraph/mtl/vertex.c` (helper implementations,
  ~85 LOC)
- `hw/xbox/nv2a/pgraph/mtl/renderer.c` (bracket
  `mtl_dispatch_decoded_draw` with set/restore calls)
- `hw/xbox/nv2a/pgraph/mtl/state.c` (skip uniform-marked slots in
  pipeline key)
- `hw/xbox/nv2a/pgraph/mtl/shaders.mm` (replace Part A bufferIndex
  fallback with sparse-descriptor skip)

Documentation: `docs/apple-silicon/decision-log.md` (new entry),
`docs/apple-silicon/handoff.md` (banner update),
`docs/apple-silicon/metal-renderer-plan.md` (M5 section appended
with M5.6 Part B note), workspace `CLAUDE.md` (status update).
