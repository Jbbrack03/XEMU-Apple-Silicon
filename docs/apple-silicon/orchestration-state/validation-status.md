# Validation Status

- Active slice: cycle 32 Path A.4 real-Xbox deployment of cycle-31 visual-breadcrumb build vs cycle-31 witness-only XBE — run-only / doc-only slice; ZERO source/script edits; ZERO XBE rebuilds.
- Validation state: **CLOSED. Codex SKIPPED under rule #15 doc-only / run-only carve-out** (same path as cycles 26 + 28 + 30).

## Rule #15 applicability (cycle 32)

Rule #15 mandates Codex validation for non-trivial uncommitted source diffs (renderer / TCG / NV2A / build / runtime flag plumbing / apple-silicon scripts; aggregate > 30 lines). Cycle 32's diff is:

- ZERO C source edits.
- ZERO script edits.
- ZERO XBE rebuilds.
- Doc updates only: `docs/apple-silicon/handoff.md` (cycle-32 entry prepended), `docs/apple-silicon/decision-log.md` (cycle-32 entry prepended), orchestration-state quartet (4 files closure pass).
- Evidence-file additions under `benchmark-runs/cycle32-real-xbox-witness-only-visual-20260523T135646Z/` (gitignored per project convention; never committed).

This is a textbook **doc-only / run-only carve-out** under rule #15. Same path as cycles 26 (commit `a31e061144`), 28 (commit `c77b509149`), and 30 (commit `dfe1480cba`). **Codex SKIPPED.**

The cycle-31 binary deployed this cycle (`witness-only/bin/default.xbe` 155 648 B; SHA-256 `c00c726c96f2172badbe0dcd20c111ab89eee95960b8ce43d03c472db4e09edb`) is bit-identical to the cycle-31 build that passed 3-round Codex green at cycle-31 closure (commit `41f350c174`). The cycle-31 Codex validation marker at `.claude/state/codex-validate-last-run` is the relevant marker for the binary deployed; cycle 32 does NOT supersede it.

## Gate status (cycle 32) — final

- [x] Required docs read (handoff cycle-31 + cycle-30 entries, decision-log cycle-31 + cycle-30 entries, orchestration-state quartet, orchestration-workflow, witness-only/README.md, witness-only/manifest.json, composite-record.sh, extract-keyframes.py).
- [x] MS2109 visibility confirmed via AVFoundation enumeration before any Xbox-side action.
- [x] Xbox reachability + agent state confirmed before any Xbox-side action.
- [x] Repo/git state confirmed; 5 pre-existing `.hermes_cycle*.txt` + `.hermes_launch_cycle*.sh` files preserved un-staged.
- [x] Baseline + pre-run scan preconditions MET (witness.scan = D-cycle-27; witness.scan-self = count=0).
- [x] Cycle-31 binary deployed (`--overwrite` FTP-upload; post-upload size + mtime verified).
- [x] Composite-capture leg armed (procedurally launched; capture failure diagnosed in-session).
- [x] runxbe issued + dashboard return polled.
- [x] Post-run scans collected (D-cycle-27 + count=0; identical to cycle 30 E2; F4' RULED OUT for the cycle-32 redo candidate space).
- [x] Capture failure root cause diagnosed via standalone 4 s MS2109 probe.
- [x] F8 outcome classified per cycle-31 8-row F1..F8 + F4' discriminator table.
- [x] `SUMMARY.md` written under `benchmark-runs/cycle32-real-xbox-witness-only-visual-20260523T135646Z/`.
- [x] Canonical docs synced (handoff.md, decision-log.md, current-cycle.md, claude-status.md, this file, handoff-summary.md).
- [ ] Closure commit pending.

## Codex validation marker

The relevant Codex validation marker for the binary deployed this cycle is the cycle-31 marker at `.claude/state/codex-validate-last-run` (3-round green; round 3 LOOKS GOOD). Cycle 32 does NOT supersede this marker because cycle 32 introduces no new source/script.

## Why this is not a regression of any prior cycle's validation guarantees

- Cycle 23 / 25 / 27 / 29 / 31 each Codex-validated their own implementation slices. Cycle 32 does not touch any of those slices' code: `lib/xbed_a4_witness.{c,h}` intact, `lib/xbed_self_witness.{c,h}` intact, `oracle-agent/*` intact, `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit intact, `witness-only/main.c` intact (the cycle-31 source is what was built into the deployed binary).
- Cycle 32 is a run-only / doc-only slice; prior cycles' guarantees remain valid.

## Evidence integrity

- `benchmark-runs/cycle32-real-xbox-witness-only-visual-20260523T135646Z/{00..12-*.log, SUMMARY.md, composite-cycle32/{capture-meta.json, capture-stderr.log}}` preserved on disk (gitignored).
- `composite-cycle32/video.mp4` was never produced — `composite-cycle32/capture-meta.json` `status=ffmpeg-failed`, `ffmpeg_rc=137`, `capture_timed_out=true`; `composite-cycle32/capture-stderr.log` is 0 bytes (ffmpeg never wrote any output to stderr).
- All 14 step logs include UTC timestamps + raw command output for reconstruction.

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing.
- No shared-lib edits; no oracle-agent edits.
- No XBE rebuilds.
- No cycle-32 redo (Hermes's call after physical-side composite-cable / capture-input verification).
- No cycle-33 implementation work.
- No retail-title / §G.5 / RT-as-texture work.
