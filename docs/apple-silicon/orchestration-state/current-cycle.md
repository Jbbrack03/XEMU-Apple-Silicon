# Current Cycle

- Cycle: 29 Path A.4 option (c) self-allocated witness — **bounded code slice CLOSED on `apple-silicon-performance`**. Implementation + 4-round Codex (final round LOOKS GOOD) + paired-doc sync + rebuilt XBEs. Real-Xbox deployment is cycle-30 scope (Hermes's call).
- Started: 2026-05-23 (Hermes-supervised bounded session; Claude Code worker autonomous run from this Mac).
- Closed: 2026-05-23 (closure commit `725bc97bbd` landed on `apple-silicon-performance`).
- State: **CLOSED.** New shared diag-XBE lib `lib/xbed_self_witness.{h,c}` allocates the diag XBE's OWN persistent page via `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory` and stamps a unique `'WTNS'` magic (0x534E5457) + version 1 + reserved0=(0xA4<<24)|stage + reserved1=counter; new oracle-agent read-only verb `witness.scan-self` enumerates kseg0 'WTNS' pages with the same `MmGetPhysicalAddress`-gated safety pattern + plausibility predicate as `cmd_witness_scan`; `witness-only/main.c` calls self-witness AFTER existing cycle-23 fires (Codex round-1 high ordering decision); shim opted in by `witness-only/Makefile` only (NOT `lib.mk` default SRCS). Positioned narrowly as a (γ)-only discriminator — α+β remain live on a successful cycle-30 readback because cycle-29 stamps a self-owned page, not the agent's XCTR page (Codex round-1 high finding adopted in all 5 doc surfaces). Build green; both XBEs rebuilt cleanly.
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: post-cycle-28 closure commit `c77b509149` on `apple-silicon-performance`.
- Bounded goal: "Implement cycle 29 option (c): create the minimal next-step infrastructure so witness-only can allocate ITS OWN persistent page with a unique witness magic tag, and the oracle-agent can read that page back after a run without relying on finding the agent XCTR buffer from non-agent context."
- Result: SLICE COMPLETE. ZERO xemu-fork host source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO `oracle-agent/controller.c` touched (cycle-27 preserve gate intact); ZERO image-blit / retail-title source touched.

## Plan summary (this session, executed in order)

1. Read required docs/state (handoff.md cycle-28 entry, decision-log.md cycle-28 entry, orchestration-state quartet, orchestration-workflow.md, oracle-agent + witness-only + lib source, cycle-23 witness lockstep contract).
2. Designed cycle-29 magic + header layout (`'WTNS'` = 0x534E5457; 16-byte header symmetric with `oracle_ctrl_buffer`; 0xA4 tag mirrors cycle-23 convention; counter ceiling 4096 mirrors cycle-23 plausibility filter).
3. Created `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.{h,c}` with `xbed_self_witness_fire(stage)`: allocates persistent contiguous page on first call, stamps magic + version + reserved0 + reserved1, returns phys (0 on hard failure). `wbinvd` after every stamp.
4. Wired into `lib/lib.mk` (initially).
5. Modified `witness-only/main.c` — added self-witness fires (initially BEFORE cycle-23 fires).
6. Added new agent verb `cmd_witness_scan_self` in `oracle-agent/commands.{c,h}`; registered in `oracle-agent/main.c::s_cmds[]`; `cmd_help` text updated.
7. Updated paired docs: `witness-only/{README.md, manifest.json}` + agent body comment + new shim header doc.
8. Rebuilt both XBEs cleanly.
9. **Codex round 1 = MAJOR ISSUES** (3 findings: HIGH #1 strictly-additive claim was misleading; HIGH #2 discriminator over-claim; LOW #3 lib.mk pulled new SRCS everywhere). All 3 adopted:
   - Moved cycle-29 fires to AFTER cycle-23 fires so cycle-23 path is bit-identical to cycle 25 through second cycle-23 fire.
   - Rewrote all 5 doc surfaces to position cycle 29 as (γ)-only discriminator with α+β remaining live.
   - Moved `xbed_self_witness.c` out of `lib.mk` and added opt-in line to `witness-only/Makefile` only.
10. Rebuilt both XBEs after round-1 adoption.
11. **Codex round 2 = MINOR ISSUES** (2 LOW findings: round-1 HIGH #1 PARTIAL because shim header still said "strict superset"; new LOW because operator tables missed partial-success shapes). Both adopted: header text harmonized; cycle-30 table expanded to 7 rows (E1/E1'/E1''/E2/E3/E4/E5).
12. **Codex round 3 = MINOR ISSUES** (round-2 LOW PARTIAL — tolerated `0xA4000003 reserved1=1` second-self-fire-only shape still uncalled). Adopted: README + manifest now explicitly say table is representative not exhaustive + document reader's acceptance rule.
13. **Codex round 4 = LOOKS GOOD.** Round-3 LOW RESOLVED; no new findings.
14. Wrote validation marker at `.claude/state/codex-validate-last-run`.
15. Canonical docs synced: handoff.md cycle-29 entry on top (cycle-28 preserved unchanged); decision-log.md cycle-29 entry above cycle-28 (no supersession); orchestration-state quartet closure pass (this file + claude-status.md + validation-status.md + handoff-summary.md).

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Cycle-29 option (c) design locked in (magic, layout, allocator parameters, scanner predicate).
3. [x] New shim `lib/xbed_self_witness.{h,c}` created and reviewed.
4. [x] `witness-only/main.c` patched (self-witness AFTER cycle-23 fires; Codex round-1 high ordering decision).
5. [x] `witness-only/Makefile` opts in `xbed_self_witness.c`; `lib/lib.mk` does NOT carry it in default SRCS.
6. [x] New agent verb `witness.scan-self` registered and documented.
7. [x] Both XBEs rebuilt cleanly (`oracle-agent` 417 792 B unchanged; `witness-only` 151 552 B = +4 096 B).
8. [x] 4-round Codex validation completed (round 4 = LOOKS GOOD); validation marker written.
9. [x] Canonical docs synced (handoff.md, decision-log.md, orchestration-state quartet).
10. [x] Closure commit landed on `apple-silicon-performance`: `725bc97bbd`.
11. [x] Two pre-existing untracked `.hermes_cycle*.txt` prompt files NOT staged.

## Out-of-scope (kept bounded for cycle 29)

- NO xemu-fork host source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched.
- NO `oracle-agent/controller.c` allocator/preserve-gate touched.
- NO image-blit source touched.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO real-Xbox deployment (cycle-30 scope, Hermes's call).
- NO PushNotification — bounded implementation slice, not blocker / milestone.

## Outcome (cycle 29)

**Implementation complete + Codex-validated.** The cycle-29 self-allocated witness infrastructure is shipped; the cycle-30 readback path is set up to discriminate cause (γ) "main() never reaches the fire calls" from causes (α) "scan can't find XCTR" and (β) "scan finds XCTR but write faults." (α) and (β) BOTH remain live on a successful cycle-30 readback — the cycle-29 self-witness stamps a self-owned page, not the agent's XCTR page, so it cannot distinguish α from β. α-vs-β discrimination is cycle 31+ scope (option (b) — agent-side prior-phys dump + read-only kseg0 dump verb).

## Recommended cycle-30 scope (NOT executed this session — Hermes's call)

FTP-deploy cycle-29 oracle-agent + cycle-29 witness-only to real Xbox. Canonical cycle-26-style sequence extended with `witness.scan-self` queries at baseline + post-run:
1. Reachability + cycle-23 agent baseline; query `witness.scan` AND `witness.scan-self` (precondition: `witness.scan count=1 live=1 reserved0=0` AND `witness.scan-self count=0`; power-cycle if either fails).
2. `reboot` agent → dashboard FTP-LIST.
3. FTP-upload cycle-29 oracle-agent + cycle-29 witness-only.
4. `ensure-agent` launches cycle-29 build; rescan baseline.
5. `runxbe witness-only`; poll FTP/21 + agent/9001 + ICMP ping for dashboard return (expect ~70 s on success).
6. On return: `ensure-agent` (cycle-29 build) + final `witness.scan` AND `witness.scan-self`.

Expected outcomes E1/E1'/E1''/E2/E3/E4/E5 enumerated in `witness-only/README.md` cycle-30 discriminator table.
