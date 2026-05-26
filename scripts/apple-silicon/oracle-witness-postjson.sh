#!/usr/bin/env bash
#
# oracle-witness-postjson.sh — Bounded helper for the cycle-45E-proven
# real-Xbox witness-only post-JSON validation flow.
#
# One invocation performs the exact 45E-proven sequence:
#   1. Optional reachability / smoke preflight
#   2. JSON-support preflight (help --json)
#   3. unsafe.enable + eeprom.scratch.reset
#   4. run-diag with --post-json-command readbacks
#   5. Validate structured artifacts
#
# Usage:
#   scripts/apple-silicon/oracle-witness-postjson.sh [OPTIONS]
#
# Options:
#   --xbe PATH           Xbox-side XBE path (default: E:\Apps\witness-only\default.xbe)
#   --ftp-collect PATH   Xbox-side FTP collect dir (default: /E/Apps/witness-only)
#   --out DIR            Host-side output directory (required)
#   --no-preflight       Skip the optional reachability/smoke preflight
#   --no-eeprom-reset    Skip the EEPROM scratch reset (use if baseline already set)
#   --dry-run            Print the full command sequence and expected outputs
#                        without executing commands or validating files
#   --help               Show this help text
#
# Expected output artifacts (written under $OUT/post-json/):
#   eeprom.scratch.read.json
#   witness.scan-self.json
#   witness.scan.json
#   verdict.json (written by oracle-orchestrator.py)
#
# Expected proven values (cycle-45E witness-only recipe):
#   eeprom.scratch.read.json → byte 188 (0xBC)
#   witness.scan-self.json   → count = 0
#   witness.scan.json        → count = 1
#
# Failure semantics:
#   - Missing help --json support → deployed oracle-agent drift, NOT hardware truth
#   - post-json-readback-failed → transport/protocol/deployment drift, NOT hardware truth
#   - Always refresh/redeploy the Xbox-side oracle-agent before re-attempting
#
# Set by environment (falls back to project defaults):
#   ORACLE_HOST       Xbox LAN IP (default: 192.168.0.200)
#   ORACLE_PORT       Agent TCP port (default: 9001)
#   ORACLE_FTP_USER   FTP user (default: xbox)
#   ORACLE_FTP_PASS   FTP password (default: xbox)
#
# Written for cycle 45F — bounded helper packaging slice.
# Codex review required before closure if this script is modified beyond
# the initial landing (non-trivial shell logic).

set -euo pipefail

# ── defaults ──────────────────────────────────────────────────────────
XBE_PATH="E:\\Apps\\witness-only\\default.xbe"
FTP_COLLECT="/E/Apps/witness-only"
OUT_DIR=""
DO_PREFLIGHT=1
DO_EEPROM_RESET=1
DRY_RUN=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT="${SCRIPT_DIR}/oracle-client.py"
ORCHESTRATOR="${SCRIPT_DIR}/oracle-orchestrator.py"
SMOKE="${SCRIPT_DIR}/oracle-smoke.sh"

REQUIRED_CMDS=(
    "eeprom.scratch.read"
    "eeprom.scratch.reset"
    "witness.scan"
    "witness.scan-self"
)

# ── helpers ───────────────────────────────────────────────────────────

usage() {
    head -50 "${BASH_SOURCE[0]}" | grep '^#' | sed 's/^# \?//'
    exit 0
}

log() {
    echo "[oracle-witness-postjson] $*"
}

log_ok() {
    log "OK: $*"
}

log_fail() {
    log "FAIL: $*"
}

run_cmd() {
    local desc="$1"; shift
    if [[ "$DRY_RUN" -eq 1 ]]; then
        log "DRY-RUN: $desc"
        log "DRY-RUN:   $*"
        return 0
    fi
    log "$desc"
    if ! "$@"; then
        log_fail "command failed: $*"
        return 1
    fi
    log_ok "$desc"
}

# ── argument parsing ──────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --xbe)           XBE_PATH="$2";           shift 2 ;;
        --ftp-collect)   FTP_COLLECT="$2";        shift 2 ;;
        --out)           OUT_DIR="$2";            shift 2 ;;
        --no-preflight)  DO_PREFLIGHT=0;          shift   ;;
        --no-eeprom-reset) DO_EEPROM_RESET=0;     shift   ;;
        --dry-run)       DRY_RUN=1;               shift   ;;
        --help|-h)       usage                     ;;
        *)               log_fail "unknown option: $1"; usage ;;
    esac
done

if [[ -z "$OUT_DIR" ]]; then
    log_fail "--out DIR is required"
    usage
fi

# ── preflight (optional) ─────────────────────────────────────────────

if [[ "$DO_PREFLIGHT" -eq 1 ]]; then
    log "Running optional reachability/smoke preflight..."
    if [[ "$DRY_RUN" -eq 1 ]]; then
        log "DRY-RUN: $SMOKE"
    else
        if [[ -x "$SMOKE" ]]; then
            if ! "$SMOKE" 2>&1 | tail -5; then
                log "Smoke preflight returned non-zero; continuing (preflight is optional)"
            fi
        else
            log "oracle-smoke.sh not found or not executable at $SMOKE; skipping"
        fi
    fi
fi

# ── step 1: JSON-support preflight ───────────────────────────────────

log "Step 1: Verifying live JSON support via 'help --json'..."

if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: python3 \"$CLIENT\" raw 'help --json'"
else
    HELP_OUTPUT=""
    if ! HELP_OUTPUT=$(python3 "$CLIENT" raw 'help --json' 2>&1); then
        log_fail "'help --json' failed — this is deployed oracle-agent drift, not hardware truth."
        log "Action: refresh/redeploy the Xbox-side oracle-agent before re-attempting."
        exit 1
    fi
    log "help --json response received."

    # Check that all required commands are advertised
    for req_cmd in "${REQUIRED_CMDS[@]}"; do
        if ! echo "$HELP_OUTPUT" | grep -qi "$req_cmd"; then
            log_fail "'help --json' does not advertise required command: $req_cmd"
            log "This is deployed oracle-agent drift, not hardware truth."
            log "Action: refresh/redeploy the Xbox-side oracle-agent before re-attempting."
            exit 1
        fi
    done
    log_ok "All required JSON commands advertised: ${REQUIRED_CMDS[*]}"
fi

# ── step 2: arm unsafe + reset EEPROM scratch ─────────────────────────

if [[ "$DO_EEPROM_RESET" -eq 1 ]]; then
    log "Step 2: Arming unsafe writes and clearing EEPROM scratch baseline..."

    if [[ "$DRY_RUN" -eq 1 ]]; then
        log "DRY-RUN: python3 \"$CLIENT\" raw 'unsafe.enable'"
        log "DRY-RUN: python3 \"$CLIENT\" raw 'eeprom.scratch.reset --json'"
    else
        run_cmd "Enabling unsafe writes" \
            python3 "$CLIENT" raw 'unsafe.enable'

        run_cmd "Resetting EEPROM scratch baseline" \
            python3 "$CLIENT" raw 'eeprom.scratch.reset --json'

        log "EEPROM scratch reset complete. Future operators: the pre-run EEPROM"
        log "value may differ from the post-run value because this helper resets"
        log "the baseline to 0x00 before chainloading the diagnostic XBE."
    fi
else
    log "Step 2: Skipping EEPROM scratch reset (--no-eeprom-reset)."
fi

# ── step 3: run-diag with post-JSON readbacks ─────────────────────────

log "Step 3: Running bounded witness-only validation with post-JSON readbacks..."

POST_JSON_ARGS=()
for cmd in "eeprom.scratch.read" "witness.scan-self" "witness.scan"; do
    POST_JSON_ARGS+=(--post-json-command "$cmd")
done

if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: python3 \"$ORCHESTRATOR\" run-diag"
    log "DRY-RUN:   --xbe '$XBE_PATH'"
    log "DRY-RUN:   --ftp-collect '$FTP_COLLECT'"
    log "DRY-RUN:   --out '$OUT_DIR'"
    for arg in "${POST_JSON_ARGS[@]}"; do
        log "DRY-RUN:   $arg"
    done
else
    if ! python3 "$ORCHESTRATOR" run-diag \
        --xbe "$XBE_PATH" \
        --ftp-collect "$FTP_COLLECT" \
        --out "$OUT_DIR" \
        "${POST_JSON_ARGS[@]}"; then
        log_fail "run-diag returned non-zero."
        log "If the failure reason is 'post-json-readback-failed', treat it as"
        log "transport/protocol or deployment drift first — not hardware truth."
        log "Action: refresh/redeploy the Xbox-side oracle-agent and rerun."
        exit 1
    fi
    log_ok "run-diag completed successfully."
fi

# ── step 4: validate structured artifacts ─────────────────────────────

log "Step 4: Validating structured artifacts..."

POST_JSON_DIR="$OUT_DIR/post-json"
VERDICT_FILE="$OUT_DIR/verdict.json"

if [[ "$DRY_RUN" -eq 1 ]]; then
    log "DRY-RUN: no files were created; skipping artifact validation."
    log "DRY-RUN: expected output directory: $OUT_DIR"
    log "DRY-RUN: expected verdict artifact: $VERDICT_FILE"
    log "DRY-RUN: expected post-JSON artifacts under $POST_JSON_DIR/"
    log "DRY-RUN:   - eeprom.scratch.read.json"
    log "DRY-RUN:   - witness.scan-self.json"
    log "DRY-RUN:   - witness.scan.json"
    log "DRY-RUN: canonical proven values for the 45E witness-only recipe:"
    log "DRY-RUN:   eeprom.scratch.read.json -> byte 188 (0xBC)"
    log "DRY-RUN:   witness.scan-self.json   -> count 0"
    log "DRY-RUN:   witness.scan.json        -> count 1"
    log "DRY-RUN: done."
    exit 0
fi

# Check verdict.json
if [[ -f "$VERDICT_FILE" ]]; then
    VERDICT_STATUS=$(python3 -c "
import json, sys
with open('$VERDICT_FILE') as f:
    v = json.load(f)
print(v.get('status', 'unknown'))
" 2>/dev/null || echo "parse-error")
    if [[ "$VERDICT_STATUS" == "ok" ]]; then
        log_ok "verdict.json: status = ok"
    else
        log_fail "verdict.json: status = $VERDICT_STATUS (expected ok)"
        log "The run may have partial success; inspect $OUT_DIR for details."
    fi
else
    log_fail "verdict.json not found at $VERDICT_FILE"
fi

# Check post-JSON artifacts exist
for artifact in "eeprom.scratch.read.json" "witness.scan-self.json" "witness.scan.json"; do
    if [[ -f "$POST_JSON_DIR/$artifact" ]]; then
        log_ok "Artifact present: $artifact"
    else
        log_fail "Artifact missing: $artifact (expected at $POST_JSON_DIR/$artifact)"
    fi
done

# Extract and display proven expected values from the artifacts
if [[ -d "$POST_JSON_DIR" ]]; then
    log ""
    log "Proven artifact values (cycle-45E witness-only recipe):"

    if [[ -f "$POST_JSON_DIR/eeprom.scratch.read.json" ]]; then
        EE_BYTE=$(python3 -c "
import json
with open('$POST_JSON_DIR/eeprom.scratch.read.json') as f:
    v = json.load(f)
resp = v.get('response', {})
# The response may contain the byte value in various shapes;
# try common paths.
val = resp.get('byte') or resp.get('value') or resp.get('data', '')
if isinstance(val, int):
    print(f'0x{val:02X} ({val})')
elif isinstance(val, str) and len(val) == 2:
    print(f'0x{val.upper()} ({int(val, 16)})')
else:
    print(f'not directly parseable: {val!r}')
" 2>/dev/null || echo "parse-error")
        log "  eeprom.scratch.read.json → byte = $EE_BYTE (expected 0xBC / 188)"
    fi

    if [[ -f "$POST_JSON_DIR/witness.scan-self.json" ]]; then
        SCAN_SELF_COUNT=$(python3 -c "
import json
with open('$POST_JSON_DIR/witness.scan-self.json') as f:
    v = json.load(f)
resp = v.get('response', {})
print(resp.get('count', 'not found'))
" 2>/dev/null || echo "parse-error")
        log "  witness.scan-self.json   → count = $SCAN_SELF_COUNT (expected 0)"
    fi

    if [[ -f "$POST_JSON_DIR/witness.scan.json" ]]; then
        SCAN_COUNT=$(python3 -c "
import json
with open('$POST_JSON_DIR/witness.scan.json') as f:
    v = json.load(f)
resp = v.get('response', {})
print(resp.get('count', 'not found'))
" 2>/dev/null || echo "parse-error")
        log "  witness.scan.json        → count = $SCAN_COUNT (expected 1)"
    fi
fi

log ""
log "Output directory: $OUT_DIR"
log "Post-JSON artifacts: $POST_JSON_DIR/"
log "  - eeprom.scratch.read.json"
log "  - witness.scan-self.json"
log "  - witness.scan.json"
log ""
log "Done. Full invocation recorded under $OUT_DIR."
