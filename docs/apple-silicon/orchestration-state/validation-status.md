# Validation Status

- Active slice: cycle 36 real-Xbox discriminator run for cycle-35 pre-main breadcrumb — RUN-ONLY / DOC-ONLY (ZERO source/script/XBE edits).
- Validation state: **Rule #15 Codex SKIPPED under doc-only / run-only carve-out** (same path as cycles 26 / 28 / 30 / 32 / 34). The cycle-35 binary deployed this cycle is bit-identical to the cycle-35 build that passed 4-round Codex green at cycle-35 closure commit `515e03f4e7`; cycle-35 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed artifact. **OUTCOME G0** classified from real-Xbox evidence: (zero stripes across 26 NTSC composite snapshots, `witness.scan-self count=0`, `witness.scan D-cycle-27`), REPRODUCED across two runxbe attempts in same physical power session.

## Rule #15 applicability (cycle 36)

Cycle 36 is RUN-ONLY / DOC-ONLY:

- ZERO `scripts/apple-silicon/xbe-tests/witness-only/main.c` edits.
- ZERO `scripts/apple-silicon/xbe-tests/lib/xbed_*` edits.
- ZERO `scripts/apple-silicon/xbe-tests/oracle-agent/` edits.
- ZERO `scripts/apple-silicon/composite-record.sh` / `composite-preflight.sh` edits.
- ZERO `scripts/apple-silicon/xbe-tests/witness-only/{Makefile,README.md,manifest.json}` edits.
- ZERO XBE rebuilds.
- ZERO host xemu source (`hw/`, `ui/`, `target/`, `include/`) touched.
- ZERO `nxdk/` source touched.
- ZERO `tools/xemu-capture/` source touched.

Doc-only / run-only carve-out APPLIES — same path as cycles 26 / 28 / 30 / 32 / 34 closures. **Codex SKIPPED with explicit justification** (the validated artifact is the cycle-35 binary observed bit-identically this cycle; cycle-35 Codex green at closure `515e03f4e7` covers it).

## Gate status (cycle 36)

- [x] Required docs read.
- [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward through cycles 35 + 36).
- [x] Reachability + baseline both scans MET (D-cycle-27 + count=0).
- [x] Reboot Xbox + FTP-upload cycle-35 `default.xbe` with `--overwrite` (SHA-256 + remote mtime advance verify replacement).
- [x] ensure-agent + recheck preconditions still MET.
- [x] composite-preflight ok (xemu-capture, 1.591 s).
- [x] composite-record.sh ARMED before runxbe (preflight OK + ffmpeg launched; subsequent silent-stall is cycle-34 finding (i) reproducing, NOT a procedural failure of cycle 36).
- [x] First runxbe + dashboard recovery (t+38s, clean); final scans = (D-cycle-27, count=0).
- [x] Second runxbe + 25-snap NTSC composite burst with explicit `--width 720 --height 480`; ZERO stripes detected; final scans = (D-cycle-27, count=0) REPRODUCED.
- [x] G-row classification = G0; rationale + three pre-`.CRT$XXC` sub-cases + γ.1 invalidation documented.
- [x] SUMMARY.md written.
- [x] handoff.md + decision-log.md cycle-36 entries on top with cycle-35 entries preserved unchanged below.
- [x] Orchestration-state quartet closure pass.
- [-] Codex SKIPPED per rule #15 carve-out (explicit justification recorded).
- [x] Closure commit landed as `265010549f` on `apple-silicon-performance`.

## Real-Xbox evidence

- **Baseline scans:** `witness.scan = D-cycle-27 (count=1 phys=0x03eb3000 reserved0=0 reserved1=0 mapped_pages_seen=419)` AND `witness.scan-self = count=0`.
- **FTP upload:** cycle-35 SHA-256 `ab52df8dee32c24b857b3df749e3b8fc0a5a7e8f0949e06ec5c7e4d82aaef5bd`; remote mtime advanced from `Dec 10 17:17` (cycle 31) to `Dec 11 02:20` (cycle 35).
- **Composite preflight:** `status=ok` via xemu-capture in 1.591 s; pre-runxbe snap_00 = 720x480 RGB; 25 874 unique colors; max=(255,255,255); capture path healthy.
- **composite-record.sh:** preflight OK; ffmpeg launched per command line; SILENT-STALLED 103 s (rc=137 capture_timed_out=true); 0 bytes stderr; 0 bytes video.mp4. (Cycle-34 finding (i) reproduced.)
- **First runxbe:** issued 23:02:38Z; dashboard FTP back 23:03:16Z = t+38s clean recovery; final scans = (D-cycle-27, count=0).
- **Second runxbe:** issued 23:06:43Z; 26-snap NTSC burst with explicit `--width 720 --height 480` over t+0..t+29.5s; 13 pure-black (signal-off frames) + 13 dashboard transition/return frames (snap_20-22 carry 27k..40k unique colors); ZERO stripe colors detected across all 26 snaps; final scans = (D-cycle-27, count=0) REPRODUCED.
- **Reproducibility wins:** count=0 reproduced across both runs; D-cycle-27 reproduced 5 readbacks this session and ≥6 cycles (26/28/30/32/34/36); kernel-pool phys=0x03eb3000 reuse ≥20+ consecutive observations across at least 2 physical power sessions; mapped_pages_seen=419 reproduced at every readback.

## G-row classification rationale

Reads as `(stripes visible, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)`:

Cycle 36 observed `(none, 0, n/a, D-cycle-27)`. Per the cycle-35 G-row discriminator table:

- G0 row requires `(none, 0, n/a, D-cycle-27)` — **MATCHES EXACTLY.** Interpretation: NO WTNS page allocated → not even `.CRT$XXC` slot (stage=4) ran.
- G1 / G2 / G2' / G3 / G4 ALL require `count >= 1` — eliminated.
- F-row interpretation is moot: every F row of the cycle-32 F-table assumes main() was at least attempted; G0 says even the pre-main slots didn't fire, strictly earlier than any F-row precondition.

Three pre-`.CRT$XXC`-fire sub-cases share the G0 shape. Cycle-35 evidence on real Xbox CANNOT distinguish them (real Xbox lacks the host-log breadcrumb channel that would tag each sub-case):

(a) Crash inside `_start` / `__security_init_cookie` / TLS-size computation / `_PDCLIB_xbox_libc_init` — strictly BEFORE `_PDCLIB_xbox_run_pre_initializers()` walked `.CRT$XX*` slots. STRICTLY EARLIER than anything cycle 34 could distinguish.
(b) Walker invoked the `.CRT$XXC` slot but the `xbed_self_witness_fire` helper body crashed BEFORE reaching `MmAllocateContiguousMemoryEx`.
(c) `MmAllocateContiguousMemoryEx` returned NULL silently from `.CRT$XXC` (edge case in `lib/xbed_self_witness.c:54-74`).

**γ.1 ("`XVideoSetMode` faulted before returning") is INVALIDATED** because the cycle-35 `.CRT$XXC` slot fires strictly BEFORE `main()` enters; if `main()` had even started (let alone reached `XVideoSetMode`), `count >= 1` would have been observed.

## Why this is not a regression of any prior cycle's validation guarantees

Cycle 23 / 25 / 27 / 29 / 31 / 33 / 35 each Codex-validated their own implementation slices. Cycle 36 does not touch any of those slices' code: `lib/xbed_a4_witness.{c,h}` intact, `lib/xbed_self_witness.{c,h}` intact, `oracle-agent/*` intact, `lib/lib.mk` intact, `lib/xbed_runtime.{c,h}` intact, image-blit intact, `nxdk/` intact, `tools/xemu-capture/` intact, `scripts/apple-silicon/composite-record.sh` + `composite-preflight.sh` intact, `witness-only/main.c` + `Makefile` + `README.md` + `manifest.json` intact. The cycle-35 binary observed bit-identically this cycle is the artifact those validations cover.

## Evidence integrity

- Real-Xbox evidence captured under `benchmark-runs/cycle36-real-xbox-witness-only-pre-main-discriminator-20260523T225306Z/` (gitignored per project convention): 16 per-step logs + SUMMARY.md + composite-cycle36/ failed-recording dir + snapshots-runxbe2-ntsc/ 26-snap burst + 15-stripe-analysis.{json,log}.
- composite-cycle36/capture-meta.json carries the documentary record of the ffmpeg silent-stall (cycle-34 finding (i) reproducing in a separate physical power session).

## Out of scope for this cycle (validation perspective)

- No xemu-fork host source touched; no new flag plumbing inside xemu.
- No shared-lib edits; no oracle-agent edits; no `lib.mk` edits.
- No `witness-only/main.c` / Makefile / README.md / manifest.json edits.
- No nxdk source edits.
- No `tools/xemu-capture/` source edits.
- No `composite-record.sh` / `composite-preflight.sh` source edits.
- No retail-title / §G.5 / RT-as-texture work.
- No cycle-37+ pre-`.CRT$XXC` discriminator work (Hermes's call).
- No cleanup of pre-existing untracked `.hermes_*` files or pre-existing tracked drift in `capture-composite-reference.sh` / `retail-*.py` scripts (preserved per cycle-34 prompt guardrail).
