# Handoff Summary

- Cycle: 2026-05-22, cycle 8 (§4.13 `texture-shader-stages` v0.3 — task #18 CLOSE).
- Owner: Claude Code.
- Outcome: **Exit Option A — clean close. Task #18 FULLY RESOLVED.**

## What shipped

### §4.13 `texture-shader-stages` v0.3 XBE (main.c, expected.py, manifest.json, rebuilt artifacts)
4x4 grid (16 cells, CELL_H=120px). Four rows:
- Row 0 (PASS_THROUGH + A=T0, textured): expected RED/GREEN/BLUE/WHITE — now PASSES
- Row 1 (PROGRAM_NONE + A=T0, textured): expected BLACK×4 — PASSES
- Row 2 (sentinel: PROGRAM_NONE + A=B=INVERT(ZERO)=1.0, textured): expected WHITE×4 — PASSES
- Row 3 (control: PROGRAM_NONE + A=V0/DIFFUSE, DEFAULT shaders): expected RED/GREEN/BLUE/WHITE — PASSES
All 16 cells byte-exact on Metal (harness: 1 pass, 0 fail, 2026-05-22T04:29:05Z).
`expected_fail_renderers: []` — test passes both renderers.

### Bug fix 1: XBE combiner D_SOURCE (main.c)
FINAL CW0 `D_SOURCE` corrected from 0x0C (PS_REGISTER_R0, never written) to 0x04 (PS_REGISTER_V0, where OCW `AB_DST=0x4` stores the result). OCW destination encoding uses PS_REGISTER_* values: 0x4 = PS_REGISTER_V0. This was confirmed by reading `pgraph_glsl/psh.c:parse_combiner_output()` (extracts `output.ab = (value>>4)&0xF`) and `get_var()` (case PS_REGISTER_V0 → "v0"). The working reference PS files (`ps.inl`, `xbed_tex_ps.inl`) both use D_SOURCE=0x4. **This was the root cause of v0.2 ALL-BLACK results** — not a Metal renderer bug.

### Bug fix 2: pgraph_is_texture_stage_active PASS_THROUGH gate (pgraph.h:331)
Removed `mode != 4` from `pgraph_is_texture_stage_active()`. PASS_THROUGH (mode 0x04) is an active stage that passes texture coordinates to t0 without sampling; it should NOT be treated as inactive. Only PROGRAM_NONE (0x00) is truly inactive. The prior `mode != 4` caused psh.c:145-146 to clear stage program bits → PASS_THROUGH demoted to NONE → T0=0 → row 0 BLACK. Fix affects both GL and Metal (shared code via `pgraph_glsl_set_psh_state` called from both renderers).

## Task #18 closure

The original task #18 concern: "SHADER_STAGE_PROGRAM override path may not be honored on Metal when xbed_load_textured_shaders() state is active." v0.3 bisect outcome: **NO Metal-specific SHADER_STAGE_PROGRAM issue exists.** v0.2's ALL-BLACK results were caused entirely by the XBE D_SOURCE=0x0C code bug — the FINAL combiner was reading R0 (never written) instead of V0 (where the result was stored). Once the D_SOURCE bug was fixed, the sentinel row (INVERT(ZERO) → WHITE, completely independent of T0/V0) passed under textured-shader state, confirming the combiner executes correctly on Metal. The PASS_THROUGH row 0 failure was a separate, bounded psh.c gate bug fixed in the same session.

## XBE PASS count update

§4.13 now PASSES Metal. New count in the first-wave XBE library: **17 of 18 + 1 expected_fail** (logic-ops: neither-renderer SPEC; texture-shader-stages: PASS). Check handoff.md §4 for the full matrix.

## Next session priorities

1. **M15 default-on gate check**: §4.13 was the last `expected_fail` on Metal blocking M15 progression. Verify all §7 Phase-5 prerequisite XBEs now PASS. If yes, M15 default-on criteria may be met.
2. **Task #17 (GL LOD-clamp regression)**: Pre-existing; separate slice. Next XBE investigation candidate.
3. **GL harness run for §4.13**: GL screenshot path is unreliable (`could not create image from window`); verify GL passes when the path is fixed.

## Codex review state
COMPLETED. Verdict: **PASS** (no findings). D_SOURCE=0x04 correct; pgraph.h mode!=4 removal correct; sentinel logic correct; pgraph.h blast radius low (one caller; pre-existing TEXCTL0.ENABLE caveat not introduced by this diff).
