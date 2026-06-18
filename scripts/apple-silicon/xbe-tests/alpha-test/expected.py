"""
alpha-test — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells, each
exercising one NV2A alpha-test (func, ref) combination. The XBE's
source-file header carries the full per-cell derivation (verified
against hw/xbox/nv2a/nv2a_regs.h, pgraph.c SET_ALPHA_FUNC/REF, and
glsl/psh.c:1510-1530 alpha-test codegen); this module encodes the same
per-cell expected colors so the harness can pixel-compare the captured
frame against a known oracle.

Per cell: a BLUE background quad is drawn with alpha test OFF, then a
RED quad (per-cell alpha A) is drawn with alpha test ON (cell func,
ref=0x80). RED survives only when the alpha test PASSES; otherwise it
is discarded and the BLUE background shows through.

Renderer semantics (glsl/psh.c): fragAlpha = round(fragColor.a * 255);
the fragment passes when (fragAlpha <OP> alphaRef) is true, where
alphaRef is the raw 8-bit ref and <OP> is the C operator for the
0-indexed PshAlphaFunc (NEVER=0..ALWAYS=7). ALWAYS always passes,
NEVER always discards.

  Cell  Func          A      Comparison (fragAlpha OP 128)  Result
  ----  ------------  -----  -----------------------------  ------
  0     NEVER  (0)    255    unconditional discard          BLUE
  1     ALWAYS (7)    0      no test (always pass)          RED
  2     LESS   (1)    64     64  < 128  = true              RED
  3     LESS   (1)    192    192 < 128  = false             BLUE
  4     GEQUAL (6)    128    128 >= 128 = true              RED
  5     GREATER(4)    128    128 >  128 = false             BLUE
  6     EQUAL  (2)    128    128 == 128 = true              RED
  7     NOTEQUAL(5)   128    128 != 128 = false             BLUE

  Row 0 (y 0..239):   BLUE  RED   RED   BLUE
  Row 1 (y 240..479): RED   BLUE  RED   BLUE
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

RED = (0xFF, 0x00, 0x00, 0xFF)
BLUE = (0x00, 0x00, 0xFF, 0xFF)

# Cell colors in RGBA order. RED = alpha test passed (foreground RED
# written); BLUE = alpha test discarded (background BLUE survives).
# Mirrors k_cells in main.c after the per-cell alpha-test resolution.
# Both endpoints are saturated 0/255 components -> byte-exact across
# renderers regardless of display gamma; no precision tolerance needed.
CELL_RGBA = (
    BLUE,  # 0 NEVER    A=0xFF -> discard
    RED,   # 1 ALWAYS   A=0x00 -> pass
    RED,   # 2 LESS     A=0x40 (64  < 128) -> pass
    BLUE,  # 3 LESS     A=0xC0 (192 < 128) -> discard
    RED,   # 4 GEQUAL   A=0x80 (128 >= 128) -> pass
    BLUE,  # 5 GREATER  A=0x80 (128 >  128) -> discard
    RED,   # 6 EQUAL    A=0x80 (128 == 128) -> pass
    BLUE,  # 7 NOTEQUAL A=0x80 (128 != 128) -> discard
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
            f"alpha-test expected only {WIDTH}x{HEIGHT}, "
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
