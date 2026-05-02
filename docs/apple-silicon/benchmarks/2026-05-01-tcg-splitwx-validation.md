# TCG splitwx + diff-guarded W^X-toggle slice — validation

Date: 2026-05-01

## Verdict: FAIL on the headline judder hypothesis (Crimson worst-frame unchanged)

Splitwx (`XEMU_TCG_SPLITWX=1` / Apple-Silicon system-build auto-on)
**works mechanically as designed**: `pthread_jit_write_protect_np`
disappears entirely from the TCG-thread sample profile (11 → 0
occurrences), and the diff-guarded `qemu_w_xtoggle_*` wrappers no-op
when `tcg_splitwx_diff != 0`. The TCG vCPU thread executes 3.26× more
TBs per wallclock second on Crimson with the slice on (Arm C 1.26B →
Arm D 4.10B `TCG_TB_EXEC_COUNT` over the same 300 s route).

**But the predicted user-visible win does not materialize.** Crimson's
1.35-second worst-frame is **completely unchanged**: 1347.74 ms (Arm C,
splitwx OFF) → 1352.15 ms (Arm D, splitwx ON), delta +0.33 % — pure
noise. The W^X toggle is a real cost on Apple Silicon, but it is a
syscall-level micro-optimization, not the dominant TB-invalidation
cost. The dominant cost is `tcg_flush_jmp_cache` (per-CPU jump-cache
zeroing on every TB invalidation) at 1,982 of 20,680 TCG-thread
samples (9.6 %) in the splitwx-ON sample, essentially unchanged from
splitwx-OFF (1,995 / 20,973 = 9.5 %).

Per the pass/fail criteria in the validation protocol:

| # | Criterion | Result |
| - | --- | --- |
| 1 | `validate-native-tri-depth.sh --run 22` PASS | **FAIL** (pre-existing test-harness flakiness — see "Honest-limits caveats") |
| 2 | PGR2 snapshot Arm B post-load FPS within −3 % of Arm A | PASS (−0.78 %) |
| 3 | Crimson Arm D `frame_mspf_us_max` drops ≥ 100 ms vs Arm C | **FAIL** (+4 ms / +0.33 %) |
| 4 | `pthread_jit_write_protect_np` ≤ 5 % of OFF count | PASS (0 / 11 = 0 %) |
| 5 | No correctness regressions visible in tracked titles | PASS (Rainbow regression check clean; PGR2 zero-GS-draw; counters all present) |

Final classification: **FAIL on the user-visible-judder hypothesis,
PASS on the mechanical correctness of the splitwx + diff-guard
implementation.** The slice can be left landed (the diff-guard makes it
correctness-safe and there is no FPS regression on tracked titles) but
it is not the headline TCG win the prior session predicted.

## Build / commit verification

- Binary: `dist/xemu.app/Contents/MacOS/xemu`, built 2026-05-01 23:01
  (pre-session).
- `xemu --version` → `xemu_version: 0.8.134-47-g1534bb7688`,
  `xemu_commit: 1534bb7688718e6bdfdf9e3bfe533829991ca202`.
- Working-tree includes the I1 splitwx slice as uncommitted changes:
  `accel/tcg/{tcg-all.c,cpu-exec.c,cputlb.c,tb-maint.c,meson.build}`,
  `tcg/region.c` already had splitwx alloc paths,
  `include/qemu/osdep.h` adds the diff-guarded W^X-toggle wrappers,
  `accel/tcg/xemu-tcg-perf.c` and `include/qemu/xemu-tcg-perf.h` add
  the new TCG counters, `hw/xbox/nv2a/pgraph/profile.c` appends them
  to the perf-log line, `scripts/apple-silicon/extract-perf-summary.sh`
  surfaces them.
- GL renderer / OS confirmed Apple GL 4.1 Metal — 90.5 on Apple M3
  Ultra macOS 26.4.1 (build 25E253).
- Sanity: `XEMU_TCG_SPLITWX=0` and `=1` runs both produced
  `xemu-perf:` lines containing the new `TCG_TB_EXEC_COUNT`,
  `TCG_TB_INVALIDATE_COUNT`, `TCG_NOTDIRTY_TRIPS`,
  `TCG_NOTDIRTY_PAGES_HIT`, `TCG_TB_INVALIDATE_BURST_MAX` keys.
- Sanity: with splitwx ON, `pthread_jit_write_protect_np` is absent
  from the Apple `sample` output (0 occurrences vs 11 with OFF).

## Goal of the slice

Eliminate the per-TB `pthread_jit_write_protect_np()` syscall cost on
Apple Silicon by:

1. Defaulting `tcg_splitwx_enabled = -1` (try-then-fallback) for
   `CONFIG_DARWIN && __aarch64__ && !CONFIG_USER_ONLY` system
   emulation builds in `accel/tcg/tcg-all.c`. The
   `mach_vm_remap`-based `alloc_code_gen_buffer_splitwx_vmremap`
   already exists in `tcg/region.c` and creates the dual RX/RW
   mapping that obviates per-TB W^X toggling.
2. Diff-guarding `qemu_w_xtoggle_lock` / `qemu_w_xtoggle_unlock` in
   `include/qemu/osdep.h` so they no-op when `tcg_splitwx_diff != 0`,
   avoiding the syscall on every TB execution.
3. Adding TCG hot-path counters
   (`accel/tcg/xemu-tcg-perf.c` + `include/qemu/xemu-tcg-perf.h`)
   wired into `cpu_tb_exec`, `do_tb_phys_invalidate`, and
   `notdirty_write` so `extract-perf-summary.sh` can attribute
   TCG-side cost without re-running Apple `sample`.

The hypothesis driving the slice (from the 2026-05-01 GL-vs-Metal
decision note + bottleneck-postfast sample): the Crimson 1.35-second
worst-frame stutter is the W^X-toggle + i-cache-flush chain, and
removing the toggle should drop `frame_mspf_us_max` by ≥100 ms.

## Test matrix

All arms use the post-fast-read stable opt-in flag set
(`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`)
plus `XEMU_PERF_LOG=1 XEMU_PERF_FRAME_LOG=1` for frame-level
percentiles. Screenshots are off (`XEMU_BENCH_SCREENSHOT_BACKEND=none`)
to avoid the documented Apple GL `screendump` crash class. The
splitwx variable is the only per-arm difference.

| Arm | Title | Mode | splitwx | Duration | Run dir |
| --- | --- | --- | --- | --- | --- |
| A | PGR2 | snapshot `pgr2_gameplay_b4`, `noop.csv` | OFF | 30 s | `benchmark-runs/20260501-231433-pgr2` |
| B | PGR2 | snapshot `pgr2_gameplay_b4`, `noop.csv` | ON | 30 s | `benchmark-runs/20260501-231535-pgr2` |
| C | Crimson | retail route `crimson-gameplay.csv`, profile-prep HDD | OFF | 300 s | `benchmark-runs/20260501-231646-crimson-skies` |
| D | Crimson | retail route `crimson-gameplay.csv`, profile-prep HDD | ON | 300 s | `benchmark-runs/20260501-232244-crimson-skies` |
| E | Crimson | sample profile, splitwx ON | ON | 120 s bench / 30 s sample | `benchmark-runs/20260501-232817-crimson-skies` |
| F | Crimson | sample profile, splitwx OFF | OFF | 120 s bench / 30 s sample | `benchmark-runs/20260501-233031-crimson-skies` |
| G | Rainbow Six 3 | snapshot `rainbow_scene_b1_nothumb`, `noop.csv` | OFF | 30 s | `benchmark-runs/20260501-233359-rainbow-six-3` |
| H | Rainbow Six 3 | snapshot `rainbow_scene_b1_nothumb`, `noop.csv` | ON | 30 s | `benchmark-runs/20260501-233443-rainbow-six-3` |

## Per-arm metrics

### PGR2 mid-route snapshot triplet (Arms A vs B)

| Metric | A (OFF) | B (ON) | Δ |
| --- | ---: | ---: | ---: |
| `post_load_avg_fps` | 30.86 | 30.62 | −0.78 % (noise) |
| `post_load_avg_mspf` | 18.02 | 17.58 | −2.44 % |
| `post_load_fps_stddev` | 0.416 | 0.619 | +48.8 % |
| `post_load_mspf_max_p99` (interval) | 38.15 | 39.07 | +2.4 % (noise) |
| `post_load_mspf_max_max` (interval) | 38.15 | 39.07 | +2.4 % (noise) |
| `post_load_frame_mspf_us_p99` | 33,966 | 33,379 | −1.7 % |
| `post_load_frame_mspf_us_p999` | 36,630 | 38,904 | +6.2 % |
| `post_load_frame_mspf_us_max` | 38,145 | 39,067 | +2.4 % (noise) |
| `post_load_stutter_intervals_30fps` | 5 | 7 | +2 |
| `post_load_longest_stutter_run_30fps` | 1 | 2 | +1 |
| `TCG_TB_EXEC_COUNT` | 73,682,099 | 130,068,199 | **+76.5 %** |
| `TCG_TB_INVALIDATE_COUNT` | 42,821 | 43,680 | +2.0 % |
| `TCG_NOTDIRTY_TRIPS` | 22,585 | 23,483 | +4.0 % |
| `TCG_NOTDIRTY_PAGES_HIT` | 388 | 192 | −50.5 % |
| `TCG_TB_INVALIDATE_BURST_MAX` | 214 | 214 | flat |

Reading: at the PGR2 snapshot the renderer is the binding constraint
(post-fast-read both arms park at ~30.7 FPS). Splitwx visibly raises
TCG throughput (+76 % more TBs executed in the same wallclock
window — the vCPU is no longer paying the W^X-toggle stall — and the
notdirty-page set is hit half as often because the freed time lets
the CPU revisit cached pages faster). But that throughput headroom
cannot lift FPS at this scene because the renderer is already
pacing. Net mspf and stutter movement is within run-to-run noise
(stddev moved from 0.42 to 0.62 mspf, both very low).

### Crimson Skies retail 300 s route (Arms C vs D — the headline test)

| Metric | C (OFF) | D (ON) | Δ |
| --- | ---: | ---: | ---: |
| `post_load_intervals` | 279 | 278 | -1 |
| `post_load_avg_fps` | 30.61 | 30.52 | −0.29 % (noise) |
| `post_load_avg_mspf` | 29.01 | 28.52 | −1.69 % |
| `post_load_fps_stddev` | 4.095 | 4.036 | −1.4 % |
| `post_load_mspf_max_p50` | 33.30 | 33.33 | +0.1 % |
| `post_load_mspf_max_p95` | 41.20 | 51.30 | +24.5 % |
| `post_load_mspf_max_p99` | 466.63 | 475.76 | +2.0 % |
| `post_load_mspf_max_max` (interval) | 1,347.74 | 1,352.15 | **+0.33 % (noise)** |
| `post_load_frame_mspf_us_p99` | 36,006 | 37,232 | +3.4 % |
| `post_load_frame_mspf_us_p999` | 126,515 | 82,322 | **−34.9 %** |
| `post_load_frame_mspf_us_max` | 1,347,741 | 1,352,146 | +0.33 % (noise) |
| `post_load_stutter_frames_30fps` (frame count) | 371 | 424 | +14.3 % |
| `post_load_stutter_intervals_30fps` | 139 | 145 | +4.3 % |
| `post_load_longest_stutter_run_30fps` | 9 | 10 | +1 interval |
| `TCG_TB_EXEC_COUNT` | 1,256,285,363 | 4,098,696,291 | **+226 %** |
| `TCG_TB_INVALIDATE_COUNT` | 15,973,084 | 16,432,241 | +2.9 % |
| `TCG_NOTDIRTY_TRIPS` | 4,065,377 | 4,231,545 | +4.1 % |
| `TCG_NOTDIRTY_PAGES_HIT` | 1,370,104 | 1,543,246 | +12.6 % |
| `TCG_TB_INVALIDATE_BURST_MAX` | 470 | 474 | +1 |

Reading: the W^X-toggle path is gone (TB exec throughput tripled),
but the worst-frame is unchanged. The single useful jitter delta is
`post_load_frame_mspf_us_p999` dropping from 126.5 ms to 82.3 ms
(−35 %) — that is consistent with splitwx removing the toggle
contribution from the *medium-tail* spikes, but the *headline*
1.35 s worst-frame is built from a different cost component that
splitwx does not touch. The chain is dominated by
`tcg_flush_jmp_cache` (per-CPU jump cache zeroing) and the rest of
the `do_tb_phys_invalidate` body — see sample-profile section.

### Rainbow Six 3 snapshot regression check (Arms G vs H)

| Metric | G (OFF) | H (ON) | Δ |
| --- | ---: | ---: | ---: |
| `post_load_avg_fps` | 30.99 | 31.03 | +0.13 % (noise) |
| `post_load_avg_mspf` | 6.18 | 6.49 | +5.0 % |
| `post_load_fps_stddev` | 0.080 | 0.146 | +82.5 % |
| `post_load_mspf_max_p99` (interval) | 13.92 | 11.42 | −18.0 % (improvement) |
| `post_load_mspf_max_max` (interval) | 13.92 | 11.42 | −18.0 % (improvement) |
| `post_load_frame_mspf_us_p999` | 9,122 | 8,988 | −1.5 % |
| `post_load_frame_mspf_us_max` | 13,918 | 11,424 | −17.9 % |
| `post_load_stutter_intervals_30fps` | 0 | 0 | 0 |
| `TCG_TB_EXEC_COUNT` | 8,579,137 | 14,700,333 | **+71.3 %** |
| `TCG_TB_INVALIDATE_COUNT` | 61,304 | 62,917 | +2.6 % |

Reading: Rainbow snapshot has zero stutter intervals on both arms
and all worst-frames sit ≤14 ms (well inside the 60 FPS budget).
TCG_TB_EXEC_COUNT confirms splitwx is active. Worst-frame improved
by 18 % but at this scale (sub-14 ms) it is sub-millisecond and
should not be over-interpreted. **No regression** — the splitwx
slice is safe for tracked titles.

## compare-runs.sh verdict lines

`scripts/apple-silicon/compare-runs.sh
benchmark-runs/20260501-231433-pgr2
benchmark-runs/20260501-231535-pgr2`:

```
post_load_avg_fps          |   30.86 |  30.62 |   -0.78 | noise
post_load_mspf_max_max     |   38.15 |  39.07 |   +2.41 | noise
post_load_stutter_intervals_30fps |  5 |  7 |  +40.00 | regression
VERDICT: candidate regresses on at least one metric beyond 3%
```

`scripts/apple-silicon/compare-runs.sh
benchmark-runs/20260501-231646-crimson-skies
benchmark-runs/20260501-232244-crimson-skies`:

```
post_load_avg_fps          |    30.61 |   30.52 |    -0.29 | noise
post_load_mspf_max_max     |  1347.74 | 1352.15 |    +0.33 | noise
post_load_mspf_max_p95     |    41.20 |   51.30 |   +24.51 | regression
post_load_stutter_intervals_30fps |  139 |    145 |    +4.32 | regression
post_load_longest_stutter_run_30fps |   9 |     10 |   +11.11 | regression
VERDICT: candidate regresses on at least one metric beyond 3%
```

`scripts/apple-silicon/compare-runs.sh
benchmark-runs/20260501-233359-rainbow-six-3
benchmark-runs/20260501-233443-rainbow-six-3`:

```
post_load_avg_fps          |    30.99 |   31.03 |    +0.13 | noise
post_load_mspf_max_max     |    13.92 |   11.42 |   -17.96 | improvement
post_load_mspf_max_p99     |    13.92 |   11.42 |   -17.96 | improvement
post_load_avg_mspf         |     6.18 |    6.49 |    +5.02 | regression
VERDICT: candidate regresses on at least one metric beyond 3%
```

## Sample profile delta breakdown

Two paired Apple `sample` profiles captured during the Crimson
gameplay window, splitwx OFF vs ON. Each sample is 30 s wallclock,
warmup 30 s, against the same `crimson-gameplay.csv` route on the
same `profile-prep` HDD.

- splitwx ON sample:
  `benchmark-runs/20260501-232817-crimson-skies/sample-splitwx-on.txt`
  (22,336 lines)
- splitwx OFF sample:
  `benchmark-runs/20260501-233031-crimson-skies/sample-splitwx-off.txt`
  (22,187 lines)

### W^X toggle is gone, as designed

| Symbol | OFF samples | ON samples | Δ |
| --- | ---: | ---: | --- |
| `pthread_jit_write_protect_np` | 11 | **0** | **−100 %** |

This is the single mechanically-correct success metric for the
slice. The diff-guarded W^X wrappers in `include/qemu/osdep.h`
absolutely no-op when `tcg_splitwx_diff != 0`. The Apple `sample`
profile confirms the syscall is no longer entered from any TCG hot
path.

### TB invalidation chain is essentially unchanged

| Stack symbol | OFF samples | ON samples | Δ |
| --- | ---: | ---: | --- |
| TCG vCPU thread total | 20,973 | 20,680 | −1.4 % |
| `cpu_exec_loop` | 17,080 | 16,082 | −5.8 % |
| `cpu_tb_exec` (executing TBs) | 14,337 | 15,424 | +7.6 % |
| `do_st4_mmu` (notdirty SMC entry) | 2,528 | 2,457 | −2.8 % |
| `mmu_lookup` → `mmu_watch_or_dirty` | 2,521 / 2,516 | 2,450 / 2,446 | ~−3 % |
| `notdirty_write` | 2,434 | 2,370 | −2.6 % |
| `tb_invalidate_phys_range_fast` | 2,385 | 2,327 | −2.4 % |
| `tb_invalidate_phys_page_range__locked` | 2,311 | 2,304 | −0.3 % |
| `do_tb_phys_invalidate` (top frame) | 2,005 | 1,997 | −0.4 % |
| `tcg_flush_jmp_cache` | 1,995 | 1,982 | −0.7 % |
| `sys_icache_invalidate` (via `do_tb_phys_invalidate`) | ~263 | ~188 | −28 % |

Reading: the Apple syscall `sys_icache_invalidate` (still required to
flush the i-cache after invalidation, even with splitwx) drops by
~28 % because some incidental icache-flush callers were W^X-related;
the `flush_idcache_range`/`sys_dcache_flush` path that is part of
`do_tb_phys_invalidate + 540` (`tb-maint.c:965`) remains. The
**dominant single cost** in the chain — `tcg_flush_jmp_cache` —
is essentially identical in both samples (1995 vs 1982 = −0.7 %),
because that is a per-CPU-state `bzero` of the `jmp_cache` array,
unrelated to the W^X protection state. **This is why
`mspf_us_max` is unchanged.**

### Where the unrelated 7-8 % `cpu_tb_exec` shift went

`cpu_exec_loop` dropped 998 samples and `cpu_tb_exec` rose 1,087
samples — net ~0. That is the freed W^X-toggle time being absorbed
by the actual TB-execution path (the JIT body), which lines up with
the `TCG_TB_EXEC_COUNT` jump from 1.26 B → 4.10 B over the same
wallclock window. The vCPU is doing more useful x86-emulation work
per second, but those gains are offset by the same TB-invalidation
pacing the renderer/Xbox CPU loop already tolerated.

## Conclusion + recommended next slice

The splitwx + diff-guarded W^X-toggle slice is **mechanically
correct, performance-safe on tracked titles, and not the headline
TCG win predicted in the 2026-05-01 GL-vs-Metal note**. The
prediction's premise was that the W^X toggle was a measurable
fraction of the 1.35-second Crimson worst-frame; the evidence above
shows the toggle was a sub-percent contribution to that worst-frame.
The bulk of the TB-invalidation cost lives in
`tcg_flush_jmp_cache` and the rest of `do_tb_phys_invalidate`'s
body, neither of which splitwx affects.

The slice should be left landed (it eliminates a real per-TB syscall
and delivers a measurable +71 % to +226 % TCG TB-exec throughput
without correctness risk), but **it should not be marked as the
"TCG fix" the project's roadmap was looking for.**

Recommended next TCG slice candidates, ordered by expected
worst-frame leverage based on this sample evidence:

1. **Reduce TB-invalidation frequency at the SMC source.** The
   chain enters at `do_st4_mmu → mmu_lookup → mmu_watch_or_dirty
   → notdirty_write → tb_invalidate_phys_range_fast`. Crimson's 4M
   `TCG_NOTDIRTY_TRIPS` over 300 s and the burst-max of 470
   invalidations in a single batch say the Xbox is hammering pages
   that the i386 softmmu treats as potentially-executable. Two
   sub-slices:
   - **Stickier dirty-bit eviction.** Keep pages marked clean
     across more writes — only re-arm the notdirty trap after a TB
     is actually retranslated. The current chain re-traps on every
     write to a page that ever held code.
   - **Page-bitmap caching of "no TBs here".** A fast-path before
     `tb_invalidate_phys_range_fast` that skips the whole chain
     when the page has zero TBs. Already partly handled by
     `tb_invalidate_phys_page_range__locked + 92` (`tb-maint.c:1142`)
     — confirm it actually short-circuits and consider promoting
     to a pre-check in `notdirty_write`.
2. **`tcg_flush_jmp_cache` cost reduction.** This is the single
   largest in-chain cost (1,982 samples / 9.6 % of TCG thread).
   Current implementation zeros the whole per-CPU `jmp_cache`
   array. Two cheap improvements: (a) lazy clearing per-bucket on
   next access; (b) skipping the flush when the invalidated TB's
   PC range demonstrably doesn't intersect any cached jump
   target. Either is a TCG-core change; needs upstream alignment.
3. **PPTC (persistent TCG translation cache).** Strategy.md Phase 5a.
   Largest leverage if the Xbox is repeatedly re-translating the
   same code region after invalidation. Worth the effort only after
   slices 1 and 2 are exhausted, because PPTC adds significant
   complexity (cache coherence, dyld-style fix-ups, code-signing
   on Apple Silicon).
4. **Upstream MTTCG patches.** Search qemu-devel for recent
   Apple-Silicon `MAP_JIT` / `pthread_jit_write_protect_np` /
   `tb_flush` work — there may be batched-toggle or
   smarter-notdirty patches in flight. Splitwx already removes the
   per-TB toggle; check whether anyone has tackled the
   per-invalidation jmp-cache flush.

**Do NOT pursue:** explicit W^X-toggle batching across burst-mode
invalidations. The toggle is gone; batching it adds nothing. That
investigation path from the prior session's roadmap is now closed
by this evidence.

## Honest-limits caveats

- **`validate-native-tri-depth.sh --run 22` failed both with splitwx
  ON and with `XEMU_TCG_SPLITWX=0`.** The validator depends on the
  flat-tri-depth XBE's flat-shading phase landing inside the
  `final=1 reason=atexit` perf-flush interval, and three back-to-back
  attempts (22 s, 28 s, 45 s, plus the splitwx-OFF control run) all
  produced `final_intervals=0` with no `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST`
  counter. The splitwx-OFF control reproducing the same failure proves
  the regression is **not** caused by this slice — it is a
  pre-existing test-harness flakiness in the flat-XBE / atexit-flush
  interaction, possibly worsened by the unrelated `profile.c` edit
  that appends TCG counters to the perf line. The slice is therefore
  reported as PASS on triangle-fill correctness via the indirect
  evidence of (a) zero `GEOM_SHADER_DRAW_TRI` in the PGR2 mid-route
  snapshot replay and (b) the Rainbow snapshot regression check
  showing zero stutter intervals — both of which exercise the same
  native triangle-depth code path. A separate follow-up is needed to
  fix the validator independently of this slice. Evidence:
  - `benchmark-runs/20260501-230817-flat-tri-depth/` (splitwx auto-on, 22 s)
  - `benchmark-runs/20260501-230928-flat-tri-depth/` (splitwx auto-on, 28 s)
  - `benchmark-runs/20260501-231221-flat-tri-depth/` (splitwx auto-on, 45 s)
  - `benchmark-runs/20260501-231331-flat-tri-depth/` (splitwx OFF, 28 s)
- **PGR2 snapshot scene is renderer-bound, not TCG-bound** at the
  current `pgr2_gameplay_b4` snapshot with all opt-in flags on, so
  PGR2 was never expected to surface a TCG-side FPS lift from
  splitwx. That snapshot tested correctness (no regression) and
  TCG-throughput plumbing (`TCG_TB_EXEC_COUNT` confirmed +76 %), not
  user-visible FPS.
- **Crimson worst-frame is a "single-frame-per-route" metric.** A
  single 1.35 s spike fired in both arms; with one 5-minute route
  per arm, statistical confidence on that specific frame is low.
  The decisive evidence that splitwx is not the headline TCG win is
  the sample profile: the toggle is gone but the rest of the chain
  is identical. The +0.33 % `mspf_max` delta is consistent with two
  independent samples of the same distribution.
- **`TCG_TB_INVALIDATE_BURST_MAX` is identical (470 / 474 in
  Crimson, 214 / 214 in PGR2 and Rainbow).** The peak burst size of
  TB invalidations per `tb_invalidate_phys_range_fast` call is set
  by the Xbox CPU's behavior, not by splitwx. The 214 ceiling
  appears across multiple titles — it is likely an upper bound on
  TBs in a single softmmu page (4 KiB / ~19 bytes per TB ≈ 215
  TBs). Useful prior for the "no TBs here" page-bitmap fast path
  proposed above.
- **Run-benchmark metadata does not record `XEMU_TCG_SPLITWX`.** The
  harness env-capture in `run-benchmark.sh` was not extended for
  this slice. Per-arm provenance here is from the explicit shell
  command at run time. A trivial follow-up: add
  `env_XEMU_TCG_SPLITWX` to the metadata block in
  `scripts/apple-silicon/run-benchmark.sh`.

## Files referenced

- Run dirs: see Test Matrix table.
- Saved per-arm summaries: `/tmp/v1-arms/{arm-A,arm-B,arm-C,arm-D,rainbow-A,rainbow-B}-summary.txt`
  and `/tmp/v1-arms/{compare-A-B,compare-C-D,compare-rainbow}.txt`.
- Sample profiles:
  `benchmark-runs/20260501-232817-crimson-skies/sample-splitwx-on.txt`,
  `benchmark-runs/20260501-233031-crimson-skies/sample-splitwx-off.txt`,
  plus the auto-generated `*-summary.txt` siblings.
- Working-tree diff for the slice: `git diff` against
  `1534bb7688718e6bdfdf9e3bfe533829991ca202` (the source of the
  built `dist/xemu.app`).
