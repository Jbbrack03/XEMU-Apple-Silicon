# 2026-05-12 — Noctua NF-A6x25 FLX fan validation on the retail oracle Xbox

User upgraded the retail-Xbox oracle (v1.6, "P2L" Xyclops SMC, MAC
`00:12:5A:00:5B:CF`, 192.168.0.200) by swapping the stock 60 mm fan
for a Noctua NF-A6x25 FLX (3-pin, plugged directly into the v1.6 fan
header — no adapter or LNA reported). Goal: verify the upgrade is
actually cooling the Xbox.

Headless Xbox, so dashboard temp readout isn't an option. Built
oracle-agent v0.4 with new `smc.*` RPC surface (commit pending) to
read SMC sensors and drive `FANMODE` / `FANSPEED` over TCP 9001.

## Headline finding

**The fan is working but the thermal envelope is bad.** At Noctua-100%
PWM (manual mode, raw=50/50) the Xbox settles at **57 °C M/B steady
state at idle**, asymptote reached after ~7-9 minutes. In SMC
auto-mode the Noctua delivers ~30% PWM and the Xbox stabilizes at
**65-66 °C M/B at idle**. Both numbers are well outside the
Tinker-Mods-FAQ "ideal idle" band of 26-29 °C and the "acceptable
idle" band of 21-35 °C. 57 °C steady-state idle leaves no thermal
headroom for gaming load (which historically adds 10-25 °C on top of
idle), implying thermal-trip risk under retail-oracle gameplay
captures.

**Verdict:** the fan swap is necessary-but-not-sufficient. Curve
tuning alone cannot fix this — even at maximum airflow capability
the Noctua FLX (29.2 m³/h vs the stock fan's 62.2 m³/h) is
insufficient with the current thermal interface state. Recommended
next steps in priority order:

1. Re-paste CPU + GPU heatsinks with modern TIM (MX-4 / NT-H2 /
   Kryonaut). Original 22-year-old TIM is universally degraded;
   community reports 10-15 °C drops from this alone.
2. Compressed-air clean the heatsink fins and the fan duct shroud.
3. Verify the fan-duct seal isn't bypassing airflow around the
   heatsinks (especially if a 3D-printed adapter is in use).
4. Re-measure. If idle is still >40 °C after the above, consider
   replacing the FLX with an NF-A8 PWM (50.4 m³/h — 1.7× the
   airflow, requires Dremel work on the chassis).

## Data

### Codex follow-up validation, 2026-05-12 11:59-12:10 CDT

Codex rechecked the live Xbox after the initial Claude session. The
agent still reported a hot idle:

| Phase                            | Cpu_c | Board_c | Fan mode | Fan_raw_rb |
|----------------------------------|-------|---------|----------|------------|
| Follow-up baseline, idle         |   67  |   67    | auto*    |     10     |
| Manual fan=20                    |   67  |   67    | manual   |     10     |
| Manual fan=40                    |   67  |   67    | manual   |     20     |
| Manual fan=60                    |   67  |   67    | manual   |     30     |
| Manual fan=80                    |   67  |   67    | manual   |     40     |
| Manual fan=100, +0 s             |   67  |   67    | manual   |     50     |
| Manual fan=100, +60 s            |   64  |   64    | manual   |     50     |
| Manual fan=100, +180 s           |   61  |   61    | manual   |     50     |
| Manual fan=100, +300 s           |   59  |   59    | manual   |     50     |
| Manual fan=100, +414 s           |   57  |   57    | manual   |     50     |
| Manual fan=100, +594 s           |   57  |   57    | manual   |     50     |

`smc.fan` mapping is confirmed end-to-end on this Xyclops board:
20/40/60/80/100% produced raw readbacks of 10/20/30/40/50. The
100% cooldown curve independently reproduces the earlier 57 °C idle
asymptote. `*` Caveat: the `fan_mode` text in `smc.temps` is the
agent's session state, not a hardware read of `FANMODE`; use
`fan_raw_rb` as the reliable fan evidence unless a future command adds
a safe hardware `FANMODE` read.

| Phase                            | Cpu_c | Board_c | Fan mode | Fan_raw_rb |
|----------------------------------|-------|---------|----------|------------|
| First read (auto, post-deploy)   |   66  |   66    | auto     |     15     |
| Auto, +30 s                      |   66  |   66    | auto     |     15     |
| Auto, +180 s (mostly steady)     |   65  |   65    | auto     |     16     |
| Manual fan=100, +0 s             |   65  |   65    | manual   |     50     |
| Manual fan=100, +60 s            |   64  |   64    | manual   |     50     |
| Manual fan=100, +120 s           |   62  |   62    | manual   |     50     |
| Manual fan=100, +240 s           |   60  |   60    | manual   |     50     |
| Manual fan=100, +360 s           |   58  |   58    | manual   |     50     |
| Manual fan=100, +480 s           |   57  |   57    | manual   |     50     |
| Manual fan=100, +540 s (asymp.)  |   57  |   57    | manual   |     50     |

Observations / data hygiene notes:

- **CPU sensor (reg 0x09) mirrors board sensor (reg 0x0a)** on every
  read. This is a known Xyclops v1.6 quirk — there is no usable CPU
  on-die thermal diode on this revision, so the SMC reports the
  motherboard thermistor reading for both registers. Both readings
  should be interpreted as a single M/B temperature.
- **SMC version (reg 0x01) reads "P2L"** (0x50, 0x32, 0x4C) confirming
  this is the Xyclops revision, not the earlier PIC16LC found on
  v1.0-1.5 boards. The xemu emulator emulates a PIC16LC
  (`hw/xbox/smbus_xbox_smc.c:52-55`); the fact that FANMODE/FANSPEED
  writes work identically on Xyclops is a useful data point — the
  high-level register interface is compatible across SMC revisions
  (Codex flagged this as unproven from source; verified empirically).
- **`SMC_REG_FANSPEED_RB` (0x10) is implemented on Xyclops P2L** and
  correctly mirrors `FANSPEED` writes. Follow-up testing proved the
  expected raw mapping (10/20/30/40/50 for 20/40/60/80/100%). In
  idle baseline reads it has shown both 15-16 (≈30%) and 10 (20%);
  the latter matches the backed-up BIOS/dashboard config floor
  (`fanSpeed = 10`). Do not treat the auto/floor behavior as fully
  characterized yet. The safe conclusion is narrower: the Noctua
  needs a much higher commanded speed to pull the board down, and
  even raw=50 only reaches ~57 °C idle.
- **PIC challenge handshake is NOT a barrier**: chainloaded XBE made
  `HalWriteSMBusValue(0x20, ...)` calls successfully with no special
  handshake. nxdk's `hal/led.c:16-29` uses the same pattern. Codex's
  open question #2 is resolved negative.
- **FANMODE persistence across reboot is NOT tested** here; the agent
  now auto-restores FANMODE=auto on `cmd_reboot` and `cmd_runxbe` for
  safety.

## Agent v0.4 work in this session

Added under `scripts/apple-silicon/xbe-tests/oracle-agent/`:

- `smc.h` / `smc.c` (new) — SMC RPC handlers with allowlisted reads
  ({0x01, 0x03, 0x04, 0x09, 0x0a, 0x10, 0x1b}) and writes ({0x05, 0x06}
  gated by `unsafe.enable`). `oracle_smc_cleanup_if_manual()` reverts
  FANMODE=auto on agent-exit paths.
- `commands.c` — `cmd_help` extended; `cmd_reboot` and `cmd_runxbe`
  call cleanup hook before exiting. `cmd_bye` deliberately does NOT
  call cleanup (oracle-client.py's polite close would otherwise
  silently revert every caller's fan setting after each command — a
  bug caught and fixed in this session before any real measurement).
- `main.c` — registered `smc.read`, `smc.write`, `smc.temps`, `smc.fan`
  in the dispatch table.
- Makefile — added smc.c to SRCS.
- Bumped VERSION_STR to "v0.4 (Phase 2 + controller.* + smc.*)".

The `smc.temps` convenience command reports CPU + board °C, AV pack,
the agent's last commanded fan mode/percent, and `fan_raw_rb`
(commanded-PWM readback) in one line — designed for cheap polling
loops over a long monitoring window.

## Current state when leaving the session

Fan is at **manual 100%**. The auto-cleanup hook fires only on a
deliberate `reboot` / `runxbe` through the agent; on a hard power
cycle the SMC FANMODE may or may not persist (Xyclops behavior on
P2L not characterized — Codex open question #3). To restore the
SMC's built-in thermal algorithm:

```sh
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
python3 scripts/apple-silicon/oracle-client.py raw unsafe.enable
python3 scripts/apple-silicon/oracle-client.py raw smc.fan val=auto
```

Do not run retail-oracle gameplay routes on this Xbox until the
thermal envelope is resolved — at 57 °C idle, gaming load will push
M/B temps into the 70-80 °C thermal-trip zone, killing capture runs
mid-route. The Tier-3 OGX360 hardware bridge and the per-title
patcher pipelines will continue to develop unblocked because they
don't require this specific Xbox to be in a runnable state.

## Open questions / follow-ups

- **Xyclops fan PWM mapping** — assumed 0..50 → 0..100% per the
  PIC16LC convention. The empirical asymptote of 57 °C at written
  raw=50 is consistent with "max possible fan speed = ~3000 RPM
  Noctua spec", but we haven't independently measured RPM via the
  tach pin (would require either a multimeter on pin 3 or a
  hall-effect probe). Worth verifying if a future session has time.
- **fan_raw_rb idle behavior** — follow-up baseline read raw=10 at
  67 °C, matching the backed-up BIOS/dashboard `fanSpeed = 10` floor.
  Earlier raw=15-16 reads are still real data, but the control source
  (SMC auto curve vs dashboard/BIOS fan floor vs previous session
  state) is not fully characterized.
- **Codex open question #1** (FANMODE/FANSPEED mapping on Xyclops):
  RESOLVED — 0..50 raw works exactly as on PIC16LC.
- **Codex open question #2** (PIC challenge): RESOLVED NEGATIVE —
  no challenge handshake needed for fan writes from chainloaded XBE.
- **Codex open question #3** (FANMODE persistence across reboot):
  STILL OPEN — agent's defensive cleanup-on-exit makes this moot
  for normal use, but a hard power-loss could strand the fan in
  manual. Worth a 30-second test next time someone is at the Xbox.
- **Codex open question #4** (other SMC thermal registers): NF found —
  0x10 (fan readback) is in the read allowlist and works; nothing
  else in the public register map adds thermal info beyond CPU + M/B
  temp.
