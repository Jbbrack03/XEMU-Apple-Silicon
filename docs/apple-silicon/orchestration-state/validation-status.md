# Validation Status

- Active slice: cycle 19 — real-Xbox parity check for image-blit.iso under the status-drain diagnostic.
- Validation state: **CLOSED — parity attempt reproducibly produced no real-Xbox witness; default-on decision deferred. All gates below satisfied for the assigned bounded scope.**

## Gate status

- [x] Fresh worker receipt posted after canonical-doc read and reflected in current-cycle.md plus claude-status.md (15:35 CDT).
- [x] Real-Xbox oracle / harness path exercised for the bounded parity check (twice; rule #1 reproducibility confirmation).
- [x] Durable evidence captured: `benchmark-runs/cycle19-real-xbox-parity-image-blit-{20260522T203718Z,retry-20260522T204139Z}/` (each with report.md, summary.json, real-xbox.log, pre/post.png, verdict.json, artifacts/default.xbe).
- [x] Decision impact recorded clearly: `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix shape DEFERRED pending a real-Xbox-witnessable diag XBE (handoff.md + decision-log.md cycle-19 entries).
- [x] Canonical docs + orchestration-state files synced to the actual outcome.
- [x] Codex validation considered — N/A this cycle. Rule #15 trigger (>30-line uncommitted diff on renderer / TCG / NV2A / build / apple-silicon scripts) does not fire on doc-only + evidence-preservation work.

## Witness outcome summary

| Attempt | Run dir | Chainload→FTP-back | Files retrieved from `/E/Apps/image-blit/` |
|---|---|---:|---|
| 1 | `cycle19-real-xbox-parity-image-blit-20260522T203718Z/` | 22.4 s | `default.xbe` (upload echo) only |
| 2 | `cycle19-real-xbox-parity-image-blit-retry-20260522T204139Z/` | 22.3 s | `default.xbe` (upload echo) only |

Both attempts produced `verdict.json status: ok` for the chainload-and-collect cycle itself, but no `D:\image-blit-capture.bin` and no `D:\image-blit-done.txt`. The pixel oracle correctly marked the cell as `fail: no-xoss-blob-pulled`.

## What still stands from cycle 17

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.

## What changed

- The path from cycle 17's local MET to a default-on flip now explicitly requires a new bounded slice that establishes a real-Xbox witness (Path A or Path B in the handoff/decision-log entries).
