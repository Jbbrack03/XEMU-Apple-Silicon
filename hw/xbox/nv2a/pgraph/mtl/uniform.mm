/*
 * NV2A PGRAPH Metal renderer — uniform-buffer staging ring (slice M7.1).
 *
 * The .mm side owns the id<MTLBuffer> ring; the .c side (uniform.c)
 * walks the std140 packing and calls into the externs declared here.
 *
 * Ring shape: 4 slots × 4 MiB each.  Slots rotate per-frame so the GPU
 * can be reading a previous slot while the CPU writes the next one.
 * Each slot is `MTLResourceStorageModeShared | CPUCacheModeWriteCombined`
 * so writes don't pollute the CPU read-cache.
 *
 * The buffer.mm ring exists for vertex/index data; we keep this ring
 * separate so the uniform path doesn't compete for the same backing
 * (uniforms are written every draw; vertex staging is bigger but less
 * frequent).
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern "C" void *xemu_metal_get_device(void);

#define MTL_UNIFORM_RING_SLOTS    4
#define MTL_UNIFORM_RING_SLOT_SZ  (4 * 1024 * 1024)

static id<MTLDevice>  s_device = nil;
static id<MTLBuffer>  s_slots[MTL_UNIFORM_RING_SLOTS];
static int            s_slot_idx = 0;
static size_t         s_slot_off = 0;
static bool           s_init = false;

extern "C" bool pgraph_mtl_uniform_ring_init(void)
{
    if (s_init) {
        return true;
    }
    s_device = (__bridge id<MTLDevice>)xemu_metal_get_device();
    if (s_device == nil) {
        fprintf(stderr,
                "pgraph_mtl_uniform_ring_init: no Metal device\n");
        return false;
    }

    @autoreleasepool {
        for (int i = 0; i < MTL_UNIFORM_RING_SLOTS; i++) {
            id<MTLBuffer> b = [s_device
                newBufferWithLength:MTL_UNIFORM_RING_SLOT_SZ
                            options:MTLResourceStorageModeShared |
                                    MTLResourceCPUCacheModeWriteCombined];
            if (b == nil) {
                for (int j = 0; j < i; j++) {
                    s_slots[j] = nil;
                }
                return false;
            }
            b.label = [NSString stringWithFormat:@"xemu.metal.ubo_ring_%d", i];
            s_slots[i] = b;
        }
    }
    s_slot_idx = 0;
    s_slot_off = 0;
    s_init = true;
    return true;
}

extern "C" void pgraph_mtl_uniform_ring_finalize(void)
{
    if (!s_init) {
        return;
    }
    for (int i = 0; i < MTL_UNIFORM_RING_SLOTS; i++) {
        s_slots[i] = nil;
    }
    s_device = nil;
    s_init = false;
}

extern "C" void pgraph_mtl_uniform_ring_begin_frame(void)
{
    if (!s_init) {
        return;
    }
    /* Advance to the next slot; reset offset. */
    s_slot_idx = (s_slot_idx + 1) % MTL_UNIFORM_RING_SLOTS;
    s_slot_off = 0;
}

extern "C" void pgraph_mtl_uniform_ring_end_frame(void)
{
    /* No-op; commit is implicit via the encoder. */
}

extern "C" void *
pgraph_mtl_uniform_ring_reserve(size_t size,
                                void **out_mtl_buffer,
                                size_t *out_offset)
{
    if (!s_init || size == 0) {
        return NULL;
    }
    /* Align to 256 bytes — required by Metal for setVertexBuffer offset
     * on macOS (ConstantBufferAlignmentForBufferLayout requires 4-byte
     * alignment, but for non-vertex/non-index buffer offsets used as
     * UBOs we use 256 to satisfy Apple GPU ConstantBufferAlignment). */
    s_slot_off = (s_slot_off + 255) & ~((size_t)255);

    if (s_slot_off + size > MTL_UNIFORM_RING_SLOT_SZ) {
        /* Spill to next slot. */
        s_slot_idx = (s_slot_idx + 1) % MTL_UNIFORM_RING_SLOTS;
        s_slot_off = 0;
    }
    if (size > MTL_UNIFORM_RING_SLOT_SZ) {
        return NULL;
    }

    id<MTLBuffer> buf = s_slots[s_slot_idx];
    void *cpu = (uint8_t *)buf.contents + s_slot_off;

    if (out_mtl_buffer) {
        *out_mtl_buffer = (__bridge void *)buf;
    }
    if (out_offset) {
        *out_offset = s_slot_off;
    }

    s_slot_off += size;
    return cpu;
}
