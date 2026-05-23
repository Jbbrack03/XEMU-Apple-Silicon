# Validation Status

- Active slice: cycle 25 Path A.4 witness-mechanism viability discriminator XBE. Operationally a new-XBE source slice (5 new files under `scripts/apple-silicon/xbe-tests/witness-only/`; 2 built artifacts; cycle-23 lib + agent + image-blit UNTOUCHED; no xemu-fork host source touched).
- Validation state: **CLOSED. Codex round-3 PASS_WITH_FINDINGS; all blocking + medium + low findings RESOLVED. Validation marker written.**

## Gate status (cycle 25) — final

- [x] Fresh worker receipt posted before deeper work (`docs/apple-silicon/orchestration-state/current-cycle.md` + `claude-status.md`).
- [x] Required docs read (cycle-24 handoff entry, decision-log cycle-24 entry, oracle-and-xbe rule snapshot, peer-XBE patterns from `pipeline-smoke/`, `image-blit/`, `controller-readback/`).
- [x] Authored: `scripts/apple-silicon/xbe-tests/witness-only/{main.c, Makefile, manifest.json, README.md, .gitignore}`.
- [x] Built `bin/default.xbe` (147 456 B) + `witness-only.iso` (720 896 B) via `eval "$(nxdk/bin/activate -s)" && make`. Lib.mk pattern identical to image-blit's.
- [x] Local xemu-Metal smoke validation green: 9× `witness-only: main() entered`, 9× `xbed_a4_witness: enter stage=1`, 9× `fire1 returned phys=0x00000000`, 9× `xbed_a4_witness: enter stage=3`, 9× `fire2 returned phys=0x00000000`, 8× `rebooting via HalReturnToFirmware(HalRebootRoutine)` lines across a 25 s timeout window under `XEMU_GUEST_LOG=1`. Evidence preserved at `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-20260523T034519Z/{xemu.log, summary.txt}` (gitignored per project convention).
- [x] Codex validation round 1 (changes mode): MAJOR ISSUES, 4 findings. All adopted:
  - HIGH #1 — Outcome-A overclaim narrowed across README.md, manifest.json, current-cycle.md (and main.c during round 2).
  - HIGH #2 — claude-status.md rewritten as strict in-progress receipt.
  - MEDIUM #3 — .gitignore added per peer-XBE convention.
  - LOW #4 — manifest success-case count restated as "exactly 2 total buffers (1 live + 1 new orphan)".
- [x] Codex validation round 2: BLOCK on residual #1 PARTIAL (main.c had 2 leftover overclaim sites) + new LOW (current-cycle.md ↔ claude-status.md disagreement). Both adopted.
- [x] Codex validation round 3: **PASS_WITH_FINDINGS**. Round-2 #1 RESOLVED (`main.c:47-55, 95-100`). Round-2 new LOW PARTIAL (claude-status.md residual stale wording — addressed in this update before final docs sync). Round-1 #2/#3/#4 CARRIED RESOLVED. No new issues. No open questions.
- [x] Canonical docs synced: `handoff.md` cycle-25 entry on top (cycle-24 entry preserved unchanged); `decision-log.md` cycle-25 entry above cycle-24 (no supersession); orchestration-state quartet (`current-cycle.md`, `claude-status.md`, `validation-status.md` — this file, `handoff-summary.md`) closure pass.
- [x] Validation marker written to `.claude/state/codex-validate-last-run`.
- [x] Closure commit landed on `apple-silicon-performance` in this commit.
- [ ] **Cycle-26 real-Xbox deployment slice** — explicitly out of scope; Hermes-scheduled; cycle-25 deliberately stops here.

## Codex validation decision (cycle 25)

Cycle 25 ships **non-trivial XBE source** (5 NEW files + 2 built artifacts; main.c is ~180 lines including docs/comments). Per rule #15, Codex validation is **mandatory** — NOT a doc-only carve-out. Three rounds were run; round 3 = PASS_WITH_FINDINGS. Validation marker written.

Round 1 findings adopted:
1. HIGH #1 — Outcome-A discriminator overclaim: witness-only's `Sleep(500)` substitution for the marker helper means a clean `0xA4000003` outcome does NOT independently exclude the marker helper as a contributor to image-blit's hang. Narrowed across all relevant files; marker-helper exclusion requires a follow-on cycle.
2. HIGH #2 — claude-status.md overstated completion; rewritten as strict in-progress receipt; closure language deferred until validation marker landed.
3. MEDIUM #3 — .gitignore added per peer-XBE convention.
4. LOW #4 — manifest success-case count restated as "exactly 2 total buffers".

Round 2 findings adopted:
- Residual #1 PARTIAL on main.c: 2 leftover overclaim sites rewritten.
- New LOW: current-cycle.md exit-checkbox state flipped to reflect actual on-disk state.

Round 3 findings:
- Round-2 #1 RESOLVED (main.c:47-55, 95-100).
- Round-2 new LOW PARTIAL (claude-status.md residual stale wording about "round 2 pending") — addressed in this final update before docs sync.
- No new issues; no open questions.

## What stands from cycles 17 + 19 + 20 + 21 + 22 + 23 + 24

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle 19/20/21's 5× reproducible 22.4 s chainload→FTP-back gap for pre-witness image-blit binaries — UNCHANGED.
- Cycle 22 invalidation of cycle-19 hypothesis #1 (launch-path blocker) and cycle-21 hypothesis #3 (FATX-driver/NT-mount state divergence for D:\\) — unchanged.
- Cycle 23 closure (witness instrumentation Codex-validated round-2 PASS_WITH_FINDINGS + MINOR resolved post-round-2) — unchanged.
- Cycle 24 concrete-blocker outcome (cycle-23 image-blit hard-hangs real Xbox; cycle-22 leading hypothesis WEAKENED; NEW hypothesis #5 promoted) — unchanged.

## What changes (cycle 25)

- Cycle-24's recommended witness-only XBE is now SHIPPED, BUILT, smoke-validated, and Codex-validated. Cycle-26 real-Xbox deployment is unblocked from the source-slice side — Hermes can schedule it whenever.
- Cycle-22 leading hypothesis status is UNCHANGED (still WEAKENED carried from cycle 24); cycle 26 will discriminate.
- Hypothesis #5 (witness mechanism real-Xbox safety from non-agent process context) remains TOP-PRIORITY; cycle 26 discriminates.
- §H.6 default-on shape decision is UNCHANGED (still blocked on cycle-26 witness-mechanism viability discrimination, NOT on cycle-25 itself).
