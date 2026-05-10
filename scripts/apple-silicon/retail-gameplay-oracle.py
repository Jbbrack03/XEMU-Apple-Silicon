#!/usr/bin/env python3
"""Run a guarded real-Xbox retail gameplay oracle capture.

This is the production wrapper for the "retail game" leg of the oracle:

  1. prove the requested route and backends are safe,
  2. start composite A/V capture,
  3. launch the retail XBE through the oracle agent,
  4. drive the title-facing input backend with the route CSV,
  5. include the dashboard-return combo in the same route,
  6. wait for the dashboard to return, and
  7. extract keyframes/audio artifacts for visual review and diffing.

Important boundary: the oracle agent process dies when it `runxbe`s a retail
game. Live `controller.*` RPC replay is therefore NOT a retail-game input
backend. A valid run must supply either:

  - a hardware controller-emulator driver command, or
  - a proven resident Tier-2 hook preload/driver command.

By default this tool blocks instead of stranding the Xbox in-game. Use
`--allow-unproven` only while physically watching the console and ready to
power-cycle it.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_HOST = os.environ.get("ORACLE_HOST", "192.168.0.200")

import importlib.util as _imp

_spec = _imp.spec_from_file_location("oracle_orchestrator", HERE / "oracle-orchestrator.py")
orch = _imp.module_from_spec(_spec)  # type: ignore[arg-type]
assert _spec and _spec.loader
_spec.loader.exec_module(orch)  # type: ignore[union-attr]

_spec2 = _imp.spec_from_file_location("oracle_client", HERE / "oracle-client.py")
oc = _imp.module_from_spec(_spec2)  # type: ignore[arg-type]
assert _spec2 and _spec2.loader
_spec2.loader.exec_module(oc)  # type: ignore[union-attr]


BUTTONS = {
    "a", "b", "x", "y", "start", "back", "white", "black",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "lstick_btn", "rstick_btn", "guide",
}
AXES = {"ltrigger", "rtrigger", "lstick_x", "lstick_y", "rstick_x", "rstick_y"}
NEUTRAL_CONTROLS = [(name, 0) for name in sorted(BUTTONS | AXES)]


def run(argv: list[str], timeout: float | None = None,
        log: Path | None = None) -> dict[str, Any]:
    started = time.time()
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
        result = {
            "cmd": argv,
            "rc": p.returncode,
            "stdout": p.stdout,
            "stderr": p.stderr,
            "elapsed_s": round(time.time() - started, 3),
        }
    except Exception as exc:
        result = {"cmd": argv, "rc": None, "error": str(exc)}
    if log:
        log.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def evidence_ok(path: str | None) -> bool:
    if not path:
        return False
    p = Path(path)
    if not p.exists():
        return False
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return False
    return data.get("status") == "ok" or data.get("verdict") == "ok"


def parse_route(path: Path) -> tuple[list[tuple[int, str, int]], dict[str, Any]]:
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
            try:
                t = int(row[0])
                control = row[1].strip().lower()
                value = int(row[2])
            except ValueError as exc:
                raise ValueError(f"{path}:{line_no}: bad integer field") from exc
            if control not in BUTTONS and control not in AXES:
                raise ValueError(f"{path}:{line_no}: unknown control {control!r}")
            events.append((t, control, value))
            controls.add(control)
    events.sort(key=lambda e: e[0])
    first = events[0][0] if events else None
    last = events[-1][0] if events else None
    summary = {
        "path": str(path),
        "events": len(events),
        "first_ms": first,
        "last_ms": last,
        "duration_s": None if first is None or last is None else round((last - first) / 1000.0, 3),
        "controls": sorted(controls),
    }
    return events, summary


def write_route_with_exit(events: list[tuple[int, str, int]], out_csv: Path,
                          exit_mode: str, exit_delay_ms: int,
                          exit_hold_ms: int, exit_attempts: int = 1,
                          exit_repeat_gap_ms: int = 5000) -> dict[str, Any]:
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    combined = [(0, control, value) for control, value in NEUTRAL_CONTROLS]
    combined.extend(events)
    exit_info: dict[str, Any] = {"mode": exit_mode, "added": False}
    if exit_mode == "controller-igr":
        base = (combined[-1][0] if combined else 0) + exit_delay_ms
        attempts = max(1, exit_attempts)
        neutralize = max(0, base - 1000)
        # Softmod IGR combo on the project Xbox: Back + Start + LT + RT.
        combined.extend((neutralize, control, value)
                        for control, value in NEUTRAL_CONTROLS)
        attempt_info = []
        for i in range(attempts):
            press = base + i * (exit_hold_ms + exit_repeat_gap_ms)
            release = press + exit_hold_ms
            combined.extend([
                (press, "back", 1),
                (press, "start", 1),
                (press, "ltrigger", 32767),
                (press, "rtrigger", 32767),
                (release, "back", 0),
                (release, "start", 0),
                (release, "ltrigger", 0),
                (release, "rtrigger", 0),
            ])
            attempt_info.append({"press_ms": press, "release_ms": release})
        exit_info.update({"added": True, "neutralize_ms": neutralize,
                          "attempts": attempt_info})
    with out_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        for row in sorted(combined, key=lambda e: e[0]):
            w.writerow(row)
    exit_info["route_csv"] = str(out_csv)
    exit_info["events_total"] = len(combined)
    first_ms = combined[0][0] if combined else 0
    last_ms = combined[-1][0] if combined else 0
    exit_info["first_ms"] = first_ms
    exit_info["last_ms"] = last_ms
    exit_info["duration_s"] = round(max(0, last_ms - first_ms) / 1000.0, 3)
    return exit_info


def format_cmd(template: str, fields: dict[str, Any]) -> str:
    safe = {k: shlex.quote(str(v)) for k, v in fields.items()}
    raw = {f"{k}_raw": str(v) for k, v in fields.items()}
    return template.format(**safe, **raw)


def hardware_driver_template(device: str | None, port_index: int,
                             rate_multiplier: float) -> str:
    argv = [
        sys.executable,
        str(HERE / "ogx360-bridge/mac-side/controller-replay-hardware.py"),
        "{route_csv_raw}",
        "--port-index", str(port_index),
        "--rate-multiplier", str(rate_multiplier),
        "--time-origin", "zero",
        "--clear-on-start",
        "--neutral-on-exit",
    ]
    if device:
        argv.extend(["--device", device])
    # `route_csv_raw` is an intentional placeholder, so quote every
    # concrete token individually and leave that placeholder for format_cmd.
    return " ".join("{route_csv}" if arg == "{route_csv_raw}" else shlex.quote(arg)
                    for arg in argv)


def popen_shell(cmd: str, log_path: Path) -> subprocess.Popen:
    log = log_path.open("w", encoding="utf-8")
    log.write(f"$ {cmd}\n")
    log.flush()
    return subprocess.Popen(
        cmd,
        shell=True,
        cwd=ROOT,
        stdout=log,
        stderr=subprocess.STDOUT,
        text=True,
    )


def capture_device_visible(pattern: str) -> dict[str, Any]:
    capture_tool = HERE / "xemu-capture-app.py"
    if capture_tool.exists():
        result = run([sys.executable, str(capture_tool), "list"], timeout=15.0)
    else:
        ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
        result = run([ffmpeg, "-hide_banner", "-f", "avfoundation",
                      "-list_devices", "true", "-i", ""], timeout=15.0)
    text = (result.get("stdout") or "") + (result.get("stderr") or "")
    result["matched"] = pattern.lower() in text.lower()
    return result


def ensure_dashboard_ftp(host: str) -> tuple[bool, str]:
    if orch.wait_for_ftp(host, retries=3, delay=1.0):
        return True, "ftp already reachable"
    try:
        with oc.OracleClient(host, timeout=10.0) as client:
            ack = client.reboot()
        orch.wait_for_ftp(host, retries=60, delay=2.0)
        if orch.wait_for_ftp(host, retries=3, delay=1.0):
            return True, f"agent rebooted to dashboard: {ack}"
        return False, "agent reboot issued, but dashboard FTP did not return"
    except Exception as exc:
        return False, f"dashboard FTP unavailable and agent reboot failed: {exc}"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--game-xbe", required=True,
                   help=r"Retail game path, e.g. E:\Games\PGR2\default.xbe")
    p.add_argument("--input-csv", required=True)
    p.add_argument("--out", default=None)
    p.add_argument("--input-backend", choices=("tier2-hook", "hardware"), required=True)
    p.add_argument("--input-evidence",
                   help="JSON with status/verdict=ok proving this backend")
    p.add_argument("--exit-backend", choices=("controller-igr", "power-cycle-relay"),
                   default="controller-igr")
    p.add_argument("--exit-evidence",
                   help="JSON with status/verdict=ok proving autonomous dashboard return")
    p.add_argument("--prelaunch-cmd",
                   help="Optional shell command run before game launch. Placeholders: "
                        "{route_csv}, {game_xbe}, {host}, {out_dir}")
    p.add_argument("--launch-backend", choices=("dashboard-ftp", "agent-runxbe"),
                   default="dashboard-ftp",
                   help="Retail games should launch from dashboard FTP SITE EXEC. "
                        "agent-runxbe is kept for diagnostic XBE compatibility.")
    p.add_argument("--input-driver-cmd",
                   help="Shell command that drives the title-facing backend while "
                        "the game runs. Placeholders are shell-quoted by default: "
                        "{route_csv}, {game_xbe}, {host}, {out_dir}; use *_raw "
                        "variants only for trusted commands.")
    p.add_argument("--hardware-device",
                   help="Serial device for --input-backend hardware. Default: "
                        "controller-replay-hardware.py autodetects /dev/cu.usbmodem*.")
    p.add_argument("--hardware-port-index", type=int, default=1,
                   help="OGX360 I2C slave / Xbox controller port index for hardware replay.")
    p.add_argument("--hardware-rate-multiplier", type=float, default=1.0)
    p.add_argument("--launch-delay-s", type=float, default=3.0,
                   help="Delay after runxbe before starting input driver")
    p.add_argument("--route-offset-ms", type=int, default=0,
                   help="Add this many milliseconds to every recorded input "
                        "event before appending the exit route. Useful when "
                        "retail hardware reaches title/menu later than xemu.")
    p.add_argument("--capture-device", default="USB2")
    p.add_argument("--capture-audio-device", default="USB2")
    p.add_argument("--capture-backend", choices=("xemu-capture", "ffmpeg"),
                   default=os.environ.get("RETAIL_CAPTURE_BACKEND", "xemu-capture"),
                   help="xemu-capture records approved PNG reference frames; "
                        "ffmpeg records MP4/AAC but requires its own macOS "
                        "Camera approval.")
    p.add_argument("--frame-interval-s", type=float, default=2.0,
                   help="PNG frame interval for --capture-backend xemu-capture.")
    p.add_argument("--no-audio", action="store_true")
    p.add_argument("--record-extra-s", type=float, default=15.0,
                   help="Seconds after route end to keep recording")
    p.add_argument("--capture-timeout-extra-s", type=float, default=20.0,
                   help="Kill composite capture if it runs this many seconds "
                        "past the requested duration.")
    p.add_argument("--allow-capture-failure", action="store_true",
                   help="Do not fail the run solely because composite capture "
                        "failed or produced no video. Useful only for "
                        "dashboard-return bootstrapping.")
    p.add_argument("--exit-delay-ms", type=int, default=5000)
    p.add_argument("--exit-hold-ms", type=int, default=6000)
    p.add_argument("--exit-attempts", type=int, default=2)
    p.add_argument("--exit-repeat-gap-ms", type=int, default=6000)
    p.add_argument("--keyframe-threshold", type=float, default=0.20)
    p.add_argument("--keyframe-every-s", type=float, default=2.0)
    p.add_argument("--max-keyframes", type=int, default=60)
    p.add_argument("--post-dashboard-screenshot", action="store_true",
                   help="Capture an oracle-agent screenshot after dashboard "
                        "return. This relaunches the agent and leaves FTP down, "
                        "so workflow callers should usually leave it off.")
    p.add_argument("--allow-unproven", action="store_true",
                   help="Run despite missing backend evidence. Dangerous.")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args(argv)

    if args.input_backend == "hardware" and not args.input_driver_cmd:
        args.input_driver_cmd = hardware_driver_template(
            args.hardware_device,
            args.hardware_port_index,
            args.hardware_rate_multiplier,
        )

    out_dir = Path(args.out) if args.out else ROOT / "benchmark-runs" / (
        "retail-gameplay-oracle-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    report: dict[str, Any] = {
        "schema": "retail-gameplay-oracle-v1",
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": args.host,
        "game_xbe": args.game_xbe,
        "out_dir": str(out_dir),
        "input_backend": args.input_backend,
        "exit_backend": args.exit_backend,
        "hardware": {
            "device": args.hardware_device,
            "port_index": args.hardware_port_index,
            "rate_multiplier": args.hardware_rate_multiplier,
        } if args.input_backend == "hardware" else None,
        "gates": [],
        "artifacts": {},
    }
    failures: list[str] = []

    def gate(name: str, ok: bool, message: str) -> None:
        report["gates"].append({"name": name, "status": "pass" if ok else "fail",
                                "message": message})
        if not ok:
            failures.append(f"{name}: {message}")

    try:
        events, csv_summary = parse_route(Path(args.input_csv).resolve())
        if args.route_offset_ms:
            events = [(t + args.route_offset_ms, control, value)
                      for t, control, value in events]
            csv_summary["route_offset_ms"] = args.route_offset_ms
            if csv_summary.get("first_ms") is not None:
                csv_summary["first_ms"] += args.route_offset_ms
            if csv_summary.get("last_ms") is not None:
                csv_summary["last_ms"] += args.route_offset_ms
        report["input_csv"] = csv_summary
        gate("input-csv", bool(events), f"{csv_summary['events']} events")
    except Exception as exc:
        report["input_csv"] = {"path": args.input_csv, "error": str(exc)}
        gate("input-csv", False, str(exc))
        events = []

    input_ok = evidence_ok(args.input_evidence)
    exit_ok = evidence_ok(args.exit_evidence)
    gate("title-facing-input-evidence", input_ok or args.allow_unproven,
         f"backend={args.input_backend} evidence={args.input_evidence or '<none>'}")
    gate("autonomous-exit-evidence", exit_ok or args.allow_unproven,
         f"backend={args.exit_backend} evidence={args.exit_evidence or '<none>'}")
    gate("driver-command", bool(args.prelaunch_cmd or args.input_driver_cmd) or args.allow_unproven,
         "prelaunch/input driver command supplied"
         if (args.prelaunch_cmd or args.input_driver_cmd)
         else "no command can feed the backend during the retail run")

    status = run([sys.executable, str(HERE / "oracle-orchestrator.py"),
                  "--host", args.host, "status"], timeout=20.0,
                 log=out_dir / "preflight-status.json")
    report["oracle_status"] = status
    gate("xbox-reachable", status.get("rc") == 0,
         "oracle-orchestrator status rc=0" if status.get("rc") == 0
         else "Xbox/oracle status is not green")

    cap_probe = capture_device_visible(args.capture_device)
    report["capture_probe"] = cap_probe
    gate("composite-capture-device", bool(cap_probe.get("matched")) or args.allow_unproven,
         f"AVFoundation device pattern {args.capture_device!r}")

    route_csv = out_dir / "route-with-exit.csv"
    exit_info = write_route_with_exit(events, route_csv, args.exit_backend,
                                      args.exit_delay_ms, args.exit_hold_ms,
                                      args.exit_attempts,
                                      args.exit_repeat_gap_ms)
    report["exit_route"] = exit_info
    route_duration_s = float(exit_info.get("duration_s", 0.0)) if events else 0.0
    record_duration_s = max(5.0, args.launch_delay_s + route_duration_s + args.record_extra_s)
    report["record_duration_s"] = round(record_duration_s, 3)

    if failures and not args.allow_unproven:
        report["verdict"] = "blocked"
        report["blocked_reasons"] = failures
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        print(json.dumps({"verdict": "blocked", "out_dir": str(out_dir),
                          "blocked_reasons": failures}, indent=2))
        return 1

    if args.dry_run:
        report["verdict"] = "dry-run"
        (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                              encoding="utf-8")
        print(json.dumps({"verdict": "dry-run", "out_dir": str(out_dir)}, indent=2))
        return 0

    fields = {
        "route_csv": route_csv,
        "game_xbe": args.game_xbe,
        "host": args.host,
        "out_dir": out_dir,
    }
    if args.prelaunch_cmd:
        cmd = format_cmd(args.prelaunch_cmd, fields)
        report["prelaunch_cmd"] = cmd
        pre = run(["/bin/bash", "-lc", cmd], timeout=None, log=out_dir / "prelaunch.json")
        gate("prelaunch-cmd", pre.get("rc") == 0, f"rc={pre.get('rc')}")
        if pre.get("rc") != 0 and not args.allow_unproven:
            report["verdict"] = "fail"
            report["blocked_reasons"] = ["prelaunch command failed"]
            (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                                  encoding="utf-8")
            return 1

    comp_dir = out_dir / "composite"
    if args.capture_backend == "ffmpeg":
        comp_cmd = [
            str(HERE / "composite-record.sh"),
            "--duration", str(record_duration_s),
            "--out-dir", str(comp_dir),
            "--device", args.capture_device,
            "--audio-device", args.capture_audio_device,
            "--label", "retail-gameplay",
        ]
        if args.no_audio:
            comp_cmd.append("--no-audio")
    else:
        comp_cmd = [
            sys.executable, str(HERE / "capture-frame-sequence.py"),
            args.capture_device,
            "--duration", str(record_duration_s),
            "--interval", str(args.frame_interval_s),
            "--out-dir", str(comp_dir),
            "--width", "720",
            "--height", "480",
            "--timeout", "8",
            "--label", "retail-gameplay",
        ]
    rec_log = (out_dir / "composite-record-wrapper.log").open("w", encoding="utf-8")
    rec = subprocess.Popen(comp_cmd, cwd=ROOT, stdout=rec_log,
                           stderr=subprocess.STDOUT, text=True)
    report["capture_cmd"] = comp_cmd
    report["capture_backend"] = args.capture_backend
    time.sleep(2.0)

    if args.launch_backend == "agent-runxbe":
        if not orch.ensure_agent(host=args.host):
            report["verdict"] = "fail"
            report["blocked_reasons"] = ["failed to launch oracle agent"]
            (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                                  encoding="utf-8")
            return 1
        try:
            with oc.OracleClient(args.host, timeout=30.0) as client:
                report["runxbe_ack"] = client.runxbe(args.game_xbe)
        except Exception as exc:
            report["verdict"] = "fail"
            report["error"] = f"agent runxbe failed: {exc}"
            try:
                rec.terminate()
            except Exception:
                pass
            (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                                  encoding="utf-8")
            return 1
    else:
        dashboard_ready, dashboard_msg = ensure_dashboard_ftp(args.host)
        report["dashboard_launch_preflight"] = {
            "ok": dashboard_ready,
            "message": dashboard_msg,
        }
        if not dashboard_ready:
            report["verdict"] = "fail"
            report["error"] = dashboard_msg
            try:
                rec.terminate()
            except Exception:
                pass
            (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                                  encoding="utf-8")
            return 1
        try:
            orch.site_run_xbe(args.host, args.game_xbe)
            report["runxbe_ack"] = f"dashboard FTP launch issued for {args.game_xbe}"
        except Exception as exc:
            report["verdict"] = "fail"
            report["error"] = f"dashboard FTP launch failed: {exc}"
            try:
                rec.terminate()
            except Exception:
                pass
            (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                                  encoding="utf-8")
            return 1

    if args.input_driver_cmd:
        time.sleep(max(args.launch_delay_s, 0.0))
        cmd = format_cmd(args.input_driver_cmd, fields)
        report["input_driver_cmd"] = cmd
        driver = popen_shell(cmd, out_dir / "input-driver.log")
        driver_rc = driver.wait()
        report["input_driver_rc"] = driver_rc
    else:
        report["input_driver_rc"] = None
        time.sleep(max(record_duration_s - 2.0, 0.0))

    capture_timed_out = False
    try:
        rec_rc = rec.wait(timeout=max(1.0, record_duration_s + args.capture_timeout_extra_s))
    except subprocess.TimeoutExpired:
        capture_timed_out = True
        try:
            rec.kill()
        except Exception:
            pass
        rec_rc = rec.wait()
    rec_log.close()
    report["capture_rc"] = rec_rc
    report["capture_timed_out"] = capture_timed_out
    video = comp_dir / "video.mp4"
    frame_meta = comp_dir / "capture-meta.json"
    frame_paths = sorted(comp_dir.glob("frame-*.png"))
    if frame_meta.exists():
        report["artifacts"]["capture_meta"] = str(frame_meta)
    if frame_paths:
        report["artifacts"]["reference_frames"] = str(comp_dir)
        report["reference_frame_count"] = len(frame_paths)
    if args.capture_backend == "ffmpeg" and video.exists():
        report["artifacts"]["video"] = str(video)
        kf_dir = out_dir / "keyframes"
        kf = run([
            sys.executable, str(HERE / "extract-keyframes.py"), str(video),
            "--out-dir", str(kf_dir),
            "--threshold", str(args.keyframe_threshold),
            "--every-s", str(args.keyframe_every_s),
            "--max-keyframes", str(args.max_keyframes),
        ], timeout=None, log=out_dir / "extract-keyframes.json")
        report["keyframes_rc"] = kf.get("rc")
        report["artifacts"]["keyframes"] = str(kf_dir)
        if not args.no_audio:
            audio_dir = out_dir / "audio"
            au = run([sys.executable, str(HERE / "audio-waveform.py"), str(video),
                      "--out-dir", str(audio_dir)],
                     timeout=None, log=out_dir / "audio-waveform.json")
            report["audio_rc"] = au.get("rc")
            report["artifacts"]["audio"] = str(audio_dir)

    returned = orch.wait_for_ftp(args.host, retries=60, delay=2.0)
    if (not returned and args.exit_backend == "controller-igr"
            and args.input_backend == "hardware" and args.input_driver_cmd):
        recovery_route = out_dir / "dashboard-recovery-route.csv"
        recovery_info = write_route_with_exit([], recovery_route, "controller-igr",
                                              1000, max(args.exit_hold_ms, 8000),
                                              max(args.exit_attempts, 2),
                                              args.exit_repeat_gap_ms)
        recovery_fields = dict(fields)
        recovery_fields["route_csv"] = recovery_route
        recovery_cmd = format_cmd(args.input_driver_cmd, recovery_fields)
        report["dashboard_recovery"] = {
            "route": recovery_info,
            "cmd": recovery_cmd,
        }
        recovery = run(["/bin/bash", "-lc", recovery_cmd],
                       timeout=float(recovery_info["duration_s"]) + 30.0,
                       log=out_dir / "dashboard-recovery.json")
        report["dashboard_recovery"]["rc"] = recovery.get("rc")
        returned = orch.wait_for_ftp(args.host, retries=60, delay=2.0)
        report["dashboard_recovery"]["dashboard_returned"] = returned
    report["dashboard_returned"] = returned
    if returned and args.post_dashboard_screenshot:
        post = out_dir / "post-dashboard.png"
        orch.capture_screenshot(args.host, post)
        if post.exists():
            report["artifacts"]["post_dashboard_png"] = str(post)

    failed = []
    if rec_rc != 0 and not args.allow_capture_failure:
        failed.append(f"composite capture rc={rec_rc}")
    if args.input_driver_cmd and report.get("input_driver_rc") != 0:
        failed.append(f"input driver rc={report.get('input_driver_rc')}")
    if not returned:
        failed.append("dashboard did not return after capture window")
    if args.capture_backend == "ffmpeg" and not video.exists() and not args.allow_capture_failure:
        failed.append("video.mp4 missing")
    if args.capture_backend == "xemu-capture" and not frame_paths and not args.allow_capture_failure:
        failed.append("reference frames missing")

    report["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    report["verdict"] = "ok" if not failed else "fail"
    if failed:
        report["failed_reasons"] = failed
    (out_dir / "verdict.json").write_text(json.dumps(report, indent=2, sort_keys=True),
                                          encoding="utf-8")
    print(json.dumps({"verdict": report["verdict"], "out_dir": str(out_dir),
                      "failed_reasons": failed}, indent=2))
    return 0 if report["verdict"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
