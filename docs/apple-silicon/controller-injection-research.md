# Controller Injection — Feasibility & Design

Last updated: 2026-05-07 (initial draft, alongside the v0.3 oracle
agent's `controller.*` protocol commands. Two Codex review passes on
the same day refined the agent's buffer ABI to match xemu's
`ControllerState` byte-for-byte; this doc's Tier-1 plan continues to
reference the post-fix ABI — int16 triggers, 26-byte ports, 120-byte
buffer, button bits matching `CONTROLLER_BUTTON_*` exactly.)

## Problem statement

The oracle pipeline already drives the **emulator** (xemu) into gameplay
states via `XEMU_SCRIPTED_INPUT=path.csv` (see `ui/xemu-input.c:140-275`)
or live via `XEMU_RECORD_INPUT`. The third oracle leg — the real Xbox
console — needs the same capability so the same recorded gameplay CSV
can drive both engines and the validation pipeline can compare in
gameplay (not just menus / boot screens).

The agent's new `controller.*` RPCs (Phase 1, shipped 2026-05-06) own
the **synthetic input state** — i.e. what the controller "should" be
reporting. What's missing is the **delivery path** that makes that
synthetic state visible to the running game's input-read code.

This document inventories the feasible delivery paths, ranks them by
risk vs reward, and records the design decisions for each.

## Three implementation tiers

| Tier | Coverage | Path | Status |
| ---- | -------- | ---- | ------ |
| 1    | Our own diag XBEs | Shared-buffer + `xbed_input_synth` shim | Designed, ready to ship. ~1 session of work. |
| 2    | Retail games | Kernel hook on `XInputGetState` + USB poll | Designed, NOT YET implemented. ~2-3 sessions of careful reverse-engineering + risk mitigation. |
| 3    | Generic / fallback | Hardware controller emulator (Mac → Xbox port) | Documented as alternative. Hardware project (Teensy + open-source firmware), out of scope for the current toolchain. |

Tiers 1 and 2 are both software-only and use the same agent-side state
buffer. Tier 3 is a hardware backstop if Tier 2 turns out to be too
risky on a particular kernel / dashboard combination.

---

## Tier 1 — Shared-buffer + diag-XBE shim (RECOMMENDED NEXT STEP)

**Coverage:** any diagnostic XBE we author (`xbe-tests/<id>/`).
**Risk:** very low. No kernel patching, no USB stack work; the diag
XBE simply reads memory we already own.
**Engineering:** ~1 session.

### Architecture

The agent allocates the synthetic-input state buffer (defined by
`oracle_ctrl_buffer` in
`scripts/apple-silicon/xbe-tests/oracle-agent/controller.h`) at a
**known kernel-pool physical address** rather than in BSS. The address
is published via the `controller.buffer-info` RPC. Diag XBEs link
against a small `xbed_input_synth.h` shim (added to `xbe-tests/lib/`)
that:

1. On boot, queries the agent (or hardcodes the address) for the
   buffer's location.
2. Maps the page into the diag-XBE's address space using the kernel's
   `MmMapIoSpace` against the published physical address.
3. Each frame, before the diag's render loop runs, reads the
   relevant `port[N]` fields and applies them to whatever simulated
   gameplay state the diag is exercising.

### Where the diag XBE consumes the buffer

Diag XBEs already own their own input — they're not running a
real game. They can choose to read from the synthetic buffer instead
of (or in addition to) physical controllers:

```c
#include "xbed_input_synth.h"

void diag_main(void) {
    xbed_input_synth_attach(/* port */ 0);
    xbed_setup_pipeline();
    while (!xbed_should_exit()) {
        struct oracle_ctrl_port_state s;
        xbed_input_synth_read(&s);
        if (s.buttons & ORACLE_BTN_A) { /* run scene branch A */ }
        if (s.lstick_x > 16000)       { /* steer right */ }
        xbed_render_frame();
    }
}
```

This is enough for diag XBEs that exercise multiple gameplay branches
(e.g. a `combiner-stage` diag that wants to render every NV2A combiner
preset under both A-pressed and A-released states).

### Phase 1 vs Phase 2 split

What the agent **already does** (v0.3, this session):

- Owns the canonical `oracle_ctrl_buffer` (BSS-allocated, in-process).
- Exposes `controller.set / .get / .button / .axis / .clear /
  .buffer-info`.
- Bumps `port[N].seq` and `port[N].timestamp_us` on every state change.

What's left for Tier 1 (next session):

- Move the buffer from BSS to a kernel-pool allocation
  (`MmAllocateContiguousMemory` or `ExAllocatePoolWithTag` so the
  physical address is stable and can be communicated cross-process).
- Add `xbe-tests/lib/xbed_input_synth.{h,c}` so any diag XBE links
  with `lib.mk` and gets `xbed_input_synth_attach()` /
  `xbed_input_synth_read()` with two lines of code.
- Author one Tier 1 diag XBE that demonstrates: e.g.
  `controller-roundtrip` writes a known sequence via the agent,
  chainloads, the diag reads each event from the buffer and
  composites a per-event color stripe to disk; oracle compares
  against a math-derived expected.png.

After that lands, every new diag XBE can opt into synthetic-input
gating with no extra agent work.

### Cross-XBE persistence

The **kernel persists across `XLaunchXBE`** — verified by Phase 3.0
(`pipeline-smoke`) where the kernel-resident lwIP stack survived the
chainload-and-back cycle. So an allocation made by the agent before
chainload remains at the same physical address inside the diag XBE.
The catch is that the agent's PROCESS context is torn down, so the
allocation must be made from a kernel pool the kernel keeps alive.
`ExAllocatePoolWithTag(NonPagedPool, …, 'XCTR')` is the standard
pattern; the tag lets us locate the allocation post-chainload.

---

## Tier 2 — Kernel hook for retail games (HARDER, FUTURE WORK)

**Coverage:** every retail Xbox game.
**Risk:** moderate-to-high. A buggy hook crashes the kernel, and
the user has to physically power-cycle the Xbox.
**Engineering:** ~2-3 sessions of careful work, plus a test-bed
diag XBE that exercises every code path before pointing it at retail.

### How real games read controller state

Retail Xbox games read controllers via the leaked Microsoft XInput
API:

```c
DWORD XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState);
```

`XInputGetState` is **not in the kernel** — it's part of the libgame
runtime (xboxgames.lib / xinput.lib) statically linked into every
title. Hooking it requires patching the call-site of the running
title, which is per-title and brittle.

The cleaner hook point is **the kernel function the libgame XInput
implementation eventually calls**: `XID_GetReport` (HID device-level
report read) routed through the kernel's USB OHCI driver. Every game
reads from the same underlying USB port, so a single hook covers
every title.

The reverse-engineering map (rough, requires verification against
nxdk's xboxkrnl.h ordinal table + a kernel-symbol dump):

```
Game code:     XInputGetState
                  → libgame routes to USB device handle
                  → kernel:USBD_BulkOrInterruptTransfer
                       → kernel:OhciControllerInterruptDispatch
                          → reads HID report from USB endpoint buffer
```

The hook target is **`OhciControllerInterruptDispatch`** (or the
nxdk-equivalent symbol) — the function that copies the HID report
into the buffer the controller-handle's read endpoint hands back.
The hook checks if the requesting endpoint's port has a synthetic
state in `oracle_ctrl_buffer`; if yes, it returns the synthetic HID
report instead of the device's real one.

### Risk inventory (R1–R5)

| ID | Risk | Mitigation |
| -- | ---- | ---------- |
| R1 | Buggy hook → kernel panic, user must power-cycle | Always trampoline to original after our wrapper; never lose a return path. Test against `pipeline-smoke` (CPU-only diag) before any kernel-hooked retail boot. |
| R2 | Kernel symbol address drifts across BIOS / dashboard rebuilds | Resolve at runtime via PE export-table walk against the running kernel image. iND-BiOS-specific build-targeting goes in a config blob; agent reads it at boot. |
| R3 | XInput rumble path (`XInputSetState`) is ALSO routed through USB | Hook the write side too; the synthetic buffer ignores rumble-out (drops it; future: relay to a Mac-side "rumble received" log so the validator can compare). |
| R4 | A real controller plugged in fights the synthetic state | The hook returns the synthetic state for any port where `port[N].seq > 0` (i.e. the agent has touched that port); otherwise it passes through to the real controller. Mac side issues `controller.clear port=N` to "release" a port back to its physical controller. |
| R5 | Kernel hook breaks on agent-relaunch (re-applies on top of itself) | Hook installer checks for a sentinel (e.g. an ORACLE_HOOK_INSTALLED flag at a kernel-pool address); idempotent install. Uninstall happens on `unsafe.disable` or agent rebuild. |

### Why this isn't shipped this session

- Reverse-engineering OHCI symbol locations on this specific iND-BiOS
  build needs a kernel symbol dump that we haven't taken yet.
- The hook needs at least one CPU-painted "controller readback" diag
  XBE to validate against before we point it at a retail game.
- The user's Xbox is the project's only oracle hardware; bricking it
  with a buggy hook would block the entire validation pipeline. Slow
  is fast.

Once a `controller-readback` diag XBE exists (writes to D:\\ what it
reads from XInput, oracle compares against synthetic state we
pre-set), the hook itself can land in a follow-up session with
quantifiable progress: each hook attempt is a 30-second cycle of
"set synthetic state → run readback diag → compare what landed in
D:\\ to what we set".

---

## Tier 3 — Hardware controller emulator (FALLBACK)

**Coverage:** any Xbox, any title, any kernel.
**Risk:** none on the Xbox side; the hardware just plugs in.
**Engineering:** small hardware project + a Mac-side serial driver.

If Tier 2 turns out to be impossible on this kernel (e.g. the OHCI
driver has anti-tamper baked in by iND-BiOS, or the symbol-resolution
keeps drifting), Tier 3 closes the gap with a small hardware adapter:

- **Microcontroller:** Teensy 4.0 (USB Host + USB Device modes,
  ~$30) running the open-source `OGX-Mini` or `MaxLeechXC`
  firmware that emulates an OG Xbox controller (XID HID device).
- **Mac side:** plug the Teensy into the Mac via USB; a
  `controller-replay-hardware.py` translates `controller.set` calls
  into serial commands (Teensy receives, generates the HID report,
  Xbox sees a "real" controller).
- **Xbox side:** plug the Teensy's USB output into one of the four
  Xbox controller ports (via standard USB-to-Xbox adapter cable —
  ~$5).

This is genuinely backward-compatible (the Xbox sees a normal
controller), and entirely independent of any iND-BiOS / dashboard /
kernel state. Trade-off: an additional ~$50 hardware purchase, an
extra serial leg in the validation pipeline, and another physical
device sitting on the project bench.

If we go this route, the same `controller-replay.py` Mac tool covers
both backends: an `--via hardware` flag swaps the agent RPC for
serial.

---

## Recommendation for next session

1. **Build Tier 1 first.** Move the agent's buffer to kernel pool +
   ship `xbed_input_synth` shim + author one validation diag XBE.
   This unblocks the entire diag-XBE library for input-driven
   validation without touching kernel-mode code.
2. **Take a kernel symbol dump from the project Xbox** while Tier 1
   is hot. The agent's `mem.read` already covers the kseg0 RAM image;
   walking the kernel's PE export table is straightforward once the
   loaded base address is known. Capture this proactively so Tier 2
   has the symbol map it needs.
3. **Author a `controller-readback` diag XBE** that exercises XInput
   from the guest side and writes what it read to D:\\. This becomes
   the validation gate for Tier 2.
4. **Defer Tier 2 install** until items 1-3 are green. Slow is fast.
5. **Order the Tier 3 hardware** ONLY if Tier 2 turns out to be
   blocked by something we can't characterize cheaply.

## Cross-references

- Agent protocol surface:
  `scripts/apple-silicon/xbe-tests/oracle-agent/controller.{h,c}`
  (Phase 1, this session).
- Mac replay tool:
  `scripts/apple-silicon/controller-replay.py` (this session).
- Xemu's CSV format reference:
  `ui/xemu-input.c:140-275`.
- Existing CSV library:
  `scripts/apple-silicon/input-scripts/{pgr2,sc2,rainbow,crimson}-*.csv`.
- Xbox kernel boot mechanism (relevant for Tier 2 symbol resolution):
  decision-log "2026-05-06: Real Xbox dashboard swapped XBMC4Gamers
  → UnleashX; iND-BiOS boot mechanism empirically determined".
- Phase-3.0 chainload validation that proves kernel state survives
  `XLaunchXBE`:
  decision-log "2026-05-06: Real Xbox oracle Phase 3.0 —
  pipeline-smoke validates orchestrator end-to-end".
