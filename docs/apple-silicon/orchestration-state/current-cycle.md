# Current Cycle

- Cycle: 25 Path A.4 witness-mechanism viability discriminator XBE (**CLOSED — 2026-05-22**).
- Started: 2026-05-22 (Hermes-supervised fresh bounded session).
- Closed: 2026-05-22 (this update + closure commit).
- Worker receipt posted: 2026-05-22 (early in session, before deeper work).
- State: **CLOSED. `witness-only` discriminator XBE SHIPPED + Codex round-3 PASS_WITH_FINDINGS + canonical docs synced. Cycle-26 real-Xbox deployment slice deferred to Hermes.**
- Owner: Claude Code worker (fresh bounded session), launched 2026-05-22.
- HEAD at start: `29a455a78e` (cycle-24 closure-doc sync follow-up).
- Cycle-25 closure commit: this commit on `apple-silicon-performance`.
- Bounded goal: "Ship the cycle-25 witness-only diagnostic XBE under `scripts/apple-silicon/xbe-tests/witness-only/` so Hermes can later schedule the real-Xbox deployment slice from durable docs, without relying on this session transcript. NO real-Xbox deployment in this session."
- Result: SHIPPED. 5 new files + 2 built artifacts; cycle-23 lib + agent + image-blit UNTOUCHED; xemu-fork host source UNTOUCHED.

## Plan summary (this session, executed in order)

1. ✅ Read required docs/rules and post worker receipt to current-cycle.md + claude-status.md before deeper work.
2. ✅ Authored `scripts/apple-silicon/xbe-tests/witness-only/{main.c, Makefile, manifest.json, README.md, .gitignore}`.
   - `main.c` body: `xbed_host_log_write` anchor → `xbed_a4_witness_fire(XBED_A4_STAGE_MAIN_ENTERED)` → log fire1 return → `Sleep(500)` → `xbed_a4_witness_fire(XBED_A4_STAGE_POST_MARKER0)` → log fire2 return → `Sleep(500)` → `HalReturnToFirmware(HalRebootRoutine)`.
   - NO `xbed_init`, NO pbkit, NO NV2A, NO `XVideoSetMode`, NO `fopen`, NO `image_blit_marker_*`.
   - `Makefile` uses the `lib.mk` pattern identical to `image-blit/Makefile`.
   - `manifest.json` declares `real_xbox_only:true`, `oracle_priority:["real-xbox"]`, two record-only `expected_results` keys (cycle-26 real-Xbox + cycle-25 local xemu-Metal smoke).
3. ✅ Built `bin/default.xbe` (147 456 B) + `witness-only.iso` (720 896 B) via `eval "$(nxdk/bin/activate -s)" && make`.
4. ✅ Local xemu-Metal smoke validation green: 9× `witness-only: main() entered`, 9/9 fire1+fire2 pairs, 8× `HalReturnToFirmware` reboot lines across 25 s under `XEMU_GUEST_LOG=1`. Evidence at `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-20260523T034519Z/`.
5. ✅ Mandatory Codex validation on new XBE source + manifest (rule #15 — non-trivial code slice). 3 rounds:
   - Round 1: MAJOR ISSUES, 4 findings; all adopted.
   - Round 2: BLOCK on residual #1 PARTIAL + new LOW; both adopted.
   - Round 3: **PASS_WITH_FINDINGS**, all blocking + medium + low RESOLVED, no new issues.
   - Validation marker written.
6. ✅ Synced canonical docs: handoff.md cycle-25 entry on top (cycle-24 entry preserved unchanged); decision-log.md cycle-25 entry above cycle-24 (no supersession); orchestration-state quartet (current-cycle.md = this file, claude-status.md, validation-status.md, handoff-summary.md) closure pass.
7. ✅ Closure commit landed on `apple-silicon-performance` in this commit.
8. ✅ Stop cleanly. Cycle-26 real-Xbox deployment is Hermes's call.

## Exit criteria — final status

1. [x] Required docs read and plan summarized to current-cycle.md + claude-status.md (early in session).
2. [x] `witness-only/{main.c, Makefile, manifest.json, README.md, .gitignore}` authored.
3. [x] `bin/default.xbe` (147 456 B) + `witness-only.iso` (720 896 B) produced cleanly via `make` under the project nxdk flow.
4. [x] Local xemu-Metal smoke validation green: 9× `witness-only: main() entered`, 9× `xbed_a4_witness: enter stage=1`, 9× `xbed_a4_witness: enter stage=3`, 8× `rebooting via HalReturnToFirmware` lines under `XEMU_GUEST_LOG=1`. Evidence preserved under `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-*/`. Local validation only proves the mechanism's logic-level correctness in emulation; real-Xbox MMIO-aliasing failure modes are NOT reproducible in xemu and remain cycle-26 scope.
5. [x] Codex validation 3 rounds (round 3 = **PASS_WITH_FINDINGS**); validation marker written.
6. [x] Canonical docs synced (handoff.md, decision-log.md, orchestration-state quartet).
7. [x] Cycle-25 closure commit landed on `apple-silicon-performance`.

## Out-of-scope (kept bounded for cycle 25)

- NO real-Xbox deployment (that is cycle 26, Hermes's call).
- NO xemu-fork host source touched.
- NO changes to `lib/xbed_a4_witness.{c,h}` (cycle-23 implementation is binding).
- NO changes to `oracle-agent/` (its `witness.scan` reader is sufficient).
- NO changes to image-blit (sibling XBE; image-blit stays untouched).
- NO retail-title, §G.5, RT-as-texture, second-wave XBE work.
- NO flag default flips.

## Discriminator semantics (carried forward from cycle 24; cycle-25 #1 Codex finding integrated)

Cycle-25 real-Xbox run shape (cycle 26, NOT this session):

1. Hermes physically power-cycles Xbox if multiple A.4-tagged orphans pre-exist (cycle-24 left a stale persistent buffer; if Hermes ran additional attempts in the same power session, prior orphans would accumulate).
2. Hermes ensures cycle-23 oracle-agent is deployed at `/E/Apps/oracle-agent/default.xbe` (its `witness.scan` verb is required to read the buffer afterward).
3. Hermes uploads `witness-only/bin/default.xbe` to `/E/Apps/witness-only/default.xbe` via FTP.
4. Hermes runs `oracle-orchestrator.py ensure-agent`, then baseline `witness.scan`. Hard precondition: exactly ONE live `oracle_ctrl_buffer` with `reserved[0] == 0`.
5. Hermes runs `oracle-client.py runxbe 'E:\Apps\witness-only\default.xbe'`.
6. Hermes polls FTP/21 + agent/9001 + ICMP ping.
7. After dashboard returns, Hermes restarts agent and queries `witness.scan` again.

Branch results:

- Reboots in ~5..15 s + orphan with `reserved[0] == 0xA4000003` → witness mechanism IS real-Xbox-safe **in this minimal XBE**; image-blit's hang is in code ABSENT from witness-only — that set is pbkit / NV2A / xbed_init / xbed_render_loop_then_capture AND the `image_blit_marker(0, ...)` helper itself (witness-only substitutes a passive `Sleep(500)` for the marker helper between the two fires, so independently excluding the marker helper requires a follow-on cycle that exercises it). **Cycle-22 leading hypothesis ("pre-main crash") INVALIDATED.** Cycle 27 splits image-blit's instrumentation across multiple smaller discriminator XBEs (e.g. witness-plus-marker, witness-plus-pbkit-init, witness-plus-xbed_init) to localize.
- Hangs Xbox identically to cycle 24 (no FTP/21 / agent/9001 / ICMP ping response for 5+ minutes) → witness mechanism itself is real-Xbox-incompatible from a non-agent process context; redesign required (EEPROM scratchpad / non-MMIO-aliased RAM / abandon in-XBE witness in favor of XBE-level static binary diff).
- Reboots cleanly but orphan has `reserved[0] == 0xA4000001` (MAIN_ENTERED but NOT POST_MARKER0) → witness fires once but second fire hangs (CPU state corruption between fires); less likely; worth surfacing for cycle 27 design.
