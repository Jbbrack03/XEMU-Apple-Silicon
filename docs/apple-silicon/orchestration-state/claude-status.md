# Claude Status

- Objective: cycle 27 Path A.4 option (a) — preserve an existing oracle controller buffer witness stamp across agent restart by modifying `oracle-agent/controller.c::s_allocate_fresh` (XBE source slice only; no real-Xbox run this session).
- Status: **CLOSED.** Fresh bounded session 2026-05-23; option (a) shipped with Codex 3-round validation green (round 3 = LOOKS GOOD).

## Why cycle 27 ran this session

Hermes pre-session instruction explicitly assigned cycle-27 option (a) as the bounded slice for this session, citing the cycle-26 closure (commit `a31e061144`) which left the stamp-vs-no-stamp ambiguity OPEN and listed 4 candidate cycle-27 redesigns. Option (a) was selected as the LOW-risk LOW-LOC pick to address the most-likely failure mode (kernel-pool-reuse-driven memset-wipe in the relaunched agent). The other three options (b/c/d) remain on the table for cycle 29+ if cycle 28 does not resolve the ambiguity.

## What this session shipped

1. New static helper `s_page_has_plausible_witness_header(vp)` in `oracle-agent/controller.c` mirroring the cycle-23 lockstep plausibility filter but TIGHTENED (Codex round-1 medium) to accept only the two header shapes the agent's writer paths actually produce: `(reserved0==0, reserved1==0)` or `((reserved0>>24)==0xA4, 1<=reserved1<=4096)`. The preserve gate is a strict subset of the cycle-23 scan filter (false negative → safe fallback; false positive → would silently retain garbage as a real header).
2. `s_allocate_fresh` two-branch refactor: preserve branch keeps magic/version/reserved[0,1] and clears only `port[]`; legacy branch keeps full-zero + re-stamp magic/version. `cache_writeback_invalidate` after either branch.
3. One conditional `debugPrint` breadcrumb in the preserve branch reports `phys`, `reserved[0]`, `reserved[1]` so on-screen verification (composite capture / debug overlay) can confirm the branch activated without `witness.scan`.
4. `controller.h` `oracle_ctrl_init` doc comment extended to describe the cycle-27 refinement.
5. `commands.c::cmd_witness_scan` body comment extended with a "Cycle-27 additional success shape" block listing the new `count=1 live=1 reserved0=0xA4xxxxxx` positive shape alongside the legacy orphan shape.
6. `witness-only/README.md` discriminator-semantics table updated to list both success shapes and clarify cycle-28 follow-on.
7. `witness-only/manifest.json` `artifacts.witness_readback.notes` + `expected_results.real-xbox/physical/cycle-26.notes` both extended to describe both shapes.
8. `oracle-agent/bin/default.xbe` rebuilt (size unchanged at 417 792 B); `oracle-agent.iso` rebuilt (983 040 B).
9. Codex 3 rounds: round 1 = MINOR ISSUES (medium + low; both adopted); round 2 = MINOR ISSUES (round-1 MEDIUM RESOLVED, round-1 LOW PARTIAL; adopted); round 3 = LOOKS GOOD. Validation marker written.
10. Canonical docs synced: handoff.md cycle-27 entry, decision-log.md cycle-27 entry, orchestration-state quartet (current-cycle.md, claude-status.md = this file, validation-status.md, handoff-summary.md) closure pass.

## Session progress

- [x] Read required docs/state files (handoff.md cycle-26/25/24 entries, orchestration-workflow.md, decision-log entries, all four orchestration-state files, oracle-and-xbe rule snapshot, controller.{c,h}, lib/xbed_a4_witness.{c,h}, commands.c witness reader).
- [x] Designed preserve-branch predicate + branch logic (tightened in Codex round 1).
- [x] Implemented option (a) minimally in `controller.c` + paired doc-only edits in `controller.h`, `commands.c`, `witness-only/README.md`, `witness-only/manifest.json`.
- [x] Rebuilt oracle-agent XBE; verified size unchanged + binary built cleanly.
- [x] Codex round 1 (changes mode): MINOR ISSUES; both findings adopted in full.
- [x] Codex round 2 (changes mode): MINOR ISSUES; round-1 MEDIUM RESOLVED, round-1 LOW PARTIAL adopted.
- [x] Codex round 3 (changes mode): LOOKS GOOD; no new findings.
- [x] Validation marker written at `.claude/state/codex-validate-last-run`.
- [x] Updated orchestration-state quartet for cycle-27 closure.
- [x] Updated handoff.md cycle-27 entry; appended decision-log cycle-27 entry.
- [x] Closure commit landed on `apple-silicon-performance`: `df999e41ea`.

## Confidence + risk notes

- HIGH confidence in the predicate tightness (predicate is a strict subset of the cycle-23 scan filter; false negative falls back to legacy safe behavior; false positive would require the kernel-pool page to coincidentally spell `XCTR+1+(0,0)` or `XCTR+1+(0xA4xxxxxx, 1..4096)`).
- HIGH confidence in the preserve branch's correctness (the branch only memsets `port[]`, leaves the 16-byte header intact, preserves cache-writeback semantics).
- HIGH confidence in build + Codex green (3 rounds; round 3 LOOKS GOOD).
- MEDIUM confidence in the cycle-28 outcome shape A1 (`count=1 live=1 reserved0=0xA4xxxxxx`). It assumes: (i) kernel pool still returns the same phys deterministically across agent re-launches as cycle 26 observed; (ii) the relaunched agent's `MmAllocateContiguousMemoryEx` returns that same phys (not some other pool page); (iii) the preserve gate matches on that page. Any of those failing falls back to legacy behavior or surfaces a different outcome shape, all of which are documented in the discriminator semantics.
- LOW risk to existing behavior: non-preserve branch is identical to the pre-cycle-27 code path.

## What this session does NOT do

- NO real-Xbox deployment.
- NO host source touched.
- NO image-blit source touched.
- NO witness-only source touched (manifest/README doc edits only).
- NO `lib/xbed_a4_witness.{c,h}` touched.
- NO flag default flips.
- NO cycle-27 options (b)/(c)/(d) pursued.

## Next proposed action

Cycle-27 closure commit `df999e41ea` already landed on `apple-silicon-performance`. Cycle 28 (real-Xbox deployment of the cycle-27 oracle-agent) is Hermes's call.
