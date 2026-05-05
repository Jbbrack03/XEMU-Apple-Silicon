# 2026-05-05 — Crimson Skies Metal "blocker" was config, not renderer bug; harness fixes

## Summary

The handoff carried Crimson Skies as a Metal visual-route "BLOCKED" canary —
`benchmark-runs/20260504-100815-crimson-skies` produced one patterned-green
frame followed by black drawable screenshots. Today's investigation
demonstrated that the Crimson Metal renderer is **not** broken. Re-running
the same input script with the canonical M15 Metal recipe (specifically
`XEMU_METAL_FRONT_FB_FALLBACK=1`) produces correct rendering of the
tarot-card menu with sustained ~30 FPS for the full 90s benchmark.

Three orthogonal harness bugs were also fixed.

## Direct Crimson Metal correctness evidence

Run: `benchmark-runs/20260505-104139-crimson-skies` (90s, canonical recipe).

Recipe (env explicit):
```
XEMU_RENDERER=METAL
XEMU_METAL_TRANSLATED_PIPELINE=1
XEMU_NATIVE_TRI_DEPTH=1
XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1
XEMU_METAL_FRONT_FB_FALLBACK=1
XEMU_METAL_MSAA=4
```

Captured PNGs at frame 600 + every 300 submitted frames thereafter
(total 16 captures). Frames 0005 through 0016 each show the Crimson
Skies main menu (Justice / Wealth / Lovers / Death tarot cards on
parchment background) rendered correctly. Frame 0001 is black (the
script is still in pre-menu loading at frame 600 = ~10s in).

Counters (representative late interval id 82, fps=30.96 — last
non-`final=1` interval before atexit cleanup):
- `METAL_PIPELINE_TRANSLATED_FAILED = 0`
- `METAL_PIPELINE_FALLBACKS = 0`
- `METAL_DRAW_PASS_COALESCED = 95 / METAL_DRAW_COUNT = 416 = 22.8%`
  (M5.7 coalescing intact, well above 10% threshold)
- `METAL_FRONT_FB_PUBLISHES = 32` (publish path active, one per
  flip)

Sustained ~30.97 FPS across intervals 79-82 (matches Crimson Skies'
console-native 30 Hz rendering target). The sub-10 FPS interval 9
in earlier loading-screen phase is expected boot/load behavior, not
a steady-state metric.

PNGs available at `/tmp/crimson-canonical-fallback.0001.png` through
`.0016.png`.

## Why the previous test failed

The original failing run was launched without explicit
`XEMU_METAL_FRONT_FB_FALLBACK=1`. Crimson's CRTC publish target is
`vram_addr=0x32a4000` (640x480, format 8 = A8R8G8B8) for the menu, but
the engine abandons that surface mid-run. From ~30s onward Crimson
draws to `0x1c04000` (1280x480, format 4 = X8R8G8B8) and `0x1ad8000`
(640x480, format 4) instead — `metal_draw_target` counters confirm
`0x32a4000` receives 12-30 draws per interval while `0x2e06000`
receives thousands.

Without the fallback, Metal publishes the now-quiescent `0x32a4000` →
black drawable. With the fallback, Metal publishes the most-recent
selected color binding instead → real rendered scene reaches the
display.

This is the same architectural pattern handled by PGR2: the
front-fb-fallback path was the documented PGR2 fix, and Crimson is
analogous. The handoff's Crimson "BLOCKED" framing was a
miscategorization — the run that produced the black drawable did not
have the canonical recipe applied.

## Three harness bugs fixed in this session

### 1. `metal-gl-compare.sh` did not thread the canonical M15 Metal recipe

The W2 paired-diff harness invoked the Metal leg with only
`XEMU_RENDERER=METAL` + `XEMU_METAL_VALIDATION=1`, leaving
`FRONT_FB_FALLBACK` / `TRANSLATED_PIPELINE` / `MSAA` / `NATIVE_*` to
whatever the user's shell happened to have. The canary regression
gate `metal-canary-regress.sh` already hardcodes the full canonical
recipe; bringing parity to the paired-diff harness is the fix.

Pattern used (canonical default with user-env override):
```bash
[[ "${XEMU_METAL_TRANSLATED_PIPELINE+x}" != "x" ]] && \
    metal_extra_env+=("XEMU_METAL_TRANSLATED_PIPELINE=1")
[[ "${XEMU_METAL_FRONT_FB_FALLBACK+x}" != "x" ]] && \
    metal_extra_env+=("XEMU_METAL_FRONT_FB_FALLBACK=1")
[[ "${XEMU_METAL_MSAA+x}" != "x" ]] && \
    metal_extra_env+=("XEMU_METAL_MSAA=4")
```

GL leg gets `XEMU_GL_MSAA=4` by the same default-with-override pattern
so the AA edge classes are comparable between legs.

### 2. `metal-canary-regress.sh` parsed the atexit cleanup interval

The W3 counter-mode gate's `last_interval_counter()` walked every
`xemu-perf: interval_id=` line and returned the value from the last
match. xemu emits a degenerate final interval at process teardown
(`interval_id=N final=1 reason=atexit interval_ms=0 frames=1
fps=0.00`) which made the gate FAIL otherwise-healthy runs on
`fps==0.00 / draws==1 / publishes==0`. Rainbow's smoke run was hitting
this today — fps=6-7 throughout the run, fps=0.00 in the atexit
record.

Fix: skip lines containing `final=1` so the gate reads the last *real*
interval. Validated by re-running the gate end-to-end:

```
[pgr2][counters]    PASS draws=48   fps=23.86 publishes=24
[rainbow][counters] PASS draws=472  fps=7.00  publishes=8   coalesced=357
[halo][counters]    PASS draws=2325 fps=30.96 publishes=31  coalesced=2294
[boot][counters]    PASS draws=416  fps=30.99 publishes=32  coalesced=96
verdict: PASS
```

Run: `benchmark-runs/20260505-110002-canary-regress`.

### 3. `macos-capture.sh` Quartz cache + retry-with-backoff

GL-leg paired captures via `screencapture -l <wid>` were silently
falling back to full-desktop capture because `_load_quartz()` cached
its failed-import sentinel as `False`, then `find_window_id()`'s
`if Quartz is None: return None` did not match (`False is None` is
False), so the function tried to use the cached `False` as a Quartz
module on retries — `AttributeError: 'bool' object has no attribute
'kCGWindowListOptionOnScreenOnly'` crashed the capture script.

Two fixes:

- Reorder the cache check in `_load_quartz()` so `_quartz_module is
  False` is tested before `is not None`. Returns `None` correctly on
  cached failure.
- Add a retry-with-backoff loop in `capture_one()` (5 attempts,
  ~0.75s total) to absorb the brief window between xemu process
  start and AppKit window registration. The earlier "single-shot
  lookup" path could miss the window during cold-boot trigger fire.

These changes are correctness-on-paper; the local environment has
Quartz installed only for `/usr/bin/python3.9` (system Python), but
`macos-capture.sh` invokes `/opt/homebrew/bin/python3.14`. PEP 668
prevents pip-installing into the homebrew python globally; a venv
or pinning the script to `python3.9` would unblock window-targeted
capture locally. Documented as a known env limitation.

## Paired-diff cold-launch alignment is structurally limited

Two attempted runs of `metal-gl-compare.sh crimson` (ordinal 30
and ordinal 1500, with the canonical recipe + harness fixes
above) both returned FAIL verdicts not because of renderer
divergence but because:

- Ordinal 30 fires during Xbox boot before the GL window has
  drawn anything; GL leg captures the macOS desktop with a
  black xemu rect.
- Ordinal 1500 lands at different game states between legs:
  GL reaches Crimson's *Settings* submenu by 58.5s (xemu.log
  shows fps=58 on GL), Metal stays on the *card-fan main menu*
  at 60s+ (xemu.log shows ~30 fps on Metal). The input script
  is wall-clock-driven, so leg timing differences accumulate
  into different menu states by ordinal 1500.

The fix is snapshot loadvm anchoring (slice F3): record a
per-title snapshot at a stable visual state, plumb the existing
`--snapshot <tag> --loadvm-at <sec>` flags from
`metal-gl-compare.sh`, and capture immediately after loadvm
before either leg can drift. This is filed as a new slice (F3)
in the decision-log; recording the per-title snapshots is
interactive (user-blocking).

## Validation

- Build: not required (script-only changes).
- `bash -n scripts/apple-silicon/{metal-canary-regress,metal-gl-compare,macos-capture}.sh`: PASS.
- `metal-canary-regress.sh --mode counters`: 4/4 PASS.
- Direct Crimson Metal canonical-recipe run: 16/16 captured PNGs,
  frames 0005+ show menu rendering correctly.

## Next-session priorities (carry-over)

1. **F3 — per-title snapshot anchor** for paired diff (slice spec
   in decision-log).
2. **Record `sc2-gameplay.csv`** via `record-input.sh sc2`
   (interactive; user-blocking).
3. **Audio listen-test** for `XEMU_APU_LOCK_RELEASE`
   (interactive; user-blocking).
4. **M15 default-on visual-gate sweep** once F3 + SC2 land:
   PGR2 / Rainbow / Crimson / SC2 + one broader-sweep title via
   `metal-gl-compare.sh --snapshot <tag>` with ≤1% per-pixel diff.
5. **Front-fb fallback policy decision** — Crimson now joins PGR2
   as documented "PASS only with fallback ON" titles.
   Default-on flip is the simplest path; document the
   correctness caveat (the fallback is best-effort, not faithful
   CRTC publish).
