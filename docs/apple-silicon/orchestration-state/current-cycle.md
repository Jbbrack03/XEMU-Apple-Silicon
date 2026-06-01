# Current Cycle

## Structured summary
- State: BLOCKED_CONTROL_PLANE.
- Live worker truth: no currently active 50AI or 50AM worker is running in Kanban, so do not treat either lane as live/active.
- Routing-policy truth: Hermes supervises/orchestrates; Qwen remains the primary implementation lane in policy; Codex remains the independent validator/escalation lane; Claude Code remains manual rollback only.
- Historical exact-question status: 50AH already answered the exact native-depth question with recovered evidence; treat it as historical rather than an active implementation target.
- Current blocker: the control-plane docs had been overstating an ACTIVE 50AI/50AM story after the live worker disappeared.
- Immediate bounded action: this documentation-only reconciliation retires the stale active-lane wording across the four live control-plane docs.
- Readiness truth: broader validation-plan exit and serious user-testing readiness remain explicitly unproven.
- Repo truth: do not treat the repo as clean or as single-file-scoped; this slice only reconciles four control-plane docs.
- Family recommendation: after this reconciliation, the 48H documentation/control-plane family should remain not-active and be classified CLOSED rather than ACTIVE, because the stale prose is repaired and no further live-worker claim remains in these docs.
- Last truth update: 2026-06-01.

## Detailed record
- Hermes remains the supervisor/orchestrator for the Apple Silicon xemu control plane.
- Qwen remains the primary implementation lane in routing policy, but this file no longer claims that a live Qwen worker is presently active.
- Codex remains the independent validator and escalation lane when recovery or contradiction resolution is needed.
- Claude Code remains reserved for manual rollback only and is not an active implementation lane.
- Recovered 50AH evidence already established that native depth draws occur on the exact PGR2 lane, while the saved depth surface stayed uniformly white and executed sibling-sync completions stayed at zero. That exact native-depth question is therefore historical/already answered for control-plane purposes.
- Current truthful posture is not-active: no live 50AI worker and no live 50AM worker are currently present, so the control-plane state must not be described as ACTIVE.
- This reconciliation is documentation-only and intentionally does not modify code, scripts, or benchmarks.
- Nothing in this control-plane rewrite proves project exit criteria, validation-plan completion, or serious end-user readiness.
