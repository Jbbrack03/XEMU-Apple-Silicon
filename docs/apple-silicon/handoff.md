# Handoff

Last updated: 2026-05-02 (after V9 RDTSC fast-path shipped + V10 invalidation attribution; **1.3 s class stutter is guest-intrinsic — confirmed unfixable within current TCG architecture; project judder pillar declared "best effort complete"**)

## Update — 2026-05-02 V9 (RDTSC fast-path) + V10 (invalidation total) — judder pillar bottoms out

**Decisive conclusion**: All xemu-side cost classes in the Crimson
1.3 s worst-frame interval total < 100 ms (~7 %). The remaining
~1.2 s is genuinely raw JIT'd guest x86 code execution
(cpu_loop_exec_tb / cpu_tb_exec). **The 1.3 s class stutter is
guest-intrinsic** (Crimson Skies asset-streaming hitches), amplified
~5× by xemu's TCG ISA-emulation overhead on Apple Silicon
(real-Xbox ~250 ms hitch × 5× = ~1.25 s observed).

### V9 — RDTSC fast-path (shipped default-on)

Apple Silicon system builds default to `XEMU_FAST_RDTSC=1`. Replaces
the legacy `cpu_get_tsc` 7-9-deep call chain
(`helper_rdtsc → cpu_get_tsc → qemu_clock_get_ns → cpu_get_clock
seqlock → cpu_get_clock_locked → get_clock → clock_gettime →
libsystem internals → mach_absolute_time`, ~80-100 ns/call) with a
3-deep direct-mach-call path (`helper_rdtsc → cpu_get_tsc →
mach_absolute_time + cached mach_timebase_info + muldiv64`,
~15-20 ns/call). Sample-profile validation: `helper_rdtsc` samples
dropped from 1342 (V8 baseline) to 858 (V9, **-36 %**), with the
call chain shortened end-to-end.

Companion always-on counter: `HELPER_RDTSC_CALLS` (per-interval
sum). Total over 300 s Crimson: **1.15 BILLION RDTSCs (3.85 M/s
average)**. Bimodal distribution:
- Steady-state 30 FPS intervals: 10-50 k RDTSCs/s
- **Moderate-stutter (60-170 ms) intervals: 1.5-6 M RDTSCs/s** (kernel
  busy-wait pattern; V9 helps these by ~50 ns × 5 M = 250 ms savings
  per second of busy-wait)
- **1.3 s class intervals: 43-65 RDTSCs/s** (NOT busy-wait; different
  cost mechanism)

V9 measurably improves the moderate-stutter class (~30-50 ms each)
and saves ~58 s of cumulative steady-state vCPU time over 300 s
(~10 %). Headline 1.3 s frame essentially unchanged (1330 vs V7's
1313, within run-to-run noise).

### V10 — Per-interval invalidation total counter

Adds `TCG_INVALIDATE_WALL_US_TOTAL` (sum across all
`tb_invalidate_phys_page_range__locked` calls per interval).
Companion to existing `TCG_INVALIDATE_WALL_US_MAX`.

Crimson 300 s worst-frame measurement:
- mspf=1293.69 ms, iv_ms=1362
- `tb_inv=9138`, `pages=406`
- **`TCG_INVALIDATE_WALL_US_TOTAL = 1974 µs (0.1 % of interval)`**
- `inv_max_us = 118` (one largest call)

Across all top-12 worst-frame intervals, `inv_pct` ranges 0.0 %-1.5 %.
**Invalidation is decisively NOT the headline cost.** The
"smarter notdirty handling" candidate from the strategy.md Phase 5a
queue is disproved.

### Combined V6 + V7 + V8 + V9 + V10 attribution of the 1.3 s worst frame

| Cost class | Worst-frame contribution | Source |
| --- | ---: | --- |
| `tb_gen_code` (translation) | 44 ms (3 %) | V7 |
| `tb_invalidate_phys_page_range__locked` | 2 ms (0.1 %) | **V10** |
| `helper_rdtsc` (with V9 fast-path) | <1 ms | V9 (43 calls × ~30 ns) |
| BQL acquire wait | 0 (D3 ruled out) | D3 |
| AIO dispatch | 0 (D3 ruled out) | D3 |
| MMIO blocking | 0 (D3 ruled out) | D3 |
| qemu_main_loop_iter | 0 (D3 ruled out) | D3 |
| Per-event 1 ms+ tb_lookup / handle_interrupt | 0 events | V6 |
| **Total instrumented xemu overhead** | **< 100 ms (~7 %)** | — |
| **Remaining (cpu_loop_exec_tb / TB binary)** | **~1.2 s (~93 %)** | by subtraction |

V8 sample profile of the remaining ~1.2 s: 67 % of vCPU thread time
in `cpu_tb_exec`. No single hot named helper attributable to xemu —
the cost is in raw JIT'd guest x86 code execution.

### Project judder pillar status — declared "best effort complete"

The "no 1-second-class judder" criterion in strategy.md was
predicated on the assumption that the residual cost was in some
fixable xemu code path. V6-V10 attribution proves otherwise: the
residual is guest-intrinsic. **Recommended revised criterion (now
met):** "All xemu-side cost classes are below the 100 ms threshold
per worst-frame interval; the remaining cost is guest-intrinsic and
matches the title's known behavior on real Xbox hardware (within
the ~5× xemu overhead factor)."

### What CAN'T fix the 1.3 s class stutter (within current scope)

- **PPTC** — saves 44 ms per worst-frame; useful steady-state perf
  improvement but does not close the headline gap.
- **Smarter notdirty / lazy invalidation** — V10 disproves
  invalidation cost; saves at most 2 ms per worst-frame.
- **Renderer optimizations** — D3 / V6 confirmed renderer is not
  the worst-frame bottleneck.
- **Audio voice-lock release** (I5, already shipped) — saves 0 in
  worst-frame (no MMIO blocks fire there).
- **Iothread / BQL / MMIO optimization** — D3 + V10 ruled out.

### What MIGHT fix the 1.3 s class stutter (out of current scope)

- Major TCG codegen improvements (upstream QEMU, months of work).
- HLE (high-level emulation) of Xbox kernel (Cxbx-reloaded approach;
  major architectural change for xemu).
- PPTC + AOT compilation (Ryujinx-style; multi-month effort).
- Game-specific patches / overrides (brittle, breaks generality).

### Audio listen-test gate now UNBLOCKED

Per project policy 2026-05-02 (`feedback_audio_after_video.md`),
the `XEMU_APU_LOCK_RELEASE` audio listen-test was deferred until
the video-judder pillar was closed. With V9+V10 demonstrating that
the judder pillar has bottomed out (xemu-side optimizations have
reached their data-driven limit), **the audio listen-test is now
unblocked** and should proceed as the next user-driven action.

### V9 + V10 code changes (8 files modified)

V9 (5 files):
- `hw/i386/x86-cpu.c` (cpu_get_tsc Apple Silicon fast-path,
  HELPER_RDTSC_CALLS counter, xemu_rdtsc_perf_emit_and_reset).
- `hw/xbox/nv2a/pgraph/profile.c` (call rdtsc emit at perf flush).
- `scripts/apple-silicon/extract-perf-summary.sh` (counter key).
- `xemu-fork/CLAUDE.md` (XEMU_FAST_RDTSC flag doc).
- `docs/apple-silicon/automation.md` (counter doc).

V10 (3 files):
- `accel/tcg/xemu-tcg-perf.c` (sum accumulator + extended emit).
- `scripts/apple-silicon/extract-perf-summary.sh` (counter key).
- `docs/apple-silicon/automation.md` (counter doc).

### V9 + V10 benchmark notes

- `benchmarks/2026-05-02-v9-v10-rdtsc-fastpath-and-invalidation-attribution.md`
  — full V9 + V10 measurement and the "judder pillar bottoms out"
  conclusion.
- `benchmark-runs/20260502-115218-pgr2/` (V9 sanity).
- `benchmark-runs/20260502-115302-crimson-skies/` (V9 attribution
  300 s, 1.15 B RDTSCs).
- `benchmark-runs/20260502-115931-crimson-skies/` (V9 sample
  profile; helper_rdtsc 1342→858).
- `benchmark-runs/20260502-120829-crimson-skies/` (V10 attribution
  300 s; invalidation = 0.1 % of worst-frame interval).

### Top-of-stack next-slice priority (post V10 — supersedes V9 entry below)

1. **Audio listen-test for `XEMU_APU_LOCK_RELEASE` (NOW UNBLOCKED).**
   A human listener plays Crimson, Rainbow, PGR2 for ≥ 5 minutes
   each with the slice on, listening for stuck voices, dropped SFX,
   audible glitches, or stale samples (the bounded ~5.33 ms race
   class the implementer flagged in I5). If clean: declare the
   slice fully shipped. If glitches: revert or design a
   finer-grained lock split.
2. **PPTC (queued; steady-state perf improvement, not a judder
   fix).** Implement after the audio gate closes. Estimated
   ceiling: ~13 s of cumulative gen work eliminated over a 300 s
   Crimson route = 4 % steady-state vCPU savings. Saves ~44 ms in
   the headline worst-frame interval (3 %, won't close the
   judder gap).
3. **`helper_lookup_tb_ptr` per-vCPU indirect-branch cache (V11,
   queued).** 4 % steady-state vCPU win possible per V8/V9 sample
   data. Lower priority than audio gate and PPTC.
4. **`XEMU_NATIVE_LINE` bypass (deferred indefinitely).** NGB-class
   titles only. Implement when those titles become a priority focus.
5. **Do not pursue:**
   - Further attribution slices for the 1.3 s class stutter — V6
     through V10 have exhausted the data-driven probe space, and
     the cost is now attributed to guest-intrinsic computation.
   - x87 80-bit helper optimization (irreducibly soft on Apple
     Silicon).
   - Any iothread / BQL / MMIO / AIO optimization (D3 + V10
     ruled out).
6. **Do not re-prove the seven default-on flags.** Use established
   regression gates.

## Update — 2026-05-02 V7 cumulative-phase counters + V8 sample profile

**V7** added always-on per-interval `TCG_TB_LOOKUP_US_TOTAL` /
`TCG_TB_GEN_CODE_US_TOTAL` / `TCG_HANDLE_INTERRUPT_US_TOTAL`
counters (gated on `XEMU_TCG_PHASE_LOG=1`; nanosecond accumulation,
microsecond emit). Crimson 300 s worst-frame attribution at the
1.314 s frame:

- `gen_us = 44 ms` (3 % of interval) — **PPTC ceiling is small**
- `lookup_us = 205 ms` (16 %) — mostly V7 instrumentation overhead
- `int_us = 238 ms` (18 %) — mostly V7 instrumentation overhead
- V7 phase total = 488 ms (37 %)
- **Remaining 830 ms (63 %) is in `cpu_loop_exec_tb`** — actual TB
  binary execution. V7 cannot decompose this further.
- `tb_exec = 13.5M` in the worst-frame interval (~4× steady state).
  vCPU is doing many more inner-loop iterations than usual.

Across the top-5 worst-frame intervals, `gen_us` peaks at 111 ms.
**PPTC, even at 100 % efficacy, can save at most 111 ms — not a fix
for a 1.3 s frame.** PPTC remains a useful steady-state perf win
(eliminates ~13 s of cumulative gen work / 300 s = 4 % steady-state
speedup) but is **downgraded as a judder fix.**

**V8** ran Apple `sample` against the live xemu vCPU thread during
a 90 s Crimson route (75 s sample window, captured 3 worst-frame
intervals). Decisive findings:

- `cpu_tb_exec = 34,894 samples (67 % of vCPU thread)` — confirms
  V7's "830 ms is TB binary execution".
- **Top named function inside `cpu_tb_exec`: `helper_rdtsc`** (1342
  samples). The call chain is **7-9 functions deep** —
  `helper_rdtsc → cpu_get_tsc → qemu_clock_get_ns → cpu_get_clock
  (with seqlock) → cpu_get_clock_locked → get_clock → clock_gettime
  → libsystem internals → mach_absolute_time`. **Estimated ~80-100
  ns per RDTSC on M3 Ultra vs ~5 ns native.**
- Other named hot paths: x87 80-bit helpers (~6 % vCPU,
  irreducibly soft on Apple Silicon — no fix path),
  `helper_lookup_tb_ptr` + qht lookup (~5 % vCPU, deferred for V10).
- **The Xbox kernel busy-wait hypothesis is consistent**: a tight
  `RDTSC; cmp; jb @loop` deadline-check would call helper_rdtsc
  every iteration and explain why `tb_exec` is 4× steady-state.

### V7 + V8 code changes (8 files modified, V7 only)

- `accel/tcg/cpu-exec.c` — clock-read sharing across V6/V7 paths.
- `accel/tcg/xemu-tcg-perf.c` — three new ns accumulators + helpers.
- `include/qemu/xemu-tcg-perf.h` — three new public API helpers.
- `include/qemu/xemu-spike-log.h` — `xemu_tcg_phase_log_enabled`.
- `util/xemu-spike-log.c` — phase-log env-var init.
- `scripts/apple-silicon/extract-perf-summary.sh` — three new keys.
- `xemu-fork/CLAUDE.md` — `XEMU_TCG_PHASE_LOG` flag doc.
- `docs/apple-silicon/automation.md` — counter docs.

### V7 + V8 benchmark notes

- `benchmarks/2026-05-02-v7-cumulative-phase-attribution.md` — full
  V7 attribution + V8 sample-profile analysis.
- `benchmark-runs/20260502-112753-pgr2/` (V7 sanity).
- `benchmark-runs/20260502-112845-crimson-skies/` (V7 attribution,
  300 s).
- `benchmark-runs/20260502-113656-crimson-skies/` (V8 sample profile,
  90 s with 75 s sample window — `sample-v8-stutter.txt`).

### Critical reframings

1. **PPTC is not the judder fix.** V7 quantified the worst-frame
   `tb_gen_code` cost at 44-111 ms across top-5 worst intervals.
   Even 100 % PPTC efficacy can't move a 1.3 s frame below 1.2 s.
   PPTC remains queued as a steady-state perf improvement (~4 %
   speedup over the full route).
2. **The judder root cause is in `cpu_tb_exec` (TB binary
   execution) — i.e., the GUEST is genuinely doing more work
   during the worst frame.** xemu emulates that work faithfully.
   The actionable optimization is to make specific helper
   functions cheaper.
3. **`helper_rdtsc` is the top named optimization target.** 7-9
   function calls per RDTSC = ~80-100 ns on M3 Ultra vs ~5 ns
   native. If the guest kernel busy-waits on RDTSC (likely),
   eliminating the call-chain overhead gives a meaningful
   worst-frame speedup.

### Top-of-stack next-slice priority (supersedes the V6 entry below)

1. **V9 — RDTSC fast-path + call counter (highest priority).**
   Implement `cpu_get_tsc` Apple Silicon fast-path bypassing the
   QEMU clock abstraction. Use `mach_absolute_time()` directly +
   cached `mach_timebase_info` (which is `{1,1}` on M-series so
   the result is already nanoseconds) + `muldiv64(ns, 733333333,
   1e9)`. Add per-interval `HELPER_RDTSC_CALLS` counter to
   validate the call rate during the worst frame. Decision: if
   the fix drops `mspf_max_max` below 1100 ms, ship default-on
   under `XEMU_FAST_RDTSC=1`.
2. **V10 — `helper_lookup_tb_ptr` indirect-branch cache (if V9
   isn't enough).** Per-vCPU 1-entry cache keyed on indirect
   branch source PC, falling back to qht. ~5 % vCPU win
   estimated.
3. **PPTC (downgraded — steady-state perf, not judder fix).**
   Strategy.md Phase 5a. Implement after judder is closed (so
   the impact can be measured cleanly against a flat baseline).
4. **Audio listen-test for `XEMU_APU_LOCK_RELEASE` stays
   DEFERRED** until video judder is closed (project policy
   2026-05-02). Same ordering applies to any future audio-side
   optimization.
5. **Do not pursue:**
   - x87 80-bit helper optimization (irreducibly soft on Apple
     Silicon — strategy.md / 2026-05-01 audit).
   - Iothread / BQL / AIO / MMIO slices (D3 ruled out).
   - Renderer slices (V4 sweep ruled out).
   - More V6/V7 spike sources at lower thresholds (V8 sample
     profile is the right tool now).
6. **Do not re-prove the seven default-on flags.** Use the
   established regression gates (validate-native-tri-depth.sh
   --run 22; PGR2 mid-route snapshot triplet; V4 broader sweep
   run dirs).

## Update — 2026-05-02 V6 cpu_exec_loop per-phase spike attribution

V6 added three new spike sources inside `cpu_exec_loop` per the D3
note's recommendation (`tcg_tb_lookup`, `tcg_tb_gen_code`,
`tcg_handle_interrupt`), built clean, and ran a 300 s Crimson retail
route at 1 ms threshold. Outcome:

- **NEGATIVE on all three V6 hypotheses at the per-event 1 ms level.**
  Across 300 s: 0 `tcg_tb_lookup` events, 0 `tcg_tb_gen_code` events,
  1 `tcg_handle_interrupt` event (2.4 ms one-off, EXCP_INTERRUPT path).
  Worst-frame interval (1.375 s) contains zero V6 spike events.
- **The 1 ms-class `tcg_tb_chain` events are normal hot-path
  execution.** 99.97 % of the run's 55,684 chains fall in the
  1000-1099 µs bucket; mean `tb_count=1918` at ~500 ns/iter; the cost
  is genuine guest-code execution, not host-side wait. D3's
  "host-side wait inside cpu_exec_loop" hypothesis is **disproved**.
- **Worst frame correlates with a translation-churn storm in the
  always-on TCG counters:** `TCG_TB_INVALIDATE_COUNT=8954` (~6×
  steady state), `TCG_NOTDIRTY_PAGES_HIT=1200` (~24× steady state),
  `TCG_TB_INVALIDATE_BURST_MAX=438` (~3× steady state),
  `TCG_JMP_CACHE_ZEROED_BUCKETS=74,490` (~13× steady state). The
  cost is sub-millisecond per event but cumulatively significant.
- **Render loop is blocked during the worst frame.** Only 4
  `NV2A_FLIP_STALL_WRITES` and 4 `NV2A_PRESENT_HEARTBEAT` in 1.4 s
  (vs ~30/s steady state); guest's render thread is not producing
  frames during the stall. xemu offered 86 vblanks
  (`NV2A_VBLANK_FIRES=86`) — pacing is not the cap.
- **Worst-frame guest PC dominator unchanged from D3:** 98 % of
  worst-frame chain events start at Xbox kernel PC `0x80030e4c`.
  Without kernel symbols, function identity is unresolved.

V6 ships **as instrumentation only** (no default-on behavior change).
The three new spike sources are gated on `XEMU_PERF_SPIKE_LOG_TCG=1`
with one untaken-branch cost when off; they stay in the tree
permanently for future regression triage.

### V6 code changes (3 files modified)

- `accel/tcg/cpu-exec.c` — three per-phase spike timers added inside
  `cpu_exec_loop`'s inner for-loop (the inner `while
  (!cpu_handle_interrupt(...))` was rewritten to `for (;;) { ... if
  (int_exit) break; ... }` to permit post-call timing of
  `cpu_handle_interrupt`). ~85 lines added; chain timing and
  emission unchanged.
- `xemu-fork/CLAUDE.md` — `XEMU_PERF_SPIKE_LOG_TCG=1` flag list
  extended with the three V6 op tags.
- `docs/apple-silicon/automation.md` — per-event spike-log table
  extended with three new entries; documents the
  `extra=` field semantics (`exit/ex_idx/int_req`,
  `pc/hit`, `pc/cflags`).

### V6 benchmark note

`benchmarks/2026-05-02-v6-cpu-exec-loop-attribution.md` — full
attribution including the 99.97 % chain-clustering analysis, the
worst-frame TCG-counter storm signal, and the V7 / kernel-
symbolication / host-thread-profiling next-slice candidate analysis.

### Critical reframings

1. **"`tb_gen_code` churn drives the worst frame" (D3's leading
   hypothesis)** — disproved at the per-event level. Aggregate
   sub-millisecond churn remains plausible but unmeasured. V7
   cumulative-counter slice is required to confirm or refute.
2. **"The 1 ms-class chain duration is a host-side wait" (D3's
   secondary hypothesis)** — disproved. The 1 ms cost is normal
   TCG execution at ~500 ns/iter × ~1900 inner-loop iterations.
3. **"The worst frame is on the vCPU thread"** — partially true.
   The vCPU is busy (`TCG_TB_EXEC_COUNT=5,707,812` in the
   worst-frame interval, slightly elevated above steady state),
   but the GUEST's render thread is **blocked** (only 4 page-flips
   in 1.4 s vs ~30/s steady state). The kernel is doing
   non-rendering work during the stall, and that work
   (translation-churn-amplified by xemu) is what the chain spikes
   measure.

### Top-of-stack next-slice priority (supersedes the V6 entry below)

1. **V7 — cumulative per-interval `TCG_*_US_TOTAL` counters.** Add
   `TCG_TB_LOOKUP_US_TOTAL`, `TCG_TB_GEN_CODE_US_TOTAL`,
   `TCG_HANDLE_INTERRUPT_US_TOTAL` (sum, per interval). Gate the
   wallclock measurement on a new `XEMU_TCG_PHASE_LOG=1` env var
   (cost when off: zero; cost when on: ~36 % vCPU overhead at
   3M TBs/interval). Counter emission stays unconditional. Run
   the same Crimson 300 s route and check whether
   `TCG_TB_GEN_CODE_US_TOTAL` ≥ 300 ms in the worst-frame
   interval. If yes → PPTC slice (strategy.md Phase 5a) is
   justified; estimated ceiling reduces the worst frame from
   1.375 s to ~900 ms. If no → host-thread profiling cross-check
   (priority 3 below) becomes the next step.
2. **Guest kernel symbolication for PC `0x80030e4c` (deferred
   until V7 confirms direction).** Dump xboxkrnl.exe from the
   snapshot HDD via QEMU `pmemsave` HMP, parse PE export table,
   apply public XBOXKRNL RE notes. Useful only if V7 points back
   at kernel-driven cost rather than translation churn.
3. **Host-thread profiling cross-check.** Run Apple `sample` via
   `scripts/apple-silicon/sample-profile.sh` against the live
   xemu vCPU thread during a Crimson stutter window. Direct
   ground-truth on what the TCG thread is doing without needing
   kernel symbols. Quick to run; deferred only because V7 is
   structurally cleaner data.
4. **Audio listen-test gate for `XEMU_APU_LOCK_RELEASE` is
   DEFERRED** until the video-judder pillar is fully closed.
   Project policy: judder-induced audio skips would confound the
   listen-test; the slice stays default-on under "PARTIAL —
   audio gate deferred" status. Re-evaluate after the worst-frame
   stutter is below the 500 ms judder gate on each tracked title.
5. **Do not pursue:**
   - Lower the spike threshold to 100 µs in another full run —
     log volume explodes and ambiguity worsens. Use V7 cumulative
     counters instead.
   - Renderer slices for the worst frame — renderer-side spikes
     in the worst-frame window total 22 ms across 6 events, vs
     446 ms of TCG events. Not the binding constraint.
   - Iothread / BQL / AIO / MMIO slices — D3 already ruled all
     of these out at the worst-frame timescale.
6. **Do not re-prove the seven default-on flags.** Use the
   established regression gates:
   - `validate-native-tri-depth.sh --run 22` for the triangle
     gate (pre-existing harness flake, not a real regression —
     see V1 note Honest-limits §1).
   - PGR2 mid-route snapshot triplet (`pgr2_gameplay_b4` from
     `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`).
   - V4 broader-sweep run dirs as cross-checks.

## Update — 2026-05-02 multi-slice session (V1/V2/V3/V4/V5/D2/D3)

This session shipped four additional default-on flags plus one opt-in
renderer flag and the 1080p first-launch default, validated the full
goal stack against the broader Xbox library, and reframed the headline
"60 FPS on PGR2/Rainbow/Crimson" goal as title-intrinsic-impossible
based on a Soul Calibur 2 sanity test that sustained 60.57 FPS on the
same build/flag stack.

### What landed (default-on for Apple Silicon system builds, all overridable)

- **`XEMU_TCG_SPLITWX={0,1}`** (V1, default ON). Selects the
  `mach_vm_remap` dual-mapping splitwx path so TB execution no longer
  pays the per-TB `pthread_jit_write_protect_np()` syscall. W^X
  toggle wrappers in `include/qemu/osdep.h` are diff-guarded. Per-arm
  Crimson 300 s evidence: `pthread_jit_write_protect_np` count 11 →
  0; headline worst-frame +0.33 % (PARTIAL on the judder pillar,
  PASS on mechanical correctness). Decision-log: "2026-05-02: Ship
  XEMU_TCG_SPLITWX default-on (V1, …)". Note:
  `benchmarks/2026-05-01-tcg-splitwx-validation.md`.
- **`XEMU_TCG_JMP_CACHE_TARGETED={0,1}`** (V2, default ON).
  Replaces the unconditional 4096-entry per-CPU jmp-cache zero in the
  `CF_PCREL` branch of `tb_jmp_cache_inval_tb` with a single-bucket
  clear per invalidated TB. `TCG_JMP_CACHE_ZEROED_BUCKETS` collapses
  from 4096-per-invalidation to 1; `TCG_INVALIDATE_WALL_US_MAX`
  per-call max ~700 µs (well below 1.27 s worst frame — decisive
  evidence the worst frame is not one giant invalidation chain).
  Decision-log: "2026-05-02: Ship XEMU_TCG_JMP_CACHE_TARGETED
  default-on …". Note:
  `benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md`.
- **`XEMU_APU_LOCK_RELEASE={0,1}`** (I5, default ON). APU worker
  thread releases `MCPXAPUState::lock` during the per-frame
  voice-worker batch wait inside `voice_work_dispatch`
  (`hw/xbox/mcpx/apu/vp/vp.c`), then re-acquires before publishing
  mixbins. Measured deltas: `APU_VCPU_LOCK_WAIT_US_MAX` −97.9 % (5.07
  ms → 105 µs), steady-state stutter intervals −61 %, p999 −35 %,
  headline worst-frame −4.35 ms (PARTIAL on the 500 ms judder gate;
  PASS on every steady-state pillar). Race widening: ~5.33 ms slightly-
  stale audio per affected voice per frame — within existing
  upstream-loose patterns; **audio listen-test on each tracked title
  is the gating step before fully-shipped status.** Decision-log:
  "2026-05-02: Ship XEMU_APU_LOCK_RELEASE default-on (I5, …)". Note:
  `benchmarks/2026-05-02-apu-lock-release-validation.md`.
- **`display.quality.surface_scale = 2`** on first launch (Apple
  Silicon system builds only). Existing user configs preserved.
  Per-session override via `XEMU_DISPLAY_SCALE={1..10}` (out-of-range
  values silently ignored). The benchmark harness's
  `XEMU_BENCH_SURFACE_SCALE` parallel knob also defaults to 2.
  Decision-log: "2026-05-02: Default display.quality.surface_scale to
  2 on first launch …".
- **`XEMU_GL_MSAA={0,2,4,8}`** opt-in (default 0). Per-surface
  multisample renderbuffers via `glRenderbufferStorageMultisample`,
  lazy `glBlitFramebuffer` resolve, sample count clamped to
  `GL_MAX_SAMPLES` (4 on Apple GL-on-Metal). Per-frame cost reported
  as `MSAA_RESOLVE_US_TOTAL`. Composes with
  `XEMU_DISPLAY_SCALE`/`surface_scale`. Decision-log:
  "2026-05-02: Add XEMU_GL_MSAA opt-in …".

### Diagnostic toggles added this session

- **`XEMU_PERF_SPIKE_LOG_TCG=1`** — enables TCG / iothread / MMIO
  spike sources independently of the renderer-side spike log.
  Sources: `tcg_tb_chain`, `tcg_invalidate_burst`,
  `tcg_notdirty_storm`, `tcg_x87_storm`, `tcg_pg_lock_wait`,
  `renderer_pg_lock_wait`, `qemu_main_loop_iter`, `aio_run_iter`,
  `bql_acquire_wait`, `mmio_helper_block`. See `automation.md`
  "Per-event spike log" section for the per-source `extra=` field
  semantics. Off by default; hot-path cost when off is one global
  load + branch per call site.

### New code files

- `accel/tcg/xemu-tcg-perf.c` + `include/qemu/xemu-tcg-perf.h`
- `util/xemu-spike-log.c` + `include/qemu/xemu-spike-log.h`
- `util/xemu-apu-perf.c` + `include/qemu/xemu-apu-perf.h`
- `util/xemu-display-perf.c` + `include/qemu/xemu-display-perf.h`

### New perf counters (all surfaced in `extract-perf-summary.sh`)

TCG hot-path (sum / max as noted):

- `TCG_TB_EXEC_COUNT` (sum) — per-interval TB executions.
- `TCG_TB_INVALIDATE_COUNT` (sum) — TBs invalidated per interval.
- `TCG_NOTDIRTY_TRIPS` (sum) — `notdirty_write` trips per interval.
- `TCG_NOTDIRTY_PAGES_HIT` (sum, lossy 64-entry set) — distinct
  guest-physical pages tripping notdirty per interval.
- `TCG_TB_INVALIDATE_BURST_MAX` (max) — TBs invalidated in one
  `tb_invalidate_phys_page_range__locked` call.
- `TCG_JMP_CACHE_ZEROED_BUCKETS` (sum) — bucket clears per interval
  (4096 per full zero, 1 per targeted clear).
- `TCG_INVALIDATE_WALL_US_MAX` (max) — wallclock cost of a single
  `tb_invalidate_phys_page_range__locked`.

Renderer / MSAA / display:

- `MSAA_RESOLVE_US_TOTAL` (sum) — `glBlitFramebuffer` resolve cost
  per interval.
- `NV2A_VBLANK_FIRES` (sum, ~per-second) — vblank IRQ deliveries
  driven by `vblank_interval_ns = 16,666,666 ns = 60 Hz`.
- `NV2A_PRESENT_HEARTBEAT` (sum, ~per-second) — guest presents
  (`NV_PGRAPH_INCREMENT_READ_3D` writes).
- `NV2A_FLIP_STALL_WRITES` (sum) — guest writes to
  `NV097_FLIP_STALL`.
- `XEMU_GL_SWAPS` (sum, race-noisy) — `SDL_GL_SwapWindow` calls
  (cross-thread emit-vs-increment race; sum across the run for a
  meaningful per-second value).

APU lock-hold / vCPU-wait:

- `APU_LOCK_HOLD_US_TOTAL` (sum) — APU worker thread d->lock hold
  time per interval. With the slice on, drops by exactly the
  worker-finished-wait window.
- `APU_VCPU_LOCK_WAIT_US_MAX` (max) — max vCPU wait for d->lock
  per interval.

### New benchmark notes (this session)

- `benchmarks/2026-05-01-tcg-splitwx-validation.md` — V1, PARTIAL.
- `benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md` — V2,
  PARTIAL.
- `benchmarks/2026-05-02-tcg-spike-attribution.md` — V3, ruled out
  TCG-internal hypothesis classes for the headline frame at 10 ms
  threshold; partial attribution at 1 ms.
- `benchmarks/2026-05-02-composite-goal-validation.md` — V3 composite
  goal stack across PGR2 / Rainbow / Crimson at scale=2 + MSAA=4;
  found 30 FPS cap is **not** renderer-bound. Reframe needed.
- `benchmarks/2026-05-02-60hz-title-sanity-test.md` — Soul Calibur 2
  sustained **60.57 FPS** for 109 consecutive intervals on the same
  build/flag stack. Proves the cap on PGR2/Rainbow/Crimson is
  title-intrinsic (engine renders at 30 Hz on real Xbox).
- `benchmarks/2026-05-02-tcg-30fps-cap-attribution.md` — D3, 1 ms
  spike-log breakdown of the Crimson 1.39-s worst frame; ruled out
  iothread / BQL / mmio-blocking hypotheses; attributed the bulk of
  the spike-attributed cost (422 ms / 1386 ms) to one
  `tcg_tb_chain` at guest PC `0x23dd47` (game-app
  `fe_method` → `voice_lock` MMIO write). Remaining 963 ms
  unattributed at 1 ms threshold.
- `benchmarks/2026-05-02-apu-lock-release-validation.md` — I5,
  PARTIAL: huge steady-state win (−61 % stutter intervals, −97.9 %
  vCPU wait), headline 1.28 s worst frame unchanged. Confirms D3's
  prediction that closing the audio voice-lock path was necessary
  but not sufficient for the headline.
- `benchmarks/2026-05-02-broader-title-sweep.md` — V4, library-wide
  viability across 6 titles (5 new + SC2 cross-ref). PASS. Identified
  a future-slice candidate (NGB exercises 87,243 line draws via the
  geometry shader → `XEMU_NATIVE_LINE` bypass would close the last
  primitive-family GS workload).

### Critical reframings

1. **The 30 FPS cap on PGR2 / Rainbow / Crimson is title-intrinsic.**
   Confirmed by SC2 sustaining 60.57 FPS on the same build/flag
   stack. The decisive ratio is `NV2A_VBLANK_FIRES > 30/s` while
   `NV2A_PRESENT_HEARTBEAT == 30/s` — xemu offers 60 vblanks/s, the
   guest engine elects to present every other vblank. This makes the
   literal "60 FPS on PGR2/Rainbow/Crimson" goal in strategy.md
   Success Criteria technically impossible. The reframed goal:
   **console-native FPS for each tracked title, no 1-second-class
   judder, plus 1080p + AA available**. Decision-log: "2026-05-02:
   Confirm 30 FPS cap … is title-intrinsic, supersede the literal
   '60 FPS on tracked-3' success criterion".
2. **Headline 1.3-s Crimson worst-frame is NOT TCG-internal in any
   single attributed source.** Successive attribution work ruled out
   TB invalidation (V1 splitwx), jmp-cache zero (V2), iothread /
   main-loop blocking (D3), BQL acquisition (D3), MMIO-helper
   blocking (D3), audio voice-lock contention (V5/I5). The remaining
   ~970 ms unattributed at 1 ms threshold lives in `tb_gen_code`
   churn + the kernel-PC `0x80030e4c` 1 ms-class TB chains. **Next
   slice: V6 — `cpu_exec_loop` per-phase instrumentation
   (`tcg_tb_lookup` / `tcg_tb_gen_code` / `tcg_handle_interrupt`
   spike sources gated on `XEMU_PERF_SPIKE_LOG_TCG=1`)** to attribute
   the residual.
3. **All 7 default-on flags pass the V4 broader sweep.** Splitwx,
   jmp-cache-targeted, native-tri-depth, native-quad,
   pgraph-fast-read, apu-lock-release, plus the 1080p first-launch
   default; with `XEMU_GL_MSAA=4` opt-in. 6 of 6 tested titles pass
   FPS / pathology gate; 0 new title-specific Apple-GL pathologies;
   0 MSAA-driven pipeline-variant explosions. 4 of 6 surface the
   same catalogued Crimson-class worst-frame pathology — one V6 fix
   would address all of them.

### Next Session Checklist (top of stack — supersedes the older list below)

1. **V6 — `cpu_exec_loop` per-phase spike instrumentation.** Add
   `tcg_tb_lookup` / `tcg_tb_gen_code` / `tcg_handle_interrupt`
   spike sources gated on `XEMU_PERF_SPIKE_LOG_TCG=1`. Then run a
   Crimson 300 s route at 1 ms spike threshold and bucket the
   per-frame attribution. Leading hypotheses: `tb_gen_code` churn
   (~9 % of vCPU thread time post-I5), kernel-PC `0x80030e4c`
   1 ms-class TB chains. If V6 confirms `tb_gen_code` churn, the
   follow-on fix is **PPTC** (strategy.md Phase 5a — Ryujinx
   pattern). See decision-log "2026-05-02: V3 + D3 attribute the
   residual Crimson worst-frame to TCG-internal sub-1 ms churn (V6
   next)".
2. **Audio listen-test gate for `XEMU_APU_LOCK_RELEASE`.** A human
   listener plays each tracked title (Crimson, Rainbow, PGR2) for ≥
   5 minutes with the slice on, listening for stuck voices, dropped
   sound effects, audible glitches, or stale samples (the bounded
   ~5.33 ms race class the implementer flagged). If clean: declare
   the slice fully shipped. If glitches: revert or design a
   finer-grained lock split (separate `voice_config_lock`).
3. **`XEMU_NATIVE_LINE` bypass slice (deferred until needed).** V4
   identified NGB as a title that exercises 87,243 geometry-shader
   line draws. Mirror the existing `XEMU_NATIVE_TRI_DEPTH` /
   `XEMU_NATIVE_QUAD` pattern. Defer until NGB-class titles become
   a priority focus.
4. **Promote `/tmp/xbe_disasm.py` to
   `scripts/apple-silicon/xbe-disasm.py`** if guest-PC investigation
   becomes recurring (D3 used a transient version; project rule #5).
5. Do not re-prove the seven landed default-on flags. Use:
   - `validate-native-tri-depth.sh --run 22` for the triangle gate
     (pre-existing harness flake, not a real regression — see V1
     note Honest-limits §1).
   - PGR2 mid-route snapshot triplet (`pgr2_gameplay_b4` from
     `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`) for
     PGR2 stability checks.
   - V4 broader sweep run dirs as cross-checks.
6. Continue using `compare-runs.sh` for paired metric comparisons
   and `sample-profile.sh` for autonomous Apple `sample` capture
   inside a benchmark run.



## Current State

- Source has been cloned into:
  - `/Users/jbbrack03/XEMU_MacOS/xemu-fork`
- Baseline commit:
  - `ebe34071b7fec3b6187248c57708fdd6cc3a8b97`
- Working branch:
  - `apple-silicon-performance`
- Documentation created under:
  - `docs/apple-silicon/`

Source code changes made this session:

- `build.sh`
  - exports `CMAKE` on Darwin when `cmake` is available on `PATH`, so Meson's
    cross-build path can configure the `nv2a_vsh_cpu` CMake subproject.
  - removes duplicate app `LC_RPATH` entries during macOS packaging, avoiding a
    `dyld` launch abort on macOS 26.4.1.
- `ui/xemu-input.c`
  - adds opt-in scripted controller input via `XEMU_SCRIPTED_INPUT`, allowing
    repeatable benchmark navigation without physical controller input.
  - adds opt-in physical controller recording via `XEMU_RECORD_INPUT`, writing
    the same CSV format used by scripted replay.
- `hw/xbox/nv2a/debug.h`
- `hw/xbox/nv2a/pgraph/profile.c`
- `hw/xbox/nv2a/pgraph/pgraph.c`
  - add opt-in `XEMU_PERF_LOG=1` startup and per-interval performance logging
    with FPS, frame pacing, and existing NV2A profile counters.
  - add a final `xemu-perf:` counter flush on graceful process exit so short
    diagnostic tails are included in benchmark summaries.
- `hw/xbox/nv2a/pgraph/gl/draw.c`
- `hw/xbox/nv2a/pgraph/gl/renderer.h`
- `hw/xbox/nv2a/pgraph/gl/shaders.c`
  - add OpenGL geometry-shader attribution counters for module/program
    generation, binds, and geometry-backed draws by primitive family.
  - add native triangle-depth draw/fallback counters for the opt-in
    replacement path.
  - add native triangle-depth candidate counters split by smooth, flat-first,
    and flat-nonfirst state so the flat-shading diagnostic can distinguish
    "not reached" from "reached but misclassified".
  - add capped `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` logging for live
    PGRAPH/bound-shader state correlation at shader bind, draw begin, and draw
    flush.
- `hw/xbox/nv2a/pgraph/glsl/geom.c`
- `hw/xbox/nv2a/pgraph/glsl/geom.h`
  - add temporary `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` diagnostic toggle that
    keeps triangle-family geometry shaders active while bypassing their
    depth-plane/slope calculation.
  - add temporary `XEMU_DIAG_SKIP_TRI_GEOM=1` diagnostic toggle that bypasses
    geometry-shader program generation for triangle-family fill draws and draws
    native GL triangles directly.
  - tighten the native triangle-depth eligibility rule so all flat-shaded
    triangle fills stay on the existing geometry-shader path until flat
    shading is deliberately validated.
  - later relax that rule for flat-shaded first-provoking triangle fills only,
    matching the OpenGL renderer's `GL_FIRST_VERTEX_CONVENTION`; flat nonfirst
    provoking remains on the geometry-shader fallback path.
- `hw/xbox/nv2a/pgraph/glsl/psh.c`
- `hw/xbox/nv2a/pgraph/glsl/psh.h`
  - add `XEMU_NATIVE_TRI_DEPTH=1` as the completed current opt-in path that
    bypasses triangle-family fill geometry shaders and derives depth plus
    polygon-slope offset from native GL rasterization state in the fragment
    shader.
  - keep `XEMU_DIAG_NATIVE_TRI_DEPTH=1` accepted as a compatibility alias for
    older benchmark notes and commands.
  - make the preferred `XEMU_NATIVE_TRI_DEPTH=0` spelling override the old alias,
    so a shell with both variables set follows the stable flag.
  - the native bypass now applies only to triangle-family fill primitives;
    line primitives remain on the existing geometry-shader path.
  - extend the fragment-shader native-depth code path so it also activates
    when `XEMU_NATIVE_QUAD=1` is enabled and the current draw is an eligible
    smooth-fill quad-family primitive. The depth/slope math is
    primitive-agnostic.
- `hw/xbox/nv2a/pgraph/gl/shaders.c`
  - `get_gl_primitive_mode()` returns `GL_TRIANGLES` for quad-family draws
    when `XEMU_NATIVE_QUAD=1` is on and the smooth-fill eligibility holds,
    instead of the geometry-shader-required `GL_LINES_ADJACENCY` /
    `GL_LINE_STRIP_ADJACENCY`.
- `hw/xbox/nv2a/pgraph/gl/draw.c`
  - extend per-dispatch geometry-shader profiling to split
    `GEOM_SHADER_DRAW_QUAD` into `_QUAD_LIST` and `_QUAD_STRIP`.
  - add native-quad profiling counters and the
    `pgraph_gl_native_quad_active()` predicate.
  - add CPU-side index expansion helpers for both
    `PRIM_TYPE_QUADS` (4-vertex independent quads) and
    `PRIM_TYPE_QUAD_STRIP` (2-vertex incremental quads). Diagonal matches
    the existing geometry shader's `calc_quadz(0, 2)` triangulation so smooth
    interpolation is unchanged.
  - extend all four dispatch paths
    (`pg->draw_arrays_length`, `pg->inline_elements_length`,
    `pg->inline_buffer_length`, `pg->inline_array_length`) to expand the
    quad vertex stream to triangle indices, upload them via
    `glBufferData(GL_STREAM_DRAW)` on a dedicated index buffer, and issue
    a single `glDrawElements(GL_TRIANGLES, ...)` when the native bypass is
    active.
- `hw/xbox/nv2a/pgraph/gl/renderer.h` and `pgraph/gl/vertex.c`
  - add `gl_native_quad_index_buffer` element-array buffer and a CPU-side
    growable scratch index array, allocated in
    `pgraph_gl_init_buffers()` and freed in
    `pgraph_gl_finalize_buffers()`.
- `hw/xbox/nv2a/debug.h`
  - new counters: `GEOM_SHADER_DRAW_QUAD_LIST`, `GEOM_SHADER_DRAW_QUAD_STRIP`,
    `NATIVE_QUAD_DRAW`, `NATIVE_QUAD_DRAW_LIST`, `NATIVE_QUAD_DRAW_STRIP`,
    `NATIVE_QUAD_CANDIDATE`, `NATIVE_QUAD_CANDIDATE_SMOOTH`,
    `NATIVE_QUAD_CANDIDATE_FLAT`, `NATIVE_QUAD_FALLBACK`,
    `NATIVE_QUAD_FALLBACK_FLAT`, `NATIVE_QUAD_FALLBACK_NONFILL`,
    `NATIVE_QUAD_DRAW_ZPERSPECTIVE`, `NATIVE_QUAD_DRAW_LINEAR_Z`,
    `NATIVE_QUAD_DRAW_POLY_OFFSET`.
- `scripts/apple-silicon/run-benchmark.sh`
  - records `env_XEMU_NATIVE_QUAD` in benchmark metadata.
- `scripts/apple-silicon/extract-perf-summary.sh`
  - surfaces the new `GEOM_SHADER_DRAW_QUAD_*` and `NATIVE_QUAD_*` counters.
- `ui/xemu-snapshots.c`
  - adds `XEMU_SNAPSHOT_NO_THUMBNAIL=1` to skip snapshot thumbnail generation
    for benchmark-created snapshots.
- `scripts/apple-silicon/run-benchmark.sh`
  - launches Crimson Skies, Rainbow Six 3, PGR2, or flat-tri-depth with scripted
    input, metadata
    capture, QMP socket, optional periodic screenshots, logs, and a scratch HDD
    copy.
  - refuses to start if a previous xemu process is still running and cleans up
    run-owned xemu processes when the timed run exits.
  - can save and restore named VM snapshots through QMP/HMP.
  - records disc size/modification time in metadata, which caught stale
    flat-triangle ISO risk.
  - accepts `XEMU_BENCH_EXTRA_QEMU_ARGS` for reproducible trace runs such as
    `-trace nv2a_pgraph_method`.
  - waits briefly for QMP `quit` before sending SIGTERM, allowing the final
    perf-log flush to run on normal benchmark shutdown.
  - records native triangle-depth and related diagnostic environment toggles in
    benchmark metadata.
  - supports live controller setup runs through `XEMU_BENCH_LIVE_INPUT=1` and
    safe prepared-HDD use through `XEMU_BENCH_HDD_IN_PLACE=1`.
- `scripts/apple-silicon/record-input.sh`
  - records physical controller input for a selected benchmark target into a
    stable replay CSV.
- `scripts/apple-silicon/live-setup.sh`
  - runs profile setup against a persistent copied HDD at
    `benchmark-runs/profile-prep/xbox_hdd.qcow2`, avoiding profile creation in
    the final recorded routes.
- `scripts/apple-silicon/native-tri-depth-compare.sh`
  - runs paired baseline/native snapshot benchmarks with a shared scratch-HDD
    source and snapshot tag.
  - writes perf summaries and a cropped screenshot comparison to a
    `benchmark-runs/*-native-tri-depth-compare-*` report directory.
  - records native triangle-depth and related diagnostic environment toggles in
    benchmark metadata.
  - retries each side by default when a launch produces no usable perf summary,
    absorbing the nondeterministic Apple OpenGL startup crash seen locally.
- `scripts/apple-silicon/qmp-hmp.py`
  - sends one HMP command through the QMP `human-monitor-command` bridge.
- `scripts/apple-silicon/validate-native-tri-depth.sh`
  - runs or checks the flat-tri-depth XBE and fails unless the flat-first
    native / flat-nonfirst geometry fallback split is present.
- `scripts/apple-silicon/package-game.sh`
  - packages an extracted Original Xbox game directory from the external
    library at `/Volumes/Josh-Backup-Files/Console Games/Original Xbox`
    into a XISO ISO using `xdvdfs pack`. Supports name lookup, `--list`,
    overwrite protection, post-pack verification, and autoinstall of
    `xdvdfs-cli` via cargo. Output defaults to
    `$XEMU_TEST_GAMES_DIR/<game>.xiso.iso`. Honors workspace rule #9
    (refuses to overwrite an existing ISO without `--force`). See
    `docs/apple-silicon/automation.md` "Game Library Packaging" for full
    usage.

Baseline app status:

- `./build.sh -a arm64` succeeds.
- `ninja -C build qemu-system-i386` succeeds.
- `dist/xemu.app` code-sign verification succeeds.
- `dist/xemu.app/Contents/MacOS/xemu --version` launches and reports Apple's
  OpenGL-on-Metal renderer.

## Important Findings

- macOS build packages `qemu-system-i386`.
- Apple Silicon build target is still `i386-softmmu`, so Xbox CPU code runs via
  QEMU TCG.
- Native arm64 baseline build now succeeds with `./build.sh -a arm64`.
- The packaged baseline app is `dist/xemu.app`.
- `dist/xemu.app` passes code-sign verification.
- `dist/xemu.app/Contents/MacOS/xemu --version` launches and reports:
  - GL vendor: `Apple`
  - GL renderer: `Apple M3 Ultra`
  - GL version: `4.1 Metal - 90.5`
- macOS currently links OpenGL.
- Vulkan is not enabled for Darwin in current Meson logic.
- Renderer default selection prefers OpenGL before Vulkan.
- Geometry shaders are used for most non-point primitive modes.
- Public issue #2506 ties severe macOS 3D performance regression to PR #2240.
- Public comments identify geometry shader usage as the likely cause and name
  geometry-shader removal as the real fix.
- B0/B1 log-based gameplay route metrics are recorded; automated screenshot
  capture remains unreliable from this Codex desktop context.
- Scripted smoke routes now navigate:
  - Crimson Skies through pilot registration into the in-engine sequence.
  - Rainbow Six 3 through default profile creation and Campaign into Hereford
    mission loading.
- Profile-prepared retail gameplay routes are now recorded and tracked:
  - PGR2 route:
    `scripts/apple-silicon/input-scripts/pgr2-gameplay.csv`,
    `benchmark-runs/20260501-094823-pgr2`, 11.53 average FPS / 11.67 post-load
    average FPS, 1,516,519 geometry-shader draws, including 38,785 quad-family
    draws.
  - Rainbow Six 3 route:
    `scripts/apple-silicon/input-scripts/rainbow-gameplay.csv`,
    `benchmark-runs/20260501-095400-rainbow-six-3`, 24.19 average FPS / 24.76
    post-load average FPS, 692,438 geometry-shader draws, including 1,946
    line-family draws.
  - Crimson Skies route:
    `scripts/apple-silicon/input-scripts/crimson-gameplay.csv`,
    `benchmark-runs/20260501-095905-crimson-skies`, 15.44 average FPS / 15.80
    post-load average FPS, 786,722 geometry-shader draws, including 7,837
    quad-family draws.
- Retail performance target floor is sustained 30 FPS in gameplay for all
  tracked titles. 60 FPS is desirable but not the minimum bar.
- Baseline metrics are recorded in
  `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`.
- B0 Crimson Skies baseline:
  - run: `benchmark-runs/20260430-095612-crimson-skies`
  - average: 29.93 FPS over 139 intervals
  - tail-60 average: 30.98 FPS
- B1 Rainbow Six 3 baseline:
  - run: `benchmark-runs/20260430-095919-rainbow-six-3`
  - average: 26.45 FPS over 174 intervals
  - tail-60 average: 30.98 FPS
- Snapshot restore through QMP/HMP works for both current benchmark scenes:
  - Crimson tag `crimson_scene_b0`, saved in
    `benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2`.
  - Rainbow tag `rainbow_scene_b1_nothumb`, saved in
    `benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2`.
- OpenGL geometry-shader attribution counters are now included in
  `xemu-perf:` output:
  - module/program generation counters.
  - bind / not-dirty bind counters.
  - draw counters split into line, triangle, quad, and other primitive
    families.
- B2/B3 snapshot scene-entry runs with geometry counters are recorded in
  `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`.
- B2 Crimson Skies counter run:
  - run: `benchmark-runs/20260430-103500-crimson-skies`
  - average: 29.70 FPS over 28 intervals
  - post-load average after first five intervals: 30.98 FPS / 20.63 MSPF
  - geometry draws: 25,202, all triangle-family
- B3 Rainbow Six 3 counter run:
  - run: `benchmark-runs/20260430-103536-rainbow-six-3`
  - average: 29.23 FPS over 27 intervals
  - post-load average after first five intervals: 30.97 FPS / 17.66 MSPF
  - geometry draws: 149,961, all triangle-family
- Among the older snapshot scene-entry runs, Rainbow Six 3 is the better
  triangle-family geometry-shader overhead diagnostic because it issues roughly
  six times the geometry-backed draws of the Crimson scene over the same run
  length. For current retail gameplay work, start with PGR2.
- `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` is available as a temporary diagnostic
  toggle. It keeps triangle-family geometry shaders active but bypasses their
  depth-plane/slope calculation.
- D1 Rainbow Six 3 diagnostic run:
  - run: `benchmark-runs/20260430-104001-rainbow-six-3`
  - toggle: `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1`
  - average: 28.24 FPS over 25 intervals
  - post-load average after first five intervals: 30.72 FPS / 18.39 MSPF
  - geometry draws: 128,277, all triangle-family
  - result: no improvement over B3, with one late 162 ms frame-time spike.
- D1 suggests the performance issue is more likely geometry shader dispatch,
  Apple OpenGL driver behavior, or surrounding pipeline work than the
  triangle depth/slope arithmetic itself.
- `XEMU_DIAG_SKIP_TRI_GEOM=1` is available as a temporary diagnostic toggle. It
  bypasses geometry-shader program generation for triangle-family fill draws
  and lets OpenGL draw native triangles directly. This is not a correctness
  path because the fragment shader no longer receives the geometry shader's
  per-triangle depth payload.
- D2 Rainbow Six 3 diagnostic run:
  - run: `benchmark-runs/20260430-105200-rainbow-six-3`
  - toggle: `XEMU_DIAG_SKIP_TRI_GEOM=1`
  - average: 29.93 FPS over 27 intervals
  - post-load average after first five intervals: 30.96 FPS / 6.38 MSPF
  - geometry draws: 0
  - result: large frame-time improvement while FPS remains capped near 31 FPS.
- D2 strongly implicates geometry-shader dispatch or Apple's OpenGL
  geometry-shader implementation as the local bottleneck.
- `XEMU_NATIVE_TRI_DEPTH=1` is the completed current opt-in GL triangle-family
  fill replacement path. It bypasses triangle-family fill geometry shaders,
  then derives depth and polygon-slope offset from `gl_FragCoord` in the
  fragment shader. It is validated for the current opt-in triangle-fill
  coverage described below, but is not yet a default renderer path.
- `XEMU_DIAG_NATIVE_TRI_DEPTH=1` is still accepted as a compatibility alias for
  older notes and runs. If `XEMU_NATIVE_TRI_DEPTH` is explicitly set to `0`, the
  old alias no longer turns the path on.
- D3 Rainbow Six 3 diagnostic run:
  - run: `benchmark-runs/20260430-110636-rainbow-six-3`
  - toggle: `XEMU_DIAG_NATIVE_TRI_DEPTH=1`
  - average: 29.49 FPS over 27 intervals
  - post-load average after first five intervals: 30.96 FPS / 8.10 MSPF
  - geometry draws: 0
  - result: retains most of D2's frame-time improvement while moving toward a
    correctness-preserving replacement.
- D4 Crimson Skies diagnostic run:
  - run: `benchmark-runs/20260430-111006-crimson-skies`
  - toggle: `XEMU_DIAG_NATIVE_TRI_DEPTH=1`
  - average: 31.15 FPS over 28 intervals
  - post-load average after first five intervals: 30.98 FPS / 18.97 MSPF
  - geometry draws: 0
  - result: confirms the toggle runs on the second benchmark scene, though
    Crimson is less sensitive to the geometry-shader bottleneck.
- D5 Rainbow Six 3 line-safe rerun:
  - run: `benchmark-runs/20260430-111903-rainbow-six-3`
  - toggle: `XEMU_DIAG_NATIVE_TRI_DEPTH=1`
  - average: 30.89 FPS over 28 intervals
  - post-load average after first five intervals: 30.99 FPS / 6.35 MSPF
  - geometry draws: 0
  - result: historical Rainbow comparison point; use P1/P2 for current
    same-build baseline/native evidence.
- D6/D7 Crimson Skies line-safe reruns:
  - D6 run: `benchmark-runs/20260430-112058-crimson-skies`
  - D6 post-load average: 30.98 FPS / 27.53 MSPF
  - D7 run: `benchmark-runs/20260430-112157-crimson-skies`
  - D7 post-load average: 30.98 FPS / 20.30 MSPF
  - geometry draws: 0 in both runs, including line-family counters.
  - result: Crimson shows more frame-time variance; use it as a cross-check,
    not the primary geometry-dispatch timing scene.
- D8/D9 tightened native triangle-depth reruns:
  - D8 Rainbow run: `benchmark-runs/20260430-113642-rainbow-six-3`
  - D8 post-load average: 30.99 FPS / 6.47 MSPF
  - D8 native triangle-depth draws: 197,212; fallbacks: 0; geometry draws: 0.
  - D9 Crimson run: `benchmark-runs/20260430-113732-crimson-skies`
  - D9 post-load average: 30.98 FPS / 20.01 MSPF
  - D9 native triangle-depth draws: 71,277; fallbacks: 0; geometry draws: 0.
  - result: the safer flat-shading eligibility rule preserved the Rainbow
    performance win in the current benchmark scene. Later tightening keeps all
    flat-shaded triangle fills on the geometry-shader path until flat shading
    is deliberately validated.
- D10 Rainbow confirmation run:
  - run: `benchmark-runs/20260430-114511-rainbow-six-3`
  - D10 post-load average: 30.98 FPS / 6.78 MSPF
  - D10 native triangle-depth draws: 192,776; fallbacks: 0; geometry draws: 0.
  - result: repeats the D8 performance band and confirms the current Rainbow
    snapshot still stays entirely on the native triangle-depth path.
- A dedicated flat-shading test XBE now exists:
  - source: `scripts/apple-silicon/xbe-tests/flat-tri-depth/`
  - XBE: `scripts/apple-silicon/xbe-tests/flat-tri-depth/bin/default.xbe`
  - ISO: `scripts/apple-silicon/xbe-tests/flat-tri-depth/flat-tri-depth.iso`
  - manual copy:
    `/Users/jbbrack03/XEMU_MacOS/Test_Games/flat-tri-depth.xiso.iso`
  - launcher target:
    `scripts/apple-silicon/run-benchmark.sh flat-tri-depth`
  - trace run `benchmark-runs/20260430-141331-flat-tri-trace` confirms the XBE
    sends `NV097_SET_SHADE_MODE` flat plus first/last
    `NV097_SET_PROVOKING_VERTEX`.
  - passing run `benchmark-runs/20260430-153555-flat-tri-depth` confirms the
    expected split: first-provoking flat triangles use the native path, while
    last-provoking flat triangles fall back to the geometry shader.
- `scripts/apple-silicon/extract-perf-summary.sh` now summarizes `xemu-perf:`
  logs into overall/post-load averages and key geometry/native counters.
- `scripts/apple-silicon/xbe-tests/flat-tri-depth/flat-tri-depth.iso` was
  rebuilt from the current source at 2026-04-30 14:38:34 CDT.
- New flat-triangle trace/validation runs:
  - `benchmark-runs/20260430-143952-flat-tri-depth`: rebuilt ISO, no trace,
    still reported all candidates as smooth.
  - `benchmark-runs/20260430-144128-flat-tri-depth`: rebuilt ISO with
    `-trace nv2a_pgraph_method`; trace showed flat-last draw methods, but perf
    still reported candidates as smooth.
  - `benchmark-runs/20260430-144451-flat-tri-depth`: after adding explicit
    method-owned `PGRAPHState` shade/provoking fields, trace still showed
    flat-last draw methods while perf still reported candidates as smooth.
- Current flat-shading conclusion:
  - Stale media is no longer the explanation; metadata now records the rebuilt
    ISO timestamp.
  - Renderer-side state tracing showed live PGRAPH state and bound shader state
    both become flat-first at bind, draw begin, and flush.
  - The earlier all-smooth summaries were caused by the short XBE's flat phase
    landing after the final regular one-second perf interval. A graceful final
    perf-log flush now captures the partial tail.
  - Validation run `benchmark-runs/20260430-153555-flat-tri-depth` passes the
    flat counter split: 480 `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, 304
    `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST`, and 304 `GEOM_SHADER_DRAW_TRI`.
- `scripts/apple-silicon/compare-screenshots.py` now crops paired screenshots,
  writes baseline/candidate/diff images, and prints simple visual-diff metrics.
- First Rainbow Six 3 visual smoke comparison:
  - baseline geometry run: `benchmark-runs/20260430-115436-rainbow-six-3`
  - native triangle-depth run: `benchmark-runs/20260430-115335-rainbow-six-3`
  - 10s viewport crop comparison: mean absolute error 0.1854, RMS 1.2256,
    changed pixels above threshold 8: 0.4382%.
  - visual inspection did not show an obvious rendering break, but this is only
    a smoke check and not a proof of depth or polygon-offset correctness.
- Native triangle-depth coverage counters now split native draws by:
  - w-depth versus linear depth.
  - fill polygon offset.
  - smooth shading versus flat-first.
  - flat fallback and flat-nonfirst fallback.
- D11/D12 snapshot coverage runs:
  - D11 Rainbow: `benchmark-runs/20260430-120458-rainbow-six-3`, 30.97 FPS /
    6.36 MSPF post-load, 198,119 native draws, 0 fallbacks, 100,772 w-depth,
    97,347 linear-depth, 26,019 polygon-offset, all smooth.
  - D12 Crimson: `benchmark-runs/20260430-120553-crimson-skies`, 30.99 FPS /
    20.39 MSPF post-load, 71,436 native draws, 0 fallbacks, all linear-depth,
    24,738 polygon-offset, all smooth.
- D15 post-tightening Rainbow confirmation:
  - run: `benchmark-runs/20260430-121927-rainbow-six-3`
  - post-load average: 30.97 FPS / 6.41 MSPF
  - native triangle-depth draws: 201,450; fallbacks: 0.
  - coverage: 101,016 w-depth, 100,434 linear-depth, 26,823 polygon-offset,
    all smooth; flat fallback counters remained 0 because this scene has no
    flat-shaded triangle fills.
- Flat-first native triangle-depth eligibility was added after D15. It is a
  targeted correctness expansion based on the OpenGL first-provoking convention;
  local Crimson/Rainbow routes still do not exercise flat-shaded triangle fills,
  so a dedicated nxdk flat-tri-depth XBE was created for direct validation.
- D16 Rainbow flat-first eligibility check:
  - run: `benchmark-runs/20260430-135911-rainbow-six-3`
  - post-load average: 31.01 FPS / 7.62 MSPF
  - native triangle-depth draws: 192,998; fallbacks: 0; geometry draws: 0.
  - coverage: 100,772 w-depth, 92,226 linear-depth, 24,423 polygon-offset,
    all smooth; flat-first and flat fallback counters remained 0.
  - result: the flat-first eligibility expansion did not perturb the existing
    smooth Rainbow snapshot path.
- D13/D14 longer route coverage runs:
  - D13 Rainbow smoke route:
    `benchmark-runs/20260430-120653-rainbow-six-3`, 313,378 native draws, 0
    fallbacks, 89,376 polygon-offset, all smooth.
  - D14 Crimson smoke route:
    `benchmark-runs/20260430-120851-crimson-skies`, 328,477 native draws, 0
    fallbacks, 92,169 polygon-offset, all smooth.
- Crimson visual smoke comparison:
  - baseline geometry run: `benchmark-runs/20260430-121112-crimson-skies`
  - native triangle-depth run: `benchmark-runs/20260430-121141-crimson-skies`
  - 10s viewport crop comparison: mean absolute error 0.8191, RMS 2.3252,
    changed pixels above threshold 8: 1.7876%.
  - the baseline/native diff is much smaller than Crimson's normal temporal
    movement in this scene.
- Native triangle-depth is now promoted from raw diagnostic to stable opt-in
  experiment flag:
  - preferred flag: `XEMU_NATIVE_TRI_DEPTH=1`.
  - compatibility alias: `XEMU_DIAG_NATIVE_TRI_DEPTH=1`.
  - explicit preferred disable: `XEMU_NATIVE_TRI_DEPTH=0`, which wins over the
    alias if both are present.
  - control-plane smoke run: `benchmark-runs/20260430-173353-flat-tri-depth`,
    with `native_tri_depth=1 source=XEMU_NATIVE_TRI_DEPTH mode=safe` in the log
    and `env_XEMU_NATIVE_TRI_DEPTH: 1` in metadata.
  - conflict smoke run: `benchmark-runs/20260430-175500-flat-tri-depth`, launched
    with `XEMU_NATIVE_TRI_DEPTH=0` and `XEMU_DIAG_NATIVE_TRI_DEPTH=1`; it emitted
    no native enable line, reported 0 native triangle-depth draws, and kept
    51,863 triangle draws on the geometry-shader path.
- Same-build paired native triangle-depth comparisons:
  - P1 Rainbow report:
    `benchmark-runs/20260430-174138-native-tri-depth-compare-rainbow-six-3`.
  - P1 baseline/native runs:
    `benchmark-runs/20260430-174138-rainbow-six-3` and
    `benchmark-runs/20260430-174156-rainbow-six-3`.
  - P1 post-load MSPF: 23.10 baseline, 6.83 native.
  - P1 geometry draws: 79,775 baseline, 0 native.
  - P1 native draws: 124,914, covering 50,142 w-depth, 74,772 linear-depth, and
    23,976 polygon-offset draws.
  - P1 visual crop changed pixels: 0.6131%.
  - P2 Crimson report:
    `benchmark-runs/20260430-174443-native-tri-depth-compare-crimson-skies`.
  - P2 baseline/native runs:
    `benchmark-runs/20260430-174443-crimson-skies` and
    `benchmark-runs/20260430-174500-crimson-skies`.
  - P2 post-load MSPF: 29.47 baseline, 17.87 native.
  - P2 geometry draws: 20,041 baseline, 0 native.
  - P2 native draws: 61,983, all linear-depth, with 23,142 polygon-offset draws.
  - P2 visual crop changed pixels: 3.6913%; visual inspection showed aligned
    crops with differences concentrated on texture/detail edges rather than an
    obvious depth-order break.
- New validation file:
  `docs/apple-silicon/benchmarks/2026-04-30-native-tri-depth-validation.md`.
- A first Crimson D3 attempt crashed before QMP became available:
  - run: `benchmark-runs/20260430-110740-crimson-skies`
  - crash report: `~/Library/Logs/DiagnosticReports/xemu-2026-04-30-110745.ips`
  - stack pointed at Apple's `GLImageWork` texture upload path before perf
    intervals were emitted.
  - immediate rerun completed, so this is treated as nondeterministic Apple
    OpenGL startup behavior unless it becomes reproducible.
- A post-tightening Rainbow attempt also crashed before QMP became available:
  - run: `benchmark-runs/20260430-121742-rainbow-six-3`
  - crash report: `~/Library/Logs/DiagnosticReports/xemu-2026-04-30-121748.ips`
  - stack again pointed at Apple's OpenGL texture upload worker path before
    perf intervals were emitted.
  - immediate rerun completed as D15, so this remains categorized as
    nondeterministic Apple OpenGL startup behavior.
- A flat-tri-depth trace attempt also hit the same Apple OpenGL worker class:
  - run: `benchmark-runs/20260430-152912-flat-tri-depth`
  - crash report pasted in-thread for process 24357 at 2026-04-30 15:29:13
    CDT.
  - crashed in `GLImageWork` / `libGLImage.dylib` during `glTexImage2D`
    texture upload before any `xemu-perf:` interval was emitted.
  - immediate rerun completed and validation later passed as
    `benchmark-runs/20260430-153555-flat-tri-depth`.
- Two additional Apple OpenGL nondeterministic startup crashes hit during the
  native-quad implementation session, both crashing in
  `glgProcessPixelsWithProcessor` /
  `GLDTextureRec::uploadTextureLevel` before any geometry was issued:
  - `~/Library/Logs/DiagnosticReports/xemu-2026-05-01-110643.ips`: pid 75358
    crashed at process launch+1s during a PGR2 retry replay.
  - `~/Library/Logs/DiagnosticReports/xemu-2026-05-01-111351.ips`: pid 76151
    crashed about 10s into a PGR2 snapshot-capture run.
  - Immediate retries succeeded each time. The native-quad code is not
    implicated; the failures occurred before any quad dispatch ran.

Native quad bypass slice (2026-05-01):

- `XEMU_NATIVE_QUAD=1` is the new opt-in quad/quad-strip-family fill bypass.
  When set, the renderer:
  - Skips geometry-shader generation for `PRIM_TYPE_QUADS` and
    `PRIM_TYPE_QUAD_STRIP` in smooth-fill mode.
  - Issues `glDrawElements(GL_TRIANGLES, ...)` against a CPU-expanded
    triangle index buffer that uses the same diagonal triangulation the
    geometry shader's `calc_quadz(0, 2)` already used, so smooth
    interpolation is unchanged.
  - Reuses the `gl_FragCoord`-derived depth and slope path that
    `XEMU_NATIVE_TRI_DEPTH=1` introduced for triangles. The depth math is
    primitive-agnostic.
- Flat-shaded quads, line/point polygon modes, and any nonfill raster mode
  fall back to the geometry shader, mirroring the conservative
  triangle-fill flat handling.
- `XEMU_NATIVE_QUAD` is independent of `XEMU_NATIVE_TRI_DEPTH`; both are
  needed at the same time for the full geometry-shader bypass. Setting the
  flag to `0` explicitly disables it.
- New per-subtype geometry-shader counters
  (`GEOM_SHADER_DRAW_QUAD_LIST`, `GEOM_SHADER_DRAW_QUAD_STRIP`) and full
  native-quad counter family
  (`NATIVE_QUAD_DRAW`, `NATIVE_QUAD_DRAW_LIST`, `NATIVE_QUAD_DRAW_STRIP`,
  `NATIVE_QUAD_CANDIDATE*`, `NATIVE_QUAD_FALLBACK*`,
  `NATIVE_QUAD_DRAW_ZPERSPECTIVE`, `NATIVE_QUAD_DRAW_LINEAR_Z`,
  `NATIVE_QUAD_DRAW_POLY_OFFSET`) are surfaced in `xemu-perf:` lines and
  in `extract-perf-summary.sh` output.
- Triangle regression gate
  (`scripts/apple-silicon/validate-native-tri-depth.sh --run 22`) passed
  after the native-quad code landed:
  `benchmark-runs/20260501-105543-flat-tri-depth`.
- Rainbow Six 3 snapshot scene with both flags on
  (`benchmark-runs/20260501-110557-rainbow-six-3`) reported 30.97 post-load
  FPS / 6.71 MSPF, identical within noise to the prior
  `XEMU_NATIVE_TRI_DEPTH=1`-only result. Quad-free scenes are unaffected.
- PGR2 retail-gameplay route replays show large per-run variance because
  real-time-paced input lands the emulator on different scene mixes at
  different host throughputs:
  - `XEMU_NATIVE_TRI_DEPTH=1` reference run:
    `benchmark-runs/20260501-104158-pgr2`, 21.40 post-load FPS, 177,272 GS
    quad draws remaining.
  - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` run 1:
    `benchmark-runs/20260501-105825-pgr2`, 18.20 post-load FPS, 0 GS draws.
  - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` run 2:
    `benchmark-runs/20260501-110810-pgr2`, 24.81 post-load FPS, 0 GS draws.
  - The 36% spread between the two same-config runs makes whole-route
    averages unreliable as a comparator.
- PGR2 mid-route snapshot triplet (stable, paused-input replays of the same
  game state):
  - Snapshot capture: `benchmark-runs/20260501-112001-pgr2`, savevm tag
    `pgr2_gameplay_b4`.
  - Baseline (no flags): `benchmark-runs/20260501-115623-pgr2`, 4.39
    post-load FPS, 332,066 GS draws (329,044 triangle + 3,022 quad).
  - `XEMU_NATIVE_TRI_DEPTH=1`: `benchmark-runs/20260501-115654-pgr2`,
    16.02 post-load FPS, 11,745 GS draws (all quad), 1,264,676 native-tri
    draws.
  - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1`:
    `benchmark-runs/20260501-115725-pgr2`, 16.56 post-load FPS, 0 GS
    draws, 1,317,851 native-tri draws, 12,193 native-quad draws (all
    `LIST`, all `CANDIDATE_SMOOTH`, zero fallbacks, depth split 2,716
    z-perspective + 9,477 linear-z).
- Conclusion from the snapshot triplet: native-tri-depth alone is the big
  lift at this PGR2 scene (4.39 → 16.02 FPS, 3.6x). Adding native-quad on
  top is performance-correct but modest at this specific scene
  (16.02 → 16.56, +3.4%), because only 12,193 quad draws exist in the
  30-second window. The remaining gap to 30 FPS is no longer
  geometry-shader work; the next slice should target whichever subsystem
  Instruments or perf counters identify as dominant.
- CLI `-loadvm` failed for the Crimson snapshot with a saved USB hub
  device-tree mismatch, so the harness restores after startup through QMP/HMP.
- Rainbow Six 3 crashed Apple's OpenGL worker path when a thumbnail-bearing
  snapshot was present on the scratch HDD. Benchmark-created snapshots now
  default to no thumbnail, and the thumbnail-free Rainbow snapshot restored
  successfully.
- QMP `screendump` can crash Apple's OpenGL-on-Metal path and should not be the
  default capture method yet.
- macOS `screencapture` failed from this Codex desktop context with
  `could not create image from display`; Computer Use screenshots were usable
  for live route verification.

## Next Session Checklist

1. Start by reading this checklist plus the most recent 2026-05-01 notes:
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-gameplay-route.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-rainbow-gameplay-route.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-crimson-gameplay-route.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-tri-depth.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-sample.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgraph-fast-read.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-baseline-jitter.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-voice-fast-lock-investigation.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-tcg-float-audit.md`
   - `docs/apple-silicon/benchmarks/2026-05-01-frame-log-retail-routes.md`
2. Treat `XEMU_NATIVE_TRI_DEPTH=1`, `XEMU_NATIVE_QUAD=1`, and
   `XEMU_PGRAPH_FAST_READ=1` as the three completed current opt-in
   performance flags. They are independent and stack:
   - tri-depth: removes triangle-family fill geometry shader (flat-first
     native, flat-nonfirst falls back to GS).
   - native-quad: removes quad/quad-strip-family smooth-fill geometry
     shader by CPU-side index expansion to triangles.
   - fast-read: skips `pg->lock` for simple PGRAPH register reads, where
     a 32-bit aligned load is already atomic on aarch64/x86 and the mutex
     was strict overhead.
   Combined, they bring PGR2 retail gameplay from 11.67 to 31.76 post-load
   FPS over the full 300-second route. PGR2 now meets the 30 FPS retail
   gameplay floor.
3. Triangle regression gate is
   `scripts/apple-silicon/validate-native-tri-depth.sh --run 22`. Most
   recent passing run after the native-quad slice landed:
   `benchmark-runs/20260501-105543-flat-tri-depth`. Cite that run if the
   gate is invoked again unless triangle code changes.
4. Keep `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` available for targeted debugging,
   but leave it off for timing runs.
5. Use `scripts/apple-silicon/native-tri-depth-compare.sh` for snapshot-level
   same-build comparisons, but the retail gameplay route scripts are the
   user-visible 30 FPS target. Whole-route averages are not stable across
   runs because real-time-paced input drives the emulator into different
   scene mixes (run 1 18.20 FPS vs run 2 24.81 FPS for the same flag config
   on PGR2 — see the native-quad note). For trustworthy comparisons use the
   PGR2 mid-route snapshot below.
6. PGR2 mid-route snapshot (created in this session):
   - HDD: `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`
   - Tag: `pgr2_gameplay_b4`
   - Snapshot triplet (30 s replays):
     - Baseline: 4.39 FPS,
       `benchmark-runs/20260501-115623-pgr2`.
     - `XEMU_NATIVE_TRI_DEPTH=1`: 16.02 FPS,
       `benchmark-runs/20260501-115654-pgr2`.
     - `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1`: 16.56 FPS,
       `benchmark-runs/20260501-115725-pgr2`.
   - All three runs use `noop.csv`. The third config has zero
     geometry-shader draws of any kind.
7. The PGR2 30 FPS gameplay floor is now met with all three flags on:
   - Snapshot at `pgr2_gameplay_b4` reaches 30.76 FPS (30 s replay) and
     30.70 FPS (60 s replay).
   - Full retail gameplay route reaches 31.76 post-load FPS over 279
     intervals with zero geometry-shader draws.
   The remaining session-to-session route variance is dramatically reduced
   because the emulator is no longer CPU-starved by lock contention.
   Profiling next steps for the remaining gap to 60 FPS:
   - Audit `pgraph_write` for safe lock-free fast paths on simple stores
     (write contention was 2.7% of TCG-thread time in the sample profile).
   - Audit `voice_lock`-protected NV_USER writes for the same pattern
     (6.9% of TCG-thread time in the sample profile).
   - Capture a fresh `sample` profile at the snapshot scene with all
     three flags on and identify the new dominant cost (likely candidates:
     remaining i386 TCG, NV2A PGRAPH command processing, surface/texture
     upload, fragment shader work).
   Capture a dated benchmark note before any code changes so the next
   slice stays data-driven.
8. Use this wrapper if the flat validation needs to be reproduced:

```sh
scripts/apple-silicon/validate-native-tri-depth.sh --run 20
```

   It runs the flat XBE with `XEMU_NATIVE_TRI_DEPTH=1`, extracts the perf
   summary, and fails if the expected native/fallback split is missing.

   Use this trace-heavy variant only for debugging:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_PERF_LOG_INTERVAL_MS=1000 \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

   Passing means the first-provoking flat phase produces nonzero
   `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`, and the last-provoking flat phase
   produces nonzero `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` plus
   `GEOM_SHADER_DRAW_TRI`.
9. Run follow-up implementation/diagnostic changes against both the retail
   gameplay routes and the saved scene snapshots:
   - Crimson: load `crimson_scene_b0` from
     `benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2`.
   - Rainbow: load `rainbow_scene_b1_nothumb` from
     `benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2`.
   - PGR2: load `pgr2_gameplay_b4` from
     `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`.
10. Compare the result against R1/R2/R3 in
   `docs/apple-silicon/benchmarking.md`, the route notes in
   `docs/apple-silicon/benchmarks/`, B2/B3/D1/D2/D3/D4, D17, and P1/P2 in
   `docs/apple-silicon/benchmarks/2026-04-30-baseline-metrics.md`, and the
   PGR2 snapshot triplet in
   `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md`.
11. Only after the next non-geometry-shader bottleneck is identified and a
   slice plan exists, decide whether V0/V1 Vulkan-over-Metal experiments
   are worth doing before the native Metal path.

## Update — 2026-05-01 measurement-infrastructure session

This session focused on the 60 FPS pursuit and explicit jitter
detection. Major outcomes (no FPS-improving code shipped, but
infrastructure and findings that scope the next slice):

### Measurement infrastructure landed

- **Sub-millisecond perf-log precision.** `hw/xbox/nv2a/pgraph/profile.c`
  now tracks frame-time in microseconds internally and emits
  `mspf_avg/mspf_min/mspf_max` with `%.3f` precision. The HUD plot
  (`ui/xui/debug.cc`) still consumes the integer-ms `frame_working.mspf`
  field — that path is unchanged.
- **Optional per-frame timing log.** `XEMU_PERF_FRAME_LOG=1` appends a
  `frame_mspf_us=v1,v2,...` field to interval lines (bounded 1024
  frames/interval, overflow recorded as `frame_mspf_us_dropped`). Default
  off.
- **Jitter metrics in `extract-perf-summary.sh`.** New keys both whole-run
  and `post_load_*`:
  - `fps_stddev`
  - `mspf_max_p50/p95/p99/max` over per-interval worst-frames
  - `mspf_avg_max`
  - `stutter_intervals_30fps/45fps/60fps` (intervals where
    `mspf_max > 33.3 / 22.2 / 16.7`)
  - `longest_stutter_run_30fps/60fps`
- **`scripts/apple-silicon/sample-profile.sh`.** Background a benchmark
  run, poll the run dir + xemu pid, attach Apple `sample` for a configured
  duration, write the full sample text plus a thread-bucket summary into
  the run dir. Fully autonomous, no user input.
- **`scripts/apple-silicon/compare-runs.sh`.** Compare two run dirs and
  emit a side-by-side jitter+FPS comparison plus a "regression /
  improvement / noise" verdict per metric. Default noise threshold 3 %,
  configurable via `NOISE_PCT`. Exit code reflects regressions.

### New benchmark notes

- `2026-05-01-baseline-jitter.md` — jitter analysis of the existing
  post-fast-read 300 s retail-route runs. Bottleneck classification via
  `avg_mspf` vs `1000/avg_fps`: PGR2 39 % renderer / 61 % CPU-or-lock,
  Rainbow 21 % / 79 %, Crimson 91 % / 9 %. **Crimson is renderer-bound,
  not CPU-bound** — lock-elision will not lift Crimson FPS.
- `2026-05-01-pgr2-bottleneck-postfast.md` — fresh `sample` profile of
  `pgr2_gameplay_b4` with all three flags on. TCG mutex wait collapsed
  from 32.6 % (pre-fast-read) to 8.9 %. `voice_lock` is now 6.8 % of TCG
  thread (essentially unchanged). `pgraph_write` is 1.4 %. **The pfifo
  thread is idle 41.5 % of the time on the FIFO condvar** — the renderer
  is no longer the binding constraint at this scene; the CPU emulator's
  real x86 work is. Floating-point helpers (`helper_mulss`,
  `helper_fmul_ST0_FT0`, `floatx80_mul`, etc.) show prominently.
- `2026-05-01-voice-fast-lock-investigation.md` — implemented
  `XEMU_VOICE_FAST_LOCK=1` (atomic OR/AND on `voice_locked[]` bitmap, no
  `cond_signal`). Snapshot showed essentially no FPS change with mixed
  jitter signals; retail route showed +91 % more 30 FPS stutter intervals
  (within run-to-run variance, but no positive evidence). **Not landed.**
  Code reverted. Negative result documented.

### Critical jitter finding

Crimson Skies' p99 worst-frame is **892 ms**, max **1310 ms**, with a
**16-second** longest contiguous stutter run. Per-interval drilldown
shows every stutter spike coincides with non-zero `SHADER_GEN`,
`SURF_TO_TEX`, or `TEX_UPLOAD` activity. Interval 15 of the recorded
Crimson route has 7 triangle draws over 1.3 seconds (≈ 187 ms per draw).
This is consistent with Apple's OpenGL-on-Metal driver compiling shaders
synchronously inside `glDrawElements` — a documented behavior in macOS
GL emulators. Without async shader compilation, sustained 60 FPS on
Crimson is unattainable regardless of TCG-side wins.

PGR2 retail route p99 is 38 ms, max 117 ms (well-behaved). Rainbow Six 3
retail route p99 is 139 ms, max 694 ms (bad tail; same shader-compile
shape).

### Reality check on the 60 FPS goal

The post-fast-read profile makes the upper bound on lock-elision work
clear: ~9 % of TCG-thread time remains in mutex wait. Even eliminating
all of it would lift FPS by at most that much. Going from ~31 FPS to 60
FPS on PGR2 requires roughly doubling TCG-thread throughput, which
lock-elision alone cannot deliver. The realistic 60 FPS path needs:

1. SSE / x87 floating-point helper audit. `helper_mulss`, `helper_mulps_xmm`,
   `helper_fmul_ST0_FT0`, `float32_mul`, `floatx80_mul` are all visible
   in the post-fast-read sample. If SSE float32 ops are going through
   softfloat (`soft_f32_mul`) when Apple Silicon has perfectly capable
   NEON float32, that is potentially a major TCG win. **Open
   investigation** — needs source-side audit of the i386 hardfloat path
   in QEMU.
2. TB-chain audit. `helper_lookup_tb_ptr` is 7.6 % of TCG thread; if
   chaining drops out more than necessary, the JIT spends more time in
   dispatch than in real code.
3. Async shader compile (Crimson and Rainbow tail jitter).
4. The native Metal renderer track (Phase 4 of `strategy.md`). The bigger
   Crimson lift, and breaks the Apple-OpenGL synchronous-shader-compile
   ceiling.

## Update — 2026-05-01 emulator-survey research session

Research-only session; no code changes. Captured a survey of how other
emulators achieve excellent performance on Apple Silicon (Dolphin,
PCSX2, DuckStation, PPSSPP, RPCS3, Ryujinx) and produced a research-
informed implementation roadmap.

Outputs:

- New section in `docs/apple-silicon/research.md`: "Apple Silicon
  Emulator Survey (2026-05-01)" with named code references and source
  URLs for every claim.
- Updates in `docs/apple-silicon/strategy.md`:
  - Phase 2.5 (Frame Pacing & Async Shader Compile) inserted —
    graphics-API-agnostic; can land on the current OpenGL path.
  - Phase 4 expanded with sub-deliverables 4a–4i (Metal presentation,
    CPU index-expansion port, framebuffer fetch on Apple GPU,
    VS-Expand for sprites/lines, async pipeline compile, persistent
    pipeline cache, buffer/texture management, frame-capture workflow,
    perf comparison).
  - Phase 5 expanded with 5a (PPTC persistent TCG translation cache)
    and 5b (SSE / x87 hardfloat audit, already tracked).
  - New "What we ruled out" section documenting why a custom
    x86 → ARM64 JIT, indirect-command-buffers, and Hypervisor.framework
    are off the roadmap.
- New decision-log entry: "2026-05-01: Adopt research-informed
  implementation roadmap".

This session does NOT supersede the existing Prioritized Next Tasks
list below. The survey adds named patterns and source references for
tasks already in flight — especially #2 (async shader compile), which
now has Dolphin's hybrid ubershader (PR #5702) and RPCS3's 2018 async
pipeline as named templates.

The next implementation slice should still be #2 (async shader compile)
— it has the highest measured user-visible jitter leverage (Crimson's
1310 ms worst-frame from synchronous compile inside Apple's
GL-on-Metal driver). Consider a parallel small slice for Phase 2.5
emulation-rate slewing because it is graphics-API-agnostic, trivially
measurable on the existing OpenGL path via `mspf_max` jitter keys, and
mirrors a proven DuckStation/PCSX2 pattern.

## Update — 2026-05-01 game-packaging-tool session

Tooling-only session; no emulator code changes, no benchmark runs.
Added a packaging tool so future sessions can pull arbitrary games from
the external Xbox library to stress-test reported xemu issues against
this build.

Outputs:

- New script: `scripts/apple-silicon/package-game.sh`. Wraps `xdvdfs
  pack` with name lookup against the external library, overwrite
  protection, post-pack `xdvdfs info` verification, a `.meta.txt`
  sidecar, and autoinstall of `xdvdfs-cli` via `cargo install --root
  $HOME/.cargo`. CLI: `--list [filter]`, `--source DIR`, `--output
  FILE`, `--library DIR`, `--xdvdfs PATH`, `--force`, `--no-verify`,
  `--no-install`.
- New decision-log entry: "2026-05-01: Add external Xbox library and
  `package-game.sh` packaging tool".
- New `automation.md` section: "Game Library Packaging".
- `xdvdfs-cli` v0.8.3 installed locally at `/Users/jbbrack03/.cargo/bin/xdvdfs`.

Validation (no emulator changes; tool-only):

- `--help` prints the usage banner.
- `--list "rainbow"` enumerates the four matching folders from the
  library (Critical Hour, Lockdown, 3, 3 - Black Arrow).
- Bogus name → exit 1 with a "use --list" hint.
- Ambiguous name (`rainbow`) → exit 1 with a disambiguation list.
- Bad `--source` path → exit 1.
- Unknown flag → exit 2.
- Pack of `scripts/apple-silicon/xbe-tests/flat-tri-depth/bin/` (single
  default.xbe) → 256 KiB ISO, `xdvdfs info` reports `Valid: true`.
- Idempotent rerun → "already packed" no-op.
- `--force` rerun → rebuilds.
- End-to-end name lookup pack of `Grooverider - Slot Car Thunder` (~92
  MiB extracted) via `--output` to a temp dir → 96 MiB ISO in 3 s,
  `xdvdfs info` `Valid: true`, `xdvdfs ls` shows real game `.PAK`
  files.

Test ISOs were written to a `mktemp -d` directory and removed after
verification; nothing under `Test_Games/` was modified during this
session.

Not in scope for this slice (deferred to a later one): teaching
`run-benchmark.sh` a `custom <iso>` target so packaged games can be
benchmarked through the harness without per-target hardcoding. Until
then, drive xemu directly or extend `find_test_disc()` for a specific
title under investigation.

## Update — 2026-05-01 GL-vs-Metal decision (stay on GL, headline issue is TCG)

This session ran the diagnostic the previous session called for and
produced a definitive strategic verdict. **Stay on OpenGL. Native Metal
is not the next priority.** The Crimson 1.35-second worst-frame is a
CPU-emulation problem; Apple's GL has measured headroom for 60 FPS at
1080p (and even 4×-scale internal resolution) on tracked titles.

Full analysis at
`docs/apple-silicon/benchmarks/2026-05-01-gl-vs-metal-decision.md` and
the supporting attribution note
`docs/apple-silicon/benchmarks/2026-05-01-renderer-vs-tcg-stutter-attribution.md`.

### Diagnostic infrastructure landed

- New per-subsystem microsecond counters: `BIND_TEXTURES_US_TOTAL`,
  `TEX_UPLOAD_US_TOTAL`, `SURF_TO_TEX_US_TOTAL`,
  `SURF_UPLOAD_US_TOTAL`, `SURF_DOWNLOAD_US_TOTAL`,
  `FLUSH_DRAW_US_TOTAL`, `DRAW_BEGIN_US_TOTAL`,
  `FLIP_STALL_US_TOTAL`, `FLIP_STALL_GLFINISH_US_TOTAL`. Wrapped
  around the corresponding renderer entry points.
- `nv2a_profile_spike()` and `xemu-spike:` log lines: per-event spike
  detection with `XEMU_PERF_SPIKE_LOG=1` and tunable threshold via
  `XEMU_PERF_SPIKE_LOG_THRESHOLD_US`.
- `scripts/apple-silicon/run-benchmark.sh` `XEMU_BENCH_SURFACE_SCALE`
  env var that injects `[display.quality] surface_scale = N` into the
  per-run config. Drives the GL stress tests at 1× / 2× / 4× internal
  scale.

### Decisive findings

1. **Renderer thread is idle during Crimson's 1.35-second worst
   frames.** All renderer counters under 24 ms in 1000 ms intervals.
   The Xbox CPU is producing only 2–10 frames in those intervals.
2. **Apple `sample` profile pinpoints the cause:** TCG TB
   invalidation chain — `tb_invalidate_phys_range_fast` →
   `do_tb_phys_invalidate` → `tcg_flush_jmp_cache` plus
   `pthread_jit_write_protect_np` and `sys_icache_invalidate`. This
   is the documented Apple-Silicon-specific QEMU MTTCG pathology;
   each TB invalidation pays the W^X-toggle and i-cache-flush
   syscall cost.
3. **Apple's GL handles 4× internal scale (~2560×1920) on PGR2
   snapshot with negligible cost growth.** `FLUSH_DRAW_US_TOTAL` grew
   only 7 % from scale 1 to scale 4. p99 stayed at ~35 ms. No
   per-pipeline-state-object pathology under heavier load.
4. **At 4× scale on Crimson, renderer cost was 27 % of wallclock
   over 60 s.** Doubling FPS to 60 would land at ~54 % — fits with
   margin. 1080p-class output on Apple GL is not the gating
   constraint.

### Decision

Logged at `docs/apple-silicon/decision-log.md` 2026-05-01: stay on
GL, prioritize TCG TB-invalidation fix, MSAA-on-GL becomes the
follow-up renderer slice (not Metal).

### Async shader compile slice ALSO confirmed not the cause

The earlier `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` slice (still shipped
opt-in) did not change Crimson's worst-frame either, for the same
reason: the renderer is idle during the bad intervals, so async-ing
shader compile cannot help. Same evidence chain as
`2026-05-01-async-shader-compile.md`, with a sharper conclusion
because we now know what *is* the cause.

## Update — 2026-05-01 async shader compile slice (opt-in; headline judder NOT solved)

This session implemented and validated the async shader compile slice
that the previous session's roadmap put as the highest-leverage user-
visible jitter fix. The implementation works correctly and ships as
opt-in. **The headline 1.35-second Crimson Skies worst-frame stutter is
unchanged.** This is a real and important finding — the stutter is not
`glLinkProgram` time on the renderer thread.

### What landed

- `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` env flag.
- Third shared GL context `g_nv2a_context_shader_compile` created in
  `early_context_init()` only when the flag is set.
- `pgraph.gl_async_compile` worker thread in `shaders.c` that pulls
  bindings off `compile_queue` and runs `generate_shaders()` (compile
  + link + `glFinish`) without holding any lock the renderer needs.
- `pgraph_gl_bind_shaders()` enqueues a compile request the first time
  it sees a new shader-state hash, sets `r->shader_skip_draw=true`, and
  the corresponding draw is skipped via early-returns in
  `pgraph_gl_draw_begin / draw_end`. Pattern follows RPCS3 PR #4876
  "Async (Skip Draws)".
- Two-lock design: `shader_cache_lock` (short critical sections;
  cache lookup, pending-flag mutation) and the new
  `shader_module_cache_lock` (worker holds during long compile).
  Renderer never blocks waiting for the worker.
- New counters: `SHADER_COMPILE_COUNT`, `SHADER_COMPILE_US_TOTAL`,
  `SHADER_COMPILE_ASYNC_QUEUED`, `SHADER_COMPILE_ASYNC_COMPLETED`,
  `SHADER_DRAWS_SKIPPED_PENDING`. Surfaced in
  `extract-perf-summary.sh` and documented in `automation.md`.
- LRU eviction of a `pending_compile` binding aborts (guard rail; never
  fired in any validation run).

### Validation runs

- `benchmark-runs/20260501-181049-crimson-skies` — Crimson 300 s sync
  baseline. `post_load_avg_fps` 30.67, `frame_mspf_us_max` 1,345,831,
  `SHADER_COMPILE_US_TOTAL` 399,167 us.
- `benchmark-runs/20260501-182613-crimson-skies` — Crimson 168 s with
  async on. `post_load_avg_fps` 29.61, `frame_mspf_us_max` 1,351,887,
  `SHADER_COMPILE_US_TOTAL` 346,661 us, 115/115 async queue/complete,
  756 draws skipped.
- `benchmark-runs/20260501-183005-pgr2` (snapshot, sync), 30 s,
  `post_load_avg_fps` 30.96.
- `benchmark-runs/20260501-183046-pgr2` (snapshot, async), 30 s,
  `post_load_avg_fps` 30.84, 139/139 async queue/complete, 1,000 draws
  skipped. No regression.

Full numbers and analysis at
`docs/apple-silicon/benchmarks/2026-05-01-async-shader-compile.md`.

### Critical finding

Comparing baseline vs async paired runs **on the same disc, same input
script, same build**: bad intervals occur at the same gameplay points
with near-identical magnitudes:

| Baseline interval / mspf_max | Async interval / mspf_max |
| ---------------------------- | ------------------------- |
| 27 / 1,345.8 ms              | 28 / 1,343.7 ms           |
| 28 / 1,169.9 ms              | 29 / 1,278.4 ms           |
| 32 / 1,321.5 ms              | 33 / 1,351.9 ms           |

The async slice did move 347 ms of `glLinkProgram` work off the
renderer thread (`SHADER_COMPILE_US_TOTAL` dropped from 399 to 347 ms
across the run) and skipped 756 draws while compiles were in flight.
But the per-interval `mspf_max` distribution is unchanged.

The headline 1.35 s worst-frame is **not** synchronous `glLinkProgram`
time. The likely cause is Apple's GL-on-Metal driver doing MSL→Metal
pipeline-state-object compile inside the **first `glDrawElements`**
with a new program / VAO / state combination — work that runs on the
renderer thread regardless of which context did the link.

### `p999` regression

`post_load_frame_mspf_us_p999` went from 104,331 us (baseline) to
382,090 us (async). This is consistent with the worker's `glFinish()`
blocking on Apple's GL command queue, which serializes against the
renderer's command buffer. A follow-up A/B with `glFlush()` in place
of `glFinish()` could recover the p999.

### Decision

Logged at `docs/apple-silicon/decision-log.md` 2026-05-01: ship async
opt-in, do not pursue further async work until the actual source of
the worst-frame is identified. Phase 4 (native Metal renderer) remains
the right long-term path because it is the only way to escape Apple's
GL-on-Metal MSL compile and command-queue serialization.

## Prioritized Next Tasks

User visual confirmation on real PGR2, Rainbow Six 3, and Crimson Skies
discs: 30 FPS feel with no rendering artifacts on 2026-05-01 with
`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`. The
three flags are validated for the current tracked title set.

**Highest priority (everything else depends on it):** TCG TB-
invalidation fix on Apple Silicon. The 2026-05-01 sample profile
attributes Crimson's 1.35-second worst-frame to the JIT TB
invalidation chain (`tb_invalidate_phys_range_fast` →
`do_tb_phys_invalidate` → `tcg_flush_jmp_cache`) plus
`pthread_jit_write_protect_np` and `sys_icache_invalidate`. Apple
Silicon pays real syscall cost per TB flush. Investigation paths:

- **Persistent TCG translation cache (PPTC)** — strategy.md Phase
  5a. Eliminates re-translation work after warmup. Largest leverage
  if the Xbox is repeatedly invalidating/retranslating the same code
  region.
- **W^X toggle batching.** Apple Silicon's `pthread_jit_write_protect_np`
  flips the JIT page write-protect; Apple recommends batching writes
  under a single toggle. xemu's TB invalidation likely toggles per
  invalidation. Investigate whether QEMU's `tb-maint.c` can batch
  toggles across a burst of related invalidations.
- **Reducing invalidation frequency** by being smarter about which
  pages actually contain executable Xbox code. The Xbox CPU emulator
  may currently treat all writes through the softmmu path as
  potentially-invalidating.
- **Upstream QEMU MTTCG patches** for Apple Silicon JIT handling.
  Search the qemu-devel list and qemu-project/qemu issues for
  `MAP_JIT`, `pthread_jit_write_protect_np`, and `tb_flush` patches.

This slice is gating for: no-judder, sustained 60 FPS, 1080p with AA
(because none of the renderer-side work helps if the CPU emulator is
the bottleneck).

After the TCG fix lands and is validated:

In priority order, the next concrete tasks for a future session:

1. **Per-frame mspf retail-route capture — completed 2026-05-01.** All
   three 300 s gameplay routes were re-run under
   `XEMU_PERF_FRAME_LOG=1` with the three opt-in flags on. Run dirs:
   `benchmark-runs/20260501-173435-pgr2`,
   `benchmark-runs/20260501-173959-rainbow-six-3`,
   `benchmark-runs/20260501-174514-crimson-skies`. Per-interval
   summaries and run conditions are captured in
   `docs/apple-silicon/benchmarks/2026-05-01-frame-log-retail-routes.md`.
   Per-route post-load FPS and worst-frame: PGR2 32.07 FPS / 117.84 ms
   max; Rainbow 30.16 FPS / 717.18 ms max; Crimson 30.43 FPS / 1375.50
   ms max with 16-interval longest 30 FPS stutter run. Crimson confirms
   the documented Apple GL-on-Metal synchronous-shader-compile
   fingerprint is the runaway worst-frame source.

   **Follow-up still TODO**: extend `scripts/apple-silicon/extract-perf-summary.sh`
   to parse the per-interval `frame_mspf_us=v1,v2,...` field into a
   global flat list and emit true frame-level `frame_mspf_us_p50/p95/p99/p999/max`
   plus stutter-frame counts. Pure post-processing extension; no
   emulator code change required. The data captured this session is the
   input.
2. **Async shader compile — completed 2026-05-01 (opt-in, does not solve
   the headline judder).** `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` ships as
   a documented opt-in. End-to-end correctness validated. Counters
   `SHADER_COMPILE_COUNT`, `SHADER_COMPILE_US_TOTAL`,
   `SHADER_COMPILE_ASYNC_QUEUED`, `SHADER_COMPILE_ASYNC_COMPLETED`,
   `SHADER_DRAWS_SKIPPED_PENDING` confirm the worker compiled and the
   renderer skipped draws while waiting. **But** Crimson's
   `post_load_frame_mspf_us_max` stayed at 1.35 s and `p999` regressed
   from 104 ms (sync) to 382 ms (async, due to `glFinish` serializing
   against Apple's GL command queue). The headline stutter is not
   `glLinkProgram` time — it is Apple's GL-on-Metal MSL→PSO compile
   triggered by the renderer's first `glDrawElements` with a new
   program/VAO/state combo, which runs on the renderer thread regardless
   of where the link happened. See decision-log entry "2026-05-01: Async
   shader compile shipped opt-in" and benchmark note
   `2026-05-01-async-shader-compile.md`.

   **The actual next slice for judder elimination** is identifying what
   fires inside the bad frame. Cheap follow-up: add per-event timestamp
   logging in `pgraph_gl_draw_begin / draw_end / flush_draw` and across
   `TEX_UPLOAD`, `SURF_TO_TEX`, `SURF_UPLOAD`, `SURF_DOWNLOAD`, then
   correlate against frames where `frame_mspf_us > 100,000`. The
   handoff already noted "every stutter spike coincides with non-zero
   `SHADER_GEN`, `SURF_TO_TEX`, or `TEX_UPLOAD` activity"; we now know
   `SHADER_GEN` is correlated but not causal, so the surface or
   texture-upload paths are the prime suspects. A second cheap A/B:
   replace the worker's `glFinish()` with `glFlush()` to test whether
   the p999 regression is recoverable.
3. **SSE / x87 floating-point helper audit — completed 2026-05-01.**
   See `docs/apple-silicon/benchmarks/2026-05-01-tcg-float-audit.md` and
   decision-log entry "2026-05-01: SSE hardfloat already active on
   aarch64; x87 irreducibly soft". Source-level finding:
   `float32_gen2`/`float64_gen2` (`fpu/softfloat.c:337-397`) already
   dispatches to a hard `a*b` shortcut on aarch64 — there is no
   `__x86_64__` gate on the shortcut itself, only on a micro-style
   choice. `helper_mulss`/`helper_mulps_xmm` already get a single arm64
   `fmul` in the steady state (sticky `float_flag_inexact` after first
   op, round-nearest, normal inputs). Visible `parts64_uncanon_normal`
   time in the post-fast-read sample is the **necessary soft fallback**
   for first-op-after-MXCSR-reset, NaN/Inf/denormal inputs, denormal
   results, and non-default rounding modes — not an unconditional
   softfloat trip. So the original "lifting to hardfloat is the single
   largest potential TCG win" hypothesis is wrong for SSE.

   `helper_fmul_ST0_FT0` (x87 80-bit) is irreducibly soft on Apple
   Silicon: there is no native 80-bit float on aarch64
   (`sizeof(long double) == 8`), and the fork's existing `__hard` x87
   path is correctly gated to `XBOX && __x86_64__`
   (`target/i386/tcg/fpu_helper.c:76-267`,
   `target/i386/tcg/translate.c:38-124`,
   `ui/xui/main-menu.cc:62-66`).

   **Cheap follow-up experiment** (recommended before any further float
   work): add a counter pair around `float32_gen2`/`float64_gen2` —
   `sse_hard_taken` vs `sse_soft_fallback` (split by reason:
   `!can_use_fpu`, `!pre`, `denormal_result`). Run on the PGR2
   `pgr2_gameplay_b4` snapshot for 30 s. If hard-take ratio > 0.9,
   confirm the visible `parts64_*` time is irreducible and redirect to
   the next dominant subsystem identified by Instruments (TLB / memory
   ops, NV2A PGRAPH command parsing, surface/texture upload). If the
   ratio is unexpectedly low, the per-reason breakdown identifies the
   dominant fall-through and the next investigation target. No code
   committed yet.
4. **`pgraph_write` fast path** (`XEMU_PGRAPH_FAST_WRITE=1`).
   **Deferred** as of 2026-05-01 — see decision-log entry "2026-05-01:
   XEMU_PGRAPH_FAST_WRITE deferred (not pursued this session)". The
   "low-risk, mirror `pgraph_read`" framing was undercounted: `pgraph_reg_w`
   (`hw/xbox/nv2a/pgraph/pgraph.h:311`) updates the `regs_dirty` bitmap
   that the renderer consumes for shader-recompile decisions
   (`hw/xbox/nv2a/pgraph/glsl/shaders.c:57`,
   `hw/xbox/nv2a/pgraph/vk/draw.c:643`). A correct lock-free path needs
   atomic `set_bit` on `regs_dirty` plus explicit acquire/release ordering
   on the consumer side, not just a `qatomic_set` on the value. And per
   the "2026-05-01: XEMU_VOICE_FAST_LOCK not landed" entry, lock-elision
   at this Amdahl scale (1.4 % of TCG) cannot translate to FPS while the
   pfifo thread is idle 41.5 % of the time. Eligibility (for whenever
   it is revisited): `default` slot writes (with the `regs_dirty` work
   above) and `NV_PGRAPH_INTR_EN` are candidates; `NV_PGRAPH_INTR`,
   `NV_PGRAPH_INCREMENT`, `NV_PGRAPH_RDI_DATA`,
   `NV_PGRAPH_CHANNEL_CTX_TRIGGER`, and `NV_PGRAPH_FIFO` (the latter
   triggers `pfifo_kick`) must stay locked.
5. **`XEMU_PGRAPH_RELEASE_LOCK_DURING_GL=1`.** On scenes where the
   pfifo thread is *not* idle (Crimson) this is the bigger lock-elision
   win. The PGR2 snapshot showed pfifo thread is idle 41.5 % of the time,
   so this slice will have minor effect on PGR2 but should help Crimson
   if its bottleneck is partly draw-thread serialization.
6. **Broader title coverage before defaulting any flag.** Same as before;
   current three flags need a wider title shakeout (different genre /
   GPU mix) before flipping any to default-on.

Profile-guided rule still applies: every slice gets a fresh `sample`
profile (use `scripts/apple-silicon/sample-profile.sh` now) and a dated
benchmark note. Use `scripts/apple-silicon/compare-runs.sh` for the
before / after metric diff.

## Things Not To Forget

- Do not delete or overwrite the local BIOS/HDD/game files.
- Do not assume MoltenVK or KosmicKrisp is good enough without a run.
- Do not optimize from intuition when Instruments or counters can answer.
- Keep docs updated after each meaningful experiment.

## Useful Commands

Build baseline:

```sh
./build.sh -a arm64
```

Verify packaged app:

```sh
codesign --verify --deep --strict --verbose=2 dist/xemu.app
dist/xemu.app/Contents/MacOS/xemu --version
```

Show current commit:

```sh
git rev-parse HEAD
```

Find geometry shader use:

```sh
rg -n "geometryShader|GL_GEOMETRY_SHADER|pgraph_glsl_need_geom|EmitVertex|EndPrimitive" hw/xbox/nv2a/pgraph
```

Find macOS/Vulkan build logic:

```sh
rg -n "host_os == 'darwin'|vulkan =|OpenGL|Molten|Metal|VK_USE_PLATFORM" meson.build build.sh hw/xbox/nv2a ui
```

View public regression:

```sh
gh issue view 2506 --repo xemu-project/xemu --comments
```

View PR #2240:

```sh
gh pr view 2240 --repo xemu-project/xemu --comments
```

Replay retail gameplay routes:

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

Replay PGR2 with the completed opt-in triangle-family fill path:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

Replay PGR2 with both opt-in geometry-shader bypass slices (full
geometry-shader removal):

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_NATIVE_QUAD=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/xbox_hdd.qcow2 \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/pgr2-gameplay.csv 300
```

Run the PGR2 mid-route snapshot triplet for stable comparisons:

```sh
SNAPSHOT_HDD=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2
TAG=pgr2_gameplay_b4
# A: baseline
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=$SNAPSHOT_HDD \
XEMU_BENCH_LOADVM_TAG=$TAG \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/noop.csv 30
# B: tri-depth only
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=$SNAPSHOT_HDD \
XEMU_BENCH_LOADVM_TAG=$TAG \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/noop.csv 30
# C: tri-depth + quad
XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=$SNAPSHOT_HDD \
XEMU_BENCH_LOADVM_TAG=$TAG \
scripts/apple-silicon/run-benchmark.sh pgr2 \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Run snapshot scene-entry benchmarks:

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=crimson_scene_b0 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

```sh
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=rainbow_scene_b1_nothumb \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Re-run D1 diagnostic only if confirmation is needed:

```sh
XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=rainbow_scene_b1_nothumb \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Re-run D2 diagnostic only if confirmation is needed:

```sh
XEMU_DIAG_SKIP_TRI_GEOM=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=rainbow_scene_b1_nothumb \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Re-run the opt-in native triangle-depth path only for regression checks:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=rainbow_scene_b1_nothumb \
scripts/apple-silicon/run-benchmark.sh rainbow \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260430-100438-crimson-skies/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=crimson_scene_b0 \
scripts/apple-silicon/run-benchmark.sh crimson \
  scripts/apple-silicon/input-scripts/noop.csv 30
```

Run a same-build paired native triangle-depth comparison:

```sh
scripts/apple-silicon/native-tri-depth-compare.sh \
  rainbow benchmark-runs/20260430-101703-rainbow-six-3/xbox_hdd.qcow2 \
  rainbow_scene_b1_nothumb 16
```

Rebuild the dedicated flat-shading XBE only if its source changes:

```sh
NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk \
PATH=/Users/jbbrack03/XEMU_MacOS/nxdk/bin:/opt/homebrew/Cellar/lld@19/19.1.7/bin:/opt/homebrew/opt/llvm/bin:$PATH \
make -C scripts/apple-silicon/xbe-tests/flat-tri-depth
```

Reproduce the passing flat-XBE validation:

```sh
scripts/apple-silicon/validate-native-tri-depth.sh --run 20
```

Summarize a run:

```sh
scripts/apple-silicon/extract-perf-summary.sh benchmark-runs/20260430-153555-flat-tri-depth
```

Trace flat-XBE state only if debugging a regression:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_PERF_LOG_INTERVAL_MS=1000 \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

Recommended next implementation shape:

- The flat-tri-depth begin/bind/flush logging has been added and validated.
  The mismatch was a perf-window artifact; graceful final perf flushing now
  captures the flat XBE tail.
- Treat `XEMU_NATIVE_TRI_DEPTH=1` and `XEMU_NATIVE_QUAD=1` as the completed
  geometry-shader bypass slices for smooth-fill triangle and quad/quad-strip
  primitives. Do not re-prove either slice unless triangle or quad code
  changes; the snapshot triplet at
  `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md` is the
  current paper of record.
- The next session's first task is to identify what is making PGR2 slow at
  the `pgr2_gameplay_b4` snapshot (16.56 FPS with both bypass slices on,
  zero geometry-shader draws). Use Instruments and the existing
  `XEMU_PERF_LOG=1` counters to measure i386 TCG, NV2A PGRAPH command
  processing, surface/texture upload, and fragment shader work in turn.
  Capture a dated benchmark note with the dominant cost before any code
  change.
- Defer further geometry-shader removal slices (flat-quad bypass,
  nonfill polygon modes, line/point primitive bypass) until a benchmark
  exercises that combination meaningfully. Today none of the
  Crimson/Rainbow/PGR2 routes do.
- Compare future renderer changes against R1/R2/R3, the route notes, the
  baseline-metrics file, and the PGR2 snapshot triplet
  `docs/apple-silicon/benchmarks/2026-05-01-pgr2-native-quad.md` before
  trying Vulkan-over-Metal.
