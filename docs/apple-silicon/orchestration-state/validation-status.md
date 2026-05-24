# Validation Status

- Active slice: cycle 40 real-Xbox EEPROM scratchpad discriminator run — bounded run-only slice executing the cycle-39 11-step runbook on the physical Xbox. Deploys cycle-39 oracle-agent v0.5 + witness-only XBE; arms EEPROM scratchpad baseline; chainloads witness-only; recovers post-run `eeprom.scratch.read` + `witness.scan-self` + `witness.scan` evidence; classifies against cycle-40 G-row table.
- Validation state: **Rule #15 SKIPPED — run-only / doc-only carve-out applies.** ZERO source / script / nxdk / host xemu / lib edits this cycle. The deployed XBE binaries (witness-only SHA `7528bb5bf4c9cc8f00c934c38e015166de7a6f89236ddc8e2ad2195a47ce4e0d`, oracle-agent SHA `d419b452f127e5a065a51b2d5bba49b6f2ec1f1826cdec57845793c34440cb53`) are exactly the cycle-39 builds that passed Codex 3-round review at closure commit `33fb5b7e34`. The cycle-39 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed artifacts. **OUTCOME: G0(c)** per the cycle-40 G-row discriminator table.

## Rule #15 applicability (cycle 40)

Cycle 40 is RUN-ONLY / DOC-ONLY (rule #15 trigger #2 does NOT fire — ZERO uncommitted code diff in xemu-fork/ scope this cycle; only doc/state updates to `docs/apple-silicon/handoff.md` + `decision-log.md` + orchestration-state quartet).

- ZERO LOC of new source code, script, or build-system change in `xemu-fork/` apple-silicon scripts.
- ZERO XBE rebuilds.
- ZERO host xemu source touched.
- ZERO `nxdk/` source touched.
- Doc-only updates to canonical project docs (handoff.md + decision-log.md + orchestration-state quartet) explicitly within the rule #15 trivial-work carve-out (`Trivial work skips automatically (≤30-line uncommitted diff, doc-only changes, single-line fixes).`).
- Compact evidence directory `benchmark-runs/cycle40-real-xbox-eeprom-discriminator-20260524T021727Z/` is gitignored per project convention (matches the `benchmark-runs/cycleNN-*/` pattern used across cycles 36 / 37 / 38).

Codex validation NOT REQUIRED per rule #15 trivial-work / run-only / doc-only carve-out. The deployed binaries are unchanged from cycle 39 closure.

## Cycle-40 outcome — G0(c)

Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)`:

| Signal | Observed | Expected for G0(c) |
|---|---|---|
| `eeprom.scratch.read` byte at 0xFF | `0xA4` (tag=0xA, stage_nib=0x4) | `0xA4` ✓ |
| `witness.scan-self` count | `0` | `0` ✓ |
| `witness.scan-self` reserved1 | n/a (count=0) | n/a ✓ |
| `witness.scan` shape | D-cycle-27 (count=1 phys=0x03eb3000 reserved=0) | D-cycle-27 ✓ |
| Dashboard FTP recovery | t+6s | (no specific expectation in the cycle-40 G-row table; anomalously fast vs cycle-36 t+38s; consistent with kernel-detected allocation crash → watchdog hardware reset) |

**G0(c) is uniquely selected.** Sub-cases G0(a) "pre-`.CRT$X*` startup crash" and G0(b) "helper body crash before pre-MmAlloc instruction" are ELIMINATED by the EEPROM byte landing at `0xA4` (both (a) and (b) would have left byte = `0x00`, the cycle-39 reset baseline). G1..G4 are excluded by count=0 (all G1..G4 rows require count >= 1). Indeterminate is excluded by the explicit `0xA4` (cycle-39 valid breadcrumb) classification of the 4-branch decoder.

## Cycle-40 readback table (reference, from cycle-39 closure)

| EEPROM 0xFF | scan-self count | scan-self reserved1 | scan shape | G-row | Interpretation | Next |
|---|---|---|---|---|---|---|
| `0x00` | 0 | n/a | `D-cycle-27` | G0(a)+(b) | EEPROM write never executed. Cannot distinguish (a) pre-`.CRT$X*` startup crash from (b) helper-body crash before reaching the EEPROM-write instruction. | Cycle 41: custom XBE-header callback (high scope). |
| **`0xA4`** | **0** | **n/a** | **`D-cycle-27`** | **G0(c) ← THIS CYCLE** | EEPROM write landed → `MmAllocateContiguousMemoryEx` returned NULL silently OR crashed. Sub-cases (a)+(b) ELIMINATED. | Cycle 41: allocation-flag variations. |
| `0xA4` | ≥1 | 1..4 | `D-cycle-27` or `A1/A2` | G1..G4 | Reverts to cycle-35 G1..G4 interpretations on the WTNS path. | Apply cycle-35 G-row table for the surviving shape. |
| any other | n/a | n/a | n/a | indeterminate | Re-run with explicit `eeprom.scratch.reset`. | If reproducible, investigate concurrent EEPROM access. |

## Hypothesis state after cycle 40

- γ.0 ("execution never entered `main()` AT ALL") — UNCHANGED FULLY CORROBORATED. Cycle 40 narrows the underlying mechanism within γ.0 from "pre-main crash, anywhere" to specifically "`MmAllocateContiguousMemoryEx` returns NULL silently or crashes inside, blocking the cycle-29 first-call branch from completing."
- Three live G0 sub-cases entering cycle 40 → ONE live G0 sub-case after cycle 40:
  - (a) crash inside nxdk's pre-`.CRT$X*` startup — **ELIMINATED by cycle 40** (EEPROM write executed after `_PDCLIB_xbox_run_pre_initializers` walked `.CRT$XXA..XXZ` and invoked `_witness_only_pre_main_crt_xx`).
  - (b) crash inside `_witness_only_pre_main_crt_xx`'s body BEFORE `xbed_self_witness_fire` reaches its pre-MmAlloc instruction — **ELIMINATED by cycle 40** (EEPROM write is positioned AS THE LAST INSTRUCTION before `MmAllocateContiguousMemoryEx`; its execution proves `_witness_only_pre_main_crt_xx` → `xbed_self_witness_fire` → first-call branch all completed up to the EEPROM-write instruction).
  - (c) `MmAllocateContiguousMemoryEx` returns NULL silently (or crashes) — **CONFIRMED by cycle 40**.
- Cycle-22 pre-main-crash hypothesis — UNCHANGED FULLY CORROBORATED, NARROWED to (c) only.
- γ.1 ("`XVideoSetMode` itself faulted before returning") — UNCHANGED INVALIDATED.

## Reproducibility shape continuation

- `phys=0x03eb3000` deterministic kernel-pool reuse REPRODUCED across 4 readbacks this session (baseline + post-deploy + post-witness-upload + post-chainload) and 20+ consecutive observations across cycles 26..40.
- `mapped_pages_seen=419` REPRODUCED at every readback this cycle.
- `witness.scan-self count=0` REPRODUCED across cycle-40 baseline + post-chainload (matching cycle 36 G0 evidence + cycle 26..36 consistent pattern).
- EEPROM non-volatility across two soft reboots within this session CONFIRMED (baseline byte `0x00` survived reboot 2; post-chainload byte `0xA4` survived chainload-induced reset).

## Gate status (cycle 40)

- [x] Required docs read.
- [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34+ guardrail (carried forward through cycles 35 / 36 / 37 / 38 / 39 / 40).
- [x] Real-Xbox reachability confirmed (ping=true, agent=true v0.4 resident, baseline scans MET).
- [x] Cycle-39 oracle-agent v0.5 deployed via FTP `--overwrite` + ensure-agent (banner cosmetic-only "v0.4"; new verbs `eeprom.scratch.*` confirmed working).
- [x] EEPROM scratchpad baseline armed: `unsafe.enable` + `eeprom.scratch.reset` → byte 0x00 confirmed.
- [x] Cycle-39 witness-only XBE deployed via FTP `--overwrite` after 2nd reboot.
- [x] Preconditions re-verified post-deploy: EEPROM 0xFF still 0x00; witness.scan D-cycle-27; witness.scan-self count=0.
- [x] Composite capture SKIPPED with documented rationale (cycle-34+36 reproduced ffmpeg silent-stall; primary signal agent-side).
- [x] Chainloaded witness-only via `runxbe path=E:\Apps\witness-only\default.xbe`.
- [x] Dashboard FTP recovery polled — back at t+6s; auth OK.
- [x] Final scans + EEPROM read recovered: byte=`0xA4`, count=0, scan D-cycle-27.
- [x] Cross-checked via full `eeprom` hex dump — last byte at offset 0xFF = `A4`.
- [x] Classified as G0(c) per cycle-40 G-row table.
- [x] SUMMARY.md + 14 step-numbered evidence logs written under `benchmark-runs/cycle40-real-xbox-eeprom-discriminator-20260524T021727Z/`.
- [x] handoff.md + decision-log.md cycle-40 entries on top above cycle-39.
- [x] Orchestration-state quartet (this file + current-cycle.md + claude-status.md + handoff-summary.md) closure pass.
- [x] Codex SKIPPED — run-only / doc-only carve-out; cycle-39 marker remains valid for deployed binaries.
- [ ] Closure commit on `apple-silicon-performance` (next step).

## Why this is not a regression of any prior cycle's validation guarantees

Cycle 23 / 25 / 27 / 29 / 31 / 33 / 35 / 39 each Codex-validated their own implementation slices. Cycle 40 touches ZERO code or scripts — the deployed binaries are exactly the cycle-39 Codex-validated builds. The only changes in cycle 40 are doc/state updates explicitly within the rule #15 trivial-work carve-out. M15 unchanged; no flag-default change; no shipping behavior change.

## Out of scope for this cycle (validation perspective)

- No source code, script, nxdk, or host xemu edits.
- No XBE rebuilds.
- No flag-default change.
- No retail-title / §G.5 / RT-as-texture work.
- No PushNotification (run-only slice; cycle 41 may warrant one if a successful allocation-flag variation lands).
- No cleanup of pre-existing untracked `.hermes_*` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34+ guardrail).
- No re-validation of the eight default-on Apple Silicon flags (rule #11; not applicable to this slice anyway).

## Next-cycle Codex applicability projection (cycle 41 — Hermes's call)

Per cycle-39 closure's binding contingent path for G0(c), cycle 41 explores `MmAllocateContiguousMemoryEx` allocation-flag variations in `lib/xbed_self_witness.c:133-138`. A single variation is a ~5-LOC source change → ≤30 LOC aggregate uncommitted diff → MAY qualify for the rule #15 trivial-work carve-out (single-line / small-fix territory). If cycle 41 tries multiple variations in one pass, the aggregate diff likely exceeds 30 LOC → Codex validation REQUIRED. The cycle-39 Codex-validated sticky-flag gate must be preserved across any cycle-41 changes (it is what guarantees the cycle-40 G0(c) evidence is reproducible in subsequent cycles).
