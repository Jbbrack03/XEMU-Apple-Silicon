/*
 * crtc-publish — front-buffer publish policy oracle (Tier-1 NV2A diag XBE).
 *
 * NV2A feature exercised: §H.7 (front-fb publish), §3b.4 (CRTC scan-out),
 *                          §K.2 (per-surface binding state), §H.5
 *                          (NV097_CLEAR_SURFACE on the bound RT).
 * NV097 methods:           SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET,
 *                          SET_BEGIN_END(TRIANGLES), DRAW_ARRAYS,
 *                          CLEAR_SURFACE (via pb_fill), FLIP_STALL
 *                          (pushed manually so the renderer's
 *                          flip-stall publish path is exercised).
 *                          Surface switching uses pbkit extra-buffers
 *                          (pb_target_extra_buffer) which reprograms
 *                          DMA channel 9's base address — picked up
 *                          by xemu's surface_update via the
 *                          accompanying NV097_SET_SURFACE_PITCH push.
 * Self-validation tier:    1 (host-side capture; math-derived oracle).
 * Oracle priority:         real-xbox (canonical) + math-derived
 *                          (audit, per-recipe).
 *
 * --- What this XBE tests ---------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.4, this is the oracle for the
 * Metal renderer's front-buffer publish policy. The XBE renders three
 * distinct VRAM color surfaces in succession every frame:
 *
 *   Surface A : pbkit back buffer       (cleared to RED   0xFFFF0000;
 *                                         0 draw calls)
 *   Surface B : pbkit extra buffer 0    (cleared to GREEN 0xFF00FF00;
 *                                         1 draw call)
 *   Surface C : pbkit extra buffer 1    (cleared to BLUE  0xFF0000FF;
 *                                         3 draw calls)
 *
 * After the C draws, the XBE rebinds the pbkit back buffer (so
 * pbkit's swap chain stays consistent) and manually pushes
 * NV097_FLIP_STALL. pbkit's own swap mechanism (pb_finished) uses
 * DPC + direct PCRTC_START programming and does NOT push FLIP_STALL,
 * so triggering xemu's flip-stall handler requires the manual push.
 *
 * --- What the host should publish for each renderer ------------------
 *
 * The captured front-buffer reflects whichever surface the
 * renderer's publish path resolved to:
 *
 *   * Real Xbox: the NV2A CRTC physically scans whichever surface
 *     pbkit's swap chain rotated to the front. That is always
 *     surface A's content. The XBE-side XOSS capture reads
 *     PCRTC_START (kseg0-mapped) and produces a RED frame.
 *
 *   * xemu-GL: the GL renderer has no fallback path; flip_stall
 *     publishes the CRTC-pointed surface. Captured frame is RED.
 *
 *   * xemu-Metal with XEMU_METAL_FRONT_FB_FALLBACK=0: same as GL —
 *     pgraph_mtl_flip_stall publishes the CRTC-pointed surface (A).
 *     Captured frame is RED.
 *
 *   * xemu-Metal with XEMU_METAL_FRONT_FB_FALLBACK=1: the
 *     publish_latest_draw_fallback path runs from flip_stall and
 *     selects the surface with the highest per-frame draw count.
 *     Our pattern gives C three draws to B's one and A's zero, so C
 *     wins unambiguously. Captured frame is BLUE.
 *
 * --- Math derivation -------------------------------------------------
 *
 * Back-buffer dimensions: 640x480, X8R8G8B8.
 * XEMU_DISPLAY_SCALE=1 (diag-mode default per plan §2.6) maps guest
 * 640x480 → host 640x480 1:1.
 *
 * pb_fill(0, 0, W, H, COLOR) issues NV097_CLEAR_SURFACE with
 * NV097_CLEAR_SURFACE_R/G/B/A clear values that produce a clip-rect-
 * sized fill of the bound surface in COLOR. Each surface gets a
 * single fill, so each surface's content is uniformly its color.
 *
 * The marker draw is a full-screen quad with diffuse color matching
 * the surface's clear color, so the draw is visually a no-op (the
 * surface is already that color), but it still counts toward
 * `frame_draw_count` in the Metal renderer's per-binding tracker
 * (incremented by pgraph_mtl_surface_note_color_draw at draw.mm:1133
 * after every drawPrimitives call, regardless of fragment coverage).
 *
 * Expected output, per-(renderer, flag-recipe):
 *
 *   real-xbox / any                          : all pixels RED   (A).
 *   xemu/gl   / scale=1, msaa=0              : all pixels RED   (A).
 *   xemu/metal/ scale=1, msaa=0, fallback=0  : all pixels RED   (A).
 *   xemu/metal/ scale=1, msaa=0, fallback=1  : all pixels BLUE  (C).
 *
 * Catches:
 *   - Renderer ignores guest CRTC scan address and republishes a
 *     stale or wrong surface (would show GREEN, BLUE, or black).
 *   - FALLBACK=1 path publishes the wrong fallback candidate (not C)
 *     — surface manager tracking bug.
 *   - FALLBACK=1 path falls through to s_color_binding when
 *     s_fallback_draw_candidate is populated (would show RED
 *     because s_color_binding=A at flip_stall) — would catch a
 *     specific regression in publish_latest_draw_fallback ordering.
 *
 * Reproducibility: byte-identical-after-mask across two cold runs.
 * No banner, no timestamps, no frame counter on the captured frame.
 * Pure deterministic uniform color fields.
 */
#include "xbed_capture.h"
#include "xbed_runtime.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define WIN_W 640
#define WIN_H 480

#define COL_A 0xFFFF0000u  /* opaque red   — surface A (CRTC-pointed) */
#define COL_B 0xFF00FF00u  /* opaque green — surface B (intermediate) */
#define COL_C 0xFF0000FFu  /* opaque blue  — surface C (fallback)     */

typedef struct {
    float pos[3];
    float color[3];
} __attribute__((packed)) ColoredVertex;

/* Full-screen quad (two triangles, six vertices). The marker draw
 * issues this geometry with a diffuse color matching the surface's
 * clear color — i.e. the draw paints the same color the surface was
 * just cleared to. Visually invisible, but still a real draw that
 * increments the Metal renderer's per-binding frame_draw_count
 * tracker via pgraph_mtl_surface_note_color_draw (called from
 * draw.mm:1133). */
static const ColoredVertex k_quad_verts[6] = {
    {{-1.0f,  1.0f, 0.5f}, {1.0f, 1.0f, 1.0f}},
    {{ 1.0f,  1.0f, 0.5f}, {1.0f, 1.0f, 1.0f}},
    {{ 1.0f, -1.0f, 0.5f}, {1.0f, 1.0f, 1.0f}},
    {{-1.0f,  1.0f, 0.5f}, {1.0f, 1.0f, 1.0f}},
    {{ 1.0f, -1.0f, 0.5f}, {1.0f, 1.0f, 1.0f}},
    {{-1.0f, -1.0f, 0.5f}, {1.0f, 1.0f, 1.0f}},
};

static ColoredVertex *s_alloc_vertices;

static void s_set_diffuse(float r, float g, float b)
{
    for (int i = 0; i < 6; i++) {
        s_alloc_vertices[i].pos[0] = k_quad_verts[i].pos[0];
        s_alloc_vertices[i].pos[1] = k_quad_verts[i].pos[1];
        s_alloc_vertices[i].pos[2] = k_quad_verts[i].pos[2];
        s_alloc_vertices[i].color[0] = r;
        s_alloc_vertices[i].color[1] = g;
        s_alloc_vertices[i].color[2] = b;
    }
}

static void s_bind_quad_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_vertices[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_alloc_vertices[0].color[0]);
}

static void s_issue_one_marker_draw(float r, float g, float b)
{
    s_set_diffuse(r, g, b);
    s_bind_quad_attribs();
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, 6);
}

static void s_push_flip_stall(void)
{
    /* Manual NV097_FLIP_STALL push. pbkit's pb_finished() does NOT
     * push this method — its swap mechanism uses a PB_FINISHED
     * subprog interrupt + DPC + direct VIDEOREG(PCRTC_START) write,
     * none of which trigger xemu's NV097_FLIP_STALL handler. We need
     * the handler to fire so pgraph_mtl_flip_stall (renderer.c:1031)
     * runs the publish path — either publish_display_front_fb (CRTC)
     * or publish_latest_draw_fallback (fallback candidate) per the
     * XEMU_METAL_FRONT_FB_FALLBACK env flag. */
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_FLIP_STALL, 0);
    pb_end(p);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    /* xbed_render_loop_then_capture already called xbed_frame_begin,
     * which ran pb_target_back_buffer — i.e. surface A is bound and
     * ready. */

    /* The marker draws below use clip-space coords [-1, +1] that go
     * through the default VS at xbe-tests/lib/vs.vs.cg, which
     * multiplies POSITION by the viewport matrix at C[96..99]. Load
     * those constants every frame so the transform is well-defined.
     * pbkit's set_draw_buffer (called from pb_target_extra_buffer)
     * does not touch transform constants, so one load before the
     * first draw would also suffice — but per-frame load matches
     * the existing diag XBEs (mirror / depth-floor / color-channel)
     * and removes any dependence on prior-frame state. */
    xbed_load_viewport_matrix();

    /* === Surface A (CRTC-pointed pbkit back buffer) ===
     *
     * Clear to RED; 0 marker draws. A is the surface real-Xbox CRTC
     * physically scans after pbkit's swap rotates this buffer to
     * front. With 0 draws, A's frame_draw_count stays at 0 — the
     * lowest among A/B/C — so xemu-Metal's
     * publish_latest_draw_fallback never picks A as the candidate
     * (it's only chosen as the fallback-current-binding fallback,
     * which doesn't apply when a candidate exists). */
    xbed_clear_color_argb(COL_A);

    /* === Surface B (pbkit extra buffer 0) ===
     *
     * pb_target_extra_buffer triggers pbkit's set_draw_buffer which
     * reprograms DMA channel 9's base address to B and pushes
     * NV097_SET_SURFACE_PITCH (NV20_TCL_PRIMITIVE_3D_BUFFER_PITCH
     * 0x0000020c — same numeric value, see nxdk/lib/pbkit/
     * nv_objects.h). The pitch push reaches xemu's
     * SET_SURFACE_PITCH handler (pgraph.c:1153) which invokes
     * surface_update, prompting the Metal renderer to re-evaluate
     * the bound surface via nv_dma_load + surface_color.offset.
     * The new vram_addr (B's physical base) is looked up in the
     * cache → new MtlSurfaceBinding for B.
     *
     * Clear B to GREEN, then issue 1 marker draw.
     * B.frame_draw_count = 1 after this. */
    pb_target_extra_buffer(0);
    xbed_clear_color_argb(COL_B);
    s_issue_one_marker_draw(0.0f, 1.0f, 0.0f);

    /* === Surface C (pbkit extra buffer 1) ===
     *
     * Same mechanism as B. Clear to BLUE, then 3 marker draws.
     * C.frame_draw_count = 3 after this. C is the highest-count
     * surface, so publish_latest_draw_fallback selects it as the
     * candidate. */
    pb_target_extra_buffer(1);
    xbed_clear_color_argb(COL_C);
    s_issue_one_marker_draw(0.0f, 0.0f, 1.0f);
    s_issue_one_marker_draw(0.0f, 0.0f, 1.0f);
    s_issue_one_marker_draw(0.0f, 0.0f, 1.0f);

    /* === Rebind A so pbkit's swap chain ends the frame consistent
     *     AND so the Metal renderer's s_color_binding actually
     *     becomes A before FLIP_STALL ===
     *
     * pbkit's pb_finished (called from xbed_frame_end_and_swap)
     * expects the back buffer to be the active surface so the
     * triple-buffer rotation works.
     *
     * Codex 2026-05-20: `pb_target_back_buffer()` by itself does
     * NOT update Metal's `s_color_binding`. The actual surface bind
     * happens inside `mtl_bind_current_surfaces` (renderer.c:769),
     * which is only called from the clear handler
     * (pgraph_mtl_clear_surface at renderer.c:947) and the
     * draw-flush handler (pgraph_mtl_flush_draw at renderer.c:1783).
     * `pgraph_mtl_surface_update` (renderer.c:2048) is a dirty-VRAM
     * poller, not a bind.
     *
     * If we left s_color_binding = C at FLIP_STALL, a regression
     * where `publish_latest_draw_fallback` falls through to
     * s_color_binding (surface.mm:1977-1982) instead of using
     * s_fallback_draw_candidate would still publish BLUE — the
     * test would pass with a broken candidate path. The extra clear
     * below rebinds A explicitly (clear → mtl_bind_current_surfaces
     * → s_color_binding = A) while keeping A.frame_draw_count = 0
     * (clears don't go through note_color_draw).
     *
     * After this rebind:
     *   - s_color_binding (Metal renderer) = A's binding
     *   - s_fallback_draw_candidate         = C's binding (highest count)
     * The fallback publish path now genuinely tests "candidate wins
     * over current binding": a broken candidate would publish RED
     * (s_color_binding=A); a correct candidate publishes BLUE
     * (s_fallback_draw_candidate=C). */
    pb_target_back_buffer();
    xbed_clear_color_argb(COL_A);

    /* === Trigger xemu's flip-stall publish path ===
     *
     * Manual FLIP_STALL push. pgraph_mtl_flip_stall (renderer.c:1031)
     * then runs publish_display_front_fb (fallback=0; publishes A)
     * or publish_latest_draw_fallback (fallback=1; publishes C).
     * fallback_draw_reset() is called at the end of
     * publish_latest_draw_fallback, so per-binding counts reset for
     * the next frame's accumulation. */
    s_push_flip_stall();
}

int main(void)
{
    /* Request two extra VRAM-backed render targets BEFORE xbed_init —
     * pb_init (called from xbed_init) reads pb_ExtraBuffersCount to
     * size the per-buffer allocation. Each extra buffer is W*pitch
     * = 640*2560 = 1.5 MB; total 3 MB of extra VRAM. */
    pb_extra_buffers(2);

    if (xbed_init(WIN_W, WIN_H) != XBED_OK) {
        return 1;
    }
    debugPrint("crtc-publish v0.1\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    /* Allocate the marker quad's vertex array in PAGE_WRITECOMBINE
     * memory so the GPU's vertex puller sees the data without a
     * cache flush. Same pattern flat-tri-depth uses. */
    s_alloc_vertices = MmAllocateContiguousMemoryEx(
        sizeof(k_quad_verts), 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_alloc_vertices) {
        debugPrint("MmAllocateContiguousMemoryEx failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    memcpy(s_alloc_vertices, k_quad_verts, sizeof(k_quad_verts));

    /* Render the same pattern 300 times (~5 s at 60 Hz) so xemu's
     * XEMU_METAL_SCREENSHOT_INTERVAL=15 / AT_FRAME=30 path has
     * plenty of post-flip drawables to snapshot, then capture front
     * via PCRTC_START + reboot so the real-Xbox FTP-collect path
     * picks up the XOSS file as the canonical reference. */
    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\crtc-publish-capture.bin",
        "D:\\crtc-publish-done.txt",
        "crtc-publish");
    return 0;
}
