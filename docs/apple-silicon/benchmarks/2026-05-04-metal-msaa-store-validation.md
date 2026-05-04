# 2026-05-04 Metal MSAA Store/Resolve Validation

## Summary

PGR2 exposed a real Metal MSAA4 correctness bug after the previous
surface/RTT fixes: the frame was mostly black with only UI text visible
when `XEMU_METAL_MSAA=4` was enabled. The root cause was the MSAA
render-pass store policy. Color passes used
`MTLStoreActionMultisampleResolve`, which updates only the single-sample
resolve target; later pass breaks used `MTLLoadActionLoad` on the MSAA
texture and could therefore load discarded/stale black contents. Depth
and stencil used `MTLStoreActionDontCare` while later passes also load
those MSAA attachments.

Fix:

- `hw/xbox/nv2a/pgraph/mtl/draw.mm`: MSAA color draw passes now use
  `MTLStoreActionStoreAndMultisampleResolve`; MSAA depth/stencil draw
  passes now use `MTLStoreActionStore`.
- `hw/xbox/nv2a/pgraph/mtl/surface.mm`: MSAA color clear passes now use
  `MTLStoreActionStoreAndMultisampleResolve`; MSAA depth/stencil clear
  passes now use `MTLStoreActionStore`.

Common env for the successful visual canaries:

```sh
XEMU_RENDERER=METAL
XEMU_METAL_TRANSLATED_PIPELINE=1
XEMU_NATIVE_TRI_DEPTH=1
XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1
XEMU_METAL_FRONT_FB_FALLBACK=1
XEMU_METAL_MSAA=4
XEMU_PERF_FRAME_LOG=1
XEMU_BENCH_SCREENSHOT_BACKEND=none
```

## Failure Reproduced Before Fix

- PGR2: `benchmark-runs/20260504-100203-pgr2`
- Screenshot:
  `benchmark-runs/visual-checks/pgr2-gate-metal-msaa4-f900.png`
- Result: FAIL. Mostly black frame with only UI prompt visible.
- Key counters: `METAL_PIPELINE_FAILED=0`,
  `METAL_PIPELINE_TRANSLATED_FAILED=0`, `METAL_PIPELINE_FALLBACKS=0`,
  `METAL_DRAWS_SKIPPED_PENDING_TOTAL=0`, `METAL_SCREENSHOTS_TAKEN=1`,
  `INPUT_LAT_US_MAX=2484`.

## Successful MSAA4 Canaries After Fix

| Title | Run | Visual | FPS / mspf | Notes |
| --- | --- | --- | --- | --- |
| PGR2 | `benchmark-runs/20260504-100458-pgr2` | PASS, `benchmark-runs/visual-checks/pgr2-gate-metal-msaa4-f900-after-msaa-store.png` | `post_load_avg_fps=42.12`, `post_load_avg_mspf=16.19` | `METAL_MSAA_RESOLVE_COUNT=952`, `INPUT_LAT_US_MAX=2494` |
| Rainbow Six 3 | `benchmark-runs/20260504-100546-rainbow-six-3` | PASS, `benchmark-runs/visual-checks/rainbow-gate-metal-msaa4-f600-after-msaa-store.png` | `post_load_avg_fps=31.41`, `post_load_avg_mspf=34.87` | Loading-screen route; counters clean, loading jitter remains noisy |
| Xbox boot/flubber | `benchmark-runs/20260504-100747-crimson-skies` | PASS, `benchmark-runs/visual-checks/boot-gate-metal-msaa4-f300-after-msaa-store.png` | `post_load_avg_fps=24.98`, `post_load_avg_mspf=74.06` | Boot/loading route, use as visual/stability canary only |
| Halo CE | `benchmark-runs/20260504-101125-halo-ce` | PASS, `benchmark-runs/visual-checks/halo-gate-metal-msaa4-f1200-after-msaa-store.png` | `post_load_avg_fps=30.51`, `post_load_avg_mspf=32.17` | Menu textures/colors clean; loading spikes still affect p99 |

All successful rows had `METAL_PIPELINE_FAILED=0`,
`METAL_PIPELINE_TRANSLATED_FAILED=0`, `METAL_PIPELINE_FALLBACKS=0`,
`METAL_SHADER_COMPILE_FAILED_TOTAL=0`, and
`METAL_DRAWS_SKIPPED_PENDING_TOTAL=0`.

## Routes That Are Not Visual Canaries Yet

- Crimson gameplay: `benchmark-runs/20260504-100815-crimson-skies`
  completed and counters stayed clean, but interval screenshots
  `crimson-gameplay-gate-msaa4-after-msaa-store.0001.png` through
  `.0013.png` showed one patterned frame followed by black drawable
  captures. Treat as stability/perf data only until the automation
  reaches a rendered gameplay frame.
- SC2: `benchmark-runs/20260504-101242-soul-calibur-2` sustained
  `post_load_avg_fps=57.63` with clean MSAA4 counters, but the
  no-input route captured boot/flubber and then black frames. Add a
  routed input script or load a known-good snapshot before using SC2
  for visual diff.
- PGR2 without the front-fb fallback:
  `benchmark-runs/20260504-101416-pgr2` produced an upside-down/wrong
  frame at
  `benchmark-runs/visual-checks/pgr2-gate-metal-msaa4-no-front-fb-f900-after-msaa-store.png`.
  This confirms `XEMU_METAL_FRONT_FB_FALLBACK=1` is still required for
  the current PGR2 Metal canary; M15 default-on should not ignore this.

## Verdict

The MSAA4 black-frame bug is fixed for the core render/clear path and
validated across PGR2, Rainbow Six 3, Xbox boot/flubber, and Halo CE.
M15 default-on remains blocked by the front-fb fallback policy, the
Crimson gameplay visual route, the SC2 visual route, and the full paired
Metal-vs-GL visual/perf gate.
