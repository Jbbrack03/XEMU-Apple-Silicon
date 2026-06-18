# 2026-06-15 — M0 foundations: XBE board baseline + clear-sync instrument

Session goal (per `metal-parity-roadmap.md` M0): establish regenerable
visibility (coverage-matrix generator + Metal stall-attribution) and capture
current truth, before any feature/perf work. All autonomous; Claude solo.

## 1. Metal XBE board baseline (verified, regenerable)

Run: `xbe_orchestrator.py run --renderer metal` on the Jun-9 binary →
`benchmark-runs/m0-metal-baseline-20260615-142318/summary.json`. Canonical board
regenerated into `xbe-coverage-matrix.md` via the new
`scripts/apple-silicon/xbe-coverage-matrix.py`.

**True Metal status (canonical recipe): 15 PASS, 1 xfail, 1 FAIL, 1 not-built.**
This differs from the prior prose ("15/17 PASS + 2 expected_fail") — the value of
a regenerable board:

- **`stencil-ops` FAIL** (sig 80.2%) — real Metal correctness regression on §G.4
  (catalog_ref also G.2/G.3/H.5). Docs/memory claimed task #14 closed it PASS.
  Now tracked as task #10 (M-I, feature-isolated fix). Pre-existing on the Jun-9
  binary (independent of this session's counter change).
- `logic-ops` xfail (by design — neither GL nor Metal implements NV2A logic ops).
- `swizzle-mipmap` PASS on Metal (its `expected_fail` is GL-only per manifest).
- `texture-shader-stages` PASS; `combiner-basic`, `texture-*`, `blend-matrix`,
  `crtc-publish`, `depth-floor`, `flat-quad-propagation`, `mirror`,
  `native-quad-tri-depth`, `cmp-vertex-format`, `color-channel` PASS.
- `msaa-aa-factor` **not-built** (no ISO/XBE) — excluded from green count.

**Feature-surface coverage: 91 catalog surfaces, 38 covered by ≥1 XBE, 32 green
on Metal, 53 uncovered.** The 53 uncovered (A.1 vertex-shader ops, A.2 FFP, B.2-4
ingestion, C.1/C.3/C.6/C.7, D.9-D.13, E.2/E.6/E.7/E.8/E.10-E.12, F.2/F.5/F.6,
G.5, H.1/H.3/H.4, I.*, etc.) are the Workstream-A / M-I backlog.

**Deferred legs (need user/hardware, task #11):** GL leg (harness uses
`macos-capture.sh` → needs Screen Recording TCC permission; consider an in-renderer
`XEMU_GL_SCREENSHOT_PATH` path to make it autonomous) and real-Xbox leg (console
online). Until then the board's GL/real-Xbox columns are blank and the bar is
Metal-vs-math-oracle, not the full GL == Metal == Oracle triangulation.

## 2. Clear-sync instrument (new) — quantified jitter suspect

Added `METAL_CLEAR_SYNC_US_TOTAL` + `METAL_CLEAR_SYNC_COUNT` (wraps the synchronous
`[cmd waitUntilCompleted]` in `pgraph_mtl_surface_clear`, `surface.mm`). Built
clean (M5 7/7). End-to-end on `color-channel`:

- Aggregate `METAL_CLEAR_SYNC_US_TOTAL = 2,159,737 µs` over `2,007` clears →
  **~1.08 ms CPU stall per synchronous clear**; per-interval totals 80–375 ms.
- The frame thread blocks on a full CPU→GPU→CPU round-trip per clear. The
  in-code comment estimated "sub-ms per frame, ~2-4 clears/frame"; the instrument
  shows ~1 ms *per clear*. This is a measured primary suspect for the Metal p99
  jitter gap (GL ~41 ms vs Metal ~300 ms on PGR2) — Workstream C / M-III (#7).
- Caveat: diagnostic XBEs clear more than retail titles; the **per-clear ~1 ms**
  is the portable datum. Confirm on a tracked retail title (M-III) and test the
  `XEMU_METAL_NO_CLEAR_SYNC=1` opt-out's perf delta vs the fence-only path.

## 3. M0 tooling shipped

- `scripts/apple-silicon/xbe-coverage-matrix.py` — joins manifests (`catalog_ref`)
  + harness `summary.json` + the catalog's `### X.N` headers → regenerable
  green/red board (md + json). Validated: 91/91 surfaces parsed.
- `METAL_CLEAR_SYNC_*` counters (above).
- Doc health: `handoff.md` 13,251→3,834 lines, `decision-log.md` 15,515→89 lines;
  pre-2026-06 history relocated verbatim to `_archive/` (byte-conservation proven
  by heading-multiset diff; all 200 decisions + binding markers preserved).

## Next

M-I begins with task #10 (stencil-ops fix) — first feature-isolated target. The
remaining 53 uncovered surfaces are the saturation backlog. M-III (#7) gets the
clear-sync finding as its lead thread. Do not tune retail titles (rule #17).
