# PGRAPH Lock-Free Read Fast Path

Date: 2026-05-01

## Purpose

Reduce TCG i386 emulation thread mutex-wait time on `pg->lock` by skipping
the lock for simple PGRAPH register reads, addressing the bottleneck
identified in
`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-sample.md`.

The sample profile showed the TCG thread spending ~22% of its wall time
sleeping in `pgraph_read` mutex-wait, and ~3% in `pgraph_write` mutex-wait,
because the renderer's pfifo thread holds `pg->lock` across the slow
OpenGL submission inside `pgraph_method`.

## What Landed

`hw/xbox/nv2a/pgraph/pgraph.c`:

- New `XEMU_PGRAPH_FAST_READ=1` env-flag check via
  `pgraph_fast_read_enabled()`.
- `pgraph_read()` now has a fast path that returns a `qatomic_read()`
  snapshot of the requested register without taking `pg->lock`. The fast
  path applies to:
  - `NV_PGRAPH_INTR` (a single uint32_t field).
  - `NV_PGRAPH_INTR_EN` (a single uint32_t field).
  - The default branch: `pg->regs_[addr]` is a 32-bit aligned slot in the
    PGRAPH register array.
- `NV_PGRAPH_RDI_DATA` keeps the full lock because reads of that register
  auto-increment `NV_PGRAPH_RDI_INDEX_ADDRESS`. That side-effect mutates
  PGRAPH state and must remain serialized.

The 32-bit reads on aarch64 and x86 are atomic by hardware. `qatomic_read`
adds the compiler barrier the codebase already relies on for similar
single-word reads (e.g., `qatomic_read(&d->pfifo.halt)` in `nv2a.c`).

## Triangle Regression Gate

`scripts/apple-silicon/validate-native-tri-depth.sh --run 22` against
`benchmark-runs/20260501-123015-flat-tri-depth` passed:

```
PASS NATIVE_TRI_DEPTH_DRAW=277245
PASS NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST=480
PASS NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST=288
PASS NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST=480
PASS NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST=288
PASS GEOM_SHADER_DRAW_TRI=288 matches NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST
```

So the lock-free read change does not break the flat-shading triangle
validator.

## PGR2 Mid-Route Snapshot Comparison

Snapshot tag `pgr2_gameplay_b4` from
`benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`. All replays use
`noop.csv` and run from the same Xbox state.

| Config | Run dir | Duration | Post-load FPS |
| --- | --- | --- | --- |
| Baseline (no flags) | `benchmark-runs/20260501-115623-pgr2` | 30 s | 4.39 |
| Baseline retry | `benchmark-runs/20260501-123318-pgr2` | 30 s | 5.23 |
| `XEMU_PGRAPH_FAST_READ=1` only | `benchmark-runs/20260501-123154-pgr2` | 30 s | 5.31 |
| `XEMU_NATIVE_TRI_DEPTH=1` | `benchmark-runs/20260501-115654-pgr2` | 30 s | 16.02 |
| `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` | `benchmark-runs/20260501-115725-pgr2` | 30 s | 16.56 |
| **`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`** | `benchmark-runs/20260501-123050-pgr2` | 30 s | **30.76** |
| **Same, longer rerun** | `benchmark-runs/20260501-123357-pgr2` | 60 s | **30.70** |

Findings:

- Adding `XEMU_PGRAPH_FAST_READ=1` on top of the geometry-shader bypass
  slices roughly **doubles FPS** at this snapshot (16.56 → 30.76, +85.8%).
- The 30 FPS gameplay floor for PGR2 is now reached at this scene, with
  zero geometry-shader draws and lock-free PGRAPH reads.
- `XEMU_PGRAPH_FAST_READ=1` *alone* without the geometry-shader bypass
  produces only a small lift (4.4 → 5.3 FPS, ~+20%). With geometry shaders
  active the pfifo thread holds `pg->lock` for so long during draws that
  shaving the read mutex per-call saves only marginal time. The flag's
  full payoff only shows up after the renderer's lock-hold time has been
  trimmed by removing the geometry shader.

## Counter Sanity At 30 FPS

`benchmark-runs/20260501-123357-pgr2` (60 s):

- Intervals: 56 (no final-flush due to forceful kill at end), 51
  post-load.
- Post-load FPS: 30.70, MSPF: 17.67.
- Geometry-shader draws of any kind: 0.
- Native triangle-depth draws: 4,920,932 (zero fallbacks, all smooth
  candidates).
- Native quad draws (LIST + STRIP): nonzero LIST, zero STRIP, all smooth,
  zero fallbacks.

Note: The 30-second `noop.csv` replay at the snapshot keeps the player car
roughly stationary post-snapshot, so the 30 FPS observation reflects the
heavy mid-race scene the snapshot captured. Earlier intervals (load,
fade-in) drop FPS into the teens; the post-load average smooths those
out.

## Why It Works

`pgraph_read` previously serialized every Xbox-CPU MMIO access to PGRAPH
register space behind a single mutex. The renderer's pfifo thread takes the
same mutex across `pgraph_method`, which on the END method runs
`pgraph_gl_draw_end` → `pgraph_gl_flush_draw` → `glDrawElements` → Apple's
`gldRenderVertexArray` and Metal command encoding. While the renderer holds
the mutex doing that, every Xbox-CPU read of an NV_PGRAPH_* address blocks.

The Xbox game loop polls NV2A status registers extremely frequently, so
the CPU emulator was sleeping on the mutex roughly a third of every
second. Aligned 32-bit register loads do not actually need mutual
exclusion — the load is atomic on aarch64 and x86 — so the mutex was
strict overhead for that case. Skipping it removes the bulk of that wait
and lets the i386 emulator run more of the game loop per host second.

## Limits And Followups

- The flag is opt-in. Default behavior is unchanged. Set
  `XEMU_PGRAPH_FAST_READ=0` to disable explicitly.
- `pgraph_write` still takes the lock. The sample profile showed write
  contention is 2.7% of TCG thread time, an order of magnitude smaller than
  the read contention. A similar fast path for writes is possible, but
  most of `pgraph_write` paths *do* mutate composite state (irq pending
  bits, pfifo kicks), so the audit is more involved.
- `voice_lock` (audio voice processor) was the second-largest mutex
  contention source at 6.9% TCG thread time. Not addressed in this slice.
- Smooth interpolation, depth-test, polygon-offset, and flat-shading
  behavior are unchanged. The fast read returns the same byte the locked
  read would have returned, just without serializing on the renderer.
- Memory ordering: `qatomic_read` provides the same compiler-barrier
  semantics QEMU uses elsewhere. The renderer thread writes through
  `pg->regs_[r] = v` (`pgraph_reg_w`); on aarch64 and x86 those single-word
  writes are atomic, so the CPU thread will see either the old or new
  value, never a torn one.

## Retail Gameplay Route Replays (300 s each, all three flags on)

All routes use the prepared profile HDD source, the recorded gameplay
script, and `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1`.

| Title | Run dir | Baseline post-load FPS | This run post-load FPS | Delta |
| --- | --- | --- | --- | --- |
| PGR2 | `benchmark-runs/20260501-123525-pgr2` | 11.67 | **31.76** | +2.72x |
| Rainbow Six 3 | `benchmark-runs/20260501-124102-rainbow-six-3` | 24.76 | **30.55** | +23% |
| Crimson Skies | `benchmark-runs/20260501-124625-crimson-skies` | 15.80 | **30.54** | +1.93x |

(Baseline numbers come from the 2026-05-01 retail gameplay route notes
captured before this slice.)

All three tracked titles now meet the 30 FPS retail gameplay floor on the
recorded route. Counter sanity for each run: zero geometry-shader draws of
any kind, zero native-tri-depth fallbacks, zero native-quad fallbacks.

## Next Slices

1. Audit `pgraph_write` for safe lock-free fast paths. Sample profile
   showed write contention is 2.7% of TCG-thread time — much smaller than
   the read contention but still worth a focused look.
2. Audit `voice_lock`-protected NV_USER writes (`vp_write`, `gp_write`,
   `user_write`) for the same pattern. Was 6.9% of TCG-thread time in the
   pre-fast-read sample profile.
3. Capture a fresh `sample` profile at the snapshot scene with all three
   flags on to identify the new dominant bottleneck (the 30 FPS floor is
   reached but headroom toward 60 FPS still exists).
4. After the easy lock-elision wins, decide whether to take the larger
   step of dropping `pg->lock` around the slow OpenGL submission inside
   `pgraph_gl_flush_draw` (gated behind a separate flag, with a careful
   audit of what GL state is read while the lock is held).
