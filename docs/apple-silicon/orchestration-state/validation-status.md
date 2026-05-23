# Validation Status

- Active slice: cycle 27 Path A.4 option (a) — `oracle-agent/controller.c::s_allocate_fresh` now preserves an existing plausible `oracle_ctrl_buffer` witness header across agent restart instead of unconditionally `memset`-wiping it. Substantive C source slice (~100 LOC across new helper + branch refactor + paired doc-only edits across 4 other files).
- Validation state: **CLOSED. Codex 3-round validation green; round 3 = LOOKS GOOD. Validation marker written at `.claude/state/codex-validate-last-run`.**

## Gate status (cycle 27) — final

- [x] Required docs read (handoff.md cycle-26/25/24, orchestration-workflow.md, decision-log entries, current-cycle/claude-status/validation-status/handoff-summary, oracle-and-xbe rule snapshot, controller.{c,h}, lib/xbed_a4_witness.{c,h}, commands.c witness reader).
- [x] Worker receipt posted to current-cycle.md + claude-status.md.
- [x] Option (a) implementation: `s_page_has_plausible_witness_header` helper added with strict-subset predicate (Codex round-1 medium); `s_allocate_fresh` two-branch preserve/legacy refactor; conditional debugPrint breadcrumb in preserve branch; cache_writeback_invalidate after either branch.
- [x] Paired doc edits: `controller.h` `oracle_ctrl_init` updated; `commands.c::cmd_witness_scan` body comment extended; `witness-only/README.md` discriminator table updated; `witness-only/manifest.json` `artifacts.witness_readback.notes` + `expected_results.real-xbox/physical/cycle-26.notes` extended.
- [x] oracle-agent XBE rebuilt via nxdk `make`; bin/default.xbe = 417 792 B (size unchanged from cycle 26); oracle-agent.iso = 983 040 B; benign `lld: warning: .edata=.rdata` repeats from prior cycles. manifest.json re-parses cleanly.
- [x] Codex validation round 1 (changes mode): MINOR ISSUES (medium predicate-too-loose + low witness-only-docs-orphan-only-shape). Both adopted in full.
- [x] Codex validation round 2 (changes mode, post-adoption diff): MINOR ISSUES (round-1 MEDIUM RESOLVED; round-1 LOW PARTIAL — manifest `expected_results.real-xbox/physical/cycle-26.notes` retained legacy-only wording). Adopted: that field now describes both shapes.
- [x] Codex validation round 3 (changes mode, post-PARTIAL-fix diff): LOOKS GOOD (round-2 PARTIAL RESOLVED; round-1 MEDIUM still RESOLVED; no new round-3 findings; one out-of-scope note pointing at decision-log.md cycle-26 entry, addressed by adding cycle-27 entry above without superseding cycle-26).
- [x] Validation marker written at `.claude/state/codex-validate-last-run`.
- [x] Canonical docs synced: handoff.md cycle-27 entry on top (cycle-26 preserved unchanged); decision-log.md cycle-27 entry above cycle-26 (no supersession); orchestration-state quartet closure pass (current-cycle.md, claude-status.md, validation-status.md = this file, handoff-summary.md).
- [x] Closure commit landed on `apple-silicon-performance`: `df999e41ea`.
- [ ] **Cycle 28 real-Xbox deployment** — explicitly out of scope; Hermes-scheduled.

## Codex validation decision (cycle 27)

Cycle 27 is a **substantive code-source slice** on `oracle-agent/controller.c` (++97 lines of substantive C plus paired doc-only edits totaling ~170 lines across 5 files). Rule #15 trigger #2 fires unambiguously (non-trivial uncommitted code in `xemu-fork/`, aggregate diff > 30 lines on the renderer-adjacent oracle-agent C source). Three Codex rounds run; round 3 = LOOKS GOOD with no new findings. Adoption record:

- Round 1 MEDIUM (predicate too loose): RESOLVED.
- Round 1 LOW (docs only describe orphan-shape success): RESOLVED across `cmd_witness_scan` body comment, witness-only README, and both manifest fields.
- Round 2 PARTIAL (manifest `expected_results.real-xbox/physical/cycle-26.notes` residual): RESOLVED.
- Round 3 no new findings; out-of-scope note about decision-log cycle-26 entry not applicable (decision-log cycle-27 entry added above cycle-26 in this same commit, preserving cycle-26 as historical record).

## What stands from cycles 17 + 19 + 20 + 21 + 22 + 23 + 24 + 25 + 26

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle 19/20/21's 5× reproducible 22.4 s chainload→FTP-back gap for pre-witness image-blit binaries — UNCHANGED.
- Cycle 22 invalidation of cycle-19 hypothesis #1 (launch-path blocker) and cycle-21 hypothesis #3 (FATX-driver/NT-mount state divergence for D:\\) — unchanged.
- Cycle 23 closure (witness instrumentation Codex round-2 PASS_WITH_FINDINGS + MINOR resolved post-round-2) — unchanged.
- Cycle 24 concrete-blocker outcome (cycle-23 image-blit hard-hangs real Xbox; cycle-22 leading hypothesis WEAKENED; NEW hypothesis #5 promoted) — unchanged.
- Cycle 25 witness-only XBE SHIPPED + Codex round-3 PASS_WITH_FINDINGS + local xemu-Metal smoke green — unchanged.
- Cycle 26 closure (outcome shape D = ~70 s recovery + no observable orphan; hypothesis #5 PARTIALLY INVALIDATED in the catastrophic-hang sense; stamp-vs-no-stamp ambiguity OPEN) — unchanged from a state-of-the-world perspective; cycle 27 SHIPS THE TOOL to break that ambiguity but does not RUN it on real Xbox.

## What changes (cycle 27)

- Agent's `s_allocate_fresh` no longer unconditionally wipes a `MmPersistContiguousMemory`-tagged page that already carries a plausible `oracle_ctrl_buffer` header.
- A new positive cycle-26-style readback shape `count=1 live=1 reserved0=0xA4xxxxxx` is now documented across `commands.c::cmd_witness_scan` body comment, `witness-only/README.md`, and `witness-only/manifest.json`. Either this new live-buffer shape OR the legacy orphan shape counts as "witness landed".
- Cycle-22 leading hypothesis status: UNCHANGED (still WEAKENED). Cycle 27 ships the discriminator-sharpening tool; cycle 28 runs it.
- Hypothesis #5 status: UNCHANGED from cycle 26 (PARTIALLY INVALIDATED in the catastrophic-hang sense).
- §H.6 default-on shape decision REMAINS DEFERRED.
- M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-28 real-Xbox discriminator run with cycle-27 oracle-agent), §G.5, RT-as-texture.

## What cycle 27 does NOT resolve

- **Stamp-vs-no-stamp question at the real-Xbox level.** Cycle 27 ships the tool; cycle 28 runs it.
- **The cycle-26 ~70 s recovery shape.** Preserve branch only sharpens the post-chainload readback, not the chainload-→dashboard-ready timing.
- **Cycle-22 leading hypothesis.** Will be resolved one way or the other by cycle 28's `witness.scan` readback.
