# Claude Status

- Objective: cycle 42B custom pre-WinMainCRTStartup entry-point thunk — bounded implementation+local-build+Codex slice on the cycle-22 calling-context axis (the only live axis after cycle-42A exhausted the in-XBE / lib-only candidate set). Strategy: NEW `scripts/apple-silicon/xbe-tests/witness-only/witness_only_crt0.c` defines `witness_only_pre_winmain_crt_startup` which (1) defensively writes EEPROM offset 0xFF to `0xA6 = 0xA0 | 6` via `HalWriteSMBusValue` (defense against vsnprintf-pre-libc fault), (2) calls `xbed_self_witness_fire(stage=6=WITNESS_ONLY_STAGE_PRE_WINMAIN_CRT)` with the cycle-29 self-witness shim unchanged, (3) calls nxdk's standard `WinMainCRTStartup` so the rest of the runtime is bit-identical to a non-cycle-42B nxdk XBE. The thunk runs BEFORE nxdk's `__security_init_cookie` + TLS sizing + `_PDCLIB_xbox_libc_init` + `_PDCLIB_xbox_run_pre_initializers` + any `.CRT$X*` slot. Selected as the PE entry point via `NXDK_LDFLAGS += -entry:witness_only_pre_winmain_crt_startup` in `witness-only/Makefile`. ZERO nxdk source edits; ZERO `lib/*` edits; ZERO oracle-agent edits. Cycle-23 lockstep contract + cycle-29 self-witness shim + cycle-31 paint + cycle-35 .CRT$XX*+XCU slots + cycle-39 EEPROM sticky gate + cycle-41a..41e historical comment blocks + cycle-42A multi-page redesign: ALL PRESERVED unchanged.
- Status: **IMPLEMENTATION COMPLETE; Codex GREEN deploy-ready; real-Xbox deployment DEFERRED to next bounded slice.** Code change: NEW `witness_only_crt0.c` (~390 LOC, ~280 LOC of in-source documentation per Codex 5-round adoption) + 12-line addition to `witness-only/Makefile` (SRCS entry + -entry: link flag + rationale comment). Final witness-only artifact SHA = `3cc670fb4df5871811d94bcc8339e5126ab0e61a78e44802ba9e655d742442a1` (155 648 B; same XBE page boundary as cycles 31..42A; source bit-identical to Codex-R5-GREEN-confirmed state). PE-level verification: `llvm-readobj` reports `AddressOfEntryPoint=0x3510` (ImageBase 0x10000, virtual VA 0x13510); `llvm-objdump` at 0x13510 confirms the expected sequence: `s_witness_only_42b_eeprom_attempted` sticky check → `HalWriteSMBusValue(0xA8, 0xFF, FALSE, 0xA6)` import-thunk call → `xbed_self_witness_fire(6)` direct call → `WinMainCRTStartup` direct call. Cycle-42B is the smallest-blast-radius implementation of candidate B from the cycle-41e/42A closure: INSPECTING `nxdk/tools/cxbe/Xbe.cpp:222` showed cxbe does NOT invent entry points — it propagates the PE optional-header's `AddressOfEntryPoint` field — so a per-XBE `-entry:` link flag is functionally equivalent to (and strictly smaller blast radius than) modifying cxbe's header generator. The cycle-22 calling-context axis is now testable WITHOUT nxdk source edits.

## Why cycle 42B ran this session

The cycle-42A closure pre-recorded candidate B as "the necessary follow-up" once branch (c) (size axis) had been varied without flipping G0(c): "custom XBE-header callback before `_start` via `nxdk/tools/cxbe/` modifications — separates the calling-context axis from the size axis; strictest pre-CRT context distinguishes 'calling-context-specific failure inside the cycle-29 `.CRT$XXC` slot' from 'shared-upstream failure independent of calling context'. Larger blast radius than candidate A (requires modifying nxdk's XBE-header generator), but at this point the calling-context axis is the only remaining live cycle-22 candidate that can be tested inside the in-XBE oracle workflow." This session's design discovery is that the "modify nxdk's XBE-header generator" framing was unnecessary — cxbe propagates rather than invents the entry-point address, so a per-XBE `-entry:` link flag achieves the same calling-context discrimination at strictly smaller blast radius (no nxdk source edits required).

## What this session shipped

1. **NEW file** `scripts/apple-silicon/xbe-tests/witness-only/witness_only_crt0.c` (~390 LOC): the cycle-42B custom PE entry-point thunk + extensive in-source documentation (rationale + WTNS-shim fire ordering + joint (reserved0_last_stage, reserved1) discriminator table + risk surface analysis + preserved invariants).
2. **Makefile addition** at `scripts/apple-silicon/xbe-tests/witness-only/Makefile`: 12-line block adding `SRCS += witness_only_crt0.c` + `NXDK_LDFLAGS += -entry:witness_only_pre_winmain_crt_startup` + rationale comment.
3. **Build verification**: clean nxdk rebuild succeeds; lld accepts the custom entry symbol without errors; PE AddressOfEntryPoint correctly relocated; disassembly at the entry VA matches the expected source sequence byte-for-byte.
4. **Codex 5-round validation** (mode=`changes`): R1 MAJOR ISSUES (HIGH 0xA6+count=0 overclaim + MED logically-impossible 0xA4+count=0 row; both ADOPTED via documentation rewrite) → R2 MED (post-allocation vs pre-allocation guard misattribution; ADOPTED via (a)/(b)/(c) failure-mode list) → R3 HIGH (success row over-interpreted reserved1; ADOPTED via reserved1-keyed multi-row table) → R4 NOT GREEN (reserved1+execution-died ambiguity + cycle-23 XCTR mis-attribution; ADOPTED via JOINT (reserved0_last_stage, reserved1) table + XCTR clarification) → R5 GREEN — explicit "Deploy-ready." All hard findings adopted via in-source documentation rewrites; ZERO code changes between R1 and R5. Codex marker refreshed.
5. **Cycle-22 leading hypothesis NARROWED to the calling-context axis**: cycles 40+41a+41b+41c+41d+41e+42A exhausted cache-policy + address-range + alignment + entry-point + size; cycle 42B's discriminator targets calling-context as the only remaining live axis testable inside the in-XBE oracle workflow scope.
6. **Real-Xbox deployment runbook delta** documented in `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/SUMMARY.md`. Key differences vs cycle 42A runbook: expected post-run EEPROM byte is now `0xA6` (not `0xA4`); `witness.scan-self` interpretation requires BOTH count + reserved0_last_stage + reserved1 (cycle 42A only needed count + reserved0); joint table enumerates all 14 (M, reserved1) combinations.
7. **Evidence directory** at `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/` with SUMMARY.md + 5× Codex prompts + 5× Codex outputs (gitignored per project convention).
8. **Canonical docs synced.** handoff.md cycle-42B entry on top above cycle-42A; decision-log.md cycle-42B entry above cycle-42A; orchestration-state quartet (this file + current-cycle.md + validation-status.md + handoff-summary.md) updated to cycle-42B closure.

## Session progress

- [x] Read required docs/state.
- [x] Inspected `git status --short` + recent commits — HEAD at cycle-42A closeout-sync `1fca1bba62`; pre-existing tracked drift + 25+ untracked `.hermes_*` files + `composite_preflight.py` + 2 `lib/*.inl` preserved un-staged.
- [x] Inspected `nxdk/tools/cxbe/{Main.cpp,Xbe.cpp,Xbe.h}` → discovered cxbe propagates rather than invents the PE entry-point address.
- [x] Inspected `nxdk/lib/pdclib/platform/xbox/{crt0.c,crt_initializers.c,tls.c}` → mapped the standard nxdk startup sequence.
- [x] Designed minimal-blast-radius candidate B: per-XBE `-entry:` link flag + thin thunk + tail call to nxdk's `WinMainCRTStartup`.
- [x] Wrote `witness-only/witness_only_crt0.c` with thunk + extensive documentation.
- [x] Modified `witness-only/Makefile` to wire in the new source file + custom entry.
- [x] Clean nxdk rebuild → witness-only artifact built successfully.
- [x] PE-level verification via `llvm-readobj` (AddressOfEntryPoint=0x3510) + `llvm-objdump` (entry instruction sequence matches source).
- [x] Codex round-1 → MAJOR ISSUES (HIGH + MED; both ADOPTED in-source).
- [x] Codex round-2 → MED (ADOPTED).
- [x] Codex round-3 → HIGH (ADOPTED).
- [x] Codex round-4 → NOT GREEN (HIGH + LOW; both ADOPTED).
- [x] Codex round-5 → GREEN — "Deploy-ready"; no new findings.
- [x] Codex marker refreshed at `.claude/state/codex-validate-last-run`.
- [x] Wrote compact SUMMARY.md to `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/`.
- [x] Updated handoff.md cycle-42B entry on top above cycle-42A.
- [x] Updated decision-log.md cycle-42B entry above cycle-42A.
- [x] Updated orchestration-state quartet (this file + current-cycle.md + validation-status.md + handoff-summary.md).
- [x] Slice closure commit on `apple-silicon-performance` (final action of session).
- [ ] Real-Xbox deployment: DEFERRED to next bounded slice (Hermes's call). Runbook delta vs cycle 42A documented in SUMMARY.md.

## Confidence + risk notes

- **HIGH confidence in the cycle-42B implementation correctness.** PE-level verification confirms the custom entry point is wired correctly: `AddressOfEntryPoint=0x3510` resolves to the thunk's first instruction; disassembly at the entry VA shows the expected SetupArgs → HalWriteSMBusValue → xbed_self_witness_fire → WinMainCRTStartup sequence with the correct register values (0xA8 SMBus addr, 0xFF offset, 0x0 FALSE, 0xA6 byte value, 0x6 stage code).
- **HIGH confidence in the cycle-42B discriminator semantics.** The joint (reserved0_last_stage, reserved1) interpretation is now precise per Codex R4.HIGH adoption. The cleanest "calling-context CONFIRMED" signal is (stage=0xA3, reserved1=5, count>=1) ⇒ k=1 ⇒ pre-WinMain was the first successful allocator AND all five WTNS-shim fires reached.
- **MEDIUM confidence in the cycle-42B 0xA6+count=0 INCONCLUSIVE diagnosis.** Per Codex R2 adoption, this outcome collapses three distinct failure modes that cycle 42B alone cannot separate: (a) pre-allocator fault (vsnprintf in pre-libc context); (b) allocator-then-post-guard rejection; (c) true allocator rejection. A future cycle 42C could split them via stage-6-specific shim bypass behavior OR additional EEPROM marker bytes at each decision point.
- **HIGH confidence in the smallest-blast-radius design choice.** cxbe inspection shows it propagates rather than invents entry points; therefore a per-XBE `-entry:` link flag is functionally equivalent to (and strictly smaller than) modifying cxbe's header generator. The cycle-22 calling-context axis is now testable WITHOUT nxdk source edits.
- **MEDIUM-HIGH confidence in Codex finding adoption.** All R1+R2+R3+R4 hard findings adopted via in-source documentation rewrites; R5 confirmed "Deploy-ready" with no residual findings, no open questions, no out-of-scope issues.
- **LOW risk to all prior-cycle invariants.** Cycle-23 lockstep / cycle-29 self-witness shim / cycle-31 paint / cycle-35 `.CRT$X*` slots / cycle-39 EEPROM-scratchpad + sticky-flag gate / cycle-41a..41e historical comment blocks + cycle-41c symmetric phys-range guards / cycle-41d page-alignment guard / cycle-42A multi-page redesign: ALL PRESERVED unchanged.

## What this session does NOT do

- NO host xemu source edits.
- NO `lib/xbed_self_witness.{c,h}` edits (cycle-29 + cycle-39 + cycle-41a..41e + cycle-42A invariants intact).
- NO `lib/xbed_a4_witness.{c,h}` edits (cycle-23 lockstep contract intact).
- NO `lib/lib.mk` edits.
- NO `oracle-agent/*` edits (cycle-39 v0.5 verbs intact; cycle-42B emits a stage-6 first call that the existing `witness.scan-self` verb reports without modification).
- NO `xbed_runtime.{c,h}` edits.
- NO `witness-only/main.c` edits (cycle-35 .CRT$XX*+XCU slots + cycle-31 paint + in-main fires intact; they now run as 2nd..5th calls to the idempotent shim).
- NO `witness-only/manifest.json` edits.
- NO `nxdk/` source edits (read-only inspection only).
- NO real-Xbox deployment in this session.
- NO PushNotification (implementation-only milestone; real-Xbox-validated outcome is the trigger for any future notification per project rule #16).
- NO cleanup of pre-existing untracked `.hermes_*` / `composite_preflight.py` files or pre-existing tracked drift.

## Next proposed action

Cycle 42B's bounded implementation+local-build+Codex sub-slice closes deploy-ready. The substantive next slice is cycle 42B real-Xbox deployment (Hermes's call): execute the runbook delta documented in `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/SUMMARY.md`, recover post-run `witness.scan-self` (count + reserved0 + reserved1) + `eeprom.scratch.read` + full eeprom hex dump, classify per the joint (reserved0_last_stage, reserved1) table. Three plausible outcomes documented with next-step actions for each.
