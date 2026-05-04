# 2026-05-04 Metal PGR2 Texture-Bind Attribution

## Summary

This handoff session continued the Metal backend work after the surface/RTT
visual canaries. The strategy was to map the mature OpenGL/Vulkan texture and
shader behavior onto Metal instead of guessing. The session produced three
code fixes and one measurement slice:

- Metal texture binding no longer treats `pg->texture_dirty[stage]` as proof
  that guest VRAM changed. That flag is bind/register state; QEMU VRAM dirty
  ranges are the content signal.
- Paletted textures no longer invalidate the Metal texture cache on every
  bind. Palette changes are covered by the same dirty-range probe as texture
  bytes.
- Shared GLSL pixel-shader generation now declares `dot0..dot3` once and
  assigns dot-combiner intermediates, fixing Metal/Vulkan/GL shader generation
  failures for PGR2 dot-reflection/specular modes.
- Metal now emits CPU wall-time counters for dispatch, texture bind, draw
  encode, render-pass flush, texture upload, and surface download hot paths.

After the shader fix, PGR2 no longer reported Metal translation failures or
pipeline fallbacks. The remaining PGR2 Metal pacing bottleneck was measured as
CPU time inside texture binding, not draw encode, pass flushing, or shader
fallback.

## Builds And Validation

Validated during the session:

```sh
./build.sh -a arm64
codesign --verify --deep --strict --verbose=2 dist/xemu.app
scripts/apple-silicon/metal-shader-validation/run-validation.sh
```

Observed results:

- Build: PASS before the final early-cache fast-path edit.
- Codesign: PASS.
- Shader validation: PASS, `7/7 passed, 0 failed`.
- A later rebuild after adding C-linkage for new `.mm` perf accessors also
  completed successfully.

The final early-cache fast path in this commit was built, but still needs the
next long PGR2 gameplay rerun before it can be claimed as a performance win.

## Key Runs

### PGR2 before semantic texture-dirty fix

- Run: `benchmark-runs/20260504-125456-pgr2`
- Result: still showed the texture upload storm.
- Late intervals remained around thousands of uploads and multi-GB/s upload
  traffic.

### PGR2 after semantic texture-dirty and shader-dot fixes

- Run: `benchmark-runs/20260504-132806-pgr2`
- Renderer settings:

```sh
XEMU_RENDERER=METAL
XEMU_METAL_TRANSLATED_PIPELINE=1
XEMU_NATIVE_TRI_DEPTH=1
XEMU_NATIVE_QUAD=1
XEMU_PGRAPH_FAST_READ=1
XEMU_METAL_FRONT_FB_FALLBACK=1
XEMU_METAL_MSAA=4
XEMU_MACOS_NATIVE_INPUT=1
XEMU_BENCH_SCREENSHOT_BACKEND=none
XEMU_PERF_FRAME_LOG=1
XEMU_BENCH_VISUAL_ANALYSIS=0
```

- Duration: 150s. This satisfies the requirement to run past the Xbox/title
  loading animation and into title behavior.
- Summary:
  - `post_load_avg_fps=11.21`
  - `METAL_GLSL_TRANSLATE_FAIL=0`
  - `METAL_PIPELINE_FALLBACKS=0`
  - `METAL_TEX_UPLOADS_TOTAL=100986`
  - `METAL_TEX_UPLOAD_BYTES_TOTAL=8660842208`
  - `METAL_DISPATCH_US_TOTAL=118851264`
  - `METAL_TEX_BIND_US_TOTAL=105962660`
  - `METAL_DRAW_ENCODE_US_TOTAL=7159221`
  - `METAL_DRAW_PASS_OPENS=103414`
  - `METAL_DRAW_PASS_COALESCED=572886`
  - `METAL_OPEN_PASS_FLUSH_US_TOTAL=103402`
  - `INPUT_LAT_US_MAX=2499`

Late gameplay intervals showed the same pattern repeatedly: roughly 1.1-1.4
seconds per wall-clock second were spent in `pgraph_mtl_texture_bind_from_pg`,
while draw encode and pass flush time were small. Example:

```text
id=75 fps=3.97 draws=6520 dispatch_us=1229797 bind_us=1116173 encode_us=82999
id=80 fps=3.79 draws=8150 dispatch_us=1542203 bind_us=1398519 encode_us=105225
id=98 fps=3.99 draws=6512 dispatch_us=1227851 bind_us=1114267 encode_us=82911
```

This established the next bottleneck: Metal was decoding/converting texture
data before the cache could answer whether the texture was already resident.
OpenGL and Vulkan both avoid this by checking cache/binding state before
regenerating texture contents.

## Final Code State

The final patch in this commit adds `pgraph_mtl_texture_bind_slot_cached_full`
and moves the Metal cache-hit decision ahead of `decode_face_levels()` when the
texture range and palette range are clean. This should make cache hits cheap:
the renderer binds the resident `id<MTLTexture>` and sampler without paying CPU
decode/conversion cost.

The next session should rerun the same 150s PGR2 command and compare against
`benchmark-runs/20260504-132806-pgr2`. Expected evidence for success:

- `METAL_TEX_BIND_US_TOTAL` falls sharply in late intervals.
- `METAL_TEX_UPLOADS_TOTAL` falls if the remaining churn was avoidable cache
  miss/decode behavior.
- `post_load_avg_fps` rises toward the OpenGL control run
  `benchmark-runs/20260504-131504-pgr2` (`post_load_avg_fps=31.32`).

## Still Open

- Re-run PGR2 for at least 150s after the early-cache fast path.
- Add GL/Vulkan-style LRU and possibly dirty+content-hash reuse to Metal if
  uploads remain high.
- Re-run visual capture after performance stabilizes; the previous PGR2 visual
  storyboard still showed black car/body geometry and red HUD overlays before
  the shader-dot fix was validated visually.
- Keep M15 default-on blocked until Metal passes the broader GL comparison:
  PGR2, Rainbow Six 3, Crimson Skies, SC2, and at least one broader-sweep
  title with FPS, jitter, visuals, and input-latency evidence.
