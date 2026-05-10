#!/bin/bash
set -euo pipefail

if [ $# -ne 1 ] && [ $# -ne 3 ]; then
    echo "usage: $0 SKETCH.ino [--upload-port /dev/cu.usbmodemXXXX]" >&2
    exit 2
fi

SKETCH=$1
UPLOAD_PORT=""
if [ $# -eq 3 ]; then
    if [ "$2" != "--upload-port" ]; then
        echo "unknown option: $2" >&2
        exit 2
    fi
    UPLOAD_PORT=$3
fi

OUT=$(mktemp -d /tmp/ogx360-slot1-flash.XXXXXX)
trap 'rm -rf "$OUT"' EXIT

arduino-cli compile --fqbn arduino:avr:leonardo --output-dir "$OUT" "$SKETCH"
HEX="$OUT/$(basename "$SKETCH").hex"
if [ ! -f "$HEX" ]; then
    echo "compiled hex not found: $HEX" >&2
    exit 1
fi

echo "[slot1] compiled $HEX"
echo "[slot1] If 1200-baud touch fails, double-tap slot 1 RST now."
echo "[slot1] Watching /dev/cu.usbmodem* for 90s..."

if [ -n "$UPLOAD_PORT" ]; then
    PORTS=("$UPLOAD_PORT")
else
    PORTS=(/dev/cu.usbmodem*)
fi

deadline=$(($(date +%s) + 90))
tried=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    for dev in "${PORTS[@]}" /dev/cu.usbmodem*; do
        [ -e "$dev" ] || continue
        stamp="$dev:$(stat -f %m "$dev" 2>/dev/null || echo 0)"
        case " $tried " in *" $stamp "*) continue;; esac
        tried="$tried $stamp"
        echo "[slot1] trying $dev"
        if /opt/homebrew/bin/avrdude -q -c avr109 -p atmega32u4 \
            -P "$dev" -b 57600 -D -U "flash:w:$HEX:i"; then
            echo "[slot1] FLASH OK on $dev"
            exit 0
        fi
    done
    sleep 0.2
done

echo "[slot1] TIMEOUT - no bootloader accepted the upload" >&2
exit 1
