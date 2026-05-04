# Benchmark Automation

Last updated: 2026-05-04 (Visual Flight Recorder added and wired into
`run-benchmark.sh` behind `XEMU_BENCH_VISUAL_ANALYSIS=1`; PGR2 Metal
surface/RTT canary is clean; Rainbow Six 3 loading-screen canary is
clean; Xbox boot/flubber canary is clean after the front-face fix. The
earlier green/wireframe report was the boot animation, not in-game
Crimson Skies. Crimson gameplay automation now completes without
aborting after the texture-DMA bounds and invalid-stage shader fixes,
but its current screenshot lands on a black transition/loading frame
and is not a visual canary. Added diagnostics
`XEMU_METAL_DISABLE_SURFACE_TEX_ADDRS=1` and `metal_tex_oob`;
front-fb fallback docs now reflect that it publishes the selected
render-target binding. The 2026-05-04 PGR2 attribution pass also added
Metal CPU wall-time counters:
`METAL_DISPATCH_US_TOTAL`, `METAL_TEX_BIND_US_TOTAL`,
`METAL_DRAW_ENCODE_US_TOTAL`, `METAL_DRAW_PASS_OPENS`,
`METAL_DRAW_PASS_COALESCED`, `METAL_DRAW_PASS_FLUSHES`,
`METAL_OPEN_PASS_FLUSH_US_TOTAL`, `METAL_TEX_UPLOAD_US_TOTAL`, and
`METAL_SURFACE_DOWNLOAD_US_TOTAL`; these are parsed by
`extract-perf-summary.sh`. Existing diagnostics/counters from 2026-05-03
remain available:
`XEMU_METAL_DIAG_CLEAR=1`, `XEMU_METAL_SCREENSHOT_SOURCE=vram:0xADDR`,
`METAL_FRONT_FB_PUBLISHES`, `METAL_SURFACE_CACHE_SIZE`,
`METAL_IMAGE_BLITS`, `METAL_SURFACE_RECREATE_SHAPE_MISMATCH`,
`METAL_SURFACE_VRAM_DIRTY_HITS`, `_UPLOADS`, `_UPLOAD_BYTES`,
`METAL_SCREENSHOTS_TAKEN`, plus input-latency counters
`INPUT_USB_POLLS`, `INPUT_BACKEND_UPDATES`, `INPUT_LAT_US_TOTAL`,
`INPUT_LAT_US_MAX`. `run-benchmark.sh` includes `sc2` and `halo`
title keys.)

This fork has a small scripted-input harness for repeatable Apple Silicon
benchmark runs. It is opt-in and does not affect normal xemu launches.

## Scripted Input

Set `XEMU_SCRIPTED_INPUT` to a CSV file before launching xemu:

```sh
XEMU_SCRIPTED_INPUT=scripts/apple-silicon/input-scripts/crimson-skies-smoke.csv \
XEMU_SCRIPTED_INPUT_PORT=1 \
dist/xemu.app/Contents/MacOS/xemu ...
```

The CSV format is:

```csv
time_ms,control,value
8000,start,1
8200,start,0
12000,a,1
12200,a,0
```

Button values are `0` or `1`. Axis values are signed 16-bit controller values
from `-32768` to `32767`.

Supported buttons:

- `a`, `b`, `x`, `y`
- `start`, `back`
- `white`, `black`
- `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`
- `lstick_btn`, `rstick_btn`, `guide`

Supported axes:

- `ltrigger`, `rtrigger`
- `lstick_x`, `lstick_y`
- `rstick_x`, `rstick_y`

## Benchmark Launcher

Run one of the local benchmark targets:

```sh
scripts/apple-silicon/run-benchmark.sh crimson
scripts/apple-silicon/run-benchmark.sh rainbow
scripts/apple-silicon/run-benchmark.sh pgr2
scripts/apple-silicon/run-benchmark.sh flat-tri-depth
```

Optional arguments:

```sh
scripts/apple-silicon/run-benchmark.sh crimson path/to/input.csv 240
```

Each run creates a directory under `benchmark-runs/` containing:

- `metadata.txt`: machine, commit, game, and input script details.
- `xemu.toml`: per-run config pointing at the scratch HDD and selected disc.
- `xemu.log`: xemu stdout/stderr, including scripted-input load status.
- `capture.log`: QMP screenshot capture status.
- `snapshot.log`: QMP/HMP `savevm` or `loadvm` status when requested.
- `screenshots/`: periodic PNG captures.
- `xbox_hdd.qcow2`: APFS-cloned scratch HDD copy when available.
- `qmp.sock`: QMP socket while the run is active.

The launcher intentionally uses a scratch HDD copy so benchmark navigation does
not mutate the source HDD image.

## Visual Flight Recorder

For renderer correctness work, do not rely on one still image when the route
could be in a transition, animated scene, loading screen, or flickering failure
state. Use a bounded visual timeline and summarize it into compact artifacts:

```sh
XEMU_RENDERER=METAL \
XEMU_METAL_TRANSLATED_PIPELINE=1 \
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_NATIVE_QUAD=1 \
XEMU_PGRAPH_FAST_READ=1 \
XEMU_METAL_FRONT_FB_FALLBACK=1 \
XEMU_METAL_MSAA=4 \
XEMU_METAL_SCREENSHOT_INTERVAL=120 \
XEMU_BENCH_VISUAL_ANALYSIS=1 \
scripts/apple-silicon/run-benchmark.sh \
  --metal-screenshot benchmark-runs/visual-checks/crimson-route.png \
  --metal-screenshot-at-frame 300 \
  crimson scripts/apple-silicon/input-scripts/crimson-gameplay.csv 90
```

With `XEMU_BENCH_VISUAL_ANALYSIS=1`, `run-benchmark.sh` writes the compact
report to `RUN_DIR/visual-analysis/` and logs analyzer output to
`RUN_DIR/visual-analysis.log`. To summarize an existing PNG sequence manually:

```sh
scripts/apple-silicon/visual-flight-recorder.py \
  --frames-dir benchmark-runs/visual-checks \
  --glob 'crimson-route*.png' \
  --out-dir /tmp/xemu-crimson-visual
```

For a run directory that already contains periodic `screenshots/`, the default
output location is `RUN_DIR/visual-analysis`:

```sh
scripts/apple-silicon/visual-flight-recorder.py \
  --run-dir benchmark-runs/20260504-100815-crimson-skies
```

The report contains:

- `visual-summary.json`: black-frame percentage, longest black/static runs,
  motion/change metrics, selected keyframes, and a small perf-log summary when
  `xemu.log` is available.
- `timeline.csv`: per-frame luma, nonblack percentage, entropy, perceptual
  hashes, and motion-vs-previous-frame metrics.
- `storyboard.jpg`: compact contact sheet of the selected frames.
- `keyframes/`: only the selected representative frames.

If a short screen recording is more useful than sampled PNGs, pass
`--video path.mov`. The analyzer extracts frames with `ffmpeg` into a temporary
directory and deletes them automatically at exit. Use `--keep-temp` only for a
one-off debugging session; do not keep extracted video frames under
`benchmark-runs/`.

The per-run config writes `[display.quality] surface_scale = N`, where
`N` defaults to **2** (matching the Apple Silicon system build's
first-run default — 1080p-class internal resolution). Override per
invocation with `XEMU_BENCH_SURFACE_SCALE=N` (e.g. for the GL renderer
A/B sweep documented in
`benchmarks/2026-05-01-gl-vs-metal-decision.md`). The harness's
`XEMU_BENCH_SURFACE_SCALE` and the runtime
`XEMU_DISPLAY_SCALE={1,2,3,4}` flag are parallel knobs at different
layers — the harness controls the per-run xemu.toml, while
`XEMU_DISPLAY_SCALE` overrides the loaded value at xemu startup
without touching any toml.

## Recording Input

Physical controller input can be recorded into the same CSV format used by the
scripted-input replay path:

```sh
scripts/apple-silicon/record-input.sh pgr2 300
```

The recording wrapper launches the selected game with port 1 open for a real
controller, records controller changes to `recorded-input.csv` in the run
directory, and still captures normal benchmark metadata/logs. Pass an explicit
output path when a stable route file should be overwritten directly:

```sh
scripts/apple-silicon/record-input.sh pgr2 300 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv
```

The recorder is controlled by:

- `XEMU_RECORD_INPUT`: output CSV path.
- `XEMU_RECORD_INPUT_PORT`: one-based controller port, default `1`.
- `XEMU_RECORD_INPUT_AXIS_DELTA`: minimum analog-axis change to record,
  default `1024`.
- `XEMU_RECORD_INPUT_AXIS_DEADZONE`: small neutral-axis values treated as zero,
  default `256`.

Replay a captured route with the normal launcher:

```sh
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

## Captured Retail Gameplay Routes

The current profile-prepared retail gameplay routes are:

| Game | Route File | Benchmark Note | Baseline Capture |
| --- | --- | --- | --- |
| PGR2 | `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv` | `docs/apple-silicon/benchmarks/2026-05-01-pgr2-gameplay-route.md` | `benchmark-runs/20260501-094823-pgr2` |
| Rainbow Six 3 | `scripts/apple-silicon/input-scripts/rainbow-gameplay.csv` | `docs/apple-silicon/benchmarks/2026-05-01-rainbow-gameplay-route.md` | `benchmark-runs/20260501-095400-rainbow-six-3` |
| Crimson Skies | `scripts/apple-silicon/input-scripts/crimson-gameplay.csv` | `docs/apple-silicon/benchmarks/2026-05-01-crimson-gameplay-route.md` | `benchmark-runs/20260501-095905-crimson-skies` |

Replay them from the prepared profile HDD:

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

For opt-in triangle-family fill regression checks, add
`XEMU_NATIVE_TRI_DEPTH=1`. For opt-in quad/quad-strip-family fill bypass on
top of that, also add `XEMU_NATIVE_QUAD=1`. For lock-free PGRAPH register
reads, also add `XEMU_PGRAPH_FAST_READ=1`. All three flags are independent
and stack:

- `XEMU_NATIVE_TRI_DEPTH=1` removes the triangle-family geometry shader.
  Flat-first triangles stay native, flat-nonfirst triangle strips and fans
  fall back to the geometry shader.
- `XEMU_NATIVE_QUAD=1` removes the quad/quad-strip geometry shader for
  smooth-shaded fill draws by expanding quads to native triangles on the CPU
  and reusing the same `gl_FragCoord`-derived depth/slope path the triangle
  bypass uses. Flat-shaded quads, line/point polygon modes, and any nonfill
  raster mode fall back to the geometry shader.
- `XEMU_PGRAPH_FAST_READ=1` skips `pg->lock` for simple PGRAPH register
  reads (atomic 32-bit loads of `NV_PGRAPH_INTR`, `NV_PGRAPH_INTR_EN`, and
  the default `pg->regs_[]` slot). Removes the bulk of TCG-thread
  mutex-wait time when the renderer is holding the lock during a draw.
  `NV_PGRAPH_RDI_DATA` still locks because the read has a side effect.
  Biggest gain shows up when combined with the geometry-shader bypasses,
  because the renderer's lock-hold time is what creates contention in the
  first place.
- `XEMU_TCG_SPLITWX={0,1}` overrides the splitwx auto-default for the TCG
  JIT. Apple Silicon system builds default to splitwx-on (the
  `mach_vm_remap` dual-mapping path in `tcg/region.c`), which removes the
  per-TB-execution `pthread_jit_write_protect_np()` syscall measured at
  ~16 % of TCG-thread on-CPU time during the Crimson Skies 1.35-second
  worst-frame stutter on 2026-05-01. Set `XEMU_TCG_SPLITWX=0` to fall back
  to the upstream MAP_JIT path (correctness-equivalent, same per-TB toggle
  cost). Set `XEMU_TCG_SPLITWX=1` to force splitwx on platforms where the
  auto-default is off. Memory implication: the splitwx path keeps two VA
  aliases of the JIT region, so committed virtual address space for the JIT
  buffer doubles (physical pages are shared via the same backing). Explicit
  `-accel tcg,split-wx=on|off` always wins over the env var; the env var
  wins over the auto-default. If splitwx allocation fails at startup the
  TCG init falls back to MAP_JIT and logs the failure once.
- `XEMU_TCG_JMP_CACHE_TARGETED={0,1}` overrides the per-page-targeted
  jmp-cache invalidation auto-default. Apple Silicon system builds default
  to ON. The slice replaces the unconditional 4096-entry jmp-cache zero
  inside `tb_jmp_cache_inval_tb`'s `CF_PCREL` branch (which i386
  system-mode sets globally; see `target/i386/cpu.c:9325`) with a single
  bucket clear per invalidated TB, batched after the
  `tb_invalidate_phys_page_range__locked` loop. The dominant residual cost
  in the V1 splitwx-on Crimson sample profile is `tcg_flush_jmp_cache`
  (1,982 of 20,680 TCG-thread samples = 9.6 %); shrinking it by a factor
  ~1/4096 per call is the leverage. Correctness: the lookup in
  `cpu-exec.c::tb_lookup` (line 267) validates `tb_cflags(tb) == s.cflags`
  and `do_tb_phys_invalidate` sets `CF_INVALID` (line 942) before removing
  the TB from `tb_ctx.htable` (line 949), so a stale `tb*` left in an
  unrelated jmp-cache bucket fails the cflags compare and falls through to
  `tb_htable_lookup`, which won't find the (already-removed) invalidated
  TB and translates fresh. Set `XEMU_TCG_JMP_CACHE_TARGETED=0` to fall
  back to the upstream full-zero path (rollback for A/B testing or
  correctness-regression triage); set `XEMU_TCG_JMP_CACHE_TARGETED=1` to
  force on where the auto-default is off. Non-PCREL TBs and the
  `tb_flush` / cputlb full-flush callers continue to use the unmodified
  full-zero path. The accompanying `TCG_JMP_CACHE_ZEROED_BUCKETS` and
  `TCG_INVALIDATE_WALL_US_MAX` perf counters (below) attribute the slice's
  effect on a per-interval basis.
- `XEMU_GL_MSAA={0,2,4,8}` opts in to multisample anti-aliasing on the
  OpenGL renderer path. Default: 0 (off; byte-identical to today). When
  set to 2/4/8, the renderer allocates each surface's color and depth
  attachments as multisample renderbuffers via
  `glRenderbufferStorageMultisample(samples, internal_format, w, h)` and
  attaches them to the draw FBO; the existing `gl_buffer` GL_TEXTURE_2D
  is kept as the resolved single-sample target that downstream consumers
  (surface download, surface-to-texture, display render) sample/read.
  Resolves are lazy: a per-surface `msaa_resolved` flag suppresses
  redundant blits, and the surface is marked dirty by
  `pgraph_gl_set_surface_dirty`. The requested sample count is clamped
  to `GL_MAX_SAMPLES` reported by the active GL context at init; the
  effective value is logged once at startup as
  `xemu-perf: gl_msaa=N source=XEMU_GL_MSAA requested=R max_samples=M`.
  Composes cleanly with `XEMU_DISPLAY_SCALE`/`surface_scale`: render
  dimensions feed both, so `surface_scale=2` plus `XEMU_GL_MSAA=4`
  yields a 2x supersampled, 4x multisampled internal target. New per-
  frame counter `MSAA_RESOLVE_US_TOTAL` (below) reports the wall-clock
  spent in the blit-resolves; `SHADER_COMPILE_*` counters should be
  watched the first time MSAA is enabled to confirm Apple's GL-on-Metal
  driver does not balloon pipeline-variant compile cost under the
  multisample render-target state, which is the renderer-side success
  criterion documented in `decision-log.md` ("Stay on OpenGL ...").
- `XEMU_RENDERER={OPENGL,VULKAN,METAL,NULL}` overrides the loaded
  `display.renderer` value for this xemu session without modifying
  the user's saved preference. Aliases: `GL` for `OPENGL`, `VK` for
  `VULKAN`. Case-insensitive. Unrecognized values are silently
  ignored. Used by `metal-shader-validation/run-validation.sh` to
  force METAL on a CI invocation while leaving the user's interactive
  default untouched. Requires the corresponding renderer to be
  compiled in (METAL only on darwin/aarch64; VULKAN only on Linux /
  Windows builds in this fork). Bridge implemented in
  `ui/xemu-settings.cc::xemu_settings_apply_renderer_env`. Apple
  Silicon performance fork.
- `XEMU_METAL_SHADER_VALIDATE={0,1,strict,2}` runs the M5 in-process
  shader-validation harness during xemu init. The harness exercises
  the GLSL → SPIR-V → MSL → MTLLibrary pipeline against the
  representative ShaderState fixtures in
  `hw/xbox/nv2a/pgraph/mtl/shader_validation.c` and prints a
  per-fixture pass/fail report to stderr (`[xemu-metal-validate] …`
  lines). `=1` is advisory (xemu continues after the report); `=strict`
  / `=2` aborts the process on any failure. Counters
  `METAL_SHADER_VALIDATE_OK` / `METAL_SHADER_VALIDATE_FAIL` reflect
  the latest run. The harness fires from two locations: (a) early in
  `xemu_metal_init()` (so it runs before any machine boot — pair with
  `XEMU_RENDERER=METAL`); (b) inside `pgraph_mtl_init()` when the
  Metal renderer is selected (so it runs once per machine boot under
  the user's normal flow). Default off. Apple Silicon performance
  fork; slice M5.
- `XEMU_METAL_SHADER_VALIDATE_AND_EXIT={0,1}` exits the xemu process
  after `XEMU_METAL_SHADER_VALIDATE` reports — used by
  `run-validation.sh` to avoid bringing up the GUI. Process exit
  code is 0 on full pass, 1 on any failure. Default off (validation
  runs but xemu continues). Apple Silicon performance fork; slice M5.
- `XEMU_METAL_VALIDATION={0,1}` (**M14 2026-05-02**) — opt-in Metal
  API validation layer for development. When set, `xemu_metal_init`
  promotes `MTL_DEBUG_LAYER=1` into the process environment **before**
  the first `MTLCreateSystemDefaultDevice()` call, which is the only
  point at which Apple's Metal framework reads `MTL_DEBUG_LAYER`
  (later `setenv` calls have no effect once a device exists). If the
  user has already pinned `MTL_DEBUG_LAYER` themselves the value is
  preserved (the `XEMU_METAL_VALIDATION=1` promotion uses
  `setenv(..., overwrite=0)`). Surfaced once at startup as
  `xemu-perf: metal_validation requested=R promoted=P
  mtl_debug_layer_active=A`, where `requested` follows
  `XEMU_METAL_VALIDATION`, `promoted` is 1 only if xemu actually wrote
  the env, and `mtl_debug_layer_active` reflects the live
  `MTL_DEBUG_LAYER` value at device-creation time.
  Production / shipped builds leave the variable unset and run with
  validation off; turn it on while debugging missing argument bindings,
  unbalanced retain/release on Metal objects, or an unexpected
  `MTLCommandBuffer` status. Default 0. Apple Silicon performance
  fork; slice M14. Implementation in `ui/xemu-metal.mm`.
- `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH={0,1}` (M7) forces
  `pgraph_mtl_heap_supports_framebuffer_fetch()` to return false even
  on Apple Silicon — the framebuffer-fetch combiner path then falls
  back to the render-pass-split fallback (currently a stub for Intel
  Mac targets, unreachable in production on this fork). The Apple1+
  detection is latched at `pgraph_mtl_heap_init()` from
  `[device supportsFamily:MTLGPUFamilyApple1]`; this flag overrides
  the result. Boot log reports `apple1_framebuffer_fetch=0|1`.
  Default 0. Apple Silicon performance fork; slice M7.
- `XEMU_METAL_FORCE_PASSTHROUGH={0,1}` (M7) forces every Metal-renderer
  draw onto the M3/M4 hand-coded passthrough pipeline. Skips the
  state-to-PipelineKey build and the translated-pipeline cache lookup
  entirely. Bisection knob — useful if a specific game's shader state
  causes the M5 GLSL→SPIR-V→MSL chain to misbehave; flipping
  `=1` reverts the renderer to the known-good M3/M4 state-of-the-art
  for that draw call without affecting the other renderer-init paths.
  Default 0. Apple Silicon performance fork; slice M7.
- `XEMU_METAL_TRANSLATED_PIPELINE={0,1}` (M7 / **M7.1**) opt-in for the
  translated pipeline encode path. **As of M7.1 (2026-05-02) this flag
  is functional** — when set to `1`, every eligible draw is encoded
  through the spirv-cross-built MTLRenderPipelineState with std140-
  packed VSH + PSH UBOs and per-NV2A-stage texture/sampler bindings.
  Per-draw counter is `METAL_DRAW_TRANSLATED`. When the lookup fails
  (translator failure or pipeline build error), the renderer falls
  back to M3/M4 passthrough and increments
  `METAL_PIPELINE_FALLBACKS`. With M8 (2026-05-02), when the lookup
  returns `PENDING` (async build in flight) the draw is **skipped**
  rather than blocking — see `METAL_DRAWS_SKIPPED_PENDING_TOTAL` and
  the `XEMU_METAL_ASYNC_PIPELINE_COMPILE` flag below. Use
  `XEMU_METAL_FORCE_PASSTHROUGH=1` to bypass the lookup entirely.
  Default 0 (opt-in for M7.1 / M8 user testing); the natural
  default-1 flip is queued behind real-game cold-launch testing.
  Apple Silicon performance fork.
- `XEMU_METAL_ASYNC_PIPELINE_COMPILE={0,1}` (**M8 2026-05-02**)
  overrides the async pipeline-compile auto-default. Apple Silicon
  system builds default to ON. When ON, a cache miss in
  `pgraph_mtl_shaders_get_pipeline_ex` transitions the entry to
  PENDING and dispatches the GLSL→MSL+library+pipeline build to a
  private serial concurrent dispatch queue at QoS_UTILITY. The
  device gets `setShouldMaximizeConcurrentCompilation:YES` driven on
  init (guarded by `respondsToSelector:` per the Dolphin Apple
  Silicon gotcha) so Metal parallelizes compile across CPU cores.
  The renderer thread returns immediately. With
  `XEMU_METAL_TRANSLATED_PIPELINE=1`, PENDING means "skip the draw
  this frame" — visual artifact (briefly missing geometry) instead
  of a frame stall. With the translated pipeline OFF, PENDING falls
  through to the M3/M4 passthrough so the cache warms in the
  background without affecting the encode path. Set `=0` to fall
  back to synchronous compile (block the renderer thread for
  5-50 ms per fresh shader pair). Counters
  `METAL_SHADER_COMPILE_QUEUED_TOTAL` /
  `METAL_SHADER_COMPILE_COMPLETED_TOTAL` /
  `METAL_SHADER_COMPILE_FAILED_TOTAL` /
  `METAL_DRAWS_SKIPPED_PENDING_TOTAL` /
  `METAL_DRAWS_USING_UBERSHADER_TOTAL` (last reserved for M8.1)
  surface on the `xemu-perf:` interval line. Companion fix shipped
  in the same slice: M6's `[cb waitUntilCompleted]` after every
  texture blit is replaced by a GPU-side `MTLSharedEvent` fence so
  per-draw command buffers `encodeWaitForEvent:` instead of the CPU
  blocking. No env var gates the upload-fence change — it's a
  correctness-equivalent replacement for the synchronous wait.
  Apple Silicon performance fork.
- `XEMU_GL_RATE_SLEW={0,1}` / `XEMU_RATE_SLEW={0,1}` (**M10
  prerequisite, 2026-05-02**) opts in to graphics-API-agnostic
  emulation-rate slewing. Reads the host display's current refresh rate
  via `SDL_GetCurrentDisplayMode` at window-creation time and on
  `SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED` /
  `SDL_EVENT_WINDOW_DISPLAY_CHANGED` /
  `SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED`. When the ratio
  `host_hz / 60.0` is in `[0.95, 1.05]` and the flag is enabled, sets
  `vblank_interval_ns = (uint64_t)(16,666,666 * (60 / host_hz))`. At
  host 59.94 Hz that's ~16,683,317 ns; at 60.05 Hz ~16,652,789 ns;
  matches the PCSX2 PR #5488 / DuckStation "sync to host refresh"
  pattern. Lands on the OpenGL backend as well as the Metal backend
  (the flag mutates the global `vblank_interval_ns` consumed by both
  the vblank-timer thread and the Metal frame-pacing computation).
  Default OFF for the first cut; flip default on after a benchmark
  validates jitter improvement. Out-of-range ratios fall through to
  the 60 Hz baseline so an unusual host (e.g. 30 Hz, 120 Hz) does
  not get mistreated as a slight drift. Audio rate-match is a known
  limitation: at typical 60.00 → 59.94 host refresh the audio drift
  is sub-perceptual (~0.1 %); a future slice can apply the matching
  ratio to AC97/MCPXAPU sample math, but it is not required for M10.
  Counters `RATE_SLEW_RATIO_E6` (host_hz / 60 × 1,000,000) and
  `RATE_SLEW_ACTIVE` (0/1) surface on the `xemu-perf:` interval line
  once the module is initialized. Naming note: `XEMU_GL_RATE_SLEW`
  matches the existing `XEMU_GL_*` family even though the flag is
  graphics-API-agnostic; `XEMU_RATE_SLEW` is the clearer alias and
  wins when both are set.
  Apple Silicon performance fork.
- `XEMU_METAL_MSAA={0,2,4,8}` (**M11 2026-05-02**) — opt-in
  multisample anti-aliasing on the Metal renderer. Default 0 (off);
  parsed once at `pgraph_mtl_init` and clamped against the active
  device's `[device supportsTextureSampleCount:N]` (Apple Silicon Mac
  M3 Ultra: 2 and 4 supported, 8 → 4). Effective value logged at
  startup as `xemu-perf: metal_msaa=N source=XEMU_METAL_MSAA
  requested=R configured=C`. Allocates a multisample companion
  texture per active color/depth surface (currently
  `MTLStorageModePrivate`; the optimal `MTLStorageModeMemoryless`
  storage class returns when a future slice coalesces per-`flush_draw`
  render passes into one render pass per frame — see
  `hw/xbox/nv2a/pgraph/mtl/heap.h` "storage-mode note"). Render
  passes use the multisample companion as `texture` and the existing
  single-sample binding as `resolveTexture`; color storeAction
  becomes `MTLStoreActionMultisampleResolve` and depth storeAction
  becomes `MTLStoreActionDontCare` (the post-resolve depth is not
  consumed by the present compositor). Pipeline `rasterSampleCount`
  is matched on both the M3/M4 hand-coded passthrough cache (the
  cache key gains a `sample_count` field, becoming (color_fmt,
  depth_fmt, variant, sample_count)) and the M5/M7.1 translated
  pipeline cache (`PgraphMtlPipelineKey.render_pass_state` already
  carries `sample_count`; renderer.c now passes the latched
  effective value rather than 1). Composes with `XEMU_DISPLAY_SCALE`
  / `surface_scale` (e.g. scale 2 + MSAA 4 = 1080p-class
  supersampled, 4× multisampled). Treated as session-fixed: changing
  the env requires restart so the pipeline cache does not balloon
  with sample-count variants. New counters
  `METAL_MSAA_RESOLVE_COUNT` (per-interval resolve count;
  always-on atomic incremented on each render pass that emits a
  multisample resolve), `METAL_MSAA_RESOLVE_US_TOTAL` (placeholder
  per-interval total in microseconds — populated at a fixed nominal
  cost per resolve in M11; M13's counter-sample-buffer integration
  will replace with GPU-side timing), and `METAL_MSAA_SAMPLE_COUNT`
  (the latched effective sample count). Implementation in
  `hw/xbox/nv2a/pgraph/mtl/{heap,surface,draw,pipeline,renderer}*`
  and `util/xemu-metal-perf.c`. Plan to lift to default 4× once
  warm-launch shader-compile cost with M9 cache hits is empirically
  below 200 ms total on PGR2 / Rainbow / Crimson.
  Apple Silicon performance fork.
- `XEMU_METAL_FX_SCALE={1,2,3}` (**M12 2026-05-02**) — opt-in
  `MTLFXSpatialScaler` upscale path on the Metal renderer. Default 1
  (off); `2` and `3` enable. The numeric value is preserved for
  forward-compat with future quality-tier variants — the present
  implementation is on/off, with the actual upscale ratio determined
  implicitly by `drawable_size / input_size`. Parsed once at
  `xemu_metal_init`; the scaler instance + intermediate output
  texture are built lazily on the first present that supplies an
  NV2A framebuffer texture, and rebuilt whenever input dimensions /
  pixel format / drawable size change. Effective config logged at
  startup as `xemu-perf: metal_fx_scale=N source=XEMU_METAL_FX_SCALE
  requested=R configured=C enabled=B`. Bypassed for the frame
  whenever the drawable is at-or-below the input (downscale via
  MetalFX adds latency for no quality win). Pipeline:
  NV2A color RT (post-M11 resolve)
    → `MTLFXSpatialScaler` (`encodeToCommandBuffer:` before render
       encoder opens; the scaler is a discrete pass operation)
    → private intermediate texture (`drawable_size`,
       `BGRA8Unorm_sRGB`, `MTLStorageModePrivate`, usage =
       `ShaderWrite | ShaderRead | RenderTarget`)
    → existing fullscreen-triangle present pipeline
    → drawable.
  `colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual`
  matches the M11 sRGB-tagged input/output pixel format. Composes
  orthogonally with `XEMU_DISPLAY_SCALE` / `surface_scale` and
  `XEMU_METAL_MSAA`: e.g. `surface_scale=2` + `XEMU_METAL_MSAA=4`
  + `XEMU_METAL_FX_SCALE=2` runs MetalFX from a 1080p
  4×-multisampled resolved texture up to drawable resolution.
  `MTLFXTemporalScaler` is intentionally not implemented for M12 —
  synthesizing motion vectors from camera-only reprojection is
  risky on dynamic scenes (NV2A has no native motion vectors);
  per-title evaluation is deferred. New counters
  `METAL_FX_SPATIAL_PRESENTS` (per-interval scaler invocations),
  `METAL_FX_SPATIAL_US_TOTAL` (placeholder per-interval CPU-side
  wallclock for the encode call in microseconds — under-reports
  GPU-side scaler cost; M13's counter-sample-buffer integration
  will replace with GPU-side timing) and `METAL_FX_SCALE_FACTOR`
  (latched effective config, 1 = off, >= 2 = on) surface on the
  `xemu-perf:` interval line. Implementation in `ui/xemu-metal.mm`
  and `util/xemu-metal-perf.c`. Apple Silicon performance fork
  (gated on `darwin && aarch64`; the `MetalFX` framework is added
  alongside the existing `Foundation`/`Metal`/`MetalKit`/`QuartzCore`
  module list in `meson.build`).
- `XEMU_METAL_CAPTURE=path.gputrace` (**M13 2026-05-02**) — programmatic
  Metal frame capture via `MTLCaptureManager`. When set, the Metal
  renderer starts a capture at `xemu_metal_init` time bound to the
  active `MTLDevice`, destination
  `MTLCaptureDestinationGPUTraceDocument`, output URL
  `file://<path>`. Capture is bounded by frame count so the
  `.gputrace` stays small enough to open in Xcode (each frame is
  ~10–50 MB); the bound is `XEMU_METAL_CAPTURE_FRAMES` (default 60,
  `0` = until shutdown). After the target is reached (or at
  `xemu_metal_shutdown`) the capture is finalized via `stopCapture`.
  Open the resulting file in Xcode (Window → Organizer → GPU Frame
  Capture) to inspect bound resources, draw call ordering, MSL
  source, and per-encoder GPU time. Preconditions: (a) the
  `MetalCaptureEnabled = YES` key is set in `Info.plist` for the
  shipped `dist/xemu.app` (added in this slice), or
  `MTL_CAPTURE_ENABLED=1` is set in the launching environment;
  (b) the active device must support
  `MTLCaptureDestinationGPUTraceDocument` (Apple Silicon does). If
  either fails, the renderer logs the failure and continues without
  capture — capture is a development tool and a missing `.gputrace`
  must not abort the run. Composes with the
  `--metal-capture <path>` flag added to
  `scripts/apple-silicon/run-benchmark.sh` (sets the env var for
  the spawned xemu, also records the path in the run's
  `metadata.txt`). Counters `METAL_CAPTURE_FRAMES` (frames seen
  since startCapture, monotonic; per-interval delta = frames
  captured this interval) and `METAL_CAPTURE_ACTIVE` (latched 0/1
  flag) surface on the `xemu-perf:` interval line. Apple Silicon
  performance fork; slice M13. See decision-log "2026-05-02:
  Metal slice M13 — frame capture + counter sampling".
- `XEMU_METAL_CAPTURE_FRAMES=N` (**M13 2026-05-02**) — frame-count
  bound for `XEMU_METAL_CAPTURE`. Default 60. Set `0` to capture
  until shutdown (large `.gputrace`; only useful for very short
  runs or single-frame regression triage). Ignored when
  `XEMU_METAL_CAPTURE` is unset. Apple Silicon performance fork;
  slice M13.
- `XEMU_METAL_SCREENSHOT_PATH=/path/to/file.png` (**2026-05-03**) —
  programmatic PNG screenshot of the final composited drawable,
  captured from inside the Metal renderer. Replaces the
  `screencapture`-based `scripts/apple-silicon/macos-capture.sh` path
  for benchmark validation: there is no Screen-Recording permission
  dialog, no window occlusion, and no impact on
  `addPresentedHandler:` accounting (so `METAL_PRESENTS` is not
  artificially zeroed by a foreground dialog). Capture point is
  AFTER the HUD ImGui-Metal encoder closes and BEFORE
  `presentDrawable:`, so the encoded image is byte-identical to what
  the user would see on screen. Implementation: a blit from the
  drawable texture into a host-shared `MTLBuffer`, then a
  `addCompletedHandler:` block that runs after the GPU finishes the
  cmdbuf, swaps BGRA→RGBA, and encodes the PNG via FPNG
  (`ui/thirdparty/fpng/`). Side effect: when the env is set,
  `s_layer.framebufferOnly` is flipped from `YES` to `NO` at
  `xemu_metal_init` so the drawable can be the source of a blit
  (display compression is off only for screenshot-enabled runs).
  PNG-encoding errors are logged and swallowed — capture is best-
  effort and never crashes the renderer. Companion env vars below;
  companion script flags `--metal-screenshot <path>` /
  `--metal-screenshot-at-frame <N>` on
  `scripts/apple-silicon/run-benchmark.sh`. Apple Silicon performance
  fork.
- `XEMU_METAL_SCREENSHOT_AT_FRAME=N` (**2026-05-03**) — frame number
  (1-indexed against the Metal renderer's submit-time end-of-frame
  counter) at which `XEMU_METAL_SCREENSHOT_PATH` fires. Default 60.
  Note: this counter is the renderer's "frame submitted" tick
  (incremented unconditionally inside `xemu_metal_end_imgui_frame`),
  not `pgraph_mtl_present_total` — the latter is bumped from
  `addPresentedHandler:` which does not fire while the macOS
  Screen-Recording dialog occludes the xemu window. Using the submit
  counter keeps the trigger deterministic on benchmark machines that
  do not have Screen-Recording permission granted to the launching
  shell.
- `XEMU_METAL_SCREENSHOT_INTERVAL=N` (**2026-05-03**) — when set to
  N≥1, the screenshot capture repeats every N frames after the first
  shot, writing `<base>.0001.png`, `<base>.0002.png`, ... (the
  `.NNNN` suffix is inserted before the trailing `.png` if present,
  appended otherwise). Default 0 = single shot at
  `XEMU_METAL_SCREENSHOT_AT_FRAME`. Counter
  `METAL_SCREENSHOTS_TAKEN` (per-interval delta) surfaces on the
  `xemu-perf:` interval line and counts only successfully-encoded
  PNGs (encoding failures log + skip without bumping the counter).
- `XEMU_METAL_SCREENSHOT_SOURCE={drawable,nv2a,vram:0xADDR}`
  (**2026-05-03**) — selects which texture the screenshot path
  captures. `drawable` (default, also accepts `0`) reads the
  post-HUD final drawable. `nv2a` (also accepts `1`) reads the
  NV2A framebuffer texture pre-present, which bypasses the present
  pipeline and is the diagnostic used to disprove OS-level layer
  substitution as the source of magenta artifacts. `vram:0xADDR`
  captures any specific cached `MtlSurfaceBinding` by vram_addr
  (e.g., `vram:0x32a4000` for PGR2's front buffer, `vram:0x3628000`
  for the back buffer, `vram:0x2c06000` for an aux RT) — the
  hex value is parsed with `strtoul(..., 0)` so `0x` is required.
  Used during the M5.9 magenta investigation to capture per-surface
  contents and confirm none of front/back/aux carry the rendered
  scene.
- `XEMU_METAL_DIAG_CLEAR={0,1}` (**2026-05-03**) — diagnostic logger
  for `pgraph_mtl_surface_clear`. When `=1`, emits up to 32
  one-line `xemu-perf: metal_surface_clear vram_addr=0x..
  rgba=(R,G,B,A) write_zeta=N` records, capped to keep the log
  bounded. Used during the M5.9 magenta investigation to confirm
  that no observed clear color is `(1.0, 0.0, 1.0, *)` — every
  PGR2 surface clear is `(0,0,0,1)` (front/back framebuffers) or
  `(1,0,0,1)` (PGR2's aux RTs at `0x2c06000` / `0x2e06000`).
  Default 0 (off; zero hot-path cost). Implementation in
  `mtl/surface.mm::pgraph_mtl_surface_clear`.
- `XEMU_METAL_DUMP_DRAW_RT=START:END:PREFIX` (**W4, 2026-05-04**) —
  per-draw color render-target dump on the Metal renderer. `START`
  and `END` are 0-indexed inclusive **cumulative-per-RUN**
  flush_draw indices (NOT per-frame; matches Mesa/RADV debug-dump
  semantics). `PREFIX` is a filesystem prefix; outputs are
  `<PREFIX>.<draw_index_zero_padded_6>.png` (e.g.
  `/tmp/wd_test.000010.png`). Empty / unset / malformed → disabled
  with zero hot-path cost. The dump runs against the
  **post-MSAA-resolve** color RT: at the end of every
  `pgraph_mtl_flush_draw` the open coalesced render pass is closed
  (so the resolveTexture is current), the bound color binding's
  MTLTexture is blit-copied into a host-shared MTLBuffer, and the
  cmdbuf's `addCompletedHandler` BGRA→RGBA-swaps and writes the PNG
  via FPNG. The renderer thread does not block. First five dumps
  emit a one-line `xemu-perf: metal_draw_rt_dump idx=N path=...`
  rate-limited line. Counter `METAL_DRAW_RT_DUMPS` (per-interval
  delta) surfaces on the `xemu-perf:` line and in
  `extract-perf-summary.sh`. Implementation in
  `mtl/draw.mm::pgraph_mtl_draw_dump_rt_*` + the
  `pgraph_mtl_flush_draw` hook in `mtl/renderer.c`.
- `XEMU_GL_DUMP_DRAW_RT=START:END:PREFIX` (**W4, 2026-05-04**) —
  GL-side equivalent. Synchronous-but-isolated: `glReadPixels` blocks
  the renderer thread for the duration of the readback (intentional
  for a debug-only path; expect a noticeable per-draw cost while in
  range). The active color RT is resolved through the existing
  `pgraph_gl_resolve_surface_msaa` helper (no-op when
  `XEMU_GL_MSAA=0`), bound to the renderer's resolve FBO, and read
  back as `GL_RGBA` / `GL_UNSIGNED_BYTE`. The result is flipped
  vertically (GL pixels are bottom-up) before PNG encode so the
  output orientation matches the Metal-side dump. Counter
  `GL_DRAW_RT_DUMPS` (per-interval delta; tracked via
  `NV2A_PROF_GL_DRAW_RT_DUMPS`) surfaces on the `xemu-perf:` line
  and in `extract-perf-summary.sh`. Implementation in
  `pgraph/gl/draw.c::pgraph_gl_draw_dump_rt_*` (called from
  `pgraph_gl_draw_end`) + `pgraph/gl/dump.cc` (FPNG shim).

  **Worked first-divergent-draw triage example.** Pair this with
  W2's `metal-gl-compare.sh` to bisect a Metal-vs-GL visual
  regression.

  ```sh
  # 1. Initial wide sweep — dump draws 0..199 from both renderers.
  XEMU_METAL_DUMP_DRAW_RT=0:199:/tmp/pgr2_metal \
  XEMU_RENDERER=METAL \
  ./scripts/apple-silicon/run-benchmark.sh pgr2 \
    scripts/apple-silicon/input-scripts/pgr2-smoke.csv 30

  XEMU_GL_DUMP_DRAW_RT=0:199:/tmp/pgr2_gl \
  XEMU_RENDERER=GL \
  ./scripts/apple-silicon/run-benchmark.sh pgr2 \
    scripts/apple-silicon/input-scripts/pgr2-smoke.csv 30

  # 2. Diff each pair to find the first divergent draw.
  for i in $(seq -f "%06g" 0 199); do
    if ! cmp -s /tmp/pgr2_metal.${i}.png /tmp/pgr2_gl.${i}.png; then
      echo "first divergent draw: ${i}"
      break
    fi
  done

  # 3. Narrow with a tight per-draw range around the divergence.
  XEMU_METAL_DUMP_DRAW_RT=42:46:/tmp/pgr2_div_metal \
  XEMU_RENDERER=METAL ...
  ```

  The dump always runs against the post-resolve color RT. If you
  need pre-resolve MSAA contents, use the `XEMU_METAL_CAPTURE`
  `.gputrace` capture path instead — that's M13's job, not this
  flag's.
- `METAL_FRONT_FB_PUBLISHES` (**M5.9, 2026-05-03**): per-interval
  count of front-fb texture pointer **changes** in the Metal
  renderer's surface cache. Always-on atomic. Bumped whenever the
  resolved front-fb MTLTexture differs from the previously-published
  pointer; deduped across repeated publishes of the same texture.
  Companion always-on diagnostic line emits per-change as
  `xemu-perf: metal_front_fb_publish vram_addr=0x.. width=W height=H
  format=FMT reason={crtc,clear}`. `reason=crtc` indicates a CRTC-
  scan publish from `pgraph_mtl_flip_stall` looking up
  `d->pcrtc.start + line_offset`; `reason=clear` indicates the
  legacy "publish-on-color-clear" fallback that fires before the
  CRTC publish for the first frame. Steady-state: 1-8 publishes per
  interval as the game cycles between front-buffer / back-buffer /
  aux-RT bindings. Zero publishes after the first frame indicates
  the renderer is stuck on the same front-fb (correctness-impacting
  if the game expects frame-to-frame variance). Surface-routing
  regressions of the M5.9 class are catch-able by counter
  inspection without requiring screenshot diffing. Apple Silicon
  performance fork; slice M5.9.
- `METAL_SURFACE_CACHE_SIZE` (**M5.9, 2026-05-03**): live count of
  entries in the Metal-renderer per-VRAM surface cache. Capped at
  16 (LRU eviction). Steady-state: 4-12 entries on PGR2 / Crimson /
  Rainbow (front buffer + back buffer + a few aux RTs). Always-on
  gauge (not a delta). Apple Silicon performance fork; slice M5.9.
- `METAL_IMAGE_BLITS` (**M5.9-followup-A, 2026-05-03**): per-interval
  delta of GPU-side surface-to-surface copies issued through
  `pgraph_mtl_surface_blit_copy` (the path A handler for
  NV097_IMAGE_BLIT — matching pixel-format rect-to-rect copy via
  MTLBlitCommandEncoder). Path B (format mismatch — invalidates
  dst entry, deferred to bind-time upload) and Path C (neither in
  cache — defer to bind-time) do NOT bump the counter. PGR2 doesn't
  use this op (counter is 0 in benchmarks); titles that DO use it
  see ~1-10 per interval at frame boundaries. Apple Silicon
  performance fork; slice M5.9-followup-A.
- `METAL_SURFACE_VRAM_DIRTY_HITS` (**M5.9-followup-B+C, 2026-05-03**):
  per-interval count of 0→1 transitions on a surface's `dirty_vram`
  atomic (i.e. the access-callback-detected events where guest CPU
  wrote to a watched VRAM range). Always-on. Companion diagnostic
  line emits per first-transition as
  `xemu-perf: metal_surface_dirty vram_addr=0x.. size=S
  write_addr=0x.. write_len=L is_color=B`. Steady-state for titles
  that use guest CPU memcpy as their back→front swap mechanism:
  positive (often 1-5/interval). For titles that don't (PGR2 is one
  — confirmed 0/interval in 90 s benchmark), it stays at zero.
  Apple Silicon performance fork; slice M5.9-followup-B+C.
- `METAL_SURFACE_VRAM_UPLOADS` (**M5.9-followup-B+C, 2026-05-03**):
  per-interval count of completed VRAM→texture uploads through
  `pgraph_mtl_surface::upload_vram_to_texture`. Triggered (1) at
  cache-allocate inside `cache_find_or_create_color/_depth` and
  (2) at runtime via `upload_dirty(vram_ptr)` (called from
  `pgraph_mtl_flush_draw`) and `upload_if_dirty_at(vram_addr,
  vram_ptr)` (called from `pgraph_mtl_flip_stall` before the
  CRTC publish). Always-on. Steady-state for the bind-allocate
  path: 1-2/sec on PGR2 (matches the cache churn at the 16-entry
  cap). Spikes when titles render to many distinct surfaces.
  Apple Silicon performance fork; slice M5.9-followup-B+C.
- `METAL_SURFACE_VRAM_UPLOAD_BYTES` (**M5.9-followup-B+C,
  2026-05-03**): per-interval bytes copied through the upload
  staging buffer (sum of guest 1× source sub-rect sizes for each
  upload). Useful for checking the bandwidth cost of frequent
  upload activity. Always-on. Apple Silicon performance fork;
  slice M5.9-followup-B+C.
- `METAL_SURFACE_RECREATE_SHAPE_MISMATCH` (**M5.9-followup-E,
  2026-05-03**): per-interval count of cache entries destroyed +
  recreated because a same-vram_addr bind asked for a different
  shape (width × height × format) than the existing entry. Each
  recreate clobbers all previously-rendered content for that
  vram_addr — non-zero rate explains "draws hit but screenshots
  show fresh texture content". After the followup-E color/depth
  cache split this counter should be **0/interval steady state**
  on PGR2; non-zero values indicate either a real shape change
  (rare) or a regression. Companion bounded log line (first 16
  events, color and depth tracked separately):
  `xemu-perf: metal_surface_recreate vram_addr=0x.. old=WxH/fmtN
  new=WxH/fmtN (color|depth)`. Apple Silicon performance fork;
  slice M5.9-followup-E.

**Diagnostic-only lines (NOT emitted as KEY=VALUE on the perf
interval line; emitted as their own xemu-perf: lines AFTER the
interval line is closed):**

- `xemu-perf: metal_draw_target vram_addr=0x.. count=N` (M5.9-followup-E,
  2026-05-03): per-interval per-vram_addr count of `pgraph_mtl_flush_draw`
  invocations whose bound color binding had `vram_addr`. Up to 32
  distinct addresses tracked; addresses beyond the cap accumulate
  into `metal_draw_target_overflow`. Used to measure WHICH cached
  surface receives draws — decisive for bridging the back-buffer →
  front-fb gap that PGR2 (and similar AAA Xbox titles) creates.
  Per-interval counts; the slot table is preserved across resets so
  recurring vram_addrs keep their slot. Always-on; zero hot-path cost
  when no flush_draw fires (i.e. on the GL renderer).
- `xemu-perf: metal_draw_target_first vram_addr=0x.. slot=K`
  (M5.9-followup-E): one-shot log per distinct vram_addr at first
  sighting, fires at most 32 times per session. Useful for spotting
  newly-appearing draw targets without waiting for the next interval
  emit.
- `xemu-perf: metal_draw_target_zero count=N` (M5.9-followup-E):
  per-interval count of flush_draw invocations where
  `pgraph_mtl_surface_get_color_vram_addr()` returned 0 — meaning
  EITHER no color binding was active OR the active binding came from
  the legacy ensure-by-shape fallback (no DMA-derived address) OR a
  real surface at vram_addr=0 (PGR2 has one). The diagnostic
  conflates these three; future refinement (a `metal_draw_target_no_color`
  bucket) will separate them.
- `xemu-perf: metal_draw_target_overflow count=N` (M5.9-followup-E):
  per-interval count of flush_draw invocations whose vram_addr did
  not fit in the 32-slot table (i.e. titles using >32 distinct
  surfaces). Non-zero overflow indicates the cap should be raised
  or the diagnostic widened.

### M5.10 — VRAM-coherent surface download (default-off, opt-in)

`XEMU_METAL_FRONT_FB_DOWNLOAD={0,1}` (**M5.10, 2026-05-03**) — opt-in
for the Metal renderer's GPU→VRAM surface-download path. **Default 0
(off).** Mirrors `vk/surface.c::pgraph_vk_surface_download_if_dirty`
(vk/surface.c:939-944) with Apple Silicon UMA semantics: the bound
color/depth cache entry is marked `draw_dirty=1` at end-of-`flush_draw`
and end-of-`clear_surface`; subsequent `pgraph_mtl_flip_stall` and
`pgraph_mtl_surface_flush` walk the cache and call
`pgraph_mtl_surface_download_if_dirty_at(vram_addr, vram_ptr,
mtl_after_surface_download, d)` on each dirty entry. The download
opens a `MTLBlitCommandEncoder copyFromTexture:toBuffer:` against a
Shared `MTLBuffer`, commits + waits, then memcpys the staging buffer
into `d->vram_ptr + vram_addr` and bumps `DIRTY_MEMORY_VGA |
DIRTY_MEMORY_NV2A_TEX` for the affected range so the next
texture-bind / display-path read picks up the freshly downloaded
pixels. A cross-queue `MTLSharedEvent` fence (`s_draw_done_event` in
`mtl/draw.mm`, signaled monotonically at every draw-cmdbuf commit;
consumed via `[cmd encodeWaitForEvent:value:]` at download time)
guards against the render-queue blit reading a draw-target texture
mid-render. A KVM/HVF-parity polling path in
`pgraph_mtl_surface_update` calls
`memory_region_test_and_clear_dirty(d->vram, addr, 0x1000,
DIRTY_MEMORY_NV2A)` per cached entry when `!tcg_enabled()`; under
TCG the per-CPU access-callback path is authoritative and the
poll is skipped. Counters `METAL_SURFACE_DOWNLOADS` and
`METAL_SURFACE_DOWNLOAD_BYTES` surface on the `xemu-perf:` interval
line.

**Why default off.** The infrastructure is correct (vk-pattern port)
but is not the current PGR2 canary path; PGR2 is now bridged by the
host-side front-fb fallback plus the 2026-05-04 surface/RTT fixes.
Until download is proven correct and useful on ≥ 2 distinct titles,
it remains opt-in instrumented infrastructure. Historical cold-boot
perf cost when the flag is on: ~4× slowdown vs flag-off on PGR2
(1.5 fps → 0.3 fps in the 2026-05-03 run). Source files:
`hw/xbox/nv2a/pgraph/mtl/surface.{h,mm}`,
`hw/xbox/nv2a/pgraph/mtl/renderer.c`,
`hw/xbox/nv2a/pgraph/mtl/draw.{h,mm}`,
`hw/xbox/nv2a/pgraph/mtl/blit.c`. Apple Silicon performance fork;
slice M5.10.

- `METAL_SURFACE_DOWNLOADS` (**M5.10, 2026-05-03**): per-interval
  count of completed GPU→VRAM downloads. Always reported (gated by
  the M5.10 flag at the call sites; counter weak-symbol-falls-back
  to 0 when the surface manager isn't loaded).
- `METAL_SURFACE_DOWNLOAD_BYTES` (**M5.10, 2026-05-03**):
  per-interval bytes copied through the download staging buffer
  (sum of guest 1× source sub-rect sizes for each download).

`XEMU_METAL_FRONT_FB_FALLBACK={0,1}` (**M5.10 experimental, 2026-05-03;
PGR2 canary PASS after 2026-05-04 surface/RTT fixes**)
— additional opt-in publish path that does NOT depend on VRAM
coherency. Default 0 (off). After the CRTC-strict publish in
`pgraph_mtl_flip_stall`, when the flag is on, the renderer ALSO calls
`pgraph_mtl_surface_publish_latest_draw_fallback()` which publishes
the selected color render-target binding as the front-fb side-channel.
Last write wins, so the fallback overwrites the CRTC-strict publish.
Use case: titles like PGR2 whose CRTC-pointed surface receives only
sporadic draws while the actual rendered scene goes to a different
render target. Does NOT require `XEMU_METAL_FRONT_FB_DOWNLOAD=1` — the
two flags are independent and the fallback is purely host-side.
Emits `xemu-perf: metal_front_fb_publish ... reason=fallback-latest-draw`
and bumps `METAL_FRONT_FB_PUBLISHES` per publish. **NOT
correctness-faithful**: the back buffer may have a different aspect
ratio than the front (PGR2: 2560×960 back vs 1280×960 front at
scale=2), and titles that legitimately use both front and back
surfaces (e.g. those with a real software composite step) will see
the wrong content. The compositor's present pipeline scales whatever
texture it gets to drawable extent regardless of input dims.
Implementation: `pgraph_mtl_surface_publish_latest_draw_fallback` in
`mtl/surface.mm`, called from `pgraph_mtl_flip_stall` in
`mtl/renderer.c`. Apple Silicon performance fork; slice M5.10
experimental plus 2026-05-04 follow-up.

`XEMU_METAL_DISABLE_SURFACE_TEX_ADDRS={0,1}` (**2026-05-04
diagnostic**) — when set, disables direct render-target-as-texture
lookup by VRAM address and forces the CPU texture path. Useful for
isolating surface texture aliasing and channel/alpha normalization
issues. The default direct path now uses dimension-aware lookup and
still forces A8R8G8B8 render targets sampled as linear A8R8G8B8-family
texture views through the CPU path; that rule fixed PGR2's
dotted/yellow text.

- `XEMU_METAL_FORCE_LEGACY_PRESENT={0,1}` (**M10 2026-05-02**)
  overrides the Metal frame-pacing path. Default 0: the Metal
  presenter calls `[cmdbuf presentDrawable:drawable atTime:t]` with
  `t` computed as the next vblank deadline (`mach_now_ns +
  vblank_interval_ns` for the first frame; subsequent frames extend
  the previous target by exactly one `vblank_interval_ns`). When the
  renderer falls behind by more than 2 vblank periods the deadline
  reseeds from "now" so the system never enters rapid-fire catch-up.
  Mirrors DuckStation's `metal_device.mm:2577-2601`. Set `=1` to
  fall back to plain `presentDrawable:` (no `atTime:`) for A/B
  comparison or correctness triage. The frame's drawable is
  unconditionally fitted with an `addPresentedHandler:` block that
  measures `|drawable.presentedTime - target_seconds|` per frame and
  feeds the running totals/max counters described below. The
  `atTime:` deadline is computed in mach-base seconds via
  `mach_timebase_info`. Counters `METAL_PRESENTS` (presented frames),
  `METAL_PRESENT_JITTER_US_TOTAL` / `_AVG` / `_MAX` (microsecond
  presentation jitter, where avg = total/presents),
  `METAL_DRAWABLE_ACQUIRE_FAILS` (frames skipped because
  `[layer nextDrawable]` returned nil) surface on the `xemu-perf:`
  interval line. `METAL_DISPLAY_LINK_CALLBACKS` is reserved for
  M10.1's `CAMetalDisplayLink` integration — currently zero on this
  slice. Note: this slice closes the emulator-display sync gap
  (host vsync vs guest pacing); it does not erase the 1.3 s class
  Crimson Skies stutter, which is guest-intrinsic asset-streaming
  hitches amplified by TCG ISA-emulation overhead and is out of
  scope for the renderer track.
  Apple Silicon performance fork.
- `XEMU_METAL_PIPELINE_CACHE={0,1}` (**M9 2026-05-02**) overrides the
  persistent MSL-source disk cache for the Metal renderer. Apple
  Silicon system builds default to ON. The cache lives at
  `<xemu_settings_get_base_path()>/metal_shaders/` (sibling to the
  GL-side `shaders/` directory) and persists the combined MSL source
  string per `PgraphMtlPipelineKey` hash, mirroring `gl/shaders.c`'s
  layout (top-16 / bottom-48 hash sharding,
  `metal_shader_cache_list` LRU index file). On cold launch the
  cached MSL feeds straight into `[device newLibraryWithSource:]`,
  bypassing the GLSL→SPIR-V→MSL spirv-cross translation — the most
  expensive per-shader cost in the M5–M8 build path. Per-file
  self-describing header carries the xemu version + a Metal feature-
  set fingerprint (`AppleGPUFamily<N>/macOS<major>.<minor>` —
  Apple7=M1, Apple8=M2, Apple9=M3) + the full `PgraphMtlPipelineKey`
  blob; on header mismatch the file is unlinked. Saves run on
  detached `metal-scache-<hash>` background threads (concurrent cap
  64, synchronous fallback above the cap). File-system errors fail
  soft (log + skip; the affected entry's cache becomes a no-op,
  renderer falls back to live translation). Set `=0` to fall back
  to the M5/M8 always-translate path. Counters
  `METAL_SHADER_CACHE_LOADS` / `METAL_SHADER_CACHE_HITS` /
  `METAL_SHADER_CACHE_MISSES` surface on the `xemu-perf:` interval
  line. The cache survives version upgrades only when the xemu
  version + feature-set strings match exactly; layout changes to
  `PgraphMtlPipelineKey` invalidate the cache automatically (the
  state-blob length check rejects mismatched-size entries). The
  rejection of `MTLBinaryArchive` (limited macOS coverage as of
  2026; DuckStation/Dolphin both reach the same conclusion) is
  recorded in decision-log "2026-05-02: Metal slice M9 — persistent
  MSL-source disk cache" and amends `strategy.md` Phase 4f.
  Apple Silicon performance fork.
- `XEMU_DISPLAY_SCALE={1,2,3,4}` overrides the loaded
  `display.quality.surface_scale` value for this xemu session without
  modifying the user's saved preference. Mirrors the per-run
  `XEMU_BENCH_SURFACE_SCALE` knob the benchmark harness uses (see
  "Benchmark Launcher" below): the harness writes
  `[display.quality] surface_scale = N` into the per-run xemu.toml,
  while `XEMU_DISPLAY_SCALE` is the runtime-env equivalent for direct
  xemu launches and CI invocations. Out-of-range values (anything not
  parseable as an integer 1..10) are silently ignored. Stacks with the
  geometry-shader bypasses; correctness-equivalent — only the internal
  framebuffer/texture render-target dimensions change. Apple Silicon
  system builds default `surface_scale` to 2 on first launch (1080p-class
  internal resolution, ~7 % renderer-cost growth on PGR2 vs scale 1; see
  `benchmarks/2026-05-01-gl-vs-metal-decision.md`); existing users with
  a stored config keep their current value untouched.
- `XEMU_APU_LOCK_RELEASE={0,1}` overrides the audio voice-lock release
  auto-default. Apple Silicon system builds default to ON. Slice
  mechanism: in `hw/xbox/mcpx/apu/vp/vp.c::voice_work_dispatch`, after
  the worker batch is signaled (`qemu_cond_broadcast(&vwd->work_pending)`)
  the APU worker thread releases `MCPXAPUState::lock` for the duration of
  `qemu_cond_wait(&vwd->work_finished, &vwd->lock)`, then re-acquires
  `d->lock` (and re-acquires `vwd->lock` after, preserving the legacy
  `d->lock` outer / `vwd->lock` inner lock ordering) before draining the
  worker mixbins into the caller's per-frame mixbins. The released window
  unblocks any vCPU MMIO write that takes `d->lock` —
  `NV1BA0_PIO_VOICE_LOCK` writes through `voice_lock()`, plus DSP X/Y/P
  memory writes through `gp_write` and `ep_write` — so they no longer
  block for the full ~5.33 ms VP frame. Targets D3-attributed Crimson
  voice-lock contention (21.3 s / 300 s of vCPU thread time on
  `mcpx-apu-vp/0xfe8202fc` = NV1BA0_PIO_VOICE_LOCK; see
  `benchmarks/2026-05-02-tcg-30fps-cap-attribution.md`).

  This slice is *distinct from* the prior reverted
  `XEMU_VOICE_FAST_LOCK` slice (decision-log entry "2026-05-01:
  XEMU_VOICE_FAST_LOCK not landed", note
  `benchmarks/2026-05-01-voice-fast-lock-investigation.md`): that slice
  tried atomic OR/AND on the `d->vp.voice_locked[]` bitmap and dropped
  the cond_signal — lock-elision at the bitmap level. It was rejected
  because the contention is at the audio-frame level, not the
  bit-update level. The new slice keeps `voice_lock()` exactly as it
  was on the vCPU side (still acquires `d->lock`, still updates the
  bitmap atomically, still signals `d->cond`) and instead shrinks the
  APU thread's `d->lock` hold so the vCPU's existing acquire is no
  longer contended for most of the VP frame.

  Snapshot/process/publish boundary: the snapshot is the existing
  `vwd->queue[]` (built under `d->lock` during the voice-list walk in
  `mcpx_apu_vp_frame`), holding voice handles + list ids. Voice config
  itself lives in *guest RAM* (read by workers via `voice_get_mask`
  from `address_space_memory`), already lock-free in upstream. The
  process phase is the worker threads consuming `vwd->queue[]` and
  running envelope / resample / SVF / HRTF math into per-worker
  `self->mixbins`. The publish phase re-acquires `d->lock`, then
  `vwd->lock`, drains `vwd->mixbins` into the caller's stack-allocated
  `mixbins[]`, and releases.

  Race widening (correctness): a vCPU `voice_lock(true)` MMIO write
  can now succeed while a worker is mid-process on the same voice.
  The worker reads voice config from guest RAM at the start of
  `voice_process` and may see partially-modified config if the vCPU
  then writes `voice_set_mask` between two of the worker's reads. This
  widens an existing race class — `vp_write` paths such as
  `SET_VOICE_TAR_VOLA` / `SET_VOICE_TAR_PITCH` / `SET_VOICE_LFO_ENV`
  already modify guest RAM without acquiring `d->lock` or
  `voice_lock`, so worker reads of those fields are already racy in
  upstream. The widening adds the same exposure to fields touched
  between `voice_lock(true)` and `voice_lock(false)` in `VOICE_ON` /
  `VOICE_RELEASE` sequences (envelope start / release-rate fields).
  Per-frame impact is bounded to ~256 samples (5.33 ms) of
  slightly-stale audio for affected voices on the worst case — well
  below the perceptual threshold for the volume / envelope deltas the
  race exposes.

  Set `XEMU_APU_LOCK_RELEASE=0` to fall back to the legacy
  lock-held-throughout-frame behavior (rollback for A/B testing or
  audio-correctness regression triage); set `=1` to force on where
  the auto-default is off. The env var is consulted once at APU
  device init (`mcpx_apu_realize`); the resolved value is logged at
  startup as `xemu-perf: apu_lock_release=N
  source=XEMU_APU_LOCK_RELEASE|auto-default`. The env var beats the
  build default. Accompanying counters `APU_LOCK_HOLD_US_TOTAL` and
  `APU_VCPU_LOCK_WAIT_US_MAX` (below) attribute the slice's effect.

- `XEMU_MACOS_NATIVE_INPUT={0,1}` (**slice N2, 2026-05-03**) — opt-in
  routing of controller polling through Apple's `GameController.framework`
  on macOS instead of SDL3. Default 0 (off; existing users see exactly
  today's SDL behavior). When set, `xemu_input_init` enumerates
  `[GCController controllers]`, registers
  `GCControllerDidConnectNotification` /
  `GCControllerDidDisconnectNotification` handlers, and assigns each
  connected controller a fresh `GCControllerPlayerIndex` (xemu port N
  ↦ playerIndex N). The read path
  (`xemu_input_update_sdl_controller_state` → native backend) replaces
  the SDL Gamepad property reads with direct `GCExtendedGamepad`
  property reads — no SDL event-queue drain, no main-thread hop. SDL
  still owns the connect/disconnect lifecycle and per-port binding
  (the rebind UI is built on SDL events; Linux + Windows builds keep
  depending on SDL); the native backend only takes over the read path
  on macOS. Logged once at startup as `xemu-perf: macos_native_input
  enabled controllers=N`, followed by a per-controller diagnostic
  line (`class=GCController/GCXboxGamepad/...` `vendor="..."`
  `haptics=yes/no` `playerIndex=N`). Connect / disconnect events emit
  matching `xemu-perf: macos_native_input connect ...` /
  `disconnect ...` lines. Rumble on the native path is intentionally
  a no-op for slice N2 — Core Haptics integration is the N4 slice; the
  first call logs a one-shot diagnostic so the absence of rumble is
  visible until N4 lands. Companion env / toggle `Info.plist`
  property: `GCSupportsControllerUserInteraction = YES` (added in
  this slice; opt-in to macOS Sonoma "Game Mode" polling-rate doubling
  for Bluetooth controllers when xemu is foreground+fullscreen).
  Apple Silicon performance fork; built only on darwin+arm64.

  Companion latency counters (slice N1, always-on; surface on the
  `xemu-perf:` interval line):

  - `INPUT_USB_POLLS` — guest interrupt-IN reads on the XID gamepad
    endpoint (`hw/xbox/xid-gamepad.c::usb_xid_gamepad_handle_data`).
    8 ms cadence on real hardware; the xemu USB stack passes through
    the same rate.
  - `INPUT_BACKEND_UPDATES` — calls to
    `xemu_input_update_controller` per interval (rate-limited to
    once per 2500 µs by `XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US`).
  - `INPUT_LAT_US_TOTAL` — sum of (guest-USB-poll-time minus
    last-backend-update-time) per port over the interval. Average
    cache-to-poll latency = `INPUT_LAT_US_TOTAL / INPUT_USB_POLLS`.
  - `INPUT_LAT_US_MAX` — worst-frame cache-to-poll latency in the
    interval. Should stay under 16,000 µs (one frame); typical
    values during gameplay are < 8,000 µs.

  Cost when no input activity occurred: zero (counters are emitted
  only when at least one is non-zero).

### Apple Silicon defaults

The Apple Silicon system build (`CONFIG_DARWIN && __aarch64__`) ships
with these auto-defaults applied at startup; each is overridable as
documented above:

- `display.quality.surface_scale = 2` on first launch (no
  `xemu.toml` present yet). Existing installs keep their stored value;
  set it via the in-app "Internal resolution scale" combo or override
  per-session with `XEMU_DISPLAY_SCALE=N`.
- `tcg,split-wx=on` (via the `mach_vm_remap` dual-mapping path);
  override with `XEMU_TCG_SPLITWX={0,1}` or
  `-accel tcg,split-wx=on|off`.
- Per-page-targeted jmp-cache invalidation; override with
  `XEMU_TCG_JMP_CACHE_TARGETED={0,1}`.
- APU voice-lock release (`MCPXAPUState::lock` released during the
  per-frame voice-worker batch wait); override with
  `XEMU_APU_LOCK_RELEASE={0,1}`.

## Profile Setup Runs

Before recording real benchmark routes, use live setup runs to create or select
game profiles without recording those menu actions:

```sh
scripts/apple-silicon/live-setup.sh pgr2 600
scripts/apple-silicon/live-setup.sh crimson 600
scripts/apple-silicon/live-setup.sh rainbow 600
```

These runs use a persistent copied HDD image at:

```text
benchmark-runs/profile-prep/xbox_hdd.qcow2
```

Use that prepared HDD as the source for later recorded or replayed benchmark
runs:

```sh
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/record-input.sh pgr2 300 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv
```

Set `XEMU_BENCH_EXTRA_QEMU_ARGS` when a benchmark run needs an extra xemu/QEMU
argument such as a trace selector:

```sh
XEMU_BENCH_EXTRA_QEMU_ARGS='-trace nv2a_pgraph_method' \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

This is intended for simple diagnostic arguments. The value is appended as
shell words by the launcher.

## Flat Triangle-Depth XBE

A dedicated nxdk test lives at
`scripts/apple-silicon/xbe-tests/flat-tri-depth/`. It builds:

- `bin/default.xbe`
- `flat-tri-depth.iso`

Build it with:

```sh
NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk \
PATH=/Users/jbbrack03/XEMU_MacOS/nxdk/bin:/opt/homebrew/Cellar/lld@19/19.1.7/bin:/opt/homebrew/opt/llvm/bin:$PATH \
make -C scripts/apple-silicon/xbe-tests/flat-tri-depth
```

The ISO is also copied to
`/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso` for manual
launches. The app alternates every 240 frames between:

- first-provoking flat-shaded triangles, expected to hit
  `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`.
- last-provoking flat-shaded triangles, expected to hit
  `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`.

Current validation status: the XBE boots, the rebuilt ISO timestamp is recorded
in benchmark metadata, and the native triangle-depth path now splits the two
flat phases as intended. Run
`benchmark-runs/20260430-153555-flat-tri-depth` produced 480
`NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST` draws for first-provoking flat triangles and
304 `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` fallbacks with 304
`GEOM_SHADER_DRAW_TRI` draws for last-provoking flat triangles.

The pass/fail wrapper is:

```sh
scripts/apple-silicon/validate-native-tri-depth.sh --run 20
```

It launches the flat XBE with `XEMU_NATIVE_TRI_DEPTH=1`, disables the older
diagnostic triangle flags, extracts the perf summary, and checks that
flat-first triangles draw natively while flat-nonfirst triangles still match
the geometry-shader fallback count. A fresh packaged-app run,
`benchmark-runs/20260430-210159-flat-tri-depth`, passed with 422 flat-first
native draws, 240 flat-nonfirst fallbacks, and 240 triangle-family geometry
shader draws.

Run it through the benchmark harness with:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 20
```

Run the trace-enabled variant only when debugging the flat native/fallback
split:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_PERF_LOG_INTERVAL_MS=1000 \
XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1 \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

Pass criteria for the flat XBE:

- first-provoking flat phases produce nonzero
  `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`.
- last-provoking flat phases produce nonzero
  `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` and corresponding
  `GEOM_SHADER_DRAW_TRI` activity.
- the run includes a final `xemu-perf:` interval with `final=1`, so short test
  tails are included in the counter summary.

The launcher refuses to start when another `dist/xemu.app/Contents/MacOS/xemu`
process is already running. This avoids mixing windows, QMP sockets, and save
state from overlapping benchmark runs. Close xemu first, or set
`XEMU_BENCH_ALLOW_EXISTING=1` only for manual experiments.

Screenshot cadence can be adjusted with environment variables:

```sh
XEMU_BENCH_SCREENSHOT_INTERVAL=5 \
XEMU_BENCH_SCREENSHOT_START_DELAY=2 \
scripts/apple-silicon/run-benchmark.sh crimson
```

Screenshot backend defaults to macOS `screencapture`, which avoids touching the
guest OpenGL framebuffer:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=macos scripts/apple-silicon/run-benchmark.sh crimson
XEMU_BENCH_SCREENSHOT_BACKEND=none scripts/apple-silicon/run-benchmark.sh crimson
```

`XEMU_BENCH_SCREENSHOT_BACKEND=qmp` is available for experiments, but it has
crashed Apple's OpenGL-on-Metal path during early testing and should not be the
baseline capture method yet.

Frame pacing and NV2A profile summaries are enabled by the launcher through
`XEMU_PERF_LOG=1`. The summary interval defaults to one second and can be
changed with:

```sh
XEMU_PERF_LOG_INTERVAL_MS=500 scripts/apple-silicon/run-benchmark.sh crimson
```

The resulting `xemu.log` contains `xemu-perf:` lines with interval FPS,
sub-millisecond `mspf_avg` / `mspf_min` / `mspf_max` (microsecond
precision internally, emitted with `%.3f`), and nonzero NV2A counters.
The OpenGL path also reports geometry-shader attribution counters:

At graceful process exit, xemu emits one final partial interval with
`final=1`. The benchmark launcher waits briefly for QMP `quit` before falling
back to termination so this final counter flush can run.

Optional per-frame timing log: `XEMU_PERF_FRAME_LOG=1` appends a
`frame_mspf_us=v1,v2,...` field to each interval line, recording the
microsecond mspf of every frame in that interval (bounded to 1024 frames
per interval; overflow recorded in `frame_mspf_us_dropped`). Default
off; enable when frame-level p99 / p99.9 percentiles are needed.

### Per-event spike log (`XEMU_PERF_SPIKE_LOG`, `XEMU_PERF_SPIKE_LOG_TCG`)

For attribution of single-frame stutters that the per-interval
`xemu-perf:` line cannot explain, the per-event spike log emits one
`xemu-spike:` line per timed operation that exceeds a duration
threshold. Format:

```
xemu-spike: op=<name> duration_us=<n> now_us=<n> [extra=fields...]
```

`now_us` is `qemu_clock_get_us(QEMU_CLOCK_REALTIME)` — the same clock
domain used by the `xemu-perf:` interval timestamps, so spike lines can
be correlated to the per-interval `mspf_max` they fired inside.

Two enable bits, each off by default:

- `XEMU_PERF_SPIKE_LOG=1` — renderer-thread spike sources only:
  `draw_begin`, `flush_draw`, `surf_to_tex`, `surf_download`,
  `surf_upload`, `flip_stall_glfinish`, `bind_textures`, `tex_upload`.
  Used since the V0/V1 baseline to attribute renderer-thread cost.
- `XEMU_PERF_SPIKE_LOG_TCG=1` (V3, 2026-05-02) — TCG-thread and
  pfifo-thread spike sources:
  - `tcg_tb_chain` — one full pass through `cpu_exec_loop`'s inner
    while-handle-interrupt loop. `extra` carries `tb_count=N
    first_pc=0xADDR` so a spike here attributes the cost to a specific
    chained-TB sequence on the vCPU thread.
  - `tcg_invalidate_burst` — one call to
    `tb_invalidate_phys_page_range__locked` exceeded the threshold.
    `extra` carries `burst=N page=0xADDR`. V2 evidence shows per-call
    max is ~700us so this rarely fires at the default 50ms threshold;
    that's the point — if it fires during the worst frame, R2's
    "invalidation cost is not the source" verdict was wrong.
  - `tcg_notdirty_storm` — sliding 1-second window detected
    >100,000 `notdirty_write` trips per second (SMC re-trapping
    storm). `extra` carries `events=N rate_per_s=N`.
  - `tcg_x87_storm` — sliding 1-second window detected >50,000,000
    x87 helper calls per second across `helper_fmul_ST0_FT0`,
    `helper_fadd_STN_ST0`, `helper_fsub_STN_ST0`,
    `helper_fdiv_STN_ST0`. `extra` carries `events=N rate_per_s=N`.
  - `tcg_pg_lock_wait` — TCG vCPU thread waited >threshold to acquire
    `pg->lock` for an MMIO read/write (`pgraph_read` /
    `pgraph_write`). Spike here means the renderer/pfifo thread held
    the GL critical section beyond the threshold.
  - `renderer_pg_lock_wait` — pfifo (renderer) thread waited
    >threshold to acquire `pg->lock` in `pfifo_run_puller`. Spike
    here means the TCG vCPU thread held `pg->lock` (rare; useful as
    a cross-check).
  - `qemu_main_loop_iter` (D3, 2026-05-02) — full `main_loop_wait`
    iteration's *post-poll dispatch* phase exceeded threshold. The
    blocking `os_host_main_loop_wait` (which can sleep up to the
    soonest-timer deadline) is excluded from the duration; only the
    BH dispatch + timer fire phase counts. `extra` carries
    `total_us=N nonblocking=0|1` so the sleep portion is visible.
    A spike here means an iothread BH or main-loop timer ran for
    >threshold, blocking everything else from running on the iothread.
  - `bql_acquire_wait` (D3) — a single `bql_lock()` call waited
    >threshold to acquire the BQL. `extra` carries
    `from=<file>:<line>` (the bql_lock_impl call site, captured by
    the `__FILE__/__LINE__` macro). A vCPU spike here means another
    thread (iothread, pfifo, GL worker, audio worker) held the BQL
    while doing slow work; identify which thread by checking
    `qemu_main_loop_iter` / `aio_run_iter` / `mmio_helper_block`
    spikes that fired in the same window.
  - `aio_run_iter` (D3) — one `aio_dispatch()` pass (BH dispatch +
    fd handlers + timer dispatch) exceeded threshold. Useful for
    catching slow BHs or block-layer callbacks that are not visible
    on the main `qemu_main_loop_iter` axis.
  - `mmio_helper_block` (D3) — a single guest MMIO store helper
    (`do_st_mmio_leN`) including BQL acquisition, dispatch, and
    return exceeded threshold. `extra` carries
    `size=N addr=0xADDR mr=<name>` so the implicated MMIO region is
    obvious. **Critical for the D3 Crimson investigation**: a spike
    on `mr=mcpx-apu-vp` at `addr=0xfe8202fc` corresponds to
    `NV1BA0_PIO_VOICE_LOCK` blocking on `MCPXAPUState::lock` while
    the audio worker's `se_frame()` is mid-iteration.
  - `tcg_handle_interrupt` (V6, 2026-05-02) — one
    `cpu_handle_interrupt` call exceeded threshold. The function
    early-exits in <1 µs when no interrupt is pending; spikes here
    at the 1 ms threshold mean either (a) `bql_lock()` inside the
    `unlikely(cpu_test_interrupt(...))` branch waited for another
    thread to release the BQL, or (b) the target
    `cpu_exec_interrupt` callback itself did expensive work
    (i386 IRQ injection, exception delivery, SVM/VMX hooks).
    `extra` carries `exit=N ex_idx=N int_req=0xN` (post-call
    state — `interrupt_request` may have been cleared by the
    handler). One of the three V6 sources used to decompose the
    1 ms-class `tcg_tb_chain` events at Xbox kernel PC `0x80030e4c`
    that dominate the residual Crimson worst-frame attribution.
  - `tcg_tb_lookup` (V6) — one `tb_lookup` call (per-CPU jmp-cache
    probe + qht hash lookup on miss) exceeded threshold. Cost is
    sub-microsecond in steady state; a 1 ms-class spike points the
    bottleneck at jmp-cache thrash (recently-flushed cache after
    `tb_flush` / `tb_invalidate_phys_page_range__locked`) or a
    pathological qht hash chain walk. `extra` carries
    `pc=0xPC hit=0|1` (1 = lookup found a TB, 0 = miss → translation
    follows). Distinct from translation cost (`tcg_tb_gen_code`)
    and interrupt-handling cost (`tcg_handle_interrupt`).
  - `tcg_tb_gen_code` (V6) — one `tb_gen_code` call (the TCG
    translation pass plus its `mmap_lock`/`mmap_unlock` bracket)
    exceeded threshold. Only fires after a `tb_lookup` miss. **The
    leading hypothesis for the unattributed Crimson worst-frame
    remainder**: 11.8 % of vCPU wallclock spent in 1 ms-class
    chains starting at kernel PC `0x80030e4c` is consistent with
    code re-translation churn driven by self-modifying code or
    icache-cold paths. `extra` carries `pc=0xPC cflags=0xN`. If V6
    confirms tb_gen_code dominance, follow-on slice is **PPTC**
    (strategy.md Phase 5a — Ryujinx-style persistent profile-guided
    translation cache that survives across `tb_flush`).

  All TCG-side instrumentation is gated on `xemu_spike_log_tcg_enabled`
  so the steady-state cost when off is one global load + branch per
  call site.

Threshold: `XEMU_PERF_SPIKE_LOG_THRESHOLD_US=N` (default 50000 = 50 ms,
floor 1 ms). Lower values catch finer events at the cost of log volume.
The same threshold is used for both renderer and TCG sources.

Typical attribution workflow (V3): run a 300 s Crimson route with
`XEMU_PERF_LOG=1 XEMU_PERF_FRAME_LOG=1 XEMU_PERF_SPIKE_LOG=1
XEMU_PERF_SPIKE_LOG_TCG=1`, locate the worst-frame interval in the
per-interval log (highest `mspf_max`), and grep `xemu-spike:` lines
whose `now_us` falls inside that interval. The op type that fires there
identifies the bottleneck class for the next slice.

`scripts/apple-silicon/extract-perf-summary.sh` derives jitter metrics
from `mspf_max` per interval:
- `fps_stddev`, `mspf_max_p50/p95/p99/max`, `mspf_avg_max`.
- `stutter_intervals_30fps/45fps/60fps` — count of intervals with at
  least one frame > 33.3 / 22.2 / 16.7 ms.
- `longest_stutter_run_30fps/60fps` — longest contiguous run.
Each metric is also emitted with the `post_load_` prefix over the
post-load window (default skip first 5 intervals).

When the run was captured with `XEMU_PERF_FRAME_LOG=1`, the summary
also emits true frame-level metrics derived from the per-frame
`frame_mspf_us=...` field, aggregated across all timed intervals
(`final=1` partial-exit interval excluded, matching the timing
averages):
- `frame_mspf_us_p50`, `frame_mspf_us_p95`, `frame_mspf_us_p99`,
  `frame_mspf_us_p999`, `frame_mspf_us_max` — per-frame mspf
  percentiles in microseconds (nearest-rank).
- `frame_mspf_us_count` — total frame samples aggregated.
- `frame_mspf_us_dropped_total` — sum of `frame_mspf_us_dropped`
  across intervals (frames over the 1024/interval cap).
- `stutter_frames_30fps/45fps/60fps` — count of individual frames
  with mspf > 33.3 / 22.2 / 16.7 ms (33300 / 22200 / 16700 us).
Each frame-level metric is also emitted with the `post_load_` prefix.
Logs without `frame_mspf_us=` produce none of these keys.

`scripts/apple-silicon/sample-profile.sh GAME INPUT_CSV BENCH_SECONDS
[SAMPLE_DURATION] [WARMUP] [LABEL]` is the autonomous helper for
attaching Apple `sample` to a live xemu process. Runs the benchmark in
the background, polls for the run dir / xemu pid, sleeps WARMUP, then
calls `sample` for SAMPLE_DURATION seconds. Output written to
`<run-dir>/sample-<LABEL>.txt` with a thread-bucket summary alongside.

`scripts/apple-silicon/compare-runs.sh BASELINE CANDIDATE [SKIP]` prints
a side-by-side jitter+FPS comparison with verdicts (regression /
improvement / noise). `NOISE_PCT` env var overrides the default 3 %
threshold. Exit code reflects whether the candidate regresses.

- `GEOM_SHADER_MODULE_GEN`: new geometry shader modules compiled.
- `GEOM_SHADER_PROGRAM_GEN`: new linked programs that attach a geometry shader.
- `GEOM_SHADER_BIND` / `GEOM_SHADER_BIND_NOTDIRTY`: geometry-backed shader
  program binds or reused binds.
- `GEOM_SHADER_DRAW`: draws using a geometry-backed shader program.
- `GEOM_SHADER_DRAW_LINE`, `GEOM_SHADER_DRAW_TRI`, `GEOM_SHADER_DRAW_QUAD`,
  `GEOM_SHADER_DRAW_OTHER`: geometry-backed draws grouped by primitive family.
- `GEOM_SHADER_DRAW_QUAD_LIST`, `GEOM_SHADER_DRAW_QUAD_STRIP`: quad-family GS
  draws split into PRIM_TYPE_QUADS (4-vertex independent) and
  PRIM_TYPE_QUAD_STRIP (2-vertex incremental) so the native quad bypass
  coverage can be reasoned about by subtype.
- `NATIVE_TRI_DEPTH_DRAW` / `NATIVE_TRI_DEPTH_FALLBACK`: native triangle-depth
  replacement attempts that drew natively or fell back to geometry shaders.
- `NATIVE_TRI_DEPTH_CANDIDATE`, `NATIVE_TRI_DEPTH_CANDIDATE_SMOOTH`,
  `NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST`, and
  `NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST`: eligibility classification before
  the draw/fallback split. These are the most direct counters for flat XBE
  validation and future native-triangle correctness checks.
- `NATIVE_TRI_DEPTH_DRAW_ZPERSPECTIVE` / `NATIVE_TRI_DEPTH_DRAW_LINEAR_Z`:
  native draws split by depth mode.
- `NATIVE_TRI_DEPTH_DRAW_POLY_OFFSET`: native draws with fill polygon offset
  enabled.
- `NATIVE_TRI_DEPTH_DRAW_SMOOTH` / `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`: native
  draws split by shading/provoking-vertex coverage. Flat-shaded draws are
  allowed on the native path only when NV2A is using first-provoking-vertex
  mode, which matches the OpenGL path's `GL_FIRST_VERTEX_CONVENTION`.
- `NATIVE_TRI_DEPTH_FALLBACK_FLAT` / `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`:
  native-path attempts that stayed on geometry shaders because flat shading is
  still outside the validated native path.
- `NATIVE_QUAD_DRAW` / `NATIVE_QUAD_FALLBACK`: native quad-family bypass
  attempts that drew natively or fell back to the geometry shader.
- `NATIVE_QUAD_DRAW_LIST` / `NATIVE_QUAD_DRAW_STRIP`: native quad draws split
  by primitive subtype (PRIM_TYPE_QUADS vs PRIM_TYPE_QUAD_STRIP).
- `NATIVE_QUAD_CANDIDATE`, `NATIVE_QUAD_CANDIDATE_SMOOTH`,
  `NATIVE_QUAD_CANDIDATE_FLAT`: eligibility classification before the
  draw/fallback split. Smooth-shaded fill quads currently take the native
  path; everything else falls back.
- `NATIVE_QUAD_FALLBACK_FLAT` / `NATIVE_QUAD_FALLBACK_NONFILL`: reason for
  staying on the geometry shader when XEMU_NATIVE_QUAD=1 was set.
- `NATIVE_QUAD_DRAW_ZPERSPECTIVE` / `NATIVE_QUAD_DRAW_LINEAR_Z`: native quad
  draws split by depth mode.
- `NATIVE_QUAD_DRAW_POLY_OFFSET`: native quad draws with fill polygon offset
  enabled.
- `METAL_DRAW_COUNT`: per interval, total successful Metal draw encodes
  (`pgraph_mtl_draw_passthrough` + `pgraph_mtl_draw_indexed`). Zero
  when the GL renderer is active. Apple Silicon performance fork —
  shipped 2026-05-02 with Metal slice M4.
- `METAL_DRAW_INDEXED_COUNT`: subset of `METAL_DRAW_COUNT` that took
  the index-expanded path (triangle_fan / quads / quad_strip /
  polygon / line_loop). Comparing with `METAL_DRAW_COUNT` shows the
  geometry-expansion fraction of the workload — high values are
  expected for any title that emits substantial quad geometry (UI,
  HUD overlays, water surfaces).
- `METAL_NATIVE_TRI_DEPTH_DRAWS`: triangle-family Metal draws that
  ran through the M4 native-depth fragment shader variant. Should
  parallel the existing GL counter `NATIVE_TRI_DEPTH_DRAW` for the
  same workload — the two counters describe the same eligibility
  decision through the same helper functions
  (`pgraph_glsl_native_tri_depth_supported` /
  `pgraph_glsl_native_tri_depth_enabled`). Mismatch indicates either
  a different renderer is active or a counter-plumbing bug. Apple
  Silicon performance fork.
- `METAL_NATIVE_QUAD_DRAWS`: quad-family Metal draws that ran through
  native-depth + index expansion. Parallels GL's `NATIVE_QUAD_DRAW`
  via the shared `pgraph_glsl_native_quad_supported` /
  `pgraph_glsl_native_quad_enabled` helpers. Apple Silicon
  performance fork.
- `METAL_CLEAR_COUNT`: `clear_surface` invocations on the Metal
  renderer (M2 counter, plumbed through the perf interval line in
  M4). Apple Silicon performance fork.
- `METAL_GLSL_TRANSLATE` / `METAL_GLSL_TRANSLATE_FAIL`: per-interval
  GLSL → SPIR-V → MSL translations through
  `pgraph_mtl_glsl_translate_to_msl` (slice M5). The success counter
  ticks once per (vsh, geom, psh) shader stage compiled; the fail
  counter ticks when glslang's preprocess/parse/link OR
  `spirv_cross::CompilerMSL::compile()` rejects the input. Pre-M6
  the M3/M4 hand-coded passthrough still serves draw calls so the
  steady-state value is zero outside of the validation harness;
  steady-state non-zero values land with M6 once the cache is wired
  on the draw path. Apple Silicon performance fork.
- `METAL_SHADER_VALIDATE_OK` / `METAL_SHADER_VALIDATE_FAIL`: M5
  validation-harness fixture pass/fail counts. Increments only when
  `XEMU_METAL_SHADER_VALIDATE` is set (run-validation.sh in
  `scripts/apple-silicon/metal-shader-validation/`); zero on normal
  user runs. Apple Silicon performance fork.
- `METAL_PIPELINE_HITS` / `METAL_PIPELINE_MISSES` /
  `METAL_PIPELINE_FAILED`: M6 per-PgraphMtlPipelineKey LRU cache
  outcomes. Hits = `lru_lookup` matched an existing entry; misses
  = a fresh (vsh-glsl → spirv-cross → MSL → MTLLibrary →
  MTLRenderPipelineState) build occurred and succeeded; failed = a
  build attempt failed at one of those stages and the entry was
  marked `build_failed` so subsequent draws don't retry. Steady-state
  ratio of hits-to-misses gates whether the cache is the right size
  (capacity 2048; tune in `mtl/shadergen.c` if eviction churn is
  visible). The misses + failed totals also pair with
  `METAL_GLSL_TRANSLATE` / `METAL_GLSL_TRANSLATE_FAIL` to attribute
  whether a failure is on the translator side or the
  newRenderPipelineStateWithDescriptor side. Apple Silicon
  performance fork.
- `METAL_TEX_UPLOADS_TOTAL` / `METAL_TEX_UPLOAD_BYTES_TOTAL`: M6
  texture upload counts and total bytes per interval. Each upload
  is a `[blit copyFromBuffer:...:toTexture:...]` from the
  Shared|WriteCombined upload ring (4× 4 MiB) into a Private
  MTLTexture allocated from `heap_textures` (512 MiB). Upload is
  synchronous (commit + waitUntilCompleted) for slice M6; async +
  frame-fence integration ships with M8. Apple Silicon performance
  fork.
- `METAL_TEX_CACHE_HITS` / `METAL_TEX_CACHE_MISSES`: M6 texture
  cache outcomes. The cache is keyed by `(vram_addr, w, h,
  pixel_format)`; on hit no upload is performed and the existing
  Private MTLTexture is rebound to the stage. Steady-state hit rate
  near 1.0 indicates working-set fits the 64-entry FIFO cap; misses
  attributed to texture-state churn. Apple Silicon performance fork.
- `METAL_PIPELINE_KEY_BUILT` (M7): per-flush_draw count of successful
  `pgraph_mtl_build_pipeline_key()` invocations. Each key includes the
  full `ShaderState` (vsh + geom + psh — produced by
  `pgraph_glsl_get_shader_state`), the 9-register pipeline state
  snapshot (NV_PGRAPH_BLEND, BLENDCOLOR, CONTROL_0..3, SETUPRASTER,
  ZOFFSET{BIAS,FACTOR}), and the 16-slot per-attribute MTLVertexFormat
  + per-buffer stride. Steady-state value approaches the per-interval
  `METAL_DRAW_COUNT` once the M7 wiring is exercised. Apple Silicon
  performance fork; slice M7.
- `METAL_PIPELINE_TRANSLATED_OK` / `METAL_PIPELINE_TRANSLATED_FAILED`
  (M7): per-flush_draw cache lookup outcomes for the translated
  pipeline path. Hits + fresh-compiles that succeeded both increment
  `_OK`; only translator/build failures increment `_FAILED`. The
  failure counter pairs with `METAL_GLSL_TRANSLATE_FAIL` and
  `METAL_PIPELINE_FAILED` to attribute fault to the
  state-to-key build vs translator vs pipeline-state build. With
  M7.1 shipped (2026-05-02), every successful lookup is followed by
  an actual translated encode under
  `XEMU_METAL_TRANSLATED_PIPELINE=1`. Apple Silicon performance
  fork; slice M7.
- `METAL_DRAW_TRANSLATED` (M7.1): per-flush_draw count of draws
  encoded through the translated MTLRenderPipelineState (only
  populated when `XEMU_METAL_TRANSLATED_PIPELINE=1` and
  `XEMU_METAL_FORCE_PASSTHROUGH=0`). Should equal `METAL_DRAW_COUNT`
  in steady state when the translated path is healthy. Apple Silicon
  performance fork; slice M7.1.
- `METAL_PIPELINE_FALLBACKS` (M7.1): per-flush_draw count of draws
  that requested the translated path but fell back to M3/M4
  passthrough due to translator/build failure. Pairs with
  `METAL_PIPELINE_TRANSLATED_FAILED` (the lookup outcome counter) to
  attribute fault. Steady-state value should be 0; non-zero means a
  shader state is consistently failing translation. Apple Silicon
  performance fork; slice M7.1.
- `METAL_UNIFORM_PACK` / `METAL_UNIFORM_BYTES` (M7.1): per-flush_draw
  count and total bytes of std140-packed uniform-buffer marshaling
  events. Each `METAL_DRAW_TRANSLATED` advances the pack count by
  exactly 2 (one VSH UBO + one PSH UBO); the bytes counter is the
  sum of packed UBO sizes (typically ~1-4 KiB per VSH +
  ~512 bytes per PSH). Apple Silicon performance fork; slice M7.1.
- `METAL_SHADER_COMPILE_QUEUED_TOTAL` / `METAL_SHADER_COMPILE_COMPLETED_TOTAL`
  / `METAL_SHADER_COMPILE_FAILED_TOTAL` (M8): per-interval count of
  async pipeline-compile dispatches, successful completions, and
  build-failure completions. Steady state after warmup: queued ==
  completed (no in-flight backlog). Cold launch: queued ramps to
  ~50-200 in the first 2-5 s, completed lags by the dispatch
  queue's parallel-compile capacity. Failures should be 0 in
  steady state; non-zero means a shader state is consistently
  failing translation or pipeline build. Apple Silicon performance
  fork; slice M8.
- `METAL_DRAWS_SKIPPED_PENDING_TOTAL` (M8): per-interval count of
  draws that were skipped because the requested translated pipeline
  was still being compiled asynchronously. Only populated when
  `XEMU_METAL_TRANSLATED_PIPELINE=1` and
  `XEMU_METAL_ASYNC_PIPELINE_COMPILE=1` (default on Apple Silicon).
  Cold launch: ramps with the queued counter and tails off as the
  cache warms; steady state should be 0. Pairs with
  `METAL_SHADER_COMPILE_QUEUED_TOTAL` for the cold-launch
  attribution: high queued + high skipped means the warmup window
  is long; high queued + low skipped means async-compile finished
  quickly enough to avoid skip-the-draw artifacts. Apple Silicon
  performance fork; slice M8.
- `METAL_DRAWS_USING_UBERSHADER_TOTAL` (M8): reserved for the
  follow-up M8.1 (Path A — full Dolphin-style hybrid ubershader).
  Always zero today; non-zero indicates the hybrid ubershader is
  serving a draw whose specialized variant is still compiling.
  Apple Silicon performance fork; slice M8 (reserved for M8.1).
- `RATE_SLEW_RATIO_E6` / `RATE_SLEW_ACTIVE` (M10 prerequisite):
  reports the host display's `refresh_rate / 60.0` ratio scaled by
  1,000,000 (so 0.999000 ≈ 999000) and a 0/1 flag indicating whether
  the rate-slewing slice is currently mutating `vblank_interval_ns`.
  `ACTIVE=0` while `XEMU_GL_RATE_SLEW=0` (default), or while the
  ratio is outside `[0.95, 1.05]`, or before the module has been
  initialized. Read by extract-perf-summary.sh; useful for diff'ing
  the slew effect on `frame_mspf_us_p99` between paired runs.
  Apple Silicon performance fork.
- `METAL_PRESENTS` / `METAL_PRESENT_JITTER_US_TOTAL` /
  `METAL_PRESENT_JITTER_US_AVG` / `METAL_PRESENT_JITTER_US_MAX` (M10):
  per-interval count of presented Metal frames, the sum of
  `|drawable.presentedTime - target_seconds|` in microseconds across
  those frames, the average jitter (total / presents), and the
  running max jitter inside the interval (reset after the snapshot
  so the next interval starts fresh). The target_seconds is the
  `presentDrawable:atTime:` deadline computed by the M10 frame-pacing
  path. Lower numbers mean the host display is presenting closer to
  the intended deadline; the `_MAX` field is the headline jitter
  metric for the M10 exit gate. Zero when the GL renderer is active
  or `XEMU_METAL_FORCE_LEGACY_PRESENT=1` plus no presents this
  interval. Apple Silicon performance fork; slice M10.
- `METAL_DRAWABLE_ACQUIRE_FAILS` (M10): per-interval count of
  `[CAMetalLayer nextDrawable]` calls that returned nil (display
  server busy / triple-buffer pool exhausted). Each fail = one
  skipped frame on the Metal path. Zero in steady state; spikes
  during host-system contention. Apple Silicon performance fork;
  slice M10.
- `METAL_DISPLAY_LINK_CALLBACKS` (M10): reserved for the M10.1
  `CAMetalDisplayLink` slice. Always zero on the current M10
  presentDrawable:atTime: path. Apple Silicon performance fork;
  reserved for M10.1.
- `METAL_MSAA_RESOLVE_COUNT` / `METAL_MSAA_RESOLVE_US_TOTAL` /
  `METAL_MSAA_SAMPLE_COUNT` (M11): per-interval count of Metal
  render passes that emitted a multisample resolve, the cumulative
  microsecond cost of those resolves (placeholder fixed nominal
  value per resolve in the M11 v1 implementation; M13's counter-
  sample-buffer integration will replace with GPU-side timing), and
  the latched effective MSAA sample count. The sample-count value
  is constant for the run (MSAA is session-fixed); the resolve
  count ticks once per render pass that ends with
  `MTLStoreActionMultisampleResolve` (so once per `clear_surface`
  with MSAA enabled, plus future per-`flush_draw` resolves once
  draws emit their own MSAA resolves). Headline value for the M11
  exit gate is `METAL_MSAA_RESOLVE_US_TOTAL < 200 µs / frame` once
  GPU-side timing replaces the nominal placeholder. Zero when GL
  is the active renderer or `XEMU_METAL_MSAA=0`. Apple Silicon
  performance fork; slice M11.
- `METAL_VERTEX_US_TOTAL` / `METAL_FRAGMENT_US_TOTAL` (M13):
  per-interval cumulative GPU time spent in the vertex stage and
  fragment stage of the present render pass, in microseconds. Each
  sums (stage_end - stage_start) timestamps resolved from the
  4-sample `MTLCounterSampleBuffer` attached to the present pass
  descriptor, sampled at `MTLCounterSamplingPointAtStageBoundary`.
  Apple Silicon timestamps are nanoseconds; the renderer divides
  by 1000 before accumulating. Headline values: with the present
  pipeline being a single fullscreen-triangle draw + the HUD's
  ImGui draws, expect vertex < 50 µs/frame and fragment a few
  hundred µs/frame on M3+. Counter only tracks the present render
  pass (the only one xemu-metal.mm controls end-to-end); NV2A
  draw passes are not yet instrumented (out of M13 scope; lands in
  a future slice that wires sample buffers into
  `pgraph_mtl_*_draw`). Zero when GL is the active renderer, or
  when the device does not support stage-boundary counter sampling
  (logged once at startup if so). Apple Silicon performance fork;
  slice M13.
- `METAL_PRESENT_GPU_US_TOTAL` / `METAL_PRESENT_GPU_FRAMES` (M13):
  per-interval cumulative `cmdbuf.GPUEndTime - cmdbuf.GPUStartTime`
  in microseconds across the present cmdbuf, plus the count of
  cmdbufs that produced a non-zero delta. Provides an upper bound
  on the present cmdbuf's GPU cost (includes MetalFX scaler pass
  if encoded, present pipeline, and HUD render encoder). Average
  per-frame GPU time = `METAL_PRESENT_GPU_US_TOTAL /
  METAL_PRESENT_GPU_FRAMES`. Comparing this to the per-stage
  vertex+fragment totals reveals fixed-pipeline overhead (cmdbuf
  scheduling, drawable acquisition, etc.). Apple Silicon
  performance fork; slice M13.
- `METAL_FX_SPATIAL_GPU_US_TOTAL` (M13): per-interval cumulative
  GPU time (cmdbuf GPUEndTime − GPUStartTime) for frames where
  `MTLFXSpatialScaler` actually encoded into the cmdbuf. Replaces
  the M12 CPU-side wallclock placeholder
  `METAL_FX_SPATIAL_US_TOTAL` for cost analysis, but as an upper
  bound — the same cmdbuf also runs the present + HUD encoder, so
  this counter is "scaler + present + HUD" rather than scaler
  alone. A scaler-isolated value would require a dedicated
  cmdbuf for the encode, which is out of scope for v1. Apple
  Silicon performance fork; slice M13.
- `METAL_CAPTURE_FRAMES` / `METAL_CAPTURE_ACTIVE` (M13): per-interval
  count of frames seen since `[MTLCaptureManager startCapture...]`
  succeeded, and a latched 0/1 flag indicating whether capture is
  currently active. Both are zero when `XEMU_METAL_CAPTURE` is
  unset. The `_FRAMES` counter ramps until either
  `XEMU_METAL_CAPTURE_FRAMES` is reached (auto-stop in the
  per-frame `addCompletedHandler`) or `xemu_metal_shutdown` is
  called; `_ACTIVE` flips from 1 to 0 at the same moment. Apple
  Silicon performance fork; slice M13.
- `METAL_SHADER_CACHE_LOADS` / `METAL_SHADER_CACHE_HITS` /
  `METAL_SHADER_CACHE_MISSES` (M9): per-interval count of
  persistent MSL-source disk-cache load attempts, hits (cached MSL
  loaded successfully and bypassed spirv-cross), and misses
  (file absent, header mismatch — xemu version / feature-set /
  state-blob length, hash collision, or file-system error).
  `loads = hits + misses` always. Cold launch with a populated
  cache: hits > 0 in the first 2-5 s, misses 0 (or low if a few
  shader states are new this run). Cold launch with an empty cache
  (e.g., first run, deleted directory, or version upgrade with
  mismatch): hits 0, misses ramps with the `METAL_SHADER_COMPILE_
  QUEUED_TOTAL` counter (every miss feeds a fresh translation +
  dispatched build, and every successful build kicks off a save
  that populates the cache for the next run). Steady state: hits
  + misses both zero (no fresh shader state, all served from the
  in-memory LRU cache). Apple Silicon performance fork; slice M9.
- `SHADER_COMPILE_COUNT`: number of GL program compile/link events per
  interval (cold-compile path `generate_shaders()` plus disk-cache
  `pgraph_gl_shader_load_from_memory()`). Cache hits do not count.
- `SHADER_COMPILE_US_TOTAL`: total microseconds spent in the GL program
  compile/link path per interval. On Apple's GL-on-Metal driver this is
  the synchronous GLSL→MSL translation cost the renderer thread blocks on
  inside `glLinkProgram` / `glProgramBinary`. Divide by
  `SHADER_COMPILE_COUNT` for per-event cost. This is the direct
  attribution counter for shader-compile-driven jitter — pair with
  `frame_mspf_us_max` and the longest-stutter-run keys to confirm the
  Crimson/Rainbow worst-frame source. With
  `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`, the timing accumulates on the
  worker thread (clock time), not the renderer thread, so this counter
  no longer maps 1:1 to renderer-thread block time.
- `SHADER_COMPILE_ASYNC_QUEUED`: per interval, number of shader compile
  requests the renderer dispatched to the async worker. Only nonzero
  with `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`.
- `SHADER_COMPILE_ASYNC_COMPLETED`: per interval, number of compile
  requests the worker finished and published. Cumulative
  `QUEUED - COMPLETED` is the live queue depth.
- `SHADER_DRAWS_SKIPPED_PENDING`: per interval, number of `draw_begin /
  draw_end` calls that were skipped because the requested shader
  binding was still being compiled by the async worker. Each skipped
  draw is one frame of pop-in for that geometry. The count should
  drop sharply once the working set of shader variants is warm.
- `MSAA_RESOLVE_US_TOTAL`: total microseconds per interval spent in
  `glBlitFramebuffer` resolving the per-surface multisample renderbuffer
  into the resolved single-sample texture (and the inverse seed-blit
  performed after `pgraph_gl_upload_surface_data` overwrites the
  texture from VRAM). Zero when `XEMU_GL_MSAA` is unset or 0; non-zero
  any time the renderer touches a multisample-backed surface. Use this
  counter together with `SHADER_COMPILE_US_TOTAL` and
  `SHADER_COMPILE_COUNT` when first turning MSAA on to confirm that the
  resolve cost is bounded and that Apple's GL-on-Metal driver does not
  produce a pipeline-variant compile burst under the new multisample
  render-target state (the renderer-side success criterion in
  `decision-log.md` "Stay on OpenGL ...").

TCG hot-path counters (Apple Silicon performance fork). Atomic
increments on the TCG vCPU thread; emitted as additional fields on
the same `xemu-perf:` interval line as the NV2A counters above:

- `TCG_TB_EXEC_COUNT`: translation-block dispatches per interval. Use
  as the denominator when comparing TB-invalidation pressure across
  intervals or builds (e.g. invalidate ratio = `TCG_TB_INVALIDATE_COUNT
  / TCG_TB_EXEC_COUNT`).
- `TCG_TB_INVALIDATE_COUNT`: TBs invalidated per interval (top of
  `do_tb_phys_invalidate` in `accel/tcg/tb-maint.c`). High counts
  during stutter intervals point at SMC / notdirty thrashing rather
  than steady-state translation cost.
- `TCG_NOTDIRTY_TRIPS`: notdirty TLB trips per interval (top of
  `notdirty_write` in `accel/tcg/cputlb.c`). Each trip is a guest
  store that hit a code-bearing page and forces the slow path.
- `TCG_NOTDIRTY_PAGES_HIT`: distinct guest-physical pages observed
  by `notdirty_write` in the interval. Open-addressing 64-entry
  lossy set; saturates at 64 unique pages per interval, which is
  fine for order-of-magnitude attribution. Higher values mean the
  guest is scattering writes across many code pages instead of one
  hot page; lower values with high `TCG_NOTDIRTY_TRIPS` mean
  repeated re-dirtying of a small working set.
- `TCG_TB_INVALIDATE_BURST_MAX`: maximum number of TBs invalidated
  by a single `tb_invalidate_phys_page_range__locked` call in the
  interval. Spikes correlate with whole-page invalidation events
  (e.g. JIT-ed pages getting rewritten). Used to identify whether a
  worst-frame stutter coincides with one large invalidation burst
  versus many small ones.
- `TCG_JMP_CACHE_ZEROED_BUCKETS`: total per-CPU jmp-cache buckets
  cleared per interval (sum across all callers). Each
  `tcg_flush_jmp_cache` call counts `TB_JMP_CACHE_SIZE` (4096); each
  `tb_jmp_cache_inval_tb_targeted` (the I2 fast path) and the
  non-PCREL branch of `tb_jmp_cache_inval_tb` count one per CPU
  inspected. Used by the I2 slice to compute the achieved jmp-cache
  reduction ratio: with the slice on, the value should drop by
  roughly `4096 / (CF_PCREL_invalidations * NCPU)` versus the slice
  off arm.
- `TCG_INVALIDATE_WALL_US_MAX`: per-interval MAX wallclock cost
  (microseconds) of a single `tb_invalidate_phys_page_range__locked`
  invocation, measured via `qemu_clock_get_ns(QEMU_CLOCK_HOST)`.
  Recorded regardless of `XEMU_TCG_JMP_CACHE_TARGETED`. Decisively
  answers "is the worst per-frame stutter one giant invalidation
  chain": if `TCG_INVALIDATE_WALL_US_MAX` approaches the worst-frame
  mspf, the chain is the cause and the I2 slice should drop both
  metrics; if it stays an order of magnitude lower, the worst-frame
  is built from many small invalidations or a non-invalidation
  source.
- `TCG_INVALIDATE_WALL_US_TOTAL` (V10, 2026-05-02): per-interval
  SUM (microseconds) of wallclock cost across all
  `tb_invalidate_phys_page_range__locked` invocations on the vCPU
  thread. Companion to the existing `TCG_INVALIDATE_WALL_US_MAX`
  (per-call max). Always-on; no env gating. Decisive measurement
  for the V9-residual 1.3 s class stutter hypothesis: V9 confirmed
  the worst frame is RDTSC-quiet but `TCG_TB_INVALIDATE_COUNT=8954`
  with 1200 distinct notdirty pages — if average call cost is
  ~100 µs, the SUM = ~900 ms which would account for the bulk of
  the 1.3 s frame. If `TCG_INVALIDATE_WALL_US_TOTAL ≥ 500 ms` in
  the worst-frame interval, the fix is **smarter notdirty handling
  / lazy TB invalidation** (Phase 5a downstream entry — defer
  invalidation until next tb_lookup miss for that page, or use
  byte-range tracking to skip TBs that don't overlap the modified
  bytes). If well below the worst-frame mspf, invalidation is not
  the bottleneck and the cost lives in raw JIT'd guest code (likely
  guest-intrinsic).
- `HELPER_RDTSC_CALLS` (V9, 2026-05-02): per-interval count of
  guest RDTSC instructions executed (atomic; counted by
  `cpu_get_tsc` in `hw/i386/x86-cpu.c`). Always-on, no env
  gating; emitted only when non-zero. Used to validate the
  RDTSC busy-wait hypothesis: if a worst-frame interval shows
  `HELPER_RDTSC_CALLS` 10-100× higher than steady state, the
  guest kernel is busy-waiting on RDTSC deadline-checks (V9
  fast-path target). Steady-state Crimson rate is ~10-50k
  RDTSCs/s; worst-frame rate during a busy-wait stall is
  expected to be in the millions/s.
- `TCG_TB_LOOKUP_US_TOTAL`, `TCG_TB_GEN_CODE_US_TOTAL`,
  `TCG_HANDLE_INTERRUPT_US_TOTAL` (V7, 2026-05-02): per-interval
  sum (microseconds) of wallclock spent inside `tb_lookup`,
  `tb_gen_code`, and `cpu_handle_interrupt` respectively on the
  vCPU thread. Internal accumulation is in nanoseconds (avoids
  sub-µs per-call truncation when thousands of fast calls
  accumulate); emit divides by 1000 to surface microseconds.
  **Gated on `XEMU_TCG_PHASE_LOG=1`** (independent of the
  spike-log threshold). When off, all three counters emit zero
  and the per-call clock-read is skipped (one global load +
  branch per phase per inner-loop iteration). When on, ~36 %
  vCPU overhead worst case at 3M TBs/interval. Decisive
  measurement of cumulative sub-millisecond translation-churn
  cost that V6's per-event 1 ms threshold cannot resolve: if
  `TCG_TB_GEN_CODE_US_TOTAL ≥ 300 ms` in the worst-frame
  interval, PPTC is justified; if `TCG_TB_LOOKUP_US_TOTAL`
  dominates instead, the fix is qht hash-chain investigation;
  if `TCG_HANDLE_INTERRUPT_US_TOTAL` dominates, look at i386
  IRQ injection cost. If none of the three accounts for the
  worst-frame mspf, the cost is in `cpu_loop_exec_tb` (TB
  binary execution) or in another phase not instrumented yet.

APU lock-hold / vCPU-wait counters (audio voice-lock release slice,
opt-in flag `XEMU_APU_LOCK_RELEASE`, on by default for Apple Silicon
system builds) — emitted alongside the TCG / display counters above on
the same `xemu-perf:` interval line. Always-on atomics; only become
visible when `XEMU_PERF_LOG=1`.

- `APU_LOCK_HOLD_US_TOTAL`: per-interval sum (microseconds) of
  wall-clock that the APU worker thread holds `MCPXAPUState::lock`
  during the dispatched VP-frame section in
  `voice_work_dispatch`. Measured around the held portion only; with
  `XEMU_APU_LOCK_RELEASE=1` the released worker-finished wait window
  is excluded from the accumulator, so the counter drops by exactly
  the slice's lock-release window. Decisive measurement of the
  slice's effect on the audio-frame critical section: with the slice
  off this is dominated by the worker-finished wait (~5 ms × 188
  frames/s = ~940 ms/s); with the slice on it should drop to the
  pre/post-wait setup + drain cost (typically <100 ms/s).
- `APU_VCPU_LOCK_WAIT_US_MAX`: per-interval MAX wallclock cost
  (microseconds) of a single vCPU acquire of `MCPXAPUState::lock`,
  recorded via CAS-loop max from `voice_lock()`, `gp_write`, and
  `ep_write`. Decisive measure of whether the slice unblocked vCPU
  contention: with the slice off this approaches the VP-frame period
  (~5 ms) under audio contention because the vCPU has to wait for
  the APU thread's worker batch + DSP + monitor frame to complete;
  with the slice on it should drop to the un-contended acquire cost
  plus the still-held setup/drain sections of the dispatch loop.
  Cross-checks against the D3 `mmio_helper_block` spike attribution
  (which counts BQL acquire + dispatch + return for the
  `mcpx-apu-vp/0xfe8202fc` MMIO write end-to-end) — APU_VCPU_LOCK_WAIT_US_MAX
  is the inner mutex-acquire portion of that.

Display-pacing counters (D3, 2026-05-02) — also emitted as additional
fields on the `xemu-perf:` interval line, alongside the TCG counters
above. Always-on atomics; only become visible when `XEMU_PERF_LOG=1`.
Used to attribute the 30 FPS cap on tracked titles (PGR2 / Rainbow /
Crimson) to either xemu-side pacing, host-side pacing, or
guest-intrinsic engine pacing.

- `NV2A_VBLANK_FIRES`: number of times the xemu vblank-timer thread
  (`ui/xemu.c::vblank_timer_thread`) called `nv2a_vga_gfx_update` and
  set `NV_PCRTC_INTR_0_VBLANK` on the guest. Driven by
  `vblank_interval_ns` (hardcoded 16,666,666 ns = 60 Hz). If this
  reads ~60/s but `NV2A_PRESENT_HEARTBEAT` reads ~30/s, the cap is
  *not* xemu vblank pacing — the guest is choosing not to present
  every vblank.
- `NV2A_FLIP_STALL_WRITES`: guest writes to `NV097_FLIP_STALL`. The
  guest's "I have finished a frame, please present it" signal. Roughly
  equal to `NV2A_PRESENT_HEARTBEAT` in steady state, but lags by one
  frame during stalls.
- `NV2A_PRESENT_HEARTBEAT`: guest writes to
  `NV_PGRAPH_INCREMENT_READ_3D` — the actual READ_3D pointer advance
  that completes a page flip. Same value as
  `g_nv2a_stats.increment_fps` integrated over the interval, but
  expressed as a count rather than a rate.
- `XEMU_GL_SWAPS`: host calls to `SDL_GL_SwapWindow` from
  `ui/xemu.c::gl_render_frame`. Coupled to `NV2A_PRESENT_HEARTBEAT`
  through `pgraph_gl_get_framebuffer_surface`'s
  `qemu_event_wait(&pg->sync_complete)` — the SDL display thread
  blocks until pfifo signals a sync point, so this counter cannot
  exceed the present-heartbeat rate. **Caveat:** the per-interval
  emit is driven by `nv2a_profile_log_emit_interval` which runs from
  `pfifo_thread`, while the swap counter is incremented from the
  separate SDL display thread. The interval emit racing the swap
  increment can read 0 even when swaps are happening at the
  present rate; sum the counter across all intervals (or over a
  longer interval) for a meaningful per-second value. The
  measurement noise floor on this counter is therefore O(1
  swap/interval), useful only as a "is the host loop alive" check.

Decisive ratio for the 30 FPS cap diagnostic:
- `NV2A_VBLANK_FIRES > 30/s` and `NV2A_PRESENT_HEARTBEAT == 30/s`:
  cap is *intrinsic to the guest engine* (xemu offers more vblanks
  than the engine elects to use). Confirmed for Crimson / PGR2 /
  Rainbow on this fork by the D3 sanity-check measurements.
- `NV2A_VBLANK_FIRES == 30/s`: cap is xemu's vblank pacing; raise
  `vblank_interval_ns` (currently hardcoded). Not observed on this
  fork — `NV2A_VBLANK_FIRES` reads ~60/s on every tracked title.

Async shader compile (opt-in flag `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`):

- Spawns a single `pgraph.gl_async_compile` worker thread bound to a
  shared `g_nv2a_context_shader_compile` GL context. The worker pulls
  bindings off `PGRAPHGLState::compile_queue` and runs
  `generate_shaders()` (compile + link + glFinish) without blocking the
  renderer.
- The renderer's `pgraph_gl_bind_shaders` enqueues a compile request
  the first time it sees a new shader state hash, sets
  `r->shader_skip_draw=true`, and the corresponding draw is skipped via
  early-returns in `pgraph_gl_draw_begin` / `pgraph_gl_draw_end`.
  Subsequent bind calls on the same hash also skip until the worker
  publishes `binding->initialized=true`.
- LRU eviction of a binding with `pending_compile=true` would race the
  worker; `shader_cache_entry_post_evict` aborts in that case. The
  shader cache is 50K entries and worst-case queue depth is small, so
  this is documented as a guardrail, not an expected path.
- Trade-off: visible pop-in for the first few frames after a new shader
  appears, in exchange for never blocking the renderer on synchronous
  GLSL→MSL compile. Matches the RPCS3 `Async (Skip Draws)` pattern.
  Aimed at the Crimson Skies / Rainbow Six 3 worst-frame stutter
  documented in the post-fast-read jitter analysis.

Summarize a completed run with:

```sh
scripts/apple-silicon/extract-perf-summary.sh benchmark-runs/20260430-114511-rainbow-six-3
```

The summary reports overall averages, post-load averages with the first five
intervals skipped, geometry-shader counters, native triangle-depth counters,
and common draw-path counters.

When a final `final=1` interval exists, its counters are included in totals but
its partial-exit FPS/frame-time values are excluded from timing averages.

Compare two captured screenshots over a fixed xemu viewport crop with:

```sh
scripts/apple-silicon/compare-screenshots.py \
  benchmark-runs/20260430-115436-rainbow-six-3/screenshots/001-010000ms.png \
  benchmark-runs/20260430-115335-rainbow-six-3/screenshots/001-010001ms.png \
  --crop 641,209,1278,957 \
  --out-dir benchmark-runs/20260430-115436-rainbow-six-3/visual-compare-native-10s
```

The comparison helper saves cropped baseline/candidate images, an amplified
diff image, and prints mean absolute error, RMS error, maximum channel error,
and changed-pixel percentage. Use this only as a visual smoke check; it does
not prove depth correctness.

Run a paired baseline/native snapshot comparison with:

```sh
scripts/apple-silicon/native-tri-depth-compare.sh \
  rainbow benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
  rainbow_scene_b1_nothumb 16
```

The paired runner launches a baseline run with native triangle depth disabled,
then a candidate run with `XEMU_NATIVE_TRI_DEPTH=1`. It writes launcher logs,
perf summaries, metadata, and a cropped screenshot comparison under a
`benchmark-runs/*-native-tri-depth-compare-*` report directory. Override
`XEMU_NATIVE_TRI_DEPTH_COMPARE_SCREENSHOT_INDEX`, screenshot cadence,
`XEMU_NATIVE_TRI_DEPTH_COMPARE_RETRIES`, or pass `none` as the final crop
argument for perf-only comparisons. Retries default to two attempts per side to
absorb the nondeterministic Apple OpenGL texture-upload startup crash seen in
Crimson Skies.

## Game Library Packaging

`scripts/apple-silicon/package-game.sh` packages an extracted Original Xbox
game directory into a XISO ISO that xemu can load. It wraps
[xdvdfs](https://github.com/antangelo/xdvdfs) (`xdvdfs pack`) with
project-aware defaults so Claude can pull a game from the external library
on demand for stress-testing reported xemu issues against this build.

External library default location:

```text
/Volumes/Josh-Backup-Files/Console Games/Original Xbox
```

The library is organized into three letter-range subdirectories
(`XBOX HDD ready (#-I)`, `XBOX HDD ready (J-Q)`, `XBOX HDD ready (R-Z)`)
plus a `DLC/` directory. Each game is an extracted Xbox game tree
(`default.xbe` plus subdirectories), exactly what `xdvdfs pack` consumes.

Usage:

```sh
# List games (optionally filtered)
scripts/apple-silicon/package-game.sh --list
scripts/apple-silicon/package-game.sh --list "rainbow"

# Pack by name (case-insensitive substring match against folder names)
scripts/apple-silicon/package-game.sh "Grooverider - Slot Car Thunder"

# Pack with explicit source / output overrides
scripts/apple-silicon/package-game.sh \
  --source "/path/to/extracted/game" \
  --output Test_Games/some-game.xiso.iso
```

Default output location is `$XEMU_TEST_GAMES_DIR/<game>.xiso.iso` (which
defaults to `/Users/jbbrack03/XEMU_MacOS/Test_Games`). The launcher
`run-benchmark.sh` only knows about its four hardcoded targets
(`crimson`, `rainbow`, `pgr2`, `flat-tri-depth`); for an arbitrary
packaged game, pass the disc path through `XEMU_BENCH_EXTRA_QEMU_ARGS`
or invoke xemu directly with `-dvd_path` until the launcher learns a
`custom` target.

Behavior:

- Refuses to overwrite an existing output ISO unless `--force` is passed.
  This honors workspace rule #9 (do not modify `Test_Games/` in place).
- Writes a `<output>.meta.txt` sidecar with source path, byte size, pack
  duration, xdvdfs version, and a UTC timestamp.
- Verifies the produced ISO with `xdvdfs info` (skip via `--no-verify`).
- Auto-installs xdvdfs via `cargo install xdvdfs-cli --root
  $XEMU_XDVDFS_INSTALL_ROOT` (default `$HOME/.cargo`) when the binary is
  missing. Pass `--no-install` to disable.

Environment variables:

- `XEMU_GAME_LIBRARY` — external library root override.
- `XEMU_TEST_GAMES_DIR` — default output directory override.
- `XEMU_XDVDFS_BIN` — xdvdfs binary path override.
- `XEMU_XDVDFS_INSTALL_ROOT` — `cargo install --root` target for the
  autoinstall path.

Exit codes: `0` success (or already-packed no-op), `1` runtime/IO error,
`2` usage error or ambiguous name match.

The tool reads the external library only; it never modifies it.

## Metal Shader Validation Harness (M5)

Apple Silicon performance fork. The Metal renderer's M5 slice
(GLSL → SPIR-V → MSL translation via spirv-cross) ships with a
built-in validation harness that exercises the translator against a
representative ShaderState fixture set. Per CLAUDE.md rule #1
("no guessing") and metal-renderer-plan.md §6 R1 ("highest-risk
slice"), the harness is the gate: spirv-cross compatibility with
xemu's NV2A-generated GLSL is verified end-to-end before any
draw-path call site is changed.

Invocation:

```sh
scripts/apple-silicon/metal-shader-validation/run-validation.sh
```

The script launches `dist/xemu.app/Contents/MacOS/xemu` with
`XEMU_RENDERER=METAL`, `XEMU_METAL_SHADER_VALIDATE=1`, and
`XEMU_METAL_SHADER_VALIDATE_AND_EXIT=1`. xemu exits immediately after
the harness reports — no machine boot, no GUI interaction. Exit code
is 0 on full pass, 1 on any fixture failure, 2 on infrastructure
failure (binary missing, harness symbol missing, etc.).

Fixtures (`hw/xbox/nv2a/pgraph/mtl/shader_validation.c`):

1. `ff_vsh_minimal` — fixed-function vertex, no lighting.
2. `ff_vsh_lit_textured` — fixed-function vertex with infinite light
   + texture matrix on stage 0.
3. `psh_simple_passthrough` — zero-stage combiner (NV2A reset state).
4. `psh_two_stage_textured` — multi-stage combiner with 2D texture
   samplers.
5. `psh_alpha_test_fog` — fragment shader with alpha-test discard
   and fog blend.
6. `psh_native_tri_depth` — PR #2240 fragment-shader depth-derivation
   path (`XEMU_NATIVE_TRI_DEPTH`).

Each fixture runs the relevant generator (`pgraph_glsl_gen_vsh` /
`gen_psh`) → glslang → spirv-cross MSL → `[device
newLibraryWithSource:options:error:]` and reports per-fixture pass/
fail with the failing GLSL/MSL embedded in the error message. The
geometry-shader fixture is intentionally absent: Apple Silicon has
no native geometry shader, the Metal renderer bypasses GS via
`XEMU_NATIVE_TRI_DEPTH` / `XEMU_NATIVE_QUAD`, and spirv-cross's
GS-emulation path was found to crash the translator on a line-loop
GS payload (silent SIGSEGV inside `spvc_compiler_compile`). The
fixture's absence is documented inline in
`shader_validation.c::build_fixtures`.

Counters: `METAL_SHADER_VALIDATE_OK` / `METAL_SHADER_VALIDATE_FAIL`
on the `xemu-perf:` interval line surface the running totals (see
"Recurring Counters" below).

The harness also fires from `pgraph_mtl_init()` whenever
`XEMU_METAL_SHADER_VALIDATE` is set, so an interactive Metal-
renderer session under that env-var prints the same report on every
machine boot — useful when triaging a production translation
failure.

## Metal Renderer Draw Paths (M5.5, 2026-05-03)

The 2026-05-02 M-cycle close-out shipped the Metal renderer
infrastructure (M0–M14) but left `pgraph_mtl_flush_draw` short-
circuiting the `draw_arrays` / `inline_elements` / `inline_array`
NV2A submission paths and `pgraph_mtl_draw_end` as a no-op. In a
PGR2 paired benchmark this manifested as `METAL_DRAW_COUNT == 0`
across a 180 s run despite the guest pushing 7.5 M `BEGIN_ENDS`. M5.5
closes the gap.

### `mtl/vertex.{c,h}` (new file, ~280 LOC)

CPU-side per-element NV2A vertex-attribute decoder. Reads from VRAM
(`draw_arrays` / `inline_elements`) or `pg->inline_array`
(`inline_array`), produces flat Float4 streams compatible with the
M3/M4 hand-coded passthrough pipeline AND the M7.1 translated
pipeline.

Format coverage: `F` (raw float, 1-4 components), `UB_OGL` /
`UB_D3D` (4 unsigned-byte normalized; `UB_D3D` re-swizzles BGRA →
RGBA), `S1` (1-4 int16 normalized to [-1, 1]), `S32K` (1-4 int16
raw integer). `attr->stride == 0` or `attr->count == 0` falls back
to `attr->inline_value`, mirroring `vk/vertex.c:148/229`.

Public API:

- `pgraph_mtl_collect_vertex_streams(d, source, inline_stride, min, num, pos[], col[])`
  — fill caller-allocated Float4 streams.
- `pgraph_mtl_inline_array_vertex_stride(pg)` — compute inline_array
  per-vertex stride.
- `pgraph_mtl_inline_array_update_offsets(pg)` — publish per-attr
  inline_array_offset.

### `pgraph_mtl_draw_end` now flushes

The M3/M4 era left `pgraph_mtl_draw_end` as `(void)d`. NV2A only
invokes the renderer's `flush_draw` op via the rare ARRAY_ELEMENT
expansion in `pgraph.c::pgraph_expand_draw_arrays`; the actual
per-batch dispatch is driven by `draw_end`. With the no-op,
`flush_draw` was unreachable for ~all real games. M5.5 wires
`pgraph_mtl_draw_end → pgraph_mtl_flush_draw(d)` after the standard
nop-draw guard, mirroring `gl/draw.c:778-814`.

## Metal Renderer Render-Pass Coalescing (M5.7, 2026-05-03)

### `pgraph_mtl_draw_flush_open_pass(void)` (new public API)

The Metal draw module now holds a single `MTLCommandBuffer` +
`MTLRenderCommandEncoder` open across consecutive `flush_draw` calls
when the attachment set is unchanged. `pgraph_mtl_draw_flush_open_pass`
ends the encoder, commits the cmdbuf, and ends the staging-ring
buffer frame. Callers MUST invoke this before any operation that
depends on the surface texture being GPU-stable, including:

- `pgraph_mtl_flip_stall` (NV2A end-of-frame; compositor reads next).
- `pgraph_mtl_clear_surface` (clear opens its own pass with
  `loadAction=Clear`; prior draws must commit first).
- `pgraph_mtl_pre_savevm_trigger` (snapshot capture).
- `pgraph_mtl_pre_shutdown_trigger` (xemu shutdown).
- `pgraph_mtl_surface_flush` (surface-cache flush).
- `pgraph_mtl_draw_finalize` (cleanup before queue release).

All hooks are in `mtl/renderer.c`. The flush is a no-op when no pass
is open.

### Coalescing telemetry (cumulative; surfaced via accessors)

- `pgraph_mtl_draw_pass_opens_count()` — number of fresh render-pass
  starts. Each costs a TBDR tile-load.
- `pgraph_mtl_draw_pass_coalesced_count()` — number of draws that
  reused the existing encoder (the "free" wins).
- `pgraph_mtl_draw_pass_flushes_count()` — number of explicit
  flushes from caller hooks.

Per-interval emission via `extract-perf-summary.sh` is not yet
wired (deferred — the FPS lift is the primary signal). Cumulative
values are accessible programmatically and via debugger.

### Empirical lift (PGR2 60 s scripted gameplay, 2026-05-03)

| Metric | Pre-coalescing | Post-coalescing | Δ |
|---|---|---|---|
| `post_load_avg_fps` | 16.42 | **37.09** | **+125.9 %** |
| `METAL_DRAW_COUNT` | 18.7 k/s | 33.9 k/s | +81 % |
| `METAL_PIPELINE_TRANSLATED_FAILED` (M5.6 follow-up) | 32.8 % | 0 % | full success |

PGR2 Metal now exceeds GL's 30.91 fps baseline by 18 %. Crimson
Skies and Rainbow Six 3 also meet console-native 30 fps on Metal
(`benchmarks/2026-05-03-metal-render-pass-coalescing.md`).

## Run-benchmark.sh extended grace windows (2026-05-03)

`scripts/apple-silicon/run-benchmark.sh::cleanup` previously gave
xemu 3 s for QMP-quit and 5 s for SIGTERM before falling back to
SIGKILL. Both windows are now **15 s**. The 2026-05-02 22:50 GLG
worker crash leaves macOS in a state that slows xemu's atexit
sequence; the original 8 s combined window was tight enough that
the `final=1 reason=atexit` interval line — which flushes
cumulative `FLAT_FIRST` / `FLAT_NONFIRST` counters — never made
it out, breaking `validate-native-tri-depth.sh`. The wider grace
window restored the gate without changing any other harness
behavior. See
`benchmarks/2026-05-03-validate-native-tri-depth-flake.md`.

## Diagnostic Toggles

`XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` keeps triangle-family geometry shaders in
use, but bypasses their depth-plane and slope calculation. This is intentionally
not a correctness path. It is a temporary diagnostic for separating "geometry
shader exists" overhead from "geometry shader does expensive depth math"
overhead. The flag is included in the geometry shader state key, so diagnostic
shader programs do not reuse normal shader-cache entries.

`XEMU_DIAG_SKIP_TRI_GEOM=1` bypasses geometry-shader program generation for
triangle-family fill draws and lets OpenGL draw native triangles directly. This
is intentionally not a correctness path: the fragment shader then receives the
per-vertex depth payload through normal/flat interpolation rather than the
geometry shader's per-triangle payload. It is a temporary diagnostic for
measuring the cost of geometry-shader dispatch and Apple OpenGL driver behavior.
Line primitives remain on the normal geometry-shader path.

`XEMU_NATIVE_TRI_DEPTH=1` also bypasses triangle-family fill geometry
shaders, but changes the fragment shader to derive depth and polygon-slope
offset from native GL rasterization state. This is the completed current opt-in
triangle-family fill replacement path. Like the skip diagnostic, it leaves line
primitives on the normal geometry-shader path. Flat-shaded triangle fills
remain conservative:
first-provoking draws can use the native path, while nonfirst-provoking draws
still fall back to the geometry shader. Set `XEMU_NATIVE_TRI_DEPTH=0` to disable
the path explicitly; that preferred spelling wins even if the compatibility
alias is also set.

`XEMU_DIAG_NATIVE_TRI_DEPTH=1` remains accepted as a compatibility alias for
older benchmark notes, but new runs should prefer `XEMU_NATIVE_TRI_DEPTH=1`.

`XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` enables capped native-triangle state
logging for flat-path debugging. It records shade/provoking method state plus
the live PGRAPH fields and bound shader state at shader bind, draw begin, and
draw flush. Leave it off for timing runs.

## Snapshot Runs

The launcher can create and restore named VM snapshots through the QMP socket
using QEMU's HMP `savevm` and `loadvm` commands.

Create a snapshot after the scripted route reaches the target scene:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_SAVEVM_AT=120 \
XEMU_BENCH_SAVEVM_TAG=crimson_scene_b0 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/crimson-skies-smoke.csv 140
```

Restore that snapshot from the snapshot-bearing scratch HDD:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=crimson_scene_b0 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Snapshot restore is intentionally done after xemu startup through QMP/HMP.
Initial CLI `-loadvm` testing failed with a saved device-tree mismatch around
the emulated USB hub.

Benchmark runs set `XEMU_SNAPSHOT_NO_THUMBNAIL=1` by default. This skips
snapshot thumbnail capture, because one Rainbow Six 3 snapshot launch crashed
Apple's OpenGL-on-Metal worker path while a thumbnail-bearing snapshot was
present on the scratch HDD.

## Current Limitations

- The Crimson Skies route reaches pilot registration, accepts a generated name,
  and advances into the rendered in-engine sequence.
- The Rainbow Six 3 route reaches the default `CHAVEZ` profile, selects
  Campaign, and advances to the Hereford mission loading screen.
- The smoke routes remain useful for setup, and named VM snapshots now provide
  shorter repeatable scene-entry runs.
- The harness drives xemu's controller abstraction, not QEMU's generic
  `send-key` path.
- FPS/frame-pacing extraction is available from `xemu-perf:` log lines.

## Next Automation Steps

1. Use the recorded retail gameplay routes as the main replay targets for the
   next session, starting with PGR2 baseline versus `XEMU_NATIVE_TRI_DEPTH=1`.
2. Use `scripts/apple-silicon/native-tri-depth-compare.sh` for paired
   baseline/native checks only when future changes could affect the completed
   triangle-family fill path.
3. Use `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` only for targeted flat-path
   debugging; the flat XBE counter split is already validated.
4. Keep the smoke routes as boot/profile/menu setup before refreshing snapshots.
5. For the next geometry-shader exit slice, use PGR2 to confirm quad-family
   pressure first. Keep Rainbow Six 3 for line-family coverage and Crimson
   Skies for sustained flight/acceleration cross-checks.
