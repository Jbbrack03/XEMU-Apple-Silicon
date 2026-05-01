#!/usr/bin/env bash
# Capture an Apple `sample` profile of xemu while a benchmark is running.
#
# Usage:
#   sample-profile.sh GAME INPUT_CSV BENCH_SECONDS [SAMPLE_DURATION] [WARMUP] [LABEL]
#
# Runs `scripts/apple-silicon/run-benchmark.sh GAME INPUT_CSV BENCH_SECONDS` in
# the background, waits WARMUP seconds (default 10) for xemu to settle, then
# calls Apple's `sample` against the live xemu pid for SAMPLE_DURATION
# seconds (default 25). The textual sample output is written to
#   <run-dir>/sample-<LABEL>.txt
# and a sibling top-bucket summary to
#   <run-dir>/sample-<LABEL>-summary.txt
#
# All XEMU_* env vars (flags, snapshot tag, etc.) are inherited by the
# benchmark, so the caller selects which configuration is being profiled.
#
# This script is autonomous: no user input is required.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
    cat <<EOF
usage: $0 GAME INPUT_CSV BENCH_SECONDS [SAMPLE_DURATION] [WARMUP] [LABEL]

  GAME             crimson|rainbow|pgr2|flat-tri-depth
  INPUT_CSV        input script path (or noop.csv for snapshot replays)
  BENCH_SECONDS    total benchmark duration; must be >= WARMUP+SAMPLE_DURATION+5
  SAMPLE_DURATION  optional, default 25
  WARMUP           optional, default 10
  LABEL            optional, default 'profile'

Output written to <run-dir>/sample-<LABEL>.txt under benchmark-runs/.
EOF
}

if [[ $# -lt 3 || $# -gt 6 ]]; then usage >&2; exit 2; fi

GAME="$1"
INPUT_CSV="$2"
BENCH_SECONDS="$3"
SAMPLE_DURATION="${4:-25}"
WARMUP="${5:-10}"
LABEL="${6:-profile}"

if (( BENCH_SECONDS < WARMUP + SAMPLE_DURATION + 5 )); then
    echo "BENCH_SECONDS ($BENCH_SECONDS) must be >= WARMUP ($WARMUP) + SAMPLE_DURATION ($SAMPLE_DURATION) + 5" >&2
    exit 2
fi

# Run the benchmark in the background. Capture stdout to detect run dir.
BENCH_OUT="$(mktemp -t sample-profile-bench.XXXXXX)"
trap 'rm -f "$BENCH_OUT"' EXIT

(
    "${ROOT_DIR}/scripts/apple-silicon/run-benchmark.sh" \
        "$GAME" "$INPUT_CSV" "$BENCH_SECONDS" >"$BENCH_OUT" 2>&1
) &
BENCH_BGPID=$!

# Poll for run dir.
RUN_DIR=""
for i in $(seq 1 60); do
    if [[ -s "$BENCH_OUT" ]]; then
        RUN_DIR=$(awk '/^Run directory:/ {print $3; exit}' "$BENCH_OUT")
        if [[ -n "$RUN_DIR" && -d "$RUN_DIR" ]]; then
            break
        fi
    fi
    sleep 0.5
done

if [[ -z "$RUN_DIR" ]]; then
    echo "could not determine run directory" >&2
    cat "$BENCH_OUT" >&2
    wait "$BENCH_BGPID" || true
    exit 1
fi
echo "run_dir=$RUN_DIR"

# Poll for xemu.pid.
XEMU_PID=""
for i in $(seq 1 60); do
    if [[ -f "$RUN_DIR/xemu.pid" ]]; then
        XEMU_PID=$(cat "$RUN_DIR/xemu.pid" 2>/dev/null || true)
        if [[ -n "$XEMU_PID" ]] && kill -0 "$XEMU_PID" 2>/dev/null; then
            break
        fi
        XEMU_PID=""
    fi
    sleep 0.5
done

if [[ -z "$XEMU_PID" ]]; then
    echo "could not find live xemu pid" >&2
    wait "$BENCH_BGPID" || true
    exit 1
fi
echo "xemu_pid=$XEMU_PID"

# Warmup.
echo "warmup=${WARMUP}s"
sleep "$WARMUP"

# Verify still alive.
if ! kill -0 "$XEMU_PID" 2>/dev/null; then
    echo "xemu died during warmup" >&2
    cat "$BENCH_OUT" >&2
    exit 1
fi

OUT_FILE="$RUN_DIR/sample-${LABEL}.txt"
echo "sampling pid=$XEMU_PID for ${SAMPLE_DURATION}s -> $OUT_FILE"
sample "$XEMU_PID" "$SAMPLE_DURATION" -mayDie -file "$OUT_FILE" || true

# Wait for benchmark to finish so logs flush.
wait "$BENCH_BGPID" || true

if [[ ! -s "$OUT_FILE" ]]; then
    echo "sample produced no output" >&2
    exit 1
fi

# Light-weight bucket summary: top frames per thread (no semantic
# bucketing — that is left to the human reading the file). Useful as a
# quick overview that gets committed alongside the full sample text.
SUMMARY="$RUN_DIR/sample-${LABEL}-summary.txt"
{
    echo "=== sample-profile summary ==="
    echo "run_dir=$RUN_DIR"
    echo "xemu_pid=$XEMU_PID"
    echo "warmup_s=$WARMUP"
    echo "sample_duration_s=$SAMPLE_DURATION"
    echo "label=$LABEL"
    echo
    echo "=== threads with > 100 samples ==="
    awk '
      /^    [0-9]+ Thread_/ {
        line = $0
        gsub(/^[ ]+/, "", line)
        n = split(line, a, " ")
        samples = a[1] + 0
        if (samples > 100) {
          name = a[2]
          for (i = 3; i <= n; i++) name = name " " a[i]
          printf("%6d  %s\n", samples, name)
        }
      }
    ' "$OUT_FILE"
} > "$SUMMARY"

cat "$SUMMARY"

echo "=== full sample at $OUT_FILE ($(wc -l < "$OUT_FILE") lines) ==="
echo "summary at $SUMMARY"
