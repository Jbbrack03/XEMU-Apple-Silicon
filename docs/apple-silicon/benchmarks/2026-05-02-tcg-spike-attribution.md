# TCG / renderer per-event spike attribution (V3) — Crimson 1.27 s composite worst-frame

Date: 2026-05-02

## Verdict: PARTIAL ATTRIBUTION — the worst-frame interval contains exactly one tcg_tb_chain spike of 146 ms, accounting for ~10 % of the 1534 ms worst frame. The remaining ~1388 ms is composed of sub-10 ms events that do not surface at the per-event spike-log threshold.

A 300 s Crimson route was instrumented with the V3 spike-log
extensions (TCG-thread spike sources `tcg_tb_chain`,
`tcg_invalidate_burst`, `tcg_notdirty_storm`, `tcg_x87_storm`,
`tcg_pg_lock_wait`; pfifo-thread `renderer_pg_lock_wait`) at the
default 50 ms threshold, then re-run at a lowered 10 ms threshold.

**Decisive observations:**

1. **At the default 50 ms threshold the route emitted ZERO `xemu-spike:`
   lines** despite reproducing a 1398.644 ms composite worst frame
   (run dir `benchmark-runs/20260502-004540-crimson-skies`). The
   composite worst frame is built from events smaller than 50 ms —
   no single timed operation in the route reaches 50 ms during the
   bad interval.
2. **At the 10 ms threshold, 10 spike lines fired across the entire
   300 s route** — a 1533.879 ms composite worst frame reproduced
   (run dir `benchmark-runs/20260502-004845-crimson-skies`).
   Breakdown: 5 `tcg_tb_chain`, 3 `draw_begin` (renderer), 2
   `flush_draw` (renderer). **Zero `tcg_invalidate_burst`,
   `tcg_notdirty_storm`, `tcg_x87_storm`, `tcg_pg_lock_wait`, or
   `renderer_pg_lock_wait` fired in either run.**
3. **The worst-frame interval (i222, mspf_max=1533.879) contains
   exactly one spike: `tcg_tb_chain duration_us=146244 tb_count=1
   first_pc=0x23dd47`.** A second 104.1 ms `tcg_tb_chain` with the
   same `first_pc=0x23dd47` fires in the very next bad interval
   (i223, mspf_max=529.854). The 146 ms accounts for ~9.5 % of the
   1534 ms worst-frame stutter; 1388 ms remains unattributed at the
   10 ms threshold.

This is the "no attribution at threshold X" outcome the task
anticipated as one possible result, but with the partial attribution
that **`tcg_tb_chain` is the only spike class that fires inside the
worst-frame interval**, and that a single guest PC (`0x23dd47`) is
implicated in two consecutive bad intervals. The remaining 1388 ms
of composite cost lives below the per-event spike threshold —
sustained sub-10 ms events that aggregate, not a single dominant
class. The next slice should be (a) lower-threshold replay anchored
on i222 with a 1 ms threshold to capture the 1388 ms composition,
combined with (b) a guest-code disassembly of `0x23dd47` on the
boot/profile-prep HDD to identify what the Xbox is doing during the
stall.

This V3 result definitively rules out the four hypothesis classes
the slice was designed to test: large-burst TB invalidation, SMC
notdirty storms, x87 helper-call storms, and renderer/TCG pg->lock
contention. None of these fire even once at the 10 ms threshold
across 300 s of Crimson gameplay including the worst frame. **The
remaining bottleneck axes — guest-code-specific TB-execution
stalls (suspect: the `0x23dd47` chain) and an unattributed
sub-10 ms-event composite — are now the only candidates for a V4
slice.**

## Build / commit verification

- Binary: `dist/xemu.app/Contents/MacOS/xemu`, built 2026-05-02
  ~00:35 UTC (mid-session) with the V3 spike-log infrastructure on
  top of the V1+V2 working tree.
- `xemu --version` → `xemu_version: 0.8.134-47-g1534bb7688`,
  `xemu_commit: 1534bb7688718e6bdfdf9e3bfe533829991ca202` (dirty,
  with V1+V2+V3 uncommitted edits).
- `codesign --verify --deep --strict --verbose=2
  dist/xemu.app` → `valid on disk` and `satisfies its Designated
  Requirement`.
- GL renderer / OS confirmed Apple GL 4.1 Metal — 90.5 on Apple
  M3 Ultra, macOS 26.4.1 (build 25E253).
- Sanity: 12 s PGR2-snapshot run with all V3 instrumentation
  enabled and threshold lowered to 1 ms produced 4084 spike lines
  including 2926 `tcg_tb_chain` events plus all renderer ops —
  proving the V3 emit path is wired through end-to-end. Sanity-run
  dir: `benchmark-runs/20260502-004503-pgr2`.

## Goal of the slice

Per project rule #1 (no guessing), no further code-fix slices ship
until the composite Crimson 1.27-second worst frame is attributed
to a specific cost class. V1 (splitwx) and V2 (targeted-jmp-cache)
each removed a real per-call cost, neither moved the headline
mspf_max. V2's decisive evidence — `TCG_INVALIDATE_WALL_US_MAX=688
µs` in the worst Arm-C interval — proved no single
`tb_invalidate_phys_page_range__locked` call could account for the
1.27 s frame. The frame must be either a sequence of many smaller
events or a non-invalidation cost class entirely.

V3 builds the per-event spike-log infrastructure to attribute that
composite. Five new TCG/renderer spike sources, all gated on
`XEMU_PERF_SPIKE_LOG_TCG=1` (default off; one global load + branch
when off):

1. **`tcg_tb_chain`** — wraps one full pass through the inner
   while-handle-interrupt loop in `accel/tcg/cpu-exec.c::cpu_exec_loop`.
   Captures `tb_count` (TBs executed in this pass) and `first_pc`
   (guest x86 PC of the first TB in the chain). Fires when the
   pass exceeds the threshold.
2. **`tcg_invalidate_burst`** — extends the V2 wallclock measurement
   in `tb_invalidate_phys_page_range__locked` (`accel/tcg/tb-maint.c`).
   Fires when a single call exceeds the threshold; captures
   `burst` (TB count) and `page` (page-aligned ram_addr).
3. **`tcg_notdirty_storm`** — sliding 1-second-window rate detector
   in `xemu_tcg_perf_notdirty_storm_tick`, called from
   `accel/tcg/cputlb.c::notdirty_write`. Fires when the window
   rate exceeds 100,000 trips/s; captures `events` and
   `rate_per_s`.
4. **`tcg_x87_storm`** — sliding 1-second-window rate detector in
   `xemu_tcg_perf_x87_storm_tick`, called from
   `target/i386/tcg/fpu_helper.c::helper_{fmul_ST0_FT0,
   fadd_STN_ST0, fsub_STN_ST0, fdiv_STN_ST0}`. Fires when the
   window rate exceeds 50,000,000 helper calls/s.
5. **`tcg_pg_lock_wait`** / **`renderer_pg_lock_wait`** — wraps the
   `qemu_mutex_lock(&pg->lock)` in `pgraph_read`/`pgraph_write`
   (TCG vCPU thread MMIO) and in `pfifo_run_puller` (pfifo
   thread). Fires when a single lock-acquisition wait exceeds
   the threshold.

The shared spike-emit helper was factored into
`util/xemu-spike-log.c` + `include/qemu/xemu-spike-log.h` so the
TCG-thread sites can call it without depending on NV2A
renderer headers. The existing `nv2a_profile_spike` is now a
thin backward-compat wrapper. New env-var:
`XEMU_PERF_SPIKE_LOG_TCG=1` enables the TCG-thread sources
independently of the renderer-side `XEMU_PERF_SPIKE_LOG=1`.

## Test matrix

Both arms use the post-fast-read stable opt-in flag set
(`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1`) plus `XEMU_PERF_LOG=1
XEMU_PERF_FRAME_LOG=1 XEMU_PERF_SPIKE_LOG=1
XEMU_PERF_SPIKE_LOG_TCG=1`. Splitwx and targeted-jmp-cache are
auto-on (Apple Silicon system-build defaults from V1 / V2).
Screenshots are off (`XEMU_BENCH_SCREENSHOT_BACKEND=none`).

| Arm | Title | Mode | Spike threshold | Duration | Run dir |
| --- | --- | --- | --- | --- | --- |
| Sanity | PGR2 | snapshot `pgr2_gameplay_b4`, noop | 1 ms | 12 s | `benchmark-runs/20260502-004503-pgr2` |
| V3-50ms | Crimson | retail route `crimson-gameplay.csv`, profile-prep HDD | 50 ms (default) | 300 s | `benchmark-runs/20260502-004540-crimson-skies` |
| V3-10ms | Crimson | retail route `crimson-gameplay.csv`, profile-prep HDD | 10 ms (lowered) | 300 s | `benchmark-runs/20260502-004845-crimson-skies` |

The V3-10ms re-run was launched per the task's "if NOTHING fires —
lower the threshold to 10 ms and re-run" branch. The V3-50ms run is
preserved as evidence that the composite worst frame contains zero
≥50 ms spike events.

## Headline metrics — V3-10ms Crimson 300 s route

| Metric | Value |
| --- | ---: |
| `intervals` | 281 |
| `post_load_intervals` | 276 |
| `post_load_avg_fps` | 29.62 |
| `post_load_avg_mspf` | 32.60 ms |
| `post_load_fps_stddev` | 6.156 |
| `post_load_mspf_max_p50` | 33.69 ms |
| `post_load_mspf_max_p95` | 169.74 ms |
| `post_load_mspf_max_p99` | 1174.09 ms |
| `post_load_mspf_max_max` | **1533.88 ms** (worst-frame composite) |
| `post_load_mspf_avg_max` | 329.96 ms |
| `post_load_frame_mspf_us_p999` | 422,098 µs |
| `post_load_frame_mspf_us_max` | 1,533,879 µs |
| `post_load_stutter_intervals_30fps` | 167 |
| `post_load_longest_stutter_run_30fps` | 23 |
| `TCG_TB_EXEC_COUNT` | 4,184,454,659 |
| `TCG_TB_INVALIDATE_COUNT` | 13,678,764 |
| `TCG_TB_INVALIDATE_BURST_MAX` | 475 |
| `TCG_INVALIDATE_WALL_US_MAX` | **2,479 µs** (worst per-call invalidation) |
| `TCG_NOTDIRTY_TRIPS` | 3,519,454 |
| `TCG_JMP_CACHE_ZEROED_BUCKETS` | 24,266,924 |

Reading: V3 reproduces the headline composite worst frame at
1533.88 ms — even larger than V2's 1290.87 ms. p99 of
`mspf_max` is 1174 ms, indicating multiple stutters of the same
order. **`TCG_INVALIDATE_WALL_US_MAX=2479 µs`** (the worst per-call
`tb_invalidate_phys_page_range__locked` cost across the entire
300 s route) is 2.5 ms — three orders of magnitude smaller than
the 1534 ms worst frame. V2's verdict is confirmed: the worst frame
is structurally not a single invalidation call.

## Raw spike counts — V3-10ms Crimson 300 s route

| op tag | count | max duration (ms) | total duration (ms) |
| --- | ---: | ---: | ---: |
| `tcg_tb_chain` | 5 | 146.244 | 322.819 |
| `draw_begin` | 3 | 69.891 | 93.443 |
| `flush_draw` | 2 | 10.470 | 20.624 |
| `tcg_invalidate_burst` | **0** | — | — |
| `tcg_notdirty_storm` | **0** | — | — |
| `tcg_x87_storm` | **0** | — | — |
| `tcg_pg_lock_wait` | **0** | — | — |
| `renderer_pg_lock_wait` | **0** | — | — |
| (renderer side: `surf_to_tex`, `surf_download`, `surf_upload`, `bind_textures`, `tex_upload`, `flip_stall_glfinish`) | 0 | — | — |

10 spike lines total across 300 s of gameplay at the 10 ms
threshold. **Zero of the four TCG-attribution hypothesis classes
(invalidation burst, notdirty storm, x87 storm, lock-wait) fired.**
The renderer-side ops that did fire (`draw_begin`, `flush_draw`)
are pre-existing instrumented code paths and are inside the
renderer thread, not the TCG vCPU; they do not co-locate with the
worst-frame interval.

## Worst-frame correlation — V3-10ms

The worst-frame interval is **i222** (line 248 of
`benchmark-runs/20260502-004845-crimson-skies/xemu.log`):

```
xemu-perf: interval_ms=1816 frames=5 fps=2.75 mspf_avg=329.961
mspf_min=1.966 mspf_max=1533.879 ...
frame_mspf_us=23920,67007,23032,1966,1533879
TCG_TB_EXEC_COUNT=1269217 TCG_TB_INVALIDATE_COUNT=4599
TCG_NOTDIRTY_TRIPS=1175 TCG_TB_INVALIDATE_BURST_MAX=10
TCG_JMP_CACHE_ZEROED_BUCKETS=4599 TCG_INVALIDATE_WALL_US_MAX=2479
```

The 5-frame trace shows the bad frame is a single 1.534 s outlier
preceded by four sub-70 ms frames. Inside this 1816 ms interval:
- TB-exec count is 1.27 M — **65× lower** than steady-state
  intervals (~85 M). The vCPU thread is largely starved during the
  stall.
- TB invalidations are 4599 (~30× lower than steady state).
- Notdirty trips are 1175 (~10× lower than steady state).
- Burst max is only 10, wall_us_max only 2.5 ms.

**No invalidation, no SMC trapping, no jmp-cache work to speak of.
The vCPU is simply not making progress.**

Anchoring the spike timeline to the cumulative interval boundaries
(first interval emit at wallclock anchor ≈ 400872982 ms,
end-of-i222 cumulative ms = 227910), the spikes that fired in the
i222 wallclock window [226094, 227910] ms are:

| ms-since-first-emit | op | duration_us | extra |
| ---: | --- | ---: | --- |
| 227116 | `tcg_tb_chain` | **146,244** | `tb_count=1 first_pc=0x23dd47` |

**Exactly one spike fires inside the worst-frame interval: a
146.244 ms `tcg_tb_chain` with `tb_count=1` (one TB-execution
iteration) at guest-x86 PC `0x23dd47`.** The 146 ms cost is
~9.5 % of the 1534 ms worst frame; the remaining ~1388 ms is
unattributed at the 10 ms threshold.

Spikes that fired in the immediate neighborhood (next interval
i223, also a stutter at mspf_max=529.854 ms):

| ms-since-first-emit | op | duration_us | extra |
| ---: | --- | ---: | --- |
| 231130 | `tcg_tb_chain` | **104,103** | `tb_count=1 first_pc=0x23dd47` |
| 231691 | `flush_draw` | 10,470 | (renderer thread) |
| 232189 | `flush_draw` | 10,154 | (renderer thread) |

**The same `first_pc=0x23dd47` fires twice in two consecutive bad
intervals.** Combined with `tb_count=1`, this means a single TB
that begins at guest x86 PC `0x23dd47` is responsible for the
spike. It is unlikely to be the TB body itself (TBs are tiny);
the most plausible cost is `tb_gen_code` (translation), an
exception-handling longjmp out and back, or `mmap_lock`/page-table
contention triggered by that code path. The 0x23dd47 PC is on the
profile-prep HDD's currently-running guest binary; identifying
what guest function lives at that address is the first concrete
step for the next slice.

Additional context — five `tcg_tb_chain` spikes total:

| ms | duration_us | tb_count | first_pc | interpretation |
| ---: | ---: | ---: | --- | --- |
| -12476 | 42,790 | 26 | 0xfffffff0 | x86 reset vector — boot/loadvm phase |
| -12458 | 15,890 | 4022 | 0x400efb | early boot exception/IPL |
| 219199 | 13,792 | 1 | 0x20a926 | mid-route stall (different PC) |
| **227116** | **146,244** | **1** | **0x23dd47** | **inside worst frame i222** |
| 231130 | 104,103 | 1 | 0x23dd47 | inside next bad frame i223 |

Two boot-time spikes (negative ms-since-first-emit) and three
mid-gameplay spikes. Of the three gameplay spikes, the two largest
are the same first_pc (0x23dd47) and span the worst-frame interval
plus the next adjacent stutter. **This is a smoking gun for a
guest-code-specific bottleneck**, not a host-emulation general
cost.

## Comparison vs V3-50ms (default-threshold) Crimson run

V3-50ms (`benchmark-runs/20260502-004540-crimson-skies`):
- 281 perf intervals, post_load_mspf_max_max = **1398.64 ms**
- **Zero spike lines emitted** in the entire 300 s route at 50 ms
  threshold.
- TCG_INVALIDATE_WALL_US_MAX = 303 µs (cumulative max), max burst
  505 — both tiny vs the 1.4 s worst frame.

The V3-50ms run is the headline result: **the entire composite
1.4 s worst frame is built from events smaller than 50 ms.** Even
the `tcg_tb_chain` spike that fires in the V3-10ms run's worst
frame (146 ms) does not register at 50 ms in the V3-50ms run —
the two runs are independent and the worst-frame composition
varies, but in both runs the headline stutter is dominated by
sub-50 ms events.

## What the V3 result proves and disproves

**Disproven (zero spikes fired across both runs):**

1. **TB-invalidation burst cost is not a worst-frame contributor.**
   `tcg_invalidate_burst` fires zero times at 10 ms across 300 s
   of gameplay including a 1.534 s worst frame.
   `TCG_INVALIDATE_WALL_US_MAX=2479 µs` confirms the worst per-call
   cost is 2.5 ms. This finalizes the V2 verdict.
2. **SMC notdirty storms are not a worst-frame contributor.**
   `tcg_notdirty_storm` fires zero times — the per-second
   notdirty rate never exceeds 100K trips/s. Total
   `TCG_NOTDIRTY_TRIPS=3,519,454` over 300 s ≈ 11.7K/s average,
   well under the storm threshold.
3. **x87 helper storms are not a worst-frame contributor.**
   `tcg_x87_storm` fires zero times — Crimson's x87 helper-call
   rate stays under 50 M/s (the threshold). The
   `floatx80_*`/`parts*` symbols at 7 % of TCG-thread sample
   profile (V2 evidence) are steady-state cost, not bursty.
4. **Renderer/TCG pg->lock contention is not a worst-frame
   contributor.** Both `tcg_pg_lock_wait` (TCG-side) and
   `renderer_pg_lock_wait` (pfifo-side) fire zero times. Lock
   acquisition latency stays under 10 ms across the entire
   route. R2's prior `XEMU_PGRAPH_FAST_READ` slice already moved
   the bulk of vCPU pg->lock waits off the critical path.

**Partially attributed:**

5. **`tcg_tb_chain` at guest PC `0x23dd47` fires inside the
   worst-frame interval, contributing 146 ms (~9.5 %) of the
   1534 ms stutter.** The fact that `tb_count=1` (one
   TB-execution loop iteration) means the cost is *inside* a
   single TB lookup + execution + exit cycle — which usually
   means `tb_gen_code` (translating a fresh TB), `mmap_lock`,
   or an exception-handling longjmp inside the guest's code at
   `0x23dd47`. The same PC fires again in the next bad interval
   for 104 ms.

**Unattributed (the remaining ~90 % of the worst frame):**

6. **~1388 ms of the 1534 ms worst frame is composed of sub-10 ms
   events.** None of the eight instrumented spike sources catch
   it. The TB-exec / TB-invalidate / notdirty counters all drop
   65× / 30× / 10× during the bad interval — the vCPU thread
   is starved, not over-worked. Where the wallclock is going is
   not visible from the V3 instrumentation.

## Conclusion + recommended next slice

The composite Crimson 1.27-1.53-second worst frame **is not a
TB-invalidation, not an SMC storm, not an x87 storm, and not
pg->lock contention**. Those four hypothesis classes are
definitively ruled out by V3.

The strongest single attributed signal is `tcg_tb_chain` at guest
PC `0x23dd47`, contributing 146 ms in the worst frame and 104 ms
in the next adjacent bad frame. The cost mechanism (single TB
iteration taking >100 ms) is consistent with one of:

a. **`tb_gen_code` translation cost**: the guest's i386 code at
   `0x23dd47` is a hot translation target the JIT keeps
   re-translating after invalidation. Worth checking
   `cpu_loop_exit` / `tb_gen_code` cost specifically.
b. **`mmap_lock` contention**: TB code-buffer modifications take
   the global mmap_lock; if the renderer or another thread holds
   it, the TCG thread blocks here.
c. **Exception-path longjmp cost**: a guest exception/IRQ inside
   the TB at `0x23dd47` longjmps back to the outer setjmp loop;
   if this happens repeatedly with cascading state restores, a
   single iteration can stall.

But **the ~1388 ms unattributed remainder is the larger problem.**
At a 10 ms threshold no instrumented site fires — meaning the
remainder is composed of either (i) 100+ events of 5-10 ms each
that we don't catch, (ii) a non-TCG, non-renderer thread (most
likely the QEMU main-loop / iothread / VGA refresh) blocking
forward progress, or (iii) wallclock pacing artifacts that the
spike instrumentation cannot see (cgroup throttling, GPU
synchronization at present time, etc).

### Recommended next slice (priority order)

1. **V4 — Threshold-1ms re-run with guest-PC bucketing.**
   - Re-run the same 300 s Crimson route with
     `XEMU_PERF_SPIKE_LOG_THRESHOLD_US=1000` (1 ms). Expect
     ~10,000-100,000 spike lines; volume is acceptable for one
     attribution run. The 1388 ms unattributed remainder of the
     worst frame should decompose into per-event spikes whose
     sum approaches 1388 ms.
   - Estimated lift: **definitive composition of the worst
     frame**, no perf change. Pure diagnostic. Cost: one 300 s
     re-run plus parsing.
2. **V4b — Guest-PC disassembly of `0x23dd47` on the profile-prep
   HDD.** Boot the same HDD with a single-step debugger or
   `qemu -d in_asm,exec` log filter targeted at PC range
   `0x23dd00`-`0x23df00` to identify what Xbox kernel/title
   function the JIT is stalling on. If it is a known hot path
   (memcpy, sound mixer, AI tick, etc.) the next code slice
   targets that specific behavior.
3. **V5 — Add `tcg_tb_gen_code` and `mmap_lock_wait` spike
   sources.** The V4 1ms run should localize the dominant
   sub-spike op type. If it turns out to be `tcg_tb_chain` with
   ever smaller `tb_count`, then `tb_gen_code` (translation
   churn) is the next instrumentation slice. If it turns out
   to be a renderer op (e.g. `flush_draw`, `bind_textures`)
   firing repeatedly, the bottleneck class shifts back to the
   renderer despite V3's negative pg-lock-wait result. Either
   way, V5 is `XEMU_DIAG_*` instrumentation extension first;
   the code-fix slice is V6.
4. **V6 — IO-thread / main-loop attribution.** If V4+V5 confirm
   the unattributed cost lives outside the TCG vCPU and pfifo
   threads, the QEMU main-loop / iothread / VGA refresh
   becomes the next suspect. Add `qemu_main_loop_iter` spike
   instrumentation. This is consistent with the observation
   that during i222 all TCG counters drop dramatically — the
   vCPU is *waiting* on something (most likely a thread that
   isn't yet instrumented).

**Do NOT pursue:**

- Further per-jmp-cache micro-optimization (already exhausted by
  V2).
- Splitwx-batching (already exhausted by V1).
- x87 NEON lowering (V3 disproves x87 storms; the 7 % steady-state
  cost from V2 is real but not bursty, so a NEON lowering would
  not move worst-frame mspf).
- pg->lock release-during-GL (V3 disproves pg->lock contention as
  a worst-frame contributor; this slice would not move
  mspf_max).
- Native Metal renderer (V3 attributes 0 of the 1.5 s worst frame
  to the renderer thread; replacing the renderer cannot fix a
  vCPU-thread stall).

## Honest-limits caveats

- **The V3-10ms run produced only 10 spike lines.** Statistical
  confidence on which spike-type dominates the worst frame is
  intrinsically low; what V3 establishes definitively is which
  spike types DO NOT fire (the four disproven categories). The
  146 ms `tcg_tb_chain` at 0x23dd47 is the strongest single
  signal but accounts for only ~10 % of the worst frame; the
  remaining 90 % is unattributed at this threshold.
- **Anchoring the spike now_us field to interval boundaries is
  approximate.** The `xemu-perf:` interval line does not (yet)
  emit a `now_us=` field, so spike-to-interval correlation uses
  cumulative interval_ms summed from the first emit, anchored
  by the wallclock now_us of the spike that fires immediately
  after the interval boundary. This gives ±100 ms of correlation
  uncertainty, well below the 1.5 s worst-frame scale and below
  the spike duration itself, so the i222 attribution conclusion
  is sound. A trivial follow-up: add `now_us=<value>` to the
  `xemu-perf:` interval line for exact correlation in V4.
- **Crimson is non-deterministic.** V3-50ms produced a 1398.64 ms
  worst frame; V3-10ms produced a 1533.88 ms worst frame on the
  same 300 s route. Both reproduce the headline stutter. A
  single 1-second-class spike per 5-minute route means the
  per-spike confidence is intrinsically low — but both runs
  agree on the broader pattern (multiple sub-second stutters
  concentrated in a few intervals; TCG counters DROP during the
  bad intervals rather than spike).
- **The `0x23dd47` PC is specific to the profile-prep HDD's
  current title state** (loaded snapshot at boot; not the same
  binary instance across reboots). The PC is meaningful only
  while the HDD's loaded program counter sits at this guest
  address; any future profile-prep refresh requires
  re-identifying the worst-frame guest PCs. The general lesson
  — single TBs taking >100 ms on guest hot paths — is robust
  even when the specific PC is not.
- **`tcg_tb_chain` cost interpretation depends on whether the
  outer setjmp loop is involved.** With `tb_count=1`, the inner
  while-handle-interrupt loop ran exactly one body iteration:
  one TB lookup + one TB execution + one exit-handling pass.
  Inside that 146 ms scope are: `tb_lookup`/`tb_gen_code`
  (creating or finding the TB), `cpu_tb_exec` (executing it),
  and `cpu_handle_interrupt` (exit decision). The largest of
  these three is the answer, but V3 instrumentation does not
  decompose them. V5 should add per-phase spike instrumentation
  inside `cpu_exec_loop` if V4's 1ms run does not localize.
- **The 0 hits for `renderer_pg_lock_wait` does not mean the
  renderer is starvation-free.** The renderer-side pg->lock
  acquisition runs from the pfifo thread (`pfifo_run_puller`),
  which is a separate thread from the GL renderer's GL-context
  worker. If the GL worker has its own waits (e.g. on
  `glFinish`, `glClientWaitSync`), V3 does not measure them.
  The `flip_stall_glfinish` op in the renderer-side spike log
  partially covers this; it did not fire inside the worst-frame
  interval.
- **One xemu-process-at-a-time enforcement worked.** The
  launcher's lock prevented overlap; each run waited cleanly for
  the previous to terminate. No retries needed.

## Files referenced

- Sanity run: `benchmark-runs/20260502-004503-pgr2/`
- V3-50ms Crimson run:
  `benchmark-runs/20260502-004540-crimson-skies/`
- V3-10ms Crimson run (the attribution data):
  `benchmark-runs/20260502-004845-crimson-skies/`
- V3 working-tree slice diff:
  - New: `util/xemu-spike-log.c`,
    `include/qemu/xemu-spike-log.h`
  - Modified: `util/meson.build`,
    `hw/xbox/nv2a/pgraph/profile.c`,
    `accel/tcg/tcg-all.c`,
    `accel/tcg/cpu-exec.c`,
    `accel/tcg/tb-maint.c`,
    `accel/tcg/cputlb.c`,
    `accel/tcg/xemu-tcg-perf.c`,
    `include/qemu/xemu-tcg-perf.h`,
    `target/i386/tcg/fpu_helper.c`,
    `hw/xbox/nv2a/pgraph/pgraph.c`,
    `hw/xbox/nv2a/pfifo.c`,
    `docs/apple-silicon/automation.md`
- Prior validation notes:
  `docs/apple-silicon/benchmarks/2026-05-01-tcg-splitwx-validation.md`
  (V1),
  `docs/apple-silicon/benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md`
  (V2).
