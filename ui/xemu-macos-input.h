/*
 * xemu macOS native input backend (slice N2).
 *
 * Apple Silicon performance fork. Routes controller polling through
 * GameController.framework instead of SDL3 when the user opts in via
 * `XEMU_MACOS_NATIVE_INPUT=1`. The framework keeps controller state
 * continuously updated out-of-process in `gamecontrollerd`, so the
 * polling cost from xemu's side is just a property read — no SDL
 * event-queue drain, no thread hop.
 *
 * Default off. Existing users see exactly today's SDL behavior
 * unless they set the env. See
 * `docs/apple-silicon/macos-input-research.md` §6 for the migration
 * plan and `docs/apple-silicon/automation.md` for the env-var docs.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef XEMU_MACOS_INPUT_H
#define XEMU_MACOS_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the GameController.framework backend. Idempotent. Safe to
 * call when no controller is plugged in — it simply enumerates an
 * empty list and registers connect/disconnect notification handlers
 * for future hot-plug events. Returns true on success.
 */
bool xemu_macos_input_init(void);

/* Tear down notification handlers and drop references. */
void xemu_macos_input_shutdown(void);

/* True after init() has succeeded. */
bool xemu_macos_input_is_active(void);

/*
 * Sample the current state of the GameController bound to `port`
 * (0..3). On success writes the standard controller-state buttons
 * mask into `*buttons_out` and the six axis values (LTRIG, RTRIG,
 * LSTICK_X, LSTICK_Y, RSTICK_X, RSTICK_Y) into `axis_out[6]`. On
 * failure (nothing bound, port out of range) the function returns
 * false and does not modify the outputs. The caller should fall
 * through to whatever path it would have used otherwise.
 */
bool xemu_macos_input_get_state(int port,
                                uint16_t *buttons_out,
                                int16_t axis_out[6]);

/*
 * Drive rumble on the GameController bound to `port`. `low` and
 * `high` are the Xbox-side 16-bit motor strengths (left/heavy and
 * right/light). When the controller does not expose Core Haptics
 * (older or unsupported hardware) this call is a no-op and a
 * one-shot warning is logged.
 */
void xemu_macos_input_rumble(int port, uint16_t low, uint16_t high);

/* Number of currently connected GameControllers. Used by the
 * one-shot startup log line. */
int xemu_macos_input_controller_count(void);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_MACOS_INPUT_H */
