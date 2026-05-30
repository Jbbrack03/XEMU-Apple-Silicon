# Project State

- Control-plane version: 2026-05-30-qwen-primary-routing.
- Updated at: 2026-05-30.
- Mission: make Apple Silicon the best platform for original Xbox emulation while preserving correctness, evidence quality, and merge discipline.
- Primary supervisor: Hermes.
- Primary implementation lane: Qwen 35B A3B 4-bit via MLX for all coding work.
- Local scout lane: Qwen 27B via MLX.
- Independent validator / escalation lane: Codex.
- Claude Code: manual rollback only — not a coding lane.
- Hardware truth source for correctness-sensitive renderer/oracle claims: real Xbox evidence.

## Routing policy

- Qwen 27B: read-heavy scout / triage lane.
- Qwen 35B: primary implementation lane for all coding work, including harder and architecture-sensitive slices.
- Codex: required independent validator for non-trivial diffs; escalation lane when Qwen work needs verification.
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
4. Route all coding through the Qwen lane; use Codex for independent verification.
5. Reserve Claude Code for manual rollback scenarios only.
