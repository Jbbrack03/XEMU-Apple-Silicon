#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 out-dir duration-seconds interval-seconds start-delay-seconds" >&2
    exit 2
fi

OUT_DIR="$1"
DURATION="$2"
INTERVAL="$3"
START_DELAY="$4"

# W6 (2026-05-04): opt-in xemu-window-only capture. When
# XEMU_CAPTURE_WINDOW_PATTERN is set (typically to "xemu"), the
# capture loop resolves the window-id of the matching on-screen
# window via Quartz at the start of each capture cycle and runs
# `screencapture -l <wid>` instead of full-desktop `screencapture -x`.
# A fresh lookup per cycle handles xemu raising/lowering or being
# moved between displays during the run. If the pattern matches no
# window, falls back to full-desktop capture for that cycle (logs
# the fallback). When the env var is unset, behavior is identical
# to the pre-W6 full-desktop path. See metal-gl-compare.sh for the
# matching size-normalization safety net.
WINDOW_PATTERN="${XEMU_CAPTURE_WINDOW_PATTERN:-}"

# F1 (2026-05-04): opt-in flip-stall-driven one-shot capture. When
# XEMU_CAPTURE_FLIP_STALL_SENTINEL is set, the loop polls for the
# sentinel file's appearance and takes a SINGLE shot the moment it
# appears (the file is created by xemu's NV097_FLIP_STALL handler
# on the Nth flip_stall — see util/xemu-display-perf.c). The interval
# / start-delay scheduler is bypassed; the loop still respects
# DURATION as an upper bound so a missing trigger doesn't hang the
# benchmark. When the env var is unset, behavior is identical to
# the pre-F1 wallclock-based interval path.
FLIP_STALL_SENTINEL="${XEMU_CAPTURE_FLIP_STALL_SENTINEL:-}"

mkdir -p "$OUT_DIR"

python3 - "$OUT_DIR" "$DURATION" "$INTERVAL" "$START_DELAY" "$WINDOW_PATTERN" "$FLIP_STALL_SENTINEL" <<'PY'
import os
import subprocess
import sys
import time
from pathlib import Path

out_dir = Path(sys.argv[1])
duration = float(sys.argv[2])
interval = float(sys.argv[3])
start_delay = float(sys.argv[4])
window_pattern = sys.argv[5] if len(sys.argv) > 5 else ""
flip_stall_sentinel = sys.argv[6] if len(sys.argv) > 6 else ""

_quartz_module = None
_quartz_unavailable_logged = False


def _load_quartz():
    """Lazy-load Quartz. Returns the module or None when unavailable."""
    global _quartz_module, _quartz_unavailable_logged
    if _quartz_module is not None:
        return _quartz_module
    if _quartz_module is False:
        return None
    try:
        import Quartz  # type: ignore
    except ImportError:
        if not _quartz_unavailable_logged:
            print(
                "macos-capture: Quartz unavailable; "
                "XEMU_CAPTURE_WINDOW_PATTERN ignored, falling back to full-desktop",
                flush=True,
            )
            _quartz_unavailable_logged = True
        _quartz_module = False
        return None
    _quartz_module = Quartz
    return Quartz


def find_window_id(pattern):
    """Return the Quartz window-id of the first on-screen window whose
    process name or window name contains the pattern, or None when
    nothing matches. Pattern match is case-insensitive substring."""
    Quartz = _load_quartz()
    if Quartz is None:
        return None
    options = (
        Quartz.kCGWindowListOptionOnScreenOnly
        | Quartz.kCGWindowListExcludeDesktopElements
    )
    windows = Quartz.CGWindowListCopyWindowInfo(options, Quartz.kCGNullWindowID)
    needle = pattern.lower()
    for win in windows:
        owner = (win.get("kCGWindowOwnerName") or "").lower()
        name = (win.get("kCGWindowName") or "").lower()
        if needle in owner or needle in name:
            wid = win.get("kCGWindowNumber")
            if wid is not None:
                return int(wid)
    return None


def capture_one(filename):
    """Capture a single PNG. Returns (returncode, command_used)."""
    if window_pattern:
        wid = find_window_id(window_pattern)
        if wid is not None:
            cmd = ["screencapture", "-x", "-l", str(wid), str(filename)]
            result = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            return result, "window:%d" % wid
        # Pattern given but no match this cycle — fall through to full-desktop.
    cmd = ["screencapture", "-x", str(filename)]
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result, "fullscreen"


start = time.monotonic()
next_capture = start + start_delay
end = start + duration
index = 0

# F1 — flip-stall-driven one-shot mode.  When the sentinel path is
# given, the loop polls every 100 ms for the file's appearance and
# takes a single shot the moment it shows up.  The duration is still
# honored as an upper bound so a missing/late trigger does not hang
# the benchmark; we exit normally without a capture in that case
# (downstream metal-gl-compare.sh treats zero-shots as INFRA-FAIL).
if flip_stall_sentinel:
    sentinel = Path(flip_stall_sentinel)
    print(
        f"macos-capture: flip-stall sentinel mode sentinel={sentinel} "
        f"timeout={duration}s",
        flush=True,
    )
    while time.monotonic() < end:
        if sentinel.exists():
            elapsed = int((time.monotonic() - start) * 1000)
            filename = out_dir / f"flip-stall-{elapsed:06d}ms.png"
            result, source = capture_one(filename)
            if result.returncode == 0:
                print(
                    f"captured {filename} source={source} trigger=flip-stall",
                    flush=True,
                )
            else:
                print(
                    f"capture failed at {elapsed}ms source={source} "
                    f"trigger=flip-stall: {result.stdout.strip()}",
                    flush=True,
                )
            # One-shot: exit the loop after the trigger fires.  Sleep
            # out the remaining duration so the parent's `wait
            # CAPTURE_PID` does not race xemu's shutdown.
            remaining = end - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
            break
        time.sleep(0.1)
    else:
        print(
            f"macos-capture: flip-stall sentinel did not appear within "
            f"{duration}s; no shot taken",
            flush=True,
        )
else:
    while time.monotonic() < end:
        now = time.monotonic()
        if now < next_capture:
            time.sleep(min(0.25, next_capture - now))
            continue

        elapsed = int((now - start) * 1000)
        filename = out_dir / f"{index:03d}-{elapsed:06d}ms.png"
        result, source = capture_one(filename)
        if result.returncode == 0:
            print(f"captured {filename} source={source}", flush=True)
        else:
            print(
                f"capture failed at {elapsed}ms source={source}: "
                f"{result.stdout.strip()}",
                flush=True,
            )

        index += 1
        next_capture += interval
PY
