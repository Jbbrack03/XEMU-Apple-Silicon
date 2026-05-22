# Current Cycle

- Started: 2026-05-21 20:29 local
- Owner: Claude Code (fresh Hermes-supervised session)
- Session goal: Task #16 Metal swizzle-mipmap diagnosis/fix slice
- Scope: Investigate the selective omission of TEX0 slot-9 streaming attributes in the Metal pipeline-key / vertex-descriptor path causing `swizzle-mipmap` intra-mip UV collapse to Q0.
- Canonical docs to read first: `docs/apple-silicon/handoff.md`, `docs/apple-silicon/orchestration-workflow.md`, relevant `docs/apple-silicon/diagnostic-xbe-plan.md` task-#16 sections, and `docs/apple-silicon/automation.md` references for `XEMU_METAL_DIAG_ATTRIB_DUMP`.
- Exit criteria:
  1. Either land a bounded fix for Task #16 with targeted validation evidence, Codex validation, doc sync, and clean commit;
  2. Or, if a safe fix does not fit this slice, leave the tree clean and produce a compact durable diagnosis with exact next-step recommendation and evidence references.
- Required validation if code changes: targeted swizzle-mipmap rerun on Metal, confirm diagnostic/evidence matches the claim, run Codex validation, sync `handoff.md` + `decision-log.md` + any plan/docs touched.
- Out of scope: unrelated renderer cleanup, pushing to origin, broad retail-title reruns, Task #17 GL fix, §4.13 full texture-shader-stages implementation, §4.15 v0.2 harness expansion unless directly needed for Task #16 proof.
