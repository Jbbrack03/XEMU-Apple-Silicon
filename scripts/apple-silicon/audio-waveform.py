#!/usr/bin/env python3
"""
audio-waveform.py — render a recorded audio track as visual artifacts
the agent layer can compare across runs.

WHY THIS TOOL EXISTS
    Agents (LLMs) cannot listen to audio, but the real-Xbox audio
    output is empirically authoritative. If we capture the same audio
    from the real Xbox and from xemu and render BOTH as visual PNGs
    (waveform + spectrogram + numeric stats), the diff between the two
    PNGs is a pixel-comparable signal: missing voices, audio dropouts,
    silence regions, clipping, and pitch drift all show up visually.
    The third oracle leg's audio twin.

PIPELINE
    1. Demux audio from .mp4/.mkv/.wav input via ffmpeg (or accept WAV).
    2. Render an amplitude-vs-time PNG via showwavespic filter.
    3. Render a frequency spectrogram PNG via showspectrumpic filter.
    4. Compute time-domain stats: peak amplitude, RMS, silence
       fractions, clipping count.
    5. Emit a manifest.json bundling all of the above.

USAGE
    audio-waveform.py INPUT [--out-dir DIR]
                            [--width N --height N]   default 1600x300
                            [--silence-db N]         default -50 (dBFS)
                            [--colors STR]           ffmpeg showwavespic colors=

OUTPUT under <out-dir>
    waveform.png        time-domain amplitude
    spectrogram.png     frequency-vs-time heatmap
    audio.wav           extracted PCM (for downstream ML if needed)
    audio-stats.json    peak/rms/silence/clipping numbers
    manifest.json       index of all artifacts + ffmpeg commands used
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional, List


def _run(argv: List[str]) -> subprocess.CompletedProcess:
    return subprocess.run(argv, check=False, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def extract_wav(src: Path, out: Path) -> bool:
    """Demux the audio stream into a 48 kHz mono PCM 16-bit WAV. Mono
    so the downstream stats and renders are deterministic regardless
    of whether the source was stereo or 5.1 (we don't need spatial
    info for the oracle compare). 16-bit because ffmpeg's astats
    summary handles s16 robustly across versions."""
    out.parent.mkdir(parents=True, exist_ok=True)
    p = _run([
        "ffmpeg", "-hide_banner", "-y", "-loglevel", "error",
        "-i", str(src),
        "-vn",
        "-ac", "1",
        "-ar", "48000",
        "-sample_fmt", "s16",
        str(out),
    ])
    return p.returncode == 0 and out.exists() and out.stat().st_size > 44


def render_waveform(wav: Path, png: Path, width: int, height: int,
                    colors: str = "0x4eb3ff|0x208860") -> bool:
    """showwavespic: draws the amplitude envelope. The default colors
    are split=channels — per-channel renders are stacked vertically.
    With a mono source the second color is unused."""
    png.parent.mkdir(parents=True, exist_ok=True)
    p = _run([
        "ffmpeg", "-hide_banner", "-y", "-loglevel", "error",
        "-i", str(wav),
        "-frames:v", "1",
        "-filter_complex",
        f"showwavespic=s={width}x{height}:colors={colors}:split_channels=0",
        str(png),
    ])
    return p.returncode == 0 and png.exists()


def render_spectrogram(wav: Path, png: Path, width: int, height: int) -> bool:
    """showspectrumpic: log-frequency spectrogram. mode=combined and
    legend=enabled give a clean compare-friendly artifact (frequency
    on Y, time on X, color = magnitude in dB). Window function set to
    `hann` for the standard tradeoff between time/frequency resolution."""
    png.parent.mkdir(parents=True, exist_ok=True)
    p = _run([
        "ffmpeg", "-hide_banner", "-y", "-loglevel", "error",
        "-i", str(wav),
        "-frames:v", "1",
        "-filter_complex",
        f"showspectrumpic=s={width}x{height}:mode=combined:legend=enabled:scale=log:win_func=hann",
        str(png),
    ])
    return p.returncode == 0 and png.exists()


def compute_stats(wav: Path, silence_db: float) -> dict:
    """Run ffmpeg's astats + silencedetect filters in a single pass to
    extract peak amplitude / RMS / silence / clipping numbers. astats
    output is line-prefixed `[Parsed_astats_0 @ ...] Channel: N` and
    then `Peak level dB: …`, `RMS level dB: …`, etc. We parse stderr
    rather than depending on a JSON output mode (older ffmpeg builds
    didn't have it; this regex-parsing path works back to 4.x)."""
    p = _run([
        "ffmpeg", "-hide_banner", "-loglevel", "info",
        "-i", str(wav),
        "-filter_complex",
        f"silencedetect=noise={silence_db}dB:d=0.2,astats=metadata=0:length=0.05:reset=0",
        "-f", "null", "-",
    ])
    text = (p.stderr or "") + "\n" + (p.stdout or "")
    stats: dict = {
        "silence_db_threshold": silence_db,
        "silences": [],
        "channels": [],
        "peak_dbfs": None,
        "rms_dbfs": None,
        "duration_s": None,
        "samples": None,
    }
    import re
    silence_starts: List[float] = []
    cur_channel: Optional[dict] = None
    for line in text.splitlines():
        m = re.search(r"silence_start:\s*([\d.\-]+)", line)
        if m:
            silence_starts.append(float(m.group(1)))
            continue
        m = re.search(r"silence_end:\s*([\d.\-]+)\s*\|\s*silence_duration:\s*([\d.\-]+)", line)
        if m and silence_starts:
            stats["silences"].append({
                "start_s": silence_starts.pop(0),
                "end_s": float(m.group(1)),
                "duration_s": float(m.group(2)),
            })
            continue
        m = re.search(r"Channel:\s*(\d+)", line)
        if m:
            cur_channel = {"channel": int(m.group(1))}
            stats["channels"].append(cur_channel)
            continue
        if cur_channel is None:
            cur_channel = {"channel": 0}
            stats["channels"].append(cur_channel)
        for label, key in (
            ("Peak level dB", "peak_dbfs"),
            ("RMS level dB", "rms_dbfs"),
            ("Min level", "min_level"),
            ("Max level", "max_level"),
            ("Bit depth", "bit_depth"),
            ("DC offset", "dc_offset"),
            ("Number of samples", "samples"),
            ("Number of NaNs", "nans"),
            ("Number of Infs", "infs"),
            ("Number of denormals", "denormals"),
        ):
            m = re.search(rf"{re.escape(label)}:\s*([\-\d.eE+]+|nan|inf)", line)
            if m:
                val = m.group(1)
                try:
                    cur_channel[key] = float(val)
                except ValueError:
                    cur_channel[key] = val
                if key == "peak_dbfs" and stats["peak_dbfs"] is None:
                    try:
                        stats["peak_dbfs"] = float(val)
                    except ValueError:
                        pass
                if key == "rms_dbfs" and stats["rms_dbfs"] is None:
                    try:
                        stats["rms_dbfs"] = float(val)
                    except ValueError:
                        pass
    # Pull duration via ffprobe (more reliable than parsing astats).
    p2 = _run(["ffprobe", "-v", "error",
               "-show_entries", "format=duration",
               "-of", "default=nw=1:nk=1", str(wav)])
    try:
        stats["duration_s"] = float((p2.stdout or "").strip())
    except (TypeError, ValueError):
        pass
    if stats["duration_s"] and stats["silences"]:
        total_silence = sum(s["duration_s"] for s in stats["silences"])
        stats["silence_fraction"] = total_silence / stats["duration_s"]
    else:
        stats["silence_fraction"] = 0.0
    # Crude clipping estimate: count integer-PCM samples at +/- full-scale.
    # int16 full-scale is 32767/-32768; we read the WAV directly.
    stats["clipping_count"] = _count_clipping(wav)
    return stats


def _count_clipping(wav: Path) -> int:
    """Count int16 samples at full-scale (-32768 or +32767). Works for
    s16-mono WAV files written by extract_wav above. Returns 0 on error."""
    try:
        import wave, struct
        with wave.open(str(wav), "rb") as w:
            n = w.getnframes()
            sw = w.getsampwidth()
            ch = w.getnchannels()
            if sw != 2:
                return 0
            data = w.readframes(n)
        # Read in 1 MB chunks to avoid memory blow-up on long captures.
        clipped = 0
        step = 1 << 20
        for i in range(0, len(data), step):
            chunk = data[i:i + step]
            samples = struct.unpack(f"<{len(chunk) // 2}h", chunk)
            for s in samples:
                if s == -32768 or s == 32767:
                    clipped += 1
        return clipped
    except Exception:
        return 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--out-dir", default=None,
                    help="Output directory (default: <input-dir>/audio-analysis/)")
    ap.add_argument("--width", type=int, default=1600)
    ap.add_argument("--height", type=int, default=300)
    ap.add_argument("--silence-db", type=float, default=-50.0,
                    help="silencedetect threshold in dBFS (default: -50)")
    ap.add_argument("--colors", default="0x4eb3ff|0x208860",
                    help="showwavespic color list (pipe-separated hex)")
    args = ap.parse_args(argv)

    src = Path(args.input).resolve()
    if not src.exists():
        print(f"input not found: {src}", file=sys.stderr)
        return 2
    if shutil.which("ffmpeg") is None:
        print("ffmpeg required (brew install ffmpeg)", file=sys.stderr)
        return 1

    out_dir = Path(args.out_dir).resolve() if args.out_dir else src.parent / "audio-analysis"
    out_dir.mkdir(parents=True, exist_ok=True)

    started = time.time()
    wav_path = out_dir / "audio.wav"
    if not extract_wav(src, wav_path):
        print(f"failed to extract audio from {src}", file=sys.stderr)
        return 1

    waveform_png = out_dir / "waveform.png"
    if not render_waveform(wav_path, waveform_png, args.width, args.height,
                           args.colors):
        print("waveform render failed", file=sys.stderr)

    spectrogram_png = out_dir / "spectrogram.png"
    if not render_spectrogram(wav_path, spectrogram_png, args.width, args.height):
        print("spectrogram render failed", file=sys.stderr)

    stats = compute_stats(wav_path, args.silence_db)
    stats_path = out_dir / "audio-stats.json"
    stats_path.write_text(json.dumps(stats, indent=2))

    manifest = {
        "tool": "audio-waveform.py",
        "input": str(src),
        "out_dir": str(out_dir),
        "wall_elapsed_s": round(time.time() - started, 3),
        "artifacts": {
            "wav": wav_path.name if wav_path.exists() else None,
            "waveform_png": waveform_png.name if waveform_png.exists() else None,
            "spectrogram_png": spectrogram_png.name if spectrogram_png.exists() else None,
            "stats": stats_path.name,
        },
        "render": {"width": args.width, "height": args.height, "colors": args.colors},
        "summary": {
            "duration_s": stats.get("duration_s"),
            "peak_dbfs": stats.get("peak_dbfs"),
            "rms_dbfs": stats.get("rms_dbfs"),
            "silence_fraction": stats.get("silence_fraction"),
            "clipping_count": stats.get("clipping_count"),
            "silence_intervals_count": len(stats.get("silences", [])),
        },
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    print(f"[audio-waveform] input  : {src}")
    print(f"[audio-waveform] out    : {out_dir}")
    print(f"[audio-waveform] dur_s  : {stats.get('duration_s')}")
    print(f"[audio-waveform] peak   : {stats.get('peak_dbfs')} dBFS")
    print(f"[audio-waveform] rms    : {stats.get('rms_dbfs')} dBFS")
    print(f"[audio-waveform] silence: {stats.get('silence_fraction'):.3f} of duration")
    print(f"[audio-waveform] clip   : {stats.get('clipping_count')} samples at full-scale")
    return 0


if __name__ == "__main__":
    sys.exit(main())
