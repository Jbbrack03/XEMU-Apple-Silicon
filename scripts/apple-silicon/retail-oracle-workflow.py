#!/usr/bin/env python3
"""Run the real-Xbox retail gameplay oracle workflow end to end.

The workflow composes the production pieces into one command:

1. prove the OGX360 hardware input path with controller-readback,
2. bootstrap/prove controller IGR dashboard return with a short retail launch,
3. boot the requested retail game,
4. replay the xemu-recorded controller automation through OGX360,
5. capture composite video/audio, keyframes, and runtime JSON, and
6. require the Xbox dashboard/FTP to return.
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

TITLE_DEFAULTS = {
    "crimson": {
        "name": "Crimson Skies",
        "game_xbe": r"F:\Games\Crimson Skies\default.xbe",
        "input_csv": "scripts/apple-silicon/input-scripts/crimson-gameplay.csv",
        "route_offset_ms": 28000,
    },
    "rainbow": {
        "name": "Rainbow Six 3",
        "game_xbe": r"F:\Games\Rainbow Six 3\default.xbe",
        "input_csv": "scripts/apple-silicon/input-scripts/rainbow-gameplay.csv",
        "route_offset_ms": 0,
    },
    "pgr2": {
        "name": "Project Gotham Racing 2",
        "game_xbe": r"F:\Games\PGR2\default.xbe",
        "input_csv": "scripts/apple-silicon/input-scripts/pgr2-gameplay.csv",
        "route_offset_ms": 0,
    },
    "sc2": {
        "name": "Soul Calibur 2",
        "game_xbe": r"F:\Games\Soul Calibur 2\Default.xbe",
        "input_csv": "scripts/apple-silicon/input-scripts/sc2-gameplay.csv",
        "route_offset_ms": 0,
    },
}


def run(argv: list[str], log_path: Path, timeout: float | None = None) -> dict[str, Any]:
    started = time.time()
    try:
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
    except Exception as exc:
        result = {"cmd": argv, "rc": None, "error": str(exc)}
    log_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    return result


def evidence_ok(path: Path | None) -> bool:
    if not path or not path.exists():
        return False
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return False
    return data.get("status") == "ok" or data.get("verdict") == "ok"


def json_stdout(result: dict[str, Any]) -> dict[str, Any]:
    stdout = result.get("stdout")
    if not isinstance(stdout, str) or not stdout.strip():
        return {}
    try:
        data = json.loads(stdout)
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def parse_route(path: Path) -> dict[str, Any]:
    events: list[tuple[int, str, int]] = []
    controls: set[str] = set()
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line_no, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            row = next(csv.reader([line.replace("\t", ",")]))
            if len(row) != 3:
                row = line.split()
            if len(row) != 3:
                raise ValueError(f"{path}:{line_no}: expected time,control,value")
            t, control, value = int(row[0]), row[1].strip().lower(), int(row[2])
            events.append((t, control, value))
            controls.add(control)
    events.sort(key=lambda e: e[0])
    return {
        "path": str(path),
        "events": len(events),
        "first_ms": events[0][0] if events else None,
        "last_ms": events[-1][0] if events else None,
        "duration_s": round((events[-1][0] - events[0][0]) / 1000.0, 3)
        if events else None,
        "controls": sorted(controls),
    }


def xbox_path_exists(host: str, xbox_path: str) -> dict[str, Any]:
    normalized = xbox_path.replace("\\", "/")
    if ":" not in normalized:
        return {"exists": False, "error": "not an absolute Xbox path"}
    drive, rest = normalized.split(":", 1)
    parts = [p for p in rest.split("/") if p]
    if not parts:
        return {"exists": False, "error": "missing filename"}
    ftp_dir = "/" + drive.upper()
    if len(parts) > 1:
        ftp_dir += "/" + "/".join(parts[:-1])
    filename = parts[-1]
    try:
        ftp = ftplib.FTP(host, timeout=8)
        ftp.login(os.environ.get("ORACLE_FTP_USER", "xbox"),
                  os.environ.get("ORACLE_FTP_PASS", "xbox"))
        ftp.cwd(ftp_dir)
        size = ftp.size(filename)
        ftp.quit()
        return {"exists": True, "ftp_dir": ftp_dir, "filename": filename, "size": size}
    except Exception as exc:
        return {"exists": False, "ftp_dir": ftp_dir, "filename": filename,
                "error": str(exc)}


def write_igr_probe_route(path: Path, exit_after_ms: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = [
        (0, "a", 0),
        (exit_after_ms, "a", 0),
    ]
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerows(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--title", choices=sorted(TITLE_DEFAULTS), default="crimson")
    parser.add_argument("--game-xbe", help="Override retail XBE path.")
    parser.add_argument("--input-csv", help="Override xemu-recorded route CSV.")
    parser.add_argument("--route-offset-ms", type=int,
                        help="Override title default delay applied to route events.")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--hardware-device", default="/dev/cu.usbmodem3101")
    parser.add_argument("--hardware-port-index", type=int, default=1)
    parser.add_argument("--capture-device", default="USB2")
    parser.add_argument("--capture-audio-device", default="USB2")
    parser.add_argument("--capture-backend", choices=("xemu-capture", "ffmpeg"),
                        default=os.environ.get("RETAIL_CAPTURE_BACKEND", "xemu-capture"))
    parser.add_argument("--frame-interval-s", type=float, default=2.0)
    parser.add_argument("--no-audio", action="store_true")
    parser.add_argument("--skip-capture-preflight", action="store_true")
    parser.add_argument("--allow-broken-capture", action="store_true",
                        help="Continue even if the preflight snapshot cannot "
                             "read frames from the capture stick.")
    parser.add_argument("--skip-bridge-proof", action="store_true")
    parser.add_argument("--bridge-evidence", type=Path)
    parser.add_argument("--exit-evidence", type=Path)
    parser.add_argument("--skip-igr-proof", action="store_true")
    parser.add_argument("--igr-proof-delay-ms", type=int, default=45000)
    parser.add_argument("--gameplay-record-extra-s", type=float, default=15.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    defaults = TITLE_DEFAULTS[args.title]
    game_xbe = args.game_xbe or defaults["game_xbe"]
    input_csv = Path(args.input_csv or defaults["input_csv"]).resolve()
    route_offset_ms = (args.route_offset_ms if args.route_offset_ms is not None
                       else int(defaults.get("route_offset_ms", 0)))
    out_dir = args.out or ROOT / "benchmark-runs" / (
        "retail-oracle-workflow-" + args.title + "-" +
        time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    report: dict[str, Any] = {
        "schema": "retail-oracle-workflow-v1",
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": args.host,
        "title": args.title,
        "title_name": defaults["name"],
        "game_xbe": game_xbe,
        "input_csv": str(input_csv),
        "out_dir": str(out_dir),
        "hardware_device": args.hardware_device,
        "hardware_port_index": args.hardware_port_index,
        "route_offset_ms": route_offset_ms,
        "steps": [],
        "artifacts": {},
    }

    def step(name: str, status: str, **extra: Any) -> None:
        item = {"name": name, "status": status}
        item.update(extra)
        report["steps"].append(item)

    def write_report(status: str, rc: int, **extra: Any) -> int:
        report.update(extra)
        report["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        report["status"] = status
        (out_dir / "workflow.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(json.dumps({"status": status, "out_dir": str(out_dir)}, indent=2))
        return rc

    try:
        route_summary = parse_route(input_csv)
        report["route"] = route_summary
        step("input-route", "ok", **route_summary)
    except Exception as exc:
        step("input-route", "fail", error=str(exc))
        return write_report("blocked", 1, blocked_reasons=["input route invalid"])

    status = run([sys.executable, str(HERE / "oracle-orchestrator.py"),
                  "--host", args.host, "status"],
                 out_dir / "status.json", timeout=20.0)
    step("xbox-status", "ok" if status.get("rc") == 0 else "fail",
         rc=status.get("rc"))
    if status.get("rc") != 0:
        return write_report("blocked", 1, blocked_reasons=["Xbox/oracle status failed"])

    xbe_probe = xbox_path_exists(args.host, game_xbe)
    report["game_xbe_probe"] = xbe_probe
    step("game-xbe", "ok" if xbe_probe.get("exists") else "fail", **xbe_probe)
    if not xbe_probe.get("exists"):
        return write_report("blocked", 1, blocked_reasons=["game XBE not found on Xbox"])

    capture_tool = HERE / "xemu-capture-app.py"
    if not args.skip_capture_preflight and capture_tool.exists():
        auth = run([sys.executable, str(capture_tool), "auth"],
                   out_dir / "capture-auth.json", timeout=25.0)
        auth_json = json_stdout(auth)
        authorized = auth_json.get("camera_authorization") == "authorized"
        step("capture-auth", "ok" if authorized else "fail",
             rc=auth.get("rc"),
             camera_authorization=auth_json.get("camera_authorization"))

        if not authorized:
            requested = run([sys.executable, str(capture_tool), "auth", "--request"],
                            out_dir / "capture-auth-request.json", timeout=90.0)
            requested_json = json_stdout(requested)
            authorized = requested_json.get("camera_authorization") == "authorized"
            step("capture-auth-request", "ok" if authorized else "fail",
                 rc=requested.get("rc"),
                 camera_authorization=requested_json.get("camera_authorization"))
            if not authorized and not args.allow_broken_capture:
                return write_report(
                    "blocked", 1,
                    blocked_reasons=[
                        "xemu-capture app is not approved for macOS Camera access"
                    ],
                )

        capture_png = out_dir / "capture-preflight.png"
        capture = run([str(capture_tool), "snapshot", args.capture_device,
                       "--out", str(capture_png.resolve()), "--timeout", "8"],
                      out_dir / "capture-preflight.json", timeout=35.0)
        ok = capture.get("rc") == 0 and capture_png.exists()
        step("capture-preflight", "ok" if ok else "fail", rc=capture.get("rc"),
             screenshot=str(capture_png))
        if ok:
            report["artifacts"]["capture_preflight_png"] = str(capture_png)
        elif not args.allow_broken_capture:
            return write_report("blocked", 1,
                                blocked_reasons=["capture device did not deliver frames"])

    bridge_evidence = args.bridge_evidence
    if not args.skip_bridge_proof and not evidence_ok(bridge_evidence):
        bridge_dir = out_dir / "bridge-readback"
        bridge_cmd = [
            sys.executable,
            str(HERE / "ogx360-bridge/validation/bridge-readback-test.py"),
            "--host", args.host,
            "--device", args.hardware_device,
            "--out-dir", str(bridge_dir),
        ]
        bridge = run(bridge_cmd, out_dir / "bridge-readback.json", timeout=140.0)
        bridge_evidence = bridge_dir / "verdict.json"
        step("bridge-readback", "ok" if bridge.get("rc") == 0 else "fail",
             rc=bridge.get("rc"), evidence=str(bridge_evidence))
        report["artifacts"]["bridge_evidence"] = str(bridge_evidence)
        if bridge.get("rc") != 0 or not evidence_ok(bridge_evidence):
            return write_report("blocked", 1,
                                blocked_reasons=["OGX360 bridge readback proof failed"])
    elif bridge_evidence and evidence_ok(bridge_evidence):
        step("bridge-readback", "ok", evidence=str(bridge_evidence), reused=True)
    elif args.skip_bridge_proof:
        step("bridge-readback", "fail", evidence=str(bridge_evidence),
             reused=True, error="missing or non-ok bridge evidence")
        return write_report("blocked", 1,
                            blocked_reasons=["bridge proof skipped without ok evidence"])

    exit_evidence = args.exit_evidence
    if not args.skip_igr_proof and not evidence_ok(exit_evidence):
        igr_route = out_dir / "igr-proof-route.csv"
        write_igr_probe_route(igr_route, args.igr_proof_delay_ms)
        igr_dir = out_dir / "igr-proof"
        igr_cmd = [
            sys.executable, str(HERE / "retail-gameplay-oracle.py"),
            "--host", args.host,
            "--game-xbe", game_xbe,
            "--input-csv", str(igr_route),
            "--out", str(igr_dir),
            "--input-backend", "hardware",
            "--input-evidence", str(bridge_evidence),
            "--exit-backend", "controller-igr",
            "--launch-backend", "dashboard-ftp",
            "--hardware-device", args.hardware_device,
            "--hardware-port-index", str(args.hardware_port_index),
            "--launch-delay-s", "0",
            "--record-extra-s", "8",
            "--capture-backend", args.capture_backend,
            "--frame-interval-s", "5",
            "--route-offset-ms", "0",
            "--exit-delay-ms", "5000",
            "--exit-hold-ms", "6000",
            "--exit-attempts", "2",
            "--allow-capture-failure",
            "--allow-unproven",
        ]
        if args.no_audio:
            igr_cmd.append("--no-audio")
        if args.dry_run:
            igr_cmd.append("--dry-run")
        igr_timeout = max(240.0, args.igr_proof_delay_ms / 1000.0 + 180.0)
        igr = run(igr_cmd, out_dir / "igr-proof.json", timeout=igr_timeout)
        exit_evidence = igr_dir / "verdict.json"
        step("igr-proof", "ok" if igr.get("rc") == 0 and evidence_ok(exit_evidence) else "fail",
             rc=igr.get("rc"), evidence=str(exit_evidence))
        report["artifacts"]["exit_evidence"] = str(exit_evidence)
        if args.dry_run:
            return write_report("dry-run", 0)
        if igr.get("rc") != 0 or not evidence_ok(exit_evidence):
            return write_report("blocked", 1,
                                blocked_reasons=["controller IGR dashboard-return proof failed"])
    elif exit_evidence and evidence_ok(exit_evidence):
        step("igr-proof", "ok", evidence=str(exit_evidence), reused=True)
    elif args.skip_igr_proof:
        step("igr-proof", "fail", evidence=str(exit_evidence),
             reused=True, error="missing or non-ok exit evidence")
        return write_report("blocked", 1,
                            blocked_reasons=["IGR proof skipped without ok evidence"])

    gameplay_dir = out_dir / "gameplay"
    gameplay_cmd = [
        sys.executable, str(HERE / "retail-gameplay-oracle.py"),
        "--host", args.host,
        "--game-xbe", game_xbe,
        "--input-csv", str(input_csv),
        "--out", str(gameplay_dir),
        "--input-backend", "hardware",
        "--input-evidence", str(bridge_evidence),
        "--exit-backend", "controller-igr",
        "--exit-evidence", str(exit_evidence),
        "--launch-backend", "dashboard-ftp",
        "--hardware-device", args.hardware_device,
        "--hardware-port-index", str(args.hardware_port_index),
        "--launch-delay-s", "0",
        "--capture-device", args.capture_device,
        "--capture-audio-device", args.capture_audio_device,
        "--capture-backend", args.capture_backend,
        "--frame-interval-s", str(args.frame_interval_s),
        "--route-offset-ms", str(route_offset_ms),
        "--record-extra-s", str(args.gameplay_record_extra_s),
        "--exit-delay-ms", "5000",
        "--exit-hold-ms", "6000",
        "--exit-attempts", "2",
    ]
    if args.no_audio:
        gameplay_cmd.append("--no-audio")
    if args.dry_run:
        gameplay_cmd.append("--dry-run")
    timeout = max(600.0, float(route_summary["last_ms"] or 0) / 1000.0 +
                  args.gameplay_record_extra_s + 480.0)
    gameplay = run(gameplay_cmd, out_dir / "gameplay.json", timeout=timeout)
    gameplay_verdict = gameplay_dir / "verdict.json"
    report["artifacts"]["gameplay_verdict"] = str(gameplay_verdict)
    step("gameplay-capture", "ok" if gameplay.get("rc") == 0 and evidence_ok(gameplay_verdict)
         else "fail", rc=gameplay.get("rc"), verdict=str(gameplay_verdict))

    if gameplay.get("rc") != 0 or not evidence_ok(gameplay_verdict):
        return write_report("fail", 1, failed_reasons=["gameplay oracle run failed"])
    return write_report("ok", 0)


if __name__ == "__main__":
    raise SystemExit(main())
