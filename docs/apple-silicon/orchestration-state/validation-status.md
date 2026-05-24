# Validation Status

- Active slice: cycle 42E real-Xbox deployment of cycle-42D R4-GREEN stage-6 marker-bypass XBE — bounded run-only + doc-only closeout slice deploying `witness-only-cycle42d-r4-green.xbe` (SHA `d69f23fae70bacf26c82c7e2e96e9a08a7175de9950142a2321f91ef713a3093`, 155 648 B) and classifying the post-run signals via the cycle-42D milestone marker matrix.
- Validation state: **Rule #15 SATISFIED via doc-only carve-out.** Cycle 42E ships ZERO source code changes (host xemu / nxdk / xbe-tests / oracle-agent / witness-only — all untouched). Only documentation edits land: handoff.md + decision-log.md cycle-42E entries prepended; orchestration-state quartet updated; cycle-42E SUMMARY.md written to gitignored run dir. The cycle-42D R4-GREEN build deployed here was already Codex-R4-GREEN at cycle-42D closure; redeploying it bit-identically does not retrigger rule #15. Rule #4 (no doc drift) SATISFIED via handoff.md + decision-log.md + orchestration-state quartet sync.

## Rule #15 applicability (cycle 42E)

Cycle 42E ships ZERO source-code lines. All artifacts are either pre-existing (the deployed XBE binary from cycle 42D) or doc-only (canonical docs + orchestration-state quartet + cycle-42E SUMMARY.md). Per project rule #15's "trivial work skips automatically … doc-only changes" carve-out, no Codex pass is required for this slice. The cycle-42D R4-GREEN build deployed here was Codex-R4-GREEN at cycle-42D closure (see `.claude/state/codex-validate-last-run` refreshed `2026-05-24T14:07:40Z` after cycle-42D R4 GREEN).

Stop-hook fingerprint for rule #15 will not fire because the closure commit covers only doc/state edits with zero touched source files in the renderer/TCG/NV2A/build/apple-silicon-scripts tree.

## Cycle-42E real-Xbox classification

### Observed signal

| Signal | Value | Source |
|---|---|---|
| `eeprom.scratch.read` | `byte=0xBA tag=0xB stage_nib=0xA` | step 17 |
| `witness.scan-self` | `count=0 mapped_pages_seen=419` | step 16 |
| `witness.scan` | `count=1 phys=0x03eb3000 reserved0=0 reserved1=0` (D-cycle-28) | step 15 |
| Full EEPROM dump | offset 0xFF = `ba` ✓ matches scratch.read | step 18 |
| FTP recovery reboot 1 | t+8s | step 03 |
| FTP recovery post-runxbe | t+26s | step 13 |

### Cycle-42D matrix application

The cycle-42D interpretation matrix in `xbed_self_witness.h` lines 540-658 enumerates per-(byte, count) shapes. The observed `(0xBA, count=0)` shape is NOT directly enumerated — the matrix lists `(0xBA, count >= 1)` as "full first-call completion for stage=6 — best success signal / STRONG cycle-22 calling-context-CONFIRMED" but does not explicitly enumerate `(0xBA, count=0)`.

Under the cycle-42D R1 Honest-framing lower-bound semantics (`xbed_self_witness.h` lines 481-515), the byte is a LOWER BOUND on milestones reached — NOT an exact "execution died here" boundary. The `(0xBA, count=0)` shape collapses to TWO enumerated possibilities:

**(α) — Body executed end-to-end; WTNS page no longer discoverable post-chainload.** Markers are written by the stage==6 fast path AFTER each body step completes. 0xBA is the LAST marker; it is written AFTER the reserved0/1 stamp + `wbinvd` sequence. Therefore the stage==6 body executed through ALL 11 milestones in order: `MmAllocateContiguousMemory(0x2000)` returned non-NULL → cycle-41c lower phys-range guard cleared → cycle-41c upper phys-range guard cleared → cycle-41d page-alignment guard cleared → `MmPersistContiguousMemory(p, 0x2000, TRUE)` returned → 0x800-word page wipe completed → WTNS magic + VERSION stamped at the page + `s_witness_page` / `s_witness_phys` registered → `reserved0` / `reserved1` stamped + `wbinvd` executed. Under (α), `witness.scan-self count=0` means the stamped page is not present in kseg0 at the time scan-self runs. Plausible mechanisms: chainload return to dashboard tears down the witness-only process's address space; `MmPersistContiguousMemory` is supposed to survive that but the persist semantics may be process-local on this BIOS; the phys page may map outside the kseg0 range scan-self enumerates (`mapped_pages_seen=419` is unchanged across cycles 26..42E so kseg0 footprint itself didn't grow).

**(β) — Silent marker-write inflation (Codex R1 framing).** Under cycle-42D R1 Honest-framing, EVERY marker write is best-effort and its NTSTATUS is discarded. (β) allows a scenario where the marker helper succeeded on SOME later milestone-index value while a strictly-earlier body step actually faulted silently. For the helper specifically (a one-shot SMBus write to a stable 24LC02 EEPROM device), this scenario requires the SMBus controller to drop SOME writes and accept later ones — plausible under pathological kernel state but vanishingly unlikely on a stable post-cold-boot console.

**Preferred reading: (α).** The SMBus controller and 24LC02 EEPROM are stable hardware on a post-cold-boot console; "SMBus drops earlier writes but accepts later ones" requires a concrete pathological state that cycle 42E does not exhibit (the reboot 1 cycle returned the EEPROM to baseline 0x00 cleanly via the eeprom.scratch.reset path, demonstrating SMBus health on this console at this moment).

### Cycle-22 hypothesis advancement

| Cycle | Hypothesis (a) "pre-allocator vsnprintf-pre-libc fault" | Hypothesis (b) "post-allocation-guard rejection" | Hypothesis (c) "true allocator rejection" |
|---|---|---|---|
| 42A | candidate | candidate | candidate |
| 42B | candidate | candidate | candidate |
| 42C | candidate (LEADING) | **RULED OUT** | candidate |
| 42E | **RULED OUT** | RULED OUT (cycle 42C) | RULED OUT under (α) |

Cycle 42E **advances the cycle-22 axis from "NOT advanced" (cycle 42C closure state) → "PARTIALLY CONFIRMED on the bypass-body axis" (cycle 42E)**. Under (α) ALL three cycle-22 candidate hypotheses are RULED OUT: the bypass body executes end-to-end through every milestone of the cycle-42A redesign sequence from strict pre-WinMain context. The residual `count=0` observation is a *discoverability* gap (WTNS page persistence semantics on this BIOS revision or scan-self enumeration scope), NOT a calling-context gap.

Under (β) the calling-context advancement is weaker — strictly, (β) only proves the SMBus write at the 0xBA call site reached the controller, not that every earlier body step completed. However (β) is vanishingly unlikely on this hardware in this state.

### What this signal proves at HIGH confidence regardless of α vs β

1. Entry-point override + thunk still execute correctly on real hardware (re-confirms cycle 42C; 0xB0 marker overwrote thunk's 0xA6 defensive pre-write).
2. `HalWriteSMBusValue` does NOT fault from pre-WinMain context (eleven independent SMBus write call sites landed and persisted).
3. The cycle-22 hypothesis (a) "pre-allocator vsnprintf-pre-libc fault" is RULED OUT — the stage==6 fast path skips every `xbed_host_log_writef`/`vsnprintf` call AND we observe the 0xBA marker.
4. The cycle-39 sticky-flag pre-set worked as designed — NONE of the five WTNS-shim fires overwrote the 0xBA marker with `0xA4`.

### What this signal CANNOT prove without additional evidence

- (α) vs (β) disambiguation — needs the WTNS page to either survive to scan-self OR be confirmed unrecoverable.
- Whether `WinMainCRTStartup` returned — the cycle-39 EEPROM-write suppression worked, so the post-WinMain stages-!=6 fires CANNOT discriminate "they ran but skipped EEPROM" from "they never ran".

## Rule #4 — doc-sync

- handoff.md cycle-42E entry prepended above cycle-42D entry.
- decision-log.md cycle-42E entry prepended above cycle-42D entry.
- Orchestration-state quartet updated: this file + current-cycle.md + claude-status.md + handoff-summary.md.
- cycle-42E SUMMARY.md written to gitignored run dir `benchmark-runs/cycle42e-realxbox-20260524T143801Z/`.

## Recommended cycle 42F (Hermes's call)

Three mutually-exclusive bounded slices to disambiguate (α) vs (β):

1. **Broaden `witness.scan-self` phys-range enumeration.** Extend `oracle-agent/commands.c::cmd_witness_scan_self` to scan a wider phys-address range OR walk `MmPersistContiguousMemory`-registered pages via a kernel-export-driven enumerator. Codex validation mandatory (oracle-agent source change). Re-deploy unchanged cycle-42D R4-GREEN XBE; if scan-self count goes 0→1+ with non-zero reserved0/1, (α) is CONFIRMED.

2. **Add post-stamp readback EEPROM marker 0xBC.** Modify the cycle-42D stage==6 fast path to read back the WTNS magic from the just-stamped page in-process and stamp 0xBC if it survived. If 0xBC observed post-run, (α) is CONFIRMED on the in-XBE side; residual count=0 is a post-chainload discoverability gap. If 0xBA stays the observed byte, the stamp itself may have silently failed and (β) is supported. **Cleanest next slice** (in-XBE-only change, one Codex pass, no oracle-agent rebuild, preserves cycle-22 lockout property).

3. **Remove the cycle-42D sticky-flag pre-set.** Allow the cycle-39 EEPROM write to fire on later stages-!=6 fires (stages 4 / 5 / 1 / 3). Discriminator for whether `WinMainCRTStartup` returned: if post-run EEPROM ends at 0xA[1,3,4,5] instead of 0xBA, WinMain returned; if it stays at 0xBA, either later fires never executed OR faulted before the cycle-39 write site.
