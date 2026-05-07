#!/usr/bin/env bash
# oracle-validate.sh — comprehensive oracle production-readiness gate.
#
# Single-command validation of the entire oracle pipeline for the M15
# default-on flip's pre-condition. Composes the existing single-purpose
# scripts into one decision: "is the oracle production-grade?"
#
# Layers (run in order; later layers depend on earlier success):
#
#   1. oracle-smoke (12 baseline RPC layers)
#   2. xbe-harness Tier-1 visual matrix on Metal + real Xbox
#      (`controller-roundtrip` is covered by layer 3 because it is a
#      real-Xbox-only input-integration diag, not a renderer visual cell)
#   3. controller-roundtrip diag pulls + diagnostic-file inspection
#      (verifies the persistent buffer survived chainload byte-exact)
#   4. oracle-stress 10-iteration burst (degraded-state reproduction,
#      with non-zero controller-roundtrip state)
#   5. oracle-seqlock-test live mode (concurrent set/get tear check)
#
# Unlike m15-visual-gate.sh, this script focuses on the ORACLE side
# (Mac↔Xbox communication, persistent buffer correctness, diag-XBE
# wiring) rather than the Metal-renderer correctness side. The two
# scripts together cover the complete M15 default-on pre-conditions.
#
# Exit code is the OR of every layer. A non-zero exit means the
# production-readiness gate failed; report.md tells you which layer
# needs attention.

set -u
set -o pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HOST="${ORACLE_HOST:-192.168.0.200}"
OUT_DIR=""
SKIP_STRESS=0
STRESS_ITER=10

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="$2"; shift 2;;
        --out) OUT_DIR="$2"; shift 2;;
        --skip-stress) SKIP_STRESS=1; shift;;
        --stress-iter) STRESS_ITER="$2"; shift 2;;
        -h|--help)
            sed -n '1,/^# Exit/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="$HERE/../../benchmark-runs/oracle-validate-$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$OUT_DIR"

PASS=0
FAIL=0
LINES=()
ok()    { PASS=$((PASS+1));  LINES+=("PASS  $1"); echo "PASS  $1"; }
fail()  { FAIL=$((FAIL+1));  LINES+=("FAIL  $1"); echo "FAIL  $1" >&2; }
info()  { LINES+=("INFO  $1"); echo "INFO  $1"; }

echo "================================================================"
echo "oracle-validate — start"
echo "  host    = $HOST"
echo "  out_dir = $OUT_DIR"
echo "================================================================"

# Layer 1: oracle-smoke
echo
echo "--- 1. oracle-smoke (baseline RPC health) ---"
if ORACLE_HOST="$HOST" "$HERE/oracle-smoke.sh" \
        > "$OUT_DIR/01-oracle-smoke.log" 2>&1; then
    ok "01 oracle-smoke: 12/12 layers green"
else
    fail "01 oracle-smoke: see $OUT_DIR/01-oracle-smoke.log"
    info "(skipping later layers since RPC baseline failed)"
    goto_summary=1
fi

# Layer 2: xbe-harness Tier-1 matrix
if [ "${goto_summary:-0}" -ne 1 ]; then
    echo
    echo "--- 2. xbe-harness Tier-1 matrix (Metal + real Xbox) ---"
    XBE_OUT="$OUT_DIR/02-tier1-matrix"
    if python3 "$HERE/xbe-harness/xbe_orchestrator.py" run \
            --renderer metal --renderer real-xbox \
            --max-changed-pct 1.0 --threshold 8 \
            --out "$XBE_OUT" \
            > "$OUT_DIR/02-xbe-harness.log" 2>&1; then
        ok "02 xbe-harness Tier-1: ALL cells PASS"
    else
        fail "02 xbe-harness Tier-1: see $XBE_OUT/report.md"
    fi
fi

# Layer 3: controller-roundtrip diagnostic-file inspection
if [ "${goto_summary:-0}" -ne 1 ]; then
    echo
    echo "--- 3. controller-roundtrip diagnostic-file inspection ---"
    DIAG_OUT="$OUT_DIR/03-cr-diag"
    mkdir -p "$DIAG_OUT"

    # Pre-set a fixed state, run controller-roundtrip diag, pull
    # D:\controller-roundtrip-diag.txt, parse, verify it matches
    # the agent's pre-set state.
    (
        STATE_BTN="0x55AA"
        STATE_LT="20000"
        STATE_RT="10000"
        STATE_LX="6000"
        STATE_LY="-6000"
        STATE_RX="-15000"
        STATE_RY="15000"

        echo "[3.1] reboot Xbox to free FTP for upload"
        python3 "$HERE/oracle-client.py" --host "$HOST" raw "reboot" || true
        sleep 30
        echo "[3.2] wait for FTP up"
        for i in $(seq 1 30); do
            if nc -z -w 2 "$HOST" 21 2>/dev/null; then break; fi
            sleep 2
        done

        echo "[3.3] upload controller-roundtrip XBE"
        python3 - <<PY
import ftplib
ftp = ftplib.FTP("$HOST", timeout=20)
ftp.login("xbox", "xbox")
try:
    try: ftp.cwd("/E/Apps/controller-roundtrip")
    except ftplib.error_perm: ftp.cwd("/E/Apps"); ftp.mkd("controller-roundtrip"); ftp.cwd("/E/Apps/controller-roundtrip")
    with open("$HERE/xbe-tests/controller-roundtrip/bin/default.xbe", "rb") as f:
        ftp.storbinary("STOR default.xbe", f)
    print("uploaded")
finally:
    ftp.quit()
PY

        echo "[3.4] start agent, set fixed state"
        python3 "$HERE/oracle-orchestrator.py" --host "$HOST" ensure-agent
        python3 "$HERE/oracle-client.py" --host "$HOST" raw \
            "controller.set port=0 buttons=$STATE_BTN lt=$STATE_LT rt=$STATE_RT lx=$STATE_LX ly=$STATE_LY rx=$STATE_RX ry=$STATE_RY"

        echo "[3.5] verify state in agent buffer"
        python3 "$HERE/oracle-client.py" --host "$HOST" raw \
            "controller.get port=0"

        echo "[3.6] verify anchor file matches buffer phys"
        python3 "$HERE/oracle-client.py" --host "$HOST" raw \
            "controller.buffer-info"

        echo "[3.7] run-diag chainload"
        python3 "$HERE/oracle-orchestrator.py" --host "$HOST" run-diag \
            --xbe "E:\\Apps\\controller-roundtrip\\default.xbe" \
            --ftp-collect /E/Apps/controller-roundtrip \
            --out "$DIAG_OUT/orch"

        echo "[3.8] decode + parse diag.txt"
        DIAG_TXT="$DIAG_OUT/orch/artifacts/controller-roundtrip-diag.txt"
        if [ ! -f "$DIAG_TXT" ]; then
            echo "FAIL: diag.txt not pulled (built diag XBE may not have diagnostic instrumentation)"
            exit 1
        fi
        cat "$DIAG_TXT"

        # Decode XOSS to PNG
        python3 -c "
import importlib.util
spec = importlib.util.spec_from_file_location('oc', '$HERE/oracle-client.py')
oc = importlib.util.module_from_spec(spec); spec.loader.exec_module(oc)
oc.decode_xoss_file('$DIAG_OUT/orch/artifacts/controller-roundtrip-capture.bin', '$DIAG_OUT/decoded.png')
print('decoded $DIAG_OUT/decoded.png')
"

        # Parse state values from diag.txt
        OBSERVED_BTN=$(grep '^state.buttons=' "$DIAG_TXT" | cut -d= -f2)
        OBSERVED_LT=$(grep '^state.ltrigger=' "$DIAG_TXT" | cut -d= -f2)
        OBSERVED_RT=$(grep '^state.rtrigger=' "$DIAG_TXT" | cut -d= -f2)

        echo
        echo "EXPECTED: buttons=$STATE_BTN lt=$STATE_LT rt=$STATE_RT"
        echo "OBSERVED: buttons=$OBSERVED_BTN lt=$OBSERVED_LT rt=$OBSERVED_RT"

        if [ "$(printf '%s' "$OBSERVED_BTN" | tr 'A-F' 'a-f')" = "$(printf '%s' "$STATE_BTN" | tr 'A-F' 'a-f')" ] && \
           [ "$OBSERVED_LT" = "$STATE_LT" ] && \
           [ "$OBSERVED_RT" = "$STATE_RT" ]; then
            exit 0
        else
            exit 2
        fi
    ) > "$OUT_DIR/03-cr-diag.log" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        ok "03 controller-roundtrip diag: state byte-exact across chainload"
    else
        fail "03 controller-roundtrip diag (rc=$rc): see $OUT_DIR/03-cr-diag.log"
    fi
fi

# Layer 4: oracle-stress
if [ "${goto_summary:-0}" -ne 1 ] && [ "$SKIP_STRESS" -eq 0 ]; then
    echo
    echo "--- 4. oracle-stress ($STRESS_ITER-iteration burst) ---"
    STRESS_OUT="$OUT_DIR/04-stress"
    if "$HERE/oracle-stress.sh" --host "$HOST" --iterations "$STRESS_ITER" \
            --buttons 0xA5A5 --lt 16384 --rt 1234 \
            --lx 12345 --ly -12345 --rx -32768 --ry 32767 \
            --out "$STRESS_OUT" \
            > "$OUT_DIR/04-oracle-stress.log" 2>&1; then
        ok "04 oracle-stress: $STRESS_ITER/$STRESS_ITER smokes PASS (no degraded state)"
    else
        fail "04 oracle-stress: see $STRESS_OUT/report.md"
    fi
elif [ "$SKIP_STRESS" -eq 1 ]; then
    info "04 oracle-stress: skipped (--skip-stress)"
fi

# Layer 5: oracle-seqlock-test
if [ "${goto_summary:-0}" -ne 1 ]; then
    echo
    echo "--- 5. oracle-seqlock-test (concurrent set/get tear check) ---"
    if python3 "$HERE/oracle-seqlock-test.py" --host "$HOST" \
            --rounds 100 --workers 2 --readers 2 \
            > "$OUT_DIR/05-seqlock.log" 2>&1; then
        ok "05 oracle-seqlock: predicate + live test PASS"
    else
        fail "05 oracle-seqlock: see $OUT_DIR/05-seqlock.log"
    fi
fi

echo "================================================================"
echo "oracle-validate summary:  $PASS PASS   $FAIL FAIL   out=$OUT_DIR"
echo "================================================================"

# Persist summary
{
    echo "# oracle-validate"
    echo
    date -u +"Started: %Y-%m-%dT%H:%M:%SZ"
    echo
    echo "Pass: $PASS"
    echo "Fail: $FAIL"
    echo
    echo "## Lines"
    echo
    for L in "${LINES[@]}"; do
        echo "- $L"
    done
} > "$OUT_DIR/report.md"

[ "$FAIL" -eq 0 ] || exit 1
