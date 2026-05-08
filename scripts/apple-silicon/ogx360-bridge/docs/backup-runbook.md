# Slot 2 Firmware Backup Runbook

Backup procedure for the Pro Micro currently in slot 2 of the OGX360.
The chip is running the unmodified Ryzee119 OGX360 slave firmware. We
want a hex dump of its flash so the OGX360 can be restored to factory
state if anything during development bricks slot 2.

**Cannot be run autonomously** — backup requires physically tapping
the reset button on slot 2's Pro Micro to enter the Caterina bootloader
(the application firmware does not expose USB CDC, so avrdude has no
way to talk to the chip until the bootloader runs). The commands below
are ready to copy/paste once you're ready.

## Why bootloader mode

The Ryzee119 slave firmware is built with `-DDISABLE_CDC` (see
`vendor/OGX360/Firmware/platformio.ini`). That means the running
application presents only the XID HID interface to USB, no serial
endpoint. avrdude's `avr109` programmer protocol speaks over USB CDC
serial, so it cannot reach the chip while the application is active.

The Caterina bootloader (the standard ATmega32U4 bootloader on Pro
Micros and Leonardos) is a separate program in the upper 4KB of flash.
It always exposes a USB CDC serial endpoint when active. It runs:

- For ~750 ms at every cold boot (single reset), then jumps to the
  application unless an upload is starting.
- For 8 seconds when triggered by a "double-tap reset" within ~750 ms.
- Indefinitely when triggered by Arduino IDE's auto-reset (sends a
  baud=1200 line-state pulse that the application firmware honors —
  but only if the application has CDC enabled, which Ryzee119's does
  not). So **double-tap reset is the only available trigger**.

## Procedure

### Step 1 — verify slot 2 is currently visible

With slot 2 plugged into the Mac Studio and a known-good data cable:

```sh
ioreg -p IOUSB -l -w 0 2>/dev/null | grep -E "idProduct|idVendor"
```

You should see `idVendor = 1118 (0x045E)` and `idProduct = 649 (0x0289)`.
That confirms slot 2's application firmware is running and the cable is
data-capable.

### Step 2 — enter bootloader mode

**Double-tap the reset button on slot 2's Pro Micro** within ~750 ms.
The chip resets into the bootloader and stays there for 8 seconds.

Within that 8-second window, a new USB device will appear at
`/dev/cu.usbmodem*`. The vendor/product IDs will be one of:

- `0x2341 / 0x0036` — Arduino Leonardo bootloader
- `0x2341 / 0x0037` — Arduino Leonardo (alternate)
- `0x1B4F / 0x9203` — SparkFun Pro Micro 5V bootloader (Caterina)
- `0x1B4F / 0x9204` — SparkFun Pro Micro 3.3V bootloader

You can find the exact device path with:

```sh
ls /dev/cu.usbmodem*
```

Make a note of the path — you'll have ~6 seconds left to issue the
avrdude command before the bootloader times out and jumps back to the
slave application. If you miss the window, just double-tap again.

### Step 3 — dump the flash

In the same 8-second window:

```sh
DEV=/dev/cu.usbmodemXXXX   # paste the actual device path from step 2
avrdude \
  -c avr109 \
  -p atmega32u4 \
  -P "$DEV" \
  -b 57600 \
  -U flash:r:/Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/firmware/slave-backup/slot2-original.hex:i
```

Expected: avrdude reads 32 KB of flash (~10 seconds) and writes the
result to `slot2-original.hex` in Intel HEX format.

If the timeout fires before avrdude connects, it'll error with
`programmer is not responding`. Re-trigger the bootloader and retry.

### Step 4 — verify the backup

```sh
ls -la /Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/firmware/slave-backup/slot2-original.hex
head -3 /Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/firmware/slave-backup/slot2-original.hex
```

The file should be a few hundred KB of ASCII Intel HEX records starting
with `:` colon. If the file exists and has reasonable size, the backup
is good.

For paranoia, compute and record a SHA-256:

```sh
shasum -a 256 \
  /Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/firmware/slave-backup/slot2-original.hex
```

Save that hash somewhere durable (e.g. in a comment in this runbook
after capture) so a future "did this file change?" check has an oracle.

## Restoring the backup if needed

If at any point slot 2 is bricked or has been reflashed with something
broken:

```sh
DEV=/dev/cu.usbmodemXXXX   # from `ls /dev/cu.usbmodem*` after double-tap reset
avrdude \
  -c avr109 \
  -p atmega32u4 \
  -P "$DEV" \
  -b 57600 \
  -U flash:w:/Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/firmware/slave-backup/slot2-original.hex:i
```

After the write completes, the chip will reboot into the restored slave
firmware and re-enumerate as `0x045E:0x0289` (OG Xbox Controller).

## Troubleshooting

- **No `/dev/cu.usbmodem*` appears after double-tap.** The reset is
  failing or going through too fast. Try slower (the two presses must
  be within ~750 ms but not so fast they merge into one). Or the chip
  may have just rebooted into the application before you got the
  second tap in — try again within the bootloader window.
- **`programmer is not responding`.** You missed the 8-second window.
  Re-trigger the bootloader and run avrdude faster. (Pro tip: queue
  the avrdude command in a terminal first, then double-tap, then hit
  Enter.)
- **avrdude says wrong device or wrong signature.** Confirm `-p
  atmega32u4` is correct (it is for any Pro Micro / Leonardo). If the
  signature mismatch persists, the cable/port may be flaky — try a
  different USB-A port on the Mac.
