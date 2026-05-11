# Benchmark Automation

Last updated: 2026-05-07 (oracle production-ready). The real-Xbox
oracle is ready for production pipeline use. B1-B4 are closed:
controller-roundtrip non-zero state is fixed via kseg0-canonical
controller-buffer writes + CPU writeback/invalidate; the 10-iteration
stress passed; live seqlock passed; and the untested reattach build
was removed. Use `oracle-validate.sh` as the production oracle-side
gate.

Earlier session: Oracle gap-closure session:
**xbe-harness QMP-socket-path fix + agent's atomic anchor rename**.
Three defects shipped in this round —
(1) `xbe-harness` Metal cells were silently FAILING with
"no-screenshot-captured" because the QMP UNIX-socket path inside
`benchmark-runs/m15-gate-<UTC>/04-tier1-matrix/<xbe>/<renderer>/`
exceeded macOS's 104-byte UNIX socket limit; fixed by relocating
the socket to `/tmp/xq-<pid>-<rand>.sock`. Metal-only matrix now
3/3 PASS (controller-roundtrip skipped per new `real_xbox_only`
manifest field).
(2) The oracle agent's anchor write
(`E:\Apps\oracle-agent\state\ctrl-addr.txt`) used a racy
DeleteFileA + MoveFileA two-step (because nxdk's `MoveFileA`
hardcodes `ReplaceIfExists=FALSE`); when the canonical file
couldn't be deleted (FATX cache state, file lock), the rename
silently failed and the on-disk anchor stayed stale → diag XBEs
mapped OLD persistent buffer pages and reported stale state.
Fixed by calling `NtSetInformationFile` directly with
`ReplaceIfExists=TRUE` plus `NtFlushBuffersFile` after the rename
to commit FATX metadata to disk before the kernel image swap
(XLaunchXBE / runxbe) can wipe in-memory state.
(3) `m15-visual-gate.sh` shader-validation grep pattern
("validated.*7/7|all.*PASS") didn't match the actual log line
("summary: 7/7 passed, 0 failed"); fixed.
**New scripts**: `oracle-stress.sh` (10-iteration smoke loop for
the transient agent degraded-state hypothesis), `oracle-seqlock-test.py`
(predicate selftest + concurrent set/get tear check),
`oracle-validate.sh` (5-layer composite oracle production-grade
gate). The previous opt-in `bin-reattach/default.xbe` build was later
removed before production because the anchor-recorded physical page
was not a sufficient allocator-ownership proof.
**New manifest field**: `real_xbox_only: true` (controller-roundtrip)
makes the matrix runner skip non-real-xbox renderers cleanly with
a `skip` status that doesn't count as failure. **New diagnostic**:
`controller-roundtrip` now writes `D:\controller-roundtrip-diag.txt`
with the anchor file content + first 64 bytes of the attached
buffer + the read state values; pulled automatically by
`oracle-orchestrator.py run-diag`'s ftp_collect step. Earlier work:

Last updated: 2026-05-07 (oracle pipeline next-tier tooling SHIPPED:
agent v0.3 with `controller.*` synthetic-input protocol;
`controller-replay.py` Mac-side CSV → agent driver;
`composite-record.sh` ffmpeg AVFoundation NTSC video+audio capture
from MS2109; `extract-keyframes.py` scene-change keyframe
extractor; `audio-waveform.py` audio oracle leg with waveform +
spectrogram + stats. Orchestrator now auto-detects FTP launch
verb so it works under both XBMC4Gamers (`SITE RunXBE`) and
UnleashX (`SITE EXEC`); agent moved to dashboard-independent
`/E/Apps/oracle-agent/`. Earlier 2026-05-06: real-Xbox oracle
Phase 1 + Phase 2 + Phase 3.0 SHIPPED — orchestrator pipeline
validated end-to-end against the project Xbox via the
`pipeline-smoke` Tier-4 diag XBE. Captured framebuffer SHA-256
byte-for-byte matches math-derived expected. Mac-side wrappers
`oracle-client.py` + `oracle-orchestrator.py` are live; agent
ships nine commands (mem/nv2a/vram read+write, screenshot,
runxbe, unsafe.enable, help) on top of Phase 1's
info/eeprom/reboot/bye. See "Real Xbox oracle agent",
"oracle-client.py", "oracle-orchestrator.py", and
"pipeline-smoke" sections below for protocol + commands +
pipeline drive recipe; per-XBE READMEs for usage. Earlier
2026-05-05: Crimson Metal "blocker" reclassified as
config; three harness bugs fixed; F3 snapshot anchor partially
proven; Quartz install documented. Crimson now joins PGR2 / Rainbow /
Halo / boot as MSAA4 PASS canary with canonical recipe
(`XEMU_METAL_FRONT_FB_FALLBACK=1` + the rest). `metal-gl-compare.sh`
now threads the canonical 7-flag M15 Metal recipe + matching
`XEMU_GL_MSAA=4` + geometry-shader bypasses to both legs with
user-env override pattern; `metal-canary-regress.sh`
`last_interval_counter()` skips the `final=1 reason=atexit`
cleanup interval; `macos-capture.sh` adds opt-in
`XEMU_CAPTURE_WINDOW_REQUIRED=1` strict mode + Quartz cache fix +
`find_window_id()` retry-with-backoff. Quartz install for
homebrew Python 3.14: `pip install --user --break-system-packages
pyobjc-framework-Quartz` (PEP 668 escape hatch — verified safe on
M3 Ultra reference machine). F3 snapshot save+load via QMP/HMP
proven autonomous for same-renderer; cross-renderer Metal-saved →
GL-loaded crashes (workaround: save via GL). Earlier 2026-05-04:
W3 counter-mode regression gate operational + W4 unconditional-flush
fix. `metal-canary-regress.sh` supports `--mode {counters,pixels,both}`;
counter mode parses last-interval `xemu-perf:` counters and
validates renderer health autonomously without depending on
pixel-perfect golds. End-to-end PASS verdict on all four canaries
in ~6 minutes. The W4 wrapper in `pgraph_mtl_flush_draw` previously
called `pgraph_mtl_draw_flush_open_pass` unconditionally, defeating
M5.7 coalescing on every benchmark; gated behind a new
`pgraph_mtl_draw_dump_rt_active()` accessor. The previously
banner'd "PGR2/Rainbow drawable-magenta in autonomous shell" was
reclassified as a workflow setup issue (smoke scripts are
placeholders; gold images required interactive profile-HDD +
gameplay-script setup), NOT a renderer regression. Earlier 2026-05-04: F1+F2+W3 baseline-lock
repairs: `XEMU_CAPTURE_AT_FLIP_STALL` +
`XEMU_CAPTURE_FLIP_STALL_SENTINEL` flip-stall capture trigger and
`metal-gl-compare.sh --snapshot / --loadvm-at / --trigger /
--trigger-ordinal` flags (F1), tracked canary gold artifact store
under `docs/apple-silicon/canary-baselines/` with `MANIFEST.tsv`
provenance (F2), three W3 script repairs (positional-arg in
`run-benchmark.sh` consumers, `--resize smaller` for
resolution-mismatched golds, `printf -- '-…'` for bash 3.2). Earlier
2026-05-04: Metal porting workflow rollout: auto-on
Metal validation + post-build M5 gate (W1), paired Metal-vs-GL diff
harness `metal-gl-compare.sh` (W2), single-renderer canary regression
gate `metal-canary-regress.sh` (W3), per-draw color RT dump
`XEMU_METAL_DUMP_DRAW_RT` / `XEMU_GL_DUMP_DRAW_RT` (W4), MoltenVK +
pgraph/vk triangulation backend BLOCKED (W5), Codex-flagged Phase 2
gate fix-ups: window-targeted GL capture, paired-diff HUD-off, doc
flag corrections, range-semantics standardized, Vulkan demoted to
fallback (W6); see `metal-porting-workflow.md` for the canonical
operating playbook. Earlier today: Visual Flight Recorder added and
wired into `run-benchmark.sh` behind `XEMU_BENCH_VISUAL_ANALYSIS=1`;
PGR2 Metal surface/RTT canary is clean; Rainbow Six 3 loading-screen
canary is clean; Xbox boot/flubber canary is clean after the front-face
fix. The earlier green/wireframe report was the boot animation, not
in-game Crimson Skies. Crimson gameplay automation now completes without
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
| Soul Calibur 2 | `scripts/apple-silicon/input-scripts/sc2-gameplay.csv` | `docs/apple-silicon/benchmarks/2026-05-05-sc2-gameplay-route.md` | `benchmark-runs/20260505-163659-soul-calibur-2` |

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
- `XEMU_METAL_VALIDATION={0,1}` (**M14 2026-05-02**; W1 2026-05-04
  also promotes `MTL_SHADER_VALIDATION`) — opt-in Metal API + shader
  validation layer for development. When set, `xemu_metal_init`
  promotes `MTL_DEBUG_LAYER=1` AND `MTL_SHADER_VALIDATION=1` into the
  process environment **before** the first
  `MTLCreateSystemDefaultDevice()` call, which is the only point at
  which Apple's Metal framework reads either env (later `setenv`
  calls have no effect once a device exists). If the user has
  already pinned either env themselves the value is preserved (the
  promotion uses `setenv(..., overwrite=0)`). The shader-validation
  promotion catches a class of shader-side bugs (out-of-bounds buffer
  reads, malformed bindings) that the API-layer `MTL_DEBUG_LAYER`
  cannot see. Surfaced once at startup as
  `xemu-perf: metal_validation requested=R promoted=P
  mtl_debug_layer_active=A mtl_shader_validation_active=A`, where
  `requested` follows `XEMU_METAL_VALIDATION`, `promoted` is 1 only
  if xemu actually wrote `MTL_DEBUG_LAYER`, and the two `_active`
  fields reflect the live env at device-creation time.
  Production / shipped builds leave the variable unset and run with
  validation off; turn it on while debugging missing argument bindings,
  unbalanced retain/release on Metal objects, an unexpected
  `MTLCommandBuffer` status, or a shader-side resource bug. Default 0.
  W1 (2026-05-04) auto-on policy: `scripts/apple-silicon/run-benchmark.sh`
  exports `XEMU_METAL_VALIDATION=1` whenever `XEMU_RENDERER=METAL`
  is set in the launching environment, unless `--metal-no-validate`
  is passed or the user already pinned the env. Renderer code itself
  keeps its default-off opt-in behavior. Apple Silicon performance
  fork; slice M14 + W1. Implementation in `ui/xemu-metal.mm`.
- `XEMU_METAL_HUD={0,1}` (**W1 2026-05-04**) — opt-in for Apple's
  Metal Performance HUD overlay. When set to 1, `xemu_metal_init`
  promotes `MTL_HUD_ENABLED=1` into the process environment **before**
  the first `MTLCreateSystemDefaultDevice()` call (same overwrite=0
  pattern as `XEMU_METAL_VALIDATION`), so an explicit user value
  wins. The Metal HUD is a zero-perf-cost observation tool that
  renders a small overlay with frame time, GPU usage, and memory
  stats. Useful for development; turn it off for clean visual canary
  captures. Surfaced once at startup as
  `xemu-perf: metal_hud requested=R promoted=P mtl_hud_enabled_active=A`,
  mirroring the format of the `metal_validation` line. Default 0.
  W1 auto-on policy: `scripts/apple-silicon/run-benchmark.sh` exports
  `XEMU_METAL_HUD=1` whenever `XEMU_RENDERER=METAL` is set, unless
  `--metal-no-hud` is passed or the user already pinned the env.
  Apple Silicon performance fork; slice W1. Implementation in
  `ui/xemu-metal.mm`.
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

**Window-targeted capture (W6, 2026-05-04).** Set
`XEMU_CAPTURE_WINDOW_PATTERN=xemu` (or any case-insensitive substring
matching the xemu window name or owner name) to make
`scripts/apple-silicon/macos-capture.sh` resolve the on-screen
window-id via Quartz `CGWindowListCopyWindowInfo` at each capture
cycle and run `screencapture -l <wid>` instead of full-desktop
`screencapture -x`. Falls back to full-desktop capture for any cycle
where the matched window is not on-screen (e.g. xemu still loading,
window minimized, or PyObjC's `Quartz` module not available — the
fallback is logged once). Used by W2's `metal-gl-compare.sh` to
bound the GL-leg capture to xemu's drawable region; pair with
`compare-screenshots.py --resize smaller` to absorb residual
retina-vs-drawable scaling. Unset by default; behavior with the env
var unset is identical to the pre-W6 full-desktop path.

**Strict mode (2026-05-05).** Set `XEMU_CAPTURE_WINDOW_REQUIRED=1`
to make the script exit code 3 on Quartz unavailability or window-id
miss instead of falling back to full-desktop. `metal-gl-compare.sh`
sets this by default for the GL leg (user env wins) — without it,
fullscreen fallback compares macOS desktop chrome against the Metal
in-renderer drawable, producing meaningless ~70% changed_pct FAILs
in paired diff. The retry loop in `find_window_id()` (5 attempts,
~0.75s total) absorbs the brief AppKit window-registration race at
xemu cold launch.

**Quartz install for the script's Python.** `macos-capture.sh`
invokes `python3` from PATH. On Apple Silicon Homebrew systems that's
typically `/opt/homebrew/bin/python3` (Python 3.14+). PyObjC's
`Quartz` module must be installed against that interpreter; the
default Apple-bundled `pyobjc-framework-Quartz` only covers
`/usr/bin/python3` (Python 3.9). PEP 668 marks the Homebrew
interpreter as externally-managed, so the user-level install command
needs the explicit escape hatch:

```sh
/opt/homebrew/bin/python3 -m pip install --user \
    --break-system-packages pyobjc-framework-Quartz
```

This installs into `~/Library/Python/3.14/lib/python/site-packages`
without touching the Homebrew Cellar. Verified safe on the project's
M3 Ultra reference machine (2026-05-05). Skip if the install path
ever changes — a venv at `xemu-fork/.venv-quartz/` plus a
`#!/path/to/.venv-quartz/bin/python3` shebang on `macos-capture.sh`
is the alternative if `--break-system-packages` is later disallowed.

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

**Size-mismatch policy (W6, 2026-05-04).** By default
`compare-screenshots.py` exits when `baseline.size != candidate.size`.
Pass `--resize smaller` to make the comparator LANCZOS-resize the
larger image down to the smaller's dimensions before crop and diff;
the original raw sizes are echoed as `raw_baseline_size=` /
`raw_candidate_size=` and `resized={no,smaller}` so callers can
record what was normalized. The W2 paired Metal-vs-GL harness uses
this mode because the GL macOS-screencapture path and the Metal
in-renderer drawable path reliably produce different pixel
dimensions even with window-targeted GL capture (retina vs.
drawable scaling).

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

### Post-build shader-validation gate (W1, 2026-05-04)

`build.sh` runs `metal-shader-validation/run-validation.sh` as a final
post-build step on Apple Silicon (`Darwin && arm64`). A non-zero rc
from the runner aborts the build with the log tail printed to stderr,
so a shipped `dist/xemu.app` always reflects a green M5 fixture pass.
The gate's stdout is teed to `build/shader-validation-postbuild.log`
for inspection.

Pass `--skip-shader-validation` to `build.sh` to bypass the gate for
hot-iteration loops where the harness has already been validated this
session, e.g.:

```sh
./build.sh -a arm64 --skip-shader-validation
```

The gate is no-op on non-Darwin / non-arm64 builds (the runner expects
the macOS xemu.app bundle).

## Auto-on Metal validation in dev runs (W1, 2026-05-04)

`scripts/apple-silicon/run-benchmark.sh` automatically promotes the
Metal validation layer + Performance HUD whenever
`XEMU_RENDERER=METAL` is set in the launching environment. The
philosophy: the cost of a forgotten validation flag in a dev run is
high (silent shader/API misuse hides until the GPU triggers a hard
fault), the cost of an extra log line plus an HUD overlay is zero.

Effective behavior:

- `XEMU_METAL_VALIDATION=1` is exported unless `--metal-no-validate`
  is passed, OR the user already exported a non-empty
  `XEMU_METAL_VALIDATION`.
- `XEMU_METAL_HUD=1` is exported unless `--metal-no-hud` is passed,
  OR the user already exported a non-empty `XEMU_METAL_HUD`.
- xemu's renderer code keeps its default-off opt-in behavior. The
  auto-on lives entirely in the benchmark launcher; running
  `dist/xemu.app/Contents/MacOS/xemu` directly without the launcher
  preserves today's `XEMU_METAL_VALIDATION=0` default.

Effective state is recorded in the run's `metadata.txt` under
`metal_auto_validation` / `metal_auto_hud` (one of `auto-on`,
`auto-off (--metal-no-*)`, `user-pinned (...)`,
`not-applicable (renderer != METAL)`) and `env_XEMU_METAL_VALIDATION`
/ `env_XEMU_METAL_HUD`. The xemu-side startup banner's
`metal_validation` / `metal_hud` lines are surfaced by
`extract-perf-summary.sh` as `metal_validation_requested`,
`metal_validation_promoted`, `mtl_debug_layer_active`,
`mtl_shader_validation_active`, `metal_hud_requested`,
`metal_hud_promoted`, `mtl_hud_enabled_active`.

Opt-out flags:

- `--metal-no-validate` — pass when measuring Metal-renderer perf and
  the validation overhead would skew the measurement.
- `--metal-no-hud` — pass when capturing visual canaries
  (PNG screenshots / Metal-vs-GL diffs) where the HUD overlay would
  pollute the reference image.

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

## Capture-on-flip-stall trigger (F1, 2026-05-04)

`XEMU_CAPTURE_AT_FLIP_STALL=N` and
`XEMU_CAPTURE_FLIP_STALL_SENTINEL=/path` together pin a paired-diff
capture to a specific guest-side event so the GL and Metal legs can
align on the same in-game frame even with cold-launch timing drift.
Both env vars are renderer-agnostic, opt-in, default off; with both
unset the renderer's behavior is identical to the pre-F1 path.

- `XEMU_CAPTURE_AT_FLIP_STALL=N` — `N` is the 1-indexed Nth
  `NV097_FLIP_STALL` since process start. Implementation in
  `util/xemu-display-perf.c:42-187` (atomic counter + armed flag,
  `qatomic_*`-based; CAS-protected one-shot arm) plus
  `hw/xbox/nv2a/pgraph/pgraph.c:1045` (`xemu_capture_at_flip_stall_tick()`
  call site, right after `xemu_pfifo_perf_record_flip_stall_set`).
  When the count equals the target, the trigger arms one-shot; the
  Metal renderer's `end_imgui_frame`
  (`ui/xemu-metal.mm:1525-1539`) consumes the armed flag and one-shots
  the in-renderer screenshot path, bypassing the existing
  `s_screenshot_at_frame` ordinal matching. `0`/unset disables the
  trigger entirely; subsequent flip_stalls past the target stay
  no-op.
- `XEMU_CAPTURE_FLIP_STALL_SENTINEL=/path` — companion filesystem
  sentinel touched once on arm via `O_CREAT | O_EXCL`. The sentinel
  must NOT exist at run start; `EEXIST` is logged and the renderer
  treats the existing file as already-armed. The GL leg's
  `scripts/apple-silicon/macos-capture.sh` (lines ~31-44 env
  passthrough, ~115-160 sentinel-poll branch) polls the path every
  100 ms and one-shots `screencapture` on first appearance.

`metal-gl-compare.sh` activates the F1 trigger via
`--trigger flip` (see the W2 section below). End-to-end one-shot
contract: xemu's vCPU-thread FLIP_STALL handler bumps the counter,
the renderer-side consume call clears the armed flag, and the GL
host-side poller picks up the sentinel touch — both legs capture on
the same guest-side instant.

## Paired Metal-vs-GL Diff Harness (W2, 2026-05-04)

`scripts/apple-silicon/metal-gl-compare.sh` drives two
`run-benchmark.sh` invocations of the same game/input — one with
`XEMU_RENDERER=GL` (baseline) and one with `XEMU_RENDERER=METAL`
(candidate) — captures matched screenshots from each, runs
`compare-screenshots.py` per requested frame, runs `compare-runs.sh` for
the perf-summary delta, and emits `report.md` + `summary.json` with a
PASS/FAIL verdict against a per-pixel-changed threshold. It is the
mechanical enforcement of the M15 visual gate from
`docs/apple-silicon/metal-renderer-plan.md` §4 M15 ("≤ 1 % per-pixel
diff vs GL on the validation title set"); slice W3 wires it into a
canary regression gate.

The GL leg uses the existing `macos`-screencapture backend with
`XEMU_BENCH_SCREENSHOT_INTERVAL=1` so PNGs land at one-second cadence
in `<gl_run>/screenshots/`. **W6 (2026-05-04):** the GL leg also sets
`XEMU_CAPTURE_WINDOW_PATTERN=xemu` so `macos-capture.sh` runs
`screencapture -l <wid>` against the matched xemu window via
Quartz's `CGWindowListCopyWindowInfo` instead of full-desktop
`screencapture -x`; the GL capture is bounded to the same logical
region the Metal in-renderer drawable PNG covers. Falls back to
full-desktop capture for any cycle where the xemu window is not
on-screen. The Metal leg uses `--metal-screenshot <base>` plus
`XEMU_METAL_SCREENSHOT_INTERVAL=60` so the in-renderer post-HUD-
pre-present capture path writes a sequence of `<base>.0001.png` /
`.0002.png` / ... files (byte-identical to the user-visible
drawable, no Screen-Recording dialog, no window occlusion). The
Metal leg also passes `--metal-no-hud` (W6) so the Performance HUD
overlay never bleeds into the captured PNGs versus the GL leg.
Frame ordinals in `--frames N,M,K` index into the sorted sequence
on each side (1-indexed); the default `mid,end` heuristic picks the
middle and last entry of the captured set's intersection.

**W6 size-mismatch normalization.** Even with window-targeted GL
capture, retina vs. drawable scaling can leave the GL PNG at a
different pixel resolution than the Metal PNG. `metal-gl-compare.sh`
computes the auto-derived crop as `0,0,min(W_gl,W_metal),
min(H_gl,H_metal)` and passes `--resize smaller` to
`compare-screenshots.py`, which LANCZOS-resizes the larger image
down to the smaller's dimensions before crop and diff. Each frame's
`raw_baseline_size`, `raw_candidate_size`, and `resized` fields are
captured in `frames.tsv`, the per-frame `compare-stdout.txt`, the
`report.md` table, and the per-frame entries of `summary.json`.
Pass an explicit `--crop x,y,w,h` to the wrapper to override (must
fit inside the smaller-of-two dimensions).

Usage:

```sh
scripts/apple-silicon/metal-gl-compare.sh <game> [--input <csv>]
    [--frames N,M,K] [--crop x,y,w,h] [--threshold pct]
    [--duration seconds] [--out-dir <path>]
    [--snapshot <tag>] [--loadvm-at <sec>]
    [--trigger <flip|frame>] [--trigger-ordinal <N>]
    [--help]
```

F1 alignment flags (added 2026-05-04):

- `--snapshot <tag>` exports `XEMU_BENCH_LOADVM_TAG=<tag>` on both
  legs so the GL and Metal cold-launches load the same snapshot via
  the existing QMP/HMP path in `run-benchmark.sh` (project rule #12
  honored — no CLI `-loadvm`).
- `--loadvm-at <sec>` exports `XEMU_BENCH_LOADVM_AT=<sec>`; default
  `2`. Wall-clock seconds after process start at which the launcher
  issues the QMP `loadvm` command.
- `--trigger <flip|frame>` selects the capture trigger; default
  `frame` (back-compat ordinal-based capture indexed against
  `s_screenshot_at_frame`). `flip` activates F1's path: both legs
  receive `XEMU_CAPTURE_AT_FLIP_STALL=<N>` and
  `XEMU_CAPTURE_FLIP_STALL_SENTINEL=<auto-generated path>`, the
  in-renderer screenshot fires when the Nth FLIP_STALL ticks, and
  the GL leg's `macos-capture.sh` polls the sentinel.
- `--trigger-ordinal <N>` defaults to `30` for `flip` (lets the
  shader cache warm before the captured frame); ignored for `frame`.
  When `--trigger flip` and `--frames` is set with a non-`1` value,
  the script logs `ignoring --frames=… (flip trigger captures one
  frame)` and forces ordinal `1` (post-Codex UX fix at
  `metal-gl-compare.sh` ~line 550).

Worked example — paired PGR2 visual + perf check, default 30 s per
renderer, 1 % changed-pixels threshold on two ordinals:

```sh
scripts/apple-silicon/metal-gl-compare.sh pgr2 --duration 30
```

Worked example — F1 deterministic frame alignment from a saved
snapshot using flip-stall trigger:

```sh
scripts/apple-silicon/metal-gl-compare.sh pgr2 \
    --snapshot pgr2_gameplay_b4 \
    --loadvm-at 2 \
    --trigger flip \
    --trigger-ordinal 30 \
    --duration 30
```

Both legs load the same snapshot 2 s into the run, then capture on
the 30th NV097_FLIP_STALL after process start; the captured frame is
guaranteed to correspond to the same in-guest moment regardless of
shader-compile or scheduler variance between the two cold launches.

Output directory layout (default
`benchmark-runs/<TS>-metal-gl-compare-<game>/`):

```
<out>/
  harness.log                 — script-level log
  gl-launcher.log             — full stdout/stderr of the GL run-benchmark
  metal-launcher.log          — full stdout/stderr of the Metal run-benchmark
  gl/run-dir.txt              — path to <gl_run> directory
  metal/run-dir.txt           — path to <metal_run> directory
  metal/screenshot.NNNN.png   — Metal-rendered drawable PNGs
  diffs/frame-<N>/            — compare-screenshots.py outputs per ordinal
    baseline-crop.png
    candidate-crop.png
    diff-amplified.png
    compare-stdout.txt
  diffs/frames.tsv            — tab-separated machine-readable per-frame stats
  perf-diff.txt               — compare-runs.sh markdown table
  report.md                   — top-level PASS/FAIL report
  summary.json                — machine-readable verdict + per-frame stats
```

Exit codes: `0` PASS (every compared frame ≤ `--threshold`), `1` FAIL
(at least one frame exceeded), `2` infrastructure failure (binary
missing, screenshots missing, sub-script error). The script does NOT
modify `run-benchmark.sh`, `compare-screenshots.py`, or
`compare-runs.sh` — it wraps them. `XEMU_METAL_VALIDATION=1` is
exported on the Metal leg so any Metal-API misuse is logged whether or
not slice W1's auto-on landed.

## Canary regression gate (W3, 2026-05-04; counter mode 2026-05-04 evening)

`scripts/apple-silicon/metal-canary-regress.sh` is the single-renderer
"post-change smoke" tool from the Phase 1 daily loop in
`docs/apple-silicon/metal-porting-workflow.md` §3.5. After every Metal
renderer change it runs the four green Metal canaries (PGR2, Rainbow
Six 3, Halo CE, Xbox boot/flubber) under the established green-canary
env recipe and validates the result via one of two modes.

**Two validation modes (`--mode {counters,pixels,both}`):**

- **`--mode counters` (DEFAULT, autonomous-friendly).** Parses the
  last-interval `xemu-perf:` line of each canary's run and validates
  renderer-health counters against per-canary thresholds:
  `METAL_PIPELINE_TRANSLATED_FAILED == 0` (PSH/VSH translator works);
  `METAL_DRAWABLE_ACQUIRE_FAILS == 0` (CAMetalDrawable available);
  `METAL_PIPELINE_FALLBACKS / METAL_DRAW_COUNT < 0.50` (passthrough
  rare); `METAL_FRONT_FB_PUBLISHES > 0` (publish path active);
  `METAL_DRAW_COUNT > 0`, `fps > 1.0` (basic liveness); when
  `METAL_DRAW_COUNT > 100`: `METAL_DRAW_PASS_COALESCED /
  METAL_DRAW_COUNT > 0.10` (M5.7 coalescing not regressed; W4-style
  unconditional pass-flush would push this to 0.0). End-to-end
  validated PASS verdict on all four canaries in
  `benchmark-runs/20260504-221957-canary-regress` at ~6-minute
  runtime. **This is the default and recommended mode after every
  Metal renderer change.**
- **`--mode pixels` (legacy).** Captures one screenshot per canary at
  the canary's frame ordinal via `XEMU_METAL_SCREENSHOT_PATH` /
  `XEMU_METAL_SCREENSHOT_AT_FRAME` and per-pixel-diffs each shot
  against the stored gold PNG under
  `docs/apple-silicon/canary-baselines/<canary>/<frame>.png` (F2,
  2026-05-04 — promoted from gitignored `benchmark-runs/visual-checks/`).
  Limited utility: gold images were captured INTERACTIVELY with the
  profile HDD + recorded gameplay scripts; the smoke recipe cannot
  reproduce the same game state, so the diff measures setup mismatch
  + frame-ordinal drift rather than renderer regression. Preserved
  for use cases where the operator has manually re-captured stable
  golds (e.g. via interactive snapshot capture at static menu
  states). See decision-log "2026-05-04 evening: W4 unconditional
  pass-flush fix + magenta investigation reclassified" for the full
  empirical analysis.
- **`--mode both`.** Run counters first, then pixels. Both must pass.

Emits `report.md` + `summary.json` with the per-mode tables and
overall PASS/FAIL verdict.

**Manifest provenance (F2).** The companion file
`docs/apple-silicon/canary-baselines/MANIFEST.tsv` carries one
header row plus one data row per canary with the schema:

| Column | Meaning |
| :--- | :--- |
| `canary` | Canary key matching `CANARY_TABLE` (`pgr2` / `rainbow` / `halo` / `boot`). |
| `gold_path` | Path of the gold PNG relative to `canary-baselines/`. |
| `build_commit` | Full SHA of the xemu commit the gold was captured under. |
| `build_date` | YYYY-MM-DD the gold was captured. |
| `xemu_version` | `dist/xemu.app/Contents/MacOS/xemu --version` output at capture time. |
| `gpu_family` | Apple GPU family (`Apple7`/`Apple8`/`Apple9` for M1/M2/M3). |
| `macos_version` | macOS major.minor.patch at capture time. |
| `flags` | Full env recipe used at capture time (flag=value, space-separated). |
| `frame` | Frame ordinal (matches `XEMU_METAL_SCREENSHOT_AT_FRAME`). |
| `source_run` | `benchmark-runs/<TS>-<game>` directory the gold came from. |
| `threshold_pct` | Per-canary changed-pixel-percent threshold for the diff gate. |
| `notes` | Free-text notes (HUD state, MSAA fixes landed, promotion provenance). |

The harness validates the manifest at startup (line ~38 reads the
`MANIFEST_TSV` global; lines ~251-311 contain the validation block):
header column count + every CANARY_TABLE row resolves to a manifest
row + each gold file exists. Failure aborts with INFRA-FAIL `2`.
The validated manifest is then surfaced in two places per run: a
`## Manifest provenance` section in `report.md` and a top-level
`manifest` object in `summary.json`. Every harness artifact therefore
carries its own build/commit/flag provenance for triage.

**Three W3 script repairs landed when the gate first ran end-to-end
(F2 close-out, 2026-05-04).** The four-canary loop exposed three
real bugs that the prior single-canary smokes had not exercised:

1. **Positional-arg bug.** `run-benchmark.sh:200-201` reads positional
   `2` as `INPUT_SCRIPT` unconditionally; Halo and Boot canaries
   previously passed only `<game> <duration>` and got
   `missing required file: 120`. Fixed in `metal-canary-regress.sh`'s
   `CANARY_TABLE`: every row now carries an explicit input path
   (halo→`noop.csv`, boot→`crimson-skies-smoke.csv`); `run_canary`
   uses the 3-positional form so the launcher never has to guess.
2. **`--resize smaller`.** Golds were captured at 1280×931 vs current
   renderer drawable at 1280×960; `compare-screenshots.py` exited 1
   on dimension mismatch. The compare invocation now passes
   `--resize smaller`, mirroring W6's `metal-gl-compare.sh` fix; crop
   is still derived from gold dimensions and LANCZOS resize handles
   the height delta before crop and diff.
3. **`printf -- '-…'` for bash 3.2.** macOS bash 3.2 treats
   `printf '-…'` as starting with an option flag. Fixed in BOTH
   `metal-canary-regress.sh` (lines ~543-550) AND
   `metal-gl-compare.sh` (lines ~769-791) per Codex review.

The script ASSERTS the eight closed default-on Apple Silicon flags
still produce the recorded gold PNG; per project rule #11 it does NOT
re-validate those flags. Failure to assert means a Metal-side
regression on the asserted recipe, not a flag-design question. Unlike
W2's paired diff harness, this is single-renderer: only Metal runs,
the gold PNG is the baseline.

Canary table (hard-coded in the script; source of truth =
`docs/apple-silicon/canary-baselines/MANIFEST.tsv`; Halo and Boot use
explicit `noop.csv` / `crimson-skies-smoke.csv` paths after the W3
positional-arg repair so `run-benchmark.sh:200-201` never mis-reads
the duration as the input file):

| Canary  | Launcher alias | Input script                              | Frame ordinal | Gold PNG (under `docs/apple-silicon/canary-baselines/`)         |
| ------- | -------------- | ----------------------------------------- | ------------- | --------------------------------------------------------------- |
| pgr2    | `pgr2`         | `input-scripts/pgr2-smoke.csv`            | 900           | `pgr2/f900.png`                                                 |
| rainbow | `rainbow`      | `input-scripts/rainbow-six-3-smoke.csv`   | 600           | `rainbow/f600.png`                                              |
| halo    | `halo`         | `input-scripts/noop.csv`                  | 1200          | `halo/f1200.png`                                                |
| boot    | `crimson`      | `input-scripts/crimson-skies-smoke.csv`   | 300           | `boot/f300.png`                                                 |

The `boot` row uses the `crimson` launcher alias because the Xbox
boot+flubber gold was originally captured during a Crimson Skies
launch that hit the boot animation before the title screen; the disc
is irrelevant — frame 300 sits inside the dashboard boot sequence.

Env recipe (verbatim from `handoff.md` "PGR2 PASS" bullet — see the
"Stable opt-in" entries in `xemu-fork/CLAUDE.md` for each flag's full
semantics; this script does not re-document them):

```
XEMU_RENDERER=METAL
XEMU_METAL_TRANSLATED_PIPELINE=1
XEMU_NATIVE_TRI_DEPTH=1
XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1
XEMU_METAL_FRONT_FB_FALLBACK=1
XEMU_METAL_MSAA=4
```

The launcher's `--metal-no-hud` flag is passed so the Metal Performance
HUD overlay never bleeds into the captured screenshot (the gold PNGs
were recorded HUD-off). `XEMU_BENCH_SCREENSHOT_BACKEND=none` disables
the macOS-screencapture cron so the only output PNG is the in-renderer
single-shot capture. The effective HUD/validation state is recorded
in the per-run `report.md` and `summary.json` (`metal_hud=off`,
`metal_validation=auto-on`) so a triage walks the artifact rather
than re-deriving it from the launcher invocation.

Usage:

```sh
scripts/apple-silicon/metal-canary-regress.sh
    [--mode counters|pixels|both]
    [--canary <name>] [--threshold pct]
    [--out-dir <path>] [--help]
```

Worked example — full counter-mode regression smoke (default,
~6-minute runtime, autonomous-friendly):

```sh
scripts/apple-silicon/metal-canary-regress.sh
```

Single-canary smoke after a localized renderer change:

```sh
scripts/apple-silicon/metal-canary-regress.sh --canary pgr2
```

Run BOTH modes (counter mode first, then pixel mode against the
stored golds — useful when the operator has re-captured stable
golds and wants both gates to confirm):

```sh
scripts/apple-silicon/metal-canary-regress.sh --mode both
```

Output directory layout (default
`benchmark-runs/<TS>-canary-regress/`):

```
<out>/
  harness.log                       — script-level log
  results.tsv                       — tab-separated per-canary stats
  report.md                         — top-level PASS/FAIL report
  summary.json                      — machine-readable verdict + per-canary stats
  <canary>/
    launcher.log                    — full stdout/stderr of run-benchmark.sh
    run-dir.txt                     — path to the spawned <run> directory
    screenshot.png                  — Metal-rendered drawable PNG
    diff/
      baseline-crop.png             — gold (cropped to its full image)
      candidate-crop.png            — captured screenshot
      diff-amplified.png            — 8x-amplified per-pixel diff
      compare-stdout.txt            — compare-screenshots.py output
```

Exit codes: `0` PASS (every canary ≤ `--threshold`), `1` FAIL (at
least one canary regressed), `2` INFRA-FAIL (binary missing, gold or
input asset missing, run-benchmark.sh failure, screenshot not
produced, diff infrastructure failure). The script does NOT modify
`run-benchmark.sh`, `compare-screenshots.py`, or `metal-gl-compare.sh`.

Per-canary durations are tuned conservatively so the frame ordinal is
reached even on a cold pipeline cache: 60 s for boot (f300), 75 s for
rainbow (f600), 90 s for pgr2 (f900), 120 s for halo (f1200). Cold-
launch shader compile front-loads the first ~10 s; the headroom
prevents a false INFRA-FAIL when the host is under load.

## Triangulation backend status: BLOCKED — MoltenVK + pgraph/vk

**Status (2026-05-04, slice W5).** Enabling the existing
`hw/xbox/nv2a/pgraph/vk/` Vulkan renderer through MoltenVK as a third
runnable backend on Apple Silicon (alongside GL and Metal) for
triangulating Metal correctness bugs is **BLOCKED** at the device-
feature level. No build / meson / runtime changes were made for this
slice; the Apple Silicon arm64 build remains Metal + GL only.

**Specific blocker.** MoltenVK 1.4.1 (current Homebrew bottle,
`brew --prefix molten-vk` → `/opt/homebrew/Cellar/molten-vk/1.4.1`) on
the M3 Ultra reports the Vulkan core feature `geometryShader = 0`
(false). The xemu Vulkan renderer hard-requires that feature at
`hw/xbox/nv2a/pgraph/vk/instance.c:492` (`F(geometryShader, true)` in
the `desired_features[]` table; `required = true`) and aborts device
creation with `Error: Device does not support required feature
geometryShader` if it is missing.

This was confirmed by direct programmatic probe rather than by
documentation reading. The probe (transient, not committed) linked
against `/opt/homebrew/Cellar/molten-vk/1.4.1/lib/libMoltenVK.dylib`,
called `vkCreateInstance` + `vkGetPhysicalDeviceFeatures` against the
M3 Ultra:

```
Device 0: Apple M3 Ultra (apiVersion=1.2.334)
  geometryShader = 0
  shaderTessellationAndGeometryPointSize = 1
  fillModeNonSolid = 1
  depthClamp = 1
  occlusionQueryPrecise = 1
  shaderClipDistance = 1
```

The root cause is structural: Apple's Metal API has no native
geometry-shader stage, and MoltenVK does not implement geometry-shader
emulation (e.g. via compute shaders or transform-feedback-style passes).
This is the same constraint that drove xemu's native Metal renderer
design to avoid GS entirely (see
`docs/apple-silicon/metal-renderer-plan.md`,
`docs/apple-silicon/emulator-metal-survey.md` §D, and the Metal-side
work that derives per-triangle depth in the fragment shader instead of
in a GS).

**Where xemu vk hard-depends on geometry shaders** (not exhaustive,
established for this slice):

- `pgraph/vk/instance.c:492` — required-feature gate.
- `pgraph/vk/gpuprops.c:62-602` — boot-time GPU-properties
  calibration `render_geom_shader_triangles` runs three GS-driven
  pipelines to detect the host's triangle / triangle-strip /
  triangle-fan rotation winding. Without GS support the renderer
  cannot complete its initialization probe.
- `pgraph/vk/draw.c:750-755` — every primary draw with
  `r->shader_binding->geom.module_info` non-NULL plugs a GS module
  into the pipeline shader-stages array.
- `pgraph/vk/shaders.c:267-282` — `pgraph_glsl_need_geom()` drives
  shader-binding cache key generation; fork PR #2240 work
  (`XEMU_NATIVE_TRI_DEPTH`, `XEMU_NATIVE_QUAD`) bypasses GS in the GL
  path but the vk path still uses GS for non-bypass cases and for the
  flat-shaded quad branch.

Removing GS as a hard requirement would mean rewriting the entire
boot-time gpuprops calibration path, the `glsl/geom.c` shader-
generation path, and the per-draw GS plumbing — multi-week renderer
rework explicitly outside this slice's scope.

**What was NOT done (so the existing build path is unchanged).**

- `meson.build` is unchanged. `vulkan = not_found` on darwin remains.
- `build.sh` is unchanged. No MoltenVK env exports.
- `hw/xbox/nv2a/pgraph/vk/meson.build` is unchanged. The vk
  source-set is still `if vulkan.found()` (i.e. never built on
  darwin).
- No `XEMU_RENDERER=VULKAN` plumbing was added on darwin.
- No source modifications under `hw/xbox/nv2a/pgraph/vk/`.

`./build.sh -a arm64` still produces a Metal + GL build with no vk
renderer, identical to the pre-slice baseline.

**Prerequisite if a future attempt tries again.** `brew install
molten-vk` is the supported install (1.4.1 confirmed bottled). The
ICD JSON lives at
`/opt/homebrew/Cellar/molten-vk/1.4.1/etc/vulkan/icd.d/MoltenVK_icd.json`
and the dylib at
`/opt/homebrew/Cellar/molten-vk/1.4.1/lib/libMoltenVK.dylib`. xemu
should not bundle MoltenVK as a runtime dep — require user install.

**Recommended alternative triangulation paths.**

1. **Per-draw color render-target dump (slice W4 in flight).** A
   parallel slice dumps each draw's bound color RT to disk for both
   the Metal and the GL renderer; running the same scripted-input
   route through both renderers produces directly comparable
   per-draw PNG sequences. This is the highest-value triangulation
   tool that does **not** require a third backend. Reinforce W4's
   importance — it now carries the full triangulation load.
2. **GS emulation in MoltenVK via compute shaders.** Could be
   prototyped upstream in MoltenVK using compute-shader vertex
   transform with output to a buffer, then re-fed as a vertex stream
   for the fragment pass. This is a multi-month MoltenVK upstream
   effort, not a project for this fork.
3. **Wait for GS support in MoltenVK.** As of MoltenVK 1.4.1 there
   is no published roadmap commitment; not a near-term path.
4. **LunarG VulkanSDK on macOS.** Same constraint — VulkanSDK on
   macOS uses MoltenVK underneath. Adds the validation layer and
   tooling but does not add geometry-shader feature support.
5. **Native Vulkan via Vulkan-Direct-on-Metal (KosmicKrisp,
   nicely-private projects).** Same constraint at the Metal layer —
   no native GS stage in Metal.

**Bottom line.** A pgraph/vk + MoltenVK triangulation backend on
Apple Silicon would need an upstream MoltenVK GS-emulation feature
(does not exist) **or** an in-fork rework of the entire vk
renderer's geometry-shader dependency (multi-week, far outside the
diagnostic scope of "triangulation backend"). The W4 per-draw color
RT dump remains the recommended triangulation path. See decision-log
"2026-05-04: MoltenVK W5 BLOCKED" for the full investigation record.

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

## Real Xbox oracle agent (2026-05-06)

The project's OpenXenium-modded retail Xbox is the hardware oracle
referenced in `real-xbox-oracle-feasibility.md`. The architecture
landed today supersedes the original "leverage Microsoft XBDM"
direction; see decision-log "2026-05-06: Real Xbox oracle Phase 1
— custom oracle agent supersedes XBDM" for why.

Three in-tree artifacts:

### `scripts/apple-silicon/xbox-ftp-mirror.py`

Python 3 recursive FTP mirror with SHA-256 manifest. Uses the
stdlib `ftplib` only — no third-party deps. Two modes:

```sh
python3 scripts/apple-silicon/xbox-ftp-mirror.py \
  --mode mirror --remote /C \
  --local /path/to/dest --label C
# writes per-file MANIFEST-C.tsv (path \t size \t sha256), SKIPPED-C.tsv,
# and mirror-C.log under the destination

python3 scripts/apple-silicon/xbox-ftp-mirror.py \
  --mode inventory --remote /F \
  --local /path/to/dest --label F
# walks the subtree without downloading; writes INVENTORY-F.tsv
```

Connection target hardcoded at the top of the script
(`HOST/USER/PASS` constants); change for a different console.
Skips `CACHE/` directories anywhere in the tree (transient game
cache, not interesting for backup). Used 2026-05-06 to capture
the project Xbox's Tier-1 backup (1.5 GB; ~5 min over wired LAN).

### `scripts/apple-silicon/xbe-tests/eeprom-dump/`

One-shot nxdk XBE that captures the 256-byte EEPROM and writes a
decrypted-info file alongside, then reboots. See the directory's
`README.md` for the full deploy-and-run recipe; the short version:

```sh
# (one-time: ensure E:\XBMC4Gamers\Apps\eeprom-dump\ exists, upload XBE)
curl -u xbox:xbox --quote 'CWD /E/XBMC4Gamers/Apps' \
  --quote 'MKD eeprom-dump' ftp://192.168.0.200/ -o /dev/null
curl -u xbox:xbox -T scripts/apple-silicon/xbe-tests/eeprom-dump/bin/default.xbe \
  ftp://192.168.0.200/E/XBMC4Gamers/Apps/eeprom-dump/default.xbe

# launch (XBE writes 3 files, sleeps 5s, reboots; ~30s end-to-end)
curl -u xbox:xbox \
  --quote 'SITE RunXBE Special://xbmc/Apps/eeprom-dump/default.xbe' \
  ftp://192.168.0.200/ -o /dev/null

# pull artifacts after Xbox returns to XBMC4Gamers — to a path OUTSIDE
# the repo. eeprom-fresh.bin / eeprom-info.txt are per-console secrets
# (online key + HDD-key derivation material); they must never end up
# committed. The directory's .gitignore is a backstop, but the right
# default is to pull them straight to your oracle-backup tree:
DEST=/Users/jbbrack03/XEMU_MacOS/xbox-oracle-backup/$(date -u +%F)/eeprom
mkdir -p "$DEST"
curl -u xbox:xbox -o "$DEST/eeprom-fresh.bin" \
  ftp://192.168.0.200/E/XBMC4Gamers/Apps/eeprom-dump/eeprom-fresh.bin
curl -u xbox:xbox -o "$DEST/eeprom-info.txt" \
  ftp://192.168.0.200/E/XBMC4Gamers/Apps/eeprom-dump/eeprom-info.txt
```

The XBE writes to `D:\` because that's the only kernel-auto-mapped
drive letter for a launched XBE; `D:\eeprom-fresh.bin` lands at
`E:\XBMC4Gamers\Apps\eeprom-dump\eeprom-fresh.bin` (FTP-accessible
on the next boot).

### `scripts/apple-silicon/xbe-tests/oracle-agent/`

Persistent nxdk XBE with a TCP listener on port 9001, replacing
the leaked-XDK XBDM path. Source split across
`main.c`/`protocol.{h,c}`/`commands.{h,c}` per project rule on
keeping per-file scope under ~200 lines.

**Phase 1 commands** (always present): `info`, `eeprom`, `reboot`,
`bye`.

**Phase 2 commands** (shipped 2026-05-06):

| Command                              | Behavior                                       |
| ------------------------------------ | ---------------------------------------------- |
| `mem.read addr=0xHEX len=N`          | Binary memory read; 202- BINARY framing        |
| `mem.write addr=0xHEX data=<hex>`    | Memory write; gated by `unsafe.enable`         |
| `nv2a.read off=0xHEX`                | One 32-bit NV2A BAR0 register                  |
| `nv2a.write off=0xHEX val=0xHEX`     | Write 32-bit BAR0 register; gated              |
| `vram.read off=0xHEX len=N`          | Read NV2A 0xF0000000 VRAM aperture             |
| `screenshot`                         | Front-buffer capture; binary `XOSS` header     |
| `runxbe path=<xbox-path>`            | Chainload another XBE; agent terminates        |
| `unsafe.enable`                      | Arm `mem.write` + `nv2a.write` for the session |
| `help`                               | Multi-line list of all commands                |

**Phase 2 + controller.* commands** (agent v0.3, shipped 2026-05-07):

| Command                                                  | Behavior |
| -------------------------------------------------------- | -------- |
| `controller.set port=N [buttons=0xHHHH] [lt=N] [rt=N] [lx=N] [ly=N] [rx=N] [ry=N]` | Update one or more fields on port N (0..3). Missing keys keep current values. Bumps `seq` + `timestamp_us`. |
| `controller.button port=N name=<id> value=<0|1>`        | Edit one button by xemu name (a, b, x, y, dpad_*, back, start, white, black, lstick_btn, rstick_btn, guide). |
| `controller.axis port=N name=<id> value=<int>`          | Edit one axis (ltrigger, rtrigger, lstick_x/y, rstick_x/y). Triggers clamped int16 0..32767 (xemu axis range — XID u8 is `>> 7` at shim/hook layer); sticks clamped int16 -32768..32767. |
| `controller.get [port=N]`                               | Multi-line readback of current state. Omit `port=` to dump all four ports. |
| `controller.clear [port=N]`                             | Zero one or all ports. |
| `controller.buffer-info`                                | Report buffer's virtual address, size, magic (`'XCTR'`=0x58435452), version (1), port count (4), per-port size (24). For future shim/kernel-hook consumers. |

The state lives in agent BSS as a 120-byte `oracle_ctrl_buffer`
(magic + version + reserved + 4 × 26-byte port states; see
`controller.h`). Each port has `buttons` (16-bit OR of
`ORACLE_BTN_*` bits — same VALUES as xemu's
`CONTROLLER_BUTTON_*` masks at `ui/xemu-input.h:41-57`),
`ltrigger` / `rtrigger` (i16 0..32767, xemu axis range — Xbox
XID HID-report u8 0..255 is produced from this by `>> 7` at
`hw/xbox/xid.c:108-109`; the future shim does the same),
`lstick_x/y` + `rstick_x/y` (i16 -32768..32767), `seq`
(monotonic per-port), and `timestamp_us` (xboxkrnl
`KeQueryPerformanceCounter`-derived). **The buffer ABI matches
xemu's `ControllerState.buttons` + `axis[]` byte-for-byte** so
a `XEMU_RECORD_INPUT` CSV replays through the agent without any
value-domain translation: `controller.button name=a value=1`
sets bit 0 (== `CONTROLLER_BUTTON_A`); `controller.axis
name=ltrigger value=32000` stores the int16 value directly.
The buffer is only visible inside the agent process for now;
cross-XBE access (so a chainloaded diag XBE can read what the
agent wrote) is the "Tier 1" work documented in
`docs/apple-silicon/controller-injection-research.md`.

Protocol additions: `202- BINARY <length>` followed by exactly
`<length>` raw bytes (no trailer). Used by `mem.read`, `vram.read`,
`screenshot`. The agent's address allowlist for `mem.read` /
`mem.write` covers RAM only — the four canonical Xbox RAM aliases
(physical low-alias, kseg0, kseg1, write-combined VRAM aperture).
MMIO regions (NV2A BAR0, APU, ACI, USB) are intentionally NOT in
the allowlist; byte-wide memcpy over MMIO produces side effects
that the device side may not tolerate. For NV2A BAR0 use the typed
`nv2a.read` / `nv2a.write` commands which do 32-bit aligned
access. Reads/writes outside the allowlist return `500-`. Lengths
capped at 1 MiB per call; writes capped at 1024 bytes.

The `screenshot` payload is `XOSS` magic + u32 width + u32 height +
u32 stride, followed by `stride * height` raw pixel bytes (the
stride already encodes bytes-per-pixel). Pixel format is the
front-buffer's native layout — typically 32-bit X8R8G8B8
little-endian (`B G R X` in memory). The Mac client converts to
RGBA + PNG.

Quick smoke test (Phase 1 + Phase 2 commands):

```sh
( printf 'info\nbye\n'; sleep 1 ) | nc -w 5 192.168.0.200 9001
python3 scripts/apple-silicon/oracle-client.py info
python3 scripts/apple-silicon/oracle-client.py screenshot --out /tmp/x.png
python3 scripts/apple-silicon/oracle-client.py nv2a-read 0x600800
```

The agent's `eeprom` command output is byte-for-byte identical to
the file written by the eeprom-dump XBE — useful as a self-check
when the agent is first deployed on a new console.

### `scripts/apple-silicon/oracle-client.py`

Mac-side wrapper around the agent protocol. Provides:

- A `OracleClient` Python class for in-process scripting. The
  filename `oracle-client.py` contains a hyphen so it isn't directly
  importable; the orchestrator already handles this via
  `importlib.util.spec_from_file_location(...)`, which is the same
  pattern any in-tree consumer should use:

  ```python
  import importlib.util
  from pathlib import Path
  _spec = importlib.util.spec_from_file_location(
      "oracle_client",
      Path(__file__).resolve().parent / "oracle-client.py",
  )
  oc = importlib.util.module_from_spec(_spec)
  _spec.loader.exec_module(oc)

  with oc.OracleClient("192.168.0.200") as oracle:
      info = oracle.info()
      eeprom = oracle.eeprom()
      val = oracle.nv2a_read(0x600800)              # PCRTC_START
      data = oracle.mem_read(0x80000000, 64)        # kseg0 low RAM
      pixels, w, h, stride = oracle.screenshot()
      rgba = oc.bgrx_to_rgba(pixels, w, h, stride)
      oc.save_screenshot_png(rgba, w, h, "out.png")
  ```

- A CLI for shell driver scripts:

  ```sh
  oracle-client.py info
  oracle-client.py eeprom --out /tmp/eeprom.bin
  oracle-client.py mem-read 0x80000000 64 --out /tmp/low.bin
  oracle-client.py nv2a-read 0x600800
  oracle-client.py vram-read 0x03eb4000 4096 --out /tmp/fb.bin
  oracle-client.py screenshot --out /tmp/agent.png
  oracle-client.py unsafe-enable
  oracle-client.py mem-write 0x80020000 deadbeef
  oracle-client.py runxbe 'C:\xboxdash.xbe'
  oracle-client.py wait-ready --retries 30 --delay 1
  oracle-client.py raw 'help'
  ```

The class's `__exit__` sends `bye` before closing so the agent's
TCP state machine sees an orderly FIN, not a socket-level RST. This
hardening landed 2026-05-06 after a connection-cycle hang surfaced
during initial Phase 2 smoke testing (the agent's lwIP PCB pool
appeared to wedge after ~12 fast connect/RST cycles; the polite-
close path keeps PCBs reusable). Pillow is auto-detected for PNG
encoding; falls back to a stdlib-only zlib+CRC encoder when Pillow
is unavailable.

Defaults read `ORACLE_HOST` / `ORACLE_PORT` from the env (override
with `--host` / `--port`).

### `scripts/apple-silicon/xbe-tests/pipeline-smoke/`

**Tier-4 diag XBE that validates the orchestrator pipeline
end-to-end.** Pixels are CPU-painted directly into the Xbox
front-buffer (no pbkit / no NV2A pgraph), so this XBE doesn't
test renderer behavior — it tests the orchestrator's
chainload-and-back-and-pull plumbing.

What it does at runtime:
1. `XVideoSetMode(640, 480, 32)` and grab the front-buffer via
   `XVideoGetFB()`.
2. Paint every pixel `0xFF000000` (opaque black) except pixel
   `(320, 50)` which is `0xFFFFFFFF` (opaque white). Single-
   pixel deterministic oracle.
3. `XVideoFlushFB()`, sleep 1.5 s for human spot-check.
4. Write the framebuffer to `D:\pipeline-smoke-capture.bin` in
   XOSS format (the same 16-byte header + raw BGRX pixels the
   agent's `screenshot` command emits).
5. Write `D:\pipeline-smoke-done.txt` as a liveness marker.
6. `HalReturnToFirmware(HalRebootRoutine)` — warm-reset back to
   dashboard.

Drive end-to-end via `oracle-orchestrator.py run-diag`:

```sh
python3 scripts/apple-silicon/oracle-orchestrator.py run-diag \
  --xbe   'E:\XBMC4Gamers\Apps\pipeline-smoke\default.xbe' \
  --ftp-collect /E/XBMC4Gamers/Apps/pipeline-smoke \
  --out   benchmark-runs/pipeline-smoke-run
```

The orchestrator pulls the capture .bin via FTP after the
diag XBE reboots back; decode + compare to math-derived expected
via:

```sh
python3 scripts/apple-silicon/xbe-tests/pipeline-smoke/expected.py /tmp/expected.png
# decode the captured .bin to PNG via oracle-client's bgrx_to_rgba
# (see scripts/apple-silicon/xbe-tests/pipeline-smoke/README.md)
```

A clean run produces a captured PNG whose SHA-256 matches
`docs/apple-silicon/xbox-real-references/pipeline-smoke/real-xbox.png`
byte-for-byte.

The Tier-1 NV2A-pipeline diag XBEs (mirror, color-channel,
depth-floor) build on top of this proven plumbing — they swap
the CPU `memcpy` for a pbkit-driven NV2A draw, but reuse the
XOSS-capture-then-reboot lifecycle and the orchestrator's
`run-diag` driver unchanged.

### `scripts/apple-silicon/oracle-orchestrator.py`

Pipeline driver that ties the agent + a diagnostic XBE + FTP
artifact pull together into a single autonomous run. Subcommands:

```sh
# Probe Xbox + FTP + agent liveness; exit 0 if Xbox is responsive.
oracle-orchestrator.py status

# Idempotently launch the agent if it isn't already listening.
oracle-orchestrator.py ensure-agent

# Save a single front-buffer screenshot.
oracle-orchestrator.py capture --out shots/now.png

# Full chainload-and-collect cycle: agent → runxbe → wait FTP back
# → relaunch agent → mirror artifacts → screenshot post-state.
oracle-orchestrator.py run-diag \
    --xbe   'E:\XBMC4Gamers\Apps\diag-mirror\default.xbe' \
    --ftp-collect /E/XBMC4Gamers/Apps/diag-mirror \
    --out   benchmark-runs/oracle-mirror

# Compare a captured PNG against a reference oracle frame.
oracle-orchestrator.py validate \
    --captured  benchmark-runs/.../post.png \
    --reference docs/apple-silicon/xbox-real-references/mirror/00.png
```

Defaults to host `$ORACLE_HOST` (`192.168.0.200`), agent path
`Special://xbmc/Apps/oracle-agent/default.xbe`. The diagnostic XBE
is responsible for capturing whatever it needs to its own `D:\`
directory before rebooting; the orchestrator only knows where to
look (`--ftp-collect`).

`run-diag` writes a `verdict.json` to the output directory with
status, timestamps, and pulled-artifact paths so it can be consumed
by downstream pipeline steps.

### Standard recipe

For any oracle-driven validation session:

1. Boot Xbox, FTP up XBMC4Gamers, optionally `SITE TakeScreenshot`
   to confirm dashboard state.
2. `SITE RunXBE Special://xbmc/Apps/oracle-agent/default.xbe` —
   takes ~5 s for port 9001 to listen.
3. Mac-side client connects, drives the oracle through Phase 2+
   commands (Phase 2 commands not yet shipped — see handoff §"Next
   session priorities").
4. When done: send `reboot` over the agent. ~30 s back to
   XBMC4Gamers.

Cleanup-and-decommission steps live in
`/Users/jbbrack03/XEMU_MacOS/xbox-oracle-backup/2026-05-06/RESTORE.md`
(outside the repo because it documents per-console state including
the EEPROM dump path; do not commit).

## tools/xemu-capture/ — native macOS composite-out capture (2026-05-06)

Mac-side Swift CLI bundled as a code-signed `.app` so macOS TCC
tracks Camera permission by stable bundle ID
`com.xemu-macos.capture`. Survives across Claude sessions and
reboots while the signed app bundle remains unchanged. Drives any
UVC-class video capture device
(verified against the MacroSilicon MS2109 "AV TO USB2.0" stick
attached to the project Xbox's composite-out).

Build:

```sh
cd tools/xemu-capture && make
# Produces dist/xemu-capture.app/Contents/MacOS/xemu-capture
```

CLI invocation:

```sh
scripts/apple-silicon/xemu-capture-app.py <subcommand>

  list                                  JSON list of all video capture devices.
  auth [--request]                      Report/request macOS Camera approval.
  probe DEVICE                          Supported formats / fps for a device.
  inputs DEVICE                         Physical input sources (composite, S-Video).
                                        MS2109 returns empty — its selector is a
                                        UVC vendor extension AVFoundation does
                                        not surface.
  set-input DEVICE INPUT                Pick an active input by id or name.
  snapshot DEVICE --out PATH            Single-frame PNG capture.
                  [--width W --height H]   Force NTSC 720x480 or 640x480 (otherwise
                                          sessionPreset picks PAL 720x576).
                  [--warmup-frames N]      Drop N initial frames (default 5).
                  [--timeout SECONDS]      Max wait for the chosen frame (default 10).
                  [--input INPUT]          Switch active input first.
  sequence DEVICE --out-prefix PFX --count N --interval SECS
                                        Multi-frame sequence; useful for
                                        signal-lock / unplug-replug tests.
  serve [--port N]                      Long-running TCP daemon (default 8889).
                                        Accepts JSON-line ops:
                                        {"op":"snapshot","device":"…","out":"…"}
                                        {"op":"inputs","device":"…"}
                                        {"op":"set-input","device":"…","input":"…"}
                                        {"op":"list"}
                                        {"op":"shutdown"}
  version                               Print version.
```

**Important:** automation must invoke `xemu-capture` through
`scripts/apple-silicon/xemu-capture-app.py` or the installed
`scripts/apple-silicon/bin/xemu-capture` wrapper. Do not invoke
`dist/xemu-capture.app/Contents/MacOS/xemu-capture` or
`.build/release/xemu-capture` directly for camera operations. Direct
binary execution can bypass the LaunchServices app identity that macOS
approved, causing TCC to report `not_determined` while the `.app` is
already authorized.

`scripts/apple-silicon/capture-frame-sequence.py` is the retail oracle's
default capture primitive. It repeatedly calls the LaunchServices app
wrapper for `snapshot`, writes `frame-0001.png ...`, and records
`capture-meta.json`. This keeps every camera-touching operation under
the same macOS-approved app identity. The older ffmpeg MP4 path remains
available via `--capture-backend ffmpeg`, but it is not the default for
retail workflow validation because ffmpeg has its own TCC identity and
can silently hang without a separate Camera grant.

Device-name matching is substring-insensitive against the localized
device name. For the MS2109 capture stick, `"USB2"` is enough.

**Critical knob: format selection.** AVFoundation's default
`sessionPreset = .high` picks the largest-area format the device
advertises (the MS2109 reports both 720×480 NTSC @ 30 fps and
720×576 PAL @ 25 fps; PAL has more pixels). With NTSC composite
input, a PAL decode produces unusable captures. `xemu-capture`
re-pins `device.activeFormat` AFTER `session.startRunning()` to
work around this — pass `--width 720 --height 480` explicitly for
NTSC content.

**Verified MS2109 (vendor `0x534D` / product `0x0021`) format
support:**

| Width | Height | Standard | FPS  | Notes |
|-------|--------|----------|------|-------|
| 160   | 120    | NTSC     | 30   | Quarter |
| 320   | 240    | NTSC     | 30   | Half |
| 640   | 480    | NTSC     | 30   | Square-pixel NTSC |
| 720   | 480    | NTSC     | 30   | Rec.601 NTSC, default for diag XBE captures |
| 720   | 576    | PAL      | 25   | Avoid for NTSC sources |

**Known stability quirk: USB power on the Mac Studio M2 Ultra
front USB-C ports.** The ASMedia 3142 controller backing the front
USB-C ports brown-outs intermittently when the MS2109 is plugged
in via a USB-C adapter. The device enumerates then drops within
2-3 seconds. Workaround: use the back USB-A ports (Apple's
controller) or the rear USB-C (DRD controller `usb-drd4-port-hs`).
The user's working configuration is rear USB-C via USB-A → USB-C
adapter.

**Known cosmetic limitation: NTSC pixel aspect.** Output PNGs are
720×480 with non-square pixels (Rec.601 NTSC). Displayed at
native 1.5:1 aspect in Preview rather than the 4:3 (1.33:1)
aspect a CRT TV would show. For pixel-exact comparison against
diag-XBE expected.py output (which assumes 720×480 square pixels),
this is correct as-is. For human visual review, optional resize to
640×480 square pixels would correct the ratio.

**TCC permission model.** The app is ad-hoc-codesigned (`codesign
-s -`) with `--identifier com.xemu-macos.capture` and includes
`NSCameraUsageDescription` in `Info.plist`. The first invocation
that touches AVFoundation's camera APIs (anything beyond `list`
and `probe`) triggers macOS's "xemu-capture would like to access
the Camera" prompt under System Settings → Privacy & Security →
Camera. Once granted for a specific build of the app, the
permission persists across Claude sessions, Mac reboots, and
project rebuilds **as long as the binary's cdhash doesn't change**.

Per Apple TN3127, ad-hoc signatures use a cdhash-only designated
requirement. Rebuilding the binary changes the cdhash, which can
re-trigger the macOS Camera prompt (TCC sees "different code,
re-confirm"). The checked-in prebuilt
`dist/xemu-capture.app/` keeps Camera permission while unchanged.
If you `make clean && make` to rebuild, expect a one-time regrant
prompt the first time the new binary opens a camera session. Keep the
checked-in `dist/xemu-capture.app/` unchanged during normal oracle runs;
the retail workflow now preflights `auth` and blocks before booting a
game if macOS Camera approval is missing.

For truly stable permission across rebuilds, sign with a real
Developer ID identity instead of ad-hoc (would require a
Developer Program account); not worth the cost for the project's
local-tool use case.

The `--options runtime` (Hardened Runtime) flag is intentionally
NOT used during signing. Per Apple's docs, Hardened Runtime
requires resource-access entitlements (`com.apple.security.device.camera = true`)
to be supplied via a separate entitlements plist; without them,
even after a TCC grant the OS may deny camera access. For an
ad-hoc local tool we use the simpler unhardened ad-hoc path.

## Composite A/V recording — `composite-record.sh` (2026-05-07)

ffmpeg-based capture wrapper around the MS2109 USB stick that
records BOTH video and audio simultaneously into one synchronized
.mp4. The MS2109 advertises a UAC (USB Audio Class) interface
alongside its UVC video device — both labelled "AV TO USB2.0" —
so a single capture stick provides the third oracle leg's
full audiovisual signal.

```sh
./scripts/apple-silicon/composite-record.sh [flags]
```

| Flag | Default | Notes |
| ---- | ------- | ----- |
| `--duration SECONDS`     | 15        | Capture length |
| `--out-dir DIR`          | `benchmark-runs/<UTC-stamp>-composite` | Run directory |
| `--device NAME`          | `USB2`    | Substring match against `ffmpeg -list_devices true` |
| `--audio-device NAME`    | `USB2`    | UAC interface; same MS2109 stick by default |
| `--width N --height N`   | 720 / 480 | NTSC default |
| `--fps N`                | 30        | NTSC field-pair rate |
| `--label NAME`           | (none)    | Tag baked into the run dir name |
| `--no-audio`             | off       | Skip audio capture |
| `--pixel-format FMT`     | uyvy422   | AVFoundation pixel format the device emits |

Outputs under the run directory:
- `video.mp4` — H.264 (`h264_videotoolbox`) + AAC, mp4 container.
- `capture-meta.json` — device names, format, duration, ffmpeg
  argv, observed video duration, exit code.
- `capture-stderr.log` — raw ffmpeg stderr (debug failures).

**Device-resolution note:** ffmpeg's AVFoundation driver ONLY
accepts exact device names or numeric indices (xemu-capture's
substring matcher does not apply). The script parses
`ffmpeg -f avfoundation -list_devices true -i ""` once per run
to map substring → numeric index, then uses the index. Indices
are unstable across plug/unplug, but stable for the duration of
one record session.

**HW-accelerated encode:** `h264_videotoolbox` runs on Apple
Silicon's Media Engine — no CPU encode cost, no battery drain.
Bitrate target is 6 Mbit/s (more than enough for 720×480 @ 30 fps
composite content).

**Stability quirk:** see `tools/xemu-capture/` "Known stability
quirk" — the MS2109 brown-outs flaky USB-C ports on Mac Studio's
ASMedia 3142 controller. Use a back USB-A or DRD-controller front
USB-C if `composite-record.sh` exits with `Input/output error`.

## Keyframe extraction — `extract-keyframes.py` (2026-05-07)

Industry-standard "agent-driven gameplay validation" pipeline
step: capture a video, pick INTERESTING frames (scene cuts +
periodic samples), pixel-diff each against xemu's rendering of
the same scene. Without keyframe selection the diff cost
balloons; with keyframes a 30-second gameplay segment compresses
to ~10-30 PNGs to compare.

```sh
python3 scripts/apple-silicon/extract-keyframes.py VIDEO [flags]
```

| Flag | Default | Notes |
| ---- | ------- | ----- |
| `--out-dir DIR`        | `<video-dir>/keyframes/` | |
| `--threshold T`        | 0.30      | ffmpeg `gt(scene,T)` score in [0..1] |
| `--min-gap-s N`        | 0.5       | Suppress matches closer than N s |
| `--every-s N`          | 0 (off)   | Also emit a frame every N s (timed) |
| `--max-keyframes N`    | 60        | Cap on emitted scene PNGs |
| `--width N --height N` | (none)    | Resize during extraction |
| `--manifest PATH`      | `<out>/manifest.json` | |

Outputs:
- `<out-dir>/scene/0001.png ... NNNN.png` — scene-change keys.
- `<out-dir>/timed/0001.png ... NNNN.png` — fixed-cadence keys
  (only when `--every-s` is set).
- `<out-dir>/scene-timestamps.csv` — `kind,n,time_s,scene_score,png`.
- `<out-dir>/manifest.json` — full run metadata + per-frame entries.

**Architecture note (2026-05-07).** An earlier implementation of
the script chained `gt(t-prev_selected_t,N)` into the `select`
filter so ffmpeg would do min-gap filtering itself. Empirically
that broke the `scene` metric — once a frame is suppressed by
min-gap, the next frame's `scene_score` is computed against the
*previous emitted* frame instead of the prior input frame,
producing systematically wrong scores and missing real cuts. The
fix is to let ffmpeg emit every scene match (one PNG per match
above threshold) and apply min-gap as a Python post-filter,
deleting suppressed PNGs. This costs a few extra ffmpeg encodes
but keeps the scene metric correct. If you ever revisit this
script, do NOT re-introduce a `prev_selected_t` clause.

**showinfo + metadata=print ordering quirk:** with ffmpeg 8.0.1,
the showinfo line emits BEFORE the corresponding
`lavfi.scene_score=<val>` line for each emitted frame. The parser
handles either order by attaching a score to the most recently
appended frame whose score is still `None`.

## Audio waveform oracle — `audio-waveform.py` (2026-05-07)

The audio analog of the agent's `screenshot` capture: takes a
recorded audio track and renders it as visual PNGs that an
LLM agent can pixel-compare. Closes the "Future / scoping idea"
item from 2026-05-06's automation list.

```sh
python3 scripts/apple-silicon/audio-waveform.py INPUT [flags]
```

`INPUT` can be any media file with an audio track (mp4, mkv, wav,
mov...). The script demuxes via ffmpeg, then renders PNGs.

| Flag | Default | Notes |
| ---- | ------- | ----- |
| `--out-dir DIR`         | `<input-dir>/audio-analysis/` | |
| `--width N --height N`  | 1600 / 300 | Render resolution |
| `--silence-db N`        | -50.0 (dBFS) | `silencedetect` threshold |
| `--colors STR`          | `0x4eb3ff\|0x208860` | `showwavespic` color list |

Outputs:
- `audio.wav` — extracted mono 48 kHz s16 PCM.
- `waveform.png` — amplitude-vs-time via `showwavespic`.
- `spectrogram.png` — log-frequency heatmap via `showspectrumpic`
  (mode=combined, win_func=hann, legend enabled).
- `audio-stats.json` — peak / RMS in dBFS via `astats`; silence
  intervals via `silencedetect`; clipping count via direct s16
  WAV scan; `silence_fraction` and per-channel field tables.
- `manifest.json` — top-level summary tying it all together.

**Why visual artifacts.** Agents (LLMs) cannot listen to audio,
but the real-Xbox audio output is empirically authoritative. If
we render the same audio from real Xbox and from xemu as
identical-format PNGs, a per-pixel diff catches missing voices,
audio dropouts, silence regions, clipping, and pitch drift —
all of which produce visible artifacts in either the waveform
or the spectrogram. The third oracle leg's audio twin.

**Validation evidence (2026-05-07).** Run on a 10 s composite
capture of UnleashX dashboard:
- 1.58 s of audio extracted (the dashboard chime).
- Peak −7.84 dBFS, RMS −9.23 dBFS.
- 0 silence intervals, 0 clipping samples.
- 1600×300 waveform PNG + 1884×428 spectrogram PNG generated.

## Controller-input synthesis — `controller-replay.py` (2026-05-07)

Mac-side replay tool that drives the agent's `controller.*` RPCs
from a `XEMU_RECORD_INPUT`-format CSV. The same CSV that runs
xemu via `XEMU_SCRIPTED_INPUT=path.csv` plays through the agent
without translation: the button/axis vocabulary is identical
(see `ui/xemu-input.c:101-127` and the agent's `controller.h`).

```sh
python3 scripts/apple-silicon/controller-replay.py CSV [flags]
```

| Flag | Default | Notes |
| ---- | ------- | ----- |
| `--host H`              | 192.168.0.200 | `$ORACLE_HOST` overrides |
| `--port P`              | 9001      | `$ORACLE_PORT` overrides |
| `--port-index N`        | 0         | Xbox controller port (0..3) |
| `--rate-multiplier M`   | 1.0       | Time-warp the replay |
| `--start-at-ms N`       | 0         | Skip events before this ms mark |
| `--stop-at-ms N`        | end-of-file | Skip events after this ms mark |
| `--dry-run`             | off       | Parse + simulate; no RPC |
| `--clear-on-start`      | off       | Send `controller.clear` first |

Output: per-event status lines on stderr; final JSON summary on
stdout (event count, jitter histogram, RPC errors).

**Important retail-game boundary.** This tool talks to the
oracle agent over TCP/9001. Once the agent `runxbe`s a retail
game, the agent process is gone, so live RPC replay cannot feed
gameplay input. `controller-replay.py` is valid for agent-buffer
testing and Tier-1 diag XBEs only; retail gameplay requires a
proven title-facing backend (resident Tier-2 hook or hardware
controller emulator).

Tier 1 is shipped through the `xbed_input_synth` shared-buffer
shim and validated by `controller-roundtrip`. Tier 2 remains a
separate, higher-risk retail-game hook; see
`docs/apple-silicon/controller-injection-research.md`.

**Validation evidence (2026-05-07).** 8-event smoke CSV
delivered in 360 ms wall against the project Xbox. Mean
jitter 25.7 ms (network RTT to Xbox over LAN). The `seq` and
`timestamp_us` fields on each port advance correctly.

## Retail gameplay oracle — `retail-oracle-workflow.py` (2026-05-10)

The OGX360 bridge is now the production retail-game input backend. Use
the workflow wrapper for normal real-Xbox gameplay captures:

```sh
python3 scripts/apple-silicon/retail-oracle-workflow.py --title crimson
```

The wrapper:

1. validates the hardware path with `ogx360-bridge/validation/bridge-readback-test.py`
   and writes reusable `status=ok` evidence;
2. bootstraps a short hardware-controller IGR proof for dashboard return;
3. launches the selected retail XBE from dashboard FTP (`SITE EXEC`);
4. replays the xemu `XEMU_RECORD_INPUT` CSV through
   `ogx360-bridge/mac-side/controller-replay-hardware.py`;
5. records 720x480 composite reference PNGs through the approved
   `xemu-capture.app` identity;
6. sends the controller IGR route and requires dashboard FTP to return.

Known title defaults are `crimson`, `rainbow`, `pgr2`, and `sc2`.
For production retail-oracle work, the stable title set is currently
`crimson`, `rainbow`, and `pgr2`. `sc2` remains available as a workflow
target, but it is deferred as a production retail gate because its
real-hardware return path is still unstable on the current Xbox image.
Override paths with `--game-xbe` / `--input-csv` when the Xbox HDD
layout differs. Output lands under
`benchmark-runs/retail-oracle-workflow-<title>-<UTC>/` with
`workflow.json`, per-step command logs, bridge evidence, IGR proof,
and the final `gameplay/verdict.json`.

Important timing details:
- Retail workflow invokes hardware replay with `--time-origin zero`, so
  xemu-recorded startup delays are preserved. The legacy hardware-replay
  default remains `--time-origin first-event` for quick bench tests.
- Crimson Skies applies a title default `route_offset_ms=28000` because
  the real Xbox reaches the Crimson title/menu later than xemu. The
  decisive offset run reached title/menu, cutscene/game scene, plane
  frames, IGR return, and final UnleashX dashboard.
- Override with `--route-offset-ms N` for title retuning.

Validation evidence:
- `benchmark-runs/retail-oracle-workflow-crimson-routeoffset-20260510T183546Z/workflow.json`
  reports `status=ok`.
- `benchmark-runs/retail-oracle-workflow-rainbow-20260510T214732Z/workflow.json`
  reports `status=ok`.
- `benchmark-runs/retail-oracle-workflow-pgr2-20260510T231608Z/workflow.json`
  reports `status=ok`.
- `gameplay/verdict.json` reports `verdict=ok`,
  `reference_frame_count=91`, `capture_rc=0`,
  `input_driver_rc=0`, and `dashboard_returned=true`.
- `gameplay/composite/contact-sheet-all-frames.png` visually shows
  dashboard -> Crimson boot/loading -> title/menu -> cutscene/game
  scene/plane frames -> UnleashX/dashboard return.
- Final console state after validation: `ping=true`, `ftp=true`,
  `agent=false`.
- Narrative record:
  `docs/apple-silicon/benchmarks/2026-05-10-retail-oracle-workflow.md`.
- SC2 launched and reached real gameplay on hardware, but repeated
  controller-IGR, XInput-hook, and title-owned patch detours still left
  dashboard FTP unrecovered on the current Xbox image. Treat SC2 as a
  deferred retail-oracle outlier for now, not as a production gate.

## Retail gameplay oracle primitive — `retail-gameplay-oracle.py` (2026-05-07)

Guarded end-to-end wrapper for the real-Xbox retail-game oracle
workflow. It launches a retail XBE, records composite reference frames
by default, drives a title-facing input backend, and waits for
autonomous dashboard recovery. The older ffmpeg backend can still record
composite A/V plus keyframes/audio when explicitly requested.

As of 2026-05-10 the production backend is `hardware`: the Mac drives
the OGX360 bridge over serial while slot 2 presents as a normal Xbox
controller. The older `title-patch` line remains useful research
context for software-only paths, but the workflow above is the default
retail oracle path.

```sh
python3 scripts/apple-silicon/retail-gameplay-oracle.py \
  --game-xbe 'F:\Games\Crimson Skies\default.xbe' \
  --input-csv scripts/apple-silicon/input-scripts/crimson-gameplay.csv \
  --input-backend hardware \
  --input-evidence /path/to/input-evidence.json \
  --exit-evidence /path/to/dashboard-return-evidence.json \
  --launch-backend dashboard-ftp \
  --capture-backend xemu-capture \
  --hardware-device /dev/cu.usbmodem3101
```

The command blocks by default unless both pieces of production
evidence exist:

- `title-facing-input-evidence`: JSON with `status=ok` or
  `verdict=ok`, proving the chosen backend reaches the normal retail
  input path. Agent RPC replay does not qualify after launch.
- `autonomous-exit-evidence`: JSON with `status=ok` or `verdict=ok`,
  proving the chosen backend can return from a running game to the
  dashboard.

Useful flags:

| Flag | Default | Notes |
| ---- | ------- | ----- |
| `--input-backend` | required | `tier2-hook` or `hardware` |
| `--prelaunch-cmd` | none | For title patch staging or future resident-hook preload/setup |
| `--input-driver-cmd` | generated for `hardware` | Shell command that drives the route while the game runs |
| `--hardware-device` / `--hardware-port-index` | autodetect / 1 | OGX360 bridge serial device and slot address |
| `--launch-backend` | `dashboard-ftp` | Use dashboard FTP `SITE EXEC` for retail games; `agent-runxbe` is diagnostic/legacy only |
| `--capture-backend` | `xemu-capture` | Approved-app PNG frame sequence; `ffmpeg` is optional MP4/AAC |
| `--frame-interval-s` | 2.0 | PNG frame cadence for `xemu-capture` capture backend |
| `--route-offset-ms` | 0 | Add title-specific real-hardware timing delay to every route event |
| `--launch-delay-s` | 3.0 | Delay after launch before the driver starts |
| `--record-extra-s` | 15.0 | Recording tail after the route+exit combo |
| `--exit-delay-ms` / `--exit-hold-ms` | 5000 / 6000 | IGR combo timing for hardware-style backends |
| `--exit-attempts` | 2 | Repeat IGR combo if the first attempt is missed |
| `--post-dashboard-screenshot` | off | Optional; relaunches the agent after dashboard return, so workflow callers should normally leave it off |
| `--allow-unproven` | off | Dangerous; only while physically supervising |

Output lands under
`benchmark-runs/retail-gameplay-oracle-<UTC>/`: `verdict.json`,
`route-with-exit.csv`, `composite/capture-meta.json`, and
`composite/frame-*.png`. When `--capture-backend ffmpeg` is used,
it additionally writes `composite/video.mp4`, `keyframes/`, and
`audio/`.

## Retail title scan — `xbe-inspect.py` (2026-05-07)

Read-only XBE metadata and string scanner used to evaluate software-only
retail input strategies without patching a title.

```sh
python3 scripts/apple-silicon/xbe-inspect.py /tmp/xemu-title-scan/halo/default.xbe --strings 40
python3 scripts/apple-silicon/xbe-inspect.py /tmp/xemu-title-scan/soul-calibur-2/Default.xbe --strings 40
python3 scripts/apple-silicon/xbe-inspect.py /tmp/xemu-title-scan/outrun2/default.xbe --strings 40
```

The scanner reports title ID, image bounds, debug path, linked libraries,
kernel thunk metadata, and key strings around XInput/XID/controller/launch
terms. It was added to make the software-only conclusion reproducible:
agent RPC after `runxbe` is gone, LaunchData-only does not drive retail games,
and one generic title-level XInput patch is not stable enough for the full
library. For the fixed canary scope, per-title XBE patching is now the
production path documented in
`docs/apple-silicon/retail-title-patching-strategy.md`.

## Tier-2 shim prior-art analyzer — `tier2-shim-analyze.py` (2026-05-07)

Read-only analyzer for the retail-input kernel shim. It compares the live
annotated Xbox kernel export dump with an NKPatcher source checkout and
reports whether this console matches a known NKPatcher IGR hook recipe.

```sh
python3 scripts/apple-silicon/tier2-shim-analyze.py \
  --out-json benchmark-runs/tier2-shim-analysis-20260507T2310Z/summary.json \
  --out-md benchmark-runs/tier2-shim-analysis-20260507T2310Z/report.md
```

Current result: `verdict=viable-prior-art-match`. The project Xbox matches
NKPatcher `patcher_5838`; the relevant `KeRaiseIrqlToDpcLevel` export-slot VA
is `0x800104e8`, and the expected preinstall slot value is `0x00003d04`.
This makes the NKPatcher-style export-slot hook the primary Tier-2 candidate;
see `docs/apple-silicon/tier2-kernel-shim-viability.md`.

Use `tier2-shim-preflight.py` for the first live check before any future hook
installer is allowed to write:

```sh
python3 scripts/apple-silicon/tier2-shim-preflight.py \
  --analysis benchmark-runs/tier2-shim-analysis-20260507T2310Z/summary.json \
  --out benchmark-runs/tier2-shim-preflight/summary.json
```

It only reads the export slot and requires the expected unhooked value.

## Real Xbox dashboard: UnleashX (switched 2026-05-06)

The project Xbox now boots UnleashX (was XBMC4Gamers until
2026-05-06). Switch was performed by replacing
`/C/evoxdash.xbe` (the file iND-BiOS unconditionally launches at
cold boot) with a chainloader pointing to UnleashX. The previous
XBMC4Gamers chainloader is preserved at `/C/evoxdash.xbe.xbmc.bak`
on the Xbox; full Tier-1 backup at
`/Users/jbbrack03/XEMU_MacOS/xbox-oracle-backup/2026-05-06/`.

**iND-BiOS boot mechanism — empirically determined ON THIS
CONSOLE.** With this console's running BIOS, the boot target
was `/C/evoxdash.xbe` regardless of `/C/ind-bios.cfg` edits.
We verified by setting `DASH1` directly to UnleashX's path on
disk and rebooting; the existing XBMC4Gamers chainloader at
`/C/evoxdash.xbe` still loaded XBMC. The swap that DID take
effect was overwriting the chainloader file itself. Public
iND-BiOS references may describe DASH1/2/3 as cold-boot
priority for other BIOS revisions / config sources; on this
console they appear to behave as IGR (in-game reset)
controller-button-combo alternates instead. Do not generalize
this finding to other consoles without re-verifying.

**FTP command differences from XBMC4Gamers:**

| Operation             | XBMC4Gamers (old)              | UnleashX (current)        |
|-----------------------|--------------------------------|---------------------------|
| Chainload an XBE      | `SITE RunXBE <path>`           | `SITE EXEC <path>`        |
| Welcome banner        | `220-XBMC FileZilla Server …`  | `220 UnleashX FTP Server ready.` |
| Reboot                | `SITE REBOOT`                  | `SITE REBOOT` (same) |

**`oracle-orchestrator.py` now handles the dashboard launch
verb difference.** It auto-detects `SITE EXEC` on the current
UnleashX dashboard and keeps `SITE RunXBE` as the XBMC4Gamers
fallback; `ORACLE_LAUNCH_VERB` can still force a verb for
debugging. The dashboard-independent `ensure-agent` /
`run-diag` path is closed and covered by the oracle
production-readiness gate.

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
6. Real Xbox oracle Phase 2 (2026-05-06): SHIPPED.
   `mem.read` / `mem.write` / `nv2a.read` / `nv2a.write` /
   `vram.read` / `screenshot` / `runxbe` / `unsafe.enable` / `help`
   commands all live in `scripts/apple-silicon/xbe-tests/oracle-agent/`
   (now split into `main.c` + `protocol.{h,c}` + `commands.{h,c}`).
   Mac-side wrappers shipped: `oracle-client.py` (Python API + CLI)
   and `oracle-orchestrator.py` (full diag-XBE chainload pipeline).
   See the "Real Xbox oracle agent" section above for the protocol
   spec, command tables, and CLI examples. Smoke-tested on the
   project Xbox 2026-05-06: `info`, `help`, `eeprom` (SHA-256 byte-
   for-byte match against the file dump), `mem.read`, `nv2a.read`,
   `vram.read`, `screenshot` (640x480 RGBA PNG), and write-gating
   verified — `mem.write` returns `500-` when not armed.
7. Real Xbox oracle Phase 3.0 (2026-05-06): SHIPPED.
   `pipeline-smoke` Tier-4 diag XBE (CPU-painted single-pixel
   oracle) under `scripts/apple-silicon/xbe-tests/pipeline-smoke/`
   proves the orchestrator's `run-diag` chainload-and-back cycle
   end-to-end. Captured framebuffer SHA-256 byte-for-byte matches
   `expected.py:default()`. Two orchestrator bugs fixed in
   flight: (a) artifact pull happened AFTER agent-relaunch but
   the agent suspends XBMC's FTP server — reordered to
   pull-then-relaunch; (b) zero-files-pulled now produces a
   structured `ftp-pull-empty` verdict instead of masking as
   `ok`. Real-Xbox reference stashed at
   `docs/apple-silicon/xbox-real-references/pipeline-smoke/real-xbox.png`.
   See "pipeline-smoke" section above for the drive recipe.
8. Real Xbox oracle Phase 3.1+3.2 (2026-05-06): **SHIPPED.**
   Three Tier-1 NV2A-pipeline diag XBEs landed under
   `scripts/apple-silicon/xbe-tests/`:
   - `mirror/` — pixel-position oracle. Renders a 4x4 white block
     at window (318, 48)-(322, 52) on opaque-black; catches
     Y-mirror bugs.
   - `color-channel/` — RT format and channel-ordering oracle.
     Four full-height vertical strips (red/green/blue/white via
     TYPE_F DIFFUSE); catches B/R swaps in publish path or
     DIFFUSE → COLOR passthrough.
   - `depth-floor/` — depth test + native_tri_depth oracle.
     Full-screen white floor at z=0.5 + blue bottom-half wall
     at z=0.0; with LEQUAL depth test the wall wins where
     drawn (top half white, bottom half blue). Saturated
     0/255 colors only — byte-exact across renderers regardless
     of any display-side gamma table. Catches depth-test /
     depth-write / Y-mirror regressions.
   All three share `xbe-tests/lib/` (xbed_runtime + xbed_capture
   + passthrough vs.vs.cg/ps.ps.cg) so a new diag XBE is ~150
   lines of test-specific code on top. Each XBE is paired with
   `expected.py` (math-derived audit oracle) and `manifest.json`
   (per-(renderer, flag-recipe) expected_results). Rebuild via
   `make` after `eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)"`.

9. Real Xbox oracle production harness (2026-05-06): **SHIPPED.**
   `scripts/apple-silicon/xbe-harness/` — top-level driver that
   runs each Tier-1 XBE on every available renderer (xemu-GL,
   xemu-Metal, real Xbox) and emits a per-cell PASS/FAIL
   matrix. CLI:
   ```sh
   python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py list
   python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py probe
   python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py expected --xbe mirror --out /tmp/m.png
   python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py capture-reference --xbe mirror
   python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run [--xbe ID] [--renderer R] [--out DIR]
   ```
   Renderer drivers: xemu side launches xemu directly with the
   diag iso, scratch HDD (APFS clone), canonical Metal recipe
   (TRANSLATED_PIPELINE=1, FRONT_FB_FALLBACK=1, HUD=0,
   VALIDATION=1) for Metal; macos-capture.sh sidecar for GL.
   Real-Xbox driver wraps `oracle-orchestrator.py run-diag`:
   FTP-uploads the XBE, agent chainloads, FTP-collects XOSS
   blob, decodes to PNG. Comparison gate iterates over all
   captured screenshots (xemu boot + diag-render + post-reboot
   dashboard) and picks the lowest changed_pixels_pct vs the
   reference (math-derived from expected.py or
   real-xbox-canonical from
   `docs/apple-silicon/xbox-real-references/<id>/`); pass if
   `changed_pixels_pct ≤ --max-changed-pct` (default 0.5%).
   See `scripts/apple-silicon/xbe-harness/README.md` for full
   layout, render-loop pattern, and per-XBE add-new-XBE recipe.

10. **Mac-side composite-out capture leg (2026-05-06): SHIPPED.**
    `tools/xemu-capture/` (Swift `.app`, bundle ID
    `com.xemu-macos.capture`, code-signed for stable TCC).
    Drives any UVC capture device; verified against MacroSilicon
    MS2109 USB stick. CLI: `list / probe / inputs / set-input /
    snapshot / sequence / serve`. See "tools/xemu-capture/" section
    above for full usage. The third oracle leg complementing the
    math-derived oracle (xbe-harness) and the agent-side
    framebuffer-capture oracle (oracle-orchestrator).

11. **Real Xbox dashboard switch XBMC4Gamers → UnleashX
    (2026-05-06): SHIPPED.** See "Real Xbox dashboard: UnleashX"
    section above. **Followup CLOSED 2026-05-07.**
    `oracle-orchestrator.py` now auto-detects the FTP launch verb
    by parsing `SITE HELP` (RunXBE on XBMC4Gamers; EXEC on
    UnleashX, the project's current dashboard). Override via
    `$ORACLE_LAUNCH_VERB`. Closes task #13.

12. **Move oracle agent to `/E/Apps/oracle-agent/`.**
    **CLOSED 2026-05-07.** Agent now lives at
    `E:\Apps\oracle-agent\default.xbe`; `DEFAULT_AGENT_PATH` in
    `oracle-orchestrator.py` updated to match. Old XBMC-relative
    path still accepted via `$ORACLE_AGENT_PATH`. Closes task #14.

13. **Oracle agent v0.3 + `controller.*` synthetic-input protocol
    (2026-05-07): SHIPPED.** New file
    `scripts/apple-silicon/xbe-tests/oracle-agent/controller.{h,c}`
    plus updates to `commands.h` / `main.c` / `Makefile`. RPCs:
    `controller.set port=N [buttons=…] [lt=…] [rt=…] [lx=…]
    [ly=…] [rx=…] [ry=…]`,
    `controller.button port=N name=<id> value=<0|1>`,
    `controller.axis port=N name=<id> value=<int>`,
    `controller.get [port=N]`, `controller.clear [port=N]`,
    `controller.buffer-info`. Backed by `oracle_ctrl_buffer`
    (magic=`'XCTR'`=0x58435452, version=1, 4 ports of 26 bytes
    each = 120 total; per-port size grew from 24 to 26 bytes
    after the 2026-05-07 Codex review fixed triggers to int16).
    Button/axis vocabulary matches
    `ui/xemu-input.c:101-127` verbatim so a `XEMU_RECORD_INPUT`
    CSV replays through the agent without translation.
    `oracle_ctrl_init()` runs at agent boot. End-to-end validated
    against the project Xbox: every command + help + smoke-tested
    via `oracle-client.py raw`. Tier 1 later moved this buffer into
    persistent kernel memory and shipped the `xbed_input_synth`
    diag-XBE shim; Tier 2 is the remaining retail-game hook path.
    See `controller-injection-research.md` and
    `tier2-kernel-shim-viability.md`.

14. **`scripts/apple-silicon/controller-replay.py` (2026-05-07):
    SHIPPED.** Replays a `XEMU_RECORD_INPUT` CSV through the
    agent's `controller.*` RPCs at the original wall-clock cadence.
    CLI: `controller-replay.py CSV [--host H] [--port P]
    [--port-index N] [--rate-multiplier M] [--start-at-ms N]
    [--stop-at-ms N] [--dry-run] [--clear-on-start]`. Reports
    per-event jitter (mean/median/max/p95). End-to-end validated:
    8 events delivered in 360 ms with 25.7 ms mean jitter (network
    RTT to Xbox over LAN). Pairs with the existing
    `scripts/apple-silicon/input-scripts/{pgr2,sc2,rainbow,crimson}-*.csv`
    library so the same gameplay routes drive both engines.

15. **`scripts/apple-silicon/composite-record.sh` (2026-05-07):
    SHIPPED.** ffmpeg-based AVFoundation capture wrapper for the
    MS2109 USB composite stick. Pins NTSC 720x480 @ 30 fps + 48
    kHz stereo audio (the MS2109's UAC interface — same physical
    stick provides both legs). Resolves substring device names →
    numeric indices via `ffmpeg -list_devices true` (ffmpeg's
    AVFoundation driver requires exact name or index, not the
    substring matcher xemu-capture uses). Output:
    `<benchmark-runs>/<UTC-stamp>-composite-<label>/{video.mp4,
    capture-meta.json, capture-stderr.log}` — H.264 video
    (`h264_videotoolbox`) + AAC audio. CLI flags:
    `--duration / --out-dir / --device / --audio-device / --width
    / --height / --fps / --label / --no-audio / --pixel-format`.
    End-to-end validated: 10 s capture of UnleashX dashboard via
    composite cable produced 5.18 MB H.264 + AAC mp4
    (`width=720 height=480 codec=h264`).

16. **`scripts/apple-silicon/extract-keyframes.py` (2026-05-07):
    SHIPPED.** ffmpeg-based scene-change keyframe extractor +
    optional fixed-cadence time-driven extractor. Implements the
    standard agent-driven game-development workflow: capture
    gameplay → extract interesting frames → diff against xemu's
    rendering of the same scene. CLI: `extract-keyframes.py VIDEO
    [--out-dir DIR] [--threshold T] [--min-gap-s N] [--every-s N]
    [--max-keyframes N] [--width N --height N] [--manifest PATH]`.
    Output: `<run>/keyframes/{scene/0001.png ...,
    timed/0001.png ..., scene-timestamps.csv, manifest.json}`.
    **Architecture note**: an earlier draft chained
    `gt(t-prev_selected_t,N)` into the select expression so ffmpeg
    would do min-gap filtering itself. Empirically that broke the
    `scene` metric — once a frame is suppressed by min-gap, the
    next frame's scene_score is computed against the *previous
    emitted* frame instead of the prior input frame, producing
    systematically wrong scores and missing real cuts. The fix
    is to let ffmpeg emit every scene match (one PNG each) and
    apply min-gap as a Python post-filter, deleting suppressed
    PNGs. End-to-end validated on the 10 s composite capture: 1
    scene match + 5 timed (every 2 s) keyframes.

17. **`scripts/apple-silicon/audio-waveform.py` (2026-05-07):
    SHIPPED.** Audio oracle leg — closes the "Future / scoping
    idea" item from 2026-05-06. Demuxes mono 48 kHz s16 PCM via
    ffmpeg, then renders (a) `waveform.png` via ffmpeg's
    `showwavespic` filter, (b) `spectrogram.png` via
    `showspectrumpic` (log-frequency, hann window, legend
    enabled), (c) `audio-stats.json` (peak / RMS in dBFS via
    `astats`; silence intervals via `silencedetect`; clipping
    count via direct s16 WAV scan). Output bundle is the visual
    PNG analog of the agent's `screenshot` capture but for the
    audio path — agents can pixel-compare the real-Xbox
    waveform.png against xemu's waveform.png to catch dropouts,
    clipping, silence regions, and pitch drift WITHOUT
    listening. CLI: `audio-waveform.py INPUT [--out-dir DIR]
    [--width N --height N] [--silence-db N] [--colors STR]`.
    Validated on the 10 s composite capture: 1.58 s of audio
    extracted from UnleashX dashboard chime, peak −7.84 dBFS,
    RMS −9.23 dBFS, 0 silence intervals, 0 clipping samples.

18. **Investigate pbkit + D:\\ fopen hang (task #9).** Tier-1
    diag XBEs (mirror, color-channel, depth-floor) chainload +
    reboot fine but never write `D:\<id>-capture.bin` —
    pipeline-smoke (no pbkit) writes its blob fine, so the
    issue is pbkit-specific fopen. **No longer a hard blocker
    as of 2026-05-07** — `composite-record.sh` provides an
    alternative Tier-1 reference path: run the diag XBE,
    capture its rendered output via the MS2109 stick during
    the render-loop hold, compare post-resolve PNG against
    math-derived oracle. The in-XBE D:\\ write path is still
    desirable as the cleanest reference; investigate when
    convenient. Try: `pb_kill()` before fopen, reduce render
    frame count from 300 → 60, add `fflush()` + check `fclose`
    rc, OR add explicit debugPrint after each fopen attempt
    + run with TV attached to read the console output.

19. **Tier 1 of controller injection — diag-XBE shared-buffer
    shim (NEXT SESSION).** Move `oracle_ctrl_buffer` from BSS
    to `ExAllocatePoolWithTag(NonPagedPool, …, 'XCTR')` so the
    physical address is stable across `XLaunchXBE`. Add
    `xbe-tests/lib/xbed_input_synth.{h,c}` so any diag XBE
    opts in by including `lib.mk`. Author one
    `controller-roundtrip` Tier-1 diag XBE that reads the
    buffer + composites a per-event color stripe to disk for
    pixel-exact comparison. Closes the diag-XBE-driven
    gameplay-validation loop. See `controller-injection-research.md`
    for the full plan including Tier 2 (kernel hook) and Tier 3
    (Teensy hardware emulator) tiers.

20. **Take a kernel symbol dump from the project Xbox.** Use
    the agent's `mem.read` to walk the kernel image's PE export
    table and dump every exported symbol's address. Stash under
    `xbox-oracle-backup/2026-05-06/kernel-symbols/`. Foundation
    for Tier 2 of controller injection (kernel hook on
    `OhciControllerInterruptDispatch` so retail games see
    synthetic input).


## Oracle pipeline (2026-05-07 — full Tier-1 validation shipped)

The real-Xbox oracle is now production-ready. Use these wrappers
in preference to the underlying scripts; they handle the inter-
script ordering (FTP-up-then-down, agent-up-then-down, post-runxbe
recovery) and emit structured exit codes so they compose cleanly
into CI gates.

- `scripts/apple-silicon/oracle-smoke.sh` — single-command pipeline
  health check. 12 mandatory layers (ping, ensure-agent, info,
  eeprom, mem.read, nv2a.read, vram.read, controller.* roundtrip,
  buffer-info magic, screenshot) plus optional Tier-1 diag pipeline
  per `--tier1 <ids>`. Exit 0 = all green, 1 = any layer failed.
  Run before any Metal-renderer change that wants to be validated
  against real-Xbox truth, and as a regression check after any
  oracle-side refactor. See top of script for full usage.

- `scripts/apple-silicon/m15-visual-gate.sh` — composite M15
  default-on visual-gate runner. Runs in canonical order: build
  verification, oracle health, Metal canary regression gate
  (counters), Tier-1 diag-XBE matrix on Metal + real Xbox. With
  `--paired` adds Metal-vs-GL canary diff. Exit 0 = M15 default-on
  flip is unblocked from the oracle's perspective; non-zero blocks
  the flip. Designed to be the single command that decides "is
  Metal ready to be default-on?" — answer is YES iff exit == 0
  with all gates green.

- `scripts/apple-silicon/capture-composite-reference.sh` —
  capture a real-Xbox reference frame for a diag XBE via the
  MS2109 composite stick (third oracle leg, independent of the
  in-XBE D:\\ write path). Auto-extracts scene-change keyframes
  from the captured video and picks the one that best matches the
  diag's expected pattern as the canonical reference PNG. Saves
  to `docs/apple-silicon/xbox-real-references/<id>/<label>.png`.

- `oracle-orchestrator.py health-check` — structured-JSON probe
  of every reachable layer (ping/ftp/agent/buffer-info/anchor).
  Exit 0 if ping AND (ftp OR agent) AND (if agent up) buffer
  magic matches `'XCTR'`. Used by `oracle-smoke.sh` for the
  buffer-info layer; suitable for CI gates that need a single
  scriptable health probe.

- `oracle-orchestrator.py status` — lighter probe (ping/ftp/agent
  only). Exit 0 if ping AND (ftp OR agent). Use this when you
  need a fast yes/no for "is the Xbox accessible somehow"; use
  `health-check` when you need the agent's version + buffer
  metadata.

### Diag-XBE library (Tier 1: shipped 2026-05-07)

Four Tier-1 diag XBEs validate the NV2A + the agent's Tier-1
controller injection. All four PASS byte-exact on real Xbox AND
on xemu Metal:

| XBE | What it tests | Pre-run setup | PASS criterion |
|---|---|---|---|
| `mirror` | front-buffer Y-orientation; primitive coverage rules | none | math-derived (`expected.py:default`) |
| `color-channel` | RT format / channel ordering (B/R swap detection) | none | math-derived |
| `depth-floor` | LEQUAL depth + Z perspective + native-tri-depth path | none | math-derived |
| `controller-roundtrip` | Tier-1 controller injection: synth state → kernel pool → diag XBE → render | `controller.set port=0 buttons=… lt=… …` BEFORE chainload | math-derived (`expected.py:from_state(...)`) using the SAME values |

Build all four locally:

```sh
eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)"
for d in mirror color-channel depth-floor controller-roundtrip; do
    (cd scripts/apple-silicon/xbe-tests/$d && make)
done
```

### Tier-1 controller injection (shipped 2026-05-07)

The agent's `oracle_ctrl_buffer` now lives in a persistent
kernel-pool allocation (`MmAllocateContiguousMemoryEx` +
`MmPersistContiguousMemory`) instead of BSS. The physical address
is published in the persistence anchor file
`E:\Apps\oracle-agent\state\ctrl-addr.txt`. A chainloaded diag XBE
that links the `xbed_input_synth_*` shim (declared in
`xbe-tests/lib/xbed_input_synth.h`) calls
`xbed_input_synth_attach()` to mount E:, read the anchor, and map
the buffer into its address space via the kseg0 identity map.
Subsequent `xbed_input_synth_read(port, &state)` calls return the
latest synthetic state set by Mac-side `controller.set` RPCs.

The `controller-roundtrip` Tier-1 diag XBE validates the round-trip
end-to-end:

```sh
./scripts/apple-silicon/oracle-smoke.sh --tier1 controller-roundtrip \
    --buttons 0xA5A5 --lt 16384 --rt 8192 \
    --lx 12345 --ly -12345 --rx -32768 --ry 32767
```

Validated 2026-05-07: byte-exact match (`changed_pixels_pct=0.0000`)
between the captured front buffer and `expected.py:from_state(...)`
for the same synthetic state. This proves:

- The kernel-pool buffer survives the `runxbe` chainload.
- The shim's anchor-file read + kseg0 mapping are correct.
- The shim's seq-stamped read avoids torn reads.
- The diag XBE's render path correctly observes every byte of the
  agent-set state.

Tier 2 (kernel hook for retail games) and Tier 3 (Teensy hardware
emulator) remain as documented in
`docs/apple-silicon/controller-injection-research.md`. Tier 1 alone
unblocks **diag-XBE-driven** gameplay validation: any future diag
XBE that wants to react to synthetic input just `#include`s
`xbed_input_synth.h` and calls `attach()` + `read()`.

### Front-buffer capture path (PCRTC fix, 2026-05-07)

`xbed_capture_front_to_xoss` (in
`scripts/apple-silicon/xbe-tests/lib/xbed_capture.c`) now reads
the actual displayed front buffer via the NV2A's `PCRTC_START`
register (`0xFD000000 + 0x600800`), not the kernel-managed
`XVideoGetFB()` page. This fix unblocked all three Tier-1 NV2A
diag XBEs (mirror / color-channel / depth-floor) on real Xbox —
they previously rendered correctly via pbkit but the capture
grabbed the kernel framebuffer (which still contained the diag's
debug-print console). With the PCRTC path, capture grabs whatever
the CRTC is actually scanning out, which IS the rendered pattern.

The fix falls back to `XVideoGetFB()` when PCRTC_START is zero
(pbkit not initialized — pipeline-smoke's CPU-paint case), so
existing CPU-painted Tier-4 XBEs still work unchanged.

### M15 default-on visual gate (oracle-driven)

Per `metal-renderer-plan.md` §M15 + `oracle-workflow.md`, the M15
default-on flip requires (among other criteria):

> All Tier-1 diag XBEs PASS on real Xbox AND on xemu Metal,
> with per-pixel `changed_pixels_pct < 1.0` against either the
> math-derived oracle or the canonical real-Xbox reference.

Driven by:

```sh
./scripts/apple-silicon/m15-visual-gate.sh
```

Or, just the Tier-1 matrix:

```sh
python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
    --renderer metal --renderer real-xbox \
    --max-changed-pct 1.0 --threshold 8
```

### Persistence anchor file format

`E:\Apps\oracle-agent\state\ctrl-addr.txt` is the canonical
discovery point for the agent's persistent controller buffer.
Format (v1, 2026-05-07):

```
XCTR
0x<8-hex-physical-address>
0x<8-hex-virtual-address>     ; informational (kseg0 = phys|0x80000000)
0x<8-hex-size-bytes>          ; always 0x00001000 in v1
```

The v1 anchor file is read by `xbed_input_synth_attach` on diag XBE
startup. The previous agent reattach reader was removed before
production; each agent launch fresh-allocates and republishes the
anchor.

A version mismatch (`XBED_INPUT_SYNTH_VERSION` ≠ buffer's
`version` field) fails-loud rather than reading garbage.

### Anchor write verification (2026-05-07)

The production agent writes the canonical anchor directly, flushes
and closes it, then reopens and byte-verifies the exact published
address. `controller.buffer-info` reports `anchor_ok=1` only after
that readback verification succeeds.
The on-disk anchor stayed at the OLD agent's phys; diag XBEs
mapped that old persistent buffer and reported stale state.

Fix (2026-05-07): use `NtSetInformationFile` directly with
`FILE_RENAME_INFORMATION.ReplaceIfExists = TRUE` for a single
atomic replace operation, then call `NtFlushBuffersFile` to
commit the FATX rename to disk before the kernel image swap
(`XLaunchXBE` / `runxbe`) can wipe in-memory state. The earlier
fopen + fflush + fclose into the `.tmp` sibling is unchanged.

### `oracle-stress.sh` — repeated-smoke loop (2026-05-07)

```sh
./scripts/apple-silicon/oracle-stress.sh [--iterations N]
```

Runs `oracle-smoke.sh --tier1 mirror,color-channel,depth-floor,
controller-roundtrip` in a `[1..N]` loop (default N=10) to
reproduce the transient "agent listening but RPCs return empty
payload" degraded state observed once on 2026-05-07 evening.
Per-iteration logs go to
`benchmark-runs/oracle-stress-<UTC>/iter-NNN.log`; on the first
failure, the post-fail diagnostic snapshot (agent `info`,
`controller.buffer-info`, `nv2a.read`, `mem.read 0x80000000`) is
written to `iter-NNN-diag/post-fail.txt`. Exit 0 if all N
iterations PASS (degraded state NOT reproduced); exit 1 + logs
the first failing iteration if reproduced.

### `oracle-seqlock-test.py` — concurrent set/get tear check (2026-05-07)

```sh
# Offline algorithmic check (no Xbox needed):
python3 scripts/apple-silicon/oracle-seqlock-test.py --selftest

# Live test (hammers controller.set + controller.get from
# concurrent threads):
python3 scripts/apple-silicon/oracle-seqlock-test.py \
    --rounds 100 --workers 2 --readers 2
```

Validates the agent's per-port seqlock (`seq_begin_write` →
parity-odd, `seq_end_write` → parity-even) catches torn reads
end-to-end across the wire protocol. The selftest mode is a
5-case unit test of the predicate logic — runs offline. The
live test launches `--workers` concurrent writers and
`--readers` concurrent readers against the agent for `--rounds`
iterations each; PASSES iff every observed snapshot has even
seq AND the writer made forward progress (max_seq > 0).

### `oracle-validate.sh` — composite production-grade gate (2026-05-07)

```sh
./scripts/apple-silicon/oracle-validate.sh
```

Single-command "is the oracle production-grade?" gate. Composes:
1. `oracle-smoke.sh` (12 baseline RPC layers).
2. `xbe-harness Tier-1 matrix` on Metal + real Xbox.
3. `controller-roundtrip` chainload + diagnostic-file inspection
   (verifies the persistent buffer survived chainload byte-exact;
   uses `D:\controller-roundtrip-diag.txt` written by the diag
   XBE's instrumented attach path).
4. `oracle-stress.sh` 10-iteration burst.
5. `oracle-seqlock-test.py` live mode.

Exit 0 = oracle is production-grade; non-zero = `report.md`
records which layer failed. This is the "oracle side" complement
to `m15-visual-gate.sh`'s "renderer side"; the two together cover
the M15 default-on pre-conditions.

### Re-attach build removed (2026-05-07)

The opt-in `bin-reattach/default.xbe` /
`ORACLE_CTRL_ALLOW_REATTACH` path was removed before production.
Production always fresh-allocates the persistent controller page,
writes the anchor directly, verifies it by readback (`anchor_ok=1`),
and relies on the 10-iteration stress gate to bound restart behavior.
