# Blockers

- State: CLEAR.
- Active blocker count: 0.
- Last reviewed: 2026-05-26.
- Notify Josh: only when a blocker is active and materially affects progress or requires his intervention.

## Active blockers

- None.

## Resolved blockers

### Cycle 45E stale oracle-agent deployment
- State: RESOLVED
- Blocker class: control-plane
- First seen: 2026-05-26T20:42:53Z
- Resolved at: 2026-05-26T20:50:27Z
- Affected lane: supervisor-owned real-Xbox validation path
- Impact: the first cycle 45E run-diag post-JSON attempt failed because the deployed Xbox-side oracle-agent still ignored the JSON flag and returned legacy text payloads, which made the new collector report post-json-readback-failed.
- Resolution: Hermes rebuilt scripts/apple-silicon/xbe-tests/oracle-agent, uploaded the fresh XBE to /E/Apps/oracle-agent/default.xbe, verified live JSON responses, and reran the witness-only validation successfully.

### Cycle 45D initial Qwen receipt miss
- State: RESOLVED
- Blocker class: control-plane
- First seen: 2026-05-26T19:49:00Z
- Resolved at: 2026-05-26T19:50:59Z
- Affected lane: Qwen-35B
- Impact: the first cycle 45D implementation launch stayed process-alive but failed to emit its required receipt promptly, so Hermes terminated the unconfirmed Qwen session and rotated the same bounded slice onto a live Codex fallback before leaving the control plane ACTIVE.
