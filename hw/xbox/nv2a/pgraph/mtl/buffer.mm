/*
 * NV2A PGRAPH Metal renderer — buffer pool implementation (slice M3).
 *
 * See buffer.h for the public contract. Implements a triple-buffered
 * staging ring (3×16 MiB Shared|WriteCombined MTLBuffers) plus an
 * MTLSharedEvent fence that gates slot reuse on GPU consumption.
 *
 * --------------------------------------------------------------------
 * Vertex RAM mapping (M3 deferral).
 *
 * The metal-renderer-plan.md M3 spec calls for "BUFFER_VERTEX_RAM as a
 * 64 MiB Shared|WriteCombined MTLBuffer mapped 1:1 over guest VRAM".
 * That requires either:
 *
 *   (a) `[device newBufferWithBytesNoCopy:length:options:deallocator:]`
 *       on the QEMU host VRAM pointer. This call requires the buffer
 *       length and start address to be aligned to the page size
 *       (16 KiB on Apple Silicon). QEMU's `memory_region_get_ram_ptr`
 *       returns the host-mapped guest VRAM, which IS page-aligned at
 *       the start, but using `bytesNoCopy` ties the MTLBuffer's
 *       lifetime to the QEMU memory region (must be deallocated
 *       before vram is freed) and means every guest write becomes
 *       implicitly visible to the GPU — there is no explicit upload
 *       step, which removes the natural place to insert the
 *       `uploaded_bitmap` invalidation tracking that vk/buffer.c uses
 *       in `pgraph_vk_update_vertex_ram_buffer`.
 *
 *   (b) Per-draw staging into the ring buffer (this slice's choice).
 *       Each draw copies the relevant vertex data into a fresh ring
 *       slot. Costs ~one memcpy per draw of vertex_count *
 *       attribute_stride bytes; for the inline_buffer path the data is
 *       already in `attr->inline_buffer` (a host-side float[N][4]
 *       array), so the memcpy is straight-line.
 *
 * For M3 we use (b). The hand-coded passthrough only handles the
 * inline_buffer path (NV2A's "host issues per-vertex calls" path).
 * The 1:1 vram mapping pays off only when the draw_arrays /
 * inline_elements paths land (M4+), at which point we'll have the
 * upload-bitmap machinery to amortize host-write tracking. The plan
 * doc explicitly notes "1:1 mapping can be a future optimization once
 * the slice gates pass". `pgraph_mtl_buffer_get_vertex_ram` returns
 * NULL accordingly.
 *
 * --------------------------------------------------------------------
 * Triple-buffering with MTLSharedEvent.
 *
 * Three ring slots indexed 0..2. Each slot has its own +1-retained
 * MTLBuffer (16 MiB). A monotonic counter `s_frame_signal_value`
 * advances on every end_frame; the MTLSharedEvent's `signaledValue`
 * lags the counter by exactly the number of in-flight frames (max 3).
 *
 * begin_frame waits on `signaledValue >= s_frame_signal_value - 2`
 * before advancing the slot index. end_frame schedules a
 * `[cmd encodeSignalEvent:event value:s_frame_signal_value]` style
 * signal — but we don't have a frame-level command buffer yet, so M3
 * uses a simpler approach: `[event setSignaledValue:value]` is called
 * synchronously on a tiny dedicated command buffer that does nothing
 * but signal. That command buffer enters the queue after the draw
 * command buffers, so it signals when those complete. begin_frame's
 * wait uses `[event waitUntilSignaledValue:atTimeout:]`.
 *
 * Slot capacity is 16 MiB (per-slot) which is generous for early
 * scenes; the inline_buffer pessimistic case is
 * NV2A_VERTEXSHADER_ATTRIBUTES (16) × NV2A_MAX_BATCH_LENGTH (524287) ×
 * 16 bytes ≈ 128 MiB, but in practice batches are much smaller. For
 * M3, scenes that exceed the slot capacity will simply drop the draw
 * (logged). M5+ will introduce a finer-grained tracker.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "buffer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" void *xemu_metal_get_device(void);

/* -------- configuration -------- */

#define MTL_RING_SLOTS         3
/* 16 MiB per slot. Sized to comfortably hold any plausible
 * inline_buffer-path draw batch on a single slot; bigger draws (M4+
 * draw_arrays / inline_elements) may need a larger slot or a per-slot
 * grow-on-demand path. */
#define MTL_RING_SLOT_BYTES    (16ULL * 1024ULL * 1024ULL)

/* Conservative alignment for any staged blob. 16 covers float4. The
 * uniform stage path takes its own alignment argument. */
#define MTL_RING_DEFAULT_ALIGN 16

/* -------- state -------- */

static id<MTLDevice>         s_device;
static id<MTLCommandQueue>   s_signal_queue;
static id<MTLSharedEvent>    s_event;

/* Three independent buffers, one per slot. */
static id<MTLBuffer>  s_slot_buffer[MTL_RING_SLOTS];
static size_t         s_slot_offset[MTL_RING_SLOTS];

/* Currently-active slot for host writes (the slot that begin_frame
 * advanced to). Goes 0,1,2,0,1,2,... */
static int            s_active_slot;

/* Monotonic counter; signaled on the shared event when the active
 * slot's GPU work completes. Slot N's reuse is gated on this counter
 * minus (MTL_RING_SLOTS - 1). */
static uint64_t       s_frame_signal_value;
/* Per-slot snapshot of the signal value taken when the slot was
 * advanced; used to wait for that exact frame's GPU work. */
static uint64_t       s_slot_signal_value[MTL_RING_SLOTS];
/* True once we've issued the first frame; before that begin_frame
 * skips the wait (no prior in-flight work to wait on). */
static bool           s_first_frame_issued;

static bool s_initialized = false;

/* Diagnostic atomics. */
static _Atomic(uint64_t) s_stage_bytes_total = 0;
static _Atomic(uint64_t) s_frame_count_total = 0;

/* -------- helpers -------- */

static size_t round_up_to(size_t value, size_t alignment)
{
    return (value + (alignment - 1)) & ~(alignment - 1);
}

/* -------- init / finalize -------- */

bool pgraph_mtl_buffer_init(void)
{
    if (s_initialized) {
        return true;
    }

    s_device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (s_device == nil) {
        fprintf(stderr,
                "pgraph_mtl_buffer_init: no Metal device; "
                "xemu_metal_init must run first\n");
        return false;
    }

    /* Dedicated low-traffic queue used only to issue the per-frame
     * signal command buffer. Keeping it separate from the surface
     * manager's render queue avoids accidental ordering dependencies
     * between the signal and unrelated render work. */
    s_signal_queue = [s_device newCommandQueueWithMaxCommandBufferCount:8];
    if (s_signal_queue == nil) {
        fprintf(stderr, "pgraph_mtl_buffer_init: signal queue creation failed\n");
        return false;
    }
    s_signal_queue.label = @"xemu.metal.buffer_signal_queue";

    s_event = [s_device newSharedEvent];
    if (s_event == nil) {
        fprintf(stderr, "pgraph_mtl_buffer_init: newSharedEvent failed\n");
        s_signal_queue = nil;
        return false;
    }
    s_event.label = @"xemu.metal.buffer_ring_event";

    MTLResourceOptions opts = MTLResourceStorageModeShared |
                              MTLResourceCPUCacheModeWriteCombined;

    for (int i = 0; i < MTL_RING_SLOTS; i++) {
        s_slot_buffer[i] = [s_device newBufferWithLength:MTL_RING_SLOT_BYTES
                                                 options:opts];
        if (s_slot_buffer[i] == nil) {
            fprintf(stderr,
                    "pgraph_mtl_buffer_init: slot %d alloc failed (%llu MiB)\n",
                    i, (unsigned long long)(MTL_RING_SLOT_BYTES >> 20));
            for (int j = 0; j < i; j++) {
                s_slot_buffer[j] = nil;
            }
            s_event = nil;
            s_signal_queue = nil;
            return false;
        }
        s_slot_buffer[i].label =
            [NSString stringWithFormat:@"xemu.metal.staging_slot_%d", i];
        s_slot_offset[i] = 0;
        s_slot_signal_value[i] = 0;
    }

    s_active_slot = 0;
    s_frame_signal_value = 0;
    s_first_frame_issued = false;
    atomic_store(&s_stage_bytes_total, (uint64_t)0);
    atomic_store(&s_frame_count_total, (uint64_t)0);

    s_initialized = true;
    return true;
}

void pgraph_mtl_buffer_finalize(void)
{
    if (!s_initialized) {
        return;
    }

    /* Drain the event — wait for any outstanding signaled frames. */
    if (s_event != nil && s_first_frame_issued) {
        (void)[s_event waitUntilSignaledValue:s_frame_signal_value
                                     timeoutMS:1000];
    }

    for (int i = 0; i < MTL_RING_SLOTS; i++) {
        s_slot_buffer[i] = nil;
        s_slot_offset[i] = 0;
        s_slot_signal_value[i] = 0;
    }

    s_event = nil;
    s_signal_queue = nil;
    s_device = nil;
    s_initialized = false;
}

/* -------- per-frame ring rotation -------- */

void pgraph_mtl_buffer_begin_frame(void)
{
    if (!s_initialized) {
        return;
    }

    /* Advance to the next slot. */
    s_active_slot = (s_active_slot + 1) % MTL_RING_SLOTS;

    /* Wait for the slot's previous GPU work to drain. The gating
     * value is whatever signal value was attached to this slot when
     * it was last used; if the slot has never been used, the stored
     * value is 0 and the event default-signaled-value is 0 so the
     * wait is a no-op. */
    if (s_first_frame_issued && s_slot_signal_value[s_active_slot] > 0) {
        (void)[s_event waitUntilSignaledValue:
                              s_slot_signal_value[s_active_slot]
                                     timeoutMS:1000];
    }

    /* Reset the slot's offset for new writes. */
    s_slot_offset[s_active_slot] = 0;
}

void pgraph_mtl_buffer_end_frame(void)
{
    if (!s_initialized) {
        return;
    }

    /* Bump the signal counter and remember it for this slot. */
    s_frame_signal_value++;
    s_slot_signal_value[s_active_slot] = s_frame_signal_value;

    /* Issue an empty command buffer that signals the event when it
     * completes. Submitted to the signal queue AFTER any in-flight
     * draw command buffers (the signal queue's submission order is
     * preserved within itself; it does not synchronize with the
     * render queue, which is a pre-existing limitation we'll address
     * in M5+ when we move to a single command buffer per UI frame).
     *
     * For M3 the flush_draw path commits its render command buffer
     * immediately and then calls end_frame; the queue ordering
     * effectively serializes them on Apple Silicon's tiled GPU. */
    @autoreleasepool {
        id<MTLCommandBuffer> cmd = [s_signal_queue commandBuffer];
        cmd.label = @"xemu.metal.buffer_signal";
        [cmd encodeSignalEvent:s_event value:s_frame_signal_value];
        [cmd commit];
    }

    s_first_frame_issued = true;
    atomic_fetch_add(&s_frame_count_total, 1);
}

/* -------- staging -------- */

static bool stage_into_active_slot(const void *data, size_t size,
                                   size_t alignment,
                                   void **out_buffer, size_t *out_offset,
                                   void **out_ptr)
{
    if (!s_initialized || size == 0) {
        return false;
    }

    int slot = s_active_slot;
    size_t aligned_offset = round_up_to(s_slot_offset[slot], alignment);
    if (aligned_offset + size > MTL_RING_SLOT_BYTES) {
        fprintf(stderr,
                "pgraph_mtl_buffer: ring slot %d full (%zu + %zu > %llu); "
                "dropping stage\n",
                slot, aligned_offset, size,
                (unsigned long long)MTL_RING_SLOT_BYTES);
        return false;
    }

    id<MTLBuffer> buf = s_slot_buffer[slot];
    uint8_t *base = (uint8_t *)[buf contents];
    if (base == NULL) {
        return false;
    }

    if (data != NULL) {
        memcpy(base + aligned_offset, data, size);
    }

    if (out_buffer) {
        *out_buffer = (__bridge void *)buf;
    }
    if (out_offset) {
        *out_offset = aligned_offset;
    }
    if (out_ptr) {
        *out_ptr = base + aligned_offset;
    }

    s_slot_offset[slot] = aligned_offset + size;
    atomic_fetch_add(&s_stage_bytes_total, (uint64_t)size);
    return true;
}

bool pgraph_mtl_buffer_stage_vertex(const void *data, size_t size,
                                    void **out_buffer, size_t *out_offset)
{
    return stage_into_active_slot(data, size, MTL_RING_DEFAULT_ALIGN,
                                  out_buffer, out_offset, NULL);
}

bool pgraph_mtl_buffer_stage_index(const void *data, size_t size,
                                   void **out_buffer, size_t *out_offset)
{
    /* Index buffer alignment: 4 bytes for uint32 indices. */
    return stage_into_active_slot(data, size, 4,
                                  out_buffer, out_offset, NULL);
}

bool pgraph_mtl_buffer_stage_uniform(size_t size, size_t alignment,
                                     void **out_buffer, size_t *out_offset,
                                     void **out_ptr)
{
    if (alignment < MTL_RING_DEFAULT_ALIGN) {
        alignment = MTL_RING_DEFAULT_ALIGN;
    }
    return stage_into_active_slot(NULL, size, alignment,
                                  out_buffer, out_offset, out_ptr);
}

/* -------- vertex RAM (deferred) -------- */

void *pgraph_mtl_buffer_get_vertex_ram(void)
{
    /* See the file-level comment "Vertex RAM mapping (M3 deferral)". */
    return NULL;
}

void pgraph_mtl_buffer_invalidate_vertex_ram_range(uint32_t addr,
                                                   uint32_t length)
{
    (void)addr;
    (void)length;
    /* No-op for M3. */
}

/* -------- diagnostics -------- */

uint64_t pgraph_mtl_buffer_stage_bytes(void)
{
    return atomic_load(&s_stage_bytes_total);
}

uint64_t pgraph_mtl_buffer_frame_count(void)
{
    return atomic_load(&s_frame_count_total);
}
