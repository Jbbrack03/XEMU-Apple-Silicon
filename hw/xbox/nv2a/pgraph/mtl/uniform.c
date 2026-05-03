/*
 * NV2A PGRAPH Metal renderer — uniform-buffer marshaling (slice M7.1).
 *
 * See uniform.h for the public contract. The std140 packer walks the
 * `*UniformInfo[]` arrays (declared in declaration order via the
 * UNIFORM_DECL_X macros in glsl/{vsh,psh}.h) and copies typed values
 * from VshUniformValues / PshUniformValues into a flat byte blob.
 *
 * Why std140 and not the spirv-reflect layout vk uses?
 *
 * - The Vulkan-flavored GLSL emitted by `pgraph_glsl_gen_vsh` /
 *   `pgraph_glsl_gen_psh` declares the UBO with `layout(std140)`,
 *   so the spirv-cross MSL backend produces a struct whose member
 *   layout is exactly std140-equivalent. spirv-reflect would
 *   re-derive the same offsets; doing the packing here directly
 *   avoids pulling spirv-reflect into the Metal port.
 *
 * - The vk path (vk/glsl.c::block_to_uniforms via spirv-reflect)
 *   produces ShaderUniform descriptors that match what
 *   uniform_std140 in vk/glsl.h synthesizes manually. This file
 *   uses the manual synthesis form, which is the same algorithm.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"

#include "uniform.h"

#include "hw/xbox/nv2a/pgraph/glsl/common.h"
#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"
#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* These extern declarations duplicate the ones in vsh.c / psh.c so
 * this .c file (per-target compile) can read the metadata arrays. */
extern const UniformInfo VshUniformInfo[];
extern const UniformInfo PshUniformInfo[];

/* Backing for the staging ring lives on the .mm side (because Metal
 * id<MTLBuffer> can't appear in a .c file). The .c side calls into
 * those externs to fetch CPU-mapped memory + finalize the upload. */
extern bool pgraph_mtl_uniform_ring_init(void);
extern void pgraph_mtl_uniform_ring_finalize(void);
extern void pgraph_mtl_uniform_ring_begin_frame(void);
extern void pgraph_mtl_uniform_ring_end_frame(void);

/* Reserve `size` bytes from the current ring slot. Returns a CPU
 * pointer to write into and the (buffer, offset) the encoder uses.
 * Returns NULL on overflow. */
extern void *pgraph_mtl_uniform_ring_reserve(size_t size,
                                             void **out_mtl_buffer,
                                             size_t *out_offset);

/* Counters. */
static _Atomic uint64_t s_pack_count = 0;
static _Atomic uint64_t s_pack_bytes = 0;

bool pgraph_mtl_uniform_init(void)
{
    if (!pgraph_mtl_uniform_ring_init()) {
        return false;
    }
    atomic_store(&s_pack_count, 0);
    atomic_store(&s_pack_bytes, 0);
    return true;
}

void pgraph_mtl_uniform_finalize(void)
{
    pgraph_mtl_uniform_ring_finalize();
}

void pgraph_mtl_uniform_begin_frame(void)
{
    pgraph_mtl_uniform_ring_begin_frame();
}

void pgraph_mtl_uniform_end_frame(void)
{
    pgraph_mtl_uniform_ring_end_frame();
}

/* -------- std140 packer -------- */

/* For std140:
 *   - scalar (float/int/uint): align = 4, stride within struct = 4
 *   - vec2  : align = 8,  size = 8
 *   - vec3  : align = 16, size = 12 (padded to 16 if next member is array)
 *   - vec4  : align = 16, size = 16
 *   - ivec2 : same as vec2
 *   - ivec4 : same as vec4
 *   - mat2  : column-major, each column padded to vec4 → 32 bytes total
 *   - array of T: each element padded up to align(vec4) = 16, count×16
 *
 * This matches uniform_std140() in vk/glsl.h. */
static size_t std140_align_for_type(enum UniformElementType type, size_t count)
{
    /* Arrays always align to vec4. */
    if (count > 1) {
        return 16;
    }
    switch (type) {
    case UniformElementType_float:
    case UniformElementType_int:
    case UniformElementType_uint:
        return 4;
    case UniformElementType_vec2:
    case UniformElementType_ivec2:
        return 8;
    case UniformElementType_vec3:
    case UniformElementType_vec4:
    case UniformElementType_ivec4:
    case UniformElementType_mat2:
        return 16;
    }
    return 16;
}

/* Bytes per array stride (vec4-padded for arrays). For arrays of vec3,
 * spirv-cross still pads to 16. */
static size_t std140_stride_for_type(enum UniformElementType type)
{
    switch (type) {
    case UniformElementType_float:
    case UniformElementType_int:
    case UniformElementType_uint:
        return 16;
    case UniformElementType_vec2:
    case UniformElementType_ivec2:
        return 16;
    case UniformElementType_vec3:
    case UniformElementType_vec4:
    case UniformElementType_ivec4:
        return 16;
    case UniformElementType_mat2:
        /* mat2 = 2 columns of vec4 padding = 32 */
        return 32;
    }
    return 16;
}

/* Element size (no padding) when count == 1. */
static size_t std140_element_size(enum UniformElementType type)
{
    switch (type) {
    case UniformElementType_float:
    case UniformElementType_int:
    case UniformElementType_uint:
        return 4;
    case UniformElementType_vec2:
    case UniformElementType_ivec2:
        return 8;
    case UniformElementType_vec3:
        return 12;
    case UniformElementType_vec4:
    case UniformElementType_ivec4:
        return 16;
    case UniformElementType_mat2:
        return 32;
    }
    return 16;
}

/* Components per typed value (used to know the source step in
 * VshUniformValues / PshUniformValues, where each element is laid out
 * tight). */
static size_t std140_src_element_size(enum UniformElementType type)
{
    switch (type) {
    case UniformElementType_float:
    case UniformElementType_int:
    case UniformElementType_uint:
        return 4;
    case UniformElementType_vec2:
    case UniformElementType_ivec2:
        return 8;
    case UniformElementType_vec3:
        return 12;
    case UniformElementType_vec4:
    case UniformElementType_ivec4:
        return 16;
    case UniformElementType_mat2:
        return 16; /* 4 floats tight in source */
    }
    return 16;
}

static size_t pack_uniforms(const UniformInfo *info_arr, size_t n_uniforms,
                            const void *values_struct,
                            uint8_t *out, size_t out_capacity)
{
    size_t offset = 0;
    for (size_t i = 0; i < n_uniforms; i++) {
        const UniformInfo *u = &info_arr[i];
        size_t align = std140_align_for_type(u->type, u->count);
        offset = (offset + (align - 1)) & ~(align - 1);

        const uint8_t *src = (const uint8_t *)values_struct + u->val_offs;

        if (u->count == 1) {
            size_t sz = std140_element_size(u->type);
            if (offset + sz > out_capacity) {
                return 0;
            }
            if (u->type == UniformElementType_mat2) {
                /* Source: 4 floats tight (m00, m01, m10, m11) — std140
                 * mat2 stored as 2 columns of vec4 (each 16 bytes). */
                memset(out + offset, 0, 32);
                memcpy(out + offset + 0,  src + 0, 8);   /* col0 */
                memcpy(out + offset + 16, src + 8, 8);   /* col1 */
            } else {
                memcpy(out + offset, src, sz);
            }
            offset += sz;
        } else {
            size_t stride = std140_stride_for_type(u->type);
            size_t src_step = std140_src_element_size(u->type);
            for (size_t k = 0; k < u->count; k++) {
                if (offset + stride > out_capacity) {
                    return 0;
                }
                /* Zero pad slot then copy element. */
                memset(out + offset, 0, stride);
                if (u->type == UniformElementType_mat2) {
                    /* mat2[k] : per element occupies 32 bytes (2 cols
                     * of vec4 padding). Source is 4 floats tight. */
                    memcpy(out + offset + 0,  src + 0, 8);
                    memcpy(out + offset + 16, src + 8, 8);
                    /* mat2 uses stride=32; advance offset by 32. */
                    offset += 32;
                } else {
                    memcpy(out + offset, src, src_step);
                    offset += stride;
                }
                src += src_step;
            }
        }
    }
    return offset;
}

/* -------- public API -------- */

size_t pgraph_mtl_uniform_stage_vsh(PGRAPHState *pg,
                                    const void *vsh_state,
                                    void **out_buf,
                                    size_t *out_off)
{
    if (!pg || !vsh_state || !out_buf || !out_off) {
        return 0;
    }
    const VshState *state = (const VshState *)vsh_state;

    /* Build a "all locs valid" array so set_*_uniform_values fills
     * everything. The translated MSL stage hard-references every member
     * of the std140 block (spirv-cross materialises the full struct);
     * cherry-picking subsets isn't safe here. */
    VshUniformLocs locs;
    for (int i = 0; i < VshUniform__COUNT; i++) {
        locs[i] = i + 1;  /* nonzero = present */
    }

    VshUniformValues values;
    memset(&values, 0, sizeof(values));
    pgraph_glsl_set_vsh_uniform_values(pg, state, locs, &values);

    /* Reserve, pack into the buffer, and return. */
    void *mtl_buf = NULL;
    size_t off = 0;
    void *dst = pgraph_mtl_uniform_ring_reserve(PGRAPH_MTL_VSH_UBO_MAX_BYTES,
                                                &mtl_buf, &off);
    if (!dst) {
        return 0;
    }

    size_t packed_size = pack_uniforms(VshUniformInfo, VshUniform__COUNT,
                                       &values,
                                       (uint8_t *)dst,
                                       PGRAPH_MTL_VSH_UBO_MAX_BYTES);
    if (packed_size == 0) {
        return 0;
    }

    *out_buf = mtl_buf;
    *out_off = off;

    atomic_fetch_add(&s_pack_count, 1);
    atomic_fetch_add(&s_pack_bytes, (uint64_t)packed_size);
    return packed_size;
}

size_t pgraph_mtl_uniform_stage_psh(PGRAPHState *pg,
                                    const void *psh_state,
                                    void **out_buf,
                                    size_t *out_off)
{
    (void)psh_state; /* PshUniform locs filled the same way for all states. */
    if (!pg || !out_buf || !out_off) {
        return 0;
    }

    PshUniformLocs locs;
    for (int i = 0; i < PshUniform__COUNT; i++) {
        locs[i] = i + 1;
    }

    PshUniformValues values;
    memset(&values, 0, sizeof(values));
    pgraph_glsl_set_psh_uniform_values(pg, locs, &values);

    /* texScale defaults to 1.0 — vk fills these from per-binding
     * surface_scale; the M7.1 path uses 1.0 (no scale) until the
     * surface-to-texture path lands. */
    for (int i = 0; i < 4; i++) {
        values.texScale[i] = 1.0f;
    }

    void *mtl_buf = NULL;
    size_t off = 0;
    void *dst = pgraph_mtl_uniform_ring_reserve(PGRAPH_MTL_PSH_UBO_MAX_BYTES,
                                                &mtl_buf, &off);
    if (!dst) {
        return 0;
    }

    size_t packed_size = pack_uniforms(PshUniformInfo, PshUniform__COUNT,
                                       &values,
                                       (uint8_t *)dst,
                                       PGRAPH_MTL_PSH_UBO_MAX_BYTES);
    if (packed_size == 0) {
        return 0;
    }

    *out_buf = mtl_buf;
    *out_off = off;

    atomic_fetch_add(&s_pack_count, 1);
    atomic_fetch_add(&s_pack_bytes, (uint64_t)packed_size);
    return packed_size;
}

uint64_t pgraph_mtl_uniform_pack_count(void)
{
    return atomic_load(&s_pack_count);
}

uint64_t pgraph_mtl_uniform_bytes_total(void)
{
    return atomic_load(&s_pack_bytes);
}
