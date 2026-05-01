# Voice Fast-Lock Investigation (Negative Result)

Date: 2026-05-01

## Hypothesis

The post-fast-read sample profile
(`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`)
showed `voice_lock` at 6.8 % of TCG-thread time — the largest remaining
mutex-wait source after `pgraph_read` was made lock-free. Hypothesis: a
lock-free voice_lock fast path (atomic OR/AND on the per-voice
`voice_locked[]` bitmap, no `cond_signal`) would recover most of that
6.8 %, lifting PGR2 / Rainbow FPS proportionally.

## Implementation

In `hw/xbox/mcpx/apu/vp/vp.c`:

```c
if (voice_fast_lock_enabled()) {
    if (lock) {
        qatomic_or(&d->vp.voice_locked[v / 64], mask);
    } else {
        qatomic_and(&d->vp.voice_locked[v / 64], ~mask);
    }
    return;
}
```

Gated behind `XEMU_VOICE_FAST_LOCK=1`. Reader-side `is_voice_locked()`
already uses `qatomic_read`, so the bitmap is race-free without the
mutex.

The dropped `cond_signal` was bounded by the audio worker's existing
`cond_timedwait` calls — 1 ms on the voice-work-dispatch path
(vp.c:1737), 5 ms on the APU-trapped path (apu.c:296). A missed signal
costs at most 1 ms of audio-worker latency in the common path, which
should be audio-imperceptible.

## Measurement

PGR2 mid-route snapshot (`pgr2_gameplay_b4`), 30 s `noop.csv` replay:

| Metric | 3 flags only | + voice_fast_lock | Δ |
| --- | --- | --- | --- |
| post_load_avg_fps | 30.64 | 30.79 | +0.49 % (noise) |
| post_load_avg_mspf | 18.06 | 17.31 | -4.15 % (improvement) |
| post_load_mspf_max_p99 | 39.86 | 35.99 | -9.71 % (improvement) |
| post_load_stutter_intervals_30fps | 6 | 11 | +83.33 % (regression) |
| post_load_longest_stutter_run_30fps | 2 | 4 | +100 % (regression) |

PGR2 retail gameplay route (300 s,
`pgr2-gameplay.csv`):

| Metric | 3 flags only | + voice_fast_lock | Δ |
| --- | --- | --- | --- |
| post_load_avg_fps | 31.76 | 31.84 | +0.25 % (noise) |
| post_load_avg_mspf | 12.34 | 13.14 | +6.48 % (regression) |
| post_load_mspf_max_p99 | 38.00 | 41.53 | +9.29 % (regression) |
| post_load_stutter_intervals_30fps | 36 | 69 | +91.67 % (regression) |
| post_load_longest_stutter_run_30fps | 3 | 6 | +100 % (regression) |

Run dirs:

- Snapshot baseline: `benchmark-runs/20260501-134801-pgr2`
- Snapshot voice_fast_lock: `benchmark-runs/20260501-134840-pgr2`
- Route baseline: `benchmark-runs/20260501-123525-pgr2`
- Route voice_fast_lock: `benchmark-runs/20260501-134947-pgr2`

## Verdict

The change does **not** improve average FPS in either measurement and
shows mixed signals on jitter — p99 max-frame is tighter at the snapshot
but worse on the route, and the count of intervals with at least one
> 33 ms frame went up substantially in both measurements. The retail
route comparison shows variance well above the change's expected effect
size, so the regression is not statistically conclusive, but there is
also no positive evidence to ship the flag.

Per the project's data-driven rule, the flag is **not landed**. Code
reverted.

## Why the fresh sample said 6.8 %, but FPS didn't move

The 6.8 % `voice_lock` figure was measured at the
`pgr2_gameplay_b4` snapshot replay with `noop.csv` (no controller
input). Even at that scene, the audio worker thread holds `&d->lock`
across each `se_frame()` cycle (~5.33 ms), so any CPU thread that hits
a voice_lock call during that window pays the wait. Removing the
mutex on the voice-bitmap update path does eliminate that direct
contention. But:

1. The CPU thread is no longer the binding constraint at this scene
   (post-fast-read profile shows pfifo_thread idle 41.5 % of time on
   the FIFO condvar). Freeing 6.8 % of TCG-thread time does not
   translate to FPS unless the renderer can absorb more work — and it
   was already idle.
2. The audio worker's missed-signal latency, even bounded at 1 ms,
   still adds variance. With the change, the audio worker can be up
   to 1 ms late waking from `cond_timedwait`, which appears to perturb
   when audio-bound code paths complete relative to the renderer
   submission cycle. The increased jitter in the snapshot p50 metric
   is consistent with that.

Both effects together explain the noise-level FPS change with
slightly worse jitter shape. Lock-elision yields are bounded by the
post-elision-thread's idleness, and `voice_lock` happens to land in a
regime where the CPU thread already has more cycles than it can use.

## Followups

1. The snapshot scene is biased toward minimal voice-event traffic
   (paused car, no engine-rev cadence change). A scene with active
   audio (lots of voices starting / stopping) might show a different
   result. Building such a scene is non-trivial and has no clear FPS
   target relevance because gameplay FPS in the audio-busy phases is
   already at the 30 FPS floor.
2. The remaining 8.9 % TCG-thread mutex wait is split across many
   small contributors. The bigger TCG efficiency wins likely live
   in the floating-point helper paths (SSE / x87) — see
   `2026-05-01-pgr2-bottleneck-postfast.md` for the helper sample
   share. A focused investigation of whether SSE float32 ops are
   going through softfloat unnecessarily on Apple Silicon is the next
   data-driven step on the TCG side.
3. Renderer-side: Crimson Skies stutters are dominated by Apple's
   synchronous shader compile path inside `glDrawElements` — an
   async-shader-compile slice (separate from any lock-elision work)
   should give a clear user-visible jitter improvement on Crimson
   without affecting other titles.
