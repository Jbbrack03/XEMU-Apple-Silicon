# Current Cycle

- Cycle: 42B custom pre-WinMainCRTStartup entry-point thunk (calling-context discriminator) — **implementation+local-build+Codex slice COMPLETE on `apple-silicon-performance`; real-Xbox deployment DEFERRED to next bounded slice.** Cycle-42A closure exhausted the cycle-22 candidate set within the in-XBE / lib-only oracle workflow scope (5 axes — cache policy, address range, alignment, -Ex/non-Ex ABI, size — all G0(c) PERSISTS). The remaining live cycle-22 axis is calling-context. Cycle 42B's bounded slice tests that axis via a NEW XBE PE entry-point thunk (`witness_only_pre_winmain_crt_startup` in `scripts/apple-silicon/xbe-tests/witness-only/witness_only_crt0.c`) selected via `-entry:witness_only_pre_winmain_crt_startup` in the witness-only Makefile's NXDK_LDFLAGS. The thunk fires the cycle-29 self-witness shim (stage 6 = `WITNESS_ONLY_STAGE_PRE_WINMAIN_CRT`) BEFORE nxdk's standard `WinMainCRTStartup` runs — i.e. before `__security_init_cookie`, TLS size computation, `_PDCLIB_xbox_libc_init`, `_PDCLIB_xbox_run_pre_initializers`, and any `.CRT$X*` slot. A defensive `HalWriteSMBusValue(0xA8, 0xFF, FALSE, 0xA6)` runs as the very first instruction of the thunk so the cycle-40-shape EEPROM byte discriminator answers regardless of whether vsnprintf inside the shim faults from the pre-libc-init context. After the witness work the thunk calls `WinMainCRTStartup` so the rest of the runtime is bit-identical to a non-cycle-42B nxdk XBE. ZERO nxdk source edits; ZERO `lib/*` edits; ZERO oracle-agent edits. CRITICAL DESIGN INSIGHT: inspection of `nxdk/tools/cxbe/Xbe.cpp:222` shows cxbe does NOT invent an entry point — it propagates the PE optional-header entry-point address. Therefore the prompt's framing of candidate B as "modify nxdk/tools/cxbe" is unnecessary; the smallest meaningful implementation of candidate B is a per-XBE `-entry:` link flag (smaller blast radius, no nxdk-source impact).
- Started: 2026-05-24 (fresh bounded Claude Code session post cycle-42A closeout-sync).
- Closed (implementation+local-build+Codex sub-slice): 2026-05-24. Real-Xbox deployment is the next bounded slice; not executed in this session.
- State: **IMPLEMENTATION COMPLETE; Codex GREEN deploy-ready; real-Xbox deployment pending.** Code change: NEW `scripts/apple-silicon/xbe-tests/witness-only/witness_only_crt0.c` (~390 LOC, mostly documentation per Codex 5-round adoption) + 12-line addition to `witness-only/Makefile`. Build verified: PE `AddressOfEntryPoint=0x3510` (ImageBase=0x10000, virtual entry=0x13510); disassembly at 0x13510 confirms SetupArgs → HalWriteSMBusValue(0xA8,0xFF,FALSE,0xA6) → xbed_self_witness_fire(6) → WinMainCRTStartup. Cycle-42B witness-only artifact SHA = `3cc670fb4df5871811d94bcc8339e5126ab0e61a78e44802ba9e655d742442a1` (155 648 B; same XBE page boundary as cycles 31..42A; source bit-identical to codex-R5-GREEN-confirmed state). All cycle-23 lockstep + cycle-29 self-witness shim + cycle-31 paint + cycle-35 .CRT$XX*+XCU slots + cycle-39 EEPROM sticky gate + cycle-41a..41e historical comment blocks + cycle-42A multi-page redesign: PRESERVED unchanged.
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-24.
- HEAD at start: cycle-42A closeout-sync `1fca1bba62f110a106d3da9b2b7211c104546742` on `apple-silicon-performance`.
- Bounded goal (verbatim, from prompt): cycle 42B — calling-context axis discriminator via custom XBE-header callback before _start (candidate B from canonical docs). Job: determine minimal viable implementation path, then execute as much of the bounded slice as possible without drifting scope. Hard requirements: stay inside workspace; preserve pre-existing tracked drift + untracked .hermes_* artifacts; do NOT resume old session; keep scope tight on candidate B; update orchestration-state artifacts; Codex-validate non-trivial code changes; if real-Xbox validation becomes next correct step, proceed only if implementation/build/doc state is ready and assignment still feels bounded, otherwise stop at clean handoff.
- Result: **Implementation+build+Codex sub-slice COMPLETE.** (i) Inspected nxdk/tools/cxbe and discovered cxbe propagates rather than invents entry points; smaller-blast-radius `-entry:` flag approach selected over cxbe-source-edit. (ii) Inspected nxdk crt0 startup chain (`nxdk/lib/pdclib/platform/xbox/crt0.c`) — the WinMainCRTStartup body is `__security_init_cookie → TLS sizing → _PDCLIB_xbox_libc_init → _PDCLIB_xbox_run_pre_initializers (.CRT$XX* walk) → thrd_create(main_wrapper)`. Cycle-42B thunk inserts BEFORE all of these. (iii) Wrote `witness_only_crt0.c` (~390 LOC) with the thunk + 280-LOC documentation block covering rationale + discriminator semantics + risk surface + preserved invariants. (iv) Modified `witness-only/Makefile` to add `SRCS += witness_only_crt0.c` + `NXDK_LDFLAGS += -entry:witness_only_pre_winmain_crt_startup` (+12 / -0). (v) Clean nxdk rebuild after each Codex round; final SHA `3cc670fb…`. (vi) PE-level verification: `llvm-readobj` confirms AddressOfEntryPoint=0x3510; `llvm-objdump` at 0x13510 confirms expected instruction sequence. (vii) Codex 5 rounds (mode=`changes`): R1 MAJOR (HIGH "0xA6+count=0 overclaim" + MED "logically-impossible 0xA4+count=0 row") → R2 MED (post-allocation vs pre-allocation guard misattribution) → R3 HIGH (single-axis reserved1 success row overclaim) → R4 NOT GREEN (reserved1+execution-died ambiguity + cycle-23 XCTR misattribution) → R5 GREEN — explicit "Deploy-ready." All hard findings adopted via in-source documentation rewrites; ZERO code changes between R1 and R5. (viii) Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed. (ix) Evidence directory `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/` with SUMMARY.md + 5× Codex prompts + 5× Codex outputs (gitignored per project convention). (x) Real-Xbox deployment runbook delta vs cycle 42A documented in SUMMARY.md. (xi) Canonical docs (handoff.md + decision-log.md + orchestration-state quartet) updated. (xii) Bounded slice commit landed on `apple-silicon-performance` preserving pre-existing tracked drift + untracked `.hermes_*` artifacts + `composite_preflight.py` unstaged.

## Plan summary (this session, executed in order)

1. Read canonical docs/state (handoff.md cycle-42A entry, decision-log.md cycle-42A entry, orchestration-workflow §§3,4,7,8,9, orchestration-state quartet, witness-only README + main.c + Makefile + manifest, `lib/xbed_self_witness.{c,h}`).
2. Inspected git status (HEAD = cycle-42A closeout-sync `1fca1bba62`); confirmed pre-existing tracked drift + 25+ untracked `.hermes_*` + `composite_preflight.py` + 2 `lib/*.inl` to preserve unstaged.
3. Inspected `nxdk/tools/cxbe/Main.cpp` + `Xbe.cpp` + `Xbe.h` → discovered cxbe does NOT invent entry points (`Xbe.cpp:222`: `ep = x_Exe->m_OptionalHeader.m_entry + m_Header.dwPeBaseAddr`). The smallest meaningful candidate-B implementation is a per-XBE `-entry:` link flag.
4. Inspected `nxdk/lib/pdclib/platform/xbox/crt0.c` and `crt_initializers.c` + `tls.c` → mapped the WinMainCRTStartup body sequence; identified `__security_init_cookie` / TLS / `_PDCLIB_xbox_libc_init` / `.CRT$XX*` as everything cycle-42B's thunk runs BEFORE.
5. Wrote `witness-only/witness_only_crt0.c` with the thunk + extensive in-source documentation.
6. Modified `witness-only/Makefile`: `SRCS += witness_only_crt0.c` + `NXDK_LDFLAGS += -entry:witness_only_pre_winmain_crt_startup` + rationale comment.
7. Clean nxdk rebuild succeeded; `llvm-readobj` + `llvm-objdump` confirmed PE AddressOfEntryPoint and entry-point disassembly.
8. Codex round-1 → MAJOR ISSUES (HIGH overclaim + MED logical-impossibility); ADOPTED via documentation rewrite. Rebuild → new SHA.
9. Codex round-2 → MED (post-allocation guards mis-attributed); ADOPTED. Rebuild.
10. Codex round-3 → HIGH (success row over-interprets reserved1); ADOPTED via reserved1-keyed table. Rebuild.
11. Codex round-4 → NOT GREEN (reserved1+execution-died ambiguity + XCTR misattribution); ADOPTED via JOINT (reserved0_last_stage, reserved1) table + XCTR clarification. Rebuild.
12. Codex round-5 → GREEN — "Deploy-ready" with no new findings.
13. Wrote Codex marker at `.claude/state/codex-validate-last-run` (rule #15 compliance); archived 5× Codex prompts + 5× outputs to evidence dir.
14. Wrote compact SUMMARY.md to `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/`.
15. Updated handoff.md: cycle-42B entry on top above cycle-42A.
16. Updated decision-log.md: cycle-42B entry above cycle-42A.
17. Updated orchestration-state quartet (this file + `claude-status.md` + `validation-status.md` + `handoff-summary.md`).
18. Bounded slice commit on `apple-silicon-performance`.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` + 2 `lib/*.inl` preserved unstaged.
3. [x] Candidate-B entry path investigated in code (cxbe + nxdk crt0).
4. [x] Implementation: NEW `witness_only_crt0.c` + 12-line Makefile addition.
5. [x] Clean nxdk rebuild produced new witness-only artifact (final SHA `3cc670fb…`).
6. [x] PE-level verification: AddressOfEntryPoint + disassembly confirm thunk is the actual entry.
7. [x] Codex 5 rounds executed; trajectory MAJOR → MED → HIGH → NOT GREEN → GREEN; all hard findings adopted via in-source documentation rewrites; Codex marker refreshed.
8. [x] Evidence directory `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/` with SUMMARY.md + 5× codex prompts + 5× codex outputs (gitignored per project convention).
9. [x] Canonical docs/state updated: handoff.md + decision-log.md + orchestration-state quartet.
10. [x] Slice closure commit landed on `apple-silicon-performance` (next step at the end of this session).
11. [ ] Real-Xbox deployment: DEFERRED to next bounded slice. Runbook delta vs cycle 42A documented in `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/SUMMARY.md`.

## What this session does NOT do

- NO host xemu source edits.
- NO `lib/xbed_self_witness.{c,h}` edits (cycle-29 + cycle-39 + cycle-41a..41e + cycle-42A invariants intact).
- NO `lib/xbed_a4_witness.{c,h}` edits (cycle-23 lockstep contract intact).
- NO `lib/lib.mk` edits.
- NO `oracle-agent/*` edits (cycle-39 v0.5 verbs intact; cycle-42B emits a stage-6 first call that the existing `witness.scan-self` verb reports without modification).
- NO `xbed_runtime.{c,h}` edits.
- NO `witness-only/main.c` edits (cycle-35 .CRT$XX*+XCU slots + cycle-31 paint + in-main fires intact; they now run as 2nd..5th calls to the idempotent shim instead of 1st..4th as in cycle 42A).
- NO `witness-only/manifest.json` edits.
- NO `nxdk/` source edits (read-only inspection only of `tools/cxbe/Main.cpp` + `Xbe.cpp` + `Xbe.h` + `lib/pdclib/platform/xbox/crt0.c` + `crt_initializers.c` + `tls.c`).
- NO real-Xbox deployment in this session.
- NO tracked changes to scripts/apple-silicon/*.{sh,py} pre-existing drift; NO changes to 2 `lib/*.inl` pre-existing drift.
- NO PushNotification (implementation-only milestone; real-Xbox-validated outcome is the trigger for any future notification per project rule #16).
