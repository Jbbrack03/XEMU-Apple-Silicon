# Claude Status

- Objective: cycle 41a `PAGE_NOCACHE` allocation-flag variation — bounded implementation+run slice executing the cycle-40 closeout's recommended LOWEST-SCOPE cycle-41 candidate. Change `MmAllocateContiguousMemoryEx`'s `Protect` argument in `lib/xbed_self_witness.c:138` from bare `PAGE_READWRITE` to `PAGE_READWRITE | PAGE_NOCACHE`; rebuild witness-only XBE; Codex-validate; deploy to physical Xbox; execute the cycle-40-shape runbook; recover post-run EEPROM scratchpad byte + `witness.scan-self` + `witness.scan` evidence; classify against the cycle-40 G0(c) regression gate; file the bounded cycle-41b next-step recommendation.
- Status: **CLOSED. OUTCOME = G0(c) PERSISTS under `PAGE_NOCACHE`.** Post-chainload EEPROM byte at offset 0xFF = `0xA4` (tag=0xA, stage_nib=0x4) + `witness.scan-self count=0` + `witness.scan = D-cycle-27` shape — all unchanged from cycle 40. `MmAllocateContiguousMemoryEx(0x1000, 0x10000, 0x3ffffff, 0x1000, PAGE_READWRITE | PAGE_NOCACHE)` STILL did NOT yield a usable allocation on this real-Xbox kernel. Dashboard FTP recovery at t+8s post-chainload (cycle-40 t+6s; +2s within sampling variance; still anomalously fast vs cycle-36 t+38s clean recovery; consistent with watchdog hardware reset). Cycle-41a ELIMINATES `PAGE_NOCACHE` as a working cache-policy fix. The cycle-22 leading hypothesis is FURTHER STRENGTHENED but not yet narrowed beyond cycle-40's narrowing: at least one of {`PAGE_WRITECOMBINE` cache-policy, address-range floor/ceiling combination, page-alignment requirement, the `-Ex` variant itself} is the failing constraint. handoff.md + decision-log.md cycle-41a entries on top with cycle-40 + 39 + 38 + 37 + 36 + 35 preserved unchanged below.

## Why cycle 41a ran this session

Cycle 40 closure (commit `df92fb3306`) pre-recorded the cycle-41 candidate list ranked low-scope-first: (1) cache-policy `PAGE_NOCACHE` then `PAGE_WRITECOMBINE`; (2) broaden address range; (3) drop alignment; (4) fall back to non-`-Ex` variant. Cycle 41a takes the lowest-scope first candidate. Precedent for `PAGE_READWRITE | PAGE_NOCACHE` against `MmAllocateContiguousMemoryEx` exists in nxdk itself at `nxdk/lib/usb/libusbohci_xbox/usbh_xbox.c:35-41` for the OHCI DMA-coherent ring buffers (known-good nxdk-side site that uses the `-Ex` variant). The slice is bounded to one 1-line source change + rebuild + Codex round-1 + redeploy + run — each variation is independently bounded; cycle-41b (WRITECOMBINE) and subsequent variations are deferred to their own slices.

## What this session shipped

1. **1-line source change** at `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c:138` flipping the `Protect` argument from bare `PAGE_READWRITE` to `PAGE_READWRITE | PAGE_NOCACHE` (0x04 → 0x04 | 0x200). Cycle-29 first-call branch / EEPROM-write breadcrumb / sticky `s_eeprom_scratch_attempted` gate / `MmPersistContiguousMemory` / `wbinvd` stamp / `phys | 0x80000000` cached-mirror alias for the readback path: ALL UNCHANGED.
2. **Codex-adopted comment updates** — `xbed_self_witness.c:127-145` comment tightened to explicitly frame cycle-41a as ALLOCATOR-ACCEPTANCE triage only (Codex P1 medium finding); `xbed_self_witness.h:101-118` Safety-notes block rewritten to document the cycle-41a NOCACHE divergence from the agent's plain-RW pattern + the nxdk OHCI precedent (Codex P2 low finding).
3. **Codex round-1 validation** — mode=`changes`; verdict=MINOR ISSUES. P1 medium + P2 low both ADOPTED via comment-only updates (no semantic code change). Alias-change suggestion (`phys | 0xB0000000`) DEFLECTED as out-of-slice scope. Open question about WRITECOMBINE resolved by Codex itself ("more natural later experiment, not an obvious requirement for this specific witness page") — filed as cycle-41b candidate. Out-of-scope finding about the agent's own `PAGE_READWRITE` allocator at `oracle-agent/controller.c:221-248` NOTED but not actioned (the agent's buffer empirically works across many chainloads since cycle 23; not in the failing kernel-state region). Codex marker at `.claude/state/codex-validate-last-run` refreshed for rule #15 compliance.
4. **Clean nxdk rebuild** — `eval $(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s) && make NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk` → new witness-only artifact SHA = `7dae8cf9cc08c60f699628eb284a0b1e93b7d8ac580af6851b55b3ed6bb08f78` (155 648 B; same size as cycle-39/cycle-40 builds — delta is the 1-line code change + Codex-adopted comment updates only). Oracle-agent unchanged (cycle-39 v0.5 SHA `d419b452…`).
5. **Real-Xbox cycle-41a outcome G0(c) PERSISTS** collected and classified. Three primary signals all observed and consistent with the G0(c) row of the cycle-40 G-row table: (i) `eeprom.scratch.read` = `off=0xFF byte=0xA4 tag=0xA stage_nib=0x4` cross-confirmed by full `eeprom` hex dump (last byte = `A4`); (ii) `witness.scan-self` = `count=0 mapped_pages_seen=419`; (iii) `witness.scan` = `count=1 phys=0x03eb3000 reserved0=0 reserved1=0` (D-cycle-27 shape). Dashboard FTP recovery time t+8s (anomalously fast vs cycle-36 t+38s clean recovery).
6. **Cycle-22 leading hypothesis FURTHER STRENGTHENED.** Cycle 41a confirms the kernel rejects the cycle-29 allocation tuple even with the `PAGE_NOCACHE` cache-policy bit added — eliminating "bare-RW alone fails" as the candidate failure mode. At least one of {WRITECOMBINE cache-policy, address-range, alignment, `-Ex` vs non-`-Ex`} is the actual failing constraint.
7. **Cycle-41b next step filed** in cycle-41a SUMMARY ranked-low-scope-first list: `PAGE_WRITECOMBINE` cache-policy candidate (symmetric one-line change). If WRITECOMBINE also fails identically, cache-policy variations are exhausted → cycle 41c broadens to address-range / alignment / non-`-Ex` fallback.
8. **Evidence directory** at `benchmark-runs/cycle41a-pagenocache-20260524T040940Z/` with SUMMARY.md + 18 step-numbered evidence logs + codex-prompt.md + codex-output.md + codex-login-status.txt (gitignored per project convention).
9. **Canonical docs/state synced.** handoff.md + decision-log.md cycle-41a entries on top with cycle-40 + 39 + 38 + 37 + 36 + 35 preserved unchanged below; orchestration-state quartet (this file + current-cycle.md + validation-status.md + handoff-summary.md) updated to cycle-41a closure.

## Session progress

- [x] Read required docs/state (handoff.md cycle-40 top + cycle-39 context + cycle-41 recommendation, decision-log.md cycle-40 top entry, orchestration-state quartet, `witness-only/README.md` cycle-40 G-row table + cycle-40 deployment runbook, `witness-only/manifest.json` cycle-40 expected_results).
- [x] Inspected `git status --short` + recent commits — HEAD at cycle-40 closure `df92fb3306`; pre-existing tracked drift + 19+ untracked `.hermes_*` files + `composite_preflight.py` preserved un-staged per cycle-34+ guardrail.
- [x] Verified `PAGE_NOCACHE = 0x200` definition in `nxdk/lib/xboxkrnl/xboxkrnl.h:3303` + nxdk OHCI precedent at `nxdk/lib/usb/libusbohci_xbox/usbh_xbox.c:35-41`.
- [x] Applied 1-line `Protect` argument change + cycle-41a rationale comment at `xbed_self_witness.c:127-145`.
- [x] Clean nxdk rebuild → intermediate witness-only SHA captured.
- [x] Codex round-1 `changes`-mode review; verdict=MINOR ISSUES; P1 + P2 ADOPTED via comment-only updates; alias-change DEFLECTED.
- [x] Re-built after Codex-adopted comment updates → final witness-only SHA `7dae8cf9…`.
- [x] Codex marker refreshed at `.claude/state/codex-validate-last-run`.
- [x] Probed reachability: Xbox ping=true, agent=true (v0.5 resident from cycle 40); baseline scans MET.
- [x] Reboot 1 → dashboard FTP back at t+6s.
- [x] FTP-uploaded cycle-41a witness-only XBE via `xbox-ftp-upload.py --overwrite`.
- [x] `ensure-agent` re-launched v0.5; armed EEPROM scratchpad baseline (`unsafe.enable` + `eeprom.scratch.reset` → byte=0x00 confirmed).
- [x] Composite capture SKIPPED — cycle-34+36 silent-stall; cycle-41a signals fully agent-side.
- [x] Chainloaded cycle-41a witness-only via `runxbe path=E:\Apps\witness-only\default.xbe` at 041123Z.
- [x] Polled dashboard FTP recovery — back at t+8s (anomalously fast, consistent with watchdog reset).
- [x] Recovered final evidence: EEPROM byte=`0xA4`, `witness.scan-self count=0`, `witness.scan` D-cycle-27 — classified as G0(c) PERSISTS.
- [x] Full `eeprom` hex dump cross-check confirmed last byte = `A4`.
- [x] Wrote compact SUMMARY.md + codex-prompt.md + codex-output.md to `benchmark-runs/cycle41a-pagenocache-20260524T040940Z/`.
- [x] Updated handoff.md cycle-41a entry on top above cycle-40.
- [x] Updated decision-log.md cycle-41a entry above cycle-40.
- [x] Updated orchestration-state quartet (this file + current-cycle.md + validation-status.md + handoff-summary.md).
- [ ] Commit slice changes on `apple-silicon-performance` (next step in this session).

## Confidence + risk notes

- **HIGH confidence in G0(c) PERSISTS classification.** All three primary signals (EEPROM byte = `0xA4`, witness.scan-self count = 0, witness.scan = D-cycle-27) point to the exact G0(c) row of the cycle-40 G-row table. The cycle-39 sticky `s_eeprom_scratch_attempted` flag (Codex round-2 P1 fix) guarantees the EEPROM byte was preserved across any later fires. The reading is unambiguously "stage-4 .CRT$XXC fire reached the EEPROM-write instruction at least once, but allocation did not succeed". Cycle-41a's `PAGE_NOCACHE` variation produced the same shape as cycle-40's bare-`PAGE_READWRITE`.
- **HIGH confidence in reproducibility shape continuation.** `phys=0x03eb3000` deterministic kernel-pool reuse + `mapped_pages_seen=419` REPRODUCED across baseline + post-deployment + final readbacks (21+ consecutive observations across cycles 26..41a). EEPROM non-volatility across the pre-chainload reboot confirmed (post-reset byte=0x00 survived to runxbe; only the chainloaded XBE's `xbed_self_witness_fire` wrote 0xA4).
- **HIGH confidence in Codex finding adoption decisions.** P1 medium + P2 low were comment-only and trivially correct (the cycle-41a comment did over-claim, and the cycle-29 header comment was stale). The alias-change suggestion was correctly DEFLECTED — cycle-41a was scoped to a single bounded variation; the alias-change is a separate experiment that can be funded later if needed. The WRITECOMBINE follow-up is filed as cycle-41b.
- **MEDIUM confidence in cycle-41b WRITECOMBINE outcome.** WRITECOMBINE differs from NOCACHE in cache-line write-coalescing semantics (CPU may buffer writes into bursts) but otherwise both bypass the L1/L2 cache. Whether the cycle-29 allocation tuple's failure on this kernel is sensitive to that specific distinction is unknown — cycle-41b will answer.
- **LOW risk to cycle-23 / 27 / 29 / 31 / 33 / 35 / 37 / 38 / 39 / 40 prior guarantees.** Cycle-29 first-call branch invariants (EEPROM-write breadcrumb, sticky flag gate, allocation tuple semantics except for the protect bit, persist + stamp + wbinvd flush, cached-mirror alias for readback) ALL preserved. Codex round-1 explicitly confirmed the diff did not weaken the cycle-40 EEPROM regression gate.
- **NOTE on cycle-41a deviations from canonical runbook.** ONE minor deviation: composite-record.sh SKIPPED with the cycle-34 + cycle-36 silent-stall rationale; cycle-41a primary + secondary signals are fully agent-side. Otherwise identical to the cycle-40 11-step runbook.

## What this session does NOT do

- NO host xemu source edits.
- NO `lib/xbed_a4_witness.{c,h}` edits (cycle-23 lockstep contract intact).
- NO `oracle-agent/*` edits (cycle-39 v0.5 verbs intact).
- NO `xbed_runtime.{c,h}` edits.
- NO `witness-only/main.c` / Makefile / manifest.json edits.
- NO `lib/lib.mk` edits.
- NO `nxdk/` source edits (read-only inspection only of PAGE_NOCACHE definition + OHCI precedent).
- NO `tools/xemu-capture/` source touched.
- NO `composite-record.sh` / `composite-preflight.sh` source touched.
- NO net change to any flag default; M15 unchanged.
- NO PushNotification (run-only slice; cycle 41a is a single bounded variation that did not flip the gate).
- NO cleanup of pre-existing untracked `.hermes_*` / `composite_preflight.py` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34+ guardrail).
- NO scope-expansion to additional allocation variations in the same session — cycle-41b (WRITECOMBINE) and onward are deferred to their own bounded slices.
- NO `phys | 0xB0000000` end-to-end uncached-alias experiment (Codex P1 alternative; DEFLECTED as out-of-slice scope).

## Next proposed action

Cycle 41a closes with the `PAGE_NOCACHE` cache-policy candidate ELIMINATED. The substantive next slice is cycle 41b (Hermes's call): apply the symmetric `PAGE_READWRITE | PAGE_WRITECOMBINE` variation in `lib/xbed_self_witness.c:138`, rebuild + redeploy + run via the cycle-40-shape runbook, classify against the cycle-40 G0(c) regression gate. Each variation remains independently bounded (~1-LOC source change + 1 rebuild + 1 redeploy + 1 run). If WRITECOMBINE also fails identically, cycle 41c broadens to address-range variations.
