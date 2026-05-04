# 2026-05-04 Metal PGR2 Surface / RTT Validation

## Summary

This session closed the PGR2 Metal menu/logo/text/color canary. The
remaining Metal default-on blocker is Crimson Skies visual correctness,
not the older PGR2 white/magenta/front-buffer failure.

## Build / Signing

- Build command: `./build.sh -a arm64`
- Result: PASS
- Signing check:
  `codesign --verify --deep --strict --verbose=2 dist/xemu.app`
- Result: PASS

Preflight before emulator/debugger runs:

```sh
pgrep -fl "Contents/MacOS/xemu|qemu-system-i386|lldb" || true
```

## Metal Configuration

Primary canary configuration:

```sh
XEMU_RENDERER=METAL
XEMU_METAL_TRANSLATED_PIPELINE=1
XEMU_NATIVE_TRI_DEPTH=1
XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1
XEMU_METAL_FRONT_FB_FALLBACK=1
```

Useful diagnostics kept for the next session:

```sh
XEMU_METAL_DISABLE_SURFACE_TEX_ADDRS=1
XEMU_METAL_DIAG_TEX_BIND=1
XEMU_METAL_DIAG_SURFACE_TEX=1
XEMU_METAL_DUMP_TARGET_SHADER=all
```

## What Changed

- Surface cache now supports multiple shapes for the same VRAM address,
  with exact/near shape lookup and a larger cap of 64 entries.
- Direct fallback publishing uses the selected binding instead of a
  stale global color binding.
- Render-target-as-texture lookup is dimension-aware, avoiding
  same-address shape aliasing.
- Access-callback registration and dirty uploads walk all same-VRAM
  sibling entries.
- Scaled upload now fills the full host-scaled texture instead of only
  the 1x guest sub-rect.
- A8R8G8B8 render targets sampled as linear A8R8G8B8-family texture
  views use the CPU texture path, fixing PGR2's dotted/yellow text and
  color normalization mismatch.

## Validation Results

| Game | Run | Screenshot | Result |
| --- | --- | --- | --- |
| PGR2 | `benchmark-runs/20260504-024441-pgr2` | `benchmark-runs/visual-checks/pgr2-final-f900.png` | PASS: menu/logo/textures/colors clean |
| Rainbow Six 3 | `benchmark-runs/20260504-024617-rainbow-six-3` | `benchmark-runs/visual-checks/rainbow-final-f600.png` | PASS: loading-screen logo/colors clean |
| Crimson Skies | `benchmark-runs/20260504-024740-crimson-skies` | `benchmark-runs/visual-checks/crimson-smoke-f300.png` | FAIL: untextured green aircraft over black scene |
| Crimson Skies passthrough | `benchmark-runs/20260504-024830-crimson-skies` | `benchmark-runs/visual-checks/crimson-passthrough-f300.png` | FAIL: all-white; not a better oracle |

PGR2 late-interval counter floors:

- `METAL_PIPELINE_TRANSLATED_FAILED=0`
- `METAL_DRAWS_SKIPPED_PENDING_TOTAL=0`
- `METAL_SURFACE_RECREATE_SHAPE_MISMATCH=0`

PGR2 late FPS mostly ranged from ~32 to 59. Input max was about
2.1-2.5 ms.

## Next Steps

1. Fix Crimson Skies Metal visual correctness. The clean counters point
   toward shader/texture semantics rather than surface churn.
2. Compare Crimson translated vs passthrough captures; use texture-bind,
   surface-texture, and target-shader diagnostics.
3. If needed, add a small nxdk/pbkit custom XBE to isolate the suspected
   texture-combiner, alpha/channel, render-target-as-texture, or
   vertex-color behavior. Avoid proprietary/leaked XDK dependencies.
4. Re-run the PGR2 and Rainbow canaries after every Crimson fix.
5. Keep M15 default-on blocked until PGR2, Rainbow, Crimson, SC2, and one
   broader-sweep title pass paired Metal-vs-GL visual diff and FPS/jitter
   validation.
