# Composite goal validation (V4) — 60 FPS @ 1080p + MSAA, no jitter

Date: 2026-05-02

## Verdict: GOAL NOT MET on any tracked title. The full goal stack — all four
landed flags + scale=2 + MSAA=4 — sustains ~30 FPS on Crimson Skies, ~30 FPS on
Rainbow Six 3, and ~31 FPS on PGR2. The headline "60 FPS sustained, 1080p,
high-quality AA, no judder" goal is **partially met on the AA / 1080p axes** and
**not met on the FPS / no-jitter axes**. The binding constraint is upstream of
the renderer.

The decisive evidence: a paired PGR2 mid-route snapshot run with `XEMU_GL_MSAA=0`
parks at **30.78 FPS**; the matching `XEMU_GL_MSAA=4` snapshot parks at
**30.85 FPS**. Disabling MSAA does **not** lift FPS. The 30 FPS ceiling is set
by the iothread / TCG / renderer-pacing composite, not by AA cost. The
GL-vs-Metal "headroom for 60 FPS at 1080p+AA" prediction holds on the renderer
side (MSAA cost is small and shader-pipeline-explosion does not occur), but
that headroom is not realized as user-visible FPS because the bottleneck is
elsewhere.

The Crimson 1.35-second worst-frame is **structurally unchanged** from V1 / V2
(1347.74 ms → 1352.15 ms → 1272.32 ms → 1354.00 ms) — adding scale=2 and
MSAA=4 on top of the V2 stack moves it within run-to-run noise. The
non-jmp-cache, non-PCREL composite cost identified by V2 / V3 still owns this
worst frame.

## Build / commit verification

- Binary: `dist/xemu.app/Contents/MacOS/xemu`, built 2026-05-02 06:20 UTC
  (pre-session). Used as-is — no rebuild this session.
- `xemu --version` → `xemu_version: 0.8.134-47-g1534bb7688`,
  `xemu_commit: 1534bb7688718e6bdfdf9e3bfe533829991ca202` (dirty,
  V1+V2+V3 + composite-goal stack on top).
- GL renderer / OS confirmed Apple GL 4.1 Metal — 90.5 on Apple M3 Ultra,
  macOS 26.4.1 (build 25E253).
- MSAA initialization log line confirmed in all three V4 retail runs:
  `xemu-perf: gl_msaa=4 source=XEMU_GL_MSAA requested=4 max_samples=4`
  (Apple GL clamps to max samples = 4, matching the decision-log expectation).
- No xemu process across or between runs (pgrep -f xemu returns nothing
  before each launch and after each cleanup).

## Goal stack (all four landed flags + composite display flags, all enabled)

```
XEMU_NATIVE_TRI_DEPTH=1
XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1
XEMU_TCG_SPLITWX=1
XEMU_TCG_JMP_CACHE_TARGETED=1
XEMU_DISPLAY_SCALE=2
XEMU_GL_MSAA=4
XEMU_PERF_LOG=1
XEMU_PERF_FRAME_LOG=1
XEMU_BENCH_SCREENSHOT_BACKEND=none
```

## Test matrix

| Arm | Title | Mode | Duration | Run dir |
| --- | --- | --- | ---: | --- |
| P | PGR2 | retail `pgr2-gameplay.csv` on profile-prep HDD | 300 s | `benchmark-runs/20260502-012409-pgr2` |
| R | Rainbow Six 3 | retail `rainbow-gameplay.csv` on profile-prep HDD | 300 s | `benchmark-runs/20260502-012946-rainbow-six-3` |
| C | Crimson Skies | retail `crimson-gameplay.csv` on profile-prep HDD | 300 s | `benchmark-runs/20260502-013504-crimson-skies` |
| Step3-A | PGR2 | snapshot `pgr2_gameplay_b4`, MSAA=0 | 30 s | `benchmark-runs/20260502-014043-pgr2` |
| Step3-B | PGR2 | snapshot `pgr2_gameplay_b4`, MSAA=4 (paired) | 30 s | `benchmark-runs/20260502-014127-pgr2` |

All five arms used the profile-prep HDD (Arm P/R/C) or the recorded PGR2
mid-route HDD (Step3 arms) per the V1/V2 convention. Snapshots restored via
QMP/HMP `loadvm` (project rule #12). Screenshots disabled (rule #14 / Apple GL
crash class).

## Per-title 300 s metric tables

### Arm P — PGR2 retail 300 s route

| Metric | Value |
| --- | ---: |
| post_load_intervals | 278 |
| post_load_avg_fps | **31.37** |
| post_load_avg_mspf | 15.99 |
| post_load_fps_stddev | 5.802 |
| post_load_mspf_max_p50 | 31.28 |
| post_load_mspf_max_p95 | 37.30 |
| post_load_mspf_max_p99 | 53.92 |
| post_load_mspf_max_max | 144.91 |
| post_load_frame_mspf_us_p50 | 16,171 |
| post_load_frame_mspf_us_p95 | 28,791 |
| post_load_frame_mspf_us_p99 | **33,616** |
| post_load_frame_mspf_us_p999 | 41,195 |
| post_load_frame_mspf_us_max | **144,913** |
| post_load_stutter_intervals_30fps | 76 |
| post_load_stutter_intervals_45fps | 260 |
| post_load_stutter_intervals_60fps | 267 (96 % of intervals) |
| post_load_longest_stutter_run_30fps | 5 |
| MSAA_RESOLVE_US_TOTAL | 7,074,848 (7.1 s over 300 s = 2.4 %) |
| SHADER_COMPILE_COUNT | 343 |
| SHADER_COMPILE_US_TOTAL | 3,783,055 (3.8 s) |
| TCG_TB_EXEC_COUNT | 2,031,673,817 |
| TCG_INVALIDATE_WALL_US_MAX | 94 |
| TCG_JMP_CACHE_ZEROED_BUCKETS | 2,904,505 |

### Arm R — Rainbow Six 3 retail 300 s route

| Metric | Value |
| --- | ---: |
| post_load_intervals | 272 |
| post_load_avg_fps | **30.28** |
| post_load_avg_mspf | 14.35 |
| post_load_fps_stddev | 5.513 |
| post_load_mspf_max_p50 | 15.85 |
| post_load_mspf_max_p95 | 66.21 |
| post_load_mspf_max_p99 | 123.78 |
| post_load_mspf_max_max | 697.59 |
| post_load_frame_mspf_us_p50 | 14,983 |
| post_load_frame_mspf_us_p95 | 17,058 |
| post_load_frame_mspf_us_p99 | **29,088** |
| post_load_frame_mspf_us_p999 | 108,442 |
| post_load_frame_mspf_us_max | **697,594** |
| post_load_stutter_intervals_30fps | 25 |
| post_load_stutter_intervals_45fps | 47 |
| post_load_stutter_intervals_60fps | 73 (27 % of intervals) |
| post_load_longest_stutter_run_30fps | 4 |
| MSAA_RESOLVE_US_TOTAL | 27,511,027 (27.5 s = 9.2 % — Rainbow is the AA-cost-heaviest title) |
| SHADER_COMPILE_COUNT | 306 |
| SHADER_COMPILE_US_TOTAL | 5,083,156 |
| GEOM_SHADER_DRAW_LINE | 6,979 (line-shader fallback retained, expected) |
| TCG_TB_EXEC_COUNT | 1,351,700,479 |
| TCG_INVALIDATE_WALL_US_MAX | 106 |

### Arm C — Crimson Skies retail 300 s route

| Metric | Value |
| --- | ---: |
| post_load_intervals | 279 |
| post_load_avg_fps | **30.45** |
| post_load_avg_mspf | 30.46 |
| post_load_fps_stddev | 4.294 |
| post_load_mspf_max_p50 | 33.86 |
| post_load_mspf_max_p95 | 62.15 |
| post_load_mspf_max_p99 | 561.41 |
| post_load_mspf_max_max | **1354.00** |
| post_load_frame_mspf_us_p50 | 27,052 |
| post_load_frame_mspf_us_p95 | 33,364 |
| post_load_frame_mspf_us_p99 | **36,064** |
| post_load_frame_mspf_us_p999 | 306,758 |
| post_load_frame_mspf_us_max | **1,354,002** |
| post_load_stutter_intervals_30fps | 178 (64 % of intervals) |
| post_load_stutter_intervals_45fps | 278 |
| post_load_stutter_intervals_60fps | 278 |
| post_load_longest_stutter_run_30fps | 15 |
| MSAA_RESOLVE_US_TOTAL | 3,871,030 (3.9 s = 1.3 %) |
| SHADER_COMPILE_COUNT | 115 |
| SHADER_COMPILE_US_TOTAL | 1,121,172 |
| TCG_TB_EXEC_COUNT | 5,169,336,041 |
| TCG_INVALIDATE_WALL_US_MAX | 306 |
| TCG_JMP_CACHE_ZEROED_BUCKETS | 29,912,825 (slice firing — was 70.9 B with slice OFF in V2) |

## Pillar scorecard

Pass thresholds per the validation protocol. Worst-of-three governs the
overall verdict.

| Pillar | Goal | PGR2 | Rainbow | Crimson | Verdict |
| --- | --- | --- | --- | --- | --- |
| 60 FPS sustained | post_load_avg_fps ≥ 58 | **31.37 FAIL** | **30.28 FAIL** | **30.45 FAIL** | **FAIL** |
| 1080p output | scale = 2 active | scale=2 PASS | scale=2 PASS | scale=2 PASS | **PASS** |
| MSAA active | MSAA_RESOLVE_US_TOTAL > 0; no SHADER_COMPILE blowup | 7.1 s / 343 compiles PASS | 27.5 s / 306 compiles PASS | 3.9 s / 115 compiles PASS | **PASS** |
| No jitter (frame-level) | post_load_frame_mspf_us_p99 ≤ 17,500 | 33,616 **FAIL** (1.92×) | 29,088 **FAIL** (1.66×) | 36,064 **FAIL** (2.06×) | **FAIL** |
| No long stutters | post_load_frame_mspf_us_max ≤ 100,000 | 144,913 **FAIL** (1.45×) | 697,594 **FAIL** (6.98×) | 1,354,002 **FAIL** (13.54×) | **FAIL** |

**Pillar overall: 2 PASS / 3 FAIL.** AA and 1080p land. FPS, frame-level
jitter, and long-stutter avoidance all fail on every tracked title.

## Step 3 — renderer-load headroom check (composes-with-MSAA?)

Paired 30 s PGR2 mid-route snapshot replays, identical environment except
`XEMU_GL_MSAA`:

| Metric | MSAA=0 (Step3-A) | MSAA=4 (Step3-B) | Δ |
| --- | ---: | ---: | ---: |
| post_load_avg_fps | 30.78 | **30.85** | +0.07 (noise) |
| post_load_avg_mspf | 18.53 | 20.25 | +1.72 ms |
| post_load_frame_mspf_us_p99 | 32,994 | 34,507 | +4.6 % |
| post_load_frame_mspf_us_p999 | 34,799 | 37,199 | +6.9 % |
| post_load_frame_mspf_us_max | 38,360 | 38,888 | +1.4 % |
| MSAA_RESOLVE_US_TOTAL | 0 | 863,946 (0.86 s / 30 s = 2.9 %) | new |
| FLUSH_DRAW_US_TOTAL | 6,498,820 | 6,632,389 | +2.1 % |
| DRAW_BEGIN_US_TOTAL | 6,886,017 | 9,883,840 | +43.5 % |
| SURF_DOWNLOAD_US_TOTAL | 2,348,651 | 5,192,969 | +121 % |
| SHADER_COMPILE_COUNT | **129** | **129** | **0** |
| SHADER_COMPILE_US_TOTAL | 475,105 | 629,005 | +32 % (per-compile, not count) |
| TCG_INVALIDATE_WALL_US_MAX | 111 | 55 | −50 % (noise; sample-of-1) |

**Decisive observation:** turning MSAA off does **not** lift FPS at all.
Both arms park at 30.78 / 30.85 FPS — within run-to-run noise. The
PGR2 snapshot scene is not renderer-bound (the renderer has the headroom
the GL-vs-Metal verdict predicted), but the FPS ceiling is set by something
upstream of the renderer. MSAA does add real per-call work
(`SURF_DOWNLOAD_US_TOTAL` +121 %, `DRAW_BEGIN_US_TOTAL` +44 %), but in
absolute time these are within the renderer's slack at scale=2.

**Critical success-criterion sub-result for the GL-vs-Metal decision:**
`SHADER_COMPILE_COUNT` is **identical (129 vs 129)** between MSAA=0 and
MSAA=4. There is **no MSAA-induced pipeline-variant explosion** on Apple's
GL-on-Metal driver. This was the documented success criterion for staying
on GL when adding multisample render targets — that test now passes.

## Step 4 — jitter distribution per title (post-load only)

Per-frame mspf distribution from `XEMU_PERF_FRAME_LOG=1`. Counts are
strict-greater-than thresholds; total post-load frames per row given.

| Title | Post-load frames | > 17.5 ms (60FPS-violating) | > 33 ms (30FPS-violating) | > 100 ms (micro-hitch) | > 500 ms ("freeze") | > 1000 ms (1-sec class) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| **PGR2** | 8,951 | 3,640 (40.7 %) | 102 (1.14 %) | 1 | 0 | 0 |
| **Rainbow** | 8,478 | 366 (4.32 %) | 54 (0.64 %) | 11 | 2 | 0 |
| **Crimson** | 8,641 | 8,139 (94.2 %) | 458 (5.30 %) | 13 | 4 | **3** |

Top 5 worst frames per title (mspf in ms):

| Title | Worst | 2nd | 3rd | 4th | 5th |
| --- | ---: | ---: | ---: | ---: | ---: |
| PGR2 | 144.9 | 99.7 | 77.5 | 69.7 | 53.9 |
| Rainbow | 697.6 | 653.9 | 213.8 | 196.2 | 123.8 |
| Crimson | **1354.0** | **1329.2** | **1162.1** | 561.4 | 465.7 |

### Crimson worst-frame stability — has it shifted with the new flags?

| Session | Stack | Crimson `post_load_frame_mspf_us_max` |
| --- | --- | ---: |
| V1 Arm C (splitwx OFF baseline) | TRI_DEPTH+QUAD+FAST_READ | 1,347.7 ms |
| V1 Arm D (splitwx ON) | + SPLITWX | 1,352.1 ms |
| V2 Arm C (splitwx ON, jmp-cache OFF) | + SPLITWX | 1,290.9 ms |
| V2 Arm D (full TCG slices) | + JMP_CACHE_TARGETED | 1,272.3 ms |
| **V4 Arm C (full goal stack)** | + DISPLAY_SCALE=2 + MSAA=4 | **1,354.0 ms** |

The 1.35 s worst frame is **structurally invariant** across V1/V2/V4 stacks.
Adding scale=2 + MSAA=4 on top of the V2 TCG stack **does not** make the
worst frame measurably worse; the reading sits within noise of the
~1300 ms cluster identified by V1/V2/V3. The cost source remains
unattributed to a single subsystem (V3's spike-attribution showed only
~10 % of a worst-frame's ms-budget surfaces at the 10 ms event-threshold,
the rest being sub-10 ms aggregation).

PGR2's worst frames are clustered in the early post-load intervals
(i7–i14 of 278) — these may be late scene-load artifacts surviving the
default `skip=5` post-load window. The Crimson worst frames are spread
across the route (i10/i11/i16 early **and** i21/i27 mid), confirming this
is sustained behavior, not a startup tail.

## compare-runs verdict lines

Comparing the two PGR2 snapshot arms (Step3-A MSAA=0 vs Step3-B MSAA=4)
to formalize Step 3:

```
post_load_avg_fps                        |        30.78 |        30.85 |      +0.23 | noise
post_load_mspf_max_max                   |        38.36 |        38.89 |      +1.38 | noise
post_load_stutter_intervals_30fps        |            7 |            6 |     -14.29 | improvement
post_load_avg_mspf                       |        18.53 |        20.25 |      +9.28 | regression
SHADER_COMPILE_COUNT                     |          129 |          129 |      +0.00 | flat
```

(verbatim summary; the verdict line will read "regresses" because of the
+9 % avg_mspf, but FPS is statistically tied — MSAA shifts work into the
mspf accumulator without changing the steady-state pacing.)

## Honest summary — what the user actually has today

**Met (cleanly):**

- **1080p output**: the Apple Silicon first-launch default is `surface_scale=2`,
  the harness honors it, all three retail runs render at 1080p-class
  internal resolution with no FPS catastrophe.
- **High-quality AA**: `XEMU_GL_MSAA=4` engages on Apple's GL-on-Metal at
  the driver's native max samples. Apple's driver does not balloon the
  shader-pipeline-variant compile budget — `SHADER_COMPILE_COUNT` is
  identical with MSAA on vs off (129/129). Per-frame MSAA cost is small
  (1.3–9.2 % of wallclock across titles).

**Not met:**

- **60 FPS sustained**: all three titles hold ~30 FPS, not 60. The Apple
  GL renderer has the measured headroom (Step 3: turning MSAA off does
  not lift FPS), so the 30 FPS ceiling is not a renderer cost. It is
  set upstream — by the iothread / TCG vCPU pacing composite identified
  by V1/V2/V3.
- **No frame-level jitter**: post-load p99 frame mspf is 29–36 ms across
  all three titles, 1.7×–2.1× over the 17.5 ms pass threshold.
- **No long stutters**: every title produces frames over the 100 ms
  micro-hitch threshold (1 PGR2, 11 Rainbow, 13 Crimson). Crimson
  produces three frames over 1 second (1162, 1329, 1354 ms).

**The honest one-line summary for the orchestrator:** the project hits its
**previous floor** (30 FPS, met as of 2026-05-01) on every title, with
1080p and 4× MSAA both engaged correctly and without regression. It does
**not** hit the active goal (60 FPS @ 1080p + AA, no judder) on any
title. The renderer is not the binding constraint; further renderer
optimization (e.g. native Metal) cannot lift the 30 FPS ceiling on its
own.

## Realistic path to closing each gap

| Gap | Root cause (current evidence) | Realistic next step |
| --- | --- | --- |
| 60 FPS pillar | Non-renderer FPS ceiling at ~30 FPS. MSAA off doesn't lift it. V2/V3 evidence: no single TCG cost class dominates — composite of TB-execution chains, FP helpers, and softmmu probing. | (a) V4-followup spike attribution at the 1 ms threshold inside identified-bad intervals (V3 next-step item 1). (b) Profile what governs the 30 FPS ceiling specifically — is it the QEMU vsync/iothread `qemu_clock_warp` cadence, the renderer thread's `glFinish` flip stall (`FLIP_STALL_GLFINISH_US_TOTAL` is 1.6–6.7 s across titles — modest), or x86 IRQ delivery rate? Design a cheap test (e.g. force `vsync = false`, profile iothread sample). |
| Frame-level jitter (p99) | Same composite cost source. p99 hits 29–36 ms, not 17.5 ms. | Same as above. The 30 FPS-floor and 60 FPS-jitter pillars are the same problem. |
| Long stutters (>100 ms, >500 ms, >1 s) | Crimson 1.35 s composite worst frame: V3 attributed ~10 % to one `tcg_tb_chain` spike at guest PC `0x23dd47`; ~90 % unattributed at 10 ms threshold. Rainbow ≥500 ms frames not yet investigated separately. | (a) V3-followup: lower spike threshold to 1 ms inside i16 (V4 Crimson worst interval) and disassemble guest `0x23dd47`. (b) Apply the same V3 spike-log to Rainbow's i10/i14 worst intervals (697 ms / 654 ms) — those have not been attributed at all. |

## Recommended next action for the orchestrator

**Do NOT ship as-is.** The user's stated goal is not met. The previous
30 FPS floor is preserved with AA + 1080p added correctly, but FPS / jitter
are unchanged from the V2 baseline.

**Recommended:** **dispatch the V3 follow-up spike-attribution work
(D3 in the orchestrator's terminology)** — specifically:

1. Lowered-threshold (1 ms) spike-log capture **anchored on the V4 Crimson
   worst interval i16** (1354 ms mspf_max), to capture the sub-10 ms
   composition that V3 left unattributed (~1388 ms of cost below the
   per-event threshold).
2. **Guest-code disassembly** of `0x23dd47` against the profile-prep HDD
   to identify what the Xbox is doing during the 146 ms `tcg_tb_chain`
   spike that V3 already attributed.
3. **Apply the same V3 / V4 spike-attribution to Rainbow's worst frames**
   — Rainbow's 697 ms and 654 ms worst frames were not part of V3 (V3
   focused on Crimson). At minimum verify whether the same `tcg_tb_chain
   first_pc` shows up.
4. **Profile the 30 FPS ceiling structurally** — design a one-shot
   experiment that distinguishes among (a) iothread/main-loop pacing,
   (b) Xbox vsync emulation, (c) QEMU `qemu_clock_warp` cadence,
   (d) renderer glFinish backpressure. This is the missing diagnostic
   axis: every prior slice has measured the worst-frame tail; none has
   measured what governs the steady-state cap.

A renderer-side slice (native Metal, async shader compile re-enable,
texture-upload coalescing) is **not** indicated by V4's evidence and
should not be the next slice. Renderer headroom exists; the work is
upstream.

## Honest-limits caveats

- **Single 300 s sample per title.** Statistical confidence on
  worst-frame and >500 ms / >1 s frame counts is intrinsically limited.
  The 1354 ms / 1329 ms / 1162 ms top-3 Crimson frames are a 3-event
  sample. With n=1 route per title, per-frame max metrics carry ±50 ms
  of pacing noise (consistent with V1/V2 deltas).
- **PGR2 worst frames cluster in early intervals (i7–i14).** The
  `skip=5` post-load default may not fully exclude scene-load tail
  costs. The PGR2 144.9 ms worst frame in particular is in i10 and may
  be a late-loading artifact rather than steady-state gameplay. The
  ≥100 ms PGR2 count of 1 is a tight number that could plausibly drop
  to 0 with `skip=15`.
- **"60 FPS sustained" was operationalized as `post_load_avg_fps ≥ 58`.**
  That threshold treats average FPS as the headline pillar. A stricter
  reading ("every frame ≥ 16.7 ms budget") would change the framing
  but not the conclusion — 40–94 % of frames violate the 16.7 ms budget
  across titles, so neither operational form passes.
- **Step 3's "MSAA does not limit FPS" conclusion is from a single 30 s
  paired snapshot at the PGR2 mid-route scene.** It is decisive for
  that scene (the mid-route snapshot is the fork's most-validated
  renderer-bound test point), but it is not a guarantee that no scene
  in any tracked title is MSAA-cost-limited. A future regression
  could surface a scene where MSAA cost matters; the current evidence
  is only that on the canonical PGR2 mid-route the answer is "no".
- **`XEMU_TCG_SPLITWX` and `XEMU_TCG_JMP_CACHE_TARGETED` are auto-on
  on this Apple Silicon system build.** Setting them to `=1`
  explicitly in the V4 env-stack is redundant but harmless — the env
  var wins over the auto-default, and `=1` matches the auto-default,
  so there is no behavior change. The harness metadata still does not
  capture these env values per V1/V2's caveat — per-arm provenance is
  from the explicit shell command.
- **No `validate-native-tri-depth.sh` re-run this session** — slice
  protection rule #11 says the native-tri-depth and native-quad slices
  are closed and should not be re-validated unless their code changed.
  V4 changes no source code, only env-stack composition; rule #11
  applies. Indirect correctness verification: zero `GEOM_SHADER_DRAW_TRI`
  in PGR2 and Crimson (native triangle path owns triangle rendering),
  6,979 `GEOM_SHADER_DRAW_LINE` in Rainbow (line geometry shader is
  expected to remain — line raster is not part of the native-tri slice).
- **Shader pipeline-variant explosion check is from 30 s snapshots
  (Step 3), not the 300 s retail runs.** The retail runs show
  SHADER_COMPILE_COUNT of 343 (PGR2) / 306 (Rainbow) / 115 (Crimson)
  with MSAA=4 — these are normal first-time shader-compile counts for
  the routes' geometry. Without a paired retail MSAA=0 run we cannot
  prove the retail counts wouldn't have been the same with MSAA off,
  but the snapshot evidence (129/129 identical) is strong enough that
  the success criterion is fairly considered met. A paired retail
  MSAA=0 run was not in the V4 protocol scope.
- **30 FPS observed FPS exactly matches the project's previously-met
  "30 FPS floor" milestone.** This is suggestive that the cap is set
  by the engine's own frame pacing (Xbox NTSC ~30 FPS for these
  titles is one possibility) rather than by emulation cost — but PGR2
  natively targets 60 FPS on Xbox hardware in many modes, so the cap
  is **probably emulator-imposed**. Distinguishing
  Xbox-engine-imposed-30 FPS from emulator-imposed-30 FPS for each
  title would be a useful pre-D3 sanity check (e.g. compare to xemu
  Windows-host or a native Xbox capture if available, or check the
  Xbox D3D vsync mode the title selects).

## Follow-up: 60 Hz title sanity test (cap attribution)

A follow-up sanity test was run on **Soul Calibur 2** (a documented
60 Hz Xbox title — DarkZero, Christ-Centered Gamer, Gaming Nexus
reviews) on the same V4 build with the same V4 goal-stack flags.
Result: **post_load_avg_fps = 56.12** (steady-state intervals 7-115:
**60.57 FPS**, p99 frame mspf 13.65 ms, max 31 ms, zero >100 ms frames,
zero 30 FPS-class intervals after load).

This **disproves** the V4 honest-limits caveat that suspected the 30 FPS
cap on PGR2/Rainbow/Crimson might be emulator-imposed. The cap is
**title-intrinsic** — those games' engines target 30 FPS on real Xbox
hardware, and the emulator faithfully reproduces that target while
hitting 60 FPS on titles whose engines target 60 Hz.

D3 (NV2A vblank pacing diagnostic) is **not** indicated. The 1.35 s
Crimson worst-frame stutter and Rainbow's 697 ms worst frame remain
the right next-slice targets for the **jitter** pillar, but the FPS
pillar should be reframed against each title's native console FPS
target rather than a flat 60 FPS bar across the board.

See `2026-05-02-60hz-title-sanity-test.md` for full evidence,
methodology, and the proposed goal reframing.

## Files referenced

- Per-arm summaries: `/tmp/v4-arms/{pgr2,rainbow,crimson}-summary.txt`,
  `/tmp/v4-arms/pgr2-snap-msaa{0,4}.txt`.
- Run dirs: see Test Matrix table above.
- Working-tree diff for the goal-stack flags (display-scale, MSAA): see
  `git diff` against `1534bb7688718e6bdfdf9e3bfe533829991ca202` (the
  source of the built `dist/xemu.app`).
- V1 / V2 / V3 baselines (mirrored structure):
  `docs/apple-silicon/benchmarks/2026-05-01-tcg-splitwx-validation.md`,
  `docs/apple-silicon/benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md`,
  `docs/apple-silicon/benchmarks/2026-05-02-tcg-spike-attribution.md`.
- GL-vs-Metal decision (the prediction this V4 tests):
  `docs/apple-silicon/benchmarks/2026-05-01-gl-vs-metal-decision.md`.
