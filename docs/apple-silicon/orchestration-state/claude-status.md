# Claude Status

- Objective: cycle 25 Path A.4 witness-mechanism viability discriminator XBE — ship a minimal `witness-only` diag XBE under `scripts/apple-silicon/xbe-tests/witness-only/` so Hermes can later schedule the cycle-26 real-Xbox deployment slice from durable docs without relying on this session transcript.
- Status: **CLOSED.** Fresh bounded session 2026-05-22; HEAD at start = `29a455a78e`. Cycle-25 closure commit lands in this commit.

## Why cycle 25 exists

Cycle 24 ran the cycle-23 A.4 witness on real Xbox and CLOSED with a CONCRETE BLOCKER: cycle-23 image-blit hard-hung the Xbox for 928.3 s of continuous polling on FTP/21 + agent/9001 + ICMP ping. The cycle-22 leading hypothesis ("image-blit crashes BEFORE main()'s first instruction") is now WEAKENED but neither corroborated nor invalidated, because cycle 19/20/21's reproducible 22.4 s chainload→FTP-back gap regressed to indefinite hang — the only change between cycle-21 image-blit and cycle-23 image-blit is ~196 LOC of cycle-23 witness instrumentation, which is itself weak-but-real evidence that *something* inside that instrumentation is executing in cycle-23 image-blit that did not execute in cycle-21 image-blit. NEW hypothesis #5 (the kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context) was promoted as top-priority to discriminate before any further A.4 readback attempt.

Cycle 25 ships the discriminator XBE: a minimal `witness-only` diag that fires the witness twice (MAIN_ENTERED + POST_MARKER0) with sleep gaps and `HalReturnToFirmware(HalRebootRoutine)`s. NO pbkit / NV2A / file I/O / xbed_init. **The intermediate marker helper (image-blit's `image_blit_marker(0, ...)` fopen) is REPLACED with a passive `Sleep(500)` — a successful cycle-26 outcome therefore proves the witness mechanism is real-Xbox-safe in this minimal context but does NOT independently exclude the marker helper as a contributor to image-blit's hang.** Builds via `lib/lib.mk` so it links `xbed_a4_witness.c` exactly the way image-blit does. Cycle 25 does NOT include the real-Xbox run — that is Hermes's call for cycle 26.

## What this session shipped

1. NEW `scripts/apple-silicon/xbe-tests/witness-only/`:
   - `main.c` — minimal `main()` body (~10 statements) + ~100 lines of documentation comments.
   - `Makefile` — lib.mk pattern (identical to `image-blit/Makefile`).
   - `manifest.json` — `real_xbox_only: true`, `oracle_priority: ["real-xbox"]`, record-only `expected_results` for both cycle-26 real-Xbox and cycle-25 local xemu-Metal smoke.
   - `README.md` — purpose / build / local validation / cycle-26 deployment sequence / discriminator-semantics table.
   - `.gitignore` — peer-XBE convention (`*.obj`, `*.exe`, `*.c.d`, `*.cpp.d`, `__pycache__/`).
2. BUILT `bin/default.xbe` (147 456 B) + `witness-only.iso` (720 896 B) via `eval "$(nxdk/bin/activate -s)" && make`.
3. Local xemu-Metal smoke validation green (`benchmark-runs/cycle25-witness-only-xemu-metal-smoke-*/`).
4. Codex 3-round validation green (round 3 = PASS_WITH_FINDINGS).
5. Canonical docs synced (handoff.md cycle-25 entry on top, decision-log.md cycle-25 entry above cycle-24, orchestration-state quartet closure pass).
6. Validation marker written to `.claude/state/codex-validate-last-run`.

## Session progress

- [x] Required docs/rules read; plan summarized to `current-cycle.md` + this file.
- [x] Authored `scripts/apple-silicon/xbe-tests/witness-only/{main.c, Makefile, manifest.json, README.md, .gitignore}`.
- [x] Built `bin/default.xbe` + `witness-only.iso` via nxdk.
- [x] Local xemu-Metal smoke validation green (9 main() entries, 9 fire1, 9 fire2, 8 reboot lines under XEMU_GUEST_LOG=1 across 25 s).
- [x] Codex round 1 (changes mode) → MAJOR ISSUES, 4 findings, ALL ADOPTED.
- [x] Codex round 2 → BLOCK on residual #1 PARTIAL + new LOW, both adopted.
- [x] Codex round 3 → **PASS_WITH_FINDINGS**, no new issues.
- [x] Canonical docs synced (handoff.md cycle-25 entry on top; decision-log.md cycle-25 entry above cycle-24; orchestration-state quartet closure pass).
- [x] Validation marker written.
- [x] Closure commit on `apple-silicon-performance` landed in this commit.

## Confidence + risk notes

- HIGH confidence in the XBE design — it is a direct restatement of the cycle-24 handoff recommendation, narrowed per Codex round-1 #1 finding.
- HIGH confidence in the lib.mk linking pattern — image-blit and 15+ other XBEs already use it.
- MEDIUM confidence that local xemu-Metal validation predicts real-Xbox behavior. The witness mechanism worked correctly on xemu-Metal for cycle 23 (4 boots green) but hard-hung real Xbox from image-blit's process context in cycle 24. xemu's RAM map and `MmGetPhysicalAddress` emulation cannot reproduce real-Xbox MMIO-aliasing failure modes. Local validation only proves the witness mechanism works in emulation — the real-Xbox answer is cycle-26's job.
- LOW risk of additional Xbox damage from cycle 26's eventual deployment. Hermes-side power-cycle ops step is well-trodden across cycles 19+20+21+24.

## What this session does NOT do

- NO real-Xbox deployment (Hermes-scheduled cycle 26).
- NO changes to `lib/xbed_a4_witness.{c,h}` (cycle-23 implementation is binding).
- NO changes to `oracle-agent/` (its `witness.scan` reader is sufficient).
- NO changes to image-blit (sibling XBE; image-blit stays untouched).
- NO xemu-fork host source touched.
- NO flag default flips.

## Next proposed action

Commit the cycle-25 closure on `apple-silicon-performance` and stop cleanly. Cycle-26 real-Xbox deployment is Hermes's call.
