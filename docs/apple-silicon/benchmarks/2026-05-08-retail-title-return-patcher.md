# 2026-05-08 - Retail Title Return Patcher

Goal: begin the adopted per-title retail-game patching ladder by producing
reproducible return-only XBE probes. These probes replace a verified retail
XBE's entrypoint with a tiny boot stub that waits, calls
`HalReturnToFirmware(HalRebootRoutine)`, and returns to the dashboard without
requiring title assets.

## Tools Added

- `scripts/apple-silicon/retail-title-patcher.py`
  - verifies known source SHA-256 and title ID where available;
  - extends the final XBE section with a 64-byte return stub;
  - marks the patched section preload+executable;
  - redirects the retail entrypoint;
  - writes per-XBE patch metadata.
- `scripts/apple-silicon/retail-title-return-proof.py`
  - uploads a patched XBE to the dashboard via FTP;
  - launches the oracle agent;
  - chainloads the patched XBE through `runxbe`;
  - waits for dashboard FTP to return;
  - writes `verdict.json` evidence.

## Generated Probes

Current regenerated probe set:

```text
benchmark-runs/retail-title-patches/return-only-20260508T033126Z/
```

Targets generated:

- `pgr2`
- `crimson`
- `rainbow`
- `sc2`
- `halo`
- `burnout3`
- `outrun2`

PGR2 sanity inspection after regeneration:

- old entry: `0x00192437`
- new entry: `0x004b43a0`
- patched section: `.XTLID`
- patched section flags: `0x0000003e` (preload + executable, plus existing
  metadata flags)

## Live Attempts

First live proof command:

```sh
scripts/apple-silicon/retail-title-return-proof.py \
  benchmark-runs/retail-title-patches/return-only-20260508T032455Z/pgr2/default.xbe \
  --remote-xbe 'E:\Apps\oracle-patches\pgr2-return\default.xbe'
```

Evidence:

```text
benchmark-runs/retail-return-proof-20260508T032523Z/verdict.json
```

Result: `status=no-dashboard-return`.

The agent acknowledged `runxbe`:

```json
"runxbe_ack": "launching E:\\Apps\\oracle-patches\\pgr2-return\\default.xbe"
```

Dashboard FTP did not return within the proof window, and the Xbox was not
pingable afterward.

Likely root cause: the first generated PGR2 probe placed the entry stub in the
final `.XTLID` section but only set the executable bit, leaving the section
non-preloaded (`0x3c`). Because entrypoint code runs before demand-loading any
late sections, the loader could jump into an unmapped page. The patcher was
fixed to set preload+executable (`flags |= 0x6`), and the probe set was
regenerated at `return-only-20260508T033126Z`.

After manual restart, the regenerated probe set passed live for every target:

| Target | Evidence dir | Patched SHA-256 | Result |
| --- | --- | --- | --- |
| PGR2 | `benchmark-runs/retail-return-proof-20260508T131747Z` | `f1bb8873d24412dd7610012d2b0c3c1981e6581078f4616ee84752a7c2d4a1dc` | `ok` |
| Crimson Skies | `benchmark-runs/retail-return-proof-20260508T131956Z` | `0ffc9d77ed1c4b2578f164bbdf58031d4d9c9d4239d7c1d223835d29a4f649d2` | `ok` |
| Rainbow Six 3 | `benchmark-runs/retail-return-proof-20260508T132051Z` | `3a5f796b2dcb3438b7c94f2d3449215307b39093520c6ac68cb960524a4a8a8f` | `ok` |
| Soul Calibur 2 | `benchmark-runs/retail-return-proof-20260508T132147Z` | `e3551967aef82ffc941f83ee690947bc6aeb7a54611f1188923961c18db52554` | `ok` |
| Halo CE | `benchmark-runs/retail-return-proof-20260508T132246Z` | `8be1fbb9f2f23dc3bde3011949f95cbf1cad8be9ca746caa10f30d84d542ccd5` | `ok` |
| Burnout 3 | `benchmark-runs/retail-return-proof-20260508T132343Z` | `3d7c1cdfca567239d404d87e6dd8e605a7ecd8368a2b858219cae849cb01696a` | `ok` |
| OutRun 2 | `benchmark-runs/retail-return-proof-20260508T132441Z` | `ae3c71e8305194092d30e9f8d9d48ac18abe315874c1a812bce49a6c4ecf4a4b` | `ok` |

## XInput Automation Follow-Up

The patcher now also supports:

```sh
scripts/apple-silicon/retail-title-patcher.py pgr2 --mode input-proof
scripts/apple-silicon/retail-title-patcher.py pgr2 --mode route
```

Input-proof probes were generated for all seven targets under:

```text
benchmark-runs/retail-title-patches/input-proof-20260508T135352Z/
```

The first PGR2 fake-device automation proof failed live:

```text
benchmark-runs/retail-automation-proof-20260508T134127Z/verdict.json
```

Result: `status=fail`, no dashboard FTP return, Xbox down afterward. This
invalidates the current fake-device hook set as an accepted proof. A safer
physical-device-only PGR2 proof was generated at:

```text
benchmark-runs/retail-title-patches/input-proof-20260508T135353Z/pgr2/default.xbe
```

That proof has not been launched yet.
