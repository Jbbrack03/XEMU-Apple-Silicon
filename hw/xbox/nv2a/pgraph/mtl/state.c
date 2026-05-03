/*
 * NV2A PGRAPH Metal renderer — state-to-PipelineKey conversion (slice M7).
 *
 * See state.h for the public contract. This file is .c (per-target) so
 * it can include nv2a_int.h transitively via pgraph.h and the glsl
 * headers; the .mm side never includes it.
 *
 * Mirrors vk/draw.c::init_pipeline_key + the format mapping in
 * vk/vertex.c::pgraph_vk_bind_vertex_attributes. Differences from VK:
 *
 *   - Outputs MTLVertexFormat enum values (cast to uint32_t) instead of
 *     VkFormat. The mapping table is hardcoded against Metal's public
 *     MTLVertexFormat enum (see state.h table).
 *   - Vertex layouts mirror NV2A attribute slots 1:1 (16 attributes /
 *     16 buffer layouts). VK's renderer uses a denser packing
 *     (`num_active_vertex_*`); we keep the slot indexing because the
 *     PgraphMtlPipelineKey already sizes its arrays to 16 and the GLSL
 *     generator emits `[[attribute(N)]]` based on NV2A slot index.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"

#include "state.h"
#include "shaderstate.h"

#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* MTLVertexFormat enum values, taken verbatim from Apple's public
 * <Metal/MTLVertexDescriptor.h> on macOS 14 SDK. Hardcoded so this .c
 * file stays Metal-header-free (Metal headers are ObjC and don't compile
 * cleanly here). 0 = Invalid. */
enum {
    MTL_VFMT_INVALID                = 0,
    MTL_VFMT_UCHAR2                 = 1,
    MTL_VFMT_UCHAR3                 = 2,
    MTL_VFMT_UCHAR4                 = 3,
    MTL_VFMT_CHAR2                  = 4,
    MTL_VFMT_CHAR3                  = 5,
    MTL_VFMT_CHAR4                  = 6,
    MTL_VFMT_UCHAR2_NORMALIZED      = 7,
    MTL_VFMT_UCHAR3_NORMALIZED      = 8,
    MTL_VFMT_UCHAR4_NORMALIZED      = 9,
    MTL_VFMT_CHAR2_NORMALIZED       = 10,
    MTL_VFMT_CHAR3_NORMALIZED       = 11,
    MTL_VFMT_CHAR4_NORMALIZED       = 12,
    MTL_VFMT_USHORT2                = 13,
    MTL_VFMT_USHORT3                = 14,
    MTL_VFMT_USHORT4                = 15,
    MTL_VFMT_SHORT2                 = 16,
    MTL_VFMT_SHORT3                 = 17,
    MTL_VFMT_SHORT4                 = 18,
    MTL_VFMT_USHORT2_NORMALIZED     = 19,
    MTL_VFMT_USHORT3_NORMALIZED     = 20,
    MTL_VFMT_USHORT4_NORMALIZED     = 21,
    MTL_VFMT_SHORT2_NORMALIZED      = 22,
    MTL_VFMT_SHORT3_NORMALIZED      = 23,
    MTL_VFMT_SHORT4_NORMALIZED      = 24,
    MTL_VFMT_HALF2                  = 25,
    MTL_VFMT_HALF3                  = 26,
    MTL_VFMT_HALF4                  = 27,
    MTL_VFMT_FLOAT                  = 28,
    MTL_VFMT_FLOAT2                 = 29,
    MTL_VFMT_FLOAT3                 = 30,
    MTL_VFMT_FLOAT4                 = 31,
    MTL_VFMT_INT                    = 32,
    MTL_VFMT_INT2                   = 33,
    MTL_VFMT_INT3                   = 34,
    MTL_VFMT_INT4                   = 35,
    MTL_VFMT_UINT                   = 36,
    MTL_VFMT_UINT2                  = 37,
    MTL_VFMT_UINT3                  = 38,
    MTL_VFMT_UINT4                  = 39,
    MTL_VFMT_INT1010102_NORMALIZED  = 40,
    MTL_VFMT_UINT1010102_NORMALIZED = 41,
    MTL_VFMT_UCHAR4_NORMALIZED_BGRA = 42,
    MTL_VFMT_UCHAR                  = 45,
    MTL_VFMT_CHAR                   = 46,
    MTL_VFMT_UCHAR_NORMALIZED       = 47,
    MTL_VFMT_CHAR_NORMALIZED        = 48,
    MTL_VFMT_USHORT                 = 49,
    MTL_VFMT_SHORT                  = 50,
    MTL_VFMT_USHORT_NORMALIZED      = 51,
    MTL_VFMT_SHORT_NORMALIZED       = 52,
    MTL_VFMT_HALF                   = 53,
};

uint32_t pgraph_mtl_translate_vertex_format(uint32_t nv097_format_type,
                                            uint32_t count)
{
    if (count == 0 || count > 4) {
        return MTL_VFMT_INVALID;
    }

    switch (nv097_format_type) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        switch (count) {
        case 1: return MTL_VFMT_FLOAT;
        case 2: return MTL_VFMT_FLOAT2;
        case 3: return MTL_VFMT_FLOAT3;
        case 4: return MTL_VFMT_FLOAT4;
        }
        break;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        switch (count) {
        case 1: return MTL_VFMT_UCHAR_NORMALIZED;
        case 2: return MTL_VFMT_UCHAR2_NORMALIZED;
        case 3: return MTL_VFMT_UCHAR3_NORMALIZED;
        case 4: return MTL_VFMT_UCHAR4_NORMALIZED;
        }
        break;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        /* D3D-style 8-bit normalized, BGRA swizzle. NV2A only emits
         * count == 4 for this. */
        if (count == 4) {
            return MTL_VFMT_UCHAR4_NORMALIZED_BGRA;
        }
        break;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        switch (count) {
        case 1: return MTL_VFMT_SHORT_NORMALIZED;
        case 2: return MTL_VFMT_SHORT2_NORMALIZED;
        case 3: return MTL_VFMT_SHORT3_NORMALIZED;
        case 4: return MTL_VFMT_SHORT4_NORMALIZED;
        }
        break;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        /* int16 raw — Metal's "Short" formats. (Vulkan calls these
         * SSCALED; same on-wire data, different sampling semantics.) */
        switch (count) {
        case 1: return MTL_VFMT_SHORT;
        case 2: return MTL_VFMT_SHORT2;
        case 3: return MTL_VFMT_SHORT3;
        case 4: return MTL_VFMT_SHORT4;
        }
        break;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        /* 3 signed normalized 10/11/11 components packed into 32 bits.
         * Metal's Int1010102_Normalized is the closest match (one extra
         * bit on R, one fewer on A — but A is forced to 1.0 by the
         * pipeline anyway). vk renderer also flags this with a
         * shader-side conversion (compressed_attrs); the same shader
         * post-conversion still applies on Metal. */
        if (count == 1) {
            return MTL_VFMT_INT1010102_NORMALIZED;
        }
        break;

    default:
        break;
    }
    return MTL_VFMT_INVALID;
}

/* MTLVertexStepFunction values:
 *   Constant      = 0
 *   PerVertex     = 1
 *   PerInstance   = 2
 *   PerPatch      = 3
 *   PerPatchControlPoint = 4
 */
#define MTL_VFN_PER_VERTEX 1

bool pgraph_mtl_build_pipeline_key(NV2AState *d,
                                   uint32_t color_format,
                                   uint32_t depth_format,
                                   uint32_t sample_count,
                                   PgraphMtlPipelineKey *out_key)
{
    if (d == NULL || out_key == NULL) {
        return false;
    }
    PGRAPHState *pg = &d->pgraph;

    memset(out_key, 0, sizeof(*out_key));
    out_key->clear = false;
    out_key->render_pass_state.color_format = color_format;
    out_key->render_pass_state.depth_format = depth_format;
    out_key->render_pass_state.sample_count = sample_count ? sample_count : 1;

    /* Shader state — full ShaderState walk.  The generator zeros padding
     * (memset in pgraph_glsl_get_shader_state) so memcmp comparison is
     * deterministic.  This is the same call the GL and VK renderers make
     * on a shader-state dirty event. */
    out_key->shader_state = pgraph_glsl_get_shader_state(pg);

    /* Per-pipeline reg snapshot. The 9-entry order matches vk/draw.c's
     * init_pipeline_key for cross-renderer parity (so the two caches
     * share the same effective key shape). */
    static const unsigned int regs_to_snapshot[9] = {
        NV_PGRAPH_BLEND,
        NV_PGRAPH_BLENDCOLOR,
        NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,
        NV_PGRAPH_CONTROL_2,
        NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER,
        NV_PGRAPH_ZOFFSETBIAS,
        NV_PGRAPH_ZOFFSETFACTOR,
    };
    for (int i = 0; i < 9; i++) {
        out_key->regs[i] = pgraph_reg_r(pg, regs_to_snapshot[i]);
    }

    /* Per-attribute / per-buffer descriptors. We map NV2A attribute
     * slot N → both attrs[N] and bufs[N] (1:1 indexing). Slots with
     * count == 0 (uniform_attrs) are left zero so the pipeline build
     * skips them. */
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->count == 0) {
            continue;
        }

        /* For inline_buffer-driven draws (the M3/M4 path), the attr
         * format is whatever the per-attribute inline_buffer staging
         * emits — always 4 floats — and the format/stride must reflect
         * that, NOT attr->format/attr->size which describe the raw
         * vertex_data_array_format if any. The
         * inline_buffer_populated bit signals which of the two paths
         * is in flight per attribute. */
        uint32_t mtl_format = MTL_VFMT_INVALID;
        uint32_t stride = 0;
        if (attr->inline_buffer_populated) {
            mtl_format = MTL_VFMT_FLOAT4;
            stride     = 4 * (uint32_t)sizeof(float);
        } else {
            mtl_format = pgraph_mtl_translate_vertex_format(attr->format,
                                                            attr->count);
            stride = (uint32_t)attr->stride;
        }
        if (mtl_format == MTL_VFMT_INVALID || stride == 0) {
            continue;
        }

        out_key->attrs[i].format       = mtl_format;
        out_key->attrs[i].offset       = 0;  /* 1:1 buffer→attr layout */
        out_key->attrs[i].buffer_index = (uint32_t)i;

        out_key->bufs[i].stride        = stride;
        out_key->bufs[i].step_function = MTL_VFN_PER_VERTEX;
        out_key->bufs[i].step_rate     = 1;
    }

    return true;
}
