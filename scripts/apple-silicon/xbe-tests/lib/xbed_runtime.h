/*
 * xbed_runtime — shared diag-XBE runtime helpers.
 *
 * Wraps the boilerplate every Tier-1 NV2A diag XBE needs:
 *   - XVideoSetMode + pb_init
 *   - default render-state setup (NV097_SET_BLEND_ENABLE etc.)
 *   - viewport-matrix shader constants
 *   - back-buffer clear / depth-stencil clear / present-and-wait
 *   - shutdown
 *
 * Each diag XBE links the lib by adding `SRCS += $(CURDIR)/../lib/xbed_*.c`
 * to its Makefile (see `lib/lib.mk` for the include snippet).
 *
 * The lib targets the diagnostic-XBE plan v2 §3.1 API surface
 * (`docs/apple-silicon/diagnostic-xbe-plan.md`).
 */
#ifndef XBED_RUNTIME_H
#define XBED_RUNTIME_H

#include <stdint.h>

typedef enum {
    XBED_OK = 0,
    XBED_FAIL_INIT,
    XBED_FAIL_VRAM,
    XBED_FAIL_NET,
    XBED_FAIL_CAPTURE,
} xbed_status_t;

/* xbed_init: bring up XVideo + pbkit at (width, height, 32 bpp).
 * On failure returns XBED_FAIL_INIT and sleeps 2 s before returning so
 * a real-Xbox tester can read the debug print. */
xbed_status_t xbed_init(int width, int height);

/* xbed_shutdown: tear down pbkit. Diag XBEs typically reboot via
 * xbed_capture_and_reboot() and never reach here. */
void          xbed_shutdown(void);

/* Frame loop helpers. Mirror the triangle sample's order:
 *   xbed_frame_begin();
 *   <issue draws>
 *   xbed_frame_end_and_swap();   // flushes, waits for GPU, swaps */
void          xbed_frame_begin(void);
void          xbed_frame_end_and_swap(void);

/* Convenience: clear back buffer to ARGB color. */
void          xbed_clear_color_argb(uint32_t argb);

/* Default render-state setup (no blend, no alpha, no cull, no depth,
 * fill mode, smooth shading). Diag XBEs that need depth or blend
 * enable them themselves AFTER calling this. */
void          xbed_set_default_render_state(void);

/* Standard viewport matrix at the active back-buffer dimensions.
 * Loads into transform constants C[96..99] (the matrix slot that
 * `lib/vs.vs.cg` consumes — same offset the triangle sample uses). */
void          xbed_load_viewport_matrix(void);

/* Standard VS / PS upload. The shaders are baked into `lib/vs.inl`
 * and `lib/ps.inl`; both are passthrough with screen-space POSITION
 * (already in window coordinates) + DIFFUSE → COLOR. */
void          xbed_load_default_shaders(void);

/* Textured VS / PS upload. The shaders are baked into
 * `lib/xbed_tex_vs.inl` and `lib/xbed_tex_ps.inl`; the VS passes
 * POSITION + DIFFUSE + TEXCOORD0 through; the PS samples stage 0
 * via TEXCOORD0 and modulates by DIFFUSE (set DIFFUSE = white to
 * see the raw texture sample). Use with `xbed_texture_bind_stage0`
 * from `xbed_texture.h`. Stage 0 must be set up BEFORE the first
 * draw issued with these shaders bound. */
void          xbed_load_textured_shaders(void);

/* Geometry helpers: clear all 16 attribute slots to TYPE_F (so unused
 * slots don't leak prior state), then bind individual attributes. */
void          xbed_clear_all_attribs_to_float(void);
void          xbed_set_attrib_pointer(unsigned index, unsigned format,
                                      unsigned size, unsigned stride,
                                      const void *data);
void          xbed_draw_arrays(unsigned mode, int start, int count);

/* Back-buffer geometry (after xbed_init). */
int           xbed_back_buffer_width(void);
int           xbed_back_buffer_height(void);

/* Host-visible log channel (cycle 15, 2026-05-22). When xemu is
 * launched with XEMU_GUEST_LOG=1, the host installs an IO-port sink
 * on port 0xE9 (see `hw/xbox/xbox_guest_log.c`) that forwards bytes
 * written by the guest to xemu stderr with an `xemu-guest-log:`
 * prefix. Each call to xbed_host_log_write() emits one line (a
 * trailing '\n' is appended automatically). Each call to
 * xbed_host_log_writef() formats and emits one line.
 *
 * Renderer-agnostic; does NOT depend on screenshot capture, so Tier-2
 * XBEs can surface per-cell oracle verdicts even when the GL or Metal
 * display-capture path is broken. On real Xbox hardware (or stock
 * upstream xemu without the device wired in) the OUT instruction is a
 * silent no-op, so calls are safe regardless of host.
 *
 * Implementation: GCC inline `outb` to a fixed compile-time port
 * (0xE9). The port is intentionally NOT runtime-configurable on
 * either side — a host-side override without a matching rebuild
 * would silently disconnect the channel. If you ever need to move
 * the port, change BOTH this define AND XBOX_GUEST_LOG_IOPORT in
 * `hw/xbox/xbox_guest_log.c`, then rebuild xemu AND the XBE
 * library. */
#define XBED_HOST_LOG_PORT 0xE9

void          xbed_host_log_write(const char *line);
void          xbed_host_log_writef(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* XBED_RUNTIME_H */
