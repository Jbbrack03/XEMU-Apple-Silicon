# Validation Status

- Current slice: §4.13 `texture-shader-stages` v0.3 sentinel + control-row bisect — task #18 CLOSE (cycle 8, 2026-05-22).
- Validation state: **GATES MET FOR OPTION-A EXIT (clean close).**
- Repo baseline before slice: clean tree on commit `aa67e4b6a1` (cycle 7 closure).
- Slice scope: Implement v0.3 bisect; find and fix two root causes (XBE D_SOURCE bug + pgraph.h PASS_THROUGH gate); confirm all 16 cells PASS on Metal.

## Gate status

- **Build (XBE):** PASS. `make clean && make` in texture-shader-stages dir produces `bin/default.xbe` and `texture-shader-stages.iso` cleanly.
- **Build (xemu):** PASS. `./build.sh -a arm64 --skip-shader-validation` produces signed `dist/xemu.app` cleanly after pgraph.h fix.
- **Harness discovery:** N/A this cycle (no structural changes to harness; relying on cycle 7 state).
- **Expected-oracle:** PASS (math-derived 16-cell oracle; uniform-color cells byte-exact in float→8-bit step).
- **Metal end-to-end run (final):** **PASS.** Harness: 1 pass, 0 fail, 0 skip, 0 infra-error. Run timestamp: 2026-05-22T04:29:05Z. All 16 cells byte-exact at center pixel sample.
- **Per-cell verification:** PASS. All 16 cells: row 0 = RED/GREEN/BLUE/WHITE, row 1 = BLACK×4, row 2 = WHITE×4, row 3 = RED/GREEN/BLUE/WHITE.
- **Bisect verdict:** RESOLVED. sentinel PASS + control PASS → combiner executes under textured-shader state; no Metal-specific SHADER_STAGE_PROGRAM issue. Both root causes (XBE D_SOURCE + pgraph.h gate) found and fixed.
- **Codex validation:** COMPLETED. Verdict: **PASS** (no findings). D_SOURCE=0x04 confirmed correct; pgraph.h mode!=4 removal confirmed correct; sentinel logic confirmed correct; pgraph.h blast radius confirmed low.
- **Doc sync:** COMPLETE after this update (handoff.md cycle-8 banner + decision-log cycle-8 entry TBD; all 4 orchestration-state files updated in this session).

## Known open items

- None for task #18 — CLOSED.
- **Task #17 (GL LOD-clamp regression):** Pre-existing; separate slice. Not touched in cycle 8.
- **§4.13 GL harness run:** GL screenshot capture path still unreliable (`could not create image from window`). The pgraph.h fix applies equally to GL (shared code path); GL passes by analysis but not by harness measurement. GL smoke test deferred to when the GL screenshot path is fixed.
- **M15 default-on prerequisites:** §4.13 now PASSES Metal, improving the XBE-PASS count. Recount needed in handoff.

## Cycle exit verdict

**Option A — clean close.** Task #18 fully resolved. Two bugs found and fixed (XBE D_SOURCE, pgraph.h gate). All 16 cells PASS byte-exact on Metal. No regressions expected (pgraph.h change is isolated to the PASS_THROUGH branch of an existing gating condition). Codex PASS. Ready to commit.
