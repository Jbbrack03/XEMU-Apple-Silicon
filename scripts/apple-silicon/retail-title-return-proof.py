#!/usr/bin/env python3
"""Upload and launch a return-only patched XBE on the real Xbox.

The output JSON is suitable as autonomous-exit evidence for the guarded retail
oracle tools when it reports status=ok.
"""

from __future__ import annotations

import argparse
import ftplib
import hashlib
import importlib.util as _imp
import json
import os
from pathlib import Path
import sys
import time
from typing import Any


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_HOST = os.environ.get("ORACLE_HOST", "192.168.0.200")
DEFAULT_FTP_USER = os.environ.get("ORACLE_FTP_USER", "xbox")
DEFAULT_FTP_PASS = os.environ.get("ORACLE_FTP_PASS", "xbox")

_spec = _imp.spec_from_file_location("oracle_orchestrator", HERE / "oracle-orchestrator.py")
orch = _imp.module_from_spec(_spec)  # type: ignore[arg-type]
assert _spec and _spec.loader
_spec.loader.exec_module(orch)  # type: ignore[union-attr]

_spec2 = _imp.spec_from_file_location("oracle_client", HERE / "oracle-client.py")
oc = _imp.module_from_spec(_spec2)  # type: ignore[arg-type]
assert _spec2 and _spec2.loader
_spec2.loader.exec_module(oc)  # type: ignore[union-attr]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def xbox_to_ftp_dir(xbox_path: str) -> tuple[str, str]:
    normalized = xbox_path.replace("\\", "/")
    if ":" not in normalized:
        raise ValueError(f"not an Xbox absolute path: {xbox_path}")
    drive, rest = normalized.split(":", 1)
    parts = [p for p in rest.split("/") if p]
    if not parts:
        raise ValueError(f"path has no filename: {xbox_path}")
    filename = parts[-1]
    ftp_dir = "/" + drive.upper()
    if len(parts) > 1:
        ftp_dir += "/" + "/".join(parts[:-1])
    return ftp_dir, filename


def ensure_ftp_dirs(ftp: ftplib.FTP, ftp_dir: str) -> None:
    parts = [p for p in ftp_dir.split("/") if p]
    cur = ""
    for part in parts:
        cur += "/" + part
        try:
            ftp.cwd(cur)
        except ftplib.error_perm:
            ftp.mkd(cur)
            ftp.cwd(cur)


def upload_file(host: str, local: Path, xbox_path: str) -> dict[str, Any]:
    ftp_dir, filename = xbox_to_ftp_dir(xbox_path)
    ftp = ftplib.FTP(host, timeout=20)
    ftp.login(DEFAULT_FTP_USER, DEFAULT_FTP_PASS)
    try:
        ensure_ftp_dirs(ftp, ftp_dir)
        with local.open("rb") as f:
            resp = ftp.storbinary(f"STOR {filename}", f)
        size = ftp.size(filename)
        return {"ftp_dir": ftp_dir, "filename": filename, "response": resp, "size": size}
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("patched_xbe", type=Path)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--remote-xbe", required=True,
                        help=r"Xbox path, e.g. E:\Apps\oracle-patches\pgr2-return\default.xbe")
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--wait-retries", type=int, default=90)
    parser.add_argument("--wait-delay-s", type=float, default=2.0)
    parser.add_argument("--no-post-screenshot", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    patched = args.patched_xbe.resolve()
    out_dir = args.out or ROOT / "benchmark-runs" / (
        "retail-return-proof-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    report: dict[str, Any] = {
        "schema": "retail-title-return-proof-v1",
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": args.host,
        "patched_xbe": str(patched),
        "patched_xbe_sha256": sha256_file(patched),
        "remote_xbe": args.remote_xbe,
        "out_dir": str(out_dir),
    }

    if args.dry_run:
        report["status"] = "dry-run"
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        print(json.dumps({"status": "dry-run", "out_dir": str(out_dir)}, indent=2))
        return 0

    try:
        report["upload"] = upload_file(args.host, patched, args.remote_xbe)
    except Exception as exc:
        report["status"] = "upload-failed"
        report["error"] = str(exc)
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        return 1

    if not orch.ensure_agent(host=args.host):
        report["status"] = "agent-launch-failed"
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        return 1

    chainload_acked = False
    try:
        client = oc.OracleClient(args.host, timeout=30.0)
        client.connect()
        try:
            report["runxbe_ack"] = client.runxbe(args.remote_xbe)
            chainload_acked = True
        finally:
            client.close()
    except Exception as exc:
        report["status"] = "runxbe-failed"
        report["error"] = str(exc)
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        return 1

    report["chainload_acked"] = chainload_acked
    report["chainload_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    returned = orch.wait_for_ftp(args.host, retries=args.wait_retries, delay=args.wait_delay_s)
    report["dashboard_ftp_returned"] = returned
    if not returned:
        report["status"] = "no-dashboard-return"
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        return 1

    if not args.no_post_screenshot:
        post_png = out_dir / "post-dashboard.png"
        if orch.capture_screenshot(args.host, post_png):
            report["post_dashboard_png"] = str(post_png)

    report["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    report["status"] = "ok"
    (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                          encoding="utf-8")
    print(json.dumps({"status": "ok", "out_dir": str(out_dir)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
