# Validation Status

- Active slice: cycle 24 Path A.4 real-Xbox discriminator run for image-blit. Operationally a docs/evidence + binary-deploy slice (no xemu-fork host code edits, no XBE rebuilds — used cycle-23 binaries as-is).
- Validation state: **CLOSED with a CONCRETE BLOCKER outcome.**

## Gate status (cycle 24) — final

- [x] Fresh worker receipt posted before deeper work (21:30 CDT).
- [x] Xbox reachable at session start (`ping 192.168.0.200` 0% loss / ~0.5 ms RTT; `nc -z 192.168.0.200 9001` → port open).
- [x] Cycle-23 oracle-agent + image-blit binaries FTP-uploaded to `/E/Apps/oracle-agent/default.xbe` and `/E/Apps/image-blit/default.xbe` after rebooting Xbox to dashboard. Remote sizes confirmed via `LIST` (417 792 B + 159 744 B = local sizes).
- [x] Agent re-launched via `oracle-orchestrator.py ensure-agent` (`SITE EXEC` → `200 EXEC command succeeded`); `witness.scan` verb registered (`help | grep witness` returns the cycle-23 description).
- [x] Baseline `witness.scan` returned `count=1 mapped_pages_seen=419` with the single live buffer at phys=0x03eb3000 / virt=0x83eb3000 / reserved[0]=0 / reserved[1]=0 / magic XCTR / anchor_ok=1 — precondition MET; no power-cycle needed before chainload.
- [x] Chainload `runxbe E:\Apps\image-blit\default.xbe` issued at 2026-05-23T02:34:34Z. Chainload epoch recorded.
- [ ] **FTP-back / agent-restart NOT observed.** 928.3 s of continuous polling on FTP/21 + agent/9001 + ICMP ping — all silent. Measurement aborted at 2026-05-23T02:50:02Z. Cycle 19/20/21 reproducible 22.3..22.4 s chainload→FTP-back gap regressed to indefinite hang.
- [ ] **Post-chainload `witness.scan` UNRECOVERABLE.** Cannot read the persistent kernel-pool buffer without network reachability. The `MmPersistContiguousMemory`-tagged witness buffer survives soft reset but NOT power-off; the only recovery path (physical power-cycle by Hermes) erases the witness state. Cycle-24 A.4 byte is unrecoverable from this run.
- [x] Conservative interpretation recorded against cycle-23 semantics: cycle-22 leading hypothesis WEAKENED (not corroborated or invalidated); NEW hypothesis #5 (witness mechanism real-Xbox safety from non-agent process context) promoted as top-priority discriminator candidate for cycle 25.
- [x] Canonical docs synced (handoff.md cycle-24 entry on top; decision-log.md cycle-24 entry above cycle-23; orchestration-state quartet closure pass).
- [x] Evidence preserved on disk: `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/{01-deploy.log, 02-baseline-witness-scan.log, 03-chainload-image-blit.log}`.

## Codex validation decision

**Cycle 24 is a docs/evidence + binary-deploy slice. Skipped under rule #15's "doc-only changes" / "≤30-line uncommitted diff" carve-out.**

**Skip justification (final).** Cycle 24 ships:
1. Zero source/script code edits.
2. Zero XBE rebuilds.
3. Doc edits across `docs/apple-silicon/handoff.md`, `docs/apple-silicon/decision-log.md`, `docs/apple-silicon/orchestration-state/*`.
4. Three new evidence log files under `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/`.
5. Evidence-only operations on real Xbox: `reboot` (existing verb), FTP upload of cycle-23-built binaries (no rebuild), `ensure-agent` `SITE EXEC` (existing tool), `witness.scan` reads (read-only), `runxbe` chainload (existing verb), connectivity polling (`ping`, `nc -z`).

Aggregate diff is markdown + evidence-log-only. Validation marker NOT written. If cycle 25 implements the witness-only XBE (the recommended follow-up), Codex validation becomes mandatory before deploying.

## What stands from cycles 17 + 19 + 20 + 21 + 22 + 23

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle 19/20/21's 5× reproducible 22.4 s chainload→FTP-back gap for pre-witness image-blit binaries — UNCHANGED as a baseline. Cycle 24's regression vs that baseline is itself the central finding.
- Cycle 22 invalidation of cycle-19 hypothesis #1 (launch-path blocker) and cycle-21 hypothesis #3 (FATX-driver/NT-mount state divergence for D:\\) — unchanged.
- Cycle 23 closure (witness instrumentation Codex-validated round-2 PASS_WITH_FINDINGS + MINOR resolved post-round-2) — unchanged.

## What changes (cycle 24)

- Cycle-22 leading hypothesis WEAKENED (not invalidated; not corroborated). New hypothesis #5 (witness mechanism real-Xbox safety from non-agent process context) added.
- The 22.4 s chainload→FTP-back gap is no longer reproducible with the cycle-23 image-blit binary on this Xbox revision.
- M15 default-on shape for §H.6 is now blocked on cycle-25 witness-mechanism viability discrimination (was blocked on cycle-24 A.4 result; cycle 24 did not deliver a result).
