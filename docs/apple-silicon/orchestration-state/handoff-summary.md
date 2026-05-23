# Handoff Summary

- Cycle 17 closed the local renderer-agnostic milestone; cycle 18 packaged the state in a doc-only checkpoint.
- Cycle 19 attempted a real-Xbox parity check via the cycle-15 v0.4 image-blit XBE; reproducibly produced zero `D:\image-blit-capture.bin` / `D:\image-blit-done.txt` across two runs. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on decision DEFERRED.
- Cycle 20 took Path A: 13 staged `D:\image-blit-marker-NN-STAGE.txt` markers; local xemu validation showed all 13 fire; every fopen returned NULL on the ISO mount; two real-Xbox runs BOTH produced zero marker files; chainload→FTP-back stable at 22.4 s.
- Cycle 21 took Path A.2: re-route markers from `D:\` to `E:\Apps\image-blit\…`. Local validation 4 boots OK; single real-Xbox run produced clean NEGATIVE — 4th independent 22.4 s reproduction, `verdict.json status: ok`, ZERO marker files. Cycle-19 hypothesis #1 demoted from "leading" to "insufficient as sole explanation."
- Cycle 22 took Path A.3: provenance audit of `docs/apple-silicon/xbox-real-references/*` captures. All five reference sets WERE produced through the same `XLaunchXBE` chainload mechanism (HIGH confidence). Cycle-19 hypothesis #1 fully INVALIDATED. Image-blit failure re-classified as **image-blit-specific**. Cycle-22 leading hypothesis: image-blit crashes BEFORE main()'s first instruction.
- Cycle 23 took Path A.4: shipped non-fopen kernel-pool controller-buffer witness for image-blit + agent-side `witness.scan` RPC. Local xemu-Metal validation green (4 boots). Codex round-1 BLOCK → all 4 findings adopted → round-2 PASS_WITH_FINDINGS → MINOR PARTIAL closed post-round-2. Closure commit `5fce3b14e4`; doc-sync follow-up `b614bdbc83`.
- Cycle 24 ran the cycle-23 witness on real Xbox. CLOSED with a CONCRETE BLOCKER. Cycle-23 binaries deployed via FTP. Baseline `witness.scan` precondition MET. `runxbe E:\Apps\image-blit\default.xbe` issued at 2026-05-23T02:34:34Z. Xbox went fully silent for 928.3 s before measurement was aborted. Post-chainload `witness.scan` UNRECOVERABLE without a physical power-cycle that erases the persistent buffer. Cycle-22 leading hypothesis ("pre-main crash") is WEAKENED but not corroborated or invalidated. NEW hypothesis #5: the kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context. Closure commit `487e729d4f`; doc-sync follow-up `29a455a78e`.
- **Cycle 25 implemented the cycle-24-recommended witness-only diagnostic XBE.** SHIPPED + CLOSED. New `scripts/apple-silicon/xbe-tests/witness-only/{main.c, Makefile, manifest.json, README.md, .gitignore}` + built `bin/default.xbe` (147 456 B) + `witness-only.iso` (720 896 B). main.c body: ~10 statements — `xbed_a4_witness_fire(MAIN_ENTERED)` → `Sleep(500)` → `xbed_a4_witness_fire(POST_MARKER0)` → `Sleep(500)` → `HalReturnToFirmware(HalRebootRoutine)` plus three `xbed_host_log_write*` anchor lines for `XEMU_GUEST_LOG=1` visibility. NO `xbed_init`, NO pbkit, NO NV2A, NO `XVideoSetMode`, NO `fopen`, NO `image_blit_marker_*`. Built via `lib/lib.mk` exactly the way image-blit does — keeps linked `.text` invariant. Local xemu-Metal smoke green: 9× `main() entered`, 9× `enter stage=1`, 9× `fire1 returned`, 9× `enter stage=3`, 9× `fire2 returned`, 8× `rebooting via HalReturnToFirmware` lines across a 25 s timeout window. Codex 3-round validation: round 1 = MAJOR ISSUES 4 findings (HIGH overclaim + HIGH state overstatement + MEDIUM gitignore + LOW count) all adopted; round 2 = BLOCK on residual #1 PARTIAL + new LOW (current-cycle.md disagreement) both adopted; **round 3 = PASS_WITH_FINDINGS**, all blocking + medium + low RESOLVED, no new issues, validation marker written. Cycle 25 does NOT include the real-Xbox run — cycle 26 is Hermes's call.

## Cycle 25 design + outcome (locked at session close 2026-05-22)

| Step | Action | Outcome |
|---|---|---|
| 1 | Read required docs + post worker receipt | Done; plan summarized to current-cycle.md + claude-status.md before deeper work |
| 2 | Author witness-only files | `main.c`, `Makefile`, `manifest.json`, `README.md`, `.gitignore` |
| 3 | Build via `make` + nxdk | `bin/default.xbe` 147 456 B, `witness-only.iso` 720 896 B; lib.mk pattern identical to image-blit's |
| 4 | Local xemu-Metal smoke validation | 9× main() entered, 9/9 fire1+fire2 pairs, 8× reboots in 25 s; loops correctly within the timeout window |
| 5 | Codex round 1 (changes mode) | MAJOR ISSUES — 4 findings; all adopted |
| 6 | Codex round 2 | BLOCK on residual #1 PARTIAL + new LOW; both adopted |
| 7 | Codex round 3 | **PASS_WITH_FINDINGS**; round-2 #1 RESOLVED; round-2 LOW PARTIAL addressed; no new issues; validation marker written |
| 8 | Canonical docs sync | handoff.md cycle-25 entry on top; decision-log.md cycle-25 entry above cycle-24; orchestration-state quartet closure pass |
| 9 | Closure commit on `apple-silicon-performance` | (recorded after this update) |

## Cycle 25 scope discipline

- 5 NEW files under `scripts/apple-silicon/xbe-tests/witness-only/` (main.c, Makefile, manifest.json, README.md, .gitignore) + 2 built artifacts (bin/default.xbe, witness-only.iso).
- ZERO changes to `lib/xbed_a4_witness.{c,h}` (cycle-23 implementation is binding).
- ZERO changes to `oracle-agent/` (its `witness.scan` reader is sufficient).
- ZERO changes to image-blit (sibling XBE; image-blit stays untouched).
- ZERO xemu-fork host source touched.
- ZERO flag default flips.
- ZERO retail-title / §G.5 / RT-as-texture / second-wave XBE work.
- ZERO real-Xbox deployment (cycle 26 is Hermes's call).
- Cycle 25 closes cleanly; canonical docs + orchestration-state quartet synced; evidence preserved on disk; Codex validation marker recorded.

## Important scope clarification (Codex round-1 #1 finding, adopted)

Cycle 25 substitutes a passive `Sleep(500)` for image-blit's intermediate `image_blit_marker(0, ...)` call between the two witness fires. A successful cycle-26 outcome (orphan with `reserved[0]==0xA4000003` + clean reboot) therefore proves the witness mechanism is real-Xbox-safe IN THIS MINIMAL XBE, and that image-blit's hang is in code ABSENT from witness-only. The ABSENT code set is pbkit / NV2A / xbed_init / xbed_render_loop_then_capture AND the marker helper itself. **Independently excluding the marker helper as a contributor requires a follow-on cycle (cycle 26.5 / cycle 27 candidate) that runs the actual marker between the two fires.** This caveat is stated explicitly in main.c header, README.md, manifest.json, current-cycle.md, handoff.md cycle-25 entry, and the decision-log cycle-25 entry.

## Next bounded slice (cycle 26 — Hermes-scheduled real-Xbox deployment)

Hard precondition: physically power-cycle the Xbox if multiple A.4-tagged orphans pre-exist (cycle-24 left a stale persistent buffer; if Hermes ran additional attempts in the same power session, prior orphans would accumulate). Sequence:

1. `oracle-orchestrator.py ensure-agent` (cycle-23 build of oracle-agent must be deployed; provides `witness.scan` verb).
2. Baseline `oracle-client.py raw witness.scan` — must show exactly 1 live `oracle_ctrl_buffer` with `reserved[0]==0`.
3. FTP-upload `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` to `/E/Apps/witness-only/default.xbe`.
4. `oracle-client.py runxbe 'E:\Apps\witness-only\default.xbe'`.
5. Poll FTP/21 + agent/9001 + ICMP ping (cycle-24 poll pattern from `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-*/03-chainload-image-blit.log` is appropriate).
6. On dashboard return: `ensure-agent` again + `witness.scan`. Inspect highest-phys orphan.

Branch results:

- Reboots in ~5..15 s + `reserved[0]==0xA4000003` orphan → witness mechanism IS real-Xbox-safe IN THIS MINIMAL XBE; image-blit's hang is in code ABSENT from witness-only (pbkit / NV2A / xbed_init / draw AND the marker helper); cycle-22 leading hypothesis INVALIDATED; cycle 27 splits image-blit's instrumentation across multiple smaller discriminator XBEs.
- Hangs identically to cycle 24 → witness mechanism itself is real-Xbox-incompatible; redesign required (EEPROM scratchpad / non-MMIO-aliased RAM / abandon in-XBE witness).
- Reboots cleanly + `reserved[0]==0xA4000001` orphan → witness fires once but second fire hangs (less likely; worth surfacing for cycle 27 design).

## Codex validation

Cycle 25 ran 3 rounds. Round 3 = PASS_WITH_FINDINGS with all blocking + medium + low findings RESOLVED. Validation marker recorded at `.claude/state/codex-validate-last-run`.

## Evidence on disk

- `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-20260523T034519Z/xemu.log` — full xemu-Metal stderr/stdout across the smoke run (`benchmark-runs/` gitignored per project convention).
- `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-20260523T034519Z/summary.txt` — anchor-line counts.

## Closure commit

To be recorded as soon as the cycle-25 closure commit lands on `apple-silicon-performance` after this update.
