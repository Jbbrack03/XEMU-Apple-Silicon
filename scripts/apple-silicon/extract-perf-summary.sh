#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<EOF
usage: $0 RUN_DIR|xemu.log [post-load-skip-intervals]

Summarizes xemu-perf interval lines from an Apple Silicon benchmark run.
The default post-load summary skips the first five intervals.
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

/xemu-perf: interval_ms=/ {
    intervals++
    interval_fps = 0
    interval_mspf = 0
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

    if (timed_intervals > skip) {
        post_intervals++
        post_fps_total += interval_fps
        post_mspf_total += interval_mspf
    }
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
    keys[9] = "GEOM_SHADER_DRAW_OTHER"
    keys[10] = "NATIVE_TRI_DEPTH_DRAW"
    keys[11] = "NATIVE_TRI_DEPTH_CANDIDATE"
    keys[12] = "NATIVE_TRI_DEPTH_CANDIDATE_SMOOTH"
    keys[13] = "NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST"
    keys[14] = "NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST"
    keys[15] = "NATIVE_TRI_DEPTH_FALLBACK"
    keys[16] = "NATIVE_TRI_DEPTH_DRAW_ZPERSPECTIVE"
    keys[17] = "NATIVE_TRI_DEPTH_DRAW_LINEAR_Z"
    keys[18] = "NATIVE_TRI_DEPTH_DRAW_POLY_OFFSET"
    keys[19] = "NATIVE_TRI_DEPTH_DRAW_SMOOTH"
    keys[20] = "NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST"
    keys[21] = "NATIVE_TRI_DEPTH_FALLBACK_FLAT"
    keys[22] = "NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST"
    keys[23] = "SHADER_GEN"
    keys[24] = "SHADER_BIND"
    keys[25] = "BEGIN_ENDS"
    keys[26] = "DRAW_ARRAYS"
    keys[27] = "INLINE_ELEMENTS"
    keys[28] = "INLINE_ARRAYS"
    keys[29] = "INLINE_BUFFERS"

    for (i = 1; i <= 29; i++) {
        printf("%s=%d\n", keys[i], counters[keys[i]])
    }
}
' "$LOG_FILE"
