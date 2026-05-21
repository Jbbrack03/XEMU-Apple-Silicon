"""
flat-quad-propagation -- math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells.
Each cell is rendered as OP_QUADS with FLAT shading. NV2A's OP_QUADS
flat-shade rule: vertex 3 (LAST) is always the provoking vertex.
Vertices 0/1/2 carry BLACK distractor; vertex 3 carries the
EXPECTED color. With Metal's task #13 CPU-side flat-quad color
propagation, each cell renders its expected color uniformly.

  Row 0:  RED     GREEN   BLUE    WHITE
  Row 1:  YELLOW  CYAN    MAGENTA RED (repeat -- pure 0/255 only)
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4   # 160
CELL_H = HEIGHT // 2  # 240

CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  # 0 RED
    (0x00, 0xFF, 0x00, 0xFF),  # 1 GREEN
    (0x00, 0x00, 0xFF, 0xFF),  # 2 BLUE
    (0xFF, 0xFF, 0xFF, 0xFF),  # 3 WHITE
    (0xFF, 0xFF, 0x00, 0xFF),  # 4 YELLOW
    (0x00, 0xFF, 0xFF, 0xFF),  # 5 CYAN
    (0xFF, 0x00, 0xFF, 0xFF),  # 6 MAGENTA
    (0xFF, 0x00, 0x00, 0xFF),  # 7 RED (repeat; pure 0/255 only)
)


def _cell_for(x: int, y: int) -> int:
    col = x // CELL_W
    row = y // CELL_H
    return row * 4 + col


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"flat-quad-propagation expected only {WIDTH}x{HEIGHT}, "
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
