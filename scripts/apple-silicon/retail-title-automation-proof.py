#!/usr/bin/env python3
"""Run a patched retail-title automation XBE with composite keyframe evidence."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util as _imp
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_HOST = os.environ.get("ORACLE_HOST", "192.168.0.200")

_spec = _imp.spec_from_file_location("oracle_orchestrator", HERE / "oracle-orchestrator.py")
orch = _imp.module_from_spec(_spec)  # type: ignore[arg-type]
assert _spec and _spec.loader
_spec.loader.exec_module(orch)  # type: ignore[union-attr]

_spec2 = _imp.spec_from_file_location("oracle_client", HERE / "oracle-client.py")
oc = _imp.module_from_spec(_spec2)  # type: ignore[arg-type]
assert _spec2 and _spec2.loader
_spec2.loader.exec_module(oc)  # type: ignore[union-attr]

_spec3 = _imp.spec_from_file_location("return_proof", HERE / "retail-title-return-proof.py")
return_proof = _imp.module_from_spec(_spec3)  # type: ignore[arg-type]
assert _spec3 and _spec3.loader
_spec3.loader.exec_module(return_proof)  # type: ignore[union-attr]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(argv: list[str], log: Path, timeout: float | None = None) -> dict[str, Any]:
    started = time.time()
    proc = subprocess.run(
        argv,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )
    result = {
        "cmd": argv,
        "rc": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "elapsed_s": round(time.time() - started, 3),
    }
    log.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def capture_device_visible(pattern: str) -> dict[str, Any]:
    ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
    proc = subprocess.run(
        [ffmpeg, "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=15,
        check=False,
    )
    text = (proc.stdout or "") + (proc.stderr or "")
    return {
        "rc": proc.returncode,
        "matched": pattern.lower() in text.lower(),
        "stdout": proc.stdout,
        "stderr": proc.stderr,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("patched_xbe", type=Path)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--remote-xbe", required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--record-s", type=float, default=45.0)
    parser.add_argument("--capture-device", default="USB2")
    parser.add_argument("--capture-audio-device", default="USB2")
    parser.add_argument("--no-audio", action="store_true")
    parser.add_argument("--keyframe-threshold", type=float, default=0.18)
    parser.add_argument("--keyframe-every-s", type=float, default=2.0)
    parser.add_argument("--max-keyframes", type=int, default=60)
    parser.add_argument("--wait-retries", type=int, default=90)
    parser.add_argument("--wait-delay-s", type=float, default=2.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    patched = args.patched_xbe.resolve()
    out_dir = args.out or ROOT / "benchmark-runs" / (
        "retail-automation-proof-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    report: dict[str, Any] = {
        "schema": "retail-title-automation-proof-v1",
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": args.host,
        "patched_xbe": str(patched),
        "patched_xbe_sha256": sha256_file(patched),
        "remote_xbe": args.remote_xbe,
        "out_dir": str(out_dir),
        "record_s": args.record_s,
        "artifacts": {},
    }

    meta_path = patched.with_suffix(patched.suffix + ".patch.json")
    if meta_path.exists():
        try:
            report["patch_meta"] = json.loads(meta_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            report["patch_meta_error"] = str(exc)

    cap_probe = capture_device_visible(args.capture_device)
    report["capture_probe"] = cap_probe
    if not cap_probe["matched"] and not args.dry_run:
        report["status"] = "capture-device-missing"
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        return 1

    if args.dry_run:
        report["status"] = "dry-run"
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        print(json.dumps({"status": "dry-run", "out_dir": str(out_dir)}, indent=2))
        return 0

    try:
        report["upload"] = return_proof.upload_file(args.host, patched, args.remote_xbe)
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

    comp_dir = out_dir / "composite"
    comp_cmd = [
        str(HERE / "composite-record.sh"),
        "--duration", str(args.record_s),
        "--out-dir", str(comp_dir),
        "--device", args.capture_device,
        "--audio-device", args.capture_audio_device,
        "--label", "retail-automation",
    ]
    if args.no_audio:
        comp_cmd.append("--no-audio")
    rec_log = (out_dir / "composite-record-wrapper.log").open("w", encoding="utf-8")
    rec = subprocess.Popen(comp_cmd, cwd=ROOT, stdout=rec_log,
                           stderr=subprocess.STDOUT, text=True)
    report["capture_cmd"] = comp_cmd
    time.sleep(2.0)

    try:
        with oc.OracleClient(args.host, timeout=30.0) as client:
            report["runxbe_ack"] = client.runxbe(args.remote_xbe)
    except Exception as exc:
        report["status"] = "runxbe-failed"
        report["error"] = str(exc)
        try:
            rec.terminate()
        except Exception:
            pass
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        return 1

    rec_rc = rec.wait()
    rec_log.close()
    report["capture_rc"] = rec_rc
    video = comp_dir / "video.mp4"
    if video.exists():
        report["artifacts"]["video"] = str(video)
        kf_dir = out_dir / "keyframes"
        kf = run([
            sys.executable, str(HERE / "extract-keyframes.py"), str(video),
            "--out-dir", str(kf_dir),
            "--threshold", str(args.keyframe_threshold),
            "--every-s", str(args.keyframe_every_s),
            "--max-keyframes", str(args.max_keyframes),
        ], out_dir / "extract-keyframes.json")
        report["keyframes_rc"] = kf["rc"]
        report["artifacts"]["keyframes"] = str(kf_dir)
        if not args.no_audio:
            audio_dir = out_dir / "audio"
            au = run([sys.executable, str(HERE / "audio-waveform.py"), str(video),
                      "--out-dir", str(audio_dir)],
                     out_dir / "audio-waveform.json")
            report["audio_rc"] = au["rc"]
            report["artifacts"]["audio"] = str(audio_dir)

    returned = orch.wait_for_ftp(args.host, retries=args.wait_retries, delay=args.wait_delay_s)
    report["dashboard_ftp_returned"] = returned
    if returned:
        post_png = out_dir / "post-dashboard.png"
        if orch.capture_screenshot(args.host, post_png):
            report["artifacts"]["post_dashboard_png"] = str(post_png)

    failed = []
    if rec_rc != 0:
        failed.append(f"composite capture rc={rec_rc}")
    if not video.exists():
        failed.append("video.mp4 missing")
    if not returned:
        failed.append("dashboard FTP did not return")

    report["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    report["status"] = "ok" if not failed else "fail"
    if failed:
        report["failed_reasons"] = failed
    (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                          encoding="utf-8")
    print(json.dumps({"status": report["status"], "out_dir": str(out_dir),
                      "failed_reasons": failed}, indent=2))
    return 0 if report["status"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
