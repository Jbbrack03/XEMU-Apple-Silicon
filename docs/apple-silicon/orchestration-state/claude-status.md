# Claude Status

- Objective: cycle-21 Path A.2 — re-route image-blit progress-marker writes from `D:\` (proven blocked under `runxbe` SITE-EXEC chainload across three cycle-19+cycle-20 real-Xbox runs) to the harness's known-writeable, FTP-collectable `E:\Apps\image-blit\…` target. Rebuild; locally validate; re-run real-Xbox witness; interpret conservatively; Codex-validate; sync canonical docs.
- Status: **CLOSED — marker re-route shipped, local validation green, real-Xbox witness returned a clear NEGATIVE answer, cycle-19 hypothesis #1 demoted from leading to insufficient.**
- Active session: cycle-21 (Claude Max via /Users/jbbrack03/.local/bin/claude-max-shell). HEAD at start = `254b888b80` (cycle-20 packaging). Cycle-20 closure commit was `8daed392af`.

## Outcome (one-paragraph)

Modified `scripts/apple-silicon/xbe-tests/image-blit/main.c` so the cycle-20 `image_blit_marker` helper writes to `E:\Apps\image-blit\image-blit-marker-NN-STAGE.txt` instead of `D:\image-blit-marker-NN-STAGE.txt`. Added an idempotent cached `image_blit_ensure_e_mount` helper (calls `nxIsDriveMounted('E')` → fallback `nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")` → `CreateDirectoryA("E:\\Apps", NULL)` + `CreateDirectoryA("E:\\Apps\\image-blit", NULL)`) — the same pattern shipped in `oracle-agent/controller.c::s_ensure_e_drive_mounted` + `s_write_anchor_file`, `oracle-agent/tier2.c`, `lib/xbed_input_synth.c`, and `controller-readback/main.c`. Bumped the marker-path buffer from 64 to 96 bytes for the longer prefix (basenames still ≤39 chars; FATX 42-char invariant unchanged). Added explicit `e_mount=N` to the host-log line so xemu logs decompose mount success vs fopen success. Local xemu-Metal validation across 4 boots: 52 marker host-log lines fire with `e_mount=1` everywhere and **zero `fopen-failed`** (vs cycle-20's 52/52 fopen-failed for the same XBE structure with D:\ paths); v0.4 tally drift across the 4 boots `3/8 mask=0x31, 3/8 mask=0x31, 2/8 mask=0x30, 2/8 mask=0x30` is byte-identical to cycle-20's, confirming no regression. Single bounded real-Xbox run: chainload→FTP-back 22.4 s (the **fourth independent reproduction** of that exact gap across cycle-19+cycle-20+cycle-21), `verdict.json status: ok`, FTP-collect from `/E/Apps/image-blit/` retrieved exactly 1 file (`default.xbe` upload echo, 159 744 B), **ZERO `image-blit-marker-*` files** present. The cycle-21 question ("does re-routing to E:\ make markers observable on real Xbox?") is answered **NO**. Cycle-19 hypothesis #1 ("D:\ remap mismatch under `runxbe` chainload is the witness-path blocker") is INVALIDATED as a sole explanation — a provably-writeable, provably-retrievable E:\ path produces zero retrieved markers under the same chainload, so the blocker is upstream of any in-XBE `fopen` call.

## Evidence preserved

Local xemu-Metal:
- `benchmark-runs/cycle21-image-blit-markers-local-metal-guestlog-20260522T231623Z/` — first local run (E:\ path, NO `CreateDirectoryA`). Surfaced the local-validation oversight; immediately fixed.
- `benchmark-runs/cycle21-image-blit-markers-local-metal-guestlog-20260522T231823Z/` — second local run (E:\ path WITH `CreateDirectoryA` chain). 52/52 markers fire, 0/52 fopen-failed, v0.4 tally byte-identical to cycle 20.

Real Xbox:
- `benchmark-runs/cycle21-real-xbox-image-blit-markers-20260522T232031Z/` — single bounded attempt. chainload→FTP-back 22.4 s, `verdict.json status: ok`, FTP-collect returned 1 file (`default.xbe` upload echo, 159 744 B), 0 marker files. Contains `report.md`, `summary.json`, `image-blit/real-xbox/real-xbox.log`, `orch/{pre,post}.png + verdict.json + artifacts/default.xbe`.

Codex validation:
- Mode `changes`, verdict MINOR ISSUES. Two findings (both about doc-sync completeness at Codex-run-time), both adopted in full in this same closure pass.
- Marker at `.claude/state/codex-validate-last-run` per rule #15.

## Diff at session close (uncommitted)

- `scripts/apple-silicon/xbe-tests/image-blit/main.c` — ~120 net added lines (helper + comment block rewrite + idempotent E:\ mount + dir-create + path-buffer resize + host-log `e_mount=N` field).
- `scripts/apple-silicon/xbe-tests/image-blit/bin/default.xbe` — rebuilt binary (size unchanged at 159 744 B).
- `scripts/apple-silicon/xbe-tests/image-blit/image-blit.iso` — rebuilt ISO (size unchanged at 720 896 B; content updated).
- `docs/apple-silicon/handoff.md` — cycle-21 entry on top; cycle-20 preserved below.
- `docs/apple-silicon/decision-log.md` — cycle-21 entry above cycle-20; cycle-17 / cycle-19 / cycle-20 NOT superseded.
- `docs/apple-silicon/orchestration-state/*` — all four files updated (this one, current-cycle.md, validation-status.md, handoff-summary.md).
- `.hermes_cycle21_path_a2_prompt.txt` — untracked Hermes-side scratch; not staged.
- Cycle-21 incidental .inl drift in `scripts/apple-silicon/xbe-tests/lib/{vs,xbed_tex_vs}.inl` (source-path strings shifted by `make clean` cycling the build context from `blend-matrix/` to `image-blit/`) was **reverted** before Codex validation to keep cycle-21 commit scope clean per assignment guardrail.

## Canonical docs synced

- `docs/apple-silicon/handoff.md` — cycle-21 Path A.2 entry written; cycle-20 entry preserved.
- `docs/apple-silicon/decision-log.md` — cycle-21 entry written; cycle-19 / cycle-17 / cycle-20 NOT superseded.
- `docs/apple-silicon/orchestration-state/*` — all four files reflect cycle-21 outcome (current-cycle.md, claude-status.md, validation-status.md, handoff-summary.md).

## Codex validation

- Mode: `changes`. Verdict: MINOR ISSUES. Two findings (medium: doc sync still pending at the Codex-run-time snapshot; low: claude-status said all four orch-state files were updated but handoff-summary.md was not yet touched). Both adopted in full and addressed within the same closure pass. Codex's open question on the next-bounded-slice framing ("trivial write-only XBE on `runxbe` path vs A.3 provenance check?") is recorded verbatim in the handoff cycle-21 entry. Validation marker at `.claude/state/codex-validate-last-run`.

## Next bounded slice (NOT promoted this cycle)

- **Path A.3 (now top-priority per rule #1).** Cross-check whether the existing `xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor}` captures came from `runxbe` SITE-EXEC chainload or from a different launch path. If from `runxbe`, the cycle-21 result is image-blit-specific (the XBE itself crashes early on this chainload path) and the failure is in image-blit; if from a different launch path, the oracle pipeline needs a non-`runxbe` chainload mode for any XBE that depends on observable file writes. Cheapest single follow-up per rule #1: requires only inspecting existing artifacts + tooling history; no new code.
- **Path A.4 (new, proposed cycle 21).** Add a non-fopen witness to image-blit: write a few bytes into the oracle-agent's persistent kernel-pool controller buffer (`oracle_ctrl_buffer` at the agent-published phys address) BEFORE attempting any marker fopen. Next agent boot reads the buffer via `controller.buffer-info` / `controller.get`. Bypasses every partition-mount + FATX-driver-state concern. Heavier than A.3 but adds independent high-signal evidence.
- **Path B.** Smaller PFIFO-race-only Tier-1 diag XBE that captures via PCRTC. Still on the table.

Scope choice belongs to the next Hermes pass.
