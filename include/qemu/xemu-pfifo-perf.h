/*
 * Apple Silicon performance fork: NV2A PFIFO / marker progress counters.
 *
 * These counters attribute long guest-visible GPU waits without taking hot
 * locks. They are emitted on the existing `xemu-perf:` interval line.
 */

#ifndef QEMU_XEMU_PFIFO_PERF_H
#define QEMU_XEMU_PFIFO_PERF_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum XemuPFIFOStallReason {
    XEMU_PFIFO_STALL_FLIP = 1u << 0,
    XEMU_PFIFO_STALL_NOP = 1u << 1,
    XEMU_PFIFO_STALL_CONTEXT = 1u << 2,
    XEMU_PFIFO_STALL_FIFO_ACCESS = 1u << 3,
};

void xemu_pfifo_perf_record_pusher_run(void);
void xemu_pfifo_perf_record_pusher_stall(uint32_t reasons);
void xemu_pfifo_perf_record_puller_call(void);
void xemu_pfifo_perf_record_puller_stall(uint32_t reasons);
void xemu_pfifo_perf_add_pusher_words(uint64_t words);
void xemu_pfifo_perf_add_puller_method_words(uint64_t words);
void xemu_pfifo_perf_sample_dma_backlog(uint64_t bytes);
void xemu_pfifo_perf_record_user_dma_put_write(void);
void xemu_pfifo_perf_record_user_dma_get_write(void);
void xemu_pfifo_perf_record_user_dma_put_read(void);
void xemu_pfifo_perf_record_user_dma_get_read(void);
void xemu_pfifo_perf_record_patt_color0_write(void);
void xemu_pfifo_perf_record_patt_color0_read(void);
void xemu_pfifo_perf_record_flip_stall_set(void);
void xemu_pfifo_perf_record_flip_stall_check(bool still_waiting);

void xemu_pfifo_perf_emit_and_reset(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_PFIFO_PERF_H */
