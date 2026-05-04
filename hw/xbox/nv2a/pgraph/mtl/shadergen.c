/*
 * NV2A PGRAPH Metal renderer — shader pipeline cache + GLSL generator
 * shim (slice M6 — M5 Part B; M8 — async compile state machine).
 *
 * The .c side owns:
 *   1. The PgraphMtlPipelineKey LRU cache (qemu/lru.h relies on
 *      qemu/queue.h's typeof macros, which aren't portable to .mm).
 *   2. The GLSL generator calls (vsh.h / psh.h require per-target
 *      preprocessor flags that meson's specific_ss only propagates to
 *      .c builds).
 *   3. M8: the per-entry state machine (READY / PENDING / FAILED), a
 *      mutex protecting cache writes, and the async-build dispatch
 *      orchestration (the actual dispatch_async + Metal API calls live
 *      in shaders.mm, but the queueing wrapper lives here so the cache
 *      lookup can transition state under the lock without crossing
 *      back into ObjC++).
 *
 * The .mm side (shaders.mm) owns the Metal-API touchpoints —
 * GLSL→MSL translation, MTLLibrary build, MTLRenderPipelineState
 * build, the dispatch queue, and resource release.
 *
 * On cache miss the .c side calls into shaders.mm via the
 * `pgraph_mtl_shaders_build_pipeline` extern (sync) or the
 * `pgraph_mtl_shaders_dispatch_build` extern (async); on success the
 * returned (pipeline_state, library) handles are stored on the LRU
 * node and released via pgraph_mtl_shaders_release_pipeline on
 * eviction.
 *
 * Counters live in shaders.mm; the .c side calls
 * pgraph_mtl_shaders_inc_{hits,misses,failed} after the lookup outcome
 * is known.
 *
 * Mirrors vk/shaders.c structurally — fast_hash + lru_lookup + memcmp
 * compare + post_node_evict release.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "qemu/mstring.h"
#include "qemu/thread.h"

#include "disk_cache.h"
#include "shaders.h"
#include "shaderstate.h"
#include "glsl.h"

#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"
#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* MSL binding scheme used by the renderer. The Vulkan-flavored GLSL
 * generators emit `layout(set=N, binding=M)` and spirv-cross with
 * `enable_decoration_binding=YES` forwards those numbers to MSL slot
 * indices, giving us a deterministic [[buffer(N)]] / [[texture(N)]] /
 * [[sampler(N)]] map. */
#define MTL_VSH_UBO_BINDING 0
#define MTL_PSH_UBO_BINDING 1
#define MTL_PSH_TEX_BINDING 2

/* -------- counter increment hooks (defined in shaders.mm) -------- */
extern void pgraph_mtl_shaders_inc_hits(void);
extern void pgraph_mtl_shaders_inc_misses(void);
extern void pgraph_mtl_shaders_inc_failed(void);
extern void pgraph_mtl_shaders_inc_compile_queued(void);
extern void pgraph_mtl_shaders_inc_compile_completed(void);
extern void pgraph_mtl_shaders_inc_compile_async_failed(void);

/* -------- pipeline build hook (defined in shaders.mm) --------
 *
 * M9: `pre_translated_msl` (optional, NULL on fresh translation;
 * non-NULL on disk-cache hit) and `out_combined_msl` (optional, returns
 * the combined MSL the build used for disk-cache persistence; caller
 * frees with `free()`).
 */
extern bool pgraph_mtl_shaders_build_pipeline(
    const char *vsh_glsl,
    const char *psh_glsl,
    const char *pre_translated_msl,
    uint32_t color_format,
    uint32_t depth_format,
    uint32_t sample_count,
    uint32_t blend_reg,
    uint32_t control_0,
    unsigned n_attrs,
    const uint32_t *attr_format,
    const uint32_t *attr_offset,
    const uint32_t *attr_buffer_index,
    unsigned n_bufs,
    const uint32_t *buf_stride,
    const uint32_t *buf_step_function,
    const uint32_t *buf_step_rate,
    void **out_pipeline_state,
    void **out_library,
    char **out_combined_msl);
extern void pgraph_mtl_shaders_release_pipeline(void *pipeline_state,
                                                void *library);

/* M8/M9: the .mm side dispatches the build to a private serial queue.
 * `ctx` is an opaque pointer back into this file's PendingCompile — the
 * worker invokes `pgraph_mtl_shaders_async_complete(ctx, ok, ps, lib,
 * combined_msl)` when the build finishes. Per-attribute arrays are
 * heap-allocated in g_malloc and freed by the dispatch worker.
 *
 * M9: `pre_translated_msl` (optional) is owned by the worker — pass a
 * disk-cache-loaded MSL string to skip GLSL→MSL translation. */
extern void pgraph_mtl_shaders_dispatch_build(
    void *ctx,
    char *vsh_glsl,
    char *psh_glsl,
    char *pre_translated_msl,
    uint32_t color_format,
    uint32_t depth_format,
    uint32_t sample_count,
    uint32_t blend_reg,
    uint32_t control_0,
    unsigned n_attrs,
    uint32_t *attr_format,
    uint32_t *attr_offset,
    uint32_t *attr_buffer_index,
    unsigned n_bufs,
    uint32_t *buf_stride,
    uint32_t *buf_step_function,
    uint32_t *buf_step_rate);
extern bool pgraph_mtl_shaders_init_dispatch_queue(void);
extern void pgraph_mtl_shaders_finalize_dispatch_queue(void);

/* -------- GLSL generator shim (also exposed standalone) -------- */

static char *mstring_take_cstr(MString *m)
{
    if (m == NULL) {
        return NULL;
    }
    const char *s = mstring_get_str(m);
    char *out = s ? g_strdup(s) : NULL;
    mstring_unref(m);
    return out;
}

char *pgraph_mtl_shadergen_vsh(const ShaderState *state)
{
    if (state == NULL) {
        return NULL;
    }
    const char *force_passthrough =
        getenv("XEMU_METAL_DEBUG_PASSTHROUGH_VERTEX");
    if (force_passthrough != NULL && force_passthrough[0] != '\0' &&
        force_passthrough[0] != '0') {
        return g_strdup(
            "#version 450\n"
            "layout(location = 0) in vec4 v0;\n"
            "void main() {\n"
            "    gl_Position = v0;\n"
            "}\n");
    }

    GenVshGlslOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.vulkan = true;
    opts.prefix_outputs = false;
    opts.use_push_constants_for_uniform_attrs = false;
    opts.force_uniform_block_full_layout = true;
    opts.ubo_binding = MTL_VSH_UBO_BINDING;
    MString *m = pgraph_glsl_gen_vsh(&state->vsh, opts);
    return mstring_take_cstr(m);
}

char *pgraph_mtl_shadergen_psh(const ShaderState *state)
{
    if (state == NULL) {
        return NULL;
    }
    const char *force_color = getenv("XEMU_METAL_DEBUG_FORCE_FRAGMENT_COLOR");
    if (force_color != NULL && force_color[0] != '\0' &&
        force_color[0] != '0') {
        return g_strdup(
            "#version 450\n"
            "layout(location = 0) out vec4 fragColor;\n"
            "void main() {\n"
            "    fragColor = vec4(1.0, 0.0, 1.0, 1.0);\n"
            "}\n");
    }
    const char *force_tex0 = getenv("XEMU_METAL_DEBUG_FORCE_TEXTURE0");
    if (force_tex0 != NULL && force_tex0[0] != '\0' &&
        force_tex0[0] != '0') {
        if (strcmp(force_tex0, "coords") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(location = 5) in vec4 vtxT0;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    vec2 uv = vtxT0.xy / max(abs(vtxT0.w), 0.00001);\n"
                "    fragColor = vec4(fract(uv.x), fract(uv.y), 0.0, 1.0);\n"
                "}\n");
        }
        if (strcmp(force_tex0, "diffuse") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(location = 0) in vec4 vtxD0;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    fragColor = vec4(vtxD0.rgb, 1.0);\n"
                "}\n");
        }
        if (strcmp(force_tex0, "specular") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(location = 1) in vec4 vtxD1;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    fragColor = vec4(vtxD1.rgb, 1.0);\n"
                "}\n");
        }
        if (strcmp(force_tex0, "raw") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(location = 5) in vec4 vtxT0;\n"
                "layout(binding = 2) uniform sampler2D texSamp0;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    vec2 uv = vtxT0.xy / max(abs(vtxT0.w), 0.00001);\n"
                "    fragColor = texture(texSamp0, uv);\n"
                "}\n");
        }
        if (strcmp(force_tex0, "center") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(binding = 2) uniform sampler2D texSamp0;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    fragColor = texture(texSamp0, vec2(0.5, 0.5));\n"
                "}\n");
        }
        if (strcmp(force_tex0, "center1") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(binding = 3) uniform sampler2D texSamp1;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    fragColor = texture(texSamp1, vec2(0.5, 0.5));\n"
                "}\n");
        }
        if (strcmp(force_tex0, "center2") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(binding = 4) uniform sampler2D texSamp2;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    fragColor = texture(texSamp2, vec2(0.5, 0.5));\n"
                "}\n");
        }
        if (strcmp(force_tex0, "center3") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(binding = 5) uniform sampler2D texSamp3;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    fragColor = texture(texSamp3, vec2(0.5, 0.5));\n"
                "}\n");
        }
        if (strcmp(force_tex0, "raw1") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(location = 6) in vec4 vtxT1;\n"
                "layout(binding = 3) uniform sampler2D texSamp1;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    vec2 uv = vtxT1.xy / max(abs(vtxT1.w), 0.00001);\n"
                "    fragColor = texture(texSamp1, uv);\n"
                "}\n");
        }
        if (strcmp(force_tex0, "raw2") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(location = 7) in vec4 vtxT2;\n"
                "layout(binding = 4) uniform sampler2D texSamp2;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    vec2 uv = vtxT2.xy / max(abs(vtxT2.w), 0.00001);\n"
                "    fragColor = texture(texSamp2, uv);\n"
                "}\n");
        }
        if (strcmp(force_tex0, "raw3") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(location = 8) in vec4 vtxT3;\n"
                "layout(binding = 5) uniform sampler2D texSamp3;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    vec2 uv = vtxT3.xy / max(abs(vtxT3.w), 0.00001);\n"
                "    fragColor = texture(texSamp3, uv);\n"
                "}\n");
        }
        if (strcmp(force_tex0, "scaled") == 0) {
            return g_strdup(
                "#version 450\n"
                "layout(location = 5) in vec4 vtxT0;\n"
                "layout(binding = 1, std140) uniform PshUniforms {\n"
                "int alphaRef;\n"
                "mat2 bumpMat[4];\n"
                "float bumpOffset[4];\n"
                "float bumpScale[4];\n"
                "vec4 clipRange;\n"
                "ivec4 clipRegion[8];\n"
                "uint colorKey[4];\n"
                "uint colorKeyMask[4];\n"
                "vec4 consts[18];\n"
                "float depthFactor;\n"
                "float depthOffset;\n"
                "vec4 fogColor;\n"
                "ivec2 surfaceScale;\n"
                "float texScale[4];\n"
                "};\n"
                "layout(binding = 2) uniform sampler2D texSamp0;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    vec2 uv = vtxT0.xy / max(abs(vtxT0.w), 0.00001);\n"
                "    vec2 denom = vec2(textureSize(texSamp0, 0)) / "
                "max(texScale[0], 0.00001);\n"
                "    fragColor = texture(texSamp0, uv / max(denom, vec2(1.0)));\n"
                "}\n");
        }
        if (strcmp(force_tex0, "const2") == 0 ||
            strcmp(force_tex0, "const4") == 0 ||
            strcmp(force_tex0, "mask") == 0 ||
            strcmp(force_tex0, "lit") == 0) {
            const char *body = NULL;
            if (strcmp(force_tex0, "const2") == 0) {
                body = "    fragColor = vec4(consts[4].rgb, 1.0);\n";
            } else if (strcmp(force_tex0, "const4") == 0) {
                body = "    fragColor = vec4(consts[8].rgb, 1.0);\n";
            } else if (strcmp(force_tex0, "mask") == 0) {
                body =
                    "    float m = texture(texSamp1, t1uv).a + "
                    "texture(texSamp2, t2uv).a + "
                    "texture(texSamp3, t3uv).a;\n"
                    "    fragColor = vec4(vec3(m), 1.0);\n";
            } else {
                body =
                    "    vec4 t0 = texture(texSamp0, t0uv / "
                    "max(vec2(textureSize(texSamp0, 0)) / "
                    "max(texScale[0], 0.00001), vec2(1.0)));\n"
                    "    float m = texture(texSamp1, t1uv).a + "
                    "texture(texSamp2, t2uv).a + "
                    "texture(texSamp3, t3uv).a;\n"
                    "    fragColor = vec4(t0.rgb * vec3(m), 1.0);\n";
            }
            return g_strdup_printf(
                "#version 450\n"
                "layout(location = 5) in vec4 vtxT0;\n"
                "layout(location = 6) in vec4 vtxT1;\n"
                "layout(location = 7) in vec4 vtxT2;\n"
                "layout(location = 8) in vec4 vtxT3;\n"
                "layout(binding = 1, std140) uniform PshUniforms {\n"
                "int alphaRef;\n"
                "mat2 bumpMat[4];\n"
                "float bumpOffset[4];\n"
                "float bumpScale[4];\n"
                "vec4 clipRange;\n"
                "ivec4 clipRegion[8];\n"
                "uint colorKey[4];\n"
                "uint colorKeyMask[4];\n"
                "vec4 consts[18];\n"
                "float depthFactor;\n"
                "float depthOffset;\n"
                "vec4 fogColor;\n"
                "ivec2 surfaceScale;\n"
                "float texScale[4];\n"
                "};\n"
                "layout(binding = 2) uniform sampler2D texSamp0;\n"
                "layout(binding = 3) uniform sampler2D texSamp1;\n"
                "layout(binding = 4) uniform sampler2D texSamp2;\n"
                "layout(binding = 5) uniform sampler2D texSamp3;\n"
                "layout(location = 0) out vec4 fragColor;\n"
                "void main() {\n"
                "    vec2 t0uv = vtxT0.xy / max(abs(vtxT0.w), 0.00001);\n"
                "    vec2 t1uv = vtxT1.xy / max(abs(vtxT1.w), 0.00001);\n"
                "    vec2 t2uv = vtxT2.xy / max(abs(vtxT2.w), 0.00001);\n"
                "    vec2 t3uv = vtxT3.xy / max(abs(vtxT3.w), 0.00001);\n"
                "%s"
                "}\n", body);
        }
        return g_strdup(
            "#version 450\n"
            "layout(location = 5) in vec4 vtxT0;\n"
            "layout(binding = 2) uniform sampler2D texSamp0;\n"
            "layout(location = 0) out vec4 fragColor;\n"
            "void main() {\n"
            "    vec2 uv = vtxT0.xy / max(abs(vtxT0.w), 0.00001);\n"
            "    vec2 size = vec2(textureSize(texSamp0, 0));\n"
            "    fragColor = texture(texSamp0, uv / max(size, vec2(1.0)));\n"
            "}\n");
    }
    GenPshGlslOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.vulkan = true;
    opts.ubo_binding = MTL_PSH_UBO_BINDING;
    opts.tex_binding = MTL_PSH_TEX_BINDING;
    MString *m = pgraph_glsl_gen_psh(&state->psh, opts);
    return mstring_take_cstr(m);
}

/* -------- LRU cache + state machine -------- */

#define MTL_PIPELINE_LRU_CAP 2048

/* Per-entry state machine. The MISSING state is transient — it only
 * exists between cache_init_node and the body of get_pipeline_ex,
 * which decides immediately whether to dispatch async or build sync.
 * External lookups never observe MISSING. */
typedef enum PipelineEntryState {
    PIPELINE_ENTRY_MISSING = 0,  /* Just inserted; build not yet started. */
    PIPELINE_ENTRY_PENDING = 1,  /* Async build dispatched. */
    PIPELINE_ENTRY_READY   = 2,  /* Build complete; pipeline_state valid. */
    PIPELINE_ENTRY_FAILED  = 3,  /* Build failed permanently. */
} PipelineEntryState;

typedef struct PipelineCacheEntry {
    LruNode               node;
    PgraphMtlPipelineKey  key;
    void                 *pipeline_state; /* MTLRenderPipelineState */
    void                 *library;        /* MTLLibrary */
    PipelineEntryState    state;
    /* M8: epoch increments on every entry reuse (insertion or eviction).
     * Async completion handlers compare the captured epoch to the
     * current one and discard the result if the entry has been recycled
     * out from under them. Avoids the racy "completion handler writes
     * to a now-different cache key" bug. */
    uint32_t              epoch;
} PipelineCacheEntry;

static Lru                 s_lru;
static PipelineCacheEntry *s_entries  = NULL;
static bool                s_init     = false;
static QemuMutex           s_lock;

/* M8: env var XEMU_METAL_ASYNC_PIPELINE_COMPILE = {0,1}. By default,
 * keep async warmup for passthrough mode, but switch to synchronous
 * misses when translated drawing is requested. The async path skips
 * draws while a translated pipeline is pending; that is useful as a
 * profiling experiment but not correct enough for render-to-texture and
 * postprocess-heavy games. */
static bool s_async_enabled_cached = false;
static int  s_async_enabled        = -1;

static bool async_enabled(void)
{
    if (!s_async_enabled_cached) {
        const char *e = getenv("XEMU_METAL_ASYNC_PIPELINE_COMPILE");
        if (e == NULL || e[0] == '\0') {
            const char *tp = getenv("XEMU_METAL_TRANSLATED_PIPELINE");
            bool translated =
                (tp != NULL && tp[0] != '\0' && tp[0] != '0');
            s_async_enabled = translated ? 0 : 1;
        } else {
            s_async_enabled = (e[0] != '0') ? 1 : 0;
        }
        s_async_enabled_cached = true;
    }
    return s_async_enabled != 0;
}

/* M8: opaque context handed to the .mm async worker. */
typedef struct PendingCompile {
    /* Identity of the entry at dispatch time. The worker's completion
     * handler re-validates by hash + epoch under the cache lock before
     * writing the result, so a recycled entry doesn't get stomped. */
    PipelineCacheEntry *entry;
    uint64_t            hash;
    uint32_t            epoch;
} PendingCompile;

/* -------- LRU adapters -------- */

static void cache_init_node(Lru *lru, LruNode *node, const void *key)
{
    (void)lru;
    PipelineCacheEntry *e = container_of(node, PipelineCacheEntry, node);
    memcpy(&e->key, key, sizeof(PgraphMtlPipelineKey));
    e->pipeline_state = NULL;
    e->library        = NULL;
    e->state          = PIPELINE_ENTRY_MISSING;
    e->epoch++;
}

static bool cache_compare_nodes(Lru *lru, LruNode *node, const void *key)
{
    (void)lru;
    PipelineCacheEntry *e = container_of(node, PipelineCacheEntry, node);
    return memcmp(&e->key, key, sizeof(PgraphMtlPipelineKey)) != 0;
}

static void cache_post_evict(Lru *lru, LruNode *node)
{
    (void)lru;
    PipelineCacheEntry *e = container_of(node, PipelineCacheEntry, node);
    pgraph_mtl_shaders_release_pipeline(e->pipeline_state, e->library);
    e->pipeline_state = NULL;
    e->library        = NULL;
    e->state          = PIPELINE_ENTRY_MISSING;
    e->epoch++;
    memset(&e->key, 0, sizeof(e->key));
}

/* -------- public lifecycle -------- */

bool pgraph_mtl_shaders_init(void)
{
    if (s_init) {
        return true;
    }
    if (!pgraph_mtl_glsl_init()) {
        fprintf(stderr,
                "pgraph_mtl_shaders_init: GLSL translator init failed\n");
        return false;
    }

    s_entries = (PipelineCacheEntry *)
        g_malloc0_n(MTL_PIPELINE_LRU_CAP, sizeof(PipelineCacheEntry));
    if (!s_entries) {
        pgraph_mtl_glsl_finalize();
        return false;
    }

    lru_init(&s_lru);
    for (int i = 0; i < MTL_PIPELINE_LRU_CAP; i++) {
        lru_add_free(&s_lru, &s_entries[i].node);
    }
    s_lru.init_node       = cache_init_node;
    s_lru.compare_nodes   = cache_compare_nodes;
    s_lru.post_node_evict = cache_post_evict;

    qemu_mutex_init(&s_lock);

    if (!pgraph_mtl_shaders_init_dispatch_queue()) {
        fprintf(stderr,
                "pgraph_mtl_shaders_init: async dispatch queue init "
                "failed; will fall back to synchronous compiles\n");
    }

    /* M9: bring up the persistent MSL-source disk cache. Failure here
     * is non-fatal — the cache becomes a no-op and the renderer
     * always re-translates. */
    if (!pgraph_mtl_disk_cache_init()) {
        fprintf(stderr,
                "pgraph_mtl_shaders_init: disk shader cache init "
                "failed; cold-launch translation cost will not be "
                "amortized across runs\n");
    }

    s_init = true;
    return true;
}

void pgraph_mtl_shaders_finalize(void)
{
    if (!s_init) {
        return;
    }
    /* Drain in-flight async compiles before tearing down the cache so
     * a late completion handler doesn't write into freed memory. */
    pgraph_mtl_shaders_finalize_dispatch_queue();

    /* M9: drain disk-cache writer threads. Must happen after the
     * dispatch queue is drained because async-completion handlers may
     * spawn writer threads. */
    pgraph_mtl_disk_cache_finalize();

    qemu_mutex_lock(&s_lock);
    lru_flush(&s_lru);
    g_free(s_entries);
    s_entries = NULL;
    qemu_mutex_unlock(&s_lock);

    qemu_mutex_destroy(&s_lock);
    s_init = false;
    pgraph_mtl_glsl_finalize();
}

/* -------- async completion callback -------- */

/*
 * Called by the .mm async worker when a build completes. `ctx` is the
 * PendingCompile pointer originally handed to dispatch_build; this
 * function takes ownership and frees it.
 *
 * The completion path validates the entry hasn't been evicted/recycled
 * since dispatch by comparing (hash, epoch) under the cache lock. If
 * the entry was recycled, the just-built pipeline is released via
 * pgraph_mtl_shaders_release_pipeline (so it doesn't leak), and the
 * recycled entry is left in whatever state it has now.
 *
 * M9: `combined_msl` (optional, owned by callee — must be free()'d) is
 * the combined MSL the build used. Non-NULL only on fresh translation
 * (cache miss path); NULL when the build came from a disk-cache hit.
 * On successful build of a fresh translation we persist the MSL to the
 * disk cache so the next launch skips the translation step.
 */
void pgraph_mtl_shaders_async_complete(void *ctx, bool ok,
                                       void *pipeline_state, void *library,
                                       char *combined_msl)
{
    PendingCompile *pc = (PendingCompile *)ctx;
    if (pc == NULL) {
        if (ok && (pipeline_state != NULL || library != NULL)) {
            pgraph_mtl_shaders_release_pipeline(pipeline_state, library);
        }
        if (combined_msl) {
            free(combined_msl);
        }
        return;
    }

    bool stomped = false;
    /* Snapshot the key-blob under the lock so the disk-cache save (which
     * happens outside the lock) doesn't race the entry's recycle. */
    PgraphMtlPipelineKey saved_key;
    bool                 should_save = false;

    qemu_mutex_lock(&s_lock);

    if (s_init && pc->entry != NULL) {
        PipelineCacheEntry *e = pc->entry;
        /* Validate the entry still matches what we dispatched. */
        if (e->epoch == pc->epoch && e->state == PIPELINE_ENTRY_PENDING) {
            if (ok && pipeline_state != NULL) {
                e->pipeline_state = pipeline_state;
                e->library        = library;
                e->state          = PIPELINE_ENTRY_READY;
                pgraph_mtl_shaders_inc_compile_completed();
                if (combined_msl != NULL) {
                    /* Fresh translation succeeded — capture the key for
                     * disk-cache persistence. We do the actual save
                     * outside the lock to avoid serializing renderer
                     * cache lookups behind the writer thread spawn. */
                    memcpy(&saved_key, &e->key, sizeof(saved_key));
                    should_save = true;
                }
            } else {
                e->state = PIPELINE_ENTRY_FAILED;
                pgraph_mtl_shaders_inc_failed();
                pgraph_mtl_shaders_inc_compile_async_failed();
            }
        } else {
            /* Entry was recycled; release the pipeline we just built. */
            stomped = true;
        }
    } else {
        stomped = true;
    }

    qemu_mutex_unlock(&s_lock);

    if (stomped && (pipeline_state != NULL || library != NULL)) {
        pgraph_mtl_shaders_release_pipeline(pipeline_state, library);
    }

    if (should_save && combined_msl != NULL) {
        pgraph_mtl_disk_cache_save_msl(&saved_key, combined_msl);
    }
    if (combined_msl) {
        free(combined_msl);
    }

    g_free(pc);
}

/* -------- public lookup -------- */

void *pgraph_mtl_shaders_get_pipeline_ex(const PgraphMtlPipelineKey *key,
                                         PgraphMtlPipelineLookupState *out_state)
{
    PgraphMtlPipelineLookupState dummy;
    if (out_state == NULL) {
        out_state = &dummy;
    }
    *out_state = PGRAPH_MTL_PIPELINE_FAILED;

    if (!s_init || key == NULL) {
        return NULL;
    }

    uint64_t hash = fast_hash((const uint8_t *)key,
                              sizeof(PgraphMtlPipelineKey));

    /* Snapshot data we need outside the lock for sync/async build. */
    bool need_dispatch_async = false;
    bool need_build_sync     = false;
    void *result_ps          = NULL;
    PendingCompile *pc       = NULL;
    PipelineCacheEntry *active_entry = NULL;

    char    *vsh_glsl   = NULL;
    char    *psh_glsl   = NULL;
    /* M9: optional pre-translated MSL from the disk cache. When non-NULL
     * the build skips GLSL→MSL translation. */
    char    *cached_msl = NULL;
    uint32_t color_fmt  = 0;
    uint32_t depth_fmt  = 0;
    uint32_t sample_cnt = 0;
    /* Heap-allocated so they outlive the lock for the async path. */
    uint32_t *attr_fmt      = NULL;
    uint32_t *attr_off      = NULL;
    uint32_t *attr_buf      = NULL;
    uint32_t *buf_stride    = NULL;
    uint32_t *buf_step_fn   = NULL;
    uint32_t *buf_step_rate = NULL;

    /* Phase 1: lookup or insert under the lock. */
    qemu_mutex_lock(&s_lock);

    LruNode *node = lru_lookup(&s_lru, hash, key);
    PipelineCacheEntry *e = container_of(node, PipelineCacheEntry, node);

    switch (e->state) {
    case PIPELINE_ENTRY_READY:
        pgraph_mtl_shaders_inc_hits();
        result_ps   = e->pipeline_state;
        *out_state  = PGRAPH_MTL_PIPELINE_READY;
        break;

    case PIPELINE_ENTRY_PENDING:
        /* Quietly count this as a hit for cache occupancy purposes —
         * the work is in flight, no fresh compile needed. */
        pgraph_mtl_shaders_inc_hits();
        *out_state  = PGRAPH_MTL_PIPELINE_PENDING;
        break;

    case PIPELINE_ENTRY_FAILED:
        *out_state  = PGRAPH_MTL_PIPELINE_FAILED;
        break;

    case PIPELINE_ENTRY_MISSING:
    default:
        pgraph_mtl_shaders_inc_misses();
        if (async_enabled()) {
            e->state = PIPELINE_ENTRY_PENDING;
            need_dispatch_async = true;
            *out_state = PGRAPH_MTL_PIPELINE_PENDING;
        } else {
            need_build_sync = true;
        }
        break;
    }

    /* Snapshot key data while still holding the lock. */
    if (need_dispatch_async || need_build_sync) {
        active_entry = e;
        color_fmt    = e->key.render_pass_state.color_format;
        depth_fmt    = e->key.render_pass_state.depth_format;
        sample_cnt   = e->key.render_pass_state.sample_count;

        attr_fmt      = g_malloc_n(NV2A_VERTEXSHADER_ATTRIBUTES,
                                   sizeof(uint32_t));
        attr_off      = g_malloc_n(NV2A_VERTEXSHADER_ATTRIBUTES,
                                   sizeof(uint32_t));
        attr_buf      = g_malloc_n(NV2A_VERTEXSHADER_ATTRIBUTES,
                                   sizeof(uint32_t));
        buf_stride    = g_malloc_n(NV2A_VERTEXSHADER_ATTRIBUTES,
                                   sizeof(uint32_t));
        buf_step_fn   = g_malloc_n(NV2A_VERTEXSHADER_ATTRIBUTES,
                                   sizeof(uint32_t));
        buf_step_rate = g_malloc_n(NV2A_VERTEXSHADER_ATTRIBUTES,
                                   sizeof(uint32_t));
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            attr_fmt[i]      = e->key.attrs[i].format;
            attr_off[i]      = e->key.attrs[i].offset;
            attr_buf[i]      = e->key.attrs[i].buffer_index;
            buf_stride[i]    = e->key.bufs[i].stride;
            buf_step_fn[i]   = e->key.bufs[i].step_function;
            buf_step_rate[i] = e->key.bufs[i].step_rate;
        }

        /* M9: try to load the pre-translated combined MSL from the
         * persistent disk cache before generating GLSL. On hit we'll
         * skip the GLSL→SPIR-V→MSL translation step entirely. The lookup
         * is done under the cache lock to prevent races with another
         * lookup that lands on the same entry; it's a fast-path stat()
         * and read on the disk anyway, so the lock-hold cost is minimal
         * and bounded by file-system latency. */
        cached_msl = pgraph_mtl_disk_cache_load_msl(&e->key);

        /* Generate the GLSL strings under the lock — pgraph_mtl_shadergen_*
         * read shader_state. We still generate them on a disk-cache hit
         * because the build helper's API takes both; the strings are
         * cheap to generate and unused when cached_msl is non-NULL. */
        vsh_glsl = pgraph_mtl_shadergen_vsh(&e->key.shader_state);
        psh_glsl = pgraph_mtl_shadergen_psh(&e->key.shader_state);

        if (vsh_glsl == NULL || psh_glsl == NULL) {
            /* Fail the entry; we'll drop into FAILED below. */
            e->state = PIPELINE_ENTRY_FAILED;
            *out_state = PGRAPH_MTL_PIPELINE_FAILED;
            pgraph_mtl_shaders_inc_failed();
            need_dispatch_async = false;
            need_build_sync     = false;
            g_free(vsh_glsl);
            g_free(psh_glsl);
            vsh_glsl = NULL;
            psh_glsl = NULL;
            if (cached_msl) {
                g_free(cached_msl);
                cached_msl = NULL;
            }
            g_free(attr_fmt);     attr_fmt      = NULL;
            g_free(attr_off);     attr_off      = NULL;
            g_free(attr_buf);     attr_buf      = NULL;
            g_free(buf_stride);   buf_stride    = NULL;
            g_free(buf_step_fn);  buf_step_fn   = NULL;
            g_free(buf_step_rate); buf_step_rate = NULL;
        } else if (need_dispatch_async) {
            pc = g_new0(PendingCompile, 1);
            pc->entry = e;
            pc->hash  = hash;
            pc->epoch = e->epoch;
        }
    }

    qemu_mutex_unlock(&s_lock);

    /* Async path: ownership of vsh_glsl / psh_glsl / cached_msl /
     * attr_* arrays transfers to the dispatch worker. */
    if (need_dispatch_async && pc != NULL) {
        pgraph_mtl_shaders_inc_compile_queued();
        pgraph_mtl_shaders_dispatch_build(
            pc, vsh_glsl, psh_glsl, cached_msl,
            color_fmt, depth_fmt, sample_cnt,
            key->regs[0], key->regs[2],
            NV2A_VERTEXSHADER_ATTRIBUTES,
            attr_fmt, attr_off, attr_buf,
            NV2A_VERTEXSHADER_ATTRIBUTES,
            buf_stride, buf_step_fn, buf_step_rate);
        return NULL;
    }

    /* Sync fallback path. */
    if (need_build_sync) {
        void *ps_handle      = NULL;
        void *lib_handle     = NULL;
        char *combined_built = NULL;
        bool ok = pgraph_mtl_shaders_build_pipeline(
            vsh_glsl, psh_glsl, cached_msl,
            color_fmt, depth_fmt, sample_cnt,
            key->regs[0], key->regs[2],
            NV2A_VERTEXSHADER_ATTRIBUTES,
            attr_fmt, attr_off, attr_buf,
            NV2A_VERTEXSHADER_ATTRIBUTES,
            buf_stride, buf_step_fn, buf_step_rate,
            &ps_handle, &lib_handle,
            /* Capture combined MSL only on fresh translation. */
            (cached_msl != NULL) ? NULL : &combined_built);
        g_free(vsh_glsl);
        g_free(psh_glsl);
        if (cached_msl) {
            g_free(cached_msl);
            cached_msl = NULL;
        }
        g_free(attr_fmt);
        g_free(attr_off);
        g_free(attr_buf);
        g_free(buf_stride);
        g_free(buf_step_fn);
        g_free(buf_step_rate);

        qemu_mutex_lock(&s_lock);
        if (active_entry->state == PIPELINE_ENTRY_MISSING) {
            if (ok) {
                active_entry->pipeline_state = ps_handle;
                active_entry->library        = lib_handle;
                active_entry->state          = PIPELINE_ENTRY_READY;
                result_ps  = ps_handle;
                *out_state = PGRAPH_MTL_PIPELINE_READY;
                /* M9: persist the freshly-translated MSL to disk so
                 * the next launch skips the spirv-cross step. The
                 * save is async (background writer thread) so it
                 * doesn't block the renderer thread. */
                if (combined_built != NULL) {
                    pgraph_mtl_disk_cache_save_msl(&active_entry->key,
                                                   combined_built);
                }
            } else {
                active_entry->state = PIPELINE_ENTRY_FAILED;
                pgraph_mtl_shaders_inc_failed();
                *out_state = PGRAPH_MTL_PIPELINE_FAILED;
            }
        } else {
            /* Entry recycled while we were building synchronously
             * (would only happen if another thread evicted it; we
             * hold the renderer thread for the whole sync path so
             * this should not occur in practice). Release built
             * resources. */
            qemu_mutex_unlock(&s_lock);
            if (ok) {
                pgraph_mtl_shaders_release_pipeline(ps_handle, lib_handle);
            }
            if (combined_built) {
                free(combined_built);
            }
            *out_state = PGRAPH_MTL_PIPELINE_FAILED;
            return NULL;
        }
        qemu_mutex_unlock(&s_lock);
        if (combined_built) {
            free(combined_built);
        }
    }

    return result_ps;
}

void *pgraph_mtl_shaders_get_pipeline(const PgraphMtlPipelineKey *key)
{
    PgraphMtlPipelineLookupState st;
    void *ps = pgraph_mtl_shaders_get_pipeline_ex(key, &st);
    return (st == PGRAPH_MTL_PIPELINE_READY) ? ps : NULL;
}
