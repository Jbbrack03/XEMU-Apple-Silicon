# Validation Status

- Active slice: cycle 41e non-`-Ex` fallback variation — bounded implementation+run slice executing the cycle-41d closure's binding contingent path. Replaces the cycles-29..41d `MmAllocateContiguousMemoryEx(0x1000u, 0x00000000u, 0x7FFFFFFFu, 0u, PAGE_READWRITE | PAGE_WRITECOMBINE)` 5-arg call in `lib/xbed_self_witness.c` with the non-`-Ex` `MmAllocateContiguousMemory(0x1000u)` 1-arg call (the 1-arg form is the actual nxdk API per `nxdk/lib/xboxkrnl/xboxkrnl.h:3473-3476`; the cycle-41d closure docs described a 2-arg signature that would not compile against the nxdk header). All cycle-29 / cycle-39 / cycle-41c symmetric phys-range guards / cycle-41d page-alignment guard preserved unchanged (still load-bearing because non-`-Ex` ABI cedes all of Protect / placement / Alignment to kernel defaults; cycle-29 consumer scan window `[0x80010000, 0x84000000]` 0x1000-stride unchanged). Codex-validates (4 rounds), rebuilds witness-only XBE, deploys to physical Xbox, executes cycle-40-shape runbook, recovers post-run evidence, classifies against cycle-40 G0(c) regression gate.
- Validation state: **Rule #15 SATISFIED via Codex 4-round `changes`-mode review.** Verdict trajectory MAJOR ISSUES → MAJOR ISSUES → MINOR ISSUES (with explicit "Deploy-readiness: green") → MINOR ISSUES (residual LOW "Comment-only, not a deployment blocker" DEFERRED), with all hard findings ADOPTED and 1 LOW finding REBUTTED with grep evidence (line vs ordinal). Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed. **OUTCOME: G0(c) PERSISTS under non-`-Ex` fallback** per the cycle-40 G-row discriminator table. **Cycle-41e provides STRONG evidence AGAINST cycle-22 branch (b) "the `-Ex` variant itself is the failing constraint" being the SOLE failing constraint; cycle-41 SCOPE EXHAUSTED.**

## Rule #15 applicability (cycle 41e)

Cycle 41e is an IMPLEMENTATION slice (rule #15 trigger #2 fires — uncommitted code in `lib/xbed_self_witness.{c,h}` is renderer-adjacent + apple-silicon-scripts scope). The cycle-41e diff is moderate (+273 / -198 LOC across 2 files including the load-bearing 1-literal change, Honest-framing rewrite, sign-flip correction, degenericized cycle-39 breadcrumb, softened unsupported claims, paired comment block + header doc updates, three guard log relabels), well above the trivial-work skip threshold. The prompt explicitly mandated Codex validation for this slice; 4 rounds executed accordingly.

Codex round summary (mode=`changes`):

### Round 1 — MAJOR ISSUES (3 P-level findings)

- **R1.P1 (medium) — DEFERRED to cycle-41e closure commit.** Codex flagged that the cycle-41d closure docs (handoff.md, decision-log.md, claude-status.md) describe the cycle-41e substitution as a 2-arg `MmAllocateContiguousMemory(size, protect)` call against the actual 1-arg nxdk API. This is a rule-#4 cross-doc drift the cycle-41e source diff cannot fix; the cycle-41e closure commit IS the doc-sync step where handoff.md + decision-log.md + the orchestration-state quartet get the cycle-41e entries with the corrected 1-arg call surface and the cycle-41e outcome. Deferral is the correct disposition; R2 confirmed acceptable.
- **R1.P2 (medium) — ADOPTED via Honest-framing paragraph.** Codex flagged that the cycle-41e rationale overstated what the experiment isolates — "tests EXACTLY one thing" and "ELIMINATES branch (b) and only branch (c) remains" were too strong because the entry-point swap also cedes caller control over Protect / placement / Alignment to kernel defaults. Adoption: new "Honest framing" paragraph in `xbed_self_witness.c` Rationale block + reworded "What cycle 41e does NOT directly test" paragraph + matching `xbed_self_witness.h` Safety-notes softening. Both success and failure interpretations now read "STRONG but not conclusive evidence FOR/AGAINST branch (b)" rather than formally eliminating/proving branches.
- **R1.P3 (low) — ADOPTED via softened guard-block intro.** Codex flagged that line 277 (then "guards close the interpretation gap so a count=0 observation can only mean 'allocation failed'") was internally inconsistent with the later envelope text (which correctly says count=0 also covers out-of-window and sub-page-aligned returns). Adoption: rewrote the intro to "fall in the bounded envelope 'allocation failed (NULL) OR allocator returned out-of-window phys OR allocator returned sub-page-aligned phys'" matching the closing-paragraph envelope text exactly.

### Round 2 — MAJOR ISSUES (1 new P-level finding)

- **R2.MED (sign-flip) — ADOPTED via sign-corrected wording in both `.c` and `.h`.** Codex flagged that leftover wording at `.c:340-344` said success would "AND eliminate branch (b)" and at `.h:135-142` said success is "STRONG but not conclusive evidence against branch (b)" — both sign-flipped vs the new R1.P2-adopted Honest-framing paragraph (which correctly says success → evidence FOR branch (b), failure → evidence AGAINST branch (b)). Adoption: rewrote `.c:340-344` to "STRONG-but-not-conclusive evidence FOR branch (b)"; rewrote `.h:135-142` to "STRONG but not conclusive evidence FOR branch (b) being the failing constraint"; both spots note the Codex R2 sign-flip correction in-source. R3 confirmed the sign-flip itself fixed consistently.

### Round 3 — MINOR ISSUES (3 LOW; explicit deploy-readiness GREEN)

- **R3.LOW #1 — ADOPTED via cycle-39 breadcrumb degenericization.** Codex flagged stale `MmAllocateContiguousMemoryEx` wording in the cycle-39 EEPROM-breadcrumb comment block (`.c:56-72`) + the failure-log string (`.c:111-116`) — both still hard-coded the historical `-Ex` name from cycles 29..41d. Adoption: rewrote comment to "the allocator entry point itself varies per cycle: cycles 29..41d called MmAllocateContiguousMemoryEx; cycle 41e onward calls the non-`-Ex` MmAllocateContiguousMemory" + "Position: AS THE LAST INSTRUCTION before the allocator call"; failure-log line reworded to "continuing to the allocator call".
- **R3.LOW #2 — REBUTTED with grep evidence.** Codex claimed the cited `xbed_self_witness.c:141` reference to `nxdk/lib/xboxkrnl/xboxkrnl.exe.def:172` was off (per Codex it should be `:165`). REBUTTED: `grep -n MmAllocateContiguousMemory@4 /Users/jbbrack03/XEMU_MacOS/nxdk/lib/xboxkrnl/xboxkrnl.exe.def` returns `172:    MmAllocateContiguousMemory@4             @ 165 NONAME`. The `:172` is the file LINE number; the `@ 165` is the symbol's EXPORT ORDINAL — these are distinct fields. The cited line number is correct as-written. R4 confirmed the rebuttal: "R3 LOW #2 rebuttal: correct. xboxkrnl.exe.def:172 is the file line number; @ 165 is the export ordinal."
- **R3.LOW #3 — ADOPTED via softening to "unmeasured on this hardware".** Codex flagged that the "historically PAGE_READWRITE, cacheable write-back" parenthetical (`.c:145` + `.h:116`) was not backed by a local citation and was inconsistent with the surrounding "non-`-Ex` defaults are unmeasured on this hardware" framing — violates rule #1 "do not guess". Adoption: removed the unsupported claim from both `.c` and `.h`; replaced with explicit "The exact kernel-default Protect / placement / Alignment for this console + build are NOT measured on this hardware" + reference to the Honest-framing paragraph + the cycle-41c+41d defensive guards that close consumer-visibility blind spots regardless of what the kernel returns.

R3 explicit close: "Deploy-readiness: green for the real-Xbox run from a code-path standpoint."

### Round 4 — MINOR ISSUES (1 residual LOW DEFERRED)

- **R4.LOW (residual `-Ex` naming) — DEFERRED.** Codex flagged that several other comment spots outside the R3 target block (`.c:20`, `.c:439`, `.h:30, 225, 259, 305, 316`) still describe the active cycle-41e path as `MmAllocateContiguousMemoryEx`. Codex explicit "Comment-only, not a deployment blocker." DEFERRED — these are pre-existing references to the cycle-29..41d historical name and remain technically accurate as historical context (the comment at `.c:20` for instance documents the cycle-29 derivation; updating it to "the allocator call" loses historical precision). Will be cleaned in a follow-up bounded slice if needed.

R4 explicit close: "No runtime blocker for real-Xbox deployment showed up in this diff. The MmAllocateContiguousMemory(0x1000u) swap is ABI-correct, and the preserved phys-range/alignment guards keep the intended interpretation envelope intact. Other than the deferred cross-doc drift you excluded, I did not find another hard-rule conflict beyond the residual source-comment drift above."

Hard rule conflicts: NONE in the cycle-41e source diff (the R1.P1 cross-doc drift is resolved by this closure commit's doc-sync step).

## Cycle-41e outcome — G0(c) PERSISTS under non-`-Ex` fallback

Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan shape)`:

| Signal | Observed (cycle 41e) | Cycle 41d | Cycle 41c | Cycle-40 expected for G0(c) | Match |
|---|---|---|---|---|---|
| `eeprom.scratch.read` byte at 0xFF | `0xA4` (tag=0xA, stage_nib=0x4) | `0xA4` | `0xA4` | `0xA4` | ✓ G0(c) PERSISTS |
| `witness.scan-self` count | `0` | `0` | `0` | `0` | ✓ |
| `witness.scan-self` reserved1 | n/a (count=0) | n/a | n/a | n/a | ✓ |
| `witness.scan` shape | D-cycle-27 (count=1 phys=0x03eb3000 reserved=0) | D-cycle-27 | D-cycle-27 | D-cycle-27 | ✓ |
| Dashboard FTP recovery | t+16s | t+19s | t+24s | (no specific table expectation) | within sampling band; consistent watchdog reset shape; recovery timing NOT load-bearing for G-row classification |

**G0(c) uniquely selected (same as cycles 40 + 41a + 41b + 41c + 41d).** The non-`-Ex` fallback did NOT shift the outcome to G1..G4 (which would require `witness.scan-self count >= 1`). The cycle-41e hypothesis "the `-Ex` validation logic specifically rejects the cycle-29 calling-context state and bypassing `-Ex` will succeed" is REJECTED. The broader cycle-22 branch (b) "the `-Ex` variant itself is the failing constraint" is provided STRONG-but-not-conclusive evidence AGAINST being the SOLE failing constraint (a shared upstream failure mode common to both entry points remains conceivable).

## What cycle-41e discriminates

- **STRONG evidence AGAINST** the simpler "`-Ex` validation logic specifically rejects the cycle-29 tuple" hypothesis (sub-claim of cycle-22 branch (b)). Both `-Ex` and non-`-Ex` allocator entry points reject the cycle-29 1-page request identically.
- **DOES NOT FORMALLY ELIMINATE** branch (b) on its own. The non-`-Ex` kernel-default Protect / placement / Alignment are not measured on this hardware; a shared upstream failure mode common to both entry points (e.g. allocator-pool state corruption from the `.CRT$XXC` slot's pre-CRT execution context) remains conceivable.
- **NARROWS toward** branch (c) `size=0x1000`-specific interaction. The five bounded cycle-41 variations have varied every axis the API exposes inside the WTNS layout contract (cache-policy, address-range, alignment, entry-point) and converged on G0(c) PERSISTS. The remaining unvariated axis is `size`.
- **OUT OF CYCLE-41 SCOPE:** branch (c) testing requires either redesigning the cycle-29 self-witness as multi-page (changes the WTNS layout contract) or moving the allocation to a pre-CRT context (custom XBE-header callback before `_start`).

## Hypothesis state after cycle 41e

The cycle-22 leading hypothesis is FURTHER NARROWED beyond cycle-41d's narrowing:

The failing constraint is NOT cache-policy (cycle 40 + 41a + 41b exhausted bare RW / NOCACHE / WRITECOMBINE under `-Ex`), NOT the address range alone (cycle 41c eliminated under `-Ex` via matched-tuple), NOT the page-alignment requirement (cycle 41d eliminated under `-Ex` via `Alignment=0u`), AND NOT the `-Ex` validation logic alone (cycle 41e: non-`-Ex` fails identically). The remaining live cycle-22 candidate is **branch (c) `size=0x1000`-specific interaction** — OUT OF cycle-41 scope.

Reproducibility shape continues:
- `phys=0x03eb3000` deterministic kernel-pool reuse: 25+ consecutive observations across cycles 26..41e.
- `mapped_pages_seen=419` at every readback.
- EEPROM non-volatility across the pre-chainload reboot 1 confirmed.

## What this validates / what is still in flight

- Rule #15 (Codex validation for non-trivial implementation): **SATISFIED** via Codex 4 rounds with all hard findings adopted; R3 explicit "Deploy-readiness: green"; R4 explicit "No runtime blocker for real-Xbox deployment".
- Cycle-40 EEPROM regression gate (`byte = 0xA4` after any cycle-39+ chainload): **HELD** by cycle-41e (sticky `s_eeprom_scratch_attempted` flag from cycle-39 / Codex round-2 P1 fix continues to fire correctly through the cycle-41e calling context).
- Cycle-29 consumer scan-window contract (`[0x80010000, 0x84000000]`, 4 KiB stride): **HELD** by the preserved cycle-41c symmetric phys-range guards + the preserved cycle-41d page-alignment guard.
- Cycle-23 lockstep + cycle-29 self-witness + cycle-31 paint + cycle-35 `.CRT$X*` slot + cycle-39 EEPROM-scratchpad mechanism + cycle-41a + cycle-41b comment tightening + cycle-41c symmetric phys-range guards + cycle-41d page-alignment guard: **ALL preserved**.
- Cycle-22 leading hypothesis: FURTHER NARROWED to branch (c). Cycle-41 SCOPE EXHAUSTED. Cycle 42 candidate A (multi-page redesign) tests branch (c) directly.
- M15 default-on shape: **still blocked** (cycle 42+ size-axis or calling-context-axis work + downstream cycles).
- Eight default-on Apple Silicon flags: untouched. Cycle-41e changes no host xemu source. Project rule #11 not in play.

## Closing checklist

- [x] Source diff applied: 1-literal load-bearing non-`-Ex` swap + Honest-framing comment rewrite + sign-flip-corrected wording + degenericized cycle-39 breadcrumb + softened unsupported claim + cycle-41c→cycle-41e log relabel + paired comment block + header doc updates.
- [x] Clean nxdk rebuild produced new witness-only artifact (deployed-build SHA `5c9fad12…`; source bit-identical to codex-R4-confirmed state).
- [x] Codex 4 rounds executed; verdict trajectory MAJOR → MAJOR → MINOR ("green") → MINOR ("not a deployment blocker") with all hard findings adopted; Codex marker refreshed.
- [x] Real-Xbox runbook executed end-to-end (cycle-40 shape).
- [x] G0(c) PERSISTS classification confirmed via three primary signals + full EEPROM hex dump cross-check.
- [x] Evidence directory written with SUMMARY.md + 21 step-numbered logs + 4× Codex prompts + 4× Codex outputs.
- [x] handoff.md / decision-log.md / orchestration-state quartet updated; prior cycles preserved unchanged below.
- [x] Closure commit landed on `apple-silicon-performance` as `deb2206491` (cycle-41e non-`-Ex` fallback variation closure).
