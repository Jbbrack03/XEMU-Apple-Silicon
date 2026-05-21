# msaa-aa-factor

Tier-1 NV2A diag XBE that exercises the host renderer's MSAA edge-AA
factor (`§C.4 / §K.4` of `nv2a-feature-surface-research.md` and §4.15
of `diagnostic-xbe-plan.md`).

## What it does

Renders **one solid-WHITE triangle** at vertices `(60, 60)`, `(60, 420)`,
`(580, 240)` over a solid-BLACK background. The two diagonal edges
slope at `180/520 ≈ 0.346` px/px, so every column inside `[60, 580]`
places the edge at a distinct sub-pixel fractional position. This is
the textbook input for a coverage-based MSAA factor: with MSAA disabled
each pixel resolves to a hard step (0 or 255); with MSAA=2/4 the edge
band carries renderer-dependent per-coverage gradient values.

## v0.1 scope (NARROWED — per Codex review 2026-05-21)

This is a **MSAA path-activation + edge-AA-present SMOKE test**, not the
full per-mode gradient-profile validation the §4.15 spec ultimately
calls for. The current gate proves:

1. The Metal renderer honors `XEMU_METAL_MSAA=N` (the resolve counter
   ticks and the sample-count counter is N).
2. The MSAA resolve path doesn't corrupt non-edge tiles (interior +
   exterior signal regions stay byte-exact).
3. The triangle geometry rasterizes to the expected cover region.

It does NOT yet differentiate "msaa=2 silently collapsed to msaa=4"
from "msaa=2 working correctly" — both produce a similar-looking AA
band relative to the hard-step oracle. Per-mode profile differentiation
+ a positive lower-bound edge-band-occupancy assertion are queued for
**v0.2** (second-wave follow-up; tracked in the diagnostic-XBE plan
§4.15 and the decision-log entry for this slice).

## How it's gated (v0.1)

The XBE itself does NOT enable MSAA — the host renderer's
`XEMU_METAL_MSAA` flag (or `XEMU_GL_MSAA` on the GL leg) controls it.
The harness exercises two Metal cells per matrix run:

1. **canonical (msaa=2)** — `XEMU_METAL_MSAA=2` via
   `metal_canonical_overrides`.
2. **msaa4 variant** — `XEMU_METAL_MSAA=4` via
   `additional_metal_recipes`.

(There is intentionally NO msaa=0 cell for Metal here. Every other
first-wave XBE already runs at the default `XEMU_METAL_MSAA=0`, so the
"hard-step works" case is already covered globally. This XBE's job is
to prove the MSAA path engages without regressing the interior /
exterior signal regions.)

Pass criteria per cell:

- **Pixel oracle (math-derived):** captured PNG ≈ hard-step
  rasterization. The harness's `compare_overrides.max_changed_pct = 3.0`
  absorbs the ~0.72%-of-image edge AA band that differs between
  hard-step and any MSAA mode. `min_signal_match_pct = 95.0` protects
  against the whole triangle failing to draw.
- **Counter assertion (`required_counters_min`, two gates):**
  - `METAL_MSAA_RESOLVE_COUNT >= 100` over the run — proves the MSAA
    resolve path runs continuously, not just once. The Metal renderer
    increments this every time it resolves the multisample color
    target for present (`hw/xbox/nv2a/pgraph/mtl/surface.mm`).
  - `METAL_MSAA_SAMPLE_COUNT >= 12` (intervals × sample-count sum) —
    proves the current sample count was ≥ 2 for the bulk of intervals.
    With `XEMU_METAL_MSAA=0` and 12-ish intervals the sum is ~12
    (sample_count=1 each interval); with msaa=2 it's ~24; with msaa=4
    ~48. The threshold `>= 12` is the safe lower bound: it rules out
    a regression where MSAA flag is honored but the actual surface
    creates a single-sample color companion.

  If either counter falls short, the cell fails even if pixels match.

## What it does NOT catch

- Exact per-sample positions. Apple Silicon Metal's standard 4× pattern
  is NOT bit-identical to NV2A hardware's pattern; spec only requires
  monotone coverage. A second-wave `msaa-sample-position` XBE would
  isolate per-sample positions via a point-sprite test pattern.
- Depth-aware MSAA resolve filters (min / max). Deferred follow-up.
- MSAA interaction with surface_scale > 1. The harness runs scale=1
  per the canonical recipe.

## Files

| file           | purpose                                                |
|----------------|--------------------------------------------------------|
| `main.c`       | nxdk entry; renders the single triangle.              |
| `expected.py`  | math-derived oracle (hard-step rasterization).         |
| `manifest.json`| XBE manifest with MSAA recipe + counter gate.          |
| `Makefile`     | nxdk build wiring via `../lib/lib.mk`.                 |

## Build

```sh
cd scripts/apple-silicon/xbe-tests/msaa-aa-factor
make
```

Produces `bin/default.xbe` + `msaa-aa-factor.iso`.

## Run

```sh
scripts/apple-silicon/xbe-harness/xbe_orchestrator.py \
    --xbe msaa-aa-factor --renderer metal
```

Two Metal cells (canonical `msaa=2`, variant `msaa=4`) per matrix run.
Each cell PASSes when (a) the pixel oracle compare passes under the
manifest's tolerance, AND (b) BOTH counter assertions pass
(`METAL_MSAA_RESOLVE_COUNT >= 100` and
`METAL_MSAA_SAMPLE_COUNT >= 12`).

## v0.2 follow-up (queued)

- Per-mode keyed `expected_results` so a 4× → 2× collapse fails.
- Positive lower-bound assertion on AA-band pixel COUNT (rejects a
  pure hard-step output even when the upper compare bound is met).
- Optional edge-perpendicular probe lines with renderer-tolerant
  gradient-shape oracles.
