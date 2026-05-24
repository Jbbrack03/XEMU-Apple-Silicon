# Validation Status

- Active slice: cycle 42F post-stamp in-process WTNS-magic readback discriminator — bounded implementation+build+Codex slice adding milestone marker `0xBC` to the cycle-42D stage==6 fast path, gated on a volatile in-process readback of `vp_c42d[0] == XBED_SELF_WITNESS_MAGIC` after the cycle-42D `0xBA` full-completion marker. Option 2 of the cycle-42E SUMMARY's three mutually-exclusive cycle-42F slice candidates (cleanest in-XBE-only disambiguator for the cycle-42E (α) vs (β) collapse).
- Validation state: **Rule #15 SATISFIED via Codex 5-round GREEN ADOPTION.** R1 MED → R2 MED → R3 MED → R4 MED → R5 GREEN explicit "DEPLOY-READY". ALL hard findings adopted across the 5 rounds via documentation-only edits; ZERO C source body changes between R1 and R5. Codex marker at `.claude/state/codex-validate-last-run` refreshed to `2026-05-24T15:46:00Z` after R5. Rule #4 (no doc drift) SATISFIED via handoff.md + decision-log.md + orchestration-state quartet + cycle-42F SUMMARY.md sync.

## Codex 5-round validation summary

| Round | Finding | Adoption |
|---|---|---|
| R1 | MED — documentation over-claims wbinvd-then-load forces DRAM commit + 0xBC proves stamp "actually committed to memory"; correct narrower claim is in-process observability on the cached `phys \| 0x80000000` alias post-flush. Cited Intel SDM Vol. 3A §2.8.6 / §14. Notes: no behavioral regression; honest-framing/matrix delta otherwise consistent; uncached RAM mirror is `phys \| 0xB0000000` in this codebase (cached alias choice still correct for this discriminator). | ADOPTED via 3 documentation-only edits in cycle-42F-introduced text. |
| R2 | MED — R1 primary closed BUT 5 pre-existing legacy comments (NOT introduced by cycle 42F) still attribute post-chainload visibility / survival to wbinvd / MmPersistContiguousMemory in isolation: `xbed_self_witness.c:{35, 283, 325, 954}` + `xbed_self_witness.h:212`. Internally contradicts new cycle-42F honest-framing block. | ADOPTED via 5 documentation-only INTENT-vs-measured-fact edits. |
| R3 | MED — `xbed_self_witness.c:457` says "observing 0xBC at post-run PROVES (α)" but (α) was defined in the header as the (0xBA, count=0) collapse including post-chainload non-discoverability; header matrix explicitly keeps `(0xBC, 0)` vs `(0xBC, 1+)` distinct. Byte-only 0xBC does NOT prove the full (α) case. | ADOPTED via 1 documentation-only narrowing edit. |
| R4 | MED — second spot at `.c:424-428` carries the same over-claim "(α) ... is the load-bearing reading". Same fix shape as R3. | ADOPTED via 1 documentation-only narrowing edit. |
| R5 | **GREEN — "DEPLOY-READY".** Explicit "No findings"; R4 site closed; R1+R2+R3+R4 sites all remain closed; residual grep hits are attribution quotes describing prior wording (not live claims). | n/a (final round). |

## Rule #15 applicability (cycle 42F)

Cycle 42F ships ~6 lines of new C body (the readback `if` block at the tail of the stage==6 fast path) + 1 new milestone-index #define + extensive in-source documentation. The aggregate diff is >> the 30-line trivial-skip threshold, so Codex validation is mandatory. The Codex 5-round GREEN ADOPTION cycle is what closes the rule #15 obligation for this slice. The post-R1 documentation-only adoption pattern (ZERO C source body changes between R1 and R5) mirrors the cycle-42B 5-round + cycle-42D 4-round pattern from the same code area.

Stop-hook fingerprint for rule #15 will not re-fire because the closure commit covers only the cycle-42F source edits + cycle-42F doc edits + cycle-42F orchestration-state quartet — all of which are within the validated R5-GREEN state.

## Cycle-42F implementation summary

### Source change

- `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.h`: +1 new milestone constant `XBED_SELF_WITNESS_C42D_M_POST_READBACK = 0xCu` (yielding post-run byte 0xBC). Added comprehensive Cycle-42F Safety-notes subsection (rationale + readback semantics + Honest-framing what-0xBC-does-and-does-NOT-prove + post-run interpretation matrix delta + preservation contract). Added 0xBC entry to the cycle-42D milestone byte table. Added `(0xBC, 0)` and `(0xBC, 1+)` rows to the post-run interpretation matrix. Updated function-contract docstring with the cycle-42F addendum. Updated historical-references paragraph. Softened the cycle-42A subsection's "both pages survive the chainload" wording to INTENT vs measured-fact framing per R2 MED.
- `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c`: Appended readback `if (vp_c42d[0] == XBED_SELF_WITNESS_MAGIC) { self_witness_cycle42d_marker(XBED_SELF_WITNESS_C42D_M_POST_READBACK); }` after the existing `self_witness_cycle42d_marker(XBED_SELF_WITNESS_C42D_M_FULL_COMPLETION)` call in the stage==6 fast path. Added comprehensive cycle-42F implementation comment block above the readback (rationale + what 0xBC proves + what 0xBC does NOT prove + Honest framing carried from cycle-42D R1.HIGH). Softened the `self_witness_wbinvd` helper docstring + the cycle-42D body's wbinvd-visibility note + the cycle-42D body's MmPersistContiguousMemory comment + the cycle-29 stages-!=6 path's MmPersistContiguousMemory comment per R2 MED. Updated end-of-bypass comment marker.

### Build verification

- Clean nxdk `make` succeeds with `-entry:witness_only_pre_winmain_crt_startup` selected (cycle-42B thunk preserved).
- `llvm-readobj --file-headers main.exe` reports `AddressOfEntryPoint=0x37D0`, `ImageBase=0x10000` ⇒ virtual entry VA = 0x137D0 (cycle-42D R4-GREEN was 0x137C0; the 16-byte forward move reflects the cycle-42F readback compare + branch + new marker call site expansion).
- `llvm-objdump -d main.exe --start-address=0x13210 --stop-address=0x13830` confirms the new readback compare + conditional 0xBC marker call site at 0x13479..0x1348c, with all cycle-42D milestone markers + marker helper + wbinvd helper + stages-!=6 path bit-identical to cycle-42D R4-GREEN.
- Final cycle-42F R5-GREEN deployed-build SHA = `7831b0706850d961805d35f2f336c32866b2204dca9a470418895588e69f441c` (155 648 B). Oracle-agent unchanged at cycle-39 v0.5 SHA `d419b452…`.

## What cycle 42F PROVES (post-deployment in a future bounded slice)

Cycle 42F's value proposition is per-outcome:

- **`(eeprom=0xBC, count=0)` ⇒** cycle-42E (β)-via-unfired-WTNS-stamp RULED OUT + cycle-22 axis advances to "CONFIRMED on the bypass-body + stamp-observability axis". The cycle-42E "discoverability gap" framing is the correct reading; the residual in-window-torn-down vs out-of-window-surviving disambiguation requires the cycle-42F alternative slice (broaden agent-side `witness.scan-self`).
- **`(eeprom=0xBA, count=0)` ⇒** unchanged from cycle-42E shape; could be either (a) readback returned non-magic (would support (β)-via-unfired-stamp) or (b) readback returned magic but 0xBC SMBus write failed silently. (a) vs (b) NOT distinguishable from EEPROM alone — but the asymmetry between observed-0xBA-after-42F and observed-0xBC-after-42F is what carries the new disambiguation power.
- **`(eeprom=0xBC, count=1+)` ⇒** NOT expected from the cycle-42E baseline; would imply intermittent post-chainload visibility on this hardware. Reproducibility check required; if confirmed, cycle-22 axis FULLY CONFIRMED.

## What cycle 42F does NOT prove

- It does NOT prove the WTNS page will survive the post-XBE chainload return to the dashboard (`MmPersistContiguousMemory` retention semantics on this BIOS revision for pre-WinMainCRT allocations remain unmeasured).
- It does NOT prove the page's phys falls inside the agent-side `witness.scan-self` enumeration window `[0x80010000, 0x84000000]`.
- It does NOT prove the body executed past milestone 10 (the bypass returns IMMEDIATELY after the 0xBC marker write or after 0xBA if the readback fails).
- It does NOT change the cycle-42D `(0xBn, count=0)` rows for n < 10.
- It does NOT add behavioral changes to stages-!=6 path (cycle-29 .CRT$XXC stage=4, .CRT$XCU stage=5, in-main WTNS1/WTNS2 stage=1/3 fires) — those still execute the unchanged cycle-42A code path.

## Rule #4 — doc-sync

- handoff.md cycle-42F entry prepended above cycle-42E entry.
- decision-log.md cycle-42F entry prepended above cycle-42E entry.
- Orchestration-state quartet updated: this file + current-cycle.md + claude-status.md + handoff-summary.md.
- cycle-42F SUMMARY.md written to gitignored run dir `benchmark-runs/cycle42f-readback-marker-20260524T152642Z/` with 5 Codex prompts + 5 Codex outputs + 4 per-round SHA files + 4 per-round XBE binaries + per-round PE headers + per-round disassembly + per-round build-verification + 4 codex-input.diffs.

## Recommended cycle 42G (Hermes's call)

Cycle 42G = real-Xbox deployment of the cycle-42F R5-GREEN witness-only XBE. Reuse the cycle-42E 18-step deployment runbook with ONE interpretation change: the expected post-run `eeprom.scratch.read` value set widens from `{0x00, 0xA4, 0xA6, 0xB0..0xBA, 0xBB}` to `{0x00, 0xA4, 0xA6, 0xB0..0xBA, 0xBB, 0xBC}`. Full interpretation matrix lives in `xbed_self_witness.h` "Cycle-42F" Safety-notes subsection.

Out-of-scope-for-cycle-42G follow-ups (if cycle 42G yields `(0xBC, 0)`): the cycle-42F SUMMARY's "recommended next slice" framing for the residual in-window-torn-down vs out-of-window-surviving disambiguation is the cycle-42F alternative slice — option 1 of the original cycle-42E menu (broaden `oracle-agent/commands.c::cmd_witness_scan_self` phys-range enumeration). That is an oracle-agent source change and would re-trigger rule #15.
