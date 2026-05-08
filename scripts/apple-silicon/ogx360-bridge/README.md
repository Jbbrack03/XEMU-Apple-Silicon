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
│   └── integration-plan.md      # tomorrow's bring-up checklist
└── vendor/
    └── OGX360/                  # cloned reference (GPL-3.0-or-later)
```

## Status

- [x] Arduino toolchain installed (`arduino-cli`, `avrdude`)
- [x] Working directory laid out
- [x] Ryzee119 OGX360 source cloned to `vendor/`
- [x] Master ↔ slave I²C protocol reverse-engineered and documented
      (`docs/protocol-analysis.md`)
- [x] Custom slot 1 master firmware written and compile-tested
      (`firmware/master/master.ino` — 23% flash, 18% RAM on ATmega32U4)
- [x] Mac-side replay tool written and unit-tested
      (`mac-side/controller-replay-hardware.py`)
- [x] Backup runbook written (`docs/backup-runbook.md`)
- [x] Integration plan written (`docs/integration-plan.md`)
- [skip] Slot 2 firmware backup — Caterina bootloader entry on slot 2
      could not be triggered (OGX360 onboard reset is power-cycle, not
      RST-pin; manual RST/GND short on the Pro Micro pin header also
      did not activate the bootloader within multiple attempts).
      Acceptable: the slave firmware is reproducible from
      `vendor/OGX360/` source via PlatformIO; tomorrow's plan never
      reflashes slot 2 anyway. See "Backup status" section below.
- [ ] **Pending hardware:** new USB-C Pro Micro arrival (tomorrow)
- [ ] **Pending hardware:** install + flash + end-to-end Xbox test

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
11. Wrote `docs/integration-plan.md` — tomorrow's bring-up checklist.

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
anyway. Tomorrow's integration never reflashes slot 2, so this is a
future-work consideration rather than a tomorrow's-blocker.

## Next session — once the bridge is working

- Move `mac-side/controller-replay-hardware.py` into
  `xemu-fork/scripts/apple-silicon/`.
- Update `xemu-fork/docs/apple-silicon/controller-injection-research.md`
  to mark Tier 3 as **shipped** with this implementation.
- Add a decision-log entry recording the integration date and any
  measured jitter / reliability numbers from the first full route
  replay through the bridge.
- Test against the canary input scripts (PGR2, Crimson, Rainbow, SC2,
  Halo) to confirm the bridge is production-grade for the oracle
  pipeline as a fallback when the Tier 2A per-title patching path
  stalls.
