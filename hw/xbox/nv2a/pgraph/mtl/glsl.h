/*
 * NV2A PGRAPH Metal renderer — GLSL → SPIR-V → MSL translation (slice M5).
 *
 * Mirrors the structural template in `vk/glsl.c`. Apple Silicon has no
 * native GLSL or SPIR-V consumer, so we route through a Vulkan-flavored
 * SPIR-V intermediate and let spirv-cross emit MSL.
 *
 * The translator does NOT reach into PGRAPHState. It is a pure
 * (glslang_stage_t, const char *) → (char *MSL) transform.
 *
 * Settings used (per metal-renderer-plan.md §3.3):
 *   - MSL version 2.3 (lowest version that supports framebuffer fetch
 *     via [[color(N)]] inputs on macOS Apple Silicon).
 *   - SPVC_COMPILER_OPTION_MSL_FRAMEBUFFER_FETCH_SUBPASS = true (groundwork
 *     for M7's combiner-via-framebuffer-fetch — emitted but unused by the
 *     M5 GLSL).
 *   - SPVC_COMPILER_OPTION_MSL_PLATFORM = MACOS.
 *   - Resource bindings: deterministic, derived from the
 *     descriptor-set / binding indices already wired by `glsl_opts`.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_GLSL_H
#define HW_XBOX_NV2A_PGRAPH_MTL_GLSL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage identifiers. Mirrors glslang_stage_t but exposed locally so
 * callers in the Metal renderer don't need to include glslang headers. */
typedef enum {
    PGRAPH_MTL_GLSL_STAGE_VERTEX   = 0,
    PGRAPH_MTL_GLSL_STAGE_FRAGMENT = 1,
    PGRAPH_MTL_GLSL_STAGE_GEOMETRY = 2,
} PgraphMtlGlslStage;

bool pgraph_mtl_glsl_init(void);
void pgraph_mtl_glsl_finalize(void);

/*
 * Translate a Vulkan-flavored GLSL string to MSL.
 *
 * Returns a malloc'd MSL string on success (caller frees with free()).
 * On failure returns NULL and writes a malloc'd error string to
 * *out_error if out_error is non-NULL (caller frees with free()).
 *
 * The error string is a concatenation of glslang preprocess/parse/link
 * diagnostics or the spirv-cross last-error string.
 */
char *pgraph_mtl_glsl_translate_to_msl(PgraphMtlGlslStage stage,
                                       const char *glsl_source,
                                       char **out_error);

/* Translate GLSL → SPIR-V only. Used by the validation harness for
 * isolating compile failures to either the glslang or spirv-cross
 * step. On success returns a malloc'd uint32_t array and writes its
 * word count to *out_word_count; caller frees with free(). */
uint32_t *pgraph_mtl_glsl_compile_to_spv(PgraphMtlGlslStage stage,
                                         const char *glsl_source,
                                         size_t *out_word_count,
                                         char **out_error);

/* Counters surfaced via util/xemu-metal-perf. */
uint64_t pgraph_mtl_glsl_translate_count(void);
uint64_t pgraph_mtl_glsl_translate_failures(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_GLSL_H */
