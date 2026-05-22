# Current Cycle

- Cycle: 20 Path A (**CLOSED — 2026-05-22 17:25 CDT**).
- Started: 2026-05-22 17:04 CDT.
- Worker receipt posted: 2026-05-22 17:04 CDT.
- State: CLOSED (bounded scope satisfied; outcome recorded in `handoff.md` cycle-20 entry + `decision-log.md` cycle-20 entry).
- Owner: Claude Code worker (Claude Max via /Users/jbbrack03/.local/bin/claude-max-shell).
- HEAD at start: `bd9ba8cadb` (cycle-19 closure).
- Bounded goal (as assigned): instrument `scripts/apple-silicon/xbe-tests/image-blit/main.c` with early, always-on, FTP-collectable progress markers; rebuild; validate locally; run real-Xbox once; interpret marker outcome conservatively; run Codex validation; sync canonical docs.
- Result: 13 staged markers (00..12) shipped in main.c. Local xemu validation confirms all 13 fire via the `xemu-guest-log:` host channel and that every `fopen("D:\\…","wb")` returns NULL on the ISO mount path (existing harness has been masked by the renderer-side screenshot hook). Real-Xbox runs (two — once pre-Codex labels, once post-Codex shortened labels) BOTH produced ZERO marker files; chainload→FTP-back stable at 22.4 s. Cycle-19 hypothesis #1 (D:\ remap mismatch under `runxbe` chainload) is promoted to leading hypothesis; hypotheses #2/#3/#4 cannot be discriminated by this evidence alone. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on decision REMAINS DEFERRED.

## Exit criteria — final status

1. [x] Worker receipt posted to claude-status.md + current-cycle.md before deeper work (17:04 CDT).
2. [x] image-blit progress-marker instrumentation landed; bounded reviewable diff (~101 net lines in main.c only).
3. [x] Rebuild completed successfully (image-blit.iso + bin/default.xbe produced; post-Codex labels in current binary).
4. [x] Local validation on xemu-Metal: all 13 markers fire via host-log; v0.4 pass=3/8 mask=0x31 tally unchanged. Evidence under `benchmark-runs/cycle20-image-blit-markers-local-metal-{20260522T220904Z,guestlog-20260522T221035Z,postcodex-20260522T221959Z}/`.
5. [x] Real-Xbox runs: zero markers retrieved across two attempts; chainload→FTP-back 22.4 s (matches cycle-19's two attempts → three independent reproductions in total). Evidence under `benchmark-runs/cycle20-real-xbox-image-blit-markers-{20260522T221224Z,postcodex-20260522T222048Z}/`.
6. [x] Marker outcome interpreted conservatively in `handoff.md` + `decision-log.md` cycle-20 entries. Specifically NOT claimed: that the XBE crashes at any particular stage, that hypothesis #1 is universal across all `runxbe`-launched XBEs, or that hypotheses #2/#3/#4 are discriminated. Specifically IS claimed: that `D:\` write-back on the `runxbe` chainload path is blocked for image-blit irrespective of XBE stage or basename length.
7. [x] Codex validation completed: mode `changes`, verdict MINOR ISSUES, one medium-severity FATX-overflow finding adopted in full (labels shortened, rebuilt, re-validated). Validation marker at `.claude/state/codex-validate-last-run`.
8. [x] Canonical docs synced: handoff.md, decision-log.md, and all four orchestration-state files updated to reflect the cycle-20 actual outcome.
9. [x] Slice committed as `8daed392af` (the cycle-20 closure commit). Tree clean at session end.

## Out-of-scope (kept bounded per the assignment)

- Did NOT start Path B (smaller PFIFO-race-only Tier-1 diag XBE).
- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on.
- Did NOT broaden into unrelated renderer work.
- Did NOT modify retail-title metrics or §G.5 / RT-as-texture / second-wave XBE work.
- Did NOT investigate alternative write-back partitions (Path A.2) or cross-check existing `xbox-real-references/` provenance (Path A.3). Both belong to a future Hermes pass.
- Did NOT modify xemu-fork host source — instrumentation slice is XBE-only.
