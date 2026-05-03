/*
 * Apple Silicon performance fork: IDE / ATAPI latency counters.
 */

#ifndef QEMU_XEMU_IDE_PERF_H
#define QEMU_XEMU_IDE_PERF_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void xemu_ide_perf_record_submit(uint64_t bytes);
void xemu_ide_perf_record_complete(uint64_t bytes, uint64_t latency_us);
void xemu_ide_perf_record_inflight(uint64_t inflight);
void xemu_ide_perf_record_atapi_pio_sync(uint64_t bytes, uint64_t latency_us);
void xemu_ide_perf_emit_and_reset(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_IDE_PERF_H */
