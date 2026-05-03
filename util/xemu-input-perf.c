/*
 * Apple Silicon performance fork: input-latency counters (slice N1).
 *
 * See include/qemu/xemu-input-perf.h for the API contract. The
 * implementation is per-port last-backend-update timestamps plus
 * a few atomic counters; no mutex (qatomic_*  is wait-free on
 * aarch64 and x86 for aligned 64-bit slots).
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/xemu-input-perf.h"

#define INPUT_PERF_NUM_PORTS 4

static int64_t input_perf_last_backend_us[INPUT_PERF_NUM_PORTS];

static uint64_t input_perf_usb_polls;
static uint64_t input_perf_backend_updates;
static uint64_t input_perf_lat_us_total;
static uint64_t input_perf_lat_us_max;

static void input_perf_max(uint64_t *counter, uint64_t value)
{
    uint64_t old = qatomic_read(counter);

    while (value > old) {
        uint64_t previous = qatomic_cmpxchg(counter, old, value);
        if (previous == old) {
            break;
        }
        old = previous;
    }
}

void xemu_input_perf_record_backend_update_port(int port, int64_t now_us)
{
    if (port < 0 || port >= INPUT_PERF_NUM_PORTS) {
        qatomic_inc(&input_perf_backend_updates);
        return;
    }

    qatomic_set_i64(&input_perf_last_backend_us[port], now_us);
    qatomic_inc(&input_perf_backend_updates);
}

void xemu_input_perf_record_backend_update(int64_t now_us)
{
    /* Bump the global counter and stamp every port. The native macOS
     * backend stamps each port individually as it updates that port's
     * GCController state; this entry point is the catch-all the SDL
     * path uses when the per-port mapping isn't immediately available
     * at the call site. */
    for (int i = 0; i < INPUT_PERF_NUM_PORTS; i++) {
        qatomic_set_i64(&input_perf_last_backend_us[i], now_us);
    }
    qatomic_inc(&input_perf_backend_updates);
}

void xemu_input_perf_record_usb_poll(int port, int64_t now_us)
{
    qatomic_inc(&input_perf_usb_polls);

    if (port < 0 || port >= INPUT_PERF_NUM_PORTS) {
        return;
    }

    int64_t last = qatomic_read_i64(&input_perf_last_backend_us[port]);
    if (last == 0 || now_us < last) {
        /* No backend update yet (last == 0), or the monotonic clock
         * went backwards / a new backend reset the timestamp. Skip
         * folding bogus latency in. */
        return;
    }

    uint64_t delta = (uint64_t)(now_us - last);
    qatomic_add(&input_perf_lat_us_total, delta);
    input_perf_max(&input_perf_lat_us_max, delta);
}

void xemu_input_perf_emit_and_reset(FILE *out)
{
    if (out == NULL) {
        return;
    }

    uint64_t polls = qatomic_xchg(&input_perf_usb_polls, 0);
    uint64_t updates = qatomic_xchg(&input_perf_backend_updates, 0);
    uint64_t lat_total = qatomic_xchg(&input_perf_lat_us_total, 0);
    uint64_t lat_max = qatomic_xchg(&input_perf_lat_us_max, 0);

    if ((polls | updates | lat_total | lat_max) == 0) {
        return;
    }

    fprintf(out,
            " INPUT_USB_POLLS=%llu INPUT_BACKEND_UPDATES=%llu"
            " INPUT_LAT_US_TOTAL=%llu INPUT_LAT_US_MAX=%llu",
            (unsigned long long)polls,
            (unsigned long long)updates,
            (unsigned long long)lat_total,
            (unsigned long long)lat_max);
}
