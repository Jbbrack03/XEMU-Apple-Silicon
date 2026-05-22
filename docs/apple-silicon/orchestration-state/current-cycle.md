# Current Cycle

- Started: 2026-05-22 04:10 CDT (approximate; fresh bounded session after cycle 7 commit `aa67e4b6a1`).
- Closed:  2026-05-22 04:35 CDT (approximate; tied to commit timestamp).
- Owner: Claude Code (fresh bounded session implementing v0.3 control-row + sentinel bisect for task #18; full resolution).
- Session goal: Implement v0.3 control-row bisect; run harness; if evidence localizes the bug, apply the smallest grounded fix. Exit on either clean close (A) or bounded partial (B).
- Required reading consumed: `handoff.md` cycle-7 banner, `decision-log.md` cycle-7 entry, `orchestration-state/*.md`, `diagnostic-xbe-plan.md` §4.13, existing v0.2 source + manifest + expected.py, `glsl/psh.c`, `pgraph.h`, `psh_regs.h`, `ps.inl`, `xbed_tex_ps.inl`.
- Scope guardrails (all honored):
  - Worked only inside `/Users/jbbrack03/XEMU_MacOS/xemu-fork`.
  - Implemented v0.3 control-row + sentinel; ran on Metal; found D_SOURCE=0x0C XBE bug; fixed; ran again; all 4 rows passed; then identified and fixed the psh.c PASS_THROUGH gate bug; full PASS confirmed.
  - Updated orchestration-state artifacts.
  - Codex validation: COMPLETED. Verdict: **PASS** (no findings). D_SOURCE=0x04 confirmed correct; pgraph.h mode!=4 removal confirmed correct; sentinel logic confirmed correct; pgraph.h blast radius confirmed low (one caller, pre-existing TEXCTL0.ENABLE caveat is not introduced by this diff).
  - Renderer-correctness CLAIM: MADE — §4.13 texture-shader-stages PASSES Metal (harness: 1 pass, 0 fail, 2026-05-22T04:29:05Z run).
- Exit criteria taken: **Option A (clean localized fix + verified passing + docs updated).**
  - Two bugs found and fixed:
    1. XBE combiner D_SOURCE=0x0C bug (in `main.c`) — the real root cause of v0.2 ALL-BLACK.
    2. `pgraph_is_texture_stage_active()` excluding PASS_THROUGH mode 4 (in `pgraph.h:331`) — separate psh.c gate bug, affects both renderers.
  - Both bugs fixed; full harness PASS on Metal (all 16 cells byte-exact).
  - Task #18 CLOSED.
- Exit state: tree has uncommitted diff (main.c, expected.py, manifest.json, pgraph.h, rebuilt XBE artifacts, doc updates). Codex to run; then commit.
