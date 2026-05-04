#!/usr/bin/env bash
# metal-canary-regress.sh — single-renderer Metal canary regression gate
# (slice W3, 2026-05-04).
#
# Drives `run-benchmark.sh` in `XEMU_RENDERER=METAL` mode against the four
# green Metal canaries (PGR2 menu / Rainbow Six 3 loading / Halo CE menu /
# Xbox boot+flubber) under the established opt-in flag recipe, captures a
# single screenshot at the canary's frame ordinal via the in-renderer
# `XEMU_METAL_SCREENSHOT_*` path, and per-pixel-diffs each shot against the
# stored gold PNG under `benchmark-runs/visual-checks/`. Emits a markdown
# report and JSON summary with PASS/FAIL.
#
# This is the "post-change smoke" tool from
# `docs/apple-silicon/metal-porting-workflow.md` Phase 1 daily loop §3.5:
# after every Metal renderer change, the four canaries must still match
# their gold PNGs within the threshold or the change regressed something.
#
# Project rule #11: this script ASSERTS the closed default-on Apple Silicon
# flags still produce the gold PNG; it does NOT re-validate them. Failure
# to assert means a Metal-side regression, not a flag-design question.
#
# NOT paired with GL — that is W2's `metal-gl-compare.sh`. This script is
# single-renderer (Metal) against a stored baseline.
#
# bash 3.2-compatible (macOS default).
# Apple Silicon performance fork.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_ROOT="${ROOT_DIR}/benchmark-runs"
XEMU_BIN="${ROOT_DIR}/dist/xemu.app/Contents/MacOS/xemu"
RUN_BENCHMARK="${ROOT_DIR}/scripts/apple-silicon/run-benchmark.sh"
COMPARE_SCREENSHOTS="${ROOT_DIR}/scripts/apple-silicon/compare-screenshots.py"
INPUT_SCRIPT_DIR="${ROOT_DIR}/scripts/apple-silicon/input-scripts"
GOLD_DIR="${ROOT_DIR}/benchmark-runs/visual-checks"

# Canary table — single source of truth.
#
# Each row is "<name>|<game-alias>|<input-relpath-or-empty>|<frame>|<duration>|<gold-basename>".
#
# - name             : alias the user passes to --canary
# - game-alias       : value passed as run-benchmark.sh's <game> positional
# - input-relpath    : path under scripts/apple-silicon/input-scripts/
#                      relative to that dir; empty string = let
#                      run-benchmark.sh use its DEFAULT_SCRIPT (noop.csv
#                      for halo and boot per the launcher's case stmt)
# - frame            : XEMU_METAL_SCREENSHOT_AT_FRAME (1-indexed end-of-
#                      frame counter, NOT pgraph_mtl_present_total)
# - duration         : seconds to keep xemu running. Tuned conservatively
#                      so the frame ordinal is reached even when Metal
#                      cold-launches a fresh shader cache:
#                        f300 / boot   → 60 s   (boot/flubber renders fast)
#                        f600 / rainbow→ 75 s   (loading screen)
#                        f900 / pgr2   → 90 s   (menu, post-load)
#                        f1200 / halo  → 120 s  (menu, post-load)
#                      Runtime ratio is non-linear: cold shader compile
#                      front-loads the first ~10 s. Headroom prevents a
#                      false INFRA-FAIL when the host is under load.
# - gold-basename    : file under benchmark-runs/visual-checks/ that the
#                      captured screenshot is per-pixel-diffed against.
#                      The handoff.md "PASS (visual canary)" bullets are
#                      the source of truth for these names.
CANARY_TABLE='
pgr2|pgr2|pgr2-smoke.csv|900|90|pgr2-gate-metal-msaa4-f900-after-msaa-store.png
rainbow|rainbow|rainbow-six-3-smoke.csv|600|75|rainbow-gate-metal-msaa4-f600-after-msaa-store.png
halo|halo||1200|120|halo-gate-metal-msaa4-f1200-after-msaa-store.png
boot|crimson||300|60|boot-gate-metal-msaa4-f300-after-msaa-store.png
'
# boot uses the `crimson` launcher alias because the Xbox-boot/flubber
# screenshot was originally captured during a Crimson Skies run that hit
# the boot animation before the Crimson title screen. The gold PNG is the
# Xbox dashboard boot+flubber sequence — the game alias just selects the
# disc and the launcher harness; the captured frame is upstream of any
# game logic. (Source: handoff.md "Xbox boot/flubber PASS" bullet,
# benchmark-runs/20260504-100747-crimson-skies.)

usage() {
    cat <<EOF
usage: $0 [--canary <name>] [--threshold pct] [--out-dir <path>] [--help]

Single-renderer Metal canary regression gate (slice W3, 2026-05-04).
Runs each named canary through the Metal renderer with the established
green-canary env recipe, captures one screenshot at the canary's frame
ordinal, and per-pixel-diffs it against the stored gold PNG under
benchmark-runs/visual-checks/. Emits report.md + summary.json.

Options:
  --canary <name>    Run only the named canary. Default: all four.
                     Names: pgr2 | rainbow | halo | boot.
  --threshold pct    Maximum changed-pixels percentage per canary for
                     the run to PASS. Default 1.0 (1 %).
  --out-dir <path>   Output directory. Default
                     benchmark-runs/<TS>-canary-regress/.
  --help             Print this usage.

Canary table (hard-coded; source of truth = docs/apple-silicon/handoff.md
"PASS (visual canary)" bullets):

  pgr2     -> input-scripts/pgr2-smoke.csv         frame 900   gold pgr2-gate-metal-msaa4-f900-after-msaa-store.png
  rainbow  -> input-scripts/rainbow-six-3-smoke.csv frame 600  gold rainbow-gate-metal-msaa4-f600-after-msaa-store.png
  halo     -> launcher default (noop.csv)          frame 1200  gold halo-gate-metal-msaa4-f1200-after-msaa-store.png
  boot     -> launcher default (noop.csv)          frame 300   gold boot-gate-metal-msaa4-f300-after-msaa-store.png

Env recipe (verbatim from handoff.md "PGR2 PASS" bullet; project rule #11
forbids re-validating these — this script ASSERTS them):

  XEMU_RENDERER=METAL
  XEMU_METAL_TRANSLATED_PIPELINE=1
  XEMU_NATIVE_TRI_DEPTH=1
  XEMU_NATIVE_QUAD=1
  XEMU_PGRAPH_FAST_READ=1
  XEMU_METAL_FRONT_FB_FALLBACK=1
  XEMU_METAL_MSAA=4

Exit codes:
  0  PASS — every canary <= --threshold against its gold PNG
  1  FAIL — at least one canary regressed beyond threshold
  2  INFRA-FAIL — binary missing, gold/input asset missing,
                  run-benchmark.sh failure, screenshot not produced,
                  diff infrastructure failure
EOF
}

err() {
    echo "metal-canary-regress: $*" >&2
}

# --- argument parsing ------------------------------------------------------

CANARY_FILTER=""
THRESHOLD="1.0"
OUT_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            usage
            exit 0
            ;;
        --canary)
            [[ $# -ge 2 ]] || { err "--canary requires a name"; exit 2; }
            CANARY_FILTER="$2"; shift 2 ;;
        --canary=*)
            CANARY_FILTER="${1#--canary=}"; shift ;;
        --threshold)
            [[ $# -ge 2 ]] || { err "--threshold requires a value"; exit 2; }
            THRESHOLD="$2"; shift 2 ;;
        --threshold=*)
            THRESHOLD="${1#--threshold=}"; shift ;;
        --out-dir)
            [[ $# -ge 2 ]] || { err "--out-dir requires a path"; exit 2; }
            OUT_DIR="$2"; shift 2 ;;
        --out-dir=*)
            OUT_DIR="${1#--out-dir=}"; shift ;;
        --)
            shift; break ;;
        --*)
            err "unknown flag: $1"
            usage >&2
            exit 2 ;;
        *)
            err "unexpected positional argument: $1"
            usage >&2
            exit 2 ;;
    esac
done

if [[ -n "$CANARY_FILTER" ]]; then
    case "$CANARY_FILTER" in
        pgr2|rainbow|halo|boot) ;;
        *)
            err "unknown canary '$CANARY_FILTER' (expected pgr2|rainbow|halo|boot)"
            exit 2 ;;
    esac
fi

# Threshold validation — bash 3.2 has no float compare; regex is the
# cheapest sanity check.
if ! [[ "$THRESHOLD" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    err "--threshold must be a non-negative number (got '$THRESHOLD')"
    exit 2
fi

# --- pre-flight ------------------------------------------------------------

if [[ ! -x "$XEMU_BIN" ]]; then
    err "missing or non-executable xemu binary: $XEMU_BIN"
    err "run './build.sh -a arm64' from $ROOT_DIR first"
    exit 2
fi

if ! "$XEMU_BIN" --version >/dev/null 2>&1; then
    err "xemu --version failed; binary may be unsigned or broken: $XEMU_BIN"
    exit 2
fi

for tool in "$RUN_BENCHMARK" "$COMPARE_SCREENSHOTS"; do
    if [[ ! -e "$tool" ]]; then
        err "missing helper: $tool"
        exit 2
    fi
done

# Verify each requested canary has its input script + gold PNG.
canary_rows() {
    # Echo the rows of CANARY_TABLE that match CANARY_FILTER (or all rows
    # when filter is empty), one per line.
    local row
    while IFS= read -r row; do
        [[ -z "$row" ]] && continue
        local name="${row%%|*}"
        if [[ -z "$CANARY_FILTER" || "$name" == "$CANARY_FILTER" ]]; then
            printf '%s\n' "$row"
        fi
    done <<EOF
$(printf '%s' "$CANARY_TABLE" | grep -v '^$')
EOF
}

PREFLIGHT_FAIL=0
while IFS='|' read -r name game input frame duration gold; do
    [[ -z "$name" ]] && continue
    if [[ -n "$input" ]]; then
        input_path="${INPUT_SCRIPT_DIR}/${input}"
        if [[ ! -e "$input_path" ]]; then
            err "missing input script for canary '$name': $input_path"
            PREFLIGHT_FAIL=1
        fi
    fi
    gold_path="${GOLD_DIR}/${gold}"
    if [[ ! -e "$gold_path" ]]; then
        err "missing gold PNG for canary '$name': $gold_path"
        PREFLIGHT_FAIL=1
    fi
done < <(canary_rows)

if [[ "$PREFLIGHT_FAIL" -ne 0 ]]; then
    err "pre-flight failed; aborting"
    exit 2
fi

# --- output directory ------------------------------------------------------

if [[ -z "$OUT_DIR" ]]; then
    STAMP="$(date +%Y%m%d-%H%M%S)"
    OUT_DIR="${RUN_ROOT}/${STAMP}-canary-regress"
fi

mkdir -p "$OUT_DIR"

LOG_FILE="$OUT_DIR/harness.log"
RESULTS_TSV="$OUT_DIR/results.tsv"
: > "$RESULTS_TSV"

log() {
    printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG_FILE"
}

log "metal-canary-regress W3 starting"
log "  canary_filter = ${CANARY_FILTER:-<all>}"
log "  threshold     = ${THRESHOLD}%"
log "  out_dir       = $OUT_DIR"

# --- compare helper --------------------------------------------------------

# Read the displayed PNG dimensions and synthesize "0,0,W,H" for
# compare-screenshots.py (which requires --crop). Mirrors metal-gl-compare.sh's
# default_crop_for() so the diff covers the full image.
default_crop_for() {
    local png="$1"
    python3 - "$png" <<'PY'
import sys
from PIL import Image
img = Image.open(sys.argv[1])
print(f"0,0,{img.size[0]},{img.size[1]}")
PY
}

parse_kv() {
    local key="$1"
    local file="$2"
    awk -F= -v k="$key" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$file"
}

# --- per-canary run --------------------------------------------------------

# Run a single canary. Sets RUN_RC, CHANGED_PCT, MAE, RMS, MAX_ABS,
# CAPTURED_PNG, RUN_DIR. Returns 0 if the diff completed (regardless of
# pass/fail outcome), 2 on infrastructure failure.
run_canary() {
    local name="$1"
    local game="$2"
    local input="$3"
    local frame="$4"
    local duration="$5"
    local gold="$6"

    local canary_dir="$OUT_DIR/$name"
    mkdir -p "$canary_dir"
    local launcher_log="$canary_dir/launcher.log"
    local shot_path="$canary_dir/screenshot.png"
    local gold_path="${GOLD_DIR}/${gold}"

    # Resolve the input CSV. Empty input means use the launcher's default
    # script (noop.csv for halo/boot per run-benchmark.sh's case stmt).
    local input_arg=""
    if [[ -n "$input" ]]; then
        input_arg="${INPUT_SCRIPT_DIR}/${input}"
    fi

    log "[$name] starting Metal canary run"
    log "  game     = $game"
    log "  input    = ${input_arg:-<launcher default>}"
    log "  frame    = $frame"
    log "  duration = ${duration}s"
    log "  gold     = $gold_path"

    # Build the env recipe verbatim from handoff.md "PGR2 PASS" bullet.
    # Project rule #11: do not add or remove flags; this set is closed.
    # XEMU_BENCH_SCREENSHOT_BACKEND=none disables the macos-screencapture
    # cron so the only output PNG is the in-renderer single-shot capture.
    local rc
    set +e
    if [[ -n "$input_arg" ]]; then
        XEMU_RENDERER=METAL \
        XEMU_METAL_TRANSLATED_PIPELINE=1 \
        XEMU_NATIVE_TRI_DEPTH=1 \
        XEMU_NATIVE_QUAD=1 \
        XEMU_PGRAPH_FAST_READ=1 \
        XEMU_METAL_FRONT_FB_FALLBACK=1 \
        XEMU_METAL_MSAA=4 \
        XEMU_BENCH_SCREENSHOT_BACKEND=none \
            "$RUN_BENCHMARK" \
                --metal-screenshot "$shot_path" \
                --metal-screenshot-at-frame "$frame" \
                --metal-no-hud \
                "$game" "$input_arg" "$duration" \
            > "$launcher_log" 2>&1
        rc=$?
    else
        XEMU_RENDERER=METAL \
        XEMU_METAL_TRANSLATED_PIPELINE=1 \
        XEMU_NATIVE_TRI_DEPTH=1 \
        XEMU_NATIVE_QUAD=1 \
        XEMU_PGRAPH_FAST_READ=1 \
        XEMU_METAL_FRONT_FB_FALLBACK=1 \
        XEMU_METAL_MSAA=4 \
        XEMU_BENCH_SCREENSHOT_BACKEND=none \
            "$RUN_BENCHMARK" \
                --metal-screenshot "$shot_path" \
                --metal-screenshot-at-frame "$frame" \
                --metal-no-hud \
                "$game" "$duration" \
            > "$launcher_log" 2>&1
        rc=$?
    fi
    set -e

    if [[ $rc -ne 0 ]]; then
        err "[$name] run-benchmark.sh failed with status $rc; see $launcher_log"
        return 2
    fi

    local run_dir
    run_dir="$(awk -F': ' '/^Run directory: / { print $2 }' "$launcher_log" | tail -n 1)"
    if [[ -z "$run_dir" || ! -d "$run_dir" ]]; then
        err "[$name] could not parse run directory from $launcher_log"
        return 2
    fi
    printf '%s\n' "$run_dir" > "$canary_dir/run-dir.txt"
    log "[$name] run dir: $run_dir"

    # The XEMU_METAL_SCREENSHOT_PATH hot path writes to the literal path
    # supplied, plus an "<base>.NNNN.png" interval suffix when
    # XEMU_METAL_SCREENSHOT_INTERVAL is non-zero. We only request a single
    # shot (interval defaults to 0), so the un-suffixed path is canonical.
    if [[ ! -e "$shot_path" ]]; then
        # Fall back to a glob — if a future renderer-side change starts
        # writing the suffixed form unconditionally, take the first one.
        local fallback
        fallback="$(find "$canary_dir" -maxdepth 1 -name 'screenshot*.png' -type f 2>/dev/null | sort | head -n 1 || true)"
        if [[ -n "$fallback" && -e "$fallback" ]]; then
            shot_path="$fallback"
            log "[$name] note: using suffixed screenshot fallback: $shot_path"
        else
            err "[$name] expected screenshot at $shot_path but none found"
            err "[$name] launcher log: $launcher_log"
            return 2
        fi
    fi

    # Diff against gold.
    local diff_dir="$canary_dir/diff"
    mkdir -p "$diff_dir"
    local crop
    if ! crop="$(default_crop_for "$gold_path" 2>"$diff_dir/crop-err.txt")"; then
        err "[$name] could not read gold PNG dimensions: $gold_path"
        cat "$diff_dir/crop-err.txt" >&2 || true
        return 2
    fi

    local stdout_file="$diff_dir/compare-stdout.txt"
    set +e
    "$COMPARE_SCREENSHOTS" \
        "$gold_path" "$shot_path" \
        --crop "$crop" \
        --out-dir "$diff_dir" \
        > "$stdout_file" 2>&1
    local cmp_rc=$?
    set -e
    if [[ $cmp_rc -ne 0 ]]; then
        err "[$name] compare-screenshots.py failed (rc=$cmp_rc); see $stdout_file"
        return 2
    fi

    local mae rms max_abs changed_pct
    mae="$(parse_kv mean_abs_error "$stdout_file")"
    rms="$(parse_kv rms_error "$stdout_file")"
    max_abs="$(parse_kv max_abs_error "$stdout_file")"
    changed_pct="$(parse_kv changed_pixels_pct "$stdout_file")"

    local verdict="PASS"
    if awk -v c="$changed_pct" -v t="$THRESHOLD" 'BEGIN { exit !(c+0 > t+0) }'; then
        verdict="FAIL"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$verdict" "$changed_pct" "$mae" "$rms" "$max_abs" \
        "$shot_path" "$gold_path" "$run_dir" \
        >> "$RESULTS_TSV"

    log "[$name] $verdict changed_pct=$changed_pct mae=$mae rms=$rms"
    return 0
}

# --- top-level orchestration ----------------------------------------------

# Sequential execution — concurrent xemu instances would race the Metal
# capture path and run-benchmark.sh refuses overlap by default anyway.
INFRA_FAIL=0
while IFS='|' read -r name game input frame duration gold; do
    [[ -z "$name" ]] && continue
    if ! run_canary "$name" "$game" "$input" "$frame" "$duration" "$gold"; then
        INFRA_FAIL=1
    fi
done < <(canary_rows)

if [[ "$INFRA_FAIL" -ne 0 ]]; then
    err "infrastructure failure during canary execution; partial results in $OUT_DIR"
    exit 2
fi

# Determine overall PASS/FAIL by walking results.tsv.
PASS=1
FAIL_LINES=()
while IFS=$'\t' read -r name verdict changed_pct mae rms max_abs shot gold run_dir; do
    [[ -z "$name" ]] && continue
    if [[ "$verdict" == "FAIL" ]]; then
        PASS=0
        FAIL_LINES+=("canary $name: changed_pixels_pct=$changed_pct > threshold=$THRESHOLD")
    fi
done < "$RESULTS_TSV"

REPORT_MD="$OUT_DIR/report.md"
SUMMARY_JSON="$OUT_DIR/summary.json"

verdict_total="PASS"
if [[ "$PASS" -eq 0 ]]; then verdict_total="FAIL"; fi

{
    printf '# %s — metal-canary-regress report\n\n' "$verdict_total"
    printf '- threshold: %s%% changed-pixels per canary\n' "$THRESHOLD"
    printf '- canary filter: %s\n' "${CANARY_FILTER:-<all four>}"
    printf '- env recipe: XEMU_RENDERER=METAL XEMU_METAL_TRANSLATED_PIPELINE=1 XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1 XEMU_METAL_FRONT_FB_FALLBACK=1 XEMU_METAL_MSAA=4\n'
    printf '- harness log: %s\n' "$LOG_FILE"
    printf '\n## Per-canary results\n\n'
    printf '| canary | verdict | changed_pct | mae | rms | max_abs | screenshot | gold | run_dir |\n'
    printf '|--------|---------|------------:|----:|----:|--------:|------------|------|---------|\n'
    while IFS=$'\t' read -r name verdict changed_pct mae rms max_abs shot gold run_dir; do
        [[ -z "$name" ]] && continue
        printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
            "$name" "$verdict" "$changed_pct" "$mae" "$rms" "$max_abs" \
            "$shot" "$gold" "$run_dir"
    done < "$RESULTS_TSV"
    if [[ "$PASS" -eq 0 ]]; then
        printf '\n## Canaries over threshold\n\n'
        for line in "${FAIL_LINES[@]}"; do
            printf -- '- %s\n' "$line"
        done
    fi
    printf '\n_Generated by `scripts/apple-silicon/metal-canary-regress.sh` (slice W3, 2026-05-04)._\n'
} > "$REPORT_MD"

# summary.json — hand-rolled (matches metal-gl-compare.sh) to avoid a
# hard dep on jq.
{
    printf '{\n'
    printf '  "pass": %s,\n' "$([[ $PASS -eq 1 ]] && echo true || echo false)"
    printf '  "verdict": "%s",\n' "$verdict_total"
    printf '  "threshold": %s,\n' "$THRESHOLD"
    printf '  "canary_filter": "%s",\n' "${CANARY_FILTER:-all}"
    printf '  "canaries": [\n'
    first=1
    while IFS=$'\t' read -r name verdict changed_pct mae rms max_abs shot gold run_dir; do
        [[ -z "$name" ]] && continue
        if [[ $first -eq 1 ]]; then first=0; else printf ',\n'; fi
        printf '    { "name": "%s", "verdict": "%s", "changed_pct": %s, "mae": %s, "rms": %s, "max_abs": %s, "screenshot": "%s", "gold": "%s", "run_dir": "%s" }' \
            "$name" "$verdict" "$changed_pct" "$mae" "$rms" "$max_abs" \
            "$shot" "$gold" "$run_dir"
    done < "$RESULTS_TSV"
    printf '\n  ]\n'
    printf '}\n'
} > "$SUMMARY_JSON"

log "wrote $REPORT_MD"
log "wrote $SUMMARY_JSON"
log "verdict: $verdict_total"

if [[ "$PASS" -eq 1 ]]; then
    exit 0
fi
exit 1
