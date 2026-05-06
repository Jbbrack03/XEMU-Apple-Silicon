/*
 * xbed_capture — write the post-flip front buffer to disk and reboot.
 *
 * The diag XBE pipeline (`oracle-orchestrator.py run-diag`) chainloads
 * a diag XBE, waits for it to write artifacts onto its own D:\
 * directory, and waits for FTP to come back after the reboot. This
 * helper provides the standard "render is done, capture and exit"
 * tail that every Tier-1 XBE shares.
 *
 * Capture format is the same XOSS layout the oracle agent's
 * `screenshot` command uses (4-byte magic 'XOSS' + u32 width + u32
 * height + u32 stride + raw pixels in front-buffer-native byte
 * order). The Mac side decodes via
 * `oracle-client.py:bgrx_to_rgba` regardless of source.
 */
#ifndef XBED_CAPTURE_H
#define XBED_CAPTURE_H

#include "xbed_runtime.h"

/* Write the current front-buffer (post-flip) as XOSS to `xoss_path`
 * (a D:\... or absolute Xbox path). Returns XBED_OK on success or
 * XBED_FAIL_CAPTURE on I/O error. The caller is expected to call
 * xbed_frame_end_and_swap() before this so the most recent draw is
 * actually scanned out. */
xbed_status_t xbed_capture_front_to_xoss(const char *xoss_path);

/* Convenience: capture, write a `done.txt` liveness marker (so the
 * orchestrator can distinguish "diag finished OK" from "diag crashed
 * after some bytes were partial-written"), sleep a bit so a real-Xbox
 * spectator can see the pattern, then HalReturnToFirmware(reboot).
 *
 * `xbe_id` is embedded in the done file for audit; pass the manifest
 * id (e.g. "mirror"). */
void xbed_capture_and_reboot(const char *xoss_path,
                             const char *done_path,
                             const char *xbe_id);

/* Render a frame callback `n_frames` times before capture+reboot. The
 * callback gets the (0-indexed) frame number and a user pointer.
 * Used to give the host (xemu) enough rendered frames to land at
 * least one screenshot during the diag pattern, while still
 * rebooting cleanly so the real-Xbox FTP-collect path works.
 *
 * Each iteration does xbed_frame_begin(), invokes `fn`, then
 * xbed_frame_end_and_swap(). The XBE's main() should pre-load the
 * shaders / VS uniforms / vertex bindings ONCE before the loop, then
 * the callback only issues per-frame state and draw calls.
 *
 * Recommended `n_frames` for Tier-1 diag XBEs: ~300 (5 s at 60 Hz)
 * — plenty of capture opportunities while keeping the per-XBE run
 * under ~10 s on real Xbox.
 */
typedef void (*xbed_frame_fn)(uint32_t frame_idx, void *user_ctx);
void xbed_render_loop_then_capture(xbed_frame_fn fn, void *ctx,
                                   uint32_t n_frames,
                                   const char *xoss_path,
                                   const char *done_path,
                                   const char *xbe_id);

#endif /* XBED_CAPTURE_H */
