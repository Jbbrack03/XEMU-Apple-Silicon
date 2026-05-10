#!/bin/bash
set -e
HEX=/Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/ogx360-bridge/vendor/OGX360/Firmware/.pio/build/OGX360/firmware.hex
SLOT1=/dev/cu.usbmodem3101
echo "[watcher] watching for new /dev/cu.usbmodem* (slot 1 ignored)..."
deadline=$(($(date +%s) + 60))
while [ $(date +%s) -lt $deadline ]; do
    for dev in /dev/cu.usbmodem*; do
        if [ -e "$dev" ] && [ "$dev" != "$SLOT1" ]; then
            echo "[watcher] FOUND bootloader at $dev"
            sleep 0.3   # tiny settle
            echo "[avrdude] flashing..."
            /opt/homebrew/bin/avrdude -c avr109 -p atmega32u4 -P "$dev" -b 57600 -D -U flash:w:"$HEX":i 2>&1
            rc=$?
            echo "[avrdude] exit=$rc"
            exit $rc
        fi
    done
    sleep 0.1
done
echo "[watcher] TIMEOUT - no bootloader detected within 60s"
exit 1
