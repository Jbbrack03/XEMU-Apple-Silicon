#!/usr/bin/env bash
# metal-canary-regress.sh — single-renderer Metal canary regression gate
# (slice W3, 2026-05-04; counter-mode added 2026-05-04 evening).
#
# Two validation modes:
#
# - `--mode counters` (DEFAULT, autonomous-friendly): runs each canary
#   with the smoke recipe and validates the last-interval `xemu-perf:`
#   counters against per-canary thresholds. Catches concrete renderer
#   regressions (PSH/VSH translation failures, M5.7 coalescing drops,
#   drawable acquire failures, present rate collapse) WITHOUT depending
#   on pixel-perfect gold images. This is the regression check that
#   actually works without interactive gold capture.
#
# - `--mode pixels` (legacy): captures one screenshot at the canary's
#   frame ordinal and per-pixel-diffs it against the stored gold PNG
#   under `docs/apple-silicon/canary-baselines/`. Limited utility —
#   frame ordinals are not deterministic across cold boots and the
#   smoke input scripts cannot reach the menu game states the gold
#   images were captured at. See decision-log "2026-05-04 evening: W4
#   unconditional pass-flush fix + magenta investigation reclassified"
#   for the full analysis. Kept for use cases where the operator has
#   manually re-captured stable golds.
#
# - `--mode both`: runs counters first, then pixels. Both must pass.
#
# Project rule #11: this script ASSERTS the closed default-on Apple
# Silicon flags still produce green canary counters; it does NOT
# re-validate them. Counter-threshold failure means a Metal-side
# regression, not a flag-design question.
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
GOLD_DIR="${ROOT_DIR}/docs/apple-silicon/canary-baselines"
MANIFEST_TSV="${GOLD_DIR}/MANIFEST.tsv"

# Canary table — single source of truth.
#
# Each row is "<name>|<game-alias>|<input-relpath>|<frame>|<duration>|<gold-basename>".
# The counter-mode validation uses (name, game-alias, input, duration);
# the pixel-mode validation also uses (frame, gold-basename).
#
# Per-canary counter thresholds are encoded directly in counter_validate()
# below — they are derived from the empirical green-canary measurements
# in handoff.md "PGR2 PASS" / "Rainbow PASS" / "Halo PASS" /
# "boot/flubber PASS" bullets.
CANARY_TABLE='
pgr2|pgr2|pgr2-smoke.csv|900|90|pgr2/f900.png
rainbow|rainbow|rainbow-six-3-smoke.csv|600|75|rainbow/f600.png
halo|halo|noop.csv|1200|120|halo/f1200.png
boot|crimson|crimson-skies-smoke.csv|300|60|boot/f300.png
'

usage() {
    cat <<EOF
usage: $0 [--mode counters|pixels|both] [--canary <name>] [--threshold pct] [--out-dir <path>] [--help]

Single-renderer Metal canary regression gate (slice W3).

Two validation modes:

  --mode counters (DEFAULT)
      Validate last-interval xemu-perf: counters against per-canary
      thresholds (METAL_PIPELINE_TRANSLATED_FAILED == 0, coalescing
      ratio, drawable acquire failures, present rate, FPS). Autonomous-
      friendly: does NOT depend on pixel-perfect gold images. Catches
      concrete renderer regressions like W4's M5.7 coalescing defeat
      that pixel-diff would not surface (because pixel-diff also
      depends on the smoke recipe being able to reach the gold's game
      state, which the placeholder smoke scripts cannot).

  --mode pixels
      Capture one screenshot at the canary's frame ordinal and
      per-pixel-diff it against the stored gold PNG. Limited utility:
      gold images were captured interactively with profile HDD +
      gameplay scripts; the smoke recipe cannot reproduce the same
      game state, so the diff measures setup mismatch + frame-ordinal
      drift rather than renderer regression. See decision-log entry
      "W4 unconditional pass-flush fix + magenta investigation
      reclassified" for details.

  --mode both
      Run counters first, then pixels. Both must pass.

Other options:

  --canary <name>    Run only the named canary. Default: all four.
                     Names: pgr2 | rainbow | halo | boot.
  --threshold pct    Pixel-mode threshold (changed-pixels percentage).
                     Ignored in counters mode. Default 1.0.
  --out-dir <path>   Output directory. Default
                     benchmark-runs/<TS>-canary-regress/.
  --help             Print this usage.

Canary table (hard-coded):

  pgr2     -> input-scripts/pgr2-smoke.csv          duration 90s
  rainbow  -> input-scripts/rainbow-six-3-smoke.csv duration 75s
  halo     -> input-scripts/noop.csv                duration 120s
  boot     -> input-scripts/crimson-skies-smoke.csv duration 60s

Env recipe (verbatim from handoff.md "PGR2 PASS" bullet; project rule #11
forbids re-validating these — this script ASSERTS them):

  XEMU_RENDERER=METAL
  XEMU_METAL_TRANSLATED_PIPELINE=1
  XEMU_NATIVE_TRI_DEPTH=1
  XEMU_NATIVE_QUAD=1
  XEMU_PGRAPH_FAST_READ=1
  XEMU_METAL_FRONT_FB_FALLBACK=1
  XEMU_METAL_MSAA=4

Counter thresholds (per-canary, asserted in counters mode):

  All canaries:
    METAL_PIPELINE_TRANSLATED_FAILED == 0  (PSH/VSH translator works)
    METAL_DRAWABLE_ACQUIRE_FAILS    == 0  (CAMetalDrawable available)
    METAL_PIPELINE_FALLBACKS / METAL_DRAW_COUNT < 0.50  (passthrough rare)
    METAL_FRONT_FB_PUBLISHES > 0          (front-fb publish path active)
    METAL_DRAW_COUNT > 0                  (renderer is drawing)
    fps > 1.0                             (basic liveness)

  When METAL_DRAW_COUNT > 100 per interval (real drawing, not just boot):
    METAL_DRAW_PASS_COALESCED / METAL_DRAW_COUNT > 0.10
        (M5.7 render-pass coalescing not regressed; W4-style
        unconditional pass-flush would push this to 0. Active gameplay
        achieves 0.70-0.99 typically; Xbox boot animation can drop to
        0.20-0.30 due to many small isolated render passes.)

Exit codes:
  0  PASS — all selected canaries passed all selected modes
  1  FAIL — at least one canary regressed
  2  INFRA-FAIL — binary missing, run-benchmark.sh failure,
                  log/diff infrastructure failure
EOF
}

err() {
    echo "metal-canary-regress: $*" >&2
}

# --- argument parsing ------------------------------------------------------

CANARY_FILTER=""
THRESHOLD="1.0"
OUT_DIR=""
MODE="counters"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            usage
            exit 0
            ;;
        --mode)
            [[ $# -ge 2 ]] || { err "--mode requires a value"; exit 2; }
            MODE="$2"; shift 2 ;;
        --mode=*)
            MODE="${1#--mode=}"; shift ;;
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

case "$MODE" in
    counters|pixels|both) ;;
    *)
        err "unknown --mode '$MODE' (expected counters|pixels|both)"
        exit 2 ;;
esac

if [[ -n "$CANARY_FILTER" ]]; then
    case "$CANARY_FILTER" in
        pgr2|rainbow|halo|boot) ;;
        *)
            err "unknown canary '$CANARY_FILTER' (expected pgr2|rainbow|halo|boot)"
            exit 2 ;;
    esac
fi

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

if [[ ! -e "$RUN_BENCHMARK" ]]; then
    err "missing helper: $RUN_BENCHMARK"
    exit 2
fi

if [[ "$MODE" != "counters" && ! -e "$COMPARE_SCREENSHOTS" ]]; then
    err "missing helper: $COMPARE_SCREENSHOTS"
    exit 2
fi

# Verify input scripts exist; pixel mode also verifies golds.
canary_rows() {
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
    if [[ "$MODE" != "counters" ]]; then
        gold_path="${GOLD_DIR}/${gold}"
        if [[ ! -e "$gold_path" ]]; then
            err "missing gold PNG for canary '$name': $gold_path"
            PREFLIGHT_FAIL=1
        fi
    fi
done < <(canary_rows)

if [[ "$PREFLIGHT_FAIL" -ne 0 ]]; then
    err "pre-flight failed; aborting"
    exit 2
fi

# --- MANIFEST.tsv validation (F2; only required for pixel modes) ----------

declare -a MANIFEST_CANARIES=()
declare -a MANIFEST_COMMITS=()

if [[ "$MODE" != "counters" ]]; then
    if [[ ! -e "$MANIFEST_TSV" ]]; then
        err "missing MANIFEST.tsv: $MANIFEST_TSV"
        err "F2 requires the manifest to track gold provenance for pixel mode"
        exit 2
    fi

    manifest_cols="$(awk -F$'\t' 'NR==1 {print NF; exit}' "$MANIFEST_TSV")"
    if [[ "$manifest_cols" != "12" ]]; then
        err "MANIFEST.tsv header has $manifest_cols columns, expected 12"
        exit 2
    fi

    MANIFEST_FAIL=0
    while IFS=$'\t' read -r m_canary m_gold_path m_build_commit m_build_date \
            m_xemu_version m_gpu_family m_macos_version m_flags m_frame \
            m_source_run m_threshold_pct m_notes; do
        [[ -z "$m_canary" ]] && continue
        if [[ "$m_canary" == "canary" ]]; then continue; fi
        resolved="${GOLD_DIR}/${m_gold_path}"
        if [[ ! -e "$resolved" ]]; then
            err "MANIFEST row for canary '$m_canary' references missing gold: $resolved"
            MANIFEST_FAIL=1
            continue
        fi
        printf '[manifest] canary=%s build_commit=%s gold=%s\n' \
            "$m_canary" "$m_build_commit" "$m_gold_path"
        MANIFEST_CANARIES+=("$m_canary")
        MANIFEST_COMMITS+=("$m_build_commit")
    done < "$MANIFEST_TSV"

    if [[ "$MANIFEST_FAIL" -ne 0 ]]; then
        err "MANIFEST.tsv validation failed; aborting"
        exit 2
    fi
fi

# --- output directory ------------------------------------------------------

if [[ -z "$OUT_DIR" ]]; then
    STAMP="$(date +%Y%m%d-%H%M%S)"
    OUT_DIR="${RUN_ROOT}/${STAMP}-canary-regress"
fi

mkdir -p "$OUT_DIR"

LOG_FILE="$OUT_DIR/harness.log"
COUNTER_RESULTS_TSV="$OUT_DIR/counter-results.tsv"
PIXEL_RESULTS_TSV="$OUT_DIR/pixel-results.tsv"
: > "$COUNTER_RESULTS_TSV"
: > "$PIXEL_RESULTS_TSV"

log() {
    printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG_FILE"
}

log "metal-canary-regress starting"
log "  mode          = $MODE"
log "  canary_filter = ${CANARY_FILTER:-<all>}"
log "  threshold     = ${THRESHOLD}%"
log "  out_dir       = $OUT_DIR"

# --- helpers ---------------------------------------------------------------

# Read the displayed PNG dimensions and synthesize "0,0,W,H" for
# compare-screenshots.py.
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

# Extract a counter value from the LAST `xemu-perf: interval_id=...`
# line in the run's xemu.log. The xemu-perf interval line carries
# space-separated key=value pairs; we want the value of the requested
# key from the latest interval.
last_interval_counter() {
    local log_file="$1"
    local key="$2"
    # Grep only interval-id lines, take last, then awk-parse.
    awk -v k="$key" '
        /^xemu-perf: interval_id=/ { last = $0 }
        END {
            if (!last) { print ""; exit }
            n = split(last, fields, " ")
            for (i = 1; i <= n; i++) {
                if (index(fields[i], k "=") == 1) {
                    sub(k "=", "", fields[i])
                    print fields[i]
                    exit
                }
            }
            print ""
        }' "$log_file"
}

# Float comparison via awk (bash 3.2 has no float).
gt() {
    awk -v a="$1" -v b="$2" 'BEGIN { exit !(a+0 > b+0) }'
}
lt() {
    awk -v a="$1" -v b="$2" 'BEGIN { exit !(a+0 < b+0) }'
}
ge() {
    awk -v a="$1" -v b="$2" 'BEGIN { exit !(a+0 >= b+0) }'
}
eq_zero() {
    awk -v a="$1" 'BEGIN { exit !(a+0 == 0) }'
}

# Counter-mode validation. Reads xemu.log for the last interval's
# counter values and validates them against the canary thresholds.
# Returns a verdict string ("PASS" / "FAIL: <reason>") via stdout.
counter_validate() {
    local log_file="$1"

    local translated_failed coalesced draws drawable_fails fps fallbacks publishes
    translated_failed="$(last_interval_counter "$log_file" METAL_PIPELINE_TRANSLATED_FAILED)"
    coalesced="$(last_interval_counter "$log_file" METAL_DRAW_PASS_COALESCED)"
    draws="$(last_interval_counter "$log_file" METAL_DRAW_COUNT)"
    drawable_fails="$(last_interval_counter "$log_file" METAL_DRAWABLE_ACQUIRE_FAILS)"
    fps="$(last_interval_counter "$log_file" fps)"
    fallbacks="$(last_interval_counter "$log_file" METAL_PIPELINE_FALLBACKS)"
    publishes="$(last_interval_counter "$log_file" METAL_FRONT_FB_PUBLISHES)"

    # Empty values default to 0 except where a positive value is required.
    : "${translated_failed:=0}"
    : "${coalesced:=0}"
    : "${draws:=0}"
    : "${drawable_fails:=0}"
    : "${fps:=0}"
    : "${fallbacks:=0}"
    : "${publishes:=0}"

    # Echo per-counter values for downstream parsers.
    printf 'METAL_PIPELINE_TRANSLATED_FAILED=%s\n' "$translated_failed"
    printf 'METAL_DRAW_PASS_COALESCED=%s\n' "$coalesced"
    printf 'METAL_DRAW_COUNT=%s\n' "$draws"
    printf 'METAL_DRAWABLE_ACQUIRE_FAILS=%s\n' "$drawable_fails"
    printf 'fps=%s\n' "$fps"
    printf 'METAL_PIPELINE_FALLBACKS=%s\n' "$fallbacks"
    printf 'METAL_FRONT_FB_PUBLISHES=%s\n' "$publishes"

    # Validation rules.
    if ! eq_zero "$translated_failed"; then
        printf 'verdict=FAIL\n'
        printf 'reason=METAL_PIPELINE_TRANSLATED_FAILED=%s != 0 (PSH/VSH translator regressed)\n' \
            "$translated_failed"
        return
    fi
    if ! eq_zero "$drawable_fails"; then
        printf 'verdict=FAIL\n'
        printf 'reason=METAL_DRAWABLE_ACQUIRE_FAILS=%s != 0 (CAMetalDrawable starved)\n' \
            "$drawable_fails"
        return
    fi
    if ! gt "$publishes" "0"; then
        printf 'verdict=FAIL\n'
        printf 'reason=METAL_FRONT_FB_PUBLISHES=%s == 0 (front-fb publish path inactive)\n' \
            "$publishes"
        return
    fi
    if ! gt "$draws" "0"; then
        printf 'verdict=FAIL\n'
        printf 'reason=METAL_DRAW_COUNT=%s == 0 (renderer not drawing)\n' \
            "$draws"
        return
    fi
    if ! gt "$fps" "1.0"; then
        printf 'verdict=FAIL\n'
        printf 'reason=fps=%s <= 1.0 (renderer stalled)\n' \
            "$fps"
        return
    fi
    # Pipeline-fallback ratio: passthrough should be rare.
    if gt "$draws" "0"; then
        local fallback_ratio
        fallback_ratio="$(awk -v f="$fallbacks" -v d="$draws" 'BEGIN { print (d>0)?f/d:0 }')"
        if gt "$fallback_ratio" "0.50"; then
            printf 'verdict=FAIL\n'
            printf 'reason=METAL_PIPELINE_FALLBACKS/METAL_DRAW_COUNT=%s > 0.50 (translated path collapsed)\n' \
                "$fallback_ratio"
            return
        fi
    fi
    # Coalescing ratio: only check when drawing is non-trivial. Below
    # 100 draws/interval is typically idle/post-boot and coalescing
    # ratio is noisy because most draws are isolated.
    #
    # Threshold rationale: the W4 unconditional pass-flush regression
    # collapsed coalescing to ~0.0 across every workload (every guest
    # draw forced its own pass). Active gameplay (PGR2 / Rainbow /
    # Halo menu) typically achieves > 0.70 coalescing post-fix; the
    # Xbox boot animation can be as low as 0.20-0.30 because the boot
    # disc issues many small isolated render passes. 0.10 is the
    # threshold that catches the W4-style total-collapse regression
    # while accommodating boot's naturally-isolated draw pattern.
    if gt "$draws" "100"; then
        local coal_ratio
        coal_ratio="$(awk -v c="$coalesced" -v d="$draws" 'BEGIN { print (d>0)?c/d:0 }')"
        if lt "$coal_ratio" "0.10"; then
            printf 'verdict=FAIL\n'
            printf 'reason=METAL_DRAW_PASS_COALESCED/METAL_DRAW_COUNT=%s < 0.10 (M5.7 coalescing regressed)\n' \
                "$coal_ratio"
            return
        fi
    fi
    printf 'verdict=PASS\n'
    printf 'reason=all counter thresholds met\n'
}

# --- per-canary run --------------------------------------------------------

run_canary_counters() {
    local name="$1"
    local game="$2"
    local input="$3"
    local duration="$4"

    local canary_dir="$OUT_DIR/$name"
    mkdir -p "$canary_dir"
    local launcher_log="$canary_dir/launcher.log"

    local input_arg=""
    if [[ -n "$input" ]]; then
        input_arg="${INPUT_SCRIPT_DIR}/${input}"
    fi

    log "[$name][counters] starting Metal canary run"
    log "  game     = $game"
    log "  input    = ${input_arg:-<launcher default>}"
    log "  duration = ${duration}s"

    set +e
    XEMU_RENDERER=METAL \
    XEMU_METAL_TRANSLATED_PIPELINE=1 \
    XEMU_NATIVE_TRI_DEPTH=1 \
    XEMU_NATIVE_QUAD=1 \
    XEMU_PGRAPH_FAST_READ=1 \
    XEMU_METAL_FRONT_FB_FALLBACK=1 \
    XEMU_METAL_MSAA=4 \
    XEMU_BENCH_SCREENSHOT_BACKEND=none \
        "$RUN_BENCHMARK" \
            --metal-no-hud \
            "$game" "$input_arg" "$duration" \
        > "$launcher_log" 2>&1
    local rc=$?
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

    local xemu_log="$run_dir/xemu.log"
    if [[ ! -e "$xemu_log" ]]; then
        err "[$name] missing xemu.log: $xemu_log"
        return 2
    fi

    # Run counter validation; capture all output.
    local validate_out="$canary_dir/counter-validate.txt"
    counter_validate "$xemu_log" > "$validate_out"

    local verdict reason
    verdict="$(parse_kv verdict "$validate_out")"
    reason="$(parse_kv reason "$validate_out")"

    # Capture key counter values for the report.
    local translated_failed coalesced draws drawable_fails fps fallbacks publishes
    translated_failed="$(parse_kv METAL_PIPELINE_TRANSLATED_FAILED "$validate_out")"
    coalesced="$(parse_kv METAL_DRAW_PASS_COALESCED "$validate_out")"
    draws="$(parse_kv METAL_DRAW_COUNT "$validate_out")"
    drawable_fails="$(parse_kv METAL_DRAWABLE_ACQUIRE_FAILS "$validate_out")"
    fps="$(parse_kv fps "$validate_out")"
    fallbacks="$(parse_kv METAL_PIPELINE_FALLBACKS "$validate_out")"
    publishes="$(parse_kv METAL_FRONT_FB_PUBLISHES "$validate_out")"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$verdict" "$translated_failed" "$coalesced" "$draws" \
        "$drawable_fails" "$fps" "$fallbacks" "$publishes" \
        "$run_dir" "$reason" \
        >> "$COUNTER_RESULTS_TSV"

    log "[$name][counters] $verdict translated_failed=$translated_failed coalesced=$coalesced draws=$draws fps=$fps publishes=$publishes"
    if [[ "$verdict" != "PASS" ]]; then
        log "[$name][counters] $reason"
    fi
    return 0
}

run_canary_pixels() {
    local name="$1"
    local game="$2"
    local input="$3"
    local frame="$4"
    local duration="$5"
    local gold="$6"

    local canary_dir="$OUT_DIR/$name"
    mkdir -p "$canary_dir"
    local launcher_log="$canary_dir/pixel-launcher.log"
    local shot_path="$canary_dir/screenshot.png"
    local gold_path="${GOLD_DIR}/${gold}"

    local input_arg=""
    if [[ -n "$input" ]]; then
        input_arg="${INPUT_SCRIPT_DIR}/${input}"
    fi

    log "[$name][pixels] starting Metal canary run"

    set +e
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
    local rc=$?
    set -e

    if [[ $rc -ne 0 ]]; then
        err "[$name][pixels] run-benchmark.sh failed with status $rc; see $launcher_log"
        return 2
    fi

    local run_dir
    run_dir="$(awk -F': ' '/^Run directory: / { print $2 }' "$launcher_log" | tail -n 1)"
    if [[ -z "$run_dir" || ! -d "$run_dir" ]]; then
        err "[$name][pixels] could not parse run directory from $launcher_log"
        return 2
    fi

    if [[ ! -e "$shot_path" ]]; then
        local fallback
        fallback="$(find "$canary_dir" -maxdepth 1 -name 'screenshot*.png' -type f 2>/dev/null | sort | head -n 1 || true)"
        if [[ -n "$fallback" && -e "$fallback" ]]; then
            shot_path="$fallback"
        else
            err "[$name][pixels] expected screenshot at $shot_path but none found"
            return 2
        fi
    fi

    local diff_dir="$canary_dir/diff"
    mkdir -p "$diff_dir"
    local crop
    if ! crop="$(default_crop_for "$gold_path" 2>"$diff_dir/crop-err.txt")"; then
        err "[$name][pixels] could not read gold PNG dimensions: $gold_path"
        return 2
    fi

    local stdout_file="$diff_dir/compare-stdout.txt"
    set +e
    "$COMPARE_SCREENSHOTS" \
        "$gold_path" "$shot_path" \
        --crop "$crop" \
        --resize smaller \
        --out-dir "$diff_dir" \
        > "$stdout_file" 2>&1
    local cmp_rc=$?
    set -e
    if [[ $cmp_rc -ne 0 ]]; then
        err "[$name][pixels] compare-screenshots.py failed (rc=$cmp_rc); see $stdout_file"
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
        >> "$PIXEL_RESULTS_TSV"

    log "[$name][pixels] $verdict changed_pct=$changed_pct mae=$mae rms=$rms"
    return 0
}

# --- top-level orchestration ----------------------------------------------

INFRA_FAIL=0

if [[ "$MODE" == "counters" || "$MODE" == "both" ]]; then
    log "=== counter-mode run starting ==="
    while IFS='|' read -r name game input frame duration gold; do
        [[ -z "$name" ]] && continue
        if ! run_canary_counters "$name" "$game" "$input" "$duration"; then
            INFRA_FAIL=1
        fi
    done < <(canary_rows)
fi

if [[ "$MODE" == "pixels" || "$MODE" == "both" ]]; then
    log "=== pixel-mode run starting ==="
    while IFS='|' read -r name game input frame duration gold; do
        [[ -z "$name" ]] && continue
        if ! run_canary_pixels "$name" "$game" "$input" "$frame" "$duration" "$gold"; then
            INFRA_FAIL=1
        fi
    done < <(canary_rows)
fi

if [[ "$INFRA_FAIL" -ne 0 ]]; then
    err "infrastructure failure during canary execution; partial results in $OUT_DIR"
    exit 2
fi

# Determine overall PASS/FAIL.
PASS=1
FAIL_LINES=()

if [[ "$MODE" == "counters" || "$MODE" == "both" ]]; then
    while IFS=$'\t' read -r name verdict tf coal draws daf fps fb pub run_dir reason; do
        [[ -z "$name" ]] && continue
        if [[ "$verdict" == "FAIL" ]]; then
            PASS=0
            FAIL_LINES+=("counters: canary $name: $reason")
        fi
    done < "$COUNTER_RESULTS_TSV"
fi

if [[ "$MODE" == "pixels" || "$MODE" == "both" ]]; then
    while IFS=$'\t' read -r name verdict changed_pct mae rms max_abs shot gold run_dir; do
        [[ -z "$name" ]] && continue
        if [[ "$verdict" == "FAIL" ]]; then
            PASS=0
            FAIL_LINES+=("pixels: canary $name: changed_pixels_pct=$changed_pct > threshold=$THRESHOLD")
        fi
    done < "$PIXEL_RESULTS_TSV"
fi

REPORT_MD="$OUT_DIR/report.md"
SUMMARY_JSON="$OUT_DIR/summary.json"

verdict_total="PASS"
if [[ "$PASS" -eq 0 ]]; then verdict_total="FAIL"; fi

{
    printf -- '# %s — metal-canary-regress report\n\n' "$verdict_total"
    printf -- '- mode: %s\n' "$MODE"
    printf -- '- canary filter: %s\n' "${CANARY_FILTER:-<all four>}"
    printf -- '- pixel-mode threshold: %s%% changed-pixels per canary\n' "$THRESHOLD"
    printf -- '- env recipe: XEMU_RENDERER=METAL XEMU_METAL_TRANSLATED_PIPELINE=1 XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1 XEMU_METAL_FRONT_FB_FALLBACK=1 XEMU_METAL_MSAA=4\n'
    printf -- '- metal_hud: off (--metal-no-hud)\n'
    printf -- '- metal_validation: auto-on (W1)\n'
    printf -- '- harness log: %s\n' "$LOG_FILE"
    if [[ "$MODE" == "counters" || "$MODE" == "both" ]]; then
        printf '\n## Counter-mode results\n\n'
        printf '| canary | verdict | translated_failed | draw_pass_coalesced | draw_count | drawable_fails | fps | pipeline_fallbacks | front_fb_publishes | run_dir |\n'
        printf '|--------|---------|------------------:|--------------------:|-----------:|---------------:|----:|-------------------:|-------------------:|---------|\n'
        while IFS=$'\t' read -r name verdict tf coal draws daf fps fb pub run_dir reason; do
            [[ -z "$name" ]] && continue
            printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
                "$name" "$verdict" "$tf" "$coal" "$draws" "$daf" "$fps" \
                "$fb" "$pub" "$run_dir"
        done < "$COUNTER_RESULTS_TSV"
    fi
    if [[ "$MODE" == "pixels" || "$MODE" == "both" ]]; then
        printf '\n## Pixel-mode results\n\n'
        printf '| canary | verdict | changed_pct | mae | rms | max_abs | screenshot | gold | run_dir |\n'
        printf '|--------|---------|------------:|----:|----:|--------:|------------|------|---------|\n'
        while IFS=$'\t' read -r name verdict changed_pct mae rms max_abs shot gold run_dir; do
            [[ -z "$name" ]] && continue
            printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
                "$name" "$verdict" "$changed_pct" "$mae" "$rms" "$max_abs" \
                "$shot" "$gold" "$run_dir"
        done < "$PIXEL_RESULTS_TSV"
    fi
    if [[ "$PASS" -eq 0 ]]; then
        printf '\n## Failures\n\n'
        for line in "${FAIL_LINES[@]}"; do
            printf -- '- %s\n' "$line"
        done
    fi
    if [[ ${#MANIFEST_CANARIES[@]} -gt 0 ]]; then
        printf '\n## Manifest provenance\n\n'
        printf '_Source: `docs/apple-silicon/canary-baselines/MANIFEST.tsv`_\n\n'
        printf '| canary | build_commit |\n'
        printf '|--------|--------------|\n'
        i=0
        while [[ $i -lt ${#MANIFEST_CANARIES[@]} ]]; do
            printf '| %s | %s |\n' "${MANIFEST_CANARIES[$i]}" "${MANIFEST_COMMITS[$i]}"
            i=$((i + 1))
        done
    fi
    printf '\n_Generated by `scripts/apple-silicon/metal-canary-regress.sh` (slice W3, counter mode 2026-05-04 evening)._\n'
} > "$REPORT_MD"

# summary.json
{
    printf '{\n'
    printf '  "pass": %s,\n' "$([[ $PASS -eq 1 ]] && echo true || echo false)"
    printf '  "verdict": "%s",\n' "$verdict_total"
    printf '  "mode": "%s",\n' "$MODE"
    printf '  "threshold": %s,\n' "$THRESHOLD"
    printf '  "canary_filter": "%s",\n' "${CANARY_FILTER:-all}"
    printf '  "metal_hud": "off",\n'
    printf '  "metal_validation": "auto-on",\n'
    if [[ ${#MANIFEST_CANARIES[@]} -gt 0 ]]; then
        printf '  "manifest": {\n'
        mfirst=1
        mi=0
        while [[ $mi -lt ${#MANIFEST_CANARIES[@]} ]]; do
            if [[ $mfirst -eq 1 ]]; then mfirst=0; else printf ',\n'; fi
            printf '    "%s": { "build_commit": "%s" }' \
                "${MANIFEST_CANARIES[$mi]}" "${MANIFEST_COMMITS[$mi]}"
            mi=$((mi + 1))
        done
        printf '\n  },\n'
    fi
    if [[ "$MODE" == "counters" || "$MODE" == "both" ]]; then
        printf '  "counter_canaries": [\n'
        first=1
        while IFS=$'\t' read -r name verdict tf coal draws daf fps fb pub run_dir reason; do
            [[ -z "$name" ]] && continue
            if [[ $first -eq 1 ]]; then first=0; else printf ',\n'; fi
            printf '    { "name": "%s", "verdict": "%s", "translated_failed": %s, "draw_pass_coalesced": %s, "draw_count": %s, "drawable_acquire_fails": %s, "fps": %s, "pipeline_fallbacks": %s, "front_fb_publishes": %s, "run_dir": "%s" }' \
                "$name" "$verdict" "${tf:-0}" "${coal:-0}" "${draws:-0}" \
                "${daf:-0}" "${fps:-0}" "${fb:-0}" "${pub:-0}" "$run_dir"
        done < "$COUNTER_RESULTS_TSV"
        printf '\n  ]'
        if [[ "$MODE" == "both" ]]; then printf ',\n'; else printf '\n'; fi
    fi
    if [[ "$MODE" == "pixels" || "$MODE" == "both" ]]; then
        printf '  "pixel_canaries": [\n'
        first=1
        while IFS=$'\t' read -r name verdict changed_pct mae rms max_abs shot gold run_dir; do
            [[ -z "$name" ]] && continue
            if [[ $first -eq 1 ]]; then first=0; else printf ',\n'; fi
            printf '    { "name": "%s", "verdict": "%s", "changed_pct": %s, "mae": %s, "rms": %s, "max_abs": %s, "screenshot": "%s", "gold": "%s", "run_dir": "%s" }' \
                "$name" "$verdict" "$changed_pct" "$mae" "$rms" "$max_abs" \
                "$shot" "$gold" "$run_dir"
        done < "$PIXEL_RESULTS_TSV"
        printf '\n  ]\n'
    else
        printf '\n'
    fi
    printf '}\n'
} > "$SUMMARY_JSON"

log "wrote $REPORT_MD"
log "wrote $SUMMARY_JSON"
log "verdict: $verdict_total"

if [[ "$PASS" -eq 1 ]]; then
    exit 0
fi
exit 1
