# Claude Status

- Objective: cycle-13 §H.6 `image-blit` residual code-path audit + state-sync closure (doc-only slice).
- Status: **CLOSED.** Doc-only audit committed; all load-bearing citations spot-verified; state artifacts durably closed.
- Session: `hermes_xemu_live_20260522_110134`

## Outcome

- Diff is doc-only: `docs/apple-silicon/{handoff.md, decision-log.md, orchestration-state/*}`. Zero `xemu-fork/hw/` content. Zero `xemu-fork/scripts/apple-silicon/` content.
- All load-bearing audit citations spot-verified against the live tree:
  - `grep -rn '0x400700\|PGRAPH_STATUS' hw/xbox/nv2a/` returns ZERO hits (PGRAPH_STATUS is never published).
  - `hw/xbox/nv2a/nv2a.c:247-248` — `qemu_thread_create("nv2a.pfifo_thread", pfifo_thread, ...)`.
  - `hw/xbox/nv2a/pfifo.c` — `pfifo_write` (lines ~84-110) calls `pfifo_kick(d)`; `pfifo_kick` (lines ~112-115) is `qemu_cond_broadcast(&d->pfifo.fifo_cond)`; `pfifo_run_puller` (lines ~226-272) acquires `d->pgraph.lock` and dispatches `pgraph_method` from the PFIFO thread.
  - `hw/xbox/nv2a/pgraph/pgraph.c:115-150` — `pgraph_read` fast path returns `qatomic_read(&pg->regs_[addr])` with no acquire barrier; `pgraph_method` defined at lines ~744+.
  - `hw/xbox/nv2a/pgraph/mtl/blit.c:215-220` — `perform_blit_cpu(...)` CPU memcpy runs on PFIFO thread.
  - `hw/xbox/nv2a/pgraph/mtl/renderer.c:2525` — `.image_blit = pgraph_mtl_image_blit` registration.
  - `include/qemu/atomic.h:77-84` — `qatomic_read` is `__atomic_load_n(..., __ATOMIC_RELAXED)`.
  - `nxdk/lib/pbkit/outer.h:461-462` — `NV_PGRAPH_STATUS = 0x00400700`, `NV_PGRAPH_STATUS_NOT_BUSY = 0`.
  - `nxdk/lib/pbkit/pbkit.c:486-494` — `pb_wait_until_gr_not_busy` busy-poll body.
  - `nxdk/lib/pbkit/pbkit_dma.c:55-58` — `pb_agp_access` returns `fb | AGP_MEMORY_REMAP`.
  - `XEMU_PGRAPH_FAST_READ` default-on confirmed via `.claude/rules/{flags-renderer.md, renderer-state.md}` and `docs/apple-silicon/automation.md`.
- Codex validation NOT required: doc-only slice (rule #15 trivial-doc exemption — zero source change).
- Slice commit hash: `0bd85f70fe` (recorded in this follow-up state-sync commit; the closure commit cannot reference its own SHA).

## Carry-forward technical conclusion

The §H.6 `image-blit` residual is a **PFIFO ↔ vCPU dispatch race against the never-published `NV_PGRAPH_STATUS`**, NOT a guest CPU cache-coherency issue. The race lives in PFIFO/PGRAPH machinery shared by GL, Vulkan and Metal backends. The same XBE should exhibit a similar PASS/FAIL split under `XEMU_RENDERER=GL`. Implications extend beyond §H.6: every retail title that uses `pb_wait_until_gr_not_busy` as a software fence between an IMAGE_BLIT (or other PGRAPH-resident operation) and a CPU VRAM read is silently affected.

## Next bounded slice (NOT started this cycle)

Cycle 14 — run `image-blit.iso` through `scripts/apple-silicon/xbe-harness` under `XEMU_RENDERER=GL`. Confirms/disproves the renderer-agnostic race hypothesis cheaply. See `decision-log.md` 2026-05-22 cycle-13 entry "Cycle-14 next-slice plan" for the staged follow-on slices (`XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` diagnostic flag, real-Xbox oracle parity check).
