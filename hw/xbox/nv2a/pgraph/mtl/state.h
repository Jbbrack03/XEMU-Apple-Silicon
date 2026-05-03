/*
 * NV2A PGRAPH Metal renderer — state-to-PipelineKey conversion (slice M7).
 *
 * Walks PGRAPHState and produces a fully populated PgraphMtlPipelineKey
 * suitable for `pgraph_mtl_shaders_get_pipeline`. Mirrors the structural
 * pattern of vk/draw.c::init_pipeline_key + vk/vertex.c's vertex_*
 * descriptor build, condensed for Metal's MTLVertexFormat enum.
 *
 * This file is the C-side bridge between PGRAPHState (per-target) and
 * the Metal-flavored PipelineKey. The .c side already includes the
 * per-target glsl headers (psh.h / vsh.h via shaderstate.h); the .mm
 * side never sees this file.
 *
 * Vertex format mapping (NV097 → MTLVertexFormat enum value):
 *
 *   Type           | count=1            count=2             count=3             count=4
 *   ---------------+---------------------------------------------------------------
 *   F (float)      | Float    (28)      Float2  (29)        Float3  (30)        Float4  (31)
 *   UB_OGL (u8 N)  | UCharNorm (5)*     UChar2Norm (6)*     UChar3Norm (7)*     UChar4Norm (8)
 *   UB_D3D (u8 N)  |  -                  -                   -                  UChar4Norm_BGRA (35)
 *   S1 (i16 N)     | ShortNorm (15)*    Short2Norm (16)*    Short3Norm (17)*    Short4Norm (18)
 *   S32K (i16 raw) | Short    (19)*     Short2  (20)*       Short3  (21)*       Short4  (22)
 *   CMP (10:11:11) | Int1010102Normalized (40)
 *
 * (*) Some MTLVertexFormat enum values for non-4-component variants
 * exist on macOS 10.13+; we use them as needed. Values reflect Metal's
 * public MTLVertexFormat enum.
 *
 * The Apple Silicon NV2A geometry-shader bypass means non-fill polygon
 * modes / flat-non-first-provoking are not exercised through the Metal
 * port (see metal-renderer-plan.md §3.8); the produced key reflects the
 * eligible-draw shader state only.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_STATE_H
#define HW_XBOX_NV2A_PGRAPH_MTL_STATE_H

#include "shaderstate.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NV2AState;
typedef struct NV2AState NV2AState;

/*
 * Build a PipelineKey from the current PGRAPHState. The output `key`
 * is fully populated (memset-zeroed first so memcmp comparison works
 * deterministically).
 *
 * Inputs:
 *   d         — NV2A state.
 *   color_format / depth_format — MTLPixelFormat values from the
 *                                 surface manager (cast to uint32_t).
 *   sample_count — MSAA sample count; 1 for non-MSAA.
 *
 * Returns true on success, false if the shader state cannot be
 * generated (in which case the caller should fall back to the
 * passthrough path).
 */
bool pgraph_mtl_build_pipeline_key(NV2AState *d,
                                   uint32_t color_format,
                                   uint32_t depth_format,
                                   uint32_t sample_count,
                                   PgraphMtlPipelineKey *key);

/*
 * Translate an NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_* value plus a
 * component count into the MTLVertexFormat enum value (cast to
 * uint32_t). Returns 0 if the type/count combination is invalid.
 *
 * Exposed for the test harness; the .c-side caller is also welcome to
 * use this directly.
 */
uint32_t pgraph_mtl_translate_vertex_format(uint32_t nv097_format_type,
                                            uint32_t component_count);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_STATE_H */
