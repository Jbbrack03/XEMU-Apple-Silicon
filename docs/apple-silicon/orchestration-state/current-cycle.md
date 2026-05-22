# Current Cycle

- Cycle: 13 (CLOSED — §H.6 `image-blit` residual code-path audit + state-sync closure; doc-only slice).
- Started: 2026-05-22 11:01 CDT.
- Closed:  2026-05-22 (this commit).
- State: CLOSED / cycle-13 doc-only audit landed cleanly with all load-bearing citations spot-verified.
- Owner: Claude Code (hermes_xemu_live_20260522_110134).
- Outcome: doc-only diff — `docs/apple-silicon/{handoff.md, decision-log.md, orchestration-state/*}`. Zero `xemu-fork/hw/` and zero `xemu-fork/scripts/apple-silicon/` content. §H.6 `image-blit` residual reframed from cycle-12's "guest CPU cache-coherency" framing to a **PFIFO ↔ vCPU dispatch race against the never-published `NV_PGRAPH_STATUS`** register, with the default-on `XEMU_PGRAPH_FAST_READ` returning the zero-initialized bit via `__ATOMIC_RELAXED`. Cycle-12 conclusions remain valid in direction; cycle-13 sharpens the mechanism.
- Codex validation NOT required: doc-only slice, no source change, rule #15 trivial-doc exemption applies.
- Slice commit hash: `<closure-hash-tbd>` (recorded in the follow-up state-sync commit).
- Next bounded slice (NOT started in this cycle): cycle 14 — GL-leg replay of `image-blit.iso` through `scripts/apple-silicon/xbe-harness`. The dispatch-race hypothesis predicts a similar PASS/FAIL split on GL renderer-agnostically; that single experiment is the cheapest, sharpest confirmation. See `decision-log.md` 2026-05-22 cycle-13 entry "Cycle-14 next-slice plan" for the candidate diagnostic flag (`XEMU_DIAG_PGRAPH_STATUS_DRAIN=1`).
