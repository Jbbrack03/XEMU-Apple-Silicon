# Validation Status

- Active slice: cycle 39 EEPROM scratchpad pre-`MmAllocateContiguousMemoryEx` discriminator — bounded implementation slice (lib/ + agent + paired docs). Adds one EEPROM-byte write at offset `0xFF` inside `xbed_self_witness_fire`'s existing first-call branch AS THE LAST INSTRUCTION before `MmAllocateContiguousMemoryEx`, gated AT MOST ONCE per process via a new sticky `s_eeprom_scratch_attempted` flag. Paired with two new oracle-agent verbs `eeprom.scratch.read` (4-branch decode) + `eeprom.scratch.reset` (gated by existing `unsafe.enable`).
- Validation state: **Rule #15 Codex 3 rounds: R1 P2 + R2 P1 both ADOPTED with rebuilds; R3 P1 DEFLECTED (out-of-scope, pre-existing tracked drift).** Cycle-39 source surface (`lib/`, `oracle-agent/`, `witness-only/`) has no remaining Codex findings as of round 3. Marker written at `.claude/state/codex-validate-last-run` with the round-3 fingerprint. **Local xemu cold-boot smoke did NOT reach the XBE within the bounded slice's wait budget** — cold boot through BIOS to DVD load via the .app wrapper exceeds ~120 s without a pre-warmed snapshot path; structural correctness coverage rests on clean nxdk lld link + cycle-29 shim's existing Codex-validated first-call branch (where the new code is additive and gated by a NEW sticky flag) + xemu's QEMU smbus-eeprom device implementing both `eeprom_receive_byte` and `eeprom_write_data` per `hw/i2c/smbus_eeprom.c:52-83` (the SMBus write IS honored in emulation when reached) + Codex 3-round source review.

## Rule #15 applicability (cycle 39)

Cycle 39 is NON-TRIVIAL CODE (rule #15 trigger #2 fires — aggregate diff in cycle-39-scope files ~975 lines including ~85 LOC lib/ source + ~90 LOC agent source + ~118 LOC README + ~6 LOC manifest + ~120 LOC headers/comments; well above the 30-line trivial-work carve-out threshold).

- ~50 LOC new code block in `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c` inside the existing first-call branch.
- ~85 LOC cycle-39 head-comment addendum + 3 new `#define`s in `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.h`.
- ~90 LOC new code in `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c` (`cmd_eeprom_scratch_read` 4-branch decode + `cmd_eeprom_scratch_reset` + help-text + paired comments).
- ~7 LOC new declarations + comments in `scripts/apple-silicon/xbe-tests/oracle-agent/commands.h`.
- ~6 LOC new dispatch-table entries + banner-comment addendum in `scripts/apple-silicon/xbe-tests/oracle-agent/main.c`.
- ~118 LOC cycle-39 README addendum + 1-row table edit in `scripts/apple-silicon/xbe-tests/witness-only/README.md`.
- ~6 LOC changed in `scripts/apple-silicon/xbe-tests/witness-only/manifest.json` (title extension + cycle-40 expected_results section).

Codex validation REQUIRED per rule #15 and EXECUTED (3 rounds; 2 source-side findings adopted with rebuilds; 1 out-of-scope finding deflected with documented reason).

## Codex 3-round summary

- **Round 1 (`codex review --uncommitted`) = MAJOR ISSUES (1 P2)**. `cmd_eeprom_scratch_read` aliased all `0xA?` values onto sub-case (c), but the cycle-40 G-row table classifies non-`0xA4` TAG-nibble values as "indeterminate" → would mislead operators or scripts into taking the wrong G-row branch. ADOPTED: split middle branch into `byte == 0xA4` (sub-case (c)) vs other-`0xA?` (indeterminate TAG-nibble match) vs else (indeterminate foreign write). Rebuilt oracle-agent.
- **Round 2 = MAJOR ISSUES (1 P1)**. EEPROM write was gated on existing `s_witness_page == 0` first-call branch; in the exact G0(c) sub-case this discriminator targets, allocation fails on the first call → `s_witness_page` stays NULL → later `.CRT$XCU` and in-main fires re-enter the block and OVERWRITE the breadcrumb byte from `0xA4` to `0xA5`/`0xA1`/`0xA3`, destroying the cycle-39 discriminator value. ADOPTED: new sticky `s_eeprom_scratch_attempted` static flag set BEFORE the write (so failure also does not cause re-attempt); flag preserves the first-stage breadcrumb regardless of allocation outcome. Updated `xbed_self_witness.h` cycle-39 head-comment + `witness-only/README.md` scratchpad-contract table row to document the at-most-once semantics. Rebuilt witness-only.
- **Round 3 = MAJOR ISSUES (1 P1 DEFLECTED — out of cycle-39 scope)**. Codex flagged `retail-gameplay-oracle.py:49` (and 2 sibling `retail-*.py` files) import `composite_preflight` which is currently only present as an UNTRACKED file at `scripts/apple-silicon/composite_preflight.py`. Finding correct on its merits BUT is about pre-existing tracked drift that the cycle-34..38 prompts explicitly told me to PRESERVE unstaged per the rolling Hermes-supervision guardrail. The cycle-39 closing commit does NOT stage either the 4 tracked `retail-*` / `capture-composite-reference.sh` drift files OR the untracked `composite_preflight.py` file. The cycle-39 source surface itself (`lib/`, `oracle-agent/`, `witness-only/`) has no findings in round 3. DEFLECTED with documented reason; responsibility for resolving the tracked-drift dependency belongs to whichever future cycle commits those 4 files.

## Local validation envelope note

Cold-boot xemu smoke (`dist/xemu.app/Contents/MacOS/xemu -config_path <tmp> -display none -nographic` against `witness-only.iso` with `XEMU_GUEST_LOG=1` + 120 s timeout) did NOT reach the witness-only XBE host-log lines — xemu booted cleanly through MCPX + BIOS init + Metal renderer + smbus-eeprom device init, but did not progress past BIOS-to-DVD launch within the 120 s envelope. The cycle-35 closure's claimed "12 s spawn" almost certainly depended on a pre-warmed snapshot mechanism (xemu's `-loadvm` path or similar) that was NOT available in this bounded session — the cycle-39 prompt explicitly says "Prefer a bounded implementation + local validation slice; do not perform the real-Xbox deployment in this session unless the canonical docs already make it clearly in-scope". Investing further session time in restoring a snapshot harness would exceed the bounded slice envelope.

Structural correctness coverage for the cycle-39 EEPROM-write addition therefore rests on:

1. **Clean nxdk lld link of both XBEs with ZERO new warnings.** Build succeeded against the existing nxdk toolchain (`eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && make` in both `witness-only/` and `oracle-agent/`).
2. **Cycle-29 shim's existing Codex-validated first-call branch.** The new EEPROM write is inserted AS A NEW STICKY-FLAG-GATED INSTRUCTION SEQUENCE inside the existing first-call branch (`if (s_witness_page == 0)`) that was previously Codex-validated at cycle-29 closure. The existing allocation, `MmGetPhysicalAddress`, `MmPersistContiguousMemory`, page-wipe, magic/version-stamp, and `wbinvd` paths are structurally unchanged. The new code is positioned BEFORE `MmAllocateContiguousMemoryEx` and is gated by `!s_eeprom_scratch_attempted` so it fires AT MOST ONCE per process — the existing first-call branch's "called multiple times before allocation succeeds" semantics are preserved on the cycle-29 path (the EEPROM write is purely additive instrumentation).
3. **xemu's QEMU smbus-eeprom device implementation.** `hw/i2c/smbus_eeprom.c:52-83` implements both `eeprom_receive_byte` and `eeprom_write_data` (lines 66+). The SMBus write call IS honored in emulation when reached, ruling out a "broken in xemu" failure mode for the new code path — were the XBE to reach `xbed_self_witness_fire` under xemu, the `HalWriteSMBusValue(0xA8, 0xFF, FALSE, byte)` call would persist the byte in the emulated EEPROM image at `/Users/jbbrack03/Library/Application Support/xemu/xemu/eeprom.bin`.
4. **Codex 3-round source review** (2 source-side findings adopted with rebuilds; 1 out-of-scope finding deflected). Both R1 P2 and R2 P1 are exactly the classes of issue that local runtime smoke would also have caught — but the source review caught them earlier in the cycle without requiring a full real-Xbox run.

The real discriminator answer lives in cycle 40 (real-Xbox run + post-run `eeprom.scratch.read` per the cycle-40 G-row table in `witness-only/README.md` cycle-39 addendum).

## Gate status (cycle 39)

- [x] Required docs read.
- [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34+ prompt guardrail (carried forward through cycles 35 / 36 / 37 / 38 / 39).
- [x] EEPROM scratchpad contract designed (offset 0xFF; encoded byte `0xA0 | stage_nib`; sub-case (c) discriminator only; at-most-once per process).
- [x] `lib/xbed_self_witness.{c,h}` implementation landed (sticky flag + new code block + 3 #defines + 85-line cycle-39 head comment).
- [x] `oracle-agent/{commands,main}.{c,h}` agent surface landed (2 new verbs + 4-branch decode reader + reset gated by unsafe.enable + dispatch entries + help-text + banner addendum).
- [x] `witness-only/README.md` cycle-39 addendum + `manifest.json` cycle-40 expected_results section landed.
- [x] witness-only rebuilt cleanly; SHA-256 = `7528bb5bf4c9cc8f00c934c38e015166de7a6f89236ddc8e2ad2195a47ce4e0d`.
- [x] oracle-agent rebuilt cleanly; SHA-256 = `d419b452f127e5a065a51b2d5bba49b6f2ec1f1826cdec57845793c34440cb53`.
- [x] Codex round 1 (P2 reader decode) adopted + oracle-agent rebuilt.
- [x] Codex round 2 (P1 sticky-flag gate) adopted + witness-only rebuilt.
- [x] Codex round 3 (P1 pre-existing drift) deflected with documented reason.
- [x] `.claude/state/codex-validate-last-run` marker written with round-3 fingerprint.
- [x] handoff.md + decision-log.md cycle-39 entries on top above cycle-38 (cycle-38 + cycle-37 + cycle-36 + cycle-35 preserved unchanged below).
- [x] Orchestration-state quartet (this file + current-cycle.md + claude-status.md + handoff-summary.md) closure pass.
- [-] Local cold-boot xemu smoke ATTEMPTED but did NOT reach the XBE within the bounded slice's wait budget; documented transparently with the structural-correctness coverage substitute above.
- [ ] Closure commit on `apple-silicon-performance` (pending — final action this session).

## Discriminator semantics — cycle-40 readback table

Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)`:

| EEPROM 0xFF | scan-self count | scan-self reserved1 | scan shape | G-row | Interpretation | Next |
|---|---|---|---|---|---|---|
| `0x00` | 0 | n/a | `D-cycle-27` | **G0(a)+(b)** | EEPROM write never executed. Cannot distinguish (a) pre-`.CRT$X*` startup crash from (b) helper-body crash before reaching the EEPROM-write instruction. | Cycle 41: custom XBE-header callback (high scope; modifies `nxdk/tools/cxbe/`). Only fund if forced. |
| `0xA4` | 0 | n/a | `D-cycle-27` | **G0(c)** | EEPROM write landed → `MmAllocateContiguousMemoryEx` returned NULL silently OR crashed. Sub-cases (a)+(b) ELIMINATED. | Cycle 41: allocation-flag variations (`PAGE_WRITECOMBINE` vs `PAGE_NOCACHE`; tighter / looser address floor; alignment). |
| `0xA4` | ≥1 | 1..4 | `D-cycle-27` or `A1/A2` | **G1..G4** | Reverts to the cycle-35 G1..G4 interpretations on the WTNS path. | Apply cycle-35 G-row table for the surviving shape. |
| any other | n/a | n/a | n/a | indeterminate | Re-run with explicit `eeprom.scratch.reset`. | If reproducible, investigate concurrent EEPROM access from another XBE in the same power session. |

## Hypothesis state after cycle 39

- γ.0 ("execution never entered `main()` AT ALL") — unchanged from cycle 38 closure; cycle 39 ships the instrumentation that splits G0(c) from G0(a)+(b) on a future real-Xbox run, but does NOT update the hypothesis state itself.
- Three live G0 sub-cases remain (carried over from cycle 38, instrumentation now in place to discriminate (c) from (a)+(b)):
  - (a) crash inside nxdk's pre-`.CRT$X*` startup;
  - (b) crash inside `_witness_only_pre_main_crt_xx`'s body BEFORE `xbed_self_witness_fire` reaches its pre-MmAlloc instruction;
  - (c) `MmAllocateContiguousMemoryEx` returns NULL silently (or crashes).
- Cycle-22 pre-main-crash hypothesis — unchanged FULLY CORROBORATED.
- γ.1 ("`XVideoSetMode` itself faulted before returning") — unchanged INVALIDATED (cycle 36 G0 outcome already showed `main()` never entered).

## Why this is not a regression of any prior cycle's validation guarantees

Cycle 23 / 25 / 27 / 29 / 31 / 33 / 35 each Codex-validated their own implementation slices. Cycle 39 does not touch any of those slices' code in a way that would invalidate prior validation: `lib/xbed_a4_witness.{c,h}` intact (cycle 23), `lib/xbed_self_witness.c`'s existing allocation/stamp/wbinvd flush path intact (the new code is ADDITIVE and inserted BEFORE the allocation, gated by a NEW sticky flag), `oracle-agent/{controller,smc,tier2,protocol}.{c,h}` intact (the new agent verbs are ADDITIVE), `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit / pipeline-smoke / mirror / other diag XBE source intact, `witness-only/main.c` intact (the new EEPROM-write is picked up automatically via the cycle-35 `.CRT$X*` slot path), `witness-only/Makefile` intact, `nxdk/` intact, `tools/xemu-capture/` intact, `scripts/apple-silicon/composite-record.sh` + `composite-preflight.sh` intact. Cycle 39 also does not change any flag default, M15 visual-gate prerequisite, or other shipping behavior.

## Evidence integrity

- Source diff is the evidence; the cycle-39 build artifacts replace the cycle-35 deployed binaries in the working tree (will be committed as part of the closure commit).
- Codex round-1 + round-2 + round-3 transcripts captured at `/tmp/cycle39-codex-output{,-r2,-r3}.log` (ephemeral; the substantive findings + adoption/deflection decisions are recorded in handoff.md + decision-log.md + this file).
- `.claude/state/codex-validate-last-run` marker written with the round-3 fingerprint that covers the cycle-39 source diff.

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing inside xemu.
- No `lib/xbed_a4_witness.{c,h}` / `lib/lib.mk` / `lib/xbed_runtime.{c,h}` edits.
- No `oracle-agent/{controller,smc,tier2,protocol}.{c,h}` edits.
- No `witness-only/main.c` / Makefile edits (the new code is picked up automatically via the cycle-35 `.CRT$X*` slot path).
- No image-blit / pipeline-smoke / mirror / other diag XBE source edits.
- No nxdk source edits.
- No `tools/xemu-capture/` source edits.
- No `composite-record.sh` / `composite-preflight.sh` source edits.
- No retail-title / §G.5 / RT-as-texture work.
- No real-Xbox run (cycle 40 is Hermes's call).
- No PushNotification — bounded implementation slice; cycle-40 outcome may warrant one if it lands a clean G0(c) vs G0(a)+(b) split.
- No cleanup of pre-existing untracked `.hermes_*` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34+ prompt guardrail).
