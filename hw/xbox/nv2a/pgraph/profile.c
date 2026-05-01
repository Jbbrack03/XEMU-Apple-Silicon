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

NV2AStats g_nv2a_stats;

static struct {
    bool initialized;
    bool enabled;
    int64_t interval_us;
    int64_t interval_start_us;
    uint64_t frames;
    int64_t mspf_sum;
    int mspf_min;
    int mspf_max;
    uint64_t counters[NV2A_PROF__COUNT];
    bool registered_atexit;
    bool flushed;
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
    double avg_mspf =
        perf_log.frames > 0 ? (double)perf_log.mspf_sum / perf_log.frames : 0;

    fprintf(stderr,
            "xemu-perf: interval_ms=%lld frames=%llu fps=%.2f "
            "mspf_avg=%.2f mspf_min=%d mspf_max=%d increment_fps=%u",
            (long long)(elapsed_us / 1000),
            (unsigned long long)perf_log.frames,
            fps,
            avg_mspf,
            perf_log.mspf_min,
            perf_log.mspf_max,
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
    fprintf(stderr, "\n");

    perf_log.interval_start_us = now;
    perf_log.frames = 0;
    perf_log.mspf_sum = 0;
    perf_log.mspf_min = 0;
    perf_log.mspf_max = 0;
    memset(perf_log.counters, 0, sizeof(perf_log.counters));
}

static void nv2a_profile_log_add_frame(int64_t now, int mspf,
                                       const int counters[NV2A_PROF__COUNT])
{
    if (perf_log.frames == 0 && !counters_nonzero(perf_log.counters)) {
        perf_log.interval_start_us = now;
        perf_log.mspf_min = mspf;
        perf_log.mspf_max = mspf;
    } else if (perf_log.frames == 0) {
        perf_log.mspf_min = mspf;
        perf_log.mspf_max = mspf;
    }

    perf_log.frames++;
    perf_log.mspf_sum += mspf;
    perf_log.mspf_min = MIN(perf_log.mspf_min, mspf);
    perf_log.mspf_max = MAX(perf_log.mspf_max, mspf);

    for (unsigned int i = 0; i < NV2A_PROF__COUNT; i++) {
        perf_log.counters[i] += counters[i];
    }
}

static void nv2a_profile_log_frame(int64_t now, int mspf,
                                   const int counters[NV2A_PROF__COUNT])
{
    nv2a_profile_log_init();
    if (!perf_log.enabled) {
        return;
    }

    nv2a_profile_log_add_frame(now, mspf, counters);

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
    int64_t render_time = g_nv2a_stats.last_flip_time ?
                           (now - g_nv2a_stats.last_flip_time) / 1000 : 0;

    g_nv2a_stats.frame_working.mspf = render_time;
    g_nv2a_stats.frame_history[g_nv2a_stats.frame_ptr] =
        g_nv2a_stats.frame_working;
    nv2a_profile_log_frame(now, render_time,
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
        nv2a_profile_log_add_frame(now, g_nv2a_stats.frame_working.mspf,
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
