# Current Cycle

- Cycle: 37 static XBE binary diff — `witness-only` vs `pipeline-smoke` + `mirror` — **CLOSED on `apple-silicon-performance`**. Bounded analysis-only slice: parse the cycle-35 `witness-only/bin/default.xbe` (155 648 B, SHA-256 `ab52df8dee...`) alongside two known-good comparison targets; diff XBE-format headers / sections / TLS / kernel-thunk imports / `.CRT$X*` walker arrays; surface the cycle-37 narrowing of the cycle-36 G0 sub-case set; file the cycle-38 recommendation. ZERO XBE rebuilds; ZERO source/script/nxdk/host xemu edits; ZERO real-Xbox run; ZERO Codex (analysis/doc-only per the cycle-37 prompt's carve-out).
- Started: 2026-05-23 23:28:28Z (Hermes-supervised bounded session; Claude Code worker run launched after cycle-36 closure-sync commit `040e1d97d8`).
- Closed: 2026-05-23 (3 diff axes covered; F1..F6 findings recorded; cycle-38 recommendation filed; canonical docs/state synced).
- State: **CLOSED — narrowed-but-not-conclusive negative result.** Four cycle-36-listed G0 candidate failure modes RULED OUT (malformed XBE header / TLS-size crash / kernel-import surface mismatch / dead-code-eliminated cycle-35 slots); one new structural drift surfaced (+1 unexplained `.CRT$X*` walker group with a singleton fn-ptr at `.text 0x16720` in witness-only vs mirror); cycle-38 next step recommended (re-link with lld `--print-map` and resolve the symbol at that address).
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: cycle-36 closure-sync commit `040e1d97d8` on `apple-silicon-performance`.
- Bounded goal: "execute the lowest-risk highest-value G0 follow-up from the cycle-36 closeout by performing a static binary-diff / structural-comparison investigation between the failing cycle-35 `witness-only` XBE and at least one known-good nxdk XBE that boots and paints on real Xbox (`pipeline-smoke` and/or `mirror`); evidence over speculation; do not modify nxdk / host xemu / shared witness libs; analysis/doc-only Codex carve-out applies."
- Result: **narrowed-but-not-conclusive negative result.** Live G0 candidate set tightened from "any pre-`.CRT$XXC` window" to a smaller candidate set: (i) nxdk's pre-`.CRT$XX*` startup code (`_start` / `__security_init_cookie` / TLS setup / `_PDCLIB_xbox_libc_init`) reacting differently on real Xbox vs xemu; (ii) `MmAllocateContiguousMemoryEx` returning NULL silently on real Xbox from the `.CRT$XXC` slot context (sub-case c); (iii) a not-yet-identified symbol at `.text 0x16720` in witness-only's extra CRT walker group faulting on real Xbox. Cycle-22 pre-main-crash hypothesis FULLY CORROBORATED (per cycle 34) and FURTHER STRENGTHENED with three concrete narrower sub-hypotheses replacing the earlier enumeration of G0 sub-cases (a)/(b)/(c).

## Plan summary (this session, executed in order)

1. Read canonical docs/state (CLAUDE.md, handoff.md cycle-36+35, decision-log.md cycle-36+35, orchestration-state quartet, `xbe-tests/witness-only/manifest.json` + cycle-35 addendum, `pipeline-smoke/manifest.json`, `mirror/manifest.json`, `lib/lib.mk`).
2. Inspected git status + recent commits (HEAD = cycle-36 closure-sync `040e1d97d8`); confirmed pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` + 13 untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files + untracked `composite_preflight.py` to preserve unstaged.
3. Justified comparison-target selection: pipeline-smoke (bare nxdk known-good) + mirror (lib-stack known-good — strictest comparison since source-side delta to witness-only is minimal).
4. Built purpose-built Python XBE parser (`benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/xbe_parse.py`; field offsets cross-checked against Caustik's XBE spec + nxdk `tools/cxbe`; satisfies project rule #5).
5. Parsed all three XBEs into structured JSON (`01-xbe-headers.json`).
6. F1: tabulated XBE header / section / library / init-flag / entry-point key across all three; confirmed witness-only is well-formed at the XBE-header level (identical to mirror).
7. F2: tabulated TLS layout across all three; confirmed bit-identical 276-vsize / 4-raw / NULL-callback / 0-zerofill / 0x110-datarange layout; RULED OUT sub-case (a)'s "TLS-size computation crash" candidate.
8. F3: decoded kernel thunk-table with debug XOR key `0xEFB1F152` / retail key `0x5B6D40B6`; set-diff'd kernel-import ordinal sets across all three; confirmed witness-only ≡ mirror (78 ordinals identical); RULED OUT "`xbed_self_witness.c` introduces a new kernel call that crashes pre-main".
9. F4: confirmed cycle-35 slots physically present in binary (host-log strings + `.CRT$XXC` / `.CRT$XCU` sections in main.obj + helper-function symbols); RULED OUT "cycle-35 slots got dead-code-eliminated".
10. F5: located the CRT walker region in `.rdata` of all three XBEs; dumped 48 dwords per XBE (`02-crt-region-dump.txt`); tabulated walker-group counts (`[1, 2, 1, 2]` for pipeline-smoke and mirror; `[1, 1, 3, 1, 2]` for witness-only); SURFACED the +1 extra walker group with singleton fn-ptr at `.text 0x16720` as the only structural drift; enumerated candidate sources (`profiling.obj` / `fiber.obj` `.CRT$XXT`; `automount_d.obj` `.CRT$XIT`).
11. F6: cross-referenced cycle-35 closure commit local-xemu smoke transcript; confirmed cycle-35 binary DOES boot on xemu; G0 crash is REAL-XBOX-ONLY; static XBE bytes load identically on both hosts (no load-time relocations); divergence is in either nxdk's pre-`.CRT$XX*` startup or `Mm*` kernel-call edge-case.
12. Filed cycle-38 recommendation: re-link with lld `--print-map` to resolve the `.text 0x16720` symbol — tooling-only change, no source edits, no rebuild of any shared lib.
13. Saved evidence artifacts to `benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/` (gitignored): `xbe_parse.py`, `01-xbe-headers.json`, `02-crt-region-dump.txt`, `03-evidence.txt`, `SUMMARY.md`.
14. Updated canonical docs/state: `handoff.md` cycle-37 entry on top; `decision-log.md` cycle-37 entry above cycle-36; orchestration-state quartet (this file + claude-status.md + validation-status.md + handoff-summary.md) synced to cycle-37 closure.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward through cycles 35 / 36 / 37).
3. [x] Bounded static-diff investigation actually performed and evidenced (3 diff axes; 6 numbered findings; full SUMMARY.md).
4. [x] Canonical docs/state updated with the result and next-step recommendation.
5. [x] `git status` clean except intended doc edits + known pre-existing drift / untracked files.
6. [x] Commit slice on `apple-silicon-performance` (canonical doc updates).
7. [-] Codex SKIPPED per analysis/doc-only carve-out (rule #15 not triggered — no non-trivial code change).

## Out-of-scope (kept bounded for cycle 37)

- NO host xemu source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact).
- NO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact).
- NO `lib/lib.mk` touched.
- NO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact).
- NO `xbed_runtime.{c,h}` touched.
- NO image-blit touched.
- NO `witness-only/main.c` / `Makefile` / `manifest.json` / `README.md` touched.
- NO `nxdk/` source touched (parser READ nxdk/lib/pdclib/platform/xbox/crt_initializers.c + per-obj CRT-subsection enumeration via objdump, but did NOT modify).
- NO `tools/xemu-capture/` source touched.
- NO `scripts/apple-silicon/composite-record.sh` / `composite-preflight.sh` source touched.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO XBE rebuilds (cycle-35 build observed; bit-identical to closure `515e03f4e7`).
- NO real-Xbox run.
- NO PushNotification — bounded analysis slice, not a milestone (cycle-37 narrowing is informative but does not unblock M15; it tightens the cycle-38+ scope window).
- NO cleanup of pre-existing untracked `.hermes_*` / `composite_preflight.py` files at repo root.
- NO touch of pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` / `retail-gameplay-oracle.py` / `retail-oracle-workflow.py` / `retail-title-automation-proof.py` (preserved per cycle-34 prompt guardrail; carried forward).

## Recommended cycle-38+ scope (NOT executed this session — Hermes's call)

**Primary recommendation (cycle 38, cheapest, highest-information).** Re-link the existing witness-only build with lld's `--print-map` output (`-Wl,-Map=witness-only.map`) and resolve which symbol corresponds to the `.text 0x16720` singleton fn-ptr in witness-only's extra CRT walker group. Tooling-only change (one extra linker flag); no source edits, no slot additions, no rebuild of any shared lib. Either eliminates the +1 walker-group hypothesis (benign nxdk-default initializer — file the structural difference, deprioritize) OR identifies a non-trivial pre-main code path unique to witness-only that becomes the next instrumentation target. If the latter, cycle-39 can place an `out 0xe9` host-log breadcrumb inside that symbol's entry to discriminate "this path executes on xemu but faults on real Xbox" via standalone xemu validation.

**Fallback (cycle 39, if map-file analysis is negative).** Cycle-36 Option C: EEPROM scratchpad write inside `xbed_self_witness_fire` BEFORE `MmAllocateContiguousMemoryEx`, using the agent's `unsafe.enable` + EEPROM-write path. A successful EEPROM tick would uniquely discriminate sub-case (c) (`MmAllocateContiguousMemoryEx` returned NULL silently) from sub-cases (a) and (b). Adds substantial scope (new shared-lib API + EEPROM transactional commit + agent verb) and should only be funded after cycle 38's tooling-only answer.

**Deprioritized (per cycle 35 + cycle 37 evidence).** Custom XBE-header callback before nxdk's `_start` (option 1 from the cycle-36 closeout) remains deprioritized — would require modifying nxdk itself; γ.0 sub-windows it could uniquely distinguish are vanishingly unlikely cycle-22 hang sites given F1/F2 ruled out the most plausible nxdk-pre-startup failure modes.

Secondary findings carried forward from cycle 36 (NOT cycle-37 fix scope): (i) composite-record.sh ffmpeg silent-stall reproduced across two physical power sessions; cycle 38+ candidate is to extend composite-preflight.sh with `--require-both-detectors` OR have composite-record.sh always run a brief ffmpeg liveness check before arming the full duration. (ii) xemu-capture snapshot defaults to 720x576 PAL — cycle-35 README cycle-36 runbook already includes explicit `--width 720 --height 480` in the example invocation as a partial mitigation.
