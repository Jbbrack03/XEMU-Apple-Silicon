# Validation Status

- Active slice: cycle 20 Path A — instrument `image-blit/main.c` with FTP-collectable progress markers; re-run real-Xbox witness.
- Validation state: **CLOSED — instrumentation slice shipped, local validation green, real-Xbox witness re-confirmed BLOCKED, Codex validation passed (MINOR ISSUES adopted in full).**

## Gate status

- [x] Fresh worker receipt posted before deeper work (17:04 CDT).
- [x] Bounded reviewable diff landed (~101 net lines, main.c only).
- [x] Rebuild successful (post-Codex shortened labels in current binary).
- [x] Local xemu validation: 13 markers fire via `xemu-guest-log:` channel, every `fopen("D:\\…","wb")` returns NULL on ISO mount, v0.4 pass=3/8 mask=0x31 tally unchanged.
- [x] Real-Xbox runs: two attempts, both produced zero marker files; chainload→FTP-back stable at 22.4 s (matches cycle 19's 22.4 s + 22.3 s for a third reproducibility confirmation overall).
- [x] Conservative interpretation recorded in `handoff.md` + `decision-log.md` cycle-20 entries — promotes cycle-19 hypothesis #1 to leading hypothesis WITHOUT claiming universality or claiming specific XBE crash stage.
- [x] Codex validation: mode `changes`, MINOR ISSUES, finding adopted in full (FATX 42-char basename overflow on two labels), binary rebuilt, real-Xbox re-validated with corrected build.
- [x] Validation marker written at `.claude/state/codex-validate-last-run`.

## Witness outcome summary (cycle 20 Path A)

| Run | Run dir | Chainload→FTP-back | Files retrieved from `/E/Apps/image-blit/` |
|---|---|---:|---|
| local xemu Metal (no host-log) | `cycle20-image-blit-markers-local-metal-20260522T220904Z/` | n/a | (host-side screenshots only; same v0.4 expected_fail verdict) |
| local xemu Metal (XEMU_GUEST_LOG=1) | `cycle20-image-blit-markers-local-metal-guestlog-20260522T221035Z/` | n/a | all 13 markers in host log; every fopen returned NULL |
| local xemu Metal (post-Codex labels) | `cycle20-image-blit-markers-local-metal-postcodex-20260522T221959Z/` | n/a | same outcome; tally unchanged |
| real Xbox (pre-Codex labels) | `cycle20-real-xbox-image-blit-markers-20260522T221224Z/` | 22.4 s | `default.xbe` (upload echo) only — 0 markers |
| real Xbox (post-Codex labels) | `cycle20-real-xbox-image-blit-markers-postcodex-20260522T222048Z/` | 22.4 s | `default.xbe` (upload echo) only — 0 markers |

Both real-Xbox attempts: `verdict.json status: ok` for the chainload-and-collect cycle, no `D:\image-blit-capture.bin`, no `D:\image-blit-done.txt`, and no `D:\image-blit-marker-NN-*.txt`. Pixel oracle correctly classifies cells as `fail: no-xoss-blob-pulled`.

## What stands from cycle 17 + cycle 19

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle-19 hypothesis #1 (D:\ remap under `runxbe`) — **promoted to leading hypothesis by cycle-20 evidence.**

## What changed (cycle 20)

- Instrumentation slice landed in `image-blit/main.c` (bounded, XBE-only).
- New cheap-to-run knowledge: D:\ write-back is blocked on the `runxbe` SITE-EXEC chainload path for image-blit, irrespective of XBE stage or basename length.
- The path from cycle 17's local MET to a default-on flip now explicitly requires either (Path A.2) routing the witness to a non-D:\ partition, (Path A.3) cross-checking existing `xbox-real-references/` provenance, or (Path B) building a smaller PFIFO-race-only XBE that captures via PCRTC. Choice belongs to the next Hermes pass.
