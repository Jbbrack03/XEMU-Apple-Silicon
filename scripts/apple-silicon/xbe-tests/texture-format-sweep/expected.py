"""
texture-format-sweep — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells, each
sampled from a uniform 4x4 texture. v0.1 covers 4 linear 32-bit
formats (A8R8G8B8, X8R8G8B8, A8B8G8R8, B8G8R8A8) on the first row;
the second row repeats A8R8G8B8 at 4 more cube-corner colors. All
sampled colors are saturated 0/255 cube corners; the NEAREST filter
on a uniformly-filled 4x4 texture should produce byte-exact output.

  Row 0 (y 0..239):    RED      GREEN    BLUE     WHITE
  Row 1 (y 240..479):  YELLOW   CYAN     MAGENTA  RED
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  # 0 RED      A8R8G8B8
    (0x00, 0xFF, 0x00, 0xFF),  # 1 GREEN    X8R8G8B8
    (0x00, 0x00, 0xFF, 0xFF),  # 2 BLUE     A8B8G8R8
    (0xFF, 0xFF, 0xFF, 0xFF),  # 3 WHITE    B8G8R8A8
    (0xFF, 0xFF, 0x00, 0xFF),  # 4 YELLOW   A8R8G8B8 (re)
    (0x00, 0xFF, 0xFF, 0xFF),  # 5 CYAN     A8R8G8B8 (re)
    (0xFF, 0x00, 0xFF, 0xFF),  # 6 MAGENTA  A8R8G8B8 (re)
    (0xFF, 0x00, 0x00, 0xFF),  # 7 RED      A8R8G8B8 (re)
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
            f"texture-format-sweep expected only {WIDTH}x{HEIGHT}, "
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
