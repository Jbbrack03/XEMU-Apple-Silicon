#!/bin/bash
#
# composite-preflight.sh — verify the MS2109 composite-capture stick
# is actually producing frames BEFORE arming a long composite-record.sh
# capture. Designed to fail fast (default ~8 s) on a no-signal condition
# so unattended cycles don't burn ~93 s waiting for ffmpeg's silent
# stall (cycle-32 / OUTCOME F8 failure mode).
#
# Primary detector  : xemu-capture snapshot via the TCC-approved app
#                     bundle wrapper (scripts/apple-silicon/bin/xemu-capture).
#                     Single warmup-padded frame written to a temp PNG;
#                     non-empty PNG + status=ok ⇒ live signal.
# Fallback detector : ffmpeg -f avfoundation -frames:v 1 against the
#                     resolved AVFoundation device index, killed via
#                     a wall-clock deadline if no frame arrives.
#
# USAGE
#   composite-preflight.sh [--device NAME] [--audio-device NAME]
#                          [--width N] [--height N] [--fps N]
#                          [--pixel-format FMT]
#                          [--timeout SECONDS] [--warmup-frames N]
#                          [--out-dir DIR]
#                          [--mode auto|xemu-capture|ffmpeg]
#                          [--no-audio] [--json] [--quiet]
#
# DEFAULTS
#   --device           USB2          (substring match, MS2109 stick)
#   --audio-device     USB2          (documentation-only here; preflight
#                                     never opens the audio interface)
#   --width / height   720 / 480     (NTSC; matches composite-record.sh)
#   --fps              30
#   --pixel-format     uyvy422       (MS2109 native; matches composite-record.sh)
#   --timeout          8             (per-detector wall-clock seconds for
#                                     --mode {xemu-capture,ffmpeg}; OVERALL
#                                     wall-clock budget for --mode auto — when
#                                     the auto-mode xemu-capture attempt
#                                     consumes the budget, the ffmpeg fallback
#                                     is given the remaining time only, and is
#                                     skipped entirely if < 0.5 s remains)
#   --warmup-frames    5             (xemu-capture path only)
#   --mode             auto          (prefer xemu-capture, fall back to ffmpeg)
#   --out-dir          (TMPDIR)      (writes preflight-meta.json + preflight.png)
#
# EXIT CODES
#   0  signal detected (frame received within timeout)
#   2  no-signal (at least one detector ran to its full timeout without
#      seeing a frame — cycle-32 OUTCOME F8 shape)
#   3  device not enumerated (substring did not match an AVFoundation video device)
#   4  no detector backend available (no xemu-capture wrapper AND no ffmpeg)
#   5  invalid CLI usage
#   1  host-side backend error / unexpected (e.g. TCC permission
#      denial, ffmpeg quick-fail, xemu-capture missing/broken — every
#      detector failed quickly without reaching its timeout). Distinct
#      from rc=2 so the operator sees host-side fault feedback instead
#      of being told to reseat composite cables (cycle-33 closeout, Codex
#      high #1 adopted).
#
# OUTPUT
#   preflight-meta.json   machine-readable result (always written into --out-dir
#                         when --out-dir is supplied; otherwise to a temp dir
#                         and the path is echoed on stdout in --json mode).
#   preflight.png         the captured probe frame on success (optional;
#                         deleted on no-signal so callers never see a stale frame).
#   stdout (--json)       prints the final JSON object to stdout for piping.
#
# ENVIRONMENT
#   XEMU_CAPTURE        override path to the xemu-capture wrapper (default:
#                       scripts/apple-silicon/bin/xemu-capture relative to
#                       this script).
#   FFMPEG              override ffmpeg path.
#
# SAFETY
#   This script DOES NOT touch the Xbox. It only probes the Mac-side
#   capture device. Safe to invoke at any time; runs entirely on the host.
#
# RATIONALE (cycle 33, 2026-05-23)
#   Cycle 32 produced OUTCOME F8: ffmpeg was launched against the MS2109
#   but never received a frame across 70 s of `-t` + 20 s watchdog grace,
#   wall-elapsed 93 s before SIGKILL. A follow-up 4 s standalone ffmpeg
#   probe reproduced the same silent-no-frames behavior. The capture-side
#   procedural code in composite-record.sh was sound; the failure was
#   hardware-side (no live composite signal at the MS2109 input).
#   composite-preflight.sh detects the same condition in seconds so that
#   the cycle-32 redo aborts the run BEFORE issuing `runxbe`, instead of
#   silently recording zero frames during the chainload window.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DEVICE="USB2"
AUDIO_DEVICE="USB2"
WIDTH=720
HEIGHT=480
FPS=30
PIXEL_FORMAT="uyvy422"
TIMEOUT_S=8
WARMUP_FRAMES=5
OUT_DIR=""
MODE="auto"
INCLUDE_AUDIO=1
JSON_OUTPUT=0
QUIET=0

print_help() {
    sed -n '2,70p' "${BASH_SOURCE[0]}" | sed -e 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --device)         DEVICE="$2"; shift 2 ;;
        --audio-device)   AUDIO_DEVICE="$2"; shift 2 ;;
        --width)          WIDTH="$2"; shift 2 ;;
        --height)         HEIGHT="$2"; shift 2 ;;
        --fps)            FPS="$2"; shift 2 ;;
        --pixel-format)   PIXEL_FORMAT="$2"; shift 2 ;;
        --timeout)        TIMEOUT_S="$2"; shift 2 ;;
        --warmup-frames)  WARMUP_FRAMES="$2"; shift 2 ;;
        --out-dir)        OUT_DIR="$2"; shift 2 ;;
        --mode)           MODE="$2"; shift 2 ;;
        --no-audio)       INCLUDE_AUDIO=0; shift ;;
        --json)           JSON_OUTPUT=1; shift ;;
        --quiet)          QUIET=1; shift ;;
        -h|--help)        print_help; exit 0 ;;
        *)
            echo "[composite-preflight] unknown arg: $1" >&2
            exit 5 ;;
    esac
done

case "$MODE" in
    auto|xemu-capture|ffmpeg) ;;
    *)
        echo "[composite-preflight] --mode must be auto|xemu-capture|ffmpeg (got '$MODE')" >&2
        exit 5 ;;
esac

if ! [[ "$TIMEOUT_S" =~ ^[0-9]+$ ]] || [ "$TIMEOUT_S" -lt 1 ]; then
    echo "[composite-preflight] --timeout must be a positive integer (got '$TIMEOUT_S')" >&2
    exit 5
fi

log() {
    if [ "$QUIET" -eq 0 ]; then
        echo "[composite-preflight] $*" >&2
    fi
}

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="$(mktemp -d -t composite-preflight)"
else
    mkdir -p "$OUT_DIR"
fi

PROBE_PNG="$OUT_DIR/preflight.png"
META_PATH="$OUT_DIR/preflight-meta.json"
FALLBACK_LOG="$OUT_DIR/preflight-ffmpeg.log"

XEMU_CAPTURE="${XEMU_CAPTURE:-$SCRIPT_DIR/bin/xemu-capture}"
FFMPEG="${FFMPEG:-$(command -v ffmpeg || true)}"

# Cycle-33 closeout (Codex round-4 medium #2 adopted): a bad $FFMPEG
# override (non-empty path that isn't executable) used to skip the rc=4
# no_backend gate because the availability check used `[ -n "$FFMPEG" ]`.
# Normalize FFMPEG to empty when the path isn't executable so every
# downstream "ffmpeg available?" check answers correctly and the
# no_backend gate is reached when appropriate.
if [ -n "$FFMPEG" ] && [ ! -x "$FFMPEG" ]; then
    log "FFMPEG='$FFMPEG' is not executable; treating ffmpeg backend as unavailable."
    FFMPEG=""
fi

PHYSICAL_CHECKLIST=$(cat <<'EOF'
Physical-side checklist (cycle-32 F8 mitigation; from canonical cycle-32 evidence):
  1. Confirm the composite cable is seated firmly at the Xbox AV pack and at the MS2109 RCA-yellow input.
  2. Confirm the MS2109 selector is set to COMPOSITE (not S-Video).
  3. Confirm the Xbox AV output mode is set to COMPOSITE in the dashboard / EEPROM (S-Video / HDTV packs route differently).
  4. Confirm the MS2109 is plugged into a stable USB port — Mac Studio front USB-C ASMedia 3142 brown-outs the device; prefer back USB-A or rear USB-C (DRD).
  5. Optional: `scripts/apple-silicon/bin/xemu-capture snapshot USB2 --out /tmp/probe.png` and visually inspect the resulting PNG (the only xemu-capture verb that actually proves live frames are arriving — `probe` only lists supported formats and `inputs` only enumerates the source selector positions). Use `inputs DEVICE` + `set-input DEVICE composite` only AFTER snapshot succeeds, to confirm the active input is composite vs S-Video.
EOF
)

emit_json() {
    # $1 = status string ("ok" / "no_signal" / "device_not_found" / "no_backend" / "error")
    # $2 = exit code (integer)
    # $3 = detector used (PRIMARY — "xemu-capture" / "ffmpeg" / "none")
    # $4 = elapsed seconds (float as string)
    # $5 = detail message
    # Machine-readable per-detector breakdown is taken from globals
    # DETECTORS_ATTEMPTED / DETECTOR_REASONS / DETECTOR_DETAILS so
    # downstream consumers don't have to parse the freeform detail
    # text (Codex round-7 medium #1 adopted).
    local status="$1" rc="$2" detector="$3" elapsed="$4" detail="$5"
    local probe_present="false"
    if [ -f "$PROBE_PNG" ] && [ -s "$PROBE_PNG" ]; then
        probe_present="true"
    fi
    local ffmpeg_present_str="false"
    [ -n "$FFMPEG" ] && ffmpeg_present_str="true"
    local xemu_capture_present_str="false"
    [ -x "$XEMU_CAPTURE" ] && xemu_capture_present_str="true"

    python3 - "$META_PATH" \
        "$status" "$rc" "$detector" "$elapsed" "$detail" \
        "$DEVICE" "$AUDIO_DEVICE" "$WIDTH" "$HEIGHT" "$FPS" "$PIXEL_FORMAT" \
        "$TIMEOUT_S" "$WARMUP_FRAMES" "$MODE" "$INCLUDE_AUDIO" \
        "$probe_present" "$PROBE_PNG" "$ffmpeg_present_str" "$xemu_capture_present_str" \
        "$FFMPEG" "$XEMU_CAPTURE" "$OUT_DIR" \
        "${DETECTORS_ATTEMPTED:-}" "${DETECTOR_REASONS:-}" "${DETECTOR_DETAILS:-}" <<'PY'
import json, sys, datetime
(meta_path, status, rc, detector, elapsed, detail,
 device, audio_device, width, height, fps, pixel_format,
 timeout_s, warmup_frames, mode, include_audio,
 probe_present, probe_png, ffmpeg_present, xemu_capture_present,
 ffmpeg_path, xemu_capture_path, out_dir,
 detectors_attempted, detector_reasons, detector_details) = sys.argv[1:27]

# Parse the per-detector breakdown out of the globals' comma/semicolon
# shapes into proper JSON. detectors_attempted is "a,b"; reasons is
# "a=ok;b=timeout"; details is "[a] msg | [b] msg".
def split_csv(s):
    return [x for x in (s or "").split(",") if x]

def parse_reasons(s):
    out = {}
    for chunk in (s or "").split(";"):
        if "=" in chunk:
            k, _, v = chunk.partition("=")
            out[k] = v
    return out

def split_details(s):
    return [x.strip() for x in (s or "").split(" | ") if x.strip()]

obj = {
    "schema": "composite-preflight/v1",
    "status": status,
    "exit_code": int(rc),
    "detector": detector,
    "detectors_attempted": split_csv(detectors_attempted),
    "detector_reasons": parse_reasons(detector_reasons),
    "detector_details": split_details(detector_details),
    "elapsed_s": float(elapsed) if elapsed else None,
    "detail": detail,
    "checked_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "device": device,
    "audio_device": audio_device,
    "width": int(width),
    "height": int(height),
    "fps": int(fps),
    "pixel_format": pixel_format,
    "timeout_s": int(timeout_s),
    "warmup_frames": int(warmup_frames),
    "mode": mode,
    "include_audio": include_audio == "1",
    "probe_image": probe_png if probe_present == "true" else None,
    "probe_image_present": probe_present == "true",
    "ffmpeg_available": ffmpeg_present == "true",
    "xemu_capture_available": xemu_capture_present == "true",
    "ffmpeg_path": ffmpeg_path or None,
    "xemu_capture_path": xemu_capture_path or None,
    "out_dir": out_dir,
}
with open(meta_path, "w") as f:
    json.dump(obj, f, indent=2)
    f.write("\n")
print(json.dumps(obj))
PY
}

emit_result_and_exit() {
    local status="$1" rc="$2" detector="$3" elapsed="$4" detail="$5"
    # Codex round-7 medium #2 adopted: delete any stale probe PNG
    # BEFORE serializing the JSON so the "probe_image" / "probe_image_present"
    # fields can't lie about a file that emit_json sees but
    # emit_result_and_exit then removes.
    if [ "$rc" -ne 0 ]; then
        rm -f "$PROBE_PNG"
    fi

    local json_str
    json_str="$(emit_json "$status" "$rc" "$detector" "$elapsed" "$detail")"

    if [ "$rc" -eq 0 ]; then
        log "OK ($detector, ${elapsed}s) — frame received from '$DEVICE'."
    else
        log "FAIL ($status, ${detector}, ${elapsed}s) — $detail"
        if [ "$QUIET" -eq 0 ]; then
            printf '%s\n' "$PHYSICAL_CHECKLIST" >&2
            log "meta: $META_PATH"
        fi
    fi

    if [ "$JSON_OUTPUT" -eq 1 ]; then
        printf '%s\n' "$json_str"
    fi

    exit "$rc"
}

# Cycle-33 closeout (Codex round-4 medium #1 adopted): start the
# wall-clock budget BEFORE enumeration so the documented "overall
# wall-clock budget" semantics for --mode auto actually hold. Slow
# `ffmpeg -list_devices` or `xemu-capture list` calls now count against
# the budget instead of being silently free. elapsed_since_start /
# ffmpeg_budget_remaining helpers (defined further below) read this
# variable directly.
START_EPOCH_NS="$(python3 -c 'import time; print(int(time.time()*1e9))')"

# --- Step 1: enumerate AVFoundation devices once so we can detect a
# missing MS2109 quickly and use the same numeric-index resolution
# composite-record.sh does for the ffmpeg fallback. -------------------
#
# Cycle-33 closeout (Codex medium #2 adopted): gate the enumeration
# choice by --mode so backend-specific prechecks never block a
# DIFFERENT requested backend. --mode ffmpeg uses ffmpeg's -list_devices
# only; --mode xemu-capture uses `xemu-capture list` only; --mode auto
# prefers ffmpeg's enumeration when available (cheap and well-known)
# and falls back to xemu-capture list otherwise. The detector chosen
# in Step 2 is the same path whose enumeration ran here.

USE_FFMPEG_ENUM=0
USE_XEMU_LIST=0
case "$MODE" in
    ffmpeg)
        [ -n "$FFMPEG" ] && USE_FFMPEG_ENUM=1
        ;;
    xemu-capture)
        [ -x "$XEMU_CAPTURE" ] && USE_XEMU_LIST=1
        ;;
    auto)
        if [ -n "$FFMPEG" ]; then
            USE_FFMPEG_ENUM=1
        elif [ -x "$XEMU_CAPTURE" ]; then
            USE_XEMU_LIST=1
        fi
        ;;
esac

DEVICE_LIST=""
FFMPEG_ENUM_RC=0
FFMPEG_ENUM_OK=0
if [ "$USE_FFMPEG_ENUM" -eq 1 ]; then
    set +e
    DEVICE_LIST="$("$FFMPEG" -hide_banner -f avfoundation \
        -list_devices true -i "" 2>&1)"
    FFMPEG_ENUM_RC=$?
    set -e
    # ffmpeg -list_devices exits non-zero on this version because the
    # empty `-i ""` is its sentinel for the listing mode — the rc by
    # itself does NOT distinguish "enumeration succeeded" from
    # "enumeration framework broken." Use a header heuristic instead:
    # AVFoundation's listing always prints the "AVFoundation video
    # devices:" banner before any device rows when the framework loaded
    # correctly. If the banner is missing AND ffmpeg printed something,
    # treat that as a host-side backend error rather than as
    # device_not_found (Codex round-3 medium #2 adopted).
    if printf '%s' "$DEVICE_LIST" | grep -q 'AVFoundation video devices:'; then
        FFMPEG_ENUM_OK=1
    fi
fi

resolve_device() {
    local pat lower kind line
    pat="$1"; kind="$2"
    lower="$(printf '%s' "$pat" | tr '[:upper:]' '[:lower:]')"
    awk -v pat="$lower" -v kind="$kind" '
        /AVFoundation video devices:/ { section="video"; next }
        /AVFoundation audio devices:/ { section="audio"; next }
        section == kind {
            line = $0
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

VIDEO_IDX=""
if [ -n "$DEVICE_LIST" ]; then
    VIDEO_IDX="$(resolve_device "$DEVICE" video)"
fi

if [ "$USE_FFMPEG_ENUM" -eq 1 ] && [ "$FFMPEG_ENUM_OK" -ne 1 ]; then
    # Codex round-3 medium #2 adopted: ffmpeg ran but did not produce
    # a recognizable AVFoundation device listing — that is a host-side
    # backend fault (broken ffmpeg build, missing AVFoundation
    # framework, etc.), NOT a device_not_found. Fall back to
    # xemu-capture list in auto mode so a working backend can still
    # answer; otherwise emit rc=1 with the captured ffmpeg output so
    # the operator sees the actual error.
    if [ "$MODE" = "auto" ] && [ -x "$XEMU_CAPTURE" ]; then
        log "ffmpeg -list_devices produced no AVFoundation device banner (rc=$FFMPEG_ENUM_RC); falling back to xemu-capture list."
        USE_XEMU_LIST=1
    else
        local_tail="$(printf '%s' "$DEVICE_LIST" | tail -n 1 | tr -d '\n' | cut -c1-200)"
        emit_result_and_exit "error" 1 "none" "0" \
            "ffmpeg -list_devices did not produce an AVFoundation device banner (rc=$FFMPEG_ENUM_RC). Likely host-side ffmpeg/AVFoundation problem, not a missing device. Last log line: ${local_tail:-<empty>}"
    fi
elif [ "$USE_FFMPEG_ENUM" -eq 1 ] && [ -z "$VIDEO_IDX" ]; then
    if [ "$MODE" = "auto" ] && [ -x "$XEMU_CAPTURE" ]; then
        # Cycle-33 closeout (Codex round-2 medium #1 adopted): in auto
        # mode, do not declare device_not_found purely from ffmpeg's
        # enumeration miss — also consult `xemu-capture list` before
        # giving up, so a substring matched by xemu-capture's
        # device-name table but not by ffmpeg's is not falsely
        # rejected. If xemu-capture list also misses → confirmed
        # device_not_found. If it matches → fall through to detector
        # selection and let xemu-capture handle the actual probe.
        log "ffmpeg enumeration missed '$DEVICE'; consulting xemu-capture list as a second opinion (auto mode)."
        USE_XEMU_LIST=1
    else
        log "MS2109 not enumerated by ffmpeg (-list_devices). Substring '$DEVICE' did not match any AVFoundation video device."
        emit_result_and_exit "device_not_found" 3 "none" "0" \
            "AVFoundation enumeration found no video device matching --device '$DEVICE'."
    fi
fi

# `xemu-capture list` pre-check for device_not_found when the active
# backend is xemu-capture (Codex round-1 medium #1, round-2 partial
# follow-up, plus cycle-33 closeout medium #2: now gated by MODE rather
# than by absence of ffmpeg so --mode xemu-capture is independent of
# ffmpeg's enumeration result). Also engaged in --mode auto as the
# second-opinion fallback when ffmpeg's enumeration missed the device
# (cycle-33 closeout, Codex round-2 medium #1). Empty / malformed list
# output is surfaced as rc=1 unexpected error rather than silently
# allowed to fall through — the operator gets actionable feedback
# instead of a misleading "no_signal" classification (Codex round-2
# medium #1 follow-up).
if [ "$USE_XEMU_LIST" -eq 1 ]; then
    set +e
    XEMU_LIST_JSON="$("$XEMU_CAPTURE" list 2>/dev/null)"
    XEMU_LIST_RC=$?
    set -e
    if [ "$XEMU_LIST_RC" -ne 0 ] || [ -z "$XEMU_LIST_JSON" ]; then
        emit_result_and_exit "error" 1 "none" "0" \
            "xemu-capture list returned no output (rc=$XEMU_LIST_RC); cannot pre-check device enumeration and ffmpeg is unavailable. Build tools/xemu-capture/ or install ffmpeg."
    fi
    # Classify list output as one of: matched / no_match / parse_error.
    # Cycle-33 closeout: pass the JSON via argv (not stdin) — the
    # original `printf | python3 - "$DEVICE" <<'PY'` invocation had
    # bash redirect stdin to the heredoc (last redirection wins),
    # making sys.stdin.read() return the python source instead of the
    # JSON and breaking device-name matching entirely. argv passthrough
    # is the simplest fix that keeps the python self-contained.
    set +e
    XEMU_MATCH_RESULT="$(python3 - "$DEVICE" "$XEMU_LIST_JSON" <<'PY'
import json, sys
pat = sys.argv[1].lower()
data = sys.argv[2]
try:
    obj = json.loads(data)
except Exception:
    print("parse_error")
    sys.exit(0)
for d in obj.get("devices", []) or []:
    if pat in (d.get("name") or "").lower():
        print("matched")
        sys.exit(0)
print("no_match")
PY
)"
    set -e
    case "$XEMU_MATCH_RESULT" in
        matched)
            : # pre-check passed; fall through to detector selection
            ;;
        no_match)
            emit_result_and_exit "device_not_found" 3 "none" "0" \
                "xemu-capture list found no video device matching --device '$DEVICE'."
            ;;
        parse_error|*)
            emit_result_and_exit "error" 1 "none" "0" \
                "xemu-capture list output was unparseable JSON; cannot pre-check device enumeration and ffmpeg is unavailable."
            ;;
    esac
fi

# --- Step 2: pick a backend per --mode ---------------------------------

XEMU_AVAILABLE=0
if [ -x "$XEMU_CAPTURE" ]; then
    XEMU_AVAILABLE=1
fi
FFMPEG_AVAILABLE=0
if [ -n "$FFMPEG" ]; then
    FFMPEG_AVAILABLE=1
fi

case "$MODE" in
    xemu-capture)
        if [ "$XEMU_AVAILABLE" -ne 1 ]; then
            emit_result_and_exit "no_backend" 4 "none" "0" \
                "--mode xemu-capture requested but $XEMU_CAPTURE is missing or not executable."
        fi
        ;;
    ffmpeg)
        if [ "$FFMPEG_AVAILABLE" -ne 1 ]; then
            emit_result_and_exit "no_backend" 4 "none" "0" \
                "--mode ffmpeg requested but ffmpeg was not found on \$PATH (\$FFMPEG override empty)."
        fi
        ;;
    auto)
        if [ "$XEMU_AVAILABLE" -ne 1 ] && [ "$FFMPEG_AVAILABLE" -ne 1 ]; then
            emit_result_and_exit "no_backend" 4 "none" "0" \
                "Neither $XEMU_CAPTURE nor ffmpeg is available; cannot probe."
        fi
        ;;
esac

# --- Step 3: xemu-capture snapshot (primary detector) ------------------
# START_EPOCH_NS was set above Step 1 so enumeration is inside the
# documented --timeout budget (Codex round-4 medium #1 adopted).

elapsed_since_start() {
    python3 - "$START_EPOCH_NS" <<'PY'
import sys, time
start_ns = int(sys.argv[1])
print(f"{(time.time()*1e9 - start_ns)/1e9:.3f}")
PY
}

xemu_capture_budget_remaining() {
    # In forced --mode xemu-capture / --mode ffmpeg, the documented
    # contract is "per-detector --timeout SECONDS", so the per-detector
    # budget is the full TIMEOUT_S regardless of enumeration time.
    # In --mode auto the contract is "overall wall-clock budget", so
    # the xemu-capture detector receives only the remaining time after
    # enumeration (Codex round-5 medium #1 adopted, round-6 follow-up:
    # pass FLOAT seconds because xemu-capture's CLI parses --timeout as
    # Double — flooring to integer plus the prior >=1s clamp inflated
    # the budget by up to ~1s on near-exhausted auto runs).
    if [ "$MODE" != "auto" ]; then
        printf '%s' "$TIMEOUT_S"
        return 0
    fi
    python3 - "$START_EPOCH_NS" "$TIMEOUT_S" <<'PY'
import sys, time
start_ns = int(sys.argv[1])
budget = float(sys.argv[2])
elapsed = (time.time()*1e9 - start_ns)/1e9
remaining = max(0.0, budget - elapsed)
# Float seconds with millisecond precision; caller is responsible for
# treating remaining < 0.5 as "skip the detector entirely" (same
# threshold the ffmpeg fallback uses). xemu-capture / xemu-capture-app
# both accept float --timeout / --wait-s.
print(f"{remaining:.3f}")
PY
}

try_xemu_capture() {
    local x_budget
    x_budget="$(xemu_capture_budget_remaining)"
    # Skip the detector entirely when the remaining auto-mode budget
    # is too small to be meaningful, matching the ffmpeg-fallback
    # <0.5s skip threshold (Codex round-6 follow-up to round-5 medium
    # #1). For non-auto modes x_budget is always TIMEOUT_S (>=1s by
    # CLI validation) so this branch never fires there.
    if python3 - "$x_budget" <<'PY' 2>/dev/null
import sys
raise SystemExit(0 if float(sys.argv[1]) < 0.5 else 1)
PY
    then
        log "xemu-capture probe skipped: remaining budget ${x_budget}s < 0.5s after enumeration; honoring overall --timeout=${TIMEOUT_S}s contract."
        LAST_DETECTOR_REASON="skipped_budget_exhausted"
        LAST_DETECTOR_DETAIL="xemu-capture skipped: remaining auto-mode budget ${x_budget}s < 0.5s after enumeration."
        return 1
    fi
    log "trying xemu-capture snapshot (device='$DEVICE', budget=${x_budget}s, warmup=${WARMUP_FRAMES})..."
    local launch_start_ns launch_end_ns elapsed_ms
    launch_start_ns="$(python3 -c 'import time; print(int(time.time()*1e9))')"
    # Cycle-33 closeout (Codex round-2 medium #2 adopted): the
    # xemu-capture-app.py wrapper defaults its JSON-wait to
    # max(15, --timeout+12) seconds, which inflates the actual budget
    # well past the documented per-detector --timeout. Pass --wait-s
    # explicitly so the wrapper's deadline matches our budget contract.
    # In --mode auto, $x_budget is the REMAINING overall budget after
    # enumeration so the documented "overall wall-clock budget"
    # actually holds across enumeration + this detector (Codex round-5
    # medium #1).
    set +e
    "$XEMU_CAPTURE" --wait-s "$x_budget" snapshot "$DEVICE" \
        --out "$PROBE_PNG" \
        --warmup-frames "$WARMUP_FRAMES" \
        --timeout "$x_budget" \
        --width "$WIDTH" --height "$HEIGHT" \
        > "$OUT_DIR/preflight-xemu-capture.log" 2>&1
    local rc=$?
    set -e
    launch_end_ns="$(python3 -c 'import time; print(int(time.time()*1e9))')"
    elapsed_ms=$(( (launch_end_ns - launch_start_ns) / 1000000 ))
    # The timeout floor that classifies "ran to budget" uses the
    # per-detector budget actually given, not TIMEOUT_S, so the auto-mode
    # remaining-budget shape is classified correctly. x_budget is a
    # float seconds string ("0.872" / "5.000"); convert to integer ms
    # via python so arithmetic with $elapsed_ms stays well-defined.
    local detector_budget_ms
    detector_budget_ms="$(python3 -c "import sys; print(int(round(float(sys.argv[1])*1000)))" "$x_budget")"
    # xemu-capture writes JSON to stdout on success; require status=ok.
    if [ "$rc" -eq 0 ] && [ -s "$PROBE_PNG" ] && \
       grep -q '"status"[[:space:]]*:[[:space:]]*"ok"' "$OUT_DIR/preflight-xemu-capture.log" 2>/dev/null; then
        LAST_DETECTOR_REASON="ok"
        LAST_DETECTOR_DETAIL="xemu-capture snapshot OK"
        return 0
    fi
    # Cycle-33 closeout (Codex high #1 adopted): distinguish
    # "ran the full timeout budget and got no frame" (timeout shape =
    # cycle-32 F8 no_signal) from "process failed quickly" (backend
    # error — permission/codec/device-busy/etc.). Heuristic: if
    # elapsed >= 80% of the per-detector budget AND no status=ok was
    # written, treat as timeout. Otherwise treat as backend_error and
    # surface the tail of the log so the operator sees the real cause
    # instead of being told to reseat the composite cable.
    local timeout_floor_ms
    timeout_floor_ms=$(( detector_budget_ms * 4 / 5 ))
    if [ "$elapsed_ms" -ge "$timeout_floor_ms" ]; then
        LAST_DETECTOR_REASON="timeout"
        LAST_DETECTOR_DETAIL="xemu-capture snapshot timed out after ${elapsed_ms}ms (budget=${detector_budget_ms}ms); rc=$rc."
    else
        local tail_msg
        tail_msg="$(tail -n 1 "$OUT_DIR/preflight-xemu-capture.log" 2>/dev/null | tr -d '\n' | cut -c1-200)"
        LAST_DETECTOR_REASON="backend_error"
        LAST_DETECTOR_DETAIL="xemu-capture snapshot failed in ${elapsed_ms}ms (rc=$rc); last log line: ${tail_msg:-<empty>}"
    fi
    return 1
}

# --- Step 4: ffmpeg one-frame fallback ---------------------------------

ffmpeg_budget_remaining() {
    if [ "$MODE" != "auto" ]; then
        printf '%s' "$TIMEOUT_S"
        return 0
    fi
    python3 - "$START_EPOCH_NS" "$TIMEOUT_S" <<'PY'
import sys, time
start_ns = int(sys.argv[1])
budget = float(sys.argv[2])
elapsed = (time.time()*1e9 - start_ns)/1e9
remaining = budget - elapsed
print(f"{max(0.0, remaining):.2f}")
PY
}

try_ffmpeg() {
    if [ -z "$VIDEO_IDX" ]; then
        log "ffmpeg fallback skipped: --device '$DEVICE' did not resolve to a numeric index."
        LAST_DETECTOR_REASON="backend_error"
        LAST_DETECTOR_DETAIL="ffmpeg fallback skipped: '$DEVICE' did not resolve to a numeric index."
        return 1
    fi
    local ff_timeout
    ff_timeout="$(ffmpeg_budget_remaining)"
    log "trying ffmpeg fallback (video=[$VIDEO_IDX], timeout=${ff_timeout}s)..."
    rm -f "$PROBE_PNG"
    set +e
    "$FFMPEG" -hide_banner -loglevel warning -y \
        -f avfoundation \
        -framerate "$FPS" \
        -video_size "${WIDTH}x${HEIGHT}" \
        -pixel_format "$PIXEL_FORMAT" \
        -i "$VIDEO_IDX:none" \
        -frames:v 1 \
        "$PROBE_PNG" 2> "$FALLBACK_LOG" &
    local pid=$!
    # Track whether we killed ffmpeg via the wall-clock deadline so we
    # can distinguish timeout shape (cycle-32 F8 no_signal) from a
    # quick-fail backend error (Codex high #1 adopted, cycle-33 closeout).
    local killed_by_deadline=0
    local deadline
    deadline="$(python3 - "$ff_timeout" <<'PY'
import sys, time
print(time.time() + float(sys.argv[1]))
PY
)"
    while kill -0 "$pid" 2>/dev/null; do
        if python3 - "$deadline" <<'PY'
import sys, time
raise SystemExit(0 if time.time() > float(sys.argv[1]) else 1)
PY
        then
            killed_by_deadline=1
            kill "$pid" 2>/dev/null || true
            sleep 0.3
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            set -e
            LAST_DETECTOR_REASON="timeout"
            LAST_DETECTOR_DETAIL="ffmpeg -frames:v 1 produced no frame within ${ff_timeout}s; killed by deadline."
            return 1
        fi
        sleep 0.2
    done
    wait "$pid"
    local rc=$?
    set -e
    if [ "$rc" -eq 0 ] && [ -s "$PROBE_PNG" ]; then
        LAST_DETECTOR_REASON="ok"
        LAST_DETECTOR_DETAIL="ffmpeg -frames:v 1 wrote $PROBE_PNG."
        return 0
    fi
    # Process exited on its own with rc!=0 (not killed by us) — almost
    # always a host-side / device-backend error, not the cycle-32 F8
    # no-signal hardware shape.
    local tail_msg
    tail_msg="$(tail -n 1 "$FALLBACK_LOG" 2>/dev/null | tr -d '\n' | cut -c1-200)"
    LAST_DETECTOR_REASON="backend_error"
    LAST_DETECTOR_DETAIL="ffmpeg exited rc=$rc without being killed (killed_by_deadline=$killed_by_deadline); last log line: ${tail_msg:-<empty>}"
    return 1
}

DETECTOR_USED="none"
DETECTORS_ATTEMPTED=""
DETECTOR_REASONS=""
DETECTOR_DETAILS=""
SUCCESS=0
LAST_DETECTOR_REASON=""
LAST_DETECTOR_DETAIL=""

record_attempt() {
    if [ -z "$DETECTORS_ATTEMPTED" ]; then
        DETECTORS_ATTEMPTED="$1"
    else
        DETECTORS_ATTEMPTED="$DETECTORS_ATTEMPTED,$1"
    fi
    DETECTOR_USED="$1"
}

record_outcome() {
    # $1 = detector name; uses LAST_DETECTOR_REASON / LAST_DETECTOR_DETAIL
    local reason="${LAST_DETECTOR_REASON:-unknown}"
    local detail="${LAST_DETECTOR_DETAIL:-no detail}"
    if [ -z "$DETECTOR_REASONS" ]; then
        DETECTOR_REASONS="$1=$reason"
        DETECTOR_DETAILS="[$1] $detail"
    else
        DETECTOR_REASONS="$DETECTOR_REASONS;$1=$reason"
        DETECTOR_DETAILS="$DETECTOR_DETAILS | [$1] $detail"
    fi
}

case "$MODE" in
    xemu-capture)
        record_attempt "xemu-capture"
        if try_xemu_capture; then
            SUCCESS=1
        fi
        record_outcome "xemu-capture"
        ;;
    ffmpeg)
        record_attempt "ffmpeg"
        if try_ffmpeg; then
            SUCCESS=1
        fi
        record_outcome "ffmpeg"
        ;;
    auto)
        if [ "$XEMU_AVAILABLE" -eq 1 ]; then
            # Codex round-3 low #3 (extended round-6 follow-up): only
            # record an "xemu-capture" attempt when there's actually
            # budget left to launch it. The budget check is mirrored
            # inside try_xemu_capture too, but doing it here keeps
            # DETECTORS_ATTEMPTED honest in the JSON.
            X_REMAINING="$(xemu_capture_budget_remaining)"
            if python3 - "$X_REMAINING" <<'PY' 2>/dev/null
import sys
raise SystemExit(0 if float(sys.argv[1]) < 0.5 else 1)
PY
            then
                log "xemu-capture probe skipped: remaining budget ${X_REMAINING}s < 0.5s; honoring overall --timeout=${TIMEOUT_S}s contract."
            else
                record_attempt "xemu-capture"
                if try_xemu_capture; then
                    SUCCESS=1
                else
                    log "xemu-capture probe did not return a frame; falling back to ffmpeg."
                fi
                record_outcome "xemu-capture"
            fi
        fi
        if [ "$SUCCESS" -eq 0 ] && [ "$FFMPEG_AVAILABLE" -eq 1 ]; then
            # Codex round-3 low #3: only record an "ffmpeg" attempt
            # when we actually launch it. Auto mode skips the ffmpeg
            # fallback when the overall --timeout budget is essentially
            # exhausted (< 0.5 s remaining); without this check the
            # JSON would falsely claim ffmpeg was attempted.
            FF_REMAINING="$(ffmpeg_budget_remaining)"
            if python3 - "$FF_REMAINING" <<'PY'
import sys
raise SystemExit(0 if float(sys.argv[1]) < 0.5 else 1)
PY
            then
                log "ffmpeg fallback skipped: remaining budget ${FF_REMAINING}s < 0.5s after xemu-capture attempt; honoring overall --timeout=${TIMEOUT_S}s contract."
            else
                record_attempt "ffmpeg"
                if try_ffmpeg; then
                    SUCCESS=1
                fi
                record_outcome "ffmpeg"
            fi
        fi
        ;;
esac

ELAPSED="$(elapsed_since_start)"

if [ "$SUCCESS" -eq 1 ]; then
    emit_result_and_exit "ok" 0 "$DETECTOR_USED" "$ELAPSED" \
        "Captured one probe frame from '$DEVICE'."
fi

# Cycle-33 closeout (Codex high #1 adopted): classify outcome by the
# REASONS reported by each detector, not just by "no frame produced."
# - If ANY attempted detector reported reason=timeout, the overall
#   shape matches cycle-32 OUTCOME F8 (no live signal within budget);
#   emit no_signal / rc=2 and surface the physical-side checklist.
# - If NO detector reached the timeout (i.e. all attempts ended in
#   reason=backend_error / parse_error / similar), the failure is
#   almost certainly host-side: TCC permission denial, ffmpeg
#   format/device-busy, broken xemu-capture binary, etc. Emit
#   status=error / rc=1 with the captured detector messages instead
#   of misleading the operator into reseating the composite cable.
HAS_TIMEOUT=0
case ";$DETECTOR_REASONS;" in
    *";xemu-capture=timeout;"*|*";ffmpeg=timeout;"*) HAS_TIMEOUT=1 ;;
esac
# Cycle-33 closeout (Codex round-8 medium #1 adopted): the "detector"
# field is documented (here and in automation.md / capture-meta.json) as
# the PRIMARY backend that produced the outcome — a singular enum, one
# of "xemu-capture" / "ffmpeg" / "none". Passing the comma-joined
# DETECTORS_ATTEMPTED list ("xemu-capture,ffmpeg") into that slot in
# auto-mode failure paths broke the schema for downstream tooling. The
# multi-backend detail already lives in detectors_attempted /
# detector_reasons / detector_details, so emit "none" here when no
# single detector succeeded — exactly the "no_backend" path's
# established convention.
if [ "$HAS_TIMEOUT" -eq 1 ]; then
    emit_result_and_exit "no_signal" 2 "none" "$ELAPSED" \
        "No frame received from '$DEVICE' within ${TIMEOUT_S}s on detectors: ${DETECTORS_ATTEMPTED:-none}. Likely cycle-32 OUTCOME F8 shape (hardware-side no-signal). Reasons: ${DETECTOR_REASONS:-none}. Details: ${DETECTOR_DETAILS:-none}."
fi

emit_result_and_exit "error" 1 "none" "$ELAPSED" \
    "All preflight detectors failed without reaching the per-detector timeout — likely a host-side backend error (permission/codec/device-busy/binary missing), NOT the cycle-32 hardware-side no-signal shape. Detectors attempted: ${DETECTORS_ATTEMPTED:-none}. Reasons: ${DETECTOR_REASONS:-none}. Details: ${DETECTOR_DETAILS:-none}."
