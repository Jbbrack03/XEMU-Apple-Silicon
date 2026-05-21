/*
 * msaa-aa-factor — MSAA edge-gradient profile Tier-1 NV2A diag XBE.
 *
 * NV2A feature exercised: §C.4 (per-fragment AA factor) and §K.4
 *                         (resolve / display path interaction with
 *                         multisampled color RT).
 * Self-validation tier:    1 (host-side capture; math-derived oracle
 *                         with epsilon tolerance for AA boundary).
 * Oracle priority:         real-xbox (canonical) + math-derived (audit).
 *
 * --- What this XBE tests --------------------------------------------
 *
 * Per `diagnostic-xbe-plan.md` v2 §4.15: "High-contrast diagonal edge at
 * sub-pixel angle; per AA mode (none / 2× / 4×); sample edge
 * perpendicular; expected gradient profile per mode."
 *
 * The XBE renders ONE high-contrast WHITE triangle on a solid-BLACK
 * background. The triangle's three vertices are positioned so that BOTH
 * of its long diagonal edges cross the 640x480 raster at sub-pixel
 * angles guaranteed to NOT line up with any pixel boundary:
 *
 *   v0 = (60, 60)
 *   v1 = (60, 420)
 *   v2 = (580, 240)
 *
 * Edge slopes:
 *   - top edge    (v0 → v2): Δx=520, Δy=180; slope = 180/520 ≈ 0.346
 *   - bottom edge (v1 → v2): Δx=520, Δy=-180; slope ≈ -0.346
 *   - left edge   (v0 → v1): Δx=0, Δy=360 (vertical; pixel-aligned, no AA)
 *
 * The two diagonal edges advance ~0.346 pixels in y per pixel in x, so
 * every column inside [60..580] places the edge at a different
 * fractional position within its pixel. This is the textbook input for
 * a coverage-based MSAA AA factor:
 *   - msaa=0 (no AA): each pixel resolves to a HARD step — fully BLACK
 *     where the triangle does not cover the pixel center, fully WHITE
 *     where it does. The transition is exactly one pixel wide.
 *   - msaa=2: each pixel inside the edge band is rendered with 2
 *     coverage samples. Per-pixel output ∈ {0, 128, 255} (assuming
 *     non-rotated 2× pattern; renderer-dependent fractional values
 *     are allowed by the harness's `compare_overrides`).
 *   - msaa=4: each pixel inside the edge band is rendered with 4
 *     coverage samples. Per-pixel output ∈ {0, 64, 128, 191, 255} or
 *     similar (renderer-dependent intermediate steps from 4-sample
 *     coverage).
 *
 * The math-derived oracle (see `expected.py`) is the HARD-STEP rendering
 * (i.e. the msaa=0 output): pixels strictly inside the triangle are
 * 255, pixels strictly outside are 0. The XBE PASSes a multisampled
 * renderer when:
 *
 *   1. Interior of the triangle (the solid-WHITE signal region) is
 *      byte-exact 255 — proves the geometry rasterized to the expected
 *      cover region.
 *   2. Exterior of the triangle (the solid-BLACK signal region) is
 *      byte-exact 0 — proves nothing leaked outside the expected
 *      cover region (e.g. broken MSAA store-and-resolve writing
 *      multisample contents into the wrong tile).
 *   3. Edge boundary pixels — the ~2-pixel-wide AA band along the
 *      diagonal hypotenuse — are tolerated by `compare_overrides`. The
 *      AA gradient values themselves are renderer-dependent (Apple
 *      Silicon's standard 4× pattern differs from NV2A's; spec only
 *      requires monotone coverage). The boundary band is ≤1% of total
 *      pixels (≈2200 of 307200), well under the manifest's 3.0%
 *      `max_changed_pct`.
 *
 * The XBE itself is renderer-MSAA-agnostic: it issues plain NV2A draws
 * with smooth shading and no per-fragment AA hints. Whether MSAA
 * engages is determined by the HOST renderer (`XEMU_METAL_MSAA=N` on
 * Metal; `XEMU_GL_MSAA=N` on GL) and the `surface_scale`. This XBE
 * therefore proves that the renderer:
 *   (a) honors the MSAA flag (asserted via `required_counters_min` on
 *       `METAL_MSAA_RESOLVE_COUNT` in the manifest);
 *   (b) produces a non-degenerate coverage band at the expected
 *       geometric position;
 *   (c) keeps the interior + exterior signal regions byte-exact.
 *
 * --- Catches --------------------------------------------------------
 *
 *   - MSAA path never engaged: `METAL_MSAA_RESOLVE_COUNT == 0` →
 *     manifest counter-assertion fails the cell.
 *   - MSAA resolve writes garbage into non-edge tiles (e.g. broken
 *     `MTLStoreActionStoreAndMultisampleResolve` semantics): interior
 *     or exterior signal region fails the byte-exact 0/255 check.
 *   - Geometry mis-rasterized (wrong triangle position / orientation
 *     / fill rule): expected signal region position diverges from
 *     captured signal region → `min_signal_match_pct` fails.
 *   - Front-face culling regression: triangle vanishes; black-everywhere
 *     output fails the signal-region check.
 *
 * --- Does NOT catch (documented limits, second-wave follow-ups) -----
 *
 *   - Exact per-sample coverage at sub-pixel resolution (would require
 *     readback of pre-resolve multisample data; deferred to a Tier-2
 *     XBE that emits multiple probe lines and a per-line oracle).
 *   - Per-sample programmable position correctness (Apple Silicon
 *     `[MTLRenderPipelineDescriptor setSampleCount:]` vs
 *     `setSamplePositions:` paths; defer to a second-wave
 *     `msaa-sample-position` XBE that toggles
 *     `MTLPrimitiveType:point` with a per-sample test pattern).
 *   - Depth-aware MSAA resolve modes (DepthResolveFilter min/max).
 *
 * --- Geometry layout ------------------------------------------------
 *
 * Single isoceles triangle, window-space:
 *
 *     y=60   v0 (60,60) -----
 *               \              ---__
 *                \                  ---__
 *                 \                       ---__ v2 (580, 240)
 *                  \                  ___---
 *                   \             ___-
 *                    \        ___-
 *     y=420  v1 (60, 420) ---
 *
 * Triangle covers exactly 93,600 of 307,200 pixels (~30.5%; geometric
 * area = 1/2 * base * height = 1/2 * 360 * 520 = 93,600). Edge band
 * is ~2200 pixels (≈0.72%) at msaa=2/4 — see actual captured
 * changed_pixels_pct in the 2026-05-21 validation run.
 *
 * --- Reproducibility ------------------------------------------------
 *
 * Pure deterministic pattern; no banner, no counter, no per-frame
 * variation. Byte-identical across two cold runs at the same MSAA
 * mode.
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

/* Triangle vertex positions (window-space, integer for repeatability).
 * Slopes of the two diagonals are 180/520 ≈ 0.346 so every column
 * inside [60..580] places the edge at a distinct fractional sub-pixel
 * position. */
#define V0_X 60
#define V0_Y 60
#define V1_X 60
#define V1_Y 420
#define V2_X 580
#define V2_Y 240

#define VERTS_TOTAL 3

typedef struct {
    float pos[3];     /* clip-space, z=0.5 */
    float color[4];   /* RGBA (0..1); all white in this XBE */
} __attribute__((packed)) AaVertex;

static AaVertex   s_verts[VERTS_TOTAL];
static AaVertex  *s_alloc_verts;

static inline void mk_vert(AaVertex *v, int x_w, int y_w)
{
    /* Window-space (0..W, 0..H) → NDC clip-space.
     * Y-flip so y=0 in window maps to NDC +1 (top). */
    v->pos[0]   = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1]   = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2]   = 0.5f;
    v->color[0] = 1.0f;
    v->color[1] = 1.0f;
    v->color[2] = 1.0f;
    v->color[3] = 1.0f;
}

static void build_geometry(void)
{
    mk_vert(&s_verts[0], V0_X, V0_Y);
    mk_vert(&s_verts[1], V1_X, V1_Y);
    mk_vert(&s_verts[2], V2_X, V2_Y);
}

static void enforce_common_state(void)
{
    /* No depth, no blend, no alpha test, no stencil, no cull. Fill mode
     * smooth shading. The renderer's MSAA companion (when enabled via
     * XEMU_METAL_MSAA / XEMU_GL_MSAA) is created automatically against
     * the same color-RT format we render into. */
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE,   0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK,          0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE,        0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE,   0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE,    0);
    pb_end(p);
}

static void bind_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(AaVertex), &s_alloc_verts[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(AaVertex), &s_alloc_verts[0].color[0]);
}

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    xbed_clear_color_argb(0xFF000000);   /* solid black background */
    xbed_load_viewport_matrix();
    enforce_common_state();
    bind_attribs();

    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, VERTS_TOTAL);
}

int main(void)
{
    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    debugPrint("msaa-aa-factor v0.1\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    build_geometry();

    s_alloc_verts = MmAllocateContiguousMemoryEx(
        sizeof(s_verts), 0, 0x3ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_alloc_verts) {
        debugPrint("MmAllocateContiguousMemoryEx failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    memcpy(s_alloc_verts, s_verts, sizeof(s_verts));

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\msaa-aa-factor-capture.bin",
        "D:\\msaa-aa-factor-done.txt",
        "msaa-aa-factor");
    return 0;
}
