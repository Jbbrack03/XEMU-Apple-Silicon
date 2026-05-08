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
| 1    | Our own diag XBEs | Shared-buffer + `xbed_input_synth` shim | SHIPPED 2026-05-07. Production-validated by `controller-roundtrip`. |
| 2    | Retail games | Kernel/XID hook, validated by SDL/XID readback | PREP TOOLS SHIPPED 2026-05-07; hook NOT YET implemented. |
| 3    | Generic / fallback | Hardware controller emulator (Mac → Xbox port) | Documented as alternative. Hardware project (Teensy + open-source firmware), out of scope for the current toolchain. |

Tiers 1 and 2 are both software-only and use the same agent-side state
buffer. Tier 3 is a hardware backstop if Tier 2 turns out to be too
risky on a particular kernel / dashboard combination.

## 2026-05-07 software-only verdict

There is not a production-ready path to control a retail game after
`runxbe` with the current shipped tooling. The oracle agent is the XBE
that performs `runxbe`; once the retail title starts, the agent TCP
server and live `controller.*` RPC path are gone.

The viable software-only route remains Tier 2: install a resident
kernel-level shim before launch, keep its code/data outside the agent
XBE image, and have it feed the title-facing XInput/XID report path
from the already-proven persistent controller buffer. See
`retail-gameplay-software-paths.md` for the full matrix and required
readback proof. See `tier2-kernel-shim-viability.md` for the prior-art
survey and the current decision to start from NKPatcher's
`KeRaiseIrqlToDpcLevel` export-slot hook.

Two software shortcuts are now explicitly ruled out for production:

- `LaunchData` can pass data only to cooperating XBEs. Retail games do
  not know our route format, so launch data alone cannot drive input.
- Generic title-level `XInputGetState` patching is not stable enough
  for the oracle. Local scans of Halo, Soul Calibur 2, and OutRun 2
  show different static XAPI/library layouts and inconsistent useful
  strings.

---

## Tier 1 — Shared-buffer + diag-XBE shim (SHIPPED 2026-05-07)

**Coverage:** any diagnostic XBE we author (`xbe-tests/<id>/`).
**Risk:** very low. No kernel patching, no USB stack work; the diag
XBE simply reads memory we already own.
**Engineering:** complete. See `oracle-workflow.md` and
`automation.md` for the production validation commands.

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

### Shipped implementation

The agent now owns a persistent kernel-pool `oracle_ctrl_buffer`
allocated with `MmAllocateContiguousMemoryEx` +
`MmPersistContiguousMemory`. The physical address is published through
`controller.buffer-info` and the persisted anchor at
`E:\Apps\oracle-agent\state\ctrl-addr.txt`.

Diag XBEs link `xbe-tests/lib/xbed_input_synth.{h,c}` through
`lib.mk`, call `xbed_input_synth_attach()`, and read the latest
seq-stamped state via `xbed_input_synth_read()`.

The `controller-roundtrip` diag XBE validates the full path:

1. Mac side sets a non-zero synthetic controller state through the
   oracle agent.
2. The agent writes that state into the persistent kernel-pool buffer.
3. The oracle chainloads `controller-roundtrip`.
4. The diag maps the buffer through the anchor file, reads the state,
   and renders a math-comparable frame.
5. `oracle-validate.sh` verifies the rendered frame is byte-exact
   against the expected state.

Production evidence as of 2026-05-07:

- `oracle-validate-20260507T182615Z`: full validation run, including
  10/10 stress.
- `oracle-validate-20260507T194408Z`: clean post-fix composite run,
  including smoke, visual matrix, controller-roundtrip, and seqlock.

After this, every new diag XBE can opt into synthetic-input gating
with no extra agent work.

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

## Tier 2 — Kernel hook for retail games (PREP TOOLS SHIPPED; HOOK FUTURE WORK)

**Coverage:** every retail Xbox game.
**Risk:** moderate-to-high. A buggy hook crashes the kernel, and
the user has to physically power-cycle the Xbox.
**Engineering:** safe readback/symbol tools are now in-tree; the unsafe hook
still needs careful implementation and must pass the readback diag before any
retail title is launched.

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

The reverse-engineering map (rough, now backed by local tooling for
verification against nxdk's xboxkrnl.h ordinal table + a kernel-symbol dump):

```
Game code:     XInputGetState
                  → libgame routes to USB device handle
                  → kernel:USBD_BulkOrInterruptTransfer
                       → kernel:OhciControllerInterruptDispatch
                          → reads HID report from USB endpoint buffer
```

The original hook hypothesis was **`OhciControllerInterruptDispatch`**
(or the nxdk-equivalent symbol) — the function that copies the HID
report into the buffer the controller-handle's read endpoint hands
back. The prior-art pass changed the ranking: this console's live
kernel export dump exactly matches NKPatcher's `patcher_5838` IGR
recipe, so the primary candidate is now the NKPatcher-style
`KeRaiseIrqlToDpcLevel` export-slot hook at `0x800104e8`. That boundary
has prior art for observing retail-game controller state and should be
tested before deeper USB/OHCI internals. `IofCompleteRequest` and lower
OHCI/XID routines remain fallbacks if the NKPatcher boundary misses
required titles.

### Risk inventory (R1–R5)

| ID | Risk | Mitigation |
| -- | ---- | ---------- |
| R1 | Buggy hook → kernel panic, user must power-cycle | Always trampoline to original after our wrapper; never lose a return path. Test against `pipeline-smoke` (CPU-only diag) before any kernel-hooked retail boot. |
| R2 | Kernel symbol address drifts across BIOS / dashboard rebuilds | Resolve at runtime via PE export-table walk against the running kernel image. iND-BiOS-specific build-targeting goes in a config blob; agent reads it at boot. |
| R3 | XInput rumble path (`XInputSetState`) is ALSO routed through USB | Hook the write side too; the synthetic buffer ignores rumble-out (drops it; future: relay to a Mac-side "rumble received" log so the validator can compare). |
| R4 | A real controller plugged in fights the synthetic state | The hook returns the synthetic state for any port where `port[N].seq > 0` (i.e. the agent has touched that port); otherwise it passes through to the real controller. Mac side issues `controller.clear port=N` to "release" a port back to its physical controller. |
| R5 | Kernel hook breaks on agent-relaunch (re-applies on top of itself) | Hook installer checks for a sentinel (e.g. an ORACLE_HOOK_INSTALLED flag at a kernel-pool address); idempotent install. Uninstall happens on `unsafe.disable` or agent rebuild. |

### Shipped Tier-2 prep tools

- `scripts/apple-silicon/xbe-tests/controller-readback/` — nxdk SDL XBE that
  reads the normal title-facing controller path and writes
  `D:\controller-readback.txt` + `D:\controller-readback-done.txt`.
- `scripts/apple-silicon/controller-readback-validate.py` — Mac-side runner
  that chainloads the readback XBE via the oracle orchestrator, mirrors
  artifacts, parses key/value output, and can enforce `--expect key=value`
  checks once a synthetic hook exists.
- `scripts/apple-silicon/xbox-kernel-symbol-dump.py` — safe PE export dumper
  for the running Xbox kernel. It probes only known base candidates and export
  tables through `oracle-client.py mem.read`; it does not scan RAM.
- `scripts/apple-silicon/xbox-kernel-export-annotate.py` — joins that
  ordinal-only dump with nxdk's `xboxkrnl.exe.def`; current evidence names
  366/366 exports and confirms useful primitives such as
  `MmMapIoSpace@12` and `HalReturnToFirmware@4`.
- `scripts/apple-silicon/xbe-inspect.py` — read-only XBE scanner for title,
  certificate, library, thunk, debug-path, and key input/launch strings. It
  documents why generic retail title patching is a research path, not the
  default oracle backend.
- `scripts/apple-silicon/retail-oracle-smoke.py` — production retail-game
  smoke gate. It refuses to launch a retail XBE unless the run has evidence
  for both a title-facing input backend and an autonomous return-to-dashboard
  backend. A blocked result is the correct result until Tier 2 or Tier 3 is
  proven.
- `scripts/apple-silicon/retail-gameplay-oracle.py` — guarded launch →
  route → capture → exit runner. It is intentionally blocked by evidence
  gates until Tier 2 or another title-facing backend is proven.
- `docs/apple-silicon/retail-gameplay-software-paths.md` — definitive
  software-only control matrix and Tier-2 acceptance test.
- `scripts/apple-silicon/tier2-shim-analyze.py` — read-only analyzer that
  cross-checks the live kernel export dump against NKPatcher IGR recipes.
  Current result: this console matches `patcher_5838` with
  `KeRaiseIrqlToDpcLevel` export-slot VA `0x800104e8`.
- `docs/apple-silicon/tier2-kernel-shim-viability.md` — Tier-2 prior-art
  survey, candidate ranking, and proof ladder.

Exit may not need to be expressed as controller input once Tier 2 exists. The
running kernel exports `HalReturnToFirmware@4`, and nxdk exposes
`HalQuickRebootRoutine` / `HalRebootRoutine`; a resident shim can test that as
an autonomous dashboard-return path before combining it with input injection.

The readback XBE uses nxdk SDL's Xbox controller backend. That is not the exact
static Microsoft `XInputGetState` symbol a retail game links, but it is the
right safe preflight for the USB/XID path before patching any kernel or title
code.

### Why the hook itself is not shipped yet

- The next live step is a read-only `tier2-shim-preflight.py` run to verify
  the `0x800104e8` export slot still contains the expected unhooked value.
- The first hook must be no-op/counter only. It must prove the
  `KeRaiseIrqlToDpcLevel` boundary is active during `controller-readback`
  before any state mutation is attempted.
- The user's Xbox is the project's only oracle hardware; bricking it
  with a buggy hook would block the entire validation pipeline. Slow
  is fast.

The hook itself can now land with quantifiable progress: each attempt is a
short cycle of "set synthetic state → run readback diag → compare what landed
in `D:\` to what we set".

---

## Tier 3 — Hardware controller emulator (FALLBACK / DEFERRED)

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

1. **Treat Tier 1 as closed.** Keep using `oracle-validate.sh` and
   `m15-visual-gate.sh` to prove it stays closed.
2. **Run the live Tier-2 preflight** with the oracle agent online:
   `tier2-shim-preflight.py` must read `0x800104e8` and observe
   little-endian `0x00003d04`.
3. **Implement only a no-op/counter hook first.** It should use the
   NKPatcher-style `KeRaiseIrqlToDpcLevel` export-slot boundary, refuse to
   install if preflight fails, tail-jump to the original path, and expose a
   counter/ring-buffer artifact.
4. **Prove the no-op hook with `controller-readback`.** Require no crash and a
   non-zero hook/context counter before any input mutation.
5. **Implement the synthetic `XINPUT_STATE` override** only after item 4 is
   green. The acceptance test is synthetic controller buffer state showing up
   in `controller-readback.txt`.
6. **Order Tier 3 hardware only if this boundary misses required retail
   titles** or proves too brittle to characterize cheaply.

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
