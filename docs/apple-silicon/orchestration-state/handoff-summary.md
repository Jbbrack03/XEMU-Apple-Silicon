# Handoff Summary

- Cycle: 2026-05-22, cycle 10 (§E.13 per-format pitch + image-rect
  alignment XBE — first second-wave Gate 2 slice).
- Owner: Claude Code.
- Outcome: **Exit Option A — clean close.**

## What shipped

### `xbe-tests/texture-pitch-alignment/` v0.2

Tier-1 diagnostic XBE covering §E.13 in
`nv2a-feature-surface-research.md`. 4x2 grid, 8 cells, single
LU_IMAGE_A8R8G8B8 format. Sweeps
`(IMAGE_RECT.width, IMAGE_RECT.height, TEXCTL1.IMAGE_PITCH)` across
8 combinations. v0.2 adds `EXTRA_PAD_ROWS = 8` sentinel rows
beneath each active rectangle so `IMAGE_RECT.height` has a grounded
oracle (v0.1 allocated exactly `pitch * height` — height-wrong
sampling would have hit uninitialised memory; v0.2 fixes per Codex
finding).

### Verdict on Metal

PASS byte-correct vs math-derived oracle.
- `changed_pixels_pct = 0.9919` (≪ 3.0 gate)
- `signal_match_pct = 100.0000` (≥ 97.0 gate)
- captured frame: `texture-pitch-alignment.0124.png`
- evidence: `benchmark-runs/20260522T055517Z-texture-pitch-alignment-metal-v0.2-PASS/`

### M15 default-on Gate 2

| § | XBE | status |
|---|---|---|
| E.13 | per-format pitch + image-rect alignment | **MET** (this cycle) |
| H.6 | IMAGE_BLIT | unstarted |
| G.5 | Z compression boundary | unstarted |
| — | RT-as-texture sampling (PGR2 late-stage-0) | unstarted |

Gate 2 reads **1 of 4 MET**. M15 default-on overall remains
**NOT MET** pending the remaining three slices plus Gate 3 retail
re-verification.

## Why the next slice is §H.6

§H.6 IMAGE_BLIT XBE (Tier 2 — guest VRAM oracle per
`diagnostic-xbe-plan.md` §5) is the next-highest-value bounded
slice. The NV2A IMAGE_BLIT path is implicated by the
Crimson / Halo / Rainbow retail traces and currently has zero
isolated coverage. Tier-2 evidence (guest VRAM readback via the
oracle agent) is the documented approach.

## Validation / review state

- Codex validation on v0.1: **MAJOR ISSUES** (height oracle gap +
  cell-4 mislabel + claude-status drift).
- Codex re-review on v0.2: **MINOR ISSUES** — doc-drift only, no
  code findings. All minor items addressed before commit. Codex
  marker for the v0.2 fingerprint written per rule #15.
- Doc sync: COMPLETE (handoff.md, decision-log.md,
  orchestration-state, oracle-and-xbe rule).
