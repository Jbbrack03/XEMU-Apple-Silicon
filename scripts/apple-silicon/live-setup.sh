#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREP_DIR="${ROOT_DIR}/benchmark-runs/profile-prep"
PREP_HDD="${PREP_DIR}/xbox_hdd.qcow2"
SOURCE_HDD="/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/hdd/xbox_hdd.qcow2"

usage() {
    cat <<EOF
usage: $0 crimson|rainbow|pgr2 [duration-seconds]

Launches a game with live physical-controller input and no input recording,
using a persistent copied HDD image for profile/setup work.
EOF
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

mkdir -p "$PREP_DIR"
if [[ ! -e "$PREP_HDD" ]]; then
    cp -c "$SOURCE_HDD" "$PREP_HDD" 2>/dev/null || cp "$SOURCE_HDD" "$PREP_HDD"
fi

XEMU_BENCH_LIVE_INPUT=1 \
XEMU_BENCH_HDD_IN_PLACE=1 \
XEMU_BENCH_HDD_SOURCE="$PREP_HDD" \
"${ROOT_DIR}/scripts/apple-silicon/run-benchmark.sh" \
  "$1" "${ROOT_DIR}/scripts/apple-silicon/input-scripts/noop.csv" "${2:-600}"

echo "Prepared HDD: $PREP_HDD"
