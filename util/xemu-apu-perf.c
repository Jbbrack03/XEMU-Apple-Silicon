/*
 * Apple Silicon performance fork: MCPX APU lock-hold / vCPU-wait counters.
 *
 * See include/qemu/xemu-apu-perf.h for the API contract. Implementation
 * is two atomic counters (sum and CAS-loop max) and a snapshot/reset
 * helper. No mutex; qatomic_add / qatomic_cmpxchg / qatomic_xchg are
 * wait-free on aarch64.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/xemu-apu-perf.h"

static uint64_t apu_perf_lock_hold_us_total;
static uint64_t apu_perf_vcpu_lock_wait_us_max;

void xemu_apu_perf_add_lock_hold_us(uint64_t us)
{
    if (us == 0) {
        return;
    }
    qatomic_add(&apu_perf_lock_hold_us_total, us);
}

void xemu_apu_perf_record_vcpu_wait_us(uint64_t us)
{
    if (us == 0) {
        return;
    }
    /* Lock-free max via CAS-loop. The vCPU wait happens from the single
     * Xbox vCPU thread under MTTCG plus the APU worker (which does
     * `qemu_mutex_lock(&d->lock)` for its own re-entries); contention
     * on this counter is rare. */
    uint64_t cur = qatomic_read(&apu_perf_vcpu_lock_wait_us_max);
    while (us > cur) {
        uint64_t prev = qatomic_cmpxchg(&apu_perf_vcpu_lock_wait_us_max,
                                        cur, us);
        if (prev == cur) {
            break;
        }
        cur = prev;
    }
}

void xemu_apu_perf_emit_and_reset(FILE *out)
{
    if (out == NULL) {
        return;
    }

    uint64_t hold_total = qatomic_xchg(&apu_perf_lock_hold_us_total, 0);
    uint64_t wait_max = qatomic_xchg(&apu_perf_vcpu_lock_wait_us_max, 0);

    if ((hold_total | wait_max) == 0) {
        return;
    }

    fprintf(out,
            " APU_LOCK_HOLD_US_TOTAL=%llu APU_VCPU_LOCK_WAIT_US_MAX=%llu",
            (unsigned long long)hold_total,
            (unsigned long long)wait_max);
}
