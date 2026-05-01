# Crimson Skies Gameplay Route Capture

Date: 2026-05-01

## Purpose

Capture a repeatable Crimson Skies gameplay route that reaches the known
near-target-but-choppy state: smooth opening/menu behavior followed by gameplay
that does not sustain the target framerate, with choppy acceleration animation
and occasional audio skips.

## Route

- Game: Crimson Skies
- Disc: `/Users/jbbrack03/XEMU_MacOS/Test_Games/Crimson skies.xiso.iso`
- Prepared HDD source: `benchmark-runs/profile-prep/xbox_hdd.qcow2`
- Recorded input script: `scripts/apple-silicon/input-scripts/crimson-gameplay.csv`
- Capture run: `benchmark-runs/20260501-095905-crimson-skies`
- Route length: approximately 114 seconds of recorded input.
- Recorded events: 25,935 controller events plus header.

## Command

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/record-input.sh crimson 300 \
  scripts/apple-silicon/input-scripts/crimson-gameplay.csv
```

Replay with:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/crimson-gameplay.csv 300
```

## Perf Summary

From `benchmark-runs/20260501-095905-crimson-skies/xemu.log`:

- Intervals: 107
- Timed intervals: 106
- Final intervals: 1
- Average FPS: 15.44
- Post-load average FPS after first five intervals: 15.80
- Average MSPF: 71.10
- Post-load average MSPF: 67.89
- Geometry-shader program generations: 101
- Geometry-shader binds: 264,908
- Geometry-shader draws: 786,722
- Geometry-shader triangle draws: 778,885
- Geometry-shader quad draws: 7,837
- Shader generations: 101
- Shader binds: 264,908
- Begin/end calls: 786,722
- Draw arrays: 269
- Inline elements: 696,462
- Inline arrays: 88,428
- Inline buffers: 1,563

## Notes

This route is the current retail benchmark for Crimson Skies' sustained
gameplay pacing problem. It includes normal flight input and `Y` acceleration
events, so it should exercise the choppy acceleration case observed during
manual play. The route is mostly triangle-family geometry-shader work, with a
smaller amount of quad-family geometry-shader draw activity.
