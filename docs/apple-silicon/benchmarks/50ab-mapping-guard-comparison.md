# 50AB Three-State Comparison Analysis

## Exact Host/Repo Identity
- Host: JoshsMacStudio.localdomain (jbbrack03@192.168.0.3)
- Repo: /Users/jbbrack03/XEMU_MacOS/xemu-fork
- Branch: review-packet-a
- HEAD: 42ca560f8b (50Z: add mapping-compatibility guard)
- Note: surface.mm has uncommitted 50V diagnostic changes (scale/viewport logging)

## Exact Commands Used

### State 1: Baseline (no sibling sync)
```bash
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
export XEMU_RENDERER=METAL
export XEMU_METAL_SCREENSHOT_SOURCE=depth
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 XEMU_BENCH_LOADVM_AT=2 \
./scripts/apple-silicon/run-benchmark.sh \
  --metal-screenshot benchmark-runs/50AB-state1-baseline-pgr2/screenshots/depth-only.png \
  --metal-no-hud pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```
Run dir: 20260531-122623-pgr2

### State 2: Depth sync, no guard
```bash
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
export XEMU_RENDERER=METAL
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 XEMU_BENCH_LOADVM_AT=2 \
./scripts/apple-silicon/run-benchmark.sh \
  --metal-sibling-sync-depth-only \
  --metal-screenshot benchmark-runs/50AB-state2-sync-no-guard-pgr2/screenshots/depth-only.png \
  --metal-no-hud pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```
Run dir: 20260531-122233-pgr2

### State 3: Depth sync + mapping guard
```bash
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
export XEMU_RENDERER=METAL
export XEMU_METAL_SIBLING_SYNC_DEPTH_MAPPING_GUARD=1
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 XEMU_BENCH_LOADVM_AT=2 \
./scripts/apple-silicon/run-benchmark.sh \
  --metal-sibling-sync-depth-only \
  --metal-screenshot benchmark-runs/50AB-state3-sync-with-guard-pgr2/screenshots/depth-only.png \
  --metal-no-hud pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```
Run dir: 20260531-122345-pgr2

## Artifact/Log Paths

| State | Run Dir | Screenshot | Log |
|-------|---------|------------|-----|
| 1 (baseline) | 20260531-122623-pgr2 | 50AB-state1-baseline-pgr2/screenshots/depth-only.png | 20260531-122623-pgr2/xemu.log |
| 2 (sync, no guard) | 20260531-122233-pgr2 | 50AB-state2-sync-no-guard-pgr2/screenshots/depth-only.png | 20260531-122233-pgr2/xemu.log |
| 3 (sync + guard) | 20260531-122345-pgr2 | 50AB-state3-sync-with-guard-pgr2/screenshots/depth-only.png | 20260531-122345-pgr2/xemu.log |

## Sibling Sync Statistics (cumulative across all intervals)

| State | SYNCS | SKIPS | Log Entries | Guard Skips | Iso Breach=Yes | Iso Breach=No |
|-------|-------|-------|-------------|-------------|----------------|---------------|
| 1 (baseline) | 0 | 0 | 0 | 0 | 0 | 0 |
| 2 (sync, no guard) | 236 | 446,161 | 472 | 0 | 118 | 118 |
| 3 (sync + guard) | 0 | 458,385 | 488 | 244 | 0 | 0 |

Key: State 3 converted all 236 syncs into skips (244 guard skip log entries, slight difference due to 50V diagnostic logging firing before the guard check).

## Depth Buffer Analysis

| State | Dimensions | Black (0) | White (255) | Mean | Std |
|-------|-----------|-----------|-------------|------|-----|
| 1 (baseline) | 1280x960 | 75.0% | 25.0% | 63.8 | 110.4 |
| 2 (sync, no guard) | 2048x1024 | 0.0% | 100.0% | 255.0 | 0.0 |
| 3 (sync + guard) | 2560x960 | 0.0% | 100.0% | 255.0 | 0.0 |

## Judgment

**B) Guard does NOT materially change the visible regression family.**

Both State 2 (sync without guard) and State 3 (sync with guard) produce an identical fully-white depth buffer (100% white, mean=255, std=0). The mapping guard successfully converts the mapping-incompatible depth merges at 0x038e0000 into explicit skips (244 guard skip entries), but the visual artifact persists unchanged.

The baseline (State 1) shows a binary depth buffer (75% black, 25% white) which is the expected depth buffer without sibling sync interference. States 2 and 3 both show a fully-white depth buffer, indicating that the depth buffer is being filled with maximum depth values regardless of whether the mapping-incompatible merges are executed or skipped.

## Next Hypothesis

Since suppressing the mapping-incompatible merges does not change the visible regression, the 0x038e0000 merges are NOT the active bottleneck. The fully-white depth buffer suggests a different root cause:

1. **Depth buffer clear/initialization issue:** The depth buffer may be cleared to maximum depth (far plane = 255 in the grayscale export) and never properly written with actual depth values.

2. **Depth test always passing:** If all depth values are at the far plane, all depth tests pass, resulting in a fully-white buffer.

3. **The sibling sync itself may be overwriting depth values:** Even the syncs that pass the isolation predicate and overlap check (118 entries) may be writing incorrect depth values.

4. **The depth screenshot source may not capture the actual rendered depth:** The depth buffer export path might be capturing a different buffer than the one used for rendering.

**Recommended next step:** Investigate whether the depth buffer export path is capturing the correct buffer. Consider:
- Adding debug output to verify depth buffer contents at various pipeline stages
- Checking if the depth screenshot source captures the pre-compositing or post-compositing buffer
- Verifying the depth clear value and depth write enable state
- Comparing the depth buffer with the nv2a (color) buffer to see if the color rendering is also affected
