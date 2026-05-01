# PGR2 Native Quad Bypass Implementation And Snapshot Comparison

Date: 2026-05-01

## Purpose

Implement and validate `XEMU_NATIVE_QUAD=1`, the second opt-in geometry-shader
removal slice. The triangle-family bypass on its own already eliminates
~99% of geometry-shader draws in PGR2 (see
`2026-05-01-pgr2-native-tri-depth.md`); this slice removes the remaining
quad-family draws for smooth-fill primitives by expanding quads to native
triangles on the CPU and reusing the existing `gl_FragCoord`-derived
depth/slope code path in the fragment shader.

## What Landed

Source-code changes (see `git status` for a full diff):

- `hw/xbox/nv2a/debug.h`: new counters
  - `NV2A_PROF_GEOM_SHADER_DRAW_QUAD_LIST`,
    `NV2A_PROF_GEOM_SHADER_DRAW_QUAD_STRIP` (subtype split for the existing
    quad-family GS draw counter).
  - `NV2A_PROF_NATIVE_QUAD_DRAW`, `NV2A_PROF_NATIVE_QUAD_DRAW_LIST`,
    `NV2A_PROF_NATIVE_QUAD_DRAW_STRIP`,
    `NV2A_PROF_NATIVE_QUAD_CANDIDATE`,
    `NV2A_PROF_NATIVE_QUAD_CANDIDATE_SMOOTH`,
    `NV2A_PROF_NATIVE_QUAD_CANDIDATE_FLAT`,
    `NV2A_PROF_NATIVE_QUAD_FALLBACK`,
    `NV2A_PROF_NATIVE_QUAD_FALLBACK_FLAT`,
    `NV2A_PROF_NATIVE_QUAD_FALLBACK_NONFILL`,
    `NV2A_PROF_NATIVE_QUAD_DRAW_ZPERSPECTIVE`,
    `NV2A_PROF_NATIVE_QUAD_DRAW_LINEAR_Z`,
    `NV2A_PROF_NATIVE_QUAD_DRAW_POLY_OFFSET`.
- `hw/xbox/nv2a/pgraph/glsl/geom.{c,h}`:
  - New `pgraph_glsl_native_quad_enabled()` reading
    `XEMU_NATIVE_QUAD` (`=0` overrides on, `=1` enables, unset disables).
  - New `pgraph_glsl_native_quad_supported()` enforcing smooth shading +
    fill polygon mode + `PRIM_TYPE_QUADS`/`PRIM_TYPE_QUAD_STRIP`. Flat
    shading, non-fill polygon modes, and other primitive types fall back to
    the geometry shader.
  - `pgraph_glsl_need_geom()` returns false for quad-family draws when the
    native bypass is supported, so no geometry shader is generated.
  - `GeomState::native_quad` propagated through state setup.
- `hw/xbox/nv2a/pgraph/glsl/psh.{c,h}`:
  - `PshState::native_quad` set the same way `native_tri_depth` is set.
  - The fragment-shader generator now uses the native depth/slope path when
    either `native_tri_depth` or `native_quad` is set, since the math is
    primitive-agnostic.
- `hw/xbox/nv2a/pgraph/gl/shaders.c`:
  - `get_gl_primitive_mode()` returns `GL_TRIANGLES` for quad/quad-strip
    when the native bypass is active, instead of `GL_LINES_ADJACENCY` /
    `GL_LINE_STRIP_ADJACENCY` that the geometry shader requires.
- `hw/xbox/nv2a/pgraph/gl/draw.c`:
  - New per-subtype geometry-shader counters and per-fallback-reason
    native-quad counters.
  - `pgraph_gl_flush_draw()` extended on all four dispatch paths
    (draw_arrays, inline_elements, inline_buffer, inline_array). When the
    quad-family native bypass is active, the dispatcher expands the quad
    vertex stream into 6 triangle indices per quad (or 6 indices per
    successive quad-strip pair), uploads the result via
    `glBufferData(GL_STREAM_DRAW)` on a dedicated index buffer, and issues a
    single `glDrawElements(GL_TRIANGLES, ...)`. Triangulation matches the
    existing geometry shader's diagonal so smooth interpolation is
    unchanged.
- `hw/xbox/nv2a/pgraph/gl/renderer.h` and `gl/vertex.c`:
  - `gl_native_quad_index_buffer` element-array buffer plus a CPU-side
    growable scratch index array, allocated in `pgraph_gl_init_buffers()`
    and freed in `pgraph_gl_finalize_buffers()`.
- `scripts/apple-silicon/run-benchmark.sh`:
  - Records `env_XEMU_NATIVE_QUAD` in benchmark metadata.
- `scripts/apple-silicon/extract-perf-summary.sh`:
  - Surfaces the new geometry-shader and native-quad counters.

## Triangle Path Regression Gate

`scripts/apple-silicon/validate-native-tri-depth.sh --run 22` against
`benchmark-runs/20260501-105543-flat-tri-depth` passes:

```
PASS NATIVE_TRI_DEPTH_DRAW=287872
PASS NATIVE_TRI_DEPTH_CANDIDATE_FLAT_FIRST=480
PASS NATIVE_TRI_DEPTH_CANDIDATE_FLAT_NONFIRST=300
PASS NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST=480
PASS NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST=300
PASS GEOM_SHADER_DRAW_TRI=300 matches NATIVE_TRI_DEPTH_FALLBACK_FLAT_NONFIRST
```

So adding native-quad infrastructure did not perturb the triangle-fill
slice.

## Rainbow Snapshot Smoke Check

Quad-free Rainbow Six 3 snapshot scene with both flags on:

- Run: `benchmark-runs/20260501-110557-rainbow-six-3`
- Flags: `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1`
- Post-load average FPS: 30.97
- Post-load average MSPF: 6.71

Compared to D8 (just `XEMU_NATIVE_TRI_DEPTH=1`,
`benchmark-runs/20260430-113642-rainbow-six-3`, 30.99 FPS / 6.47 MSPF) the
native-quad code is performance-neutral when no quads are present.

## PGR2 Mid-Route Snapshot Triplet

The whole-route PGR2 averages are unstable across runs because slow runs
diverge from the fixed-time input script and end up rendering different
parts of the track. To get an apples-to-apples comparison, a snapshot was
captured at 130 seconds into the recorded gameplay route and then replayed
for 30 seconds with `noop.csv` in three flag configurations.

- Snapshot capture run: `benchmark-runs/20260501-112001-pgr2`
- Snapshot HDD: `benchmark-runs/20260501-112001-pgr2/xbox_hdd.qcow2`
- Snapshot tag: `pgr2_gameplay_b4`

| Config | Run dir | Post-load FPS | GS draws | Native-tri draws | Native-quad draws |
| --- | --- | --- | --- | --- | --- |
| Baseline (no flags) | `benchmark-runs/20260501-115623-pgr2` | 4.39 | 332,066 (329,044 tri + 3,022 quad) | 0 | 0 |
| `XEMU_NATIVE_TRI_DEPTH=1` | `benchmark-runs/20260501-115654-pgr2` | 16.02 | 11,745 (all quad) | 1,264,676 | 0 |
| `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` | `benchmark-runs/20260501-115725-pgr2` | 16.56 | 0 | 1,317,851 | 12,193 |

Counter sanity:

- All native-quad draws are `NATIVE_QUAD_DRAW_LIST` (PRIM_TYPE_QUADS); zero
  PRIM_TYPE_QUAD_STRIP draws in this PGR2 snapshot.
- All native-quad draws are `NATIVE_QUAD_CANDIDATE_SMOOTH` with zero
  fallbacks.
- Native-quad depth-mode split: 2,716 z-perspective + 9,477 linear-z.
- Zero polygon-offset native-quad draws in this scene.

Conclusions from the snapshot triplet:

- The triangle-fill bypass alone produces a 3.6x FPS lift at this
  PGR2 scene (4.39 → 16.02). That matches the existing whole-route
  observation that triangle-family geometry-shader work was the dominant
  remaining renderer cost.
- Adding the quad-family bypass on top is performance-correct but
  comparatively modest at this specific scene: 16.02 → 16.56 (+3.4%). The
  scene only has 12,193 quad draws over 30 seconds, so the absolute
  geometry-shader work removed is small.
- Even with all geometry-shader draws gone, this PGR2 snapshot does not
  reach the 30 FPS gameplay floor (16.56 FPS). The remaining bottleneck is
  not the geometry shader. Likely candidates: i386 TCG emulation, NV2A
  PGRAPH command processing, surface or texture upload bandwidth, or
  fragment-shader work. None of those have been measured yet at this scene.

## Whole-Route Replay Variance

For completeness, the whole-route PGR2 replays were also collected, but
two same-config runs gave 18.20 FPS and 24.81 FPS post-load. That 36%
spread across runs of identical code on identical input demonstrates that
whole-route averages are not currently a reliable comparator.

| Config | Run dir | Post-load FPS | Notes |
| --- | --- | --- | --- |
| `XEMU_NATIVE_TRI_DEPTH=1` (reference) | `benchmark-runs/20260501-104158-pgr2` | 21.40 | from earlier session |
| `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` (run 1) | `benchmark-runs/20260501-105825-pgr2` | 18.20 | full route |
| `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1` (run 2) | `benchmark-runs/20260501-110810-pgr2` | 24.81 | full route, different scene mix |

The variance comes from real-time-paced input running against an emulator
whose host throughput depends on host load and Apple OpenGL warm-up. Use
the snapshot triplet above for any comparison that needs to be trusted.

## Apple OpenGL Startup Crashes

Two pre-perf-interval crashes hit during this session:

- `~/Library/Logs/DiagnosticReports/xemu-2026-05-01-110643.ips`: pid 75358
  crashed at process launch+1s in `glgProcessPixelsWithProcessor` /
  `GLDTextureRec::uploadTextureLevel`.
- `~/Library/Logs/DiagnosticReports/xemu-2026-05-01-111351.ips`: pid 76151
  crashed about 10s into a snapshot-capture run at the same Apple OpenGL
  texture-upload class.

Both match the previously documented Apple OpenGL nondeterministic startup
crash class (Crimson D3, post-tightening Rainbow, flat-tri-depth trace).
Immediate retries succeeded each time. The native-quad code is not
implicated because the crashes occurred before any geometry was issued by
the changed dispatch paths.

## Next Implementation Slice

The geometry-shader removal track has now eliminated all triangle-family
and all smooth-fill quad-family geometry-shader draws in PGR2, Rainbow, and
Crimson. The next bottleneck for PGR2 to reach 30 FPS in gameplay is no
longer geometry-shader work. Suggested next steps in order:

1. Use Instruments / `XEMU_PERF_LOG=1` counters to identify the dominant
   remaining cost at the PGR2 mid-route snapshot. The strongest candidates
   are i386 TCG, surface/texture upload, and fragment shader work.
2. Add a flat-quad bypass slice if PGR2 or another title shows nontrivial
   `NATIVE_QUAD_CANDIDATE_FLAT` once a flat-shading test scene is created.
3. Add a polygon-line / polygon-point quad bypass slice only after a title
   exercises that combination.
