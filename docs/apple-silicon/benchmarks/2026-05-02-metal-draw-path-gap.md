# 2026-05-02 — Metal renderer draw-path gap discovered during paired-bench setup

## Summary

While preparing the M15 paired Metal-vs-GL validation runs (per
`metal-renderer-plan.md` §4 M15) for PGR2, the **Metal renderer was
empirically observed to issue zero geometry draws** for actual game
content. `flush_draw` short-circuits for every draw path PGR2 (and
similarly Crimson Skies / Rainbow Six 3) actually uses. The renderer
ships, opens its window, clears the surface, but never encodes any
NV2A geometry — `METAL_DRAW_COUNT == 0` and `METAL_PRESENTS == 0`
throughout a 180 s scripted-gameplay run.

This is a **functional gap, not a perf gap**. M15's default-on
criteria (≥ console-native FPS, ≤ 1 % per-pixel diff vs GL, p99
jitter ≥ 20 % improvement, < 5 s cold-launch compile) cannot be
evaluated until the renderer renders.

## How it was found

1. Build verified at HEAD `ad6afbe8b7` (Metal renderer commit). M5
   shader-validation harness 7/7 PASS;
   `validate-native-tri-depth.sh --run 22` 7/7 PASS — both regression
   gates green, so the Metal infrastructure builds clean.
2. PGR2 GL baseline run captured at `benchmark-runs/20260502-225430-pgr2`
   (180 s, scripted `pgr2-gameplay.csv`, profile-prep HDD scratch
   copy, no loadvm — loadvm was attempted earlier and Apple's GLG
   image-upload path crashed during snapshot restore, see crash
   report `xemu-2026-05-02-225017.ips`; the no-loadvm + scripted-input
   approach avoids the GL-on-Metal driver crash and produces a clean
   reference). Result: `avg_fps=30.92, post_load_avg_mspf=13.71,
   p99 mspf_max=45.03 ms, stutter_intervals_30fps=63/163`.
3. PGR2 Metal A/B run captured at `benchmark-runs/20260502-225801-pgr2`
   (same 180 s, same script, same scratch copy, only difference:
   `XEMU_RENDERER=METAL` plus `XEMU_METAL_TRANSLATED_PIPELINE=1`).
   Result: `intervals=126, avg_fps=33.53, post_load_avg_mspf=8.33`,
   but the relevant Metal counters were:

```
METAL_DRAW_COUNT=0
METAL_DRAW_INDEXED_COUNT=0
METAL_PIPELINE_KEY_BUILT=0
METAL_PIPELINE_HITS=0
METAL_PIPELINE_MISSES=0
METAL_PIPELINE_TRANSLATED_OK=0
METAL_DRAW_TRANSLATED=0
METAL_TEX_UPLOADS_TOTAL=0
METAL_SHADER_CACHE_LOADS=0
METAL_SHADER_CACHE_HITS=0
METAL_SHADER_CACHE_MISSES=0
METAL_PRESENTS=0
METAL_PRESENT_JITTER_US_AVG=0
METAL_VERTEX_US_TOTAL=16          (single frame's counter sample)
METAL_FRAGMENT_US_TOTAL=27        (single frame's counter sample)
METAL_PRESENT_GPU_FRAMES=1        (single sampled present pass total)
METAL_CLEAR_COUNT=25519           (clears DO fire — ~205 / s)
BEGIN_ENDS=7578294                (NV2A guest pushed 42 k draw boundaries / s)
NV2A_PRESENT_HEARTBEAT (per-int)=31  (guest believes it is presenting at 30 Hz)
```

   The "higher" Metal FPS (33.5 vs 30.9) is an artifact of doing
   essentially no GPU work — the Metal renderer is a clear-only
   no-op for geometry; the timer keeps ticking against an empty
   pipeline. The xemu window is **black** for the duration of the
   run (confirmed via `screencapture -x` of the live session: black
   xemu window with the macOS Screen-Recording permission dialog
   layered on top because earlier macos-capture runs prompted for
   screen-recording access; permission state is irrelevant to the
   black-screen finding because the underlying NV2A guest IS pushing
   geometry — the gap is on the renderer side).

## Root cause

`hw/xbox/nv2a/pgraph/mtl/renderer.c::pgraph_mtl_flush_draw` only
implements the `inline_buffer` path:

```c
/* renderer.c:456-468 */
/* Only the inline_buffer path is implemented through M4 ... */
if (pg->draw_arrays_length || pg->inline_elements_length ||
    pg->inline_array_length) {
    return;
}
if (pg->inline_buffer_length == 0) {
    return;
}
```

PGR2's GL-side breakdown shows `INLINE_ELEMENTS ≈ 67 489 / s`,
`INLINE_BUFFERS ≈ 1 493 / s`, `DRAW_ARRAYS ≈ 389 / s` (per the
20260502-152149-pgr2 reference run before the M-cycle landed). The
Metal renderer therefore short-circuits ~98 % of the title's draw
calls. The remaining ~2 % `inline_buffer` calls also do not surface
because the M3/M4 inline_buffer extractor expects the inline buffer
to be non-empty at flush_draw time, and PGR2's combiner-driven flow
keeps it empty between BEGIN/END pairs.

What did get exercised:

- **Surface manager + clear path**: 25 519 clears across the run
  (~205 / s). Each is its own command buffer + render pass + commit
  per `surface.mm:437-446`.
- **Present compositor + Metal HUD path**: opens, but never receives
  an NV2A framebuffer surface to present (because no draws ever
  populated one), so `METAL_PRESENTS=0`.

## Why the M-cycle close-out missed this

The M5 / M6 / M7 / M7.1 slices each declared "SHIPPED" and they each
*did* land their listed deliverables (shader translator + cache;
texture upload infra; combiner pipeline state-to-key; translated
encode swap). What none of them did is wire the Vulkan-renderer
`flush_draw` `draw_arrays` / `inline_elements` / `inline_array`
branches into Metal — the comment at `renderer.c:456` says explicitly
"land with M5+ when the format-resolving / aligned-vertex-buffer
remap logic ports from `vk/draw.c`," but no M5+ slice actually did
the port. The plan-text M5 exit gate was the GLSL→MSL shader-
validation harness, not a per-game visual diff that would have caught
the missing draw paths. The native-tri-depth regression gate exercises
the GL renderer (it's the GL flat-tri-depth XBE counter split), so it
also passes without checking Metal output.

The M-cycle declared M14 closed and queued M15 for "user-driven
validation". The first such validation attempt is this discovery.

## Impact on stated goals

The user-stated goals for this performance pass (recorded at session
start: "1080p at 30/60 fps with high quality antialiasing, low
controller input lag, no framerate jitter, with our new Metal
backend") presume Metal renders. Until the missing draw paths land,
none of the Metal-side perf goals are reachable. Specifically:

- 30 / 60 FPS at 1080p — N/A, Metal renders no geometry.
- High-quality AA via `XEMU_METAL_MSAA=4` — N/A, no draws to AA.
- Low input-to-photon latency via `presentDrawable:atTime:` — N/A,
  `METAL_PRESENTS=0`.
- Frame jitter via M10 atTime: pacing — N/A, no presents to time.

GL-side, the user goals are **partially** met today:
- 30 FPS at `surface_scale=2` (≈1080p): met for PGR2 / Crimson /
  Rainbow / SC2 (SC2 sustains 60 Hz on the same build). Confirmed
  in `2026-05-02-tcg-30fps-cap-attribution.md` and the V9+V10
  attribution run.
- 4× MSAA on GL: shipped as `XEMU_GL_MSAA={0,2,4,8}` opt-in.
- Jitter: V9 + V10 closed the small / moderate stutter classes;
  the 1.3 s class Crimson stutter is declared guest-intrinsic
  ("best effort complete" 2026-05-02). p99 mspf ≈ 45 ms remains on
  PGR2 — see "Implications" below for what's still actionable.

## Implications & path forward (recommended)

The realistic next steps fall into two tracks. They are not
mutually exclusive but they have different effort profiles.

### Track 1 — Close the Metal draw-path gap

This is the work the M-cycle declared "shipped" but did not land.
The structural reference is `hw/xbox/nv2a/pgraph/vk/draw.c
::pgraph_vk_flush_draw` (lines 2021-2200), which dispatches all four
NV2A submission paths through one shared vertex-attribute remapping
pipeline (`pgraph_vk_bind_vertex_attributes`,
`copy_remapped_attributes_to_inline_buffer`,
`sync_vertex_ram_buffer`, `remap_unaligned_attributes`).

The Metal port needs equivalents of:
- **`pgraph_vk_bind_vertex_attributes`** (vk/vertex.c) — walks
  pg->vertex_attributes, decodes per-attribute format/stride/offset,
  pulls bytes from VRAM, converts to a unified Float4 inline buffer.
- **`copy_remapped_attributes_to_inline_buffer`** + the
  `VertexBufferRemap` machinery that handles unaligned strides.
- **`sync_vertex_ram_buffer`** — page-tracks the relevant VRAM
  ranges so re-uploads happen only on dirty reads.
- A Metal-side index buffer staging path. The vertex-attribute
  remap helpers are not in shared `pgraph/`; they live in `vk/` and
  `gl/` (with `gl/`'s version structurally simpler because GL can
  bind raw VRAM mmap'd via `BUFFER_VERTEX_RAM`). The Metal port
  most closely mirrors Vulkan's, since both need an explicit
  CPU→GPU staging round-trip.

Effort estimate: substantial. The Vulkan implementation of these
helpers is ~1 500 LOC across `vertex.c` + `buffer.c` + the
`flush_draw` dispatch. A minimum-viable Metal port that supports
PGR2 / Crimson / Rainbow gameplay (Float4 position + Float4 color
only, no normals / texcoords / fog UBO) is on the order of 600-800
LOC of new Metal-side code. The full port (textures, fog, multi-
stage combiner UBOs, all attribute formats) is larger and overlaps
with M6 Part B + M7.1's deferred items.

The full deliverable is the M5+ port the original plan called for;
splitting it into "M5.5 — minimum viable draw paths" and "M5.6 —
full attribute / texture / combiner parity with GL" gives a
sequenceable plan.

### Track 2 — Drive the user-stated goals on the GL backend in parallel

GL already gets 30 FPS at `surface_scale=2` for the tracked titles
and ships MSAA. The user-stated goals "no framerate jitter" and
"low controller input lag" still have headroom on GL:

- **PGR2 p99 mspf 45 ms** in this run (today's bench) — GL still
  has tail jitter. The V9 + V10 work closed sub-100 ms classes;
  the remaining 30-50 ms tail likely has renderer-side and TCG-
  scheduling components. Not yet attributed.
- **Audio listen-test for `XEMU_APU_LOCK_RELEASE`** — still
  unblocked per the project state; orthogonal to the renderer
  debate; required before that slice flips fully.
- **Input-latency track (N1-N6)**: GameController.framework
  migration (`macos-input-research.md`) is independent of the
  renderer track and lowers input latency on either backend.
- **`XEMU_GL_RATE_SLEW` default-on** — the M10 prerequisite ships
  default-off because no paired benchmark has yet validated the
  jitter improvement on GL. A 2-3 run paired benchmark would close
  that decision.

GL-side jitter / latency / rate-slew work composes with eventual
Metal default-on (Track 1 unblocks Metal; Track 2's wins come along
when Metal lands).

## Concrete next-session actions

To make the Metal renderer functional, in order:

1. **M5.5 (proposed) — minimum viable draw paths in Metal.** Port
   `pgraph_vk_bind_vertex_attributes` + `sync_vertex_ram_buffer`
   + the four `flush_draw` branches into `mtl/draw.mm` /
   `mtl/buffer.mm`. Float4 position + Float4 color only. Visible
   exit gate: PGR2 / Crimson / Rainbow render *something* (geometry
   in the right place, even if untextured / wrong colors). Counter
   exit gate: `METAL_DRAW_INDEXED_COUNT > 0` per-interval > 0,
   `METAL_PRESENTS > 0`.

2. **M5.6 — vertex-attribute completeness.** Wire normals,
   texcoords, fog, secondary color attributes through the same
   inline-Float4 remap. Then route through M7.1 translated pipeline
   path so combiner / fog / texture state takes effect.

3. **M6 Part B completion** (already deferred) — full S3TC / 3D /
   cube / palette texture lifecycle so M5.6 has real textures to
   sample.

4. **Repeat the paired Metal-vs-GL benchmark.** Same scripted route,
   same HDD scratch copy, both renderers. Apply the M15 default-on
   criteria.

Before any of that, the existing project documentation (handoff,
metal-renderer-plan, decision-log) should be reconciled to reflect
that M-cycle close-out missed the draw-path port.

## Files referenced

- `hw/xbox/nv2a/pgraph/mtl/renderer.c:456-468` — the early-return
  block that drops all non-inline_buffer draws.
- `hw/xbox/nv2a/pgraph/mtl/draw.mm:262-310, 359-409` — per-draw
  command-buffer + render-pass open/close pattern (the eventual
  coalescing target, but a downstream concern relative to the
  draw-path gap).
- `hw/xbox/nv2a/pgraph/vk/draw.c:2021-2200` — Vulkan reference for
  the dispatch.
- `hw/xbox/nv2a/pgraph/vk/draw.c:1559, 1974, 1775` — vertex-attribute
  helpers.
- `benchmark-runs/20260502-225430-pgr2/` — GL paired baseline.
- `benchmark-runs/20260502-225801-pgr2/` — Metal paired result
  (zero geometry).
- `~/Library/Logs/DiagnosticReports/xemu-2026-05-02-225017.ips` —
  Apple GLG crash captured during the loadvm-based attempt that was
  abandoned in favor of scripted-gameplay paired runs.

## Honest assessment

I cannot complete an autonomous "achieve performance goals on the
Metal backend" pass in this session because the renderer does not
render. The data-driven path (project rule #1) requires Metal to
produce frames before optimizing them. Implementing the draw-path
port (Track 1) is the correct work and is what the M-cycle originally
called for — but it is multi-session engineering, not a perf
optimization sweep. Track 2 (GL jitter / input / rate-slew) is
ready to execute today and matches the user's "no jitter, low input
lag" sub-goals.

I am pausing further benchmark work and surfacing this finding so
that the next-session priority — Track 1 vs. Track 2 split — can
be set explicitly rather than implicitly through "keep iterating".
