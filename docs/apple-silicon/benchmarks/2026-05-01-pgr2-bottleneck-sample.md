# PGR2 Mid-Route Bottleneck Sample Profile

Date: 2026-05-01

## Purpose

Identify what is making PGR2 slow at the `pgr2_gameplay_b4` snapshot (16.56
FPS with both `XEMU_NATIVE_TRI_DEPTH=1` and `XEMU_NATIVE_QUAD=1` on, zero
geometry-shader draws). Geometry-shader removal hit a wall there; the next
slice cannot be data-driven without knowing where the wall-clock time is
going.

## Method

1. Started the snapshot benchmark in the background:

   ```sh
   SNAPSHOT_HDD=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2
   TAG=pgr2_gameplay_b4
   XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 \
   XEMU_BENCH_SCREENSHOT_BACKEND=none \
   XEMU_BENCH_HDD_SOURCE=$SNAPSHOT_HDD \
   XEMU_BENCH_LOADVM_TAG=$TAG \
   scripts/apple-silicon/run-benchmark.sh pgr2 \
     scripts/apple-silicon/input-scripts/noop.csv 60
   ```

   Run dir: `benchmark-runs/20260501-122035-pgr2`.

2. After ~10 s warm-up, attached Apple's `sample` tool to the live xemu PID
   for 25 s of steady-state:

   ```sh
   sample <pid> 25 -mayDie -file /tmp/sample-pgr2-snapshot.txt
   ```

3. Parsed the resulting call-graph by thread and bucketed the inner-most
   xemu/system frames.

`sample` ships with macOS, requires no Xcode setup, and produces plain-text
output that can be parsed without a separate tool. The 25 s window at
1 ms intervals gives 17,876 samples per thread — enough resolution to
distinguish percent-level buckets.

## Top-Level Thread Picture

xemu has 93 threads at this snapshot. Two are doing all of the meaningful
work:

| Thread | Role | Steady-state behavior |
| --- | --- | --- |
| `Thread_10393313` | TCG i386 CPU emulator (`mttcg_cpu_thread_fn`) | 99.8% on-CPU. About a third of that is *waiting on host mutexes*, not running x86 code. |
| `Thread_10393390` | NV2A PFIFO + PGRAPH thread (`pfifo_thread`) | About 29% doing real renderer work (`pgraph_method` → `pgraph_gl_draw_end` → `pgraph_gl_flush_draw` → Apple OpenGL/Metal). Rest is idle on the FIFO condvar. |

The xemu **main thread** is overwhelmingly idle: 50% blocked on the NV2A
render condvar (`qemu_event_wait` waiting for a frame), 29% blocked in
`Cocoa_GL_SwapWindow` → SkyLight present (i.e., display VSYNC). It is not
the bottleneck. The audio voice worker threads (`Thread_10393314`,
`Thread_10393315`) are mostly idle on their condvars too — they're not the
bottleneck either.

## TCG Thread Attribution (out of 17,876 samples / 25 s)

| Bucket | Samples | % of TCG thread |
| --- | --- | --- |
| Real i386 emulation: JIT-translated x86 (cpu\_tb\_exec leaves) | ~9,938 | 55.5% |
| Float math helpers (`helper_mul/add/sub/comi*`) | 853 | 4.7% |
| **`pgraph_read` mutex wait** | **3,976** | **22.2%** |
| **`voice_lock` mutex wait** (audio voice processor) | **1,236** | **6.9%** |
| `pgraph_write` mutex wait | 496 | 2.7% |
| `gp_write` / `user_write` / `vp_write` mutex wait | 155 | 0.8% |
| TB lookup (`helper_lookup_tb_ptr`) | 891 | 4.9% |
| `tlb_reset_dirty` range invalidations | 303 | 1.6% |
| `cpu_io_recompile` | 98 | 0.5% |
| Other (overhead, kernel mode-switches, etc.) | ~30 | <0.5% |

Total mutex wait: **~32.6%** of TCG thread time. Approximately one third
of the i386 emulator's wall time is *sleeping*, waiting for a host mutex
held by the renderer thread. The remaining ~67% is real emulation +
support work.

The dominant lock-wait sources are `pg->lock` (the NV2A PGRAPH lock,
acquired on every Xbox-CPU MMIO access into PGRAPH register space), and
`voice_lock` (the Xbox APU voice processor lock).

## Renderer (PFIFO) Thread Attribution

`Thread_10393390` (`pfifo_thread`):

- 6,645 samples (37%): idle on the FIFO condvar, waiting for commands.
- 5,195 samples (29%): real PGRAPH command processing, of which the
  dominant chain is `pgraph_method` → `pgraph_NV097_SET_BEGIN_END_handler`
  (END method) → `pgraph_gl_draw_end` → `pgraph_gl_flush_draw` →
  `glDrawElements_ACC_GL3Exec` → `gldRenderVertexArray` (Apple's GL/Metal
  shim) → `GLDContextRec::setRenderState` /
  `setRenderSamplersAndTextures` / `prepareResourceForGPUAccess`.
- The rest is in pfifo housekeeping, RAMHT lookups, and small PGRAPH
  methods.

The renderer thread is *not* CPU-saturated. It only does ~29% real work and
is otherwise blocked on the FIFO condvar. The renderer's slowness is not
the headline cost; **its slowness only matters because it holds `pg->lock`
across the slow OpenGL submission**, which serializes the CPU emulator.

## Why The Geometry-Shader Slices Helped

The 60 → 50 → 33 → 18 → 17 FPS step was almost entirely about how long
`pg->lock` is held during a draw. A geometry-shader-backed
`glDrawElements` blocks the pfifo thread inside Apple's GL/Metal driver
for longer than a native-triangle one, which keeps the CPU emulator
asleep on PGRAPH MMIO reads. Removing geometry shaders cuts the
lock-hold-per-draw time, which transitively unlocks emulator throughput.
That is the same mechanism the next slice should target directly.

## Locking Sites

- `pgraph_read` (`pgraph.c:79`): `qemu_mutex_lock(&pg->lock)` for the full
  duration of every MMIO read.
- `pgraph_write` (`pgraph.c:123`): `qemu_mutex_lock(&pg->lock)` for the full
  duration of every MMIO write.
- `pfifo.c:189` and `pfifo.c:224`: pfifo thread acquires `pg->lock` and
  holds it across `pgraph_method()`, which includes the entire OpenGL
  submission for `NV097_SET_BEGIN_END` (END method).
- `pgraph.c:2892`: there are commented-out `//qemu_mutex_unlock(&pg->lock);`
  / `//qemu_mutex_lock(&pg->lock);` brackets around the
  `BACK_END_WRITE_SEMAPHORE_RELEASE` semaphore-write path, suggesting
  earlier maintainers also considered narrowing the critical section here.
- `voice_lock` is taken on every NV_USER write to APU voice registers; that
  lock is contended between the CPU thread and the audio voice worker (which
  is mostly idle but still acquires/releases voice_lock).

## Implications For The Next Slice

Geometry-shader removal has reached its useful limit at these scenes. The
new bottleneck class is **host-side lock contention between the CPU
emulator and the renderer/audio threads**. The most promising attacks, in
increasing order of risk:

1. **Reproducible phase timing in `xemu-perf:` lines.** Add a
   `XEMU_PERF_PHASE_TIMING=1` flag that records, per interval:
   - Total TCG thread time and time spent in `qemu_mutex_lock` blocked
     state on `pg->lock` and `voice_lock`.
   - Total `pfifo_thread` time and time spent inside `pgraph_method` /
     `pgraph_gl_flush_draw` while holding `pg->lock`.
   This makes the sample findings repeatable across runs without re-running
   `sample`, and gives every future change an objective lock-contention
   metric.
2. **Lock-free read for hot PGRAPH read addresses.** Identify the one or
   two register addresses (likely an interrupt/status word) responsible for
   the bulk of `pgraph_read` calls and convert those branches to
   `qatomic_read` rather than taking the mutex. Bounded blast radius and
   easy to revert if it breaks something.
3. **Drop `pg->lock` around the slow OpenGL submission in
   `pgraph_gl_flush_draw`.** After PGRAPH state has been captured into the
   shader binding and vertex buffers, the GL calls themselves do not need
   the PGRAPH state lock. Gate behind a flag
   (`XEMU_PGRAPH_RELEASE_LOCK_DURING_GL=1`) and audit carefully. If safe,
   this directly cuts the dominant pfifo-thread lock-hold time.
4. **Audit `voice_lock` contention.** Same shape as `pg->lock` but smaller
   numerical impact; still worth a look once #2 / #3 land.

## Sample File

`/tmp/sample-pgr2-snapshot.txt` (28,531 lines, 1.0 MB). Not committed to
the repo. Re-derive with the command above against
`benchmark-runs/20260501-122001-pgr2/xbox_hdd.qcow2` if a fresh sample is
needed.
