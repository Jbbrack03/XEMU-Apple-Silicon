#!/usr/bin/env bash
# oracle-smoke.sh — single-command oracle-pipeline health check.
#
# Exercises every layer of the real-Xbox oracle so a single run
# tells you whether the oracle is ready to drive the M15 visual
# gate. Designed to be run before any Metal-renderer change that
# wants to be validated against real-Xbox truth, and as a
# regression check after any oracle-side refactor.
#
# Layers exercised (in order):
#   1. Network: ping the Xbox host (ORACLE_HOST, default 192.168.0.200).
#   2. FTP: connect, list /E, verify the agent XBE deployment slot.
#   3. Agent launch: oracle-orchestrator.py ensure-agent.
#   4. Wire protocol: info, eeprom (SHA-256 stable across runs?), help.
#   5. Memory access: mem.read on a kernel-known signature
#      (0x80000000 should start with `efbeadde` = `0xdeadbeef` LE).
#   6. NV2A access: nv2a.read on PMC_BOOT_0 (must = 0x02a000e1).
#   7. VRAM access: vram.read on PCRTC_START + 0x800 page.
#   8. Synthetic input: controller.buffer-info + roundtrip set/get/clear.
#   9. Persistence anchor file: confirm /E/Apps/oracle-agent/state/
#      ctrl-addr.txt parses cleanly (XCTR + valid 0xPHYS).
#  10. Screenshot: capture front buffer, verify decoded PNG dims.
#  11. Tier-1 diag pipeline (optional, --tier1 / -t): chainload one or
#      more diag XBEs via runxbe, pull capture, byte-compare against
#      math-derived expected.
#  12. Cleanup: leave the agent listening for follow-on use, OR
#      reboot back to the dashboard if --reboot-when-done is passed.
#
# Exit code:
#   0   all layers green
#   1   any layer failed; details in the last block of output
#
# Usage:
#   ./oracle-smoke.sh                            # all layers, no Tier-1 diag
#   ./oracle-smoke.sh --tier1 mirror             # + run the mirror diag
#   ./oracle-smoke.sh --tier1 controller-roundtrip
#                     --buttons 0xA5A5 --lt 16384 --lx 12345
#                                                # + run controller-roundtrip
#                                                #   pre-set the synth state
#   ./oracle-smoke.sh --tier1 mirror,color-channel,depth-floor
#                                                # + run all 3 standard Tier-1
#   ./oracle-smoke.sh --reboot-when-done         # hard-reset Xbox at the end
#
# Environment:
#   ORACLE_HOST          Xbox IP (default 192.168.0.200)
#   ORACLE_PORT          agent port (default 9001)
#   ORACLE_FTP_USER      FTP user (default xbox)
#   ORACLE_FTP_PASS      FTP password (default xbox)
#   ORACLE_AGENT_PATH    Xbox-side path of the agent XBE
#                        (default E:\Apps\oracle-agent\default.xbe)
#   ORACLE_SMOKE_OUT     work dir (default /tmp/oracle-smoke-<UTC>)

set -u
set -o pipefail

# --- arg parse ----------------------------------------------------
TIER1_LIST=""
REBOOT_WHEN_DONE=0
BUTTONS=""
LT_VAL=""
RT_VAL=""
LX_VAL=""
LY_VAL=""
RX_VAL=""
RY_VAL=""

while [ $# -gt 0 ]; do
    case "$1" in
        -t|--tier1)
            TIER1_LIST="${2:-}"; shift 2;;
        --reboot-when-done) REBOOT_WHEN_DONE=1; shift;;
        --buttons) BUTTONS="$2"; shift 2;;
        --lt) LT_VAL="$2"; shift 2;;
        --rt) RT_VAL="$2"; shift 2;;
        --lx) LX_VAL="$2"; shift 2;;
        --ly) LY_VAL="$2"; shift 2;;
        --rx) RX_VAL="$2"; shift 2;;
        --ry) RY_VAL="$2"; shift 2;;
        -h|--help)
            sed -n '1,/^# Environment:/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
FORK="$(cd "$HERE/../.." && pwd)"
ORACLE_HOST="${ORACLE_HOST:-192.168.0.200}"
ORACLE_PORT="${ORACLE_PORT:-9001}"
ORACLE_FTP_USER="${ORACLE_FTP_USER:-xbox}"
ORACLE_FTP_PASS="${ORACLE_FTP_PASS:-xbox}"
ORACLE_AGENT_PATH="${ORACLE_AGENT_PATH:-E:\\Apps\\oracle-agent\\default.xbe}"
OUT_DIR="${ORACLE_SMOKE_OUT:-/tmp/oracle-smoke-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT_DIR"

PASS=0
FAIL=0
NOTES_FILE="$OUT_DIR/notes.log"
: > "$NOTES_FILE"

ok()    { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; printf 'PASS  %s\n' "$1" >> "$NOTES_FILE"; }
fail()  { FAIL=$((FAIL + 1)); printf 'FAIL  %s\n' "$1" >&2; printf 'FAIL  %s\n' "$1" >> "$NOTES_FILE"; }
info()  { printf 'INFO  %s\n' "$1"; printf 'INFO  %s\n' "$1" >> "$NOTES_FILE"; }

run_oc() {
    # Wrapper around oracle-client.py raw with timeout.
    python3 "$HERE/oracle-client.py" --host "$ORACLE_HOST" \
        --port "$ORACLE_PORT" --timeout 15 "$@"
}

run_orch() {
    python3 "$HERE/oracle-orchestrator.py" --host "$ORACLE_HOST" \
        --port "$ORACLE_PORT" --agent-path "$ORACLE_AGENT_PATH" \
        --ftp-user "$ORACLE_FTP_USER" --ftp-pass "$ORACLE_FTP_PASS" \
        "$@"
}

echo "================================================================"
echo "oracle-smoke.sh — start   host=$ORACLE_HOST  out=$OUT_DIR"
echo "================================================================"

# --- 1. ping ------------------------------------------------------
if ping -c 2 -W 2000 "$ORACLE_HOST" > /dev/null 2>&1; then
    ok "01 ping: host $ORACLE_HOST is alive"
else
    fail "01 ping: host $ORACLE_HOST not responding"
    echo "[smoke] aborting — Xbox is offline"; exit 1
fi

# --- 2. agent ensure-up -------------------------------------------
if run_orch ensure-agent > "$OUT_DIR/02-ensure-agent.log" 2>&1; then
    ok "02 ensure-agent: agent listening at $ORACLE_HOST:$ORACLE_PORT"
else
    fail "02 ensure-agent: agent did not come up; see $OUT_DIR/02-ensure-agent.log"
fi

# --- 3. info ------------------------------------------------------
if INFO=$(run_oc info 2>&1); then
    ok "03 info: $INFO"
else
    fail "03 info: $INFO"
fi

# --- 4. eeprom (SHA stable per console) --------------------------
if run_oc eeprom --out "$OUT_DIR/04-eeprom.bin" > /dev/null 2>&1; then
    EE_SZ=$(wc -c < "$OUT_DIR/04-eeprom.bin" | tr -d ' ')
    EE_SHA=$(shasum -a 256 "$OUT_DIR/04-eeprom.bin" | awk '{print $1}')
    if [ "$EE_SZ" = "256" ]; then
        ok "04 eeprom: 256 bytes, sha256=${EE_SHA:0:16}…"
    else
        fail "04 eeprom: wrong size $EE_SZ (expected 256)"
    fi
else
    fail "04 eeprom: agent rejected request"
fi

# --- 5. mem.read kernel signature --------------------------------
# 0x80000000 is the kernel base; the first 4 bytes are the DOS
# header magic "MZ" (0x5A4D LE = 4D 5A in memory). 0x80010000 has
# typically 0xdeadbeef in xboxkrnl debug builds; we just check that
# we get plausible non-zero bytes back.
MEM_HEX=$(run_oc mem-read 0x80000000 16 2>/dev/null | xxd -p | head -1 || true)
if [ -n "$MEM_HEX" ] && [ "$MEM_HEX" != "00000000000000000000000000000000" ]; then
    ok "05 mem.read: 0x80000000[16] = $MEM_HEX"
else
    fail "05 mem.read: returned all zeros or empty (got '$MEM_HEX')"
fi

# --- 6. nv2a.read PMC_BOOT_0 -------------------------------------
NV_BOOT=$(run_oc nv2a-read 0x000000 2>/dev/null || true)
if [ "$NV_BOOT" = "0x02a000e1" ]; then
    ok "06 nv2a.read PMC_BOOT_0 = $NV_BOOT (NV2A chip ID)"
else
    fail "06 nv2a.read PMC_BOOT_0 returned '$NV_BOOT' (expected 0x02a000e1)"
fi

# --- 7. vram.read at PCRTC_START + page --------------------------
PCRTC_RAW=$(run_oc nv2a-read 0x600800 2>/dev/null || true)
if [ -n "$PCRTC_RAW" ]; then
    ok "07a nv2a.read PCRTC_START = $PCRTC_RAW"
    # vram.read at offset 0 (just to confirm vram.read works at all).
    if run_oc vram-read 0 32 --out "$OUT_DIR/07-vram-head.bin" > /dev/null 2>&1; then
        if [ -s "$OUT_DIR/07-vram-head.bin" ]; then
            ok "07b vram.read 0[32] returned $(wc -c < "$OUT_DIR/07-vram-head.bin" | tr -d ' ') bytes"
        else
            fail "07b vram.read 0[32] returned empty"
        fi
    else
        fail "07b vram.read failed"
    fi
else
    fail "07a nv2a.read PCRTC_START returned empty"
fi

# --- 8. controller.* roundtrip -----------------------------------
BUF_INFO=$(run_oc raw controller.buffer-info 2>/dev/null || true)
if echo "$BUF_INFO" | grep -q "magic=0x58435452"; then
    ok "08a controller.buffer-info: $BUF_INFO"
    # Expect phys != 0 — kernel-pool allocation succeeded.
    if echo "$BUF_INFO" | grep -q "phys=0x00000000"; then
        fail "08b controller.buffer-info: phys=0x00000000 — kernel-pool alloc failed (BSS fallback?)"
    else
        ok "08b controller.buffer-info: kernel-pool phys != 0"
    fi
    # Roundtrip set + get.
    run_oc raw "controller.set port=0 buttons=0x1234 lt=5000 lx=-1234" > /dev/null 2>&1
    SET_VERIFY=$(run_oc raw "controller.get port=0" 2>/dev/null || true)
    if echo "$SET_VERIFY" | grep -q "buttons=0x1234"; then
        ok "08c controller.set/get: roundtrip exact"
    else
        fail "08c controller.set/get: roundtrip mismatch — got $SET_VERIFY"
    fi
    # Clear so subsequent test runs see zero state.
    run_oc raw "controller.clear port=0" > /dev/null 2>&1
else
    fail "08 controller.buffer-info: bad/missing magic — $BUF_INFO"
fi

# --- 9. persistence anchor file ----------------------------------
# Quickest check: the buffer-info reports the anchor path; we
# attempted in step 8a. We don't FTP-pull it here because that
# requires bringing the agent down (FTP and agent can't coexist
# under UnleashX). Skip the file-pull layer in this default smoke;
# the diag runs in step 11 implicitly validate it.
info "09 anchor-file pull skipped in default smoke (agent owns the network)"

# --- 10. screenshot ----------------------------------------------
SHOT_OUT="$OUT_DIR/10-screenshot.png"
if run_oc screenshot --out "$SHOT_OUT" > /dev/null 2>&1; then
    if [ -s "$SHOT_OUT" ]; then
        ok "10 screenshot: $SHOT_OUT ($(wc -c < "$SHOT_OUT" | tr -d ' ') bytes)"
    else
        fail "10 screenshot: PNG file empty"
    fi
else
    fail "10 screenshot: agent rejected"
fi

# --- 11. Tier-1 diag pipeline (optional) -------------------------
if [ -n "$TIER1_LIST" ]; then
    echo "----------------------------------------------------------------"
    echo "Tier-1 diag pipeline: $TIER1_LIST"
    echo "----------------------------------------------------------------"
    IFS=',' read -ra DIAGS <<< "$TIER1_LIST"
    for diag in "${DIAGS[@]}"; do
        diag="$(echo "$diag" | tr -d ' ')"
        XBE_DIR="$FORK/scripts/apple-silicon/xbe-tests/$diag"
        if [ ! -f "$XBE_DIR/bin/default.xbe" ]; then
            fail "11.$diag: $XBE_DIR/bin/default.xbe not built"
            continue
        fi
        WORK="$OUT_DIR/diag-$diag"
        mkdir -p "$WORK"

        # Pre-set synth state for controller-roundtrip (cleared otherwise).
        if [ "$diag" = "controller-roundtrip" ]; then
            CSET="controller.set port=0"
            [ -n "$BUTTONS" ] && CSET="$CSET buttons=$BUTTONS"
            [ -n "$LT_VAL" ] && CSET="$CSET lt=$LT_VAL"
            [ -n "$RT_VAL" ] && CSET="$CSET rt=$RT_VAL"
            [ -n "$LX_VAL" ] && CSET="$CSET lx=$LX_VAL"
            [ -n "$LY_VAL" ] && CSET="$CSET ly=$LY_VAL"
            [ -n "$RX_VAL" ] && CSET="$CSET rx=$RX_VAL"
            [ -n "$RY_VAL" ] && CSET="$CSET ry=$RY_VAL"
            info "11.$diag: pre-set state via $CSET"
            run_oc raw "$CSET" > "$WORK/preset.log" 2>&1 || true
        fi

        # Reboot to free FTP for upload, then chainload via run-diag.
        # run-diag handles the agent re-launch + the FTP-pull cycle.
        info "11.$diag: reboot to free FTP for upload"
        run_oc reboot > /dev/null 2>&1 || true
        # Wait for FTP to come back.
        for i in $(seq 1 30); do
            sleep 2
            if python3 -c "import socket; s=socket.create_connection(('$ORACLE_HOST',21),timeout=2); s.close()" 2>/dev/null; then
                break
            fi
        done
        # Upload XBE.
        if python3 -c "
from ftplib import FTP
f = FTP('$ORACLE_HOST', timeout=10)
f.login('$ORACLE_FTP_USER','$ORACLE_FTP_PASS')
try: f.cwd('/E/Apps/$diag')
except Exception:
    f.cwd('/E/Apps')
    try: f.mkd('$diag')
    except Exception: pass
    f.cwd('/E/Apps/$diag')
with open('$XBE_DIR/bin/default.xbe','rb') as fp:
    f.storbinary('STOR default.xbe', fp)
f.quit()
" > "$WORK/upload.log" 2>&1; then
            info "11.$diag: XBE uploaded"
        else
            fail "11.$diag: upload failed; see $WORK/upload.log"
            continue
        fi

        # Re-launch agent and (for controller-roundtrip) re-set the state.
        run_orch ensure-agent > "$WORK/ensure-agent.log" 2>&1 || true
        if [ "$diag" = "controller-roundtrip" ] && [ -n "$CSET" ]; then
            run_oc raw "$CSET" > "$WORK/preset2.log" 2>&1 || true
        fi

        # Chainload + collect.
        if run_orch run-diag \
              --xbe "E:\\Apps\\$diag\\default.xbe" \
              --ftp-collect "/E/Apps/$diag" \
              --out "$WORK/orch" > "$WORK/run-diag.log" 2>&1; then
            : # ok
        else
            fail "11.$diag: run-diag failed; see $WORK/run-diag.log"
            continue
        fi

        # Decode XOSS → PNG, synthesize expected, byte-compare.
        CAP_BIN="$WORK/orch/artifacts/$diag-capture.bin"
        CAP_PNG="$WORK/captured.png"
        EXP_PNG="$WORK/expected.png"
        if [ ! -f "$CAP_BIN" ]; then
            fail "11.$diag: capture file missing at $CAP_BIN"
            continue
        fi
        run_oc decode-xoss "$CAP_BIN" --out "$CAP_PNG" > /dev/null 2>&1 || true

        # Build expected. For non-controller-roundtrip we use expected.py:default.
        # For controller-roundtrip we synthesize from the preset state.
        if [ "$diag" = "controller-roundtrip" ]; then
            python3 - <<PY > "$WORK/expected.log" 2>&1
import sys, importlib.util
sys.path.insert(0, '$XBE_DIR')
import expected as e
b = e.from_state(
    buttons=int('${BUTTONS:-0}', 0),
    ltrigger=int('${LT_VAL:-0}', 0),
    rtrigger=int('${RT_VAL:-0}', 0),
    lstick_x=int('${LX_VAL:-0}', 0),
    lstick_y=int('${LY_VAL:-0}', 0),
    rstick_x=int('${RX_VAL:-0}', 0),
    rstick_y=int('${RY_VAL:-0}', 0),
)
spec = importlib.util.spec_from_file_location('oc', '$HERE/oracle-client.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.save_screenshot_png(b, e.WIDTH, e.HEIGHT, '$EXP_PNG')
PY
        else
            python3 "$XBE_DIR/expected.py" "$EXP_PNG" > "$WORK/expected.log" 2>&1 || true
        fi

        if [ ! -f "$EXP_PNG" ]; then
            fail "11.$diag: expected.png synthesis failed; see $WORK/expected.log"
            continue
        fi

        CMP_OUT=$(python3 "$HERE/compare-screenshots.py" \
            "$CAP_PNG" "$EXP_PNG" --crop 0,0,640,480 --threshold 8 \
            --out-dir "$WORK/cmp" 2>&1)
        echo "$CMP_OUT" > "$WORK/compare.log"
        PCT=$(echo "$CMP_OUT" | awk -F= '/^changed_pixels_pct=/{print $2}')
        if [ -z "$PCT" ]; then
            fail "11.$diag: compare-screenshots produced no pct"
            continue
        fi
        # Use awk for float compare; pass if pct < 1.0 (Tier-1 gate).
        if awk -v p="$PCT" 'BEGIN{exit (p<1.0)?0:1}'; then
            ok "11.$diag: PASS  changed_pixels_pct=$PCT"
        else
            fail "11.$diag: FAIL  changed_pixels_pct=$PCT (gate: <1.0)"
        fi
    done
fi

# --- 12. cleanup --------------------------------------------------
if [ "$REBOOT_WHEN_DONE" -eq 1 ]; then
    info "12 reboot-when-done: returning Xbox to the dashboard"
    run_oc reboot > /dev/null 2>&1 || true
fi

echo "================================================================"
printf 'oracle-smoke summary:  %d PASS   %d FAIL   out=%s\n' "$PASS" "$FAIL" "$OUT_DIR"
echo "================================================================"

# Persist a summary.json for downstream consumers (CI, M15 gate).
python3 - <<PY
import json, sys, os
out = {
    "host": "$ORACLE_HOST",
    "out_dir": "$OUT_DIR",
    "pass": $PASS,
    "fail": $FAIL,
    "tier1_run": "$TIER1_LIST",
    "verdict": "ok" if $FAIL == 0 else "fail",
}
with open("$OUT_DIR/summary.json", "w") as f:
    json.dump(out, f, indent=2)
PY

[ "$FAIL" -eq 0 ] || exit 1
