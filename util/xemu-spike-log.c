/*
 * Apple Silicon performance fork: shared spike-log emit helper.
 *
 * See include/qemu/xemu-spike-log.h for the API contract. The
 * implementation is intentionally tiny: an env-driven init, three
 * cached globals, and a fprintf path. The hot-path skip test (the
 * enable-bit check + threshold compare) is done at the call site so
 * the cost is a single load + branch when no spike has fired.
 */

#include "qemu/osdep.h"
#include "qemu/xemu-spike-log.h"
#include "qemu/timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool xemu_spike_log_renderer_enabled;
bool xemu_spike_log_tcg_enabled;
bool xemu_tcg_phase_log_enabled;
int64_t xemu_spike_threshold_us = 50000;

static bool spike_log_initialized;

static bool env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

void xemu_spike_log_init(void)
{
    if (spike_log_initialized) {
        return;
    }
    spike_log_initialized = true;

    xemu_spike_log_renderer_enabled = env_flag_enabled("XEMU_PERF_SPIKE_LOG");
    xemu_spike_log_tcg_enabled = env_flag_enabled("XEMU_PERF_SPIKE_LOG_TCG");
    xemu_tcg_phase_log_enabled = env_flag_enabled("XEMU_TCG_PHASE_LOG");

    const char *threshold_env = getenv("XEMU_PERF_SPIKE_LOG_THRESHOLD_US");
    if (threshold_env && threshold_env[0]) {
        char *end = NULL;
        long t = strtol(threshold_env, &end, 10);
        /* Reject negative / sub-1ms values. The 1ms floor avoids
         * pathological log volume if a caller fat-fingers the env. */
        if (end != threshold_env && *end == '\0' && t >= 1000) {
            xemu_spike_threshold_us = t;
        }
    }
}

void xemu_spike_emit(const char *op, int64_t duration_us, const char *extra)
{
    /* Defense in depth: caller is supposed to gate before calling, but
     * keep the threshold check here too so an unguarded call never
     * exceeds budget. */
    if (duration_us < xemu_spike_threshold_us) {
        return;
    }

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (extra && extra[0]) {
        fprintf(stderr,
                "xemu-spike: op=%s duration_us=%lld now_us=%lld %s\n",
                op ? op : "?",
                (long long)duration_us,
                (long long)now_us,
                extra);
    } else {
        fprintf(stderr,
                "xemu-spike: op=%s duration_us=%lld now_us=%lld\n",
                op ? op : "?",
                (long long)duration_us,
                (long long)now_us);
    }
}
