#!/usr/bin/env python3
import argparse
import json
import socket
import time
from pathlib import Path


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(30.0)
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
    parser = argparse.ArgumentParser(description="Run an HMP command over QMP")
    parser.add_argument("--socket", required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("command", nargs="+")
    args = parser.parse_args()

    command_line = " ".join(args.command)
    qmp = wait_for_socket(args.socket, args.timeout)
    try:
        result = qmp.command("human-monitor-command", {
            "command-line": command_line,
        })
        if result:
            print(result, end="" if result.endswith("\n") else "\n")
    finally:
        qmp.close()


if __name__ == "__main__":
    main()
