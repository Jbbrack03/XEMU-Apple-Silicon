# Benchmark Automation

Last updated: 2026-05-02 (V1/V2/V3/V4/V5/D2/D3 multi-slice session: TCG splitwx + jmp-cache-targeted, APU lock-release, MSAA opt-in, 1080p first-launch default, broader title sweep)

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
