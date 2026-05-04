# 2026-05-04 Visual Flight Recorder

## Summary

The Metal route blockers are now better instrumented for visual feedback.
Single still screenshots were too thin for Crimson and SC2 because both
routes move through boot, transition, and black-frame states. This session
added a compact visual timeline analyzer and wired it into the benchmark
launcher as an opt-in post-run step.

Tooling:

- `scripts/apple-silicon/visual-flight-recorder.py`
- `XEMU_BENCH_VISUAL_ANALYSIS=1` in
  `scripts/apple-silicon/run-benchmark.sh`

The analyzer consumes either a PNG sequence or a short video. It writes:

- `visual-summary.json`
- `timeline.csv`
- `storyboard.jpg`
- selected `keyframes/`

Video inputs are sampled with `ffmpeg` into a temporary directory and the
extracted frames are deleted automatically unless `--keep-temp` is explicitly
used.

## Validation

Static checks:

```sh
bash -n scripts/apple-silicon/run-benchmark.sh
python3 -m py_compile scripts/apple-silicon/visual-flight-recorder.py
```

Existing failed sequence summaries:

| Route | Input frames | Result |
| --- | --- | --- |
| Crimson gameplay | `benchmark-runs/visual-checks/crimson-gameplay-gate-msaa4-after-msaa-store*.png` | 13 frames, 92.31 % black, longest black run starts at frame index 1 and lasts 12 frames |
| SC2 no-input | `benchmark-runs/visual-checks/sc2-gate-metal-msaa4-interval-after-msaa-store*.png` | 8 frames, 87.50 % black, longest black run starts at frame index 1 and lasts 7 frames |

Video path validation:

- Built a temporary MP4 fixture from the SC2 PNG sequence with `ffmpeg`.
- Ran `visual-flight-recorder.py --video /tmp/xemu-vfr-test.mp4`.
- Confirmed no stale `xemu-visual-frames-*` temp directories remained.

End-to-end benchmark hook validation:

```sh
XEMU_RENDERER=METAL \
XEMU_METAL_TRANSLATED_PIPELINE=1 \
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_NATIVE_QUAD=1 \
XEMU_PGRAPH_FAST_READ=1 \
XEMU_METAL_FRONT_FB_FALLBACK=1 \
XEMU_METAL_MSAA=4 \
XEMU_METAL_SCREENSHOT_INTERVAL=120 \
XEMU_BENCH_VISUAL_ANALYSIS=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
XEMU_PERF_FRAME_LOG=1 \
scripts/apple-silicon/run-benchmark.sh \
  --metal-screenshot /tmp/xemu-vfr-e2e-sc2.png \
  --metal-screenshot-at-frame 60 \
  sc2 scripts/apple-silicon/input-scripts/noop.csv 20
```

Output:

- Run directory:
  `benchmark-runs/20260504-113305-soul-calibur-2`
- Storyboard:
  `benchmark-runs/20260504-113305-soul-calibur-2/visual-analysis/storyboard.jpg`
- Report:
  `benchmark-runs/20260504-113305-soul-calibur-2/visual-analysis/visual-summary.json`

The short SC2 run produced 10 sampled frames. It shows boot/early dark
frames, flubber, patterned transition frames, then black output. This is a
route/visual-observation confirmation, not a renderer correctness fix.

## Next Steps

1. Use `XEMU_BENCH_VISUAL_ANALYSIS=1` for the next Crimson gameplay and SC2
   route investigations.
2. For Metal route-debug runs, prefer sampled Metal renderer PNGs with
   `XEMU_METAL_SCREENSHOT_INTERVAL=N`; use short video only when animation
   timing or flicker needs a denser timeline.
3. Keep raw extracted frames out of `benchmark-runs/`. Preserve only compact
   visual-analysis outputs and failing-run source PNGs that are intentionally
   part of the audit trail.
4. After Crimson and SC2 produce valid rendered gameplay timelines, run the
   paired Metal-vs-GL visual/perf gate.
