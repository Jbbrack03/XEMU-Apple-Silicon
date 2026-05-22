# Validation Status

- Active slice: cycle-14 GL replay for the §H.6 `image-blit` residual.
- Validation state: **CLOSED — bounded GL replay executed; outcome documented; cell-by-cell pattern comparison against Metal not achievable in this slice; next-slice recommendation in `current-cycle.md`.**

## Planned gates for this slice

- [x] Fresh worker receipt posted after canonical-doc read (see `current-cycle.md` §"Worker receipt").
- [x] GL replay executed through the existing harness with artifacts/logs sufficient to characterize what the GL leg produces vs the prior Metal result. Four runs total: one sidecar `macos-capture.sh` attempt (failed on macOS Screen Recording TCC gate) and three in-renderer `XEMU_GL_SCREENSHOT_PATH` + `XEMU_CAPTURE_AT_FLIP_STALL` captures at ordinals 120 / 280 / 340 / 450.
- [x] Canonical docs/state updated with the GL outcome and the resulting conclusion. Outcome: the GL in-renderer screenshot path consistently captures a pre-dashboard "boot-logo-class" frame, regardless of the chosen flip-stall ordinal. fs=280 and fs=340 from independent xemu runs are byte-identical; fs=450 (post-reboot) differs only marginally. The image-blit dashboard, present on Metal v0.3 baseline frame 0124, never lands on the GL front-surface lookup in this experiment. Consequently the cycle-13 PFIFO ↔ vCPU dispatch-race hypothesis is **neither confirmed nor falsified** by the bounded GL leg.
- [x] No code changes were made; Codex validation is **not** mandatory for this slice.
- [x] Tree left clean and the slice-close commit explains the change set.

## Notes

- This was a bounded validation slice. The empirical finding (GL capture-path stuck on the early surface) is not by itself evidence for or against the dispatch-race framing; it just means the chosen experiment couldn't decide the question. A separate, bounded, source-touching slice is required to make the renderer-agnostic claim testable. See `current-cycle.md` §"Next-slice recommendation" for the two candidate follow-ups (GL sequence capture, or guest-side per-cell oracle output channel).
- Any future source-touching slice triggered from this finding will be non-trivial and so Codex validation will be mandatory before close.
- Real-Xbox / oracle validation remains a later gate for any claim that goes beyond the renderer-agnostic confirmation experiment, irrespective of this slice's verdict.
