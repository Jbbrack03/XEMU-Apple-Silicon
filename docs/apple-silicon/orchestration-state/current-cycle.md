# Current Cycle

- Cycle: 34 cycle-32 redo on real Xbox vs cycle-31 visual-breadcrumb build — **CLOSED on `apple-silicon-performance`** (closure commit pending at session end). OUTCOME **F4** = zero stripes visible across 22 NTSC-correct composite snapshots + `witness.scan = D-cycle-27` + `witness.scan-self = count=0`. Per cycle-31 cycle-32 9-row discriminator table this is γ.0 OR γ.1 → **cycle-22 pre-main-crash hypothesis FULLY CORROBORATED in its strongest form**. The cycle-32 redo finally moved from F8 (no-capture-procedural-failure recorded at cycle-32 closure `ac515383bb`) to F4 (genuine discriminator answer) now that Josh's physical Xbox restart + QuickTime composite-capture verification cleared the hardware-side blocker. Run-only / doc-only slice; ZERO source/script edits; Codex SKIPPED under rule #15 doc-only / run-only carve-out (same path as cycles 26 / 28 / 30 / 32).
- Started: 2026-05-23 (Hermes-supervised bounded session; Claude Code worker autonomous run launched 20:36:25Z UTC).
- Closed: 2026-05-23 (run-evidence captured, classification reasoning written, canonical docs/state synced, commit pending at exit).
- State: **CLOSED.** Real-Xbox discriminator slice; the substantive cycle-32 redo has finally produced its discriminator answer.
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: cycle-33 closeout-sync commit `6385e2f326` on `apple-silicon-performance`.
- Bounded goal: "Execute the substantive cycle-32 redo now that composite capture is confirmed alive. Follow the canonical deployment/runbook path and classify the outcome using the cycle-31/32 discriminator table."
- Result: **F4 discriminator answer landed.** γ.0 OR γ.1; cycle-22 pre-main-crash hypothesis FULLY CORROBORATED in strongest form. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-35+ pre-main breadcrumb implementation); cycle-35+ scope is Hermes's call.

## Plan summary (this session, executed in order)

1. Read canonical docs/state (cycle-32 + cycle-33 handoff + decision-log entries; orchestration-state quartet; automation.md composite sections; witness-only/README.md cycle-31 addendum + cycle-32 deployment runbook; composite-record.sh; composite-preflight.sh; oracle-and-xbe.md rule).
2. Inspected git status + recent commits (HEAD = cycle-33 closeout-sync `6385e2f326`; cycle-33 closure `dcaf7a0206` resident).
3. Pre-flight composite capture probe — `composite-preflight.sh --device USB2 --timeout 8 --json --mode auto` → `status=ok` in 1.671 s via xemu-capture detector with real 720x480 dashboard frame (652 unique colors). Confirms Josh's Mac Studio QuickTime verification.
4. Xbox reachability check — `ping=true, ftp=true, agent=false` (dashboard, post Josh's physical restart).
5. FTP-list confirms cycle-31 `witness-only/bin/default.xbe` (155 648 B) + cycle-29 `oracle-agent/bin/default.xbe` (417 792 B) still resident at expected paths (preserved across the power-cycle because both live on `/E/Apps/...` HDD).
6. ensure-agent launches the cycle-29 agent from dashboard (SITE EXEC OK; agent ready at 9001).
7. Baseline both scans — `witness.scan = count=1 phys=0x03eb3000 reserved0=0 reserved1=0 mapped_pages_seen=419` AND `witness.scan-self = count=0`. Hard preconditions MET; matches cycle 30 / cycle 32 baselines exactly.
8. Compose substitution discovery: `composite-record.sh` (ffmpeg-AVFoundation-based) cannot run in this bash session because ffmpeg is unavailable on inherited PATH; even with explicit `FFMPEG=/opt/homebrew/bin/ffmpeg` the direct ffmpeg AVFoundation probe silent-stalls (ZERO bytes of stderr across full timeout — reproducing cycle-32 OUTCOME F8 shape locally) while xemu-capture against the same device succeeds in ~2 s — hypothesized TCC camera-access permission inheritance asymmetry between the two binaries.
9. Substituted the ffmpeg leg with an xemu-capture snapshot burst. ZERO source changes — only different invocations of already-shipped tools.
10. First `runxbe witness-only` issued; first burst into `snapshots/` accidentally used xemu-capture's 720x576 PAL default dimensions (because bare `snapshot DEVICE --out PATH` omits `--width`/`--height`) — all 19 PNGs returned RGB(0,0,0) with identical SHAs (Claude-side procedural error; NOT XBE evidence; preserved for audit).
11. Verified Xbox returned to dashboard cleanly after first runxbe; post-first-runxbe scans = `(D-cycle-27, count=0)` — already enough to land F4 if no-stripe finding holds after the format mismatch is corrected.
12. Second `runxbe witness-only` issued with `--width 720 --height 480` explicit in every snapshot invocation. Pre-runxbe snap_00 shows REAL dashboard signal (25 357 unique colors, max=(255,255,255)) — capture pipeline confirmed healthy. 22-frame burst at ~1.2 s cadence over t+0.07s..t+24.17s post-runxbe: EVERY frame RGB(0,0,0) pure black with unique=1.
13. Post-burst-15s status check + ensure-agent + final scans = `witness.scan = D-cycle-27` AND `witness.scan-self = count=0` (matches cycle-32 post-state exactly).
14. Classification reasoning written in `benchmark-runs/cycle34-cycle32-redo-real-xbox-witness-only-visual-20260523T203702Z/SUMMARY.md` against the cycle-31 cycle-32 9-row discriminator table: only F4 matches all three axes simultaneously (none-visible + count=0 + D-cycle-27).
15. Canonical docs sync — handoff.md cycle-34 entry on top with cycle-33 + cycle-32 preserved unchanged below; decision-log.md cycle-34 entry above cycle-33; orchestration-state quartet closure pass (this file + claude-status.md + validation-status.md + handoff-summary.md).
16. Closure commit pending at session end.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; pre-existing untracked `.hermes_*` + `composite_preflight.py` files preserved un-staged; pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` + 3 `retail-*.py` scripts preserved unstaged per cycle-34 prompt guardrail.
3. [x] Xbox reachability + composite preflight + ftp-list preconditions verified.
4. [x] Baseline both scans MET (witness.scan D-cycle-27 + witness.scan-self count=0).
5. [x] Canonical cycle-31 cycle-32 deployment runbook executed with one bounded substitution (xemu-capture burst in place of ffmpeg AVFoundation recording — ZERO source changes).
6. [x] Second runxbe executed with NTSC-correct snapshot dimensions; 22-frame burst captured; pre-runxbe dashboard frame proves capture pipeline healthy.
7. [x] Post-runxbe scans confirm `(D-cycle-27, count=0)` two-tuple identical to cycle 30 / cycle 32 post-states.
8. [x] Outcome classified as **F4** (γ.0 OR γ.1; cycle-22 pre-main-crash hypothesis FULLY CORROBORATED in strongest form) with full row-by-row reasoning in `benchmark-runs/.../SUMMARY.md`.
9. [x] Handoff + decision-log cycle-34 entries on top; cycle-33 + cycle-32 entries preserved unchanged below.
10. [x] Orchestration-state quartet closure pass (this file + claude-status.md + validation-status.md + handoff-summary.md).
11. [x] Rule #15 disposition: doc-only / run-only carve-out applies (same path as cycles 26 / 28 / 30 / 32) — Codex SKIPPED. Cycle-31 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed cycle-31 binary.
12. [ ] Closure commit on `apple-silicon-performance` (pending at session end).

## Out-of-scope (kept bounded for cycle 34)

- NO host xemu source touched (no `hw/`, `ui/`, `target/`, `include/`).
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact).
- NO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact).
- NO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact).
- NO `xbed_runtime.{c,h}` touched; NO image-blit touched; NO `witness-only/main.c` touched (cycle-31 source intact).
- NO XBE rebuilds; NO `tools/xemu-capture/` source touched.
- NO `scripts/apple-silicon/composite-record.sh` source touched (cycle-33 implementation intact).
- NO `scripts/apple-silicon/composite-preflight.sh` source touched (cycle-33 implementation intact).
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO PushNotification — bounded run-only / doc-only slice, not a milestone.
- NO cleanup of pre-existing `.hermes_*` files at repo root.
- NO touch of pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` / `retail-gameplay-oracle.py` / `retail-oracle-workflow.py` / `retail-title-automation-proof.py` (preserved per cycle-34 prompt guardrail).
- NO touch of pre-existing untracked `scripts/apple-silicon/composite_preflight.py` (preserved per the same guardrail).
- NO widening into the cycle-33-preflight wrapper-gating slice (the xemu-capture-yes / ffmpeg-no asymmetry and the xemu-capture PAL-default dimensions are filed as cycle-35+ secondary findings, NOT cycle-34 fixes).

## Recommended cycle-35+ scope (NOT executed this session — Hermes's call)

Per cycle-31 cycle-32 discriminator table F4 "Next" column: pre-main breadcrumbs. Candidates:
1. nxdk `.CRT$XCU` static-init slot stamp that runs after PE-load but before `main()`.
2. Custom XBE-header callback (kernel-controlled entry slot, runs before `.CRT$*`).
3. Thinner alternative to `XVideoSetMode` (e.g. direct NV2A CRTC register writes that bypass the kernel display init path entirely).

Secondary findings worth queuing alongside:
- Extend cycle-33 preflight `--mode auto` so the ffmpeg leg is gated EVEN when xemu-capture reports ok (closes the TCC-asymmetry blind spot exposed this cycle).
- Either default xemu-capture snapshot dimensions to NTSC for MS2109 source, OR update the `witness-only/README.md` example invocation at line 180 to include `--width 720 --height 480` (closes the PAL-default-on-NTSC-signal silent-zero failure mode exposed this cycle).
