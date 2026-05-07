/*
 * controller-roundtrip — synthetic-input integration oracle.
 *
 * Validates that the oracle agent's controller buffer (set via Mac-side
 * `controller.set` RPCs) survives an `XLaunchXBE` chainload and is
 * readable by a downstream diag XBE through the
 * `xbed_input_synth_*` shim layer.
 *
 * Test sequence (intended caller flow, mirrored in
 * `scripts/apple-silicon/oracle-orchestrator.py`):
 *
 *   1. Mac orchestrator boots the agent.
 *   2. Mac orchestrator calls `controller.set port=0 buttons=…
 *      lt=… rt=… lx=… ly=… rx=… ry=…` to write a known synthetic
 *      state into the agent's persistent kernel-pool buffer.
 *   3. Mac orchestrator chainloads THIS XBE via `runxbe`.
 *   4. THIS XBE reads port-0 state via the shim and renders a known
 *      pattern derived from the bytes it observed (see Pattern below).
 *   5. THIS XBE captures the post-flip front buffer and reboots back
 *      to the dashboard.
 *   6. Mac orchestrator pulls the capture and compares against
 *      `expected.py:from_state(...)` synthesized from the SAME
 *      synthetic state it set in step 2.
 *
 * If the captured PNG matches the math-derived expected, the buffer
 * survived chainload byte-for-byte AND the shim read it correctly.
 * Mismatch isolates the bug to:
 *   - The agent's persistence path (kernel pool eviction / wrong page).
 *   - The shim's anchor-file read or kseg0 mapping.
 *   - The shim's seq-stamped read tearing under concurrent writes
 *     (defensive: caller MUST quiesce writes before runxbe).
 *
 * Pattern (640x480 X8R8G8B8 framebuffer):
 *
 *   - Top half (rows 0..239): solid color derived from the buttons
 *     field. Each button bit lights a 40x40 cell laid out as a 4x4
 *     grid (4 bits per row, 4 rows of bits = 16 bits total). The
 *     cell is white (0xFFFFFFFF) if the corresponding bit is set,
 *     black otherwise. Cell (col, row) covers pixels (col*40 +
 *     CELL_PAD .. col*40 + 40 - CELL_PAD, row*40 + CELL_PAD ..
 *     row*40 + 40 - CELL_PAD) for 0 <= col < 4, 0 <= row < 4.
 *     16 cells * 40^2 px = 25,600 px in a 160x160 grid; centered
 *     horizontally on the screen (left edge at x = (640-160)/2 =
 *     240) and vertically in the top half (top edge at y =
 *     (240-160)/2 = 40).
 *
 *   - Bottom half (rows 240..479): six horizontal stripes, one per
 *     axis (lt, rt, lx, ly, rx, ry). Each stripe is 240/6 = 40
 *     pixels tall. The stripe's filled-fraction encodes the axis
 *     value normalized to [0..1]:
 *       lt / rt: value / 32767 (clamp at 0..1; negative → 0)
 *       lx / ly / rx / ry: (value + 32768) / 65535
 *     Filled portion is white (0xFFFFFFFF), unfilled is black. The
 *     stripe spans the full 640-pixel width.
 *
 * The pattern is deliberately additive: zero state → all black except
 * the static labels (the orchestrator's reference is also pre-populated
 * with zero state when it issues `controller.clear`). A non-trivial
 * synthetic state lights specific pixels deterministically, so the
 * comparison is byte-exact (no anti-aliasing, no flat-shading
 * variability, no FP drift; matches the existing `mirror` oracle's
 * byte-exact tier).
 *
 * Failure modes the diag intentionally surfaces:
 *
 *   - Shim attach failure (no agent buffer in kernel pool / anchor
 *     file missing / E: not mountable): all-cyan (0xFF00FFFF)
 *     screen so the orchestrator's verdict is unambiguously
 *     "wiring broken" rather than "synthetic state mismatched".
 *   - Shim attach success + zero state: all-black (no cells lit, no
 *     stripes filled). Distinguishable from the cyan failure case.
 */
#include "xbed_capture.h"
#include "xbed_input_synth.h"
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

#define WIDTH      640
#define HEIGHT     480
#define CELL_PX    40   /* per-button grid cell, px square */
#define CELL_PAD    2   /* black gutter inside each cell */
#define GRID_X0   240   /* (640-160)/2 */
#define GRID_Y0    40   /* (240-160)/2 */
#define STRIPE_PX  40   /* 240/6 */
#define STRIPES_Y0 240

typedef struct {
    float pos[3];
    float color[3];
} __attribute__((packed)) ColoredVertex;

/* Pre-allocated GPU-visible vertex buffer. Sized for the worst case
 * (16 button cells + 6 axis stripes = 22 rects × 6 vertices = 132).
 * Allocated once in main(); s_push_rect just appends into it and
 * issues a 6-vertex draw against the appropriate offset. We keep a
 * cumulative `s_vert_count` so all draws in a frame share the same
 * physical page (the GPU's vertex puller streams from a single base
 * address) — this matches mirror/main.c's pre-allocate-once pattern
 * and avoids any per-rect MmAllocateContiguousMemoryEx + free dance
 * that risks freeing a buffer the GPU is still consuming. */
#define MAX_RECTS    32
#define VERTS_PER_RT 6
static ColoredVertex *s_vbuf;
static int            s_vert_count;

/* Reset the per-frame draw cursor. Called from render_one before any
 * s_push_rect. */
static void s_vbuf_reset(void) { s_vert_count = 0; }

/* Append one rect's 6 vertices to the shared vbuf without issuing a
 * draw. Caller flushes via s_vbuf_flush() which issues ONE big draw
 * for all queued vertices — mirroring mirror/main.c's pattern (one
 * pre-allocated buffer, one BEGIN/END pair per frame). Per-rect draws
 * triggered NV097 BEGIN/END state thrash that produced cyan
 * interpolation artifacts at triangle boundaries; consolidating into
 * a single OP_TRIANGLES draw cleans that up. */
static void s_push_rect(float x0, float y0, float x1, float y1,
                        float r, float g, float b)
{
    if (!s_vbuf) return;
    if (s_vert_count + VERTS_PER_RT > MAX_RECTS * VERTS_PER_RT) return;

    /* clip-space conversion (matches mirror/main.c):
     *   cx = (x - WIDTH/2)  / (WIDTH/2)
     *   cy = (HEIGHT/2 - y) / (HEIGHT/2)   (Y-flipped) */
    const float cx0 = (x0 - WIDTH * 0.5f)  / (WIDTH * 0.5f);
    const float cx1 = (x1 - WIDTH * 0.5f)  / (WIDTH * 0.5f);
    const float cy0 = (HEIGHT * 0.5f - y0) / (HEIGHT * 0.5f);
    const float cy1 = (HEIGHT * 0.5f - y1) / (HEIGHT * 0.5f);

    ColoredVertex *v = &s_vbuf[s_vert_count];
    v[0] = (ColoredVertex){{ cx0, cy0, 0.0f }, { r, g, b }}; /* tl */
    v[1] = (ColoredVertex){{ cx1, cy0, 0.0f }, { r, g, b }}; /* tr */
    v[2] = (ColoredVertex){{ cx1, cy1, 0.0f }, { r, g, b }}; /* br */
    v[3] = (ColoredVertex){{ cx0, cy0, 0.0f }, { r, g, b }};
    v[4] = (ColoredVertex){{ cx1, cy1, 0.0f }, { r, g, b }};
    v[5] = (ColoredVertex){{ cx0, cy1, 0.0f }, { r, g, b }};
    s_vert_count += VERTS_PER_RT;
}

/* Issue one OP_TRIANGLES draw covering all queued vertices. */
static void s_vbuf_flush(void)
{
    if (!s_vbuf || s_vert_count == 0) return;
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_vbuf[0].pos[0]);
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(ColoredVertex), &s_vbuf[0].color[0]);
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, s_vert_count);
}

/* Render the button-grid + axis-stripe pattern from a port state. */
static void s_render_state(const struct xbed_port_state *st)
{
    /* Per-frame setup: viewport matrix + attribute slot defaults. The
     * lib's helpers issue PB commands directly so the order matters
     * (matrix-load BEFORE first draw_arrays). The s_push_rect calls
     * below append into s_vbuf and issue per-rect draws. */
    xbed_load_viewport_matrix();
    xbed_clear_all_attribs_to_float();
    s_vbuf_reset();

    /* Top half: 4x4 button grid. */
    for (int bit = 0; bit < 16; bit++) {
        if (!(st->buttons & (1u << bit))) continue;
        int col = bit % 4;
        int row = bit / 4;
        float x0 = (float)(GRID_X0 + col * CELL_PX + CELL_PAD);
        float y0 = (float)(GRID_Y0 + row * CELL_PX + CELL_PAD);
        float x1 = (float)(GRID_X0 + col * CELL_PX + CELL_PX - CELL_PAD);
        float y1 = (float)(GRID_Y0 + row * CELL_PX + CELL_PX - CELL_PAD);
        s_push_rect(x0, y0, x1, y1, 1.0f, 1.0f, 1.0f);
    }

    /* Bottom half: 6 axis stripes. Order: lt, rt, lx, ly, rx, ry. */
    int16_t axes[6] = {
        st->ltrigger, st->rtrigger,
        st->lstick_x, st->lstick_y, st->rstick_x, st->rstick_y,
    };
    int is_signed[6] = {0, 0, 1, 1, 1, 1};
    for (int i = 0; i < 6; i++) {
        float frac;
        if (is_signed[i]) {
            /* lstick/rstick: (-32768..32767) → (0..1) */
            frac = ((float)axes[i] + 32768.0f) / 65535.0f;
        } else {
            /* triggers: (0..32767) → (0..1); negative clamps to 0 */
            frac = axes[i] < 0 ? 0.0f : (float)axes[i] / 32767.0f;
        }
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (frac == 0.0f) continue;

        int filled_w = (int)((float)WIDTH * frac);
        if (filled_w == 0) continue;
        float y0 = (float)(STRIPES_Y0 + i * STRIPE_PX);
        float y1 = (float)(STRIPES_Y0 + i * STRIPE_PX + STRIPE_PX);
        s_push_rect(0.0f, y0, (float)filled_w, y1, 1.0f, 1.0f, 1.0f);
    }

    /* One BEGIN/END pair for all queued rects this frame. */
    s_vbuf_flush();
}

static struct xbed_port_state s_state;
static int                    s_have_state;

static void render_one(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;

    /* Background:
     *   - all-black if shim attach succeeded (the additive pattern then
     *     overlays the cells / stripes)
     *   - all-cyan if shim attach failed (so the orchestrator can tell
     *     "wiring broken" apart from "synthetic state mismatched") */
    if (xbed_input_synth_attached()) {
        xbed_clear_color_argb(0xFF000000);
    } else {
        xbed_clear_color_argb(0xFF00FFFF);
        return;
    }

    /* Read fresh state every frame — Mac-side `controller.set`
     * may have updated the buffer between attach and now. (For the
     * initial Tier-1 test the orchestrator quiesces writes before
     * runxbe; future use cases may stream updates.) */
    if (!s_have_state ||
        xbed_input_synth_read(0, &s_state) != 0) {
        return;
    }
    s_render_state(&s_state);
}

int main(void)
{
    if (xbed_init(WIDTH, HEIGHT) != XBED_OK) return 1;

    debugPrint("controller-roundtrip v0.1\n");

    xbed_set_default_render_state();
    xbed_load_default_shaders();

    /* Pre-allocate the shared vertex buffer in PAGE_WRITECOMBINE
     * memory (mirrors mirror/main.c) so all per-frame draws share a
     * single GPU-visible page. Size: 32 rects × 6 verts × 24 B = 4608 B
     * which fits in a single 4 KiB page after rounding up. */
    s_vbuf = (ColoredVertex *)MmAllocateContiguousMemoryEx(
        (size_t)MAX_RECTS * VERTS_PER_RT * sizeof(ColoredVertex),
        0, 0x03ffb000, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_vbuf) {
        debugPrint("MmAllocateContiguousMemoryEx (vbuf) failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }

    xbed_input_synth_status_t at = xbed_input_synth_attach();
    if (at == XBED_INPUT_SYNTH_OK) {
        debugPrint("attached OK at phys=0x%08lx virt=0x%08lx\n",
                   (unsigned long)xbed_input_synth_phys_addr(),
                   (unsigned long)xbed_input_synth_virt_addr());
        if (xbed_input_synth_read(0, &s_state) == 0) {
            s_have_state = 1;
            debugPrint("port0: btn=0x%04x lt=%d rt=%d lx=%d ly=%d "
                       "rx=%d ry=%d seq=%u\n",
                       (unsigned)s_state.buttons,
                       (int)s_state.ltrigger, (int)s_state.rtrigger,
                       (int)s_state.lstick_x, (int)s_state.lstick_y,
                       (int)s_state.rstick_x, (int)s_state.rstick_y,
                       (unsigned)s_state.seq);
        }
    } else {
        debugPrint("xbed_input_synth_attach failed: %d\n", (int)at);
    }

    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\controller-roundtrip-capture.bin",
        "D:\\controller-roundtrip-done.txt",
        "controller-roundtrip");
    return 0;
}
