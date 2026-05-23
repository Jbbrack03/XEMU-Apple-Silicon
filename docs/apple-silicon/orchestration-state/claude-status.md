# Claude Status

- Objective: cycle 35 Path A.4 pre-main breadcrumb via `.CRT$X*` static-init slots — implement + locally smoke-validate + Codex-validate the cheapest mechanism that runs strictly BEFORE `main()` to distinguish γ.0 (`main()` never entered AT ALL) from γ.1 (`XVideoSetMode` itself faulted before returning) — the ambiguity left by cycle-34's F4 outcome.
- Status: **CLOSED.** Implementation landed (option (1) `.CRT$X*` slot stamp chosen; options (2) custom XBE-header callback and (3) direct NV2A CRTC writes rejected with documented reasoning). Local xemu smoke PASSED: both `.CRT$XXC` (stage=4) and `.CRT$XCU` (stage=5) slots fire BEFORE `main()` enters, with the WTNS counter ticking 0 → 1 → 2 pre-main, then 2 → 3 → 4 across the in-`main()` cycle-29 self-witness fires. Codex 4 rounds: round 1 MAJOR ISSUES (3 findings all adopted — G2 over-claim softened, new G2' row added for cycle-32 F4'-analogue, canonical-state drift synced); round 2 MINOR ISSUES (2 findings all adopted — handoff-summary missed G2', handoff.md residual "will be written"); round 3 MINOR ISSUES (1 finding adopted — abbreviated G2'/G3 shorthand surfaces updated); round 4 LOOKS GOOD. Validation marker written. Closure commit landed as `515e03f4e7` on `apple-silicon-performance`.

## Why cycle 35 ran this session

Cycle 34 closure (commit `b5327d4d17`) recorded OUTCOME F4 = zero stripes + `witness.scan = D-cycle-27` + `witness.scan-self = count=0` → per the cycle-31 cycle-32 9-row discriminator table this collapses to (γ.0) "execution never entered `main()` AT ALL" OR (γ.1) "`XVideoSetMode` itself faulted hard before returning" — cycle-22 pre-main-crash hypothesis FULLY CORROBORATED in its strongest form, but cycle 34 alone cannot tell γ.0 from γ.1. Cycle 35's bounded assignment: ship the highest-value cycle-35+ follow-up — a pre-main breadcrumb mechanism — preferring the least-invasive candidate that runs before `main()` while preserving the cycle-23 / cycle-29 / cycle-31 discriminator contracts.

## What this session shipped

1. **Design choice with full rationale.** Option (1) `.CRT$X*` static-init slot stamp CHOSEN. Mechanism documented + exercised on every nxdk-built XBE (`nxdk/lib/pdclib/platform/xbox/crt_initializers.c`). ZERO nxdk / linker / XBE-header changes required. ZERO new shared-lib code (reuses cycle-29 `xbed_self_witness_fire`). Scope = `witness-only/main.c` + paired docs only. Option (2) custom XBE-header callback REJECTED (would require modifying nxdk's XBE-header generator; γ.0 sub-windows it could uniquely distinguish are vanishingly unlikely cycle-22 hang sites). Option (3) direct NV2A CRTC register writes REJECTED (doesn't address γ.0-vs-γ.1; widens NV2A surface).
2. **`witness-only/main.c` edits.** Cycle-35 head-comment addendum (~100 LOC); two locally-defined stage constants (`WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XX = 4` + `WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XC = 5`; deliberately NOT added to `lib/xbed_a4_witness.h` to keep that header's cycle-23 lockstep namespace untouched); two static `_PVFV`-shaped helper functions; two `__attribute__((section(".CRT$X*C"), used))` function-pointer slot declarations. The `.CRT$XXC` slot runs in `WinMainCRTStartup`'s entry thread AFTER `__security_init_cookie` + TLS setup + `_PDCLIB_xbox_libc_init` but BEFORE `thrd_create(main_wrapper)`; the `.CRT$XCU` slot runs in `main_wrapper`'s thread immediately BEFORE `main()`.
3. **Rebuild.** `witness-only/bin/default.xbe` 155 648 B (UNCHANGED from cycle 31; new code fits in existing nxdk page boundary). `witness-only.iso` 720 896 B (unchanged). Benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles; ZERO new warnings.
4. **Local xemu smoke validation PASS.** Captured stderr from a 12 s xemu spawn with `XEMU_GUEST_LOG=1` against `witness-only.iso` shows, in expected order: `.CRT$XXC` breadcrumb + WTNS allocate phys=0x03fdf000 + fire stage=4 counter=1 → `.CRT$XCU` breadcrumb + WTNS fire stage=5 counter=2 → `main() entered (cycle 25)` → cycle-23 XCTR fires (phys=0 expected on standalone) → in-main WTNS fires counter=3 then counter=4. Demonstrates the `.CRT$X*` slot mechanism works correctly AND the cycle-29 shim's idempotent same-page reuse holds across all 4 WTNS fires.
5. **Paired-doc updates.** `witness-only/README.md` cycle-35 addendum: design rationale (option 1 chosen, options 2/3 rejected with reasoning table); expanded WTNS counter encoding; new cycle-36 G-row discriminator table (G0 / G1 / G2 / G2' / G3 / G4 — G2' is the cycle-32 F4'-analogue for graceful XVideoSetMode FALSE); cycle-36 deployment runbook (11-step, extends cycle-32 runbook with `--overwrite` upload flag); cycle-35 build artifacts + local validation evidence + cross-references to nxdk CRT source. `witness-only/manifest.json` title + purpose + new `real-xbox/physical/cycle-36` `expected_results`.
6. **Canonical docs/state sync.** `handoff.md` cycle-35 entry on top with full cycle-34 entry preserved unchanged below; `decision-log.md` cycle-35 entry above cycle-34; orchestration-state quartet (this file + current-cycle.md + validation-status.md + handoff-summary.md) closure pass.
7. **Codex validation per rule #15.** Round-by-round disposition recorded; validation marker written at `.claude/state/codex-validate-last-run` on LOOKS GOOD.

## Session progress

- [x] Read required docs/state files (CLAUDE.md, handoff cycle-34 + cycle-31, decision-log cycle-34, orchestration-workflow.md, oracle-and-xbe.md, witness-only/README.md, witness-only/main.c, manifest.json, lib/xbed_a4_witness.h, lib/xbed_self_witness.h).
- [x] Confirmed git state + pre-existing tracked drift + pre-existing untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward).
- [x] Verified nxdk CRT entry-point structure (`crt0.c` + `crt_initializers.c`); identified `.CRT$XXC` + `.CRT$XCU` as the two least-invasive pre-`main()` slot positions.
- [x] Designed cycle-35 mechanism with full options 1/2/3 trade-off analysis; option (1) chosen.
- [x] Edited `witness-only/main.c` with head-comment addendum + 2 helpers + 2 slot declarations.
- [x] Rebuilt `witness-only/bin/default.xbe` (155 648 B unchanged) + `witness-only.iso` (720 896 B unchanged) cleanly.
- [x] Local xemu smoke (12 s timeout, `XEMU_GUEST_LOG=1`): both `.CRT$X*` slots fire BEFORE `main()` with WTNS counter at 2 pre-main, then 4 by end of run.
- [x] Cleaned up smoke scratch dir.
- [x] Updated `witness-only/README.md` cycle-35 addendum + cycle-36 G-row discriminator table + cycle-36 runbook + cross-references.
- [x] Updated `witness-only/manifest.json` title + purpose + cycle-36 expected_results. JSON validated.
- [x] `handoff.md` + `decision-log.md` cycle-35 entries on top; cycle-34 entries preserved unchanged below.
- [x] Orchestration-state quartet closure pass (this file + current-cycle.md + validation-status.md + handoff-summary.md).
- [x] Codex validation per rule #15 round-by-round disposition recorded.
- [x] Closure commit on `apple-silicon-performance` landed as `515e03f4e7`.

## Confidence + risk notes

- **HIGH confidence in `.CRT$X*` slot mechanism.** Local xemu smoke directly proves the section attribute placement works — the linker placed both slot pointers between nxdk's `.CRT$XXA`/`XXZ` and `.CRT$XCA`/`XCZ` sentinels respectively, and both walker functions invoke them in the expected order. This is host-toolchain-deterministic; clang/lld's `.CRT$X*` section handling on real-Xbox-targeted PE/COFF output matches the cycle-35 design.
- **HIGH confidence in idempotent shim reuse.** Local smoke shows the cycle-29 `xbed_self_witness_fire` shim correctly allocates the WTNS page on the FIRST call (`.CRT$XXC` slot) and reuses it on all 3 subsequent calls (`.CRT$XCU` + 2 in-`main()` fires). Counter ticks 1 → 2 → 3 → 4 with the correct stage byte at each fire.
- **HIGH confidence cycle-23 lockstep contract is intact.** ZERO touch of `lib/xbed_a4_witness.{c,h}`; cycle-35 stage codes (4, 5) defined locally in `witness-only/main.c` so the namespace at 1, 2, 3 is unaffected. Cycle-23 reader (`oracle-agent/commands.c::cmd_witness_scan`) + cycle-27 preserve gate + lockstep filter set all unchanged.
- **HIGH confidence cycle-29 self-witness shim is intact.** ZERO touch of `lib/xbed_self_witness.{c,h}` or `lib/lib.mk`; cycle 35 only adds new CALL SITES from new `.CRT$X*` slots. The shim's `s_witness_page` static + `s_witness_phys` static + `wbinvd` per-fire flush + `'WTNS' magic + version 1 + reserved0 = (0xA4 << 24) | stage + reserved1 = counter` layout all unchanged.
- **HIGH confidence cycle-31 visual breadcrumb semantics are intact.** ZERO touch of the cycle-31 `xbed_breadcrumb_init` / `xbed_breadcrumb_paint` helpers or the 5 `paint(N)` call sites in `main()`. The cycle-31 stripe interpretation per `witness-only/README.md` cycle-31 cycle-32 9-row discriminator table is unchanged; cycle 35 adds the new G-row table that EXTENDS (not replaces) the F-row table.
- **MEDIUM confidence cycle 36 will land at G2 (the γ.1 candidate).** Cycle 34's F4 collapsed γ.0 and γ.1; the γ.0 sub-windows the `.CRT$X*` slots can isolate (G0 / G1) are vanishingly unlikely (they would require crashes inside `_start` / `__security_init_cookie` / TLS setup / `thrd_create` / `.CRT$XI*` — paths exercised by every nxdk-built XBE that successfully runs on this Xbox). G2 (both pre-main slots ran; `main()` did NOT enter, or entered but crashed inside paint(0) = `XVideoSetMode`) is the prior-most-likely cycle-36 outcome — but the cycle-35 design does NOT assume any particular row will land; it ships the discriminator and lets cycle 36 answer.
- **LOW risk to cycle-23 / cycle-27 / cycle-29 / cycle-31 / cycle-33 prior guarantees.** ZERO source touched in any of those slices; their Codex validations remain in force.

## What this session does NOT do

- NO host xemu source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact).
- NO `lib/xbed_self_witness.{c,h}` touched (cycle-29 shim intact; only new call sites added in `witness-only/main.c`).
- NO `lib/lib.mk` touched.
- NO `oracle-agent/*` touched.
- NO `xbed_runtime.{c,h}` touched.
- NO image-blit touched.
- NO `nxdk/` source touched.
- NO `tools/xemu-capture/` source touched.
- NO `scripts/apple-silicon/composite-record.sh` or `composite-preflight.sh` source touched (cycle-33 implementations intact; cycle-34 secondary findings remain filed for separate cycle).
- NO retail-title / §G.5 / RT-as-texture work.
- NO real-Xbox run (cycle 36 scope, Hermes's call).
- NO flag default flips; NO M15 movement; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` unchanged.
- NO PushNotification — bounded implementation slice, not a milestone.
- NO cleanup of `.hermes_*` files.
- NO touch of pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` / `retail-gameplay-oracle.py` / `retail-oracle-workflow.py` / `retail-title-automation-proof.py` (preserved per cycle-34 prompt guardrail).
- NO touch of pre-existing untracked `scripts/apple-silicon/composite_preflight.py` (preserved per the same guardrail).

## Next proposed action

Closure commit landed as `515e03f4e7` on `apple-silicon-performance`. The substantive next slice is cycle 36 (Hermes's call): FTP-deploy cycle-35 `witness-only/bin/default.xbe` (155 648 B; SAME path; use `--overwrite` because size matches cycle 31's exactly) + ARM composite-capture leg + runxbe + final `witness.scan-self` to read the `reserved1` counter for G-row classification. Secondary findings worth queuing for cycle 37+ are the same two cycle-34 findings still open (composite-preflight TCC asymmetry; xemu-capture PAL-default-on-NTSC silent-zero).
