# Handoff Summary

- Cycle 17 closed the local renderer-agnostic milestone; cycle 18 packaged the state in a doc-only checkpoint.
- Cycle 19 attempted a real-Xbox parity check via the cycle-15 v0.4 image-blit XBE; reproducibly produced zero `D:\image-blit-capture.bin` / `D:\image-blit-done.txt` across two runs. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on decision DEFERRED.
- Cycle 20 took Path A: 13 staged `D:\image-blit-marker-NN-STAGE.txt` markers; local xemu validation showed all 13 fire; every fopen returned NULL on the ISO mount; two real-Xbox runs BOTH produced zero marker files; chainload→FTP-back stable at 22.4 s.
- Cycle 21 took Path A.2: re-route markers from `D:\` to `E:\Apps\image-blit\…`. Local validation 4 boots OK; single real-Xbox run produced clean NEGATIVE — 4th independent 22.4 s reproduction, `verdict.json status: ok`, ZERO marker files. Cycle-19 hypothesis #1 demoted from "leading" to "insufficient as sole explanation."
- Cycle 22 took Path A.3: provenance audit of `docs/apple-silicon/xbox-real-references/*` captures. All five reference sets WERE produced through the same `XLaunchXBE` chainload mechanism (HIGH confidence). Cycle-19 hypothesis #1 fully INVALIDATED. Image-blit failure re-classified as **image-blit-specific**. Cycle-22 leading hypothesis: image-blit crashes BEFORE main()'s first instruction.
- Cycle 23 took Path A.4: shipped non-fopen kernel-pool controller-buffer witness for image-blit + agent-side `witness.scan` RPC. Local xemu-Metal validation green (4 boots). Codex round-1 BLOCK → all 4 findings adopted → round-2 PASS_WITH_FINDINGS → MINOR PARTIAL closed post-round-2. Closure commit `5fce3b14e4`; doc-sync follow-up `b614bdbc83`.
- Cycle 24 ran the cycle-23 witness on real Xbox. CLOSED with a CONCRETE BLOCKER. Cycle-23 binaries deployed via FTP. Baseline `witness.scan` precondition MET. `runxbe E:\Apps\image-blit\default.xbe` issued at 2026-05-23T02:34:34Z. Xbox went fully silent for 928.3 s before measurement was aborted. Post-chainload `witness.scan` UNRECOVERABLE without a physical power-cycle that erases the persistent buffer. Cycle-22 leading hypothesis ("pre-main crash") is WEAKENED but not corroborated or invalidated. NEW hypothesis #5: the kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context. Closure commit `487e729d4f`; doc-sync follow-up `29a455a78e`.
- Cycle 25 implemented the cycle-24-recommended witness-only diagnostic XBE. SHIPPED + CLOSED. 5 new files under `scripts/apple-silicon/xbe-tests/witness-only/` + built `bin/default.xbe` (147 456 B) + `witness-only.iso` (720 896 B). Local xemu-Metal smoke green; Codex 3-round validation green (round 3 = PASS_WITH_FINDINGS).
- **Cycle 26 ran the cycle-25 witness-only XBE on real Xbox.** CLOSED — partial discriminator outcome (outcome shape D). Xbox was in dashboard state at session start (post-cycle-24 physical power-cycle by Hermes). Baseline witness.scan precondition MET (count=1, phys=0x03eb3000, reserved[0]=0, mapped_pages_seen=419). FTP-uploaded witness-only/bin/default.xbe to `/E/Apps/witness-only/default.xbe`. Chainloaded witness-only 4× plus 2 controls (invalid-path + mirror). **Definitive chainload→dashboard-ready timing via `curl FTP LIST` poll: ~70 s for witness-only, vs 20.67 s for invalid-path control, vs ~36 s for known-good mirror control.** Post-run witness.scan after EVERY chainload: identical to baseline (no orphan ever observed). **Hypothesis #5 PARTIALLY INVALIDATED in the catastrophic-hang sense** (Xbox recovered fully in 70 s, no physical power-cycle needed; vs cycle-24's 928 s+ silent). Cycle-22 leading hypothesis status: still WEAKENED — cycle-26 evidence cannot discriminate "witness fire never landed a stamp" vs "stamp landed but new agent's `s_allocate_fresh::memset` wiped it after kernel pool returned the same phys=0x03eb3000 across 3 observed agent re-launches". No source/script code edits this session; evidence-gathering only, Codex skipped under rule #15 doc-only carve-out.

## Cycle 26 design + outcome (locked at session close 2026-05-22)

| Step | Action | Outcome |
|---|---|---|
| 1 | Read required docs + verify Xbox state | Xbox @ 192.168.0.200; FTP/21 OPEN, agent/9001 CLOSED → dashboard state, post-cycle-24 power-cycle confirmed |
| 2 | ensure-agent + baseline witness.scan | precondition MET: count=1 phys=0x03eb3000 reserved[0]=0 mapped_pages_seen=419 |
| 3 | reboot + FTP upload witness-only XBE | 147 456 B uploaded to /E/Apps/witness-only/default.xbe (FTP LIST verified) |
| 4 | ensure-agent + baseline rescan (post-reboot) | identical state (consistent) |
| 5 | runxbe witness-only 4× (incrementally tightening poll method) | chainload issued OK each time; agent dies + dashboard eventually returns |
| 6 | Definitive timing via `curl FTP LIST` poll (log 20-) | **~70.17 s witness-only chainload → dashboard fully ready** |
| 7 | Control: invalid-path chainload | t+20.67 s (matches agent XLaunchXBE-failure path) |
| 8 | Control: mirror chainload (known-good Tier-1 diag) | t≈36 s (clean known-good chainload) |
| 9 | Post-run witness.scan (4× total) | identical to baseline every time; NO ORPHAN observed |
| 10 | Canonical docs sync | handoff.md cycle-26 on top; decision-log cycle-26 above cycle-25; orchestration-state quartet closure pass |
| 11 | Closure commit on `apple-silicon-performance` | Landed in this commit |

## Cycle 26 scope discipline

- ZERO source/script code edits this session.
- ZERO XBE rebuilds.
- ZERO changes to `lib/xbed_a4_witness.{c,h}`.
- ZERO changes to `oracle-agent/`.
- ZERO changes to image-blit.
- ZERO changes to cycle-25 witness-only XBE source.
- ZERO xemu-fork host source touched.
- ZERO flag default flips.
- ZERO retail-title / §G.5 / RT-as-texture / second-wave XBE work.
- Cycle 26 evidence-gathered + doc-only; closes cleanly; canonical docs + orchestration-state quartet synced; evidence preserved on disk.

## Important interpretation notes

- **Outcome D RULES OUT outcome B** (catastrophic hang). The kseg0-scan witness mechanism does NOT, in this minimal XBE, render the real Xbox unrecoverable. Hypothesis #5 is partially weakened: the mechanism may still be unsafe in subtler ways, but it does not catastrophically hang.
- **Outcome D DOES NOT YET PROVE outcome A or C.** A successful witness stamp would have produced an orphan observable in post-run witness.scan. None was observed across 4 chainloads. This could mean (i) no stamp landed (witness fire never completed) OR (ii) stamp landed but `s_allocate_fresh::memset` in the relaunched agent silently wiped it after the kernel pool deterministically returned the same phys=0x03eb3000 (observed 3× across the session). Session evidence cannot discriminate.
- **The ~70 s chainload→dashboard gap is a NEW failure-mode shape** between "clean reboot" (~20 s) and "hard hang" (~928 s+). It could reflect a slow kseg0 scan, a delayed-fault watchdog recovery, or a slow BIOS POST after an exception. Session evidence cannot discriminate.
- **Methodology lesson:** `nc -z -w 1` produces spurious early port=open readings during Xbox network-stack transitions. `curl --max-time 2 ftp://.../` issuing a real FTP LIST is the reliable ground-truth signal for "dashboard fully ready". Encoded into future cycle-26-style poll loops.

## Next bounded slice (cycle 27 candidate — Hermes-scheduled)

To break the cycle-25/26 stamp-vs-no-stamp ambiguity. Pick ONE:

- (a) Modify `oracle-agent/controller.c::s_allocate_fresh` to NOT `memset` when it finds an existing `XCTR + version==1` buffer at the returned phys (preserve any landed witness stamp). LOW risk; LOW LOC.
- (b) Add a `runxbe`-side dump of the prior agent's controller-buffer phys+reserved[] to a known file BEFORE chainloading; have a `read-only kseg0 dump` agent verb that reads from that exact phys without re-initializing. MEDIUM risk; MEDIUM LOC.
- (c) Use a fresh `MmAllocateContiguousMemoryEx` page from witness-only directly (separate magic tag, separate page) instead of stamping the agent's buffer. HIGH risk (allocator semantics from non-agent context); MEDIUM LOC.
- (d) Add an on-screen visual breadcrumb (debugPrint at a known character position) that's captured via `oracle-orchestrator.py capture` mid-run. LOW risk; LOW LOC; but requires a `screenshot` verb call that doesn't re-init the buffer (need to add or repurpose an agent verb).

Cycle 27 should also discriminate the ~70 s delay: slow kseg0 scan vs delayed-fault watchdog vs slow BIOS POST.

## Codex validation

Cycle 26 skipped Codex validation under rule #15's doc-only / ≤30-line uncommitted source diff carve-out. Zero source/script edits this session; all Xbox-side operations used existing tooling.

## Evidence on disk

- `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/{00..20-*.log, SUMMARY.md}` (gitignored per project convention; consistent with cycles 19/20/21/22/24/25).

## Closure commit

Landing in this commit on `apple-silicon-performance`.
