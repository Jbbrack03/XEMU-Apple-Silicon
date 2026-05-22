"""
texture-shader-stages — math-derived expected oracle.

640x480 RGBA front buffer. 4-col x 4-row grid of 160x120 cells.

v0.3 exercises 2 of 19 NV2A SHADER_STAGE_PROGRAM modes at stage 0, plus
two bisect rows isolating the two surviving candidates from the v0.2
DIFFUSE-source bisect:

  Row 0 (y   0..119):  PASS_THROUGH (0x04), combiner A=T0, textured shaders
                        cells: RED, GREEN, BLUE, WHITE
                        path:  t0 = pT0 -> R0 -> fragColor
  Row 1 (y 120..239):  PROGRAM_NONE (0x00), combiner A=T0, textured shaders
                        cells: BLACK x 4
                        path:  t0 = vec4(0,0,0,1) -> R0=0 -> BLACK
  Row 2 (y 240..359):  PROGRAM_NONE (0x00), sentinel combiner, textured shaders
                        cells: WHITE x 4
                        path:  A=INVERT(ZERO)=1.0, B=INVERT(ZERO)=1.0 ->
                               R0=1.0 -> WHITE, independent of T0/V0/tex
  Row 3 (y 360..479):  PROGRAM_NONE (0x00), combiner A=V0/DIFFUSE, DEFAULT shaders
                        cells: RED, GREEN, BLUE, WHITE
                        path:  default shaders; v0=DIFFUSE -> R0 -> fragColor

Shared combiner topology (rows 0, 1, 3): A -> R0 -> fragColor.rgb
  (FINAL D = R0); final-combiner G = DIFFUSE.a -> fragColor.a = 255.
Row 2 sentinel: A=B=INVERT(ZERO) -> R0=1.0; same FINAL CW structure.
All cells encode saturated 0/255 cube-corner channels (or constant WHITE);
the float -> 8-bit framebuffer quantize step is byte-exact in cell interiors.

The manifest applies compare_overrides (max_changed_pct=5.0 /
signal>=95.0%) only to absorb sub-pixel rasterizer edges along the
3 inter-cell vertical boundaries + 3 horizontal row-boundary lines +
harness frame-selection slack.

Bisect interpretation:
  row 2 (sentinel) PASS on Metal: combiner IS executed under textured-shader
    setup; prior failures (v0.2) are about T0/V0 inputs returning 0.
  row 2 (sentinel) FAIL on Metal: draws are silently discarded before the
    combiner executes under textured-shader setup.
  row 3 (control) PASS on Metal: V0/DIFFUSE path works with default shaders;
    textured-shader state machine specifically breaks the V0/DIFFUSE dispatch.
  row 3 (control) FAIL on Metal: V0/DIFFUSE path broken even with default
    shaders; more fundamental issue independent of shader setup.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
CELL_W = WIDTH // 4     # 160
CELL_H = HEIGHT // 4    # 120

# Row 0: PASS_THROUGH, A=T0, textured -> per-cell TEXCOORD0 colors
# Row 1: PROGRAM_NONE, A=T0, textured -> BLACK (t0 = 0)
# Row 2: PROGRAM_NONE, sentinel (A=B=INVERT(ZERO)=1.0), textured -> WHITE
# Row 3: PROGRAM_NONE, A=V0/DIFFUSE, default shaders -> per-cell DIFFUSE colors
CELL_RGBA = (
    (0xFF, 0x00, 0x00, 0xFF),  #  0 RED      PASS_THROUGH, A=T0
    (0x00, 0xFF, 0x00, 0xFF),  #  1 GREEN    PASS_THROUGH, A=T0
    (0x00, 0x00, 0xFF, 0xFF),  #  2 BLUE     PASS_THROUGH, A=T0
    (0xFF, 0xFF, 0xFF, 0xFF),  #  3 WHITE    PASS_THROUGH, A=T0
    (0x00, 0x00, 0x00, 0xFF),  #  4 BLACK    PROGRAM_NONE, A=T0
    (0x00, 0x00, 0x00, 0xFF),  #  5 BLACK    PROGRAM_NONE, A=T0
    (0x00, 0x00, 0x00, 0xFF),  #  6 BLACK    PROGRAM_NONE, A=T0
    (0x00, 0x00, 0x00, 0xFF),  #  7 BLACK    PROGRAM_NONE, A=T0
    (0xFF, 0xFF, 0xFF, 0xFF),  #  8 WHITE    PROGRAM_NONE, sentinel A=INVERT(0)
    (0xFF, 0xFF, 0xFF, 0xFF),  #  9 WHITE    PROGRAM_NONE, sentinel A=INVERT(0)
    (0xFF, 0xFF, 0xFF, 0xFF),  # 10 WHITE    PROGRAM_NONE, sentinel A=INVERT(0)
    (0xFF, 0xFF, 0xFF, 0xFF),  # 11 WHITE    PROGRAM_NONE, sentinel A=INVERT(0)
    (0xFF, 0x00, 0x00, 0xFF),  # 12 RED      PROGRAM_NONE, A=V0 (default shaders)
    (0x00, 0xFF, 0x00, 0xFF),  # 13 GREEN    PROGRAM_NONE, A=V0 (default shaders)
    (0x00, 0x00, 0xFF, 0xFF),  # 14 BLUE     PROGRAM_NONE, A=V0 (default shaders)
    (0xFF, 0xFF, 0xFF, 0xFF),  # 15 WHITE    PROGRAM_NONE, A=V0 (default shaders)
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
