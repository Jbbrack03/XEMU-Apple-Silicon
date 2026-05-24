# Validation Status

- Active slice: cycle 42A multi-page cycle-29 self-witness redesign — bounded implementation+run slice executing the cycle-41e closure's recommended candidate A. Bump the cycle-29 self-witness allocator request from cycle-41e's `MmAllocateContiguousMemory(0x1000u)` (1 page) to `MmAllocateContiguousMemory(0x2000u)` (2 pages, 8 KiB) in `lib/xbed_self_witness.c`; bump `MmPersistContiguousMemory(p, 0x2000u, TRUE)` and the page-wipe loop bound to match; WTNS magic + version + reserved0 + reserved1 header still lives ONLY at offset 0 of the FIRST page of the multi-page allocation; the second page is zero-filled by the page-wipe loop. Cycle-29 consumer at `oracle-agent/commands.c::cmd_witness_scan_self` comment-only updated (NO logic change). All cycle-29 / cycle-39 / cycle-41c symmetric phys-range guards / cycle-41d page-alignment guard preserved unchanged (guards check the FIRST page of the allocation, which is sufficient because the WTNS magic + header live exclusively on the first page; guard log strings relabeled cycle-41e → cycle-42A and updated to note the first-page-only nature of the check). Codex-validates (4 rounds), rebuilds witness-only XBE, deploys to physical Xbox, executes cycle-40-shape runbook, recovers post-run evidence, classifies against cycle-40 G0(c) regression gate plus new size-axis expectation.
- Validation state: **Rule #15 SATISFIED via Codex 4-round `changes`-mode review.** Verdict trajectory MINOR ISSUES → MINOR ISSUES → NOT-GREEN (with R3 MED ADOPTED) → GREEN (with explicit "Deploy-ready"). All hard findings ADOPTED. Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed. **OUTCOME: G0(c) PERSISTS under cycle-42A 0x2000 multi-page allocation** per the cycle-40 G-row discriminator table. **Cycle-42A provides STRONG evidence AGAINST cycle-22 branch (c) "`size=0x1000`-specific interaction" being the failing axis at the smallest multi-page step; the cycle-22 candidate set within in-XBE / lib-only oracle workflow scope is now EXHAUSTED.**

## Rule #15 applicability (cycle 42A)

Cycle 42A is an IMPLEMENTATION slice (rule #15 trigger #2 fires — uncommitted code in `lib/xbed_self_witness.{c,h}` + `oracle-agent/commands.c` is renderer-adjacent + apple-silicon-scripts scope). The cycle-42A diff is +345 / −33 LOC across 3 files (3-site load-bearing literal change + Honest-framing comment rewrite + sign-flip-corrected wording + cycle-29 overview history-fence + function-contract docstring rewrite + ~95-LOC Cycle-42A Safety-notes subsection + paired comment block + header doc updates + consumer comment update + 3 guard log relabels), well above the trivial-work skip threshold. The prompt explicitly mandated Codex validation for this slice; 4 rounds executed accordingly.

Codex round summary (mode=`changes`):

### Round 1 — MINOR ISSUES (1 MED + 1 LOW)

- **R1.MED (header-staleness) — ADOPTED via function-contract docstring rewrite + cycle-29 option-(c) overview history-fence.** Codex flagged that several header comments still described the live implementation as a one-page `MmAllocateContiguousMemoryEx` path despite the code now using a two-page non-`-Ex` allocation. Adoption: rewrote the function-contract docstring with explicit "Current live behavior (CYCLE-42A multi-page redesign; supersedes the cycles-29..41e single-page behavior described in the Safety-notes block above)" paragraph + history-fence pointer to the new Cycle-42A Safety-notes subsection. Also updated the cycle-29 option-(c) overview paragraph to explicitly distinguish cycle-29 original design (1-page, `-Ex`) from cycle-42A current live behavior (2-page, non-`-Ex`) with a forward pointer to the Cycle-42A subsection.
- **R1.LOW (consumer `count>=2` leak shape) — ADOPTED via consumer-comment rewrite.** Codex flagged that the `oracle-agent/commands.c::cmd_witness_scan_self` `count>=2` paragraph said "each diag-XBE run leaks one persistent page" — inaccurate for cycle-42A (now 2 pages per allocation). Adoption: rewrote to say "ONE persistent allocation" per run (not "one persistent page"), with cycle-42A note clarifying each allocation is now 2 pages (0x2000 B) instead of 1 page (0x1000 B).

### Round 2 — MINOR ISSUES (1 new MED)

- **R2.MED (logically-impossible "not conclusive" example) — ADOPTED via replacement with internally-distinct-code-path rationale in both `.c` and `.h`.** Codex flagged that the cycle-42A "not conclusive" example was logically impossible in both the `.c` Rationale block and the `.h` Safety-notes Cycle-42A subsection: the example said a shared allocator artifact could leave "a 2-page contiguous run free when no 1-page hole was available," but any free 2-page contiguous run necessarily contains a free 1-page hole — so the rationale could not explain "0x2000 succeeds where 0x1000 failed". Adoption: replaced the broken example in both `.c` and `.h` with an internally-distinct-code-path rationale (the kernel allocator may route single-page and multi-page contiguous requests through DIFFERENT internal code paths — size-bucketed free lists, separate pool arenas, or distinct minimum-size policies for contiguous-memory allocations from a pre-`main()` calling context); each replacement notes the Codex R2 adoption in-source.

### Round 3 — NOT-GREEN (1 new MED)

- **R3.MED (`mapped_pages_seen` misattribution) — ADOPTED via consumer comment rewrite.** Codex flagged that the new `commands.c` note said `mapped_pages_seen` "grows by 2 per retained cycle-42A allocation," but the implementation increments `mapped_pages_seen` for every mapped scan page before the WTNS filter, then reports it separately from `count` — that counter is the kseg0 survey counter, not an allocation counter. Adoption: rewrote the consumer comment to:
  - reaffirm `count` grows by exactly 1 per cycle-42A allocation (only first page carries WTNS magic; second page is zero-filled and fails the magic predicate);
  - clarify `mapped_pages_seen` is the kseg0 survey counter (every page in scan window with non-zero `MmGetPhysicalAddress`);
  - note retail Xbox kseg0 identity mapping for physical RAM is generally persistent across reboots so the counter is typically stable (cycles 41a..41e all reported 419);
  - explicitly state `mapped_pages_seen` is NOT a load-bearing signal for the cycle-42A allocation-shape interpretation.

### Round 4 — GREEN — deploy-ready

- **No new findings, no open questions, no out-of-scope issues.** Codex explicit: "GREEN — deploy-ready. The R3 MED is addressed, and the revised `cmd_witness_scan_self` wording now matches the actual producer/consumer behavior for `count` vs `mapped_pages_seen`." Per Codex's strengths section: the producer correctly allocates `0x2000`, persists `0x2000`, zeroes the full `0x2000`, and stamps WTNS only on the first page; the consumer's unchanged `0x1000` stride will still report one hit per allocation; the existing visibility guards remain in the right place relative to the first-page stamp; the new comments keep the interpretation honest about size not being a pure single-axis discriminator; the "typically stable 419" note is grounded in local project evidence (`handoff.md:3` records `mapped_pages_seen=419` reproduced across prior cycle readbacks).

Hard rule conflicts: NONE in the cycle-42A source diff.

## Cycle-42A outcome — G0(c) PERSISTS under 0x2000 multi-page allocation

Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan shape)`:

| Signal | Observed (cycle 42A) | Cycle 41e | Cycle 41d | Cycle-40 expected for G0(c) | Match |
|---|---|---|---|---|---|
| `eeprom.scratch.read` byte at 0xFF | `0xA4` (tag=0xA, stage_nib=0x4) | `0xA4` | `0xA4` | `0xA4` | ✓ G0(c) PERSISTS |
| `witness.scan-self` count | `0` | `0` | `0` | `0` | ✓ |
| `witness.scan-self` reserved1 | n/a (count=0) | n/a | n/a | n/a | ✓ |
| `witness.scan` shape | D-cycle-27 (count=1 phys=0x03eb3000 reserved=0) | D-cycle-27 | D-cycle-27 | D-cycle-27 | ✓ |
| Full `eeprom` hex dump last byte | `A4` | `A4` | `A4` | `A4` | ✓ |
| Dashboard FTP recovery | t+36s | t+16s | t+19s | (no specific table expectation) | within graceful HalReturnToFirmware shape band (cycle 36 t+38s); recovery timing NOT load-bearing for G-row classification |

**G0(c) uniquely selected (same as cycles 40 + 41a + 41b + 41c + 41d + 41e).** The cycle-42A 0x2000 multi-page allocation did NOT shift the outcome to G1..G4 (which would require `witness.scan-self count >= 1`). The cycle-42A hypothesis "multi-page allocation succeeds where single-page fails" is REJECTED. The broader cycle-22 branch (c) "`size=0x1000`-specific interaction" is provided STRONG-but-not-conclusive evidence AGAINST being the failing axis at the smallest multi-page step.

## What cycle-42A discriminates

- **STRONG evidence AGAINST** the simpler "kernel-validation rejects size=0x1000 specifically" hypothesis (sub-claim of cycle-22 branch (c)). Both 0x1000 (cycles 40..41e) AND 0x2000 (cycle 42A) allocator requests are rejected identically from the cycle-29 `.CRT$XXC` slot calling context.
- **DOES NOT FORMALLY ELIMINATE** branch (c) on its own. The kernel allocator may route single-page and multi-page contiguous requests through different internal code paths (size-bucketed free lists / separate pool arenas / distinct minimum-size policies for contiguous-memory allocations from a pre-`main()` calling context); a multi-page success or failure may reflect that internal code-path divergence rather than a pure "size-as-validation-axis" signal. A formal closure of branch (c) would require sweeping size across multiple multi-page steps OR cross-validation with candidate B.
- **EXHAUSTS the cycle-22 candidate set within the in-XBE / lib-only oracle workflow scope.** Five bounded axes have been varied (cache-policy, address-range, alignment, entry-point, size) with every variation producing G0(c) PERSISTS. The only remaining live axis is calling-context.
- **OUT OF cycle-42A scope:** calling-context testing requires moving allocation to a pre-CRT context via custom XBE-header callback before `_start` (cycle 42 candidate B — requires modifying nxdk's XBE-header generator at `nxdk/tools/cxbe/`).

## Hypothesis state after cycle 42A

The cycle-22 leading hypothesis is FURTHER NARROWED beyond cycle-41e's narrowing:

The failing constraint is NOT cache-policy (cycle 40 + 41a + 41b exhausted bare RW / NOCACHE / WRITECOMBINE under `-Ex`), NOT the address range alone (cycle 41c eliminated under `-Ex` via matched-tuple), NOT the page-alignment requirement (cycle 41d eliminated under `-Ex` via `Alignment=0u`), NOT the `-Ex` validation logic alone (cycle 41e: non-`-Ex` fails identically at 0x1000), AND NOT size-as-validation-axis at the smallest multi-page step (cycle 42A: 0x2000 fails identically). The remaining live cycle-22 candidate is **calling-context** — currently inaccessible without moving the allocation to a pre-CRT context (cycle 42 candidate B).

Reproducibility shape continues:
- `phys=0x03eb3000` deterministic kernel-pool reuse: 26+ consecutive observations across cycles 26..42A.
- `mapped_pages_seen=419` at every readback (kseg0 survey counter; stable across runs on retail).
- EEPROM non-volatility across the pre-chainload reboot confirmed (pre-baseline byte=0xA4 survived; post-`eeprom.scratch.reset` byte=0x00 was the engineered baseline; post-cycle-42A-run byte=0xA4 is the cycle-39 breadcrumb that landed during the cycle-42A first call).

## What this validates / what is still in flight

- Rule #15 (Codex validation for non-trivial implementation): **SATISFIED** via Codex 4 rounds with all hard findings adopted; R4 explicit "Deploy-ready" / "GREEN — deploy-ready".
- Cycle-40 EEPROM regression gate (`byte = 0xA4` after any cycle-39+ chainload): **HELD** by cycle-42A (sticky `s_eeprom_scratch_attempted` flag from cycle-39 / Codex round-2 P1 fix continues to fire correctly through the cycle-42A multi-page first call).
- Cycle-29 consumer scan-window contract (`[0x80010000, 0x84000000]`, 0x1000 stride): **HELD** by the preserved cycle-41c symmetric phys-range guards + the preserved cycle-41d page-alignment guard (both check the FIRST page of the cycle-42A multi-page allocation, which is sufficient because the WTNS magic + header live exclusively on the first page).
- Cycle-23 lockstep + cycle-29 self-witness + cycle-31 paint + cycle-35 `.CRT$X*` slot + cycle-39 EEPROM-scratchpad mechanism + cycle-41a..41e historical comment blocks + cycle-41c symmetric phys-range guards + cycle-41d page-alignment guard: **ALL preserved**.
- Cycle-22 leading hypothesis: FURTHER NARROWED — candidate set within in-XBE / lib-only scope EXHAUSTED. Cycle 42 candidate B (pre-`_start` callback via `nxdk/tools/cxbe/`) is the necessary follow-up for the remaining calling-context axis.
- M15 default-on shape: **still blocked** (cycle 42B+ calling-context-axis work + downstream cycles).
- Eight default-on Apple Silicon flags: untouched. Cycle-42A changes no host xemu source. Project rule #11 not in play.

## Closing checklist

- [x] Source diff applied: 3-site 0x1000u → 0x2000u literal change + cycle-42A bounded-variation comment block + Honest-framing rewrite + sign-flip-corrected wording + cycle-29 overview history-fence + function-contract docstring rewrite + Cycle-42A Safety-notes subsection + consumer comment update + 3 guard log relabels.
- [x] Clean nxdk rebuild produced new witness-only artifact (deployed-build SHA `698e6fef…`; source bit-identical to codex-R4-confirmed state).
- [x] Codex 4 rounds executed; verdict trajectory MINOR → MINOR → NOT-GREEN (R3 MED ADOPTED) → GREEN ("Deploy-ready") with all hard findings adopted; Codex marker refreshed.
- [x] Real-Xbox runbook executed end-to-end (cycle-40 shape adapted for new witness shape).
- [x] G0(c) PERSISTS classification confirmed via three primary signals + full EEPROM hex dump cross-check.
- [x] Evidence directory written with SUMMARY.md + 17 step-numbered logs + 4× Codex prompts + 4× Codex outputs.
- [x] handoff.md / decision-log.md / orchestration-state quartet updated; prior cycles preserved unchanged below.
- [x] Closure commit landed on `apple-silicon-performance` as `aa2981ee3ab1a52434d7a147826230417d11a723`. Bounded closeout-sync follow-up commit on top syncs orchestration-state quartet to reference the landed hash.
