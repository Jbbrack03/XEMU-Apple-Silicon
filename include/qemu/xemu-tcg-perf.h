/*
 * Apple Silicon performance fork: TCG hot-path counters.
 *
 * Lightweight atomic counters incremented on the TCG vCPU thread and
 * snapshotted alongside the NV2A renderer perf interval line. Kept
 * separate from `nv2a_profile_*` so the increments do not require the
 * NV2A renderer's per-frame state and are safe to call from any thread.
 *
 * Increments are unconditionally cheap (single qatomic_inc / set);
 * they only become visible when XEMU_PERF_LOG=1 and the NV2A profile
 * module emits its interval line.
 */

#ifndef QEMU_XEMU_TCG_PERF_H
#define QEMU_XEMU_TCG_PERF_H

#include <stdint.h>
#include <stdio.h>

#include "exec/cpu-common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hot-path increments. Inline-able so the compiler can inline the
 * qatomic_inc into call sites; keep them out-of-line if the header
 * dependency cycle becomes a problem. */
void xemu_tcg_perf_inc_tb_exec(void);
void xemu_tcg_perf_inc_tb_invalidate(void);

/* notdirty trips: increment the trip counter unconditionally and
 * record the (page-aligned) ram_addr in a small open-addressed set so
 * the unique-pages-touched count can be reported per interval. */
void xemu_tcg_perf_notdirty_trip(ram_addr_t page_addr);

/* Per-call MAX of TBs invalidated in a single
 * tb_invalidate_phys_page_range__locked invocation. */
void xemu_tcg_perf_record_invalidate_burst(uint32_t count);

/* I2 diagnostic counters: jmp-cache buckets cleared per interval (sum)
 * and per-call MAX wallclock cost (microseconds) of
 * tb_invalidate_phys_page_range__locked. */
void xemu_tcg_perf_add_jmp_cache_zeroed(uint32_t buckets);
void xemu_tcg_perf_record_invalidate_wall_us(uint64_t us);

/* V3 attribution: per-second sliding-window rate detectors. Each tick
 * is one event; once the window closes (1 s wallclock) the helper
 * compares the count to the storm threshold and, if exceeded, calls
 * xemu_spike_emit("tcg_notdirty_storm" / "tcg_x87_storm", duration_us,
 * extra). Callers MUST gate the tick on xemu_spike_log_tcg_enabled so
 * the steady-state cost is one load+branch when off. */
void xemu_tcg_perf_notdirty_storm_tick(void);
void xemu_tcg_perf_x87_storm_tick(void);

/* Emit the current snapshot as `KEY=value` fields appended to the open
 * `xemu-perf:` interval line, then reset. Called from the NV2A
 * profile module's interval-flush path. */
void xemu_tcg_perf_emit_and_reset(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_TCG_PERF_H */
