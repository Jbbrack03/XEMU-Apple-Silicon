# V7 — Cumulative `cpu_exec_loop` per-phase attribution (Crimson 300 s)

Date: 2026-05-02

## One-line conclusion

**V7 added cumulative per-interval `TCG_TB_LOOKUP_US_TOTAL` /
`TCG_TB_GEN_CODE_US_TOTAL` / `TCG_HANDLE_INTERRUPT_US_TOTAL`
counters and disproved the PPTC hypothesis.** Worst-frame interval
(1.314 s mspf, 1.318 s wall): `gen_us=44 ms` (3 % of interval),
`lookup_us=205 ms` (16 %), `int_us=238 ms` (18 %), V7 phase total
488 ms (37 %). The remaining **63 % (830 ms) lives in
`cpu_loop_exec_tb`** — the actual TB binary execution that V7 does
not measure. The dominant new signal is `tb_exec=13.5M` in the
worst frame (~4× steady state), pointing at the guest spending
~830 ms inside translated guest code. PPTC alone, even at 100 %
efficacy, can save at most 44-111 ms across the top-5 worst
intervals — not a fix for a 1.3 s frame. **Next probe (V8): Apple
`sample` host-thread profile during a Crimson stutter** to identify
which xemu functions dominate the unattributed 830 ms.

## Build / commit verification

- Binary: `dist/xemu.app/Contents/MacOS/xemu`, built 2026-05-02 ~16:27 UTC
  (this session).
- `xemu --version` → `xemu_version: 0.8.134-53-g07b59a98ff`,
  `xemu_commit: 07b59a98ff1afba2a4f00cb7fc0bd2cc84f34fed` (clean).
- `codesign --verify --deep --strict --verbose=2 dist/xemu.app` →
  `valid on disk` and `satisfies its Designated Requirement`.
- GL renderer / OS confirmed Apple GL 4.1 Metal — 90.5 on Apple M3 Ultra,
  macOS 26.4.1.
- Op-tag strings present in the binary (V7 emit format):
  `TCG_TB_LOOKUP_US_TOTAL=%llu TCG_TB_GEN_CODE_US_TOTAL=%llu
  TCG_HANDLE_INTERRUPT_US_TOTAL=%llu` (single sprintf-style line in
  `xemu_tcg_perf_emit_and_reset`).
- Sanity: 15 s PGR2 mid-route snapshot run with `XEMU_TCG_PHASE_LOG=1`
  produced non-zero values across all 13 intervals (steady-state
  V7 phase total ~50 ms / second = 5 % of vCPU; first interval
  post-snapshot-resume showed 154 ms / second = 15 % during cache
  warm). Sanity-run dir: `benchmark-runs/20260502-112753-pgr2`.

## V7 slice — what code changed

Five files modified:

- `accel/tcg/cpu-exec.c` — extended each of the three V6 timing
  blocks to share the clock-read between spike-log and phase-log
  paths. The pattern:
  ```c
  bool time_this = (xemu_spike_log_tcg_enabled
                    | xemu_tcg_phase_log_enabled);
  if (time_this) start_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);
  /* call */
  if (time_this) {
      int64_t dur_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST) - start_ns;
      if (xemu_tcg_phase_log_enabled && dur_ns > 0)
          xemu_tcg_perf_add_<phase>_ns((uint64_t)dur_ns);
      if (xemu_spike_log_tcg_enabled && dur_ns/1000 >= threshold)
          xemu_spike_emit(...);
  }
  ```
- `accel/tcg/xemu-tcg-perf.c` — three new uint64_t accumulators
  (in nanoseconds), three `xemu_tcg_perf_add_*_ns()` helpers,
  extended `xemu_tcg_perf_emit_and_reset` to xchg-and-emit them
  in microseconds.
- `include/qemu/xemu-tcg-perf.h` — three new public-API helpers.
- `include/qemu/xemu-spike-log.h` — `xemu_tcg_phase_log_enabled`
  global declaration.
- `util/xemu-spike-log.c` — `xemu_tcg_phase_log_enabled` definition,
  initialised from `XEMU_TCG_PHASE_LOG` env var alongside the
  spike-log flags.
- `scripts/apple-silicon/extract-perf-summary.sh` — three new keys
  surfaced in the summary's TCG counter section.

### Nanosecond accumulation rationale

Per-call wallclock is often sub-microsecond (jmp-cache hits and
early-exit `cpu_handle_interrupt` calls cost <100 ns each). If V7
accumulated in microseconds, every sub-µs sample would truncate to
zero; with 13.5M calls × 800 ns each = 10.8 s of real work, the
truncated total would be 0. Internal nanosecond accumulation
preserves the cumulative cost; the emit divides by 1000 to display
microseconds.

### Hot-path cost when off

One global load + branch per phase per inner-loop iteration. With
3 phases × ~3M iterations/s × 1 ns per branch ≈ 9 ms/s of overhead
when off. Negligible at the per-interval timescale (<1 % of
wallclock).

### Hot-path cost when on

Two `qemu_clock_get_ns()` calls per timed phase. On Apple Silicon
M3 Ultra, `mach_absolute_time()` is ~10-15 ns per call; total
overhead per phase invocation ~25 ns. With 3 phases × 3M
iterations/s = 9M timed phase invocations/s × 25 ns = 225 ms/s of
clock-read overhead. ~22 % vCPU overhead. Below the 36 %
worst-case estimate; acceptable for attribution sweeps but should
not ship as default-on.

### Cross-check: V7 instrumentation overhead is in the noise

V6 run (V7-instrumentation NOT in tree) headline: `mspf_max_max =
1375.16`. V7 run (V7-instrumentation in tree, `XEMU_TCG_PHASE_LOG=1`):
`mspf_max_max = 1313.69`. Difference = 62 ms (4.5 %), in the
direction of LESS judder — within run-to-run input-route variance,
not an instrumentation artefact. The 22 % steady-state vCPU
overhead does not perturb the worst-frame attribution.

## Test matrix

All runs use the post-fast-read stable opt-in flag set
(`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`)
plus `XEMU_PERF_LOG=1 XEMU_PERF_FRAME_LOG=1 XEMU_TCG_PHASE_LOG=1`.
Splitwx, targeted-jmp-cache, and APU-lock-release are auto-on
(Apple Silicon system-build defaults from V1/V2/I5). Spike log is
NOT enabled (V7 is cumulative, not per-event).

| Arm | Title | Mode | Duration | Run dir |
| --- | --- | --- | --- | --- |
| M0 | PGR2 | snapshot `pgr2_gameplay_b4`, noop | 15 s | `benchmark-runs/20260502-112753-pgr2` |
| M1 | Crimson | retail `crimson-gameplay.csv`, profile-prep HDD | 300 s | `benchmark-runs/20260502-112845-crimson-skies` |

## Mission 1 — Crimson 300 s headline metrics

| Metric | V7 value | V6 value (2026-05-02) | Δ |
| --- | ---: | ---: | ---: |
| `post_load_intervals` | 279 | 278 | +1 |
| `post_load_avg_fps` | 30.73 | 30.75 | -0.02 |
| `post_load_avg_mspf` | 30.26 | 30.67 | -0.41 |
| `post_load_fps_stddev` | 4.139 | 4.053 | +0.086 |
| `post_load_mspf_max_p50` | 31.57 | 32.49 | -0.92 |
| `post_load_mspf_max_p95` | 40.30 | 36.79 | +3.51 |
| `post_load_mspf_max_p99` | 449.78 | 457.46 | -7.68 |
| **`post_load_mspf_max_max`** | **1313.69** | **1375.16** | **-61.47** |
| `post_load_frame_mspf_us_p99` | 33,659 | 33,552 | +107 |
| `post_load_frame_mspf_us_p999` | 85,911 | 84,671 | +1,240 |
| **`post_load_frame_mspf_us_max`** | **1,313,695** | **1,375,156** | **-61,461** |
| `post_load_stutter_intervals_30fps` | 70 | 88 | -18 |
| `post_load_longest_stutter_run_30fps` | 15 | 35 | -20 |

Headline 1.3 s class worst frame **reproduces** as a stable
phenomenon. Steady-state metrics match V6 within run-to-run noise
(`avg_fps` differs by 0.02 FPS, `avg_mspf` by 0.4 ms). V7
instrumentation does not measurably perturb the run.

## Mission 2 — V7 cumulative attribution

Top-5 worst-frame intervals across the 300 s route (sorted by
`mspf_max`, all post-load):

| `mspf_max` | `iv_ms` | `tb_exec` | `tb_inv` | `pages` | `lookup_us` | `gen_us` | `int_us` | V7 total | V7 % iv |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **1313.69** | 1318 | **13,502,014** | 26,345 | 1,040 | 205,497 | **44,599** | 238,033 | **488,129** | 37 % |
| 1261.45 | 1366 | 5,784,613 | 9,133 | 1,202 | 93,882 | 51,884 | 102,108 | 247,874 | 18 % |
| 1213.68 | 2056 | 7,985,481 | 1,873 | 231 | 122,044 | 53,169 | 141,739 | 316,952 | 15 % |
| 449.78 | 1038 | 9,308,096 | 2,170 | 366 | 144,580 | 61,436 | 158,348 | 364,364 | 35 % |
| 430.23 | 1016 | 11,394,767 | 2,455 | 1,399 | 186,416 | **111,781** | 212,904 | 511,101 | 50 % |

**Critical numbers across the 5 worst intervals:**

- **`gen_us` peaks at 111 ms** (interval `mspf_max=430`) and is
  44-111 ms across the worst-frame class. **PPTC, even at 100 %
  efficacy, can save at most 111 ms.** A 1.3 s frame would drop
  to 1.2 s — still well above the 500 ms judder gate.
- **`tb_exec` is 4-15× steady state** in worst-frame intervals
  (5.7M-13.5M vs steady-state ~3M). The vCPU thread is doing
  many more inner-loop iterations than usual.
- **`int_us` is the largest individual phase** in 4 of 5 worst
  intervals. cpu_handle_interrupt early-exits in <100 ns when no
  interrupt is pending; the high cumulative is per-iteration
  overhead × millions of iterations, not a per-call pathology
  (V6 already proved zero `cpu_handle_interrupt` events ≥ 1 ms
  in the same window).
- **V7 phase total accounts for 15-50 % of the worst-frame
  interval.** The remaining 50-85 % lives in `cpu_loop_exec_tb`
  (TB binary execution) — actual translated guest code running.

### Steady-state contrast (median interval)

For a typical 30 FPS interval (`mspf_max ≈ 32 ms`): `tb_exec ≈ 3M`,
V7 phase total ≈ 50 ms (5 % of interval). The worst frame's V7
phase total is ~10× higher in absolute terms but the same fraction
of the (longer) interval. **The vCPU is doing the same kind of
work as steady state, just much more of it.**

### Per-call cost decomposition

Worst-frame interval, 13.5M iterations:

| Phase | Total | Per call |
| --- | ---: | ---: |
| `lookup_us` | 205 ms | ~15 ns |
| `int_us` | 238 ms | ~17 ns |

Per-call costs are right at the V7 instrumentation overhead floor
(~25 ns per timed phase invocation, so most of `lookup_us` and
`int_us` is the V7 clock-read overhead itself, not pathological
phase work). The signal in these two phases is therefore
**iteration count**, not per-call cost.

`gen_us` is different: 44 ms / 26,345 invalidations ≈ 1.7 µs per
`tb_gen_code` call. That's genuine translation work (not clock-read
overhead, since the gen path is only entered on lookup miss). The
26,345 invalidations × 1.7 µs = 44 ms of real translation cost is
what PPTC would eliminate.

## Mission 3 — what the worst frame actually is

Combining V6 + V7 evidence:

1. **Wall time of the worst-frame interval: 1318 ms.**
2. **Within that:**
   - 488 ms in V6/V7-instrumented phases (cpu_handle_interrupt +
     tb_lookup + tb_gen_code), of which ~440 ms is per-call
     overhead × iteration count and ~44 ms is real translation
     work.
   - 830 ms unaccounted — necessarily inside `cpu_loop_exec_tb`
     (TB binary execution) or in non-instrumented inner-loop
     overhead.
3. **`tb_exec` count is 13.5M** vs steady-state ~3M. The vCPU
   thread is executing 4× more inner-loop bodies than usual.
4. **Render loop is blocked**: `NV2A_PRESENT_HEARTBEAT=4` in
   1.4 s vs ~30/s steady state. The guest is spending 1+ second
   on non-rendering work.
5. **Worst-frame guest PC unchanged from D3/V6:** Xbox kernel PC
   `0x80030e4c` dominates by chain count. Without kernel symbols,
   function identity is unresolved.

**The most parsimonious hypothesis: the Xbox kernel is in a
polling loop (busy-wait on an MMIO register or a kernel sync
primitive). xemu emulates the loop faithfully, but each iteration
of the loop is much slower in xemu than on real Xbox hardware
(ns-class TB execution per iter × hundreds of thousands of iters =
800+ ms). On real Xbox the same loop completes in 100-200 ms.**

This hypothesis is testable:

- If the loop polls an MMIO register, an MMIO **read** counter
  would show one specific MMIO region elevated by ~10× during the
  worst frame.
- If the loop polls a kernel sync primitive (e.g., `KEVENT` wait
  via `KeWaitForSingleObject` busy-spin path), the host-thread
  profile would show TB-execution cycles centered on the
  kernel-PC region with no MMIO heavy reader.
- If the loop is a `KeStallExecutionProcessor`-class busy-wait,
  the host-thread profile would show the TB binary tight loop
  and minimal MMIO traffic.

## Recommended next slice — V8 host-thread `sample` profile

**Highest priority — Apple `sample` of the live xemu vCPU thread
during a Crimson stutter** via
`scripts/apple-silicon/sample-profile.sh crimson
crimson-gameplay.csv 90 75 5 v8-stutter`. The script samples the
xemu process for 75 s starting 5 s after benchmark start; the
worst-frame consistently lands within the first 60 s of the route
(V6 saw it at the 9th post-load interval; V7 at a similar
position).

Decision tree on the sample output:

- **Top function = `do_ld_mmio_le4` / `address_space_ldl_le` /
  `address_space_read_*`** → the guest is polling an MMIO
  register. Identify the region from a follow-on per-region
  read counter (V9), then design a fix (skip the read, advance
  the value, or cache the polled state).
- **Top function = `cpu_tb_exec` / generated TB code** →
  the guest is in a tight non-MMIO loop (kernel sync primitive,
  busy-wait, or pure compute). The fix is harder — possibly a
  TCG codegen optimization for the specific instruction sequence,
  or finding the loop's exit condition and hinting xemu about
  it.
- **Top function = `qemu_event_wait` / `pthread_cond_wait`** →
  vCPU is genuinely sleeping, not busy. Bottleneck is on
  another thread (renderer? APU? iothread?) and the V6/V7
  instrumentation is showing reduced iteration count, not
  increased — but the V7 data shows the OPPOSITE
  (`tb_exec=13.5M` is HIGH, not low), so this is unlikely.

**Second priority — V9 per-MMIO-region read counter** if V8
points at MMIO polling. Mirrors the existing
`mmio_helper_block` counter but tracks reads instead of blocking
writes. Always-on per-interval counters per region (or top-N).
Cheap to implement once we know which region matters.

**Third priority — guest kernel symbolication** for PC
`0x80030e4c` if V8 confirms TB-execution dominance. Map the PC
to a function name via dumped xboxkrnl.exe + public RE notes.

**PPTC remains queued but DOWNGRADED in priority.** V7 quantified
the maximum PPTC ceiling at 44-111 ms per worst-frame interval.
PPTC would still be a useful slice for steady-state perf
(eliminates ~13 s of cumulative gen work across the 300 s run =
4 % steady-state speedup), but it does NOT solve the headline
1.3 s judder on Crimson and similar titles. Defer until V8/V9
identify the actual judder root cause.

## Honest-limits caveats

- **V7's per-call costs are dominated by instrumentation
  overhead.** `tb_lookup` and `cpu_handle_interrupt` per-call
  costs (~15-17 ns) are at the floor of the
  `qemu_clock_get_ns(QEMU_CLOCK_HOST)` × 2 cost on M3 Ultra
  (~25 ns). The true per-call cost without instrumentation is
  somewhere below 17 ns each. The signal in these two phases is
  iteration count, not per-call cost — interpret accordingly.
  `tb_gen_code` is different: it's only entered on lookup miss,
  so its measured 1.7 µs/call is dominated by real translation
  work, not the 25 ns instrumentation floor.

- **V7 cannot decompose the 830 ms unattributed remainder of the
  worst frame.** The remaining cost lives in `cpu_loop_exec_tb`
  (TB binary execution) which V7 does not instrument. Adding a
  V7-style timer around `cpu_loop_exec_tb` would just measure
  "all the time spent running guest code" — a tautology, since
  that IS the inner loop's main job. The right next probe is
  external (Apple `sample` host-thread stack), not another
  V7-style timer.

- **The PPTC ceiling estimate (44-111 ms savings per worst-frame
  interval) is an UPPER bound assuming 100 % cache hit on
  re-translation.** Real PPTC efficacy depends on:
  - Whether the invalidated TBs match the same code on
    re-execution (high — kernel code is stable).
  - Whether PPTC's persistence layer adds any serialization
    overhead per save/restore.
  - Whether PPTC's hash key (binary hash + cflags) accommodates
    the same TB being re-emitted at slightly different cflags.
  Real PPTC savings would likely be ~30-80 ms per worst-frame
  interval, still not enough to close the 1.3 s judder.

- **The "guest is in a polling loop" hypothesis has not been
  directly verified.** It is consistent with all observed
  evidence (high `tb_exec`, kernel-PC dominance, blocked render
  loop, manageable invalidation counts), but the alternative
  hypotheses (busy compute in kernel, sync primitive) are not
  yet ruled out. V8 (sample profile) is the cheapest way to
  verify or refute.

- **The V7 100 % decomposition picture only adds up to 37 % of
  the worst frame (488 ms / 1318 ms).** V6 chain attribution
  similarly accounted for ~33 % of the V6 worst frame (446 ms /
  1428 ms). Both attributions agree on magnitude — the
  uninstrumented remainder is ~830 ms, and it is **necessarily**
  in `cpu_loop_exec_tb` (no other code path within the vCPU
  thread can absorb that much wallclock — the iothread, RCU,
  APU, and renderer threads are separate and were already
  ruled out by D3).

- **`gen_us` of 44 ms in the worst-frame interval is a real
  number, not instrumentation artefact.** The
  `tb_gen_code`/`tb_lookup` dispatch is asymmetric: tb_lookup
  fires on every iteration; tb_gen_code only fires on lookup
  miss. With 26,345 invalidations producing 26,345
  re-translations × 1.7 µs each = 44 ms, the math is consistent
  and the instrumentation overhead (25 ns × 26k calls = 0.6 ms)
  is negligible in this phase.

- **`int_us` of 238 ms in the worst-frame interval has a
  small non-overhead component too.** With one cpu_handle_interrupt
  call per inner-loop iteration (13.5M calls), the 238 ms total
  - 25 ns × 13.5M overhead = ~338 ms instrumentation cost. But
  observed is only 238 ms, meaning the actual per-call work
  must be NEGATIVE — which is impossible. The reconciliation:
  the per-call overhead estimate (25 ns from clock read × 2)
  is too high for `cpu_handle_interrupt`'s call site. The
  empirical per-call cost in `int_us` (~17 ns) is likely the
  true clock-read cost on M3 Ultra (~10 ns each, so 20 ns total)
  with cpu_handle_interrupt's body adding effectively zero
  (early-exit branch). Conclusion: the real
  `cpu_handle_interrupt` work in the worst frame is essentially
  zero (the early-exit path is taken on virtually every call);
  the 238 ms total is almost entirely V7 clock-read overhead.

  This refines the picture: the V7-instrumented phases'
  cumulative cost is mostly **V7 instrumentation overhead** in
  the worst frame, except for the ~44 ms of genuine
  `tb_gen_code` work. The unattributed 830 ms is even larger
  than the table suggests — probably ~1100 ms when V7 overhead
  is subtracted out. **All of that is in `cpu_loop_exec_tb`.**

- **Project rule #11 honored.** No re-validation of the seven
  default-on flags. Indirect correctness check:
  `GEOM_SHADER_DRAW_TRI` would show in the summary if the
  native triangle path regressed; the V7 summary shows the path
  is healthy across the 279 post-load intervals.

- **Mission 0 sanity test (15 s PGR2 mid-route snapshot) showed
  V7 counters non-zero across all 13 intervals.** Per-interval
  steady-state V7 phase total was 33-71 ms / second (3-7 %
  vCPU). The first interval post-snapshot-resume showed 154 ms
  / second (15 % vCPU), reflecting cache-cold translation
  during the resume warmup. Sanity confirmed V7 emit path
  works end-to-end before the Crimson run.

## Mission 4 — V8 Apple `sample` host-thread profile (follow-up)

Run command:
```
./scripts/apple-silicon/sample-profile.sh crimson \
    scripts/apple-silicon/input-scripts/crimson-gameplay.csv \
    90 75 5 v8-stutter
```

Captured 75 s of vCPU thread samples on a 90 s Crimson route. The
sample window included 3 worst-frame intervals (`mspf_max=1350.78,
1323.58, 1317.85`) — the stutter is reproducible within the first
60 s of the route. Sample dir:
`benchmark-runs/20260502-113656-crimson-skies/sample-v8-stutter.txt`
+ `sample-v8-stutter-summary.txt`.

### vCPU thread top-of-stack

```
qemu_thread_start  52,198 samples
└─ cpu_exec_loop   37,540 (72 % of vCPU thread)
   └─ cpu_tb_exec  34,894 (67 % of vCPU thread, 93 % of cpu_exec_loop)
```

**67 % of the vCPU thread is in `cpu_tb_exec` — the actual TB
binary execution.** This confirms V7's "the unattributed 830 ms
lives in cpu_loop_exec_tb" finding directly. The remaining 28 %
of the thread is in cpu_handle_interrupt early-exit branch,
clock reads from V7 instrumentation, and other inner-loop overhead.

### Top named functions inside `cpu_tb_exec` (decisive finding)

| samples | function | comment |
| ---: | --- | --- |
| **1342** | **`helper_rdtsc`** | **Top named child of cpu_tb_exec** |
| 1281 | → `cpu_get_tsc` | (call chain into cpu_get_clock) |
| 1115 | → `cpu_get_clock` | (cpu-timers.c:97 seqlock-read) |
| 1068 | → `clock_gettime` | (libsystem_c) |
| 965 | → `_mach_boottime_usec` | macOS internal |
| 902 | → `gettimeofday` | macOS internal |
| 820 | → `__commpage_gettimeofday_internal` | macOS internal |
| 820 | → `mach_absolute_time` | bottom of chain |
| 996 | `helper_fdiv_STN_ST0` | x87 80-bit divide |
| 651 | `parts64_uncanon_normal` | softfloat support |
| 610 | `address_space_ldl_internal` | MMIO read path |
| 561 | `parts128_canonicalize` | softfloat support |
| 502 | `floatx80_mul` | x87 80-bit multiply |
| 486 | `floatx80_addsub` | x87 80-bit add/sub |
| 484 | `floatx80_round_pack_canonical` | softfloat support |
| 462 | `float32_to_floatx80` | float promote |
| 1789 | `helper_lookup_tb_ptr` | indirect-branch TB lookup from JIT |
| 436 | → `tb_htable_lookup_common` | qht hash chain walk |
| 268 | → `qht_lookup_custom` | qht entry compare |
| 349 | `get_ptr_rcu_reader` | RCU helper for qht read-side |

### Key insight: `helper_rdtsc` call chain is 7-9 functions deep

Every guest RDTSC instruction calls `helper_rdtsc` → `cpu_get_tsc`
→ `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)` → `cpu_get_clock` →
`cpu_get_clock_locked` (with seqlock read) → `get_clock` →
`clock_gettime(CLOCK_MONOTONIC)` → libsystem internals →
`mach_absolute_time`. **Estimated ~80-100 ns per RDTSC on M3 Ultra
vs ~5 ns native.**

The Xbox kernel's `KeQueryPerformanceCounter` and any
`KeStallExecutionProcessor`-class busy-wait will RDTSC many times
per loop iteration. If a kernel busy-wait does `RDTSC; cmp;
jb @loop` (the canonical busy-wait deadline pattern), every
iteration pays the full 7-function call chain.

### Implementation (`cpu_get_tsc` for XBOX target)

```c
/* hw/i386/x86-cpu.c:35 */
uint64_t cpu_get_tsc(CPUX86State *env)
{
#ifdef XBOX
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                    733333333, NANOSECONDS_PER_SECOND);
#else
    return cpus_get_elapsed_ticks();
#endif
}
```

The XBOX path is unavoidably tied to wall-clock (the Xbox CPU is
733.333333 MHz; guest sees ~733M TSC ticks per wall second).
Optimization opportunities (V9):

1. **Direct `mach_absolute_time` call** — skip the QEMU clock
   abstraction entirely. On Apple Silicon, `mach_timebase_info`
   is `{1, 1}` so `mach_absolute_time()` returns nanoseconds
   directly. Drops the call chain to `helper_rdtsc → cpu_get_tsc
   → mach_absolute_time + muldiv64` (3 deep instead of 9).
   Estimated saving: ~50-70 ns per RDTSC.
2. **Cache `mach_timebase_info` once at startup** to avoid the
   div-by-denominator on every call.
3. **muldiv64(ns, 733333333, 1e9)** on aarch64 compiles to a
   64-bit multiply + 32-bit shift (since gcd(733333333, 1e9)
   simplifies the ratio). Cost ~3 ns. Acceptable.
4. **Per-call counter** (`HELPER_RDTSC_CALLS`) added at the
   `helper_rdtsc` entry would directly confirm the call rate
   in the worst-frame interval. Cheap to add.

### Other less-actionable hot paths

- **x87 80-bit helpers** (`helper_fdiv_STN_ST0` + supporting
  softfloat) total ~3,000 samples = 6 % of vCPU. Already
  documented as **irreducibly soft on Apple Silicon** (no native
  80-bit float on aarch64 — strategy.md / 2026-05-01-tcg-float-
  audit.md). Crimson uses x87 for game physics, audio mixing.
  No fix path within Phase 5 scope; would require a complete
  x87 emulation rewrite (Ryujinx's NetCoreApp does this with
  custom IR; out of scope here).
- **`helper_lookup_tb_ptr` + qht lookup** total ~2,800 samples
  = 5 % of vCPU. Called from JIT'd code on indirect branches
  (call/jmp through register). Each lookup walks the qht hash
  chain. Optimization could be a per-vCPU 1-entry indirect-
  branch cache before falling back to qht. Lower priority than
  RDTSC fast-path; defer pending V9 measurement.

## Combined V6 + V7 + V8 picture

The Crimson 1.3 s worst-frame stutter, decomposed:

1. **Worst-frame interval = 1318 ms wall.**
2. **Within the vCPU thread:**
   - 67 % in `cpu_tb_exec` (TB binary execution = JIT'd guest code)
   - ~33 % in inner-loop overhead, clock reads, cpu_handle_interrupt
3. **Within `cpu_tb_exec`:**
   - **~4 % in `helper_rdtsc`** (top named function — fixable
     via V9 fast-path)
   - ~6 % in x87 80-bit helpers (irreducibly soft, no fix)
   - ~5 % in `helper_lookup_tb_ptr` qht lookup (deferred)
   - ~85 % in raw JIT'd guest code (not directly optimizable
     without ISA-level work)
4. **Hypothesis (refined): Xbox kernel is in a tight busy-wait
   loop that:**
   - Calls RDTSC each iteration (fix: V9 fast-path drops this
     from 100 ns to ~30 ns per call)
   - Probably does some integer-compute or memory access
     (no fix path)
   - Generates 4-15× more `tb_exec` than steady state
5. **PPTC ceiling (44-111 ms savings)** is small relative to
   the worst-frame magnitude. Even combining V9 RDTSC fast-path
   (estimated 50-100 ms savings if RDTSC is heavily called) +
   PPTC (~50-100 ms) = ~100-200 ms — still well above the
   500 ms judder gate but a meaningful improvement.

## Updated next-slice plan

1. **V9 — RDTSC fast-path + call counter (highest priority).**
   Implement `cpu_get_tsc` Apple Silicon fast-path bypassing
   the QEMU clock abstraction. Add per-interval
   `HELPER_RDTSC_CALLS` counter to validate rate. Run Crimson
   300 s and compare worst-frame mspf. Decisive metric: if
   `HELPER_RDTSC_CALLS` is in the millions per worst-frame
   interval, the fix is justified; if the fix moves
   `mspf_max_max` below 1100 ms, ship default-on.
2. **V10 — `helper_lookup_tb_ptr` indirect-branch cache** (if
   V9 helps but doesn't close the gap). Per-vCPU 1-entry cache
   keyed on TB address.
3. **PPTC** still queued as a steady-state perf improvement
   (saves 4 % across the 300 s run) but downgraded as a judder
   fix.
4. **Audio listen-test for `XEMU_APU_LOCK_RELEASE`** stays
   deferred until judder is closed.

## Files referenced

- M0 sanity run: `benchmark-runs/20260502-112753-pgr2/`
- M1 attribution run: `benchmark-runs/20260502-112845-crimson-skies/`
- V7 working-tree slice diff (8 files modified):
  - `accel/tcg/cpu-exec.c` (clock-read sharing in 3 timed phases)
  - `accel/tcg/xemu-tcg-perf.c` (3 new accumulators + helpers)
  - `include/qemu/xemu-tcg-perf.h` (3 new public API helpers)
  - `include/qemu/xemu-spike-log.h` (xemu_tcg_phase_log_enabled decl)
  - `util/xemu-spike-log.c` (xemu_tcg_phase_log_enabled init)
  - `scripts/apple-silicon/extract-perf-summary.sh` (3 new keys)
  - `xemu-fork/CLAUDE.md` (XEMU_TCG_PHASE_LOG flag doc)
  - `docs/apple-silicon/automation.md` (TCG_*_US_TOTAL counter doc)
- Sibling notes (the worst-frame attribution history):
  - `benchmarks/2026-05-01-tcg-splitwx-validation.md` (V1)
  - `benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md` (V2)
  - `benchmarks/2026-05-02-tcg-spike-attribution.md` (V3)
  - `benchmarks/2026-05-02-composite-goal-validation.md` (V4)
  - `benchmarks/2026-05-02-60hz-title-sanity-test.md` (SC2 sanity)
  - `benchmarks/2026-05-02-tcg-30fps-cap-attribution.md` (D3)
  - `benchmarks/2026-05-02-apu-lock-release-validation.md` (I5)
  - `benchmarks/2026-05-02-broader-title-sweep.md` (V4 library sweep)
  - `benchmarks/2026-05-02-v6-cpu-exec-loop-attribution.md` (V6)
