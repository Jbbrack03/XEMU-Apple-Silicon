# PGR2 Mid-Route Bottleneck Sample Profile (Post-Fast-Read)

Date: 2026-05-01

## Purpose

The `XEMU_PGRAPH_FAST_READ=1` lock-elision slice landed earlier today and
roughly doubled snapshot FPS (16.56 → 30.76). The previous bottleneck
sample profile
(`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-sample.md`)
predates that slice. To pursue 60 FPS we need a fresh profile: which
locks/functions dominate now that `pgraph_read` is lock-free?

## Method

Same scene as the previous profile — `pgr2_gameplay_b4` snapshot replay
with all three opt-in flags on:

```sh
XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1 \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 \
scripts/apple-silicon/sample-profile.sh \
  pgr2 scripts/apple-silicon/input-scripts/noop.csv 60 25 10 postfast
```

`scripts/apple-silicon/sample-profile.sh` is a new harness that backgrounds
`run-benchmark.sh`, polls for the run dir + xemu pid, sleeps `WARMUP`
seconds, attaches Apple `sample` for `SAMPLE_DURATION` seconds, and writes
both the full sample text and a top-bucket summary to the run directory.
Fully autonomous; no user input.

Run dir: `benchmark-runs/20260501-132058-pgr2`
Sample file: `benchmark-runs/20260501-132058-pgr2/sample-postfast.txt`
Summary: `benchmark-runs/20260501-132058-pgr2/sample-postfast-summary.txt`

## Top-Level Thread Picture

`pfifo_thread` (NV2A renderer) is now visibly under-utilized: **41.5 % of
its time is spent on `_pthread_cond_wait` waiting for the next FIFO
command** because the CPU emulator can no longer keep it fed. That alone is
diagnostic — we are no longer renderer-bound at this scene.

The TCG i386 thread sample weight is dominated by `cpu_tb_exec` running
JIT-translated x86 code; mutex wait has collapsed from ~32 % to ~9 %.

## TCG Thread Inclusive Sample Counts (out of ~17,246)

| Bucket | Samples | % of TCG |
| --- | --- | --- |
| `cpu_tb_exec` (JIT x86 inclusive) | 16,884 | 97.9 % |
| `helper_lookup_tb_ptr` | 1,317 | 7.6 % |
| `do_st_mmio_leN` (CPU MMIO writes) | 1,612 | 9.4 % |
| `memory_region_dispatch_write` | 1,539 | 8.9 % |
| `qemu_mutex_lock_impl` (any lock) | 1,532 | 8.9 % |
| `_pthread_mutex_firstfit_lock_wait` | 1,519 | 8.8 % |
| **`voice_lock`** | **1,171** | **6.8 %** |
| `pgraph_write` | 242 | 1.4 % |

Floating-point helpers also stand out:

| Helper | Samples | Notes |
| --- | --- | --- |
| `helper_mulss` | 406 | SSE scalar single-precision multiply |
| `helper_fmul_ST0_FT0` | 341 | x87 80-bit multiply |
| `helper_mulps_xmm` | 312 | SSE packed single-precision multiply |
| `float32_mul` | 353 | softfloat 32-bit |
| `floatx80_mul` | 288 | softfloat 80-bit (x87) |
| `parts64_uncanon_normal` | 311 | softfloat normalize/denormalize |

(Inclusive samples include children, so subtree sums double-count. Use the
deltas between near-leaf entries to estimate exclusive cost.)

## Pfifo Thread Inclusive Sample Counts (out of ~17,236)

| Bucket | Samples | % of pfifo |
| --- | --- | --- |
| `pfifo_thread` (root) | 17,236 | 100 % |
| `pgraph_method` (real renderer work) | 9,417 | 54.6 % |
| `_pthread_cond_wait` (idle on FIFO) | 7,811 | 45.3 % |
| `pgraph_NV097_SET_BEGIN_END_handler` | 4,259 | 24.7 % |
| `pgraph_gl_draw_end` | 4,235 | 24.6 % |
| `pgraph_gl_draw_begin` | 4,078 | 23.7 % |
| `pgraph_gl_flush_draw` | 4,009 | 23.3 % |
| `glDrawElements_ACC_GL3Exec` | 3,516 | 20.4 % |
| `pgraph_gl_bind_textures` | 2,172 | 12.6 % |
| `gldRenderVertexArray` (Apple) | 1,913 | 11.1 % |
| `pgraph_gl_bind_shaders` | 1,615 | 9.4 % |
| `surface_download` (`glReadPixels`) | 1,357 | 7.9 % |

## What Changed Vs. Pre-Fast-Read

| Metric | Pre-fast-read | Post-fast-read | Change |
| --- | --- | --- | --- |
| TCG mutex wait | ~32.6 % | 8.9 % | -23.7 pp |
| `pgraph_read` mutex wait | 22.2 % | ~0 % | gone |
| `voice_lock` mutex wait | 6.9 % | 6.8 % | unchanged |
| `pgraph_write` mutex wait | 2.7 % | 1.4 % | -1.3 pp |
| `pfifo_thread` cond_wait (idle) | ~37 % | ~41.5 % | +4.5 pp |
| Snapshot post-load FPS | 16.56 | 30.76 | +85.8 % |

`pgraph_write` shrank because the renderer holds `pg->lock` for less
total time per draw now (no geometry-shader compilation/dispatch on the
critical path), so writes contend less. `voice_lock` is essentially
unchanged because nothing in the fast-read slice touched the audio voice
processor path.

## Implications For The 60 FPS Pursuit

PGR2 at this scene is now CPU-bound. The renderer thread is idle 41.5 % of
the time waiting for FIFO commands. Doubling FPS to 60 requires roughly
doubling TCG-thread throughput, with a hard ceiling on what lock-elision
alone can deliver.

**Bounded gains from remaining lock elision (PGR2-class scenes)**:

- `voice_lock` fast path: up to ≈ 6.8 % of TCG thread time recovered.
- `pgraph_write` fast path: up to ≈ 1.4 % recovered.
- `pgraph_gl_flush_draw` lock release: bounded by what fraction of the
  remaining 1.4 % `pgraph_write` wait is caused by the GL submission
  hold. Already small. The bigger value of releasing the lock is on
  scenes (Crimson) where pfifo is *not* idle and is itself blocking CPU
  MMIO traffic. Worth measuring there, not here.
- Total best case: ~8 % more FPS on PGR2/Rainbow scenes from
  lock-elision alone.

**Where the remaining 91 % goes**: real x86 emulation, with floating-point
helpers (SSE float32 mul, x87 80-bit ops) showing prominently. Possible
follow-ups:

1. Audit whether SSE single-precision ops are going through softfloat
   (`float32_mul`, `soft_f32_mul`) or hardfloat. On Apple Silicon, NEON
   float32 should be usable via QEMU hardfloat; if softfloat is in the
   path it is a major opportunity.
2. TB-chain audit: `helper_lookup_tb_ptr` accounts for ~7.6 % and the
   immediate descendant `qht_lookup_custom` at 1.1 %. If chaining is
   not happening as much as it could, the JIT loop spends more time in
   the dispatch helper than in real code.
3. Floating-point precision strategy: the Xbox CPU uses x87 (80-bit
   extended) registers. aarch64 has no native 80-bit float, so xemu
   uses softfloat for `floatx80_*`. If the games use SSE float32 paths
   primarily, those might be hardfloat-eligible; if they lean heavily
   on x87, we are stuck unless we accept a precision approximation.

## Next Slices

In rank order by expected FPS lift on PGR2/Rainbow class scenes:

1. **`XEMU_VOICE_FAST_LOCK=1`** — convert the safe single-uint32_t voice
   register write paths to `qatomic_set` so the audio voice worker's
   lock contention with the CPU thread is removed. Expected ≈ 6.8 % FPS.
2. **`XEMU_PGRAPH_FAST_WRITE=1`** — convert simple PGRAPH register
   default-slot writes to `qatomic_set`, leaving composite-state writes
   on the slow path. Expected ≈ 1-2 % FPS, but trivially safe and
   completes the read/write symmetry.
3. **Re-profile**, then decide between:
   a. `XEMU_PGRAPH_RELEASE_LOCK_DURING_GL=1` — only valuable on
      renderer-loaded scenes (Crimson), so should be measured there.
   b. SSE/x87 float-path hardfloat audit — large upside but more work
      and may not be safely applicable to all paths.
   c. Async shader compile to address Crimson stutters.

## Sample File

Run dir: `benchmark-runs/20260501-132058-pgr2`. Re-derive with:

```sh
XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1 \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 \
scripts/apple-silicon/sample-profile.sh \
  pgr2 scripts/apple-silicon/input-scripts/noop.csv 60 25 10 postfast
```
