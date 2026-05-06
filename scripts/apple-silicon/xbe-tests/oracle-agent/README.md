# oracle-agent — network-listening Xbox oracle

A small nxdk-built XBE that turns a softmodded retail Xbox into a
network-attached oracle for xemu correctness validation. It speaks a
text-line RPC protocol on TCP port 9001 and replaces the
leaked-XDK-dependent XBDM path that
`docs/apple-silicon/real-xbox-oracle-feasibility.md` originally
proposed (see decision-log "2026-05-06: Real Xbox oracle architecture
pivot — custom oracle agent supersedes XBDM").

**Status: Phase 2 shipped 2026-05-06.** All commands (mem/nv2a/vram
read+write, screenshot, runxbe chainload) are live and smoke-tested
against the project Xbox.

## Why not XBDM

We tried the XBDM path on 2026-05-06: edited `ind-bios.cfg`
`DISABLEDM=0`, edited `x2config.ini` `startDebug=1`, uploaded
`xbdm.dll` to `/E/xbdm.dll` (variants from SDK 4361 archive.org). Port
731 stayed closed regardless. Most likely cause: the iND-BiOS
revision flashed on the project's OpenXenium predates the BFM 5004.67
build that added documented `DISABLEDM`-driven debug-monitor
loading. Without TV access we cannot read the boot banner;
brute-forcing through later iND-BiOS images would risk a non-
recoverable flash.

The custom agent has no dependency on a debug BIOS, no Microsoft
proprietary code, ships its source under our control, and is
extensible to any oracle command the project actually needs. The
trade-off (lost in exchange) is XBDM-protocol compatibility with
Visual Studio / Xbox Neighborhood — but no project workflow uses
those anyway.

## Build

Standard nxdk build. Activate manually if needed (Apple Silicon
needs `lld` from Homebrew — see project root CLAUDE.md):

```sh
brew install lld   # one-time, if not already
export NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk
export PATH=/opt/homebrew/opt/lld/bin:/opt/homebrew/opt/llvm/bin:$NXDK_DIR/bin:$PATH
cd scripts/apple-silicon/xbe-tests/oracle-agent
make
# bin/default.xbe is the artifact
```

The committed `bin/default.xbe` (~390 KB) is a pre-built binary so
contributors who just want to run the oracle don't need nxdk
installed.

## Source layout

Phase 2 split the source across multiple files. `commands.c` runs
~370 lines because each Phase 2 command is small enough that a
further split would be more friction than help; the rest of the
files are well under the project's ~200-line guideline. If new
commands push commands.c past ~600 lines, split into
`cmds_mem.c` / `cmds_visual.c` / `cmds_launch.c`.

| File         | Scope                                                          |
| ------------ | -------------------------------------------------------------- |
| `main.c`     | Entry point, network bring-up, accept loop, command dispatch   |
| `protocol.h` | Wire-protocol API: `op_send_*`, `op_parse_*`, range checks     |
| `protocol.c` | Implementations of the protocol helpers                        |
| `commands.h` | Per-command handler declarations                               |
| `commands.c` | Phase 1 + Phase 2 command implementations + write-gate state   |

Adding a new command means: extend `cmd_entry s_cmds[]` in `main.c`,
declare the handler in `commands.h`, implement in `commands.c`.

## Deploy & run

This assumes a softmodded Xbox running XBMC4Gamers with FTP enabled
(`xbox`/`xbox`) and `SITE RunXBE` available (XBMC FileZilla 1.5.6's
default).

```sh
# upload (one-time)
curl -u xbox:xbox \
  --quote 'CWD /E/XBMC4Gamers/Apps' \
  --quote 'MKD oracle-agent' \
  ftp://192.168.0.200/ -o /dev/null
curl -u xbox:xbox -T bin/default.xbe \
  ftp://192.168.0.200/E/XBMC4Gamers/Apps/oracle-agent/default.xbe

# launch (FTP connection drops as the kernel chainloads — that's normal)
curl -u xbox:xbox \
  --quote 'SITE RunXBE Special://xbmc/Apps/oracle-agent/default.xbe' \
  ftp://192.168.0.200/ -o /dev/null

# wait ~5 seconds for the agent's TCP listener to come up, OR use
# the Mac client's built-in poller:
python3 ../../oracle-client.py wait-ready --retries 30 --delay 1

# OR drive the whole launch through the orchestrator (idempotent):
python3 ../../oracle-orchestrator.py ensure-agent
```

## Protocol

Connect with any line-oriented TCP client (`nc`, `telnet`, Python
socket, ...). The agent greets with `200- xbox-oracle-agent ready`,
then accepts one command per line.

Response codes (XBDM/SMTP-inspired so a streaming line parser works):

- `200- <text>` single-line success
- `201- OK [<count>]` multi-line text payload begin; lines follow;
  ends with a line containing exactly `.`
- `202- BINARY <length>` length-prefixed binary payload; exactly
  `<length>` raw bytes follow (no trailer; client reads exactly
  that many)
- `500- <text>` error

### Phase 1 commands

| Command  | Behavior                                              |
|----------|-------------------------------------------------------|
| `info`   | Single-line agent version + console IP + video mode + writes_enabled |
| `eeprom` | 256-byte EEPROM as 32-byte hex lines (201-)           |
| `reboot` | Reboot Xbox; agent quits, iND-BiOS chains to dashboard|
| `bye`    | Close connection cleanly                              |
| `help`   | Multi-line list of all commands                       |

### Phase 2 commands

| Command                              | Behavior                                       |
| ------------------------------------ | ---------------------------------------------- |
| `mem.read addr=0xHEX len=N`          | Binary memory read; returns 202- BINARY        |
| `mem.write addr=0xHEX data=<hex>`    | Memory write; gated by `unsafe.enable`         |
| `nv2a.read off=0xHEX`                | One 32-bit NV2A BAR0 register (200- 0xVALUE)   |
| `nv2a.write off=0xHEX val=0xHEX`     | Write 32-bit BAR0 register; gated              |
| `vram.read off=0xHEX len=N`          | Read NV2A 0xF0000000 VRAM aperture; 202-       |
| `screenshot`                         | Front-buffer capture; 202- BINARY <total>      |
| `runxbe path=<xbox-path>`            | Chainload another XBE; agent terminates        |
| `unsafe.enable`                      | Arm `mem.write` + `nv2a.write` for the session |

#### Memory address allowlist

`mem.read` and `mem.write` validate the address range against a
fixed RAM-only allowlist (the four canonical Xbox RAM aliases):

| Range                              | Description                            |
| ---------------------------------- | -------------------------------------- |
| `0x00000000` – `0x03FFFFFF`        | Physical RAM (low alias)               |
| `0x80000000` – `0x83FFFFFF`        | kseg0 — RAM cached mirror              |
| `0xB0000000` – `0xB3FFFFFF`        | kseg1 — RAM uncached mirror            |
| `0xF0000000` – `0xF3FFFFFF`        | NV2A "VRAM" write-combined aperture    |

MMIO regions (NV2A BAR0, APU, ACI, USB) are intentionally **NOT**
in the allowlist. Generic byte-wide `memcpy`/`netconn_write` over
MMIO produces unaligned and split-bus accesses with side effects
that the device side may not tolerate. For NV2A BAR0 use the typed
`nv2a.read` / `nv2a.write` commands, which always do 32-bit
aligned access. APU/ACI/USB are out-of-scope for the oracle today.

Reads/writes outside the allowlist return `500- addr range ... not
in allowlist`. Lengths are capped at 1 MiB per request; writes
capped at 1024 bytes.

#### Screenshot binary format

The 202- payload starts with a 16-byte header followed by raw
pixels:

| Offset | Type | Meaning                                            |
| -----: | ---- | -------------------------------------------------- |
| 0      | u32  | magic `'XOSS'` = 0x53534F58                        |
| 4      | u32  | width  (le)                                        |
| 8      | u32  | height (le)                                        |
| 12     | u32  | stride (le, bytes per scan line)                   |
| 16     | …    | `stride * height` bytes of pixels                  |

Pixel format is the active video mode's native layout — typically
32-bit X8R8G8B8 little-endian (`B G R X` in memory). Convert to
RGBA + PNG on the client with `oracle.bgrx_to_rgba()` +
`save_screenshot_png()` (or just `oracle-client.py screenshot --out
file.png`).

The agent calls `XVideoWaitForVBlank()` before sampling the
front-buffer to minimize tearing; the snapshot is still
single-frame, so a fast scan-line race remains theoretically
possible.

#### Write gating

`mem.write` and `nv2a.write` require an `unsafe.enable` arm command
once per process lifetime:

```
$ python3 oracle-client.py mem-write 0x80020000 deadbeef
oracle error: writes disabled — call unsafe.enable first
$ python3 oracle-client.py unsafe-enable
writes enabled for this session
$ python3 oracle-client.py mem-write 0x80020000 deadbeef
wrote 4 bytes at 0x80020000
```

Note that the wire-protocol verb is `unsafe.enable` (with a dot)
but the Python CLI subcommand is `unsafe-enable` (with a hyphen)
because argparse subcommand names cannot contain dots. The agent
sees `unsafe.enable` over the wire either way.

The arm flag is process-global, not per-connection; it survives
across multiple TCP connections to the same agent invocation. It
resets on agent restart (any `reboot` / `runxbe` / power-cycle).

#### `runxbe` lifecycle

`runxbe path=<xbox-path>` calls `XLaunchXBE(path)` after acking the
client. The agent terminates and the kernel chainloads the named
image. To return to a state where the agent is listening again:

1. The chainloaded XBE finishes its work and either returns to
   firmware (typically rebooting back to the dashboard) or exits in
   any other way that leaves XBMC4Gamers running.
2. The Mac orchestrator polls FTP/21 for the dashboard to come back.
3. The orchestrator `SITE RunXBE`s the agent again.

Step 3 is required because XBMC4Gamers has no auto-launch concept —
nothing on the Xbox side relaunches the agent automatically.
`oracle-orchestrator.py run-diag` automates this whole cycle.

## Smoke test

```sh
( printf 'info\nbye\n'; sleep 1 ) | nc -w 5 192.168.0.200 9001
# expected:
#   200- xbox-oracle-agent ready
#   200- xbox-oracle-agent v0.2 (Phase 2); ip=192.168.0.200; mode=640x480@32bpp; writes_enabled=0
#   200- bye

# Or via the Mac CLI:
python3 ../../oracle-client.py info
python3 ../../oracle-client.py screenshot --out /tmp/shot.png
```

The `eeprom` command's hex output should match
`xbox-oracle-backup/2026-05-06/eeprom/eeprom-fresh.bin` byte-for-byte
for the project's specific Xbox (see RESTORE.md §1 for that console's
identifying values). The Phase 2 in-tree counterpart is
`oracle-client.py eeprom --out /tmp/eeprom-via-agent.bin` followed
by `shasum -a 256 /tmp/eeprom-via-agent.bin xbox-oracle-backup/.../eeprom-fresh.bin`.

## Known issue: connection-cycle hang

During initial Phase 2 smoke testing (2026-05-06) the agent stopped
responding to new TCP connections after ~12 fast connect/RST cycles.
ARP still saw the console at the Ethernet layer; ICMP/TCP both
silent. Recovery required a physical power-cycle.

Hypothesis: the Mac client's pre-Phase-2 `__exit__` did a hard
`socket.close()` without sending FIN, leaving lwIP PCBs in
CLOSE_WAIT until the small PCB pool exhausted. Hardening
2026-05-06:

1. `OracleClient.close()` now sends `bye` and a clean shutdown
   before closing the socket.
2. `cmd_mem_write`'s 2 KB scratch buffer moved off the stack to
   reduce per-conn pressure.

If the hang reproduces despite the hardening: capture
`netconn_recv` errors via the agent's debugPrint output and
investigate lwIP `MEMP_NUM_TCP_PCB` / `MEMP_NUM_TCP_PCB_LISTEN`
pool sizes.