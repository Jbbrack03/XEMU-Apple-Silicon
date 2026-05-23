# Validation Status

- Active slice: cycle 23 Path A.4 — non-fopen kernel-pool controller-buffer witness for image-blit (XBE-only + oracle-agent RPC extension; no xemu-fork host source touched). Local xemu-Metal validation across 4 boots is green; Codex round-1 BLOCK with 4 findings → all adopted → round-2 PASS_WITH_FINDINGS → MINOR PARTIAL closed post-round-2.
- Validation state: **CLOSED — implementation + local validation + Codex iteration complete; canonical docs synced; commit is the final step.** Real-Xbox discriminator run is OUT OF SCOPE for cycle 23 per the bounded assignment.

## Gate status (cycle 23)

- [x] Fresh worker receipt posted before deeper work (19:18 CDT).
- [x] Design locked (19:24 CDT) — kseg0 scan + MmGetPhysicalAddress per-page gate + shared `reserved[0]/reserved[1]` filter set + HIGHEST-phys targeting + new `witness.scan` RPC on the agent. No fopen anywhere in the witness path.
- [x] Implementation: `xbed_a4_witness.{h,c}` (~170 lines) + `lib.mk` SRCS add + image-blit/main.c call sites (+26 lines) + oracle-agent `cmd_witness_scan` RPC (+127 lines) + main.c registration (+1 line).
- [x] Clean build of both XBEs (image-blit + oracle-agent); only nxdk-internal warnings, no warnings on cycle-23 code.
- [x] Local xemu-Metal validation across 4 boots: witness fires on every boot (`xbed_a4_witness: enter stage=1` + `enter stage=3` lines), scan correctly reports "no XCTR buffer found" on standalone xemu (no agent), `mapped_pages_seen=378` cold-boot / 77 warm-reboot.
- [x] Local xemu-Metal validation: image-blit's first-boot v0.4 pass=3/8 mask=0x31 tally UNCHANGED (no instrumentation regression).
- [x] Codex round 1: BLOCK with 4 findings — 2 BLOCKING (agent reader needs `MmGetPhysicalAddress` gate + filter parity with writer), 1 MEDIUM (writer first-match attribution ambiguous), 1 MINOR (header doc drift).
- [x] All round-1 findings adopted in full:
  - BLOCKING #1: `cmd_witness_scan` gains `MmGetPhysicalAddress` per-page gate via new `a4_reader_candidate_ok` helper.
  - BLOCKING #2: `cmd_witness_scan` applies same `reserved[0]/reserved[1]` filters as writer (lockstep documented in commands.c body comment + filter helpers extracted on both sides).
  - MEDIUM #3: writer changed from first-match to HIGHEST-phys-match (most recent agent allocation; deterministic across repeated runs in one power session).
  - MINOR #4: header `xbed_a4_witness.h` Safety notes rewritten + "first occurrence/first match" wording replaced with "HIGHEST-phys passing candidate" wording.
- [x] Codex round 2: PASS_WITH_FINDINGS — all 3 BLOCKING + MEDIUM RESOLVED, MINOR PARTIAL (residual header "first match" wording in 2 places).
- [x] MINOR PARTIAL closed post-round-2 via direct comment sync.
- [x] Validation marker written at `.claude/state/codex-validate-last-run`: `2026-05-23T01:02:05Z cycle 23 Path A.4 — codex-validate changes round 2 PASS_WITH_FINDINGS (all BLOCKING resolved, MEDIUM resolved, MINOR resolved via header comment sync after round 2)`.
- [x] Canonical docs synced (`handoff.md` cycle-23 entry, `decision-log.md` cycle-23 entry, orchestration-state quartet).
- [ ] Commit covering code + docs + ISOs + validation marker — pending; cycle-23 closure commit hash to be recorded in `handoff-summary.md` after commit lands.

## Codex validation decision

**Rule #15 trigger #2 (non-trivial uncommitted code in xemu-fork/ apple-silicon scripts > 30 lines) MET.** Ran `/codex-validate changes`. Round 1 verdict: BLOCK with 4 findings. Round 2 verdict (after adopting all round-1 findings): PASS_WITH_FINDINGS. Round-2 MINOR PARTIAL closed post-round-2 via direct comment sync. Validation marker written.

The codex-validate workflow this cycle used the local Codex CLI in read-only sandbox mode (`-s read-only`) per skill conventions; preflight confirmed Codex was logged in via ChatGPT (not API key); no destructive operations. Round-1 prompt + diff payload at `/tmp/codex-cycle23-prompt-*.txt` + `/tmp/codex-cycle23-diff-*.txt`; round-1 output at `/tmp/codex-cycle23-output-*.txt`. Round-2 equivalents at `/tmp/codex-cycle23-r2-*`.

## What stands from cycles 17 + 19 + 20 + 21 + 22

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged. Cycle 23 does NOT regress the local xemu pass=3/8 mask=0x31 baseline.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle 20 + cycle 21 empirical observation that image-blit's `fopen`-based markers (D:\\ then E:\\) produce ZERO files on real Xbox under `runxbe` chainload, with consistent 22.4 s chainload→FTP-back gap across 4 reproductions — unchanged; A.4 is the discriminator that targets this opaque failure with a fopen-free mechanism.
- Cycle 22 invalidation of cycle-19 hypothesis #1 (launch-path blocker) and cycle-21 hypothesis #3 (FATX-driver/NT-mount state divergence for D:\\) — unchanged; cycle 23 implements A.4 specifically because of those invalidations.

## What changes (cycle 23)

- Cycle-22 leading hypothesis (image-blit dies before main()'s first instruction) becomes testable on real Xbox via the A.4 witness mechanism. Cycle 23 itself does NOT run the test — it ships the instrumentation. The discriminating real-Xbox run belongs to cycle 24.

## Yes/no/inconclusive answer to the cycle-23 assignment question

**Cycle 23 was "ADD the witness." Answer: YES — witness added, local-validated, Codex-validated, doc-synced; ready for cycle-24 real-Xbox discriminator run.**

The discriminating result for cycle-22's leading hypothesis is PENDING cycle 24's real-Xbox run. Cycle 23's outcome shape was: ship the instrumentation + prove it works locally + Codex-validate before any real-Xbox run. All three are complete.
