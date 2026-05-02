# xemu-fork — Project Memory (Apple Silicon Performance Fork)

This is the QEMU/xemu source tree for the Apple Silicon performance fork.
Working branch: `apple-silicon-performance`. Upstream remote: `upstream`
(xemu-project/xemu). Origin remote: `origin` (your fork).

For workspace-level rules, test assets, and the goal/scope of this project
see `../CLAUDE.md`. **Do not duplicate that content here** — read it.

## Canonical project documentation

Every Apple Silicon-specific decision, benchmark, and handoff lives under
`docs/apple-silicon/`. Read these before starting work:

- `docs/apple-silicon/handoff.md` — current state, source-code changes made,
  next-session checklist, useful commands.
- `docs/apple-silicon/decision-log.md` — append-only decisions with
  rationale and supersession markers.
- `docs/apple-silicon/strategy.md` — phased plan (Phase 0 baseline → Phase
  4 native Metal renderer).
- `docs/apple-silicon/research.md` — evidence base; cites local source
  references and public xemu issues.
- `docs/apple-silicon/benchmarking.md` — measurement matrix and retail
  performance gates.
- `docs/apple-silicon/automation.md` — benchmark harness, scripted-input
  format, perf-counter list, snapshot workflow.
- `docs/apple-silicon/benchmarks/<date>-<name>.md` — dated session notes,
  one per benchmark session. Add a new file for each meaningful run; do not
  edit older notes.

## Where the fork's changes live

The fork-specific source-code changes are concentrated in:

- `build.sh` — macOS arm64 build path, CMAKE export for Meson cross-build,
  duplicate-LC_RPATH cleanup before code-sign.
- `hw/xbox/nv2a/pgraph/` — perf logging, geometry-shader attribution
  counters, `XEMU_NATIVE_TRI_DEPTH` triangle-family fill replacement,
  diagnostic toggles.
  - `profile.c`, `pgraph.c`, `pgraph.h`, `debug.h` — counters and perf
    interval logging.
  - `gl/draw.c`, `gl/renderer.h`, `gl/shaders.c` — GL geometry-shader
    attribution and native-triangle-depth dispatch.
  - `glsl/geom.c`, `glsl/geom.h` — eligibility logic for native-tri-depth
    and the `XEMU_DIAG_*` toggles.
  - `glsl/psh.c`, `glsl/psh.h` — fragment shader depth/polygon-slope
    derivation for the native path.
- `ui/xemu-input.c` — `XEMU_SCRIPTED_INPUT` (CSV replay) and
  `XEMU_RECORD_INPUT` (CSV record).
- `ui/xemu-snapshots.c` — `XEMU_SNAPSHOT_NO_THUMBNAIL=1`.
- `scripts/apple-silicon/` — benchmark harness, perf summary, validators.

When making renderer changes, prefer searching the existing tree rather
than recalling specific files from memory; commits and the file list above
can drift.

## Runtime flags maintained by this fork

Stable opt-in:

- `XEMU_NATIVE_TRI_DEPTH=1` — opt-in triangle-family fill native GL path
  (no geometry shader; depth/slope derived in fragment shader). The
  flat-first case stays native; flat-nonfirst falls back to the geometry
  shader. Set `=0` to disable explicitly (overrides the alias below).
- `XEMU_DIAG_NATIVE_TRI_DEPTH=1` — compatibility alias for the above. New
  runs and scripts should use the stable name.
- `XEMU_NATIVE_QUAD=1` — opt-in quad/quad-strip-family fill bypass. CPU
  expands quads to triangles using the geometry shader's diagonal
  triangulation, and the fragment shader derives depth via the same
  `gl_FragCoord` path used by `XEMU_NATIVE_TRI_DEPTH`. Smooth fill only;
  flat-shaded quads, line/point polygon modes, and any nonfill raster mode
  stay on the geometry shader. Independent of `XEMU_NATIVE_TRI_DEPTH`; for
  the full bypass set both. Set `=0` to disable explicitly.
- `XEMU_PGRAPH_FAST_READ=1` — opt-in lock-free PGRAPH register read fast
  path. `pgraph_read()` returns a `qatomic_read()` snapshot of the
  requested register without acquiring `pg->lock` for `NV_PGRAPH_INTR`,
  `NV_PGRAPH_INTR_EN`, and the default `pg->regs_[]` slot reads.
  `NV_PGRAPH_RDI_DATA` still locks because reading it auto-increments
  `NV_PGRAPH_RDI_INDEX_ADDRESS`. Aligned 32-bit loads on aarch64 and x86
  are atomic, so the mutex is strict overhead for these reads; skipping it
  removes the bulk of TCG-thread mutex-wait time. Independent of the
  geometry-shader bypasses; biggest payoff comes when both bypasses are on
  (so the renderer holds the lock for shorter intervals). Set `=0` to
  disable explicitly.
- `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` — opt-in async shader compile
  worker. A `pgraph.gl_async_compile` thread bound to a third shared
  `g_nv2a_context_shader_compile` GL context runs `glLinkProgram` /
  `generate_shaders()` off the renderer's critical path; the renderer
  uses a "skip the draw" fallback while a binding is still being
  compiled (RPCS3 PR #4876 pattern). Counters
  `SHADER_COMPILE_ASYNC_QUEUED`, `SHADER_COMPILE_ASYNC_COMPLETED`,
  `SHADER_DRAWS_SKIPPED_PENDING` confirm worker drain and skipped-
  draw count. **Note:** correct & shipped opt-in, but does NOT fix
  Crimson Skies' 1.35-second worst-frame stutter (proved 2026-05-01;
  the stutter is on the TCG vCPU thread, not the renderer). Default
  off. See `docs/apple-silicon/benchmarks/2026-05-01-async-shader-compile.md`
  and `docs/apple-silicon/benchmarks/2026-05-01-gl-vs-metal-decision.md`.

Diagnostic toggles (intentionally not correctness paths):

- `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` — keep tri geometry shaders, skip
  their depth/slope math.
- `XEMU_DIAG_SKIP_TRI_GEOM=1` — skip tri geometry shaders entirely. Drops
  per-triangle depth payload; correctness-breaking.
- `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` — capped logging for shader-bind /
  draw-begin / draw-flush state correlation.

Logging:

- `XEMU_PERF_LOG=1` — emit `xemu-perf:` interval lines. `mspf_avg` /
  `mspf_min` / `mspf_max` are sub-millisecond floats; the renderer
  HUD's integer-ms `frame_working.mspf` is unaffected.
- `XEMU_PERF_LOG_INTERVAL_MS=N` — interval (default 1000).
- `XEMU_PERF_FRAME_LOG=1` — append per-frame `frame_mspf_us=v1,v2,...`
  to each interval line (bounded 1024 frames; overflow noted in
  `frame_mspf_us_dropped`). Off by default.
- `XEMU_PERF_SPIKE_LOG=1` — emit `xemu-spike: op=<name>
  duration_us=<n> now_us=<n>` per-event spike lines whenever a single
  timed renderer operation exceeds the spike threshold. Used to
  pinpoint which renderer path is slow during a stutter. Off by
  default.
- `XEMU_PERF_SPIKE_LOG_THRESHOLD_US=N` — minimum operation duration
  (microseconds) that triggers a spike line. Default 50000
  (50 ms). Lower values catch finer events at the cost of log volume.
- `XEMU_SNAPSHOT_NO_THUMBNAIL=1` — skip snapshot thumbnail capture.

Input automation:

- `XEMU_SCRIPTED_INPUT=path/to.csv` + `XEMU_SCRIPTED_INPUT_PORT=1`.
- `XEMU_RECORD_INPUT=path/to.csv` + `XEMU_RECORD_INPUT_PORT=1`.

Benchmark launcher knobs (read in `scripts/apple-silicon/run-benchmark.sh`):

- `XEMU_BENCH_HDD_SOURCE`, `XEMU_BENCH_HDD_IN_PLACE`, `XEMU_BENCH_LIVE_INPUT`,
  `XEMU_BENCH_RECORD_INPUT`, `XEMU_BENCH_SCREENSHOT_BACKEND`,
  `XEMU_BENCH_SCREENSHOT_INTERVAL`, `XEMU_BENCH_SCREENSHOT_START_DELAY`,
  `XEMU_BENCH_SAVEVM_AT`, `XEMU_BENCH_SAVEVM_TAG`, `XEMU_BENCH_LOADVM_TAG`,
  `XEMU_BENCH_LOADVM_AT`, `XEMU_BENCH_EXTRA_QEMU_ARGS`,
  `XEMU_BENCH_ALLOW_EXISTING`,
  `XEMU_BENCH_SURFACE_SCALE` (injects `[display.quality] surface_scale = N`
  into the per-run config; used for the GL renderer-load A/B test
  documented in `benchmarks/2026-05-01-gl-vs-metal-decision.md`).

When adding a new flag, also extend `extract-perf-summary.sh` and
`automation.md` so the value shows up in summaries and is documented.

## Commit and PR conventions

- Stay on the `apple-silicon-performance` branch unless rebasing.
- One concept per commit, with a short imperative subject. Existing fork
  commits use that style (`Add Apple Silicon performance instrumentation`,
  `Add benchmark input recording`, `Document retail gameplay performance
  targets`, etc.).
- For QEMU upstream patches, follow QEMU's checkpatch / Signed-off-by /
  style requirements. For Apple-Silicon-only fork commits those are
  recommended but not enforced.

## When work crosses subsystems

- Touching the renderer? Update `docs/apple-silicon/research.md` if a new
  source reference is relevant, and add a benchmark note. Do not change
  Vulkan or generic QEMU display code first; xemu uses `-display xemu` and
  a custom NV2A PGRAPH path (see `system/vl.c` references in
  `research.md`).
- Touching the benchmark harness? Update `docs/apple-silicon/automation.md`
  and add a perf-summary key to `extract-perf-summary.sh` if you added a
  counter.
- Touching the build / packaging? Document the change in
  `decision-log.md`; macOS build/package fixes are tracked there
  explicitly.
