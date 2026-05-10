# Controller Injection — Feasibility & Design

Last updated: 2026-05-09 (Tier 3 OGX360 bridge bring-up:
**Mac-side byte-exact validated**, slot 2 reflashed for byte-shift
bug, Xbox-side input readback unresolved). Previous header retained
for context: 2026-05-08 retail strategy pivot adopted per-title XBE
patching for the current 5-6 game oracle scope; the v0.3 agent
buffer ABI matches xemu's `ControllerState` byte-for-byte.

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

## Implementation tiers

| Tier | Coverage | Path | Status |
| ---- | -------- | ---- | ------ |
| 1    | Our own diag XBEs | Shared-buffer + `xbed_input_synth` shim | SHIPPED 2026-05-07. Production-validated by `controller-roundtrip`. |
| 2A   | Fixed retail canary set | Per-title XBE patches that synthesize input and return to dashboard | ADOPTED 2026-05-08. Active production path. |
| 2B   | Generic retail games | Kernel/XID hook, validated by SDL/XID readback | PREP TOOLS SHIPPED; live export-slot implementation crashed 2026-05-08. Not production. |
| 3    | Generic / fallback | Hardware controller emulator (Mac → OGX360 → Xbox controller port) | **Mac-side byte-exact PROVEN 2026-05-09**, Xbox-side input readback unresolved. Slot 1 (new USB-C Pro Micro) flashed with our custom master firmware in-place via 1200-baud touch. Slot 2 reflashed with stock Ryzee119 firmware after diagnosing a byte-shift bug in its pre-existing build. `validation/bench-validate.py` 25/25 PASS through every Duke field. **OPEN:** controller-readback XBE detects slot 2 (correct VID/PID, SDL handle) but reports zero input despite bridge sender holding known values. See `scripts/apple-silicon/ogx360-bridge/docs/2026-05-09-bringup-results.md` and the project README's "Next session" section. |

Tiers 1, 2A, and 2B are software paths. Tier 2A does not require a live
agent after `runxbe`; each patched title owns its route playback and
dashboard-return behavior. Tier 3 remains the generic backstop if the
per-title approach stalls.

## 2026-05-08 software-only verdict

There is not a production-ready path to control a retail game after
`runxbe` with the current shipped tooling. The oracle agent is the XBE
that performs `runxbe`; once the retail title starts, the agent TCP
server and live `controller.*` RPC path are gone.

The previously preferred generic software route was a resident
kernel-level shim. Its read-only preflight passed, but the first live
mutating export-slot redirection crashed the project Xbox, including a
jump-only rung. That path is preserved as research, not production.

The accepted production route for the current oracle scope is now
**per-title XBE patching**. The fixed canary set makes title-specific
patches acceptable: each patched retail title should synthesize input
from a route buffer and autonomously return to the dashboard. See
`retail-title-patching-strategy.md` for the target titles, patch
model, and next-session ladder.

Two software shortcuts are now explicitly ruled out for production:

- `LaunchData` can pass data only to cooperating XBEs. Retail games do
  not know our route format, so launch data alone cannot drive input.
- One generic title-level `XInputGetState` patch is not stable enough
  for the full library. Local scans of Halo, Soul Calibur 2, and OutRun
  2 show different static XAPI/library layouts and inconsistent useful
  strings. Per-title patches are acceptable only because the current
  oracle scope is the fixed 5-6 game set.

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

## Tier 2A — Per-title retail XBE patches (ADOPTED 2026-05-08)

**Coverage:** the current fixed retail canary set.
**Risk:** moderate. Patch bugs are title-local and reversible from FTP/HDD
backup; they do not require live kernel export-slot mutation.
**Engineering:** next production task. Full plan:
`retail-title-patching-strategy.md`.

This path accepts that a 5-6 title oracle does not need one universal input
hook on day one. Each target title gets a reproducible Mac-side patcher that
identifies the expected XBE, patches its input-read path or nearby XAPI call
site, and feeds a compact route buffer derived from the existing xemu CSV.

The patch must also own exit. The first proof for each title is a return-only
patch that launches, waits a short interval, calls `HalReturnToFirmware`, and
requires dashboard FTP recovery. Only after that proof passes should the patch
synthesize gameplay input.

The accepted title order is PGR2, Crimson Skies, Rainbow Six 3, Soul Calibur
2, Halo CE, and one broader-sweep sixth title such as OutRun 2 or Burnout 3.

---

## Tier 2B — Kernel hook for generic retail games (RESEARCH ONLY)

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

- The read-only `tier2-shim-preflight.py` rung passed live, but both the
  no-op/counter install and a later jump-only install froze or crashed the
  project Xbox.
- The user's Xbox is the project's only oracle hardware; repeating mutating
  Tier-2 install attempts is not justified for the retail oracle pipeline.
- The hardened Tier-2 commands remain guarded as research artifacts behind
  `confirm=crash-risk-20260508`; production work should not use them.

The hook path is therefore superseded for production by Tier 2A per-title
patching. Keep this section for prior-art context and for any future generic
controller backend effort.

---

## Tier 3 — Hardware controller emulator (STAGED 2026-05-08)

**Coverage:** any Xbox, any title, any kernel.
**Risk:** none on the Xbox side; the hardware just plugs in.
**Engineering:** complete in-tree (firmware + Mac-side replay tool +
docs) — pending hardware bring-up only.

The Tier 3 path is now an OGX360-based bridge rather than a Teensy
build, because the user already had an OGX360 from a previous
project. With the new USB-C Pro Micro arriving 2026-05-09 to replace
the original master Pro Micro (whose micro-USB connector was destroyed
pre-session and could not be rescued), the bridge is functionally a
small custom-firmware spike on top of an existing 4-port board.

**Architecture:**

```
Mac Studio                            OGX360 PCB
  +---------+     USB-C / CDC      +----------------------+
  | xemu CSV| ----- slot 1 ------> | custom master FW      |
  | replay  |   115200 / 8N1      |  (firmware/master/.ino)|
  +---------+                     |       |                |
                                  |       | I²C 400 kHz    |
                                  |       v                |
                                  |  slot 2 slave FW       |
                                  | (Ryzee119, unmodified) |
                                  |       |                |
                                  +-------|----------------+
                                          |
                                          v USB / XID HID
                                  USB-A → OG Xbox port adapter
                                          |
                                          v
                                     Xbox controller port 1
```

**Key design decisions:**

- **Slot 1 firmware is custom; slot 2 firmware is unmodified Ryzee119
  slave.** We replicate the master/slave I²C protocol exactly so the
  unmodified slave receives our state as if it came from a real
  Ryzee119 master. Byte-level protocol spec at
  `scripts/apple-silicon/ogx360-bridge/docs/protocol-analysis.md`.

- **No Teensy purchase, no second microcontroller for the bridge.**
  The OGX360 already has 4 Pro Micro footprints; we only populate
  slot 1 (master) and slot 2 (slave) for the oracle use case.

- **Wire format from Mac to slot 1:** 25-byte serial frame
  `[0xAB][0xCD][PORT 1..3][TYPE 1=DUKE][PAYLOAD 20 bytes][XOR cksum]`.
  PAYLOAD is exactly the Ryzee119 `usbd_duke_in_t` struct — slot 1
  unwraps the serial framing and re-frames for I²C with `0xF1` status
  byte prefix.

- **Mac-side script** is `controller-replay-hardware.py` — same CSV
  vocabulary as the existing agent-RPC `controller-replay.py`. The
  same `scripts/apple-silicon/input-scripts/*.csv` library drives
  both engines. To switch from agent-RPC to hardware-bridge for any
  given route, the user just runs the alternate script.

**In-tree deliverables (2026-05-08 evening):**

- `scripts/apple-silicon/ogx360-bridge/firmware/master/master.ino` —
  custom slot 1 firmware. Compiles against `arduino:avr:leonardo` to
  23% flash, 18% RAM. Plenty of headroom for future enhancements
  (Steel Battalion support, rumble passback to Mac, etc.).
- `scripts/apple-silicon/ogx360-bridge/mac-side/controller-replay-hardware.py`
  — Mac-side replay tool. Frame builder unit-tested against four
  known controller states; CSV parsing validated against the existing
  input-script library.
- `scripts/apple-silicon/ogx360-bridge/docs/protocol-analysis.md` —
  reverse-engineered byte-level I²C protocol spec.
- `scripts/apple-silicon/ogx360-bridge/docs/integration-plan.md` —
  tomorrow's bring-up checklist with pass/fail criteria.
- `scripts/apple-silicon/ogx360-bridge/docs/backup-runbook.md` —
  slot 2 firmware backup procedure (kept for future reference; could
  not run autonomously this session because Caterina bootloader entry
  on slot 2 could not be triggered).

**Status of slot 2 firmware backup:** skipped. The OGX360 onboard
reset button appears to be a power-cycle (cuts VBUS) rather than wired
to the chip's RST pin, and manual short of the slot 2 Pro Micro's
RST/GND pin header pins did not trigger Caterina's stay-in-bootloader
mode either. Acceptable: the slave firmware is GPL-3.0 open source and
reproducible from `vendor/OGX360/` (kept out of git via the bridge's
`.gitignore`) via PlatformIO. The integration plan never reflashes
slot 2, so this is not a tomorrow's blocker — only a future-work
consideration if slot 2 is ever bricked.

**Tomorrow's first bring-up step:** pre-flash the new USB-C Pro Micro
on the bench with `master.ino` (USB-C → Mac directly, factory Caterina
+ arduino-cli upload), verify it enumerates as USB CDC, then solder
into slot 1 of the OGX360. End-to-end smoke test target: Xbox responds
to a single A-button press driven from a `controller-replay-hardware.py`
run with a 2-line CSV.

---

## Recommendation for next session

1. **Treat Tier 1 as closed.** Keep using `oracle-validate.sh` and
   `m15-visual-gate.sh` to prove it stays closed.
2. **Recover the Xbox and run only read-only Tier-2 status checks.** Do not run
   Tier-2 install commands.
3. **Start Tier 2A with PGR2.** Mirror/fingerprint the retail XBE, write a
   reproducible patcher, and preserve the original XBE.
4. **Prove autonomous exit first.** A patched title must return to dashboard
   on its own before gameplay capture. Prefer direct
   `HalReturnToFirmware(HalQuickRebootRoutine)` /
   `HalReturnToFirmware(HalRebootRoutine)` from injected code.
5. **Prove one visible input event.** Only after dashboard return works should
   the patch synthesize a small input sequence.
6. **Run the full route and capture.** Use `retail-gameplay-oracle.py` with
   title-patch input and autonomous-exit evidence.
7. **Repeat the ladder** for Crimson Skies, Rainbow Six 3, Soul Calibur 2,
   Halo CE, and the chosen sixth title.

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
- Current per-title retail strategy:
  `docs/apple-silicon/retail-title-patching-strategy.md`.
- Xbox kernel boot mechanism (relevant for Tier 2 symbol resolution):
  decision-log "2026-05-06: Real Xbox dashboard swapped XBMC4Gamers
  → UnleashX; iND-BiOS boot mechanism empirically determined".
- Phase-3.0 chainload validation that proves kernel state survives
  `XLaunchXBE`:
  decision-log "2026-05-06: Real Xbox oracle Phase 3.0 —
  pipeline-smoke validates orchestrator end-to-end".
