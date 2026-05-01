# Decision Log

## 2026-04-29: Treat Apple Silicon work as a fork

Decision:

This project will optimize for Apple Silicon macOS even if the approach becomes
too invasive for easy upstream compatibility.

Rationale:

The user explicitly wants a fork-quality solution and does not want us blocked
by upstream acceptability. The renderer and platform changes are likely large
enough to diverge substantially.

## 2026-04-29: Do not use permanent downgrade/revert as the final fix

Decision:

We may temporarily gate or disable the PR #2240 geometry-heavy path for
diagnosis, but the final plan is not "just revert #2240."

Rationale:

PR #2240 fixes real depth precision, polygon offset, flat shading, and primitive
behavior issues. Reverting improves performance by discarding correctness work.
The correct approach is to preserve the intended behavior through a more
Apple-friendly pipeline.

## 2026-04-29: Geometry shader dependency is a primary blocker

Decision:

Eliminating geometry-shader dependence is the first major renderer refactor.

Rationale:

Public macOS regression data implicates heavier geometry shader usage, and the
current code uses geometry shaders for most primitive types. Vulkan-over-Metal
and native Metal paths both become simpler and more robust if this dependency is
removed.

## 2026-04-29: Metal is the preferred final Apple Silicon backend

Decision:

The fork should aim for a native Metal renderer as the long-term fast path.
Vulkan-over-Metal should be prototyped and measured, not assumed.

Rationale:

Apple positions Metal as the modern GPU API replacing OpenGL. MoltenVK and
KosmicKrisp are promising, but xemu has unusual emulator workloads and currently
requires features that may not map cheaply. A native Metal path gives the fork
the most control.

## 2026-04-29: Benchmarking must precede irreversible architecture choices

Decision:

Before committing deeply to Metal-only or Vulkan-over-Metal-first, collect
baseline measurements and feature-probe data.

Rationale:

We have strong evidence for the problem class, but not yet local measurements
for this machine, these games, or current SDK/driver behavior.

## 2026-04-29: Keep macOS build/package fixes in-tree

Decision:

Small macOS build and packaging fixes are allowed before renderer work when
they are required to produce a runnable Apple Silicon baseline.

Rationale:

The baseline arm64 build initially failed because Meson's cross-build path did
not use the default `cmake` binary. After that was fixed, the packaged app
failed at launch because macOS `dyld` rejected duplicate `LC_RPATH` entries.
Without fixing those in `build.sh`, benchmark runs would depend on manual,
easy-to-forget shell workarounds.

Verification:

- `./build.sh -a arm64` succeeds.
- `dist/xemu.app` passes code-sign verification.
- `dist/xemu.app/Contents/MacOS/xemu --version` launches and reports the Apple
  OpenGL-on-Metal renderer.

## 2026-04-30: Use QMP/HMP for benchmark snapshot restore

Decision:

Benchmark scene snapshots should be restored after xemu startup through QMP/HMP
instead of by passing `-loadvm` on the command line.

Rationale:

Initial CLI `-loadvm` testing failed against a Crimson Skies snapshot with a
saved USB hub device-tree mismatch. Starting xemu normally and then sending HMP
`loadvm` through the QMP socket restored the same snapshot successfully, and the
same path also restored the Rainbow Six 3 scene snapshot.

Verification:

- Crimson snapshot `crimson_scene_b0` restored from
  `benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2`.
- Rainbow snapshot `rainbow_scene_b1_nothumb` restored from
  `benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2`.

## 2026-04-30: Disable benchmark snapshot thumbnails by default

Decision:

Benchmark-created snapshots should set `XEMU_SNAPSHOT_NO_THUMBNAIL=1` by
default.

Rationale:

A Rainbow Six 3 scratch HDD containing a normal thumbnail-bearing snapshot
crashed Apple's OpenGL-on-Metal worker path during a later benchmark launch
before QMP restore. A thumbnail-free Rainbow snapshot saved at the same scene
restored successfully. This keeps benchmark snapshots focused on deterministic
scene entry while avoiding a separate thumbnail/render-thread failure mode.

## 2026-04-30: Attribute geometry-shader work before replacing it

Decision:

Add OpenGL geometry-shader attribution counters to the existing `xemu-perf:`
interval log before attempting larger renderer changes.

Rationale:

The public macOS regression points at geometry shaders, but the fork needs
local per-scene evidence. Counting geometry module/program generation, binds,
and draw calls by primitive family lets snapshot runs identify which game scene
is the better diagnostic target and whether a change affects the suspected
path.

Verification:

- B2 Crimson Skies: 25,202 geometry-backed draws in 30s, all triangle-family.
- B3 Rainbow Six 3: 149,961 geometry-backed draws in 30s, all
  triangle-family.
- Rainbow Six 3 is the stronger local geometry-shader diagnostic scene.

## 2026-04-30: Triangle depth arithmetic is not the first bottleneck

Decision:

Keep `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` as a temporary diagnostic, but do
not pursue triangle depth/slope arithmetic simplification as the next
performance path.

Rationale:

D1 kept triangle-family geometry shaders active while bypassing their
depth-plane and slope calculation. Rainbow Six 3 did not improve compared with
B3, and it showed one late 162 ms frame-time spike. That makes geometry-shader
dispatch, Apple OpenGL driver behavior, or surrounding pipeline work a better
next target than arithmetic inside the geometry shader.

Verification:

- D1 Rainbow Six 3: 30.72 FPS post-load / 18.39 MSPF.
- B3 Rainbow Six 3: 30.97 FPS post-load / 17.66 MSPF.

## 2026-04-30: Geometry-shader dispatch is the next replacement target

Decision:

Prioritize a correctness-preserving triangle path that avoids GL geometry
shader dispatch before Vulkan-over-Metal experiments.

Rationale:

`XEMU_DIAG_SKIP_TRI_GEOM=1` bypassed geometry-shader program generation for
triangle-family fill draws in the Rainbow Six 3 snapshot scene. It is knowingly
incorrect because the fragment shader no longer receives the geometry shader's
per-triangle depth payload, but it reduced post-load average frame time from
B3's 17.66 ms to 6.38 ms and dropped geometry draw counters to zero. That is a
stronger signal than D1's arithmetic simplification result.

Verification:

- D2 Rainbow Six 3: 30.96 FPS post-load / 6.38 MSPF.
- D2 geometry draw counters: 0.
- B3 Rainbow Six 3: 30.97 FPS post-load / 17.66 MSPF.

## 2026-04-30: Native triangle depth is the first GL replacement prototype

Decision:

Use `XEMU_DIAG_NATIVE_TRI_DEPTH=1` as the next experimental branch for removing
triangle-family GL geometry-shader dispatch while preserving the depth and
polygon-offset behavior in the fragment shader.

Rationale:

D3 bypassed triangle-family fill geometry shaders and derived `zvalue` plus the
polygon-slope term from `gl_FragCoord`. It kept geometry draw counters at zero
and retained most of D2's frame-time improvement in Rainbow Six 3, while being
much closer to a correctness-preserving design than simply dropping the
geometry payload. The prototype still needs visual and depth-correctness
validation before it can become the normal renderer path.

Verification:

- D3 Rainbow Six 3: 30.96 FPS post-load / 8.10 MSPF, geometry draw counters: 0.
- D5 Rainbow Six 3 after narrowing the diagnostic skip to triangle-family fill
  primitives only: 30.99 FPS post-load / 6.35 MSPF, geometry draw counters: 0.
- D4 Crimson Skies: 30.98 FPS post-load / 18.97 MSPF, geometry draw counters:
  0.
- D7 Crimson Skies after the same line-safe narrowing: 30.98 FPS post-load /
  20.30 MSPF, geometry draw counters: 0.
- B3 Rainbow Six 3: 30.97 FPS post-load / 17.66 MSPF.

## 2026-04-30: Keep flat shading on the geometry-shader path

Status: superseded later on 2026-04-30 by the first-provoking flat-shading
eligibility expansion and the dedicated flat-tri-depth XBE. This entry records
the earlier conservative checkpoint.

Decision:

`XEMU_DIAG_NATIVE_TRI_DEPTH=1` should bypass triangle-family fill geometry
shaders only for smooth-shaded draws. All flat-shaded triangle fills should
fall back to the geometry-shader path until a dedicated flat/provoking-vertex
validation scene exists.

Rationale:

The OpenGL renderer globally uses `GL_FIRST_VERTEX_CONVENTION`. The geometry
shader can emulate flat provoking-vertex behavior by copying the selected
vertex's flat attributes to every emitted vertex. The native triangle path has
not yet been validated against flat-shaded cases, and current local scenes do
not exercise them, so keeping all flat shading on the geometry-shader path is
the safer prototype boundary.

Verification:

- D8 Rainbow Six 3: 30.99 FPS post-load / 6.47 MSPF, geometry draw counters: 0,
  native triangle-depth draws: 197,212, fallbacks: 0.
- D10 Rainbow Six 3 confirmation: 30.98 FPS post-load / 6.78 MSPF, geometry
  draw counters: 0, native triangle-depth draws: 192,776, fallbacks: 0.
- D15 Rainbow Six 3 after the smooth-only tightening: 30.97 FPS post-load /
  6.41 MSPF, geometry draw counters: 0, native triangle-depth draws: 201,450,
  fallbacks: 0, all smooth.
- D9 Crimson Skies: 30.98 FPS post-load / 20.01 MSPF, geometry draw counters: 0,
  native triangle-depth draws: 71,277, fallbacks: 0.

## 2026-04-30: Native triangle-depth coverage is strong except flat shading

Status: superseded later on 2026-04-30. Smooth-depth coverage is still valid,
and the dedicated flat-shading XBE now validates the first-provoking
native-path / nonfirst-provoking fallback split.

Decision:

Keep the native triangle-depth prototype focused on validated cases, and treat
flat shading as the remaining promotion blocker until direct XBE evidence is
clean.

Rationale:

Additional coverage counters show the current Rainbow and Crimson workloads
exercise the important depth math cases that were previously only inferred:
linear depth, w-depth, and fill polygon offset. Both games stayed entirely on
smooth shading in the measured scenes and scripted routes, so they cannot
validate flat-shaded native rendering. At this point the implementation was
kept smooth-only; later work allowed first-provoking flat triangles and added a
purpose-built XBE to validate that boundary directly.

Verification:

- D11 Rainbow snapshot: 198,119 native draws, 0 fallbacks, 100,772 w-depth,
  97,347 linear-depth, 26,019 polygon-offset, all smooth.
- D12 Crimson snapshot: 71,436 native draws, 0 fallbacks, 71,436 linear-depth,
  24,738 polygon-offset, all smooth.
- D13/D14 90-second scripted routes: 641,855 combined native draws, 0
  fallbacks, no flat-first native draws, no flat fallbacks.
- D15 post-tightening Rainbow snapshot: 201,450 native draws, 0 fallbacks,
  101,016 w-depth, 100,434 linear-depth, 26,823 polygon-offset, all smooth.
- Rainbow and Crimson baseline/native screenshot smoke comparisons did not show
  an obvious visual regression.

## 2026-04-30: Allow first-provoking flat triangles, but validate with XBE

Decision:

Allow flat-shaded first-provoking triangle fills on the native triangle-depth
path because the OpenGL renderer explicitly uses `GL_FIRST_VERTEX_CONVENTION`.
Keep flat-shaded nonfirst-provoking triangle fills on the existing geometry
shader fallback path.

Rationale:

The native OpenGL rasterizer can match NV2A first-provoking flat interpolation
directly under the renderer's current provoking-vertex convention. Nonfirst
provoking still requires the geometry shader to select the same flat attribute
source as NV2A. The retail Crimson/Rainbow scenes do not exercise flat-shaded
triangle fills, so this boundary needs a purpose-built XBE instead of more
retail route searching.

Verification:

- D16 Rainbow Six 3:
  `benchmark-runs/20260430-135911-rainbow-six-3`, 31.01 FPS / 7.62 MSPF
  post-load, 192,998 native triangle-depth draws, 0 fallbacks, 0 geometry
  draws. The scene remained all smooth, so the eligibility expansion did not
  perturb the known benchmark path.
- Flat XBE source and artifacts:
  `scripts/apple-silicon/xbe-tests/flat-tri-depth/`,
  `bin/default.xbe`, and `flat-tri-depth.iso`.
- Manual-launch ISO copy:
  `/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso`.
- Trace run `benchmark-runs/20260430-141331-flat-tri-trace` confirms the XBE
  sends `NV097_SET_SHADE_MODE` flat plus first and last
  `NV097_SET_PROVOKING_VERTEX`.

Follow-up:

Superseded by the final perf flush validation below. The XBE boots, sends the
right guest methods, and now produces the expected native/fallback counter
split.

## 2026-04-30: Flat XBE mismatch is a binding-correlation problem

Status: superseded by the final perf flush validation below.

Decision:

Treat the flat-tri-depth failure as a renderer state-correlation problem, not
as a missing validation asset or stale ISO problem.

Rationale:

The flat XBE was rebuilt, and benchmark metadata now records disc size and
mtime so stale media can be spotted. The trace-enabled rebuilt run shows the
expected flat-last sequence (`SHADE_MODE 0x1d00`, `PROVOKING_VERTEX 0`,
`DRAW_ARRAYS 0x2000003`), but `xemu-perf` still classified all 102,684 native
triangle-depth candidates as smooth. Adding explicit method-owned PGRAPH
shade/provoking fields for shader-state generation did not change the
classification. That makes a generic register-bit decode issue unlikely; the
next useful evidence is a direct comparison between live PGRAPH state and the
bound shader state for the same draw.

Verification:

- Rebuilt flat ISO: 2026-04-30 14:38:34 CDT, 720,896 bytes.
- `benchmark-runs/20260430-143952-flat-tri-depth`: rebuilt ISO, all native
  triangle-depth candidates still smooth.
- `benchmark-runs/20260430-144128-flat-tri-depth`: trace confirms flat-last
  draws from the rebuilt ISO, counters still smooth.
- `benchmark-runs/20260430-144451-flat-tri-depth`: after method-owned
  shade/provoking fields, counters still smooth.

Next action:

Superseded by the final perf flush validation below. Low-volume logging was
added and showed the live and bound shader state were flat-first when expected.

## 2026-04-30: Use final perf flush for short diagnostic XBEs

Status: implemented and validated.

Decision:

Emit one final `xemu-perf:` interval on graceful process exit, and let the
benchmark launcher wait briefly for QMP `quit` before falling back to SIGTERM.

Rationale:

The flat-tri-depth XBE changed into its flat phases after the last regular
one-second perf interval in short runs. Renderer tracing showed the live PGRAPH
state and bound shader state both became flat-first at shader bind, draw begin,
and draw flush, so the all-smooth summaries were a measurement-window artifact
rather than a shader-state propagation bug.

Verification:

- `benchmark-runs/20260430-152952-flat-tri-depth`: trace showed flat-first
  live and bound shader state, but regular intervals still ended before the
  flat tail.
- `benchmark-runs/20260430-153555-flat-tri-depth`: final interval
  `final=1 reason=atexit` captured 480 `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, 304
  `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`, and 304 `GEOM_SHADER_DRAW_TRI`.

Consequence:

The native triangle-depth prototype's flat handling is validated for the
current dedicated XBE: first-provoking flat triangles can use native GL
triangle rasterization, while last-provoking flat triangles correctly stay on
the geometry-shader fallback path.

## 2026-04-30: Give native triangle-depth a stable experiment flag

Status: implemented and smoke-tested.

Decision:

Use `XEMU_NATIVE_TRI_DEPTH=1` as the preferred opt-in name for the native
triangle-depth prototype. Keep `XEMU_DIAG_NATIVE_TRI_DEPTH=1` as a compatibility
alias for earlier benchmark notes and reproduction commands. If
`XEMU_NATIVE_TRI_DEPTH=0` is explicitly set, it overrides the old alias.

Rationale:

The prototype has moved past a raw dispatch-cost diagnostic: it now has smooth
retail-scene coverage, flat first-provoking coverage, flat nonfirst fallback
coverage, perf counters, and visual smoke checks. It is still not ready to
become a default renderer path, but it deserves a stable experiment name that
can be used by validation scripts without implying that every run is temporary
debug plumbing.

Verification:

- `benchmark-runs/20260430-173353-flat-tri-depth` was launched with
  `XEMU_NATIVE_TRI_DEPTH=1`.
- The log reported
  `xemu-perf: native_tri_depth=1 source=XEMU_NATIVE_TRI_DEPTH mode=safe`.
- Benchmark metadata recorded `env_XEMU_NATIVE_TRI_DEPTH: 1`.
- The run emitted a final `xemu-perf:` flush and reported 262,586 native
  triangle-depth draws with zero geometry-shader draws.
- `benchmark-runs/20260430-175500-flat-tri-depth` was launched with
  `XEMU_NATIVE_TRI_DEPTH=0` and `XEMU_DIAG_NATIVE_TRI_DEPTH=1`; the stable
  disable took precedence, no native enable line was emitted, and the run
  reported 0 native triangle-depth draws.

## 2026-04-30: Treat native triangle-depth as validated for current triangle-fill coverage

Status: implemented for opt-in use; not a default renderer path.

Decision:

For the current Apple Silicon fork, `XEMU_NATIVE_TRI_DEPTH=1` is validated as
the active opt-in triangle-family fill replacement path for retail snapshot
testing. It should remain opt-in until broader game coverage and longer
visual/depth validation exist.

Rationale:

The path now has counter evidence for smooth-shaded retail scenes, both depth
modes, fill polygon offset, first-provoking flat native draws, nonfirst flat
fallbacks, and same-build baseline/native screenshot comparisons. It removes
all triangle-family geometry-shader draws in the current Rainbow Six 3 and
Crimson Skies snapshots without an obvious visual smoke failure.

Verification:

- Rainbow paired comparison:
  `benchmark-runs/20260430-174138-native-tri-depth-compare-rainbow-six-3`
  reduced post-load MSPF from 23.10 to 6.83, geometry-shader draws from 79,775
  to 0, and reported 0.6131% changed pixels in the fixed viewport crop.
- Crimson paired comparison:
  `benchmark-runs/20260430-174443-native-tri-depth-compare-crimson-skies`
  reduced post-load MSPF from 29.47 to 17.87, geometry-shader draws from
  20,041 to 0, and reported 3.6913% changed pixels in the fixed viewport crop.

Consequence:

Next work in this category should focus on broader coverage and eventual
defaulting policy rather than re-proving the same Rainbow/Crimson triangle-fill
case. The immediate next implementation focus was later narrowed on 2026-05-01
to the remaining geometry-shader users, while keeping broader coverage as the
defaulting prerequisite.

## 2026-05-01: Close the current triangle-fill slice and move on

Status: documented and validated.

Decision:

Treat `XEMU_NATIVE_TRI_DEPTH=1` as the completed current opt-in
triangle-family fill replacement path. Use
`scripts/apple-silicon/validate-native-tri-depth.sh --run 20` as the regression
gate for this slice, and move the next implementation session to the remaining
geometry-shader users.

Rationale:

The path has smooth retail coverage, flat first-provoking native coverage, flat
nonfirst fallback coverage, depth-mode and polygon-offset counters, same-build
Rainbow/Crimson paired comparisons, and a fresh packaged-app validator run:
`benchmark-runs/20260430-210159-flat-tri-depth`.

Consequence:

Do not start the next session by revalidating triangle-family fill unless the
triangle path changes. Start by measuring or creating coverage for line
primitives, quad/quad-strip expansion, polygon fill, or nonfill triangle modes,
then choose one category to remove or narrow. Broader retail coverage is still
required before defaulting `XEMU_NATIVE_TRI_DEPTH=1`, but it is not the next
implementation blocker.

## 2026-05-01: Use retail gameplay routes as the next performance gate

Status: recorded and tracked.

Decision:

Use the recorded PGR2, Rainbow Six 3, and Crimson Skies gameplay routes as the
primary user-visible performance gate for the next renderer work. The
performance floor is sustained 30 FPS in gameplay for all tracked titles; 60 FPS
is desirable but not the minimum stability/performance bar.

Rationale:

The older smoke routes and scene snapshots are useful for controlled
diagnostics, but the new routes reproduce the actual manual observations:
PGR2 collapses in gameplay, Rainbow Six 3 drops when character movement starts,
and Crimson Skies shows sustained gameplay pacing and acceleration-animation
choppiness. These routes also expose remaining primitive families that the
completed triangle-family fill path does not remove.

Verification:

- PGR2:
  `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv`,
  `benchmark-runs/20260501-094823-pgr2`, 11.53 average FPS, 1,516,519
  geometry-shader draws, including 38,785 quad-family draws.
- Rainbow Six 3:
  `scripts/apple-silicon/input-scripts/rainbow-gameplay.csv`,
  `benchmark-runs/20260501-095400-rainbow-six-3`, 24.19 average FPS, 692,438
  geometry-shader draws, including 1,946 line-family draws.
- Crimson Skies:
  `scripts/apple-silicon/input-scripts/crimson-gameplay.csv`,
  `benchmark-runs/20260501-095905-crimson-skies`, 15.44 average FPS, 786,722
  geometry-shader draws, including 7,837 quad-family draws.

Consequence:

Start the next implementation session with PGR2 baseline versus
`XEMU_NATIVE_TRI_DEPTH=1` replay. If quad-family geometry-shader work remains
the strongest signal, prioritize quad/quad-strip expansion removal or
narrowing before moving to line primitives, polygon fill, or nonfill triangle
modes.
