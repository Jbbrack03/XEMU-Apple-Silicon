"""
crtc-publish — math-derived expected oracle (per-recipe).

640x480 RGBA front buffer. The expected output depends on which
renderer + flag recipe ran:

  * Surface A (red 0xFFFF0000)  — CRTC-pointed pbkit back buffer.
  * Surface B (green 0xFF00FF00) — pbkit extra buffer 0 (intermediate;
                                    1 marker draw, not used as
                                    expected by any recipe).
  * Surface C (blue 0xFF0000FF) — pbkit extra buffer 1 (3 marker
                                   draws → highest frame_draw_count →
                                   xemu-Metal fallback candidate).

Recipe key | publish path                                      | color
-----------+---------------------------------------------------+------
real-xbox  | NV2A CRTC physically scans pbkit's swapped front  | RED
xemu/gl    | pgraph_gl flip_stall publishes CRTC-pointed       | RED
metal/fb=0 | publish_display_front_fb (CRTC-pointed)           | RED
metal/fb=1 | publish_latest_draw_fallback (candidate = C)      | BLUE

The XBE source-file header (`main.c`) contains the full math
derivation. This module encodes the same math in Python; a reviewer
can confirm the two agree per `diagnostic-xbe-plan.md` v2 §2.5.

Per manifest.json, each per-recipe expected_results entry maps to
one of:

  default                  - canonical RED (real-xbox, GL, metal+fb=0)
  metal_with_fallback      - BLUE (metal+fb=1)
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480

# RGBA tuples for the three surfaces, matching the XBE's
# xbed_clear_color_argb() arguments. The XBE writes ARGB to VRAM and
# the host capture path (oracle_client.bgrx_to_rgba +
# save_screenshot_png) writes RGBA PNGs; the reference returned here
# is the same RGBA layout the harness compares against.
COL_A_RGBA = (0xFF, 0x00, 0x00, 0xFF)  # red
COL_C_RGBA = (0x00, 0x00, 0xFF, 0xFF)  # blue


def _solid(rgba_color: tuple, width: int, height: int) -> bytes:
    """Build width*height*4 RGBA bytes buffer of a uniform color."""
    r, g, b, a = rgba_color
    rgba = bytearray(width * height * 4)
    for i in range(0, len(rgba), 4):
        rgba[i + 0] = r
        rgba[i + 1] = g
        rgba[i + 2] = b
        rgba[i + 3] = a
    return bytes(rgba)


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Canonical: surface A (RED) — the CRTC-pointed buffer.

    Used by:
      * real-xbox  (any flag recipe; real HW always scans CRTC).
      * xemu-GL    (no fallback path; publishes CRTC-pointed).
      * xemu-Metal with XEMU_METAL_FRONT_FB_FALLBACK=0
                    (publish_display_front_fb publishes CRTC).
    """
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"crtc-publish expected only {WIDTH}x{HEIGHT}, got "
            f"{width}x{height}"
        )
    return _solid(COL_A_RGBA, width, height)


def metal_with_fallback(width: int = WIDTH, height: int = HEIGHT,
                        **flags) -> bytes:
    """xemu-Metal with XEMU_METAL_FRONT_FB_FALLBACK=1.

    The flip_stall publish path runs publish_latest_draw_fallback
    (surface.mm:1970), which picks the cached MtlSurfaceBinding with
    the highest per-frame draw count. Our XBE's per-frame pattern is
    A=0 / B=1 / C=3 draws, so C wins. C was cleared to BLUE and the
    marker draws paint the same BLUE, so the published surface is
    uniform BLUE.
    """
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"crtc-publish expected only {WIDTH}x{HEIGHT}, got "
            f"{width}x{height}"
        )
    return _solid(COL_C_RGBA, width, height)


def to_png(path: str, generator: str = "default") -> None:
    """CLI helper: write the math-derived expected PNG for one of the
    public generators (`default` or `metal_with_fallback`) to PATH."""
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
    gen_fn = {
        "default": default,
        "metal_with_fallback": metal_with_fallback,
    }.get(generator)
    if gen_fn is None:
        raise ValueError(
            f"unknown generator: {generator!r}; "
            f"expected 'default' or 'metal_with_fallback'")
    oc.save_screenshot_png(gen_fn(), WIDTH, HEIGHT, path)


if __name__ == "__main__":
    import sys
    if len(sys.argv) not in (2, 3):
        print("usage: expected.py <out.png> [default|metal_with_fallback]",
              file=sys.stderr)
        sys.exit(1)
    gen = sys.argv[2] if len(sys.argv) == 3 else "default"
    to_png(sys.argv[1], gen)
    print(f"wrote {WIDTH}x{HEIGHT} expected ({gen}) PNG to {sys.argv[1]}")
