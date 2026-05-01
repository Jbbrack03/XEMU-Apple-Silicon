#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 out-dir duration-seconds interval-seconds start-delay-seconds" >&2
    exit 2
fi

OUT_DIR="$1"
DURATION="$2"
INTERVAL="$3"
START_DELAY="$4"

mkdir -p "$OUT_DIR"

python3 - "$OUT_DIR" "$DURATION" "$INTERVAL" "$START_DELAY" <<'PY'
import subprocess
import sys
import time
from pathlib import Path

out_dir = Path(sys.argv[1])
duration = float(sys.argv[2])
interval = float(sys.argv[3])
start_delay = float(sys.argv[4])

start = time.monotonic()
next_capture = start + start_delay
end = start + duration
index = 0

while time.monotonic() < end:
    now = time.monotonic()
    if now < next_capture:
        time.sleep(min(0.25, next_capture - now))
        continue

    elapsed = int((now - start) * 1000)
    filename = out_dir / f"{index:03d}-{elapsed:06d}ms.png"
    result = subprocess.run(
        ["screencapture", "-x", str(filename)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode == 0:
        print(f"captured {filename}", flush=True)
    else:
        print(f"capture failed at {elapsed}ms: {result.stdout.strip()}", flush=True)

    index += 1
    next_capture += interval
PY
