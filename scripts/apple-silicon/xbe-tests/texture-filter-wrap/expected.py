"""
texture-filter-wrap — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells.
Each cell samples a 4x4 quadrant texture with NEAREST filter at a
constant UV per cell. Row 0 uses wrap=CLAMP_TO_EDGE with in-range
UVs; row 1 uses wrap=REPEAT with UVs offset by +1.0 in U so the
wrap brings sampling back to the same texels as row 0. Both rows
render the same R/G/B/W pattern.

  Row 0 (CLAMP_TO_EDGE):  RED   GREEN   BLUE    WHITE
  Row 1 (REPEAT):         RED   GREEN   BLUE    WHITE
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  # 0 RED   row0 CLAMP_TO_EDGE
    (0x00, 0xFF, 0x00, 0xFF),  # 1 GREEN row0 CLAMP_TO_EDGE
    (0x00, 0x00, 0xFF, 0xFF),  # 2 BLUE  row0 CLAMP_TO_EDGE
    (0xFF, 0xFF, 0xFF, 0xFF),  # 3 WHITE row0 CLAMP_TO_EDGE
    (0xFF, 0x00, 0x00, 0xFF),  # 4 RED   row1 WRAP
    (0x00, 0xFF, 0x00, 0xFF),  # 5 GREEN row1 WRAP
    (0x00, 0x00, 0xFF, 0xFF),  # 6 BLUE  row1 WRAP
    (0xFF, 0xFF, 0xFF, 0xFF),  # 7 WHITE row1 WRAP
)


def _cell_for(x: int, y: int) -> int:
    col = x // CELL_W
    row = y // CELL_H
    return row * 4 + col


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Math-derived expected output. Uniform-color cells; pure 0/255
    components survive display gamma byte-exact."""
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"texture-filter-wrap expected only {WIDTH}x{HEIGHT}, "
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
    """CLI helper: write the math-derived expected PNG to PATH."""
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
