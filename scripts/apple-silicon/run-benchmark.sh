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
usage: $0 [--metal-capture <path>] [--metal-screenshot <path>] [--metal-screenshot-at-frame <N>] [--metal-no-validate] [--metal-no-hud] crimson|rainbow|pgr2|sc2|halo|flat-tri-depth [input-script.csv] [duration-seconds]

Runs xemu with the Apple Silicon scripted-input benchmark harness enabled.
Outputs logs and a scratch HDD copy under benchmark-runs/.

Set XEMU_BENCH_RECORD_INPUT=auto to record physical controller input to the
run directory instead of replaying a scripted input file.
Set XEMU_BENCH_LIVE_INPUT=1 for physical controller input without recording.
Set XEMU_BENCH_HDD_IN_PLACE=1 only with a copied HDD image that should be
modified directly by profile/setup runs.

Options:
  --metal-capture <path>   Programmatic Metal frame capture (M13 2026-05-02).
                           Sets XEMU_METAL_CAPTURE=<path> for the run; the
                           Metal renderer's MTLCaptureManager writes a
                           .gputrace document at <path> bounded by
                           XEMU_METAL_CAPTURE_FRAMES (default 60). Open the
                           output in Xcode (Window > Organizer > GPU Frame
                           Capture). Requires the Metal renderer to be the
                           active backend; on the GL renderer the env var
                           is harmless and ignored.
  --metal-screenshot <path>
                           Programmatic PNG screenshot of the final
                           composited drawable (2026-05-03). Sets
                           XEMU_METAL_SCREENSHOT_PATH=<path> for the run;
                           the Metal renderer captures the drawable into a
                           shared MTLBuffer in the post-HUD-encoder /
                           pre-presentDrawable: window and writes it as a
                           PNG via FPNG. Compared with macOS \`screencapture\`,
                           this path takes no Screen-Recording permission
                           dialog and never occludes the xemu window.
                           Requires the Metal renderer to be active; on
                           the GL renderer the env var is harmless and
                           ignored.
  --metal-screenshot-at-frame <N>
                           Frame number (1-indexed against the upcoming
                           present) at which --metal-screenshot fires.
                           Default 60. Maps to
                           XEMU_METAL_SCREENSHOT_AT_FRAME=<N>.
  --metal-no-validate      W1 (2026-05-04) — opt out of the auto-on
                           XEMU_METAL_VALIDATION=1 export that
                           run-benchmark.sh applies whenever
                           XEMU_RENDERER=METAL. Default behavior auto-
                           promotes Metal API + shader validation in
                           dev runs (cost: an extra log line and the
                           validation layer's checks); pass this flag
                           when measuring perf and you do not want the
                           validation overhead. An explicit user-set
                           XEMU_METAL_VALIDATION (any non-empty value)
                           in the calling environment also wins over
                           the auto-export.
  --metal-no-hud           W1 (2026-05-04) — opt out of the auto-on
                           XEMU_METAL_HUD=1 export that run-benchmark.sh
                           applies whenever XEMU_RENDERER=METAL. Default
                           behavior shows Apple's Metal Performance HUD
                           overlay (zero perf cost; pure observation).
                           Pass this flag for clean visual canary
                           captures. An explicit user-set XEMU_METAL_HUD
                           in the calling environment wins over the
                           auto-export.
EOF
}

# Optional flag(s) parsed before the positional args. Kept simple
# rather than pulling in a full getopt/long-options dance.
METAL_CAPTURE_PATH=""
METAL_SCREENSHOT_PATH=""
METAL_SCREENSHOT_AT_FRAME=""
METAL_NO_VALIDATE=0
METAL_NO_HUD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --metal-capture)
            if [[ $# -lt 2 ]]; then
                echo "--metal-capture requires a path argument" >&2
                exit 2
            fi
            METAL_CAPTURE_PATH="$2"
            shift 2
            ;;
        --metal-capture=*)
            METAL_CAPTURE_PATH="${1#--metal-capture=}"
            shift
            ;;
        --metal-screenshot)
            if [[ $# -lt 2 ]]; then
                echo "--metal-screenshot requires a path argument" >&2
                exit 2
            fi
            METAL_SCREENSHOT_PATH="$2"
            shift 2
            ;;
        --metal-screenshot=*)
            METAL_SCREENSHOT_PATH="${1#--metal-screenshot=}"
            shift
            ;;
        --metal-screenshot-at-frame)
            if [[ $# -lt 2 ]]; then
                echo "--metal-screenshot-at-frame requires a frame number" >&2
                exit 2
            fi
            METAL_SCREENSHOT_AT_FRAME="$2"
            shift 2
            ;;
        --metal-screenshot-at-frame=*)
            METAL_SCREENSHOT_AT_FRAME="${1#--metal-screenshot-at-frame=}"
            shift
            ;;
        --metal-no-validate)
            METAL_NO_VALIDATE=1
            shift
            ;;
        --metal-no-hud)
            METAL_NO_HUD=1
            shift
            ;;
        --)
            shift
            break
            ;;
        --*)
            echo "unknown flag: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

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
    sc2)
        GAME_NAME="soul-calibur-2"
        DISC="$(find_test_disc "Soul Calibur 2.xiso.iso")"
        DEFAULT_SCRIPT="${ROOT_DIR}/scripts/apple-silicon/input-scripts/noop.csv"
        ;;
    halo)
        GAME_NAME="halo-ce"
        DISC="$(find_test_disc "Halo - Combat Evolved.xiso.iso")"
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
LIVE_INPUT="${XEMU_BENCH_LIVE_INPUT:-0}"
HDD_IN_PLACE="${XEMU_BENCH_HDD_IN_PLACE:-0}"
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
VISUAL_ANALYSIS="${XEMU_BENCH_VISUAL_ANALYSIS:-0}"
# Tool 2 (2026-05-19): every-frame temporal capture mode. When set to 1,
# the launcher forces PNG-every-frame output (renderer-native on Metal,
# parallel ffmpeg AVFoundation on GL). Designed for downstream
# `scripts/apple-silicon/temporal-flicker-analyze.py` consumption.
# capture-gameplay-temporal.sh sets this; it can also be set directly.
TEMPORAL_CAPTURE="${XEMU_BENCH_TEMPORAL_CAPTURE:-0}"
TEMPORAL_FPS="${XEMU_BENCH_TEMPORAL_FPS:-60}"
PERF_LOG_INTERVAL_MS="${XEMU_PERF_LOG_INTERVAL_MS:-1000}"
SAVEVM_AT="${XEMU_BENCH_SAVEVM_AT:-}"
SAVEVM_TAG="${XEMU_BENCH_SAVEVM_TAG:-${GAME_NAME}-scene}"
LOADVM_TAG="${XEMU_BENCH_LOADVM_TAG:-}"
LOADVM_AT="${XEMU_BENCH_LOADVM_AT:-2}"
SNAPSHOT_NO_THUMBNAIL="${XEMU_BENCH_SNAPSHOT_NO_THUMBNAIL:-1}"
EXTRA_QEMU_ARGS="${XEMU_BENCH_EXTRA_QEMU_ARGS:-}"
PORT1_BINDING="keyboard"
if [[ -n "$RECORD_INPUT" || "$LIVE_INPUT" == "1" ]]; then
    PORT1_BINDING=""
fi

for file in "$XEMU" "$MCPX" "$BIOS" "$HDD_SOURCE" "$DISC"; do
    if [[ ! -e "$file" ]]; then
        echo "missing required file: $file" >&2
        exit 1
    fi
done

if [[ -z "$RECORD_INPUT" && "$LIVE_INPUT" != "1" && ! -e "$INPUT_SCRIPT" ]]; then
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
if [[ "$HDD_IN_PLACE" == "1" ]]; then
    SCRATCH_HDD="$HDD_SOURCE"
else
    cp -c "$HDD_SOURCE" "$SCRATCH_HDD" 2>/dev/null || cp "$HDD_SOURCE" "$SCRATCH_HDD"
fi

# W1 (2026-05-04) — auto-on Metal validation + HUD in dev runs. When the
# Metal renderer is selected via XEMU_RENDERER=METAL, promote
# XEMU_METAL_VALIDATION=1 and XEMU_METAL_HUD=1 unless the user passed
# the corresponding --metal-no-* opt-out OR already pinned the env var
# themselves. The cost of a forgotten validation flag in a dev run is
# high (silent shader/API misuse); the cost of an extra log line + a
# Performance HUD overlay is zero. xemu-side renderer code keeps its
# default-off opt-in behavior — the auto-on lives here.
#
# Run before the metadata write below so env_XEMU_METAL_VALIDATION /
# env_XEMU_METAL_HUD reflect the effective state.
AUTO_METAL_VALIDATION="not-applicable (renderer != METAL)"
AUTO_METAL_HUD="not-applicable (renderer != METAL)"
if [[ "${XEMU_RENDERER:-}" == "METAL" ]]; then
    if [[ "$METAL_NO_VALIDATE" -eq 1 ]]; then
        AUTO_METAL_VALIDATION="auto-off (--metal-no-validate)"
    elif [[ -n "${XEMU_METAL_VALIDATION:-}" ]]; then
        AUTO_METAL_VALIDATION="user-pinned (XEMU_METAL_VALIDATION=${XEMU_METAL_VALIDATION})"
    else
        export XEMU_METAL_VALIDATION=1
        AUTO_METAL_VALIDATION="auto-on"
    fi
    if [[ "$METAL_NO_HUD" -eq 1 ]]; then
        AUTO_METAL_HUD="auto-off (--metal-no-hud)"
    elif [[ -n "${XEMU_METAL_HUD:-}" ]]; then
        AUTO_METAL_HUD="user-pinned (XEMU_METAL_HUD=${XEMU_METAL_HUD})"
    else
        export XEMU_METAL_HUD=1
        AUTO_METAL_HUD="auto-on"
    fi
    echo "Metal auto-on: validation=${AUTO_METAL_VALIDATION} hud=${AUTO_METAL_HUD}"
fi

# Default to 2 so the per-run config matches the Apple Silicon system
# build's first-run default (1080p-class, ~7 % renderer-cost growth on
# PGR2 vs scale 1; see docs/apple-silicon/benchmarks/2026-05-01-gl-vs-metal-decision.md).
# Override with XEMU_BENCH_SURFACE_SCALE=N for explicit A/B sweeps.
SURFACE_SCALE="${XEMU_BENCH_SURFACE_SCALE:-2}"

cat > "$CONFIG_FILE" <<EOF
[general]
show_welcome = false

[input]
auto_bind = true
background_input_capture = true

[display.window]
vsync = false

[display.quality]
surface_scale = $SURFACE_SCALE

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
    elif [[ "$LIVE_INPUT" == "1" ]]; then
        echo "input_script: none"
    else
        echo "input_script: $INPUT_SCRIPT"
    fi
    echo "record_input: ${RECORD_INPUT:-none}"
    echo "live_input: $LIVE_INPUT"
    echo "run_dir: $RUN_DIR"
    echo "xemu: $XEMU"
    echo "disc: $DISC"
    echo "disc_size_bytes: $(stat -f%z "$DISC" 2>/dev/null || stat -c%s "$DISC" 2>/dev/null || echo unknown)"
    echo "disc_mtime: $(stat -f%Sm "$DISC" 2>/dev/null || stat -c%y "$DISC" 2>/dev/null || echo unknown)"
    echo "hdd_source: $HDD_SOURCE"
    echo "scratch_hdd: $SCRATCH_HDD"
    echo "hdd_in_place: $HDD_IN_PLACE"
    echo "config_file: $CONFIG_FILE"
    echo "screenshot_interval_seconds: $SCREENSHOT_INTERVAL"
    echo "screenshot_start_delay_seconds: $SCREENSHOT_START_DELAY"
    echo "screenshot_backend: $SCREENSHOT_BACKEND"
    echo "visual_analysis: $VISUAL_ANALYSIS"
    echo "perf_log: XEMU_PERF_LOG=1"
    echo "perf_log_interval_ms: $PERF_LOG_INTERVAL_MS"
    echo "savevm_at_seconds: ${SAVEVM_AT:-none}"
    echo "savevm_tag: ${SAVEVM_AT:+$SAVEVM_TAG}"
    echo "loadvm_tag: ${LOADVM_TAG:-none}"
    echo "loadvm_at_seconds: ${LOADVM_TAG:+$LOADVM_AT}"
    echo "snapshot_no_thumbnail: $SNAPSHOT_NO_THUMBNAIL"
    echo "extra_qemu_args: ${EXTRA_QEMU_ARGS:-none}"
    echo "surface_scale: $SURFACE_SCALE"
    echo "env_XEMU_BENCH_SURFACE_SCALE: ${XEMU_BENCH_SURFACE_SCALE:-unset}"
    echo "env_XEMU_DISPLAY_SCALE: ${XEMU_DISPLAY_SCALE:-unset}"
    echo "env_XEMU_NATIVE_TRI_DEPTH: ${XEMU_NATIVE_TRI_DEPTH:-unset}"
    echo "env_XEMU_NATIVE_QUAD: ${XEMU_NATIVE_QUAD:-unset}"
    echo "env_XEMU_PGRAPH_FAST_READ: ${XEMU_PGRAPH_FAST_READ:-unset}"
    echo "env_XEMU_PERF_FRAME_LOG: ${XEMU_PERF_FRAME_LOG:-unset}"
    echo "env_XEMU_DIAG_NATIVE_TRI_DEPTH: ${XEMU_DIAG_NATIVE_TRI_DEPTH:-unset}"
    echo "env_XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE: ${XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE:-unset}"
    echo "env_XEMU_DIAG_SKIP_TRI_GEOM: ${XEMU_DIAG_SKIP_TRI_GEOM:-unset}"
    echo "env_XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH: ${XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH:-unset}"
    echo "metal_capture_path: ${METAL_CAPTURE_PATH:-none}"
    echo "env_XEMU_METAL_CAPTURE: ${XEMU_METAL_CAPTURE:-unset}"
    echo "env_XEMU_METAL_CAPTURE_FRAMES: ${XEMU_METAL_CAPTURE_FRAMES:-unset}"
    echo "metal_screenshot_path: ${METAL_SCREENSHOT_PATH:-none}"
    echo "metal_screenshot_at_frame: ${METAL_SCREENSHOT_AT_FRAME:-default(60)}"
    echo "env_XEMU_METAL_SCREENSHOT_PATH: ${XEMU_METAL_SCREENSHOT_PATH:-unset}"
    echo "env_XEMU_METAL_SCREENSHOT_AT_FRAME: ${XEMU_METAL_SCREENSHOT_AT_FRAME:-unset}"
    echo "env_XEMU_METAL_SCREENSHOT_INTERVAL: ${XEMU_METAL_SCREENSHOT_INTERVAL:-unset}"
    echo "env_XEMU_RENDERER: ${XEMU_RENDERER:-unset}"
    echo "metal_auto_validation: ${AUTO_METAL_VALIDATION}"
    echo "metal_auto_hud: ${AUTO_METAL_HUD}"
    echo "env_XEMU_METAL_VALIDATION: ${XEMU_METAL_VALIDATION:-unset}"
    echo "env_XEMU_METAL_HUD: ${XEMU_METAL_HUD:-unset}"
    echo
    sw_vers || true
    uname -m || true
    sysctl -n machdep.cpu.brand_string 2>/dev/null || true
    git -C "$ROOT_DIR" rev-parse HEAD || true
    git -C "$ROOT_DIR" status --short || true
} > "$META_FILE"

cleanup() {
    if [[ -n "${TEMPORAL_FFMPEG_PID:-}" ]] && kill -0 "$TEMPORAL_FFMPEG_PID" 2>/dev/null; then
        # SIGINT first so the .mov container finalizes cleanly.
        kill -INT "$TEMPORAL_FFMPEG_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            if ! kill -0 "$TEMPORAL_FFMPEG_PID" 2>/dev/null; then break; fi
            sleep 0.5
        done
        kill -KILL "$TEMPORAL_FFMPEG_PID" 2>/dev/null || true
        wait "$TEMPORAL_FFMPEG_PID" 2>/dev/null || true
    fi

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

            # 2026-05-03: extended QMP-quit grace window from 3 s to 15 s.
            # The 2026-05-02 22:50 GLG crash left macOS-side OpenGL
            # worker state that makes xemu's shutdown path slow on
            # subsequent benchmark runs; with the 3 s window xemu was
            # hitting SIGTERM/SIGKILL before atexit could emit the
            # `final=1` interval line that flushes cumulative
            # FLAT_FIRST / FLAT_NONFIRST counters. The wider window
            # gives xemu enough time to exit cleanly via QMP-quit
            # → atexit → final perf-log emit. Validated empirically
            # via `validate-native-tri-depth.sh` PASS recovery.
            for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
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

        # SIGTERM grace window also extended from 5 s to 15 s for the
        # same reason — xemu's signal handler runs the same atexit
        # sequence as QMP-quit, just slower than 5 s sometimes.
        for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
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

# M13 — programmatic Metal capture: when --metal-capture <path> was passed,
# export XEMU_METAL_CAPTURE so the Metal renderer's MTLCaptureManager writes
# a .gputrace there. Pre-exporting (vs inline `KEY=VAL "$XEMU"`) keeps the
# three launch branches below readable and avoids having to interleave the
# capture var into each of them.
if [[ -n "$METAL_CAPTURE_PATH" ]]; then
    export XEMU_METAL_CAPTURE="$METAL_CAPTURE_PATH"
    echo "Metal capture: $METAL_CAPTURE_PATH"
fi

# 2026-05-03 — programmatic PNG screenshot of the final composited
# drawable. Exports XEMU_METAL_SCREENSHOT_PATH so the Metal renderer
# captures the post-HUD-pre-present drawable into a PNG.
# --metal-screenshot-at-frame is optional (default 60); maps to
# XEMU_METAL_SCREENSHOT_AT_FRAME. Same export-pre-launch pattern as
# --metal-capture above.
if [[ -n "$METAL_SCREENSHOT_PATH" ]]; then
    export XEMU_METAL_SCREENSHOT_PATH="$METAL_SCREENSHOT_PATH"
    if [[ -n "$METAL_SCREENSHOT_AT_FRAME" ]]; then
        export XEMU_METAL_SCREENSHOT_AT_FRAME="$METAL_SCREENSHOT_AT_FRAME"
    fi
    echo "Metal screenshot: $METAL_SCREENSHOT_PATH (at frame=${METAL_SCREENSHOT_AT_FRAME:-60})"
fi

# Tool 2 (2026-05-19): temporal capture mode — every-frame PNG output
# suitable for `temporal-flicker-analyze.py`. Forces Metal renderer-
# native every-frame screenshot (source=nv2a, pre-HUD) OR a parallel
# ffmpeg AVFoundation primary-display capture for GL. Output lives at
# $RUN_DIR/frames/ — Metal writes metal-gameplay.NNNN.png directly via
# the existing in-renderer screenshot path; GL ffmpeg is spawned below
# after the xemu launch. Honors --metal-screenshot if already set
# (the operator's explicit path wins).
TEMPORAL_FRAMES_DIR=""
TEMPORAL_GL_MOV=""
if [[ "$TEMPORAL_CAPTURE" == "1" ]]; then
    TEMPORAL_FRAMES_DIR="${RUN_DIR}/frames"
    mkdir -p "$TEMPORAL_FRAMES_DIR"
    if [[ "${XEMU_RENDERER:-}" == "METAL" ]]; then
        if [[ -z "${XEMU_METAL_SCREENSHOT_PATH:-}" ]]; then
            export XEMU_METAL_SCREENSHOT_PATH="${TEMPORAL_FRAMES_DIR}/metal-gameplay.png"
        fi
        export XEMU_METAL_SCREENSHOT_AT_FRAME="${XEMU_METAL_SCREENSHOT_AT_FRAME:-1}"
        export XEMU_METAL_SCREENSHOT_INTERVAL="${XEMU_METAL_SCREENSHOT_INTERVAL:-1}"
        export XEMU_METAL_SCREENSHOT_SOURCE="${XEMU_METAL_SCREENSHOT_SOURCE:-nv2a}"
        echo "Temporal capture (METAL): every-frame PNG -> $XEMU_METAL_SCREENSHOT_PATH"
    fi
    # GL ffmpeg invocation is deferred until after xemu launches so the
    # display window is up before AVFoundation begins capture.
fi

# Tool 3 (2026-05-19): launcher prefix env var. Expanded unquoted
# before the xemu launch in each of the three launch branches below
# (record/live/scripted). Bash word-splits the variable on whitespace
# AND strips embedded quotes, so multi-word commands like
# `lldb -o "process status"` break. The supported contract is
# SINGLE-WORD ONLY — pass a path to an executable wrapper that
# internally invokes the multi-word debugger / tracer command.
# `scripts/apple-silicon/lldb-gl-launch.sh` generates such a wrapper.
LAUNCHER_PREFIX="${XEMU_BENCH_LAUNCHER_PREFIX:-}"
if [[ -n "$LAUNCHER_PREFIX" ]]; then
    echo "Launcher prefix: $LAUNCHER_PREFIX"
fi

if [[ -n "$RECORD_INPUT" ]]; then
    echo "Recording controller input: $RECORD_INPUT"
    XEMU_RECORD_INPUT="$RECORD_INPUT" \
    XEMU_RECORD_INPUT_PORT=1 \
    XEMU_PERF_LOG=1 \
    XEMU_PERF_LOG_INTERVAL_MS="$PERF_LOG_INTERVAL_MS" \
    XEMU_SNAPSHOT_NO_THUMBNAIL="$SNAPSHOT_NO_THUMBNAIL" \
    ${LAUNCHER_PREFIX} \
    "$XEMU" \
      -config_path "$CONFIG_FILE" \
      -qmp "unix:${QMP_SOCKET},server=on,wait=off" \
      ${EXTRA_QEMU_ARGS} \
      > "$LOG_FILE" 2>&1 &
elif [[ "$LIVE_INPUT" == "1" ]]; then
    echo "Using live controller input without recording"
    XEMU_PERF_LOG=1 \
    XEMU_PERF_LOG_INTERVAL_MS="$PERF_LOG_INTERVAL_MS" \
    XEMU_SNAPSHOT_NO_THUMBNAIL="$SNAPSHOT_NO_THUMBNAIL" \
    ${LAUNCHER_PREFIX} \
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
    ${LAUNCHER_PREFIX} \
    "$XEMU" \
      -config_path "$CONFIG_FILE" \
      -qmp "unix:${QMP_SOCKET},server=on,wait=off" \
      ${EXTRA_QEMU_ARGS} \
      > "$LOG_FILE" 2>&1 &
fi

XEMU_PID=$!
echo "$XEMU_PID" > "${RUN_DIR}/xemu.pid"

# Tool 2 (2026-05-19): GL temporal capture. ffmpeg AVFoundation captures
# the primary display in parallel with xemu. Downstream
# temporal-flicker-analyze.py crops to the xemu window via --gl-crop.
# AVFoundation device index 1 is the primary display on Apple Silicon
# (verified via `ffmpeg -f avfoundation -list_devices true` 2026-05-12).
TEMPORAL_FFMPEG_PID=""
if [[ "$TEMPORAL_CAPTURE" == "1" && "${XEMU_RENDERER:-}" == "GL" ]]; then
    TEMPORAL_GL_MOV="${TEMPORAL_FRAMES_DIR}/gameplay.mov"
    TEMPORAL_FFMPEG_LOG="${RUN_DIR}/ffmpeg-temporal.log"
    # Brief 1s settle so the xemu window is up before AVFoundation starts.
    sleep 1
    ffmpeg -hide_banner -y \
        -f avfoundation -framerate "$TEMPORAL_FPS" -capture_cursor 0 -i "1:none" \
        -t "$DURATION" \
        -c:v libx264 -preset ultrafast -crf 12 \
        "$TEMPORAL_GL_MOV" \
        > "$TEMPORAL_FFMPEG_LOG" 2>&1 &
    TEMPORAL_FFMPEG_PID=$!
    echo "Temporal capture (GL): ffmpeg pid $TEMPORAL_FFMPEG_PID -> $TEMPORAL_GL_MOV"
fi

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

# Tool 2 (2026-05-19): post-run, decompose the GL .mov into a PNG
# sequence so temporal-flicker-analyze.py (--glob 'gameplay-*.png')
# can read the frames. Metal already writes individual PNGs as it goes.
if [[ "$TEMPORAL_CAPTURE" == "1" && "${XEMU_RENDERER:-}" == "GL" && -n "${TEMPORAL_GL_MOV:-}" ]]; then
    if [[ -s "$TEMPORAL_GL_MOV" ]]; then
        echo "Decomposing $TEMPORAL_GL_MOV to PNG sequence..."
        ffmpeg -hide_banner -y \
            -i "$TEMPORAL_GL_MOV" \
            -vf "fps=$TEMPORAL_FPS" \
            "${TEMPORAL_FRAMES_DIR}/gameplay-%04d.png" \
            >> "${RUN_DIR}/ffmpeg-temporal.log" 2>&1 || \
            echo "warning: ffmpeg decompose failed; see ${RUN_DIR}/ffmpeg-temporal.log" >&2
    else
        echo "warning: ffmpeg .mov is empty/missing; nothing to decompose" >&2
    fi
fi

if [[ "$VISUAL_ANALYSIS" == "1" ]]; then
    VISUAL_ANALYSIS_LOG="${RUN_DIR}/visual-analysis.log"
    VISUAL_ANALYSIS_OUT="${RUN_DIR}/visual-analysis"
    VISUAL_FRAMES_DIR=""
    VISUAL_GLOB="*.png"

    if [[ -n "$METAL_SCREENSHOT_PATH" ]]; then
        VISUAL_FRAMES_DIR="$(dirname "$METAL_SCREENSHOT_PATH")"
        VISUAL_SHOT_NAME="$(basename "$METAL_SCREENSHOT_PATH")"
        if [[ "$VISUAL_SHOT_NAME" == *.png ]]; then
            VISUAL_GLOB="${VISUAL_SHOT_NAME%.png}*.png"
        else
            VISUAL_GLOB="${VISUAL_SHOT_NAME}*"
        fi
    elif [[ -d "$SCREENSHOT_DIR" ]]; then
        VISUAL_FRAMES_DIR="$SCREENSHOT_DIR"
    fi

    if [[ -n "$VISUAL_FRAMES_DIR" && -d "$VISUAL_FRAMES_DIR" ]]; then
        echo "Running visual analysis: $VISUAL_ANALYSIS_OUT"
        if ! python3 "${ROOT_DIR}/scripts/apple-silicon/visual-flight-recorder.py" \
            --run-dir "$RUN_DIR" \
            --frames-dir "$VISUAL_FRAMES_DIR" \
            --glob "$VISUAL_GLOB" \
            --out-dir "$VISUAL_ANALYSIS_OUT" \
            > "$VISUAL_ANALYSIS_LOG" 2>&1; then
            echo "visual analysis failed; see $VISUAL_ANALYSIS_LOG" >&2
        fi
    else
        echo "visual analysis requested but no screenshot frames were found" \
            > "$VISUAL_ANALYSIS_LOG"
    fi
fi

if [[ -n "$METAL_CAPTURE_PATH" ]]; then
    METAL_CAPTURE_MANIFEST="${RUN_DIR}/metal-capture-manifest.json"
    METAL_CAPTURE_MANIFEST_MD="${RUN_DIR}/metal-capture-manifest.md"
    if ! python3 "${ROOT_DIR}/scripts/apple-silicon/metal-capture-manifest.py" \
        --run-dir "$RUN_DIR" \
        --capture "$METAL_CAPTURE_PATH" \
        --out "$METAL_CAPTURE_MANIFEST" \
        --out-md "$METAL_CAPTURE_MANIFEST_MD" \
        > "${RUN_DIR}/metal-capture-manifest.log" 2>&1; then
        echo "metal capture manifest incomplete; see ${RUN_DIR}/metal-capture-manifest.log" >&2
    fi
fi

echo "Finished. Metadata: $META_FILE"
echo "Log: $LOG_FILE"
echo "Capture log: $CAPTURE_LOG"
echo "Snapshot log: $SNAPSHOT_LOG"
