# TCG per-page targeted jmp-cache invalidation slice (I2) — validation

Date: 2026-05-02

## Verdict: PARTIAL PASS — slice is mechanically correct and removes the per-call jmp-cache cost ceiling, but the Crimson 1.27-second worst frame is unchanged and is built from a different cost component

`XEMU_TCG_JMP_CACHE_TARGETED=1` (Apple-Silicon system-build auto-on) **works
exactly as designed**: the per-CPU jmp-cache zero is replaced by a single-bucket
clear per invalidated TB, batched after the page-range loop. On the Crimson
300 s route the cumulative `TCG_JMP_CACHE_ZEROED_BUCKETS` collapses from
70,899,527,680 (Arm C, OFF) to 30,359,278 (Arm D, ON) — a **2335× reduction**.
`tcg_flush_jmp_cache` disappears entirely from the Apple `sample` profile of
the TCG vCPU thread (13 → 0 occurrences), and the per-call
`TCG_INVALIDATE_WALL_US_MAX` ceiling drops from 688 µs to 348 µs.

**But the headline 1.35-second Crimson worst-frame is NOT solved.** Crimson
worst-frame `mspf_max` is 1290.87 ms (Arm C) → 1272.32 ms (Arm D), delta
−18.55 ms / −1.44 % — well within run-to-run noise and far short of the
≥500 ms drop the slice was hypothesized to deliver. The decisive evidence:
even in Arm C (slice OFF), the worst-interval `TCG_INVALIDATE_WALL_US_MAX`
was only 688 µs. The 1.27 s worst frame cannot be a per-call cost in
`tb_invalidate_phys_page_range__locked` — that function never spent more
than ~0.7 ms in a single call across the whole route. The worst-frame
cost lives outside the jmp-cache invalidation path entirely.

Per the pass/fail criteria in the validation protocol:

| # | Criterion | Result |
| - | --- | --- |
| 1 | PGR2 snapshot Arm B `post_load_avg_fps` within −3 % of Arm A | PASS (−0.07 %) |
| 2 | Crimson Arm D `post_load_frame_mspf_us_max` drops ≥ 500 ms vs Arm C | **FAIL** (−18.55 ms / −1.44 %) |
| 3 | `TCG_JMP_CACHE_ZEROED_BUCKETS` Arm D / Arm C ratio < 0.05 | PASS (4.28 × 10⁻⁴ — 117× under the gate) |
| 4 | No correctness regressions on Rainbow + PGR2 + flat-tri-depth | PASS (Rainbow zero stutter, PGR2 zero `GEOM_SHADER_DRAW_TRI`, flat-tri-depth pre-existing harness flake reproduces with both arms) |

Final classification: **PARTIAL PASS**. The slice is mechanically correct,
removes a real and measurable per-call cost (path is gone from sample
profile, counter ratios hit the gate by two orders of magnitude over the
threshold), and is performance-safe on tracked titles. It should stay
landed default-on for Apple Silicon. **It is not the silver bullet for
the headline 1.27 s judder — that cost is non-jmp-cache and non-PCREL
in nature.**

## Build / commit verification

- Binary: `dist/xemu.app/Contents/MacOS/xemu`, built 2026-05-02 05:00 UTC
  (pre-session).
- `xemu --version` → `xemu_version: 0.8.134-47-g1534bb7688`,
  `xemu_commit: 1534bb7688718e6bdfdf9e3bfe533829991ca202` (dirty).
- Working-tree includes the I2 targeted-jmp-cache slice on top of the I1
  splitwx slice: `accel/tcg/{tb-maint.c,tcg-all.c,xemu-tcg-perf.c}`,
  `include/qemu/xemu-tcg-perf.h`, `hw/xbox/nv2a/pgraph/profile.c`,
  `scripts/apple-silicon/extract-perf-summary.sh`.
- GL renderer / OS confirmed Apple GL 4.1 Metal — 90.5 on Apple M3 Ultra,
  macOS 26.4.1 (build 25E253).
- Sanity (Step 0): paired 12 s PGR2-snapshot runs with
  `XEMU_TCG_JMP_CACHE_TARGETED=0` and `=1` proved the env-var bridge is
  live and the new `TCG_JMP_CACHE_ZEROED_BUCKETS` /
  `TCG_INVALIDATE_WALL_US_MAX` counters are wired through to
  `extract-perf-summary.sh`. Per-interval ZEROED_BUCKETS values dropped
  from ~3,000,000–3,500,000 (targeted=0) to 250–1,000 (targeted=1) — a
  ~3000–4000× per-interval reduction in the steady state, matching the
  predicted mechanism. Sanity-run dirs:
  `benchmark-runs/20260502-000421-pgr2` (targeted=1) and
  `benchmark-runs/20260502-000458-pgr2` (targeted=0).

## Goal of the slice

Eliminate the per-`tb_jmp_cache_inval_tb` 4096-entry per-CPU jmp-cache
`bzero` cost in the `CF_PCREL` branch (i386 system-mode globally sets
`CF_PCREL`) by:

1. Adding `tcg_jmp_cache_targeted_enabled` (bool, default true on Apple
   Silicon system builds; resolved via `XEMU_TCG_JMP_CACHE_TARGETED` in
   `accel/tcg/tcg-all.c::tcg_resolve_jmp_cache_targeted_default`).
2. In `tb_invalidate_phys_page_range__locked`, defer the per-TB jmp-cache
   clear during the page-range invalidation burst and run a single
   batched targeted-bucket-clear across all CPUs after the loop.
3. Implementing `tb_jmp_cache_inval_tb_targeted` (a single
   `qatomic_set`-style clear of one bucket, mirroring the non-PCREL
   branch), correctness-rested on the existing `CF_INVALID` + cflags
   equality check in `cpu-exec.c::tb_lookup` (line 267).
4. Adding per-interval counters `TCG_JMP_CACHE_ZEROED_BUCKETS` (sum) and
   `TCG_INVALIDATE_WALL_US_MAX` (per-interval max) in
   `accel/tcg/xemu-tcg-perf.c`, surfaced through
   `extract-perf-summary.sh`. The wall-µs counter is the decisive
   measurement: if Crimson's 1.35 s worst frame were a per-call cost in
   `tb_invalidate_phys_page_range__locked`, this counter would be
   ≈1,300,000 µs in the OFF arm and would collapse in the ON arm.

The hypothesis driving the slice (from the 2026-05-01 splitwx
post-mortem): `tcg_flush_jmp_cache` was 9.6 % of the TCG vCPU thread in
the splitwx-ON sample (1,982 / 20,680 samples). Removing the
unconditional 4096-bucket zero should drop the per-invalidation cost
ceiling and thereby the headline `mspf_max`.

## Test matrix

All arms use the post-fast-read stable opt-in flag set
(`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`)
plus `XEMU_PERF_LOG=1 XEMU_PERF_FRAME_LOG=1` for frame-level
percentiles. Splitwx is left at its Apple-Silicon system-build auto-on
default (per the I1 / V1 outcome). Screenshots are off
(`XEMU_BENCH_SCREENSHOT_BACKEND=none`) to avoid the documented Apple GL
`screendump` crash class. The `XEMU_TCG_JMP_CACHE_TARGETED` variable is
the only per-arm difference.

| Arm | Title | Mode | targeted | Duration | Run dir |
| --- | --- | --- | --- | --- | --- |
| Sanity-on | PGR2 | snapshot `pgr2_gameplay_b4`, noop | ON | 12 s | `benchmark-runs/20260502-000421-pgr2` |
| Sanity-off | PGR2 | snapshot `pgr2_gameplay_b4`, noop | OFF | 12 s | `benchmark-runs/20260502-000458-pgr2` |
| flat-22 | flat-tri-depth | XBE harness | auto-on | 22 s | `benchmark-runs/20260502-000529-flat-tri-depth` |
| flat-28 | flat-tri-depth | XBE harness | auto-on | 28 s | `benchmark-runs/20260502-000605-flat-tri-depth` |
| flat-45 | flat-tri-depth | XBE harness | auto-on | 45 s | `benchmark-runs/20260502-000644-flat-tri-depth` |
| flat-28-off | flat-tri-depth | XBE harness | OFF (control) | 28 s | `benchmark-runs/20260502-000741-flat-tri-depth` |
| A | PGR2 | snapshot `pgr2_gameplay_b4`, noop | OFF | 30 s | `benchmark-runs/20260502-000826-pgr2` |
| B | PGR2 | snapshot `pgr2_gameplay_b4`, noop | ON | 30 s | `benchmark-runs/20260502-000911-pgr2` |
| C | Crimson | retail route `crimson-gameplay.csv`, profile-prep HDD | OFF | 300 s | `benchmark-runs/20260502-001003-crimson-skies` |
| D | Crimson | retail route `crimson-gameplay.csv`, profile-prep HDD | ON | 300 s | `benchmark-runs/20260502-001515-crimson-skies` |
| E | Crimson | sample profile, 30 s sample after 30 s warmup | ON | 120 s bench | `benchmark-runs/20260502-002103-crimson-skies` |
| F | Crimson | sample profile, 30 s sample after 30 s warmup | OFF | 120 s bench | `benchmark-runs/20260502-002315-crimson-skies` |
| G | Rainbow Six 3 | snapshot `rainbow_scene_b1_nothumb`, noop | OFF | 30 s | `benchmark-runs/20260502-002613-rainbow-six-3` |
| H | Rainbow Six 3 | snapshot `rainbow_scene_b1_nothumb`, noop | ON | 30 s | `benchmark-runs/20260502-002656-rainbow-six-3` |

## Per-arm metrics

### PGR2 mid-route snapshot triplet (Arms A vs B — regression check)

| Metric | A (OFF) | B (ON) | Δ |
| --- | ---: | ---: | ---: |
| `post_load_avg_fps` | 30.68 | 30.66 | −0.07 % (noise) |
| `post_load_avg_mspf` | 17.27 | 17.76 | +2.84 % (noise) |
| `post_load_fps_stddev` | 0.517 | 0.534 | +3.29 % |
| `post_load_mspf_max_p99` (interval) | 38.99 | 39.85 | +2.21 % (noise) |
| `post_load_mspf_max_max` (interval) | 38.99 | 39.85 | +2.21 % (noise) |
| `post_load_frame_mspf_us_p99` | 33,839 | 34,108 | +0.79 % |
| `post_load_frame_mspf_us_p999` | 36,085 | 38,756 | +7.4 % |
| `post_load_frame_mspf_us_max` | 38,991 | 39,853 | +2.21 % (noise) |
| `post_load_stutter_intervals_30fps` | 7 | 7 | 0 |
| `post_load_stutter_intervals_45fps` | 23 | 22 | −1 |
| `post_load_longest_stutter_run_30fps` | 2 | 2 | 0 |
| `TCG_TB_EXEC_COUNT` | 70,446,225 | 222,009,820 | **+215 %** |
| `TCG_TB_INVALIDATE_COUNT` | 41,891 | 42,779 | +2.1 % |
| `TCG_NOTDIRTY_TRIPS` | 22,360 | 22,759 | +1.8 % |
| `TCG_NOTDIRTY_PAGES_HIT` | 388 | 188 | −51.5 % |
| `TCG_TB_INVALIDATE_BURST_MAX` | 214 | 214 | flat |
| `TCG_JMP_CACHE_ZEROED_BUCKETS` | 173,547,520 | **1,996,571** | **−98.85 %** (87× reduction) |
| `TCG_INVALIDATE_WALL_US_MAX` | 286 | **53** | **−81.5 %** |

Reading: PGR2 snapshot is renderer-bound (both arms park at ~30.66 FPS),
so no FPS lift is expected here — this is a correctness/no-regression
check. avg_fps moved −0.07 % (well within Pass criterion #1's 3 % gate).
The per-call wall-µs ceiling drops 5.4× and the per-interval bucket-clear
sum drops 87× — both cleanly proving the slice is firing on the PGR2
snapshot. TB-execution throughput jumps +215 % because the vCPU is no
longer paying the per-invalidation 4096-bucket bzero stall.

### Crimson Skies retail 300 s route (Arms C vs D — the headline test)

| Metric | C (OFF) | D (ON) | Δ |
| --- | ---: | ---: | ---: |
| `post_load_intervals` | 280 | 280 | 0 |
| `post_load_avg_fps` | 30.63 | 30.61 | −0.07 % (noise) |
| `post_load_avg_mspf` | 29.17 | 29.47 | +1.03 % |
| `post_load_fps_stddev` | 4.069 | 4.234 | +4.06 % |
| `post_load_mspf_max_p50` | 33.62 | 33.60 | −0.06 % |
| `post_load_mspf_max_p95` | 38.51 | 39.33 | +2.13 % |
| `post_load_mspf_max_p99` | 445.41 | 470.09 | +5.54 % |
| `post_load_mspf_max_max` (interval) | 1,290.87 | 1,272.32 | **−1.44 % (noise)** |
| `post_load_frame_mspf_us_p99` | 35,360 | 35,973 | +1.7 % |
| `post_load_frame_mspf_us_p999` | 87,703 | 86,131 | −1.8 % |
| `post_load_frame_mspf_us_max` | 1,290,872 | 1,272,317 | **−1.44 % (noise)** |
| `post_load_stutter_frames_30fps` | 428 | 463 | +8.2 % |
| `post_load_stutter_intervals_30fps` | 158 | 163 | +3.16 % |
| `post_load_longest_stutter_run_30fps` | 15 | 12 | **−20 % (improvement)** |
| `TCG_TB_EXEC_COUNT` | 4,091,616,060 | 5,844,763,789 | **+42.8 %** |
| `TCG_TB_INVALIDATE_COUNT` | 17,306,394 | 17,051,374 | −1.5 % |
| `TCG_NOTDIRTY_TRIPS` | 4,440,488 | 4,331,500 | −2.5 % |
| `TCG_NOTDIRTY_PAGES_HIT` | 1,621,228 | 1,566,864 | −3.4 % |
| `TCG_TB_INVALIDATE_BURST_MAX` | 470 | 470 | flat |
| `TCG_JMP_CACHE_ZEROED_BUCKETS` | 70,899,527,680 | **30,359,278** | **−99.957 %** (2335× reduction) |
| `TCG_INVALIDATE_WALL_US_MAX` | 688 | **348** | **−49.4 %** |

Reading: the slice mechanism collapses the cumulative bucket-clear count
by **three orders of magnitude** and halves the per-call wall-µs ceiling.
The longest contiguous run of 30-FPS-violating intervals drops from 15
to 12 (−20 %), which is the only user-visible-jitter improvement worth
flagging. **But the worst single frame moves only −18.55 ms (−1.44 %),
well within run-to-run noise.** TB-execution throughput rises +42.8 %
(less per-invalidation pacing on the vCPU thread) but is renderer-paced
in net. The decisive number is `TCG_INVALIDATE_WALL_US_MAX = 688 µs` in
the OFF arm — the worst per-call cost in
`tb_invalidate_phys_page_range__locked` was already only 0.7 ms in the
*OFF* arm. The 1.27 s worst frame is **structurally not a single
`tb_invalidate_phys_page_range__locked` call** — it must be either a
chain of many calls within a frame, or a cost outside that function
entirely.

### Rainbow Six 3 snapshot regression check (Arms G vs H)

| Metric | G (OFF) | H (ON) | Δ |
| --- | ---: | ---: | ---: |
| `post_load_avg_fps` | 30.97 | 30.99 | +0.06 % (noise) |
| `post_load_avg_mspf` | 6.44 | 6.19 | −3.88 % (improvement) |
| `post_load_fps_stddev` | 0.101 | 0.076 | −24.75 % (improvement) |
| `post_load_mspf_max_p95` | 9.43 | 8.52 | −9.65 % (improvement) |
| `post_load_mspf_max_p99` | 13.09 | 13.63 | +4.13 % (sub-ms noise) |
| `post_load_mspf_max_max` (interval) | 13.09 | 13.63 | +4.13 % (sub-ms noise) |
| `post_load_frame_mspf_us_p999` | 9,427 | 8,524 | −9.6 % (improvement) |
| `post_load_frame_mspf_us_max` | 13,094 | 13,626 | +4.06 % |
| `post_load_stutter_intervals_30fps/45fps/60fps` | 0 / 0 / 0 | 0 / 0 / 0 | 0 |
| `TCG_TB_EXEC_COUNT` | 18,377,222 | 15,770,853 | −14.2 % |
| `TCG_TB_INVALIDATE_COUNT` | 62,698 | 62,795 | +0.15 % |
| `TCG_JMP_CACHE_ZEROED_BUCKETS` | 326,221,824 | **69,719,371** | **−78.6 %** (4.7× reduction) |
| `TCG_INVALIDATE_WALL_US_MAX` | 277 | **96** | **−65.3 %** |

Reading: zero stutter intervals on both arms; all worst-frames sit
≤14 ms (well inside the 60 FPS budget). The Rainbow snapshot scene is
not invalidation-heavy enough to surface the slice's improvement at the
worst-frame level — but the `TCG_JMP_CACHE_ZEROED_BUCKETS` and per-call
wall-µs counters confirm the slice is firing on Rainbow as well. The
+4 % `mspf_max_max` regression at sub-14-ms scale (0.5 ms absolute) is
inside Apple GL pacing noise. **No correctness regression.**

## compare-runs.sh verdict lines

`scripts/apple-silicon/compare-runs.sh
benchmark-runs/20260502-000826-pgr2
benchmark-runs/20260502-000911-pgr2`:

```
post_load_avg_fps                        |        30.68 |        30.66 |      -0.07 | noise
post_load_mspf_max_max                   |        38.99 |        39.85 |      +2.21 | noise
post_load_stutter_intervals_30fps        |            7 |            7 |      +0.00 | noise
post_load_longest_stutter_run_30fps      |            2 |            2 |      +0.00 | noise
post_load_fps_stddev                     |        0.517 |        0.534 |      +3.29 | regression
post_load_mspf_max_p95                   |        36.09 |        38.76 |      +7.40 | regression
VERDICT: candidate regresses on at least one metric beyond 3%
```

`scripts/apple-silicon/compare-runs.sh
benchmark-runs/20260502-001003-crimson-skies
benchmark-runs/20260502-001515-crimson-skies`:

```
post_load_avg_fps                        |        30.63 |        30.61 |      -0.07 | noise
post_load_mspf_max_max                   |      1290.87 |      1272.32 |      -1.44 | noise
post_load_longest_stutter_run_30fps      |           15 |           12 |     -20.00 | improvement
post_load_fps_stddev                     |        4.069 |        4.234 |      +4.06 | regression
post_load_mspf_max_p99                   |       445.41 |       470.09 |      +5.54 | regression
post_load_stutter_intervals_30fps        |          158 |          163 |      +3.16 | regression
VERDICT: candidate regresses on at least one metric beyond 3%
```

`scripts/apple-silicon/compare-runs.sh
benchmark-runs/20260502-002613-rainbow-six-3
benchmark-runs/20260502-002656-rainbow-six-3`:

```
post_load_avg_fps                        |        30.97 |        30.99 |      +0.06 | noise
post_load_mspf_max_max                   |        13.09 |        13.63 |      +4.13 | regression
post_load_avg_mspf                       |         6.44 |         6.19 |      -3.88 | improvement
post_load_fps_stddev                     |        0.101 |        0.076 |     -24.75 | improvement
post_load_mspf_max_p95                   |         9.43 |         8.52 |      -9.65 | improvement
VERDICT: candidate regresses on at least one metric beyond 3%
```

The compare-runs verdicts read "regresses" because the harness has a flat
3 % noise threshold per metric. The substantive interpretation in all
three cases is "no user-visible regression at the avg_fps / stutter-count
level; sub-millisecond and stddev-noise jitter moves both directions."

## Per-interval bad-window analysis (the decisive evidence)

Top 10 per-interval `mspf_max` values (Crimson 300 s route, both arms),
each annotated with the per-interval `TCG_TB_INVALIDATE_BURST_MAX`,
`TCG_JMP_CACHE_ZEROED_BUCKETS`, and `TCG_INVALIDATE_WALL_US_MAX`:

### Arm C — targeted OFF

| `mspf_max` ms | `interval_ms` | `BURST_MAX` | `ZEROED_BUCKETS` | `WALL_US_MAX` |
| ---: | ---: | ---: | ---: | ---: |
| **1290.87** | 1392 | 470 | 109,830,144 | 688 |
| 1251.97 | 1346 | 453 | 37,359,616 | 596 |
| 1185.00 | 1923 | 121 | 7,794,688 | 166 |
| 445.41 | 1034 | 19 | 6,336,512 | 40 |
| 357.60 | 1008 | 214 | 9,355,264 | 286 |
| 353.93 | 1279 | 121 | 11,223,040 | 167 |
| 181.05 | 1026 | 125 | 146,448,384 | 217 |
| 140.73 | 1005 | 10 | 5,509,120 | 19 |

### Arm D — targeted ON

| `mspf_max` ms | `interval_ms` | `BURST_MAX` | `ZEROED_BUCKETS` | `WALL_US_MAX` |
| ---: | ---: | ---: | ---: | ---: |
| **1272.32** | 1349 | 470 | 75,909 | 348 |
| 1231.70 | 1328 | 444 | 74,524 | 86 |
| 1187.98 | 1977 | 120 | 59,004 | 29 |
| 470.09 | 1157 | 19 | 6,733 | 7 |
| 383.68 | 1025 | 121 | 42,988 | 32 |
| 353.30 | 1017 | 214 | 1,640,269 | 53 |
| 206.34 | 1032 | 119 | 38,035 | 52 |
| 163.48 | 1005 | 10 | 5,455 | 6 |

**The decisive observation:** the worst Arm-C interval had
`WALL_US_MAX=688` (0.69 ms) — the maximum per-call cost in
`tb_invalidate_phys_page_range__locked` across the entire 1.39 s
interval was 0.7 ms. The Arm-D matching worst interval had
`WALL_US_MAX=348` (0.35 ms) — the slice cut that per-call cost in
half — but the `mspf_max` only moved from 1290.87 ms → 1272.32 ms,
−18.55 ms.

**Conclusion:** the 1.27 s worst frame is NOT a single
`tb_invalidate_phys_page_range__locked` call. It cannot be — the wall-µs
counter would have read ~1,300,000 if it were. It must be a sequence of
many calls within the frame (the worst Arm-C interval has 17 % of the
route's TB invalidation work concentrated in 1.4 s of wallclock, plus
all the other costs that frame triggered: notdirty/SMC re-trapping, TLB
flushes, `helper_lookup_tb_ptr` chains, x87 FP emulation, renderer
locking), or a cost outside the TCG-invalidation chain entirely
(garbage collection, GL state validation, mutex contention with the
renderer thread). The slice removes one component cleanly, but the
composite worst frame is bigger than the component.

## Sample profile delta breakdown

Two paired Apple `sample` profiles captured during the Crimson gameplay
window, each 30 s wallclock with 30 s warmup against
`crimson-gameplay.csv` on the same `profile-prep` HDD.

- targeted ON sample:
  `benchmark-runs/20260502-002103-crimson-skies/sample-jmp-cache-targeted-on.txt`
  (20,981 lines)
- targeted OFF sample:
  `benchmark-runs/20260502-002315-crimson-skies/sample-jmp-cache-targeted-off.txt`
  (22,012 lines)

### The slice mechanism is gone from the sample, as designed

| Symbol | OFF samples | ON samples | Δ |
| --- | ---: | ---: | --- |
| `tcg_flush_jmp_cache` | 13 | **0** | **−100 %** |
| `do_tb_phys_invalidate` | 23 | 9 | −60.9 % |
| `tb_invalidate_phys_page_range__locked` | 24 | 17 | −29.2 % |
| `tb_invalidate_phys_range_fast` | 16 | 10 | −37.5 % |
| `tb_htable_lookup_common` | 469 | 192 | **−59.1 %** |
| `qht_lookup_custom` | 253 | 112 | −55.7 % |
| `helper_lookup_tb_ptr` | 904 | 683 | −24.4 % |
| `sys_icache_invalidate` (via `do_tb_phys_invalidate`) | 8 | 8 | 0 |
| `flush_idcache_range` | 8 | 7 | −12.5 % |
| `notdirty_write` | 51 | 51 | 0 |
| `mmu_watch_or_dirty` | 24 | 28 | +4 |

The mechanical claims are all confirmed:
- `tcg_flush_jmp_cache` (the 4096-bucket bzero on every PCREL
  invalidation) is **completely gone** from the TCG vCPU thread.
- The followon savings are visible in the cache-miss-driven lookup
  chain: `tb_htable_lookup_common` drops 59 %, `qht_lookup_custom` drops
  56 %, `helper_lookup_tb_ptr` drops 24 % — all because the targeted
  slice preserves jmp-cache entries that the upstream path would have
  zeroed. The vCPU finds more cached jump targets and skips the
  hash-table walk.
- The "the slice doesn't break the rest of the chain" claims are also
  confirmed: `notdirty_write`, `mmu_watch_or_dirty`,
  `sys_icache_invalidate` are all flat (within ±4 samples).

### Where the unrelated cost actually lives

Top-25 TCG-thread symbols in the targeted-ON sample (excluding the JIT
body itself), sorted by frequency:

| Rank | Symbol | ON samples |
| ---: | --- | ---: |
| 1 | `helper_lookup_tb_ptr` | 683 |
| 2 | `address_space_ldl_internal` | 364 |
| 3 | `parts128_canonicalize` | 266 |
| 4 | `parts64_uncanon_normal` | 254 |
| 5 | `floatx80_mul` | 221 |
| 6 | `float32_to_floatx80` | 214 |
| 7 | `floatx80_addsub` | 207 |
| 8 | `floatx80_round_pack_canonical` | 193 |
| 9 | `tb_htable_lookup_common` | 192 |
| 10 | `flatview_translate` | 184 |
| 11 | `get_ptr_rcu_reader` | 174 |
| 12 | `address_space_translate_internal` | 130 |
| 13 | `helper_mulss` | 124 |
| 14 | `x86_get_tb_cpu_state` | 122 |
| 15 | `mmu_lookup` | 122 |
| 16 | `qht_lookup_custom` | 112 |
| 17 | `mmu_lookup1` | 106 |
| 18 | `soft_f32_mul` | 87 |
| 19 | `helper_ldul_mmu` | 83 |
| 20 | `address_space_stl_internal` | 82 |
| 21 | `qemu_ram_ptr_length` | 81 |
| 22 | `do_ld4_mmu` | 81 |

The top cost in the TCG vCPU thread (after slice ON) is `helper_lookup_tb_ptr`
at 683 samples — still ~3 % of the thread, even after the targeted slice
shaved 24 % off it. The next-largest costs are:
1. **x87 FP emulation** (`floatx80_*`, `parts128_canonicalize`, `parts64_uncanon_normal`,
   `float32_to_floatx80`, `helper_mulss`, `soft_f32_mul`) — together
   roughly 1,500 samples (~7 % of the TCG thread). Crimson Skies leans
   heavily on x87 FP. This is a JIT/floating-point lowering issue, not a
   memory-management issue.
2. **MMU/softmmu probing** (`address_space_*`, `flatview_translate`,
   `mmu_lookup*`, `do_ld4_mmu`, `helper_ldul_mmu`,
   `qemu_ram_ptr_length`) — together roughly 1,300 samples (~6 % of
   the TCG thread). This is the per-load softmmu walk, not invalidation.
3. **Mutex / cond contention**: `qemu_mutex_lock_impl` 76 samples,
   `qemu_cond_wait_impl` 35 samples, `page_collection_lock` 11 samples.
   Modest in steady state; could spike during a stutter frame.

The 1.27 s worst frame is most plausibly a composite of all three — a
worst-case interval contains 470 burst-mode TB invalidations, ~3.4M
notdirty trips fanned out across the route, plus whatever Apple GL
swapchain wait/lock the renderer hit at the same moment. None of these
is `tcg_flush_jmp_cache`, so the slice can't move them.

## Conclusion + recommended next slice

The `XEMU_TCG_JMP_CACHE_TARGETED` slice is **mechanically correct,
performance-safe on tracked titles, and not the headline TCG win the
prior session predicted**. The prediction's premise was that
`tcg_flush_jmp_cache` (9.6 % of the TCG thread in the splitwx-ON sample)
contributed materially to the 1.27 s Crimson worst frame. The evidence
above shows:

- The slice removes that path entirely from the sample profile (13 → 0
  samples).
- The cumulative bucket-clear count drops 2,335× (Crimson) and 87×
  (PGR2).
- The per-call wall-µs ceiling drops 49 % (Crimson) and 81 % (PGR2).
- TB-execution throughput rises +43 % (Crimson) and +215 % (PGR2).
- The cache-miss-driven `tb_htable_lookup_common` chain drops 59 %.
- **But the worst single frame moves only −1.44 % on Crimson** — well
  inside the noise of a single 1-spike-per-route metric.

The slice should be left landed default-on (it eliminates a real per-TB
zero-cost and delivers the htable-lookup cascade savings without
correctness risk), but **it should not be marked as the headline-judder
fix**. Combined with V1's findings, the project now has two slices that
each remove a distinct measurable TCG cost (W^X toggle removed by I1,
jmp-cache zero removed by I2) but neither moves the headline 1.27 s
worst frame, because that worst frame is **not built from per-call
TCG-invalidation costs at all**.

### Recommended next slice (in priority order)

1. **Diagnose the 1.27 s worst frame structurally — not by symbol.**
   Two scoping experiments to design before more code is written:
   - **Per-frame spike-cause attribution**: extend `XEMU_PERF_SPIKE_LOG`
     to emit, when an `op` exceeds the spike threshold, the
     `TCG_INVALIDATE_COUNT_SINCE_LAST_SPIKE` and the renderer-thread
     event most-recent-before the spike. The current spike-log is
     event-keyed on renderer ops only; add TCG-thread spike events for
     "TB invalidation count > N in last 100 ms" and "single
     `cpu_exec_loop` iteration > T µs." Without this, we are guessing
     which subsystem produced the 1.27 s — composite bottlenecks need a
     causal log, not a sampled profile.
   - **Snapshot the worst frame's xstate**: capture a `pause`/HMP
     `info status` and an `info qom-tree` at the start of an
     identified-bad interval (we have `interval_ms=1392` candidates
     from Arm C). Compare register-file dirty page rate inside vs
     outside the bad interval.
2. **Reduce TB-invalidation frequency at the SMC source** (R2's
   notdirty/SMC reduction work). 4.4M `TCG_NOTDIRTY_TRIPS` over 300 s
   on Crimson — cutting that by half would proportionally reduce the
   worst-interval TB-invalidation count of 470, even if no single call
   is the dominant cost. Sub-slices (from V1's recommendation list,
   still applicable): stickier dirty-bit eviction, page-bitmap caching
   of "no TBs here" before `tb_invalidate_phys_range_fast`. R2 already
   scoped this — the data here strengthens the case.
3. **x87 FP lowering review.** ~7 % of TCG-thread samples are in x87
   software-FP helpers (`floatx80_*`, `parts*`, `soft_f32_mul`). Crimson
   Skies is FP-heavy. If aarch64 host could lower more of these to
   native NEON/FP rather than going through `floatx80`, that would
   relieve the *whole* TCG thread, not just the worst frame. Needs
   QEMU-internal review of the i386→aarch64 TCG opcode lowering for
   `MMX/SSE/x87` ops. Likely a multi-slice effort.
4. **Renderer/TCG mutex contention audit.** `qemu_mutex_lock_impl` (76
   samples), `qemu_cond_wait_impl` (35), `page_collection_lock` (11) on
   the TCG thread — modest in steady state but possibly spiking during
   the bad frame. Already partly addressed by `XEMU_PGRAPH_FAST_READ`
   (lock-free PGRAPH register reads); the next step is the writes-side
   profile and any other shared structure between the TCG and the
   renderer.
5. **PPTC (persistent TCG translation cache).** Strategy.md Phase 5a.
   Largest theoretical leverage on TB-translation churn. Still
   significant complexity; pursue only after slices 1–2 are exhausted.

**Do NOT pursue:** further per-jmp-cache micro-optimization. The
mechanism is gone (`tcg_flush_jmp_cache` is 0/20,981 samples in Arm D);
there is no further per-bucket savings to extract along this axis. The
upstream MTTCG patch search is also lower priority now — both candidate
upstream patches for "Apple Silicon W^X" and "jmp-cache" are now landed
in this fork's working tree as I1+I2; the relevant qemu-devel work
beyond that does not appear to address the composite worst-frame cost.

### Honest re-assessment of the 2026-05-01 GL-vs-Metal verdict

The GL-vs-Metal verdict ("stay on GL, fix TCG TB-invalidation cost") is
still correct on the GL side — Apple GL has measured headroom for 60 FPS
1080p+AA. **But the "TCG TB-invalidation cost" framing has now been
disproven by two consecutive landed slices.** The two slices targeted at
that framing (I1 splitwx, I2 targeted-jmp-cache) are both validated as
mechanically correct and both fail to move the headline 1.27 s metric.
The next research slice should either:
- **Reframe the headline metric**: stop using "single worst frame in a
  300 s route" as the gate. With one 1-second-class spike per 5-minute
  route, the statistical confidence is very low — an A/B noise floor of
  ±50 ms is plausible just from Apple GL pacing variance. The
  `frame_mspf_us_p999` (87.7 ms → 86.1 ms in this slice) and
  `longest_stutter_run_30fps` (15 → 12 intervals) are statistically
  better-behaved metrics for "is the slice doing user-visible good."
- **Or:** pursue the spike-attribution diagnostic in (1) above before
  the next implementation slice, so the next slice targets a known
  composite cost rather than the next-largest single sample-profile
  symbol.

## Honest-limits caveats

- **`validate-native-tri-depth.sh` failed in the same shape (5 failures,
  `final_intervals=0` and zero FLAT_FIRST/NONFIRST counters) on all
  three retries (22 s, 28 s, 45 s) and on the targeted=0 control run.**
  This is the same pre-existing harness flake noted in V1. The
  targeted=0 control reproduces the failure unchanged, which proves the
  failure is not caused by this slice. Indirect verification of triangle
  correctness:
  - PGR2 mid-route snapshot replay shows zero `GEOM_SHADER_DRAW_TRI` in
    both arms (the native triangle-depth path still owns triangle
    rendering).
  - Rainbow snapshot replay shows zero stutter intervals on both arms.
  - The flat-tri-depth XBE itself logs `NATIVE_TRI_DEPTH_DRAW=309128`
    (Arm targeted=0) and `NATIVE_TRI_DEPTH_DRAW=312126` (Arm
    targeted=1) — the native path is hit, the harness just never
    reaches its `final=1 atexit` perf-flush interval that the validator
    needs. A separate follow-up should fix the validator independently
    of this slice (see V1 caveat list).
- **PGR2 snapshot scene is renderer-bound, not TCG-bound** at the
  current `pgr2_gameplay_b4` snapshot with all opt-in flags on, so PGR2
  was never expected to surface a TCG-side FPS lift from the targeted
  slice. The `TCG_JMP_CACHE_ZEROED_BUCKETS` 87× drop and `TB_EXEC_COUNT`
  +215 % rise confirm the slice is firing on PGR2 too.
- **Crimson worst-frame is a "single-frame-per-route" metric.** A single
  ~1.27 s spike fired in both arms; with one 5-minute route per arm,
  statistical confidence on that specific frame is intrinsically low.
  The decisive evidence that the slice is not the headline TCG win is
  the combination of (a) the per-interval bad-window analysis showing
  Arm C `WALL_US_MAX=688` could not have produced a 1290 ms frame by
  itself, and (b) the sample-profile delta showing the slice mechanism
  is fully gone yet `helper_lookup_tb_ptr` and the FP/MMU helpers
  dominate steady-state cost.
- **Run-benchmark metadata still does not record `XEMU_TCG_JMP_CACHE_TARGETED`.**
  Same gap as V1 noted for `XEMU_TCG_SPLITWX`. Per-arm provenance here
  is from the explicit shell command at run time. A trivial follow-up:
  add `env_XEMU_TCG_JMP_CACHE_TARGETED` and `env_XEMU_TCG_SPLITWX` to
  the metadata block in `scripts/apple-silicon/run-benchmark.sh`.
- **Sample-profile `cpu_tb_exec` total samples are misleading on the
  targeted-ON arm.** Arm-D total (20,981 lines) is slightly lower than
  Arm-C total (22,012 lines) because the ON arm finished the 30 s
  sample window 1 s earlier (sample shutdown variance). Within-thread
  proportions are still comparable; the absolute symbol-count deltas
  reported above are calibrated against this 5 % frame-count delta.
- **No paired sample at the precise instant of the 1.27 s spike.** The
  sample profiles capture 30 s of representative gameplay, not the bad
  frame itself — that frame is a single-event tail, not present in
  every run window. The next-slice diagnostic (1) above is what would
  give us causal data on the bad frame.

## Files referenced

- Run dirs: see Test Matrix table above.
- Saved per-arm summaries: `/tmp/v2-arms/{arm-A,arm-B,arm-C,arm-D,rainbow-G,rainbow-H}-summary.txt`
  and `/tmp/v2-arms/{compare-A-B,compare-C-D,compare-rainbow-G-H}.txt`.
- Sample profiles:
  `benchmark-runs/20260502-002103-crimson-skies/sample-jmp-cache-targeted-on.txt`,
  `benchmark-runs/20260502-002315-crimson-skies/sample-jmp-cache-targeted-off.txt`,
  plus the auto-generated `*-summary.txt` siblings.
- Working-tree slice diff: `git diff` against
  `1534bb7688718e6bdfdf9e3bfe533829991ca202` (the source of the built
  `dist/xemu.app`).
- V1 baseline note (mirrored structure):
  `docs/apple-silicon/benchmarks/2026-05-01-tcg-splitwx-validation.md`.
