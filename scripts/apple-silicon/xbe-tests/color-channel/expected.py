"""
color-channel — math-derived expected oracle.

640x480 RGBA front buffer:
  cols  [0, 160) → opaque red    (0xFFFF0000 in #AARRGGBB)
  cols  [160, 320) → opaque green (0xFF00FF00)
  cols  [320, 480) → opaque blue  (0xFF0000FF)
  cols  [480, 640) → opaque white (0xFFFFFFFF)
  All rows.

The XBE renders four full-height vertical strips with these colors
via TYPE_F (4-float) DIFFUSE attribute. Catches the SC2 "wrong colors"
symptom — a B/R swap in the front-buffer publish path or in the
DIFFUSE → COLOR passthrough would invert the per-strip color order.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
STRIPS = (
    (0,   160, (0xFF, 0x00, 0x00, 0xFF)),  # red,   RGBA
    (160, 320, (0x00, 0xFF, 0x00, 0xFF)),  # green
    (320, 480, (0x00, 0x00, 0xFF, 0xFF)),  # blue
    (480, 640, (0xFF, 0xFF, 0xFF, 0xFF)),  # white
)


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"color-channel expected only {WIDTH}x{HEIGHT}, got {width}x{height}"
        )
    rgba = bytearray(width * height * 4)
    for y in range(height):
        row_off = y * width * 4
        for (x0, x1, (r, g, b, a)) in STRIPS:
            for x in range(x0, x1):
                o = row_off + x * 4
                rgba[o + 0] = r
                rgba[o + 1] = g
                rgba[o + 2] = b
                rgba[o + 3] = a
    return bytes(rgba)


def to_png(path: str) -> None:
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
