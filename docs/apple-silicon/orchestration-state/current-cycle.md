# Current Cycle

- Cycle: 14 (CLOSED — GL replay of the §H.6 `image-blit` residual, inconclusive for the renderer-agnostic-race hypothesis but informative about a GL-side capture gap).
- Started: 2026-05-22 11:42 CDT.
- Closed: 2026-05-22 11:58 CDT.
- Closure commit: `f6273c9cba`.
- State: CLOSED / GL replay executed, dashboard never reached the GL front surface in any of four flip-stall ordinals.
- Owner: Claude Code (hermes_xemu_live_20260522_114212).
- Bounded goal: Run the existing `image-blit.iso` harness flow under `XEMU_RENDERER=GL` to test whether the cycle-13 PFIFO ↔ vCPU dispatch-race hypothesis reproduces renderer-agnostically on GL.
- Why this slice exists: cycle 13 closed cleanly and the canonical next highest-value step is the cheapest confirmation experiment before any diagnostic implementation work.

## Worker receipt (posted 2026-05-22)

- Docs read: orchestration-state files (`current-cycle.md`, `claude-status.md`, `validation-status.md`, `handoff-summary.md`), `orchestration-workflow.md`, `xbe-tests/image-blit/README.md`. `handoff.md` is 576 KB and exceeds Read limits; will sample with targeted reads only if needed since orchestration-state is canonical for this slice.
- Bounded objective: one GL-leg replay of `image-blit` through `scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run --xbe image-blit --renderer gl`, compared cell-by-cell against Metal v0.3 baseline `xbe-harness-20260522-090124/image-blit/metal/`.
- Current hypothesis: the residual 5-of-8 mismatch is a PFIFO ↔ vCPU dispatch race; GL should show the same first-mismatch signature (got=sentinel, expected=RED, `(mx=0,my=0)`) on the failing cells. A materially different pattern falsifies the renderer-agnostic framing.
- First concrete action: invoke the `xbe-harness` orchestrator GL leg with no source edits, fresh output dir.
- Planned validation path: decode the GL leg's v0.3 dashboard 2×2 sub-rect, read `signal_match_pct` / `changed_pixels_pct`, pull per-cell `image-blit: cell N …` lines from `xemu.log`, then update durable state with the three-outcome decision (identical / partial-divergent / falsified).
- No source changes planned; Codex validation not mandatory for this slice unless scope changes.

## GL replay outcome (2026-05-22)

Four bounded `xbe_orchestrator.py run --xbe image-blit --renderer gl` invocations were executed at successive `XEMU_CAPTURE_AT_FLIP_STALL` ordinals, with `XEMU_GL_SCREENSHOT_PATH` directing the in-renderer GL screenshot path into each run's `screenshots/` dir:

| Run dir | flip_stall ordinal | PNG md5 | Verdict |
|---|---|---|---|
| `xbe-harness-20260522-164520-cycle14-gl/` | — (sidecar `macos-capture.sh`) | n/a | infra fail: `screencapture -l <wid>` could not create image from window (macOS Screen Recording permission). |
| `xbe-harness-20260522-164833-cycle14-gl-flipstall/` | 120 (interval 3, pre-60-fps) | `242998a0…018` | Captured boot-logo state; non-near-black pixels concentrated centrally; dashboard not visible. |
| `xbe-harness-20260522-165223-cycle14-gl-fs280/` | 280 (cum mid-interval-6, 60 fps zone) | `74db95fa…469` | Same centred boot-logo content; max pixel `(168,199,6)` at `(353,210)`. |
| `xbe-harness-20260522-165521-cycle14-gl-fs340/` | 340 (cum end-of-interval-7, last 60 fps window) | `74db95fa…469` | **Byte-identical** to fs=280 across two separate xemu runs. |
| `xbe-harness-20260522-165726-cycle14-gl-fs450/` | 450 (well past post-XBE reboot) | `f11a5a6e…880` | Same centred boot-logo signature; slightly different bytes (e.g. animation-frame drift), same brightness pattern. |

Per-interval observations (perf log):
- intervals 0–5 (~38 fps): nxdk boot + 8 IMAGE_BLIT dispatches + per-cell oracle CPU work + shader compile (100 ms in interval 0).
- intervals 6–7 (60–61 fps): cumulative flip_stall jumps 232 → 353 — this is the XBE's `xbed_render_loop_then_capture(n_frames=300)` window for the dashboard at full vblank.
- interval 8 (7.2 s gap, 5 flips): `Sleep(1500)` + XOSS write + `Sleep(500)` + `HalReturnToFirmware` reboot sequence.
- intervals 9–11 (low fps): post-reboot xemu/UnleashX idling at 4–10 fps.

Despite the flip_stall counter reaching 353 inside the dashboard window, the in-renderer GL screenshot **never captures the image-blit dashboard**. fs=280 and fs=340 are byte-identical PNGs from independent xemu runs at different ordinals, and fs=450 (post-reboot) is only marginally different. The Metal baseline `xbe-harness-20260522-090124/image-blit/metal/screenshots/image-blit.0124.png` shows the expected 4×2 verdict grid (3 cells PASS-green, 5 cells with v0.3 2×2 sub-rect FAIL encoding) — so the comparator artefact exists.

## Interpretation

This experiment does **not** confirm or falsify the cycle-13 PFIFO ↔ vCPU dispatch-race hypothesis. The reason is upstream of the hypothesis test: the GL renderer's `pgraph_gl_capture_display_if_requested` path (`hw/xbox/nv2a/pgraph/gl/display.c:466`) consistently returns the same pre-dashboard frame regardless of when the flip-stall trigger fires inside the XBE's render loop. Whether that is because:
- the XBE's draws to its allocated VRAM buffers are not landing in the surface the GL display lookup picks (the boot-logo / pbkit pre-allocated front-buffer surface);
- the GL surface cache holds a stale entry for the displayed VRAM page across the entire XBE run;
- or the front-buffer page is in fact being updated but the captured drawable is read pre-flip-commit;

is not determined by this slice. None of those alternatives can be distinguished without source-level instrumentation that would push past the bounded-slice scope (which would in turn trigger the workspace rule #15 mandatory Codex gate).

The empirical observation that GL captures three byte-identical or near-identical frames at flip_stall ordinals spanning the XBE render window AND the post-reboot UnleashX window is itself a non-trivial finding about the GL in-renderer screenshot path for diagnostic XBEs; it does not by itself support a PFIFO ↔ vCPU dispatch-race claim, because it is just as compatible with a renderer-side display-surface lookup issue.

## Next-slice recommendation

Two cheap follow-ups are tractable; each is a small, scoped, separate cycle:

1. **GL sequence capture** — extend the GL in-renderer screenshot path to mirror `XEMU_METAL_SCREENSHOT_AT_FRAME` + `XEMU_METAL_SCREENSHOT_INTERVAL` semantics (write a numbered PNG sequence instead of single-shot). Re-run image-blit on GL and confirm/deny whether the dashboard is ever visible at any frame. This decides whether the right answer is "GL never paints the dashboard" or "GL paints the dashboard at flip ordinals we didn't try." Source change in `gl/display.c` only; non-trivial, so Codex required.
2. **Guest-side oracle output channel** — surface the XBE's per-cell `image-blit: cell N PASS|FAIL` debugPrint output to xemu stderr so the host harness can record per-cell verdicts independent of any host-side visual oracle. This bypasses the GL screenshot problem entirely for Tier-2 XBEs and makes the renderer-agnostic question testable without first solving the GL capture-path question. Source change in xemu's debug-print / KdReportSystemMessage path; non-trivial, so Codex required.

Either gives a renderer-agnostic answer to the cycle-13 hypothesis without committing to the `NV_PGRAPH_STATUS` implementation up-front. Option 2 is cheaper to validate and more useful long-term for the diagnostic-XBE library.
- Exit criteria:
  1. Post the worker receipt with docs read, bounded objective, current hypothesis, first concrete action, and planned validation path.
  2. Run one bounded GL-leg replay of `image-blit.iso` through the existing harness with artifacts sufficient to compare against the prior Metal result.
  3. Update the canonical docs/state with the observed GL result, the conclusion about whether the race looks renderer-agnostic, and the next recommended slice.
  4. Keep scope inside the workspace; do not begin the diagnostic `NV_PGRAPH_STATUS` implementation in this session unless the docs are first updated to justify a slice change.
  5. Leave the tree either cleanly committed for a completed doc/validation slice or in a tightly explained state with doc sync complete.
