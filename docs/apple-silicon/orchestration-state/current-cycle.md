# Current Cycle

- Cycle: 36 real-Xbox discriminator run for cycle-35 pre-main breadcrumb — **CLOSED on `apple-silicon-performance`**. Bounded run-only slice: deploy cycle-35 `witness-only/bin/default.xbe` (155 648 B, SHA-256 `ab52df8dee...`) via FTP `--overwrite` to `/E/Apps/witness-only/default.xbe`, run the cycle-36 canonical sequence (composite-capture leg + runxbe + final `witness.scan-self` for G-row classification), and classify the outcome against the cycle-35 G-row discriminator table.
- Started: 2026-05-23 22:53:06Z (Hermes-supervised bounded session; Claude Code worker run launched after cycle-35 closure commit `515e03f4e7`).
- Closed: 2026-05-23 (G-row classified; canonical docs/state synced; closure commit `265010549f` landed on `apple-silicon-performance`).
- State: **CLOSED — OUTCOME G0** (zero stripes + `witness.scan-self count=0` + `witness.scan = D-cycle-27`). REPRODUCED across two runxbe attempts in same physical power session.
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: cycle-35 closure commit `515e03f4e7` on `apple-silicon-performance`.
- Bounded goal: "Execute cycle 36 only — the real-Xbox discriminator run for the cycle-35 witness-only pre-main breadcrumb build. Use the canonical runbook from `witness-only/README.md` cycle-35 addendum. ZERO source/script/XBE edits expected. Classify the outcome per the G-row table; sync canonical docs/state; commit cleanly."
- Result: **G0** — the crash occurred BEFORE the `.CRT$XXC` slot (stage=4) fired. Three pre-`.CRT$XXC`-fire sub-cases share this shape and cannot be distinguished by cycle-35 evidence on real Xbox (no host-log breadcrumb): (a) crash in `_start`/`__security_init_cookie`/TLS/`_PDCLIB_xbox_libc_init`; (b) walker invoked but helper body crashed before `MmAllocateContiguousMemoryEx`; (c) `MmAllocateContiguousMemoryEx` returned NULL silently. γ.1 ("`XVideoSetMode` faulted") INVALIDATED. Cycle-22 pre-main hypothesis NARROWED FURTHER beyond cycle-34's F4 to pre-`.CRT$XXC` window. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-37+ pre-`.CRT$XXC` discriminator work + downstream cycles).

## Plan summary (this session, executed in order)

1. Read canonical docs/state (CLAUDE.md, orchestration-workflow.md, handoff cycle-35+34, decision-log cycle-35+34, orchestration-state quartet, witness-only/README.md cycle-35 addendum, witness-only/manifest.json).
2. Inspected git status + recent commits (HEAD = cycle-35 closure `515e03f4e7`).
3. Probed reachability: ping=true ftp=false agent=true (cycle-29 agent resident from cycle 34).
4. Baseline scans: `witness.scan = D-cycle-27` AND `witness.scan-self = count=0` — preconditions MET.
5. Reboot to dashboard; FTP-uploaded cycle-35 `default.xbe` with `--overwrite` (mtime advanced confirms file replaced).
6. Ensure-agent + recheck both scans (still MET).
7. Composite preflight ok (xemu-capture, 1.591 s); armed `composite-record.sh --duration 80`.
8. composite-record.sh ffmpeg SILENT-STALLED for 103 s (`rc=137 capture_timed_out=true`); cycle-34 finding (i) reproduced in a SEPARATE physical power session.
9. First `runxbe` issued 23:02:38Z; dashboard FTP back 23:03:16Z = t+38s (clean recovery).
10. Final scans (first run): D-cycle-27 + count=0 — already enough to land G0.
11. Pre-runxbe xemu-capture snap_00 verification: 720x480 RGB, 25 874 unique colors (capture path healthy).
12. Second `runxbe` issued 23:06:43Z + 25-snap burst over t+0..t+29.5s with explicit `--width 720 --height 480` (cycle-34-style snapshot-burst substitution).
13. Stripe analysis: ZERO stripe colors detected across all 26 snaps (13 pure-black, 13 dashboard transition/return).
14. Final scans (second run): D-cycle-27 + count=0 — REPRODUCED.
15. SUMMARY.md written to run dir; canonical docs/state updates (handoff.md + decision-log.md + orchestration-state quartet) landed as commit `265010549f`.
16. Closure commit on `apple-silicon-performance` LANDED as `265010549f`; bounded doc-sync follow-up commit on top syncs orchestration-state quartet to reference the landed hash.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward through cycles 35 + 36).
3. [x] Canonical cycle-36 runbook from `witness-only/README.md` executed verbatim (with cycle-34 snapshot-burst fallback when composite-record.sh ffmpeg silent-stalled).
4. [x] Cycle-35 XBE FTP-uploaded with `--overwrite` (size matches cycle 31's; the uploader's default size-only diff would otherwise have skipped); remote mtime advance verifies replacement.
5. [x] ARM composite-capture leg ran before runxbe (preflight OK + ffmpeg launched; subsequent silent-stall is the cycle-34-finding-(i) failure mode, not a procedural failure of cycle 36).
6. [x] Two runxbe attempts produced REPRODUCED scans = (D-cycle-27, count=0).
7. [x] 26-snap NTSC composite burst over t+0..t+29.5s captured + classified: ZERO stripes detected.
8. [x] G-row classification = **G0**; rationale + three pre-`.CRT$XXC` sub-cases documented; γ.1 ruled out.
9. [x] SUMMARY.md written.
10. [x] `handoff.md` + `decision-log.md` cycle-36 entries on top; cycle-35 entries preserved unchanged below.
11. [x] Orchestration-state quartet closure pass (this file + claude-status.md + validation-status.md + handoff-summary.md).
12. [-] Codex SKIPPED per rule #15 doc-only / run-only carve-out (same path as cycles 26/28/30/32/34); cycle-35 marker `515e03f4e7` remains relevant for the deployed artifact.
13. [x] Closure commit landed as `265010549f` on `apple-silicon-performance`.

## Out-of-scope (kept bounded for cycle 36)

- NO host xemu source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact).
- NO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact).
- NO `lib/lib.mk` touched.
- NO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact).
- NO `xbed_runtime.{c,h}` touched.
- NO image-blit touched.
- NO `witness-only/main.c` touched (cycle-35 source intact).
- NO `nxdk/` source touched.
- NO `tools/xemu-capture/` source touched.
- NO `scripts/apple-silicon/composite-record.sh` / `composite-preflight.sh` source touched (cycle-33 implementations intact; cycle-36 reproduction of cycle-34 finding (i) filed; cycle-37+ fix scope).
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO XBE rebuilds (cycle-35 build observed; bit-identical to closure `515e03f4e7`).
- NO PushNotification — bounded run-only slice, not blocker / milestone (G0 outcome is informative but does not unblock M15; it narrows the cycle-37+ scope window).
- NO cleanup of pre-existing untracked `.hermes_*` files at repo root.
- NO touch of pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` / `retail-gameplay-oracle.py` / `retail-oracle-workflow.py` / `retail-title-automation-proof.py` (preserved per cycle-34 prompt guardrail; carried forward).
- NO touch of pre-existing untracked `scripts/apple-silicon/composite_preflight.py` (preserved per the same guardrail; carried forward).

## Recommended cycle-37+ scope (NOT executed this session — Hermes's call)

Cycle 36 G0 narrowed the crash window to one of three pre-`.CRT$XXC`-fire sub-cases that cycle-35 evidence cannot distinguish. The cycle-35 README G0-row "Next" column enumerates three candidates; pick one or combine:

1. **Custom XBE-header callback that runs before nxdk's `_start`** — requires modifying nxdk's `tools/cxbe/` XBE-header generator to expose a kernel-controlled entry slot; widens scope across nxdk but uniquely distinguishes sub-case (a) (`_start`/`__security_init_cookie`/TLS/`_PDCLIB_xbox_libc_init` crash).
2. **Static binary diff against a known-good nxdk XBE** (`pipeline-smoke` or `mirror` — both boot and paint successfully on real Xbox per cycles 26/28/30/32/34) to localize the `xbed_self_witness.c` / `xbed_a4_witness.c` / `lib/xbed_runtime.c` / pre-main-breadcrumb code differences; ZERO new XBE source required; could surface a stack alignment / TLS-layout / section-attribute drift that pre-dates `.CRT$XXC` walker invocation.
3. **EEPROM-scratchpad write inside `xbed_self_witness_fire`** BEFORE the `MmAllocateContiguousMemoryEx` call, using the agent's `unsafe.enable` + EEPROM-write path; a successful EEPROM tick would discriminate sub-case (c) (`MmAllocateContiguousMemoryEx` returned NULL) from (a)/(b); adds substantial scope (new shared-lib API + EEPROM transactional commit + agent verb).

Secondary findings worth queuing for cycle 37+ (NOT cycle-36 fix scope): (i) cycle-34 finding upgraded — composite-record.sh ffmpeg silent-stall reproduced across two physical power sessions; cycle 37 candidate is to extend composite-preflight.sh with `--require-both-detectors` OR have composite-record.sh always run a brief ffmpeg liveness check before arming the full duration. (ii) cycle-34 secondary finding (ii) — xemu-capture snapshot defaults to 720x576 PAL — remains open; cycle-35 README cycle-36 runbook now includes explicit `--width 720 --height 480` in the example invocation as a partial mitigation.
