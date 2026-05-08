# 2026-05-07 — retail real-Xbox oracle smoke gate

## Context

We attempted to validate the workflow the Metal backend actually needs from
the hardware oracle: launch a retail game, replay the recorded controller
route into the game, capture composite keyframes, and return to dashboard
without human intervention.

## Result

The run was correctly **blocked** before launching the retail XBE.

Artifact:

- `benchmark-runs/retail-oracle-smoke-20260507T220605Z/report.md`
- `benchmark-runs/retail-oracle-smoke-20260507T220605Z/verdict.json`

Green legs:

- Crimson Skies route CSV parsed: 25,935 events, 100.573 seconds.
- Xbox reachable at `192.168.0.200`; dashboard FTP was up.
- Composite capture device was visible through AVFoundation.
- Installed FTP titles were enumerated under `/F/Games`, including Crimson
  Skies and Rainbow Six 3.

Blocked legs:

- No proven title-facing input backend exists yet. The oracle agent can replay
  into its synthetic state buffer, but retail games do not read that buffer.
- No proven autonomous exit backend exists yet. Launching a retail game would
  tear down the oracle agent; without controller-path injection or a hardware
  reset/IGR route, the Xbox could be stranded in-game.

## Tooling added

- `scripts/apple-silicon/retail-oracle-smoke.py` now enforces this as a
  production gate and refuses unsafe retail launches.
- `scripts/apple-silicon/xbox-kernel-export-annotate.py` joins the live
  ordinal-only kernel dump with nxdk's `xboxkrnl.exe.def`; current artifact
  names 366/366 exports.
- `metal-tools-readiness.sh` and `metal-feedback-dashboard.py` now surface the
  retail-game oracle gap explicitly.

## Decision

Do not use real-Xbox retail-game footage as an autonomous gameplay oracle until
`retail-oracle-smoke.py` returns `verdict=ok` with evidence for both the input
backend and the dashboard-return backend.
