# Handoff Summary

- Cycle: 2026-05-22, Hermes-supervised cycle 5 (task #16 closure slice).
- Owner: Claude Code.
- Outcome: **Exit Option A — Task #16 CLOSED.** swizzle-mipmap PASSes byte-exact on Metal (`changed_pixels_pct=0.0000`, `signal_match_pct=100.0000`). First-wave Metal XBE PASS count: **15/17 → 16/17**. Zero regressions across 11 other XBEs exercised (3 LU_IMAGE_ xbed_texture users, 7 non-texture diag XBEs, mirror, color-channel, combiner-basic, blend-matrix). One deterministic pre-existing FAIL (pipeline-smoke) confirmed unrelated — pipeline-smoke is Tier-4 CPU-painted, no PGRAPH, no textures; root cause is xemu mutating `surface_scale=2` from harness's `surface_scale=1` toml. Tree state at closure: 3 source files modified (`hw/xbox/nv2a/pgraph/mtl/texture_pg.c`, `scripts/apple-silicon/xbe-tests/lib/xbed_texture.c`, `scripts/apple-silicon/xbe-tests/swizzle-mipmap/manifest.json`) + 4 rebuilt XBE binary sets + 6 doc updates (handoff cycle-5 banner, decision-log cycle-5 entry, 4 orchestration-state files). Codex validation completed with MINOR ISSUES (1 LOW finding adopted via revert of cosmetic `.inl` path-noise; 1 open question deferred to a documented follow-up slice). Ready to commit; do NOT push to origin.
- Key changes shipped (durable):
  1. **Metal renderer non-cubemap-2D `s.border` 2x-upload**, in
     `hw/xbox/nv2a/pgraph/mtl/texture_pg.c::decode_face_levels` +
     `pgraph_mtl_texture_bind_from_pg`. Mirrors `gl/texture.c:451-456`
     and `vk/texture.c:111`. The fix path covers: doubled VRAM read +
     doubled MTLTexture allocation in `decode_face_levels`; consistent
     adjusted_width/height in cache-key lookup
     (`pgraph_mtl_texture_bind_slot_cached_full`); adjusted
     texture_length for dirty-range download/invalidate; surface
     fast-path guard so bordered textures never alias a flat RT view
     (which would emit wrong pixel data since RT surfaces don't carry
     the doubled-with-border VRAM layout).
  2. **xbed_texture library `BORDER_SOURCE_COLOR` default**, in
     `scripts/apple-silicon/xbe-tests/lib/xbed_texture.c::xbed_texture_bind_stage0`.
     One-line `fmt |= XBED_FMT_BORDER_SOURCE_BIT;` so the composed
     format word matches the nxdk `samples/mesh/main.c:145` reference
     `0x0001122a` (bit 3 = 1 = `BORDER_SOURCE_COLOR`). Forces `s.border = false`
     for current library callers so they no longer accidentally trip
     `psh.c::apply_border_adjustment`. The 3 LU_IMAGE_ users are
     unaffected behaviorally (linear path skips the bordered UV
     transform regardless of `s.border`); only `swizzle-mipmap` flips.
  3. **swizzle-mipmap manifest update**: `expected_fail_renderers`
     from `["xemu/gl", "xemu/metal"]` to `["xemu/gl"]` + rewritten
     `expected_fail_notes`. The GL leg remains expected_fail under
     task #17 (LOD-clamp regression — `pgraph_get_texture_shape`
     truncates `levels` and uploads wrong data for cells 1-6).
  4. **4 XBE binary sets rebuilt** (`.iso` + `bin/default.xbe` +
     `main.exe` + `main.obj` for swizzle-mipmap + texture-format-sweep
     + texture-filter-wrap + texture-dma-ab). The shared
     `xbed_texture.c` is included via `lib/lib.mk`.
- Prior narrative superseded: 2026-05-21 (Hermes cycle 4)'s "Net next-highest-value actions" item 1 "(c): ship both — preferred — principled" is now the implemented decision; the fix scope is no longer speculative.
- New tooling shipped: NONE (no new env flags or counters — the slice ships only the two functional fixes + the rebuilt binaries + doc/manifest updates).
- Validation evidence (durable, all under `benchmark-runs/`):
  - `20260522T054316Z-task16-swizzle-mipmap-validation/` — swizzle-mipmap on Metal canonical PASS byte-exact.
  - `20260522T054422Z-task16-xbed-texture-regress/` — 3 LU_IMAGE_ xbed_texture XBEs PASS on Metal.
  - `20260522T055631Z-task16-wider-regress/` — 7/7 non-texture XBEs PASS on Metal (msaa-aa-factor not-built — pre-existing).
  - `20260522T054657Z-task16-renderer-regress-smoke/` + `20260522T055508Z-task16-pipeline-smoke-recheck/` — pipeline-smoke FAIL pre-existing harness/config issue, documented.
- Next slice's highest-value experiments (suggested; not binding):
  1. **Author §4.13 `texture-shader-stages`** — last unstarted first-wave XBE. 19 NV2A texture-shader modes; significant scope; needs combiner-helper + texture-shader-stage infrastructure. Closes M15 default-on prerequisite (per `metal-renderer-plan.md` §M15 + 2026-05-20 evening XBE-first methodology pivot).
  2. **Author a dedicated `swizzle-bordered` XBE** that intentionally sets `BORDER_SOURCE = TEXTURE` with 128x128 swizzled VRAM data; guards the Metal renderer's just-fixed bordered-texture path against regression. Consider exposing `BORDER_SOURCE` as a field on `XbedTextureStage0` per Codex's open question.
  3. **Investigate Task #17 (GL LOD-clamp regression)** — separate slice. Likely lives in `gl/texture.c` per-mip upload when `s.levels < 7` due to the `pgraph_get_texture_shape::levels = MIN(levels, max + 1)` clamp; uploads/samples wrong data for cells 1-6 in swizzle-mipmap on GL.
  4. **Investigate pipeline-smoke `surface_scale=2` leak** — harness / xemu.toml interaction; pre-existing, not regressed by this slice but worth a fix slice for harness hygiene.
- Codex review state: COMPLETED. Verdict: MINOR ISSUES.
  - LOW (adopted): `lib/vs.inl` + `lib/xbed_tex_vs.inl` had workstation-absolute path comments from the build path; reverted via `git checkout --` since shader bytecode is identical.
  - OPEN (deferred): expose `BORDER_SOURCE` on `XbedTextureStage0` for future bordered XBE — captured as a follow-up; today the hardcoded COLOR default matches nxdk samples/mesh and unblocks the four current users.
  - OUT OF SCOPE (Codex didn't verify): pipeline-smoke attribution — I confirmed independently via deterministic re-run.
  - Marker `~/.claude/state/codex-validate-last-run` written post-finding-adoption.
