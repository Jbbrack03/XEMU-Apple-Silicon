#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_ROOT="${ROOT_DIR}/benchmark-runs"
DEFAULT_CROP="641,209,1278,957"

usage() {
    cat <<EOF
usage: $0 crimson|rainbow SNAPSHOT_HDD SNAPSHOT_TAG [duration-seconds] [crop|none]

Runs paired baseline/native triangle-depth snapshot benchmarks, summarizes
performance, and compares matching screenshots over a fixed crop.

Example:
  $0 rainbow benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \\
    rainbow_scene_b1_nothumb 16
EOF
}

if [[ $# -lt 3 || $# -gt 5 ]]; then
    usage >&2
    exit 2
fi

TARGET="$1"
SNAPSHOT_HDD="$2"
SNAPSHOT_TAG="$3"
DURATION="${4:-16}"
CROP="${5:-$DEFAULT_CROP}"
SCREENSHOT_INDEX="${XEMU_NATIVE_TRI_DEPTH_COMPARE_SCREENSHOT_INDEX:-1}"
RUN_RETRIES="${XEMU_NATIVE_TRI_DEPTH_COMPARE_RETRIES:-2}"

if [[ ! "$SCREENSHOT_INDEX" =~ ^[0-9]+$ ]]; then
    echo "XEMU_NATIVE_TRI_DEPTH_COMPARE_SCREENSHOT_INDEX must be a non-negative integer" >&2
    exit 2
fi

if [[ ! "$RUN_RETRIES" =~ ^[1-9][0-9]*$ ]]; then
    echo "XEMU_NATIVE_TRI_DEPTH_COMPARE_RETRIES must be a positive integer" >&2
    exit 2
fi

case "$TARGET" in
    crimson)
        GAME_NAME="crimson-skies"
        ;;
    rainbow)
        GAME_NAME="rainbow-six-3"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if [[ ! -f "$SNAPSHOT_HDD" ]]; then
    echo "missing snapshot HDD: $SNAPSHOT_HDD" >&2
    exit 1
fi

REPORT_DIR="${RUN_ROOT}/$(date +%Y%m%d-%H%M%S)-native-tri-depth-compare-${GAME_NAME}"
mkdir -p "$REPORT_DIR"

run_case() {
    local label="$1"
    local native_tri_depth="$2"
    local attempt
    local launcher_log
    local output
    local run_dir
    local summary_file="${REPORT_DIR}/${label}-perf-summary.txt"

    for ((attempt = 1; attempt <= RUN_RETRIES; attempt++)); do
        launcher_log="${REPORT_DIR}/${label}-launcher-attempt-${attempt}.log"
        echo "Starting ${label} ${GAME_NAME} run (attempt ${attempt}/${RUN_RETRIES})" >&2
        set +e
        XEMU_BENCH_HDD_SOURCE="$SNAPSHOT_HDD" \
        XEMU_BENCH_LOADVM_TAG="$SNAPSHOT_TAG" \
        XEMU_BENCH_SCREENSHOT_BACKEND="${XEMU_BENCH_SCREENSHOT_BACKEND:-macos}" \
        XEMU_BENCH_SCREENSHOT_START_DELAY="${XEMU_BENCH_SCREENSHOT_START_DELAY:-6}" \
        XEMU_BENCH_SCREENSHOT_INTERVAL="${XEMU_BENCH_SCREENSHOT_INTERVAL:-4}" \
        XEMU_NATIVE_TRI_DEPTH="$native_tri_depth" \
        XEMU_DIAG_NATIVE_TRI_DEPTH=0 \
        XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=0 \
        XEMU_DIAG_SKIP_TRI_GEOM=0 \
        XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=0 \
        "${ROOT_DIR}/scripts/apple-silicon/run-benchmark.sh" \
            "$TARGET" "${ROOT_DIR}/scripts/apple-silicon/input-scripts/noop.csv" \
            "$DURATION" 2>&1 | tee "$launcher_log"
        local launcher_status=${PIPESTATUS[0]}
        set -e

        if [[ "$launcher_status" -ne 0 ]]; then
            echo "${label} launcher failed with status ${launcher_status}" >&2
            continue
        fi

        run_dir="$(awk -F': ' '/^Run directory: / { print $2 }' "$launcher_log" |
            tail -n 1)"

        if [[ -z "$run_dir" || ! -d "$run_dir" ]]; then
            echo "could not determine run directory for ${label}" >&2
            continue
        fi

        if "${ROOT_DIR}/scripts/apple-silicon/extract-perf-summary.sh" "$run_dir" \
            > "$summary_file"; then
            cp "$launcher_log" "${REPORT_DIR}/${label}-launcher.log"
            RUN_CASE_DIR="$run_dir"
            return 0
        fi

        echo "${label} run produced no usable xemu-perf summary: ${run_dir}" >&2
    done

    echo "${label} ${GAME_NAME} run failed after ${RUN_RETRIES} attempt(s)" >&2
    return 1
}

RUN_CASE_DIR=""
run_case baseline 0
BASELINE_RUN="$RUN_CASE_DIR"
run_case native 1
CANDIDATE_RUN="$RUN_CASE_DIR"

printf 'baseline_run=%s\n' "$BASELINE_RUN" > "${REPORT_DIR}/metadata.txt"
printf 'candidate_run=%s\n' "$CANDIDATE_RUN" >> "${REPORT_DIR}/metadata.txt"
printf 'snapshot_hdd=%s\n' "$SNAPSHOT_HDD" >> "${REPORT_DIR}/metadata.txt"
printf 'snapshot_tag=%s\n' "$SNAPSHOT_TAG" >> "${REPORT_DIR}/metadata.txt"
printf 'duration_seconds=%s\n' "$DURATION" >> "${REPORT_DIR}/metadata.txt"
printf 'run_retries=%s\n' "$RUN_RETRIES" >> "${REPORT_DIR}/metadata.txt"
printf 'screenshot_index=%s\n' "$SCREENSHOT_INDEX" >> "${REPORT_DIR}/metadata.txt"
printf 'crop=%s\n' "$CROP" >> "${REPORT_DIR}/metadata.txt"

if [[ "$CROP" != "none" ]]; then
    screenshot_line=$((SCREENSHOT_INDEX + 1))
    baseline_screenshot="$(
        find "${BASELINE_RUN}/screenshots" -maxdepth 1 -name '*.png' |
        sort |
        sed -n "${screenshot_line}p"
    )"
    candidate_screenshot="$(
        find "${CANDIDATE_RUN}/screenshots" -maxdepth 1 -name '*.png' |
        sort |
        sed -n "${screenshot_line}p"
    )"

    if [[ -z "$baseline_screenshot" || -z "$candidate_screenshot" ]]; then
        echo "not enough screenshots to compare index ${SCREENSHOT_INDEX}" >&2
        exit 1
    fi

    "${ROOT_DIR}/scripts/apple-silicon/compare-screenshots.py" \
        "$baseline_screenshot" \
        "$candidate_screenshot" \
        --crop "$CROP" \
        --out-dir "${REPORT_DIR}/visual-compare" \
        > "${REPORT_DIR}/visual-compare.txt"
fi

echo "Finished native triangle-depth comparison"
echo "Report: $REPORT_DIR"
echo "Baseline: $BASELINE_RUN"
echo "Native: $CANDIDATE_RUN"
