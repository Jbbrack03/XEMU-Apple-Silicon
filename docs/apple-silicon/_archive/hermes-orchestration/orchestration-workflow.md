> **ARCHIVED / INACTIVE as of 2026-06-15.** This document describes the
> decommissioned Hermes orchestration model and the Codex external-validation
> step. Neither is in use. The project now operates with **Claude Code working
> solo** — Claude owns implementation, validation, orchestration, and handoffs
> directly (CLAUDE.md rule #15). Preserved as a reference design only; it may be
> revived in future, potentially with a different agent. Do not treat anything
> below as a current operating procedure. See `../../handoff.md` for current
> state.

# Claude Code Orchestration Workflow

Last updated: 2026-05-21 (Hermes supervision model formalized for the Apple
Silicon xemu fork). This document is the canonical workflow for using Claude
Code as the primary coding worker while Hermes acts as the orchestration,
validation, and handoff layer. It complements `handoff.md` (current state),
`metal-porting-workflow.md` (renderer-development loop), `automation.md`
(tooling and harnesses), and `oracle-workflow.md` (real-Xbox validation).

## 1. Purpose

The project goal is not merely to make Claude Code productive; it is to make
Claude Code productive without allowing context bloat, doc drift, false-green
validation, or unattended-session confusion to become the bottleneck.

The working model is:

- Claude Code owns deep implementation context inside its own session.
- Hermes owns orchestration, assignment framing, independent validation,
  handoffs, and escalation.
- Codex remains the required external validator before any non-trivial slice is
  treated as done.
- Real-Xbox oracle evidence remains mandatory anywhere renderer correctness is
  the acceptance gate.

Hermes must NOT attempt to mirror Claude's entire transcript into its own
context window. Hermes supervises through structured state and evidence, not by
replaying all worker thoughts.

## 2. Roles and boundaries

### 2.1 Claude Code (worker)

Claude Code is the primary implementation agent. It may:

- read the local codebase deeply;
- edit code, scripts, and docs;
- run builds, tests, and project tools;
- maintain local task-level reasoning over long windows;
- prepare status artifacts for Hermes.

Claude Code must NOT be treated as the final authority that a slice is done.
Terminal success alone is insufficient.

### 2.2 Hermes (orchestrator / quality gate)

Hermes owns:

- choosing the next bounded assignment;
- making sure Claude starts from the current project state;
- preventing context-window collapse by consuming only distilled artifacts;
- checking git state, logs, benchmark outputs, and validation artifacts;
- requiring Codex validation for non-trivial changes;
- requiring doc sync so canonical docs stay ahead of memory drift;
- escalating blockers or milestone completions to the user.

Hermes should supervise via deltas, summaries, run artifacts, and file-backed
state, not by tailing the entire Claude transcript forever.

### 2.3 Codex (independent validator)

Codex is the secondary reviewer. Before work is considered complete, run the
relevant Codex validation pass (`plan`, `changes`, or equivalent project review
step) and adopt or explicitly deflect findings in the docs.

### 2.4 Real Xbox oracle (hardware witness)

When the work touches renderer correctness, visual parity, controller-route
truth, or claims near M15/default-on quality, real-hardware evidence remains a
hard gate. The oracle is not optional just because local GL/Metal tests pass.

## 3. Context-management rule: artifacts over transcript

Hermes has a smaller context window than Claude Code. Therefore the official
rule is:

> Hermes must supervise Claude through compact, structured artifacts and
> evidence files, not through full-session transcript ingestion.

### 3.1 Forbidden supervision pattern

Do NOT build the orchestration loop around any of the following:

- repeatedly pasting Claude's full terminal transcript into Hermes;
- reading the complete Claude session from the beginning on every check;
- relying on one never-ending Hermes chat as the only project memory;
- treating raw build logs as the canonical handoff format.

### 3.2 Required supervision pattern

Instead, Hermes should read only what changed since the last check:

- current assignment;
- latest worker status summary;
- git diff summary;
- latest build/test result;
- latest benchmark or capture artifact;
- latest validation verdict;
- blocker summary;
- next proposed action.

This keeps Hermes bounded even while Claude's own session remains large.

## 4. Required orchestration artifacts

Store the orchestration state on disk so each Hermes pass can start fresh from
files rather than prior chat history.

Recommended project-local artifact set:

- `docs/apple-silicon/orchestration-state/project-state.md`
  - stable mission, active constraints, current strategic focus.
- `docs/apple-silicon/orchestration-state/current-cycle.md`
  - this cycle's bounded assignment, start time, owner, exit criteria.
- `docs/apple-silicon/orchestration-state/claude-status.md`
  - Claude's compact heartbeat: objective, hypothesis, files touched, tests,
    blockers, next step.
- `docs/apple-silicon/orchestration-state/validation-status.md`
  - independent checks: build/tests, Codex review, oracle/visual status,
    pass/fail/pending.
- `docs/apple-silicon/orchestration-state/blockers.md`
  - issues requiring user input or cross-machine action.
- `docs/apple-silicon/orchestration-state/handoff-summary.md`
  - end-of-cycle summary for the next fresh Hermes pass.

The exact filenames may evolve, but the workflow requirement does not: project
state must live in durable artifacts, not only in model context.

## 5. Required status shape for Claude Code

Claude's status updates must stay compact and structured. A good heartbeat is:

- Objective
- Current hypothesis
- Files changed
- Commands/tests run
- Evidence produced
- Blockers / uncertainties
- Next proposed action
- Confidence / risk notes

The status file should be short enough for Hermes to re-read repeatedly without
wasting context budget.

## 6. Single supervised cycle (human-attended)

Use this when validating a new loop design or when the slice is risky.

1. Hermes reads `handoff.md`, `strategy.md`, and the active workflow docs.
2. Hermes selects one bounded assignment with explicit exit criteria.
3. Hermes launches Claude Code in the project workspace.
4. Hermes checks quota early (`/usage`) before asking for substantial work.
5. Claude performs the slice and updates `claude-status.md` periodically.
6. Hermes monitors only compact evidence:
   - status artifact;
   - recent terminal tail when needed;
   - git diff summary;
   - latest build/test/benchmark outputs.
7. When Claude claims completion, Hermes requires:
   - independent build/test confirmation;
   - Codex validation for non-trivial changes;
   - doc sync into canonical project docs;
   - oracle/visual validation when renderer correctness is involved.
8. Hermes records the outcome in `handoff-summary.md` and the canonical docs.
9. Hermes ends the worker session cleanly before starting the next major loop.

## 7. Long-running unattended orchestration

The unattended model must use many fresh Hermes passes, not one infinitely
accumulating conversation.

Official unattended loop:

1. A fresh Hermes run starts.
2. It reads the orchestration-state files plus repo/git status.
3. It checks whether Claude is already running.
4. If Claude is idle, Hermes launches a bounded assignment.
5. If Claude is running, Hermes reads only the latest state artifacts and a
   short terminal tail if the status looks stale.
6. Hermes validates milestone evidence as it appears.
7. Hermes writes back a compact summary and updated status files.
8. Hermes exits.
9. The next scheduled Hermes pass resumes from files, not chat history.

This design is mandatory for durable unattended operation because it prevents
context-window exhaustion and makes recovery from interruptions straightforward.

## 8. Monitoring discipline

Hermes should prefer the following evidence order:

1. structured status files;
2. git diff / changed-file summary;
3. latest targeted test output;
4. latest benchmark/capture/oracle artifact;
5. short tail of the Claude terminal only if the structured artifacts are
   stale, missing, or contradictory.

Avoid full-log reads unless debugging the orchestration machinery itself.

## 9. Validation gates

A slice is not done until all applicable gates are green.

### 9.1 Always required

- explicit exit criteria met;
- git diff reviewed;
- relevant local builds/tests rerun;
- canonical docs updated when behavior, workflow, or evidence changed.

### 9.2 Required for non-trivial implementation work

- Codex validation completed;
- findings adopted or explicitly documented as deferred/out-of-scope.

### 9.3 Required for renderer-correctness claims

- local GL-vs-Metal evidence is coherent;
- artifact set is preserved in run directories;
- real-Xbox oracle workflow used where the acceptance gate requires it;
- visual parity is judged from aligned gameplay/keyframe evidence, not from a
  single convenient screenshot or terminal success.

## 10. Permission model

Because this repo uses Claude Code as an autonomous worker, the launch mode may
allow permission bypass when the user has explicitly authorized it.

Official rule for this project:

- supervised dry runs may launch Claude Code with permission bypass when the
  user has explicitly granted that authorization;
- unattended runs may do the same only within the bounded project workspace and
  assignment scope defined by Hermes;
- destructive or scope-expanding actions still require explicit user intent at
  the orchestration level.

Permission bypass is a throughput tool, not a substitute for bounded scope,
validation, or review.

## 11. Notifications and escalation

Notify the user on Telegram when any of the following occur:

- Claude is blocked and cannot continue autonomously;
- a milestone is reached and independent validation is ready for review;
- a validation gate fails after a meaningful attempt;
- the worker appears stuck, looping, or drifting from the assignment;
- human judgment is required for a risky branching decision.

The terminal may be unattended, so important orchestration events should not
rely on the terminal alone.

## 12. Anti-drift rules

To keep the orchestration system trustworthy:

- do not let session memory outrun the canonical docs;
- sync docs after meaningful workflow or evidence changes;
- prefer append-only evidence and explicit handoffs over ad hoc recollection;
- do not promote a provisional workaround into project truth without capturing
  it in the docs and validation artifacts;
- do not allow the worker to self-certify completion without external checks.

## 13. Minimal per-cycle checklist

Before starting a cycle:

- read `handoff.md`;
- read the relevant workflow doc(s);
- define one bounded assignment;
- define exit criteria;
- verify whether permission bypass is in scope.

During the cycle:

- keep Claude's status artifact current;
- monitor deltas, not transcripts;
- inspect only the evidence needed to decide the next move.

Before closing the cycle:

- verify builds/tests/evidence;
- run Codex validation if applicable;
- update canonical docs;
- write the compact handoff summary;
- send Telegram notice if blocked or materially complete.

## 14. Bottom line

Claude Code gets the long implementation window.
Hermes gets the dashboard.
The filesystem gets the durable memory.
Validation artifacts get the final vote.

That is the only model that scales to long-running unattended orchestration
without collapsing under context-window pressure.
