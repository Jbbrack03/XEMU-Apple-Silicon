# Validation Status

- Active slice: cycle 37 static XBE binary diff `witness-only` vs `pipeline-smoke` + `mirror` — ANALYSIS-ONLY / DOC-ONLY (ZERO source/script/XBE edits; ZERO XBE rebuilds; ZERO real-Xbox run; ZERO touch of nxdk / host xemu / shared witness libs / oracle-agent / tooling source).
- Validation state: **Rule #15 Codex SKIPPED under analysis/doc-only carve-out**. Slice produced no non-trivial code change; no rule-#15 trigger fired. The cycle-35 binary observed bit-identically this cycle is the cycle-35 build that passed 4-round Codex green at cycle-35 closure commit `515e03f4e7`; cycle-35 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed artifact. **Narrowed-but-not-conclusive negative result**: four cycle-36-listed G0 candidate failure modes RULED OUT (F1 malformed XBE header; F2 TLS-size crash; F3 kernel-import mismatch; F4 dead-code-eliminated cycle-35 slots); one new structural drift surfaced (F5 +1 unexplained `.CRT$X*` walker group with a singleton fn-ptr at `.text 0x16720` in witness-only vs mirror); cycle-38 recommendation filed (re-link with lld `--print-map` to resolve the symbol).

## Rule #15 applicability (cycle 37)

Cycle 37 is ANALYSIS-ONLY / DOC-ONLY:

- ZERO `scripts/apple-silicon/xbe-tests/witness-only/main.c` / `Makefile` / `manifest.json` / `README.md` edits.
- ZERO `scripts/apple-silicon/xbe-tests/lib/xbed_*` edits.
- ZERO `scripts/apple-silicon/xbe-tests/lib/lib.mk` edits.
- ZERO `scripts/apple-silicon/xbe-tests/oracle-agent/` edits.
- ZERO `scripts/apple-silicon/composite-record.sh` / `composite-preflight.sh` edits.
- ZERO XBE rebuilds.
- ZERO host xemu source (`hw/`, `ui/`, `target/`, `include/`) touched.
- ZERO `nxdk/` source touched (READ `nxdk/lib/pdclib/platform/xbox/crt_initializers.c` for CRT-walker logic + enumerated per-obj `.CRT$X*` subsection contributors via objdump as read-only inspection only).
- ZERO `tools/xemu-capture/` source touched.

Analysis/doc-only carve-out APPLIES — the slice's only writes are (i) canonical docs/state (handoff.md + decision-log.md + orchestration-state quartet) and (ii) gitignored evidence artifacts under `benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/`. **Codex SKIPPED with explicit justification** (the validated artifact is the cycle-35 binary; cycle-35 Codex green at closure `515e03f4e7` covers it).

## Gate status (cycle 37)

- [x] Required docs read.
- [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward through cycles 35 / 36 / 37).
- [x] Comparison-target justification recorded (pipeline-smoke + mirror; both known-good on real Xbox per `.claude/rules/oracle-and-xbe.md`).
- [x] Purpose-built XBE parser (`xbe_parse.py`) cross-checked against Caustik's XBE spec + nxdk `tools/cxbe`.
- [x] All three XBEs parsed into structured JSON (`01-xbe-headers.json`).
- [x] XBE header / section / TLS / kernel-import / library-version diff covered.
- [x] `.CRT$X*` walker arrays located + decoded in `.rdata` for all three XBEs (`02-crt-region-dump.txt`).
- [x] F1..F6 findings recorded in SUMMARY.md + handoff.md cycle-37 entry + decision-log.md cycle-37 entry.
- [x] Cycle-38 recommendation filed (lld `--print-map` to resolve `.text 0x16720` symbol).
- [x] Orchestration-state quartet (this file + current-cycle.md + claude-status.md + handoff-summary.md) closure pass.
- [-] Codex SKIPPED per analysis/doc-only carve-out (explicit justification recorded).
- [ ] Closure commit on `apple-silicon-performance` (pending — committed last in this session).

## Static-diff evidence

- **XBE header structure (F1).** Pipeline-smoke / mirror / witness-only all share: `base_vaddr=0x10000`, `size_of_headers=376`, `num_sections=4` (`.text` / `.rdata` / `.data` / `.tls` in that order), `init_flags=0x05`, `pe_stack_commit=65 536`, `pe_heap_reserve=1 048 576`, library count=1, entry-point key=retail. Bitwise-identical section flag bitmaps per section. Witness-only is NOT malformed at the XBE-header level.
- **TLS layout (F2).** Per-XBE `.tls` section: vsize=276, raw_size=4. TLS directory: `size_of_zero_fill=0`, `tls_callback_va=0x00000000` (no TLS callback functions), `raw_data_end - raw_data_start = 0x110` bytes. ALL THREE XBEs identical. RULES OUT sub-case (a)'s "TLS-size computation crash" candidate.
- **Kernel imports (F3).** Witness-only = 78 ordinals; mirror = same 78 ordinals (set-diff empty); pipeline-smoke = 72-ordinal subset (the 6-ordinal delta {47, 100, 137, 156, 168, 173} is fully accounted for by `lib/lib.mk`). NO kernel function imported by witness-only is missing from mirror. RULES OUT "xbed_self_witness.c introduces a new kernel call that crashes pre-main".
- **Cycle-35 slot presence (F4).** `strings -a -t x` recovers both cycle-35 host-log strings at file offsets `0x22237` (`.CRT$XXC` slot) + `0x221f9` (`.CRT$XCU` slot). `objdump -h main.obj` confirms `.CRT$XXC` (4 bytes = 1 fn ptr) + `.CRT$XCU` (4 bytes = 1 fn ptr) sections exist. `nm main.obj` confirms helper-function bodies + slot variables: `_s_witness_only_pre_main_crt_xc_slot`, `_s_witness_only_pre_main_crt_xx_slot`, `_witness_only_pre_main_crt_xc`, `_witness_only_pre_main_crt_xx`. RULES OUT "slots got dead-code-eliminated".
- **`.CRT$X*` walker layout (F5, ONLY structural drift).** Pipeline-smoke and mirror each have 4 walker groups (`[1, 2, 1, 2]` fn-ptrs; 6 fn-ptrs total). Witness-only has 5 walker groups (`[1, 1, 3, 1, 2]` fn-ptrs; 8 fn-ptrs total). Net delta witness-only vs mirror: +1 walker group AND +2 fn-ptrs (the +2 matches cycle 35's 2 new slots cleanly). The +1 EXTRA WALKER GROUP is NOT directly accounted for by cycle 35; the singleton fn-ptr in that group points at `.text 0x16720`. Candidate sources (UNVERIFIED in this slice): nxdk `profiling.obj` / `fiber.obj` `.CRT$XXT` contributors; `automount_d.obj` `.CRT$XIT` contributor; `xbed_self_witness.c` symbol reference dragging one of these in.
- **Real-Xbox-only divergence (F6).** Cycle-35 closure commit (`515e03f4e7`) records local-xemu smoke validation passed (all 4 expected WTNS fires in correct order under `XEMU_GUEST_LOG=1`). XBEs have no load-time relocations so bytes-in-memory are identical between xemu and real Xbox. The crash is in either (i) nxdk's pre-`.CRT$XX*` startup code (`_start` / `__security_init_cookie` / TLS setup / `_PDCLIB_xbox_libc_init`) reacting differently to real-Xbox BIOS / kernel context but tolerated by xemu, OR (ii) `MmAllocateContiguousMemoryEx`'s real-Xbox behavior diverging from xemu's emulation (well-known surface — xemu's MmGetPhysicalAddress emulation cannot reproduce every real-Xbox failure mode), OR (iii) a not-yet-identified symbol at `.text 0x16720` in witness-only's extra CRT walker group.

## Why this is not a regression of any prior cycle's validation guarantees

Cycle 23 / 25 / 27 / 29 / 31 / 33 / 35 each Codex-validated their own implementation slices. Cycle 37 does not touch any of those slices' code: `lib/xbed_a4_witness.{c,h}` intact, `lib/xbed_self_witness.{c,h}` intact, `oracle-agent/*` intact, `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit intact, `nxdk/` intact (READ-only), `tools/xemu-capture/` intact, `scripts/apple-silicon/composite-record.sh` + `composite-preflight.sh` intact, `witness-only/main.c` + `Makefile` + `README.md` + `manifest.json` intact. Cycle 37 also does not change any flag default, M15 visual-gate prerequisite, or other shipping behavior; it only narrows the cycle-22 pre-main-crash hypothesis state.

## Evidence integrity

- Static-diff evidence captured under `benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/` (gitignored per project convention): `xbe_parse.py` (purpose-built parser, cross-checked against XBE spec); `01-xbe-headers.json` (full parsed headers + sections + imports + TLS for all three XBEs); `02-crt-region-dump.txt` (raw 48-dword decode of the CRT walker region for all three); `03-evidence.txt` (cycle-35 strings + main.obj sections + main.obj symbol table + nxdk per-obj `.CRT$X*` contributor enumeration); `SUMMARY.md` (full findings document with mission, methodology, comparison-target justification, F1..F6 findings, exit-criteria status, verdict + cycle-38 recommendation, out-of-scope list).

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing inside xemu.
- No shared-lib edits; no oracle-agent edits; no `lib.mk` edits.
- No `witness-only/main.c` / Makefile / README.md / manifest.json edits.
- No nxdk source edits (READ-only inspection of `crt_initializers.c` + per-obj `.CRT$X*` enumeration only).
- No `tools/xemu-capture/` source edits.
- No `composite-record.sh` / `composite-preflight.sh` source edits.
- No retail-title / §G.5 / RT-as-texture work.
- No real-Xbox run; no XBE rebuild.
- No cycle-38+ map-file regeneration / pre-`.CRT$XXC` discriminator work (Hermes's call).
- No cleanup of pre-existing untracked `.hermes_*` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34 prompt guardrail).
