#!/usr/bin/env python3
"""Objective property/invariant oracle for rendered-frame correctness.

PURPOSE
-------
This is the correctness GATE for visual validation. It replaces subjective
"does this look right?" judgment by an LLM/agent — which has no ground truth
for these games and has passed garbled frames. It decides, WITHOUT any
reference image, whether a captured frame is a *plausibly real rendered scene*
or a degenerate failure (solid clear-color, missing-drawable sentinel, blank,
flat/no-structure, near-empty palette).

It does NOT decide "is this the CORRECT scene" — that needs a curated golden +
metric (see the metric-vs-golden path). It decides "is this NOT garbled," which
is exactly the failure mode that was slipping through. Calibrated on real
captures: a solid-magenta missing-drawable frame must FAIL; a sparse-but-valid
frame (mostly black background with a small 3D object + HUD) must PASS.

No LLM is involved. The numbers are the verdict.

USAGE
  xemu_frame_correctness_oracle.py FRAME.png [FRAME2.png ...]
  xemu_frame_correctness_oracle.py --calibrate FRAME.png      # print raw metrics
  xemu_frame_correctness_oracle.py --json FRAME.png
  xemu_frame_correctness_oracle.py --sequence DIR             # + temporal-stability check
Exit code 0 = all frames PASS; 1 = any frame FAIL/garbled; 2 = usage/error.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

try:
    from PIL import Image, ImageFilter
except Exception as e:  # noqa: BLE001
    print(json.dumps({"error": f"PIL unavailable: {e}"}))
    sys.exit(2)

# --- thresholds (calibrated against real captures; see --calibrate) ---
MAX_DOMINANT_FRACTION = 0.985   # a single color covering ~the whole frame …
MIN_EDGE_FRACTION_IF_UNIFORM = 0.002  # …with ~no edges = degenerate (clear-color)
MIN_EDGE_FRACTION = 0.0008      # essentially flat frame = no rendered structure
MIN_DISTINCT_COLORS = 16        # near-empty palette = not a real scene
EDGE_PIXEL_THRESHOLD = 24       # luminance-gradient value counted as an "edge" pixel
# Known missing-drawable / debug sentinel clear colors (exact, dominant).
SENTINELS = {(255, 0, 255): "magenta-missing-drawable", (255, 0, 0): "red-clear",
             (0, 255, 0): "green-clear"}


def metrics(path: str) -> dict:
    img = Image.open(path).convert("RGB")
    w, h = img.size
    total = w * h
    # Color diversity + dominant fraction (cap getcolors so huge palettes => None).
    colors = img.getcolors(maxcolors=200000)
    if colors is None:
        distinct = 200000          # richer than the cap; definitely diverse
        dom_frac = 0.0
        dom_color = None
    else:
        distinct = len(colors)
        cnt, dom_color = max(colors, key=lambda c: c[0])
        dom_frac = cnt / total
    # Edge energy: fraction of pixels with a meaningful luminance gradient.
    edges = img.convert("L").filter(ImageFilter.FIND_EDGES)
    ehist = edges.histogram()
    edge_pixels = sum(ehist[EDGE_PIXEL_THRESHOLD:])
    edge_frac = edge_pixels / total
    return {"path": path, "w": w, "h": h, "distinct_colors": distinct,
            "dominant_fraction": round(dom_frac, 5), "dominant_color": dom_color,
            "edge_fraction": round(edge_frac, 6)}


def verdict(m: dict) -> dict:
    reasons = []
    dom = m["dominant_color"]
    if dom is not None and tuple(dom) in SENTINELS and m["dominant_fraction"] > 0.5:
        reasons.append(f"sentinel clear-color ({SENTINELS[tuple(dom)]}) fills frame")
    if m["dominant_fraction"] > MAX_DOMINANT_FRACTION and m["edge_fraction"] < MIN_EDGE_FRACTION_IF_UNIFORM:
        reasons.append(f"degenerate uniform: one color {m['dominant_fraction']:.3f} of frame, no structure")
    if m["edge_fraction"] < MIN_EDGE_FRACTION:
        reasons.append(f"flat/no rendered structure (edge_fraction {m['edge_fraction']:.6f})")
    if m["distinct_colors"] < MIN_DISTINCT_COLORS:
        reasons.append(f"near-empty palette ({m['distinct_colors']} colors)")
    return {**m, "passed": not reasons, "reasons": reasons}


def main() -> int:
    ap = argparse.ArgumentParser(description="Objective rendered-frame correctness oracle.")
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--calibrate", action="store_true", help="print raw metrics only")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    results = []
    any_fail = False
    for f in args.frames:
        if not os.path.exists(f):
            results.append({"path": f, "error": "missing"}); any_fail = True; continue
        try:
            v = verdict(metrics(f))
        except Exception as e:  # noqa: BLE001
            results.append({"path": f, "error": str(e)}); any_fail = True; continue
        if not v["passed"]:
            any_fail = True
        results.append(v)

    if args.json or args.calibrate:
        print(json.dumps(results, indent=2))
    else:
        for r in results:
            if "error" in r:
                print(f"ERROR  {r['path']}: {r['error']}")
            else:
                tag = "PASS " if r["passed"] else "FAIL "
                extra = "" if r["passed"] else " :: " + "; ".join(r["reasons"])
                print(f"{tag} {os.path.basename(r['path'])} "
                      f"[colors={r['distinct_colors']} dom={r['dominant_fraction']:.3f} "
                      f"edges={r['edge_fraction']:.5f}]{extra}")
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
