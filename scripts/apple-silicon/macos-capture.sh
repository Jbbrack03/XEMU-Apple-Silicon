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

# 2026-05-05 (Codex review followup): opt-in strict window mode for
# paired-diff use. When XEMU_CAPTURE_WINDOW_REQUIRED=1, a Quartz
# unavailability OR a window-id lookup miss after backoff retries
# fails the capture (exit 3) instead of silently falling back to
# full-desktop screencapture. Full-desktop fallback for the GL leg
# of metal-gl-compare.sh has been a recurring source of meaningless
# ~70% changed_pct FAILs (the GL screenshot is then macOS desktop
# chrome compared against the Metal in-renderer drawable). Default
# off so the pre-W6 / pre-F1 use cases keep their tolerant behavior.
WINDOW_REQUIRED="${XEMU_CAPTURE_WINDOW_REQUIRED:-0}"

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

python3 - "$OUT_DIR" "$DURATION" "$INTERVAL" "$START_DELAY" "$WINDOW_PATTERN" "$FLIP_STALL_SENTINEL" "$WINDOW_REQUIRED" <<'PY'
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
window_required = bool(int((sys.argv[7] if len(sys.argv) > 7 else "0") or "0"))

_quartz_module = None
_quartz_unavailable_logged = False


def _load_quartz():
    """Lazy-load Quartz. Returns the module or None when unavailable.

    Caches the failed-load sentinel as False so repeated calls
    don't pay the import cost on every retry. None is the
    "not yet attempted" sentinel; False is "tried, missing"."""
    global _quartz_module, _quartz_unavailable_logged
    if _quartz_module is False:
        return None
    if _quartz_module is not None:
        return _quartz_module
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
    """Capture a single PNG. Returns (returncode, command_used) or
    (None, "window-required-failed") to signal an INFRA failure when
    XEMU_CAPTURE_WINDOW_REQUIRED=1 and the window-id lookup fails.

    When a window pattern is set, retry window-id lookup with backoff
    (0.05/0.1/0.2/0.4s = up to ~0.75s total) before falling back.
    Quartz CGWindowListCopyWindowInfo can briefly miss a freshly-
    created xemu window during cold launch (the F1 flip-stall trigger
    fires within the first few seconds and can race the AppKit
    window registration), and a full-desktop fallback at that moment
    captures the macOS desktop with a tiny black xemu rect — useless
    for paired diff.

    NOTE: The F3 patch (window-ID re-verification immediately before
    screencapture) was removed because it did not address the root
    cause of the capture failure. The actual blocker is that
    screencapture -l <wid> fails with "could not create image from
    window" when the process is launched via SSH on macOS. This is
    a fundamental macOS security design: the screen is protected by
    the WindowServer, and only processes in the same security session
    as the GUI can access the screen. An SSH-launched process is in
    a separate security session and cannot image the screen.

    The fix for this requires either:
    (a) Running the benchmark in the GUI session (not via SSH), or
    (b) Using a different capture mechanism that works in the SSH
        context (e.g., VNC or a remote desktop protocol).

    Neither of these is in scope for the current task. The F3 patch
    was cosmetic and did not handle the case where the actual
    screencapture command fails. It has been reverted.
    """
    if window_pattern:
        wid = None
        for backoff_s in (0.0, 0.05, 0.1, 0.2, 0.4):
            if backoff_s > 0:
                time.sleep(backoff_s)
            wid = find_window_id(window_pattern)
            if wid is not None:
                break
        if wid is not None:
            # Window found — attempt capture. Note: on macOS,
            # screencapture -l <wid> fails when the process is
            # launched via SSH due to WindowServer security
            # isolation. See the docstring for details.
            cmd = ["screencapture", "-x", "-l", str(wid), str(filename)]
            result = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            return result, "window:%d" % wid
        # Pattern given but no match after retries.
        if window_required:
            print(
                f"macos-capture: WINDOW_REQUIRED=1 and window-id lookup failed "
                f"for pattern={window_pattern!r}; refusing full-desktop fallback",
                flush=True,
            )
            return None, "window-required-failed"
        # Fall back to full-desktop capture.
        cmd = ["screencapture", "-x", str(filename)]
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        return result, "fullscreen"
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
            if result is None:
                # WINDOW_REQUIRED strict mode failure — already logged
                # in capture_one. Exit nonzero so the parent harness
                # treats the leg as INFRA-FAIL instead of producing a
                # meaningless full-desktop capture for paired diff.
                sys.exit(3)
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
            # out the remaining duration so the parent's  does not race xemu's shutdown.
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
        if result is None:
            # WINDOW_REQUIRED strict mode failure (interval mode).
            # Treat as INFRA-FAIL so the parent harness sees nonzero.
            sys.exit(3)
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
