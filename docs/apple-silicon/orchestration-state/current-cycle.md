# Current Cycle

- Cycle: 31 Path A.4 option (d) on-screen visual breadcrumb — **CLOSED on `apple-silicon-performance`** (implementation slice; ZERO real-Xbox run; cycle-32 real-Xbox deployment is Hermes's call).
- Started: 2026-05-23 (Hermes-supervised bounded session; Claude Code worker autonomous run).
- Closed: 2026-05-23.
- State: **CLOSED.** Cycle 30 (closure commit `dfe1480cba`) observed outcome E2 = `witness.scan = D-cycle-27` AND `witness.scan-self = count=0`; (γ) "main() never reaches the fire calls" is LEADING; cycle-22 pre-main-crash hypothesis re-strengthened toward leading but not fully corroborated. Cycle 31 ships option (d) from the cycle-29 closure catalog: pbkit-free `XVideoSetMode(640, 480, 32, REFRESH_DEFAULT)` + a direct CPU paint of 5 distinguishable horizontal stripes into the resulting linear framebuffer; breadcrumb paint #0 runs as the FIRST observable side effect of `main()`, subsequent paints follow each checkpoint (XCTR fire1 return, XCTR fire2 return, WTNS self-fire1 return, WTNS self-fire2 return). The final settle Sleep is extended from cycle 25's 500 ms to 2 000 ms so a composite-capture stream at ~30 fps captures ≥60 frames of the deepest-painted state. NO shared-lib changes (helpers live entirely in `witness-only/main.c`); NO pbkit; NO NV2A class-object setup; NO xbed_init; NO file I/O. Single new include is `<hal/video.h>` (+`<string.h>` for `memset`).
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: cycle-30 closure commit `dfe1480cba` on `apple-silicon-performance`.
- Bounded goal: "Implement the smallest credible witness-only change that can answer the gamma question at a coarser granularity than cycle 30: did witness-only main() execute far enough to emit a synchronous visible breadcrumb on real hardware? Ship the implementation slice cleanly and leave a precise runbook for the next deployment slice."
- Result: IMPLEMENTATION SLICE COMPLETE. `witness-only/main.c` modified to add `xbed_breadcrumb_init` + `xbed_breadcrumb_paint` static helpers + 5 paint sites; `witness-only/README.md` cycle-31 addendum + 8-row cycle-32 discriminator table + deployment runbook; `witness-only/manifest.json` purpose + `expected_results.real-xbox/physical/cycle-32` section. XBE rebuilt: `bin/default.xbe` 155 648 B (+4 096 B from cycle 29's 151 552 B); `witness-only.iso` 720 896 B (unchanged — same ISO sector boundary). Codex validation per rule #15 (non-trivial diff, ~250 lines C source + paired docs).

## Plan summary (this session, executed in order)

1. Read required docs/state (handoff cycle-30 entry, decision-log cycle-30 entry, orchestration-state quartet, orchestration-workflow, witness-only/README.md, witness-only/main.c, witness-only/Makefile, witness-only/manifest.json, lib/xbed_runtime.{c,h}, nxdk/lib/hal/video.{h,c}).
2. Confirmed repo/git state (HEAD `dfe1480cba`, three pre-existing untracked `.hermes_cycle*.txt` prompt files preserved un-staged; the new `.hermes_cycle31_option_d_prompt.txt` joins the cycle-22 + cycle-23 prompt files at root).
3. Decided implementation approach: modify witness-only/main.c in place (vs. creating a sibling XBE). Modification preserves the natural evolution of witness-only (cycle 25 → cycle 29 → cycle 31); a sibling would have duplicated the cycle-25/29 invariants without clear added benefit; modifying in place keeps the smallest possible diff.
4. Added cycle-31 head-comment addendum + 5 stripe-color constants + `xbed_breadcrumb_init` (XVideoSetMode + clear-to-black; idempotent) + `xbed_breadcrumb_paint(stage)` (96-row band fill + XVideoFlushFB).
5. Inserted 5 `xbed_breadcrumb_paint(N)` calls into `main()`: paint(0) BEFORE the cycle-25 host-log line; paint(1) after fire1 return; paint(2) after fire2 return; paint(3) after self-fire1 return; paint(4) after self-fire2 return.
6. Extended the pre-reboot Sleep from cycle 25's 500 ms to 2 000 ms with cycle-31 rationale comment block.
7. Rebuilt witness-only XBE; verified +4 096 B size delta from cycle 29 (fits in one nxdk XBE page boundary).
8. Updated paired docs: `witness-only/README.md` cycle-31 addendum + 5-stripe color map + 8-row cycle-32 discriminator table + cycle-32 deployment runbook + cross-references updated. `witness-only/manifest.json` title + purpose + `real-xbox/physical/cycle-32` expected_results section (F1..F8 outcomes).
9. Ran Codex validation per rule #15.
10. Synced canonical docs (handoff.md cycle-31 entry on top, decision-log.md cycle-31 entry on top, orchestration-state quartet closure pass).

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; pre-existing `.hermes_cycle*.txt` files preserved un-staged.
3. [x] `witness-only/main.c` modified (head-comment addendum + 2 static helpers + 5 paint sites + Sleep extension).
4. [x] `witness-only/README.md` cycle-31 addendum + 5-stripe map + 8-row cycle-32 discriminator table + runbook.
5. [x] `witness-only/manifest.json` title + purpose + cycle-32 expected_results.
6. [x] `witness-only/bin/default.xbe` rebuilt (155 648 B, +4 096 B from cycle 29).
7. [x] Codex validation per rule #15.
8. [x] Canonical docs synced (handoff.md, decision-log.md, orchestration-state quartet).
9. [x] Closure commit landed as `41f350c174` on `apple-silicon-performance`.

## Out-of-scope (kept bounded for cycle 31)

- NO xemu-fork host source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact).
- NO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact).
- NO `lib/lib.mk` touched (cycle-29 opt-in policy intact).
- NO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 witness.scan-self verb intact).
- NO image-blit source touched.
- NO `xbed_runtime.{c,h}` touched (existing `xbed_init` reference pattern reused without modification).
- NO real-Xbox run this session (cycle-32 scope, Hermes's call).
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO PushNotification — bounded implementation slice, not blocker / milestone.

## Recommended cycle-32 scope (NOT executed this session — Hermes's call)

Deploy the cycle-31 `witness-only/bin/default.xbe` (155 648 B) to `/E/Apps/witness-only/default.xbe` and run the cycle-30 canonical sequence WITH the composite-capture leg ARMED via `scripts/apple-silicon/composite-record.sh` (MS2109 USB stick + ffmpeg AVFoundation) BEFORE issuing `runxbe`. Cycle-29 oracle-agent stays in place (already deployed by cycle 30; `witness.scan-self` verb still registered). Hard preconditions add: composite-capture leg ARMED + MS2109 recognized. Expected outcomes F1..F8 per `witness-only/README.md` cycle-32 discriminator table — keyed on the deepest visible stripe color × `(witness.scan, witness.scan-self)` two-tuple.

The decisive readback combines deepest-stripe count with the cycle-30 two-tuple. F1 (all 5 stripes + WTNS success + XCTR D-cycle-27) means γ INVALIDATED with α/β live on XCTR side. F3 (stripe 0 only + count=0) indicates the cycle-23 witness mechanism is the failure source on real Xbox in this minimal XBE — redesign required. F4 (no stripes + count=0) covers γ.0 (`main()` never entered) OR γ.1 (XVideoSetMode faulted) — cycle-22 pre-main hypothesis FULLY CORROBORATED in its strongest form; next cycle ships pre-main breadcrumbs. F4' (no stripes + WTNS count=1) — graceful XVideoSetMode FALSE return — means `main()` DID execute and γ is INVALIDATED via the WTNS path (Codex round-2 high finding adopted distinguishing F4 from F4'). F2 / F5 / F6 (full success on both mechanisms) / F7 / F8 sit between these endpoints with their own follow-up branches per the 9-row F1..F8 + F4' table.
