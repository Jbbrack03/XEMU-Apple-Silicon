/*
 * Apple Silicon performance fork: TCG hot-path counters.
 *
 * Increments are atomic (qatomic_inc/qatomic_set) and wait-free; the
 * snapshot/reset path is called from the NV2A renderer's interval
 * flush (single owner) and uses qatomic_xchg so concurrent vCPU
 * increments are safely observed exactly once. The unique-pages-touched
 * set is a small open-addressed lossy table — under contention or when
 * full it overcounts (page already present probes find an empty slot
 * elsewhere); on the cold steady state it is exact. We accept the
 * imprecision because the goal is order-of-magnitude attribution, not
 * exact accounting, and the hot path must not take a mutex.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "qemu/xemu-tcg-perf.h"
#include "qemu/xemu-spike-log.h"

#include <stdio.h>

/* Counter storage. Use uint64_t with qatomic ops for portability. */
static uint64_t tcg_perf_tb_exec_count;
static uint64_t tcg_perf_tb_invalidate_count;
static uint64_t tcg_perf_notdirty_trips;
static uint64_t tcg_perf_invalidate_burst_max;

/* I2 counters: per-interval sum of jmp-cache buckets cleared, and the
 * per-interval MAX wallclock cost (us) of a single
 * tb_invalidate_phys_page_range__locked call. The first lets V2 compute
 * the achieved reduction ratio (4096 per full-zero call vs 1 per
 * targeted-bucket clear). The second decides whether the headline
 * 1.35-second worst-frame is one giant invalidation chain or many
 * small ones. */
static uint64_t tcg_perf_jmp_cache_zeroed_buckets;
static uint64_t tcg_perf_invalidate_wall_us_max;
static uint64_t tcg_perf_invalidate_wall_us_total;

/* V7 counters: per-interval sum of cpu_exec_loop per-phase wallclock,
 * accumulated in *nanoseconds* and emitted as microseconds. V6
 * disproved per-event 1 ms+ dominance for these three phases; V7
 * tracks cumulative cost to catch the sub-millisecond aggregate that
 * V6's per-event spike threshold misses. Nanosecond accumulation
 * avoids sub-µs per-call truncation when thousands of fast calls
 * accumulate. */
static uint64_t tcg_perf_tb_lookup_ns_total;
static uint64_t tcg_perf_tb_gen_code_ns_total;
static uint64_t tcg_perf_handle_interrupt_ns_total;

/* Open-addressing set of (page-aligned) ram_addr_t. 64 entries is
 * plenty for an interval-bounded unique-page approximation; if more
 * than 64 distinct pages trip notdirty per interval the set becomes
 * lossy, which is fine — we only need order-of-magnitude attribution
 * to know whether the slice cleared the bottleneck. */
#define TCG_PERF_NOTDIRTY_PAGE_SET_SIZE 64
static uint64_t tcg_perf_notdirty_pages[TCG_PERF_NOTDIRTY_PAGE_SET_SIZE];
static uint64_t tcg_perf_notdirty_pages_hit;

void xemu_tcg_perf_inc_tb_exec(void)
{
    qatomic_inc(&tcg_perf_tb_exec_count);
}

void xemu_tcg_perf_inc_tb_invalidate(void)
{
    qatomic_inc(&tcg_perf_tb_invalidate_count);
}

void xemu_tcg_perf_record_invalidate_burst(uint32_t count)
{
    if (count == 0) {
        return;
    }
    /* Lock-free max via CAS-loop. The burst path runs from any vCPU
     * thread that triggers SMC; contention is rare. */
    uint64_t cur = qatomic_read(&tcg_perf_invalidate_burst_max);
    while (count > cur) {
        uint64_t prev = qatomic_cmpxchg(&tcg_perf_invalidate_burst_max,
                                        cur, (uint64_t)count);
        if (prev == cur) {
            break;
        }
        cur = prev;
    }
}

void xemu_tcg_perf_add_jmp_cache_zeroed(uint32_t buckets)
{
    if (buckets == 0) {
        return;
    }
    qatomic_add(&tcg_perf_jmp_cache_zeroed_buckets, (uint64_t)buckets);
}

void xemu_tcg_perf_add_tb_lookup_ns(uint64_t ns)
{
    if (ns == 0) {
        return;
    }
    qatomic_add(&tcg_perf_tb_lookup_ns_total, ns);
}

void xemu_tcg_perf_add_tb_gen_code_ns(uint64_t ns)
{
    if (ns == 0) {
        return;
    }
    qatomic_add(&tcg_perf_tb_gen_code_ns_total, ns);
}

void xemu_tcg_perf_add_handle_interrupt_ns(uint64_t ns)
{
    if (ns == 0) {
        return;
    }
    qatomic_add(&tcg_perf_handle_interrupt_ns_total, ns);
}

void xemu_tcg_perf_record_invalidate_wall_us(uint64_t us)
{
    /* V10: per-interval SUM of invalidation wall time. A worst-frame
     * interval with TCG_TB_INVALIDATE_COUNT=8954 + average call cost
     * 100 µs would sum to 900 ms — directly attributing the headline
     * 1.3 s class stutter to the invalidation chain itself rather than
     * to translation churn (V7 disproved tb_gen_code dominance) or
     * RDTSC overhead (V9 confirmed RDTSC-quiet during stall). */
    qatomic_add(&tcg_perf_invalidate_wall_us_total, us);

    /* V2: lock-free max via CAS-loop. Contention is rare (the SMC chain
     * runs from any vCPU thread that triggers a notdirty trap; multiple
     * vCPUs collide only when several pages happen to be invalidated
     * within nanoseconds of each other). */
    uint64_t cur = qatomic_read(&tcg_perf_invalidate_wall_us_max);
    while (us > cur) {
        uint64_t prev = qatomic_cmpxchg(&tcg_perf_invalidate_wall_us_max,
                                        cur, us);
        if (prev == cur) {
            break;
        }
        cur = prev;
    }
}

void xemu_tcg_perf_notdirty_trip(ram_addr_t page_addr)
{
    qatomic_inc(&tcg_perf_notdirty_trips);

    /* Lossy linear-probe insertion. Hash ram_addr down to set bucket. */
    uint64_t key = (uint64_t)page_addr;
    if (key == 0) {
        /* 0 is the sentinel for "empty" in the set; avoid recording
         * page 0 to keep the empty-slot test simple. */
        return;
    }
    uint64_t h = (key >> 12) ^ (key >> 22);
    for (unsigned i = 0; i < TCG_PERF_NOTDIRTY_PAGE_SET_SIZE; i++) {
        unsigned slot = (h + i) & (TCG_PERF_NOTDIRTY_PAGE_SET_SIZE - 1);
        uint64_t cur = qatomic_read(&tcg_perf_notdirty_pages[slot]);
        if (cur == key) {
            return;
        }
        if (cur == 0) {
            uint64_t prev = qatomic_cmpxchg(&tcg_perf_notdirty_pages[slot],
                                            0, key);
            if (prev == 0) {
                qatomic_inc(&tcg_perf_notdirty_pages_hit);
                return;
            }
            if (prev == key) {
                return;
            }
            /* slot was claimed by a different page; keep probing */
        }
    }
    /* Set is full. Count as a hit anyway so the metric reflects
     * pressure rather than silently dropping; analysis docs explain
     * the saturation behavior. */
    qatomic_inc(&tcg_perf_notdirty_pages_hit);
}

/*
 * V3 attribution: sliding 1-second-window rate detectors for
 * SMC notdirty trips and x87 helper calls. Each detector keeps an
 * atomic event count plus a window-start nanosecond timestamp. On
 * every tick, the count is incremented; if the window has elapsed,
 * the count is reset and (if it exceeded the storm threshold) a
 * spike line is emitted.
 *
 * The window-close test races between vCPU threads but the cost is
 * always either "read two atomics + branch" (steady state, window
 * still open) or, very rarely, the window-roll path. The CAS on
 * the window-start timestamp serializes window resets so only one
 * thread emits the spike per window.
 */

#define XEMU_TCG_STORM_WINDOW_NS    1000000000LL  /* 1 s */
#define XEMU_TCG_NOTDIRTY_STORM_THRESHOLD  100000U   /* trips/s */
#define XEMU_TCG_X87_STORM_THRESHOLD      50000000U  /* helper calls/s */

typedef struct {
    uint64_t count;
    int64_t window_start_ns;
} StormDetector;

static StormDetector notdirty_storm;
static StormDetector x87_storm;

static void storm_detector_tick(StormDetector *d, uint32_t threshold,
                                const char *op_name)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    int64_t window_start = qatomic_read(&d->window_start_ns);

    if (window_start == 0) {
        /* First-ever tick: try to claim the window-start slot. If the
         * CAS loses, another thread initialised it; fall through. */
        if (qatomic_cmpxchg(&d->window_start_ns, 0, now_ns) == 0) {
            window_start = now_ns;
        } else {
            window_start = qatomic_read(&d->window_start_ns);
        }
    }

    uint64_t cur = qatomic_inc_fetch(&d->count);

    /* Window-close test. Only one thread should emit + reset. */
    if (now_ns - window_start >= XEMU_TCG_STORM_WINDOW_NS) {
        /* Try to claim the window roll. If CAS wins, we own the emit
         * + reset. */
        if (qatomic_cmpxchg(&d->window_start_ns, window_start, now_ns)
            == window_start) {
            uint64_t observed = qatomic_xchg(&d->count, 0);
            if (observed >= threshold) {
                /* Compute rate per second. now_ns - window_start is in
                 * [1e9, ~3e9] nominally; cap at 10s to keep the int
                 * arithmetic safe. */
                int64_t window_ns = now_ns - window_start;
                if (window_ns < 1000000) {
                    window_ns = 1000000;
                }
                uint64_t rate = (observed * 1000000000ULL) /
                                (uint64_t)window_ns;
                char extra[80];
                snprintf(extra, sizeof(extra),
                         "events=%llu rate_per_s=%llu",
                         (unsigned long long)observed,
                         (unsigned long long)rate);
                /* Use window_ns (in us) as the spike's "duration_us" so
                 * the timeline correlation logic ("which spikes fired
                 * inside the worst frame") still works on now_us. */
                xemu_spike_emit(op_name, window_ns / 1000, extra);
            }
        }
        /* If we lost the CAS another thread will close the window;
         * our increment is preserved in the next window. */
    }

    (void)cur;
}

void xemu_tcg_perf_notdirty_storm_tick(void)
{
    storm_detector_tick(&notdirty_storm,
                        XEMU_TCG_NOTDIRTY_STORM_THRESHOLD,
                        "tcg_notdirty_storm");
}

void xemu_tcg_perf_x87_storm_tick(void)
{
    storm_detector_tick(&x87_storm,
                        XEMU_TCG_X87_STORM_THRESHOLD,
                        "tcg_x87_storm");
}

void xemu_tcg_perf_emit_and_reset(FILE *out)
{
    if (out == NULL) {
        return;
    }

    uint64_t tb_exec = qatomic_xchg(&tcg_perf_tb_exec_count, 0);
    uint64_t tb_inv = qatomic_xchg(&tcg_perf_tb_invalidate_count, 0);
    uint64_t notdirty_trips = qatomic_xchg(&tcg_perf_notdirty_trips, 0);
    uint64_t notdirty_pages = qatomic_xchg(&tcg_perf_notdirty_pages_hit, 0);
    uint64_t burst_max = qatomic_xchg(&tcg_perf_invalidate_burst_max, 0);
    uint64_t jmp_zeroed = qatomic_xchg(&tcg_perf_jmp_cache_zeroed_buckets, 0);
    uint64_t inv_wall_us_max = qatomic_xchg(&tcg_perf_invalidate_wall_us_max, 0);
    uint64_t inv_wall_us_total =
        qatomic_xchg(&tcg_perf_invalidate_wall_us_total, 0);
    uint64_t lookup_ns = qatomic_xchg(&tcg_perf_tb_lookup_ns_total, 0);
    uint64_t gen_ns = qatomic_xchg(&tcg_perf_tb_gen_code_ns_total, 0);
    uint64_t int_ns = qatomic_xchg(&tcg_perf_handle_interrupt_ns_total, 0);
    uint64_t lookup_us = lookup_ns / 1000;
    uint64_t gen_us = gen_ns / 1000;
    uint64_t int_us = int_ns / 1000;

    /* Reset the page set after sampling. Ordering vs concurrent
     * inserts: a vCPU increment racing this loop may have its page
     * cleared; that page reappears next interval. Acceptable for an
     * approximation. */
    for (unsigned i = 0; i < TCG_PERF_NOTDIRTY_PAGE_SET_SIZE; i++) {
        qatomic_set(&tcg_perf_notdirty_pages[i], 0);
    }

    if ((tb_exec | tb_inv | notdirty_trips | notdirty_pages | burst_max
         | jmp_zeroed | inv_wall_us_max | inv_wall_us_total
         | lookup_us | gen_us | int_us) == 0) {
        return;
    }

    fprintf(out,
            " TCG_TB_EXEC_COUNT=%llu TCG_TB_INVALIDATE_COUNT=%llu"
            " TCG_NOTDIRTY_TRIPS=%llu TCG_NOTDIRTY_PAGES_HIT=%llu"
            " TCG_TB_INVALIDATE_BURST_MAX=%llu"
            " TCG_JMP_CACHE_ZEROED_BUCKETS=%llu"
            " TCG_INVALIDATE_WALL_US_MAX=%llu"
            " TCG_INVALIDATE_WALL_US_TOTAL=%llu"
            " TCG_TB_LOOKUP_US_TOTAL=%llu"
            " TCG_TB_GEN_CODE_US_TOTAL=%llu"
            " TCG_HANDLE_INTERRUPT_US_TOTAL=%llu",
            (unsigned long long)tb_exec,
            (unsigned long long)tb_inv,
            (unsigned long long)notdirty_trips,
            (unsigned long long)notdirty_pages,
            (unsigned long long)burst_max,
            (unsigned long long)jmp_zeroed,
            (unsigned long long)inv_wall_us_max,
            (unsigned long long)inv_wall_us_total,
            (unsigned long long)lookup_us,
            (unsigned long long)gen_us,
            (unsigned long long)int_us);
}
