#!/usr/bin/env python3
"""Temporal flicker analyzer for paired renderer frame sequences.

Background: the single-frame canary methodology (MSAA4 screenshots at fixed
flip ordinals like f600, f900, f1200) cannot detect intermittent rendering
defects that last 1-2 frames between sample points — flickering textures,
missing-when-rendered geometry, menu elements that pop in and out. User
report 2026-05-12 documents exactly this class of bug being missed by the
existing PASS canaries.

This tool consumes PNG-every-frame captures (produced by
capture-boot-temporal.sh or equivalent every-frame screenshot env) and
emits:

  - Per-leg temporal metrics: pixel-toggle heat map, flicker rate per
    second, mean adjacent-frame diff, frames with diff above spike
    threshold ("blink events"), longest stable run, color statistics.

  - Paired Metal-vs-GL comparison: side-by-side heat maps, flicker rate
    delta, "instability gap" regions where Metal is materially worse,
    color-fidelity comparison (mean-color drift between renderers).

  - Storyboards and blink reels: contact sheets selected to surface the
    failures rather than mask them.

Usage:
  Single leg:
    temporal-flicker-analyze.py \
        --frames-dir benchmark-runs/<ts>-boot-metal/frames/ \
        --glob 'metal-boot.*.png' \
        --out-dir benchmark-runs/<ts>-boot-metal/flicker/

  Paired:
    temporal-flicker-analyze.py \
        --metal-frames benchmark-runs/<ts>-boot-metal/frames/ \
        --metal-glob 'metal-boot.*.png' \
        --gl-frames benchmark-runs/<ts>-boot-gl/frames/ \
        --gl-glob 'boot-*.png' \
        --out-dir benchmark-runs/<ts>-paired-flicker/

Crop semantics: macOS-window captures include menu bar + xemu title bar.
For paired comparison, normalize by cropping out the chrome. The GL ffmpeg
AVFoundation capture inherits the macOS desktop chrome too; the Metal
renderer-native capture does not (Metal renders straight to a 1280x960
NV2A surface). Asymmetric crops are supported via --metal-crop / --gl-crop.

Output:
  summary.json     Per-leg + paired metrics (machine-readable).
  report.md        Human-readable narrative.
  heatmap-*.png    Per-leg instability heat maps.
  blink-reel/      Per-leg top-N high-diff frames with adjacent context.
  storyboard-*.jpg Selected representative frames per leg.

The tool intentionally avoids the visual-flight-recorder.py keyframe-by-
information-density approach for blink selection because flicker frames
often have LOW visual information (a momentary clear, a flash of a clear
color, etc.). Selection is by adjacent-frame delta, not by content
richness.
"""

import argparse
import json
import math
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image


@dataclass
class FrameMetric:
    index: int
    name: str
    mean_r: float
    mean_g: float
    mean_b: float
    mean_luma: float
    is_solid: bool          # >99% of pixels within 4-LSB of the mean.
    solid_color_hint: str   # e.g. "magenta", "black", "near-uniform"
    prev_diff_mae: float    # Mean absolute error vs previous frame (0-255).
    prev_diff_changed_pct: float  # % of pixels with |delta| > 9.
    prev_diff_max_abs: int  # Max absolute per-channel delta vs previous.
    spike: bool             # changed_pct >= spike_threshold OR prev_diff_mae >= spike_mae.


@dataclass
class LegReport:
    label: str
    frames_dir: str
    glob: str
    crop: Optional[tuple]
    frame_count: int
    width: int
    height: int
    duration_seconds: Optional[float]
    mean_diff_mae: float
    mean_changed_pct: float
    spike_count: int
    spike_frames: list             # list of (index, name, changed_pct, mae)
    longest_stable_run: dict       # {start_index, length}
    solid_frame_count: int         # frames classified as near-uniform
    solid_color_breakdown: dict    # {"magenta": N, "black": N, ...}
    blink_rate_per_sec: float      # spike_count / duration_seconds
    instability_heatmap_pct: float # % of pixels with stddev > 12 across frames
    instability_heatmap_png: str
    storyboard_png: str
    blink_reel_dir: str


# -------------------------------------------------------------------- helpers

IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".bmp"}


def parse_crop(value):
    if value is None:
        return None
    parts = value.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("crop must be x,y,width,height")
    x, y, w, h = [int(p) for p in parts]
    if w <= 0 or h <= 0:
        raise argparse.ArgumentTypeError("crop w/h must be positive")
    return (x, y, w, h)


def open_rgb(path: Path, crop):
    with Image.open(path) as raw:
        rgb = raw.convert("RGB")
        if crop is not None:
            x, y, w, h = crop
            x = max(0, min(x, rgb.width))
            y = max(0, min(y, rgb.height))
            rgb = rgb.crop((x, y, min(rgb.width, x + w), min(rgb.height, y + h)))
        return np.asarray(rgb, dtype=np.uint8)


def classify_solid(arr: np.ndarray):
    """Classify whether the frame is near-uniform and give a color hint.

    Heuristic: >= 99% of pixels are within ±4 of the mean per channel.
    """
    mean = arr.reshape(-1, 3).mean(axis=0)
    diff = np.abs(arr.astype(np.int16) - mean.astype(np.int16))
    within = (diff <= 4).all(axis=-1)
    pct_uniform = float(within.mean()) * 100.0
    is_solid = pct_uniform >= 99.0
    r, g, b = mean
    hint = "non-uniform"
    if is_solid:
        if r < 16 and g < 16 and b < 16:
            hint = "black"
        elif r > 240 and g > 240 and b > 240:
            hint = "white"
        elif r > 240 and g < 30 and b > 240:
            hint = "magenta"
        elif g > 240 and r < 30 and b < 30:
            hint = "green"
        elif r > 240 and g < 30 and b < 30:
            hint = "red"
        elif b > 240 and r < 30 and g < 30:
            hint = "blue"
        else:
            hint = "near-uniform"
    return is_solid, hint, (float(r), float(g), float(b))


def diff_metrics(prev: np.ndarray, cur: np.ndarray):
    """Per-channel absolute delta against previous frame."""
    if prev is None or prev.shape != cur.shape:
        return 0.0, 0.0, 0
    d = np.abs(cur.astype(np.int16) - prev.astype(np.int16))
    mae = float(d.mean())
    changed = float((d.max(axis=-1) > 9).mean()) * 100.0
    max_abs = int(d.max())
    return mae, changed, max_abs


def collect_frames(frames_dir: Path, glob_pattern: str):
    paths = [p for p in frames_dir.glob(glob_pattern)
             if p.is_file() and p.suffix.lower() in IMAGE_EXTS]
    return sorted(paths, key=lambda p: str(p))


def longest_run(predicates):
    best_start = -1
    best_len = 0
    cur_start = -1
    cur_len = 0
    for i, ok in enumerate(predicates):
        if ok:
            if cur_len == 0:
                cur_start = i
            cur_len += 1
            if cur_len > best_len:
                best_start, best_len = cur_start, cur_len
        else:
            cur_len = 0
    return {"start_index": best_start, "length": best_len}


# -------------------------------------------------------------------- leg core

def analyze_leg(
    label: str,
    frames_dir: Path,
    glob_pattern: str,
    crop,
    out_dir: Path,
    duration_seconds: Optional[float],
    spike_changed_threshold: float = 35.0,
    spike_mae_threshold: float = 20.0,
    max_blink_reel: int = 16,
    stride: int = 1,
):
    paths = collect_frames(frames_dir, glob_pattern)
    if not paths:
        raise SystemExit(f"no frames matched {frames_dir}/{glob_pattern}")

    # First, prep an instability accumulator at the resolution of the first
    # frame (post-crop). Per-pixel sum / sum_of_squares / count to derive
    # per-pixel stddev across the entire sequence.
    first = open_rgb(paths[0], crop)
    h, w, _ = first.shape
    sum_acc = np.zeros((h, w, 3), dtype=np.float64)
    sumsq_acc = np.zeros((h, w, 3), dtype=np.float64)
    n_acc = 0

    metrics = []
    prev = None
    solid_colors = {}
    for idx, p in enumerate(paths):
        if stride > 1 and idx % stride != 0 and idx != len(paths) - 1:
            continue
        cur = open_rgb(p, crop)
        if cur.shape != first.shape:
            # Resize to match for accumulator (PIL nearest).
            with Image.open(p) as im:
                rgb = im.convert("RGB").resize((w, h), Image.Resampling.NEAREST)
                if crop is not None:
                    rgb = rgb.crop((crop[0], crop[1],
                                    min(rgb.width, crop[0] + crop[2]),
                                    min(rgb.height, crop[1] + crop[3])))
                cur = np.asarray(rgb, dtype=np.uint8)

        sum_acc += cur.astype(np.float64)
        sumsq_acc += cur.astype(np.float64) ** 2
        n_acc += 1

        is_solid, hint, (mr, mg, mb) = classify_solid(cur)
        if is_solid:
            solid_colors[hint] = solid_colors.get(hint, 0) + 1
        mae, changed, max_abs = diff_metrics(prev, cur)
        spike = (changed >= spike_changed_threshold) or (mae >= spike_mae_threshold)
        metrics.append(FrameMetric(
            index=idx,
            name=p.name,
            mean_r=float(mr),
            mean_g=float(mg),
            mean_b=float(mb),
            mean_luma=float(0.299 * mr + 0.587 * mg + 0.114 * mb),
            is_solid=is_solid,
            solid_color_hint=hint,
            prev_diff_mae=float(mae),
            prev_diff_changed_pct=float(changed),
            prev_diff_max_abs=int(max_abs),
            spike=bool(spike),
        ))
        prev = cur

    # Per-pixel stddev across frames -> instability heat map.
    if n_acc > 0:
        mean = sum_acc / n_acc
        var = np.clip(sumsq_acc / n_acc - mean ** 2, 0, None)
        stddev = np.sqrt(var)
    else:
        stddev = np.zeros_like(sum_acc)
    stddev_l = stddev.mean(axis=-1)  # 1-channel "instability" map
    unstable_pct = float((stddev_l > 12.0).mean()) * 100.0

    # Render heat map.
    out_dir.mkdir(parents=True, exist_ok=True)
    heat = np.clip(stddev_l * 4.0, 0, 255).astype(np.uint8)
    heat_rgb = np.stack([heat, heat // 4, np.zeros_like(heat)], axis=-1)
    heat_img = Image.fromarray(heat_rgb, mode="RGB")
    heat_path = out_dir / f"heatmap-{label}.png"
    heat_img.save(heat_path, optimize=True)

    # Blink reel: top N spike frames by changed_pct.
    spikes = sorted([m for m in metrics if m.spike],
                    key=lambda m: m.prev_diff_changed_pct, reverse=True)
    blink_dir = out_dir / f"blink-reel-{label}"
    blink_dir.mkdir(parents=True, exist_ok=True)
    for m in spikes[:max_blink_reel]:
        src = paths[m.index]
        dst = blink_dir / src.name
        if not dst.exists():
            try:
                dst.write_bytes(src.read_bytes())
            except Exception:
                pass

    # Storyboard: 8 frames spread evenly across the run, plus the worst 2
    # spike frames. JPEG mosaic.
    chosen = set()
    if len(paths) > 0:
        stride_sb = max(1, len(paths) // 8)
        for i in range(0, len(paths), stride_sb):
            chosen.add(i)
    for m in spikes[:2]:
        chosen.add(m.index)
    chosen = sorted(chosen)[:10]

    tile_w = 240
    thumbs = []
    for i in chosen:
        with Image.open(paths[i]) as im:
            rgb = im.convert("RGB")
            ratio = tile_w / rgb.width
            tile_h = max(1, int(rgb.height * ratio))
            thumbs.append(rgb.resize((tile_w, tile_h), Image.Resampling.LANCZOS))
    if thumbs:
        sb_h = max(t.height for t in thumbs)
        sb = Image.new("RGB", (tile_w * len(thumbs), sb_h), (32, 32, 32))
        for k, t in enumerate(thumbs):
            sb.paste(t, (k * tile_w, 0))
        sb_path = out_dir / f"storyboard-{label}.jpg"
        sb.save(sb_path, quality=88)
    else:
        sb_path = out_dir / f"storyboard-{label}.jpg"

    # Aggregate counters.
    mean_mae = float(np.mean([m.prev_diff_mae for m in metrics])) if metrics else 0.0
    mean_changed = float(np.mean([m.prev_diff_changed_pct for m in metrics])) if metrics else 0.0
    spike_count = sum(1 for m in metrics if m.spike)
    solid_count = sum(1 for m in metrics if m.is_solid)
    longest = longest_run([m.is_solid for m in metrics])
    blink_rate = (spike_count / duration_seconds) if duration_seconds and duration_seconds > 0 else 0.0

    report = LegReport(
        label=label,
        frames_dir=str(frames_dir),
        glob=glob_pattern,
        crop=crop,
        frame_count=len(paths),
        width=w,
        height=h,
        duration_seconds=duration_seconds,
        mean_diff_mae=mean_mae,
        mean_changed_pct=mean_changed,
        spike_count=spike_count,
        spike_frames=[(m.index, m.name, m.prev_diff_changed_pct, m.prev_diff_mae)
                      for m in spikes[:max_blink_reel]],
        longest_stable_run=longest,
        solid_frame_count=solid_count,
        solid_color_breakdown=solid_colors,
        blink_rate_per_sec=blink_rate,
        instability_heatmap_pct=unstable_pct,
        instability_heatmap_png=str(heat_path),
        storyboard_png=str(sb_path),
        blink_reel_dir=str(blink_dir),
    )
    return report, metrics


# -------------------------------------------------------------------- main

def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    p.add_argument("--frames-dir", type=Path)
    p.add_argument("--glob", default="*.png")
    p.add_argument("--metal-frames", type=Path)
    p.add_argument("--metal-glob", default="metal-boot.*.png")
    p.add_argument("--gl-frames", type=Path)
    p.add_argument("--gl-glob", default="boot-*.png")
    p.add_argument("--out-dir", type=Path, required=True)
    p.add_argument("--crop", type=parse_crop, default=None,
                   help="x,y,w,h crop applied to both legs (single-leg path uses this too)")
    p.add_argument("--metal-crop", type=parse_crop, default=None)
    p.add_argument("--gl-crop", type=parse_crop, default=None)
    p.add_argument("--duration-seconds", type=float, default=None,
                   help="Total wall duration of capture; used to compute blink rate.")
    p.add_argument("--spike-changed-pct", type=float, default=35.0,
                   help="Spike threshold for prev-frame changed_pct (default 35).")
    p.add_argument("--spike-mae", type=float, default=20.0,
                   help="Spike threshold for mean abs error (default 20).")
    p.add_argument("--max-blink-reel", type=int, default=16,
                   help="Top-N spike frames kept in the blink reel (default 16).")
    p.add_argument("--stride", type=int, default=1,
                   help="Sample every Nth frame to speed up runs (default 1).")
    args = p.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)

    paired = (args.metal_frames is not None) and (args.gl_frames is not None)
    if not paired and args.frames_dir is None:
        raise SystemExit(
            "Either --frames-dir (single leg) or --metal-frames + --gl-frames (paired) required."
        )

    summary = {
        "paired": paired,
        "spike_changed_pct_threshold": args.spike_changed_pct,
        "spike_mae_threshold": args.spike_mae,
        "stride": args.stride,
    }

    if paired:
        metal_crop = args.metal_crop or args.crop
        gl_crop = args.gl_crop or args.crop
        metal_report, _ = analyze_leg(
            "metal",
            args.metal_frames,
            args.metal_glob,
            metal_crop,
            args.out_dir,
            args.duration_seconds,
            args.spike_changed_pct,
            args.spike_mae,
            args.max_blink_reel,
            args.stride,
        )
        gl_report, _ = analyze_leg(
            "gl",
            args.gl_frames,
            args.gl_glob,
            gl_crop,
            args.out_dir,
            args.duration_seconds,
            args.spike_changed_pct,
            args.spike_mae,
            args.max_blink_reel,
            args.stride,
        )
        summary["metal"] = asdict(metal_report)
        summary["gl"] = asdict(gl_report)
        # Headline comparative numbers.
        summary["delta"] = {
            "mean_changed_pct_metal_minus_gl":
                metal_report.mean_changed_pct - gl_report.mean_changed_pct,
            "blink_rate_metal_minus_gl":
                metal_report.blink_rate_per_sec - gl_report.blink_rate_per_sec,
            "solid_frame_count_metal_minus_gl":
                metal_report.solid_frame_count - gl_report.solid_frame_count,
            "instability_heatmap_pct_metal_minus_gl":
                metal_report.instability_heatmap_pct - gl_report.instability_heatmap_pct,
        }
    else:
        only_report, _ = analyze_leg(
            "leg",
            args.frames_dir,
            args.glob,
            args.crop,
            args.out_dir,
            args.duration_seconds,
            args.spike_changed_pct,
            args.spike_mae,
            args.max_blink_reel,
            args.stride,
        )
        summary["leg"] = asdict(only_report)

    out_json = args.out_dir / "summary.json"
    with open(out_json, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True, default=str)

    # Brief human-readable report.
    md_path = args.out_dir / "report.md"
    lines = []
    lines.append("# Temporal flicker analysis\n")
    lines.append(f"Generated: out_dir={args.out_dir}")
    lines.append(f"Spike thresholds: changed_pct={args.spike_changed_pct} mae={args.spike_mae}\n")
    if paired:
        m = summary["metal"]
        g = summary["gl"]
        lines.append("## Per-leg headline")
        lines.append(f"| Metric | Metal | GL | delta (Metal - GL) |")
        lines.append(f"|---|---:|---:|---:|")
        lines.append(f"| frames | {m['frame_count']} | {g['frame_count']} | — |")
        lines.append(f"| mean adj-frame changed_pct | {m['mean_changed_pct']:.2f} | {g['mean_changed_pct']:.2f} | {m['mean_changed_pct']-g['mean_changed_pct']:+.2f} |")
        lines.append(f"| mean adj-frame MAE | {m['mean_diff_mae']:.2f} | {g['mean_diff_mae']:.2f} | {m['mean_diff_mae']-g['mean_diff_mae']:+.2f} |")
        lines.append(f"| blink_rate/sec | {m['blink_rate_per_sec']:.2f} | {g['blink_rate_per_sec']:.2f} | {m['blink_rate_per_sec']-g['blink_rate_per_sec']:+.2f} |")
        lines.append(f"| solid_frame_count | {m['solid_frame_count']} | {g['solid_frame_count']} | {m['solid_frame_count']-g['solid_frame_count']:+d} |")
        lines.append(f"| longest stable run | {m['longest_stable_run']['length']} | {g['longest_stable_run']['length']} | — |")
        lines.append(f"| instability_pct (px stddev>12) | {m['instability_heatmap_pct']:.2f} | {g['instability_heatmap_pct']:.2f} | {m['instability_heatmap_pct']-g['instability_heatmap_pct']:+.2f} |")
        lines.append("\n### Solid color breakdown")
        lines.append(f"- metal: {m['solid_color_breakdown']}")
        lines.append(f"- gl:    {g['solid_color_breakdown']}")
        lines.append("\n### Artifacts")
        lines.append(f"- metal heatmap: {Path(m['instability_heatmap_png']).name}")
        lines.append(f"- gl heatmap:    {Path(g['instability_heatmap_png']).name}")
        lines.append(f"- metal storyboard: {Path(m['storyboard_png']).name}")
        lines.append(f"- gl storyboard:    {Path(g['storyboard_png']).name}")
        lines.append(f"- metal blink reel: {Path(m['blink_reel_dir']).name}/")
        lines.append(f"- gl blink reel:    {Path(g['blink_reel_dir']).name}/")
    else:
        leg = summary["leg"]
        lines.append("## Single-leg headline")
        lines.append(f"- frame_count: {leg['frame_count']}")
        lines.append(f"- mean adj-frame changed_pct: {leg['mean_changed_pct']:.2f}")
        lines.append(f"- mean adj-frame MAE: {leg['mean_diff_mae']:.2f}")
        lines.append(f"- spike_count: {leg['spike_count']}")
        lines.append(f"- blink_rate/sec: {leg['blink_rate_per_sec']:.2f}")
        lines.append(f"- solid_frame_count: {leg['solid_frame_count']}")
        lines.append(f"- instability_pct (px stddev>12): {leg['instability_heatmap_pct']:.2f}")
        lines.append(f"- solid color breakdown: {leg['solid_color_breakdown']}")
        lines.append(f"- artifacts: {Path(leg['instability_heatmap_png']).name}, "
                     f"{Path(leg['storyboard_png']).name}, "
                     f"{Path(leg['blink_reel_dir']).name}/")
    md_path.write_text("\n".join(lines) + "\n")

    print(f"summary: {out_json}")
    print(f"report:  {md_path}")
    print(json.dumps(summary, indent=2, sort_keys=True, default=str)[:1500])


if __name__ == "__main__":
    main()
