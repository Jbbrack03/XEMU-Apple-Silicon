/*
 * Apple Silicon performance fork: Metal renderer perf counters.
 *
 * Counters decomposing the Metal renderer's per-interval activity:
 *
 *   METAL_DRAW_COUNT
 *       Total `pgraph_mtl_draw_passthrough` + `pgraph_mtl_draw_indexed`
 *       successful draw encodes per interval.
 *   METAL_DRAW_INDEXED_COUNT
 *       Subset of METAL_DRAW_COUNT that took the index-expanded path
 *       (triangle_fan, quads, quad_strip, polygon, line_loop). Comparing
 *       this to METAL_DRAW_COUNT shows the geometry-expansion fraction
 *       of the workload.
 *   METAL_NATIVE_TRI_DEPTH_DRAWS
 *       Triangle-family draws that ran through the M4 native-depth
 *       fragment shader (M3's passthrough does not write depth). Should
 *       parallel the existing GL counter NATIVE_TRI_DEPTH_DRAW for the
 *       same workload — that's the M4 exit-gate signal.
 *   METAL_NATIVE_QUAD_DRAWS
 *       Quad-family draws that ran through native-depth + index
 *       expansion. Parallels GL's NATIVE_QUAD_DRAW.
 *   METAL_CLEAR_COUNT
 *       `clear_surface` invocations that produced a Metal render-pass
 *       clear (matches the M2 counter that was not yet plumbed through
 *       the perf interval line).
 *   METAL_GLSL_TRANSLATE
 *       Successful GLSL → SPIR-V → MSL translations through the M5
 *       translator (`pgraph_mtl_glsl_translate_to_msl`). Lifetime
 *       counter so the per-interval delta tracks shader compile rate.
 *       Pairs with METAL_GLSL_TRANSLATE_FAIL to flag silent fallback.
 *   METAL_GLSL_TRANSLATE_FAIL
 *       Translator failures (glslang or spirv-cross rejected the
 *       input). When the M6+ draw cache lands, every increment here
 *       indicates a draw that could not be served by the
 *       state-driven shader path.
 *   METAL_SHADER_VALIDATE_OK / METAL_SHADER_VALIDATE_FAIL
 *       M5 validation-harness fixture pass / fail counts. Increments
 *       only when XEMU_METAL_SHADER_VALIDATE is set; otherwise both
 *       are zero.
 *
 * The counters are always-on monotonic atomics inside
 * `hw/xbox/nv2a/pgraph/mtl/{draw,surface}.{mm,c}`; this header exposes
 * a single emit/reset hook used by `nv2a_profile_log_emit_interval`,
 * matching the pattern used by xemu-tcg-perf / xemu-apu-perf /
 * xemu-display-perf.
 */

#ifndef QEMU_XEMU_METAL_PERF_H
#define QEMU_XEMU_METAL_PERF_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Append `KEY=value` fields to the open `xemu-perf:` interval line and
 * reset the per-interval deltas. Called from
 * `nv2a_profile_log_emit_interval` regardless of which renderer is
 * active — when the Metal renderer is not in use the counters are
 * zero and the function is a no-op. */
void xemu_metal_perf_emit_and_reset(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_METAL_PERF_H */
