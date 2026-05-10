# OGX360 Bridge — Mac to Xbox Controller Injection

A custom firmware + Mac-side driver that turns a partially-broken
OGX360 (slot 1 desoldered, slot 2 working with the original Ryzee119
slave firmware) plus a single new USB-C Pro Micro into a hardware
bridge for live, Mac-driven Xbox controller injection. This is the
**Tier 3 hardware controller emulator** path documented in
`xemu-fork/docs/apple-silicon/controller-injection-research.md`.

## Architecture

```
Mac Studio                          OGX360 PCB
  +-----+    USB-C        +--------------------+
  | xemu| ------ slot 1 -->| custom master FW    |
  | CSV |                 |   (this project)    |
  +-----+                 |        |            |
                          |        | I2C        |
                          |        v            |
                          |  slot 2 slave FW    |--- USB micro -> Xbox
                          |  (Ryzee119, stock)  |   adapter cable    port 1
                          +--------------------+
```

- **Slot 1 (master role):** custom firmware in `firmware/master/master.ino`.
  Replaces the original Ryzee119 master firmware. Reads framed serial
  packets over USB CDC from the Mac. Forwards each frame's payload to
  slave Pro Micros via the existing OGX360 I²C protocol.
- **Slot 2 (slave role):** unmodified Ryzee119 OGX360 slave firmware,
  the same code that ships from the upstream repo. Reads I²C from the
  master, presents as an OG Xbox Controller (XID HID, VID/PID
  `0x045E:0x0289`) over USB.

## Repository layout

```
ogx360-bridge/
├── README.md                    # this file
├── firmware/
│   ├── master/master.ino        # custom slot 1 firmware (CDC -> I2C master)
│   └── slave-backup/            # *.hex of the existing slot 2 firmware
├── mac-side/
│   ├── controller-replay-hardware.py   # the Mac-side replay tool
│   └── .venv/                   # Python virtualenv with pyserial
├── docs/
│   ├── protocol-analysis.md     # reverse-engineered I2C protocol notes
│   ├── backup-runbook.md        # how to dump slot 2's firmware
│   └── integration-plan.md      # completed bring-up checklist / runbook
└── vendor/
    └── OGX360/                  # cloned reference (GPL-3.0-or-later)
```

## Status

**Bridge SHIPPED 2026-05-10.** Byte-exact validation passes
end-to-end from Mac CSV through slot 2's XID HID emit, and Xbox-side
`controller-readback` now sees the bridge as live controller input
through the retail Xbox controller port. The key Xbox-side fix was
forcing a post-chainload input transition; a constant-held state can
falsely read as zero because the OGX360 slave only emits interrupt
reports when the XID report changes.

- [x] Arduino toolchain installed (`arduino-cli`, `avrdude`)
- [x] Working directory laid out
- [x] Ryzee119 OGX360 source cloned to `vendor/` (pulled with
      submodules 2026-05-09 for the slot 2 reflash)
- [x] PlatformIO installed in `mac-side/.venv/` (used to build the
      stock Ryzee119 firmware for the slot 2 reflash)
- [x] Master ↔ slave I²C protocol reverse-engineered and documented
      (`docs/protocol-analysis.md`)
- [x] Custom slot 1 master firmware written and compile-tested
      (`firmware/master/master.ino` — 23% flash, 18% RAM on ATmega32U4)
- [x] Mac-side replay tool written and unit-tested
      (`mac-side/controller-replay-hardware.py`)
- [x] Backup runbook written (`docs/backup-runbook.md`)
- [x] Integration plan written (`docs/integration-plan.md`)
- [x] **2026-05-09:** Slot 1 (new USB-C Pro Micro) flashed in-place
      via 1200-baud touch + arduino-cli upload. CDC enumerates
      cleanly as Arduino Leonardo at `/dev/cu.usbmodem3101`.
- [x] **2026-05-09:** Slot 2 reflashed with stock Ryzee119 firmware
      (the previous slave firmware had a non-standard byte-shift
      bug that hard-locked `wButtons` at 0x0014). Reflash performed
      via RST→GND header short → Caterina bootloader → avrdude.
      `validation/flash-slot2.sh` is the watcher script that
      caught the bootloader CDC and ran avrdude.
- [x] **2026-05-10:** Bench validation byte-exact PASS through
      slot 2's XID HID emit (every wButtons bit, every analog
      button, both triggers, every stick at extremes, combo, rapid
      100Hz transitions, real CSV replay, 30s soak — see
      `validation/bench-validate.py`).
- [skip] Slot 2 firmware backup — original (broken) firmware not
      preserved; slot 2 is now running stock Ryzee119 which is
      reproducible from `vendor/OGX360/`.
- [x] **2026-05-10:** Xbox-side input validation PASS.
      `validation/bridge-readback-test.py` starts neutral, chainloads
      `controller-readback`, toggles target/neutral for fresh XID
      interrupt reports, then holds target. The XBE reports
      `button.a=1`, `button.dpad_right=1`, and `axis.leftx=25000`
      for slot 2 (`VID 0x045E PID 0x0289`).

## Quick reference — wire format

Mac → slot 1 over USB CDC at 115200 baud, 8N1:

```
[0xAB][0xCD][PORT 1..3][TYPE 1=DUKE][PAYLOAD x 20][CHECKSUM = XOR of bytes 2..23]
```

Total frame: 25 bytes. PAYLOAD is exactly the
`usbd_duke_in_t` struct from Ryzee119's `usbd_xid.h:145-162`.

Slot 1 → slot 2 over I²C at 400 kHz:

```
[STATUS = 0xF1 (DUKE)][PAYLOAD x 20]
```

PAYLOAD is identical to the Mac→slot1 PAYLOAD — slot 1 just unwraps the
serial framing and re-frames for I²C. See `docs/protocol-analysis.md`
for full byte-by-byte details.

## Tonight's autonomous work — what was done

Working autonomously through the user-approved plan, as of 2026-05-08:

1. Installed Homebrew packages: `arduino-cli`, `avrdude`.
2. Installed the Arduino AVR core (provides Leonardo / ATmega32U4
   target for compilation and upload).
3. Cloned `https://github.com/Ryzee119/OGX360` to `vendor/OGX360`.
4. Read the firmware (`main.cpp`, `master.cpp`, `slave.cpp`,
   `usbd/usbd_xid.h`) and produced `docs/protocol-analysis.md` —
   a complete byte-level spec of the master/slave I²C protocol so the
   custom slot 1 firmware can drive the unmodified Ryzee119 slave.
5. Wrote `firmware/master/master.ino` — the custom slot 1 firmware.
   Compiles clean against `arduino:avr:leonardo`.
6. Wrote `mac-side/controller-replay-hardware.py` — a Mac-side replay
   tool that follows the same CSV vocabulary as the existing
   `controller-replay.py` (in `xemu-fork/scripts/apple-silicon/`)
   so the existing input-script library drives both engines.
7. Set up a Python virtualenv at `mac-side/.venv/` with pyserial
   installed (avoids PEP 668 issues with Homebrew Python).
8. Unit-tested the frame builder against four known states (neutral,
   A+start+lstick, dpad+stick-sign, etc.) — all PASS.
9. Smoke-tested the replay script in dry-run against `noop.csv` and a
   real `crimson-skies-smoke.csv` — clean parsing, no errors.
10. Wrote `docs/backup-runbook.md` for the slot 2 firmware backup
    (cannot run autonomously — requires the user to physically tap
    slot 2's reset button to enter the bootloader).
11. Wrote `docs/integration-plan.md` — completed bring-up checklist.

## What needs the user

- **Tomorrow:** when the new Pro Micro arrives, follow
  `docs/integration-plan.md` step-by-step. Estimated 30-60 minutes
  from "Pro Micro arrives" to "Xbox responding to Mac input."
  Recommended order is **flash slot 1 firmware to the new Pro Micro
  while it's still loose on the bench (USB-C → Mac directly), verify
  it enumerates correctly, then solder it into the OGX360 slot 1
  footprint** — this sidesteps any OGX360-PCB-level reset-routing
  issues for the first flash.

## Backup status (skipped — recoverable from source)

We attempted to back up slot 2's existing firmware via avrdude. Three
trigger methods were tried:

1. **OGX360 onboard reset button, double-tap.** No `/dev/cu.usbmodem*`
   appeared. Most likely cause: that button is a power-cycle (cuts
   VBUS), not wired to the chip's RST pin. Power-on resets do not
   trigger Caterina's stay-in-bootloader detection.
2. **Manual short of slot 2's Pro Micro RST → GND header pins, twice.**
   No bootloader appeared. Possible causes: pin labeling on a clone
   board, contact quality, or a non-standard bootloader on this unit.

We chose not to push further to avoid risk of accidentally bridging
RST to VCC instead of GND, which can damage the chip. Slot 2 remains
alive and enumerating as `0x045E:0x0289`.

**Recovery plan if slot 2 is ever bricked.** The slave firmware is
GPL-3.0-or-later open source and is already cloned at
`vendor/OGX360/`. To rebuild + reflash:

```sh
brew install platformio
cd vendor/OGX360/Firmware
pio run -e OGX360 --target upload
```

This uploads via the same Caterina-bootloader path we couldn't trigger
in our backup attempts — but it's only needed if the slave firmware
itself is broken, in which case we'd be debugging the bootloader entry
anyway. Normal route integration does not reflash slot 2, so this is
a future-work recovery consideration rather than a production blocker.

## Xbox-side validation

The Xbox-side proof is:

```sh
scripts/apple-silicon/ogx360-bridge/validation/bridge-readback-test.py \
  --host 192.168.0.200 \
  --device /dev/cu.usbmodem3101
```

Expected final report includes:

```text
has_controller=1
vendor=0x045e
product=0x0289
axis.leftx=25000
button.a=1
button.dpad_right=1
```

Do not validate Xbox-side input by holding one constant state before
chainload. That can produce a false zero-input report because the
Ryzee119 XID code suppresses duplicate interrupt reports. The
readback test intentionally toggles target/neutral after chainload,
then holds the target state so SDL sees a fresh report.

Remaining production follow-ups are integration exercises, not bridge
bring-up blockers:

- Run the canary input scripts (PGR2 / Crimson / Rainbow / SC2 /
  Halo / Burnout 3 / OutRun 2) end-to-end through the bridge and log
  route-level jitter / reliability for the retail-game oracle pipeline.
- Optionally promote `mac-side/controller-replay-hardware.py` to
  `scripts/apple-silicon/controller-replay-hardware.py` once the
  retail pipeline starts invoking the hardware backend directly.

## Recovery — slot 2 reflash procedure (used 2026-05-09)

If slot 2's slave firmware ever needs rebuilding from source:

```sh
cd vendor/OGX360
git submodule update --init --depth 1 --recursive   # first time only
cd Firmware
../../../../mac-side/.venv/bin/pio run -e OGX360
# Output: .pio/build/OGX360/firmware.hex (~26 KB)
```

To flash slot 2: short its RST→GND header pins twice within ~750 ms
(double-tap reset). The slave firmware has `-DDISABLE_CDC` so the
1200-baud-touch path does not work; physical reset is required.
The watcher in `validation/flash-slot2.sh` polls for the bootloader
CDC to appear and runs avrdude immediately when it does.

## Diagnostic + validation scripts

- `validation/bench-validate.py` — expanded byte-exact bench
  validation (slot 2 must be on the Mac via micro-USB). PASS on
  2026-05-10 against production slot 1 + stock slot 2: `wButtons`
  sweep, analog buttons/triggers, stick extremes, combo, 100 Hz rapid
  transition, `crimson-skies-smoke.csv` at 20x, 234 randomized soak
  states, and final neutral.
- `validation/bridge-readback-test.py` — Xbox-side validation driver.
  Starts neutral, chainloads `controller-readback`, toggles
  target/neutral after chainload to force fresh interrupt reports,
  holds target, then FTPs the report back. PASS on 2026-05-10 with
  `has_controller=1`, `vendor=0x045e`, `product=0x0289`,
  `axis.leftx=25000`, `button.a=1`, and `button.dpad_right=1`.
- `validation/flash-slot1.sh` — slot 1 compile/upload helper for
  production or diagnostic master firmware.
- `validation/flash-slot2.sh` — bootloader watcher + avrdude.
- `diag/master_echo/master_echo.ino` — diagnostic master firmware
  variant. Echoes every I²C transmit's exact bytes back over CDC
  so the host can compare claimed-vs-actual wire bytes. Used
  2026-05-09 to confirm `master.ino` writes the correct payload.
- `diag/master_i2c_diag/master_i2c_diag.ino` — diagnostic master
  variant that prints the boot ping ACK status and per-address
  ACK counters at 2 Hz over CDC. Used 2026-05-09 to confirm slot 2
  ACKs 100 % of master transmits at I²C address 1.
