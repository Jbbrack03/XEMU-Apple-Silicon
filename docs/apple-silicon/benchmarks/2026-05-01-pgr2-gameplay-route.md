# PGR2 Gameplay Route Capture

Date: 2026-05-01

## Purpose

Capture a repeatable Project Gotham Racing 2 gameplay route that reaches the
known poor-performance state: smooth opening/menu behavior followed by very
choppy gameplay with audio and video pacing collapse.

## Route

- Game: Project Gotham Racing 2
- Disc:
  `/Volumes/Final Cut Pro Libraries/Projects/XEMU_MacOS/Test_Games/PGR2.xiso.iso`
- Prepared HDD source:
  `benchmark-runs/profile-prep/xbox_hdd.qcow2`
- Recorded input script:
  `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv`
- Capture run:
  `benchmark-runs/20260501-094823-pgr2`
- Route length: approximately 177 seconds of recorded input.
- Recorded events: 33,493 controller events plus header.

## Command

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/record-input.sh pgr2 300 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv
```

Replay with:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

## Perf Summary

From `benchmark-runs/20260501-094823-pgr2/xemu.log`:

- Intervals: 133
- Timed intervals: 132
- Final intervals: 1
- Average FPS: 11.53
- Post-load average FPS after first five intervals: 11.67
- Geometry-shader program generations: 314
- Geometry-shader binds: 251,849
- Geometry-shader draws: 1,516,519
- Geometry-shader triangle draws: 1,477,734
- Geometry-shader quad draws: 38,785
- Shader generations: 322
- Shader binds: 257,154
- Begin/end calls: 1,545,485
- Draw arrays: 25,414
- Inline elements: 1,467,934
- Inline buffers: 51,368

## Notes

This route is the strongest current retail benchmark for severe Apple OpenGL
geometry-shader pressure. The capture includes both triangle-family and
quad-family geometry-shader draw activity, making it more useful than the
earlier triangle-heavy Rainbow Six 3 and Crimson Skies snapshot scenes for the
next renderer optimization slice.
