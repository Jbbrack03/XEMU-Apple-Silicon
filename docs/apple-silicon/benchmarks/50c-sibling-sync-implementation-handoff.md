# 50C implementation handoff: runtime sibling-sync merge-scene discrimination

Date: 2026-06-02
Task: t_fa8f7c9f
Verified live repo/worktree before any edits:
- Host: jbbrack03@192.168.0.3
- Repo: /Users/jbbrack03/XEMU_MacOS/xemu-fork
- Branch: review-packet-a
- HEAD at inspection time: 70099834a9cfe6525ec583a19ddb4f68aaca3f44

## What I inspected first

1. Verified the Mac host identity with `/home/jbbrack03/.hermes/scripts/xemu_mac_host_ensure.py` and direct SSH to `jbbrack03@192.168.0.3`.
2. Verified the live worktree identity (`review-packet-a` at `70099834a9cfe6525ec583a19ddb4f68aaca3f44`).
3. Inspected the exact 50B artifact and the current PGR2 merge-scene writeup already present in the live repo:
   - `docs/apple-silicon/benchmarks/50b-sibling-sync-root-cause-narrowing.md`
   - `docs/apple-silicon/benchmarks/2026-05-30-pgr2-sibling-sync-scene.md`
4. Inspected live uncommitted implementation/diagnostic diffs in:
   - `hw/xbox/nv2a/pgraph/mtl/surface.mm`
   - `hw/xbox/nv2a/pgraph/mtl/draw.mm`
   - `scripts/apple-silicon/run-benchmark.sh`

## Exact scene selected

Use Project Gotham Racing 2 on snapshot `pgr2_gameplay_b4`, loaded 2 seconds after resume from:

`benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`

Why this exact scene:
- The live PGR2 writeup documents repeated depth sibling merges on this path at `vram=0x038e0000`.
- The observed merge-producing alternation is the depth target, not the color sibling path, with guest clip sizes alternating between `1280x480` and `1278x442`.
- 50B validation already established that bounded Crimson Skies reruns produced zero sibling merges, so Crimson Skies is not the right discriminator scene for this slice.

## Truthful current evidence posture

- Clip-rect isolation remains the leading hypothesis, but is still not runtime-proven on a merge-producing scene comparison.
- Depth-blit semantics remains unresolved-to-weak; the merge-producing evidence exists, but the isolated comparison has not yet been run in this slice.
- MSAA companion handling remains unresolved and effectively unexercised by the current runtime evidence.
- No claim is made that root cause is resolved, renderer correctness is established, or serious-user-testing readiness exists.

## Minimum bounded implementation choice

I did not widen the renderer code. The smallest truthful 50C implementation is a validator-ready run recipe that exercises the already-known merge-producing PGR2 scene in four tightly bounded configurations:

1. baseline (no sibling sync)
2. color-only sync
3. depth-only sync
4. full color+depth sync

This is the narrowest move because the current blocker is not missing renderer plumbing; it is missing runtime comparison evidence on one exact merge-producing scene.

## Files prepared for the live repo

### 1) `scripts/apple-silicon/run-50c-sibling-sync-comparison.sh`
Why:
- Encodes one reproducible four-run recipe against the exact PGR2 snapshot path.
- Uses a fresh timestamped run root to avoid colliding with older 2026-05-30 run directories.
- Prints repo identity, counters, first merge lines, and a concise interpretation guide for the validator.

### 2) `docs/apple-silicon/benchmarks/50c-sibling-sync-implementation-handoff.md`
Why:
- Captures the verified repo/worktree identity, exact commands, toggles, known expected log signals, and truthful hypothesis framing.
- Gives the validator a single bounded handoff document without overclaiming beyond the evidence.

## Exact validator commands

From `/Users/jbbrack03/XEMU_MacOS/xemu-fork` on the Mac:

```bash
chmod +x scripts/apple-silicon/run-50c-sibling-sync-comparison.sh
scripts/apple-silicon/run-50c-sibling-sync-comparison.sh 30
```

The script itself runs these four cases on the same title/state:

```bash
./scripts/apple-silicon/run-benchmark.sh pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
./scripts/apple-silicon/run-benchmark.sh --metal-sibling-sync pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
./scripts/apple-silicon/run-benchmark.sh --metal-sibling-sync-depth-only --metal-screenshot <run-root>/run3-depth-only/depth-only.png pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
./scripts/apple-silicon/run-benchmark.sh --metal-sibling-sync --metal-sibling-sync-depth --metal-screenshot <run-root>/run4-full-sync/full-sync.png pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```

With shared environment:

```bash
export XEMU_RENDERER=METAL
export XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2
export XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4
export XEMU_BENCH_LOADVM_AT=2
```

## Expected runtime signals

- Run 1 baseline: no sibling sync activity.
- Run 2 color-only: expected zero color merge lines on this snapshot path.
- Run 3 depth-only: expected depth merge lines at `vram=0x038e0000`.
- Run 4 full-sync: expected depth merge lines again; color merge lines may still remain absent on this path.

The validator should compare Run 3 versus Run 4 visuals first, because that is the narrowest discriminator for whether depth sync is causal or merely correlated.

## How the outcomes would update the three hypotheses

### Clip-rect isolation
- Stronger if Run 3 reproduces the same regression locations as Run 4 and the logs still show the `1280x480` / `1278x442` alternation at `0x038e0000`.
- Weaker if Run 3 stays visually clean despite depth merges firing.
- Still unresolved if Run 3 artifacts differ materially from Run 4.

### Depth-blit semantics
- Stronger if Run 3 shows z-test-style failures absent from Run 2.
- Weaker if Run 3 stays visually clean.
- Still unresolved if Run 3 artifacts do not match depth-failure patterns.

### MSAA companion handling
- Stronger only if the observed artifacts correlate with MSAA-specific evidence in the logs.
- Weaker if Run 3 stays clean regardless of MSAA state.
- Otherwise still unresolved.

## What remains unproven for broader readiness

- No root-cause resolution has been established.
- No renderer-wide correctness claim is justified.
- No serious-user-testing readiness claim is justified.
- Only one title/state is covered by this slice.

## Live branch state to preserve truthfully

At inspection time the live repo already had unrelated uncommitted diagnostic work in:
- `hw/xbox/nv2a/pgraph/mtl/surface.mm`
- `hw/xbox/nv2a/pgraph/mtl/draw.mm`
- `scripts/apple-silicon/run-benchmark.sh`

Those diagnostics are not claimed here as new 50C implementation work. This slice only packages the narrow reproduction/handoff needed to exercise the merge-producing scene truthfully.
