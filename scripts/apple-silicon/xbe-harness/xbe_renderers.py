"""
xbe_renderers — per-renderer drivers for diag XBEs.

Each driver knows how to:
  1. Take a built diag XBE (iso path on disk).
  2. Run it on the renderer (xemu-GL / xemu-Metal / real-xbox).
  3. Produce a captured front-buffer PNG at a known path.

The drivers normalize across renderer differences so the comparison
layer (xbe_compare.py) can treat all captures uniformly.
"""
from __future__ import annotations

import importlib.util
import json
import os
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from xbe_discover import XbeManifest

_HERE = Path(__file__).resolve().parent
_AS_DIR = _HERE.parent
_FORK_ROOT = _AS_DIR.parent.parent
ORACLE_ORCH_PATH = _AS_DIR / "oracle-orchestrator.py"
ORACLE_CLIENT_PATH = _AS_DIR / "oracle-client.py"

XEMU_BIN = _FORK_ROOT / "dist" / "xemu.app" / "Contents" / "MacOS" / "xemu"
MCPX = Path("/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/mcpx/mcpx_1.0.bin")
BIOS = Path("/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/bios/Complex_4627.bin")
HDD_SOURCE = Path("/Users/jbbrack03/XEMU_MacOS/Xbox-Emulator-Files/hdd/xbox_hdd.qcow2")


@dataclass
class RunResult:
    status: str            # "ok" | "fail" | "infra-error"
    captured_png: Path
    log: str               # stdout+stderr from the renderer
    notes: str = ""
    extra: dict = None     # type: ignore[assignment]


# ---------- shared helpers ----------

def _load_oc():
    spec = importlib.util.spec_from_file_location(
        "oracle_client", ORACLE_CLIENT_PATH)
    mod = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    assert spec and spec.loader
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


# ---------- xemu (GL + Metal) ----------

def _build_xemu_toml(work_dir: Path, scratch_hdd: Path,
                     iso: Path, surface_scale: int) -> Path:
    """Write a minimal xemu.toml that points at the diag iso, plus
    BIOS/MCPX/HDD."""
    cfg = work_dir / "xemu.toml"
    cfg.write_text(
        "[general]\n"
        "show_welcome = false\n\n"
        "[input]\n"
        "auto_bind = true\n"
        "background_input_capture = true\n\n"
        "[display.window]\n"
        "vsync = false\n\n"
        "[display.quality]\n"
        f"surface_scale = {surface_scale}\n\n"
        "[sys.files]\n"
        f"bootrom_path = '{MCPX}'\n"
        f"flashrom_path = '{BIOS}'\n"
        f"hdd_path = '{scratch_hdd}'\n"
        f"dvd_path = '{iso}'\n\n"
        "[input.bindings]\n"
        "port1 = 'keyboard'\n"
        "port1_driver = 'usb-xbox-gamepad'\n"
    )
    return cfg


def _qmp_quit(qmp_socket: Path, timeout: float = 1.0) -> bool:
    try:
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        s.connect(str(qmp_socket))
        s.recv(4096)
        s.sendall(json.dumps({"execute": "qmp_capabilities"}).encode() + b"\r\n")
        s.recv(4096)
        s.sendall(json.dumps({"execute": "quit"}).encode() + b"\r\n")
        s.close()
        return True
    except OSError:
        return False


def _ensure_xemu_built() -> Path:
    if not XEMU_BIN.exists():
        raise FileNotFoundError(
            f"xemu binary not built at {XEMU_BIN}. Run "
            f"`./build.sh -a arm64` from xemu-fork/.")
    return XEMU_BIN


def _kill_existing_xemu() -> None:
    """Best-effort: kill any pre-existing xemu process so we can run a
    fresh one. The launcher ordinarily refuses with
    XEMU_BENCH_ALLOW_EXISTING=0; here we own the harness and want a
    clean slate per run."""
    try:
        out = subprocess.check_output(["pgrep", "-f", str(XEMU_BIN)],
                                      stderr=subprocess.DEVNULL).decode().strip()
    except subprocess.CalledProcessError:
        return
    if not out:
        return
    for pid in out.split():
        try:
            os.kill(int(pid), signal.SIGTERM)
        except (OSError, ValueError):
            pass
    time.sleep(1.0)


def run_xemu(manifest: XbeManifest, renderer: str,
             work_dir: Path,
             flag_recipe: Dict[str, str],
             surface_scale: int = 1,
             timeout_seconds: int = 35) -> RunResult:
    """Run a diag XBE on xemu (GL or Metal). Captures the post-flip
    front buffer via the in-renderer screenshot path.

    `renderer` is "GL" or "METAL". `flag_recipe` is a dict of
    XEMU_* env-var overrides (e.g. {"XEMU_METAL_FRONT_FB_FALLBACK":
    "1"} for the Metal canonical recipe).

    Always sets:
      - XEMU_PERF_LOG=1, interval=1000ms (so xemu emits the closing
        atexit-interval line for renderer health introspection).
      - XEMU_DISPLAY_SCALE=<surface_scale>.
      - For Metal: XEMU_METAL_SCREENSHOT_PATH + interval/at-frame so a
        sequence of post-flip captures lands in work_dir/screenshots/.
      - For GL: macos-capture.sh as a sibling capture process (window
        screencapture path).

    Returns a RunResult with `captured_png` set to the chosen
    representative capture (the latest existing PNG in the screenshot
    sequence) or to a sentinel "no-capture" path.
    """
    _ensure_xemu_built()
    _kill_existing_xemu()

    work_dir.mkdir(parents=True, exist_ok=True)
    scratch_hdd = work_dir / "xbox_hdd.qcow2"
    if not HDD_SOURCE.exists():
        return RunResult("infra-error", work_dir / "no-capture.png",
                         "", notes="hdd-source-missing")
    # Fast clone via APFS (-c) when available; fall back to plain cp.
    rc = subprocess.call(["cp", "-c", str(HDD_SOURCE), str(scratch_hdd)],
                         stderr=subprocess.DEVNULL)
    if rc != 0:
        shutil.copy(HDD_SOURCE, scratch_hdd)

    cfg = _build_xemu_toml(work_dir, scratch_hdd, manifest.iso_path,
                           surface_scale)
    # Cycle-17: allow per-run timeout override for diagnostic flags that
    # add wait latency (e.g. XEMU_DIAG_PGRAPH_STATUS_DRAIN). Default 35s
    # is unchanged for normal flag combinations.
    env_timeout = os.environ.get("XBE_HARNESS_TIMEOUT_SECONDS")
    if env_timeout:
        try:
            timeout_seconds = int(env_timeout)
        except ValueError:
            pass
    # QMP UNIX-socket paths cap at ~104 bytes on macOS; benchmark-runs
    # nesting (m15-gate-<UTC>/04-tier1-matrix/<xbe>/<renderer>/) blows
    # past that. Use /tmp directly with a short unique filename.
    qmp_sock = Path("/tmp") / f"xq-{os.getpid()}-{secrets.token_hex(4)}.sock"
    log_path = work_dir / "xemu.log"
    screenshots_dir = work_dir / "screenshots"
    screenshots_dir.mkdir(exist_ok=True)
    base_screenshot = screenshots_dir / f"{manifest.id}.png"

    env = os.environ.copy()
    # Project-default flags that remain unaffected by the diag recipe.
    env.setdefault("XEMU_PERF_LOG", "1")
    env.setdefault("XEMU_PERF_LOG_INTERVAL_MS", "1000")
    env.setdefault("XEMU_SNAPSHOT_NO_THUMBNAIL", "1")
    env.setdefault("XEMU_DISPLAY_SCALE", str(surface_scale))
    env.setdefault("XEMU_NATIVE_TRI_DEPTH", "1")
    env.setdefault("XEMU_NATIVE_QUAD", "1")
    env.setdefault("XEMU_PGRAPH_FAST_READ", "1")

    if renderer.upper() == "METAL":
        env["XEMU_RENDERER"] = "METAL"
        env.setdefault("XEMU_METAL_TRANSLATED_PIPELINE", "1")
        # Diag XBEs paint deterministic patterns to the back buffer
        # then flip; set a wide-enough capture window to land at
        # least one PNG after the flip but before the XBE's reboot.
        env.setdefault("XEMU_METAL_SCREENSHOT_PATH", str(base_screenshot))
        env.setdefault("XEMU_METAL_SCREENSHOT_AT_FRAME", "30")
        env.setdefault("XEMU_METAL_SCREENSHOT_INTERVAL", "15")
        # Diag XBEs are sensitive to HUD overlay artifacts in the
        # captured drawable; force HUD off (matches manifest's
        # required_flags).
        env["XEMU_METAL_HUD"] = "0"
        env["XEMU_METAL_VALIDATION"] = "1"
    elif renderer.upper() == "GL":
        # Explicit unset so we don't inherit a parent METAL.
        env["XEMU_RENDERER"] = "GL"
    else:
        return RunResult("infra-error", base_screenshot, "",
                         notes=f"unknown renderer: {renderer}")

    # Apply per-recipe overrides (these win).
    for k, v in (flag_recipe or {}).items():
        env[k] = str(v)

    args = [
        str(XEMU_BIN),
        "-config_path", str(cfg),
        "-qmp", f"unix:{qmp_sock},server=on,wait=off",
    ]

    log_f = log_path.open("wb")
    try:
        proc = subprocess.Popen(args, env=env, stdout=log_f, stderr=log_f)
    except OSError as e:
        log_f.close()
        return RunResult("infra-error", base_screenshot, str(e),
                         notes="xemu-exec-failed")

    # GL: spawn macos-capture.sh as a sidecar to capture window frames.
    # macos-capture.sh signature: out-dir duration-s interval-s start-delay-s
    capture_proc: Optional[subprocess.Popen] = None
    if renderer.upper() == "GL":
        cap_dir = screenshots_dir
        cap_interval = "2"
        cap_start_delay = "8"  # let xemu boot before first capture
        capture_args = [
            str(_AS_DIR / "macos-capture.sh"),
            str(cap_dir), str(timeout_seconds),
            cap_interval, cap_start_delay,
        ]
        cap_env = env.copy()
        cap_env["XEMU_CAPTURE_WINDOW_PATTERN"] = "xemu"
        cap_log = work_dir / "capture.log"
        try:
            capture_proc = subprocess.Popen(
                capture_args, env=cap_env,
                stdout=cap_log.open("wb"), stderr=subprocess.STDOUT)
        except OSError as e:
            log_f.write(f"\n[harness] macos-capture failed: {e}\n".encode())

    # Wait timeout_seconds OR until the process exits (which happens
    # once the diag XBE issues HalReturnToFirmware and xemu winds
    # down... actually xemu will just keep emulating the dashboard,
    # so we always have to time out).
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline and proc.poll() is None:
        time.sleep(0.5)

    # Quit xemu cleanly via QMP; fall back to TERM/KILL if needed.
    if proc.poll() is None:
        if qmp_sock.exists():
            _qmp_quit(qmp_sock)
        for _ in range(15):
            if proc.poll() is not None:
                break
            time.sleep(1.0)
        if proc.poll() is None:
            proc.terminate()
            for _ in range(5):
                if proc.poll() is not None:
                    break
                time.sleep(1.0)
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)

    if capture_proc is not None and capture_proc.poll() is None:
        capture_proc.terminate()
        try:
            capture_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            capture_proc.kill()

    log_f.close()

    # Best-effort QMP socket cleanup (xemu unlinks on graceful exit;
    # cover the SIGKILL path too).
    try:
        qmp_sock.unlink()
    except FileNotFoundError:
        pass
    except OSError:
        pass

    # Pick the latest captured PNG. Prefer the .NNNN sequence form
    # from the Metal capture path; fall back to plain base_screenshot.
    pngs = sorted(screenshots_dir.glob("*.png"))
    if not pngs:
        return RunResult("fail", base_screenshot,
                         log_path.read_text(errors="replace"),
                         notes="no-screenshot-captured")
    chosen = pngs[-1]
    return RunResult(
        status="ok", captured_png=chosen,
        log=log_path.read_text(errors="replace"),
        extra={"screenshots": [str(p) for p in pngs]})


# ---------- real-xbox (oracle agent) ----------

def _ensure_agent_via_orch(host: str, agent_path: str) -> bool:
    """Spawn oracle-orchestrator.py ensure-agent. Returns True if the
    agent ack'd. Used to bring the agent up between FTP upload and the
    optional pre-run controller setup."""
    cmd = [sys.executable, str(ORACLE_ORCH_PATH),
           "--host", host, "--agent-path", agent_path,
           "ensure-agent"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=120)
        return proc.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _pre_run_setup_real_xbox(manifest: XbeManifest, host: str,
                             agent_path: str) -> List[str]:
    """Diag-XBE-specific setup that runs on the real Xbox AFTER the
    XBE has been uploaded and BEFORE chainload. The agent must be up
    on return; the caller's run-diag will see it as already up.

    Returns a list of log lines describing what was done.
    """
    log: List[str] = []
    if manifest.id == "controller-roundtrip":
        # The persistent kernel-pool controller buffer survives across
        # XLaunchXBE; we don't know its state at run start (could be
        # non-zero from a prior session). controller-roundtrip's
        # `expected.py:default()` synthesizes the zero-state pattern,
        # so we MUST write and verify an explicit all-zero state before
        # chainload. Use controller.set instead of controller.clear
        # because set is the same path used by non-zero replay tests.
        log.append("[pre-run] ensuring agent up for controller.set zero")
        if not _ensure_agent_via_orch(host, agent_path):
            log.append("[pre-run] ERROR ensure-agent failed")
            return log
        try:
            oc = _load_oc()
            with oc.OracleClient(host, 9001, timeout=10.0) as c:
                zero = ("controller.set port=0 buttons=0x0000 "
                        "lt=0 rt=0 lx=0 ly=0 rx=0 ry=0")
                code, payload = c.raw(zero)
                log.append(f"[pre-run] {zero} -> {code} "
                           f"{payload.decode(errors='replace')}")
                if code != "200":
                    log.append("[pre-run] ERROR zero-state set failed")
                    return log
                code, payload = c.raw("controller.get port=0")
                text = payload.decode(errors="replace")
                log.append(f"[pre-run] controller.get port=0 -> "
                           f"{code}\n{text}")
                required = [
                    "buttons=0x0000",
                    "triggers lt=0 rt=0",
                    "lstick x=0 y=0",
                    "rstick x=0 y=0",
                ]
                if code != "201" or any(tok not in text for tok in required):
                    log.append("[pre-run] ERROR zero-state verify failed")
        except Exception as e:
            log.append(f"[pre-run] ERROR controller zero-state failed: {e}")
    return log


def run_real_xbox(manifest: XbeManifest, work_dir: Path,
                  host: str = "192.168.0.200",
                  agent_path: str =
                  "E:\\Apps\\oracle-agent\\default.xbe",
                  ftp_user: str = "xbox", ftp_pass: str = "xbox",
                  upload_xbe: bool = True) -> RunResult:
    """Deploy + chainload + collect on the real Xbox.

    Steps:
      1. ensure_agent (FTP-launch the oracle agent if not already up).
      2. (optional, default ON) FTP-upload the diag XBE to its
         standard slot under E:\\Apps\\<id>\\default.xbe (dashboard-
         independent path; was E:\\XBMC4Gamers\\Apps\\... pre 2026-05-07).
         Requires the agent to be DOWN (UnleashX's FTP server suspends
         while the agent runs); if the agent is up we reboot first.
      3. oracle-orchestrator.py run-diag (chainload + collect).
      4. Decode the collected XOSS capture into a PNG.
    """
    work_dir.mkdir(parents=True, exist_ok=True)
    captured_png = work_dir / f"{manifest.id}.png"
    log_lines: List[str] = []

    def log(s: str) -> None:
        log_lines.append(s)

    def finish(status: str, notes: str = "",
               extra: Optional[dict] = None) -> RunResult:
        try:
            (work_dir / "real-xbox.log").write_text(
                "\n".join(log_lines) + "\n", errors="replace")
        except OSError:
            pass
        return RunResult(status=status, captured_png=captured_png,
                         log="\n".join(log_lines), notes=notes,
                         extra=extra)

    if not manifest.xbe_path.exists():
        return finish("infra-error", f"xbe not built: {manifest.xbe_path}")

    if upload_xbe:
        # Need FTP up. If agent is up, reboot first via the agent.
        if _agent_alive(host):
            log("agent is up; sending reboot to free FTP")
            _agent_reboot(host)
        ok = _wait_for_ftp(host, retries=120, delay=2.0,
                           user=ftp_user, password=ftp_pass)
        if not ok:
            return finish("infra-error", "ftp-not-back-after-reboot")
        rc, out = _ftp_upload_xbe(host, manifest.id, manifest.xbe_path,
                                  user=ftp_user, password=ftp_pass)
        log(out)
        if rc != 0:
            return finish("infra-error", "xbe-upload-failed")

    # Per-XBE pre-run setup (e.g. controller-roundtrip needs
    # a verified zero-state write before chainload).
    for line in _pre_run_setup_real_xbox(manifest, host, agent_path):
        log(line)
    if any("[pre-run] ERROR" in line for line in log_lines):
        return finish("infra-error", "pre-run-setup-failed")

    xbox_xbe_path = f"E:\\\\Apps\\\\{manifest.id}\\\\default.xbe"
    ftp_collect = f"/E/Apps/{manifest.id}"
    cmd = [
        sys.executable, str(ORACLE_ORCH_PATH),
        "--host", host, "--agent-path", agent_path,
        "--ftp-user", ftp_user, "--ftp-pass", ftp_pass,
        "run-diag",
        "--xbe", xbox_xbe_path,
        "--ftp-collect", ftp_collect,
        "--out", str(work_dir / "orch"),
    ]
    log(f"$ {' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    log(proc.stdout)
    log(proc.stderr)
    if proc.returncode != 0:
        return finish("fail", f"run-diag-rc={proc.returncode}")

    # Locate the captured XOSS blob.
    artifacts_dir = work_dir / "orch" / "artifacts"
    capture_blob = artifacts_dir / f"{manifest.id}-capture.bin"
    if not capture_blob.exists():
        # Fallback: search for *-capture.bin
        candidates = list(artifacts_dir.glob("*-capture.bin"))
        if not candidates:
            return finish("fail", "no-xoss-blob-pulled")
        capture_blob = candidates[0]

    # Decode to PNG.
    try:
        from xbe_compare import decode_xoss_to_png
        decode_xoss_to_png(capture_blob, captured_png)
    except Exception as e:
        return finish("fail", f"xoss-decode-failed: {e}")

    return finish("ok", extra={"xoss": str(capture_blob)})


# ---------- real-xbox helpers ----------

def _agent_alive(host: str, port: int = 9001, timeout: float = 2.0) -> bool:
    """Polite-close oracle handshake (greeting + bye + shutdown)."""
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        try:
            s.settimeout(timeout)
            try: s.recv(256)
            except OSError: pass
            try: s.sendall(b"bye\r\n")
            except OSError: pass
            try: s.recv(256)
            except OSError: pass
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass
        finally:
            s.close()
        return True
    except OSError:
        return False


def _agent_reboot(host: str, port: int = 9001) -> None:
    oc = _load_oc()
    try:
        with oc.OracleClient(host, port, timeout=10.0) as c:
            try:
                c.reboot()
            except oc.OracleError:
                pass
    except OSError:
        pass


def _wait_for_ftp(host: str, retries: int, delay: float,
                  user: str, password: str) -> bool:
    import ftplib
    for i in range(retries):
        try:
            ftp = ftplib.FTP(host, timeout=4)
            ftp.login(user, password)
            ftp.quit()
            return True
        except (OSError,) + tuple(ftplib.all_errors):
            time.sleep(delay)
    return False


def _ftp_upload_xbe(host: str, xbe_id: str, src_xbe: Path,
                    user: str, password: str) -> Tuple[int, str]:
    """Upload `src_xbe` to /E/Apps/<id>/default.xbe (dashboard-
    independent path under UnleashX; was /E/XBMC4Gamers/Apps/<id>/
    pre 2026-05-07). Creates the parent dir if missing. Returns
    (rc, log)."""
    import ftplib
    log_lines: List[str] = []
    target_dir = f"/E/Apps/{xbe_id}"
    try:
        ftp = ftplib.FTP(host, timeout=15)
        ftp.login(user, password)
        try:
            try:
                ftp.cwd(target_dir)
            except ftplib.error_perm:
                # mkd path components
                ftp.cwd("/E/Apps")
                try:
                    ftp.mkd(xbe_id)
                except ftplib.error_perm as e:
                    log_lines.append(f"mkd {xbe_id} failed: {e}")
                ftp.cwd(target_dir)
            with src_xbe.open("rb") as f:
                ftp.storbinary("STOR default.xbe", f)
            log_lines.append(f"uploaded {src_xbe} -> {target_dir}/default.xbe")
        finally:
            try:
                ftp.quit()
            except (OSError,) + tuple(ftplib.all_errors):
                pass
        return 0, "\n".join(log_lines)
    except (OSError,) + tuple(__import__("ftplib").all_errors) as e:
        log_lines.append(f"ftp upload failed: {e}")
        return 1, "\n".join(log_lines)
