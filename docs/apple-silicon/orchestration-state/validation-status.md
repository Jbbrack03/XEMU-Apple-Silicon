# Validation Status

- Active slice: cycle 34 cycle-32 redo on real Xbox vs cycle-31 visual-breadcrumb build — run-only / doc-only slice; ZERO source/script edits.
- Validation state: **Rule #15 doc-only / run-only carve-out applies** (same path as cycles 26 / 28 / 30 / 32). Codex SKIPPED. The cycle-31 binary observed this cycle is bit-identical to the cycle-31 build that passed 3-round Codex green at cycle-31 closure (`41f350c174`); the `.claude/state/codex-validate-last-run` marker from cycle-31 closure remains the relevant marker for the deployed artifact.

## Rule #15 applicability (cycle 34)

Rule #15 trigger #2 (non-trivial uncommitted code in `xemu-fork/`; renderer / TCG / NV2A / build / runtime flag plumbing / apple-silicon scripts; aggregate diff > 30 lines) does NOT fire this cycle. Cycle-34 changes are:

- ZERO source/script edits.
- ZERO XBE rebuilds.
- ZERO host source touched.
- ZERO `tools/xemu-capture/` source touched.
- Doc-only edits to canonical state: `docs/apple-silicon/handoff.md` cycle-34 entry on top; `docs/apple-silicon/decision-log.md` cycle-34 entry above cycle-33; orchestration-state quartet (this file + current-cycle.md + claude-status.md + handoff-summary.md) closure pass.
- Run evidence under `benchmark-runs/cycle34-cycle32-redo-real-xbox-witness-only-visual-20260523T203702Z/` (gitignored per project convention; 9 numbered evidence logs + 4 snapshot directories + `SUMMARY.md`).

Same rule-15 disposition as cycles 26 / 28 / 30 / 32 (each of which was a real-Xbox discriminator run that touched no source). The doc-only / run-only carve-out is the explicit rationale.

## Gate status (cycle 34)

- [x] Required docs read.
- [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail.
- [x] Composite preflight `--mode auto` `status=ok` in 1.671 s via xemu-capture; real 720x480 dashboard probe PNG saved.
- [x] FTP-list confirmed cycle-31 witness-only + cycle-29 oracle-agent still resident at expected paths.
- [x] ensure-agent OK; baseline both scans MET — `witness.scan = count=1 phys=0x03eb3000 reserved0=0 reserved1=0` AND `witness.scan-self = count=0`.
- [x] Composite-record substitution executed (xemu-capture snapshot burst in place of ffmpeg AVFoundation recording; ZERO source changes).
- [x] Second `runxbe witness-only` issued with NTSC dimensions; 22-frame burst captured; pre-runxbe dashboard frame proves capture pipeline healthy.
- [x] Post-runxbe scans = `(D-cycle-27, count=0)` identical to cycle 30 / cycle 32 post-states.
- [x] Outcome classified as **F4** with row-by-row reasoning in `benchmark-runs/.../SUMMARY.md`.
- [x] `handoff.md` + `decision-log.md` cycle-34 entries on top.
- [x] Orchestration-state quartet closure pass.
- [x] Rule #15 doc-only / run-only carve-out applies — Codex SKIPPED.
- [ ] Closure commit on `apple-silicon-performance` (pending at session end).

## Local validation evidence

- `composite-preflight.sh --device USB2 --timeout 8 --json --out-dir benchmark-runs/.../preflight` → rc=0 `status=ok` 1.671 s, real 720x480 NTSC probe PNG (652 unique colors).
- `composite-preflight.sh --device USB2 --timeout 8 --mode ffmpeg --json --out-dir benchmark-runs/.../preflight-ffmpeg` → rc=2 `no_signal` 8.93 s (documents the xemu-capture-yes / ffmpeg-no asymmetry — cycle-35+ filed finding).
- `xemu-capture snapshot USB2 --out snap_diag_ntsc.png --width 720 --height 480` → 720x480 PNG with 42 926 unique colors and max=(255,255,255) (confirms NTSC-dimensions path delivers real frames; PAL-default returns all-zero).
- `oracle-orchestrator.py status` → `ping=true, ftp=true, agent=false` (pre-session); after ensure-agent, `agent ready at 192.168.0.200:9001`.
- `oracle-client.py raw witness.scan` baseline → `count=1 phys=0x03eb3000 reserved0=0 reserved1=0 mapped_pages_seen=419`; `oracle-client.py raw witness.scan-self` baseline → `count=0 mapped_pages_seen=419`.
- 22 NTSC-correct snapshots in `snapshots-runxbe2-ntsc/` over t+0.07..t+24.17s post-runxbe → every frame RGB(0,0,0) pure black with unique=1 (analyzed via inline python3 with PIL: 5-band classification + per-pixel min/max).
- Final post-runxbe-and-recovery scans → identical to baseline `(D-cycle-27, count=0)`.

## Codex validation marker

Cycle-31 closure marker at `.claude/state/codex-validate-last-run` (round 3 LOOKS GOOD against the cycle-31 build) remains the relevant marker for the deployed `witness-only/bin/default.xbe`. Cycle-33 closure marker (round 6 LOOKS GOOD against composite-preflight.sh + composite-record.sh) remains the relevant marker for the cycle-33 tooling. Cycle 34 does NOT retrigger rule #15 (no source/script edits) so no new marker is written.

## Why this is not a regression of any prior cycle's validation guarantees

- Cycle 23 / 25 / 27 / 29 / 31 / 33 each Codex-validated their own implementation slices (XBE source + oracle-agent source + apple-silicon scripts). Cycle 34 does not touch any of those slices' code: `lib/xbed_a4_witness.{c,h}` intact, `lib/xbed_self_witness.{c,h}` intact, `oracle-agent/*` intact, `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit intact, `witness-only/main.c` intact (bit-identical to cycle-31 closure), `tools/xemu-capture/` intact, `scripts/apple-silicon/composite-record.sh` + `scripts/apple-silicon/composite-preflight.sh` intact.
- Cycle 34 is a real-Xbox run + doc/state sync slice; prior cycles' guarantees remain valid.
- The deployed cycle-31 `witness-only/bin/default.xbe` (155 648 B) at `/E/Apps/witness-only/default.xbe` and the deployed cycle-29 `oracle-agent/bin/default.xbe` (417 792 B) at `/E/Apps/oracle-agent/default.xbe` are bit-identical to the artifacts that passed Codex green at their respective closures.

## Evidence integrity

- No source-file modifications this cycle.
- Run evidence under `benchmark-runs/cycle34-cycle32-redo-real-xbox-witness-only-visual-20260523T203702Z/` (gitignored). Contents:
  - `00-reachability.log`, `00-composite-preflight.log`
  - `01-ftp-list-binaries.log`
  - `02-ensure-agent.log`
  - `03-baseline-both-scans.log`
  - `04-composite-record.log`, `composite-cycle34/capture-meta.json`
  - `05-runxbe.log`
  - `06-poststate-post-first-runxbe.log`
  - `07-postrun-first-runxbe-scans.log`
  - `08-runxbe2.log`
  - `09-final-scans-after-runxbe2.log`
  - `SUMMARY.md`
  - `preflight/` (xemu-capture preflight; rc=0 status=ok)
  - `preflight-ffmpeg/` (ffmpeg preflight; rc=2 no_signal — documents asymmetry)
  - `composite-cycle34/` (capture-meta.json with preflight-failed status; ffmpeg_invoked=false)
  - `snapshots/` (PAL-default first burst — all-zero; PRESERVED for audit + as evidence of the cycle-33 step-11 example-invocation gap)
  - `snapshots-runxbe2-ntsc/` (NTSC-correct second burst — pre-runxbe dashboard real signal + 22 post-runxbe pure-black)
  - `snap_diag_ntsc.png` (one-shot diag confirming NTSC dimensions deliver real frames)
- Three modified canonical docs + one orchestration-state quartet (4 files) are the entire change surface staged for the closure commit.

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing inside xemu.
- No shared-lib edits; no oracle-agent edits.
- No XBE rebuilds.
- No `tools/xemu-capture/` source edits.
- No `composite-record.sh` / `composite-preflight.sh` source edits.
- No retail-title / §G.5 / RT-as-texture work.
- No cycle-35+ pre-main breadcrumb implementation (Hermes's call).
- No cleanup of pre-existing untracked `.hermes_*` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34 prompt guardrail).
