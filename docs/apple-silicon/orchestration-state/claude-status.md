# Claude Status

- Objective: cycle 38 lld link-map symbol resolution for the cycle-37 unexplained `.text 0x16720` walker fn-ptr — bounded tooling-only slice: re-link the cycle-35 `witness-only` build with `LDFLAGS=-map:<path>` (one additional lld-link flag) and read the resulting linker map to determine whether `.text 0x16720` is a benign nxdk default initializer or a substantive pre-main path unique to `witness-only`. Aim: revise or confirm the cycle-37 F5 "+1 unexplained walker group" hypothesis with concrete symbol-resolution evidence.
- Status: **CLOSED. Concrete positive result.** `.text 0x16720` = `_automount_d_drive` from `libnxdk_automount_d:automount_d.obj` (nxdk default, force-included for every nxdk XBE that does NOT set `NXDK_DISABLE_AUTOMOUNT_D=y`; neither witness-only / mirror / pipeline-smoke sets that flag). Cycle-37 F5 "+1 unexplained walker group" REVISED to a heuristic-counting artifact — semantic CRT walker entry delta vs mirror is exactly +2 (the two cycle-35 slots), matching cycle 35's design. SUMMARY.md + 04-crt-walker-symbol-resolution.txt + 05-reproducibility-evidence.txt + witness-only.map (104 KB) on disk. handoff.md + decision-log.md cycle-38 entries on top with cycle-37 + cycle-36 + cycle-35 preserved unchanged below. Codex SKIPPED per rule #15 trivial-work carve-out (rebuild is tooling-only; deployed bytes restored to cycle-35 Codex-validated artifact).

## Why cycle 38 ran this session

Cycle 37 closure (commit `6f74c7e449`) recorded F5 as the only structural drift between witness-only and mirror: +1 unexplained `.CRT$X*` walker group with a singleton fn-ptr at `.text 0x16720`. Cycle 37's closeout enumerated three cycle-37+ candidate sources (`profiling.obj` / `fiber.obj` `.CRT$XXT`; `automount_d.obj` `.CRT$XIT`) but did NOT verify which one. The cycle-38 recommendation was: re-link witness-only with lld's map output and resolve the symbol — tooling-only change; no source / shared-lib edit; one re-link. Cycle 38 executes that recommendation as a bounded tooling-only slice.

## What this session shipped

1. **lld-link map flag identification.** `LD = nxdk-link` resolves to `lld -flavor link` (PE/COFF mode); the map flag in PE/COFF mode is `-map:<filename>` (NOT GNU ld's `-Map=<filename>` — that syntax doesn't apply to lld-link).
2. **Reproducibility-preserving rebuild.** Backed up cycle-35 `main.exe` + `bin/default.xbe`; force re-linked with `LDFLAGS=-map:<path>` via `make V=1`; identical `.obj` inputs (none changed since cycle 35); resulting binary structurally bit-identical to cycle-35 modulo 8 timestamp bytes total (verified via `cmp -l`: 6 bytes differ in `default.xbe` across 3 separate 2-byte spans = three XBE timestamp embeddings; 2 bytes differ in `main.exe` at one COFF timestamp). Restored cycle-35 artifacts: tracked `bin/default.xbe` + `witness-only.iso` via `git checkout --`; gitignored `main.exe` from local backup. Post-restore SHA-256s match cycle-35 originals exactly (`ab52df8d…` / `a8033fef…` / `6f4ecd59…`).
3. **Symbol resolution.** Parsed lld map's `Publics by Value` table; resolved each cycle-37 walker-array fn-ptr against the table. `.text 0x16720` = `_automount_d_drive` (libnxdk_automount_d:automount_d.obj). All 8 cycle-37 fn-ptrs resolved (5 walker entries + 3 past-XXZ entries that turn out to be `libpdclib:malloc.obj` `.rdata` constants mis-classified by the cycle-37 heuristic).
4. **Five findings filed (F38.1..F38.5).** F38.1: `.text 0x16720` = `_automount_d_drive`. F38.2: `.CRT$XIT` is nxdk default, not witness-only opt-in (`nxdk/lib/nxdk/Makefile:38-41`). F38.3: full CRT walker layout enumerated (XX = 3 entries; XI = 1 entry; XC = 1 entry; net delta vs mirror = +2 cycle-35 entries). F38.4: cycle-37 dump entries past `.CRT$XXZ` are NOT walker entries. F38.5: cycle-37 F5 "+1 walker group" REVISED to layout/heuristic artifact.
5. **Hypothesis state update.** γ.0 narrowed further: no witness-only-unique pre-`.CRT$X*` code path exists; 3 live G0 sub-cases remain (a/b/c carried over from cycle 36); cycle-37 F5 "+1 walker group drag-in" alternative ELIMINATED.
6. **Evidence on disk.** `benchmark-runs/cycle38-link-map-symbol-resolution-20260524T000443Z/{SUMMARY.md, witness-only.map, 04-crt-walker-symbol-resolution.txt, 05-reproducibility-evidence.txt, main.exe.cycle35-original, default.xbe.cycle35-original}` (gitignored per project convention).
7. **Cycle-39 recommendation filed.** The static surface has now been exhausted as a discriminator for the 3 remaining G0 sub-cases. Recommended cycle 39 = cycle-36 Option C (EEPROM scratchpad write inside `xbed_self_witness_fire` BEFORE `MmAllocateContiguousMemoryEx`, paired with agent EEPROM read-back). Alternatives considered and rejected: `out 0xe9` host-log breadcrumb (invisible on real Xbox); `.CRT$XCV` slot between XCU and main (adds no new info beyond cycle-35 stage-4 fire).
8. **Canonical docs/state synced.** handoff.md + decision-log.md cycle-38 entries on top with cycle-37 + cycle-36 + cycle-35 preserved unchanged below; orchestration-state quartet (this file + current-cycle.md + validation-status.md + handoff-summary.md) closure pass complete.

## Session progress

- [x] Read required docs/state (CLAUDE.md, handoff.md cycle-37+36, decision-log.md cycle-37+36, orchestration-state quartet, cycle-37 evidence dir SUMMARY.md + 02-crt-region-dump.txt + 03-evidence.txt).
- [x] Confirmed git state + pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward through cycles 35 / 36 / 37 / 38).
- [x] Inspected witness-only Makefile + lib.mk + nxdk/Makefile + nxdk-link wrapper; identified lld-link map flag syntax.
- [x] Backed up cycle-35 binaries; force re-linked with `LDFLAGS=-map:<path>`; verified reproducibility (8 timestamp bytes total diff); restored cycle-35 artifacts; verified post-restore SHA-256s match.
- [x] Parsed lld map's CRT subsection layout + `Publics by Value` table; resolved all 8 cycle-37 walker-array fn-ptrs.
- [x] Cross-referenced findings against `nxdk/lib/nxdk/automount_d.c` + `nxdk/lib/nxdk/Makefile:38-41` + `nxdk/lib/pdclib/platform/xbox/crt_initializers.c`.
- [x] Filed F38.1..F38.5 findings + cycle-39 recommendation.
- [x] SUMMARY.md + 04-crt-walker-symbol-resolution.txt + 05-reproducibility-evidence.txt written; evidence artifacts saved under gitignored benchmark-runs dir.
- [x] handoff.md + decision-log.md cycle-38 entries on top with cycle-37 + cycle-36 + cycle-35 preserved unchanged below.
- [x] Orchestration-state quartet closure pass.
- [x] Codex SKIPPED per rule #15 trivial-work carve-out (rebuild is tooling-only; deployed bytes restored to cycle-35 Codex-validated artifact).
- [ ] Closure commit on `apple-silicon-performance` (pending — committed last in this session before exit).

## Confidence + risk notes

- **HIGH confidence in F38.1 symbol resolution.** The lld map's `Publics by Value` table directly resolves `.text 0x16720` to `_automount_d_drive` from `libnxdk_automount_d:automount_d.obj`. Reproducibility verified: the cycle-38 rebuild differs from cycle-35 in only 8 timestamp bytes total — `.text` / `.rdata` / `.data` / `.tls` content is bit-identical, so the map's symbol layout is authoritative for the cycle-35 deployed binary.
- **HIGH confidence in F38.2 nxdk-default claim.** `nxdk/lib/nxdk/Makefile:38-41` explicitly force-includes `_automount_d_drive` (`NXDK_LDFLAGS += -include:_automount_d_drive`) for every nxdk XBE that does NOT set `NXDK_DISABLE_AUTOMOUNT_D=y`. Direct grep confirmed neither witness-only / mirror / pipeline-smoke sets that flag.
- **HIGH confidence in F38.3 walker enumeration.** `nxdk/lib/pdclib/platform/xbox/crt_initializers.c:15-48` defines exactly two walker passes (`_PDCLIB_xbox_run_pre_initializers` walks `__xx_a..__xx_z`; `_PDCLIB_xbox_run_crt_initializers` walks `__xi_a..__xi_z` then `__xc_a..__xc_z`). Witness-only's map shows the cycle-35 slots correctly positioned between the appropriate sentinels.
- **HIGH confidence in F38.5 cycle-37 F5 revision.** The cycle-37 zero-delimited-group heuristic counted the merged `.CRT=.rdata` region structurally; the lld map gives the semantic walker-walked region exactly. Net semantic delta vs mirror = +2 cycle-35 entries.
- **LOW risk to cycle-23 / cycle-27 / cycle-29 / cycle-31 / cycle-33 / cycle-35 / cycle-37 prior guarantees.** ZERO source touched in any of those slices; their Codex validations remain in force; the cycle-38 rebuild was tooling-only and the deployed bytes were restored to the cycle-35 Codex-validated artifact.
- **MEDIUM confidence in cycle-39 recommendation prioritization.** EEPROM scratchpad write is the only remaining low-cost discriminator; cycle 38 explicitly enumerates and rejects two cheaper alternatives (`out 0xe9` host-log breadcrumb and `.CRT$XCV` slot) with concrete reasons. The custom XBE-header callback fallback only fires if cycle 39 forces it (EEPROM tick NOT landing).

## What this session does NOT do

- NO host xemu source touched.
- NO `lib/xbed_*` / `oracle-agent/*` / `witness-only/main.c` / `witness-only/Makefile` / `witness-only/manifest.json` / `witness-only/README.md` touched.
- NO `nxdk/` source touched (lld map READ `nxdk/lib/nxdk/automount_d.c` + `nxdk/lib/nxdk/Makefile` + `nxdk/lib/pdclib/platform/xbox/crt_initializers.c` as read-only inspection, but did NOT modify).
- NO `tools/xemu-capture/` source touched.
- NO `composite-record.sh` / `composite-preflight.sh` source touched.
- NO net XBE binary change vs cycle-35 (rebuild + restoration confirms cycle-35 deployed bytes preserved).
- NO real-Xbox run.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips; NO M15 movement; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` unchanged.
- NO PushNotification — bounded tooling slice, cycle-38 result is informative but does not unblock M15.
- NO cleanup of `.hermes_*` files or pre-existing tracked drift / untracked `composite_preflight.py`.

## Next proposed action

Cycle 38 closes with the `.text 0x16720` symbol question definitively answered. The substantive next slice is cycle 39 (Hermes's call): cycle-36 Option C — EEPROM scratchpad write inside `xbed_self_witness_fire` BEFORE `MmAllocateContiguousMemoryEx`, paired with agent EEPROM read-back via the `unsafe.enable` + EEPROM-write surface. Scope is bounded to `lib/xbed_self_witness.c` + the agent's EEPROM-write path; cycle-23 lockstep, cycle-29 self-witness shim semantics, and the cycle-35 slot mechanism all preserved. Codex validation REQUIRED (rule #15; non-trivial diff in lib/ + agent).
