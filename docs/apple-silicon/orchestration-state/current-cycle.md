# Current Cycle

- Cycle: 17 (**CLOSED 2026-05-22 — bounded implementation of `XEMU_DIAG_PGRAPH_STATUS_DRAIN` shipped + validated renderer-agnostically**).
- Started: 2026-05-22 13:28 CDT.
- Closed: 2026-05-22 14:01 CDT.
- State: CLOSED.
- Owner: Claude Code (hermes_xemu_live_20260522_132851).
- Bounded goal: implement the opt-in `XEMU_DIAG_PGRAPH_STATUS_DRAIN` diagnostic path so `NV_PGRAPH_STATUS` stays visibly busy while PFIFO has queued work, then validate whether `image-blit` flips from the current partial mask to all 8 cells green through the existing host-visible guest-log channel.
- Outcome: MET. image-blit `pass=3/8 mask=0x31` → `pass=8/8 mask=0xff` on Metal (4 boots) AND GL (15 boots) under the flag; cycle-13 race hypothesis definitively confirmed. Codex `changes` review returned MINOR ISSUES; both adopted before close.

## Exit criteria — all met

1. ✅ Posted fresh worker receipt after re-reading the canonical docs and updated this file plus `claude-status.md`.
2. ✅ Made one bounded implementation attempt for `XEMU_DIAG_PGRAPH_STATUS_DRAIN` within the workspace only; scope kept tight to the PFIFO/PGRAPH busy-publication path. Two failed iterations on the eligibility gate are preserved as evidence.
3. ✅ Concrete validation evidence captured through the host-visible guest-log channel on both Metal (`benchmark-runs/cycle17-status-drain-metal-PASS-gl-timeout-20260522/`) and GL (`benchmark-runs/cycle17-status-drain-gl-long-timeout-20260522/`); baseline reruns also captured for diff.
4. ✅ Mandatory Codex validation completed; both adopted findings recorded in the decision-log.
5. ✅ `validation-status.md`, `handoff-summary.md`, `decision-log.md`, and `handoff.md` synced before closure.

## Next bounded slice (NOT started in cycle 17)

- Cycle-11 follow-up item #3: real-Xbox oracle parity check on `image-blit.iso` under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1`. Gates the long-term-fix decision (default-on vs. properly published busy bit vs. default PFIFO barrier).

## Supervisor start note (2026-05-22)

- Prior slice status verified: cycle 16 was already CLOSED in the artifacts, the repo was clean, and the worker was sitting at a closed-session prompt. That session was intentionally retired to preserve clean context boundaries.
- Fresh-session requirement: read the canonical docs first, then give a short worker receipt before implementation.

## Worker receipt (2026-05-22 — fresh session, cycle 17 open)

- **Docs read:** orchestration-workflow.md; current-cycle.md; claude-status.md; validation-status.md; handoff-summary.md; handoff.md (cycle-15 + cycle-13 sections); decision-log.md (cycle-15 entry + cycle-13 follow-up plan); renderer-state.md; flags-renderer.md.
- **Bounded slice objective:** add opt-in `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` so `pgraph_read(NV_PGRAPH_STATUS)` returns non-zero (busy) while `pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] != pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]`. Scope: surgical edit confined to `hw/xbox/nv2a/pgraph/pgraph.c` with an `NV_PGRAPH_STATUS` constant where appropriate; no behavior change when the env var is unset.
- **Current hypothesis:** With the diagnostic flag on, `pb_wait_until_gr_not_busy` in the guest will now actually spin until the PFIFO puller has drained the IMAGE_BLIT push, closing the cycle-13 dispatch-race window. image-blit v0.4 tally should flip from `pass=3/8 mask=0x31` → `pass=8/8 mask=0xff` on both Metal and GL legs.
- **First concrete action:** patch `pgraph_read` to add a `pgraph_status_drain_enabled()` cached env-var read (mirroring `pgraph_fast_read_enabled()` at pgraph.c:95-113) and a special case at PGRAPH-relative offset `0x700` (NV_PGRAPH_STATUS = 0x00400700 − 0x00400000). Build via `./build.sh -a arm64`.
- **Planned validation path:** run `image-blit.iso` through `scripts/apple-silicon/xbe-harness` with `XEMU_GUEST_LOG=1 XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` on Metal first, then GL. Read tally lines from `xemu.log` via the cycle-15 host-visible guest-log channel (`xemu-guest-log: image-blit: tally pass=N/8 mask=0xXX`). Expected: 8/8 on both legs. Compare baseline (flag off, already characterised in cycle-15 evidence) versus with-flag for a clean delta. Codex `/codex-validate changes` mandatory before close.

## Lessons captured in cycle 17

- A naive `PUT != GET` check hangs the guest's pbkit busy poll forever during BIOS / pbkit early-init, because the pusher's stall conditions (`NV_PGRAPH_FIFO_ACCESS` off, `pgraph.waiting_for_nop` set, etc.) leave `DMA_GET` stuck without `DMA_PUT` having been advanced for the same reason. The diagnostic MUST gate on the same eligibility set as `pfifo_run_pusher`. Documented in the source comment block at `pgraph_read` cycle-17 branch + `decision-log.md` cycle 17 entry.
- The default 35 s xbe-harness window assumes the existing flag baseline; adding wait latency per `pb_wait_until_gr_not_busy()` call makes the GL leg's BIOS/pbkit boot exceed the window. `XBE_HARNESS_TIMEOUT_SECONDS` is the durable override knob (env-var only, no CLI flag) — documented in the harness README and `flags-bench.md`.
