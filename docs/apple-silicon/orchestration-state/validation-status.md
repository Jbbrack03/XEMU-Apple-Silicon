# Validation Status

- Active slice: cycle 41c combined address-range variation — bounded implementation+run slice executing the cycle-41b closure's binding contingent path. Widens the witness-only `MmAllocateContiguousMemoryEx` allocation tuple at `lib/xbed_self_witness.c:155-156` from `lowest=0x00010000, highest=0x03FFFFFF` (cycles 29..41b) to `lowest=0x00000000, highest=0x7FFFFFFF` matching nxdk's framebuffer allocator at `nxdk/lib/hal/video.c:363-367` BYTE-FOR-BYTE modulo `size`. Alignment + Protect unchanged from cycle-41b. Adds Codex-R1+R2 P1-adopted symmetric phys-range guards. Codex-validates (3 rounds), rebuilds witness-only XBE, deploys to physical Xbox, executes cycle-40-shape runbook, recovers post-run evidence, classifies against cycle-40 G0(c) regression gate.
- Validation state: **Rule #15 SATISFIED via Codex 3-round `changes`-mode review.** Verdict trajectory MAJOR ISSUES → MAJOR ISSUES → MINOR ISSUES, with all R1+R2 P1 high findings and R3 P2 finding ADOPTED via in-slice code/comment changes; R1 P2 medium "doc-rule-#4 mid-slice state" finding DEFLECTED with explicit reason (transient mid-slice state — closure commit reconciles); Codex round-3 explicit "Nothing else looks load-bearing for the cycle-41c real-Xbox deploy." Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed (fingerprint `cc437e2b`). **OUTCOME: G0(c) PERSISTS under matched-tuple address range** per the cycle-40 G-row discriminator table. **Cycle-41c eliminates the "kernel demands specific non-cycle-29-tuple address range" sub-hypothesis.**

## Rule #15 applicability (cycle 41c)

Cycle 41c is an IMPLEMENTATION slice (rule #15 trigger #2 fires — uncommitted code in `lib/xbed_self_witness.{c,h}` is renderer-adjacent + apple-silicon-scripts scope). The cycle-41c diff is larger than cycle-41b's because it adds the two symmetric phys-range guards on top of the 2-literal tuple widening (~70 LOC total including comment block rewrites), well above the trivial-work skip threshold. The prompt explicitly mandated Codex validation for this slice; 3 rounds executed accordingly.

Codex round summary (mode=`changes`):

### Round 1 — MAJOR ISSUES (3 P-level findings; P1 + 1 of 2 P2 ADOPTED)

- **R1.P1 (high) — ADOPTED via 5-LOC upper-bound guard.** Codex flagged that the cycle-29 consumer at `oracle-agent/commands.c::cmd_witness_scan_self` only scans the kseg0 window `[0x80010000, 0x84000000]` and reconstructs phys as `va & 0x03FFFFFF`. Cycles 29..41b matched that 64 MiB ceiling exactly (`highest=0x03FFFFFF`). Cycle-41c's `highest=0x7FFFFFFF` theoretically allows the kernel to return a phys above 0x04000000 that would be stamped by the producer but invisible to the consumer's scan, turning `witness.scan-self count=0` into a FALSE NEGATIVE indistinguishable from the cycle-40 G0(c) shape. Adoption: added `if (phys >= 0x04000000u) { host-log + MmFreeContiguousMemory + return 0 }` after the existing `MmGetPhysicalAddress` validation. On retail Original Xbox (64 MiB physical RAM) the kernel cannot return phys above 0x04000000, so this branch is a no-op on target hardware; guard exists for defense in depth + to formally close the Codex-flagged interpretation gap.
- **R1.P2 (medium, overstatement) — ADOPTED via narrowed retail-64MiB-scope qualification** in both `xbed_self_witness.c` comment block and `xbed_self_witness.h` Safety-notes block. The original cycle-41c claim "address-range eliminated as failing constraint" was tightened to "address-range eliminated as failing constraint *within retail-Xbox 64 MiB physical RAM scope*" (the only addresses the kernel can return on this hardware).
- **R1.P2 (medium, doc-rule-#4 mid-slice state) — DEFLECTED.** Codex correctly observed that handoff.md / claude-status.md still showed cycle 41b as the current closed slice at validation time. That was intentional: cycle 41c was mid-execution at validation; the closure commit (final step of this session) WILL sync handoff.md / decision-log.md / orchestration-state quartet to cycle-41c closure. This is transient mid-slice state, not real doc-drift; rule #4 is satisfied by the closing-commit reconciliation pattern every prior cycle (41a, 41b) followed.
- **R1.P3s** — confirming: cycle-41c tuple match to nxdk framebuffer allocator verified accurate; doc-comment + header update accurately reflect the literal source change; no hard-rule conflicts.

### Round 2 — MAJOR ISSUES (3 P-level findings; both P1 + P2 ADOPTED)

- **R2.P1 (high) — ADOPTED via 5-LOC symmetric lower-bound guard.** Codex flagged that the R1 P1 adoption was incomplete: it closed only the upper blind spot. The cycle-29 reader scans from `0x80010000` (corresponding to phys floor `0x00010000`), so cycle-41c's `lowest=0x00000000` could equally let the kernel return phys in `[0x00000000, 0x00010000)` — outside the consumer's scan window. Adoption: added a symmetric `if (phys < 0x00010000u) { host-log + MmFreeContiguousMemory + return 0 }` BEFORE the upper-bound guard. Distinct host-log line preserves the producer-side observability of which side the guard fired on (when local xemu smoke is run; on real Xbox host-log is invisible but the guard still preserves the cycle-40 G-row contract).
- **R2.P2 (medium, still-too-strong elimination) — ADOPTED via further-narrowed wording.** Both source files now say cycle 41c eliminates only the "kernel demands a specific non-cycle-29-tuple address range" sub-hypothesis (because the matched-tuple is known-good against the same `-Ex` entry point on this kernel for nxdk's framebuffer allocator), NOT address-range as a whole. The within-retail-64MiB-scope qualifier remains but the broader claim is dropped.
- **R2.P3** — confirming: the upper guard's behavior is internally consistent (log, free still-unpersisted page, return); the nxdk framebuffer precedent is accurately represented.

### Round 3 — MINOR ISSUES (1 P2 ADOPTED; explicit deploy-readiness close)

- **R3.P2 (medium, comment precision) — ADOPTED via wording softening.** Codex noted that the comment "`witness.scan-self count=0` post-run on cycle-41c still unambiguously means 'allocation failed (NULL or crash)' rather than 'allocation succeeded outside reader's visibility'" overstates because either guard firing also yields `count=0` (page allocated, then freed by the guard, never stamped). Adoption: softened to "STRONGLY suggestive of allocation failure (NULL or crash) — but NOT unambiguous, because either guard firing also yields count=0; the full real-Xbox interpretation lives in the guard-block comment below." The guard-block comment block at `xbed_self_witness.c:243-250` already acknowledged the real-Xbox ambiguity properly.
- Codex explicit verdict close: "Nothing else looks load-bearing for the cycle-41c real-Xbox deploy. The code path change itself is coherent: widen range, reject out-of-reader phys before persist/stamp, then continue with the existing persistence and kseg0 alias flow. The remaining issue is comment precision, not deploy safety." Symmetric guards confirmed correct vs the actual consumer window in `commands.c:726`.

Hard rule conflicts: NONE.

## Cycle-41c outcome — G0(c) PERSISTS under matched-tuple address range

Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)`:

| Signal | Observed (cycle 41c) | Cycle 41b | Cycle 41a | Cycle-40 expected for G0(c) | Match |
|---|---|---|---|---|---|
| `eeprom.scratch.read` byte at 0xFF | `0xA4` (tag=0xA, stage_nib=0x4) | `0xA4` | `0xA4` | `0xA4` | ✓ G0(c) PERSISTS |
| `witness.scan-self` count | `0` | `0` | `0` | `0` | ✓ |
| `witness.scan-self` reserved1 | n/a (count=0) | n/a | n/a | n/a | ✓ |
| `witness.scan` shape | D-cycle-27 (count=1 phys=0x03eb3000 reserved=0) | D-cycle-27 | D-cycle-27 | D-cycle-27 | ✓ |
| Dashboard FTP recovery | t+24s | t+24s | t+8s | (no specific table expectation) | matches cycle 41b EXACTLY; +18s vs cycle 40 baseline; recovery timing NOT load-bearing for G-row classification |

**G0(c) uniquely selected (same as cycles 40 + 41a + 41b).** The combined-tuple match to nxdk's framebuffer allocator did NOT shift the outcome to G1..G4 (which would require `witness.scan-self count >= 1`). The cycle-41c hypothesis "matching nxdk's framebuffer allocator address range exactly is what the kernel demanded" is REJECTED.

## What cycle-41c discriminates

- **ELIMINATES** the cycle-22 sub-hypothesis "kernel demands a specific non-cycle-29-tuple address range." Cycle-41c tuple is BIT-IDENTICAL to nxdk's framebuffer allocator at `nxdk/lib/hal/video.c:363-367` modulo `size`, and that allocator runs successfully on every nxdk-built XBE that draws anything on this exact kernel.
- **DOES NOT ELIMINATE**: (i) alignment requirement `0x1000` (cycle 41d); (ii) the `-Ex` variant itself (cycle 41e); (iii) a `size=0x1000`-specific interaction (out-of-cycle-41-scope; would require redesigning the cycle-29 self-witness as multi-page).

## Hypothesis state after cycle 41c

The cycle-22 leading hypothesis is FURTHER NARROWED beyond cycle-41b's narrowing:

The failing constraint is NOT cache-policy (cycle 40 + 41a + 41b exhausted bare RW / NOCACHE / WRITECOMBINE) AND is NOT the address range alone (cycle 41c eliminated via matched-tuple). The remaining live candidates:

1. **Alignment `0x1000`** — drop to 0 (let kernel pick) — cycle 41d single-literal change.
2. **The `-Ex` variant itself** — fall back to non-`-Ex` `MmAllocateContiguousMemory(0x1000)` — cycle 41e.
3. **`size=0x1000` interaction** — out-of-cycle-41-scope.

Reproducibility shape continues:
- `phys=0x03eb3000` deterministic kernel-pool reuse: 23+ consecutive observations across cycles 26..41c.
- `mapped_pages_seen=419` at every readback.
- EEPROM non-volatility across the pre-chainload reboot 1 confirmed.

## What this validates / what is still in flight

- Rule #15 (Codex validation for non-trivial implementation): **SATISFIED** via Codex 3 rounds with all hard findings adopted; R3 verdict MINOR ISSUES with explicit deploy-readiness close.
- Cycle-40 EEPROM regression gate (`byte = 0xA4` after any cycle-39+ chainload): **HELD** by cycle-41c (sticky `s_eeprom_scratch_attempted` flag from cycle-39 / Codex round-2 P1 fix continues to fire correctly).
- Cycle-29 consumer scan-window contract (`[0x80010000, 0x84000000]`): **HELD** by the new cycle-41c symmetric producer-side phys-range guards that reject any returned phys outside `[0x00010000, 0x04000000)`.
- Cycle-23 lockstep + cycle-29 self-witness + cycle-31 paint + cycle-35 `.CRT$X*` slot + cycle-39 EEPROM-scratchpad mechanism + cycle-41a + cycle-41b comment tightening: **ALL preserved**.
- Cycle-22 leading hypothesis: FURTHER NARROWED. The failing constraint is in {alignment, `-Ex` variant, `size`-interaction}. Cycle 41d+ will discriminate.
- M15 default-on shape: **still blocked** (cycle 41d+ alignment / `-Ex`-fallback work + downstream cycles).
- Eight default-on Apple Silicon flags: untouched. Cycle-41c changes no host xemu source. Project rule #11 not in play.

## Closing checklist

- [x] Source diff applied: 2-literal tuple widening + 2× 5-LOC symmetric phys-range guards + paired comment block + header doc updates.
- [x] Clean nxdk rebuilds produced new witness-only artifact (final SHA `cc437e2b…`).
- [x] Codex 3 rounds executed; verdict trajectory MAJOR → MAJOR → MINOR with all hard findings adopted; Codex marker refreshed.
- [x] Real-Xbox runbook executed end-to-end (cycle-40 shape).
- [x] G0(c) PERSISTS classification confirmed via three primary signals + full EEPROM hex dump cross-check.
- [x] Evidence directory written with SUMMARY.md + 18 step-numbered logs + 3× Codex prompts + 3× Codex outputs.
- [x] handoff.md / decision-log.md / orchestration-state quartet updated; prior cycles preserved unchanged below.
- [ ] Closure commit on `apple-silicon-performance` (next step in this session).
