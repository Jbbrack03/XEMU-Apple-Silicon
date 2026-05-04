#!/usr/bin/env python3
import argparse
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageEnhance, ImageStat


def parse_crop(value):
    parts = value.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("crop must be x,y,width,height")
    try:
        x, y, width, height = [int(part) for part in parts]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("crop values must be integers") from exc
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("crop width and height must be positive")
    return x, y, width, height


def crop_image(image, crop):
    x, y, width, height = crop
    return image.crop((x, y, x + width, y + height))


def main():
    parser = argparse.ArgumentParser(
        description="Compare two benchmark screenshots over a fixed crop"
    )
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--crop", required=True, type=parse_crop,
                        help="x,y,width,height crop rectangle")
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--diff-scale", type=float, default=8.0)
    parser.add_argument("--threshold", type=int, default=8,
                        help="per-channel absolute difference threshold")
    parser.add_argument("--resize", choices=("none", "smaller"),
                        default="none",
                        help=("size-mismatch policy. 'none' (default): exit "
                              "with an error when baseline.size != "
                              "candidate.size. 'smaller': resize the larger "
                              "image down to the smaller's dimensions via "
                              "LANCZOS before crop/diff. The crop rectangle "
                              "is interpreted against the (possibly resized) "
                              "common dimensions."))
    args = parser.parse_args()

    baseline_path = Path(args.baseline)
    candidate_path = Path(args.candidate)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    baseline = Image.open(baseline_path).convert("RGB")
    candidate = Image.open(candidate_path).convert("RGB")

    # W6 (2026-05-04): size-mismatch handling. The full-desktop GL
    # capture vs the drawable-only Metal capture in W2's
    # metal-gl-compare.sh reliably differ in resolution; the prior
    # always-error policy made every paired run an INFRA-FAIL.
    # `--resize smaller` resizes the larger image down to the smaller
    # so the diff is computed over a real common region; the original
    # raw sizes are echoed so the caller can record what was
    # normalized.
    raw_baseline_size = baseline.size
    raw_candidate_size = candidate.size
    resized = "no"
    if baseline.size != candidate.size:
        if args.resize == "none":
            raise SystemExit(
                f"image sizes differ: {baseline.size} vs {candidate.size}"
            )
        # args.resize == "smaller"
        target_w = min(baseline.size[0], candidate.size[0])
        target_h = min(baseline.size[1], candidate.size[1])
        if target_w <= 0 or target_h <= 0:
            raise SystemExit(
                f"resize-to-smaller produced a non-positive target size: "
                f"baseline={baseline.size} candidate={candidate.size}"
            )
        target_size = (target_w, target_h)
        if baseline.size != target_size:
            baseline = baseline.resize(target_size, Image.LANCZOS)
        if candidate.size != target_size:
            candidate = candidate.resize(target_size, Image.LANCZOS)
        resized = "smaller"

    baseline_crop = crop_image(baseline, args.crop)
    candidate_crop = crop_image(candidate, args.crop)
    diff = ImageChops.difference(baseline_crop, candidate_crop)
    stat = ImageStat.Stat(diff)

    channels = len(stat.mean)
    pixels = baseline_crop.size[0] * baseline_crop.size[1]
    mae = sum(stat.mean) / channels
    rms = math.sqrt(sum(value * value for value in stat.rms) / channels)
    max_abs = max(channel_max[1] for channel_max in stat.extrema)

    threshold = args.threshold
    red, green, blue = diff.split()
    max_channel = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    histogram = max_channel.histogram()
    changed = sum(histogram[threshold + 1:])

    baseline_out = out_dir / "baseline-crop.png"
    candidate_out = out_dir / "candidate-crop.png"
    diff_out = out_dir / "diff-amplified.png"

    baseline_crop.save(baseline_out)
    candidate_crop.save(candidate_out)
    ImageEnhance.Brightness(diff).enhance(args.diff_scale).save(diff_out)

    print(f"baseline={baseline_path}")
    print(f"candidate={candidate_path}")
    print(f"raw_baseline_size={raw_baseline_size[0]}x{raw_baseline_size[1]}")
    print(f"raw_candidate_size={raw_candidate_size[0]}x{raw_candidate_size[1]}")
    print(f"resized={resized}")
    print(f"crop={args.crop[0]},{args.crop[1]},{args.crop[2]},{args.crop[3]}")
    print(f"pixels={pixels}")
    print(f"mean_abs_error={mae:.4f}")
    print(f"rms_error={rms:.4f}")
    print(f"max_abs_error={max_abs}")
    print(f"changed_pixels_threshold={threshold}")
    print(f"changed_pixels={changed}")
    print(f"changed_pixels_pct={(changed / pixels) * 100:.4f}")
    print(f"baseline_crop={baseline_out}")
    print(f"candidate_crop={candidate_out}")
    print(f"diff_amplified={diff_out}")


if __name__ == "__main__":
    main()
