# 2026-05-01 — Per-event timing diagnostic: Crimson 1.35s stutter is upstream of the renderer

## Summary

Added per-subsystem microsecond timing counters and a `xemu-spike:` event
log around every suspect renderer operation
(`pgraph_gl_bind_textures`, `pgraph_gl_render_surface_to_texture`,
`pgraph_gl_upload_surface_data`, `pgraph_gl_surface_download_if_dirty`,
`pgraph_gl_draw_begin`, `pgraph_gl_flush_draw`, the GL texture upload
inside `upload_gl_texture`, and the per-frame `glFinish` inside
`pgraph_gl_flip_stall`).

Ran the Crimson 300 s retail route under all three opt-in flags
(`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`)
plus `XEMU_PERF_FRAME_LOG=1` and `XEMU_PERF_SPIKE_LOG=1`. Bad intervals
(per-interval `mspf_max > 1000 ms`) and good intervals
(`mspf_max < 35 ms`) compared side by side.

**Verdict: the renderer thread is essentially idle during Crimson's
1.35-second worst-frame intervals. The bottleneck is upstream of
PGRAPH — the Xbox CPU emulation (TCG vCPU thread) is not feeding the
FIFO fast enough.** Async shader compile would not have fixed it; a
native Metal renderer would not fix it either.

## Counters added

- `BIND_TEXTURES_US_TOTAL`, `TEX_UPLOAD_US_TOTAL`,
  `SURF_TO_TEX_US_TOTAL`, `SURF_UPLOAD_US_TOTAL`,
  `SURF_DOWNLOAD_US_TOTAL`: wall-clock microseconds spent inside the
  named operation per perf interval, summed across all calls.
- `DRAW_BEGIN_US_TOTAL`, `FLUSH_DRAW_US_TOTAL`,
  `FLIP_STALL_US_TOTAL`, `FLIP_STALL_GLFINISH_US_TOTAL`: same, for
  the renderer's draw entry and flip barrier. `FLUSH_DRAW` is the
  `glDrawElements` / `glMultiDrawArrays` dispatch path — where Apple's
  GL-on-Metal driver was hypothesized to do MSL→pipeline-state
  compile inside the first draw with a new state combination.
- `xemu-spike:` event log: per-event lines emitted whenever a single
  timed operation exceeds `XEMU_PERF_SPIKE_LOG_THRESHOLD_US` (default
  50,000 us). Off unless `XEMU_PERF_SPIKE_LOG=1`.

All surfaced in `extract-perf-summary.sh`.

## Bad-interval breakdown — Crimson retail route

`benchmark-runs/20260501-190239-crimson-skies`, 285 timed intervals.
`IDLE` = 1000 ms − (FLUSH + BEGIN + SDOWN + FLIP).

| interval | mspf_max | frames | FLUSH | BEGIN | FLIP | GLFIN | TEX | SDOWN | TUP | RENDERER_TOTAL | IDLE |
| -------- | -------- | ------ | ----- | ----- | ---- | ----- | --- | ----- | --- | -------------- | ---- |
| 2        | 347 ms   | 39     | 68    | 146   | 52   | 52    | 16  | 4     | 3   | 271 ms         | 728 ms |
| 10       | 1,256 ms | 8      | 1     | 11    | 8    | 8     | 0   | 1     | 0   | 22 ms          | 977 ms |
| 11       | 1,185 ms | 10     | 1     | 4     | 12   | 12    | 1   | 4     | 0   | 23 ms          | 976 ms |
| 16       | 1,315 ms | 2      | 0     | 0     | 4    | 4     | 0   | 1     | 0   | 5 ms           | 994 ms |
| 17       | 426 ms   | 12     | 1     | 1     | 12   | 12    | 1   | 1     | 0   | 16 ms          | 983 ms |
| 18       | 490 ms   | 9      | 1     | 3     | 9    | 9     | 1   | 0     | 0   | 14 ms          | 985 ms |

The bad intervals show:
- **Renderer-thread busy time: 5–23 ms out of 1000 ms wallclock.** The
  pfifo thread spent 97–99 % of the second IDLE.
- **Frame count drops to 2–10 fps.** The Xbox CPU emulation is not
  flipping the framebuffer.
- `FLIP_STALL_GLFINISH` is 4–12 ms — Apple's GL command queue drains
  promptly, because the renderer barely submitted anything.
- No individual operation exceeded the 50 ms spike threshold; spike log
  emitted **zero** lines across the 300 s run.

## Good-interval baseline — same run, mspf_max < 35 ms

| interval | mspf_max | frames | FLUSH | BEGIN | FLIP | RENDERER_TOTAL | IDLE |
| -------- | -------- | ------ | ----- | ----- | ---- | -------------- | ---- |
| 3        | 25 ms    | 45     | 100   | 153   | 48   | 375 ms         | 624 ms |
| 4        | 26 ms    | 42     | 107   | 168   | 40   | 406 ms         | 593 ms |
| 5        | 24 ms    | 44     | 112   | 176   | 43   | 428 ms         | 571 ms |
| 25       | 33 ms    | 31     | 505   | 195   | 14   | 769 ms         | 230 ms |
| 26       | 27 ms    | 31     | 392   | 171   | 14   | 645 ms         | 354 ms |

Good intervals show 375–770 ms of renderer-thread busy time, 31–61 fps.
Renderer is active, paced, and has 200–600 ms of headroom even at peak
(interval 25 has 230 ms idle = 23 % spare).

## Conclusion

The 1.35-second Crimson worst-frame is **not** a renderer-side problem.
During those 1.35-second wall-clock windows:

- The pfifo thread (renderer) is essentially idle.
- Only 2–10 frames flip — the Xbox CPU emulation is the rate limiter.
- Apple's GL-on-Metal driver is consequently not active either; there is
  nothing for it to compile or submit.

This eliminates several previously-tracked hypotheses for Crimson's
judder:

1. **Apple GL synchronous shader compile inside `glDrawElements`** —
   ruled out. `FLUSH_DRAW_US_TOTAL` is 0–1 ms in the bad intervals; if
   Apple were stalling inside a draw, it would show up here.
2. **`glLinkProgram` on the renderer thread** — already disproved by
   the 2026-05-01 async-shader-compile slice.
3. **Per-MSL-pipeline-state-object compile on Apple's driver** —
   plausible in principle but not the cause of *these* specific
   stutters; the renderer is not running draws during them.
4. **Heavy texture upload / surface flush** — ruled out. All
   surface/texture timers are 0–4 ms in the bad intervals.

The actual cause must be on the **TCG vCPU thread** (or the QEMU main
loop / pfifo coordination path), not on the renderer thread. Candidates:

- **TB cache invalidation** when the Xbox modifies its own code (script
  systems, JITed shaders, hot-path patching). Would block the TCG
  thread until translation re-completes.
- **TLB flushes / large memory-mapping changes.** QEMU's softmmu
  invalidates TLB on protection changes.
- **Disk I/O stall** — the simulated DVD read could block the IDE
  thread, which the Xbox CPU then waits on.
- **A specific QEMU helper** that has poor amortized cost on Apple
  Silicon. The 2026-05-01 SSE/x87 audit ruled out the float helpers,
  but other softmmu / atomic / sync paths are not yet measured.
- **pfifo / pgraph synchronization with `glFinish` in `flip_stall`**
  pacing the Xbox CPU more aggressively than necessary. The spike
  pattern of "frames=2 in one second" is consistent with the Xbox CPU
  stalling for a frame budget after each flip — possible but unproved.

## Implication for the renderer-architecture decision (GL / MoltenVK / Metal)

Strongly biases the answer.

- **For Crimson's headline judder specifically**, switching to native
  Metal does not address the cause. The renderer is not the bottleneck.
- **For sustained 60 FPS**, we have two ceilings:
  1. TCG throughput — this run shows the Xbox CPU CAN stall hard
     enough to drop frames to single digits. Without TCG headroom,
     60 FPS is impossible regardless of renderer.
  2. Renderer throughput — Crimson good intervals show 200–600 ms of
     renderer headroom per second at 30 FPS. Doubling FPS at current
     resolution would cut headroom roughly in half and likely still
     fit within Apple's GL on this title.
- **For 1080p / AA / higher-quality textures**, the renderer load
  multiplies. AA in particular triggers more Apple-GL pipeline-state
  variants. None of the bad-interval data tells us how Apple's GL
  behaves under heavier renderer load; this needs a separate
  measurement (e.g. force `pgraph_gl_set_surface_scale_factor` to 4×
  and re-run).

The next implementation slice is therefore **TCG / Xbox-CPU
investigation**, not async-shader work and not a Metal port.
Specifically:

1. Take a `sample` profile DURING one of the bad intervals and
   identify which TCG helpers / softmmu paths dominate. Use the Crimson
   route at the recorded interval-10/16 timing offset and trigger
   sample-profile.sh with the right warmup.
2. If TB-cache invalidation is the cause, evaluate PPTC (persistent
   TCG translation cache; strategy.md Phase 5a) — eliminates re-
   translation work after warmup.
3. If softmmu TLB pressure is the cause, look at whether large mapping
   changes can be batched or whether TLB sizing is suboptimal.
4. If `flip_stall` glFinish is gating the Xbox CPU's frame budget, try
   `XEMU_FLIP_NO_FINISH=1` opt-in as an A/B (likely safe; the existing
   GL surface model already syncs at present-time).

## Source-code changes

- `hw/xbox/nv2a/debug.h`: added `BIND_TEXTURES_US_TOTAL`,
  `TEX_UPLOAD_US_TOTAL`, `SURF_TO_TEX_US_TOTAL`,
  `SURF_UPLOAD_US_TOTAL`, `SURF_DOWNLOAD_US_TOTAL`,
  `FLUSH_DRAW_US_TOTAL`, `DRAW_BEGIN_US_TOTAL`,
  `FLIP_STALL_US_TOTAL`, `FLIP_STALL_GLFINISH_US_TOTAL`. Declared
  `nv2a_profile_spike()`.
- `hw/xbox/nv2a/pgraph/profile.c`: implemented `nv2a_profile_spike()`
  with `XEMU_PERF_SPIKE_LOG=1` /
  `XEMU_PERF_SPIKE_LOG_THRESHOLD_US=N` controls.
- `hw/xbox/nv2a/pgraph/gl/draw.c`: timing wrappers around
  `pgraph_gl_draw_begin` and `pgraph_gl_flush_draw`.
- `hw/xbox/nv2a/pgraph/gl/texture.c`: timing wrapper around
  `pgraph_gl_bind_textures` and `upload_gl_texture`.
- `hw/xbox/nv2a/pgraph/gl/surface.c`: timing wrappers around
  `pgraph_gl_render_surface_to_texture`,
  `pgraph_gl_upload_surface_data`,
  `pgraph_gl_surface_download_if_dirty`.
- `hw/xbox/nv2a/pgraph/gl/renderer.c`: timing wrapper around
  `pgraph_gl_flip_stall` (`glFinish`).
- `scripts/apple-silicon/extract-perf-summary.sh`: surfaces the new
  counters.
