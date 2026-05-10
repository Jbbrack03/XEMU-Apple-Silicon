# OGX360 Bridge — Bring-up Session Results (2026-05-09)

## Summary

The new USB-C Pro Micro arrived 2026-05-09 and was soldered into the
OGX360 slot 1 footprint by the user. Working autonomously from there,
this session brought the bridge from "staged but never run" to
**Mac-side byte-exact validated** (25/25 PASS through slot 2's XID HID
emit) and partially validated Xbox-side during the original session
(controller enumerated with correct VID/PID, but readback XBE reported
zero input). The 2026-05-10 follow-up at the end of this document
resolves that zero-input result as a validation-method false negative
and records an Xbox-side PASS.

The session also discovered and corrected a slot 2 firmware bug: the
slave firmware that shipped on this OGX360 had a non-standard byte
mapping that hard-locked `wButtons` at `0x0014` (= bLength echoed at
the wrong offset). Slot 2 was reflashed with stock Ryzee119 firmware
and now passes byte-exact bench validation.

Xbox-side end-to-end validation (Mac CSV → bridge → Xbox controller
port → nxdk SDL readback) is **resolved as PASS in the 2026-05-10
follow-up**. The original unresolved notes are preserved below as
historical context.

## Slot 1: in-place flash via 1200-baud touch

The user soldered the new Pro Micro before flashing it. Per the
integration plan, this risked OGX360-PCB-level RST routing problems on
the first flash, but the factory Caterina bootloader honors 1200-baud
touch via its USB CDC stack — that path bypasses the physical RST
pin entirely.

```sh
cd firmware/master
arduino-cli compile --fqbn arduino:avr:leonardo master.ino
arduino-cli upload  --fqbn arduino:avr:leonardo --port /dev/cu.usbmodem3101 master.ino
```

Both compile and upload PASS first try. 6596 bytes flash / 478 bytes
RAM. Application boots cleanly and re-enumerates as Arduino Leonardo
(VID 0x2341 / PID 0x8036) with subclass 2 (CDC ACM).

## Slot 2: I²C ACK proves the master path; HID byte-shift bug surfaces

After flashing slot 1, slot 2 was still plugged into the Xbox
controller adapter. To confirm I²C transport:

1. Built `diag/master_i2c_diag/master_i2c_diag.ino` — a master variant
   that echoes per-address ACK counters every 2 s over CDC.
2. Reflashed slot 1 with the diag firmware (1200-baud touch + upload).
3. Read CDC for 6 s.

Output:

```
# boot_ping=0,2,2 tx_status=0,2,2 ack/tx=2428/2428,0/2428,0/2428
# boot_ping=0,2,2 tx_status=0,2,2 ack/tx=2928/2928,0/2928,0/2928
# boot_ping=0,2,2 tx_status=0,2,2 ack/tx=3428/3428,0/3428,0/3428
```

`ack/tx=N/N` for address 1 = 100 % ACK rate. Status code 0 = success.
Slot 2 was responsive at I²C address 1 (matching the OGX360 PCB's
default jumpering of slave 1). Address 2 and 3 NACK as expected (no
slaves present).

## Slot 2: HID readback exposes the byte-shift bug

User moved slot 2 from the Xbox adapter to the Mac (via micro-USB
cable) so the Mac could read slot 2's XID HID interrupt-in endpoint.

**Slot 2 enumerated as VID 0x045E / PID 0x0289** (the standard
Microsoft Xbox Controller signature emitted by the Ryzee119 slave).
Interface class/subclass 0x58 / 0x42 = XID. Endpoint 0x81 IN, 32-byte
maxpacket, bInterval=4 (= 4 ms = 250 Hz).

To prove the master ↔ slave bridge end-to-end on the Mac, used pyusb
to claim slot 2's interface, opened slot 1's CDC port at 115200, and
sent specific frames while a background reader thread polled
slot 2's interrupt-in.

**First test**: send `wButtons=0xAA55, A=0x77, B=0x33, X=0x44, Y=0x55`.
Master's wire-byte echo (via `diag/master_echo`) confirmed the I²C
bytes were exactly:

```
F1 00 14 55 AA 77 33 44 55 00 00 00 00 00 00 00 00 00 00 00 00
   ^^ status, then 20 payload bytes correctly laid out.
```

Slot 2's HID input report (via pyusb interrupt-in **and** GET_REPORT
control transfer) reported:

```
00 14 14 00 AA 77 33 44 55 00 00 00 00 00 00 00 00 00 00 00
   ^  ^  ^  ^^^^^^^^^^^^^^
   |  |  |  master payload[3..18] correctly placed at struct[4..19]
   |  |  master payload[3] (intended wButtons HIGH) at struct[4] (= A)
   |  always zero (struct[3])
   always 0x14 (= bLength echoed at struct[2])
```

**Diagnostic conclusion**: this slave firmware shifts payload[3..18] →
struct[4..19] and hard-locks struct[2..3] at `(0x14, 0x00)`. The Xbox
interprets struct[2..3] as `wButtons` per the OG Duke spec, so it
sees `wButtons = 0x0014` constantly = bits 4 (START) + 2 (DLEFT)
permanently pressed. Digital buttons are completely uncontrollable.

This is **not** the upstream Ryzee119 protocol. Source verification
showed the upstream `i2c_get_data` does plain `for (i = 0; i < rxlen;
i++) rxbuf[i] = Wire.read()` with no shift. The slot 2 we received
was running an older or modified firmware build with non-standard
byte mapping.

## Slot 2 reflash

Cloned upstream Ryzee119 (with submodules) and built the OGX360
target via PlatformIO (installed in `mac-side/.venv/`):

```sh
git clone --depth 1 https://github.com/Ryzee119/OGX360.git vendor/OGX360
cd vendor/OGX360 && git submodule update --init --depth 1 --recursive
cd Firmware
.venv/bin/pio run -e OGX360
# .pio/build/OGX360/firmware.hex = 26060 bytes (90.9 % flash, 83.6 % RAM)
```

To flash slot 2 we needed Caterina bootloader entry, but the slave
firmware has `-DDISABLE_CDC` so 1200-baud touch is not available.
**The user shorted slot 2's RST→GND header pins twice within ~750 ms
to enter the Caterina double-tap bootloader window.** This produced
`/dev/cu.usbmodem8401` (different from slot 1's `/dev/cu.usbmodem3101`)
for ~8 seconds.

`validation/flash-slot2.sh` runs as a watcher: polls `/dev/cu.usbmodem*`
for any device other than slot 1, and immediately runs avrdude with
the avr109 protocol against the new bootloader port.

```
[watcher] FOUND bootloader at /dev/cu.usbmodem8401
[avrdude] flashing...
Writing 26060 bytes to flash
Writing | ################################################## | 100% 2.66s
Reading | ################################################## | 100% 0.81s
26060 bytes of flash verified
[avrdude] exit=0
```

After reflash, slot 2 re-enumerated as VID 0x045E / PID 0x0289 (same
public identity, but now stock Ryzee119 internals).

## Bench validation: 25/25 byte-exact PASS

`validation/bench-validate.py` executes the full Duke protocol
surface against the Mac-attached slot 2:

| # | Category | Tests | Result |
| - | -------- | ----- | ------ |
| 1 | wButtons full bit sweep (8 bits + all-set + reserved high byte + neutral) | 11 | 11 PASS |
| 2 | Analog buttons + triggers (A/B/X/Y/BLACK/WHITE 0xFF, L/R 0x80/0xFF/0x00, cleared) | 4 | 4 PASS |
| 3 | 16-bit sticks at extremes (-32768/32767/midpoint -1/neutral on lstick + rstick) | 5 | 5 PASS |
| 4 | Combo: every field non-zero simultaneously (wB=0xFF, A..R, all 4 sticks at non-zero) | 2 | 2 PASS |
| 5 | Rapid 100-toggle stress @ 100 Hz, then verify final neutral state | 1 | 1 PASS |
| 6 | Real CSV replay (`crimson-skies-smoke.csv`, 38 events at 20× rate) | 1 | 1 PASS, 0 errors |
| 7 | 30 s soak: 234 randomized states, then verify final neutral | 1 | 1 PASS, 0 errors |
| **Total** | | **25** | **25 PASS** |

Every wButtons bit (D-pad up/down/left/right, START, BACK, LS-click,
RS-click) and every analog field byte-exactly matches what the host
intended to send. Slot 2's HID emit is fully under the bridge's
control.

## Xbox-side validation: detection works, input reads zero

User moved slot 2 back to the Xbox controller adapter. Slot 1 stayed
on the Mac via USB-C. Master.ino was re-flashed (the diag firmware
was replaced before the swap).

**Test**: while bridge sender sustains a known controller state
(`wButtons=0x0008` = DRIGHT, `A=0xAA`, `lstickX=+25000`), chainload
the `controller-readback` XBE via FTP `SITE EXEC`. The XBE polls
`SDL_GameController` for ~5 s (300 frames × 16 ms), captures the
final state, writes a key=value report to `D:\controller-readback.txt`,
and reboots.

Result:

```
status=ok
frames=300
has_controller=1
player_index=1
vendor=0x045e
product=0x0289
axis.leftx=0  axis.lefty=0  axis.rightx=0  axis.righty=0
axis.lefttrigger=0  axis.righttrigger=0
button.a=0  button.b=0  button.x=0  button.y=0
button.back=0  button.start=0  button.white=0  button.black=0
button.dpad_up=0  button.dpad_down=0
button.dpad_left=0  button.dpad_right=0
button.leftstick=0  button.rightstick=0
```

SDL **detects** slot 2 (`has_controller=1`, vendor and product
correct, `player_index=1`, `frames=300`) but every axis and button
reads zero. Two replays at different sender rates (50 Hz vs 200 Hz)
and different sender warm-up times (0.5 s vs 3 s) gave the same
result.

## Failed attempt: Xbox kernel RAM signature scan

To bypass the SDL/XBE layer, attempted to chainload the oracle agent
and use `mem.read` to scan kernel RAM for a unique bridge signature
(`wButtons=0x55AA`, `A=0xCC`, sticks with distinctive bytes). The
agent crashed on `mem.read(0x80610000, 65536)` — that range is
outside the agent's RAM allowlist, and the agent died ungracefully
instead of returning a 500 error.

After the crash the Xbox stopped responding to ICMP / FTP / TCP 9001.
A hard power-cycle is required to recover. **Do not repeat this
test until the agent's allowlist + error handling is hardened.**

## Resolved follow-up

The Xbox-side question above is resolved by the 2026-05-10 follow-up
below. The bridge drives the Xbox controller state when validation
forces a fresh input transition after chainload.

## Files added this session

```
ogx360-bridge/
├── diag/                                 NEW
│   ├── master_echo/master_echo.ino       diagnostic: echo every I²C TX
│   └── master_i2c_diag/master_i2c_diag.ino  diagnostic: per-addr ACK
├── docs/2026-05-09-bringup-results.md    NEW (this file)
└── validation/                           NEW
    ├── bench-validate.py                 exhaustive byte-exact bench test
    ├── bridge-readback-test.py           Xbox-side readback driver
    └── flash-slot2.sh                    bootloader watcher + avrdude
```

The vendor/OGX360 clone (with its submodules) is gitignored per
existing policy. The mac-side/.venv install of platformio is also
gitignored.

## 2026-05-10 follow-up: Xbox-side PASS

The 2026-05-09 zero-input Xbox-side result was a false negative in the
validation method, not a bridge failure. The sender held one constant
state before and during chainload; Ryzee119's `XID_::sendReport`
sends an interrupt report only when the report bytes differ from the
slave's local cached copy, so SDL could open after chainload without
receiving a fresh input transition.

Follow-up diagnostics:

- Slot 1 I2C diag firmware reported slot 2 ACKing 100% at I2C address
  1 (`boot_ping=0,2,2`, `tx_status=0,2,2`).
- Slot 1 echo firmware proved exact Mac frame parsing and exact I2C
  transmit bytes, e.g. `F10014AA55CC33445566778899D2042EFBA861589E`.
- Slot 2 GET_REPORT and interrupt reads matched that payload byte for
  byte while attached to the Mac.
- Production `master.ino` was restored and
  `validation/bench-validate.py` passed the expanded suite:
  wButtons, analog buttons/triggers, stick extremes, combo, 100 Hz
  rapid transitions, `crimson-skies-smoke.csv` at 20x, 234 randomized
  soak states, and final neutral.
- With slot 2 moved to the Xbox adapter, the patched
  `validation/bridge-readback-test.py` starts neutral, chainloads
  `controller-readback`, toggles target/neutral for fresh interrupt
  reports, then holds target. The XBE reported:

```text
status=ok
frames=300
has_controller=1
player_index=1
vendor=0x045e
product=0x0289
axis.leftx=25000
button.a=1
button.dpad_right=1
```

Verdict: Tier 3 OGX360 bridge is working end-to-end from Mac serial
frames through the OGX360 I2C bus, slot 2 XID HID, the retail Xbox
controller port, and nxdk SDL controller readback.
