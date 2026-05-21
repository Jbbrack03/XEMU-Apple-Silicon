/*
 * Geforce NV2A PGRAPH Metal Renderer (slice M2 — surface manager + clear)
 *
 * Apple Silicon performance fork. The Metal renderer's NV2A-side ops
 * delegate to the surface manager (mtl/surface.mm) and heap manager
 * (mtl/heap.mm) for clear-only correctness. Drawing, textures, shader
 * translation, and MSAA are explicit no-ops here; they land in
 * subsequent slices per docs/apple-silicon/metal-renderer-plan.md.
 *
 * The file extension is .c (not .m / .mm) intentionally — meson's
 * per-target c_args (which add -DCOMPILING_PER_TARGET / -DCONFIG_TARGET
 * / -DCONFIG_DEVICES required by nv2a_int.h transitively) propagate to
 * .c compiles but not to .m / .mm compiles in this build setup. This
 * .c file is the only place in the mtl/ subdir that can include
 * nv2a_int.h. ObjC/Metal API usage lives in heap.mm and surface.mm,
 * which take/return opaque uint32 / void* values and never touch
 * NV2AState directly.
 *
 * Selecting display.renderer = METAL with this slice will boot xemu and
 * (per the M2 exit gate) show the cleared color in the window for any
 * game that issues clear_surface but no draws yet — typically the
 * black/blue Xbox boot splash before the first 3D draw call.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "ui/xemu-settings.h"

#include <stdatomic.h>

#include "buffer.h"
#include "draw.h"
#include "format.h"
#include "glsl.h"
#include "heap.h"
#include "index_gen.h"
#include "pipeline.h"
#include "shader_validation.h"
#include "shaders.h"
#include "shaderstate.h"
#include "state.h"
#include "surface.h"
#include "texture.h"
#include "uniform.h"
#include "vertex.h"

#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"

/* Shared eligibility helpers for native_tri_depth / native_quad.
 * Defined in glsl/geom.c; declared via glsl/shaders.h which the GL
 * renderer also uses (gl/renderer.h:36). The Metal renderer uses
 * these so the eligibility rules cannot drift between GL and Metal —
 * same input, same answer. */
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

char *pgraph_mtl_shadergen_vsh(const ShaderState *state);
char *pgraph_mtl_shadergen_psh(const ShaderState *state);

/* M7 env-var flags (latched once at first use). */
static bool s_force_passthrough_cached = false;
static int  s_force_passthrough        = -1;
static bool s_use_translated_cached    = false;
static int  s_use_translated           = -1;
static _Atomic uint64_t s_vsh_diag_lines = 0;

static bool mtl_vsh_diag_enabled(void)
{
    const char *e = getenv("XEMU_METAL_DIAG_VSH");
    return e != NULL && e[0] != '\0' && e[0] != '0';
}

static void mtl_dump_target_shader_once(uint32_t color_target,
                                        const PgraphMtlPipelineKey *key)
{
    static _Atomic uint32_t s_dumped = 0;
    const char *env = getenv("XEMU_METAL_DUMP_TARGET_SHADER");
    if (env == NULL || env[0] == '\0' || key == NULL ||
        g_ascii_strcasecmp(env, "0") == 0) {
        return;
    }

    bool dump_all = g_ascii_strcasecmp(env, "all") == 0;
    if (!dump_all) {
        char *endp = NULL;
        unsigned long target = strtoul(env, &endp, 0);
        if (endp == env || *endp != '\0' || (uint32_t)target != color_target) {
            return;
        }
    }

    uint32_t dump_index = 0;
    if (dump_all) {
        dump_index = atomic_fetch_add(&s_dumped, 1);
        if (dump_index >= 64) {
            return;
        }
    } else {
        uint32_t expected = 0;
        if (!atomic_compare_exchange_strong(&s_dumped, &expected, 1)) {
            return;
        }
    }

    char *vsh = pgraph_mtl_shadergen_vsh(&key->shader_state);
    char *psh = pgraph_mtl_shadergen_psh(&key->shader_state);
    const char *dir = getenv("XEMU_METAL_DUMP_TARGET_SHADER_DIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }

    char path[PATH_MAX];
    if (dump_all) {
        snprintf(path, sizeof(path),
                 "%s/xemu-metal-target-%04u-0x%08x-prim%u.glsl",
                 dir, dump_index, color_target,
                 (unsigned)key->shader_state.geom.primitive_mode);
    } else {
        snprintf(path, sizeof(path), "%s/xemu-metal-target-0x%08x.glsl",
                 dir, color_target);
    }
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "/* color_target=0x%08x */\n", color_target);
        fprintf(f, "/* regs: ");
        for (unsigned int i = 0; i < ARRAY_SIZE(key->regs); i++) {
            fprintf(f, "%s0x%08x", i ? " " : "", key->regs[i]);
        }
        fprintf(f, " */\n\n");
        fprintf(f, "/* VSH */\n%s\n\n/* PSH */\n%s\n",
                vsh ? vsh : "(null)", psh ? psh : "(null)");
        fclose(f);
        fprintf(stderr,
                "xemu-perf: metal_target_shader_dump target=0x%x path=%s\n",
                color_target, path);
    } else {
        fprintf(stderr,
                "xemu-metal: failed to write target shader dump %s\n",
                path);
    }

    g_free(vsh);
    g_free(psh);
}

/* 2026-05-03 magenta-RT diagnostic: per-vram_addr "draw target" table.
 *
 * For each `pgraph_mtl_flush_draw` invocation, after `mtl_bind_current_surfaces`
 * succeeds, we bump a counter keyed by the bound color binding's vram_addr.
 * On every nv2a_profile_log_emit_interval the table is dumped as one
 * `xemu-perf: metal_draw_target vram_addr=0x.. count=N` line per distinct
 * address (capped to MTL_DRAW_TARGET_TABLE_SIZE entries; addresses beyond
 * the cap are accumulated into `s_draw_target_overflow`).
 *
 * This is the decisive measurement for the 2026-05-03 followup-B+C open
 * question: WHICH SurfaceBinding receives the rendered scene? PGR2's
 * METAL_DRAW_COUNT runs at 75k draws/min but the screenshot path shows
 * none of the front buffer (0x32a4000), back buffer (0x3628000), or
 * aux RT (0x2c06000) contain it. This counter answers it directly.
 *
 * Always-on; zero hot-path cost when no flush_draw fires (which is the
 * GL-renderer case). Per-flush_draw cost is one linear scan over up to
 * 32 entries plus an atomic add — negligible vs the actual draw work.
 */
#define MTL_DRAW_TARGET_TABLE_SIZE 32
typedef struct MtlDrawTargetEntry {
    uint32_t vram_addr;
    _Atomic(uint64_t) count;
} MtlDrawTargetEntry;
static MtlDrawTargetEntry s_draw_target_table[MTL_DRAW_TARGET_TABLE_SIZE];
static _Atomic(uint32_t)  s_draw_target_used = 0;
static _Atomic(uint64_t)  s_draw_target_overflow = 0;
static _Atomic(uint64_t)  s_draw_target_zero_addr = 0;
static QemuMutex          s_draw_target_lock;
static bool               s_draw_target_lock_init = false;

static void mtl_draw_target_lock_ensure(void)
{
    /* Initialized lazily on first bump. The flush_draw call site already
     * runs under d->pgraph.lock so two concurrent renderer threads are
     * not possible, but the emit path is called from the profile log
     * timer and may race with a future renderer thread; guard the
     * insert/scan with a small mutex to keep the table coherent. */
    if (!s_draw_target_lock_init) {
        qemu_mutex_init(&s_draw_target_lock);
        s_draw_target_lock_init = true;
    }
}

static void mtl_draw_target_bump(uint32_t vram_addr)
{
    if (vram_addr == 0) {
        atomic_fetch_add(&s_draw_target_zero_addr, 1);
        return;
    }
    mtl_draw_target_lock_ensure();
    qemu_mutex_lock(&s_draw_target_lock);
    uint32_t used = atomic_load(&s_draw_target_used);
    for (uint32_t i = 0; i < used; i++) {
        if (s_draw_target_table[i].vram_addr == vram_addr) {
            atomic_fetch_add(&s_draw_target_table[i].count, 1);
            qemu_mutex_unlock(&s_draw_target_lock);
            return;
        }
    }
    if (used < MTL_DRAW_TARGET_TABLE_SIZE) {
        s_draw_target_table[used].vram_addr = vram_addr;
        atomic_store(&s_draw_target_table[used].count, 1);
        atomic_store(&s_draw_target_used, used + 1);
        qemu_mutex_unlock(&s_draw_target_lock);
        /* One-shot diagnostic line per distinct vram_addr — fires at
         * most MTL_DRAW_TARGET_TABLE_SIZE times per session. Useful for
         * spotting newly appearing draw targets without waiting for
         * the next interval emit. */
        fprintf(stderr,
                "xemu-perf: metal_draw_target_first vram_addr=0x%x slot=%u\n",
                (unsigned)vram_addr, (unsigned)used);
        return;
    }
    atomic_fetch_add(&s_draw_target_overflow, 1);
    qemu_mutex_unlock(&s_draw_target_lock);
}

void pgraph_mtl_draw_target_emit_interval(FILE *out);

void pgraph_mtl_draw_target_emit_interval(FILE *out)
{
    if (out == NULL) {
        return;
    }
    if (!s_draw_target_lock_init) {
        return;
    }
    qemu_mutex_lock(&s_draw_target_lock);
    uint32_t used = atomic_load(&s_draw_target_used);
    uint64_t overflow = atomic_load(&s_draw_target_overflow);
    uint64_t zero_addr = atomic_load(&s_draw_target_zero_addr);
    /* Snapshot + reset so each interval reports its own deltas. The
     * vram_addr slot table is preserved across resets — addresses that
     * keep receiving draws keep their slot — but the count is reset to
     * zero on each emit. */
    for (uint32_t i = 0; i < used; i++) {
        uint64_t c = atomic_exchange(&s_draw_target_table[i].count, 0);
        if (c > 0) {
            fprintf(out,
                    "xemu-perf: metal_draw_target vram_addr=0x%x count=%llu\n",
                    (unsigned)s_draw_target_table[i].vram_addr,
                    (unsigned long long)c);
        }
    }
    if (overflow > 0) {
        atomic_store(&s_draw_target_overflow, 0);
        fprintf(out,
                "xemu-perf: metal_draw_target_overflow count=%llu\n",
                (unsigned long long)overflow);
    }
    if (zero_addr > 0) {
        atomic_store(&s_draw_target_zero_addr, 0);
        fprintf(out,
                "xemu-perf: metal_draw_target_zero count=%llu\n",
                (unsigned long long)zero_addr);
    }
    qemu_mutex_unlock(&s_draw_target_lock);
}

/* M11: effective MSAA sample count (1 = off; 2/4/8 when on). Latched
 * at pgraph_mtl_init from XEMU_METAL_MSAA, clamped against the active
 * device's `[device supportsTextureSampleCount:N]`. Per the M11 plan
 * MSAA is treated as session-fixed: changing the env var requires a
 * restart so the pipeline cache does not balloon with sample-count
 * variants. */
static uint32_t s_metal_msaa_sample_count = 1;

/* Parse XEMU_METAL_MSAA into a sample count. Returns 1 (off) when
 * the var is unset, 0/1, unparseable, or not in {2, 4, 8}. */
static uint32_t parse_metal_msaa_env(uint32_t *out_requested)
{
    if (out_requested) {
        *out_requested = 0;
    }
    const char *value = getenv("XEMU_METAL_MSAA");
    if (!value || !value[0]) {
        return 1;
    }
    char *endp = NULL;
    unsigned long parsed = strtoul(value, &endp, 10);
    if (!endp || *endp != '\0') {
        return 1;
    }
    if (out_requested) {
        *out_requested = (uint32_t)parsed;
    }
    if (parsed == 2 || parsed == 4 || parsed == 8) {
        return (uint32_t)parsed;
    }
    return 1;
}

static bool mtl_force_passthrough(void)
{
    if (!s_force_passthrough_cached) {
        const char *e = getenv("XEMU_METAL_FORCE_PASSTHROUGH");
        s_force_passthrough = (e && e[0] && e[0] != '0') ? 1 : 0;
        s_force_passthrough_cached = true;
    }
    return s_force_passthrough != 0;
}

/* M5.10 (2026-05-03): VRAM-coherent surface download is opt-in via
 * `XEMU_METAL_FRONT_FB_DOWNLOAD={0,1}`. Default OFF — the download
 * infrastructure landed but does not yet bridge PGR2's specific
 * back→front mechanism (still under investigation), and the GPU→VRAM
 * blit + waitUntilCompleted in download_surface_to_vram has measurable
 * cold-boot perf cost (~4× slowdown observed on PGR2 cold-boot).
 * Setting XEMU_METAL_FRONT_FB_DOWNLOAD=1 enables the path for
 * development / bisection. When OFF, all download_dirty_all calls and
 * the cache-binding set_draw_dirty hot-path stores no-op. */
static int  s_front_fb_download = 0;
static bool s_front_fb_download_cached = false;
static bool mtl_front_fb_download_enabled(void)
{
    if (!s_front_fb_download_cached) {
        const char *e = getenv("XEMU_METAL_FRONT_FB_DOWNLOAD");
        s_front_fb_download = (e && e[0] && e[0] != '0') ? 1 : 0;
        s_front_fb_download_cached = true;
    }
    return s_front_fb_download != 0;
}

/* M5.10 experimental fallback (2026-05-03):
 * `XEMU_METAL_FRONT_FB_FALLBACK={0,1}` opts in to publishing the
 * most-recently-bound color RT as the front-fb when the CRTC-pointed
 * surface lookup hits a stale entry. Used to bridge titles whose
 * CRTC-pointed surface is a HUD overlay (~1 draw / frame) while
 * actual scene goes to a back buffer at a different vram_addr (PGR2
 * is the canonical case). NOT correctness-faithful — see surface.mm
 * for the full caveats — but is cheap and enables visual validation
 * for titles that don't have an observable VRAM-mediated swap. */
static int  s_front_fb_fallback = 0;
static bool s_front_fb_fallback_cached = false;
static bool mtl_front_fb_fallback_enabled(void)
{
    if (!s_front_fb_fallback_cached) {
        const char *e = getenv("XEMU_METAL_FRONT_FB_FALLBACK");
        s_front_fb_fallback = (e && e[0] && e[0] != '0') ? 1 : 0;
        s_front_fb_fallback_cached = true;
    }
    return s_front_fb_fallback != 0;
}

/* Tool 1 (2026-05-19): structured per-flip surface-graph dump.
 *
 * Driven by three env vars read once at first flip:
 *   XEMU_METAL_SURFACE_GRAPH_DUMP=/path  (required to activate)
 *   XEMU_METAL_SURFACE_GRAPH_AT_FLIP_STALL=N   (one-shot at Nth flip)
 *   XEMU_METAL_SURFACE_GRAPH_INTERVAL=N        (every-N flips)
 *
 * If neither AT_FLIP_STALL nor INTERVAL is set, defaults to every flip.
 * If both are set, AT_FLIP_STALL wins (fires once at N, then stops).
 *
 * Lifecycle: lazy fopen(..., "a") on first dump. The FILE* persists
 * for the renderer's lifetime; fflush() runs every dump but no fsync
 * (60 flips/sec on tracked titles would dominate the publish window).
 * The OS closes the descriptor on process exit; no explicit cleanup
 * is needed because this is a diagnostic-only path. */
static FILE       *s_surface_graph_file        = NULL;
static uint64_t    s_surface_graph_flip_seq    = 0;
static int         s_surface_graph_mode_cached = 0;
static int         s_surface_graph_at          = 0; /* 0 = unset */
static int         s_surface_graph_interval    = 0; /* 0 = unset (=> every flip) */
static bool        s_surface_graph_fired_one   = false;
static const char *s_surface_graph_path        = NULL;

static int mtl_env_int_or_zero(const char *name)
{
    const char *e = getenv(name);
    if (!e || !e[0]) {
        return 0;
    }
    long v = strtol(e, NULL, 10);
    if (v < 0 || v > INT_MAX) {
        return 0;
    }
    return (int)v;
}

static void mtl_surface_graph_dump_if_enabled(NV2AState *d)
{
    (void)d;
    if (!s_surface_graph_mode_cached) {
        s_surface_graph_path     = getenv("XEMU_METAL_SURFACE_GRAPH_DUMP");
        s_surface_graph_at       = mtl_env_int_or_zero(
                                       "XEMU_METAL_SURFACE_GRAPH_AT_FLIP_STALL");
        s_surface_graph_interval = mtl_env_int_or_zero(
                                       "XEMU_METAL_SURFACE_GRAPH_INTERVAL");
        s_surface_graph_mode_cached = 1;
    }
    if (s_surface_graph_path == NULL || s_surface_graph_path[0] == '\0') {
        return;
    }

    s_surface_graph_flip_seq++;
    uint64_t n = s_surface_graph_flip_seq;
    bool fire = false;
    if (s_surface_graph_at > 0) {
        fire = (!s_surface_graph_fired_one) &&
               (n == (uint64_t)s_surface_graph_at);
    } else if (s_surface_graph_interval > 0) {
        fire = ((n % (uint64_t)s_surface_graph_interval) == 0);
    } else {
        fire = true;
    }
    if (!fire) {
        return;
    }

    if (s_surface_graph_file == NULL) {
        s_surface_graph_file = fopen(s_surface_graph_path, "a");
        if (s_surface_graph_file == NULL) {
            fprintf(stderr,
                    "xemu-perf: metal_surface_graph_dump_open_fail "
                    "path=%s errno=%d\n",
                    s_surface_graph_path, errno);
            /* Poison the cached path so we don't keep retrying. */
            s_surface_graph_path = NULL;
            return;
        }
    }

    pgraph_mtl_surface_dump_graph_jsonl(s_surface_graph_file, "flip_stall", n);
    s_surface_graph_fired_one = true;
}

static void mtl_get_display_dimensions(NV2AState *d,
                                       unsigned int *out_width,
                                       unsigned int *out_height)
{
    PGRAPHState *pg = &d->pgraph;
    unsigned int width = 0, height = 0;

    d->vga.get_resolution(&d->vga, (int *)&width, (int *)&height);
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
        NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }
    pgraph_apply_scaling_factor(pg, &width, &height);

    if (out_width) {
        *out_width = width;
    }
    if (out_height) {
        *out_height = height;
    }
}

static bool mtl_use_translated_pipeline(void)
{
    if (!s_use_translated_cached) {
        const char *e = getenv("XEMU_METAL_TRANSLATED_PIPELINE");
        /* Default OFF for M7: the M5 translator + M5/M6 pipeline cache
         * are wired (state-to-key + lookup), but uniform-buffer marshaling
         * + per-stage texture binding are intentionally deferred. The
         * production default stays on the M3/M4 hand-coded passthrough
         * pipeline so M0–M6 visible behavior is preserved. Setting
         * XEMU_METAL_TRANSLATED_PIPELINE=1 enters the translated path
         * for development / bisection. */
        s_use_translated = (e && e[0] && e[0] != '0') ? 1 : 0;
        s_use_translated_cached = true;
    }
    return s_use_translated != 0;
}

/* M7 counters — exposed via accessors at file end. */
#include <stdatomic.h>
static _Atomic uint64_t s_pipeline_key_built     = 0;
static _Atomic uint64_t s_pipeline_translated_ok = 0;
static _Atomic uint64_t s_pipeline_translated_fb = 0;
static _Atomic uint64_t s_dispatch_us_total      = 0;
static _Atomic uint64_t s_texture_bind_us_total  = 0;
/* M8: counts draws skipped because the translated-pipeline build was
 * still in flight. Independent of s_pipeline_translated_fb (which
 * counts permanent build failures). */
static _Atomic uint64_t s_draws_skipped_pending  = 0;
/* Task #13: counts draws where CPU-side flat-color propagation
 * triggered (FLAT-shaded OP_QUADS / OP_QUAD_STRIP with NV2A's
 * default LAST-vertex provoking convention). Validates that the
 * propagation path actually engages when expected. */
static _Atomic uint64_t s_flat_quad_propagations  = 0;

static inline void mtl_add_elapsed_us(_Atomic uint64_t *counter,
                                      int64_t start_us)
{
    int64_t elapsed = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start_us;
    if (elapsed > 0) {
        atomic_fetch_add(counter, (uint64_t)elapsed);
    }
}

/* M5.9-followup-B (2026-05-03): CPU-write access callback dispatch.
 *
 * The callback runs from the TCG vCPU thread when guest code writes
 * to a watched VRAM range (registered via mem_access_callback_insert).
 * The xemu fork's `MemAccessCallbackFunc` signature is
 *     void (*MemAccessCallbackFunc)(void *opaque, MemoryRegion *mr,
 *                                   hwaddr addr, hwaddr len, bool write);
 * `addr` is a RAM-address (mr->ram_addr + offset) — the same
 * coordinate space we used at registration; the surface manager keys
 * on `vram_addr` which is the VRAM-relative offset. Translation:
 * `vram_addr_offset = addr - memory_region_get_ram_addr(d->vram)`. */
static void mtl_surface_access_callback(void *opaque, MemoryRegion *mr,
                                        hwaddr addr, hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;
    if (d == NULL || d->vram == NULL) {
        return;
    }

    /* M5.9-followup-B+C diagnostic: count callback invocations even
     * when the affected entry is missing/non-overlapping. Helps
     * confirm registration is wired and the TCG vCPU thread is
     * actually delivering events. */
    static _Atomic(uint64_t) s_cb_invocations = 0;
    uint64_t cb_n = atomic_fetch_add(&s_cb_invocations, 1) + 1;
    if (cb_n <= 4) {
        fprintf(stderr,
                "xemu-perf: metal_access_cb cb_n=%llu addr=0x%llx len=%llu "
                "write=%d\n",
                (unsigned long long)cb_n,
                (unsigned long long)addr, (unsigned long long)len,
                (int)write);
    }

    /* Only writes mark surfaces dirty for the upload path. Reads of
     * a draw-dirty surface require a download (deferred to
     * followup-D). */
    if (!write) {
        return;
    }

    /* The callback's `addr` parameter is the OFFSET WITHIN the
     * MemoryRegion — already vram-relative. See physmem.c line 939:
     * `mr_offset = hit_addr - ram_addr_base`. No subtraction
     * needed; pass directly to the cache. */
    if (addr > UINT32_MAX || len > UINT32_MAX) {
        return;
    }

    qemu_mutex_lock(&d->pgraph.lock);
    pgraph_mtl_surface_mark_dirty_overlapping((uint32_t)addr, (uint32_t)len);
    qemu_mutex_unlock(&d->pgraph.lock);
}

/* Track the set of vram_addrs currently armed with an access
 * callback. The callback void* is owned by the surface cache (via
 * register_access_cb_for); this side just orchestrates registration
 * + unregistration calls. */
static void mtl_arm_access_callback(NV2AState *d, uint32_t vram_addr,
                                    uint32_t size)
{
    /* Only TCG-mode supports the per-callback memory-access
     * notification (mem_access_callback_insert is gated on
     * tcg_enabled() in vk/surface.c). KVM/HVF would need
     * memory_region_test_and_clear_dirty polling — deferred. */
    if (!tcg_enabled()) {
        return;
    }
    if (size == 0) {
        return;
    }
    /* M5.9 ensure_color/ensure_depth promote a synthetic vram_addr=0
     * entry when no SET_SURFACE_OFFSET has fired yet. Don't arm a
     * callback at addr=0 — it would watch the entire low VRAM range
     * and fire spuriously for every VGA / boot-time write. The real
     * arm happens once the first DMA-bound surface lands. */
    if (vram_addr == 0) {
        return;
    }
    /* If a callback is already armed for this vram_addr, don't double-
     * register. The surface cache stores the void* on the entry. */
    void *existing = pgraph_mtl_surface_get_metal_texture_at(vram_addr);
    (void)existing; /* presence indicator only */

    /* Pull whatever was previously registered (likely NULL on first
     * arm), and replace it. Mirrors vk's
     * `surface->access_cb = mem_access_callback_insert(...)`. */
    void *prev_cb = NULL;
    pgraph_mtl_surface_unregister_access_cb_for(vram_addr, &prev_cb);
    if (prev_cb != NULL) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0),
                                          (MemAccessCallback *)prev_cb);
    }

    MemAccessCallback *cb = mem_access_callback_insert(
        qemu_get_cpu(0), d->vram, (hwaddr)vram_addr, (hwaddr)size,
        &mtl_surface_access_callback, d);
    pgraph_mtl_surface_register_access_cb_for(vram_addr, cb);

    /* M5.9-followup-B diag: one-shot per arm so we can see what
     * ranges are being watched. Capped to keep logs readable. */
    static _Atomic(uint64_t) s_arms = 0;
    uint64_t n = atomic_fetch_add(&s_arms, 1) + 1;
    if (n <= 8) {
        fprintf(stderr,
                "xemu-perf: metal_arm_cb n=%llu vram_addr=0x%x size=%u "
                "cb=%p tcg=%d\n",
                (unsigned long long)n,
                (unsigned)vram_addr, (unsigned)size,
                cb, (int)tcg_enabled());
    }
}

static void mtl_disarm_all_access_callbacks(NV2AState *d)
{
    if (!tcg_enabled()) {
        return;
    }
    /* Iterate cache addresses; for each, pull the stored cb pointer
     * and tell QEMU to remove it. */
    uint32_t addrs[128];
    unsigned int n = pgraph_mtl_surface_iter_addresses(addrs, 128);
    for (unsigned int i = 0; i < n; i++) {
        void *cb = NULL;
        pgraph_mtl_surface_unregister_access_cb_for(addrs[i], &cb);
        if (cb != NULL) {
            mem_access_callback_remove_by_ref(qemu_get_cpu(0),
                                              (MemAccessCallback *)cb);
        }
    }
}

/* M5.10 (2026-05-03): callback invoked from surface.mm after each
 * GPU→VRAM download. Marks the affected VRAM range dirty for the VGA
 * scan-out client and the NV2A texture cache so subsequent texture
 * binds re-fetch fresh pixels. Mirrors `vk/surface.c::download_surface`
 * lines 485-490. */
static void mtl_after_surface_download(void *opaque, uint32_t vram_addr,
                                       uint32_t byte_size)
{
    NV2AState *d = (NV2AState *)opaque;
    if (d == NULL || d->vram == NULL || byte_size == 0) {
        return;
    }
    memory_region_set_client_dirty(d->vram, (hwaddr)vram_addr,
                                   (hwaddr)byte_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, (hwaddr)vram_addr,
                                   (hwaddr)byte_size,
                                   DIRTY_MEMORY_NV2A_TEX);
}


static void pgraph_mtl_sync(NV2AState *d)
{
    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_mtl_flush(NV2AState *d)
{
    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_mtl_process_pending(NV2AState *d)
{
    if (
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)
        ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_mtl_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_mtl_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_mtl_clear_report_value(NV2AState *d)
{
}

/* Decode the surface dimensions from PGRAPHState. Mirrors
 * vk/surface.c::get_surface_dimensions. */
static void mtl_get_surface_dimensions(PGRAPHState const *pg,
                                       unsigned int *width,
                                       unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1u << pg->surface_shape.log_width;
        *height = 1u << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

/* M5.9: bytes-per-pixel for an NV097 color format. Local copy of the
 * surface.mm helper so we can compute `size = pitch * height` here
 * without crossing the .mm boundary. */
static unsigned int mtl_color_bpp(uint32_t nv097)
{
    switch (nv097) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        return 2;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        return 4;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        return 1;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        return 2;
    default:
        return 4;
    }
}

static unsigned int mtl_zeta_bpp(uint32_t nv097)
{
    switch (nv097) {
    case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
        return 2;
    case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
        return 4;
    default:
        return 4;
    }
}

static void mtl_update_surface_binding_dim(PGRAPHState *pg,
                                           unsigned int width,
                                           unsigned int height)
{
    pg->surface_binding_dim.width = width;
    pg->surface_binding_dim.clip_x = pg->surface_shape.clip_x;
    pg->surface_binding_dim.clip_width = pg->surface_shape.clip_width;
    pg->surface_binding_dim.height = height;
    pg->surface_binding_dim.clip_y = pg->surface_shape.clip_y;
    pg->surface_binding_dim.clip_height = pg->surface_shape.clip_height;
}

/* M5.9: bind the NV2A's currently-configured color/depth surfaces into
 * the per-VRAM cache. This replaces the M2-era pgraph_mtl_surface_ensure_*
 * call sites where we know the vram_addr (cleared paths and draws that
 * have a valid pg->dma_color/_zeta). For paths that do NOT yet know the
 * vram_addr (early boot before SET_SURFACE_OFFSET), fall back to the
 * legacy ensure-by-shape variant.
 *
 * NOTE on dimensions: the MTLTexture is allocated at the SCALED (host
 * surface_scale_factor) dimensions, but the VRAM-side footprint is at
 * the UNSCALED (1×) guest dimensions — guest writes 640×480, the
 * renderer up-samples for display. We pass the scaled (w, h) for the
 * texture allocation and the unscaled (w, h) plus actual guest pitch
 * for the VRAM `size` so we don't read off the end of the guest's
 * surface in upload_vram_to_texture.
 *
 * Returns true if at least one aspect was bound. */
static bool mtl_bind_current_surfaces(NV2AState *d, bool color, bool zeta)
{
    PGRAPHState *pg = &d->pgraph;
    bool bound_any = false;

    unsigned int width = 0, height = 0;
    mtl_get_surface_dimensions(pg, &width, &height);
    pgraph_apply_anti_aliasing_factor(pg, &width, &height);
    if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        width += pg->surface_shape.clip_x;
        height += pg->surface_shape.clip_y;
    }
    /* Width/height here are the GUEST 1× dimensions (post-AA). The
     * texture is allocated at the host-scaled dimensions. */
    unsigned int scaled_w = width, scaled_h = height;
    pgraph_apply_scaling_factor(pg, &scaled_w, &scaled_h);

    if (color && pg->surface_shape.color_format && width > 0 && height > 0) {
        if (pg->dma_color != 0) {
            DMAObject dma = nv_dma_load(d, pg->dma_color);
            if (dma.dma_class == NV_DMA_IN_MEMORY_CLASS) {
                hwaddr vram_addr = dma.address + pg->surface_color.offset;
                unsigned int bpp = mtl_color_bpp(pg->surface_shape.color_format);
                unsigned int pitch = pg->surface_color.pitch;
                if (pitch == 0) {
                    pitch = width * bpp;
                }
                /* M5.9-followup-B+C diagnostic: log distinct color binds
                 * once each so we can see what addresses the guest is
                 * binding (front, back, aux RTs). Capped to avoid
                 * log spam. */
                static uint32_t s_logged_color_addrs[16];
                static unsigned int s_n_logged_color = 0;
                bool already = false;
                for (unsigned int i = 0; i < s_n_logged_color; i++) {
                    if (s_logged_color_addrs[i] == (uint32_t)vram_addr) {
                        already = true;
                        break;
                    }
                }
                if (!already && s_n_logged_color < 16) {
                    s_logged_color_addrs[s_n_logged_color++] =
                        (uint32_t)vram_addr;
                    fprintf(stderr,
                            "xemu-perf: metal_color_bind vram_addr=0x%x "
                            "guest=%ux%u scaled=%ux%u pitch=%u format=%u\n",
                            (unsigned)vram_addr, width, height,
                            scaled_w, scaled_h, pitch,
                            (unsigned)pg->surface_shape.color_format);
                }
                /* `size` is the 1× VRAM-side footprint — caller's
                 * upload_vram_to_texture reads from `vram_ptr +
                 * vram_addr` for this many bytes. */
                uint32_t natural = (uint32_t)(width * bpp);
                uint32_t row     = pitch > natural ? pitch : natural;
                uint32_t size    = (uint32_t)(row * height);
                /* Sanity-clamp against the guest VRAM cap (~64 MB). If
                 * the addr is bogus, fall through to ensure-by-shape. */
                if (vram_addr + size <= 0x4000000) {
                    /* M5.9-followup-C (2026-05-03): pass guest 1× dims
                     * explicitly so the upload path knows the source
                     * sub-rect inside the host-scaled MTLTexture.
                     * d->vram_ptr supplies VRAM upload data so a
                     * freshly cache-allocated entry picks up whatever
                     * the guest already wrote. */
                    if (pgraph_mtl_surface_bind_color_ex(
                            (uint32_t)vram_addr, size,
                            scaled_w, scaled_h,
                            width, height,
                            pitch,
                            pg->surface_shape.color_format,
                            d->vram_ptr)) {
                        uint32_t bound_guest_w = width;
                        uint32_t bound_guest_h = height;
                        uint32_t bound_pitch = pitch;
                        if (pgraph_mtl_surface_get_color_surface_info_at(
                                (uint32_t)vram_addr, NULL, NULL, NULL,
                                &bound_guest_w, &bound_guest_h,
                                &bound_pitch, NULL)) {
                            width = bound_guest_w;
                            height = bound_guest_h;
                            scaled_w = width;
                            scaled_h = height;
                            pgraph_apply_scaling_factor(pg, &scaled_w,
                                                        &scaled_h);
                            pitch = bound_pitch;
                            natural = (uint32_t)(width * bpp);
                            row = pitch > natural ? pitch : natural;
                            size = (uint32_t)(row * height);
                        }
                        /* M5.9-followup-B (2026-05-03): arm a CPU-write
                         * access callback for the surface's VRAM
                         * range so guest CPU writes (e.g. back→front
                         * memcpy) re-mark dirty and trigger upload on
                         * next read. */
                        mtl_arm_access_callback(d, (uint32_t)vram_addr,
                                                size);
                        bound_any = true;
                        mtl_update_surface_binding_dim(pg, width, height);
                    }
                }
            }
        }
        if (!bound_any) {
            pgraph_mtl_surface_ensure_color(scaled_w, scaled_h,
                                            pg->surface_shape.color_format);
            bound_any = true;
            mtl_update_surface_binding_dim(pg, width, height);
        }
    }
    if (zeta && pg->surface_shape.zeta_format && width > 0 && height > 0) {
        bool bound_z = false;
        if (pg->dma_zeta != 0) {
            DMAObject dma = nv_dma_load(d, pg->dma_zeta);
            if (dma.dma_class == NV_DMA_IN_MEMORY_CLASS) {
                hwaddr vram_addr = dma.address + pg->surface_zeta.offset;
                unsigned int bpp = mtl_zeta_bpp(pg->surface_shape.zeta_format);
                unsigned int pitch = pg->surface_zeta.pitch;
                if (pitch == 0) {
                    pitch = width * bpp;
                }
                uint32_t natural = (uint32_t)(width * bpp);
                uint32_t row     = pitch > natural ? pitch : natural;
                uint32_t size    = (uint32_t)(row * height);
                if (vram_addr + size <= 0x4000000) {
                    /* Depth surfaces don't currently upload (the
                     * upload helper skips !is_color), but arming the
                     * callback is still correct so a future depth-
                     * upload path lights up automatically. */
                    if (pgraph_mtl_surface_bind_depth_ex(
                            (uint32_t)vram_addr, size,
                            scaled_w, scaled_h,
                            width, height,
                            pitch,
                            pg->surface_shape.zeta_format,
                            d->vram_ptr)) {
                        mtl_arm_access_callback(d, (uint32_t)vram_addr,
                                                size);
                        bound_z = true;
                        bound_any = true;
                        mtl_update_surface_binding_dim(pg, width, height);
                    }
                }
            }
        }
        if (!bound_z) {
            pgraph_mtl_surface_ensure_depth(scaled_w, scaled_h,
                                            pg->surface_shape.zeta_format);
            bound_any = true;
            mtl_update_surface_binding_dim(pg, width, height);
        }
    }
    return bound_any;
}

static void pgraph_mtl_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;

    /* M5.5+: drain any open coalesced pass before issuing the clear.
     * The clear opens its own render pass with loadAction=Clear which
     * implicitly invalidates whatever was in tile memory; the prior
     * draws must therefore be committed first or they're lost. */
    pgraph_mtl_draw_flush_open_pass();

    bool write_color   = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_depth   = (parameter & NV097_CLEAR_SURFACE_Z);
    bool write_stencil = (parameter & NV097_CLEAR_SURFACE_STENCIL);
    bool write_zeta    = write_depth || write_stencil;

    if (!write_color && !write_zeta) {
        return;
    }

    pg->clearing = true;

    /* M5.9: bind the current NV2A surfaces into the per-VRAM cache.
     * Falls back to ensure-by-shape if the DMA registers are not yet
     * configured. */
    mtl_bind_current_surfaces(d, write_color, write_zeta);

    /* Decode clear color and depth. */
    float rgba[4] = { 0, 0, 0, 1 };
    if (write_color && pg->surface_shape.color_format) {
        pgraph_get_clear_color(pg, rgba);
    }

    float depth = 1.0f;
    int   stencil = 0;
    if (write_zeta && pg->surface_shape.zeta_format) {
        pgraph_get_clear_depth_stencil_value(pg, &depth, &stencil);
    }

    /* M2 ignores the per-channel write mask in NV097_CLEAR_SURFACE_R/G/B/A
     * and the clear-rect scissor (renders the full surface). The exit
     * gate for M2 is "the game's cleared color is visible in the
     * window"; per-channel and per-rect refinement land with M3+ when
     * the render-pass machinery is reused for draws.
     *
     * 2026-05-20 (task #14 Codex finding #1): NV097_CLEAR_SURFACE_Z and
     * _STENCIL are now passed independently. Previously the depth and
     * stencil aspects of a combined depth+stencil format were always
     * cleared together whenever either bit was set, which diverged from
     * gl/draw.c::pgraph_gl_clear_surface (which gates each via the
     * corresponding bit independently). */

    pgraph_mtl_surface_clear(write_color, rgba,
                             write_depth, depth,
                             write_stencil, stencil);

    pg->surface_color.draw_dirty |= write_color;
    pg->surface_zeta.draw_dirty  |= write_zeta;

    /* M5.10 (2026-05-03): a clear is a draw — mark the cache binding
     * draw-dirty so flip_stall's download_dirty_all writes the cleared
     * pixels back to guest VRAM. Without this, subsequent guest reads
     * of the cleared surface (e.g. as a sampled texture in a post-
     * process pass) would see stale pre-clear pixels. Gated on the
     * M5.10 feature flag (default off). */
    if (mtl_front_fb_download_enabled()) {
        if (write_color) pgraph_mtl_surface_set_draw_dirty_color();
        if (write_zeta)  pgraph_mtl_surface_set_draw_dirty_depth();
    }

    pg->clearing = false;
}

static void pgraph_mtl_draw_begin(NV2AState *d)
{
    /* M3: nothing critical to do here; flush_draw owns the actual
     * encode. M5+ will use this hook for shader binding / dirty-state
     * propagation, mirroring vk/draw.c::pgraph_vk_draw_begin. */
    (void)d;
}

static void pgraph_mtl_flush_draw(NV2AState *d);

static void pgraph_mtl_draw_end(NV2AState *d)
{
    /* M5.5: NV2A's renderer-ops contract calls draw_end after each
     * NV097_END to flush whatever batch was accumulated between
     * draw_begin / draw_end. The GL renderer (gl/draw.c:814) calls
     * pgraph_gl_flush_draw(d) here; the Metal renderer mirrors that
     * pattern. Without this hook, the only path into Metal's
     * flush_draw was the rare ARRAY_ELEMENT expansion in
     * pgraph.c::pgraph_expand_draw_arrays, which is why earlier
     * Metal benchmarks showed METAL_DRAW_COUNT == 0 even though the
     * NV2A guest was pushing tens of thousands of draws per second. */
    PGRAPHState *pg = &d->pgraph;

    /* Skip the flush for nop-draws (color / depth / stencil all masked
     * off). Mirrors gl/draw.c:790-812. The check is repeated here
     * (rather than relying on flush_draw) so we get parity counters
     * with the GL path; flush_draw doesn't currently emit a "no-op
     * skipped" line. */
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red   = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue  = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test  = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
        NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    if (!(color_write || depth_test || stencil_test)) {
        return;
    }

    pgraph_mtl_flush_draw(d);
}

static void pgraph_mtl_flip_stall(NV2AState *d)
{
    /* M5.5+: NV2A is signaling end-of-frame. The compositor will
     * pick up the framebuffer surface texture for present; flush
     * the open coalesced pass so the GPU work is committed. */
    pgraph_mtl_draw_flush_open_pass();

    /* M5.9: do the CRTC-aware front-fb publish here. The Metal
     * compositor reads `pgraph_mtl_get_framebuffer_metal_texture()`
     * via a side-channel and never goes through the renderer-ops
     * `get_framebuffer_surface` callback that the GL path uses, so
     * we must publish from a hook that's called per-frame. flip_stall
     * fires once per NV097_FLIP_STALL — exactly once per frame.
     *
     * Mirrors vk/renderer.c:172-205 — look up `d->pcrtc.start +
     * vga_display_params.line_offset` in the per-VRAM cache and
     * publish the resolved MTLTexture. On miss, leave the previous
     * front-fb pointer in place. */
    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);
    hwaddr crtc_addr = d->pcrtc.start + vga_display_params.line_offset;

    /* M5.10 (2026-05-03): download every draw-dirty surface to guest
     * VRAM so the guest's authoritative buffer-swap path (whatever
     * mechanism it is — back→front memcpy, NV2A engine DMA, software
     * post-process pass) can read the latest GPU-rendered pixels. The
     * download path uses MTLBlitCommandEncoder copyFromTexture:toBuffer:
     * + waitUntilCompleted; on Apple Silicon UMA this is a barrier-
     * coherence sync rather than a real memory copy. The post-download
     * VRAM bytes are also dirty-flagged (DIRTY_MEMORY_VGA |
     * DIRTY_MEMORY_NV2A_TEX) via the callback so subsequent texture
     * sampling refetches from VRAM. Mirrors vk's download_dirty pattern
     * (vk/surface.c:524-535).
     *
     * The download is gated on the per-entry draw_dirty bit which is
     * set by `pgraph_mtl_flush_draw` / `clear_surface` after each draw.
     * The first time through the cache is empty of draw_dirty entries
     * (no draws yet) and the call is a cheap walk.
     *
     * Wrapped in the M5.10 feature flag (default off): on PGR2 the
     * download path is not yet sufficient to bridge the back→front gap
     * (the guest's actual buffer-swap mechanism is still under
     * investigation), and the GPU→VRAM blit + waitUntilCompleted has
     * measurable per-flip cost that's not worth eating until the
     * mechanism is identified and the path proven correct. */
    if (mtl_front_fb_download_enabled() && d->vram_ptr != NULL) {
        pgraph_mtl_surface_download_dirty_all(d->vram_ptr,
                                              mtl_after_surface_download, d);
    }

    /* M5.9-followup-C (2026-05-03): if the resolved front-fb surface
     * has dirty_vram set (guest CPU wrote to its VRAM range since the
     * last upload), upload from VRAM into the texture before the
     * compositor samples it. This is the path that handles guest
     * software-renderer / back→front memcpy buffer-swap mechanisms
     * where the guest's CPU writes are the authoritative pixel
     * source. With M5.10's download path landing first, this also
     * picks up any back→front bytes the guest's swap mechanism just
     * wrote (the download made them visible in VRAM, the upload
     * propagates them into the published texture). */
    if (d->vram_ptr != NULL) {
        pgraph_mtl_surface_upload_if_dirty_at((uint32_t)crtc_addr,
                                              d->vram_ptr);
    }

    bool use_front_fb_fallback = mtl_front_fb_fallback_enabled();
    bool published = false;
    if (!use_front_fb_fallback) {
        unsigned int display_w = 0, display_h = 0;
        mtl_get_display_dimensions(d, &display_w, &display_h);
        published = pgraph_mtl_surface_publish_display_front_fb(
            (uint32_t)crtc_addr, display_w, display_h,
            (uint32_t)vga_display_params.line_offset, "crtc-display");
    }

    /* M5.10 experimental fallback (2026-05-03): if the CRTC-pointed
     * surface didn't resolve OR was just published but the title's
     * actual rendered scene goes to a different back buffer (PGR2
     * canonical case), publish the most-recently-bound color RT
     * (s_color_binding). The fallback always publishes, overwriting
     * the CRTC-resolved publish above — last write wins. Gated on the
     * separate XEMU_METAL_FRONT_FB_FALLBACK feature flag so the
     * default (off) preserves the CRTC-strict behavior; opt-in lets
     * users see actual scene content for titles that need it. */
    (void)published;
    if (use_front_fb_fallback) {
        unsigned int display_w = 0, display_h = 0;
        mtl_get_display_dimensions(d, &display_w, &display_h);
        pgraph_mtl_surface_publish_latest_draw_fallback(display_w,
                                                        display_h,
                                                        (uint32_t)crtc_addr);
    }

    /* Tool 1 (2026-05-19): structured per-flip surface-graph dump for
     * the PGR2 multi-RT compositing investigation (M5.12/M17). Runs
     * after both publish paths so the dump sees the FINAL state.
     * Caller holds `pg->lock` per T2; the dump function relies on that
     * to walk s_cache_head safely. */
    mtl_surface_graph_dump_if_enabled(d);
}

/* MTLPrimitiveType values, mirrored from <Metal/MTLRenderCommandEncoder.h>.
 * Defined here because renderer.c cannot include Metal headers
 * (per-target preprocessor flag boundary). */
enum {
    MTL_PRIM_POINT          = 0,
    MTL_PRIM_LINE           = 1,
    MTL_PRIM_LINE_STRIP     = 2,
    MTL_PRIM_TRIANGLE       = 3,
    MTL_PRIM_TRIANGLE_STRIP = 4,
};

/* Translate NV2A primitive_mode to MTLPrimitiveType for the
 * non-indexed (drawPrimitives) path. Returns 0xFFFFFFFF for
 * primitives that need M4 index expansion (caller routes those to
 * mtl_translate_expanded_primitive instead). */
static uint32_t mtl_translate_primitive(uint32_t nv097_primitive)
{
    switch (nv097_primitive) {
    case PRIM_TYPE_POINTS:         return MTL_PRIM_POINT;
    case PRIM_TYPE_LINES:          return MTL_PRIM_LINE;
    case PRIM_TYPE_LINE_STRIP:     return MTL_PRIM_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES:      return MTL_PRIM_TRIANGLE;
    case PRIM_TYPE_TRIANGLE_STRIP: return MTL_PRIM_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
    default:
        return 0xFFFFFFFF;
    }
}

/*
 * Categorize an NV2A primitive_mode for the M4 indexed-draw path.
 * Returns:
 *   - MTL_PRIM_TRIANGLE for triangle expansions (fan, polygon)
 *   - MTL_PRIM_LINE     for line expansions (line_loop)
 *   - MTL_PRIM_TRIANGLE for quad expansions (quads, quad_strip)
 *   - 0xFFFFFFFF otherwise
 *
 * Triangle-list and line-list output topologies are the only ones
 * Metal exposes that the index expansions in mtl/index_gen.c emit.
 */
static uint32_t mtl_translate_expanded_primitive(uint32_t nv097_primitive)
{
    switch (nv097_primitive) {
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
        return MTL_PRIM_TRIANGLE;
    case PRIM_TYPE_LINE_LOOP:
        return MTL_PRIM_LINE;
    default:
        return 0xFFFFFFFF;
    }
}

/* Expand a contiguous vertex range into a uint32 index list
 * appropriate for the NV2A primitive. Returns the number of indices
 * written (0 on failure). The caller owns `out` and must size it via
 * the matching pgraph_mtl_idx_capacity_* helper. */
static unsigned int mtl_expand_indices(uint32_t nv097_primitive,
                                       uint32_t *out, size_t out_capacity,
                                       unsigned int vertex_count)
{
    switch (nv097_primitive) {
    case PRIM_TYPE_TRIANGLE_FAN:
        return (unsigned int)pgraph_mtl_idx_expand_triangle_fan(
            out, out_capacity, vertex_count);
    case PRIM_TYPE_QUADS:
        return (unsigned int)pgraph_mtl_idx_expand_quads(
            out, out_capacity, vertex_count);
    case PRIM_TYPE_QUAD_STRIP:
        return (unsigned int)pgraph_mtl_idx_expand_quad_strip(
            out, out_capacity, vertex_count);
    case PRIM_TYPE_POLYGON:
        return (unsigned int)pgraph_mtl_idx_expand_polygon(
            out, out_capacity, vertex_count);
    case PRIM_TYPE_LINE_LOOP:
        return (unsigned int)pgraph_mtl_idx_expand_line_loop(
            out, out_capacity, vertex_count);
    default:
        return 0;
    }
}

static size_t mtl_expanded_index_capacity(uint32_t nv097_primitive,
                                          unsigned int vertex_count);

static unsigned int mtl_expand_indexed_indices(uint32_t nv097_primitive,
                                               uint32_t *out,
                                               size_t out_capacity,
                                               const uint32_t *indices,
                                               unsigned int index_count)
{
    if (out == NULL || indices == NULL) {
        return 0;
    }

    size_t needed = mtl_expanded_index_capacity(nv097_primitive, index_count);
    if (needed == 0 || out_capacity < needed) {
        return 0;
    }

    switch (nv097_primitive) {
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON: {
        unsigned int triangles = index_count - 2;
        for (unsigned int i = 0; i < triangles; i++) {
            out[i * 3 + 0] = indices[0];
            out[i * 3 + 1] = indices[i + 1];
            out[i * 3 + 2] = indices[i + 2];
        }
        return triangles * 3;
    }
    case PRIM_TYPE_QUADS: {
        unsigned int quads = index_count / 4;
        for (unsigned int i = 0; i < quads; i++) {
            uint32_t a = indices[4 * i + 0];
            uint32_t b = indices[4 * i + 1];
            uint32_t c = indices[4 * i + 2];
            uint32_t d = indices[4 * i + 3];
            out[i * 6 + 0] = b;
            out[i * 6 + 1] = c;
            out[i * 6 + 2] = a;
            out[i * 6 + 3] = c;
            out[i * 6 + 4] = d;
            out[i * 6 + 5] = a;
        }
        return quads * 6;
    }
    case PRIM_TYPE_QUAD_STRIP: {
        unsigned int quads = (index_count - 2) / 2;
        for (unsigned int i = 0; i < quads; i++) {
            uint32_t a = indices[2 * i + 0];
            uint32_t b = indices[2 * i + 1];
            uint32_t c = indices[2 * i + 2];
            uint32_t d = indices[2 * i + 3];
            out[i * 6 + 0] = a;
            out[i * 6 + 1] = b;
            out[i * 6 + 2] = c;
            out[i * 6 + 3] = c;
            out[i * 6 + 4] = b;
            out[i * 6 + 5] = d;
        }
        return quads * 6;
    }
    case PRIM_TYPE_LINE_LOOP: {
        for (unsigned int i = 0; i < index_count - 1; i++) {
            out[i * 2 + 0] = indices[i];
            out[i * 2 + 1] = indices[i + 1];
        }
        out[(index_count - 1) * 2 + 0] = indices[index_count - 1];
        out[(index_count - 1) * 2 + 1] = indices[0];
        return index_count * 2;
    }
    default:
        return 0;
    }
}

static size_t mtl_expanded_index_capacity(uint32_t nv097_primitive,
                                          unsigned int vertex_count)
{
    switch (nv097_primitive) {
    case PRIM_TYPE_TRIANGLE_FAN:
        return pgraph_mtl_idx_capacity_triangle_fan(vertex_count);
    case PRIM_TYPE_QUADS:
        return pgraph_mtl_idx_capacity_quads(vertex_count);
    case PRIM_TYPE_QUAD_STRIP:
        return pgraph_mtl_idx_capacity_quad_strip(vertex_count);
    case PRIM_TYPE_POLYGON:
        return pgraph_mtl_idx_capacity_polygon(vertex_count);
    case PRIM_TYPE_LINE_LOOP:
        return pgraph_mtl_idx_capacity_line_loop(vertex_count);
    default:
        return 0;
    }
}

/*
 * Determine whether the current draw is eligible for the
 * native-tri-depth fragment-shader path. Mirrors
 * gl/draw.c::pgraph_gl_profile_native_tri_depth_draw — same
 * eligibility rules, same env-var gating via
 * pgraph_glsl_native_tri_depth_enabled() — so the per-frame
 * counter splits parallel the GL path exactly.
 */
static bool mtl_native_tri_depth_eligible(PGRAPHState *pg)
{
    enum ShaderPrimitiveMode prim =
        (enum ShaderPrimitiveMode)pg->primitive_mode;
    if (prim != PRIM_TYPE_TRIANGLES &&
        prim != PRIM_TYPE_TRIANGLE_STRIP &&
        prim != PRIM_TYPE_TRIANGLE_FAN) {
        return false;
    }
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    enum ShaderPolygonMode front = (enum ShaderPolygonMode)GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    enum ShaderPolygonMode back = (enum ShaderPolygonMode)GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    return pgraph_glsl_native_tri_depth_enabled() &&
           pgraph_glsl_native_tri_depth_supported(
               prim, front, back, pg->smooth_shading,
               pg->first_vertex_is_provoking);
}

static bool mtl_native_quad_eligible(PGRAPHState *pg)
{
    enum ShaderPrimitiveMode prim =
        (enum ShaderPrimitiveMode)pg->primitive_mode;
    if (prim != PRIM_TYPE_QUADS && prim != PRIM_TYPE_QUAD_STRIP) {
        return false;
    }
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    enum ShaderPolygonMode front = (enum ShaderPolygonMode)GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    enum ShaderPolygonMode back = (enum ShaderPolygonMode)GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    return pgraph_glsl_native_quad_enabled() &&
           pgraph_glsl_native_quad_supported(prim, front, back,
                                             pg->smooth_shading);
}

/* M5.8 removed mtl_inline_buffer_attrs — the inline_buffer path now
 * shares mtl_dispatch_decoded_draw's streams-based interface, with
 * borrowed `attr->inline_buffer` pointers. Synthesized DIFFUSE for the
 * M3/M4 fallback is handled inside mtl_dispatch_decoded_draw. */

/* M5.5 / M5.8: dispatch the decoded vertex / index buffers through the
 * eligibility / translated-pipeline / passthrough machinery. Shared
 * by every flush_draw branch (inline_buffer / inline_elements /
 * draw_arrays / inline_array). The caller owns the streams array and
 * indices; this helper does not free them.
 *
 * Required: streams[NV2A_VERTEX_ATTR_POSITION].data must be non-NULL
 * (no draw without per-vertex positions). Slots whose `data == NULL`
 * are treated as uniform attributes — pg->uniform_attrs is set
 * accordingly so the GLSL generator emits `inlineValue[k]` for them.
 *
 * `indices` is non-NULL when the caller has already built an index
 * stream (inline_elements, or expanded triangle_fan / quads). For
 * non-indexed Metal-native primitives, pass indices=NULL, icount=0
 * and the helper will use drawPrimitives. */
static void mtl_dispatch_decoded_draw(NV2AState *d,
                                      void *color_tex, void *depth_tex,
                                      uint32_t color_fmt, uint32_t depth_fmt,
                                      uint32_t vp_w, uint32_t vp_h,
                                      uint32_t draw_target_vram_addr,
                                      const MtlAttributeStream *streams,
                                      unsigned int vcount,
                                      const uint32_t *indices,
                                      unsigned int icount,
                                      bool native_tri, bool native_quad)
{
    PGRAPHState *pg = &d->pgraph;
    if (vcount == 0 || streams == NULL) {
        return;
    }
    /* M3/M4 hand-coded passthrough path requires per-vertex POSITION
     * (slot 0) and DIFFUSE (slot 3) — synthesized DIFFUSE if the
     * decoder didn't supply it. The translated path likewise needs
     * POSITION at minimum. */
    if (streams[NV2A_VERTEX_ATTR_POSITION].data == NULL) {
        return;
    }
    int64_t dispatch_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    /* Task #13: CPU-side flat-color propagation for FLAT-shaded
     * OP_QUADS / OP_QUAD_STRIP. Apple Silicon Metal has no
     * geometry-shader stage, and the native_quad fast-path explicitly
     * rejects flat-shaded quads (glsl/geom.c:181-188) because the
     * A-C diagonal cannot put vertex 3 first in both emitted
     * triangles -- and crucially vertex 3 is only present in ONE
     * of the two emitted triangles, so even reordering can't make
     * Metal's [[flat]] qualifier (first-vertex convention) produce
     * the NV2A LAST-vertex-provoking flat color for both triangles.
     *
     * Approach: replicate vertex 3's DIFFUSE / SPECULAR /
     * BACK_DIFFUSE / BACK_SPECULAR across vertices 0/1/2 in each
     * quad. The rasterizer then sees uniform color across each
     * triangle under smooth interpolation -- functionally equivalent
     * to NV2A's flat shading with vertex 3 provoking.
     *
     * After propagation we temporarily set pg->smooth_shading=true
     * so the GLSL native_quad_supported check accepts the path
     * (no geometry shader needed). The saved value is restored
     * after the encode so cross-renderer state stays consistent.
     */
    bool   saved_smooth_shading = pg->smooth_shading;
    bool   did_flat_quad_propagation = false;
    /* QUAD_STRIP excluded -- vertex sharing makes single-pass CPU
     * propagation incorrect (Codex 2026-05-20 review). See
     * pgraph_mtl_propagate_flat_quad_colors for the long form. */
    if (!pg->smooth_shading && !pg->first_vertex_is_provoking &&
        pg->primitive_mode == PRIM_TYPE_QUADS) {
        did_flat_quad_propagation =
            pgraph_mtl_propagate_flat_quad_colors(pg,
                (MtlAttributeStream *)streams, vcount);
        if (did_flat_quad_propagation) {
            pg->smooth_shading = true;
            /* Recompute native_quad eligibility now that we have
             * effectively-smooth color streams. native_tri eligibility
             * is unaffected (we only changed shading-mode for quads). */
            native_quad = mtl_native_quad_eligible(pg);
            atomic_fetch_add(&s_flat_quad_propagations, 1);
        }
    }

    uint32_t variant = (native_tri || native_quad)
                           ? MTL_DRAW_VARIANT_NATIVE_DEPTH
                           : MTL_DRAW_VARIANT_PASSTHROUGH;

    /* M5.8: classify each NV2A attribute slot as streaming
     * (per-vertex) or uniform (single-value via VSH UBO inlineValue[]).
     * Set BEFORE building the pipeline key so
     * pgraph_glsl_get_shader_state(pg) captures the right
     * uniform_attrs into the cached ShaderState — the cache key memcmp
     * + the GLSL generator must agree. The previous values are saved
     * here and restored after the encode so cross-renderer state stays
     * consistent. See mtl/vertex.c:pgraph_mtl_set_attr_masks. */
    uint16_t saved_uniform = 0, saved_compressed = 0, saved_swizzle = 0;
    pgraph_mtl_set_attr_masks(pg, &saved_uniform, &saved_compressed,
                              &saved_swizzle);

    /* Translated-pipeline lookup (shared with the original
     * inline_buffer flow). */
    void *translated_pipeline = NULL;
    bool  translated_pending  = false;
    PgraphMtlPipelineKey translated_key;
    bool have_translated_key = false;
    if (!mtl_force_passthrough()) {
        for (int t = 0; t < NV2A_MAX_TEXTURES; t++) {
            int64_t bind_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            (void)pgraph_mtl_texture_bind_from_pg(pg, t);
            mtl_add_elapsed_us(&s_texture_bind_us_total, bind_start_us);
        }
        PgraphMtlPipelineKey key;
        if (pgraph_mtl_build_pipeline_key(d, color_fmt, depth_fmt,
                                          s_metal_msaa_sample_count, &key)) {
            translated_key = key;
            have_translated_key = true;
            mtl_dump_target_shader_once(
                draw_target_vram_addr, &key);
            atomic_fetch_add(&s_pipeline_key_built, 1);
            PgraphMtlPipelineLookupState st;
            void *ps = pgraph_mtl_shaders_get_pipeline_ex(&key, &st);
            if (st == PGRAPH_MTL_PIPELINE_READY && ps != NULL) {
                atomic_fetch_add(&s_pipeline_translated_ok, 1);
                translated_pipeline = ps;
            } else if (st == PGRAPH_MTL_PIPELINE_PENDING) {
                translated_pending = true;
            } else {
                atomic_fetch_add(&s_pipeline_translated_fb, 1);
            }
        }
    }

    if (translated_pending && mtl_use_translated_pipeline() &&
        !mtl_force_passthrough()) {
        atomic_fetch_add(&s_draws_skipped_pending, 1);
        pgraph_mtl_restore_attr_masks(pg, saved_uniform, saved_compressed,
                                      saved_swizzle);
        if (did_flat_quad_propagation) {
            pg->smooth_shading = saved_smooth_shading;
        }
        mtl_add_elapsed_us(&s_dispatch_us_total, dispatch_start_us);
        return;
    }

    bool use_translated_path =
        translated_pipeline != NULL && mtl_use_translated_pipeline() &&
        !mtl_force_passthrough();

    bool drew = false;

    /* M3/M4 fallback path needs raw position+color float arrays. The
     * passthrough fragment shader hardcodes `[[attribute(3)]]` for
     * color, so synthesize an inline-value-filled stream when the
     * decoder didn't produce one (matches the inline_buffer flow's
     * mtl_inline_buffer_attrs synthesis). */
    const float *positions = streams[NV2A_VERTEX_ATTR_POSITION].data;
    const float *colors    = streams[NV2A_VERTEX_ATTR_DIFFUSE].data;
    float       *synth_colors = NULL;
    if (!use_translated_path && colors == NULL) {
        synth_colors = g_malloc_n(vcount * 4, sizeof(float));
        VertexAttribute *col_attr =
            &pg->vertex_attributes[NV2A_VERTEX_ATTR_DIFFUSE];
        for (unsigned int i = 0; i < vcount; i++) {
            synth_colors[i * 4 + 0] = col_attr->inline_value[0];
            synth_colors[i * 4 + 1] = col_attr->inline_value[1];
            synth_colors[i * 4 + 2] = col_attr->inline_value[2];
            synth_colors[i * 4 + 3] = col_attr->inline_value[3];
        }
        colors = synth_colors;
    }

    if (use_translated_path) {
        ShaderState ss = pgraph_glsl_get_shader_state(pg);
        if (mtl_vsh_diag_enabled() &&
            atomic_fetch_add(&s_vsh_diag_lines, 1) < 96) {
            const float *p0 = streams[NV2A_VERTEX_ATTR_POSITION].data;
            uint32_t mode = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                                     NV_PGRAPH_CSV0_D_MODE);
            fprintf(stderr,
                    "xemu-perf: metal_vsh_diag prim=%u vcount=%u icount=%u "
                    "uniform=0x%04x compressed=0x%04x swizzle=0x%04x "
                    "mode=%u fixed=%d prog_len=%d surface_dim=%ux%u "
                    "binding_dim=%ux%u scale=%u v0={%.6g,%.6g,%.6g,%.6g}\n",
                    (unsigned)pg->primitive_mode, vcount, icount,
                    ss.vsh.uniform_attrs, ss.vsh.compressed_attrs,
                    ss.vsh.swizzle_attrs, mode, ss.vsh.is_fixed_function,
                    ss.vsh.programmable.program_length,
                    pg->surface_shape.clip_width,
                    pg->surface_shape.clip_height,
                    pg->surface_binding_dim.width,
                    pg->surface_binding_dim.height,
                    pg->surface_scale_factor,
                    p0 ? p0[0] : 0.0f, p0 ? p0[1] : 0.0f,
                    p0 ? p0[2] : 0.0f, p0 ? p0[3] : 0.0f);
        }
        pgraph_mtl_uniform_begin_frame();

        void *vsh_ubo = NULL, *psh_ubo = NULL;
        size_t vsh_off = 0, psh_off = 0;
        size_t vsh_size =
            pgraph_mtl_uniform_stage_vsh(pg, &ss.vsh, &vsh_ubo, &vsh_off);
        size_t psh_size =
            pgraph_mtl_uniform_stage_psh(pg, &ss.psh, &psh_ubo, &psh_off);

        void *stage_tex[4] = { NULL, NULL, NULL, NULL };
        void *stage_smp[4] = { NULL, NULL, NULL, NULL };
        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
        uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
        uint32_t control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
        uint32_t setup_raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
        unsigned int scissor_x = pg->surface_shape.clip_x;
        unsigned int scissor_y = pg->surface_shape.clip_y;
        unsigned int scissor_w = pg->surface_shape.clip_width;
        unsigned int scissor_h = pg->surface_shape.clip_height;
        pgraph_apply_anti_aliasing_factor(pg, &scissor_x, &scissor_y);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_w, &scissor_h);
        pgraph_apply_scaling_factor(pg, &scissor_x, &scissor_y);
        pgraph_apply_scaling_factor(pg, &scissor_w, &scissor_h);
        void *default_smp = pgraph_mtl_texture_get_default_sampler();
        for (int t = 0; t < NV2A_MAX_TEXTURES; t++) {
            stage_tex[t] = pgraph_mtl_texture_get_metal_texture(t);
            stage_smp[t] = pgraph_mtl_texture_get_sampler_state(t);
            if (stage_smp[t] == NULL) {
                stage_smp[t] = default_smp;
            }
        }
        if (have_translated_key) {
            mtl_dump_target_shader_once(draw_target_vram_addr,
                                        &translated_key);
        }

        uint32_t mtl_prim = mtl_translate_primitive(pg->primitive_mode);
        if (indices != NULL && icount > 0) {
            uint32_t prim = mtl_prim;
            if (prim != 0xFFFFFFFF) {
                pgraph_mtl_draw_translated(translated_pipeline,
                                           streams,
                                           MTL_VERTEX_NUM_ATTRIBUTES,
                                           vcount,
                                           indices, icount,
                                           prim, vp_w, vp_h,
                                           color_tex, depth_tex,
                                           depth_fmt,
                                           blend_color,
                                           control_0, control_1, control_2,
                                           setup_raster,
                                           scissor_x, scissor_y,
                                           scissor_w, scissor_h,
                                           vsh_ubo, vsh_off, vsh_size,
                                           psh_ubo, psh_off, psh_size,
                                           stage_tex, stage_smp);
                drew = true;
            } else {
                prim = mtl_translate_expanded_primitive(pg->primitive_mode);
                size_t cap = mtl_expanded_index_capacity(pg->primitive_mode,
                                                          icount);
                if (prim != 0xFFFFFFFF && cap > 0) {
                    uint32_t *exp_idx = g_malloc_n(cap, sizeof(uint32_t));
                    unsigned int eicount =
                        mtl_expand_indexed_indices(pg->primitive_mode,
                                                   exp_idx, cap,
                                                   indices, icount);
                    if (eicount > 0) {
                        pgraph_mtl_draw_translated(translated_pipeline,
                                                   streams,
                                                   MTL_VERTEX_NUM_ATTRIBUTES,
                                                   vcount,
                                                   exp_idx, eicount,
                                                   prim, vp_w, vp_h,
                                                   color_tex, depth_tex,
                                                   depth_fmt,
                                                   blend_color,
                                                   control_0, control_1,
                                                   control_2, setup_raster,
                                                   scissor_x, scissor_y,
                                                   scissor_w, scissor_h,
                                                   vsh_ubo, vsh_off, vsh_size,
                                                   psh_ubo, psh_off, psh_size,
                                                   stage_tex, stage_smp);
                        drew = true;
                    }
                    g_free(exp_idx);
                }
            }
        } else if (mtl_prim != 0xFFFFFFFF) {
            pgraph_mtl_draw_translated(translated_pipeline,
                                       streams,
                                       MTL_VERTEX_NUM_ATTRIBUTES,
                                       vcount,
                                       NULL, 0,
                                       mtl_prim,
                                       vp_w, vp_h, color_tex, depth_tex,
                                       depth_fmt,
                                       blend_color,
                                       control_0, control_1, control_2,
                                       setup_raster,
                                       scissor_x, scissor_y,
                                       scissor_w, scissor_h,
                                       vsh_ubo, vsh_off, vsh_size,
                                       psh_ubo, psh_off, psh_size,
                                       stage_tex, stage_smp);
            drew = true;
        } else {
            uint32_t expanded_prim =
                mtl_translate_expanded_primitive(pg->primitive_mode);
            if (expanded_prim != 0xFFFFFFFF) {
                size_t cap = mtl_expanded_index_capacity(pg->primitive_mode,
                                                          vcount);
                if (cap > 0) {
                    uint32_t *exp_idx = g_malloc_n(cap, sizeof(uint32_t));
                    unsigned int eicount =
                        mtl_expand_indices(pg->primitive_mode, exp_idx, cap,
                                           vcount);
                    if (eicount > 0) {
                        pgraph_mtl_draw_translated(translated_pipeline,
                                                   streams,
                                                   MTL_VERTEX_NUM_ATTRIBUTES,
                                                   vcount,
                                                   exp_idx, eicount,
                                                   expanded_prim, vp_w, vp_h,
                                                   color_tex, depth_tex,
                                                   depth_fmt,
                                                   blend_color,
                                                   control_0, control_1,
                                                   control_2, setup_raster,
                                                   scissor_x, scissor_y,
                                                   scissor_w, scissor_h,
                                                   vsh_ubo, vsh_off, vsh_size,
                                                   psh_ubo, psh_off, psh_size,
                                                   stage_tex, stage_smp);
                        drew = true;
                    }
                    g_free(exp_idx);
                }
            }
        }

        pgraph_mtl_uniform_end_frame();
    } else {
        if (mtl_use_translated_pipeline() && translated_pipeline == NULL &&
            !mtl_force_passthrough()) {
            pgraph_mtl_draw_inc_pipeline_fallback_count();
        }

        uint32_t mtl_prim = mtl_translate_primitive(pg->primitive_mode);
        if (indices != NULL && icount > 0) {
            uint32_t prim = mtl_prim;
            if (prim != 0xFFFFFFFF) {
                pgraph_mtl_draw_indexed(positions, colors, vcount,
                                        indices, icount,
                                        prim, variant,
                                        vp_w, vp_h, color_tex, depth_tex,
                                        color_fmt, depth_fmt);
                drew = true;
            } else {
                prim = mtl_translate_expanded_primitive(pg->primitive_mode);
                size_t cap = mtl_expanded_index_capacity(pg->primitive_mode,
                                                          icount);
                if (prim != 0xFFFFFFFF && cap > 0) {
                    uint32_t *exp_idx = g_malloc_n(cap, sizeof(uint32_t));
                    unsigned int eicount =
                        mtl_expand_indexed_indices(pg->primitive_mode,
                                                   exp_idx, cap,
                                                   indices, icount);
                    if (eicount > 0) {
                        pgraph_mtl_draw_indexed(positions, colors, vcount,
                                                exp_idx, eicount,
                                                prim, variant,
                                                vp_w, vp_h,
                                                color_tex, depth_tex,
                                                color_fmt, depth_fmt);
                        drew = true;
                    }
                    g_free(exp_idx);
                }
            }
        } else if (mtl_prim != 0xFFFFFFFF) {
            pgraph_mtl_draw_passthrough(positions, colors, vcount, mtl_prim,
                                        variant, vp_w, vp_h,
                                        color_tex, depth_tex,
                                        color_fmt, depth_fmt);
            drew = true;
        } else {
            uint32_t expanded_prim =
                mtl_translate_expanded_primitive(pg->primitive_mode);
            if (expanded_prim != 0xFFFFFFFF) {
                size_t cap =
                    mtl_expanded_index_capacity(pg->primitive_mode, vcount);
                if (cap > 0) {
                    uint32_t *exp_idx = g_malloc_n(cap, sizeof(uint32_t));
                    unsigned int eicount =
                        mtl_expand_indices(pg->primitive_mode, exp_idx, cap,
                                           vcount);
                    if (eicount > 0) {
                        pgraph_mtl_draw_indexed(positions, colors, vcount,
                                                exp_idx, eicount,
                                                expanded_prim, variant,
                                                vp_w, vp_h,
                                                color_tex, depth_tex,
                                                color_fmt, depth_fmt);
                        drew = true;
                    }
                    g_free(exp_idx);
                }
            }
        }
    }

    if (synth_colors) {
        g_free(synth_colors);
    }

    if (drew) {
        if (native_tri) {
            nv2a_profile_inc_counter(NV2A_PROF_NATIVE_TRI_DEPTH_DRAW);
            pgraph_mtl_draw_inc_native_tri_depth_count();
            /* Per-mode breakdown mirrors gl/draw.c:422-428. Lets a
             * test (the native-quad-tri-depth XBE) assert that the
             * specific provoking-vertex path engaged, not just the
             * aggregate. Eligibility requires either smooth_shading
             * OR (flat AND first_vertex_is_provoking) per
             * glsl/geom.c:156, so the second branch always covers
             * the flat case for an eligible native-tri draw. */
            if (pg->smooth_shading) {
                nv2a_profile_inc_counter(
                    NV2A_PROF_NATIVE_TRI_DEPTH_DRAW_SMOOTH);
            } else if (pg->first_vertex_is_provoking) {
                nv2a_profile_inc_counter(
                    NV2A_PROF_NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST);
            }
        }
        if (native_quad) {
            nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW);
            pgraph_mtl_draw_inc_native_quad_count();
            if (pg->primitive_mode == PRIM_TYPE_QUADS) {
                nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW_LIST);
            } else if (pg->primitive_mode == PRIM_TYPE_QUAD_STRIP) {
                nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW_STRIP);
            }
        }
        if (color_tex) pg->surface_color.draw_dirty = true;
        if (depth_tex) pg->surface_zeta.draw_dirty = true;
        /* M5.10 (2026-05-03): also mark the per-VRAM cache binding
         * draw-dirty. This is required not only by the optional
         * front-framebuffer download path, but by texture binds that
         * overlap a render target and must read back the latest GPU
         * contents before CPU-side texture decode. Mirrors
         * vk/draw.c:1830-1860::pgraph_vk_set_surface_dirty. */
        if (color_tex) pgraph_mtl_surface_set_draw_dirty_color();
        if (depth_tex) pgraph_mtl_surface_set_draw_dirty_depth();
    }

    /* M5.8: restore previous masks. The encode-time use of
     * uniform_attrs is fully captured: state.c read it via
     * pgraph_glsl_get_shader_state during pipeline-key build, the
     * cache lookup baked it into the entry, and the staged VSH UBO
     * already carries the inline_value bytes for every uniform-marked
     * attribute. */
    pgraph_mtl_restore_attr_masks(pg, saved_uniform, saved_compressed,
                                  saved_swizzle);
    if (did_flat_quad_propagation) {
        pg->smooth_shading = saved_smooth_shading;
    }
    mtl_add_elapsed_us(&s_dispatch_us_total, dispatch_start_us);
}

static void pgraph_mtl_flush_draw_inner(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    if (pg->inline_buffer_length == 0 &&
        pg->inline_elements_length == 0 &&
        pg->draw_arrays_length == 0 &&
        pg->inline_array_length == 0) {
        return;
    }


    /* M5.9: bind the current NV2A surfaces into the per-VRAM cache.
     * On Metal-renderer first-draw the cache lookup is a miss so we
     * allocate; subsequent draws against the same RT hit the cache and
     * the existing texture is reused. */
    mtl_bind_current_surfaces(d, pg->surface_shape.color_format != 0,
                              pg->surface_shape.zeta_format != 0);

    /* 2026-05-03 magenta-RT diagnostic: bump the per-vram_addr draw-target
     * counter for the bound color binding. Reads vram_addr off the
     * cached SurfaceBinding directly so we measure the actual draw
     * target after any cache-resolution logic, not the raw DMA-derived
     * address (the two should match in steady state but the counter
     * also catches ensure-by-shape fallbacks where vram_addr=0). */
    uint32_t draw_target_vram_addr = pgraph_mtl_surface_get_color_vram_addr();
    mtl_draw_target_bump(draw_target_vram_addr);

    void *color_tex = pgraph_mtl_surface_get_color_texture();
    void *depth_tex = pgraph_mtl_surface_get_depth_texture();
    uint32_t color_fmt = pgraph_mtl_surface_get_color_format();
    uint32_t depth_fmt = pgraph_mtl_surface_get_depth_format();
    uint32_t vp_w      = pgraph_mtl_surface_get_width();
    uint32_t vp_h      = pgraph_mtl_surface_get_height();

    if (color_tex == NULL && depth_tex == NULL) {
        return;
    }
    /* Upload only the bound draw target before rendering. Uploading every
     * dirty cached surface here makes unrelated CPU-written front buffers
     * pay a full scaled upload on each draw; texture consumers upload their
     * own compatible surface on demand in texture_pg.c. */
    if (d->vram_ptr != NULL && draw_target_vram_addr != 0) {
        pgraph_mtl_surface_upload_if_dirty_at(draw_target_vram_addr,
                                              d->vram_ptr);
    }
    if (color_tex == NULL) {
        color_fmt = 0;
    }

    bool native_tri  = mtl_native_tri_depth_eligible(pg);
    bool native_quad = mtl_native_quad_eligible(pg);

    /* M5.5/M5.8: inline_elements branch. Build full per-attribute
     * streams for [min..max] guest elements, offset indices by min so
     * the first decoded element is index 0 in the local arrays. */
    if (pg->inline_elements_length > 0) {
        uint32_t min_e = (uint32_t)-1, max_e = 0;
        for (unsigned int i = 0; i < pg->inline_elements_length; i++) {
            uint32_t e = pg->inline_elements[i];
            if (e < min_e) min_e = e;
            if (e > max_e) max_e = e;
        }
        if (min_e == (uint32_t)-1) {
            return;
        }
        uint32_t span = max_e - min_e + 1;

        MtlAttributeStream streams[MTL_VERTEX_NUM_ATTRIBUTES];
        pgraph_mtl_collect_all_vertex_streams(d, MTL_VERTEX_SRC_VRAM, 0,
                                              min_e, span, streams);

        uint32_t *idx = g_malloc_n(pg->inline_elements_length,
                                    sizeof(uint32_t));
        for (unsigned int i = 0; i < pg->inline_elements_length; i++) {
            idx[i] = pg->inline_elements[i] - min_e;
        }

        mtl_dispatch_decoded_draw(d, color_tex, depth_tex,
                                  color_fmt, depth_fmt, vp_w, vp_h,
                                  draw_target_vram_addr,
                                  streams, span,
                                  idx, pg->inline_elements_length,
                                  native_tri, native_quad);

        g_free(idx);
        pgraph_mtl_free_attribute_streams(streams);
        return;
    }

    /* M5.5/M5.8: draw_arrays branch. Decode each draw_arrays_start[i]
     * / count[i] subrange independently — matches the GL/VK pattern
     * of one drawcall per subrange. */
    if (pg->draw_arrays_length > 0) {
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            uint32_t start = pg->draw_arrays_start[i];
            uint32_t count = pg->draw_arrays_count[i];
            if (count == 0) {
                continue;
            }
            MtlAttributeStream streams[MTL_VERTEX_NUM_ATTRIBUTES];
            pgraph_mtl_collect_all_vertex_streams(d, MTL_VERTEX_SRC_VRAM, 0,
                                                  start, count, streams);

            mtl_dispatch_decoded_draw(d, color_tex, depth_tex,
                                      color_fmt, depth_fmt, vp_w, vp_h,
                                      draw_target_vram_addr,
                                      streams, count,
                                      NULL, 0,
                                      native_tri, native_quad);

            pgraph_mtl_free_attribute_streams(streams);
        }
        return;
    }

    /* M5.5/M5.8: inline_array branch. */
    if (pg->inline_array_length > 0) {
        unsigned int vertex_stride = pgraph_mtl_inline_array_vertex_stride(pg);
        if (vertex_stride == 0) {
            return;
        }
        pgraph_mtl_inline_array_update_offsets(pg);
        unsigned int vcount =
            pg->inline_array_length * 4u / vertex_stride;
        if (vcount == 0) {
            return;
        }
        MtlAttributeStream streams[MTL_VERTEX_NUM_ATTRIBUTES];
        pgraph_mtl_collect_all_vertex_streams(d, MTL_VERTEX_SRC_INLINE_ARRAY,
                                              vertex_stride, 0, vcount, streams);

        mtl_dispatch_decoded_draw(d, color_tex, depth_tex,
                                  color_fmt, depth_fmt, vp_w, vp_h,
                                  draw_target_vram_addr,
                                  streams, vcount,
                                  NULL, 0,
                                  native_tri, native_quad);

        pgraph_mtl_free_attribute_streams(streams);
        return;
    }


    /* inline_buffer fallback (original M3/M4 path). M5.8 wraps every
     * populated inline_buffer into the streams array — we don't copy
     * the data; the caller-owned `attr->inline_buffer` outlives the
     * dispatch and the GPU staging completes synchronously inside it.
     */
    unsigned int vcount = pg->inline_buffer_length;
    if (vcount == 0) {
        return;
    }

    MtlAttributeStream streams[MTL_VERTEX_NUM_ATTRIBUTES];
    for (int i = 0; i < MTL_VERTEX_NUM_ATTRIBUTES; i++) {
        streams[i].data = NULL;
        streams[i].bytes = 0;
    }
    /* Borrow each populated inline_buffer pointer; do NOT free at the
     * end of this scope. */
    for (int i = 0; i < MTL_VERTEX_NUM_ATTRIBUTES; i++) {
        VertexAttribute *a = &pg->vertex_attributes[i];
        if (a->inline_buffer_populated && a->inline_buffer != NULL) {
            streams[i].data = a->inline_buffer;
            streams[i].bytes = (size_t)vcount * 4 * sizeof(float);
        }
    }
    /* M3/M4 hand-coded passthrough requires POSITION at slot 0 — bail
     * if the inline_buffer flow didn't populate it. */
    if (streams[NV2A_VERTEX_ATTR_POSITION].data == NULL) {
        return;
    }

    /* For the inline_buffer path we set masks based on
     * inline_buffer_populated (vk parity:
     * pgraph_vk_bind_vertex_attributes_inline). Use the inline-buffer-
     * specific helper; mtl_dispatch_decoded_draw saves/restores around
     * its own call too — that's fine, the inner restore will put
     * uniform_attrs back to whatever we set here. */
    uint16_t saved_u = 0, saved_c = 0, saved_s = 0;
    pgraph_mtl_set_attr_masks_inline_buffer(pg, &saved_u, &saved_c, &saved_s);

    mtl_dispatch_decoded_draw(d, color_tex, depth_tex,
                              color_fmt, depth_fmt, vp_w, vp_h,
                              draw_target_vram_addr,
                              streams, vcount,
                              NULL, 0,
                              native_tri, native_quad);

    pgraph_mtl_restore_attr_masks(pg, saved_u, saved_c, saved_s);
    /* Borrowed pointers — no free. */
}

/* W4 (2026-05-04): wrapper around pgraph_mtl_flush_draw_inner that
 * runs the per-draw color RT dump after the inner flush returns. The
 * dump path closes the open coalesced render pass first so the
 * post-MSAA-resolve color texture is the source of the blit (the
 * resolveTexture survives the close; the multisample companion is
 * persisted-then-discarded on the next pass open).
 *
 * Originally W4 always called `pgraph_mtl_draw_flush_open_pass()` and
 * `pgraph_mtl_surface_get_color_texture()` here, gating only the dump
 * itself on `s_dump_enabled` inside `pgraph_mtl_draw_dump_rt_after_flush_draw`.
 * That defeated the M5.7 render-pass coalescing optimization on every
 * benchmark run regardless of whether dumping was enabled — every
 * guest draw forced an MSAA resolve and a fresh pass open, which on
 * PGR2 / Rainbow caused the per-VRAM surface cache's MSAA companion
 * to be dropped between consecutive draws to the same render target
 * and the present pipeline read uninitialized magenta from the next
 * cycle's freshly-allocated companion.
 *
 * The fix: gate the open-pass flush and texture lookup behind
 * `pgraph_mtl_draw_dump_rt_active()`. When dumping is off the
 * coalesced pass stays open across consecutive flush_draw calls (the
 * M5.7 contract). When dumping is on the per-draw close+resolve cost
 * is accepted as a debug-mode tax — that's the use case for which
 * the dump exists.
 *
 * The cumulative draw index is bumped once per flush_draw call (the
 * NV2A logical draw boundary). It is independent of the per-pipeline
 * METAL_DRAW_COUNT (which counts each MTL drawIndexedPrimitives /
 * drawPrimitives encode) — flush_draw maps 1:1 to a guest draw call,
 * which is the index the caller cares about when correlating against
 * the equivalent GL render pass.
 */
static void pgraph_mtl_flush_draw(NV2AState *d)
{
    pgraph_mtl_flush_draw_inner(d);

    if (!pgraph_mtl_draw_dump_rt_active()) {
        return;
    }

    /* Flush the open coalesced pass so the resolved color texture is
     * what the dump's blit reads. Without this the resolve has not
     * happened yet (it happens at pass close), and an MSAA companion's
     * sub-pixel layout is not what the caller wants. */
    pgraph_mtl_draw_flush_open_pass();

    void *color_tex = pgraph_mtl_surface_get_color_texture();
    pgraph_mtl_draw_dump_rt_after_flush_draw(color_tex);
}

static void pgraph_mtl_get_report(NV2AState *d, uint32_t parameter)
{
    pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
}

/* M5.9-followup-A: NV097_IMAGE_BLIT handler implemented in mtl/blit.c
 * (mirrors vk/blit.c + gl/blit.c). Kept in a separate .c file so this
 * file's renderer-ops dispatch table stays uncluttered, and so blit.c
 * can compile against the per-target preprocessor flags via
 * specific_ss the same way renderer.c does. */
extern void pgraph_mtl_image_blit(NV2AState *d);

static void pgraph_mtl_pre_savevm_trigger(NV2AState *d)
{
    /* M5.5+: drain any open coalesced pass before snapshotting so the
     * post-restore renderer doesn't trip over an in-flight encoder. */
    pgraph_mtl_draw_flush_open_pass();
}

static void pgraph_mtl_pre_savevm_wait(NV2AState *d)
{
}

static void pgraph_mtl_pre_shutdown_trigger(NV2AState *d)
{
    /* M5.5+: drain before shutdown for the same reason as savevm. */
    pgraph_mtl_draw_flush_open_pass();
}

static void pgraph_mtl_pre_shutdown_wait(NV2AState *d)
{
}

static void pgraph_mtl_process_pending_reports(NV2AState *d)
{
}

static void pgraph_mtl_surface_update(NV2AState *d, bool upload,
                                      bool color_write, bool zeta_write)
{
    /* M5.10 (2026-05-03): KVM/HVF parity polling. The TCG path uses the
     * memory-region access callback registered in `mtl_arm_access_callback`
     * to mark surfaces dirty on guest CPU writes. KVM/HVF lacks that
     * machinery, so we mirror the GL renderer's
     * `gl/surface.c:1385-1389` pattern: `memory_region_test_and_clear_dirty`
     * polls the DIRTY_MEMORY_NV2A bit; if any tracked surface's range
     * is dirty, mark its cache entry dirty_vram so the next bind /
     * publish uploads from VRAM. The bit is cleared by the test-and-
     * clear so we only re-mark on subsequent guest writes.
     *
     * Under TCG this loop is a no-op (the per-CPU access-callback path
     * already handled it inline). M5.10 follow-up: gate on
     * !tcg_enabled() because per-call polling cost is non-trivial when
     * surface_update fires hundreds of times per second (NV097_WAIT_FOR_IDLE
     * / flip / blit hooks); under KVM/HVF the access-callback path is
     * unavailable so this is the authoritative trigger.
     *
     * Hooked into upload=true paths only; the no-upload variant
     * (download direction) is handled by `pgraph_mtl_flip_stall`. */
    (void)color_write;
    (void)zeta_write;
    if (!upload || d == NULL || d->vram == NULL) {
        return;
    }
    /* Gate on !tcg_enabled() to skip the polling loop when the per-CPU
     * access-callback path is already authoritative. PGR2 surface_update
     * fires per-NV097_WAIT_FOR_IDLE / per-flip and we observed ~10x
     * boot slowdown when the polling ran on every call regardless of
     * acceleration mode. */
    if (tcg_enabled()) {
        return;
    }
    /* M5.10 codex finding (HIGH severity, 2026-05-03): poll the FULL
     * surface byte range, not a fixed 4 KB. Real framebuffers are
     * multi-MB (PGR2 back: 2.5 MB; front: 1.2 MB; aux RTs ~256 KB-1 MB);
     * test-and-clear over only the first page silently dropped guest
     * writes outside the first 4 KB. */
    uint32_t addrs[32];
    uint32_t sizes[32];
    unsigned int n = pgraph_mtl_surface_iter_address_size(addrs, sizes, 32);
    for (unsigned int i = 0; i < n; i++) {
        if (sizes[i] == 0) {
            continue;
        }
        bool dirty = memory_region_test_and_clear_dirty(
            d->vram, (hwaddr)addrs[i], (hwaddr)sizes[i],
            DIRTY_MEMORY_NV2A);
        if (dirty) {
            pgraph_mtl_surface_mark_dirty_overlapping(addrs[i], sizes[i]);
        }
    }
}

static void pgraph_mtl_surface_flush(NV2AState *d)
{
    /* M5.5+: drain any open coalesced pass so the surface texture is
     * GPU-stable for whatever consumer the upstream caller had in
     * mind. M5.9: also drop the per-VRAM cache so a fresh
     * surface_scale_factor / display reset does not retain stale
     * MTLTextures. M5.9-followup-B: disarm any access-callbacks before
     * the cache is freed so we don't leave dangling MemAccessCallback
     * pointers in the per-CPU watch list. */
    pgraph_mtl_draw_flush_open_pass();
    /* M5.10 (2026-05-03): download every draw-dirty surface to guest
     * VRAM before dropping the cache so the post-flush bind picks up
     * the rendered content from VRAM instead of starting from a
     * cleared state. Mirrors `vk/surface.c:524-535::pgraph_vk_download_dirty_surfaces`
     * which is called from the equivalent flush path. Gated on the
     * M5.10 feature flag (default off). */
    if (mtl_front_fb_download_enabled() && d != NULL && d->vram_ptr != NULL) {
        pgraph_mtl_surface_download_dirty_all(d->vram_ptr,
                                              mtl_after_surface_download, d);
    }
    mtl_disarm_all_access_callbacks(d);
    pgraph_mtl_surface_cache_flush();
}

static void pgraph_mtl_set_surface_scale_factor(NV2AState *d,
                                                unsigned int scale)
{
    /* Honor the global config knob; the surface manager will reallocate
     * on the next clear/draw because the dimensions will mismatch. */
    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;
    d->pgraph.surface_scale_factor = MAX((int)scale, 1);
}

static unsigned int pgraph_mtl_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor ? d->pgraph.surface_scale_factor : 1;
}

static int pgraph_mtl_get_framebuffer_surface(NV2AState *d)
{
    /* M5.9: CRTC-aware publish. Mirrors vk/renderer.c:172-205 and
     * gl/display.c:414-448 — the published front-fb is the surface in
     * the per-VRAM cache that contains `d->pcrtc.start +
     * vga_display_params.line_offset`, NOT whichever surface was most
     * recently clear-bound.
     *
     * The signature returns `int` (the GL renderer returns a GLuint).
     * For Metal we return 1/0 (presence signal) and publish the
     * resolved MTLTexture via the side-channel accessor
     * `pgraph_mtl_get_framebuffer_metal_texture()` read by the
     * compositor in ui/xemu-metal.mm.
     *
     * On miss (no cache entry contains the CRTC address) we leave the
     * previous front-fb pointer in place — better to display a
     * stale-but-correct frame than reset to black mid-stream. The
     * presence signal still returns 1 if a previous publish landed; 0
     * if the cache is entirely empty.
     *
     * T2 (2026-05-12 evening): this op is now invoked from
     * xemu_metal_render_frame at every host vsync (~60 Hz), not just
     * on guest NV097_FLIP_STALL. The previous implementation called
     * `pgraph_mtl_surface_publish_display_front_fb` which runs a
     * render-pass compose into a display-sized texture with a
     * `waitUntilCompleted` GPU sync — fine at 0.33 Hz (the flip_stall
     * rate on BIOS boot) but catastrophic at 60 Hz (drops effective
     * present cadence to ~2 fps). Switch to the cheap
     * `pgraph_mtl_surface_publish_front_fb` path which just stores
     * the surface's MTLTexture pointer in the side channel. The
     * compositor (xemu-metal.mm:1410) already does its own present-
     * pipeline scaling into the drawable. Line-offset correction is
     * not applied here; the compositor handles aspect/scaling.
     * `pgraph_mtl_flip_stall` keeps the compose path for the
     * once-per-guest-flip publish where the dimension correction
     * matters. */
    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);
    hwaddr crtc_addr = d->pcrtc.start + vga_display_params.line_offset;

    qemu_mutex_unlock(&d->pfifo.lock);

    /* Try to publish the surface that contains `crtc_addr`. */
    (void)mtl_get_display_dimensions;  /* still useful in flip_stall path */

    /* T2 followup (Codex review, 2026-05-12): take pg->lock around the
     * cache access. Without this, the host-refresh publish races with
     * PFIFO-thread cache mutations (binding_destroy, eviction, rebind)
     * which run under pg->lock from the NV097 method handlers. The
     * pre-T2 flip_stall publish path was implicitly safe because
     * `DEF_METHOD(NV097, FLIP_STALL)` (pgraph.c:1030) holds pg->lock
     * during the call; this op was called from the display thread
     * without that guarantee, but at the 0.33 Hz flip_stall rate the
     * race window almost never hit. T2 moved this to 60 Hz, widening
     * the race ~200×. Taking pg->lock here serializes with PFIFO
     * cache mutations and matches the locking discipline of other
     * Metal renderer ops that touch the cache from non-PFIFO threads
     * (e.g. mtl_after_surface_download at renderer.c:466).
     *
     * Lock-order note: nv2a_get_framebuffer_surface (pgraph.c:481)
     * already holds renderer_lock when calling this op. PFIFO workers
     * never take renderer_lock, so renderer_lock → pg->lock is safe;
     * no AB-BA deadlock is possible. */
    qemu_mutex_lock(&d->pgraph.lock);
    int has_fb = pgraph_mtl_surface_has_front_framebuffer();
    if (!(mtl_front_fb_fallback_enabled() && has_fb)) {
        bool published = pgraph_mtl_surface_publish_front_fb_pointer_only(
            (uint32_t)crtc_addr, "crtc-refresh");
        (void)published;
        has_fb = pgraph_mtl_surface_has_front_framebuffer();
    }
    qemu_mutex_unlock(&d->pgraph.lock);

    return has_fb;
}

static GPUProperties *pgraph_mtl_get_gpu_properties(void)
{
    static GPUProperties props;
    return &props;
}

static void pgraph_mtl_init(NV2AState *d, Error **errp)
{
    /* Slice M2: bring up the heap manager and surface manager. They
     * both rely on xemu_metal_init() having already created the
     * MTLDevice (it has — main thread runs xemu_metal_init at SDL
     * window-creation time, well before the NV2A renderer is selected
     * via display.renderer = METAL).
     *
     * If init fails the renderer enters a degraded state (clears
     * become silent no-ops); we don't propagate the error out as a
     * fatal because the caller has no fallback once the SDL window
     * was created with SDL_WINDOW_METAL. The fprintf inside the heap
     * init is the user-visible signal. */
    if (!pgraph_mtl_heap_init()) {
        fprintf(stderr, "pgraph_mtl_init: heap init failed; clear "
                        "operations will be no-ops\n");
        return;
    }
    if (!pgraph_mtl_surface_init()) {
        fprintf(stderr, "pgraph_mtl_init: surface init failed; clear "
                        "operations will be no-ops\n");
        pgraph_mtl_heap_finalize();
        return;
    }

    /* M3 brings up the staging-buffer ring, the pipeline cache, and
     * the draw command queue. Failures here are not fatal — clears
     * still work, just no draws will land. */
    if (!pgraph_mtl_buffer_init()) {
        fprintf(stderr, "pgraph_mtl_init: buffer ring init failed; "
                        "draws will be no-ops\n");
    }
    if (!pgraph_mtl_pipeline_init()) {
        fprintf(stderr, "pgraph_mtl_init: pipeline cache init failed; "
                        "draws will be no-ops\n");
    }
    if (!pgraph_mtl_draw_init()) {
        fprintf(stderr, "pgraph_mtl_init: draw init failed; "
                        "draws will be no-ops\n");
    }

    /* W4 (2026-05-04): parse XEMU_METAL_DUMP_DRAW_RT once. Must
     * happen after pgraph_mtl_draw_init so the queue / device handles
     * exist before the first flush_draw runs. Off-by-default; zero
     * hot-path cost when unset. */
    pgraph_mtl_draw_dump_rt_init();

    /* M5: bring up the GLSL → SPIR-V → MSL translator. Failure here
     * is non-fatal — without the translator, shader-state-driven
     * draws will fall back to the M3/M4 hand-coded passthrough
     * pipeline. */
    if (!pgraph_mtl_glsl_init()) {
        fprintf(stderr, "pgraph_mtl_init: GLSL translator init failed; "
                        "shader-state-driven draws will use the "
                        "M3/M4 hand-coded passthrough\n");
    }

    /* M6 (cache + draw-path swap): per-PipelineKey LRU cache. */
    if (!pgraph_mtl_shaders_init()) {
        fprintf(stderr, "pgraph_mtl_init: shader pipeline cache init "
                        "failed; draws will fall back to the M3/M4 "
                        "hand-coded passthrough\n");
    }

    /* M6 (textures + sampling): texture cache + sampler cache. */
    if (!pgraph_mtl_texture_init()) {
        fprintf(stderr, "pgraph_mtl_init: texture cache init failed; "
                        "textured draws will be untextured\n");
    }

    /* M7.1: uniform staging ring for translated UBOs. */
    if (!pgraph_mtl_uniform_init()) {
        fprintf(stderr, "pgraph_mtl_init: uniform ring init failed; "
                        "translated draws will fall back to "
                        "M3/M4 passthrough\n");
    }

    /* M5: optional shader-validation harness. Driven by
     * XEMU_METAL_SHADER_VALIDATE; advisory by default, aborts on
     * failure when set to "strict" or "2". The harness runs the
     * representative ShaderState fixtures end-to-end through GLSL
     * → SPIR-V → MSL → MTLLibrary. See shader_validation.h. */
    (void)pgraph_mtl_shader_validate_run();

    /* Reload the config-driven surface scale factor so the first
     * ensure_color/ensure_depth picks up the user-visible
     * display.quality.surface_scale value (1 default; 2 on Apple
     * Silicon first-run). */
    int factor = g_config.display.quality.surface_scale;
    d->pgraph.surface_scale_factor = MAX(factor, 1);

    /* M11: parse XEMU_METAL_MSAA, clamp to the device's supported
     * sample counts, and publish to the surface manager so it
     * allocates the multisample companion textures alongside the
     * single-sample bindings. The companion is created lazily on
     * the first ensure_color/ensure_depth that produces a real
     * binding shape. */
    uint32_t requested = 0;
    uint32_t configured = parse_metal_msaa_env(&requested);
    uint32_t effective = configured;
    while (effective > 1 &&
           !pgraph_mtl_heap_supports_sample_count(effective)) {
        /* Step down to the next supported lower count.
         * MTLDevice supportsTextureSampleCount: typically returns
         * true for {1, 2, 4, 8}; this loop is defensive against
         * future device variants. */
        if (effective == 8) {
            effective = 4;
        } else if (effective == 4) {
            effective = 2;
        } else {
            effective = 1;
        }
    }
    s_metal_msaa_sample_count = effective;
    pgraph_mtl_surface_set_msaa_sample_count(effective);
    fprintf(stderr,
            "xemu-perf: metal_msaa=%u source=XEMU_METAL_MSAA "
            "requested=%u configured=%u\n",
            effective, requested, configured);
}

/* M11: accessor for the latched effective MSAA sample count. Surface
 * manager + state.c + draw.mm consult this to keep render-pass
 * storeAction, pipeline rasterSampleCount, and PipelineKey
 * sample_count consistent. */
uint32_t pgraph_mtl_renderer_msaa_sample_count(void)
{
    return s_metal_msaa_sample_count;
}

static void pgraph_mtl_finalize(NV2AState *d)
{
    /* Tear down in reverse init order. M5.9-followup-B: disarm all
     * access callbacks before the cache is finalized to avoid
     * dangling pointers in the per-CPU watch list. */
    mtl_disarm_all_access_callbacks(d);
    pgraph_mtl_uniform_finalize();
    pgraph_mtl_texture_finalize();
    pgraph_mtl_shaders_finalize();
    pgraph_mtl_glsl_finalize();
    pgraph_mtl_draw_finalize();
    pgraph_mtl_pipeline_finalize();
    pgraph_mtl_buffer_finalize();
    pgraph_mtl_surface_finalize();
    pgraph_mtl_heap_finalize();
}

static PGRAPHRenderer pgraph_mtl_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_METAL,
    .name = "Metal",
    .ops = {
        .init = pgraph_mtl_init,
        .finalize = pgraph_mtl_finalize,
        .clear_report_value = pgraph_mtl_clear_report_value,
        .clear_surface = pgraph_mtl_clear_surface,
        .draw_begin = pgraph_mtl_draw_begin,
        .draw_end = pgraph_mtl_draw_end,
        .flip_stall = pgraph_mtl_flip_stall,
        .flush_draw = pgraph_mtl_flush_draw,
        .get_report = pgraph_mtl_get_report,
        .image_blit = pgraph_mtl_image_blit,
        .pre_savevm_trigger = pgraph_mtl_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_mtl_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_mtl_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_mtl_pre_shutdown_wait,
        .process_pending = pgraph_mtl_process_pending,
        .process_pending_reports = pgraph_mtl_process_pending_reports,
        .surface_update = pgraph_mtl_surface_update,
        .surface_flush = pgraph_mtl_surface_flush,
        .set_surface_scale_factor = pgraph_mtl_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_mtl_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_mtl_get_framebuffer_surface,
        .get_gpu_properties = pgraph_mtl_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_mtl_renderer);
}

/* M7 counters — surface via extract-perf-summary.sh. */
uint64_t pgraph_mtl_pipeline_key_built_count(void)
{
    return atomic_load(&s_pipeline_key_built);
}
uint64_t pgraph_mtl_pipeline_translated_ok_count(void)
{
    return atomic_load(&s_pipeline_translated_ok);
}
uint64_t pgraph_mtl_pipeline_translated_failed_count(void)
{
    return atomic_load(&s_pipeline_translated_fb);
}
uint64_t pgraph_mtl_dispatch_us_total(void)
{
    return atomic_load(&s_dispatch_us_total);
}
uint64_t pgraph_mtl_texture_bind_us_total(void)
{
    return atomic_load(&s_texture_bind_us_total);
}

/* M8 counter — draws skipped because the translated pipeline was still
 * building. */
uint64_t pgraph_mtl_draws_skipped_pending_count(void)
{
    return atomic_load(&s_draws_skipped_pending);
}

/* Task #13 counter — draws where CPU-side flat-quad color propagation
 * fired (FLAT-shaded OP_QUADS / OP_QUAD_STRIP with NV2A's default
 * LAST-vertex provoking convention). */
uint64_t pgraph_mtl_flat_quad_propagations_count(void)
{
    return atomic_load(&s_flat_quad_propagations);
}
