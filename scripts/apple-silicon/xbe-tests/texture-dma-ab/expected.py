"""
texture-dma-ab — math-derived expected oracle.

640x480 RGBA front buffer. Both halves render the same RED texture
sampled via DMA channel A (left) vs DMA channel B (right). With
pbkit's default DMA setup (both channels resolve to base 0 of VRAM),
the two halves are pixel-identical.

  Whole frame: (255, 0, 0, 255) RED.
"""
from __future__ import annotations


WIDTH = 640
HEIGHT = 480

RED = (0xFF, 0x00, 0x00, 0xFF)


def default(width: int = WIDTH, height: int = HEIGHT, **flags) -> bytes:
    if width != WIDTH or height != HEIGHT:
        raise ValueError(
            f"texture-dma-ab expected only {WIDTH}x{HEIGHT}, "
            f"got {width}x{height}"
        )
    r, g, b, a = RED
    rgba = bytearray(width * height * 4)
    for o in range(0, len(rgba), 4):
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
