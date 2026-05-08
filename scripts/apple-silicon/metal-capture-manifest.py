#!/usr/bin/env python3
"""Create a sidecar manifest for a programmatic Metal .gputrace run.

The .gputrace itself is still an Xcode-owned document, but this manifest
keeps the surrounding benchmark evidence machine-readable: run metadata,
capture path/size, capture start/stop log lines, and the Metal-related perf
tail. The goal is to make every capture auditable without reopening Xcode
just to know what run produced it.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
from typing import Any


ROOT = Path(__file__).resolve().parents[2]


def read_text(path: Path, limit: int | None = None) -> str:
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            return f.read() if limit is None else f.read(limit)
    except OSError:
        return ""


def parse_metadata(path: Path) -> dict[str, str]:
    meta: dict[str, str] = {}
    for line in read_text(path).splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        key = key.strip()
        if key:
            meta[key] = value.strip()
    return meta


def path_size(path: Path) -> int:
    if not path.exists():
        return 0
    if path.is_file():
        return path.stat().st_size
    total = 0
    for p in path.rglob("*"):
        try:
            if p.is_file():
                total += p.stat().st_size
        except OSError:
            pass
    return total


def path_file_count(path: Path) -> int:
    if not path.exists():
        return 0
    if path.is_file():
        return 1
    return sum(1 for p in path.rglob("*") if p.is_file())


def scan_log(path: Path, tail_limit: int = 40) -> dict[str, Any]:
    capture_lines: list[str] = []
    metal_perf_lines: list[str] = []
    validation_lines: list[str] = []
    text = read_text(path)
    for line in text.splitlines():
        low = line.lower()
        if "metal_capture" in line or ".gputrace" in line:
            capture_lines.append(line)
        if "xemu-perf:" in line and ("METAL_" in line or "metal_" in line):
            metal_perf_lines.append(line)
        if "metal" in low and ("validation" in low or "error" in low or "warning" in low):
            validation_lines.append(line)
    started = any("metal_capture_started" in line for line in capture_lines)
    stopped = any("metal_capture_stopped" in line for line in capture_lines)
    return {
        "capture_started": started,
        "capture_stopped": stopped,
        "capture_lines": capture_lines[-tail_limit:],
        "metal_perf_tail": metal_perf_lines[-tail_limit:],
        "metal_validation_tail": validation_lines[-tail_limit:],
    }


def infer_capture_path(run_dir: Path, meta: dict[str, str]) -> Path | None:
    for key in ("metal_capture_path", "env_XEMU_METAL_CAPTURE"):
        value = meta.get(key, "")
        if value and value not in {"none", "unset"}:
            return Path(value)
    candidates = sorted(run_dir.glob("*.gputrace"))
    if candidates:
        return candidates[-1]
    return None


def make_manifest(run_dir: Path, capture: Path | None) -> dict[str, Any]:
    meta = parse_metadata(run_dir / "metadata.txt")
    if capture is None:
        capture = infer_capture_path(run_dir, meta)
    log_scan = scan_log(run_dir / "xemu.log")
    capture_abs = capture.resolve() if capture else None
    capture_exists = bool(capture_abs and capture_abs.exists())
    manifest: dict[str, Any] = {
        "schema": "xemu-metal-capture-manifest-v1",
        "repo": str(ROOT),
        "run_dir": str(run_dir.resolve()),
        "metadata_path": str((run_dir / "metadata.txt").resolve()),
        "log_path": str((run_dir / "xemu.log").resolve()),
        "game": meta.get("game"),
        "duration_seconds": meta.get("duration_seconds"),
        "renderer": meta.get("env_XEMU_RENDERER"),
        "surface_scale": meta.get("surface_scale"),
        "metal_capture_frames": meta.get("env_XEMU_METAL_CAPTURE_FRAMES"),
        "metal_auto_validation": meta.get("metal_auto_validation"),
        "metal_auto_hud": meta.get("metal_auto_hud"),
        "capture": {
            "path": str(capture_abs) if capture_abs else None,
            "exists": capture_exists,
            "kind": "directory" if capture_abs and capture_abs.is_dir() else "file" if capture_abs and capture_abs.is_file() else "missing",
            "bytes": path_size(capture_abs) if capture_abs else 0,
            "file_count": path_file_count(capture_abs) if capture_abs else 0,
        },
        "log_scan": log_scan,
        "open_in_xcode": f"open -a Xcode {capture_abs}" if capture_abs else None,
        "readiness": {
            "has_capture_document": capture_exists,
            "has_start_log": log_scan["capture_started"],
            "has_stop_log": log_scan["capture_stopped"],
        },
    }
    missing = [k for k, v in manifest["readiness"].items() if not v]
    manifest["verdict"] = "ok" if not missing else "incomplete"
    manifest["missing"] = missing
    return manifest


def write_markdown(path: Path, manifest: dict[str, Any]) -> None:
    capture = manifest["capture"]
    lines = [
        "# Metal Capture Manifest",
        "",
        f"- verdict: {manifest['verdict']}",
        f"- game: {manifest.get('game') or 'unknown'}",
        f"- run: `{manifest['run_dir']}`",
        f"- capture: `{capture.get('path')}`",
        f"- capture exists: {capture.get('exists')} ({capture.get('bytes')} bytes, {capture.get('file_count')} files)",
        f"- renderer: {manifest.get('renderer')}",
        f"- surface scale: {manifest.get('surface_scale')}",
        f"- capture frames: {manifest.get('metal_capture_frames')}",
        "",
        "## Open",
        "",
        f"```sh\n{manifest.get('open_in_xcode') or '# no capture path found'}\n```",
        "",
        "## Readiness",
        "",
    ]
    for key, value in manifest["readiness"].items():
        lines.append(f"- {key}: {value}")
    if manifest.get("missing"):
        lines += ["", "## Missing", ""]
        lines.extend(f"- {item}" for item in manifest["missing"])
    lines += ["", "## Capture Log Lines", ""]
    lines.extend(f"- `{line}`" for line in manifest["log_scan"]["capture_lines"][-12:])
    lines += ["", "## Metal Perf Tail", ""]
    perf = manifest["log_scan"]["metal_perf_tail"][-12:]
    lines.extend(f"- `{line}`" for line in perf) if perf else lines.append("- none")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--out", type=Path, help="JSON output path")
    parser.add_argument("--out-md", type=Path, help="Markdown output path")
    args = parser.parse_args()

    run_dir = args.run_dir.resolve()
    out = args.out or (run_dir / "metal-capture-manifest.json")
    out_md = args.out_md or out.with_suffix(".md")
    manifest = make_manifest(run_dir, args.capture)
    out.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_markdown(out_md, manifest)
    print(f"wrote {out}")
    print(f"wrote {out_md}")
    return 0 if manifest["verdict"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
