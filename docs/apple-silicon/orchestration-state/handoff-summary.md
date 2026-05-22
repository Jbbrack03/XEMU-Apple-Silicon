# Handoff Summary

- Cycle 17 is CLOSED. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` shipped opt-in and validated renderer-agnostically.
- image-blit.iso flips from cycle-15's `pass=3/8 mask=0x31` to `pass=8/8 mask=0xff` on Metal (4 boots) AND GL (15 boots) under the flag; the cycle-13 PFIFO ↔ vCPU dispatch-race hypothesis is now definitively confirmed.
- Two failed eligibility-gate iterations are preserved as evidence — a naive `PUT != GET` check hangs the BIOS / pbkit; the diagnostic must mirror `pfifo_run_pusher`'s full stall set.
- Codex `/codex-validate changes` returned MINOR ISSUES; both adopted in this slice (docs control-plane sync + harness README update).
- Next bounded slice (cycle 18): cycle-11 follow-up item #3 — real-Xbox oracle parity check on `image-blit.iso` under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1`. Gates the default-on / long-term-fix decision (properly published busy bit vs. default PFIFO barrier vs. just shipping the diagnostic).
