#!/usr/bin/env bash
set -e
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
RUN_DIR="benchmark-runs/50AX-captured-surface-transition-diagnostic-pgr2"
mkdir -p "$RUN_DIR"

# Create the config file at standard path
cat > "$HOME/Library/Application Support/xemu/xemu/xemu.toml" <<TOML
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
bootrom_path = "/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/mcpx/mcpx_1.0.bin"
flashrom_path = "/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/bios/Complex_4627.bin"
eeprom_path = "/Users/jbbrack03/Library/Application Support/xemu/xemu/eeprom.bin"
hdd_path = "$HOME/.xemu/hdds/default.qcow2"
dvd_path = "/Volumes/Final Cut Pro Libraries/Projects/XEMU_MacOS/Test_Games/PGR2.xiso.iso"
[display]
renderer = "METAL"
[input.bindings]
port1 = "keyboard"
port1_driver = "usb-xbox-gamepad"
TOML

XEMU="/Users/jbbrack03/XEMU_MacOS/xemu-fork/dist/xemu.app/Contents/MacOS/xemu"

# 50AX: diagnostic run with all depth diagnostic env vars enabled
# to capture clear/load/store/z-state transitions on surface 0x038e0000
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_METAL_RTT_SIBLING_SYNC_DEPTH=1 \
XEMU_METAL_DIAG_DEPTH_CLEAR=1 \
XEMU_METAL_DIAG_DEPTH_LOAD=1 \
XEMU_METAL_DIAG_DEPTH_STORE=1 \
XEMU_METAL_DIAG_DEPTH_STATE=1 \
XEMU_METAL_DIAG_DEPTH_BIND=1 \
XEMU_METAL_DIAG_DEPTH_CAPTURE=1 \
XEMU_PERF_LOG=1 \
"$XEMU" 2> "$RUN_DIR/xemu.log" &
XEMU_PID=$!

# Wait a few seconds for xemu to start and process input
sleep 5

# Send quit command to stop after bounded time
kill -INT $XEMU_PID 2>/dev/null || true
wait $XEMU_PID 2>/dev/null || true

echo "=== 50AX DIAG: collecting output ==="
echo ""
echo "=== 50AX DIAG lines ==="
grep "50AX DIAG" "$RUN_DIR/xemu.log" 2>/dev/null || echo "(no 50AX DIAG lines found)"
echo ""
echo "=== Screenshot depth capture diagnostic ==="
grep "metal_screenshot_depth_capture_diag" "$RUN_DIR/xemu.log" 2>/dev/null || echo "(no screenshot depth capture diagnostic lines found)"
echo ""
echo "=== Sibling sync counters ==="
grep -i "sibling_sync_count\|sibling_sync_skip\|native_tri_depth_count" "$RUN_DIR/xemu.log" 2>/dev/null | tail -5 || echo "(no counter lines found)"
echo ""
echo "=== Total log lines ==="
wc -l "$RUN_DIR/xemu.log" 2>/dev/null || echo "(log not found)"
echo ""
echo "=== Any errors/warnings ==="
grep -i "error\|warning\|fail" "$RUN_DIR/xemu.log" 2>/dev/null | head -10 || echo "(no errors found)"
