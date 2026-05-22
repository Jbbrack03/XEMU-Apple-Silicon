# Validation Status

- Active slice: cycle-13 §H.6 `image-blit` residual code-path audit + state-sync closure (doc-only slice).
- Validation state: **CLOSED — all closure gates met.**

## Closure gates

- [x] Fresh worker receipt posted after canonical-doc read (handoff.md, decision-log.md, orchestration-workflow.md, four orchestration-state files).
- [x] Dirty files inspected and confirmed doc-only: `docs/apple-silicon/{handoff.md, decision-log.md, orchestration-state/*}`. Zero `xemu-fork/hw/` content. Zero `xemu-fork/scripts/apple-silicon/` content.
- [x] All load-bearing audit citations spot-verified against the live tree:
  - `grep -rn '0x400700\|PGRAPH_STATUS' hw/xbox/nv2a/` returns ZERO hits (PGRAPH_STATUS is never published anywhere under `hw/xbox/nv2a/`).
  - `hw/xbox/nv2a/nv2a.c:247-248` — `qemu_thread_create("nv2a.pfifo_thread", pfifo_thread, ...)`.
  - `hw/xbox/nv2a/pfifo.c` — `pfifo_write` ~84-110 → `pfifo_kick` ~112-115 (`qemu_cond_broadcast(&d->pfifo.fifo_cond)`); `pfifo_run_puller` ~226-272 acquires `d->pgraph.lock` and dispatches `pgraph_method`.
  - `hw/xbox/nv2a/pgraph/pgraph.c:115-150` — `pgraph_read` fast path returns `qatomic_read(&pg->regs_[addr])` with no acquire barrier.
  - `hw/xbox/nv2a/pgraph/mtl/{blit.c:215-220, renderer.c:2525}` — `perform_blit_cpu` CPU memcpy + `.image_blit = pgraph_mtl_image_blit` registration.
  - `include/qemu/atomic.h:77-84` — `qatomic_read` = `__atomic_load_n(..., __ATOMIC_RELAXED)`.
  - `nxdk/lib/pbkit/outer.h:461-462` — `NV_PGRAPH_STATUS = 0x00400700`, `NV_PGRAPH_STATUS_NOT_BUSY = 0`.
  - `nxdk/lib/pbkit/pbkit.c:486-494` — `pb_wait_until_gr_not_busy` busy-poll body.
  - `nxdk/lib/pbkit/pbkit_dma.c:55-58` — `pb_agp_access` returns `fb | AGP_MEMORY_REMAP`.
  - `XEMU_PGRAPH_FAST_READ` default-on per `.claude/rules/flags-renderer.md` and `.claude/rules/renderer-state.md` (closed slice).
- [x] Clean doc-only commit landed for the cycle-13 audit. Slice commit hash: `0bd85f70fe` (recorded in this follow-up state-sync commit; the closure commit cannot reference its own SHA).
- [x] `current-cycle.md`, `claude-status.md`, `validation-status.md`, `handoff-summary.md` resynced to durable CLOSED records before close (Codex HIGH finding from cycle-12 respected — no ACTIVE/PENDING scaffolding in git history).
- [x] Repo left clean at slice close (final `git status --short` expected empty after the state-sync follow-up commit).
- [N/A] Codex validation: doc-only slice, no `xemu-fork/hw/` or `xemu-fork/scripts/apple-silicon/` source change → rule #15 trivial-doc exemption applies.
- [N/A] Visual / real-Xbox oracle validation: doc-only audit, no new renderer claim. Real-Xbox oracle parity check is staged for cycle-14 follow-on once the diagnostic flag flips all 8 cells green locally.
