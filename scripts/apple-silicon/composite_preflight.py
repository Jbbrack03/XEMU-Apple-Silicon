"""Synchronous composite-preflight gate for orchestration wrappers (cycle 34).

Thin host-side helper around ``composite-preflight.sh`` so the three wrappers
that background ``composite-record.sh`` can abort BEFORE issuing the Xbox-side
launch when the cycle-32 OUTCOME F8 silent-stall failure mode is present (no
live MS2109 signal, no enumerated device, no detector backend).

Cycle-33 wired the same probe into ``composite-record.sh`` itself, but those
wrappers launch the recorder in the background and then issue ``runxbe`` /
``SITE EXEC`` after a fixed ``sleep 2`` — so a failing preflight inside the
backgrounded recorder kills only the recorder leg while the Xbox-side leg
fires blindly. Cycle 34 moves the gate to a synchronous pre-launch step in
each wrapper. The shared helper here keeps the Python call sites identical
between ``retail-title-automation-proof.py`` and ``retail-gameplay-oracle.py``
and the only inputs are values the wrappers already accept (device, audio
device, frame format, timeout / mode opt-outs).

The bash wrapper (``capture-composite-reference.sh``) inlines the same call
via the shell, so this module is Python-only.
"""

from __future__ import annotations

import json
import os
import subprocess
import time
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
DEFAULT_PREFLIGHT = HERE / "composite-preflight.sh"


def env_int(name: str, default: int, min_value: int = 1) -> int:
    """Parse ``$name`` as int; fall back to ``default`` on empty / unset / non-int / below-min.

    Cycle 34 Codex round-14 low adopted: ``int(os.environ.get(name, "8"))``
    eagerly raised ``ValueError`` on an empty env var (``""``), crashing
    argparse defaults BEFORE ``--help`` could render.
    Cycle 34 Codex round-18 low adopted: also reject values below
    ``min_value`` (default 1) so a `COMPOSITE_PREFLIGHT_TIMEOUT=0` or
    negative value falls back to the default at the wrapper layer instead
    of leaking through to ``composite-preflight.sh`` (which rejects them
    with rc=5 invalid_cli). Mirrors the shell wrapper's ^[1-9][0-9]*$
    validation.
    """
    raw = os.environ.get(name) or ""
    if not raw.strip():
        return default
    try:
        value = int(raw)
    except ValueError:
        return default
    if value < min_value:
        return default
    return value


def env_str(name: str, default: str, choices: tuple[str, ...] | None = None) -> str:
    """Like ``env_int`` but for strings; empty / unset falls back to default.

    When ``choices`` is provided, values outside the set fall back to
    ``default`` too. Cycle 34 Codex round-16 low adopted: argparse does NOT
    validate ``default=`` against ``choices=``, so a garbage env default
    would silently propagate past the wrapper's argparse layer and only
    fail later inside ``composite-preflight.sh`` with rc=5 invalid_cli.
    Filtering here keeps the failure local + loud at the wrapper layer
    instead of leaking through to the shell preflight.
    """
    raw = os.environ.get(name) or ""
    if not raw.strip():
        return default
    if choices is not None and raw not in choices:
        return default
    return raw


def run_preflight(
    out_dir: Path,
    *,
    device: str = "USB2",
    audio_device: str = "USB2",
    width: int = 720,
    height: int = 480,
    fps: int = 30,
    pixel_format: str = "uyvy422",
    timeout: int = 8,
    mode: str = "auto",
    no_audio: bool = False,
    preflight_bin: Path | None = None,
) -> dict[str, Any]:
    """Run ``composite-preflight.sh`` synchronously and return a summary dict.

    The returned dict always contains:

    * ``ok`` (bool): ``True`` when the preflight reported a signal-detected
      result; ``False`` for any non-zero exit OR launch failure.
    * ``exit_code`` (int): the preflight's exit code, normalized so the
      Python helper's contract matches ``composite-preflight.sh``'s rc
      enumeration (0 ok / 2 no_signal / 3 device_not_found / 4 no_backend /
      5 invalid_cli / 1 host-side error). Launcher missing maps to 4
      (no_backend); spawn-failure maps to 1 (host-side error).
    * ``status`` (str): one of ``ok``, ``no_signal``, ``device_not_found``,
      ``no_backend``, ``invalid_cli``, ``error``, ``launcher_missing``.
    * ``detector`` (str): which detector produced the outcome
      (``xemu-capture`` / ``ffmpeg`` / ``none``).
    * ``elapsed_s`` (float | None): wall-clock seconds spent in the preflight.
    * ``detail`` (str): human-readable summary line.
    * ``meta_path`` (str | None): absolute path to ``preflight-meta.json`` when
      one was written.
    * ``meta`` (dict | None): the parsed preflight meta when available.
    * ``cmd`` (list[str]): the argv used to spawn the preflight.
    * ``wrapper_elapsed_s`` (float): wall-clock seconds including the
      ``subprocess.run`` overhead.

    ``out_dir`` is created if necessary; the preflight writes
    ``preflight-meta.json`` and supporting logs underneath.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    preflight_bin = Path(preflight_bin) if preflight_bin else DEFAULT_PREFLIGHT

    if not preflight_bin.exists() or not os.access(preflight_bin, os.X_OK):
        return {
            "ok": False,
            "exit_code": 4,
            "status": "launcher_missing",
            "detector": "none",
            "elapsed_s": None,
            "detail": (
                f"composite-preflight.sh missing or not executable at "
                f"{preflight_bin}"
            ),
            "meta_path": None,
            "meta": None,
            "cmd": [str(preflight_bin)],
            "wrapper_elapsed_s": 0.0,
        }

    cmd = [
        str(preflight_bin),
        "--device", str(device),
        "--audio-device", str(audio_device),
        "--width", str(width),
        "--height", str(height),
        "--fps", str(fps),
        "--pixel-format", str(pixel_format),
        "--timeout", str(timeout),
        "--mode", str(mode),
        "--out-dir", str(out_dir),
        "--quiet",
    ]
    if no_audio:
        cmd.append("--no-audio")

    started = time.time()
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        rc = proc.returncode
        stdout = proc.stdout
        stderr = proc.stderr
    except Exception as exc:
        return {
            "ok": False,
            "exit_code": 1,
            "status": "error",
            "detector": "none",
            "elapsed_s": None,
            "detail": f"failed to spawn composite-preflight.sh: {exc}",
            "meta_path": None,
            "meta": None,
            "cmd": cmd,
            "wrapper_elapsed_s": round(time.time() - started, 3),
        }

    meta_path = out_dir / "preflight-meta.json"
    meta: dict[str, Any] | None = None
    if meta_path.exists():
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            meta = None

    status = (meta or {}).get("status") if meta else None
    detector = (meta or {}).get("detector") if meta else None
    elapsed_s = (meta or {}).get("elapsed_s") if meta else None
    detail = (meta or {}).get("detail") if meta else None
    if not status:
        status = {
            0: "ok",
            2: "no_signal",
            3: "device_not_found",
            4: "no_backend",
            5: "invalid_cli",
        }.get(rc, "error")
    if not detector:
        detector = "none"
    if not detail:
        tail = (stderr or stdout or "").strip().splitlines()[-1:]
        detail = tail[0] if tail else f"composite-preflight rc={rc}"

    return {
        "ok": rc == 0,
        "exit_code": rc,
        "status": status,
        "detector": detector,
        "elapsed_s": float(elapsed_s) if isinstance(elapsed_s, (int, float)) else None,
        "detail": detail,
        "meta_path": str(meta_path) if meta_path.exists() else None,
        "meta": meta,
        "cmd": cmd,
        "wrapper_elapsed_s": round(time.time() - started, 3),
        "stdout_tail": (stdout or "").splitlines()[-3:],
        "stderr_tail": (stderr or "").splitlines()[-3:],
    }


def preflight_blocking_reason(result: dict[str, Any]) -> str:
    """Format a one-line ``failed_reasons``/``blocked_reasons`` entry."""
    status = result.get("status", "error")
    rc = result.get("exit_code", "?")
    detector = result.get("detector", "none")
    elapsed = result.get("elapsed_s")
    if isinstance(elapsed, (int, float)):
        return (
            f"composite-preflight {status} rc={rc} detector={detector} "
            f"elapsed={elapsed:.3f}s"
        )
    return f"composite-preflight {status} rc={rc} detector={detector}"
