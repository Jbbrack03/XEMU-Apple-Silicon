#!/usr/bin/env python3
import argparse
import json
import os
import socket
import time
from pathlib import Path

from PIL import Image


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(5.0)
        self.sock.connect(path)
        self.file = self.sock.makefile("rwb", buffering=0)
        self._read_message()
        self.command("qmp_capabilities")

    def close(self):
        self.file.close()
        self.sock.close()

    def _read_message(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError("QMP socket closed")
        return json.loads(line.decode("utf-8"))

    def command(self, execute, arguments=None):
        request = {"execute": execute}
        if arguments is not None:
            request["arguments"] = arguments
        self.file.write(json.dumps(request).encode("utf-8") + b"\r\n")

        while True:
            response = self._read_message()
            if "event" in response:
                continue
            if "error" in response:
                desc = response["error"].get("desc", response["error"])
                raise RuntimeError(f"{execute}: {desc}")
            return response.get("return")


def wait_for_socket(path, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if Path(path).exists():
            try:
                return QMP(path)
            except (OSError, RuntimeError):
                pass
        time.sleep(0.25)
    raise TimeoutError(f"QMP socket did not become ready: {path}")


def main():
    parser = argparse.ArgumentParser(description="Capture xemu screenshots via QMP")
    parser.add_argument("--socket", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--duration", type=float, required=True)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--start-delay", type=float, default=5.0)
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    qmp = wait_for_socket(args.socket, 15)
    try:
        start = time.monotonic()
        next_capture = start + args.start_delay
        end = start + args.duration
        index = 0
        flip_stall_sentinel = os.environ.get("XEMU_CAPTURE_FLIP_STALL_SENTINEL", "")

        def capture(filename):
            filename = Path(filename)
            try:
                qmp.command("screendump", {
                    "filename": str(filename),
                    "format": "png",
                })
            except RuntimeError as exc:
                # Some xemu/QEMU builds do not expose QMP screendump
                # directly. When HMP screendump is available through
                # human-monitor-command, write PPM and convert to PNG so
                # the downstream screenshot diff stays unchanged.
                ppm = filename.with_suffix(".ppm")
                try:
                    if ppm.exists():
                        ppm.unlink()
                except OSError:
                    pass
                try:
                    hmp_ret = qmp.command("human-monitor-command", {
                        "command-line": f"screendump {ppm} -f ppm",
                    })
                except RuntimeError as hmp_exc:
                    print(
                        f"capture failed: qmp={exc}; hmp={hmp_exc}",
                        flush=True,
                    )
                    return
                if hmp_ret:
                    print(f"qmp-capture: hmp screendump returned: {hmp_ret!r}", flush=True)
                if ppm.exists():
                    Image.open(ppm).save(filename)
                    ppm.unlink()
            if filename.exists():
                print(f"captured {filename}", flush=True)
            else:
                print(f"capture failed: no output file written: {filename}", flush=True)

        if flip_stall_sentinel:
            sentinel = Path(flip_stall_sentinel)
            print(
                f"qmp-capture: flip-stall sentinel mode sentinel={sentinel} "
                f"timeout={args.duration}s",
                flush=True,
            )
            while time.monotonic() < end:
                if sentinel.exists():
                    elapsed = int((time.monotonic() - start) * 1000)
                    capture(out_dir / f"flip-stall-{elapsed:06d}ms.png")
                    remaining = end - time.monotonic()
                    if remaining > 0:
                        time.sleep(remaining)
                    break
                time.sleep(0.1)
            else:
                print(
                    f"qmp-capture: flip-stall sentinel did not appear within "
                    f"{args.duration}s; no shot taken",
                    flush=True,
                )
            return

        while time.monotonic() < end:
            now = time.monotonic()
            if now < next_capture:
                time.sleep(min(0.25, next_capture - now))
                continue

            elapsed = int((now - start) * 1000)
            capture(out_dir / f"{index:03d}-{elapsed:06d}ms.png")
            index += 1
            next_capture += args.interval
    finally:
        qmp.close()


if __name__ == "__main__":
    main()
