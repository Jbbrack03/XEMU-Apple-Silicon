#!/usr/bin/env bash
# scripts/apple-silicon/metal-shader-validation/run-validation.sh
#
# M5 shader-validation harness runner.
#
# Launches xemu with renderer = METAL forced via the XEMU_RENDERER env
# var, runs the in-process M5 shader-validation harness on the
# representative ShaderState fixtures (see hw/xbox/nv2a/pgraph/mtl/
# shader_validation.c), and exits the process immediately after the
# harness reports — no full machine boot required.
#
# Usage:
#   run-validation.sh
#
# Returns 0 if every fixture passed, 1 if any fixture failed, 2 on
# infrastructure failure (xemu binary missing, harness symbol missing,
# etc.).
#
# Env-vars:
#   XEMU_BIN  — override the xemu binary path
#               (default: $REPO_ROOT/dist/xemu.app/Contents/MacOS/xemu)
#
# Prints the captured harness output to stdout.

set -uo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd "$SCRIPT_DIR/../../.." &> /dev/null && pwd )"
XEMU_BIN="${XEMU_BIN:-$REPO_ROOT/dist/xemu.app/Contents/MacOS/xemu}"

if [[ ! -x "$XEMU_BIN" ]]; then
    echo "ERROR: xemu binary not found at $XEMU_BIN" >&2
    echo "       run \`./build.sh -a arm64\` first" >&2
    exit 2
fi

LOG_FILE=$(mktemp -t xemu_metal_validate.XXXXXX)
trap 'rm -f "$LOG_FILE"' EXIT

echo "[run-validation] launching $XEMU_BIN with"
echo "[run-validation]   XEMU_RENDERER=METAL"
echo "[run-validation]   XEMU_METAL_SHADER_VALIDATE=1"
echo "[run-validation]   XEMU_METAL_SHADER_VALIDATE_AND_EXIT=1"

# Run xemu and capture output. The validate-and-exit flag means the
# process exits cleanly (0 = pass, 1 = fail) after the harness.
XEMU_RENDERER=METAL \
XEMU_METAL_SHADER_VALIDATE=1 \
XEMU_METAL_SHADER_VALIDATE_AND_EXIT=1 \
"$XEMU_BIN" >"$LOG_FILE" 2>&1
XEMU_RC=$?

echo "===== [xemu-metal-validate] output ====="
grep "\[xemu-metal-validate\]" "$LOG_FILE" || true
echo "========================================"

# Extract the summary line for explicit reporting.
SUMMARY=$(grep "\[xemu-metal-validate\] summary:" "$LOG_FILE" || true)
if [[ -z "$SUMMARY" ]]; then
    echo "[run-validation] FAIL: harness summary line never appeared" >&2
    echo "[run-validation] xemu rc=$XEMU_RC; tail of log:" >&2
    tail -30 "$LOG_FILE" >&2
    exit 2
fi

if [[ $XEMU_RC -ne 0 ]]; then
    echo "[run-validation] FAIL: xemu exited rc=$XEMU_RC ($SUMMARY)" >&2
    exit 1
fi

echo "[run-validation] PASS: $SUMMARY"
exit 0
