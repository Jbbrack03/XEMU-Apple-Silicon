#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
    cat <<EOF
usage: $0 crimson|rainbow|pgr2 [duration-seconds] [output.csv]

Launches xemu with a physical controller bound to port 1 and records input to
the benchmark run directory, or to output.csv when provided.
EOF
}

if [[ $# -lt 1 || $# -gt 3 ]]; then
    usage >&2
    exit 2
fi

GAME="$1"
DURATION="${2:-240}"
OUTPUT="${3:-auto}"

XEMU_BENCH_RECORD_INPUT="$OUTPUT" \
"${ROOT_DIR}/scripts/apple-silicon/run-benchmark.sh" \
  "$GAME" "${ROOT_DIR}/scripts/apple-silicon/input-scripts/noop.csv" "$DURATION"
