# Flat Triangle Depth XBE

This nxdk test alternates every 240 frames between two flat-shaded triangles:

- Left: `NV097_SET_FLAT_SHADE_OP_VERTEX_FIRST`, expected to use xemu's native
  triangle-depth path when `XEMU_NATIVE_TRI_DEPTH=1`.
- Right: `NV097_SET_FLAT_SHADE_OP_VERTEX_LAST`, expected to keep using the
  geometry-shader fallback.

Build from this directory with:

```sh
NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk \
PATH=/Users/jbbrack03/XEMU_MacOS/nxdk/bin:/opt/homebrew/Cellar/lld@19/19.1.7/bin:/opt/homebrew/opt/llvm/bin:$PATH \
make
```

The build emits `bin/default.xbe` and `flat-tri-depth.iso`.

After rebuilding, copy or run `flat-tri-depth.iso` from this directory. The
benchmark launcher also records disc size and modification time in
`metadata.txt`; use that to catch stale ISO runs before interpreting counters.

Current validation status: stale ISO has been ruled out, trace runs confirm the
XBE sends flat shade mode plus first/last provoking-vertex state, and final perf
flushing captures the expected counter split. Run
`benchmark-runs/20260430-153555-flat-tri-depth` reported 480
`NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST` draws, 304
`NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` fallbacks, and 304
`GEOM_SHADER_DRAW_TRI` draws.

Preferred pass/fail validation command from the repository root:

```sh
scripts/apple-silicon/validate-native-tri-depth.sh --run 20
```

Fresh packaged-app validation after the code cleanup passed in
`benchmark-runs/20260430-210159-flat-tri-depth` with 422 flat-first native
draws, 240 flat-nonfirst fallbacks, and 240 triangle-family geometry-shader
draws.

Trace-enabled benchmark command, only for debugging:

```sh
XEMU_NATIVE_TRI_DEPTH=1 \
XEMU_BENCH_SCREENSHOT_BACKEND=none \
XEMU_PERF_LOG_INTERVAL_MS=1000 \
XEMU_BENCH_EXTRA_QEMU_ARGS='-trace nv2a_pgraph_method' \
scripts/apple-silicon/run-benchmark.sh flat-tri-depth \
  scripts/apple-silicon/input-scripts/noop.csv 22
```

Passing means `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST` is nonzero during the
first-provoking phase, and `NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST` plus
`GEOM_SHADER_DRAW_TRI` are nonzero during the last-provoking phase.
