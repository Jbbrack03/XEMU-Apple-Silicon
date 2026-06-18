# Decision Log

## Index (added 2026-06-15 — restructure, no content changed)

Pre-2026-06 decisions (2026-04-29 -> 2026-05-31) were relocated verbatim to
`_archive/decision-log/decision-log-pre-2026-06.md`. Nothing was summarized,
rewritten, or deleted; every dated decision and `superseded by...` marker is
preserved byte-for-byte in the archive.

Still-binding decisions: see `metal-parity-roadmap.md` -> "Operating principles".

This live file keeps 2026-06-01-onward decisions below; new decisions continue
to append here (append-only).

---

## 2026-06-18 (XBE-first methodology surfaced a real Metal polygon-offset / depth-bias correctness gap — §C.3)

**Decision / finding.** The §C.3 `polygon-offset` diagnostic XBE (M-I second-wave slice 2, shipped v0.2 this session) exposed a **genuine Metal-renderer correctness gap**: the NV2A polygon-offset / depth-bias uniforms (`depthOffset` / `depthFactor`) reach the Metal fragment shader as **0 at runtime**, so the offset-dependent cells misrender on Metal while GL applies the bias correctly. The XBE is recorded `expected_fail_renderers: ["xemu/metal"]`; the gap is filed as task #10 (fix) + task #11 (XBE v0.3 redesign). The renderer fix is NOT yet attempted — only the diagnostic isolation is done.

**Evidence.** 2 cold Metal runs (`benchmark-runs/polygon-offset-metal-run{1,2}`, 136/137 frames) both score 62.5% signal_match = exactly the 5/8 offset-independent cells correct, 3/8 offset-dependent cells wrong — the deterministic signature of zero depth bias. In this fork polygon offset is applied in the fragment shader (PR #2240 / `XEMU_NATIVE_TRI_DEPTH`: `zvalue += depthOffset; zvalue += depthFactor*nativeTriMZ` in `glsl/psh.c`); Metal consumes the same generated GLSL as GL, so the divergence localizes to the Metal uniform plumbing, not the shader math. The `polygon-offset-gl-run1` FAIL is a separate GL FLIP_STALL / resolution-scale **capture artifact**, not a GL correctness problem (task #11).

**Why this matters (methodology vindication).** This is the XBE-first development loop (rule #17) working as designed: a feature-isolated diagnostic XBE caught a real renderer bug that the retail-title oracle would have surfaced only as a vague "some decals look wrong" symptom on an unknown title. The bug is now isolated to a 2-hypothesis decision (PSH emission gate `glsl/psh.c:1764-1778` vs Metal uniform staging `mtl/uniform.c:348-416`), with a single decisive next diagnostic (add the uniforms to the `metal_psh_uniform_diag` print at `mtl/uniform.c:374-391` and re-run).

**Binding constraint on the fix (rule #6).** The fix MUST preserve the PR #2240 fragment-shader depth path — replace the broken Metal uniform plumbing, do NOT strip the fragment-shader bias or revert to host-API `glPolygonOffset`/`setDepthBias`. Rule #6 forbids reverting the depth/polygon-offset correctness work; this gap is a plumbing defect *within* that path, not a reason to remove it.

**Coverage outcome.** Matrix regenerated (generator, not hand-edited) including the polygon-offset run dirs: Metal **PASS 15, xfail 2 (`logic-ops`, `polygon-offset`), FAIL 0, skip 1**; feature surfaces **40/91 covered, 35 green on Metal, 51 uncovered** (§C.3 counts as covered-but-Metal-xfail). No new `XEMU_*` flag added this session.

## 2026-06-15 (Codex validation decommissioned + Hermes orchestration archived — Claude Code operates solo)

**Decision.** Stop using Codex as the checks-and-balances / second-opinion validator and stop using Hermes as the orchestration layer. All of those roles revert to Claude Code working solo: Claude owns implementation, validation, orchestration, and handoffs directly. Codex tooling is removed; the Hermes orchestration framework is archived as inactive (not deleted) for possible future revival, potentially with a different agent.

**Rationale.** Per the user (2026-06-15): the project is no longer using Codex or Hermes. Keeping the `/codex-validate` skill, its two Stop/PostToolUse hooks, and the Hermes orchestration scaffolding active would mean the documented workflow no longer matches how the project actually operates (rule #4 no-drift). The Hermes framework is archived rather than deleted because it may be revived later.

**Outcome.**
- **Removed (Codex active systems):** `.claude/skills/codex-validate/`, `.claude/hooks/check-codex-validate.sh`, `.claude/hooks/remind-codex-validate-plan.sh`, their registration in `.claude/settings.json` (only `check-doc-sync.sh` Stop hook remains), and the `feedback_codex_validate.md` auto-memory + its `MEMORY.md` index line.
- **CLAUDE.md:** rule #15 repurposed from "Validate non-trivial work via Codex" to "Self-validate non-trivial work before stopping" (Claude owns checks-and-balances directly; rule numbering preserved so #16/#17 references stay valid). Skills list trimmed to four; Stop-hooks section reduced to one hook; the `orchestration-workflow.md` reference dropped from the doc index.
- **Archived (Hermes orchestration):** `orchestration-workflow.md` and `orchestration-state/` moved to `docs/apple-silicon/_archive/hermes-orchestration/` with an INACTIVE banner + archive `README.md`. README.md doc-catalog entry updated to point at the archive.
- **Living docs/rules scrubbed of forward-looking Codex/Hermes procedure** (handoff.md top banner; metal-porting-workflow.md operating loop; benchmarking.md; tooling-gap-plan.md; metal-renderer-plan.md TODOs; diagnostic-xbe-plan.md §6/§7/§9 cadence; nv2a-feature-surface-research.md §6/§8.1 cadence; `.claude/rules/renderer-state.md` + `oracle-and-xbe.md`).
- **Deliberately left intact as accurate historical records:** source-code provenance comments ("Codex review … finding", "Hermes cycle N"); all `benchmark-runs/*` and `.claude/state/*` run artifacts (incl. `repo-root-hermes-archive/`); `.bak` files; and the append-only cycle narrative in `handoff.md` / earlier `decision-log.md` entries. These describe how past work was done and are not current procedure.

## 2026-06-02 (reliability re-architecture + autonomous real-Xbox oracle loop)

**Decision.** Re-align the Hermes orchestration off the 252-task PGR2 "50-series" micro-hypothesis spiral and back onto the plan backbone, deploy the missing reliability + correctness mechanisms, and prove the headless real-Xbox autonomous game-test loop.

**Rationale.** Diagnosis showed the throughput problems were harness/orchestration, not Qwen capability or the graphics hypothesis: hung workers held all worker slots (stale-reclaim was set to 4h), the architect was defaulting to an empty kanban board, chronic Qwen tasks looped without converging, and visual validation relied on an LLM "looks right" judgment that had passed garbled frames. The board had drifted entirely off its own plan (252/377 open tasks on one artifact, 0 on the XBE backbone), against strategy.md's own "no blind PGR2 reruns" guidance.

**Outcome.** (1) Reliability: stale-reclaim 4h->90min; active board switched to xemu; deterministic Qwen->Codex escalation; kanban_comment bug fixed. (2) Board: 67 PGR2 drift tasks archived (reversible); plan backbone created -- F1 (correctness oracle gate, DONE), F2 (deterministic capture), O1 (real-Xbox golden oracle, human-gated), B1 (PGR2-as-spec-XBE), B2 (Phase-4 XBE wave), M1 (M15-gate tracking, DONE), M2 (close 4 missing M15 links). (3) Correctness tool: built scripts/apple-silicon/xemu_frame_correctness_oracle.py -- objective property/invariant + metric-vs-golden gate; LLM visual judgment removed from the gate. (4) Architect prompt hardened (plan-anchoring, depth-cap, no blind PGR2 reruns, oracle-is-the-gate). (5) Real-Xbox oracle: per-game profiles extracted from the XEMU test HDD (FATX) and copied to E:\UDATA/TDATA; the OGX360 IGR exit (Back+Start+LT+RT -> iND-BiOS) proven to return the console to the dashboard; a full Crimson Skies retail-gameplay-oracle run validated the autonomous loop end-to-end (dashboard_returned=true; 82/91 captured frames pass the correctness oracle). Open: F2 in development, O1 awaiting a one-time human golden curation; the renderer-correctness problem itself remains unproven.

## 2026-06-03 — Rainbow/M15 capture is NOT human-gated; grounded black-frame classifier + authoritative gameplay gate

**Capture truth (debunks the 2026-06-03 "human-gated" escalation).** An
orchestrated agent escalated that the M2 Rainbow / M15 gameplay-evidence
capture lane was "genuinely human-gated," asking for passwordless sudo
(launchctl asuser/bsexec) or a manual GUI-session bridge to run
`screencapture -x` from an SSH background session. This was WRONG — wrong tool,
not a real gate:
- Evidence frames are captured IN-EMULATOR (`XEMU_GL_SCREENSHOT_PATH` flip-stall
  GL framebuffer dump; QMP/HMP `screendump`; Metal NV2A PNG) — no macOS screen
  capture, no Aqua session, no TCC, no sudo. `metal-gl-compare.sh --trigger
  flip` already uses this; live runs (e.g. benchmark-runs/20260603-101131-…-
  halo-f2-smoke) produce 1280x960 game-only PNGs that pass the frame oracle.
- Even a real screen grab is not gated for a LOCAL in-session process running
  as jbbrack03 (the console user); only the SSH-background path the agent chose
  is. Capture must never run from the SSH/background bootstrap.
- Action: agents must NOT escalate capture as human-gated or request sudo/GUI
  bridges. Capture is solved; the open work is renderer correctness + F2
  deterministic alignment, not capture infrastructure.

**Grounded black-frame classifier (compare-screenshots.py + metal-gl-compare.sh).**
A black Metal frame is usually a renderer gap (geometry Metal can't draw yet)
but sometimes legitimate (fade-to-black). It is now classified DIFFERENTIALLY
against the GL reference at the SAME guest moment, emitted per frame as
`content_class` (summary.json `frames[]` + report.md table):
- `METAL_GEOMETRY_GAP` — GL has geometry, Metal is black, pair state-aligned →
  authoritative hard fail, named.
- `METAL_GEOMETRY_GAP_UNVERIFIED` — same divergence but NOT state-aligned (cold
  launch); falls back to the pixel-diff threshold, makes no authoritative
  renderer-bug claim.
- `EXPECTED_BLACK` — both renderers black (fade/load) → passes regardless of
  pixel diff.
- `CONTENT_BOTH` / `METAL_SPURIOUS` / `AMBIGUOUS`.
Agents/validators must read `content_class`: a black Metal frame is a RENDERER
signal, never a capture/infra/human-gate failure. Thresholds calibrated on real
captures; tested by `scripts/apple-silicon/test_compare_content_class.py` (9/9),
shellcheck clean. Validated e2e on a real boot (benchmark-runs/…-contentclass-
e2e: flip-60 early-boot frame correctly read EXPECTED_BLACK, not a false gap).

**Authoritative gameplay evidence requires a snapshot.** Cold-launch gameplay
diffs can only yield `METAL_GEOMETRY_GAP_UNVERIFIED`. `m15-visual-gate.sh` now
has a state-aligned gameplay-evidence step (`--gameplay-game GAME
--gameplay-snapshot TAG`, e.g. `pgr2 pgr2_gameplay_b4`) that drives a restored
savevm tag at a matched flip ordinal so GL and Metal are the same guest moment;
gameplay evidence without a snapshot is REFUSED (not silently downgraded).
`metal-gl-compare.sh` warns when `--evidence-class gameplay` runs without
`--snapshot`. To close the M2 evidence-only links authoritatively: create
per-title gameplay savevm tags (F2) and run the gate's gameplay step.

**Codex-validate candidate (rule #15).** These changes alter the M15 gate's
verdict semantics (named renderer-gap vs generic threshold fail; EXPECTED_BLACK
pass-through) and merit an independent Codex read.

## 2026-06-15 (evening) — Metal-parity roadmap adopted as canonical direction; real-Xbox oracle scope reframed

**Decision.** (1) Adopt `metal-parity-roadmap.md` as the canonical strategic
direction (5 workstreams A–E, gated milestones M0→M-V), superseding the scattered
"next-actions" lists in `handoff.md` / `.claude/rules/renderer-state.md`. (2) The
real-Xbox oracle's working use is **retail-title acceptance** (M-IV / Workstream D);
real-Xbox **diagnostic-XBE** goldens are deferred (not critical path). For M-I
feature correctness, the math-derived `expected.py` oracle is spec-authoritative for
well-specified features (stencil/blend/depth/logic/colour); the real Xbox remains the
only oracle-grade truth for ambiguous/complex (retail) scenes, and visual validity is
NEVER judged by eyeballing — always via the deterministic comparison tools.

**Rationale.** (a) The project declared XBE-first binding on 2026-05-20 but then
relapsed into three weeks of PGR2 sibling-sync tuning (commits 2026-05-30→06-09) —
the exact "fixated on one title" anti-pattern; the roadmap's milestone gates + the
regenerable coverage board make the discipline enforceable. (b) Per the user
(2026-06-15) GL is more mature than Metal but is NOT oracle-grade, and a math oracle
can encode a misunderstanding — so the real Xbox is the truth source. (c) BUT the
homebrew diagnostic XBEs were found to CRASH on real NV2A hardware during render,
before capture (data-proven via `E:\Apps\<id>\` crash-bisection markers: `stencil-ops`
wrote zero markers; `image-blit` dies at its blit), so real-Xbox diag-XBE goldens are
blocked by a deeper xemu↔hardware-divergence problem — while retail games DO run on
hardware, making retail the high-value oracle use.

**Outcome.**
- New: `metal-parity-roadmap.md`, `xbe-coverage-matrix.md` + its generator
  `scripts/apple-silicon/xbe-coverage-matrix.py`. M0 complete (coverage generator,
  `METAL_CLEAR_SYNC_US_TOTAL` instrument, lossless doc restructure, real Metal XBE
  baseline = 15 PASS/1 FAIL/1 xfail/1 not-built, 53/91 surfaces uncovered).
- Real-Xbox capture-path fix shipped (`xbed_capture.c`: D:\ → `E:\Apps\<id>\`).
  Diagnostic-XBE-on-hardware crash investigation tracked as a future, non-critical task.
- `METAL_CLEAR_SYNC_US_TOTAL` measured ~1.08 ms CPU stall per synchronous clear — the
  lead suspect for Metal's worse p99 jitter (Workstream C / M-III).
- Binding rules unchanged; this refines HOW the existing XBE-first (rule #17) +
  oracle-as-final-gate methodology is executed. Evidence:
  `benchmarks/2026-06-15-m0-baseline-and-clear-sync.md` +
  `benchmarks/2026-06-15-real-xbox-oracle-access-and-xbe-crash.md`.

## 2026-06-18 (adversarial multi-agent review gate replaces the Codex second-opinion validator)

**Decision.** Re-introduce an independent checks-and-balances gate for completed
work — but built from Claude Opus subagents instead of Codex. New `/adversarial-gate`
skill + three project agents under `.claude/agents/` (`adversarial-reviewer`,
`finding-validator`, `fix-implementer`) run a closed loop: red-team review across
three lenses (correctness / project-rules / simplicity-efficiency) → independent
validation of every finding → fix the confirmed ones → re-review, looping until a
full round comes back with zero confirmed findings (or a 5-round safety cap forces
escalation). Claude acts as Orchestrator and dispatches agents for all review/
validate/fix work, keeping its own context clean.

**Rationale.** Per the user (2026-06-18): the Codex second-opinion validator was
valuable but is gone; replace the *functionality*, not by reviving Codex. The
2026-06-15 "Claude solo self-review" (rule #15) is weaker than an independent
adversarial reviewer — self-review shares the author's blind spots. A multi-agent
loop restores genuine independence: separate fresh-context agents review, a
separate agent adversarially validates each finding (killing false positives and
gold-plating, which guarantees convergence), and a separate agent fixes. This does
NOT revive Codex or Hermes — both stay decommissioned (see 2026-06-15 entry); this
is a from-scratch Claude-native replacement.

**Outcome.**
- New: `.claude/skills/adversarial-gate/SKILL.md`; `.claude/agents/adversarial-reviewer.md`,
  `finding-validator.md`, `fix-implementer.md`, and `focus-drift-auditor.md` (all
  `model: opus`, dispatched with maximum reasoning; reviewers + validator + auditor
  are read-only, only the fixer edits).
- `focus-drift-auditor` (added 2026-06-18, same session) is a trajectory guardrail,
  not a diff reviewer: every gate round it audits progress against the roadmap +
  coverage matrix to detect premature **title-fixation** — redirecting focus to make
  one retail title render before foundational XBE feature-surface work is done and
  the issue is isolated to that title (rule #17; the 2026-05-30→06-09 PGR2 sibling-
  sync relapse is the cautionary case). A confirmed `DRIFTING` verdict forces an
  `ESCALATED` gate result even when the code review is clean, and routes a course-
  correction to the user (never to the fixer). Testing a fix on a game stays fine;
  the auditor flags only *sustained* off-path focus.
- **CLAUDE.md rule #15 repurposed** from "Claude solo self-review" to "gate
  non-trivial work through `/adversarial-gate` until a CLEAN verdict" (numbering
  preserved; #16/#17 references stay valid). Skills list grown four → five.
- Durable evidence marker: `xemu-fork/.claude/state/adversarial-gate-last-run`
  (mirrors the retired `codex-validate-last-run` convention).
- Supersedes the self-review portion of the 2026-06-15 decision; the Codex/Hermes
  decommission in that entry stands.

## 2026-06-18 (task #10 `stencil-ops` root cause — guest XBE vertex-buffer-reuse race, NOT a renderer clear bug)

**Decision.** Record the corrected root cause for `stencil-ops` failing on Metal and the fix that landed. `stencil-ops` failed because of a **guest-side XBE vertex-buffer-reuse race in `scripts/apple-silicon/xbe-tests/stencil-ops/main.c`**, not a renderer clear-rect bug. The XBE reused one 6-vertex VRAM buffer, overwriting it in place per cell and drawing at `start=0` with no inter-cell GR drain; the host renderer reads vertices lazily from guest VRAM at flush, so TCG raced ahead of PGRAPH and every op/probe draw decoded the **last** cell's geometry → only the last cell rendered. This is the **same renderer-agnostic PFIFO/vCPU vertex-race class previously confirmed for `image-blit` (cycle-15)**; cross-renderer proof: GL renders the last 2 cells, Metal the last 1, same mechanism, pure timing.

**Rationale.** Instrumentation proved the Metal `SET_CLEAR_RECT` sub-rect clear path is correct: scissor is exactly the per-cell 160×240 rect across 3184 clears, and the clear is stencil/depth-only (never touches color). The clear path did not cause the bug; the task #14 per-clear sync only widened the timing window that exposed the latent guest bug. Fixing at the renderer level (the prior plan) would have been tuning the wrong layer, in violation of rule #1 (no guessing) and the XBE-first methodology.

**Outcome.**
- **Fix:** write-once vertex buffer in `stencil-ops/main.c` (mirrors `blend-matrix`; 96 verts, op at `idx*12`, probe at `idx*12+6`, built + memcpy'd once before the cell loop). `stencil-ops` now PASSes **deterministic 8/8 on Metal** (3 boots + `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` cross-check). The renderer was unchanged by the fix.
- **Retained:** the task #14 cross-queue clear→draw sync (`s_clear_done_event` MTLSharedEvent + `[cmd waitUntilCompleted]`, opt-out `XEMU_METAL_NO_CLEAR_SYNC=1`, default OFF) and the `SET_CLEAR_RECT` sub-rect scaffold are both correct and stay in tree.
- **Renderer cleanups (behavior-preserving):** removed dead `write_mask` generality from the clear-quad pipeline-cache key (only ever `MTLColorWriteMaskAll`) + corrected the overclaiming `k_clear_quad_msl` comment; removed leftover `metal_clear_subrect` instrumentation. Clean build: shader-validation 7/7 PASS, codesign valid, canary 4/4 green.
- **`pipeline-smoke` board-scoping fix (task #7):** Tier-4 `capture_blob` XBEs are now SKIPPED on the xemu drawable board (gated by `XbeManifest.is_tier4_capture_blob`) and routed to their real-Xbox XOSS oracle — the drawable board structurally can't validate a CPU-paint-and-reboot XBE, so the prior `FAIL` was a FALSE fail.
- **Coverage matrix regenerated** (`xbe-coverage-matrix.md`): Metal board now **14 PASS / 0 FAIL / 1 xfail (`logic-ops`) / 1 skip (`pipeline-smoke`) / 1 not-built (`msaa-aa-factor`)**; 38/91 surfaces covered.
- **Open follow-ups (non-blocking, task #8):** real-Xbox Tier-4 XOSS fresh-capture infra (`D:\` write not landing from the `E:\Apps` deploy); `xbe_compare` frame-selector hardening for tiny-signal references.

**Supersedes** the "stencil-ops failed because of a renderer cross-queue clear→draw race / `SET_CLEAR_RECT` clear-rect bug, fixed by task #14 clear-sync" framing recorded in the prior task #14 / task #10 handoff narrative, and the "scissor Y-origin flip" fix hypothesis in the 2026-06-15 handoff next-actions. Those are withdrawn: the clear path was correct, no defective `dist/` build exists, and the fix was guest-side.
