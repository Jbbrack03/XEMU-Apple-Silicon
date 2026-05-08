# Retail Title Patching Strategy

Last updated: 2026-05-08.
Status: **ADOPTED for the current retail-game oracle scope.**

## Decision

For the current retail-game oracle, use **per-title XBE patching** for the
project's 5-6 tracked games instead of continuing the live Tier-2 kernel hook
or waiting on a hardware controller emulator.

This is an intentional scope trade. A generic controller backend is still the
better long-term product, but the oracle's immediate goal is narrower: drive a
fixed set of high-value retail games on the real Xbox, capture gameplay, return
to dashboard autonomously, and compare those frames/audio/timing against xemu.

The Tier-2 `KeRaiseIrqlToDpcLevel` export-slot hook is not a production path
after the 2026-05-08 live crash. Hardware controller emulation remains a good
fallback, especially with OGX360 hardware available, but per-title patching is
now the next production direction because it keeps the work in software and is
acceptable for the fixed title set.

## Target Titles

Patch the known canary titles first:

| Priority | Title | Why it matters | Existing route status |
| --- | --- | --- | --- |
| 1 | Project Gotham Racing 2 | Front-buffer / render-target behavior, racing workload, streaming. | `pgr2-gameplay.csv` exists. |
| 2 | Crimson Skies | Heavy gameplay, audio/CPU/streaming stutter class. | `crimson-gameplay.csv` exists. |
| 3 | Rainbow Six 3 | Tactical FPS path, movement-triggered gameplay coverage. | `rainbow-gameplay.csv` exists. |
| 4 | Soul Calibur 2 | 60 Hz combat, fast animation, fight timing. | `sc2-gameplay.csv` exists. |
| 5 | Halo CE | Mainstream XAPI/XInput stack and iconic renderer workload. | Route still needs recording or patch-assisted bootstrap. |
| 6 | OutRun 2 or Burnout 3 | 60 Hz racing / broader-sweep coverage. | Choose after the first four patches prove the workflow. |

If these games run flawlessly against real-Xbox captures, the confidence level
for mainstream compatibility is high, but not universal. The diagnostic-XBE
suite remains the feature-surface oracle for long-tail NV2A behavior that the
canary titles do not exercise.

## Patch Model

Each patched title should be produced by a reproducible Mac-side patcher, not by
manual hex editing. The patcher should:

1. Verify the input XBE title/certificate/build fingerprint before writing.
2. Locate title-specific XInput/XAPI call sites or stable function patterns.
3. Inject a small script-player thunk or patch existing input-read code to call
   it.
4. Feed controller state from a compact route buffer derived from the existing
   xemu CSV format.
5. Leave a documented fallback behavior when the route is exhausted or disabled.

The first implementation can embed the route in the patched XBE or in a
sidecar file staged next to it on the Xbox HDD. Sidecar loading is nicer for
iteration, but embedding is acceptable for the first proof if it reduces moving
parts.

## Autonomous Exit Requirement

Autonomous exit is part of the patch, not a nice-to-have. A title patch is not
accepted unless the Xbox returns to the dashboard without user input.

Primary exit path:

- The injected route engine calls the running kernel's
  `HalReturnToFirmware(HalQuickRebootRoutine)` or
  `HalReturnToFirmware(HalRebootRoutine)` after `route_done_ms +
  capture_tail_ms`.

Fallback exit path:

- If direct `HalReturnToFirmware` is unsafe in a title, patch the title to call
  a known dashboard-return path through its linked XAPI, or use a title-specific
  reset path with the same proof requirement.

Do not rely on appending the softmod IGR combo to a title-level XInput patch as
the primary exit path. A title-level patch can make the game see those buttons,
but it does not necessarily make the kernel/dashboard IGR layer see a physical
USB report.

## Acceptance Proof

For each title:

1. Preserve the original XBE and generate a reproducible patched XBE.
2. Run a **return-only patch** first: launch patched title, wait a short fixed
   interval, call the exit path, and require dashboard FTP recovery.
3. Run an **input-read proof** second: inject a tiny deterministic input sequence
   that visibly changes menu/game state or writes a title-specific marker.
4. Run the full route with composite A/V capture.
5. Require `retail-oracle-smoke.py` / `retail-gameplay-oracle.py` evidence for
   both `title-facing-input` and `autonomous-exit`.
6. Save artifacts under `benchmark-runs/retail-title-patch-<title>-<UTC>/`.

## Next Session Checklist

1. Power-cycle/recover the Xbox if it is still down; verify dashboard, FTP, and
   oracle-agent reachability. Upload only the guarded agent and run read-only
   Tier-2 preflight if needed for status; do not run Tier-2 install commands.
2. Start with PGR2 because it already has a route and is a high-value renderer
   canary.
3. Mirror the title XBE to the Mac, run `xbe-inspect.py`, and record the
   fingerprint in a benchmark note.
4. Build the first per-title patcher for PGR2.
5. Prove autonomous dashboard return before any gameplay route.
6. Prove one visible input event.
7. Run the full PGR2 route through `retail-gameplay-oracle.py`.
8. Repeat the same ladder for Crimson Skies, Rainbow Six 3, Soul Calibur 2,
   Halo CE, then the chosen sixth title.
