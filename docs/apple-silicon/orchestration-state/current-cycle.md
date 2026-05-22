# Current Cycle

- Cycle: 11 (resumed bounded session after cycle 10 commit
  `bd27467eca`; covers §H.6 IMAGE_BLIT XBE).
- Started: 2026-05-22 07:14 CDT (carry-forward from prior bounded
  session that left untracked `scripts/apple-silicon/xbe-tests/image-blit/`
  and a failing harness run at
  `benchmark-runs/xbe-harness-20260522-071449/`).
- Closed:  2026-05-22 07:55 CDT (this session; evidence-run timestamp
  `xbe-harness-20260522-075241`).
- Owner: Claude Code (resumed bounded session).
- Session goal: Either repair the §H.6 IMAGE_BLIT XBE so Metal
  validation PASSes byte-correct, or — if grounded evidence proves a
  renderer issue — produce a bounded partial with exact proof and
  synced docs.
- Required reading consumed: `handoff.md` cycle-10 banner,
  `decision-log.md` cycle-10 entry, `orchestration-workflow.md`,
  `diagnostic-xbe-plan.md` §H.6 + §2.1, `nv2a-feature-surface-research.md`
  §H.6, `texture-pitch-alignment/main.c` (template),
  `hw/xbox/nv2a/pgraph/mtl/blit.c`, `hw/xbox/nv2a/pfifo.c` (PFIFO
  ramht_lookup + puller), `hw/xbox/nv2a/pgraph/pgraph.c` (NV062/NV09F
  handlers), `hw/xbox/nv2a/nv2a.c` (nv_dma_load + nv_dma_map),
  `nxdk/lib/pbkit/pbkit.c` (set_draw_buffer + pb_create_dma_ctx +
  pb_init), `nxdk/lib/pbkit/pbkit_dma.{c,h}`,
  `hw/xbox/nv2a/pgraph/mtl/surface.mm` (cache + s_image_blits counter).
- Scope guardrails (all honored):
  - Worked only inside `/Users/jbbrack03/XEMU_MacOS/xemu-fork` plus
    `scripts/apple-silicon/xbe-tests/image-blit/` (plus the minimal
    docs/state artifacts required for closure).
  - **No xemu renderer source changed.** All fixes confined to the
    XBE source (`main.c`, `nv2a_regs_image_blit.h`, manifest,
    README).
  - Updated orchestration-state artifacts.
  - Codex validation: COMPLETED on v0.2 → MAJOR ISSUES (stale README
    + manifest + claude-status drift around METAL_IMAGE_BLITS=0).
    All findings adopted in-session before commit.
- Exit criteria taken: **Option B (bounded partial).**
  - Original v0.1 assertion crash at `mtl/blit.c:170` is fully
    resolved (channel 9/11 reprogram trap identified via qemu
    `-trace events=nv2a_dma_map`; fixed by switching to handles
    3 / 4 which pbkit creates with `base=0, Limit=MAXRAM` and
    NEVER reprograms).
  - On Metal, **3 of 8 cells PASS byte-correct** (cells 0, 4, 5).
    5 of 8 fail (cells 1, 2, 3, 6, 7). Pattern: PASS envelope is
    `width == height == 8 AND (in_x, in_y) ≤ (4, 4)`.
  - Evidence durable at
    `benchmark-runs/xbe-harness-20260522-075241/` (signal_match_pct
    = 37.5000, changed_pixels_pct = 62.7083; best frame
    `image-blit.0124.png`). Crashing reference at
    `benchmark-runs/xbe-harness-20260522-071449/` (v0.1, blit.c:170
    assertion). Disproved-hypothesis reference at
    `benchmark-runs/xbe-harness-20260522-073448/` (MAXRAM-only,
    still asserts because buffer placement was never the issue).
    qemu trace evidence at `/tmp/image-blit-trace/xemu.log`.
  - Canonical docs synced (handoff.md cycle 11 banner,
    decision-log.md cycle 11 entry, orchestration-state including
    this file + claude-status + validation-status, image-blit
    README + manifest).
  - Codex marker written for the v0.2 fingerprint.
- Exit state: tree has uncommitted diff (new XBE dir + doc syncs).
  v0.2 Codex review complete; ready to commit. Residual 5-cell
  Metal failure mode documented as the next bounded slice (see
  decision-log cycle 11 entry + claude-status §Residual hypothesis
  for the investigation checklist).
