#!/usr/bin/env python3
"""
extract-keyframes.py — extract scene-change keyframes from a recorded
gameplay video (or any video file).

The pipeline used by agent-driven game-development workflows:

    1. Capture gameplay → mp4               (composite-record.sh)
    2. Extract keyframes from mp4           (THIS TOOL)
    3. Run xemu rendering of the same scene
    4. Diff per-keyframe vs the captured frames

Step 2 needs to pick "interesting" frames (scene cuts, asset loads,
lighting changes) instead of every frame, otherwise the diff cost
balloons. ffmpeg's `select=gt(scene,N)` filter scores each frame
against the previous one with a frame-difference metric in [0..1];
frames above the threshold are scene boundaries.

This wrapper:

  1. Runs ffmpeg with select=gt(scene,T) and writes one PNG per
     scene-change frame plus a CSV mapping frame-index → timestamp →
     scene-score.
  2. Optionally also writes a "fixed-cadence" PNG every N seconds so
     the comparison can interleave scene-driven and time-driven keys.
     Use this when the scene-detection threshold misses long static
     sections you still want a sample of (e.g. an unchanging menu).

USAGE
    extract-keyframes.py VIDEO [--out-dir DIR]
                              [--threshold T]            (default 0.30)
                              [--min-gap-s N]            (default 0.5)
                              [--every-s N]              (extra time-driven keys)
                              [--max-keyframes N]        (default 60)
                              [--width N --height N]     (resize while extracting)
                              [--manifest PATH]          (write JSON manifest here)

OUTPUT
    <out-dir>/keyframes/scene/0001.png   one per scene-change frame
                       /timed/0001.png   one per --every-s mark (if requested)
    <out-dir>/scene-timestamps.csv       frame_idx, time_s, scene_score
    <out-dir>/manifest.json              run metadata + frame manifest
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple


def _run(argv: List[str], capture: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(argv, check=False,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None,
                          text=True)


def _ffprobe_duration(video: Path) -> Optional[float]:
    p = _run(["ffprobe", "-v", "error",
              "-show_entries", "format=duration",
              "-of", "default=nw=1:nk=1", str(video)])
    try:
        return float((p.stdout or "").strip())
    except (TypeError, ValueError):
        return None


def _extract_scene_keyframes(video: Path, out_dir: Path, threshold: float,
                             min_gap_s: float, max_keyframes: int,
                             resize: Optional[Tuple[int, int]]
                             ) -> Tuple[List[dict], str]:
    """Run ffmpeg with `select=gt(scene,T)` and stream showinfo /
    metadata=print to capture per-frame metadata. Returns the list of
    emitted scene frames (post min-gap filtering, capped at
    max_keyframes), with the PNG path on disk for each kept frame.

    Architecture note: an earlier version of this function chained
    `gt(t-prev_selected_t,N)` into the select expression so ffmpeg
    would do min-gap filtering itself. Empirically that broke the
    `scene` metric — once a frame is suppressed by min-gap, the next
    frame's scene_score is computed against the *previous emitted*
    frame instead of the prior input frame, producing systematically
    wrong scores and missing real cuts. The fix is to let ffmpeg emit
    every scene match (one PNG each) and apply min-gap as a Python
    post-filter, deleting suppressed PNGs. This costs a few extra
    encodes but keeps the scene metric correct.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    select_expr = (
        f"select='gt(scene\\,{threshold})',"
        "metadata=print:file=-,showinfo"
    )
    if resize:
        select_expr = f"scale={resize[0]}:{resize[1]}," + select_expr
    pattern = str(out_dir / "raw_%04d.png")
    # Cap raw extraction at 4× the requested max so min-gap filtering
    # has headroom; if the cap is hit the post-filter still trims to
    # max_keyframes.
    raw_cap = max(max_keyframes * 4, max_keyframes)
    argv = [
        "ffmpeg", "-hide_banner", "-y",
        "-loglevel", "info",
        "-i", str(video),
        "-filter:v", select_expr,
        "-fps_mode", "vfr",
        "-frames:v", str(raw_cap),
        pattern,
    ]
    p = _run(argv)
    text = (p.stderr or "") + "\n" + (p.stdout or "")
    raw_frames: List[dict] = []
    # showinfo emits its line BEFORE metadata=print emits the
    # corresponding lavfi.scene_score=<val> in this filter chain
    # (verified empirically with ffmpeg 8.0.1). So we accept either
    # order: when we see the showinfo line we append a frame; when we
    # see the score line we attach it to the most-recently-appended
    # frame whose score is still None. This is robust to either
    # ordering across ffmpeg versions.
    for line in text.splitlines():
        m = re.search(r"lavfi\.scene_score=([\d.]+)", line)
        if m:
            score = float(m.group(1))
            for f in reversed(raw_frames):
                if f.get("scene_score") is None:
                    f["scene_score"] = score
                    break
            continue
        m = re.search(r"\] n:\s*(\d+)\b.*?pts_time:([\d.]+)", line)
        if m:
            n = int(m.group(1))
            pts = float(m.group(2))
            raw_frames.append({
                "n": n,
                "time_s": pts,
                "scene_score": None,
            })
    # Pair raw frames with the PNGs ffmpeg actually wrote (encounter order).
    raw_pngs = sorted(out_dir.glob("raw_*.png"))
    for i, frame in enumerate(raw_frames):
        if i < len(raw_pngs):
            frame["_raw_png"] = raw_pngs[i]
    # Apply min-gap and max-keyframes in Python. `min_gap_s` suppresses
    # cuts that fire within `min_gap_s` of a previously-kept cut; this
    # is the de-bouncer for things like fade transitions that produce
    # multiple consecutive scene_score>threshold frames.
    kept: List[dict] = []
    last_kept_t = float("-inf")
    for f in raw_frames:
        if f["time_s"] - last_kept_t < min_gap_s:
            continue
        if len(kept) >= max_keyframes:
            break
        last_kept_t = f["time_s"]
        kept.append(f)
    # Rename kept raw_*.png → 0001.png ..., delete raw frames we dropped.
    kept_paths = set(f.get("_raw_png") for f in kept if "_raw_png" in f)
    for raw_png in raw_pngs:
        if raw_png not in kept_paths:
            try:
                raw_png.unlink()
            except OSError:
                pass
    for i, f in enumerate(kept, start=1):
        if "_raw_png" in f:
            new_path = out_dir / f"{i:04d}.png"
            try:
                f["_raw_png"].rename(new_path)
            except OSError:
                pass
            f["png"] = new_path.name
        del f["_raw_png"]
    return kept, p.stderr or ""


def _extract_timed(video: Path, out_dir: Path, every_s: float,
                   resize: Optional[Tuple[int, int]],
                   max_frames: int) -> List[dict]:
    """Time-driven extraction at fixed cadence. Useful for long
    near-static scenes the scene detector skips. Internally the same
    ffmpeg `fps=1/N` filter."""
    out_dir.mkdir(parents=True, exist_ok=True)
    expr = f"fps=1/{every_s},showinfo"
    if resize:
        expr = f"scale={resize[0]}:{resize[1]}," + expr
    pattern = str(out_dir / "%04d.png")
    argv = [
        "ffmpeg", "-hide_banner", "-y",
        "-loglevel", "info",
        "-i", str(video),
        "-vf", expr,
        "-vsync", "vfr",
        "-frames:v", str(max_frames),
        pattern,
    ]
    p = _run(argv)
    out: List[dict] = []
    for line in (p.stderr or "").splitlines():
        m = re.search(r"\] n:\s*(\d+)\b.*?pts_time:([\d.]+)", line)
        if m:
            out.append({
                "n": int(m.group(1)),
                "time_s": float(m.group(2)),
                "scene_score": None,
                "kind": "timed",
            })
    return out


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("--out-dir", default=None,
                    help="Directory for extracted frames + manifest "
                         "(default: <video-dir>/keyframes/)")
    ap.add_argument("--threshold", type=float, default=0.30,
                    help="Scene-change score threshold in [0..1] "
                         "(default: 0.30)")
    ap.add_argument("--min-gap-s", type=float, default=0.5,
                    help="Suppress scene matches closer than this "
                         "(default: 0.5 s)")
    ap.add_argument("--every-s", type=float, default=0.0,
                    help="Also emit a frame every N seconds in addition "
                         "to scene cuts (default: disabled)")
    ap.add_argument("--max-keyframes", type=int, default=60)
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--manifest", default=None,
                    help="Write summary JSON here (default: "
                         "<out-dir>/manifest.json)")
    args = ap.parse_args(argv)

    video = Path(args.video).resolve()
    if not video.exists():
        print(f"video not found: {video}", file=sys.stderr)
        return 2
    if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
        print("ffmpeg / ffprobe required (brew install ffmpeg)", file=sys.stderr)
        return 1

    out_dir = Path(args.out_dir).resolve() if args.out_dir else video.parent / "keyframes"
    scene_dir = out_dir / "scene"
    timed_dir = out_dir / "timed"
    scene_dir.mkdir(parents=True, exist_ok=True)

    resize = None
    if args.width and args.height:
        resize = (args.width, args.height)

    print(f"[extract-keyframes] video    : {video}")
    print(f"[extract-keyframes] out_dir  : {out_dir}")
    print(f"[extract-keyframes] threshold: {args.threshold}")

    started = time.time()
    duration = _ffprobe_duration(video)
    scene_frames, scene_log = _extract_scene_keyframes(
        video, scene_dir, args.threshold, args.min_gap_s,
        args.max_keyframes, resize)
    timed_frames: List[dict] = []
    if args.every_s > 0:
        max_timed = int(duration / args.every_s) + 4 if duration else 32
        timed_frames = _extract_timed(video, timed_dir, args.every_s,
                                      resize, max_timed)

    # _extract_scene_keyframes already populates frame["png"] with the
    # final filename relative to scene_dir — promote to the kind tag and
    # rebase the relative path so it's relative to out_dir.
    for f in scene_frames:
        f["kind"] = "scene"
        if "png" in f:
            f["png"] = str((scene_dir / f["png"]).relative_to(out_dir))
    timed_pngs = sorted(timed_dir.glob("*.png")) if timed_frames else []
    for i, f in enumerate(timed_frames):
        if i < len(timed_pngs):
            f["png"] = str(timed_pngs[i].relative_to(out_dir))

    csv_path = out_dir / "scene-timestamps.csv"
    with csv_path.open("w", newline="") as fout:
        w = csv.writer(fout)
        w.writerow(["kind", "n", "time_s", "scene_score", "png"])
        for f in scene_frames + timed_frames:
            w.writerow([f.get("kind", ""), f.get("n", ""),
                        f.get("time_s", ""),
                        f.get("scene_score") if f.get("scene_score") is not None else "",
                        f.get("png", "")])

    manifest_path = Path(args.manifest) if args.manifest else out_dir / "manifest.json"
    manifest = {
        "tool": "extract-keyframes.py",
        "video": str(video),
        "video_duration_s": duration,
        "threshold": args.threshold,
        "min_gap_s": args.min_gap_s,
        "every_s": args.every_s if args.every_s > 0 else None,
        "max_keyframes": args.max_keyframes,
        "resize": list(resize) if resize else None,
        "wall_elapsed_s": round(time.time() - started, 3),
        "scene_count": len(scene_frames),
        "timed_count": len(timed_frames),
        "scene_frames": scene_frames,
        "timed_frames": timed_frames,
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    with manifest_path.open("w") as f:
        json.dump(manifest, f, indent=2)

    print(f"[extract-keyframes] scene PNGs: {len(scene_frames)}")
    print(f"[extract-keyframes] timed PNGs: {len(timed_frames)}")
    print(f"[extract-keyframes] manifest  : {manifest_path}")
    print(f"[extract-keyframes] csv       : {csv_path}")
    return 0 if scene_frames or timed_frames else 1


if __name__ == "__main__":
    sys.exit(main())
