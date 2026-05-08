# 2026-05-08 Tier-2 No-Op Hook Live Probe

## Goal

Test whether the software-only retail controller path can advance past the
first mutating proof rung: install a resident `KeRaiseIrqlToDpcLevel`
export-slot hook, then later use `controller-readback` to prove the hook
survives `runxbe` and runs while a title-facing input stack is active.

This is the prerequisite for booting a retail game, capturing gameplay, and
autonomously exiting through software controller automation.

## Preflight Result

Read-only analysis and live slot verification passed.

```sh
python3 scripts/apple-silicon/tier2-shim-analyze.py \
  --out-json benchmark-runs/tier2-shim-analysis-live/summary.json \
  --out-md benchmark-runs/tier2-shim-analysis-live/report.md

python3 scripts/apple-silicon/tier2-shim-preflight.py \
  --host 192.168.0.200 \
  --analysis benchmark-runs/tier2-shim-analysis-live/summary.json \
  --out benchmark-runs/tier2-shim-preflight-live/summary.json
```

Live oracle-agent RPC also returned:

```text
200 verdict=ok slot=0x800104e8 observed_rva=0x00003d04 expected_rva=0x00003d04
```

This confirms the project Xbox still matched the NKPatcher `patcher_5838`
candidate slot before any write.

## Install Attempts

First installer build allocated the resident page with
`PAGE_EXECUTE_READWRITE`. The agent rejected the attempt cleanly:

```text
failed to allocate/attach Tier-2 hook page
```

The export slot was not patched in that attempt.

Second installer build used the same `PAGE_READWRITE` allocation style as the
working persistent controller buffer. `tier2.install-noop` was run after
`unsafe.enable`. The RPC timed out before returning a response. Immediately
afterward the console was unreachable:

```json
{
  "host": "192.168.0.200",
  "ping": false,
  "ftp": false,
  "agent": false
}
```

`ping -c 2 -W 1000 192.168.0.200` also reported 100% packet loss.

## Current Verdict

The implemented NKPatcher-style counter hook is **not viable as tested**. It
failed before the `controller-readback` rung, so no retail game was launched
and no post-boot software controller automation was proven.

This does not prove every possible software-only controller path is impossible.
It does prove that the current export-slot no-op/counter implementation is not
production-safe and must not be treated as a working retail oracle backend.

## Follow-Up Hardening

After the crash, the local implementation was revised offline:

- Added `tier2.install-jump-only`, which installs only a tail-jump to the
  original `KeRaiseIrqlToDpcLevel` target.
- Revised `tier2.install-noop` to preserve EFLAGS and use a plain
  `inc dword ptr [hook_page.calls]` instead of `lock inc`.
- Extended `controller-readback` to report Tier-2 hook code size and flags.

Both revised XBEs built locally. After a manual power-cycle, the revised agent
was uploaded and launched. Read-only preflight again passed:

```text
200 verdict=ok slot=0x800104e8 observed_rva=0x00003d04 expected_rva=0x00003d04
```

The safer `tier2.install-jump-only` rung was then attempted after
`unsafe.enable`. It timed out before returning a response, and the Xbox again
became unreachable:

```json
{
  "host": "192.168.0.200",
  "ping": false,
  "ftp": false,
  "agent": false
}
```

That means the crash is not explained by the counter memory operation. The
resident export-slot redirection itself is unsafe in this implementation.

## Recovery Note

First action after power-cycling the Xbox:

```sh
ping -c 2 192.168.0.200
python3 scripts/apple-silicon/oracle-orchestrator.py --host 192.168.0.200 status
```

Only after the dashboard/FTP path is back should the revised agent be uploaded.
Run `tier2.preflight` only. The local rebuilt agent now guards the mutating
Tier-2 install commands behind the explicit argument
`confirm=crash-risk-20260508`; upload that guarded build after the next power
cycle before launching the agent. Do not use the guarded override for the
retail oracle pipeline.
