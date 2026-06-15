#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageStat
except Exception as exc:  # noqa: BLE001
    print(json.dumps({"error": f"PIL unavailable: {exc}"}))
    sys.exit(2)

IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".bmp"}
DEFAULT_MAX_MEAN_ABS_ERROR = 12.0
DEFAULT_MAX_CHANGED_PIXELS_PCT = 5.0
DEFAULT_CHANGED_PIXEL_THRESHOLD = 8


def list_frames(frames_dir: Path, pattern: str) -> list[Path]:
    frames = [
        p for p in frames_dir.glob(pattern)
        if p.is_file() and p.suffix.lower() in IMAGE_EXTS
    ]
    return sorted(frames, key=lambda p: p.name)


def run_oracle(oracle_script: Path, frames: list[Path]) -> tuple[int, list[dict], str]:
    cmd = [sys.executable, str(oracle_script), "--json", *[str(f) for f in frames]]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    stdout = proc.stdout.strip()
    try:
        data = json.loads(stdout) if stdout else []
    except json.JSONDecodeError as exc:
        raise SystemExit(f"oracle JSON parse failed: {exc}\nstdout={stdout}\nstderr={proc.stderr}") from exc
    if not isinstance(data, list):
        raise SystemExit(f"oracle output was not a JSON list: {data!r}")
    return proc.returncode, data, proc.stderr.strip()


def match_golden(frame: Path, golden_path: Path | None) -> Path | None:
    if golden_path is None:
        return None
    if golden_path.is_file():
        return golden_path
    candidate = golden_path / frame.name
    return candidate if candidate.exists() else None


def compare_images(frame_path: Path, golden_path: Path, threshold: int) -> dict:
    frame = Image.open(frame_path).convert("RGB")
    golden = Image.open(golden_path).convert("RGB")
    result = {
        "golden": str(golden_path),
        "size_match": frame.size == golden.size,
        "frame_size": [frame.size[0], frame.size[1]],
        "golden_size": [golden.size[0], golden.size[1]],
    }
    if frame.size != golden.size:
        result.update({
            "passed": False,
            "reasons": [f"size mismatch frame={frame.size} golden={golden.size}"],
        })
        return result

    diff = ImageChops.difference(frame, golden)
    stat = ImageStat.Stat(diff)
    channels = len(stat.mean)
    pixels = frame.size[0] * frame.size[1]
    mae = sum(stat.mean) / channels
    rms = math.sqrt(sum(value * value for value in stat.rms) / channels)
    red, green, blue = diff.split()
    max_channel = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    histogram = max_channel.histogram()
    changed = sum(histogram[threshold + 1:])
    changed_pct = (changed / pixels) * 100.0 if pixels else 0.0
    reasons = []
    if mae > DEFAULT_MAX_MEAN_ABS_ERROR:
        reasons.append(
            f"golden mean_abs_error {mae:.4f} > {DEFAULT_MAX_MEAN_ABS_ERROR:.4f}"
        )
    if changed_pct > DEFAULT_MAX_CHANGED_PIXELS_PCT:
        reasons.append(
            f"golden changed_pixels_pct {changed_pct:.4f} > {DEFAULT_MAX_CHANGED_PIXELS_PCT:.4f}"
        )
    result.update({
        "mean_abs_error": round(mae, 4),
        "rms_error": round(rms, 4),
        "changed_pixels": changed,
        "changed_pixels_pct": round(changed_pct, 4),
        "changed_pixel_threshold": threshold,
        "max_mean_abs_error": DEFAULT_MAX_MEAN_ABS_ERROR,
        "max_changed_pixels_pct": DEFAULT_MAX_CHANGED_PIXELS_PCT,
        "passed": not reasons,
        "reasons": reasons,
    })
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Mandatory visual-validation gate")
    parser.add_argument("--frames-dir", required=True)
    parser.add_argument("--glob", default="*.png")
    parser.add_argument("--golden")
    parser.add_argument("--out")
    parser.add_argument("--changed-pixel-threshold", type=int, default=DEFAULT_CHANGED_PIXEL_THRESHOLD)
    parser.add_argument("--oracle-script")
    args = parser.parse_args()

    frames_dir = Path(args.frames_dir)
    if not frames_dir.is_dir():
        raise SystemExit(f"frames dir missing: {frames_dir}")
    frames = list_frames(frames_dir, args.glob)
    if not frames:
        raise SystemExit(f"no frames found in {frames_dir} matching {args.glob}")

    golden_path = Path(args.golden) if args.golden else None
    oracle_script = Path(args.oracle_script) if args.oracle_script else Path(__file__).with_name("xemu_frame_correctness_oracle.py")
    if not oracle_script.exists():
        raise SystemExit(f"oracle script missing: {oracle_script}")

    oracle_rc, oracle_rows, oracle_stderr = run_oracle(oracle_script, frames)
    by_path = {str(Path(row.get("path", "")).resolve()): row for row in oracle_rows if isinstance(row, dict)}

    results = []
    any_fail = oracle_rc != 0
    for frame in frames:
        oracle_row = by_path.get(str(frame.resolve()), {"path": str(frame), "error": "oracle result missing", "passed": False})
        oracle_passed = bool(oracle_row.get("passed", False)) and "error" not in oracle_row
        combined_reasons = list(oracle_row.get("reasons", []))
        metric = None
        matched_golden = match_golden(frame, golden_path)
        if matched_golden is not None:
            metric = compare_images(frame, matched_golden, args.changed_pixel_threshold)
            if not metric.get("passed", False):
                combined_reasons.extend(metric.get("reasons", []))
        combined_passed = oracle_passed and (metric is None or metric.get("passed", False))
        if not combined_passed:
            any_fail = True
        results.append({
            "frame": str(frame),
            "passed": combined_passed,
            "oracle": oracle_row,
            "golden_metric": metric,
            "reasons": combined_reasons,
        })

    summary = {
        "frames_dir": str(frames_dir),
        "glob": args.glob,
        "golden": str(golden_path) if golden_path else None,
        "oracle_script": str(oracle_script),
        "frame_count": len(results),
        "pass_count": sum(1 for row in results if row["passed"]),
        "fail_count": sum(1 for row in results if not row["passed"]),
        "oracle_rc": oracle_rc,
        "oracle_stderr": oracle_stderr,
        "verdict": "PASS" if not any_fail else "FAIL",
        "results": results,
    }

    payload = json.dumps(summary, indent=2)
    if args.out:
        out_path = Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(payload + "\n")

    print(f"visual_validation_verdict={summary['verdict']}")
    print(f"frames={summary['frame_count']}")
    print(f"pass_count={summary['pass_count']}")
    print(f"fail_count={summary['fail_count']}")
    if golden_path:
        print(f"golden={golden_path}")
    if oracle_stderr:
        print(f"oracle_stderr={oracle_stderr}")
    for row in results:
        tag = "PASS" if row["passed"] else "FAIL"
        print(f"{tag} {os.path.basename(row['frame'])}")
        for reason in row["reasons"]:
            print(f"  - {reason}")
        metric = row.get("golden_metric")
        if metric:
            print(
                "  - golden metric: "
                f"mae={metric['mean_abs_error']:.4f} changed_pct={metric['changed_pixels_pct']:.4f}"
            )

    print(payload)
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
