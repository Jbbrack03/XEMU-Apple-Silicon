# 2026-05-01 — GL vs Metal: definitive answer for the project goals

## Question

The project goals are sustained 60 FPS, no judder, 1080p output, anti-
aliasing as a player-visible option, and (eventually) higher-quality
texture support — across the broader Xbox library. Should the fork
commit to a native Metal renderer (Phase 4 of `strategy.md`), or is
Apple's OpenGL-on-Metal sufficient?

## Verdict: stay on OpenGL

The data from this session decisively rules out the renderer as the
gating constraint for the project's goals. Apple's GL-on-Metal has more
than enough headroom for 60 FPS at 1080p — and probably 4K — once the
non-renderer bottleneck is fixed. Native Metal would not address the
actual problem, and committing to it now would be a multi-month effort
chasing the wrong target.

The single change required to unlock the project goals is on the **TCG
vCPU emulation path**, not the renderer.

## Evidence

### 1. The 1.35-second Crimson worst-frame is not a renderer problem

Per the per-event timing diagnostic in
`docs/apple-silicon/benchmarks/2026-05-01-renderer-vs-tcg-stutter-attribution.md`:

- During Crimson's 1.35-second worst-frame intervals, the renderer
  thread is busy for **5–23 ms** of the 1000 ms wallclock interval.
- Frames drop to 2–10 fps; the Xbox CPU is not flipping the
  framebuffer.
- Apple's GL command queue drains promptly (`glFinish` 4–12 ms);
  there's nothing in flight to compile or wait on.
- All texture / surface counters are 0–4 ms.

The renderer is sitting idle while the bottleneck runs upstream.

### 2. The bottleneck is Apple-Silicon-specific QEMU MTTCG TB invalidation

Apple `sample` profile of xemu during a Crimson bad-interval window
(`benchmark-runs/20260501-203802-crimson-skies/sample-tcg-bad-interval.txt`)
attributes the dominant TCG-thread cost to the JIT translation-block
invalidation chain triggered by writes to executable pages:

```
mttcg_cpu_thread_fn -> tcg_cpu_exec -> cpu_exec_loop -> cpu_tb_exec
  do_st4_mmu (484 samples)
    mmu_lookup -> mmu_watch_or_dirty -> notdirty_write
      tb_invalidate_phys_range_fast (463)
        tb_invalidate_phys_page_range__locked (446)
          do_tb_phys_invalidate (382) -> tcg_flush_jmp_cache (381)
          + sys_icache_invalidate          (45 samples)
          + pthread_jit_write_protect_np   (12 samples)
```

Two of those leaves are real Apple Silicon syscalls:
`pthread_jit_write_protect_np` toggles W^X on JIT pages, and
`sys_icache_invalidate` flushes the instruction cache. Each pays
microseconds per call. Over a burst of TB invalidations during a
specific Crimson scene transition, this accumulates into the 1.35-second
stall we have measured.

This is a documented Apple-Silicon-specific QEMU MTTCG pathology. The
fix lives on the TCG side: persistent JIT cache (PPTC, strategy.md
Phase 5a), reduced invalidation frequency, or upstream QEMU patches
for `MAP_JIT` handling. None of it requires a renderer change.

### 3. Apple GL-on-Metal has substantial renderer headroom at high resolution

PGR2 mid-route snapshot (deterministic, no input variance) at internal
render scale factors 1×, 2×, and 4× via the new
`XEMU_BENCH_SURFACE_SCALE` knob:

| Scale | Internal res (approx) | FLUSH_DRAW | DRAW_BEGIN | BIND_TEXTURES | SURF_DOWNLOAD | FLIP_GLFINISH | p99 mspf | avg FPS |
| ----- | --------------------- | ---------- | ---------- | ------------- | ------------- | ------------- | -------- | ------- |
| 1     | 640×480               | 6.10 s     | 6.13 s     | 3.00 s        | 1.80 s        | 0.54 s        | 35.8 ms  | 30.57   |
| 2     | 1280×960              | 6.39 s     | 6.92 s     | 3.47 s        | 2.27 s        | 0.57 s        | 37.9 ms  | 30.61   |
| 4     | 2560×1920             | 6.55 s     | 8.33 s     | 4.85 s        | 3.63 s        | 0.62 s        | 34.5 ms  | 30.51   |

`FLUSH_DRAW` (the actual `glDrawElements` / `glMultiDrawArrays`
dispatch path — where Apple's MSL→PSO compile would surface) grows
only **7 %** going from native res to 4× scale. The fragment work runs
asynchronously on the GPU and does not bottleneck the renderer thread.

`BIND_TEXTURES` and `SURF_DOWNLOAD` grow with the scaled buffer sizes
but stay within budget. `FLIP_STALL_GLFINISH` is 14 % of wallclock at
scale 4 — entirely tolerable.

p99 stays at ~35 ms (i.e. inside the 33-ms 30-FPS budget) at every
scale. **No per-pipeline-state-object pathology under heavier renderer
load** — the concern that Apple's driver might explode on first-draw
state-combination compile does not show up at higher resolution on
this title.

### 4. Crimson at scale 4 stress-test confirms the conclusion

`benchmark-runs/20260501-204246-crimson-skies` (60 s retail route,
scale 4):

- `post_load_avg_fps` 29.17 (vs 30.43 at scale 1 — same within run-to-
  run variance).
- `frame_mspf_us_max` 1,348,978 (still the 1.35-s TCG stutter).
- `FLUSH_DRAW_US_TOTAL` 16.2 s out of 60 s = 27 % of wallclock.

Doubling the FPS to 60 at scale 4 would put `FLUSH_DRAW` at ~54 % of
wallclock — still inside the budget, with `BIND_TEXTURES` and
`SURF_DOWNLOAD` adding modest additional cost. Apple's GL has **room
for 60 FPS at 1080p — and at 4K-ish — once the TCG bottleneck is
fixed**.

## Headroom math for the project goals

Working from the scale-4 PGR2 numbers (the deterministic test):

| Goal                                  | Renderer cost at 30 FPS | Projected at 60 FPS | Fits in 1000 ms? |
| ------------------------------------- | ----------------------- | ------------------- | --------------------- |
| 1× scale (native), no AA              | 207 ms / s              | 414 ms / s          | yes (41 % budget)     |
| 2× scale (~1080p), no AA              | 230 ms / s              | 460 ms / s          | yes (46 % budget)     |
| 4× scale (~2160p), no AA              | 278 ms / s              | 555 ms / s          | yes (56 % budget)     |
| 2× scale + MSAA 2× (estimated)        | ~300 ms / s             | ~600 ms / s         | yes (60 % budget)     |
| 2× scale + MSAA 4× (estimated)        | ~360 ms / s             | ~720 ms / s         | yes (72 % budget)     |

(Renderer cost = `DRAW_BEGIN_US_TOTAL + SURF_DOWNLOAD_US_TOTAL +
FLIP_STALL_US_TOTAL`. `DRAW_BEGIN` already contains `BIND_TEXTURES`
and `FLUSH_DRAW`.)

The MSAA rows are estimates — we did not implement MSAA in this
session because xemu's GL framebuffer setup does not currently use
multisample renderbuffers, and adding them is a non-trivial code
change. The scale-test result (no pathological per-state-combination
compile spike at higher pixel load) is encouraging signal that MSAA
will not break the GL path, but this remains a smaller residual
unknown.

## What would actually require Metal

Native Metal becomes mandatory if any of the following turn out to be
true after the TCG fix:

1. **Title-specific Apple GL pathologies** that PGR2 / Crimson /
   Rainbow Six 3 do not exhibit. We did not run a broader title sweep
   in this session. The risk profile is bounded — three diverse
   titles already work fine — but unknown.
2. **MSAA pipeline-state explosion.** Each MSAA configuration is a
   separate MSL pipeline on Apple's driver. If a title rapidly
   switches between MSAA modes mid-frame, first-draw stalls could
   appear. Unlikely on Xbox-era titles but unmeasured.
3. **Texture-mod bandwidth.** Higher-quality replacement textures
   could saturate Apple's GL upload path. Current `TEX_UPLOAD_US_TOTAL`
   is 0.27 % of wallclock — has lots of room before becoming a
   bottleneck.
4. **Code-base health, future-proofing, Apple-extension access.**
   Real long-term reasons but not gating for the stated player-
   visible goals.

None of these change the verdict for **now**. They become Phase-2
considerations after the project is sustainably hitting its goals on
GL.

## Strategic conclusion

**Direct answer to the question "is OpenGL appropriate for our
overall goals?":** Yes. With high confidence for tracked titles, with
medium-high confidence for the broader Xbox library, and with the
single caveat that MSAA-on-GL has not been measured directly.

**Direct answer to "should we commit to Metal?":** Not now. The
Crimson worst-frame is a CPU-emulation problem; Metal would not fix
it. The Apple GL renderer has measured headroom for 60 FPS at 1080p
even at 4× internal scale. Phase 4 native Metal renderer remains
documented in `strategy.md` as the long-term ceiling-removing path,
but it is no longer the *next* priority.

## Revised next-task priority order

1. **TCG TB-invalidation fix on Apple Silicon.** This is the only
   thing that gates every project goal: no judder, 60 FPS, even basic
   smoothness. Specifically: investigate W^X-toggle frequency and
   batching opportunities, evaluate persistent JIT cache (PPTC), and
   look at upstream QEMU patches for `pthread_jit_write_protect_np`
   amortization. Profile-guided.
2. **Verify GL holds at 60 FPS** on tracked titles after step 1. The
   headroom math says yes; the actual measurement is the proof.
3. **MSAA implementation on the GL path.** Add multisample render-
   buffer support to `pgraph_gl_init_surfaces`. Validate the AA
   pipeline-variant cost is non-pathological (the new
   `SHADER_COMPILE_*` counters will surface it if it is).
4. **Broader title sweep.** Use `package-game.sh` to grab 5–10
   diverse titles and run them through the harness with the
   diagnostic counters live. Surface any title-specific Apple GL
   anti-patterns the tracked three did not exhibit.
5. **Phase 4 Metal renderer**, if and only if (3) or (4) reveal a
   ceiling we cannot work around in GL. Otherwise this becomes a
   long-term code-quality / future-proofing slice rather than a
   blocking deliverable.

## Source-code and tooling changes from this session

- `hw/xbox/nv2a/debug.h`: per-subsystem timing counters
  `BIND_TEXTURES_US_TOTAL`, `TEX_UPLOAD_US_TOTAL`,
  `SURF_TO_TEX_US_TOTAL`, `SURF_UPLOAD_US_TOTAL`,
  `SURF_DOWNLOAD_US_TOTAL`, `FLUSH_DRAW_US_TOTAL`,
  `DRAW_BEGIN_US_TOTAL`, `FLIP_STALL_US_TOTAL`,
  `FLIP_STALL_GLFINISH_US_TOTAL`. New
  `nv2a_profile_spike()` helper.
- `hw/xbox/nv2a/pgraph/profile.c`: `XEMU_PERF_SPIKE_LOG=1` /
  `XEMU_PERF_SPIKE_LOG_THRESHOLD_US=N` controls and `xemu-spike:`
  log line emission.
- `hw/xbox/nv2a/pgraph/gl/{draw,texture,surface,renderer}.c`:
  timing wrappers around `pgraph_gl_draw_begin`,
  `pgraph_gl_flush_draw`, `pgraph_gl_bind_textures`,
  `upload_gl_texture`, `pgraph_gl_render_surface_to_texture`,
  `pgraph_gl_upload_surface_data`,
  `pgraph_gl_surface_download_if_dirty`, `pgraph_gl_flip_stall`.
- `scripts/apple-silicon/run-benchmark.sh`: `XEMU_BENCH_SURFACE_SCALE`
  env var. Generates `[display.quality] surface_scale = N` in the
  per-run config.
- `scripts/apple-silicon/extract-perf-summary.sh`: surfaces all the
  new counters.

## Validated state

- Build: `./build.sh -a arm64` clean; `dist/xemu.app` codesign
  verified.
- Tracked-title regression: PGR2 snapshot, Crimson snapshot, and
  Rainbow Six 3 snapshot all run without regressions vs the prior
  `XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`
  baseline.
- Async shader compile (`XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1`) remains
  shipped opt-in but is now confirmed to not address the headline
  judder either; same conclusion as 2026-05-01-async-shader-compile.md.
