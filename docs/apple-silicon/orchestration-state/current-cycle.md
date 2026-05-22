# Current Cycle

- Cycle: 22 Path A.3 (**CLOSED — 2026-05-22 19:18 CDT**).
- Started: 2026-05-22 18:41 CDT.
- Worker receipt posted: 2026-05-22 18:41 CDT.
- State: CLOSED — provenance audit completed with HIGH-confidence verdicts; cycle-21 interpretation materially sharpened; canonical docs synced; planned commit covers doc/state changes only.
- Owner: Claude Code worker (Claude Max via /Users/jbbrack03/.local/bin/claude-max-shell), fresh bounded session.
- HEAD at start: `edee829e49` (cycle-21 Path A.2 docs/state closure).
- Cycle-21 closure commit (HEAD-1): `df8eb65efc` (cycle-21 Path A.2 marker re-route + canonical doc sync).
- Bounded goal (as assigned): determine, with file-backed evidence, whether the existing `xbox-real-references/` reference captures were produced through the same `runxbe` SITE-EXEC chainload path used by the current oracle workflow, or through a meaningfully different launch path.
- Result: **YES, all five reference capture sets (pipeline-smoke / mirror / color-channel / depth-floor / controller-roundtrip + mirror-composite) were produced through the same `XLaunchXBE`-based chainload mechanism the cycle-21 image-blit witness uses.** Per-set verdicts at HIGH confidence (file-backed by git history + harness/agent source lineage). Cycle-19 hypothesis #1 ("D:\\ remap mismatch / runxbe-SITE-EXEC chainload blocks witness-path file writes") is **fully INVALIDATED** (cycle 21 only had it demoted). Cycle-21 hypothesis #3 (FATX-driver / NT-mount state diverges between FTP-server-time and chainloaded-XBE-time) invalidated for D:\\. Image-blit failure across cycle 19+20+21 conclusively re-classified as **image-blit-specific**, not a launch-path defect. Cycle-22 leading hypothesis: **image-blit crashes BEFORE its main() body's first instruction completes** (CRT init / static-init / DllCharacteristics / pre-main XBE thunking). Right next bounded slice: **Path A.4** (non-fopen kernel-pool controller-buffer witness from cycle-21's proposed follow-up list).

## Exit criteria — final status

1. [x] Worker receipt posted to claude-status.md + current-cycle.md before deeper work (18:41 CDT).
2. [x] Inventory of every artifact under `docs/apple-silicon/xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor,controller-roundtrip}`: 6 PNG files, ZERO README/metadata/provenance docs. Audit relies on git history + source lineage.
3. [x] Git-history trace of each capture file:
       - `pipeline-smoke/real-xbox.png` → `aae0138565` (2026-05-06 15:32 CDT, **pre-19:21 UnleashX-switch**) — XBMC4Gamers/SITE RunXBE.
       - `color-channel,depth-floor,mirror/real-xbox.png` → `823733f2e6` (2026-05-06 23:03 CDT, file mtime 21:48, **post-switch**) — UnleashX/SITE EXEC.
       - `mirror/composite.png + controller-roundtrip/real-xbox-zero.png` → `58bf218838` (2026-05-07 10:39 CDT) — UnleashX/SITE EXEC.
       Dashboard-transition commit: `e74715cd71` (2026-05-06 19:21 CDT, "Composite-capture leg + UnleashX dashboard switch + iND-BiOS findings").
4. [x] Source-lineage diff capture-time → HEAD:
       - `oracle-agent/commands.c::cmd_runxbe`: 2 cosmetic edits only (path-arg parsing rework 2026-05-10; SMC fan-curve cleanup 2026-05-12); `XLaunchXBE(path)` kernel call unchanged.
       - `oracle-orchestrator.py::run_diag`: 49-line diff, all comment-only.
       - `xbe-harness/xbe_renderers.py::run_real_xbox`: diff adds /tmp QMP socket + env timeout knob + agent-launch + pre-run-setup helpers; real-Xbox chainload flow unchanged.
5. [x] Conservative per-set verdict: **all five sets → runxbe path (HIGH confidence)**. Cycle-21 interpretation re-cast: image-blit failure is image-blit-specific. Leading hypothesis after cycle 22: image-blit crashes before its main() body's first instruction (`image_blit_marker(0, "program_entered")`) completes.
6. [x] Canonical docs synced — `handoff.md` (cycle-22 entry on top; cycle-17/19/20/21 entries preserved); `decision-log.md` (cycle-22 entry above cycle-21; no supersession of earlier entries).
7. [x] orchestration-state quartet updated (this file, claude-status.md, validation-status.md, handoff-summary.md).
8. [x] Codex validation decision recorded in `validation-status.md`: **SKIPPED under rule #15's "doc-only changes" carve-out** (zero code changes; aggregate edits are markdown-only). Per-slice justification recorded.
9. [ ] Intended-scope doc/state changes commit pending (next step). Stop after closure summary.

## Out-of-scope (kept bounded per the assignment)

- Did NOT start Path A.4 (oracle-agent kernel-pool buffer witness for image-blit — now top-priority for next bounded slice per A.3's discriminating result).
- Did NOT start Path B (smaller PFIFO-race-only Tier-1 diag XBE).
- Did NOT modify xemu-fork host source.
- Did NOT modify any XBE source (no rebuilds).
- Did NOT touch retail-title metrics, §G.5, RT-as-texture, or any second-wave XBE work.
- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on.
- Did NOT take any real-Xbox runs in this cycle.
- Did NOT generate new reference captures (audit was strictly on existing artifacts).
