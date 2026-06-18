"""
polygon-offset — math-derived expected oracle (§C.3).  v0.2 corrected.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells, each
exercising one NV2A polygon-offset / depth-bias combination.

Renderer semantics (fork PR #2240, XEMU_NATIVE_TRI_DEPTH): polygon offset
is applied in the FRAGMENT SHADER, NOT via glPolygonOffset/setDepthBias.
For the native_tri_depth / native_quad non-z_perspective path
(glsl/psh.c:1052-1059):

    zvalue = gl_FragCoord.z * clipRange.y            # window depth
    nativeTriMZ = max(|dFdx(zvalue)*sx|,|dFdy(zvalue)*sy|)  # screen slope
    zvalue += depthOffset                # = NV_PGRAPH_ZOFFSETBIAS  (units)
    zvalue += depthFactor*nativeTriMZ    # = NV_PGRAPH_ZOFFSETFACTOR (factor)

then (if depth_clipping=CULL, which pbkit enables) the fragment is
DISCARDED if zvalue < clipRange.z || zvalue > clipRange.w
(glsl/psh.c:1116-1120). Metal consumes the same generated GLSL.

CRITICAL CORRECTION (the v0.1 oracle bug, caught 2026-06-18):
the diag-lib viewport matrix (lib/xbed_runtime.c:36, s_viewport[2][2] =
65536.0) maps clip z to window-Z with a fixed scale of 65536, NOT the
full window-Z span. So the BASE quad at clip z=0.5 lands at window-Z
0.5*65536 = 32768, NOT 0.5*16777215. v0.1 wrongly assumed the latter.
With base window-Z = 32768 and the XBE's pinned clip range [0, 16777215]:

  Cell  PolyOff  units        factor  Zo                 Result
  ----  -------  -----------  ------  -----------------  ------
  0     OFF      (n/a)        (n/a)   32768  (tie)       RED
  1     ON       0.0          0.0     32768  (tie)       RED
  2     ON       +100000      0.0     132768 > base      GREEN
  3     ON       -100000      0.0     -67232 < 0 DISCARD GREEN
  4     ON       +8000000     0.0     8032768 > base     GREEN
  5     ON       -8000000     0.0     <0 DISCARD         GREEN
  6     ON       0.0          +10     +~273 bias > base  GREEN
  7     ON       0.0          -10     -~273 bias < base  RED

  Row 0 (y 0..239):   RED   RED   GREEN  GREEN
  Row 1 (y 240..479): GREEN GREEN GREEN  RED

Note on the v0.1 -> v0.2 oracle change: cells 3 and 5 (large NEGATIVE
units) underflow the pinned clip range and are CLIP-CULLED (discard)
rather than winning LEQUAL, because the small base window-Z (32768)
leaves no headroom below zero for a -100000/-8000000 bias. They
therefore degenerate from "negative-bias-wins" tests into clip-cull
tests; both yield GREEN. A v0.3 redesign should either (a) place the
base plane near mid-clip-range (clip z chosen so base window-Z ~ 8.4e6,
e.g. clip z ~ 128 given the 65536 viewport scale) or (b) use small
negative units (~ -2000) that stay in [0, 16777215], so the negative-
offset cells genuinely isolate the LEQUAL-win sign. Documented as a
follow-up; v0.2 oracle reflects the CORRECT behavior of the XBE as
currently built so the harness verdict is honest.

This oracle = correct NV2A / GL behavior. The Metal renderer currently
applies NO effective depth bias (depthOffset/depthFactor reach the
Metal shader as 0 at runtime), so Metal renders all 8 cells RED and
FAILS this oracle at cells 2,3,4,5,6 -- a confirmed Metal §C.3 gap.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

RED = (0xFF, 0x00, 0x00, 0xFF)
GREEN = (0x00, 0xFF, 0x00, 0xFF)

# Cell colors in RGBA order. Corrected per the 65536 viewport Z scale.
CELL_RGBA = (
    RED,    # 0 offset OFF             -> tie               -> RED
    RED,    # 1 ON units=factor=0      -> tie               -> RED
    GREEN,  # 2 ON +100000 units       -> Zo > base         -> GREEN
    GREEN,  # 3 ON -100000 units       -> Zo < 0  DISCARD   -> GREEN
    GREEN,  # 4 ON +8000000 units      -> Zo > base         -> GREEN
    GREEN,  # 5 ON -8000000 units      -> Zo < 0  DISCARD   -> GREEN
    GREEN,  # 6 ON +10 factor slope    -> +bias > base      -> GREEN
    RED,    # 7 ON -10 factor slope    -> -bias < base      -> RED
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
            f"polygon-offset expected only {WIDTH}x{HEIGHT}, "
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
