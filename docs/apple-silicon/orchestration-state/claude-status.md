# Claude Status

- Objective: cycle 23 Path A.4 — add a non-fopen kernel-pool controller-buffer witness to image-blit so we can tell, on real Xbox, whether image-blit dies before main()'s first instruction (cycle-22 leading hypothesis) or reaches early runtime before file I/O (alternative framing). Bypasses every partition-mount + FATX-driver-state concern by writing into the oracle-agent's persistent `oracle_ctrl_buffer` directly via kseg0, found via 'XCTR' magic scan with `MmGetPhysicalAddress` safety gate + reserved-field filters — no `fopen` anywhere in the witness path.
- Status: **CLOSED — code shipped, local xemu-Metal validation green, Codex round-2 PASS_WITH_FINDINGS (all BLOCKING + MEDIUM resolved; MINOR resolved post-round-2 via comment sync); canonical docs synced; closure commit (code + docs + ISOs + validation marker) landed as `5fce3b14e4`.**
- Active session: cycle 23 (Claude Code worker, Claude Max via /Users/jbbrack03/.local/bin/claude-max-shell), fresh bounded session. HEAD at start = `a023cda719` (cycle-22 Path A.3 closure commit).

## Hypothesis being discriminated

Cycle-22 leading hypothesis: image-blit crashes BEFORE main() body's first instruction (`image_blit_marker(0, "program_entered")` at `image-blit/main.c:780`) — i.e. CRT init / static-init / DllCharacteristics / pre-main XBE thunking. Cycle 22 invalidated cycle-19 hypothesis #1 (launch-path blocker) via comparator evidence (five reference captures wrote D:\\<id>-capture.bin successfully via the same `XLaunchXBE` chainload path). A.4 is the cheapest next discriminator because its witness mechanism has NO fopen dependency.

## Design (locked 2026-05-22 19:24 CDT)

- **Writer (image-blit):** new lib helper `lib/xbed_a4_witness.{h,c}` scans kseg0 [0x80010000, 0x84000000] in 4 KiB strides with `MmGetPhysicalAddress` per-page safety gate (skip unmapped pages without dereferencing) plus a shared candidate filter ('XCTR' magic at offset 0, version 1 at offset 4, reserved[0] ∈ {0, 0xA4xxxxxx}, reserved[1] < 4096), picks the HIGHEST-phys passing candidate (= most recent agent allocation; deterministic across repeated runs since agent allocator grows monotonically per restart), writes `(0xA4 << 24) | stage` to `reserved[0]` (offset 8) + bumps `reserved[1]` (offset 12), wbinvd. Two call sites in `image-blit/main.c`: `XBED_A4_STAGE_MAIN_ENTERED` (1) as the first instruction of `main()`; `XBED_A4_STAGE_POST_MARKER0` (3) immediately after `image_blit_marker(0, ...)` returns.
- **Reader (oracle-agent):** new RPC `witness.scan` in `oracle-agent/commands.{h,c}` enumerates ALL `oracle_ctrl_buffer` instances in kseg0 with the SAME safety gate + filter set as the writer (lockstep documented; helpers extracted on both sides). Reports `buf.N phys=… virt=… live=N reserved0=… reserved1=…` lines + a `count=N mapped_pages_seen=M` summary. The `live` flag cross-references `oracle_ctrl_get()` so the host side immediately sees which buffer is the current agent allocation vs. orphans.
- **Discriminator semantics (cycle-24 real-Xbox readback):**
  - Witness fires (orphan reserved[0] == 0xA4xxxxxx) → image-blit DID reach main()'s first instruction → cycle-22 leading hypothesis INVALIDATED.
  - Witness does NOT fire (no orphan with non-zero reserved[0]) → cycle-22 leading hypothesis CORROBORATED.
  - Witness fires with stage MAIN_ENTERED only → main() entered but marker_00 helper itself crashed.

## Files changed (final, ready to commit)

- NEW: `scripts/apple-silicon/xbe-tests/lib/xbed_a4_witness.h` (113 lines).
- NEW: `scripts/apple-silicon/xbe-tests/lib/xbed_a4_witness.c` (159 lines).
- EDIT: `scripts/apple-silicon/xbe-tests/lib/lib.mk` (+1 line; SRCS add).
- EDIT: `scripts/apple-silicon/xbe-tests/image-blit/main.c` (+26 lines; 2 witness call sites + comments).
- EDIT: `scripts/apple-silicon/xbe-tests/image-blit/{bin/default.xbe, image-blit.iso}` (rebuilt).
- EDIT: `scripts/apple-silicon/xbe-tests/oracle-agent/commands.{h,c}` (+127 lines; cmd_witness_scan + a4_reader_candidate_ok helper + help entry).
- EDIT: `scripts/apple-silicon/xbe-tests/oracle-agent/main.c` (+1 line; register `"witness.scan"` verb).
- EDIT: `scripts/apple-silicon/xbe-tests/oracle-agent/{bin/default.xbe, oracle-agent.iso}` (rebuilt).
- EDIT: `docs/apple-silicon/handoff.md` (cycle-23 entry on top; cycle-17/19/20/21/22 preserved).
- EDIT: `docs/apple-silicon/decision-log.md` (cycle-23 entry above cycle-22; no supersession).
- EDIT: `docs/apple-silicon/orchestration-state/{current-cycle.md, claude-status.md, validation-status.md, handoff-summary.md}` (all four updated for cycle-23 closure).
- NEW: `.claude/state/codex-validate-last-run` (cycle-23 marker).
- ZERO xemu-fork host source files touched.
- ZERO benchmark-runs, ZERO real-Xbox runs.

## Evidence produced

- 4 local xemu-Metal boots (under `XEMU_GUEST_LOG=1` via `xbe-harness`) at `/tmp/cycle23-{noop,filtered,gated,post-codex,final}/image-blit/metal/xemu.log`:
  - Witness call sites BOTH fire on every boot (`xbed_a4_witness: enter stage=1` + `enter stage=3` lines).
  - Standalone xemu has no agent running, scan correctly reports "no XCTR buffer found" with `mapped_pages_seen=378` cold-boot / 77 warm-reboot.
  - Image-blit first-boot tally `pass=3/8 mask=0x31` UNCHANGED from cycle-21 baseline.
  - All cycle-20+21 markers 00..12 still fire to host-log channel.
- Codex round-1 verdict: BLOCK with 4 findings (recorded in `/tmp/codex-cycle23-output-*.txt`).
- Codex round-2 verdict: PASS_WITH_FINDINGS — all 3 BLOCKING + MEDIUM RESOLVED, MINOR PARTIAL (closed post-round-2).
- Validation marker: `.claude/state/codex-validate-last-run`.

## Codex validation

**Round 1 BLOCK + Round 2 PASS_WITH_FINDINGS + post-round-2 MINOR sync = effective PASS.** Rule #15 trigger #2 (non-trivial uncommitted code in xemu-fork/ apple-silicon scripts > 30 lines) was met; ran `/codex-validate changes` per rule. All findings adopted in full:
- BLOCKING #1: `cmd_witness_scan` gained `MmGetPhysicalAddress` per-page gate.
- BLOCKING #2: `cmd_witness_scan` applies same `reserved[0]/reserved[1]` filters as writer (lockstep documented).
- MEDIUM #3: writer changed from first-match to HIGHEST-phys-match for unambiguous repeated-run attribution.
- MINOR #4: header doc drift on "first match"/"first occurrence" wording → replaced with "HIGHEST-phys passing candidate."

Validation marker written at `.claude/state/codex-validate-last-run`:
`2026-05-23T01:02:05Z cycle 23 Path A.4 — codex-validate changes round 2 PASS_WITH_FINDINGS (all BLOCKING resolved, MEDIUM resolved, MINOR resolved via header comment sync after round 2)`

## Next bounded slice (NOT promoted this cycle)

**Cycle 24.** Real-Xbox run of the patched image-blit + oracle-agent (Hermes-scheduled). Discriminator interpretation per the table in the cycle-23 decision-log entry. Hard precondition documented in `lib/xbed_a4_witness.h`: baseline `witness.scan` must show exactly 1 live buffer with reserved[0]=0; if multiple orphans pre-exist from a prior cycle-24 attempt, Hermes must power-cycle the Xbox first.

## Confidence + risk notes

- HIGH confidence in writer-side mechanism (xemu-Metal validated, Codex round-2 PASS).
- HIGH confidence in agent-side `witness.scan` reader correctness (same filter set as writer, same safety pattern, Codex round-2 lockstep verified).
- MEDIUM-HIGH confidence in the discriminator's ability to actually answer cycle-22's leading hypothesis on real Xbox — contingent on the witness call site executing if main() runs at all. If real-Xbox kseg0 page-table behavior differs materially from xemu's emulation (e.g., real Xbox has even fewer mapped pages at chainload time), the MmGetPhysicalAddress gate would just cause the scan to find no candidates and report "no XCTR buffer found" — non-destructive failure mode.
- LOW risk for cycle 24: no destructive operations; witness writes only stamp unused header fields of agent-allocated pages; reader is read-only.
