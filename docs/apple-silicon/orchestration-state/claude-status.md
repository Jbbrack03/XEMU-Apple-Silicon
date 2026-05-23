# Claude Status

- Objective: cycle 33 composite-capture fail-fast preflight slice — ship `scripts/apple-silicon/composite-preflight.sh` + default-on integration into `scripts/apple-silicon/composite-record.sh` so the cycle-32 OUTCOME F8 silent-stall (~93 s) aborts in ~8 s with an actionable physical-side checklist instead.
- Status: **CLOSED.** Tool slice shipped; local validation green; docs/state synced; Codex validation closed at 6 rounds this closeout session (round 6 = LOOKS GOOD; full disposition in handoff.md + decision-log.md cycle-33 entries); validation marker written at `.claude/state/codex-validate-last-run`; closure commit pending.

## Why cycle 33 ran this session

Cycle 32 closure (commit `ac515383bb`) documented OUTCOME F8 = "cycle-32 procedural failure — MS2109 composite-capture leg recorded zero frames" and explicitly deferred cycle 33 implementation work to a later Hermes-scheduled session. Hermes's bounded prompt for this session was: "Ship a bounded cycle-33 tooling improvement that prevents another cycle-32-style silent composite-capture stall." A fresh prior session attempted the same scope but stopped after being launched in don't-ask permission mode and unable to write files; no source edits landed in that attempt, leaving the repo clean except intentional `.hermes_*` files.

## What this session shipped

1. **New `scripts/apple-silicon/composite-preflight.sh`.** ~310 lines (executable bash + inline python3). Detects "MS2109 connected but no live signal" in seconds. Primary detector = xemu-capture snapshot via the TCC-approved app bundle path (`scripts/apple-silicon/bin/xemu-capture snapshot DEVICE --out preflight.png`); fallback = `ffmpeg -f avfoundation -frames:v 1` against the resolved AVFoundation numeric index, killed via a python3 wall-clock deadline if no frame arrives. `--mode auto` (default) tries xemu-capture first then falls back to ffmpeg; `--mode xemu-capture` and `--mode ffmpeg` force a single path. JSON output (schema `composite-preflight/v1`) always written to `preflight-meta.json` capturing status / exit_code / detector / elapsed_s / detail + every input parameter + backend availability flags + probe-image path. Exit codes: 0 ok, 2 no_signal, 3 device_not_found, 4 no_backend, 5 invalid CLI, 1 unexpected. On any non-zero exit the script prints a 5-line physical-side checklist drawn from cycle-32 evidence + deletes any non-empty probe PNG so callers never see a stale frame.
2. **`scripts/apple-silicon/composite-record.sh` integration.** New flags: `--skip-preflight`, `--preflight-timeout SECONDS` (default 8), `--preflight-mode auto|xemu-capture|ffmpeg`. Two env-var overrides: `COMPOSITE_PREFLIGHT_TIMEOUT`, `COMPOSITE_PREFLIGHT_MODE`. Default-on: preflight runs first against the same device parameters with `--out-dir $OUT_DIR/preflight/`; on failure composite-record.sh writes a stub `capture-meta.json` (schema `composite-record/v1`, `status="preflight-failed"`, `ffmpeg_invoked=false`, embedded `preflight` summary object) and exits with the preflight's own rc (identity passthrough: 2 cycle-32 F8 no_signal / 3 device_not_found / 4 no_backend / 1 host-side backend error / 5 invalid CLI — full enumeration adopted from Codex cycle-33-closeout round 2) WITHOUT touching ffmpeg. On preflight success the long capture proceeds unchanged and the post-run capture-meta.json is patched with the same `preflight` summary alongside existing fields.
3. **Paired doc sync.** `docs/apple-silicon/automation.md` (new "Composite capture preflight" section directly above "Composite A/V recording" + three new flag rows + new "Preflight default (cycle 33)" paragraph in the composite-record section). `.claude/rules/flags-bench.md` (new "Composite-capture preflight (cycle 33)" subsection listing env-var defaults + the `--skip-preflight` operator switch). `docs/apple-silicon/handoff.md` + `decision-log.md` cycle-33 entries on top. Orchestration-state quartet closure pass.

## Session progress

- [x] Read required docs/state files.
- [x] Confirmed git state + intentional `.hermes_*` files preserved.
- [x] Inspected composite-record.sh, xemu-capture-app.py wrapper, scripts/apple-silicon/bin/xemu-capture.
- [x] Implemented `composite-preflight.sh` with xemu-capture + ffmpeg detectors + JSON output + checklist.
- [x] Wired default-on preflight into `composite-record.sh` with `--skip-preflight` opt-out + `--preflight-timeout` + `--preflight-mode` controls + env-var defaults.
- [x] Local validation: live no-signal MS2109 probe rc=2 in ~3.6 s; end-to-end preflight-failure structured marker; `--skip-preflight` legacy reproduction.
- [x] `automation.md` + `flags-bench.md` synced.
- [x] `handoff.md` + `decision-log.md` cycle-33 entries on top.
- [x] Orchestration-state quartet closure pass.
- [x] Codex validation closed at 6 rounds (round 6 LOOKS GOOD). Marker written at `.claude/state/codex-validate-last-run`.
- [ ] Closure commit pending.

## Confidence + risk notes

- **HIGH confidence in the no-signal detection path.** The live MS2109 currently sees no composite signal (cycle 32's hardware-side failure mode persists in this session). Both the xemu-capture path (which falls back cleanly when the underlying AVFoundation session sees no frames) and the ffmpeg `-frames:v 1` path correctly time out within `--preflight-timeout` seconds and return rc=2 with `status=no_signal` and a populated checklist. The detection turnaround (~3.6 s in the validation run) reproduces the cycle-32 hardware-side failure faster than the long capture by >25×.
- **MEDIUM confidence in the signal-present (rc=0) path.** This session could not exercise the "signal present" path because the live MS2109 has no composite input — that is the cycle-32 failure mode under investigation. The xemu-capture `snapshot` verb is unchanged from its 2026-05-06 design (referenced in `automation.md` "tools/xemu-capture/" section) and is the same path the retail oracle uses successfully, so a signal-present case should pass cleanly. Cycle-32 redo will exercise rc=0 end-to-end.
- **LOW risk to existing capture path.** `--skip-preflight` reproduces the legacy `composite-record.sh` behavior bit-identically — verified in-session by an end-to-end `--skip-preflight --no-audio --duration 1` run that produced the canonical cycle-32 silent-stall + SIGKILL shape, with the only diff being a `preflight: {status: "skipped", ...}` summary object added to capture-meta.json.
- **LOW risk to retail oracle / xemu-capture TCC grant.** The preflight invokes xemu-capture through the same `scripts/apple-silicon/bin/xemu-capture` wrapper the retail oracle uses; TCC sees the same `com.xemu-macos.capture` bundle identity; no new authorization prompt.
- **LOW risk to project rule #14.** The preflight does NOT use QMP `screendump` or the `qmp` screenshot backend; rule #14 not engaged.

## What this session does NOT do

- NO real-Xbox cycle-32 redo (Hermes's call after physical-side verification).
- NO xemu host source touched.
- NO XBE rebuilds; NO `lib/xbed_*` / `oracle-agent/*` / `witness-only/main.c` / image-blit / `xbed_runtime.{c,h}` touched.
- NO `tools/xemu-capture/` source touched (the binary is already TCC-authorized; cycle 33 only consumes the wrapper output).
- NO flag default flips; NO M15 movement; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` unchanged.
- NO PushNotification — bounded tooling slice, not a milestone.
- NO cleanup of `.hermes_*` files.

## Next proposed action

Closure commit on `apple-silicon-performance`. Codex validation under rule #15 trigger #2 closed at 6 rounds this session (round 6 LOOKS GOOD). After commit, the substantive next slice remains the cycle-32 redo (Hermes's call after physical-side composite-cable / capture-input verification); the cycle-33 preflight makes that redo safer to run unattended.
