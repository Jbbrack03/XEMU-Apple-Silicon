# Validation Status

- Current slice: §4.13 `texture-shader-stages` v0.1 — last unstarted first-wave XBE, Hermes cycle 6 (launched 2026-05-22 01:35:34 CDT).
- Validation state: GATES MET FOR BOUNDED-PARTIAL EXIT.
- Repo baseline before slice: clean tree on commit `261b6a6a56` (task #16 closure).
- Slice scope: ship the v0.1 SPEC ORACLE covering 2 of 19 NV2A SHADER_STAGE_PROGRAM modes at stage 0 (`PASS_THROUGH` 0x04 + `PROGRAM_NONE` 0x00) — the smallest high-value vertical slice that exercises the 5-bit-per-stage register dispatch path without requiring multi-stage chaining infrastructure.

## Gate status

- **Build:** PASS. `make` against the nxdk toolchain produces `bin/default.xbe` (151,552 bytes) and `texture-shader-stages.iso` cleanly; SHA matches between bin/ and iso content.
- **Harness discovery:** PASS. `python3 xbe_orchestrator.py list` now reports 20 XBEs (was 19), including `texture-shader-stages tier=1 BUILT`.
- **Expected-oracle generation:** PASS. `python3 expected.py /tmp/texture-shader-stages-expected.png` produces a 640x480 RGBA PNG with the 4x2 colored / black layout.
- **Metal end-to-end run:** **FAIL on Metal (expected per manifest declaration).** The XBE boots and clears OK but draws produce no visible output. Harness correctly reports `expected_fail` status given the manifest's `expected_fail_renderers: ["xemu/metal"]`. See evidence under `benchmark-runs/20260522T070256Z-task18-texture-shader-stages-metal-v0.1-baseline/` (canonical baseline with `nv2a` screenshot source pinned via `metal_canonical_overrides`).
- **Cross-XBE sanity:** PASS. `combiner-basic` PASSes on the same harness setup in the same session (`/tmp/combiner-basic-sanity/`), so the harness is sound — the §4.13 FAIL is renderer-interaction-specific.
- **Codex validation:** PASS. Verdict **MINOR ISSUES** (2 findings, both adopted; 1 open question deflected via explicit documentation; 1 out-of-scope note that strengthens the slice's case). Marker `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` will be written on commit-clean fingerprint.
- **Visual / oracle validation:** N/A for renderer-correctness CLAIM (the slice does not claim Metal renderer correctness for §D.8; it ships as an expected_fail SPEC ORACLE that documents the FAIL as task #18 for a separate renderer-fix slice).
- **Doc sync:** COMPLETE. `handoff.md` cycle 6 banner appended; `decision-log.md` cycle 6 entry appended; orchestration-state files refreshed.

## Known open items

- **Task #18 (next session):** Investigate why per-cell SHADER_STAGE_PROGRAM writes do not produce visible output on Metal despite the cache key correctly including `shader_stage_program` (Codex 2026-05-22 verified the key composition via shaderstate.h → shaders.h → psh.h). Three candidate root causes documented in the manifest's `expected_fail_notes`; next concrete code step is the v0.2 DIFFUSE-source bisect cell.
- **GL leg exclusion** from `expected_fail_renderers`: documented explicitly in `expected_fail_notes` — the harness GL screencap path is known-unreliable; the GL leg is not encoded as expected_fail because the harness reports `no-screenshot-captured` rather than a meaningful diff. A separate harness-improvement slice would add renderer-native GL screenshots.

## Cycle exit verdict

**Bounded partial closed cleanly per the cycle's documented fallback exit criterion B.** The slice lands the bounded vertical implementation (5 new files: `main.c` + `expected.py` + `manifest.json` + `Makefile` + build artifacts) with durable evidence (3 distinct benchmark-runs directories), exact blockers documented (3 candidate root causes for task #18), updated canonical docs (handoff + decision-log + 3 orchestration-state files), and the next concrete code step (v0.2 DIFFUSE-source bisect cell). Ready to commit locally; do NOT push to origin.
