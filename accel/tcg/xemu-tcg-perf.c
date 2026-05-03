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
static uint64_t tcg_perf_xbox_idle_loop_hits;
static uint64_t tcg_perf_xbox_idle_loop_yields;
static uint64_t tcg_perf_xbox_idle_loop_halts;
static uint64_t tcg_perf_mmio_read_count;
static uint64_t tcg_perf_mmio_read_us_total;
static uint64_t tcg_perf_mmio_read_us_max;
static uint64_t tcg_perf_mmio_read_pgraph_count;
static uint64_t tcg_perf_mmio_read_pgraph_us_total;
static uint64_t tcg_perf_mmio_read_pgraph_us_max;

/* XEMU_STUTTER_TRACE=1: per-interval guest-PC chain histogram. This is
 * intentionally approximate and lock-free: the vCPU updates slots while
 * the NV2A perf emitter snapshots/resets them from another thread. A
 * racing sample may be attributed to the adjacent interval, which is fine
 * for stutter attribution because the slow-frame intervals are seconds
 * wide and the dominant chains are orders of magnitude larger than the
 * race window. */
#define TCG_CHAIN_HISTO_SIZE 4096
#define TCG_CHAIN_TOP_N 8
#define TCG_TB_ENTRY_HISTO_SIZE 4096
#define TCG_TB_ENTRY_TOP_N 8
#define TCG_MMIO_READ_HISTO_SIZE 512
#define TCG_MMIO_READ_TOP_N 8

typedef struct TCGChainHistoSlot {
    uint64_t pc;
    uint64_t chains;
    uint64_t total_us;
    uint64_t max_us;
    uint64_t tb_count;
} TCGChainHistoSlot;

static TCGChainHistoSlot tcg_chain_histo[TCG_CHAIN_HISTO_SIZE];

typedef struct TCGTBEntryHistoSlot {
    uint64_t pc;
    uint64_t execs;
    uint64_t guest_insns;
    uint64_t guest_bytes;
} TCGTBEntryHistoSlot;

static TCGTBEntryHistoSlot tcg_tb_entry_histo[TCG_TB_ENTRY_HISTO_SIZE];

typedef struct TCGMMIOReadHistoSlot {
    uint64_t addr;
    uint64_t mr_offset;
    uint64_t count;
    uint64_t total_us;
    uint64_t max_us;
    uint64_t last_value;
    uint64_t same_value_count;
    const char *mr_name;
    uint32_t size;
    uint32_t is_pgraph;
} TCGMMIOReadHistoSlot;

static TCGMMIOReadHistoSlot tcg_mmio_read_histo[TCG_MMIO_READ_HISTO_SIZE];

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

static void xemu_tcg_perf_update_max(uint64_t *dst, uint64_t value)
{
    uint64_t cur = qatomic_read(dst);
    while (value > cur) {
        uint64_t prev = qatomic_cmpxchg(dst, cur, value);
        if (prev == cur) {
            break;
        }
        cur = prev;
    }
}

static bool xemu_tcg_perf_mr_is_pgraph(const char *mr_name)
{
    return mr_name != NULL &&
           (strstr(mr_name, "PGRAPH") != NULL ||
            strstr(mr_name, "pgraph") != NULL);
}

void xemu_tcg_perf_record_mmio_read(uint64_t addr, uint64_t mr_offset,
                                    uint64_t value, uint32_t size,
                                    const char *mr_name, uint64_t wall_us)
{
    bool is_pgraph = xemu_tcg_perf_mr_is_pgraph(mr_name);

    qatomic_inc(&tcg_perf_mmio_read_count);
    qatomic_add(&tcg_perf_mmio_read_us_total, wall_us);
    xemu_tcg_perf_update_max(&tcg_perf_mmio_read_us_max, wall_us);
    if (is_pgraph) {
        qatomic_inc(&tcg_perf_mmio_read_pgraph_count);
        qatomic_add(&tcg_perf_mmio_read_pgraph_us_total, wall_us);
        xemu_tcg_perf_update_max(&tcg_perf_mmio_read_pgraph_us_max, wall_us);
    }

    uint64_t h = (addr >> 2) ^ (mr_offset >> 2) ^ (addr >> 16);
    for (unsigned i = 0; i < TCG_MMIO_READ_HISTO_SIZE; i++) {
        unsigned slot_idx = (h + i) & (TCG_MMIO_READ_HISTO_SIZE - 1);
        TCGMMIOReadHistoSlot *slot = &tcg_mmio_read_histo[slot_idx];
        uint64_t cur_addr = qatomic_read(&slot->addr);

        if (cur_addr == addr) {
            qatomic_inc(&slot->count);
            qatomic_add(&slot->total_us, wall_us);
            xemu_tcg_perf_update_max(&slot->max_us, wall_us);
            uint64_t previous = qatomic_xchg(&slot->last_value, value);
            if (previous == value) {
                qatomic_inc(&slot->same_value_count);
            }
            return;
        }

        if (cur_addr == 0 && qatomic_cmpxchg(&slot->addr, 0, addr) == 0) {
            qatomic_set(&slot->mr_offset, mr_offset);
            qatomic_set(&slot->count, 1);
            qatomic_set(&slot->total_us, wall_us);
            qatomic_set(&slot->max_us, wall_us);
            qatomic_set(&slot->last_value, value);
            qatomic_set(&slot->same_value_count, 0);
            slot->mr_name = mr_name;
            qatomic_set(&slot->size, size);
            qatomic_set(&slot->is_pgraph, is_pgraph ? 1 : 0);
            return;
        }
    }
}

void xemu_tcg_perf_record_chain(uint64_t first_pc, uint64_t wall_us,
                                uint32_t tb_count)
{
    if (first_pc == 0 || wall_us == 0) {
        return;
    }

    uint64_t h = (first_pc >> 4) ^ (first_pc >> 16) ^ (first_pc >> 28);
    for (unsigned i = 0; i < TCG_CHAIN_HISTO_SIZE; i++) {
        unsigned slot_idx = (h + i) & (TCG_CHAIN_HISTO_SIZE - 1);
        TCGChainHistoSlot *slot = &tcg_chain_histo[slot_idx];
        uint64_t cur_pc = qatomic_read(&slot->pc);

        if (cur_pc == first_pc) {
            qatomic_inc(&slot->chains);
            qatomic_add(&slot->total_us, wall_us);
            qatomic_add(&slot->tb_count, (uint64_t)tb_count);
            xemu_tcg_perf_update_max(&slot->max_us, wall_us);
            return;
        }

        if (cur_pc == 0 &&
            qatomic_cmpxchg(&slot->pc, 0, first_pc) == 0) {
            qatomic_set(&slot->chains, 1);
            qatomic_set(&slot->total_us, wall_us);
            qatomic_set(&slot->tb_count, (uint64_t)tb_count);
            qatomic_set(&slot->max_us, wall_us);
            return;
        }
    }
}

void xemu_tcg_perf_record_tb_entry(uint64_t pc, uint32_t icount,
                                   uint32_t guest_size)
{
    if (pc == 0) {
        return;
    }

    uint64_t h = (pc >> 4) ^ (pc >> 16) ^ (pc >> 28);
    for (unsigned i = 0; i < TCG_TB_ENTRY_HISTO_SIZE; i++) {
        unsigned slot_idx = (h + i) & (TCG_TB_ENTRY_HISTO_SIZE - 1);
        TCGTBEntryHistoSlot *slot = &tcg_tb_entry_histo[slot_idx];
        uint64_t cur_pc = qatomic_read(&slot->pc);

        if (cur_pc == pc) {
            qatomic_inc(&slot->execs);
            qatomic_add(&slot->guest_insns, (uint64_t)icount);
            qatomic_add(&slot->guest_bytes, (uint64_t)guest_size);
            return;
        }

        if (cur_pc == 0 && qatomic_cmpxchg(&slot->pc, 0, pc) == 0) {
            qatomic_set(&slot->execs, 1);
            qatomic_set(&slot->guest_insns, (uint64_t)icount);
            qatomic_set(&slot->guest_bytes, (uint64_t)guest_size);
            return;
        }
    }
}

void xemu_tcg_perf_record_xbox_idle_loop(bool yielded, bool halted)
{
    qatomic_inc(&tcg_perf_xbox_idle_loop_hits);
    if (yielded) {
        qatomic_inc(&tcg_perf_xbox_idle_loop_yields);
    }
    if (halted) {
        qatomic_inc(&tcg_perf_xbox_idle_loop_halts);
    }
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
    uint64_t xbox_idle_hits =
        qatomic_xchg(&tcg_perf_xbox_idle_loop_hits, 0);
    uint64_t xbox_idle_yields =
        qatomic_xchg(&tcg_perf_xbox_idle_loop_yields, 0);
    uint64_t xbox_idle_halts =
        qatomic_xchg(&tcg_perf_xbox_idle_loop_halts, 0);
    uint64_t mmio_read_count =
        qatomic_xchg(&tcg_perf_mmio_read_count, 0);
    uint64_t mmio_read_us_total =
        qatomic_xchg(&tcg_perf_mmio_read_us_total, 0);
    uint64_t mmio_read_us_max =
        qatomic_xchg(&tcg_perf_mmio_read_us_max, 0);
    uint64_t mmio_read_pgraph_count =
        qatomic_xchg(&tcg_perf_mmio_read_pgraph_count, 0);
    uint64_t mmio_read_pgraph_us_total =
        qatomic_xchg(&tcg_perf_mmio_read_pgraph_us_total, 0);
    uint64_t mmio_read_pgraph_us_max =
        qatomic_xchg(&tcg_perf_mmio_read_pgraph_us_max, 0);
    uint64_t lookup_us = lookup_ns / 1000;
    uint64_t gen_us = gen_ns / 1000;
    uint64_t int_us = int_ns / 1000;
    struct {
        uint64_t pc;
        uint64_t chains;
        uint64_t total_us;
        uint64_t max_us;
        uint64_t tb_count;
    } top[TCG_CHAIN_TOP_N] = { 0 };
    struct {
        uint64_t pc;
        uint64_t execs;
        uint64_t guest_insns;
        uint64_t guest_bytes;
    } tb_top[TCG_TB_ENTRY_TOP_N] = { 0 };
    struct {
        uint64_t addr;
        uint64_t mr_offset;
        uint64_t count;
        uint64_t total_us;
        uint64_t max_us;
        uint64_t last_value;
        uint64_t same_value_count;
        const char *mr_name;
        uint32_t size;
        uint32_t is_pgraph;
    } mmio_top[TCG_MMIO_READ_TOP_N] = { 0 };

    /* Reset the page set after sampling. Ordering vs concurrent
     * inserts: a vCPU increment racing this loop may have its page
     * cleared; that page reappears next interval. Acceptable for an
     * approximation. */
    for (unsigned i = 0; i < TCG_PERF_NOTDIRTY_PAGE_SET_SIZE; i++) {
        qatomic_set(&tcg_perf_notdirty_pages[i], 0);
    }

    for (unsigned i = 0; i < TCG_CHAIN_HISTO_SIZE; i++) {
        TCGChainHistoSlot *slot = &tcg_chain_histo[i];
        uint64_t pc = qatomic_read(&slot->pc);
        if (pc == 0) {
            continue;
        }

        uint64_t chains = qatomic_xchg(&slot->chains, 0);
        uint64_t total_us = qatomic_xchg(&slot->total_us, 0);
        uint64_t max_us = qatomic_xchg(&slot->max_us, 0);
        uint64_t tb_count = qatomic_xchg(&slot->tb_count, 0);
        qatomic_set(&slot->pc, 0);

        if (total_us == 0) {
            continue;
        }

        for (unsigned j = 0; j < TCG_CHAIN_TOP_N; j++) {
            if (total_us > top[j].total_us) {
                for (unsigned k = TCG_CHAIN_TOP_N - 1; k > j; k--) {
                    top[k] = top[k - 1];
                }
                top[j].pc = pc;
                top[j].chains = chains;
                top[j].total_us = total_us;
                top[j].max_us = max_us;
                top[j].tb_count = tb_count;
                break;
            }
        }
    }

    for (unsigned i = 0; i < TCG_TB_ENTRY_HISTO_SIZE; i++) {
        TCGTBEntryHistoSlot *slot = &tcg_tb_entry_histo[i];
        uint64_t pc = qatomic_read(&slot->pc);
        if (pc == 0) {
            continue;
        }

        uint64_t execs = qatomic_xchg(&slot->execs, 0);
        uint64_t guest_insns = qatomic_xchg(&slot->guest_insns, 0);
        uint64_t guest_bytes = qatomic_xchg(&slot->guest_bytes, 0);
        qatomic_set(&slot->pc, 0);

        if (execs == 0) {
            continue;
        }

        for (unsigned j = 0; j < TCG_TB_ENTRY_TOP_N; j++) {
            if (guest_insns > tb_top[j].guest_insns) {
                for (unsigned k = TCG_TB_ENTRY_TOP_N - 1; k > j; k--) {
                    tb_top[k] = tb_top[k - 1];
                }
                tb_top[j].pc = pc;
                tb_top[j].execs = execs;
                tb_top[j].guest_insns = guest_insns;
                tb_top[j].guest_bytes = guest_bytes;
                break;
            }
        }
    }

    for (unsigned i = 0; i < TCG_MMIO_READ_HISTO_SIZE; i++) {
        TCGMMIOReadHistoSlot *slot = &tcg_mmio_read_histo[i];
        uint64_t addr = qatomic_read(&slot->addr);
        if (addr == 0) {
            continue;
        }

        uint64_t mr_offset = qatomic_xchg(&slot->mr_offset, 0);
        uint64_t count = qatomic_xchg(&slot->count, 0);
        uint64_t total_us = qatomic_xchg(&slot->total_us, 0);
        uint64_t max_us = qatomic_xchg(&slot->max_us, 0);
        uint64_t last_value = qatomic_xchg(&slot->last_value, 0);
        uint64_t same_value_count = qatomic_xchg(&slot->same_value_count, 0);
        const char *mr_name = slot->mr_name ? slot->mr_name : "?";
        uint32_t size = qatomic_xchg(&slot->size, 0);
        uint32_t is_pgraph = qatomic_xchg(&slot->is_pgraph, 0);
        slot->mr_name = NULL;
        qatomic_set(&slot->addr, 0);

        if (count == 0) {
            continue;
        }

        for (unsigned j = 0; j < TCG_MMIO_READ_TOP_N; j++) {
            if (count > mmio_top[j].count) {
                for (unsigned k = TCG_MMIO_READ_TOP_N - 1; k > j; k--) {
                    mmio_top[k] = mmio_top[k - 1];
                }
                mmio_top[j].addr = addr;
                mmio_top[j].mr_offset = mr_offset;
                mmio_top[j].count = count;
                mmio_top[j].total_us = total_us;
                mmio_top[j].max_us = max_us;
                mmio_top[j].last_value = last_value;
                mmio_top[j].same_value_count = same_value_count;
                mmio_top[j].mr_name = mr_name;
                mmio_top[j].size = size;
                mmio_top[j].is_pgraph = is_pgraph;
                break;
            }
        }
    }

    if ((tb_exec | tb_inv | notdirty_trips | notdirty_pages | burst_max
         | jmp_zeroed | inv_wall_us_max | inv_wall_us_total
         | lookup_us | gen_us | int_us | top[0].total_us
         | tb_top[0].guest_insns | xbox_idle_hits
         | xbox_idle_yields | xbox_idle_halts | mmio_read_count
         | mmio_read_us_total | mmio_read_us_max
         | mmio_read_pgraph_count | mmio_read_pgraph_us_total
         | mmio_read_pgraph_us_max | mmio_top[0].count) == 0) {
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
            " TCG_HANDLE_INTERRUPT_US_TOTAL=%llu"
            " TCG_XBOX_IDLE_LOOP_HITS=%llu"
            " TCG_XBOX_IDLE_LOOP_YIELDS=%llu"
            " TCG_XBOX_IDLE_LOOP_HALTS=%llu"
            " MMIO_READ_COUNT=%llu"
            " MMIO_READ_US_TOTAL=%llu"
            " MMIO_READ_US_MAX=%llu"
            " MMIO_READ_PGRAPH_COUNT=%llu"
            " MMIO_READ_PGRAPH_US_TOTAL=%llu"
            " MMIO_READ_PGRAPH_US_MAX=%llu",
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
            (unsigned long long)int_us,
            (unsigned long long)xbox_idle_hits,
            (unsigned long long)xbox_idle_yields,
            (unsigned long long)xbox_idle_halts,
            (unsigned long long)mmio_read_count,
            (unsigned long long)mmio_read_us_total,
            (unsigned long long)mmio_read_us_max,
            (unsigned long long)mmio_read_pgraph_count,
            (unsigned long long)mmio_read_pgraph_us_total,
            (unsigned long long)mmio_read_pgraph_us_max);

    if (top[0].total_us != 0) {
        fprintf(out, " TCG_CHAIN_TOP=");
        for (unsigned i = 0; i < TCG_CHAIN_TOP_N && top[i].total_us != 0; i++) {
            fprintf(out, "%s0x%llx:%llu:%llu:%llu:%llu",
                    i == 0 ? "" : ";",
                    (unsigned long long)top[i].pc,
                    (unsigned long long)top[i].chains,
                    (unsigned long long)top[i].total_us,
                    (unsigned long long)top[i].max_us,
                    (unsigned long long)top[i].tb_count);
        }
    }

    if (tb_top[0].guest_insns != 0) {
        fprintf(out, " TCG_TB_TOP=");
        for (unsigned i = 0;
             i < TCG_TB_ENTRY_TOP_N && tb_top[i].guest_insns != 0; i++) {
            fprintf(out, "%s0x%llx:%llu:%llu:%llu",
                    i == 0 ? "" : ";",
                    (unsigned long long)tb_top[i].pc,
                    (unsigned long long)tb_top[i].execs,
                    (unsigned long long)tb_top[i].guest_insns,
                    (unsigned long long)tb_top[i].guest_bytes);
        }
    }

    if (mmio_top[0].count != 0) {
        fprintf(out, " MMIO_READ_TOP=");
        for (unsigned i = 0;
             i < TCG_MMIO_READ_TOP_N && mmio_top[i].count != 0; i++) {
            fprintf(out, "%s0x%llx@0x%llx:%u:%u:%llu:%llu:%llu:0x%llx:%llu:%s",
                    i == 0 ? "" : ";",
                    (unsigned long long)mmio_top[i].addr,
                    (unsigned long long)mmio_top[i].mr_offset,
                    mmio_top[i].size,
                    mmio_top[i].is_pgraph,
                    (unsigned long long)mmio_top[i].count,
                    (unsigned long long)mmio_top[i].total_us,
                    (unsigned long long)mmio_top[i].max_us,
                    (unsigned long long)mmio_top[i].last_value,
                    (unsigned long long)mmio_top[i].same_value_count,
                    mmio_top[i].mr_name ? mmio_top[i].mr_name : "?");
        }
    }
}
