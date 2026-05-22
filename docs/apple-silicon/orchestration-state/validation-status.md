# Validation Status

- Current slice: cycle 10 §E.13 per-format pitch + image-rect
  alignment XBE (texture-pitch-alignment v0.2).
- Validation state: **Slice complete; Metal PASS byte-correct.**
- Repo baseline: working tree on commit `75b9f70413` (cycle 9
  closure) + new diag XBE directory + doc-sync diffs.
- Slice scope: ship the first second-wave Gate 2 XBE; reuse
  `xbed_texture` + `texture-format-sweep` 4x2-grid pattern; do
  not touch shared lib or renderer.

## Gate status (M15 default-on)

- **Gate 1 — first-wave XBE saturation:** **MET.** 17 PASS on
  Metal + 1 expected_fail SPEC (unchanged this cycle).
- **Gate 2 — second-wave retail-implicated feature coverage:**
  **1 of 4 MET** (E.13 met this cycle; H.6, G.5, RT-as-texture
  still unstarted).
- **Gate 3 — retail-title canary re-verification after XBE-library
  green:** **NOT MET.** Blocked on Gate 2 (3 slices remain).
- **Gate 4 — no correctness bug ≥30 days:** **MET.**
- **Codex validation:** v0.1 raised MAJOR ISSUES (height oracle
  gap + cell-4 mislabel); v0.2 addressed both, PASS retained.
- **Doc sync:** **COMPLETE** in this cycle.

## Compare statistics (Metal, v0.2)

- `changed_pixels_pct = 0.9919` (≪ 3.0 gate)
- `signal_match_pct = 100.0000` (≥ 97.0 gate)
- captured frame: `texture-pitch-alignment.0124.png`
- evidence:
  `benchmark-runs/20260522T055517Z-texture-pitch-alignment-metal-v0.2-PASS/`

## Known open items

- M15 default-on remains gated on the remaining 3 of 4 Gate 2
  slices plus Gate 3 retail re-verification.
- task #17 (GL LOD-clamp regression) remains outside this gate.
- Pre-existing real-Xbox reference promotion for the texture-cluster
  XBEs is deferred until the next hardware bring-up window.

## Cycle exit verdict

**Option A — clean close.** First second-wave Gate 2 slice
shipped, validated byte-correct on Metal, Codex findings addressed,
canonical docs synced, durable evidence preserved.
