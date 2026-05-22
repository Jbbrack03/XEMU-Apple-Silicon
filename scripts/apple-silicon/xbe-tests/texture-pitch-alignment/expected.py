"""
texture-pitch-alignment — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells, each
sampled from a uniform-color LU_IMAGE_A8R8G8B8 texture under a
different `(IMAGE_RECT.width, IMAGE_RECT.height, TEXCTL1.IMAGE_PITCH)`
configuration. Every per-cell active rectangle is filled with the
cell's target cube-corner color; the surrounding VRAM padding is
filled with sentinel gray 0x808080. If the renderer honors both
registers (pitch + image-rect) correctly, NEAREST sampling with
CLAMP_TO_EDGE wrap lands every fragment in the active rectangle and
the cell renders as a uniform target color.

The visible expected output is therefore identical to
`texture-format-sweep`'s 4x2 cube-corner grid even though the
mechanism under test is different:

  Row 0 (y 0..239):    RED      GREEN    BLUE     WHITE
  Row 1 (y 240..479):  YELLOW   CYAN     MAGENTA  RED

If pitch is silently ignored, cells 1-7 (everything except cell 0,
the only true `pitch == width * bytes_per_pixel` baseline) will
visibly leak sentinel-gray; cell 4 uses the smallest non-baseline
pitch padding (4 extra bytes per row) so the strict byte-exact gate
also catches the smallest-stride bug.

If IMAGE_RECT.height is silently rounded up to a power of two (or
otherwise ignored), the trailing physical-padding rows below the
active rectangle (kept sentinel-grey by main.c -- see EXTRA_PAD_ROWS)
will be sampled and the cell visibly leaks gray.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  # 0 RED      pitch == w*bpp (only true baseline)
    (0x00, 0xFF, 0x00, 0xFF),  # 1 GREEN    pitch 4x w*bpp
    (0x00, 0x00, 0xFF, 0xFF),  # 2 BLUE     wider active, same pitch
    (0xFF, 0xFF, 0xFF, 0xFF),  # 3 WHITE    non-pow2 (5x3) dims
    (0xFF, 0xFF, 0x00, 0xFF),  # 4 YELLOW   smallest pad: pitch = w*bpp + 4
    (0x00, 0xFF, 0xFF, 0xFF),  # 5 CYAN     pitch 8x w*bpp
    (0xFF, 0x00, 0xFF, 0xFF),  # 6 MAGENTA  odd width=7 + pitch=64
    (0xFF, 0x00, 0x00, 0xFF),  # 7 RED      tiny (3x3) active
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
            f"texture-pitch-alignment expected only {WIDTH}x{HEIGHT}, "
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
