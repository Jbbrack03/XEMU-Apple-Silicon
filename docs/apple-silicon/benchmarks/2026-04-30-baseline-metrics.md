# Baseline Metrics and Snapshot Smoke

Date: 2026-04-30

## Build

- Commit: `ebe34071b7fec3b6187248c57708fdd6cc3a8b97`
- Branch: `apple-silicon-performance`
- Build command: `./build.sh -a arm64`
- App: `dist/xemu.app`
- Verification:
  - `codesign --verify --deep --strict --verbose=2 dist/xemu.app`: passes
  - executable: Mach-O 64-bit arm64
  - bundled executable rpath count: one `@executable_path/../Libraries/arm64/`

## Renderer

- Backend: OpenGL
- GL vendor: `Apple`
- GL renderer: `Apple M3 Ultra`
- GL version: `4.1 Metal - 90.5`
- GL shading language: `4.10`

## Harness Changes Used

- `XEMU_PERF_LOG=1` enables once-per-second `xemu-perf:` lines in `xemu.log`.
- `XEMU_BENCH_SCREENSHOT_BACKEND=none` was used because QMP screendump and
  macOS screencapture are not reliable from this Codex desktop context.
- Snapshot save/load was driven through QMP `human-monitor-command` after xemu
  startup. Command-line `-loadvm` was rejected because xemu's dynamic USB/input
  device tree was not ready yet.
- Benchmark snapshot runs set `XEMU_SNAPSHOT_NO_THUMBNAIL=1` to avoid OpenGL
  thumbnail capture/loading during automated save/restore.

## Baseline Runs

| ID | Game | Run directory | Duration | Intervals | Avg FPS | Tail-60 FPS | Tail-60 MSPF | Notes |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| B0 | Crimson Skies | `benchmark-runs/20260430-095612-crimson-skies` | 150s | 139 | 29.93 | 30.98 | 27.27 | Boot/menu intervals include large stalls; late route settles near 31 FPS. |
| B1 | Rainbow Six 3 | `benchmark-runs/20260430-095919-rainbow-six-3` | 190s | 174 | 26.45 | 30.98 | 18.23 | Boot/menu intervals are noisy; late route reaches the Hereford loading flow. |

The overall averages include startup, profile creation, menus, and loading.
The tail-60 values are the better baseline comparison target until named
snapshots become the normal benchmark entry point.

## Snapshot Runs

| Game | Snapshot run | Snapshot tag | Save time | VM clock | Restore run | Restore result |
| --- | --- | --- | ---: | --- | --- | --- |
| Crimson Skies | `benchmark-runs/20260430-100438-crimson-skies` | `crimson_scene_b0` | 120s | `0000:01:59.687` | `benchmark-runs/20260430-100944-crimson-skies` | Restored through QMP; 28 intervals, avg 29.52 FPS / 26.80 MSPF. |
| Rainbow Six 3 | `benchmark-runs/20260430-101703-rainbow-six-3` | `rainbow_scene_b1_nothumb` | 165s | `0000:02:44.693` | `benchmark-runs/20260430-102023-rainbow-six-3` | Restored through QMP; 28 intervals, avg 29.22 FPS / 27.95 MSPF. |

## Geometry Counter Runs

After adding OpenGL geometry-shader perf counters, the snapshot scenes were
re-run from the same saved HDD images. Screenshot capture remained disabled.

| ID | Game | Run directory | Duration | Intervals | Avg FPS | Post-load FPS | Post-load MSPF | Geometry modules | Geometry binds | Geometry draws | Triangle geometry draws |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| B2 | Crimson Skies | `benchmark-runs/20260430-103500-crimson-skies` | 30s | 28 | 29.70 | 30.98 | 20.63 | 3 | 12,343 | 25,202 | 25,202 |
| B3 | Rainbow Six 3 | `benchmark-runs/20260430-103536-rainbow-six-3` | 30s | 27 | 29.23 | 30.97 | 17.66 | 4 | 61,180 | 149,961 | 149,961 |

`Post-load` excludes the first five one-second intervals so QMP restore and
early scene settling do not dominate the comparison. Both scenes route all
geometry-shader-backed draws through triangle-family primitives. Rainbow Six 3
is the stronger diagnostic target for geometry-shader overhead because it
issues roughly six times as many geometry-backed draws as Crimson over the same
duration.

## Diagnostic and Prototype Runs

`XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` keeps triangle-family geometry shaders
enabled but replaces their depth-plane/slope work with a simple pass-through
depth payload. This is a performance diagnostic only; rendering correctness was
not visually validated for this run.

`XEMU_DIAG_SKIP_TRI_GEOM=1` bypasses geometry-shader program generation for
triangle-family fill draws and draws native GL triangles directly. This is also
a performance diagnostic only; it is expected to lose correct per-triangle depth
payload behavior.

`XEMU_NATIVE_TRI_DEPTH=1` is the preferred spelling for the current opt-in
triangle-family fill replacement path. It bypasses triangle-family fill geometry
shaders and lets the fragment shader derive depth and polygon-slope offset from
native GL rasterization state (`gl_FragCoord`). The older
`XEMU_DIAG_NATIVE_TRI_DEPTH=1` spelling remains a compatibility alias for the
historical runs in this table. Later D17/P1/P2 work validated this slice for the
current opt-in coverage. It is not the default renderer path yet. Nonfirst
flat-shaded triangle fills still fall back to the geometry-shader path.

| ID | Toggle | Game | Run directory | Duration | Intervals | Avg FPS | Post-load FPS | Post-load MSPF | Geometry modules | Geometry programs | Geometry binds | Geometry draws | Native tri-depth draws | Native tri-depth fallbacks |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| D1 | `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` | Rainbow Six 3 | `benchmark-runs/20260430-104001-rainbow-six-3` | 30s | 25 | 28.24 | 30.72 | 18.39 | 4 | 52 | 52,414 | 128,277 | n/a | n/a |
| D2 | `XEMU_DIAG_SKIP_TRI_GEOM=1` | Rainbow Six 3 | `benchmark-runs/20260430-105200-rainbow-six-3` | 30s | 27 | 29.93 | 30.96 | 6.38 | 0 | 0 | 0 | 0 | n/a | n/a |
| D3 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | Rainbow Six 3 | `benchmark-runs/20260430-110636-rainbow-six-3` | 30s | 27 | 29.49 | 30.96 | 8.10 | 0 | 0 | 0 | 0 | n/a | n/a |
| D4 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | Crimson Skies | `benchmark-runs/20260430-111006-crimson-skies` | 30s | 28 | 31.15 | 30.98 | 18.97 | 0 | 0 | 0 | 0 | n/a | n/a |
| D5 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | Rainbow Six 3 | `benchmark-runs/20260430-111903-rainbow-six-3` | 30s | 28 | 30.89 | 30.99 | 6.35 | 0 | 0 | 0 | 0 | n/a | n/a |
| D6 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | Crimson Skies | `benchmark-runs/20260430-112058-crimson-skies` | 30s | 28 | 31.37 | 30.98 | 27.53 | 0 | 0 | 0 | 0 | n/a | n/a |
| D7 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | Crimson Skies | `benchmark-runs/20260430-112157-crimson-skies` | 30s | 28 | 31.26 | 30.98 | 20.30 | 0 | 0 | 0 | 0 | n/a | n/a |
| D8 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | Rainbow Six 3 | `benchmark-runs/20260430-113642-rainbow-six-3` | 30s | 28 | 30.99 | 30.99 | 6.47 | 0 | 0 | 0 | 0 | 197,212 | 0 |
| D9 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | Crimson Skies | `benchmark-runs/20260430-113732-crimson-skies` | 30s | 28 | 31.32 | 30.98 | 20.01 | 0 | 0 | 0 | 0 | 71,277 | 0 |
| D10 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | Rainbow Six 3 | `benchmark-runs/20260430-114511-rainbow-six-3` | 30s | 28 | 30.93 | 30.98 | 6.78 | 0 | 0 | 0 | 0 | 192,776 | 0 |

D1 did not improve over B3. The post-load average was slightly lower than B3
and one late interval had a 162 ms frame-time spike. This suggests the Apple
OpenGL performance problem is more likely tied to geometry shader dispatch,
driver behavior, or surrounding pipeline work than to the triangle depth/slope
math inside the geometry shader.

D2 removed the geometry-shader path from triangle-family fill draws in the
Rainbow Six 3 snapshot scene. FPS remained capped near 31 FPS, but post-load
average frame time fell from B3's 17.66 ms to 6.38 ms and geometry draw counters
dropped to zero. This strongly implicates geometry-shader dispatch or Apple's
OpenGL geometry-shader implementation rather than arithmetic inside the
geometry shader body.

D3 keeps triangle-family fill draws on native GL triangles while making the
fragment shader derive `zvalue` and the polygon-slope term from
`gl_FragCoord`. Rainbow Six 3 remained capped near 31 FPS and post-load frame
time stayed much better than B3 at 8.10 ms, but still slower than D2's
correctness-breaking 6.38 ms. This suggests a practical GL replacement path may
exist, with some remaining fragment-side cost and correctness work.

D4 cross-checked the same native triangle-depth prototype against Crimson
Skies. The scene remained capped near 31 FPS, with post-load frame time close
to the B2 geometry-counter baseline. This scene is less sensitive than Rainbow
Six 3 because it has far fewer geometry-backed triangle draws.

D5 reran Rainbow Six 3 after tightening the diagnostic skip condition so it
only bypasses geometry shaders for triangle-family fill primitives, leaving
line primitives on the existing geometry path. The scene still reported zero
geometry-backed draws and post-load frame time fell to 6.35 ms. D5 is now a
historical comparison point; use the P1/P2 paired runs in
`docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md` for
current same-build evidence.

D6/D7 reran Crimson Skies with the same line-safe diagnostic skip. Both still
reported zero geometry-backed draws. D6 had a slower 27.53 ms post-load average,
while the immediate repeat D7 returned to 20.30 ms. Treat Crimson as a useful
cross-check but not the primary timing signal; Rainbow Six 3 remains the better
geometry-dispatch scene.

D8/D9 reran the native triangle-depth prototype after tightening the eligibility
rule for flat shading. At that point the native path still allowed flat-first
draws, but both current snapshot scenes were entirely smooth-shaded and reported
zero fallbacks. Rainbow Six 3 stayed at the D5-level performance band, with
6.47 ms post-load frame time and 197,212 native triangle-depth draws.

D10 repeated the tightened native triangle-depth path on Rainbow Six 3 using
`scripts/apple-silicon/extract-perf-summary.sh` for the summary. It confirmed
the D8 performance band with 6.78 ms post-load frame time, 192,776 native
triangle-depth draws, zero native fallbacks, and zero geometry-shader module,
program, bind, or draw counters.

D17 created the missing flat-shading validation asset rather than relying on
Crimson or Rainbow to encounter a flat-shaded scene. The nxdk test lives in
`scripts/apple-silicon/xbe-tests/flat-tri-depth/`, builds
`bin/default.xbe` and `flat-tri-depth.iso`, and is also copied to
`/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso`. It boots
through the benchmark harness, and the trace run
`benchmark-runs/20260430-141331-flat-tri-trace` confirms it sends
`NV097_SET_SHADE_MODE` flat plus both first and last provoking-vertex modes.
Follow-up runs rebuilt the ISO and ruled out stale media:
`benchmark-runs/20260430-143952-flat-tri-depth`,
`benchmark-runs/20260430-144128-flat-tri-depth`, and
`benchmark-runs/20260430-144451-flat-tri-depth`. Later capped native-tri-depth
tracing showed live PGRAPH state and bound shader state both become flat-first,
and final perf flushing fixed the short-tail measurement gap. Passing run
`benchmark-runs/20260430-153555-flat-tri-depth` reports 480
`NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, 304
`NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`, and 304 `GEOM_SHADER_DRAW_TRI`.
The reusable regression gate is
`scripts/apple-silicon/validate-native-tri-depth.sh --run 20`; fresh packaged
app validation in `benchmark-runs/20260430-210159-flat-tri-depth` passed with
422 flat-first native draws, 240 flat-nonfirst fallbacks, and 240
triangle-family geometry-shader draws.

## Visual Smoke Check

After macOS `screencapture` became usable in this Codex desktop session, the
Rainbow Six 3 snapshot was captured once on the baseline geometry-shader path
and once on the native triangle-depth prototype path.

| ID | Path | Run directory | Duration | Post-load FPS | Post-load MSPF | Geometry draws | Native tri-depth draws | Native fallbacks | Screenshots |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| C1 | baseline geometry shader | `benchmark-runs/20260430-115436-rainbow-six-3` | 16s | 30.98 | 18.06 | 84,973 | 0 | 0 | `screenshots/000-006000ms.png`, `001-010000ms.png`, `002-014000ms.png` |
| C2 | `XEMU_DIAG_NATIVE_TRI_DEPTH=1` | `benchmark-runs/20260430-115335-rainbow-six-3` | 16s | 30.98 | 6.35 | 0 | 129,565 | 0 | `screenshots/000-006005ms.png`, `001-010001ms.png`, `002-014005ms.png` |

The 10-second screenshots were compared with
`scripts/apple-silicon/compare-screenshots.py` over crop
`641,209,1278,957`, covering the xemu guest viewport and excluding desktop
chrome.

| Comparison | Output directory | Mean abs error | RMS error | Max abs error | Changed pixels > 8 | Changed pixels |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Baseline 10s vs native 10s | `benchmark-runs/20260430-115436-rainbow-six-3/visual-compare-native-10s` | 0.1854 | 1.2256 | 98 | 5,359 / 1,223,046 | 0.4382% |
| Baseline 10s vs baseline 14s | `benchmark-runs/20260430-115436-rainbow-six-3/visual-compare-baseline-temporal` | 0.2586 | 0.9648 | 59 | 3,218 / 1,223,046 | 0.2631% |
| Native 10s vs native 14s | `benchmark-runs/20260430-115335-rainbow-six-3/visual-compare-native-temporal` | 0.5226 | 2.0168 | 68 | 12,945 / 1,223,046 | 1.0584% |

Visual inspection of the baseline/native crop and the amplified diff did not
show an obvious native-triangle rendering break in this static Rainbow scene.
The numeric difference is in the same band as normal temporal movement in the
scene. This is a smoke check only; it does not validate depth ordering,
polygon-offset behavior, or flat-shading edge cases.

Fresh same-build visual/performance comparisons are recorded in
`docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md`:

- P1 Rainbow Six 3: 23.10 baseline MSPF versus 6.83 native MSPF, 79,775 baseline
  geometry draws versus 0 native-run geometry draws, and 0.6131% changed pixels.
- P2 Crimson Skies: 29.47 baseline MSPF versus 17.87 native MSPF, 20,041
  baseline geometry draws versus 0 native-run geometry draws, and 3.6913%
  changed pixels.

## Snapshot Caveats

- `-loadvm crimson_scene_b0` failed with a USB device-tree mismatch. Loading by
  QMP/HMP after xemu startup fixed this.
- A Rainbow snapshot saved with the default thumbnail path could be listed, but
  launching from that snapshot-bearing HDD crashed Apple's OpenGL worker path
  before QMP became available. Benchmark snapshots now skip thumbnails.
- One Crimson D3 attempt (`benchmark-runs/20260430-110740-crimson-skies`)
  crashed before QMP became available, with the macOS crash report pointing at
  Apple's `GLImageWork` texture upload path. The immediate rerun
  (`benchmark-runs/20260430-111006-crimson-skies`) completed, so this is
  currently treated as a nondeterministic Apple OpenGL startup failure rather
  than a confirmed D3 shader failure.
- Snapshot-bearing HDDs are per-run artifacts. Use `XEMU_BENCH_HDD_SOURCE` to
  copy from the run directory that contains the snapshot.

## Conclusion

Baseline measurement, geometry-path attribution, two triangle diagnostics, the
native triangle-depth replacement path, a dedicated flat-shading XBE, and paired
same-build retail comparisons are now available. The current
triangle-family fill slice is validated for current opt-in Apple Silicon testing,
but remains disabled by default until broader retail coverage exists. Later
2026-05-01 gameplay route captures provide that next retail benchmark layer:
PGR2, Rainbow Six 3, and Crimson Skies now have recorded controller routes and
baseline perf summaries. Next renderer work should start with PGR2, then target
one remaining geometry-shader user, likely quad/quad-strip expansion first if
the PGR2 baseline versus `XEMU_NATIVE_TRI_DEPTH=1` replay confirms the expected
quad-family pressure.
