# Tier-2 Kernel Shim Viability

Status: 2026-05-07

## Verdict

Tier 2 is viable enough to pursue. The strongest path is not a blind OHCI/USB
probe. It is an NKPatcher-style export-slot hook at
`KeRaiseIrqlToDpcLevel`, adapted from IGR detection into synthetic
`XINPUT_STATE` override.

This decision is data-driven:

- NKPatcher is public prior art for in-memory Xbox kernel patching and IGR.
- The project Xbox's live kernel export dump exactly matches NKPatcher's
  `patcher_5838` IGR recipe.
- NKPatcher's IGR code hooks `KeRaiseIrqlToDpcLevel`, rewrites the return path,
  inspects the title-facing controller state, and can return to dashboard via
  `HalReturnToFirmware`.
- Our existing persistent controller buffer already survives `runxbe` and is
  read by cooperating diag XBEs.

## Prior Art Checked

### NKPatcher

Source family:

- `Rocky5/Xbox-Softmodding-Tool`
- commit inspected: `24973cb3e402d6c39bb4ce0f40c31aceeed0a6ff`
- Local reference checkout:
  `/tmp/xbox-tier2-refs/Rocky5-Xbox-Softmodding-Tool`
- Relevant source:
  `App Sources/NKPatcher/Softmod Save NKP11/nkpatcher.asm`

Findings:

- NKPatcher patches the running retail kernel in memory instead of replacing
  the whole BIOS.
- It supports retail kernel families through 5838.
- Its IGR feature hooks a kernel export boundary and detects gamepad state
  while retail games run.
- It uses `HalReturnToFirmware`, `LaunchDataPage`,
  `MmAllocateContiguousMemory`, and `MmPersistContiguousMemory` in ways that
  match our existing oracle primitives.

### ENDGAME

Source family:

- `XboxDev/endgame-exploit`
- commit inspected: `93e65366ce9666a8571fcbffdd23a26b55cc8976`
- Local reference checkout: `/tmp/xbox-tier2-refs-endgame`

Findings:

- Useful as shellcode engineering reference: position-independent code,
  dynamic export resolution, cache/TLB flush discipline, IRQL lowering, and
  `LaunchDataPage` + `HalReturnToFirmware` launch.
- It is not an input hook and does not patch the kernel for ongoing retail
  input control.

### XboxHD+ kpatch

Source family:

- `MakeMHz/xbox-hd-plus`
- commit inspected: `a984ab2f444f0aa5edf2b6313170403ac33d113e`
- Local reference checkout: `/tmp/xbox-tier2-refs-xhd`

Findings:

- Confirms modern production use of kernel patches on OG Xbox, including iND
  BIOS beta patch support and per-title compatibility handling.
- Less directly useful than NKPatcher because public artifacts are IPS patch
  data/readme rather than hook source for controller state.

## Analyzer Result

Repro command:

```sh
python3 scripts/apple-silicon/tier2-shim-analyze.py \
  --out-json benchmark-runs/tier2-shim-analysis-20260507T2310Z/summary.json \
  --out-md benchmark-runs/tier2-shim-analysis-20260507T2310Z/report.md
```

Result:

- `verdict=viable-prior-art-match`
- matched NKPatcher recipe: `patcher_5838`
- NKPatcher source line: `1512`
- `KeRaiseIrqlToDpcLevel` export-slot VA: `0x800104e8`
- expected current slot value before install: `0x00003d04`

Matched live exports:

| Export | Ordinal | VA |
| --- | ---: | --- |
| `KeRaiseIrqlToDpcLevel` | 129 | `0x80013d04` |
| `HalReturnToFirmware` | 49 | `0x8001542d` |
| `HalWriteSMBusValue` | 50 | `0x80014743` |
| `LaunchDataPage` | 164 | `0x8003c360` |
| `MmAllocateContiguousMemory` | 165 | `0x8001e4d3` |
| `MmPersistContiguousMemory` | 178 | `0x8001e021` |

Before any installer writes the hook, it must read `0x800104e8` and verify the
slot still contains little-endian `0x00003d04`. If not, another patch already
owns that export slot and Tier 2 must stop or chain explicitly.

Live preflight command:

```sh
python3 scripts/apple-silicon/tier2-shim-preflight.py \
  --analysis benchmark-runs/tier2-shim-analysis-20260507T2310Z/summary.json \
  --out benchmark-runs/tier2-shim-preflight/summary.json
```

`tier2-shim-preflight.py` is read-only. It does not call `unsafe-enable` and
does not write memory.

## Candidate Ranking

| Candidate | Confidence | Decision |
| --- | --- | --- |
| `KeRaiseIrqlToDpcLevel` export-slot hook | High | Primary path. Prior art on the same kernel family already uses it for retail-game controller IGR. |
| `IofCompleteRequest` / IRP completion hook | Medium | Keep as fallback if KeRaise path misses some titles; no call-context proof yet. |
| OHCI/XID internal routine hook | Medium-low | Closest to raw reports, but no exported symbol and higher crash risk. |
| Per-title `XInputGetState` patch | Low | Not production-generic; local XBE scans show inconsistent static XAPI layouts. |

## Proposed Hook Model

Use a resident blob allocated before retail launch:

1. Agent allocates persistent executable memory for the Tier-2 code/data.
2. Agent records the existing `KeRaiseIrqlToDpcLevel` export-slot value.
3. Agent writes the export-slot RVA for the resident hook into `0x800104e8`.
4. Hook mirrors NKPatcher's safe shape:
   - inspect caller/return context,
   - only act when the call looks like the XInput state path,
   - otherwise tail-jump to the original `KeRaiseIrqlToDpcLevel`.
5. First implementation only counts candidate hits and writes a ring buffer.
6. Later implementation rewrites `XINPUT_STATE` from `oracle_ctrl_buffer`.
7. Exit path uses either synthetic IGR or direct resident
   `HalReturnToFirmware(HalQuickRebootRoutine)`.

## Proof Ladder

Do not start with retail gameplay. The safe ladder is:

1. **Static match:** run `tier2-shim-analyze.py`; require
   `verdict=viable-prior-art-match`.
2. **Live preflight read:** verify `mem.read 0x800104e8 4` equals
   `04 3d 00 00`.
3. **Install no-op hook:** patch the export slot to a resident hook that only
   tail-jumps to the original and increments a counter.
4. **Diag readback:** chainload `controller-readback`; require no crash and a
   non-zero hook counter.
5. **Context capture:** log candidate return/state pointers while running
   `controller-readback`; do not mutate state yet.
6. **Synthetic override:** set a non-zero oracle controller buffer, mutate only
   the recognized `XINPUT_STATE`, and require `controller-readback.txt` to
   report the synthetic values.
7. **Autonomous return:** prove resident `HalReturnToFirmware` or synthetic
   IGR returns to dashboard.
8. **Retail smoke:** only then allow `retail-gameplay-oracle.py` to launch a
   retail game with Tier-2 evidence.

## Open Risks

- Some retail titles may not hit the NKPatcher-observed XInput path. The local
  title scan makes this unlikely for mainstream XAPI titles, but the proof must
  include at least Halo, Soul Calibur 2, and OutRun 2 before calling it broad.
- Export-slot ownership conflict is possible if another softmod/BIOS IGR patch
  has already hooked `KeRaiseIrqlToDpcLevel`. The preflight slot read catches
  this.
- The first mutating hook must understand the exact original-Xbox
  `XINPUT_STATE` shape, not Windows XInput. The original XDK state has
  `wButtons`, `bAnalogButtons[8]`, and four `SHORT` sticks.
- Cache/TLB/IRQL discipline matters. ENDGAME and NKPatcher both flush caches
  around self-modifying kernel code; Tier 2 should copy that discipline.

## Implementation Decision

Proceed with Tier 2, but only as an evidence-gated hook ladder. The next code
artifact should be a no-op/counter hook installer plus readback artifact, not a
full input override.
