# Validation Status

- Current slice: Task #16 — Metal swizzle-mipmap diagnostic slice (Hermes cycle 2, 2026-05-21 evening)
- Build/tests: `./build.sh -a arm64 --skip-shader-validation` succeeded 4× during the slice. M5 shader-validation post-build gate intentionally skipped for the hot-iteration loop.
- Code coverage: diag additions only (no behavior change when env unset). New `set_attr_masks` diag is gated on `XEMU_METAL_DIAG_ATTRIB_DUMP`. New `XEMU_METAL_DUMP_TARGET_SHADER=stride44` heuristic noise-filter mode added; default mode unchanged.
- Codex validation: COMPLETED. Verdict: MAJOR ISSUES (3 findings). All 3 adopted in-slice:
  - HIGH (stride44 overstates what it proves): reworded as heuristic noise filter in renderer.c comment, automation.md, .claude/rules/flags-renderer.md, handoff.md, decision-log.md.
  - MEDIUM (/tmp artifacts cited as "durable"): copied logs / glsl-dumps / screenshots / reference into `docs/apple-silicon/task-16-evidence-2026-05-21/`; updated references in handoff.md and decision-log.md.
  - LOW ("GLSL/MSL pair" wording): changed to "GLSL VSH/PSH source" in automation.md and .claude/rules/flags-renderer.md.
- Oracle / visual gate: not required for this slice — diagnostic-only, no renderer-correctness claim. The swizzle-mipmap XBE remains `expected_fail` on Metal; harness rotation is unaffected.
- Doc-sync: handoff.md (banner appended), decision-log.md (supersedes-prior entry), automation.md (Diagnostic Toggles), .claude/rules/flags-renderer.md (index entries), and the new docs/apple-silicon/task-16-evidence-2026-05-21/ artifact dir all in sync.
