"""
native-quad-tri-depth — math-derived expected oracle (v0.3, no FLAT-quad).

640x480 RGBA front buffer. The XBE renders THREE stripe-passes per
frame (the original v0.2 four-pass design's FLAT OP_QUADS stripe was
removed in v0.3 after it exposed a real Metal renderer gap; see
main.c "Why no FLAT-quad stripe" + decision-log "2026-05-20 (evening,
late): native-quad-tri-depth XBE caught Metal FLAT-quad gap"):

  PASS 1 (TOP    half y in [0,   240), OP_QUADS,     SMOOTH)
  PASS 2 (BOTTOM half y in [240, 400), OP_TRIANGLES, SMOOTH)
  PASS 3 (BOTTOM half y in [400, 480), OP_TRIANGLES, FLAT,
                                                     FLAT_SHADE_OP=VERTEX_FIRST)

Each pass paints uniform-color 160x80 cells in pure 0/255 RGB. The
math-derived oracle is the same 4x3 grid in both halves regardless
of which pass produced each cell: top half and bottom half are
byte-identical pixel-for-pixel.

Cell colors (row * 4 + col), positions identical in top and bottom
half (cell row 0 spans y [0,80) in the top half and y [240,320) in
the bottom half):

  Row 0:           RED      GREEN    BLUE     WHITE
  Row 1:           YELLOW   CYAN     RED      GREEN
  Row 2:           BLUE     WHITE    YELLOW   CYAN

Rows 0-1 are SMOOTH on both halves. Row 2 is SMOOTH on the top half
(OP_QUADS) and FLAT/VERTEX_FIRST on the bottom half (OP_TRIANGLES).
Both rendering paths must produce the same uniform color per cell.

Catches:

  - SMOOTH stripes (rows 0-1) top half vs bottom half mismatch:
    NATIVE_QUAD path rasterizes / interpolates differently than
    NATIVE_TRI_DEPTH reference.
  - FLAT triangle stripe (bottom half row 2) wrong color:
    NATIVE_TRI_DEPTH's first-vertex-provoking path selected the
    wrong vertex (would show BLACK distractor).
  - Silent bypass fall-through: caught by `required_counters_min` in
    manifest.json, not by this pixel oracle.

FLAT-quad coverage is NOT tested here; it is filed as a follow-up
`flat-quad-propagation` XBE pending the Metal-renderer slice that
adds CPU-side flat-color propagation for OP_QUADS/QUAD_STRIP.

The XBE source-file header (`main.c`) carries the full math
derivation. This module encodes the same math in Python; a reviewer
can confirm the two agree per diagnostic-xbe-plan.md v2 §2.5.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
HALF_H = HEIGHT // 2       # 240
CELL_W = WIDTH // 4        # 160
CELL_H = HALF_H // 3       # 80

# Cell colors in RGBA order (alpha always opaque). Mirrors
# k_cell_rgb[][] in main.c. Indexed as row * 4 + col.
CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  # (0,0) RED
    (0x00, 0xFF, 0x00, 0xFF),  # (0,1) GREEN
    (0x00, 0x00, 0xFF, 0xFF),  # (0,2) BLUE
    (0xFF, 0xFF, 0xFF, 0xFF),  # (0,3) WHITE
    (0xFF, 0xFF, 0x00, 0xFF),  # (1,0) YELLOW
    (0x00, 0xFF, 0xFF, 0xFF),  # (1,1) CYAN
    (0xFF, 0x00, 0x00, 0xFF),  # (1,2) RED
    (0x00, 0xFF, 0x00, 0xFF),  # (1,3) GREEN
    (0x00, 0x00, 0xFF, 0xFF),  # (2,0) BLUE
    (0xFF, 0xFF, 0xFF, 0xFF),  # (2,1) WHITE
    (0xFF, 0xFF, 0x00, 0xFF),  # (2,2) YELLOW
    (0x00, 0xFF, 0xFF, 0xFF),  # (2,3) CYAN
)


def _cell_for(x: int, y: int) -> int:
    """Return the cell index (row * 4 + col) covering pixel (x, y).

    Cells are 160 wide x 80 tall and arranged identically in the top
    half (rows 0..239) and bottom half (rows 240..479). Both halves
    use the same color table; this function returns the same cell
    index for (x, y) and (x, y + 240) when 0 <= y < 240.
    """
    col = x // CELL_W
    if y < HALF_H:
        row = y // CELL_H
    else:
        row = (y - HALF_H) // CELL_H
    return row * 4 + col


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Math-derived expected output, byte-identical in both halves.

    Returns a width*height*4 RGBA buffer where each pixel's color is
    determined solely by the cell it lies in. Used by every renderer
    cell (gl/metal/real-xbox at any recipe) because the cells use
    pure 0/255 components and never go through any path that would
    transform them differently across renderers.
    """
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"native-quad-tri-depth expected only {WIDTH}x{HEIGHT}, "
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
