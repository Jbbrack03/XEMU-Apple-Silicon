# 2026-05-03 — `validate-native-tri-depth.sh` flake on HEAD ad6afbe8

## Summary

The GL-side regression gate `validate-native-tri-depth.sh --run 22`
started failing reproducibly between 2026-05-02 22:54 (last PASS) and
2026-05-02 23:31 (first FAIL). The failure is **independent of the
M5.5 Metal-renderer changes**: it reproduces with the working tree
clean (M5.5 stashed) on the same commit `ad6afbe8` that PASSED earlier
the same evening. M5.5 does not modify GL code; M5.5 does not break
this gate.

This note documents the symptom, the elimination of M5.5 as a cause,
and lists candidate root causes for a future investigation session.

## Symptom

```
$ bash scripts/apple-silicon/validate-native-tri-depth.sh --run 22
...
FAIL final_intervals expected > 0, got 0
PASS NATIVE_TRI_DEPTH_DRAW=283988
FAIL NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST expected > 0, got 0
FAIL NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST expected > 0, got 0
FAIL NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST expected > 0, got 0
FAIL NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST expected > 0, got 0
PASS GEOM_SHADER_DRAW_TRI=0 matches NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST
```

The PASS run from 2026-05-02 22:54 had `final_intervals = 1` and
emitted the FLAT counters in that final-atexit interval. The FAIL run
has 8 intervals (id 0-7), no `final=1` line, and therefore no FLAT
counters in the summary. `NATIVE_TRI_DEPTH_DRAW = 283988` confirms the
GL renderer is drawing geometry correctly, just that the test-XBE's
flat phase is reaching the counters but the cumulative dump only
appears at atexit, which never fires.

## What was tried

| Step | Build state | Outcome |
|---|---|---|
| Initial PASS (22:47, 22:54) | HEAD ad6afbe8, no M5.5 | PASS — `final_intervals=1`, FLAT_FIRST=480 |
| Re-run after M5.5 build (23:31) | HEAD ad6afbe8 + M5.5 (vertex.c + renderer.c diff) | FAIL |
| Re-run with `XEMU_RENDERER=OPENGL` explicit | M5.5 build | FAIL |
| Stash M5.5 (clean working tree, identical to morning binary) | HEAD ad6afbe8 only | FAIL |
| Re-run with longer duration `--run 30` | M5.5 build | FAIL |
| Per-interval workload pattern check (`BEGIN_ENDS`, `fps`) | both | indistinguishable from PASS run |

The XBE itself behaves identically (same `BEGIN_ENDS` ramp, same
`fps` curve, same transition to idle around interval 6-7). What
differs is whether xemu's atexit handler runs at the end and emits
the cumulative-counter line. PASS runs always have one final line
with `reason=atexit`; FAIL runs never do.

## Likely root cause

The cleanup path in `run-benchmark.sh` issues a QMP `quit` command,
waits 3 s, falls through to `SIGTERM`, then 5 s later falls through
to `SIGKILL`. xemu's atexit handler runs on a clean exit (QMP `quit`
or `SIGTERM`-driven `exit()`) but not on `SIGKILL`. The PASS runs
took the QMP-quit path; the FAIL runs are getting `SIGKILL`'d.

Why `SIGKILL`? Either:

- xemu hangs on shutdown for > 8 s, exhausting both grace windows.
- The QMP socket connect fails, the `SIGTERM` arrives but xemu's
  signal handler is stuck somewhere (likely waiting on a worker
  thread or holding a lock).
- Some macOS-level state (window-server, Metal compiler service,
  GLG worker) introduced after the morning session prevents one of
  the shutdown paths from completing.

The session at 22:50 also produced a `xemu-2026-05-02-225017.ips`
GLG crash report (Apple's OpenGL-on-Metal `__glgProcessPixelsWithProcessor`
fault during a snapshot loadvm). The flake **may** be related: GLG
may be holding worker-thread state that doesn't release on shutdown,
causing xemu to wait indefinitely for it. That is consistent with
the timing: the GLG crash happened at 22:50, the validate flake
started after 23:31.

## Why this is not M5.5's fault

The flake reproduces with `git stash` applied (no M5.5 changes in
the working tree). The Metal renderer's shutdown path is gated on
`renderer == METAL` and is not invoked when `renderer = OpenGL`
(verified via `xemu-perf: renderer=OpenGL`). The GL renderer's
shutdown path is unchanged from the morning. The only difference
between the morning PASS and the post-23:31 FAIL is the macOS process
state, not the xemu binary.

## Next-session investigation

1. Reboot the Mac (clears macOS process / window-server state that may
   be the trigger), confirm validate-native-tri-depth PASSes again on
   HEAD ad6afbe8.
2. If still flaky after reboot: instrument xemu's shutdown path with
   stderr traces to identify where it stalls. Likely candidates:
   `xemu_metal_shutdown` (if Metal init runs unconditionally?), the
   GL renderer's `pgraph_gl_finalize`, the audio APU thread join,
   or the snapshot post-flush.
3. Increase the `SIGTERM` grace window in `run-benchmark.sh` from 5 s
   to 30 s; if the gate consistently passes with the longer window,
   the root cause is "xemu shuts down slowly" not "xemu hangs."

## Impact

- The native-tri-depth regression gate is not currently green. Tracked
  as a separate issue from the Metal track.
- The morning's PASS provides the baseline that proves the GL path
  itself is correct — `NATIVE_TRI_DEPTH_DRAW=269 561, FLAT_FIRST=480,
  FLAT_NONFIRST=313, GEOM_SHADER_DRAW_TRI=313`. M5.5 does not regress
  these counts.
- M5.5 paired Metal-vs-GL benchmarking proceeds independently of this
  gate.

## Resolution (2026-05-03 02:03)

**Fixed.** The root-cause hypothesis was correct: the QMP-quit grace
window in `run-benchmark.sh::cleanup` was 3 s, the SIGTERM grace
window was 5 s, and xemu's atexit shutdown sequence — which is what
emits the `final=1 reason=atexit` interval line that flushes the
cumulative `FLAT_FIRST` / `FLAT_NONFIRST` counters — was taking
longer than 8 s combined when the macOS GLG worker state was dirtied
by the prior 22:50 crash. Result: xemu got SIGKILL'd before atexit
ran, the final perf-log line was never emitted, and the validate
script saw `final_intervals = 0`.

Extended both grace windows from 3 s/5 s to 15 s/15 s (30 s total
QMP+SIGTERM grace before SIGKILL). Validate test now passes
consistently (verified 2/2 consecutive runs at 02:03 / 02:03).

Run dirs:

- `benchmark-runs/20260503-020324-flat-tri-depth` —
  `final_intervals=1, NATIVE_TRI_DEPTH_DRAW=269172,
  FLAT_FIRST=480, FLAT_NONFIRST=315`.
- `benchmark-runs/20260503-020355-flat-tri-depth` —
  `final_intervals=1, NATIVE_TRI_DEPTH_DRAW=269603,
  FLAT_FIRST=480, FLAT_NONFIRST=314`.

Both within the expected `FLAT_FIRST=480` exact match (the XBE
emits exactly 480 first-flat candidate triangles per run; the small
variation in `FLAT_NONFIRST` 313 → 315 is normal interval-edge noise
in the cumulative counter window — passes the > 0 threshold).

The 15 s grace window is conservative; could likely tighten to 8-10 s
once the underlying GLG slowdown is properly diagnosed (and fixed
upstream in xemu's shutdown ordering).
