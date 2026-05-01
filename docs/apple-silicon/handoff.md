# Handoff

Last updated: 2026-05-01 (after measurement-infrastructure expansion and voice-fast-lock investigation)

## Current State

- Source has been cloned into:
  - `/Users/jbbrack03/XEMU_MacOS/xemu-fork`
- Baseline commit:
  - `ebe34071b7fec3b6187248c57708fdd6cc3a8b97`
- Working branch:
  - `apple-silicon-performance`
- Documentation created under:
  - `docs/apple-silicon/`

Source code changes made this session:

- `build.sh`
  - exports `CMAKE` on Darwin when `cmake` is available on `PATH`, so Meson's
    cross-build path can configure the `nv2a_vsh_cpu` CMake subproject.
  - removes duplicate app `LC_RPATH` entries during macOS packaging, avoiding a
    `dyld` launch abort on macOS 26.4.1.
- `ui/xemu-input.c`
  - adds opt-in scripted controller input via `XEMU_SCRIPTED_INPUT`, allowing
    repeatable benchmark navigation without physical controller input.
  - adds opt-in physical controller recording via `XEMU_RECORD_INPUT`, writing
    the same CSV format used by scripted replay.
- `hw/xbox/nv2a/debug.h`
- `hw/xbox/nv2a/pgraph/profile.c`
- `hw/xbox/nv2a/pgraph/pgraph.c`
  - add opt-in `XEMU_PERF_LOG=1` startup and per-interval performance logging
    with FPS, frame pacing, and existing NV2A profile counters.
  - add a final `xemu-perf:` counter flush on graceful process exit so short
    diagnostic tails are included in benchmark summaries.
- `hw/xbox/nv2a/pgraph/gl/draw.c`
- `hw/xbox/nv2a/pgraph/gl/renderer.h`
- `hw/xbox/nv2a/pgraph/gl/shaders.c`
  - add OpenGL geometry-shader attribution counters for module/program
    generation, binds, and geometry-backed draws by primitive family.
  - add native triangle-depth draw/fallback counters for the opt-in
    replacement path.
  - add native triangle-depth candidate counters split by smooth, flat-first,
    and flat-nonfirst state so the flat-shading diagnostic can distinguish
    "not reached" from "reached but misclassified".
  - add capped `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` logging for live
    PGRAPH/bound-shader state correlation at shader bind, draw begin, and draw
    flush.
- `hw/xbox/nv2a/pgraph/glsl/geom.c`
- `hw/xbox/nv2a/pgraph/glsl/geom.h`
  - add temporary `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` diagnostic toggle that
    keeps triangle-family geometry shaders active while bypassing their
    depth-plane/slope calculation.
  - add temporary `XEMU_DIAG_SKIP_TRI_GEOM=1` diagnostic toggle that bypasses
    geometry-shader program generation for triangle-family fill draws and draws
    native GL triangles directly.
  - tighten the native triangle-depth eligibility rule so all flat-shaded
    triangle fills stay on the existing geometry-shader path until flat
    shading is deliberately validated.
  - later relax that rule for flat-shaded first-provoking triangle fills only,
    matching the OpenGL renderer's `GL_FIRST_VERTEX_CONVENTION`; flat nonfirst
    provoking remains on the geometry-shader fallback path.
- `hw/xbox/nv2a/pgraph/glsl/psh.c`
- `hw/xbox/nv2a/pgraph/glsl/psh.h`
  - add `XEMU_NATIVE_TRI_DEPTH=1` as the completed current opt-in path that
    bypasses triangle-family fill geometry shaders and derives depth plus
    polygon-slope offset from native GL rasterization state in the fragment
    shader.
  - keep `XEMU_DIAG_NATIVE_TRI_DEPTH=1` accepted as a compatibility alias for
    older benchmark notes and commands.
  - make the preferred `XEMU_NATIVE_TRI_DEPTH=0` spelling override the old alias,
    so a shell with both variables set follows the stable flag.
  - the native bypass now applies only to triangle-family fill primitives;
    line primitives remain on the existing geometry-shader path.
  - extend the fragment-shader native-depth code path so it also activates
    when `XEMU_NATIVE_QUAD=1` is enabled and the current draw is an eligible
    smooth-fill quad-family primitive. The depth/slope math is
    primitive-agnostic.
- `hw/xbox/nv2a/pgraph/gl/shaders.c`
  - `get_gl_primitive_mode()` returns `GL_TRIANGLES` for quad-family draws
    when `XEMU_NATIVE_QUAD=1` is on and the smooth-fill eligibility holds,
    instead of the geometry-shader-required `GL_LINES_ADJACENCY` /
    `GL_LINE_STRIP_ADJACENCY`.
- `hw/xbox/nv2a/pgraph/gl/draw.c`
  - extend per-dispatch geometry-shader profiling to split
    `GEOM_SHADER_DRAW_QUAD` into `_QUAD_LIST` and `_QUAD_STRIP`.
  - add native-quad profiling counters and the
    `pgraph_gl_native_quad_active()` predicate.
  - add CPU-side index expansion helpers for both
    `PRIM_TYPE_QUADS` (4-vertex independent quads) and
    `PRIM_TYPE_QUAD_STRIP` (2-vertex incremental quads). Diagonal matches
    the existing geometry shader's `calc_quadz(0, 2)` triangulation so smooth
    interpolation is unchanged.
  - extend all four dispatch paths
    (`pg->draw_arrays_length`, `pg->inline_elements_length`,
    `pg->inline_buffer_length`, `pg->inline_array_length`) to expand the
    quad vertex stream to triangle indices, upload them via
    `glBufferData(GL_STREAM_DRAW)` on a dedicated index buffer, and issue
    a single `glDrawElements(GL_TRIANGLES, ...)` when the native bypass is
    active.
- `hw/xbox/nv2a/pgraph/gl/renderer.h` and `pgraph/gl/vertex.c`
  - add `gl_native_quad_index_buffer` element-array buffer and a CPU-side
    growable scratch index array, allocated in
    `pgraph_gl_init_buffers()` and freed in
    `pgraph_gl_finalize_buffers()`.
- `hw/xbox/nv2a/debug.h`
  - new counters: `GEOM_SHADER_DRAW_QUAD_LIST`, `GEOM_SHADER_DRAW_QUAD_STRIP`,
    `NATIVE_QUAD_DRAW`, `NATIVE_QUAD_DRAW_LIST`, `NATIVE_QUAD_DRAW_STRIP`,
    `NATIVE_QUAD_CANDIDATE`, `NATIVE_QUAD_CANDIDATE_SMOOTH`,
    `NATIVE_QUAD_CANDIDATE_FLAT`, `NATIVE_QUAD_FALLBACK`,
    `NATIVE_QUAD_FALLBACK_FLAT`, `NATIVE_QUAD_FALLBACK_NONFILL`,
    `NATIVE_QUAD_DRAW_ZPERSPECTIVE`, `NATIVE_QUAD_DRAW_LINEAR_Z`,
    `NATIVE_QUAD_DRAW_POLY_OFFSET`.
- `scripts/apple-silicon/run-benchmark.sh`
  - records `env_XEMU_NATIVE_QUAD` in benchmark metadata.
- `scripts/apple-silicon/extract-perf-summary.sh`
  - surfaces the new `GEOM_SHADER_DRAW_QUAD_*` and `NATIVE_QUAD_*` counters.
- `ui/xemu-snapshots.c`
  - adds `XEMU_SNAPSHOT_NO_THUMBNAIL=1` to skip snapshot thumbnail generation
    for benchmark-created snapshots.
- `scripts/apple-silicon/run-benchmark.sh`
  - launches Crimson Skies, Rainbow Six 3, PGR2, or flat-tri-depth with scripted
    input, metadata
    capture, QMP socket, optional periodic screenshots, logs, and a scratch HDD
    copy.
  - refuses to start if a previous xemu process is still running and cleans up
    run-owned xemu processes when the timed run exits.
  - can save and restore named VM snapshots through QMP/HMP.
  - records disc size/modification time in metadata, which caught stale
    flat-triangle ISO risk.
  - accepts `XEMU_BENCH_EXTRA_QEMU_ARGS` for reproducible trace runs such as
    `-trace nv2a_pgraph_method`.
  - waits briefly for QMP `quit` before sending SIGTERM, allowing the final
    perf-log flush to run on normal benchmark shutdown.
  - records native triangle-depth and related diagnostic environment toggles in
    benchmark metadata.
  - supports live controller setup runs through `XEMU_BENCH_LIVE_INPUT=1` and
    safe prepared-HDD use through `XEMU_BENCH_HDD_IN_PLACE=1`.
- `scripts/apple-silicon/record-input.sh`
  - records physical controller input for a selected benchmark target into a
    stable replay CSV.
- `scripts/apple-silicon/live-setup.sh`
  - runs profile setup against a persistent copied HDD at
    `benchmark-runs/profile-prep/xbox_hdd.qcow2`, avoiding profile creation in
    the final recorded routes.
- `scripts/apple-silicon/native-tri-depth-compare.sh`
  - runs paired baseline/native snapshot benchmarks with a shared scratch-HDD
    source and snapshot tag.
  - writes perf summaries and a cropped screenshot comparison to a
    `benchmark-runs/*-native-tri-depth-compare-*` report directory.
  - records native triangle-depth and related diagnostic environment toggles in
    benchmark metadata.
  - retries each side by default when a launch produces no usable perf summary,
    absorbing the nondeterministic Apple OpenGL startup crash seen locally.
- `scripts/apple-silicon/qmp-hmp.py`
  - sends one HMP command through the QMP `human-monitor-command` bridge.
- `scripts/apple-silicon/validate-native-tri-depth.sh`
  - runs or checks the flat-tri-depth XBE and fails unless the flat-first
    native / flat-nonfirst geometry fallback split is present.

Baseline app status:

- `./build.sh -a arm64` succeeds.
- `ninja -C build qemu-system-i386` succeeds.
- `dist/xemu.app` code-sign verification succeeds.
- `dist/xemu.app/Contents/MacOS/xemu --version` launches and reports Apple's
  OpenGL-on-Metal renderer.

## Important Findings

- macOS build packages `qemu-system-i386`.
- Apple Silicon build target is still `i386-softmmu`, so Xbox CPU code runs via
  QEMU TCG.
- Native arm64 baseline build now succeeds with `./build.sh -a arm64`.
- The packaged baseline app is `dist/xemu.app`.
- `dist/xemu.app` passes code-sign verification.
- `dist/xemu.app/Contents/MacOS/xemu --version` launches and reports:
  - GL vendor: `Apple`
  - GL renderer: `Apple M3 Ultra`
  - GL version: `4.1 Metal - 90.5`
- macOS currently links OpenGL.
- Vulkan is not enabled for Darwin in current Meson logic.
- Renderer default selection prefers OpenGL before Vulkan.
- Geometry shaders are used for most non-point primitive modes.
- Public issue #2506 ties severe macOS 3D performance regression to PR #2240.
- Public comments identify geometry shader usage as the likely cause and name
  geometry-shader removal as the real fix.
- B0/B1 log-based gameplay route metrics are recorded; automated screenshot
  capture remains unreliable from this Codex desktop context.
- Scripted smoke routes now navigate:
  - Crimson Skies through pilot registration into the in-engine sequence.
  - Rainbow Six 3 through default profile creation and Campaign into Hereford
    mission loading.
- Profile-prepared retail gameplay routes are now recorded and tracked:
  - PGR2 route:
    `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv`,
    `benchmark-runs/20260501-094823-pgr2`, 11.53 average FPS / 11.67 post-load
    average FPS, 1,516,519 geometry-shader draws, including 38,785 quad-family
    draws.
  - Rainbow Six 3 route:
    `scripts/apple-silicon/input-scripts/rainbow-gameplay.csv`,
    `benchmark-runs/20260501-095400-rainbow-six-3`, 24.19 average FPS / 24.76
    post-load average FPS, 692,438 geometry-shader draws, including 1,946
    line-family draws.
  - Crimson Skies route:
    `scripts/apple-silicon/input-scripts/crimson-gameplay.csv`,
    `benchmark-runs/20260501-095905-crimson-skies`, 15.44 average FPS / 15.80
    post-load average FPS, 786,722 geometry-shader draws, including 7,837
    quad-family draws.
- Retail performance target floor is sustained 30 FPS in gameplay for all
  tracked titles. 60 FPS is desirable but not the minimum bar.
- Baseline metrics are recorded in
  `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`.
- B0 Crimson Skies baseline:
  - run: `benchmark-runs/20260430-095612-crimson-skies`
  - average: 29.93 FPS over 139 intervals
  - tail-60 average: 30.98 FPS
- B1 Rainbow Six 3 baseline:
  - run: `benchmark-runs/20260430-095919-rainbow-six-3`
  - average: 26.45 FPS over 174 intervals
  - tail-60 average: 30.98 FPS
- Snapshot restore through QMP/HMP works for both current benchmark scenes:
  - Crimson tag `crimson_scene_b0`, saved in
    `benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2`.
  - Rainbow tag `rainbow_scene_b1_nothumb`, saved in
    `benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2`.
- OpenGL geometry-shader attribution counters are now included in
  `xemu-perf:` output:
  - module/program generation counters.
  - bind / not-dirty bind counters.
  - draw counters split into line, triangle, quad, and other primitive
    families.
- B2/B3 snapshot scene-entry runs with geometry counters are recorded in
  `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`.
- B2 Crimson Skies counter run:
  - run: `benchmark-runs/20260430-103500-crimson-skies`
  - average: 29.70 FPS over 28 intervals
  - post-load average after first five intervals: 30.98 FPS / 20.63 MSPF
  - geometry draws: 25,202, all triangle-family
- B3 Rainbow Six 3 counter run:
  - run: `benchmark-runs/20260430-103536-rainbow-six-3`
  - average: 29.23 FPS over 27 intervals
  - post-load average after first five intervals: 30.97 FPS / 17.66 MSPF
  - geometry draws: 149,961, all triangle-family
- Among the older snapshot scene-entry runs, Rainbow Six 3 is the better
  triangle-family geometry-shader overhead diagnostic because it issues roughly
  six times the geometry-backed draws of the Crimson scene over the same run
  length. For current retail gameplay work, start with PGR2.
- `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` is available as a temporary diagnostic
  toggle. It keeps triangle-family geometry shaders active but bypasses their
  depth-plane/slope calculation.
- D1 Rainbow Six 3 diagnostic run:
  - run: `benchmark-runs/20260430-104001-rainbow-six-3`
  - toggle: `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1`
  - average: 28.24 FPS over 25 intervals
  - post-load average after first five intervals: 30.72 FPS / 18.39 MSPF
  - geometry draws: 128,277, all triangle-family
  - result: no improvement over B3, with one late 162 ms frame-time spike.
- D1 suggests the performance issue is more likely geometry shader dispatch,
  Apple OpenGL driver behavior, or surrounding pipeline work than the
  triangle depth/slope arithmetic itself.
- `XEMU_DIAG_SKIP_TRI_GEOM=1` is available as a temporary diagnostic toggle. It
  bypasses geometry-shader program generation for triangle-family fill draws
  and lets OpenGL draw native triangles directly. This is not a correctness
  path because the fragment shader no longer receives the geometry shader's
  per-triangle depth payload.
- D2 Rainbow Six 3 diagnostic run:
  - run: `benchmark-runs/20260430-105200-rainbow-six-3`
  - toggle: `XEMU_DIAG_SKIP_TRI_GEOM=1`
  - average: 29.93 FPS over 27 intervals
  - post-load average after first five intervals: 30.96 FPS / 6.38 MSPF
  - geometry draws: 0
  - result: large frame-time improvement while FPS remains capped near 31 FPS.
- D2 strongly implicates geometry-shader dispatch or Apple's OpenGL
  geometry-shader implementation as the local bottleneck.
- `XEMU_NATIVE_TRI_DEPTH=1` is the completed current opt-in GL triangle-family
  fill replacement path. It bypasses triangle-family fill geometry shaders,
  then derives depth and polygon-slope offset from `gl_FragCoord` in the
  fragment shader. It is validated for the current opt-in triangle-fill
  coverage described below, but is not yet a default renderer path.
- `XEMU_DIAG_NATIVE_TRI_DEPTH=1` is still accepted as a compatibility alias for
  older notes and runs. If `XEMU_NATIVE_TRI_DEPTH` is explicitly set to `0`, the
  old alias no longer turns the path on.
- D3 Rainbow Six 3 diagnostic run:
  - run: `benchmark-runs/20260430-110636-rainbow-six-3`
  - toggle: `XEMU_DIAG_NATIVE_TRI_DEPTH=1`
  - average: 29.49 FPS over 27 intervals
  - post-load average after first five intervals: 30.96 FPS / 8.10 MSPF
  - geometry draws: 0
  - result: retains most of D2's frame-time improvement while moving toward a
    correctness-preserving replacement.
- D4 Crimson Skies diagnostic run:
  - run: `benchmark-runs/20260430-111006-crimson-skies`
  - toggle: `XEMU_DIAG_NATIVE_TRI_DEPTH=1`
  - average: 31.15 FPS over 28 intervals
  - post-load average after first five intervals: 30.98 FPS / 18.97 MSPF
  - geometry draws: 0
  - result: confirms the toggle runs on the second benchmark scene, though
    Crimson is less sensitive to the geometry-shader bottleneck.
- D5 Rainbow Six 3 line-safe rerun:
  - run: `benchmark-runs/20260430-111903-rainbow-six-3`
  - toggle: `XEMU_DIAG_NATIVE_TRI_DEPTH=1`
  - average: 30.89 FPS over 28 intervals
  - post-load average after first five intervals: 30.99 FPS / 6.35 MSPF
  - geometry draws: 0
  - result: historical Rainbow comparison point; use P1/P2 for current
    same-build baseline/native evidence.
- D6/D7 Crimson Skies line-safe reruns:
  - D6 run: `benchmark-runs/20260430-112058-crimson-skies`
  - D6 post-load average: 30.98 FPS / 27.53 MSPF
  - D7 run: `benchmark-runs/20260430-112157-crimson-skies`
  - D7 post-load average: 30.98 FPS / 20.30 MSPF
  - geometry draws: 0 in both runs, including line-family counters.
  - result: Crimson shows more frame-time variance; use it as a cross-check,
    not the primary geometry-dispatch timing scene.
- D8/D9 tightened native triangle-depth reruns:
  - D8 Rainbow run: `benchmark-runs/20260430-113642-rainbow-six-3`
  - D8 post-load average: 30.99 FPS / 6.47 MSPF
  - D8 native triangle-depth draws: 197,212; fallbacks: 0; geometry draws: 0.
  - D9 Crimson run: `benchmark-runs/20260430-113732-crimson-skies`
  - D9 post-load average: 30.98 FPS / 20.01 MSPF
  - D9 native triangle-depth draws: 71,277; fallbacks: 0; geometry draws: 0.
  - result: the safer flat-shading eligibility rule preserved the Rainbow
    performance win in the current benchmark scene. Later tightening keeps all
    flat-shaded triangle fills on the geometry-shader path until flat shading
    is deliberately validated.
- D10 Rainbow confirmation run:
  - run: `benchmark-runs/20260430-114511-rainbow-six-3`
  - D10 post-load average: 30.98 FPS / 6.78 MSPF
  - D10 native triangle-depth draws: 192,776; fallbacks: 0; geometry draws: 0.
  - result: repeats the D8 performance band and confirms the current Rainbow
    snapshot still stays entirely on the native triangle-depth path.
- A dedicated flat-shading test XBE now exists:
  - source: `scripts/apple-silicon/xbe-tests/flat-tri-depth/`
  - XBE: `scripts/apple-silicon/xbe-tests/flat-tri-depth/bin/default.xbe`
  - ISO: `scripts/apple-silicon/xbe-tests/flat-tri-depth/flat-tri-depth.iso`
  - manual copy:
    `/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso`
  - launcher target:
    `scripts/apple-silicon/run-benchmark.sh flat-tri-depth`
  - trace run `benchmark-runs/20260430-141331-flat-tri-trace` confirms the XBE
    sends `NV097_SET_SHADE_MODE` flat plus first/last
    `NV097_SET_PROVOKING_VERTEX`.
  - passing run `benchmark-runs/20260430-153555-flat-tri-depth` confirms the
    expected split: first-provoking flat triangles use the native path, while
    last-provoking flat triangles fall back to the geometry shader.
- `scripts/apple-silicon/extract-perf-summary.sh` now summarizes `xemu-perf:`
  logs into overall/post-load averages and key geometry/native counters.
- `scripts/apple-silicon/xbe-tests/flat-tri-depth/flat-tri-depth.iso` was
  rebuilt from the current source at 2026-04-30 14:38:34 CDT.
- New flat-triangle trace/validation runs:
  - `benchmark-runs/20260430-143952-flat-tri-depth`: rebuilt ISO, no trace,
    still reported all candidates as smooth.
  - `benchmark-runs/20260430-144128-flat-tri-depth`: rebuilt ISO with
    `-trace nv2a_pgraph_method`; trace showed flat-last draw methods, but perf
    still reported candidates as smooth.
  - `benchmark-runs/20260430-144451-flat-tri-depth`: after adding explicit
    method-owned `PGRAPHState` shade/provoking fields, trace still showed
    flat-last draw methods while perf still reported candidates as smooth.
- Current flat-shading conclusion:
  - Stale media is no longer the explanation; metadata now records the rebuilt
    ISO timestamp.
  - Renderer-side state tracing showed live PGRAPH state and bound shader state
    both become flat-first at bind, draw begin, and flush.
  - The earlier all-smooth summaries were caused by the short XBE's flat phase
    landing after the final regular one-second perf interval. A graceful final
    perf-log flush now captures the partial tail.
  - Validation run `benchmark-runs/20260430-153555-flat-tri-depth` passes the
    flat counter split: 480 `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, 304
    `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`, and 304 `GEOM_SHADER_DRAW_TRI`.
- `scripts/apple-silicon/compare-screenshots.py` now crops paired screenshots,
  writes baseline/candidate/diff images, and prints simple visual-diff metrics.
- First Rainbow Six 3 visual smoke comparison:
  - baseline geometry run: `benchmark-runs/20260430-115436-rainbow-six-3`
  - native triangle-depth run: `benchmark-runs/20260430-115335-rainbow-six-3`
  - 10s viewport crop comparison: mean absolute error 0.1854, RMS 1.2256,
    changed pixels above threshold 8: 0.4382%.
  - visual inspection did not show an obvious rendering break, but this is only
    a smoke check and not a proof of depth or polygon-offset correctness.
- Native triangle-depth coverage counters now split native draws by:
  - w-depth versus linear depth.
  - fill polygon offset.
  - smooth shading versus flat-first.
  - flat fallback and flat-nonfirst fallback.
- D11/D12 snapshot coverage runs:
  - D11 Rainbow: `benchmark-runs/20260430-120458-rainbow-six-3`, 30.97 FPS /
    6.36 MSPF post-load, 198,119 native draws, 0 fallbacks, 100,772 w-depth,
    97,347 linear-depth, 26,019 polygon-offset, all smooth.
  - D12 Crimson: `benchmark-runs/20260430-120553-crimson-skies`, 30.99 FPS /
    20.39 MSPF post-load, 71,436 native draws, 0 fallbacks, all linear-depth,
    24,738 polygon-offset, all smooth.
- D15 post-tightening Rainbow confirmation:
  - run: `benchmark-runs/20260430-121927-rainbow-six-3`
  - post-load average: 30.97 FPS / 6.41 MSPF
  - native triangle-depth draws: 201,450; fallbacks: 0.
  - coverage: 101,016 w-depth, 100,434 linear-depth, 26,823 polygon-offset,
    all smooth; flat fallback counters remained 0 because this scene has no
    flat-shaded triangle fills.
- Flat-first native triangle-depth eligibility was added after D15. It is a
  targeted correctness expansion based on the OpenGL first-provoking convention;
  local Crimson/Rainbow routes still do not exercise flat-shaded triangle fills,
  so a dedicated nxdk flat-tri-depth XBE was created for direct validation.
- D16 Rainbow flat-first eligibility check:
  - run: `benchmark-runs/20260430-135911-rainbow-six-3`
  - post-load average: 31.01 FPS / 7.62 MSPF
  - native triangle-depth draws: 192,998; fallbacks: 0; geometry draws: 0.
  - coverage: 100,772 w-depth, 92,226 linear-depth, 24,423 polygon-offset,
    all smooth; flat-first and flat fallback counters remained 0.
  - result: the flat-first eligibility expansion did not perturb the existing
    smooth Rainbow snapshot path.
- D13/D14 longer route coverage runs:
  - D13 Rainbow smoke route:
    `benchmark-runs/20260430-120653-rainbow-six-3`, 313,378 native draws, 0
    fallbacks, 89,376 polygon-offset, all smooth.
  - D14 Crimson smoke route:
    `benchmark-runs/20260430-120851-crimson-skies`, 328,477 native draws, 0
    fallbacks, 92,169 polygon-offset, all smooth.
- Crimson visual smoke comparison:
  - baseline geometry run: `benchmark-runs/20260430-121112-crimson-skies`
  - native triangle-depth run: `benchmark-runs/20260430-121141-crimson-skies`
  - 10s viewport crop comparison: mean absolute error 0.8191, RMS 2.3252,
    changed pixels above threshold 8: 1.7876%.
  - the baseline/native diff is much smaller than Crimson's normal temporal
    movement in this scene.
- Native triangle-depth is now promoted from raw diagnostic to stable opt-in
  experiment flag:
  - preferred flag: `XEMU_NATIVE_TRI_DEPTH=1`.
  - compatibility alias: `XEMU_DIAG_NATIVE_TRI_DEPTH=1`.
  - explicit preferred disable: `XEMU_NATIVE_TRI_DEPTH=0`, which wins over the
    alias if both are present.
  - control-plane smoke run: `benchmark-runs/20260430-173353-flat-tri-depth`,
    with `native_tri_depth=1 source=XEMU_NATIVE_TRI_DEPTH mode=safe` in the log
    and `env_XEMU_NATIVE_TRI_DEPTH: 1` in metadata.
  - conflict smoke run: `benchmark-runs/20260430-175500-flat-tri-depth`, launched
    with `XEMU_NATIVE_TRI_DEPTH=0` and `XEMU_DIAG_NATIVE_TRI_DEPTH=1`; it emitted
    no native enable line, reported 0 native triangle-depth draws, and kept
    51,863 triangle draws on the geometry-shader path.
- Same-build paired native triangle-depth comparisons:
  - P1 Rainbow report:
    `benchmark-runs/20260430-174138-native-tri-depth-compare-rainbow-six-3`.
  - P1 baseline/native runs:
    `benchmark-runs/20260430-174138-rainbow-six-3` and
    `benchmark-runs/20260430-174156-rainbow-six-3`.
  - P1 post-load MSPF: 23.10 baseline, 6.83 native.
  - P1 geometry draws: 79,775 baseline, 0 native.
  - P1 native draws: 124,914, covering 50,142 w-depth, 74,772 linear-depth, and
    23,976 polygon-offset draws.
  - P1 visual crop changed pixels: 0.6131%.
  - P2 Crimson report:
    `benchmark-runs/20260430-174443-native-tri-depth-compare-crimson-skies`.
  - P2 baseline/native runs:
    `benchmark-runs/20260430-174443-crimson-skies` and
    `benchmark-runs/20260430-174500-crimson-skies`.
  - P2 post-load MSPF: 29.47 baseline, 17.87 native.
  - P2 geometry draws: 20,041 baseline, 0 native.
  - P2 native draws: 61,983, all linear-depth, with 23,142 polygon-offset draws.
  - P2 visual crop changed pixels: 3.6913%; visual inspection showed aligned
    crops with differences concentrated on texture/detail edges rather than an
    obvious depth-order break.
- New validation file:
  `docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md`.
- A first Crimson D3 attempt crashed before QMP became available:
  - run: `benchmark-runs/20260430-110740-crimson-skies`
  - crash report: `~/Library/Logs/DiagnosticReports/xemu-2026-04-30-110745.ips`
  - stack pointed at Apple's `GLImageWork` texture upload path before perf
    intervals were emitted.
  - immediate rerun completed, so this is treated as nondeterministic Apple
    OpenGL startup behavior unless it becomes reproducible.
- A post-tightening Rainbow attempt also crashed before QMP became available:
  - run: `benchmark-runs/20260430-121742-rainbow-six-3`
  - crash report: `~/Library/Logs/DiagnosticReports/xemu-2026-04-30-121748.ips`
  - stack again pointed at Apple's OpenGL texture upload worker path before
    perf intervals were emitted.
  - immediate rerun completed as D15, so this remains categorized as
    nondeterministic Apple OpenGL startup behavior.
- A flat-tri-depth trace attempt also hit the same Apple OpenGL worker class:
  - run: `benchmark-runs/20260430-152912-flat-tri-depth`
  - crash report pasted in-thread for process 24357 at 2026-04-30 15:29:13
    CDT.
  - crashed in `GLImageWork` / `libGLImage.dylib` during `glTexImage2D`
    texture upload before any `xemu-perf:` interval was emitted.
  - immediate rerun completed and validation later passed as
    `benchmark-runs/20260430-153555-flat-tri-depth`.
- Two additional Apple OpenGL nondeterministic startup crashes hit during the
  native-quad implementation session, both crashing in
  `glgProcessPixelsWithProcessor` /
  `GLDTextureRec::uploadTextureLevel` before any geometry was issued:
  - `~/Library/Logs/DiagnosticReports/xemu-2026-05-01-110643.ips`: pid 75358
    crashed at process launch+1s during a PGR2 retry replay.
  - `~/Library/Logs/DiagnosticReports/xemu-2026-05-01-111351.ips`: pid 76151
    crashed about 10s into a PGR2 snapshot-capture run.
  - Immediate retries succeeded each time. The native-quad code is not
    implicated; the failures occurred before any quad dispatch ran.

Native quad bypass slice (2026-05-01):

- `XEMU_NATIVE_QUAD=1` is the new opt-in quad/quad-strip-family fill bypass.
  When set, the renderer:
  - Skips geometry-shader generation for `PRIM_TYPE_QUADS` and
    `PRIM_TYPE_QUAD_STRIP` in smooth-fill mode.
  - Issues `glDrawElements(GL_TRIANGLES, ...)` against a CPU-expanded
    triangle index buffer that uses the same diagonal triangulation the
    geometry shader's `calc_quadz(0, 2)` already used, so smooth
    interpolation is unchanged.
  - Reuses the `gl_FragCoord`-derived depth and slope path that
    `XEMU_NATIVE_TRI_DEPTH=1` introduced for triangles. The depth math is
    primitive-agnostic.
- Flat-shaded quads, line/point polygon modes, and any nonfill raster mode
  fall back to the geometry shader, mirroring the conservative
  triangle-fill flat handling.
- `XEMU_NATIVE_QUAD` is independent of `XEMU_NATIVE_TRI_DEPTH`; both are
  needed at the same time for the full geometry-shader bypass. Setting the
  flag to `0` explicitly disables it.
- New per-subtype geometry-shader counters
  (`GEOM_SHADER_DRAW_QUAD_LIST`, `GEOM_SHADER_DRAW_QUAD_STRIP`) and full
  native-quad counter family
  (`NATIVE_QUAD_DRAW`, `NATIVE_QUAD_DRAW_LIST`, `NATIVE_QUAD_DRAW_STRIP`,
  `NATIVE_QUAD_CANDIDATE*`, `NATIVE_QUAD_FALLBACK*`,
  `NATIVE_QUAD_DRAW_ZPERSPECTIVE`, `NATIVE_QUAD_DRAW_LINEAR_Z`,
  `NATIVE_QUAD_DRAW_POLY_OFFSET`) are surfaced in `xemu-perf:` lines and
  in `extract-perf-summary.sh` output.
- Triangle regression gate
  (`scripts/apple-silicon/validate-native-tri-depth.sh --run 22`) passed
  after the native-quad code landed:
  `benchmark-runs/20260501-105543-flat-tri-depth`.
- Rainbow Six 3 snapshot scene with both flags on
  (`benchmark-runs/20260501-110557-rainbow-six-3`) reported 30.97 post-load
  FPS / 6.71 MSPF, identical within noise to the prior
  `XEMU_NATIVE_TRI_DEPTH=1`-only result. Quad-free scenes are unaffected.
- PGR2 retail-gameplay route replays show large per-run variance because
  real-time-paced input lands the emulator on different scene mixes at
  different host throughputs:
  - `XEMU_NATIVE_TRI_DEPTH=1` reference run:
    `benchmark-runs/20260501-104158-pgr2`, 21.40 post-load FPS, 177,272 GS
    quad draws remaining.
  - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` run 1:
    `benchmark-runs/20260501-105825-pgr2`, 18.20 post-load FPS, 0 GS draws.
  - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` run 2:
    `benchmark-runs/20260501-110810-pgr2`, 24.81 post-load FPS, 0 GS draws.
  - The 36% spread between the two same-config runs makes whole-route
    averages unreliable as a comparator.
- PGR2 mid-route snapshot triplet (stable, paused-input replays of the same
  game state):
  - Snapshot capture: `benchmark-runs/20260501-112001-pgr2`, savevm tag
    `pgr2_gameplay_b4`.
  - Baseline (no flags): `benchmark-runs/20260501-115623-pgr2`, 4.39
    post-load FPS, 332,066 GS draws (329,044 triangle + 3,022 quad).
  - `XEMU_NATIVE_TRI_DEPTH=1`: `benchmark-runs/20260501-115654-pgr2`,
    16.02 post-load FPS, 11,745 GS draws (all quad), 1,264,676 native-tri
    draws.
  - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1`:
    `benchmark-runs/20260501-115725-pgr2`, 16.56 post-load FPS, 0 GS
    draws, 1,317,851 native-tri draws, 12,193 native-quad draws (all
    `LIST`, all `CANDIDATE_SMOOTH`, zero fallbacks, depth split 2,716
    z-perspective + 9,477 linear-z).
- Conclusion from the snapshot triplet: native-tri-depth alone is the big
  lift at this PGR2 scene (4.39 → 16.02 FPS, 3.6x). Adding native-quad on
  top is performance-correct but modest at this specific scene
  (16.02 → 16.56, +3.4%), because only 12,193 quad draws exist in the
  30-second window. The remaining gap to 30 FPS is no longer
  geometry-shader work; the next slice should target whichever subsystem
  Instruments or perf counters identify as dominant.
- CLI `-loadvm` failed for the Crimson snapshot with a saved USB hub
  device-tree mismatch, so the harness restores after startup through QMP/HMP.
- Rainbow Six 3 crashed Apple's OpenGL worker path when a thumbnail-bearing
  snapshot was present on the scratch HDD. Benchmark-created snapshots now
  default to no thumbnail, and the thumbnail-free Rainbow snapshot restored
  successfully.
- QMP `screendump` can crash Apple's OpenGL-on-Metal path and should not be the
  default capture method yet.
- macOS `screencapture` failed from this Codex desktop context with
  `could not create image from display`; Computer Use screenshots were usable
  for live route verification.

## Next Session Checklist

1. Start by reading this checklist plus the most recent 2026-05-01 notes:
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-gameplay-route.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-rainbow-gameplay-route.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-crimson-gameplay-route.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-tri-depth.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-sample.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgraph-fast-read.md`
2. Treat `XEMU_NATIVE_TRI_DEPTH=1`, `XEMU_NATIVE_QUAD=1`, and
   `XEMU_PGRAPH_FAST_READ=1` as the three completed current opt-in
   performance flags. They are independent and stack:
   - tri-depth: removes triangle-family fill geometry shader (flat-first
     native, flat-nonfirst falls back to GS).
   - native-quad: removes quad/quad-strip-family smooth-fill geometry
     shader by CPU-side index expansion to triangles.
   - fast-read: skips `pg->lock` for simple PGRAPH register reads, where
     a 32-bit aligned load is already atomic on aarch64/x86 and the mutex
     was strict overhead.
   Combined, they bring PGR2 retail gameplay from 11.67 to 31.76 post-load
   FPS over the full 300-second route. PGR2 now meets the 30 FPS retail
   gameplay floor.
3. Triangle regression gate is
   `scripts/apple-silicon/validate-native-tri-depth.sh --run 22`. Most
   recent passing run after the native-quad slice landed:
   `benchmark-runs/20260501-105543-flat-tri-depth`. Cite that run if the
   gate is invoked again unless triangle code changes.
4. Keep `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` available for targeted debugging,
   but leave it off for timing runs.
5. Use `scripts/apple-silicon/native-tri-depth-compare.sh` for snapshot-level
   same-build comparisons, but the retail gameplay route scripts are the
   user-visible 30 FPS target. Whole-route averages are not stable across
   runs because real-time-paced input drives the emulator into different
   scene mixes (run 1 18.20 FPS vs run 2 24.81 FPS for the same flag config
   on PGR2 — see the native-quad note). For trustworthy comparisons use the
   PGR2 mid-route snapshot below.
6. PGR2 mid-route snapshot (created in this session):
   - HDD: `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`
   - Tag: `pgr2_gameplay_b4`
   - Snapshot triplet (30 s replays):
     - Baseline: 4.39 FPS,
       `benchmark-runs/20260501-115623-pgr2`.
     - `XEMU_NATIVE_TRI_DEPTH=1`: 16.02 FPS,
       `benchmark-runs/20260501-115654-pgr2`.
     - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1`: 16.56 FPS,
       `benchmark-runs/20260501-115725-pgr2`.
   - All three runs use `noop.csv`. The third config has zero
     geometry-shader draws of any kind.
7. The PGR2 30 FPS gameplay floor is now met with all three flags on:
   - Snapshot at `pgr2_gameplay_b4` reaches 30.76 FPS (30 s replay) and
     30.70 FPS (60 s replay).
   - Full retail gameplay route reaches 31.76 post-load FPS over 279
     intervals with zero geometry-shader draws.
   The remaining session-to-session route variance is dramatically reduced
   because the emulator is no longer CPU-starved by lock contention.
   Profiling next steps for the remaining gap to 60 FPS:
   - Audit `pgraph_write` for safe lock-free fast paths on simple stores
     (write contention was 2.7% of TCG-thread time in the sample profile).
   - Audit `voice_lock`-protected NV_USER writes for the same pattern
     (6.9% of TCG-thread time in the sample profile).
   - Capture a fresh `sample` profile at the snapshot scene with all
     three flags on and identify the new dominant cost (likely candidates:
     remaining i386 TCG, NV2A PGRAPH command processing, surface/texture
     upload, fragment shader work).
   Capture a dated benchmark note before any code changes so the next
   slice stays data-driven.
8. Use this wrapper if the flat validation needs to be reproduced:

```sh
scripts/apple-silicon/validate-native-tri-depth.sh --run 20
```

   It runs the flat XBE with `XEMU_NATIVE_TRI_DEPTH=1`, extracts the perf
   summary, and fails if the expected native/fallback split is missing.

   Use this trace-heavy variant only for debugging:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_PERF_LOG_INTERVAL_MS=1000 \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

   Passing means the first-provoking flat phase produces nonzero
   `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, and the last-provoking flat phase
   produces nonzero `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` plus
   `GEOM_SHADER_DRAW_TRI`.
9. Run follow-up implementation/diagnostic changes against both the retail
   gameplay routes and the saved scene snapshots:
   - Crimson: load `crimson_scene_b0` from
     `benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2`.
   - Rainbow: load `rainbow_scene_b1_nothumb` from
     `benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2`.
   - PGR2: load `pgr2_gameplay_b4` from
     `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`.
10. Compare the result against R1/R2/R3 in
   `docs/apple-silicon/benchmarking.md`, the route notes in
   `docs/apple-silicon/benchmarks/`, B2/B3/D1/D2/D3/D4, D17, and P1/P2 in
   `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`, and the
   PGR2 snapshot triplet in
   `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md`.
11. Only after the next non-geometry-shader bottleneck is identified and a
   slice plan exists, decide whether V0/V1 Vulkan-over-Metal experiments
   are worth doing before the native Metal path.

## Update — 2026-05-01 measurement-infrastructure session

This session focused on the 60 FPS pursuit and explicit jitter
detection. Major outcomes (no FPS-improving code shipped, but
infrastructure and findings that scope the next slice):

### Measurement infrastructure landed

- **Sub-millisecond perf-log precision.** `hw/xbox/nv2a/pgraph/profile.c`
  now tracks frame-time in microseconds internally and emits
  `mspf_avg/mspf_min/mspf_max` with `%.3f` precision. The HUD plot
  (`ui/xui/debug.cc`) still consumes the integer-ms `frame_working.mspf`
  field — that path is unchanged.
- **Optional per-frame timing log.** `XEMU_PERF_FRAME_LOG=1` appends a
  `frame_mspf_us=v1,v2,...` field to interval lines (bounded 1024
  frames/interval, overflow recorded as `frame_mspf_us_dropped`). Default
  off.
- **Jitter metrics in `extract-perf-summary.sh`.** New keys both whole-run
  and `post_load_*`:
  - `fps_stddev`
  - `mspf_max_p50/p95/p99/max` over per-interval worst-frames
  - `mspf_avg_max`
  - `stutter_intervals_30fps/45fps/60fps` (intervals where
    `mspf_max > 33.3 / 22.2 / 16.7`)
  - `longest_stutter_run_30fps/60fps`
- **`scripts/apple-silicon/sample-profile.sh`.** Background a benchmark
  run, poll the run dir + xemu pid, attach Apple `sample` for a configured
  duration, write the full sample text plus a thread-bucket summary into
  the run dir. Fully autonomous, no user input.
- **`scripts/apple-silicon/compare-runs.sh`.** Compare two run dirs and
  emit a side-by-side jitter+FPS comparison plus a "regression /
  improvement / noise" verdict per metric. Default noise threshold 3 %,
  configurable via `NOISE_PCT`. Exit code reflects regressions.

### New benchmark notes

- `2026-05-01-baseline-jitter.md` — jitter analysis of the existing
  post-fast-read 300 s retail-route runs. Bottleneck classification via
  `avg_mspf` vs `1000/avg_fps`: PGR2 39 % renderer / 61 % CPU-or-lock,
  Rainbow 21 % / 79 %, Crimson 91 % / 9 %. **Crimson is renderer-bound,
  not CPU-bound** — lock-elision will not lift Crimson FPS.
- `2026-05-01-pgr2-bottleneck-postfast.md` — fresh `sample` profile of
  `pgr2_gameplay_b4` with all three flags on. TCG mutex wait collapsed
  from 32.6 % (pre-fast-read) to 8.9 %. `voice_lock` is now 6.8 % of TCG
  thread (essentially unchanged). `pgraph_write` is 1.4 %. **The pfifo
  thread is idle 41.5 % of the time on the FIFO condvar** — the renderer
  is no longer the binding constraint at this scene; the CPU emulator's
  real x86 work is. Floating-point helpers (`helper_mulss`,
  `helper_fmul_ST0_FT0`, `floatx80_mul`, etc.) show prominently.
- `2026-05-01-voice-fast-lock-investigation.md` — implemented
  `XEMU_VOICE_FAST_LOCK=1` (atomic OR/AND on `voice_locked[]` bitmap, no
  `cond_signal`). Snapshot showed essentially no FPS change with mixed
  jitter signals; retail route showed +91 % more 30 FPS stutter intervals
  (within run-to-run variance, but no positive evidence). **Not landed.**
  Code reverted. Negative result documented.

### Critical jitter finding

Crimson Skies' p99 worst-frame is **892 ms**, max **1310 ms**, with a
**16-second** longest contiguous stutter run. Per-interval drilldown
shows every stutter spike coincides with non-zero `SHADER_GEN`,
`SURF_TO_TEX`, or `TEX_UPLOAD` activity. Interval 15 of the recorded
Crimson route has 7 triangle draws over 1.3 seconds (≈ 187 ms per draw).
This is consistent with Apple's OpenGL-on-Metal driver compiling shaders
synchronously inside `glDrawElements` — a documented behavior in macOS
GL emulators. Without async shader compilation, sustained 60 FPS on
Crimson is unattainable regardless of TCG-side wins.

PGR2 retail route p99 is 38 ms, max 117 ms (well-behaved). Rainbow Six 3
retail route p99 is 139 ms, max 694 ms (bad tail; same shader-compile
shape).

### Reality check on the 60 FPS goal

The post-fast-read profile makes the upper bound on lock-elision work
clear: ~9 % of TCG-thread time remains in mutex wait. Even eliminating
all of it would lift FPS by at most that much. Going from ~31 FPS to 60
FPS on PGR2 requires roughly doubling TCG-thread throughput, which
lock-elision alone cannot deliver. The realistic 60 FPS path needs:

1. SSE / x87 floating-point helper audit. `helper_mulss`, `helper_mulps_xmm`,
   `helper_fmul_ST0_FT0`, `float32_mul`, `floatx80_mul` are all visible
   in the post-fast-read sample. If SSE float32 ops are going through
   softfloat (`soft_f32_mul`) when Apple Silicon has perfectly capable
   NEON float32, that is potentially a major TCG win. **Open
   investigation** — needs source-side audit of the i386 hardfloat path
   in QEMU.
2. TB-chain audit. `helper_lookup_tb_ptr` is 7.6 % of TCG thread; if
   chaining drops out more than necessary, the JIT spends more time in
   dispatch than in real code.
3. Async shader compile (Crimson and Rainbow tail jitter).
4. The native Metal renderer track (Phase 4 of `strategy.md`). The bigger
   Crimson lift, and breaks the Apple-OpenGL synchronous-shader-compile
   ceiling.

## Prioritized Next Tasks

User visual confirmation on real PGR2, Rainbow Six 3, and Crimson Skies
discs: 30 FPS feel with no rendering artifacts on 2026-05-01 with
`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`. The
three flags are validated for the current tracked title set.

In priority order, the next concrete tasks for a future session:

1. **Re-run the 300 s retail routes once with `XEMU_PERF_FRAME_LOG=1`**
   so each title has a frame-level mspf distribution captured. The
   per-frame log enables true frame-level p99 / p99.9 in
   `extract-perf-summary.sh` (extension still TODO — current jitter
   metrics use per-interval `mspf_max` as the percentile basis). Capture
   under fresh dated benchmark notes.
2. **Async shader compile for Crimson / Rainbow tail jitter.** Apple
   GL → Metal compiles synchronously inside `glDrawElements`; the existing
   `SHADER_GEN` counter only tracks xemu-side GLSL emission. Add a
   counter `SHADER_COMPILE_MS_TOTAL` per interval (host-side compile time)
   to attribute jitter to the synchronous compile. Then implement an
   `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` slice that compiles shaders on a
   worker thread with a placeholder bind on the renderer's critical
   path. Expect the largest user-visible jitter improvement on Crimson,
   minor effect elsewhere.
3. **SSE / x87 floating-point helper audit.** Confirm whether SSE
   single-precision ops (`helper_mulss`, `helper_mulps_xmm`,
   `helper_addss`, etc.) actually go through `soft_f32_mul` /
   `parts64_uncanon_normal` on Apple Silicon, or whether the QEMU
   hardfloat path is active for them. If softfloat, lifting them to
   hardfloat is the single largest potential TCG win on this platform.
   Document under `<date>-tcg-float-audit.md`.
4. **`pgraph_write` fast path** (`XEMU_PGRAPH_FAST_WRITE=1`). Smallest
   remaining lock-elision win at 1.4 % of TCG. Mirror `pgraph_read`:
   `default` slot writes and `NV_PGRAPH_INTR_EN` are safe; everything
   else (`NV_PGRAPH_INTR`, `NV_PGRAPH_INCREMENT`, `NV_PGRAPH_RDI_DATA`,
   `NV_PGRAPH_CHANNEL_CTX_TRIGGER`) mutates composite state and stays on
   the slow path. Low-risk, completes the read/write symmetry.
5. **`XEMU_PGRAPH_RELEASE_LOCK_DURING_GL=1`.** On scenes where the
   pfifo thread is *not* idle (Crimson) this is the bigger lock-elision
   win. The PGR2 snapshot showed pfifo thread is idle 41.5 % of the time,
   so this slice will have minor effect on PGR2 but should help Crimson
   if its bottleneck is partly draw-thread serialization.
6. **Broader title coverage before defaulting any flag.** Same as before;
   current three flags need a wider title shakeout (different genre /
   GPU mix) before flipping any to default-on.

Profile-guided rule still applies: every slice gets a fresh `sample`
profile (use `scripts/apple-silicon/sample-profile.sh` now) and a dated
benchmark note. Use `scripts/apple-silicon/compare-runs.sh` for the
before / after metric diff.

## Things Not To Forget

- Do not delete or overwrite the local BIOS/HDD/game files.
- Do not assume MoltenVK or KosmicKrisp is good enough without a run.
- Do not optimize from intuition when Instruments or counters can answer.
- Keep docs updated after each meaningful experiment.

## Useful Commands

Build baseline:

```sh
./build.sh -a arm64
```

Verify packaged app:

```sh
codesign --verify --deep --strict --verbose=2 dist/xemu.app
dist/xemu.app/Contents/MacOS/xemu --version
```

Show current commit:

```sh
git rev-parse HEAD
```

Find geometry shader use:

```sh
rg -n "geometryShader|GL_GEOMETRY_SHADER|pgraph_glsl_need_geom|EmitVertex|EndPrimitive" hw/xbox/nv2a/pgraph
```

Find macOS/Vulkan build logic:

```sh
rg -n "host_os == 'darwin'|vulkan =|OpenGL|Molten|Metal|VK_USE_PLATFORM" meson.build build.sh hw/xbox/nv2a ui
```

View public regression:

```sh
gh issue view 2506 --repo xemu-project/xemu --comments
```

View PR #2240:

```sh
gh pr view 2240 --repo xemu-project/xemu --comments
```

Replay retail gameplay routes:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/rainbow-gameplay.csv 300
```

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/crimson-gameplay.csv 300
```

Replay PGR2 with the completed opt-in triangle-family fill path:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

Replay PGR2 with both opt-in geometry-shader bypass slices (full
geometry-shader removal):

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_NATIVE_QUAD=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

Run the PGR2 mid-route snapshot triplet for stable comparisons:

```sh
SNAPSHOT_HDD=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2
TAG=pgr2_gameplay_b4
# A: baseline
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=$SNAPSHOT_HDD \
XEMU_BENCH_LOADVM_TAG=$TAG \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/noop.csv 30
# B: tri-depth only
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=$SNAPSHOT_HDD \
XEMU_BENCH_LOADVM_TAG=$TAG \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/noop.csv 30
# C: tri-depth + quad
XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=$SNAPSHOT_HDD \
XEMU_BENCH_LOADVM_TAG=$TAG \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Run snapshot scene-entry benchmarks:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=crimson_scene_b0 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=rainbow_scene_b1_nothumb \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Re-run D1 diagnostic only if confirmation is needed:

```sh
XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=rainbow_scene_b1_nothumb \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Re-run D2 diagnostic only if confirmation is needed:

```sh
XEMU_DIAG_SKIP_TRI_GEOM=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=rainbow_scene_b1_nothumb \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Re-run the opt-in native triangle-depth path only for regression checks:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=rainbow_scene_b1_nothumb \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=crimson_scene_b0 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Run a same-build paired native triangle-depth comparison:

```sh
scripts/apple-silicon/native-tri-depth-compare.sh \
  rainbow benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
  rainbow_scene_b1_nothumb 16
```

Rebuild the dedicated flat-shading XBE only if its source changes:

```sh
NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk \
PATH=/Users/jbbrack03/XEMU_MacOS/nxdk/bin:/opt/homebrew/Cellar/lld@19/19.1.7/bin:/opt/homebrew/opt/llvm/bin:$PATH \
make -C scripts/apple-silicon/xbe-tests/flat-tri-depth
```

Reproduce the passing flat-XBE validation:

```sh
scripts/apple-silicon/validate-native-tri-depth.sh --run 20
```

Summarize a run:

```sh
scripts/apple-silicon/extract-perf-summary.sh benchmark-runs/20260430-153555-flat-tri-depth
```

Trace flat-XBE state only if debugging a regression:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_PERF_LOG_INTERVAL_MS=1000 \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

Recommended next implementation shape:

- The flat-tri-depth begin/bind/flush logging has been added and validated.
  The mismatch was a perf-window artifact; graceful final perf flushing now
  captures the flat XBE tail.
- Treat `XEMU_NATIVE_TRI_DEPTH=1` and `XEMU_NATIVE_QUAD=1` as the completed
  geometry-shader bypass slices for smooth-fill triangle and quad/quad-strip
  primitives. Do not re-prove either slice unless triangle or quad code
  changes; the snapshot triplet at
  `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md` is the
  current paper of record.
- The next session's first task is to identify what is making PGR2 slow at
  the `pgr2_gameplay_b4` snapshot (16.56 FPS with both bypass slices on,
  zero geometry-shader draws). Use Instruments and the existing
  `XEMU_PERF_LOG=1` counters to measure i386 TCG, NV2A PGRAPH command
  processing, surface/texture upload, and fragment shader work in turn.
  Capture a dated benchmark note with the dominant cost before any code
  change.
- Defer further geometry-shader removal slices (flat-quad bypass,
  nonfill polygon modes, line/point primitive bypass) until a benchmark
  exercises that combination meaningfully. Today none of the
  Crimson/Rainbow/PGR2 routes do.
- Compare future renderer changes against R1/R2/R3, the route notes, the
  baseline-metrics file, and the PGR2 snapshot triplet
  `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md` before
  trying Vulkan-over-Metal.
