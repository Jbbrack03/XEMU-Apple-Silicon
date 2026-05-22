# Claude Status

- Objective: cycle-20 Path A — instrument `image-blit/main.c` with early, always-on, FTP-collectable progress markers so the real-Xbox witness failure (cycle 19) becomes stage-debuggable.
- Status: **CLOSED — instrumentation shipped, real-Xbox witness path re-confirmed BLOCKED, conservative interpretation recorded.**
- Active session: cycle-20 (Claude Max via /Users/jbbrack03/.local/bin/claude-max-shell). HEAD at start = `bd9ba8cadb` (cycle-19 closure). Bounded scope complete.

## Outcome (one-paragraph)

Added a single best-effort helper `image_blit_marker(unsigned idx, const char *stage)` to `scripts/apple-silicon/xbe-tests/image-blit/main.c`, plus 13 staged calls at every meaningful checkpoint from "first line of `main()` before `xbed_init`" through "last call before `xbed_render_loop_then_capture`". Each marker writes a small `D:\image-blit-marker-NN-STAGE.txt` file via `fopen / fprintf / fclose` AND mirrors the marker through the existing `xbed_host_log_writef` channel. Local xemu-Metal validation (with and without `XEMU_GUEST_LOG=1`) confirms all 13 markers fire AND every `fopen("D:\\…","wb")` returns NULL on the ISO mount path (CD-ROM = read-only; the existing harness has been masked by xemu's renderer-side screenshot hook writing to a HOST path, not through `D:\`). Real-Xbox runs (two — one pre-Codex labels, one post-Codex Codex-shortened labels per the FATX 42-char limit) BOTH produced zero marker files in FTP-collect; chainload→FTP-back gap 22.4 s, matching the two cycle-19 attempts to within 0.1 s (third reproducibility confirmation overall). Cycle-19 hypothesis #1 (D:\ remap mismatch under `runxbe` SITE-EXEC chainload) is promoted to leading hypothesis; hypotheses #2/#3/#4 are not discriminated by this evidence. The originally-framed Path A question ("which stage fails on real Xbox") is not directly delivered — markers couldn't get off-board — but the answer is sharper: the WITNESS MECHANISM (D:\ fopen) is the blocker, not stage-specific XBE failure modes.

## Evidence preserved

Local xemu (Metal):
- `benchmark-runs/cycle20-image-blit-markers-local-metal-20260522T220904Z/` — no host-log; same v0.4 expected_fail verdict, frame 124 best match.
- `benchmark-runs/cycle20-image-blit-markers-local-metal-guestlog-20260522T221035Z/` — `XEMU_GUEST_LOG=1`; all 13 markers fire, every fopen fails.
- `benchmark-runs/cycle20-image-blit-markers-local-metal-postcodex-20260522T221959Z/` — post-Codex labels; same outcome.

Real Xbox:
- `benchmark-runs/cycle20-real-xbox-image-blit-markers-20260522T221224Z/` — pre-Codex labels (markers 09 / 12 at-limit / over-FATX-limit). chainload→FTP-back 22.4 s. Zero markers retrieved.
- `benchmark-runs/cycle20-real-xbox-image-blit-markers-postcodex-20260522T222048Z/` — post-Codex shortened labels (all ≤39 chars). chainload→FTP-back 22.4 s. Zero markers retrieved.

Each real-Xbox run dir contains `report.md`, `summary.json`, `image-blit/real-xbox/real-xbox.log`, and `orch/{pre,post}.png + verdict.json + artifacts/default.xbe`.

## Diff at session close (uncommitted)

- `scripts/apple-silicon/xbe-tests/image-blit/main.c` — 101 net added lines (helper + 13 marker calls + explanatory comments).
- `scripts/apple-silicon/xbe-tests/image-blit/bin/default.xbe` — rebuilt binary (155 648 → 159 744 B).
- `scripts/apple-silicon/xbe-tests/image-blit/image-blit.iso` — rebuilt ISO (same 720 896 B, content updated).
- `docs/apple-silicon/handoff.md` — cycle-20 entry at top (cycle-19 entry preserved below).
- `docs/apple-silicon/decision-log.md` — cycle-20 entry above the cycle-19 entry; cycle-17 / cycle-19 NOT superseded.
- `docs/apple-silicon/orchestration-state/*` — all four files updated (this one, current-cycle.md, validation-status.md, handoff-summary.md).

## Canonical docs synced

- `docs/apple-silicon/handoff.md` — cycle-20 Path A entry written; cycle-19 entry preserved.
- `docs/apple-silicon/decision-log.md` — cycle-20 entry written; cycle-19 / cycle-17 NOT superseded.
- `docs/apple-silicon/orchestration-state/*` — all four files reflect cycle-20 outcome.

## Codex validation

- Mode: `changes`. Verdict: MINOR ISSUES. One medium-severity finding (FATX 42-char basename overflow on `default_state_set` / `before_capture_loop`). Adopted in full — labels shortened to `state_set` / `pre_capture`, helper comment block re-grounded against the FATX limit with an explicit ≤14-char label budget, binary rebuilt, validation re-run on real Xbox with the corrected binary (third overall reproducibility confirmation). Validation marker at `.claude/state/codex-validate-last-run` per rule #15.

## Next bounded slice (NOT promoted this cycle)

- Path A.2: re-route marker writes to a known-writeable partition (e.g. absolute `E:\Apps\image-blit\marker-NN.txt` paths, or `T:\…` title-data) and re-run; if markers land, we learn the actual XBE execution stage on real Xbox.
- Path A.3: cross-check whether the existing `xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor}` captures came from `runxbe` SITE-EXEC chainload or from a different launch path; if `runxbe`, the leading hypothesis is image-blit-specific (not universal).
- Path B: smaller PFIFO-race-only Tier-1 diag XBE that captures via PCRTC; still on the table.

Scope choice belongs to the next Hermes pass.
