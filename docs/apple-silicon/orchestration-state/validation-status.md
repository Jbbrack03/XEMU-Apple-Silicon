# Validation Status

- Active slice: cycle 42B custom pre-WinMainCRTStartup entry-point thunk (calling-context discriminator) — bounded implementation+local-build+Codex sub-slice on the only remaining live cycle-22 axis. Mechanism: NEW `scripts/apple-silicon/xbe-tests/witness-only/witness_only_crt0.c` defining `witness_only_pre_winmain_crt_startup` (selected as PE entry via `-entry:` flag in `witness-only/Makefile`) which fires `xbed_self_witness_fire(stage=6)` BEFORE nxdk's `WinMainCRTStartup`, then calls into the standard nxdk startup so the rest of the runtime is bit-identical. ZERO nxdk source edits; ZERO `lib/*` edits.
- Validation state: **Rule #15 SATISFIED via Codex 5-round `changes`-mode review.** Verdict trajectory MAJOR ISSUES → MED → HIGH → NOT GREEN → GREEN (with explicit "Deploy-ready"). All hard findings ADOPTED via in-source documentation rewrites; ZERO C-source changes between R1 and R5. Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed. Build verified: PE `AddressOfEntryPoint=0x3510` (ImageBase 0x10000, VA 0x13510); `llvm-objdump` at 0x13510 confirms expected SetupArgs → HalWriteSMBusValue(0xA8, 0xFF, FALSE, 0xA6) → xbed_self_witness_fire(6) → WinMainCRTStartup sequence with correct register values. Final witness-only artifact SHA = `3cc670fb4df5871811d94bcc8339e5126ab0e61a78e44802ba9e655d742442a1` (155 648 B; same XBE page boundary as cycles 31..42A). **Real-Xbox deployment: DEFERRED to next bounded slice** (Hermes's call); runbook delta vs cycle 42A documented in `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/SUMMARY.md`.

## Rule #15 applicability (cycle 42B)

Cycle 42B is an IMPLEMENTATION slice (rule #15 trigger #2 fires — non-trivial uncommitted code in `scripts/apple-silicon/xbe-tests/witness-only/witness_only_crt0.c` + `Makefile`, apple-silicon-scripts scope). The cycle-42B diff is +391 / −1 LOC across 2 files (NEW witness_only_crt0.c with ~390 LOC of which ~280 LOC is in-source documentation per Codex adoption rounds, + 12-line Makefile addition); well above the trivial-work skip threshold. The prompt explicitly mandated Codex validation for this slice; 5 rounds executed accordingly.

Codex round summary (mode=`changes`):

### Round 1 — MAJOR ISSUES (1 HIGH + 1 MED + 1 LOW)

- **R1.HIGH (`0xA6 + count=0` row overclaim) — ADOPTED via documentation rewrite.** Codex flagged that the original `0xA6 + count=0` row claimed "calling-context ELIMINATED as axis" but that conclusion is not supported: the defensive EEPROM=0xA6 write happens BEFORE the call to `xbed_self_witness_fire`, and inside the shim several early-return / fault sites sit between the SMBus write and `MmAllocateContiguousMemory`. A vsnprintf fault inside `xbed_host_log_writef("enter stage=...")` (pre-libc-init context) yields the same observable 0xA6+count=0 without the allocator ever being attempted. **ADOPTED**: rewrote row as INCONCLUSIVE.
- **R1.MED (`0xA4 + count=0` row logically-impossible) — ADOPTED via row removal.** Codex flagged that the row described a flow the code cannot produce: if our thunk reaches the cycle-35 .CRT$XXC fire, the defensive pre-write has already landed 0xA6; the cycle-39 sticky-flag inside the shim is set on the first call so the .CRT$XXC fire cannot overwrite 0xA6 with 0xA4. **ADOPTED**: removed row, added explanatory paragraph.
- **R1.LOW (cosmetic "tail-call" wording) — ADOPTED via global replace.** Codex noted "tail-call" was inaccurate since the call returns. **ADOPTED**: `tail-call → call` throughout.

### Round 2 — MED (1 new)

- **R2.MED (post-allocation vs pre-allocation guard misattribution) — ADOPTED via failure-mode taxonomy rewrite.** Codex flagged that the phys-range / alignment guards do NOT sit between the SMBus write and the allocator — they sit POST-allocation, after `MmAllocateContiguousMemory` returns successfully and after `MmGetPhysicalAddress` succeeds, then they free and return 0. The "0xA6+count=0 means allocator-never-attempted" framing was too broad. **ADOPTED**: rewrote the 0xA6+count=0 paragraph to enumerate three distinct failure modes (a) pre-allocator fault (vsnprintf-pre-libc); (b) allocator-then-post-guard rejection (phys outside cycle-29 window or sub-page-aligned); (c) allocator-rejected (true G0(c) in pre-WinMainCRT context). Honest framing: cycle 42B alone cannot distinguish a/b/c from a single readback.

### Round 3 — HIGH (1 new)

- **R3.HIGH (success row over-interpreted reserved1) — ADOPTED via reserved1-keyed table.** Codex flagged that the original single success row `0xA6 + count>=1` couldn't distinguish "pre-WinMain succeeded as first allocator" from "pre-WinMain failed but later .CRT$XX*/in-main fire succeeded" — both leave EEPROM=0xA6 (defensive pre-write) + count>=1. Disambiguation requires looking at reserved1 (the shim's call counter, ticked +1 on each successful fire). **ADOPTED**: replaced the single success row with reserved1=5 / =4 / =1..3 rows mapping to "pre-WinMain was first allocator" / "pre-WinMain failed, .CRT$XXC succeeded" / "more pre-main fires failed."

### Round 4 — NOT GREEN (1 HIGH + 1 LOW)

- **R4.HIGH (reserved1+execution-died ambiguity) — ADOPTED via JOINT (reserved0_last_stage, reserved1) table.** Codex flagged that reserved1 alone is still insufficient because the cycle-22 leading hypothesis is "execution dies partway through startup" — a crash between fires can produce the same reserved1 for different first-successful-allocator indices. Example: pre-WinMain succeeds, .CRT$XXC succeeds, then crash before .CRT$XCU yields reserved1=2, which the R3 table mislabeled as "both .CRT$XX* failed and WTNS1 succeeded." **ADOPTED**: replaced the reserved1-only table with a JOINT (reserved0_last_stage, reserved1) table. Defined M = i-index of the last successful fire (from the reserved0 low-byte stage); k = M - reserved1 + 1 = i-index of the first successful allocator. Full 14-row matrix enumerated with stage byte → M mapping (0xA6 → 1, 0xA4 → 2, 0xA5 → 3, 0xA1 → 4, 0xA3 → 5). The cleanest "calling-context CONFIRMED" signal is (stage=0xA3, reserved1=5) ⇒ k=1 ⇒ pre-WinMain was the first successful allocator AND all five WTNS-shim fires reached.
- **R4.LOW (cycle-23 XCTR mis-attributed as WTNS-page consumer) — ADOPTED via clarification.** Codex flagged that prior wording said the in-main cycle-23 XCTR fires "reuse the page," which is wrong: only `xbed_self_witness_fire` touches the WTNS page; cycle-23 XCTR fires use the separate `xbed_a4_witness_fire` shim targeting the agent's persistent XCTR buffer. **ADOPTED**: clarified that XCTR fires are an independent witness path on the agent's XCTR buffer; removed XCTR fires from the WTNS-shim fire ordering list.

### Round 5 — GREEN — deploy-ready

- **No new findings, no open questions, no out-of-scope issues.** Codex explicit: "GREEN. Deploy-ready. R4.HIGH is addressed in-source: witness_only_crt0.c now switches to the joint (reserved0_last_stage, reserved1) interpretation, defines M and k, and enumerates the valid combinations. That matches the unchanged shim behavior in xbed_self_witness.c. R4.LOW is addressed in-source: witness_only_crt0.c now explicitly separates cycle-23 XCTR fires from the WTNS path, and the WTNS ordering listed there matches the actual WTNS fire sites in main.c."

Hard rule conflicts: NONE in the cycle-42B diff.

## Cycle-42B implementation outcome — build deploy-ready, hardware deployment deferred

The cycle-42B bounded sub-slice this session covers implementation + local-build + Codex validation. Real-Xbox deployment is the next bounded slice. The implementation produced:

| Artifact | Value |
|---|---|
| witness-only XBE SHA-256 (final, post-R5) | `3cc670fb4df5871811d94bcc8339e5126ab0e61a78e44802ba9e655d742442a1` |
| Size | 155 648 B (same XBE page boundary as cycles 31..42A) |
| PE AddressOfEntryPoint | 0x3510 (ImageBase 0x10000, virtual VA 0x13510) |
| Entry-point disassembly | SetupArgs → HalWriteSMBusValue(0xA8, 0xFF, FALSE, 0xA6) → xbed_self_witness_fire(6) → WinMainCRTStartup ✓ |
| nxdk source edits | ZERO |
| `lib/*` edits | ZERO |
| `oracle-agent/*` edits | ZERO |
| `witness-only/main.c` edits | ZERO |
| New files | `scripts/apple-silicon/xbe-tests/witness-only/witness_only_crt0.c` (~390 LOC) |
| Modified files | `scripts/apple-silicon/xbe-tests/witness-only/Makefile` (+12 / -0) |
| Codex marker | `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed |

## What cycle-42B discriminates (when deployed)

- **STRONG evidence FOR calling-context** if `eeprom.scratch.read=0xA6` AND `witness.scan-self count>=1` AND reserved0_last_stage=0xA3 AND reserved1=5 (i.e. k=1, pre-WinMain was the first successful allocator and all five WTNS-shim fires reached). Not formally conclusive because the pre-WinMain calling context differs from .CRT$X* on multiple axes simultaneously (no security cookie / no TLS / no libc / no thread / different return-address-on-stack); a single observation does not enumerate which property is the operative one — that requires cycle 43+ single-axis variations.
- **INCONCLUSIVE** if `eeprom.scratch.read=0xA6` AND `witness.scan-self count=0` — three distinct failure modes collapse: (a) vsnprintf-pre-libc fault before allocator; (b) allocator-then-post-guard rejection (phys outside cycle-29 window or sub-page-aligned); (c) true allocator rejection in pre-WinMain context. Cycle 42C+ would need stage-6-specific bypass behavior inside the shim OR additional EEPROM marker bytes to split a/b/c.
- **REGRESSION** if `eeprom.scratch.read=0x00` AND `witness.scan-self count=0` — the thunk's defensive SMBus write did NOT land. Either entry-point override didn't take (Makefile / link-flag issue) OR HalWriteSMBusValue faulted from pre-libc context. Re-validate build artifact + PE entry-point disassembly.
- **GRADIENT SIGNAL** if `eeprom.scratch.read=0xA6` AND `witness.scan-self count>=1` AND (stage, reserved1) indicates k>1 — pre-WinMain calling context is STRICTLY WORSE for the allocator than the later context that first succeeded. Informative about gradient direction (later context works, earlier doesn't); does NOT confirm calling-context as the failing axis for cycles 35..42A.

## Hypothesis state after cycle 42B (build/Codex-only sub-slice)

The cycle-22 leading hypothesis is still where cycle 42A left it: the failing constraint is NOT cache-policy + NOT the address range alone + NOT the page-alignment requirement + NOT the `-Ex` variant alone + NOT size-as-validation-axis at the smallest multi-page step. Remaining live cycle-22 candidate is **calling-context**. Cycle 42B's bounded implementation+local-build+Codex sub-slice now ships the discriminator; the cycle-22 narrowing itself awaits the real-Xbox deployment in the next bounded slice.

## What this validates / what is still in flight

- Rule #15 (Codex validation for non-trivial implementation): **SATISFIED** via Codex 5 rounds with all hard findings adopted; R5 explicit "Deploy-ready" / "GREEN."
- Rule #4 (no doc drift): handoff.md + decision-log.md + orchestration-state quartet updated with cycle-42B entries.
- Rule #5 (build tools when toolset is the limit): satisfied — the cycle-42B `-entry:` thunk extends the discriminator toolset to reach the calling-context axis without modifying nxdk or lib code.
- Cycle-40 EEPROM regression gate: **HELD** in design (cycle-39 sticky-flag inside the shim is preserved unchanged; cycle-42B adds a SEPARATE defensive write to the same byte with the same encoding so the post-run byte is deterministic).
- Cycle-29 consumer scan-window contract: **HELD** by the preserved cycle-41c symmetric phys-range guards + cycle-41d page-alignment guard + cycle-42A multi-page redesign.
- Cycle-23 lockstep + cycle-29 self-witness + cycle-31 paint + cycle-35 `.CRT$X*` slot + cycle-39 EEPROM-scratchpad + cycle-41a..41e historical comment blocks + cycle-42A multi-page redesign: **ALL preserved unchanged**.
- Cycle-22 leading hypothesis: still where cycle 42A left it (calling-context axis live); cycle 42B real-Xbox deployment is the next bounded slice that would advance the narrowing.
- M15 default-on shape: **still blocked** (cycle 42B real-Xbox deployment + downstream cycles).
- Eight default-on Apple Silicon flags: untouched. Cycle-42B changes no host xemu source. Project rule #11 not in play.

## Closing checklist

- [x] Source diff applied: NEW `witness_only_crt0.c` + 12-line `Makefile` addition.
- [x] Clean nxdk rebuild produced cycle-42B witness-only artifact (deployed-build SHA `3cc670fb…`; source bit-identical to Codex-R5-GREEN-confirmed state).
- [x] PE-level verification: AddressOfEntryPoint + entry instruction sequence match the source.
- [x] Codex 5 rounds executed; verdict trajectory MAJOR → MED → HIGH → NOT GREEN → GREEN ("Deploy-ready") with all hard findings adopted; Codex marker refreshed.
- [x] Evidence directory written with SUMMARY.md + 5× Codex prompts + 5× Codex outputs.
- [x] handoff.md / decision-log.md / orchestration-state quartet updated; prior cycles preserved unchanged below.
- [x] Closure commit lands on `apple-silicon-performance` (final action of this session).
- [ ] Real-Xbox deployment: DEFERRED to next bounded slice. Runbook delta vs cycle 42A documented.
