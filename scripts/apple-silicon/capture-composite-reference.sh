#!/usr/bin/env bash
#
# capture-composite-reference.sh — capture a real-Xbox reference frame
# for a diag XBE via the MS2109 composite-capture stick, bypassing
# pbkit + D:\ fopen. This is an alternative oracle leg: when you can't
# (or don't want to) get an in-XBE D:\ write to work, the capture
# stick observes whatever is on screen at the time of capture.
#
# Pre-2026-05-07 this was the workaround for the pbkit + D:\\ fopen
# blocker on Tier-1 diag XBEs (mirror / color-channel / depth-floor).
# That's now fixed by the PCRTC_START path in xbed_capture.c, so this
# script's primary use case is broader: any future scenario where the
# in-XBE capture fails or where we want a visual independent witness
# (composite output) for cross-comparison against the agent's pixel-
# exact RPC screenshot.
#
# Workflow:
#   1. Reboot Xbox via the agent so FTP is available for diag XBE
#      upload (if --upload is set).
#   2. Upload the diag XBE to E:\Apps\<id>\default.xbe (default ON).
#   3. Re-launch the agent, chainload the diag via runxbe.
#   4. Start composite-record.sh in background to capture the screen.
#   5. The diag XBE renders its pattern, holds the on-screen pattern
#      for the hardcoded `Sleep(1500)` window in
#      `xbed_capture_and_reboot()`, then reboots. The composite
#      recording window is sized to cover the entire chainload-render-
#      reboot cycle plus $RECORD_EXTRA_S of buffer; the diag's own
#      hold is NOT overridable from this script (see header note).
#   6. After capture, the diag rebootss back to the dashboard.
#   7. Extract scene-change keyframes from the capture and pick the
#      one that matches the diag's expected pattern best. Save as
#      docs/apple-silicon/xbox-real-references/<id>/composite.png.
#
# This produces a reference PNG that is INDEPENDENT of:
#   - The agent's TCP/9001 screenshot RPC (different code path).
#   - The diag XBE's xbed_capture/D:\ fopen path (different code path).
#   - The pbkit + PCRTC_START path (the composite stick reads what
#     the TV/encoder sees, after CRTC scan-out).
# Three independent witnesses: math-derived oracle, agent screenshot
# RPC, composite capture. If all three agree, we have very high
# confidence the diag is correct.
#
# USAGE
#   capture-composite-reference.sh --xbe-id <id> [--record-extra N]
#                                  [--no-upload]
#                                  [--device NAME] [--label LBL]
#                                  [--skip-preflight]
#                                  [--preflight-timeout SECONDS]
#                                  [--preflight-mode auto|xemu-capture|ffmpeg]
#
# DEFAULTS
#   --xbe-id        required; one of mirror, color-channel, depth-floor, ...
#   --record-extra  8   (seconds added to the recording window beyond
#                        the diag's intrinsic ~30 s chainload + render +
#                        reboot cycle; do NOT confuse with the diag's
#                        on-screen hold time, which is hardcoded to 1.5 s
#                        in `xbed_capture_and_reboot()` and is NOT
#                        overridable from this script — see
#                        `xbe-tests/lib/xbed_capture.c::xbed_capture_and_reboot`)
#   --device        USB2      (composite-record.sh's MS2109 device match)
#   --label         composite (output PNG suffix)
#   --preflight-timeout  8        (composite-preflight.sh wall-clock seconds)
#   --preflight-mode     ffmpeg   (auto|xemu-capture|ffmpeg; defaults to ffmpeg
#                                   to match composite-record.sh's recorder
#                                   backend; override via --preflight-mode or
#                                   COMPOSITE_PREFLIGHT_MODE if you want auto)
#
# PREFLIGHT (cycle 34, 2026-05-23)
#   Before issuing the Xbox-side reboot / FTP-upload / ensure-agent / runxbe
#   sequence, this wrapper now runs `composite-preflight.sh` synchronously
#   against the same MS2109 device parameters. If the preflight fails
#   (no_signal, device_not_found, no_backend, host-side backend error) the
#   wrapper aborts WITHOUT touching the Xbox.
#
# OUTPUT
#   benchmark-runs/<UTC>-composite-ref-<id>/   raw capture run dir
#   docs/apple-silicon/xbox-real-references/<id>/<label>.png
#
# Exit codes:
#   0   success (capture + extract + upload all clean)
#   1   host-side wrapper failure (capture / extract / upload step failed,
#       or preflight host-side backend error)
#   2   cycle-34 preflight: no_signal
#   3   cycle-34 preflight: device_not_found
#   4   cycle-34 preflight: no detector backend available or launcher missing
#   5   cycle-34 preflight: invalid CLI usage

set -u
set -o pipefail

XBE_ID=""
RECORD_EXTRA_S=8
UPLOAD=1
DEVICE="USB2"
LABEL="composite"
SKIP_PREFLIGHT=0
_COMPOSITE_PREFLIGHT_TIMEOUT_ENV="${COMPOSITE_PREFLIGHT_TIMEOUT:-}"
if [[ "$_COMPOSITE_PREFLIGHT_TIMEOUT_ENV" =~ ^[1-9][0-9]*$ ]]; then
    PREFLIGHT_TIMEOUT="$_COMPOSITE_PREFLIGHT_TIMEOUT_ENV"
else
    PREFLIGHT_TIMEOUT=8
fi
case "${COMPOSITE_PREFLIGHT_MODE:-}" in
    auto|xemu-capture|ffmpeg) PREFLIGHT_MODE="$COMPOSITE_PREFLIGHT_MODE" ;;
    *) PREFLIGHT_MODE="ffmpeg" ;;
esac
unset _COMPOSITE_PREFLIGHT_TIMEOUT_ENV
ORACLE_HOST="${ORACLE_HOST:-192.168.0.200}"
ORACLE_FTP_USER="${ORACLE_FTP_USER:-xbox}"
ORACLE_FTP_PASS="${ORACLE_FTP_PASS:-xbox}"
export ORACLE_HOST ORACLE_FTP_USER ORACLE_FTP_PASS

require_value() {
    if [ "$#" -lt 2 ]; then
        echo "[capture-composite-ref] flag '$1' requires a value" >&2
        exit 5
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --xbe-id) require_value "$@"; XBE_ID="$2"; shift 2;;
        --record-extra) require_value "$@"; RECORD_EXTRA_S="$2"; shift 2;;
        --hold)
            require_value "$@"
            echo "[capture-composite-ref] WARNING: --hold renamed to --record-extra (see header)" >&2
            RECORD_EXTRA_S="$2"; shift 2;;
        --no-upload) UPLOAD=0; shift;;
        --device) require_value "$@"; DEVICE="$2"; shift 2;;
        --label) require_value "$@"; LABEL="$2"; shift 2;;
        --skip-preflight) SKIP_PREFLIGHT=1; shift;;
        --preflight-timeout) require_value "$@"; PREFLIGHT_TIMEOUT="$2"; shift 2;;
        --preflight-mode) require_value "$@"; PREFLIGHT_MODE="$2"; shift 2;;
        -h|--help)
            awk '
                NR == 1 { next }
                /^#/ {
                    line = $0
                    sub(/^# ?/, "", line)
                    print line
                    if ($0 ~ /^# Exit codes:/) {
                        saw_exit_codes = 1
                    }
                    next
                }
                saw_exit_codes { exit }
                { exit }
            ' "$0"
            exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 5;;
    esac
done

if [ -z "$XBE_ID" ]; then
    echo "usage: $0 --xbe-id <id> [--hold N] [--no-upload]" >&2
    exit 5
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
FORK="$(cd "$HERE/../.." && pwd)"
XBE_DIR="$FORK/scripts/apple-silicon/xbe-tests/$XBE_ID"

if [ ! -f "$XBE_DIR/bin/default.xbe" ]; then
    echo "diag XBE not built: $XBE_DIR/bin/default.xbe" >&2
    echo "Run \`make\` in $XBE_DIR first." >&2
    exit 1
fi

OUT_DIR="$FORK/benchmark-runs/$(date -u +%Y%m%dT%H%M%SZ)-composite-ref-$XBE_ID"
mkdir -p "$OUT_DIR"

echo "[capture-composite-ref] xbe=$XBE_ID record-extra=${RECORD_EXTRA_S}s out=$OUT_DIR"

# Synchronously confirm the MS2109 is producing frames BEFORE we touch the
# Xbox. This wrapper backgrounds composite-record.sh and would otherwise still
# chainload the XBE if the recorder's own internal preflight failed.
PREFLIGHT_DIR="$OUT_DIR/preflight"
PREFLIGHT_BIN="$HERE/composite-preflight.sh"
PREFLIGHT_STATUS="skipped"
PREFLIGHT_DETECTOR="none"
PREFLIGHT_RC=0
PREFLIGHT_ELAPSED_S=""
PREFLIGHT_META=""
if [ "$SKIP_PREFLIGHT" -eq 1 ]; then
    echo "[capture-composite-ref] preflight SKIPPED (--skip-preflight)"
elif [ ! -x "$PREFLIGHT_BIN" ]; then
    echo "[capture-composite-ref] composite-preflight.sh missing or not executable at $PREFLIGHT_BIN" >&2
    echo "[capture-composite-ref] pass --skip-preflight to bypass intentionally" >&2
    python3 - "$OUT_DIR/wrapper-result.json" "$XBE_ID" "$DEVICE" "$LABEL" \
        "$PREFLIGHT_BIN" <<'PY'
import datetime, json, os, sys
(result_path, xbe_id, device, label, preflight_bin) = sys.argv[1:6]
obj = {
    "schema": "capture-composite-reference/v1",
    "wrote_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "status": "preflight-failed",
    "xbe_id": xbe_id,
    "device": device,
    "label": label,
    "xbox_touched": False,
    "preflight": {
        "status": "launcher_missing",
        "exit_code": 4,
        "detector": "none",
        "elapsed_s": None,
        "meta_path": None,
        "launcher_path": preflight_bin,
        "launcher_executable": os.access(preflight_bin, os.X_OK),
    },
}
with open(result_path, "w") as f:
    json.dump(obj, f, indent=2)
    f.write("\n")
PY
    exit 4
else
    mkdir -p "$PREFLIGHT_DIR"
    PREFLIGHT_ARGS=(
        --device "$DEVICE"
        --audio-device "$DEVICE"
        --timeout "$PREFLIGHT_TIMEOUT"
        --mode "$PREFLIGHT_MODE"
        --out-dir "$PREFLIGHT_DIR"
        --quiet
    )
    echo "[capture-composite-ref] preflight: $PREFLIGHT_BIN ${PREFLIGHT_ARGS[*]}"
    PREFLIGHT_RC=0
    "$PREFLIGHT_BIN" "${PREFLIGHT_ARGS[@]}" || PREFLIGHT_RC=$?
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
        echo "[capture-composite-ref] preflight FAILED (status=$PREFLIGHT_STATUS, rc=$PREFLIGHT_RC, detector=$PREFLIGHT_DETECTOR)" >&2
        echo "[capture-composite-ref] meta: $PREFLIGHT_META" >&2
        echo "[capture-composite-ref] aborting BEFORE rebooting Xbox / uploading / chainloading $XBE_ID — no Xbox-side action taken." >&2
        python3 - "$OUT_DIR/wrapper-result.json" "$XBE_ID" "$DEVICE" "$LABEL" \
            "$PREFLIGHT_META" "$PREFLIGHT_STATUS" "$PREFLIGHT_DETECTOR" \
            "$PREFLIGHT_RC" "$PREFLIGHT_ELAPSED_S" <<'PY'
import datetime, json, os, sys
(result_path, xbe_id, device, label,
 pf_meta, pf_status, pf_detector, pf_rc, pf_elapsed) = sys.argv[1:10]
obj = {
    "schema": "capture-composite-reference/v1",
    "wrote_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "status": "preflight-failed",
    "xbe_id": xbe_id,
    "device": device,
    "label": label,
    "xbox_touched": False,
    "preflight": {
        "status": pf_status,
        "exit_code": int(pf_rc),
        "detector": pf_detector,
        "elapsed_s": float(pf_elapsed) if pf_elapsed else None,
        "meta_path": pf_meta if (pf_meta and os.path.exists(pf_meta)) else None,
    },
}
with open(result_path, "w") as f:
    json.dump(obj, f, indent=2)
    f.write("\n")
PY
        exit "$PREFLIGHT_RC"
    fi
    echo "[capture-composite-ref] preflight OK (detector=$PREFLIGHT_DETECTOR, elapsed=${PREFLIGHT_ELAPSED_S:-?}s)"
fi

# 1. Reboot to free FTP if uploading.
if [ "$UPLOAD" -eq 1 ]; then
    echo "[capture-composite-ref] reboot to free FTP for diag upload"
    python3 "$HERE/oracle-client.py" reboot > /dev/null 2>&1 || true
    echo "[capture-composite-ref] waiting up to 60s for FTP to come back"
    for i in $(seq 1 30); do
        sleep 2
        if python3 -c "import socket; s=socket.create_connection(('$ORACLE_HOST',21),timeout=2); s.close()" 2>/dev/null; then
            break
        fi
    done

    # Upload XBE.
    python3 -c "
from ftplib import FTP
f = FTP('$ORACLE_HOST', timeout=15)
f.login('$ORACLE_FTP_USER','$ORACLE_FTP_PASS')
try: f.cwd('/E/Apps/$XBE_ID')
except Exception:
    f.cwd('/E/Apps')
    try: f.mkd('$XBE_ID')
    except Exception: pass
    f.cwd('/E/Apps/$XBE_ID')
with open('$XBE_DIR/bin/default.xbe','rb') as fp:
    f.storbinary('STOR default.xbe', fp)
f.quit()
" || { echo "upload failed" >&2; exit 1; }
    echo "[capture-composite-ref] uploaded $XBE_ID"
fi

# 2. Re-launch agent + start composite recording.
python3 "$HERE/oracle-orchestrator.py" ensure-agent > /dev/null 2>&1 \
    || { echo "ensure-agent failed" >&2; exit 1; }

# 3. Start composite recording in background. Total record window:
#    hold + agent transition (~30s reboot back to dashboard) =
#    RECORD_EXTRA_S + 30 (covers the diag's render loop + reboot back).
REC_DURATION=$((RECORD_EXTRA_S + 30))
LABEL_TAG="${LABEL}-${XBE_ID}"
REC_PREFLIGHT_ARGS=(--skip-preflight)
echo "[capture-composite-ref] starting composite-record.sh for ${REC_DURATION}s..."
"$HERE/composite-record.sh" --duration "$REC_DURATION" --label "$LABEL_TAG" \
    --device "$DEVICE" "${REC_PREFLIGHT_ARGS[@]}" \
    > "$OUT_DIR/composite-record.log" 2>&1 &
REC_PID=$!

# Give ffmpeg ~2s to start capturing before we trigger the diag.
sleep 2

# 4. Chainload diag.
echo "[capture-composite-ref] chainloading diag $XBE_ID..."
python3 "$HERE/oracle-client.py" runxbe \
    "E:\\Apps\\$XBE_ID\\default.xbe" > /dev/null 2>&1 || true

# 5. Wait for ffmpeg to finish. The recording window covers:
#    - ~5 s for the diag to render its 300-frame loop on real Xbox
#    - ~1.5 s of post-render hold (hardcoded in xbed_capture_and_reboot)
#    - ~25 s for HalReturnToFirmware → dashboard reload
#    - $RECORD_EXTRA_S buffer to ensure we capture the post-render
#      drawable before the reboot
wait "$REC_PID" || echo "[capture-composite-ref] ffmpeg exited with status $?"

# 6. Find the produced video.
REC_VID=$(find "$FORK/benchmark-runs" -maxdepth 2 -name "video.mp4" \
    -path "*-composite-${LABEL_TAG}*/*" -newer "$OUT_DIR" -print -quit 2>/dev/null | head -1)
if [ -z "$REC_VID" ]; then
    REC_VID=$(ls -t "$FORK/benchmark-runs"/*"-composite-${LABEL_TAG}"*/video.mp4 2>/dev/null | head -1)
fi
if [ -z "$REC_VID" ] || [ ! -f "$REC_VID" ]; then
    echo "[capture-composite-ref] no video.mp4 produced; see $OUT_DIR/composite-record.log" >&2
    exit 1
fi
echo "[capture-composite-ref] video at $REC_VID"
ln -sf "$REC_VID" "$OUT_DIR/video.mp4" 2>/dev/null || cp "$REC_VID" "$OUT_DIR/video.mp4"

# 7. Extract scene-change keyframes.
KF_DIR="$OUT_DIR/keyframes"
mkdir -p "$KF_DIR"
python3 "$HERE/extract-keyframes.py" "$REC_VID" \
    --threshold 0.10 --every-s 1 --max-keyframes 30 \
    --out "$KF_DIR" > "$OUT_DIR/extract-keyframes.log" 2>&1 || true

# 8. Auto-pick the keyframe that best matches the diag's expected
# math-derived oracle. Synthesize the expected PNG, then find the
# scene/timed keyframe with the highest similarity.
EXP_PNG="$OUT_DIR/expected.png"
python3 "$XBE_DIR/expected.py" "$EXP_PNG" > /dev/null 2>&1 || \
    { echo "expected.py failed for $XBE_ID" >&2; exit 1; }

BEST_KF=""
BEST_PCT="100.0"
for kf in "$KF_DIR"/scene/*.png "$KF_DIR"/timed/*.png; do
    [ -f "$kf" ] || continue
    PCT=$(python3 "$HERE/compare-screenshots.py" "$kf" "$EXP_PNG" \
            --crop 0,0,640,480 --threshold 16 \
            --resize smaller --out-dir "$OUT_DIR/cmp-tmp" 2>/dev/null \
        | awk -F= '/^changed_pixels_pct=/{print $2}')
    if [ -n "$PCT" ]; then
        if awk -v a="$PCT" -v b="$BEST_PCT" 'BEGIN{exit (a<b)?0:1}'; then
            BEST_PCT="$PCT"
            BEST_KF="$kf"
        fi
    fi
done

if [ -z "$BEST_KF" ]; then
    echo "[capture-composite-ref] no keyframe extracted" >&2
    exit 1
fi
echo "[capture-composite-ref] best keyframe: $BEST_KF (changed_pixels_pct=$BEST_PCT)"

# 9. Save canonical reference.
DEST_DIR="$FORK/docs/apple-silicon/xbox-real-references/$XBE_ID"
mkdir -p "$DEST_DIR"
DEST_PNG="$DEST_DIR/${LABEL}.png"
cp "$BEST_KF" "$DEST_PNG"
echo "[capture-composite-ref] saved canonical reference: $DEST_PNG"
echo "[capture-composite-ref] best-match changed_pixels_pct vs math-derived: $BEST_PCT"

exit 0
