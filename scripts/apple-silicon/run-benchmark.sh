#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_ROOT="${ROOT_DIR}/benchmark-runs"
XEMU="${ROOT_DIR}/dist/xemu.app/Contents/MacOS/xemu"

MCPX="/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/mcpx/mcpx_1.0.bin"
BIOS="/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/bios/Complex_4627.bin"
HDD="/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/hdd/xbox_hdd.qcow2"
HDD_SOURCE="${XEMU_BENCH_HDD_SOURCE:-$HDD}"
TEST_GAMES_DIR="${XEMU_TEST_GAMES_DIR:-/Users/jbbrack03/XEMU_MacOS/Test_Games}"
ALT_TEST_GAMES_DIR="/Volumes/Final Cut Pro Libraries/Projects/XEMU_MacOS/Test_Games"

find_test_disc() {
    local filename="$1"
    if [[ -e "${TEST_GAMES_DIR}/${filename}" ]]; then
        printf '%s\n' "${TEST_GAMES_DIR}/${filename}"
    elif [[ -e "${ALT_TEST_GAMES_DIR}/${filename}" ]]; then
        printf '%s\n' "${ALT_TEST_GAMES_DIR}/${filename}"
    else
        printf '%s\n' "${TEST_GAMES_DIR}/${filename}"
    fi
}

usage() {
    cat <<EOF
usage: $0 crimson|rainbow|pgr2|flat-tri-depth [input-script.csv] [duration-seconds]

Runs xemu with the Apple Silicon scripted-input benchmark harness enabled.
Outputs logs and a scratch HDD copy under benchmark-runs/.

Set XEMU_BENCH_RECORD_INPUT=auto to record physical controller input to the
run directory instead of replaying a scripted input file.
EOF
}

if [[ $# -lt 1 || $# -gt 3 ]]; then
    usage >&2
    exit 2
fi

case "$1" in
    crimson)
        GAME_NAME="crimson-skies"
        DISC="$(find_test_disc "Crimson skies.xiso.iso")"
        DEFAULT_SCRIPT="${ROOT_DIR}/scripts/apple-silicon/input-scripts/crimson-skies-smoke.csv"
        ;;
    rainbow)
        GAME_NAME="rainbow-six-3"
        DISC="$(find_test_disc "Rainbow Six 3.xiso.iso")"
        DEFAULT_SCRIPT="${ROOT_DIR}/scripts/apple-silicon/input-scripts/rainbow-six-3-smoke.csv"
        ;;
    pgr2)
        GAME_NAME="pgr2"
        DISC="$(find_test_disc "PGR2.xiso.iso")"
        DEFAULT_SCRIPT="${ROOT_DIR}/scripts/apple-silicon/input-scripts/pgr2-smoke.csv"
        ;;
    flat-tri-depth)
        GAME_NAME="flat-tri-depth"
        DISC="${ROOT_DIR}/scripts/apple-silicon/xbe-tests/flat-tri-depth/flat-tri-depth.iso"
        DEFAULT_SCRIPT="${ROOT_DIR}/scripts/apple-silicon/input-scripts/noop.csv"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

INPUT_SCRIPT="${2:-$DEFAULT_SCRIPT}"
DURATION="${3:-180}"
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="${RUN_ROOT}/${STAMP}-${GAME_NAME}"
RECORD_INPUT_REQUEST="${XEMU_BENCH_RECORD_INPUT:-}"
if [[ "$RECORD_INPUT_REQUEST" == "1" || "$RECORD_INPUT_REQUEST" == "auto" ]]; then
    RECORD_INPUT="${RUN_DIR}/recorded-input.csv"
else
    RECORD_INPUT="$RECORD_INPUT_REQUEST"
fi
SCRATCH_HDD="${RUN_DIR}/xbox_hdd.qcow2"
LOG_FILE="${RUN_DIR}/xemu.log"
META_FILE="${RUN_DIR}/metadata.txt"
QMP_SOCKET="${RUN_DIR}/qmp.sock"
CONFIG_FILE="${RUN_DIR}/xemu.toml"
SCREENSHOT_DIR="${RUN_DIR}/screenshots"
CAPTURE_LOG="${RUN_DIR}/capture.log"
SNAPSHOT_LOG="${RUN_DIR}/snapshot.log"
SCREENSHOT_INTERVAL="${XEMU_BENCH_SCREENSHOT_INTERVAL:-10}"
SCREENSHOT_START_DELAY="${XEMU_BENCH_SCREENSHOT_START_DELAY:-5}"
SCREENSHOT_BACKEND="${XEMU_BENCH_SCREENSHOT_BACKEND:-macos}"
PERF_LOG_INTERVAL_MS="${XEMU_PERF_LOG_INTERVAL_MS:-1000}"
SAVEVM_AT="${XEMU_BENCH_SAVEVM_AT:-}"
SAVEVM_TAG="${XEMU_BENCH_SAVEVM_TAG:-${GAME_NAME}-scene}"
LOADVM_TAG="${XEMU_BENCH_LOADVM_TAG:-}"
LOADVM_AT="${XEMU_BENCH_LOADVM_AT:-2}"
SNAPSHOT_NO_THUMBNAIL="${XEMU_BENCH_SNAPSHOT_NO_THUMBNAIL:-1}"
EXTRA_QEMU_ARGS="${XEMU_BENCH_EXTRA_QEMU_ARGS:-}"
PORT1_BINDING="keyboard"
if [[ -n "$RECORD_INPUT" ]]; then
    PORT1_BINDING=""
fi

for file in "$XEMU" "$MCPX" "$BIOS" "$HDD_SOURCE" "$DISC"; do
    if [[ ! -e "$file" ]]; then
        echo "missing required file: $file" >&2
        exit 1
    fi
done

if [[ -z "$RECORD_INPUT" && ! -e "$INPUT_SCRIPT" ]]; then
    echo "missing required file: $INPUT_SCRIPT" >&2
    exit 1
fi

EXISTING_XEMU_PIDS="$(pgrep -f "$XEMU" || true)"
if [[ -n "$EXISTING_XEMU_PIDS" && "${XEMU_BENCH_ALLOW_EXISTING:-0}" != "1" ]]; then
    echo "xemu is already running; close it before starting a benchmark run:" >&2
    echo "$EXISTING_XEMU_PIDS" >&2
    exit 1
fi

mkdir -p "$RUN_DIR"
cp -c "$HDD_SOURCE" "$SCRATCH_HDD" 2>/dev/null || cp "$HDD_SOURCE" "$SCRATCH_HDD"

cat > "$CONFIG_FILE" <<EOF
[general]
show_welcome = false

[input]
auto_bind = true
background_input_capture = true

[display.window]
vsync = false

[sys.files]
bootrom_path = '$MCPX'
flashrom_path = '$BIOS'
hdd_path = '$SCRATCH_HDD'
dvd_path = '$DISC'

[input.bindings]
port1 = '$PORT1_BINDING'
port1_driver = 'usb-xbox-gamepad'
EOF

{
    echo "date: $(date)"
    echo "game: $GAME_NAME"
    echo "duration_seconds: $DURATION"
    if [[ -n "$RECORD_INPUT" ]]; then
        echo "input_script: none"
    else
        echo "input_script: $INPUT_SCRIPT"
    fi
    echo "record_input: ${RECORD_INPUT:-none}"
    echo "run_dir: $RUN_DIR"
    echo "xemu: $XEMU"
    echo "disc: $DISC"
    echo "disc_size_bytes: $(stat -f%z "$DISC" 2>/dev/null || stat -c%s "$DISC" 2>/dev/null || echo unknown)"
    echo "disc_mtime: $(stat -f%Sm "$DISC" 2>/dev/null || stat -c%y "$DISC" 2>/dev/null || echo unknown)"
    echo "hdd_source: $HDD_SOURCE"
    echo "scratch_hdd: $SCRATCH_HDD"
    echo "config_file: $CONFIG_FILE"
    echo "screenshot_interval_seconds: $SCREENSHOT_INTERVAL"
    echo "screenshot_start_delay_seconds: $SCREENSHOT_START_DELAY"
    echo "screenshot_backend: $SCREENSHOT_BACKEND"
    echo "perf_log: XEMU_PERF_LOG=1"
    echo "perf_log_interval_ms: $PERF_LOG_INTERVAL_MS"
    echo "savevm_at_seconds: ${SAVEVM_AT:-none}"
    echo "savevm_tag: ${SAVEVM_AT:+$SAVEVM_TAG}"
    echo "loadvm_tag: ${LOADVM_TAG:-none}"
    echo "loadvm_at_seconds: ${LOADVM_TAG:+$LOADVM_AT}"
    echo "snapshot_no_thumbnail: $SNAPSHOT_NO_THUMBNAIL"
    echo "extra_qemu_args: ${EXTRA_QEMU_ARGS:-none}"
    echo "env_XEMU_NATIVE_TRI_DEPTH: ${XEMU_NATIVE_TRI_DEPTH:-unset}"
    echo "env_XEMU_DIAG_NATIVE_TRI_DEPTH: ${XEMU_DIAG_NATIVE_TRI_DEPTH:-unset}"
    echo "env_XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE: ${XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE:-unset}"
    echo "env_XEMU_DIAG_SKIP_TRI_GEOM: ${XEMU_DIAG_SKIP_TRI_GEOM:-unset}"
    echo "env_XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH: ${XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH:-unset}"
    echo
    sw_vers || true
    uname -m || true
    sysctl -n machdep.cpu.brand_string 2>/dev/null || true
    git -C "$ROOT_DIR" rev-parse HEAD || true
    git -C "$ROOT_DIR" status --short || true
} > "$META_FILE"

cleanup() {
    if [[ -n "${CAPTURE_PID:-}" ]] && kill -0 "$CAPTURE_PID" 2>/dev/null; then
        kill "$CAPTURE_PID" 2>/dev/null || true
        wait "$CAPTURE_PID" 2>/dev/null || true
    fi

    if [[ -n "${SAVEVM_PID:-}" ]] && kill -0 "$SAVEVM_PID" 2>/dev/null; then
        kill "$SAVEVM_PID" 2>/dev/null || true
        wait "$SAVEVM_PID" 2>/dev/null || true
    fi

    if [[ -n "${LOADVM_PID:-}" ]] && kill -0 "$LOADVM_PID" 2>/dev/null; then
        kill "$LOADVM_PID" 2>/dev/null || true
        wait "$LOADVM_PID" 2>/dev/null || true
    fi

    RUN_XEMU_PIDS="${XEMU_PID:-}"

    if [[ -n "$RUN_XEMU_PIDS" ]]; then
        if [[ -S "$QMP_SOCKET" ]]; then
            python3 - "$QMP_SOCKET" <<'PY' >/dev/null 2>&1 || true
import json
import socket
import sys

sock = socket.socket(socket.AF_UNIX)
sock.settimeout(1.0)
sock.connect(sys.argv[1])
sock.recv(4096)
sock.sendall(json.dumps({"execute": "qmp_capabilities"}).encode() + b"\r\n")
sock.recv(4096)
sock.sendall(json.dumps({"execute": "quit"}).encode() + b"\r\n")
sock.close()
PY

            for _ in 1 2 3; do
                any_alive=0
                for pid in $RUN_XEMU_PIDS; do
                    if kill -0 "$pid" 2>/dev/null; then
                        any_alive=1
                    fi
                done
                if [[ "$any_alive" -eq 0 ]]; then
                    [[ -n "${XEMU_PID:-}" ]] && wait "$XEMU_PID" 2>/dev/null || true
                    return
                fi
                sleep 1
            done
        fi

        for pid in $RUN_XEMU_PIDS; do
            kill "$pid" 2>/dev/null || true
        done

        for _ in 1 2 3 4 5; do
            any_alive=0
            for pid in $RUN_XEMU_PIDS; do
                if kill -0 "$pid" 2>/dev/null; then
                    any_alive=1
                fi
            done
            if [[ "$any_alive" -eq 0 ]]; then
                [[ -n "${XEMU_PID:-}" ]] && wait "$XEMU_PID" 2>/dev/null || true
                return
            fi
            sleep 1
        done

        for pid in $RUN_XEMU_PIDS; do
            kill -KILL "$pid" 2>/dev/null || true
        done
        [[ -n "${XEMU_PID:-}" ]] && wait "$XEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

echo "Starting $GAME_NAME for ${DURATION}s"
echo "Run directory: $RUN_DIR"

if [[ -n "$RECORD_INPUT" ]]; then
    echo "Recording controller input: $RECORD_INPUT"
    XEMU_RECORD_INPUT="$RECORD_INPUT" \
    XEMU_RECORD_INPUT_PORT=1 \
    XEMU_PERF_LOG=1 \
    XEMU_PERF_LOG_INTERVAL_MS="$PERF_LOG_INTERVAL_MS" \
    XEMU_SNAPSHOT_NO_THUMBNAIL="$SNAPSHOT_NO_THUMBNAIL" \
    "$XEMU" \
      -config_path "$CONFIG_FILE" \
      -qmp "unix:${QMP_SOCKET},server=on,wait=off" \
      ${EXTRA_QEMU_ARGS} \
      > "$LOG_FILE" 2>&1 &
else
    XEMU_SCRIPTED_INPUT="$INPUT_SCRIPT" \
    XEMU_SCRIPTED_INPUT_PORT=1 \
    XEMU_PERF_LOG=1 \
    XEMU_PERF_LOG_INTERVAL_MS="$PERF_LOG_INTERVAL_MS" \
    XEMU_SNAPSHOT_NO_THUMBNAIL="$SNAPSHOT_NO_THUMBNAIL" \
    "$XEMU" \
      -config_path "$CONFIG_FILE" \
      -qmp "unix:${QMP_SOCKET},server=on,wait=off" \
      ${EXTRA_QEMU_ARGS} \
      > "$LOG_FILE" 2>&1 &
fi

XEMU_PID=$!
echo "$XEMU_PID" > "${RUN_DIR}/xemu.pid"

case "$SCREENSHOT_BACKEND" in
  macos)
    "${ROOT_DIR}/scripts/apple-silicon/macos-capture.sh" \
      "$SCREENSHOT_DIR" "$DURATION" "$SCREENSHOT_INTERVAL" "$SCREENSHOT_START_DELAY" \
      > "$CAPTURE_LOG" 2>&1 &
    ;;
  qmp)
    python3 "${ROOT_DIR}/scripts/apple-silicon/qmp-capture.py" \
      --socket "$QMP_SOCKET" \
      --out-dir "$SCREENSHOT_DIR" \
      --duration "$DURATION" \
      --interval "$SCREENSHOT_INTERVAL" \
      --start-delay "$SCREENSHOT_START_DELAY" \
      > "$CAPTURE_LOG" 2>&1 &
    ;;
  none)
    (sleep "$DURATION") > "$CAPTURE_LOG" 2>&1 &
    ;;
  *)
    echo "Unknown XEMU_BENCH_SCREENSHOT_BACKEND '$SCREENSHOT_BACKEND'" >&2
    exit 2
    ;;
esac
CAPTURE_PID=$!

if [[ -n "$LOADVM_TAG" ]]; then
    (
        sleep "$LOADVM_AT"
        echo "loading VM snapshot '$LOADVM_TAG' at ${LOADVM_AT}s"
        python3 "${ROOT_DIR}/scripts/apple-silicon/qmp-hmp.py" \
          --socket "$QMP_SOCKET" loadvm "$LOADVM_TAG"
    ) > "$SNAPSHOT_LOG" 2>&1 &
    LOADVM_PID=$!
else
    : > "$SNAPSHOT_LOG"
fi

if [[ -n "$SAVEVM_AT" ]]; then
    (
        sleep "$SAVEVM_AT"
        echo "saving VM snapshot '$SAVEVM_TAG' at ${SAVEVM_AT}s"
        python3 "${ROOT_DIR}/scripts/apple-silicon/qmp-hmp.py" \
          --socket "$QMP_SOCKET" savevm "$SAVEVM_TAG"
        echo "snapshot list after save:"
        python3 "${ROOT_DIR}/scripts/apple-silicon/qmp-hmp.py" \
          --socket "$QMP_SOCKET" info snapshots
    ) >> "$SNAPSHOT_LOG" 2>&1 &
    SAVEVM_PID=$!
fi

sleep "$DURATION"
wait "$CAPTURE_PID" 2>/dev/null || true
if [[ -n "${LOADVM_PID:-}" ]]; then
    wait "$LOADVM_PID" 2>/dev/null || true
fi
if [[ -n "${SAVEVM_PID:-}" ]]; then
    wait "$SAVEVM_PID" 2>/dev/null || true
fi
cleanup
trap - EXIT INT TERM

echo "Finished. Metadata: $META_FILE"
echo "Log: $LOG_FILE"
echo "Capture log: $CAPTURE_LOG"
echo "Snapshot log: $SNAPSHOT_LOG"
