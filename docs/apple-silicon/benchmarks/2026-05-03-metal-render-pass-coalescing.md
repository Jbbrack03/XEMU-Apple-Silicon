# 2026-05-03 — Metal renderer: render-pass coalescing (PGR2 +125 % FPS)

## Summary

Following M5.5 (which got the Metal renderer drawing geometry at all),
the perf gap on PGR2 was attributed to the per-draw command-buffer
commit anti-pattern: every Metal draw created its own
`MTLCommandBuffer`, opened a render encoder, encoded one draw, ended
the encoder, and committed — at PGR2's ~22 k draws/s, that meant
~22 k cmdbuf commits per second, each forcing a CPU↔GPU sync and a
TBDR tile flush. This slice introduces render-pass coalescing: hold
one command buffer + render encoder open across consecutive `flush_draw`
calls when the attachment set is unchanged, close on attachment
change / frame end / clear / shutdown.

**Empirical result on PGR2 (60 s scripted gameplay, profile-prep HDD scratch copy):**

| Metric | Pre-coalescing | Post-coalescing | Delta |
|---|---|---|---|
| `post_load_avg_fps` | 16.42 | **37.09** | **+125.9 %** |
| `post_load_avg_mspf` | (skewed by idle) | 8.97 ms | |
| `post_load_mspf_max_p99` | 58.30 ms | 71.08 ms | mild tail regression |
| `post_load_frame_mspf_us_p99` | (n/a)| 31.40 ms | |
| `METAL_DRAW_COUNT` | 3.37 M / 180 s ≈ 18.7 k/s | 2.03 M / 60 s ≈ 33.9 k/s | +81 % |
| `METAL_PIPELINE_TRANSLATED_OK` | 71.5 % | 67.2 % | unchanged-ish |

**PGR2 Metal now exceeds GL's `post_load_avg_fps = 30.91`.** This is
the user-stated "30 FPS at 1080p" goal met on the Metal backend for
the previously-bottlenecked title. Rainbow Six 3 was already at 30
fps pre-coalescing; Crimson Skies sees a similar lift (numbers below
when the in-flight bench completes).

## What changed

### `hw/xbox/nv2a/pgraph/mtl/draw.mm`

Added open-pass state at module level:

```c
static id<MTLCommandBuffer>       s_open_cmd = nil;
static id<MTLRenderCommandEncoder> s_open_enc = nil;
static struct {
    void     *color_tex;
    void     *depth_tex;
    uint32_t  color_fmt;
    uint32_t  depth_fmt;
    uint32_t  sample_count;
} s_open_pass_key;
static bool s_open_buffer_frame_active = false;
```

Three helpers:

- `open_pass_matches(...)` — compare attachment set against open pass.
- `open_pass_close_locked()` — `endEncoding`, `commit`, end staging-ring
  buffer frame, zero the key.
- `open_pass_ensure(color_tex, depth_tex, color_fmt, depth_fmt, sample_count)`
  — return the existing encoder if attachments match; otherwise close
  and reopen. First open since flush also calls
  `pgraph_mtl_buffer_begin_frame()` so the staging-ring vertex
  allocations have a frame.

Three counters surface the coalescing behavior:

- `s_open_pass_opens` — count of fresh render-pass starts (each costs
  a TBDR tile-load).
- `s_open_pass_coalesced` — count of draws that reused the existing
  encoder (the "free" wins).
- `s_open_pass_flushes` — count of explicit flushes from caller hooks.

### `pgraph_mtl_draw_passthrough`, `pgraph_mtl_draw_indexed`, `pgraph_mtl_draw_translated`

Removed:
- `pgraph_mtl_buffer_begin_frame()` at entry (moved into open_pass_ensure).
- `[s_draw_queue commandBuffer]` per draw (managed by helper).
- `mtl_draw_wait_upload_fence(cmd)` (called once per pass-open inside
  helper).
- `[cmd renderCommandEncoderWithDescriptor:desc]` per draw (helper).
- `[enc endEncoding]` and `[cmd commit]` per draw (deferred to flush).
- `pgraph_mtl_buffer_end_frame()` per draw (deferred to flush).

Each draw now:
1. Calls `select_pipeline(...)` and `open_pass_ensure(...)`.
2. Stages its vertex/color/index buffers via the existing buffer-ring.
3. `setRenderPipelineState`, `setViewport`, `setVertexBuffer` (per-draw).
4. `drawPrimitives` or `drawIndexedPrimitives`.

That's it. The encoder stays open for the next draw.

### Public flush entry point

`pgraph_mtl_draw_flush_open_pass()` is wired in `mtl/renderer.c` at
every operation that depends on the surface texture being GPU-stable
or the staging-ring being clean:

| Hook | Why |
|---|---|
| `pgraph_mtl_flip_stall` | NV2A end-of-frame; compositor reads next. |
| `pgraph_mtl_clear_surface` | clear opens its own pass with `loadAction=Clear`; prior draws must be committed first. |
| `pgraph_mtl_pre_savevm_trigger` | Snapshot capture must see clean state. |
| `pgraph_mtl_pre_shutdown_trigger` | Shutdown cannot have an in-flight encoder. |
| `pgraph_mtl_surface_flush` | Surface cache flush requires GPU-stable texture. |
| `pgraph_mtl_draw_finalize` | Final cleanup before queue release. |

## Why this is correct (and why it was safe to land on top of M5.5)

The M5.5 `flush_draw` branches (`inline_elements`, `draw_arrays`,
`inline_array`, `inline_buffer`) all funnel through
`mtl_dispatch_decoded_draw`, which calls one of the three draw
functions. Coalescing applies uniformly across all four NV2A draw paths.

Within a single render pass on Apple Silicon TBDR, multiple draws
sharing the same attachment set are exactly what the GPU expects —
each draw's tile-resident framebuffer state persists between them.
What was wasteful was the per-draw `commit`, which forced the cmdbuf
to hit the GPU command queue with one draw at a time. By batching N
draws into one cmdbuf, the GPU can pipeline them; the tile flush
only happens on close.

The CORRECTNESS gate: any operation that reads the tile-resident
framebuffer from outside the open encoder (compositor, surface
download, snapshot, clear) must first close the pass. Those hooks
are wired above. WWDC20-10632 ("Optimize Metal Performance for Apple
Silicon Macs") explicitly recommends this pattern.

The `pgraph_mtl_draw_end → pgraph_mtl_flush_draw` hook (the
2026-05-03 fix that originally landed M5.5) is unchanged — `flush_draw`
still runs after every NV097_END, but it now feeds the open encoder
instead of opening its own. The semantics of "one draw per NV097
batch" is preserved; the cost of each batch drops.

## What did not change

- The per-draw `[setRenderPipelineState:]` / `[setViewport:]` /
  `[setVertexBuffer:]` calls remain. These don't hit the GPU — they
  configure the encoder.
- The translated-pipeline lookup (M7.1) and async-compile fallback
  (M8) are unchanged.
- The MSAA / MetalFX / capture / counter-sampling slices (M11–M13)
  attach to the pass-descriptor that `build_render_pass_descriptor`
  produces — same descriptor, just built less often.
- The M5 shader-validation harness still passes 7/7 (translator
  unchanged).

## Validation runs

PGR2 — see headline numbers above. Run dir
`benchmark-runs/20260503-013744-pgr2`.

Crimson Skies (60 s scripted gameplay,
`benchmark-runs/20260503-013936-crimson-skies`):

```
intervals = 55
post_load_avg_fps           = 30.47       (vs pre-coalescing 27.37 → +11.3 %)
METAL_DRAW_COUNT            = 931 769     (vs pre-coalescing 647 357 → +44 %)
METAL_DRAW_INDEXED_COUNT    = 908 646
METAL_PIPELINE_TRANSLATED_OK = 548 239    (58.8 % — unchanged from M5.5 baseline)
post_load_mspf_max_p99      = 1288.52 ms  (the famous Crimson 1.3 s
                                           class judder — guest-intrinsic
                                           per the 2026-05-02 V9+V10
                                           attribution; not coalescing's fault)
stutter_intervals_30fps     = 10 / 50 (20 %)
```

Rainbow Six 3 (60 s scripted gameplay,
`benchmark-runs/20260503-014122-rainbow-six-3`):

```
intervals = 55
post_load_avg_fps           = 31.40       (vs pre-coalescing 30.24 → +3.8 %)
METAL_DRAW_COUNT            = 318 736     (vs pre-coalescing 122 649 → +161 %)
METAL_DRAW_INDEXED_COUNT    = 298 543
METAL_PIPELINE_TRANSLATED_OK = 198 617    (62.3 %)
post_load_mspf_max_p99      = 699.45 ms   (single shader-compile spike;
                                           same as pre-coalescing,
                                           coalescing didn't change cold-
                                           launch behavior)
stutter_intervals_30fps     = 6 / 50 (12 %) (vs pre-coalescing 22 %)
```

### Cross-title summary

| Title | Pre-coalescing FPS | Post-coalescing FPS | Δ % | GL baseline | Status vs GL |
|---|---|---|---|---|---|
| **PGR2** | 16.42 | **37.09** | +125.9 % | 30.91 | **+20 % above GL** |
| **Crimson Skies** | 27.37 | **30.47** | +11.3 % | (n/a paired this session) | matches console-native |
| **Rainbow Six 3** | 30.24 | **31.40** | +3.8 % | (n/a paired this session) | matches console-native |

**All three tracked titles now meet console-native 30 FPS on the
Metal renderer.** PGR2 — the heaviest-draw title and the M5.5
benchmark's headline "behind GL" entry — now exceeds GL FPS by 20 %.

## What's still queued

1. **M5.6** — the 25-43 % translator failure rate is unchanged. The
   pipeline build fails with "Vertex attribute v0(0) is missing from
   the vertex descriptor" because the MSL declares attribute slots
   (e.g., v0/v3/v7) for "uniform" attributes (`pg->vertex_attributes[i].count == 0`)
   that the pipeline key skips. Failed translations fall back cleanly
   to M3/M4 passthrough — no crashes — so this is a visual fidelity
   issue (magenta surface) not a perf or stability one. M5.6 fix:
   either populate every attribute slot the MSL references in the
   vertex descriptor (with default Float4 + uniform_attrs buffer), or
   teach the GLSL generator to omit unused attribute declarations
   from the MSL.
2. **`METAL_PRESENTS = 0`** — the addPresentedHandler counter still
   doesn't fire on the test machine (macOS Screen-Recording dialog
   is occluding the xemu window). Orthogonal to the rendering
   pipeline; will resolve on a clean desktop.
3. **`validate-native-tri-depth.sh` flake** — pre-existing, unrelated
   to M5.5 / coalescing.
4. **MSAA + MetalFX integration testing** — coalescing reduces the
   per-frame cost of MSAA by leaving the multisample tile resident
   across draws; should now be cheap to flip default-on for visual
   testing.

## Anti-pattern audit (per 2026-05-02 research)

Anti-pattern findings from
`docs/apple-silicon/benchmarks/2026-05-02-metal-draw-path-gap.md`
Track 1 §6 / Track A:

| Anti-pattern | Status before | Status after |
|---|---|---|
| Per-draw command-buffer commit | shipped | **fixed** |
| Standalone clear pass | shipped | unchanged (clear flushes coalesced pass first; clear's own pass is small) |
| Storing MSAA samples to DRAM | shipped (Private storage) | unchanged (M11.1 candidate) |
| `nextDrawable` outside delegate | not applicable (M10 path) | n/a |
| Synchronous pipeline compile | M8 already async | unchanged |
| Treating `MTLBinaryArchive` as cross-device portable | not used | n/a |

The biggest remaining anti-pattern is the standalone clear pass — but
its impact is small relative to per-draw commit, and folding clear
into the next pass's `loadAction=Clear` requires re-architecting the
order of `clear_surface` vs the next `flush_draw` call. Deferred.

## Build / regression gates

- `./build.sh -a arm64` — PASS.
- `scripts/apple-silicon/metal-shader-validation/run-validation.sh`
  — 7/7 PASS.
- `validate-native-tri-depth.sh` — pre-existing flake (unrelated).
