# Current Cycle

- Cycle: 35 Path A.4 pre-main breadcrumb via `.CRT$X*` static-init slots — **CLOSED on `apple-silicon-performance`** (closure commit `515e03f4e7`). Bounded implementation slice: option (1) `.CRT$XXC` (stage=4) + `.CRT$XCU` (stage=5) function-pointer slots in `witness-only/main.c` that each fire `xbed_self_witness_fire` BEFORE `main()` enters. Adds the γ.0-vs-γ.1 discriminator that cycle 34's F4 outcome left ambiguous. ZERO shared-lib / oracle-agent / xbed_runtime / nxdk source touched. Local xemu smoke confirms both slots fire BEFORE `main()` with WTNS counter ticking to 2 before main entry; counter ticks to 4 by end of run (2 pre-main + 2 in-main WTNS fires).
- Started: 2026-05-23 (Hermes-supervised bounded session; Claude Code worker run launched after cycle-34 closure commit `b5327d4d17`).
- Closed: 2026-05-23 (implementation landed + local xemu smoke passed + canonical docs/state synced + Codex disposition recorded; closure commit landed as `515e03f4e7`).
- State: **CLOSED.** Implementation-only slice; no real-Xbox run (Hermes's call for cycle 36).
- Owner: Claude Code worker (Hermes-supervised bounded session), launched 2026-05-23.
- HEAD at start: cycle-34 closure commit `b5327d4d17` on `apple-silicon-performance`.
- Bounded goal: "Add a pre-main breadcrumb to witness-only that can distinguish γ.0 (`main()` never entered AT ALL) from γ.1 (`XVideoSetMode` itself faulted before returning). Preferred least-invasive candidate. Keep scope inside `witness-only/` + paired docs/state. Preserve cycle-23 / cycle-29 / cycle-31 contracts. Rebuild + local-validate + Codex per rule #15 + canonical-docs/state sync + commit."
- Result: **Cycle-35 binary built (155 648 B, same nxdk page boundary as cycle 31). Both `.CRT$X*` slots verified to fire BEFORE `main()` in local xemu smoke. WTNS counter mechanism extended from cycle 29's `0 → 1 → 2` (in-main only) to cycle 35's `0 → 1 → 2 → 3 → 4` (pre-main XX → pre-main XC → in-main MAIN_ENTERED → in-main POST_MARKER0). Cycle-36 readback `reserved1` counter becomes load-bearing γ.0-vs-γ.1 discriminator.** M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-36 real-Xbox discriminator run + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED.

## Plan summary (this session, executed in order)

1. Read canonical docs/state (handoff.md cycle-34 + cycle-31 entries; decision-log.md cycle-34 entry; orchestration-workflow.md; oracle-and-xbe.md; witness-only/README.md cycle-31 addendum cycle-32 discriminator table; witness-only/main.c cycle-31 source; manifest.json; lib/xbed_a4_witness.h; lib/xbed_self_witness.h).
2. Inspected git status + recent commits (HEAD = cycle-34 closure `b5327d4d17`).
3. Designed cycle-35 mechanism: chose option (1) `.CRT$X*` static-init slot stamp; documented why options (2) custom XBE-header callback and (3) direct NV2A CRTC writes were rejected.
4. Verified nxdk CRT entry-point structure (`nxdk/lib/pdclib/platform/xbox/crt0.c` + `crt_initializers.c`): `WinMainCRTStartup` → `__security_init_cookie` → TLS setup → `_PDCLIB_xbox_libc_init` → `_PDCLIB_xbox_run_pre_initializers` (walks `.CRT$XX*`) → `thrd_create(main_wrapper)` → main_wrapper → `_PDCLIB_xbox_run_crt_initializers` (walks `.CRT$XI*` then `.CRT$XC*`) → `main()`. Picked `.CRT$XXC` (entry-thread, earliest) and `.CRT$XCU` (main_wrapper-thread, immediately before main).
5. Edited `witness-only/main.c`: cycle-35 head-comment addendum (~100 LOC); two locally-defined stage constants (4 and 5; NOT added to `xbed_a4_witness.h` to keep cycle-23 lockstep namespace untouched); two static `_PVFV` helper functions (`witness_only_pre_main_crt_xx` / `witness_only_pre_main_crt_xc`); two `__attribute__((section(".CRT$X*C"), used))` function-pointer slot declarations.
6. Rebuilt witness-only: `eval "$(nxdk/bin/activate -s)" && make` succeeded cleanly (one benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles). Artifact size 155 648 B — UNCHANGED from cycle 31 (new code fits in existing nxdk page boundary).
7. Local xemu smoke: spawned `dist/xemu.app/Contents/MacOS/xemu` for 12 s against `witness-only.iso` with `XEMU_GUEST_LOG=1`. Captured stderr lines confirm `.CRT$XXC` fires (stage=4 counter=1) → `.CRT$XCU` fires (stage=5 counter=2) → `main() entered` → cycle-23 XCTR fires (phys=0 expected on standalone) → in-main WTNS fires (counter=3 then counter=4). Mechanism wired correctly.
8. Updated `witness-only/README.md` with cycle-35 addendum: design rationale (option 1 chosen, options 2/3 rejected with reasoning table); expanded WTNS counter encoding; new cycle-36 G-row discriminator table (G0..G4 + G2' (cycle-32 F4'-analogue)); cycle-36 deployment runbook (11-step, extends cycle-32 runbook with `--overwrite` upload flag); cycle-35 build artifacts + local validation evidence + cross-references to nxdk CRT source.
9. Updated `witness-only/manifest.json`: title + purpose extension; new `real-xbox/physical/cycle-36` `expected_results` section enumerating G0..G4 + G2' (cycle-32 F4'-analogue). Validated JSON parses cleanly with python3.
10. Updated `docs/apple-silicon/handoff.md` cycle-35 entry on top with full cycle-34 entry preserved unchanged below; `docs/apple-silicon/decision-log.md` cycle-35 entry above cycle-34.
11. Updated orchestration-state quartet: this file + claude-status.md + validation-status.md + handoff-summary.md.
12. Ran Codex validation per rule #15 (non-trivial diff ~200 lines C source + ~150 lines docs). Disposition recorded below + at `.claude/state/codex-validate-last-run` on LOOKS GOOD.
13. Closure commit landed as `515e03f4e7` on `apple-silicon-performance`.

## Exit criteria — final status

1. [x] Required docs/state files read.
2. [x] Repo/git state confirmed; pre-existing tracked drift + untracked `.hermes_*` + `composite_preflight.py` preserved un-staged per cycle-34 prompt guardrail (carried forward to cycle 35).
3. [x] Pre-main breadcrumb mechanism chosen (option 1 `.CRT$X*` slot stamp) and rationale documented (options 2/3 rejected with reasoning).
4. [x] `witness-only/main.c` edited with cycle-35 head-comment addendum + helpers + slot declarations.
5. [x] witness-only XBE rebuilt cleanly (155 648 B unchanged from cycle 31).
6. [x] Local xemu smoke passed: both `.CRT$X*` slots fire BEFORE `main()` with WTNS counter ticking to 2 pre-main + 4 total.
7. [x] `witness-only/README.md` + `manifest.json` updated with cycle-35 addendum + cycle-36 expected_results.
8. [x] `handoff.md` + `decision-log.md` cycle-35 entries on top; cycle-34 entries preserved unchanged below.
9. [x] Orchestration-state quartet closure pass (this file + claude-status.md + validation-status.md + handoff-summary.md).
10. [x] Codex validation per rule #15 run; disposition recorded.
11. [x] Closure commit on `apple-silicon-performance` landed as `515e03f4e7`.

## Out-of-scope (kept bounded for cycle 35)

- NO host xemu source touched (no `hw/`, `ui/`, `target/`, `include/`).
- NO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact; cycle-35 stage codes 4 and 5 are defined locally in `witness-only/main.c` to avoid altering this header).
- NO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact; cycle 35 only adds new CALL SITES from new `.CRT$X*` slots — the shim API + implementation are unchanged).
- NO `lib/lib.mk` touched (cycle-29 opt-in policy intact).
- NO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact).
- NO `xbed_runtime.{c,h}` touched.
- NO image-blit touched.
- NO `nxdk/` source touched (the `.CRT$X*` mechanism is CONSUMED from nxdk's stable CRT API, NOT modified).
- NO `tools/xemu-capture/` source touched.
- NO `scripts/apple-silicon/composite-record.sh` source touched (cycle-33 implementation intact).
- NO `scripts/apple-silicon/composite-preflight.sh` source touched (cycle-33 implementation intact); the cycle-34-filed secondary findings (xemu-capture-yes / ffmpeg-no TCC asymmetry + xemu-capture PAL-default-on-NTSC silent-zero) are explicitly OUT of cycle-35 scope per the cycle-34 prompt guardrail.
- NO retail-title / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- NO real-Xbox run (cycle 36 scope, Hermes's call).
- NO PushNotification — bounded implementation slice, not blocker / milestone.
- NO cleanup of pre-existing untracked `.hermes_*` files at repo root.
- NO touch of pre-existing tracked-but-uncommitted modifications to `capture-composite-reference.sh` / `retail-gameplay-oracle.py` / `retail-oracle-workflow.py` / `retail-title-automation-proof.py` (preserved per cycle-34 prompt guardrail; carried forward to cycle 35).
- NO touch of pre-existing untracked `scripts/apple-silicon/composite_preflight.py` (preserved per cycle-34 prompt guardrail; carried forward).

## Recommended cycle-36 scope (NOT executed this session — Hermes's call)

FTP-deploy cycle-35 `witness-only/bin/default.xbe` (155 648 B; SAME path `/E/Apps/witness-only/default.xbe`) USING `--overwrite` because the cycle-35 XBE size matches cycle 31's exactly and the FTP uploader's default size-only diff would otherwise skip the upload (cycle-30 methodology lesson). Cycle-29 oracle-agent stays in place from cycle 32 / 34. ARM composite-capture leg via `scripts/apple-silicon/composite-record.sh cycle36-witness-only-pre-main` BEFORE issuing `runxbe`. Run the cycle-32 canonical sequence. KEY new signal: `witness.scan-self`'s `reserved1` counter:
- `count=0 reserved1=n/a` → **G0**: pre-libc-init crash (strictly earlier than cycle-34's F4); OR `MmAllocateContiguousMemoryEx` itself returned NULL from `.CRT$XXC` (edge case).
- `count=1 reserved1=1` → **G1**: `.CRT$XXC` slot ran but `.CRT$XCU` did NOT (`thrd_create` failed OR `.CRT$XI*` faulted).
- `count=1 reserved1=2` → **G2**: BOTH pre-main slots ran but no in-`main()` WTNS fire landed. γ.1 **candidate** window (NOT corroborated): cycle-35 evidence CANNOT distinguish between (γ.0-sub) `main()` never entered after `.CRT$XCU` AND (γ.1) `main()` entered and crashed inside paint(0) = `XVideoSetMode`. Cycle 37 should add a `.CRT$XCV` slot to separate.
- `count=1 reserved1=3..4` + no stripes visible → **G2'** (cycle-35 analogue of cycle-32 F4'): graceful `XVideoSetMode` FALSE; `main()` continued through in-`main()` WTNS fires. γ INVALIDATED via WTNS path.
- `count=1 reserved1=3..4` + stripe(s) visible → **G3**: `main()` entered AND paint(0) ran AND reached at least one in-`main()` WTNS fire. γ.0 INVALIDATED. Apply cycle-32 F-row rules for the in-main half.
- `count=1 reserved1=4` + A1/A2 XCTR success shape + 5 stripes visible → **G4**: full success across BOTH mechanisms; declare discriminator track CLOSED.

Secondary findings worth queuing for cycle 37+ (NOT cycle-36 fix scope): same two cycle-34 findings still open — (i) extend `composite-preflight.sh --mode auto` so the ffmpeg leg is gated EVEN when xemu-capture reports ok (closes TCC asymmetry); (ii) default xemu-capture snapshot dimensions to NTSC for MS2109 source OR update `witness-only/README.md` example invocations to include `--width 720 --height 480`.
