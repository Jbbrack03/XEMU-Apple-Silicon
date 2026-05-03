/*
 * NV2A PGRAPH Metal renderer — buffer pool (slice M3).
 *
 * Triple-buffered staging ring for per-frame vertex / index / uniform
 * uploads, plus a placeholder for the "vertex RAM" 1:1 mapping over guest
 * VRAM (M3 uses per-draw staging; the 1:1 mapping is a future optimization
 * — see buffer.mm for the rationale).
 *
 * The ring uses three slots of equal size; each slot is a single
 * id<MTLBuffer> with MTLResourceStorageModeShared |
 * MTLResourceCPUCacheModeWriteCombined. An MTLSharedEvent gates reuse:
 * advancing the host-write slot blocks until the GPU has finished
 * consuming its previous use.
 *
 * Per docs/apple-silicon/metal-renderer-plan.md slice M3 the gate is
 * "a simple 2D scene draws — colored triangles visible in the window,
 * geometrically correct, not yet textured or combiner-shaded".
 *
 * This header is C-callable. Implementation is .mm and uses ARC.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_BUFFER_H
#define HW_XBOX_NV2A_PGRAPH_MTL_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize / finalize. Init must run after xemu_metal_init() (the
 * MTLDevice owner). Returns true on success, false if the device is
 * unavailable or buffer creation failed.
 */
bool pgraph_mtl_buffer_init(void);
void pgraph_mtl_buffer_finalize(void);

/*
 * Per-UI-frame ring rotation. Call begin_frame at the start of a frame
 * (advances the write slot, blocks on the shared event if the next slot
 * is still in flight) and end_frame at the end (signals the event so
 * the GPU's consumption marker advances when the command buffer
 * completes).
 *
 * For M3, begin/end_frame are bracketed inside flush_draw because the
 * Metal renderer does not yet have a per-frame begin/end on the host
 * side — every flush is its own command buffer. Future slices will
 * coalesce into one command buffer per UI frame.
 */
void pgraph_mtl_buffer_begin_frame(void);
void pgraph_mtl_buffer_end_frame(void);

/*
 * Stage vertex data into the current ring slot. Copies `size` bytes
 * from `data` into the staging buffer at the next aligned offset and
 * returns the id<MTLBuffer> via *out_buffer (cast to void*) and the
 * byte offset via *out_offset.
 *
 * Returns true on success; false if the requested size exceeds the
 * slot's free capacity (caller may force a flush + retry, but for M3
 * we just drop the draw if this happens).
 *
 * The returned buffer pointer is valid for the lifetime of the current
 * frame — until the matching end_frame fence has signaled and the slot
 * has been recycled. M3's draw.mm encodes immediately after staging,
 * so the lifetime constraint is satisfied trivially.
 */
bool pgraph_mtl_buffer_stage_vertex(const void *data, size_t size,
                                    void **out_buffer, size_t *out_offset);

bool pgraph_mtl_buffer_stage_index(const void *data, size_t size,
                                   void **out_buffer, size_t *out_offset);

/*
 * Reserve space for a uniform block. Returns the buffer (cast to void*)
 * and the offset; the caller writes into the buffer via *out_ptr.
 *
 * alignment must be a power of two (use 256 to satisfy Metal constant-
 * buffer alignment requirements; uniform offsets in argument tables
 * have a 32-byte minimum alignment on Apple Silicon, but 256 is the
 * safe portable choice and matches xemu's GL renderer).
 *
 * Returns false on capacity overflow.
 */
bool pgraph_mtl_buffer_stage_uniform(size_t size, size_t alignment,
                                     void **out_buffer, size_t *out_offset,
                                     void **out_ptr);

/*
 * Vertex-RAM accessor. For M3, returns NULL — we use per-draw staging
 * via pgraph_mtl_buffer_stage_vertex instead of mapping a 64 MiB
 * id<MTLBuffer> 1:1 over guest VRAM. The accessor is preserved in the
 * API surface so M4+ can introduce the persistent VRAM mapping without
 * rewriting the draw paths. See buffer.mm "Vertex RAM mapping" for the
 * rationale.
 */
void *pgraph_mtl_buffer_get_vertex_ram(void);

/*
 * Mark a guest VRAM range stale. For M3 this is a no-op because the
 * draw path always re-stages from CPU memory; M4+ will use this to
 * invalidate the cached upload bitmap.
 */
void pgraph_mtl_buffer_invalidate_vertex_ram_range(uint32_t addr,
                                                   uint32_t length);

/*
 * Diagnostic counters surfaced by extract-perf-summary.sh.
 */
uint64_t pgraph_mtl_buffer_stage_bytes(void);
uint64_t pgraph_mtl_buffer_frame_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_BUFFER_H */
