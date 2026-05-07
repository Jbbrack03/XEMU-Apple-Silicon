#!/usr/bin/env bash
# oracle-stress.sh — exercise the oracle agent's RPC + diag-XBE
# chainload-and-back loop in a tight burst to surface the transient
# "agent listening but RPCs return empty payloads" degraded state
# observed once on 2026-05-07 evening.
#
# Working hypothesis (from handoff.md "Gap 5"): lwIP PCB pool
# exhaustion under repeated upload + chainload + relaunch cycles. We
# now do that loop tighter than the original Phase 2 testing
# (once per diag instead of once per session).
#
# Loop:
#   for i in 1..N:
#       oracle-smoke.sh --tier1 mirror,color-channel,depth-floor,controller-roundtrip
#                       (with optional --buttons / --lt etc.)
#       if smoke fails: report exact iteration that failed + capture artifacts
#
# Pass criteria:
#   ALL N iterations green → no degraded state detected; record this
#                            as the upper bound of "tested clean".
#   Any iteration's smoke failure → degraded state reproduced; the
#                                   per-iteration logs + the agent's
#                                   `info` payload at fault tell us
#                                   which RPC degraded first.
#
# Default N is 10 (covers ~30 minutes wallclock). Override with
# --iterations or `ORACLE_STRESS_ITER`. The smoke test takes ~3
# minutes per cycle on the project Xbox so 10 iterations surface
# any pool-exhaustion class within reasonable runtime budget.

set -u
set -o pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HOST="${ORACLE_HOST:-192.168.0.200}"
ITERATIONS="${ORACLE_STRESS_ITER:-10}"
OUT_DIR=""
SMOKE_ARGS=("--tier1" "mirror,color-channel,depth-floor,controller-roundtrip")

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="$2"; shift 2;;
        --iterations|-n) ITERATIONS="$2"; shift 2;;
        --out) OUT_DIR="$2"; shift 2;;
        --no-tier1) SMOKE_ARGS=(); shift;;
        --buttons|--lt|--rt|--lx|--ly|--rx|--ry)
            # Pass-through to oracle-smoke.sh
            SMOKE_ARGS+=("$1" "$2"); shift 2;;
        -h|--help)
            sed -n '1,/^# Default N/p' "$0" | sed 's/^# \{0,1\}//'
            echo
            echo "Usage:"
            echo "  $0 [--iterations N] [--host HOST] [--out DIR]"
            echo "     [--buttons 0xH] [--lt N] [--rt N] [--lx N] [--ly N] [--rx N] [--ry N]"
            echo "     [--no-tier1]"
            exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="$HERE/../../benchmark-runs/oracle-stress-$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$OUT_DIR"

PASS=0
FAIL_AT=""
FAIL_LOG=""

echo "================================================================"
echo "oracle-stress — start"
echo "  host       = $HOST"
echo "  iterations = $ITERATIONS"
echo "  out_dir    = $OUT_DIR"
echo "  smoke args = ${SMOKE_ARGS[*]}"
echo "================================================================"

for ((i = 1; i <= ITERATIONS; i++)); do
    iter_log="$OUT_DIR/iter-$(printf '%03d' "$i").log"
    echo "[$i/$ITERATIONS] running smoke (log: $iter_log)"
    if ORACLE_HOST="$HOST" "$HERE/oracle-smoke.sh" "${SMOKE_ARGS[@]}" \
            > "$iter_log" 2>&1; then
        PASS=$((PASS + 1))
    else
        rc=$?
        FAIL_AT="$i"
        FAIL_LOG="$iter_log"
        echo "[$i/$ITERATIONS] FAIL rc=$rc; capturing post-fail diagnostics..."

        # Best-effort: capture the agent's `info`, `controller.buffer-info`,
        # `nv2a.read PMC_BOOT_0`, and current TCP/9001 state for the
        # post-mortem. Failures here are non-fatal.
        diag_dir="$OUT_DIR/iter-$(printf '%03d' "$i")-diag"
        mkdir -p "$diag_dir"
        {
            echo "$ python3 oracle-client.py info"
            python3 "$HERE/oracle-client.py" --host "$HOST" info \
                2>&1 || echo "(info failed)"
            echo
            echo "$ python3 oracle-client.py raw 'controller.buffer-info'"
            python3 "$HERE/oracle-client.py" --host "$HOST" raw \
                "controller.buffer-info" 2>&1 || echo "(buffer-info failed)"
            echo
            echo "$ python3 oracle-client.py raw 'nv2a.read offset=0x0'"
            python3 "$HERE/oracle-client.py" --host "$HOST" raw \
                "nv2a.read offset=0x0" 2>&1 || echo "(nv2a.read failed)"
            echo
            echo "$ python3 oracle-client.py raw 'mem.read addr=0x80000000 len=16'"
            python3 "$HERE/oracle-client.py" --host "$HOST" raw \
                "mem.read addr=0x80000000 len=16" 2>&1 \
                || echo "(mem.read failed)"
        } > "$diag_dir/post-fail.txt" 2>&1
        break
    fi
done

# Summary
{
    echo "# oracle-stress"
    echo
    date -u +"Started: %Y-%m-%dT%H:%M:%SZ"
    echo
    echo "iterations_target: $ITERATIONS"
    echo "iterations_passed: $PASS"
    if [ -n "$FAIL_AT" ]; then
        echo "fail_at: $FAIL_AT"
        echo "fail_log: $FAIL_LOG"
    fi
} > "$OUT_DIR/report.md"

if [ "$PASS" -eq "$ITERATIONS" ]; then
    echo "================================================================"
    echo "oracle-stress: $PASS/$ITERATIONS PASS — degraded state NOT reproduced"
    echo "out=$OUT_DIR"
    echo "================================================================"
    exit 0
fi

echo "================================================================"
echo "oracle-stress: $PASS/$ITERATIONS PASS — degraded state REPRODUCED at iter $FAIL_AT"
echo "fail_log=$FAIL_LOG"
echo "post-fail diagnostics=$OUT_DIR/iter-$(printf '%03d' "$FAIL_AT")-diag/"
echo "out=$OUT_DIR"
echo "================================================================"
exit 1
