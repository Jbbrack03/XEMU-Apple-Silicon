/*
 * NV2A PGRAPH Metal renderer — GLSL → SPIR-V → MSL translation (slice M5).
 *
 * See glsl.h for the public contract. This file mirrors the structural
 * template of vk/glsl.c (the GLSL → SPIR-V half) and then routes the
 * resulting SPIR-V through spirv-cross's C API (CompilerMSL backend).
 *
 * Lifecycle: glslang_initialize_process() / finalize must bracket all
 * uses; init/finalize functions here are reference-counted relative to
 * the renderer init/finalize hooks. spirv-cross has no global state.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "glsl.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glslang/Include/glslang_c_interface.h>
#include <spirv_cross_c.h>

static _Atomic int      s_init_refcount     = 0;
static _Atomic uint64_t s_translate_count    = 0;
static _Atomic uint64_t s_translate_failures = 0;

/* Limits copied verbatim from vk/glsl.c — same generator output, same
 * limit set required. */
static const glslang_resource_t s_resource_limits = {
    .max_lights = 32,
    .max_clip_planes = 6,
    .max_texture_units = 32,
    .max_texture_coords = 32,
    .max_vertex_attribs = 64,
    .max_vertex_uniform_components = 4096,
    .max_varying_floats = 64,
    .max_vertex_texture_image_units = 32,
    .max_combined_texture_image_units = 80,
    .max_texture_image_units = 32,
    .max_fragment_uniform_components = 4096,
    .max_draw_buffers = 32,
    .max_vertex_uniform_vectors = 128,
    .max_varying_vectors = 8,
    .max_fragment_uniform_vectors = 16,
    .max_vertex_output_vectors = 16,
    .max_fragment_input_vectors = 15,
    .min_program_texel_offset = -8,
    .max_program_texel_offset = 7,
    .max_clip_distances = 8,
    .max_compute_work_group_count_x = 65535,
    .max_compute_work_group_count_y = 65535,
    .max_compute_work_group_count_z = 65535,
    .max_compute_work_group_size_x = 1024,
    .max_compute_work_group_size_y = 1024,
    .max_compute_work_group_size_z = 64,
    .max_compute_uniform_components = 1024,
    .max_compute_texture_image_units = 16,
    .max_compute_image_uniforms = 8,
    .max_compute_atomic_counters = 8,
    .max_compute_atomic_counter_buffers = 1,
    .max_varying_components = 60,
    .max_vertex_output_components = 64,
    .max_geometry_input_components = 64,
    .max_geometry_output_components = 128,
    .max_fragment_input_components = 128,
    .max_image_units = 8,
    .max_combined_image_units_and_fragment_outputs = 8,
    .max_combined_shader_output_resources = 8,
    .max_image_samples = 0,
    .max_vertex_image_uniforms = 0,
    .max_tess_control_image_uniforms = 0,
    .max_tess_evaluation_image_uniforms = 0,
    .max_geometry_image_uniforms = 0,
    .max_fragment_image_uniforms = 8,
    .max_combined_image_uniforms = 8,
    .max_geometry_texture_image_units = 16,
    .max_geometry_output_vertices = 256,
    .max_geometry_total_output_components = 1024,
    .max_geometry_uniform_components = 1024,
    .max_geometry_varying_components = 64,
    .max_tess_control_input_components = 128,
    .max_tess_control_output_components = 128,
    .max_tess_control_texture_image_units = 16,
    .max_tess_control_uniform_components = 1024,
    .max_tess_control_total_output_components = 4096,
    .max_tess_evaluation_input_components = 128,
    .max_tess_evaluation_output_components = 128,
    .max_tess_evaluation_texture_image_units = 16,
    .max_tess_evaluation_uniform_components = 1024,
    .max_tess_patch_components = 120,
    .max_patch_vertices = 32,
    .max_tess_gen_level = 64,
    .max_viewports = 16,
    .max_vertex_atomic_counters = 0,
    .max_tess_control_atomic_counters = 0,
    .max_tess_evaluation_atomic_counters = 0,
    .max_geometry_atomic_counters = 0,
    .max_fragment_atomic_counters = 8,
    .max_combined_atomic_counters = 8,
    .max_atomic_counter_bindings = 1,
    .max_vertex_atomic_counter_buffers = 0,
    .max_tess_control_atomic_counter_buffers = 0,
    .max_tess_evaluation_atomic_counter_buffers = 0,
    .max_geometry_atomic_counter_buffers = 0,
    .max_fragment_atomic_counter_buffers = 1,
    .max_combined_atomic_counter_buffers = 1,
    .max_atomic_counter_buffer_size = 16384,
    .max_transform_feedback_buffers = 4,
    .max_transform_feedback_interleaved_components = 64,
    .max_cull_distances = 8,
    .max_combined_clip_and_cull_distances = 8,
    .max_samples = 4,
    .max_mesh_output_vertices_nv = 256,
    .max_mesh_output_primitives_nv = 512,
    .max_mesh_work_group_size_x_nv = 32,
    .max_mesh_work_group_size_y_nv = 1,
    .max_mesh_work_group_size_z_nv = 1,
    .max_task_work_group_size_x_nv = 32,
    .max_task_work_group_size_y_nv = 1,
    .max_task_work_group_size_z_nv = 1,
    .max_mesh_view_count_nv = 4,
    .maxDualSourceDrawBuffersEXT = 1,
    .limits = {
        .non_inductive_for_loops = 1,
        .while_loops = 1,
        .do_while_loops = 1,
        .general_uniform_indexing = 1,
        .general_attribute_matrix_vector_indexing = 1,
        .general_varying_indexing = 1,
        .general_sampler_indexing = 1,
        .general_variable_indexing = 1,
        .general_constant_matrix_vector_indexing = 1,
    },
};

bool pgraph_mtl_glsl_init(void)
{
    int prev = atomic_fetch_add(&s_init_refcount, 1);
    if (prev == 0) {
        if (!glslang_initialize_process()) {
            atomic_fetch_sub(&s_init_refcount, 1);
            return false;
        }
    }
    return true;
}

void pgraph_mtl_glsl_finalize(void)
{
    int prev = atomic_fetch_sub(&s_init_refcount, 1);
    if (prev == 1) {
        glslang_finalize_process();
    } else if (prev <= 0) {
        /* Asymmetric finalize. Restore. */
        atomic_fetch_add(&s_init_refcount, 1);
    }
}

uint64_t pgraph_mtl_glsl_translate_count(void)
{
    return atomic_load(&s_translate_count);
}

uint64_t pgraph_mtl_glsl_translate_failures(void)
{
    return atomic_load(&s_translate_failures);
}

static glslang_stage_t to_glslang_stage(PgraphMtlGlslStage stage)
{
    switch (stage) {
    case PGRAPH_MTL_GLSL_STAGE_VERTEX:   return GLSLANG_STAGE_VERTEX;
    case PGRAPH_MTL_GLSL_STAGE_FRAGMENT: return GLSLANG_STAGE_FRAGMENT;
    case PGRAPH_MTL_GLSL_STAGE_GEOMETRY: return GLSLANG_STAGE_GEOMETRY;
    }
    return GLSLANG_STAGE_VERTEX;
}

static char *concat_diag(const char *info, const char *debug,
                         const char *source_or_pp)
{
    size_t li = info ? strlen(info) : 0;
    size_t ld = debug ? strlen(debug) : 0;
    size_t ls = source_or_pp ? strlen(source_or_pp) : 0;
    char *out = (char *)malloc(li + ld + ls + 64);
    if (!out) {
        return NULL;
    }
    snprintf(out, li + ld + ls + 64,
             "[INFO]: %s\n[DEBUG]: %s\n%s\n",
             info ? info : "(null)",
             debug ? debug : "(null)",
             source_or_pp ? source_or_pp : "(null)");
    return out;
}

uint32_t *pgraph_mtl_glsl_compile_to_spv(PgraphMtlGlslStage stage,
                                         const char *glsl_source,
                                         size_t *out_word_count,
                                         char **out_error)
{
    if (out_error) {
        *out_error = NULL;
    }
    if (!glsl_source || !out_word_count) {
        if (out_error) {
            *out_error = strdup("invalid arguments");
        }
        return NULL;
    }
    *out_word_count = 0;

    glslang_input_t input = {
        .language                       = GLSLANG_SOURCE_GLSL,
        .stage                          = to_glslang_stage(stage),
        .client                         = GLSLANG_CLIENT_VULKAN,
        .client_version                 = GLSLANG_TARGET_VULKAN_1_3,
        .target_language                = GLSLANG_TARGET_SPV,
        .target_language_version        = GLSLANG_TARGET_SPV_1_6,
        .code                           = glsl_source,
        .default_version                = 460,
        .default_profile                = GLSLANG_NO_PROFILE,
        .force_default_version_and_profile = false,
        .forward_compatible             = false,
        .messages                       = GLSLANG_MSG_DEFAULT_BIT,
        .resource                       = &s_resource_limits,
    };

    glslang_shader_t *shader = glslang_shader_create(&input);
    if (!shader) {
        if (out_error) {
            *out_error = strdup("glslang_shader_create failed");
        }
        return NULL;
    }

    if (!glslang_shader_preprocess(shader, &input)) {
        if (out_error) {
            *out_error = concat_diag(
                glslang_shader_get_info_log(shader),
                glslang_shader_get_info_debug_log(shader),
                glsl_source);
        }
        glslang_shader_delete(shader);
        return NULL;
    }

    if (!glslang_shader_parse(shader, &input)) {
        if (out_error) {
            *out_error = concat_diag(
                glslang_shader_get_info_log(shader),
                glslang_shader_get_info_debug_log(shader),
                glslang_shader_get_preprocessed_code(shader));
        }
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_program_t *program = glslang_program_create();
    if (!program) {
        if (out_error) {
            *out_error = strdup("glslang_program_create failed");
        }
        glslang_shader_delete(shader);
        return NULL;
    }
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT |
                                       GLSLANG_MSG_VULKAN_RULES_BIT)) {
        if (out_error) {
            *out_error = concat_diag(
                glslang_program_get_info_log(program),
                glslang_program_get_info_debug_log(program),
                "(link)");
        }
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_spv_options_t spv_options = { .validate = true };
    glslang_program_SPIRV_generate_with_options(program, input.stage,
                                                &spv_options);

    size_t word_count = glslang_program_SPIRV_get_size(program);
    if (word_count == 0) {
        if (out_error) {
            *out_error = strdup("SPIR-V generation produced zero words");
        }
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }

    uint32_t *out = (uint32_t *)malloc(word_count * sizeof(uint32_t));
    if (!out) {
        if (out_error) {
            *out_error = strdup("OOM allocating SPIR-V buffer");
        }
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }
    glslang_program_SPIRV_get(program, (unsigned int *)out);
    *out_word_count = word_count;

    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return out;
}

static SpvExecutionModel spv_exec_model_for_stage(PgraphMtlGlslStage stage)
{
    switch (stage) {
    case PGRAPH_MTL_GLSL_STAGE_VERTEX:   return SpvExecutionModelVertex;
    case PGRAPH_MTL_GLSL_STAGE_FRAGMENT: return SpvExecutionModelFragment;
    case PGRAPH_MTL_GLSL_STAGE_GEOMETRY: return SpvExecutionModelGeometry;
    }
    return SpvExecutionModelVertex;
}

static char *cross_compile_msl(PgraphMtlGlslStage stage,
                               const uint32_t *spirv,
                               size_t spirv_word_count,
                               char **out_error)
{
    spvc_context        ctx       = NULL;
    spvc_parsed_ir      ir        = NULL;
    spvc_compiler       compiler  = NULL;
    spvc_compiler_options options = NULL;

    spvc_result r = spvc_context_create(&ctx);
    if (r != SPVC_SUCCESS || !ctx) {
        if (out_error) {
            *out_error = strdup("spvc_context_create failed");
        }
        return NULL;
    }

    r = spvc_context_parse_spirv(ctx, (const SpvId *)spirv,
                                 spirv_word_count, &ir);
    if (r != SPVC_SUCCESS) {
        if (out_error) {
            const char *e = spvc_context_get_last_error_string(ctx);
            *out_error = strdup(e ? e : "spvc_context_parse_spirv failed");
        }
        spvc_context_destroy(ctx);
        return NULL;
    }

    r = spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir,
                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                     &compiler);
    if (r != SPVC_SUCCESS) {
        if (out_error) {
            const char *e = spvc_context_get_last_error_string(ctx);
            *out_error = strdup(e ? e : "spvc_context_create_compiler failed");
        }
        spvc_context_destroy(ctx);
        return NULL;
    }

    r = spvc_compiler_create_compiler_options(compiler, &options);
    if (r != SPVC_SUCCESS) {
        if (out_error) {
            const char *e = spvc_context_get_last_error_string(ctx);
            *out_error = strdup(e ? e :
                                "spvc_compiler_create_compiler_options failed");
        }
        spvc_context_destroy(ctx);
        return NULL;
    }

    /* MSL 2.3 — covers Apple Silicon GPUs (family 7+). */
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_VERSION,
                                   SPVC_MAKE_MSL_VERSION(2, 3, 0));
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_PLATFORM,
                                   SPVC_MSL_PLATFORM_MACOS);
    /* Framebuffer fetch groundwork for M7. The flag tells spirv-cross
     * to translate input attachments / framebuffer-fetch loads to
     * `[[color(N)]]` MSL inputs rather than emulating with subpass
     * loads. Doesn't affect generators that don't emit subpass loads
     * (M5's GLSL doesn't), but landing the flag now keeps the option
     * matrix stable for M7. */
    spvc_compiler_options_set_bool(options,
        SPVC_COMPILER_OPTION_MSL_FRAMEBUFFER_FETCH_SUBPASS, SPVC_TRUE);
    /* Use the SPIR-V binding decorations directly. The Vulkan-flavored
     * GLSL generator already emits `layout(set=N, binding=M)` on every
     * UBO / sampler / storage resource; with this flag spirv-cross
     * preserves those numbers in the MSL output's [[buffer(N)]] /
     * [[texture(N)]] / [[sampler(N)]] slots. Without it spirv-cross
     * auto-numbers, which makes the binding scheme non-deterministic
     * across stages. */
    spvc_compiler_options_set_bool(options,
        SPVC_COMPILER_OPTION_MSL_ENABLE_DECORATION_BINDING, SPVC_TRUE);
    /* Apple's depth convention is upper-left, [0, 1]; SPIR-V default
     * is upper-left + [-1, 1] — fixup_clipspace forces a depth-range
     * remap to [0, 1] in the vertex shader output. We invert Y at
     * pipeline build time via flip_vertex_y. */
    spvc_compiler_options_set_bool(options,
        SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION, SPVC_TRUE);

    r = spvc_compiler_install_compiler_options(compiler, options);
    if (r != SPVC_SUCCESS) {
        if (out_error) {
            const char *e = spvc_context_get_last_error_string(ctx);
            *out_error = strdup(e ? e :
                                "spvc_compiler_install_compiler_options failed");
        }
        spvc_context_destroy(ctx);
        return NULL;
    }

    /* Stage-context decoration. spirv-cross's MSL backend already does
     * stage discovery from the entry point; this is a no-op for
     * single-entry-point modules but keeps the dispatch consistent
     * with future multi-entry shaders. */
    (void)spv_exec_model_for_stage(stage);

    const char *msl_source = NULL;
    r = spvc_compiler_compile(compiler, &msl_source);
    if (r != SPVC_SUCCESS || msl_source == NULL) {
        if (out_error) {
            const char *e = spvc_context_get_last_error_string(ctx);
            *out_error = strdup(e ? e : "spvc_compiler_compile failed");
        }
        spvc_context_destroy(ctx);
        return NULL;
    }

    /* msl_source is owned by the context; copy it before destroy. */
    char *out = strdup(msl_source);
    spvc_context_destroy(ctx);

    if (!out && out_error) {
        *out_error = strdup("OOM duplicating MSL source");
    }
    return out;
}

char *pgraph_mtl_glsl_translate_to_msl(PgraphMtlGlslStage stage,
                                       const char *glsl_source,
                                       char **out_error)
{
    if (out_error) {
        *out_error = NULL;
    }
    if (!glsl_source) {
        if (out_error) {
            *out_error = strdup("null glsl_source");
        }
        atomic_fetch_add(&s_translate_failures, 1);
        return NULL;
    }

    size_t word_count = 0;
    char *spv_err = NULL;
    uint32_t *spv = pgraph_mtl_glsl_compile_to_spv(stage, glsl_source,
                                                   &word_count, &spv_err);
    if (!spv) {
        if (out_error) {
            *out_error = spv_err;
        } else if (spv_err) {
            free(spv_err);
        }
        atomic_fetch_add(&s_translate_failures, 1);
        return NULL;
    }

    char *msl_err = NULL;
    char *msl = cross_compile_msl(stage, spv, word_count, &msl_err);
    free(spv);

    if (!msl) {
        if (out_error) {
            *out_error = msl_err;
        } else if (msl_err) {
            free(msl_err);
        }
        atomic_fetch_add(&s_translate_failures, 1);
        return NULL;
    }

    atomic_fetch_add(&s_translate_count, 1);
    return msl;
}
