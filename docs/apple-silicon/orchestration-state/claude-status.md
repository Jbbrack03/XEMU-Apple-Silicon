# Claude Status

- Objective: Ship §E.13 per-format pitch + image-rect alignment XBE.
- Status: **CYCLE 10 CLOSED — Option A.** v0.2 ships and PASSes
  on Metal byte-correct against the math-derived oracle.
- Slice landed: `xbe-tests/texture-pitch-alignment/` Tier-1 diag
  XBE covering §E.13. 4x2 grid, 8 cells, LU_IMAGE_A8R8G8B8 only;
  sweeps `(IMAGE_RECT.width, IMAGE_RECT.height,
  TEXCTL1.IMAGE_PITCH)` across baseline / oversized / odd-dim
  combinations. v0.2 adds `EXTRA_PAD_ROWS = 8` sentinel rows
  beneath each active rectangle so height has a grounded oracle.
- Evidence consumed:
  - `docs/apple-silicon/handoff.md` cycle 9 + 10 banners
  - `docs/apple-silicon/orchestration-workflow.md`
  - `docs/apple-silicon/decision-log.md`
  - `docs/apple-silicon/diagnostic-xbe-plan.md` §4 / §5 (E.13)
  - `docs/apple-silicon/nv2a-feature-surface-research.md` §E.13
  - `.claude/rules/renderer-metal.md`, `.claude/rules/oracle-and-xbe.md`
  - `scripts/apple-silicon/xbe-tests/texture-format-sweep/{main.c,...}`
  - `scripts/apple-silicon/xbe-tests/lib/xbed_texture.{c,h}` + `lib.mk`
  - `hw/xbox/nv2a/nv2a_regs.h` (TEXTURE_CONTROL1.IMAGE_PITCH,
    TEXTURE_IMAGE_RECT.WIDTH/HEIGHT layouts)
  - `hw/xbox/nv2a/pgraph/texture.c:166-300` (pitch + image-rect
    consumer path)
  - `hw/xbox/nv2a/pgraph/mtl/texture_pg.c:367-379` (Metal-side
    upload that uses row_pitch)
- Files touched:
  - new: `scripts/apple-silicon/xbe-tests/texture-pitch-alignment/{main.c,Makefile,expected.py,manifest.json,README.md,bin/default.xbe,texture-pitch-alignment.iso,main.exe,main.obj,main.c.d}`
  - sync: `docs/apple-silicon/handoff.md`, `decision-log.md`,
    `orchestration-state/{claude-status,current-cycle,handoff-summary,validation-status}.md`,
    `.claude/rules/oracle-and-xbe.md` (XBE rotation index)
- Results on Metal:
  - v0.1 PASS at the pixel oracle gate but Codex review on v0.1 →
    **MAJOR ISSUES**: IMAGE_RECT.height not safely exercised +
    cell 4 mislabeled as baseline. v0.1 is therefore NOT
    considered the closure revision; the PASS is preserved here
    only for traceability of the iteration.
  - v0.2 adds `EXTRA_PAD_ROWS = 8` + corrected labeling +
    documents the height oracle. v0.2 PASS (signal_match=100%,
    changed_pct=0.99, gate 3.0). Codex re-review on the v0.2 diff
    → **MINOR ISSUES** (doc-drift only, no code findings); all
    minor items addressed before commit. Codex validation marker
    written for the v0.2 fingerprint per rule #15.
- M15 Gate 2 status: **1 of 4 MET** (this slice). Remaining: §H.6
  IMAGE_BLIT, §G.5 Z compression boundary, RT-as-texture sampling
  XBE (PGR2 late-stage-0 class).
- Next proposed action: open a fresh bounded session to ship §H.6
  IMAGE_BLIT XBE (Tier 2 — guest VRAM oracle per
  `diagnostic-xbe-plan.md` §5).
