"""
msaa-aa-factor — math-derived expected oracle.

640x480 RGBA front buffer. Solid-BLACK background with one solid-WHITE
triangle whose two diagonal edges carry the test signal. The oracle is
the HARD-STEP rasterization (i.e. the msaa=0 output): every pixel whose
center lies strictly inside the triangle is byte-exact (255,255,255,255);
every other pixel is byte-exact (0,0,0,255).

The XBE renders the same triangle regardless of MSAA mode. With MSAA=N
enabled in the host renderer, edge pixels resolve to per-coverage
fractional values instead of the hard step — those are tolerated by the
manifest's `compare_overrides.max_changed_pct` (≈3%; the edge band is
≈0.72% of the image).

The catalog (§4.15) calls this an "epsilon" oracle for exactly that
reason: the edge gradient is renderer-dependent (Apple Silicon's
standard 4× pattern is not bit-identical to NV2A's), but the interior +
exterior signal regions are byte-exact and the edge stays inside the
expected geometric band.

Triangle vertices (window-space, integer):
  v0 = (60,  60)
  v1 = (60,  420)
  v2 = (580, 240)

Inside-test uses the half-plane / barycentric sign convention (every
vertex shares the same sign for an inside point). Pixel-center
evaluation: pixel (x, y) sample at (x + 0.5, y + 0.5).
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480

# Triangle vertex coords. Must match `main.c`'s V*_X / V*_Y constants
# byte-for-byte.
V0 = (60.0, 60.0)
V1 = (60.0, 420.0)
V2 = (580.0, 240.0)


def _edge(ax: float, ay: float, bx: float, by: float,
          px: float, py: float) -> float:
    """2D edge-function value at point P with respect to directed edge
    A→B. Sign is positive on one side, negative on the other; zero on
    the edge itself.

    Convention: returns >0 for points to the "left" of A→B in standard
    image-space coordinates where +y points down. We do not care about
    which side that is — only that the three edge values share the same
    sign for an inside point and any vertex order will be handled by
    a single "all-same-sign" check.
    """
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax)


def _is_inside(px: float, py: float) -> bool:
    """True if pixel-center (px, py) is strictly inside the triangle
    (V0, V1, V2). Edge pixels (e = 0) are returned False to match the
    HARD-STEP oracle: only fully-interior pixels are WHITE."""
    e01 = _edge(V0[0], V0[1], V1[0], V1[1], px, py)
    e12 = _edge(V1[0], V1[1], V2[0], V2[1], px, py)
    e20 = _edge(V2[0], V2[1], V0[0], V0[1], px, py)
    # All strictly positive OR all strictly negative depending on
    # winding. We don't care which; just that all three have the same
    # sign and none is zero.
    if e01 > 0.0 and e12 > 0.0 and e20 > 0.0:
        return True
    if e01 < 0.0 and e12 < 0.0 and e20 < 0.0:
        return True
    return False


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Math-derived expected output (HARD-STEP rasterization). Same
    oracle for every MSAA mode — the harness's `compare_overrides`
    absorbs the small per-mode AA boundary band.

    Returned as RGBA bytes in row-major order, width*height*4 bytes
    total."""
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"msaa-aa-factor expected only {WIDTH}x{HEIGHT}, "
            f"got {width}x{height}")
    rgba = bytearray(width * height * 4)
    for y in range(height):
        py = y + 0.5
        for x in range(width):
            px = x + 0.5
            o = (y * width + x) * 4
            if _is_inside(px, py):
                rgba[o + 0] = 0xFF
                rgba[o + 1] = 0xFF
                rgba[o + 2] = 0xFF
            # else: bytearray is zero-initialised (black RGB).
            rgba[o + 3] = 0xFF
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
