# 2026-05-19 — Retail oracle post-repaste thermal check

User replaced the thermal compound on both the CPU and GPU of the
retail Xbox oracle and confirmed the fan direction is correct. This
note re-checks the live hardware against the 2026-05-12 pre-repaste
baseline and decides whether the Xbox can re-enter service for
development.

Console under test:
- v1.6 retail Xbox with Xyclops SMC version `P2L`
- MAC `00:12:5A:00:5B:CF`
- IP `192.168.0.200`
- Noctua NF-A6x25 FLX rear fan still installed

Reference baseline:
- `benchmarks/2026-05-12-noctua-fan-validation.md`
- Pre-repaste auto idle: 65-67 °C
- Pre-repaste fan=100% steady-state idle: 57 °C after ~7-9 min

## Procedure

1. Verified the Xbox was reachable on the network (`ping` and FTP on
   port 21 succeeded).
2. Confirmed the oracle agent was not already listening on TCP 9001;
   launched it with `oracle-orchestrator.py ensure-agent`.
3. Read `smc.temps` in auto mode.
4. Enabled writes, set `smc.fan val=100`, and recorded the same style
   of cooldown/steady-state trace used on 2026-05-12.
5. Restored `smc.fan val=auto` and took a post-restore read.
6. Pulled raw `smc.read` values for `0x01`, `0x09`, `0x0a`, and `0x10`
   to confirm that the convenience wrapper matched the underlying SMC
   bytes.

## Data

### Baseline after launching the agent

| Phase | Cpu_c | Board_c | Fan mode | Fan_raw_rb |
|---|---:|---:|---|---:|
| Auto idle | 55 | 55 | auto | 10 |

### Manual fan=100% hold

| Phase | Cpu_c | Board_c | Fan mode | Fan_raw_rb |
|---|---:|---:|---|---:|
| +0 s | 55 | 55 | manual | 50 |
| +60 s | 56 | 56 | manual | 50 |
| +180 s | 56 | 56 | manual | 50 |
| +300 s | 56 | 56 | manual | 50 |
| +420 s | 56 | 56 | manual | 50 |

### After restoring auto

| Phase | Cpu_c | Board_c | Fan mode | Fan_raw_rb |
|---|---:|---:|---|---:|
| +0 s after auto restore | 56 | 56 | auto* | 50 |
| +60 s after auto restore | 56 | 56 | auto* | 47 |

`*` `fan_mode` is the agent session state label, not a hardware read of
`FANMODE`; `fan_raw_rb` is the reliable hardware-side fan evidence.

### Raw SMC reads

| Register | Meaning | Value |
|---|---|---|
| `0x01` | SMC version bytes | `0x50 0x32 0x4c` = `P2L` |
| `0x09` | CPU temp register | `0x38` = 56 |
| `0x0a` | Board temp register | `0x38` = 56 |
| `0x10` | Fan speed readback | `0x25` = 37 |

The `0x09` / `0x0a` mirror persists on this board. Treat the readout as
one v1.6/Xyclops thermal signal, not as two independently calibrated
sensors.

## Comparison vs 2026-05-12

| Condition | 2026-05-12 pre-repaste | 2026-05-19 post-repaste |
|---|---:|---:|
| Auto idle | 65-67 °C | 55 °C |
| Fan=100% steady-state | 57 °C | 56 °C |

The repaste improves the auto-idle reading by roughly 10-12 °C. The
fan=100% floor moved only slightly, but the important operational
change is that the box no longer idles in the mid-to-high 60s at the
low fan floor.

## Interpretation

The post-repaste data does **not** support keeping this Xbox blocked on
thermal grounds.

- The read path is direct hardware access through the Xyclops/SMC, not
  a dashboard UI reading.
- The repaste produced a material improvement versus the 2026-05-12
  baseline.
- The remaining mid-50s readout is plausible for a v1.6 board and is
  not, by itself, evidence of an overheating fault.
- No external IR thermometer or thermocouple was available, so this
  note does **not** claim a universally calibrated "true CPU
  temperature" for v1.6 Xboxes. It only establishes that the previous
  hard "too hot to use" interpretation is no longer justified for this
  specific oracle.

## Verdict

**Retail Xbox oracle is available again for development.**

Do not block real-hardware captures solely because `smc.temps` reports
55-56 °C on this v1.6 board. Re-open thermal investigation only if
there are concrete symptoms such as thermal shutdowns, sustained fan-max
behavior, or new traces materially hotter than this one.
