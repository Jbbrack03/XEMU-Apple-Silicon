# Validation Status

- Active slice: cycle 28 Path A.4 real-Xbox deployment of cycle-27 preserve-branch oracle-agent vs cycle-25 witness-only XBE. **Run-only / doc-only slice** — ZERO source/script code edits, ZERO XBE rebuilds.
- Validation state: **CLOSED. Outcome D-cycle-27 observed (`count=1 live=1 reserved0=0 reserved1=0` in final `witness.scan` after cycle-27 preserve-branch agent re-allocates the deterministic kernel-pool buffer phys=0x03eb3000). Codex validation skipped under rule #15 doc-only / run-only carve-out (same path as cycle 26).**

## Gate status (cycle 28) — final

- [x] Required docs read (handoff.md cycle-27/26 entries, orchestration-workflow.md, decision-log.md cycle-27/26 entries, orchestration-state quartet, oracle-and-xbe rule snapshot, oracle-orchestrator.py ensure_agent flow, cycle-26 SUMMARY.md).
- [x] Worker receipt posted to current-cycle.md + claude-status.md.
- [x] Xbox reachability + cycle-23 agent responsiveness probed (08:44:29Z).
- [x] Baseline `witness.scan` precondition MET (count=1 phys=0x03eb3000 reserved0=0 reserved1=0 mapped_pages_seen=419 — matches cycle 26 exactly).
- [x] `reboot` agent → dashboard FTP-LIST OK at t+27s.
- [x] FTP STOR cycle-27 `oracle-agent/bin/default.xbe` (417 792 B) → verified remote size.
- [x] Confirmed cycle-25 `witness-only/default.xbe` (147 456 B) still resident from cycle-26 deploy.
- [x] `ensure-agent` launches cycle-27 build; SITE EXEC OK; ready at port 9001 within ~10s; banner unchanged (`v0.4 (Phase 2 + controller.* + smc.*)` — expected, internal allocator logic only).
- [x] Post-launch `witness.scan` identical to baseline (kernel pool returned same persistent phys to cycle-27 agent; preserve gate saw `(0,0)` predicate match).
- [x] `runxbe E:\Apps\witness-only\default.xbe` via cycle-27 agent → agent died on chainload → FTP-LIST + 9001 + ICMP poll → **dashboard fully ready at t+70s** (reproduces cycle 26's 70.17s).
- [x] Post-run `ensure-agent` (cycle-27 build) + FINAL `witness.scan` = **count=1 buf.0 phys=0x03eb3000 reserved0=0 reserved1=0** = outcome D-cycle-27.
- [x] Evidence preserved on disk: 9 logs + SUMMARY.md under `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/`.
- [x] Canonical docs synced: handoff.md cycle-28 entry on top (cycle-27 preserved unchanged); decision-log.md cycle-28 entry above cycle-27 (no supersession); orchestration-state quartet closure pass (current-cycle.md, claude-status.md, validation-status.md = this file, handoff-summary.md).
- [x] Closure commit landed on `apple-silicon-performance`: `c77b509149`.
- [ ] **Cycle 29 design + run** — explicitly out of scope; Hermes-scheduled.

## Codex validation decision (cycle 28)

Cycle 28 is a **run-only / doc-only slice** — ZERO source/script code edits, ZERO XBE rebuilds, all Xbox-side operations used existing agent verbs and existing Mac-side tooling (`oracle-client.py`, `oracle-orchestrator.py`, `curl FTP`). Rule #15 trigger #2 (non-trivial uncommitted code in xemu-fork/) does NOT fire. The rule #15 doc-only / ≤30-line uncommitted source diff carve-out applies. **Skipped per the same path as cycle 26**, which also ran the canonical sequence with ZERO source edits.

## What stands from cycles 17 + 19 + 20 + 21 + 22 + 23 + 24 + 25 + 26 + 27

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle 19/20/21's 5× reproducible 22.4 s chainload→FTP-back gap for pre-witness image-blit binaries — UNCHANGED.
- Cycle 22 invalidation of cycle-19 hypothesis #1 and cycle-21 hypothesis #3 (FATX-driver/NT-mount state divergence for D:\\) — unchanged.
- Cycle 23 closure (witness instrumentation Codex round-2 PASS_WITH_FINDINGS + MINOR resolved) — unchanged.
- Cycle 24 concrete-blocker outcome (image-blit hard-hangs real Xbox; cycle-22 leading hypothesis WEAKENED; NEW hypothesis #5 promoted) — unchanged.
- Cycle 25 witness-only XBE SHIPPED + Codex round-3 PASS_WITH_FINDINGS — unchanged.
- Cycle 26 closure (outcome shape D = ~70 s recovery + no observable orphan; hypothesis #5 PARTIALLY INVALIDATED in catastrophic-hang sense; stamp-vs-no-stamp ambiguity OPEN) — UPDATED by cycle 28 (see below).
- Cycle 27 closure (`s_allocate_fresh` preserve branch SHIPPED with Codex 3-round green) — unchanged; preserve branch was correctly built and deployed (cycle 28 confirms wiring is correct, just had nothing to preserve).

## What changes (cycle 28)

- The cycle-26 indistinguishable-causes pair "stamp landed and got wiped vs stamp never landed" collapses on the wiped side. **Stamp-landed-then-wiped hypothesis INVALIDATED.**
- Cycle-26 ambiguity now resolved to **"stamp never landed"** with three live sub-causes (α/β/γ) recorded in handoff.md cycle-28 entry, SUMMARY.md, and the cycle-29 candidate scope discussion.
- Cycle-27 option (a) is **demonstrated insufficient** for breaking the cycle-26 ambiguity to A1/A2.
- 70 s chainload→dashboard-ready for witness-only is now **two-cycle-reproducible real Xbox behavior** (cycle 26 ~70.17 s + cycle 28 ~70 s).
- Kernel-pool deterministic phys=0x03eb3000 reuse is now **≥6 consecutive observations in same physical power session** (cycle 26's 3 + cycle 28's ≥3).
- Cycle-22 leading hypothesis status: UNCHANGED (still WEAKENED). Cycle 28 cannot move it.
- Hypothesis #5 status: **REFINED**. Catastrophic-hang sense remains PARTIALLY INVALIDATED; subtler "silently no-ops" sense now consistent with cycle-28 evidence but indistinguishable from (γ) without further instrumentation.
- §H.6 default-on shape decision REMAINS DEFERRED.
- M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-29 design + run), §G.5, RT-as-texture.

## What cycle 28 does NOT resolve

- **Why the stamp never landed.** Three live causes:
  - (α) `xbed_a4_witness::a4_candidate_ok` kseg0 scan doesn't find agent's XCTR buffer from non-agent process context.
  - (β) Scan finds it but write faults silently (PAT/WC/WB attribute divergence or stale cache line).
  - (γ) Witness-only's main() never reaches the fire calls (consistent with cycle-22 leading hypothesis).
- **The ~70 s recovery shape.** Could be slow kseg0 scan, delayed-fault watchdog window, or slow BIOS POST. Cycle 29 may collapse this ambiguity as a side-effect if option (c)'s self-allocated-page approach shows BIOS-POST-like timing (~17 s) instead of 70 s.
- **Cycle-22 leading hypothesis.** Still WEAKENED; cycle 29 option (c) or (d) is the path forward.
