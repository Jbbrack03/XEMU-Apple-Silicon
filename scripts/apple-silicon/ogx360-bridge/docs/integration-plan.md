# Integration Plan — Bring-up Checklist (completed 2026-05-10)

This was the original sequence for bringing up the USB-C Pro Micro
replacement and OGX360 bridge end-to-end. Keep it as a recovery/runbook
reference: the bridge is now shipped as of 2026-05-10, with slot 1 on
production master firmware, slot 2 on stock Ryzee119 firmware, Mac-side
byte-exact validation passing, and Xbox-side `controller-readback`
passing via the transition-based validation pattern.

## Pre-flight checks (do these before installing anything)

### 1. Verify the new Pro Micro pinout

Lay the new Pro Micro alongside one of the photos in
`vendor/OGX360/Images/` (or any standard SparkFun Pro Micro reference
photo). The 12-pin side headers should match exactly:

- One side: `TX0 RX1 GND GND D2 D3 D4 D5 D6 D7 D8 D9` (top to bottom
  with USB at top)
- Other side: `RAW GND RST VCC A3 A2 A1 A0 D15 D14 D16 D10`

If your unit has a different layout (sometimes USB-C clones swap pin
ordering or add extra pins), **stop and check the silkscreen carefully
before installing**. A swapped pinout will short rails through the
OGX360 PCB and possibly damage both boards.

Also confirm: 5V variant (16 MHz crystal). The OGX360 firmware assumes
5V/16MHz fuses. If your unit is the 3.3V/8MHz variant, the firmware
will run at half speed and I²C timing will be off — workable for a
bench test but not recommended for shipping.

### 2. Slot 2 firmware backup — SKIPPED

We attempted this and could not trigger Caterina bootloader entry on
slot 2 during the original backup attempt (see README.md "Backup
status" section for the attempts and their failure modes). Accepted
skip: the slave firmware is reproducible from source at `vendor/OGX360/`
(GPL-3.0). In the later 2026-05-09 bring-up, slot 2 was intentionally
reflashed with stock Ryzee119 firmware after a byte-shifted pre-existing
firmware build was isolated.

If you want to retry the backup later (e.g. after consulting Pro Micro
schematics specific to your board's revision), `backup-runbook.md`
still has the full procedure. Otherwise skip ahead.

## Install and bring-up

### 3. Pre-flash the new Pro Micro before soldering (recommended)

Before installing into the OGX360, flash the master firmware while
the new Pro Micro is still loose on the bench. This sidesteps any
OGX360-PCB-level reset-routing issues for the first flash (we already
saw that slot 2's reset path is unreliable for bootloader entry — no
reason to gamble on slot 1's path being different).

- Plug the new Pro Micro directly into the Mac via USB-C.
- Confirm a `/dev/cu.usbmodem*` device appears immediately (factory
  Caterina presents as USB CDC indefinitely until an application is
  flashed).
- Compile + upload:

```sh
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/firmware/master
DEV=$(ls /dev/cu.usbmodem* | head -1)
/opt/homebrew/bin/arduino-cli compile --fqbn arduino:avr:leonardo master.ino
/opt/homebrew/bin/arduino-cli upload --fqbn arduino:avr:leonardo --port "$DEV" master.ino
```

- Watch the LED. Once the application is running, the LED should slow
  blink at ~1 Hz (the firmware's idle heartbeat — no Mac frames yet).
- Confirm the application's CDC endpoint is alive:

```sh
ls /dev/cu.usbmodem*
```

A `/dev/cu.usbmodem*` should still be present (the application also
exposes USB CDC via Arduino's USB stack — that's how the Mac sends
controller frames in production). VID/PID will likely be `0x2341/0x8036`
(Arduino Leonardo) or whatever the clone identifies as.

### 4. Install the now-flashed Pro Micro into slot 1

- Unplug the new Pro Micro from the Mac.
- Power off the OGX360 (unplug any other USB cables).
- Solder the new Pro Micro into the slot 1 footprint. Standard
  through-hole — same difficulty as removing the old one but in
  reverse. Watch the orientation — the USB-C connector should face
  the same direction as the original micro-USB on the now-removed
  Pro Micro.

### 5. Plug slot 2 back in (data cable, not power-only)

With slot 1 still connected to the Mac:

- Plug slot 2's micro-USB into the Xbox controller adapter cable.
- Plug the Xbox controller adapter cable into your real Xbox's
  controller port 1.
- Power on the Xbox.

Slot 2 gets 5V from the Xbox's controller port (just like a real OG
Xbox controller). With the slot 1 master pinging slaves at boot, slot
2's onboard LED should briefly flash (~250 ms) when slot 1 boots —
that's the `0xAA` ping handler in the slave firmware.

The Xbox should detect "controller in port 1" — initially with no
input. The slave is happily emitting neutral controller state because
slot 1 is sending all-zero Duke frames over I²C every 4 ms.

### 6. Identify slot 2's I²C address

Tap the reset button on the new slot 1 master Pro Micro. As it
re-pings each I²C address (1, 2, 3) at boot, watch for slot 2's LED
blink. The blink pattern is short (~250 ms) and identifies which
address the OGX360 PCB has jumpered slot 2 to.

If the blink happens within ~50 ms of slot 1 power-on (master pings
address 1 first), slot 2 is at address **1** — which is the default
the Mac-side script assumes. Done.

If the blink happens after a delay (~150 ms), slot 2 is address 2 or
3. Pass `--port-index 2` (or 3) to `controller-replay-hardware.py`
when running.

### 7. Mac-side smoke test (no Xbox interaction yet)

```sh
# Find slot 1's serial device (slot 2 is XID HID, no /dev entry).
ls /dev/cu.usbmodem*

# Dry-run a small CSV — confirms the script and the firmware are
# talking even before we touch the Xbox.
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/mac-side
.venv/bin/python controller-replay-hardware.py \
  /Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/input-scripts/crimson-skies-smoke.csv \
  --port-index 1 \
  --rate-multiplier 10
```

Expected: the script runs cleanly, prints a JSON summary with
`frames_sent` matching `events_total`, low schedule jitter, and zero
errors. Slot 1's LED should blink rapidly (~20 Hz) during replay
because the firmware switches to "active" heartbeat when frames arrive.

### 8. End-to-end test on the Xbox

Boot the Xbox to the dashboard or a known-input-responsive screen
(UnleashX is fine). Run a short input test:

```sh
# Press A button only, hold for 1 second.
cat > /tmp/single-A.csv <<EOF
0,a,1
1000,a,0
EOF

cd /Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/mac-side
.venv/bin/python controller-replay-hardware.py /tmp/single-A.csv --port-index 1
```

Expected: the Xbox responds to a single A button press as if a
controller was physically pressed — dashboard navigation, menu
selection, etc.

If yes: you have a working Mac → bridge → Xbox controller injection
path. The same path drives any input CSV from
`scripts/apple-silicon/input-scripts/`.

## Troubleshooting

- **Slot 1 doesn't enumerate after flashing the custom firmware.**
  Probably the firmware crashed during init. Double-tap reset on slot
  1 to enter bootloader (presents `/dev/cu.usbmodem*` for 8 s), then
  re-flash. If the crash repeats, look at the firmware compile output
  for warnings.

- **Slot 2 LED doesn't blink at slot 1 boot.** Either slot 2 isn't
  actually at I²C address 1 (try 2 or 3), or the OGX360's I²C bus
  pull-ups are missing. The latter is unlikely if slot 2's firmware
  was working before — but worth checking with a multimeter on SDA/SCL
  (Pro Micro pins D2/D3) for ~5V on each line when slot 1 is powered.

- **Mac-side script runs but no Xbox response.** Two layers to check:
    1. Is slot 1 actually receiving frames? Compile master.ino with
       `-DDEBUG_STATUS_LINES` and the firmware will print frame
       counts every 5 s over CDC. `screen $DEV 115200` to watch.
    2. Is slot 2 actually getting I²C traffic? Reflash slot 2 with a
       version of the slave firmware modified to blink the LED on
       every I²C `onReceive` and watch.

- **`Mass storage device` shows up instead of CDC after flash.** Pro
  Micro USB-C clones occasionally have a hardware bug with the
  Caterina bootloader. Re-flash the bootloader from the Arduino IDE
  via ICSP — out of scope for this runbook.

## Current status after bring-up

The bridge is shipped end-to-end as of 2026-05-10. Slot 1 production
firmware is restored, slot 2 is on stock Ryzee119 firmware, Mac-side
byte-exact bench validation passes, and Xbox-side `controller-readback`
reports the forced target state (`A`, `dpad_right`, `leftx=25000`).

The remaining work is route-level oracle integration, not bridge
bring-up:

- Test against PGR2 / Crimson / Rainbow / SC2 / Halo input scripts and
  measure jitter and reliability over a full route.
- Optionally move the Mac-side script into
  `scripts/apple-silicon/controller-replay-hardware.py` once the
  retail oracle pipeline invokes the hardware backend directly.
