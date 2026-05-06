"""
pipeline-smoke — math-derived expected oracle.

Generates a 640x480 RGBA pixel buffer that exactly matches the
front-buffer state the XBE paints before reboot:

  - All pixels: opaque black (R=0, G=0, B=0, A=255).
  - Pixel at guest coord (320, 50): opaque white (R=255, G=255,
    B=255, A=255).

This module is the audit oracle when no real-Xbox capture is
available. When a real-Xbox reference frame is captured under
docs/apple-silicon/xbox-real-references/pipeline-smoke/, the
captured frame must equal what this generator produces;
disagreement is an actionable finding.

Per `diagnostic-xbe-plan.md` §3.5, manifest entries that
reference this module use `generator: "expected.py:default"`.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480
WHITE_X = 320
WHITE_Y = 50


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    """Returns a `width * height * 4` RGBA bytes buffer with the
    expected pattern. Flag keyword arguments are accepted but
    unused — the pattern is renderer-independent."""
    if width != WIDTH or height != HEIGHT:
        # The current XBE is hard-wired to 640x480; bail loudly if
        # called with a mismatched shape so a manifest typo
        # surfaces fast.
        raise ValueError(
            f"pipeline-smoke expected only {WIDTH}x{HEIGHT}, got {width}x{height}"
        )
    rgba = bytearray(width * height * 4)
    for i in range(0, len(rgba), 4):
        rgba[i + 3] = 0xFF  # alpha
    idx = (WHITE_Y * width + WHITE_X) * 4
    rgba[idx + 0] = 0xFF  # R
    rgba[idx + 1] = 0xFF  # G
    rgba[idx + 2] = 0xFF  # B
    rgba[idx + 3] = 0xFF  # A
    return bytes(rgba)


def to_png(path: str) -> None:
    """CLI helper: write the math-derived expected PNG to PATH."""
    import sys
    sys.path.insert(0, str(__file__.rsplit("/", 4)[0]))
    # oracle-client.py contains the stdlib-only PNG encoder. Re-use it
    # via importlib.util so the dash in the filename doesn't get in
    # the way.
    import importlib.util
    from pathlib import Path
    here = Path(__file__).resolve().parent
    client_path = here.parent.parent / "oracle-client.py"
    spec = importlib.util.spec_from_file_location("oracle_client", client_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load oracle-client.py from {client_path}")
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
