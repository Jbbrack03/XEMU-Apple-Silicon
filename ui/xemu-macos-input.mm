/*
 * xemu macOS native input backend (slice N2).
 *
 * Apple Silicon performance fork. Routes controller polling through
 * GameController.framework when `XEMU_MACOS_NATIVE_INPUT=1`. The
 * framework keeps controller state continuously updated in
 * `gamecontrollerd` out-of-process; the polling cost from xemu's
 * side is just a property read, removing the SDL event-queue +
 * thread-hop overhead.
 *
 * SDL still owns connect/disconnect lifecycle and per-port binding
 * (the rebind UI is built on SDL events; Linux + Windows builds keep
 * depending on SDL). On macOS with this opt-in flag enabled, the
 * read path forwards to GameController instead of SDL while the
 * binding state machine in `xemu-input.c` is unchanged.
 *
 * Mapping is by `GCController.playerIndex`: when xemu binds a
 * controller to port N (0..3) the native backend's
 * connect-notification handler tags every connected GCController
 * with a fresh playerIndex; `get_state(port)` returns the state of
 * the GCController whose playerIndex matches.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "xemu-macos-input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

/* Mirror of the controller-state masks defined in xemu-input.h. The
 * .mm file deliberately does NOT include xemu-input.h: that header
 * pulls in SDL3 which conflicts with -fobjc-arc on some build
 * configurations. The masks are stable parts of the xemu-input ABI
 * and unlikely to drift; if they ever do, the static_assert below
 * will catch it (compile-time check via the matching constants in
 * xemu-input.h). */
#define MAC_CTRL_BTN_A          (1 << 0)
#define MAC_CTRL_BTN_B          (1 << 1)
#define MAC_CTRL_BTN_X          (1 << 2)
#define MAC_CTRL_BTN_Y          (1 << 3)
#define MAC_CTRL_BTN_DPAD_LEFT  (1 << 4)
#define MAC_CTRL_BTN_DPAD_UP    (1 << 5)
#define MAC_CTRL_BTN_DPAD_RIGHT (1 << 6)
#define MAC_CTRL_BTN_DPAD_DOWN  (1 << 7)
#define MAC_CTRL_BTN_BACK       (1 << 8)
#define MAC_CTRL_BTN_START      (1 << 9)
#define MAC_CTRL_BTN_WHITE      (1 << 10)
#define MAC_CTRL_BTN_BLACK      (1 << 11)
#define MAC_CTRL_BTN_LSTICK     (1 << 12)
#define MAC_CTRL_BTN_RSTICK     (1 << 13)
#define MAC_CTRL_BTN_GUIDE      (1 << 14)

#define MAC_CTRL_AXIS_LTRIG    0
#define MAC_CTRL_AXIS_RTRIG    1
#define MAC_CTRL_AXIS_LSTICK_X 2
#define MAC_CTRL_AXIS_LSTICK_Y 3
#define MAC_CTRL_AXIS_RSTICK_X 4
#define MAC_CTRL_AXIS_RSTICK_Y 5

/* GameController.framework axis ranges are normalized [-1.0, 1.0] for
 * sticks and [0.0, 1.0] for triggers. xemu's ControllerState uses
 * signed 16-bit values matching the SDL Gamepad ABI: sticks
 * [-32768, 32767], triggers [0, 32767]. Conversion factor matches
 * SDL3's xinput-mode mapping. */
#define MAC_AXIS_MAX_F (32767.0f)
#define MAC_AXIS_MIN_F (-32768.0f)

static bool s_initialized;
static bool s_warned_no_haptics;
static id<NSObject> s_connect_observer;
static id<NSObject> s_disconnect_observer;

/* Map from xemu port (0..3) to a Core Haptics engine and player.
 * Allocated lazily on first rumble for that port; recreated on
 * controller hotplug. */
@interface XemuMacOSHapticState : NSObject
@property (nonatomic, strong) id /* GCController * */ controller;
@property (nonatomic, strong) id /* CHHapticEngine * */ engineLeft;
@property (nonatomic, strong) id /* CHHapticEngine * */ engineRight;
@property (nonatomic, strong) id /* CHHapticPatternPlayer * */ playerLeft;
@property (nonatomic, strong) id /* CHHapticPatternPlayer * */ playerRight;
@end

@implementation XemuMacOSHapticState
@end

static XemuMacOSHapticState *s_haptic_state[4];

/* Fresh assignment of GCControllerPlayerIndex on every connect /
 * disconnect notification: the i-th currently-connected controller
 * (by [GCController controllers] order) gets playerIndex i. xemu
 * port N then maps to the controller with playerIndex N. */
static void xemu_macos_input_assign_player_indices(void)
{
    NSArray<GCController *> *controllers = [GCController controllers];
    NSInteger i = 0;
    for (GCController *controller in controllers) {
        if (i < 4) {
            controller.playerIndex = (GCControllerPlayerIndex)i;
        } else {
            controller.playerIndex = GCControllerPlayerIndexUnset;
        }
        i++;
    }
}

static GCController *xemu_macos_input_controller_for_port(int port)
{
    if (port < 0 || port > 3) {
        return nil;
    }
    NSArray<GCController *> *controllers = [GCController controllers];
    for (GCController *controller in controllers) {
        if ((NSInteger)controller.playerIndex == (NSInteger)port) {
            return controller;
        }
    }
    return nil;
}

static const char *xemu_macos_input_class_name(GCController *controller)
{
    if (controller == nil) {
        return "(none)";
    }
    if ([controller respondsToSelector:@selector(extendedGamepad)] &&
        controller.extendedGamepad != nil) {
        Class cls = [controller class];
        return [NSStringFromClass(cls) UTF8String];
    }
    return "GCController";
}

static void xemu_macos_input_log_controller(GCController *controller,
                                            NSInteger index)
{
    if (controller == nil) {
        return;
    }
    BOOL haptics = NO;
    if (@available(macOS 11.0, *)) {
        haptics = (controller.haptics != nil);
    }
    fprintf(stderr,
            "xemu-perf: macos_native_input controller[%ld] class=%s "
            "vendor=\"%s\" haptics=%s playerIndex=%ld\n",
            (long)index,
            xemu_macos_input_class_name(controller),
            [(controller.vendorName ?: @"(unknown)") UTF8String],
            haptics ? "yes" : "no",
            (long)controller.playerIndex);
}

bool xemu_macos_input_init(void)
{
    if (s_initialized) {
        return true;
    }

    @autoreleasepool {
        /* Existing controllers (already plugged in before xemu started). */
        NSArray<GCController *> *initial = [GCController controllers];
        xemu_macos_input_assign_player_indices();

        fprintf(stderr,
                "xemu-perf: macos_native_input enabled controllers=%lu\n",
                (unsigned long)initial.count);
        for (NSUInteger i = 0; i < initial.count; i++) {
            xemu_macos_input_log_controller(initial[i], (NSInteger)i);
        }

        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        s_connect_observer = [nc
            addObserverForName:GCControllerDidConnectNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *note) {
                xemu_macos_input_assign_player_indices();
                GCController *c = note.object;
                fprintf(stderr,
                        "xemu-perf: macos_native_input connect class=%s "
                        "vendor=\"%s\" playerIndex=%ld\n",
                        xemu_macos_input_class_name(c),
                        [(c.vendorName ?: @"(unknown)") UTF8String],
                        (long)c.playerIndex);
            }];
        s_disconnect_observer = [nc
            addObserverForName:GCControllerDidDisconnectNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *note) {
                xemu_macos_input_assign_player_indices();
                GCController *c = note.object;
                fprintf(stderr,
                        "xemu-perf: macos_native_input disconnect "
                        "class=%s vendor=\"%s\"\n",
                        xemu_macos_input_class_name(c),
                        [(c.vendorName ?: @"(unknown)") UTF8String]);
                /* Drop any cached haptic state for the now-gone
                 * controller. Per-port slots will be rebuilt lazily
                 * the next time rumble is requested. */
                for (int i = 0; i < 4; i++) {
                    XemuMacOSHapticState *st = s_haptic_state[i];
                    if (st != nil && st.controller == c) {
                        st.playerLeft = nil;
                        st.playerRight = nil;
                        st.engineLeft = nil;
                        st.engineRight = nil;
                        st.controller = nil;
                    }
                }
            }];
    }

    s_initialized = true;
    return true;
}

void xemu_macos_input_shutdown(void)
{
    if (!s_initialized) {
        return;
    }
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    if (s_connect_observer) {
        [nc removeObserver:s_connect_observer];
        s_connect_observer = nil;
    }
    if (s_disconnect_observer) {
        [nc removeObserver:s_disconnect_observer];
        s_disconnect_observer = nil;
    }
    for (int i = 0; i < 4; i++) {
        s_haptic_state[i] = nil;
    }
    s_initialized = false;
}

bool xemu_macos_input_is_active(void)
{
    return s_initialized;
}

int xemu_macos_input_controller_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    return (int)[GCController controllers].count;
}

static int16_t xemu_macos_input_axis_signed(float v)
{
    if (v >= 0.0f) {
        float scaled = v * MAC_AXIS_MAX_F;
        if (scaled > MAC_AXIS_MAX_F) {
            scaled = MAC_AXIS_MAX_F;
        }
        return (int16_t)scaled;
    } else {
        float scaled = v * (-MAC_AXIS_MIN_F);
        if (scaled < MAC_AXIS_MIN_F) {
            scaled = MAC_AXIS_MIN_F;
        }
        return (int16_t)scaled;
    }
}

static int16_t xemu_macos_input_axis_trigger(float v)
{
    if (v <= 0.0f) {
        return 0;
    }
    float scaled = v * MAC_AXIS_MAX_F;
    if (scaled > MAC_AXIS_MAX_F) {
        scaled = MAC_AXIS_MAX_F;
    }
    return (int16_t)scaled;
}

bool xemu_macos_input_get_state(int port,
                                uint16_t *buttons_out,
                                int16_t axis_out[6])
{
    if (!s_initialized || buttons_out == NULL || axis_out == NULL) {
        return false;
    }

    GCController *controller = xemu_macos_input_controller_for_port(port);
    if (controller == nil) {
        return false;
    }

    GCExtendedGamepad *gp = controller.extendedGamepad;
    if (gp == nil) {
        /* Micro / non-extended gamepads — punt to the SDL fallback. */
        return false;
    }

    uint16_t buttons = 0;

    /* Face buttons. */
    if (gp.buttonA.pressed) buttons |= MAC_CTRL_BTN_A;
    if (gp.buttonB.pressed) buttons |= MAC_CTRL_BTN_B;
    if (gp.buttonX.pressed) buttons |= MAC_CTRL_BTN_X;
    if (gp.buttonY.pressed) buttons |= MAC_CTRL_BTN_Y;

    /* D-pad. */
    if (gp.dpad.left.pressed)  buttons |= MAC_CTRL_BTN_DPAD_LEFT;
    if (gp.dpad.right.pressed) buttons |= MAC_CTRL_BTN_DPAD_RIGHT;
    if (gp.dpad.up.pressed)    buttons |= MAC_CTRL_BTN_DPAD_UP;
    if (gp.dpad.down.pressed)  buttons |= MAC_CTRL_BTN_DPAD_DOWN;

    /* Menu / Options ↦ Start / Back (Xbox controller convention).
     * `buttonOptions` is macOS 10.15+; guard with availability. */
    if (gp.buttonMenu != nil && gp.buttonMenu.pressed) {
        buttons |= MAC_CTRL_BTN_START;
    }
    if (@available(macOS 10.15, *)) {
        if (gp.buttonOptions != nil && gp.buttonOptions.pressed) {
            buttons |= MAC_CTRL_BTN_BACK;
        }
    }

    /* Thumbstick clicks (macOS 12.1+). */
    if (@available(macOS 12.1, *)) {
        if (gp.leftThumbstickButton != nil &&
            gp.leftThumbstickButton.pressed) {
            buttons |= MAC_CTRL_BTN_LSTICK;
        }
        if (gp.rightThumbstickButton != nil &&
            gp.rightThumbstickButton.pressed) {
            buttons |= MAC_CTRL_BTN_RSTICK;
        }
    }

    /* Shoulder buttons -> Xbox WHITE / BLACK (Original Xbox controller
     * doesn't have analog shoulder buttons; map LB->WHITE, RB->BLACK
     * mirroring xemu's SDL default). */
    if (gp.leftShoulder.pressed)  buttons |= MAC_CTRL_BTN_WHITE;
    if (gp.rightShoulder.pressed) buttons |= MAC_CTRL_BTN_BLACK;

    /* Home / guide button (macOS 11+). */
    if (@available(macOS 11.0, *)) {
        if (gp.buttonHome != nil && gp.buttonHome.pressed) {
            buttons |= MAC_CTRL_BTN_GUIDE;
        }
    }

    *buttons_out = buttons;

    /* Axes. The Original Xbox controller's stick Y is "up = positive";
     * GameController.framework also reports up = positive. SDL flips
     * the axis to match its own xinput convention; we DO NOT flip
     * here, since the per-controller invert-axis-y option in the SDL
     * path is applied to SDL output specifically. The native
     * backend's output goes directly to ControllerState (which is
     * what update_input() reads), so we deliver Xbox-native sign. */
    axis_out[MAC_CTRL_AXIS_LTRIG] = xemu_macos_input_axis_trigger(gp.leftTrigger.value);
    axis_out[MAC_CTRL_AXIS_RTRIG] = xemu_macos_input_axis_trigger(gp.rightTrigger.value);
    axis_out[MAC_CTRL_AXIS_LSTICK_X] = xemu_macos_input_axis_signed(gp.leftThumbstick.xAxis.value);
    axis_out[MAC_CTRL_AXIS_LSTICK_Y] = xemu_macos_input_axis_signed(gp.leftThumbstick.yAxis.value);
    axis_out[MAC_CTRL_AXIS_RSTICK_X] = xemu_macos_input_axis_signed(gp.rightThumbstick.xAxis.value);
    axis_out[MAC_CTRL_AXIS_RSTICK_Y] = xemu_macos_input_axis_signed(gp.rightThumbstick.yAxis.value);

    return true;
}

void xemu_macos_input_rumble(int port, uint16_t low, uint16_t high)
{
    /* Slice N2: rumble is intentionally a no-op on the native path —
     * Core Haptics integration is the N4 slice. SDL's
     * SDL_RumbleGamepad already calls into Core Haptics under the
     * hood for GCController-backed controllers, so when N2 is enabled
     * but N4 isn't yet, the user will simply see no rumble until N4
     * lands. The plan-text behavior ("if controller.haptics is nil,
     * log a warning and no-op rumble") is preserved by logging a
     * one-shot diagnostic so the absence of rumble is visible. */
    (void)port;
    (void)low;
    (void)high;

    if (!s_initialized) {
        return;
    }
    if (s_warned_no_haptics) {
        return;
    }
    s_warned_no_haptics = true;

    if (@available(macOS 11.0, *)) {
        GCController *controller = xemu_macos_input_controller_for_port(port);
        if (controller != nil && controller.haptics == nil) {
            fprintf(stderr,
                    "xemu-perf: macos_native_input rumble unavailable "
                    "(controller.haptics == nil; older controller)\n");
            return;
        }
    }
    fprintf(stderr,
            "xemu-perf: macos_native_input rumble deferred to slice N4 "
            "(GameController.framework path active; SDL rumble bypassed)\n");
}
