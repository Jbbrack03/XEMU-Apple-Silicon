# Soul Calibur 2 Gameplay Route Capture + Audio Listen-Test

Date: 2026-05-05

## Purpose

Capture a repeatable Soul Calibur 2 gameplay route that reaches a real
in-game rendered state, unblocking SC2 as a Metal paired-diff visual
canary. SC2 was previously the only canary title lacking a recorded
route; the existing `noop.csv` reached only Xbox boot/flubber and SC2's
title screen, leaving Metal-vs-GL visual comparison with nothing to
diff against.

The same recording session also served as a (single-title) listen-test
verification for the `XEMU_APU_LOCK_RELEASE` slice (I5).

## Route

- Game: Soul Calibur II
- Disc: `/Users/jbbrack03/XEMU_MacOS/Test_Games/Soul Calibur 2.xiso.iso`
- HDD source: master HDD `Xbox-Emulator-Files/hdd/xbox_hdd.qcow2`
  (NOT profile-prepared — the route includes Xbox boot, dashboard
  bypass, and SC2 splash navigation)
- Recorded input script: `scripts/apple-silicon/input-scripts/sc2-gameplay.csv`
- Capture run: `benchmark-runs/20260505-163659-soul-calibur-2`
- Wall time: ~150 s (user-terminated after gameplay state reached;
  planned 360 s)
- Recorded events: 11,384 controller events plus header
- First input: at 21.5 s wall (Start press to bypass Xbox boot/SC2 splash)
- Renderer: GL (`XEMU_RENDERER` unset; surface_scale=2 = 1080p-class)

## Event distribution

| Control | Events | Note |
| --- | ---: | --- |
| `lstick_y` | 8,366 | character vertical movement |
| `lstick_x` | 2,530 | character horizontal movement |
| `rtrigger` | 172 | SC2 attack (right) |
| `rstick_y` | 103 | (incidental) |
| `rstick_x` | 44 | (incidental) |
| `ltrigger` | 43 | SC2 attack (left) |
| `y` / `x` / `b` / `a` | 102 | face-button attacks/blocks |
| `dpad_*` | 18 | menu cursor navigation |
| `start` | 6 | dashboard / SC2 splash / pause |

84 % of events are analog stick — the signature of active gameplay,
not menu idling.

## Performance observation (GL, surface_scale=2, active combat)

User-observed FPS during active 3D combat: **~15 FPS**, matching
captured perf intervals (last three intervals before user-termination:
`fps=20.61`, `fps=12.34`, `fps=10.51`). This is the **first SC2
combat-state FPS measurement on this fork**.

The prior reference value `post_load_avg_fps=57.63` from
`benchmark-runs/20260504-101242-soul-calibur-2` (used in 2026-05-02 to
prove SC2 is a 60Hz title) was almost certainly captured at the title
screen / character select / attract loop — much simpler than 3D combat
with two characters animating, particles, ring effects, and HUD. Active
combat at surface_scale=2 is approximately 4× heavier.

The 60Hz console-native target for SC2 combat is therefore not met by
the GL renderer on Apple Silicon at this resolution. This is exactly
the kind of real-route data point that motivates the Metal renderer
work; Apple's GL-on-Metal layer is deprecated and has known scaling
limits at higher resolutions and complex 3D scenes.

**Next**: replay this route under the canonical Metal recipe
(`XEMU_RENDERER=METAL XEMU_METAL_TRANSLATED_PIPELINE=1
XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1
XEMU_METAL_FRONT_FB_FALLBACK=1 XEMU_METAL_MSAA=4`) to obtain a paired
Metal-vs-GL FPS number and confirm correct rendering.

## Audio listen-test result (I5 / `XEMU_APU_LOCK_RELEASE`)

User completed an audio listen-test concurrent with the recording
session. Conditions:

- ~150 s of active gameplay reaching Arcade combat
- Audio paths exercised: announcer voice, character attack grunts /
  ring-out callouts, SC2 BGM, in-round impact SFX
- `XEMU_APU_LOCK_RELEASE` was at its Apple Silicon default (ON)

**Result: PASS.** No audio glitches, no stuck voices, no dropped SFX,
no audible clicks or pops. User reported clean playback for the full
duration.

**Decision (user-authoritative): close I5.** Per user's
2026-05-05 directive ("Option 1" in the listen-test rubric review),
the `XEMU_APU_LOCK_RELEASE` slice is declared **fully shipped** on
the basis of this single-title verification.

**Deviation from canonical rubric.** The original project rule (fork
CLAUDE.md, workspace CLAUDE.md, prior decision-log) specified Crimson
/ Rainbow / PGR2 ≥ 5 min each, because those titles are where the
2026-05-02 D3 attribution measured voice-lock contention (21.3 s /
300 s vCPU thread time blocked on `mcpx-apu-vp/0xfe8202fc =
NV1BA0_PIO_VOICE_LOCK`). SC2 was not part of that attribution; its
audio engine and contention pattern are distinct from the racing /
shooter titles. The deviation is accepted as a user-authoritative
decision; revisit if audio regressions surface later in the named
titles.

## Command (recording)

```sh
./scripts/apple-silicon/record-input.sh sc2 360 \
  ./scripts/apple-silicon/input-scripts/sc2-gameplay.csv
```

## Replay

```sh
./scripts/apple-silicon/run-benchmark.sh sc2 \
  ./scripts/apple-silicon/input-scripts/sc2-gameplay.csv 300
```

## Caveats

- **Master HDD bootstrap.** Unlike the PGR2 / Rainbow / Crimson routes
  which replay from a profile-prepared HDD, this route boots from the
  master HDD and walks Xbox dashboard → SC2 splash → main menu →
  Arcade → character select → in-round combat. Replay timing
  therefore depends on dashboard load timing being deterministic. If
  replay diverges, re-record against a profile-prepared HDD that
  auto-boots SC2.
- **F3 snapshot anchor still pending.** This route is sufficient for
  reaching gameplay but is wall-clock-aligned, not flip-stall-aligned.
  Per the 2026-05-05 paired-diff alignment limit (see `handoff.md`),
  the M15 paired-diff harness needs an `sc2-canary` snapshot recorded
  at a stable visual moment so both legs land on the same guest
  state. Next session: run this route under the canonical Metal
  recipe with `XEMU_BENCH_SAVEVM_AT=<seconds>
  XEMU_BENCH_SAVEVM_TAG=sc2-canary` to capture the snapshot, then
  validate same-renderer loadvm.
- **GL combat FPS reflects the recording session.** The 15 FPS figure
  is from active combat with input recording active. Recording
  overhead is minimal (CSV append on event change) and does not
  account for the magnitude of the gap; this is genuine GL-on-Metal
  scaling cost on a complex 3D scene at 1080p-class.
