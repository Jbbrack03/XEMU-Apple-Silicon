# Native Triangle-Depth Validation

Date: 2026-04-30

## Build

- Commit: `ebe34071b7fec3b6187248c57708fdd6cc3a8b97`
- Branch: `apple-silicon-performance`
- Build command: `./build.sh -a arm64`
- App: `dist/xemu.app`
- Verification:
  - `codesign --verify --deep --strict --verbose=2 dist/xemu.app`: passes.
  - App executable date: `Thu Apr 30 12:17:27 CDT 2026`.
  - App reports Apple OpenGL-on-Metal:
    - GL vendor: `Apple`
    - GL renderer: `Apple M3 Ultra`
    - GL version: `4.1 Metal - 90.5`

## Coverage Counters Added

The native triangle-depth prototype now reports additional coverage counters:

- `NATIVE_TRI_DEPTH_DRAW_ZPERSPECTIVE`
- `NATIVE_TRI_DEPTH_DRAW_LINEAR_Z`
- `NATIVE_TRI_DEPTH_DRAW_POLY_OFFSET`
- `NATIVE_TRI_DEPTH_DRAW_SMOOTH`
- `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`
- `NATIVE_TRI_DEPTH_FALLBACK_FLAT`
- `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`

These counters separate "the fast path was used" from "which correctness cases
were actually exercised."

## Snapshot Coverage Runs

| ID | Game | Run directory | Duration | Post-load FPS | Post-load MSPF | Native draws | Fallbacks | W-depth | Linear depth | Polygon offset | Smooth | Flat-first |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| D11 | Rainbow Six 3 | `benchmark-runs/20260430-120458-rainbow-six-3` | 30s | 30.97 | 6.36 | 198,119 | 0 | 100,772 | 97,347 | 26,019 | 198,119 | 0 |
| D12 | Crimson Skies | `benchmark-runs/20260430-120553-crimson-skies` | 30s | 30.99 | 20.39 | 71,436 | 0 | 0 | 71,436 | 24,738 | 71,436 | 0 |
| D15 | Rainbow Six 3 post-tightening | `benchmark-runs/20260430-121927-rainbow-six-3` | 30s | 30.97 | 6.41 | 201,450 | 0 | 101,016 | 100,434 | 26,823 | 201,450 | 0 |
| D16 | Rainbow Six 3 flat-first eligibility check | `benchmark-runs/20260430-135911-rainbow-six-3` | 30s | 31.01 | 7.62 | 192,998 | 0 | 100,772 | 92,226 | 24,423 | 192,998 | 0 |

The Rainbow snapshot now proves the native path covers both depth modes plus
fill polygon offset in the same scene. Crimson covers linear depth plus fill
polygon offset. Both current snapshot scenes are entirely smooth-shaded.
D15 was run after tightening native triangle-depth eligibility to smooth-shaded
draws only; it confirms the Rainbow fast path still stays fully native in this
scene, with zero flat fallbacks because the scene has no flat-shaded triangle
fills. After D15, the eligibility rule was relaxed for flat-shaded
first-provoking triangle fills because OpenGL `GL_FIRST_VERTEX_CONVENTION`
matches that NV2A mode. D16 reran the Rainbow snapshot after that relaxation;
it remained fully native with no geometry-shader draws and no flat/fallback
counters, so the change did not perturb the existing smooth-shaded benchmark
scene.

## Route Coverage Runs

| ID | Game | Run directory | Duration | Native draws | Fallbacks | W-depth | Linear depth | Polygon offset | Smooth | Flat-first |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| D13 | Rainbow Six 3 smoke route | `benchmark-runs/20260430-120653-rainbow-six-3` | 90s | 313,378 | 0 | 51 | 313,327 | 89,376 | 313,378 | 0 |
| D14 | Crimson Skies smoke route | `benchmark-runs/20260430-120851-crimson-skies` | 90s | 328,477 | 0 | 0 | 328,477 | 92,169 | 328,477 | 0 |

The longer scripted routes also did not encounter flat-first native draws or
flat fallbacks. The implementation was therefore kept conservative after these
runs: only flat-shaded first-provoking triangle fills can join the native path,
while flat-shaded nonfirst-provoking triangle fills stay on the geometry-shader
path. A dedicated validation scene was added later in this session; its current
passing run validates the flat native/fallback split directly.

## Visual Smoke Checks

### Rainbow Six 3

Baseline geometry run:

- `benchmark-runs/20260430-115436-rainbow-six-3`
- Post-load: 30.98 FPS / 18.06 MSPF
- Geometry draws: 84,973

Native triangle-depth run:

- `benchmark-runs/20260430-115335-rainbow-six-3`
- Post-load: 30.98 FPS / 6.35 MSPF
- Native draws: 129,565
- Fallbacks: 0

10-second viewport crop comparison:

- Output: `benchmark-runs/20260430-115436-rainbow-six-3/visual-compare-native-10s`
- Crop: `641,209,1278,957`
- Mean absolute error: 0.1854
- RMS error: 1.2256
- Changed pixels above threshold 8: 5,359 / 1,223,046 (0.4382%)

Baseline/native image differences were in the same band as normal temporal
movement in the scene.

### Crimson Skies

Baseline geometry run:

- `benchmark-runs/20260430-121112-crimson-skies`
- Post-load: 30.98 FPS / 23.40 MSPF
- Geometry draws: 19,443

Native triangle-depth run:

- `benchmark-runs/20260430-121141-crimson-skies`
- Post-load: 30.98 FPS / 28.07 MSPF
- Native draws: 64,883
- Fallbacks: 0

10-second viewport crop comparison:

- Output: `benchmark-runs/20260430-121112-crimson-skies/visual-compare-native-10s`
- Crop: `641,209,1278,957`
- Mean absolute error: 0.8191
- RMS error: 2.3252
- Changed pixels above threshold 8: 21,863 / 1,223,046 (1.7876%)

Crimson has substantially more temporal movement than Rainbow in this snapshot:
baseline 10s-vs-14s changed 20.7253% of pixels and native 10s-vs-14s changed
20.6292%. The baseline/native 10s diff is much smaller than the temporal drift,
so there is no obvious visual smoke failure here either.

## Current Conclusion

The native triangle-depth path is no longer just a raw speed hack in the
current local scenes:

- It repeatedly removes geometry-shader dispatch from triangle-family fill
  draws.
- Rainbow Six 3 shows the main performance win: roughly 18 ms post-load frame
  time on the geometry path versus roughly 6.3 ms on the native path.
- The current evidence covers smooth-shaded linear depth, w-depth, and fill
  polygon offset.
- Visual smoke checks in both local games did not show an obvious regression.

The dedicated flat-shading XBE now validates the current flat policy at the
counter level. The native path allows the first-provoking case, which matches
OpenGL's configured provoking-vertex mode, and still falls back for
nonfirst-provoking flat-shaded triangle fills. Remaining correctness work should
focus on broader visual/depth validation before making the opt-in path a default
renderer path.

## Control-Plane Smoke

After adding the non-diagnostic `XEMU_NATIVE_TRI_DEPTH=1` spelling, a short
flat-tri-depth smoke run verified that the new environment variable reaches the
renderer and benchmark metadata:

- run: `benchmark-runs/20260430-173353-flat-tri-depth`
- command mode: `XEMU_NATIVE_TRI_DEPTH=1`
- log startup line: `native_tri_depth=1 source=XEMU_NATIVE_TRI_DEPTH mode=safe`
- metadata line: `env_XEMU_NATIVE_TRI_DEPTH: 1`
- final interval: `final=1 reason=atexit`
- native triangle-depth draws: 262,586
- geometry-shader draws: 0

This was an 8-second control-plane smoke, not a replacement for the longer
flat split validation below.

`XEMU_NATIVE_TRI_DEPTH=0` is also the authoritative explicit disable. If both it
and the old `XEMU_DIAG_NATIVE_TRI_DEPTH=1` alias are present, the preferred flag
wins and the native path stays disabled.

Conflict smoke run:

- run: `benchmark-runs/20260430-175500-flat-tri-depth`
- command mode: `XEMU_NATIVE_TRI_DEPTH=0` and `XEMU_DIAG_NATIVE_TRI_DEPTH=1`
- metadata recorded both environment values.
- no `native_tri_depth=1` startup line was emitted.
- `NATIVE_TRI_DEPTH_DRAW`: 0
- `GEOM_SHADER_DRAW_TRI`: 51,863

## Paired Snapshot Comparisons

Fresh same-build paired comparisons were run after introducing
`XEMU_NATIVE_TRI_DEPTH=1` and the paired comparison harness.

| ID | Game | Report directory | Baseline run | Native run | Baseline post-load MSPF | Native post-load MSPF | Geometry draws baseline/native | Native draws | Visual changed pixels |
| --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| P1 | Rainbow Six 3 | `benchmark-runs/20260430-174138-native-tri-depth-compare-rainbow-six-3` | `benchmark-runs/20260430-174138-rainbow-six-3` | `benchmark-runs/20260430-174156-rainbow-six-3` | 23.10 | 6.83 | 79,775 / 0 | 124,914 | 0.6131% |
| P2 | Crimson Skies | `benchmark-runs/20260430-174443-native-tri-depth-compare-crimson-skies` | `benchmark-runs/20260430-174443-crimson-skies` | `benchmark-runs/20260430-174500-crimson-skies` | 29.47 | 17.87 | 20,041 / 0 | 61,983 | 3.6913% |

P1 confirms the strong Rainbow result with a same-build baseline/native pair:
the native path removes all geometry-shader draws in the snapshot scene and
cuts post-load frame time by roughly 70%. The visual diff remains very small
for this scene.

P2 confirms the Crimson cross-check after one discarded baseline startup crash
in Apple's OpenGL texture-upload worker path. The successful pair removes all
geometry-shader draws and improves post-load frame time by roughly 39%. The
visual diff is larger than Rainbow, but the cropped baseline and native images
are visually aligned; the amplified diff is concentrated on texture/detail
edges rather than an obvious depth-order failure.

The first P2 baseline attempt was discarded:

- failed run: `benchmark-runs/20260430-174234-crimson-skies`
- crash report: `~/Library/Logs/DiagnosticReports/xemu-2026-04-30-174241.ips`
- stack class: Apple `GLImageWork` / `glTexImage2D` texture upload before QMP
  became ready.

`scripts/apple-silicon/native-tri-depth-compare.sh` now treats a missing perf
summary as a failed attempt and retries by default, so this failure mode no
longer produces a misleading paired report.

## Dedicated Flat-Shading XBE

A purpose-built nxdk test was added at
`scripts/apple-silicon/xbe-tests/flat-tri-depth/` and built successfully:

- XBE: `scripts/apple-silicon/xbe-tests/flat-tri-depth/bin/default.xbe`
- ISO: `scripts/apple-silicon/xbe-tests/flat-tri-depth/flat-tri-depth.iso`
- Manual-launch copy:
  `/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso`

The app sends `NV097_SET_SHADE_MODE` flat and alternates every 240 frames
between first-provoking and last-provoking `NV097_SET_PROVOKING_VERTEX`.

Validation runs:

- `benchmark-runs/20260430-141057-flat-tri-depth`
- `benchmark-runs/20260430-141439-flat-tri-depth`
- trace run: `benchmark-runs/20260430-141331-flat-tri-trace`

The method trace confirms the XBE sends the intended state:

```text
NV097_SET_SHADE_MODE 0x1d00
NV097_SET_PROVOKING_VERTEX 0x1
NV097_SET_PROVOKING_VERTEX 0x0
```

Follow-up tracing and final perf flushing validated the expected split. The
passing run is `benchmark-runs/20260430-153555-flat-tri-depth`:

- final interval: `final=1 reason=atexit`
- `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`: 480
- `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`: 304
- `GEOM_SHADER_DRAW_TRI`: 304

This means first-provoking flat triangles can use the native triangle-depth
path, while last-provoking flat triangles correctly stay on the existing
geometry-shader path.

A fresh packaged-app validation after the code cleanup added
`scripts/apple-silicon/validate-native-tri-depth.sh` as the pass/fail wrapper:

- run: `benchmark-runs/20260430-210159-flat-tri-depth`
- command: `scripts/apple-silicon/validate-native-tri-depth.sh --run 20`
- final interval: `final=1`
- `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`: 422
- `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`: 240
- `GEOM_SHADER_DRAW_TRI`: 240

The validator also confirmed that the triangle-family geometry-shader draw
count exactly matched the flat-nonfirst fallback count.

Follow-up work rebuilt the ISO and added explicit candidate counters:

- rebuilt ISO timestamp: 2026-04-30 14:38:34 CDT
- `benchmark-runs/20260430-143952-flat-tri-depth`: rebuilt ISO, still all
  native triangle-depth candidates reported smooth.
- `benchmark-runs/20260430-144128-flat-tri-depth`: method trace confirmed the
  rebuilt ISO reached flat-last draws (`SHADE_MODE 0x1d00`,
  `PROVOKING_VERTEX 0`, `DRAW_ARRAYS 0x2000003`), but counters still reported
  smooth.
- `benchmark-runs/20260430-144451-flat-tri-depth`: after adding method-owned
  PGRAPH shade/provoking fields for shader-state generation, the trace still
  showed flat-last draws and counters still reported smooth.
- `benchmark-runs/20260430-152952-flat-tri-depth`: capped
  `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` logging showed live PGRAPH state and
  bound shader state were both flat-first at bind, draw begin, and flush.
- `benchmark-runs/20260430-153555-flat-tri-depth`: after adding final
  `xemu-perf:` flushing and a graceful QMP quit wait in the benchmark harness,
  the flat counters split as expected.

The stale-ISO hypothesis and shader-state-correlation hypothesis are both
ruled out for the current flat XBE. The all-smooth summaries were caused by
the short test tail landing after the last regular perf interval.

## Category Status

The flat XBE now passes counter-level validation, and P1/P2 provide fresh
same-build retail snapshot comparisons. For the current Apple Silicon fork,
native triangle-depth is the completed current opt-in replacement for
triangle-family fill draws.

Remaining work should not keep re-proving the same slice. The 2026-05-01 PGR2,
Rainbow Six 3, and Crimson Skies gameplay routes add the next retail benchmark
layer; use those routes for future user-visible performance gates. Broader
promotion work is still needed before defaulting the path:

1. Replay the retail gameplay routes under baseline and `XEMU_NATIVE_TRI_DEPTH=1`,
   starting with PGR2.
2. Keep `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` available for targeted flat-path
   debugging, but leave it disabled for timing runs.
3. Use the final perf flush when short diagnostic tests place important draws
   near process shutdown.
4. Move the next geometry-shader removal category to quad/quad-strip expansion
   first if PGR2 confirms the expected quad-family pressure; otherwise use the
   route counters to choose line primitives, polygon fill, or nonfill
   primitives.

## Known Harness Noise

One post-tightening Rainbow attempt crashed before QMP became ready:

- `benchmark-runs/20260430-121742-rainbow-six-3`
- crash report: `~/Library/Logs/DiagnosticReports/xemu-2026-04-30-121748.ips`

The crash happened in Apple's OpenGL texture-upload worker path before any
`xemu-perf:` intervals were emitted. An immediate rerun completed cleanly as
D15, matching an earlier Crimson startup crash pattern, so this is currently
treated as nondeterministic Apple OpenGL startup behavior rather than a native
triangle-depth regression.

A flat-tri-depth trace attempt also crashed in the same Apple OpenGL worker
class:

- `benchmark-runs/20260430-152912-flat-tri-depth`
- pasted crash report: process 24357, 2026-04-30 15:29:13 CDT
- stack: `GLImageWork` / `libGLImage.dylib` during `glTexImage2D` texture
  upload before any `xemu-perf:` interval was emitted

Immediate reruns completed, including the passing
`benchmark-runs/20260430-153555-flat-tri-depth`, so this remains categorized as
nondeterministic Apple OpenGL startup/upload behavior.
