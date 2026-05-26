# Orchestration State Contract

Purpose: keep the xemu Apple-Silicon control plane compact, truthful, and machine-checkable.

## Global rules

- Every state file must begin with a short structured summary before any long narrative.
- Use one truth state only when a field allows enumerated values.
- Allowed worker/slice states: ACTIVE, CLOSEOUT, CLOSED, BLOCKED_QUOTA, BLOCKED_AUTH, BLOCKED_PERMISSIONS, BLOCKED_CONTROL_PLANE, PAUSED.
- CLOSED means the closure commit already exists in git and no live worker is still finishing that same slice.
- CLOSEOUT means implementation is done but commit/state/doc sync is still being reconciled.
- ACTIVE means a live bounded slice is in progress right now.
- If there is no live slice, say so explicitly instead of implying ACTIVE by omission.
- Put durable truth in the summary block; put evidence, rationale, and history below it.
- When a field becomes unknown, write UNKNOWN explicitly.

## Required top sections by file

### project-state.md
- Control-plane version
- Updated at
- Mission
- Primary supervisor
- Primary worker lane
- Local lane default
- Independent validator
- Hardware truth source
- Routing policy
- Context policy
- Current orchestration priorities

### current-cycle.md
- State
- Active cycle
- Branch
- Commit truth
- Live worker
- Validation truth
- Next bounded slice
- Last truth update

### claude-status.md
- Worker lane
- State
- Active session
- Current objective
- Last completed slice
- Next expected action
- Blocker class
- Last truth update

### validation-status.md
- State
- Slice under validation
- Last green validation
- Codex status
- Hardware gate status
- Required before promotion/closure
- Last truth update

### blockers.md
For each active blocker record:
- State
- Blocker class
- First seen
- Affected lane
- Impact
- Recovery action
- Notify Josh

### handoff-summary.md
- Control-plane state
- Latest completed slice
- Active worker
- Next bounded slice
- Biggest caution
- Last truth update

## Writing rules

- Keep the structured summary under roughly 20 lines when possible.
- Put timestamps in ISO-like text when known.
- Mention session names, commits, and branches only in the summary fields where they matter.
- Long historical entries belong under a "Detailed record" or archive section.
- Do not leave stale phrases like "closure commit pending" after the commit exists.
