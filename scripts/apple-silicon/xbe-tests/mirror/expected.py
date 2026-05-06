"""
mirror — math-derived expected oracle.

640x480 RGBA front buffer:
  - All pixels: opaque black (R=G=B=0, A=255), EXCEPT
  - Pixels (318..321, 48..51), 4x4 block: opaque white
    (R=G=B=255, A=255).

The XBE renders a quad spanning window-coords (318, 48)-(322, 52)
with vertices at integer pixel corners. Under the d3d top-left fill
convention NV2A inherits, this exactly covers the half-open rectangle
[318, 322) x [48, 52) — i.e. the 16 pixels (318..321, 48..51).

This module is the audit oracle when no real-Xbox capture is
available. When a real-Xbox reference frame is captured under
docs/apple-silicon/xbox-real-references/mirror/, the captured frame
must equal what this generator produces; disagreement is an actionable
finding (either NV2A has a quirk we missed or this math is wrong).

Per `diagnostic-xbe-plan.md` §3.5, manifest entries reference this
module via `generator: "expected.py:default"`.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
RECT_X0 = 318
RECT_Y0 = 48
RECT_X1 = 322  # exclusive
RECT_Y1 = 52   # exclusive


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Returns a `width * height * 4` RGBA bytes buffer with the
    expected pattern. Flag keyword arguments are accepted but unused
    — the pattern is renderer-independent (real Xbox / xemu-GL /
    xemu-Metal must all produce identical output)."""
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"mirror expected only {WIDTH}x{HEIGHT}, got {width}x{height}"
        )
    rgba = bytearray(width * height * 4)
    # All-black, alpha 255.
    for i in range(0, len(rgba), 4):
        rgba[i + 3] = 0xFF
    # 4x4 white block at top-left (RECT_X0, RECT_Y0).
    for y in range(RECT_Y0, RECT_Y1):
        row_off = y * width * 4
        for x in range(RECT_X0, RECT_X1):
            o = row_off + x * 4
            rgba[o + 0] = 0xFF
            rgba[o + 1] = 0xFF
            rgba[o + 2] = 0xFF
            rgba[o + 3] = 0xFF
    return bytes(rgba)


def to_png(path: str) -> None:
    """CLI helper: write the math-derived expected PNG to PATH."""
    import importlib.util
    from pathlib import Path
    here = Path(__file__).resolve().parent
    client_path = here.parent.parent / "oracle-client.py"
    spec = importlib.util.spec_from_file_location("oracle_client", client_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load oracle-client.py from {client_path}")
    oc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oc)  # type: ignore[union-attr]
    oc.save_screenshot_png(default(), WIDTH, HEIGHT, path)


if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print("usage: expected.py <out.png>", file=sys.stderr)
        sys.exit(1)
    to_png(sys.argv[1])
    print(f"wrote {WIDTH}x{HEIGHT} expected PNG to {sys.argv[1]}")
