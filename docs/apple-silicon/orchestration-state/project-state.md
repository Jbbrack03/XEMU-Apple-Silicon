# Project State

- Control-plane version: 2026-06-01-qwen-primary-routing-no-live-worker.
- Updated at: 2026-06-01.
- Mission: make Apple Silicon the best platform for original Xbox emulation while preserving correctness, evidence quality, and merge discipline.
- Primary supervisor: Hermes.
- Primary implementation lane in policy: Qwen 35B A3B 4-bit via MLX for coding work.
- Local scout lane in policy: Qwen 27B via MLX.
- Independent validator / escalation lane: Codex.
- Claude Code: manual rollback only — not a coding lane.
- Current live posture: no active 50AI or 50AM worker is presently running, so do not describe the control plane as having an ACTIVE implementation lane right now.
- Hardware truth source for correctness-sensitive renderer/oracle claims: real Xbox evidence.

## Routing policy

- Qwen 27B: read-heavy scout / triage lane.
- Qwen 35B: primary implementation lane for coding work, including harder and architecture-sensitive slices, when a live worker is actually running.
- Codex: required independent validator for non-trivial diffs; escalation lane when Qwen work needs verification or when contradictions need independent resolution.
- Claude Code: manual rollback only. Not used for active coding or architecture slices.

## Context policy

- Default local coding cap: 64k.
- Normal escalation tier: 96k.
- 131k+: explicit escalation only.
- ~262k: exceptional rescue / synthesis mode only.

## Current orchestration priorities

1. Keep bounded slices and truthful state files.
2. Catch control-plane contradictions early.
3. Keep Codex validation in the loop for non-trivial work.
4. Route coding through the Qwen lane when a live implementation slice exists; use Codex for independent verification.
5. Reserve Claude Code for manual rollback scenarios only.
6. Do not overstate live activity when Kanban shows no current worker.
