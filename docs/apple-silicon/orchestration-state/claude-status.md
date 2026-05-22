# Claude Status

- Objective: cycle-17 bounded implementation slice for `XEMU_DIAG_PGRAPH_STATUS_DRAIN`.
- Status: **CLOSED — implementation shipped, image-blit 3/8 → 8/8 on Metal AND GL renderer-agnostically, Codex MINOR ISSUES adopted, durable docs synced.**
- Session: `hermes_xemu_live_20260522_132851`
- Started: 2026-05-22 13:28 CDT.
- Closed: 2026-05-22 14:01 CDT.

## Worker receipt

- **Docs read:** orchestration-workflow.md; current-cycle.md; claude-status.md; validation-status.md; handoff-summary.md; handoff.md (cycle-15 + cycle-13 sections); decision-log.md (cycle-15 entry + cycle-13 follow-up plan); renderer-state.md; flags-renderer.md.
- **Bounded slice objective:** add opt-in `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` so `pgraph_read(NV_PGRAPH_STATUS)` returns non-zero (busy) whenever `pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] != pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]`. Scope kept tight to `hw/xbox/nv2a/pgraph/pgraph.c`; opt-in only; no behavior change when unset.
- **Current hypothesis:** With the flag on, `pb_wait_until_gr_not_busy` in the guest will spin until PFIFO has drained the IMAGE_BLIT push, closing the cycle-13 dispatch race. image-blit v0.4 tally should flip from `pass=3/8 mask=0x31` → `pass=8/8 mask=0xff` on both renderers through the cycle-15 host-visible guest-log channel.
- **Final outcome:** hypothesis CONFIRMED. Metal flag-on = `pass=8/8 mask=0xff` across 4 boots; GL flag-on (under `XBE_HARNESS_TIMEOUT_SECONDS=120`) = `pass=8/8 mask=0xff` across 15 boots; baseline reruns on the same xemu binary reconfirm `pass=3/8 mask=0x31` without the flag.

## Closure summary

- Code shipped: ~50 lines in `hw/xbox/nv2a/pgraph/pgraph.c` + 2 lines in `hw/xbox/nv2a/nv2a_regs.h`; harness `XBE_HARNESS_TIMEOUT_SECONDS` override added in `xbe_renderers.py`.
- Docs synced: `automation.md`, `handoff.md`, `decision-log.md`, `xbe-harness/README.md`, `.claude/rules/flags-renderer.md`, `.claude/rules/flags-bench.md`, all orchestration-state files.
- Codex validation: ran `/codex-validate changes`; MINOR ISSUES (one medium control-plane drift, one low harness-README drift); both adopted in this slice.
- Next bounded step (cycle 18): cycle-11 follow-up item #3 — real-Xbox oracle parity check on `image-blit.iso` under the new flag. Decides default-on vs. properly published busy bit vs. default PFIFO barrier.

## Supervisor note

- Previous packaging slice closed cleanly; this new session exists to take the next highest-value bounded step immediately rather than leaving Claude idle.
