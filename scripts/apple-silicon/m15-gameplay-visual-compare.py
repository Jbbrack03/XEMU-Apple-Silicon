#!/usr/bin/env python3
"""Build M15 gameplay visual evidence from GL, Metal, and optional oracle frames.

The existing metal-gl-compare.sh harness proves that one requested capture
point can be diffed. M15 needs a stricter artifact: multiple actual gameplay
keyframes, selected from sequences, aligned by visual content instead of raw
ordinal, with non-gameplay frames rejected up front.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFont, ImageStat


IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".bmp"}
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
BENCHMARK_ROOT = REPO_ROOT / "benchmark-runs"


@dataclass
class FrameFeature:
    index: int
    path: str
    width: int
    height: int
    mean_luma: float
    nonblack_pct: float
    entropy: float
    colorfulness: float
    motion_mae: float
    changed_pct_from_prev: float
    dhash: str
    hist: list[float]
    is_black: bool
    is_static_from_prev: bool
    score: float


@dataclass
class PairResult:
    index: int
    gl_index: int
    metal_index: int
    oracle_index: int | None
    alignment_distance: float
    oracle_alignment_distance: float | None
    changed_pct: float
    mean_abs_error: float
    rms_error: float
    max_abs_error: int
    gl_png: str
    metal_png: str
    oracle_png: str | None
    triptych_png: str
    diff_png: str


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def parse_crop(value: str | None) -> tuple[int, int, int, int] | None:
    if not value:
        return None
    parts = value.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("crop must be x,y,width,height")
    try:
        x, y, w, h = [int(p) for p in parts]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("crop values must be integers") from exc
    if w <= 0 or h <= 0:
        raise argparse.ArgumentTypeError("crop width/height must be positive")
    return x, y, w, h


def parse_int_set(value: str | None) -> set[int]:
    if not value:
        return set()
    out: set[int] = set()
    for raw in value.split(","):
        raw = raw.strip()
        if not raw:
            continue
        try:
            out.add(int(raw))
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                f"ignored frame indexes must be comma-separated integers: {value}"
            ) from exc
    return out


def resolve_frame_dir(path: Path) -> Path:
    if path.is_dir() and (path / "screenshots").is_dir():
        return path / "screenshots"
    if path.is_dir() and (path / "gameplay/composite").is_dir():
        return path / "gameplay/composite"
    if path.is_dir() and (path / "composite").is_dir():
        return path / "composite"
    return path


def list_images(path: Path, glob: str) -> list[Path]:
    frame_dir = resolve_frame_dir(path)
    if frame_dir.is_file() and frame_dir.suffix.lower() in IMAGE_EXTS:
        return [frame_dir]
    if not frame_dir.is_dir():
        raise SystemExit(f"frame path is not a directory or image: {path}")
    images = [
        p for p in frame_dir.glob(glob)
        if p.is_file() and p.suffix.lower() in IMAGE_EXTS
    ]
    return sorted(images, key=lambda p: str(p))


def crop_to_common(image: Image.Image, crop: tuple[int, int, int, int] | None) -> Image.Image:
    if crop is None:
        return image
    x, y, w, h = crop
    x = max(0, min(x, image.width - 1))
    y = max(0, min(y, image.height - 1))
    return image.crop((x, y, min(image.width, x + w), min(image.height, y + h)))


def avg_hash(gray: Image.Image) -> str:
    small = gray.resize((8, 8), Image.Resampling.BILINEAR)
    values = list(small.tobytes())
    mean = sum(values) / len(values)
    bits = 0
    for value in values:
        bits = (bits << 1) | int(value >= mean)
    return f"{bits:016x}"


def d_hash(gray: Image.Image) -> str:
    small = gray.resize((9, 8), Image.Resampling.BILINEAR)
    values = list(small.tobytes())
    bits = 0
    for y in range(8):
        row = values[y * 9:(y + 1) * 9]
        for x in range(8):
            bits = (bits << 1) | int(row[x] > row[x + 1])
    return f"{bits:016x}"


def hamming_hex(a: str, b: str) -> int:
    if not a or not b:
        return 64
    return (int(a, 16) ^ int(b, 16)).bit_count()


def entropy(gray: Image.Image) -> float:
    hist = gray.histogram()
    total = sum(hist)
    if total <= 0:
        return 0.0
    out = 0.0
    for count in hist:
        if count:
            p = count / total
            out -= p * math.log2(p)
    return out


def color_hist(rgb: Image.Image) -> list[float]:
    small = rgb.resize((64, 64), Image.Resampling.BILINEAR)
    hist: list[float] = []
    for channel in small.split():
        bins = channel.histogram()
        for i in range(0, 256, 16):
            hist.append(sum(bins[i:i + 16]))
    total = sum(hist) or 1.0
    return [v / total for v in hist]


def colorfulness(rgb: Image.Image) -> float:
    stat = ImageStat.Stat(rgb)
    return float(sum(stat.stddev[:3]) / 3.0) if len(stat.stddev) >= 3 else 0.0


def motion(prev_gray: Image.Image | None, gray: Image.Image) -> tuple[float, float]:
    if prev_gray is None or prev_gray.size != gray.size:
        return 0.0, 0.0
    diff = ImageChops.difference(prev_gray, gray)
    mean = float(ImageStat.Stat(diff).mean[0])
    hist = diff.histogram()
    changed = sum(hist[9:])
    total = gray.width * gray.height
    return mean, (changed / total) * 100.0 if total else 0.0


def compute_features(
    paths: list[Path],
    crop: tuple[int, int, int, int] | None,
    black_luma: float,
    min_nonblack_pct: float,
    static_motion: float,
    ignore_indexes: set[int],
) -> list[FrameFeature]:
    features: list[FrameFeature] = []
    prev_gray: Image.Image | None = None
    prev_dhash = ""
    for idx, path in enumerate(paths):
        with Image.open(path) as raw:
            rgb = crop_to_common(raw.convert("RGB"), crop)
            gray = rgb.convert("L")
            stat = ImageStat.Stat(gray)
            hist = gray.histogram()
            total = gray.width * gray.height
            nonblack_pct = (sum(hist[9:]) / total) * 100.0 if total else 0.0
            mean_luma = float(stat.mean[0])
            m_mae, changed_pct = motion(prev_gray, gray)
            dh = d_hash(gray)
            hdist = hamming_hex(prev_dhash, dh) if prev_dhash else 64
            ent = entropy(gray)
            cf = colorfulness(rgb)
            is_black = mean_luma <= black_luma and nonblack_pct < min_nonblack_pct
            is_static = idx > 0 and m_mae <= static_motion and hdist <= 2
            score = (
                ent * 8.0
                + cf * 0.25
                + m_mae * 1.8
                + changed_pct * 0.35
                + nonblack_pct * 0.08
                - (80.0 if is_black else 0.0)
                - (18.0 if is_static else 0.0)
                - (120.0 if idx in ignore_indexes else 0.0)
            )
            features.append(FrameFeature(
                index=idx,
                path=str(path),
                width=rgb.width,
                height=rgb.height,
                mean_luma=mean_luma,
                nonblack_pct=nonblack_pct,
                entropy=ent,
                colorfulness=cf,
                motion_mae=m_mae,
                changed_pct_from_prev=changed_pct,
                dhash=dh,
                hist=color_hist(rgb),
                is_black=is_black,
                is_static_from_prev=is_static,
                score=score,
            ))
            prev_gray = gray.copy()
            prev_dhash = dh
    return features


def acceptable_frames(
    features: list[FrameFeature],
    min_nonblack_pct: float,
    min_entropy: float,
    min_motion_mae: float,
) -> list[FrameFeature]:
    return [
        f for f in features
        if not f.is_black
        and f.nonblack_pct >= min_nonblack_pct
        and f.entropy >= min_entropy
        and (f.index == 0 or f.motion_mae >= min_motion_mae or not f.is_static_from_prev)
    ]


def select_keyframes(candidates: list[FrameFeature], max_keyframes: int, min_gap: int) -> list[FrameFeature]:
    chosen: list[FrameFeature] = []
    for frame in sorted(candidates, key=lambda f: f.score, reverse=True):
        if all(abs(frame.index - other.index) >= min_gap for other in chosen):
            chosen.append(frame)
        if len(chosen) >= max_keyframes:
            break
    return sorted(chosen, key=lambda f: f.index)


def feature_distance(a: FrameFeature, b: FrameFeature) -> float:
    hash_dist = hamming_hex(a.dhash, b.dhash) / 64.0
    hist_dist = sum(abs(x - y) for x, y in zip(a.hist, b.hist))
    luma_dist = abs(a.mean_luma - b.mean_luma) / 255.0
    entropy_dist = abs(a.entropy - b.entropy) / 8.0
    return hash_dist * 0.55 + hist_dist * 0.25 + luma_dist * 0.15 + entropy_dist * 0.05


def align_one(
    source: FrameFeature,
    candidates: list[FrameFeature],
    source_count: int,
    candidate_count: int,
    max_progress_delta: float,
) -> tuple[FrameFeature, float] | None:
    if not candidates:
        return None
    source_progress = source.index / max(1, source_count - 1)
    window = [
        c for c in candidates
        if abs((c.index / max(1, candidate_count - 1)) - source_progress) <= max_progress_delta
    ]
    pool = window or candidates
    best = min(pool, key=lambda c: feature_distance(source, c))
    return best, feature_distance(source, best)


def resized_pair(
    a_path: Path,
    b_path: Path,
    a_crop: tuple[int, int, int, int] | None,
    b_crop: tuple[int, int, int, int] | None,
) -> tuple[Image.Image, Image.Image]:
    a = crop_to_common(Image.open(a_path).convert("RGB"), a_crop)
    b = crop_to_common(Image.open(b_path).convert("RGB"), b_crop)
    target = (min(a.width, b.width), min(a.height, b.height))
    if a.size != target:
        a = a.resize(target, Image.Resampling.LANCZOS)
    if b.size != target:
        b = b.resize(target, Image.Resampling.LANCZOS)
    return a, b


def compare_images(
    gl_path: Path,
    metal_path: Path,
    gl_crop: tuple[int, int, int, int] | None,
    metal_crop: tuple[int, int, int, int] | None,
    out_dir: Path,
    threshold: int,
) -> tuple[float, float, float, int, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    gl, metal = resized_pair(gl_path, metal_path, gl_crop, metal_crop)
    diff = ImageChops.difference(gl, metal)
    stat = ImageStat.Stat(diff)
    channels = len(stat.mean)
    mae = sum(stat.mean) / channels
    rms = math.sqrt(sum(v * v for v in stat.rms) / channels)
    max_abs = max(ch_max[1] for ch_max in stat.extrema)
    red, green, blue = diff.split()
    max_channel = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    hist = max_channel.histogram()
    changed = sum(hist[threshold + 1:])
    pixels = gl.width * gl.height
    changed_pct = (changed / pixels) * 100.0 if pixels else 0.0
    diff_path = out_dir / "diff-amplified.png"
    ImageEnhance.Brightness(diff).enhance(8.0).save(diff_path)
    gl.save(out_dir / "gl-crop.png")
    metal.save(out_dir / "metal-crop.png")
    return changed_pct, mae, rms, max_abs, diff_path


def fit_thumb(path: Path, width: int, crop: tuple[int, int, int, int] | None) -> Image.Image:
    with Image.open(path) as raw:
        rgb = crop_to_common(raw.convert("RGB"), crop)
        ratio = width / rgb.width
        height = max(1, int(rgb.height * ratio))
        return rgb.resize((width, height), Image.Resampling.LANCZOS)


def make_triptych(
    gl_path: Path,
    metal_path: Path,
    oracle_path: Path | None,
    diff_path: Path,
    out_path: Path,
    gl_crop: tuple[int, int, int, int] | None,
    metal_crop: tuple[int, int, int, int] | None,
    oracle_crop: tuple[int, int, int, int] | None,
    labels: Iterable[str],
) -> None:
    thumb_w = 320
    panels = [
        fit_thumb(gl_path, thumb_w, gl_crop),
        fit_thumb(metal_path, thumb_w, metal_crop),
    ]
    if oracle_path is not None:
        panels.append(fit_thumb(oracle_path, thumb_w, oracle_crop))
    panels.append(fit_thumb(diff_path, thumb_w, None))
    labels = list(labels)
    font = ImageFont.load_default()
    label_h = 30
    panel_h = max(p.height for p in panels)
    board = Image.new("RGB", (thumb_w * len(panels), panel_h + label_h), "white")
    draw = ImageDraw.Draw(board)
    for i, panel in enumerate(panels):
        x = i * thumb_w
        board.paste(panel, (x, label_h))
        draw.text((x + 8, 8), labels[i] if i < len(labels) else "", fill=(0, 0, 0), font=font)
    board.save(out_path, quality=92)


def make_contact_sheet(triptychs: list[Path], out_path: Path) -> None:
    if not triptychs:
        return
    thumbs: list[Image.Image] = []
    for path in triptychs:
        with Image.open(path) as raw:
            rgb = raw.convert("RGB")
            target_w = 960
            ratio = target_w / rgb.width
            thumbs.append(rgb.resize((target_w, max(1, int(rgb.height * ratio))), Image.Resampling.LANCZOS))
    pad = 16
    width = max(t.width for t in thumbs) + pad * 2
    height = sum(t.height for t in thumbs) + pad * (len(thumbs) + 1)
    board = Image.new("RGB", (width, height), "white")
    y = pad
    for thumb in thumbs:
        board.paste(thumb, (pad, y))
        y += thumb.height + pad
    board.save(out_path, quality=92)


def write_report(
    path: Path,
    payload: dict,
    pairs: list[PairResult],
    fail_lines: list[str],
) -> None:
    lines = [
        f"# {payload['verdict']} — M15 gameplay visual compare",
        "",
        f"- game: {payload['game']}",
        f"- threshold: {payload['threshold']}% changed pixels per keyframe",
        f"- selected_keyframes: {len(pairs)}",
        f"- gl_frames: {payload['sources']['gl_frames']}",
        f"- metal_frames: {payload['sources']['metal_frames']}",
    ]
    if payload["sources"].get("oracle_frames"):
        lines.append(f"- oracle_frames: {payload['sources']['oracle_frames']}")
    lines.extend([
        f"- contact_sheet: {payload['artifacts']['contact_sheet']}",
        "",
        "## Keyframes",
        "",
        "| key | gl | metal | oracle | align_dist | changed_pct | triptych |",
        "|----:|---:|------:|-------:|-----------:|------------:|----------|",
    ])
    for pair in pairs:
        oracle = "n/a" if pair.oracle_index is None else str(pair.oracle_index)
        lines.append(
            f"| {pair.index} | {pair.gl_index} | {pair.metal_index} | {oracle} | "
            f"{pair.alignment_distance:.4f} | {pair.changed_pct:.4f} | {pair.triptych_png} |"
        )
    if fail_lines:
        lines.extend(["", "## Frames Over Threshold", ""])
        lines.extend(f"- {line}" for line in fail_lines)
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True, help="M15 game alias, e.g. pgr2")
    parser.add_argument("--gl-frames", required=True, type=Path, help="GL run dir, screenshots dir, or image")
    parser.add_argument("--metal-frames", required=True, type=Path, help="Metal run dir, screenshots dir, or image")
    parser.add_argument("--oracle-frames", type=Path, help="Optional oracle workflow/composite dir")
    parser.add_argument("--out-dir", type=Path,
                        help="Default: benchmark-runs/<TS>-metal-gl-compare-<game>-gameplay")
    parser.add_argument("--glob", default="*.png")
    parser.add_argument("--crop", type=parse_crop)
    parser.add_argument("--gl-crop", type=parse_crop,
                        help="Source-specific GL crop x,y,width,height; defaults to --crop")
    parser.add_argument("--metal-crop", type=parse_crop,
                        help="Source-specific Metal crop x,y,width,height; defaults to --crop")
    parser.add_argument("--oracle-crop", type=parse_crop,
                        help="Source-specific oracle crop x,y,width,height; defaults to --crop")
    parser.add_argument("--threshold", type=float, default=1.0,
                        help="Maximum changed-pixels pct for PASS")
    parser.add_argument("--pixel-threshold", type=int, default=8,
                        help="Per-channel threshold used to count changed pixels")
    parser.add_argument("--max-keyframes", type=int, default=6)
    parser.add_argument("--min-keyframes", type=int, default=3)
    parser.add_argument("--min-gap", type=int, default=4,
                        help="Minimum GL frame-index gap between selected keyframes")
    parser.add_argument("--min-nonblack-pct", type=float, default=5.0)
    parser.add_argument("--min-entropy", type=float, default=1.2)
    parser.add_argument("--min-motion-mae", type=float, default=0.2)
    parser.add_argument("--black-luma", type=float, default=4.0)
    parser.add_argument("--static-motion", type=float, default=1.0)
    parser.add_argument("--max-align-distance", type=float, default=0.35)
    parser.add_argument("--max-progress-delta", type=float, default=0.25)
    parser.add_argument("--ignore-gl-indexes", type=parse_int_set, default=set())
    parser.add_argument("--ignore-metal-indexes", type=parse_int_set, default=set())
    parser.add_argument(
        "--diagnostic", action="store_true",
        help=(
            "Mark this run as a triage diagnostic, not strict M15 evidence. "
            "Sets evidence_class=diagnostic and gameplay_evidence=false in "
            "the output summary regardless of verdict. Auto-set when any "
            "strictness-affecting parameter is relaxed below its M15 "
            "default (see auto-detection logic post-parse)."
        ),
    )
    args = parser.parse_args(argv)

    # Mechanical strictness check: if the operator relaxed any of the
    # M15-evidence-grade parameters, force diagnostic mode even when the
    # --diagnostic flag was forgotten. The strict default values listed
    # here MUST match the argparse defaults above; if one changes, change
    # the other. Codex flagged in the 2026-05-11 evening review that
    # operator-remembered diagnostic marking can reintroduce the
    # capture/static-canary-PASS-poses-as-gameplay-evidence footgun, so
    # the parser auto-demotes relaxed runs.
    relaxed_reasons: list[str] = []
    if args.threshold > 1.0:
        relaxed_reasons.append(
            f"--threshold={args.threshold} > strict 1.0 (PASS bar)")
    if args.min_keyframes < 3:
        relaxed_reasons.append(
            f"--min-keyframes={args.min_keyframes} < strict 3")
    if args.max_align_distance > 0.35:
        relaxed_reasons.append(
            f"--max-align-distance={args.max_align_distance} > strict 0.35")
    if args.max_progress_delta > 0.25:
        relaxed_reasons.append(
            f"--max-progress-delta={args.max_progress_delta} > strict 0.25")
    if args.min_nonblack_pct < 5.0:
        relaxed_reasons.append(
            f"--min-nonblack-pct={args.min_nonblack_pct} < strict 5.0")
    if args.min_entropy < 1.2:
        relaxed_reasons.append(
            f"--min-entropy={args.min_entropy} < strict 1.2")
    if args.min_motion_mae < 0.2:
        relaxed_reasons.append(
            f"--min-motion-mae={args.min_motion_mae} < strict 0.2")
    if args.ignore_gl_indexes:
        relaxed_reasons.append(
            "--ignore-gl-indexes set (selectively excluding GL frames)")
    if args.ignore_metal_indexes:
        relaxed_reasons.append(
            "--ignore-metal-indexes set (selectively excluding Metal frames)")
    auto_diagnostic = bool(relaxed_reasons) and not args.diagnostic
    if auto_diagnostic:
        eprint(
            "[m15-gameplay-visual-compare] auto-demoting to diagnostic "
            f"due to relaxed parameter(s): {'; '.join(relaxed_reasons)}"
        )
        args.diagnostic = True

    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = args.out_dir or (BENCHMARK_ROOT / f"{stamp}-metal-gl-compare-{args.game}-gameplay")
    out_dir.mkdir(parents=True, exist_ok=True)
    diffs_dir = out_dir / "diffs"
    triptych_dir = out_dir / "triptychs"
    diffs_dir.mkdir(exist_ok=True)
    triptych_dir.mkdir(exist_ok=True)

    gl_paths = list_images(args.gl_frames, args.glob)
    metal_paths = list_images(args.metal_frames, args.glob)
    oracle_paths = list_images(args.oracle_frames, args.glob) if args.oracle_frames else []
    if not gl_paths or not metal_paths:
        raise SystemExit(f"need at least one GL and Metal frame (gl={len(gl_paths)} metal={len(metal_paths)})")

    gl_crop = args.gl_crop or args.crop
    metal_crop = args.metal_crop or args.crop
    oracle_crop = args.oracle_crop or args.crop

    gl_features = compute_features(
        gl_paths, gl_crop, args.black_luma, args.min_nonblack_pct,
        args.static_motion, args.ignore_gl_indexes,
    )
    metal_features = compute_features(
        metal_paths, metal_crop, args.black_luma, args.min_nonblack_pct,
        args.static_motion, args.ignore_metal_indexes,
    )
    oracle_features = compute_features(
        oracle_paths, oracle_crop, args.black_luma, args.min_nonblack_pct,
        args.static_motion, set(),
    ) if oracle_paths else []

    gl_candidates = acceptable_frames(
        gl_features, args.min_nonblack_pct, args.min_entropy, args.min_motion_mae,
    )
    metal_candidates = acceptable_frames(
        metal_features, args.min_nonblack_pct, args.min_entropy, args.min_motion_mae,
    )
    oracle_candidates = acceptable_frames(
        oracle_features, args.min_nonblack_pct, args.min_entropy, args.min_motion_mae,
    )

    selected = select_keyframes(gl_candidates, args.max_keyframes, args.min_gap)
    infra_failures: list[str] = []
    if len(selected) < args.min_keyframes:
        infra_failures.append(
            f"only {len(selected)} usable GL gameplay keyframes after rejection; "
            f"need at least {args.min_keyframes}"
        )
    if len(metal_candidates) < args.min_keyframes:
        infra_failures.append(
            f"only {len(metal_candidates)} usable Metal gameplay keyframes after rejection; "
            f"need at least {args.min_keyframes}"
        )

    pairs: list[PairResult] = []
    fail_lines: list[str] = []
    triptych_paths: list[Path] = []
    if not infra_failures:
        for pair_idx, gl_feature in enumerate(selected, start=1):
            metal_match = align_one(
                gl_feature, metal_candidates, len(gl_features), len(metal_features),
                args.max_progress_delta,
            )
            if metal_match is None:
                infra_failures.append(f"no Metal match for GL frame {gl_feature.index}")
                continue
            metal_feature, align_dist = metal_match
            if align_dist > args.max_align_distance:
                infra_failures.append(
                    f"GL frame {gl_feature.index} best Metal match {metal_feature.index} "
                    f"alignment_distance={align_dist:.4f} exceeds {args.max_align_distance:.4f}"
                )
                continue

            oracle_feature: FrameFeature | None = None
            oracle_dist: float | None = None
            if oracle_candidates:
                oracle_match = align_one(
                    gl_feature, oracle_candidates, len(gl_features), len(oracle_features),
                    args.max_progress_delta,
                )
                if oracle_match:
                    oracle_feature, oracle_dist = oracle_match

            frame_dir = diffs_dir / f"frame-{pair_idx:02d}"
            changed_pct, mae, rms, max_abs, diff_path = compare_images(
                Path(gl_feature.path), Path(metal_feature.path), gl_crop, metal_crop,
                frame_dir, args.pixel_threshold,
            )
            triptych_path = triptych_dir / f"keyframe-{pair_idx:02d}.jpg"
            labels = [
                f"GL #{gl_feature.index}",
                f"Metal #{metal_feature.index}",
            ]
            if oracle_feature is not None:
                labels.append(f"Oracle #{oracle_feature.index}")
            labels.append(f"Diff {changed_pct:.2f}%")
            make_triptych(
                Path(gl_feature.path), Path(metal_feature.path),
                Path(oracle_feature.path) if oracle_feature else None,
                diff_path, triptych_path, gl_crop, metal_crop, oracle_crop, labels,
            )
            triptych_paths.append(triptych_path)
            if changed_pct > args.threshold:
                fail_lines.append(
                    f"keyframe {pair_idx}: changed_pct={changed_pct:.4f} > threshold={args.threshold:.4f}"
                )
            pairs.append(PairResult(
                index=pair_idx,
                gl_index=gl_feature.index,
                metal_index=metal_feature.index,
                oracle_index=oracle_feature.index if oracle_feature else None,
                alignment_distance=align_dist,
                oracle_alignment_distance=oracle_dist,
                changed_pct=changed_pct,
                mean_abs_error=mae,
                rms_error=rms,
                max_abs_error=max_abs,
                gl_png=gl_feature.path,
                metal_png=metal_feature.path,
                oracle_png=oracle_feature.path if oracle_feature else None,
                triptych_png=str(triptych_path),
                diff_png=str(diff_path),
            ))

    contact_sheet = out_dir / "contact-sheet.jpg"
    make_contact_sheet(triptych_paths, contact_sheet)

    pass_value = not infra_failures and not fail_lines and len(pairs) >= args.min_keyframes
    verdict = "PASS" if pass_value else ("INFRA-FAIL" if infra_failures else "FAIL")
    summary_path = out_dir / "summary.json"
    report_path = out_dir / "report.md"
    evidence_class = "diagnostic" if args.diagnostic else "gameplay"
    gameplay_evidence = pass_value and not args.diagnostic
    payload = {
        "schema": "m15-gameplay-visual-compare-v1",
        "pass": pass_value,
        "verdict": verdict,
        "game": args.game,
        "evidence_class": evidence_class,
        "gameplay_evidence": gameplay_evidence,
        "diagnostic": bool(args.diagnostic),
        "threshold": args.threshold,
        "pixel_threshold": args.pixel_threshold,
        "sources": {
            "gl_frames": str(resolve_frame_dir(args.gl_frames)),
            "metal_frames": str(resolve_frame_dir(args.metal_frames)),
            "oracle_frames": str(resolve_frame_dir(args.oracle_frames)) if args.oracle_frames else None,
            "crop": args.crop,
            "gl_crop": gl_crop,
            "metal_crop": metal_crop,
            "oracle_crop": oracle_crop,
        },
        "artifacts": {
            "report": str(report_path),
            "summary": str(summary_path),
            "contact_sheet": str(contact_sheet),
            "triptychs": str(triptych_dir),
            "diffs": str(diffs_dir),
        },
        "selection": {
            "gl_frame_count": len(gl_features),
            "metal_frame_count": len(metal_features),
            "oracle_frame_count": len(oracle_features),
            "gl_candidate_count": len(gl_candidates),
            "metal_candidate_count": len(metal_candidates),
            "oracle_candidate_count": len(oracle_candidates),
            "min_keyframes": args.min_keyframes,
            "max_keyframes": args.max_keyframes,
            "min_nonblack_pct": args.min_nonblack_pct,
            "min_entropy": args.min_entropy,
            "min_motion_mae": args.min_motion_mae,
            "max_align_distance": args.max_align_distance,
            "max_progress_delta": args.max_progress_delta,
        },
        "infra_failures": infra_failures,
        "failures": fail_lines,
        "frames": [
            {
                "index": pair.index,
                "mae": round(pair.mean_abs_error, 4),
                "rms": round(pair.rms_error, 4),
                "max_abs": pair.max_abs_error,
                "changed_pct": round(pair.changed_pct, 4),
                "gl_png": pair.gl_png,
                "metal_png": pair.metal_png,
                "oracle_png": pair.oracle_png,
                "triptych_png": pair.triptych_png,
                "diff_png": pair.diff_png,
                "gl_index": pair.gl_index,
                "metal_index": pair.metal_index,
                "oracle_index": pair.oracle_index,
                "alignment_distance": round(pair.alignment_distance, 6),
                "oracle_alignment_distance": (
                    round(pair.oracle_alignment_distance, 6)
                    if pair.oracle_alignment_distance is not None else None
                ),
            }
            for pair in pairs
        ],
        "rejected": {
            "gl_black_or_static": [
                asdict(f) for f in gl_features
                if f not in gl_candidates and (f.is_black or f.is_static_from_prev)
            ][:40],
            "metal_black_or_static": [
                asdict(f) for f in metal_features
                if f not in metal_candidates and (f.is_black or f.is_static_from_prev)
            ][:40],
        },
    }
    summary_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    write_report(report_path, payload, pairs, fail_lines + infra_failures)

    # Make the summary easy to discover from shell logs.
    print(f"verdict={verdict}")
    print(f"keyframes={len(pairs)}")
    print(f"summary={summary_path}")
    print(f"contact_sheet={contact_sheet}")
    if infra_failures:
        for line in infra_failures:
            eprint(f"INFRA: {line}")
        return 2
    return 0 if pass_value else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
