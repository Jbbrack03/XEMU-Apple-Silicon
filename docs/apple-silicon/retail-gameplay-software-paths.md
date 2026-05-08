# Retail Gameplay Oracle: Software-Only Input Paths

Status: 2026-05-08

This note records the definitive state of software-only control for retail
Xbox games launched by the oracle. It exists to prevent a false-positive
"gameplay oracle ready" verdict when the agent can launch and capture video
but cannot yet drive the title-facing controller path.

## Current Answer

There is no **generic** production-ready software path to control an arbitrary
retail game after `runxbe` today.

For the current oracle scope, the accepted production path is now
**per-title XBE patching** for the fixed 5-6 canary games. This is not a
universal controller backend, but it is sufficient for the immediate goal:
drive PGR2, Crimson Skies, Rainbow Six 3, Soul Calibur 2, Halo CE, and one
sixth broader-sweep title on real hardware with captured gameplay and an
autonomous dashboard return. The durable plan is
`retail-title-patching-strategy.md`.

The previously preferred resident kernel shim remains documented, but the live
`KeRaiseIrqlToDpcLevel` export-slot implementation crashed the project Xbox at
the jump-only rung. It is no longer the next production path.

`retail-oracle-smoke.py` and `retail-gameplay-oracle.py` must still gate runs
by evidence. A patched title qualifies only after it proves both title-facing
input and autonomous dashboard return.

## Evidence

- `controller-roundtrip` proves the agent can allocate a persistent
  controller buffer and a cooperating diagnostic XBE can read it after
  chainload.
- `controller-readback` proves a downstream title-facing controller stack can
  see a real controller after chainload.
- `oracle-client.py runxbe` kills the oracle agent process. Therefore
  Mac-side `controller.set`, `controller.button`, and `controller-replay.py`
  cannot keep driving input once a retail title is running.
- Per-title XBE patches avoid that lifecycle problem by moving route playback
  into the running title itself.
- Live kernel export annotation shows useful primitives for a resident
  implementation: `PsCreateSystemThread`, timer/DPC exports, interrupt
  exports, `IofCallDriver`, and `IofCompleteRequest`.
- The kernel also exports `HalReturnToFirmware`. A resident shim may be able
  to return to the dashboard directly with `HalQuickRebootRoutine` /
  `HalRebootRoutine` instead of synthesizing the IGR combo, but this has the
  same prerequisite: code that survives the retail title launch.
- The running kernel export table does not expose a clean public XInput/XID
  function to hook by name.
- `scripts/apple-silicon/tier2-shim-analyze.py` matched this console's live
  kernel exports to NKPatcher's `patcher_5838` IGR recipe. That is strong
  prior art for observing retail-game controller state through a kernel
  export-slot hook.
- The 2026-05-08 live Tier-2 install attempt froze/crashed the Xbox even with
  a jump-only export-slot redirection. That moves the resident hook path out
  of production and into research.

## Software Path Matrix

| Path | Verdict | Why |
| --- | --- | --- |
| Agent TCP `controller.*` after launch | Ruled out | The agent is the launched XBE; `runxbe` replaces it. There is no RPC server left while the retail game runs. |
| LaunchData-only preload | Ruled out for retail games | `XLaunchXBEEx` can pass launch data, but retail titles do not know our format and will not read the persistent script buffer. |
| One generic title-level XInput patch | Not production-generic | Retail titles statically link different XAPI builds and often do not expose stable `XInput*` strings or symbols. |
| Per-title XBE patching | Adopted for current scope | Acceptable because the oracle needs a fixed 5-6 title set. Each title gets a reproducible patcher, route playback, and an autonomous exit proof. |
| Resident kernel-level XID/XInput shim | Research only | It can sit below the title in theory, but the live export-slot implementation crashed the project Xbox. Do not use Tier-2 install commands for production. |
| Hardware controller emulator | Fallback | OGX360 / microcontroller paths remain viable if per-title patching stalls or broader generic coverage becomes necessary. |

## Local Title Scan

`scripts/apple-silicon/xbe-inspect.py` was added so this evidence can be
reproduced.

```bash
scripts/apple-silicon/xbe-inspect.py /tmp/xemu-title-scan/halo/default.xbe --strings 40
scripts/apple-silicon/xbe-inspect.py /tmp/xemu-title-scan/soul-calibur-2/Default.xbe --strings 40
scripts/apple-silicon/xbe-inspect.py /tmp/xemu-title-scan/outrun2/default.xbe --strings 40
```

Findings from the local XISOs:

- Halo exposes helpful strings such as `XInputOpen (gamepad) failed` and
  `XGetState (gamepad) failed`.
- Soul Calibur 2 and OutRun 2 link XAPILIB but do not expose comparable
  stable XInput call strings.
- All three have different library mixes and build paths. This supports the
  conclusion that one generic title patch is not enough; per-title patchers
  must verify each target title fingerprint and pattern independently.

## Required Production Proof

The per-title software path is complete for a title only after this exact proof
passes:

1. Preserve the original XBE and generate a reproducible patched XBE from a
   fingerprinted input.
2. Prove a return-only patch first: launch the patched title, wait a short
   fixed interval, call `HalReturnToFirmware(HalQuickRebootRoutine)` or
   `HalReturnToFirmware(HalRebootRoutine)`, and require dashboard FTP recovery.
3. Prove one visible synthetic input event through the patched title's normal
   game/menu path.
4. Run the full route with composite A/V capture.
5. Provide `title-facing-input-evidence` and `autonomous-exit-evidence` JSON to
   `retail-gameplay-oracle.py`.
6. Save the run artifacts and patch fingerprint under
   `benchmark-runs/retail-title-patch-<title>-<UTC>/`.

Until steps 2 and 3 pass for a title, the answer to "can the oracle control
this launched retail game through software?" is: not yet.

## Most Likely Implementation

The next implementation is a title-specific Mac-side patcher:

- Inspect and fingerprint the original XBE.
- Locate per-title input-read call sites or XAPI patterns.
- Inject a route player or redirect input reads to a route player.
- Feed that route player from a compact version of the existing xemu CSV.
- Call `HalReturnToFirmware` after route completion plus capture tail.

Do not rely on a title-level synthetic IGR button combo as the primary exit
path. A title patch can make the game see those buttons without necessarily
making the kernel/dashboard IGR hook see a physical controller report.
