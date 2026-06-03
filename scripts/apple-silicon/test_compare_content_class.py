#!/usr/bin/env python3
"""Tests for the differential content classifier in compare-screenshots.py.

Grounds the renderer-gap verdict: a black candidate frame is only a Metal
GEOMETRY_GAP failure when the reference (GL) shows geometry AND the pair is
state-aligned; a fade-to-black (both legs black) must read EXPECTED_BLACK.

Run: python3 scripts/apple-silicon/test_compare_content_class.py
Exit 0 = all pass.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "compare-screenshots.py")
W = H = 256


def rich_png(path):
    # Max-entropy noise: huge palette, high edge energy, no dominant color.
    Image.frombytes("RGB", (W, H), os.urandom(W * H * 3)).save(path)


def black_png(path):
    Image.new("RGB", (W, H), (0, 0, 0)).save(path)


def magenta_png(path):
    # Missing-drawable sentinel: uniform but bright (luma ~73, not "black").
    Image.new("RGB", (W, H), (255, 0, 255)).save(path)


def run(baseline, candidate, aligned, crop=None):
    out = tempfile.mkdtemp(prefix="cc-test-")
    if crop is None:
        bw, bh = Image.open(baseline).size
        cw, ch = Image.open(candidate).size
        crop = f"0,0,{min(bw, cw)},{min(bh, ch)}"
    res = subprocess.run(
        [sys.executable, SCRIPT, baseline, candidate,
         "--crop", crop, "--resize", "smaller", "--out-dir", out,
         "--state-aligned", aligned],
        capture_output=True, text=True, check=True)
    kv = {}
    for line in res.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            kv[k] = v
    return kv


CASES = []


def case(name, mk_ref, mk_cand, aligned, expect_class, expect_fail):
    CASES.append((name, mk_ref, mk_cand, aligned, expect_class, expect_fail))


case("gap-aligned",      rich_png,  black_png,  "yes",     "METAL_GEOMETRY_GAP",            "yes")
case("gap-unaligned",    rich_png,  black_png,  "no",      "METAL_GEOMETRY_GAP_UNVERIFIED", "no")
case("gap-unknown",      rich_png,  black_png,  "unknown", "METAL_GEOMETRY_GAP_UNVERIFIED", "no")
case("expected-black",   black_png, black_png,  "yes",     "EXPECTED_BLACK",                "no")
case("content-both",     rich_png,  rich_png,   "yes",     "CONTENT_BOTH",                  "no")
case("metal-spurious",   black_png, rich_png,   "yes",     "METAL_SPURIOUS",                "yes")
# A bright uniform sentinel must NOT be absorbed into EXPECTED_BLACK.
case("sentinel-not-black", black_png, magenta_png, "yes",  "AMBIGUOUS",                     "no")


def main():
    tmp = tempfile.mkdtemp(prefix="cc-frames-")
    paths = {}
    for mk in (rich_png, black_png, magenta_png):
        p = os.path.join(tmp, mk.__name__ + ".png")
        mk(p)
        paths[mk] = p

    failures = 0
    for name, mk_ref, mk_cand, aligned, exp_class, exp_fail in CASES:
        kv = run(paths[mk_ref], paths[mk_cand], aligned)
        got_class = kv.get("content_class")
        got_fail = kv.get("content_failure")
        ok = got_class == exp_class and got_fail == exp_fail
        print(f"{'PASS' if ok else 'FAIL'}  {name:18} "
              f"class={got_class} fail={got_fail} "
              f"(ref={kv.get('baseline_state')} cand={kv.get('candidate_state')})")
        if not ok:
            failures += 1
            print(f"      expected class={exp_class} fail={exp_fail}")

    # Real-capture grounding: the two frames Josh watched (GL gameplay vs Metal
    # black). Cold-launch flip pair => unaligned => UNVERIFIED (not a hard bug).
    real = ("/Users/jbbrack03/XEMU_MacOS/xemu-fork/benchmark-runs/"
            "20260603-101131-metal-gl-compare-halo-f2-smoke")
    gl, metal = real + "/gl/screenshot.png", real + "/metal/screenshot.png"
    if os.path.exists(gl) and os.path.exists(metal):
        kv = run(gl, metal, "no")
        ok = (kv.get("baseline_state") == "content"
              and kv.get("candidate_state") == "black"
              and kv.get("content_class") == "METAL_GEOMETRY_GAP_UNVERIFIED")
        print(f"{'PASS' if ok else 'FAIL'}  real-halo-frames   "
              f"class={kv.get('content_class')} "
              f"(ref={kv.get('baseline_state')} cand={kv.get('candidate_state')})")
        if not ok:
            failures += 1
        # And the same pair *if* it were state-aligned => authoritative gap.
        kv2 = run(gl, metal, "yes")
        ok2 = kv2.get("content_class") == "METAL_GEOMETRY_GAP" and kv2.get("content_failure") == "yes"
        print(f"{'PASS' if ok2 else 'FAIL'}  real-halo-aligned  "
              f"class={kv2.get('content_class')} fail={kv2.get('content_failure')}")
        if not ok2:
            failures += 1
    else:
        print("SKIP  real-halo-frames (captures not present)")

    print(f"\n{'ALL PASS' if failures == 0 else str(failures) + ' FAILURES'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
