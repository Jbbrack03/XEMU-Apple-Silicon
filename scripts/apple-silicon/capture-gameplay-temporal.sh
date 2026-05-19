#!/usr/bin/env bash
# Gameplay-route temporal capture (Tool 2, 2026-05-19).
#
# Thin orchestrator over run-benchmark.sh. Records PNG-every-frame
# under one renderer over a controller-driven gameplay route, suitable
# for downstream `scripts/apple-silicon/temporal-flicker-analyze.py`
# consumption. This is the gameplay analogue of capture-boot-temporal.sh.
#
# Codex review 2026-05-19 (finding #3): gameplay-temporal overlaps with
# run-benchmark.sh (scratch HDD, xemu-running guard, scripted input,
# QMP socket, snapshot loadvm, cleanup) rather than with the boot
# script (which uses a placeholder DVD + no input). This wrapper sets
# XEMU_BENCH_TEMPORAL_CAPTURE=1 and calls run-benchmark.sh — the
# launcher owns the actual capture mechanism (renderer-native every-
# frame Metal screenshot or parallel ffmpeg AVFoundation for GL).
#
# Usage:
#   capture-gameplay-temporal.sh --renderer {GL|METAL} --game <alias> \
#       [--input <csv>] [--duration N] [--fps N] \
#       [--snapshot <tag>] [--loadvm-at <sec>] [--out-name <name>]
#
# Output layout:
#   benchmark-runs/<ts>-<game>/
#     ├─ frames/
#     │   ├─ metal-gameplay.NNNN.png   (Metal leg)
#     │   └─ gameplay-NNNN.png         (GL leg, after ffmpeg decompose)
#     ├─ ffmpeg-temporal.log           (GL leg)
#     ├─ xemu.log
#     ├─ screenshots/                  (empty; backend=none in temporal mode)
#     └─ metadata.txt + qmp.sock + ...
#
# Apple-Silicon-fork rules honored (transitively via run-benchmark.sh):
#   #9  scratch HDD copy
#   #10 refuses to launch when another xemu is running
#   #13 XEMU_SNAPSHOT_NO_THUMBNAIL=1
#   #14 SCREENSHOT_BACKEND=none avoids QMP screendump

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_BENCHMARK="${ROOT_DIR}/scripts/apple-silicon/run-benchmark.sh"
INPUT_SCRIPT_DIR="${ROOT_DIR}/scripts/apple-silicon/input-scripts"

usage() {
    cat <<EOF
usage: $0 --renderer {GL|METAL} --game <alias> [options]

Records PNG-every-frame under a scripted-input gameplay route via
run-benchmark.sh, suitable for temporal-flicker-analyze.py.

Required:
  --renderer GL|METAL   Which renderer leg to capture.
  --game <alias>        crimson|rainbow|pgr2|sc2|halo|flat-tri-depth.

Optional:
  --input <csv>         Scripted-input CSV. Default: <game>-gameplay.csv
                        if present (preferred for gameplay evidence
                        routes), else <game>-smoke.csv, else the
                        per-alias default that run-benchmark.sh uses.
  --duration <sec>      Capture duration. Default 30.
  --fps <N>             GL leg ffmpeg framerate. Default 60. Metal leg
                        is renderer-native every-frame regardless.
  --snapshot <tag>      QMP/HMP loadvm tag. Loaded at --loadvm-at sec.
  --loadvm-at <sec>     Default 2.
  --out-name <name>     Override run-dir basename suffix. The actual
                        run dir is still created by run-benchmark.sh
                        under benchmark-runs/<ts>-<alias>/ — this
                        adds a trailing symlink for convenience.

Environment passthrough:
  XEMU_NATIVE_TRI_DEPTH / XEMU_NATIVE_QUAD / XEMU_PGRAPH_FAST_READ
  XEMU_GL_MSAA (GL recipe)
  XEMU_METAL_TRANSLATED_PIPELINE / XEMU_METAL_FRONT_FB_FALLBACK /
  XEMU_METAL_MSAA (Metal recipe)
  XEMU_BENCH_ALLOW_EXISTING
  XEMU_METAL_SURFACE_GRAPH_DUMP=path   (Tool 1; emit alongside frames)

EOF
}

RENDERER=""
GAME=""
INPUT_CSV=""
DURATION="30"
FPS="60"
SNAPSHOT_TAG=""
LOADVM_AT="2"
OUT_NAME=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --renderer) RENDERER="$(echo "$2" | tr '[:lower:]' '[:upper:]')"; shift 2 ;;
        --renderer=*) RENDERER="$(echo "${1#--renderer=}" | tr '[:lower:]' '[:upper:]')"; shift ;;
        --game) GAME="$2"; shift 2 ;;
        --game=*) GAME="${1#--game=}"; shift ;;
        --input) INPUT_CSV="$2"; shift 2 ;;
        --input=*) INPUT_CSV="${1#--input=}"; shift ;;
        --duration) DURATION="$2"; shift 2 ;;
        --duration=*) DURATION="${1#--duration=}"; shift ;;
        --fps) FPS="$2"; shift 2 ;;
        --fps=*) FPS="${1#--fps=}"; shift ;;
        --snapshot) SNAPSHOT_TAG="$2"; shift 2 ;;
        --snapshot=*) SNAPSHOT_TAG="${1#--snapshot=}"; shift ;;
        --loadvm-at) LOADVM_AT="$2"; shift 2 ;;
        --loadvm-at=*) LOADVM_AT="${1#--loadvm-at=}"; shift ;;
        --out-name) OUT_NAME="$2"; shift 2 ;;
        --out-name=*) OUT_NAME="${1#--out-name=}"; shift ;;
        --) shift; break ;;
        *) echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$RENDERER" != "GL" && "$RENDERER" != "METAL" ]]; then
    echo "--renderer GL|METAL is required" >&2; usage >&2; exit 2
fi
if [[ -z "$GAME" ]]; then
    echo "--game is required" >&2; usage >&2; exit 2
fi
case "$GAME" in
    crimson|rainbow|pgr2|sc2|halo|flat-tri-depth) ;;
    *) echo "unknown game alias '$GAME'" >&2; exit 2 ;;
esac
case "$DURATION" in
    ''|*[!0-9]*) echo "--duration must be a positive integer" >&2; exit 2 ;;
esac

# Resolve input CSV. Prefer <game>-gameplay.csv (longer/richer routes for
# gameplay evidence) over <game>-smoke.csv. Fall back to the launcher's
# alias default (noop.csv for sc2/halo/flat-tri-depth; per-alias for
# crimson/rainbow/pgr2) so we can ALWAYS pass three positional args to
# run-benchmark.sh and propagate --duration. Mirrors the case
# statement in run-benchmark.sh.
if [[ -z "$INPUT_CSV" ]]; then
    for cand in \
        "${INPUT_SCRIPT_DIR}/${GAME}-gameplay.csv" \
        "${INPUT_SCRIPT_DIR}/${GAME}-smoke.csv"; do
        if [[ -e "$cand" ]]; then INPUT_CSV="$cand"; break; fi
    done
fi
if [[ -z "$INPUT_CSV" ]]; then
    case "$GAME" in
        crimson)        INPUT_CSV="${INPUT_SCRIPT_DIR}/crimson-skies-smoke.csv" ;;
        rainbow)        INPUT_CSV="${INPUT_SCRIPT_DIR}/rainbow-six-3-smoke.csv" ;;
        pgr2)           INPUT_CSV="${INPUT_SCRIPT_DIR}/pgr2-smoke.csv" ;;
        sc2|halo|flat-tri-depth)
                        INPUT_CSV="${INPUT_SCRIPT_DIR}/noop.csv" ;;
    esac
fi
if [[ -z "$INPUT_CSV" || ! -e "$INPUT_CSV" ]]; then
    echo "input CSV not found / unresolved for game '$GAME': '$INPUT_CSV'" >&2
    exit 2
fi

# Build the env block. Defaults match the M15 canonical recipe from
# metal-gl-compare.sh; explicit user env wins.
ENV_VARS=(XEMU_RENDERER="$RENDERER"
          XEMU_BENCH_TEMPORAL_CAPTURE=1
          XEMU_BENCH_TEMPORAL_FPS="$FPS"
          XEMU_BENCH_SCREENSHOT_BACKEND=none
          XEMU_PERF_FRAME_LOG="${XEMU_PERF_FRAME_LOG:-1}")
[[ "${XEMU_NATIVE_TRI_DEPTH+x}" != "x" ]] && ENV_VARS+=("XEMU_NATIVE_TRI_DEPTH=1")
[[ "${XEMU_NATIVE_QUAD+x}" != "x" ]] && ENV_VARS+=("XEMU_NATIVE_QUAD=1")
[[ "${XEMU_PGRAPH_FAST_READ+x}" != "x" ]] && ENV_VARS+=("XEMU_PGRAPH_FAST_READ=1")
if [[ "$RENDERER" == "GL" ]]; then
    [[ "${XEMU_GL_MSAA+x}" != "x" ]] && ENV_VARS+=("XEMU_GL_MSAA=4")
else
    [[ "${XEMU_METAL_TRANSLATED_PIPELINE+x}" != "x" ]] && \
        ENV_VARS+=("XEMU_METAL_TRANSLATED_PIPELINE=1")
    [[ "${XEMU_METAL_FRONT_FB_FALLBACK+x}" != "x" ]] && \
        ENV_VARS+=("XEMU_METAL_FRONT_FB_FALLBACK=1")
    [[ "${XEMU_METAL_MSAA+x}" != "x" ]] && ENV_VARS+=("XEMU_METAL_MSAA=4")
fi
if [[ -n "$SNAPSHOT_TAG" ]]; then
    ENV_VARS+=("XEMU_BENCH_LOADVM_TAG=$SNAPSHOT_TAG"
               "XEMU_BENCH_LOADVM_AT=$LOADVM_AT")
fi

# Pre-flight: refuse if another xemu is running (also caught by
# run-benchmark.sh, but failing here gives a clearer error).
if [[ "${XEMU_BENCH_ALLOW_EXISTING:-0}" != "1" ]]; then
    if pgrep -x xemu >/dev/null 2>&1; then
        echo "another xemu is running; refusing (rule #10). " >&2
        echo "set XEMU_BENCH_ALLOW_EXISTING=1 to override." >&2
        exit 1
    fi
fi

echo "capture-gameplay-temporal: renderer=$RENDERER game=$GAME duration=${DURATION}s fps=$FPS"
echo "  input_csv: ${INPUT_CSV:-(launcher default)}"
[[ -n "$SNAPSHOT_TAG" ]] && echo "  snapshot:  $SNAPSHOT_TAG @ ${LOADVM_AT}s"

# Forward positional args to run-benchmark.sh. When INPUT_CSV is empty
# the launcher will use its built-in <game>-smoke default.
LAUNCH_OUT="$(mktemp -t cap-gameplay-temp.XXXXX.log)"
trap 'rm -f "$LAUNCH_OUT"' EXIT

set +e
env "${ENV_VARS[@]}" \
    "$RUN_BENCHMARK" "$GAME" "$INPUT_CSV" "$DURATION" \
    | tee "$LAUNCH_OUT"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -ne 0 ]]; then
    echo "run-benchmark.sh failed rc=$rc" >&2
    exit $rc
fi

RUN_DIR="$(awk -F': ' '/^Run directory: / { print $2 }' "$LAUNCH_OUT" | tail -n 1)"
if [[ -z "$RUN_DIR" || ! -d "$RUN_DIR" ]]; then
    echo "could not parse run directory from launcher output" >&2
    exit 2
fi
FRAMES_DIR="${RUN_DIR}/frames"

# Quick frame count for the operator. The metal leg writes individual
# PNGs as it runs; the GL leg writes a .mov which run-benchmark.sh
# already decomposed to PNGs in-place.
if [[ "$RENDERER" == "METAL" ]]; then
    FRAME_COUNT=$(find "$FRAMES_DIR" -maxdepth 1 -type f -name 'metal-gameplay.*.png' 2>/dev/null | wc -l | tr -d ' ')
    GLOB="metal-gameplay.*.png"
else
    FRAME_COUNT=$(find "$FRAMES_DIR" -maxdepth 1 -type f -name 'gameplay-*.png' 2>/dev/null | wc -l | tr -d ' ')
    GLOB="gameplay-*.png"
fi
FRAME_COUNT="${FRAME_COUNT:-0}"

# Per-run summary alongside the launcher metadata.
SUMMARY="${RUN_DIR}/temporal-summary.json"
python3 - "$SUMMARY" "$RENDERER" "$DURATION" "$FPS" "$FRAME_COUNT" "$FRAMES_DIR" "$GLOB" "$GAME" <<'PY'
import json, sys
out, renderer, duration, fps, fc, frames_dir, glob, game = sys.argv[1:9]
report = {
    "renderer": renderer, "game": game,
    "duration_seconds": float(duration), "fps_request": int(fps),
    "frame_count": int(fc), "frames_dir": frames_dir, "frames_glob": glob,
    "effective_fps_estimate": int(fc) / float(duration) if float(duration) > 0 else 0,
    "evidence_class": "gameplay-temporal",
}
with open(out, "w") as f:
    json.dump(report, f, indent=2, sort_keys=True)
print(json.dumps(report, indent=2, sort_keys=True))
PY

# Optional symlink for the requested out-name.
if [[ -n "$OUT_NAME" ]]; then
    LINK="$(dirname "$RUN_DIR")/${OUT_NAME}"
    ln -sfn "$(basename "$RUN_DIR")" "$LINK" 2>/dev/null || true
    echo "symlink: $LINK -> $RUN_DIR"
fi

echo
echo "Done. Run dir: $RUN_DIR"
echo "Frames:       $FRAME_COUNT in $FRAMES_DIR (glob: $GLOB)"
echo "Summary:      $SUMMARY"
echo
echo "Next: temporal-flicker-analyze.py \\"
echo "        --frames-dir '$FRAMES_DIR' \\"
echo "        --glob '$GLOB' \\"
echo "        --out-dir '${RUN_DIR}/flicker'"
