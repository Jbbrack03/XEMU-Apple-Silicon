# Project State

- Control-plane version: 2026-05-25-qwen-local-lane.
- Updated at: 2026-05-25.
- Mission: make Apple Silicon the best platform for original Xbox emulation while preserving correctness, evidence quality, and merge discipline.
- Primary supervisor: Hermes.
- Primary long-context worker lane: Claude Code.
- Default local coding lane: Qwen 35B A3B 4-bit via MLX for bounded supervised tasks.
- Local scout lane: Qwen 27B via MLX.
- Independent validator: Codex.
- Hardware truth source for correctness-sensitive renderer/oracle claims: real Xbox evidence.

## Routing policy

- Qwen 27B: read-heavy scout / triage lane.
- Qwen 35B: default local coding lane for bounded Python tooling and workflow/plumbing work in disposable worktrees.
- Claude Code: primary lane for harder, broader, or architecture-sensitive slices.
- Codex: required independent review for non-trivial diffs and fallback for architecture-sensitive work.

## Context policy

- Default local coding cap: 64k.
- Normal escalation tier: 96k.
- 131k+: explicit escalation only.
- ~262k: exceptional rescue / synthesis mode only.

## Current orchestration priorities

1. Keep bounded slices and truthful state files.
2. Catch control-plane contradictions early.
3. Keep Codex validation in the loop for non-trivial work.
4. Promote the Qwen lane by verified task class, not by hype.
5. Preserve Claude Code for the harder and more architectural slices.
