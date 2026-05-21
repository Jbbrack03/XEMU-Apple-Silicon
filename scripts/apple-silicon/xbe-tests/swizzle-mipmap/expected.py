"""
swizzle-mipmap — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells.
Cells 0..6 sample mip 0..6 of a 64x64 SZ_A8R8G8B8 mip chain; cell 7
is unused (BLACK clear).

Each cell paints a 2x2 sub-grid of quadrant colors. The four
quadrant colors per mip N are derived as:

    quad_rgb[Q] = (base_rgb[Q] * tint[N] + 127) // 255

where:
    base_rgb[Q0] = (255,   0,   0)    # red
    base_rgb[Q1] = (  0, 255,   0)    # green
    base_rgb[Q2] = (  0,   0, 255)    # blue
    base_rgb[Q3] = (255, 255,   0)    # yellow
    tint[0..6] = 0xFF, 0xDF, 0xBF, 0x9F, 0x7F, 0x5F, 0x3F

Mips 5 (2x2) and 6 (1x1) are degenerate: at 1x1, all four quadrants
collapse to a single texel. The XBE draws all 4 quadrants for cell
6 sampling the single Q0 color (the same color shows in all four
quadrants of that cell).

Quadrant layout per cell:
    +-------+-------+
    |  Q0   |  Q1   |   y < CELL_H/2
    +-------+-------+
    |  Q2   |  Q3   |   y >= CELL_H/2
    +-------+-------+
       x<W/2  x>=W/2
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
GRID_COLS = 4
GRID_ROWS = 2
CELL_W = WIDTH // GRID_COLS    # 160
CELL_H = HEIGHT // GRID_ROWS   # 240

CELL_MIP = (0, 1, 2, 3, 4, 5, 6, -1)  # -1 = unused

BASE_RGB = (
    (0xFF, 0x00, 0x00),  # Q0 red
    (0x00, 0xFF, 0x00),  # Q1 green
    (0x00, 0x00, 0xFF),  # Q2 blue
    (0xFF, 0xFF, 0x00),  # Q3 yellow
)
TINT = (0xFF, 0xDF, 0xBF, 0x9F, 0x7F, 0x5F, 0x3F)


def _tint(channel: int, tint: int) -> int:
    return (channel * tint + 127) // 255


def _mip_dim(mip: int) -> int:
    """Width = height for our square mip chain. Mip 0 = 64; halves each
    level; minimum 1 at mip 6."""
    w = 64 >> mip
    return max(1, w)


def _texel_for_quadrant_uv(w: int, q: int) -> int:
    """Linear texel index sampled by NEAREST at the quadrant's UV.
    Quadrant centers in normalized space: 0.25 (left half) or 0.75
    (right half). For texture width w, NEAREST(u) = floor(u * w)
    clipped to [0, w-1]. Same scheme for v."""
    qx = q & 1
    qy = q >> 1
    u = 0.75 if qx else 0.25
    v = 0.75 if qy else 0.25
    tx = min(w - 1, max(0, int(u * w)))
    ty = min(w - 1, max(0, int(v * w)))
    return tx, ty


def _quad_for_texel(w: int, tx: int, ty: int) -> int:
    """Which source quadrant Q (0..3) does texel (tx, ty) come from?
    Mirrors the XBE-side fill: half_w = max(1, w//2); qx = (tx >= half_w)
    ? 1 : 0; quad = qy*2 + qx. For w == 1, all texels collapse to Q0."""
    if w == 1:
        return 0
    half = max(1, w // 2)
    qx = 1 if tx >= half else 0
    qy = 1 if ty >= half else 0
    return qy * 2 + qx


def _quad_rgba(mip: int, q: int) -> tuple[int, int, int, int]:
    """For cell (mip), quadrant Q painted by the XBE's small quad
    sampling UV (qx*0.5+0.25, qy*0.5+0.25). Resolve the sampled
    texel + the source quadrant Q' of that texel under the XBE's
    fill scheme, and return the tinted color of Q'."""
    if mip < 0 or mip >= len(TINT):
        return (0, 0, 0, 0xFF)
    w = _mip_dim(mip)
    tx, ty = _texel_for_quadrant_uv(w, q)
    src_q = _quad_for_texel(w, tx, ty)
    base = BASE_RGB[src_q]
    t = TINT[mip]
    return (_tint(base[0], t), _tint(base[1], t), _tint(base[2], t), 0xFF)


def _cell_for(x: int, y: int) -> tuple[int, int]:
    """Returns (cell_idx, quadrant_idx) for pixel (x, y).
    cell_idx 0..7; quadrant_idx 0..3."""
    col = x // CELL_W
    row = y // CELL_H
    cell_idx = row * GRID_COLS + col
    half_w = CELL_W // 2
    half_h = CELL_H // 2
    cx = x - col * CELL_W
    cy = y - row * CELL_H
    qx = 1 if cx >= half_w else 0
    qy = 1 if cy >= half_h else 0
    return cell_idx, qy * 2 + qx


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"swizzle-mipmap expected only {WIDTH}x{HEIGHT}, "
            f"got {width}x{height}"
        )
    # Precompute per-(cell, quadrant) RGBA so the pixel loop is O(1).
    cell_quad_rgba = {}
    for cell in range(GRID_CELLS := GRID_COLS * GRID_ROWS):
        mip = CELL_MIP[cell]
        for q in range(4):
            cell_quad_rgba[(cell, q)] = _quad_rgba(mip, q)

    rgba = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            cell, q = _cell_for(x, y)
            r, g, b, a = cell_quad_rgba[(cell, q)]
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
