# Validation Status

## Structured summary
- State: RE-ALIGNED_TO_PLAN_BACKBONE (reconciled 2026-06-03; the 2026-06-01 BLOCKED_CONTROL_PLANE was a stale no-live-worker doc-prose state, superseded by the 2026-06-02 reliability re-architecture — see current-cycle.md and decision-log.md).
- Control-plane truth: Hermes supervises/orchestrates the project; Qwen is the primary implementation lane in policy; Codex is the independent validator/escalation lane; Claude Code is manual rollback only.
- Historical exact-lane finding: 50AH already answered the exact native-depth question with recovered evidence. Native depth draws occurred on the exact PGR2 lane, but the saved depth surface stayed uniformly white and executed sibling-sync completions stayed at zero.
- Live worker truth: no currently active 50AI or 50AM worker exists, so the control plane must not be summarized as having an ACTIVE canonical software lane.
- Current control-plane blocker: NONE (the 2026-06-01 stale-50AI/50AM-prose blocker was resolved by the 2026-06-02 re-architecture; blockers.md is CLEAR). The 2026-06-03 "Rainbow/M15 capture is human-gated" escalation was FALSE and is not a blocker: capture is in-emulator (XEMU_GL_SCREENSHOT_PATH / QMP screendump), works headless, needs no sudo/GUI bridge — see decision-log.md 2026-06-03.
- Current bounded recovery: this documentation-only slice reconciles the four live control-plane docs to the no-live-worker truth.
- Broader validation-plan exit criteria: REMAIN UNPROVEN.
- Serious user-testing readiness: REMAINS UNPROVEN.
- Repo cleanliness / narrow-scope truth: DO NOT CLAIM clean tree or single-file scope.
- Recommended family posture after reconciliation: CLOSED for this documentation/control-plane cleanup family, while the broader project remains unresolved and unproven.
- Last truth update: 2026-06-03.

## Detailed record
- Validation status now reflects the live Kanban truth instead of stale ACTIVE branch prose.
- The important validated point from 50AH is historical evidence recovery, not a new active implementation target.
- Routing policy remains stable even though no live worker is currently active: Hermes supervises, Qwen is the primary implementation lane in policy, Codex is the independent validator/escalation lane, and Claude Code is manual rollback only.
- This file intentionally distinguishes policy from live posture: policy still points coding through Qwen, but the current live posture is not-active because no 50AI or 50AM worker exists.
- This file intentionally does not claim that validation is complete, that the overall plan has exited, or that the project is ready for serious user testing.
- 2026-06-03 capture/classifier truth: gameplay/M15 evidence frames are captured in-emulator (renderer-native PNG), not via macOS screen capture — capture is NOT human-gated. A black Metal frame is now classified differentially vs the GL reference at the same guest moment (`content_class` in metal-gl-compare summary.json/report.md): METAL_GEOMETRY_GAP (authoritative only when state-aligned), METAL_GEOMETRY_GAP_UNVERIFIED, or EXPECTED_BLACK. Validators must read `content_class`; a black Metal frame is a renderer signal, never a capture/infra failure. This does not change the UNPROVEN status of renderer correctness or broader exit criteria.
