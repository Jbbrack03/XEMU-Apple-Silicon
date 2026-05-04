# 2026-05-04 Metal PGR2 Surface / RTT Validation

## Summary

This session closed the PGR2 Metal menu/logo/text/color canary and was
later superseded by the same-day boot/flubber + Crimson stability
follow-up. The earlier green/wireframe report was the Xbox boot
animation, not in-game Crimson Skies. The boot/flubber canary now
passes; Metal default-on remains blocked by the broader Metal-vs-GL
gameplay/visual-diff gate, not by this specific boot-animation failure.

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
- Follow-up in the same session fixed Metal front-face winding for the
  Xbox boot/flubber animation, added indexed unsupported-primitive
  expansion, hardened cubemap-border handling, and made invalid texture
  DMA offsets non-fatal via `pgraph_try_get_texture_phys_addr()` /
  `metal_tex_oob`.

## Validation Results

| Game | Run | Screenshot | Result |
| --- | --- | --- | --- |
| PGR2 | `benchmark-runs/20260504-024441-pgr2` | `benchmark-runs/visual-checks/pgr2-final-f900.png` | PASS: menu/logo/textures/colors clean |
| Rainbow Six 3 | `benchmark-runs/20260504-024617-rainbow-six-3` | `benchmark-runs/visual-checks/rainbow-final-f600.png` | PASS: loading-screen logo/colors clean |
| PGR2 latest | `benchmark-runs/20260504-092708-pgr2` | `benchmark-runs/visual-checks/pgr2-post-oob-f900.png` | PASS after texture-OOB hardening |
| Rainbow Six 3 latest | `benchmark-runs/20260504-092750-rainbow-six-3` | `benchmark-runs/visual-checks/rainbow-post-oob-f600.png` | PASS after texture-OOB hardening |
| Xbox boot/flubber | `benchmark-runs/20260504-092824-crimson-skies` | `benchmark-runs/visual-checks/boot-post-oob-f300.png` | PASS: shaded boot animation and glow; no green wireframe/blob failure |
| Crimson Skies gameplay stability | `benchmark-runs/20260504-092403-crimson-skies` | `benchmark-runs/visual-checks/crimson-gameplay-metal-f1800.png` | STABILITY PASS: no abort; screenshot is black transition/loading output, so not a visual canary |

PGR2 late-interval counter floors:

- `METAL_PIPELINE_TRANSLATED_FAILED=0`
- `METAL_DRAWS_SKIPPED_PENDING_TOTAL=0`
- `METAL_SURFACE_RECREATE_SHAPE_MISMATCH=0`

PGR2 late FPS mostly ranged from ~32 to 59. Input max was about
2.1-2.5 ms.

## Next Steps

1. Run the broader Metal-vs-GL gameplay gate: PGR2, Rainbow Six 3,
   Crimson Skies after the boot animation, SC2, and one broader-sweep
   title.
2. Add or retune the Crimson gameplay automation so it captures a
   rendered gameplay frame instead of the current black
   transition/loading frame.
3. For every Metal renderer change, re-run PGR2, Rainbow Six 3, and
   Xbox boot/flubber canaries plus the Crimson stability route.
4. Keep M15 default-on blocked until the paired visual diff, FPS/jitter,
   and input-latency gate passes.
