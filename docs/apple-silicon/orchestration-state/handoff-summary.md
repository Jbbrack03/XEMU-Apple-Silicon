# Handoff Summary

- Cycle 17 closed the local renderer-agnostic milestone; cycle 18 packaged the state in a doc-only checkpoint.
- Cycle 19 attempted a real-Xbox parity check via the cycle-15 v0.4 image-blit XBE; reproducibly produced zero `D:\image-blit-capture.bin` / `D:\image-blit-done.txt` across two runs. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on decision DEFERRED.
- Cycle 20 took Path A: 13 staged `D:\image-blit-marker-NN-STAGE.txt` markers; local xemu validation showed all 13 fire; every fopen returned NULL on the ISO mount; two real-Xbox runs BOTH produced zero marker files; chainload→FTP-back stable at 22.4 s.
- Cycle 21 took Path A.2: re-route markers from `D:\` to `E:\Apps\image-blit\…`. Local validation 4 boots OK; single real-Xbox run produced clean NEGATIVE — 4th independent 22.4 s reproduction, `verdict.json status: ok`, ZERO marker files. Cycle-19 hypothesis #1 demoted from "leading" to "insufficient as sole explanation."
- Cycle 22 took Path A.3: provenance audit of `docs/apple-silicon/xbox-real-references/*` captures. All five reference sets WERE produced through the same `XLaunchXBE` chainload mechanism (HIGH confidence). Cycle-19 hypothesis #1 fully INVALIDATED. Image-blit failure re-classified as **image-blit-specific**. Cycle-22 leading hypothesis: image-blit crashes BEFORE main()'s first instruction.
- Cycle 23 took Path A.4: shipped non-fopen kernel-pool controller-buffer witness for image-blit + agent-side `witness.scan` RPC. Local xemu-Metal validation green. Codex round-1 BLOCK → all 4 findings adopted → round-2 PASS_WITH_FINDINGS → MINOR PARTIAL closed post-round-2. Closure commit `5fce3b14e4`; doc-sync follow-up `b614bdbc83`.
- Cycle 24 ran the cycle-23 witness on real Xbox. CLOSED with a CONCRETE BLOCKER. Cycle-23 binaries deployed via FTP. Baseline `witness.scan` precondition MET. `runxbe E:\Apps\image-blit\default.xbe` issued. Xbox went fully silent for 928.3 s before measurement was aborted. Post-chainload `witness.scan` UNRECOVERABLE without a physical power-cycle that erases the persistent buffer. Cycle-22 leading hypothesis ("pre-main crash") WEAKENED. NEW hypothesis #5: kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context. Closure commit `487e729d4f`; doc-sync follow-up `29a455a78e`.
- Cycle 25 implemented the cycle-24-recommended witness-only diagnostic XBE. SHIPPED + CLOSED. 5 new files under `scripts/apple-silicon/xbe-tests/witness-only/` + built bin/default.xbe (147 456 B) + witness-only.iso (720 896 B). Codex 3-round green.
- Cycle 26 ran the cycle-25 witness-only XBE on real Xbox. CLOSED — outcome shape D. ~70.17 s chainload→dashboard-ready vs 20.67 s invalid-path control vs ~36 s mirror control. Post-run `witness.scan` after EVERY chainload identical to baseline; NO orphan observed across 4 chainloads. Hypothesis #5 PARTIALLY INVALIDATED in the catastrophic-hang sense. Stamp-vs-no-stamp ambiguity OPEN. Cycle-22 leading hypothesis status: still WEAKENED. Closure commit `a31e061144`.
- **Cycle 27 implemented option (a) from the cycle-26 closure's four candidate cycle-27 designs:** `oracle-agent/controller.c::s_allocate_fresh` now PRESERVES an existing plausible `oracle_ctrl_buffer` witness header across agent restart instead of unconditionally `memset`-wiping it. New static helper `s_page_has_plausible_witness_header(vp)` with predicate TIGHTENED to a strict subset of the cycle-23 scan filter (Codex round-1 medium): accepts only `(reserved0==0, reserved1==0)` or `((reserved0>>24)==0xA4, 1<=reserved1<=4096)`. Preserve branch clears only `port[]`; legacy branch keeps full-zero + re-stamp magic/version. One conditional `debugPrint` breadcrumb when preserve fires. Paired doc edits in `controller.h`, `commands.c::cmd_witness_scan` body comment, `witness-only/README.md` discriminator table, and `witness-only/manifest.json` add the new positive success shape `count=1 live=1 reserved0=0xA4xxxxxx` alongside the legacy orphan shape. oracle-agent XBE rebuilt (417 792 B; size unchanged). Codex 3-round: round 1 = MINOR ISSUES (medium + low; both adopted); round 2 = MINOR ISSUES (round-1 MEDIUM RESOLVED; round-1 LOW PARTIAL adopted); round 3 = LOOKS GOOD. Validation marker written. ZERO host source touched; ZERO image-blit / witness-only / xbed_a4_witness source touched. Two pre-existing untracked `.hermes_*.txt` prompt files at repo root NOT staged.

## Cycle 27 design + outcome (locked at session close 2026-05-23)

| Step | Action | Outcome |
|---|---|---|
| 1 | Read required docs + state files + cycle-23/26 source | Plan confirmed; existing cycle-23 lockstep filter provides the contract for the cycle-27 preserve gate |
| 2 | Implement `s_page_has_plausible_witness_header` + two-branch `s_allocate_fresh` + debugPrint breadcrumb + paired doc updates | 5 files modified (controller.c substantive; controller.h + commands.c + witness-only/README.md + witness-only/manifest.json doc-only) |
| 3 | Rebuild oracle-agent XBE via nxdk make | bin/default.xbe 417 792 B (size unchanged); oracle-agent.iso 983 040 B |
| 4 | Codex validation round 1 (changes mode) | MINOR ISSUES: medium predicate-too-loose + low witness-only-docs-orphan-only-shape. Both adopted in full |
| 5 | Rebuild oracle-agent XBE after round-1 adoption | size still unchanged; manifest.json reparses cleanly |
| 6 | Codex validation round 2 | MINOR ISSUES: round-1 MEDIUM RESOLVED; round-1 LOW PARTIAL on one manifest field. Adopted |
| 7 | Codex validation round 3 | LOOKS GOOD: round-2 PARTIAL RESOLVED; round-1 MEDIUM still RESOLVED; no new findings |
| 8 | Write validation marker | `.claude/state/codex-validate-last-run` updated |
| 9 | Canonical docs sync | handoff.md cycle-27 entry on top; decision-log cycle-27 entry above cycle-26; orchestration-state quartet closure pass |
| 10 | Closure commit on `apple-silicon-performance` | Landed as `df999e41ea` |

## Cycle 27 scope discipline

- ZERO xemu-fork host source touched (no `hw/`, `ui/`, `target/`, `include/`).
- ZERO image-blit source touched.
- ZERO witness-only source touched (only paired doc edits in README.md + manifest.json).
- ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 scan filter intentionally stays wider than the cycle-27 preserve gate; lockstep contract documented in three comment blocks).
- ZERO XBE rebuilds beyond `oracle-agent`.
- ZERO real-Xbox run.
- ZERO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- ZERO flag default flips.
- ZERO cycle-27 options (b)/(c)/(d) pursued.
- Two pre-existing untracked `.hermes_cycle22_path_a3_prompt.txt` + `.hermes_cycle23_docsync_prompt.txt` at repo root preserved un-staged per Hermes pre-session instruction.

## Important interpretation notes

- The preserve gate is INTENTIONALLY a strict subset of the cycle-23 scan filter. A false negative just falls back to legacy full-zero behavior (safe; just the cycle-26 behavior we observed). A false positive would silently retain garbage as if it were a real witness header (would corrupt cycle-28 readback interpretation). Asymmetric failure modes justify the asymmetric strictness.
- Local validation in standalone xemu does NOT meaningfully exercise the preserve branch because xemu's `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory` semantics don't deterministically reproduce the real-Xbox kernel-pool reuse pattern observed in cycle 26. Build success + 3-round Codex green is the appropriate high-confidence verification for this slice; the real-Xbox preserve-branch path is cycle-28 scope.
- The new positive success shape `count=1 live=1 reserved0=0xA4xxxxxx` and the legacy orphan shape are BOTH valid cycle-28 success outcomes. Operators reading any of the four updated locations (commands.c comment, witness-only README, manifest `artifacts.witness_readback.notes`, manifest `expected_results.real-xbox/physical/cycle-26.notes`) see both shapes; the ambiguity between "fire didn't land" and "fire landed but agent wiped it" is now resolvable from the next real-Xbox readback.
- The cycle-26 ~70 s recovery shape (slow kseg0 scan vs delayed-fault watchdog vs slow BIOS POST) is NOT addressed by the preserve branch. Cycle 28 should still discriminate this — the preserve branch only sharpens the post-chainload readback.

## Next bounded slice (cycle 28 — Hermes-scheduled)

Re-deploy the cycle-27 `oracle-agent/bin/default.xbe` (size unchanged at 417 792 B but preserve branch now live), reuse cycle-25 `witness-only/bin/default.xbe` as-is. Canonical sequence + the `curl FTP LIST` poll methodology that cycle 26 encoded. Power-cycle Xbox first if multiple A.4-tagged buffers pre-exist. Sequence: ensure-agent → baseline `witness.scan` precondition (count=1 live=1 reserved0=0) → FTP-upload new oracle-agent → relaunch agent → `runxbe witness-only` → poll FTP/21 + agent/9001 + ICMP ping → on dashboard return restart agent + query `witness.scan`. Expected positive shapes: A1 (`count=1 live=1 reserved0=0xA4000003`) — preserve branch retained the live-buffer stamp on the reused phys; A2 (`count>=2` with stamped orphan reserved0=0xA4000003) — legacy orphan shape if the kernel pool returned a different phys this time. Either A1 or A2 means cycle-22 leading hypothesis INVALIDATED. D-cycle-27 (`count=1 live=1 reserved0=0`) means the stamp never landed; cycle 29 promotes option (c) or (d). B (identical 928s+ hang) means hypothesis #5 promoted back; redesign required.

Cycle 28 should also re-attempt to discriminate the ~70 s recovery shape via an on-screen visual breadcrumb (`oracle-orchestrator.py capture` mid-run) or composite capture of the cycle-27 `debugPrint` breadcrumb during agent re-launch.

## Codex validation

Cycle 27 ran 3 rounds (rule #15 mandatory — non-trivial diff on renderer-adjacent oracle-agent C source). Round 3 = LOOKS GOOD with no new findings; round-1 + round-2 findings all RESOLVED. Validation marker recorded.

## Evidence on disk

- No new `benchmark-runs/` directory this cycle (no real-Xbox run; no XBE harness sweep).
- Build artifacts: `scripts/apple-silicon/xbe-tests/oracle-agent/{bin/default.xbe, oracle-agent.iso}` (committed; size unchanged from cycle 26 but content differs).

## Closure commit

Landed as `df999e41ea` on `apple-silicon-performance`.
