# pipeline-smoke — orchestrator pipeline validator

A minimal CPU-painted diagnostic XBE whose only purpose is to
**prove the orchestrator's `run-diag` chainload-and-back cycle
works end-to-end** before any real NV2A-pipeline diagnostic XBE
gets written.

It is intentionally **Tier-4** in the
`docs/apple-silicon/diagnostic-xbe-plan.md` taxonomy: pixels are
written directly to the front-buffer via CPU `memcpy`, NOT via the
NV2A pgraph pipeline. So this XBE does NOT test renderer behavior
— it tests pipeline plumbing.

## What it does

1. `XVideoSetMode(640, 480, 32, REFRESH_DEFAULT)`.
2. `XVideoGetFB()` returns the kernel-published front-buffer
   pointer (cacheable system RAM, 1:1 mapped).
3. CPU-paint the test pattern: every pixel `0xFF000000` (opaque
   black) except pixel `(320, 50)` which is `0xFFFFFFFF` (opaque
   white). Single-pixel oracle, byte-deterministic.
4. `XVideoFlushFB()` (sfence) so the display engine sees a
   coherent frame.
5. Sleep 1.5 s so a human at the TV can spot-check.
6. Write the framebuffer bytes to `D:\pipeline-smoke-capture.bin`
   in **XOSS format** (the same 16-byte header + raw BGRX pixels
   layout that the oracle agent's `screenshot` command uses, so
   `oracle-client.bgrx_to_rgba()` decodes it directly).
7. Write `D:\pipeline-smoke-done.txt` as a liveness marker.
8. `HalReturnToFirmware(HalRebootRoutine)` — warm-reset back to
   the dashboard so the orchestrator can relaunch the agent and
   FTP-pull the artifacts.

`D:\` is the kernel-auto-mapped XBE-parent directory; with the
XBE deployed at `E:\XBMC4Gamers\Apps\pipeline-smoke\default.xbe`,
both files land at
`E:\XBMC4Gamers\Apps\pipeline-smoke\pipeline-smoke-*` and are
FTP-accessible after the reboot.

## Build

```sh
brew install lld   # one-time
export NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk
export PATH=/opt/homebrew/opt/lld/bin:/opt/homebrew/opt/llvm/bin:$NXDK_DIR/bin:$PATH
make
# bin/default.xbe is the artifact (~110 KB)
```

## Deploy + drive end-to-end

```sh
# (one-time) make sure the agent is on the Xbox
# (per scripts/apple-silicon/xbe-tests/oracle-agent/README.md)

# upload the diag XBE
curl -u xbox:xbox --quote 'CWD /E/XBMC4Gamers/Apps' \
  --quote 'MKD pipeline-smoke' ftp://192.168.0.200/ -o /dev/null
curl -u xbox:xbox -T bin/default.xbe \
  ftp://192.168.0.200/E/XBMC4Gamers/Apps/pipeline-smoke/default.xbe

# run the full chainload + collect cycle
python3 ../../oracle-orchestrator.py run-diag \
  --xbe   'E:\XBMC4Gamers\Apps\pipeline-smoke\default.xbe' \
  --ftp-collect /E/XBMC4Gamers/Apps/pipeline-smoke \
  --out   ../../../../benchmark-runs/pipeline-smoke-run
```

The orchestrator prints a status line per phase and emits
`benchmark-runs/.../verdict.json` with timestamps + pulled
artifact paths.

## Validate against the math-derived expected

```sh
python3 - <<'PY'
import struct, importlib.util, sys
spec = importlib.util.spec_from_file_location(
    "oracle_client",
    "../../oracle-client.py",
)
oc = importlib.util.module_from_spec(spec); spec.loader.exec_module(oc)

data = open("../../../../benchmark-runs/pipeline-smoke-run/artifacts/pipeline-smoke-capture.bin", "rb").read()
assert data[:4] == b"XOSS"
w, h, stride = struct.unpack_from("<III", data, 4)
rgba = oc.bgrx_to_rgba(data[16:], w, h, stride)
oc.save_screenshot_png(rgba, w, h, "captured.png")

sys.path.insert(0, ".")
import expected as exp
print("PASS" if rgba == exp.default() else "FAIL")
PY
```

A clean run produces a `captured.png` whose SHA-256 matches
`docs/apple-silicon/xbox-real-references/pipeline-smoke/real-xbox.png`
byte-for-byte.

## Why this isn't a renderer test

Real renderer testing happens through the NV2A pgraph pipeline
(vertex shaders + fragment shaders + pipeline state). This XBE
sidesteps all of that — it pokes the framebuffer with CPU
`memcpy`. So it cannot:

- Catch the SC2 top-mirrored-to-bottom symptom (that's a vertex
  shader / viewport bug).
- Catch the SC2 wrong-colors symptom (that's a vertex format
  decode + RT format bug).
- Catch the SC2 floor-disappearing symptom (that's a depth-test
  bug).

The Tier-1 NV2A diag XBEs (`mirror`, `color-channel`,
`depth-floor`, …) listed in `docs/apple-silicon/diagnostic-xbe-plan.md`
are what catch those. They build on the same shared
`xbed_runtime` skeleton this XBE established the chainload
plumbing for.
