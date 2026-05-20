"""
cmp-vertex-format — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells, each
filled with one of the 8 RGB cube corner colors. Each cell tests the
renderer's decoder of NV2A's CMP attribute format (3 signed-normalized
components packed into a 32-bit word, X bits 0-10, Y bits 11-21,
Z bits 22-31, with negative values clamped to -1 then the framebuffer
clamping to [0, 255]).

Cell colors (row * 4 + col), positions:

  Row 0 (y 0..239):    WHITE   YELLOW   MAGENTA  CYAN
  Row 1 (y 240..479):  RED     GREEN    BLUE     BLACK

The XBE source-file header (`main.c`) carries the full math
derivation. This module encodes the same color table in Python; a
reviewer can confirm the two agree per diagnostic-xbe-plan.md v2 §2.5.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

# Cell colors in RGBA order (alpha always opaque). Mirrors the
# k_cmp_normals encoded normals in main.c after the renderer's CMP
# decoder + framebuffer [0, 1] clamp + 8-bit quantization.
CELL_RGBA = (
    (0xFF, 0xFF, 0xFF, 0xFF),  # (0,0) WHITE   from (+1,+1,+1)
    (0xFF, 0xFF, 0x00, 0xFF),  # (0,1) YELLOW  from (+1,+1,-1)
    (0xFF, 0x00, 0xFF, 0xFF),  # (0,2) MAGENTA from (+1,-1,+1)
    (0x00, 0xFF, 0xFF, 0xFF),  # (0,3) CYAN    from (-1,+1,+1)
    (0xFF, 0x00, 0x00, 0xFF),  # (1,0) RED     from (+1,-1,-1)
    (0x00, 0xFF, 0x00, 0xFF),  # (1,1) GREEN   from (-1,+1,-1)
    (0x00, 0x00, 0xFF, 0xFF),  # (1,2) BLUE    from (-1,-1,+1)
    (0x00, 0x00, 0x00, 0xFF),  # (1,3) BLACK   from (-1,-1,-1)
)


def _cell_for(x: int, y: int) -> int:
    """Return the cell index (row * 4 + col) covering pixel (x, y)."""
    col = x // CELL_W
    row = y // CELL_H
    return row * 4 + col


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Math-derived expected output. Uniform-color cells; pure 0/255
    components survive display gamma byte-exact."""
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"cmp-vertex-format expected only {WIDTH}x{HEIGHT}, "
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
