# Validation Status

- Active slice: cycle 42C real-Xbox deployment of cycle-42B pre-WinMainCRTStartup thunk — bounded run-only + doc-only closeout slice. Mechanism: deployed the Codex-R5-GREEN cycle-42B witness-only XBE (SHA `3cc670fb…`) to real Xbox hardware per the runbook documented in `benchmark-runs/cycle42b-prewinmaincrt-20260524T115521Z/SUMMARY.md` § "Real-Xbox deployment runbook (delta vs cycle 42A)"; classified the outcome per the joint (reserved0_last_stage, reserved1) table. NO source changes; NO Codex pass (rule #15 doc-only carve-out).
- Validation state: **Rule #15 SATISFIED via doc-only carve-out** (the cycle-42B build deployed here is Codex-R5-GREEN; cycle-42C diff is run-only + doc-only — no implementation changes). Rule #4 (no doc drift) SATISFIED via handoff.md + decision-log.md + orchestration-state quartet sync. Real-Xbox oracle validation EXECUTED per the cycle-42B runbook; outcome classified.

## Rule #15 applicability (cycle 42C)

Cycle 42C is a RUN-ONLY + DOC-ONLY slice (rule #15 trigger #2 "non-trivial uncommitted code in `xemu-fork/`" does NOT fire because cycle 42C ships ZERO C / Makefile / script source changes). The only files produced are:

- `benchmark-runs/cycle42c-realxbox-20260524T122656Z/{SUMMARY.md, 00..18-*.{txt,json}}` (gitignored per project convention);
- canonical-doc updates: `handoff.md` + `decision-log.md` + `orchestration-state/{current-cycle,claude-status,validation-status,handoff-summary}.md`.

The Stop-hook >30-line aggregate-diff threshold is exceeded by the doc updates, but the hook's documented prose-skip carve-out at `xemu-fork/CLAUDE.md` rule #15 ("doc-only changes" skip automatically) applies. The cycle-42B Codex 5-round review (R1 MAJOR ISSUES → R2 MED → R3 HIGH → R4 NOT GREEN → R5 GREEN "Deploy-ready") fully covers the deployed artifact at SHA `3cc670fb…`.

## Cycle-42C real-Xbox run outcome — INCONCLUSIVE per cycle-42B SUMMARY runbook row (c)

The cycle-42C bounded run-only sub-slice produced the following:

| Artifact | Value |
|---|---|
| Deployed witness-only XBE SHA-256 | `3cc670fb4df5871811d94bcc8339e5126ab0e61a78e44802ba9e655d742442a1` (cycle-42B Codex-R5-GREEN artifact, deployed bit-identically) |
| Size | 155 648 B |
| Oracle-agent SHA | (unchanged; cycle-39 v0.5 `d419b452…` resident from cycle 41e onward) |
| Reboot 1 timestamp | 2026-05-24T12:27:17Z |
| Dashboard FTP recovery (reboot 1) | t+18s |
| FTP-upload result | uploaded=1 skipped=0 failed=0 |
| Eeprom.scratch baseline (post-reset, pre-runxbe) | byte=0x00 ✓ |
| runxbe timestamp | 2026-05-24T12:28:40Z |
| Dashboard FTP recovery (post-runxbe) | t+20s |
| Final `witness.scan` | `count=1 phys=0x03eb3000 reserved0=0 reserved1=0` (D-cycle-27 shape; cycle-23 lockstep intact) |
| Final `witness.scan-self` | `count=0 mapped_pages_seen=419` |
| Final `eeprom.scratch.read` | **`byte=0xA6 tag=0xA stage_nib=0x6`** |
| Full eeprom hex dump cross-check | last byte = `a6` (line ends `…0900a6`) ✓ |
| Outcome row (per cycle-42B SUMMARY) | **(c) INCONCLUSIVE: (EEPROM=0xA6, count=0)** |

## What cycle-42C discriminates

The observed shape `(eeprom.scratch.read=0xA6, witness.scan-self count=0, EEPROM-byte-did-NOT-flip-to-0xA4)` proves at **HIGH confidence**:

1. **Entry-point override took.** The PE `AddressOfEntryPoint=0x3510` redirection to `witness_only_pre_winmain_crt_startup` ran on real hardware.
2. **`HalWriteSMBusValue(0xA8, 0xFF, FALSE, 0xA6)` does NOT fault from the strict pre-WinMain context.** Defensive pre-write landed and persisted.
3. **NONE of the five WTNS-shim fires reached the cycle-39 EEPROM write site inside the shim.** Otherwise the byte would have flipped to `0xA4` per the cycle-39 sticky-flag-on-entry contract.

**This RULES OUT hypothesis (b)** from the cycle-42B SUMMARY's (a)/(b)/(c) failure-mode taxonomy: the cycle-41c/d post-allocation guards preserve the sticky-flag setter sequence; (b) "allocator-then-post-guard rejection" would have flipped EEPROM → 0xA4 from at least one of the five fires. The fact that this did NOT happen means no fire even reached the sticky-flag setter line.

**Live cycle-42B failure modes narrow from {a, b, c} to {a, "post-cycle-39-write-site fault that prevents the sticky flag from being set on entry"}.**

The empirically-supported leading hypothesis = **(a) pre-allocator fault inside the shim's pre-libc-init context — most likely `xbed_host_log_writef → vsnprintf`** (which sits in the shim's hot path BEFORE the cycle-39 EEPROM write site at `xbed_self_witness.c`). The alternative "the very first instruction inside the shim faults before any user-code executes" is implausible because shim entry code is bog-standard C with no pre-libc dependencies until `xbed_host_log_writef`.

## What this validates / what is still in flight

- Rule #15 (Codex validation for non-trivial implementation): **SATISFIED via doc-only carve-out** — the cycle-42B build deployed here is Codex-R5-GREEN; cycle 42C ships no implementation changes.
- Rule #4 (no doc drift): handoff.md + decision-log.md + orchestration-state quartet updated with cycle-42C entries.
- Rule #5 (build tools when toolset is the limit): N/A this slice (deployment, not tool-extension).
- Cycle-40 EEPROM regression gate: **HELD** in design (the gate is on the cycle-29 self-witness shim's cycle-39 sticky-flag write; cycle-42B added a SEPARATE defensive pre-write with stage 6 encoding that landed cleanly; the shim's own write site was unreachable from this run's calling context, which is the diagnostic signal cycle-42B was designed to elicit — not a regression).
- Cycle-29 consumer scan-window contract: **HELD** by the preserved cycle-41c symmetric phys-range guards + cycle-41d page-alignment guard + cycle-42A multi-page redesign.
- Cycle-23 lockstep + cycle-29 self-witness + cycle-31 paint + cycle-35 `.CRT$X*` slots + cycle-39 EEPROM-scratchpad + cycle-41a..41e historical comment blocks + cycle-42A multi-page redesign + cycle-42B pre-WinMain thunk: **ALL preserved unchanged**.
- Cycle-22 leading hypothesis state: **NOT advanced** by cycle 42C. The calling-context axis is still live; cycle 42B's discriminator did not deliver a clean signal because the shim faults before reaching the allocator. However, **hypothesis (b) post-allocation-guard rejection is RULED OUT**, and the live failure-mode set narrows from {a, b, c} to {a, "post-cycle-39-write-site fault that prevents the sticky flag from being set on entry"} — itself dominated by (a) `xbed_host_log_writef → vsnprintf` pre-libc fault on plausibility grounds.
- M15 default-on shape: **still blocked** (cycle 42D + downstream cycles).
- Eight default-on Apple Silicon flags: untouched. Cycle-42C changes no host xemu source. Project rule #11 not in play.

## Closing checklist

- [x] Real-Xbox deployment runbook executed end-to-end (18 logged steps).
- [x] Build artifact verified at SHA `3cc670fb…` pre-deployment.
- [x] Agent reachable throughout the run; FTP recoveries within sampling band.
- [x] EEPROM baseline cleanly reset (0xA4 → 0x00) before runxbe.
- [x] Final signals captured (witness.scan + witness.scan-self + eeprom.scratch.read + full eeprom hex dump).
- [x] Outcome classified per the cycle-42B SUMMARY runbook table (row (c) INCONCLUSIVE).
- [x] (b)-elimination reasoning documented in SUMMARY.md and decision-log.
- [x] Leading hypothesis narrowed to (a) `xbed_host_log_writef → vsnprintf` pre-libc fault.
- [x] Cycle-42D scope recommendation documented (stage-6-specific shim bypass + EEPROM marker bytes at each decision point).
- [x] handoff.md / decision-log.md / orchestration-state quartet updated; prior cycles preserved unchanged below.
- [x] Closure commit lands on `apple-silicon-performance` (final action of this session, doc-only).
