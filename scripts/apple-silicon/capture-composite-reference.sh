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
#
# OUTPUT
#   benchmark-runs/<UTC>-composite-ref-<id>/   raw capture run dir
#   docs/apple-silicon/xbox-real-references/<id>/<label>.png
#
# Exit code: 0 on success, 1 on any failure (capture / extract / upload).

set -u
set -o pipefail

XBE_ID=""
RECORD_EXTRA_S=8
UPLOAD=1
DEVICE="USB2"
LABEL="composite"
ORACLE_HOST="${ORACLE_HOST:-192.168.0.200}"
ORACLE_FTP_USER="${ORACLE_FTP_USER:-xbox}"
ORACLE_FTP_PASS="${ORACLE_FTP_PASS:-xbox}"
export ORACLE_HOST ORACLE_FTP_USER ORACLE_FTP_PASS

while [ $# -gt 0 ]; do
    case "$1" in
        --xbe-id) XBE_ID="$2"; shift 2;;
        --record-extra) RECORD_EXTRA_S="$2"; shift 2;;
        --hold)
            # Back-compat alias: --hold was misleadingly named in the
            # initial draft. Treat it as --record-extra and warn.
            echo "[capture-composite-ref] WARNING: --hold renamed to --record-extra (see header)" >&2
            RECORD_EXTRA_S="$2"; shift 2;;
        --no-upload) UPLOAD=0; shift;;
        --device) DEVICE="$2"; shift 2;;
        --label) LABEL="$2"; shift 2;;
        -h|--help) sed -n '1,/^# Exit code/p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

if [ -z "$XBE_ID" ]; then
    echo "usage: $0 --xbe-id <id> [--hold N] [--no-upload]" >&2
    exit 2
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

# Sanity: composite stick reachable?
if ! ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 \
        | grep -q "$DEVICE"; then
    echo "[capture-composite-ref] WARN: '$DEVICE' not in AVFoundation device list" >&2
    echo "  (run: ffmpeg -f avfoundation -list_devices true -i '')" >&2
    echo "[capture-composite-ref] proceeding anyway — hardware may still respond" >&2
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
echo "[capture-composite-ref] starting composite-record.sh for ${REC_DURATION}s..."
"$HERE/composite-record.sh" --duration "$REC_DURATION" --label "$LABEL_TAG" \
    --device "$DEVICE" > "$OUT_DIR/composite-record.log" 2>&1 &
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
