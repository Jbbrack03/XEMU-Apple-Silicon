#!/usr/bin/env python3
"""Summarize visual behavior from benchmark screenshots or a short video.

The tool is intentionally conservative about artifacts: extracted video frames
live in a TemporaryDirectory by default, while the run directory keeps only a
JSON report, a CSV timeline, a storyboard, and selected keyframes.
"""

import argparse
import csv
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat


IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".bmp"}
PERF_FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")


@dataclass
class FrameMetrics:
    index: int
    path: str
    source_path: str
    width: int
    height: int
    mean_luma: float
    rms_luma: float
    nonblack_pct: float
    nearwhite_pct: float
    colorfulness: float
    entropy: float
    avg_hash: str
    dhash: str
    motion_mae: float
    changed_pct: float
    is_black: bool
    is_static_from_prev: bool


def eprint(*args):
    print(*args, file=sys.stderr)


def parse_crop(value):
    if value is None:
        return None
    parts = value.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("crop must be x,y,width,height")
    try:
        x, y, w, h = [int(part) for part in parts]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("crop values must be integers") from exc
    if w <= 0 or h <= 0:
        raise argparse.ArgumentTypeError("crop width/height must be positive")
    return x, y, w, h


def apply_crop(image, crop):
    if crop is None:
        return image
    x, y, w, h = crop
    x = max(0, min(x, image.width))
    y = max(0, min(y, image.height))
    return image.crop((x, y, min(image.width, x + w), min(image.height, y + h)))


def list_frame_paths(frames_dir, pattern):
    paths = [
        p for p in Path(frames_dir).glob(pattern)
        if p.is_file() and p.suffix.lower() in IMAGE_EXTS
    ]
    return sorted(paths, key=lambda p: str(p))


def extract_video_frames(video_path, out_dir, sample_fps, max_frames):
    out_pattern = out_dir / "frame-%06d.png"
    vf = f"fps={sample_fps}"
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(video_path),
        "-vf", vf,
        "-frames:v", str(max_frames),
        str(out_pattern),
    ]
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"ffmpeg failed while extracting video frames: {exc}") from exc
    return list_frame_paths(out_dir, "*.png")


def bits_to_hex(bits):
    bits = list(bits)
    value = 0
    for bit in bits:
        value = (value << 1) | int(bit)
    width = max(1, math.ceil(len(bits) / 4))
    return f"{value:0{width}x}"


def avg_hash(gray):
    small = gray.resize((8, 8), Image.Resampling.BILINEAR)
    pixels = list(small.tobytes())
    mean = sum(pixels) / len(pixels)
    return bits_to_hex(p >= mean for p in pixels)


def d_hash(gray):
    small = gray.resize((9, 8), Image.Resampling.BILINEAR)
    pixels = list(small.tobytes())
    bits = []
    for y in range(8):
        row = pixels[y * 9:(y + 1) * 9]
        bits.extend(row[x] > row[x + 1] for x in range(8))
    return bits_to_hex(bits)


def hamming_hex(a, b):
    if not a or not b:
        return 64
    return (int(a, 16) ^ int(b, 16)).bit_count()


def image_entropy(gray):
    hist = gray.histogram()
    total = sum(hist)
    if total == 0:
        return 0.0
    entropy = 0.0
    for count in hist:
        if count:
            p = count / total
            entropy -= p * math.log2(p)
    return entropy


def colorfulness_rgb(rgb):
    stat = ImageStat.Stat(rgb)
    if len(stat.stddev) < 3:
        return 0.0
    return float(sum(stat.stddev[:3]) / 3.0)


def motion_metrics(prev_gray, gray):
    if prev_gray is None or prev_gray.size != gray.size:
        return 0.0, 0.0
    diff = ImageChops.difference(prev_gray, gray)
    stat = ImageStat.Stat(diff)
    mean_abs = float(stat.mean[0])
    hist = diff.histogram()
    changed = sum(hist[9:])
    total = gray.width * gray.height
    return mean_abs, (changed / total) * 100.0 if total else 0.0


def compute_metrics(paths, out_keyframe_dir, crop, black_threshold, static_threshold):
    metrics = []
    prev_gray = None
    prev_hash = None
    for idx, path in enumerate(paths):
        with Image.open(path) as raw:
            rgb = apply_crop(raw.convert("RGB"), crop)
            gray = rgb.convert("L")

            stat = ImageStat.Stat(gray)
            hist = gray.histogram()
            total = gray.width * gray.height
            nonblack = sum(hist[9:])
            nearwhite = sum(hist[241:])
            mean_luma = float(stat.mean[0])
            rms_luma = float(stat.rms[0])
            motion_mae, changed_pct = motion_metrics(prev_gray, gray)
            ah = avg_hash(gray)
            dh = d_hash(gray)
            hash_distance = hamming_hex(prev_hash, dh) if prev_hash else 64
            is_black = mean_luma <= black_threshold and (
                (nonblack / total) * 100.0 if total else 0.0
            ) <= 0.5
            is_static = idx > 0 and motion_mae <= static_threshold and hash_distance <= 2

            copied = out_keyframe_dir / f"frame-{idx:04d}{path.suffix.lower()}"
            # Keyframe selection happens later; store original path for now.
            metrics.append(FrameMetrics(
                index=idx,
                path=str(copied),
                source_path=str(path),
                width=gray.width,
                height=gray.height,
                mean_luma=mean_luma,
                rms_luma=rms_luma,
                nonblack_pct=(nonblack / total) * 100.0 if total else 0.0,
                nearwhite_pct=(nearwhite / total) * 100.0 if total else 0.0,
                colorfulness=colorfulness_rgb(rgb),
                entropy=image_entropy(gray),
                avg_hash=ah,
                dhash=dh,
                motion_mae=motion_mae,
                changed_pct=changed_pct,
                is_black=is_black,
                is_static_from_prev=is_static,
            ))
            prev_gray = gray.copy()
            prev_hash = dh
    return metrics


def longest_run(metrics, predicate):
    best_start = -1
    best_len = 0
    cur_start = -1
    cur_len = 0
    for item in metrics:
        if predicate(item):
            if cur_len == 0:
                cur_start = item.index
            cur_len += 1
            if cur_len > best_len:
                best_start, best_len = cur_start, cur_len
        else:
            cur_len = 0
    return {"start_index": best_start, "length": best_len}


def choose_keyframes(metrics, max_keyframes):
    if not metrics:
        return []
    chosen = {0, len(metrics) - 1}

    black_run = longest_run(metrics, lambda m: m.is_black)
    if black_run["length"] > 0:
        chosen.add(black_run["start_index"])
        chosen.add(min(len(metrics) - 1, black_run["start_index"] + black_run["length"] - 1))

    static_run = longest_run(metrics, lambda m: m.is_static_from_prev)
    if static_run["length"] > 2:
        chosen.add(static_run["start_index"])
        chosen.add(min(len(metrics) - 1, static_run["start_index"] + static_run["length"] - 1))

    ranked = []
    for m in metrics:
        score = (
            m.motion_mae * 2.0 +
            m.changed_pct * 0.4 +
            m.entropy * 8.0 +
            m.colorfulness * 0.2 -
            (30.0 if m.is_black else 0.0)
        )
        ranked.append((score, m.index))
    for _, idx in sorted(ranked, reverse=True):
        if len(chosen) >= max_keyframes:
            break
        chosen.add(idx)

    if len(chosen) < max_keyframes:
        stride = max(1, len(metrics) // max_keyframes)
        for idx in range(0, len(metrics), stride):
            if len(chosen) >= max_keyframes:
                break
            chosen.add(idx)

    return sorted(chosen)[:max_keyframes]


def copy_keyframes(metrics, keyframe_indexes, keyframe_dir):
    keyframe_dir.mkdir(parents=True, exist_ok=True)
    selected = set(keyframe_indexes)
    for m in metrics:
        if m.index in selected:
            src = Path(m.source_path)
            dest = keyframe_dir / f"frame-{m.index:04d}{src.suffix.lower()}"
            shutil.copy2(src, dest)
            m.path = str(dest)
        else:
            m.path = ""


def make_storyboard(metrics, keyframe_indexes, out_path, thumb_width):
    if not keyframe_indexes:
        return
    thumbs = []
    font = ImageFont.load_default()
    label_h = 54
    for idx in keyframe_indexes:
        m = metrics[idx]
        with Image.open(m.source_path) as im:
            rgb = im.convert("RGB")
            ratio = thumb_width / rgb.width
            thumb_h = max(1, int(rgb.height * ratio))
            thumb = rgb.resize((thumb_width, thumb_h), Image.Resampling.LANCZOS)
            tile = Image.new("RGB", (thumb_width, thumb_h + label_h), "white")
            tile.paste(thumb, (0, 0))
            draw = ImageDraw.Draw(tile)
            label = (
                f"#{m.index} mean={m.mean_luma:.1f} nonblack={m.nonblack_pct:.1f}%\n"
                f"motion={m.motion_mae:.1f} changed={m.changed_pct:.1f}% "
                f"{'BLACK' if m.is_black else ''}"
            )
            draw.text((6, thumb_h + 5), label, fill=(0, 0, 0), font=font)
            thumbs.append(tile)

    cols = min(3, len(thumbs))
    rows = math.ceil(len(thumbs) / cols)
    tile_w = thumb_width
    tile_h = max(t.height for t in thumbs)
    board = Image.new("RGB", (cols * tile_w, rows * tile_h), "white")
    for i, tile in enumerate(thumbs):
        x = (i % cols) * tile_w
        y = (i // cols) * tile_h
        board.paste(tile, (x, y))
    board.save(out_path, quality=92)


def write_timeline_csv(metrics, path):
    fields = list(asdict(metrics[0]).keys()) if metrics else [
        "index", "path", "source_path", "width", "height", "mean_luma",
        "rms_luma", "nonblack_pct", "nearwhite_pct", "colorfulness",
        "entropy", "avg_hash", "dhash", "motion_mae", "changed_pct",
        "is_black", "is_static_from_prev",
    ]
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for m in metrics:
            writer.writerow(asdict(m))


def parse_perf_log(log_path):
    if not log_path or not Path(log_path).exists():
        return {}
    intervals = []
    draw_targets = []
    screenshot_writes = []
    for line in Path(log_path).read_text(errors="replace").splitlines():
        if "xemu-perf:" not in line:
            continue
        if " interval_ms=" in line:
            fields = {}
            for key, value in PERF_FIELD_RE.findall(line):
                if key == "frame_mspf_us":
                    continue
                try:
                    fields[key] = float(value)
                except ValueError:
                    fields[key] = value
            intervals.append(fields)
        elif "metal_draw_target " in line:
            fields = dict(PERF_FIELD_RE.findall(line))
            draw_targets.append(fields)
        elif "metal_screenshot_written " in line:
            fields = dict(PERF_FIELD_RE.findall(line))
            screenshot_writes.append(fields)

    def avg(key):
        vals = [i[key] for i in intervals if isinstance(i.get(key), (int, float))]
        return sum(vals) / len(vals) if vals else 0.0

    def maxv(key):
        vals = [i[key] for i in intervals if isinstance(i.get(key), (int, float))]
        return max(vals) if vals else 0.0

    interesting = [
        "METAL_PIPELINE_FAILED", "METAL_PIPELINE_TRANSLATED_FAILED",
        "METAL_PIPELINE_FALLBACKS", "METAL_DRAWS_SKIPPED_PENDING_TOTAL",
        "METAL_SCREENSHOTS_TAKEN", "METAL_SURFACE_DOWNLOADS",
        "METAL_FRONT_FB_PUBLISHES", "INPUT_LAT_US_MAX",
    ]
    counters = {
        key: sum(i.get(key, 0.0) for i in intervals if isinstance(i.get(key), (int, float)))
        for key in interesting
    }
    return {
        "interval_count": len(intervals),
        "avg_fps": avg("fps"),
        "max_mspf": maxv("mspf_max"),
        "avg_mspf": avg("mspf_avg"),
        "counters_sum": counters,
        "draw_target_lines": draw_targets[-20:],
        "screenshot_writes": screenshot_writes,
    }


def build_summary(metrics):
    if not metrics:
        return {}
    black_count = sum(1 for m in metrics if m.is_black)
    static_count = sum(1 for m in metrics if m.is_static_from_prev)
    avg_motion = sum(m.motion_mae for m in metrics[1:]) / max(1, len(metrics) - 1)
    avg_changed = sum(m.changed_pct for m in metrics[1:]) / max(1, len(metrics) - 1)
    max_motion = max(metrics, key=lambda m: m.motion_mae)
    max_entropy = max(metrics, key=lambda m: m.entropy)
    max_color = max(metrics, key=lambda m: m.colorfulness)
    return {
        "frame_count": len(metrics),
        "black_frame_count": black_count,
        "black_frame_pct": (black_count / len(metrics)) * 100.0,
        "longest_black_run": longest_run(metrics, lambda m: m.is_black),
        "static_frame_count": static_count,
        "static_frame_pct_after_first": (static_count / max(1, len(metrics) - 1)) * 100.0,
        "longest_static_run": longest_run(metrics, lambda m: m.is_static_from_prev),
        "avg_motion_mae": avg_motion,
        "avg_changed_pct": avg_changed,
        "max_motion_frame": max_motion.index,
        "max_motion_mae": max_motion.motion_mae,
        "max_entropy_frame": max_entropy.index,
        "max_entropy": max_entropy.entropy,
        "max_colorfulness_frame": max_color.index,
        "max_colorfulness": max_color.colorfulness,
        "first_frame": metrics[0].source_path,
        "last_frame": metrics[-1].source_path,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Create a compact visual timeline from xemu benchmark frames/video."
    )
    parser.add_argument("--run-dir", help="Benchmark run directory; default outputs under RUN_DIR/visual-analysis")
    parser.add_argument("--frames-dir", help="Directory of existing screenshots/frames")
    parser.add_argument("--glob", default="*.png", help="Frame glob inside --frames-dir")
    parser.add_argument("--video", help="Video file to sample with ffmpeg")
    parser.add_argument("--out-dir", help="Output directory")
    parser.add_argument("--sample-fps", type=float, default=2.0,
                        help="FPS to sample when --video is used")
    parser.add_argument("--max-video-frames", type=int, default=240)
    parser.add_argument("--max-keyframes", type=int, default=12)
    parser.add_argument("--thumb-width", type=int, default=360)
    parser.add_argument("--crop", type=parse_crop)
    parser.add_argument("--black-threshold", type=float, default=4.0)
    parser.add_argument("--static-threshold", type=float, default=1.0)
    parser.add_argument("--keep-temp", action="store_true",
                        help="Keep extracted video frames for debugging")
    args = parser.parse_args()

    if not args.frames_dir and not args.video:
        if args.run_dir:
            args.frames_dir = str(Path(args.run_dir) / "screenshots")
        else:
            parser.error("provide --frames-dir, --video, or --run-dir with screenshots/")

    run_dir = Path(args.run_dir) if args.run_dir else None
    out_dir = Path(args.out_dir) if args.out_dir else (
        (run_dir / "visual-analysis") if run_dir else Path("visual-analysis")
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    keyframe_dir = out_dir / "keyframes"
    if keyframe_dir.exists():
        shutil.rmtree(keyframe_dir)

    temp_cm = None
    temp_path = None
    try:
        if args.video:
            if shutil.which("ffmpeg") is None:
                raise SystemExit("ffmpeg is required for --video")
            if args.keep_temp:
                temp_path = Path(tempfile.mkdtemp(prefix="xemu-visual-frames-"))
                eprint(f"keeping extracted frames in {temp_path}")
            else:
                temp_cm = tempfile.TemporaryDirectory(prefix="xemu-visual-frames-")
                temp_path = Path(temp_cm.name)
            frame_paths = extract_video_frames(
                Path(args.video), temp_path, args.sample_fps, args.max_video_frames
            )
        else:
            frame_paths = list_frame_paths(args.frames_dir, args.glob)

        if not frame_paths:
            raise SystemExit("no frames found")

        metrics = compute_metrics(
            frame_paths, keyframe_dir, args.crop,
            args.black_threshold, args.static_threshold,
        )
        keyframes = choose_keyframes(metrics, args.max_keyframes)
        copy_keyframes(metrics, keyframes, keyframe_dir)

        storyboard_path = out_dir / "storyboard.jpg"
        timeline_path = out_dir / "timeline.csv"
        report_path = out_dir / "visual-summary.json"

        make_storyboard(metrics, keyframes, storyboard_path, args.thumb_width)
        write_timeline_csv(metrics, timeline_path)

        perf = parse_perf_log((run_dir / "xemu.log") if run_dir else None)
        report = {
            "source": {
                "run_dir": str(run_dir) if run_dir else None,
                "frames_dir": args.frames_dir,
                "video": args.video,
                "frame_count": len(frame_paths),
                "crop": args.crop,
            },
            "artifacts": {
                "report": str(report_path),
                "timeline_csv": str(timeline_path),
                "storyboard": str(storyboard_path),
                "keyframes_dir": str(keyframe_dir),
            },
            "summary": build_summary(metrics),
            "keyframe_indexes": keyframes,
            "keyframes": [asdict(metrics[i]) for i in keyframes],
            "perf": perf,
        }
        report_path.write_text(json.dumps(report, indent=2) + "\n")

        print(f"frames={len(frame_paths)}")
        print(f"black_frame_pct={report['summary'].get('black_frame_pct', 0):.2f}")
        print(f"avg_motion_mae={report['summary'].get('avg_motion_mae', 0):.2f}")
        print(f"storyboard={storyboard_path}")
        print(f"report={report_path}")
        print(f"timeline={timeline_path}")
    finally:
        if temp_cm is not None:
            temp_cm.cleanup()


if __name__ == "__main__":
    main()
