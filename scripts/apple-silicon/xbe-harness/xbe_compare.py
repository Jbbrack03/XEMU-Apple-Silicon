"""
xbe_compare — comparison primitives for diag-XBE captures.

Wraps `compare-screenshots.py` (already in the project) but adds
helpers for:
  - Synthesizing the math-derived expected PNG from a manifest's
    `generator: "expected.py:<fn>"` reference.
  - Picking the right reference per (renderer, flag-recipe) tuple
    from manifest.expected_results (real-xbox > math-derived).
  - Decoding XOSS captures pulled from the real Xbox into PNGs the
    compare script understands.
"""
from __future__ import annotations

import importlib.util
import json
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple

from xbe_discover import XbeManifest

_HERE = Path(__file__).resolve().parent
_AS_DIR = _HERE.parent
COMPARE_SCRIPT = _AS_DIR / "compare-screenshots.py"
ORACLE_CLIENT_PATH = _AS_DIR / "oracle-client.py"


def _load_oracle_client():
    """Import oracle-client.py despite its dash-in-name."""
    spec = importlib.util.spec_from_file_location(
        "oracle_client", ORACLE_CLIENT_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {ORACLE_CLIENT_PATH}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


@dataclass
class CompareVerdict:
    status: str  # "pass" | "fail" | "infra-error"
    rc: int
    captured: Path
    reference: Path
    out_dir: Path
    stdout: str
    stderr: str
    notes: str = ""

    def as_dict(self) -> dict:
        return {
            "status": self.status,
            "rc": self.rc,
            "captured": str(self.captured),
            "reference": str(self.reference),
            "out_dir": str(self.out_dir),
            "notes": self.notes,
            "stdout_tail": self.stdout[-2000:],
            "stderr_tail": self.stderr[-2000:],
        }


def decode_xoss_to_png(xoss_path: Path, png_path: Path) -> Tuple[int, int]:
    """Decode a XOSS-format binary (4-byte 'XOSS' magic + 12-byte hdr +
    raw bytes-per-row * H pixel data) into an RGBA PNG. Returns (W, H).
    Used to convert real-Xbox `<xbe>-capture.bin` artifacts into a
    PNG comparable against expected.py / real-xbox-reference PNGs."""
    data = xoss_path.read_bytes()
    if len(data) < 16 or data[:4] != b"XOSS":
        raise ValueError(f"not a XOSS file: {xoss_path}")
    w, h, stride = struct.unpack_from("<III", data, 4)
    pixels = data[16:]
    if len(pixels) != stride * h:
        raise ValueError(
            f"XOSS payload size {len(pixels)} != stride*h "
            f"({stride}*{h}={stride*h}) in {xoss_path}")
    oc = _load_oracle_client()
    rgba = oc.bgrx_to_rgba(pixels, w, h, stride)
    png_path.parent.mkdir(parents=True, exist_ok=True)
    oc.save_screenshot_png(rgba, w, h, str(png_path))
    return w, h


def synthesize_expected_png(manifest: XbeManifest, generator_ref: str,
                            out_png: Path,
                            flags: Optional[dict] = None) -> Tuple[int, int]:
    """Run the manifest's `generator: "expected.py:<fn>"` to produce a
    math-derived expected PNG. Returns (W, H)."""
    if ":" not in generator_ref:
        raise ValueError(
            f"generator ref must be 'expected.py:<fn>': {generator_ref}")
    py_name, fn_name = generator_ref.split(":", 1)
    py_path = manifest.dir / py_name
    if not py_path.exists():
        raise FileNotFoundError(f"generator py not found: {py_path}")
    spec = importlib.util.spec_from_file_location(
        f"_expected_{manifest.id}", py_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {py_path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    fn = getattr(mod, fn_name)
    rgba = fn(**(flags or {}))
    width = getattr(mod, "WIDTH")
    height = getattr(mod, "HEIGHT")
    oc = _load_oracle_client()
    out_png.parent.mkdir(parents=True, exist_ok=True)
    oc.save_screenshot_png(rgba, width, height, str(out_png))
    return width, height


def select_reference_key(manifest: XbeManifest, renderer: str,
                         flags: dict) -> str:
    """Build the manifest expected_results key. Falls back to
    'any/any/any' if a more-specific key isn't present.

    Recipe-aware: keys can include `fallback=<0|1>` and
    `translated=<0|1>` segments alongside `scale` and `msaa`. The
    selector tries the most specific key first, peeling segments
    off in priority order: fallback > translated > msaa > scale.

    Real-Xbox keys: 'real-xbox/cerbios/default'."""
    scale = str(flags.get("scale", "1"))
    msaa = str(flags.get("msaa", "0"))
    fallback = str(flags.get("fallback", "0"))
    translated = str(flags.get("translated", "0"))
    candidates = [
        # full recipe
        f"{renderer}/scale={scale}/msaa={msaa}/fallback={fallback}/translated={translated}",
        f"{renderer}/scale={scale}/msaa={msaa}/fallback={fallback}",
        f"{renderer}/scale={scale}/msaa={msaa}",
        # peel msaa
        f"{renderer}/scale={scale}/any",
        f"{renderer}/any/any",
        # global default
        "any/any/any",
    ]
    for k in candidates:
        if k in manifest.expected_results:
            return k
    raise KeyError(
        f"no expected_results entry for {manifest.id} {renderer} "
        f"scale={scale} msaa={msaa} fallback={fallback}; "
        f"tried {candidates}")


def resolve_reference_png(manifest: XbeManifest, key: str,
                          work_dir: Path) -> Tuple[Path, str]:
    """Materialize the reference PNG at `work_dir/reference.png`.
    Returns (png_path, kind) where kind ∈ {"real-xbox", "math-derived"}.

    For real-xbox keys, the PNG is expected to already exist under
    docs/apple-silicon/xbox-real-references/<id>/. For math-derived,
    we run the generator to produce a fresh PNG (cheap; deterministic)."""
    spec = manifest.expected_results[key]
    kind = spec.get("kind")
    out_png = work_dir / "reference.png"
    if kind == "real-xbox-capture":
        ref_rel = spec["ref"]
        ref_root = (_AS_DIR.parent.parent / "docs" / "apple-silicon").resolve()
        # `ref` is relative to docs/apple-silicon/.
        ref_path = (ref_root / ref_rel).resolve()
        if not ref_path.exists():
            raise FileNotFoundError(
                f"real-xbox reference missing: {ref_path}. Capture it via "
                f"`xbe-harness capture-reference --xbe {manifest.id}` after "
                f"deploying to the real Xbox.")
        out_png.parent.mkdir(parents=True, exist_ok=True)
        out_png.write_bytes(ref_path.read_bytes())
        return out_png, "real-xbox"
    if kind == "math-derived":
        gen = spec["generator"]
        synthesize_expected_png(manifest, gen, out_png)
        return out_png, "math-derived"
    raise ValueError(f"unknown expected_results.kind: {kind}")


def signal_match_check(captured_png: Path, reference_png: Path,
                       threshold: int = 16,
                       min_signal_match_pct: float = 99.0) -> Tuple[bool, str]:
    """Sparse-signal sanity check: verify pixels that the reference
    declares as non-black ALSO match within tolerance in the
    captured image.

    Catches the failure mode that `compare-screenshots.py
    changed_pixels_pct` misses for sparse oracles like `mirror`:
    a renderer that fails to draw a 4x4 white block changes only 16
    pixels (0.0052%) — well under any reasonable percent gate — but
    the test purpose (verify the block IS drawn) is unmet.

    Returns (passed, reason). Reason includes the signal pixel count
    and the match percentage. The "signal" is defined as: pixels in
    the reference where (R + G + B) > 0 (i.e. not pure-black). For a
    pure-black reference (no signal pixels), returns (True, "no
    signal pixels in reference") so the check is a no-op.
    """
    from PIL import Image
    ref = Image.open(reference_png).convert("RGB")
    cap = Image.open(captured_png).convert("RGB")
    if cap.size != ref.size:
        # Resize captured down to reference's dimensions. For the
        # signal-match check use BOX (area-average) not LANCZOS so
        # boundary pixels of a sparse signal don't get filtered out
        # by sinc ringing. BOX preserves the average intensity of
        # source signal pixels — for a pure-white pixel under 2x
        # retina present, a 2x2 patch of (255,255,255) → (255,255,255).
        if (cap.size[0] >= ref.size[0] and cap.size[1] >= ref.size[1]):
            cap = cap.resize(ref.size, Image.BOX)
        elif (ref.size[0] >= cap.size[0] and ref.size[1] >= cap.size[1]):
            ref = ref.resize(cap.size, Image.BOX)
        else:
            return False, (f"size mismatch and neither dim is dominant: "
                           f"cap={cap.size} ref={ref.size}")
    # Vectorize: pull bytes once, work per-channel via bytearray ops.
    # Avoids 307200 .getpixel() calls per check (5 s/check → ~50 ms).
    ref_bytes = ref.tobytes()
    cap_bytes = cap.tobytes()
    n_pixels = ref.size[0] * ref.size[1]
    if len(ref_bytes) != n_pixels * 3 or len(cap_bytes) != n_pixels * 3:
        return False, (f"unexpected byte counts: ref={len(ref_bytes)} "
                       f"cap={len(cap_bytes)} expected={n_pixels * 3}")
    signal_total = 0
    signal_match = 0
    for off in range(0, n_pixels * 3, 3):
        r = ref_bytes[off]
        g = ref_bytes[off + 1]
        b = ref_bytes[off + 2]
        if r == 0 and g == 0 and b == 0:
            continue
        signal_total += 1
        cr = cap_bytes[off]
        cg = cap_bytes[off + 1]
        cb = cap_bytes[off + 2]
        if (abs(r - cr) <= threshold and
                abs(g - cg) <= threshold and
                abs(b - cb) <= threshold):
            signal_match += 1
    if signal_total == 0:
        return True, "no signal pixels in reference"
    pct = (signal_match / signal_total) * 100.0
    passed = pct >= min_signal_match_pct
    return passed, (f"signal_total={signal_total} signal_match={signal_match} "
                    f"signal_match_pct={pct:.4f} (gate ≥ {min_signal_match_pct})")


def frame_quality_score(captured_png: Path, reference_png: Path,
                        threshold: int = 16) -> Tuple[float, float, str]:
    """In-process frame quality scan. Returns:

        (signal_match_pct, total_match_pct, reason)

    `signal_match_pct` is the same metric `signal_match_check` returns
    — fraction of non-black-in-reference pixels that match within
    `threshold` in the captured.

    `total_match_pct` is the fraction of *all* pixels matching the
    reference within `threshold`. Used as a tiebreak by
    `xbe_orchestrator.run_matrix`'s frame selector: when multiple
    candidate frames tie at 100% signal_match_pct (the dashboard
    happens to have white pixels where the diag XBE's signal pixels
    land — see mirror.0022 vs mirror.0062 in
    `benchmark-runs/xbe-rotation-20260520T170630Z/`), the candidate
    with the higher total_match_pct is the actual diag-render frame
    because its non-signal (background) pixels also match the
    reference. Without this tiebreak, the first-in-glob frame wins
    even when it's a dashboard frame with happenstance signal-match.

    Runs in-process via PIL (no subprocess invocation per frame).
    A 640x480 image takes ~50 ms on an M3 Ultra, so the full 138
    candidates for a mirror rotation costs ~7 s — acceptable.
    """
    from PIL import Image
    ref = Image.open(reference_png).convert("RGB")
    cap = Image.open(captured_png).convert("RGB")
    if cap.size != ref.size:
        if (cap.size[0] >= ref.size[0] and cap.size[1] >= ref.size[1]):
            cap = cap.resize(ref.size, Image.BOX)
        elif (ref.size[0] >= cap.size[0] and ref.size[1] >= cap.size[1]):
            ref = ref.resize(cap.size, Image.BOX)
        else:
            return -1.0, -1.0, (f"size mismatch and neither dim is "
                                f"dominant: cap={cap.size} ref={ref.size}")
    ref_bytes = ref.tobytes()
    cap_bytes = cap.tobytes()
    n_pixels = ref.size[0] * ref.size[1]
    if len(ref_bytes) != n_pixels * 3 or len(cap_bytes) != n_pixels * 3:
        return -1.0, -1.0, (f"unexpected byte counts: ref={len(ref_bytes)} "
                            f"cap={len(cap_bytes)} expected={n_pixels * 3}")
    signal_total = 0
    signal_match = 0
    total_match = 0
    for off in range(0, n_pixels * 3, 3):
        r = ref_bytes[off]
        g = ref_bytes[off + 1]
        b = ref_bytes[off + 2]
        cr = cap_bytes[off]
        cg = cap_bytes[off + 1]
        cb = cap_bytes[off + 2]
        pixel_match = (abs(r - cr) <= threshold and
                       abs(g - cg) <= threshold and
                       abs(b - cb) <= threshold)
        if pixel_match:
            total_match += 1
        if not (r == 0 and g == 0 and b == 0):
            signal_total += 1
            if pixel_match:
                signal_match += 1
    sig_pct = (100.0 if signal_total == 0
               else (signal_match / signal_total) * 100.0)
    tot_pct = (total_match / n_pixels) * 100.0
    return sig_pct, tot_pct, (
        f"signal_total={signal_total} signal_match={signal_match} "
        f"signal_match_pct={sig_pct:.4f} "
        f"total_match_pct={tot_pct:.4f}")


def compare(captured_png: Path, reference_png: Path, out_dir: Path,
            crop: str = "0,0,640,480", threshold: int = 0,
            max_changed_pct: float = 0.0,
            resize: str = "smaller",
            min_signal_match_pct: float = 99.0) -> CompareVerdict:
    """Run compare-screenshots.py and enforce a PASS/FAIL verdict.

    `threshold`: per-channel absolute byte diff considered "the same"
                 pixel (0 = exact, 8 = matches metal-gl-compare default).
    `max_changed_pct`: fraction of pixels (out of 100) allowed to
                       exceed the per-channel threshold. 0.0 = exact
                       pixel match required; 1.0 = up to 1% pixel
                       drift allowed.

    NOTE: `compare-screenshots.py` always exits 0; it just prints
    `changed_pixels_pct=N`. We parse that and apply our own gate.

    Default `resize=smaller` because xemu's Metal in-renderer screenshot
    captures at the drawable's native dimensions (often the host-scaled
    display, e.g. 1280x960 at retina), while our reference PNGs are at
    native guest 640x480; the resize-smaller policy preserves the
    comparison's pixel mapping at native resolution."""
    if not COMPARE_SCRIPT.exists():
        return CompareVerdict("infra-error", -1, captured_png, reference_png,
                              out_dir, "", "compare-screenshots.py missing",
                              notes="missing-compare-script")
    if not captured_png.exists():
        return CompareVerdict("infra-error", -1, captured_png, reference_png,
                              out_dir, "", f"no captured PNG: {captured_png}",
                              notes="no-captured-png")
    if not reference_png.exists():
        return CompareVerdict("infra-error", -1, captured_png, reference_png,
                              out_dir, "", f"no reference PNG: {reference_png}",
                              notes="no-reference-png")
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, str(COMPARE_SCRIPT),
        str(reference_png), str(captured_png),
        "--crop", crop,
        "--out-dir", str(out_dir),
        "--threshold", str(threshold),
        "--resize", resize,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        return CompareVerdict(
            status="infra-error", rc=proc.returncode,
            captured=captured_png, reference=reference_png,
            out_dir=out_dir, stdout=proc.stdout, stderr=proc.stderr,
            notes="compare-script-error",
        )
    pct = _parse_changed_pct(proc.stdout)
    if pct is None:
        return CompareVerdict(
            status="infra-error", rc=proc.returncode,
            captured=captured_png, reference=reference_png,
            out_dir=out_dir, stdout=proc.stdout, stderr=proc.stderr,
            notes="could not parse changed_pixels_pct from compare output",
        )
    pct_pass = pct <= max_changed_pct

    # Sparse-signal sanity check: the percent gate alone passes a
    # renderer that fails to draw a sparse signal (mirror's 4x4 white
    # block = 0.0052% changed if missing, well under 0.5%). Verify
    # that pixels declared non-black in the reference ALSO match in
    # the captured image. See xbe_compare.signal_match_check docstring.
    # Use a wider threshold here (140) because BOX-downsampling a
    # retina-scaled capture blends boundary pixels by up to 50%
    # (255 * 0.5 = 128) — a per-channel diff up to ~140 is normal
    # for the boundary pixels of an integer-aligned signal patch.
    sig_pass, sig_notes = signal_match_check(
        captured_png, reference_png,
        threshold=140,
        min_signal_match_pct=min_signal_match_pct)

    if pct_pass and sig_pass:
        status = "pass"
        notes = f"changed_pixels_pct={pct:.4f} | {sig_notes}"
    else:
        status = "fail"
        why = []
        if not pct_pass:
            why.append(f"changed_pixels_pct={pct:.4f} > {max_changed_pct}")
        if not sig_pass:
            why.append(f"signal-check FAIL ({sig_notes})")
        notes = " | ".join(why)
    return CompareVerdict(
        status=status, rc=proc.returncode,
        captured=captured_png, reference=reference_png,
        out_dir=out_dir, stdout=proc.stdout, stderr=proc.stderr,
        notes=notes,
    )


def _parse_changed_pct(stdout: str) -> Optional[float]:
    """Find `changed_pixels_pct=N.NNNN` line, return float or None."""
    for line in stdout.splitlines():
        if line.startswith("changed_pixels_pct="):
            try:
                return float(line.split("=", 1)[1].strip())
            except ValueError:
                return None
    return None
