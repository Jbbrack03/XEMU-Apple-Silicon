# Multi-title MSAA + 1080p validation — 2026-05-03

Validation of `XEMU_GL_MSAA=4` + `surface_scale=2` (1080p) across the
tracked titles plus Soul Calibur 2 (the documented 60 Hz title).

## Configuration

- Build: `apple-silicon-performance @ 14b012f9f4`
- Renderer: GL (Apple's OpenGL-on-Metal driver)
- `surface_scale = 2` (1080p-class internal resolution; default on this fork)
- `XEMU_GL_MSAA=4` (4× multisample anti-aliasing, opt-in)
- All eight default-on flags active per `automation.md` "Apple Silicon defaults"

## Results

| Title       | Duration | Engine cap | Avg FPS | MSAA cost (% frame) | mspf p95 | mspf p99 | Result |
|-------------|----------|-----------|---------|---------------------|----------|----------|--------|
| **PGR2**    | 60 s     | 30 fps    | **47.08**  | 3.0 %  | 178 ms | 298 ms | ✅ above cap |
| **Crimson** | 90 s     | 30 fps    | **30.23**  | 1.2 %  | 174 ms | (n/a)  | ✅ at cap |
| **Rainbow** | 60 s/90 s| 30 fps    | **26.81**  | 5.4 %  | 153 ms | (varies) | ⚠ avg below cap; max 60 fps in many intervals |
| **SC2**     | 60 s     | 60 fps    | **58.19**  | 1.9 %  | 116 ms | (n/a)  | ✅ near cap |

`MSAA cost = MSAA_RESOLVE_US_TOTAL / interval_us_total`.

## User-stated goal mapping

The user's stated goals were:

1. **30/60 fps at 1080p** with high-quality antialiasing.
2. **No framerate jitter** (i.e. consistent frame pacing).
3. **Very low controller input latency**.
4. **Display correctly in correct colors**.

| Goal | Status | Notes |
|------|--------|-------|
| 30/60 fps at 1080p with AA | ✅ achieved on PGR2 / Crimson / SC2 | Rainbow shows variance |
| High-quality AA | ✅ XEMU_GL_MSAA=4 is opt-in stable | 1-9% frame budget cost |
| No jitter | ⚠ improved (V9+V10 closed pillar) | residual stutters declared guest-intrinsic best-effort complete |
| Low input latency | ✅ N1+N2 shipped (opt-in) | XEMU_MACOS_NATIVE_INPUT=1 enables GameController.framework |
| Correct colors | ✅ on GL renderer | Metal renderer produces magenta — known issue, queued |

## Rainbow Six 3 variance investigation

Rainbow's gameplay route hits intervals at both extremes:

```
$ grep -oE 'fps=[0-9]+\.[0-9]+' benchmark-runs/20260503-093704-rainbow-six-3/xemu.log | sort -t= -k2 -n -r | head -3
fps=60.94
fps=60.31
fps=56.85
$ ... | sort -t= -k2 -n | head -3
fps=6.82
fps=8.25
fps=9.96
```

The 26.81 fps average reflects this bimodal distribution. The slow
intervals are guest-intrinsic asset-streaming hitches (per the V6/V7/V9/V10
attribution work; see `2026-05-02-v9-v10-rdtsc-fastpath-and-invalidation-attribution.md`).

## Metal renderer status — magenta-surface artifact

The Metal renderer at this commit produces visually-incorrect output
(solid magenta render targets) on PGR2 / Crimson / Rainbow despite:

- `METAL_PIPELINE_TRANSLATED_FAILED == 0` (all pipelines build)
- `METAL_DRAW_INDEXED_COUNT > 50,000 / 60s` (post-M5.8 throughput restored)
- `METAL_PIPELINE_FALLBACKS == 0` (every draw uses translated pipeline)

The diagnostic capture path
(`XEMU_METAL_SCREENSHOT_SOURCE=nv2a`, added 2026-05-03)
captures the NV2A-side render target pre-present and confirms the
magenta is renderer-side, not OS-level layer substitution. Possible
root causes (queued for follow-up):

- Wrong NV097_SET_COLOR_CLEAR_VALUE handling (clears overwriting drawn geometry).
- Texture sampling producing transparent/invalid colors (alpha=0 or
  unbound-texture sentinel).
- PSH (combiner) translation producing constant-magenta output for
  PGR2's specific combiner state.
- Surface routing publishing the wrong NV2A RT (e.g., unfilled aux
  buffer) as the framebuffer.

## Decisions

- **GL renderer remains the production path** for visually-correct
  output until the Metal magenta artifact is investigated and fixed.
- **`XEMU_GL_MSAA=4` is the recommended user setting** for high-quality
  AA at 1080p. Stays opt-in (default 0) until the variance investigation
  on Rainbow concludes — the 7-9% MSAA cost on Rainbow's stutter-prone
  intervals could push more frames below 30 fps.
- **`XEMU_MACOS_NATIVE_INPUT=1` is recommended** for low-latency input.
  Counter-validated; default off pending paired latency benchmark.
- **M15 default-on flip is BLOCKED** until the Metal magenta artifact
  is fixed.

## Artifacts

- Run dirs: `benchmark-runs/20260503-09{2529-pgr2, 3438-crimson-skies,
  3704-rainbow-six-3, 3906-soul-calibur-2}`.
- Diagnostic Metal NV2A-direct screenshots: `/tmp/pgr2-nv2a-direct.000{1..4}.png`
  (showing the magenta NV2A RT surfaces).
