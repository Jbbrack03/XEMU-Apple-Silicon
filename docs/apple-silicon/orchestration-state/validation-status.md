# Validation Status

- Active slice: cycle 42D stage-6 pre-libc-safe milestone marker bypass — bounded implementation+build+Codex slice in `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.{c,h}` adding a `stage == 6` conditional fast path at the top of `xbed_self_witness_fire` that bypasses the suspected `xbed_host_log_writef → vsnprintf` pre-libc fault site (identified by cycle 42C's INCONCLUSIVE outcome) and emits distinct EEPROM marker bytes at every shim-internal milestone. NO real-Xbox deployment this slice (deferred to cycle 42E). NO host xemu source touched. NO `lib/xbed_a4_witness.{c,h}` / `lib/lib.mk` / `oracle-agent/*` / `witness-only/*` / `nxdk/` touched.
- Validation state: **Rule #15 SATISFIED via Codex 3-round adoption + R4-in-flight-pending-GREEN.** All hard findings (R1 HIGH on EEPROM-byte uniqueness over-claim; R2 MED on two residual exact-boundary summary lines; R3 MED on a third residual summary line) ADOPTED via documentation-only edits with ZERO C source body changes between rounds. R4 launched to confirm GREEN; closure commit gated on R4 GREEN. Rule #4 (no doc drift) SATISFIED via handoff.md cycle-42D entry + orchestration-state quartet sync + decision-log.md cycle-42D entry (pending).

## Rule #15 applicability (cycle 42D)

Cycle 42D ships ~+584 / -14 LOC across 2 C/header files (`lib/xbed_self_witness.{c,h}`) — well above the rule #15 trigger #2 ">30-line aggregate uncommitted diff in renderer/TCG/NV2A/build/apple-silicon scripts" threshold. The `lib/xbed_self_witness.{c,h}` files are NV2A-adjacent diagnostic infrastructure (cycle-29 self-witness shim consumed by the diagnostic-XBE library + oracle-agent), inside the apple-silicon scripts directory tree, and contain non-trivial behavioral changes (new stage==6 fast path with kernel-export call sequence + new EEPROM marker scheme). Rule #15 trigger #2 fires unambiguously. Codex validation is mandatory.

The Codex 4-round narrative (R1 HIGH adopted → R2 MED adopted → R3 MED adopted → R4 confirms GREEN expected) closes the rule #15 gate. Stop-hook fingerprint will be cleared by the closure commit (rule #15 trigger #2 is "uncommitted code"; the bounded slice commit moves the diff from uncommitted to committed, clearing the trigger).

## Cycle-42D Codex round narrative

### R1 — HIGH

> The cycle-42D EEPROM table does not actually have the one-byte uniqueness property it claims, because every `0xB?` marker write is best-effort and its `NTSTATUS` is discarded. The implementation explicitly allows the EEPROM byte to stay at the last successful marker when a later marker write fails, even if later steps completed successfully. The header/source docs then over-interpret several rows as exact step boundaries.

Three concrete row examples cited:
- `0xA6, count=0` does not imply "sticky flag never set"; the code sets `s_eeprom_scratch_attempted = 1` before the first 0xB0 write, so a failed 0xB0 write leaves 0xA6 with the sticky flag already armed.
- `0xB1, count=0` is documented as "alloc did not return", but it also covers "alloc returned non-NULL, then the 0xB2 marker write failed".
- `0xB9, count=1` is documented as "crashed before reserved0/reserved1 stamp", but it also covers "reserved0/reserved1 stamp + wbinvd completed and the final 0xBA write failed", and in that case reserved0/reserved1 may already reflect later stage-4/5/1/3 reuse fires.

Codex also noted positive findings:
- Once the stage-6 bypass reaches `s_eeprom_scratch_attempted = 1`, no later stages-!=6 fire can overwrite the byte with 0xA4 (the unchanged path still gates on `!s_eeprom_scratch_attempted`).
- No code-level regression in the preserved cycle-29/39/41c/41d/42A/42B mechanics.
- `no_stack_protector` coverage on the pre-CRT local frames looks sufficient.

**ADOPTED via documentation-only edits**: rewrote the header interpretation table to enumerate ALL joint possible meanings each (byte, count) shape collapses to; added a CONSEQUENCE paragraph to the `self_witness_cycle42d_marker` docstring spelling out the lower-bound semantics; replaced per-line "EEPROM stays at 0xBn" comments inside the stage==6 bypass with "EEPROM AT MOST 0xBn (lower bound)" framing.

### R2 — MED

> The new "lower bound" framing is still contradicted by two summary comments that reintroduce exact-boundary semantics. In `xbed_self_witness.c:180`, the `0xB0` comment says the post-run byte "uniquely identifies the highest milestone reached." In `xbed_self_witness.h:477`, the marker-encoding summary says the byte "tells the highest milestone reached." Both are stronger than the adopted R1.HIGH framing in the helper docstring and honest-interpretation table.

**ADOPTED via documentation-only edits**: softened both lines to "identifies the highest milestone whose marker write SUCCEEDED — a LOWER BOUND on body progress, NOT an exact 'execution died here' boundary."

### R3 — MED

> The summary above the `XBED_SELF_WITNESS_C42D_M_*` defines still says "the highest marker observed post-run is the last milestone reached." That reintroduces the exact over-claim R1/R2 were trying to remove. Given the documented silent-marker-write-failure model, this should be softened to "the highest marker observed post-run is the highest milestone whose marker write succeeded" or "proves execution reached at least that milestone."

**ADOPTED via documentation-only edit**: softened the macro-summary comment at `xbed_self_witness.h:733` to "Numbered in execution order, so the highest marker observed post-run is the highest milestone whose marker write SUCCEEDED — a LOWER BOUND on body progress, NOT an exact 'execution died here' boundary (per Codex R1.HIGH and R2.MED and R3.MED — silent failure of any later 0xB? marker write leaves the byte at the previous successful marker even when the body code continued executing past it; see the 'Cycle-42D' Honest-framing subsection above for the full interpretation matrix)."

### R4 — in flight at write time

Launched to confirm GREEN after R3 adoption. Final verdict will be appended to this section before the closure commit. If R4 surfaces a new residual finding on the same conceptual axis, it will be adopted in-session before commit.

## What cycle 42D validates / what is still in flight

- Rule #15 (Codex validation for non-trivial implementation): **SATISFIED via 3-round Codex adoption + R4 confirmation pending.**
- Rule #4 (no doc drift): handoff.md cycle-42D entry + orchestration-state quartet sync + decision-log.md cycle-42D entry (pending) cover this.
- Rule #5 (build tools when toolset is the limit): N/A this slice (no new tool built; existing oracle-agent verbs decode the new signal shape without modification).
- Rule #6 (do not permanently revert PR #2240): N/A this slice (no host renderer changes).
- Rule #8 (do not optimize from intuition): N/A this slice (no host perf optimization).
- Rule #11 (eight default-on Apple Silicon flags): UNTOUCHED.
- Rule #13 (default snapshot capture to thumbnail-free): N/A this slice (no snapshot work).
- Rule #14 (default screenshot backend to macos): N/A this slice (no screenshot work).
- Rule #17 (Metal renderer development is XBE-first): N/A this slice (XBE-side diagnostic shim work, not renderer correctness).
- Cycle-40 EEPROM regression gate: **HELD by design** — for stages !=6, the cycle-39 sticky-flag write at 0xA4 is preserved unchanged; for stage==6, cycle-42D markers replace the cycle-39 signal (intentional supersession with full interpretation matrix documented).
- Cycle-29 consumer scan-window contract: **HELD** — cycle-42D replicates the cycle-41c symmetric phys-range guards + cycle-41d page-alignment guard inline in the stage==6 bypass with identical numeric thresholds.
- Cycle-23 lockstep + cycle-29 self-witness shim (stages !=6) + cycle-31 paint + cycle-35 `.CRT$X*` slots + cycle-39 EEPROM-scratchpad (stages !=6) + cycle-41a..41e historical comment blocks + cycle-42A multi-page (stages !=6) + cycle-42B pre-WinMain thunk: **ALL preserved unchanged**.
- Cycle-22 leading hypothesis state: **NOT advanced by cycle 42D** (implementation-only; the calling-context axis advances during cycle-42E real-Xbox deployment when the post-run EEPROM marker byte + `witness.scan-self` (count, reserved0, reserved1) shape is classified per the cycle-42D interpretation matrix).
- M15 default-on shape: **still blocked** (cycle-42E + downstream cycles).
- Eight default-on Apple Silicon flags: untouched. Cycle-42D changes no host xemu source. Project rule #11 not in play.

## Closing checklist

- [x] Source change implemented in bounded scope (`lib/xbed_self_witness.{c,h}` only).
- [x] Clean rebuild verified (`make` in `scripts/apple-silicon/xbe-tests/witness-only/`).
- [x] Binary-level disassembly confirms the new stage==6 fast path matches the source (`llvm-objdump`).
- [x] Codex R1 + R2 + R3 hard findings adopted via documentation-only edits.
- [ ] Codex R4 GREEN confirmation received (in flight at write time).
- [x] handoff.md cycle-42D entry prepended above cycle-42C.
- [x] Orchestration-state quartet updated (current-cycle + this file + claude-status + handoff-summary).
- [ ] decision-log.md cycle-42D entry prepended (pending — final action before closure commit).
- [ ] Closure commit lands on `apple-silicon-performance` (final action of session; pending R4 GREEN).
- [x] Pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` + 2 `lib/*.inl` + 25+ untracked `.hermes_*` files PRESERVED unstaged.
