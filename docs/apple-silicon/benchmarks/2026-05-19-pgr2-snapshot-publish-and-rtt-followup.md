# 2026-05-19 — PGR2 snapshot follow-up: host-refresh publish fix landed, RTT correctness still blocks Metal

Goal: use the new May 19 measurement tools to continue the PGR2
`pgr2_gameplay_b4` snapshot investigation, validate candidate front-fb
publish fixes frame-by-frame, and decide whether the remaining failure is
still in the publish path or deeper in the render-target-as-texture path.

## Conclusion

One real bug was fixed, but the Metal backend is **not** visually correct
yet.

- **Fixed:** the per-host-refresh `crtc-refresh` publish in
  `pgraph_mtl_get_framebuffer_surface()` was overwriting the guest
  flip-stall fallback publish every host vsync. This caused the
  "one good frame, then it disappears" behavior in temporal captures.
- **Rejected:** a display-shape heuristic that preferred the
  640×480 format-4 sibling (`0x3b58000`) over the dominant-draw
  wide surface (`0x3c84000`) made the publish graph look cleaner, but
  full frame-by-frame validation showed it was still wrong: stable late
  frames lost geometry and HUD detail.
- **Current blocker:** even after the host-refresh clobber fix, the
  stable late PGR2 frames are still wrong under the dominant-draw path.
  The best current evidence points to render-target-as-texture
  correctness, not pure front-fb selection. Stage 0 repeatedly samples
  `0x3c84000` through the linear external-surface fast path during the
  bad late frames, and disabling the fast path does not restore GL-like
  output.

Do **not** treat the Metal backend as visually correct for PGR2. Do
**not** escalate to retail-oracle gameplay validation for this title
until local GL-vs-Metal content alignment is materially improved.

## Build + validation gate

All code iterations in this note rebuilt cleanly with:

```sh
./build.sh -a arm64
```

Each rebuild passed the post-build Metal shader-validation gate:

- `summary: 7/7 passed, 0 failed`

## Snapshot anchor

The PGR2 snapshot tag lives on the dedicated May 1 snapshot HDD, not the
generic profile-prep image:

- HDD:
  `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`
- Tag:
  `pgr2_gameplay_b4`

Using the wrong HDD causes a misleading "snapshot missing" failure, so all
runs below explicitly export:

```sh
XEMU_BENCH_HDD_SOURCE=benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2
XEMU_BENCH_LOADVM_TAG=pgr2_gameplay_b4
XEMU_BENCH_LOADVM_AT=2
```

## Runs

### Initial reference runs

These established the candidate final-composite surfaces before any code
change this evening:

- Metal fallback reference:
  `benchmark-runs/20260519-175458-pgr2`
- Direct `vram:0x3c84000` capture:
  `benchmark-runs/20260519-175545-pgr2`
- Direct `vram:0x3b58000` capture:
  `benchmark-runs/20260519-175601-pgr2`
- GL reference:
  `benchmark-runs/20260519-175713-pgr2`
- First surface-graph dump:
  `benchmark-runs/pgr2-snapshot-current.surface-graph.jsonl`

The direct captures established the core ambiguity:

- `0x3c84000` could show an upside-down / corrupted composite.
- `0x3b58000` could show a sane-looking PGR2 gameplay frame.

That made `0x3b58000` a tempting publish candidate, but not a proven one.

### Shape-based publish experiments

The new surface-graph dump tool made it possible to test publish-source
selection directly:

- first shape-heuristic run:
  `benchmark-runs/20260519-180555-pgr2`
  with graph
  `benchmark-runs/pgr2-snapshot-postfix2.surface-graph.jsonl`
- narrowed format-4-only heuristic:
  `benchmark-runs/20260519-180921-pgr2`
  with graph
  `benchmark-runs/pgr2-snapshot-postfix3.surface-graph.jsonl`

Graph result from the narrowed heuristic:

- early/mid flips returned to the original dominant-draw sequence
  (`0x2e06000`, `0x2c06000`, `0x3628000`)
- late flips switched to `0x3b58000`
  instead of `0x3c84000`

This looked promising in the graph, but it failed the visual bar when
validated frame-by-frame. Stable late frames such as
`benchmark-runs/20260519-181911-pgr2/frames/metal-gameplay.0125.png`
through `.0138.png` remained wrong: missing geometry, blank HUD text,
and bad reflective sampling compared with the earlier direct
`0x3b58000` sanity frame
`benchmark-runs/20260519-175601-pgr2/frames/metal-gameplay.0135.png`.

Conclusion: the display-shape heuristic was selecting a surface that
could look plausible in isolated stills while still being wrong over a
full validated sequence. The heuristic was removed.

### Host-refresh publish clobber fix

Late in the session, the real publish-path bug was identified:

- the guest flip-stall path could publish a fallback-selected surface
- but the host-refresh path immediately overwrote that choice every vsync
  with `pgraph_mtl_surface_publish_front_fb_pointer_only(..., "crtc-refresh")`

That interaction explained transient "flash of the right frame" behavior.

The fix landed in `renderer.c`: when the front-fb fallback mode is
enabled and a front framebuffer is already published, the host-refresh
path no longer stomps it with a CRTC pointer-only publish.

Evidence run after this fix:

- `benchmark-runs/20260519-181911-pgr2`
- graph:
  `benchmark-runs/pgr2-snapshot-postfix4.surface-graph.jsonl`

This was a real improvement:

- late publish sequence stabilized on one chosen surface instead of
  oscillating back to the stale CRTC surface
- temporal analysis improved sharply:
  `benchmark-runs/20260519-181911-pgr2/flicker/summary.json`
  reports `mean_changed_pct=0.4290`, `spike_count=2`

But the stable image was still wrong, so the fix closed only one layer
of the bug.

### Final current-build reference

After removing the rejected display-shape heuristic and keeping only the
host-refresh clobber fix, the current reference run is:

- Metal:
  `benchmark-runs/20260519-182241-pgr2`
- graph:
  `benchmark-runs/pgr2-snapshot-postfix5.surface-graph.jsonl`
- GL:
  `benchmark-runs/20260519-182716-pgr2`
- gameplay compare:
  `benchmark-runs/m15-gameplay-pgr2-postfix5-gl-compare/summary.json`

The graph shows the late phase stably publishing `0x3c84000`:

- publish counts:
  `0x3c84000 23`, `0x2c06000 20`, `0x3628000 12`, `0x2e06000 12`
- last 20 flips all publish
  `0x3c84000` with `reason=fallback-dominant-draw`

That is useful because it removes publish instability from the diagnosis.
The output is still wrong even when the publish choice is stable.

### Linear-alias copy follow-up

The next experiment targeted the late stage-0 `0x3c84000` alias path
directly.

Run:

- Metal temporal capture with the same snapshot anchor and noop route:
  `benchmark-runs/20260519-201711-pgr2`
- strict GL compare:
  `benchmark-runs/m15-gameplay-pgr2-linear-alias-copy-gl-compare/summary.json`

Code change under test:

- linear same-VRAM alias siblings no longer bridge through VRAM before the
  stage-0 bind
- instead, the exact matched surface is copied GPU-to-GPU into a sampled
  texture (`path=copy-alias`) while preserving rect-texture coordinate
  normalization

Why this was worth testing:

- the old path was emitting synthetic dirty events for both
  `0x3c84000` siblings right before the late bind:
  one `write_len=2263040` event for the clipped `1278x442` sibling and one
  `write_len=2457600` event for the full `1280x480` sibling
- that meant the alias bridge was round-tripping partial-footprint data
  through guest VRAM before re-uploading the final sampled surface

What changed measurably:

- the late bind path really switched:
  `metal_surface_texture stage=0 vram_addr=0x3c84000 ... path=copy-alias`
- the synthetic `metal_surface_dirty vram_addr=0x3c84000 ...` lines
  disappeared from the late bind window
- strict GL-vs-Metal alignment improved but still failed hard:
  old best-match distances `0.4925..0.5596` became
  `0.4505..0.4818`

What did **not** change enough:

- PGR2 is still visually wrong; representative frame
  `benchmark-runs/20260519-201711-pgr2/frames/metal-gameplay.0255.png`
  still shows the giant dark overpass slab and white HUD bars instead of the
  sane gameplay composition seen in
  `benchmark-runs/20260519-175601-pgr2/frames/metal-gameplay.0135.png`
- gameplay compare remains `INFRA-FAIL`
- temporal metrics are not a win by themselves:
  `mean_changed_pct` rose from `1.5758` to `2.8692`

Conclusion:

- **keep** the linear-alias copy change as a targeted correctness
  improvement and better localization step
- **do not** treat it as closure; the remaining blocker is now narrower:
  even after removing the lossy alias-to-VRAM bridge, the copied
  `0x3c84000` content is still not GL-like
- next work should focus on why the copied late composite is wrong
  (content/format/use-site), not on the already-removed VRAM bridge

## Frame-by-frame findings

This slice explicitly validated full temporal sequences, not just a few
keyframes.

### What improved

The host-refresh clobber fix turned a transient late-stage image into a
stable late-stage image. That is measurable progress and is worth keeping.

### What still fails

The stable late frames in
`benchmark-runs/20260519-182241-pgr2/frames/metal-gameplay.0583.png`
through `.0602.png` are still visibly wrong:

- white HUD bars in place of readable text
- corrupted car reflections / magenta interior patches
- broken geometry / black cutouts
- scene composition visibly unlike the earlier sane gameplay frame and
  unlike the GL reference

This is why the title is still blocked even though the publish graph is
now more coherent.

## GL-vs-Metal evidence

Strict gameplay comparison on the current best Metal run still fails hard:

- output:
  `benchmark-runs/m15-gameplay-pgr2-postfix5-gl-compare/summary.json`
- verdict:
  `INFRA-FAIL`

Representative failure lines:

- `GL frame 103 best Metal match 313 alignment_distance=0.5596 exceeds 0.3500`
- `GL frame 192 best Metal match 518 alignment_distance=0.4925 exceeds 0.3500`

This is sufficient local evidence that the Metal output still diverges too
far from GL to justify a retail-oracle gameplay pass for PGR2.

## RTT / texture-path evidence

The late bad frames correlate with stage-0 surface sampling of
`0x3c84000`.

From the current-build log:

- `benchmark-runs/20260519-182241-pgr2/xemu.log`

Repeated lines during the late phase:

- `metal_surface_texture stage=0 vram_addr=0x3c84000 ... path=external`

That points to the linear external-surface fast path in
`texture_pg.c` / `texture.mm`, not just front-fb publication.

To test that directly, the same snapshot was rerun with:

```sh
XEMU_METAL_DISABLE_SURFACE_TEX=1
```

Run:

- `benchmark-runs/20260519-182540-pgr2`

Result: the corruption changed shape but **did not go away**. Late frames
such as
`benchmark-runs/20260519-182540-pgr2/frames/metal-gameplay.0192.png`
and `.0200.png` are still badly wrong. So the bug is not "only the
surface-texture fast path"; it remains in the broader RTT-as-texture /
alias correctness space.

## Code state worth keeping

Keep:

- the host-refresh publish preservation in `renderer.c`
  (it removes a real false-overwrite bug)
- the earlier surface-copy / alias-dirtying infrastructure in
  `texture_pg.c`, `texture.mm`, `surface.mm`
  (still needed groundwork)

Do **not** keep or re-introduce:

- the display-shape publish heuristic that forces `0x3b58000`
  (graph improvement, visual regression)

## Next debugging slice

The next session should start from the current best Metal run:

- `benchmark-runs/20260519-182241-pgr2`

First task:

1. Instrument the late PGR2 RTT sampling path around
   `texture_pg.c` stage 0 binds of `0x3c84000`.
2. Determine whether the corruption comes from:
   - wrong source surface contents,
   - wrong format/alias interpretation,
   - stale sibling view,
   - or incorrect use of the sampled RTT in the final composite draw.
3. Re-run the same snapshot + full temporal validation after each change.

Do not spend the next slice on retail-oracle gameplay validation for PGR2.
The local GL-vs-Metal mismatch is still too large for oracle time to be
the limiting factor.
