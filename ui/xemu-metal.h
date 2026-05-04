/*
 * xemu Metal renderer host integration (Apple Silicon performance fork).
 *
 * Slice M1 — window + device + ImGui-Metal HUD.
 *
 * Owns the process-wide Metal device, command queue, SDL Metal view, and
 * CAMetalLayer. The HUD compositor lives here for the Metal path; NV2A
 * draws are still no-ops at this slice (the renderer ops live in
 * hw/xbox/nv2a/pgraph/mtl/renderer.c and are pure no-ops until M2).
 *
 * This header is C-callable so ui/xemu.c (plain C) can use it. The .m
 * implementation file does NOT include hw/xbox/nv2a/nv2a_int.h — it gets
 * target-agnostic types only and communicates with renderer.c through
 * opaque handles, mirroring the boundary documented in
 * docs/apple-silicon/metal-renderer-plan.md slice M1.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef XEMU_METAL_H
#define XEMU_METAL_H

#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Phase 1: Initialize the host Metal stack:
 *  - SDL_Metal_CreateView on the supplied window.
 *  - SDL_Metal_GetLayer to obtain the CAMetalLayer.
 *  - MTLCreateSystemDefaultDevice + a single MTLCommandQueue.
 *  - Configure the layer (BGRA8Unorm_sRGB, framebufferOnly,
 *    maximumDrawableCount=3, displaySyncEnabled=YES).
 *
 * Does NOT touch ImGui (the ImGui context is not created yet at this
 * point). The ImGui-Metal backend init happens in xemu_metal_imgui_init()
 * after ImGui::CreateContext().
 *
 * Returns true on success. On failure xemu prints a message and the
 * caller should treat the renderer choice as fatal — the GL fallback
 * is not viable from inside a Metal-already-selected window because
 * the SDL window was created with SDL_WINDOW_METAL.
 */
bool xemu_metal_init(SDL_Window *window);

/*
 * Phase 2: Bring up the ImGui Metal backends. Called from xemu_hud_init()
 * after ImGui::CreateContext(). Initializes
 * ImGui_ImplSDL3_InitForMetal(window) and ImGui_ImplMetal_Init(device).
 * Returns true on success.
 */
bool xemu_metal_imgui_init(SDL_Window *window);

/*
 * Tear down the ImGui Metal backend, the command queue, the device,
 * the layer, and the SDL Metal view. Safe to call if init failed.
 */
void xemu_metal_shutdown(void);

/*
 * Render one HUD frame on the Metal path. Mirrors gl_render_frame's
 * lifetime: lock main loop for hud_update, unlock, then encode the
 * HUD draw data into a single command buffer and present it.
 *
 * For slice M1 the NV2A side is still a no-op renderer (mtl/renderer.c
 * stub), so the visible result is a black background with the ImGui
 * HUD on top. M2 introduces the surface manager; the HUD compositor
 * here will then read the NV2A framebuffer texture.
 */
void xemu_metal_render_frame(void);

/*
 * Accessors so the C++ HUD code can pass the device to ImGui_ImplMetal_*
 * (font texture rebuild) and the layer to MetalFX / future capture
 * code. Returned as void* so non-ObjC translation units can hold them
 * opaquely; cast back to id<MTLDevice> / CAMetalLayer* in ObjC code.
 */
void *xemu_metal_get_device(void);
void *xemu_metal_get_layer(void);

/*
 * True after xemu_metal_init() has succeeded and before
 * xemu_metal_shutdown() has been called. Used by the HUD code to pick
 * the correct ImGui backend at update/render time without re-querying
 * g_config.display.renderer (avoids races if the user changes the
 * dropdown — the active session's renderer choice is locked at boot).
 */
bool xemu_metal_is_active(void);

/*
 * Called by the C++ HUD code from xemu_hud_update() before
 * ImGui::NewFrame(). On the Metal path this stashes the active
 * MTLRenderPassDescriptor for the current frame's drawable and calls
 * ImGui_ImplMetal_NewFrame() and ImGui_ImplSDL3_NewFrame(). Returns
 * true if the Metal path actually started a frame (drawable was
 * acquired). When false, the caller should skip rendering this frame.
 *
 * Implemented in xemu-metal.m. Declared here so main.cc can call it
 * without including the ObjC header.
 */
bool xemu_metal_begin_imgui_frame(void);

/*
 * True after xemu_metal_begin_imgui_frame() has successfully acquired a
 * drawable and prepared the ImGui Metal backend for the current frame.
 * The HUD update path uses this to avoid acquiring the drawable a second
 * time when xemu_metal_render_frame() has already done the blocking work
 * outside BQL.
 */
bool xemu_metal_imgui_frame_active(void);

/*
 * Encode the ImGui draw data into the active command buffer's render
 * encoder, present the drawable, and commit. Called from
 * xemu_hud_render() on the Metal path.
 */
void xemu_metal_end_imgui_frame(void);

/*
 * Initialize / destroy the ImGui-Metal font atlas texture. Called from
 * FontManager::Rebuild() on the Metal path (replaces
 * ImGui_ImplOpenGL3_CreateFontsTexture()).
 */
void xemu_metal_create_fonts_texture(void);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_METAL_H */
