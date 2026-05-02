/*
 * Apple Silicon performance fork: display / vblank / present counters.
 *
 * Four atomic counters that decompose the "30 FPS cap attribution"
 * question into four independent measurements:
 *
 *   xemu_display_perf_vblank_fired()   - the xemu vblank-timer thread
 *                                         fires NV_PCRTC vblank to the
 *                                         guest. Bumped from
 *                                         hw/xbox/nv2a/nv2a.c
 *                                         (nv2a_vga_gfx_update).
 *   xemu_display_perf_flip_stall()     - the guest writes
 *                                         NV097_FLIP_STALL: it has
 *                                         finished a frame and is
 *                                         requesting a present. Bumped
 *                                         from
 *                                         hw/xbox/nv2a/pgraph/pgraph.c
 *                                         FLIP_STALL handler.
 *   xemu_display_perf_present()        - the guest's READ_3D pointer
 *                                         advances (page flip
 *                                         completes). The "present
 *                                         heartbeat". Bumped from the
 *                                         existing nv2a_profile_increment
 *                                         path.
 *   xemu_display_perf_gl_swap()        - the host runs SDL_GL_SwapWindow
 *                                         (the actual host present).
 *                                         Bumped from ui/xemu.c
 *                                         (gl_render_frame).
 *
 * Snapshotted + reset per-interval by xemu_display_perf_emit_and_reset,
 * called from the NV2A profile interval-flush path so the values appear
 * on the same `xemu-perf:` line as the existing FPS / mspf metrics.
 *
 * The decisive ratio for the 30 FPS cap diagnostic:
 *   - VBLANK_FIRES > 30/s + PRESENT_HEARTBEAT == 30/s + GL_SWAPS = 60/s
 *     => xemu pacing is correct, the guest is intrinsically rendering
 *     at 30 FPS (or its FLIP_STALL handler is the bottleneck).
 *   - VBLANK_FIRES == 30/s
 *     => xemu's vblank pacing is the cap; raise vblank_interval_ns.
 *   - VBLANK_FIRES > 30/s + GL_SWAPS == 30/s
 *     => the host display thread is throttled (likely by host vsync
 *     blocking SDL_GL_SwapWindow at 60/s but the engine only advances
 *     the framebuffer half as often).
 *
 * Cost: one qatomic_inc per event. Always on; the values only become
 * visible when XEMU_PERF_LOG=1 so the per-interval emit cost is gated.
 */

#ifndef QEMU_XEMU_DISPLAY_PERF_H
#define QEMU_XEMU_DISPLAY_PERF_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void xemu_display_perf_vblank_fired(void);
void xemu_display_perf_flip_stall(void);
void xemu_display_perf_present(void);
void xemu_display_perf_gl_swap(void);

/* Append `KEY=value` fields to the open `xemu-perf:` interval line and
 * reset the counters for the next interval. Called from
 * nv2a_profile_log_emit_interval. */
void xemu_display_perf_emit_and_reset(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_DISPLAY_PERF_H */
