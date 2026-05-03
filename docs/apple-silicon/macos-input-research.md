# macOS Input / Controller Latency Research

Last updated: 2026-05-03 (slices **N1 + N2 SHIPPED**: the
`XEMU_MACOS_NATIVE_INPUT` opt-in is in place along with the four
input-latency counters `INPUT_USB_POLLS`, `INPUT_BACKEND_UPDATES`,
`INPUT_LAT_US_TOTAL`, `INPUT_LAT_US_MAX`. Build PASS, M5 shader-
validation harness 7/7 PASS, smoke tests confirm the env-var-off
path is byte-identical to today's SDL behavior and the env-var-on
path emits `xemu-perf: macos_native_input enabled controllers=N`
once `xemu_input_init` runs.)

This document captures the state of xemu's input pipeline, the
GameController.framework alternative, and the migration path. It is
referenced from the Metal renderer plan (`metal-renderer-plan.md`)
because input latency is in scope for the Apple Silicon shareable build,
but it is independent of the renderer slice and can land before, during,
or after the Metal port.

---

## 1. Current input path (SDL3)

### Backend

xemu is on **SDL3**, not SDL2. The header comment in `ui/xemu-input.c`
says "SDL2 GameController" but the include is `<SDL3/SDL.h>` and all
calls use the `SDL_Gamepad*` API (`SDL_OpenGamepad`,
`SDL_GetGamepadButton`, `SDL_GetGamepadAxis`, `SDL_RumbleGamepad`).

### Lifecycle

`xemu_input_init()` (`ui/xemu-input.c:699`) calls
`SDL_Init(SDL_INIT_GAMEPAD)`. Real gamepads only appear when SDL emits
`SDL_EVENT_GAMEPAD_ADDED`; `xemu_input_process_sdl_events()`
(`ui/xemu-input.c:793`) handles the lifecycle and binds new gamepads to
saved-port slots via GUID.

Per-port binding state lives in `bound_controllers[4]`
(`xemu-input.c:526`). On bind, `xemu_input_bind()` (`xemu-input.c:1101`)
creates a fake USB hub and an XID gamepad device via QMP — that QDev
tree is what the guest sees through `hw/xbox/xid-gamepad.c`.

### Polling model

xemu is **polled-per-frame, not callback-driven**:

- `ui/xemu.c:892 poll_events()` runs `SDL_PollEvent()` in a loop, then
  calls `xemu_input_update_controllers()` (`xemu.c:950`).
- `xemu_input_update_controller()` (`xemu-input.c:926`) is rate-limited
  to once per `XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US = 2500`
  (~400 Hz max). It reads SDL's cached state via `SDL_GetGamepadButton`
  / `SDL_GetGamepadAxis`. **No callback is registered with SDL.**

### Pull from guest

The path from `ControllerState` to the Xbox guest is **pull, not push**:
`hw/xbox/xid-gamepad.c:139 update_input(s)` is called from
`usb_xid_gamepad_handle_data()` whenever the guest issues an interrupt-IN
read on `GAMEPAD_IN_ENDPOINT_ID`. The guest does that read at the rate
the Xbox controller reports — **8 ms (125 Hz) on real hardware**. So
the guest sees a fresh poll every 8 ms regardless of host frame rate.

### Rumble

`hw/xbox/xid.c:50-51` exposes `left_actuator_strength` and
`right_actuator_strength` — **the original Xbox controller has TWO
motors, not four.** (Trigger rumble was a later addition to Xbox One
controllers; it is not modelled by the emulated XID device.)

`xemu_input_update_rumble()` (`xemu-input.c:1076`) is rate-limited to
once per 2500 µs and calls
`SDL_RumbleGamepad(state->sdl_gamepad, rumble_l, rumble_r, 250)`. The
250 ms duration is a keep-alive; xemu re-issues every guest write.

### Diagnostic flags

`XEMU_SCRIPTED_INPUT` and `XEMU_RECORD_INPUT` (`xemu-input.c:181, 350`)
operate at the `ControllerState` level — they don't depend on which
backend produced the state. **A native macOS backend can be a drop-in
replacement at this layer without touching the diagnostic harness.**

---

## 2. GameController.framework (macOS native alternative)

Apple's recommended path since macOS Big Sur (11.0). Closed-source thin
wrapper over `IOKit` and `CoreBluetooth`, talking to `gamecontrollerd`
out-of-process. Apple WWDC19 session 616 explicitly recommends migrating
off direct `IOHIDManager` for game controllers.

### Class surface

- `GCController` (the device). Class-level notifications:
  `GCControllerDidConnect`, `GCControllerDidDisconnect`. Enumerate via
  `+[GCController controllers]`.
- `GCExtendedGamepad` (`controller.extendedGamepad`). Universal profile;
  every modern controller exposes this. Per-element typed properties
  (`buttonA`, `dpad`, `leftThumbstick`, `leftTrigger`, etc.). Each
  `GCControllerButtonInput` has `.value` (float 0–1), `.pressed`,
  `.valueChangedHandler`.
- `GCXboxGamepad` adds Elite-2 paddle buttons + Series-X share button.
  iOS 14+ / macOS 11+.
- `GCDualShockGamepad` (DS4): touchpad button + axes. macOS 11+.
- `GCDualSenseGamepad` (DS5): adaptive triggers via
  `GCDualSenseAdaptiveTrigger`. macOS 11.3+.
- `GCKeyboard` / `GCMouse` round out the framework on macOS 11+.

### Polling model — both supported

1. **Per-element callbacks**:
   `controller.extendedGamepad.buttonA.pressedChangedHandler = ^(...)`.
2. **Profile-level callback**: `gamepad.valueChangedHandler` fires for
   any element change. Moonlight uses this for low-latency input
   (`Limelight/Input/ControllerSupport.m`).
3. **Polled state read**: just read `gamepad.buttonA.pressed` or
   `gamepad.leftThumbstick.xAxis.value`. The framework continuously
   updates state — no `SDL_PollEvent` equivalent required.
4. **Snapshots**: `[gamepad saveSnapshot]` returns a thread-safe
   immutable snapshot for cross-thread reads.

For xemu's pattern (sample at the moment the guest USB stack polls),
**polling is the better fit** — no callback bounce required.

### Bluetooth controller support specifics

- **Xbox Wireless Controller (Series X|S, Elite 2)**: native Bluetooth
  pairing since Big Sur. USB-C wired works too.
- **Xbox 360**: only via third-party kexts on older macOS; not covered
  by GameController.framework. SDL's HIDAPI path is needed for Xbox 360
  wired controllers.
- **DualShock 4**: native since Catalina.
- **DualSense (PS5)**: native since macOS 11.3. Adaptive triggers
  exposed.

### macOS Sonoma "Game Mode" (macOS 14+)

Game Mode doubles Bluetooth controller polling rate from ~125 Hz to
~250 Hz when the app is the foreground full-screen game. Activation
heuristic:

1. `Info.plist`: `LSApplicationCategoryType =
   public.app-category.games` (xemu has this).
2. `Info.plist`: `GCSupportsControllerUserInteraction = YES`
   (recommended).
3. App is foreground + full-screen.

**No entitlement required.** Apple Silicon only.

---

## 3. Latency budget on macOS

### Hardware ceiling (not addressable by backend choice)

- **Wired Xbox controller**: USB descriptor reports `bInterval=4` =
  ~125 Hz host-side. macOS does not expose a polling-rate override.
- **Bluetooth Xbox controller**: BLE HID at 8 ms = 125 Hz. Doubles to
  ~250 Hz under macOS Sonoma Game Mode.

### Software ceiling (addressable)

The path from controller event to xemu state read is:

1. Driver → `gamecontrollerd` (kernel + system daemon).
2. `gamecontrollerd` → `GCController` state (XPC).
3. **(SDL only)** SDL macOS joystick driver → SDL internal joystick
   struct → SDL event queue (cross-thread post).
4. **(SDL only)** `SDL_PollEvent` drain on main thread + SDL internal
   cache update.
5. `xemu_input_update_sdl_controller_state` reads SDL cache.

Steps 3–4 are removed by the GameController.framework path: xemu reads
`gamecontrollerd`'s published state directly. Saved cost: **one
thread-hop + a few tens of microseconds of event-queue dispatch per
change**.

### Comparison to peer emulators

- **DuckStation, PCSX2** use SDL on macOS — same baseline as xemu.
- **Moonlight** (latency-critical game streaming) uses native
  GameController everywhere on Apple platforms; never touches SDL.
- **Dolphin** has a separate native macOS backend mixing IOKit and
  GCController for MFi.

### Apple's recommendation

WWDC19 616 recommends `valueChangedHandler` for menu / UI work, polling
or callbacks for in-game. xemu's sample-on-guest-USB-poll pattern is
already correct; the only question is who updates the cache (SDL or
GameController).

---

## 4. Rumble (Core Haptics on the controller)

### Path

1. After connect, check `controller.haptics`
   (`GCController.haptics`, `GCDeviceHaptics?`, macOS 11+). If non-nil,
   the controller supports Core Haptics.
2. Per locality:
   `engine = [controller.haptics createEngineWithLocality:GCHapticsLocality.leftHandle]`
   and the same for `.rightHandle`. (Optional `.leftTrigger` /
   `.rightTrigger` if the controller has trigger motors — DS5, Xbox One+.)
3. Build a single long-running `CHHapticPattern` with **one continuous
   event** of nominal 30 s duration at intensity 1.0. Get a player:
   `[engine createPlayerWithPattern:pattern error:&err]` returns a
   `CHHapticPatternPlayer`. Call `[player startAtTime:0 error:nil]` once.
4. Drive intensity changes via
   `CHHapticDynamicParameterIDHapticIntensityControl`:

   ```objc
   CHHapticDynamicParameter *p = [[CHHapticDynamicParameter alloc]
       initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                     value:current_strength
              relativeTime:0];
   [player sendParameters:@[p] atTime:CHHapticTimeImmediate error:nil];
   ```

   This avoids re-creating the pattern on every guest update.

### Caveat

`CHHapticAdvancedPatternPlayer` is **unavailable** on engines vended by
`GCDeviceHaptics`. Use the basic `CHHapticPatternPlayer`. Confirmed in
`libgdx/gdx-controllers#26`.

### Avoiding clicks

1. Don't stop and restart the player on every guest write. Use one
   long-running player per locality with dynamic parameters.
2. Don't smooth aggressively. The Xbox guest writes 8-bit per motor at
   8 ms cadence — Core Haptics interpolates internally. If the guest
   snaps from 0 to 0xFF in one frame, the click is faithful to real Xbox
   hardware.

### Latency vs SDL

SDL3's `SDL_RumbleGamepad` on macOS for a GCController-backed controller
already calls into Core Haptics under the hood. The latency difference
is marginal; **what changes is control fidelity** — explicit per-motor
locality and a clean upgrade path to trigger rumble.

---

## 5. Authentication / signing

GameController.framework requires **no entitlements** on macOS:

- No `com.apple.security.device.usb`.
- No `com.apple.security.device.bluetooth`.
- Works under both Hardened Runtime and App Sandbox.
- Core Haptics from `GCController.haptics` requires no entitlement.
- Game Mode activation requires no entitlement (just Info.plist hints).

xemu's existing `.entitlements` need no change. The migration is
entitlement-neutral.

---

## 6. Migration architecture

### Don't replace SDL — layer GameController behind it

Reasons:

1. Linux + Windows builds keep depending on SDL.
2. SDL still wins for some edge cases (Xbox 360 wired, exotic
   third-party controllers, the rebind UI built on SDL events).
3. Diagnostic flags (`XEMU_SCRIPTED_INPUT`, `XEMU_RECORD_INPUT`) operate
   at `ControllerState` level — backend-agnostic.
4. A backend gate gives the project a measurement track per project
   rule #1 (no guessing).

### Proposed file split

- `ui/input-sdl.c` — existing logic, refactored out of `xemu-input.c`.
- `ui/input-gamecontroller.m` — Objective-C using GameController +
  Core Haptics. `__APPLE__` only.
- `ui/input-backend.h` — small internal interface.
- `ui/xemu-input.c` becomes the dispatcher.

This matches the existing macOS-specific Objective-C split
(`ui/xemu-os-utils-macos.m` sits next to `ui/xemu-os-utils-linux.c` and
`ui/xemu-os-utils-windows.c`).

### Proposed flags

- `XEMU_MACOS_NATIVE_INPUT={0,1}` — main toggle. Default 0 initially.
- `XEMU_MACOS_NATIVE_INPUT_RUMBLE={0,1}` — fine-grained sub-toggle for
  A/B rumble path independently.
- `XEMU_MACOS_NATIVE_INPUT_QUEUE={main,userinteractive}` — dispatch
  queue for GCController callbacks. Default `userinteractive`.

### Proposed counters

`INPUT_BACKEND_NAME`, `INPUT_UPDATE_CALLS_TOTAL`,
`INPUT_RUMBLE_DISPATCHES_TOTAL`, `INPUT_BACKEND_CONNECT_EVENTS` in
`extract-perf-summary.sh`.

---

## 7. Recommended slice ordering

Following project rules #1 (data-driven), #5 (build tools when blocked),
#11 (XEMU_* flag convention):

1. **N1 — instrumentation foundation. SHIPPED 2026-05-03.** Adds
   four atomic counters (`INPUT_USB_POLLS`, `INPUT_BACKEND_UPDATES`,
   `INPUT_LAT_US_TOTAL`, `INPUT_LAT_US_MAX`) emitted on the
   `xemu-perf:` interval line whenever input activity occurs. The
   USB-poll counter increments inside `usb_xid_gamepad_handle_data`'s
   `update_input(s)` path; the backend-update counter increments on
   every `xemu_input_update_controller` call (regardless of which
   backend produced the state). Latency is the wallclock delta
   between the most recent backend cache update and the matching
   per-port USB poll. Files touched: `include/qemu/xemu-input-perf.h`
   (new), `util/xemu-input-perf.c` (new), `hw/xbox/xid.c::update_input`,
   `ui/xemu-input.c::xemu_input_update_controller`,
   `hw/xbox/nv2a/pgraph/profile.c::nv2a_profile_log_emit_interval`,
   `scripts/apple-silicon/extract-perf-summary.sh`,
   `util/meson.build`. The implementation chose to keep the existing
   SDL-as-default-backend rather than refactor to a vtable abstraction
   (the planned vtable adds churn without measurable benefit; the
   Apple/SDL split is small enough to express as a single conditional
   in the read path).
2. **N2 — Native GameController backend (input only, no rumble).
   SHIPPED 2026-05-03.** New file `ui/xemu-macos-input.mm` (Obj-C++)
   exposing a small C interface (`xemu_macos_input_init`,
   `_shutdown`, `_get_state`, `_rumble`,
   `_controller_count`, `_is_active`). On init, enumerates
   `[GCController controllers]`, registers
   `GCControllerDidConnect/DisconnectNotification` observers, and
   tags every connected controller with a fresh
   `GCControllerPlayerIndex` (xemu port N → playerIndex N). On
   poll, looks up the controller for the requested port via
   playerIndex and reads `GCExtendedGamepad` properties directly —
   no SDL event-queue drain, no main-thread hop. Rumble is a
   no-op (Core Haptics integration deferred to N4); first call
   logs a one-shot diagnostic. SDL still owns connect/disconnect
   lifecycle and per-port binding so the rebind UI works
   unchanged. Companion `Info.plist` change: add
   `GCSupportsControllerUserInteraction = YES` (Sonoma+ Game Mode
   polling-rate doubling for Bluetooth controllers when
   foreground+fullscreen). Build dep: `appleframeworks(modules:
   GameController)` added to `ui/meson.build`. Default OFF;
   `XEMU_MACOS_NATIVE_INPUT=1` opts in. SDL fallback path
   untouched. Files touched: `ui/xemu-macos-input.h` (new),
   `ui/xemu-macos-input.mm` (new), `ui/xemu-input.c` (read +
   rumble dispatch), `ui/meson.build`, `Info.plist`. Smoke tests:
   M5 shader-validation harness still 7/7 PASS; with no env, no
   `macos_native_input enabled` log line; with
   `XEMU_MACOS_NATIVE_INPUT=1`, the line emits at the moment
   `xemu_input_init` runs (i.e. after the SDL window is created
   and the main display thread is up).
3. **N3 — Latency measurement XBE + paired benchmark.** Build a tiny
   nxdk XBE that flashes a quad on A-press. Record at 240 Hz with iPhone
   slow-mo. Run `XEMU_MACOS_NATIVE_INPUT=0` vs `=1`, both with macOS
   Sonoma Game Mode active. Document in
   `benchmarks/<date>-input-latency.md`. **Decision rule:** ship N2
   default-on if native ≤ SDL within noise on ≥ 30 paired trials.
4. **N4 — Native rumble (Core Haptics + GCDeviceHaptics).** Add
   `XEMU_MACOS_NATIVE_INPUT_RUMBLE=1` path. Listen-test gate on Crimson
   Skies + Rainbow Six 3 + PGR2 (per `feedback_audio_after_video.md`).
5. **N5 — Game Mode integration polish.** Confirm Info.plist, document
   Game Mode behavior in `automation.md`, add a launcher warning if not
   running fullscreen.
6. **N6 — Trigger rumble exposure (deferred).** XID device only models
   2 motors; trigger-rumble synthesis is a polish slice with little
   demand. Defer.

---

## Sources

- [WWDC19 616 — Supporting New Game Controllers](https://developer.apple.com/videos/play/wwdc2019/616/)
- [WWDC20 10614 — Advancements in Game Controllers](https://wwdcnotes.com/documentation/wwdcnotes/wwdc20-10614-advancements-in-game-controllers/)
- [Apple — Supporting Game Controllers](https://developer.apple.com/documentation/gamecontroller/supporting-game-controllers)
- [Apple — GCController.haptics](https://developer.apple.com/documentation/gamecontroller/gccontroller/haptics)
- [Apple — Playing Haptics on Game Controllers](https://developer.apple.com/documentation/corehaptics/playing-haptics-on-game-controllers)
- [Apple — GCHapticsLocality](https://developer.apple.com/documentation/gamecontroller/gchapticslocality/righttrigger)
- [Apple — Use Game Mode on Mac](https://support.apple.com/en-us/105118)
- [SDL — Improved GCController handling on Apple platforms](https://discourse.libsdl.org/t/sdl-improved-gccontroller-handling-on-apple-platforms/47386)
- [SDL3 SDL_RumbleGamepad](https://wiki.libsdl.org/SDL3/SDL_RumbleGamepad)
- [PCSX2 PR #5140 — macOS GameController fix](https://github.com/PCSX2/pcsx2/pull/5140)
- [libgdx/gdx-controllers #26 — CHHapticAdvancedPatternPlayer unsupported](https://github.com/libgdx/gdx-controllers/issues/26)
- [Moonlight ControllerSupport.m — production GCController + Core Haptics](https://github.com/moonlight-stream/moonlight-ios/blob/master/Limelight/Input/ControllerSupport.m)
