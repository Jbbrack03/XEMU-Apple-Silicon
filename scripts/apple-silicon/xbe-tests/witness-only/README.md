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

## Cycle-31 addendum (option (d), 2026-05-23)

Cycle 30 closure (commit `dfe1480cba`) ran the cycle-29 build on real
Xbox; outcome **E2** = `witness.scan = D-cycle-27` AND
`witness.scan-self = count=0`. Per the cycle-30 discriminator table
this makes **(γ)** "main() never reaches the fire calls" LEADING and
re-strengthens the cycle-22 pre-main-crash hypothesis toward leading,
but does NOT fully corroborate it: `main()` could equally well crash
AFTER its first instruction but BEFORE either fire site. Cycle 31
ships **option (d)** from the cycle-29 closure catalog: a pbkit-free
`XVideoSetMode(640, 480, 32, REFRESH_DEFAULT)` + a direct CPU paint of
distinguishable horizontal stripes into the resulting linear
framebuffer. The breadcrumb is the first observable side effect of
`main()`. If composite capture during the cycle-32 (or whatever Hermes
schedules next) real-Xbox run records the top (RED) stripe, `main()`
ran past its first executable instruction and **(γ) is INVALIDATED**.

The cycle-31 paint helpers (`xbed_breadcrumb_init` /
`xbed_breadcrumb_paint`) live entirely in `witness-only/main.c` — no
shared-lib changes (no edits to `lib/xbed_a4_witness.{c,h}`, no edits
to `lib/xbed_self_witness.{c,h}`, no edits to `lib/lib.mk`). The
single new include is `<hal/video.h>` for `XVideoSetMode` +
`XVideoGetFB` + `XVideoFlushFB`; `<string.h>` for `memset`. No pbkit
is called. The kernel paths exercised (`AvGetSavedDataAddress`,
`MmAllocateContiguousMemoryEx` with `PAGE_WRITECOMBINE`,
`AvSetDisplayMode`, `XVideoSetGammaRamp`) are the same paths
`lib/xbed_runtime.c:43-60`'s `xbed_init` already uses for every diag
XBE that draws anything.

### Cycle-31 breadcrumb stripe map

Each stripe is 96 rows tall × 640 wide × 32 bpp ARGB8888 on a 640×480
framebuffer (480 / 5 = 96):

| Stripe (top→bottom) | Color  | ARGB8888    | What it proves                                          |
|---|---|---|---|
| 0  | RED    | `0xFFFF0000` | `main()` reached its first executable instruction      |
| 1  | ORANGE | `0xFFFF7F00` | `xbed_a4_witness_fire(MAIN_ENTERED)` returned          |
| 2  | YELLOW | `0xFFFFFF00` | `xbed_a4_witness_fire(POST_MARKER0)` returned          |
| 3  | GREEN  | `0xFF00FF00` | `xbed_self_witness_fire(MAIN_ENTERED)` returned        |
| 4  | BLUE   | `0xFF0000FF` | `xbed_self_witness_fire(POST_MARKER0)` returned        |

The deepest visible stripe in a composite-captured frame is the
deepest checkpoint reached. Stripe 0 alone visible = `main()` ran past
its first call but did not return from `xbed_a4_witness_fire`. Stripes
0+1 visible = cycle-23 fire1 returned but fire2 did not. Stripes
0+1+2 visible = both cycle-23 fires returned but cycle-29 self-fire1
did not (which would be unexpected — self-fire1 is the
`MmAllocateContiguousMemoryEx` site that cycle 29 considered
known-good). Stripes 0+1+2+3 visible = all of the above plus self-
fire1 returned but self-fire2 did not. All 5 stripes visible = the
full witness path through `main()` executed; the cycle-32 readback
shape is then disambiguated by combining stripe count with the
`(witness.scan, witness.scan-self)` two-tuple per the F1 / F5 / F6
rows of the cycle-32 discriminator table below.

### Cycle-32 discriminator semantics (9 rows; representative, NOT exhaustive)

Reads as `(deepest visible stripe, witness.scan-self count, witness.scan shape)`:

| Stripe(s) visible | `witness.scan-self`                    | `witness.scan`           | Interpretation                                                                                       | Next                                              |
|---|---|---|---|---|
| **0..4 (all)**          | `count=1 reserved0=0xA4000003 reserved1=2` | `D-cycle-27`               | `main()` ran past every checkpoint; WTNS landed; XCTR did NOT land (`witness.scan` still D-cycle-27 means no XCTR stamp survived on the agent's persistent buffer). γ INVALIDATED; (α) OR (β) is the XCTR-side failure mode. Outcome **F1**. | Cycle 33: re-elevate option (b) for α-vs-β discrimination on the XCTR side. |
| **0..2 only**           | `count=0`                                  | `D-cycle-27`               | `main()` ran through cycle-23 fires but crashed before self-fire1 site. WTNS did NOT land (page was never allocated). XCTR also did not land. Outcome **F2**.              | Cycle 33: instrument the call site between fire2 and self-fire1 (or split the cycle-23-vs-cycle-29-fires window with a sleep or a static no-op call). |
| **0 only**              | `count=0`                                  | `D-cycle-27` (or unchanged) | `main()` ran past first instruction but did NOT return from `xbed_a4_witness_fire(MAIN_ENTERED)`. Outcome **F3** — strong cycle-23 mechanism-on-real-Xbox failure signal. | Cycle 33: redesign cycle-23 witness mechanism (EEPROM scratchpad / abandon kseg0 scan). |
| **(none visible)**      | `count=0`                                  | `D-cycle-27` (or unchanged) | Either (γ.0) execution never entered `main()` AT ALL OR (γ.1) `XVideoSetMode` itself faulted (`main()` never returned from the kernel display init). The init-failed state latches so only ONE `XVideoSetMode` call is at risk (see `xbed_breadcrumb_init` in `main.c`). Outcome **F4** — cycle-22 pre-main hypothesis FULLY CORROBORATED in its strongest form. Note: a graceful `XVideoSetMode` FALSE return does NOT land here — it lands at F4' below because the cycle-29 self-witness fires still execute and stamp the WTNS page. | Cycle 33: pre-main breadcrumbs (XCTR fire from a `.CRT$XCU` static-init slot OR a `.dllcharacteristics` early-call). |
| **(none visible)**      | `count=1 reserved0=0xA4000003 reserved1=2` (or partial WTNS shape) | `D-cycle-27` | Graceful `XVideoSetMode` FALSE return — `main()` DID execute past `paint(0)` (which became a no-op when the helper latched FAILED) AND went on to run the cycle-29 self-witness fires. The AV encoder rejected the requested 640×480×32 mode (exotic AV configuration). γ INVALIDATED via the WTNS path; the stripe-0 absence is informative about kernel display init's behavior in this XBE context, NOT about `main()` execution. Outcome **F4'**. | Cycle 33: investigate the AV encoder rejection cause (probe `XVideoListModes` enumeration on this AV configuration); cycle-32 result still INVALIDATES γ even without a visible stripe. |
| **0..4 (all)**          | `count=0`                                  | `D-cycle-27`               | Stripes painted but neither witness fire landed. Outcome **F5** — exotic; would mean the four fire-call instructions ran but their writes never reached RAM. Cache-attribute divergence on the kernel-pool page (β) or self-witness-page allocation failed silently. | Cycle 33: re-elevate option (b) — agent-side prior-phys dump + read-only kseg0 dump to characterize the cache attributes on the agent's persistent page from non-agent context. |
| **0..4 (all)**          | `count=1 reserved0=0xA4000003 reserved1=2` | `count=1 reserved0=0xA4xxxxxx` (A1/A2 success shape) | Full success across BOTH mechanisms — `main()` ran past every checkpoint AND XCTR stamp survived on the agent's persistent buffer AND WTNS landed. Outcome **F6** — cycles 26 / 28 / 30 readings must have been observation-side artifacts; would be the cleanest possible discriminator outcome. | Cycle 33: full re-validation; consider declaring discriminator track CLOSED. |
| **partial stripes** + bands missing intermediately | (any)                                | (any)                      | A paint landed for a later stripe but the earlier one didn't — should be vanishingly rare given the FB is PAGE_WRITECOMBINE + XVideoFlushFB sfences after each paint. Treat as ambiguous; re-run. | Cycle 33: re-run; if reproducible, instrument the paint helper. |
| **(no composite capture)** | (any)                                  | (any)                      | Capture leg not running / capture-card not detected. Cycle-32 procedural failure, NOT a discriminator answer. | Cycle 32 redo with composite capture confirmed armed. |

The table is **representative, not exhaustive**. Composite capture
will record many frames during the witness-only run; the analyzer
should sample a window of frames near `t = runxbe_issued + 2s` (just
before the final 2 000 ms settle Sleep starts releasing for the
reboot) to read the deepest stable stripe state.

### Cycle-31 build artifacts

- `bin/default.xbe` — 155 648 B (+4 096 B = +1 page from cycle 29's
  151 552 B; the new `XVideoSetMode` linkage + ~150 LOC of paint
  helpers fit in one nxdk XBE page boundary).
- `witness-only.iso` — 720 896 B (unchanged — same ISO sector
  boundary as cycle 29).

### Cycle-32 deployment runbook (Hermes-scheduled; NOT this session)

Hard preconditions (in addition to all cycle-30 preconditions):

1. Composite-capture leg ARMED before `runxbe`:
   `scripts/apple-silicon/composite-record.sh <label>` running in a
   parallel terminal (see `docs/apple-silicon/automation.md`
   "composite-record.sh" section). Verify MS2109 USB stick recognized
   via `tools/xemu-capture/build/xemu-capture list`.
2. Cycle-29 oracle-agent already deployed at `E:\Apps\oracle-agent\default.xbe`
   (cycle 30 already deployed it; verify via `help` listing the
   `witness.scan-self` verb).
3. Cycle-31 witness-only deployed at `E:\Apps\witness-only\default.xbe`.
   Local SHA-256 differs from cycle 29 (size mismatch — uploader will
   replace without `--overwrite`; verify post-upload). Capture both
   the cycle-31 SHA-256 and the remote-side mtime advance in the step
   log.
4. Baseline `witness.scan count=1 live=1 reserved0=0 reserved1=0`
   AND baseline `witness.scan-self count=0`. Power-cycle if either
   precondition fails.

Sequence (extends the cycle-30 sequence with the composite-capture
arm + analyze steps):

```sh
# 1. Reachability + state probe.
./scripts/apple-silicon/oracle-orchestrator.py status

# 2. Baseline both scans.
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self

# 3. Reboot to dashboard.
./scripts/apple-silicon/oracle-client.py raw reboot
# Wait for FTP/21 dashboard return (authenticated probe per cycle 30).

# 4. FTP-upload cycle-31 witness-only (size differs — no --overwrite
#    needed, but verify post-upload SHA-256).
./scripts/apple-silicon/xbox-ftp-upload.py \
    scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe \
    /E/Apps/witness-only/default.xbe

# 5. Relaunch agent, recheck both scans (preconditions).
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self

# 6. *** ARM COMPOSITE CAPTURE *** in a parallel terminal:
./scripts/apple-silicon/composite-record.sh cycle32-witness-only-screen

# 7. Chainload witness-only.
./scripts/apple-silicon/oracle-client.py runxbe 'E:\Apps\witness-only\default.xbe'

# 8. Poll FTP/21 (authenticated) + 9001 + ICMP until dashboard FTP returns
#    (cycle 30 observed t+39s on the cycle-29 build; cycle-31 timing
#    may shift due to XVideoSetMode + 2 000 ms settle Sleep, expect
#    t+5..15s longer on a clean exit).

# 9. *** STOP COMPOSITE CAPTURE *** in parallel terminal.

# 10. Post-run: ensure-agent + final both scans.
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self

# 11. Analyze composite recording: extract keyframes near t+2s and
#     read the deepest stable stripe color per the cycle-31 stripe map.
./scripts/apple-silicon/extract-keyframes.py \
    benchmark-runs/<UTC>-composite-cycle32-witness-only-screen/
# Then visually classify per the cycle-32 discriminator table.
```

The decisive readback combines the deepest-visible-stripe count with
the cycle-30 `(witness.scan, witness.scan-self)` two-tuple per the
8-row cycle-32 discriminator table.

## Cycle-35 addendum (pre-main breadcrumb via .CRT$X* slots, 2026-05-23)

Cycle 34 closure (commit `b5327d4d17`) landed OUTCOME **F4** = zero
stripes visible across 22 NTSC-correct composite snapshots over
t+0.07s..t+24.17s post-`runxbe` + `witness.scan = D-cycle-27` +
`witness.scan-self = count=0`. Per the cycle-31 cycle-32 9-row table
this collapses to **(γ.0)** "execution never entered `main()` AT ALL"
OR **(γ.1)** "`XVideoSetMode` itself faulted hard before returning."
Cycle-22 pre-main-crash hypothesis FULLY CORROBORATED in its strongest
form. Cycle 34 alone cannot tell γ.0 from γ.1.

Cycle 35 adds the cheapest mechanism that runs strictly BEFORE `main()`:
two new function-pointer slots in nxdk's CRT-initializer sections so the
diag XBE fires its existing `xbed_self_witness_fire` shim TWICE before
`main()` is entered. The fires use brand-new pre-main stage codes that
do NOT overlap the existing `XBED_A4_STAGE_*` namespace (1, 2, 3 owned
by cycle 23 / `xbed_a4_witness.h`):

| Stage | Slot      | When                                                                                                                         | Where (thread / phase)                                                          |
|-------|-----------|------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------|
| 4     | `.CRT$XXC` | After `__security_init_cookie` + TLS setup + `_PDCLIB_xbox_libc_init`, BEFORE `thrd_create(main_wrapper)`                    | `WinMainCRTStartup` entry thread (earliest straight-line user C in process)     |
| 5     | `.CRT$XCU` | After `_PDCLIB_xbox_run_crt_initializers()`'s `.CRT$XI*` pass succeeds, immediately BEFORE `main()`                          | `main_wrapper` thread (the same thread `main()` itself runs on)                 |

Each fire ticks `xbed_self_witness_fire`'s counter (`reserved1`) and
overwrites the stage byte in `reserved0`. The shim is idempotent:
`.CRT$XXC` slot allocates + zeroes + magic/version-stamps the WTNS page
on first call and stamps stage=4; `.CRT$XCU` reuses the same page and
stamps stage=5; the in-`main()` WTNS fires stamp stage=1 / stage=3 in
the same fashion. After a fully successful run, the page carries
`reserved0=0xA4000003` (the last-stamped POST_MARKER0 byte) and
`reserved1=4` (2 pre-main + 2 in-main WTNS fires).

### Why .CRT$X* over the other cycle-34 F4 "Next"-column candidates

| Candidate                                                                                | Verdict   | Reasoning                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
|------------------------------------------------------------------------------------------|-----------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| (1) nxdk `.CRT$X*` static-init slot stamp                                                | **CHOSEN** | Mechanism documented + exercised on every nxdk-built XBE (`nxdk/lib/pdclib/platform/xbox/crt_initializers.c`). ZERO nxdk / linker / XBE-header changes required. ZERO new shared-lib code: fires reuse `xbed_self_witness_fire` (cycle-29). Scope = `witness-only/main.c` only. Local xemu smoke confirms slots fire BEFORE `main()` in the expected order with WTNS counter ticking to 2 before main entry.                                                                                                                                              |
| (2) Custom XBE-header callback (kernel-controlled entry slot, runs before `.CRT$*`)      | REJECTED  | No documented "pre-CRT entry slot" in nxdk's XBE-header generator (`nxdk/tools/cxbe/`). Implementing would have to modify nxdk itself, widening scope beyond `witness-only` + paired docs. Strictly earlier than `.CRT$XX*` but the γ.0 sub-windows it could uniquely distinguish (`_start` / `__security_init_cookie` / TLS-size computation crashes) are vanishingly unlikely cycle-22 hang sites. Marginal discriminator value does NOT justify modifying nxdk.                                                                                          |
| (3) Thinner alternative to `XVideoSetMode` (direct NV2A CRTC register writes)            | REJECTED  | Does not address the γ.0-vs-γ.1 question — if `main()` does not enter at all, no in-`main()` code runs regardless of whether it pokes CRTC registers or calls `XVideoSetMode`. Also widens the NV2A surface (cycle-23 lockstep + cycle-29 self-witness shim would have to coexist with direct register pokes), violating the cycle-34 prompt's "tightly scoped" guardrail. Filed for cycle-36+ IF cycle-36 narrows the crash site to γ.1 AND a less-invasive paint mechanism becomes useful (likely deferred indefinitely since γ.1 alone is rare-shape). |

### Cycle-36 discriminator table (G rows; extends cycle-32 F rows)

Cycle 36 (Hermes-scheduled) deploys the cycle-35 build on real Xbox and
runs the cycle-32 canonical sequence verbatim (composite-capture arm +
runxbe + post-run scans). The composite + cycle-29 WTNS readback rules
from the cycle-32 table still apply for F1..F8 + F4'; cycle 35 adds the
finer-grained G rows that disambiguate F4 (cycle-34's observed shape)
into γ.0 sub-cases plus a NEW γ.1 candidate window. The cycle-36 readback
combines deepest-visible-stripe + `(witness.scan, witness.scan-self)` two-
tuple + WTNS `reserved1` counter:

Reads as `(stripes visible, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)`:

| Stripes  | scan-self count | scan-self reserved1 | scan shape       | Interpretation                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Next                                                                                                                                                                                                          |
|----------|-----------------|---------------------|-----------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| none     | 0               | n/a                 | `D-cycle-27`    | **G0**: NO WTNS page allocated → not even `.CRT$XXC` slot ran. γ.0 **NARROWED** to "`_start` / `__security_init_cookie` / TLS-size computation / `_PDCLIB_xbox_libc_init` crash." Strictly earlier than anything cycle 34 could distinguish. NB: also surfaces if `MmAllocateContiguousMemoryEx` itself returned NULL from the `.CRT$XXC` slot (allocation-failure edge case in `lib/xbed_self_witness.c:54-74`); a single host-log line `xbed_self_witness: MmAllocateContiguousMemoryEx failed` would discriminate that sub-case (visible if the cycle-36 run also captured `XEMU_GUEST_LOG=1`-equivalent breadcrumbs, which real Xbox does NOT). | Cycle 37: option (2) custom XBE-header callback OR pivot to XBE-level static binary diff against a known-good nxdk XBE (mirror or pipeline-smoke) to localize the pre-libc-init crash. Independently confirm allocator viability via a known-good nxdk XBE re-run. |
| none     | 1               | 1                   | `D-cycle-27`    | **G1**: `.CRT$XXC` slot ran; `.CRT$XCU` did NOT. γ.0 **NARROWED** to "`thrd_create(main_wrapper)` failed OR `main_wrapper`'s `.CRT$XI*` C-initializer pass faulted." `main()` never entered.                                                                                                                                                                                                                                                                                                                                              | Cycle 37: add a `.CRT$XIC` slot that fires between XX and XC to halve the window. Or instrument `thrd_create` return code via host-log breadcrumb.                                                              |
| none     | 1               | 2                   | `D-cycle-27`    | **G2** (γ.1 **candidate** window — narrowed but NOT corroborated): BOTH pre-main slots ran but no in-`main()` WTNS fire landed. Two sub-cases share this shape and the cycle-35 evidence CANNOT distinguish them from each other: (γ.0 sub) `main()` was never entered (crash anywhere between `.CRT$XCU` return and `main()`'s first instruction); OR (γ.1) `main()` entered and crashed inside paint(0) = `XVideoSetMode` BEFORE the cycle-23 XCTR fires + cycle-29 in-`main()` WTNS fires could tick the counter higher. The absence of stripe 0 is **consistent with** but does NOT prove either. | Cycle 37: instrument the `.CRT$XCU` → `main()` entry gap with a `.CRT$XCV` slot fire that ticks reserved1 to `3` BEFORE `main()`'s first instruction (separating γ.0-sub from γ.1). If a cycle-37 run isolates γ.1, option (3) direct NV2A CRTC writes becomes the next slice. |
| none     | 1               | 3 or 4              | `D-cycle-27`    | **G2'** (cycle-35 analogue of cycle-32 F4'): no stripes visible BUT in-`main()` WTNS fires landed (count=1, reserved1=3 if only MAIN_ENTERED in-main fire; reserved1=4 if both). γ INVALIDATED — `main()` entered AND ran past the cycle-23 XCTR fires AND reached at least one cycle-29 in-`main()` WTNS fire. The stripe-0 absence here reflects `xbed_breadcrumb_init` latching FAILED on a graceful `XVideoSetMode` FALSE return (no-op'd `paint(0..4)`); main() continued through the rest of its body. Identical structurally to cycle-32 F4'; cycle 35 inherits that interpretation. | Cycle 37: investigate AV-encoder rejection cause (probe `XVideoListModes` enumeration); γ INVALIDATED via WTNS path; α-vs-β on the XCTR side becomes the live question — re-elevate option (b). |
| 0..4     | 1               | 3 or 4              | `D-cycle-27`    | **G3**: stripes visible + pre-main slots ran + at least one in-`main()` WTNS fire landed (count=1 page; reserved1=3 means in-main MAIN_ENTERED self-fire ran, reserved1=4 means POST_MARKER0 self-fire also ran). γ.0 **INVALIDATED.** `XVideoSetMode` succeeded (stripe 0 visible). Stripe count + remaining cycle-32 F-row rules apply for the in-`main()` checkpoint reached. | Cycle 37: apply the F1/F2/F3 row from the cycle-32 table for the in-`main()` half of the discriminator. |
| 0..4     | 1               | 4                   | A1/A2 success    | **G4**: full success across BOTH mechanisms — pre-main + in-`main()` WTNS landed AND XCTR stamp survived on agent's persistent buffer. Strongest possible cycle-36 outcome. | Cycle 37: declare discriminator track CLOSED; cycles 26/28/30/32/34 must have been observation-side artifacts. |

The cycle-36 readback's `reserved1` counter is the load-bearing signal:
it counts WTNS fires that LANDED. Combine with stripe count (composite
side) and `witness.scan` shape (XCTR side) per the cycle-32 F-table for
the in-`main()` interpretation.

### Cycle-35 build artifacts

- `bin/default.xbe` — 155 648 B (same nxdk XBE page boundary as cycle 31's
  155 648 B; the new ~200 bytes of pre-main breadcrumb code + 2 `.CRT$X*`
  slot pointers fit within the existing page).
- `witness-only.iso` — 720 896 B (unchanged — same ISO sector boundary).

### Cycle-35 local validation evidence

- Build success via `eval "$(nxdk/bin/activate -s)" && make`: same benign
  `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior
  cycles. ZERO new warnings.
- Local xemu smoke (timeout-12s spawn against witness-only.iso with
  `XEMU_GUEST_LOG=1`): host-log channel emitted, in order:
  1. `.CRT$XXC pre-main breadcrumb running (cycle 35)`
  2. `xbed_self_witness: allocated self-witness page phys=0x03fdf000 ... magic='WTNS' version=1`
  3. `xbed_self_witness: fired stage=4 ... counter=1`
  4. `witness-only: pre-main-xx fire returned phys=0x03fdf000`
  5. `.CRT$XCU pre-main breadcrumb running (cycle 35)`
  6. `xbed_self_witness: fired stage=5 ... counter=2`
  7. `witness-only: pre-main-xc fire returned phys=0x03fdf000`
  8. `witness-only: main() entered (cycle 25)` (cycle-31 in-`main()` path runs unchanged)
  9. cycle-23 XCTR fires → phys=0 (expected on standalone xemu with no agent)
  10. cycle-29 in-`main()` WTNS fires → counter=3 then counter=4 on same self-allocated page
- The ordering and counter values match the cycle-35 design exactly: the
  `.CRT$X*` machinery fires BOTH slots BEFORE main() enters; the shim's
  idempotent same-page reuse holds across all 4 WTNS calls.

### Cycle-36 deployment runbook (Hermes-scheduled; NOT this session)

Hard preconditions (in addition to cycle 30 / cycle 32 preconditions):

1. Composite-capture leg ARMED before `runxbe` (same as cycle 32 / 34).
2. Cycle-29 oracle-agent already deployed at `E:\Apps\oracle-agent\default.xbe`
   (cycle 32 / 34 deployed it; verify via `help` listing the `witness.scan-self`
   verb).
3. Cycle-35 witness-only deployed at `E:\Apps\witness-only\default.xbe`.
   Size unchanged (155 648 B same as cycle 31); FTP uploader will replace
   only with `--overwrite` since the size matches the cycle-31 binary
   exactly. Capture BOTH local and remote-side SHA-256 + mtime advance
   in the step log to confirm the cycle-35 binary actually replaced the
   cycle-31 one on the Xbox HDD.
4. Baseline `witness.scan count=1 live=1 reserved0=0 reserved1=0` AND
   baseline `witness.scan-self count=0`. Power-cycle if either fails.

Sequence (extends the cycle-32 runbook by one upload + the cycle-36-aware
analyze step):

```sh
# 1. Reachability + state probe.
./scripts/apple-silicon/oracle-orchestrator.py status

# 2. Baseline both scans.
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self

# 3. Reboot to dashboard.
./scripts/apple-silicon/oracle-client.py raw reboot
# Wait for FTP/21 dashboard return (authenticated probe per cycle 30).

# 4. FTP-upload cycle-35 witness-only WITH --overwrite (size matches
#    cycle 31; the uploader's default size-only diff would otherwise
#    skip the upload — cycle 30 methodology lesson).
./scripts/apple-silicon/xbox-ftp-upload.py --overwrite \
    scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe \
    /E/Apps/witness-only/default.xbe

# 5. Relaunch agent, recheck both scans (preconditions).
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self

# 6. *** ARM COMPOSITE CAPTURE *** in a parallel terminal:
./scripts/apple-silicon/composite-record.sh cycle36-witness-only-pre-main

# 7. Chainload witness-only.
./scripts/apple-silicon/oracle-client.py runxbe 'E:\Apps\witness-only\default.xbe'

# 8. Poll FTP/21 (authenticated) + 9001 + ICMP until dashboard FTP returns
#    (cycle 32/34 observed t+30..70 s on the cycle-31 build; cycle 35 may
#    shift slightly due to two extra pre-main allocations — likely <1s).

# 9. *** STOP COMPOSITE CAPTURE *** in parallel terminal.

# 10. Post-run: ensure-agent + final both scans. The KEY new signal is
#     witness.scan-self's reserved1 counter:
#       reserved1=0/count=0           → G0 (pre-libc-init crash; or alloc NULL)
#       reserved1=1                   → G1 (between XX and XC)
#       reserved1=2                   → G2 (γ.1 candidate — main() never entered OR crashed in paint(0); cycle-35 cannot distinguish — full table)
#       reserved1=3..4 + no stripes   → G2' (graceful XVideoSetMode FALSE; main() continued; γ INVALIDATED via WTNS path)
#       reserved1=3..4 + stripe(s)    → G3 (main() entered; apply cycle-32 F-row rules)
./scripts/apple-silicon/oracle-orchestrator.py ensure-agent
./scripts/apple-silicon/oracle-client.py raw witness.scan
./scripts/apple-silicon/oracle-client.py raw witness.scan-self

# 11. Analyze composite recording: extract keyframes near t+2s and
#     read the deepest stable stripe color per the cycle-31 stripe map.
#     IMPORTANT: pass --width 720 --height 480 explicitly to every
#     xemu-capture snapshot invocation when using the cycle-34 burst
#     substitution (cycle-34 lesson: the bare `snapshot DEVICE --out
#     PATH` invocation defaults to 720x576 PAL which decodes NTSC as
#     pure-zero RGB).
./scripts/apple-silicon/extract-keyframes.py \
    benchmark-runs/<UTC>-composite-cycle36-witness-only-pre-main/
# Combine deepest stripe + witness.scan-self counter per the G-row table.
```

## Cross-references

- `lib/xbed_a4_witness.{h,c}` — the cycle-23 XCTR-scan witness mechanism.
- `lib/xbed_self_witness.{h,c}` — the cycle-29 option (c) self-allocated witness shim.
- `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c::cmd_witness_scan` — cycle-23 XCTR readback RPC.
- `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c::cmd_witness_scan_self` — cycle-29 WTNS readback RPC.
- `scripts/apple-silicon/xbe-tests/image-blit/main.c:789,806` — the cycle-23 witness call sites this XBE deliberately mirrors.
- `lib/xbed_runtime.c:43-60` — the existing `xbed_init` reference for the `XVideoSetMode` call pattern cycle 31 reuses (without `pb_init`).
- `nxdk/lib/hal/video.h` + `nxdk/lib/hal/video.c:303-420` — `XVideoSetMode` / `XVideoGetFB` / `XVideoFlushFB` API surface cycle 31 depends on.
- `docs/apple-silicon/handoff.md` cycle-24 / cycle-26 / cycle-28 / cycle-29 / cycle-30 / cycle-31 entries — failure-mode delta + closure outcomes + cycle-29 + cycle-31 design.
- `docs/apple-silicon/decision-log.md` cycle-24 / cycle-26 / cycle-28 / cycle-29 / cycle-30 / cycle-31 entries — discriminator design + branch decision logic.
- `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/03-chainload-image-blit.log` — cycle-24 indefinite-hang evidence to compare cycle-30 polling against.
- `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/SUMMARY.md` — cycle-28 D-cycle-27 closure that motivated cycle 29.
- `benchmark-runs/cycle30-real-xbox-witness-only-self-20260523T103624Z/SUMMARY.md` — cycle-30 E2 closure that motivated cycle 31.
- `nxdk/lib/pdclib/platform/xbox/crt0.c` — `WinMainCRTStartup` + `main_wrapper`; documents the entry-thread → thread-create → main_wrapper sequence cycle 35 hooks into.
- `nxdk/lib/pdclib/platform/xbox/crt_initializers.c` — nxdk's `.CRT$XX*` / `.CRT$XI*` / `.CRT$XC*` section terminators + walker functions (`_PDCLIB_xbox_run_pre_initializers` + `_PDCLIB_xbox_run_crt_initializers`). Cycle 35 registers `.CRT$XXC` and `.CRT$XCU` slots that sort alphabetically between nxdk's A and Z sentinels.
