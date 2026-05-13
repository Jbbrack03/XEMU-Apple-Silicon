#!/usr/bin/env bash
# Boot-animation temporal capture (T1, 2026-05-12).
#
# Records the Xbox BIOS-to-dashboard boot animation with PNG-every-frame
# resolution under a single renderer, suitable for downstream temporal
# flicker analysis (scripts/apple-silicon/temporal-flicker-analyze.py).
#
# Why a separate script:
#   run-benchmark.sh is built around scripted-input gameplay routes with
#   discrete per-game inputs. Boot-animation evidence is a different
#   workload — input-free, game-agnostic, very short, and needs every-
#   frame capture rather than every-N-seconds. Bolting it into the trio
#   harness would muddle the per-game launch matrix.
#
# Capture mechanism per renderer:
#   METAL: renderer-native PNG-every-frame via XEMU_METAL_SCREENSHOT_PATH
#          + AT_FRAME=1 + INTERVAL=1 + SOURCE=nv2a. Writes
#          frames/metal-boot.NNNN.png pre-HUD-composite.
#   GL:    parallel ffmpeg AVFoundation full-display capture at the
#          requested fps. AVFoundation device "1:none" is the primary
#          display, not the xemu window — the resulting frames include
#          the full macOS desktop and require post-processing crops
#          (see temporal-flicker-analyze.py --gl-crop) for paired
#          comparison against Metal's renderer-native NV2A surface.
#          Writes video frames/boot.mov and decomposes to
#          frames/boot-%04d.png after xemu exits. Includes macOS chrome
#          + xemu window (relevant when "missing elements" may live in
#          the UI compositor, not the NV2A renderer).
#
# Boot-only workload:
#   Uses the in-tree diagnostic XBE (flat-tri-depth.iso, 720 KB) as the
#   placeholder DVD media so xemu launches a full Xbox session. The
#   BIOS animation runs the same regardless of disc contents; truncate
#   the capture duration to stay inside the BIOS / dashboard window
#   (typical: ~8 s BIOS animation, dashboard reachable ~12 s after
#   launch). After that, flat-tri-depth begins rendering its own
#   geometry — still useful temporal signal but no longer "boot" data.
#
# Output layout:
#   benchmark-runs/<ts>-boot-<renderer>-temporal/
#     ├─ meta.txt             metadata snapshot
#     ├─ xemu.log             full xemu stderr+stdout
#     ├─ frames/
#     │   ├─ metal-boot.NNNN.png  (Metal leg)
#     │   └─ boot.mov + boot-NNNN.png  (GL leg)
#     ├─ ffmpeg.log           ffmpeg stderr (GL leg only)
#     └─ summary.json         per-leg counters + capture stats
#
# Idempotency:
#   - Honors project rule #9: never touches Xbox-Emulator-Files in place.
#     Creates a scratch HDD copy.
#   - Honors project rule #10: refuses to launch if another xemu is
#     already running unless XEMU_BENCH_ALLOW_EXISTING=1.
#   - Honors project rule #13: XEMU_SNAPSHOT_NO_THUMBNAIL=1.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_ROOT="${ROOT_DIR}/benchmark-runs"
XEMU="${ROOT_DIR}/dist/xemu.app/Contents/MacOS/xemu"

MCPX="/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/mcpx/mcpx_1.0.bin"
BIOS="/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/bios/Complex_4627.bin"
HDD_SOURCE="${XEMU_BENCH_HDD_SOURCE:-/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/hdd/xbox_hdd.qcow2}"
DISC="${ROOT_DIR}/scripts/apple-silicon/xbe-tests/flat-tri-depth/flat-tri-depth.iso"

usage() {
    cat <<EOF
usage: $0 --renderer {GL|METAL} [--duration SECONDS] [--fps N] [--out-name NAME]

Records the Xbox BIOS-to-dashboard boot animation with PNG-every-frame
temporal resolution under the chosen renderer.

Required:
  --renderer GL|METAL   Which renderer to capture.

Optional:
  --duration N          Capture duration in seconds. Default 18
                        (covers ~8 s BIOS animation + 10 s dashboard).
  --fps N               GL leg ffmpeg screen-capture fps. Default 60.
                        (Metal leg is always every-frame regardless.)
  --out-name NAME       Override run-dir basename. Default
                        <timestamp>-boot-<renderer>-temporal.
  --no-decompose        Skip the ffmpeg PNG decomposition step (GL only).
                        Useful when the .mov is the canonical artifact.

Environment overrides:
  XEMU_BENCH_HDD_SOURCE     Override source HDD image.
  XEMU_BENCH_ALLOW_EXISTING Permit launch when another xemu is running.

EOF
}

RENDERER=""
DURATION="18"
FPS="60"
OUT_NAME=""
DECOMPOSE="1"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --renderer)
            RENDERER="$(echo "$2" | tr '[:lower:]' '[:upper:]')"
            shift 2
            ;;
        --renderer=*)
            RENDERER="${1#--renderer=}"
            RENDERER="$(echo "$RENDERER" | tr '[:lower:]' '[:upper:]')"
            shift
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --duration=*)
            DURATION="${1#--duration=}"
            shift
            ;;
        --fps)
            FPS="$2"
            shift 2
            ;;
        --fps=*)
            FPS="${1#--fps=}"
            shift
            ;;
        --out-name)
            OUT_NAME="$2"
            shift 2
            ;;
        --out-name=*)
            OUT_NAME="${1#--out-name=}"
            shift
            ;;
        --no-decompose)
            DECOMPOSE="0"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown arg: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$RENDERER" != "GL" && "$RENDERER" != "METAL" ]]; then
    echo "--renderer GL|METAL is required" >&2
    usage >&2
    exit 2
fi

for f in "$XEMU" "$MCPX" "$BIOS" "$HDD_SOURCE" "$DISC"; do
    if [[ ! -e "$f" ]]; then
        echo "missing required file: $f" >&2
        exit 1
    fi
done

if [[ "${XEMU_BENCH_ALLOW_EXISTING:-0}" != "1" ]]; then
    if pgrep -x xemu >/dev/null 2>&1; then
        echo "another xemu process is running; refuse to launch (rule #10)." >&2
        echo "set XEMU_BENCH_ALLOW_EXISTING=1 to override." >&2
        exit 1
    fi
fi

TS="$(date +%Y%m%dT%H%M%SZ)"
if [[ -z "$OUT_NAME" ]]; then
    OUT_NAME="${TS}-boot-$(echo "$RENDERER" | tr '[:upper:]' '[:lower:]')-temporal"
fi
RUN_DIR="${RUN_ROOT}/${OUT_NAME}"
FRAMES_DIR="${RUN_DIR}/frames"
# Refuse to clobber a previous run when --out-name reuses a dir that
# already has PNGs in it. Stale frames mixed into a fresh capture would
# silently contaminate the temporal-flicker analyzer. Auto-generated
# timestamp out-names are unique by construction; this guard only
# trips when the operator explicitly reuses a name.
if [[ -d "$FRAMES_DIR" ]] && \
   find "$FRAMES_DIR" -maxdepth 1 -type f -name '*.png' -print -quit 2>/dev/null | grep -q .; then
    echo "refuse to overwrite existing non-empty frames dir: $FRAMES_DIR" >&2
    echo "remove it (rm -rf '$RUN_DIR') or pass a different --out-name." >&2
    exit 1
fi
mkdir -p "$FRAMES_DIR"

LOG_FILE="${RUN_DIR}/xemu.log"
META_FILE="${RUN_DIR}/meta.txt"
FFMPEG_LOG="${RUN_DIR}/ffmpeg.log"
SUMMARY_FILE="${RUN_DIR}/summary.json"
CONFIG_FILE="${RUN_DIR}/xemu.toml"
SCRATCH_HDD="${RUN_DIR}/scratch_hdd.qcow2"
# macOS UNIX-socket paths are capped at 104 bytes (sys/un.h sun_path[104]).
# Long run-dir names (timestamps + descriptive suffixes) overflow that
# budget when the socket lives under benchmark-runs/, so park it in /tmp
# under a short fingerprint and symlink for traceability.
QMP_SOCKET="/tmp/xemu-qmp-${TS}-$$.sock"
ln -sf "$QMP_SOCKET" "${RUN_DIR}/qmp.sock" 2>/dev/null || true

echo "Copying HDD image..."
cp "$HDD_SOURCE" "$SCRATCH_HDD"

cat > "$CONFIG_FILE" <<EOF
[general]
show_welcome = false

[input]
auto_bind = false
background_input_capture = false

[display.window]
vsync = false

[display.quality]
surface_scale = 2

[sys.files]
bootrom_path = '$MCPX'
flashrom_path = '$BIOS'
hdd_path = '$SCRATCH_HDD'
dvd_path = '$DISC'
EOF

{
    echo "date: $(date)"
    echo "renderer: $RENDERER"
    echo "duration_seconds: $DURATION"
    echo "fps_request: $FPS"
    echo "run_dir: $RUN_DIR"
    echo "xemu: $XEMU"
    echo "disc: $DISC"
    echo "hdd_source: $HDD_SOURCE"
    echo "scratch_hdd: $SCRATCH_HDD"
    echo "config_file: $CONFIG_FILE"
    echo "frames_dir: $FRAMES_DIR"
    echo
    sw_vers || true
    uname -m || true
    sysctl -n machdep.cpu.brand_string 2>/dev/null || true
    git -C "$ROOT_DIR" rev-parse HEAD || true
    git -C "$ROOT_DIR" status --short || true
} > "$META_FILE"

cleanup() {
    if [[ -n "${FFMPEG_PID:-}" ]] && kill -0 "$FFMPEG_PID" 2>/dev/null; then
        # Politely ask ffmpeg to finalize the .mov; SIGINT triggers
        # graceful container close.
        kill -INT "$FFMPEG_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            if ! kill -0 "$FFMPEG_PID" 2>/dev/null; then break; fi
            sleep 0.5
        done
        kill -KILL "$FFMPEG_PID" 2>/dev/null || true
        wait "$FFMPEG_PID" 2>/dev/null || true
    fi
    if [[ -n "${XEMU_PID:-}" ]] && kill -0 "$XEMU_PID" 2>/dev/null; then
        if [[ -S "$QMP_SOCKET" ]]; then
            python3 "${ROOT_DIR}/scripts/apple-silicon/qmp-hmp.py" \
                --socket "$QMP_SOCKET" quit >/dev/null 2>&1 || true
        fi
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            if ! kill -0 "$XEMU_PID" 2>/dev/null; then break; fi
            sleep 1
        done
        kill "$XEMU_PID" 2>/dev/null || true
        sleep 2
        kill -KILL "$XEMU_PID" 2>/dev/null || true
        wait "$XEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

echo "Starting $RENDERER boot capture for ${DURATION}s"
echo "Run directory: $RUN_DIR"

# Build the xemu environment per renderer.
export XEMU_RENDERER="$RENDERER"
export XEMU_PERF_LOG=1
export XEMU_PERF_FRAME_LOG=1
export XEMU_PERF_LOG_INTERVAL_MS=1000
export XEMU_SNAPSHOT_NO_THUMBNAIL=1
# Honor the closed default-on Apple Silicon opt-in flags so the boot
# path uses the same renderer config as gameplay runs.
export XEMU_NATIVE_TRI_DEPTH="${XEMU_NATIVE_TRI_DEPTH:-1}"
export XEMU_NATIVE_QUAD="${XEMU_NATIVE_QUAD:-1}"
export XEMU_PGRAPH_FAST_READ="${XEMU_PGRAPH_FAST_READ:-1}"

if [[ "$RENDERER" == "METAL" ]]; then
    export XEMU_METAL_SCREENSHOT_PATH="${FRAMES_DIR}/metal-boot.png"
    export XEMU_METAL_SCREENSHOT_AT_FRAME="${XEMU_METAL_SCREENSHOT_AT_FRAME:-1}"
    export XEMU_METAL_SCREENSHOT_INTERVAL="${XEMU_METAL_SCREENSHOT_INTERVAL:-1}"
    export XEMU_METAL_SCREENSHOT_SOURCE="${XEMU_METAL_SCREENSHOT_SOURCE:-nv2a}"
    # Match the default canary env used by the existing MSAA4 PNG gates.
    export XEMU_METAL_TRANSLATED_PIPELINE="${XEMU_METAL_TRANSLATED_PIPELINE:-1}"
    export XEMU_METAL_MSAA="${XEMU_METAL_MSAA:-4}"
    echo "Metal screenshot env: PATH=$XEMU_METAL_SCREENSHOT_PATH AT_FRAME=$XEMU_METAL_SCREENSHOT_AT_FRAME INTERVAL=$XEMU_METAL_SCREENSHOT_INTERVAL SOURCE=$XEMU_METAL_SCREENSHOT_SOURCE"
fi

"$XEMU" \
    -config_path "$CONFIG_FILE" \
    -qmp "unix:${QMP_SOCKET},server=on,wait=off" \
    > "$LOG_FILE" 2>&1 &
XEMU_PID=$!
echo "xemu pid: $XEMU_PID"

# For the GL leg we drive screen capture from ffmpeg+AVFoundation in
# parallel. AVFoundation screen-input index "1" is the primary display
# on Apple Silicon (verified via `ffmpeg -f avfoundation -list_devices
# true` at script ship time, but is machine-local — `--list-devices`
# enumeration on a multi-display Mac may shift indices). Captures the
# entire primary display, not the xemu window; downstream analyzers
# must crop to the xemu region (see temporal-flicker-analyze.py
# --gl-crop). Renderer-native every-frame for GL would require
# extending pgraph_gl_capture_display_if_requested to interval mode;
# that's a follow-up refinement if AVFoundation evidence proves
# insufficient.
if [[ "$RENDERER" == "GL" ]]; then
    sleep 1
    BOOT_MOV="${FRAMES_DIR}/boot.mov"
    ffmpeg -hide_banner -y \
        -f avfoundation -framerate "$FPS" -capture_cursor 0 -i "1:none" \
        -t "$DURATION" \
        -c:v libx264 -preset ultrafast -crf 12 \
        "$BOOT_MOV" \
        > "$FFMPEG_LOG" 2>&1 &
    FFMPEG_PID=$!
    echo "ffmpeg pid: $FFMPEG_PID  (writing $BOOT_MOV @ ${FPS}fps)"
fi

# Wait for the capture window; xemu keeps running.
sleep "$DURATION"

# Stop the capture leg first.
if [[ -n "${FFMPEG_PID:-}" ]] && kill -0 "$FFMPEG_PID" 2>/dev/null; then
    kill -INT "$FFMPEG_PID" 2>/dev/null || true
    wait "$FFMPEG_PID" 2>/dev/null || true
    FFMPEG_PID=""
fi

# Then graceful xemu quit.
if [[ -S "$QMP_SOCKET" ]]; then
    python3 "${ROOT_DIR}/scripts/apple-silicon/qmp-hmp.py" \
        --socket "$QMP_SOCKET" quit >/dev/null 2>&1 || true
fi
wait "$XEMU_PID" 2>/dev/null || true
XEMU_PID=""
trap - EXIT INT TERM

# Decompose ffmpeg .mov to PNG sequence for the GL leg unless suppressed.
if [[ "$RENDERER" == "GL" && "$DECOMPOSE" == "1" ]]; then
    BOOT_MOV="${FRAMES_DIR}/boot.mov"
    if [[ -s "$BOOT_MOV" ]]; then
        echo "Decomposing $BOOT_MOV to PNG sequence..."
        ffmpeg -hide_banner -y \
            -i "$BOOT_MOV" \
            -vf "fps=$FPS" \
            "${FRAMES_DIR}/boot-%04d.png" \
            >> "$FFMPEG_LOG" 2>&1 || \
            echo "warning: ffmpeg decompose failed; see $FFMPEG_LOG" >&2
    else
        echo "warning: ffmpeg .mov is empty/missing; nothing to decompose" >&2
    fi
fi

# Summary stats. `ls glob | wc -l` aborts under `set -euo pipefail` when
# the glob matches nothing, which defeats the intent of writing a zero-
# frame summary on capture failure. `find -maxdepth 1` handles the empty
# case cleanly.
FRAME_COUNT=0
if [[ "$RENDERER" == "METAL" ]]; then
    FRAME_COUNT=$(find "$FRAMES_DIR" -maxdepth 1 -type f -name 'metal-boot.*.png' 2>/dev/null | wc -l | tr -d ' ')
else
    FRAME_COUNT=$(find "$FRAMES_DIR" -maxdepth 1 -type f -name 'boot-*.png' 2>/dev/null | wc -l | tr -d ' ')
fi
FRAME_COUNT="${FRAME_COUNT:-0}"

PRESENT_HEARTBEATS=$(grep -oE 'NV2A_PRESENT_HEARTBEAT=[0-9]+' "$LOG_FILE" 2>/dev/null | tail -1 | cut -d= -f2 || true)
PRESENT_HEARTBEATS="${PRESENT_HEARTBEATS:-0}"
METAL_PRESENT_TOTAL=$(grep -oE 'METAL_PRESENT_TOTAL=[0-9]+' "$LOG_FILE" 2>/dev/null | tail -1 | cut -d= -f2 || true)
METAL_PRESENT_TOTAL="${METAL_PRESENT_TOTAL:-0}"

python3 - "$SUMMARY_FILE" "$RENDERER" "$DURATION" "$FPS" "$FRAME_COUNT" "$PRESENT_HEARTBEATS" "$METAL_PRESENT_TOTAL" "$FRAMES_DIR" <<'PY'
import json, sys, os
out = sys.argv[1]
renderer = sys.argv[2]
duration = float(sys.argv[3])
fps = int(sys.argv[4])
frame_count = int(sys.argv[5])
present_heartbeats = int(sys.argv[6] or 0)
metal_present_total = int(sys.argv[7] or 0)
frames_dir = sys.argv[8]
report = {
    "renderer": renderer,
    "duration_seconds": duration,
    "fps_request": fps,
    "frame_count": frame_count,
    "frames_dir": frames_dir,
    "nv2a_present_heartbeat_last_interval": present_heartbeats,
    "metal_present_total_last_interval": metal_present_total,
    "effective_fps_estimate": frame_count / duration if duration > 0 else 0,
    "evidence_class": "boot-animation-temporal",
}
with open(out, "w") as f:
    json.dump(report, f, indent=2, sort_keys=True)
print(json.dumps(report, indent=2, sort_keys=True))
PY

echo
echo "Done. Run dir: $RUN_DIR"
echo "Frames: $FRAME_COUNT"
echo "Summary: $SUMMARY_FILE"
