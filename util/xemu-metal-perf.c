/*
 * Apple Silicon performance fork: Metal renderer perf counters.
 *
 * See include/qemu/xemu-metal-perf.h for the API contract.
 *
 * The actual atomic counters live inside the Metal renderer
 * (mtl/draw.mm / mtl/surface.mm) so they can be incremented from the
 * .mm files without crossing the per-target header boundary. This file
 * provides the emit-and-reset hook called from
 * nv2a_profile_log_emit_interval.
 *
 * Linkage: weak symbols. When the Metal renderer is not compiled in
 * (i.e. on non-Apple-Silicon builds where mtl/meson.build is gated
 * out), the weak default counter accessors return zero and the
 * emit function is a no-op.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/xemu-metal-perf.h"

/* Snapshot baselines so the per-interval delta can be reported even
 * though the underlying counters in mtl/*.{mm,c} are monotonic. The
 * baselines are not atomic — the emit hook is single-threaded
 * (called from the renderer thread inside profile.c). */
static uint64_t s_baseline_draw;
static uint64_t s_baseline_draw_indexed;
static uint64_t s_baseline_native_tri_depth;
static uint64_t s_baseline_native_quad;
static uint64_t s_baseline_clear;
static uint64_t s_baseline_glsl_translate;
static uint64_t s_baseline_glsl_translate_failures;
static uint64_t s_baseline_validate_ok;
static uint64_t s_baseline_validate_fail;
/* M6 — pipeline cache + texture upload counters. */
static uint64_t s_baseline_pipeline_hits;
static uint64_t s_baseline_pipeline_misses;
static uint64_t s_baseline_pipeline_failed;
static uint64_t s_baseline_tex_uploads;
static uint64_t s_baseline_tex_upload_bytes;
static uint64_t s_baseline_tex_cache_hits;
static uint64_t s_baseline_tex_cache_misses;
/* M7 — state-to-PipelineKey + draw-path lookup counters. */
static uint64_t s_baseline_pipeline_key_built;
static uint64_t s_baseline_pipeline_translated_ok;
static uint64_t s_baseline_pipeline_translated_failed;
/* 2026-05-04 — CPU wall-time counters for Metal hot paths. */
static uint64_t s_baseline_dispatch_us_total;
static uint64_t s_baseline_texture_bind_us_total;
static uint64_t s_baseline_draw_encode_us_total;
static uint64_t s_baseline_draw_pass_opens;
static uint64_t s_baseline_draw_pass_coalesced;
static uint64_t s_baseline_draw_pass_flushes;
static uint64_t s_baseline_open_pass_flush_us_total;
static uint64_t s_baseline_tex_upload_us_total;
static uint64_t s_baseline_surface_download_us_total;
/* M7.1 — translated-pipeline encode + uniform staging + fallback. */
static uint64_t s_baseline_draw_translated;
static uint64_t s_baseline_pipeline_fallbacks;
static uint64_t s_baseline_uniform_pack;
static uint64_t s_baseline_uniform_bytes;
/* M8 — async pipeline compile + skip-the-draw counters. */
static uint64_t s_baseline_compile_queued;
static uint64_t s_baseline_compile_completed;
static uint64_t s_baseline_compile_async_failed;
static uint64_t s_baseline_draws_skipped_pending;
static uint64_t s_baseline_draws_using_ubershader;
/* M9 — persistent MSL-source disk cache. */
static uint64_t s_baseline_shader_cache_loads;
static uint64_t s_baseline_shader_cache_hits;
static uint64_t s_baseline_shader_cache_misses;
/* M10 — frame pacing. */
static uint64_t s_baseline_presents_total;
static uint64_t s_baseline_present_jitter_us_total;
static uint64_t s_baseline_drawable_acquire_fails;
static uint64_t s_baseline_display_link_callbacks;
/* M11 — MSAA + resolve. */
static uint64_t s_baseline_msaa_resolve_count;
static uint64_t s_baseline_msaa_resolve_us_total;
/* M12 — MetalFX spatial scaler. */
static uint64_t s_baseline_fx_spatial_us_total;
static uint64_t s_baseline_fx_spatial_presents;
/* M13 — frame capture + per-stage GPU-time counter sampling. */
static uint64_t s_baseline_fx_spatial_gpu_us_total;
static uint64_t s_baseline_vertex_us_total;
static uint64_t s_baseline_fragment_us_total;
static uint64_t s_baseline_present_gpu_us_total;
static uint64_t s_baseline_present_gpu_frames;
static uint64_t s_baseline_capture_frames_seen;
/* 2026-05-03 — programmatic PNG screenshot of the final composited
 * drawable. Bumped from inside the cmdbuf addCompletedHandler in
 * ui/xemu-metal.mm once the PNG has been written successfully. */
static uint64_t s_baseline_screenshots_taken;
/* M5.9 (2026-05-03) — per-VRAM surface cache + CRTC-aware publish. */
static uint64_t s_baseline_front_fb_publishes;
/* M5.9-followup-A (2026-05-03) — NV097_IMAGE_BLIT GPU-side copies. */
static uint64_t s_baseline_image_blits;
/* M5.9-followup-B+C (2026-05-03) — CPU-write dirty tracking + VRAM upload. */
static uint64_t s_baseline_vram_dirty_hits;
static uint64_t s_baseline_vram_uploads;
static uint64_t s_baseline_vram_upload_bytes;
/* 2026-05-03 magenta-RT diagnostic — cache shape-mismatch recreate. */
static uint64_t s_baseline_recreate_shape_mismatch;
/* M5.10 (2026-05-03) — VRAM-coherent surface download counters. */
static uint64_t s_baseline_surface_downloads;
static uint64_t s_baseline_surface_download_bytes;
/* W4 (2026-05-04) — per-draw color RT dump (XEMU_METAL_DUMP_DRAW_RT). */
static uint64_t s_baseline_draw_rt_dumps;
/* Tool 1 (2026-05-19) — structured surface-graph dump
 * (XEMU_METAL_SURFACE_GRAPH_DUMP). */
static uint64_t s_baseline_surface_graph_dumps;
/* 2026-05-20 — cross-sibling sync at color rebind
 * (XEMU_METAL_RTT_SIBLING_SYNC). */
static uint64_t s_baseline_sibling_syncs;
static uint64_t s_baseline_sibling_sync_skips;
/* Task #13 — CPU-side flat-color propagation for OP_QUADS / OP_QUAD_STRIP
 * (Apple Silicon Metal has no geometry-shader stage). */
static uint64_t s_baseline_flat_quad_propagations;

/* Weak monotonic counter accessors. Defined for-real in the Metal
 * renderer; default to zero when Metal is not compiled in (e.g. on
 * non-Apple-Silicon hosts). */
__attribute__((weak)) uint64_t pgraph_mtl_draw_count(void) { return 0; }
__attribute__((weak)) uint64_t pgraph_mtl_draw_indexed_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_draw_native_tri_depth_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_draw_native_quad_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_surface_clear_count(void)
{
    return 0;
}

/* M5 — GLSL → SPIR-V → MSL translator + shader-validation harness. */
__attribute__((weak)) uint64_t pgraph_mtl_glsl_translate_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_glsl_translate_failures(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_shader_validate_ok_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_shader_validate_fail_count(void)
{
    return 0;
}

/* M6 — pipeline cache. */
__attribute__((weak)) uint64_t pgraph_mtl_shaders_pipeline_hits(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_shaders_pipeline_misses(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_shaders_pipeline_failed(void)
{
    return 0;
}

/* M6 — texture upload + sampling. */
__attribute__((weak)) uint64_t pgraph_mtl_texture_uploads_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_texture_upload_bytes(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_texture_cache_hits(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_texture_cache_misses(void)
{
    return 0;
}

/* M7 — state-to-PipelineKey + draw-path lookup. */
__attribute__((weak)) uint64_t pgraph_mtl_pipeline_key_built_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_pipeline_translated_ok_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_pipeline_translated_failed_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_dispatch_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_texture_bind_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_draw_encode_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_draw_open_pass_flush_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_texture_upload_us_total(void)
{
    return 0;
}

/* M7.1 — translated-pipeline encode + uniform staging + fallback. */
__attribute__((weak)) uint64_t pgraph_mtl_draw_translated_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_draw_pipeline_fallback_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_uniform_pack_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_uniform_bytes_total(void)
{
    return 0;
}

/* M8 — async pipeline compile + skip-the-draw. */
__attribute__((weak)) uint64_t pgraph_mtl_shaders_compile_queued(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_shaders_compile_completed(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_shaders_compile_async_failed(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_draws_skipped_pending_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_flat_quad_propagations_count(void)
{
    return 0;
}

/* M5.5+ render-pass coalescing telemetry. Strong symbols in
 * mtl/draw.mm; weak fallback returns 0 so non-Metal builds compile. */
__attribute__((weak)) uint64_t pgraph_mtl_draw_pass_opens_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_draw_pass_coalesced_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_draw_pass_flushes_count(void)
{
    return 0;
}

/* Path A (full hybrid ubershader) is deferred for M8 — see
 * docs/apple-silicon/metal-renderer-plan.md M8 entry. The accessor is
 * reserved so the per-interval counter slot is allocated and surfaces
 * as zero today; when a future M8.1 lands the ubershader, this
 * accessor's strong symbol returns the per-interval count and
 * extract-perf-summary.sh + automation.md require no further
 * changes. */
__attribute__((weak)) uint64_t pgraph_mtl_draws_using_ubershader_count(void)
{
    return 0;
}

/* M9 — persistent MSL-source disk cache. */
__attribute__((weak)) uint64_t pgraph_mtl_disk_cache_loads(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_disk_cache_hits(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_disk_cache_misses(void)
{
    return 0;
}

/* M10 — frame pacing. Strong symbols live in ui/xemu-metal.mm; the
 * weak defaults below let the host binary link cleanly when the .mm
 * file is excluded (e.g. on Intel Macs / non-darwin builds). */
__attribute__((weak)) uint64_t pgraph_mtl_present_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_present_jitter_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_present_jitter_us_max_value(void)
{
    return 0;
}
__attribute__((weak)) void pgraph_mtl_present_jitter_us_max_reset(void)
{
}
__attribute__((weak)) uint64_t pgraph_mtl_drawable_acquire_fails(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_display_link_callbacks(void)
{
    return 0;
}

/* M11 — MSAA + resolve. Strong symbols defined in mtl/surface.mm and
 * mtl/renderer.c. The weak defaults below let the host binary link
 * cleanly when the .mm files are excluded (non-Apple-Silicon builds). */
__attribute__((weak)) uint64_t pgraph_mtl_surface_msaa_resolve_count(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_surface_msaa_resolve_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint32_t pgraph_mtl_renderer_msaa_sample_count(void)
{
    return 1;
}

/* M12 — MetalFX spatial scaler. Strong symbols defined in
 * ui/xemu-metal.mm; the weak defaults below let the host binary link
 * cleanly when the .mm file is excluded (non-Apple-Silicon builds) or
 * when the Metal renderer is not the active backend. */
__attribute__((weak)) uint64_t pgraph_mtl_fx_spatial_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_fx_spatial_presents(void)
{
    return 0;
}
__attribute__((weak)) uint32_t pgraph_mtl_fx_scale_factor(void)
{
    return 1;
}

/* M13 — frame capture + per-stage GPU-time counter sampling. Strong
 * symbols defined in ui/xemu-metal.mm; the weak defaults below let
 * the host binary link cleanly when the .mm file is excluded
 * (non-Apple-Silicon builds) or when the Metal renderer is not the
 * active backend. */
__attribute__((weak)) uint64_t pgraph_mtl_fx_spatial_gpu_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_vertex_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_fragment_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_present_gpu_us_total(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_present_gpu_frames(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_capture_frames_seen(void)
{
    return 0;
}
__attribute__((weak)) uint32_t pgraph_mtl_capture_active(void)
{
    return 0;
}

/* 2026-05-03 — programmatic PNG screenshot of the final composited
 * drawable. Strong symbol defined in ui/xemu-metal.mm; the weak
 * default below lets non-Apple-Silicon builds link cleanly. */
__attribute__((weak)) uint64_t pgraph_mtl_screenshots_taken(void)
{
    return 0;
}

/* M5.9 (2026-05-03) — per-VRAM surface cache + CRTC-aware publish.
 * Strong symbols in mtl/surface.mm; weak fallbacks below for the
 * non-Apple-Silicon link. */
__attribute__((weak)) uint64_t pgraph_mtl_surface_front_fb_publishes(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_surface_cache_entries(void)
{
    return 0;
}

/* M5.9-followup-A (2026-05-03) — NV097_IMAGE_BLIT GPU-side copies.
 * Strong symbol in mtl/surface.mm; weak fallback for the
 * non-Apple-Silicon link. */
__attribute__((weak)) uint64_t pgraph_mtl_surface_image_blits(void)
{
    return 0;
}

/* M5.9-followup-B+C (2026-05-03) — CPU-write dirty tracking + VRAM
 * upload. Strong symbols in mtl/surface.mm; weak fallbacks for the
 * non-Apple-Silicon link. */
__attribute__((weak)) uint64_t pgraph_mtl_surface_vram_dirty_hits(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_surface_vram_uploads(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_surface_vram_upload_bytes(void)
{
    return 0;
}

/* M5.10 (2026-05-03) — VRAM-coherent surface download. Strong symbols
 * in mtl/surface.mm; weak fallbacks for the non-Apple-Silicon link. */
__attribute__((weak)) uint64_t pgraph_mtl_surface_downloads(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_surface_download_bytes(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_surface_download_us_total(void)
{
    return 0;
}

/* 2026-05-03 magenta-RT diagnostic — cache shape-mismatch recreate. */
__attribute__((weak)) uint64_t
pgraph_mtl_surface_recreate_shape_mismatch(void)
{
    return 0;
}

/* 2026-05-03 magenta-RT diagnostic — per-vram_addr draw-target table.
 * Strong symbol in mtl/renderer.c; weak fallback (no-op) for the
 * non-Apple-Silicon link. The emit prints zero-or-more
 * `xemu-perf: metal_draw_target vram_addr=0x.. count=N` lines per
 * interval. */
__attribute__((weak)) void pgraph_mtl_draw_target_emit_interval(FILE *out)
{
    (void)out;
}

/* W4 (2026-05-04) — per-draw color RT dump counter. Strong symbol in
 * mtl/draw.mm; weak fallback (returns zero) for non-Apple-Silicon
 * builds. */
__attribute__((weak)) uint64_t pgraph_mtl_draw_rt_dumps_count(void)
{
    return 0;
}

/* Tool 1 (2026-05-19) — surface-graph dump counter. Strong symbol in
 * mtl/surface.mm; weak fallback (returns zero) for non-Apple-Silicon
 * builds. */
__attribute__((weak)) uint64_t pgraph_mtl_surface_graph_dumps(void)
{
    return 0;
}

/* 2026-05-20 — cross-sibling sync counters. Strong symbols in
 * mtl/surface.mm; weak fallbacks (return zero) for non-Apple-Silicon
 * builds. */
__attribute__((weak)) uint64_t pgraph_mtl_surface_sibling_syncs(void)
{
    return 0;
}
__attribute__((weak)) uint64_t pgraph_mtl_surface_sibling_sync_skips(void)
{
    return 0;
}

void xemu_metal_perf_emit_and_reset(FILE *out)
{
    if (out == NULL) {
        return;
    }

    uint64_t draw_total       = pgraph_mtl_draw_count();
    uint64_t draw_indexed     = pgraph_mtl_draw_indexed_count();
    uint64_t native_tri_depth = pgraph_mtl_draw_native_tri_depth_count();
    uint64_t native_quad      = pgraph_mtl_draw_native_quad_count();
    uint64_t clear_total      = pgraph_mtl_surface_clear_count();
    uint64_t glsl_xlate       = pgraph_mtl_glsl_translate_count();
    uint64_t glsl_xlate_fail  = pgraph_mtl_glsl_translate_failures();
    uint64_t validate_ok      = pgraph_mtl_shader_validate_ok_count();
    uint64_t validate_fail    = pgraph_mtl_shader_validate_fail_count();
    uint64_t pipeline_hits    = pgraph_mtl_shaders_pipeline_hits();
    uint64_t pipeline_miss    = pgraph_mtl_shaders_pipeline_misses();
    uint64_t pipeline_fail    = pgraph_mtl_shaders_pipeline_failed();
    uint64_t tex_uploads      = pgraph_mtl_texture_uploads_count();
    uint64_t tex_upload_bytes = pgraph_mtl_texture_upload_bytes();
    uint64_t tex_cache_hits   = pgraph_mtl_texture_cache_hits();
    uint64_t tex_cache_miss   = pgraph_mtl_texture_cache_misses();
    uint64_t pkey_built       = pgraph_mtl_pipeline_key_built_count();
    uint64_t pkey_xlate_ok    = pgraph_mtl_pipeline_translated_ok_count();
    uint64_t pkey_xlate_fail  = pgraph_mtl_pipeline_translated_failed_count();
    uint64_t dispatch_us      = pgraph_mtl_dispatch_us_total();
    uint64_t tex_bind_us      = pgraph_mtl_texture_bind_us_total();
    uint64_t draw_encode_us   = pgraph_mtl_draw_encode_us_total();
    uint64_t pass_opens       = pgraph_mtl_draw_pass_opens_count();
    uint64_t pass_coalesced   = pgraph_mtl_draw_pass_coalesced_count();
    uint64_t pass_flushes     = pgraph_mtl_draw_pass_flushes_count();
    uint64_t pass_flush_us    = pgraph_mtl_draw_open_pass_flush_us_total();
    uint64_t tex_upload_us    = pgraph_mtl_texture_upload_us_total();
    uint64_t draw_translated  = pgraph_mtl_draw_translated_count();
    uint64_t pipe_fallback    = pgraph_mtl_draw_pipeline_fallback_count();
    uint64_t ubo_pack         = pgraph_mtl_uniform_pack_count();
    uint64_t ubo_bytes        = pgraph_mtl_uniform_bytes_total();
    uint64_t cc_queued        = pgraph_mtl_shaders_compile_queued();
    uint64_t cc_completed     = pgraph_mtl_shaders_compile_completed();
    uint64_t cc_async_failed  = pgraph_mtl_shaders_compile_async_failed();
    uint64_t draws_skipped    = pgraph_mtl_draws_skipped_pending_count();
    uint64_t flat_quad_propag = pgraph_mtl_flat_quad_propagations_count();
    uint64_t draws_uber       = pgraph_mtl_draws_using_ubershader_count();
    uint64_t sc_loads         = pgraph_mtl_disk_cache_loads();
    uint64_t sc_hits          = pgraph_mtl_disk_cache_hits();
    uint64_t sc_misses        = pgraph_mtl_disk_cache_misses();
    /* M10 — frame pacing. */
    uint64_t presents_total   = pgraph_mtl_present_total();
    uint64_t jitter_us_total  = pgraph_mtl_present_jitter_us_total();
    uint64_t jitter_us_max    = pgraph_mtl_present_jitter_us_max_value();
    uint64_t draw_acq_fails   = pgraph_mtl_drawable_acquire_fails();
    uint64_t dl_callbacks     = pgraph_mtl_display_link_callbacks();
    /* M11 — MSAA + resolve. */
    uint64_t msaa_resolves    = pgraph_mtl_surface_msaa_resolve_count();
    uint64_t msaa_resolve_us  = pgraph_mtl_surface_msaa_resolve_us_total();
    uint32_t msaa_samples     = pgraph_mtl_renderer_msaa_sample_count();
    /* M12 — MetalFX spatial scaler. */
    uint64_t fx_spatial_us    = pgraph_mtl_fx_spatial_us_total();
    uint64_t fx_spatial_pres  = pgraph_mtl_fx_spatial_presents();
    uint32_t fx_scale_factor  = pgraph_mtl_fx_scale_factor();
    /* M13 — frame capture + per-stage GPU timing. */
    uint64_t fx_gpu_us        = pgraph_mtl_fx_spatial_gpu_us_total();
    uint64_t vertex_us        = pgraph_mtl_vertex_us_total();
    uint64_t fragment_us      = pgraph_mtl_fragment_us_total();
    uint64_t present_gpu_us   = pgraph_mtl_present_gpu_us_total();
    uint64_t present_gpu_fr   = pgraph_mtl_present_gpu_frames();
    uint64_t capture_seen     = pgraph_mtl_capture_frames_seen();
    uint32_t capture_active   = pgraph_mtl_capture_active();
    /* 2026-05-03 — PNG screenshot counter. */
    uint64_t screenshots_taken = pgraph_mtl_screenshots_taken();
    /* M5.9 — front-fb publish counter + live cache size. */
    uint64_t front_fb_publishes = pgraph_mtl_surface_front_fb_publishes();
    uint64_t surface_cache_size = pgraph_mtl_surface_cache_entries();
    /* M5.9-followup-A — NV097_IMAGE_BLIT GPU-side copies. */
    uint64_t image_blits        = pgraph_mtl_surface_image_blits();
    /* M5.9-followup-B+C — CPU-write dirty tracking + VRAM upload. */
    uint64_t vram_dirty_hits    = pgraph_mtl_surface_vram_dirty_hits();
    uint64_t vram_uploads       = pgraph_mtl_surface_vram_uploads();
    uint64_t vram_upload_bytes  = pgraph_mtl_surface_vram_upload_bytes();
    /* 2026-05-03 magenta-RT diagnostic — shape-mismatch recreates. */
    uint64_t recreate_mismatch  = pgraph_mtl_surface_recreate_shape_mismatch();
    /* M5.10 — VRAM-coherent surface download counters. */
    uint64_t surface_downloads_total      = pgraph_mtl_surface_downloads();
    uint64_t surface_download_bytes_total = pgraph_mtl_surface_download_bytes();
    uint64_t surface_download_us_total =
        pgraph_mtl_surface_download_us_total();
    /* W4 — per-draw color RT dump counter. */
    uint64_t draw_rt_dumps_total = pgraph_mtl_draw_rt_dumps_count();
    /* Tool 1 (2026-05-19) — surface-graph dump counter. */
    uint64_t surface_graph_dumps_total = pgraph_mtl_surface_graph_dumps();
    /* 2026-05-20 — cross-sibling sync counters. */
    uint64_t sibling_syncs_total      = pgraph_mtl_surface_sibling_syncs();
    uint64_t sibling_sync_skips_total = pgraph_mtl_surface_sibling_sync_skips();

    uint64_t draw_delta       = draw_total       - s_baseline_draw;
    uint64_t indexed_delta    = draw_indexed     - s_baseline_draw_indexed;
    uint64_t tri_delta        = native_tri_depth - s_baseline_native_tri_depth;
    uint64_t quad_delta       = native_quad      - s_baseline_native_quad;
    uint64_t clear_delta      = clear_total      - s_baseline_clear;
    uint64_t xlate_delta      = glsl_xlate       - s_baseline_glsl_translate;
    uint64_t xlate_fail_delta = glsl_xlate_fail  -
                                s_baseline_glsl_translate_failures;
    uint64_t val_ok_delta     = validate_ok      - s_baseline_validate_ok;
    uint64_t val_fail_delta   = validate_fail    - s_baseline_validate_fail;
    uint64_t pipe_hits_delta  = pipeline_hits    - s_baseline_pipeline_hits;
    uint64_t pipe_miss_delta  = pipeline_miss    - s_baseline_pipeline_misses;
    uint64_t pipe_fail_delta  = pipeline_fail    - s_baseline_pipeline_failed;
    uint64_t tex_up_delta     = tex_uploads      - s_baseline_tex_uploads;
    uint64_t tex_bytes_delta  = tex_upload_bytes - s_baseline_tex_upload_bytes;
    uint64_t tex_hits_delta   = tex_cache_hits   - s_baseline_tex_cache_hits;
    uint64_t tex_miss_delta   = tex_cache_miss   - s_baseline_tex_cache_misses;
    uint64_t pkey_built_delta     = pkey_built      -
                                    s_baseline_pipeline_key_built;
    uint64_t pkey_xlate_ok_delta  = pkey_xlate_ok   -
                                    s_baseline_pipeline_translated_ok;
    uint64_t pkey_xlate_fail_delta = pkey_xlate_fail -
                                     s_baseline_pipeline_translated_failed;
    uint64_t dispatch_us_delta = dispatch_us -
                                 s_baseline_dispatch_us_total;
    uint64_t tex_bind_us_delta = tex_bind_us -
                                 s_baseline_texture_bind_us_total;
    uint64_t draw_encode_us_delta = draw_encode_us -
                                    s_baseline_draw_encode_us_total;
    uint64_t pass_opens_delta = pass_opens -
                                s_baseline_draw_pass_opens;
    uint64_t pass_coalesced_delta = pass_coalesced -
                                    s_baseline_draw_pass_coalesced;
    uint64_t pass_flushes_delta = pass_flushes -
                                  s_baseline_draw_pass_flushes;
    uint64_t pass_flush_us_delta = pass_flush_us -
                                   s_baseline_open_pass_flush_us_total;
    uint64_t tex_upload_us_delta = tex_upload_us -
                                   s_baseline_tex_upload_us_total;
    uint64_t draw_xlated_delta    = draw_translated -
                                    s_baseline_draw_translated;
    uint64_t pipe_fallback_delta  = pipe_fallback   -
                                    s_baseline_pipeline_fallbacks;
    uint64_t ubo_pack_delta       = ubo_pack        -
                                    s_baseline_uniform_pack;
    uint64_t ubo_bytes_delta      = ubo_bytes       -
                                    s_baseline_uniform_bytes;
    uint64_t cc_queued_delta      = cc_queued       -
                                    s_baseline_compile_queued;
    uint64_t cc_completed_delta   = cc_completed    -
                                    s_baseline_compile_completed;
    uint64_t cc_async_fail_delta  = cc_async_failed -
                                    s_baseline_compile_async_failed;
    uint64_t draws_skip_delta     = draws_skipped   -
                                    s_baseline_draws_skipped_pending;
    uint64_t draws_uber_delta     = draws_uber      -
                                    s_baseline_draws_using_ubershader;
    uint64_t sc_loads_delta       = sc_loads        -
                                    s_baseline_shader_cache_loads;
    uint64_t sc_hits_delta        = sc_hits         -
                                    s_baseline_shader_cache_hits;
    uint64_t sc_misses_delta      = sc_misses       -
                                    s_baseline_shader_cache_misses;
    /* M10 — frame pacing. presents_delta is a delta-from-baseline like
     * the other counters; jitter_us_max snapshots the running max
     * inside the renderer and resets it after the snapshot so the
     * next interval starts fresh. The total-delta gate below treats
     * present activity as the activity check — when zero presents
     * happened in this interval, the M10 fields are skipped. */
    uint64_t presents_delta       = presents_total  -
                                    s_baseline_presents_total;
    uint64_t jitter_total_delta   = jitter_us_total -
                                    s_baseline_present_jitter_us_total;
    uint64_t draw_acq_fails_delta = draw_acq_fails  -
                                    s_baseline_drawable_acquire_fails;
    uint64_t dl_callbacks_delta   = dl_callbacks    -
                                    s_baseline_display_link_callbacks;
    uint64_t jitter_us_avg = (presents_delta > 0)
        ? (jitter_total_delta / presents_delta)
        : 0;
    /* M11 — MSAA. The resolve counter only ticks when MSAA is on; the
     * sample-count value is the latched effective config (1 = off). */
    uint64_t msaa_resolves_delta  = msaa_resolves   -
                                    s_baseline_msaa_resolve_count;
    uint64_t msaa_us_total_delta  = msaa_resolve_us -
                                    s_baseline_msaa_resolve_us_total;
    /* M12 — MetalFX spatial scaler. The presents counter only ticks
     * when XEMU_METAL_FX_SCALE >= 2 AND the drawable is larger than
     * the input texture (otherwise the scaler is bypassed for the
     * frame). The scale_factor field is the latched effective config
     * (1 = off, >= 2 = on). */
    uint64_t fx_us_delta          = fx_spatial_us   -
                                    s_baseline_fx_spatial_us_total;
    uint64_t fx_presents_delta    = fx_spatial_pres -
                                    s_baseline_fx_spatial_presents;
    /* M13 — per-stage GPU timing deltas. capture_active is a latched
     * snapshot of capture state (1 / 0); it does not have a
     * baseline-delta semantic. */
    uint64_t fx_gpu_us_delta      = fx_gpu_us        -
                                    s_baseline_fx_spatial_gpu_us_total;
    uint64_t vertex_us_delta      = vertex_us        -
                                    s_baseline_vertex_us_total;
    uint64_t fragment_us_delta    = fragment_us      -
                                    s_baseline_fragment_us_total;
    uint64_t present_gpu_us_delta = present_gpu_us   -
                                    s_baseline_present_gpu_us_total;
    uint64_t present_gpu_fr_delta = present_gpu_fr   -
                                    s_baseline_present_gpu_frames;
    uint64_t capture_seen_delta   = capture_seen     -
                                    s_baseline_capture_frames_seen;
    /* 2026-05-03 — PNG screenshot counter delta. */
    uint64_t screenshots_taken_delta = screenshots_taken -
                                       s_baseline_screenshots_taken;
    /* M5.9 — front-fb publish delta. */
    uint64_t front_fb_publishes_delta = front_fb_publishes -
                                        s_baseline_front_fb_publishes;
    /* M5.9-followup-A — NV097_IMAGE_BLIT GPU-side copy delta. */
    uint64_t image_blits_delta        = image_blits -
                                        s_baseline_image_blits;
    /* M5.9-followup-B+C — CPU-write dirty tracking + VRAM upload deltas. */
    uint64_t vram_dirty_hits_delta    = vram_dirty_hits -
                                        s_baseline_vram_dirty_hits;
    uint64_t vram_uploads_delta       = vram_uploads -
                                        s_baseline_vram_uploads;
    uint64_t vram_upload_bytes_delta  = vram_upload_bytes -
                                        s_baseline_vram_upload_bytes;
    uint64_t recreate_mismatch_delta  = recreate_mismatch -
                                        s_baseline_recreate_shape_mismatch;
    /* M5.10 — surface download deltas. */
    uint64_t surface_downloads_delta      = surface_downloads_total -
                                            s_baseline_surface_downloads;
    uint64_t surface_download_bytes_delta = surface_download_bytes_total -
                                            s_baseline_surface_download_bytes;
    uint64_t surface_download_us_delta = surface_download_us_total -
                                         s_baseline_surface_download_us_total;
    /* W4 — per-draw color RT dump delta. */
    uint64_t draw_rt_dumps_delta = draw_rt_dumps_total -
                                   s_baseline_draw_rt_dumps;
    uint64_t surface_graph_dumps_delta = surface_graph_dumps_total -
                                         s_baseline_surface_graph_dumps;
    uint64_t sibling_syncs_delta      = sibling_syncs_total -
                                        s_baseline_sibling_syncs;
    uint64_t sibling_sync_skips_delta = sibling_sync_skips_total -
                                        s_baseline_sibling_sync_skips;
    uint64_t flat_quad_propag_delta   = flat_quad_propag -
                                        s_baseline_flat_quad_propagations;

    s_baseline_draw             = draw_total;
    s_baseline_draw_indexed     = draw_indexed;
    s_baseline_native_tri_depth = native_tri_depth;
    s_baseline_native_quad      = native_quad;
    s_baseline_clear            = clear_total;
    s_baseline_glsl_translate           = glsl_xlate;
    s_baseline_glsl_translate_failures  = glsl_xlate_fail;
    s_baseline_validate_ok              = validate_ok;
    s_baseline_validate_fail            = validate_fail;
    s_baseline_pipeline_hits            = pipeline_hits;
    s_baseline_pipeline_misses          = pipeline_miss;
    s_baseline_pipeline_failed          = pipeline_fail;
    s_baseline_tex_uploads              = tex_uploads;
    s_baseline_tex_upload_bytes         = tex_upload_bytes;
    s_baseline_tex_cache_hits           = tex_cache_hits;
    s_baseline_tex_cache_misses         = tex_cache_miss;
    s_baseline_pipeline_key_built       = pkey_built;
    s_baseline_pipeline_translated_ok   = pkey_xlate_ok;
    s_baseline_pipeline_translated_failed = pkey_xlate_fail;
    s_baseline_dispatch_us_total        = dispatch_us;
    s_baseline_texture_bind_us_total    = tex_bind_us;
    s_baseline_draw_encode_us_total     = draw_encode_us;
    s_baseline_draw_pass_opens          = pass_opens;
    s_baseline_draw_pass_coalesced      = pass_coalesced;
    s_baseline_draw_pass_flushes        = pass_flushes;
    s_baseline_open_pass_flush_us_total = pass_flush_us;
    s_baseline_tex_upload_us_total      = tex_upload_us;
    s_baseline_draw_translated          = draw_translated;
    s_baseline_pipeline_fallbacks       = pipe_fallback;
    s_baseline_uniform_pack             = ubo_pack;
    s_baseline_uniform_bytes            = ubo_bytes;
    s_baseline_compile_queued           = cc_queued;
    s_baseline_compile_completed        = cc_completed;
    s_baseline_compile_async_failed     = cc_async_failed;
    s_baseline_draws_skipped_pending    = draws_skipped;
    s_baseline_draws_using_ubershader   = draws_uber;
    s_baseline_shader_cache_loads       = sc_loads;
    s_baseline_shader_cache_hits        = sc_hits;
    s_baseline_shader_cache_misses      = sc_misses;
    s_baseline_presents_total           = presents_total;
    s_baseline_present_jitter_us_total  = jitter_us_total;
    s_baseline_drawable_acquire_fails   = draw_acq_fails;
    s_baseline_display_link_callbacks   = dl_callbacks;
    s_baseline_msaa_resolve_count       = msaa_resolves;
    s_baseline_msaa_resolve_us_total    = msaa_resolve_us;
    s_baseline_fx_spatial_us_total      = fx_spatial_us;
    s_baseline_fx_spatial_presents      = fx_spatial_pres;
    s_baseline_fx_spatial_gpu_us_total  = fx_gpu_us;
    s_baseline_vertex_us_total          = vertex_us;
    s_baseline_fragment_us_total        = fragment_us;
    s_baseline_present_gpu_us_total     = present_gpu_us;
    s_baseline_present_gpu_frames       = present_gpu_fr;
    s_baseline_capture_frames_seen      = capture_seen;
    s_baseline_screenshots_taken        = screenshots_taken;
    s_baseline_front_fb_publishes       = front_fb_publishes;
    s_baseline_image_blits              = image_blits;
    s_baseline_vram_dirty_hits          = vram_dirty_hits;
    s_baseline_vram_uploads             = vram_uploads;
    s_baseline_vram_upload_bytes        = vram_upload_bytes;
    s_baseline_recreate_shape_mismatch  = recreate_mismatch;
    s_baseline_surface_downloads        = surface_downloads_total;
    s_baseline_surface_download_bytes   = surface_download_bytes_total;
    s_baseline_surface_download_us_total = surface_download_us_total;
    s_baseline_draw_rt_dumps             = draw_rt_dumps_total;
    s_baseline_surface_graph_dumps       = surface_graph_dumps_total;
    s_baseline_sibling_syncs             = sibling_syncs_total;
    s_baseline_sibling_sync_skips        = sibling_sync_skips_total;
    s_baseline_flat_quad_propagations    = flat_quad_propag;
    /* Reset the per-interval max after we've snapshotted it. */
    pgraph_mtl_present_jitter_us_max_reset();

    uint64_t total_delta = draw_delta | indexed_delta | tri_delta |
                           quad_delta | clear_delta |
                           xlate_delta | xlate_fail_delta |
                           val_ok_delta | val_fail_delta |
                           pipe_hits_delta | pipe_miss_delta | pipe_fail_delta |
                           tex_up_delta | tex_bytes_delta |
                           tex_hits_delta | tex_miss_delta |
                           pkey_built_delta | pkey_xlate_ok_delta |
                           pkey_xlate_fail_delta |
                           dispatch_us_delta | tex_bind_us_delta |
                           draw_encode_us_delta |
                           pass_opens_delta | pass_coalesced_delta |
                           pass_flushes_delta | pass_flush_us_delta |
                           tex_upload_us_delta |
                           draw_xlated_delta | pipe_fallback_delta |
                           ubo_pack_delta | ubo_bytes_delta |
                           cc_queued_delta | cc_completed_delta |
                           cc_async_fail_delta | draws_skip_delta |
                           draws_uber_delta |
                           sc_loads_delta | sc_hits_delta | sc_misses_delta |
                           presents_delta | draw_acq_fails_delta |
                           dl_callbacks_delta |
                           msaa_resolves_delta | msaa_us_total_delta |
                           fx_us_delta | fx_presents_delta |
                           fx_gpu_us_delta | vertex_us_delta |
                           fragment_us_delta | present_gpu_us_delta |
                           present_gpu_fr_delta | capture_seen_delta |
                           screenshots_taken_delta |
                           front_fb_publishes_delta |
                           image_blits_delta |
                           vram_dirty_hits_delta |
                           vram_uploads_delta |
                           vram_upload_bytes_delta |
                           recreate_mismatch_delta |
                           surface_downloads_delta |
                           surface_download_bytes_delta |
                           surface_download_us_delta |
                           draw_rt_dumps_delta |
                           surface_graph_dumps_delta |
                           sibling_syncs_delta |
                           sibling_sync_skips_delta |
                           flat_quad_propag_delta;
    if (total_delta == 0) {
        return;
    }

    fprintf(out,
            " METAL_DRAW_COUNT=%llu METAL_DRAW_INDEXED_COUNT=%llu"
            " METAL_NATIVE_TRI_DEPTH_DRAWS=%llu"
            " METAL_NATIVE_QUAD_DRAWS=%llu METAL_CLEAR_COUNT=%llu"
            " METAL_GLSL_TRANSLATE=%llu METAL_GLSL_TRANSLATE_FAIL=%llu"
            " METAL_SHADER_VALIDATE_OK=%llu"
            " METAL_SHADER_VALIDATE_FAIL=%llu"
            " METAL_PIPELINE_HITS=%llu METAL_PIPELINE_MISSES=%llu"
            " METAL_PIPELINE_FAILED=%llu"
            " METAL_TEX_UPLOADS_TOTAL=%llu METAL_TEX_UPLOAD_BYTES_TOTAL=%llu"
            " METAL_TEX_CACHE_HITS=%llu METAL_TEX_CACHE_MISSES=%llu"
            " METAL_PIPELINE_KEY_BUILT=%llu"
            " METAL_PIPELINE_TRANSLATED_OK=%llu"
            " METAL_PIPELINE_TRANSLATED_FAILED=%llu"
            " METAL_DISPATCH_US_TOTAL=%llu"
            " METAL_TEX_BIND_US_TOTAL=%llu"
            " METAL_DRAW_ENCODE_US_TOTAL=%llu"
            " METAL_DRAW_PASS_OPENS=%llu"
            " METAL_DRAW_PASS_COALESCED=%llu"
            " METAL_DRAW_PASS_FLUSHES=%llu"
            " METAL_OPEN_PASS_FLUSH_US_TOTAL=%llu"
            " METAL_TEX_UPLOAD_US_TOTAL=%llu"
            " METAL_DRAW_TRANSLATED=%llu"
            " METAL_PIPELINE_FALLBACKS=%llu"
            " METAL_UNIFORM_PACK=%llu"
            " METAL_UNIFORM_BYTES=%llu"
            " METAL_SHADER_COMPILE_QUEUED_TOTAL=%llu"
            " METAL_SHADER_COMPILE_COMPLETED_TOTAL=%llu"
            " METAL_SHADER_COMPILE_FAILED_TOTAL=%llu"
            " METAL_DRAWS_SKIPPED_PENDING_TOTAL=%llu"
            " METAL_DRAWS_USING_UBERSHADER_TOTAL=%llu"
            " METAL_SHADER_CACHE_LOADS=%llu"
            " METAL_SHADER_CACHE_HITS=%llu"
            " METAL_SHADER_CACHE_MISSES=%llu"
            " METAL_PRESENTS=%llu"
            " METAL_PRESENT_JITTER_US_TOTAL=%llu"
            " METAL_PRESENT_JITTER_US_AVG=%llu"
            " METAL_PRESENT_JITTER_US_MAX=%llu"
            " METAL_DRAWABLE_ACQUIRE_FAILS=%llu"
            " METAL_DISPLAY_LINK_CALLBACKS=%llu"
            " METAL_MSAA_RESOLVE_COUNT=%llu"
            " METAL_MSAA_RESOLVE_US_TOTAL=%llu"
            " METAL_MSAA_SAMPLE_COUNT=%u"
            " METAL_FX_SPATIAL_PRESENTS=%llu"
            " METAL_FX_SPATIAL_US_TOTAL=%llu"
            " METAL_FX_SCALE_FACTOR=%u"
            " METAL_FX_SPATIAL_GPU_US_TOTAL=%llu"
            " METAL_VERTEX_US_TOTAL=%llu"
            " METAL_FRAGMENT_US_TOTAL=%llu"
            " METAL_PRESENT_GPU_US_TOTAL=%llu"
            " METAL_PRESENT_GPU_FRAMES=%llu"
            " METAL_CAPTURE_FRAMES=%llu"
            " METAL_CAPTURE_ACTIVE=%u"
            " METAL_SCREENSHOTS_TAKEN=%llu"
            " METAL_FRONT_FB_PUBLISHES=%llu"
            " METAL_SURFACE_CACHE_SIZE=%llu"
            " METAL_IMAGE_BLITS=%llu"
            " METAL_SURFACE_VRAM_DIRTY_HITS=%llu"
            " METAL_SURFACE_VRAM_UPLOADS=%llu"
            " METAL_SURFACE_VRAM_UPLOAD_BYTES=%llu"
            " METAL_SURFACE_RECREATE_SHAPE_MISMATCH=%llu"
            " METAL_SURFACE_DOWNLOADS=%llu"
            " METAL_SURFACE_DOWNLOAD_BYTES=%llu"
            " METAL_SURFACE_DOWNLOAD_US_TOTAL=%llu"
            " METAL_DRAW_RT_DUMPS=%llu"
            " METAL_SURFACE_GRAPH_DUMPS=%llu"
            " METAL_SIBLING_SYNCS=%llu"
            " METAL_SIBLING_SYNC_SKIPS=%llu"
            " METAL_FLAT_QUAD_PROPAGATIONS=%llu",
            (unsigned long long)draw_delta,
            (unsigned long long)indexed_delta,
            (unsigned long long)tri_delta,
            (unsigned long long)quad_delta,
            (unsigned long long)clear_delta,
            (unsigned long long)xlate_delta,
            (unsigned long long)xlate_fail_delta,
            (unsigned long long)val_ok_delta,
            (unsigned long long)val_fail_delta,
            (unsigned long long)pipe_hits_delta,
            (unsigned long long)pipe_miss_delta,
            (unsigned long long)pipe_fail_delta,
            (unsigned long long)tex_up_delta,
            (unsigned long long)tex_bytes_delta,
            (unsigned long long)tex_hits_delta,
            (unsigned long long)tex_miss_delta,
            (unsigned long long)pkey_built_delta,
            (unsigned long long)pkey_xlate_ok_delta,
            (unsigned long long)pkey_xlate_fail_delta,
            (unsigned long long)dispatch_us_delta,
            (unsigned long long)tex_bind_us_delta,
            (unsigned long long)draw_encode_us_delta,
            (unsigned long long)pass_opens_delta,
            (unsigned long long)pass_coalesced_delta,
            (unsigned long long)pass_flushes_delta,
            (unsigned long long)pass_flush_us_delta,
            (unsigned long long)tex_upload_us_delta,
            (unsigned long long)draw_xlated_delta,
            (unsigned long long)pipe_fallback_delta,
            (unsigned long long)ubo_pack_delta,
            (unsigned long long)ubo_bytes_delta,
            (unsigned long long)cc_queued_delta,
            (unsigned long long)cc_completed_delta,
            (unsigned long long)cc_async_fail_delta,
            (unsigned long long)draws_skip_delta,
            (unsigned long long)draws_uber_delta,
            (unsigned long long)sc_loads_delta,
            (unsigned long long)sc_hits_delta,
            (unsigned long long)sc_misses_delta,
            (unsigned long long)presents_delta,
            (unsigned long long)jitter_total_delta,
            (unsigned long long)jitter_us_avg,
            (unsigned long long)jitter_us_max,
            (unsigned long long)draw_acq_fails_delta,
            (unsigned long long)dl_callbacks_delta,
            (unsigned long long)msaa_resolves_delta,
            (unsigned long long)msaa_us_total_delta,
            (unsigned)msaa_samples,
            (unsigned long long)fx_presents_delta,
            (unsigned long long)fx_us_delta,
            (unsigned)fx_scale_factor,
            (unsigned long long)fx_gpu_us_delta,
            (unsigned long long)vertex_us_delta,
            (unsigned long long)fragment_us_delta,
            (unsigned long long)present_gpu_us_delta,
            (unsigned long long)present_gpu_fr_delta,
            (unsigned long long)capture_seen_delta,
            (unsigned)capture_active,
            (unsigned long long)screenshots_taken_delta,
            (unsigned long long)front_fb_publishes_delta,
            (unsigned long long)surface_cache_size,
            (unsigned long long)image_blits_delta,
            (unsigned long long)vram_dirty_hits_delta,
            (unsigned long long)vram_uploads_delta,
            (unsigned long long)vram_upload_bytes_delta,
            (unsigned long long)recreate_mismatch_delta,
            (unsigned long long)surface_downloads_delta,
            (unsigned long long)surface_download_bytes_delta,
            (unsigned long long)surface_download_us_delta,
            (unsigned long long)draw_rt_dumps_delta,
            (unsigned long long)surface_graph_dumps_delta,
            (unsigned long long)sibling_syncs_delta,
            (unsigned long long)sibling_sync_skips_delta,
            (unsigned long long)flat_quad_propag_delta);
}
