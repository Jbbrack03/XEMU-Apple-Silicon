# Validation Status

- Active slice: cycle 42G real-Xbox deployment of the cycle-42F R5-GREEN witness-only XBE (SHA `7831b0706850d961805d35f2f336c32866b2204dca9a470418895588e69f441c`, 155 648 B) per the cycle-42E 18-step runbook with the cycle-42F interpretation delta widening the expected post-run `eeprom.scratch.read` value set to include `0xBC`. Run-only + doc-only closeout slice; ZERO source changes; deployed XBE bit-identical to the cycle-42F Codex-R5-GREEN build.
- Validation state: **Rule #15 SATISFIED via doc-only carve-out** (closure commit `26046ad3a8` plus this closeout-state-sync follow-up are both pure doc edits — combined diff well within the doc-only carve-out wording in CLAUDE.md rule #15). The deployed cycle-42F R5-GREEN build was Codex-validated via 5 rounds (R1 MED → R2 MED → R3 MED → R4 MED → R5 GREEN "DEPLOY-READY"); cycle 42G adds ZERO C source body and produces ONLY docs/state edits + gitignored evidence files. Rule #4 (no doc drift) SATISFIED via this update + the matched updates to the other 3 orchestration-state quartet files + handoff.md + decision-log.md + cycle-42G SUMMARY.md.

## Cycle-42G outcome classification

| EEPROM | count | Cycle-42F matrix label | Observed? |
|---|---|---|---|
| 0xBA | 0 | cycle-42E (α)/(β) collapse (no new disambiguator info) | NO |
| **0xBC** | **0** | **cycle-22 axis CONFIRMED on bypass-body + stamp-observability axis; (β)-via-unfired-stamp RULED OUT; residual discoverability question remains** | **✅ YES** |
| 0xBC | 1+ | post-chainload visibility intermittent (would require reproducibility check) | NO |

The observed outcome IS exactly the cleanest predicted success shape from the cycle-42F outcome matrix.

## Cycle-42G deployment summary

| Step | Result |
|------|--------|
| 00 baseline status | ping=true, ftp=true, agent=false (dashboard up post-cycle-42E; required pre-baseline ensure-agent) |
| 00b pre-baseline ensure-agent | agent v0.5 launched OK |
| 01a pre-baseline witness.scan | D-cycle-28 `phys=0x03eb3000 reserved0=0 reserved1=0 count=1 mapped_pages_seen=419` |
| 01b pre-baseline witness.scan-self | `count=0 mapped_pages_seen=419` |
| 01c pre-baseline eeprom.scratch.read | `byte=0xBA` (cycle-42E leftover) |
| 02 reboot | reboot acked at 2026-05-24T16:58:49Z |
| 03 poll recovery | FTP back at t+19s |
| 04 SHA recap | exact match to cycle-42F documented SHA |
| 05 FTP upload | uploaded=1, skipped=0, failed=0 |
| 06 ensure-agent | agent v0.5 re-launched OK |
| 07 unsafe.enable | writes enabled for this session |
| 08 eeprom.scratch.reset | `byte=0x00 (cycle-39 scratchpad cleared)` |
| 09 confirm eeprom.scratch.read | `byte=0x00` |
| 10 witness.scan-self baseline | `count=0 mapped_pages_seen=419` |
| 11 witness.scan baseline | D-cycle-28 `phys=0x03eb3000` |
| 12 runxbe | runxbe acked at 2026-05-24T16:59:49Z |
| 13 poll recovery | FTP back at t+21s |
| 14 ensure-agent post-chainload | agent v0.5 re-launched OK |
| 15 final witness.scan | D-cycle-28 `phys=0x03eb3000 reserved0=0 reserved1=0` |
| 16 final witness.scan-self | `count=0 mapped_pages_seen=419` |
| **17 final eeprom.scratch.read** | **`byte=0xBC tag=0xB stage_nib=0xC`** |
| 18 full EEPROM dump cross-check | last byte at off=0xFF = `bc`; line ends `…0609 00bc`; cross-check passes |

## What cycle 42G PROVES

1. The cycle-42D stage==6 bypass body executed end-to-end through all 11 milestones (0xB0..0xBA full completion) on real hardware.
2. The cycle-42F post-stamp in-process readback of `vp_c42d[0]` through the cached kseg0 alias `(phys | 0x80000000u)` observed the WTNS magic stamp post-`wbinvd`.
3. The subsequent 0xBC SMBus marker write SUCCEEDED.
4. Cycle-42E (β) "silent marker-write inflation past an unfired WTNS stamp" RULED OUT on the bypass-body + stamp-observability axis.
5. Cycle-22 axis advances from "PARTIALLY CONFIRMED on the bypass-body axis" → "CONFIRMED on the bypass-body + stamp-observability axis."
6. The cycle-42B custom pre-WinMainCRT entry-point thunk + cycle-42D 12-marker stage==6 fast-path bypass + cycle-42F post-stamp readback are all working as designed on real hardware.

## What cycle 42G does NOT prove

1. It does NOT prove the WTNS page survives the post-XBE chainload return to the dashboard.
2. It does NOT prove the page's phys falls inside the agent-side `witness.scan-self` enumeration window `[0x80010000, 0x84000000]`.
3. It does NOT formally distinguish (i) "page torn down inside the enumeration window before agent could read it" from (ii) "page survives but lands outside the enumeration window" — that disambiguation is the cycle-42H natural next slice (broaden `cmd_witness_scan_self` phys-range enumeration; oracle-agent source change; rule #15 re-triggers).
4. It does NOT add a reproducibility re-run (matrix does not require one for `(0xBC, 0)`; would be required only for the unexpected `(0xBC, 1+)` row).

## Rule #4 — doc-sync

- handoff.md "Last updated" line replaced with cycle-42G entry; prior cycle-42F entry preserved verbatim above prior cycle-42E entry.
- decision-log.md cycle-42G entry prepended above cycle-42F entry.
- Orchestration-state quartet updated: this file + current-cycle.md + claude-status.md + handoff-summary.md.
- cycle-42G SUMMARY.md written to gitignored run dir `benchmark-runs/cycle42g-realxbox-20260524T165715Z/` with 18 step-output files + 1 pre-baseline ensure-agent log + run-meta + SHA recap + 256-byte EEPROM dump + EEPROM tail.
