#!/usr/bin/env python3
import argparse
import json
import socket
import time
from pathlib import Path


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

        while time.monotonic() < end:
            now = time.monotonic()
            if now < next_capture:
                time.sleep(min(0.25, next_capture - now))
                continue

            elapsed = int((now - start) * 1000)
            filename = out_dir / f"{index:03d}-{elapsed:06d}ms.png"
            try:
                qmp.command("screendump", {
                    "filename": str(filename),
                    "format": "png",
                })
                print(f"captured {filename}", flush=True)
            except RuntimeError as exc:
                print(f"capture failed at {elapsed}ms: {exc}", flush=True)

            index += 1
            next_capture += args.interval
    finally:
        qmp.close()


if __name__ == "__main__":
    main()
