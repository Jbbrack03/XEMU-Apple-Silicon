"""
texture-shader-stages — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 2-row grid of 160x240 cells.

v0.1 exercises 2 of 19 NV2A SHADER_STAGE_PROGRAM modes at stage 0:

  Row 0 (y 0..239):    PASS_THROUGH (0x04) -- t0 = pT0 (TEXCOORD0)
                        cells: RED, GREEN, BLUE, WHITE
  Row 1 (y 240..479):  PROGRAM_NONE (0x00) -- t0 = vec4(0,0,0,1)
                        cells: BLACK x 4 (TEXCOORD0 input ignored)

Shared combiner: t0 -> R0 -> fragColor.rgb (FINAL D = R0); final-combiner
G = DIFFUSE.a (always 1.0) -> fragColor.a = 255. All 8 cells encode
saturated 0/255 cube-corner channels; the float -> 8-bit framebuffer
quantize step is byte-exact in cell interiors. The manifest applies a
small `compare_overrides` budget (max_changed_pct=3.0 / signal>=97.0%)
only to absorb sub-pixel rasterizer edges along the 4 inter-cell
vertical seams + the 1 horizontal mid-line + harness frame-selection
slack; the per-channel threshold remains the harness default (16).

Catches:
  - SHADER_STAGE_PROGRAM 5-bit field dispatch broken: any wrong mode
    deviates visibly from BOTH the PASS_THROUGH and NONE expectations.
  - PASS_THROUGH collapsed to NONE: row 0 renders BLACK instead of
    R/G/B/W.
  - NONE collapsed to PASS_THROUGH: row 1 renders the TEXCOORD0 colors
    instead of BLACK.
  - TEXCOORD0 attribute interpolation broken at the vertex pipe: row
    0 renders one uniform color across all 4 cells.
  - Dummy texture sample leaking into fragColor: row 0 renders MAGENTA
    (the dummy texture content) instead of the per-cell PASS_THROUGH
    color.

Does NOT catch (deferred to v0.2+): the other 17 of 19 modes
(PROJECT2D, PROJECT3D, CUBEMAP, CLIPPLANE, BUMPENVMAP*, BRDF, DOT_*,
DPNDNT_*, DOTPRODUCT, DOT_RFLCT_SPEC_CONST). Multi-stage chaining
(stage 1+ reading t0 from stage 0). The
NV097_SET_SHADER_OTHER_STAGE_INPUT register semantics.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4    # 160
CELL_H = HEIGHT // 2   # 240

# Row 0: PASS_THROUGH cells -> the per-cell TEXCOORD0 (R, G, B, 1).
# Row 1: PROGRAM_NONE cells -> (0, 0, 0, 1) regardless of TEXCOORD0.
CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  # 0 RED      PASS_THROUGH
    (0x00, 0xFF, 0x00, 0xFF),  # 1 GREEN    PASS_THROUGH
    (0x00, 0x00, 0xFF, 0xFF),  # 2 BLUE     PASS_THROUGH
    (0xFF, 0xFF, 0xFF, 0xFF),  # 3 WHITE    PASS_THROUGH
    (0x00, 0x00, 0x00, 0xFF),  # 4 BLACK    PROGRAM_NONE
    (0x00, 0x00, 0x00, 0xFF),  # 5 BLACK    PROGRAM_NONE
    (0x00, 0x00, 0x00, 0xFF),  # 6 BLACK    PROGRAM_NONE
    (0x00, 0x00, 0x00, 0xFF),  # 7 BLACK    PROGRAM_NONE
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
            f"texture-shader-stages expected only {WIDTH}x{HEIGHT}, "
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
