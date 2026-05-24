# Validation Status

- Supervisor note: **Cycle 42H is now CLOSED at `934e9273ef`** (implementation+build+Codex complete; real-Xbox deployment DEFERRED to next bounded slice). This slice IS an oracle-agent source change, so **rule #15 RE-TRIGGERED and SATISFIED via a full 3-round Codex pass to GREEN**, not via the doc-only carve-out.

- Active slice: cycle 42H broaden `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c::cmd_witness_scan_self` phys-range enumeration. ONE file changed (`commands.c`; +321 / -13 LOC; ~50 LOC new C body + ~270 LOC new in-source documentation). No host xemu source touched; no producer side touched (cycle-42F R5-GREEN deployed bit-identically); no cycle-23 / cycle-39 / cycle-42B / cycle-42D mechanisms touched.

- Validation state: **Rule #15 SATISFIED via Codex 3-round R3 GREEN.** Codex marker at `.claude/state/codex-validate-last-run` refreshed to `2026-05-24T17:51:44Z` after R3 GREEN. Rule #4 (no doc drift) SATISFIED via this update + matched updates to `current-cycle.md` + `claude-status.md` + `handoff-summary.md` + canonical `handoff.md` + canonical `decision-log.md` + cycle-42H `SUMMARY.md`.

## Codex round summary

| Round | Verdict | Findings | Action |
|---|---|---|---|
| R1 | MINOR ISSUES | MED @ cap-hit kseg1 silent skip (silently conflated "no kseg1 evidence" with "kseg1 never scanned"); LOW @ K0+K1 may exceed N (arithmetic over-claim — by construction `N == K0 + K1`) | ADOPTED via two new `truncated_at_cap=` + `kseg1_scanned=` summary fields + corrected K0/K1 interpretation row + Cap-truncation observability subsection + helpline expansion |
| R2 | MINOR ISSUES | LOW @ `truncated_at_cap` only fired for kseg0 cap-overflow (field-naming/docs slightly overclaimed disambiguation when kseg1 itself reached the cap) | ADOPTED via true global truncation flag `truncated_at_cap = (kseg0_cap_hit OR kseg1_cap_hit)`; companion `kseg1_scanned=` boolean lets operator pinpoint which window; docstring rewritten with full three-shape disambiguation table; "fourth shape (truncated_at_cap=0, kseg1_scanned=0) unreachable by construction" claim added |
| R3 | LOOKS GOOD | None — all three reachable cap-shapes (no truncation / kseg0 truncation / kseg1 truncation) match documented semantics; fourth-shape unreachability claim verified by control-flow analysis | DEPLOY-READY |

## What cycle 42H PROVES (build/Codex-only sub-slice)

1. The widened scan windows are implementationally correct: cached kseg0 `[0x80000000, 0x84000000)` and uncached kseg1 `[0xA0000000, 0xA4000000)` with the same 4 KiB stride.
2. The new per-buf `alias=` field is attributed correctly by construction (helper called with `"kseg0"` then `"kseg1"`; tag emitted verbatim in each `buf.*` line).
3. The `MmGetPhysicalAddress` safety gate is preserved for both alias windows — matches the agent's pre-existing "direct pointer deref is valid in RAM alias windows" model.
4. Backward compatibility for parsers keying on the leading summary token is preserved (`count=N mapped_pages_seen=M` stays at the front of the summary line).
5. The cap-truncation booleans `truncated_at_cap=` + `kseg1_scanned=` exhaust the three reachable cap-shapes; the unreachable fourth shape is guarded by control-flow construction.
6. The cycle-23 `cmd_witness_scan` verb (XCTR scanner) and the `self_wtns_reader_candidate_ok` filter are NOT regressed — the new code is on the `cmd_witness_scan_self` path only.

## What cycle 42H does NOT prove

1. It does NOT itself run anything on real hardware — that's the next bounded slice (cycle 42I; Hermes's call).
2. It does NOT classify the cycle-42G discoverability question — it only arms the next slice's interpretation table to do so.
3. The kseg1 uncached read goes around L1+L2 caches; no expected performance regression at 4 KiB stride, but the next slice will report measured wall time if any.

## Cycle-42H interpretation table for the next bounded slice (cycle 42I)

| Outcome shape | Interpretation |
|---|---|
| `count=0 kseg0_count=0 kseg1_count=0 truncated_at_cap=0 kseg1_scanned=1` | Sub-cause (2) "page torn down before agent scan" leading; (1) low-RAM + (3) uncached-only RULED OUT |
| `count=N kseg0_count=N kseg1_count=0 (N>=1)` | Cached-alias survival; cycle-22 axis FULLY CONFIRMED on discoverability sub-axis |
| `count=N kseg0_count=0 kseg1_count=N (N>=1)` | Sub-cause (3) "page survives only at uncached kseg1 alias" CONFIRMED |
| `count=N K0>0 AND K1>0 (N == K0 + K1)` | Page mapped through BOTH aliases; cycle-22 axis FULLY CONFIRMED |
| `truncated_at_cap=1 kseg1_scanned=0` | Anomaly — kseg0 hit cap (cycle-29..42G observed at most count=1) |
| `truncated_at_cap=1 kseg1_scanned=1` | Anomaly — kseg1 hit cap mid-window |

## Rule #4 — doc-sync

- `handoff.md` "Last updated" line replaced with cycle-42H entry; prior cycle-42G entry preserved verbatim above prior cycle-42F entry.
- `decision-log.md` cycle-42H entry prepended above cycle-42G entry.
- Orchestration-state quartet updated: this file + `current-cycle.md` + `claude-status.md` + `handoff-summary.md`.
- `benchmark-runs/cycle42h-scan-self-widen-20260524T173500Z/SUMMARY.md` written.
- All evidence files in the run dir gitignored per project convention.

## Repo guardrails

- Pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` + 2 `scripts/apple-silicon/xbe-tests/lib/*.inl` PRESERVED unstaged.
- Pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged.
- No new repo-root `.hermes_*` files this cycle.
