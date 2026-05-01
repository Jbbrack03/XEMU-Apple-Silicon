#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<EOF
usage: $0 RUN_DIR|xemu.log [post-load-skip-intervals]

Summarizes xemu-perf interval lines from an Apple Silicon benchmark run.
The default post-load summary skips the first five intervals.

Outputs averaged FPS / MSPF, NV2A counters, and jitter metrics derived from
the per-interval mspf_min/max/avg fields. Jitter metrics:

  fps_stddev                  stddev of fps across timed intervals
  mspf_max_p50/95/99/max      percentile of per-interval worst-frame MSPF
  mspf_avg_max                worst per-interval mean MSPF
  stutter_intervals_30fps     count of intervals where mspf_max > 33.3
  stutter_intervals_45fps     count of intervals where mspf_max > 22.2
  stutter_intervals_60fps     count of intervals where mspf_max > 16.7
  longest_stutter_run_30fps   longest contiguous run of >33.3-ms intervals
  longest_stutter_run_60fps   longest contiguous run of >16.7-ms intervals

Jitter metrics are also emitted as post_load_* variants over the same
post-load window used for the FPS/MSPF averages.
EOF
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

INPUT="$1"
SKIP="${2:-5}"

if [[ -d "$INPUT" ]]; then
    LOG_FILE="${INPUT%/}/xemu.log"
else
    LOG_FILE="$INPUT"
fi

if [[ ! -f "$LOG_FILE" ]]; then
    echo "missing xemu log: $LOG_FILE" >&2
    exit 1
fi

awk -v skip="$SKIP" '
function add_counter(name, value) {
    counters[name] += value
}

# Track per-interval values for jitter metrics.
function record_interval(fps, mspf_avg, mspf_max, is_post_load) {
    all_count++
    all_fps[all_count] = fps
    all_mspf_max[all_count] = mspf_max
    all_mspf_avg[all_count] = mspf_avg

    if (is_post_load) {
        post_count++
        post_fps[post_count] = fps
        post_mspf_max[post_count] = mspf_max
        post_mspf_avg[post_count] = mspf_avg
    }
}

# Sort an array of length n in ascending order (insertion sort; n is small).
function isort(arr, n,    i, j, key) {
    for (i = 2; i <= n; i++) {
        key = arr[i]
        j = i - 1
        while (j >= 1 && arr[j] > key) {
            arr[j+1] = arr[j]
            j--
        }
        arr[j+1] = key
    }
}

# Return the value at percentile p (0..100) of sorted array sorted[1..n].
# Uses nearest-rank for clarity at small n.
function percentile(sorted, n, p,   rank) {
    if (n == 0) return 0
    rank = int((p / 100.0) * n + 0.5)
    if (rank < 1) rank = 1
    if (rank > n) rank = n
    return sorted[rank]
}

# Compute stddev of arr[1..n].
function stddev(arr, n,   i, sum, mean, sq) {
    if (n < 2) return 0
    sum = 0
    for (i = 1; i <= n; i++) sum += arr[i]
    mean = sum / n
    sq = 0
    for (i = 1; i <= n; i++) sq += (arr[i] - mean) * (arr[i] - mean)
    return sqrt(sq / (n - 1))
}

# Count entries in arr[1..n] strictly greater than threshold.
function count_above(arr, n, threshold,   i, c) {
    c = 0
    for (i = 1; i <= n; i++) if (arr[i] > threshold) c++
    return c
}

# Longest contiguous run in arr[1..n] of entries strictly greater than threshold.
function longest_run_above(arr, n, threshold,   i, run, best) {
    run = 0; best = 0
    for (i = 1; i <= n; i++) {
        if (arr[i] > threshold) {
            run++
            if (run > best) best = run
        } else {
            run = 0
        }
    }
    return best
}

# Emit jitter metrics for an array of length n with prefix.
function emit_jitter(prefix, fps_arr, mspf_max_arr, mspf_avg_arr, n,    sorted, i, max_avg) {
    if (n == 0) return

    printf("%sfps_stddev=%.3f\n", prefix, stddev(fps_arr, n))

    for (i = 1; i <= n; i++) sorted[i] = mspf_max_arr[i]
    isort(sorted, n)
    printf("%smspf_max_p50=%.2f\n", prefix, percentile(sorted, n, 50))
    printf("%smspf_max_p95=%.2f\n", prefix, percentile(sorted, n, 95))
    printf("%smspf_max_p99=%.2f\n", prefix, percentile(sorted, n, 99))
    printf("%smspf_max_max=%.2f\n", prefix, sorted[n])

    max_avg = 0
    for (i = 1; i <= n; i++) {
        if (mspf_avg_arr[i] > max_avg) max_avg = mspf_avg_arr[i]
    }
    printf("%smspf_avg_max=%.2f\n", prefix, max_avg)

    printf("%sstutter_intervals_30fps=%d\n", prefix, count_above(mspf_max_arr, n, 33.3))
    printf("%sstutter_intervals_45fps=%d\n", prefix, count_above(mspf_max_arr, n, 22.2))
    printf("%sstutter_intervals_60fps=%d\n", prefix, count_above(mspf_max_arr, n, 16.7))
    printf("%slongest_stutter_run_30fps=%d\n", prefix, longest_run_above(mspf_max_arr, n, 33.3))
    printf("%slongest_stutter_run_60fps=%d\n", prefix, longest_run_above(mspf_max_arr, n, 16.7))
}

/xemu-perf: interval_ms=/ {
    intervals++
    interval_fps = 0
    interval_mspf = 0
    interval_mspf_max = 0
    interval_is_final = 0

    for (i = 1; i <= NF; i++) {
        split($i, field, "=")
        if (length(field[2]) == 0) {
            continue
        }

        key = field[1]
        value = field[2] + 0

        if (key == "fps") {
            interval_fps = value
        } else if (key == "mspf_avg") {
            interval_mspf = value
        } else if (key == "mspf_max") {
            interval_mspf_max = value
        } else if (key == "final") {
            interval_is_final = value
        } else if (key == "GEOM_SHADER_MODULE_GEN" ||
                   key == "GEOM_SHADER_PROGRAM_GEN" ||
                   key == "GEOM_SHADER_BIND" ||
                   key == "GEOM_SHADER_BIND_NOTDIRTY" ||
                   key == "GEOM_SHADER_DRAW" ||
                   key == "GEOM_SHADER_DRAW_LINE" ||
                   key == "GEOM_SHADER_DRAW_TRI" ||
                   key == "GEOM_SHADER_DRAW_QUAD" ||
                   key == "GEOM_SHADER_DRAW_QUAD_LIST" ||
                   key == "GEOM_SHADER_DRAW_QUAD_STRIP" ||
                   key == "GEOM_SHADER_DRAW_OTHER" ||
                   key == "NATIVE_TRI_DEPTH_DRAW" ||
                   key == "NATIVE_TRI_DEPTH_CANDIDATE" ||
                   key == "NATIVE_TRI_DEPTH_CANDIDATE_SMOOTH" ||
                   key == "NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST" ||
                   key == "NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST" ||
                   key == "NATIVE_TRI_DEPTH_FALLBACK" ||
                   key == "NATIVE_TRI_DEPTH_DRAW_ZPERSPECTIVE" ||
                   key == "NATIVE_TRI_DEPTH_DRAW_LINEAR_Z" ||
                   key == "NATIVE_TRI_DEPTH_DRAW_POLY_OFFSET" ||
                   key == "NATIVE_TRI_DEPTH_DRAW_SMOOTH" ||
                   key == "NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST" ||
                   key == "NATIVE_TRI_DEPTH_FALLBACK_FLAT" ||
                   key == "NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST" ||
                   key == "NATIVE_QUAD_DRAW" ||
                   key == "NATIVE_QUAD_DRAW_LIST" ||
                   key == "NATIVE_QUAD_DRAW_STRIP" ||
                   key == "NATIVE_QUAD_CANDIDATE" ||
                   key == "NATIVE_QUAD_CANDIDATE_SMOOTH" ||
                   key == "NATIVE_QUAD_CANDIDATE_FLAT" ||
                   key == "NATIVE_QUAD_FALLBACK" ||
                   key == "NATIVE_QUAD_FALLBACK_FLAT" ||
                   key == "NATIVE_QUAD_FALLBACK_NONFILL" ||
                   key == "NATIVE_QUAD_DRAW_POLY_OFFSET" ||
                   key == "NATIVE_QUAD_DRAW_ZPERSPECTIVE" ||
                   key == "NATIVE_QUAD_DRAW_LINEAR_Z" ||
                   key == "SHADER_GEN" ||
                   key == "SHADER_BIND" ||
                   key == "BEGIN_ENDS" ||
                   key == "DRAW_ARRAYS" ||
                   key == "INLINE_ELEMENTS" ||
                   key == "INLINE_ARRAYS" ||
                   key == "INLINE_BUFFERS") {
            add_counter(key, value)
        }
    }

    if (interval_is_final) {
        final_intervals++
        next
    }

    timed_intervals++
    fps_total += interval_fps
    mspf_total += interval_mspf

    is_post = (timed_intervals > skip)
    if (is_post) {
        post_intervals++
        post_fps_total += interval_fps
        post_mspf_total += interval_mspf
    }

    record_interval(interval_fps, interval_mspf, interval_mspf_max, is_post)
}

END {
    if (intervals == 0) {
        print "no xemu-perf interval lines found" > "/dev/stderr"
        exit 1
    }

    printf("log_file=%s\n", FILENAME)
    printf("intervals=%d\n", intervals)
    printf("timed_intervals=%d\n", timed_intervals)
    printf("final_intervals=%d\n", final_intervals)
    if (timed_intervals > 0) {
        printf("avg_fps=%.2f\n", fps_total / timed_intervals)
        printf("avg_mspf=%.2f\n", mspf_total / timed_intervals)
    }
    printf("post_load_skip_intervals=%d\n", skip)
    printf("post_load_intervals=%d\n", post_intervals)

    if (post_intervals > 0) {
        printf("post_load_avg_fps=%.2f\n", post_fps_total / post_intervals)
        printf("post_load_avg_mspf=%.2f\n", post_mspf_total / post_intervals)
    }

    keys[1] = "GEOM_SHADER_MODULE_GEN"
    keys[2] = "GEOM_SHADER_PROGRAM_GEN"
    keys[3] = "GEOM_SHADER_BIND"
    keys[4] = "GEOM_SHADER_BIND_NOTDIRTY"
    keys[5] = "GEOM_SHADER_DRAW"
    keys[6] = "GEOM_SHADER_DRAW_LINE"
    keys[7] = "GEOM_SHADER_DRAW_TRI"
    keys[8] = "GEOM_SHADER_DRAW_QUAD"
    keys[9] = "GEOM_SHADER_DRAW_QUAD_LIST"
    keys[10] = "GEOM_SHADER_DRAW_QUAD_STRIP"
    keys[11] = "GEOM_SHADER_DRAW_OTHER"
    keys[12] = "NATIVE_TRI_DEPTH_DRAW"
    keys[13] = "NATIVE_TRI_DEPTH_CANDIDATE"
    keys[14] = "NATIVE_TRI_DEPTH_CANDIDATE_SMOOTH"
    keys[15] = "NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST"
    keys[16] = "NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST"
    keys[17] = "NATIVE_TRI_DEPTH_FALLBACK"
    keys[18] = "NATIVE_TRI_DEPTH_DRAW_ZPERSPECTIVE"
    keys[19] = "NATIVE_TRI_DEPTH_DRAW_LINEAR_Z"
    keys[20] = "NATIVE_TRI_DEPTH_DRAW_POLY_OFFSET"
    keys[21] = "NATIVE_TRI_DEPTH_DRAW_SMOOTH"
    keys[22] = "NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST"
    keys[23] = "NATIVE_TRI_DEPTH_FALLBACK_FLAT"
    keys[24] = "NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST"
    keys[25] = "NATIVE_QUAD_DRAW"
    keys[26] = "NATIVE_QUAD_DRAW_LIST"
    keys[27] = "NATIVE_QUAD_DRAW_STRIP"
    keys[28] = "NATIVE_QUAD_CANDIDATE"
    keys[29] = "NATIVE_QUAD_CANDIDATE_SMOOTH"
    keys[30] = "NATIVE_QUAD_CANDIDATE_FLAT"
    keys[31] = "NATIVE_QUAD_FALLBACK"
    keys[32] = "NATIVE_QUAD_FALLBACK_FLAT"
    keys[33] = "NATIVE_QUAD_FALLBACK_NONFILL"
    keys[34] = "NATIVE_QUAD_DRAW_ZPERSPECTIVE"
    keys[35] = "NATIVE_QUAD_DRAW_LINEAR_Z"
    keys[36] = "NATIVE_QUAD_DRAW_POLY_OFFSET"
    keys[37] = "SHADER_GEN"
    keys[38] = "SHADER_BIND"
    keys[39] = "BEGIN_ENDS"
    keys[40] = "DRAW_ARRAYS"
    keys[41] = "INLINE_ELEMENTS"
    keys[42] = "INLINE_ARRAYS"
    keys[43] = "INLINE_BUFFERS"

    for (i = 1; i <= 43; i++) {
        printf("%s=%d\n", keys[i], counters[keys[i]])
    }

    emit_jitter("",         all_fps, all_mspf_max, all_mspf_avg, all_count)
    emit_jitter("post_load_", post_fps, post_mspf_max, post_mspf_avg, post_count)
}
' "$LOG_FILE"
