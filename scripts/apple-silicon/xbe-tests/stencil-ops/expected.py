"""
stencil-ops — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells, each
filled with one of 8 saturated colors corresponding to the post-op
stencil value matching the EQUAL probe (see main.c for the per-cell
op + expected stencil mapping):

  Row 0 (y 0..239):   KEEP=RED    ZERO=GREEN  REPLACE=BLUE   INCRSAT=WHITE
  Row 1 (y 240..479): DECRSAT=YEL CYAN=INVERT MAGENTA=INCR   RED=DECR

Every cell color is a pure 0/255 RGB cube corner so the result is
byte-exact across renderers regardless of any display-side gamma table.

A cell renders BLACK when the renderer applied the wrong stencil op
(post-op stencil != expected). The math-derived oracle declares every
cell as its expected color; a black cell is a renderer bug.

The XBE source-file header (`main.c`) carries the full math
derivation. This module encodes the same color table in Python.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  # 0 KEEP    → RED
    (0x00, 0xFF, 0x00, 0xFF),  # 1 ZERO    → GREEN
    (0x00, 0x00, 0xFF, 0xFF),  # 2 REPLACE → BLUE
    (0xFF, 0xFF, 0xFF, 0xFF),  # 3 INCRSAT → WHITE
    (0xFF, 0xFF, 0x00, 0xFF),  # 4 DECRSAT → YELLOW
    (0x00, 0xFF, 0xFF, 0xFF),  # 5 INVERT  → CYAN
    (0xFF, 0x00, 0xFF, 0xFF),  # 6 INCR    → MAGENTA
    (0xFF, 0x00, 0x00, 0xFF),  # 7 DECR    → RED (reused)
)


def _cell_for(x: int, y: int) -> int:
    col = x // CELL_W
    row = y // CELL_H
    return row * 4 + col


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"stencil-ops expected only {WIDTH}x{HEIGHT}, "
            f"got {width}x{height}"
        )
    rgba = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            r, g, b, a = CELL_RGBA[_cell_for(x, y)]
            o = (y * width + x) * 4
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
    spec = importlib.util.spec_from_file_location(
        "oracle_client", client_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(
            f"could not load oracle-client.py from {client_path}")
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
