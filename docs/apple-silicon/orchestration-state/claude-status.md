# Claude Status

- Objective: cycle 37 static XBE binary diff — `witness-only` vs `pipeline-smoke` + `mirror` — bounded analysis-only investigation of the cycle-36 G0 outcome's three indistinguishable sub-cases via structural comparison against two known-good nxdk XBEs that boot on real Xbox. Aim: surface concrete pre-`.CRT$XXC`-relevant differences that could plausibly explain the G0 crash, narrow the live candidate set, recommend a bounded cycle-38 next step.
- Status: **CLOSED. Narrowed-but-not-conclusive negative result.** Four cycle-36-listed G0 candidate failure modes RULED OUT (malformed XBE header / TLS-size crash / kernel-import surface mismatch / dead-code-eliminated cycle-35 slots); one new structural drift surfaced (+1 unexplained CRT walker group with a singleton fn-ptr at `.text 0x16720` in witness-only vs mirror). SUMMARY.md written. handoff.md + decision-log.md cycle-37 entries on top with cycle-36 + cycle-35 preserved unchanged below. Codex SKIPPED per analysis/doc-only carve-out.

## Why cycle 37 ran this session

Cycle 36 closure (commit `265010549f` + sync `040e1d97d8`) recorded OUTCOME G0 with three pre-`.CRT$XXC`-fire sub-cases — (a) `_start` / `__security_init_cookie` / TLS / `_PDCLIB_xbox_libc_init` crash; (b) walker invoked but helper body crashed before `MmAllocateContiguousMemoryEx`; (c) `MmAllocateContiguousMemoryEx` returned NULL silently — that cycle-35 evidence cannot distinguish on real Xbox. Cycle 36's closeout enumerated three cycle-37+ candidates: (1) custom XBE-header callback that runs before nxdk's `_start`; (2) static binary diff against a known-good nxdk XBE; (3) EEPROM scratchpad write inside `xbed_self_witness_fire` before `MmAllocateContiguousMemoryEx`. Cycle 37 executes candidate (2) as a bounded analysis-only slice (lowest cost; no source / nxdk / host xemu / lib edits; preserves pre-existing tracked drift).

## What this session shipped

1. **Comparison-target justification.** Two known-good targets: `pipeline-smoke` (Phase 3.0 Tier-4; pure nxdk; no `lib/lib.mk`; PNG SHA-256 matches `expected.py:default()` byte-for-byte per `.claude/rules/oracle-and-xbe.md`) AND `mirror` (Phase 3.1 Tier-1; uses `lib/lib.mk` — same shared runtime as witness-only; passed xemu-Metal vs math-derived oracle and boot-functional in prior oracle-pipeline runs). Mirror is the strictest comparison since the source-side delta to witness-only is just main.c text + `xbed_self_witness.c` opt-in + the two cycle-35 `.CRT$X*` slots.
2. **Purpose-built XBE parser.** Wrote `benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/xbe_parse.py` (Python 3, ~200 LOC). Field offsets cross-checked against Caustik's XBE specification + nxdk `tools/cxbe` source (satisfies project rule #5 "build tools rather than substitute weaker evidence"). Decodes XBE header, certificate, sections, kernel-thunk table (with debug XOR key `0xEFB1F152` / retail key `0x5B6D40B6`), TLS directory, library-version array.
3. **Three diff axes executed.** Axis 1: XBE header / section / TLS structure — all three XBEs share identical structural layout (4 sections in same order, same flags, same TLS callback=NULL / zerofill=0 / 0x110 data range). Axis 2: kernel-thunk imports — witness-only ≡ mirror (78 ordinals; same set); pipeline-smoke is a 72-ordinal subset; the 6-ordinal delta is fully accounted for by `lib/lib.mk`. Axis 3: `.CRT$X*` walker arrays in `.rdata` — pipeline-smoke and mirror each have 4 walker groups (`[1, 2, 1, 2]` fn-ptrs); witness-only has 5 walker groups (`[1, 1, 3, 1, 2]` fn-ptrs).
4. **Six findings filed (F1..F6).** F1: witness-only is well-formed at the XBE-header level. F2: TLS layout is bit-identical to mirror — RULES OUT sub-case (a)'s "TLS-size computation crash". F3: kernel-import surface witness-only ≡ mirror — RULES OUT "xbed_self_witness.c introduces a new kernel call" hypothesis (all four `Mm*` calls already imported and used by mirror's `xbed_runtime.c`). F4: cycle-35 slots physically present in binary (strings + main.obj sections + main.obj symbol table) — RULES OUT "slots got dead-code-eliminated". F5 (ONLY structural drift): +1 walker group + +2 fn-ptrs in another group vs mirror; +2 matches cycle 35's 2 new slots cleanly, +1 EXTRA walker group is the new narrowing. F6: cycle-35 binary boots on xemu per cycle-35 closure smoke transcript; G0 is real-Xbox-only; static bytes load identically on both hosts (no relocations); divergence is in either nxdk pre-`.CRT$XX*` startup OR `MmAllocateContiguousMemoryEx` real-Xbox edge case.
5. **Evidence on disk.** `benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/{xbe_parse.py, 01-xbe-headers.json, 02-crt-region-dump.txt, 03-evidence.txt, SUMMARY.md}` (gitignored per project convention).
6. **Cycle-38 recommendation filed.** Lowest-cost highest-information next step: re-link existing witness-only build with lld's `--print-map` output (`-Wl,-Map=witness-only.map`) and resolve which symbol corresponds to the `.text 0x16720` singleton fn-ptr. Tooling-only change; no source edits, no slot additions, no rebuild of any shared lib. Either eliminates the +1 walker-group hypothesis (benign nxdk-default initializer) OR identifies the next instrumentation target. If negative, cycle-39 fallback is cycle-36 Option C (EEPROM scratchpad write).
7. **Canonical docs/state synced.** handoff.md + decision-log.md cycle-37 entries on top with cycle-36 + cycle-35 preserved unchanged below; orchestration-state quartet (this file + current-cycle.md + validation-status.md + handoff-summary.md) closure pass complete.

## Session progress

- [x] Read required docs/state (CLAUDE.md, handoff.md cycle-36+35, decision-log.md cycle-36+35, orchestration-state quartet, witness-only/manifest.json + cycle-35 addendum, pipeline-smoke/manifest.json, mirror/manifest.json, lib/lib.mk).
- [x] Confirmed git state + pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward through cycles 35 / 36 / 37).
- [x] Justified comparison-target selection (pipeline-smoke + mirror).
- [x] Built XBE parser; cross-checked field offsets against Caustik's spec + nxdk cxbe.
- [x] Parsed all three XBEs into structured JSON; tabulated header / TLS / section / library / init-flag / entry-point key.
- [x] Decoded kernel-thunk table for all three; set-diff'd imports.
- [x] Located + dumped CRT walker region in `.rdata` of all three XBEs.
- [x] Filed F1..F6 findings + cycle-38 recommendation.
- [x] SUMMARY.md written; evidence artifacts saved under gitignored benchmark-runs dir.
- [x] handoff.md + decision-log.md cycle-37 entries on top with cycle-36 + cycle-35 preserved unchanged below.
- [x] Orchestration-state quartet closure pass.
- [x] Codex SKIPPED per analysis/doc-only carve-out (rule #15 not triggered — no non-trivial code change).
- [ ] Closure commit on `apple-silicon-performance` (pending — committed last in this session before exit).

## Confidence + risk notes

- **HIGH confidence in F1..F4 RULE-OUTs.** Each finding is grounded in bit-identical structural comparison between witness-only and mirror (mirror confirmed boot-functional on real Xbox; witness-only's structural surface in those axes is identical). The four ruled-out failure modes were the most plausible candidates the cycle-36 closeout listed; their elimination genuinely narrows the cycle-38+ scope.
- **MEDIUM confidence in F5 +1 extra walker group interpretation.** The structural drift is real (5 walker groups vs 4 in pipeline-smoke / mirror); the candidate-source enumeration (profiling.obj / fiber.obj / automount_d.obj CRT subsection contributors) is plausible but UNVERIFIED in this slice. The cycle-38 map-file recommendation is the cheapest way to resolve it.
- **HIGH confidence in F6 real-Xbox-only divergence.** Cycle-35 closure transcript confirmed local-xemu smoke validation passed; XBEs have no load-time relocations so bytes-in-memory are identical between hosts. The crash is in either nxdk pre-`.CRT$XX*` startup OR Mm* kernel-call edge case OR the unknown `.text 0x16720` symbol.
- **LOW risk to cycle-23 / cycle-27 / cycle-29 / cycle-31 / cycle-33 / cycle-35 prior guarantees.** ZERO source touched in any of those slices; their Codex validations remain in force.
- **MEDIUM confidence in cycle-38 recommendation prioritization.** Map-file analysis is genuinely cheapest (tooling-only; no source / shared-lib edit; one re-link); EEPROM-scratchpad fallback is genuinely most expensive (new shared-lib API + EEPROM transactional commit + agent verb). Sequencing is sound but Hermes may have project-priority reasons to skip cycle 38 and go straight to cycle-39 fallback.

## What this session does NOT do

- NO host xemu source touched.
- NO `lib/xbed_*` / `oracle-agent/*` / `witness-only/main.c` / `witness-only/Makefile` / `witness-only/manifest.json` / `witness-only/README.md` touched.
- NO `nxdk/` source touched (READ nxdk/lib/pdclib/platform/xbox/crt_initializers.c + enumerated per-obj `.CRT$X*` subsection contributors via objdump as read-only inspection, but did NOT modify).
- NO `tools/xemu-capture/` source touched.
- NO `composite-record.sh` / `composite-preflight.sh` source touched (cycle-33 implementations intact; cycle-36 cycle-34-finding-(i) reproduction filed as a secondary finding; cycle-38+ fix scope).
- NO XBE rebuilds (cycle-35 binary observed; bit-identical to cycle-35 closure commit `515e03f4e7`).
- NO real-Xbox run.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips; NO M15 movement; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` unchanged.
- NO PushNotification — bounded analysis slice, cycle-37 narrowing is informative but does not unblock M15.
- NO cleanup of `.hermes_*` files or pre-existing tracked drift / untracked `composite_preflight.py`.

## Next proposed action

Cycle 37 closes with the static-diff investigation landed. The substantive next slice is cycle 38 (Hermes's call): re-link witness-only with lld `--print-map` and resolve the `.text 0x16720` singleton fn-ptr symbol. Tooling-only change; no rebuild of any shared lib; no source edits. If negative, cycle-39 fallback is cycle-36 Option C (EEPROM scratchpad write).
