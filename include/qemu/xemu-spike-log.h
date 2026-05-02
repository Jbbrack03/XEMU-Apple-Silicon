/*
 * Apple Silicon performance fork: shared spike-log emit helper.
 *
 * Provides a single emit-helper used by both the NV2A renderer
 * (nv2a_profile_spike, in hw/xbox/nv2a/pgraph/profile.c) and the
 * TCG-thread spike sites added for the V3 attribution slice. Keeping
 * the helper in util/ avoids the renderer-side header dependency from
 * accel/tcg/ and target/i386/ call sites.
 *
 * Two enable bits:
 *   xemu_spike_log_renderer_enabled  - mirrors XEMU_PERF_SPIKE_LOG.
 *                                       Owned by NV2A profile init.
 *   xemu_spike_log_tcg_enabled       - mirrors XEMU_PERF_SPIKE_LOG_TCG
 *                                       (default OFF). Read by the TCG
 *                                       hot-path spike sites; gates
 *                                       *all* per-event measurement so
 *                                       there is zero overhead when off.
 *
 * Threshold: shared global xemu_spike_threshold_us, default 50000
 * (50 ms), tunable via XEMU_PERF_SPIKE_LOG_THRESHOLD_US.
 *
 * Output format (unchanged from the original nv2a_profile_spike line):
 *   xemu-spike: op=<name> duration_us=<n> now_us=<n>
 *
 * The TCG sources may emit additional structured fields after now_us
 * (see xemu_spike_emit_extra) for "what was happening at the spike"
 * context (e.g. tb_count for tcg_tb_chain).
 */

#ifndef QEMU_XEMU_SPIKE_LOG_H
#define QEMU_XEMU_SPIKE_LOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize from environment. Idempotent; safe to call from multiple
 * subsystem inits. The first caller wins; subsequent calls are no-ops. */
void xemu_spike_log_init(void);

/* Read access to the cached env-derived enables / threshold. The TCG
 * hot-path uses these for the cheap-skip test. */
extern bool xemu_spike_log_renderer_enabled;
extern bool xemu_spike_log_tcg_enabled;
extern int64_t xemu_spike_threshold_us;

/* V7 cumulative-phase enable. Mirrors XEMU_TCG_PHASE_LOG=1; off by
 * default. When on, cpu_exec_loop accumulates per-phase wallclock
 * (tb_lookup, tb_gen_code, cpu_handle_interrupt) into the
 * TCG_TB_LOOKUP_US_TOTAL / TCG_TB_GEN_CODE_US_TOTAL /
 * TCG_HANDLE_INTERRUPT_US_TOTAL counters surfaced on the per-interval
 * xemu-perf line. Initialised once by xemu_spike_log_init alongside
 * the spike-log flags. */
extern bool xemu_tcg_phase_log_enabled;

/* Emit a spike line if duration_us >= threshold. The op string is
 * inserted verbatim. extra (may be NULL) is appended after now_us as
 * additional " key=value" fields, allowing TCG sources to attach
 * context (tb_count, page_addr, rate, etc.) without bloating this
 * API. The emit does NOT itself check the renderer/tcg enable bits;
 * callers must gate on the appropriate bit before calling. */
void xemu_spike_emit(const char *op, int64_t duration_us, const char *extra);

/* Convenience: caller-gated sites do "if (enabled && dur > threshold)
 * call xemu_spike_emit(op, dur, NULL)". The threshold check is
 * duplicated inside xemu_spike_emit as a defense in depth. */

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_SPIKE_LOG_H */
