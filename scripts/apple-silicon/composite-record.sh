#!/bin/bash
#
# composite-record.sh — record synchronized video+audio from the
# MS2109 USB composite-capture stick. Wraps `ffmpeg -f avfoundation`
# with the NTSC 720x480 @ 30 fps format-pinning the project Xbox
# emits, plus 48 kHz stereo audio from the same stick's UAC interface.
#
# This is the third oracle leg's video+audio twin: the visual analog
# of agent-side `screenshot` (one frame, pixel-exact) but capturing
# many seconds of live gameplay so we can extract keyframes and a
# waveform PNG to compare against xemu's video+audio output.
#
# USAGE
#   composite-record.sh [--duration SECONDS] [--out-dir DIR]
#                       [--device NAME] [--audio-device NAME]
#                       [--width N] [--height N] [--fps N]
#                       [--no-audio] [--label NAME]
#
# DEFAULTS
#   --duration         15
#   --out-dir          xemu-fork/benchmark-runs/<UTC-stamp>-composite
#   --device           USB2          (substring match, MS2109 stick)
#   --audio-device     USB2          (same stick, UAC interface)
#   --width / height   720 / 480     (NTSC; matches the Xbox composite mode
#                                     used in the project's xbe-tests)
#   --fps              30            (NTSC field-pair rate)
#   --label            ""            (optional tag baked into the run dir name)
#
# OUTPUT (under <out-dir>)
#   video.mp4            H.264 video + AAC audio, mp4 container
#   capture-meta.json    device names, format, duration, ffmpeg cmd, exit-code
#   capture-stderr.log   raw ffmpeg log (kept for debugging device errors)
#
# ENVIRONMENT
#   FFMPEG       path to ffmpeg (default: looked up via $PATH)
#   FFMPEG_LOG   ffmpeg -loglevel value (default: info)
#   CAPTURE_TIMEOUT_EXTRA
#                seconds past --duration before killing a stuck ffmpeg
#                process (default: 20)
#
# SAFETY
#   - The MS2109 brown-outs flaky USB-C ports on Mac Studio's ASMedia
#     3142 controller. If ffmpeg fails with "Input/output error" or
#     similar device-side errors, try a back USB-A port instead.
#   - This script does NOT switch the device's input (composite vs
#     S-Video) — it relies on hardware sync detection. To force the
#     input, invoke `xemu-capture set-input` first.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

DURATION=15
OUT_DIR=""
DEVICE="USB2"
AUDIO_DEVICE="USB2"
WIDTH=720
HEIGHT=480
FPS=30
LABEL=""
INCLUDE_AUDIO=1
PIXEL_FORMAT="uyvy422"
TIMEOUT_EXTRA="${CAPTURE_TIMEOUT_EXTRA:-20}"

while [ $# -gt 0 ]; do
    case "$1" in
        --duration)        DURATION="$2"; shift 2 ;;
        --out-dir)         OUT_DIR="$2"; shift 2 ;;
        --device)          DEVICE="$2"; shift 2 ;;
        --audio-device)    AUDIO_DEVICE="$2"; shift 2 ;;
        --width)           WIDTH="$2"; shift 2 ;;
        --height)          HEIGHT="$2"; shift 2 ;;
        --fps)             FPS="$2"; shift 2 ;;
        --label)           LABEL="$2"; shift 2 ;;
        --no-audio)        INCLUDE_AUDIO=0; shift ;;
        --pixel-format)    PIXEL_FORMAT="$2"; shift 2 ;;
        --timeout-extra)   TIMEOUT_EXTRA="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,40p' "${BASH_SOURCE[0]}" | sed -e 's/^# *//'
            exit 0 ;;
        *)
            echo "unknown arg: $1" >&2
            exit 2 ;;
    esac
done

FFMPEG="${FFMPEG:-$(command -v ffmpeg || true)}"
if [ -z "$FFMPEG" ]; then
    echo "ffmpeg not found; install via 'brew install ffmpeg' or set \$FFMPEG" >&2
    exit 1
fi

FFMPEG_LOG="${FFMPEG_LOG:-info}"

if [ -z "$OUT_DIR" ]; then
    STAMP="$(date -u +%Y%m%d-%H%M%S)"
    SUFFIX="composite"
    [ -n "$LABEL" ] && SUFFIX="composite-$LABEL"
    OUT_DIR="$PROJECT_ROOT/benchmark-runs/$STAMP-$SUFFIX"
fi
mkdir -p "$OUT_DIR"

VIDEO_OUT="$OUT_DIR/video.mp4"
META_OUT="$OUT_DIR/capture-meta.json"
LOG_OUT="$OUT_DIR/capture-stderr.log"

# AVFoundation device-name resolution. ffmpeg only accepts EXACT
# device names or numeric indices, never substrings — even though
# the macOS SDK matches case-insensitive substrings via xemu-capture.
# Resolve substring → numeric index by parsing `-list_devices true`.
# Indices are unstable across plug/unplug, but they are stable for
# the duration of one record session, which is what we need.
DEVICE_LIST="$("$FFMPEG" -hide_banner -f avfoundation \
    -list_devices true -i "" 2>&1 || true)"

resolve_device() {
    # $1 = pattern (substring, case-insensitive), $2 = "video"|"audio"
    local pat lower kind
    pat="$1"; kind="$2"
    lower="$(printf '%s' "$pat" | tr '[:upper:]' '[:lower:]')"
    # The device list groups video then audio with a section header.
    awk -v pat="$lower" -v kind="$kind" '
        /AVFoundation video devices:/ { section="video"; next }
        /AVFoundation audio devices:/ { section="audio"; next }
        section == kind {
            line = $0
            # Match e.g. "[AVFoundation indev @ 0x...] [N] Name"
            if (match(line, /\] \[[0-9]+\] /)) {
                idxpart = substr(line, RSTART+2, RLENGTH-4)
                gsub(/[\[\] ]/, "", idxpart)
                name = substr(line, RSTART + RLENGTH)
                lname = tolower(name)
                if (index(lname, pat) > 0) {
                    print idxpart
                    exit
                }
            }
        }
    ' <<< "$DEVICE_LIST"
}

VIDEO_IDX="$(resolve_device "$DEVICE" video)"
if [ -z "$VIDEO_IDX" ]; then
    echo "[composite-record] no video device matched '$DEVICE'" >&2
    echo "[composite-record] available devices:" >&2
    printf '%s\n' "$DEVICE_LIST" | grep -E "AVFoundation (video|audio) devices|\] \[[0-9]+\]" >&2 || true
    exit 1
fi

if [ "$INCLUDE_AUDIO" -eq 1 ]; then
    AUDIO_IDX="$(resolve_device "$AUDIO_DEVICE" audio)"
    if [ -z "$AUDIO_IDX" ]; then
        echo "[composite-record] no audio device matched '$AUDIO_DEVICE'" >&2
        echo "[composite-record] (use --no-audio to skip audio capture)" >&2
        exit 1
    fi
    AVFD_INPUT="$VIDEO_IDX:$AUDIO_IDX"
else
    AVFD_INPUT="$VIDEO_IDX:none"
fi
echo "[composite-record] resolved video='$DEVICE'->[$VIDEO_IDX] audio='${AUDIO_DEVICE:-N/A}'->[${AUDIO_IDX:-N/A}]"

CMD=(
    "$FFMPEG"
    -hide_banner
    -y
    -loglevel "$FFMPEG_LOG"
    -f avfoundation
    -framerate "$FPS"
    -video_size "${WIDTH}x${HEIGHT}"
    -pixel_format "$PIXEL_FORMAT"
    -i "$AVFD_INPUT"
    -t "$DURATION"
    # Re-encode to H.264; AVFoundation's raw uyvy422 input is fine for
    # a passthrough but mp4 with H.264 is what every analysis tool
    # downstream (ffmpeg select=scene, ffprobe, Quick Look) handles
    # natively. Use videotoolbox (HW-accelerated on Apple Silicon)
    # for both speed and battery; no CPU-bound encoder choice.
    -c:v h264_videotoolbox
    -b:v 6M
    -pix_fmt yuv420p
)
if [ "$INCLUDE_AUDIO" -eq 1 ]; then
    CMD+=(
        -c:a aac
        -b:a 192k
        -ac 2
    )
fi
CMD+=("$VIDEO_OUT")

# Best-effort metadata pre-write so we can recover even on ffmpeg
# crash. The post-run pass below adds exit-code + observed duration.
START_EPOCH="$(date -u +%s)"
START_ISO="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
{
    printf '{\n'
    printf '  "started_at": "%s",\n' "$START_ISO"
    printf '  "device": "%s",\n' "$DEVICE"
    printf '  "audio_device": "%s",\n' "$AUDIO_DEVICE"
    printf '  "include_audio": %s,\n' "$([ "$INCLUDE_AUDIO" -eq 1 ] && echo true || echo false)"
    printf '  "width": %s,\n' "$WIDTH"
    printf '  "height": %s,\n' "$HEIGHT"
    printf '  "fps": %s,\n' "$FPS"
    printf '  "duration_target_s": %s,\n' "$DURATION"
    printf '  "pixel_format": "%s",\n' "$PIXEL_FORMAT"
    printf '  "label": "%s",\n' "$LABEL"
    printf '  "ffmpeg": "%s",\n' "$FFMPEG"
    printf '  "video_out": "%s",\n' "$VIDEO_OUT"
    printf '  "stderr_log": "%s",\n' "$LOG_OUT"
    printf '  "ffmpeg_argv": ['
    first=1
    for a in "${CMD[@]}"; do
        if [ "$first" -eq 0 ]; then printf ','; fi
        printf '\n    %s' "$(printf '%s' "$a" | python3 -c 'import sys, json; print(json.dumps(sys.stdin.read()))')"
        first=0
    done
    printf '\n  ],\n'
    printf '  "status": "in_progress"\n'
    printf '}\n'
} > "$META_OUT"

echo "[composite-record] starting ffmpeg ($DURATION s)..."
echo "[composite-record] cmd: ${CMD[*]}"
echo "[composite-record] log: $LOG_OUT"

set +e
"${CMD[@]}" 2> "$LOG_OUT" &
CAPTURE_PID=$!
CAPTURE_TIMED_OUT=0
DEADLINE="$(python3 - "$DURATION" "$TIMEOUT_EXTRA" <<'PY'
import sys, time
print(time.time() + float(sys.argv[1]) + float(sys.argv[2]))
PY
)"
while kill -0 "$CAPTURE_PID" 2>/dev/null; do
    if python3 - "$DEADLINE" <<'PY'
import sys, time
raise SystemExit(0 if time.time() > float(sys.argv[1]) else 1)
PY
    then
        CAPTURE_TIMED_OUT=1
        echo "[composite-record] timeout; killing ffmpeg pid=$CAPTURE_PID" >&2
        kill "$CAPTURE_PID" 2>/dev/null || true
        sleep 2
        kill -9 "$CAPTURE_PID" 2>/dev/null || true
        break
    fi
    sleep 1
done
wait "$CAPTURE_PID"
RC=$?
set -e

END_EPOCH="$(date -u +%s)"
ELAPSED=$((END_EPOCH - START_EPOCH))

# Probe actual video duration from the produced file (ffmpeg sometimes
# writes shorter than -t when the device drops or the bus stalls).
ACTUAL_DURATION=""
if [ -f "$VIDEO_OUT" ]; then
    if command -v ffprobe >/dev/null; then
        ACTUAL_DURATION="$(ffprobe -v error -show_entries format=duration \
                           -of default=nw=1:nk=1 "$VIDEO_OUT" 2>/dev/null || true)"
    fi
fi

# Patch the metadata file with status + observed duration.
python3 - "$META_OUT" "$RC" "$ELAPSED" "$ACTUAL_DURATION" "$CAPTURE_TIMED_OUT" <<'PY'
import json, sys
meta_path, rc, elapsed, actual, timed_out = sys.argv[1:6]
with open(meta_path) as f:
    meta = json.load(f)
meta["status"] = "ok" if rc == "0" else "ffmpeg-failed"
meta["ffmpeg_rc"] = int(rc)
meta["wall_elapsed_s"] = int(elapsed)
meta["capture_timed_out"] = timed_out == "1"
if actual:
    try:
        meta["video_duration_s"] = float(actual)
    except ValueError:
        meta["video_duration_s"] = None
with open(meta_path, "w") as f:
    json.dump(meta, f, indent=2)
PY

if [ "$RC" -ne 0 ]; then
    echo "[composite-record] FAILED rc=$RC; tail of log:"
    tail -20 "$LOG_OUT"
    exit "$RC"
fi

VIDEO_BYTES="$(stat -f%z "$VIDEO_OUT" 2>/dev/null || echo 0)"
echo "[composite-record] OK"
echo "  video    : $VIDEO_OUT ($VIDEO_BYTES bytes; ${ACTUAL_DURATION:-?}s)"
echo "  metadata : $META_OUT"
echo "  log      : $LOG_OUT"
