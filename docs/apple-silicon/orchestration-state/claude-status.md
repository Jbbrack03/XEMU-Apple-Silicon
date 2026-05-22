# Claude Status

- Objective: cycle-12 §H.6 `image-blit` v0.3 commit/cleanup continuation.
- Status: **CLOSED.** Cycle-12 v0.3 bounded partial committed; renderer source clean; Codex MAJOR-ISSUES findings adopted; state artifacts durably closed.
- Outcome:
  - `hw/xbox/nv2a/pgraph/mtl/blit.c` diff = 0 lines in the final closure tree.
  - Codex `changes`-mode validation completed (rule #15 satisfied).
  - All three Codex findings adopted in-tree (state-file drift fixed pre-commit; BR encoding doc claim corrected in main.c + README + decision-log + handoff; top-level README/manifest overview updated for the v0.3 2×2 FAIL layout).
  - Slice commit hash: `e8fba9e925` (recorded in this follow-up state-sync commit).

## Carry-forward technical conclusion

The cycle-12 technical conclusion remains: `image-blit` v0.3 materially narrowed the §H.6 residual to a guest CPU VRAM read-back coherency issue. The Metal renderer's CPU memcpy is byte-correct for all 8 cells (proven by transient `mtl/blit.c` fprintf, now reverted). The cycle-11 hypotheses (shared blit math, tile-limit clipping, surface-cache download) are disproved.

## Next bounded slice (NOT started this cycle)

Cycle 13 — investigate xemu's CPU/TLB read-back coherency between host pgraph memcpy writes to `d->vram_ptr + phys` and guest TCG vCPU reads through the cached (`0x80000000 + phys`) and AGP-aliased (`0xF0000000 + phys`) virtual mappings. See decision-log cycle-12 entry "Cycle-13 follow-up" for candidate diagnostic angles.
