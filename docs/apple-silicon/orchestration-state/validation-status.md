# Validation Status

- Active slice: cycle 17 — `XEMU_DIAG_PGRAPH_STATUS_DRAIN` (CLOSED 2026-05-22).
- Validation state: **CLOSED — every required gate green.**

## Required gates for this slice — all met

- [x] Fresh worker receipt posted after canonical-doc read and reflected in `current-cycle.md` plus `claude-status.md`.
- [x] Bounded implementation attempt kept tight to the documented PFIFO/PGRAPH busy-publication hypothesis (`hw/xbox/nv2a/pgraph/pgraph.c` + `nv2a_regs.h`; harness `XBE_HARNESS_TIMEOUT_SECONDS` env-var added for the long-latency diag case).
- [x] Concrete validation evidence captured through the host-visible guest-log channel on Metal (4 boots, `pass=8/8 mask=0xff`) and GL (15 boots, `pass=8/8 mask=0xff`).
- [x] Mandatory Codex validation completed (`/codex-validate changes`, verdict MINOR ISSUES, both findings adopted; marker recorded at `.claude/state/codex-validate-last-run`).
- [x] Canonical docs synced before closure (`handoff.md`, `decision-log.md`, `automation.md`, `xbe-harness/README.md`, all orchestration-state files, `.claude/rules/flags-renderer.md` + `flags-bench.md`).

## Cycle-17 evidence index

| Leg | Flag | Tally | Run dir |
|---|---|---|---|
| Metal | ON  | `pass=8/8 mask=0xff` × 4 boots  | `benchmark-runs/cycle17-status-drain-metal-PASS-gl-timeout-20260522/image-blit/metal/xemu.log` |
| GL    | ON  | `pass=8/8 mask=0xff` × 15 boots | `benchmark-runs/cycle17-status-drain-gl-long-timeout-20260522/image-blit/gl/xemu.log` |
| Metal | OFF | `pass=3/8 mask=0x31` × 2 boots  | `benchmark-runs/cycle17-baseline-no-drain-metal-20260522/image-blit/metal/xemu.log` |
| GL    | OFF | `pass=3/8 mask=0x31` × 2 boots  | `benchmark-runs/cycle17-baseline-gl-only-20260522/image-blit/gl/xemu.log` |

Two failed-iteration evidence dirs preserved for the eligibility-gate lesson (see `decision-log.md` cycle-17 entry):

- `benchmark-runs/cycle17-status-drain-FIRST-ATTEMPT-too-aggressive-20260522/` — no gate, BIOS hangs.
- `benchmark-runs/cycle17-status-drain-2nd-attempt-also-hung-20260522/` — PUSH0/DMA_PUSH gates only, also hangs.

## Carry-forward context

- Cycle 15 established the reusable guest-log channel and confirmed the remaining image-blit residual is renderer-agnostic.
- Cycle 16 packaged that milestone cleanly.
- Cycle 17 (this slice) closes the cycle-13 race hypothesis empirically; cycle-11 follow-up item #2 is CLOSED.
- Cycle-11 follow-up item #3 (real-Xbox oracle parity check) is the next bounded slice and gates the default-on / long-term-fix decision.
