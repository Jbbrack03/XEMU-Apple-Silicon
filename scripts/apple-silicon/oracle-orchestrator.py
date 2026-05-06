#!/usr/bin/env python3
"""
oracle-orchestrator.py — drives the real-Xbox oracle pipeline end-to-end.

Implements the diagnostic-XBE Phase 2/3 flow described in
`docs/apple-silicon/handoff.md`:

  1. SITE RunXBE the oracle agent via FTP.
  2. Wait for TCP/9001 to come up.
  3. Optionally chainload a diagnostic XBE via `runxbe`.
  4. Wait for the Xbox to come back to FTP after the diag XBE
     reboots back to the dashboard.
  5. Pull captured artifacts via FTP into a host-side run
     directory while XBMC4Gamers' FTP server is still listening.
  6. SITE RunXBE the oracle agent again (it does not auto-relaunch).
     This step suspends FTP/21 — that's why step 5 must happen
     before step 6.
  7. Take a post-state screenshot through the agent and emit a
     JSON verdict.

This script is the bridge between the Mac-side correctness pipeline
(xemu-GL + xemu-Metal benchmark + capture) and the real-Xbox oracle.

Top-level subcommands
---------------------

  ensure-agent  Idempotent: launches the agent if not already up;
                returns 0 with the agent listening, or non-zero on
                failure. Used as a precondition by every other op.

  capture       Connect → screenshot → save under
                `xbox-real-references/<xbe-id>/<frame-id>.png`.

  run-diag      Full chainload-and-collect cycle. Args: --xbe XBOX_PATH
                [--ftp-collect XBOX_DIR] [--out HOST_DIR]. The XBE is
                expected to write its outputs onto its own D:\\
                directory (i.e. the launched-XBE's own folder), reboot
                back to dashboard, and the orchestrator pulls them.

  validate      Compare a captured artifact (PNG) against a reference
                or math-derived expected buffer. Wraps
                `compare-screenshots.py`.

  status        Probe Xbox liveness (FTP + agent TCP). Useful from
                CI / from the Stop hook.

Network preconditions:
  - Xbox is on the LAN at $ORACLE_HOST (default 192.168.0.200) with
    XBMC4Gamers running and FTP enabled (xbox/xbox).
  - The oracle-agent default.xbe is uploaded at
    /E/XBMC4Gamers/Apps/oracle-agent/default.xbe (per project layout).
"""
from __future__ import annotations

import argparse
import ftplib
import io
import json
import os
import socket
import subprocess
import sys
import time
from contextlib import closing
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

# Allow `import oracle_client` despite the dash in the filename.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import importlib.util as _imp

_spec = _imp.spec_from_file_location("oracle_client", _HERE / "oracle-client.py")
oc = _imp.module_from_spec(_spec)  # type: ignore[arg-type]
assert _spec and _spec.loader
_spec.loader.exec_module(oc)  # type: ignore[union-attr]

# `ftplib.all_errors` is a tuple of exception classes; combining it
# with OSError requires explicit unpacking, since `(OSError, tuple)`
# in an except clause is an invalid catch (TypeError at except time).
_FTP_ERRORS = (OSError,) + tuple(ftplib.all_errors)

DEFAULT_HOST = os.environ.get("ORACLE_HOST", "192.168.0.200")
DEFAULT_FTP_USER = os.environ.get("ORACLE_FTP_USER", "xbox")
DEFAULT_FTP_PASS = os.environ.get("ORACLE_FTP_PASS", "xbox")
DEFAULT_AGENT_PATH = os.environ.get(
    "ORACLE_AGENT_PATH",
    "Special://xbmc/Apps/oracle-agent/default.xbe",
)
DEFAULT_AGENT_PORT = int(os.environ.get("ORACLE_PORT", 9001))


# ---------- low-level helpers ----------

def _log(msg: str) -> None:
    print(f"[oracle] {msg}", flush=True)


def _tcp_open(host: str, port: int, timeout: float = 2.0,
              polite_oracle_probe: bool = False) -> bool:
    """TCP reachability probe. If `polite_oracle_probe`, the probe
    does an oracle-protocol handshake (read greeting, send `bye`,
    drain `200- bye`, shutdown) instead of a bare close. The polite
    path is required when probing the oracle agent's port 9001 —
    bare close + RST exhausts the agent's lwIP PCB pool after a
    dozen cycles (observed 2026-05-06; see decision-log entry of
    that date). For non-oracle ports (FTP/21, etc.) the bare probe
    is fine because it's a server with normal pcb recycling."""
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
    except OSError:
        return False
    try:
        if polite_oracle_probe:
            sock.settimeout(timeout)
            # Read greeting (200- ready). lwIP buffers the full line
            # so a single recv usually captures it.
            try:
                _ = sock.recv(256)
            except OSError:
                pass
            try:
                sock.sendall(b"bye\r\n")
            except OSError:
                pass
            try:
                _ = sock.recv(256)  # drain 200- bye
            except OSError:
                pass
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        return True
    finally:
        try:
            sock.close()
        except OSError:
            pass


def _tcp_oracle_alive(host: str, port: int, timeout: float = 2.0) -> bool:
    """Polite-close oracle-agent liveness probe. Returns True iff the
    agent is listening AND we got a clean greeting+bye round-trip."""
    return _tcp_open(host, port, timeout=timeout, polite_oracle_probe=True)


def _ping(host: str, count: int = 1, timeout_s: int = 2) -> bool:
    rc = subprocess.call(
        ["ping", "-c", str(count), "-W", str(timeout_s * 1000), host],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return rc == 0


def wait_for_ftp(host: str, retries: int = 60, delay: float = 2.0,
                 user: str = DEFAULT_FTP_USER, password: str = DEFAULT_FTP_PASS) -> bool:
    for i in range(retries):
        try:
            ftp = ftplib.FTP(host, timeout=4)
            ftp.login(user, password)
            ftp.quit()
            return True
        except _FTP_ERRORS:
            if i % 10 == 0:
                _log(f"waiting for FTP at {host} (attempt {i + 1}/{retries})")
            time.sleep(delay)
    return False


def wait_for_agent(host: str, port: int = DEFAULT_AGENT_PORT,
                   retries: int = 60, delay: float = 1.0) -> bool:
    return oc.wait_until_ready(host, port, retries, delay)


def site_run_xbe(host: str, xbe_path: str,
                 user: str = DEFAULT_FTP_USER, password: str = DEFAULT_FTP_PASS,
                 quiet: bool = False) -> None:
    """Issue `SITE RunXBE <path>` over FTP. The Xbox tears down FTP
    immediately as the kernel chainloads the named XBE, so we expect
    the connection to be reset; that is not an error."""
    try:
        ftp = ftplib.FTP(host, timeout=8)
        ftp.login(user, password)
        try:
            resp = ftp.sendcmd(f"SITE RunXBE {xbe_path}")
            if not quiet:
                _log(f"SITE RunXBE {xbe_path} → {resp}")
        finally:
            try:
                ftp.quit()
            except _FTP_ERRORS:
                pass
    except _FTP_ERRORS as e:
        # Connection-reset-by-peer is normal here.
        if not quiet:
            _log(f"SITE RunXBE: connection torn down (expected): {e}")


# ---------- agent lifecycle ----------

def ensure_agent(host: str = DEFAULT_HOST, agent_path: str = DEFAULT_AGENT_PATH,
                 port: int = DEFAULT_AGENT_PORT,
                 ready_timeout_s: float = 60.0) -> bool:
    """If TCP/<port> is already listening, do nothing. Otherwise FTP-
    `SITE RunXBE` the agent and wait for the listener to come up."""
    if _tcp_oracle_alive(host, port, timeout=2.0):
        _log(f"agent already listening at {host}:{port}")
        return True
    if not wait_for_ftp(host, retries=10, delay=1.0):
        _log(f"FTP not reachable at {host}; cannot launch agent")
        return False
    site_run_xbe(host, agent_path)
    deadline = time.monotonic() + ready_timeout_s
    delay = 1.0
    while time.monotonic() < deadline:
        if _tcp_oracle_alive(host, port, timeout=2.0):
            _log(f"agent ready at {host}:{port}")
            return True
        time.sleep(delay)
    _log(f"agent did not come up within {ready_timeout_s}s")
    return False


# ---------- artifact collection ----------

def ftp_pull_directory(host: str, remote_dir: str, local_dir: Path,
                       user: str = DEFAULT_FTP_USER,
                       password: str = DEFAULT_FTP_PASS) -> List[Path]:
    """Recursively download `remote_dir` to `local_dir`. Returns the
    list of files written. Skips directories. Tolerates the Xbox's
    "MLSD not supported" by falling back to LIST parsing."""
    local_dir.mkdir(parents=True, exist_ok=True)
    out: List[Path] = []

    ftp = ftplib.FTP(host, timeout=15)
    ftp.login(user, password)
    try:
        _walk_ftp(ftp, remote_dir, local_dir, out)
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            pass
    return out


def _walk_ftp(ftp: ftplib.FTP, remote_dir: str, local_dir: Path,
              out: List[Path]) -> None:
    try:
        ftp.cwd(remote_dir)
    except ftplib.error_perm as e:
        _log(f"cwd {remote_dir} failed: {e}")
        return
    raw_entries: List[str] = []
    ftp.retrlines("LIST", raw_entries.append)
    for line in raw_entries:
        entry = _parse_list_line(line)
        if entry is None:
            continue
        is_dir, name = entry
        if name in (".", ".."):
            continue
        remote_path = remote_dir.rstrip("/") + "/" + name
        local_path = local_dir / name
        if is_dir:
            local_path.mkdir(exist_ok=True)
            _walk_ftp(ftp, remote_path, local_path, out)
            ftp.cwd(remote_dir)  # restore CWD
        else:
            with open(local_path, "wb") as f:
                ftp.retrbinary(f"RETR {name}", f.write)
            out.append(local_path)


def _parse_list_line(line: str) -> Optional[Tuple[bool, str]]:
    """Parse a single LIST response line. Returns (is_dir, basename)
    or None on parse failure. XBMC's FTP server emits Unix-style
    `drwxr-xr-x N owner grp size date name` rows."""
    parts = line.split(maxsplit=8)
    if len(parts) < 9:
        return None
    flags = parts[0]
    name = parts[8]
    return (flags.startswith("d"), name)


# ---------- screenshot capture ----------

def capture_screenshot(host: str, out_png: Path,
                       port: int = DEFAULT_AGENT_PORT,
                       timeout: float = 30.0) -> bool:
    if not ensure_agent(host=host, port=port):
        return False
    try:
        with oc.OracleClient(host, port, timeout=timeout) as client:
            pixels, w, h, stride = client.screenshot()
            rgba = oc.bgrx_to_rgba(pixels, w, h, stride)
            out_png.parent.mkdir(parents=True, exist_ok=True)
            oc.save_screenshot_png(rgba, w, h, str(out_png))
            _log(f"captured {w}x{h} → {out_png} ({out_png.stat().st_size} bytes)")
            return True
    except oc.OracleError as e:
        _log(f"screenshot failed: {e}")
        return False


# ---------- diagnostic XBE chainload pipeline ----------

def run_diag(host: str, xbe_path: str, ftp_collect: Optional[str],
             out_dir: Path, agent_path: str = DEFAULT_AGENT_PATH,
             port: int = DEFAULT_AGENT_PORT) -> dict:
    """Full pipeline:
      1. ensure_agent
      2. screenshot pre-state (informational)
      3. runxbe to chainload diagnostic XBE
      4. wait for FTP back (= diag XBE finished + rebooted)
      5. pull artifacts from `ftp_collect` (or skip) WHILE XBMC's
         FTP server is up — relaunching the agent below suspends
         FTP/21 and would make the pull fail with
         ConnectionRefusedError, so order matters here.
      6. relaunch the agent
      7. screenshot post-state (informational)

    Returns a dict suitable for JSON.verdict.json. The diagnostic XBE
    is responsible for capturing whatever it needs to disk under its
    own D:\\ directory before rebooting; the orchestrator only knows
    where to look (`ftp_collect`). When `--ftp-collect` is supplied
    but zero files are pulled, run_diag fails with status
    "ftp-pull-empty" rather than masking it as "ok".
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    started_at = time.time()
    record: dict = {
        "host": host,
        "xbe": xbe_path,
        "ftp_collect": ftp_collect,
        "started_at": started_at,
    }
    if not ensure_agent(host=host, agent_path=agent_path, port=port):
        record["status"] = "agent-launch-failed"
        return record

    pre_png = out_dir / "pre.png"
    capture_screenshot(host, pre_png, port=port)
    record["pre_screenshot"] = str(pre_png) if pre_png.exists() else None

    _log(f"chainloading {xbe_path}")
    chainload_acked = False
    try:
        client = oc.OracleClient(host, port, timeout=30.0)
        client.connect()
        try:
            ack = client.runxbe(xbe_path)
            chainload_acked = True
            _log(f"agent acked: {ack}")
        finally:
            # The agent has already netconn_close()'d our socket, so
            # close() will swallow the bye / shutdown errors. Polite
            # close is still preferable for the case where runxbe()
            # raises an OracleError (500-) and the agent is alive.
            client.close()
    except oc.OracleRemoteError as e:
        # 500- response: agent rejected the verb; it is still
        # running. We must NOT proceed as if the chainload happened.
        record["status"] = "runxbe-rejected"
        record["error"] = str(e)
        return record
    except oc.OracleProtocolError as e:
        record["status"] = "runxbe-protocol-error"
        record["error"] = str(e)
        return record
    except oc.OracleTransportError as e:
        # Connection dropped before / during the response. If we
        # had not yet seen the ack, this is a hard failure.
        if not chainload_acked:
            record["status"] = "runxbe-transport-error"
            record["error"] = str(e)
            return record
        _log(f"runxbe post-ack drop (expected): {e}")
    except OSError as e:
        # Connection-level error. If we already saw the 200 ack the
        # drop is expected (agent self-tearing post-runxbe). If we did
        # not see an ack, this is a hard transport failure and we
        # should not proceed.
        if not chainload_acked:
            record["status"] = "runxbe-transport-error"
            record["error"] = str(e)
            return record
        _log(f"runxbe post-ack drop (expected): {e}")

    if not chainload_acked:
        record["status"] = "runxbe-no-ack"
        return record
    record["chainload_at"] = time.time()
    _log("waiting for diagnostic XBE to finish + Xbox to come back to FTP")
    if not wait_for_ftp(host, retries=120, delay=2.0):
        record["status"] = "diag-xbe-no-return"
        return record
    record["ftp_back_at"] = time.time()

    # Order matters: pull artifacts via FTP FIRST, while XBMC4Gamers'
    # FTP server is up. Once we relaunch the oracle agent below,
    # XBMC suspends and FTP/21 stops listening, so any FTP-pull
    # attempt then will fail with ConnectionRefusedError.
    if ftp_collect:
        artifact_dir = out_dir / "artifacts"
        try:
            files = ftp_pull_directory(host, ftp_collect, artifact_dir)
        except _FTP_ERRORS as e:
            record["status"] = "ftp-pull-failed"
            record["error"] = str(e)
            return record
        # An empty pull is suspicious: either --ftp-collect points at
        # a wrong path, or the diag XBE finished without writing any
        # artifacts. Either way it's a bug we want surfaced as a
        # structured failure, not buried under status="ok".
        if not files:
            record["status"] = "ftp-pull-empty"
            record["artifacts"] = []
            record["error"] = (
                f"--ftp-collect={ftp_collect} returned zero files. "
                "Either the path is wrong on the Xbox, or the diag "
                "XBE didn't write its outputs before rebooting."
            )
            return record
        record["artifacts"] = [str(p.relative_to(out_dir)) for p in files]
        _log(f"pulled {len(files)} files from {ftp_collect}")
    else:
        record["artifacts"] = []

    _log("relaunching agent for post-state capture")
    if not ensure_agent(host=host, agent_path=agent_path, port=port):
        record["status"] = "agent-relaunch-failed"
        return record

    post_png = out_dir / "post.png"
    capture_screenshot(host, post_png, port=port)
    record["post_screenshot"] = str(post_png) if post_png.exists() else None
    record["finished_at"] = time.time()
    record["status"] = "ok"
    return record


# ---------- validation ----------

def validate_against_reference(captured: Path, reference: Path,
                               crop: str, out_dir: Path,
                               threshold: int = 8,
                               diff_scale: float = 8.0,
                               resize: str = "none") -> dict:
    """Wraps the existing `compare-screenshots.py` to produce a
    pass/fail verdict in JSON-friendly form.

    Args:
        captured / reference: PNGs to compare
        crop: x,y,width,height crop rect string (compare-screenshots
              requires this; for full-frame compare on a 640x480
              capture, pass "0,0,640,480").
        out_dir: directory the compare script will write its diff
              outputs into.
        threshold / diff_scale / resize: forwarded to compare-screenshots.
    """
    cmp_path = _HERE / "compare-screenshots.py"
    if not cmp_path.exists():
        return {"status": "compare-script-missing", "path": str(cmp_path)}
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, str(cmp_path), str(captured), str(reference),
        "--crop", crop, "--out-dir", str(out_dir),
        "--threshold", str(threshold),
        "--diff-scale", str(diff_scale),
        "--resize", resize,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return {
        "status": "ok" if proc.returncode == 0 else "fail",
        "rc": proc.returncode,
        "cmd": cmd,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
    }


# ---------- CLI ----------

def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description="Real-Xbox oracle orchestrator")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=DEFAULT_AGENT_PORT)
    p.add_argument("--agent-path", default=DEFAULT_AGENT_PATH)
    p.add_argument("--ftp-user", default=DEFAULT_FTP_USER)
    p.add_argument("--ftp-pass", default=DEFAULT_FTP_PASS)
    sub = p.add_subparsers(dest="cmd", required=True)

    pe = sub.add_parser("ensure-agent",
                        help="Launch the agent if not already running")
    pe.add_argument("--timeout", type=float, default=60.0)

    pc = sub.add_parser("capture",
                        help="Save a single front-buffer screenshot")
    pc.add_argument("--out", required=True)

    pr = sub.add_parser("run-diag",
                        help="Chainload a diagnostic XBE and collect outputs")
    pr.add_argument("--xbe", required=True)
    pr.add_argument("--ftp-collect", help="FTP directory to mirror after run")
    pr.add_argument("--out", required=True, help="local output directory")

    pv = sub.add_parser("validate",
                        help="Compare captured PNG vs reference PNG")
    pv.add_argument("--captured", required=True)
    pv.add_argument("--reference", required=True)
    pv.add_argument("--crop", default="0,0,640,480",
                    help="x,y,width,height crop rectangle "
                         "(default: 0,0,640,480 for full agent screenshots)")
    pv.add_argument("--out-dir", required=True,
                    help="directory for compare-screenshots.py outputs "
                         "(diff PNG, mask PNG, summary)")
    pv.add_argument("--threshold", type=int, default=8,
                    help="per-channel absolute difference threshold")
    pv.add_argument("--diff-scale", type=float, default=8.0)
    pv.add_argument("--resize", choices=("none", "smaller"), default="none")

    sub.add_parser("status", help="Probe FTP+agent liveness")

    args = p.parse_args(argv)

    if args.cmd == "status":
        ok_ping = _ping(args.host)
        ok_ftp = _tcp_open(args.host, 21, timeout=2.0)
        ok_agent = _tcp_oracle_alive(args.host, args.port, timeout=2.0)
        result = {
            "host": args.host,
            "ping": ok_ping, "ftp": ok_ftp, "agent": ok_agent,
        }
        print(json.dumps(result, indent=2))
        return 0 if (ok_ping and ok_ftp) else 1

    if args.cmd == "ensure-agent":
        ok = ensure_agent(host=args.host, agent_path=args.agent_path,
                          port=args.port, ready_timeout_s=args.timeout)
        return 0 if ok else 1

    if args.cmd == "capture":
        out_path = Path(args.out)
        ok = capture_screenshot(args.host, out_path, port=args.port)
        return 0 if ok else 1

    if args.cmd == "run-diag":
        out_dir = Path(args.out)
        record = run_diag(args.host, args.xbe, args.ftp_collect,
                          out_dir, agent_path=args.agent_path, port=args.port)
        with open(out_dir / "verdict.json", "w") as f:
            json.dump(record, f, indent=2)
        print(json.dumps(record, indent=2))
        return 0 if record.get("status") == "ok" else 1

    if args.cmd == "validate":
        result = validate_against_reference(
            Path(args.captured), Path(args.reference),
            crop=args.crop, out_dir=Path(args.out_dir),
            threshold=args.threshold, diff_scale=args.diff_scale,
            resize=args.resize,
        )
        print(json.dumps(result, indent=2))
        return 0 if result.get("status") == "ok" else 1

    p.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
