# Validation Status

- Active slice: cycle 31 Path A.4 option (d) on-screen visual breadcrumb — IMPLEMENTATION-ONLY bounded code slice; ZERO real-Xbox run; cycle-32 real-Xbox deployment is Hermes's call.
- Validation state: **CLOSED. Codex 3-round green.** Round 1 = MAJOR ISSUES (3 high + 1 low); all 4 adopted. Round 2 = MAJOR ISSUES (1 new high + 1 new low; round-1 findings all RESOLVED); both adopted. Round 3 = LOOKS GOOD with no new findings. Validation marker recorded at `.claude/state/codex-validate-last-run`.

## Rule #15 applicability (cycle 31)

Rule #15 mandates Codex validation for non-trivial uncommitted source diffs (renderer / TCG / NV2A / build / runtime flag plumbing / apple-silicon scripts; aggregate > 30 lines). Cycle 31's diff is ~250 lines across:

- `scripts/apple-silicon/xbe-tests/witness-only/main.c`: cycle-31 head-comment addendum (~50 LOC), 5-stripe color table (~12 LOC), `xbed_breadcrumb_init` 3-state machine (~30 LOC), `xbed_breadcrumb_paint` (~22 LOC), 5 paint call sites with cycle-31 rationale comments, Sleep extension with cycle-31 rationale block.
- `scripts/apple-silicon/xbe-tests/witness-only/README.md`: cycle-31 addendum (~120 LOC) including 5-stripe color map, 8-row F1..F8 cycle-32 discriminator table, 10-step cycle-32 deployment runbook, build artifact sizes, cross-references update.
- `scripts/apple-silicon/xbe-tests/witness-only/manifest.json`: title extension, purpose paragraph extension, new `real-xbox/physical/cycle-32` `expected_results` section (~60 LOC).
- Paired canonical doc updates: `docs/apple-silicon/handoff.md` + `docs/apple-silicon/decision-log.md` cycle-31 entries; orchestration-state quartet closure pass.
- Rebuilt binary artifacts: `witness-only/bin/default.xbe` 155 648 B + `witness-only.iso` 720 896 B.

Rule #15 firmly fires; the doc-only carve-out does NOT apply (substantive C-source edits in `witness-only/main.c`).

## Codex round 1 findings (all adopted)

| # | Severity | Where | Finding | Adoption |
|---|---|---|---|---|
| 1 | HIGH | `docs/apple-silicon/{handoff.md, decision-log.md, orchestration-state/*.md}` | Orchestration-state files claimed cycle-31 canonical docs were synced, but `handoff.md` / `decision-log.md` / `validation-status.md` / `handoff-summary.md` still described cycle 30. | Canonical docs actually synced this round: handoff.md cycle-31 entry on top with cycle-30 preserved unchanged; decision-log.md cycle-31 entry on top (no supersession); this file rewritten; handoff-summary.md cycle-31 entry on top. |
| 2 | HIGH | `witness-only/README.md` + `manifest.json` cycle-32 F1 row | F1 said "both witness mechanisms landed" when `witness.scan` stays D-cycle-27 — which means XCTR did NOT land. Internally contradictory with F6 ("full success across BOTH mechanisms"). Would mislead the cycle-32 operator. | F1 wording rewritten: "main() ran past every checkpoint AND WTNS landed AND XCTR did NOT land (witness.scan still D-cycle-27 means no XCTR stamp survived on the agent's persistent buffer). γ INVALIDATED; (α) OR (β) is the XCTR-side failure mode. Cycle 33 promotes option (b) for α-vs-β discrimination on the XCTR side." F6 is now the only row that means both succeeded. Both surfaces (README + manifest) updated. |
| 3 | HIGH | `witness-only/main.c` `xbed_breadcrumb_init` + paired doc claims | Init retried `XVideoSetMode` on every paint after a prior failure, broadening the risk surface beyond the "single XVideoSetMode call" claim and weakening the γ.1 interpretation. A graceful FALSE return is distinct from a crash. | `xbed_breadcrumb_init` rewritten as a 3-state machine (UNTRIED / OK / FAILED): on graceful FALSE return latches FAILED so `paint(1..4)` cannot re-enter the kernel display init path. The single XVideoSetMode call is concentrated strictly at paint(0)'s invocation. F4 wording updated in README + manifest + claude-status.md to include graceful mode rejection alongside γ.0 / γ.1. |
| 4 | LOW | `witness-only/main.c` file banner header | Banner still said "NO XVideoSetMode," which became false in cycle 31. Misleads anyone skimming the file for context. | Banner rewritten to acknowledge XVideoSetMode is in scope (cycle-25 "NO XVideoSetMode" invariant no longer holds) and enumerate the remaining cycle-25 invariants that still hold (no pbkit / no NV2A class objects / no xbed_init / no file I/O). |

Codex round-1 open question #1 ("missing cycle-31 sync — intentional or omitted?"): the orchestration-state files were prepared optimistically; the actual canonical-doc sync was scheduled to happen later in this same session but Codex's reviewer ran against the in-flight tree. Both states (orchestration claim + actual sync) are now consistent.

Codex round-1 open question #2 ("F1 meaning"): clarified explicitly per finding #2's adoption — F1 is "WTNS only" and F6 is "BOTH mechanisms succeeded."

## Codex round 2 findings (all adopted)

Round 2 confirmed all 4 round-1 adoptions RESOLVED, then surfaced 1 new HIGH + 1 new LOW:

| # | Severity | Where | Finding | Adoption |
|---|---|---|---|---|
| 5 | HIGH | `witness-only/README.md` F4 row + `manifest.json` F4 + `decision-log.md` + `current-cycle.md` + `handoff-summary.md` + `handoff.md` | F4 row claimed it included "a graceful XVideoSetMode FALSE return" alongside (γ.0)/(γ.1), AND it claimed "cycle-22 pre-main FULLY CORROBORATED." Self-contradictory: a graceful FALSE return means `main()` DID execute past its first instruction (XVideoSetMode was CALLED and RETURNED), so it does NOT corroborate "pre-main" anything. | F4 was rewritten to cover only γ.0 (`main()` never entered) and γ.1 (XVideoSetMode crash). A new row F4' was added for the distinct graceful-FALSE case: F4' = "no stripes visible + `witness.scan-self count=1` + `witness.scan D-cycle-27`" = main() ran past paint(0) (which became a no-op when the helper latched FAILED) and continued through the cycle-29 self-witness fires, which still execute and stamp the WTNS page. F4' INVALIDATES γ via the WTNS path; cycle 33 investigates AV-encoder rejection cause. Updated in all 6 surfaces. Discriminator-table row count went from 8 to 9. |
| 6 | LOW | `witness-only/README.md` line ~362 stripe-map prose | Stale claim "All 5 stripes visible = the full witness path executed (and the cycle-32 readback should be E1 shape)" — but the new F1/F5/F6 rows give 3 different interpretations for "all 5 stripes visible" combined with different two-tuple shapes. | Stripe-map prose rewritten to explicitly say "All 5 stripes visible = the full witness path through `main()` executed; the cycle-32 readback shape is then disambiguated by combining stripe count with the (witness.scan, witness.scan-self) two-tuple per the F1 / F5 / F6 rows of the cycle-32 discriminator table below." |

Codex round-2 open question #1 ("Claude.md vs CLAUDE.md path"): the workspace has both `CLAUDE.md` (project-level, present) and `xemu-fork/CLAUDE.md` (subdirectory-level, present); Codex's read-only sandbox may not enumerate the workspace-root CLAUDE.md when invoked with `-C xemu-fork`. Not load-bearing for this review.

Codex round-2 open question #2 ("nxdk paths not present"): the `nxdk/` SDK lives at `/Users/jbbrack03/XEMU_MacOS/nxdk/` (one level above `xemu-fork/`), outside the Codex `-C` sandbox. The cross-references in `witness-only/README.md` to `nxdk/lib/hal/video.{h,c}` are intentional external SDK references. Verified locally during cycle-31 implementation.

## Codex round 3 verdict

LOOKS GOOD. Round-1 + round-2 findings all RESOLVED. No new findings. Validation marker written at `.claude/state/codex-validate-last-run`.

## Gate status (cycle 31) — final

- [x] Required docs read (handoff.md cycle-30 entry, decision-log.md cycle-30 entry, orchestration-state quartet, witness-only/README.md, witness-only/main.c, witness-only/Makefile, witness-only/manifest.json, lib/xbed_runtime.{c,h}, nxdk/lib/hal/video.{h,c}).
- [x] Repo/git state confirmed; 3 pre-existing `.hermes_cycle*.txt` prompt files preserved un-staged.
- [x] `witness-only/main.c` modified (head-comment addendum + 2 static helpers + 5 paint sites + Sleep extension; latched-init state machine adopted per Codex round-1 HIGH #3).
- [x] `witness-only/README.md` cycle-31 addendum + 5-stripe map + 8-row F1..F8 cycle-32 discriminator table + 10-step runbook (F1 wording corrected per Codex round-1 HIGH #2; F4 wording corrected per Codex round-1 HIGH #3).
- [x] `witness-only/manifest.json` title + purpose + `real-xbox/physical/cycle-32` expected_results (F1 + F4 wording aligned with README).
- [x] `witness-only/bin/default.xbe` rebuilt (155 648 B, +4 096 B from cycle 29).
- [x] Canonical docs synced (handoff.md, decision-log.md, this file, handoff-summary.md, current-cycle.md, claude-status.md).
- [x] Codex round 1 (changes mode) ran; verdict MAJOR ISSUES with 3 high + 1 low.
- [x] Round-1 findings adopted.
- [x] Codex round 2 ran; verdict MAJOR ISSUES with 1 new high + 1 new low (all 4 round-1 findings RESOLVED).
- [x] Round-2 findings adopted (F4 split into F4 + F4'; stripe-map prose updated).
- [x] Codex round 3 ran; verdict LOOKS GOOD with no new findings; round-1 + round-2 findings all RESOLVED.
- [x] Validation marker `.claude/state/codex-validate-last-run` written.
- [x] Closure commit landed as `41f350c174` on `apple-silicon-performance`.

## Codex validation marker

Validation marker written at the end of round 3 once round-1 + round-2 findings were all RESOLVED and round 3 returned LOOKS GOOD. Tied to the cycle-31 implementation slice; supersedes the cycle-29 marker.

## Why this is not a regression of any prior cycle's validation guarantees

- Cycle 23 / 25 / 27 / 29 each Codex-validated their own implementation slices. Cycle 31 does not touch any of those slices' code: `lib/xbed_a4_witness.{c,h}` intact, `lib/xbed_self_witness.{c,h}` intact, `oracle-agent/*` intact, `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit intact.
- Cycle 31 modifies only `witness-only/main.c` + paired doc surfaces. Cycle 31's Codex 3-round green attests to the cycle-31-specific changes; prior cycles' guarantees remain valid.

## Evidence integrity

- Rebuilt binary artifacts checked into the repo: `witness-only/bin/default.xbe` 155 648 B + `witness-only.iso` 720 896 B.
- Codex transcripts preserved in the session's tool-result files under `/Users/jbbrack03/.claude/projects/.../tool-results/`.
- Validation marker `.claude/state/codex-validate-last-run` updated at round-3 completion.

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing.
- No shared-lib edits; no oracle-agent edits.
- No real-Xbox run; cycle-32 deployment is Hermes's call.
- No cycle-32 promotion or implementation.
- No retail-title / §G.5 / RT-as-texture work.
