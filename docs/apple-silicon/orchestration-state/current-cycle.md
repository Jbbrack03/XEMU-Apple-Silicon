# Current Cycle

- Cycle: 38 lld link-map symbol resolution for the cycle-37 unexplained `.text 0x16720` walker fn-ptr — **CLOSED on `apple-silicon-performance`**. Bounded tooling-only slice: re-link the cycle-35 `witness-only` build with `LDFLAGS=-map:<path>` (one additional lld flag; lld-link PE/COFF syntax) and read the resulting linker map to resolve which symbol owns `.text 0x16720` in witness-only's extra `.CRT$X*` walker group. ZERO source / script / nxdk / host xemu / lib edits; ZERO real-Xbox run. ONE bounded rebuild (witness-only only; identical `.obj` inputs; resulting binary structurally bit-identical to cycle-35 modulo 8 timestamp bytes total; tracked artifacts restored to cycle-35 deployed bytes from git HEAD post-rebuild). Codex SKIPPED per rule #15 trivial-work carve-out (rebuild is tooling-only and the deployed-binary bytes were restored to the cycle-35 Codex-validated artifact).
- Started: 2026-05-24 00:04:43Z (Hermes-supervised bounded session; Claude Code worker run launched after cycle-37 closure commit `6f74c7e449`).
- Closed: 2026-05-24 (lld map regenerated; `.text 0x16720` resolved; cycle-37 F5 hypothesis revised; cycle-39 recommendation filed; canonical docs/state synced).
- State: **CLOSED — concrete positive result.** `.text 0x16720` = `_automount_d_drive` from `libnxdk_automount_d:automount_d.obj` (nxdk default, present in every nxdk XBE that does NOT set `NXDK_DISABLE_AUTOMOUNT_D=y`; neither witness-only / mirror / pipeline-smoke sets that flag). Cycle-37 F5 "+1 unexplained walker group" REVISED to a heuristic-counting artifact — semantic CRT walker entry delta vs mirror is exactly +2 (the two cycle-35 slots), matching cycle 35's design.
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: cycle-37 closure commit `6f74c7e449` on `apple-silicon-performance`.
- Bounded goal: "execute the cycle-37 recommended next step by regenerating / extracting link-map evidence for the existing `witness-only` build so we can resolve which symbol corresponds to the unexplained `.text 0x16720` singleton function pointer in the extra `.CRT$X*` walker group; determine whether that extra walker-group entry is a benign nxdk default initializer or a substantive pre-main path unique to `witness-only`; prefer the cheapest tooling-only path first; preserve all pre-existing tracked drift and untracked `.hermes_*` / `composite_preflight.py` files."
- Result: **concrete positive symbol-resolution result.** F38.1: `.text 0x16720` = `_automount_d_drive` (nxdk default). F38.2: `.CRT$XIT` is nxdk default, not witness-only opt-in. F38.3: Full CRT walker layout enumerated (XX walker = 3 entries; XI walker = 1 entry; XC walker = 1 entry; total 5 invocations; net delta vs mirror = +2 cycle-35 entries). F38.4: Cycle-37 dump entries past `.CRT$XXZ` are NOT walker entries (resolve to `libpdclib:malloc.obj` `.rdata` constants mis-classified by the cycle-37 heuristic). F38.5: Cycle-37 F5 hypothesis REVISED — no witness-only-unique pre-`.CRT$X*` code path. G0 narrowing: 3 sub-cases remain live (a/b/c carried over from cycle 36 with cycle-37 F5 "+1 walker group drag-in" alternative ELIMINATED).

## Plan summary (this session, executed in order)

1. Read canonical docs/state (CLAUDE.md, handoff.md cycle-37+36, decision-log.md cycle-37+36, orchestration-state quartet, cycle-37 evidence dir SUMMARY.md + 02-crt-region-dump.txt + 03-evidence.txt).
2. Inspected git status + recent commits (HEAD = cycle-37 closure `6f74c7e449`); confirmed pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` + 14 untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files + untracked `composite_preflight.py` to preserve unstaged.
3. Inspected witness-only Makefile + lib.mk + nxdk/Makefile + nxdk-link wrapper script; confirmed `LD = nxdk-link` = `lld -flavor link` (PE/COFF mode); identified the map flag is `-map:<filename>` (PE/COFF syntax, not GNU ld's `-Map=`).
4. Backed up cycle-35 binary artifacts (`main.exe.cycle35-original`, `default.xbe.cycle35-original`) to cycle-38 evidence dir before any rebuild.
5. Force re-link with `LDFLAGS=-map:<path>` via `make V=1`. Identical `.obj` inputs (none changed since cycle 35); only the env-supplied `LDFLAGS` extended; make recipe unmodified.
6. Verified reproducibility: `cmp -l` of cycle-35 vs cycle-38 `default.xbe` shows ONLY 6 bytes differ across 3 separate 2-byte spans (file offsets 277/278, 329/330, 381/382 — three XBE timestamp embeddings); `main.exe` shows ONLY 2 bytes differ at offsets 129/130 (one COFF timestamp). `.text` / `.rdata` / `.data` / `.tls` content is bit-identical. The cycle-38 map is authoritative for the cycle-35 deployed binary's symbol layout.
7. Restored cycle-35 artifacts: tracked `bin/default.xbe` + `witness-only.iso` via `git checkout --`; gitignored `main.exe` from local backup. Post-restore SHA-256s match cycle-35 originals exactly (`ab52df8d…` / `a8033fef…` / `6f4ecd59…`).
8. Parsed the map to enumerate CRT subsection layout (`.CRT$XCA` through `.CRT$XXZ` with offsets and sizes) and resolved each cycle-37 walker-array fn-ptr address against the map's `Publics by Value` table.
9. Cross-referenced findings against `nxdk/lib/nxdk/automount_d.c` (the `.CRT$XIT` source) + `nxdk/lib/nxdk/Makefile:38-41` (the nxdk-default opt-in mechanism) + `nxdk/lib/pdclib/platform/xbox/crt_initializers.c` (the three walker passes `_PDCLIB_xbox_run_pre_initializers` and `_PDCLIB_xbox_run_crt_initializers`).
10. F38.1: `.text 0x16720` resolved to `_automount_d_drive` (libnxdk_automount_d:automount_d.obj).
11. F38.2: confirmed `.CRT$XIT` is nxdk default, not witness-only opt-in (force-included by `NXDK_LDFLAGS += -include:_automount_d_drive` from `nxdk/lib/nxdk/Makefile:40`).
12. F38.3: enumerated full CRT walker layout; semantic walker entry delta vs mirror = exactly +2 cycle-35 entries.
13. F38.4: VAs past `.CRT$XXZ` in cycle-37 dump resolve to `libpdclib:malloc.obj` `.rdata` constants (NOT walker entries).
14. F38.5: cycle-37 F5 hypothesis REVISED ("+1 walker group" was layout/heuristic artifact, not semantic delta).
15. Filed cycle-39 recommendation: cycle-36 Option C EEPROM scratchpad write inside `xbed_self_witness_fire` before `MmAllocateContiguousMemoryEx`; alternative candidates (`out 0xe9` host-log breadcrumb; `.CRT$XCV` slot between XCU and main) rejected with reasons.
16. Saved evidence artifacts to `benchmark-runs/cycle38-link-map-symbol-resolution-20260524T000443Z/` (gitignored): `SUMMARY.md`, `witness-only.map`, `04-crt-walker-symbol-resolution.txt`, `05-reproducibility-evidence.txt`, `main.exe.cycle35-original`, `default.xbe.cycle35-original`.
17. Updated canonical docs/state: `handoff.md` cycle-38 entry on top; `decision-log.md` cycle-38 entry above cycle-37; orchestration-state quartet (this file + claude-status.md + validation-status.md + handoff-summary.md) synced to cycle-38 closure.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward through cycles 35 / 36 / 37 / 38).
3. [x] `.text 0x16720` symbol question investigated with concrete evidence, not speculation.
4. [x] Canonical docs/state updated with the result and the next bounded recommendation.
5. [x] `git status` clean except intended doc edits + known pre-existing drift / untracked files.
6. [x] Commit slice on `apple-silicon-performance` (canonical doc updates + tracked-binary restoration to cycle-35 bytes — no net binary change vs cycle-35).
7. [-] Codex SKIPPED per rule #15 trivial-work carve-out (rebuild is tooling-only; deployed bytes restored to cycle-35 Codex-validated artifact).

## Out-of-scope (kept bounded for cycle 38)

- NO host xemu source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact).
- NO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact).
- NO `lib/lib.mk` touched.
- NO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact).
- NO `xbed_runtime.{c,h}` touched.
- NO image-blit touched.
- NO `witness-only/main.c` / `Makefile` / `manifest.json` / `README.md` touched.
- NO `nxdk/` source touched (lld map READ `nxdk/lib/nxdk/automount_d.c` + `nxdk/lib/nxdk/Makefile` + `nxdk/lib/pdclib/platform/xbox/crt_initializers.c`, but did NOT modify).
- NO `tools/xemu-capture/` source touched.
- NO `scripts/apple-silicon/composite-record.sh` / `composite-preflight.sh` source touched.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO net XBE binary change vs cycle-35 (rebuild + restoration confirms cycle-35 deployed bytes preserved).
- NO real-Xbox run.
- NO PushNotification — bounded tooling slice, not a milestone (cycle-38 result is informative but does not unblock M15; it tightens the cycle-39 scope window).
- NO cleanup of pre-existing untracked `.hermes_*` / `composite_preflight.py` files at repo root.
- NO touch of pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` / `retail-gameplay-oracle.py` / `retail-oracle-workflow.py` / `retail-title-automation-proof.py` (preserved per cycle-34 prompt guardrail; carried forward through cycles 35 / 36 / 37 / 38).

## Recommended cycle-39+ scope (NOT executed this session — Hermes's call)

**Primary recommendation (cycle 39, lowest-cost highest-information).** Cycle-36 Option C: EEPROM scratchpad write inside `xbed_self_witness_fire` BEFORE `MmAllocateContiguousMemoryEx`, paired with an agent EEPROM read-back via the `unsafe.enable` + EEPROM-write surface. The static surface has now been exhausted as a discriminator for the 3 remaining G0 sub-cases (a/b/c). A successful EEPROM tick on real Xbox would uniquely discriminate sub-case (c) ("MmAllocateContiguousMemoryEx returns NULL silently") from sub-cases (a) ("crash in pre-`.CRT$X*` startup") and (b) ("crash inside `_witness_only_pre_main_crt_xx`'s body BEFORE `MmAllocateContiguousMemoryEx`"). Scope is bounded to `lib/xbed_self_witness.c` + the agent's EEPROM-write path; cycle-23 lockstep, cycle-29 self-witness shim semantics, and the cycle-35 slot mechanism all preserved. Codex validation REQUIRED (rule #15; non-trivial diff in lib/ + agent).

**Cycle-40 branches (conditional on cycle-39 outcome).** If EEPROM tick survives → G0 narrows to (c); cycle 40 explores `MmAllocateContiguousMemoryEx` allocation-flag variations (cache policy; alignment; address-floor). If EEPROM tick does NOT land → G0 narrows to (a); cycle 40 needs a custom XBE-header callback mechanism that runs BEFORE nxdk's `_start` (significantly higher scope; requires nxdk modification — only fund if cycle-39 forces it).

**Deprioritized.** `out 0xe9` host-log breadcrumb inside `_witness_only_pre_main_crt_xx` (provides no real-Xbox visibility — `out 0xe9` is xemu-only). `.CRT$XCV` slot between `.CRT$XCU` and `main()` (adds no new information beyond cycle-35's stage-4 fire — cycle 36 G0 outcome already shows the earlier `.CRT$XXC` slot didn't fire). Custom XBE-header callback (high-scope; only fund if cycle-39 forces it via NOT-landed EEPROM tick).

Secondary findings carried forward from cycle 36 (NOT cycle-38 fix scope): (i) composite-record.sh ffmpeg silent-stall reproduced across two physical power sessions; cycle 39+ candidate is to extend composite-preflight.sh with `--require-both-detectors` OR have composite-record.sh always run a brief ffmpeg liveness check before arming the full duration. (ii) xemu-capture snapshot defaults to 720x576 PAL — cycle-35 README cycle-36 runbook already includes explicit `--width 720 --height 480` in the example invocation as a partial mitigation.
