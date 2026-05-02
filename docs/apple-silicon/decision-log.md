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

## 2026-05-01: Add `XEMU_NATIVE_QUAD=1` smooth-fill quad bypass

Status: implemented and validated.

Decision:

Add a second opt-in geometry-shader removal slice, `XEMU_NATIVE_QUAD=1`, that
expands `PRIM_TYPE_QUADS` and `PRIM_TYPE_QUAD_STRIP` smooth-fill draws into
native triangle dispatches and reuses the `gl_FragCoord`-derived depth path
already used by `XEMU_NATIVE_TRI_DEPTH=1`. The flag is independent of
`XEMU_NATIVE_TRI_DEPTH`. Flat-shaded quads, line/point polygon modes, and any
nonfill raster mode stay on the existing geometry-shader path. The quad
diagonal triangulation matches the geometry shader's `calc_quadz(0, 2)`
order, so smooth interpolation is unchanged.

Rationale:

The PGR2 retail gameplay route at 11.53 baseline FPS reached only 21.40
post-load FPS with `XEMU_NATIVE_TRI_DEPTH=1` enabled, and the entire
remaining geometry-shader workload at that point was quad-family (177,272 of
177,272 GS draws). Apple's OpenGL geometry-shader path was already
identified as the dominant Apple Silicon bottleneck for the triangle-fill
slice; the same removal applied to quad-fill is the obvious next slice.

Verification:

- Triangle regression gate `validate-native-tri-depth.sh --run 22` passed at
  `benchmark-runs/20260501-105543-flat-tri-depth`. Adding the native-quad
  infrastructure did not perturb the triangle-fill validator.
- Rainbow Six 3 snapshot scene
  (`benchmark-runs/20260501-110557-rainbow-six-3`, 30.97 post-load FPS / 6.71
  MSPF) is identical within noise to the prior `XEMU_NATIVE_TRI_DEPTH=1`-only
  D8 result (30.99 FPS / 6.47 MSPF). Quad-free scenes are unaffected by the
  new code.
- PGR2 mid-route snapshot triplet (`benchmark-runs/20260501-115623-pgr2`,
  `20260501-115654-pgr2`, `20260501-115725-pgr2`):
  - Baseline: 4.39 FPS; 329,044 GS triangle draws + 3,022 GS quad draws.
  - `XEMU_NATIVE_TRI_DEPTH=1`: 16.02 FPS; 0 GS triangle draws, 11,745 GS
    quad draws remain.
  - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1`: 16.56 FPS; 0 GS draws of
    any kind, 12,193 native-quad draws (all `LIST`, all
    `CANDIDATE_SMOOTH`, zero fallbacks).
- Whole-route replays show 36% run-to-run variance (18.20 vs 24.81 post-load
  FPS for the same flag config) because real-time-paced input drives the
  emulator into different scene mixes at different host throughputs. Stable
  comparisons need snapshot replays.

Consequence:

The geometry-shader removal track has now eliminated all triangle-family and
all smooth-fill quad-family geometry-shader draws across all current
benchmark scenes. PGR2 still does not hit the 30 FPS gameplay floor at the
captured snapshot (16.56 FPS), so the next bottleneck is no longer geometry
shaders. Use Instruments and the existing perf counters at the PGR2
snapshot scene to identify whether i386 TCG, NV2A PGRAPH command processing,
surface/texture upload, or fragment shader work is the dominant remaining
cost. Defer further geometry-shader-specific work (flat-quad bypass,
nonfill polygon modes) until a benchmark exercises that combination
non-trivially.

## 2026-05-01: Add `XEMU_PGRAPH_FAST_READ=1` lock-free PGRAPH register reads

Status: implemented and validated.

Decision:

Add an opt-in fast path in `pgraph_read()` that returns a `qatomic_read()`
snapshot of the requested register without acquiring `pg->lock` for simple
register reads (`NV_PGRAPH_INTR`, `NV_PGRAPH_INTR_EN`, and the default
`pg->regs_[]` slot). Only `NV_PGRAPH_RDI_DATA` keeps the full lock because
its read auto-increments `NV_PGRAPH_RDI_INDEX_ADDRESS`. Independent of
`XEMU_NATIVE_TRI_DEPTH` and `XEMU_NATIVE_QUAD`, but stacks with them.

Rationale:

A `sample` profile of the PGR2 mid-route snapshot
(`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-sample.md`)
showed the TCG i386 emulation thread spending ~32% of its wall time
sleeping in mutex-wait, with `pgraph_read` accounting for ~22% on its own.
The renderer's pfifo thread holds `pg->lock` across the slow OpenGL
submission inside `pgraph_method`, so every Xbox-CPU MMIO read of an
NV_PGRAPH_* register stalls until the renderer is done. Aligned 32-bit
loads are atomic on aarch64 and x86, so the mutex provides no protection
that the hardware does not already give for these specific reads — it is
strict overhead. The Xbox game loop polls these registers very frequently,
so eliminating the per-call mutex roundtrip recovers a large fraction of
emulator throughput.

Verification:

- Triangle regression gate
  (`scripts/apple-silicon/validate-native-tri-depth.sh --run 22`) passed at
  `benchmark-runs/20260501-123015-flat-tri-depth`. The lock-free read does
  not affect the flat-shading triangle path.
- PGR2 mid-route snapshot triplet (paused-input replays of the same Xbox
  state): adding `XEMU_PGRAPH_FAST_READ=1` on top of
  `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` lifts post-load FPS from
  16.56 to 30.76 (+85.8%) and 30.70 over a 60-second rerun
  (`benchmark-runs/20260501-123050-pgr2`,
  `benchmark-runs/20260501-123357-pgr2`). Zero geometry-shader draws,
  zero native fallbacks.
- `XEMU_PGRAPH_FAST_READ=1` alone, without the geometry-shader bypasses,
  produces only a small lift (4.4 → 5.3 FPS post-load). The
  geometry-shader bypass is what makes the renderer's lock-hold time short
  enough that removing the per-read mutex matters.
- PGR2 retail gameplay route replay with all three flags
  (`benchmark-runs/20260501-123525-pgr2`): 31.76 post-load FPS over 279
  intervals (~4.7 minutes of real gameplay), zero geometry-shader draws,
  10.4M native-tri draws, 254K native-quad draws, all smooth, zero
  fallbacks. Compared to the 11.67 post-load FPS baseline, this is +2.72x.

Consequence:

PGR2 now meets the project's 30 FPS retail-gameplay floor with all three
opt-in flags enabled. The flags remain opt-in until broader title coverage
exists. Next slice should audit `pgraph_write` and `voice_lock` for similar
fast-path opportunities, then revisit whether any further bottleneck
exists at this scene under Instruments. Whole-route averages are now
useful comparators again because per-run scene divergence is reduced when
the emulator is no longer CPU-starved.

## 2026-05-01: Three opt-in flags visually validated for current title set

Status: visually validated by the user on 2026-05-01.

Decision:

Treat `XEMU_NATIVE_TRI_DEPTH=1`, `XEMU_NATIVE_QUAD=1`, and
`XEMU_PGRAPH_FAST_READ=1` as visually safe for the current tracked-title
set (PGR2, Rainbow Six 3, Crimson Skies) on Apple Silicon when used
together. They remain opt-in flags rather than default behavior until a
broader title-coverage gate is met.

Rationale:

Each of the three slices was code-validated against the flat-tri-depth
regression XBE and counter sanity checks. After all three landed, the
user ran each tracked title under combined flags on a real disc and
confirmed:

- PGR2: no artifacting, 30 FPS feel.
- Rainbow Six 3: no artifacting, 30 FPS feel.
- Crimson Skies: no artifacting, 30 FPS feel.

Counter-side guarantees backing this:

- `XEMU_NATIVE_TRI_DEPTH=1` only takes the native path when the eligibility
  predicate (`pgraph_glsl_native_tri_depth_supported`) holds. Flat-nonfirst
  triangles still use the geometry shader.
- `XEMU_NATIVE_QUAD=1` only takes the native path for smooth-fill
  quad/quad-strip primitives. Flat-shaded quads, line/point polygon modes,
  and any nonfill raster mode still use the geometry shader.
- `XEMU_PGRAPH_FAST_READ=1` only skips the lock for atomic 32-bit reads
  with no side effects. `NV_PGRAPH_RDI_DATA` (the only read-with-side-
  effect we know about) still locks.

Consequence:

Flags stay off by default. New titles or scenes that exercise quad
flat-shading, nonfill polygon modes, or unusual PGRAPH register access
patterns must be visually validated before being added to the tracked set.
The next gate to consider flipping any flag default-on is broader retail
coverage across genres (additional racing, FPS, platforming, and
menu-heavy titles).

## 2026-05-01: Sub-millisecond perf-log precision and per-frame timing

Status: landed in `hw/xbox/nv2a/pgraph/profile.c` on 2026-05-01.

Decision:

`xemu-perf:` interval lines now emit `mspf_avg`, `mspf_min`, `mspf_max`
as `%.3f` floats (microsecond precision internally), and an opt-in
`XEMU_PERF_FRAME_LOG=1` appends a per-frame `frame_mspf_us=v1,v2,...`
field to each interval line, bounded to 1024 frames per interval with
overflow recorded in `frame_mspf_us_dropped`. Default off. The HUD plot
in `ui/xui/debug.cc` keeps its integer-ms `frame_working.mspf` field
unchanged.

Rationale:

The 60 FPS budget is 16.67 ms; the previous integer-ms perf-log
resolution rounded away the difference between a frame inside and
outside that budget. Sub-ms precision is required for the 60 FPS
pursuit. The optional per-frame log enables true frame-level p99 /
p99.9 percentiles without changing the always-on log volume.

Consequence:

`scripts/apple-silicon/extract-perf-summary.sh` parses the new precision
without changes (its arithmetic was already float-tolerant) and gains
new jitter keys derived from the per-interval `mspf_max` field:
`fps_stddev`, `mspf_max_p50/p95/p99/max`, `stutter_intervals_30/45/60fps`,
and `longest_stutter_run_30/60fps` (whole-run and `post_load_*`
variants). Older runs captured before this change retain integer-ms
`mspf_max`; new runs are sub-ms accurate. Baseline benchmark notes
record the format transition so future comparisons can account for it.

## 2026-05-01: XEMU_VOICE_FAST_LOCK not landed

Status: investigated, implemented, measured, **rejected** on
2026-05-01. Code reverted; the `XEMU_VOICE_FAST_LOCK` flag does not
exist in the shipped binary.

Decision:

A lock-free `voice_lock()` fast path (atomic OR/AND on
`d->vp.voice_locked[]`, dropping the `qemu_cond_signal` on the
audio-worker condvar) was implemented to address the 6.8 % `voice_lock`
TCG-thread mutex wait identified in
`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`.
Measurement showed no FPS improvement on either the `pgr2_gameplay_b4`
snapshot (30.64 → 30.79 FPS, +0.49 % = noise) or the 300 s PGR2 retail
route (31.76 → 31.84 FPS, +0.25 % = noise), with mixed jitter signals:
tighter p99 max-frame on the snapshot but +91 % more 30-FPS stutter
intervals on the retail route. Per the project's data-driven rule, the
change is not landed.

Rationale:

The post-fast-read sample profile shows the pfifo thread is idle 41.5 %
of the time on the FIFO condvar at the PGR2 mid-route snapshot — the
renderer can absorb more work than the CPU thread is producing.
Removing 6.8 % of TCG-thread mutex wait does not translate to FPS in
this regime because the freed cycles cannot be put to work. The
audio-worker's missed-`cond_signal` latency (bounded at 1 ms by its
existing `cond_timedwait`) appears to introduce a small jitter-shape
shift that may be net-negative on routes with active audio events.
Full measurement details and run dirs are in
`docs/apple-silicon/benchmarks/2026-05-01-voice-fast-lock-investigation.md`.

Consequence:

Lock-elision is exhausted as a primary 60 FPS lever for PGR2-class
scenes. The remaining ~9 % TCG-thread mutex wait is split across smaller
contributors (`pgraph_write` 1.4 %, miscellaneous 0.8 %) and not worth
a flag of its own without first addressing the larger remaining cost:
real x86 emulation throughput, particularly the floating-point helper
paths (`helper_mulss`, `helper_fmul_ST0_FT0`, `floatx80_mul`,
`soft_f32_mul`). The next data-driven slice on the TCG side should
audit whether SSE / x87 ops are going through softfloat unnecessarily
on Apple Silicon. On the renderer side, Crimson Skies' documented
shader-compile stutter (1310 ms worst-frame, 16-second longest stutter
run) is the highest user-visible jitter target and is independent of
the TCG path.

## 2026-05-01: Adopt research-informed implementation roadmap

Status: documented. Each individual landing remains data-driven and will
be added to this log separately as it ships.

Decision:

Pursue, in priority order: (1) frame-pacing emulation-rate slewing
(Phase 2.5), (2) async shader compile (Phase 2.5), (3) native Metal
renderer with CPU-side index expansion + framebuffer fetch + VS-Expand +
async pipeline compile (Phase 4a–4i), (4) persistent shader/pipeline
cache (Phase 4f / Phase 5), (5) persistent TCG translation cache
(Phase 5a, PPTC pattern), (6) SSE / x87 hardfloat audit (Phase 5b,
already tracked in `handoff.md` Prioritized Next Tasks #3). Reject
custom x86 → ARM64 JIT (low ceiling, high macOS JIT pain documented in
RPCS3 PR #12115) and ICB / argument-buffer work (premature for Xbox-era
workloads).

Rationale:

The 2026-05-01 emulator survey
(`docs/apple-silicon/research.md` "Apple Silicon Emulator Survey")
catalogued how Dolphin, PCSX2, DuckStation, RPCS3, Ryujinx, and PPSSPP
solve problems analogous to xemu's. Concrete patterns with named code
references:

- Dolphin `Source/Core/VideoCommon/IndexGenerator.cpp` (CPU-side
  primitive expansion).
- PCSX2 `pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm` `m_expand_index_buffer`
  (VS-Expand with precomputed static index buffer + Metal function
  constants).
- DuckStation `src/util/metal_device.mm:2536-2620` (`presentDrawable:atTime:`
  + emulation-rate slewing for jitter-free pacing).
- DuckStation `src/util/metal_device.mm:387-410` and PCSX2 PR #5630
  (framebuffer fetch on Apple GPU family for blend / register-combiner
  passes).
- Dolphin PR #5702 (hybrid ubershader for async shader compile).
- Ryujinx PPTC blog (persistent translation cache pattern).
- RPCS3 PR #12115 (catalogued macOS Apple Silicon JIT pain — used here
  as anti-pattern reference).

These extend rather than replace the existing prioritized work. Frame
pacing (DuckStation pattern) directly addresses the 30 FPS gameplay
jitter symptom captured in
`benchmarks/2026-05-01-baseline-jitter.md`. Async shader compile
(Dolphin / RPCS3 pattern) directly addresses Crimson Skies' 1310 ms
worst-frame from Apple's GL-on-Metal synchronous compile. The Metal
backend shape — index expansion + FBFetch + VS-Expand + async
pipeline compile — is now concrete and data-driven rather than a
hand-wave.

Verification:

- Survey content captured in `docs/apple-silicon/research.md` "Apple
  Silicon Emulator Survey (2026-05-01)" with citations.
- Strategy phases updated: Phase 2.5 inserted, Phase 4 expanded with
  sub-deliverables 4a–4i, Phase 5 expanded with 5a (PPTC) and 5b
  (hardfloat audit), new "What we ruled out" section.
- No code changes in this entry. This is a planning decision.

Consequence:

Each individual landing remains gated by the project's data-driven rule
(workspace `CLAUDE.md` rule #1): fresh `sample` profile + dated
benchmark note + measured before/after, with append-only decision-log
entries when each ships. Specifically: Phase 2.5 frame-pacing slewing
should land before the Metal renderer because it is graphics-API-
agnostic and trivially measurable; the async shader compile slice
should follow because Crimson is the largest user-visible jitter target
and its bottleneck has been profiled to synchronous shader compile in
the Apple OpenGL-on-Metal driver.

## 2026-05-01: XEMU_PGRAPH_FAST_WRITE deferred (not pursued this session)

Status: source-audited, **deferred**. No code shipped. The flag does not
exist in the binary.

Decision:

The `XEMU_PGRAPH_FAST_WRITE=1` slice listed as Prioritized Next Tasks #4
in `handoff.md` is deferred. The handoff entry framed it as "low-risk,
mirror `pgraph_read`," but a source audit of `pgraph_write`
(`hw/xbox/nv2a/pgraph/pgraph.c:161`) and `pgraph_reg_w`
(`hw/xbox/nv2a/pgraph/pgraph.h:311`) shows the `default` slot write is
not a simple atomic store: it does a compare-then-set (`if (pg->regs_[r]
!= v)`) and updates the `regs_dirty` bitmap via `bitmap_set`. The
renderer consumes `regs_dirty` to decide shader recompiles
(`hw/xbox/nv2a/pgraph/glsl/shaders.c:57` and the Vulkan equivalent at
`hw/xbox/nv2a/pgraph/vk/draw.c:643`). A naive lock-free fast path could
either lose dirty bits to a producer/clearer race
(`pgraph_clear_dirty_reg_map` is called from the renderer thread) or
present a producer/consumer ordering hole where the consumer reads
`dirty=0, value=new` and skips a needed recompile, causing visual
corruption. A correct fast-write would need explicit acquire/release
ordering on both producer and consumer plus an atomic `set_bit` on the
`regs_dirty` word; that's no longer "mirror pgraph_read."

Beyond correctness, the prior decision-log entry "2026-05-01:
XEMU_VOICE_FAST_LOCK not landed" already concluded that lock-elision
is exhausted as a primary 60 FPS lever for PGR2-class scenes when the
pfifo thread is idle 41.5% of the time on the FIFO condvar — removing
1.4 % of TCG-thread mutex wait cannot translate to FPS in that regime.
The Amdahl ceiling on this slice is therefore ~1 % even if all the
correctness questions resolved.

Rationale:

Per workspace `CLAUDE.md` rule #1 (no guessing — every decision
data-driven), implementing a slice the project's own measurements
already predict won't lift FPS, with a non-trivial correctness risk the
handoff entry undercounted, fails the "high confidence" bar. Per rule #3
(be honest about limits), this entry replaces the handoff's "low-risk,
completes the read/write symmetry" framing with the actual analysis.

Verification:

- `pgraph_write` source ground truth: `hw/xbox/nv2a/pgraph/pgraph.c:161-256`.
- `pgraph_reg_w` `regs_dirty` side effect:
  `hw/xbox/nv2a/pgraph/pgraph.h:311-318`.
- Consumers of `regs_dirty`: `hw/xbox/nv2a/pgraph/glsl/shaders.c:57-92`,
  `hw/xbox/nv2a/pgraph/vk/draw.c:643-708`.
- Prior lock-elision-Amdahl conclusion:
  "2026-05-01: XEMU_VOICE_FAST_LOCK not landed" (decision-log entry above).
- Underlying sample profile:
  `docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`.

Consequence:

`handoff.md` Prioritized Next Tasks should be re-ordered so this slice is
deprioritized below (a) async shader compile (Crimson 1310 ms worst-frame
jitter — the largest user-visible win), (b) SSE / x87 hardfloat audit
outcome, and (c) frame-pacing emulation-rate slewing (Phase 2.5,
graphics-API-agnostic). Should fast-write be revisited later — for
example after the renderer is no longer the binding constraint and
freed TCG cycles can actually be put to work — the implementation must
include atomic set_bit on `regs_dirty` with explicit acquire/release
ordering on the consumer side, or accept the cost of always-mark-dirty
(which costs renderer recompiles to gain producer simplicity).

## 2026-05-01: SSE hardfloat already active on aarch64; x87 80-bit irreducibly soft

Status: source-audited. **Original hypothesis disproved.** No code shipped.

Decision:

The `handoff.md` Prioritized Next Tasks #3 hypothesis — "SSE single-precision
ops (`helper_mulss`, `helper_mulps_xmm`) actually go through `soft_f32_mul` /
`parts64_uncanon_normal` on Apple Silicon and lifting them to hardfloat is
the single largest potential TCG win" — is wrong. The SSE hardfloat fast
path already exists and is already active on aarch64. The remaining
`parts64_*` time visible in the post-fast-read sample profile is the
necessary cost of softfloat's correctness fallback (NaN/denormal inputs,
denormal results, first-op-after-MXCSR-reset, non-default rounding modes),
not an unconditional softfloat trip.

`helper_fmul_ST0_FT0` and the rest of the x87 surface are irreducibly
soft on Apple Silicon. The fork's existing `__hard` x87 path is correctly
gated to `XBOX && __x86_64__` because Apple Silicon `long double` is
8-byte (64-bit), not 80-bit. There is no native 80-bit float on aarch64
to dispatch to.

The original handoff #3 framing should be retired. The follow-up work is
either (i) confirm the hard-take ratio with a counter pair, then redirect
to a non-float TCG subsystem, or (ii) treat x87 as a separate Phase-2
NEON-kernel effort which is real work but only justified after
Instruments confirms x87 is dominant in real-game inner loops.

Rationale:

Source-only audit at `docs/apple-silicon/benchmarks/2026-05-01-tcg-float-audit.md`
walks the call chain from `helper_mulss` →
`target/i386/ops_sse.h:514-533` (`FPU_MUL` macro and
`SSE_HELPER_S(mul, FPU_MUL)`) → `float32_mul`
(`fpu/softfloat.c:2169-2174`) → `float32_gen2`
(`fpu/softfloat.c:337-366`) → `hard_f32_mul = a * b`
(`fpu/softfloat.c:2159-2162`, a single arm64 `fmul`).

The `can_use_fpu` gate (`fpu/softfloat.c:230-237`) is satisfied in the
common Xbox-game MXCSR state: `QEMU_NO_HARDFLOAT` is 0 (no `-ffast-math`),
`float_flag_inexact` is sticky after the first soft op (SSE arithmetic
helpers do not clear flags per-op — only `WRAP_FLOATCONV` at
`target/i386/ops_sse.h:703-718` does), and `float_rounding_mode ==
float_round_nearest_even` matches MXCSR RC=00 which Xbox titles set
overwhelmingly. Per-call gates `f32_is_zon2` (zero-or-normal inputs after
FTZ flush) and `f32_addsubmul_post` (denormal result fallback) at
`fpu/softfloat.c:270-292` and `fpu/softfloat.c:1975-1990` apply only to
edge cases, not the steady-state inner loop.

The x87 path has no `floatx80_gen2` analogue. `floatx80_mul`
(`fpu/softfloat.c:2218-2230`) unconditionally calls `parts_mul`. The
fork's `__hard` shim (`target/i386/tcg/fpu_helper_hard.c`) requires
80-bit `long double`, which Apple's clang on aarch64 does not provide.
The `XBOX && __x86_64__` gate at `target/i386/helper.h:104-121`,
`target/i386/tcg/fpu_helper.c:76-267`, `target/i386/tcg/translate.c:38-124`,
and `ui/xui/main-menu.cc:62-66` correctly excludes the hard path on
Apple Silicon — there is nothing to dispatch to.

Verification:

- `docs/apple-silicon/benchmarks/2026-05-01-tcg-float-audit.md` —
  full file:line citation chain.
- `fpu/softfloat.c:230-237` — `can_use_fpu` gate.
- `fpu/softfloat.c:337-397` — `float32_gen2`/`float64_gen2` dispatcher.
- `fpu/softfloat.c:2159-2174` — hard/soft `f32_mul` wiring.
- `target/i386/tcg/fpu_helper.c:780-785` — `helper_fmul_ST0_FT0` body
  (unconditional soft via `floatx80_mul`).
- `target/i386/tcg/fpu_helper_hard.c:1-4` — empty translation unit on
  non-`XBOX && __x86_64__`.
- `ui/xui/main-menu.cc:62-66` — Hard FPU UI toggle visible only on
  `__x86_64__`.

Consequence:

`handoff.md` Prioritized Next Tasks #3 is rewritten in place to reflect
this finding plus a recommended cheap follow-up: a counter pair around
`float32_gen2`/`float64_gen2` (`sse_hard_taken` vs `sse_soft_fallback`,
split by fall-through reason: `!can_use_fpu`, `!pre`, `denormal_result`),
run on the `pgr2_gameplay_b4` snapshot for 30 s. If hard-take ratio > 0.9,
confirm the `parts64_uncanon_normal` time is irreducible correctness work
and redirect future TCG investigations to the next dominant subsystem
Instruments identifies (TLB / memory-op helpers, NV2A PGRAPH command
parsing, or surface/texture upload — handoff Prioritized Next Tasks #5/#6
are still on the table). The async shader compile slice (Crimson 1310 ms
worst-frame jitter, biggest user-visible win) and Phase 2.5 frame-pacing
slewing remain the two most impactful next-implementation slices on the
roadmap; nothing about this audit changes their priority.


## 2026-05-01: Add external Xbox library and `package-game.sh` packaging tool

Decision:

Adopt `/Volumes/Josh-Backup-Files/Console Games/Original Xbox` as the
canonical external Xbox game library for this fork, and add
`scripts/apple-silicon/package-game.sh` as the project-supported way to
pull a game from that library into a xemu-loadable XISO ISO.
`xdvdfs-cli` (MIT-licensed; antangelo/xdvdfs) is installed via `cargo
install xdvdfs-cli` at `$HOME/.cargo/bin/xdvdfs`; the binary is not
vendored into the repo. The packaging script auto-installs it on first
use unless `--no-install` is passed.

Default output is `$XEMU_TEST_GAMES_DIR/<game>.xiso.iso` (i.e.
`/Users/jbbrack03/XEMU_MacOS/Test_Games/<game>.xiso.iso`). The script
refuses to overwrite an existing output file without `--force`, so
rule #9 (do not modify `Test_Games/` or `Xbox-Emulator-Files/` in
place) is preserved while still allowing additive packaging into the
existing test-games directory.

Rationale:

Future sessions will identify reported xemu issues for games we do not
currently track (PGR2, Rainbow Six 3, Crimson Skies, flat-tri-depth)
and need a reproducible way to bring those games into the benchmark
harness. The library volume contains the extracted game trees;
`xdvdfs pack` produces a valid XISO from such a tree in seconds. A
project-supported wrapper avoids ad-hoc invocations that could
accidentally overwrite tracked test ISOs or skip verification. Cargo
install (vs vendoring a binary or a Homebrew formula) keeps the repo
small and lets the tool stay current with upstream xdvdfs without a
maintenance commit.

Verification:

- `xdvdfs-cli` v0.8.3 installed and reachable at
  `/Users/jbbrack03/.cargo/bin/xdvdfs`.
- `package-game.sh` packs `scripts/apple-silicon/xbe-tests/flat-tri-depth/bin/`
  into a 256 KiB ISO that `xdvdfs info` reports `Valid: true`.
- End-to-end name-lookup pack of `Grooverider - Slot Car Thunder` from
  the external library produces a 96 MiB ISO that `xdvdfs info` reports
  `Valid: true` and that `xdvdfs ls` enumerates correctly.
- All negative paths (bogus name, ambiguous name, missing source, bad
  flag) return non-zero with descriptive messages.
- Idempotent rerun is a no-op; `--force` rebuilds.

Consequence:

Slice closed: any future session can run
`scripts/apple-silicon/package-game.sh "<game name>"` to grab a game
from the library on demand. The next adjacent slice (deferred) is
extending `run-benchmark.sh` with a `custom <iso>` target so packaged
games can flow through the harness without per-target hardcoding; the
packaging tool itself is complete.


## 2026-05-01: Async shader compile shipped opt-in; not the source of Crimson worst-frame stutter

Decision:

Ship `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` as a documented opt-in flag.
Default off. Do not pursue further async-side work until the actual
source of the 1.35-second Crimson worst-frame stutter is identified.

Rationale:

Implemented per the research-informed roadmap (Dolphin / RPCS3 pattern,
PR #4876 "Async (Skip Draws)"): a third shared
`g_nv2a_context_shader_compile` GL context, a `pgraph.gl_async_compile`
worker thread, a per-binding `pending_compile` flag, an early-return
"skip the draw" fallback in `pgraph_gl_draw_begin/end`. End-to-end
correctness is validated: the worker compiles, programs publish into the
shared name space, the renderer skips draws while compiling, and FPS
does not regress on the PGR2 snapshot or default-path runs.

Crimson Skies retail-route paired runs (same build, same input script,
both with `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1`) showed:

- `SHADER_COMPILE_US_TOTAL` 399 ms (sync) vs 347 ms (async): the
  worker successfully moved nearly all `glLinkProgram` cost off the
  renderer thread.
- `post_load_frame_mspf_us_max`: 1,345,831 us (sync) vs 1,351,887 us
  (async). Identical within run-to-run variance.
- The stutter spikes occur at the **same gameplay points** (same
  interval offsets +-1 from the script timing roll) with **near-
  identical magnitudes** (e.g. 1,345.8 ms baseline vs 1,343.7 ms
  async, 1,321.5 ms baseline vs 1,351.9 ms async).

This is conclusive: the headline 1.35 s worst-frame is **not**
`glLinkProgram` synchronous time on the renderer thread. The likely
cause is Apple's GL-on-Metal driver doing MSL->Metal pipeline-state-
object compile inside the **first `glDrawElements`** with a new
program/VAO/state combination. That work cannot be moved to a worker
thread without also setting up the renderer's VAO and surface state on
the worker context, which is a much larger refactor.

`p999` regressed from 104 ms (sync) to 382 ms (async) - consistent with
the worker's `glFinish()` blocking on Apple's GL command queue, which
serializes against the renderer's command buffer. Worth a follow-up
A/B with `glFlush()` in place of `glFinish()`.

Consequence:

- Async slice is **complete and shipped opt-in**, with a benchmark note
  at `docs/apple-silicon/benchmarks/2026-05-01-async-shader-compile.md`.
- The roadmap "async shader compile" task in `handoff.md` is closed.
- Future Apple-side judder work must first identify what actually fires
  during the worst-frame interval. The next investigation should add a
  per-event timestamp log inside `pgraph_gl_draw_begin / draw_end /
  flush_draw` and across `TEX_UPLOAD`, `SURF_TO_TEX`, `SURF_UPLOAD`,
  `SURF_DOWNLOAD`, then correlate against `frame_mspf_us > 100,000`.
- Phase 4 (native Metal renderer) remains the right long-term path:
  it is the only way to escape Apple's GL-on-Metal MSL compile and
  command-queue serialization.

Verification:

- Build `./build.sh -a arm64` succeeds; codesign verified.
- Smoke run `benchmark-runs/20260501-182456-flat-tri-depth` shows the
  `xemu-perf: async_shader_compile=1 source=...` startup banner and
  `SHADER_COMPILE_ASYNC_QUEUED == SHADER_COMPILE_ASYNC_COMPLETED` per
  interval (queue drains).
- Counter validation across `benchmark-runs/20260501-181049-crimson-skies`
  (sync), `20260501-182613-crimson-skies` (async), `20260501-183005-pgr2`
  (snapshot, sync), `20260501-183046-pgr2` (snapshot, async). All four
  runs surfaced the new keys via `extract-perf-summary.sh` without
  breaking older logs.
- `shader_cache_entry_post_evict()` abort-on-pending guard never fired
  in any run (50K-entry cache vs at most a few in-flight compiles).


## 2026-05-01: Stay on OpenGL; the headline bottleneck is TCG TB invalidation, not the renderer

Decision:

The fork stays on Apple's OpenGL-on-Metal as the active renderer path.
Native Metal (strategy.md Phase 4) is **not** the next priority. The
project goals (sustained 60 FPS, no judder, 1080p output, anti-
aliasing, higher-quality textures, broad Xbox library coverage) can
be delivered on the existing GL renderer once the upstream-of-renderer
bottleneck is fixed.

Rationale:

This session ran a per-event timing diagnostic followed by an Apple
`sample` profile during a known-bad Crimson interval. Key results:

1. **Renderer is essentially idle during Crimson's 1.35-s worst-frame
   intervals** (all renderer counters under 24 ms of 1000 ms). The
   Xbox CPU emulator only flips 2–10 frames in those windows.
2. **`sample` profile attributes the dominant TCG cost to JIT TB
   invalidation:** `tb_invalidate_phys_range_fast` →
   `do_tb_phys_invalidate` → `tcg_flush_jmp_cache` (382 samples)
   plus `pthread_jit_write_protect_np` (12 samples) and
   `sys_icache_invalidate` (45 samples). This is the documented
   Apple-Silicon-specific QEMU MTTCG W^X / i-cache cost.
3. **Apple GL handles 4× internal scale (~2560×1920) on PGR2 with
   only 7 % growth in `FLUSH_DRAW_US_TOTAL` and p99 stable at
   ~35 ms.** No per-pipeline-state-object pathology under heavier
   renderer load.
4. **Crimson at scale 4 puts renderer cost at 27 % of wallclock at
   30 FPS.** Doubling to 60 FPS lands at ~54 %, well inside budget.

Headroom math (renderer cost = `DRAW_BEGIN + SURF_DOWNLOAD +
FLIP_STALL`):

| Scale × FPS combination       | Projected wallclock budget |
| ----------------------------- | -------------------------- |
| 1× scale, 60 FPS              | 41 %                        |
| 2× scale (~1080p), 60 FPS     | 46 %                        |
| 4× scale (~2160p), 60 FPS     | 56 %                        |
| 2× + MSAA 2× (estimated), 60  | ~60 %                       |
| 2× + MSAA 4× (estimated), 60  | ~72 %                       |

All goals fit inside Apple's GL once TCG is unblocked.

What would change this verdict (none observed yet):

- Title-specific Apple GL pathologies in the broader Xbox library that
  the four tested titles do not exhibit. Not yet measured at scale.
- MSAA pipeline-variant explosion when AA is implemented. Not yet
  measured because xemu's GL framebuffer setup is not multisample.
- Texture-mod bandwidth saturation. Currently `TEX_UPLOAD_US_TOTAL` is
  0.27 % of wallclock — has plenty of room.

Consequence:

- **Stop investing in renderer-side fixes for the headline judder.**
  The async shader compile slice (`XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`)
  was correct but unrelated to the actual cause; it remains shipped
  opt-in.
- **The next implementation slice is TCG TB-invalidation cost
  reduction on Apple Silicon.** Specific paths to investigate:
  persistent TCG translation cache (PPTC, strategy.md Phase 5a),
  `pthread_jit_write_protect_np` toggle batching, smarter softmmu
  notdirty/dirty page handling, and upstream QEMU MTTCG Apple-
  Silicon patches.
- **MSAA implementation on the existing GL path is the renderer-side
  follow-up** (not Metal). Add multisample renderbuffer support to
  `pgraph_gl_init_surfaces` and verify the new
  `SHADER_COMPILE_*` counters do not show pathological pipeline-
  variant compile bursts.
- **A broader title sweep using `package-game.sh`** is the diversity
  validation step before declaring GL definitively viable for the
  whole library.
- **Phase 4 native Metal renderer remains documented in
  `strategy.md` as a long-term ceiling-removing effort**, but it is
  not the next priority. Reconsider only if MSAA implementation or
  the broader title sweep surface a renderer-side ceiling.

Verification:

- Build `./build.sh -a arm64` clean; `dist/xemu.app` codesign
  verified.
- Per-event timing data: `benchmark-runs/20260501-190239-crimson-skies`
  (300 s retail route with all opt-in flags + diagnostic counters).
- TCG sample profile:
  `benchmark-runs/20260501-203802-crimson-skies/sample-tcg-bad-interval.txt`
  (14,638 lines of `sample` output with thread-bucket summary).
- Scale stress tests:
  `benchmark-runs/20260501-204006-pgr2` (scale 1),
  `benchmark-runs/20260501-204044-pgr2` (scale 2),
  `benchmark-runs/20260501-204123-pgr2` (scale 4).
- Crimson scale-4 stress: `benchmark-runs/20260501-204246-crimson-skies`.
- Full analysis at
  `docs/apple-silicon/benchmarks/2026-05-01-gl-vs-metal-decision.md`
  and the supporting attribution note
  `docs/apple-silicon/benchmarks/2026-05-01-renderer-vs-tcg-stutter-attribution.md`.

