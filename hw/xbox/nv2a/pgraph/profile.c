/*
 * QEMU Geforce NV2A profiling helpers
 *
 * Copyright (c) 2020-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "qemu/xemu-tcg-perf.h"
#include "qemu/xemu-spike-log.h"
#include "qemu/xemu-display-perf.h"
#include "qemu/xemu-apu-perf.h"

NV2AStats g_nv2a_stats;

/*
 * Frame-time tracking is kept in microseconds internally even though the
 * in-app HUD plot (`g_nv2a_stats.frame_history[].mspf`) still uses integer
 * milliseconds. The `xemu-perf:` interval lines emit sub-millisecond
 * precision so jitter percentiles can resolve the 60 FPS budget (16.67 ms)
 * without integer-rounding away the difference between "ok" and "over
 * budget" frames.
 */
#define NV2A_PROF_FRAME_BUF_LEN 1024

static struct {
    bool initialized;
    bool enabled;
    bool frame_log_enabled;
    int64_t interval_us;
    int64_t interval_start_us;
    uint64_t frames;
    int64_t mspf_us_sum;
    int64_t mspf_us_min;
    int64_t mspf_us_max;
    /* Optional per-frame mspf_us buffer for XEMU_PERF_FRAME_LOG=1.
     * Bounded to avoid unbounded log lines under extreme frame counts. */
    int64_t frame_mspf_us[NV2A_PROF_FRAME_BUF_LEN];
    int frame_buf_count;
    int frame_buf_overflow;
    uint64_t counters[NV2A_PROF__COUNT];
    bool registered_atexit;
    bool flushed;
    /* XEMU_PERF_SPIKE_LOG=1: emit `xemu-spike:` lines for any timed
     * operation exceeding spike_threshold_us (default 50 ms, tunable
     * via XEMU_PERF_SPIKE_LOG_THRESHOLD_US). Off by default. */
    bool spike_log_enabled;
    int64_t spike_threshold_us;
} perf_log;

static bool env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static bool counters_nonzero(const uint64_t counters[NV2A_PROF__COUNT])
{
    for (unsigned int i = 0; i < NV2A_PROF__COUNT; i++) {
        if (counters[i] != 0) {
            return true;
        }
    }

    return false;
}

static bool frame_counters_nonzero(const int counters[NV2A_PROF__COUNT])
{
    for (unsigned int i = 0; i < NV2A_PROF__COUNT; i++) {
        if (counters[i] != 0) {
            return true;
        }
    }

    return false;
}

static void nv2a_profile_log_atexit(void);

static void nv2a_profile_log_init(void)
{
    if (perf_log.initialized) {
        return;
    }

    perf_log.initialized = true;
    perf_log.enabled = env_flag_enabled("XEMU_PERF_LOG");
    perf_log.frame_log_enabled = env_flag_enabled("XEMU_PERF_FRAME_LOG");

    /* V3: spike-log state lives in the shared util/xemu-spike-log.c so
     * the TCG-thread sources can call into it without a renderer
     * include dependency. The renderer mirrors the cached enable +
     * threshold for backward-compat with existing call sites. */
    xemu_spike_log_init();
    perf_log.spike_log_enabled = xemu_spike_log_renderer_enabled;
    perf_log.spike_threshold_us = xemu_spike_threshold_us;

    perf_log.interval_us = 1000000;

    const char *interval_ms_env = getenv("XEMU_PERF_LOG_INTERVAL_MS");
    if (interval_ms_env && interval_ms_env[0]) {
        char *end = NULL;
        long interval_ms = strtol(interval_ms_env, &end, 10);
        if (end != interval_ms_env && *end == '\0' && interval_ms >= 100) {
            perf_log.interval_us = interval_ms * 1000;
        } else {
            fprintf(stderr,
                    "xemu-perf: invalid XEMU_PERF_LOG_INTERVAL_MS '%s', using 1000\n",
                    interval_ms_env);
        }
    }

    if (perf_log.enabled && !perf_log.registered_atexit) {
        atexit(nv2a_profile_log_atexit);
        perf_log.registered_atexit = true;
    }
}

static void nv2a_profile_log_emit_interval(int64_t now, bool final,
                                           const char *reason)
{
    if (perf_log.frames == 0 && !counters_nonzero(perf_log.counters)) {
        return;
    }

    int64_t elapsed_us = now - perf_log.interval_start_us;
    double elapsed_s = elapsed_us / 1000000.0;
    double fps = elapsed_s > 0 ? perf_log.frames / elapsed_s : 0.0;
    double avg_mspf_ms =
        perf_log.frames > 0
            ? (double)perf_log.mspf_us_sum / perf_log.frames / 1000.0
            : 0;
    double min_mspf_ms = perf_log.mspf_us_min / 1000.0;
    double max_mspf_ms = perf_log.mspf_us_max / 1000.0;

    fprintf(stderr,
            "xemu-perf: interval_ms=%lld frames=%llu fps=%.2f "
            "mspf_avg=%.3f mspf_min=%.3f mspf_max=%.3f increment_fps=%u",
            (long long)(elapsed_us / 1000),
            (unsigned long long)perf_log.frames,
            fps,
            avg_mspf_ms,
            min_mspf_ms,
            max_mspf_ms,
            g_nv2a_stats.increment_fps);

    if (final) {
        fprintf(stderr, " final=1 reason=%s", reason ? reason : "unknown");
    }

    for (unsigned int i = 0; i < NV2A_PROF__COUNT; i++) {
        if (perf_log.counters[i] != 0) {
            fprintf(stderr, " %s=%llu",
                    nv2a_profile_get_counter_name(i),
                    (unsigned long long)perf_log.counters[i]);
        }
    }

    if (perf_log.frame_log_enabled && perf_log.frame_buf_count > 0) {
        fprintf(stderr, " frame_mspf_us=");
        for (int i = 0; i < perf_log.frame_buf_count; i++) {
            fprintf(stderr, "%s%lld",
                    i == 0 ? "" : ",",
                    (long long)perf_log.frame_mspf_us[i]);
        }
        if (perf_log.frame_buf_overflow > 0) {
            fprintf(stderr, " frame_mspf_us_dropped=%d",
                    perf_log.frame_buf_overflow);
        }
    }

    /* Apple Silicon performance fork: append TCG hot-path counter
     * snapshot. No-op when all counters are zero. */
    xemu_tcg_perf_emit_and_reset(stderr);

    /* Apple Silicon performance fork: append display-pacing counter
     * snapshot (vblank fires, FLIP_STALL writes, present heartbeat,
     * GL swaps). No-op when all counters are zero. Used by the 30 FPS
     * cap attribution diagnostic. */
    xemu_display_perf_emit_and_reset(stderr);

    /* Apple Silicon performance fork: append APU lock-hold / vCPU-wait
     * counter snapshot. No-op when all counters are zero. Used by the
     * audio voice-lock release slice (XEMU_APU_LOCK_RELEASE) to
     * attribute the change in MCPXAPUState::lock contention. */
    xemu_apu_perf_emit_and_reset(stderr);

    fprintf(stderr, "\n");

    perf_log.interval_start_us = now;
    perf_log.frames = 0;
    perf_log.mspf_us_sum = 0;
    perf_log.mspf_us_min = 0;
    perf_log.mspf_us_max = 0;
    perf_log.frame_buf_count = 0;
    perf_log.frame_buf_overflow = 0;
    memset(perf_log.counters, 0, sizeof(perf_log.counters));
}

static void nv2a_profile_log_add_frame(int64_t now, int64_t mspf_us,
                                       const int counters[NV2A_PROF__COUNT])
{
    if (perf_log.frames == 0 && !counters_nonzero(perf_log.counters)) {
        perf_log.interval_start_us = now;
        perf_log.mspf_us_min = mspf_us;
        perf_log.mspf_us_max = mspf_us;
    } else if (perf_log.frames == 0) {
        perf_log.mspf_us_min = mspf_us;
        perf_log.mspf_us_max = mspf_us;
    }

    perf_log.frames++;
    perf_log.mspf_us_sum += mspf_us;
    if (mspf_us < perf_log.mspf_us_min) perf_log.mspf_us_min = mspf_us;
    if (mspf_us > perf_log.mspf_us_max) perf_log.mspf_us_max = mspf_us;

    if (perf_log.frame_log_enabled) {
        if (perf_log.frame_buf_count < NV2A_PROF_FRAME_BUF_LEN) {
            perf_log.frame_mspf_us[perf_log.frame_buf_count++] = mspf_us;
        } else {
            perf_log.frame_buf_overflow++;
        }
    }

    for (unsigned int i = 0; i < NV2A_PROF__COUNT; i++) {
        perf_log.counters[i] += counters[i];
    }
}

static void nv2a_profile_log_frame(int64_t now, int64_t mspf_us,
                                   const int counters[NV2A_PROF__COUNT])
{
    nv2a_profile_log_init();
    if (!perf_log.enabled) {
        return;
    }

    nv2a_profile_log_add_frame(now, mspf_us, counters);

    int64_t elapsed_us = now - perf_log.interval_start_us;
    if (elapsed_us < perf_log.interval_us) {
        return;
    }

    nv2a_profile_log_emit_interval(now, false, NULL);
}

void nv2a_profile_log_startup(const char *renderer_name)
{
    nv2a_profile_log_init();
    if (!perf_log.enabled) {
        return;
    }

    fprintf(stderr, "xemu-perf: renderer=%s", renderer_name);

#if defined(CONFIG_DARWIN)
    fprintf(stderr, " host_os=darwin");
#elif defined(CONFIG_WIN32)
    fprintf(stderr, " host_os=windows");
#elif defined(CONFIG_LINUX)
    fprintf(stderr, " host_os=linux");
#endif

#if defined(__aarch64__)
    fprintf(stderr, " host_arch=arm64");
#elif defined(__x86_64__)
    fprintf(stderr, " host_arch=x86_64");
#elif defined(__arm__)
    fprintf(stderr, " host_arch=arm");
#endif

    fprintf(stderr, " log_interval_ms=%lld\n",
            (long long)(perf_log.interval_us / 1000));
}

void nv2a_profile_increment(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    const int64_t fps_update_interval = 250000;
    g_nv2a_stats.last_flip_time = now;

    static int64_t frame_count = 0;
    frame_count++;

    static int64_t ts = 0;
    int64_t delta = now - ts;
    if (delta >= fps_update_interval) {
        g_nv2a_stats.increment_fps = frame_count * 1000000 / delta;
        ts = now;
        frame_count = 0;
    }
}

void nv2a_profile_flip_stall(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t render_time_us = g_nv2a_stats.last_flip_time ?
                              (now - g_nv2a_stats.last_flip_time) : 0;
    int render_time_ms = (int)(render_time_us / 1000);

    /* HUD plot in ui/xui/debug.cc still consumes integer-ms mspf. */
    g_nv2a_stats.frame_working.mspf = render_time_ms;
    g_nv2a_stats.frame_history[g_nv2a_stats.frame_ptr] =
        g_nv2a_stats.frame_working;
    nv2a_profile_log_frame(now, render_time_us,
                           g_nv2a_stats.frame_working.counters);
    g_nv2a_stats.frame_ptr =
        (g_nv2a_stats.frame_ptr + 1) % NV2A_PROF_NUM_FRAMES;
    g_nv2a_stats.frame_count++;
    memset(&g_nv2a_stats.frame_working, 0, sizeof(g_nv2a_stats.frame_working));
}

void nv2a_profile_log_flush(const char *reason)
{
    nv2a_profile_log_init();
    if (!perf_log.enabled || perf_log.flushed) {
        return;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (frame_counters_nonzero(g_nv2a_stats.frame_working.counters)) {
        /* Working-frame mspf is integer-ms; promote to microseconds for the
         * aggregation path. Sub-ms precision is lost on this final flush
         * (one frame at most), so the log cost is negligible. */
        int64_t mspf_us = (int64_t)g_nv2a_stats.frame_working.mspf * 1000;
        nv2a_profile_log_add_frame(now, mspf_us,
                                   g_nv2a_stats.frame_working.counters);
        memset(&g_nv2a_stats.frame_working, 0,
               sizeof(g_nv2a_stats.frame_working));
    }

    nv2a_profile_log_emit_interval(now, true, reason);
    perf_log.flushed = true;
}

static void nv2a_profile_log_atexit(void)
{
    nv2a_profile_log_flush("atexit");
}

void nv2a_profile_spike(const char *op, int64_t duration_us)
{
    /* Backward-compat wrapper: keep the renderer's existing call sites
     * gated by XEMU_PERF_SPIKE_LOG (renderer enable bit), then delegate
     * the threshold check + emit to the shared helper. The double init
     * is cheap (idempotent flag check) and ensures the env vars are
     * read even if the TCG side hasn't initialised yet. */
    nv2a_profile_log_init();
    if (!xemu_spike_log_renderer_enabled) {
        return;
    }
    if (duration_us < xemu_spike_threshold_us) {
        return;
    }
    xemu_spike_emit(op, duration_us, NULL);
}

const char *nv2a_profile_get_counter_name(unsigned int cnt)
{
    const char *default_names[NV2A_PROF__COUNT] = {
        #define _X(x) stringify(x),
        NV2A_PROF_COUNTERS_XMAC
        #undef _X
    };

    assert(cnt < NV2A_PROF__COUNT);
    return default_names[cnt] + 10; /* 'NV2A_PROF_' */
}

int nv2a_profile_get_counter_value(unsigned int cnt)
{
    assert(cnt < NV2A_PROF__COUNT);
    unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) %
                       NV2A_PROF_NUM_FRAMES;
    return g_nv2a_stats.frame_history[idx].counters[cnt];
}
