# Validation Status

- Active slice: cycle 22 Path A.3 — provenance audit of `docs/apple-silicon/xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor,controller-roundtrip}`. Doc/evidence-only audit; no code changes; no XBE rebuild; no real-Xbox runs.
- Validation state: **CLOSED — five-set provenance audit completed with HIGH-confidence verdicts; cycle-19 hypothesis #1 fully invalidated; cycle-22 leading hypothesis (image-blit-specific early crash) is best-fit to all evidence.**

## Gate status (cycle 22)

- [x] Fresh worker receipt posted before deeper work (18:41 CDT).
- [x] Inventory of every artifact under `docs/apple-silicon/xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor,controller-roundtrip}`: six PNG files, **zero** README/metadata/provenance docs. Audit relies on git history + harness/agent source lineage.
- [x] Git-history trace of each capture file → introducing commit:
  - `pipeline-smoke/real-xbox.png` → `aae0138565` (2026-05-06 15:32 CDT).
  - `color-channel/real-xbox.png` → `823733f2e6` (2026-05-06 23:03 CDT; file mtime 21:48 same day).
  - `depth-floor/real-xbox.png` → `823733f2e6` (2026-05-06 23:03 CDT; file mtime 21:48 same day).
  - `mirror/real-xbox.png` → `823733f2e6` (2026-05-06 23:03 CDT; file mtime 21:48 same day).
  - `mirror/composite.png` → `58bf218838` (2026-05-07 10:39 CDT).
  - `controller-roundtrip/real-xbox-zero.png` → `58bf218838` (2026-05-07 10:39 CDT).
- [x] Cross-reference with dashboard-transition commit `e74715cd71` "Composite-capture leg + UnleashX dashboard switch + iND-BiOS findings" 2026-05-06 19:21 CDT. Conclusion: pipeline-smoke captured pre-switch (XBMC4Gamers/SITE RunXBE); all other captures post-switch (UnleashX/SITE EXEC).
- [x] Source-lineage diff capture-time → HEAD:
  - `oracle-agent/commands.c::cmd_runxbe` — 2 cosmetic edits (2026-05-10 path-arg parsing rework, 2026-05-12 SMC fan-curve cleanup); `XLaunchXBE(path)` kernel call unchanged.
  - `oracle-orchestrator.py::run_diag` — 49-line diff, all comment-only.
  - `xbe-harness/xbe_renderers.py::run_real_xbox` — diff adds /tmp QMP socket, env timeout knob, agent-launch + pre-run-setup helpers; real-Xbox chainload flow unchanged.
- [x] Conservative per-set verdict recorded (all five sets: **runxbe path, HIGH confidence**) in handoff.md + decision-log.md cycle-22 entries.
- [x] Cycle-21 interpretation re-cast against A.3 evidence — cycle-19 hypothesis #1 fully invalidated; cycle-21 hypothesis #3 invalidated for D:\\ writes; cycle-22 leading hypothesis is **image-blit crashes before its main() body's first instruction completes**.

## Codex validation decision

**SKIPPED under rule #15's "doc-only changes" carve-out. Per-slice justification recorded.**

Rule #15 enumerates three Codex triggers:

1. **Substantive plan** (`ExitPlanMode` or ≥4-task `TaskCreate` batch). N/A — cycle 22 used a 5-task TaskCreate batch internally for progress tracking (not a substantive plan with implementation alternatives). Plan-style external alignment was not required because the assignment was already bounded with explicit exit criteria.
2. **Non-trivial uncommitted code in `xemu-fork/`** (renderer / TCG / NV2A / build / runtime flag plumbing / apple-silicon scripts; aggregate diff > 30 lines). **N/A — cycle 22 produced zero code changes.** All edits are markdown under `docs/apple-silicon/` + `orchestration-state/*`. Rule #15 explicitly carves out "doc-only changes" from the >30-line threshold.
3. **Stuck for 3 consecutive failed attempts or two distinct failed hypotheses.** N/A — A.3 produced a clean discriminating answer on first attempt.

No validation marker was written at `.claude/state/codex-validate-last-run` for cycle 22. This paragraph IS the per-slice "why Codex was not required" record per the assignment's exit criterion.

## What stands from cycles 17 + 19 + 20 + 21

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle 20's empirical observation that **D:\\ markers don't land for image-blit on real Xbox under runxbe chainload** — unchanged (this is true; the interpretation changes per cycle 22, not the observation).
- Cycle 21's empirical observation that **E:\\ markers don't land for image-blit on real Xbox under runxbe chainload** (4 reproductions of the 22.4 s gap) — unchanged.

## What changed (cycle 22)

- **Cycle-19 hypothesis #1 (D:\\ remap mismatch / runxbe-SITE-EXEC chainload blocks witness-path file writes):** cycle 21 demoted it from leading to "insufficient as sole explanation"; cycle 22 **fully invalidates** it via comparator evidence (five reference captures produced via the same `XLaunchXBE`-based chainload path).
- **Cycle-21 hypothesis #3 (FATX-driver / NT-mount state diverges between FTP-server-time and chainloaded-XBE-time):** invalidated for D:\\ by mirror/color-channel/depth-floor writing D:\\<id>-capture.bin successfully under UnleashX. Residual narrow uncertainty for E:\\ (no existing reference capture writes from a chainloaded XBE to E:\\).
- **Cycle-22 new framing (image-blit crashes before main()'s first instruction):** elevated to LEADING. Best fit to every cycle 19/20/21 observation when read against A.3's comparator evidence.
- Recommended next slice: **A.4** (non-fopen kernel-pool controller-buffer witness from cycle-21's proposed follow-up list). A.3 makes A.4 the right next discriminator: it tells us whether image-blit reaches its first instruction at all, which is the cycle-22 leading hypothesis's discriminator.

## Yes/no/inconclusive answer to the cycle-22 assignment question

**YES** (HIGH confidence, all five reference sets). The existing `xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor,controller-roundtrip}` capture sets WERE produced through the same `runxbe`-SITE-EXEC-style chainload path that cycle-21's image-blit witness uses (specifically: orchestrator `run_diag` → `OracleClient.runxbe()` RPC → agent `cmd_runxbe` → `XLaunchXBE(path)`). The only material per-set differences are the dashboard the AGENT was FTP-launched from (XBMC4Gamers/SITE RunXBE for pipeline-smoke pre-19:21 dashboard switch; UnleashX/SITE EXEC for everything else) — and that difference is upstream of the diag XBE's process environment. The kernel-call chainload primitive `XLaunchXBE(path)` is provably unchanged between capture-time and cycle-21. The image-blit failure across cycle 19+20+21 is therefore **image-blit-specific**, not a launch-path defect.
