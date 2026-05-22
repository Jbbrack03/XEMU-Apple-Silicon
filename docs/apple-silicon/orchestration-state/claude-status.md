# Claude Status

- Objective: cycle 22 Path A.3 — provenance audit of `docs/apple-silicon/xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor,controller-roundtrip}`. Determine, with file-backed evidence, whether those reference captures were produced through the same `runxbe`-SITE-EXEC chainload path used by the current oracle workflow (the path that cycle-19+cycle-20+cycle-21 reproducibly proved blocks image-blit witness writes across both `D:\` and `E:\Apps\image-blit\…`) or through a meaningfully different launch path. Doc/evidence-only slice; no code changes.
- Status: **CLOSED — provenance audit completed with HIGH-confidence verdicts; cycle-19 hypothesis #1 fully invalidated; cycle-21 interpretation materially sharpened; canonical docs synced; doc/state commit pending.**
- Active session: cycle 22 (Claude Code worker, Claude Max via /Users/jbbrack03/.local/bin/claude-max-shell), fresh bounded session. HEAD at start = `edee829e49` (cycle-21 Path A.2 docs/state closure). Cycle-21 closure commit was `df8eb65efc`.

## Outcome (one-paragraph)

Inventoried `docs/apple-silicon/xbox-real-references/` (six PNG files, zero embedded README/metadata/provenance docs); traced each capture file to its introducing commit (`aae0138565` 2026-05-06 15:32 CDT for pipeline-smoke; `823733f2e6` 2026-05-06 23:03 CDT for color-channel/depth-floor/mirror; `58bf218838` 2026-05-07 10:39 CDT for controller-roundtrip + mirror/composite); read the orchestrator + xbe-harness + oracle-agent `cmd_runxbe` source at those commits AND at HEAD AND diffed them. **Finding (HIGH confidence, all five sets):** every reference capture was produced via the same diag-XBE chainload mechanism cycle-21 used — orchestrator `run_diag` → `OracleClient.runxbe(path)` RPC → agent `cmd_runxbe` → `XLaunchXBE(path)` kernel call (`scripts/apple-silicon/xbe-tests/oracle-agent/commands.c:344-380`, unchanged between capture-time and cycle-21 modulo cosmetic path-arg parsing rework + SMC fan-curve cleanup). Per-set audit table in handoff.md + decision-log.md cycle-22 entries. Cross-referenced with dashboard-transition commit `e74715cd71` 2026-05-06 19:21 CDT: pipeline-smoke captured pre-switch under XBMC4Gamers/SITE RunXBE; all four other captures post-switch under UnleashX/SITE EXEC (same dashboard cycle-21 used). **Cycle-19 hypothesis #1 ("D:\\ remap mismatch / runxbe-SITE-EXEC chainload blocks witness-path file writes") is fully INVALIDATED** (cycle 21 had it demoted to "insufficient as sole explanation"; cycle 22 falsifies it via comparator evidence: five reference captures wrote D:\\<id>-capture.bin successfully via this path). Image-blit failure across cycle 19+20+21 is conclusively re-classified as **image-blit-specific**. Cycle-22 leading hypothesis: image-blit crashes BEFORE its main() body's first instruction (`image_blit_marker(0, "program_entered")`) completes — i.e. CRT init, static-init, DllCharacteristics, or pre-main XBE thunking. Next bounded slice (NOT promoted this cycle): **Path A.4** (non-fopen kernel-pool controller-buffer witness from cycle-21's proposed follow-up list) — A.3 makes A.4 the right discriminator because the launch-path-blocker explanation is now untenable.

## Evidence preserved

Provenance trace (file-backed):
- `git log --diff-filter=A` per-capture-file → introducing commits identified above; commit messages corroborate orchestrator pipeline used.
- `git show <intro>:scripts/apple-silicon/oracle-orchestrator.py` confirms `run_diag` body at capture time used `client.runxbe(xbe_path)` → identical to HEAD.
- `git show c2274310fc:scripts/apple-silicon/xbe-tests/oracle-agent/commands.c` confirms `cmd_runxbe` body at first introduction called `XLaunchXBE(path)` → identical to HEAD (modulo two cosmetic edits that don't touch the kernel call).
- Dashboard timeline: `e74715cd71` 2026-05-06 19:21 CDT = UnleashX switch; pipeline-smoke (15:32 CDT) is pre-switch, all others post-switch.

No new artifacts generated; no benchmarks run; no real-Xbox runs taken.

## Diff at session close (uncommitted, doc/state-only)

- `docs/apple-silicon/handoff.md` — cycle-22 entry on top; duplicate header cleanup pass; cycle-17/19/20/21 entries preserved unchanged.
- `docs/apple-silicon/decision-log.md` — cycle-22 entry above cycle-21; cycle-17 / cycle-19 / cycle-20 / cycle-21 NOT superseded.
- `docs/apple-silicon/orchestration-state/{current-cycle.md, claude-status.md, validation-status.md, handoff-summary.md}` — all four files updated for cycle 22.
- Zero source/code/XBE-binary files touched.
- `.hermes_cycle22_path_a3_prompt.txt` — untracked Hermes-side scratch at workspace root; intentionally not staged.

## Canonical docs synced

- `docs/apple-silicon/handoff.md` — cycle-22 Path A.3 entry written; cycles 17/19/20/21 preserved.
- `docs/apple-silicon/decision-log.md` — cycle-22 entry written; cycles 17/19/20/21 NOT superseded.
- `docs/apple-silicon/orchestration-state/*` — all four files reflect cycle-22 outcome.

## Codex validation

**SKIPPED under rule #15's "doc-only changes" carve-out.** Zero code changes; aggregate edits are markdown-only. Rule #15's three triggers (substantive plan; non-trivial uncommitted code > 30 lines in xemu-fork/ source; stuck for 3 attempts / 2 failed hypotheses) all N/A for this slice. Per-slice justification recorded in `validation-status.md` (the assignment's exit criterion 4) and in the cycle-22 decision-log entry's "Why this is doc-only and not Codex-validated" paragraph. No validation marker written at `.claude/state/codex-validate-last-run`.

## Next bounded slice (NOT promoted this cycle)

- **Path A.4 (now top-priority per rule #1).** Add a non-fopen witness to image-blit: write a few bytes into the oracle-agent's persistent kernel-pool controller buffer (`oracle_ctrl_buffer` at the agent-published phys address) BEFORE attempting any marker fopen. Next agent boot reads the buffer via `controller.buffer-info` / `controller.get`. Discriminates the cycle-22 leading hypothesis ("image-blit crashes before main() first instruction") from the alternative ("image-blit reaches first instruction but crashes in xbed_init / pbkit / NV2A").
- **Path B.** Smaller PFIFO-race-only Tier-1 diag XBE that captures via PCRTC. Heavier than A.4; better suited if A.4 is inconclusive.
- **Path C (new cycle 22).** If A.4's witness fires, instrument xbed_init / pbkit-init / first NV2A call per-section. If A.4's witness does NOT fire, the crash is in CRT / static-init / XBE thunking — would need either a smaller image-blit variant or XBE-file-level analysis (DllCharacteristics, kernel imports, section layout).

Scope choice belongs to the next Hermes pass.
