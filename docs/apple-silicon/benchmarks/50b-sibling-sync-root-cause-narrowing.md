# 50B — Sibling-sync root-cause narrowing: depth-blit vs. clip-rect isolation

Date: 2026-05-30
Task: t_4ca14362 (50B implementation: root-cause cross-title regressions from opt-in RTT sibling sync)
Repo: /Users/jbbrack03/XEMU_MacOS/xemu-fork, branch review-packet-a, HEAD 5e7a844269

## Goal

Discriminate among three root-cause hypotheses for why the opt-in
`XEMU_METAL_RTT_SIBLING_SYNC` bind-time cross-sibling copy improves
PGR2 while regressing boot logo, Halo, and Crimson Skies.

## Three hypotheses

1. **Depth blit semantics** — the `copyFromTexture` for
   `MTLPixelFormatDepth32Float_Stencil8` textures does not correctly
   carry the stencil aspect, causing z-test failures in titles that
   use depth-stencil formats.

2. **MSAA companion handling or alignment** — the MSAA blit-copy has
   alignment constraints or the `sync_msaa` guard silently skips MSAA
   sync when sample counts mismatch, causing content divergence.

3. **Illegitimate merging across clip-rect-isolated siblings** — the
   sync blindly merges siblings based on VRAM/pitch/format without
   checking clip-rect isolation. PGR2 benefits because both siblings
   render the same scene; other titles may legitimately need isolation
   (different render stages, camera views, or UI layers).

## Analysis

### Hypothesis 1 (Depth blit semantics) — WEAK

The `copyFromTexture` for depth-stencil formats copies the entire
texture including all aspects (depth + stencil are stored in the same
MTLTexture). The code passes no `srcOptions`/`dstOptions`, which is
correct for a full-texture copy. The depth sync is issued after
`pgraph_mtl_draw_flush_open_pass()` which drains the open render
encoder, so the source's depth content is committed before the blit
reads it.

**Verdict**: The depth blit semantics appear correct. This hypothesis
is not strongly supported by the code.

### Hypothesis 2 (MSAA companion handling) — MODERATE

The MSAA blit copies `msaa_texture` from source to target. Both sides
must have matching sample counts (`sync_msaa` guard). The MSAA
textures are `MTLTextureType2DMultisample` with
`MTLStorageModePrivate`. The blit is issued on the same queue as
draws, with a wait-for-event fence.

The `sync_msaa` guard could silently skip MSAA sync when sample counts
mismatch, but this would affect both PGR2 and other titles equally,
not explain why PGR2 improves while others regress.

**Verdict**: Possible but doesn't explain the asymmetric regression
pattern. Not the primary suspect.

### Hypothesis 3 (Illegitimate merging across clip-rect-isolated siblings) — STRONG

This is the most likely root cause. The sync blindly merges siblings
based on VRAM address, pitch, format, and aspect — but does NOT check
clip-rect isolation.

For PGR2, the two siblings at `0x3c84000` (1278x442 and 1280x480)
render the same scene from the same camera, so merging helps close
the magenta artifact. For other titles, the clip-rect alternation may
serve a legitimate purpose (different render stages, different camera
views, or different UI layers), and merging them causes visual
corruption.

The regression symptoms support this:
- Boot logo: parts black + checkerboarded (wrong sibling content
  merged into the active render target)
- Halo: black screen throughout (depth sync merges wrong depth
  values, causing z-test failures across the entire frame)
- Crimson Skies: flickering + missing UI (color sync merges
  siblings that should remain isolated for different render stages)

**Verdict**: Strongly supported. This is the primary hypothesis.

## Diagnostic change

Added a separate env flag `XEMU_METAL_RTT_SIBLING_SYNC_DEPTH`
(default OFF) so that color-sync-only experiments can be tested
independently of the depth sync. This discriminates between:

- Hypothesis 1 (depth blit): If disabling depth sync eliminates
  regressions, this is supported.
- Hypothesis 3 (illegitimate merging): If disabling depth sync does
  NOT eliminate regressions, the color sync merging is the culprit.

### Files changed

- `hw/xbox/nv2a/pgraph/mtl/surface.mm`:
  - Added `sibling_sync_depth_enabled()` function (reads
    `XEMU_METAL_RTT_SIBLING_SYNC_DEPTH`, default OFF)
  - Modified `sync_depth_siblings_into()` to use
    `sibling_sync_depth_enabled()` instead of `sibling_sync_enabled()`
  - Added diagnostic `fprintf(stderr, ...)` logging in both
    `sync_color_siblings_into()` and `sync_depth_siblings_into()`
    that records: vram_addr, source dimensions, target dimensions,
    format, draw_seq_delta, and whether MSAA sync was performed

### Build

- `./build.sh -a arm64`: PASS
- M5 shader-validation post-build gate: 7/7 passed, 0 failed

## Next steps for validator (t_cff5160e)

1. Build the patched xemu on the Mac.
2. Run the multi-title regression suite with:
   - `XEMU_METAL_RTT_SIBLING_SYNC=1 XEMU_METAL_RTT_SIBLING_SYNC_DEPTH=0`
     (color sync only, depth sync OFF)
   - `XEMU_METAL_RTT_SIBLING_SYNC=1 XEMU_METAL_RTT_SIBLING_SYNC_DEPTH=1`
     (both syncs ON)
3. Compare visual output across boot logo, Halo, Crimson Skies, and
   PGR2 for both configurations.
4. Check the stderr diagnostic logs for merge details (clip dimensions,
   draw_seq_delta, sync_msaa) to identify whether the regressions
   correlate with specific merge patterns.
5. Report which hypothesis is now better supported or ruled out.

## What remains unproven for broader readiness

- The sibling-sync path is NOT a shippable fix. It remains default OFF.
- Broader Metal readiness (jitter, visual regressions on other
  features) is separate from this sibling-sync lane.
- The retail Xbox oracle multi-title visual validation gate has NOT
  been run against this change.
- p99 jitter is not addressed by this slice.
