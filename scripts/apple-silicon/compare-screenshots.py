#!/usr/bin/env python3
import argparse
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageEnhance, ImageFilter, ImageStat


# --- content classification (2026-06-03) -------------------------------------
# Grounded renderer-gap detection. A near-uniform "black" candidate frame is
# only a Metal renderer failure when the trusted reference (GL) shows real
# geometry at the SAME guest moment. An absolute black threshold cannot tell a
# renderer gap from a legitimate fade-to-black, so the verdict is DIFFERENTIAL
# (reference vs candidate) and is only authoritative when the pair is
# state-aligned (same savevm tag + matched flip ordinal). Thresholds calibrated
# on real Halo captures: GL gameplay frame distinct=84986 dom=0.029 edge=0.063;
# Metal black frame distinct=529 dom=0.968 edge=0.014 dom_luma=0.
EDGE_PIXEL_THRESHOLD = 24          # matches xemu_frame_correctness_oracle.py
CONTENT_EDGE_MIN = 0.02            # >= this edge fraction = structured geometry
CONTENT_MIN_COLORS = 2000          # >= this palette = a real rendered scene
CONTENT_DOM_MAX = 0.90             # <= this single-color share = not uniform
BLACK_DOM_MIN = 0.90               # >= this single-color share = uniform fill
BLACK_EDGE_MAX = 0.02              # <= this edge fraction = no structure
BLACK_LUMA_MAX = 32.0              # dominant color this dark = "black" (keeps a
                                   # bright uniform sentinel, e.g. magenta
                                   # missing-drawable, out of EXPECTED_BLACK)


def content_metrics(image):
    """Per-frame content metrics on an RGB image (the compared crop)."""
    width, height = image.size
    total = max(1, width * height)
    colors = image.getcolors(maxcolors=300000)
    if colors is None:                       # palette richer than the cap
        distinct, dom_frac, dom_luma = 300000, 0.0, 0.0
    else:
        distinct = len(colors)
        count, dom_color = max(colors, key=lambda c: c[0])
        dom_frac = count / total
        dom_luma = (0.2126 * dom_color[0] + 0.7152 * dom_color[1]
                    + 0.0722 * dom_color[2])
    edges = image.convert("L").filter(ImageFilter.FIND_EDGES).histogram()
    edge_frac = sum(edges[EDGE_PIXEL_THRESHOLD:]) / total
    return {"distinct_colors": distinct, "dominant_fraction": dom_frac,
            "edge_fraction": edge_frac, "dominant_luma": dom_luma}


def frame_state(m):
    """Classify one frame as 'content', 'black', or 'ambiguous'."""
    if (m["edge_fraction"] >= CONTENT_EDGE_MIN
            and m["distinct_colors"] >= CONTENT_MIN_COLORS
            and m["dominant_fraction"] <= CONTENT_DOM_MAX):
        return "content"
    if (m["dominant_fraction"] >= BLACK_DOM_MIN
            and m["edge_fraction"] <= BLACK_EDGE_MAX
            and m["dominant_luma"] <= BLACK_LUMA_MAX):
        return "black"
    return "ambiguous"


def classify_pair(ref_state, cand_state, state_aligned):
    """Differential verdict. ref = trusted renderer (GL); cand = candidate
    (Metal). Returns (content_class, is_failure, reason).

    A METAL_GEOMETRY_GAP is only asserted as an authoritative failure when the
    pair is state-aligned; otherwise it is downgraded to *_UNVERIFIED so that
    temporal drift between two unsynchronised legs cannot manufacture a false
    renderer-bug verdict (the pixel-diff threshold still governs that frame).
    """
    if ref_state == "content" and cand_state == "content":
        return ("CONTENT_BOTH", False,
                "both renderers drew geometry; pixel diff governs")
    if ref_state == "black" and cand_state == "black":
        return ("EXPECTED_BLACK", False,
                "both renderers agree the scene is black (e.g. fade/load)")
    if ref_state == "content" and cand_state == "black":
        if state_aligned == "yes":
            return ("METAL_GEOMETRY_GAP", True,
                    "reference has geometry, candidate frame is black: "
                    "renderer cannot draw this scene")
        return ("METAL_GEOMETRY_GAP_UNVERIFIED", False,
                "reference has geometry, candidate frame is black, but the "
                "pair is not state-aligned; re-run with --snapshot to "
                "distinguish a renderer gap from temporal drift")
    if ref_state == "black" and cand_state == "content":
        return ("METAL_SPURIOUS", True,
                "candidate drew geometry the reference does not have")
    return ("AMBIGUOUS", False,
            "indeterminate content state; pixel diff governs")


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
    parser.add_argument("--state-aligned", choices=("yes", "no", "unknown"),
                        default="unknown",
                        help=("whether the baseline (reference) and candidate "
                              "frames are the same guest moment (same savevm "
                              "tag + matched flip ordinal). A renderer-gap "
                              "verdict is only authoritative when 'yes'; "
                              "otherwise it is reported as *_UNVERIFIED."))
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

    # Differential content classification on the compared crops. baseline is
    # the trusted reference (GL); candidate is the renderer under test (Metal).
    ref_cm = content_metrics(baseline_crop)
    cand_cm = content_metrics(candidate_crop)
    ref_state = frame_state(ref_cm)
    cand_state = frame_state(cand_cm)
    content_class, content_failure, content_reason = classify_pair(
        ref_state, cand_state, args.state_aligned)

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
    # Differential content classification (2026-06-03).
    print(f"baseline_distinct_colors={ref_cm['distinct_colors']}")
    print(f"baseline_dominant_fraction={ref_cm['dominant_fraction']:.5f}")
    print(f"baseline_edge_fraction={ref_cm['edge_fraction']:.6f}")
    print(f"baseline_state={ref_state}")
    print(f"candidate_distinct_colors={cand_cm['distinct_colors']}")
    print(f"candidate_dominant_fraction={cand_cm['dominant_fraction']:.5f}")
    print(f"candidate_edge_fraction={cand_cm['edge_fraction']:.6f}")
    print(f"candidate_state={cand_state}")
    print(f"content_alignment={args.state_aligned}")
    print(f"content_class={content_class}")
    print(f"content_failure={'yes' if content_failure else 'no'}")
    print(f"content_reason={content_reason}")


if __name__ == "__main__":
    main()
