/*
 * NV2A PGRAPH Metal renderer — pipeline cache (slice M5/M8).
 *
 * The per-PipelineKey LRU cache built on xemu's `Lru` (include/qemu/lru.h).
 * Mirrors vk/shaders.c's structural layout, replacing VkPipeline /
 * VkRenderPass / VkShaderModule with id<MTLRenderPipelineState> /
 * id<MTLLibrary> / id<MTLFunction>.
 *
 * M5 ships the cache infrastructure end-to-end — including the
 * MTLLibrary build via the M5 GLSL → SPIR-V → MSL translator — but
 * does NOT yet replace the draw-path call site
 * (`pgraph_mtl_pipeline_get_passthrough` in mtl/draw.mm). That
 * call-site swap is paired with M6 (textures + sampler bindings) and
 * M7 (combiner-via-framebuffer-fetch) because a fully-translated
 * shader has uniform buffers + texture bindings the M5 draw machinery
 * does not yet supply. Cache exists; production wiring lands with M6.
 *
 * M8 (2026-05-02) adds an opt-in async build path. On lookup miss with
 * XEMU_METAL_ASYNC_PIPELINE_COMPILE=1 the cache inserts a placeholder
 * entry, dispatches the GLSL → SPIR-V → MSL + MTLLibrary +
 * MTLRenderPipelineState build to a private serial dispatch queue, and
 * returns immediately with state PENDING. The draw layer treats PENDING
 * as "skip the draw this frame" (RPCS3 PR #4876 fallback pattern).
 * Because skipped first-use draws break render-to-texture/postprocess
 * correctness, the default path builds synchronously when
 * XEMU_METAL_TRANSLATED_PIPELINE=1. Passthrough mode keeps async warmup
 * because those translated pipelines are not used for the current draw.
 * On translator/build failure the entry transitions to FAILED and the
 * draw layer falls back to the M3/M4 hand-coded passthrough.
 *
 * Counters:
 *   METAL_PIPELINE_HITS  — lru_lookup hits (state=READY).
 *   METAL_PIPELINE_MISSES — fresh compiles queued (state=MISSING→PENDING).
 *   METAL_PIPELINE_FAILED — translator or pipeline-state build failures.
 *   METAL_SHADER_COMPILE_QUEUED_TOTAL    — async build dispatches.
 *   METAL_SHADER_COMPILE_COMPLETED_TOTAL — async build completions (ok).
 *   METAL_SHADER_COMPILE_FAILED_TOTAL    — async build completions (fail).
 *
 * The async path is gated by env var XEMU_METAL_ASYNC_PIPELINE_COMPILE.
 * When unset, async is disabled for translated drawing and enabled for
 * passthrough warmup. Setting it explicitly overrides that default.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_SHADERS_H
#define HW_XBOX_NV2A_PGRAPH_MTL_SHADERS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PgraphMtlPipelineKey is defined in shaderstate.h, which pulls in the
 * per-target glsl/shaders.h chain. Only the .c side (shadergen.c) and
 * renderer.c include shaderstate.h directly; the .mm side talks via the
 * `pgraph_mtl_shaders_build_pipeline` extern that takes only the
 * Metal-relevant fields, avoiding the C++/glib include conflict. */
struct PgraphMtlPipelineKey;
typedef struct PgraphMtlPipelineKey PgraphMtlPipelineKey;

/* Cache lifecycle. Init brings up the Lru (capacity = 2048 entries),
 * the per-entry mutex, and the M8 async-compile dispatch queue.
 * Finalize drains in-flight compiles, evicts every entry, releases the
 * MTL objects, and tears down the translator if its refcount drops to
 * zero. */
bool pgraph_mtl_shaders_init(void);
void pgraph_mtl_shaders_finalize(void);

/* Per-entry state machine result for the M8 async-compile path. */
typedef enum PgraphMtlPipelineLookupState {
    PGRAPH_MTL_PIPELINE_READY   = 0, /* Pipeline state valid; encode draw. */
    PGRAPH_MTL_PIPELINE_PENDING = 1, /* Compile in flight; caller decides. */
    PGRAPH_MTL_PIPELINE_FAILED  = 2, /* Build failed; fall back to passthrough. */
} PgraphMtlPipelineLookupState;

/*
 * Look up an MTLRenderPipelineState for the given key.
 *
 * Returns: tri-state result via `*out_state`. Returns the pipeline
 * state pointer on READY; returns NULL on PENDING and FAILED.
 *
 * Behavior:
 *   - On hit (READY):    *out_state = READY,   returns the cached PS.
 *   - On hit (PENDING):  *out_state = PENDING, returns NULL.
 *   - On hit (FAILED):   *out_state = FAILED,  returns NULL.
 *   - On miss + async on: inserts placeholder, dispatches build,
 *                         *out_state = PENDING, returns NULL.
 *   - On miss + async off: builds synchronously, *out_state = READY/FAILED.
 */
void *pgraph_mtl_shaders_get_pipeline_ex(const PgraphMtlPipelineKey *key,
                                         PgraphMtlPipelineLookupState *out_state);

/*
 * Compatibility wrapper. Returns the pipeline state on READY, NULL
 * otherwise. Use `pgraph_mtl_shaders_get_pipeline_ex` instead if you
 * need to distinguish PENDING from FAILED.
 */
void *pgraph_mtl_shaders_get_pipeline(const PgraphMtlPipelineKey *key);

uint64_t pgraph_mtl_shaders_pipeline_hits(void);
uint64_t pgraph_mtl_shaders_pipeline_misses(void);
uint64_t pgraph_mtl_shaders_pipeline_failed(void);

/* M8 async-compile counters. */
uint64_t pgraph_mtl_shaders_compile_queued(void);
uint64_t pgraph_mtl_shaders_compile_completed(void);
uint64_t pgraph_mtl_shaders_compile_async_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_SHADERS_H */
