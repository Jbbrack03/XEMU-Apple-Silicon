# Current Cycle

- Cycle: 27 Path A.4 option (a) — `oracle-agent/controller.c::s_allocate_fresh` preserves an existing plausible `oracle_ctrl_buffer` witness header across agent restart instead of unconditionally `memset`-wiping it (**CLOSED — 2026-05-23**, XBE source slice green; cycle-28 real-Xbox deployment is Hermes's call).
- Started: 2026-05-23 (Hermes-supervised fresh bounded session; Claude Code worker autonomous run).
- Closed: 2026-05-23 (this update + closure commit).
- State: **CLOSED. Option (a) implemented minimally; oracle-agent XBE rebuilt; Codex 3-round validation green (round 3 = LOOKS GOOD); canonical docs synced; orchestration-state quartet closure pass.**
- Owner: Claude Code worker (fresh bounded session), launched 2026-05-23.
- HEAD at start: post-cycle-26 closure commit `a31e061144` on `apple-silicon-performance`.
- Bounded goal: "Implement cycle-27 option (a) only — preserve an existing oracle controller buffer witness stamp across agent restart by modifying `oracle-agent/controller.c::s_allocate_fresh`. Keep scope tight. No host source changes. No image-blit / witness-only / xbed_a4_witness changes."
- Result: SHIPPED. Five source/doc files modified (controller.c, controller.h, commands.c, witness-only/README.md, witness-only/manifest.json) + 2 rebuilt binaries (bin/default.xbe + oracle-agent.iso). NO host source touched; NO image-blit / witness-only-source / lib/xbed_a4_witness touched.

## Plan summary (this session, executed in order)

1. Read required docs/rules + orchestration-state files (handoff.md cycle-26/25/24, orchestration-workflow.md, decision-log entries, current-cycle/claude-status/validation-status/handoff-summary, oracle-and-xbe rule snapshot, controller.{c,h}, lib/xbed_a4_witness.{c,h}, commands.c witness reader).
2. Designed predicate + branch logic for `s_allocate_fresh` to mirror the cycle-23 lockstep plausibility filter (Codex round 1 tightened it to a strict subset of that filter — see exit criteria).
3. Implemented `s_page_has_plausible_witness_header` static helper + two-branch preserve/legacy structure in `s_allocate_fresh`; added one `debugPrint` breadcrumb when preserve fires.
4. Updated `controller.h` `oracle_ctrl_init` doc comment to reflect the cycle-27 refinement.
5. Rebuilt oracle-agent XBE via `eval "$(nxdk/bin/activate -s)" && make`: bin/default.xbe = 417 792 B (size unchanged from cycle 26); oracle-agent.iso = 983 040 B. Benign `lld: warning: .edata=.rdata` repeats from prior cycles.
6. Ran Codex validation round 1 (changes mode): MINOR ISSUES with 1 medium + 1 low finding. Both adopted in full.
7. Re-ran Codex validation round 2: MINOR ISSUES (round-1 MEDIUM RESOLVED; round-1 LOW PARTIAL on one residual manifest field). Adopted.
8. Re-ran Codex validation round 3: LOOKS GOOD (round-2 PARTIAL RESOLVED; round-1 MEDIUM still RESOLVED; no new round-3 findings). Validation marker written.
9. Canonical docs synced: handoff.md cycle-27 entry on top (cycle-26 preserved unchanged), decision-log.md cycle-27 entry above cycle-26 (no supersession), orchestration-state quartet refreshed for closure.

## Exit criteria — final status

1. [x] Required docs read; plan summarized to current-cycle.md (this file) + claude-status.md.
2. [x] `oracle-agent/controller.c::s_allocate_fresh` modified to preserve an existing plausible witness header; helper `s_page_has_plausible_witness_header` added; predicate accepts ONLY `(reserved0==0, reserved1==0)` OR `((reserved0>>24)==0xA4, 1<=reserved1<=4096)` (Codex round-1 medium adopted: strict subset of cycle-23 scan filter); branch clears only `port[]` in preserve case; cache_writeback_invalidate after either branch.
3. [x] `controller.h` `oracle_ctrl_init` doc updated to reflect the cycle-27 refinement (minimal paired comment per Hermes pre-session "minimal paired comments/docs if needed").
4. [x] `commands.c::cmd_witness_scan` body comment extended with "Cycle-27 additional success shape" block (Codex round-1 low adopted).
5. [x] `witness-only/README.md` discriminator-semantics table updated to list both success shapes (orphan or live preserve) and clarify cycle-28 follow-on (Codex round-1 low adopted).
6. [x] `witness-only/manifest.json` `artifacts.witness_readback.notes` + `expected_results.real-xbox/physical/cycle-26.notes` both extended to describe both shapes (Codex round-1 low + round-2 PARTIAL adopted).
7. [x] oracle-agent XBE rebuilt cleanly via nxdk; size unchanged at 417 792 B; manifest.json still parses.
8. [x] Codex validation 3 rounds, round 3 = LOOKS GOOD. Validation marker written at `.claude/state/codex-validate-last-run`.
9. [x] Canonical docs synced: handoff.md cycle-27 entry, decision-log.md cycle-27 entry, orchestration-state quartet (current-cycle.md = this file, claude-status.md, validation-status.md, handoff-summary.md) closure pass.
10. [x] Two pre-existing untracked prompt files (`.hermes_cycle22_path_a3_prompt.txt`, `.hermes_cycle23_docsync_prompt.txt`) preserved un-staged per Hermes pre-session instruction.
11. [ ] Closure commit pending (this commit on `apple-silicon-performance`).

## Out-of-scope (kept bounded for cycle 27)

- NO xemu-fork host source touched (no `hw/`, `ui/`, `target/`, `include/`).
- NO image-blit source touched.
- NO witness-only source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 scan filter intentionally stays wider than the cycle-27 preserve gate — lockstep contract documented in three comment blocks).
- NO XBE rebuilds beyond oracle-agent.
- NO real-Xbox deployment — cycle 28 is Hermes's call.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO cycle-27 options (b)/(c)/(d) pursued; they remain on the table for cycle 29+ if cycle 28 does not resolve the ambiguity.

## Outcome (cycle 27)

**Option (a) shipped.** `oracle-agent` now preserves a plausible existing witness header across agent restart. Cycle 28 (Hermes-scheduled) can deploy the new oracle-agent + the unchanged cycle-25 witness-only XBE to discriminate the cycle-26 stamp-vs-no-stamp ambiguity via a new positive success shape `count=1 live=1 reserved0=0xA4xxxxxx` (preserve branch retained the live-buffer stamp) alongside the legacy orphan shape.

## What outcome resolves and what it leaves open

**RESOLVED (this session):**
- The cycle-26 design gap "kernel-pool reuse silently wipes the stamp" is now addressed. The relaunched agent's `s_allocate_fresh` detects a plausible witness header on the returned page and preserves it.
- The cycle-26 documentation gap "what does a successful preserve-branch readback look like?" is closed across `commands.c::cmd_witness_scan` body comment, `witness-only/README.md`, and `witness-only/manifest.json`.

**LEFT OPEN (cycle 28 scope):**
- **Stamp-vs-no-stamp question still UNRESOLVED at the real-Xbox level** until cycle 28 actually runs. This session ships the discriminator-sharpening tool; cycle 28 runs it.
- **Cycle-22 leading hypothesis status: still WEAKENED.** Will be resolved one way or the other by cycle 28's `witness.scan` readback after a `witness-only` chainload.
- **The cycle-26 ~70 s recovery shape (slow kseg0 scan vs delayed-fault watchdog vs slow BIOS POST) is NOT addressed by the preserve branch.** Cycle 28 should still discriminate this — the preserve branch only sharpens the post-chainload readback, not the chainload-→dashboard-ready timing.
- **§H.6 default-on shape decision REMAINS DEFERRED.**

## Recommended cycle-28 scope (NOT executed this session)

Deploy the cycle-27 `oracle-agent/bin/default.xbe` + unchanged cycle-25 `witness-only/bin/default.xbe`. Canonical sequence + the `curl FTP LIST` poll methodology that cycle 26 encoded. Expected positive shapes: A1 (`count=1 live=1 reserved0=0xA4000003`) — preserve branch retained the live-buffer stamp on the reused phys; A2 (`count>=2` with stamped orphan reserved0=0xA4000003) — legacy orphan shape if the kernel pool returned a different phys this time. Either A1 or A2 means cycle-22 leading hypothesis INVALIDATED. D-cycle-27 (`count=1 live=1 reserved0=0`) means the stamp never landed; cycle 29 promotes option (c) or (d). B (identical 928s+ hang) means hypothesis #5 promoted back; redesign required.

§H.6 default-on / long-term-fix decision REMAINS DEFERRED.
M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-28 real-Xbox discriminator run with cycle-27 oracle-agent), §G.5, RT-as-texture.
