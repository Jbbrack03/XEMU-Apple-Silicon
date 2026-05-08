#!/usr/bin/env bash
# metal-tools-readiness.sh — quick/full readiness gate for Metal feedback tools.

set -u
set -o pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MODE="quick"
OUT_DIR="$ROOT/benchmark-runs/tools-readiness-$(date -u +%Y%m%dT%H%M%SZ)"

while [ $# -gt 0 ]; do
    case "$1" in
        --quick) MODE="quick"; shift;;
        --full) MODE="full"; shift;;
        --out) OUT_DIR="$2"; shift 2;;
        -h|--help)
            sed -n '1,80p' "$0" | sed 's/^# \{0,1\}//'
            exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

mkdir -p "$OUT_DIR"
PASS=0
FAIL=0
WARN=0
LINES=()

ok() { PASS=$((PASS+1)); LINES+=("PASS  $1"); echo "PASS  $1"; }
fail() { FAIL=$((FAIL+1)); LINES+=("FAIL  $1"); echo "FAIL  $1" >&2; }
warn() { WARN=$((WARN+1)); LINES+=("WARN  $1"); echo "WARN  $1"; }

check_file() {
    if [ -e "$1" ]; then ok "$2: $1"; else fail "$2 missing: $1"; fi
}

check_exec() {
    if [ -x "$1" ]; then ok "$2 executable: $1"; else fail "$2 not executable: $1"; fi
}

run_check() {
    local name="$1"
    shift
    local log="$OUT_DIR/${name// /-}.log"
    if "$@" > "$log" 2>&1; then
        ok "$name"
    else
        fail "$name (see $log)"
    fi
}

echo "================================================================"
echo "metal-tools-readiness — mode=$MODE out=$OUT_DIR"
echo "================================================================"

check_file "$ROOT/dist/xemu.app/Contents/MacOS/xemu" "xemu app binary"
check_file "$ROOT/tools/xemu-capture/Package.swift" "Swift capture package"
check_exec "$HERE/metal-feedback-dashboard.py" "dashboard"
check_exec "$HERE/metal-capture-manifest.py" "Metal capture manifest"
check_exec "$HERE/xbox-kernel-symbol-dump.py" "kernel symbol dump"
check_exec "$HERE/xbox-kernel-export-annotate.py" "kernel export annotator"
check_exec "$HERE/controller-readback-validate.py" "controller readback validator"
check_exec "$HERE/retail-oracle-smoke.py" "retail oracle smoke gate"
check_file "$HERE/xbe-tests/controller-readback/main.c" "controller-readback XBE source"
check_file "$HERE/xbe-tests/controller-readback/manifest.json" "controller-readback manifest"

if xcrun --find mcpbridge > "$OUT_DIR/xcode-mcpbridge.txt" 2>&1; then
    ok "Xcode mcpbridge installed"
else
    warn "Xcode mcpbridge not found by xcrun"
fi

if grep -q '^\[mcp_servers\.xcode\]' "$HOME/.codex/config.toml" 2>/dev/null; then
    if awk '/^\[mcp_servers\.xcode\]/{inblk=1; next} /^\[/{inblk=0} inblk && /enabled[[:space:]]*=[[:space:]]*false/{found=1} END{exit found?0:1}' "$HOME/.codex/config.toml"; then
        ok "Xcode MCP configured and globally disabled by policy"
    else
        warn "Xcode MCP config exists but is not clearly enabled=false"
    fi
else
    warn "Xcode MCP config block not found"
fi

run_check "python py_compile tooling" python3 -m py_compile \
    "$HERE/metal-feedback-dashboard.py" \
    "$HERE/metal-capture-manifest.py" \
    "$HERE/xbox-kernel-symbol-dump.py" \
    "$HERE/xbox-kernel-export-annotate.py" \
    "$HERE/controller-readback-validate.py" \
    "$HERE/retail-oracle-smoke.py" \
    "$HERE/oracle-client.py" \
    "$HERE/oracle-orchestrator.py"

run_check "bash syntax benchmark gates" bash -n \
    "$HERE/run-benchmark.sh" \
    "$HERE/m15-visual-gate.sh" \
    "$HERE/oracle-validate.sh" \
    "$HERE/metal-tools-readiness.sh"

run_check "dashboard render" "$HERE/metal-feedback-dashboard.py" --out "$OUT_DIR/dashboard.md"

if [ -e "$HERE/xbe-tests/controller-readback/bin/default.xbe" ]; then
    ok "controller-readback XBE built"
else
    warn "controller-readback XBE not built yet; run make in xbe-tests/controller-readback"
fi

if find "$ROOT/benchmark-runs" -maxdepth 2 -name metal-capture-manifest.json | grep -q . 2>/dev/null; then
    ok "at least one Metal capture manifest exists"
else
    warn "no prior Metal capture manifest found; next --metal-capture run will create one"
fi

KERNEL_SYMBOL_DIR="$ROOT/../xbox-oracle-backup/2026-05-06/kernel-symbols"
if [ -d "$KERNEL_SYMBOL_DIR" ] && find "$KERNEL_SYMBOL_DIR" -maxdepth 1 -name 'xboxkrnl-*-exports.json' | grep -q . 2>/dev/null; then
    ok "kernel export dump artifact exists"
else
    warn "kernel export dump artifact not found; run xbox-kernel-symbol-dump.py when oracle is online"
fi

if [ -d "$KERNEL_SYMBOL_DIR" ] && find "$KERNEL_SYMBOL_DIR" -maxdepth 1 -name 'xboxkrnl-*-exports-annotated.json' | grep -q . 2>/dev/null; then
    ok "annotated kernel export artifact exists"
else
    warn "annotated kernel export artifact not found; run xbox-kernel-export-annotate.py after dumping symbols"
fi

if find "$ROOT/benchmark-runs" -maxdepth 2 -path '*/retail-oracle-smoke-*' -name verdict.json 2>/dev/null | xargs grep -l '"verdict": "ok"' 2>/dev/null | grep -q .; then
    ok "retail-game real-Xbox smoke proof exists"
else
    warn "no successful retail-game real-Xbox smoke proof exists; Tier-2 input/exit remains a production blocker"
fi

if [ "$MODE" = "full" ]; then
    run_check "oracle validate full" "$HERE/oracle-validate.sh" --out "$OUT_DIR/oracle-validate"
    run_check "m15 visual gate paired full" "$HERE/m15-visual-gate.sh" --paired --out "$OUT_DIR/m15-visual-gate"
fi

{
    echo "# metal-tools-readiness"
    echo
    date -u +"Generated: %Y-%m-%dT%H:%M:%SZ"
    echo
    echo "Mode: $MODE"
    echo "Pass: $PASS"
    echo "Warn: $WARN"
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
payload = {
    "schema": "metal-tools-readiness-v1",
    "mode": "$MODE",
    "pass": $PASS,
    "warn": $WARN,
    "fail": $FAIL,
    "verdict": "ok" if $FAIL == 0 else "fail",
    "out_dir": "$OUT_DIR",
}
with open("$OUT_DIR/summary.json", "w", encoding="utf-8") as f:
    json.dump(payload, f, indent=2, sort_keys=True)
PY

echo "================================================================"
echo "metal-tools-readiness summary: $PASS PASS  $WARN WARN  $FAIL FAIL"
echo "report: $OUT_DIR/report.md"
echo "================================================================"

[ "$FAIL" -eq 0 ] || exit 1
