/*
 * NV2A PGRAPH Metal renderer — uniform-buffer marshaling (slice M7.1).
 *
 * Pack VshUniformValues / PshUniformValues into std140 Metal uniform
 * buffers. The translated MSL pipeline ingests them via spirv-cross's
 * `[[buffer(N)]]` attributes; bindings are deterministic via
 * `MSL_ENABLE_DECORATION_BINDING=true` and the matching
 * MTL_VSH_UBO_BINDING / MTL_PSH_UBO_BINDING used by shadergen.c.
 *
 * The std140 packer here mirrors the layout vk's
 * include/qemu/uniform_std140 produces — same generator output, same
 * std140 rules — so the typed values produced by
 * `pgraph_glsl_set_vsh_uniform_values` / `pgraph_glsl_set_psh_uniform_values`
 * round-trip into a Metal-compatible blob without spirv-reflect.
 *
 * The packed blob is uploaded into a per-frame staging ring slot
 * (`pgraph_mtl_uniform_stage`); the encode-time call returns the
 * id<MTLBuffer> + offset that the encoder binds at index N.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_MTL_UNIFORM_H
#define HW_XBOX_NV2A_PGRAPH_MTL_UNIFORM_H

#include "qemu/osdep.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MSL binding indices kept in sync with shadergen.c's
 * MTL_VSH_UBO_BINDING / MTL_PSH_UBO_BINDING / MTL_PSH_TEX_BINDING. */
#define PGRAPH_MTL_VSH_UBO_BINDING_INDEX 0
#define PGRAPH_MTL_PSH_UBO_BINDING_INDEX 1
#define PGRAPH_MTL_PSH_TEX_BINDING_INDEX 2

/* Maximum sizes (bytes) of the std140-packed UBOs. Conservative upper
 * bounds; the actual packed size is reported by the pack functions. */
#define PGRAPH_MTL_VSH_UBO_MAX_BYTES 8192
#define PGRAPH_MTL_PSH_UBO_MAX_BYTES 4096

/* Lifecycle. */
bool pgraph_mtl_uniform_init(void);
void pgraph_mtl_uniform_finalize(void);

/* Frame fence — call once at the start of each draw and at end. The
 * staging ring rotates slots based on these. */
void pgraph_mtl_uniform_begin_frame(void);
void pgraph_mtl_uniform_end_frame(void);

/* Build the VSH uniform UBO from the current PGRAPHState + ShaderState
 * and stage it into a ring slot. Outputs an id<MTLBuffer>* + byte
 * offset suitable for `[encoder setVertexBuffer:offset:atIndex:0]`.
 * Returns the byte size of the packed UBO on success, 0 on failure.
 *
 * The VshState / PshState pointers are typed as `void *` here so this
 * header is includable from the .mm side; the .c implementation casts
 * via the per-target `glsl/{vsh,psh}.h` headers. */
struct PGRAPHState;
size_t pgraph_mtl_uniform_stage_vsh(struct PGRAPHState *pg,
                                    const void *vsh_state,
                                    void **out_buf,
                                    size_t *out_off);
size_t pgraph_mtl_uniform_stage_psh(struct PGRAPHState *pg,
                                    const void *psh_state,
                                    void **out_buf,
                                    size_t *out_off);

/* Counters. */
uint64_t pgraph_mtl_uniform_pack_count(void);
uint64_t pgraph_mtl_uniform_bytes_total(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_XBOX_NV2A_PGRAPH_MTL_UNIFORM_H */
