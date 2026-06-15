# Current Cycle

## Structured summary
- State: RE-ALIGNED_TO_PLAN_BACKBONE (reliability re-architecture applied 2026-06-02; supervisor stable, ~9h since restart).
- Routing-policy truth: Hermes supervises/orchestrates; Qwen is the primary implementation lane, now bounded by a deterministic Codex-escalation backstop and an investigation depth-cap; Codex (xemu-codex-validator) is the validator/escalation lane and builds foundation tooling; Claude Code is manual rollback only.
- Board truth: the prior 252-task PGR2 "50-series" micro-hypothesis spiral was archived 2026-06-02 (reversible; findings preserved). The board now carries the plan backbone, not the drift.
- Correctness-gate truth: visual correctness is decided by the objective frame oracle (scripts/apple-silicon/xemu_frame_correctness_oracle.py) plus metric-vs-golden — NEVER an LLM "looks right" judgment. F1 (wire the oracle as the gate) is DONE.
- Autonomous-oracle truth: the headless real-Xbox game-test loop is PROVEN end-to-end (OGX360 input -> Crimson Skies gameplay capture -> iND-BiOS IGR Back+Start+LT+RT exit -> dashboard return, no human). Evidence: oracle-evidence/igr-exit-proven-20260602.json.
- PGR2 truth: no blind PGR2 reruns (per strategy.md); the depth/sibling-sync bug is re-approached only via B1 (capture the hard geometry -> synthetic spec-oracle XBE). PGR2 does not run on the oracle.
- Backbone status: F1 done; M1 (track M15 bundle gate) done; M2 (close the 4 missing evidence-only M15 links) in progress; F2 (deterministic guest-vblank capture/alignment) in development on the Mac worktree; O1 (real-Xbox golden reference) BLOCKED pending a one-time human golden curation; B1/B2 gated on F1+F2(+O1).
- Progress measure: track scripts/apple-silicon/m15-bundle-status.py (the real gate), not a subjective percentage.
- Readiness truth: the Metal renderer-correctness problem (garbled retail output) remains unproven/open; broader validation-plan exit and serious user-testing readiness remain explicitly unproven.
- Last truth update: 2026-06-02.

## Detailed record
- 2026-06-02 reliability re-architecture: hung-worker stale-reclaim lowered 4h->90min; the active kanban board was switched to xemu (the orchestrator had been defaulting to an empty board); chronically-failing Qwen tasks now auto-escalate to the Codex lane (deterministic cron + architect rule); the kanban_comment task_id fallback bug was fixed.
- The 67 open PGR2 "50-series" drift tasks were archived and the board re-aligned to the plan backbone in diagnostic-xbe-plan.md (Phase 3 oracle, Phase 4 XBE wave) + the M15 evidence-bundle gate.
- The architect prompt was hardened: anchor every slice to a plan milestone / the M15 gate; no blind PGR2 reruns; an investigation depth-cap; visual correctness is the frame oracle's verdict, never an LLM eyeball.
- Real-Xbox oracle enablement: the XEMU test-HDD per-game profiles were extracted (FATX) and copied to the oracle's E:\UDATA/TDATA so the recorded controller automations reach gameplay; Crimson Skies profile (4d530021) confirmed present and loading.
- Autonomous game-test exit solved: the OGX360-driven IGR combo (iND-BiOS IGRMODE=2 / IGRLOADSDASH=1) reliably returns the headless console to the dashboard; exercised by a full Crimson Skies retail-gameplay-oracle run (bridge-readback PASS, composite MS2109 capture, dashboard_returned=true; 82/91 captured frames pass the correctness oracle).
- This file reflects the post-re-architecture truth; the autonomous orchestration continues to advance the backbone (F2, M2) and will update this state as tasks close.
- Nothing here proves project exit criteria, renderer-correctness, or end-user readiness.
