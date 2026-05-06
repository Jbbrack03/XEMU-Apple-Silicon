# eeprom-dump — one-shot EEPROM capture XBE

A small nxdk-built XBE that reads the 256-byte Xbox EEPROM via
SMBus address 0x54 (8-bit form 0xA8), dumps it to disk, and reboots.
Used once per console at oracle-pipeline setup time to capture the
HDD-locking key before any further work that risks losing it (see
`docs/apple-silicon/real-xbox-oracle-feasibility.md` and project
root `RESTORE.md` for that Xbox).

## Build

Same as `../oracle-agent/`; see that README for nxdk + lld setup.

```sh
make
# bin/default.xbe is the artifact (~110 KB)
```

## Deploy & run

The output files (`eeprom-fresh.bin`, `eeprom-info.txt`) are
**per-console secrets** — they include the online key and the HMAC
material that derives the HDD-locking key. **Never** pull them into
the repo tree; always pull them to a path outside the repo. The
example below uses an absolute path under
`/Users/jbbrack03/XEMU_MacOS/xbox-oracle-backup/<date>/eeprom/`,
matching the convention in that backup tree's `RESTORE.md`. Adjust
to wherever you keep your oracle-backup tree, but always outside
this repo.

```sh
# upload (creates the Apps subdir if missing)
curl -u xbox:xbox \
  --quote 'CWD /E/XBMC4Gamers/Apps' \
  --quote 'MKD eeprom-dump' \
  ftp://192.168.0.200/ -o /dev/null
curl -u xbox:xbox -T bin/default.xbe \
  ftp://192.168.0.200/E/XBMC4Gamers/Apps/eeprom-dump/default.xbe

# launch (XBE runs, writes 3 files, sleeps 5s, reboots)
curl -u xbox:xbox \
  --quote 'SITE RunXBE Special://xbmc/Apps/eeprom-dump/default.xbe' \
  ftp://192.168.0.200/ -o /dev/null
# wait ~30s for reboot back to XBMC4Gamers

# pull artifacts to an OUTSIDE-THE-REPO path
DEST=/Users/jbbrack03/XEMU_MacOS/xbox-oracle-backup/$(date -u +%F)/eeprom
mkdir -p "$DEST"
curl -u xbox:xbox -o "$DEST/eeprom-fresh.bin" \
  ftp://192.168.0.200/E/XBMC4Gamers/Apps/eeprom-dump/eeprom-fresh.bin
curl -u xbox:xbox -o "$DEST/eeprom-info.txt" \
  ftp://192.168.0.200/E/XBMC4Gamers/Apps/eeprom-dump/eeprom-info.txt
[ "$(stat -f%z "$DEST/eeprom-fresh.bin")" = "256" ] || echo "WARN: dump not 256 bytes"
```

This directory's `.gitignore` also lists `eeprom-fresh.bin`,
`eeprom-info.txt`, and `xbe-ran-marker.txt` as a defense-in-depth
layer: if anyone runs the recipe with `-O` (writes to CWD) from
inside this directory anyway, git will still skip them.

## Output

The XBE writes to `D:\` (auto-mapped to its launch directory by the
kernel — drive letters other than `D:` are *not* auto-mapped, hence
this approach instead of `E:\xbox-oracle\`):

| File                    | Bytes | Meaning                                              |
|-------------------------|-------|------------------------------------------------------|
| `xbe-ran-marker.txt`    | 37    | Liveness marker — proves the XBE actually executed   |
| `eeprom-fresh.bin`      | 256   | Raw EEPROM (RC4-encrypted region preserved)          |
| `eeprom-info.txt`       | ~1900 | Decrypted identifying info + 16-byte-per-line hex dump|

The decrypted info comes from `ExQueryNonVolatileSetting`:
serial number, MAC address, online key, AV/game region, video setting,
audio setting, DVD region, language. Format matches the standard
EvoX/UnleashX `EEPROM.bin` + `XBOX INFO.txt` pair so existing tools
(xboxhdm, c-xboxtool, ConfigMagic) can use the dump directly for
HDD-key derivation if recovery is ever needed.

## Why this is irreplaceable

Each console's EEPROM contains the per-console HMAC-SHA1 key that
derives the HDD unlock key. Without it, that console's HDD is
unreadable on any other unit. The fresh dump is the *only* record
of this key for any never-previously-backed-up console (the project
Xbox MAC `00:12:5A:00:5B:CF` was confirmed unique vs the user's three
historical EEPROM backups on 2026-05-06).
