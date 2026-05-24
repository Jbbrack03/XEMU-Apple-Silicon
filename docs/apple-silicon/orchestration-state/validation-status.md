# Validation Status

- Active slice: cycle 41a `PAGE_NOCACHE` allocation-flag variation — bounded implementation+run slice executing the cycle-40 closeout's recommended LOWEST-SCOPE cycle-41 candidate. Changes `MmAllocateContiguousMemoryEx`'s `Protect` argument in `lib/xbed_self_witness.c:138` from bare `PAGE_READWRITE` to `PAGE_READWRITE | PAGE_NOCACHE`. Codex-validates, rebuilds witness-only XBE, deploys to physical Xbox, executes cycle-40-shape runbook, recovers post-run evidence, classifies against cycle-40 G0(c) regression gate.
- Validation state: **Rule #15 SATISFIED via Codex round-1 `changes`-mode review.** Verdict=MINOR ISSUES; P1 medium + P2 low ADOPTED via comment-only updates; alias-change suggestion DEFLECTED as out-of-slice scope; WRITECOMBINE filed as cycle-41b. Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed. **OUTCOME: G0(c) PERSISTS under `PAGE_NOCACHE`** per the cycle-40 G-row discriminator table.

## Rule #15 applicability (cycle 41a)

Cycle 41a is an IMPLEMENTATION slice (rule #15 trigger #2 fires — uncommitted code in `lib/xbed_self_witness.{c,h}` is renderer-adjacent + apple-silicon-scripts scope). Even though the aggregate diff is well under the 30-LOC trivial-work threshold (1-line `Protect` change + Codex-adopted comment updates), the prompt explicitly mandated Codex validation for this slice ("Treat Codex validation as REQUIRED for this implementation slice even if the diff is small; adopt or explicitly deflect findings in the docs before closing."). Codex round-1 executed accordingly.

Codex round-1 summary (mode=`changes`):
- Verdict: **MINOR ISSUES**.
- P1 medium "comment overstates kseg0+NOCACHE alias semantics" (`xbed_self_witness.c:128-145` + `:182-183`) — ADOPTED. The in-code comment was tightened to explicitly frame cycle-41a as ALLOCATOR-ACCEPTANCE triage only; the `phys | 0x80000000` cached-mirror alias is acknowledged as unchanged. Codex's alternative suggestion to add a separate `phys | 0xB0000000` end-to-end uncached-alias variant was DEFLECTED as out-of-slice scope (cycle-41a is one bounded variation; alias-change is a separate candidate that can be funded later if the cache-policy track exhausts).
- P2 low "header doc still says RW protection matches agent" (`xbed_self_witness.h:103-107`) — ADOPTED. The header Safety-notes block at lines 101-118 was rewritten to document the cycle-41a NOCACHE divergence from the agent's plain-RW pattern + the nxdk OHCI `MmAllocateContiguousMemoryEx(..., PAGE_READWRITE | PAGE_NOCACHE)` precedent at `nxdk/lib/usb/libusbohci_xbox/usbh_xbox.c:35-41`.
- Open question "WRITECOMBINE vs NOCACHE choice" — resolved by Codex itself ("WRITECOMBINE is the more natural later experiment for NV2A-facing buffers, not an obvious requirement for this specific witness page"). Filed as cycle-41b candidate.
- Out-of-scope finding "agent's own allocator at `oracle-agent/controller.c:221-248` also uses plain `PAGE_READWRITE`" — NOTED but not actioned. The agent's buffer empirically works across many chainloads since cycle 23; not in the failing kernel-state region.
- Codex strengths-section confirmed: (i) the EEPROM breadcrumb gate is unchanged (sticky `s_eeprom_scratch_attempted` still fires before `MmAllocateContiguousMemoryEx`); (ii) producer/consumer visibility is not made worse (wbinvd + cached-mirror readback both intact); (iii) `PAGE_NOCACHE = 0x200` is a real nxdk protect bit with nxdk-side precedent; (iv) only behavioral change is the `Protect` mask.

Hard rule conflicts: NONE.

## Cycle-41a outcome — G0(c) PERSISTS under `PAGE_NOCACHE`

Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)`:

| Signal | Observed (cycle 41a) | Cycle-40 baseline | Cycle-40 expected for G0(c) | Match |
|---|---|---|---|---|
| `eeprom.scratch.read` byte at 0xFF | `0xA4` (tag=0xA, stage_nib=0x4) | `0xA4` | `0xA4` | ✓ G0(c) PERSISTS |
| `witness.scan-self` count | `0` | `0` | `0` | ✓ |
| `witness.scan-self` reserved1 | n/a (count=0) | n/a | n/a | ✓ |
| `witness.scan` shape | D-cycle-27 (count=1 phys=0x03eb3000 reserved=0) | D-cycle-27 | D-cycle-27 | ✓ |
| Dashboard FTP recovery | t+8s | t+6s | (no specific table expectation) | within sampling variance vs cycle 40; still anomalously fast vs cycle-36 t+38s; consistent with watchdog hardware reset |

**G0(c) is uniquely selected (same as cycle 40).** `PAGE_NOCACHE` cache-policy bit did NOT shift the outcome to G1..G4 (which would require `witness.scan-self count >= 1`). The cycle-41a hypothesis "kernel rejects bare-RW protect → adding `PAGE_NOCACHE` should let the allocation succeed" is REJECTED.

## What cycle-41a discriminates

- ELIMINATES candidate "cache-policy bare-RW alone fails because the kernel demands an explicit cache-policy bit". The kernel did NOT accept the call with `PAGE_NOCACHE` either.
- DOES NOT ELIMINATE: cache-policy `PAGE_WRITECOMBINE` (cycle 41b candidate); broader address range (cycle 41c); lower alignment (cycle 41d); fallback to non-`-Ex` variant `MmAllocateContiguousMemory` (cycle 41e).

## Hypothesis state after cycle 41a

- γ.0 ("execution never entered `main()` AT ALL") — UNCHANGED FULLY CORROBORATED. Cycle-41a confirms cycle-40's narrowing within γ.0 to specifically "`MmAllocateContiguousMemoryEx` returns NULL silently or crashes inside, blocking the cycle-29 first-call branch from completing."
- G0(c) sub-classification is FURTHER STRENGTHENED but not yet sub-split — at least one of {`PAGE_WRITECOMBINE` cache-policy, address-range floor/ceiling combination, page-alignment requirement, the `-Ex` variant itself} is the failing constraint.
- Cycle-22 pre-main-crash hypothesis — UNCHANGED FULLY CORROBORATED, NARROWED to G0(c) only.
- γ.1 ("`XVideoSetMode` itself faulted before returning") — UNCHANGED INVALIDATED.

## Reproducibility shape continuation

- `phys=0x03eb3000` deterministic kernel-pool reuse REPRODUCED across 3 readbacks this session (baseline + post-deployment + final) and 21+ consecutive observations across cycles 26..41a.
- `mapped_pages_seen=419` REPRODUCED at every readback this cycle.
- `witness.scan-self count=0` REPRODUCED across cycle-41a baseline + post-chainload (matching cycle 40 G0(c) + cycle 26..40 consistent pattern).
- EEPROM non-volatility across the pre-chainload reboot 1 CONFIRMED (post-reset byte=0x00 survived through to runxbe; only the chainloaded XBE's `xbed_self_witness_fire` wrote 0xA4).

## Gate status (cycle 41a)

- [x] Required docs read.
- [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34+ guardrail.
- [x] 1-line source change applied at `xbed_self_witness.c:138` + cycle-41a rationale comment.
- [x] Clean nxdk rebuild → intermediate witness-only SHA captured.
- [x] Codex round-1 `changes`-mode review; verdict=MINOR ISSUES; P1 + P2 ADOPTED via comment-only updates; alias-change DEFLECTED.
- [x] Re-built after Codex-adopted comment updates → final witness-only SHA `7dae8cf9cc08c60f699628eb284a0b1e93b7d8ac580af6851b55b3ed6bb08f78` (155 648 B).
- [x] Codex marker refreshed at `.claude/state/codex-validate-last-run`.
- [x] Real-Xbox reachability confirmed (ping=true, agent=true v0.5 resident from cycle 40, baseline scans MET).
- [x] Reboot 1 → dashboard FTP back at t+6s.
- [x] FTP-uploaded cycle-41a witness-only XBE with `--overwrite`.
- [x] `ensure-agent` re-launched v0.5; armed EEPROM scratchpad baseline (`unsafe.enable` + `eeprom.scratch.reset` → byte=0x00 confirmed).
- [x] Composite capture SKIPPED with documented rationale (cycle-34+36 silent-stall; cycle-41a signals fully agent-side).
- [x] Chainloaded cycle-41a witness-only via `runxbe path=E:\Apps\witness-only\default.xbe` at 041123Z.
- [x] Dashboard FTP recovery polled — back at t+8s; auth OK.
- [x] Final scans + EEPROM read recovered: byte=`0xA4`, count=0, scan D-cycle-27.
- [x] Cross-checked via full `eeprom` hex dump — last byte at offset 0xFF = `A4`.
- [x] Classified as G0(c) PERSISTS per cycle-40 G-row table.
- [x] SUMMARY.md + 18 step-numbered evidence logs + codex artifacts under `benchmark-runs/cycle41a-pagenocache-20260524T040940Z/`.
- [x] handoff.md + decision-log.md cycle-41a entries on top above cycle-40.
- [x] Orchestration-state quartet (this file + current-cycle.md + claude-status.md + handoff-summary.md) closure pass.
- [ ] Closure commit on `apple-silicon-performance` (next step).

## Why this is not a regression of any prior cycle's validation guarantees

Cycle 23 / 25 / 27 / 29 / 31 / 33 / 35 / 39 each Codex-validated their own implementation slices. Cycle 41a touches the cycle-29 first-call branch with a single 1-line `Protect`-argument variation + Codex-adopted comment updates. The cycle-23 lockstep contract, the cycle-29 self-witness shim allocation tuple (except for the protect bit), the cycle-31 paint sequence, the cycle-35 `.CRT$X*` slot mechanism, the cycle-39 EEPROM-write breadcrumb + sticky-flag gate are ALL preserved. Codex round-1 explicitly confirmed "the only behavioral change in the first-call branch is the `Protect` mask; size, phys range, alignment, persist step, and failure handling are otherwise intact" and "the EEPROM breadcrumb gate is unchanged: the sticky `s_eeprom_scratch_attempted` still fires before `MmAllocateContiguousMemoryEx`, so `0xA4` remains the right cycle-40 regression discriminator under this diff." M15 unchanged; no flag-default change; no shipping behavior change.

## Out of scope for this cycle (validation perspective)

- No host xemu source edits.
- No additional allocation variations beyond `PAGE_NOCACHE` (cycle-41b WRITECOMBINE deferred to its own slice).
- No `phys | 0xB0000000` end-to-end uncached-alias experiment (Codex P1 alternative; DEFLECTED).
- No agent-allocator changes (Codex out-of-scope finding noted; not actioned).
- No flag-default change.
- No retail-title / §G.5 / RT-as-texture work.
- No PushNotification (run-only outcome on a single variation that did not flip the gate).
- No cleanup of pre-existing untracked `.hermes_*` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34+ guardrail).
- No re-validation of the eight default-on Apple Silicon flags (rule #11; not applicable to this slice).

## Next-cycle Codex applicability projection (cycle 41b — Hermes's call)

Per cycle-41a closure's next-step recommendation, cycle 41b applies the symmetric `PAGE_READWRITE | PAGE_WRITECOMBINE` variation at `lib/xbed_self_witness.c:138`. A single variation is again a ~1-LOC source change (with adapted comment) → ≤30-LOC aggregate uncommitted diff → MAY qualify for the rule #15 trivial-work carve-out. However, given that cycle 41a was Codex-validated by explicit prompt requirement, the same pattern is recommended for cycle 41b for consistency + to keep the validation chain unbroken across the cycle-41 variation series. The cycle-39 sticky-flag gate, the cycle-29 first-call branch's other invariants, and the cycle-40 EEPROM regression gate MUST all be preserved across cycle-41b.
