# Current Cycle

- Cycle: 26 Path A.4 real-Xbox witness-only deployment (**CLOSED — 2026-05-22**, partial discriminator outcome).
- Started: 2026-05-22 (Hermes-supervised fresh bounded session; Claude Code worker autonomous run).
- Closed: 2026-05-22 (this update + closure commit).
- State: **CLOSED. Witness-only deployed and chainloaded on real Xbox 4× (plus 2 controls). Outcome shape D (not in cycle-25 design table): clean dashboard recovery in ~70 s with no observable orphan. Hypothesis #5 PARTIALLY INVALIDATED in the hard-hang sense (Xbox did not hang as in cycle 24). Cycle-22 leading hypothesis still WEAKENED — cycle-27 candidate redesign documented to break the stamp-vs-no-stamp ambiguity.**
- Owner: Claude Code worker (fresh bounded session), launched 2026-05-22.
- HEAD at start: post-cycle-25 closure commit on `apple-silicon-performance`.
- Bounded goal: "Determine whether cycle 26 is feasible autonomously from this Mac right now; if feasible, run the canonical cycle-26 sequence, document outcome, update state."
- Result: SLICE RAN; concrete partial-discriminator outcome documented with evidence; no source-code edits, no XBE rebuilds.

## Plan summary (this session, executed in order)

1. Read required docs/rules + orchestration-state files (handoff.md cycle-25/24/23 entries, orchestration-workflow.md, decision-log entries, current-cycle/claude-status/validation-status/handoff-summary).
2. Confirmed Xbox @ 192.168.0.200 reachable: ICMP 0% loss, FTP/21 OPEN, agent/9001 CLOSED → dashboard state (Hermes had already power-cycled post cycle-24).
3. Confirmed cycle-23 oracle-agent deployed (witness.scan verb registered, banner v0.4).
4. Baseline witness.scan: count=1 buf phys=0x03eb3000 reserved[0]=0 mapped_pages_seen=419. **Precondition MET.**
5. FTP-uploaded `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` (147 456 B) → `/E/Apps/witness-only/default.xbe`. FTP LIST confirms remote size match.
6. Chainloaded witness-only 4× (incrementally tightening polling methodology because nc -z gave false-positive early ftp=Y readings). Definitive timing via `curl FTP LIST` poll (log 20-): **t+70.17 s dashboard fully ready**.
7. Ran 2 controls to disambiguate timing:
   - Invalid path chainload → t+20.67 s (matches agent's XLaunchXBE failure path: Sleep(500)+fail+Sleep(2000)+HalReturnToFirmware → BIOS POST).
   - Known-good mirror XBE chainload → t≈36 s (clean Tier-1 diag chainload).
8. Post-run witness.scan after each chainload: SAME count=1 phys=0x03eb3000 reserved[0]=0 mapped_pages_seen=419. **No orphan observed in any of 4 chainloads.**
9. Recorded the outcome (Outcome D) in evidence summary; updated orchestration-state quartet for cycle-26 closure.

## Exit criteria — final status

1. [x] Required docs read; plan summarized to current-cycle.md (this file) + claude-status.md.
2. [x] Xbox reachability verified; precondition (1 live buffer, reserved[0]==0) MET on baseline scan.
3. [x] Witness-only XBE FTP-uploaded successfully (147 456 B, FTP LIST verified).
4. [x] runxbe witness-only issued 4× (plus 2 controls); chainload→dashboard-ready timing measured with `curl FTP LIST` as ground truth (~70 s).
5. [x] Post-run witness.scan after each chainload (consistent: no orphan, same phys, reserved[0]==0).
6. [x] Evidence preserved at `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/{00..20-*.log, SUMMARY.md}` (gitignored).
7. [x] Canonical docs synced: handoff.md cycle-26 entry on top, decision-log.md cycle-26 entry above cycle-25, orchestration-state quartet (current-cycle.md = this file, claude-status.md, validation-status.md, handoff-summary.md) closure pass.
8. [x] Closure commit pending (this commit on `apple-silicon-performance`).

## Out-of-scope (kept bounded for cycle 26)

- NO source/script code edits (cycle-23 lib + agent + image-blit + cycle-25 witness-only all UNTOUCHED).
- NO XBE rebuilds.
- NO xemu-fork host source touched.
- NO retail-title, §G.5, RT-as-texture work.
- NO flag default flips.

## Outcome (cycle-26)

**Outcome D** (not in cycle-25 design table): chainload→dashboard-ready in **~70 s**, NO orphan observed in post-run scan.

Cycle-25 expected branches:
- A: reboot ~5..15 s + orphan reserved[0]==0xA4000003 → witness IS real-Xbox-safe in this minimal XBE; cycle-22 leading hypothesis INVALIDATED.
- B: identical hang to cycle 24 (>5 min silent) → witness mechanism real-Xbox-incompatible.
- C: clean reboot + orphan reserved[0]==0xA4000001 → second fire hung.

Observed (D): clean dashboard recovery WITHOUT a power-cycle (rules out B in the hang sense) BUT ~70 s gap (longer than 22 s normal reboot, shorter than 928 s+ cycle-24 hang) AND no observable orphan (rules out raw A or C).

## What outcome D resolves and what it leaves open

**RESOLVED:**
- **Outcome B (cycle-24-like hard hang) RULED OUT.** The kseg0-scan witness mechanism from a non-agent process context, in this minimal XBE, does NOT hard-hang the Xbox. Hypothesis #5 PARTIALLY INVALIDATED in the catastrophic-hang sense.
- Witness-only chainload is FUNDAMENTALLY DIFFERENT from invalid-path control (20.67 s vs 70.17 s) and from a clean known-good chainload like mirror (~36 s vs 70.17 s). Something in witness-only's execution delays recovery by ~30..50 s vs a known-good chainload.

**LEFT OPEN:**
- **Stamp-vs-no-stamp ambiguity.** No orphan observed → either (i) witness fire never landed a stamp, OR (ii) stamp landed but new agent's `s_allocate_fresh::memset(vp, 0, ...)` wiped it after the kernel pool deterministically returned the same phys=0x03eb3000 across 3 observed agent re-launches. Cannot discriminate from session-level evidence alone.
- **Cycle-22 leading hypothesis status: still WEAKENED.** The 70 s gap is consistent with witness-only's main() running to HalReturnToFirmware but encountering a delayed kernel fault (watchdog recovery), AND consistent with witness-only never reaching main() (the linked xbed_a4_witness `.text` itself causing a slow pre-main fault). Cannot resolve from current evidence.
- **§H.6 default-on shape decision REMAINS DEFERRED** — cycle-26 does not unblock §H.6 default-on; still blocked on definitively resolving the witness mechanism's real-Xbox viability.

## Recommended cycle-27 candidate scope (NOT executed this session)

To break the cycle-25/26 ambiguity. Pick ONE:
- (a) Modify `oracle-agent/controller.c::s_allocate_fresh` to NOT `memset` when it finds an existing `XCTR + version==1` buffer at the returned phys (preserve any landed witness stamp).
- (b) Add an agent verb that dumps the prior controller-buffer phys+reserved[] from a known file BEFORE chainload, then post-chainload reads from that exact phys.
- (c) Use a fresh `MmAllocateContiguousMemoryEx` page from witness-only directly (separate magic tag) instead of stamping the agent's buffer.
- (d) Add an on-screen visual breadcrumb captured via `oracle-orchestrator.py capture` mid-run.

Cycle-27 should also discriminate the ~70 s delay: is it slow kseg0 scan on real Xbox vs a delayed-fault watchdog window?

§H.6 default-on / long-term-fix decision REMAINS DEFERRED.
M15 overall still NOT MET pending §H.6 default-on shape (blocked on cycle-27 stamp-vs-no-stamp discriminator), §G.5, RT-as-texture.
