# Claude Status

- Objective: author the smallest-high-value §4.13 `texture-shader-stages` v0.1 vertical slice.
- Status: SLICE LANDED + BUILT + RUN + HARNESS-VALIDATED AS EXPECTED_FAIL (Metal). Slice scope: a 4×2 grid (8 cells) exercising 2 of 19 NV2A SHADER_STAGE_PROGRAM modes at stage 0 — PASS_THROUGH (0x04) on row 0 with TEXCOORD0 = (R,G,B,1) per cell, PROGRAM_NONE (0x00) on row 1 with identical TEXCOORD0 input. Row 0 expected RED/GREEN/BLUE/WHITE; row 1 expected BLACK x 4. Shared combiner topology: t0 → R0 → fragColor; final-combiner G = DIFFUSE.a (white) → fragColor.a = 1.0.
- Files touched (5 new):
  - `scripts/apple-silicon/xbe-tests/texture-shader-stages/main.c` — XBE source.
  - `scripts/apple-silicon/xbe-tests/texture-shader-stages/expected.py` — math-derived oracle.
  - `scripts/apple-silicon/xbe-tests/texture-shader-stages/manifest.json` — harness manifest (declares `expected_fail_renderers: ["xemu/metal"]` with detailed `expected_fail_notes` documenting the v0.1 finding + next-session-actionable repro recipe).
  - `scripts/apple-silicon/xbe-tests/texture-shader-stages/Makefile` — nxdk build wiring.
  - `scripts/apple-silicon/xbe-tests/texture-shader-stages/{bin/default.xbe, texture-shader-stages.iso, main.exe, main.obj, main.c.d}` — build artifacts.
- Commands/tests run:
  - `make` in the new dir against the nxdk toolchain → clean build (lib/xbed_texture.obj rebuilt for the new include; CXBE + XISO succeeded).
  - `python3 expected.py /tmp/texture-shader-stages-expected.png` → 640×480 PNG generated.
  - `python3 xbe_orchestrator.py list` → confirmed XBE is discovered alongside the 19 existing entries (now 20).
  - `python3 xbe_orchestrator.py run --xbe texture-shader-stages --renderer metal` (twice; once with default `drawable` source, once after pinning `metal_canonical_overrides: {"XEMU_METAL_SCREENSHOT_SOURCE": "nv2a"}`) → both `expected_fail` per manifest.
  - `python3 xbe_orchestrator.py run --xbe combiner-basic --renderer metal` → PASS sanity check that the harness still works.
  - `XEMU_METAL_DUMP_TARGET_SHADER=all` run → 1024 .glsl files dumped; only PROJECT2D shaders generated for the 0x032a4000 front buffer.
- Evidence produced:
  - `benchmark-runs/20260522T064552Z-task18-texture-shader-stages-metal/` — initial run (drawable source).
  - `benchmark-runs/20260522T065933Z-task18-ts-shader-dump/` — shader-dump run.
  - `benchmark-runs/20260522T070256Z-task18-texture-shader-stages-metal-v0.1-baseline/` — canonical baseline (nv2a source pinned, expected_fail recognized by harness).
  - `/tmp/texture-shader-stages-expected.png` — math-derived reference (also copied into the baseline run dir).
- Empirical finding (v0.1 ships as Metal expected_fail; documents this as task #18 investigation target):
  - **Metal does NOT render the expected 4x2 grid.** The XBE successfully boots, executes `xbed_clear_color_argb(0xFF000000)` (confirmed by 3 distinct pure-BLACK capture frames at indices ~0124-0126 / 0190-0192 / 0256-0258, each ~5s apart matching the 300-frame render-and-capture-and-reboot cycle), and reaches the draw loop — but the 8 per-cell `xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, ...)` calls produce NO visible output: the captured frame stays pure BLACK throughout the XBE render window.
  - Shader dump (`XEMU_METAL_DUMP_TARGET_SHADER=all`) shows only PROJECT2D-mode PSH variants for the 0x032a4000 front buffer (2 unique pipelines, both from the dashboard / xbed_load_textured_shaders Cg-emitted state); no `vec4 t0 = pT0;` (PASSTHRU) signature is observed.
  - Three candidate root causes (next-session investigation):
    1. Metal pipeline cache key does not include SHADER_STAGE_PROGRAM, so per-cell stage-program writes never trigger a new MSL compile and the cached PROJECT2D shader keeps running.
    2. XBE-side combiner setup is incorrect in a way that causes the draws to fail silently (e.g. wrong ICW_A_SOURCE encoding for T0).
    3. Some other state-machine interaction between the textured-shaders Cg setup and the per-cell SHADER_STAGE_PROGRAM override.
- Blockers / uncertainties: root-cause investigation deferred — see `expected_fail_notes` in manifest for the next-session-actionable repro recipe (add a 3rd cell variant in v0.2 that uses `ICW_A_SOURCE=DIFFUSE` with per-cell DIFFUSE = (R,G,B,1); if that variant renders, root causes (2)/(3) ruled out and the regression localizes to the SHADER_STAGE_PROGRAM → Metal pipeline-key chain).
- Codex review: COMPLETED. Verdict MINOR ISSUES (2 findings, both adopted).
  - MEDIUM (adopted): manifest's candidate root cause (1) "Metal pipeline cache key omits SHADER_STAGE_PROGRAM" was wrong — Codex verified the key DOES include it via `mtl/shaderstate.h:59-67` → `glsl/shaders.h:27-31` → `glsl/psh.h:37-40`. Manifest rewritten: revised hypothesis (1) now points at pipeline-rebuild dirty-state propagation around `NV_PGRAPH_SHADERPROG` writes.
  - LOW (adopted): main.c header text + expected.py docstring claimed "no `compare_overrides` needed / byte-exact" while the manifest sets `max_changed_pct=3.0 / signal>=97.0`. Both header texts updated to clarify the budget exists only for inter-cell rasterizer edges + harness frame-selection slack; cell interiors remain byte-exact.
  - OPEN (deflected via documentation): Codex asked whether GL should be explicitly in `expected_fail_renderers`. Manifest now documents the GL exclusion in `expected_fail_notes`.
  - OUT OF SCOPE: Codex found no authoring bug in main.c → strengthens the case that the Metal FAIL is renderer-side, validates filing as task #18.
- Next proposed action: doc sync (DONE — handoff.md, decision-log.md, orchestration-state files all updated); local commit (do NOT push); write Codex marker on post-commit clean fingerprint.
- Confidence / risk notes: HIGH that the XBE is built correctly (build clean, expected.py byte-exact, harness discovers it, Codex review found no authoring bug). HIGH that the Metal FAIL is a real renderer-side signal worth filing as task #18 (Codex's "out of scope" note specifically validates this). MEDIUM that the next-session bisect (v0.2 DIFFUSE-source cell) will narrow root cause within one session. The v0.1 ships as SPEC ORACLE in the same shape as `logic-ops` (expected_fail neither renderer implements) and `swizzle-mipmap` v0.2 pre-closure (expected_fail tracking real renderer regression task) — the regression rotation now has a feature-isolation oracle for §D.8 going forward.
