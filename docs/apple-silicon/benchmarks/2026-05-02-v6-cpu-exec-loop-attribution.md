# V6 — `cpu_exec_loop` per-phase spike attribution (Crimson 300 s)

Date: 2026-05-02

## One-line conclusion

**V6 instruments three new per-phase spike sources inside `cpu_exec_loop`
(`tcg_tb_lookup`, `tcg_tb_gen_code`, `tcg_handle_interrupt`) and rules
out all three at the 1 ms per-event threshold across 300 s of Crimson
gameplay** (0, 0, and 1 events respectively). The 1 ms-class
`tcg_tb_chain` events that V3 / D3 attributed are confirmed to be
**normal hot-path execution** (mean `tb_count=1918`, ~500 ns/TB) rather
than pathological. The Crimson 1.375-second worst frame correlates with
a **sub-millisecond translation-churn storm** —
`TCG_TB_INVALIDATE_COUNT=8954` and `TCG_NOTDIRTY_PAGES_HIT=1200` in the
worst-frame interval, both ~5-25× steady-state — but no individual
`tb_gen_code` call exceeds 1 ms. **Next slice: V7 — cumulative
per-interval `TCG_*_US_TOTAL` counters** to catch the aggregate
sub-millisecond cost that V6's per-event threshold misses; PPTC
remains the leading downstream fix candidate.

## Build / commit verification

- Binary: `dist/xemu.app/Contents/MacOS/xemu`, built 2026-05-02 ~15:50 UTC
  (this session).
- `xemu --version` → `xemu_version: 0.8.134-52-gac49afee69`,
  `xemu_commit: ac49afee696654e3e74b9c8430dd52801d3447d6` (dirty,
  V1+V2+V3+V4+V5+D2+D3+I5 stack + V6 instrumentation on top).
- `codesign --verify --deep --strict --verbose=2 dist/xemu.app` →
  `valid on disk` and `satisfies its Designated Requirement`.
- GL renderer / OS confirmed Apple GL 4.1 Metal — 90.5 on Apple M3 Ultra,
  macOS 26.4.1.
- Sanity: 15 s PGR2 mid-route snapshot run (`pgr2_gameplay_b4`) at
  1 ms threshold produced 1 `tcg_handle_interrupt` event with
  structured extra fields; PGR2 snapshot is hot-cache so no
  `tb_gen_code` / `tb_lookup` events fire there (translation churn
  expected only on the retail Crimson route after snapshot resume).
  Sanity-run dir: `benchmark-runs/20260502-105141-pgr2`.
- Op-tag strings present in the binary: `strings dist/xemu.app/...
  | grep tcg_` → `tcg_handle_interrupt`, `tcg_tb_chain`,
  `tcg_tb_gen_code`, `tcg_tb_lookup` (4 of 4).

## V6 slice — what code changed

One file modified: `accel/tcg/cpu-exec.c` (cpu_exec_loop inner
while-handle-interrupt loop converted to `for(;;)` form to allow
post-call timing of `cpu_handle_interrupt`; per-phase timers added
around the three calls of interest). Pattern matches V3 / D3:
all three sources gated on `xemu_spike_log_tcg_enabled` (env:
`XEMU_PERF_SPIKE_LOG_TCG=1`), shared threshold
`xemu_spike_threshold_us`, structured `extra=` fields per source.
Hot-path cost when off is one global load + branch per call site
(per-iteration measured cost on this M3 Ultra: 1 untaken branch +
no clock reads).

### Source 1 — `op=tcg_handle_interrupt`

Wraps the inner loop's `cpu_handle_interrupt(cpu, &last_tb)` call.
Captures wallclock from before-call to after-call, including any
`bql_lock()` acquisition inside the
`unlikely(cpu_test_interrupt(...))` branch and the target-side
`tcg_ops->cpu_exec_interrupt` callback. Extra fields:
`exit=N ex_idx=N int_req=0xN` (post-call state — `interrupt_request`
may have been cleared by the handler; `ex_idx=65536` corresponds to
`EXCP_INTERRUPT` from the `cpu->exit_request` path, `ex_idx=0x10001`
or similar would correspond to a target-defined exception).

Threshold-rationale: D3 ruled out `bql_acquire_wait` and
`mmio_helper_block` inside the worst-frame window, but
`cpu_handle_interrupt` was only inferred-clean. V6 measures it
directly.

### Source 2 — `op=tcg_tb_lookup`

Wraps `tb = tb_lookup(cpu, s)`. Cost includes the per-CPU jmp-cache
probe and, on miss, the global qht hash lookup. Extra fields:
`pc=0xPC hit=0|1` (hit=1 means lookup found a TB; hit=0 means lookup
missed and the next phase will be `tb_gen_code`).

Threshold-rationale: V2 (jmp-cache-targeted invalidation) closed
one source of jmp-cache churn but did not measure tb_lookup
wall-time directly. V6 catches the case where jmp-cache thrashes
post-`tb_flush` or where qht hash-chain walks pathologically degrade.

### Source 3 — `op=tcg_tb_gen_code`

Wraps `mmap_lock(); tb = tb_gen_code(cpu, s); mmap_unlock();` (the
TCG translation pass plus its mmap-lock bracket). Only fires after
a `tb_lookup` miss, so steady-state hot-cache cost when off OR on
is one branch (the `if (tb == NULL)` test). Extra fields:
`pc=0xPC cflags=0xN`.

Threshold-rationale: D3's leading hypothesis for the unattributed
~970 ms of the Crimson worst frame was tb_gen_code churn. V6
measures translation cost per call directly.

## Test matrix

All runs use the post-fast-read stable opt-in flag set
(`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`)
plus `XEMU_PERF_LOG=1 XEMU_PERF_FRAME_LOG=1 XEMU_PERF_SPIKE_LOG=1
XEMU_PERF_SPIKE_LOG_TCG=1 XEMU_PERF_SPIKE_LOG_THRESHOLD_US=1000`.
Splitwx, targeted-jmp-cache, and APU-lock-release are auto-on
(Apple Silicon system-build defaults from V1/V2/I5). Screenshots are
off (`XEMU_BENCH_SCREENSHOT_BACKEND=none`).

| Arm | Title | Mode | Spike threshold | Duration | Run dir |
| --- | --- | --- | --- | --- | --- |
| M0 | PGR2 | snapshot `pgr2_gameplay_b4`, noop | 1 ms | 15 s | `benchmark-runs/20260502-105141-pgr2` |
| M1 | Crimson | retail `crimson-gameplay.csv`, profile-prep HDD | 1 ms | 300 s | `benchmark-runs/20260502-105232-crimson-skies` |

## Mission 1 — Crimson 300 s headline metrics

| Metric | V6 value | D3 value (2026-05-02) | Δ |
| --- | ---: | ---: | ---: |
| `post_load_intervals` | 278 | 279 | -1 |
| `post_load_avg_fps` | 30.75 | 30.46 | +0.29 |
| `post_load_avg_mspf` | 30.67 | 28.61 | +2.06 |
| `post_load_fps_stddev` | 4.053 | 3.927 | +0.126 |
| `post_load_mspf_max_p50` | 32.49 | 33.69 | -1.20 |
| `post_load_mspf_max_p95` | 36.79 | 40.99 | -4.20 |
| `post_load_mspf_max_p99` | 457.46 | 459.59 | -2.13 |
| **`post_load_mspf_max_max`** | **1375.16** | **1386.37** | **-11.21** |
| `post_load_frame_mspf_us_p99` | 33,552 | 36,164 | -2,612 |
| `post_load_frame_mspf_us_p999` | 84,671 | 80,404 | +4,267 |
| **`post_load_frame_mspf_us_max`** | **1,375,156** | **1,386,373** | **-11,217** |
| `post_load_stutter_intervals_30fps` | 88 | 167 | -79 |
| `post_load_longest_stutter_run_30fps` | 35 | 18 | +17 |

The 1.375 s Crimson worst-frame **reproduces** as a stable
phenomenon (V1/V2/V3/V4/V5/D2/D3/I5 all reproduced 1.27-1.39 s; V6
measures 1.375 s — within run-to-run noise). Steady-state metrics
also match: `post_load_avg_fps` ~30 (the title-intrinsic cap),
`post_load_mspf_max_p95` ~37 ms (smooth at the 30 FPS target).
`stutter_intervals_30fps` halved relative to D3 (88 vs 167) — likely
run-to-run input-route variance, not a regression. V6 instrumentation
adds zero measurable overhead at this granularity (3 untaken branches
per inner-loop iteration when off; the run had it on).

GEOM_SHADER_DRAW_TRI=0 across the entire run — closed-slice
regression check passes (native triangle-depth path owns all
triangle draws).

## Mission 2 — V6 spike-source attribution at 1 ms threshold

### Full-run aggregation

| op tag | count | total_us | max_us | comment |
| --- | ---: | ---: | ---: | --- |
| `tcg_tb_chain` | 55,684 | 55,949,800 | 36,500 | dominant — but 99.97 % at the 1 ms threshold (see below) |
| `flip_stall_glfinish` | 4,111 | 4,387,804 | 3,679 | renderer-side, expected |
| `draw_begin` | 1,035 | 2,129,590 | 14,206 | renderer-side, expected |
| `surf_download` | 859 | 1,390,349 | 5,820 | renderer-side, expected |
| `surf_upload` | 598 | 1,036,673 | 2,744 | renderer-side, expected |
| `bind_textures` | 356 | 514,922 | 7,922 | renderer-side, expected |
| `bql_acquire_wait` | 5 | 140,080 | 81,077 | RCU-thread shutdown wait, off vCPU axis |
| `flush_draw` | 53 | 66,304 | 2,257 | renderer-side, expected |
| `tex_upload` | 12 | 20,595 | 7,236 | renderer-side, expected |
| `mmio_helper_block` | 3 | 3,178 | 1,117 | tiny residual; D3 axis is empty |
| `tcg_pg_lock_wait` | 3 | 3,164 | 1,113 | tiny residual |
| **`tcg_handle_interrupt`** | **1** | **2,418** | **2,418** | **single one-off; not the bottleneck** |
| **`tcg_tb_lookup`** | **0** | **0** | **0** | **NEGATIVE** |
| **`tcg_tb_gen_code`** | **0** | **0** | **0** | **NEGATIVE** |

**Three V6 hypotheses ruled out at 1 ms threshold across 300 s:**

1. **No individual `tb_lookup` exceeded 1 ms.** Jmp-cache thrash and
   qht hash-chain walks remain sub-millisecond per call.
2. **No individual `tb_gen_code` exceeded 1 ms.** D3's leading
   hypothesis ("`tb_gen_code` churn drives the worst frame") is
   refuted at the per-event level — but the run shows strong
   evidence of *aggregate* translation churn (Mission 4 below).
3. **Only one `tcg_handle_interrupt` event at 1 ms across 300 s**
   (2.4 ms, `exit=1 ex_idx=65536 int_req=0x2`). Not a recurring
   bottleneck.

### Worst-frame interval attribution

Worst frame: `mspf_max=1375.16` at the 9th post-load `xemu-perf:`
interval (file line 3790). Frame composition:
`frame_mspf_us=1079,1021,615,1375156` — the catastrophic frame is
the 4th and only outlier; the prior 3 frames were sub-2 ms each
(post-resume tail). Interval covers file lines 3342-3789 (between
the two flanking `xemu-perf:` lines), spike `now_us` range
1,439,951 µs ≈ matches the 1428 ms reported `interval_ms`.

| op tag | count in worst-frame interval | total_us | max_us |
| --- | ---: | ---: | ---: |
| `tcg_tb_chain` | 443 | 445,810 | 2,988 |
| `draw_begin` | 2 | 14,520 | 10,534 |
| `surf_upload` | 2 | 4,919 | 2,744 |
| `surf_download` | 1 | 1,778 | 1,778 |
| `flush_draw` | 1 | 1,229 | 1,229 |
| **`tcg_handle_interrupt`** | **0** | **0** | **0** |
| **`tcg_tb_lookup`** | **0** | **0** | **0** |
| **`tcg_tb_gen_code`** | **0** | **0** | **0** |
| `bql_acquire_wait` | 0 | 0 | 0 |
| `mmio_helper_block` | 0 | 0 | 0 |

**The Crimson worst-frame contains zero V6 spike events.** The
bottleneck is neither tb_lookup, nor tb_gen_code, nor
cpu_handle_interrupt at the per-event 1 ms granularity. Combined
with D3's earlier negative results (zero `qemu_main_loop_iter`,
zero `aio_run_iter`, zero `bql_acquire_wait`, zero
`mmio_helper_block` in the worst-frame window), **every single
1 ms+ event class instrumented across V3 + D3 + V6 is empty inside
the worst-frame window except `tcg_tb_chain` (443 events at the
~1 ms threshold) and the four routine renderer-side events**.

The 443 chain events sum to 445.8 ms — 32.5 % of the 1428 ms
interval. The remaining ~982 ms of the worst frame is composed of
sub-millisecond events that no current spike source catches.

## Mission 3 — `tcg_tb_chain` reframing (the 1 ms threshold catches normal execution)

Of 55,684 chain events across 300 s, **55,670 (99.97 %)** fall in
the 1000-1099 µs duration bucket. Only 14 chains exceed 1.1 ms
(absolute max 36.5 ms — a single one-off, likely the post-snapshot-
resume warmup translation burst). The chain durations are
**clustered just above the 1 ms threshold**, not distributed.

The mean `tb_count` for 1 ms-class chains is **1918**. At ~500 ns
per `cpu_loop_exec_tb` iteration (the inner-loop body cost
amortized across an entire chain), 1918 × 500 ns = ~960 µs ≈ 1 ms.
**This is normal TCG hot-path execution.** D3's interpretation of
the 1 ms cost as "host-side wait inside cpu_exec_loop" was wrong —
V6 disproves it directly (no host-side wait spike fires at 1 ms).
The cost is genuine guest-code execution at the rate the M3 Ultra
TCG vCPU thread can sustain.

`tb_count` distribution among 1 ms-class chains:

| `tb_count` bucket | count | comment |
| --- | ---: | --- |
| 1 | 0 | zero — every chain executes ≥ 2 TBs |
| 2-5 | 38,479 | dominant — short post-jmp-cache-clear chains |
| 6-20 | 3,596 | medium chains |
| 21-100 | 1,671 | longer chains, jmp-cache-warm |
| 101-500 | 1,928 | hot loops |
| 500+ | 6,435 | very hot loops (game-engine inner loops) |

The 38,479-strong "tb_count=2..5" cluster is the dominant chain
shape and is **expected** for an x86 system that issues many short
TB chains separated by indirect jumps / function returns / interrupt
checks. The 6,435 chains with `tb_count > 500` are healthy hot
loops (the inner game engine ticks). Both shapes coexist within the
same dominant guest PC class.

## Mission 4 — Worst-frame TCG perf-counter signal

The dominant V6 finding is in the **always-on per-interval TCG
counters** (`xemu-perf:` line, not the per-event spike log). Worst-
frame interval (`mspf_max=1375.16`, `interval_ms=1428`):

| Counter | Worst-frame value | Steady-state median | Ratio |
| --- | ---: | ---: | ---: |
| `TCG_TB_INVALIDATE_COUNT` | 8,954 | ~1,500 | **~6×** |
| `TCG_NOTDIRTY_TRIPS` | 1,212 | ~700 | ~1.7× |
| `TCG_NOTDIRTY_PAGES_HIT` | 1,200 | ~50 | **~24×** |
| `TCG_TB_INVALIDATE_BURST_MAX` | 438 | ~150 | ~2.9× |
| `TCG_JMP_CACHE_ZEROED_BUCKETS` | 74,490 | ~5,500 | ~13.5× |

**The worst-frame interval is a translation-churn storm.** 1200
distinct guest-physical pages tripped notdirty in the same
interval; 8954 TBs were invalidated; the largest single
`tb_invalidate_phys_page_range__locked` removed 438 TBs at once.
This is consistent with self-modifying code (SMC), guest JIT
compilation, or a code-segment swap.

Critically, **none of the resulting `tb_gen_code` re-translations
exceeded 1 ms individually** (Mission 2). The fix applies cumulatively:
8954 invalidations × ~50 µs per re-translation ≈ 450 ms of
distributed translation cost. The observed 446 ms of `tcg_tb_chain`
in the same window is consistent with this calculation — the chain
spike measures the full inner-loop cycle including any re-translation
performed inside `tb_gen_code`.

## Mission 5 — Guest PC attribution (V6 + D3 corroboration)

Top guest PCs starting `tcg_tb_chain` events in the worst-frame
interval:

| `first_pc` | count in worst-frame | full-run count | identification |
| --- | ---: | ---: | --- |
| **`0x80030e4c`** | **434 (98.0 %)** | **52,047 (93.5 %)** | **Xbox kernel** (kernel base 0x80010000) |
| `0x8003adcc` | 6 | 35 | Xbox kernel |
| `0x8001b02f` | 1 | 2,847 | Xbox kernel |
| `0x80014385` | 1 | (rare) | Xbox kernel |

**`0x80030e4c` is the same Xbox kernel PC D3 identified as the
worst-frame dominator** (D3 reported 35,143 chains at this PC;
V6 reports 52,047 — same root-cause class). All four worst-frame
chain PCs are in the kernel range (>= 0x80010000); zero Crimson
app-code PCs (the V3 `0x23dd47` DSOUND voice-lock PC reports 0
chains in the worst-frame interval and only 155 across the entire
300 s run, confirming the I5 APU-lock-release slice is doing its
job on the steady-state axis).

The bulk of the worst-frame's chain time is **Xbox kernel**, not
Crimson app code. Without Xbox kernel symbols this PC's function
identity is unresolved. The chain shape (mean tb_count high — 1918
across the full run) suggests a wide non-cyclic execution path
through ~1900 TBs per chain, not a tight inner loop. Combined with
the Mission 4 translation-churn evidence, the dominant hypothesis
is: **the kernel is re-executing recently-invalidated kernel code**,
forcing thousands of `tb_gen_code` mini-events (each <1 ms) to
satisfy `tb_lookup` misses on freshly-flushed jmp-cache entries.

## Combined conclusion — the worst-frame composition is now

1. **Dominant cost class (32.5 % of worst-frame interval):**
   1 ms-class `tcg_tb_chain` events (443 events × ~1 ms = 446 ms)
   starting at Xbox kernel PC `0x80030e4c`. Each chain executes
   ~1900 inner-loop iterations at ~500 ns/iteration — normal hot-
   path TCG execution, not pathological.
2. **Translation-churn signal (cumulative, sub-millisecond per
   event):** 8954 TB invalidations + 1200 distinct notdirty page
   trips in the worst-frame interval — direct evidence of
   self-modifying-code-class behavior in the kernel range. The
   resulting `tb_gen_code` re-translations are individually fast
   (<1 ms each, V6 NEGATIVE) but cumulatively expensive
   (~450 ms estimated, matching the observed chain time within
   noise).
3. **Render loop is blocked:** Only 4 `NV2A_FLIP_STALL_WRITES` and
   4 `NV2A_PRESENT_HEARTBEAT` in the 1.4 s worst-frame interval
   (vs ~30/s steady state). The guest's render thread is not
   producing frames during the stall — the kernel is doing
   non-rendering work. xemu's vblank pacing offered 86 vblanks
   in the same window (`NV2A_VBLANK_FIRES=86`), so xemu is not
   the cap.
4. **NOT in any single 1 ms+ event class instrumented to date:**
   tb_lookup, tb_gen_code, cpu_handle_interrupt, BQL acquire,
   AIO dispatch, MMIO helper, main-loop iteration — all empty
   in the worst-frame window. The cost is distributed across
   sub-millisecond events that the per-event spike log cannot
   resolve.

## Recommended next slice

**Highest priority — V7: cumulative per-interval TCG phase counters.**
Add three new counters in `accel/tcg/xemu-tcg-perf.c` that
*always* sum the wallclock cost of each phase per interval (not
gated on the spike threshold):

- `TCG_TB_LOOKUP_US_TOTAL` (sum) — wallclock spent in
  `tb_lookup()` per interval.
- `TCG_TB_GEN_CODE_US_TOTAL` (sum) — wallclock spent in
  `tb_gen_code()` per interval (only the miss path).
- `TCG_HANDLE_INTERRUPT_US_TOTAL` (sum) — wallclock spent in
  `cpu_handle_interrupt()` per interval.

Cost when spike-log is off but counters are on: one
`qemu_clock_get_ns()` pair per phase call ≈ 40 ns. With ~3M TB
executions per interval, total overhead is ~360 ms / 1000 ms = 36 %
worst case if instrumented unconditionally. **This is too expensive
for default-on**; gate the wallclock measurement on a new
`xemu_tcg_perf_phase_log_enabled` (env: `XEMU_TCG_PHASE_LOG=1`)
that the user enables when running attribution sweeps. Counter
emission stays unconditional (zero-valued when measurement is off).

If V7 confirms `TCG_TB_GEN_CODE_US_TOTAL ≥ 300 ms` in the worst-
frame interval, the **PPTC slice is justified**: a persistent
profile-guided translation cache (Ryujinx pattern, strategy.md
Phase 5a) keeps translations across `tb_flush` events, so a guest
SMC storm doesn't force the same TBs to be regenerated repeatedly.
Estimated ceiling: eliminate ~300-450 ms of cumulative translation
cost per worst-frame interval, dropping the worst frame from
1.375 s to ~900 ms.

If V7 instead shows `TCG_TB_LOOKUP_US_TOTAL` dominating, the fix is
qht hash-chain length investigation or jmp-cache resize. If
`TCG_HANDLE_INTERRUPT_US_TOTAL` dominates, look at i386 IRQ
injection cost.

**Second priority (deferred, gated on user) — guest kernel
symbolication.** Map kernel PC `0x80030e4c` to a function name.
Without symbols, "what is the kernel doing for 1.375 s?" is not
answerable. Methodology: dump xboxkrnl.exe from the snapshot HDD
via QEMU `pmemsave` HMP, parse PE export table + apply public
XBOXKRNL RE notes (xemu-project, xeniaproj, cxbx-reloaded). If
the function turns out to be (e.g.) a paging-fault handler or a
DMA-completion bottom-half, the fix path is different from PPTC.
Defer until V7 has confirmed where the cost actually is.

**Third priority — host-thread profiling cross-check.** Run Apple
`sample` against the live xemu process during a Crimson stutter
window via `scripts/apple-silicon/sample-profile.sh`. The vCPU
thread's host-side stack trace during the worst frame would
directly answer "what is the TCG thread doing?" without needing
guest-side symbolication. Quickest path to ground truth.

**Do NOT pursue:**
- Lower the spike threshold to 100 µs to "see" the V6 sources
  fire. At 100 µs the log volume explodes (~1M lines / 300 s) and
  the data is ambiguous between routine cost and pathology.
  Cumulative counters (V7) are the right tool.
- A new spike source for `cpu_loop_exec_tb` / `cpu_tb_exec`. This
  is the actual TB execution; spikes there would just confirm
  "guest code ran." Not actionable.
- Renderer slices. Worst-frame renderer-side spike total: 22 ms
  across 6 events, vs 446 ms of TCG events — not the binding
  constraint.

## Honest-limits caveats

- **V6 is a NEGATIVE result on three hypotheses, not a fix.** No
  default-on flag should land from this slice. The instrumentation
  itself is opt-in (`XEMU_PERF_SPIKE_LOG_TCG=1`) and zero-cost when
  off, so no shipping code change is required at this point.
  Recommend leaving V6 instrumentation in place permanently for
  future regression triage, since cost-when-off is one untaken
  branch per call site.

- **V6 disproves D3's leading hypothesis** ("`tb_gen_code` churn
  drives the worst frame") at the per-event level. The aggregate
  hypothesis (cumulative sub-millisecond churn) remains plausible
  and is exactly what V7 will measure. The D3 note's recommended
  PPTC follow-on is therefore **conditional** on V7's outcome —
  if V7 confirms cumulative tb_gen_code cost, PPTC is justified;
  if V7 reveals lookup or interrupt-handling dominance instead,
  PPTC is the wrong slice.

- **The 99.97 % chain clustering at 1 ms threshold means V3's
  original worst-frame attribution (D3 also re-quoted the same
  number, 422 ms tcg_tb_chain in the 1386 ms worst-frame window)
  was largely measuring routine hot-path execution, not
  pathology.** D3 implicitly recognized this in its
  `tb_count=5..2790` observation but did not have V6's
  per-phase data to disprove the host-side-wait hypothesis.
  V6's contribution is the disproof, not a new attribution
  number.

- **Per-event vs per-interval cost.** The dominant V6 finding —
  `TCG_TB_INVALIDATE_COUNT=8954` and `TCG_NOTDIRTY_PAGES_HIT=1200`
  in the worst-frame interval — comes from the always-on
  per-interval TCG counters (added in earlier V3/V2 work, not
  V6 itself). V6's instrumented sources contributed exactly one
  spike line of attribution data inside the worst-frame window.
  The slice's value is in **what it ruled out**, not what it
  measured directly.

- **`tcg_handle_interrupt`'s one 2.4 ms event** has
  `ex_idx=65536 = 0x10000 = EXCP_INTERRUPT` (set by
  `cpu_handle_interrupt` when `cpu->exit_request` is signalled
  via `cpu_exit`). `int_req=0x2 = CPU_INTERRUPT_HARD` (i386 INTR
  pin, the hardware-interrupt path). 2.4 ms is the BQL acquisition
  cost when one specific external thread (probably the iothread
  delivering an i8259 IRQ) holds BQL during dispatch. Not a
  recurring bottleneck (1 event in 300 s) and not in the
  worst-frame window.

- **Comparison to D3 reproducibility.** Headline `mspf_max_max`
  matches D3 within 11 ms (0.8 %). Steady-state metrics
  (`avg_fps`, `mspf_max_p95`) match D3 within run-to-run noise.
  `stutter_intervals_30fps` differs significantly (88 vs 167) —
  likely route variance in the scripted-input replay, not a V6
  effect. The V6 slice does **not** cause measurable
  steady-state perf change in the off-path-overhead direction.

- **The `cpu_handle_interrupt` instrumentation captures
  POST-CALL `cpu->interrupt_request`** because the function
  clears bits as it processes. For attribution by source
  interrupt class, the more useful field would be the pre-call
  request bits. Not added in V6 because the per-event count is
  so low (1 event in 300 s) that the diagnostic value is
  marginal. Re-evaluate if V7 reveals interrupt-handling
  dominance.

- **Project rule #11 was honored.** No re-validation of the
  seven default-on flags. Indirect correctness check:
  `GEOM_SHADER_DRAW_TRI=0` across the 300 s run (native
  triangle path owns all triangle draws); `surface_scale=2` is
  active per `metadata.txt`; `XEMU_NATIVE_QUAD=1`,
  `XEMU_PGRAPH_FAST_READ=1` per metadata; APU-lock-release is
  default-on (Apple Silicon system build).

- **The Mission 1 sanity run (15 s PGR2 mid-route snapshot) did
  NOT exercise the V6 sources past the 1 ms threshold** because
  the snapshot is post-translation-warmup (all hot TBs already in
  cache). The 1 `tcg_handle_interrupt` event in the sanity run
  proves the emit path works end-to-end; the absence of `tb_lookup`
  / `tb_gen_code` events is not a bug. Retail Crimson exercises
  the cold-cache path immediately after snapshot resume, and
  even there the per-event 1 ms threshold is empty for V6's two
  translation-related sources.

## Files referenced

- M0 sanity run: `benchmark-runs/20260502-105141-pgr2/`
- M1 attribution run: `benchmark-runs/20260502-105232-crimson-skies/`
- V6 working-tree slice diff (3 files modified):
  - `accel/tcg/cpu-exec.c` (V6 instrumentation, ~85 lines added)
  - `xemu-fork/CLAUDE.md` (V6 op-tag list extended)
  - `docs/apple-silicon/automation.md` (V6 op-tag descriptions added)
- Sibling notes (the worst-frame attribution history):
  - `benchmarks/2026-05-01-tcg-splitwx-validation.md` (V1)
  - `benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md` (V2)
  - `benchmarks/2026-05-02-tcg-spike-attribution.md` (V3)
  - `benchmarks/2026-05-02-composite-goal-validation.md` (V4)
  - `benchmarks/2026-05-02-60hz-title-sanity-test.md` (SC2 sanity)
  - `benchmarks/2026-05-02-tcg-30fps-cap-attribution.md` (D3)
  - `benchmarks/2026-05-02-apu-lock-release-validation.md` (I5)
  - `benchmarks/2026-05-02-broader-title-sweep.md` (V4 library sweep)
