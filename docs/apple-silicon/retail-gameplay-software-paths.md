# Retail Gameplay Oracle: Software-Only Input Paths

Status: 2026-05-07

This note records the definitive state of software-only control for retail
Xbox games launched by the oracle. It exists to prevent a false-positive
"gameplay oracle ready" verdict when the agent can launch and capture video
but cannot yet drive the title-facing controller path.

## Current Answer

There is no production-ready software path to control a retail game after
`runxbe` today.

There is a plausible software path, but it is not the existing
`controller.*` RPC path. The viable route is a resident kernel-level shim that
survives `XLaunchXBE`, consumes the already-proven persistent controller
buffer, and injects synthetic state into the title-facing XInput/XID path.
The best-supported shim boundary is now the NKPatcher-style
`KeRaiseIrqlToDpcLevel` export-slot hook documented in
`tier2-kernel-shim-viability.md`.
Until that shim passes the readback proof below, the retail gameplay oracle
must remain gated by `retail-oracle-smoke.py` and
`retail-gameplay-oracle.py`.

## Evidence

- `controller-roundtrip` proves the agent can allocate a persistent
  controller buffer and a cooperating diagnostic XBE can read it after
  chainload.
- `controller-readback` proves a downstream title-facing controller stack can
  see a real controller after chainload.
- `oracle-client.py runxbe` kills the oracle agent process. Therefore
  Mac-side `controller.set`, `controller.button`, and `controller-replay.py`
  cannot keep driving input once a retail title is running.
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

## Software Path Matrix

| Path | Verdict | Why |
| --- | --- | --- |
| Agent TCP `controller.*` after launch | Ruled out | The agent is the launched XBE; `runxbe` replaces it. There is no RPC server left while the retail game runs. |
| LaunchData-only preload | Ruled out for retail games | `XLaunchXBEEx` can pass launch data, but retail titles do not know our format and will not read the persistent script buffer. |
| Generic title-level XInput patch | Not production-generic | Retail titles statically link different XAPI builds and often do not expose stable `XInput*` strings or symbols. A per-title/OOVPA patcher is possible research, but it is not an easy oracle primitive. |
| Resident kernel-level XID/XInput shim | Viable but unproven | It can sit below the title, survive title launch if code/data live in persistent memory, and target the 20-byte XID gamepad report / XInput state shape. It still needs a hardware validation run. |
| Hardware controller emulator | Deferred | User explicitly said hardware is unavailable for this phase. |

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
  conclusion that title patching is a research track, not the default oracle
  input backend.

## Required Production Proof

The software-only Tier 2 path is complete only after this exact proof passes:

1. Start the oracle agent and set a non-zero synthetic controller state in the
   persistent buffer.
2. Install a resident shim whose code and data are not owned by the agent XBE
   image after launch.
3. Chainload `controller-readback`.
4. `controller-readback.txt` must report the synthetic buttons/axes, not the
   physical controller's idle state.
5. Launch at least one retail game with a short scripted route.
6. Exit autonomously using either synthetic IGR
   `back + start + left trigger + right trigger` or a proven resident
   `HalReturnToFirmware`/dashboard-return path.
7. `retail-gameplay-oracle.py` must capture composite keyframes and return to
   dashboard FTP without manual intervention.

Until step 4 passes, the answer to "can the oracle control a launched retail
game through software?" is: not with the current shipped tooling.

## Most Likely Implementation

The most promising implementation is not to patch each game. It is:

- Persist a small executable resident blob with `MmAllocateContiguousMemoryEx`
  and `MmPersistContiguousMemory`.
- Keep route data in the existing persistent controller buffer, or extend that
  buffer with a compact timed-event script.
- Patch a kernel completion or input boundary from persistent code. The
  candidate boundary is `IofCompleteRequest`/XID report completion, because
  the XID gamepad input report is 20 bytes and maps directly into the original
  Xbox `XINPUT_GAMEPAD` layout.
- Make the shim self-uninstall or restore the patched bytes before reboot.
- Prefer a resident `HalReturnToFirmware(HalQuickRebootRoutine)` exit test
  before relying on the softmod IGR combo, because it avoids proving input
  injection and dashboard return in the same first experiment.
- Use the existing readback diagnostic as the first acceptance test.

This route is invasive, so it must remain gated behind an explicit evidence
artifact before any retail gameplay workflow treats it as production.
