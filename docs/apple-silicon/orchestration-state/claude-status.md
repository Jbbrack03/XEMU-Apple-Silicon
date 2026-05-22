# Claude Status

- Objective: Implement v0.3 control-row + sentinel bisect for §4.13 `texture-shader-stages`; resolve task #18 root cause.
- Status: **TASK #18 CLOSED — FULL PASS ON METAL (harness: 1 pass, 0 fail, 2026-05-22T04:29:05Z).**
- Files touched:
  - `scripts/apple-silicon/xbe-tests/texture-shader-stages/main.c` — v0.3: 4x4 grid (16 cells, CELL_H=120); added Row 2 sentinel (PROGRAM_NONE + A=B=INVERT(ZERO)=1.0, textured shaders, expected WHITE×4) and Row 3 control (PROGRAM_NONE + A=V0/DIFFUSE, DEFAULT shaders, expected R/G/B/W); moved `xbed_load_textured_shaders()` from `main()` into `render_one()`; added mid-frame `xbed_load_default_shaders()` before Row 3; added `program_combiners_sentinel()` function. **ALSO FIXED**: FINAL CW0 `D_SOURCE` corrected from 0x0C (PS_REGISTER_R0, never written) to 0x04 (PS_REGISTER_V0, where OCW AB_DST=0x4 stores the result) in both `program_combiners_with_a_source()` and `program_combiners_sentinel()`.
  - `scripts/apple-silicon/xbe-tests/texture-shader-stages/expected.py` — 16-cell oracle; Row 2 WHITE×4; Row 3 R/G/B/W.
  - `scripts/apple-silicon/xbe-tests/texture-shader-stages/manifest.json` — v0.3 title; `expected_fail_renderers: []` (passes all renderers); `expected_fail_notes` updated with full resolution narrative.
  - `hw/xbox/nv2a/pgraph/pgraph.h:331` — `pgraph_is_texture_stage_active()`: removed `mode != 4` exclusion; PASS_THROUGH is an active stage; only PROGRAM_NONE (0) is inactive.
  - Rebuilt XBE artifacts (`bin/default.xbe`, `texture-shader-stages.iso`, `main.exe`, `main.obj`, `main.c.d`).
- Commands / tests run:
  - `make clean && make` in texture-shader-stages dir → clean build.
  - `python3 xbe_orchestrator.py run --xbe texture-shader-stages --renderer metal` (initial v0.3 with D_SOURCE=0x0C) → expected_fail (bug present).
  - Per-cell pixel analysis → sentinel BLACK instead of WHITE → identified D_SOURCE=0x0C bug.
  - Fixed D_SOURCE, rebuilt XBE.
  - `python3 xbe_orchestrator.py run --xbe texture-shader-stages --renderer metal` (D_SOURCE fixed, pgraph.h unfixed) → expected_fail (row 0 BLACK — PASS_THROUGH degraded to NONE).
  - Per-cell analysis → Row 2 WHITE ✓, Row 3 R/G/B/W ✓, Row 0 BLACK ✗ (PASS_THROUGH gate).
  - Fixed `pgraph_is_texture_stage_active()` in pgraph.h.
  - `./build.sh -a arm64 --skip-shader-validation` → clean build.
  - `python3 xbe_orchestrator.py run --xbe texture-shader-stages --renderer metal` → **PASS** (harness: 1 pass, 0 fail).
  - Per-cell pixel verification: all 16 cells byte-exact.
- Root causes found and fixed:
  1. **XBE combiner D_SOURCE bug** (in `main.c`): FINAL CW0 used `D_SOURCE=0x0C` (PS_REGISTER_R0) but OCW `AB_DST=0x4` writes to PS_REGISTER_V0 (0x04), not R0. R0 was never written → FINAL read 0 → BLACK for ALL combiner rows. This was the root cause of v0.2's ALL-BLACK results — the v0.2 failures were XBE code bugs, NOT Metal renderer bugs.
  2. **PASS_THROUGH gate bug** (in `pgraph.h:331`): `pgraph_is_texture_stage_active()` returned false for mode 4 (PASS_THROUGH) due to `mode != 4` exclusion. psh.c:145-146 then cleared the stage program bits → PASS_THROUGH treated as NONE → T0=0. Fix: removed `mode != 4`; only PROGRAM_NONE (0) is inactive. Affects both GL and Metal (shared code path via `pgraph_glsl_set_psh_state`).
- Bisect interpretation (v0.3 intermediate results before pgraph.h fix):
  - **sentinel (row 2) PASS + control (row 3) PASS**: Combiner executes under textured-shader state (combiner writes NOT silently dropped). V0/DIFFUSE path works with default shaders. No Metal-specific SHADER_STAGE_PROGRAM override issue.
  - Row 0 PASS_THROUGH failure was pgraph.h:331 PASS_THROUGH gate bug, fixed in same session.
- Confidence: **HIGH** — all 16 cells byte-exact on Metal; two root causes fully localized to source + confirmed by fix + retest.
- Next proposed action: Codex validation (rule #15); commit locally (do NOT push).
