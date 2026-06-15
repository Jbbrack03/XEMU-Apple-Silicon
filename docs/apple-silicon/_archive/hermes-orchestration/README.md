# ARCHIVED — Hermes orchestration (INACTIVE as of 2026-06-15)

This directory holds the **decommissioned** Hermes multi-agent orchestration
layer and its associated Codex external-validation workflow. **None of it is
active.** As of 2026-06-15 the project operates with **Claude Code working solo**
— Claude owns implementation, validation, orchestration, and handoffs directly.
There is no Hermes orchestrator and no Codex second-opinion step in the current
workflow.

These files are preserved (not deleted) because the orchestration framework may
be revived in the future, potentially driven by a different agent. They are kept
here as a reference design only — do **not** treat anything in this directory as
a current operating procedure.

## Contents

- `orchestration-workflow.md` — the former canonical workflow describing Claude
  Code as primary worker with Hermes as orchestration/validation/handoff layer
  and Codex as required external validator.
- `orchestration-state/` — the former live state-handoff scaffolding
  (`current-cycle.md`, `blockers.md`, `claude-status.md`, `handoff-summary.md`,
  `project-state.md`, `state-contract.md`, `validation-status.md`,
  `successor-packets/`). These were the orchestrator's working files; they are
  now frozen snapshots, not live state.

## Current workflow

See `../../handoff.md` (current state, next actions) and the project
`CLAUDE.md` working rules. Checks-and-balances and orchestration are Claude
Code's own responsibility per CLAUDE.md rule #15.
