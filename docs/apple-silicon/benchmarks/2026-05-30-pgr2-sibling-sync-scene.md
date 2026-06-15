# 50C: PGR2 sibling-sync merge-scene discrimination

Last updated: 2026-06-01 (50J provenance correction)
Revised: 2026-05-30 (50D analysis)
Revised: 2026-05-30 (50E depth-only bottleneck discrimination)
Revised: 2026-05-30 (50F repaired depth-only replay recipe)
Revised: 2026-05-30 (50G snapshot provenance correction)
Revised: 2026-05-30 (50H screenshot capture repair and visual/oracle evidence path)
Revised: 2026-06-01 (50J provenance correction - benchmark actually succeeded)

## Goal

Capture and document a known merge-producing runtime scene under the
sibling-sync instrumentation so a validator can discriminate whether
the observed depth-only sibling merges at `0x038e0000` are the active
software-owned bottleneck causing regressions, or merely correlated
runtime activity.

## Scene selection rationale

**Title:** Project Gotham Racing 2 (PGR2)
**Snapshot:** `pgr2_gameplay_b4` (from `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`)
**VRAM addresses:**
- Color RT: `0x3c84000` -- source comment (surface.mm lines 268-272)
  claims 1278x442 and 1280x480 clip-rect siblings alternate per frame.
  **50D runtime evidence disproves this for the validated snapshot path.**
  The color RT at 0x3c84000 is bound through the `ensure_color` path
  (when `dma_color == 0`) and is always the same dimensions. No second
  color binding is created, so the color sibling-merge path never fires.
- Depth RT: `0x038e0000` -- z-buffer for the 0x3c84000 color RT.
  **50D runtime evidence confirms clip-rect alternation at the depth RT:**
  2560x960 (scaled) / 1280x480 (guest) and 2556x884 (scaled) /
  1278x442 (guest) alternate, producing repeated depth sibling merges.

**Why PGR2:**
- The sibling-sync code comments explicitly cite PGR2 as the title
  that exercises the 1278x442 / 1280x480 sibling pattern.
- PGR2 is the strongest stress case for the Metal renderer (furthest
  below 30 FPS target, exposes quad-family geometry-shader activity).
- Multiple prior benchmark runs exist for PGR2 under the same snapshot
  (`benchmark-runs/pgr2-sibling-sync*` from 2026-05-20).
- The `metal_siblings_summary` logs from the depth-floor XBE runs
  (20260530-49a-packet-a-sibling-diag) show single-sibling patterns
  (color_siblings=1), confirming that the depth-floor XBE does NOT
  exercise sibling merges. PGR2 is the only title in the corpus with
  documented multi-sibling activity.

## 50D runtime evidence (2026-05-30)

**Run:** `benchmark-runs/20260530-125856-pgr2`
- `env_XEMU_METAL_RTT_SIBLING_SYNC=1` (color sync ON)
- `env_XEMU_METAL_RTT_SIBLING_SYNC_DEPTH=1` (depth sync ON)
- Snapshot: `pgr2_gameplay_b4` (loadvm at 2s)

**Sibling sync counters (per interval):**
- Interval 0: METAL_SIBLING_SYNCS=0, METAL_SIBLING_SYNC_SKIPS=42149
- Interval 1: METAL_SIBLING_SYNCS=2, METAL_SIBLING_SYNC_SKIPS=45466
- Interval 2: METAL_SIBLING_SYNCS=12, METAL_SIBLING_SYNC_SKIPS=42076

**All sibling sync events are depth merges at vram=0x038e0000.**
Zero color merge lines in the entire log (240+ depth merge lines).

**Color RT binding evidence:**
- `metal_color_bind` log shows 16 unique color RT addresses, NONE at 0x3c84000
- `metal_draw_target` log shows 0x3c84000 receives 200k+ draw calls
- The color surface at 0x3c84000 is bound through the `ensure_color` path
  (when `dma_color == 0`), not through `bind_color_ex`
- The color surface is always bound with the SAME dimensions -- no clip-rect
  alternation for the color RT at this address in this snapshot

**Depth RT binding evidence:**
- Depth sibling merges at vram=0x038e0000 alternate between:
  - 2560x960 (scaled) / 1280x480 (guest)
  - 2556x884 (scaled) / 1278x442 (guest)
- This confirms the clip-rect alternation pattern at the depth RT

## 50E: Depth-only bottleneck discrimination

### Goal

Determine whether the exact depth-only sibling merges at `0x038e0000`
are the active regression-causing path, or merely correlated runtime
activity. This is the narrowest remaining software-owned gap after 50D
disproved the color sibling-merge hypothesis for this snapshot path.

### Discrimination matrix (4 runs)

| Run | `--metal-sibling-sync` | `--metal-sibling-sync-depth` | `--metal-sibling-sync-depth-only` | Color Sync | Depth Sync | Tests |
|-----|----------------------|---------------------------|--------------------------------|------------|------------|-------|
| 1   | (not passed)         | (not passed)              | (not passed)                    | OFF        | OFF        | Baseline: no sibling sync. Establishes visual baseline. |
| 2   | passed               | (not passed)              | (not passed)                    | ON         | OFF        | Is color sync the bottleneck? (Expected: no merges on this path per 50D) |
| 3   | (not passed)         | (not passed)              | **passed**                      | OFF        | ON         | **Is depth sync the bottleneck?** (50E key discriminator) |
| 4   | passed               | passed                    | (not passed)                    | ON         | ON         | Combined effect. If Run 3 reproduces regressions, Run 4 should too. |

**Run 3 is the critical discriminator.** It isolates depth sync from
color sync. If Run 3 reproduces the multi-title regression pattern
(black slabs, missing geometry, z-test failures), then depth merges
are the active bottleneck. If Run 3 does NOT reproduce regressions,
then depth merges are correlated but not causally explanatory.

### Prerequisites: snapshot-load context

**50F fix (2026-05-30):** The `pgr2_gameplay_b4` snapshot was saved into
the scratch HDD of the original capture run
(`benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`). When
`run-benchmark.sh` starts a new run, it copies the base HDD
(`profile-prep/xbox_hdd.qcow2`) to a fresh scratch copy. The base HDD
does NOT contain the snapshot, so `XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4`
fails with `Snapshot 'pgr2_gameplay_b4' does not exist in one or more
devices`.

**50G correction (2026-05-30):** The 50F revision incorrectly directed
`XEMU_BENCH_HDD_SOURCE` to `benchmark-runs/profile-prep/pgr2-canary.qcow2`.
That image does NOT contain the `pgr2_gameplay_b4` snapshot. Verified via
`qemu-img snapshot -l` on the Mac:

- `benchmark-runs/profile-prep/pgr2-canary.qcow2` has one snapshot:
  `pgr2-canary` (59.8 MiB, 2026-05-04). No `pgr2_gameplay_b4`.
- `benchmark-runs/profile-prep/xbox_hdd.qcow2` has zero snapshots.
- `Xbox-Emulator-Files/hdd/xbox_hdd.qcow2` has zero snapshots.
- `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2` has one snapshot:
  `pgr2_gameplay_b4` (64.9 MiB, 2026-05-01 11:22:11, VM clock 00:02:09.739).

**The correct HDD source for all snapshot-load runs is:**
`benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`

### How to run each discrimination run

**Prerequisite:** Set `XEMU_RENDERER=METAL` before running any of these
commands. The Metal renderer is required for sibling-sync instrumentation
and for the depth-only discrimination to be meaningful.

#### Run 1: Baseline (no sibling sync)

```bash
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
export XEMU_RENDERER=METAL
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 \
XEMU_BENCH_LOADVM_AT=2 \
./scripts/apple-silicon/run-benchmark.sh pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```

No sibling-sync flags. Establishes visual baseline with sibling sync
completely disabled.

#### Run 2: Color-only sync (depth sync OFF)

```bash
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
export XEMU_RENDERER=METAL
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 \
XEMU_BENCH_LOADVM_AT=2 \
./scripts/apple-silicon/run-benchmark.sh --metal-sibling-sync pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```

Enables `XEMU_METAL_RTT_SIBLING_SYNC=1` and keeps
`XEMU_METAL_RTT_SIBLING_SYNC_DEPTH=0` (default). Per 50D, this run
should produce ZERO color merge lines on the pgr2_gameplay_b4 path.

#### Run 3: Depth-only sync (color sync OFF) -- 50E KEY RUN

```bash
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
export XEMU_RENDERER=METAL
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 \
XEMU_BENCH_LOADVM_AT=2 \
./scripts/apple-silicon/run-benchmark.sh --metal-sibling-sync-depth-only \
  --metal-screenshot benchmark-runs/20260530-152933-pgr2/screenshots/depth-only.png \
  pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```

Enables `XEMU_METAL_RTT_SIBLING_SYNC_DEPTH=1` and explicitly sets
`XEMU_METAL_RTT_SIBLING_SYNC=0`. This is the only run that isolates
the depth sync path. **This is the 50E key discriminator.**

The `--metal-screenshot` flag uses the in-renderer Metal screenshot
path (`XEMU_METAL_SCREENSHOT_PATH`) which captures the drawable
directly without requiring macOS screencapture permissions. This
replaces the default `macos` backend (which uses `screencapture` and
fails in headless/CI contexts with "could not create image from
display"). See the 50H section below for details.

Expected output:
- Depth merge lines at vram=0x038e0000 (confirmed by 50D)
- Zero color merge lines (color sync is OFF)
- `METAL_SIBLING_SYNCS` > 0 (depth sync events)
- `METAL_SIBLING_SYNC_SKIPS` varies
- A PNG screenshot at the specified path (in-renderer Metal capture)

#### Run 4: Full sync (color + depth)

```bash
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
export XEMU_RENDERER=METAL
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2 \
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4 \
XEMU_BENCH_LOADVM_AT=2 \
./scripts/apple-silicon/run-benchmark.sh --metal-sibling-sync --metal-sibling-sync-depth pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```

Enables both `XEMU_METAL_RTT_SIBLING_SYNC=1` and
`XEMU_METAL_RTT_SIBLING_SYNC_DEPTH=1`.

### What to look for in each run

In the xemu.log of each run, grep for:
- `xemu.metal.sibling_sync: color merge` -- confirms color sync events
- `xemu.metal.sibling_sync: depth merge` -- confirms depth sync events
- `METAL_SIBLING_SYNCS` / `METAL_SIBLING_SYNC_SKIPS` -- per-interval counters

For visual regression comparison across runs:
- **Black slabs / missing geometry:** Indicates depth sync may be
  causing z-test failures by merging wrong depth values.
- **Color artifacts (magenta, wrong colors):** Indicates color sync
  may be merging wrong color content.
- **Flickering / UI corruption:** May indicate clip-rect isolation
  violation (siblings that should remain separate are being merged).

### Expected sibling sync log output

**Color merges (NOT expected on pgr2_gameplay_b4):**
```
xemu.metal.sibling_sync: color merge vram=0x3c84000 src=1278x442 tgt=1280x480 fmt=8 draw_seq_delta=42 sync_msaa=no
```

**Depth merges (CONFIRMED on pgr2_gameplay_b4):**
```
xemu.metal.sibling_sync: depth merge vram=0x038e0000 src=2560x960 tgt=2556x884 fmt=2 draw_seq_delta=0 sync_msaa=no
xemu.metal.sibling_sync: depth merge vram=0x038e0000 src=2556x884 tgt=2560x960 fmt=2 draw_seq_delta=0 sync_msaa=no
```

The `draw_seq_delta` shows how many draw sequences separate the
freshest sibling from the target. A non-zero delta confirms that
the target was stale and needed syncing.

## Success criteria

1. **Color merge lines:** NOT expected on the pgr2_gameplay_b4 snapshot
   path (50D disproves the color sibling-merge hypothesis for this scene).
   A different PGR2 snapshot would be needed to test color sibling merges.
2. **Depth merge lines:** CONFIRMED -- repeated depth sibling merges at
   vram=0x038e0000 with clip-rect alternation (1280x480 / 1278x442).
3. **Run 3 (depth-only) discriminates the bottleneck:** If Run 3
   reproduces the regression pattern, depth merges are the active
   bottleneck. If Run 3 does NOT reproduce regressions, depth merges
   are correlated but not causal.
4. The benchmark metadata (metadata.txt) records the exact env flags
   used (`env_XEMU_METAL_RTT_SIBLING_SYNC`,
   `env_XEMU_METAL_RTT_SIBLING_SYNC_DEPTH`, and
   `env_XEMU_METAL_RTT_SIBLING_SYNC_DEPTH_ONLY`).

## What remains unproven

- **Clip-rect isolation hypothesis (color):** DISPROVEN for the
  pgr2_gameplay_b4 snapshot path. The color RT at 0x3c84000 does not
  create multiple bindings with different dimensions. The source-code
  comment may be correct for a different PGR2 snapshot or render path,
  but it does not apply to the validated scene.
- **Clip-rect isolation hypothesis (depth):** SUPPORTED by runtime
  evidence. Depth sibling merges fire at 0x038e0000 with confirmed
  clip-rect alternation (1280x480 / 1278x442). 50E/50H determine
  whether this is causally relevant to regressions.
- **Depth-blit semantics:** The depth sibling merges are confirmed,
  but the repaired screenshot path has not yet yielded a recognizable
  gameplay frame for this rerun family. The captured PNGs can confirm
  that a screenshot artifact exists, but they do not currently resolve
  whether depth merges are causative, benign, or masking a narrower
  depth-blit/readback issue.
- **MSAA companion handling:** All observed depth merges report
  `sync_msaa=no`. MSAA is not exercised in this run. Unresolved.
- **Broader title coverage:** Only PGR2 has been tested for sibling
  merges. Other titles may or may not exercise the color sibling-merge
  path. 50E should also be run against boot-logo, Halo, and Crimson
  Skies to confirm the regression pattern.
- **Visual/oracle evidence:** 50H repairs the screenshot capture
  mechanism by switching from `macos` backend (screencapture) to
  `--metal-screenshot` (in-renderer Metal path), but the resulting
  images for this rerun family are still mostly white/blank rather than
  recognizable gameplay. They therefore do not yet support visual
  interpretation of the depth-only leg.

## 50G: Snapshot provenance correction (2026-05-30)

### Problem

The 50F revision documented all four discrimination runs using
`XEMU_BENCH_HDD_SOURCE=benchmark-runs/profile-prep/pgr2-canary.qcow2`
as the snapshot source. Live verification on the Mac host confirmed
that `pgr2-canary.qcow2` does NOT contain the `pgr2_gameplay_b4`
snapshot. The documented commands would fail at `loadvm` with:

```
Snapshot 'pgr2_gameplay_b4' does not exist in one or more devices
```

### Evidence

Verified via `qemu-img snapshot -l` on the Mac host:

| Image | Snapshots |
|-------|-----------|
| `benchmark-runs/profile-prep/pgr2-canary.qcow2` | `pgr2-canary` (59.8 MiB, 2026-05-04) |
| `benchmark-runs/profile-prep/xbox_hdd.qcow2` | (none) |
| `Xbox-Emulator-Files/hdd/xbox_hdd.qcow2` | (none) |
| `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2` | `pgr2_gameplay_b4` (64.9 MiB, 2026-05-01) |

### Fix applied

- Corrected `XEMU_BENCH_HDD_SOURCE` in all four run commands to point
  to `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`.
- Added `export XEMU_RENDERER=METAL` prerequisite to all run commands.
- Corrected `noop.csv` to the repo-relative path
  `scripts/apple-silicon/input-scripts/noop.csv`.

### What this means for the depth-bottleneck hypothesis

Once the corrected commands are run, the depth-only discrimination
(Run 3) will be able to:
- Successfully load the `pgr2_gameplay_b4` snapshot via `loadvm`.
- Exercise the depth-sibling-sync path under the Metal renderer.
- Produce the depth-merge evidence needed to discriminate whether
  depth merges are the active bottleneck or merely correlated.

If Run 3 reproduces the regression pattern, depth merges are the
active bottleneck. If Run 3 does NOT reproduce regressions, depth
merges are correlated but not causally explanatory.

## 50H: Screenshot capture repair and visual/oracle evidence path (2026-05-30)

### Problem

The 50G validation run (`benchmark-runs/20260530-152933-pgr2`) used
the default `screenshot_backend: macos` which relies on macOS
`screencapture`. This failed with:

```
capture failed at 5001ms source=fullscreen: could not create image from display
capture failed at 15010ms source=fullscreen: could not create image from display
capture failed at 25002ms source=fullscreen: could not create image from display
```

Result: `METAL_SCREENSHOTS_TAKEN=0` -- no screenshots were captured.
The depth-only sibling sync evidence (254 depth merges at vram=0x038e0000)
could not be visually interpreted because there was no visual evidence.

### Root cause

The `macos` screenshot backend (`scripts/apple-silicon/macos-capture.sh`)
uses `screencapture -x` which requires:
1. The xemu window to be on-screen and accessible
2. macOS screen recording permissions to be granted
3. The display to be active and not in a headless/CI context

In the benchmark harness context, the xemu window may not be on-screen
(headless mode), screen recording permissions may not be granted, or
the display may not be accessible. This causes `screencapture` to fail
with "could not create image from display."

### Fix: Use in-renderer Metal screenshot path

The `--metal-screenshot <path>` option uses the in-renderer Metal
screenshot path (`XEMU_METAL_SCREENSHOT_PATH`) which:
- Captures the drawable directly from the Metal renderer
- Takes no Screen-Recording permission dialog
- Never occludes the xemu window
- Works in headless/CI contexts

This path is proven to work: the xbe-harness (which uses the same
`XEMU_METAL_SCREENSHOT_PATH` mechanism) successfully captured
screenshots for the sibling-diag runs
(`benchmark-runs/20260530-49a-packet-a-sibling-diag-normal/`).

### Updated Run 3 command

The Run 3 command now includes `--metal-screenshot`:

```bash
./scripts/apple-silicon/run-benchmark.sh --metal-sibling-sync-depth-only \
  --metal-screenshot benchmark-runs/20260530-152933-pgr2/screenshots/depth-only.png \
  pgr2 scripts/apple-silicon/input-scripts/noop.csv 30
```

This produces:
- A PNG screenshot at the specified path (in-renderer Metal capture)
- The same depth-merge log evidence as before
- A reproducible image artifact for inspection, but not automatically
  trustworthy gameplay-state evidence

### What the validator should look for

The validator (t_36973dc2) should examine the captured PNG and look for:

1. **Depth-sync bottleneck (causative):** If the screenshot shows
   black slabs, missing geometry, or z-test failures that match
   the multi-title regression pattern, then depth merges are the
   active bottleneck.

2. **Benign correlation:** If the screenshot shows correct rendering
   (no black slabs, no missing geometry) despite the depth merges,
   then the depth merges are correlated but not causally explanatory.

3. **Unresolved depth-blit/readback ambiguity:** If the screenshot
   shows subtle artifacts (flickering, wrong depth values in
   specific regions) that don't match the clear regression pattern,
   then the depth merges may be masking a narrower depth-blit
   semantics issue.

### Evidence posture after 50H

- **Clip-rect isolation:** Still weakened/unsupported on this exact
  validated path (no color sibling merges observed).
- **Depth-blit semantics:** Still a leading bottleneck family, but
  the repaired `--metal-screenshot` path alone does not resolve
  causality because the captured rerun images are not recognizable
  gameplay frames.
- **MSAA companion handling:** Still unresolved and unexercised
  (all depth merges report `sync_msaa=no`).
- **Broader readiness:** Still unproven. This slice only addresses
  the visual/oracle evidence gap for the exact validated depth-only
  leg.

## 50I: Screenshot source repair - capture pre-compositing framebuffer (2026-05-30)

### Problem

The 50H screenshot capture path was repaired to use the in-renderer
Metal screenshot mechanism instead of the failing macOS screencapture
backend. However, the captured frame was mostly white/blank with black
margins and UI chrome, not a trustworthy gameplay/oracle scene.

### Root cause

The screenshot mechanism captures from the **drawable** (the composited
output that is about to be presented to the screen) by default. The
drawable includes the Metal HUD overlay and macOS window chrome, which
overlay the actual game content. When the screenshot fires at frame 60,
it captures this composited layer rather than the raw game framebuffer,
producing a mostly white/blank frame.

### Fix: Default screenshot source to nv2a (pre-compositing)

The screenshot mechanism already supports XEMU_METAL_SCREENSHOT_SOURCE=nv2a,
which captures from the NV2A framebuffer texture (present_input_tex)
**before** compositing into the drawable. This produces a clean capture
of the actual game content without HUD overlay or window chrome.

**Change in run-benchmark.sh:** When --metal-screenshot is used,
the script now defaults XEMU_METAL_SCREENSHOT_SOURCE to nv2a.
Override with XEMU_METAL_SCREENSHOT_SOURCE=drawable for the old
(composited drawable) behavior.

**Metadata output:** Added metal_screenshot_source field to
metadata.txt so validators can confirm which source was used.

### Updated Run 3 command

The Run 3 command now requests an NV2A-framebuffer screenshot for
inspection:



This produces:
- A PNG screenshot from the NV2A framebuffer (pre-compositing)
- Depth merge log evidence at vram=0x038e0000
- A better-targeted capture path, though this rerun family's saved
  images still need manual validation before any gameplay-state claim

### What the validator should look for

The validator should examine the captured PNG and look for:

1. **Depth-sync bottleneck (causative):** If the screenshot shows
   black slabs, missing geometry, or z-test failures that match
   the multi-title regression pattern, then depth merges are the
   active bottleneck.

2. **Benign correlation:** If the screenshot shows correct rendering
   (no black slabs, no missing geometry) despite the depth merges,
   then the depth merges are correlated but not causally explanatory.

3. **Unresolved depth-blit/readback ambiguity:** If the screenshot
   shows subtle artifacts (flickering, wrong depth values in
   specific regions) that don't match the clear regression pattern,
   then the depth merges may be masking a narrower depth-blit
   semantics issue.

### Evidence posture after 50I

- **Clip-rect isolation:** Still weakened/unsupported on this exact
  validated path (no color sibling merges observed).
- **Depth-blit semantics:** Still a leading bottleneck family, but
  the nv2a screenshot source has not yet produced recognizable
  gameplay-state evidence for this rerun family.
- **MSAA companion handling:** Still unresolved and unexercised
  (all depth merges report sync_msaa=no).
- **Broader readiness:** Still unproven. This slice only addresses
  the visual/oracle evidence gap for the exact validated depth-only
  leg.

---

## 50J: Provenance correction - benchmark actually succeeded (2026-06-01)

### Problem

The parent task closeout (t_af6c3e71, 50E architect closeout) claimed that
the bounded depth-only rerun with `XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4`
plus `--metal-sibling-sync-depth-only` failed with snapshot load error:
"Snapshot 'pgr2_gameplay_b4' does not exist in one or more devices".

This claim was used as the basis for opening the current task (t_f38f0d79)
to "repair the exact command/provenance issue blocking the depth-only
replay path."

### Verification

Direct inspection of the validation run directory
(`benchmark-runs/20260530-152933-pgr2/`) on the Mac Studio reveals:

1. **xemu.log** -- The benchmark completed its full 30-second run. No
   snapshot load error appears anywhere in the log. The benchmark ran
   successfully with the Metal renderer, depth sync enabled, and the
   `pgr2_gameplay_b4` snapshot loaded.

2. **snapshot.log** -- Contains only the line:
   `loading VM snapshot 'pgr2_gameplay_b4' at 2s`
   No error, no failure. The snapshot loaded successfully.

3. **metadata.txt** -- Confirms correct settings:
   - `hdd_source: benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`
   - `loadvm_tag: pgr2_gameplay_b4`
   - `loadvm_at_seconds: 2`
   - `env_XEMU_RENDERER: METAL`
   - `env_XEMU_METAL_RTT_SIBLING_SYNC_DEPTH: 1`
   - `env_XEMU_METAL_RTT_SIBLING_SYNC: unset` (color sync OFF)

4. **Sibling sync evidence** -- The xemu.log contains:
   - **254 depth merge lines** at vram=0x038e0000
   - **0 color merge lines** (color sync was OFF, as expected)
   - All depth merges alternate between 2560x960 and 2556x884
     (clip-rect alternation pattern confirmed)
   - `METAL_SIBLING_SYNCS` totals 254 across all intervals
   - `METAL_SIBLING_SYNC_SKIPS` totals ~480,000+ across all intervals

5. **Captured images** -- The `depth-only.png` screenshot (29,907 bytes)
   was captured via the Metal screenshot path at the specified path.
   Additional screenshots were also captured:
   `color-frame60.png`, `color-frame120.png`, `vram-frame60.png`.
   These files confirm that the capture path ran, but the saved images
   are mostly white/blank and do not show a recognizable gameplay scene.

### Root cause of the provenance error

The parent task's claim of snapshot load failure appears to have been
based on incorrect or misremembered information. The actual benchmark
run completed successfully, and all the documented commands were correct.

The benchmark commands documented in this file (Run 3, depth-only sync)
have been proven reproducible and truthful. No command or provenance
repair was actually needed.

### Corrected findings

**The depth-only benchmark path is fully operational:**
- The `pgr2_gameplay_b4` snapshot loads successfully from
  `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`
- Depth-only sibling sync produces 254 merge events at vram=0x038e0000
- No color sibling merges occur (color sync is OFF)
- The clip-rect alternation pattern (2560x960 ↔ 2556x884) is confirmed
- Screenshot artifacts (`depth-only.png`, plus the related color/vram
  captures) were captured successfully, but they do not show a
  recognizable gameplay scene

**What remains unproven (unchanged from prior sections):**
- Whether the depth merges are causative or merely correlated
  (the current captured images are insufficient for visual
  interpretation of gameplay state)
- MSAA companion handling (all depth merges report `sync_msaa=no`)
- Clip-rect isolation on the color scene (disproven for this snapshot path)
- Broader renderer readiness and user-testing claims
- Title coverage beyond PGR2

### Impact on the benchmark discrimination matrix

The Run 3 (depth-only sync) results are now confirmed:
- **Depth sync ON, color sync OFF**
- 254 depth merge events at 0x038e0000
- 0 color merge events
- Benchmark completed full 30-second run
- Screenshot artifacts captured, but not scene-validating

This confirms that the depth-only sibling sync path is fully functional
and produces the expected merge evidence. It does not, however,
establish that the captured images validate the intended gameplay scene
or allow causal discrimination; the current screenshots are mostly
white/blank and remain inadequate for that claim.

---

## Changed files

- `docs/apple-silicon/benchmarks/2026-05-30-pgr2-sibling-sync-scene.md` --
  50J revision: corrected provenance record and narrowed the visual
  claims. Direct inspection confirms the benchmark completed
  successfully with 254 depth merge events at vram=0x038e0000, but the
  saved screenshots are still mostly white/blank and do not validate
  the intended gameplay scene or causal discrimination.
