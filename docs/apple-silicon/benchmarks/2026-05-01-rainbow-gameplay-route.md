# Rainbow Six 3 Gameplay Route Capture

Date: 2026-05-01

## Purpose

Capture a repeatable Rainbow Six 3 gameplay route that reaches the known
movement-triggered slowdown: smooth opening/menu behavior and smooth stationary
gameplay followed by choppy gameplay once character movement begins.

## Route

- Game: Rainbow Six 3
- Disc: `/Users/jbbrack03/XEMU_MacOS/Test_Games/Rainbow Six 3.xiso.iso`
- Prepared HDD source: `benchmark-runs/profile-prep/xbox_hdd.qcow2`
- Recorded input script: `scripts/apple-silicon/input-scripts/rainbow-gameplay.csv`
- Capture run: `benchmark-runs/20260501-095400-rainbow-six-3`
- Route length: approximately 166 seconds of recorded input.
- Recorded events: 61,367 controller events plus header.

## Command

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/record-input.sh rainbow 300 \
  scripts/apple-silicon/input-scripts/rainbow-gameplay.csv
```

Replay with:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/rainbow-gameplay.csv 300
```

## Perf Summary

From `benchmark-runs/20260501-095400-rainbow-six-3/xemu.log`:

- Intervals: 152
- Timed intervals: 151
- Final intervals: 1
- Average FPS: 24.19
- Post-load average FPS after first five intervals: 24.76
- Geometry-shader program generations: 349
- Geometry-shader binds: 211,990
- Geometry-shader draws: 692,438
- Geometry-shader line draws: 1,946
- Geometry-shader triangle draws: 690,492
- Shader generations: 349
- Shader binds: 211,990
- Begin/end calls: 692,438
- Draw arrays: 88,443
- Inline elements: 568,404
- Inline arrays: 21,382
- Inline buffers: 14,209

## Notes

This route is the current retail benchmark for movement-triggered gameplay
slowdown and visual artifact coverage. Unlike the PGR2 route, this capture is
mostly triangle-family geometry-shader work, but it also includes line-family
geometry-shader draws. That makes it useful for validating that future
triangle-family optimizations do not accidentally hide remaining line-path
overhead or artifact risks.
