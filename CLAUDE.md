# xemu-fork — Project Memory (Apple Silicon Performance Fork)

This is the QEMU/xemu source tree for the Apple Silicon performance fork.
Working branch: `apple-silicon-performance`. Upstream remote: `upstream`
(xemu-project/xemu). Origin remote: `origin` (your fork).

For workspace-level rules, test assets, and the goal/scope of this project
see `../CLAUDE.md`. **Do not duplicate that content here** — read it.

## Canonical project documentation

Every Apple Silicon-specific decision, benchmark, and handoff lives under
`docs/apple-silicon/`. Read these before starting work:

- `docs/apple-silicon/handoff.md` — current state, source-code changes made,
  next-session checklist, useful commands. **Read first** every session.
- `docs/apple-silicon/decision-log.md` — append-only decisions with
  rationale and supersession markers.
- `docs/apple-silicon/strategy.md` — phased plan (Phase 0 baseline → Phase
  4 native Metal renderer).
- `docs/apple-silicon/research.md` — evidence base; cites local source
  references and public xemu issues.
- `docs/apple-silicon/benchmarking.md` — measurement matrix and retail
  performance gates.
- `docs/apple-silicon/automation.md` — benchmark harness, scripted-input
  format, perf-counter list, snapshot workflow.
- `docs/apple-silicon/benchmarks/<date>-<name>.md` — dated session notes,
  one per benchmark session. Add a new file for each meaningful run; do not
  edit older notes.
- `docs/apple-silicon/metal-porting-workflow.md` — **(added 2026-05-04)**
  canonical operating playbook for the Metal renderer port. Five-phase
  model (build & boot → translation correctness → visual parity → perf
  parity → default-on), daily loop for the active phase, tools index,
  triage flowchart, phase exit-gate procedures, triangulation appendix.
  Meta-doc that sits one level above the Metal track sub-list below;
  read after `handoff.md` at the start of any Metal-track session.

Metal renderer track (added 2026-05-02; read after `handoff.md` when
the task touches the Metal port):

- `docs/apple-silicon/metal-renderer-plan.md` — staged Metal renderer
  implementation plan, slices M0–M15, validation gates, risk register
  R1–R8, open questions Q1–Q6 (all resolved as of M14).
  **Slices M0–M14 SHIPPED 2026-05-02; M5.x correctness follow-ups
  continue through 2026-05-04; M15 (default-on selection) BLOCKED
  on the front-fb fallback policy, Crimson/SC2 routed visual
  correctness, and the broader paired visual/perf gate.**
  Read this when the
  task touches the Metal port; per-slice "Status (2026-05-02):
  SHIPPED" annotations document the landed implementation.
- `docs/apple-silicon/metal-api-reference.md` — Apple Metal API
  surface reference for the port: device/queue, render pipelines,
  MSL specifics, buffers/textures, MSAA, frame timing, sync, MetalFX,
  capture, GPU family detection, common emulator pitfalls,
  "Recommended Apple Silicon defaults" table.
- `docs/apple-silicon/emulator-metal-survey.md` — file-level findings
  from Dolphin / PCSX2 / DuckStation / MoltenVK Metal backends + xemu's
  Vulkan renderer as the structural template.
- `docs/apple-silicon/macos-input-research.md` — GameController.framework
  migration plan, six input slices N1–N6, independent of the renderer
  track.

## Where the fork's changes live

The fork-specific source-code changes are concentrated in:

- `build.sh` — macOS arm64 build path, CMAKE export for Meson cross-build,
  duplicate-LC_RPATH cleanup before code-sign.
- `hw/xbox/nv2a/pgraph/` — perf logging, geometry-shader attribution
  counters, `XEMU_NATIVE_TRI_DEPTH` triangle-family fill replacement,
  diagnostic toggles.
  - `profile.c`, `pgraph.c`, `pgraph.h`, `debug.h` — counters and perf
    interval logging.
  - `gl/draw.c`, `gl/renderer.h`, `gl/shaders.c` — GL geometry-shader
    attribution and native-triangle-depth dispatch.
  - `glsl/geom.c`, `glsl/geom.h` — eligibility logic for native-tri-depth
    and the `XEMU_DIAG_*` toggles.
  - `glsl/psh.c`, `glsl/psh.h` — fragment shader depth/polygon-slope
    derivation for the native path.
  - `mtl/` — Metal renderer (slices M0–M14 + M5.5 / M5.6 / M5.7 / M5.8 / M5.9).
    Notable per-file scope:
    - `renderer.c` — NV2A-side ops dispatch; `flush_draw` branches
      for inline_buffer / inline_elements / draw_arrays / inline_array
      (M5.5); `draw_end → flush_draw` hook; six flush hooks for the
      M5.7 coalesced pass. **(M5.9, 2026-05-03)** new
      `mtl_bind_current_surfaces(d, color, zeta)` helper computes
      `vram_addr = nv_dma_load(d, pg->dma_color/_zeta).address +
      pg->surface_color/_zeta.offset`, then calls
      `pgraph_mtl_surface_bind_color/_depth(vram_addr, size, w, h,
      pitch, fmt, NULL)`; falls back to legacy `ensure_color/_depth`
      if DMA registers are unset. Used from `clear_surface` and
      `flush_draw`. `pgraph_mtl_flip_stall(d)` does the CRTC-aware
      front-fb publish: `pgraph_mtl_surface_publish_front_fb(
      (uint32_t)(d->pcrtc.start +
      vga_display_params.line_offset), "crtc")`.
      `pgraph_mtl_surface_flush(d)` calls
      `pgraph_mtl_surface_cache_flush()` so a renderer flush /
      reload drops every cached MTLTexture.
    - `draw.mm` — open-pass coalescing (`open_pass_ensure` /
      `open_pass_close_locked` / `pgraph_mtl_draw_flush_open_pass`)
      shared across M3/M4 passthrough + M7.1 translated paths.
    - `vertex.{c,h}` — **(M5.5 → M5.8)** CPU-side per-element NV2A
      vertex-attribute decoder. Public:
      `pgraph_mtl_collect_vertex_streams` (legacy POSITION+DIFFUSE),
      `pgraph_mtl_collect_all_vertex_streams` (M5.8, all 16 slots
      via `MtlAttributeStream` array),
      `pgraph_mtl_free_attribute_streams`,
      `pgraph_mtl_inline_array_vertex_stride`,
      `pgraph_mtl_inline_array_update_offsets`,
      `pgraph_mtl_set_attr_masks` (mirror of vk/vertex.c:148-154 +
      226-236 — only count==0 OR stride==0 slots are uniform),
      `pgraph_mtl_set_attr_masks_inline_buffer` (mirror of
      pgraph_vk_bind_vertex_attributes_inline). Format coverage:
      F / UB_OGL / UB_D3D / S1 / S32K / **CMP** (CMP added in
      M5.8 — signed (11,11,10) packed → CPU-decoded Float4 so the
      shader-side compressed_attrs branch never fires).
      `MTL_ATTR_BUFFER_INDEX_BASE = 1` shifts attribute streams to
      bufferIndex 1..16 so they don't shadow the VSH UBO at
      MSL `[[buffer(0)]]`.
    - `shaders.mm` — pipeline build. M5.8 indexes `vd.layouts` by
      `attr_buffer_index[i]` (not slot) to match the per-attribute
      bufferIndex shift; populates every active vertex-descriptor
      attribute slot (inactive slots are left unset because the
      GLSL generator emits `vec4 vN = inlineValue[k];` for them,
      reading via the VSH UBO).
    - `state.c` — pipeline-key builder; M5.8 emits
      `format = MTL_VFMT_FLOAT4 / stride = 16 /
      buffer_index = MTL_ATTR_BUFFER_INDEX_BASE + slot` for every
      active attribute (the decoder always normalizes to Float4).
      The legacy NV097 format → MTLVertexFormat translator
      (`pgraph_mtl_translate_vertex_format`) is retained but no
      longer called by the descriptor builder; CMP support there is
      now dead code on the Metal-renderer encode path (kept as a
      reference for future direct-fetch experiments).
    - `draw.mm` — encode. M5.8 `pgraph_mtl_draw_translated` takes an
      `MtlAttributeStream[16]` array; binds VSH UBO at vertex
      `atIndex:0` (was 1 since M7.1 — a bug that shadowed position
      bytes) and each non-NULL stream at bufferIndex
      `MTL_ATTR_BUFFER_INDEX_BASE + slot`. PSH UBO stays at
      fragment `atIndex:1`.
    - `surface.{h,mm}` — **(M5.9, 2026-05-03)** per-VRAM surface
      cache. `MtlSurfaceBinding` struct (vram_addr, size, pitch,
      width, height, format, MTLTexture, MSAA companion, last_use_seq,
      next) replaces the M2-era single-slot manager. Singly-linked
      list, cap 16 entries with LRU eviction. New API:
      `pgraph_mtl_surface_bind_color/_depth(vram_addr, size, w, h,
      pitch, fmt, vram_ptr)` (cache-promote a surface keyed by VRAM
      address); `pgraph_mtl_surface_publish_front_fb(vram_addr,
      reason)` (CRTC-aware front-fb publish via `_get_within`
      lookup; bumps `METAL_FRONT_FB_PUBLISHES` and emits
      `xemu-perf: metal_front_fb_publish vram_addr=0x..` line);
      `pgraph_mtl_surface_cache_flush()` (drops every entry, called
      from `surface_flush`); `pgraph_mtl_surface_get_metal_texture_at`
      / `_within` (lookup helpers — port of vk's `_get_at` / `_get_within`
      at `vk/surface.c:697-724`). Legacy `ensure_color/_depth` API
      kept as thin wrappers for the pre-DMA-bind paths.
      **(M5.9-followup-A, 2026-05-03)** GPU-side
      `pgraph_mtl_surface_blit_copy(src_vram_addr, dst_vram_addr,
      src_x, src_y, dst_x, dst_y, w, h)` for NV097_IMAGE_BLIT (Path
      A: matching-format rect copy via MTLBlitCommandEncoder; Path
      B: format mismatch invalidates dst entry; Path C: nothing in
      cache, defer to bind-time upload). Counter
      `METAL_IMAGE_BLITS`. PGR2 doesn't issue this op (verified 0
      per interval); plumbing intact for titles that do.
      **(M5.9-followup-B+C, 2026-05-03 — SHIPPED, visual gate NOT
      met)** New struct fields: `_Atomic(uint32_t) dirty_vram`
      (set by access callback), `void *access_cb` (opaque
      MemAccessCallback*), `guest_width`/`guest_height` (1× source
      sub-rect inside the host-scaled MTLTexture). New API:
      `pgraph_mtl_surface_bind_color_ex` / `_bind_depth_ex` (extended
      bind that takes guest_w/h),
      `pgraph_mtl_surface_mark_dirty_overlapping(addr, len)`
      (atomic-set `dirty_vram` on overlapping entries),
      `register_access_cb_for` / `unregister_access_cb_for` (store
      / retrieve the cb pointer), `upload_dirty(vram_ptr)` (iterate
      cache, upload every dirty entry), `upload_if_dirty_at(vram_addr,
      vram_ptr)` (single-entry lazy upload via `_get_within` lookup),
      `force_upload_at` (used at allocate-time), `iter_addresses`
      (snapshot for disarm-all). `upload_vram_to_texture` rewritten
      to use `guest_w/h` source sub-rect. Counter accessors:
      `pgraph_mtl_surface_vram_dirty_hits()`,
      `_vram_uploads()`, `_vram_upload_bytes()` →
      `METAL_SURFACE_VRAM_DIRTY_HITS`, `METAL_SURFACE_VRAM_UPLOADS`,
      `METAL_SURFACE_VRAM_UPLOAD_BYTES`. **PGR2 90 s benchmark:
      uploads=2/interval (bind-time path firing), dirty_hits=0/interval
      (PGR2's TCG vCPU does NOT write to watched surface ranges).
      Visual gate FAILED — captured PNGs show cleared-color sub-rect
      + heap-default magenta; rendered scene content does not reach
      the CRTC-published `0x32a4000` surface despite 1505 draws/s.
      All three task-framing buffer-swap mechanisms (a) (b) (c)
      decisively ruled out by the diagnostic counters. Followup-D
      (surface download) still deferred.** See decision-log
      "2026-05-03: Metal slice M5.9-followup-B+C — CPU-write dirty
      tracking + VRAM upload" for full investigation + open
      hypotheses.
      **(M5.9-followup-E, 2026-05-03 — SHIPPED, magenta artifact
      closed; visual gate STILL FAILS pending M5.10)** Three real
      bugs in the surface manager fixed via the new per-vram_addr
      `metal_draw_target` diagnostic counter at
      `pgraph_mtl_flush_draw`: (1) **color/depth cache collision**
      — `cache_get_at(addr)` was unfiltered by aspect, so a
      same-vram_addr color-bind / depth-bind alternation thrashed
      each prior binding via the destroy-and-recreate path
      (PGR2 hits this on `vram_addr=0x0` + a real surface where
      `dma.address+offset=0`). Split into `cache_get_at_color` and
      `cache_get_at_depth`; `METAL_SURFACE_RECREATE_SHAPE_MISMATCH`
      drops 6/interval → 0. (2) **LRU eviction of stably-published
      front-fb** — publish dedupe didn't bump `last_use_seq`, so
      the front-fb's LRU score went stale and the cache picked it
      as eviction victim, producing the heap-default magenta. Pin
      the published texture in `cache_evict_lru` + bump
      `last_use_seq` on every publish call. (3) **Cache cap
      `kMaxCacheEntries` raised 16 → 32** because PGR2 has 11+
      color RTs + depth + ensure-by-shape entries; cap was always
      saturated and LRU was thrashing real surfaces.
      **Codex-validate follow-up fix (HIGH severity):** the
      shape-mismatch recreate path (color side) now also clears
      `s_front_framebuffer_texture` if it equals the entry's
      texture, before `binding_destroy(e)` — symmetric with the
      LRU pin, so a guest-side surface reconfiguration doesn't
      reproduce the magenta class via the destroy route. New API:
      `pgraph_mtl_surface_get_color_vram_addr` /
      `_get_depth_vram_addr` getters used by the diagnostic.
      Magenta artifact closed; visual gate still fails because
      PGR2 renders to back buffer `0x3628000` and CRTC publishes
      `0x32a4000` — Metal has no mechanism to bridge these (GL
      uses display-side `get_framebuffer_surface` callback that
      reads VRAM at host-vsync time; Vulkan uses
      `pgraph_vk_surface_download_if_dirty`). New slice **M5.10
      — VRAM-coherent surface download** (or alternatively:
      register a Metal `get_framebuffer_surface` ops callback) is
      the next blocker for M15 default-on. See decision-log
      "2026-05-03: Metal slice M5.9-followup-E".
      **(M5.10/M5.11 + MSAA follow-up, 2026-05-04 — PGR2/Rainbow/Halo/boot
      MSAA4 canaries PASS; Crimson/SC2 visual routes BLOCKED)** PGR2's
      old white/magenta/front-fb failure is closed, and the later PGR2
      MSAA4 black-frame regression is fixed by storing MSAA attachments
      across pass breaks. Surface cache now retains multiple shapes per
      VRAM address, uses exact/near shape lookup, caps at 64 entries,
      publishes the selected fallback binding directly, performs
      dimension-aware render-target texture lookup, registers access
      callbacks across all same-VRAM siblings, and uploads every dirty
      sibling. Scaled VRAM upload fills the full host-scaled texture.
      A8R8G8B8-family render targets sampled as linear A8R8G8B8-family
      texture views use the CPU texture path; diagnostic env
      `XEMU_METAL_DISABLE_SURFACE_TEX_ADDRS=1`. Latest green screenshots:
      PGR2 `benchmark-runs/visual-checks/pgr2-gate-metal-msaa4-f900-after-msaa-store.png`,
      Rainbow `benchmark-runs/visual-checks/rainbow-gate-metal-msaa4-f600-after-msaa-store.png`,
      Halo `benchmark-runs/visual-checks/halo-gate-metal-msaa4-f1200-after-msaa-store.png`.
      **(PGR2 texture-bind attribution, 2026-05-04)** The subsequent
      150s PGR2 Metal gameplay run
      `benchmark-runs/20260504-132806-pgr2` proved shader translation
      failures and pipeline fallbacks are now zero, but post-load FPS
      was still 11.21 before the final rerun. New CPU wall counters
      identified `pgraph_mtl_texture_bind_from_pg` as the bottleneck:
      `METAL_TEX_BIND_US_TOTAL=105962660` vs
      `METAL_DRAW_ENCODE_US_TOTAL=7159221`. Metal now has an early
      cached-texture bind path (`pgraph_mtl_texture_bind_slot_cached_full`)
      so clean cache hits bind resident textures before
      `decode_face_levels()`. Next session should rerun the same 150s
      PGR2 route and compare against
      `docs/apple-silicon/benchmarks/2026-05-04-metal-pgr2-texture-bind-attribution.md`.
      Next Metal work should route Crimson gameplay and SC2 to real
      rendered visual canaries and resolve the front-fb fallback policy
      before revisiting M15.
    - `blit.c` — **(M5.9-followup-A, 2026-05-03)**
      `pgraph_mtl_image_blit(NV2AState *d)` mirrors
      `vk/blit.c::pgraph_vk_image_blit` and
      `gl/blit.c::pgraph_gl_image_blit`. CPU-side memcpy keeps guest
      VRAM correct (the authoritative oracle); GPU-side
      `pgraph_mtl_surface_blit_copy` propagates the same pixels into
      cache-resident MTLTextures. Format mismatch invalidates the
      dst cache entry so the next bind picks up the freshly-memcpy'd
      VRAM contents (mirrors vk's `register_cpu_access_callback +
      surface_access_callback` flow but lazy-on-rebind instead of
      immediate). Drains the open coalesced render pass before any
      blit-encoder use of the affected textures.
- `ui/xemu-input.c` — `XEMU_SCRIPTED_INPUT` (CSV replay) and
  `XEMU_RECORD_INPUT` (CSV record).
- `ui/xemu-snapshots.c` — `XEMU_SNAPSHOT_NO_THUMBNAIL=1`.
- `scripts/apple-silicon/` — benchmark harness, perf summary, validators.

When making renderer changes, prefer searching the existing tree rather
than recalling specific files from memory; commits and the file list above
can drift.

## Runtime flags maintained by this fork

Stable opt-in:

- `XEMU_NATIVE_TRI_DEPTH=1` — opt-in triangle-family fill native GL path
  (no geometry shader; depth/slope derived in fragment shader). The
  flat-first case stays native; flat-nonfirst falls back to the geometry
  shader. Set `=0` to disable explicitly (overrides the alias below).
- `XEMU_DIAG_NATIVE_TRI_DEPTH=1` — compatibility alias for the above. New
  runs and scripts should use the stable name.
- `XEMU_NATIVE_QUAD=1` — opt-in quad/quad-strip-family fill bypass. CPU
  expands quads to triangles using the geometry shader's diagonal
  triangulation, and the fragment shader derives depth via the same
  `gl_FragCoord` path used by `XEMU_NATIVE_TRI_DEPTH`. Smooth fill only;
  flat-shaded quads, line/point polygon modes, and any nonfill raster mode
  stay on the geometry shader. Independent of `XEMU_NATIVE_TRI_DEPTH`; for
  the full bypass set both. Set `=0` to disable explicitly.
- `XEMU_PGRAPH_FAST_READ=1` — opt-in lock-free PGRAPH register read fast
  path. `pgraph_read()` returns a `qatomic_read()` snapshot of the
  requested register without acquiring `pg->lock` for `NV_PGRAPH_INTR`,
  `NV_PGRAPH_INTR_EN`, and the default `pg->regs_[]` slot reads.
  `NV_PGRAPH_RDI_DATA` still locks because reading it auto-increments
  `NV_PGRAPH_RDI_INDEX_ADDRESS`. Aligned 32-bit loads on aarch64 and x86
  are atomic, so the mutex is strict overhead for these reads; skipping it
  removes the bulk of TCG-thread mutex-wait time. Independent of the
  geometry-shader bypasses; biggest payoff comes when both bypasses are on
  (so the renderer holds the lock for shorter intervals). Set `=0` to
  disable explicitly.
- `XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1` — opt-in async shader compile
  worker. A `pgraph.gl_async_compile` thread bound to a third shared
  `g_nv2a_context_shader_compile` GL context runs `glLinkProgram` /
  `generate_shaders()` off the renderer's critical path; the renderer
  uses a "skip the draw" fallback while a binding is still being
  compiled (RPCS3 PR #4876 pattern). Counters
  `SHADER_COMPILE_ASYNC_QUEUED`, `SHADER_COMPILE_ASYNC_COMPLETED`,
  `SHADER_DRAWS_SKIPPED_PENDING` confirm worker drain and skipped-
  draw count. **Note:** correct & shipped opt-in, but does NOT fix
  Crimson Skies' 1.35-second worst-frame stutter (proved 2026-05-01;
  the stutter is on the TCG vCPU thread, not the renderer). Default
  off. See `docs/apple-silicon/benchmarks/2026-05-01-async-shader-compile.md`
  and `docs/apple-silicon/benchmarks/2026-05-01-gl-vs-metal-decision.md`.
- `XEMU_TCG_SPLITWX={0,1}` — overrides the splitwx auto-default for
  the TCG JIT. Apple Silicon system builds default to splitwx-on
  (`mach_vm_remap` dual-mapping path in `tcg/region.c`), which
  removes the per-TB-execution `pthread_jit_write_protect_np()`
  syscall on every `cpu_tb_exec`. The W^X toggle wrappers in
  `include/qemu/osdep.h` are diff-guarded so they no-op when
  `tcg_splitwx_diff != 0`. Set `=0` to fall back to the upstream
  MAP_JIT path (correctness-equivalent, same per-TB toggle cost);
  set `=1` to force splitwx where the auto-default is off. Explicit
  `-accel tcg,split-wx=on|off` always wins over the env var; env
  var wins over auto-default. See `docs/apple-silicon/automation.md`
  for memory implications and the `TCG_*` perf counters used to
  validate the slice.
- `XEMU_TCG_JMP_CACHE_TARGETED={0,1}` — overrides the per-page
  targeted jmp-cache invalidation auto-default. Apple Silicon system
  builds default to ON. Replaces the unconditional 4096-entry
  per-CPU jmp-cache zero in the `CF_PCREL` branch of
  `tb_jmp_cache_inval_tb` (i386 system-mode globally sets `CF_PCREL`)
  with a single-bucket clear per invalidated TB, batched after the
  `tb_invalidate_phys_page_range__locked` loop. Correctness rests on
  the existing `CF_INVALID` + cflags-equality check in
  `cpu-exec.c::tb_lookup` (line 267). Set `=0` to fall back to the
  upstream full-zero path (rollback for A/B testing or correctness
  triage); set `=1` to force on where the auto-default is off. The
  non-PCREL `tb_jmp_cache_inval_tb` and `tb_flush` / cputlb full-flush
  paths are unchanged. See `docs/apple-silicon/automation.md` for
  the correctness argument and the `TCG_JMP_CACHE_ZEROED_BUCKETS` /
  `TCG_INVALIDATE_WALL_US_MAX` perf counters used to attribute the
  slice's effect.
- `XEMU_GL_MSAA={0,2,4,8}` — opt-in multisample anti-aliasing on the
  OpenGL renderer. Default 0 (off; byte-identical behavior). Non-zero
  values allocate per-surface multisample renderbuffers
  (`glRenderbufferStorageMultisample`) attached to the draw FBO and
  perform a lazy `glBlitFramebuffer` resolve into the existing
  single-sample texture before any consumer (surface download,
  surface-to-texture, display render) reads from it. Sample count is
  clamped to `GL_MAX_SAMPLES`; the effective value is logged once at
  startup as `xemu-perf: gl_msaa=N source=XEMU_GL_MSAA requested=R
  max_samples=M`. Composes with `XEMU_DISPLAY_SCALE`/`surface_scale`
  (e.g. scale 2 + MSAA 4 = 1080p-class supersampled, 4x multisampled).
  Per-frame cost is reported as the new `MSAA_RESOLVE_US_TOTAL`
  counter; watch `SHADER_COMPILE_*` when first enabling to confirm
  Apple's GL-on-Metal driver does not balloon pipeline-variant compile
  cost under the multisample render-target state. Implemented in
  `hw/xbox/nv2a/pgraph/gl/surface.c` and `hw/xbox/nv2a/pgraph/gl/display.c`.
- `XEMU_DISPLAY_SCALE={1,2,3,4}` — overrides the loaded
  `display.quality.surface_scale` value for this xemu session
  without modifying the user's saved preference. Mirrors
  `XEMU_BENCH_SURFACE_SCALE` (the benchmark-runner equivalent that
  injects `[display.quality] surface_scale = N` into the per-run
  config). Apple Silicon system builds default `surface_scale` to 2
  (1080p-class internal resolution; ~7 % renderer-cost growth on
  PGR2 vs scale 1, see
  `docs/apple-silicon/benchmarks/2026-05-01-gl-vs-metal-decision.md`)
  on **first launch only**; existing users with a stored config
  keep their current value untouched. Out-of-range / unparseable env
  values are silently ignored. The renderer also clamps factor < 1
  to 1 in `pgraph_*_reload_surface_scale_factor`. Bridge implemented
  in `ui/xemu-settings.cc::xemu_settings_apply_display_scale_env`;
  first-run platform default in
  `xemu_settings_first_run_default_surface_scale`.
- `XEMU_FAST_RDTSC={0,1}` (V9, 2026-05-02) — overrides the
  Apple Silicon RDTSC fast-path auto-default. Apple Silicon
  system builds default to ON. Replaces the legacy `cpu_get_tsc`
  call chain (`qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)` →
  `cpu_get_clock` seqlock → `cpu_get_clock_locked` → `get_clock`
  → `clock_gettime(CLOCK_MONOTONIC)` → libsystem internals →
  `mach_absolute_time` — 7-9 functions deep, ~80-100 ns per
  call) with a direct `mach_absolute_time` + cached
  `mach_timebase_info` + `muldiv64` path (~15-20 ns per call).
  Estimated ~70 % overhead reduction per RDTSC, targeting Xbox
  kernel busy-waits that deadline-check on RDTSC. Set `=0` to
  fall back to the legacy QEMU clock path (rollback for A/B
  testing). The vm_clock_offset (added by cpu_get_clock_locked
  when the VM is paused) is omitted from the fast path; xemu
  does not pause/resume the VM mid-execution and the guest only
  observes TSC deltas, so this is safe. Implementation in
  `hw/i386/x86-cpu.c::cpu_get_tsc`. Accompanying counter
  `HELPER_RDTSC_CALLS` (per-interval sum) confirms the call
  rate; surfaced in `extract-perf-summary.sh`. Decisive
  attribution measurement of the Crimson worst-frame stutter:
  if `HELPER_RDTSC_CALLS` is in the millions per worst-frame
  interval, the V8 sample-profile-derived hypothesis (kernel
  busy-wait on RDTSC) is confirmed. See
  `docs/apple-silicon/benchmarks/2026-05-02-v7-cumulative-phase-attribution.md`
  Mission 4 for the V8 evidence.
- `XEMU_APU_LOCK_RELEASE={0,1}` — overrides the audio voice-lock
  release auto-default. Apple Silicon system builds default to ON.
  Releases `MCPXAPUState::lock` while the APU worker thread is
  waiting for the per-frame voice-worker batch to finish inside
  `voice_work_dispatch` (`hw/xbox/mcpx/apu/vp/vp.c`), then
  re-acquires before publishing the frame's mixbins. Targets the
  D3-attributed Crimson voice-lock contention (21.3 s / 300 s of
  vCPU thread time spent blocked on
  `mcpx-apu-vp/0xfe8202fc` = NV1BA0_PIO_VOICE_LOCK; see
  `docs/apple-silicon/benchmarks/2026-05-02-tcg-30fps-cap-attribution.md`).
  Distinct from the prior reverted `XEMU_VOICE_FAST_LOCK` slice
  (decision-log entry "2026-05-01: XEMU_VOICE_FAST_LOCK not
  landed"): that slice tried bitmap-level lock-elision; this slice
  shrinks the APU thread's lock-hold so the vCPU's `voice_lock()` /
  `gp_write` / `ep_write` acquires no longer block for the full
  ~5.33 ms VP frame. Set `=0` to fall back to the legacy
  lock-held-throughout-frame behavior (rollback for A/B testing or
  audio-correctness regression triage); set `=1` to force on where
  the auto-default is off. Accompanying counters
  `APU_LOCK_HOLD_US_TOTAL` and `APU_VCPU_LOCK_WAIT_US_MAX` (below)
  attribute the slice's effect.
- `XEMU_MACOS_NATIVE_INPUT={0,1}` (slices N1 + N2, 2026-05-03) —
  opt-in native macOS controller backend via Apple's
  `GameController.framework`. Default 0 (off; existing users see
  exactly today's SDL3 behavior). When set, `xemu_input_init`
  enumerates `[GCController controllers]` and registers connect /
  disconnect notification handlers; the per-frame poll path
  (`xemu_input_update_controller`) reads `GCExtendedGamepad`
  properties directly instead of draining the SDL event queue and
  reading SDL's cached gamepad state. Removes one thread-hop +
  ~tens-of-µs of SDL event-queue dispatch per controller change.
  Mapping is by `GCControllerPlayerIndex`: xemu port N → playerIndex
  N. SDL still owns connect/disconnect lifecycle and per-port
  binding (the rebind UI is built on SDL events; Linux + Windows
  builds keep depending on SDL); the native backend only takes over
  the read path on macOS. Rumble on the native path is intentionally
  a no-op for slice N2 (Core Haptics integration is N4); first call
  logs a one-shot diagnostic. Logged once at startup as
  `xemu-perf: macos_native_input enabled controllers=N` plus a
  per-controller line documenting class / vendor / haptics / player
  index. Implementation: `ui/xemu-macos-input.{h,mm}`,
  `ui/xemu-input.c` (read-path dispatch),
  `Info.plist::GCSupportsControllerUserInteraction = YES` (macOS
  Sonoma+ Game Mode polling-rate doubling). Companion latency
  counters from slice N1 (always-on, surface on `xemu-perf:`):
  `INPUT_USB_POLLS`, `INPUT_BACKEND_UPDATES`, `INPUT_LAT_US_TOTAL`,
  `INPUT_LAT_US_MAX`. Apple Silicon performance fork; built only on
  darwin+arm64. See `docs/apple-silicon/macos-input-research.md` for
  the full migration plan and slices N3-N6 followups.
- `XEMU_METAL_SHADER_VALIDATE={0,1,strict,2}` (M5, 2026-05-02) —
  development-only Metal shader-translation harness. When set,
  `xemu_metal_init` runs the in-process M5 harness (6 fixed-function
  + combiner GLSL fixtures + 1 M7 framebuffer-fetch fixture; 7/7
  PASS expected on Apple Silicon). Each fixture runs through
  `pgraph_mtl_glsl_translate_to_msl` (GLSL→SPIR-V→MSL via
  spirv-cross) and the resulting MSL is validated by
  `[device newLibraryWithSource:]`. Counters
  `METAL_SHADER_VALIDATE_OK` / `METAL_SHADER_VALIDATE_FAIL` and
  `METAL_GLSL_TRANSLATE` / `METAL_GLSL_TRANSLATE_FAIL` surface on
  `xemu-perf:`. Set `=strict` or `=2` to abort the run on the first
  fixture failure. Default 0. Used by
  `scripts/apple-silicon/metal-shader-validation/run-validation.sh`.
  Implementation in `hw/xbox/nv2a/pgraph/mtl/shader_validation.c`.
- `XEMU_METAL_SHADER_VALIDATE_AND_EXIT={0,1}` (M5, 2026-05-02) —
  CI-runner companion for `XEMU_METAL_SHADER_VALIDATE`. Exits the
  xemu process with status 0 (all fixtures pass) or 1 (any fail)
  after the harness reports — used by the `run-validation.sh`
  script to drive the build's regression gate without leaving an
  xemu instance running. Default 0. Implementation in
  `ui/xemu-metal.mm`.
- `XEMU_METAL_PIPELINE_CACHE={0,1}` (M9, 2026-05-02) — overrides
  the persistent MSL-source disk cache for the Metal renderer.
  Apple Silicon system builds default to ON. Persists the combined
  MSL source string per `PgraphMtlPipelineKey` hash under
  `<base>/metal_shaders/<top16>/<bottom48>.msl`, mirroring
  `gl/shaders.c`'s GL-side disk cache. On cold launch the stored
  MSL is loaded directly into `[device newLibraryWithSource:]`,
  skipping the GLSL→SPIR-V→MSL spirv-cross translation step (the
  most expensive per-shader cost in the M5–M8 build path). Cache
  invalidation: per-file self-describing header carries the xemu
  version + a Metal feature-set fingerprint
  (`AppleGPUFamily<N>/macOS<major>.<minor>` — Apple7=M1, Apple8=M2,
  Apple9=M3) + the full key blob; mismatch → file unlinked. Saves
  run on detached `metal-scache-<hash>` background threads
  (concurrent cap 64; synchronous fallback above the cap, never
  block-and-skip). File-system errors fail soft (log + skip; cache
  becomes a no-op for the affected entry, renderer falls back to
  the live translator path). Set `=0` to fall back to the M5/M8
  always-translate path (rollback for A/B testing or to verify a
  specific run is not benefiting from a stale cache). Counters
  `METAL_SHADER_CACHE_LOADS` / `METAL_SHADER_CACHE_HITS` /
  `METAL_SHADER_CACHE_MISSES` surface on the `xemu-perf:` interval
  line. The cache is per-base-path, not per-game (matches the GL
  side); games that never load the same NV2A `ShaderState` won't
  share entries either way. See decision-log "2026-05-02: Metal
  slice M9 — persistent MSL-source disk cache".
- `XEMU_GL_RATE_SLEW={0,1}` / `XEMU_RATE_SLEW={0,1}` (M10
  prerequisite, 2026-05-02) — opt-in graphics-API-agnostic
  emulation-rate slewing. When set, reads the host display's
  `SDL_GetCurrentDisplayMode().refresh_rate` at window-creation
  time and on `SDL_EVENT_DISPLAY_*` / `SDL_EVENT_WINDOW_DISPLAY_CHANGED`
  / `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`; if `host_hz / 60.0` is in
  `[0.95, 1.05]` updates `vblank_interval_ns` to
  `(uint64_t)(16,666,666 * (60 / host_hz))`. Mirrors PCSX2 PR #5488 /
  DuckStation "sync to host refresh". Naming note: the
  `XEMU_GL_RATE_SLEW` form matches the existing `XEMU_GL_*` family
  even though the flag is graphics-API-agnostic (lands on both GL
  and Metal); `XEMU_RATE_SLEW` is the clearer alias and wins when
  both are set. Default OFF for the first cut (validate with a
  benchmark before flipping default-on). Counters
  `RATE_SLEW_RATIO_E6` (host_hz / 60 × 1e6) and `RATE_SLEW_ACTIVE`
  (0/1) surface on the `xemu-perf:` interval line. Audio rate-match
  is deferred (sub-perceptual at typical 0.1 % drift). Implementation
  in `ui/xemu-rate-slew.{c,h}` + `include/qemu/xemu-rate-slew.h`.
- `XEMU_METAL_FORCE_LEGACY_PRESENT={0,1}` (M10, 2026-05-02) — overrides
  the Metal frame-pacing path. Default 0: M10 uses
  `[cmdbuf presentDrawable:drawable atTime:t]` with `t` computed
  from `mach_absolute_time + vblank_interval_ns` (first frame) or
  `prev_target + vblank_interval_ns` (steady state); resets to "now"
  if more than 2 vblank periods behind. Mirrors DuckStation's
  `metal_device.mm:2577-2601`. Set `=1` to fall back to plain
  `presentDrawable:` (no `atTime:`) for A/B comparison or
  correctness triage. The drawable carries an `addPresentedHandler:`
  block that records `|drawable.presentedTime - target|` per frame
  to feed the jitter counters
  `METAL_PRESENTS` / `METAL_PRESENT_JITTER_US_TOTAL` / `_AVG` / `_MAX`
  + `METAL_DRAWABLE_ACQUIRE_FAILS`. CAMetalDisplayLink integration
  is deferred to M10.1 (the slot for `METAL_DISPLAY_LINK_CALLBACKS`
  is reserved); the M10 plan-text "prefer CAMetalDisplayLink"
  recommendation is staged behind the simpler atTime: path because
  the latter mirrors a known-good emulator pattern (DuckStation,
  PCSX2) without inverting the existing vblank-thread control flow.
  See decision-log "2026-05-02: Metal slice M10 — frame pacing via
  presentDrawable:atTime:; CAMetalDisplayLink deferred to M10.1".
  Implementation in `ui/xemu-metal.mm`.
- `XEMU_METAL_MSAA={0,2,4,8}` (M11, 2026-05-02) — opt-in multisample
  anti-aliasing on the Metal renderer. Default 0 (off) for now;
  parsed once at `pgraph_mtl_init` and clamped against the active
  device's `[device supportsTextureSampleCount:N]` (M3 Ultra: 2 and
  4 supported, 8 steps down to 4). Effective value logged at startup
  as `xemu-perf: metal_msaa=N source=XEMU_METAL_MSAA requested=R
  configured=C`. Allocates a memoryless-style multisample companion
  texture per active color/depth surface (currently
  `MTLStorageModePrivate` because xemu's per-`flush_draw` render-pass
  cadence needs `MTLLoadActionLoad` to preserve prior content; true
  `MTLStorageModeMemoryless` returns when a future slice coalesces
  per-frame draws into one render pass — see
  `hw/xbox/nv2a/pgraph/mtl/heap.h` "storage-mode note"). Render
  passes use the multisample companion as `texture` and the
  single-sample binding as `resolveTexture`; color storeAction is
  `MTLStoreActionStoreAndMultisampleResolve`, and depth/stencil
  storeAction is `MTLStoreActionStore`, because later pass breaks load
  the same MSAA attachments. Pipeline `rasterSampleCount` is matched on both the
  M3/M4 hand-coded passthrough cache (keyed on (color_fmt, depth_fmt,
  variant, sample_count)) and the M5/M7.1 translated pipeline cache
  (sample_count is already a field of `PgraphMtlPipelineKey`'s
  `render_pass_state`). Composes with `XEMU_DISPLAY_SCALE` /
  `surface_scale` (e.g. scale 2 + MSAA 4 = 1080p-class supersampled,
  4× multisampled). MSAA is treated as session-fixed: changing the
  env requires restart so the pipeline cache does not balloon with
  sample-count variants. New counters `METAL_MSAA_RESOLVE_COUNT` /
  `METAL_MSAA_RESOLVE_US_TOTAL` / `METAL_MSAA_SAMPLE_COUNT` surface
  on the `xemu-perf:` interval line. Implementation in
  `hw/xbox/nv2a/pgraph/mtl/{heap,surface,draw,pipeline,renderer}*`
  and `util/xemu-metal-perf.c`. See decision-log "2026-05-02: Metal
  slice M11 — MSAA + resolve".
- `XEMU_METAL_FX_SCALE={1,2,3}` (M12, 2026-05-02) — opt-in
  `MTLFXSpatialScaler` upscale path on the Metal renderer. Default 1
  (off); `2` and `3` enable the scaler (the numeric value is
  preserved for forward-compat with future quality-tier variants —
  the present implementation is on/off). Parsed once at
  `xemu_metal_init`; the scaler instance + intermediate output
  texture are built lazily on the first present that supplies an
  NV2A framebuffer texture, and rebuilt whenever input dimensions /
  pixel format / drawable size change. Effective config logged at
  startup as `xemu-perf: metal_fx_scale=N source=XEMU_METAL_FX_SCALE
  requested=R configured=C enabled=B`. The scaler runs on each
  present: NV2A color RT (post-M11 resolve) → `MTLFXSpatialScaler`
  → private intermediate (`drawable_size`, `BGRA8Unorm_sRGB`,
  `MTLStorageModePrivate`) → existing fullscreen-triangle present
  pipeline → drawable. The encode call is placed before the HUD
  render encoder is opened (the scaler is a discrete pass operation,
  not a render-encoder draw). Bypassed for the frame whenever the
  drawable is at-or-below the input dimensions (downscale would
  add latency for no quality win). Composes orthogonally with
  `XEMU_DISPLAY_SCALE` / `surface_scale` and `XEMU_METAL_MSAA`:
  the input to the scaler is whatever the post-resolve color RT
  is, so e.g. `surface_scale=2` + `XEMU_METAL_MSAA=4` +
  `XEMU_METAL_FX_SCALE=2` runs MetalFX from a 1080p 4×-multisampled
  resolved texture up to drawable resolution. New counters
  `METAL_FX_SPATIAL_PRESENTS` (per-interval scaler invocations),
  `METAL_FX_SPATIAL_US_TOTAL` (CPU-side wallclock for the encode
  call — under-reports GPU-side scaler cost; real GPU timing
  arrives with M13's counter sample buffers) and
  `METAL_FX_SCALE_FACTOR` (latched effective config) surface on
  the `xemu-perf:` interval line. `MTLFXTemporalScaler` is
  intentionally not implemented — synthesizing motion vectors from
  camera-only reprojection is risky on dynamic scenes (NV2A has no
  native motion vectors); per-title evaluation is deferred. M12
  v1 ships with framework-cost-only counters; the user-driven
  visual + perf evaluation gate (the M12 exit gate's "visibly
  sharper than bilinear at < 1 ms scaler cost on M3") needs a
  Metal user-driven validation session. Implementation in
  `ui/xemu-metal.mm` and `util/xemu-metal-perf.c`. See decision-log
  "2026-05-02: Metal slice M12 — MetalFX spatial scaler".
- `XEMU_METAL_CAPTURE=path.gputrace` (M13, 2026-05-02) — opt-in
  programmatic Metal frame capture via `MTLCaptureManager`. When set,
  `xemu_metal_init` starts a capture bound to the active `MTLDevice`
  with destination `MTLCaptureDestinationGPUTraceDocument` writing to
  `file://<path>`; bounded by `XEMU_METAL_CAPTURE_FRAMES` (default
  60; `0` = until shutdown) so the resulting `.gputrace` stays small
  enough to open in Xcode (each frame is ~10–50 MB). Capture
  finalizes via `stopCapture` either when the frame target is
  reached (inside the per-frame `addCompletedHandler`) or at
  `xemu_metal_shutdown`. Preconditions: (a) `MetalCaptureEnabled =
  YES` in `Info.plist` (added in this slice; ships with every
  `dist/xemu.app` of the fork) **or** `MTL_CAPTURE_ENABLED=1` in the
  launching environment; (b) the device must support
  `MTLCaptureDestinationGPUTraceDocument` (Apple Silicon does). On
  failure (preconditions unmet, unwritable path) the renderer logs
  the failure and continues without capture — capture is a
  development tool. Counter sampling via `MTLCounterSampleBuffer`
  also lands in this slice: at `xemu_metal_init` the renderer
  allocates a 4-sample buffer (vertex_start, vertex_end,
  fragment_start, fragment_end) gated on
  `[device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]`
  (Apple7+ M1+; logged once at startup as
  `xemu-perf: metal_counter_sampling enabled (...)`); the present
  render pass attaches the buffer via
  `MTLRenderPassSampleBufferAttachmentDescriptor`; the
  `addCompletedHandler` resolves the timestamps via
  `[buffer resolveCounterRange:]` and accumulates per-stage
  microsecond totals. Replaces M11/M12's CPU-side wallclock
  placeholder counters with actual GPU-side timing. New counters
  `METAL_VERTEX_US_TOTAL` / `METAL_FRAGMENT_US_TOTAL` (per-stage
  GPU time per interval), `METAL_PRESENT_GPU_US_TOTAL` /
  `METAL_PRESENT_GPU_FRAMES` (cmdbuf-level GPUEndTime - GPUStartTime
  upper bound + frame count for the per-present cmdbuf),
  `METAL_FX_SPATIAL_GPU_US_TOTAL` (cmdbuf-level GPU time for
  MetalFX-encoded frames; replaces M12's CPU-side
  `METAL_FX_SPATIAL_US_TOTAL` placeholder),
  `METAL_CAPTURE_FRAMES` (frames seen since `startCapture`,
  monotonic; per-interval delta = frames captured this interval),
  `METAL_CAPTURE_ACTIVE` (latched 0/1 flag) all surface on the
  `xemu-perf:` interval line. Companion `--metal-capture <path>`
  flag added to `scripts/apple-silicon/run-benchmark.sh` exports
  the env var for the spawned xemu and writes
  `metal_capture_path` to the run's `metadata.txt`. Implementation
  in `ui/xemu-metal.mm`, `util/xemu-metal-perf.c`,
  `Info.plist`, `scripts/apple-silicon/run-benchmark.sh`,
  `scripts/apple-silicon/extract-perf-summary.sh`. See
  decision-log "2026-05-02: Metal slice M13 — frame capture +
  counter sampling".
- `XEMU_METAL_CAPTURE_FRAMES=N` (M13, 2026-05-02) — frame-count
  bound for `XEMU_METAL_CAPTURE`. Default 60; `0` means "capture
  until shutdown" (large `.gputrace`; useful only for very short
  runs or single-frame regression triage). Ignored when
  `XEMU_METAL_CAPTURE` is unset.
- `XEMU_METAL_FRONT_FB_DOWNLOAD={0,1}` (M5.10, 2026-05-03) — opt-in
  for the Metal renderer's GPU→VRAM surface-download path. Default 0
  (off). Mirrors `vk/surface.c::pgraph_vk_surface_download_if_dirty`
  (vk/surface.c:939-944) with Apple Silicon UMA semantics: at end of
  every `flush_draw` / `clear_surface` the bound color/depth cache
  entry is marked `draw_dirty=1`; subsequent `pgraph_mtl_flip_stall`
  and `pgraph_mtl_surface_flush` walk the cache and call
  `pgraph_mtl_surface_download_if_dirty_at(vram_addr, vram_ptr,
  mtl_after_surface_download, d)` on each dirty entry. The download
  opens a `MTLBlitCommandEncoder copyFromTexture:toBuffer:` against
  a Shared `MTLBuffer`, commits + waits, then memcpys the staging
  buffer into `d->vram_ptr + vram_addr` and bumps
  `DIRTY_MEMORY_VGA | DIRTY_MEMORY_NV2A_TEX`. A cross-queue
  `MTLSharedEvent` fence (`s_draw_done_event` in `mtl/draw.mm`,
  signaled monotonically at every draw-cmdbuf commit; consumed via
  `[cmd encodeWaitForEvent:value:]` at download time) guards
  against the render-queue blit reading a draw-target texture
  mid-render. Counters `METAL_SURFACE_DOWNLOADS` and
  `METAL_SURFACE_DOWNLOAD_BYTES` surface on the `xemu-perf:`
  interval line. **Default off because** the current PGR2 canary is
  bridged by the host-side fallback plus the 2026-05-04 surface/RTT
  fixes, while the GPU→VRAM blit + waitUntilCompleted path has
  measurable historical cold-boot perf cost (~4× slowdown on PGR2
  cold-boot per 2026-05-03 measurements). Enable with
  `XEMU_METAL_FRONT_FB_DOWNLOAD=1` for development / bisection. Apple
  Silicon performance fork; slice
  M5.10. Note: at the Apple Silicon system default
  `surface_scale=2` the download path detects the host-scaled
  texture mismatch and skips per-entry (the M5.10 MVP does not
  include a GPU-side downsample pass — that is deferred). To
  exercise the actual download blit, also set
  `XEMU_DISPLAY_SCALE=1`. KVM/HVF parity polling in
  `pgraph_mtl_surface_update` is gated `!tcg_enabled()` and
  always-on when active; uses the cache entry's full size for
  `memory_region_test_and_clear_dirty`.
- `XEMU_METAL_FRONT_FB_FALLBACK={0,1}` (M5.10 experimental,
  2026-05-03; PGR2 canary PASS after 2026-05-04 surface/RTT fixes) —
  opt-in fallback that publishes the selected color render-target
  binding as the front-fb after the CRTC publish. Default 0 (off). Use
  case: titles like PGR2 where the CRTC-pointed surface receives only
  sporadic draws while the actual rendered scene goes to another render
  target. NOT correctness-faithful — titles that legitimately use both
  front and back surfaces may see the wrong content. Implementation:
  `pgraph_mtl_surface_publish_latest_draw_fallback` in `mtl/surface.mm`,
  invoked from `pgraph_mtl_flip_stall` AFTER the CRTC publish so the
  fallback wins. When OFF, behavior is exactly the current CRTC-strict
  publish. Apple Silicon performance fork; slice M5.10 experimental
  plus 2026-05-04 follow-up.
- `XEMU_METAL_DISABLE_SURFACE_TEX_ADDRS={0,1}` (2026-05-04 diagnostic) —
  disables direct render-target-as-texture lookup by VRAM address and
  forces the CPU texture path. Useful for isolating surface texture
  aliasing and channel/alpha normalization issues. The default direct
  path now uses dimension-aware lookup and still forces A8R8G8B8 render
  targets sampled as linear A8R8G8B8-family texture views through the
  CPU path; that rule fixed PGR2's dotted/yellow text.
- `XEMU_METAL_VALIDATION={0,1}` (M14, 2026-05-02; W1 2026-05-04
  also promotes `MTL_SHADER_VALIDATION`) — opt-in Metal API + shader
  validation layer for development. When set, `xemu_metal_init`
  promotes both `MTL_DEBUG_LAYER=1` AND `MTL_SHADER_VALIDATION=1`
  into the process environment **before** the first
  `MTLCreateSystemDefaultDevice()` call; Apple's Metal framework
  reads each env exactly once at first device creation, so the
  env-vars must be in place by that point or validation never
  activates for the process. If the user has already pinned either
  env themselves the value is preserved (overwrite=0). The
  shader-validation promotion catches a class of shader-side bugs
  (out-of-bounds buffer reads, malformed bindings) that the API-layer
  `MTL_DEBUG_LAYER` cannot see. Surfaced once at startup as
  `xemu-perf: metal_validation requested=R promoted=P
  mtl_debug_layer_active=A mtl_shader_validation_active=A`. Default
  0 (validation off; matches M14's "MTL_DEBUG_LAYER=0 in shipped
  builds" rule). W1 (2026-05-04) auto-on policy:
  `scripts/apple-silicon/run-benchmark.sh` exports
  `XEMU_METAL_VALIDATION=1` whenever `XEMU_RENDERER=METAL` is in the
  launching environment, unless `--metal-no-validate` is passed or
  the user already pinned the env. Apple Silicon performance fork;
  slice M14 + W1. Implementation in `ui/xemu-metal.mm`.
- `XEMU_METAL_HUD={0,1}` (W1, 2026-05-04) — opt-in for Apple's Metal
  Performance HUD overlay. When set to 1, `xemu_metal_init` promotes
  `MTL_HUD_ENABLED=1` into the process environment **before** the
  first `MTLCreateSystemDefaultDevice()` call (same overwrite=0
  pattern as `XEMU_METAL_VALIDATION`); an explicit user
  `MTL_HUD_ENABLED` wins. The HUD is a zero-perf-cost overlay with
  frame time / GPU usage / memory stats, useful for development.
  Turn it off for clean visual canary captures. Surfaced once at
  startup as `xemu-perf: metal_hud requested=R promoted=P
  mtl_hud_enabled_active=A` mirroring the format of the
  `metal_validation` line. Default 0. W1 auto-on policy:
  `scripts/apple-silicon/run-benchmark.sh` exports `XEMU_METAL_HUD=1`
  whenever `XEMU_RENDERER=METAL` is in the launching environment,
  unless `--metal-no-hud` is passed or the user already pinned the
  env. Apple Silicon performance fork; slice W1. Implementation in
  `ui/xemu-metal.mm`.
- `XEMU_METAL_SCREENSHOT_PATH=/path/to/file.png` (2026-05-03) —
  programmatic PNG screenshot of the final composited drawable,
  encoded inside the Metal renderer (no `screencapture`, no
  Screen-Recording permission dialog, no window occlusion). Capture
  point is AFTER the HUD ImGui-Metal encoder closes and BEFORE
  `presentDrawable:`, so the encoded image is byte-identical to what
  the user would see on screen. Implementation: blit drawable
  texture → host-shared `MTLBuffer`; `addCompletedHandler:` runs
  after GPU completion, swaps BGRA→RGBA, writes the PNG via FPNG
  (`ui/thirdparty/fpng/`). Side effect: the env enables flips
  `s_layer.framebufferOnly` from `YES` to `NO` at `xemu_metal_init`
  so the drawable can be the source of a blit (display compression
  is off only for screenshot-enabled runs). PNG-encoding errors are
  logged and swallowed — capture is best-effort. Counter
  `METAL_SCREENSHOTS_TAKEN` (per-interval delta) surfaces on the
  `xemu-perf:` interval line. Companion script flags
  `--metal-screenshot <path>` / `--metal-screenshot-at-frame <N>` on
  `scripts/apple-silicon/run-benchmark.sh`. Default unset.
- `XEMU_METAL_SCREENSHOT_AT_FRAME=N` (2026-05-03) — frame number
  (1-indexed against the renderer's submit-time end-of-frame
  counter, NOT `pgraph_mtl_present_total`) at which
  `XEMU_METAL_SCREENSHOT_PATH` fires. Default 60. Submit-time was
  picked because the present counter is only bumped from
  `addPresentedHandler:`, which does not fire while the macOS
  Screen-Recording dialog occludes the xemu window — that's exactly
  the configuration the screenshot path is meant to work in.
- `XEMU_METAL_SCREENSHOT_INTERVAL=N` (2026-05-03) — when N≥1 the
  capture repeats every N frames after the first shot, writing
  `<base>.0001.png`, `<base>.0002.png`, ... (the `.NNNN` suffix is
  inserted before a trailing `.png` if present, appended otherwise).
  Default 0 = single shot. Implementation in `ui/xemu-metal.mm`,
  `util/xemu-metal-perf.c`, `scripts/apple-silicon/run-benchmark.sh`,
  `scripts/apple-silicon/extract-perf-summary.sh`.
- `XEMU_METAL_SCREENSHOT_SOURCE={drawable,nv2a,vram:0xADDR}`
  (2026-05-03) — selects which texture the screenshot path captures.
  `drawable` (default) reads the post-HUD final drawable. `nv2a`
  reads the NV2A framebuffer texture pre-present, bypassing the
  present pipeline. `vram:0xADDR` captures any specific cached
  `MtlSurfaceBinding` by vram_addr (used during the M5.9 magenta
  investigation to inspect front-buffer / back-buffer / aux RT
  contents independently). The hex value is parsed with
  `strtoul(..., 0)` so the `0x` prefix is required.

Diagnostic toggles (intentionally not correctness paths):

- `XEMU_METAL_DUMP_DRAW_RT=START:END:PREFIX` (W4, 2026-05-04) — per-draw
  color render-target dump on the Metal renderer. `START` and `END` are
  0-indexed **inclusive** `[START, END]` **cumulative-per-RUN**
  flush_draw indices (NOT per-frame; matches Mesa/RADV debug-dump
  semantics; the in-source check is `idx >= START && idx <= END`).
  `PREFIX` is a filesystem prefix (absolute or relative); outputs are
  written to `<PREFIX>.<draw_index_zero_padded_6>.png`, e.g.
  `/tmp/wd_test.000010.png`. Empty / unset / malformed → disabled with
  zero hot-path cost (one global load + branch). Asynchronous: at the
  end of every `pgraph_mtl_flush_draw` the open coalesced render pass
  is closed (so the post-MSAA-resolve color texture is the source),
  the bound color binding texture is blit-copied into a host-shared
  MTLBuffer, and the cmdbuf's `addCompletedHandler` BGRA→RGBA swaps
  and writes the PNG via FPNG. Renderer thread does not block. First
  five dumps emit a `xemu-perf: metal_draw_rt_dump idx=N path=...`
  rate-limited line; further dumps are silent (counter still ticks).
  Counter `METAL_DRAW_RT_DUMPS` (per-interval delta) surfaces on the
  `xemu-perf:` interval line. Implementation in `mtl/draw.mm`.
- `XEMU_GL_DUMP_DRAW_RT=START:END:PREFIX` (W4, 2026-05-04) — GL-side
  equivalent. Same inclusive `[START, END]` semantics as the Metal
  flag. Synchronous-but-isolated: `glReadPixels` blocks the
  renderer thread for the duration of the readback (intentional for a
  debug-only path). Post-MSAA-resolve via the existing
  `pgraph_gl_resolve_surface_msaa` helper. Counter `GL_DRAW_RT_DUMPS`
  surfaces on the `xemu-perf:` interval line. Implementation in
  `pgraph/gl/draw.c` + `pgraph/gl/dump.cc` (FPNG shim).

- `XEMU_CAPTURE_AT_FLIP_STALL=N` (F1, 2026-05-04) — dev-only paired-diff
  alignment aid. `N` is the 1-indexed Nth `NV097_FLIP_STALL` since
  process start; the FLIP_STALL handler in `pgraph.c:1045` calls
  `xemu_capture_at_flip_stall_tick()` and arms one-shot via CAS when
  the count equals the target. The Metal renderer's `end_imgui_frame`
  consumes the armed flag and one-shots the in-renderer screenshot
  path, bypassing the existing `s_screenshot_at_frame` ordinal match.
  `0`/unset disables; subsequent flip_stalls past the target stay
  no-op. Renderer-agnostic; with the env unset, hot-path cost is one
  `qatomic_read` per FLIP_STALL plus a CAS-guarded lazy init.
  Implementation in `util/xemu-display-perf.c:42-187` and
  `include/qemu/xemu-display-perf.h:75-99`. Used in tandem with
  `metal-gl-compare.sh --trigger flip --trigger-ordinal N` for
  Phase 2 paired-diff alignment.
- `XEMU_CAPTURE_FLIP_STALL_SENTINEL=/path` (F1, 2026-05-04) —
  dev-only companion sentinel filesystem path. xemu touches it once
  on arm via `O_CREAT | O_EXCL`; the GL leg's `macos-capture.sh`
  polls every 100 ms and one-shots `screencapture` on first
  appearance. Path must NOT exist at run start (`EEXIST` is logged
  and treated as already-armed). Used together with
  `XEMU_CAPTURE_AT_FLIP_STALL=N`; setting only the sentinel without
  the trigger ordinal is a no-op.

- `XEMU_METAL_DIAG_CLEAR={0,1}` (2026-05-03) — diagnostic logger for
  `pgraph_mtl_surface_clear`. When `=1`, emits up to 32 one-line
  `xemu-perf: metal_surface_clear vram_addr=0x.. rgba=(R,G,B,A)
  write_zeta=N` records (capped to bound log size). Used to confirm
  the magenta artifact is NOT from any guest clear color: every PGR2
  clear observed was `(0,0,0,1)` or `(1,0,0,1)`, never magenta.
  Default 0 (off; zero hot-path cost). Implementation in
  `mtl/surface.mm::pgraph_mtl_surface_clear`.

- `XEMU_DIAG_SIMPLIFY_TRI_GEOM_DEPTH=1` — keep tri geometry shaders, skip
  their depth/slope math.
- `XEMU_DIAG_SKIP_TRI_GEOM=1` — skip tri geometry shaders entirely. Drops
  per-triangle depth payload; correctness-breaking.
- `XEMU_DIAG_NATIVE_TRI_DEPTH_TRACE=1` — capped logging for shader-bind /
  draw-begin / draw-flush state correlation.

Logging:

- `XEMU_PERF_LOG=1` — emit `xemu-perf:` interval lines. `mspf_avg` /
  `mspf_min` / `mspf_max` are sub-millisecond floats; the renderer
  HUD's integer-ms `frame_working.mspf` is unaffected.
- `XEMU_PERF_LOG_INTERVAL_MS=N` — interval (default 1000).
- `XEMU_PERF_FRAME_LOG=1` — append per-frame `frame_mspf_us=v1,v2,...`
  to each interval line (bounded 1024 frames; overflow noted in
  `frame_mspf_us_dropped`). Off by default.
- `XEMU_PERF_SPIKE_LOG=1` — emit `xemu-spike: op=<name>
  duration_us=<n> now_us=<n>` per-event spike lines whenever a single
  timed renderer operation exceeds the spike threshold. Used to
  pinpoint which renderer path is slow during a stutter. Off by
  default.
- `XEMU_PERF_SPIKE_LOG_THRESHOLD_US=N` — minimum operation duration
  (microseconds) that triggers a spike line. Default 50000
  (50 ms). Lower values catch finer events at the cost of log volume.
- `XEMU_PERF_SPIKE_LOG_TCG=1` — enable TCG / iothread / MMIO spike
  sources independently of the renderer-side `XEMU_PERF_SPIKE_LOG=1`.
  Adds: `tcg_tb_chain`, `tcg_invalidate_burst`, `tcg_notdirty_storm`,
  `tcg_x87_storm`, `tcg_pg_lock_wait`, `renderer_pg_lock_wait` (V3),
  plus `qemu_main_loop_iter`, `bql_acquire_wait`, `aio_run_iter`,
  `mmio_helper_block` (D3), plus `tcg_handle_interrupt`,
  `tcg_tb_lookup`, `tcg_tb_gen_code` (V6 — per-phase decomposition
  of the cpu_exec_loop inner iteration; see
  `docs/apple-silicon/automation.md` for the full list and `extra=`
  field semantics). Hot-path cost when off is one global load +
  branch per call site. Off by default.
- `XEMU_TCG_PHASE_LOG=1` (V7, 2026-05-02) — enable cumulative
  per-interval wallclock accumulation of the three V6 phases.
  Surfaces `TCG_TB_LOOKUP_US_TOTAL`, `TCG_TB_GEN_CODE_US_TOTAL`,
  `TCG_HANDLE_INTERRUPT_US_TOTAL` (sum, microseconds) on the
  `xemu-perf:` interval line. Internal accumulation is in
  nanoseconds to avoid sub-µs per-call truncation. Independent of
  the spike-log threshold; if both spike-log and phase-log are on,
  the per-call clock read is shared. Hot-path cost when off is one
  global load + branch per phase per inner-loop iteration. Cost
  when on: ~36 % vCPU overhead worst-case at 3M TBs/interval. Off
  by default. Used to attribute the cumulative sub-millisecond
  translation-churn cost that V6's per-event 1 ms threshold
  cannot resolve.
- `XEMU_SNAPSHOT_NO_THUMBNAIL=1` — skip snapshot thumbnail capture.

Display-pacing counters (D3, 2026-05-02; see `automation.md` for the
full text). Always-on atomics emitted on the `xemu-perf:` interval
line when `XEMU_PERF_LOG=1`:

- `NV2A_VBLANK_FIRES` — xemu-side vblank IRQ deliveries to the guest
  per interval (driven by `vblank_interval_ns = 16,666,666 ns =
  60 Hz`, hardcoded in `ui/xemu.c`).
- `NV2A_FLIP_STALL_WRITES` — guest writes to `NV097_FLIP_STALL`.
- `NV2A_PRESENT_HEARTBEAT` — actual page-flip completions
  (NV_PGRAPH_INCREMENT_READ_3D writes); same as `increment_fps`
  integrated over the interval.
- `XEMU_GL_SWAPS` — host calls to `SDL_GL_SwapWindow`. Subject to
  cross-thread emit-vs-increment race noise at the per-interval
  granularity; sum across the run for a meaningful per-second value.

Decisive ratio for cap attribution: `NV2A_VBLANK_FIRES > 30/s` while
`NV2A_PRESENT_HEARTBEAT == 30/s` ⇒ guest-intrinsic 30 FPS engine cap
(observed on PGR2/Rainbow/Crimson; the cap is **not** xemu pacing).
`NV2A_VBLANK_FIRES == 30/s` ⇒ xemu's vblank pacing is the cap (not
observed on this fork).

Input automation:

- `XEMU_SCRIPTED_INPUT=path/to.csv` + `XEMU_SCRIPTED_INPUT_PORT=1`.
- `XEMU_RECORD_INPUT=path/to.csv` + `XEMU_RECORD_INPUT_PORT=1`.

Benchmark launcher knobs (read in `scripts/apple-silicon/run-benchmark.sh`):

- `XEMU_BENCH_HDD_SOURCE`, `XEMU_BENCH_HDD_IN_PLACE`, `XEMU_BENCH_LIVE_INPUT`,
  `XEMU_BENCH_RECORD_INPUT`, `XEMU_BENCH_SCREENSHOT_BACKEND`,
  `XEMU_BENCH_SCREENSHOT_INTERVAL`, `XEMU_BENCH_SCREENSHOT_START_DELAY`,
  `XEMU_BENCH_SAVEVM_AT`, `XEMU_BENCH_SAVEVM_TAG`, `XEMU_BENCH_LOADVM_TAG`,
  `XEMU_BENCH_LOADVM_AT`, `XEMU_BENCH_EXTRA_QEMU_ARGS`,
  `XEMU_BENCH_ALLOW_EXISTING`,
  `XEMU_BENCH_SURFACE_SCALE` (injects `[display.quality] surface_scale = N`
  into the per-run config; used for the GL renderer-load A/B test
  documented in `benchmarks/2026-05-01-gl-vs-metal-decision.md`).

When adding a new flag, also extend `extract-perf-summary.sh` and
`automation.md` so the value shows up in summaries and is documented.

### Planned `XEMU_METAL_*` flags (not yet implemented)

The 2026-05-02 Metal planning session designed (but did not implement)
the following flag family. They will land alongside the Metal renderer
slices in `docs/apple-silicon/metal-renderer-plan.md` §3.1; do not add
them to the codebase ahead of the corresponding slice:

- `XEMU_METAL_FORCE_LEGACY_PRESENT={0,1}` — **LANDED M10 2026-05-02.**
  Force `presentDrawable:` only (no `atTime:`); for comparison /
  diagnosis. Default 0. See the "Stable opt-in" section above for the
  full description.
- `XEMU_METAL_DISABLE_FRAMEBUFFER_FETCH={0,1}` — **LANDED M7
  2026-05-02.** Forces `pgraph_mtl_heap_supports_framebuffer_fetch()`
  to return false even on Apple Silicon. The barrier-based pass-split
  fallback itself remains a stub (Apple Silicon Mac targets always
  have framebuffer fetch via Apple7+ ⊃ Apple1; building the pass-split
  logic against an unverifiable Intel target was skipped). The flag
  flips the Apple1+ detection result so future Intel-Mac fallback
  work can exercise it. Default 0. See docs/apple-silicon/automation.md.
- `XEMU_METAL_FORCE_PASSTHROUGH={0,1}` — **LANDED M7 2026-05-02.**
  Forces every Metal-renderer draw onto the M3/M4 hand-coded
  passthrough pipeline. Skips the M7 state-to-PipelineKey build and
  the translated-pipeline cache lookup entirely. Bisection knob.
  Default 0. See docs/apple-silicon/automation.md.
- `XEMU_METAL_TRANSLATED_PIPELINE={0,1}` — **LANDED M7 2026-05-02;
  FUNCTIONAL M7.1 2026-05-02.** Opt-in for the translated pipeline
  encode path. With M7.1 the flag now flips the actual encode: when
  set to `1`, every eligible draw goes through the spirv-cross-built
  MTLRenderPipelineState with std140-packed VSH/PSH UBOs (uniform.c
  std140 packer) + per-stage texture/sampler bindings populated from
  PGRAPHState (texture_pg.c). The lookup-and-build cache warmup the
  M7 path advanced is the same; M7.1 connects it to the encoder.
  When the lookup fails (translator failure or pipeline build error)
  the renderer falls back to the M3/M4 hand-coded passthrough and
  increments `METAL_PIPELINE_FALLBACKS`. Default 0 (opt-in for M7.1
  user testing); M8 is the natural place to flip default-on once the
  async-compile path lands and cold-launch latency is acceptable.
- `XEMU_METAL_ASYNC_PIPELINE_COMPILE={0,1}` — **LANDED M8 2026-05-02
  (Path B).** Overrides the async pipeline-compile auto-default.
  Apple Silicon system builds default to ON. When ON, a cache miss
  in `pgraph_mtl_shaders_get_pipeline_ex` transitions the entry to
  PENDING and dispatches the GLSL→MSL+library+pipeline build to a
  private serial concurrent dispatch queue at QoS_UTILITY backed by
  `setShouldMaximizeConcurrentCompilation:YES` on the device (so
  Metal parallelizes compile across CPU cores). The renderer thread
  returns immediately. With `XEMU_METAL_TRANSLATED_PIPELINE=1`,
  PENDING means "skip the draw this frame" (RPCS3 PR #4876 pattern,
  identical in spirit to the GL renderer's
  `XEMU_PGRAPH_ASYNC_SHADER_COMPILE`). With the translated pipeline
  off (default), PENDING falls through to the M3/M4 passthrough so
  the cache warms in the background but the encode never depends on
  a build being ready. Set `=0` to fall back to the M5/M6/M7/M7.1
  synchronous compile (block the renderer thread for 5-50 ms per
  fresh shader pair) — useful for diff'ing skip-the-draw artifacts
  against the no-async baseline. Counters
  `METAL_SHADER_COMPILE_QUEUED_TOTAL` /
  `METAL_SHADER_COMPILE_COMPLETED_TOTAL` /
  `METAL_SHADER_COMPILE_FAILED_TOTAL` /
  `METAL_DRAWS_SKIPPED_PENDING_TOTAL` /
  `METAL_DRAWS_USING_UBERSHADER_TOTAL` (last reserved for M8.1) all
  surface on the `xemu-perf:` interval line. **Path A (the full
  Dolphin-style hybrid ubershader) is deferred** to a follow-up
  slice (M8.1) — see decision-log "2026-05-02: Metal slice M8".
  Companion fix: M6's `[cb waitUntilCompleted]` after every texture
  blit is replaced by a GPU-side `MTLSharedEvent` fence so per-draw
  command buffers `encodeWaitForEvent:` instead of the CPU
  blocking. No env var gates the upload-fence change — it's a
  correctness-equivalent replacement for the synchronous wait.
- `XEMU_METAL_DISABLE_LOSSLESS_COMPRESSION={0,1}` — allocate render
  targets `Shared` instead of `Private` (disables hardware lossless
  compression); for correctness-vs-perf bisection. **NOT IMPLEMENTED**:
  M2 shipped before this flag was wired (default Private allocation
  is already in `pgraph_mtl_heap`). Defer to a follow-up slice once a
  perf-vs-correctness motivating case shows up; until then the
  renderer is hardcoded to Private for color/depth.
- `XEMU_METAL_PIPELINE_CACHE={0,1}` — **LANDED M9 2026-05-02.**
  Persistent MSL-source disk cache for Metal pipelines, default ON.
  See the "Stable opt-in" section below for the full description.
- `XEMU_METAL_CAPTURE=path.gputrace` — **LANDED M13 2026-05-02.**
  Programmatic frame capture via `MTLCaptureManager`. See the
  "Stable opt-in" section above for the full description.
- `XEMU_METAL_VALIDATION={0,1}` — **LANDED M14 2026-05-02.** Opt-in
  Metal API validation; promotes `MTL_DEBUG_LAYER=1` before the first
  `MTLCreateSystemDefaultDevice()` call. Default 0. See the "Stable
  opt-in" section above for the full description.
- `XEMU_METAL_MSAA={0,2,4,8}` — **LANDED M11 2026-05-02.** Metal-side
  equivalent of `XEMU_GL_MSAA`. Default 0 (off) for now (see the
  "Stable opt-in" section above for the full description); lift to 4×
  default once warm-launch shader-compile cost with M9 cache hits is
  empirically below 200 ms total on PGR2 / Rainbow / Crimson.
- `XEMU_METAL_FX_SCALE={1,2,3}` — **LANDED M12 2026-05-02.**
  MetalFX spatial upscale factor
  (default 1 = no upscale). Lands with slice M12.

`XEMU_MACOS_NATIVE_INPUT={0,1}` SHIPPED 2026-05-03 (slices N1 + N2;
see the "Stable opt-in" section above and
`docs/apple-silicon/macos-input-research.md` for the full migration
plan and slices N3-N6 followups). The remaining
`XEMU_MACOS_NATIVE_INPUT_RUMBLE` / `XEMU_MACOS_NATIVE_INPUT_QUEUE`
sub-toggles described in `macos-input-research.md` §6 are still
planned-only; they will land with the N4 (Core Haptics rumble)
slice.

## Commit and PR conventions

- Stay on the `apple-silicon-performance` branch unless rebasing.
- One concept per commit, with a short imperative subject. Existing fork
  commits use that style (`Add Apple Silicon performance instrumentation`,
  `Add benchmark input recording`, `Document retail gameplay performance
  targets`, etc.).
- For QEMU upstream patches, follow QEMU's checkpatch / Signed-off-by /
  style requirements. For Apple-Silicon-only fork commits those are
  recommended but not enforced.

## When work crosses subsystems

- Touching the renderer? Update `docs/apple-silicon/research.md` if a new
  source reference is relevant, and add a benchmark note. Do not change
  Vulkan or generic QEMU display code first; xemu uses `-display xemu` and
  a custom NV2A PGRAPH path (see `system/vl.c` references in
  `research.md`).
- **Touching the Metal renderer (`hw/xbox/nv2a/pgraph/mtl/`)?** Cross-
  reference the slice in `docs/apple-silicon/metal-renderer-plan.md` and
  honor that slice's exit/validation gate. Each Metal slice has a
  specific gate (e.g. M4 needs paired PGR2 mid-route benchmark with
  visual diff ≤ 1 % per-pixel; M5 requires the shader-validation
  harness). Add `METAL_*` counters to `extract-perf-summary.sh`.
  Append a decision-log entry when an `XEMU_METAL_*` flag flips
  default-on/off. Update `automation.md` with the flag and counter
  documentation.
- Touching the benchmark harness? Update `docs/apple-silicon/automation.md`
  and add a perf-summary key to `extract-perf-summary.sh` if you added a
  counter.
- Touching the build / packaging? Document the change in
  `decision-log.md`; macOS build/package fixes are tracked there
  explicitly. The Metal slice will need Foundation / Metal / MetalKit /
  QuartzCore framework links added to the existing Apple Silicon path
  in `meson.build` and `build.sh`; `spirv-cross` will be added as a
  Meson subproject (see `metal-renderer-plan.md` §7 Q1).
