# Automation Smoke Routes

Date: 2026-04-30

## Build

- Commit: `ebe34071b7fec3b6187248c57708fdd6cc3a8b97`
- Branch: `apple-silicon-performance`
- Build command: `./build.sh -a arm64`
- App: `dist/xemu.app`
- Renderer observed in logs:
  - GL vendor: `Apple`
  - GL renderer: `Apple M3 Ultra`
  - GL version: `4.1 Metal - 90.5`

## Harness

- Scripted input source: `XEMU_SCRIPTED_INPUT`
- Launcher: `scripts/apple-silicon/run-benchmark.sh`
- Screenshot backend while tuning: `XEMU_BENCH_SCREENSHOT_BACKEND=none`
- Live visual verification: Computer Use screenshots of the xemu window

## Runs

| Game | Run directory | Duration | Result |
| --- | --- | ---: | --- |
| Crimson Skies | `benchmark-runs/20260430-092632-crimson-skies` | 90s | Reached pilot registration, accepted generated pilot name, advanced into rendered in-engine sequence. |
| Rainbow Six 3 | `benchmark-runs/20260430-093903-rainbow-six-3` | 160s | Reached default `CHAVEZ` profile, selected Campaign, advanced to Hereford mission loading screen. |

## Notes

- Crimson Skies profile entry is not clean yet; it generates a throwaway pilot
  name before selecting Done. This is acceptable for route setup because each
  run uses a scratch HDD copy.
- Rainbow Six 3 needed a delayed first `Start` because the title screen is not
  ready until roughly 35 seconds into boot on this baseline.
- Analog stick input must be delayed in Rainbow Six 3; otherwise it scrolls the
  main menu before Campaign is selected.
- The launcher now refuses to start when another xemu process is already
  running and tears down run-owned xemu processes after the timed run.

## Capture Caveats

- QMP `screendump` crashed the Apple OpenGL-on-Metal path during testing.
- macOS `screencapture` failed from the Codex desktop context with
  `could not create image from display`.
- Use Computer Use screenshots for interactive route tuning until a reliable
  capture path is added.

## Follow-Up

- Frame-pacing and FPS logging has been added through `XEMU_PERF_LOG=1`.
- Scene snapshots were saved and restored through QMP/HMP.
- B0/B1 baseline metrics, B2/B3 geometry-counter runs, native triangle-depth
  diagnostics, and flat XBE validation runs are recorded in
  `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`.
- The generated flat-tri-depth XBE now validates the flat native/fallback
  counter split. Later paired Rainbow/Crimson comparisons are recorded in
  `docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md`.
- Later 2026-05-01 retail gameplay routes are recorded for PGR2, Rainbow Six 3,
  and Crimson Skies. Use those route files for the next user-visible
  performance gate; the next benchmark/renderer work should start with PGR2
  and remaining geometry-shader exit categories rather than re-running this
  same triangle-family fill slice.
