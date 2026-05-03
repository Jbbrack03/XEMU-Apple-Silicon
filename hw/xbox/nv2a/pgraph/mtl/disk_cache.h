/*
 * NV2A PGRAPH Metal renderer — persistent MSL-source disk cache (slice M9).
 *
 * Mirrors gl/shaders.c's per-game shader cache pattern, but persists
 * MSL source strings (the result of GLSL → SPIR-V → MSL translation
 * via shadergen.c + shaders.mm) keyed by the PgraphMtlPipelineKey hash
 * rather than GL_PROGRAM_BINARY blobs. Per the strategy.md Phase 4f
 * amendment (2026-05-02), MSL-source caching is preferred over
 * `MTLBinaryArchive` — the latter has limited macOS coverage as of
 * 2026 (DuckStation/Dolphin both reach the same conclusion). MSL
 * source caching achieves the same cold-launch warmup goal more
 * portably and skips the spirv-cross translation step on cache hit.
 *
 * Layout:
 *   <base>/metal_shaders/                  — root dir
 *   <base>/metal_shaders/metal_shader_cache_list — LRU index file
 *                                                  (sequence of uint64_t hashes)
 *   <base>/metal_shaders/<top16>/<bottom48>.msl — per-pipeline MSL file
 *
 * Per-file self-describing header:
 *   uint64_t  xemu_version_len
 *   char      xemu_version[xemu_version_len]
 *   uint64_t  metal_feature_set_len
 *   char      metal_feature_set[metal_feature_set_len]
 *   uint64_t  state_blob_len
 *   uint8_t   state_blob[state_blob_len]   (PgraphMtlPipelineKey bytes)
 *   uint64_t  msl_source_len
 *   char      msl_source[msl_source_len]   (combined MSL source string)
 *
 * Invalidation:
 *   - xemu version mismatch → file deleted on read
 *   - feature-set mismatch  → file deleted on read
 *   - state-blob size mismatch → file deleted on read (handles
 *     PgraphMtlPipelineKey layout changes between builds)
 *   - hash function change → file becomes unreachable; LRU ages out
 *
 * Async writer: each save spawns a detached background QemuThread named
 * `metal-scache-<hash>`. Mirrors the GL pattern in gl/shaders.c:894-897.
 *
 * File-system errors are non-fatal: log + skip + remove the offending
 * file; the renderer falls back to the live translator path. The disk
 * cache is opt-out via `XEMU_METAL_PIPELINE_CACHE=0`; default ON on
 * Apple Silicon.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_DISK_CACHE_H
#define HW_XBOX_NV2A_PGRAPH_MTL_DISK_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PgraphMtlPipelineKey forward-declared so callers don't need to drag
 * shaderstate.h into ObjC++ TUs. */
struct PgraphMtlPipelineKey;

/*
 * Initialize the disk cache. Reads the env var XEMU_METAL_PIPELINE_CACHE
 * (default ON), captures the Metal feature-set string from the active
 * device, ensures the on-disk directory exists, and returns true on
 * success. Returns false (with a logged warning) if any step fails;
 * in that case load/save become silent no-ops.
 *
 * Must be called after pgraph_mtl_heap_init() so the MTLDevice is up.
 */
bool pgraph_mtl_disk_cache_init(void);

/*
 * Tear down the disk cache. Joins any in-flight writer threads so they
 * don't outlive the process's settings dir. Safe to call before init
 * succeeded.
 */
void pgraph_mtl_disk_cache_finalize(void);

/*
 * Reports whether the cache is currently enabled (init succeeded and
 * env var is on). Used by the renderer to decide whether to skip
 * per-pipeline disk hits without walking the file system.
 */
bool pgraph_mtl_disk_cache_enabled(void);

/*
 * Attempt to load the cached MSL source for a given pipeline key.
 *
 * Returns a g_malloc'd NUL-terminated MSL source string on success; the
 * caller takes ownership and must free with `g_free()`. Returns NULL on
 * miss, on header mismatch (xemu version / feature set / blob size),
 * on file-system error, or when the cache is disabled.
 *
 * The g_malloc allocator is used so the result can be passed directly
 * into `pgraph_mtl_shaders_dispatch_build` (which g_free's its inputs).
 *
 * On header mismatch the file is unlinked so future runs re-translate.
 */
char *pgraph_mtl_disk_cache_load_msl(const struct PgraphMtlPipelineKey *key);

/*
 * Asynchronously persist the MSL source for a given pipeline key.
 *
 * Spawns a detached QemuThread (named `metal-scache-<hash>`) that
 * writes the file in the background; returns immediately. The MSL
 * source is duplicated internally so the caller may free its copy as
 * soon as this returns. No-op when the cache is disabled.
 *
 * Append-on-success is also written to the LRU index file via the
 * writer thread, so cold-launch reload sees the latest write order.
 */
void pgraph_mtl_disk_cache_save_msl(const struct PgraphMtlPipelineKey *key,
                                    const char *msl_source);

/*
 * Counter accessors (monotonic, atomic). Surfaced via xemu-metal-perf.c
 * as METAL_SHADER_CACHE_LOADS / METAL_SHADER_CACHE_HITS /
 * METAL_SHADER_CACHE_MISSES.
 *
 *   loads   = total load_msl attempts (whether the cache was enabled
 *             or not; disabled-cache attempts also count, returning 0
 *             for hits + 1 for misses, so summing hits+misses equals
 *             loads).
 *   hits    = load_msl returned a non-NULL MSL source.
 *   misses  = load_msl returned NULL (file absent, mismatch, or error).
 */
uint64_t pgraph_mtl_disk_cache_loads(void);
uint64_t pgraph_mtl_disk_cache_hits(void);
uint64_t pgraph_mtl_disk_cache_misses(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_DISK_CACHE_H */
