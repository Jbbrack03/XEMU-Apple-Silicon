# Validation Status

- Current slice: §4.13 `texture-shader-stages` v0.2 DIFFUSE-source bisect — task #18 investigation cycle 7 (launched 2026-05-22 02:35 CDT).
- Validation state: GATES MET FOR BOUNDED-PARTIAL EXIT.
- Repo baseline before slice: clean tree on commit `8defc984ce` (cycle 6 closure).
- Slice scope: ship the v0.2 DIFFUSE-source bisect row (cells 8-11) that v0.1's `expected_fail_notes` documented as the next-session-actionable next step. The bisect's purpose is to distinguish a renderer-side PASS_THROUGH/SHADER_STAGE_PROGRAM-dispatch bug from a broader XBE-side combiner / state-machine bug.

## Gate status

- **Build:** PASS. `make` against the nxdk toolchain produces `bin/default.xbe` (155,648 bytes) and `texture-shader-stages.iso` (720,896 bytes) cleanly; one pre-existing Cg warning (TEX0 register-mapping note) is unchanged from v0.1.
- **Harness discovery:** PASS. `python3 xbe_orchestrator.py list` shows `texture-shader-stages` discovered with the updated v0.2 title.
- **Expected-oracle generation:** PASS. `python3 expected.py /tmp/texture-shader-stages-v02-expected.png` produces a 640x480 RGBA PNG with the 4x3 colored / colored layout (rows 0+2 R/G/B/W; row 1 BLACK).
- **Metal end-to-end run:** **FAIL on Metal (expected per manifest declaration).** The harness correctly reports `expected_fail` given the manifest's `expected_fail_renderers: ["xemu/metal"]`. Captured: 8 XBE-active pure (0,0,0,0) frames across 3 render-and-reboot cycles. Evidence: `benchmark-runs/20260522T075639Z-task18-texture-shader-stages-metal-v0.2-baseline/`.
- **Bisect verdict:** ROW 2 ALSO BLACK. The DIFFUSE-source bypass row produces no visible output on Metal, ruling out the PASS_THROUGH-specific reading of v0.1's revised hypothesis 1 (the broader "override-not-honored under textured-shader state" reading remains live per Codex 2026-05-22 finding 1). Two surviving candidates: (a) textured-shader state-machine interaction; (b) NEW per Codex 2026-05-22 finding 2 — combiner-rewrite (row1→row2 `A_SOURCE` switch) may also be silently ignored under textured-shader state, which would equally explain row 2's BLACK output.
- **Cross-XBE sanity:** N/A this cycle (no new harness changes; relying on cycle 6's same-session combiner-basic PASS as the harness-soundness gate).
- **Codex validation:** COMPLETED. Verdict: **MINOR ISSUES** (3 findings, all adopted in cycle 7 before commit — MEDIUM softening of the bisect verdict wording; MEDIUM addition of the third candidate (combiner-rewrite ignored); LOW doc-status sync across orchestration files).
- **Visual / oracle validation:** N/A for renderer-correctness CLAIM (the slice ships as an expected_fail SPEC ORACLE; no new claim is made about Metal renderer correctness for §D.8 in cycle 7 — task #18 is now better narrowed but still open).
- **Doc sync:** COMPLETE. `handoff.md` cycle 7 banner appended; `decision-log.md` cycle 7 entry appended; orchestration-state files refreshed.

## Known open items

- **Task #18 (still next session):** TWO surviving candidate root causes after Codex finding 2. Next concrete code step is the v0.3 control-row bisect that uses `xbed_load_default_shaders` (no texturing) but still issues per-cell SHADER_STAGE_PROGRAM writes — isolates (a) textured-shader state-machine interaction from the broader SHADER_STAGE_PROGRAM override-not-honored case. **In parallel** (per Codex finding 2): instrument the row1→row2 combiner `A_SOURCE` switch directly with a sentinel combiner config that would produce a deterministic non-black output IF the combiner update is honored — isolates (b) combiner-rewrite ignored under textured-shader state. Alternatively / additionally: enable `XEMU_METAL_DIAG_ATTRIB_DUMP=1` to dump per-draw pipeline cache keys and verify pipeline rebuild events.
- **Separate PASS_THROUGH-degraded-to-NONE bug (psh.c:142-148):** Real, but NOT task #18 itself. Queued as a separate future fix slice with its own Metal-boot regression test (commit `046160d04d` was a Metal-boot stability fix and the gate change risks regressing boot animation without dedicated testing).
- **GL leg exclusion** from `expected_fail_renderers`: unchanged from cycle 6 — harness GL screencap path is known-unreliable; treat any GL run as smoke until a renderer-native GL screenshot path lands.

## Cycle exit verdict

**Bounded partial closed cleanly per the cycle's documented fallback exit criterion B.** The slice lands the bounded vertical implementation (4 modified files + rebuilt artifacts + 1 evidence dir under `benchmark-runs/`) with durable evidence (full 256-PNG screenshot sequence + per-row-stats narrative + 4 representative frames), refined bisect verdict documented (PASS_THROUGH-specific reading of v0.1 hypothesis 1 ruled out; broader override-not-honored reading remains live; two surviving candidates including the NEW combiner-rewrite-ignored candidate from Codex finding 2), updated canonical docs (handoff + decision-log + 3 orchestration-state files), Codex validation COMPLETED with MINOR ISSUES (all 3 findings adopted), and the next concrete code step (v0.3 control-row bisect with xbed_load_default_shaders + per-cell SHADER_STAGE_PROGRAM writes AND combiner-sentinel instrumentation for the row1→row2 A_SOURCE switch). Ready to commit locally; do NOT push to origin.
