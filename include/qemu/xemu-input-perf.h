/*
 * Apple Silicon performance fork: input-latency counters (slice N1).
 *
 * Three event counters and two latency accumulators decompose the
 * controller-input pipeline:
 *
 *   xemu_input_perf_record_backend_update(int64_t now_us)
 *       The host-side controller backend (SDL or GameController.framework)
 *       refreshed its cached ControllerState. The most recent monotonic
 *       timestamp is stamped on the per-port slot so the next guest poll
 *       can compute the cache-to-poll latency.
 *
 *   xemu_input_perf_record_backend_update_port(int port, int64_t now_us)
 *       Same as the above, but stamps a specific bound port (0..3).
 *       Used by the SDL / native paths so the latency window is
 *       per-controller rather than global.
 *
 *   xemu_input_perf_record_usb_poll(int port, int64_t now_us)
 *       The guest issued an interrupt-IN read on the XID gamepad
 *       endpoint and consumed the cached ControllerState. Computes
 *       (now_us - last_backend_update_ts[port]) and folds it into
 *       INPUT_LAT_US_TOTAL / INPUT_LAT_US_MAX. Increments
 *       INPUT_USB_POLLS.
 *
 * Surfaced on the `xemu-perf:` interval line as:
 *   INPUT_USB_POLLS, INPUT_BACKEND_UPDATES,
 *   INPUT_LAT_US_TOTAL, INPUT_LAT_US_MAX
 *
 * Cost: a couple of qatomic_inc / qatomic_add per event. No mutex.
 */

#ifndef QEMU_XEMU_INPUT_PERF_H
#define QEMU_XEMU_INPUT_PERF_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void xemu_input_perf_record_backend_update_port(int port, int64_t now_us);
void xemu_input_perf_record_backend_update(int64_t now_us);
void xemu_input_perf_record_usb_poll(int port, int64_t now_us);

void xemu_input_perf_emit_and_reset(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_INPUT_PERF_H */
