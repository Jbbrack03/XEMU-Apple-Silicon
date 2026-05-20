"""
blend-matrix — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells, each
exercising one of 8 common NV2A (sfactor, dfactor, equation) blend
combinations. The XBE's source-file header carries the full math
derivation; this module encodes the same per-cell expected colors so
the harness can pixel-compare the captured frame against a known
oracle.

Cell colors:

  Row 0 (y 0..239):   RED       GREEN     YELLOW    CYAN
  Row 1 (y 240..479): MAGENTA   BLUE      RED       GREEN

Per the §4.9 oracle in main.c:

  Cell 0 ONE/ZERO/ADD              -> RED
  Cell 1 ZERO/ONE/ADD              -> GREEN
  Cell 2 ONE/ONE/ADD               -> YELLOW (RED+GREEN)
  Cell 3 ONE/ONE/REVERSE_SUBTRACT  -> CYAN   (WHITE-RED)
  Cell 4 ONE/ONE/SUBTRACT          -> MAGENTA(WHITE-GREEN)
  Cell 5 SRC_ALPHA/ONE_MINUS_SRC_ALPHA/ADD α=255 -> BLUE
  Cell 6 DST_COLOR/ZERO/ADD        -> RED    (MAGENTA*YELLOW)
  Cell 7 ZERO/SRC_COLOR/ADD        -> GREEN  (CYAN*YELLOW)
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

# Cell colors in RGBA order (alpha always opaque). Mirrors k_cells
# in main.c after the per-cell blend math + framebuffer [0,1] clamp +
# 8-bit quantization. All values are saturated 0/255 endpoints; no
# precision tolerance needed.
CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  # 0 RED     ONE/ZERO/ADD
    (0x00, 0xFF, 0x00, 0xFF),  # 1 GREEN   ZERO/ONE/ADD
    (0xFF, 0xFF, 0x00, 0xFF),  # 2 YELLOW  ONE/ONE/ADD
    (0x00, 0xFF, 0xFF, 0xFF),  # 3 CYAN    ONE/ONE/REVERSE_SUBTRACT
    (0xFF, 0x00, 0xFF, 0xFF),  # 4 MAGENTA ONE/ONE/SUBTRACT
    (0x00, 0x00, 0xFF, 0xFF),  # 5 BLUE    SRC_ALPHA/ONE_MINUS_SRC_ALPHA/ADD
    (0xFF, 0x00, 0x00, 0xFF),  # 6 RED     DST_COLOR/ZERO/ADD modulate
    (0x00, 0xFF, 0x00, 0xFF),  # 7 GREEN   ZERO/SRC_COLOR/ADD modulate
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
            f"blend-matrix expected only {WIDTH}x{HEIGHT}, "
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
