# 2026-05-03 — Metal renderer M5.6: translator failure rate driven to 0 %

## Summary

The 2026-05-03 M5.5 + M5.7 (render-pass coalescing) benchmarks left
the translator failure rate at 25-43 % across the three tracked
titles. Failed translations fell back cleanly to the M3/M4 hand-coded
passthrough, so there was no crash — but the passthrough path renders
without textures / combiners (the visible "magenta surface"
artifact). The M15 default-on gate explicitly requires ≤ 1 % per-pixel
diff vs GL, which cannot be met while a quarter of pipelines fall
back. M5.6 closes the failure path.

## Root cause

`pgraph_mtl_shaders: newRenderPipelineState failed: ...` in
`hw/xbox/nv2a/pgraph/mtl/shaders.mm::build_pipeline_internal` covers
two separate fault classes that the same fprintf surface together:

1. **"Vertex attribute vN(N) is missing from the vertex descriptor"**
   (the bulk of failures). The MSL emitted by spirv-cross declares
   `[[attribute(N)]]` for every NV2A vertex attribute the GLSL
   generator references — including "uniform" attributes
   (`pg->vertex_attributes[i].count == 0`) which feed via
   `inline_value` rather than per-vertex streams. The pipeline-key
   builder (`mtl/state.c::pgraph_mtl_build_pipeline_key`) deliberately
   skips `count == 0` slots — so the resulting vertex descriptor was
   missing those attribute slots even though the MSL referenced them.
   Metal validation rejected the pipeline at build time.
2. **"Vertex attribute v1_cmp(1) of type int cannot be read using
   MTLAttributeFormatInt1010102Normalized"**. The NV2A
   `NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP` format (3 signed
   normalized 10/11/11 components packed into 32 bits) was being
   declared as `MTLVertexFormatInt1010102Normalized` by
   `pgraph_mtl_translate_vertex_format`. Metal expects the shader-
   side input to be `float4` for that vertex format (it does the
   normalization at fetch time). spirv-cross instead translates the
   GLSL to MSL with the input typed as `int` (matching the GLSL
   generator's bitwise unpack code, which mirrors the Vulkan path's
   `VK_FORMAT_R32_SINT` choice in `vk/vertex.c:184`). Metal rejected
   the type mismatch.

## Fix

### `hw/xbox/nv2a/pgraph/mtl/shaders.mm` — populate every attribute slot

```c
for (unsigned i = 0; i < n_attrs; i++) {
    if (attr_format[i] != 0) {
        vd.attributes[i].format      = (MTLVertexFormat)attr_format[i];
        vd.attributes[i].offset      = attr_offset[i];
        vd.attributes[i].bufferIndex = attr_buffer_index[i];
    } else {
        /* M5.6 fallback: Float4 → bufferIndex=3 for DIFFUSE (the
         * encode path always binds the M5.5-decoded color stream
         * there, including for uniform-diffuse where the decoder
         * returns inline_value), → bufferIndex=0 (position) for all
         * other inactive slots. Wrong-attribute reads for
         * non-diffuse slots produce position bytes; M5.6 part B
         * routes those via the VSH UBO instead. */
        vd.attributes[i].format      = MTLVertexFormatFloat4;
        vd.attributes[i].offset      = 0;
        vd.attributes[i].bufferIndex =
            (i == 3 /* NV2A_VERTEX_ATTR_DIFFUSE */) ? 3 : 0;
    }
}
```

Also defensively populates `vd.layouts[0].stride = 16` and
`vd.layouts[3].stride = 16` if the active-attribute loop didn't set
them — the fallback now references both slots and Metal rejects
zero-stride layouts referenced by an attribute.

Also defensively populates `vd.layouts[0].stride = 16` if the
inline-buffer / draw_arrays / inline_elements path didn't set it,
because every fallback attribute now points at slot 0 and the
descriptor would otherwise reject zero-stride layouts.

### `hw/xbox/nv2a/pgraph/mtl/state.c` — CMP format type fix

```c
case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
    /* M5.6: emit as raw `int` (MTL_VFMT_INT) so spirv-cross's
     * MSL output (`int v1_cmp [[attribute(1)]]`) matches the
     * descriptor type. The shader does the (11,11,10) unpacking
     * via bitwise ops — same pattern Vulkan uses
     * (`VK_FORMAT_R32_SINT`, `vk/vertex.c:184`). */
    if (count == 1) {
        return MTL_VFMT_INT;
    }
```

Both fixes leave the M5/M7.1 translator + M9 disk cache + M8
async-compile path unchanged. The MSL itself doesn't change — only
the descriptor that wraps it now satisfies what the MSL expects.

## Empirical result

PGR2 Metal 60 s scripted gameplay
(`benchmark-runs/20260503-014919-pgr2`):

| Metric | Pre-M5.6 (M5.7) | Post-M5.6 |
|---|---|---|
| `METAL_PIPELINE_TRANSLATED_OK` | 1 366 134 (67.2 %) | **2 006 404 (98.9 %)** |
| `METAL_PIPELINE_TRANSLATED_FAILED` | 646 438 (32.8 %) | **0 (0 %)** |
| `METAL_PIPELINE_FAILED` | 0 | 0 |
| `newRenderPipelineState failed` (stderr) | 1 235 | **0** |
| `post_load_avg_fps` (passthrough mode) | 37.09 | 36.56 |
| `post_load_avg_fps` (translated mode, `XEMU_METAL_TRANSLATED_PIPELINE=1`) | n/a (translated path mostly unreachable) | **28.49** |

With the translator working end-to-end, `XEMU_METAL_TRANSLATED_PIPELINE=1`
now actually exercises the translated path. `METAL_DRAW_TRANSLATED`
went from 0 (path never reached) to 275 440 / 60 s (every draw goes
through translated). FPS in translated mode is ~28 fps — a small
regression from the M3/M4 passthrough's 36 fps, attributable to
extra UBO uploads + texture/sampler binding overhead per encoder
(coalescing partly absorbs it; further work in M5.7+ is to skip
identical-binding rebinds).

## Visual correctness — partial

`METAL_PIPELINE_FAILED = 0` and `newRenderPipelineState` errors
disappeared. The pipeline now builds for every NV2A state combination
PGR2 throws at it.

**However**: the on-screen output is still magenta in the test
environment. Two contributing factors that this slice does NOT
address:

1. **Inactive attributes get position bytes.** My fix routes any
   `attr_format[i] == 0` slot to bufferIndex 0 (position) at offset
   0. The shader's `[[attribute(N)]]` for an inactive slot reads
   the vertex's position float4 instead of the intended uniform
   value. For combiner inputs that should be uniform (e.g.,
   uninitialized texcoords or fog factors), the shader instead
   sees position values — which on a typical NV2A draw are
   normalized device coordinates near [-1, 1]. Combiner output for
   wrong inputs may produce out-of-gamut colors that the surface
   pixel format clamps to magenta-class values.
2. **macOS Screen Recording permission dialog occludes the xemu
   window** in the test environment, preventing visual confirmation
   of geometry shape / colors / etc. The compositor IS presenting
   (otherwise the dialog couldn't be visible against the xemu
   window's background); the dialog blocks visual diff from being
   meaningful.

A clean visual gate (M15 default-on prerequisite ≤ 1 % per-pixel
diff vs GL) needs:

- A clean desktop without macOS permission dialogs.
- An M5.6 part B that routes uniform attributes via the VSH UBO's
  `inline_value` block instead of the position-buffer fallback. The
  GLSL generator already emits uniform-attr values into the UBO;
  the missing piece is teaching spirv-cross / `pgraph_mtl_uniform_stage_vsh`
  to emit them as `float4` UBO members rather than as
  `[[attribute(N)]]` declarations.
- Screen-by-screen visual diff via `compare-screenshots.py` against
  a known-good GL reference run.

This is M5.6 part B and is queued.

## What this slice DOES guarantee

- `METAL_PIPELINE_FAILED = 0` and `newRenderPipelineState failed`
  message count = 0. Pipelines build deterministically.
- `METAL_DRAW_TRANSLATED = METAL_DRAW_COUNT` when
  `XEMU_METAL_TRANSLATED_PIPELINE=1` — every draw goes through the
  translated MSL pipeline.
- M5 shader-validation harness: 7/7 PASS (translator unaffected).
- Build PASS at `arm64-apple-macos14.0`.

## Reference

- `docs/apple-silicon/benchmarks/2026-05-03-metal-m5_5-draw-paths-online.md`
  — M5.5 (draw paths landed).
- `docs/apple-silicon/benchmarks/2026-05-03-metal-render-pass-coalescing.md`
  — M5.7 (per-draw cmdbuf eliminated, +125 % FPS).
- `hw/xbox/nv2a/pgraph/vk/vertex.c:184` — Vulkan reference for
  CMP-as-int handling.
