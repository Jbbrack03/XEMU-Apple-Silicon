#!/usr/bin/env bash
set -e
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
RUN_DIR="benchmark-runs/50AH-native-tri-depth-pgr2"
mkdir -p "$RUN_DIR"

cat > "$RUN_DIR/xemu.toml" <<TOML
[general]
show_welcome = false
[input]
auto_bind = true
background_input_capture = true
[display.window]
vsync = false
[display.quality]
surface_scale = 2
[sys.files]
bootrom_path = '/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/mcpx/mcpx_1.0.bin'
flashrom_path = '/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/bios/Complex_4627.bin'
eeprom_path = '/Users/jbbrack03/Library/Application Support/xemu/xemu/eeprom.bin'
hdd_path = '$RUN_DIR/xbox_hdd.qcow2'
dvd_path = '/Volumes/Final Cut Pro Libraries/Projects/XEMU_MacOS/Test_Games/PGR2.xiso.iso'
[display]
renderer = 'METAL'
[input.bindings]
port1 = 'keyboard'
port1_driver = 'usb-xbox-gamepad'
TOML

XEMU="/Users/jbbrack03/XEMU_MacOS/xemu-fork/dist/xemu.app/Contents/MacOS/xemu"

# 50AH: enable native triangle-depth alongside sibling sync depth
# to test whether the depth-only lane becomes informative when
# native depth draws are actually occurring.
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_METAL_DIAG_DEPTH_STATS=1 \
XEMU_METAL_SCREENSHOT_SOURCE=depth \
XEMU_METAL_SCREENSHOT_PATH="$RUN_DIR/depth-only.png" \
XEMU_METAL_SCREENSHOT_AT_FRAME=180 \
XEMU_METAL_RTT_SIBLING_SYNC_DEPTH=1 \
XEMU_METAL_SIBLING_SYNC_DEPTH_MAPPING_GUARD=1 \
XEMU_METAL_RTT_SIBLING_SYNC_DEPTH_ISOLATION_PREDICATE=1 \
XEMU_PERF_LOG=1 \
"$XEMU" \
    -config_path "$RUN_DIR/xemu.toml" \
    -qmp "unix:/tmp/xemu-qmp,server=on,wait=off" \
    > "$RUN_DIR/xemu-full.log" 2>&1 &

XEMU_PID=$!
echo "$XEMU_PID" > "$RUN_DIR/xemu.pid"
echo "Started xemu with PID $XEMU_PID"
wait "$XEMU_PID"
