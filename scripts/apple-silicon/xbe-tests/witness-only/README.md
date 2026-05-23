# witness-only — cycle-25 A.4 witness-mechanism viability discriminator XBE

Minimal real-Xbox-only diagnostic XBE. Fires the cycle-23
`xbed_a4_witness_fire(...)` twice with 500 ms sleep gaps, then
`HalReturnToFirmware(HalRebootRoutine)`s. NO pbkit, NO NV2A, NO
`XVideoSetMode`, NO file I/O, NO `xbed_init`. Built via `lib/lib.mk`
so it links `xbed_a4_witness.c` exactly the way `image-blit` does.

## Background

Cycle 24 (2026-05-22) ran the cycle-23 A.4 witness on real Xbox by
deploying the cycle-23 `image-blit` binary (witness lib linked + 2
call sites bracketing `image_blit_marker(0, ...)`). The chainload
hung the Xbox for 928.3 s of continuous polling on FTP/21 +
agent/9001 + ICMP ping — a NEW failure mode that did not appear in
cycles 19/20/21 (which reproduced a stable ~22.4 s chainload→FTP-back
gap across 5 attempts with the pre-witness image-blit binary). The
only change between cycle-21 image-blit and cycle-23 image-blit is
~196 LOC of cycle-23 witness instrumentation, which is itself
weak-but-real evidence that *something* in that instrumentation is
executing in cycle-23 image-blit that did not execute in cycle-21
image-blit.

The cycle-22 leading hypothesis ("image-blit crashes BEFORE main()'s
first instruction") is therefore WEAKENED but not corroborated or
invalidated. Cycle 24 promoted NEW hypothesis #5 as top priority:

> The kseg0-scan witness mechanism may be real-Xbox-unsafe from a
> non-agent process context (different load address, different
> process, different RPC state). The `MmGetPhysicalAddress` per-page
> gate guards against page-fault on unmapped pages but does NOT
> guard against returning non-zero for a mapped-but-MMIO-aliased
> page whose read hangs the bus.

`witness-only` is the cheapest discriminator. It strips image-blit's
`main()` down to ONLY the witness calls + sleeps + reboot. The
cycle-26 real-Xbox run (NOT this session) will discriminate whether
the witness mechanism itself is the failure source or whether
image-blit's hang is from code that is absent from witness-only.

**Scope note (Codex cycle-25 finding #1).** Cycle-25 substitutes a
passive `Sleep(500)` for image-blit's intermediate
`image_blit_marker(0, ...)` call between the two witness fires. A
successful cycle-26 run therefore proves the witness mechanism is
real-Xbox-safe in this minimal context, and that image-blit's hang
is in code ABSENT from `witness-only` — but the absent code set
includes BOTH pbkit/NV2A/xbed_init/draw AND the marker helper
itself. Independently excluding the marker helper as a contributor
requires a follow-on cycle (cycle 26.5 / cycle 27 candidate) that
runs the actual marker helper between the two fires.

## Build

```sh
cd scripts/apple-silicon/xbe-tests/witness-only/
make NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk
```

Output: `bin/default.xbe` plus `witness-only.iso`. Same lib.mk
pattern as `image-blit`, so the same `xbed_a4_witness.c` (and
`xbed_runtime.c` for the host-log channel + `xbed_capture.c` etc.)
are linked in identically. The linked-but-unused helper code is
controlled invariant — its static `.text` cost matches image-blit's,
isolating the cycle-25-vs-image-blit failure-mode delta to (a) the
absence of pbkit/NV2A/draw-loop and (b) `main()`'s body shrinking.

## Local xemu-Metal smoke validation

```sh
export XEMU_GUEST_LOG=1
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
# run via your usual xemu launcher pointing at witness-only.iso;
# the XBE reboots itself, so stop xemu after the second "enter
# stage=*" line lands in stderr.
```

Expected host-log lines in xemu stderr (under
`xemu-guest-log:` prefix):

```
witness-only: main() entered (cycle 25)
xbed_a4_witness: enter stage=1
xbed_a4_witness: no XCTR buffer found at stage=1 (mapped_pages_seen=...; no agent ran before this XBE this boot, ...)
witness-only: fire1 returned phys=0x00000000
xbed_a4_witness: enter stage=3
xbed_a4_witness: no XCTR buffer found at stage=3 (mapped_pages_seen=...; no agent ran before this XBE this boot, ...)
witness-only: fire2 returned phys=0x00000000
witness-only: rebooting via HalReturnToFirmware(HalRebootRoutine)
```

Standalone xemu has no oracle agent running, so the scan correctly
reports "no XCTR buffer found" on both fires (`fire1`/`fire2`
returning `phys=0x00000000`). This is EXPECTED — local validation
proves only that the witness mechanism runs to completion in
emulation; it does NOT predict real-Xbox behavior. xemu's
`MmGetPhysicalAddress` emulation cannot reproduce real-Xbox
MMIO-aliasing failure modes. The real discriminator answer lives
in cycle 26.

## Cycle-26 real-Xbox deployment (NOT this session — Hermes's call)

Hard preconditions:

1. Hermes physically power-cycles the Xbox to clear cycle-24's
   stale persistent buffer. The persistent `oracle_ctrl_buffer` is
   `MmPersistContiguousMemory`-tagged; it survives soft reset but
   NOT power-off.
2. Cycle-23-or-later oracle-agent deployed at
   `/E/Apps/oracle-agent/default.xbe` (its `witness.scan` verb is
   required to read the post-chainload state).
3. `witness-only/bin/default.xbe` uploaded to
   `/E/Apps/witness-only/default.xbe` via FTP.

Sequence:

```sh
# 1. Reboot Xbox to dashboard FTP, ensure agent.
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent

# 2. Baseline witness.scan. HARD PRECONDITION: exactly ONE live
#    buffer with reserved[0] == 0. If multiple A.4-tagged orphans
#    pre-exist (e.g. from a prior cycle-26 attempt in the same
#    power session), power-cycle first.
./scripts/apple-silicon/oracle-client.py raw witness.scan

# 3. Chainload witness-only.
./scripts/apple-silicon/oracle-client.py runxbe 'E:\Apps\witness-only\default.xbe'

# 4. Poll FTP/21 + agent/9001 + ICMP ping until dashboard FTP
#    returns (expect ~5..15 s on success; indefinite on hang).
#    The same polling loop cycle 24 used is appropriate — see
#    benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-*/
#    03-chainload-image-blit.log for the pattern.

# 5. On dashboard return: re-launch agent, query witness.scan.
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
```

## Discriminator semantics table (carried forward from cycle-24 handoff; updated cycle 27)

Cycle-27 update: with `oracle-agent/controller.c::s_allocate_fresh`
preserving an A.4-stamped header across agent re-launch (cycle 27
option (a), commit on `apple-silicon-performance`), "witness stamped
the buffer successfully" now ALSO surfaces as `count=1` with
`live=1 reserved0=0xA4xxxxxx` whenever the kernel pool returns the
same persistent phys to the relaunched agent (the cycle-26-observed
behavior on this Xbox). Either the orphan shape OR the live shape is
a positive "witness landed" outcome under cycle 27. Treat
`count=1 live=1 reserved0=0` as the unambiguous "no stamp landed"
baseline.

| Cycle-26 outcome | Cycle-25-XBE reservoir state | Interpretation | Next cycle |
|---|---|---|---|
| **Reboots in ~5..15 s; agent witness.scan shows either (i) orphan with `reserved[0] == 0xA4000003` OR (ii) live buffer with `reserved[0] == 0xA4000003` on the cycle-26-reused phys** | Both fires landed; `reserved[1]` counter ticked twice (orphan case) or the live buffer carries the surviving stamp (preserve-branch case) | **Witness mechanism IS real-Xbox-safe in this minimal XBE.** Image-blit's hang is in code that is ABSENT from witness-only — that set is pbkit / NV2A / xbed_init / xbed_render_loop_then_capture AND the `image_blit_marker(0, ...)` helper itself. **Cycle-22 leading hypothesis "pre-main crash" is INVALIDATED.** Independently excluding the marker helper requires a follow-on cycle that runs it between the two fires. | Cycle 28: split image-blit's instrumentation across multiple smaller discriminator XBEs (e.g. witness-plus-marker, witness-plus-pbkit-init, witness-plus-xbed_init) to localize. |
| **Hangs Xbox identically to cycle 24** (no FTP/21 / agent/9001 / ping response for 5+ min) | UNRECOVERABLE without power-cycle that erases the buffer | **Witness mechanism itself is real-Xbox-incompatible.** Either the kseg0 scan hits a mapped-but-MMIO-aliased page whose read hangs the bus, or `MmGetPhysicalAddress` returns non-zero for such a page. | Cycle 27: redesign the witness. Candidates: (a) EEPROM scratchpad (survives power-cycle but requires `unsafe.enable` + careful timing); (b) abandon in-XBE witness and pivot to XBE-level static binary diff of cycle-23 vs cycle-21 image-blit; (c) restrict the scan to a single known-safe physical page (the agent's currently-live `oracle_ctrl_buffer`, address persisted to `/E/Apps/oracle-agent/state/ctrl-addr.txt`). |
| **Reboots cleanly but orphan has `reserved[0] == 0xA4000001`** (MAIN_ENTERED only) | First fire landed; second fire's write did not land (or `reserved[1]` ticked from 1 to 2 but `reserved[0]` did not update — would need to inspect `reserved[1]` value to fully disambiguate) | Witness fires once but second fire perturbs CPU state enough to delay or hang the box (with a watchdog soft-reset eventually firing). Less likely; worth surfacing. | Cycle 27: design the witness to fire only once, OR add a longer settle period between fires. |

## What this XBE intentionally does NOT do

- Does NOT call `XVideoSetMode` — no display modeswitch.
- Does NOT call `pb_init` — no NV2A class-object instantiation, no
  pbkit pushbuffer, no DMA setup.
- Does NOT call `xbed_init` — no pbkit, no viewport matrix, no
  shader upload, no default render-state.
- Does NOT call `fopen` — no D:\ or E:\ filesystem touches.
- Does NOT call `image_blit_marker_*` — cycle 20/21's fopen-based
  marker mechanism is not exercised here.
- Does NOT depend on the oracle agent being running locally for
  smoke validation (the witness scan returning "no XCTR buffer
  found" is the expected xemu-Metal outcome).

## What this XBE DOES do (full control-flow trace)

1. Enter `main()`.
2. Emit `witness-only: main() entered (cycle 25)` to port 0xE9.
3. Call `xbed_a4_witness_fire(XBED_A4_STAGE_MAIN_ENTERED)`. The
   helper scans kseg0 [0x80010000, 0x84000000] in 4 KiB strides
   with `MmGetPhysicalAddress` per-page gates, picks the
   highest-phys candidate that matches the `oracle_ctrl_buffer`
   filter set (magic = `XCTR`, version 1, `reserved[0]` ∈
   {0, 0xA4xxxxxx}, `reserved[1] < 4096`), stamps
   `(0xA4 << 24) | 1 = 0xA4000001` into `reserved[0]` and
   increments `reserved[1]`. On standalone xemu (no agent
   running) the scan reports "no XCTR buffer found" and returns 0.
4. Emit `witness-only: fire1 returned phys=0x...` to port 0xE9.
5. `Sleep(500)`.
6. Call `xbed_a4_witness_fire(XBED_A4_STAGE_POST_MARKER0)`. Same
   scan, same selection rule (highest-phys candidate). On real
   Xbox this re-selects the same buffer as fire1 and overwrites
   `reserved[0]` with `0xA4000003`; `reserved[1]` ticks from 1 to
   2.
7. Emit `witness-only: fire2 returned phys=0x...` to port 0xE9.
8. `Sleep(500)`.
9. Emit `witness-only: rebooting via HalReturnToFirmware(...)` to
   port 0xE9.
10. `debugPrint("witness-only: rebooting\n");` — redundant
    breadcrumb on standard xemu log (no `XEMU_GUEST_LOG=1` needed).
11. `HalReturnToFirmware(HalRebootRoutine);` — soft reset.

## Files

- `main.c` — the ~5-statement `main()` body described above.
- `Makefile` — `lib.mk` include + nxdk include; identical structure
  to `image-blit/Makefile`.
- `manifest.json` — `real_xbox_only: true`; declares cycle-26
  expected outcomes for `oracle-orchestrator.py` discovery.
- `README.md` — this file.
- `bin/default.xbe` — built artifact (after `make`).
- `witness-only.iso` — XISO image (after `make`).

## Cycle-29 addendum (option (c), 2026-05-23)

Cycle 28 closure (commit `c77b509149`) ran the cycle-27 preserve-branch
oracle-agent against this cycle-25 witness-only XBE on real Xbox and
observed `count=1 live=1 reserved0=0 reserved1=0` post-run — outcome
**D-cycle-27**. The cycle-27 preserve gate is correctly wired (Codex
3-round green) and a landed A.4 stamp would survive it; the unambiguous
`(0,0)` readback therefore means **no A.4 stamp landed on the agent's
XCTR buffer**. Three live causes per cycle-28 closure:

- **(α)** `xbed_a4_witness::a4_candidate_ok` kseg0 scan from
  witness-only's non-agent process context doesn't find the agent's
  persistent XCTR buffer.
- **(β)** Scan finds it but the write faults silently (PAT/WC/WB
  attribute divergence, cache line never drains).
- **(γ)** `main()` never reaches the fire calls (cycle-22 leading
  "pre-main crash" hypothesis re-strengthens).

Cycle 29 adopts **option (c)** from the cycle-27 closure catalog: the
diag XBE allocates its OWN persistent contiguous page via
`MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory`, stamps a
unique `'WTNS'` magic + `reserved0=(0xA4<<24)|stage` + counter, and
relies on the new oracle-agent `witness.scan-self` verb to enumerate
the page after the chainload. The cycle-23 XCTR fires are preserved
verbatim AND now execute BEFORE the cycle-29 self-witness fires
(Codex round-1 high finding #1 adopted), so the cycle-23 path runs
under conditions bit-identical to cycle 25 up to and including the
second XCTR fire. Cycle 29 is therefore additive but NOT a strict
superset of cycle 25 — the post-cycle-23-fires-to-reboot window gains
new kernel-allocator activity. A single cycle-30 readback exercises
BOTH mechanisms.

**Discriminator scope (Codex round-1 high finding #2 adopted):** a
cycle-30 `witness.scan-self` hit proves only that `main()` ran AND
that writes to a self-owned persistent page survive the chainload.
It does NOT exercise the failing write into the agent's XCTR page;
cause (β) "scan finds the agent buffer but write faults silently"
therefore REMAINS LIVE even on a successful cycle-29 readback. The
cycle-29 readback is positioned narrowly as a **(γ)-only**
discriminator. Breaking α-vs-β requires cycle 31+ option (b)
(agent-side prior-phys dump + read-only kseg0 dump verb).

**Cycle-30 discriminator (Hermes-scheduled — NOT this session):**

| witness.scan | witness.scan-self | Interpretation | Next |
|---|---|---|---|
| D-cycle-27 (`count=1 live=1 reserved0=0 reserved1=0`) | `count=1 reserved0=0xA4000003 reserved1=2` | `main()` ran AND BOTH self-witness fires landed AND self-page is findable from non-agent context → **(γ) INVALIDATED.** (α) AND (β) both REMAIN LIVE — the cycle-29 readback does not exercise the cycle-23 scan-or-write path against the agent's XCTR page. | Cycle 31: option (b) (agent-side prior-phys dump + read-only kseg0 dump) to break α-vs-β. |
| D-cycle-27 | `count=1 reserved0=0xA4000001 reserved1=1` | First self-witness fire landed but second did NOT — `main()` ran past the second cycle-23 fire AND past the inter-fire `Sleep(500)` AND reached the first `xbed_self_witness_fire(MAIN_ENTERED)` but stopped before the second one. (γ) INVALIDATED for the "self-fire-1 reached" sense; refinement: a soft watchdog reset fired between self-fire-1 and self-fire-2. | Same as full-success row — α-vs-β discrimination still requires cycle 31 option (b); refinement of the self-fire-1-to-self-fire-2 window is a cycle 31 side investigation. |
| D-cycle-27 | `count=1 reserved0=0 reserved1=0` | Self-witness page was ALLOCATED + magic/version stamped, but neither fire wrote a stamp into `reserved0`. Should not occur because `xbed_self_witness_fire` writes the stamp in the same call that allocates the page (one statement after the magic/version init); reader tolerates the shape as a soft success because the predicate matches the canonical fresh-allocation header. If observed, the witness shim's stamp writes did not survive (cache flush dropped, page mapping aliased, or kernel post-allocation hooks cleared the writes). Less likely; worth surfacing. | Cycle 31 instrument the writer with a host-log line per write + a post-write read-back inside the shim to confirm the write landed before reboot. |
| D-cycle-27 | `count=0` | No stamp landed anywhere → **(γ) leading**; cycle-22 pre-main-crash hypothesis re-strengthens (the cycle-29 self-witness fires run AFTER the cycle-23 XCTR fires, so this also rules out "main() reached cycle-23 fire #2 but crashed before the cycle-29 fires"). | Cycle 31+: option (d) (on-screen breadcrumb) for independent main()-runs verification. |
| A1 or A2 success (cycle-27 preserve shape) | `count=1 reserved0=0xA4000003 reserved1=2` | Both mechanisms work; cycle-26/28 readbacks must have been observation-side artifacts. Full re-validation required. | Cycle 31: re-run cycle-28 sequence with the cycle-29 agent + verify reproducibility. |
| (no readback because Xbox hangs) | (no readback) | The cycle-29 own-page allocation itself OR the new cycle-23-then-cycle-29-fires ordering is a new failure mode (cycle-24-style 928 s+ indefinite hang). | Cycle 31: redesign — EEPROM scratchpad, abandon in-XBE witness, or XBE-level static binary diff. |
| D-cycle-27 | `count>=2` matching pattern | Multiple self-witness pages accumulated across repeated cycle-30 chainloads within the same physical power session (the persistent contiguous-memory pool kept the older page(s) alive). | Power-cycle the Xbox between cycle-30 attempts if precondition cleanliness is required. |

**Table is representative, not exhaustive (Codex round-3 low finding adopted).** The agent reader `cmd_witness_scan_self` accepts any tagged WTNS page whose `reserved0` carries the 0xA4 tag byte AND whose `reserved1` is in `[1, 4096]`. Partial-success edge cases (e.g. `reserved0=0xA4000003 reserved1=1` — second self-fire stamped its stage but the first self-fire's counter increment did not propagate; or any `reserved0=0xA4xxxxxx` shape with a smaller-than-expected counter) are tolerated by the reader and should be interpreted as "self-fire writes partially landed; main() reached at least one fire site." The reader's two-tuple `(reserved0, reserved1)` is reported verbatim; operators reading the output should treat any tagged shape as a γ-invalidating signal and use the counter + stage byte to pinpoint how far through `main()` the XBE got.

**Cycle-30 sequence (same canonical sequence cycle 26 + cycle 28 used,
extended with witness.scan-self queries):**

```sh
# 1. Reachability + cycle-23 agent baseline.
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self  # cycle 29

# 2. Reboot to dashboard (clears any stale agent-process state).
./scripts/apple-silicon/oracle-client.py raw reboot
# wait for FTP/21 on the Xbox.

# 3. FTP-upload cycle-29 oracle-agent + cycle-29 witness-only.
# (Same FTP STOR pattern cycle 28 used.)

# 4. Relaunch agent, query baseline both scans.
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self

# 5. Chainload witness-only.
./scripts/apple-silicon/oracle-client.py runxbe 'E:\Apps\witness-only\default.xbe'

# 6. Poll FTP/21 + agent/9001 + ICMP ping until dashboard FTP returns.
#    Expect ~70 s on success (matches cycle 26 + cycle 28).

# 7. On dashboard return: relaunch agent, query BOTH scans.
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self  # cycle 29
```

Hard preconditions for an unambiguous cycle-30 readback:

1. Baseline `witness.scan-self count=0` BEFORE chainload. If the
   self-witness page already exists from a prior cycle-30 attempt in
   the same physical power session, Hermes must power-cycle the Xbox
   (the persistent contiguous-memory page is `MmPersistContiguousMemory`-
   tagged; it survives soft reset but NOT power-off — same property
   the cycle-23 XCTR buffer relies on).
2. Baseline `witness.scan` precondition unchanged from cycle 26/28:
   exactly ONE live buffer with `reserved[0] == 0`.

## Cross-references

- `lib/xbed_a4_witness.{h,c}` — the cycle-23 XCTR-scan witness mechanism.
- `lib/xbed_self_witness.{h,c}` — the cycle-29 option (c) self-allocated witness shim.
- `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c::cmd_witness_scan` — cycle-23 XCTR readback RPC.
- `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c::cmd_witness_scan_self` — cycle-29 WTNS readback RPC.
- `scripts/apple-silicon/xbe-tests/image-blit/main.c:789,806` — the cycle-23 witness call sites this XBE deliberately mirrors.
- `docs/apple-silicon/handoff.md` cycle-24 / cycle-26 / cycle-28 / cycle-29 entries — failure-mode delta + closure outcomes + cycle-29 design.
- `docs/apple-silicon/decision-log.md` cycle-24 / cycle-26 / cycle-28 / cycle-29 entries — discriminator design + branch decision logic.
- `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/03-chainload-image-blit.log` — cycle-24 indefinite-hang evidence to compare cycle-30 polling against.
- `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/SUMMARY.md` — cycle-28 D-cycle-27 closure that motivated cycle 29.
