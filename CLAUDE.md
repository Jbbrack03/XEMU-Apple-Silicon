# xemu-fork — Project Memory (Apple Silicon Performance Fork)

QEMU/xemu source tree for the Apple Silicon performance fork. Working branch: `apple-silicon-performance`. Upstream remote: `upstream` (xemu-project/xemu). Origin remote: `origin` (the user's fork).

Workspace-level rules, test assets, working-rules, build entrypoints, skills, and stop-hook docs live in `../CLAUDE.md` (the workspace-root CLAUDE.md). When Claude is launched from any directory inside this workspace, the directory walk loads `../CLAUDE.md` automatically alongside this file — **do not duplicate that content here**.

## Where the canonical project documentation lives

Every Apple Silicon-specific decision, benchmark, and handoff lives under `docs/apple-silicon/`. Read first every session:

- `docs/apple-silicon/handoff.md` — current state, source-code changes made, next-session checklist.
- `docs/apple-silicon/decision-log.md` — append-only decisions with rationale and supersession markers.
- `docs/apple-silicon/strategy.md` — phased plan (Phase 0 baseline → Phase 4 native Metal renderer).
- `docs/apple-silicon/automation.md` — **canonical reference for all `XEMU_*` runtime flags and `xemu-perf:` counters.** Benchmark harness, scripted-input format, snapshot workflow.
- `docs/apple-silicon/research.md` — evidence base; cites local source references and public xemu issues.
- `docs/apple-silicon/benchmarking.md` — measurement matrix and retail performance gates.
- `docs/apple-silicon/benchmarks/<date>-<name>.md` — dated session notes, one per benchmark session. Add a new file for each meaningful run; do not edit older notes.

For Metal renderer work, read after `handoff.md` when the task touches the Metal port: `metal-porting-workflow.md` first (Apple-aligned operating loop: validate → capture → classify → optimize → re-measure), then `metal-renderer-plan.md` (slices M0-M15 + validation gates), `metal-api-reference.md`, `emulator-metal-survey.md`, `tooling-gap-plan.md`.

For diagnostic-XBE library / real-Xbox oracle work: `nv2a-feature-surface-research.md`, `diagnostic-xbe-plan.md` v2, `real-xbox-oracle-feasibility.md` (superseded by Phase 1+2+3.0 implementations), `controller-injection-research.md`, `retail-title-patching-strategy.md`, `retail-gameplay-software-paths.md`, `tier2-kernel-shim-viability.md`, `oracle-workflow.md`.

For input work: `macos-input-research.md`.

## Path-scoped subsystem rules

When Claude reads files matching specific paths in this fork, the rules at `../.claude/rules/` auto-load with relevant subsystem detail:

- `renderer-metal.md` — when touching `hw/xbox/nv2a/pgraph/mtl/**`, `ui/xemu-metal*`, or `docs/apple-silicon/metal-*.md`.
- `renderer-state.md` — when touching `handoff.md`, `benchmarks/**`, or `benchmark-runs/**`.
- `oracle-and-xbe.md` — when touching `scripts/apple-silicon/xbe-tests/**`, oracle/retail/controller scripts, or `tools/xemu-capture/**`.
- `flags-renderer.md`, `flags-tcg.md`, `flags-audio.md`, `flags-input.md`, `flags-bench.md` — when touching matching source/scripts. Each is a 1-line-per-flag index pointing back to `docs/apple-silicon/automation.md` for full descriptions.

When making renderer changes, prefer searching the existing tree (commits + the file list in `renderer-metal.md` can drift; rule #1 forbids guessing).

## Cross-subsystem citation hygiene

Adding a new runtime flag? Update `docs/apple-silicon/automation.md` (canonical), append a 1-line entry to the relevant `../.claude/rules/flags-*.md`, and add counters to `scripts/apple-silicon/extract-perf-summary.sh`. Rule #4: no doc drift.
