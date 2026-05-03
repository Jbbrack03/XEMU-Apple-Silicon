/*
 * xemu Metal renderer host integration — Objective-C implementation.
 *
 * Slice M1 — window + device + ImGui-Metal HUD.
 *
 * This file does NOT include hw/xbox/nv2a/nv2a_int.h or any per-target
 * header. Per docs/apple-silicon/metal-renderer-plan.md slice M1, the
 * .m file communicates with the rest of the renderer through opaque
 * handles only, because meson's per-target c_args
 * (-DCOMPILING_PER_TARGET / -DCONFIG_TARGET / -DCONFIG_DEVICES) do not
 * propagate to .m / objc_COMPILER builds. apple-gfx.m uses the same
 * boundary.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "xemu-metal.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <mach/mach_time.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>

#include <imgui.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_sdl3.h>

/* 2026-05-03 — FPNG (used for the headless drawable-snapshot path).
 * We do NOT #include <fpng.h> directly here because fpng.h pulls in
 * <vector>, which transitively includes libc++'s <atomic>; libc++'s
 * <atomic> errors out when <stdatomic.h> (C11 atomics) is also in
 * scope on -std=c++17 ("<atomic> is incompatible with <stdatomic.h>
 * before C++23"). xemu-metal.mm already needs <stdatomic.h> for the
 * inter-thread counter atomics. The two encode functions used here
 * have C++ linkage in namespace fpng; forward-declare them by hand
 * so the link resolves without dragging in <vector>. */
namespace fpng {
    void fpng_init();
    bool fpng_encode_image_to_file(const char *pFilename,
                                   const void *pImage,
                                   uint32_t w,
                                   uint32_t h,
                                   uint32_t num_chans,
                                   uint32_t flags);
}

/* Apple Silicon performance fork: read by the M10 frame-pacing path
 * to compute the next presentation deadline. Defined in ui/xemu.c. */
extern uint64_t vblank_interval_ns;

/* Module-level state. Single window per process; switching renderers
 * requires restart, matching the GL/Vulkan story. */
static SDL_MetalView         s_metal_view;
static CAMetalLayer         *s_layer;
static id<MTLDevice>         s_device;
static id<MTLCommandQueue>   s_queue;
static bool                  s_active;

/* Slice M2 framebuffer compositor: a fullscreen-triangle pipeline that
 * samples the NV2A's color RT and writes it to the layer's drawable.
 * Built lazily on first use; nil until then. */
static id<MTLRenderPipelineState> s_present_pipeline;
static id<MTLSamplerState>        s_present_sampler;
static NSString *const k_present_msl =
    @"#include <metal_stdlib>\n"
    @"using namespace metal;\n"
    @"struct VOut {\n"
    @"    float4 pos [[position]];\n"
    @"    float2 uv;\n"
    @"};\n"
    @"vertex VOut xemu_present_vs(uint vid [[vertex_id]]) {\n"
    @"    /* Fullscreen triangle: clip-space coords (-1,-3), (-1,1), (3,1).\n"
    @"     * UV computed so the on-screen quad spans (0,0)..(1,1). */\n"
    @"    float2 p = float2((vid == 2) ? 3.0 : -1.0,\n"
    @"                      (vid == 0) ? -3.0 :  1.0);\n"
    @"    VOut o;\n"
    @"    o.pos = float4(p, 0.0, 1.0);\n"
    @"    o.uv  = float2((p.x + 1.0) * 0.5,\n"
    @"                   1.0 - (p.y + 1.0) * 0.5);\n"
    @"    return o;\n"
    @"}\n"
    @"fragment float4 xemu_present_fs(VOut in [[stage_in]],\n"
    @"                                texture2d<float> tex [[texture(0)]],\n"
    @"                                sampler samp        [[sampler(0)]]) {\n"
    @"    return tex.sample(samp, in.uv);\n"
    @"}\n";

/* Bridge to the NV2A surface manager (slice M2). The surface manager
 * publishes its current color RT via this side-channel; we cannot use
 * the PGRAPHRenderer.ops.get_framebuffer_surface op because its return
 * type is `int` and an id<MTLTexture> doesn't fit. See
 * hw/xbox/nv2a/pgraph/mtl/surface.h. */
extern "C" void *pgraph_mtl_get_framebuffer_metal_texture(void);
extern "C" void *pgraph_mtl_surface_get_metal_texture_at(uint32_t vram_addr);
extern "C" void  pgraph_mtl_release_framebuffer_metal_texture(void);

/* M5 — weak forward decl of the per-target shader-validation harness.
 * Lives in libqemu-i386-softmmu.a (per-target). The weak link lets
 * the host binary link cleanly even on platform configurations where
 * the per-target object is absent. */
extern "C" bool pgraph_mtl_shader_validate_run(void) __attribute__((weak));

static void xemu_metal_run_validation_if_requested(void)
{
    if (pgraph_mtl_shader_validate_run == NULL) {
        return;
    }
    const char *validate_env = getenv("XEMU_METAL_SHADER_VALIDATE");
    if (!validate_env || validate_env[0] == '\0' || validate_env[0] == '0') {
        return;
    }
    bool ok = pgraph_mtl_shader_validate_run();
    const char *exit_env = getenv("XEMU_METAL_SHADER_VALIDATE_AND_EXIT");
    if (exit_env && exit_env[0] != '\0' && exit_env[0] != '0') {
        fprintf(stderr,
                "[xemu-metal-validate] exiting after validation "
                "(XEMU_METAL_SHADER_VALIDATE_AND_EXIT=%s, "
                "result=%s)\n",
                exit_env, ok ? "PASS" : "FAIL");
        exit(ok ? 0 : 1);
    }
}

/* Per-frame transient state held between begin/end_imgui_frame. */
static id<CAMetalDrawable>     s_current_drawable;
static MTLRenderPassDescriptor *s_current_pass_desc;
static id<MTLCommandBuffer>    s_current_cmd;
static id<MTLRenderCommandEncoder> s_current_enc;

/* M10 — frame pacing state. */
static bool                       s_force_legacy_present;
static mach_timebase_info_data_t  s_timebase;
static double                     s_seconds_per_mach_unit;
static uint64_t                   s_last_present_target_ns;
static uint64_t                   s_last_present_target_mach;

/* M10 — jitter counters (atomic so the per-interval emit can sample
 * without holding any lock). */
static _Atomic uint64_t s_presents_total;
static _Atomic uint64_t s_present_jitter_us_total;
static _Atomic uint64_t s_present_jitter_us_max;
static _Atomic uint64_t s_drawable_acquire_fails;
static _Atomic uint64_t s_display_link_callbacks;  /* reserved for M10.1 */

/* M12 — MetalFX spatial scaler state. Latched once at xemu_metal_init
 * from XEMU_METAL_FX_SCALE; rebuilt lazily when input or output
 * dimensions / pixel format change. The scaler upscales the post-
 * resolve NV2A color RT into a private intermediate texture; the
 * existing present pipeline then samples that intermediate to write
 * the drawable.
 *
 * Semantics of XEMU_METAL_FX_SCALE:
 *   1 (default) — off. Bypass MetalFX; the present pipeline samples
 *                 the NV2A color RT directly with a linear filter.
 *   >= 2        — on. The scaler is enabled whenever the drawable is
 *                 larger than the input. The actual upscale ratio is
 *                 determined by `drawable_size / input_size`; the
 *                 numeric value is currently a 0-vs-1 enable flag so
 *                 the API surface lines up with future quality-tier
 *                 variants.
 *
 * Output texture format: matches the layer's BGRA8Unorm_sRGB so the
 * subsequent present-pipeline composite is byte-identical to the
 * non-MetalFX path's color processing.
 *
 * MTLFXTemporalScaler is intentionally not implemented — synthesizing
 * motion vectors from camera-only reprojection is risky on dynamic
 * scenes (NV2A has no native motion vectors). See
 * docs/apple-silicon/metal-renderer-plan.md §M12.
 */
static uint32_t              s_metal_fx_scale_requested;  /* raw env */
static bool                  s_metal_fx_enabled;          /* >= 2 */
static id<MTLFXSpatialScaler> s_metal_fx_scaler;
static id<MTLTexture>        s_metal_fx_output_tex;
static NSUInteger            s_metal_fx_input_w;
static NSUInteger            s_metal_fx_input_h;
static NSUInteger            s_metal_fx_output_w;
static NSUInteger            s_metal_fx_output_h;
static MTLPixelFormat        s_metal_fx_input_format;
static _Atomic uint64_t      s_metal_fx_spatial_us_total;
static _Atomic uint64_t      s_metal_fx_spatial_presents;
/* M13 — MetalFX GPU-side timing replaces M12's CPU-side wallclock
 * placeholder once the per-cmdbuf addCompletedHandler is wired up. The
 * value is the cumulative (cmdbuf.GPUEndTime - cmdbuf.GPUStartTime) for
 * frames where MetalFX encoded; it is an upper bound on the scaler
 * cost (the same cmdbuf also runs the present + HUD render encoder),
 * which is the closest GPU-side timing we can report without splitting
 * the work into a dedicated cmdbuf. Real per-pass breakdown comes via
 * the per-stage counter sample buffers below. */
static _Atomic uint64_t      s_metal_fx_spatial_gpu_us_total;

/* M13 — per-frame MTLCounterSampleBuffer state. Apple Silicon supports
 * MTLCounterSamplingPointAtStageBoundary (vertex / fragment stage
 * starts and ends), giving per-render-pass vertex+fragment GPU time.
 * The infrastructure here samples the HUD/present render pass once
 * per frame and accumulates totals into atomic counters that surface
 * on the xemu-perf: interval line. Counter sample buffers must be
 * resolved on the CPU after the cmdbuf completes; the
 * addCompletedHandler block below handles that.
 *
 * The "stage boundary" sampling form is supported on Apple4+ (M1+).
 * On older devices (Intel) this would be MTLCounterSamplingPointAtDrawBoundary
 * which is not implemented here — Apple Silicon is the only target.
 * If the device does not support stage-boundary sampling, the entire
 * counter-sample-buffer wiring is skipped and the V/F counters stay
 * at zero (no fallback path). */
static bool                  s_counter_sampling_supported;
static id<MTLCounterSampleBuffer> s_counter_sample_buffer;
static NSUInteger            s_counter_sample_buffer_capacity;
static double                s_gpu_timebase_seconds_per_unit;
static _Atomic uint64_t      s_metal_vertex_us_total;
static _Atomic uint64_t      s_metal_fragment_us_total;
static _Atomic uint64_t      s_metal_present_gpu_us_total;
static _Atomic uint64_t      s_metal_present_gpu_frames;

/* M13 — programmatic Metal capture via MTLCaptureManager. Gated on
 * XEMU_METAL_CAPTURE=path.gputrace. Required for the .gputrace to
 * open in Xcode's GPU Frame Capture viewer; production builds without
 * MetalCaptureEnabled in Info.plist will silently fail to start.
 *
 * Capture is bounded by frame count so the .gputrace file stays
 * tractable to open in Xcode. Default 60 frames; override via
 * XEMU_METAL_CAPTURE_FRAMES=N. After N frames presented post-startCapture
 * the manager is stopped automatically. xemu_metal_shutdown also
 * stops capture if it is still running. */
static bool                  s_capture_enabled;
static bool                  s_capture_active;
static uint64_t              s_capture_frames_target;
static _Atomic uint64_t      s_capture_frames_seen;

/* 2026-05-03 — programmatic PNG screenshot of the final composited
 * drawable. Gated on XEMU_METAL_SCREENSHOT_PATH=/path/to/file.png.
 * The capture point is BEFORE presentDrawable: but AFTER the HUD
 * render encoder has closed, so the captured image is the same set of
 * pixels the user is about to see on screen. Compared with macOS
 * `screencapture`, this path takes no Screen-Recording permission
 * dialog, never occludes the xemu window, and runs entirely on Metal
 * command buffers so it does not break METAL_PRESENTS accounting.
 *
 * Frame numbering is 1-indexed against the about-to-present frame
 * (atomic_load(&s_presents_total) + 1) so XEMU_METAL_SCREENSHOT_AT_FRAME=1
 * captures the very first present, =60 captures the 60th, etc. The
 * default at-frame is 60 so a typical scripted-input boot has time to
 * settle before the snapshot fires.
 *
 * XEMU_METAL_SCREENSHOT_INTERVAL=N (>=1) repeats the capture every N
 * frames after the first one, writing `<base>.0001.png`,
 * `<base>.0002.png`, ... Default 0 disables the periodic mode and the
 * single capture writes to the path verbatim.
 *
 * Note: the drawable texture cannot be the source of a blit when the
 * CAMetalLayer is configured with framebufferOnly=YES. We flip that
 * to NO at xemu_metal_init iff XEMU_METAL_SCREENSHOT_PATH is set so
 * the steady-state perf path keeps display compression on for users
 * who never request a screenshot. */
static char                 *s_screenshot_path;          /* malloc'd or NULL */
static uint64_t              s_screenshot_at_frame;      /* 1-indexed; 0 = disabled */
static uint64_t              s_screenshot_interval;      /* 0 = single shot */
/* 2026-05-03 diagnostic — XEMU_METAL_SCREENSHOT_SOURCE selects the
 * texture captured. "drawable" (default) reads the post-HUD final
 * drawable; "nv2a" reads the NV2A framebuffer texture (pre-present);
 * "vram:0xADDR" reads a specific cached SurfaceBinding by vram_addr
 * (useful to inspect back-buffer / aux RTs without going through
 * CRTC publish — magenta artifact investigation). */
static int                   s_screenshot_source;        /* 0=drawable, 1=nv2a, 2=vram_addr */
static uint32_t              s_screenshot_vram_addr;     /* used when source==2 */
static _Atomic uint64_t      s_screenshots_taken;        /* counter for METAL_SCREENSHOTS_TAKEN */
static _Atomic uint64_t      s_screenshots_done;         /* in-flight + completed; for filename suffix */
/* Separate "end-of-frame" tick: bumps every time
 * xemu_metal_end_imgui_frame runs the cmdbuf commit, regardless of
 * whether addPresentedHandler: ever fires. We can't use
 * s_presents_total for the screenshot trigger because the macOS
 * Screen-Recording dialog can occlude the xemu window during a
 * benchmark, in which case addPresentedHandler: does not fire and
 * s_presents_total stays at zero (METAL_PRESENTS=0 in the perf
 * counters — see handoff.md). The end-frame counter is the rendering
 * thread's view of "how many frames have been submitted" which is the
 * trigger we actually want for a deterministic per-frame snapshot. */
static _Atomic uint64_t      s_end_frames;

static uint64_t mach_now_ns(void)
{
    if (s_timebase.denom == 0) {
        mach_timebase_info(&s_timebase);
        s_seconds_per_mach_unit =
            (double)s_timebase.numer / (double)s_timebase.denom / 1.0e9;
    }
    uint64_t mach = mach_absolute_time();
    return mach * s_timebase.numer / s_timebase.denom;
}

static uint64_t ns_to_mach(uint64_t ns)
{
    if (s_timebase.denom == 0) {
        mach_timebase_info(&s_timebase);
        s_seconds_per_mach_unit =
            (double)s_timebase.numer / (double)s_timebase.denom / 1.0e9;
    }
    /* mach = ns * denom / numer. Using uint64 throughout; for typical
     * ns values (< 2^60) this does not overflow on Apple Silicon
     * where numer == denom == 1. */
    return ns * s_timebase.denom / s_timebase.numer;
}

static double mach_to_seconds(uint64_t mach)
{
    if (s_timebase.denom == 0) {
        mach_timebase_info(&s_timebase);
        s_seconds_per_mach_unit =
            (double)s_timebase.numer / (double)s_timebase.denom / 1.0e9;
    }
    return (double)mach * s_seconds_per_mach_unit;
}

static void atomic_max_u64(_Atomic uint64_t *p, uint64_t v)
{
    uint64_t old = atomic_load(p);
    while (v > old &&
           !atomic_compare_exchange_weak(p, &old, v)) {
        /* CAS failed; old has been updated by atomic_compare_exchange_weak. */
    }
}

/* Counter accessors — strong symbols matching the weak default
 * declarations in util/xemu-metal-perf.c. They are emitted on the
 * `xemu-perf:` interval line by xemu_metal_perf_emit_and_reset. */
extern "C" uint64_t pgraph_mtl_present_total(void)
{
    return atomic_load(&s_presents_total);
}
extern "C" uint64_t pgraph_mtl_present_jitter_us_total(void)
{
    return atomic_load(&s_present_jitter_us_total);
}
extern "C" uint64_t pgraph_mtl_present_jitter_us_max_value(void)
{
    return atomic_load(&s_present_jitter_us_max);
}
extern "C" void pgraph_mtl_present_jitter_us_max_reset(void)
{
    atomic_store(&s_present_jitter_us_max, 0);
}
extern "C" uint64_t pgraph_mtl_drawable_acquire_fails(void)
{
    return atomic_load(&s_drawable_acquire_fails);
}
extern "C" uint64_t pgraph_mtl_display_link_callbacks(void)
{
    return atomic_load(&s_display_link_callbacks);
}

/* M12 — MetalFX spatial scaler counters. Strong symbols matching the
 * weak defaults in util/xemu-metal-perf.c. Both are zero whenever
 * XEMU_METAL_FX_SCALE < 2 — the encode site below is the only writer
 * and it bails before incrementing when the scaler is disabled. */
extern "C" uint64_t pgraph_mtl_fx_spatial_us_total(void)
{
    return atomic_load(&s_metal_fx_spatial_us_total);
}
extern "C" uint64_t pgraph_mtl_fx_spatial_presents(void)
{
    return atomic_load(&s_metal_fx_spatial_presents);
}
extern "C" uint32_t pgraph_mtl_fx_scale_factor(void)
{
    /* Reports the latched effective scale factor (1 = off, >=2 = on).
     * surfaces on the xemu-perf: interval line so a run summary can
     * tell whether MetalFX was active without parsing the startup
     * log line. */
    return s_metal_fx_enabled ? (s_metal_fx_scale_requested ? s_metal_fx_scale_requested : 2) : 1;
}

extern "C" uint64_t pgraph_mtl_fx_spatial_gpu_us_total(void)
{
    return atomic_load(&s_metal_fx_spatial_gpu_us_total);
}

/* M13 — per-stage GPU-time counter accessors. Strong symbols matching
 * the weak defaults in util/xemu-metal-perf.c. Both are zero whenever
 * counter sampling is not supported on the active device, or no
 * presents have completed yet, or s_counter_sample_buffer creation
 * failed at init. */
extern "C" uint64_t pgraph_mtl_vertex_us_total(void)
{
    return atomic_load(&s_metal_vertex_us_total);
}

extern "C" uint64_t pgraph_mtl_fragment_us_total(void)
{
    return atomic_load(&s_metal_fragment_us_total);
}

extern "C" uint64_t pgraph_mtl_present_gpu_us_total(void)
{
    return atomic_load(&s_metal_present_gpu_us_total);
}

extern "C" uint64_t pgraph_mtl_present_gpu_frames(void)
{
    return atomic_load(&s_metal_present_gpu_frames);
}

extern "C" uint64_t pgraph_mtl_capture_frames_seen(void)
{
    return atomic_load(&s_capture_frames_seen);
}

extern "C" uint32_t pgraph_mtl_capture_active(void)
{
    return s_capture_active ? 1u : 0u;
}

extern "C" uint64_t pgraph_mtl_screenshots_taken(void)
{
    return atomic_load(&s_screenshots_taken);
}

static uint32_t parse_metal_fx_scale_env(uint32_t *out_requested)
{
    if (out_requested) {
        *out_requested = 0;
    }
    const char *value = getenv("XEMU_METAL_FX_SCALE");
    if (value == NULL || value[0] == '\0') {
        return 1;
    }
    char *endp = NULL;
    unsigned long parsed = strtoul(value, &endp, 10);
    if (endp == NULL || *endp != '\0') {
        return 1;
    }
    if (out_requested) {
        *out_requested = (uint32_t)parsed;
    }
    /* {1, 2, 3} valid per plan §M12; values >= 2 enable. Anything
     * else collapses to off. The numeric value is preserved in
     * `requested` so the startup log line shows what the user asked
     * for. */
    if (parsed >= 2 && parsed <= 3) {
        return (uint32_t)parsed;
    }
    return 1;
}

/* Build (or rebuild) the MetalFX spatial scaler + intermediate output
 * texture. Called from end_imgui_frame when the scaler is enabled and
 * either (a) it is the first frame after init or (b) the input
 * texture's dimensions / pixel format have changed since the previous
 * scaler instance. The output texture is sized to match the drawable.
 *
 * Returns true on success; false on any allocation / scaler-build
 * failure (in which case the caller falls back to the non-MetalFX
 * present path for this frame). The s_metal_fx_* state is left in a
 * consistent "rebuild on next frame" state if anything fails. */
static bool build_metal_fx_scaler_if_needed(NSUInteger in_w,
                                            NSUInteger in_h,
                                            MTLPixelFormat in_fmt,
                                            NSUInteger out_w,
                                            NSUInteger out_h)
{
    if (!s_metal_fx_enabled || s_device == nil) {
        return false;
    }
    /* Refuse to upscale if the input is already at-or-above the
     * drawable. MetalFX is a quality-uplift over bilinear at upscale;
     * at 1:1 or downscale it adds latency for no visible win. */
    if (in_w == 0 || in_h == 0 || out_w <= in_w || out_h <= in_h) {
        return false;
    }
    if (s_metal_fx_scaler != nil &&
        s_metal_fx_input_w == in_w && s_metal_fx_input_h == in_h &&
        s_metal_fx_output_w == out_w && s_metal_fx_output_h == out_h &&
        s_metal_fx_input_format == in_fmt &&
        s_metal_fx_output_tex != nil) {
        return true;
    }

    /* Tear down any prior instance before rebuilding. */
    s_metal_fx_scaler = nil;
    s_metal_fx_output_tex = nil;

    MTLFXSpatialScalerDescriptor *desc =
        [[MTLFXSpatialScalerDescriptor alloc] init];
    desc.inputWidth         = in_w;
    desc.inputHeight        = in_h;
    desc.outputWidth        = out_w;
    desc.outputHeight       = out_h;
    desc.colorTextureFormat = in_fmt;
    /* Output format matches the layer (BGRA8Unorm_sRGB) so the
     * subsequent present-pipeline composite — which already operates
     * in the layer's color space — does not need a colorspace
     * conversion. */
    desc.outputTextureFormat = s_layer.pixelFormat;
    /* `Perceptual` tells MetalFX the input is gamma-encoded; the
     * scaler does the perceptual-vs-linear conversion internally.
     * Matches the Apple metal-api-reference.md §9 example. The NV2A
     * color RT comes through the existing M11 resolve path as
     * BGRA8Unorm_sRGB so this is correct here. */
    desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;

    s_metal_fx_scaler = [desc newSpatialScalerWithDevice:s_device];
    if (s_metal_fx_scaler == nil) {
        fprintf(stderr,
                "xemu-metal: MTLFXSpatialScalerDescriptor "
                "newSpatialScalerWithDevice: returned nil "
                "(in=%lux%lu fmt=%lu out=%lux%lu); falling back\n",
                (unsigned long)in_w, (unsigned long)in_h,
                (unsigned long)in_fmt,
                (unsigned long)out_w, (unsigned long)out_h);
        return false;
    }
    /* MTLFXSpatialScaler does not expose a `label` property (unlike
     * MTLRenderPipelineState / MTLTexture). The intermediate output
     * texture below carries an identifying label instead, which is
     * what shows up in Metal capture / Xcode GPU frame capture. */

    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s_layer.pixelFormat
                                                            width:out_w
                                                           height:out_h
                                                        mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    /* MetalFX writes the upscaled result into outputTexture
     * (ShaderWrite); the present pipeline reads it (ShaderRead).
     * RenderTarget is set so the texture can be used as a render-pass
     * attachment if a future slice swaps the present pipeline for a
     * direct-into-output render pass. */
    td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead |
               MTLTextureUsageRenderTarget;
    s_metal_fx_output_tex = [s_device newTextureWithDescriptor:td];
    if (s_metal_fx_output_tex == nil) {
        fprintf(stderr,
                "xemu-metal: MetalFX intermediate output-texture "
                "alloc failed (%lux%lu); falling back\n",
                (unsigned long)out_w, (unsigned long)out_h);
        s_metal_fx_scaler = nil;
        return false;
    }
    s_metal_fx_output_tex.label = @"xemu.metal.fx_spatial_output";

    s_metal_fx_input_w      = in_w;
    s_metal_fx_input_h      = in_h;
    s_metal_fx_output_w     = out_w;
    s_metal_fx_output_h     = out_h;
    s_metal_fx_input_format = in_fmt;

    fprintf(stderr,
            "xemu-perf: metal_fx_spatial built in=%lux%lu fmt=%lu "
            "out=%lux%lu\n",
            (unsigned long)in_w, (unsigned long)in_h,
            (unsigned long)in_fmt,
            (unsigned long)out_w, (unsigned long)out_h);
    return true;
}

/* M13 — start a programmatic Metal capture if XEMU_METAL_CAPTURE is set.
 * Capture writes a .gputrace file; the file is finalized when
 * stopCapture is called (we do that either after N frames or in
 * xemu_metal_shutdown).
 *
 * Capture preconditions per Apple's docs:
 *   1. The app's Info.plist must have MetalCaptureEnabled = YES, or
 *      the env var MTL_CAPTURE_ENABLED=1 must be set.
 *   2. The destination MTLCaptureDestinationGPUTraceDocument must
 *      be supported on the active device — checked via
 *      [manager supportsDestination:].
 *
 * If capture cannot start (preconditions unmet, or the path is
 * unwritable), the function logs the failure and falls back silently
 * — capture is a development tool and a missing .gputrace must not
 * abort the run. */
static void start_metal_capture_if_requested(void)
{
    const char *path = getenv("XEMU_METAL_CAPTURE");
    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (s_device == nil) {
        fprintf(stderr,
                "xemu-metal: XEMU_METAL_CAPTURE set but device not yet "
                "initialized; capture skipped\n");
        return;
    }

    s_capture_enabled = true;
    /* Default to 60 frames so the .gputrace stays small enough to
     * open in Xcode (each frame is ~10-50 MB). Override with
     * XEMU_METAL_CAPTURE_FRAMES=N. 0 means "until shutdown". */
    s_capture_frames_target = 60;
    const char *frames_env = getenv("XEMU_METAL_CAPTURE_FRAMES");
    if (frames_env != NULL && frames_env[0] != '\0') {
        char *endp = NULL;
        unsigned long n = strtoul(frames_env, &endp, 10);
        if (endp != NULL && *endp == '\0') {
            s_capture_frames_target = (uint64_t)n;
        }
    }

    MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
    if (mgr == nil) {
        fprintf(stderr,
                "xemu-metal: MTLCaptureManager sharedCaptureManager "
                "returned nil; capture disabled\n");
        s_capture_enabled = false;
        return;
    }
    if (![mgr supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
        fprintf(stderr,
                "xemu-metal: GPUTraceDocument capture destination not "
                "supported (likely missing MetalCaptureEnabled in "
                "Info.plist or MTL_CAPTURE_ENABLED=1 env); capture "
                "disabled. Path was: %s\n",
                path);
        s_capture_enabled = false;
        return;
    }

    NSString *path_ns = [NSString stringWithUTF8String:path];
    NSURL *url = [NSURL fileURLWithPath:path_ns];

    MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
    desc.captureObject = s_device;
    desc.destination   = MTLCaptureDestinationGPUTraceDocument;
    desc.outputURL     = url;

    NSError *err = nil;
    BOOL ok = [mgr startCaptureWithDescriptor:desc error:&err];
    if (!ok) {
        fprintf(stderr,
                "xemu-metal: startCaptureWithDescriptor failed: %s "
                "(path=%s)\n",
                [[err localizedDescription] UTF8String] ?: "(unknown)",
                path);
        s_capture_enabled = false;
        return;
    }

    s_capture_active = true;
    fprintf(stderr,
            "xemu-perf: metal_capture_started path=%s frames_target=%llu\n",
            path,
            (unsigned long long)s_capture_frames_target);
}

/* M13 — stop the running capture and release the manager hold. Called
 * from end_imgui_frame once frames_seen >= frames_target, and from
 * xemu_metal_shutdown if the run ends before the target is reached.
 * Idempotent; safe to call without an active capture. */
static void stop_metal_capture_if_active(void)
{
    if (!s_capture_active) {
        return;
    }
    MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
    if (mgr != nil && mgr.isCapturing) {
        [mgr stopCapture];
    }
    s_capture_active = false;
    fprintf(stderr,
            "xemu-perf: metal_capture_stopped frames_captured=%llu\n",
            (unsigned long long)atomic_load(&s_capture_frames_seen));
}

/* 2026-05-03 — parse XEMU_METAL_SCREENSHOT_PATH /
 * XEMU_METAL_SCREENSHOT_AT_FRAME / XEMU_METAL_SCREENSHOT_INTERVAL.
 * Called once from xemu_metal_init before the first nextDrawable call
 * because XEMU_METAL_SCREENSHOT_PATH governs s_layer.framebufferOnly
 * (the drawable texture cannot be the source of a blit when
 * framebufferOnly=YES, so we have to flip it before any drawable is
 * acquired).
 *
 * Defaults:
 *   XEMU_METAL_SCREENSHOT_AT_FRAME → 60   (1-indexed against
 *                                          pgraph_mtl_present_total + 1)
 *   XEMU_METAL_SCREENSHOT_INTERVAL → 0    (single shot)
 */
static void parse_screenshot_env(void)
{
    const char *path = getenv("XEMU_METAL_SCREENSHOT_PATH");
    if (path == NULL || path[0] == '\0') {
        return;
    }
    s_screenshot_path = strdup(path);
    if (s_screenshot_path == NULL) {
        return;
    }

    s_screenshot_at_frame = 60;
    const char *at_env = getenv("XEMU_METAL_SCREENSHOT_AT_FRAME");
    if (at_env != NULL && at_env[0] != '\0') {
        char *endp = NULL;
        unsigned long n = strtoul(at_env, &endp, 10);
        if (endp != NULL && *endp == '\0' && n >= 1) {
            s_screenshot_at_frame = (uint64_t)n;
        }
    }

    s_screenshot_interval = 0;
    const char *iv_env = getenv("XEMU_METAL_SCREENSHOT_INTERVAL");
    if (iv_env != NULL && iv_env[0] != '\0') {
        char *endp = NULL;
        unsigned long n = strtoul(iv_env, &endp, 10);
        if (endp != NULL && *endp == '\0') {
            s_screenshot_interval = (uint64_t)n;
        }
    }

    s_screenshot_source = 0;
    const char *src_env = getenv("XEMU_METAL_SCREENSHOT_SOURCE");
    if (src_env != NULL && src_env[0] != '\0') {
        if (strcmp(src_env, "nv2a") == 0 || strcmp(src_env, "1") == 0) {
            s_screenshot_source = 1;
        } else if (strncmp(src_env, "vram:", 5) == 0) {
            /* "vram:0x3628000" — capture from a specific cached
             * SurfaceBinding by vram_addr. Used to inspect back buffer
             * contents without going through CRTC publish. */
            char *endp = NULL;
            unsigned long n = strtoul(src_env + 5, &endp, 0);
            if (endp != NULL && *endp == '\0' && n != 0) {
                s_screenshot_source = 2;
                s_screenshot_vram_addr = (uint32_t)n;
            }
        }
    }

    fprintf(stderr,
            "xemu-perf: metal_screenshot path=%s at_frame=%llu interval=%llu "
            "source=%s\n",
            s_screenshot_path,
            (unsigned long long)s_screenshot_at_frame,
            (unsigned long long)s_screenshot_interval,
            s_screenshot_source == 1 ? "nv2a" : "drawable");
}

/* 2026-05-03 — derive the per-shot filename. For single-shot mode the
 * configured path is used verbatim. For interval mode the suffix
 * `.NNNN.png` is inserted before the trailing `.png` extension (or
 * appended if the path doesn't end in `.png`). The `idx` parameter is
 * 1-indexed; the first interval shot is .0001, the second .0002, etc.
 *
 * Returned buffer is malloc'd; caller frees. Returns NULL on alloc
 * failure or if `s_screenshot_path` is NULL. */
static char *build_screenshot_filename(uint64_t idx)
{
    if (s_screenshot_path == NULL) {
        return NULL;
    }
    if (s_screenshot_interval == 0) {
        return strdup(s_screenshot_path);
    }
    /* Insert ".NNNN" before ".png" if present, else append. */
    size_t plen = strlen(s_screenshot_path);
    const char *suffix_start = NULL;
    if (plen >= 4 &&
        strcasecmp(s_screenshot_path + plen - 4, ".png") == 0) {
        suffix_start = s_screenshot_path + plen - 4;
    }
    size_t pre_len = suffix_start ? (size_t)(suffix_start - s_screenshot_path) : plen;
    /* "<pre>.NNNN<.png-or-empty>\0" — 5 chars for ".NNNN" + 4 for ".png" */
    size_t out_size = pre_len + 5 + 4 + 1;
    char *out = (char *)malloc(out_size);
    if (out == NULL) {
        return NULL;
    }
    if (suffix_start) {
        snprintf(out, out_size, "%.*s.%04llu.png",
                 (int)pre_len, s_screenshot_path,
                 (unsigned long long)idx);
    } else {
        snprintf(out, out_size, "%s.%04llu.png",
                 s_screenshot_path, (unsigned long long)idx);
    }
    return out;
}

/* 2026-05-03 — convert a BGRA8 byte buffer to RGBA8 in place. FPNG
 * expects RGBA (R first in memory); the drawable is BGRA8Unorm_sRGB
 * (B first). The swap is per-pixel; SIMD here would be premature
 * (this runs once-per-screenshot off the renderer thread). */
static void swap_bgra_to_rgba_in_place(uint8_t *p, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t b = p[i * 4 + 0];
        uint8_t r = p[i * 4 + 2];
        p[i * 4 + 0] = r;
        p[i * 4 + 2] = b;
    }
}

/* 2026-05-03 — encode `bytes` (BGRA8 byte order, w*h pixels, stride =
 * w*4) into a PNG file at `filename`. Used from the cmdbuf
 * addCompletedHandler so the PNG write happens after the GPU has
 * finished writing the drawable. Returns true on success. PNG
 * encoding errors are logged and swallowed — never crash the
 * renderer. */
static bool encode_drawable_png(const char *filename,
                                uint8_t *bgra_bytes,
                                uint32_t w,
                                uint32_t h)
{
    if (filename == NULL || bgra_bytes == NULL || w == 0 || h == 0) {
        return false;
    }
    static bool s_fpng_inited = false;
    if (!s_fpng_inited) {
        fpng::fpng_init();
        s_fpng_inited = true;
    }
    swap_bgra_to_rgba_in_place(bgra_bytes, (size_t)w * (size_t)h);
    bool ok = fpng::fpng_encode_image_to_file(filename, bgra_bytes, w, h, 4, 0);
    if (!ok) {
        fprintf(stderr,
                "xemu-metal: fpng_encode_image_to_file failed (path=%s "
                "w=%u h=%u)\n",
                filename, (unsigned)w, (unsigned)h);
    }
    return ok;
}

/* M13 — initialize the per-frame MTLCounterSampleBuffer used for
 * vertex/fragment GPU-stage timing on the present render pass. Gated
 * on supportsCounterSampling: at stage boundary; if unsupported the
 * counter sample buffer is left nil and the per-stage US counters
 * stay at zero for the run. Survives device-loss / re-init by being
 * called from xemu_metal_init each time the device comes up.
 *
 * Capacity 4 = (vertex_start, vertex_end, fragment_start, fragment_end)
 * for a single render pass per frame. The HUD/present pass is the
 * only render pass we sample; if a future slice instruments the NV2A
 * draw passes too, capacity grows accordingly. */
static void build_counter_sample_buffer_if_supported(void)
{
    s_counter_sampling_supported = false;
    s_counter_sample_buffer = nil;
    s_counter_sample_buffer_capacity = 0;

    if (s_device == nil) {
        return;
    }

    /* `supportsCounterSampling:` is macOS 11+. Apple Silicon (Apple7+)
     * always advertises StageBoundary; the call is wrapped in a
     * respondsToSelector: just so the build links cleanly against
     * older SDKs / OS targets. */
    if (![s_device respondsToSelector:@selector(supportsCounterSampling:)]) {
        return;
    }
    if (![s_device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        fprintf(stderr,
                "xemu-perf: metal_counter_sampling unsupported on this "
                "device (StageBoundary); per-stage GPU times disabled\n");
        return;
    }

    /* Locate the timestamp counter set. Apple Silicon publishes it via
     * device.counterSets; the unique counter ID is "timestamp" on
     * macOS 11+ and matches MTLCommonCounterTimestamp. */
    id<MTLCounterSet> ts_set = nil;
    for (id<MTLCounterSet> cs in s_device.counterSets) {
        if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) {
            ts_set = cs;
            break;
        }
    }
    if (ts_set == nil) {
        fprintf(stderr,
                "xemu-perf: metal_counter_sampling timestamp counter "
                "set not advertised; per-stage GPU times disabled\n");
        return;
    }

    MTLCounterSampleBufferDescriptor *cd =
        [[MTLCounterSampleBufferDescriptor alloc] init];
    cd.counterSet = ts_set;
    cd.label = @"xemu.metal.counters.present";
    cd.storageMode = MTLStorageModeShared;
    cd.sampleCount = 4;  /* vertex_start, vertex_end, fragment_start, fragment_end */

    NSError *err = nil;
    s_counter_sample_buffer =
        [s_device newCounterSampleBufferWithDescriptor:cd error:&err];
    if (s_counter_sample_buffer == nil) {
        fprintf(stderr,
                "xemu-perf: metal_counter_sampling buffer alloc failed: "
                "%s; per-stage GPU times disabled\n",
                [[err localizedDescription] UTF8String] ?: "(unknown)");
        return;
    }
    s_counter_sample_buffer_capacity = 4;
    s_counter_sampling_supported = true;

    /* Cache the GPU timebase so the post-resolve callback can convert
     * MTLCounterResultTimestamp ticks (raw GPU timestamps) to
     * nanoseconds. The Apple7+ docs put a single device-wide
     * timestamp clock at host nanosecond cadence after this call:
     *   gpu_ns = (gpu_ticks - gpu_ref) * (cpu_ref_ns / cpu_ticks)
     * but on Apple Silicon the timestamp counter is already in host
     * nanoseconds (mach_absolute_time-equivalent), so the simpler
     * delta-in-ticks → ns path works without timebase conversion. */
    s_gpu_timebase_seconds_per_unit = 1.0e-9;

    fprintf(stderr,
            "xemu-perf: metal_counter_sampling enabled "
            "(buffer_capacity=%lu storage=Shared)\n",
            (unsigned long)s_counter_sample_buffer_capacity);
}

/* M14 — XEMU_METAL_VALIDATION={0,1} opt-in for development. When set,
 * promotes MTL_DEBUG_LAYER=1 into the process env BEFORE the first
 * Metal device or layer call. Apple's Metal validation layer is
 * activated by reading MTL_DEBUG_LAYER at MTLCreateSystemDefaultDevice
 * time; later setenv has no effect (the framework caches the decision
 * once a device exists). Production builds leave the variable unset,
 * matching M14's "MTL_DEBUG_LAYER=0 in shipped builds" rule.
 *
 * If the user already has MTL_DEBUG_LAYER set in their env, we leave
 * it alone (overwrite=0 on the second setenv); XEMU_METAL_VALIDATION=1
 * only promotes when the user did not pin a value themselves. This
 * makes XEMU_METAL_VALIDATION a convenience knob without papering over
 * an explicit user setting.
 *
 * Logged once at the call site so the post-init startup banner can
 * note whether validation is active. */
static bool s_metal_validation_requested;
static bool s_metal_validation_promoted;

static void xemu_metal_apply_validation_env(void)
{
    const char *v = getenv("XEMU_METAL_VALIDATION");
    s_metal_validation_requested =
        (v != NULL && v[0] != '\0' && v[0] != '0');
    if (!s_metal_validation_requested) {
        return;
    }
    /* Only promote MTL_DEBUG_LAYER if the user has not pinned a value
     * already. setenv with overwrite=0 returns 0 either way; we test
     * the live env to know whether we actually changed anything. */
    const char *prior = getenv("MTL_DEBUG_LAYER");
    if (prior == NULL) {
        setenv("MTL_DEBUG_LAYER", "1", 0);
        s_metal_validation_promoted = true;
    } else {
        s_metal_validation_promoted = false;
    }
}

bool xemu_metal_init(SDL_Window *window)
{
    if (s_active) {
        fprintf(stderr, "xemu_metal_init: already initialized\n");
        return true;
    }

    /* M14 — must run before MTLCreateSystemDefaultDevice(). Apple's
     * Metal framework reads MTL_DEBUG_LAYER once, at first device
     * creation; later setenv calls have no effect on validation. */
    xemu_metal_apply_validation_env();

    s_metal_view = SDL_Metal_CreateView(window);
    if (s_metal_view == NULL) {
        fprintf(stderr, "xemu_metal_init: SDL_Metal_CreateView failed: %s\n",
                SDL_GetError());
        return false;
    }

    /* SDL_Metal_GetLayer returns a CAMetalLayer*; SDL3 typed it as
     * void* to keep the C API agnostic. */
    s_layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(s_metal_view);
    if (s_layer == nil) {
        fprintf(stderr, "xemu_metal_init: SDL_Metal_GetLayer returned nil\n");
        SDL_Metal_DestroyView(s_metal_view);
        s_metal_view = NULL;
        return false;
    }

    s_device = MTLCreateSystemDefaultDevice();
    if (s_device == nil) {
        fprintf(stderr, "xemu_metal_init: MTLCreateSystemDefaultDevice "
                        "returned nil; this Mac does not have a usable "
                        "Metal device\n");
        SDL_Metal_DestroyView(s_metal_view);
        s_metal_view = NULL;
        s_layer = nil;
        return false;
    }

    /* 2026-05-03 — parse XEMU_METAL_SCREENSHOT_PATH BEFORE setting
     * s_layer.framebufferOnly. The drawable texture cannot be the
     * source of a blit when framebufferOnly=YES; the screenshot path
     * needs to copy from the drawable into a shared MTLBuffer, so the
     * env var has to flip the layer to NO at init. Steady-state perf
     * is preserved when XEMU_METAL_SCREENSHOT_PATH is unset (the
     * default Apple Silicon path keeps display compression on). */
    parse_screenshot_env();

    /* Recommended Apple Silicon defaults from
     * docs/apple-silicon/metal-api-reference.md "Recommended Apple
     * Silicon defaults" quick-reference table. */
    s_layer.device                = s_device;
    s_layer.pixelFormat           = MTLPixelFormatBGRA8Unorm_sRGB;
    s_layer.framebufferOnly       = (s_screenshot_path == NULL) ? YES : NO;
    s_layer.maximumDrawableCount  = 3;
    s_layer.displaySyncEnabled    = YES;

    /* maxCommandBufferCount sized to leave headroom for: HUD frame +
     * pre-savevm + future NV2A workload + small slack. The default 64
     * is fine for a developer build but 8 keeps the in-flight pool
     * tight and matches the metal-renderer-plan.md M1 spec. */
    s_queue = [s_device newCommandQueueWithMaxCommandBufferCount:8];
    if (s_queue == nil) {
        fprintf(stderr, "xemu_metal_init: newCommandQueue failed\n");
        s_device = nil;
        SDL_Metal_DestroyView(s_metal_view);
        s_metal_view = NULL;
        s_layer = nil;
        return false;
    }
    s_queue.label = @"xemu.metal.main_queue";

    s_active = true;

    /* M10 — frame-pacing init. */
    mach_timebase_info(&s_timebase);
    s_seconds_per_mach_unit =
        (double)s_timebase.numer / (double)s_timebase.denom / 1.0e9;
    s_last_present_target_ns = 0;
    s_last_present_target_mach = 0;
    {
        const char *legacy = getenv("XEMU_METAL_FORCE_LEGACY_PRESENT");
        s_force_legacy_present =
            (legacy != NULL && legacy[0] != '\0' && legacy[0] != '0');
    }

    /* M12 — MetalFX spatial scaler init. Latched once at boot so the
     * present path can branch without re-reading the env on every
     * frame. The scaler instance + output texture are built lazily on
     * first present (we don't know input/output dimensions yet). */
    {
        uint32_t requested = 0;
        uint32_t configured = parse_metal_fx_scale_env(&requested);
        s_metal_fx_scale_requested = requested;
        s_metal_fx_enabled = (configured >= 2);
        fprintf(stderr,
                "xemu-perf: metal_fx_scale=%u source=XEMU_METAL_FX_SCALE "
                "requested=%u configured=%u enabled=%d\n",
                configured, requested, configured,
                (int)s_metal_fx_enabled);
    }

    fprintf(stderr, "xemu-metal: device=%s headless=%d low-power=%d "
                    "max-buf-len=%lu force-legacy-present=%d\n",
            [[s_device name] UTF8String],
            (int)[s_device isHeadless],
            (int)[s_device isLowPower],
            (unsigned long)[s_device maxBufferLength],
            (int)s_force_legacy_present);

    /* M14 — surface XEMU_METAL_VALIDATION state in the startup banner.
     * "active" follows MTL_DEBUG_LAYER as observed at this point: if
     * the user pinned MTL_DEBUG_LAYER themselves it will read =1, even
     * though XEMU_METAL_VALIDATION may be 0; if XEMU_METAL_VALIDATION=1
     * promoted it, both read =1. The promoted bit exposes "we set this
     * for you" so the user is not surprised by validation logs in a
     * shell that did not export MTL_DEBUG_LAYER directly. */
    {
        const char *live = getenv("MTL_DEBUG_LAYER");
        bool active = (live != NULL && live[0] != '\0' && live[0] != '0');
        fprintf(stderr,
                "xemu-perf: metal_validation requested=%d promoted=%d "
                "mtl_debug_layer_active=%d\n",
                (int)s_metal_validation_requested,
                (int)s_metal_validation_promoted,
                (int)active);
    }

    /* M13 — counter sample buffer + programmatic capture. Order matters:
     * counter sampling depends only on the device, capture should
     * start as early as possible so init-time work shows up in the
     * .gputrace if the user wants it. */
    build_counter_sample_buffer_if_supported();
    start_metal_capture_if_requested();

    /* Slice M5 — early-fire shader validation harness if requested.
     *
     * Trigger: env var XEMU_METAL_SHADER_VALIDATE=1 (advisory) or
     * "strict" / "2" (abort on failure). The harness is wired into
     * pgraph_mtl_init for the normal user-facing path, but that
     * requires the user to actually boot an Xbox machine. For
     * automation (run-validation.sh) we run it here too, immediately
     * after the device is up — no machine boot required. The
     * pgraph_mtl_shader_validate_run symbol lives in the per-target
     * libqemu-i386-softmmu.a and is referenced as a weak symbol so
     * non-x86-target builds still link. The early invocation is a
     * no-op when the env var is unset.
     *
     * If XEMU_METAL_SHADER_VALIDATE_AND_EXIT=1 is also set, we exit
     * the process after the harness reports — useful for CI-style
     * validation that doesn't need to bring up the GUI or any
     * machine. */
    if (xemu_metal_run_validation_if_requested != NULL) {
        xemu_metal_run_validation_if_requested();
    }

    return true;
}

bool xemu_metal_imgui_init(SDL_Window *window)
{
    if (!s_active) {
        fprintf(stderr, "xemu_metal_imgui_init: device not initialized "
                        "(call xemu_metal_init first)\n");
        return false;
    }

    if (!ImGui_ImplSDL3_InitForMetal(window)) {
        fprintf(stderr, "xemu_metal_imgui_init: "
                        "ImGui_ImplSDL3_InitForMetal failed\n");
        return false;
    }

    if (!ImGui_ImplMetal_Init(s_device)) {
        fprintf(stderr, "xemu_metal_imgui_init: "
                        "ImGui_ImplMetal_Init failed\n");
        ImGui_ImplSDL3_Shutdown();
        return false;
    }

    return true;
}

static void build_present_pipeline_if_needed(void)
{
    if (s_present_pipeline != nil) {
        return;
    }
    if (s_device == nil) {
        return;
    }

    NSError *err = nil;
    id<MTLLibrary> lib =
        [s_device newLibraryWithSource:k_present_msl options:nil error:&err];
    if (lib == nil) {
        fprintf(stderr,
                "xemu-metal: present-pipeline MSL compile failed: %s\n",
                [[err localizedDescription] UTF8String] ?: "(unknown)");
        return;
    }
    id<MTLFunction> vs = [lib newFunctionWithName:@"xemu_present_vs"];
    id<MTLFunction> fs = [lib newFunctionWithName:@"xemu_present_fs"];

    MTLRenderPipelineDescriptor *desc =
        [[MTLRenderPipelineDescriptor alloc] init];
    desc.label = @"xemu.metal.present";
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    desc.colorAttachments[0].pixelFormat = s_layer.pixelFormat;
    desc.colorAttachments[0].blendingEnabled = NO;

    s_present_pipeline =
        [s_device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (s_present_pipeline == nil) {
        fprintf(stderr,
                "xemu-metal: present-pipeline build failed: %s\n",
                [[err localizedDescription] UTF8String] ?: "(unknown)");
        return;
    }

    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.label = @"xemu.metal.present_sampler";
    s_present_sampler = [s_device newSamplerStateWithDescriptor:sd];
}

void xemu_metal_shutdown(void)
{
    if (!s_active) {
        return;
    }

    ImGui_ImplMetal_Shutdown();
    /* SDL3 ImGui platform backend is shared between GL and Metal paths;
     * the C++ HUD cleanup (xemu_hud_cleanup) calls ImGui_ImplSDL3_Shutdown.
     * On the Metal path we must not double-call it here. The order:
     *   xemu_hud_cleanup() → calls renderer-specific shutdown (this fn)
     *   xemu_hud_cleanup() → then calls ImGui_ImplSDL3_Shutdown.
     * So this function only tears down the Metal-specific bits. */

    /* M13 — finalize any active capture before the device drops; once
     * the device is nil, MTLCaptureManager has nothing to write into. */
    stop_metal_capture_if_active();
    s_counter_sample_buffer = nil;
    s_counter_sample_buffer_capacity = 0;
    s_counter_sampling_supported = false;

    s_present_pipeline = nil;
    s_present_sampler = nil;

    /* M12 — release MetalFX state. The output texture is in the
     * unified-memory pool and the scaler holds private compute
     * pipelines, both of which need to be released before the device
     * goes away. */
    s_metal_fx_scaler = nil;
    s_metal_fx_output_tex = nil;
    s_metal_fx_input_w = 0;
    s_metal_fx_input_h = 0;
    s_metal_fx_output_w = 0;
    s_metal_fx_output_h = 0;
    s_metal_fx_input_format = MTLPixelFormatInvalid;

    s_queue = nil;
    s_device = nil;
    s_layer = nil;
    if (s_metal_view) {
        SDL_Metal_DestroyView(s_metal_view);
        s_metal_view = NULL;
    }

    /* 2026-05-03 — release screenshot path. Any in-flight completion
     * handlers retain their per-shot fname copies, so freeing the
     * top-level path here is safe. */
    if (s_screenshot_path != NULL) {
        free(s_screenshot_path);
        s_screenshot_path = NULL;
    }

    s_active = false;
}

void *xemu_metal_get_device(void)
{
    return (__bridge void *)s_device;
}

void *xemu_metal_get_layer(void)
{
    return (__bridge void *)s_layer;
}

bool xemu_metal_is_active(void)
{
    return s_active;
}

void xemu_metal_create_fonts_texture(void)
{
    if (!s_active) {
        return;
    }
    /* Following the imgui-metal pattern: destroy then re-create so a
     * font rebuild (viewport-scale change) gets a fresh atlas. */
    ImGui_ImplMetal_DestroyFontsTexture();
    ImGui_ImplMetal_CreateFontsTexture(s_device);
}

bool xemu_metal_begin_imgui_frame(void)
{
    if (!s_active) {
        return false;
    }

    /* The CAMetalLayer's drawableSize is auto-updated by SDL_Metal_CreateView
     * when the host view resizes. We don't override it here; if M2
     * finds tearing or HiDPI scaling issues we'll revisit. */

    s_current_drawable = [s_layer nextDrawable];
    if (s_current_drawable == nil) {
        /* Display server is busy; skip the frame cleanly. */
        atomic_fetch_add(&s_drawable_acquire_fails, 1);
        return false;
    }

    s_current_pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    s_current_pass_desc.colorAttachments[0].texture     = s_current_drawable.texture;
    s_current_pass_desc.colorAttachments[0].loadAction  = MTLLoadActionClear;
    s_current_pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    s_current_pass_desc.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);

    s_current_cmd = [s_queue commandBuffer];
    s_current_cmd.label = @"xemu.metal.hud_cmd";

    /* ImGui_ImplMetal_NewFrame must be called BEFORE ImGui::NewFrame
     * so the next frame's framebuffer descriptor is captured. The
     * imgui_impl_metal.mm comment confirms this ordering. */
    ImGui_ImplMetal_NewFrame(s_current_pass_desc);
    ImGui_ImplSDL3_NewFrame();

    return true;
}

void xemu_metal_end_imgui_frame(void)
{
    if (!s_active || s_current_drawable == nil || s_current_cmd == nil) {
        /* begin returned false (skipped frame). Render data was not
         * built for this frame; just bail. */
        s_current_drawable = nil;
        s_current_pass_desc = nil;
        s_current_cmd = nil;
        return;
    }

    /* Slice M2: composite the NV2A framebuffer texture (if any) into
     * the drawable. The surface manager publishes the current color RT
     * via a side-channel because PGRAPHRenderer.ops.get_framebuffer_surface
     * returns int (incompatible with id<MTLTexture>). When the NV2A
     * has not yet produced a surface (early boot, before any
     * clear_surface call) the side-channel returns NULL and we leave
     * the load-action clear-to-black background visible — the HUD then
     * draws on top.
     *
     * Slice M12 (MetalFX spatial scaler): when XEMU_METAL_FX_SCALE >= 2
     * AND the drawable is larger than the input, run the NV2A FB
     * texture through MTLFXSpatialScaler into a private intermediate,
     * then bind that intermediate to the present pipeline instead.
     * The scaler `encodeToCommandBuffer:` must happen before the
     * render encoder is opened — it is a discrete pass operation. */
    id<MTLTexture> present_input_tex = nil;
    bool metal_fx_encoded_this_frame = false;
    void *fb_tex_handle = pgraph_mtl_get_framebuffer_metal_texture();
    if (fb_tex_handle != NULL) {
        id<MTLTexture> fb_tex = (__bridge id<MTLTexture>)fb_tex_handle;
        present_input_tex = fb_tex;

        if (s_metal_fx_enabled) {
            NSUInteger drawable_w = s_current_drawable.texture.width;
            NSUInteger drawable_h = s_current_drawable.texture.height;
            NSUInteger fb_w = fb_tex.width;
            NSUInteger fb_h = fb_tex.height;
            MTLPixelFormat fb_fmt = fb_tex.pixelFormat;

            if (build_metal_fx_scaler_if_needed(fb_w, fb_h, fb_fmt,
                                                drawable_w, drawable_h)) {
                /* Encode the spatial-scaler pass. Closes and submits
                 * its own internal compute pass on the supplied
                 * command buffer; safe to interleave with subsequent
                 * render encoders on the same cmdbuf. */
                uint64_t fx_t0 = mach_now_ns();
                s_metal_fx_scaler.colorTexture  = fb_tex;
                s_metal_fx_scaler.outputTexture = s_metal_fx_output_tex;
                [s_metal_fx_scaler encodeToCommandBuffer:s_current_cmd];
                /* CPU-side wallclock kept from M12: the encode call
                 * itself is cheap, so this under-reports GPU-side
                 * scaler cost. M13 augments this with
                 * METAL_FX_SPATIAL_GPU_US_TOTAL via the cmdbuf-level
                 * GPUStartTime/GPUEndTime accumulated in the
                 * addCompletedHandler below — that captures the entire
                 * present cmdbuf's GPU time, including the scaler
                 * pass, the present pipeline, and the HUD encoder. */
                uint64_t fx_t1 = mach_now_ns();
                atomic_fetch_add(&s_metal_fx_spatial_us_total,
                                 (fx_t1 - fx_t0) / 1000ull);
                atomic_fetch_add(&s_metal_fx_spatial_presents, 1);
                present_input_tex = s_metal_fx_output_tex;
                metal_fx_encoded_this_frame = true;
            }
        }
    }

    /* M13 — attach the counter sample buffer to the present render pass
     * descriptor so vertex/fragment stage boundaries are sampled. The
     * sample indices are arranged as:
     *   [0] vertex stage start
     *   [1] vertex stage end
     *   [2] fragment stage start
     *   [3] fragment stage end
     * Resolved in the addCompletedHandler below. The attachments
     * structure is per-render-pass-descriptor and only takes effect
     * for the encoder built from this descriptor; if a future slice
     * adds a second render pass we either need a second sample buffer
     * or to re-use the same buffer with non-overlapping indices. */
    if (s_counter_sampling_supported && s_counter_sample_buffer != nil) {
        MTLRenderPassSampleBufferAttachmentDescriptor *att =
            s_current_pass_desc.sampleBufferAttachments[0];
        att.sampleBuffer = s_counter_sample_buffer;
        att.startOfVertexSampleIndex   = 0;
        att.endOfVertexSampleIndex     = 1;
        att.startOfFragmentSampleIndex = 2;
        att.endOfFragmentSampleIndex   = 3;
    }

    s_current_enc = [s_current_cmd renderCommandEncoderWithDescriptor:s_current_pass_desc];
    s_current_enc.label = @"xemu.metal.hud_enc";

    if (present_input_tex != nil) {
        build_present_pipeline_if_needed();
        if (s_present_pipeline != nil && s_present_sampler != nil) {
            [s_current_enc setRenderPipelineState:s_present_pipeline];
            [s_current_enc setFragmentTexture:present_input_tex atIndex:0];
            [s_current_enc setFragmentSamplerState:s_present_sampler atIndex:0];
            [s_current_enc drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0
                              vertexCount:3];
        }
    }
    if (fb_tex_handle != NULL) {
        pgraph_mtl_release_framebuffer_metal_texture();
    }

    /* C++ HUD has already called ImGui::Render(); we encode its draw
     * data into the active render encoder. */
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),
                                   s_current_cmd,
                                   s_current_enc);

    [s_current_enc endEncoding];

    /* 2026-05-03 — programmatic drawable screenshot. Fires on the
     * frame whose 1-indexed number matches XEMU_METAL_SCREENSHOT_AT_FRAME
     * (or every N frames after that when XEMU_METAL_SCREENSHOT_INTERVAL
     * is set). The blit copies the drawable texture into a host-shared
     * MTLBuffer; the cmdbuf's completion handler then reads the
     * buffer's bytes, swaps BGRA→RGBA, and writes the PNG via FPNG.
     * The capture point is AFTER the HUD encoder closed and BEFORE
     * presentDrawable:, so the captured pixels match what the user
     * sees on screen.
     *
     * Frame counting: bump s_end_frames once per call (the renderer's
     * view of frames submitted). We deliberately DO NOT use
     * s_presents_total here — that counter is bumped only inside
     * addPresentedHandler:, which does not fire when the macOS
     * Screen-Recording dialog occludes the xemu window during a
     * benchmark (METAL_PRESENTS=0; see handoff.md). The submit-time
     * counter ticks deterministically regardless of presentation
     * status, which is the trigger semantic we want here. */
    uint64_t cur_end_frame = atomic_fetch_add(&s_end_frames, 1) + 1;
    if (s_screenshot_path != NULL) {
        uint64_t cur_frame = cur_end_frame;
        bool should_fire = false;
        if (s_screenshot_interval == 0) {
            should_fire = (cur_frame == s_screenshot_at_frame);
        } else if (cur_frame >= s_screenshot_at_frame) {
            uint64_t since = cur_frame - s_screenshot_at_frame;
            should_fire = (since % s_screenshot_interval) == 0;
        }
        if (should_fire) {
            /* 2026-05-03 diagnostic — XEMU_METAL_SCREENSHOT_SOURCE=nv2a
             * captures the NV2A framebuffer texture pre-present (before
             * compositing into the drawable). When the NV2A side has not
             * yet produced a frame, present_input_tex is nil and we fall
             * back to the drawable so we still get a screenshot. */
            id<MTLTexture> drawable_tex = s_current_drawable.texture;
            id<MTLTexture> source_tex = drawable_tex;
            if (s_screenshot_source == 1 && present_input_tex != nil) {
                source_tex = present_input_tex;
            } else if (s_screenshot_source == 2) {
                void *t = pgraph_mtl_surface_get_metal_texture_at(
                    s_screenshot_vram_addr);
                if (t != NULL) {
                    source_tex = (__bridge id<MTLTexture>)t;
                }
            }
            NSUInteger w = source_tex.width;
            NSUInteger h = source_tex.height;
            size_t bytes_per_row   = (size_t)w * 4;
            size_t total_bytes     = bytes_per_row * (size_t)h;
            id<MTLBuffer> readback =
                [s_device newBufferWithLength:total_bytes
                                       options:MTLResourceStorageModeShared];
            if (readback != nil) {
                readback.label = @"xemu.metal.screenshot_readback";
                id<MTLBlitCommandEncoder> blit =
                    [s_current_cmd blitCommandEncoder];
                blit.label = @"xemu.metal.screenshot_blit";
                [blit copyFromTexture:source_tex
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(w, h, 1)
                             toBuffer:readback
                    destinationOffset:0
               destinationBytesPerRow:bytes_per_row
             destinationBytesPerImage:total_bytes];
                /* synchronizeResource: is a no-op for Shared on Apple
                 * Silicon UMA; included for correctness on hypothetical
                 * Discrete-GPU paths. */
                [blit endEncoding];

                uint64_t shot_idx = atomic_fetch_add(&s_screenshots_done, 1) + 1;
                char *fname = build_screenshot_filename(shot_idx);
                /* The capturing block holds `readback` alive (ARC
                 * retains via __strong) until the handler runs. The
                 * filename buffer is malloc'd; we free it inside the
                 * block. fpng can fail; log + continue. */
                uint32_t shot_w = (uint32_t)w;
                uint32_t shot_h = (uint32_t)h;
                [s_current_cmd addCompletedHandler:^(id<MTLCommandBuffer> /*cb*/) {
                    if (fname == NULL) {
                        return;
                    }
                    /* readback is host-Shared; contents() is the raw
                     * BGRA8 byte array (stride = w*4). */
                    uint8_t *bytes = (uint8_t *)[readback contents];
                    if (bytes != NULL) {
                        bool ok = encode_drawable_png(fname, bytes,
                                                      shot_w, shot_h);
                        if (ok) {
                            atomic_fetch_add(&s_screenshots_taken, 1);
                            fprintf(stderr,
                                    "xemu-perf: metal_screenshot_written "
                                    "path=%s w=%u h=%u\n",
                                    fname,
                                    (unsigned)shot_w,
                                    (unsigned)shot_h);
                        }
                    }
                    free(fname);
                }];
            } else {
                fprintf(stderr,
                        "xemu-metal: screenshot readback buffer alloc "
                        "failed (size=%zu); skipping shot frame=%llu\n",
                        total_bytes,
                        (unsigned long long)cur_frame);
            }
        }
    }

    /* M10 — frame pacing. Default path: presentDrawable:atTime: with
     * an explicit deadline computed from vblank_interval_ns and the
     * previous target. Mirrors DuckStation metal_device.mm:2577-2601.
     *
     * The first frame uses now+vblank_interval_ns as the seed; each
     * subsequent frame extends the previous target by exactly one
     * vblank_interval_ns so guest-frame cadence stays locked to the
     * intended emulation rate. If the renderer falls behind by more
     * than one frame (e.g. a stutter), reset the seed to avoid
     * rapid-fire catch-up — same idea as the GL vblank thread's
     * "fallen behind by more than one frame" reset in
     * vblank_timer_thread.
     *
     * XEMU_METAL_FORCE_LEGACY_PRESENT=1 falls back to the simpler
     * presentDrawable: (no atTime:) so an A/B comparison can quantify
     * the M10 gain vs the M2 baseline. */
    uint64_t now_ns = mach_now_ns();
    uint64_t target_ns;
    uint64_t target_mach;
    if (s_last_present_target_ns == 0 ||
        now_ns > s_last_present_target_ns + 2 * vblank_interval_ns) {
        /* First frame, or we've fallen ≥ 2 vblank periods behind. Seed
         * the deadline from "now". */
        target_ns = now_ns + vblank_interval_ns;
    } else {
        target_ns = s_last_present_target_ns + vblank_interval_ns;
        if (target_ns < now_ns) {
            target_ns = now_ns;
        }
    }
    target_mach = ns_to_mach(target_ns);
    s_last_present_target_ns = target_ns;
    s_last_present_target_mach = target_mach;

    /* presentedHandler runs on a Metal callback queue. We capture the
     * target time so we can compute jitter = |actual - target|. The
     * |drawable| param's presentedTime is in mach-base seconds. */
    double target_seconds = mach_to_seconds(target_mach);
    [s_current_drawable addPresentedHandler:^(id<MTLDrawable> drawable) {
        double actual = [drawable presentedTime];
        if (actual <= 0.0) {
            return; /* not yet presented or unavailable */
        }
        double delta = actual - target_seconds;
        if (delta < 0) {
            delta = -delta;
        }
        uint64_t delta_us = (uint64_t)(delta * 1.0e6);
        atomic_fetch_add(&s_present_jitter_us_total, delta_us);
        atomic_max_u64(&s_present_jitter_us_max, delta_us);
        atomic_fetch_add(&s_presents_total, 1);
    }];

    /* M13 — addCompletedHandler block runs on a Metal callback queue
     * once the GPU has finished the cmdbuf. Three jobs:
     *   1. Read cmdbuf.GPUStartTime / GPUEndTime (host seconds since
     *      boot) for an upper-bound on per-frame GPU cost. When the
     *      MetalFX path encoded this frame, that delta also bounds
     *      the scaler cost.
     *   2. Resolve the counter sample buffer if it was attached and
     *      derive vertex/fragment stage durations from the four
     *      timestamps.
     *   3. Bump the capture-frames counter and stop capture once the
     *      target is reached.
     * The block captures `metal_fx_encoded_this_frame` and the
     * sample-buffer pointer by value via __block; capturing them
     * directly is fine because they're stack/static and the block
     * runs after the call returns but the captures are by-value for
     * scalars and weak retained for the buffer. */
    bool fx_encoded_capture = metal_fx_encoded_this_frame;
    bool counters_attached_capture =
        (s_counter_sampling_supported && s_counter_sample_buffer != nil);
    id<MTLCounterSampleBuffer> sample_buf_capture = s_counter_sample_buffer;
    NSUInteger sample_capacity_capture = s_counter_sample_buffer_capacity;
    [s_current_cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        /* Job 1 — cmdbuf-level GPU time. GPUStartTime/EndTime are
         * host-mach seconds; the delta in microseconds is a
         * reasonable proxy for "this frame's GPU work" until the
         * NV2A draw passes get instrumented in a future slice. */
        double gpu_start = [cb GPUStartTime];
        double gpu_end   = [cb GPUEndTime];
        if (gpu_end > gpu_start && gpu_start > 0.0) {
            uint64_t gpu_delta_us =
                (uint64_t)((gpu_end - gpu_start) * 1.0e6);
            atomic_fetch_add(&s_metal_present_gpu_us_total, gpu_delta_us);
            atomic_fetch_add(&s_metal_present_gpu_frames, 1);
            if (fx_encoded_capture) {
                atomic_fetch_add(&s_metal_fx_spatial_gpu_us_total,
                                 gpu_delta_us);
            }
        }

        /* Job 2 — resolve counter sample buffer (Apple Silicon stage-
         * boundary timestamps in nanoseconds). resolveCounterRange:
         * returns NSData containing MTLCounterResultTimestamp structs
         * (one per sample index). Mismatched sample-buffer attach (a
         * future encoder skipped sampling, leaving 0s) is detected by
         * the timestamp == 0 check below. */
        if (counters_attached_capture && sample_buf_capture != nil &&
            sample_capacity_capture >= 4) {
            NSRange range = NSMakeRange(0, sample_capacity_capture);
            NSData *resolved = [sample_buf_capture resolveCounterRange:range];
            if (resolved != nil &&
                resolved.length >= 4 * sizeof(MTLCounterResultTimestamp)) {
                const MTLCounterResultTimestamp *ts =
                    (const MTLCounterResultTimestamp *)resolved.bytes;
                /* Apple Silicon timestamps are nanoseconds. A zero
                 * timestamp means the GPU did not actually sample
                 * (e.g. the render pass was skipped); guard against
                 * negative-looking deltas from u64 wrap. */
                uint64_t v0 = ts[0].timestamp;
                uint64_t v1 = ts[1].timestamp;
                uint64_t v2 = ts[2].timestamp;
                uint64_t v3 = ts[3].timestamp;
                if (v0 != 0 && v1 > v0) {
                    uint64_t vert_us = (v1 - v0) / 1000ull;
                    atomic_fetch_add(&s_metal_vertex_us_total, vert_us);
                }
                if (v2 != 0 && v3 > v2) {
                    uint64_t frag_us = (v3 - v2) / 1000ull;
                    atomic_fetch_add(&s_metal_fragment_us_total, frag_us);
                }
            }
        }

        /* Job 3 — capture frame counting. Once we hit the target,
         * stop capture from inside the callback. stopCapture is
         * thread-safe per Apple's MTLCaptureManager docs. The
         * boolean s_capture_active guards against repeat stops if
         * multiple completed-handlers fire close together. */
        if (s_capture_enabled && s_capture_frames_target > 0) {
            uint64_t seen = atomic_fetch_add(&s_capture_frames_seen, 1) + 1;
            if (seen >= s_capture_frames_target && s_capture_active) {
                stop_metal_capture_if_active();
            }
        }
    }];

    if (s_force_legacy_present) {
        [s_current_cmd presentDrawable:s_current_drawable];
    } else {
        [s_current_cmd presentDrawable:s_current_drawable
                                atTime:target_seconds];
    }
    [s_current_cmd commit];

    s_current_enc = nil;
    s_current_cmd = nil;
    s_current_pass_desc = nil;
    s_current_drawable = nil;
}

/* Forward declarations of C / C-callable HUD entry points. xemu.c and
 * xui/xemu-hud.h declare these with extern "C" guards already, but we
 * cannot include xemu-hud.h here because it transitively includes
 * <epoxy/gl.h> which conflicts with ObjC++ on this path. Re-declare
 * manually with C linkage. */
extern "C" {
    void xemu_main_loop_lock(void);
    void xemu_main_loop_unlock(void);
    void xemu_hud_update(void);
    void xemu_hud_render(void);
}

void xemu_metal_render_frame(void)
{
    /* This is the renderer-frame entry point analogous to
     * gl_render_frame(). xemu_hud_update() is the dispatch point that
     * calls xemu_metal_begin_imgui_frame() (via the renderer-pick
     * branch in main.cc); xemu_hud_render() ends with
     * xemu_metal_end_imgui_frame() on the Metal path. So this
     * function only orchestrates the lock/unlock dance — the
     * actual Metal command-buffer work is in begin/end. */

    xemu_main_loop_lock();
    xemu_hud_update();
    xemu_main_loop_unlock();

    xemu_hud_render();
}
