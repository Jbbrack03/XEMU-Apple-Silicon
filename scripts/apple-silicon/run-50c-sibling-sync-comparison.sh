#!/bin/bash
# 50C: Runtime sibling-sync merge-scene discrimination
# Compare baseline vs color-only vs depth-only vs full-sync on the exact
# PGR2 snapshot path that is known to produce depth sibling merges.

set -euo pipefail

XEMU_DIR="/Users/jbbrack03/XEMU_MacOS/xemu-fork"
INPUT_SCRIPT="scripts/apple-silicon/input-scripts/noop.csv"
HDD_SOURCE="benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2"
LOADVM_TAG="pgr2_gameplay_b4"
LOADVM_AT="2"
DURATION="${1:-30}"
STAMP="$(date +%Y%m%d-%H%M%S)"
ROOT_RUN_DIR="benchmark-runs/${STAMP}-50c-pgr2-sibling-sync"

export XEMU_RENDERER=METAL
export XEMU_BENCH_HDD_SOURCE="$HDD_SOURCE"
export XEMU_BENCH_LOADVM_TAG="$LOADVM_TAG"
export XEMU_BENCH_LOADVM_AT="$LOADVM_AT"
export XEMU_SNAPSHOT_NO_THUMBNAIL=1
export XEMU_PERF_LOG=1
export XEMU_PERF_LOG_INTERVAL_MS=1000

cd "$XEMU_DIR"
mkdir -p "$ROOT_RUN_DIR"

cat <<EOF
==============================================
50C: Runtime sibling-sync merge-scene discrimination
==============================================
Repo: $XEMU_DIR
Branch: $(git branch --show-current)
HEAD: $(git rev-parse HEAD)
Run root: $ROOT_RUN_DIR
Title: Project Gotham Racing 2 (PGR2)
Snapshot: $LOADVM_TAG
Load-at: ${LOADVM_AT}s
Duration: ${DURATION}s per run
Known merge-producing RT: depth vram=0x038e0000
Known alternation: 1280x480 <-> 1278x442 guest clip sizes
==============================================
EOF

run_case() {
  local name="$1"
  shift
  local rundir="$ROOT_RUN_DIR/$name"
  mkdir -p "$rundir"
  echo "=== $name ==="
  XEMU_BENCH_SAVEVM_TAG="$name" \
    ./scripts/apple-silicon/run-benchmark.sh "$@" \
    pgr2 "$INPUT_SCRIPT" "$DURATION" 2>&1 | tee "$rundir/xemu-output.log"

  if [ -f "$rundir/xemu.log" ]; then
    echo "Counters:"
    grep "METAL_SIBLING_SYNC" "$rundir/xemu.log" | tail -5 || true
    echo "Color merges (first 5):"
    grep "xemu.metal.sibling_sync: color merge" "$rundir/xemu.log" | head -5 || echo "  (none)"
    echo "Depth merges (first 10):"
    grep "xemu.metal.sibling_sync: depth merge" "$rundir/xemu.log" | head -10 || echo "  (none)"
  else
    echo "WARNING: $rundir/xemu.log not found"
  fi
  echo
}

run_case run1-baseline
run_case run2-color-only --metal-sibling-sync
run_case run3-depth-only --metal-sibling-sync-depth-only \
  --metal-screenshot "$ROOT_RUN_DIR/run3-depth-only/depth-only.png"
run_case run4-full-sync --metal-sibling-sync --metal-sibling-sync-depth \
  --metal-screenshot "$ROOT_RUN_DIR/run4-full-sync/full-sync.png"

cat <<EOF
==============================================
50C interpretation guide
==============================================
Clip-rect isolation:
- strengthened if run3 depth-only reproduces the same regression locations as run4 full-sync
  while depth merges show the 1280x480 / 1278x442 alternation at vram=0x038e0000
- weakened if run3 stays visually clean despite depth merges firing
- unresolved if run3 differs materially from run4

Depth-blit semantics:
- strengthened if run3 shows z-test-style failures absent from run2
- weakened if run3 stays visually clean
- unresolved if run3 artifacts do not match depth-failure patterns

MSAA companion handling:
- strengthened only if artifacts correlate with MSAA-enabled evidence in logs
- weakened if run3 stays clean regardless of MSAA state
- otherwise unresolved

Broader readiness remains unproven:
- no root-cause resolution claim
- no renderer-correctness claim
- no serious-user-testing readiness claim
- only one title/state is exercised by this recipe
==============================================
EOF

echo "$ROOT_RUN_DIR"