#!/usr/bin/env python3
"""Run and parse the controller-readback Tier-2 preflight XBE."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / "scripts/apple-silicon"


def parse_report(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        out[key.strip()] = value.strip()
    return out


def run_orchestrator(host: str, xbe: str, ftp_collect: str, out_dir: Path) -> int:
    cmd = [
        sys.executable,
        str(HERE / "oracle-orchestrator.py"),
        "--host", host,
        "run-diag",
        "--xbe", xbe,
        "--ftp-collect", ftp_collect,
        "--out", str(out_dir / "orch"),
    ]
    with (out_dir / "orchestrator.log").open("w", encoding="utf-8") as log:
        return subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=False).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.200")
    parser.add_argument("--xbe", default=r"E:\Apps\controller-readback\default.xbe")
    parser.add_argument("--ftp-collect", default="/E/Apps/controller-readback")
    parser.add_argument("--out", type=Path,
                        default=ROOT / "benchmark-runs/controller-readback-preflight")
    parser.add_argument("--no-run", action="store_true",
                        help="Parse an existing --out/orch artifact directory.")
    parser.add_argument("--expect", action="append", default=[],
                        help="Required key=value in controller-readback.txt. May repeat.")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    rc = 0
    if not args.no_run:
        rc = run_orchestrator(args.host, args.xbe, args.ftp_collect, args.out)

    report = args.out / "orch/artifacts/controller-readback.txt"
    summary: dict[str, Any] = {
        "schema": "controller-readback-validate-v1",
        "host": args.host,
        "xbe": args.xbe,
        "ftp_collect": args.ftp_collect,
        "out": str(args.out.resolve()),
        "orchestrator_rc": rc,
        "report_path": str(report),
        "report_found": report.exists(),
        "expectations": args.expect,
        "mismatches": [],
    }
    if not report.exists():
        summary["verdict"] = "fail"
    else:
        data = parse_report(report)
        summary["report"] = data
        for item in args.expect:
            if "=" not in item:
                summary["mismatches"].append(f"malformed expectation: {item}")
                continue
            key, expected = item.split("=", 1)
            observed = data.get(key)
            if observed != expected:
                summary["mismatches"].append(
                    f"{key}: expected {expected!r}, observed {observed!r}")
        summary["verdict"] = "ok" if rc == 0 and not summary["mismatches"] else "fail"

    summary_path = args.out / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    report_md = args.out / "report.md"
    lines = [
        "# controller-readback validate",
        "",
        f"- verdict: {summary['verdict']}",
        f"- orchestrator rc: {rc}",
        f"- report found: {summary['report_found']}",
        f"- artifact: `{report}`",
        "",
    ]
    if summary.get("report"):
        lines.append("## Observed")
        lines.append("")
        for key, value in sorted(summary["report"].items()):
            lines.append(f"- {key}: `{value}`")
        lines.append("")
    if summary["mismatches"]:
        lines.append("## Mismatches")
        lines.append("")
        lines.extend(f"- {m}" for m in summary["mismatches"])
    report_md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {summary_path}")
    print(f"wrote {report_md}")
    return 0 if summary["verdict"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
