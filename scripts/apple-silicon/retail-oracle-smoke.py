#!/usr/bin/env python3
"""Preflight the real-Xbox retail-game oracle smoke.

This gate exists to keep the oracle honest. A retail-game smoke is only
production-valid when all four legs are available on the physical Xbox:

  1. launch a retail game,
  2. deliver the recorded controller automation into the title-facing input
     path, not just the oracle agent's synthetic buffer,
  3. capture composite video and extract keyframes, and
  4. exit the game and return to the dashboard without human intervention.

When those legs are not all proven, this script writes a structured
`verdict.json` and exits non-zero. It deliberately does not launch a retail
game from an unsafe state, because doing so can strand the Xbox in-game with
the oracle agent gone and no autonomous reset path.
"""

from __future__ import annotations

import argparse
import csv
import ftplib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_HOST = os.environ.get("ORACLE_HOST", "192.168.0.200")
DEFAULT_FTP_USER = os.environ.get("ORACLE_FTP_USER", "xbox")
DEFAULT_FTP_PASS = os.environ.get("ORACLE_FTP_PASS", "xbox")


def run(argv: list[str], timeout: float = 30.0) -> dict[str, Any]:
    try:
        p = subprocess.run(
            argv,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=False,
        )
        return {
            "cmd": argv,
            "rc": p.returncode,
            "stdout": p.stdout,
            "stderr": p.stderr,
        }
    except Exception as exc:
        return {"cmd": argv, "rc": None, "error": str(exc)}


def parse_csv(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {"path": str(path), "exists": path.exists()}
    if not path.exists():
        return result
    first_ms: int | None = None
    last_ms: int | None = None
    count = 0
    controls: set[str] = set()
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                row = next(csv.reader([line.replace("\t", ",")]))
                if len(row) != 3:
                    parts = line.split()
                    if len(parts) != 3:
                        raise ValueError(f"bad row: {raw.rstrip()!r}")
                    row = parts
                t = int(row[0])
                control = row[1].strip().lower()
                _ = int(row[2])
                count += 1
                controls.add(control)
                first_ms = t if first_ms is None else min(first_ms, t)
                last_ms = t if last_ms is None else max(last_ms, t)
    except Exception as exc:
        result["parse_error"] = str(exc)
        return result
    result.update(
        {
            "events": count,
            "first_ms": first_ms,
            "last_ms": last_ms,
            "duration_s": None
            if first_ms is None or last_ms is None
            else round((last_ms - first_ms) / 1000.0, 3),
            "controls": sorted(controls),
        }
    )
    return result


def ftp_list_games(host: str, roots: list[str]) -> dict[str, Any]:
    out: dict[str, Any] = {"host": host, "roots": roots, "games": []}
    try:
        ftp = ftplib.FTP(host, timeout=8)
        ftp.login(DEFAULT_FTP_USER, DEFAULT_FTP_PASS)
    except Exception as exc:
        out["error"] = str(exc)
        return out
    try:
        for root in roots:
            try:
                entries: list[str] = []
                ftp.cwd(root)
                ftp.retrlines("LIST", entries.append)
            except Exception as exc:
                out.setdefault("root_errors", {})[root] = str(exc)
                continue
            for line in entries:
                parts = line.split(maxsplit=8)
                if len(parts) < 9 or not parts[0].startswith("d"):
                    continue
                name = parts[8]
                if name in (".", ".."):
                    continue
                xbe = root.rstrip("/") + "/" + name + "/default.xbe"
                try:
                    size = ftp.size(xbe)
                    out["games"].append({"name": name, "xbe": xbe, "default_xbe_size": size})
                except Exception:
                    pass
    finally:
        try:
            ftp.quit()
        except Exception:
            pass
    return out


def capture_device_probe() -> dict[str, Any]:
    capture_tool = ROOT / "scripts/apple-silicon/xemu-capture-app.py"
    if capture_tool.exists():
        return run([sys.executable, str(capture_tool), "list"], timeout=15.0)
    ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
    return run(
        [ffmpeg, "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
        timeout=15.0,
    )


def evidence_ok(path: str | None, expected_status: str = "ok") -> bool:
    if not path:
        return False
    p = Path(path)
    if not p.exists():
        return False
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return False
    return data.get("status") == expected_status or data.get("verdict") == expected_status


def write_markdown(report: dict[str, Any], path: Path) -> None:
    lines: list[str] = [
        "# retail-oracle-smoke",
        "",
        f"Generated: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
        "",
        f"Verdict: **{report['verdict']}**",
        "",
        "## Requested route",
        "",
        f"- game_xbe: `{report.get('game_xbe')}`",
        f"- input_csv: `{report.get('input_csv')}`",
        f"- capture_duration_s: `{report.get('capture_duration_s')}`",
        "",
        "## Gates",
        "",
    ]
    for gate in report["gates"]:
        lines.append(f"- {gate['status'].upper()} `{gate['name']}`: {gate['message']}")
    if report.get("blocked_reasons"):
        lines.extend(["", "## Blocked Reasons", ""])
        for reason in report["blocked_reasons"]:
            lines.append(f"- {reason}")
    lines.extend(
        [
            "",
            "## Safety",
            "",
            "This tool did not launch the retail XBE because the autonomous "
            "input and exit paths are not both proven.",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--game-xbe", required=True, help="Xbox path to retail default.xbe")
    parser.add_argument("--input-csv", required=True, help="Recorded xemu input route")
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--out", default=None)
    parser.add_argument("--input-backend", choices=("none", "tier2-hook", "hardware"), default="none")
    parser.add_argument("--input-evidence", help="JSON proof for the chosen title-facing input backend")
    parser.add_argument("--exit-backend", choices=("none", "controller-igr", "power-cycle-relay"), default="none")
    parser.add_argument("--exit-evidence", help="JSON proof for autonomous dashboard return")
    parser.add_argument("--list-games", action="store_true", help="Also list installed FTP games when dashboard FTP is up")
    args = parser.parse_args(argv)

    out_dir = Path(args.out) if args.out else ROOT / "benchmark-runs" / (
        "retail-oracle-smoke-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    csv_summary = parse_csv(Path(args.input_csv))
    status = run([sys.executable, str(HERE / "oracle-orchestrator.py"), "--host", args.host, "status"])
    capture_probe = capture_device_probe()

    gates: list[dict[str, str]] = []
    blocked: list[str] = []

    def gate(name: str, ok: bool, message: str, block: str | None = None) -> None:
        gates.append({"name": name, "status": "pass" if ok else "fail", "message": message})
        if not ok and block:
            blocked.append(block)

    gate(
        "input-csv",
        csv_summary.get("exists") and csv_summary.get("events", 0) > 0 and "parse_error" not in csv_summary,
        f"{csv_summary.get('events', 0)} events; duration={csv_summary.get('duration_s')}s"
        if "parse_error" not in csv_summary
        else f"parse error: {csv_summary.get('parse_error')}",
        "The requested controller automation CSV is missing or invalid.",
    )
    gate(
        "xbox-reachable",
        status.get("rc") == 0,
        "oracle-orchestrator status returned rc=0" if status.get("rc") == 0 else "Xbox/oracle status is not green",
        "The Xbox is not reachable enough to begin a retail oracle run.",
    )
    gate(
        "composite-capture-device",
        "AV TO USB" in (capture_probe.get("stdout", "") + capture_probe.get("stderr", "")),
        "AVFoundation capture device is visible",
        "The composite capture device is not visible, so keyframes cannot be captured.",
    )
    gate(
        "title-facing-input-backend",
        args.input_backend != "none" and evidence_ok(args.input_evidence),
        f"backend={args.input_backend}; evidence={args.input_evidence or '<none>'}",
        "The oracle only has a synthetic state buffer today; no proven backend injects it into a retail game's XInput/XID read path.",
    )
    gate(
        "autonomous-exit-backend",
        args.exit_backend != "none" and evidence_ok(args.exit_evidence),
        f"backend={args.exit_backend}; evidence={args.exit_evidence or '<none>'}",
        "There is no proven autonomous return-to-dashboard path once a retail game is running.",
    )

    report: dict[str, Any] = {
        "schema": "retail-oracle-smoke-v1",
        "host": args.host,
        "game_xbe": args.game_xbe,
        "input_csv": str(Path(args.input_csv).resolve()),
        "capture_duration_s": args.duration,
        "input_backend": args.input_backend,
        "exit_backend": args.exit_backend,
        "csv": csv_summary,
        "oracle_status": status,
        "capture_probe": capture_probe,
        "gates": gates,
        "blocked_reasons": blocked,
    }
    if args.list_games:
        report["ftp_games"] = ftp_list_games(args.host, ["/F/Games", "/G/Games", "/E/Games"])
    report["verdict"] = "ok" if not blocked else "blocked"

    (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    write_markdown(report, out_dir / "report.md")
    print(json.dumps({"verdict": report["verdict"], "out_dir": str(out_dir), "blocked_reasons": blocked}, indent=2))
    return 0 if report["verdict"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
