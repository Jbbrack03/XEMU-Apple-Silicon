# Validation Status

- Active slice: cycle 33 composite-capture fail-fast preflight slice — implementation slice; new `scripts/apple-silicon/composite-preflight.sh` + integration into `scripts/apple-silicon/composite-record.sh` + paired docs/rules; aggregate uncommitted diff ~450 lines across two scripts + three docs + this state quartet.
- Validation state: **Codex validation CLOSED** under rule #15 trigger #2. 6 rounds run this closeout session (rounds 1-5 = MINOR ISSUES, all adopted/deflected; round 6 = LOOKS GOOD); validation marker written at `.claude/state/codex-validate-last-run`; closure commit pending.

## Rule #15 applicability (cycle 33)

Rule #15 mandates Codex validation for non-trivial uncommitted source diffs (renderer / TCG / NV2A / build / runtime flag plumbing / **apple-silicon scripts**; aggregate > 30 lines). Cycle 33's diff includes:

- New `scripts/apple-silicon/composite-preflight.sh` (~310 lines).
- `scripts/apple-silicon/composite-record.sh` (+~140 lines: new flags, env-var defaults, default-on preflight invocation block, stub `capture-meta.json` on failure, OK-path preflight summary embed).
- `docs/apple-silicon/automation.md` (new "Composite capture preflight" section + extended "Composite A/V recording" section; ~70 lines).
- `.claude/rules/flags-bench.md` (new "Composite-capture preflight (cycle 33)" subsection; ~5 lines).
- `docs/apple-silicon/handoff.md` + `docs/apple-silicon/decision-log.md` cycle-33 entries on top + this orchestration-state quartet closure pass.

Aggregate is well above the 30-line rule #15 threshold. **Codex validation is required.** Same rule #15 path as cycles 23 / 25 / 27 / 29 / 31.

The doc-only / run-only carve-out used by cycles 26 / 28 / 30 / 32 does NOT apply here because cycle 33 ships a new script + non-trivial edits to an existing apple-silicon script.

## Gate status (cycle 33)

- [x] Required docs read.
- [x] Repo/git state confirmed; 9 pre-existing `.hermes_cycle*.txt` + `.hermes_launch_cycle*.sh` files preserved un-staged.
- [x] New `composite-preflight.sh` shipped (`bash -n` clean; live no-signal probe rc=2 in ~3.6 s).
- [x] `composite-record.sh` integration shipped (`bash -n` clean; end-to-end preflight-failure structured marker verified; `--skip-preflight` legacy reproduction verified).
- [x] `automation.md` + `flags-bench.md` synced.
- [x] `handoff.md` + `decision-log.md` cycle-33 entries on top; cycle-32 entry preserved unchanged below.
- [x] Orchestration-state quartet closure pass.
- [x] Codex validation (rule #15 trigger #2) closed at 6 rounds (round 6 LOOKS GOOD). Marker written.
- [ ] Closure commit pending.

## Local validation evidence

- `bash -n scripts/apple-silicon/composite-preflight.sh` — clean.
- `bash -n scripts/apple-silicon/composite-record.sh` — clean.
- `scripts/apple-silicon/composite-preflight.sh --device NonExistentDeviceXYZ --timeout 3 --quiet` → rc=3 (`device_not_found`), JSON populated.
- `scripts/apple-silicon/composite-preflight.sh --mode ffmpeg --device USB2 --timeout 3 --quiet` → rc=2 (`no_signal`), elapsed ~3.6 s, `detector="ffmpeg"` in JSON (live MS2109 has no composite signal — reproduces cycle-32 hardware-side failure mode).
- `scripts/apple-silicon/composite-record.sh --device USB2 --duration 2 --preflight-timeout 3 --preflight-mode ffmpeg` → rc=2 in ~3.6 s, `capture-meta.json` carries `schema="composite-record/v1"`, `status="preflight-failed"`, `ffmpeg_invoked=false`, embedded `preflight` summary object; ffmpeg was never launched.
- `scripts/apple-silicon/composite-record.sh --skip-preflight --device USB2 --duration 1 --no-audio` → reproduces cycle-32 silent-stall (rc=137 SIGKILL after ~24 s wall-elapsed; `capture-meta.json` carries `status="ffmpeg-failed"`, `ffmpeg_rc=137`, `capture_timed_out=true`, plus a `preflight: {status: "skipped"}` summary object). Confirms the `--skip-preflight` escape hatch reproduces the legacy behavior unchanged.

## Codex validation marker

Written at `.claude/state/codex-validate-last-run` after round 6 LOOKS GOOD. Validation methodology: 6 rounds of `/codex-validate changes` against the evolving uncommitted diff; each round's findings either ADOPTED (with the fix landed before the next round) or DEFLECTED (with explicit scope rationale recorded in handoff.md + decision-log.md cycle-33 entries). The full round-by-round disposition lives in those two canonical docs; this orchestration-state file points at them rather than duplicating.

## Why this is not a regression of any prior cycle's validation guarantees

- Cycle 23 / 25 / 27 / 29 / 31 each Codex-validated their own implementation slices (XBE source + oracle-agent source). Cycle 33 does not touch any of those slices' code: `lib/xbed_a4_witness.{c,h}` intact, `lib/xbed_self_witness.{c,h}` intact, `oracle-agent/*` intact, `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit intact, `witness-only/main.c` intact (the cycle-31 source is bit-identical to what was committed at cycle-31 closure).
- Cycle 33 is a Mac-side host-only tooling slice + paired docs/rules; prior cycles' guarantees remain valid.
- Cycle-31 Codex marker (`.claude/state/codex-validate-last-run`) remains the relevant marker for the deployed `witness-only/bin/default.xbe` (cycle-33 does not touch any XBE source/build).

## Evidence integrity

- No `benchmark-runs/` directory this cycle (no real-Xbox run; no XBE harness sweep).
- Two modified script files + three modified doc files + the orchestration-state quartet are the entire change surface.

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing inside xemu.
- No shared-lib edits; no oracle-agent edits.
- No XBE rebuilds.
- No `tools/xemu-capture/` source edits.
- No cycle-32 redo (Hermes's call after physical-side composite-cable / capture-input verification).
- No retail-title / §G.5 / RT-as-texture work.
