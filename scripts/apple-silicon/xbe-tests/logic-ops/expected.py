"""
logic-ops — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 4-row grid (cell_w=160, cell_h=120),
one cell per NV2A color logic op. Each cell's expected color is the
bitwise result of `SRC <op> DST`:

  SRC = (R=0x40, G=0xC0, B=0x80, A=0xFF)
  DST = (R=0x80, G=0x80, B=0x80, A=0xFF)  (uniform mid-gray clear)

Op enum + per-channel math is encoded in OP_TABLE below; see
main.c source-file header for the cell-position-to-op mapping.

xemu-GL and xemu-Metal do NOT implement logic ops -- both treat the
rasterizer as COPY regardless of NV2A_SET_LOGIC_OP_*. The manifest
declares expected_fail_renderers=["metal","gl"] so the rotation
isn't gated on the missing feature; this oracle remains the spec
for what each renderer needs when logic-op support is implemented.
Real Xbox hardware is expected to PASS unchanged.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 4   # 120

SRC_R = 0x40
SRC_G = 0xC0
SRC_B = 0x80

DST_R = 0x80
DST_G = 0x80
DST_B = 0x80


def _op_clear(s, d):       return 0
def _op_and(s, d):         return (s & d) & 0xFF
def _op_and_rev(s, d):     return (s & (~d)) & 0xFF
def _op_copy(s, d):        return s & 0xFF
def _op_and_inv(s, d):     return ((~s) & d) & 0xFF
def _op_noop(s, d):        return d & 0xFF
def _op_xor(s, d):         return (s ^ d) & 0xFF
def _op_or(s, d):          return (s | d) & 0xFF
def _op_nor(s, d):         return (~(s | d)) & 0xFF
def _op_equiv(s, d):       return (~(s ^ d)) & 0xFF
def _op_invert(s, d):      return (~d) & 0xFF
def _op_or_rev(s, d):      return (s | (~d)) & 0xFF
def _op_copy_inv(s, d):    return (~s) & 0xFF
def _op_or_inv(s, d):      return ((~s) | d) & 0xFF
def _op_nand(s, d):        return (~(s & d)) & 0xFF
def _op_set(s, d):         return 0xFF


# Op functions per cell index (row * 4 + col). Order matches main.c
# k_ops[] (GL/D3D logic-op enum 0..15).
OP_TABLE = (
    _op_clear,    _op_and,       _op_and_rev,    _op_copy,
    _op_and_inv,  _op_noop,      _op_xor,        _op_or,
    _op_nor,      _op_equiv,     _op_invert,     _op_or_rev,
    _op_copy_inv, _op_or_inv,    _op_nand,       _op_set,
)


def _cell_rgba(idx: int) -> tuple:
    op = OP_TABLE[idx]
    r = op(SRC_R, DST_R)
    g = op(SRC_G, DST_G)
    b = op(SRC_B, DST_B)
    return (r, g, b, 0xFF)


def _cell_for(x: int, y: int) -> int:
    col = x // CELL_W
    row = y // CELL_H
    return row * 4 + col


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"logic-ops expected only {WIDTH}x{HEIGHT}, got "
            f"{width}x{height}")
    # Precompute per-cell RGBA to avoid 16 function calls per pixel.
    cells = tuple(_cell_rgba(i) for i in range(16))
    rgba = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            r, g, b, a = cells[_cell_for(x, y)]
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
