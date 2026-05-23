# Claude Status

- Objective: cycle 28 Path A.4 real-Xbox deployment of cycle-27 preserve-branch oracle-agent vs cycle-25 witness-only XBE — discriminate cycle-26 stamp-vs-no-stamp ambiguity.
- Status: **CLOSED.** Outcome **D-cycle-27** observed; cycle-27 option (a) demonstrated insufficient; cycle-26 "stamp landed and got wiped" INVALIDATED; "stamp never landed" conclusion now isolated; cycle 29 candidate scope identified.

## Why cycle 28 ran this session

Hermes pre-session instruction explicitly assigned the cycle-28 real-Xbox deployment slice as the bounded slice for this session, citing cycle-27 closure (commit `df999e41ea`, doc-sync `291b607a46`) which shipped the `s_allocate_fresh` preserve-branch oracle-agent and recorded the cycle-28 expected positive shapes (A1: `count=1 live=1 reserved0=0xA4xxxxxx`; A2: `count>=2` with stamped orphan) and negative shapes (D-cycle-27, B). Cycle 28 is the first chance to discriminate the cycle-26 ambiguity into either "stamp landed" (A1/A2) or "stamp never landed" (D-cycle-27) or "real-Xbox hang" (B). This is also the first real-Xbox run since the cycle-27 source change.

## What this session shipped

1. Executed canonical cycle-26-style sequence against the cycle-27 preserve-branch oracle-agent:
   - Reachability probe (cycle-23 agent resident as foreground since cycle 26).
   - Baseline `witness.scan` precondition (count=1 phys=0x03eb3000 reserved0=0 mapped_pages_seen=419 — matches cycle 26 exactly).
   - `reboot` → dashboard FTP-LIST at t+27s.
   - FTP STOR new cycle-27 oracle-agent (417 792 B) → verified.
   - `ensure-agent` launches cycle-27 build → banner unchanged (expected) → rescan identical to baseline.
   - `runxbe witness-only` → FTP-LIST poll → dashboard ready at t+70s (reproduces cycle 26's 70.17 s).
   - Post-run `ensure-agent` (cycle-27 build) + final `witness.scan` = **count=1 buf.0 phys=0x03eb3000 reserved0=0 reserved1=0**.
2. Wrote `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/SUMMARY.md` documenting all 9 evidence logs and full hypothesis analysis.
3. Canonical docs synced: handoff.md cycle-28 entry, decision-log.md cycle-28 entry, orchestration-state quartet closure pass.

## Session progress

- [x] Read required docs/state files.
- [x] Probed Xbox reachability + captured baseline `witness.scan` (precondition MET).
- [x] Rebooted agent → dashboard FTP-LIST ready (t+27s).
- [x] FTP-uploaded cycle-27 oracle-agent (417 792 B); verified remote size.
- [x] Re-launched cycle-27 oracle-agent via `ensure-agent`; confirmed responsive + post-launch rescan identical to baseline.
- [x] Chainloaded cycle-25 witness-only; FTP-LIST poll measured dashboard recovery at t+70s.
- [x] Restarted cycle-27 agent + captured final `witness.scan` = outcome D-cycle-27.
- [x] Preserved 9 evidence logs + SUMMARY.md under `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/`.
- [x] Synced handoff.md, decision-log.md, orchestration-state quartet.
- [x] Closure commit landed on `apple-silicon-performance` (this commit).

## Confidence + risk notes

- **HIGH confidence** in the outcome shape classification (D-cycle-27). The cycle-27 preserve gate's strict predicate `(reserved0==0, reserved1==0)` OR `((reserved0>>24)==0xA4, 1<=reserved1<=4096)` covers BOTH header shapes `xbed_a4_witness.c` actually writes; any landed stamp would survive `s_allocate_fresh`'s preserve branch and appear in the readback. Observed `(0,0)` is unambiguous.
- **HIGH confidence** in the "stamp-landed-then-wiped INVALIDATED" conclusion. The cycle-27 build is the same one Codex 3-round-validated at cycle-27 closure; the preserve branch logic is reviewed and correct.
- **MEDIUM confidence** in the three live "stamp never landed" causes (α/β/γ). Cycle-28 evidence cannot distinguish them; cycle-29 instrumentation needed.
- **HIGH confidence** in reproducibility findings (70 s recovery shape; deterministic phys=0x03eb3000 reuse). Both reproduced across cycle 26 + cycle 28.
- **LOW risk** to existing state. Cycle 28 made no source changes, no XBE rebuilds, and the cycle-27 oracle-agent + cycle-25 witness-only XBEs were already on the Xbox (cycle-25 from cycle 26; cycle-23 was being overwritten by cycle-27 FTP-upload, but cycle-27 is functionally a superset).

## What this session does NOT do

- NO source/script code edits.
- NO XBE rebuilds.
- NO cycle-29 design promotion (Hermes's call).
- NO retail-title / §G.5 / RT-as-texture work.
- NO flag default flips.
- NO PushNotification — outcome is partial-discriminator result, not a milestone / blocker.

## Next proposed action

Cycle-28 closure commit (this commit) lands on `apple-silicon-performance`. Cycle 29 (Hermes's call) should promote option (c) [recommended — witness-only allocates its own persistent page with unique magic tag] to discriminate (α) "agent buffer not findable from non-agent context" from (γ) "witness-only never reaches main()." If option (c)'s self-allocated page IS findable post-run by an analogous read-only scanner, (α) was the cycle-26/28 blocker. If even option (c) lands nothing, (γ) becomes leading and cycle-22's "pre-main crash" hypothesis re-strengthens.
