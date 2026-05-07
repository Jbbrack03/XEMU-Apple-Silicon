"""
controller-roundtrip — math-derived expected oracle.

Synthesizes the byte-exact RGBA front buffer the diag XBE produces
when it reads a known synthetic controller state out of the agent's
persistent kernel-pool buffer. The orchestrator computes the reference
PNG from the SAME state values it set via Mac-side `controller.set`,
so a byte-exact match between captured and reference proves both:

  1. The agent's persistent kernel-pool buffer survived the
     XLaunchXBE chainload byte-for-byte.
  2. The shim layer (`xbed_input_synth_*`) read the buffer correctly
     and the diag XBE rendered the documented pattern.

Pattern (mirrors `controller-roundtrip/main.c::s_render_state`):

  - Background: opaque BLACK (R=G=B=0, A=255). The shim-attach-FAILED
    fallback paints CYAN (R=0, G=255, B=255, A=255) instead, but
    `default()` here only generates the success-pattern reference.

  - Top half: 4x4 button cells at WIDTH=160, HEIGHT=160, anchored
    at (240, 40). Bit b lights cell (b%4, b/4) covering the
    half-open rect (col*40+2 .. col*40+38) x (row*40+2 .. row*40+38)
    in pixel coords. Cells are pure WHITE (R=G=B=255).

  - Bottom half: 6 horizontal axis stripes 40px tall starting at
    y=240, in order: lt, rt, lx, ly, rx, ry. Each stripe is filled
    from x=0 to x=int(640 * frac), where:
        triggers (lt, rt): frac = max(0, value) / 32767
        sticks (lx,ly,rx,ry): frac = (value + 32768) / 65535
    Filled pixels are WHITE (R=G=B=255).

Per-pixel rasterization rule: integer-corner quads under d3d top-left
fill convention give a half-open coverage rect [x0, x1) x [y0, y1) —
identical to the rule used by mirror/expected.py. This is renderer-
independent (real Xbox / xemu-GL / xemu-Metal must all produce the
same output).
"""
from __future__ import annotations

from typing import Optional


WIDTH = 640
HEIGHT = 480
GRID_X0 = 240
GRID_Y0 = 40
CELL_PX = 40
CELL_PAD = 2
STRIPES_Y0 = 240
STRIPE_PX = 40

# Button-bit positions match xemu/CONTROLLER_BUTTON_* and ORACLE_BTN_*.
BUTTON_BITS = list(range(16))


def _set_pixel(rgba: bytearray, x: int, y: int,
               r: int, g: int, b: int, a: int = 0xFF) -> None:
    if x < 0 or x >= WIDTH or y < 0 or y >= HEIGHT:
        return
    o = (y * WIDTH + x) * 4
    rgba[o + 0] = r
    rgba[o + 1] = g
    rgba[o + 2] = b
    rgba[o + 3] = a


def _fill_rect(rgba: bytearray, x0: int, y0: int, x1: int, y1: int,
               r: int, g: int, b: int) -> None:
    """Fill the half-open rect [x0, x1) x [y0, y1) — matches the
    diag XBE's integer-corner-quad coverage under the NV2A's d3d
    top-left fill convention."""
    if x0 < 0: x0 = 0
    if y0 < 0: y0 = 0
    if x1 > WIDTH:  x1 = WIDTH
    if y1 > HEIGHT: y1 = HEIGHT
    for y in range(y0, y1):
        row_off = y * WIDTH * 4
        for x in range(x0, x1):
            o = row_off + x * 4
            rgba[o + 0] = r
            rgba[o + 1] = g
            rgba[o + 2] = b
            rgba[o + 3] = 0xFF


def _new_black() -> bytearray:
    rgba = bytearray(WIDTH * HEIGHT * 4)
    for i in range(0, len(rgba), 4):
        rgba[i + 3] = 0xFF
    return rgba


def from_state(buttons: int = 0,
               ltrigger: int = 0, rtrigger: int = 0,
               lstick_x: int = 0, lstick_y: int = 0,
               rstick_x: int = 0, rstick_y: int = 0) -> bytes:
    """Return the expected RGBA buffer for a given synthetic state.
    All numeric ranges match the wire-protocol values: buttons is a
    16-bit OR of XBED_BTN_* / ORACLE_BTN_* / CONTROLLER_BUTTON_* bits,
    triggers are int16 0..32767 (xemu axis range; NEGATIVE clamps to 0
    here too), sticks are int16 -32768..32767."""
    rgba = _new_black()

    # Top half — button grid.
    for bit in BUTTON_BITS:
        if not (buttons & (1 << bit)):
            continue
        col = bit % 4
        row = bit // 4
        x0 = GRID_X0 + col * CELL_PX + CELL_PAD
        y0 = GRID_Y0 + row * CELL_PX + CELL_PAD
        x1 = GRID_X0 + col * CELL_PX + CELL_PX - CELL_PAD
        y1 = GRID_Y0 + row * CELL_PX + CELL_PX - CELL_PAD
        _fill_rect(rgba, x0, y0, x1, y1, 0xFF, 0xFF, 0xFF)

    # Bottom half — axis stripes.
    axes = [ltrigger, rtrigger, lstick_x, lstick_y, rstick_x, rstick_y]
    is_signed = [False, False, True, True, True, True]
    for i, (val, signed) in enumerate(zip(axes, is_signed)):
        if signed:
            frac = (float(val) + 32768.0) / 65535.0
        else:
            frac = 0.0 if val < 0 else float(val) / 32767.0
        if frac < 0.0: frac = 0.0
        if frac > 1.0: frac = 1.0
        if frac == 0.0: continue
        filled_w = int(WIDTH * frac)
        if filled_w <= 0: continue
        y0 = STRIPES_Y0 + i * STRIPE_PX
        y1 = STRIPES_Y0 + i * STRIPE_PX + STRIPE_PX
        _fill_rect(rgba, 0, y0, filled_w, y1, 0xFF, 0xFF, 0xFF)

    return bytes(rgba)


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Default oracle: zero state → all opaque-black. Used when the
    orchestrator does NOT pre-set a synthetic state (the diag XBE
    then renders the all-black background and the comparison verifies
    the wiring all the way down to MmGetPhysicalAddress)."""
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"controller-roundtrip expected only {WIDTH}x{HEIGHT}, "
            f"got {width}x{height}")
    return from_state()


def to_png(path: str) -> None:
    """CLI helper: write the zero-state expected PNG to PATH. Use
    `from_state(...)` programmatically for non-zero references."""
    import importlib.util
    from pathlib import Path
    here = Path(__file__).resolve().parent
    client_path = here.parent.parent / "oracle-client.py"
    spec = importlib.util.spec_from_file_location("oracle_client",
                                                   client_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load oracle-client.py from "
                           f"{client_path}")
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
