# OGX360 Master/Slave Protocol — Reverse-Engineered Notes

Source: cloned 2026-05-08 from https://github.com/Ryzee119/OGX360
(GPL-3.0-or-later). This document summarizes the on-the-wire I²C
protocol between the master Pro Micro (slot 1) and the slave Pro Micros
(slots 2-4) of an OGX360, derived from a direct read of the firmware in
`vendor/OGX360/Firmware/src/`. We need this so our custom slot 1 firmware
can drive the *unmodified* Ryzee119 slave firmware on slot 2.

## Hardware role detection (`main.cpp:39`)

Each Pro Micro reads `PLAYER_ID1_PIN=19` and `PLAYER_ID2_PIN=20` at boot
(both `INPUT_PULLUP`):

```
player_id = (digitalRead(PLAYER_ID1_PIN) << 1) | digitalRead(PLAYER_ID2_PIN);
```

The OGX360 PCB hard-codes these two pins per slot via solder jumpers to
GND or open. Default (open) reads HIGH (1). Grounded reads LOW (0).

| `player_id` | Role     | I²C address |
| ----------- | -------- | ----------- |
| `0b00`      | Master   | (master, no fixed addr) |
| `0b01`      | Slave 1  | 1           |
| `0b10`      | Slave 2  | 2           |
| `0b11`      | Slave 3  | 3           |

Slot 1 = master, Slot 2 = Slave 1 (= I²C address 1), Slot 3 = Slave 2,
Slot 4 = Slave 3.

The slave's `Wire.begin(slave_id)` is called with the player_id value
directly (`slave.cpp:74-75`). So the I²C address equals `player_id`.

## I²C bus parameters

| Parameter | Value |
| --------- | ----- |
| Clock     | 400 kHz (`Wire.setClock(400000)`) |
| Timeout   | 4000 µs (`Wire.setWireTimeout(4000, true)`) |
| Pull-ups  | ATmega32U4 internal pull-ups via `Wire.begin()` (no externals on the OGX360) |

The pull-ups come from the master's `Wire.begin()`. Slaves call
`Wire.begin(addr)` which also engages internal pull-ups. With *no*
master present, only the slave-side pull-ups are active — sufficient for
the slave firmware to enumerate over USB but no I²C traffic flows.

## Master → Slave packet format

`master.cpp:140-180`. For each slave `i` in `[1, MAX_GAMEPADS)`:

```
Wire.beginTransmission(i);
Wire.write(status);        // 1 byte
Wire.write(tx_buff, tx_len); // payload bytes
Wire.endTransmission(true);
```

### Status byte

```
status = 0xF0 | type
```

where `type` is `xid_type_t`:

| `type` | meaning           | byte value  |
| ------ | ----------------- | ----------- |
| `0x00` | DISCONNECTED      | `0xF0`      |
| `0x01` | DUKE              | `0xF1`      |
| `0x02` | STEELBATTALION    | `0xF2`      |

Special: `0xAA` is a "ping" (no payload) — slave responds by blinking
its LED for 250ms (`slave.cpp:17-23`). Used by master at boot to
confirm slaves are present (`master.cpp:59-66`).

### Payload (DUKE)

`sizeof(usbd_duke_in_t)` = **20 bytes**, packed little-endian
(`usbd_xid.h:145-162`):

| Offset | Type    | Field         | Notes |
| ------ | ------- | ------------- | ----- |
| 0      | u8      | `startByte`   | Always `0x00` (set in `main.cpp:23` memset) |
| 1      | u8      | `bLength`     | Always `0x14` (= 20, set in `main.cpp:28`) |
| 2-3    | u16 LE  | `wButtons`    | Bitmask, see below |
| 4      | u8      | `A`           | 0..255 (analog button) |
| 5      | u8      | `B`           | 0..255 |
| 6      | u8      | `X`           | 0..255 |
| 7      | u8      | `Y`           | 0..255 |
| 8      | u8      | `BLACK`       | 0..255 (right shoulder, analog) |
| 9      | u8      | `WHITE`       | 0..255 (left shoulder, analog) |
| 10     | u8      | `L`           | 0..255 (left trigger, analog) |
| 11     | u8      | `R`           | 0..255 (right trigger, analog) |
| 12-13  | i16 LE  | `leftStickX`  | -32768..32767 |
| 14-15  | i16 LE  | `leftStickY`  | -32768..32767 |
| 16-17  | i16 LE  | `rightStickX` | -32768..32767 |
| 18-19  | i16 LE  | `rightStickY` | -32768..32767 |

Note: original Microsoft Duke convention has analog buttons (A/B/X/Y/BLACK/WHITE)
as 0..255 representing pressure. For oracle replay we typically set them
to 0 or 0xFF only (binary). The Xbox treats anything ≥0x40 as "pressed."

### `wButtons` bitmask (`usbd_xid.h:25-32`)

| Bit | Constant      | Meaning |
| --- | ------------- | ------- |
| 0   | `DUKE_DUP`    | D-pad up |
| 1   | `DUKE_DDOWN`  | D-pad down |
| 2   | `DUKE_DLEFT`  | D-pad left |
| 3   | `DUKE_DRIGHT` | D-pad right |
| 4   | `DUKE_START`  | Start |
| 5   | `DUKE_BACK`   | Back |
| 6   | `DUKE_LS`     | Left stick click |
| 7   | `DUKE_RS`     | Right stick click |
| 8-15 | (reserved)   | |

## Slave → Master response (rumble)

After every transmission the master issues `Wire.requestFrom(i, rx_len)`
(`master.cpp:163-173`). For DUKE, `rx_len = sizeof(usbd_duke_out_t) = 6`:

| Offset | Type   | Field      |
| ------ | ------ | ---------- |
| 0      | u8     | `startByte` |
| 1      | u8     | `bLength`   |
| 2-3    | u16 LE | `lValue`   (low rumble motor) |
| 4-5    | u16 LE | `hValue`   (high rumble motor) |

For the oracle use case the Mac doesn't need rumble feedback — the bridge
firmware can ignore the response (read and discard) or skip
`requestFrom` entirely. The slave's `onRequest` handler (`slave.cpp:55-70`)
sends a single 0x00 byte if no controller type is set, so requesting is
safe even in degenerate states.

## Master → Slave timing

Master polls everything every loop iteration. The original firmware also
sends a USB report to its own port every 4 ms (`main.cpp:84-108`). For
our bridge there's no own-port USB device, so the natural cadence is
just to mirror that 4 ms timer for I²C writes — gives the slave fresh
state at 250 Hz which is well above the 125 Hz Xbox poll rate.

## What we replicate vs. don't

Our custom slot 1 firmware needs to:

1. **Replicate** the master → slave I²C protocol exactly — the slave
   firmware is unmodified and expects this format.
2. **Replace** the input source: instead of reading USB controllers via
   the MAX3421E host shield (the original firmware's job), we read frames
   from the Mac over USB CDC serial.
3. **Skip** entirely:
   - MAX3421E init (`master_init():38-56`) — no host-shield to talk to.
   - EEPROM SB sensitivity (`master_init():76-86`) — no SB use case here.
   - The own-port XID HID code (`main.cpp:79-108`) — slot 1 isn't an
     Xbox-facing controller in our design; only slot 2 is.
   - Steel Battalion handling — we only support DUKE for now. SB later
     if needed; the protocol slot is ready (`type = 0x02`).

## Slot 2 jumpering on this user's OGX360

Confirmed empirically: with slot 2's Pro Micro powered (LED steady) it
enumerates over USB as VID `0x045E` PID `0x0289` — the OG Xbox Controller
S signature emitted by the slave firmware. So slot 2's `player_id` reads
as a non-zero value (slave role), and its I²C address is one of 1, 2, 3.

The OGX360 PCB jumpers determine which slave address slot 2 has been
assigned. Most likely slot 2 = address 1 (the natural sequential
assignment), but we verify this at integration time by sending pings to
addresses 1, 2, 3 and watching for the slave LED to blink.

## License note

Ryzee119's OGX360 is GPL-3.0-or-later. Our custom slot 1 firmware (which
does not include any of his code, only re-implements the I²C protocol it
documents) is independently authored. We do not redistribute the slave
firmware in this repo — `vendor/OGX360/` is a clone retained as a
reference, and we depend on the user installing the original slave
firmware to slot 2 via the standard Ryzee119 release path. That keeps the
GPL boundary clean.
