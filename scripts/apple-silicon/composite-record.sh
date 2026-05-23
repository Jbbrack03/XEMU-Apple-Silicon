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
#                       [--skip-preflight] [--preflight-timeout SECONDS]
#                       [--preflight-mode auto|xemu-capture|ffmpeg]
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
#   --preflight-timeout 8            (composite-preflight.sh wall-clock seconds)
#   --preflight-mode   auto          (prefer xemu-capture, fall back to ffmpeg)
#
# PREFLIGHT (cycle 33, 2026-05-23)
#   By default composite-record.sh runs composite-preflight.sh first to
#   confirm the MS2109 is actually producing frames BEFORE arming the
#   long ffmpeg capture. On preflight failure the run aborts WITHOUT
#   touching ffmpeg, prints the cycle-32 physical-side checklist, and
#   exits non-zero. Pass --skip-preflight to bypass (e.g. when probing
#   the capture path itself or when xemu-capture / ffmpeg one-shot
#   probes are known to interact badly with the stick).
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
#   COMPOSITE_PREFLIGHT_TIMEOUT
#                default wall-clock seconds for the composite-preflight.sh
#                fail-fast probe (default: 8). Equivalent to passing
#                --preflight-timeout SECONDS on the CLI.
#   COMPOSITE_PREFLIGHT_MODE
#                default detector mode for the preflight (auto|xemu-capture|ffmpeg;
#                default: auto). Equivalent to --preflight-mode.
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
SKIP_PREFLIGHT=0
PREFLIGHT_TIMEOUT="${COMPOSITE_PREFLIGHT_TIMEOUT:-8}"
PREFLIGHT_MODE="${COMPOSITE_PREFLIGHT_MODE:-auto}"

while [ $# -gt 0 ]; do
    case "$1" in
        --duration)            DURATION="$2"; shift 2 ;;
        --out-dir)             OUT_DIR="$2"; shift 2 ;;
        --device)              DEVICE="$2"; shift 2 ;;
        --audio-device)        AUDIO_DEVICE="$2"; shift 2 ;;
        --width)               WIDTH="$2"; shift 2 ;;
        --height)              HEIGHT="$2"; shift 2 ;;
        --fps)                 FPS="$2"; shift 2 ;;
        --label)               LABEL="$2"; shift 2 ;;
        --no-audio)            INCLUDE_AUDIO=0; shift ;;
        --pixel-format)        PIXEL_FORMAT="$2"; shift 2 ;;
        --timeout-extra)       TIMEOUT_EXTRA="$2"; shift 2 ;;
        --skip-preflight)      SKIP_PREFLIGHT=1; shift ;;
        --preflight-timeout)   PREFLIGHT_TIMEOUT="$2"; shift 2 ;;
        --preflight-mode)      PREFLIGHT_MODE="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,50p' "${BASH_SOURCE[0]}" | sed -e 's/^# *//'
            exit 0 ;;
        *)
            echo "unknown arg: $1" >&2
            exit 2 ;;
    esac
done

FFMPEG="${FFMPEG:-$(command -v ffmpeg || true)}"

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

# Cycle-33 closeout (Codex round-7 medium #3 + Codex round-3 closeout
# minor adopted): if ffmpeg is missing OR the FFMPEG override points
# at a non-executable path, write a structured stub capture-meta.json
# before exiting instead of falling through into the device-resolution
# path (where a swallowed `ffmpeg -list_devices` failure would later
# surface as a misleading `status="device-not-found"` rc=3 host-error
# masquerading as a hardware-error). Unattended callers depend on the
# host-error vs hardware-error split the preflight contract preserves.
if [ -z "$FFMPEG" ] || [ ! -x "$FFMPEG" ]; then
    if [ -n "$FFMPEG" ] && [ ! -x "$FFMPEG" ]; then
        echo "ffmpeg override '$FFMPEG' is not executable (or not a file); install via 'brew install ffmpeg' or unset \$FFMPEG" >&2
    else
        echo "ffmpeg not found; install via 'brew install ffmpeg' or set \$FFMPEG" >&2
    fi
    python3 - "$META_OUT" "$DEVICE" "$AUDIO_DEVICE" \
        "$INCLUDE_AUDIO" "$WIDTH" "$HEIGHT" "$FPS" "$DURATION" \
        "$PIXEL_FORMAT" "$LABEL" <<'PY'
import json, sys, datetime
(meta_path, device, audio_device, include_audio,
 width, height, fps, duration, pixel_format, label) = sys.argv[1:11]
obj = {
    "schema": "composite-record/v1",
    "started_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "status": "preflight-failed",
    "preflight": {
        "status": "no_backend",
        "exit_code": 4,
        "detector": "none",
        "elapsed_s": None,
        "meta_path": None,
        "reason": "ffmpeg not found on $PATH and $FFMPEG override empty; composite-record cannot run.",
    },
    "device": device,
    "audio_device": audio_device,
    "include_audio": include_audio == "1",
    "width": int(width),
    "height": int(height),
    "fps": int(fps),
    "duration_target_s": int(duration),
    "pixel_format": pixel_format,
    "label": label,
    "ffmpeg_invoked": False,
}
with open(meta_path, "w") as f:
    json.dump(obj, f, indent=2)
    f.write("\n")
PY
    exit 4
fi

# --- Preflight (cycle 33, 2026-05-23) ----------------------------------
# Fail fast on a no-signal MS2109 before arming the long ffmpeg
# capture. Records the preflight result alongside capture-meta.json so
# unattended runs preserve evidence of why the capture leg was aborted.
PREFLIGHT_DIR="$OUT_DIR/preflight"
PREFLIGHT_META=""
PREFLIGHT_STATUS="skipped"
PREFLIGHT_DETECTOR="none"
PREFLIGHT_RC=0
PREFLIGHT_ELAPSED_S=""
if [ "$SKIP_PREFLIGHT" -eq 0 ]; then
    PREFLIGHT_BIN="$SCRIPT_DIR/composite-preflight.sh"
    if [ ! -x "$PREFLIGHT_BIN" ]; then
        echo "[composite-record] composite-preflight.sh missing or not executable at $PREFLIGHT_BIN" >&2
        echo "[composite-record] pass --skip-preflight to bypass intentionally" >&2
        # Cycle-33 closeout (Codex round-5 medium #2 adopted): write a
        # structured stub capture-meta.json BEFORE exiting so unattended
        # callers see the documented "preflight-failed" failure marker
        # in this branch too, not a missing file.
        python3 - "$META_OUT" "$DEVICE" "$AUDIO_DEVICE" \
            "$INCLUDE_AUDIO" "$WIDTH" "$HEIGHT" "$FPS" "$DURATION" \
            "$PIXEL_FORMAT" "$LABEL" "$PREFLIGHT_BIN" <<'PY'
import json, os, sys, datetime
(meta_path, device, audio_device, include_audio,
 width, height, fps, duration, pixel_format, label,
 preflight_bin) = sys.argv[1:12]
obj = {
    "schema": "composite-record/v1",
    "started_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "status": "preflight-failed",
    "preflight": {
        "status": "launcher_missing",
        "exit_code": 4,
        "detector": "none",
        "elapsed_s": None,
        "meta_path": None,
        "launcher_path": preflight_bin,
        "launcher_executable": os.access(preflight_bin, os.X_OK),
    },
    "device": device,
    "audio_device": audio_device,
    "include_audio": include_audio == "1",
    "width": int(width),
    "height": int(height),
    "fps": int(fps),
    "duration_target_s": int(duration),
    "pixel_format": pixel_format,
    "label": label,
    "ffmpeg_invoked": False,
}
with open(meta_path, "w") as f:
    json.dump(obj, f, indent=2)
    f.write("\n")
PY
        exit 4
    fi
    mkdir -p "$PREFLIGHT_DIR"
    PREFLIGHT_ARGS=(
        --device "$DEVICE"
        --audio-device "$AUDIO_DEVICE"
        --width "$WIDTH"
        --height "$HEIGHT"
        --fps "$FPS"
        --pixel-format "$PIXEL_FORMAT"
        --timeout "$PREFLIGHT_TIMEOUT"
        --mode "$PREFLIGHT_MODE"
        --out-dir "$PREFLIGHT_DIR"
    )
    [ "$INCLUDE_AUDIO" -eq 0 ] && PREFLIGHT_ARGS+=(--no-audio)
    echo "[composite-record] preflight: $PREFLIGHT_BIN ${PREFLIGHT_ARGS[*]}"
    set +e
    "$PREFLIGHT_BIN" "${PREFLIGHT_ARGS[@]}"
    PREFLIGHT_RC=$?
    set -e
    PREFLIGHT_META="$PREFLIGHT_DIR/preflight-meta.json"
    if [ -f "$PREFLIGHT_META" ]; then
        PREFLIGHT_STATUS="$(python3 - "$PREFLIGHT_META" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as f:
        print(json.load(f).get("status", "unknown"))
except Exception:
    print("unknown")
PY
)"
        PREFLIGHT_DETECTOR="$(python3 - "$PREFLIGHT_META" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as f:
        print(json.load(f).get("detector", "none") or "none")
except Exception:
    print("none")
PY
)"
        PREFLIGHT_ELAPSED_S="$(python3 - "$PREFLIGHT_META" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as f:
        v = json.load(f).get("elapsed_s")
    print("" if v is None else f"{float(v):.3f}")
except Exception:
    print("")
PY
)"
    fi
    if [ "$PREFLIGHT_RC" -ne 0 ]; then
        echo "[composite-record] preflight FAILED (status=$PREFLIGHT_STATUS, rc=$PREFLIGHT_RC, detector=$PREFLIGHT_DETECTOR)" >&2
        echo "[composite-record] meta: $PREFLIGHT_META" >&2
        echo "[composite-record] aborting BEFORE arming ffmpeg to prevent a cycle-32 OUTCOME F8 silent stall." >&2
        echo "[composite-record] use --skip-preflight only after the physical-side checklist is verified." >&2
        # Drop a stub capture-meta.json so unattended orchestration sees
        # a structured failure marker rather than a missing file.
        python3 - "$META_OUT" "$DEVICE" "$AUDIO_DEVICE" \
            "$INCLUDE_AUDIO" "$WIDTH" "$HEIGHT" "$FPS" "$DURATION" \
            "$PIXEL_FORMAT" "$LABEL" "$PREFLIGHT_META" "$PREFLIGHT_STATUS" \
            "$PREFLIGHT_DETECTOR" "$PREFLIGHT_RC" "$PREFLIGHT_ELAPSED_S" <<'PY'
import json, os, sys, datetime
(meta_path, device, audio_device, include_audio,
 width, height, fps, duration, pixel_format, label,
 preflight_meta, preflight_status, preflight_detector,
 preflight_rc, preflight_elapsed) = sys.argv[1:16]
obj = {
    "schema": "composite-record/v1",
    "started_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "status": "preflight-failed",
    "preflight": {
        "status": preflight_status,
        "exit_code": int(preflight_rc),
        "detector": preflight_detector,
        "elapsed_s": float(preflight_elapsed) if preflight_elapsed else None,
        "meta_path": preflight_meta if os.path.exists(preflight_meta) else None,
    },
    "device": device,
    "audio_device": audio_device,
    "include_audio": include_audio == "1",
    "width": int(width),
    "height": int(height),
    "fps": int(fps),
    "duration_target_s": int(duration),
    "pixel_format": pixel_format,
    "label": label,
    "ffmpeg_invoked": False,
}
with open(meta_path, "w") as f:
    json.dump(obj, f, indent=2)
    f.write("\n")
PY
        # Pass the preflight's exit code through unchanged so callers can
        # disambiguate without re-parsing preflight-meta.json. Codex
        # round-2 out-of-scope hygiene: prior comment text incorrectly
        # implied a remapping; reality is identity passthrough.
        # 2 = no-signal (cycle-32 F8 shape — at least one detector
        #     ran to full timeout without a frame).
        # 3 = device not enumerated.
        # 4 = no detector backend available.
        # 1 = host-side backend error (TCC denial / ffmpeg quick-fail /
        #     xemu-capture broken — every detector failed BEFORE reaching
        #     its timeout). Cycle-33 closeout: distinct from rc=2 so
        #     unattended orchestration can branch host-fix vs cable-fix.
        # 5 = invalid CLI from the preflight (shouldn't happen here
        #     because composite-record.sh constructs the flags itself).
        exit "$PREFLIGHT_RC"
    fi
    echo "[composite-record] preflight OK (detector=$PREFLIGHT_DETECTOR, elapsed=${PREFLIGHT_ELAPSED_S:-?}s)"
else
    echo "[composite-record] preflight SKIPPED (--skip-preflight)"
fi

# AVFoundation device-name resolution. ffmpeg only accepts EXACT
# device names or numeric indices, never substrings — even though
# the macOS SDK matches case-insensitive substrings via xemu-capture.
# Resolve substring → numeric index by parsing `-list_devices true`.
# Indices are unstable across plug/unplug, but they are stable for
# the duration of one record session, which is what we need.
DEVICE_LIST="$("$FFMPEG" -hide_banner -f avfoundation \
    -list_devices true -i "" 2>&1 || true)"

# Cycle-33 closeout (Codex round-4 closeout minor adopted): a
# broken-but-executable FFMPEG override (e.g. /usr/bin/true, or an
# ffmpeg build without AVFoundation support) silently swallows the
# `-list_devices` call above via `|| true` and leaves DEVICE_LIST
# missing the canonical "AVFoundation video devices:" header. The
# downstream resolve_device path would then treat the empty listing
# as "no video device matched" and emit a misleading rc=3
# `status="device-not-found"` shape, masking a host-backend error
# as a hardware-error. Detect the missing header here and route to
# the same structured `no_backend` rc=4 shape the early-FFMPEG
# missing branch uses, so unattended callers continue to get the
# host-error vs hardware-error split the preflight contract
# preserves.
if ! printf '%s\n' "$DEVICE_LIST" | grep -q "AVFoundation .* devices"; then
    echo "[composite-record] ffmpeg '$FFMPEG' produced no AVFoundation device listing — broken backend or missing AVFoundation support?" >&2
    python3 - "$META_OUT" "$DEVICE" "$AUDIO_DEVICE" \
        "$INCLUDE_AUDIO" "$WIDTH" "$HEIGHT" "$FPS" "$DURATION" \
        "$PIXEL_FORMAT" "$LABEL" "$FFMPEG" \
        "$PREFLIGHT_STATUS" "$PREFLIGHT_DETECTOR" "$PREFLIGHT_RC" \
        "$PREFLIGHT_ELAPSED_S" "$PREFLIGHT_META" <<'PY'
import json, sys, datetime
(meta_path, device, audio_device, include_audio,
 width, height, fps, duration, pixel_format, label,
 ffmpeg_path,
 pf_status, pf_detector, pf_rc, pf_elapsed, pf_meta) = sys.argv[1:17]
obj = {
    "schema": "composite-record/v1",
    "started_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "status": "preflight-failed",
    "preflight": {
        "status": "no_backend",
        "exit_code": 4,
        "detector": "none",
        "elapsed_s": float(pf_elapsed) if pf_elapsed else None,
        "meta_path": pf_meta or None,
        "reason": (
            "ffmpeg '" + ffmpeg_path + "' returned no AVFoundation device listing "
            "(missing 'AVFoundation ... devices:' header). Likely a broken FFMPEG "
            "override (e.g. /usr/bin/true) or an ffmpeg build compiled without "
            "AVFoundation support; either way the recorder cannot run."
        ),
        "earlier": {
            "status": pf_status,
            "exit_code": int(pf_rc),
            "detector": pf_detector,
        },
    },
    "device": device,
    "audio_device": audio_device,
    "include_audio": include_audio == "1",
    "width": int(width),
    "height": int(height),
    "fps": int(fps),
    "duration_target_s": int(duration),
    "pixel_format": pixel_format,
    "label": label,
    "ffmpeg_invoked": False,
    "ffmpeg_path": ffmpeg_path,
}
with open(meta_path, "w") as f:
    json.dump(obj, f, indent=2)
    f.write("\n")
PY
    exit 4
fi

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

# Cycle-33 closeout (Codex round-2 closeout minor #1 adopted): post-
# preflight device-resolution failures (video substring didn't match an
# AVFoundation video device; audio substring didn't match an AVFoundation
# audio device) previously did a bare `exit 1` with no top-level
# capture-meta.json — unattended callers saw a missing artifact instead
# of the documented "preflight-failed-shape" failure marker. The audio
# branch is reachable BY DESIGN even after a green preflight because
# `composite-preflight.sh` intentionally does not open audio (the
# cycle-32 F8 failure mode is video-side no-signal). Both branches now
# emit a structured capture-meta.json (schema composite-record/v1,
# status="device-not-found", ffmpeg_invoked=false, embedded
# preflight summary if available) before exiting with the documented
# preflight rc=3 (device_not_found) so the shape matches what the
# composite-preflight.sh device_not_found path already emits.
emit_device_not_found_meta() {
    # $1 = which device (video|audio), $2 = substring pattern that didn't match
    local kind="$1" pat="$2"
    python3 - "$META_OUT" "$DEVICE" "$AUDIO_DEVICE" \
        "$INCLUDE_AUDIO" "$WIDTH" "$HEIGHT" "$FPS" "$DURATION" \
        "$PIXEL_FORMAT" "$LABEL" "$kind" "$pat" \
        "$PREFLIGHT_STATUS" "$PREFLIGHT_DETECTOR" "$PREFLIGHT_RC" \
        "$PREFLIGHT_ELAPSED_S" "$PREFLIGHT_META" <<'PY'
import json, sys, datetime
(meta_path, device, audio_device, include_audio,
 width, height, fps, duration, pixel_format, label,
 kind, pattern,
 pf_status, pf_detector, pf_rc, pf_elapsed, pf_meta) = sys.argv[1:18]
obj = {
    "schema": "composite-record/v1",
    "started_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "status": "device-not-found",
    "preflight": {
        "status": pf_status,
        "exit_code": int(pf_rc),
        "detector": pf_detector,
        "elapsed_s": float(pf_elapsed) if pf_elapsed else None,
        "meta_path": pf_meta or None,
    },
    "device_not_found": {
        "kind": kind,
        "pattern": pattern,
    },
    "device": device,
    "audio_device": audio_device,
    "include_audio": include_audio == "1",
    "width": int(width),
    "height": int(height),
    "fps": int(fps),
    "duration_target_s": int(duration),
    "pixel_format": pixel_format,
    "label": label,
    "ffmpeg_invoked": False,
}
with open(meta_path, "w") as f:
    json.dump(obj, f, indent=2)
    f.write("\n")
PY
}

VIDEO_IDX="$(resolve_device "$DEVICE" video)"
if [ -z "$VIDEO_IDX" ]; then
    echo "[composite-record] no video device matched '$DEVICE'" >&2
    echo "[composite-record] available devices:" >&2
    printf '%s\n' "$DEVICE_LIST" | grep -E "AVFoundation (video|audio) devices|\] \[[0-9]+\]" >&2 || true
    emit_device_not_found_meta video "$DEVICE"
    exit 3
fi

if [ "$INCLUDE_AUDIO" -eq 1 ]; then
    AUDIO_IDX="$(resolve_device "$AUDIO_DEVICE" audio)"
    if [ -z "$AUDIO_IDX" ]; then
        echo "[composite-record] no audio device matched '$AUDIO_DEVICE'" >&2
        echo "[composite-record] (use --no-audio to skip audio capture)" >&2
        emit_device_not_found_meta audio "$AUDIO_DEVICE"
        exit 3
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

# Patch the metadata file with status + observed duration + preflight summary.
python3 - "$META_OUT" "$RC" "$ELAPSED" "$ACTUAL_DURATION" "$CAPTURE_TIMED_OUT" \
    "$PREFLIGHT_STATUS" "$PREFLIGHT_DETECTOR" "$PREFLIGHT_RC" \
    "$PREFLIGHT_ELAPSED_S" "$PREFLIGHT_META" <<'PY'
import json, os, sys
(meta_path, rc, elapsed, actual, timed_out,
 preflight_status, preflight_detector, preflight_rc,
 preflight_elapsed, preflight_meta) = sys.argv[1:11]
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
meta["preflight"] = {
    "status": preflight_status,
    "exit_code": int(preflight_rc) if preflight_rc else 0,
    "detector": preflight_detector,
    "elapsed_s": float(preflight_elapsed) if preflight_elapsed else None,
    "meta_path": preflight_meta if (preflight_meta and os.path.exists(preflight_meta)) else None,
}
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
