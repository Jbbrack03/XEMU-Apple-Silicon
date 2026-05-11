#!/usr/bin/env bash
# Compare two benchmark runs side-by-side on FPS and jitter metrics.
#
# Usage:
#   compare-runs.sh BASELINE_RUN_DIR CANDIDATE_RUN_DIR [POST_LOAD_SKIP]
#
# Prints a markdown-friendly table comparing the two runs and a verdict
# block ("regression" / "improvement" / "noise") for each metric. Exits
# nonzero if the candidate regresses on key metrics by more than NOISE_PCT.

set -euo pipefail

NOISE_PCT="${NOISE_PCT:-3}"

usage() {
    cat <<EOF
usage: $0 BASELINE_RUN_DIR CANDIDATE_RUN_DIR [POST_LOAD_SKIP]

Compares post-load FPS, mspf percentiles, and stutter counts between two
benchmark runs. NOISE_PCT (default 3) sets the regression threshold for
metrics where smaller is better (mspf, stutter counts). FPS uses the same
threshold inverted (improvement if higher).

Exit code:
  0 = no regression beyond NOISE_PCT
  1 = candidate regresses on FPS or any jitter metric beyond NOISE_PCT
  2 = usage error
EOF
}

if [[ $# -lt 2 || $# -gt 3 ]]; then usage >&2; exit 2; fi

BASE="$1"; CAND="$2"; SKIP="${3:-5}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXTRACT="${ROOT_DIR}/scripts/apple-silicon/extract-perf-summary.sh"

if [[ ! -x "$EXTRACT" ]]; then
    echo "missing $EXTRACT" >&2
    exit 1
fi

base_summary="$(mktemp -t compare-runs-base.XXXXXX)"
cand_summary="$(mktemp -t compare-runs-cand.XXXXXX)"
trap 'rm -f "$base_summary" "$cand_summary"' EXIT

"$EXTRACT" "$BASE" "$SKIP" > "$base_summary"
"$EXTRACT" "$CAND" "$SKIP" > "$cand_summary"

get() {
    local file="$1"
    local key="$2"
    awk -F= -v k="$key" '$1==k {print $2; exit}' "$file"
}

# Metrics: name, "higher_is_better"
metrics=(
  "post_load_avg_fps higher"
  "post_load_avg_mspf lower"
  "post_load_fps_stddev lower"
  "post_load_mspf_max_p50 lower"
  "post_load_mspf_max_p95 lower"
  "post_load_mspf_max_p99 lower"
  "post_load_mspf_max_max lower"
  "post_load_stutter_intervals_30fps lower"
  "post_load_stutter_intervals_45fps lower"
  "post_load_stutter_intervals_60fps lower"
  "post_load_longest_stutter_run_30fps lower"
  "post_load_longest_stutter_run_60fps lower"
)

echo "Comparison: BASELINE=$BASE  CANDIDATE=$CAND  noise_threshold=${NOISE_PCT}%"
echo
printf "%-40s | %12s | %12s | %10s | %s\n" "metric" "baseline" "candidate" "delta_pct" "verdict"
printf "%-40s-+-%-12s-+-%-12s-+-%-10s-+-%s\n" "----------------------------------------" "------------" "------------" "----------" "-------"

regressed=0
missing=0
for entry in "${metrics[@]}"; do
    name="${entry%% *}"
    direction="${entry##* }"
    bv="$(get "$base_summary" "$name")"
    cv="$(get "$cand_summary" "$name")"
    if [[ -z "$bv" || -z "$cv" ]]; then
        printf "%-40s | %12s | %12s | %10s | %s\n" "$name" "${bv:-?}" "${cv:-?}" "?" "missing"
        missing=1
        continue
    fi
    # Compute percent delta with awk (handles floats and zero baseline).
    delta_pct=$(awk -v b="$bv" -v c="$cv" 'BEGIN {
        if (b+0 == 0 && c+0 == 0) { print "0.0"; exit }
        if (b+0 == 0) { print "+inf"; exit }
        printf("%+.2f", (c - b) / b * 100)
    }')
    verdict=""
    if [[ "$delta_pct" == "+inf" ]]; then
        verdict="N/A(zero-base)"
    else
        # Strip sign for comparison
        abs_pct=${delta_pct#+}; abs_pct=${abs_pct#-}
        if awk -v a="$abs_pct" -v t="$NOISE_PCT" 'BEGIN { exit !(a+0 < t+0) }'; then
            verdict="noise"
        else
            # Determine improvement vs regression based on direction
            sign_negative=0
            case "$delta_pct" in -*) sign_negative=1 ;; esac
            if [[ "$direction" == "higher" ]]; then
                if (( sign_negative )); then verdict="regression"; regressed=1
                else verdict="improvement"; fi
            else
                if (( sign_negative )); then verdict="improvement"
                else verdict="regression"; regressed=1; fi
            fi
        fi
    fi
    printf "%-40s | %12s | %12s | %10s | %s\n" "$name" "$bv" "$cv" "$delta_pct" "$verdict"
done

echo
if (( missing )); then
    echo "VERDICT: missing metrics; benchmark interval data is incomplete"
    exit 1
elif (( regressed )); then
    echo "VERDICT: candidate regresses on at least one metric beyond ${NOISE_PCT}%"
    exit 1
else
    echo "VERDICT: no regression beyond ${NOISE_PCT}% noise threshold"
fi
