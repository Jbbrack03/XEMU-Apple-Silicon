# Current Cycle

- Cycle: 28 Path A.4 real-Xbox deployment of cycle-27 preserve-branch oracle-agent vs cycle-25 witness-only (**CLOSED — 2026-05-23**, outcome **D-cycle-27**: post-run `witness.scan` shows `count=1 live=1 reserved0=0 reserved1=0` after cycle-27 preserve-branch agent re-allocates the deterministic kernel-pool buffer phys=0x03eb3000; cycle-27 option (a) DEMONSTRATED INSUFFICIENT to break cycle-26 stamp-vs-no-stamp ambiguity to A1/A2; "stamp landed and got wiped" INVALIDATED; "stamp never landed" conclusion now isolated; cycle 29 promotes option (c) or (d)).
- Started: 2026-05-23 (Hermes-supervised bounded session; Claude Code worker autonomous run from this Mac).
- Closed: 2026-05-23 (this commit).
- State: **CLOSED. Canonical cycle-26-style sequence executed against cycle-27 oracle-agent; 70 s dashboard-recovery shape REPRODUCED (matches cycle 26); kernel-pool deterministic phys=0x03eb3000 reuse REPRODUCED (≥6 consecutive observations in same power session); FINAL discriminator readback recorded; canonical docs synced; orchestration-state quartet closure pass.**
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: post-cycle-27 closure-doc-sync commit `291b607a46` on `apple-silicon-performance`.
- Bounded goal: "Deploy cycle-27 oracle-agent + unchanged cycle-25 witness-only XBE against real Xbox; execute canonical cycle-26-style sequence; record discriminator readback; document outcome; commit."
- Result: SLICE RAN. ZERO source/script code edits; ZERO XBE rebuilds; doc + evidence-file edits only. Final readback unambiguous (D-cycle-27).

## Plan summary (this session, executed in order)

1. Read required docs/state (handoff.md cycle-27/26 entries, orchestration-workflow.md, decision-log.md cycle-27/26 entries, orchestration-state quartet, oracle-and-xbe rule snapshot, oracle-orchestrator.py ensure_agent flow, cycle-26 SUMMARY.md).
2. Probed Xbox reachability (08:44:29Z): ping 0% loss, FTP/21 CLOSED, agent/9001 OPEN → cycle-23 agent (resident since cycle 26) was foreground XBE.
3. Captured baseline `witness.scan`: `count=1 phys=0x03eb3000 reserved0=0 reserved1=0 mapped_pages_seen=419` (matches cycle-26 exactly; precondition MET).
4. `reboot` agent → dashboard FTP-LIST OK at t+27s.
5. FTP-uploaded cycle-27 `oracle-agent/bin/default.xbe` (417 792 B) → `/E/Apps/oracle-agent/default.xbe`; FTP LIST verified remote size.
6. Confirmed cycle-25 `witness-only/default.xbe` (147 456 B) still present on `/E/Apps/witness-only/` from cycle-26 deploy.
7. `ensure-agent` launches cycle-27 build; banner unchanged as expected; post-launch rescan identical to baseline (kernel pool returned same persistent phys to new agent).
8. `runxbe witness-only` via cycle-27 agent → agent died on chainload → FTP-LIST + 9001 + ICMP poll → **dashboard fully ready at t+70s** (matches cycle 26's 70.17s).
9. Post-run `ensure-agent` (cycle-27 build) + FINAL `witness.scan` → **count=1 buf.0 phys=0x03eb3000 reserved0=0 reserved1=0** = outcome D-cycle-27.
10. Wrote `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/SUMMARY.md`.
11. Canonical docs synced: handoff.md cycle-28 entry on top (cycle-27 preserved unchanged), decision-log.md cycle-28 entry above cycle-27 (no supersession), orchestration-state quartet closure pass.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Xbox reachability + precondition checks passed.
3. [x] Cycle-27 oracle-agent successfully FTP-uploaded and re-launched on Xbox.
4. [x] Canonical cycle-26-style sequence executed (chainload + FTP-LIST poll + post-run scan).
5. [x] Final `witness.scan` captured = outcome D-cycle-27 (`count=1 live=1 reserved0=0 reserved1=0`).
6. [x] Evidence preserved under `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/` (9 logs + SUMMARY.md).
7. [x] Canonical docs synced: handoff.md cycle-28 entry, decision-log.md cycle-28 entry, orchestration-state quartet (current-cycle.md = this file, claude-status.md, validation-status.md, handoff-summary.md) closure pass.
8. [x] Closure commit landed on `apple-silicon-performance` (this commit).
9. [x] Two pre-existing untracked `.hermes_cycle*.txt` prompt files NOT staged (consistent with cycles 26/27 handling).

## Out-of-scope (kept bounded for cycle 28)

- NO xemu-fork host source touched.
- NO `oracle-agent/` source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched.
- NO witness-only source touched.
- NO image-blit source touched.
- NO XBE rebuilds.
- NO cycle-29 options (b)/(c)/(d) pursued.
- NO re-run of cycle-26's invalid-path or mirror controls (already characterized).
- NO PushNotification — outcome is definitive partial-discriminator, not blocker / not milestone.

## Outcome (cycle 28)

**D-cycle-27 outcome shape observed.** The cycle-27 preserve branch is correctly built and deployed; the preserve gate's strict predicate would have detected and preserved any A.4-tagged stamp `xbed_a4_witness.c` actually writes (MAIN_ENTERED → reserved0=0xA4000001/reserved1=1; POST_MARKER0 → reserved0=0xA4000003/reserved1=2). The observed `(0,0)` readback after the cycle-27 agent re-allocates the deterministic kernel-pool page phys=0x03eb3000 conclusively means **no A.4-tagged stamp existed on the page at re-allocation time** — i.e. the witness fires from `witness-only` did not land their stamps in any observable form. The cycle-26 "stamp landed and got wiped by agent's memset" hypothesis is therefore **INVALIDATED**. The cycle-26 ambiguity collapses to "stamp never landed."

## What outcome D-cycle-27 resolves and what it leaves open

**RESOLVED (this session):**
- The cycle-26 indistinguishable-causes pair "stamp landed and got wiped vs stamp never landed" collapses on the wiped side.
- Cycle-27 option (a) is demonstrated insufficient for breaking the cycle-26 ambiguity to A1/A2.
- 70 s chainload→dashboard-ready for witness-only confirmed as real Xbox behavior (two-cycle reproducibility).
- Kernel-pool deterministic phys=0x03eb3000 reuse confirmed across at least 6 consecutive agent re-launches in same power session.

**LEFT OPEN (cycle 29 scope):**
- Why the stamp never landed. Three live causes: (α) `xbed_a4_witness::a4_candidate_ok` kseg0 scan doesn't find agent's XCTR buffer from non-agent process context, (β) scan finds it but write faults silently, (γ) witness-only's main() never reaches the fire calls.
- Cycle-22 leading hypothesis status: still WEAKENED; cycle 28 evidence equally consistent with (γ) and with (α)/(β).
- The 70 s recovery shape itself (slow kseg0 scan vs delayed-fault watchdog vs slow BIOS POST) remains undiscriminated.
- §H.6 default-on shape decision REMAINS DEFERRED.

## Recommended cycle-29 scope (NOT executed this session)

Adopt one of the cycle-27 closure's remaining candidate redesigns:

- **(c) [recommended]** Witness-only allocates its OWN persistent page via `MmAllocateContiguousMemoryEx` with a unique magic tag (separate from XCTR). Eliminates reliance on kseg0 scan finding the agent's buffer. Discriminates (α) from (γ): if (c) lands a stamp visible to an analogous read-only scanner (the agent's own scanner is known-good at finding XCTR buffers — same code as the writer's scanner), then (α) was the cycle-26/28 blocker; if even (c) lands nothing, (γ) becomes leading.
- **(d)** On-screen visual breadcrumb captured via `oracle-orchestrator.py capture` mid-run or composite capture during witness-only execution. Specifically discriminates (γ): a known-pattern breadcrumb on screen would prove witness-only's main() did execute.
- **(b)** Agent-side prior-phys dump + read-only kseg0 dump verb. Lower priority than (c) — assumes the agent buffer is findable from non-agent context, which is what (c) directly tests.

§H.6 default-on / long-term-fix decision REMAINS DEFERRED.
M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-29 design + run), §G.5, RT-as-texture.
