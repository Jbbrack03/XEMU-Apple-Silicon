# Handoff Summary

- Cycle 17 closed at commit `9024a548f7` (status-drain diagnostic, image-blit 3/8 → 8/8 mask=0xff on Metal AND GL renderer-agnostically; cycle-13 dispatch-race hypothesis confirmed).
- Cycle 18 was the packaging slice that closes the carry-forward orchestration-state diff Hermes left behind when opening this session. Scope was doc-only (quartet refresh + cycle-17 closure-hash record), matching the cycle-16 precedent (`3b5257a498`).
- **Next bounded slice (cycle 19):** real-Xbox oracle parity check on `image-blit.iso` under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` (cycle-11 follow-up item #3). This gates the default-on / long-term-fix decision (properly published busy bit vs. default PFIFO barrier vs. shipping the diagnostic as-is). Start in a fresh session; do not start it as a continuation of this packaging session.
- Pre-reqs already in place for cycle 19: `XEMU_DIAG_PGRAPH_STATUS_DRAIN` flag landed (`hw/xbox/nv2a/pgraph/pgraph.c`, `nv2a_regs.h`); host-visible guest-log channel (`XEMU_GUEST_LOG`, cycle 15) usable for tally readback; `XBE_HARNESS_TIMEOUT_SECONDS` override available for the long-latency GL leg; reference local evidence in `benchmark-runs/cycle17-status-drain-{metal-PASS-gl-timeout,gl-long-timeout}-20260522/`.
- Worktree state at handoff: clean after this packaging commit. No carry-forward dirty diff for the next session.
