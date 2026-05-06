"""
depth-floor — math-derived expected oracle (v0.2 saturated-colors).

640x480 RGBA front buffer:
  - Top half  (rows 0-239)  → opaque white  (255, 255, 255, 255)
  - Bottom half (rows 240-479) → opaque blue (0, 0, 255, 255)

The XBE renders a full-screen white floor at z=0.5, then a blue
wall covering the bottom half at z=0.0 (closer). With depth test
enabled (LEQUAL), the wall wins over the back floor pixels. All
values are 0/255 — byte-exact across renderers regardless of any
display-side gamma table.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
SPLIT_Y = 240


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"depth-floor expected only {WIDTH}x{HEIGHT}, got {width}x{height}"
        )
    rgba = bytearray(width * height * 4)
    # Top half: opaque white
    for y in range(SPLIT_Y):
        row_off = y * width * 4
        for x in range(width):
            o = row_off + x * 4
            rgba[o + 0] = 0xFF
            rgba[o + 1] = 0xFF
            rgba[o + 2] = 0xFF
            rgba[o + 3] = 0xFF
    # Bottom half: opaque blue
    for y in range(SPLIT_Y, height):
        row_off = y * width * 4
        for x in range(width):
            o = row_off + x * 4
            rgba[o + 0] = 0x00
            rgba[o + 1] = 0x00
            rgba[o + 2] = 0xFF
            rgba[o + 3] = 0xFF
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
