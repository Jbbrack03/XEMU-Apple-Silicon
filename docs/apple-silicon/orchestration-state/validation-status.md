# Validation Status

- Active slice: cycle 35 Path A.4 pre-main breadcrumb via `.CRT$X*` static-init slots — implementation-only bounded code slice; non-trivial diff (~200 LOC C source + ~150 LOC docs); rule #15 trigger #2 fires.
- Validation state: **Rule #15 Codex validation completed (4 rounds).** Round 1 = MAJOR ISSUES with 1 high + 1 medium + 1 low (all 3 adopted: G2 row over-claim softened to candidate-only window with explicit two-sub-case enumeration + cycle-37 `.CRT$XCV` follow-up; new G2' row added for cycle-32 F4'-analogue shape; canonical-state drift synced). Round 2 = MINOR ISSUES with 1 medium + 1 low (handoff-summary G-row block missed G2'; handoff.md residual "will be written" language; both adopted). Round 3 = MINOR ISSUES with 1 low (abbreviated G2'/G3 shorthand in README runbook + manifest purpose + handoff/decision-log/current-cycle/claude-status; adopted). Round 4 = LOOKS GOOD (all round-3 findings RESOLVED; no new findings). Local xemu smoke PASS (both `.CRT$XXC` + `.CRT$XCU` slots fire BEFORE `main()` in the expected order; WTNS counter ticks 1 → 2 pre-main, then 3 → 4 across in-`main()` fires). Validation marker written at `.claude/state/codex-validate-last-run` on round 4.

## Rule #15 applicability (cycle 35)

Rule #15 trigger #2 (non-trivial uncommitted code in `xemu-fork/`; renderer / TCG / NV2A / build / runtime flag plumbing / apple-silicon scripts; aggregate diff > 30 lines) FIRES this cycle:

- `scripts/apple-silicon/xbe-tests/witness-only/main.c` modified (head-comment addendum ~100 LOC + 2 new helper functions + 2 `.CRT$X*` slot declarations; ~200 LOC total source diff).
- `scripts/apple-silicon/xbe-tests/witness-only/README.md` modified (cycle-35 addendum + cycle-36 G-row discriminator table + cycle-36 runbook; ~150 LOC docs diff).
- `scripts/apple-silicon/xbe-tests/witness-only/manifest.json` modified (title + purpose extension + new `real-xbox/physical/cycle-36` `expected_results` entry).
- `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` + `witness-only.iso` rebuilt (155 648 B + 720 896 B; both unchanged from cycle 31 — new code fits in existing nxdk page boundary).
- Canonical docs sync: `handoff.md` + `decision-log.md` cycle-35 entries on top; orchestration-state quartet closure pass.

Doc-only / run-only carve-out does NOT apply (this is a source-edit slice, distinct from cycles 26 / 28 / 30 / 32 / 34 which were run-only). Codex is mandatory before closure.

## Gate status (cycle 35)

- [x] Required docs read.
- [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail.
- [x] Design choice + rationale documented (option (1) `.CRT$X*` slot stamp chosen; options (2) custom XBE-header callback + (3) direct NV2A CRTC writes rejected with reasoning).
- [x] `witness-only/main.c` edited (head-comment addendum + 2 helpers + 2 slot declarations).
- [x] Build success: 155 648 B + 720 896 B (unchanged from cycle 31).
- [x] Local xemu smoke PASS: both slots fire BEFORE `main()`; WTNS counter 1 → 2 pre-main, then 3 → 4 across in-`main()` fires; cycle-31 stripe paint sequence + cycle-23 XCTR fires + cycle-29 in-`main()` WTNS fires all unchanged.
- [x] `witness-only/README.md` + `manifest.json` cycle-35 addendum + cycle-36 expected_results.
- [x] `handoff.md` + `decision-log.md` cycle-35 entries on top.
- [x] Orchestration-state quartet closure pass.
- [x] Codex validation per rule #15 round-by-round disposition recorded.
- [ ] Closure commit on `apple-silicon-performance` (pending at session end).

## Local validation evidence

- `eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && make` in `scripts/apple-silicon/xbe-tests/witness-only/` → rc=0; benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles. ZERO new warnings.
- `strings -a bin/default.xbe | grep -E "(cycle 35|pre-main|CRT)"` confirms the 4 new strings linked in: `witness-only: pre-main-xx fire returned phys=0x%08lx`, `witness-only: pre-main-xc fire returned phys=0x%08lx`, `witness-only: .CRT$XCU pre-main breadcrumb running (cycle 35)`, `witness-only: .CRT$XXC pre-main breadcrumb running (cycle 35)`.
- Local xemu spawn against `witness-only.iso` (12 s timeout, `XEMU_GUEST_LOG=1`, `-display xemu`, scratch-cloned HDD). Stderr captured the following ordered host-log emission:
  1. `xemu-guest-log: witness-only: .CRT$XXC pre-main breadcrumb running (cycle 35)` ← `.CRT$XXC` slot fires
  2. `xemu-guest-log: xbed_self_witness: enter stage=4`
  3. `xemu-guest-log: xbed_self_witness: allocated self-witness page phys=0x03fdf000 virt=0x83fdf000 magic='WTNS' version=1` ← first WTNS fire allocates the page
  4. `xemu-guest-log: xbed_self_witness: fired stage=4 at phys=0x03fdf000 ... counter=1`
  5. `xemu-guest-log: witness-only: pre-main-xx fire returned phys=0x03fdf000`
  6. `xemu-guest-log: witness-only: .CRT$XCU pre-main breadcrumb running (cycle 35)` ← `.CRT$XCU` slot fires
  7. `xemu-guest-log: xbed_self_witness: enter stage=5`
  8. `xemu-guest-log: xbed_self_witness: fired stage=5 at phys=0x03fdf000 ... counter=2` ← reuses same page
  9. `xemu-guest-log: witness-only: pre-main-xc fire returned phys=0x03fdf000`
  10. `xemu-guest-log: witness-only: main() entered (cycle 25)` ← cycle-31 entry semantics intact
  11. `xemu-guest-log: xbed_a4_witness: enter stage=1` ← cycle-23 XCTR fires unchanged
  12. `xemu-guest-log: xbed_a4_witness: no XCTR buffer found at stage=1` (expected on standalone xemu; no agent running)
  13. `xemu-guest-log: witness-only: fire1 returned phys=0x00000000`
  14. (cycle-23 stage=3 fire same shape)
  15. `xemu-guest-log: xbed_self_witness: fired stage=1 ... counter=3` ← in-`main()` WTNS fire reuses page
  16. `xemu-guest-log: xbed_self_witness: fired stage=3 ... counter=4` ← final WTNS fire
- Final counter on WTNS page = 4 (2 pre-main + 2 in-`main()` fires), matching the cycle-35 design exactly. Last stamp = stage 3 (in-`main()` POST_MARKER0). The same page (`phys=0x03fdf000`) was reused across all 4 fires per the cycle-29 shim's `s_witness_page != 0` idempotent early return.

## Codex validation marker

Round-by-round disposition recorded (round 1 MAJOR ISSUES — 3 findings all adopted; round 2 MINOR ISSUES — 2 findings all adopted; round 3 MINOR ISSUES — 1 finding adopted; round 4 LOOKS GOOD); `.claude/state/codex-validate-last-run` updated on round 4.

## Why this is not a regression of any prior cycle's validation guarantees

- Cycle 23 / 25 / 27 / 29 / 31 / 33 each Codex-validated their own implementation slices (XBE source + oracle-agent source + apple-silicon scripts). Cycle 35 does not touch any of those slices' code: `lib/xbed_a4_witness.{c,h}` intact, `lib/xbed_self_witness.{c,h}` intact, `oracle-agent/*` intact, `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit intact, `nxdk/` intact, `tools/xemu-capture/` intact, `scripts/apple-silicon/composite-record.sh` + `composite-preflight.sh` intact.
- Cycle 35 only touches `witness-only/main.c` (which itself was last Codex-validated at cycle-31 closure `41f350c174`). The cycle-31 in-`main()` semantics (paint sequence + cycle-23 XCTR fires + cycle-29 in-`main()` WTNS fires + cycle-31 stripe-paint sequence + final `Sleep(2000)` + `HalReturnToFirmware`) are bit-identical to cycle-31; cycle 35 only ADDS pre-main code (head-comment + 2 helpers + 2 slot declarations) that runs strictly BEFORE `main()` enters.

## Evidence integrity

- Source-file modifications restricted to `scripts/apple-silicon/xbe-tests/witness-only/` + canonical docs/state.
- Local validation evidence captured in this session (12 s xemu smoke transcript above).
- Real-Xbox evidence is cycle-36 scope (Hermes's call); cycle-35 does NOT include a real-Xbox run.

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing inside xemu.
- No shared-lib edits; no oracle-agent edits; no `lib.mk` edits.
- No nxdk source edits.
- No `tools/xemu-capture/` source edits.
- No `composite-record.sh` / `composite-preflight.sh` source edits.
- No retail-title / §G.5 / RT-as-texture work.
- No real-Xbox run (cycle 36 scope, Hermes's call).
- No cycle-36 pre-flight preparation work (Hermes's call).
- No cleanup of pre-existing untracked `.hermes_*` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34 prompt guardrail).
