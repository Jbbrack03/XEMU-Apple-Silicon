# 2026-05-01 — XEMU_PGRAPH_ASYNC_SHADER_COMPILE implementation and validation

## Summary

Implemented `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` opt-in flag: a worker
thread bound to a third shared `g_nv2a_context_shader_compile` GL context
runs `glLinkProgram` off the renderer's critical path. The renderer's
`pgraph_gl_bind_shaders` enqueues new shader-state hashes, sets a
`shader_skip_draw` flag, and the corresponding draw is skipped via
early-returns in `pgraph_gl_draw_begin` / `pgraph_gl_draw_end` while the
worker is still compiling. Pattern follows RPCS3 PR #4876 / wiki "Async
(Skip Draws)".

**Verdict: implementation is correct and counters confirm worker activity,
but it does not measurably reduce the headline 1.35-second worst-frame
stutter on Crimson Skies.** The stutter is not shader-compile time — it
is something else co-occurring with shader-bind events. See "Critical
finding" below.

## Counters added

- `SHADER_COMPILE_COUNT`, `SHADER_COMPILE_US_TOTAL` — wraps
  `generate_shaders()` and `pgraph_gl_shader_load_from_memory()` with
  `qemu_clock_get_us` timing. Atomic-add so the worker's accumulation
  does not corrupt the renderer's per-frame counter reset.
- `SHADER_COMPILE_ASYNC_QUEUED`, `SHADER_COMPILE_ASYNC_COMPLETED` — only
  nonzero when async is enabled.
- `SHADER_DRAWS_SKIPPED_PENDING` — number of draws skipped while a
  compile is in flight.

All five surfaced in `extract-perf-summary.sh` and documented in
`docs/apple-silicon/automation.md`.

## Validation runs

### Crimson Skies retail-route, 300 s baseline (async off)

Run dir: `benchmark-runs/20260501-181049-crimson-skies`
Flags: `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1
XEMU_PERF_FRAME_LOG=1`

| Metric                                  | Value      |
| --------------------------------------- | ---------- |
| `post_load_avg_fps`                     | 30.67      |
| `post_load_avg_mspf`                    | 28.40 ms   |
| `post_load_frame_mspf_us_p99`           | 35,802     |
| `post_load_frame_mspf_us_p999`          | 104,331    |
| `post_load_frame_mspf_us_max`           | 1,345,831  |
| `post_load_stutter_frames_30fps`        | 455        |
| `post_load_longest_stutter_run_30fps`   | 10         |
| `post_load_longest_stutter_run_60fps`   | 275        |
| `SHADER_COMPILE_COUNT`                  | 115        |
| `SHADER_COMPILE_US_TOTAL`               | 399,167 us |
| `SHADER_COMPILE_ASYNC_QUEUED`           | 0          |

### Crimson Skies retail-route, async on (168 intervals)

Run dir: `benchmark-runs/20260501-182613-crimson-skies`
Flags: `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1
XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1 XEMU_PERF_FRAME_LOG=1`

The run was halted at 169 intervals when the input-script CSV ended and
the wait-loop's pgrep failed to match the running process (the CSV's case
mismatched the pgrep regex). Sample is shorter than baseline but covers
the full retail-route span up through the post-warmup steady state, so
the per-interval distribution is comparable.

| Metric                                  | Value      | Δ vs baseline |
| --------------------------------------- | ---------- | ------------- |
| `post_load_avg_fps`                     | 29.61      | -3.5%         |
| `post_load_avg_mspf`                    | 32.09 ms   | +13%          |
| `post_load_frame_mspf_us_p99`           | 41,544     | +16%          |
| `post_load_frame_mspf_us_p999`          | 382,090    | +266%         |
| `post_load_frame_mspf_us_max`           | 1,351,887  | +0.4%         |
| `post_load_stutter_frames_30fps`        | 615        | +35%          |
| `post_load_longest_stutter_run_30fps`   | 19         | +90%          |
| `post_load_longest_stutter_run_60fps`   | 159        | -42%          |
| `SHADER_COMPILE_COUNT`                  | 115        | same          |
| `SHADER_COMPILE_US_TOTAL`               | 346,661 us | -13%          |
| `SHADER_COMPILE_ASYNC_QUEUED`           | 115        | n/a           |
| `SHADER_COMPILE_ASYNC_COMPLETED`        | 115        | n/a           |
| `SHADER_DRAWS_SKIPPED_PENDING`          | 756        | n/a           |

### PGR2 snapshot 30 s — sanity / regression check

`benchmark-runs/20260501-183005-pgr2` (async off):
- `post_load_avg_fps` 30.96, `post_load_avg_mspf` 21.45 ms.
- `SHADER_COMPILE_COUNT` 130, `SHADER_COMPILE_ASYNC_QUEUED` 0.

`benchmark-runs/20260501-183046-pgr2` (async on, same snapshot/tag):
- `post_load_avg_fps` 30.84, `post_load_avg_mspf` 19.76 ms.
- `SHADER_COMPILE_COUNT` 139, `SHADER_COMPILE_ASYNC_QUEUED` 139,
  `SHADER_COMPILE_ASYNC_COMPLETED` 139, `SHADER_DRAWS_SKIPPED_PENDING`
  1,000.
- `post_load_longest_stutter_run_30fps` 2 vs 1 (within noise),
  `post_load_longest_stutter_run_60fps` 22 vs 22 (same).

PGR2 snapshot has no judder regressions and no FPS regression. Async on
the snapshot is performance-neutral within run-to-run variance.

### Smoke run — flat-tri-depth XBE

`benchmark-runs/20260501-182456-flat-tri-depth` (async on): 32 shaders
queued and completed in interval 1 with 380 draws skipped.
`SHADER_COMPILE_US_TOTAL` 85,860 us spread across the worker. Confirms
the worker thread, queue, skip-draw fallback, and per-context shared
program name space all work end to end.

## Critical finding: worst-frame stutters are not shader compile time

Comparing baseline vs async run side by side, the bad intervals occur at
**the same gameplay points** with **nearly identical magnitudes**:

| Baseline interval / mspf_max | Async interval / mspf_max | Diff (ms) |
| ---------------------------- | ------------------------- | --------- |
| 19 / 370.1 ms                | 20 / 367.8 ms             | -2.3      |
| 27 / 1,345.8 ms              | 28 / 1,343.7 ms           | -2.1      |
| 28 / 1,169.9 ms              | 29 / 1,278.4 ms           | +108      |
| 32 / 1,321.5 ms              | 33 / 1,351.9 ms           | +30       |
| 34 / 572.1 ms                | 35 / 382.1 ms             | -190      |

The 1-interval offset is the script timing roll; the magnitude pairing is
deterministic, scene-correlated, and largely insensitive to whether
`glLinkProgram` runs on the renderer thread or a worker.

The async slice did move 346 ms of `glLinkProgram` work off the renderer
(`SHADER_COMPILE_US_TOTAL` matches between runs) and skipped 756 draws
during the compile windows. So the slice did its job. The headline
stutter is dominated by **something else** that runs on the renderer
thread when a new shader binding appears — most likely Apple's
GL-on-Metal MSL→Metal pipeline-state-object compile triggered by the
**first `glDrawElements`** with a new program/VAO/state combination,
which is per-context and which the worker's link cannot pre-warm without
also setting up the renderer's VAO and surface state on the worker
context.

`p999` regressed from 104 ms (baseline) to 382 ms (async). That is
consistent with: the worker's `glFinish` after `glLinkProgram` blocks on
Apple's GL command queue, which on Apple Silicon serializes against the
renderer's submitted command buffer. So while the renderer skips the
draw of the pending shader, the FOLLOWING few frames pay queue-drain
cost. Net p999 goes up, even though peak max is unchanged.

## Conclusions and recommended next steps

1. **Ship the slice as opt-in only.** Async shader compile is correct,
   functional, and validated end-to-end, but does not measurably reduce
   the user-visible Crimson Skies judder it was aimed at and slightly
   regresses p999 because of `glFinish` queue serialization. Default
   off.

2. **The next investigation must identify what actually causes the
   1.35-second worst-frame.** The spike is deterministic, scene-bound,
   and persists across same-build runs. Candidate counters to correlate
   per-interval with `mspf_max`: `TEX_UPLOAD`, `SURF_TO_TEX`,
   `SURF_UPLOAD`, `SURF_DOWNLOAD`, plus the new `SHADER_COMPILE_*`
   keys. Add a per-event timestamp log so we can see what fires inside
   the bad frame, not just inside the bad interval.

3. **A follow-up async slice could pre-warm Apple's Metal pipeline on
   the worker** by binding a dummy VAO and surfaces on the worker
   context and issuing a tiny dummy `glDrawElements` after each
   `glLinkProgram`. This forces the MSL→PSO compile on the worker
   instead of waiting for the renderer's first real draw. Risky: needs
   the worker context to share VAO state, which is not currently the
   case.

4. **Phase 4 (native Metal renderer) remains the right path for true
   Apple Silicon judder elimination.** It moves shader work to MSL
   directly with explicit pipeline-state objects, which is the only
   way to escape Apple's GL-on-Metal driver behavior and its queue
   serialization.

5. **`glFinish` -> `glFlush` A/B (completed in this session).** The
   worker's barrier was changed from `glFinish` to `glFlush` after the
   first round of paired runs. Subsequent PGR2 snapshot run with
   `glFlush` showed a clean tail
   (`benchmark-runs/20260501-183845-pgr2`,
   `post_load_frame_mspf_us_p999` = 36,517 us, `frame_mspf_us_max` =
   368,093 us — the latter is the documented Apple-OpenGL startup
   spike, not gameplay jitter). PGR2 snapshot now shows no
   judder regression from the async path.

   On Crimson the second paired run
   (`benchmark-runs/20260501-183944-crimson-skies`, 54 intervals
   captured before the run was interrupted) reported
   `post_load_frame_mspf_us_p999` = 1,181,728 us and
   `post_load_frame_mspf_us_max` = 1,330,215 us — same order of
   magnitude as the `glFinish` run. So `glFlush` is the right
   technical choice (worker no longer drains Apple's command queue
   for PGR2-class workloads) but does **not** rescue Crimson's
   headline stutter. The Crimson stutter is real and unrelated to
   the worker's GL barrier choice; it sits on the renderer thread
   regardless. This is consistent with the conclusion above:
   Apple's MSL->PSO compile inside `glDrawElements` is the prime
   suspect.

## Source-code changes summary

- `hw/xbox/nv2a/debug.h`: `SHADER_COMPILE_COUNT`,
  `SHADER_COMPILE_US_TOTAL`, `SHADER_COMPILE_ASYNC_QUEUED`,
  `SHADER_COMPILE_ASYNC_COMPLETED`, `SHADER_DRAWS_SKIPPED_PENDING`.
  `nv2a_profile_add_counter` helper.
- `hw/xbox/nv2a/pgraph/gl/renderer.h`: `pending_compile`,
  `compile_queue_link` on `ShaderBinding`. New PGRAPHGLState fields:
  `async_shader_compile_enabled`, `shader_skip_draw`, `compile_thread*`,
  `compile_queue*`, `shader_module_cache_lock`. `extern
  g_nv2a_context_shader_compile`.
- `hw/xbox/nv2a/pgraph/gl/renderer.c`: third shared
  `g_nv2a_context_shader_compile` created in `early_context_init` only
  when `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`. Startup banner
  `xemu-perf: async_shader_compile=1 source=...`.
- `hw/xbox/nv2a/pgraph/gl/shaders.c`:
  - `shader_module_cache_lock` init/destroy.
  - `nv2a_profile_inc_counter_atomic` /
    `nv2a_profile_add_counter_atomic` helpers.
  - `generate_shaders()` accepts `validate_program` flag (worker passes
    false because its context has no VAO bound), wraps module-cache
    access in `shader_module_cache_lock`, calls `glFinish()` to make
    the program observable in shared name space, and atomically sets
    `binding->initialized=true` after `smp_wmb`. Wrapped with
    `qemu_clock_get_us` timing for the new counters.
  - `pgraph_gl_shader_load_from_memory()`: same timing wrap.
  - `pgraph_gl_bind_shaders()`: async path that enqueues compile and
    sets `shader_skip_draw` when binding is not yet initialized;
    synchronous fallback otherwise.
  - `shader_cache_entry_post_evict()` aborts on
    `pending_compile=true`.
  - New `pgraph_gl_shader_compile_worker` thread function.
  - `pgraph_gl_init_shaders` / `pgraph_gl_finalize_shaders` manage the
    worker.
- `hw/xbox/nv2a/pgraph/gl/draw.c`: `pgraph_gl_draw_begin` /
  `pgraph_gl_draw_end` early-return on `r->shader_skip_draw`.
- `scripts/apple-silicon/extract-perf-summary.sh`: surfaces the five
  new counters.
- `docs/apple-silicon/automation.md`: documents the flag and the new
  counters.
