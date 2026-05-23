# Validation Status

- Active slice: cycle 29 Path A.4 option (c) self-allocated witness — implementation + paired-doc + rebuild slice. ZERO real-Xbox run.
- Validation state: **CLOSED. Codex 4 rounds — round 4 = LOOKS GOOD with no new findings; rule #15 satisfied. Validation marker written at `.claude/state/codex-validate-last-run`.**

## Gate status (cycle 29) — final

- [x] Required docs read (handoff.md cycle-28/27/26/25, decision-log.md cycle-28/27/26/25, orchestration-state quartet, orchestration-workflow.md, witness/oracle-agent/lib source).
- [x] Cycle-29 option (c) design locked in (unique magic 'WTNS' = 0x534E5457; 16-byte header symmetric with `oracle_ctrl_buffer`; allocator parameters bit-identical to `oracle-agent/controller.c::s_allocate_fresh`; scan filter mirrors cycle-27 preserve-gate strict subset of cycle-23 plausibility predicate).
- [x] New shim `lib/xbed_self_witness.{h,c}` created.
- [x] `witness-only/main.c` patched (cycle-29 fires AFTER cycle-23 fires per Codex round-1 high finding #1).
- [x] `witness-only/Makefile` opts in `xbed_self_witness.c`; `lib/lib.mk` does NOT (Codex round-1 low finding #3).
- [x] New oracle-agent verb `witness.scan-self` implemented + registered + help-line added.
- [x] Paired docs updated: `witness-only/{README.md, manifest.json}` + `oracle-agent/commands.c` body comment + `lib/xbed_self_witness.h` doc + `lib/lib.mk` comment.
- [x] Both XBEs rebuilt cleanly (`oracle-agent` 417 792 B unchanged; `witness-only` 151 552 B = +4 096 B from cycle 25).
- [x] **Codex round 1 = MAJOR ISSUES (3 findings).** All 3 adopted in full.
- [x] **Codex round 2 = MINOR ISSUES (2 LOW findings).** Both adopted.
- [x] **Codex round 3 = MINOR ISSUES (1 LOW).** Adopted.
- [x] **Codex round 4 = LOOKS GOOD.** No new findings.
- [x] Validation marker written at `.claude/state/codex-validate-last-run`.
- [x] Canonical docs synced (handoff.md cycle-29 entry on top with cycle-28 preserved unchanged; decision-log.md cycle-29 entry above cycle-28; orchestration-state quartet closure pass).
- [ ] Closure commit on `apple-silicon-performance` — PENDING (next step).
- [ ] Cycle 30 (Hermes's call) — explicitly out of scope this session.

## Codex validation decision (cycle 29)

Cycle 29 is a substantive code slice — ~340 lines across the new shim + new agent verb + witness-only call sites + paired docs + rebuilt XBEs. Rule #15 trigger #2 (non-trivial uncommitted code in xemu-fork/ touching diag-XBE / oracle-agent C source) FIRES. Codex validation MANDATORY. Ran 4 rounds:

- **Round 1 = MAJOR ISSUES.** HIGH #1: "strictly additive" claim was misleading because the original cycle-29 ordering placed the self-witness fires BEFORE the cycle-23 fires, introducing kernel-allocator activity that confounded the cycle-25 baseline. HIGH #2: discriminator over-claimed; a `witness.scan-self` hit makes (γ) INVALIDATED but does not single out (α) — (β) remains live because cycle-29 stamps a self-owned page, not the agent's XCTR page. LOW #3: `xbed_self_witness.c` was added to `lib/lib.mk` default SRCS, widening the controlled-delta drift across the diag-XBE corpus.
  - **All 3 adopted.** Cycle-29 fires moved to AFTER cycle-23 fires (cycle-23 path now bit-identical to cycle 25 through second cycle-23 fire). All five doc surfaces rewritten to position cycle 29 as a (γ)-only discriminator with α+β remaining live and cycle 31+ option (b) flagged as the α-vs-β follow-up. `xbed_self_witness.c` moved out of `lib.mk` and opted in only by `witness-only/Makefile`.
- **Round 2 = MINOR ISSUES.** LOW #1 (round-1 HIGH #1 PARTIAL): `xbed_self_witness.h` still said "strict superset." LOW #2 (new): operator-facing tables missed partial-success shapes the reader tolerates.
  - **Both adopted.** Header text harmonized; cycle-30 table expanded to 7 rows (E1/E1'/E1''/E2/E3/E4/E5).
- **Round 3 = MINOR ISSUES.** LOW (round-2 LOW #2 PARTIAL): tolerated `0xA4000003 reserved1=1` second-self-fire-only shape still uncalled-out.
  - **Adopted.** README + manifest now explicitly say table is representative not exhaustive + document the reader's `(reserved0>>24)==0xA4 AND 1<=reserved1<=4096` acceptance rule.
- **Round 4 = LOOKS GOOD.** Round-3 LOW RESOLVED. No new findings.

Validation marker written at `.claude/state/codex-validate-last-run` (timestamp + sha256 fingerprint of `git status --porcelain`).

## What stands from cycles 17 + 19 + 20 + 21 + 22 + 23 + 24 + 25 + 26 + 27 + 28

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle 19/20/21's 5× reproducible 22.4 s chainload→FTP-back gap for pre-witness image-blit binaries — UNCHANGED.
- Cycle 22 invalidation of cycle-19 hypothesis #1 and cycle-21 hypothesis #3 — unchanged.
- Cycle 23 closure (witness instrumentation Codex round-2 PASS_WITH_FINDINGS) — unchanged.
- Cycle 24 concrete-blocker outcome (image-blit hard-hangs real Xbox; hypothesis #5 promoted) — unchanged.
- Cycle 25 witness-only XBE SHIPPED + Codex round-3 PASS_WITH_FINDINGS — unchanged.
- Cycle 26 closure (outcome shape D; hypothesis #5 PARTIALLY INVALIDATED in catastrophic-hang sense; stamp-vs-no-stamp ambiguity OPEN) — UPDATED by cycle 28 (now resolved to "stamp never landed").
- Cycle 27 closure (preserve-branch oracle-agent shipped with Codex 3-round green) — unchanged.
- Cycle 28 closure (D-cycle-27; stamp-landed-then-wiped INVALIDATED; α/β/γ enumerated) — unchanged.

## What changes (cycle 29)

- Cycle-30 (γ)-discriminator infrastructure SHIPPED.
- Cycle-29 binary is additive but NOT a strict superset of cycle 25 (Codex round-1 high finding #1): post-cycle-23-fires-to-reboot window now contains kernel-allocator activity for the self-witness page. Through the second cycle-23 fire, cycle 29 is bit-identical to cycle 25.
- Cycle-30 readback discriminator is narrowed to (γ)-only (Codex round-1 high finding #2): a `witness.scan-self` hit invalidates γ but α+β both remain live. α-vs-β is cycle 31+ scope (option (b)).
- `xbed_self_witness.c` is OPT-IN per-XBE (Codex round-1 low finding #3): only `witness-only/Makefile` pulls it; the rest of the diag-XBE corpus is unaffected.
- Hypothesis status:
  - (α) STILL LIVE — cycle 30 will not move it; cycle 31 option (b) is the discriminator.
  - (β) STILL LIVE — same.
  - (γ) STILL LIVE this session — cycle 30 will move it (INVALIDATED on any `witness.scan-self` hit; LEADING on count=0).
  - Cycle-22 leading hypothesis: still WEAKENED (unchanged).
  - Hypothesis #5: still PARTIALLY INVALIDATED (cycle 29 does not run on real Xbox).
- §H.6 default-on shape decision REMAINS DEFERRED.
- M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-30 real-Xbox run + downstream α-vs-β cycle 31 work), §G.5, RT-as-texture.

## What cycle 29 does NOT resolve

- **(α) vs (β) discrimination.** Cycle-29's self-witness stamps a self-owned page; it does not exercise the failing write into the agent's XCTR page. A cycle-30 (E1) `WTNS count=1 0xA4000003/2` outcome would invalidate γ but leave α and β BOTH LIVE. Cycle 31 option (b) (agent-side prior-phys dump + read-only kseg0 dump verb) is the discrimination path.
- **(γ) "pre-main crash."** Cycle 29 sets up the cycle-30 measurement; it does not RUN the measurement. Cycle 30 is Hermes's call.
- **The ~70 s recovery shape** (slow kseg0 scan vs delayed-fault watchdog vs slow BIOS POST) observed in cycles 26 + 28 — cycle 29 may collapse this as a side-effect if option (c)'s self-allocated-page approach shows BIOS-POST-like timing (~17 s) instead of 70 s, but it is not a primary cycle-29 design goal.
