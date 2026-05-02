/*
 * Apple Silicon performance fork: MCPX APU lock-hold / vCPU-wait counters.
 *
 * Two atomic counters that decompose the audio-MMIO contention question
 * D3 attribution identified (`docs/apple-silicon/benchmarks/
 * 2026-05-02-tcg-30fps-cap-attribution.md`): how much wall-clock per
 * interval the APU worker thread holds `MCPXAPUState::lock` during
 * dispatched VP frames, and what is the worst single-event vCPU wait
 * on that lock per interval.
 *
 *   xemu_apu_perf_add_lock_hold_us(us)
 *       APU worker thread call: at the end of each dispatched VP-frame
 *       processing section, add the wallclock microseconds `d->lock`
 *       was held for that section. With the lock-release slice off,
 *       this is the full wait-for-workers + drain-mixbins + downstream
 *       cost. With the slice on, the worker-finished wait window is
 *       excluded (the lock was released during it). Summed per
 *       interval; emitted as APU_LOCK_HOLD_US_TOTAL. The counter
 *       drops by exactly the slice's lock-release window when the
 *       flag is enabled, which is the decisive measurement of slice
 *       effect on the audio-frame critical section.
 *   xemu_apu_perf_record_vcpu_wait_us(us)
 *       vCPU thread call: at the end of any APU MMIO write that took
 *       `d->lock` (voice_lock, gp_write, ep_write), record the
 *       acquire-wait duration. CAS-loop max per interval; emitted as
 *       APU_VCPU_LOCK_WAIT_US_MAX. With the slice off this approaches
 *       the VP-frame period (~5.33 ms) under audio contention; with
 *       the slice on it should drop to the un-contended acquire cost
 *       plus the still-held sections of the dispatch loop.
 *
 * Cost: hold counter is `qatomic_add`; wait counter is a CAS loop.
 * Both wait-free on aarch64. Always compiled in; the `xemu-perf:`
 * interval line is only emitted when `XEMU_PERF_LOG=1`, so per-event
 * overhead is bounded by the atomic op itself when the perf log is
 * disabled.
 */

#ifndef QEMU_XEMU_APU_PERF_H
#define QEMU_XEMU_APU_PERF_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void xemu_apu_perf_add_lock_hold_us(uint64_t us);
void xemu_apu_perf_record_vcpu_wait_us(uint64_t us);

/* Append `KEY=value` fields to the open `xemu-perf:` interval line and
 * reset the counters for the next interval. Called from
 * nv2a_profile_log_emit_interval. */
void xemu_apu_perf_emit_and_reset(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_APU_PERF_H */
