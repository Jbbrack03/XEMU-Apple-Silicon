#!/usr/bin/env python3
"""Recursively upload a local file or directory to the OG Xbox over FTP."""

from __future__ import annotations

import argparse
import ftplib
import json
import os
import time
from pathlib import Path
from typing import Any


DEFAULT_HOST = os.environ.get("ORACLE_HOST", "192.168.0.200")
DEFAULT_USER = os.environ.get("ORACLE_FTP_USER", "xbox")
DEFAULT_PASS = os.environ.get("ORACLE_FTP_PASS", "xbox")


def normalize_remote(path: str) -> str:
    path = path.replace("\\", "/")
    if not path.startswith("/"):
        path = "/" + path
    return path.rstrip("/") or "/"


def ftp_mkdir_p(ftp: ftplib.FTP, remote_dir: str) -> None:
    parts = [p for p in normalize_remote(remote_dir).split("/") if p]
    cur = ""
    for part in parts:
        cur += "/" + part
        try:
            ftp.cwd(cur)
        except ftplib.error_perm:
            ftp.mkd(cur)
            ftp.cwd(cur)


def remote_size(ftp: ftplib.FTP, remote_file: str) -> int | None:
    try:
        return ftp.size(remote_file)
    except ftplib.all_errors:
        return None


def upload_file(ftp: ftplib.FTP, local_file: Path, remote_file: str,
                overwrite: bool) -> dict[str, Any]:
    local_size = local_file.stat().st_size
    existing = remote_size(ftp, remote_file)
    if existing == local_size and not overwrite:
        return {
            "path": str(local_file),
            "remote": remote_file,
            "status": "skipped",
            "bytes": local_size,
            "reason": "same-size",
        }

    started = time.time()
    with local_file.open("rb") as f:
        response = ftp.storbinary(f"STOR {Path(remote_file).name}", f)
    elapsed = round(time.time() - started, 3)
    final_size = remote_size(ftp, remote_file)
    return {
        "path": str(local_file),
        "remote": remote_file,
        "status": "uploaded",
        "bytes": local_size,
        "elapsed_s": elapsed,
        "response": response,
        "remote_size": final_size,
    }


def walk_local(source: Path) -> list[Path]:
    if source.is_file():
        return [source]
    return sorted([p for p in source.rglob("*") if p.is_file()])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("remote")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--user", default=DEFAULT_USER)
    parser.add_argument("--password", default=DEFAULT_PASS)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)

    source = args.source.resolve()
    if not source.exists():
        raise SystemExit(f"source does not exist: {source}")

    remote = normalize_remote(args.remote)
    out_path = args.out
    if out_path is None:
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        out_path = Path.cwd() / "benchmark-runs" / f"xbox-ftp-upload-{stamp}.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    files = walk_local(source)
    base = source.parent if source.is_file() else source
    report: dict[str, Any] = {
        "schema": "xbox-ftp-upload-v1",
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": args.host,
        "source": str(source),
        "remote": remote,
        "overwrite": args.overwrite,
        "file_count": len(files),
        "results": [],
    }

    ftp = ftplib.FTP(args.host, timeout=30)
    ftp.login(args.user, args.password)
    try:
        if source.is_dir():
            ftp_mkdir_p(ftp, remote)
        else:
            ftp_mkdir_p(ftp, str(Path(remote).parent))

        uploaded = 0
        skipped = 0
        failed = 0
        for local_file in files:
            if source.is_file():
                remote_file = remote
            else:
                rel = local_file.relative_to(base).as_posix()
                remote_file = normalize_remote(remote + "/" + rel)
                ftp_mkdir_p(ftp, str(Path(remote_file).parent))

            result = upload_file(ftp, local_file, remote_file, args.overwrite)
            report["results"].append(result)
            if result["status"] == "uploaded":
                uploaded += 1
            elif result["status"] == "skipped":
                skipped += 1
            else:
                failed += 1
        report["uploaded"] = uploaded
        report["skipped"] = skipped
        report["failed"] = failed
        report["status"] = "ok" if failed == 0 else "fail"
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            pass

    report["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    out_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(json.dumps({
        "status": report["status"],
        "uploaded": report.get("uploaded", 0),
        "skipped": report.get("skipped", 0),
        "failed": report.get("failed", 0),
        "out": str(out_path),
    }, indent=2))
    return 0 if report["status"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
