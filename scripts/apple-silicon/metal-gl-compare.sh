#!/usr/bin/env bash
# metal-gl-compare.sh — paired Metal-vs-GL diff harness (slice W2).
#
# Drives two `run-benchmark.sh` invocations of the same game/input under
# `XEMU_RENDERER=GL` and `XEMU_RENDERER=METAL`, captures matched
# screenshots, runs `compare-screenshots.py` per-frame, runs
# `compare-runs.sh` for perf, and emits a `report.md` + `summary.json`
# with a PASS/FAIL verdict against a per-pixel-changed threshold.
#
# Backs the M15 visual gate from `docs/apple-silicon/metal-renderer-plan.md`
# §4 M15: ≤ 1 % per-pixel diff vs GL on the validation title set.
#
# bash 3.2-compatible (macOS default).
# Apple Silicon performance fork.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_ROOT="${ROOT_DIR}/benchmark-runs"
XEMU_BIN="${ROOT_DIR}/dist/xemu.app/Contents/MacOS/xemu"
RUN_BENCHMARK="${ROOT_DIR}/scripts/apple-silicon/run-benchmark.sh"
LLDB_GL_LAUNCH="${ROOT_DIR}/scripts/apple-silicon/lldb-gl-launch.sh"
COMPARE_SCREENSHOTS="${ROOT_DIR}/scripts/apple-silicon/compare-screenshots.py"
COMPARE_RUNS="${ROOT_DIR}/scripts/apple-silicon/compare-runs.sh"
INPUT_SCRIPT_DIR="${ROOT_DIR}/scripts/apple-silicon/input-scripts"

usage() {
    cat <<EOF
usage: $0 <game> [--input <csv>] [--frames N,M,K]
                 [--crop x,y,w,h] [--threshold pct]
                 [--duration seconds] [--out-dir <path>]
                 [--snapshot <tag>] [--loadvm-at <sec>]
                 [--trigger frame|flip] [--trigger-ordinal <N>]
                 [--evidence-class canary|gameplay|capture]
                 [--metal-no-validate]
                 [--help]

Paired Metal-vs-GL visual + perf diff harness. Runs the same game/input
twice (GL baseline, then Metal candidate), captures matched screenshots,
diffs per-frame with compare-screenshots.py, diffs perf with
compare-runs.sh, and emits report.md + summary.json with a PASS/FAIL
verdict.

Arguments:
  <game>             one of: pgr2, rainbow, crimson, sc2, halo, flat-tri-depth.
                     Same aliases as run-benchmark.sh.

Options:
  --input <csv>      Scripted-input CSV. Default: input-scripts/<game>-smoke.csv
                     if it exists, else fail.
  --frames N,M,K     1-indexed screenshot ordinals to compare. Default
                     "mid,end" — picks two ordinals from the captured
                     sequence (the middle one and the last one).
                     Ignored when --trigger=flip (the trigger fires a
                     single one-shot capture per leg).
  --crop x,y,w,h     Crop rectangle passed to compare-screenshots.py.
                     Default: full frame (no crop applied; the comparator
                     receives a synthetic full-image crop after the PNG
                     is opened).
  --threshold pct    Maximum changed-pixels percentage per compared frame
                     for the run to PASS. Default 1.0 (1 %).
  --duration seconds Per-renderer benchmark duration. Default 30.
  --out-dir <path>   Output directory. Default
                     benchmark-runs/<TS>-metal-gl-compare-<game>/.
  --snapshot <tag>   F1 (2026-05-04) — load the named savevm tag into
                     the guest at \`--loadvm-at\` seconds (default 2 s)
                     via QMP/HMP, then drive the input script. Removes
                     cold-launch shader-compile / OS-scheduler / asset-
                     load drift between the two legs by starting both
                     from the same guest state.
  --loadvm-at <sec>  F1 — number of seconds after xemu launch at which
                     the harness restores --snapshot. Default 2.
  --trigger frame|flip
                     F1 — capture trigger.  \`frame\` (default for
                     back-compat) uses the original ordinal-based
                     submit-time-frame schedule on Metal and the
                     wallclock-interval cadence on GL.  \`flip\` activates
                     the flip-stall trigger: the renderer-agnostic
                     XEMU_CAPTURE_AT_FLIP_STALL counts NV097_FLIP_STALL
                     events (a guest-side page-flip request) and arms a
                     one-shot capture on the --trigger-ordinal-th event.
                     The Metal leg fires its in-renderer NV2A texture
                     screenshot directly; the GL leg writes an
                     in-renderer display-framebuffer PNG.
  --trigger-ordinal <N>
                     F1 — N for the trigger.  Defaults: 30 for
                     \`flip\` (gives the post-loadvm guest several
                     frames to settle / shader cache to warm), 60 for
                     \`frame\` (matches the existing harness default
                     submit-time at_frame).
  --evidence-class <class>
                     Classify the visual evidence written to summary.json.
                     Use "gameplay" only for matched gameplay keyframes from
                     a gameplay route. Default: canary.
  --metal-no-validate
                     Do not enable Metal API validation for the Metal leg.
                     Use this for product-like perf/jitter measurements.
  --gl-attach-lldb
                     Tool 3 (2026-05-19) — route the GL leg through
                     scripts/apple-silicon/lldb-gl-launch.sh so a
                     segfault during cold launch yields a captured
                     backtrace at <gl-run-dir>/crash.lldb.log. Primary
                     use: Halo cold-launch segfault investigation
                     (benchmark-runs/20260511-153638-metal-gl-compare-halo).
                     Adds LLDB overhead to the GL leg only; the Metal
                     leg is unaffected.
  --help             Print this usage.

Exit codes:
  0  PASS — every compared frame is within --threshold.
  1  FAIL — at least one compared frame exceeded --threshold.
  2  INFRASTRUCTURE failure (binary missing, asset missing, screenshots
     missing, sub-script error, etc.).
EOF
}

err() {
    echo "metal-gl-compare: $*" >&2
}

# --- argument parsing ------------------------------------------------------

GAME=""
INPUT_CSV=""
FRAMES_SPEC=""
CROP=""
THRESHOLD="1.0"
DURATION="30"
OUT_DIR=""
# F1 (2026-05-04) — snapshot + flip-stall-trigger plumbing.
SNAPSHOT_TAG=""
LOADVM_AT="2"
TRIGGER="frame"
TRIGGER_ORDINAL=""
METAL_VALIDATE=1
EVIDENCE_CLASS="canary"
GL_ATTACH_LLDB=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            usage
            exit 0
            ;;
        --input)
            [[ $# -ge 2 ]] || { err "--input requires a path"; exit 2; }
            INPUT_CSV="$2"; shift 2 ;;
        --input=*)
            INPUT_CSV="${1#--input=}"; shift ;;
        --frames)
            [[ $# -ge 2 ]] || { err "--frames requires a value"; exit 2; }
            FRAMES_SPEC="$2"; shift 2 ;;
        --frames=*)
            FRAMES_SPEC="${1#--frames=}"; shift ;;
        --crop)
            [[ $# -ge 2 ]] || { err "--crop requires a value"; exit 2; }
            CROP="$2"; shift 2 ;;
        --crop=*)
            CROP="${1#--crop=}"; shift ;;
        --threshold)
            [[ $# -ge 2 ]] || { err "--threshold requires a value"; exit 2; }
            THRESHOLD="$2"; shift 2 ;;
        --threshold=*)
            THRESHOLD="${1#--threshold=}"; shift ;;
        --duration)
            [[ $# -ge 2 ]] || { err "--duration requires a value"; exit 2; }
            DURATION="$2"; shift 2 ;;
        --duration=*)
            DURATION="${1#--duration=}"; shift ;;
        --out-dir)
            [[ $# -ge 2 ]] || { err "--out-dir requires a path"; exit 2; }
            OUT_DIR="$2"; shift 2 ;;
        --out-dir=*)
            OUT_DIR="${1#--out-dir=}"; shift ;;
        --snapshot)
            [[ $# -ge 2 ]] || { err "--snapshot requires a tag"; exit 2; }
            SNAPSHOT_TAG="$2"; shift 2 ;;
        --snapshot=*)
            SNAPSHOT_TAG="${1#--snapshot=}"; shift ;;
        --loadvm-at)
            [[ $# -ge 2 ]] || { err "--loadvm-at requires seconds"; exit 2; }
            LOADVM_AT="$2"; shift 2 ;;
        --loadvm-at=*)
            LOADVM_AT="${1#--loadvm-at=}"; shift ;;
        --trigger)
            [[ $# -ge 2 ]] || { err "--trigger requires a value"; exit 2; }
            TRIGGER="$2"; shift 2 ;;
        --trigger=*)
            TRIGGER="${1#--trigger=}"; shift ;;
        --trigger-ordinal)
            [[ $# -ge 2 ]] || { err "--trigger-ordinal requires a value"; exit 2; }
            TRIGGER_ORDINAL="$2"; shift 2 ;;
        --trigger-ordinal=*)
            TRIGGER_ORDINAL="${1#--trigger-ordinal=}"; shift ;;
        --evidence-class)
            [[ $# -ge 2 ]] || { err "--evidence-class requires a value"; exit 2; }
            EVIDENCE_CLASS="$2"; shift 2 ;;
        --evidence-class=*)
            EVIDENCE_CLASS="${1#--evidence-class=}"; shift ;;
        --metal-no-validate)
            METAL_VALIDATE=0; shift ;;
        --gl-attach-lldb)
            GL_ATTACH_LLDB=1; shift ;;
        --)
            shift; break ;;
        --*)
            err "unknown flag: $1"
            usage >&2
            exit 2 ;;
        *)
            if [[ -z "$GAME" ]]; then
                GAME="$1"; shift
            else
                err "unexpected positional argument: $1"
                usage >&2
                exit 2
            fi
            ;;
    esac
done

if [[ -z "$GAME" ]]; then
    err "missing required <game> argument"
    usage >&2
    exit 2
fi

# Reject the non-existent run-benchmark.sh aliases up front so the user
# sees a script-level error rather than a launcher-level one. (Mirrors
# run-benchmark.sh's case statement; this file does not encode game
# paths itself — the launcher resolves them.)
case "$GAME" in
    pgr2|rainbow|crimson|sc2|halo|flat-tri-depth)
        ;;
    *)
        err "unrecognized game alias '$GAME' (expected pgr2|rainbow|crimson|sc2|halo|flat-tri-depth)"
        exit 2 ;;
esac

# Validate numeric args.
case "$DURATION" in
    ''|*[!0-9]*) err "--duration must be a positive integer (got '$DURATION')"; exit 2 ;;
esac
if [[ "$DURATION" -lt 1 ]]; then
    err "--duration must be >= 1"
    exit 2
fi

# THRESHOLD must be a non-negative float; bash 3.2 has no float compare,
# so a regex test is the cheapest sanity check.
if ! [[ "$THRESHOLD" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    err "--threshold must be a non-negative number (got '$THRESHOLD')"
    exit 2
fi

# CROP, when given, must be x,y,w,h ints (compare-screenshots.py also
# validates; we duplicate the check so a typo fails before the long
# run starts).
if [[ -n "$CROP" ]]; then
    if ! [[ "$CROP" =~ ^[0-9]+,[0-9]+,[0-9]+,[0-9]+$ ]]; then
        err "--crop must be x,y,width,height integers (got '$CROP')"
        exit 2
    fi
fi

# F1 — validate snapshot/trigger combo.
case "$LOADVM_AT" in
    ''|*[!0-9]*) err "--loadvm-at must be a non-negative integer (got '$LOADVM_AT')"; exit 2 ;;
esac

case "$TRIGGER" in
    frame|flip) ;;
    *) err "--trigger must be 'frame' or 'flip' (got '$TRIGGER')"; exit 2 ;;
esac

case "$EVIDENCE_CLASS" in
    canary|gameplay|capture) ;;
    *) err "--evidence-class must be canary, gameplay, or capture (got '$EVIDENCE_CLASS')"; exit 2 ;;
esac

if [[ -z "$TRIGGER_ORDINAL" ]]; then
    if [[ "$TRIGGER" == "flip" ]]; then
        TRIGGER_ORDINAL=30
    else
        TRIGGER_ORDINAL=60
    fi
fi
case "$TRIGGER_ORDINAL" in
    ''|*[!0-9]*) err "--trigger-ordinal must be a positive integer (got '$TRIGGER_ORDINAL')"; exit 2 ;;
esac
if [[ "$TRIGGER_ORDINAL" -lt 1 ]]; then
    err "--trigger-ordinal must be >= 1 (got '$TRIGGER_ORDINAL')"
    exit 2
fi

# --- pre-flight checks -----------------------------------------------------

if [[ ! -x "$XEMU_BIN" ]]; then
    err "missing or non-executable xemu binary: $XEMU_BIN"
    err "run './build.sh -a arm64' from $ROOT_DIR first"
    exit 2
fi

if ! "$XEMU_BIN" --version >/dev/null 2>&1; then
    err "xemu --version failed; binary may be unsigned or broken: $XEMU_BIN"
    exit 2
fi

for tool in "$RUN_BENCHMARK" "$COMPARE_SCREENSHOTS" "$COMPARE_RUNS"; do
    if [[ ! -x "$tool" && ! -r "$tool" ]]; then
        err "missing helper: $tool"
        exit 2
    fi
done

# Resolve INPUT_CSV.
if [[ -z "$INPUT_CSV" ]]; then
    INPUT_CSV="${INPUT_SCRIPT_DIR}/${GAME}-smoke.csv"
    # The launcher's <game> alias and the input-script filename diverge for
    # crimson (game=crimson, csv=crimson-skies-smoke.csv) and rainbow
    # (game=rainbow, csv=rainbow-six-3-smoke.csv); fall back to those if
    # the simple alias miss.
    if [[ ! -e "$INPUT_CSV" ]]; then
        case "$GAME" in
            crimson) INPUT_CSV="${INPUT_SCRIPT_DIR}/crimson-skies-smoke.csv" ;;
            rainbow) INPUT_CSV="${INPUT_SCRIPT_DIR}/rainbow-six-3-smoke.csv" ;;
            sc2|halo|flat-tri-depth) INPUT_CSV="${INPUT_SCRIPT_DIR}/noop.csv" ;;
        esac
    fi
fi

if [[ ! -e "$INPUT_CSV" ]]; then
    err "input CSV not found: $INPUT_CSV"
    err "use --input <csv> to override; default lookup tried"
    err "  ${INPUT_SCRIPT_DIR}/${GAME}-smoke.csv (and known aliases)"
    exit 2
fi

# --- output directory ------------------------------------------------------

if [[ -z "$OUT_DIR" ]]; then
    STAMP="$(date +%Y%m%d-%H%M%S)"
    OUT_DIR="${RUN_ROOT}/${STAMP}-metal-gl-compare-${GAME}"
fi

mkdir -p "$OUT_DIR" "$OUT_DIR/gl" "$OUT_DIR/metal" "$OUT_DIR/diffs"

LOG_FILE="$OUT_DIR/harness.log"
GL_LAUNCHER_LOG="$OUT_DIR/gl-launcher.log"
METAL_LAUNCHER_LOG="$OUT_DIR/metal-launcher.log"

# Anything we leak into a tmp file gets cleaned up on exit. We deliberately
# do NOT clean up the OUT_DIR on failure — the operator wants the partial
# artifacts for triage.
TMPFILES=()
cleanup() {
    local f
    for f in "${TMPFILES[@]:-}"; do
        [[ -n "$f" && -e "$f" ]] && rm -f "$f" || true
    done
}
trap 'cleanup' EXIT

# Convenience helpers around tee'd logging without losing exit codes.
log() {
    printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG_FILE"
}

log "metal-gl-compare W2 starting"
log "  game        = $GAME"
log "  input       = $INPUT_CSV"
log "  duration    = ${DURATION}s"
log "  threshold   = ${THRESHOLD}%"
log "  out_dir     = $OUT_DIR"
log "  snapshot    = ${SNAPSHOT_TAG:-(none; cold launch)}"
if [[ -n "$SNAPSHOT_TAG" ]]; then
    log "  loadvm_at   = ${LOADVM_AT}s"
fi
log "  trigger     = $TRIGGER (ordinal=$TRIGGER_ORDINAL)"
log "  evidence    = $EVIDENCE_CLASS"
if [[ "$METAL_VALIDATE" -eq 1 ]]; then
    log "  metal_validate = on"
else
    log "  metal_validate = off (--metal-no-validate)"
fi

# F1 — sentinel file the GL leg's macos-capture.sh polls when --trigger
# flip is active. Lives under the harness OUT_DIR so the per-leg run
# directories stay independent of each other; xemu's flip-stall handler
# touches it once when XEMU_CAPTURE_AT_FLIP_STALL fires. Removed pre-run
# so a stale file from a previous invocation can never short-circuit
# the new run's poll loop.
FLIP_STALL_SENTINEL=""
if [[ "$TRIGGER" == "flip" ]]; then
    FLIP_STALL_SENTINEL="$OUT_DIR/flip-stall-armed.sentinel"
    rm -f "$FLIP_STALL_SENTINEL" || true
fi

# --- run helpers -----------------------------------------------------------

# Common screenshot cadence — capture every second. Both backends honor
# this in different ways: GL's macos-capture pulses screencapture every
# N seconds; Metal renders straight to PNG every N submitted frames.
# 60 frames/s on Metal vs 1s on GL roughly matches; close-enough for
# ordinal pairing inside a short benchmark.
GL_SCREENSHOT_INTERVAL_SECONDS=1
GL_SCREENSHOT_START_DELAY_SECONDS=2
METAL_SCREENSHOT_AT_FRAME=60
METAL_SCREENSHOT_INTERVAL_FRAMES=60

# Run the GL baseline.
run_gl() {
    log "starting GL baseline run (XEMU_RENDERER=GL)"
    local rc
    # W6 (2026-05-04): set XEMU_CAPTURE_WINDOW_PATTERN=xemu so
    # macos-capture.sh runs `screencapture -l <wid>` against the
    # xemu window only, instead of the full desktop. This bounds
    # the GL capture to the same logical region the Metal leg's
    # in-renderer texture PNG covers; any residual size mismatch
    # (retina vs. drawable scaling) is absorbed by the
    # `--resize smaller` normalization in compare-screenshots.py.
    #
    # F1 (2026-05-04): when --trigger flip, set the renderer-agnostic
    # XEMU_CAPTURE_AT_FLIP_STALL=N env var. The GL renderer writes an
    # in-renderer PNG via XEMU_GL_SCREENSHOT_PATH at that flip, avoiding
    # macOS window chrome / scaling artifacts. The old macOS screenshot
    # backend remains the frame-trigger fallback.
    local gl_extra_env=()
    # Canonical M15 recipe defaults for the GL leg (user env wins).
    # The geometry-shader bypass + lock-free PGRAPH read flags are
    # env-gated in pgraph.c:97 / glsl/geom.c:86,117 — they are NOT
    # auto-on for Apple Silicon builds. metal-canary-regress.sh
    # hardcodes them at lines 533-538; matching parity here keeps both
    # gates exercising the same documented PASS recipe. XEMU_GL_MSAA=4
    # is added to match the Metal leg's MSAA=4 so AA edge classes are
    # comparable. XEMU_CAPTURE_WINDOW_REQUIRED=1 makes the GL leg
    # INFRA-FAIL instead of falling back to full-desktop screencapture
    # when window-id lookup fails — full-desktop capture compares
    # macOS desktop chrome against the Metal in-renderer texture PNG,
    # producing meaningless ~70% changed_pct FAILs (Codex review,
    # 2026-05-05).
    [[ "${XEMU_NATIVE_TRI_DEPTH+x}" != "x" ]] && gl_extra_env+=("XEMU_NATIVE_TRI_DEPTH=1")
    [[ "${XEMU_NATIVE_QUAD+x}" != "x" ]] && gl_extra_env+=("XEMU_NATIVE_QUAD=1")
    [[ "${XEMU_PGRAPH_FAST_READ+x}" != "x" ]] && gl_extra_env+=("XEMU_PGRAPH_FAST_READ=1")
    [[ "${XEMU_GL_MSAA+x}" != "x" ]] && gl_extra_env+=("XEMU_GL_MSAA=4")
    [[ "${XEMU_CAPTURE_WINDOW_REQUIRED+x}" != "x" ]] && \
        gl_extra_env+=("XEMU_CAPTURE_WINDOW_REQUIRED=1")
    if [[ -n "$SNAPSHOT_TAG" ]]; then
        gl_extra_env+=("XEMU_BENCH_LOADVM_TAG=$SNAPSHOT_TAG"
                       "XEMU_BENCH_LOADVM_AT=$LOADVM_AT")
    fi
    if [[ "$TRIGGER" == "flip" ]]; then
        # Pre-leg sentinel scrub: keep the start of each leg in a
        # known state.
        rm -f "$FLIP_STALL_SENTINEL" || true
        gl_extra_env+=("XEMU_CAPTURE_AT_FLIP_STALL=$TRIGGER_ORDINAL"
                       "XEMU_CAPTURE_FLIP_STALL_SENTINEL=$FLIP_STALL_SENTINEL"
                       "XEMU_GL_SCREENSHOT_PATH=$OUT_DIR/gl/screenshot.png")
    fi
    # Tool 3 (2026-05-19): when --gl-attach-lldb is set, route through
    # lldb-gl-launch.sh so a cold-launch segfault yields a captured
    # backtrace inside the GL run dir. The wrapper sets
    # XEMU_BENCH_LAUNCHER_PREFIX and forces XEMU_RENDERER=GL; we still
    # explicitly export GL here for clarity.
    local gl_launcher="$RUN_BENCHMARK"
    if [[ "$GL_ATTACH_LLDB" -eq 1 ]]; then
        gl_launcher="$LLDB_GL_LAUNCH"
        log "  gl_launcher = lldb-gl-launch.sh (LLDB attached)"
    fi
    set +e
    if [[ "$TRIGGER" == "flip" ]]; then
        env "${gl_extra_env[@]}" \
            XEMU_RENDERER=GL \
            XEMU_BENCH_SCREENSHOT_BACKEND=none \
            "$gl_launcher" "$GAME" "$INPUT_CSV" "$DURATION" \
            > "$GL_LAUNCHER_LOG" 2>&1
    else
        env "${gl_extra_env[@]}" \
            XEMU_RENDERER=GL \
            XEMU_BENCH_SCREENSHOT_BACKEND=macos \
            XEMU_BENCH_SCREENSHOT_INTERVAL="$GL_SCREENSHOT_INTERVAL_SECONDS" \
            XEMU_BENCH_SCREENSHOT_START_DELAY="$GL_SCREENSHOT_START_DELAY_SECONDS" \
            XEMU_CAPTURE_WINDOW_PATTERN="xemu" \
            "$gl_launcher" "$GAME" "$INPUT_CSV" "$DURATION" \
            > "$GL_LAUNCHER_LOG" 2>&1
    fi
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        err "GL run-benchmark.sh failed with status $rc; see $GL_LAUNCHER_LOG"
        return 2
    fi

    local gl_run_dir
    gl_run_dir="$(awk -F': ' '/^Run directory: / { print $2 }' "$GL_LAUNCHER_LOG" | tail -n 1)"
    if [[ -z "$gl_run_dir" || ! -d "$gl_run_dir" ]]; then
        err "could not parse GL run directory from $GL_LAUNCHER_LOG"
        return 2
    fi
    printf '%s\n' "$gl_run_dir" > "$OUT_DIR/gl/run-dir.txt"
    log "  GL run dir: $gl_run_dir"
}

# Run the Metal candidate. Uses --metal-screenshot to drive the in-renderer
# PNG capture path, with XEMU_METAL_SCREENSHOT_SOURCE=nv2a so the encoded
# image is the renderer-published NV2A display texture, not the post-ImGui
# drawable. That matches the GL leg's XEMU_GL_SCREENSHOT_PATH display PNG
# and keeps xemu menu/toast overlays out of visual diffs.
run_metal() {
    log "starting Metal candidate run (XEMU_RENDERER=METAL)"
    local rc
    local metal_shot_base="$OUT_DIR/metal/screenshot.png"

    # XEMU_METAL_VALIDATION=1 by default — explicit even if W1 already
    # auto-ons it. --metal-no-validate disables it for product-like perf
    # measurements where validation overhead would skew the jitter gate.
    # W6 (2026-05-04): --metal-no-hud is REQUIRED — W1 auto-ons
    # Apple's Metal Performance HUD for any Metal benchmark. The xemu
    # ImGui UI is avoided separately by capturing source=nv2a below.
    # Validation stays on; only Apple's visual HUD is off.
    #
    # F1 (2026-05-04): in --trigger flip mode the in-renderer
    # screenshot is one-shot (XEMU_METAL_SCREENSHOT_INTERVAL is left
    # unset / 0) and fired by the renderer-side flip-stall consume
    # instead of the at_frame counter. We still pass --metal-screenshot
    # because that env (XEMU_METAL_SCREENSHOT_PATH) is what enables the
    # blit-and-encode path inside ui/xemu-metal.mm; without it the
    # consume() call on the renderer side has nowhere to write.
    local metal_extra_env=("XEMU_METAL_SCREENSHOT_SOURCE=nv2a")
    local metal_extra_args=()
    # Canonical M15 Metal recipe defaults (user env wins). The
    # documented PASS recipe for the four green canaries
    # (PGR2/Rainbow/Halo/boot) and the gameplay route diagnostic for
    # Crimson is MSAA4 + translated pipeline + front-fb fallback +
    # geometry-shader bypasses + lock-free PGRAPH reads. Without
    # these defaults the diff harness would publish a CRTC-strict
    # 0x32a4000 surface that titles abandon mid-run after
    # menu→gameplay (Crimson) or mid-render-pass switch (PGR2),
    # AND would run the wrong shader/pgraph path, causing spurious
    # "FAIL" verdicts unrelated to renderer correctness. The canary
    # regression script (metal-canary-regress.sh) already hardcodes
    # the same 7-flag recipe; this brings parity.
    [[ "${XEMU_METAL_TRANSLATED_PIPELINE+x}" != "x" ]] && \
        metal_extra_env+=("XEMU_METAL_TRANSLATED_PIPELINE=1")
    [[ "${XEMU_METAL_FRONT_FB_FALLBACK+x}" != "x" ]] && \
        metal_extra_env+=("XEMU_METAL_FRONT_FB_FALLBACK=1")
    [[ "${XEMU_METAL_MSAA+x}" != "x" ]] && \
        metal_extra_env+=("XEMU_METAL_MSAA=4")
    [[ "${XEMU_NATIVE_TRI_DEPTH+x}" != "x" ]] && \
        metal_extra_env+=("XEMU_NATIVE_TRI_DEPTH=1")
    [[ "${XEMU_NATIVE_QUAD+x}" != "x" ]] && \
        metal_extra_env+=("XEMU_NATIVE_QUAD=1")
    [[ "${XEMU_PGRAPH_FAST_READ+x}" != "x" ]] && \
        metal_extra_env+=("XEMU_PGRAPH_FAST_READ=1")
    if [[ -n "$SNAPSHOT_TAG" ]]; then
        metal_extra_env+=("XEMU_BENCH_LOADVM_TAG=$SNAPSHOT_TAG"
                          "XEMU_BENCH_LOADVM_AT=$LOADVM_AT")
    fi
    if [[ "$TRIGGER" == "flip" ]]; then
        rm -f "$FLIP_STALL_SENTINEL" || true
        metal_extra_env+=("XEMU_CAPTURE_AT_FLIP_STALL=$TRIGGER_ORDINAL"
                          "XEMU_CAPTURE_FLIP_STALL_SENTINEL=$FLIP_STALL_SENTINEL")
        # Force the Metal at-frame trigger out of the way; the consume
        # path inside xemu-metal.mm wins over the at_frame check when
        # armed but we set it to a value that will never fire on a
        # short benchmark so the back-compat path stays inert.
        metal_extra_env+=("XEMU_METAL_SCREENSHOT_INTERVAL=0")
        metal_extra_args+=("--metal-screenshot-at-frame" "999999")
    else
        metal_extra_env+=("XEMU_METAL_SCREENSHOT_INTERVAL=$METAL_SCREENSHOT_INTERVAL_FRAMES")
        metal_extra_args+=("--metal-screenshot-at-frame" "$METAL_SCREENSHOT_AT_FRAME")
    fi
    if [[ "$METAL_VALIDATE" -eq 0 ]]; then
        metal_extra_args+=("--metal-no-validate")
    else
        metal_extra_env+=("XEMU_METAL_VALIDATION=1")
    fi
    set +e
    env "${metal_extra_env[@]}" \
        XEMU_RENDERER=METAL \
        XEMU_BENCH_SCREENSHOT_BACKEND=none \
        "$RUN_BENCHMARK" \
            --metal-screenshot "$metal_shot_base" \
            "${metal_extra_args[@]}" \
            --metal-no-hud \
            "$GAME" "$INPUT_CSV" "$DURATION" \
        > "$METAL_LAUNCHER_LOG" 2>&1
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        err "Metal run-benchmark.sh failed with status $rc; see $METAL_LAUNCHER_LOG"
        return 2
    fi

    local metal_run_dir
    metal_run_dir="$(awk -F': ' '/^Run directory: / { print $2 }' "$METAL_LAUNCHER_LOG" | tail -n 1)"
    if [[ -z "$metal_run_dir" || ! -d "$metal_run_dir" ]]; then
        err "could not parse Metal run directory from $METAL_LAUNCHER_LOG"
        return 2
    fi
    printf '%s\n' "$metal_run_dir" > "$OUT_DIR/metal/run-dir.txt"
    log "  Metal run dir: $metal_run_dir"
}

# --- screenshot enumeration & frame selection ------------------------------

# Returns the sorted list of GL screenshot paths to stdout.
gl_screenshots() {
    if [[ "$TRIGGER" == "flip" ]]; then
        local direct="$OUT_DIR/gl/screenshot.png"
        if [[ -e "$direct" ]]; then
            printf '%s\n' "$direct"
        fi
        return 0
    fi
    local gl_run_dir
    gl_run_dir="$(cat "$OUT_DIR/gl/run-dir.txt")"
    local dir="$gl_run_dir/screenshots"
    if [[ ! -d "$dir" ]]; then
        return 0
    fi
    find "$dir" -maxdepth 1 -name '*.png' -type f | sort
}

# Returns the sorted list of Metal screenshot paths to stdout. The Metal
# renderer with XEMU_METAL_SCREENSHOT_INTERVAL=N writes the first shot at
# the requested at_frame as `<base>.0001.png` and subsequent ones as
# `.0002.png`, `.0003.png`, ... The base path itself (without the .NNNN
# suffix) is also sometimes produced when interval is 0; we include both
# matches for resilience against renderer-side behavior changes.
metal_screenshots() {
    local base="$OUT_DIR/metal/screenshot.png"
    local dir="$OUT_DIR/metal"
    # Prefer suffixed sequence when present; fall back to the un-suffixed
    # single shot when interval-mode wrote nothing else. Newline-terminate
    # the last entry so `wc -l` and line-by-line readers count correctly.
    local files
    files="$(find "$dir" -maxdepth 1 -name 'screenshot.*.png' -type f 2>/dev/null | sort || true)"
    if [[ -z "$files" && -e "$base" ]]; then
        printf '%s\n' "$base"
        return 0
    fi
    if [[ -n "$files" ]]; then
        printf '%s\n' "$files"
    fi
}

# Prints space-separated 1-indexed ordinals derived from FRAMES_SPEC.
# Empty FRAMES_SPEC defaults to "mid,end" against the GL screenshot
# count, except in --trigger flip mode where each leg only produces
# one shot and the only meaningful ordinal is 1.
resolve_frames() {
    local gl_count="$1"
    local metal_count="$2"
    local count
    if [[ "$gl_count" -lt "$metal_count" ]]; then
        count="$gl_count"
    else
        count="$metal_count"
    fi
    if [[ "$count" -lt 1 ]]; then
        err "no screenshots produced (gl_count=$gl_count metal_count=$metal_count)"
        return 1
    fi

    local out=()
    if [[ "$TRIGGER" == "flip" ]]; then
        # F1 flip-stall trigger fires exactly once per run — the only
        # meaningful comparison is the single captured frame. Force
        # ordinal 1 regardless of whether the caller passed --frames;
        # the help text documents --frames as ignored in flip mode and
        # the alternative (rejecting --frames) would break shared
        # command templates that always pass --frames mid,end.
        if [[ -n "$FRAMES_SPEC" && "$FRAMES_SPEC" != "1" ]]; then
            log "ignoring --frames=$FRAMES_SPEC under --trigger flip (one-shot trigger)"
        fi
        out=(1)
    elif [[ -z "$FRAMES_SPEC" || "$FRAMES_SPEC" == "mid,end" ]]; then
        local mid=$(( (count + 1) / 2 ))
        if [[ "$mid" -lt 1 ]]; then mid=1; fi
        if [[ "$count" -eq 1 ]]; then
            out=(1)
        else
            out=("$mid" "$count")
        fi
    else
        local IFS=','
        local raw=($FRAMES_SPEC)
        unset IFS
        local f
        for f in "${raw[@]}"; do
            case "$f" in
                ''|*[!0-9]*)
                    err "frames spec must be comma-separated positive ints (got '$f' in '$FRAMES_SPEC')"
                    return 1 ;;
            esac
            if [[ "$f" -lt 1 || "$f" -gt "$count" ]]; then
                err "frame ordinal $f out of range [1..$count]"
                return 1
            fi
            out+=("$f")
        done
    fi
    printf '%s\n' "${out[@]}"
}

# Maps an ordinal N (1-indexed) to the matching path from a sorted list
# (printed line-per-line on stdin). Echoes the path or empty if missing.
nth_path() {
    local ordinal="$1"
    awk -v n="$ordinal" 'NR==n { print; exit }'
}

# --- diff stage ------------------------------------------------------------

# Synthesize a default crop for compare-screenshots.py. The comparator
# requires --crop; when the user did not pass one we read both PNGs'
# sizes and emit "0,0,W,H" using the SMALLER dimensions of the pair
# (W6, 2026-05-04). The smaller-of-two is what compare-screenshots.py
# resizes the larger image down to under `--resize smaller`, so the
# crop must fit inside the common normalized area.
default_crop_for() {
    local gl_png="$1"
    local metal_png="$2"
    python3 - "$gl_png" "$metal_png" <<'PY'
import sys
from PIL import Image
gl = Image.open(sys.argv[1])
metal = Image.open(sys.argv[2])
w = min(gl.size[0], metal.size[0])
h = min(gl.size[1], metal.size[1])
print(f"0,0,{w},{h}")
PY
}

# Parses key=value lines from compare-screenshots.py stdout. Echoes the
# requested key's value (or empty if absent).
parse_kv() {
    local key="$1"
    local file="$2"
    awk -F= -v k="$key" '$1==k { sub(/^[^=]*=/, ""); print; exit }' "$file"
}

# Per-frame compare. Returns 0 if the diff ran (regardless of pass/fail);
# returns 2 on infrastructure failure.
diff_one_frame() {
    local ordinal="$1"
    local gl_png="$2"
    local metal_png="$3"
    local frame_dir="$OUT_DIR/diffs/frame-${ordinal}"
    mkdir -p "$frame_dir"

    local crop="$CROP"
    if [[ -z "$crop" ]]; then
        if ! crop="$(default_crop_for "$gl_png" "$metal_png" 2>"$frame_dir/crop-err.txt")"; then
            err "could not read PNG dimensions for default crop: $gl_png $metal_png"
            cat "$frame_dir/crop-err.txt" >&2 || true
            return 2
        fi
    fi

    # W6 (2026-05-04): pass --resize smaller so a size mismatch
    # between the GL full-desktop / window-targeted capture and the
    # Metal in-renderer texture PNG is normalized rather than
    # exiting INFRA-FAIL. Records the raw sizes in the per-frame TSV
    # so the report can show what was normalized.
    local stdout_file="$frame_dir/compare-stdout.txt"
    local rc
    set +e
    "$COMPARE_SCREENSHOTS" \
        "$gl_png" "$metal_png" \
        --crop "$crop" \
        --resize smaller \
        --out-dir "$frame_dir" \
        > "$stdout_file" 2>&1
    rc=$?
    set -e
    if [[ $rc -ne 0 ]]; then
        err "compare-screenshots.py failed (frame $ordinal, rc=$rc); see $stdout_file"
        return 2
    fi

    local mae rms max_abs changed_pct raw_gl_size raw_metal_size resized
    mae="$(parse_kv mean_abs_error "$stdout_file")"
    rms="$(parse_kv rms_error "$stdout_file")"
    max_abs="$(parse_kv max_abs_error "$stdout_file")"
    changed_pct="$(parse_kv changed_pixels_pct "$stdout_file")"
    raw_gl_size="$(parse_kv raw_baseline_size "$stdout_file")"
    raw_metal_size="$(parse_kv raw_candidate_size "$stdout_file")"
    resized="$(parse_kv resized "$stdout_file")"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$ordinal" "$gl_png" "$metal_png" "$mae" "$rms" "$max_abs" "$changed_pct" \
        "$raw_gl_size" "$raw_metal_size" "$resized" \
        >> "$OUT_DIR/diffs/frames.tsv"
}

# --- top-level orchestration ----------------------------------------------

# Run the two benchmarks back-to-back. Either failure is infrastructure
# (exit 2). The launchers themselves refuse to start if another xemu is
# already running, so accidental concurrency is already guarded.
run_gl
run_metal

# Enumerate produced screenshots.
GL_LIST_FILE="$(mktemp -t metal-gl-compare-gl.XXXXXX)"
METAL_LIST_FILE="$(mktemp -t metal-gl-compare-metal.XXXXXX)"
TMPFILES+=("$GL_LIST_FILE" "$METAL_LIST_FILE")

gl_screenshots > "$GL_LIST_FILE"
metal_screenshots > "$METAL_LIST_FILE"

GL_COUNT="$(wc -l < "$GL_LIST_FILE" | tr -d ' ')"
METAL_COUNT="$(wc -l < "$METAL_LIST_FILE" | tr -d ' ')"
log "captured screenshots: gl=$GL_COUNT metal=$METAL_COUNT"

if [[ "$GL_COUNT" -lt 1 || "$METAL_COUNT" -lt 1 ]]; then
    err "no usable screenshots produced (gl=$GL_COUNT metal=$METAL_COUNT)"
    exit 2
fi

# Resolve the frame ordinals to compare. The helper enforces that every
# requested ordinal is in the intersection [1..min(gl,metal)] so we never
# diff against an empty slot.
ORDINALS_FILE="$(mktemp -t metal-gl-compare-ord.XXXXXX)"
TMPFILES+=("$ORDINALS_FILE")
if ! resolve_frames "$GL_COUNT" "$METAL_COUNT" > "$ORDINALS_FILE"; then
    exit 2
fi

# Initialize the per-frame TSV. Header line is documented but not emitted
# so awk parsing in downstream stages is uniform.
: > "$OUT_DIR/diffs/frames.tsv"

# Diff each ordinal. A diff failure is infrastructure (exit 2); a diff
# that simply exceeds the threshold is a PASS/FAIL outcome handled later.
while IFS= read -r ordinal; do
    [[ -z "$ordinal" ]] && continue
    GL_PNG="$(nth_path "$ordinal" < "$GL_LIST_FILE")"
    METAL_PNG="$(nth_path "$ordinal" < "$METAL_LIST_FILE")"
    if [[ -z "$GL_PNG" || -z "$METAL_PNG" ]]; then
        err "missing screenshot at ordinal $ordinal (gl='$GL_PNG' metal='$METAL_PNG')"
        exit 2
    fi
    log "diff frame $ordinal: $(basename "$GL_PNG") vs $(basename "$METAL_PNG")"
    if ! diff_one_frame "$ordinal" "$GL_PNG" "$METAL_PNG"; then
        exit 2
    fi
done < "$ORDINALS_FILE"

# --- perf summary diff -----------------------------------------------------

GL_RUN_DIR="$(cat "$OUT_DIR/gl/run-dir.txt")"
METAL_RUN_DIR="$(cat "$OUT_DIR/metal/run-dir.txt")"
PERF_DIFF_FILE="$OUT_DIR/perf-diff.txt"

log "running compare-runs.sh GL=$GL_RUN_DIR METAL=$METAL_RUN_DIR"
set +e
"$COMPARE_RUNS" "$GL_RUN_DIR" "$METAL_RUN_DIR" > "$PERF_DIFF_FILE" 2>&1
PERF_RC=$?
set -e
# compare-runs.sh exits 1 when the candidate regresses on a perf metric;
# that is a reportable result here, NOT a script-level error. Only treat
# usage errors (rc=2) as infrastructure failures.
if [[ "$PERF_RC" -eq 2 ]]; then
    err "compare-runs.sh usage error; see $PERF_DIFF_FILE"
    exit 2
fi

# --- report.md + summary.json ---------------------------------------------

# Determine PASS/FAIL by walking frames.tsv.
PASS=1
FAIL_LINES=()
while IFS=$'\t' read -r ordinal gl_png metal_png mae rms max_abs changed_pct raw_gl_size raw_metal_size resized; do
    [[ -z "$ordinal" ]] && continue
    # Float compare: candidate's changed_pct vs THRESHOLD.
    if awk -v c="$changed_pct" -v t="$THRESHOLD" 'BEGIN { exit !(c+0 > t+0) }'; then
        PASS=0
        FAIL_LINES+=("frame $ordinal: changed_pixels_pct=$changed_pct > threshold=$THRESHOLD")
    fi
done < "$OUT_DIR/diffs/frames.tsv"

REPORT_MD="$OUT_DIR/report.md"
SUMMARY_JSON="$OUT_DIR/summary.json"

verdict="PASS"
if [[ "$PASS" -eq 0 ]]; then verdict="FAIL"; fi

{
    # `--` after printf ends option processing — bash 3.2 (macOS default)
    # treats a format string starting with `-` as an unknown option, so
    # every leading-dash bullet line below uses `printf --`. Same fix
    # applied in metal-canary-regress.sh's report block.
    printf -- '# %s — metal-gl-compare report\n\n' "$verdict"
    printf -- '- game: %s\n' "$GAME"
    printf -- '- input: %s\n' "$INPUT_CSV"
    printf -- '- duration: %ss\n' "$DURATION"
    printf -- '- threshold: %s%% changed-pixels per frame\n' "$THRESHOLD"
    printf -- '- gl_run: %s\n' "$GL_RUN_DIR"
    printf -- '- metal_run: %s\n' "$METAL_RUN_DIR"
    printf -- '- gl_launcher_log: %s\n' "$GL_LAUNCHER_LOG"
    printf -- '- metal_launcher_log: %s\n' "$METAL_LAUNCHER_LOG"
    if [[ "$TRIGGER" == "flip" ]]; then
        printf -- '- gl_capture: in-renderer XEMU_GL_SCREENSHOT_PATH (display-framebuffer PNG at flip-stall trigger)\n'
    else
        printf -- '- gl_capture: macos screencapture, XEMU_CAPTURE_WINDOW_PATTERN=xemu (window-id-targeted; falls back to full desktop when xemu window not found)\n'
    fi
    printf -- '- metal_capture: in-renderer XEMU_METAL_SCREENSHOT_PATH (pre-HUD NV2A published texture PNG)\n'
    printf -- '- metal_hud: off (--metal-no-hud passed; Metal Performance HUD overlay disabled)\n'
    if [[ "$METAL_VALIDATE" -eq 1 ]]; then
        printf -- '- metal_validation: on (XEMU_METAL_VALIDATION=1 explicit on Metal leg)\n'
    else
        printf -- '- metal_validation: off (--metal-no-validate passed for product-like perf)\n'
    fi
    printf -- '- size_mismatch_policy: --resize smaller (compare-screenshots.py LANCZOS-resizes the larger image down to the smaller dimensions before crop+diff)\n'
    printf -- '- snapshot: %s\n' "${SNAPSHOT_TAG:-(none; cold launch)}"
    printf -- '- evidence_class: %s\n' "$EVIDENCE_CLASS"
    if [[ -n "$SNAPSHOT_TAG" ]]; then
        printf -- '- loadvm_at: %ss\n' "$LOADVM_AT"
    fi
    printf -- '- trigger: %s (ordinal=%s)\n' "$TRIGGER" "$TRIGGER_ORDINAL"
    if [[ "$TRIGGER" == "flip" ]]; then
        if [[ -e "$FLIP_STALL_SENTINEL" ]]; then
            printf -- '- trigger_fired: yes (sentinel %s present)\n' "$FLIP_STALL_SENTINEL"
        else
            printf -- '- trigger_fired: NO (sentinel %s never appeared within %ss)\n' "$FLIP_STALL_SENTINEL" "$DURATION"
        fi
    fi
    printf '\n## Per-frame visual diff\n\n'
    printf '| frame | gl_size | metal_size | resized | mae | rms | max_abs | changed_pct |\n'
    printf '|------:|---------|------------|---------|----:|----:|--------:|------------:|\n'
    while IFS=$'\t' read -r ordinal gl_png metal_png mae rms max_abs changed_pct raw_gl_size raw_metal_size resized; do
        [[ -z "$ordinal" ]] && continue
        printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' \
            "$ordinal" "${raw_gl_size:-?}" "${raw_metal_size:-?}" "${resized:-?}" \
            "$mae" "$rms" "$max_abs" "$changed_pct"
    done < "$OUT_DIR/diffs/frames.tsv"
    if [[ "$PASS" -eq 0 ]]; then
        printf '\n## Frames over threshold\n\n'
        for line in "${FAIL_LINES[@]}"; do
            printf -- '- %s\n' "$line"
        done
    fi
    printf '\n## Perf-summary diff (compare-runs.sh)\n\n'
    printf '```\n'
    cat "$PERF_DIFF_FILE"
    printf '```\n'
    printf '\n_Generated by `scripts/apple-silicon/metal-gl-compare.sh` (slice W2)._\n'
} > "$REPORT_MD"

# Build summary.json. We hand-roll the JSON in pure bash to avoid taking
# a hard dep on jq/python3 for the final emit step (the diff stage already
# uses python3, but only when CROP defaults; the JSON stage runs even on
# trivial paths).
{
    printf '{\n'
    printf '  "pass": %s,\n' "$([[ $PASS -eq 1 ]] && echo true || echo false)"
    printf '  "verdict": "%s",\n' "$verdict"
    printf '  "game": "%s",\n' "$GAME"
    printf '  "duration_seconds": %s,\n' "$DURATION"
    printf '  "threshold": %s,\n' "$THRESHOLD"
    printf '  "gl_run_dir": "%s",\n' "$GL_RUN_DIR"
    printf '  "metal_run_dir": "%s",\n' "$METAL_RUN_DIR"
    printf '  "perf_summary_path": "%s",\n' "$PERF_DIFF_FILE"
    if [[ "$TRIGGER" == "flip" ]]; then
        printf '  "gl_capture": "in-renderer-gl-display-png",\n'
    else
        printf '  "gl_capture": "macos-screencapture-window-targeted",\n'
    fi
    printf '  "metal_capture": "in-renderer-nv2a-png",\n'
    printf '  "metal_hud": "off",\n'
    if [[ "$METAL_VALIDATE" -eq 1 ]]; then
        printf '  "metal_validation": "on",\n'
    else
        printf '  "metal_validation": "off",\n'
    fi
    printf '  "size_mismatch_policy": "resize-smaller",\n'
    printf '  "evidence_class": "%s",\n' "$EVIDENCE_CLASS"
    if [[ "$EVIDENCE_CLASS" == "gameplay" ]]; then
        printf '  "gameplay_evidence": true,\n'
    else
        printf '  "gameplay_evidence": false,\n'
    fi
    printf '  "snapshot": "%s",\n' "${SNAPSHOT_TAG:-}"
    printf '  "loadvm_at_seconds": %s,\n' "${LOADVM_AT:-0}"
    printf '  "trigger": "%s",\n' "$TRIGGER"
    printf '  "trigger_ordinal": %s,\n' "$TRIGGER_ORDINAL"
    if [[ "$TRIGGER" == "flip" ]]; then
        if [[ -e "$FLIP_STALL_SENTINEL" ]]; then
            printf '  "trigger_fired": true,\n'
        else
            printf '  "trigger_fired": false,\n'
        fi
    fi
    printf '  "frames": [\n'
    first=1
    while IFS=$'\t' read -r ordinal gl_png metal_png mae rms max_abs changed_pct raw_gl_size raw_metal_size resized; do
        [[ -z "$ordinal" ]] && continue
        if [[ $first -eq 1 ]]; then first=0; else printf ',\n'; fi
        printf '    { "index": %s, "mae": %s, "rms": %s, "max_abs": %s, "changed_pct": %s, "gl_png": "%s", "metal_png": "%s", "raw_gl_size": "%s", "raw_metal_size": "%s", "resized": "%s" }' \
            "$ordinal" "$mae" "$rms" "$max_abs" "$changed_pct" "$gl_png" "$metal_png" \
            "${raw_gl_size:-unknown}" "${raw_metal_size:-unknown}" "${resized:-unknown}"
    done < "$OUT_DIR/diffs/frames.tsv"
    printf '\n  ]\n'
    printf '}\n'
} > "$SUMMARY_JSON"

log "wrote $REPORT_MD"
log "wrote $SUMMARY_JSON"
log "verdict: $verdict"

if [[ "$PASS" -eq 1 ]]; then
    exit 0
fi
exit 1
