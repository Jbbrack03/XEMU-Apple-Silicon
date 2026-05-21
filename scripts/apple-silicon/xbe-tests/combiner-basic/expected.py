"""
combiner-basic — math-derived expected oracle.

640x480 RGBA front buffer. 4-col × 4-row grid of 160x120 cells; each cell
exercises one of 4 NV2A input mappings (UI, II, EN, EE) under one of 4
output scale modifiers (ID, SL1, SL2, NB). The XBE feeds a fixed
DIFFUSE = (0.25, 0.5, 0.75, 1.0) into a single-stage combiner whose SUM
is routed to R0 then to fragColor.rgb. The XBE's source-file header
carries the full math derivation; this module encodes the same per-cell
expected RGB so the harness can pixel-compare the captured frame
against a known oracle.

Per the §4.12 derivation:

  Stage 1: A_MAP per column applied to each channel of DIFFUSE:
    UI -> (0.25, 0.5,  0.75)
    II -> (0.75, 0.5,  0.25)
    EN -> (-0.5, 0.0,  0.5)
    EE -> ( 0.5, 0.0, -0.5)

  Stage 2: OCW OP per row applied to each channel, then clamp [-1, 1]
  (per xemu's psh.c add_stage_code's `clamp(mux_sum, -1, 1)`):

    ID  (y = x)
    SL1 (y = 2x)
    SL2 (y = 4x)
    NB  (y = x - 0.5)

  Stage 3: framebuffer clamp [0, 1], quantize to 8-bit (round-to-nearest).

Quantization: float → uint8 uses round-to-nearest (banker's rounding /
half-to-even). 0.25 * 255 = 63.75 → 64; 0.5 * 255 = 127.5 → 128 on most
GLSL/Metal float -> uint8 conversions (Apple Silicon Metal: round-half-
to-even gives 128; xemu's GL renderer: same); 0.75 * 255 = 191.25 → 191.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
GRID_COLS = 4
GRID_ROWS = 4
CELL_W = WIDTH // GRID_COLS    # 160
CELL_H = HEIGHT // GRID_ROWS   # 120

DIFFUSE = (0.25, 0.5, 0.75)    # R, G, B; alpha is 1.0 (kept = 255)


def _apply_map(c: float, mapping: str) -> float:
    """Apply NV2A input mapping to a channel value."""
    if mapping == "UI":
        return max(0.0, c)            # UNSIGNED_IDENTITY
    if mapping == "II":
        return 1.0 - max(0.0, c)      # UNSIGNED_INVERT
    if mapping == "EN":
        return 2.0 * max(0.0, c) - 1.0  # EXPAND_NORMAL
    if mapping == "EE":
        return 1.0 - 2.0 * max(0.0, c)  # EXPAND_NEGATE
    raise ValueError(f"unknown mapping {mapping!r}")


def _apply_scale(x: float, op: str) -> float:
    """Apply NV2A OCW output scale modifier to a per-channel value."""
    if op == "ID":
        return x                      # NOSHIFT (identity)
    if op == "SL1":
        return 2.0 * x                # SHIFTLEFTBY1
    if op == "SL2":
        return 4.0 * x                # SHIFTLEFTBY2
    if op == "NB":
        return x - 0.5                # NOSHIFT_BIAS
    raise ValueError(f"unknown op {op!r}")


def _clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def _quantize(x: float) -> int:
    """Float -> uint8 via round-to-nearest (banker's rounding tie-break).
    Matches what GLSL fragColor -> RGBA8 framebuffer write does on the
    renderers we target. Inputs in [0, 1]."""
    v = x * 255.0
    # Python's round() uses banker's rounding (round-half-to-even) which
    # matches GLSL/Metal's default float -> uint8 conversion behavior
    # well enough to be within the harness's per-channel threshold of 16
    # for any well-formed shader output. The 0.5 case rounds to 128 (the
    # nearest even integer for 127.5).
    return int(round(v))


def _cell_for(x: int, y: int) -> tuple[str, str]:
    """Return (mapping, op) for the cell at pixel (x, y)."""
    col = x // CELL_W
    row = y // CELL_H
    mapping = ("UI", "II", "EN", "EE")[col]
    op      = ("ID", "SL1", "SL2", "NB")[row]
    return mapping, op


def _cell_rgba(mapping: str, op: str) -> tuple[int, int, int, int]:
    """Math-derived (R, G, B, A) for one (mapping, op) cell."""
    rgb = []
    for c_in in DIFFUSE:
        # Stage 1: mapping.
        a = _apply_map(c_in, mapping)
        # Stage 2: scale + clamp to [-1, 1] (psh.c).
        scaled = _apply_scale(a, op)
        scaled = _clamp(scaled, -1.0, 1.0)
        # Stage 3: framebuffer clamp + 8-bit quantize.
        out01  = _clamp(scaled, 0.0, 1.0)
        rgb.append(_quantize(out01))
    return rgb[0], rgb[1], rgb[2], 0xFF


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Math-derived expected output. The grid is 4 cols × 4 rows of 16
    cells; each cell uniform. Mid-tones (64 / 128 / 191) are byte-exact
    in the raw framebuffer (no display gamma applied at capture)."""
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"combiner-basic expected only {WIDTH}x{HEIGHT}, "
            f"got {width}x{height}"
        )
    # Precompute per-cell RGBA so the per-pixel loop is O(1) per pixel.
    cell_rgba = {}
    for row in range(GRID_ROWS):
        for col in range(GRID_COLS):
            mapping = ("UI", "II", "EN", "EE")[col]
            op      = ("ID", "SL1", "SL2", "NB")[row]
            cell_rgba[(row, col)] = _cell_rgba(mapping, op)

    rgba = bytearray(width * height * 4)
    for y in range(height):
        row = y // CELL_H
        for x in range(width):
            col = x // CELL_W
            r, g, b, a = cell_rgba[(row, col)]
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
