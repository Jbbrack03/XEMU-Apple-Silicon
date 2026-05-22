# Claude Status

- Objective: cycle-14 bounded GL replay for the §H.6 `image-blit` residual.
- Status: **CLOSED — GL replay executed, slice closed with inconclusive verdict on the renderer-agnostic-race hypothesis and a clear next-step recommendation.**
- Session: `hermes_xemu_live_20260522_114212`

## Outcome (2026-05-22, slice closed)

- Four GL replays executed (one sidecar `macos-capture.sh`, three flip-stall-triggered in-renderer screenshots at ordinals 120 / 280 / 340 / 450).
- The sidecar path failed on the macOS Screen Recording permission gate; the in-renderer GL screenshot path produced PNGs but each one captured a pre-dashboard "boot-logo-class" frame. fs=280 and fs=340 from two independent xemu runs are **byte-identical**; fs=450 (deep post-reboot) is only marginally different.
- The image-blit dashboard, which renders correctly on the Metal renderer (3-of-8 PASS pattern preserved across the v0.3 captures), **never appears** on the GL in-renderer screenshot path at any tested ordinal.
- Conclusion: this experiment cannot confirm or falsify the cycle-13 PFIFO ↔ vCPU dispatch-race hypothesis. The blocker is upstream — the GL renderer's in-renderer display-capture path for diagnostic XBEs returns a stale or wrong surface, so cell-by-cell pattern comparison against Metal is impossible.
- Empirical artifact set: `benchmark-runs/xbe-harness-20260522-{164520,164833,165223,165521,165726}-cycle14-gl-*` (xemu logs + screenshots + harness summaries).
- Next-slice recommendation captured in `current-cycle.md` §"Next-slice recommendation": either (a) extend GL in-renderer screenshot to a numbered sequence mirroring Metal's frame/interval semantics; or (b) add a guest-side per-cell oracle output channel that bypasses the visual oracle. Option (b) is preferred — cheaper to validate and broadly useful for Tier-2 XBEs.

## Receipt (posted 2026-05-22)

- Docs read: orchestration-state quartet, `orchestration-workflow.md`, `xbe-tests/image-blit/README.md`; auto-loaded `flags-bench.md` + `oracle-and-xbe.md`. `handoff.md` deferred to targeted reads (576 KB > Read cap).
- Bounded objective: one GL-leg replay of `image-blit` via `scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run --xbe image-blit --renderer gl`, compared cell-by-cell vs Metal v0.3 baseline `xbe-harness-20260522-090124/image-blit/metal/`.
- Current hypothesis: residual 5-of-8 image-blit mismatch is a PFIFO ↔ vCPU dispatch race; GL should reproduce the same first-mismatch signature (got=sentinel, expected=RED, `(mx=0,my=0)`) if renderer-agnostic.
- First concrete action: invoke the orchestrator GL leg with no source edits; output under a fresh `benchmark-runs/xbe-harness-<UTC>/image-blit/gl/`.
- Planned validation path: decode v0.3 dashboard 2×2 sub-rect, read harness `signal_match_pct` / `changed_pixels_pct`, pull per-cell `image-blit: cell N …` lines from `xemu.log`, then update durable state with the three-outcome verdict.
- Codex validation: not mandatory for this slice (no source edits planned).

## Intended slice

- Read the canonical Apple-Silicon xemu docs first, then post a short worker receipt before deeper investigation.
- Validate or falsify the cycle-13 hypothesis that the remaining `image-blit` failures are caused by a PFIFO ↔ vCPU dispatch race that should reproduce on GL as well as Metal.
- Preferred evidence: fresh harness artifacts and compact doc updates, not long transcript tails.

## Receipt requirements (must appear early in the session)

- Docs read.
- Bounded slice objective.
- Current hypothesis.
- First concrete action.
- Planned validation path.

## Guardrails

- One bounded slice only.
- Stay inside the workspace.
- Treat Codex as mandatory if the slice turns into non-trivial implementation work.
- Treat real-Xbox/oracle validation as still required for any meaningful graphics-quality claim beyond this diagnostic GL replay.
