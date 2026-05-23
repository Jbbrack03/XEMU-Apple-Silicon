# Validation Status

- Active slice: cycle 26 Path A.4 real-Xbox witness-only deployment. Operationally a real-Xbox evidence-gathering slice (no source/script code edits, no XBE rebuilds; doc + evidence-file edits only).
- Validation state: **CLOSED. Codex validation skipped under rule #15's "doc-only / ≤30-line uncommitted diff" carve-out** (zero source/script edits this session; cycle-23/24/25 binaries unchanged; only orchestration-state quartet + handoff.md + decision-log + new evidence files added).

## Gate status (cycle 26) — final

- [x] Required docs read (handoff.md cycle-25 + cycle-24 + cycle-23 entries, orchestration-workflow.md, decision-log entries, current-cycle/claude-status/validation-status/handoff-summary at cycle-25 close, oracle-and-xbe rule snapshot).
- [x] Worker receipt posted to current-cycle.md + claude-status.md (this update).
- [x] Xbox reachability + precondition verified: 1 live `oracle_ctrl_buffer` at phys=0x03eb3000 reserved[0]==0 mapped_pages_seen=419.
- [x] Cycle-25-shipped witness-only XBE FTP-uploaded to /E/Apps/witness-only/default.xbe (147 456 B verified).
- [x] Canonical cycle-26 sequence executed: ensure-agent → baseline scan → reboot → FTP upload → ensure-agent → baseline rescan → runxbe witness-only → poll → ensure-agent → post-run scan. Repeated 4× plus 2 controls.
- [x] Ground-truth timing established via `curl FTP LIST` poll (the `nc -z -w 1` early-positive artifacts were misleading; FTP LIST issues a real protocol exchange).
- [x] Post-run witness.scan after every chainload: identical to baseline (count=1, phys=0x03eb3000, reserved[0]=0, mapped_pages_seen=419).
- [x] Controls (invalid-path → 20.67 s; mirror → ~36 s) confirm witness-only's ~70 s is a distinct outcome from both XLaunchXBE-failure and known-good chainload paths.
- [x] Evidence preserved on disk: `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/{00..20-*.log, SUMMARY.md}` (gitignored per project convention).
- [x] Canonical docs synced: handoff.md cycle-26 entry on top (cycle-25 entry preserved unchanged); decision-log.md cycle-26 entry above cycle-25 (no supersession); orchestration-state quartet closure pass.
- [ ] Closure commit on `apple-silicon-performance` (pending this session's commit).
- [ ] **Cycle-27 stamp-vs-no-stamp discriminator redesign** — explicitly out of scope; Hermes-scheduled.

## Codex validation decision (cycle 26)

Cycle 26 is **evidence-gathering only**. Zero source/script code edits in this session. Cycle-23 oracle-agent binary, cycle-23 lib/xbed_a4_witness.{c,h}, cycle-25 witness-only XBE — all unchanged. All Xbox-side operations used existing agent verbs and existing Mac-side tooling (`oracle-orchestrator.py ensure-agent`, `oracle-client.py raw witness.scan`, `oracle-client.py runxbe`, `curl FTP`). Edits this session: orchestration-state quartet (current-cycle.md, claude-status.md, validation-status.md=this file, handoff-summary.md), handoff.md cycle-26 entry, decision-log.md cycle-26 entry, plus new evidence files under `benchmark-runs/` (gitignored).

Per rule #15, doc-only + ≤30-line uncommitted source diff qualifies for the trivial-work skip. No validation marker written.

## What stands from cycles 17 + 19 + 20 + 21 + 22 + 23 + 24 + 25

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle 19/20/21's 5× reproducible 22.4 s chainload→FTP-back gap for pre-witness image-blit binaries — UNCHANGED.
- Cycle 22 invalidation of cycle-19 hypothesis #1 (launch-path blocker) and cycle-21 hypothesis #3 (FATX-driver/NT-mount state divergence for D:\\) — unchanged.
- Cycle 23 closure (witness instrumentation Codex-validated round-2 PASS_WITH_FINDINGS + MINOR resolved post-round-2) — unchanged.
- Cycle 24 concrete-blocker outcome (cycle-23 image-blit hard-hangs real Xbox; cycle-22 leading hypothesis WEAKENED; NEW hypothesis #5 promoted) — unchanged.
- Cycle 25 witness-only XBE SHIPPED + Codex round-3 PASS_WITH_FINDINGS + local xemu-Metal smoke green — unchanged.

## What changes (cycle 26)

- Hypothesis #5 is **PARTIALLY INVALIDATED in the catastrophic-hang sense**. Witness-only chainload does NOT hard-hang the Xbox (recovered fully in 70 s without physical power cycle, vs cycle-24's 928 s+ silent).
- Cycle-25 discriminator outcome table did NOT anticipate the observed shape (clean recovery + no orphan). NEW outcome shape D documented: ~70 s recovery, no orphan observable from session evidence.
- Cycle-22 leading hypothesis status: still WEAKENED (cycle 26 partial discriminator does not resolve it).
- Cycle-26 evidence REVEALS a cycle-25/26 design gap: the agent's `s_allocate_fresh::memset(vp, 0, ...)` + the kernel pool's deterministic same-phys reuse silently wipes any witness stamp on every agent re-launch. The orphan-survival assumption embedded in the cycle-25 design table is invalid on this Xbox.
- §H.6 default-on shape decision REMAINS DEFERRED.
- M15 overall still NOT MET pending §H.6 default-on shape (still blocked on cycle-27 stamp-vs-no-stamp discriminator), §G.5, RT-as-texture.

## What cycle 26 does NOT resolve

- **Stamp-vs-no-stamp question.** Whether witness-only's `xbed_a4_witness_fire` actually landed a stamp before the new agent's memset wiped it is undetermined. Cycle 27 must redesign to break this ambiguity (4 candidate approaches in current-cycle.md "Recommended cycle-27 candidate scope").
- **70 s delay question.** Whether the ~70 s recovery time reflects a slow kseg0 scan, a delayed-fault watchdog window, or a slow BIOS POST cycle is undetermined. Cycle 27 should add an on-screen visual breadcrumb or host-log channel readback to discriminate.
