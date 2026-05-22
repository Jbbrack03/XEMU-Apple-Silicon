# Current Cycle

- Cycle: 21 Path A.2 (**CLOSED — 2026-05-22 18:30 CDT**).
- Started: 2026-05-22 18:11 CDT.
- Worker receipt posted: 2026-05-22 18:11 CDT.
- State: CLOSED (bounded scope satisfied; outcome recorded in `handoff.md` cycle-21 entry + `decision-log.md` cycle-21 entry).
- Owner: Claude Code worker (Claude Max via /Users/jbbrack03/.local/bin/claude-max-shell).
- HEAD at start: `254b888b80` (cycle-20 packaging closure).
- Cycle-20 closure commit (HEAD-1): `8daed392af`.
- Bounded goal (as assigned): re-route image-blit progress-marker writes from `D:\` to `E:\Apps\image-blit\…`; rebuild; locally validate; re-run real-Xbox oracle flow; interpret conservatively; Codex-validate; sync canonical docs; commit only intended-scope changes.
- Result: marker helper re-routed to `E:\Apps\image-blit\image-blit-marker-NN-STAGE.txt` with idempotent E:\ mount + `CreateDirectoryA` chain (same pattern as `oracle-agent/controller.c` and three other XBEs). Local xemu-Metal validation across 4 boots shipped 52 marker host-log lines with `e_mount=1` and ZERO `fopen-failed` (vs cycle-20's 52/52 fopen-failed for the D:\ baseline); v0.4 tally drift byte-identical to cycle 20 (no regression). Single bounded real-Xbox run produced a clear NEGATIVE answer: chainload→FTP-back 22.4 s (4th independent reproduction across cycle-19+cycle-20+cycle-21), `verdict.json status: ok`, FTP-collect retrieved exactly 1 file (`default.xbe` upload echo), ZERO `image-blit-marker-*` files. Cycle-19 hypothesis #1 ("D:\ remap mismatch under `runxbe` chainload is the witness-path blocker") is **demoted from leading to insufficient as a sole explanation** — re-routing to a provably-writeable, provably-retrievable E:\ path does not unblock the witness either. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on decision REMAINS DEFERRED.

## Exit criteria — final status

1. [x] Worker receipt posted to claude-status.md + current-cycle.md before deeper work (18:11 CDT).
2. [x] Bounded reviewable diff landed (~120 net lines in main.c only; comment expansion + idempotent E:\ mount + dir-create + path-buffer resize + host-log `e_mount=N` field).
3. [x] Rebuild completed successfully (default.xbe 159 744 B + image-blit.iso 720 896 B; rebuilt via `eval "$(nxdk/bin/activate -s)" && make`).
4. [x] Local validation on xemu-Metal: 4 boots, 52 marker lines via host-log channel, all `e_mount=1`, 0 fopen-failed; v0.4 tally drift `3/8 mask=0x31, 3/8 mask=0x31, 2/8 mask=0x30, 2/8 mask=0x30` byte-identical to cycle 20.
5. [x] Real-Xbox run: 1 attempt; chainload→FTP-back 22.40 s (4th reproduction of that exact gap); FTP-collect returned 1 file (`default.xbe` upload echo), 0 marker files. Yes/no answer to "does re-routing to E:\ make markers observable on real Xbox?" → **NO**.
6. [x] Conservative interpretation recorded in `handoff.md` + `decision-log.md` cycle-21 entries. Cycle-19 hypothesis #1 demoted from "leading" to "insufficient as sole explanation." Hypotheses #2/#3/#4 weight goes UP but no direct discrimination. New cycle-21 hypothesis recorded (FATX-driver / NT-mount state under `runxbe` chainload may differ from FTP-server-time state).
7. [x] Codex validation completed: mode `changes`, verdict MINOR ISSUES. Two findings (both about doc-sync completeness), both adopted in full in this same closure pass. Validation marker written at `.claude/state/codex-validate-last-run`.
8. [x] Canonical docs synced: handoff.md (cycle-21 entry on top; cycle-20 preserved below), decision-log.md (cycle-21 entry above cycle-20; cycle-17/19/20 NOT superseded), all four orchestration-state files updated for cycle 21 closure.
9. [ ] Slice committed — pending final commit step (this entry is written PRE-commit so the commit can include this file's "CLOSED" status; commit hash will be appended to handoff-summary.md after the commit lands).

## Out-of-scope (kept bounded per the assignment)

- Did NOT start Path A.3 (`xbox-real-references/` provenance audit — now the top-priority follow-up per cycle-21 evidence).
- Did NOT start Path A.4 (oracle-agent kernel-pool buffer witness — new non-fopen witness path proposed in cycle-21 entry).
- Did NOT start Path B (smaller PFIFO-race-only Tier-1 diag XBE).
- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on.
- Did NOT relocate `xbed_render_loop_then_capture` capture/done writes (still under D:\); cycle-21 was markers-only.
- Did NOT modify xemu-fork host source — instrumentation slice is XBE-only.
- Did NOT touch retail-title metrics, §G.5, RT-as-texture, or any second-wave XBE work.
- Did NOT commit the unrelated `.inl` source-path drift produced by `make clean` (reverted before Codex validation per the "commit only intended-scope" guardrail).
