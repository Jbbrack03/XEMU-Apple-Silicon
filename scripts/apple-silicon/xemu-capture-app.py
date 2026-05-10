#!/usr/bin/env python3
"""Run xemu-capture through its macOS app bundle identity.

macOS TCC Camera grants are keyed to the launched application's code
identity. Invoking Contents/MacOS/xemu-capture directly can bypass the
LaunchServices app identity that the user approved in System Settings, so
automation must enter through the .app bundle.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_APP = ROOT / "tools/xemu-capture/dist/xemu-capture.app"


def normalize_args(args: list[str]) -> list[str]:
    """Make path arguments safe for a LaunchServices app with unknown cwd."""
    out: list[str] = []
    i = 0
    while i < len(args):
        arg = args[i]
        out.append(arg)
        if arg in ("--out", "--out-prefix") and i + 1 < len(args):
            path = Path(args[i + 1]).expanduser().resolve()
            path.parent.mkdir(parents=True, exist_ok=True)
            out.append(str(path))
            i += 2
            continue
        i += 1
    return out


def command_timeout(args: list[str]) -> float:
    if args[:2] == ["auth", "--request"] or args[:1] == ["auth"] and "--request" in args:
        return 75.0
    timeout = 15.0
    for i, arg in enumerate(args):
        if arg == "--timeout" and i + 1 < len(args):
            try:
                timeout = max(timeout, float(args[i + 1]) + 12.0)
            except ValueError:
                pass
    return timeout


def parse_json(stdout: str) -> dict[str, Any] | None:
    try:
        data = json.loads(stdout)
    except json.JSONDecodeError:
        return None
    return data if isinstance(data, dict) else None


def wait_for_json(stdout_path: Path, stderr_path: Path, deadline: float) -> dict[str, Any] | None:
    last_size = -1
    stable_since = time.monotonic()
    while time.monotonic() < deadline:
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        parsed = parse_json(stdout)
        if parsed is not None:
            return parsed

        size = len(stdout) + len(stderr_path.read_bytes())
        if size != last_size:
            last_size = size
            stable_since = time.monotonic()
        elif size > 0 and time.monotonic() - stable_since > 1.0:
            return None

        time.sleep(0.1)
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--app", type=Path, default=DEFAULT_APP)
    parser.add_argument("--wait-s", type=float)
    parser.add_argument("-h", "--help", action="store_true")
    ns, capture_args = parser.parse_known_args(argv)

    if ns.help or not capture_args:
        print(
            "usage: xemu-capture-app.py [--app PATH] [--wait-s SECONDS] "
            "<xemu-capture args...>",
            file=sys.stderr,
        )
        return 2 if not capture_args else 0

    app = ns.app.expanduser().resolve()
    if not app.exists():
        print(f"xemu-capture app bundle not found: {app}", file=sys.stderr)
        return 127

    capture_args = normalize_args(capture_args)
    wait_s = ns.wait_s if ns.wait_s is not None else command_timeout(capture_args)

    with tempfile.TemporaryDirectory(prefix="xemu-capture-app-") as tmp:
        stdout_path = Path(tmp) / "stdout.json"
        stderr_path = Path(tmp) / "stderr.txt"
        stdout_path.touch()
        stderr_path.touch()

        open_cmd = [
            "/usr/bin/open",
            "-n",
            str(app),
            "--stdout",
            str(stdout_path),
            "--stderr",
            str(stderr_path),
            "--args",
            *capture_args,
        ]
        launched = subprocess.run(
            open_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if launched.stdout:
            print(launched.stdout, end="", file=sys.stderr)
        if launched.stderr:
            print(launched.stderr, end="", file=sys.stderr)
        if launched.returncode != 0:
            return launched.returncode

        parsed = wait_for_json(stdout_path, stderr_path, time.monotonic() + wait_s)
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace")

        if stdout:
            print(stdout, end="" if stdout.endswith("\n") else "\n")
        if stderr:
            print(stderr, end="" if stderr.endswith("\n") else "\n", file=sys.stderr)

        if parsed is None:
            print(
                f"xemu-capture app did not return JSON within {wait_s:.1f}s",
                file=sys.stderr,
            )
            return 124
        return 0 if parsed.get("status") == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
