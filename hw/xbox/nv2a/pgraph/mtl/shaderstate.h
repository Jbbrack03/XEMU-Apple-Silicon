/*
 * NV2A PGRAPH Metal renderer — PipelineKey definition (slice M5).
 *
 * Mirrors vk/renderer.h:64-71 PipelineKey, ported to Metal-flavored
 * pixel formats and vertex descriptors. The key is POD with all
 * padding zeroed so memcmp is the comparison function and fast_hash
 * is the hash function (matching vk/shaders.c's pattern).
 *
 * Used by mtl/shaders.{h,mm}'s LRU cache to dedupe MTLRenderPipelineState
 * compiles. M5 lands the type + cache; the draw-path lookup is wired
 * in alongside texture binding (M6) and combiner-via-framebuffer-fetch
 * (M7), since a cached pipeline cannot be exercised end-to-end without
 * those slices.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_SHADERSTATE_H
#define HW_XBOX_NV2A_PGRAPH_MTL_SHADERSTATE_H

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <stdint.h>

/* Per-attachment Metal render pass state that affects pipeline
 * compilation. Pixel formats are stored as MTLPixelFormat values
 * cast to uint32_t — same convention as the M3/M4 hand-coded
 * pipeline cache. */
typedef struct PgraphMtlRenderPassState {
    uint32_t color_format;  /* MTLPixelFormat or 0 if no color RT */
    uint32_t depth_format;  /* MTLPixelFormat or 0 if no depth RT */
    uint32_t sample_count;  /* MSAA sample count; 1 for non-MSAA */
    uint32_t _padding;
} PgraphMtlRenderPassState;

/* Vertex layout — one entry per active NV2A vertex attribute slot.
 * format is the MTLVertexFormat (uint32_t cast); offset/stride/buffer
 * mirror the Metal MTLVertexAttributeDescriptor / MTLVertexBufferLayout
 * fields. NV2A_VERTEXSHADER_ATTRIBUTES = 16. */
typedef struct PgraphMtlVertexAttribute {
    uint32_t format;        /* MTLVertexFormat or 0 if attribute unused */
    uint32_t offset;
    uint32_t buffer_index;
    uint32_t _padding;
} PgraphMtlVertexAttribute;

typedef struct PgraphMtlVertexBuffer {
    uint32_t stride;
    uint32_t step_function;  /* MTLVertexStepFunction */
    uint32_t step_rate;
    uint32_t _padding;
} PgraphMtlVertexBuffer;

/* The full pipeline cache key. Layout chosen so memcmp-comparison is
 * deterministic across runs (no bool fields packed near padding;
 * everything explicitly sized + aligned). */
typedef struct PgraphMtlPipelineKey {
    bool        clear;             /* Set if this pipeline is the M3 clear */
    uint8_t     _pad0[7];
    PgraphMtlRenderPassState render_pass_state;
    ShaderState shader_state;       /* The full NV2A ShaderState */
    uint32_t    regs[9];           /* Render-state regs (blend, depth-test, etc.) */
    PgraphMtlVertexAttribute attrs[NV2A_VERTEXSHADER_ATTRIBUTES];
    PgraphMtlVertexBuffer    bufs[NV2A_VERTEXSHADER_ATTRIBUTES];
} PgraphMtlPipelineKey;

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_SHADERSTATE_H */
