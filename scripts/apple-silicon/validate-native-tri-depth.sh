#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SUMMARY_SCRIPT="${ROOT_DIR}/scripts/apple-silicon/extract-perf-summary.sh"

usage() {
    cat <<EOF
usage: $0 [RUN_DIR|xemu.log]
       $0 --run [duration-seconds]

Validates the flat-tri-depth XBE counter split for XEMU_NATIVE_TRI_DEPTH=1.
EOF
}

if [[ $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

if [[ $# -eq 0 || "${1:-}" == "--run" ]]; then
    DURATION="${2:-20}"
    if [[ ! "$DURATION" =~ ^[1-9][0-9]*$ ]]; then
        echo "duration must be a positive integer" >&2
        exit 2
    fi

    launcher_log="$(mktemp "${TMPDIR:-/tmp}/native-tri-depth-validate.XXXXXX")"
    trap 'rm -f "$launcher_log"' EXIT

    XEMU_NATIVE_TRI_DEPTH=1 \
    XEMU_DIAG_NATIVE_TRI_DEPTH=0 \
    XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=0 \
    XEMU_DIAG_SKIP_TRI_GEOM=0 \
    XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=0 \
    XEMU_BENCH_SCREENSHOT_BACKEND=none \
    "${ROOT_DIR}/scripts/apple-silicon/run-benchmark.sh" \
        flat-tri-depth \
        "${ROOT_DIR}/scripts/apple-silicon/input-scripts/noop.csv" \
        "$DURATION" 2>&1 | tee "$launcher_log"

    INPUT="$(awk -F': ' '/^Run directory: / { print $2 }' "$launcher_log" |
        tail -n 1)"
    if [[ -z "$INPUT" || ! -d "$INPUT" ]]; then
        echo "could not determine run directory from launcher output" >&2
        exit 1
    fi
else
    INPUT="$1"
fi

summary_file=""
if [[ -d "$INPUT" ]]; then
    summary_file="${INPUT%/}/native-tri-depth-validation-summary.txt"
    "$SUMMARY_SCRIPT" "$INPUT" > "$summary_file"
else
    summary_file="$(mktemp "${TMPDIR:-/tmp}/native-tri-depth-summary.XXXXXX")"
    trap 'rm -f "$summary_file"' EXIT
    "$SUMMARY_SCRIPT" "$INPUT" > "$summary_file"
fi

value_for() {
    awk -F= -v key="$1" '$1 == key { print $2 }' "$summary_file" | tail -n 1
}

int_value_for() {
    local value
    value="$(value_for "$1")"
    printf '%d' "${value:-0}"
}

failures=0

require_gt_zero() {
    local key="$1"
    local value
    value="$(int_value_for "$key")"
    if [[ "$value" -le 0 ]]; then
        echo "FAIL $key expected > 0, got $value"
        failures=$((failures + 1))
    else
        echo "PASS $key=$value"
    fi
}

require_eq() {
    local left_key="$1"
    local right_key="$2"
    local left
    local right
    left="$(int_value_for "$left_key")"
    right="$(int_value_for "$right_key")"
    if [[ "$left" -ne "$right" ]]; then
        echo "FAIL $left_key=$left expected to equal $right_key=$right"
        failures=$((failures + 1))
    else
        echo "PASS $left_key=$left matches $right_key"
    fi
}

echo "Native triangle-depth validation summary: $summary_file"

require_gt_zero final_intervals
require_gt_zero NATIVE_TRI_DEPTH_DRAW
require_gt_zero NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST
require_gt_zero NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST
require_gt_zero NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST
require_gt_zero NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST
require_eq GEOM_SHADER_DRAW_TRI NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST

if [[ "$failures" -ne 0 ]]; then
    echo "Native triangle-depth validation failed with $failures failure(s)."
    exit 1
fi

echo "Native triangle-depth validation passed."
