/*
 * Geforce NV2A PGRAPH Metal Renderer (slice M2 — surface manager + clear)
 *
 * Apple Silicon performance fork. The Metal renderer's NV2A-side ops
 * delegate to the surface manager (mtl/surface.mm) and heap manager
 * (mtl/heap.mm) for clear-only correctness. Drawing, textures, shader
 * translation, and MSAA are explicit no-ops here; they land in
 * subsequent slices per docs/apple-silicon/metal-renderer-plan.md.
 *
 * The file extension is .c (not .m / .mm) intentionally — meson's
 * per-target c_args (which add -DCOMPILING_PER_TARGET / -DCONFIG_TARGET
 * / -DCONFIG_DEVICES required by nv2a_int.h transitively) propagate to
 * .c compiles but not to .m / .mm compiles in this build setup. This
 * .c file is the only place in the mtl/ subdir that can include
 * nv2a_int.h. ObjC/Metal API usage lives in heap.mm and surface.mm,
 * which take/return opaque uint32 / void* values and never touch
 * NV2AState directly.
 *
 * Selecting display.renderer = METAL with this slice will boot xemu and
 * (per the M2 exit gate) show the cleared color in the window for any
 * game that issues clear_surface but no draws yet — typically the
 * black/blue Xbox boot splash before the first 3D draw call.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "ui/xemu-settings.h"

#include "buffer.h"
#include "draw.h"
#include "format.h"
#include "glsl.h"
#include "heap.h"
#include "index_gen.h"
#include "pipeline.h"
#include "shader_validation.h"
#include "shaders.h"
#include "shaderstate.h"
#include "state.h"
#include "surface.h"
#include "texture.h"
#include "uniform.h"

/* Shared eligibility helpers for native_tri_depth / native_quad.
 * Defined in glsl/geom.c; declared via glsl/shaders.h which the GL
 * renderer also uses (gl/renderer.h:36). The Metal renderer uses
 * these so the eligibility rules cannot drift between GL and Metal —
 * same input, same answer. */
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

/* M7 env-var flags (latched once at first use). */
static bool s_force_passthrough_cached = false;
static int  s_force_passthrough        = -1;
static bool s_use_translated_cached    = false;
static int  s_use_translated           = -1;

/* M11: effective MSAA sample count (1 = off; 2/4/8 when on). Latched
 * at pgraph_mtl_init from XEMU_METAL_MSAA, clamped against the active
 * device's `[device supportsTextureSampleCount:N]`. Per the M11 plan
 * MSAA is treated as session-fixed: changing the env var requires a
 * restart so the pipeline cache does not balloon with sample-count
 * variants. */
static uint32_t s_metal_msaa_sample_count = 1;

/* Parse XEMU_METAL_MSAA into a sample count. Returns 1 (off) when
 * the var is unset, 0/1, unparseable, or not in {2, 4, 8}. */
static uint32_t parse_metal_msaa_env(uint32_t *out_requested)
{
    if (out_requested) {
        *out_requested = 0;
    }
    const char *value = getenv("XEMU_METAL_MSAA");
    if (!value || !value[0]) {
        return 1;
    }
    char *endp = NULL;
    unsigned long parsed = strtoul(value, &endp, 10);
    if (!endp || *endp != '\0') {
        return 1;
    }
    if (out_requested) {
        *out_requested = (uint32_t)parsed;
    }
    if (parsed == 2 || parsed == 4 || parsed == 8) {
        return (uint32_t)parsed;
    }
    return 1;
}

static bool mtl_force_passthrough(void)
{
    if (!s_force_passthrough_cached) {
        const char *e = getenv("XEMU_METAL_FORCE_PASSTHROUGH");
        s_force_passthrough = (e && e[0] && e[0] != '0') ? 1 : 0;
        s_force_passthrough_cached = true;
    }
    return s_force_passthrough != 0;
}

static bool mtl_use_translated_pipeline(void)
{
    if (!s_use_translated_cached) {
        const char *e = getenv("XEMU_METAL_TRANSLATED_PIPELINE");
        /* Default OFF for M7: the M5 translator + M5/M6 pipeline cache
         * are wired (state-to-key + lookup), but uniform-buffer marshaling
         * + per-stage texture binding are intentionally deferred. The
         * production default stays on the M3/M4 hand-coded passthrough
         * pipeline so M0–M6 visible behavior is preserved. Setting
         * XEMU_METAL_TRANSLATED_PIPELINE=1 enters the translated path
         * for development / bisection. */
        s_use_translated = (e && e[0] && e[0] != '0') ? 1 : 0;
        s_use_translated_cached = true;
    }
    return s_use_translated != 0;
}

/* M7 counters — exposed via accessors at file end. */
#include <stdatomic.h>
static _Atomic uint64_t s_pipeline_key_built     = 0;
static _Atomic uint64_t s_pipeline_translated_ok = 0;
static _Atomic uint64_t s_pipeline_translated_fb = 0;
/* M8: counts draws skipped because the translated-pipeline build was
 * still in flight. Independent of s_pipeline_translated_fb (which
 * counts permanent build failures). */
static _Atomic uint64_t s_draws_skipped_pending  = 0;

static void pgraph_mtl_sync(NV2AState *d)
{
    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_mtl_flush(NV2AState *d)
{
    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_mtl_process_pending(NV2AState *d)
{
    if (
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)
        ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_mtl_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_mtl_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_mtl_clear_report_value(NV2AState *d)
{
}

/* Decode the surface dimensions from PGRAPHState. Mirrors
 * vk/surface.c::get_surface_dimensions. */
static void mtl_get_surface_dimensions(PGRAPHState const *pg,
                                       unsigned int *width,
                                       unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1u << pg->surface_shape.log_width;
        *height = 1u << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

static void pgraph_mtl_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    if (!write_color && !write_zeta) {
        return;
    }

    pg->clearing = true;

    /* Decode shape. M2 does not yet implement the full surface
     * lifecycle (vk/surface.c::pgraph_vk_surface_update); we just
     * ensure a binding exists for whichever aspect(s) the caller is
     * clearing. */
    unsigned int width = 0, height = 0;
    mtl_get_surface_dimensions(pg, &width, &height);

    pgraph_apply_anti_aliasing_factor(pg, &width, &height);
    pgraph_apply_scaling_factor(pg, &width, &height);

    if (write_color && pg->surface_shape.color_format) {
        pgraph_mtl_surface_ensure_color(width, height,
                                        pg->surface_shape.color_format);
    }
    if (write_zeta && pg->surface_shape.zeta_format) {
        pgraph_mtl_surface_ensure_depth(width, height,
                                        pg->surface_shape.zeta_format);
    }

    /* Decode clear color and depth. */
    float rgba[4] = { 0, 0, 0, 1 };
    if (write_color && pg->surface_shape.color_format) {
        pgraph_get_clear_color(pg, rgba);
    }

    float depth = 1.0f;
    int   stencil = 0;
    if (write_zeta && pg->surface_shape.zeta_format) {
        pgraph_get_clear_depth_stencil_value(pg, &depth, &stencil);
    }

    /* M2 ignores the per-channel write mask in NV097_CLEAR_SURFACE_R/G/B/A
     * and the clear-rect scissor (renders the full surface). The exit
     * gate for M2 is "the game's cleared color is visible in the
     * window"; per-channel and per-rect refinement land with M3+ when
     * the render-pass machinery is reused for draws. */

    pgraph_mtl_surface_clear(write_color, rgba, write_zeta, depth);

    pg->surface_color.draw_dirty |= write_color;
    pg->surface_zeta.draw_dirty  |= write_zeta;

    pg->clearing = false;
}

static void pgraph_mtl_draw_begin(NV2AState *d)
{
    /* M3: nothing critical to do here; flush_draw owns the actual
     * encode. M5+ will use this hook for shader binding / dirty-state
     * propagation, mirroring vk/draw.c::pgraph_vk_draw_begin. */
    (void)d;
}

static void pgraph_mtl_draw_end(NV2AState *d)
{
    (void)d;
}

static void pgraph_mtl_flip_stall(NV2AState *d)
{
    (void)d;
}

/* MTLPrimitiveType values, mirrored from <Metal/MTLRenderCommandEncoder.h>.
 * Defined here because renderer.c cannot include Metal headers
 * (per-target preprocessor flag boundary). */
enum {
    MTL_PRIM_POINT          = 0,
    MTL_PRIM_LINE           = 1,
    MTL_PRIM_LINE_STRIP     = 2,
    MTL_PRIM_TRIANGLE       = 3,
    MTL_PRIM_TRIANGLE_STRIP = 4,
};

/* Translate NV2A primitive_mode to MTLPrimitiveType for the
 * non-indexed (drawPrimitives) path. Returns 0xFFFFFFFF for
 * primitives that need M4 index expansion (caller routes those to
 * mtl_translate_expanded_primitive instead). */
static uint32_t mtl_translate_primitive(uint32_t nv097_primitive)
{
    switch (nv097_primitive) {
    case PRIM_TYPE_POINTS:         return MTL_PRIM_POINT;
    case PRIM_TYPE_LINES:          return MTL_PRIM_LINE;
    case PRIM_TYPE_LINE_STRIP:     return MTL_PRIM_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES:      return MTL_PRIM_TRIANGLE;
    case PRIM_TYPE_TRIANGLE_STRIP: return MTL_PRIM_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
    default:
        return 0xFFFFFFFF;
    }
}

/*
 * Categorize an NV2A primitive_mode for the M4 indexed-draw path.
 * Returns:
 *   - MTL_PRIM_TRIANGLE for triangle expansions (fan, polygon)
 *   - MTL_PRIM_LINE     for line expansions (line_loop)
 *   - MTL_PRIM_TRIANGLE for quad expansions (quads, quad_strip)
 *   - 0xFFFFFFFF otherwise
 *
 * Triangle-list and line-list output topologies are the only ones
 * Metal exposes that the index expansions in mtl/index_gen.c emit.
 */
static uint32_t mtl_translate_expanded_primitive(uint32_t nv097_primitive)
{
    switch (nv097_primitive) {
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
        return MTL_PRIM_TRIANGLE;
    case PRIM_TYPE_LINE_LOOP:
        return MTL_PRIM_LINE;
    default:
        return 0xFFFFFFFF;
    }
}

/* Expand a contiguous vertex range into a uint32 index list
 * appropriate for the NV2A primitive. Returns the number of indices
 * written (0 on failure). The caller owns `out` and must size it via
 * the matching pgraph_mtl_idx_capacity_* helper. */
static unsigned int mtl_expand_indices(uint32_t nv097_primitive,
                                       uint32_t *out, size_t out_capacity,
                                       unsigned int vertex_count)
{
    switch (nv097_primitive) {
    case PRIM_TYPE_TRIANGLE_FAN:
        return (unsigned int)pgraph_mtl_idx_expand_triangle_fan(
            out, out_capacity, vertex_count);
    case PRIM_TYPE_QUADS:
        return (unsigned int)pgraph_mtl_idx_expand_quads(
            out, out_capacity, vertex_count);
    case PRIM_TYPE_QUAD_STRIP:
        return (unsigned int)pgraph_mtl_idx_expand_quad_strip(
            out, out_capacity, vertex_count);
    case PRIM_TYPE_POLYGON:
        return (unsigned int)pgraph_mtl_idx_expand_polygon(
            out, out_capacity, vertex_count);
    case PRIM_TYPE_LINE_LOOP:
        return (unsigned int)pgraph_mtl_idx_expand_line_loop(
            out, out_capacity, vertex_count);
    default:
        return 0;
    }
}

static size_t mtl_expanded_index_capacity(uint32_t nv097_primitive,
                                          unsigned int vertex_count)
{
    switch (nv097_primitive) {
    case PRIM_TYPE_TRIANGLE_FAN:
        return pgraph_mtl_idx_capacity_triangle_fan(vertex_count);
    case PRIM_TYPE_QUADS:
        return pgraph_mtl_idx_capacity_quads(vertex_count);
    case PRIM_TYPE_QUAD_STRIP:
        return pgraph_mtl_idx_capacity_quad_strip(vertex_count);
    case PRIM_TYPE_POLYGON:
        return pgraph_mtl_idx_capacity_polygon(vertex_count);
    case PRIM_TYPE_LINE_LOOP:
        return pgraph_mtl_idx_capacity_line_loop(vertex_count);
    default:
        return 0;
    }
}

/*
 * Determine whether the current draw is eligible for the
 * native-tri-depth fragment-shader path. Mirrors
 * gl/draw.c::pgraph_gl_profile_native_tri_depth_draw — same
 * eligibility rules, same env-var gating via
 * pgraph_glsl_native_tri_depth_enabled() — so the per-frame
 * counter splits parallel the GL path exactly.
 */
static bool mtl_native_tri_depth_eligible(PGRAPHState *pg)
{
    enum ShaderPrimitiveMode prim =
        (enum ShaderPrimitiveMode)pg->primitive_mode;
    if (prim != PRIM_TYPE_TRIANGLES &&
        prim != PRIM_TYPE_TRIANGLE_STRIP &&
        prim != PRIM_TYPE_TRIANGLE_FAN) {
        return false;
    }
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    enum ShaderPolygonMode front = (enum ShaderPolygonMode)GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    enum ShaderPolygonMode back = (enum ShaderPolygonMode)GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    return pgraph_glsl_native_tri_depth_enabled() &&
           pgraph_glsl_native_tri_depth_supported(
               prim, front, back, pg->smooth_shading,
               pg->first_vertex_is_provoking);
}

static bool mtl_native_quad_eligible(PGRAPHState *pg)
{
    enum ShaderPrimitiveMode prim =
        (enum ShaderPrimitiveMode)pg->primitive_mode;
    if (prim != PRIM_TYPE_QUADS && prim != PRIM_TYPE_QUAD_STRIP) {
        return false;
    }
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    enum ShaderPolygonMode front = (enum ShaderPolygonMode)GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    enum ShaderPolygonMode back = (enum ShaderPolygonMode)GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    return pgraph_glsl_native_quad_enabled() &&
           pgraph_glsl_native_quad_supported(prim, front, back,
                                             pg->smooth_shading);
}

/* Pull the position / diffuse arrays out of the inline_buffer state.
 * Returns true if `out_positions` and `out_colors` (both non-NULL)
 * point to valid float[N][4] arrays for the call. The caller frees
 * `*out_synth_colors` with g_free() if non-NULL. */
static bool mtl_inline_buffer_attrs(PGRAPHState *pg,
                                    unsigned int vertex_count,
                                    const float **out_positions,
                                    const float **out_colors,
                                    float **out_synth_colors)
{
    *out_synth_colors = NULL;
    VertexAttribute *pos_attr =
        &pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION];
    VertexAttribute *col_attr =
        &pg->vertex_attributes[NV2A_VERTEX_ATTR_DIFFUSE];

    if (pos_attr->inline_buffer == NULL) {
        return false;
    }

    *out_positions = pos_attr->inline_buffer;

    if (col_attr->inline_buffer != NULL) {
        *out_colors = col_attr->inline_buffer;
    } else {
        float *synth = g_malloc_n(vertex_count * 4, sizeof(float));
        for (unsigned int i = 0; i < vertex_count; i++) {
            synth[i * 4 + 0] = col_attr->inline_value[0];
            synth[i * 4 + 1] = col_attr->inline_value[1];
            synth[i * 4 + 2] = col_attr->inline_value[2];
            synth[i * 4 + 3] = col_attr->inline_value[3];
        }
        *out_synth_colors = synth;
        *out_colors = synth;
    }

    return true;
}

static void pgraph_mtl_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    /* Only the inline_buffer path is implemented through M4 (the
     * NV2A immediate-mode submission path the upstream renderers
     * identify by `pg->inline_buffer_length > 0`). The
     * draw_arrays / inline_elements / inline_array paths land
     * with M5+ when the format-resolving / aligned-vertex-buffer
     * remap logic ports from vk/draw.c. */
    if (pg->draw_arrays_length || pg->inline_elements_length ||
        pg->inline_array_length) {
        return;
    }
    if (pg->inline_buffer_length == 0) {
        return;
    }

    /* Ensure surface bindings exist that match the current shape. M2
     * already does this for clear; for draw we must do the same in
     * case the first event in a frame is a draw rather than a clear
     * (uncommon but possible). */
    unsigned int width = 0, height = 0;
    mtl_get_surface_dimensions(pg, &width, &height);
    pgraph_apply_anti_aliasing_factor(pg, &width, &height);
    pgraph_apply_scaling_factor(pg, &width, &height);
    if (pg->surface_shape.color_format) {
        pgraph_mtl_surface_ensure_color(width, height,
                                        pg->surface_shape.color_format);
    }
    if (pg->surface_shape.zeta_format) {
        pgraph_mtl_surface_ensure_depth(width, height,
                                        pg->surface_shape.zeta_format);
    }

    void *color_tex = pgraph_mtl_surface_get_color_texture();
    void *depth_tex = pgraph_mtl_surface_get_depth_texture();
    uint32_t color_fmt = pgraph_mtl_surface_get_color_format();
    uint32_t depth_fmt = pgraph_mtl_surface_get_depth_format();
    uint32_t vp_w      = pgraph_mtl_surface_get_width();
    uint32_t vp_h      = pgraph_mtl_surface_get_height();

    if (color_tex == NULL && depth_tex == NULL) {
        return;
    }
    if (color_tex == NULL) {
        color_fmt = 0;
    }

    unsigned int vcount = pg->inline_buffer_length;

    const float *positions = NULL;
    const float *colors    = NULL;
    float       *synth     = NULL;
    if (!mtl_inline_buffer_attrs(pg, vcount, &positions, &colors, &synth)) {
        return;
    }

    /* Decide which fragment-shader variant to run.
     *
     * The Metal port follows metal-renderer-plan.md §3.8: native_tri_
     * depth and native_quad are the *only* path on Metal (Apple has
     * no geometry shader stage), so the eligibility check determines
     * only which fragment-shader variant we use — there is no GL-
     * style "geometry shader fallback" to drop into. When the draw is
     * NOT eligible for the native path (e.g. flat-non-first-provoking
     * triangles, or non-fill polygon mode), the M4 renderer still
     * draws via the passthrough path — depth will not match PR #2240
     * exactly for those cases. The full GL parity for those edge
     * cases lands when M5 ports the real PSH; in M4 the eligibility
     * gate exists primarily so the counters parallel the GL split.
     *
     * Counter parity rationale: native_tri_depth_eligible and
     * native_quad_eligible run the *same* helpers as the GL path
     * (pgraph_glsl_native_tri_depth_supported /
     * pgraph_glsl_native_quad_supported, gated by
     * pgraph_glsl_native_*_enabled). With XEMU_NATIVE_TRI_DEPTH=1 and
     * XEMU_NATIVE_QUAD=1 default-on (CLAUDE.md rule #11), the
     * candidate counters drive identically across renderers. */
    bool native_tri = mtl_native_tri_depth_eligible(pg);
    bool native_quad = mtl_native_quad_eligible(pg);
    uint32_t variant = (native_tri || native_quad)
                           ? MTL_DRAW_VARIANT_NATIVE_DEPTH
                           : MTL_DRAW_VARIANT_PASSTHROUGH;

    /* M7.1 / M8: state-to-PipelineKey + translated-pipeline lookup +
     * optional encode swap. Always exercises the build path so the
     * cache warms up; the lookup counter parallels gl_shader_compile_count.
     *
     * M8 adds tri-state result: READY / PENDING / FAILED. PENDING means
     * an async build is in flight — when the user has opted into the
     * translated pipeline (XEMU_METAL_TRANSLATED_PIPELINE=1), PENDING
     * causes the draw to be skipped (RPCS3 "skip the draw" pattern;
     * brief visual artifact rather than a frame stall). When the
     * translated pipeline is NOT opted in, PENDING is treated like
     * "ready elsewhere" for cache-warmup purposes and we fall through
     * to the M3/M4 hand-coded passthrough.
     *
     * Encode path selection:
     *   - XEMU_METAL_FORCE_PASSTHROUGH=1 → M3/M4 passthrough (debug).
     *   - XEMU_METAL_TRANSLATED_PIPELINE=1 + lookup READY → translated.
     *   - XEMU_METAL_TRANSLATED_PIPELINE=1 + lookup PENDING → skip draw.
     *   - XEMU_METAL_TRANSLATED_PIPELINE=1 + lookup FAILED → passthrough fallback.
     *   - otherwise → M3/M4 passthrough. */
    void *translated_pipeline = NULL;
    bool  translated_pending  = false;
    if (!mtl_force_passthrough()) {
        PgraphMtlPipelineKey key;
        if (pgraph_mtl_build_pipeline_key(d, color_fmt, depth_fmt,
                                          s_metal_msaa_sample_count,
                                          &key)) {
            atomic_fetch_add(&s_pipeline_key_built, 1);
            PgraphMtlPipelineLookupState st;
            void *ps = pgraph_mtl_shaders_get_pipeline_ex(&key, &st);
            if (st == PGRAPH_MTL_PIPELINE_READY && ps != NULL) {
                atomic_fetch_add(&s_pipeline_translated_ok, 1);
                translated_pipeline = ps;
            } else if (st == PGRAPH_MTL_PIPELINE_PENDING) {
                translated_pending = true;
            } else {
                atomic_fetch_add(&s_pipeline_translated_fb, 1);
            }
        }
    }

    /* If the user opted into the translated pipeline and the cache is
     * still building, skip the draw rather than block on synchronous
     * compile. Visual artifact (briefly missing geometry) instead of a
     * frame stall. M8 cold-launch behavior. */
    if (translated_pending && mtl_use_translated_pipeline() &&
        !mtl_force_passthrough()) {
        atomic_fetch_add(&s_draws_skipped_pending, 1);
        if (synth) {
            g_free(synth);
        }
        return;
    }

    bool use_translated_path =
        translated_pipeline != NULL && mtl_use_translated_pipeline() &&
        !mtl_force_passthrough();

    if (use_translated_path) {
        /* Bind per-stage textures from PGRAPHState. Each stage that
         * fails (disabled / unsupported format) is unbound — the PSH
         * still gets the default sampler so MSL-bound textures don't
         * crash, but no real texture is sampled. */
        for (int t = 0; t < NV2A_MAX_TEXTURES; t++) {
            (void)pgraph_mtl_texture_bind_from_pg(pg, t);
        }

        /* Build the typed VSH/PSH state and pack std140 UBOs. */
        ShaderState ss = pgraph_glsl_get_shader_state(pg);

        pgraph_mtl_uniform_begin_frame();

        void *vsh_ubo = NULL, *psh_ubo = NULL;
        size_t vsh_off = 0, psh_off = 0;
        size_t vsh_size =
            pgraph_mtl_uniform_stage_vsh(pg, &ss.vsh, &vsh_ubo, &vsh_off);
        size_t psh_size =
            pgraph_mtl_uniform_stage_psh(pg, &ss.psh, &psh_ubo, &psh_off);

        /* Collect per-stage texture/sampler pointers. */
        void *stage_tex[4]  = { NULL, NULL, NULL, NULL };
        void *stage_smp[4]  = { NULL, NULL, NULL, NULL };
        void *default_smp = pgraph_mtl_texture_get_default_sampler();
        for (int t = 0; t < NV2A_MAX_TEXTURES; t++) {
            stage_tex[t] = pgraph_mtl_texture_get_metal_texture(t);
            stage_smp[t] = pgraph_mtl_texture_get_sampler_state(t);
            if (stage_smp[t] == NULL) {
                stage_smp[t] = default_smp;
            }
        }

        /* Pick the primitive type. We reuse the same expansion logic
         * as the passthrough path — when the NV2A primitive isn't
         * native Metal, expand to triangles via the index generator. */
        uint32_t mtl_prim = mtl_translate_primitive(pg->primitive_mode);
        bool drew = false;
        if (mtl_prim != 0xFFFFFFFF) {
            pgraph_mtl_draw_translated(translated_pipeline,
                                       positions, colors, vcount,
                                       /*indices=*/NULL, /*icount=*/0,
                                       mtl_prim,
                                       vp_w, vp_h, color_tex, depth_tex,
                                       depth_fmt,
                                       vsh_ubo, vsh_off, vsh_size,
                                       psh_ubo, psh_off, psh_size,
                                       stage_tex, stage_smp);
            drew = true;
        } else {
            uint32_t expanded_prim =
                mtl_translate_expanded_primitive(pg->primitive_mode);
            if (expanded_prim != 0xFFFFFFFF) {
                size_t cap = mtl_expanded_index_capacity(pg->primitive_mode,
                                                          vcount);
                if (cap > 0) {
                    uint32_t *indices = g_malloc_n(cap, sizeof(uint32_t));
                    unsigned int icount =
                        mtl_expand_indices(pg->primitive_mode,
                                           indices, cap, vcount);
                    if (icount > 0) {
                        pgraph_mtl_draw_translated(translated_pipeline,
                                                   positions, colors, vcount,
                                                   indices, icount,
                                                   expanded_prim,
                                                   vp_w, vp_h,
                                                   color_tex, depth_tex,
                                                   depth_fmt,
                                                   vsh_ubo, vsh_off, vsh_size,
                                                   psh_ubo, psh_off, psh_size,
                                                   stage_tex, stage_smp);
                        drew = true;
                    }
                    g_free(indices);
                }
            }
        }

        pgraph_mtl_uniform_end_frame();

        if (drew) {
            if (native_tri) {
                nv2a_profile_inc_counter(NV2A_PROF_NATIVE_TRI_DEPTH_DRAW);
                pgraph_mtl_draw_inc_native_tri_depth_count();
            }
            if (native_quad) {
                nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW);
                pgraph_mtl_draw_inc_native_quad_count();
                if (pg->primitive_mode == PRIM_TYPE_QUADS) {
                    nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW_LIST);
                } else if (pg->primitive_mode == PRIM_TYPE_QUAD_STRIP) {
                    nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW_STRIP);
                }
            }
            if (color_tex) pg->surface_color.draw_dirty = true;
            if (depth_tex) pg->surface_zeta.draw_dirty = true;
        }

        if (synth) g_free(synth);
        return;
    }

    /* Fallback: M3/M4 passthrough. If translation was attempted and
     * failed (translated_pipeline == NULL while user requested it),
     * count the fallback. */
    if (mtl_use_translated_pipeline() && translated_pipeline == NULL &&
        !mtl_force_passthrough()) {
        pgraph_mtl_draw_inc_pipeline_fallback_count();
    }

    /* Try the non-indexed path first for primitives Metal handles
     * natively. */
    uint32_t mtl_prim = mtl_translate_primitive(pg->primitive_mode);
    bool drew = false;

    if (mtl_prim != 0xFFFFFFFF) {
        pgraph_mtl_draw_passthrough(positions, colors, vcount, mtl_prim,
                                    variant, vp_w, vp_h,
                                    color_tex, depth_tex,
                                    color_fmt, depth_fmt);
        drew = true;
    } else {
        /* Index-expanded path. */
        uint32_t expanded_prim =
            mtl_translate_expanded_primitive(pg->primitive_mode);
        if (expanded_prim != 0xFFFFFFFF) {
            size_t cap =
                mtl_expanded_index_capacity(pg->primitive_mode, vcount);
            if (cap > 0) {
                uint32_t *indices = g_malloc_n(cap, sizeof(uint32_t));
                unsigned int icount =
                    mtl_expand_indices(pg->primitive_mode,
                                       indices, cap, vcount);
                if (icount > 0) {
                    pgraph_mtl_draw_indexed(positions, colors, vcount,
                                            indices, icount,
                                            expanded_prim, variant,
                                            vp_w, vp_h,
                                            color_tex, depth_tex,
                                            color_fmt, depth_fmt);
                    drew = true;
                }
                g_free(indices);
            }
        }
    }

    if (drew) {
        /* Drive the parallel GL/Metal counters so
         * extract-perf-summary.sh's existing
         * NATIVE_TRI_DEPTH_DRAW / NATIVE_QUAD_DRAW columns describe
         * the Metal renderer too. METAL_NATIVE_TRI_DEPTH_DRAWS and
         * METAL_NATIVE_QUAD_DRAWS are separate, Metal-only counters
         * exposed via the draw module's atomics. */
        if (native_tri) {
            nv2a_profile_inc_counter(NV2A_PROF_NATIVE_TRI_DEPTH_DRAW);
            pgraph_mtl_draw_inc_native_tri_depth_count();
        }
        if (native_quad) {
            nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW);
            pgraph_mtl_draw_inc_native_quad_count();
            if (pg->primitive_mode == PRIM_TYPE_QUADS) {
                nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW_LIST);
            } else if (pg->primitive_mode == PRIM_TYPE_QUAD_STRIP) {
                nv2a_profile_inc_counter(NV2A_PROF_NATIVE_QUAD_DRAW_STRIP);
            }
        }
    }

    if (synth) {
        g_free(synth);
    }

    /* Mark the color binding dirty so the compositor re-samples it
     * next present. */
    if (color_tex) {
        pg->surface_color.draw_dirty = true;
    }
    if (depth_tex) {
        pg->surface_zeta.draw_dirty = true;
    }

    /* Reset the inline_buffer state for the next batch. The upstream
     * renderers do this via pgraph_reset_inline_buffers(); we don't
     * call that here because pgraph.c may also reset it as part of
     * its own per-batch sequencing. */
}

static void pgraph_mtl_get_report(NV2AState *d, uint32_t parameter)
{
    pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
}

static void pgraph_mtl_image_blit(NV2AState *d)
{
}

static void pgraph_mtl_pre_savevm_trigger(NV2AState *d)
{
}

static void pgraph_mtl_pre_savevm_wait(NV2AState *d)
{
}

static void pgraph_mtl_pre_shutdown_trigger(NV2AState *d)
{
}

static void pgraph_mtl_pre_shutdown_wait(NV2AState *d)
{
}

static void pgraph_mtl_process_pending_reports(NV2AState *d)
{
}

static void pgraph_mtl_surface_update(NV2AState *d, bool upload,
                                      bool color_write, bool zeta_write)
{
    /* M2 does not implement upload/download path. The surface manager's
     * only state-change hook for now is via clear_surface +
     * ensure_color/ensure_depth. M3+ will route surface_update into
     * the same per-VRAM cache the GL/VK renderers use. */
}

static void pgraph_mtl_surface_flush(NV2AState *d)
{
    /* No surface cache to flush yet — bindings persist for the lifetime
     * of the renderer in M2. */
}

static void pgraph_mtl_set_surface_scale_factor(NV2AState *d,
                                                unsigned int scale)
{
    /* Honor the global config knob; the surface manager will reallocate
     * on the next clear/draw because the dimensions will mismatch. */
    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;
    d->pgraph.surface_scale_factor = MAX((int)scale, 1);
}

static unsigned int pgraph_mtl_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor ? d->pgraph.surface_scale_factor : 1;
}

static int pgraph_mtl_get_framebuffer_surface(NV2AState *d)
{
    /* The PGRAPHRenderer.ops.get_framebuffer_surface signature returns
     * `int`. The GL impl returns a GLuint texture handle (which fits in
     * an int). Metal's id<MTLTexture> is a 64-bit pointer, so we can't
     * round-trip it through this signature.
     *
     * Resolution: this op returns 1 if a framebuffer surface exists,
     * else 0 — a truthy presence signal. The actual MTLTexture pointer
     * is published via the side-channel
     * pgraph_mtl_get_framebuffer_metal_texture(), read by the
     * compositor in ui/xemu-metal.mm.
     *
     * Today the only consumer of this op's return value is
     * gl_render_frame() in ui/xemu.c, and the Metal path skips that
     * function entirely (xemu_metal_is_active() guard at xemu.c:840).
     * So returning 1/0 here is safe.
     *
     * See decision-log "2026-05-02: Metal slice M2 — clear-only surface
     * manager + side-channel framebuffer texture accessor". */
    (void)d;
    return pgraph_mtl_surface_has_front_framebuffer();
}

static GPUProperties *pgraph_mtl_get_gpu_properties(void)
{
    static GPUProperties props;
    return &props;
}

static void pgraph_mtl_init(NV2AState *d, Error **errp)
{
    /* Slice M2: bring up the heap manager and surface manager. They
     * both rely on xemu_metal_init() having already created the
     * MTLDevice (it has — main thread runs xemu_metal_init at SDL
     * window-creation time, well before the NV2A renderer is selected
     * via display.renderer = METAL).
     *
     * If init fails the renderer enters a degraded state (clears
     * become silent no-ops); we don't propagate the error out as a
     * fatal because the caller has no fallback once the SDL window
     * was created with SDL_WINDOW_METAL. The fprintf inside the heap
     * init is the user-visible signal. */
    if (!pgraph_mtl_heap_init()) {
        fprintf(stderr, "pgraph_mtl_init: heap init failed; clear "
                        "operations will be no-ops\n");
        return;
    }
    if (!pgraph_mtl_surface_init()) {
        fprintf(stderr, "pgraph_mtl_init: surface init failed; clear "
                        "operations will be no-ops\n");
        pgraph_mtl_heap_finalize();
        return;
    }

    /* M3 brings up the staging-buffer ring, the pipeline cache, and
     * the draw command queue. Failures here are not fatal — clears
     * still work, just no draws will land. */
    if (!pgraph_mtl_buffer_init()) {
        fprintf(stderr, "pgraph_mtl_init: buffer ring init failed; "
                        "draws will be no-ops\n");
    }
    if (!pgraph_mtl_pipeline_init()) {
        fprintf(stderr, "pgraph_mtl_init: pipeline cache init failed; "
                        "draws will be no-ops\n");
    }
    if (!pgraph_mtl_draw_init()) {
        fprintf(stderr, "pgraph_mtl_init: draw init failed; "
                        "draws will be no-ops\n");
    }

    /* M5: bring up the GLSL → SPIR-V → MSL translator. Failure here
     * is non-fatal — without the translator, shader-state-driven
     * draws will fall back to the M3/M4 hand-coded passthrough
     * pipeline. */
    if (!pgraph_mtl_glsl_init()) {
        fprintf(stderr, "pgraph_mtl_init: GLSL translator init failed; "
                        "shader-state-driven draws will use the "
                        "M3/M4 hand-coded passthrough\n");
    }

    /* M6 (cache + draw-path swap): per-PipelineKey LRU cache. */
    if (!pgraph_mtl_shaders_init()) {
        fprintf(stderr, "pgraph_mtl_init: shader pipeline cache init "
                        "failed; draws will fall back to the M3/M4 "
                        "hand-coded passthrough\n");
    }

    /* M6 (textures + sampling): texture cache + sampler cache. */
    if (!pgraph_mtl_texture_init()) {
        fprintf(stderr, "pgraph_mtl_init: texture cache init failed; "
                        "textured draws will be untextured\n");
    }

    /* M7.1: uniform staging ring for translated UBOs. */
    if (!pgraph_mtl_uniform_init()) {
        fprintf(stderr, "pgraph_mtl_init: uniform ring init failed; "
                        "translated draws will fall back to "
                        "M3/M4 passthrough\n");
    }

    /* M5: optional shader-validation harness. Driven by
     * XEMU_METAL_SHADER_VALIDATE; advisory by default, aborts on
     * failure when set to "strict" or "2". The harness runs the
     * representative ShaderState fixtures end-to-end through GLSL
     * → SPIR-V → MSL → MTLLibrary. See shader_validation.h. */
    (void)pgraph_mtl_shader_validate_run();

    /* Reload the config-driven surface scale factor so the first
     * ensure_color/ensure_depth picks up the user-visible
     * display.quality.surface_scale value (1 default; 2 on Apple
     * Silicon first-run). */
    int factor = g_config.display.quality.surface_scale;
    d->pgraph.surface_scale_factor = MAX(factor, 1);

    /* M11: parse XEMU_METAL_MSAA, clamp to the device's supported
     * sample counts, and publish to the surface manager so it
     * allocates the multisample companion textures alongside the
     * single-sample bindings. The companion is created lazily on
     * the first ensure_color/ensure_depth that produces a real
     * binding shape. */
    uint32_t requested = 0;
    uint32_t configured = parse_metal_msaa_env(&requested);
    uint32_t effective = configured;
    while (effective > 1 &&
           !pgraph_mtl_heap_supports_sample_count(effective)) {
        /* Step down to the next supported lower count.
         * MTLDevice supportsTextureSampleCount: typically returns
         * true for {1, 2, 4, 8}; this loop is defensive against
         * future device variants. */
        if (effective == 8) {
            effective = 4;
        } else if (effective == 4) {
            effective = 2;
        } else {
            effective = 1;
        }
    }
    s_metal_msaa_sample_count = effective;
    pgraph_mtl_surface_set_msaa_sample_count(effective);
    fprintf(stderr,
            "xemu-perf: metal_msaa=%u source=XEMU_METAL_MSAA "
            "requested=%u configured=%u\n",
            effective, requested, configured);
}

/* M11: accessor for the latched effective MSAA sample count. Surface
 * manager + state.c + draw.mm consult this to keep render-pass
 * storeAction, pipeline rasterSampleCount, and PipelineKey
 * sample_count consistent. */
uint32_t pgraph_mtl_renderer_msaa_sample_count(void)
{
    return s_metal_msaa_sample_count;
}

static void pgraph_mtl_finalize(NV2AState *d)
{
    /* Tear down in reverse init order. */
    pgraph_mtl_uniform_finalize();
    pgraph_mtl_texture_finalize();
    pgraph_mtl_shaders_finalize();
    pgraph_mtl_glsl_finalize();
    pgraph_mtl_draw_finalize();
    pgraph_mtl_pipeline_finalize();
    pgraph_mtl_buffer_finalize();
    pgraph_mtl_surface_finalize();
    pgraph_mtl_heap_finalize();
}

static PGRAPHRenderer pgraph_mtl_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_METAL,
    .name = "Metal",
    .ops = {
        .init = pgraph_mtl_init,
        .finalize = pgraph_mtl_finalize,
        .clear_report_value = pgraph_mtl_clear_report_value,
        .clear_surface = pgraph_mtl_clear_surface,
        .draw_begin = pgraph_mtl_draw_begin,
        .draw_end = pgraph_mtl_draw_end,
        .flip_stall = pgraph_mtl_flip_stall,
        .flush_draw = pgraph_mtl_flush_draw,
        .get_report = pgraph_mtl_get_report,
        .image_blit = pgraph_mtl_image_blit,
        .pre_savevm_trigger = pgraph_mtl_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_mtl_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_mtl_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_mtl_pre_shutdown_wait,
        .process_pending = pgraph_mtl_process_pending,
        .process_pending_reports = pgraph_mtl_process_pending_reports,
        .surface_update = pgraph_mtl_surface_update,
        .surface_flush = pgraph_mtl_surface_flush,
        .set_surface_scale_factor = pgraph_mtl_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_mtl_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_mtl_get_framebuffer_surface,
        .get_gpu_properties = pgraph_mtl_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_mtl_renderer);
}

/* M7 counters — surface via extract-perf-summary.sh. */
uint64_t pgraph_mtl_pipeline_key_built_count(void)
{
    return atomic_load(&s_pipeline_key_built);
}
uint64_t pgraph_mtl_pipeline_translated_ok_count(void)
{
    return atomic_load(&s_pipeline_translated_ok);
}
uint64_t pgraph_mtl_pipeline_translated_failed_count(void)
{
    return atomic_load(&s_pipeline_translated_fb);
}

/* M8 counter — draws skipped because the translated pipeline was still
 * building. */
uint64_t pgraph_mtl_draws_skipped_pending_count(void)
{
    return atomic_load(&s_draws_skipped_pending);
}
