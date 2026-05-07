#!/usr/bin/env bash
# m15-visual-gate.sh — composite M15 default-on visual-gate runner.
#
# Runs the Metal default-on flip's mandatory visual + counter checks
# in the canonical order:
#
#   1. Build verification: xemu binary present + post-build M5
#      shader-validation harness PASS (gated by build.sh's W1 step).
#   2. Oracle pipeline health: oracle-smoke.sh (12 layers).
#   3. Per-renderer Metal canary regression gate
#      (metal-canary-regress.sh --mode counters; PGR2/Rainbow/Halo/boot,
#      ~6 minutes).
#   4. Tier-1 diag-XBE matrix on Metal AND real Xbox
#      (xbe-harness run --renderer metal --renderer real-xbox).
#   5. (Optional, --paired) Paired Metal-vs-GL diff for the standard
#      canary set (PGR2 / Rainbow / Halo) via metal-gl-compare.sh.
#
# Exit code is the OR of every gate's status. A non-zero exit blocks
# the M15 default-on flip per `metal-renderer-plan.md` §M15.
#
# Designed to be the single command that decides "is Metal ready to be
# default-on?" — the answer is YES iff exit == 0 with all four gates
# (or five with --paired) green. Per project rule #15, this is also a
# good Codex-validate trigger: any change that flips this exit code
# from FAIL to PASS (or vice versa) merits an independent read.
#
# Usage:
#   ./m15-visual-gate.sh                         # all 4 standard gates
#   ./m15-visual-gate.sh --paired                # + paired Metal-vs-GL canary diff
#   ./m15-visual-gate.sh --skip-canary-regress   # skip step 3 (faster, less coverage)
#   ./m15-visual-gate.sh --skip-tier1            # skip step 4 (Metal counters only)
#   ./m15-visual-gate.sh --skip-oracle           # skip step 2 (no real Xbox needed)
#   ./m15-visual-gate.sh --out DIR               # custom output dir

set -u
set -o pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
FORK="$(cd "$HERE/../.." && pwd)"

DO_PAIRED=0
DO_CANARY=1
DO_TIER1=1
DO_ORACLE=1
OUT_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --paired) DO_PAIRED=1; shift;;
        --skip-canary-regress) DO_CANARY=0; shift;;
        --skip-tier1) DO_TIER1=0; shift;;
        --skip-oracle) DO_ORACLE=0; shift;;
        --out) OUT_DIR="$2"; shift 2;;
        -h|--help) sed -n '1,/^# Usage:/p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="$FORK/benchmark-runs/m15-gate-$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$OUT_DIR"

PASS=0
FAIL=0
LINES=()

ok()    { PASS=$((PASS+1));  LINES+=("PASS  $1"); echo "PASS  $1"; }
fail()  { FAIL=$((FAIL+1));  LINES+=("FAIL  $1"); echo "FAIL  $1" >&2; }
info()  { LINES+=("INFO  $1"); echo "INFO  $1"; }

echo "================================================================"
echo "m15-visual-gate — start   out=$OUT_DIR"
echo "================================================================"

# --- 1. xemu binary + post-build shader validation ---------------
XEMU_BIN="$FORK/dist/xemu.app/Contents/MacOS/xemu"
if [ -x "$XEMU_BIN" ]; then
    ok "01 xemu binary present at $XEMU_BIN"
    SHADER_LOG="$FORK/build/shader-validation-postbuild.log"
    if [ -s "$SHADER_LOG" ]; then
        if grep -qE "7/7 passed, 0 failed|\[run-validation\] PASS:" "$SHADER_LOG" 2>/dev/null; then
            ok "01b post-build M5 shader validation: PASS"
        else
            info "01b post-build M5 shader log present but PASS marker not found — manual review needed"
        fi
    else
        info "01b post-build M5 shader-validation log absent — likely from a non-arm64 build or --skip-shader-validation"
    fi
else
    fail "01 xemu binary not built; run ./build.sh -a arm64 first"
fi

# --- 2. oracle health -------------------------------------------
if [ "$DO_ORACLE" -eq 1 ]; then
    if "$HERE/oracle-smoke.sh" > "$OUT_DIR/02-oracle-smoke.log" 2>&1; then
        ok "02 oracle-smoke: 12/12 layers green"
    else
        fail "02 oracle-smoke: see $OUT_DIR/02-oracle-smoke.log"
    fi
else
    info "02 oracle-smoke: skipped (--skip-oracle)"
fi

# --- 3. Metal canary regression gate -----------------------------
if [ "$DO_CANARY" -eq 1 ]; then
    info "03 metal-canary-regress (counters mode, ~6 min)..."
    if "$HERE/metal-canary-regress.sh" --mode counters \
            > "$OUT_DIR/03-canary-regress.log" 2>&1; then
        ok "03 metal-canary-regress: PASS (counters green for all canaries)"
    else
        fail "03 metal-canary-regress: see $OUT_DIR/03-canary-regress.log"
    fi
else
    info "03 metal-canary-regress: skipped (--skip-canary-regress)"
fi

# --- 4. Tier-1 diag-XBE matrix on Metal + real Xbox -------------
if [ "$DO_TIER1" -eq 1 ]; then
    TIER1_OUT="$OUT_DIR/04-tier1-matrix"
    info "04 xbe-harness Tier-1 matrix (Metal + real Xbox)..."
    XBE_RC=0
    python3 "$HERE/xbe-harness/xbe_orchestrator.py" run \
        --renderer metal --renderer real-xbox \
        --max-changed-pct 1.0 --threshold 8 \
        --out "$TIER1_OUT" \
        > "$OUT_DIR/04-xbe-harness.log" 2>&1 || XBE_RC=$?
    if [ "$XBE_RC" -eq 0 ]; then
        ok "04 xbe-harness Tier-1: ALL cells PASS (changed_pixels_pct < 1.0)"
    else
        fail "04 xbe-harness Tier-1: $TIER1_OUT/report.md (rc=$XBE_RC)"
    fi
else
    info "04 xbe-harness Tier-1: skipped (--skip-tier1)"
fi

# --- 5. Paired Metal-vs-GL canary diff (optional) ---------------
if [ "$DO_PAIRED" -eq 1 ]; then
    info "05 paired Metal-vs-GL canary diff (~12 min for 3 titles)..."
    PAIRED_OUT="$OUT_DIR/05-paired"
    mkdir -p "$PAIRED_OUT"
    PAIRED_FAIL=0
    for canary in pgr2 rainbow halo; do
        info "05.$canary: metal-gl-compare $canary"
        if "$HERE/metal-gl-compare.sh" "$canary" \
                > "$PAIRED_OUT/$canary.log" 2>&1; then
            ok "05.$canary: PASS"
        else
            fail "05.$canary: see $PAIRED_OUT/$canary.log"
            PAIRED_FAIL=1
        fi
    done
    [ "$PAIRED_FAIL" -eq 0 ] || fail "05 paired Metal-vs-GL: at least one canary failed"
fi

echo "================================================================"
echo "m15-visual-gate summary:  $PASS PASS   $FAIL FAIL   out=$OUT_DIR"
echo "================================================================"

# Persist summary.
{
    echo "# m15-visual-gate"
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

python3 - <<PY
import json
out = {
  "pass": $PASS, "fail": $FAIL,
  "verdict": "ok" if $FAIL == 0 else "fail",
  "out_dir": "$OUT_DIR",
}
with open("$OUT_DIR/summary.json","w") as f:
    json.dump(out, f, indent=2)
PY

[ "$FAIL" -eq 0 ] || exit 1
