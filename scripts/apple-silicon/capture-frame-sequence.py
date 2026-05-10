#!/usr/bin/env python3
"""Capture a timed PNG frame sequence through the approved xemu-capture app."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def parse_json(text: str) -> dict[str, Any]:
    try:
        data = json.loads(text)
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def write_meta(path: Path, meta: dict[str, Any]) -> None:
    path.write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("device")
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--duration", type=float, required=True)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--width", type=int, default=720)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--timeout", type=float, default=8.0)
    ap.add_argument("--warmup-frames", type=int, default=5)
    ap.add_argument("--label", default="reference-frames")
    args = ap.parse_args(argv)

    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    meta_path = out_dir / "capture-meta.json"
    log_path = out_dir / "capture.log"
    capture_tool = HERE / "xemu-capture-app.py"
    count = max(1, int(args.duration / args.interval) + 1)

    meta: dict[str, Any] = {
        "schema": "xemu-capture-frame-sequence-v1",
        "status": "in_progress",
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "device": args.device,
        "out_dir": str(out_dir),
        "duration_target_s": args.duration,
        "interval_s": args.interval,
        "width": args.width,
        "height": args.height,
        "timeout_s": args.timeout,
        "warmup_frames": args.warmup_frames,
        "label": args.label,
        "target_frame_count": count,
        "frames": [],
    }
    write_meta(meta_path, meta)

    start = time.monotonic()
    ok_count = 0
    with log_path.open("w", encoding="utf-8") as log:
        for i in range(count):
            target = start + i * args.interval
            sleep_s = target - time.monotonic()
            if sleep_s > 0:
                time.sleep(sleep_s)

            frame_path = out_dir / f"frame-{i + 1:04d}.png"
            cmd = [
                sys.executable, str(capture_tool),
                "snapshot", args.device,
                "--out", str(frame_path),
                "--width", str(args.width),
                "--height", str(args.height),
                "--timeout", str(args.timeout),
                "--warmup-frames", str(args.warmup_frames),
            ]
            frame_started = time.monotonic()
            proc = subprocess.run(
                cmd,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=args.timeout + 20.0,
                check=False,
            )
            elapsed = time.monotonic() - frame_started
            log.write(f"$ {' '.join(cmd)}\n")
            if proc.stdout:
                log.write(proc.stdout)
                if not proc.stdout.endswith("\n"):
                    log.write("\n")
            if proc.stderr:
                log.write(proc.stderr)
                if not proc.stderr.endswith("\n"):
                    log.write("\n")
            log.flush()

            result = parse_json(proc.stdout)
            ok = proc.returncode == 0 and frame_path.exists()
            if ok:
                ok_count += 1
            meta["frames"].append({
                "index": i + 1,
                "target_offset_s": round(i * args.interval, 3),
                "wall_offset_s": round(time.monotonic() - start, 3),
                "elapsed_s": round(elapsed, 3),
                "rc": proc.returncode,
                "ok": ok,
                "path": str(frame_path),
                "result": result,
            })
            meta["frames_ok"] = ok_count
            write_meta(meta_path, meta)

    meta["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    meta["wall_elapsed_s"] = round(time.monotonic() - start, 3)
    meta["frames_ok"] = ok_count
    meta["status"] = "ok" if ok_count > 0 else "no_frames"
    write_meta(meta_path, meta)
    print(json.dumps({
        "status": meta["status"],
        "out_dir": str(out_dir),
        "frames_ok": ok_count,
        "target_frame_count": count,
        "meta": str(meta_path),
    }, indent=2))
    return 0 if ok_count > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
