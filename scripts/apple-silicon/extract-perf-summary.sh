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

When intervals carry `frame_mspf_us=...` (XEMU_PERF_FRAME_LOG=1), true
frame-level metrics are also emitted:

  frame_mspf_us_p50/p95/p99/p999/max  per-frame mspf percentiles (us)
  frame_mspf_us_count                 total frame samples
  frame_mspf_us_dropped_total         sum of per-interval dropped counts
  stutter_frames_30fps/45fps/60fps    frames > 33.3 / 22.2 / 16.7 ms

Frame-level metrics are also emitted with the post_load_ prefix.
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

# Track the running maximum across intervals for keys whose semantics
# are "per-interval maximum" rather than "per-interval sum".
function max_counter(name, value) {
    if (!(name in counters_max) || value > counters_max[name]) {
        counters_max[name] = value
    }
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

/xemu-perf: .*interval_ms=/ {
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
                   key == "SHADER_COMPILE_COUNT" ||
                   key == "SHADER_COMPILE_US_TOTAL" ||
                   key == "SHADER_COMPILE_ASYNC_QUEUED" ||
                   key == "SHADER_COMPILE_ASYNC_COMPLETED" ||
                   key == "SHADER_DRAWS_SKIPPED_PENDING" ||
                   key == "BIND_TEXTURES_US_TOTAL" ||
                   key == "TEX_UPLOAD_US_TOTAL" ||
                   key == "SURF_TO_TEX_US_TOTAL" ||
                   key == "SURF_UPLOAD_US_TOTAL" ||
                   key == "SURF_DOWNLOAD_US_TOTAL" ||
                   key == "FLUSH_DRAW_US_TOTAL" ||
                   key == "DRAW_BEGIN_US_TOTAL" ||
                   key == "FLIP_STALL_US_TOTAL" ||
                   key == "FLIP_STALL_GLFINISH_US_TOTAL" ||
                   key == "MSAA_RESOLVE_US_TOTAL" ||
                   key == "BEGIN_ENDS" ||
                   key == "DRAW_ARRAYS" ||
                   key == "INLINE_ELEMENTS" ||
                   key == "INLINE_ARRAYS" ||
                   key == "INLINE_BUFFERS" ||
                   key == "TCG_TB_EXEC_COUNT" ||
                   key == "TCG_TB_INVALIDATE_COUNT" ||
                   key == "TCG_NOTDIRTY_TRIPS" ||
                   key == "TCG_NOTDIRTY_PAGES_HIT" ||
                   key == "TCG_JMP_CACHE_ZEROED_BUCKETS" ||
                   key == "TCG_INVALIDATE_WALL_US_TOTAL" ||
                   key == "TCG_TB_LOOKUP_US_TOTAL" ||
                   key == "TCG_TB_GEN_CODE_US_TOTAL" ||
                   key == "TCG_HANDLE_INTERRUPT_US_TOTAL" ||
                   key == "MMIO_READ_COUNT" ||
                   key == "MMIO_READ_US_TOTAL" ||
                   key == "MMIO_READ_PGRAPH_COUNT" ||
                   key == "MMIO_READ_PGRAPH_US_TOTAL" ||
                   key == "HELPER_RDTSC_CALLS" ||
                   key == "PIT_IRQ_TIMER_FIRES" ||
                   key == "PIT_COALESCE_EVENTS" ||
                   key == "PIT_COALESCE_LATE_US_TOTAL" ||
                   key == "PIT_COALESCE_SKIPPED_TRANSITIONS" ||
                   key == "TCG_XBOX_IDLE_LOOP_HITS" ||
                   key == "TCG_XBOX_IDLE_LOOP_YIELDS" ||
                   key == "TCG_XBOX_IDLE_LOOP_HALTS" ||
                   key == "PFIFO_PUSHER_RUNS" ||
                   key == "PFIFO_PUSHER_STALLS" ||
                   key == "PFIFO_PUSHER_STALL_FLIP" ||
                   key == "PFIFO_PUSHER_STALL_NOP" ||
                   key == "PFIFO_PUSHER_STALL_FIFO" ||
                   key == "PFIFO_PULLER_CALLS" ||
                   key == "PFIFO_PULLER_STALLS" ||
                   key == "PFIFO_PULLER_STALL_FLIP" ||
                   key == "PFIFO_PULLER_STALL_NOP" ||
                   key == "PFIFO_PULLER_STALL_CONTEXT" ||
                   key == "PFIFO_PULLER_STALL_FIFO" ||
                   key == "PFIFO_PUSHER_WORDS" ||
                   key == "PFIFO_PULLER_METHOD_WORDS" ||
                   key == "PFIFO_USER_DMA_PUT_WRITES" ||
                   key == "PFIFO_USER_DMA_GET_WRITES" ||
                   key == "PFIFO_USER_DMA_PUT_READS" ||
                   key == "PFIFO_USER_DMA_GET_READS" ||
                   key == "PGRAPH_PATT_COLOR0_WRITES" ||
                   key == "PGRAPH_PATT_COLOR0_READS" ||
                   key == "PGRAPH_FLIP_STALL_SETS" ||
                   key == "PGRAPH_FLIP_STALL_CHECKS" ||
                   key == "PGRAPH_FLIP_STALL_STILL_WAITING" ||
                   key == "PGRAPH_FLIP_STALL_COMPLETES" ||
                   key == "NV2A_VBLANK_FIRES" ||
                   key == "NV2A_FLIP_STALL_WRITES" ||
                   key == "NV2A_PRESENT_HEARTBEAT" ||
                   key == "XEMU_GL_SWAPS" ||
                   key == "APU_LOCK_HOLD_US_TOTAL" ||
                   key == "METAL_DRAW_COUNT" ||
                   key == "METAL_DRAW_INDEXED_COUNT" ||
                   key == "METAL_NATIVE_TRI_DEPTH_DRAWS" ||
                   key == "METAL_NATIVE_QUAD_DRAWS" ||
                   key == "METAL_CLEAR_COUNT" ||
                   key == "METAL_GLSL_TRANSLATE" ||
                   key == "METAL_GLSL_TRANSLATE_FAIL" ||
                   key == "METAL_SHADER_VALIDATE_OK" ||
                   key == "METAL_SHADER_VALIDATE_FAIL" ||
                   key == "METAL_PIPELINE_HITS" ||
                   key == "METAL_PIPELINE_MISSES" ||
                   key == "METAL_PIPELINE_FAILED" ||
                   key == "METAL_TEX_UPLOADS_TOTAL" ||
                   key == "METAL_TEX_UPLOAD_BYTES_TOTAL" ||
                   key == "METAL_TEX_CACHE_HITS" ||
                   key == "METAL_TEX_CACHE_MISSES" ||
                   key == "METAL_PIPELINE_KEY_BUILT" ||
                   key == "METAL_PIPELINE_TRANSLATED_OK" ||
                   key == "METAL_PIPELINE_TRANSLATED_FAILED" ||
                   key == "METAL_DISPATCH_US_TOTAL" ||
                   key == "METAL_TEX_BIND_US_TOTAL" ||
                   key == "METAL_DRAW_ENCODE_US_TOTAL" ||
                   key == "METAL_DRAW_PASS_OPENS" ||
                   key == "METAL_DRAW_PASS_COALESCED" ||
                   key == "METAL_DRAW_PASS_FLUSHES" ||
                   key == "METAL_OPEN_PASS_FLUSH_US_TOTAL" ||
                   key == "METAL_TEX_UPLOAD_US_TOTAL" ||
                   key == "METAL_DRAW_TRANSLATED" ||
                   key == "METAL_PIPELINE_FALLBACKS" ||
                   key == "METAL_UNIFORM_PACK" ||
                   key == "METAL_UNIFORM_BYTES" ||
                   key == "METAL_SHADER_COMPILE_QUEUED_TOTAL" ||
                   key == "METAL_SHADER_COMPILE_COMPLETED_TOTAL" ||
                   key == "METAL_SHADER_COMPILE_FAILED_TOTAL" ||
                   key == "METAL_DRAWS_SKIPPED_PENDING_TOTAL" ||
                   key == "METAL_DRAWS_USING_UBERSHADER_TOTAL" ||
                   key == "METAL_SHADER_CACHE_LOADS" ||
                   key == "METAL_SHADER_CACHE_HITS" ||
                   key == "METAL_SHADER_CACHE_MISSES" ||
                   key == "RATE_SLEW_RATIO_E6" ||
                   key == "RATE_SLEW_ACTIVE" ||
                   key == "METAL_PRESENT_JITTER_US_AVG" ||
                   key == "METAL_PRESENT_JITTER_US_TOTAL" ||
                   key == "METAL_PRESENTS" ||
                   key == "METAL_DISPLAY_LINK_CALLBACKS" ||
                   key == "METAL_DRAWABLE_ACQUIRE_FAILS" ||
                   key == "METAL_MSAA_RESOLVE_COUNT" ||
                   key == "METAL_MSAA_RESOLVE_US_TOTAL" ||
                   key == "METAL_MSAA_SAMPLE_COUNT" ||
                   key == "METAL_FX_SPATIAL_PRESENTS" ||
                   key == "METAL_FX_SPATIAL_US_TOTAL" ||
                   key == "METAL_FX_SCALE_FACTOR" ||
                   key == "METAL_FX_SPATIAL_GPU_US_TOTAL" ||
                   key == "METAL_VERTEX_US_TOTAL" ||
                   key == "METAL_FRAGMENT_US_TOTAL" ||
                   key == "METAL_PRESENT_GPU_US_TOTAL" ||
                   key == "METAL_PRESENT_GPU_FRAMES" ||
                   key == "METAL_CAPTURE_FRAMES" ||
                   key == "METAL_CAPTURE_ACTIVE" ||
                   key == "METAL_SCREENSHOTS_TAKEN" ||
                   key == "METAL_FRONT_FB_PUBLISHES" ||
                   key == "METAL_IMAGE_BLITS" ||
                   key == "METAL_SURFACE_VRAM_DIRTY_HITS" ||
                   key == "METAL_SURFACE_VRAM_UPLOADS" ||
                   key == "METAL_SURFACE_VRAM_UPLOAD_BYTES" ||
                   key == "METAL_SURFACE_RECREATE_SHAPE_MISMATCH" ||
                   key == "METAL_SURFACE_DOWNLOADS" ||
                   key == "METAL_SURFACE_DOWNLOAD_BYTES" ||
                   key == "METAL_SURFACE_DOWNLOAD_US_TOTAL" ||
                   key == "INPUT_USB_POLLS" ||
                   key == "INPUT_BACKEND_UPDATES" ||
                   key == "INPUT_LAT_US_TOTAL") {
            add_counter(key, value)
        } else if (key == "METAL_PRESENT_JITTER_US_MAX" ||
                   key == "TCG_TB_INVALIDATE_BURST_MAX" ||
                   key == "TCG_INVALIDATE_WALL_US_MAX" ||
                   key == "MMIO_READ_US_MAX" ||
                   key == "MMIO_READ_PGRAPH_US_MAX" ||
                   key == "PIT_COALESCE_LATE_US_MAX" ||
                   key == "PFIFO_DMA_BACKLOG_BYTES_MAX" ||
                   key == "APU_VCPU_LOCK_WAIT_US_MAX" ||
                   key == "INPUT_LAT_US_MAX") {
            max_counter(key, value)
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
    keys[39] = "SHADER_COMPILE_COUNT"
    keys[40] = "SHADER_COMPILE_US_TOTAL"
    keys[41] = "SHADER_COMPILE_ASYNC_QUEUED"
    keys[42] = "SHADER_COMPILE_ASYNC_COMPLETED"
    keys[43] = "SHADER_DRAWS_SKIPPED_PENDING"
    keys[44] = "BIND_TEXTURES_US_TOTAL"
    keys[45] = "TEX_UPLOAD_US_TOTAL"
    keys[46] = "SURF_TO_TEX_US_TOTAL"
    keys[47] = "SURF_UPLOAD_US_TOTAL"
    keys[48] = "SURF_DOWNLOAD_US_TOTAL"
    keys[49] = "FLUSH_DRAW_US_TOTAL"
    keys[50] = "DRAW_BEGIN_US_TOTAL"
    keys[51] = "FLIP_STALL_US_TOTAL"
    keys[52] = "FLIP_STALL_GLFINISH_US_TOTAL"
    keys[53] = "MSAA_RESOLVE_US_TOTAL"
    keys[54] = "BEGIN_ENDS"
    keys[55] = "DRAW_ARRAYS"
    keys[56] = "INLINE_ELEMENTS"
    keys[57] = "INLINE_ARRAYS"
    keys[58] = "INLINE_BUFFERS"
    keys[59] = "TCG_TB_EXEC_COUNT"
    keys[60] = "TCG_TB_INVALIDATE_COUNT"
    keys[61] = "TCG_NOTDIRTY_TRIPS"
    keys[62] = "TCG_NOTDIRTY_PAGES_HIT"
    keys[63] = "TCG_JMP_CACHE_ZEROED_BUCKETS"
    keys[64] = "NV2A_VBLANK_FIRES"
    keys[65] = "NV2A_FLIP_STALL_WRITES"
    keys[66] = "NV2A_PRESENT_HEARTBEAT"
    keys[67] = "XEMU_GL_SWAPS"
    keys[68] = "APU_LOCK_HOLD_US_TOTAL"
    keys[69] = "TCG_TB_LOOKUP_US_TOTAL"
    keys[70] = "TCG_TB_GEN_CODE_US_TOTAL"
    keys[71] = "TCG_HANDLE_INTERRUPT_US_TOTAL"
    keys[72] = "HELPER_RDTSC_CALLS"
    keys[73] = "TCG_INVALIDATE_WALL_US_TOTAL"
    keys[74] = "PIT_IRQ_TIMER_FIRES"
    keys[75] = "PIT_COALESCE_EVENTS"
    keys[76] = "PIT_COALESCE_LATE_US_TOTAL"
    keys[77] = "PIT_COALESCE_SKIPPED_TRANSITIONS"
    keys[78] = "TCG_XBOX_IDLE_LOOP_HITS"
    keys[79] = "TCG_XBOX_IDLE_LOOP_YIELDS"
    keys[80] = "TCG_XBOX_IDLE_LOOP_HALTS"
    keys[81] = "MMIO_READ_COUNT"
    keys[82] = "MMIO_READ_US_TOTAL"
    keys[83] = "MMIO_READ_PGRAPH_COUNT"
    keys[84] = "MMIO_READ_PGRAPH_US_TOTAL"
    keys[85] = "PFIFO_PUSHER_RUNS"
    keys[86] = "PFIFO_PUSHER_STALLS"
    keys[87] = "PFIFO_PUSHER_STALL_FLIP"
    keys[88] = "PFIFO_PUSHER_STALL_NOP"
    keys[89] = "PFIFO_PUSHER_STALL_FIFO"
    keys[90] = "PFIFO_PULLER_CALLS"
    keys[91] = "PFIFO_PULLER_STALLS"
    keys[92] = "PFIFO_PULLER_STALL_FLIP"
    keys[93] = "PFIFO_PULLER_STALL_NOP"
    keys[94] = "PFIFO_PULLER_STALL_CONTEXT"
    keys[95] = "PFIFO_PULLER_STALL_FIFO"
    keys[96] = "PFIFO_PUSHER_WORDS"
    keys[97] = "PFIFO_PULLER_METHOD_WORDS"
    keys[98] = "PFIFO_USER_DMA_PUT_WRITES"
    keys[99] = "PFIFO_USER_DMA_GET_WRITES"
    keys[100] = "PFIFO_USER_DMA_PUT_READS"
    keys[101] = "PFIFO_USER_DMA_GET_READS"
    keys[102] = "PGRAPH_PATT_COLOR0_WRITES"
    keys[103] = "PGRAPH_PATT_COLOR0_READS"
    keys[104] = "PGRAPH_FLIP_STALL_SETS"
    keys[105] = "PGRAPH_FLIP_STALL_CHECKS"
    keys[106] = "PGRAPH_FLIP_STALL_STILL_WAITING"
    keys[107] = "PGRAPH_FLIP_STALL_COMPLETES"
    keys[108] = "METAL_DRAW_COUNT"
    keys[109] = "METAL_DRAW_INDEXED_COUNT"
    keys[110] = "METAL_NATIVE_TRI_DEPTH_DRAWS"
    keys[111] = "METAL_NATIVE_QUAD_DRAWS"
    keys[112] = "METAL_CLEAR_COUNT"
    keys[113] = "METAL_GLSL_TRANSLATE"
    keys[114] = "METAL_GLSL_TRANSLATE_FAIL"
    keys[115] = "METAL_SHADER_VALIDATE_OK"
    keys[116] = "METAL_SHADER_VALIDATE_FAIL"
    keys[117] = "METAL_PIPELINE_HITS"
    keys[118] = "METAL_PIPELINE_MISSES"
    keys[119] = "METAL_PIPELINE_FAILED"
    keys[120] = "METAL_TEX_UPLOADS_TOTAL"
    keys[121] = "METAL_TEX_UPLOAD_BYTES_TOTAL"
    keys[122] = "METAL_TEX_CACHE_HITS"
    keys[123] = "METAL_TEX_CACHE_MISSES"
    keys[124] = "METAL_PIPELINE_KEY_BUILT"
    keys[125] = "METAL_PIPELINE_TRANSLATED_OK"
    keys[126] = "METAL_PIPELINE_TRANSLATED_FAILED"
    keys[127] = "METAL_DRAW_TRANSLATED"
    keys[128] = "METAL_PIPELINE_FALLBACKS"
    keys[129] = "METAL_UNIFORM_PACK"
    keys[130] = "METAL_UNIFORM_BYTES"
    keys[131] = "METAL_SHADER_COMPILE_QUEUED_TOTAL"
    keys[132] = "METAL_SHADER_COMPILE_COMPLETED_TOTAL"
    keys[133] = "METAL_SHADER_COMPILE_FAILED_TOTAL"
    keys[134] = "METAL_DRAWS_SKIPPED_PENDING_TOTAL"
    keys[135] = "METAL_DRAWS_USING_UBERSHADER_TOTAL"
    keys[136] = "METAL_SHADER_CACHE_LOADS"
    keys[137] = "METAL_SHADER_CACHE_HITS"
    keys[138] = "METAL_SHADER_CACHE_MISSES"
    keys[139] = "RATE_SLEW_RATIO_E6"
    keys[140] = "RATE_SLEW_ACTIVE"
    keys[141] = "METAL_PRESENTS"
    keys[142] = "METAL_DISPLAY_LINK_CALLBACKS"
    keys[143] = "METAL_DRAWABLE_ACQUIRE_FAILS"
    keys[144] = "METAL_PRESENT_JITTER_US_TOTAL"
    keys[145] = "METAL_PRESENT_JITTER_US_AVG"
    keys[146] = "METAL_MSAA_RESOLVE_COUNT"
    keys[147] = "METAL_MSAA_RESOLVE_US_TOTAL"
    keys[148] = "METAL_MSAA_SAMPLE_COUNT"
    keys[149] = "METAL_FX_SPATIAL_PRESENTS"
    keys[150] = "METAL_FX_SPATIAL_US_TOTAL"
    keys[151] = "METAL_FX_SCALE_FACTOR"
    keys[152] = "METAL_FX_SPATIAL_GPU_US_TOTAL"
    keys[153] = "METAL_VERTEX_US_TOTAL"
    keys[154] = "METAL_FRAGMENT_US_TOTAL"
    keys[155] = "METAL_PRESENT_GPU_US_TOTAL"
    keys[156] = "METAL_PRESENT_GPU_FRAMES"
    keys[157] = "METAL_CAPTURE_FRAMES"
    keys[158] = "METAL_CAPTURE_ACTIVE"
    keys[159] = "METAL_SCREENSHOTS_TAKEN"
    keys[160] = "INPUT_USB_POLLS"
    keys[161] = "INPUT_BACKEND_UPDATES"
    keys[162] = "INPUT_LAT_US_TOTAL"
    keys[163] = "METAL_FRONT_FB_PUBLISHES"
    keys[164] = "METAL_IMAGE_BLITS"
    keys[165] = "METAL_SURFACE_VRAM_DIRTY_HITS"
    keys[166] = "METAL_SURFACE_VRAM_UPLOADS"
    keys[167] = "METAL_SURFACE_VRAM_UPLOAD_BYTES"
    keys[168] = "METAL_SURFACE_DOWNLOADS"
    keys[169] = "METAL_SURFACE_DOWNLOAD_BYTES"
    keys[170] = "METAL_DISPATCH_US_TOTAL"
    keys[171] = "METAL_TEX_BIND_US_TOTAL"
    keys[172] = "METAL_DRAW_ENCODE_US_TOTAL"
    keys[173] = "METAL_DRAW_PASS_OPENS"
    keys[174] = "METAL_DRAW_PASS_COALESCED"
    keys[175] = "METAL_DRAW_PASS_FLUSHES"
    keys[176] = "METAL_OPEN_PASS_FLUSH_US_TOTAL"
    keys[177] = "METAL_TEX_UPLOAD_US_TOTAL"
    keys[178] = "METAL_SURFACE_DOWNLOAD_US_TOTAL"

    for (i = 1; i <= 178; i++) {
        printf("%s=%d\n", keys[i], counters[keys[i]])
    }
    # Per-interval-max counters: report the running max across intervals.
    printf("METAL_PRESENT_JITTER_US_MAX=%d\n",
           ("METAL_PRESENT_JITTER_US_MAX" in counters_max) ? counters_max["METAL_PRESENT_JITTER_US_MAX"] : 0)
    printf("TCG_TB_INVALIDATE_BURST_MAX=%d\n",
           ("TCG_TB_INVALIDATE_BURST_MAX" in counters_max) ? counters_max["TCG_TB_INVALIDATE_BURST_MAX"] : 0)
    printf("TCG_INVALIDATE_WALL_US_MAX=%d\n",
           ("TCG_INVALIDATE_WALL_US_MAX" in counters_max) ? counters_max["TCG_INVALIDATE_WALL_US_MAX"] : 0)
    printf("MMIO_READ_US_MAX=%d\n",
           ("MMIO_READ_US_MAX" in counters_max) ? counters_max["MMIO_READ_US_MAX"] : 0)
    printf("MMIO_READ_PGRAPH_US_MAX=%d\n",
           ("MMIO_READ_PGRAPH_US_MAX" in counters_max) ? counters_max["MMIO_READ_PGRAPH_US_MAX"] : 0)
    printf("PIT_COALESCE_LATE_US_MAX=%d\n",
           ("PIT_COALESCE_LATE_US_MAX" in counters_max) ? counters_max["PIT_COALESCE_LATE_US_MAX"] : 0)
    printf("PFIFO_DMA_BACKLOG_BYTES_MAX=%d\n",
           ("PFIFO_DMA_BACKLOG_BYTES_MAX" in counters_max) ? counters_max["PFIFO_DMA_BACKLOG_BYTES_MAX"] : 0)
    printf("APU_VCPU_LOCK_WAIT_US_MAX=%d\n",
           ("APU_VCPU_LOCK_WAIT_US_MAX" in counters_max) ? counters_max["APU_VCPU_LOCK_WAIT_US_MAX"] : 0)
    printf("INPUT_LAT_US_MAX=%d\n",
           ("INPUT_LAT_US_MAX" in counters_max) ? counters_max["INPUT_LAT_US_MAX"] : 0)

    emit_jitter("",         all_fps, all_mspf_max, all_mspf_avg, all_count)
    emit_jitter("post_load_", post_fps, post_mspf_max, post_mspf_avg, post_count)
}
' "$LOG_FILE"

# Frame-level percentile post-processing.
#
# When XEMU_PERF_FRAME_LOG=1 was set during the run, each interval line
# carries frame_mspf_us=v1,v2,... (microsecond mspf for every frame in
# that interval, bounded 1024 frames per interval; overflow recorded in
# frame_mspf_us_dropped). Aggregate those across all timed intervals
# (final=1 intervals excluded, matching the existing timing averages)
# and emit true frame-level percentiles. If no frame_mspf_us= field is
# present the script emits nothing here.
python3 - "$LOG_FILE" "$SKIP" <<'PY'
import sys

log_path = sys.argv[1]
skip = int(sys.argv[2])

def parse_kv(line):
    out = {}
    for tok in line.split():
        if "=" not in tok:
            continue
        k, _, v = tok.partition("=")
        out[k] = v
    return out

all_frames = []
post_frames = []
dropped_total = 0
post_dropped_total = 0
timed_intervals = 0
have_frame_field = False

with open(log_path, "r", errors="replace") as fh:
    for line in fh:
        if "xemu-perf:" not in line or "interval_ms=" not in line:
            continue
        kv = parse_kv(line)
        is_final = kv.get("final", "0") not in ("0", "")
        frame_field = kv.get("frame_mspf_us")
        if frame_field is not None:
            have_frame_field = True
        if is_final:
            # Match existing convention: final=1 partial-exit interval is
            # excluded from timing-average windows. Apply the same to
            # frame-level percentiles so the metrics describe live
            # gameplay, not shutdown frames.
            continue
        timed_intervals += 1
        is_post = timed_intervals > skip
        try:
            dropped = int(kv.get("frame_mspf_us_dropped", "0"))
        except ValueError:
            dropped = 0
        dropped_total += dropped
        if is_post:
            post_dropped_total += dropped
        if frame_field:
            samples = []
            for v in frame_field.split(","):
                if not v:
                    continue
                try:
                    samples.append(int(v))
                except ValueError:
                    continue
            all_frames.extend(samples)
            if is_post:
                post_frames.extend(samples)

if not have_frame_field:
    sys.exit(0)

def pct(sorted_samples, p):
    n = len(sorted_samples)
    if n == 0:
        return 0
    rank = int((p / 100.0) * n + 0.5)
    if rank < 1:
        rank = 1
    if rank > n:
        rank = n
    return sorted_samples[rank - 1]

def emit(prefix, samples, dropped_sum):
    n = len(samples)
    if n == 0:
        # Field was present somewhere in the run but this window had no
        # samples (e.g. post-load skip exceeds total intervals). Emit a
        # zero count so consumers can detect the gap unambiguously.
        print(f"{prefix}frame_mspf_us_count=0")
        print(f"{prefix}frame_mspf_us_dropped_total={dropped_sum}")
        return
    s = sorted(samples)
    print(f"{prefix}frame_mspf_us_p50={pct(s, 50)}")
    print(f"{prefix}frame_mspf_us_p95={pct(s, 95)}")
    print(f"{prefix}frame_mspf_us_p99={pct(s, 99)}")
    print(f"{prefix}frame_mspf_us_p999={pct(s, 99.9)}")
    print(f"{prefix}frame_mspf_us_max={s[-1]}")
    print(f"{prefix}frame_mspf_us_count={n}")
    print(f"{prefix}frame_mspf_us_dropped_total={dropped_sum}")
    # Stutter frame counts: strictly greater than the per-target mspf.
    # Thresholds match the per-interval stutter_intervals_* keys.
    print(f"{prefix}stutter_frames_60fps={sum(1 for v in samples if v > 16700)}")
    print(f"{prefix}stutter_frames_45fps={sum(1 for v in samples if v > 22200)}")
    print(f"{prefix}stutter_frames_30fps={sum(1 for v in samples if v > 33300)}")

emit("", all_frames, dropped_total)
emit("post_load_", post_frames, post_dropped_total)
PY
