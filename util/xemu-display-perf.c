/*
 * Apple Silicon performance fork: display / vblank / present counters.
 *
 * See include/qemu/xemu-display-perf.h for the API contract. The
 * implementation is four atomic counters and a snapshot/reset helper.
 * No mutex; qatomic_inc and qatomic_xchg are wait-free on aarch64.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/xemu-display-perf.h"
#include "qemu/xemu-ide-perf.h"
#include "qemu/xemu-pfifo-perf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t display_perf_vblank_fires;
static uint64_t display_perf_flip_stall_writes;
static uint64_t display_perf_present_heartbeat;
static uint64_t display_perf_gl_swaps;

void xemu_display_perf_vblank_fired(void)
{
    qatomic_inc(&display_perf_vblank_fires);
}

void xemu_display_perf_flip_stall(void)
{
    qatomic_inc(&display_perf_flip_stall_writes);
}

void xemu_display_perf_present(void)
{
    qatomic_inc(&display_perf_present_heartbeat);
}

void xemu_display_perf_gl_swap(void)
{
    qatomic_inc(&display_perf_gl_swaps);
}

/* Apple Silicon performance fork — slice F1 (2026-05-04):
 * "Nth flip_stall since process start" capture trigger.
 *
 * State:
 *   s_capture_target          set once at init from XEMU_CAPTURE_AT_FLIP_STALL.
 *                             0 means "disabled"; non-zero is the 1-indexed
 *                             flip_stall ordinal at which to arm.
 *   s_capture_count           monotonic count of flip_stalls seen since
 *                             process start; bumped once per
 *                             xemu_capture_at_flip_stall_tick.
 *   s_capture_armed           one-shot atomic flag (0/1): set when
 *                             count == target, cleared when consume()
 *                             is called.
 *   s_capture_sentinel_path   optional absolute path; touched once when armed.
 *
 * The init function reads the env once and is safe to call multiple times
 * (first call wins via s_capture_initialized). All counter mutations use
 * qatomic_* so the FLIP_STALL handler (vCPU thread) and the renderer
 * (rendering thread) never race. */
static uint32_t  s_capture_initialized;
static uint64_t  s_capture_target;            /* 0 = disabled */
static char     *s_capture_sentinel_path;     /* malloc'd; NULL = no sentinel */
static uint64_t  s_capture_count;
static uint32_t  s_capture_armed;             /* one-shot 0/1 */
static uint32_t  s_capture_sentinel_touched;  /* one-shot 0/1 */

static void xemu_capture_at_flip_stall_touch_sentinel(void)
{
    if (s_capture_sentinel_path == NULL) {
        return;
    }
    if (qatomic_cmpxchg(&s_capture_sentinel_touched, 0u, 1u) != 0u) {
        return;
    }
    /* O_CREAT | O_EXCL avoids a stale-sentinel ambiguity if the path
     * already existed when we started; we treat that as a configuration
     * error and log it but continue (the renderer-side arm path is the
     * source of truth for the Metal leg). The macos-capture.sh poller
     * is documented to expect the sentinel to NOT exist at run start. */
    int fd = open(s_capture_sentinel_path,
                  O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            fprintf(stderr,
                    "xemu-perf: capture_at_flip_stall sentinel already "
                    "exists path=%s — treating as armed; clean up the "
                    "file before the next run\n",
                    s_capture_sentinel_path);
        } else {
            fprintf(stderr,
                    "xemu-perf: capture_at_flip_stall sentinel open "
                    "failed path=%s errno=%d (%s)\n",
                    s_capture_sentinel_path, errno, strerror(errno));
        }
        return;
    }
    close(fd);
}

void xemu_capture_at_flip_stall_init(void)
{
    if (qatomic_cmpxchg(&s_capture_initialized, 0u, 1u) != 0u) {
        return;
    }

    const char *target_env = getenv("XEMU_CAPTURE_AT_FLIP_STALL");
    uint64_t target = 0;
    if (target_env != NULL && target_env[0] != '\0') {
        char *endp = NULL;
        unsigned long long n = strtoull(target_env, &endp, 10);
        if (endp != NULL && *endp == '\0') {
            target = (uint64_t)n;
        } else {
            fprintf(stderr,
                    "xemu-perf: capture_at_flip_stall ignoring malformed "
                    "XEMU_CAPTURE_AT_FLIP_STALL=%s (expected non-negative "
                    "integer)\n",
                    target_env);
        }
    }
    s_capture_target = target;

    const char *sentinel_env = getenv("XEMU_CAPTURE_FLIP_STALL_SENTINEL");
    if (sentinel_env != NULL && sentinel_env[0] != '\0') {
        s_capture_sentinel_path = strdup(sentinel_env);
    }

    if (target > 0) {
        fprintf(stderr,
                "xemu-perf: capture_at_flip_stall target=%llu sentinel=%s\n",
                (unsigned long long)target,
                s_capture_sentinel_path ? s_capture_sentinel_path : "(none)");
    }
}

void xemu_capture_at_flip_stall_tick(void)
{
    /* Lazy init on first tick. The CAS in init() ensures this is a
     * one-shot; subsequent ticks pay only a single qatomic_read. The
     * FLIP_STALL handler is the only caller in the hot path, so this
     * is a per-frame cost at worst, not a per-draw cost. */
    if (qatomic_read(&s_capture_initialized) == 0u) {
        xemu_capture_at_flip_stall_init();
    }
    /* Hot-path ordering:
     *   1. Bump the monotonic count unconditionally — the value surfaces
     *      via xemu_capture_at_flip_stall_count() for diagnostic logging
     *      and is cheap regardless of whether the trigger is armed.
     *   2. If a target is set and we've now reached it, arm one-shot.
     *      target == 0 disables the trigger entirely (the env was unset
     *      or evaluated to 0); the comparison below short-circuits.
     */
    uint64_t target = s_capture_target;
    uint64_t cur = qatomic_fetch_inc(&s_capture_count) + 1;
    if (target == 0 || cur != target) {
        return;
    }
    /* CAS the armed flag so we only ever arm once; the renderer's
     * consume() also CAS-clears so any spurious extra arm at a higher
     * ordinal becomes a no-op. */
    if (qatomic_cmpxchg(&s_capture_armed, 0u, 1u) == 0u) {
        fprintf(stderr,
                "xemu-perf: capture_at_flip_stall fired ordinal=%llu\n",
                (unsigned long long)cur);
        xemu_capture_at_flip_stall_touch_sentinel();
    }
}

bool xemu_capture_at_flip_stall_consume(void)
{
    return qatomic_cmpxchg(&s_capture_armed, 1u, 0u) == 1u;
}

unsigned long long xemu_capture_at_flip_stall_count(void)
{
    return (unsigned long long)qatomic_read(&s_capture_count);
}

unsigned long long xemu_capture_at_flip_stall_target(void)
{
    return (unsigned long long)s_capture_target;
}

void xemu_display_perf_emit_and_reset(FILE *out)
{
    if (out == NULL) {
        return;
    }

    uint64_t vblank = qatomic_xchg(&display_perf_vblank_fires, 0);
    uint64_t flip_stall = qatomic_xchg(&display_perf_flip_stall_writes, 0);
    uint64_t present = qatomic_xchg(&display_perf_present_heartbeat, 0);
    uint64_t gl_swap = qatomic_xchg(&display_perf_gl_swaps, 0);

    if ((vblank | flip_stall | present | gl_swap) == 0) {
        return;
    }

    fprintf(out,
            " NV2A_VBLANK_FIRES=%llu NV2A_FLIP_STALL_WRITES=%llu"
            " NV2A_PRESENT_HEARTBEAT=%llu XEMU_GL_SWAPS=%llu",
            (unsigned long long)vblank,
            (unsigned long long)flip_stall,
            (unsigned long long)present,
            (unsigned long long)gl_swap);
}

static uint64_t pfifo_perf_pusher_runs;
static uint64_t pfifo_perf_pusher_stalls;
static uint64_t pfifo_perf_pusher_stall_flip;
static uint64_t pfifo_perf_pusher_stall_nop;
static uint64_t pfifo_perf_pusher_stall_fifo_access;
static uint64_t pfifo_perf_puller_calls;
static uint64_t pfifo_perf_puller_stalls;
static uint64_t pfifo_perf_puller_stall_flip;
static uint64_t pfifo_perf_puller_stall_nop;
static uint64_t pfifo_perf_puller_stall_context;
static uint64_t pfifo_perf_puller_stall_fifo_access;
static uint64_t pfifo_perf_pusher_words;
static uint64_t pfifo_perf_puller_method_words;
static uint64_t pfifo_perf_dma_backlog_bytes_max;
static uint64_t pfifo_perf_user_dma_put_writes;
static uint64_t pfifo_perf_user_dma_get_writes;
static uint64_t pfifo_perf_user_dma_put_reads;
static uint64_t pfifo_perf_user_dma_get_reads;
static uint64_t pfifo_perf_patt_color0_writes;
static uint64_t pfifo_perf_patt_color0_reads;
static uint64_t pfifo_perf_flip_stall_sets;
static uint64_t pfifo_perf_flip_stall_checks;
static uint64_t pfifo_perf_flip_stall_still_waiting;
static uint64_t pfifo_perf_flip_stall_completes;

static void pfifo_perf_max(uint64_t *counter, uint64_t value)
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

void xemu_pfifo_perf_record_pusher_run(void)
{
    qatomic_inc(&pfifo_perf_pusher_runs);
}

void xemu_pfifo_perf_record_pusher_stall(uint32_t reasons)
{
    qatomic_inc(&pfifo_perf_pusher_stalls);
    if (reasons & XEMU_PFIFO_STALL_FLIP) {
        qatomic_inc(&pfifo_perf_pusher_stall_flip);
    }
    if (reasons & XEMU_PFIFO_STALL_NOP) {
        qatomic_inc(&pfifo_perf_pusher_stall_nop);
    }
    if (reasons & XEMU_PFIFO_STALL_FIFO_ACCESS) {
        qatomic_inc(&pfifo_perf_pusher_stall_fifo_access);
    }
}

void xemu_pfifo_perf_record_puller_call(void)
{
    qatomic_inc(&pfifo_perf_puller_calls);
}

void xemu_pfifo_perf_record_puller_stall(uint32_t reasons)
{
    qatomic_inc(&pfifo_perf_puller_stalls);
    if (reasons & XEMU_PFIFO_STALL_FLIP) {
        qatomic_inc(&pfifo_perf_puller_stall_flip);
    }
    if (reasons & XEMU_PFIFO_STALL_NOP) {
        qatomic_inc(&pfifo_perf_puller_stall_nop);
    }
    if (reasons & XEMU_PFIFO_STALL_CONTEXT) {
        qatomic_inc(&pfifo_perf_puller_stall_context);
    }
    if (reasons & XEMU_PFIFO_STALL_FIFO_ACCESS) {
        qatomic_inc(&pfifo_perf_puller_stall_fifo_access);
    }
}

void xemu_pfifo_perf_add_pusher_words(uint64_t words)
{
    qatomic_add(&pfifo_perf_pusher_words, words);
}

void xemu_pfifo_perf_add_puller_method_words(uint64_t words)
{
    qatomic_add(&pfifo_perf_puller_method_words, words);
}

void xemu_pfifo_perf_sample_dma_backlog(uint64_t bytes)
{
    pfifo_perf_max(&pfifo_perf_dma_backlog_bytes_max, bytes);
}

void xemu_pfifo_perf_record_user_dma_put_write(void)
{
    qatomic_inc(&pfifo_perf_user_dma_put_writes);
}

void xemu_pfifo_perf_record_user_dma_get_write(void)
{
    qatomic_inc(&pfifo_perf_user_dma_get_writes);
}

void xemu_pfifo_perf_record_user_dma_put_read(void)
{
    qatomic_inc(&pfifo_perf_user_dma_put_reads);
}

void xemu_pfifo_perf_record_user_dma_get_read(void)
{
    qatomic_inc(&pfifo_perf_user_dma_get_reads);
}

void xemu_pfifo_perf_record_patt_color0_write(void)
{
    qatomic_inc(&pfifo_perf_patt_color0_writes);
}

void xemu_pfifo_perf_record_patt_color0_read(void)
{
    qatomic_inc(&pfifo_perf_patt_color0_reads);
}

void xemu_pfifo_perf_record_flip_stall_set(void)
{
    qatomic_inc(&pfifo_perf_flip_stall_sets);
}

void xemu_pfifo_perf_record_flip_stall_check(bool still_waiting)
{
    qatomic_inc(&pfifo_perf_flip_stall_checks);
    if (still_waiting) {
        qatomic_inc(&pfifo_perf_flip_stall_still_waiting);
    } else {
        qatomic_inc(&pfifo_perf_flip_stall_completes);
    }
}

void xemu_pfifo_perf_emit_and_reset(FILE *out)
{
    if (out == NULL) {
        return;
    }

    uint64_t pusher_runs = qatomic_xchg(&pfifo_perf_pusher_runs, 0);
    uint64_t pusher_stalls = qatomic_xchg(&pfifo_perf_pusher_stalls, 0);
    uint64_t pusher_stall_flip =
        qatomic_xchg(&pfifo_perf_pusher_stall_flip, 0);
    uint64_t pusher_stall_nop =
        qatomic_xchg(&pfifo_perf_pusher_stall_nop, 0);
    uint64_t pusher_stall_fifo =
        qatomic_xchg(&pfifo_perf_pusher_stall_fifo_access, 0);
    uint64_t puller_calls = qatomic_xchg(&pfifo_perf_puller_calls, 0);
    uint64_t puller_stalls = qatomic_xchg(&pfifo_perf_puller_stalls, 0);
    uint64_t puller_stall_flip =
        qatomic_xchg(&pfifo_perf_puller_stall_flip, 0);
    uint64_t puller_stall_nop =
        qatomic_xchg(&pfifo_perf_puller_stall_nop, 0);
    uint64_t puller_stall_context =
        qatomic_xchg(&pfifo_perf_puller_stall_context, 0);
    uint64_t puller_stall_fifo =
        qatomic_xchg(&pfifo_perf_puller_stall_fifo_access, 0);
    uint64_t pusher_words = qatomic_xchg(&pfifo_perf_pusher_words, 0);
    uint64_t puller_method_words =
        qatomic_xchg(&pfifo_perf_puller_method_words, 0);
    uint64_t backlog_max =
        qatomic_xchg(&pfifo_perf_dma_backlog_bytes_max, 0);
    uint64_t put_writes =
        qatomic_xchg(&pfifo_perf_user_dma_put_writes, 0);
    uint64_t get_writes =
        qatomic_xchg(&pfifo_perf_user_dma_get_writes, 0);
    uint64_t put_reads =
        qatomic_xchg(&pfifo_perf_user_dma_put_reads, 0);
    uint64_t get_reads =
        qatomic_xchg(&pfifo_perf_user_dma_get_reads, 0);
    uint64_t patt_writes =
        qatomic_xchg(&pfifo_perf_patt_color0_writes, 0);
    uint64_t patt_reads =
        qatomic_xchg(&pfifo_perf_patt_color0_reads, 0);
    uint64_t flip_sets =
        qatomic_xchg(&pfifo_perf_flip_stall_sets, 0);
    uint64_t flip_checks =
        qatomic_xchg(&pfifo_perf_flip_stall_checks, 0);
    uint64_t flip_waits =
        qatomic_xchg(&pfifo_perf_flip_stall_still_waiting, 0);
    uint64_t flip_completes =
        qatomic_xchg(&pfifo_perf_flip_stall_completes, 0);

    if ((pusher_runs | pusher_stalls | pusher_stall_flip |
         pusher_stall_nop | pusher_stall_fifo | puller_calls |
         puller_stalls | puller_stall_flip | puller_stall_nop |
         puller_stall_context | puller_stall_fifo | pusher_words |
         puller_method_words | backlog_max | put_writes | get_writes |
         put_reads | get_reads | patt_writes | patt_reads | flip_sets |
         flip_checks | flip_waits | flip_completes) == 0) {
        return;
    }

    fprintf(out,
            " PFIFO_PUSHER_RUNS=%llu PFIFO_PUSHER_STALLS=%llu"
            " PFIFO_PUSHER_STALL_FLIP=%llu PFIFO_PUSHER_STALL_NOP=%llu"
            " PFIFO_PUSHER_STALL_FIFO=%llu PFIFO_PULLER_CALLS=%llu"
            " PFIFO_PULLER_STALLS=%llu PFIFO_PULLER_STALL_FLIP=%llu"
            " PFIFO_PULLER_STALL_NOP=%llu PFIFO_PULLER_STALL_CONTEXT=%llu"
            " PFIFO_PULLER_STALL_FIFO=%llu PFIFO_PUSHER_WORDS=%llu"
            " PFIFO_PULLER_METHOD_WORDS=%llu"
            " PFIFO_DMA_BACKLOG_BYTES_MAX=%llu"
            " PFIFO_USER_DMA_PUT_WRITES=%llu"
            " PFIFO_USER_DMA_GET_WRITES=%llu"
            " PFIFO_USER_DMA_PUT_READS=%llu"
            " PFIFO_USER_DMA_GET_READS=%llu"
            " PGRAPH_PATT_COLOR0_WRITES=%llu"
            " PGRAPH_PATT_COLOR0_READS=%llu"
            " PGRAPH_FLIP_STALL_SETS=%llu"
            " PGRAPH_FLIP_STALL_CHECKS=%llu"
            " PGRAPH_FLIP_STALL_STILL_WAITING=%llu"
            " PGRAPH_FLIP_STALL_COMPLETES=%llu",
            (unsigned long long)pusher_runs,
            (unsigned long long)pusher_stalls,
            (unsigned long long)pusher_stall_flip,
            (unsigned long long)pusher_stall_nop,
            (unsigned long long)pusher_stall_fifo,
            (unsigned long long)puller_calls,
            (unsigned long long)puller_stalls,
            (unsigned long long)puller_stall_flip,
            (unsigned long long)puller_stall_nop,
            (unsigned long long)puller_stall_context,
            (unsigned long long)puller_stall_fifo,
            (unsigned long long)pusher_words,
            (unsigned long long)puller_method_words,
            (unsigned long long)backlog_max,
            (unsigned long long)put_writes,
            (unsigned long long)get_writes,
            (unsigned long long)put_reads,
            (unsigned long long)get_reads,
            (unsigned long long)patt_writes,
            (unsigned long long)patt_reads,
            (unsigned long long)flip_sets,
            (unsigned long long)flip_checks,
            (unsigned long long)flip_waits,
            (unsigned long long)flip_completes);
}

static uint64_t ide_perf_submit_count;
static uint64_t ide_perf_complete_count;
static uint64_t ide_perf_bytes_total;
static uint64_t ide_perf_latency_us_total;
static uint64_t ide_perf_latency_us_max;
static uint64_t ide_perf_latency_gt_16ms;
static uint64_t ide_perf_latency_gt_33ms;
static uint64_t ide_perf_latency_gt_50ms;
static uint64_t ide_perf_latency_gt_100ms;
static uint64_t ide_perf_inflight_max;
static uint64_t ide_perf_atapi_pio_sync_count;
static uint64_t ide_perf_atapi_pio_sync_bytes;
static uint64_t ide_perf_atapi_pio_sync_us_total;
static uint64_t ide_perf_atapi_pio_sync_us_max;

static void ide_perf_max(uint64_t *counter, uint64_t value)
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

void xemu_ide_perf_record_submit(uint64_t bytes)
{
    qatomic_inc(&ide_perf_submit_count);
    qatomic_add(&ide_perf_bytes_total, bytes);
}

void xemu_ide_perf_record_complete(uint64_t bytes, uint64_t latency_us)
{
    (void)bytes;

    qatomic_inc(&ide_perf_complete_count);
    qatomic_add(&ide_perf_latency_us_total, latency_us);
    ide_perf_max(&ide_perf_latency_us_max, latency_us);
    if (latency_us > 16000) {
        qatomic_inc(&ide_perf_latency_gt_16ms);
    }
    if (latency_us > 33000) {
        qatomic_inc(&ide_perf_latency_gt_33ms);
    }
    if (latency_us > 50000) {
        qatomic_inc(&ide_perf_latency_gt_50ms);
    }
    if (latency_us > 100000) {
        qatomic_inc(&ide_perf_latency_gt_100ms);
    }
}

void xemu_ide_perf_record_inflight(uint64_t inflight)
{
    ide_perf_max(&ide_perf_inflight_max, inflight);
}

void xemu_ide_perf_record_atapi_pio_sync(uint64_t bytes, uint64_t latency_us)
{
    qatomic_inc(&ide_perf_atapi_pio_sync_count);
    qatomic_add(&ide_perf_atapi_pio_sync_bytes, bytes);
    qatomic_add(&ide_perf_atapi_pio_sync_us_total, latency_us);
    ide_perf_max(&ide_perf_atapi_pio_sync_us_max, latency_us);
}

void xemu_ide_perf_emit_and_reset(FILE *out)
{
    uint64_t submits = qatomic_xchg(&ide_perf_submit_count, 0);
    uint64_t completes = qatomic_xchg(&ide_perf_complete_count, 0);
    uint64_t bytes = qatomic_xchg(&ide_perf_bytes_total, 0);
    uint64_t latency_total = qatomic_xchg(&ide_perf_latency_us_total, 0);
    uint64_t latency_max = qatomic_xchg(&ide_perf_latency_us_max, 0);
    uint64_t gt_16ms = qatomic_xchg(&ide_perf_latency_gt_16ms, 0);
    uint64_t gt_33ms = qatomic_xchg(&ide_perf_latency_gt_33ms, 0);
    uint64_t gt_50ms = qatomic_xchg(&ide_perf_latency_gt_50ms, 0);
    uint64_t gt_100ms = qatomic_xchg(&ide_perf_latency_gt_100ms, 0);
    uint64_t inflight_max = qatomic_xchg(&ide_perf_inflight_max, 0);
    uint64_t pio_syncs = qatomic_xchg(&ide_perf_atapi_pio_sync_count, 0);
    uint64_t pio_sync_bytes =
        qatomic_xchg(&ide_perf_atapi_pio_sync_bytes, 0);
    uint64_t pio_sync_total =
        qatomic_xchg(&ide_perf_atapi_pio_sync_us_total, 0);
    uint64_t pio_sync_max =
        qatomic_xchg(&ide_perf_atapi_pio_sync_us_max, 0);

    if (out == NULL ||
        (submits | completes | bytes | latency_total | latency_max |
         gt_16ms | gt_33ms | gt_50ms | gt_100ms | inflight_max |
         pio_syncs | pio_sync_bytes | pio_sync_total | pio_sync_max) == 0) {
        return;
    }

    fprintf(out,
            " IDE_READ_SUBMITS=%llu IDE_READ_COMPLETES=%llu"
            " IDE_READ_BYTES=%llu IDE_READ_LAT_US_TOTAL=%llu"
            " IDE_READ_LAT_US_MAX=%llu IDE_READ_LAT_GT_16MS=%llu"
            " IDE_READ_LAT_GT_33MS=%llu IDE_READ_LAT_GT_50MS=%llu"
            " IDE_READ_LAT_GT_100MS=%llu IDE_READ_INFLIGHT_MAX=%llu"
            " ATAPI_PIO_SYNC_READS=%llu ATAPI_PIO_SYNC_BYTES=%llu"
            " ATAPI_PIO_SYNC_US_TOTAL=%llu ATAPI_PIO_SYNC_US_MAX=%llu",
            (unsigned long long)submits,
            (unsigned long long)completes,
            (unsigned long long)bytes,
            (unsigned long long)latency_total,
            (unsigned long long)latency_max,
            (unsigned long long)gt_16ms,
            (unsigned long long)gt_33ms,
            (unsigned long long)gt_50ms,
            (unsigned long long)gt_100ms,
            (unsigned long long)inflight_max,
            (unsigned long long)pio_syncs,
            (unsigned long long)pio_sync_bytes,
            (unsigned long long)pio_sync_total,
            (unsigned long long)pio_sync_max);
}
