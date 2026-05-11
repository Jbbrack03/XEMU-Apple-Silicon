/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "qemu/timer.h"
#include "debug.h"
#include "renderer.h"

GloContext *g_nv2a_context_render;
GloContext *g_nv2a_context_display;
GloContext *g_nv2a_context_shader_compile;

static bool pgraph_gl_async_shader_compile_env_enabled(void)
{
    const char *value = getenv("XEMU_PGRAPH_ASYNC_SHADER_COMPILE");
    return value && value[0] && strcmp(value, "0") != 0;
}

static void early_context_init(void)
{
    g_nv2a_context_render = glo_context_create();
    g_nv2a_context_display = glo_context_create();

    /* Optional third shared context for the async shader compile worker.
     * Only created when XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1 so default builds
     * are unaffected. SDL_GL_SHARE_WITH_CURRENT_CONTEXT is set by
     * glo_context_create(), so the worker context shares program/shader
     * name space with the render and display contexts. */
    if (pgraph_gl_async_shader_compile_env_enabled()) {
        g_nv2a_context_shader_compile = glo_context_create();
        fprintf(stderr,
                "xemu-perf: async_shader_compile=1 "
                "source=XEMU_PGRAPH_ASYNC_SHADER_COMPILE\n");
    }

    // Note: Due to use of shared contexts, this must happen after some other
    // context is created so the temporary context will not become the thread
    // context. After destroying the context, some a durable context should be
    // selected.
    GloContext *context = glo_context_create();
    pgraph_gl_determine_gpu_properties();
    glo_context_destroy(context);
    glo_set_current(g_nv2a_context_display);
}

static void pgraph_gl_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->gl_renderer_state = g_malloc0(sizeof(*pg->gl_renderer_state));
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* fire up opengl */
    glo_set_current(g_nv2a_context_render);

#if DEBUG_NV2A_GL
    gl_debug_initialize();
#endif

    /* DXT textures */
    assert(glo_check_extension("GL_EXT_texture_compression_s3tc"));
    /*  Internal RGB565 texture format */
    assert(glo_check_extension("GL_ARB_ES2_compatibility"));

    glGetFloatv(GL_SMOOTH_LINE_WIDTH_RANGE, r->supported_smooth_line_width_range);
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, r->supported_aliased_line_width_range);

    pgraph_gl_init_surfaces(pg);
    pgraph_gl_init_reports(d);
    pgraph_gl_init_textures(d);
    pgraph_gl_init_buffers(d);
    pgraph_gl_init_shaders(pg);
    pgraph_gl_init_display(d);

    pgraph_gl_update_entire_memory_buffer(d);

    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->supported_extensions.texture_filter_anisotropic =
        glo_check_extension("GL_EXT_texture_filter_anisotropic");

    /* W4 (2026-05-04): parse XEMU_GL_DUMP_DRAW_RT once. Off by default;
     * zero hot-path cost when unset. */
    pgraph_gl_draw_dump_rt_init();
}

static void pgraph_gl_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    glo_set_current(g_nv2a_context_render);

    pgraph_gl_finalize_surfaces(pg);
    pgraph_gl_finalize_shaders(pg);
    pgraph_gl_finalize_textures(pg);
    pgraph_gl_finalize_reports(pg);
    pgraph_gl_finalize_buffers(pg);
    pgraph_gl_finalize_display(pg);

    glo_set_current(NULL);

    g_free(pg->gl_renderer_state);
    pg->gl_renderer_state = NULL;
}

static void pgraph_gl_flip_stall(NV2AState *d)
{
    /* Time the per-frame flip barrier. glFinish drains all queued GL work
     * on the renderer thread; if Apple's GL command queue has accumulated
     * pending submissions (or if the Metal layer needs to do present-side
     * pipeline work), the cost shows up here, not in any per-draw counter.
     * The bad-interval breakdown for Crimson 2026-05-01 showed near-zero
     * accumulated draw counters during 1.35-second worst-frame intervals,
     * which is consistent with the cost being concentrated here at flip
     * (or upstream in the TCG/pfifo path). */
    int64_t flip_stall_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    NV2A_GL_DFRAME_TERMINATOR();
    int64_t glfinish_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    glFinish();
    int64_t glfinish_end_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    nv2a_profile_add_counter(NV2A_PROF_FLIP_STALL_GLFINISH_US_TOTAL,
                             (int)(glfinish_end_us - glfinish_start_us));
    nv2a_profile_spike("flip_stall_glfinish",
                       glfinish_end_us - glfinish_start_us);
    nv2a_profile_add_counter(NV2A_PROF_FLIP_STALL_US_TOTAL,
                             (int)(glfinish_end_us - flip_stall_start_us));
    pgraph_gl_capture_display_if_requested(d);
}

static void pgraph_gl_flush(NV2AState *d)
{
    pgraph_gl_surface_flush(d);
    pgraph_gl_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_gl_update_entire_memory_buffer(d);
    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_gl_process_pending(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending) ||
        qatomic_read(&r->shader_cache_writeback_pending)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_gl_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_gl_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_gl_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_gl_flush(d);
        }
        if (qatomic_read(&r->shader_cache_writeback_pending)) {
            pgraph_gl_shader_write_cache_reload_list(&d->pgraph);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_gl_pre_savevm_trigger(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qatomic_set(&r->download_dirty_surfaces_pending, true);
    qemu_event_reset(&r->dirty_surfaces_download_complete);
}

static void pgraph_gl_pre_savevm_wait(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_event_wait(&r->dirty_surfaces_download_complete);
}

static void pgraph_gl_pre_shutdown_trigger(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qatomic_set(&r->shader_cache_writeback_pending, true);
    qemu_event_reset(&r->shader_cache_writeback_complete);
}

static void pgraph_gl_pre_shutdown_wait(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_event_wait(&r->shader_cache_writeback_complete);
}

static PGRAPHRenderer pgraph_gl_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_OPENGL,
    .name = "OpenGL",
    .ops = {
        .init = pgraph_gl_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_gl_finalize,
        .clear_report_value = pgraph_gl_clear_report_value,
        .clear_surface = pgraph_gl_clear_surface,
        .draw_begin = pgraph_gl_draw_begin,
        .draw_end = pgraph_gl_draw_end,
        .flip_stall = pgraph_gl_flip_stall,
        .flush_draw = pgraph_gl_flush_draw,
        .get_report = pgraph_gl_get_report,
        .image_blit = pgraph_gl_image_blit,
        .pre_savevm_trigger = pgraph_gl_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_gl_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_gl_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_gl_pre_shutdown_wait,
        .process_pending = pgraph_gl_process_pending,
        .process_pending_reports = pgraph_gl_process_pending_reports,
        .surface_update = pgraph_gl_surface_update,
        .set_surface_scale_factor = pgraph_gl_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_gl_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_gl_get_framebuffer_surface,
        .get_gpu_properties = pgraph_gl_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_gl_renderer);
}
