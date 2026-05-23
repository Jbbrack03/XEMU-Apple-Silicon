# Current Cycle

- Cycle: 33 composite-capture fail-fast preflight slice — **CLOSED on `apple-silicon-performance`** pending closure commit. New `scripts/apple-silicon/composite-preflight.sh` + default-on integration into `scripts/apple-silicon/composite-record.sh` so the cycle-32 OUTCOME F8 silent-stall failure mode aborts in ~8 s instead of ~93 s.
- Started: 2026-05-23 (Hermes-supervised bounded session; Claude Code worker autonomous run).
- Closed: 2026-05-23.
- State: **CLOSED.** Implementation slice; ZERO real-Xbox run; cycle-32 redo remains Hermes's call after physical-side composite-cable / capture-input verification.
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: cycle-32 closure commit `ac515383bb` on `apple-silicon-performance`.
- Bounded goal: "Ship a fail-fast composite-capture preflight that converts the cycle-32 OUTCOME F8 silent-stall (~93 s) into a fast actionable abort (~8 s), wire it into `composite-record.sh` by default with an explicit `--skip-preflight` opt-out, sync canonical docs/state, run Codex validation, and commit the bounded slice."
- Result: **Tool slice SHIPPED.** Preflight reproduces cycle-32's hardware-side failure in ~3.6 s vs cycle 32's ~93 s (>25× speedup at the unattended-orchestration boundary). `composite-record.sh` aborts BEFORE arming ffmpeg on no-signal MS2109 and writes a structured `preflight-failed` capture-meta.json instead of recording zero frames. `--skip-preflight` preserves the legacy behavior.

## Plan summary (this session, executed in order)

1. Read canonical docs/state (cycle-32 handoff + decision-log entries, orchestration-state quartet, automation.md composite sections, flags-bench.md, composite-record.sh, xemu-capture-app.py wrapper contract).
2. Inspected git status + recent commits (cycle-32 closure resident; intentional `.hermes_*` files preserved un-staged).
3. Designed minimal robust preflight: xemu-capture snapshot as primary detector (TCC-approved path); ffmpeg `-frames:v 1` fallback against resolved AVFoundation index; JSON output; physical-side checklist on failure.
4. Implemented `scripts/apple-silicon/composite-preflight.sh` (~310 lines).
5. Wired into `scripts/apple-silicon/composite-record.sh` (+~140 lines): default-on with `--skip-preflight` opt-out, `--preflight-timeout` / `--preflight-mode` configurability, env-var defaults via `COMPOSITE_PREFLIGHT_TIMEOUT` / `COMPOSITE_PREFLIGHT_MODE`.
6. Local validation: `bash -n` both scripts; live no-signal probe rc=2 in ~3.6 s; end-to-end aborted with structured `preflight-failed` capture-meta.json; `--skip-preflight` reproduces legacy silent-stall.
7. Doc sync: `automation.md` (new preflight section + extended composite-record.sh section), `.claude/rules/flags-bench.md` (new Composite-capture preflight subsection), `docs/apple-silicon/handoff.md` + `decision-log.md` cycle-33 entries on top.
8. Orchestration-state quartet (this file + claude-status.md + validation-status.md + handoff-summary.md) closure pass.
9. Codex validation per rule #15 — 6 rounds run this closeout session; round 6 = LOOKS GOOD with all adopted findings landed in the script + docs.
10. Closure commit pending.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; 9 pre-existing `.hermes_cycle*.txt` + `.hermes_launch_cycle*.sh` files preserved un-staged.
3. [x] New `composite-preflight.sh` shipped with xemu-capture primary + ffmpeg fallback + JSON output + physical-side checklist.
4. [x] `composite-record.sh` integration: default-on preflight, `--skip-preflight` opt-out, `--preflight-timeout` + `--preflight-mode` configurability, env-var overrides.
5. [x] Local validation: happy-path no-signal abort in ~3.6 s; end-to-end preflight-failure structured marker; `--skip-preflight` escape hatch verified.
6. [x] `automation.md` + `flags-bench.md` synced with the new tool + flags.
7. [x] Handoff + decision-log cycle-33 entries on top; cycle-32 + cycle-31 entries preserved unchanged below.
8. [x] Orchestration-state quartet closure pass (this file + claude-status.md + validation-status.md + handoff-summary.md).
9. [x] Codex validation closed at 6 rounds (round 6 LOOKS GOOD; full round-by-round disposition in handoff.md cycle-33 "Validation" paragraph + decision-log.md cycle-33 "Codex validation" paragraph + validation-status.md "Codex validation marker" section). Marker written at `.claude/state/codex-validate-last-run`.
10. [ ] Closure commit pending.

## Out-of-scope (kept bounded for cycle 33)

- NO real-Xbox cycle-32 redo (Hermes's call after physical-side verification).
- NO host xemu source touched (no `hw/`, `ui/`, `target/`, `include/`).
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact).
- NO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact).
- NO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact).
- NO `xbed_runtime.{c,h}` touched; NO image-blit touched; NO `witness-only/main.c` touched (cycle-31 source intact).
- NO XBE rebuilds; NO `tools/xemu-capture/` source touched.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO PushNotification — bounded tooling slice, not a milestone.
- NO cleanup of intentional `.hermes_*` files at repo root.

## Recommended cycle-34 scope (NOT executed this session — Hermes's call)

The substantive next slice remains the cycle-32 redo, unchanged from cycle 32's closure recommendation:

1. **Hardware-side verification (cannot be done from a Claude session):** composite cable seated at Xbox AV port; MS2109 input selector on composite (not S-Video); Xbox AV output on composite. Optionally `cd tools/xemu-capture && make` and use `xemu-capture snapshot USB2 --out /tmp/probe.png` (the only xemu-capture verb that actually proves live frames are arriving); `inputs` / `set-input` only AFTER snapshot succeeds, to confirm the active input is composite vs S-Video.
2. **Smoke test the cycle-33 preflight** standalone: `scripts/apple-silicon/composite-preflight.sh --device USB2 --timeout 8 --json` should now return `status=ok` after the physical-side problem is fixed.
3. **Optional power-cycle.**
4. **Re-run the canonical cycle-32 sequence per `witness-only/README.md` cycle-31 addendum §"Cycle-32 deployment runbook" steps 1-11 verbatim.** The cycle-31 binary remains deployed at `/E/Apps/witness-only/default.xbe`; cycle-33 preflight will now refuse to arm the long capture if the physical-side problem reappears, returning a structured `preflight-failed` capture-meta.json in ~8 s rather than 90+ s.

On cycle-32 redo, the F-row landings collapse to F1 / F2 / F3 / F4 / F5 / F6 / F7 (F4' eliminated by cycle-32 data). F1 / F2 / F3 / F5 / F6 / F7 → γ INVALIDATED → cycle 35+ re-elevates option (b) for α-vs-β. F4 → γ.0 OR γ.1 → cycle-22 leading hypothesis FULLY CORROBORATED → cycle 35+ ships pre-main breadcrumbs.
