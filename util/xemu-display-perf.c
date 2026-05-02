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
