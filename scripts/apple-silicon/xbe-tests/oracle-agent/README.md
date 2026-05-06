# oracle-agent — network-listening Xbox oracle

A small nxdk-built XBE that turns a softmodded retail Xbox into a
network-attached oracle for xemu correctness validation. It speaks a
text-line RPC protocol on TCP port 9001 and replaces the
leaked-XDK-dependent XBDM path that
`docs/apple-silicon/real-xbox-oracle-feasibility.md` originally
proposed (see decision-log "2026-05-06: Real Xbox oracle architecture
pivot — custom oracle agent supersedes XBDM").

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

The committed `bin/default.xbe` (385 KB) is a pre-built binary so
contributors who just want to run the oracle don't need nxdk
installed.

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

# launch
curl -u xbox:xbox \
  --quote 'SITE RunXBE Special://xbmc/Apps/oracle-agent/default.xbe' \
  ftp://192.168.0.200/ -o /dev/null
# wait ~5 seconds for the agent's TCP listener to come up
```

## Protocol

Connect with any line-oriented TCP client (`nc`, `telnet`, Python
socket, ...). The agent greets with `200- xbox-oracle-agent ready`,
then accepts one command per line.

Response codes (XBDM/SMTP-inspired so a streaming line parser works):

- `200- <text>` single-line success
- `201- OK [<count>]` multi-line begin; payload lines follow; ends
  with a line containing exactly `.`
- `500- <text>` error

### Phase 1 commands (this build)

| Command  | Behavior                                              |
|----------|-------------------------------------------------------|
| `info`   | Single-line agent version + console IP                |
| `eeprom` | 256-byte EEPROM as 32-byte hex lines, ends with `.`   |
| `reboot` | Reboot Xbox; agent quits, iND-BiOS chains to dashboard|
| `bye`    | Close connection cleanly                              |

### Phase 2+ (planned, see `docs/apple-silicon/real-xbox-oracle-feasibility.md`)

- `mem.read addr=0xHHHH len=N` / `mem.write addr=… data=hex`
- `nv2a.read off=0xHHHH` / `nv2a.write off=… val=…`
- `screenshot` — front-buffer capture as raw RGBA + W/H header
- `vram.read off=0xHHHH len=N`
- `runxbe path=<xbox-path>` — chainload a different XBE (terminates
  the agent; iND-BiOS chains the named XBE, our diagnostic XBEs
  reboot back to XBMC4Gamers when done)

## Smoke test

```sh
( printf 'info\nbye\n'; sleep 1 ) | nc -w 5 192.168.0.200 9001
# expected:
#   200- xbox-oracle-agent ready
#   200- xbox-oracle-agent v0.1 (...); ip=192.168.0.200
#   200- bye
```

The `eeprom` command's hex output should match
`xbox-oracle-backup/2026-05-06/eeprom/eeprom-fresh.bin` byte-for-byte
for the project's specific Xbox (see RESTORE.md §1 for that console's
identifying values).

## Source layout

- `main.c` — TCP listener loop + command dispatch (single file for
  Phase 1; will split as Phase 2+ commands land)
- `Makefile` — standard nxdk pattern with `NXDK_NET = y` to bring in
  the lwIP TCP stack
- `bin/default.xbe` — pre-built artifact, committed for convenience
