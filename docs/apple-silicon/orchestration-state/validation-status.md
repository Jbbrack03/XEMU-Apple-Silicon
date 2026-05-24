# Validation Status

- Active slice: cycle 38 lld link-map symbol resolution for the cycle-37 unexplained `.text 0x16720` walker fn-ptr — TOOLING-ONLY / DOC-ONLY (ZERO source/script/XBE edits; ONE bounded rebuild with `LDFLAGS=-map:<path>` that produces a binary structurally bit-identical to cycle-35 modulo 8 timestamp bytes, with tracked artifacts restored to cycle-35 deployed bytes post-rebuild; ZERO real-Xbox run; ZERO touch of nxdk / host xemu / shared witness libs / oracle-agent / tooling source).
- Validation state: **Rule #15 Codex SKIPPED under trivial-work carve-out**. Slice produced no non-trivial code change; rebuild is tooling-only (`-map` flag added; identical `.obj` inputs) and the deployed-binary bytes were restored to the cycle-35 Codex-validated artifact (closure commit `515e03f4e7`); cycle-35 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed artifact. **Concrete positive symbol-resolution result**: `.text 0x16720` = `_automount_d_drive` from `libnxdk_automount_d:automount_d.obj` (nxdk default, present in every nxdk XBE that does NOT set `NXDK_DISABLE_AUTOMOUNT_D=y`; neither witness-only / mirror / pipeline-smoke sets that flag). Cycle-37 F5 "+1 unexplained walker group" REVISED to a heuristic-counting artifact — semantic CRT walker entry delta vs mirror is exactly +2 (the two cycle-35 slots), matching cycle 35's design.

## Rule #15 applicability (cycle 38)

Cycle 38 is TOOLING-ONLY / DOC-ONLY:

- ZERO `scripts/apple-silicon/xbe-tests/witness-only/main.c` / `Makefile` / `manifest.json` / `README.md` edits.
- ZERO `scripts/apple-silicon/xbe-tests/lib/xbed_*` edits.
- ZERO `scripts/apple-silicon/xbe-tests/lib/lib.mk` edits.
- ZERO `scripts/apple-silicon/xbe-tests/oracle-agent/` edits.
- ZERO `scripts/apple-silicon/composite-record.sh` / `composite-preflight.sh` edits.
- ZERO host xemu source (`hw/`, `ui/`, `target/`, `include/`) touched.
- ZERO `nxdk/` source touched (lld map READ `nxdk/lib/nxdk/automount_d.c` + `nxdk/lib/nxdk/Makefile` + `nxdk/lib/pdclib/platform/xbox/crt_initializers.c` as read-only inspection only).
- ZERO `tools/xemu-capture/` source touched.
- ONE bounded rebuild: `LDFLAGS=-map:<path>` added via env. Make recipe unmodified. Identical `.obj` inputs. Reproducibility verified via `cmp -l`: cycle-35 vs cycle-38 `default.xbe` differs in ONLY 6 bytes across 3 separate 2-byte spans (three XBE timestamp embeddings; file offsets 277/278, 329/330, 381/382); `main.exe` differs in ONLY 2 bytes (one COFF timestamp; offsets 129/130). `.text` / `.rdata` / `.data` / `.tls` bit-identical.
- Post-rebuild: tracked artifacts (`bin/default.xbe` + `witness-only.iso`) restored via `git checkout --`; gitignored `main.exe` restored from local backup. Post-restore SHA-256s match cycle-35 originals exactly (`ab52df8d…` / `a8033fef…` / `6f4ecd59…`).

Trivial-work carve-out APPLIES — the slice's only persistent writes are (i) canonical docs/state (handoff.md + decision-log.md + orchestration-state quartet) and (ii) gitignored evidence artifacts under `benchmark-runs/cycle38-link-map-symbol-resolution-20260524T000443Z/`. **Codex SKIPPED with explicit justification** (the validated artifact is the cycle-35 binary; cycle-35 Codex green at closure `515e03f4e7` covers it; cycle-38 rebuild was tooling-only and the deployed bytes were restored to the cycle-35 Codex-validated artifact).

## Gate status (cycle 38)

- [x] Required docs read.
- [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward through cycles 35 / 36 / 37 / 38).
- [x] lld-link map flag syntax identified (`-map:<filename>`; PE/COFF syntax, not GNU ld's `-Map=`).
- [x] Cycle-35 binaries backed up before rebuild.
- [x] Force re-link with `LDFLAGS=-map:<path>`; identical `.obj` inputs.
- [x] Reproducibility verified via `cmp -l` (8 timestamp bytes total diff; `.text`/`.rdata`/`.data`/`.tls` bit-identical).
- [x] Cycle-35 artifacts restored: tracked `bin/default.xbe` + `witness-only.iso` via `git checkout --`; gitignored `main.exe` from local backup; SHA-256s verified.
- [x] lld map parsed; CRT subsection layout enumerated; all 8 cycle-37 walker-array fn-ptrs resolved.
- [x] F38.1..F38.5 findings recorded in SUMMARY.md + handoff.md cycle-38 entry + decision-log.md cycle-38 entry.
- [x] Cycle-39 recommendation filed (cycle-36 Option C EEPROM scratchpad write).
- [x] Orchestration-state quartet (this file + current-cycle.md + claude-status.md + handoff-summary.md) closure pass.
- [-] Codex SKIPPED per rule #15 trivial-work carve-out (explicit justification recorded).
- [ ] Closure commit on `apple-silicon-performance` (pending — committed last in this session).

## Symbol-resolution evidence

- **F38.1 — `.text 0x16720` resolved.** lld map's `Publics by Value` table: `0001:00005720 _automount_d_drive 0000000000016720 libnxdk_automount_d:automount_d.obj`. Source: `nxdk/lib/nxdk/automount_d.c:35` registers `automount_d_drive_p` in `.CRT$XIT` via `__attribute__((section(".CRT$XIT"), used)) int (__cdecl *const automount_d_drive_p)(void) = automount_d_drive;`.
- **F38.2 — `.CRT$XIT` is nxdk default, NOT witness-only opt-in.** `nxdk/lib/nxdk/Makefile:38-41` force-includes `_automount_d_drive` (`NXDK_LDFLAGS += -include:_automount_d_drive`) for every nxdk XBE that does NOT set `NXDK_DISABLE_AUTOMOUNT_D=y`. Direct read of witness-only's Makefile, lib.mk, mirror's Makefile, pipeline-smoke's Makefile confirms NONE sets that flag. All three nxdk XBEs include `_automount_d_drive`.
- **F38.3 — Full CRT walker subsection layout enumerated.** Witness-only's three semantic CRT walker passes (per `nxdk/lib/pdclib/platform/xbox/crt_initializers.c:15-48`): `_PDCLIB_xbox_run_pre_initializers` walks `.CRT$XXA..XXZ` invoking 3 functions in order — `_witness_only_pre_main_crt_xx` (cycle-35; at `.text 0x11000`), `_fls_init` (nxdk default `libwinapi:fiber.obj`; at `.text 0x2bd00`), `_tsc_freq_init` (nxdk default `libwinapi:profiling.obj`; at `.text 0x2d950`). `_PDCLIB_xbox_run_crt_initializers` walks `.CRT$XIA..XIZ` invoking 1 function — `_automount_d_drive` (nxdk default; at `.text 0x16720`) — then walks `.CRT$XCA..XCZ` invoking 1 function — `_witness_only_pre_main_crt_xc` (cycle-35; at `.text 0x11040`). Total 5 walker invocations. Net delta vs a baseline non-witness nxdk XBE: exactly +2 walker entries (the two cycle-35 slots), matching cycle 35's design.
- **F38.4 — Cycle-37 dump entries past `.CRT$XXZ` are NOT walker entries.** VAs `0x3303c` onward resolve to `.text` addresses inside `libpdclib:malloc.obj` (`_dlmalloc_set_footprint_limit + 100`, `_internal_mallinfo + 256`, `_internal_mallinfo + 572`). Most likely `.rdata`-resident pdclib malloc constants / vtables / jump-table entries that the cycle-37 zero-delimited-group heuristic mis-classified as function pointers because their values happen to fall in the `.text` VA range. They do NOT execute as part of any pre-main walker.
- **F38.5 — Cycle-37 F5 hypothesis REVISED.** The "+1 unexplained walker group" was a heuristic-counting artifact of how the cycle-37 Python XBE parser delineated zero-bounded fn-ptr runs in the merged `.CRT=.rdata` region; the actual semantic CRT walker entry count delta vs mirror is exactly +2 (the two cycle-35 slots; XCU + XXC) — NOT +1 walker group + +2 fn-ptrs.

## Hypothesis state after cycle 38

- γ.0 ("execution never entered `main()` AT ALL") — narrowed further. Static binary surface vs mirror is now confirmed structurally identical modulo the 2 cycle-35 walker entries. The G0 real-Xbox divergence cannot be explained by a witness-only-unique pre-main code path drag-in.
- Three live G0 sub-cases remain (carried over from cycle 36, with cycle-37 F5 "+1 walker group drag-in" alternative ELIMINATED):
  - (a) crash inside nxdk's pre-`.CRT$X*` startup (`_start` / `__security_init_cookie` / TLS-size computation / `_PDCLIB_xbox_libc_init`);
  - (b) crash inside `_witness_only_pre_main_crt_xx`'s body BEFORE `MmAllocateContiguousMemoryEx` is reached;
  - (c) `MmAllocateContiguousMemoryEx` returns NULL silently from `.CRT$XXC` (edge case in `lib/xbed_self_witness.c:54-74`).
- Cycle-22 pre-main-crash hypothesis — unchanged FULLY CORROBORATED; the G0 narrowing is finer-grained but still pre-`main()`.
- γ.1 ("`XVideoSetMode` itself faulted before returning") — UNCHANGED INVALIDATED (cycle 36 G0 outcome already showed `main()` never entered).

## Why this is not a regression of any prior cycle's validation guarantees

Cycle 23 / 25 / 27 / 29 / 31 / 33 / 35 each Codex-validated their own implementation slices. Cycle 38 does not touch any of those slices' code: `lib/xbed_a4_witness.{c,h}` intact, `lib/xbed_self_witness.{c,h}` intact, `oracle-agent/*` intact, `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit intact, `nxdk/` intact (READ-only), `tools/xemu-capture/` intact, `scripts/apple-silicon/composite-record.sh` + `composite-preflight.sh` intact, `witness-only/main.c` + `Makefile` + `README.md` + `manifest.json` intact. Cycle 38 also does not change any flag default, M15 visual-gate prerequisite, or other shipping behavior; the net XBE binary bytes are unchanged vs cycle-35 (rebuild + restoration preserved the cycle-35 deployed artifact).

## Evidence integrity

- Symbol-resolution evidence captured under `benchmark-runs/cycle38-link-map-symbol-resolution-20260524T000443Z/` (gitignored per project convention): `SUMMARY.md` (full findings document); `witness-only.map` (104 KB lld linker map: timestamp, section directory, CRT subsection layout, `Publics by Value` table); `04-crt-walker-symbol-resolution.txt` (per-fn-ptr symbol resolution + walker subsection layout cross-referenced with `crt_initializers.c`); `05-reproducibility-evidence.txt` (cycle-35 vs cycle-38 SHA-256s + `cmp -l` byte-level diff + post-restore SHA-256 verification); `main.exe.cycle35-original` + `default.xbe.cycle35-original` (backups taken before rebuild; tracked artifacts subsequently restored in place).

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing inside xemu.
- No shared-lib edits; no oracle-agent edits; no `lib.mk` edits.
- No `witness-only/main.c` / Makefile / README.md / manifest.json edits.
- No nxdk source edits (lld map READ `nxdk/lib/nxdk/automount_d.c` + `nxdk/lib/nxdk/Makefile` + `nxdk/lib/pdclib/platform/xbox/crt_initializers.c` only).
- No `tools/xemu-capture/` source edits.
- No `composite-record.sh` / `composite-preflight.sh` source edits.
- No retail-title / §G.5 / RT-as-texture work.
- No real-Xbox run.
- No net XBE binary change (rebuild + restoration preserves cycle-35 deployed bytes).
- No cycle-39+ EEPROM-scratchpad-write work (Hermes's call).
- No cleanup of pre-existing untracked `.hermes_*` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34 prompt guardrail).
