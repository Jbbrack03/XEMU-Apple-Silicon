/*
 * Apple Silicon performance fork: emulation-rate slewing.
 *
 * Graphics-API-agnostic adjustment of the NV2A vblank interval to track
 * the host display's actual refresh rate when it is close to (but not
 * exactly) the Xbox 60 Hz target. Mirrors the PCSX2 PR #5488 /
 * DuckStation "Sync to host refresh rate" pattern.
 *
 * Algorithm:
 *   - Read the host current display mode via SDL3 once at startup and
 *     re-read on display-changed events.
 *   - host_hz = mode.refresh_rate (Hz, may be 0 when unknown).
 *   - ratio = host_hz / 60.0
 *   - When XEMU_GL_RATE_SLEW=1 (or alias XEMU_RATE_SLEW=1) AND
 *     0.95 ≤ ratio ≤ 1.05 AND host_hz > 0:
 *       vblank_interval_ns = (uint64_t)(16,666,666.0 * (60.0 / host_hz));
 *     Otherwise leave at 16,666,666 (60 Hz hardcoded NV2A vblank).
 *
 * The flag name `XEMU_GL_RATE_SLEW` matches the existing `XEMU_GL_*`
 * family naming convention; it is graphics-API-agnostic in
 * implementation and applies equally to the GL and Metal renderers.
 * `XEMU_RATE_SLEW` is accepted as a clearer alias.
 *
 * Audio rate-match (sample-rate adjustment to match the new vblank
 * interval) is a known limitation. At a typical 60.00→59.94 host
 * refresh, the audio drift is sub-perceptual (~0.1 %); document and
 * defer.
 *
 * Counters surfaced on the `xemu-perf:` interval line:
 *   RATE_SLEW_RATIO_E6 - host_hz / 60.0 × 1,000,000 (zero when host_hz
 *                        unknown).
 *   RATE_SLEW_ACTIVE   - 1 when the slew adjustment is currently
 *                        applied to vblank_interval_ns; 0 otherwise.
 *
 * Default OFF (XEMU_GL_RATE_SLEW=0) for the first cut. Validate with
 * a benchmark before flipping default.
 *
 * SDL is only required for the init / update entry points (which are
 * called from ui/xemu.c, where SDL is always available). The emit
 * function and accessors are SDL-free so they can be called from
 * per-target translation units like profile.c without leaking the SDL
 * header.
 */

#ifndef QEMU_XEMU_RATE_SLEW_H
#define QEMU_XEMU_RATE_SLEW_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SDL_VERSION_ATLEAST
/* When ui/xemu.c includes this header, SDL3 is already in scope. */
typedef SDL_Window XemuRateSlewWindow;
#else
struct SDL_Window;
typedef struct SDL_Window XemuRateSlewWindow;
#endif

/*
 * Initialize the rate-slew module. Reads XEMU_GL_RATE_SLEW /
 * XEMU_RATE_SLEW from the environment, computes the initial
 * ratio/active state from the supplied window's current display mode,
 * and updates `vblank_interval_ns` (defined in ui/xemu.c) accordingly.
 *
 * Idempotent: safe to call multiple times. Subsequent calls re-read
 * the host refresh rate without re-parsing the env var.
 *
 * Returns true if a non-default vblank interval was applied.
 */
bool xemu_rate_slew_init(XemuRateSlewWindow *window);

/*
 * Re-read the host display's current refresh rate and update
 * `vblank_interval_ns` if needed. Called from the SDL event handler
 * on SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED and
 * SDL_EVENT_WINDOW_DISPLAY_CHANGED.
 *
 * Cheap: a single SDL query + a clamp + an atomic store. Safe to call
 * from the SDL event watch thread.
 */
void xemu_rate_slew_update(XemuRateSlewWindow *window);

/* Accessors for tests / counters. */
double   xemu_rate_slew_current_ratio(void);
bool     xemu_rate_slew_is_active(void);
double   xemu_rate_slew_host_hz(void);

/*
 * Emit RATE_SLEW_* counters on the `xemu-perf:` interval line. Always
 * emits when the module has been initialized (host_hz > 0 indicates
 * we have a meaningful sample); no-op until init.
 */
void xemu_rate_slew_emit(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_XEMU_RATE_SLEW_H */
