# Blockers

- State: CLEAR.
- Active blocker count: 0.
- Last reviewed: 2026-05-26.
- Notify Josh: only when a blocker is active and materially affects progress or requires his intervention.

## Active blockers

- None.

## Resolved blockers

### Cycle 45D initial Qwen receipt miss
- State: RESOLVED
- Blocker class: control-plane
- First seen: 2026-05-26T19:49:00Z
- Resolved at: 2026-05-26T19:50:59Z
- Affected lane: Qwen-35B
- Impact: the first cycle 45D implementation launch stayed process-alive but failed to emit its required receipt promptly, so Hermes terminated the unconfirmed Qwen session and rotated the same bounded slice onto a live Codex fallback before leaving the control plane ACTIVE.


### Cycle 44B Qwen result-discipline failures
- State: RESOLVED
- Blocker class: control-plane
- First seen: 2026-05-26T16:29:00Z
- Resolved at: 2026-05-26T16:34:12Z
- Affected lane: Qwen-35B
- Impact: the cycle 44B implementation objective wrote receipts and even a partial `composite_preflight.py` diff, but Qwen exited twice without the required result artifact, so Hermes rotated the same bounded slice onto a live Codex fallback instead of leaving a false ACTIVE Qwen state behind.

### Cycle 43A stale ACTIVE control-plane summary
- State: RESOLVED
- Blocker class: control-plane
- First seen: 2026-05-26
- Resolved at: 2026-05-26
- Affected lane: Codex fallback
- Impact: the compact orchestration state still implied an ACTIVE slice even though the implementation worker had already finished and only bounded closeout work remained.
- Recovery action taken: Hermes reopened the slice as CLOSEOUT instead of ACTIVE, and the final closeout pass resynced the control-plane docs to the closed 43A truth after bounded diff review and validation reruns.
- Notify Josh: no
- Notes: no active blocker remains; cycle 43A is closed rather than awaiting another live worker.

### Qwen 42K result-discipline failure
- State: RESOLVED
- Blocker class: control-plane
- First seen: 2026-05-26T00:03:00Z
- Resolved at: 2026-05-26T01:18:43Z
- Affected lane: Qwen-35B
- Impact: the relaunched 42K worker proved the receipt path works, but then exited on max-turns without `.claude/state/cycle42k-result.md` or any source diff, so the slice could not be treated as an active or completed implementation pass.
- Recovery action taken: Hermes rotated the same bounded EEPROM-breadcrumb objective to a fresh Codex fallback worktree and confirmed a new receipt artifact landed there before restoring ACTIVE state.
- Notify Josh: no
- Notes: keep the next implementation attempt off Qwen for this same bounded objective unless the assignment is narrowed further.

### Claude review auth smoke (mitigated by Codex review rotation)
- State: MITIGATED
- Blocker class: auth
- First seen: 2026-05-26T20:27:46Z
- Mitigated at: 2026-05-26T20:27:46Z
- Affected lane: Claude Code
- Impact: a fresh `claude-max-bypass` smoke returned `Not logged in`, so Hermes did not trust Claude for the 45D review handoff and immediately launched a Codex review continuation instead; this is lane-health debt, not an active project blocker.
