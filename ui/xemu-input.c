/*
 * xemu Input Management
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/timer.h"
#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#endif
#include "qemu/config-file.h"

#include "xemu-input.h"
#include "xemu-notifications.h"
#include "xemu-settings.h"
#include <stdio.h>
#include <stdlib.h>

#include "system/blockdev.h"

// #define DEBUG_INPUT

#ifdef DEBUG_INPUT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US  2500
#define XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US 2500
#define XEMU_INPUT_MIN_BUTTON_HOLD_US            50000

/*
 * Scripted controller input (ported from the Apple Silicon fork).
 * XEMU_SCRIPTED_INPUT=path.csv replays timed controller events into a
 * synthetic pad bound to XEMU_SCRIPTED_INPUT_PORT (default 0). CSV rows:
 *   time_ms,control,value
 * control = a button name (a, b, x, y, dpad_left/up/right/down, start, back,
 * white, black, guide, lstick_btn, rstick_btn) or an axis name
 * (ltrigger, rtrigger, lstick_x, lstick_y, rstick_x, rstick_y); value is
 * 0/1 for buttons and -32768..32767 (0..32767 for triggers) for axes.
 * Purpose on Quest: validate the input->Duke path with no physical pad and
 * drive games into gameplay for framerate measurement.
 */
typedef enum XemuScriptedInputKind {
    XEMU_SCRIPTED_INPUT_BUTTON,
    XEMU_SCRIPTED_INPUT_AXIS,
} XemuScriptedInputKind;

typedef struct XemuScriptedInputEvent {
    int64_t time_us;
    XemuScriptedInputKind kind;
    int index;   /* button mask, or axis index */
    int value;
} XemuScriptedInputEvent;

typedef struct XemuScriptedInput {
    bool enabled;
    int port;
    int64_t start_us;
    size_t next_event;
    GArray *events;
    uint16_t buttons;
    int16_t axis[CONTROLLER_AXIS__COUNT];
    ControllerState *con;   /* synthetic controller we own */
} XemuScriptedInput;

static XemuScriptedInput scripted_input;

/* ---- XR live gamepad forwarding ----------------------------------------
 * In XR/immersive mode the OpenXR NativeActivity (the XR shell) holds Android
 * input focus, not the SDLActivity, so SDL's Android gamepad path never sees a
 * paired Bluetooth controller. The XR shell forwards raw Android gamepad state
 * to the emulator via the exported xemu_xr_set_gamepad_state() below (resolved
 * by dlsym from libxemu.so, mirroring the frame-feed bridge). We own a
 * synthetic pad on port 1 (created only in XR mode when no scripted pad claimed
 * the port) and copy the forwarded state onto it each input update.
 * Thread-safe: the setter runs on the Android input thread, the apply on the
 * emulator input thread. Static-initialized mutex => no init-order race. */
static pthread_mutex_t xr_pad_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool active;                        /* synthetic pad created + bound */
    uint16_t buttons;
    int16_t axis[CONTROLLER_AXIS__COUNT];
    uint16_t rumble_l;
    uint16_t rumble_r;
    ControllerState *con;
} xr_pad;

__attribute__((visibility("default")))
void xemu_xr_set_gamepad_state(uint16_t buttons, const int16_t *axis, int naxis)
{
    pthread_mutex_lock(&xr_pad_lock);
    xr_pad.buttons = buttons;
    for (int i = 0; i < CONTROLLER_AXIS__COUNT && i < naxis; i++) {
        xr_pad.axis[i] = axis[i];
    }
    pthread_mutex_unlock(&xr_pad_lock);
}

/* The XR NativeActivity, rather than SDLActivity, owns the Bluetooth gamepad
 * while immersive. Publish the guest's two Xbox motor strengths so the shell
 * can drive that same Android InputDevice's vibrator. */
__attribute__((visibility("default")))
uint32_t xemu_xr_get_gamepad_rumble(void)
{
    pthread_mutex_lock(&xr_pad_lock);
    uint32_t packed = (uint32_t)xr_pad.rumble_l |
                      ((uint32_t)xr_pad.rumble_r << 16);
    pthread_mutex_unlock(&xr_pad_lock);
    return packed;
}

typedef struct XemuInputButtonName { const char *name; int mask; } XemuInputButtonName;
typedef struct XemuInputAxisName { const char *name; int index; } XemuInputAxisName;

static const XemuInputButtonName xemu_input_button_names[] = {
    { "a", CONTROLLER_BUTTON_A }, { "b", CONTROLLER_BUTTON_B },
    { "x", CONTROLLER_BUTTON_X }, { "y", CONTROLLER_BUTTON_Y },
    { "dpad_left", CONTROLLER_BUTTON_DPAD_LEFT },
    { "dpad_up", CONTROLLER_BUTTON_DPAD_UP },
    { "dpad_right", CONTROLLER_BUTTON_DPAD_RIGHT },
    { "dpad_down", CONTROLLER_BUTTON_DPAD_DOWN },
    { "back", CONTROLLER_BUTTON_BACK }, { "start", CONTROLLER_BUTTON_START },
    { "white", CONTROLLER_BUTTON_WHITE }, { "black", CONTROLLER_BUTTON_BLACK },
    { "lstick_btn", CONTROLLER_BUTTON_LSTICK },
    { "rstick_btn", CONTROLLER_BUTTON_RSTICK },
    { "guide", CONTROLLER_BUTTON_GUIDE },
};
static const XemuInputAxisName xemu_input_axis_names[] = {
    { "ltrigger", CONTROLLER_AXIS_LTRIG }, { "rtrigger", CONTROLLER_AXIS_RTRIG },
    { "lstick_x", CONTROLLER_AXIS_LSTICK_X }, { "lstick_y", CONTROLLER_AXIS_LSTICK_Y },
    { "rstick_x", CONTROLLER_AXIS_RSTICK_X }, { "rstick_y", CONTROLLER_AXIS_RSTICK_Y },
};

static bool xemu_scripted_input_parse_control(const char *name,
                                              XemuScriptedInputKind *kind,
                                              int *index)
{
    for (size_t i = 0; i < ARRAY_SIZE(xemu_input_button_names); i++) {
        if (strcmp(name, xemu_input_button_names[i].name) == 0) {
            *kind = XEMU_SCRIPTED_INPUT_BUTTON;
            *index = xemu_input_button_names[i].mask;
            return true;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(xemu_input_axis_names); i++) {
        if (strcmp(name, xemu_input_axis_names[i].name) == 0) {
            *kind = XEMU_SCRIPTED_INPUT_AXIS;
            *index = xemu_input_axis_names[i].index;
            return true;
        }
    }
    return false;
}

static int xemu_scripted_input_event_compare(const void *a, const void *b)
{
    int64_t ta = ((const XemuScriptedInputEvent *)a)->time_us;
    int64_t tb = ((const XemuScriptedInputEvent *)b)->time_us;
    return (ta > tb) - (ta < tb);
}

static void xemu_scripted_input_load(void)
{
    const char *path = getenv("XEMU_SCRIPTED_INPUT");
    if (!path || !path[0]) {
        return;
    }
    const char *port_env = getenv("XEMU_SCRIPTED_INPUT_PORT");
    scripted_input.port = port_env ? atoi(port_env) : 0;
    if (scripted_input.port < 0 || scripted_input.port > 3) {
        scripted_input.port = 0;
    }

    gchar *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &contents, &len, &err)) {
        fprintf(stderr, "xemu: failed to read scripted input '%s': %s\n",
                path, err ? err->message : "?");
        if (err) g_error_free(err);
        return;
    }
    scripted_input.events = g_array_new(FALSE, FALSE,
                                        sizeof(XemuScriptedInputEvent));
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (int li = 0; lines[li]; li++) {
        gchar *line = g_strstrip(lines[li]);
        if (line[0] == '\0' || line[0] == '#') continue;
        gchar **tok = g_strsplit(line, ",", 3);
        int nt = 0; while (tok[nt]) nt++;
        if (nt < 3) { g_strfreev(tok); continue; }
        XemuScriptedInputEvent ev;
        ev.time_us = (int64_t)(g_ascii_strtoll(g_strstrip(tok[0]), NULL, 10)) * 1000;
        XemuScriptedInputKind kind; int index;
        if (!xemu_scripted_input_parse_control(g_strstrip(tok[1]), &kind, &index)) {
            fprintf(stderr, "xemu: scripted input unknown control '%s'\n", tok[1]);
            g_strfreev(tok); continue;
        }
        int value = (int)g_ascii_strtoll(g_strstrip(tok[2]), NULL, 10);
        ev.kind = kind; ev.index = index; ev.value = value;
        g_array_append_val(scripted_input.events, ev);
        g_strfreev(tok);
    }
    g_strfreev(lines);
    g_free(contents);

    if (scripted_input.events->len == 0) {
        g_array_unref(scripted_input.events);
        scripted_input.events = NULL;
        return;
    }
    qsort(scripted_input.events->data, scripted_input.events->len,
          sizeof(XemuScriptedInputEvent), xemu_scripted_input_event_compare);
    scripted_input.enabled = true;
    scripted_input.start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "scripted input: %u events from %s for port %d",
                        scripted_input.events->len, path, scripted_input.port + 1);
#endif
    fprintf(stderr, "xemu: loaded %u scripted input events from '%s' port %d\n",
            scripted_input.events->len, path, scripted_input.port + 1);
}

static void xemu_scripted_input_apply(ControllerState *state)
{
    if (!scripted_input.enabled || state->bound != scripted_input.port) {
        return;
    }
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t elapsed = now - scripted_input.start_us;
    while (scripted_input.next_event < scripted_input.events->len) {
        XemuScriptedInputEvent *ev = &g_array_index(
            scripted_input.events, XemuScriptedInputEvent,
            scripted_input.next_event);
        if (ev->time_us > elapsed) break;
        if (ev->kind == XEMU_SCRIPTED_INPUT_BUTTON) {
            if (ev->value) scripted_input.buttons |= ev->index;
            else scripted_input.buttons &= ~ev->index;
        } else {
            scripted_input.axis[ev->index] = (int16_t)ev->value;
        }
        scripted_input.next_event++;
    }
    /* s43 diag (env-gated, cold default): log button-mask edges actually
     * applied to the emulated pad, to confirm scripted presses reach the
     * guest controller state. */
    static int diag = -1;
    if (diag < 0) {
        const char *e = getenv("XEMU_INPUT_DIAG");
        diag = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (diag && scripted_input.buttons != state->buttons) {
        fprintf(stderr, "xemu-inputdiag: t=%lldms buttons 0x%04x -> 0x%04x "
                "(A=%d B=%d START=%d)\n",
                (long long)(elapsed / 1000), state->buttons,
                scripted_input.buttons,
                !!(scripted_input.buttons & CONTROLLER_BUTTON_A),
                !!(scripted_input.buttons & CONTROLLER_BUTTON_B),
                !!(scripted_input.buttons & CONTROLLER_BUTTON_START));
    }
    state->buttons = scripted_input.buttons;
    memcpy(state->axis, scripted_input.axis, sizeof(state->axis));
}

#if 0
static void xemu_input_print_controller_state(ControllerState *state)
{
    DPRINTF("     A = %d,      B = %d,     X = %d,     Y = %d\n"
           "  Left = %d,     Up = %d, Right = %d,  Down = %d\n"
           "  Back = %d,  Start = %d, White = %d, Black = %d\n"
           "Lstick = %d, Rstick = %d, Guide = %d\n"
           "\n"
           "LTrig   = %.3f, RTrig   = %.3f\n"
           "LStickX = %.3f, RStickX = %.3f\n"
           "LStickY = %.3f, RStickY = %.3f\n\n",
        !!(state->buttons & CONTROLLER_BUTTON_A),
        !!(state->buttons & CONTROLLER_BUTTON_B),
        !!(state->buttons & CONTROLLER_BUTTON_X),
        !!(state->buttons & CONTROLLER_BUTTON_Y),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_LEFT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_UP),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_RIGHT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_DOWN),
        !!(state->buttons & CONTROLLER_BUTTON_BACK),
        !!(state->buttons & CONTROLLER_BUTTON_START),
        !!(state->buttons & CONTROLLER_BUTTON_WHITE),
        !!(state->buttons & CONTROLLER_BUTTON_BLACK),
        !!(state->buttons & CONTROLLER_BUTTON_LSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_RSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_GUIDE),
        state->axis[CONTROLLER_AXIS_LTRIG],
        state->axis[CONTROLLER_AXIS_RTRIG],
        state->axis[CONTROLLER_AXIS_LSTICK_X],
        state->axis[CONTROLLER_AXIS_RSTICK_X],
        state->axis[CONTROLLER_AXIS_LSTICK_Y],
        state->axis[CONTROLLER_AXIS_RSTICK_Y]
        );
}
#endif

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);
ControllerState *bound_controllers[4] = { NULL, NULL, NULL, NULL };
const char *bound_drivers[4] = { DRIVER_DUKE, DRIVER_DUKE, DRIVER_DUKE,
                                 DRIVER_DUKE };
int test_mode;

static ControllerState *xemu_input_find_sdl_controller(SDL_JoystickID id)
{
    ControllerState *iter;

    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        if (iter->type == INPUT_DEVICE_SDL_GAMECONTROLLER &&
            iter->sdl_joystick_id == id) {
            return iter;
        }
    }

    return NULL;
}

static int xemu_input_sdl_button_to_button_id(uint8_t button)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
        return 0;
    case SDL_CONTROLLER_BUTTON_B:
        return 1;
    case SDL_CONTROLLER_BUTTON_X:
        return 2;
    case SDL_CONTROLLER_BUTTON_Y:
        return 3;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return 4;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return 5;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return 6;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return 7;
    case SDL_CONTROLLER_BUTTON_BACK:
        return 8;
    case SDL_CONTROLLER_BUTTON_START:
        return 9;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return 10;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return 11;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return 12;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return 13;
    case SDL_CONTROLLER_BUTTON_GUIDE:
        return 14;
    default:
        return -1;
    }
}

static const char **port_index_to_settings_key_map[] = {
    &g_config.input.bindings.port1,
    &g_config.input.bindings.port2,
    &g_config.input.bindings.port3,
    &g_config.input.bindings.port4,
};

static const char **port_index_to_driver_settings_key_map[] = {
    &g_config.input.bindings.port1_driver,
    &g_config.input.bindings.port2_driver,
    &g_config.input.bindings.port3_driver, 
    &g_config.input.bindings.port4_driver
};

static int *peripheral_types_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_type_0,
      &g_config.input.peripherals.port1.peripheral_type_1 },
    { &g_config.input.peripherals.port2.peripheral_type_0,
      &g_config.input.peripherals.port2.peripheral_type_1 },
    { &g_config.input.peripherals.port3.peripheral_type_0,
      &g_config.input.peripherals.port3.peripheral_type_1 },
    { &g_config.input.peripherals.port4.peripheral_type_0,
      &g_config.input.peripherals.port4.peripheral_type_1 }
};

static const char **peripheral_params_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_param_0,
      &g_config.input.peripherals.port1.peripheral_param_1 },
    { &g_config.input.peripherals.port2.peripheral_param_0,
      &g_config.input.peripherals.port2.peripheral_param_1 },
    { &g_config.input.peripherals.port3.peripheral_param_0,
      &g_config.input.peripherals.port3.peripheral_param_1 },
    { &g_config.input.peripherals.port4.peripheral_param_0,
      &g_config.input.peripherals.port4.peripheral_param_1 }
};

int *g_keyboard_scancode_map[25] = {
    &g_config.input.keyboard_controller_scancode_map.a,
    &g_config.input.keyboard_controller_scancode_map.b,
    &g_config.input.keyboard_controller_scancode_map.x,
    &g_config.input.keyboard_controller_scancode_map.y,
    &g_config.input.keyboard_controller_scancode_map.back,
    &g_config.input.keyboard_controller_scancode_map.guide,
    &g_config.input.keyboard_controller_scancode_map.start,
    &g_config.input.keyboard_controller_scancode_map.lstick_btn,
    &g_config.input.keyboard_controller_scancode_map.rstick_btn,
    &g_config.input.keyboard_controller_scancode_map.white,
    &g_config.input.keyboard_controller_scancode_map.black,
    &g_config.input.keyboard_controller_scancode_map.dpad_up,
    &g_config.input.keyboard_controller_scancode_map.dpad_down,
    &g_config.input.keyboard_controller_scancode_map.dpad_left,
    &g_config.input.keyboard_controller_scancode_map.dpad_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_up,
    &g_config.input.keyboard_controller_scancode_map.lstick_left,
    &g_config.input.keyboard_controller_scancode_map.lstick_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_down,
    &g_config.input.keyboard_controller_scancode_map.ltrigger,
    &g_config.input.keyboard_controller_scancode_map.rstick_up,
    &g_config.input.keyboard_controller_scancode_map.rstick_left,
    &g_config.input.keyboard_controller_scancode_map.rstick_right,
    &g_config.input.keyboard_controller_scancode_map.rstick_down,
    &g_config.input.keyboard_controller_scancode_map.rtrigger,
};

static void check_and_reset_in_range(int *btn, int min, int max,
                                     const char *message)
{
    if (*btn < min || *btn >= max) {
        fprintf(stderr, "%s\n", message);
        *btn = min;
    }
}

static void xemu_input_bindings_set_in_range(ControllerState *con)
{
    if (!con->controller_map) {
        return;
    }

#define CHECK_RESET_BUTTON(btn)                                            \
    check_and_reset_in_range(&con->controller_map->controller_mapping.btn, \
                             SDL_CONTROLLER_BUTTON_INVALID,                \
                             SDL_CONTROLLER_BUTTON_MAX,                    \
                             "Invalid entry for button " #btn ", resetting")

    CHECK_RESET_BUTTON(a);
    CHECK_RESET_BUTTON(b);
    CHECK_RESET_BUTTON(x);
    CHECK_RESET_BUTTON(y);
    CHECK_RESET_BUTTON(dpad_left);
    CHECK_RESET_BUTTON(dpad_up);
    CHECK_RESET_BUTTON(dpad_right);
    CHECK_RESET_BUTTON(dpad_down);
    CHECK_RESET_BUTTON(back);
    CHECK_RESET_BUTTON(start);
    CHECK_RESET_BUTTON(lshoulder);
    CHECK_RESET_BUTTON(rshoulder);
    CHECK_RESET_BUTTON(lstick_btn);
    CHECK_RESET_BUTTON(rstick_btn);
    CHECK_RESET_BUTTON(guide);

#undef CHECK_RESET_BUTTON

#define CHECK_RESET_AXIS(axis)                                              \
    check_and_reset_in_range(&con->controller_map->controller_mapping.axis, \
                             SDL_CONTROLLER_AXIS_INVALID,                   \
                             SDL_CONTROLLER_AXIS_MAX,                       \
                             "Invalid entry for button " #axis ", resetting")

    CHECK_RESET_AXIS(axis_trigger_left);
    CHECK_RESET_AXIS(axis_trigger_right);
    CHECK_RESET_AXIS(axis_left_x);
    CHECK_RESET_AXIS(axis_left_y);
    CHECK_RESET_AXIS(axis_right_x);
    CHECK_RESET_AXIS(axis_right_y);

#undef CHECK_RESET_AXIS
}

static void xemu_input_bindings_reload_map(ControllerState *con)
{
    assert(con->type == INPUT_DEVICE_SDL_GAMECONTROLLER);

    char guid[35] = { 0 };
    SDL_JoystickGetGUIDString(con->sdl_joystick_guid, guid, sizeof(guid));
    bool added_mapping =
        xemu_settings_load_gamepad_mapping(guid, &con->controller_map);
    if (!con->controller_map) {
        fprintf(stderr, "Failed to load gamepad mapping for %s\n", guid);
        return;
    }

    if (!added_mapping) {
        return;
    }

    // If this controller did not exist in the mapping array, the config will
    // have been reallocated. Any gamepad mapping pointers for other controllers
    // are now invalid, and need to be reloaded.
    ControllerState *iter, *next;
    bool iter_is_new_mapping;
    QTAILQ_FOREACH_SAFE (iter, &available_controllers, entry, next) {
        if (iter == con || iter->type != INPUT_DEVICE_SDL_GAMECONTROLLER) {
            continue;
        }

        memset(guid, 0, sizeof(guid));
        SDL_JoystickGetGUIDString(iter->sdl_joystick_guid, guid, sizeof(guid));

        iter_is_new_mapping =
            xemu_settings_load_gamepad_mapping(guid, &iter->controller_map);
        assert(!iter_is_new_mapping &&
               "Existing controller GUIDs should exist in the config");

        xemu_input_bindings_set_in_range(iter);
    }
}

static const char *get_bound_driver(int port)
{
    assert(port >= 0 && port <= 3);
    const char *driver = *port_index_to_driver_settings_key_map[port];

    // If the driver in the config is NULL, empty, or unrecognized 
    // then default to DRIVER_DUKE
    if (driver == NULL)
        return DRIVER_DUKE;
    if (strlen(driver) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_DUKE) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_S) == 0)
        return DRIVER_S;

    return DRIVER_DUKE;
}

static const int port_map[4] = { 3, 4, 1, 2 };

/* Xbox Live Communicator (voice chat). Requested by the launcher before
 * input init; attached to a free expansion slot of the player's controller
 * hub after XMUs claim theirs. The usb-xblc device opens its host audio
 * streams only when the guest activates voice, so an idle communicator
 * costs no capture or playback resources. */
static bool xemu_voice_chat_requested;

void xemu_input_set_voice_chat(bool enable)
{
    /* Static-library link anchors: both the usb-xblc device model and the
     * QEMU SDL audio driver are registered only by their type_init/module
     * constructors, which a static link happily drops without a symbol
     * reference from linked code. */
    extern void xemu_force_xblc_link(void);
    xemu_force_xblc_link();
#ifdef __ANDROID__
    /* The aaudio SDL driver anchor exists only in the Android build. */
    extern void xemu_android_force_sdlaudio_link(void);
    xemu_android_force_sdlaudio_link();
#endif

    xemu_voice_chat_requested = enable;
}

bool xemu_input_get_voice_chat(void)
{
    return xemu_voice_chat_requested;
}

/* dlsym bridge for the XR shell (mirrors xemu_xr_set_gamepad_state). */
__attribute__((visibility("default")))
int xemu_xr_get_voice_chat_enabled(void)
{
    return xemu_voice_chat_requested;
}

static void xemu_input_attach_xblc(int player_index)
{
    if (!xemu_voice_chat_requested) {
        return;
    }
    assert(player_index >= 0 && player_index < 4);
    ControllerState *player = bound_controllers[player_index];
    if (!player) {
        return;
    }

    static const int slot_port_map[2] = { 2, 3 };
    int slot = -1;
    for (int i = 0; i < 2; i++) {
        if (player->peripheral_types[i] == PERIPHERAL_NONE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        fprintf(stderr,
                "xemu: voice chat requested but both expansion slots of "
                "player %d are occupied\n", player_index + 1);
        return;
    }

    QDict *qdict = qdict_new();
    qdict_put_str(qdict, "driver", "usb-xblc");
    char *id = g_strdup_printf("xblc_%d", player_index);
    qdict_put_str(qdict, "id", id);
    g_free(id);
    char *port = g_strdup_printf("1.%d.%d", port_map[player_index],
                                 slot_port_map[slot]);
    qdict_put_str(qdict, "port", port);
    g_free(port);

    Error *err = NULL;
    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict,
                                          &err);
    DeviceState *dev = NULL;
    if (opts) {
        dev = qdev_device_add(opts, &err);
    }
    qobject_unref(qdict);
    if (!dev) {
        fprintf(stderr, "xemu: failed to attach communicator: %s\n",
                err ? error_get_pretty(err) : "unknown error");
        error_free(err);
        return;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "voice chat: communicator attached (player %d "
                        "slot %c)", player_index + 1, 'A' + slot);
#endif
}

void xemu_input_init(void)
{
    if (g_config.input.background_input_capture) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "Failed to initialize SDL gamecontroller subsystem: %s\n", SDL_GetError());
        exit(1);
    }

    // Create the keyboard input (always first)
    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_SDL_KEYBOARD;
    new_con->name = "Keyboard";
    new_con->bound = -1;
    new_con->peripheral_types[0] = PERIPHERAL_NONE;
    new_con->peripheral_types[1] = PERIPHERAL_NONE;
    new_con->peripherals[0] = NULL;
    new_con->peripherals[1] = NULL;

    for (int i = 0; i < 25; i++) {
        static const char *format_str =
            "WARNING: Keyboard controller map scancode out of range "
            "(%d) : Disabled\n";
        char buf[128];
        snprintf(buf, sizeof(buf), format_str, i);
        check_and_reset_in_range(g_keyboard_scancode_map[i],
                                 SDL_SCANCODE_UNKNOWN, SDL_NUM_SCANCODES, buf);
    }

    bound_drivers[0] = get_bound_driver(0);
    bound_drivers[1] = get_bound_driver(1);
    bound_drivers[2] = get_bound_driver(2);
    bound_drivers[3] = get_bound_driver(3);

    // Check to see if we should auto-bind the keyboard
    int port = xemu_input_get_controller_default_bind_port(new_con, 0);
    if (port >= 0) {
        xemu_input_bind(port, new_con, 0);
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
        xemu_queue_notification(buf);
        xemu_input_rebind_xmu(port);
    }

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);

    /* Scripted input: create a synthetic pad bound to the target port so
     * games can be driven with no physical controller (validation + gameplay
     * framerate measurement). Runs even if a real pad connects later. */
    xemu_scripted_input_load();
    if (scripted_input.enabled) {
        ControllerState *sc = malloc(sizeof(ControllerState));
        memset(sc, 0, sizeof(ControllerState));
        sc->type = INPUT_DEVICE_SDL_KEYBOARD; /* no SDL handle needed */
        sc->name = "Scripted";
        sc->bound = -1;
        sc->peripheral_types[0] = PERIPHERAL_NONE;
        sc->peripheral_types[1] = PERIPHERAL_NONE;
        scripted_input.con = sc;
        QTAILQ_INSERT_TAIL(&available_controllers, sc, entry);
        xemu_input_bind(scripted_input.port, sc, 0);
        xemu_input_rebind_xmu(scripted_input.port);
        xemu_input_attach_xblc(scripted_input.port);
        fprintf(stderr, "xemu: scripted controller bound to port %d\n",
                scripted_input.port + 1);
    }

    /* XR live gamepad: in immersive mode SDL can't see a paired pad (the XR
     * NativeActivity holds focus), so create a synthetic pad on port 0 that the
     * XR shell drives via xemu_xr_set_gamepad_state(). Only when no scripted pad
     * already owns the port. */
    if (!scripted_input.enabled && !xr_pad.active &&
        getenv("XEMU_ANDROID_XR_MODE")) {
        ControllerState *xc = malloc(sizeof(ControllerState));
        memset(xc, 0, sizeof(ControllerState));
        xc->type = INPUT_DEVICE_SDL_KEYBOARD; /* no SDL handle needed */
        xc->name = "XR Gamepad";
        xc->bound = -1;
        xc->peripheral_types[0] = PERIPHERAL_NONE;
        xc->peripheral_types[1] = PERIPHERAL_NONE;
        pthread_mutex_lock(&xr_pad_lock);
        xr_pad.con = xc;
        xr_pad.active = true;
        pthread_mutex_unlock(&xr_pad_lock);
        QTAILQ_INSERT_TAIL(&available_controllers, xc, entry);
        xemu_input_bind(0, xc, 0);
        xemu_input_rebind_xmu(0);
        xemu_input_attach_xblc(0);
        fprintf(stderr, "xemu: XR gamepad synthetic pad bound to port 1\n");
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "XR gamepad forwarding active (port 1)");
#endif
    }
}

int xemu_input_get_controller_default_bind_port(ControllerState *state, int start)
{
    char guid[35] = { 0 };
    if (state->type == INPUT_DEVICE_SDL_GAMECONTROLLER) {
        SDL_JoystickGetGUIDString(state->sdl_joystick_guid, guid, sizeof(guid));
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        snprintf(guid, sizeof(guid), "keyboard");
    }

    for (int i = start; i < 4; i++) {
        const char *binding = *port_index_to_settings_key_map[i];
        if (binding == NULL || binding[0] == '\0') {
            continue;
        }
        if (strcmp(guid, binding) == 0) {
            return i;
        }
    }

    return -1;
}

void xemu_save_peripheral_settings(int player_index, int peripheral_index,
                                   int peripheral_type,
                                   const char *peripheral_parameter)
{
    int *peripheral_type_ptr =
        peripheral_types_settings_map[player_index][peripheral_index];
    const char **peripheral_param_ptr =
        peripheral_params_settings_map[player_index][peripheral_index];

    assert(peripheral_type_ptr);
    assert(peripheral_param_ptr);

    *peripheral_type_ptr = peripheral_type;
    xemu_settings_set_string(
        peripheral_param_ptr,
        peripheral_parameter == NULL ? "" : peripheral_parameter);
}

void xemu_input_process_sdl_events(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERDEVICEADDED) {
        DPRINTF("Controller Added: %d\n", event->cdevice.which);

        // Attempt to open the added controller
        SDL_GameController *sdl_con;
        sdl_con = SDL_GameControllerOpen(event->cdevice.which);
        if (sdl_con == NULL) {
            DPRINTF("Could not open joystick %d as a game controller\n", event->cdevice.which);
            return;
        }

        // Success! Create a new node to track this controller and continue init
        ControllerState *new_con = malloc(sizeof(ControllerState));
        memset(new_con, 0, sizeof(ControllerState));
        new_con->type                 = INPUT_DEVICE_SDL_GAMECONTROLLER;
        new_con->name                 = SDL_GameControllerName(sdl_con);
        new_con->sdl_gamecontroller   = sdl_con;
        new_con->sdl_joystick         = SDL_GameControllerGetJoystick(new_con->sdl_gamecontroller);
        new_con->sdl_joystick_id      = SDL_JoystickInstanceID(new_con->sdl_joystick);
        new_con->sdl_joystick_guid    = SDL_JoystickGetGUID(new_con->sdl_joystick);
        new_con->bound                = -1;
        new_con->peripheral_types[0] = PERIPHERAL_NONE;
        new_con->peripheral_types[1] = PERIPHERAL_NONE;
        new_con->peripherals[0] = NULL;
        new_con->peripherals[1] = NULL;

        char guid_buf[35] = { 0 };
        SDL_JoystickGetGUIDString(new_con->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
        DPRINTF("Opened %s (%s)\n", new_con->name, guid_buf);

        QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
        xemu_input_bindings_reload_map(new_con);

        // Do not replace binding for a currently bound device. In the case that
        // the same GUID is specified multiple times, on different ports, allow
        // any available port to be bound.
        //
        // This can happen naturally with X360 wireless receiver, in which each
        // controller gets the same GUID (go figure). We cannot remember which
        // controller is which in this case, but we can try to tolerate this
        // situation by binding to any previously bound port with this GUID. The
        // upside in this case is that a person can use the same GUID on all
        // ports and just needs to bind to the receiver and never needs to hit
        // this dialog.


        // Attempt to re-bind to port previously bound to
        int port = 0;
        bool did_bind = false;
        while (!did_bind) {
            port = xemu_input_get_controller_default_bind_port(new_con, port);
            if (port < 0) {
                // No (additional) default mappings
                break;
            } else if (!xemu_input_get_bound(port)) {
                xemu_input_bind(port, new_con, 0);
                did_bind = true;
                break;
            } else {
                // Try again for another port
                port++;
            }
        }

        // Try to bind to any open port, and if so remember the binding
        if (!did_bind && g_config.input.auto_bind) {
            for (port = 0; port < 4; port++) {
                if (!xemu_input_get_bound(port)) {
                    xemu_input_bind(port, new_con, 1);
                    did_bind = true;
                    break;
                }
            }
        }

        if (did_bind) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
            xemu_queue_notification(buf);
            xemu_input_rebind_xmu(port);
        }
    } else if (event->type == SDL_CONTROLLERBUTTONDOWN) {
        ControllerState *state =
            xemu_input_find_sdl_controller(event->cbutton.which);
        int button_id =
            xemu_input_sdl_button_to_button_id(event->cbutton.button);

        if (state && button_id >= 0) {
            state->button_hold_until_us[button_id] =
                qemu_clock_get_us(QEMU_CLOCK_REALTIME) +
                XEMU_INPUT_MIN_BUTTON_HOLD_US;
        }
    } else if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        DPRINTF("Controller Removed: %d\n", event->cdevice.which);
        int handled = 0;
        ControllerState *iter, *next;
        QTAILQ_FOREACH_SAFE(iter, &available_controllers, entry, next) {
            if (iter->type != INPUT_DEVICE_SDL_GAMECONTROLLER) continue;

            if (iter->sdl_joystick_id == event->cdevice.which) {
                DPRINTF("Device removed: %s\n", iter->name);

                // Disconnect
                if (iter->bound >= 0) {
                    // Queue a notification to inform user controller disconnected
                    // FIXME: Probably replace with a callback registration thing,
                    // but this works well enough for now.
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Port %d disconnected", iter->bound+1);
                    xemu_queue_notification(buf);

                    // Unbind the controller, but don't save the unbinding in
                    // case the controller is reconnected
                    xemu_input_bind(iter->bound, NULL, 0);
                }

                // Unlink
                QTAILQ_REMOVE(&available_controllers, iter, entry);

                // Deallocate
                if (iter->sdl_gamecontroller) {
                    SDL_GameControllerClose(iter->sdl_gamecontroller);
                }

                for (int i = 0; i < 2; i++) {
                    if (iter->peripherals[i])
                        g_free(iter->peripherals[i]);
                }
                free(iter);

                handled = 1;
                break;
            }
        }
        if (!handled) {
            DPRINTF("Could not find handle for joystick instance\n");
        }
    } else if (event->type == SDL_CONTROLLERDEVICEREMAPPED) {
        DPRINTF("Controller Remapped: %d\n", event->cdevice.which);
    }
}

void xemu_input_update_controller(ControllerState *state)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_input_updated_ts) <
        XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US) {
        return;
    }

    /* Scripted pad overrides real input for its bound port. */
    if (scripted_input.enabled && state == scripted_input.con) {
        xemu_scripted_input_apply(state);
        state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        return;
    }

    /* XR-forwarded live gamepad drives its synthetic pad. */
    if (xr_pad.active && state == xr_pad.con) {
        pthread_mutex_lock(&xr_pad_lock);
        state->buttons = xr_pad.buttons;
        memcpy(state->axis, xr_pad.axis, sizeof(state->axis));
        pthread_mutex_unlock(&xr_pad_lock);
        state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        return;
    }

    if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_input_update_sdl_kbd_controller_state(state);
    } else if (state->type == INPUT_DEVICE_SDL_GAMECONTROLLER) {
        xemu_input_update_sdl_controller_state(state);
    }

    state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

void xemu_input_update_controllers(void)
{
    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_controller(iter);
    }
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_rumble(iter);
    }
}

void xemu_input_update_sdl_kbd_controller_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    const uint8_t *kbd = SDL_GetKeyboardState(NULL);

#define KBD_STATE(btn) \
    (kbd[g_config.input.keyboard_controller_scancode_map.btn])

    state->buttons |= KBD_STATE(a) << 0;
    state->buttons |= KBD_STATE(b) << 1;
    state->buttons |= KBD_STATE(x) << 2;
    state->buttons |= KBD_STATE(y) << 3;
    state->buttons |= KBD_STATE(dpad_left) << 4;
    state->buttons |= KBD_STATE(dpad_up) << 5;
    state->buttons |= KBD_STATE(dpad_right) << 6;
    state->buttons |= KBD_STATE(dpad_down) << 7;
    state->buttons |= KBD_STATE(back) << 8;
    state->buttons |= KBD_STATE(start) << 9;
    state->buttons |= KBD_STATE(white) << 10;
    state->buttons |= KBD_STATE(black) << 11;
    state->buttons |= KBD_STATE(lstick_btn) << 12;
    state->buttons |= KBD_STATE(rstick_btn) << 13;
    state->buttons |= KBD_STATE(guide) << 14;

    if (KBD_STATE(lstick_up))
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = 32767;
    if (KBD_STATE(lstick_left))
        state->axis[CONTROLLER_AXIS_LSTICK_X] = -32768;
    if (KBD_STATE(lstick_right))
        state->axis[CONTROLLER_AXIS_LSTICK_X] = 32767;
    if (KBD_STATE(lstick_down))
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = -32768;
    if (KBD_STATE(ltrigger))
        state->axis[CONTROLLER_AXIS_LTRIG] = 32767;

    if (KBD_STATE(rstick_up))
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = 32767;
    if (KBD_STATE(rstick_left))
        state->axis[CONTROLLER_AXIS_RSTICK_X] = -32768;
    if (KBD_STATE(rstick_right))
        state->axis[CONTROLLER_AXIS_RSTICK_X] = 32767;
    if (KBD_STATE(rstick_down))
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = -32768;
    if (KBD_STATE(rtrigger))
        state->axis[CONTROLLER_AXIS_RTRIG] = 32767;

#undef KBD_STATE
}

void xemu_input_update_sdl_controller_state(ControllerState *state)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));
    if (!state->controller_map) {
        return;
    }

#define SDL_MASK_BUTTON(state, btn, idx)                  \
    (SDL_GameControllerGetButton(                         \
         (state)->sdl_gamecontroller,                     \
         (state)->controller_map->controller_mapping.btn) \
     << idx)

    state->buttons |= SDL_MASK_BUTTON(state, a, 0);
    state->buttons |= SDL_MASK_BUTTON(state, b, 1);
    state->buttons |= SDL_MASK_BUTTON(state, x, 2);
    state->buttons |= SDL_MASK_BUTTON(state, y, 3);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_left, 4);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_up, 5);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_right, 6);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_down, 7);
    state->buttons |= SDL_MASK_BUTTON(state, back, 8);
    state->buttons |= SDL_MASK_BUTTON(state, start, 9);
    state->buttons |= SDL_MASK_BUTTON(state, lshoulder, 10);
    state->buttons |= SDL_MASK_BUTTON(state, rshoulder, 11);
    state->buttons |= SDL_MASK_BUTTON(state, lstick_btn, 12);
    state->buttons |= SDL_MASK_BUTTON(state, rstick_btn, 13);
    state->buttons |= SDL_MASK_BUTTON(state, guide, 14);

#undef SDL_MASK_BUTTON

#define SDL_GET_AXIS(state, axis)    \
    SDL_GameControllerGetAxis(       \
        (state)->sdl_gamecontroller, \
        (state)->controller_map->controller_mapping.axis)

    state->axis[0] = SDL_GET_AXIS(state, axis_trigger_left);
    state->axis[1] = SDL_GET_AXIS(state, axis_trigger_right);
    state->axis[2] = SDL_GET_AXIS(state, axis_left_x);
    state->axis[3] = SDL_GET_AXIS(state, axis_left_y);
    state->axis[4] = SDL_GET_AXIS(state, axis_right_x);
    state->axis[5] = SDL_GET_AXIS(state, axis_right_y);

#undef SDL_GET_AXIS

// FIXME: Check range
#define INVERT_AXIS(controller_axis) \
    state->axis[controller_axis] = -1 - state->axis[controller_axis]

    if (state->controller_map->controller_mapping.invert_axis_left_x) {
        INVERT_AXIS(CONTROLLER_AXIS_LSTICK_X);
    }

    if (!state->controller_map->controller_mapping.invert_axis_left_y) {
        INVERT_AXIS(CONTROLLER_AXIS_LSTICK_Y);
    }

    if (state->controller_map->controller_mapping.invert_axis_right_x) {
        INVERT_AXIS(CONTROLLER_AXIS_RSTICK_X);
    }

    if (!state->controller_map->controller_mapping.invert_axis_right_y) {
        INVERT_AXIS(CONTROLLER_AXIS_RSTICK_Y);
    }

#undef INVERT_AXIS

    for (int i = 0; i < 15; i++) {
        if (state->button_hold_until_us[i] > now) {
            state->buttons |= CONTROLLER_STATE_BUTTON_ID_TO_MASK(i);
        } else {
            state->button_hold_until_us[i] = 0;
        }
    }

    // xemu_input_print_controller_state(state);
}

void xemu_input_update_rumble(ControllerState *state)
{
#ifdef __ANDROID__
    if (state == xr_pad.con) {
        pthread_mutex_lock(&xr_pad_lock);
        if (g_config.input.allow_vibration) {
            xr_pad.rumble_l = state->rumble_l;
            xr_pad.rumble_r = state->rumble_r;
        } else {
            xr_pad.rumble_l = 0;
            xr_pad.rumble_r = 0;
        }
        pthread_mutex_unlock(&xr_pad_lock);
        return;
    }
#endif
    if (state->type != INPUT_DEVICE_SDL_GAMECONTROLLER) {
        return;
    }

    if (!state->controller_map) {
        return;
    }

    if (!state->controller_map->enable_rumble) {
        return;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_rumble_updated_ts) <
        XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US) {
        return;
    }

    SDL_GameControllerRumble(state->sdl_gamecontroller, state->rumble_l, state->rumble_r, 250);
    state->last_rumble_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

ControllerState *xemu_input_get_bound(int index)
{
    return bound_controllers[index];
}

void xemu_input_bind(int index, ControllerState *state, int save)
{
    // FIXME: Attempt to disable rumble when unbinding so it's not left
    // in rumble mode

    // Unbind existing controller
    if (bound_controllers[index]) {
        assert(bound_controllers[index]->device != NULL);
        Error *err = NULL;

        // Unbind any XMUs
        for (int i = 0; i < 2; i++) {
            if (bound_controllers[index]->peripherals[i]) {
                // If this was an XMU, unbind the XMU
                if (bound_controllers[index]->peripheral_types[i] ==
                    PERIPHERAL_XMU)
                    xemu_input_unbind_xmu(index, i);

                // Free up the XmuState and set the peripheral type to none
                g_free(bound_controllers[index]->peripherals[i]);
                bound_controllers[index]->peripherals[i] = NULL;
                bound_controllers[index]->peripheral_types[i] = PERIPHERAL_NONE;
            }
        }

        qdev_unplug((DeviceState *)bound_controllers[index]->device, &err);
        assert(err == NULL);

        bound_controllers[index]->bound = -1;
        bound_controllers[index]->device = NULL;
        bound_controllers[index] = NULL;
    }

    // Save this controller's GUID in settings for auto re-connect
    if (save) {
        char guid_buf[35] = { 0 };
        if (state) {
            if (state->type == INPUT_DEVICE_SDL_GAMECONTROLLER) {
                SDL_JoystickGetGUIDString(state->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
            } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
                snprintf(guid_buf, sizeof(guid_buf), "keyboard");
            }
        }
        xemu_settings_set_string(port_index_to_settings_key_map[index], guid_buf);
        xemu_settings_set_string(port_index_to_driver_settings_key_map[index],
                                 bound_drivers[index]);
    }

    // Bind new controller
    if (state) {
        if (state->bound >= 0) {
            // Device was already bound to another port. Unbind it.
            xemu_input_bind(state->bound, NULL, 1);
        }

        bound_controllers[index] = state;
        bound_controllers[index]->bound = index;

        char *tmp;

        // Create controller's internal USB hub.
        QDict *usbhub_qdict = qdict_new();
        qdict_put_str(usbhub_qdict, "driver", "usb-hub");
        tmp = g_strdup_printf("1.%d", port_map[index]);
        qdict_put_str(usbhub_qdict, "port", tmp);
        qdict_put_int(usbhub_qdict, "ports", 3);
        QemuOpts *usbhub_opts = qemu_opts_from_qdict(qemu_find_opts("device"), usbhub_qdict, &error_abort);
        DeviceState *usbhub_dev = qdev_device_add(usbhub_opts, &error_abort);
        g_free(tmp);

        // Create XID controller. This is connected to Port 1 of the controller's internal USB Hub
        QDict *qdict = qdict_new();

        // Specify device driver
        qdict_put_str(qdict, "driver", bound_drivers[index]);

        // Specify device identifier
        static int id_counter = 0;
        tmp = g_strdup_printf("gamepad_%d", id_counter++);
        qdict_put_str(qdict, "id", tmp);
        g_free(tmp);

        // Specify index/port
        qdict_put_int(qdict, "index", index);
        tmp = g_strdup_printf("1.%d.1", port_map[index]);
        qdict_put_str(qdict, "port", tmp);
        g_free(tmp);

        // Create the device
        QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict, &error_abort);
        DeviceState *dev = qdev_device_add(opts, &error_abort);
        assert(dev);

        // Unref for eventual cleanup
        qobject_unref(usbhub_qdict);
        object_unref(OBJECT(usbhub_dev));
        qobject_unref(qdict);
        object_unref(OBJECT(dev));

        state->device = usbhub_dev;
    }
}

bool xemu_input_bind_xmu(int player_index, int expansion_slot_index,
                         const char *filename, bool is_rebind)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *player = bound_controllers[player_index];
    enum peripheral_type peripheral_type =
        player->peripheral_types[expansion_slot_index];
    if (peripheral_type != PERIPHERAL_XMU)
        return false;

    XmuState *xmu = (XmuState *)player->peripherals[expansion_slot_index];

    // Unbind existing XMU
    if (xmu->dev != NULL) {
        xemu_input_unbind_xmu(player_index, expansion_slot_index);
    }

    if (filename == NULL)
        return false;

    // Look for any other XMUs that are using this file, and unbind them
    for (int player_i = 0; player_i < 4; player_i++) {
        ControllerState *state = bound_controllers[player_i];
        if (state != NULL) {
            for (int peripheral_i = 0; peripheral_i < 2; peripheral_i++) {
                if (state->peripheral_types[peripheral_i] == PERIPHERAL_XMU) {
                    XmuState *xmu_i =
                        (XmuState *)state->peripherals[peripheral_i];
                    assert(xmu_i);

                    if (xmu_i->filename != NULL &&
                        strcmp(xmu_i->filename, filename) == 0) {
                        char *buf =
                            g_strdup_printf("This XMU is already mounted on "
                                            "player %d slot %c\r\n",
                                            player_i + 1, 'A' + peripheral_i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                        return false;
                    }
                }
            }
        }
    }

    xmu->filename = g_strdup(filename);

    const int xmu_map[2] = { 2, 3 };
    char *tmp;

    static int id_counter = 0;
    tmp = g_strdup_printf("xmu_%d", id_counter++);

    // Add the file as a drive
    QDict *qdict1 = qdict_new();
    qdict_put_str(qdict1, "id", tmp);
    qdict_put_str(qdict1, "format", "raw");
    qdict_put_str(qdict1, "file", filename);

    QemuOpts *drvopts =
        qemu_opts_from_qdict(qemu_find_opts("drive"), qdict1, &error_abort);

    DriveInfo *dinfo = drive_new(drvopts, 0, &error_abort);
    assert(dinfo);

    // Create the usb-storage device
    QDict *qdict2 = qdict_new();

    // Specify device driver
    qdict_put_str(qdict2, "driver", "usb-storage");

    // Specify device identifier
    qdict_put_str(qdict2, "drive", tmp);
    g_free(tmp);

    // Specify index/port
    tmp = g_strdup_printf("1.%d.%d", port_map[player_index],
                          xmu_map[expansion_slot_index]);
    qdict_put_str(qdict2, "port", tmp);
    g_free(tmp);

    // Create the device
    QemuOpts *opts =
        qemu_opts_from_qdict(qemu_find_opts("device"), qdict2, &error_abort);

    DeviceState *dev = qdev_device_add(opts, &error_abort);
    assert(dev);

    xmu->dev = (void *)dev;

    // Unref for eventual cleanup
    qobject_unref(qdict1);
    qobject_unref(qdict2);

    if (!is_rebind) {
        xemu_save_peripheral_settings(player_index, expansion_slot_index,
                                      peripheral_type, xmu->filename);
    }

    return true;
}

void xemu_input_unbind_xmu(int player_index, int expansion_slot_index)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *state = bound_controllers[player_index];
    if (state->peripheral_types[expansion_slot_index] != PERIPHERAL_XMU)
        return;

    XmuState *xmu = (XmuState *)state->peripherals[expansion_slot_index];
    if (xmu != NULL) {
        if (xmu->dev != NULL) {
            qdev_unplug((DeviceState *)xmu->dev, &error_abort);
            object_unref(OBJECT(xmu->dev));
            xmu->dev = NULL;
        }

        g_free((void *)xmu->filename);
        xmu->filename = NULL;
    }
}

void xemu_input_rebind_xmu(int port)
{
    // Try to bind peripherals back to controller
    for (int i = 0; i < 2; i++) {
        enum peripheral_type peripheral_type =
            (enum peripheral_type)(*peripheral_types_settings_map[port][i]);

        // If peripheralType is out of range, change the settings for this
        // controller and peripheral port to default
        if (peripheral_type < PERIPHERAL_NONE ||
            peripheral_type >= PERIPHERAL_TYPE_COUNT) {
            xemu_save_peripheral_settings(port, i, PERIPHERAL_NONE, NULL);
            peripheral_type = PERIPHERAL_NONE;
        }

        const char *param = *peripheral_params_settings_map[port][i];

        if (peripheral_type == PERIPHERAL_XMU) {
            if (param != NULL && strlen(param) > 0) {
                // This is an XMU and needs to be bound to this controller
                if (qemu_access(param, R_OK | W_OK) == 0) {
                    bound_controllers[port]->peripheral_types[i] =
                        peripheral_type;
                    bound_controllers[port]->peripherals[i] =
                        g_malloc(sizeof(XmuState));
                    memset(bound_controllers[port]->peripherals[i], 0,
                           sizeof(XmuState));
                    bool did_bind = xemu_input_bind_xmu(port, i, param, true);
                    if (did_bind) {
                        char *buf =
                            g_strdup_printf("Connected XMU %s to port %d%c",
                                            param, port + 1, 'A' + i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                    }
                } else {
                    char *buf =
                        g_strdup_printf("Unable to bind XMU at %s to port %d%c",
                                        param, port + 1, 'A' + i);
                    xemu_queue_error_message(buf);
                    g_free(buf);
                }
            }
        }
    }
}

void xemu_input_set_test_mode(int enabled)
{
    test_mode = enabled;
}

int xemu_input_get_test_mode(void)
{
    return test_mode;
}

void xemu_input_reset_input_mapping(ControllerState *state)
{
    if (state->type == INPUT_DEVICE_SDL_GAMECONTROLLER) {
        char guid[35] = { 0 };
        SDL_JoystickGetGUIDString(state->sdl_joystick_guid, guid, sizeof(guid));
        xemu_settings_reset_controller_mapping(guid);
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_settings_reset_keyboard_mapping();
    }
}
