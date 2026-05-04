/*
 * NV2A PGRAPH Metal renderer — persistent MSL-source disk cache (slice M9).
 *
 * Implementation. See disk_cache.h for the API contract and the file
 * layout / header format. Mirrors gl/shaders.c's structural pattern
 * line-by-line (read-back-or-error sentinel, per-shader background
 * writer thread, top-16 / bottom-48 hash sharding, LRU index file).
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/fast-hash.h"
#include "qemu/thread.h"

#include "xemu-version.h"
#include "ui/xemu-settings.h"

#include "disk_cache.h"
#include "heap.h"
#include "shaderstate.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------- module state -------- */

static bool   s_init                  = false;
static bool   s_enabled               = false;
static char  *s_root_dir              = NULL; /* "<base>/metal_shaders" */
static char  *s_lru_path              = NULL; /* "<base>/metal_shaders/metal_shader_cache_list" */
static char   s_feature_set[128]      = {0};  /* "AppleGPUFamily<N>/macOS<M>.<m>" */
static size_t s_feature_set_len       = 0;    /* strlen(feature_set) + 1 (NUL) */

static QemuMutex s_writer_lock;     /* Serializes LRU-index appends + active-counter updates. */
static QemuCond  s_writer_cond;     /* Signaled when a writer exits; finalize waits on it. */
static bool      s_writer_lock_inited = false;
static unsigned  s_active_writers    = 0;

#define MTL_DISK_CACHE_ABI_TAG "metal-vsh-full-ubo-20260504"

/* In-flight writer thread tracking. Cap is concurrent (running)
 * writers; new saves block synchronously when the cap is hit (the
 * cache is best-effort but skipping the write entirely is worse than
 * briefly blocking the caller).
 *
 * Writers run detached: each save spawns a QEMU_THREAD_DETACHED thread
 * that auto-releases on completion. We don't track individual
 * QemuThread handles after spawn; instead a counter `s_active_writers`
 * is decremented on writer exit, and finalize blocks on
 * s_writer_cond until it reaches zero. */
#define MTL_DISK_MAX_WRITERS 64

/* -------- counters -------- */

static _Atomic uint64_t s_loads  = 0;
static _Atomic uint64_t s_hits   = 0;
static _Atomic uint64_t s_misses = 0;

uint64_t pgraph_mtl_disk_cache_loads(void)
{
    return atomic_load(&s_loads);
}
uint64_t pgraph_mtl_disk_cache_hits(void)
{
    return atomic_load(&s_hits);
}
uint64_t pgraph_mtl_disk_cache_misses(void)
{
    return atomic_load(&s_misses);
}

/* -------- env-var gate -------- */

static bool env_cache_enabled(void)
{
    const char *e = getenv("XEMU_METAL_PIPELINE_CACHE");
    if (e == NULL || e[0] == '\0') {
        return true;  /* Default ON on Apple Silicon. */
    }
    return e[0] != '0';
}

static char *cache_version_string(void)
{
    return g_strdup_printf("%s|%s", xemu_version, MTL_DISK_CACHE_ABI_TAG);
}

/* -------- path helpers -------- */

static char *bin_dir_for_hash(uint64_t hash)
{
    return g_strdup_printf("%s/%04" PRIx64,
                           s_root_dir, (uint64_t)((hash >> 48) & 0xFFFF));
}

static char *bin_path_for_hash(const char *bin_dir, uint64_t hash)
{
    uint64_t lo = hash & (((uint64_t)1 << 48) - 1);
    return g_strdup_printf("%s/%012" PRIx64 ".msl", bin_dir, lo);
}

/* -------- init / finalize -------- */

bool pgraph_mtl_disk_cache_init(void)
{
    if (s_init) {
        return true;
    }

    /* Latch the env-var setting. If disabled, leave the module in the
     * "init returned but cache disabled" state — load/save become
     * no-ops; the renderer never has to special-case the env var. */
    s_enabled = env_cache_enabled();

    /* Build the feature-set fingerprint string from the heap module's
     * latched detection. heap_init must have run before us — the call
     * order is enforced by renderer.c (heap_init → ... → shaders_init →
     * disk_cache_init). If the heap couldn't probe a family (Intel Mac
     * or device not up), we use family 0; a future Apple Silicon run
     * with a real family will mismatch and unlink the file. */
    uint32_t family = pgraph_mtl_heap_apple_gpu_family();
    uint32_t macver = pgraph_mtl_heap_macos_version();
    int macmaj = (int)((macver >> 16) & 0xFFFF);
    int macmin = (int)(macver & 0xFFFF);
    int n = snprintf(s_feature_set, sizeof(s_feature_set),
                     "AppleGPUFamily%u/macOS%d.%d",
                     family, macmaj, macmin);
    if (n <= 0 || (size_t)n >= sizeof(s_feature_set)) {
        fprintf(stderr,
                "pgraph_mtl_disk_cache_init: feature-set string too "
                "long; disabling cache\n");
        s_enabled = false;
        s_init = true;
        return true;  /* Soft-disable. */
    }
    s_feature_set_len = (size_t)n + 1;  /* Includes terminating NUL. */

    if (!s_enabled) {
        s_init = true;
        return true;
    }

    const char *base = xemu_settings_get_base_path();
    if (base == NULL || base[0] == '\0') {
        fprintf(stderr,
                "pgraph_mtl_disk_cache_init: no base path; disabling "
                "cache\n");
        s_enabled = false;
        s_init = true;
        return true;
    }

    /* xemu_settings_get_base_path() returns a string already ending in
     * the platform separator on most paths, but not always (portable
     * mode uses SDL_GetBasePath). Handle both. */
    size_t base_len = strlen(base);
    bool   trailing = base_len > 0 && (base[base_len - 1] == '/' ||
                                       base[base_len - 1] == '\\');
    s_root_dir = trailing
                     ? g_strdup_printf("%smetal_shaders", base)
                     : g_strdup_printf("%s/metal_shaders", base);
    if (s_root_dir == NULL) {
        s_enabled = false;
        s_init = true;
        return true;
    }
    if (qemu_mkdir(s_root_dir) < 0 && errno != EEXIST) {
        fprintf(stderr,
                "pgraph_mtl_disk_cache_init: mkdir %s failed (%s); "
                "disabling cache\n",
                s_root_dir, strerror(errno));
        g_free(s_root_dir);
        s_root_dir = NULL;
        s_enabled = false;
        s_init = true;
        return true;
    }
    s_lru_path = g_strdup_printf("%s/metal_shader_cache_list", s_root_dir);

    qemu_mutex_init(&s_writer_lock);
    qemu_cond_init(&s_writer_cond);
    s_writer_lock_inited = true;

    fprintf(stderr,
            "pgraph_mtl_disk_cache_init: enabled root=%s "
            "feature_set=%s\n",
            s_root_dir, s_feature_set);

    s_init = true;
    return true;
}

void pgraph_mtl_disk_cache_finalize(void)
{
    if (!s_init) {
        return;
    }

    /* Disable new saves before draining; gate the env-var test. */
    s_enabled = false;

    /* Wait for any in-flight writer threads. Writers run detached and
     * decrement s_active_writers on exit, signaling s_writer_cond. */
    if (s_writer_lock_inited) {
        qemu_mutex_lock(&s_writer_lock);
        while (s_active_writers > 0) {
            qemu_cond_wait(&s_writer_cond, &s_writer_lock);
        }
        qemu_mutex_unlock(&s_writer_lock);
        qemu_cond_destroy(&s_writer_cond);
        qemu_mutex_destroy(&s_writer_lock);
        s_writer_lock_inited = false;
    }

    g_free(s_root_dir);
    g_free(s_lru_path);
    s_root_dir = NULL;
    s_lru_path = NULL;
    s_enabled = false;
    s_init = false;
    s_feature_set[0] = '\0';
    s_feature_set_len = 0;
}

bool pgraph_mtl_disk_cache_enabled(void)
{
    return s_init && s_enabled;
}

/* -------- load -------- */

char *pgraph_mtl_disk_cache_load_msl(const struct PgraphMtlPipelineKey *key)
{
    atomic_fetch_add(&s_loads, 1);

    if (!s_init || !s_enabled || key == NULL) {
        atomic_fetch_add(&s_misses, 1);
        return NULL;
    }

    uint64_t hash = fast_hash((const uint8_t *)key,
                              sizeof(PgraphMtlPipelineKey));

    char *bin_dir   = bin_dir_for_hash(hash);
    char *file_path = bin_path_for_hash(bin_dir, hash);
    g_free(bin_dir);

    FILE *f = qemu_fopen(file_path, "rb");
    if (f == NULL) {
        g_free(file_path);
        atomic_fetch_add(&s_misses, 1);
        return NULL;
    }

    char *cached_xemu_version = NULL;
    char *cached_feature_set  = NULL;
    char *expected_version    = cache_version_string();
    void *state_blob          = NULL;
    char *msl_source          = NULL;

    uint64_t xv_len = 0;
    uint64_t fs_len = 0;
    uint64_t st_len = 0;
    uint64_t ms_len = 0;

#define READ_OR_FAIL(buf, len) \
    do { \
        if (fread((buf), (len), 1, f) != 1) { \
            goto fail; \
        } \
    } while (0)

    READ_OR_FAIL(&xv_len, sizeof(xv_len));
    /* Bound check. The longest legitimate xemu_version is short (semver
     * + git hash, < 64 bytes); cap the read at 1 MiB so a corrupted
     * file can't make us OOM. */
    if (xv_len == 0 || xv_len > (1ULL << 20)) {
        goto fail;
    }
    cached_xemu_version = g_malloc((size_t)xv_len + 1);
    READ_OR_FAIL(cached_xemu_version, (size_t)xv_len);
    cached_xemu_version[xv_len] = '\0';
    if (expected_version == NULL ||
        strcmp(cached_xemu_version, expected_version) != 0) {
        goto fail;
    }

    READ_OR_FAIL(&fs_len, sizeof(fs_len));
    if (fs_len == 0 || fs_len > (1ULL << 20)) {
        goto fail;
    }
    cached_feature_set = g_malloc((size_t)fs_len + 1);
    READ_OR_FAIL(cached_feature_set, (size_t)fs_len);
    cached_feature_set[fs_len] = '\0';
    if (strcmp(cached_feature_set, s_feature_set) != 0) {
        goto fail;
    }

    READ_OR_FAIL(&st_len, sizeof(st_len));
    if (st_len != sizeof(PgraphMtlPipelineKey)) {
        /* Layout/version mismatch — invalidate. */
        goto fail;
    }
    state_blob = g_malloc((size_t)st_len);
    READ_OR_FAIL(state_blob, (size_t)st_len);
    if (memcmp(state_blob, key, sizeof(PgraphMtlPipelineKey)) != 0) {
        /* Hash collision — extremely unlikely with fast_hash, but
         * possible. Treat as miss; do NOT delete the file (the
         * collision-counterpart entry will be the right one for the
         * other key). */
        fclose(f);
        f = NULL;
        g_free(file_path);
        g_free(cached_xemu_version);
        g_free(cached_feature_set);
        g_free(expected_version);
        g_free(state_blob);
        atomic_fetch_add(&s_misses, 1);
        return NULL;
    }

    READ_OR_FAIL(&ms_len, sizeof(ms_len));
    /* MSL source can legitimately be > 1 MiB for combiner-rich shaders;
     * 16 MiB is a generous upper bound. */
    if (ms_len == 0 || ms_len > (16ULL << 20)) {
        goto fail;
    }
    msl_source = g_malloc((size_t)ms_len + 1);
    READ_OR_FAIL(msl_source, (size_t)ms_len);
    msl_source[ms_len] = '\0';

#undef READ_OR_FAIL

    fclose(f);
    g_free(file_path);
    g_free(cached_xemu_version);
    g_free(cached_feature_set);
    g_free(expected_version);
    g_free(state_blob);

    atomic_fetch_add(&s_hits, 1);
    /* Caller frees with g_free() (matches g_malloc above). */
    return msl_source;

fail:
    if (f) {
        fclose(f);
    }
    /* Header mismatch / corruption: unlink so future runs re-translate
     * cleanly. */
    qemu_unlink(file_path);
    g_free(file_path);
    g_free(cached_xemu_version);
    g_free(cached_feature_set);
    g_free(expected_version);
    g_free(state_blob);
    g_free(msl_source);
    atomic_fetch_add(&s_misses, 1);
    return NULL;
}

/* -------- save (background writer) -------- */

typedef struct WriterCtx {
    uint64_t                 hash;
    PgraphMtlPipelineKey     key;          /* Full POD copy of the key. */
    char                    *msl_source;   /* g_malloc'd, owned by ctx. */
    /* True if this writer was spawned with thread tracking; finalize
     * spins on s_active_writers reaching zero. False for the synchronous
     * inline fallback. */
    bool                     tracked;
} WriterCtx;

static void *writer_thread(void *arg)
{
    WriterCtx *ctx = (WriterCtx *)arg;

    char *bin_dir   = bin_dir_for_hash(ctx->hash);
    char *file_path = bin_path_for_hash(bin_dir, ctx->hash);
    char *cache_version = NULL;
    FILE *f = NULL;

    /* Best-effort mkdir; ignore EEXIST. */
    if (qemu_mkdir(bin_dir) < 0 && errno != EEXIST) {
        fprintf(stderr,
                "pgraph_mtl_disk_cache: mkdir %s failed: %s\n",
                bin_dir, strerror(errno));
        goto cleanup;
    }

    f = qemu_fopen(file_path, "wb");
    if (f == NULL) {
        fprintf(stderr,
                "pgraph_mtl_disk_cache: open %s for write failed: %s\n",
                file_path, strerror(errno));
        goto cleanup;
    }

    cache_version = cache_version_string();
    if (cache_version == NULL) {
        goto cleanup;
    }

    uint64_t xv_len = (uint64_t)(strlen(cache_version) + 1);
    uint64_t fs_len = (uint64_t)s_feature_set_len;
    uint64_t st_len = (uint64_t)sizeof(PgraphMtlPipelineKey);
    uint64_t ms_len = (uint64_t)strlen(ctx->msl_source);

#define WRITE_OR_FAIL(buf, len) \
    do { \
        if (fwrite((buf), (len), 1, f) != 1) { \
            goto write_fail; \
        } \
    } while (0)

    WRITE_OR_FAIL(&xv_len, sizeof(xv_len));
    WRITE_OR_FAIL(cache_version, (size_t)xv_len);

    WRITE_OR_FAIL(&fs_len, sizeof(fs_len));
    WRITE_OR_FAIL(s_feature_set, (size_t)fs_len);

    WRITE_OR_FAIL(&st_len, sizeof(st_len));
    WRITE_OR_FAIL(&ctx->key, (size_t)st_len);

    WRITE_OR_FAIL(&ms_len, sizeof(ms_len));
    WRITE_OR_FAIL(ctx->msl_source, (size_t)ms_len);

#undef WRITE_OR_FAIL

    fclose(f);
    f = NULL;
    g_free(cache_version);
    cache_version = NULL;

    /* Append the hash to the LRU index file. Single-writer at a time
     * via s_writer_lock so concurrent saves don't interleave. */
    if (s_lru_path != NULL && s_writer_lock_inited) {
        qemu_mutex_lock(&s_writer_lock);
        FILE *lru = qemu_fopen(s_lru_path, "ab");
        if (lru != NULL) {
            if (fwrite(&ctx->hash, sizeof(uint64_t), 1, lru) != 1) {
                fprintf(stderr,
                        "pgraph_mtl_disk_cache: LRU index append "
                        "failed for hash %" PRIx64 "\n",
                        ctx->hash);
            }
            fclose(lru);
        }
        qemu_mutex_unlock(&s_writer_lock);
    }

    goto cleanup;

write_fail:
    fprintf(stderr,
            "pgraph_mtl_disk_cache: write to %s failed (%s); removing "
            "partial file\n",
            file_path, strerror(errno));
    fclose(f);
    f = NULL;
    qemu_unlink(file_path);

cleanup:
    if (f != NULL) {
        fclose(f);
    }
    g_free(cache_version);
    g_free(bin_dir);
    g_free(file_path);
    g_free(ctx->msl_source);
    bool tracked = ctx->tracked;
    g_free(ctx);

    if (tracked && s_writer_lock_inited) {
        qemu_mutex_lock(&s_writer_lock);
        if (s_active_writers > 0) {
            s_active_writers--;
        }
        qemu_cond_broadcast(&s_writer_cond);
        qemu_mutex_unlock(&s_writer_lock);
    }
    return NULL;
}

void pgraph_mtl_disk_cache_save_msl(const struct PgraphMtlPipelineKey *key,
                                    const char *msl_source)
{
    if (!s_init || !s_enabled || key == NULL || msl_source == NULL) {
        return;
    }
    if (msl_source[0] == '\0') {
        return;
    }

    WriterCtx *ctx = g_new0(WriterCtx, 1);
    memcpy(&ctx->key, key, sizeof(PgraphMtlPipelineKey));
    ctx->hash       = fast_hash((const uint8_t *)key,
                                sizeof(PgraphMtlPipelineKey));
    ctx->msl_source = g_strdup(msl_source);
    ctx->tracked    = false;

    /* Track the writer so finalize can drain. If we're at the
     * concurrent-writer cap, run synchronously inline — the cache is
     * best-effort and skipping the write is worse than briefly
     * blocking the caller. */
    bool reserved_slot = false;
    if (s_writer_lock_inited) {
        qemu_mutex_lock(&s_writer_lock);
        if (s_active_writers < MTL_DISK_MAX_WRITERS) {
            s_active_writers++;
            reserved_slot = true;
        }
        qemu_mutex_unlock(&s_writer_lock);
    }

    if (!reserved_slot) {
        /* Synchronous fallback (cap exceeded). */
        ctx->tracked = false;
        writer_thread(ctx);
        return;
    }

    ctx->tracked = true;
    QemuThread t;
    char name[24];
    snprintf(name, sizeof(name), "metal-scache-%llx",
             (unsigned long long)ctx->hash);
    qemu_thread_create(&t, name, writer_thread, ctx, QEMU_THREAD_DETACHED);
}
