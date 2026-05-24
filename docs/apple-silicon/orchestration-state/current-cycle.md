# Current Cycle

- Cycle: 39 EEPROM scratchpad pre-`MmAllocateContiguousMemoryEx` discriminator — **CLOSED on `apple-silicon-performance`**. Bounded implementation slice executing the cycle-38 closeout's recommended Option C: add a single-byte EEPROM scratchpad write at offset `0xFF` inside `xbed_self_witness_fire` AS THE LAST INSTRUCTION before `MmAllocateContiguousMemoryEx` (gated AT MOST ONCE per process via a NEW sticky `s_eeprom_scratch_attempted` flag — Codex round-2 P1 finding adopted). Paired with two new oracle-agent verbs `eeprom.scratch.read` + `eeprom.scratch.reset` (reset gated by existing `unsafe.enable`; reader uses a 4-branch decode that explicitly classifies non-`0xA4` `0xA?` values as "indeterminate" rather than aliasing onto sub-case (c) — Codex round-1 P2 finding adopted). Cycle-40 real-Xbox deployment is NOT in this slice.
- Started: 2026-05-23 (Hermes-supervised bounded session; Claude Code worker run launched after cycle-38 closure commit `1127cafa0d`).
- Closed: 2026-05-23 (implementation + 2 builds + Codex 3 rounds + canonical-doc sync + orchestration-state quartet refresh; closing commit pending as last action this session).
- State: **CLOSED — implementation + Codex-validated**. Both rounds of Codex source-side findings (R1 P2 reader 4-branch decode; R2 P1 sticky-flag gate) adopted; R3 returned no new cycle-39 issues (the round-3 P1 was about pre-existing tracked drift in `retail-*.py` importing the preserved-untracked `composite_preflight.py` — explicitly out of cycle-39 scope per the rolling cycle-34+ Hermes-supervision guardrail; deflected with documented reason). Codex marker written to `.claude/state/codex-validate-last-run` at the round-3 fingerprint. Cycle-29 shim allocation/stamp/wbinvd path, cycle-23 lockstep contract, cycle-31 paint sequence, cycle-35 `.CRT$X*` slot mechanism all preserved.
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: cycle-38 closure commit `1127cafa0d` on `apple-silicon-performance`.
- Bounded goal: "implement the cycle-38 recommended next step — add an EEPROM scratchpad discriminator that writes a durable breadcrumb inside `xbed_self_witness_fire` BEFORE `MmAllocateContiguousMemoryEx`, and add the paired oracle-agent read/write surface needed to recover that breadcrumb on real Xbox; lowest-cost highest-information discriminator for the remaining G0 sub-cases by letting a future real-Xbox run distinguish (c) `MmAllocateContiguousMemoryEx` returned NULL silently from (a) pre-`.CRT$X*` startup crash and (b) crash inside `_witness_only_pre_main_crt_xx` before the allocation call. Stay inside the workspace; do not modify host xemu source. Only touch the minimum witness/agent/docs/build surfaces. Preserve all pre-existing tracked drift and untracked `.hermes_*` / `composite_preflight.py` files. Keep the cycle-23 lockstep witness contract intact unless the canonical docs prove a necessary reason to change it. Prefer a bounded implementation + local validation slice; do not perform the real-Xbox deployment unless the canonical docs already make it clearly in-scope. Codex validation REQUIRED before closeout. One bounded assignment only."
- Result: **bounded implementation slice landed**. (i) `lib/xbed_self_witness.h` — 3 new `#define`s + ~85-line cycle-39 head-comment addendum. (ii) `lib/xbed_self_witness.c` — 1 new sticky-flag static (`s_eeprom_scratch_attempted`) + ~50-LOC code block inside existing first-call branch before `MmAllocateContiguousMemoryEx`. (iii) `oracle-agent/commands.c` — 2 new command implementations + 2 new help lines + 2 new `#define`s. (iv) `oracle-agent/commands.h` — 2 new declarations. (v) `oracle-agent/main.c` — 2 new dispatch-table entries + banner-comment addendum. (vi) `witness-only/README.md` — cycle-39 addendum (scratchpad-contract table + cycle-40 G-row discriminator table + cycle-40 deployment runbook + design-choice rationale). (vii) `witness-only/manifest.json` — title extended + `real-xbox/physical/cycle-40` expected_results section. (viii) Both XBEs rebuilt cleanly (zero new warnings) — `witness-only/bin/default.xbe` SHA-256 `7528bb5bf4c9cc8f00c934c38e015166de7a6f89236ddc8e2ad2195a47ce4e0d` (155 648 B unchanged); `oracle-agent/bin/default.xbe` SHA-256 `d419b452f127e5a065a51b2d5bba49b6f2ec1f1826cdec57845793c34440cb53` (417 792 B unchanged). (ix) `witness-only/main.c` UNCHANGED — the EEPROM-write code is picked up automatically through the cycle-35 `.CRT$X*` slot path. (x) Codex 3 rounds: R1 P2 + R2 P1 adopted; R3 P1 deflected (out-of-scope, pre-existing tracked drift). (xi) `.claude/state/codex-validate-last-run` marker written.

## Plan summary (this session, executed in order)

1. Read canonical docs/state (CLAUDE.md, handoff.md cycle-38+37+36+35, decision-log.md cycle-38, orchestration-state quartet, `lib/xbed_self_witness.{c,h}`, witness-only/main.c, oracle-agent/{main,commands}.{c,h}, witness-only/README.md cycle-35 addendum + manifest.json, nxdk `HalReadSMBusValue` + `HalWriteSMBusValue` signatures in `xboxkrnl.h`).
2. Inspected git status + recent commits (HEAD = cycle-38 closure `1127cafa0d`); confirmed pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` + 17 untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files + untracked `composite_preflight.py` to preserve unstaged.
3. Designed the EEPROM scratchpad contract: offset 0xFF (in 0xC0..0xFF reserved/unused tail); single byte = `0xA0 | (stage & 0x0F)`; written via `HalWriteSMBusValue(0xA8, 0xFF, FALSE, byte)` AS THE LAST INSTRUCTION before `MmAllocateContiguousMemoryEx`; sub-case (c) discriminator only (does NOT distinguish (a) from (b) — that requires a pre-`.CRT$X*` callback deferred to cycle 40+ if forced).
4. Implemented in `lib/xbed_self_witness.h` — 3 new `#define`s + 85-line cycle-39 head-comment addendum.
5. Implemented in `lib/xbed_self_witness.c` — ~40-LOC code block inside existing first-call branch with NTSTATUS host-log diagnostics.
6. Added agent surface: `cmd_eeprom_scratch_read` + `cmd_eeprom_scratch_reset` in `oracle-agent/commands.{c,h}` + dispatch-table entries + help-text lines + banner-comment addendum in `main.c`.
7. Rebuilt witness-only (155 648 B, clean lld link) + oracle-agent (417 792 B, clean lld link).
8. Local xemu cold-boot smoke attempted with `dist/xemu.app/Contents/MacOS/xemu -config_path <tmp> -display none -nographic`; cold boot through BIOS to DVD-launch did NOT reach the XBE within the bounded slice's wait budget — structural correctness coverage rests on clean nxdk link + cycle-29 shim's existing Codex-validated first-call branch + xemu's QEMU smbus-eeprom device implementing write + Codex source review.
9. Added cycle-39 README addendum (~120 LOC) + manifest `real-xbox/physical/cycle-40` expected_results section (with the G0(a)+(b) / G0(c) / G1..G4 / indeterminate decode rows + cycle-40 11-step deployment runbook delta vs cycle 36).
10. Codex round 1 (`codex review --uncommitted`): P2 finding — `cmd_eeprom_scratch_read` aliased all `0xA?` values onto sub-case (c). ADOPTED: split middle branch into `byte == 0xA4` vs other-`0xA?` (indeterminate). Rebuilt oracle-agent.
11. Codex round 2: P1 finding — EEPROM write re-fired on later calls when allocation failed, overwriting the breadcrumb byte from 0xA4 to 0xA5/0xA1/0xA3 and destroying the discriminator value. ADOPTED: added new sticky `s_eeprom_scratch_attempted` flag set BEFORE the write so it fires AT MOST ONCE per process regardless of allocation outcome. Updated `xbed_self_witness.h` cycle-39 head-comment + `witness-only/README.md` scratchpad-contract table with the at-most-once semantics. Rebuilt witness-only.
12. Codex round 3: P1 finding about pre-existing tracked drift in `retail-*.py` importing untracked `composite_preflight.py`. DEFLECTED: explicit out-of-scope per the cycle-34..38 Hermes-supervision guardrail; the cycle-39 closing commit does NOT stage those files. Cycle-39 source surface itself had no findings in round 3.
13. Wrote `.claude/state/codex-validate-last-run` marker with round-3 fingerprint.
14. Updated canonical docs/state: `handoff.md` cycle-39 entry on top above cycle-38; `decision-log.md` cycle-39 entry above cycle-38; orchestration-state quartet (this file + `claude-status.md` + `validation-status.md` + `handoff-summary.md`) closure pass.
15. (Pending — final action) Closure commit on `apple-silicon-performance`.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34+ prompt guardrail (carried forward through cycles 35 / 36 / 37 / 38 / 39).
3. [x] EEPROM scratchpad discriminator design + implementation + sticky-flag gate landed in source.
4. [x] Agent read/reset surface added; gated correctly via `unsafe.enable`.
5. [x] Both XBEs rebuilt cleanly; SHA-256s recorded.
6. [x] Codex validation completed (3 rounds; 2 source-side findings adopted with rebuilds; 1 out-of-scope finding deflected with documented reason); marker written at `.claude/state/codex-validate-last-run`.
7. [x] Canonical docs/state updated with the implementation details, validation status, and the cycle-40 recommendation.
8. [x] `git status` clean except intended cycle-39 changes + known pre-existing drift / untracked files.
9. [ ] Closure commit on `apple-silicon-performance` (pending — committed last in this session before exit).
10. [-] Local xemu cold-boot smoke did NOT reach the XBE within the bounded slice's wait budget (cold boot through BIOS to DVD load exceeds ~120 s without a snapshot path); structural correctness coverage rests on clean link + cycle-29 shim's existing Codex-validated path + xemu's QEMU smbus-eeprom device + Codex source review. Documented transparently in handoff.md + decision-log.md.

## Out-of-scope (kept bounded for cycle 39)

- NO host xemu source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact).
- NO `lib/lib.mk` touched.
- NO `lib/xbed_runtime.{c,h}` touched.
- NO `oracle-agent/{controller,smc,tier2,protocol}.{c,h}` touched.
- NO `image-blit` / `pipeline-smoke` / `mirror` / any other diag XBE touched.
- NO `witness-only/main.c` touched (the new EEPROM-write is picked up automatically via the cycle-35 `.CRT$X*` slot path).
- NO `witness-only/Makefile` touched.
- NO `nxdk/` source touched.
- NO `tools/xemu-capture/` source touched.
- NO `composite-record.sh` / `composite-preflight.sh` source touched.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO real-Xbox run (cycle 40 is Hermes's call).
- NO PushNotification — bounded implementation slice; cycle-40 outcome may warrant one if it lands a clean G0(c) vs G0(a)+(b) split.
- NO cleanup of pre-existing untracked `.hermes_*` / `composite_preflight.py` files at repo root.
- NO touch of pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` / `retail-gameplay-oracle.py` / `retail-oracle-workflow.py` / `retail-title-automation-proof.py` (preserved per cycle-34+ prompt guardrail; carried forward through cycles 35 / 36 / 37 / 38 / 39).

## Recommended cycle-40+ scope (NOT executed this session — Hermes's call)

**Primary recommendation (cycle 40).** Deploy the cycle-39 oracle-agent (v0.5) + cycle-39 witness-only XBE on real Xbox via the cycle-40 11-step runbook in `witness-only/README.md`. Steps 3a (`unsafe.enable` + `eeprom.scratch.reset`) and 11 (`eeprom.scratch.read`) bracket the existing cycle-36 chainload window. The post-run EEPROM byte at offset `0xFF` is the primary cycle-40 signal; combine with `witness.scan-self count`/`reserved1` + composite stripes per the cycle-40 G-row table.

**Cycle-41 branches conditional on cycle-40 outcome.** If outcome = G0(c) (`byte=0xA4 + WTNS count=0`) → cycle 41 explores `MmAllocateContiguousMemoryEx` allocation-flag variations (cache policy `PAGE_WRITECOMBINE` vs `PAGE_NOCACHE`; tighter / looser address-floor; alignment). If outcome = G0(a)+(b) (`byte=0x00 + WTNS count=0`) → cycle 41 needs a custom XBE-header callback that runs BEFORE nxdk's `_start` (significantly higher scope — requires modifying `nxdk/tools/cxbe/`; only fund if cycle 40 forces it).

**Secondary findings carried forward** (NOT cycle-39 fix scope): (i) composite-record.sh ffmpeg silent-stall reproduced across two physical power sessions (cycle 36 reproduction of cycle-34 finding (i)); cycle 41+ candidate is to extend composite-preflight.sh with `--require-both-detectors` OR have composite-record.sh always run a brief ffmpeg liveness check before arming the full duration. (ii) xemu-capture snapshot defaults to 720x576 PAL — cycle-35 README cycle-36 runbook includes explicit `--width 720 --height 480` as partial mitigation.
