"""
image-blit — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row dashboard grid of 160x240
cells, each cell encoding one IMAGE_BLIT verdict from the
guest-side CPU oracle. The XBE pre-paints a 32x32 source buffer
with four 16x16 quadrants (RED top-left, GREEN top-right, BLUE
bottom-left, WHITE bottom-right), pre-fills each 64x64 destination
buffer with sentinel gray 0x808080, issues per-cell
`NV_IMAGE_BLIT` with SRCCOPY semantics, waits for GPU idle, reads
the destination back via `pb_agp_access`, and compares every pixel
against the expected blit result:

  - pixels inside the declared dst rect:
      expected = src[in_x + (x - out_x), in_y + (y - out_y)]
  - pixels outside the dst rect (but still inside the 64x64
    allocation): expected = sentinel.

Each cell renders green (0xFF00FF00) on PASS or red (0xFFFF0000) on
FAIL. The math-derived oracle is therefore a deterministic 4x2
grid of pure green cells — pure 0/255 channels that survive
display gamma byte-exact (same property `texture-format-sweep` /
`texture-pitch-alignment` rely on for their cube-corner-only
palettes).

Cell sweep (in_x, in_y, out_x, out_y, width, height):

  Cell  In       Out      WxH    Probes
  ----  -------  -------  -----  ----------------------------------
  0     (0,0)    (0,0)    8x8    smallest contiguous rect (RED)
  1     (0,0)    (0,0)    16x16  full RED quadrant
  2     (0,0)    (0,0)    32x32  all 4 quadrants
  3     (8,8)    (0,0)    8x8    in_x/in_y honored (RED only)
  4     (0,0)    (4,4)    8x8    out_x/out_y honored
  5     (4,4)    (8,8)    8x8    both offsets non-zero (RED)
  6     (0,0)    (0,0)    1x16   degenerate width (1-px column)
  7     (0,0)    (0,0)    16x1   degenerate height (1-px row)

Expected dashboard (when all 8 blits PASS):

  Row 0 (y 0..239):    GREEN GREEN GREEN GREEN
  Row 1 (y 240..479):  GREEN GREEN GREEN GREEN

The strict byte-exact compare gate trips the moment any cell
turns red — guaranteed by the green-vs-red 100% contrast.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

# Pure green; matches main.c's DASH_PASS = 0xFF00FF00.
PASS_RGBA = (0x00, 0xFF, 0x00, 0xFF)


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Math-derived expected output: 8 solid-green cells.

    The XBE's encoded verdict is per-cell, but since all 8 cells must
    PASS for the slice to be considered green, the expected pattern is
    a uniform-green frame. A single failing cell flips that region to
    pure red and trips the harness's `min_signal_match_pct = 97.0`
    gate (red != green at 100% of the cell's pixels means at least
    one of the 8 cells contributes 12.5% of the frame as mismatched
    -- well above the 3.0% `max_changed_pct` gate).
    """
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"image-blit expected only {WIDTH}x{HEIGHT}, got {width}x{height}"
        )
    r, g, b, a = PASS_RGBA
    rgba = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
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
