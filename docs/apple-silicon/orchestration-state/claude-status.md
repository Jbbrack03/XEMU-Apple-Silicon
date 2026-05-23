# Claude Status

- Objective: cycle 36 real-Xbox discriminator run for cycle-35 pre-main breadcrumb — execute the cycle-36 canonical runbook from `witness-only/README.md` cycle-35 addendum verbatim against the cycle-35 binary (155 648 B, SHA-256 `ab52df8dee...`) on the project's real Xbox; classify outcome per the cycle-35 G-row table; sync canonical docs/state.
- Status: **CLOSED. OUTCOME G0.** Reproduced across two runxbe attempts in same physical power session. SUMMARY.md written. handoff.md + decision-log.md cycle-36 entries on top; orchestration-state quartet closure pass complete. Codex SKIPPED per rule #15 doc-only / run-only carve-out. Closure commit pending.

## Why cycle 36 ran this session

Cycle 35 closure (commit `515e03f4e7`) shipped the option-(1) pre-main breadcrumb infrastructure (two `.CRT$X*` static-init slots that fire `xbed_self_witness_fire` BEFORE `main()` enters) and locally smoke-validated it on standalone xemu (slots fire, counter ticks 1→2 pre-main + 3→4 in-main). Cycle 36's bounded assignment was to deploy the cycle-35 build on real Xbox and read the `witness.scan-self reserved1` counter as the load-bearing γ.0-vs-γ.1 discriminator per the cycle-35 G-row table.

## What this session shipped

1. **Reachability + baseline.** `oracle-orchestrator.py status` = ping=true ftp=false agent=true (cycle-29 agent resident from cycle 34); baseline `witness.scan = D-cycle-27` (count=1, phys=0x03eb3000, reserved0=0, reserved1=0, mapped_pages_seen=419) AND baseline `witness.scan-self = count=0` — both preconditions MET.
2. **Cycle-35 XBE FTP-deployed.** `xbox-ftp-upload.py --overwrite` (REQUIRED because cycle-35 XBE size matches cycle-31's exactly — the uploader's default size-only diff would otherwise skip): SHA-256 `ab52df8dee32c24b857b3df749e3b8fc0a5a7e8f0949e06ec5c7e4d82aaef5bd`, 155 648 B; remote mtime advanced from `Dec 10 17:17` (cycle 31) to `Dec 11 02:20` (cycle 35) verifying replacement.
3. **First runxbe + final scans.** runxbe at 23:02:38Z; dashboard FTP back at 23:03:16Z (t+38s, clean recovery); final scans = (D-cycle-27, count=0) — already enough to land G0.
4. **Composite-record.sh ffmpeg silent-stall reproduced.** Preflight ok via xemu-capture in 1.591 s; ffmpeg launched per the printed command line; SILENT-STALLED for 103 s (`rc=137 capture_timed_out=true`, ZERO bytes stderr / video.mp4). Same pattern cycle 34 hit (filed as secondary finding (i)) — now reproduced across SEPARATE physical power sessions, upgrading the finding from "one-off" to "reproducible."
5. **Second runxbe + xemu-capture snapshot burst (cycle-34 fallback substitution).** Pre-runxbe snap_00 verified capture path healthy (25 874 unique colors dashboard). 25-snap burst with explicit `--width 720 --height 480` over t+0..t+29.5s; ALL 26 snaps analyzed (incl. snap_00); ZERO stripe colors detected; 13/26 pure-black; 13/26 dashboard transition/return frames (snap_20-22 carry 27k..40k unique colors confirming dashboard pixels). Final scans = (D-cycle-27, count=0) — REPRODUCED.
6. **G-row classification = G0.** Three signals (zero stripes + count=0 + D-cycle-27) match the G0 row of the cycle-35 G-table EXACTLY; G1/G2/G2'/G3/G4 all eliminated (require count >= 1). G0 means the crash occurred BEFORE the `.CRT$XXC` slot (stage=4) fired. Three pre-`.CRT$XXC` sub-cases share this shape and cannot be distinguished by cycle-35 evidence on real Xbox (no host-log breadcrumb): (a) `_start`/`__security_init_cookie`/TLS/`_PDCLIB_xbox_libc_init` crash; (b) walker invoked but helper body crashed before `MmAllocateContiguousMemoryEx`; (c) `MmAllocateContiguousMemoryEx` returned NULL silently.
7. **Hypothesis state advanced.** (γ) "main() never reaches" FURTHER STRENGTHENED. Cycle-22 pre-main hypothesis NARROWED beyond cycle-34's F4 to pre-`.CRT$XXC` window. **γ.1 INVALIDATED** (slot fires BEFORE `main()`; if main() entered then crashed at XVideoSetMode, the pre-main slot would have fired first and `count>=1`). (α) and (β) FURTHER DEPRIORITIZED. Hypothesis #5 UNCHANGED PARTIALLY INVALIDATED in catastrophic-hang sense (clean t+~26..38s dashboard recovery across both attempts).
8. **Reproducibility wins.** count=0 reproduced across both attempts; D-cycle-27 reproduced across 5 readbacks this session + ≥6 cycles (26/28/30/32/34/36); kernel-pool phys=0x03eb3000 reuse ≥20+ consecutive observations now across at least two physical power sessions (cycle 34 = fresh power; cycle 36 = same-power continuation); mapped_pages_seen=419 reproduced at every readback.
9. **Evidence on disk.** 16 logged steps + SUMMARY.md + composite-cycle36/ failed-recording dir + snapshots-runxbe2-ntsc/ 26-snap burst + stripe-analysis.json under `benchmark-runs/cycle36-real-xbox-witness-only-pre-main-discriminator-20260523T225306Z/` (gitignored per project convention).
10. **Canonical docs/state synced.** handoff.md + decision-log.md cycle-36 entries on top with cycle-35 entries preserved unchanged below; orchestration-state quartet (this file + current-cycle.md + validation-status.md + handoff-summary.md) closure pass complete.

## Session progress

- [x] Read required docs/state files.
- [x] Confirmed git state + pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward to cycle 36).
- [x] Reachability + baseline both scans MET.
- [x] Reboot Xbox + FTP-upload cycle-35 XBE with `--overwrite`; remote mtime advance confirms replacement.
- [x] ensure-agent + recheck scans still MET.
- [x] composite-preflight ok; composite-record.sh ffmpeg silent-stall reproduced (cycle-34 finding (i)).
- [x] First runxbe + dashboard recovery t+38s; final scans = (D-cycle-27, count=0).
- [x] Second runxbe + 25-snap NTSC burst; ZERO stripes detected; final scans = (D-cycle-27, count=0) REPRODUCED.
- [x] G-row classification = G0; rationale + three sub-cases + γ.1 invalidation documented.
- [x] SUMMARY.md written.
- [x] handoff.md + decision-log.md cycle-36 entries on top; cycle-35 entries preserved unchanged below.
- [x] Orchestration-state quartet closure pass.
- [x] Codex SKIPPED per rule #15 doc-only/run-only carve-out — justified by ZERO source/script/XBE edits this cycle.
- [ ] Closure commit on `apple-silicon-performance` pending.

## Confidence + risk notes

- **HIGH confidence in G0 classification.** The G-row table requires `count == 0` for G0; first run = count=0; second run = count=0 (reproduced). The three signal axes (stripes, scan-self count, scan shape) match G0 EXACTLY. G1/G2/G2'/G3/G4 all eliminated (each requires count >= 1).
- **HIGH confidence γ.1 is INVALIDATED.** γ.1 = "XVideoSetMode itself faulted before returning" requires main() to have started and reached its first call. The cycle-35 `.CRT$XXC` slot fires AFTER `_PDCLIB_xbox_libc_init` but BEFORE `thrd_create(main_wrapper)` — strictly before main() entry. If main() had even started (let alone reached XVideoSetMode), the pre-main slot would have fired first and ticked `count` to at least 1.
- **MEDIUM confidence in cycle-37+ scope.** The three sub-cases (a)/(b)/(c) of G0 are not distinguishable from cycle-35 evidence on real Xbox. Each cycle-37+ candidate (custom XBE-header callback, static binary diff, EEPROM scratchpad write) has different scope/risk trade-offs; Hermes will pick based on the project's priorities.
- **MEDIUM confidence the cycle-36 evidence is robust against the composite-record.sh failure.** The cycle-34 fallback (xemu-capture snapshot burst with explicit `--width 720 --height 480`) produced 26 NTSC-correct snapshots; the pre-runxbe dashboard snap (25 874 unique) AND dashboard-return snaps at t+22..23s (27k..40k unique) prove the capture path is healthy; the 13 pure-black snaps inside the runxbe window are evidence of genuine composite-signal-off frames (no NV2A publish), NOT broken capture. F8 (no-capture-procedural-failure) DOES NOT APPLY — cycle 36 has actual visual evidence.
- **LOW risk to cycle-23 / cycle-27 / cycle-29 / cycle-31 / cycle-33 / cycle-35 prior guarantees.** ZERO source touched in any of those slices; their Codex validations remain in force.

## What this session does NOT do

- NO host xemu source touched.
- NO `lib/xbed_*` / `oracle-agent/*` / `witness-only/main.c` / `xbed_runtime.{c,h}` touched.
- NO `nxdk/` source touched.
- NO `tools/xemu-capture/` source touched.
- NO `composite-record.sh` / `composite-preflight.sh` source touched (cycle-33 implementations intact; cycle-36 reproduced cycle-34 finding (i); cycle-37+ fix scope).
- NO XBE rebuilds (cycle-35 binary observed; bit-identical to cycle-35 closure commit `515e03f4e7`).
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips; NO M15 movement; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` unchanged.
- NO PushNotification — bounded run-only slice, G0 outcome is informative but does not unblock M15.
- NO cleanup of `.hermes_*` files.
- NO touch of pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` / `retail-gameplay-oracle.py` / `retail-oracle-workflow.py` / `retail-title-automation-proof.py` (preserved per cycle-34 prompt guardrail; carried forward).
- NO touch of pre-existing untracked `scripts/apple-silicon/composite_preflight.py` (preserved per same guardrail; carried forward).

## Next proposed action

Closure commit (`apple-silicon-performance` branch). The substantive next slice is cycle 37+ (Hermes's call): pick one of the three G0 "Next" candidates per the cycle-35 README G0-row + cycle-36 closure rationale (custom XBE-header callback; static binary diff against pipeline-smoke/mirror; EEPROM scratchpad write inside `xbed_self_witness_fire`).
