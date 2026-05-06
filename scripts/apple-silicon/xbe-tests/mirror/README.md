# mirror — pixel-position oracle (Tier-1)

What it does: renders a 4x4 white block at window-coords (318, 48)-(322, 52)
on an opaque-black back-buffer, swaps, captures the post-flip front
buffer to D:\mirror-capture.bin, writes a D:\mirror-done.txt liveness
marker, and reboots back to the dashboard.

Catches: Y-mirror bugs in the renderer or front-buffer publisher
(the SC2 "top-mirrored-to-bottom" symptom — a Y-mirror would put
the white block at row 429 instead of row 50).

## Build

```sh
eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && \
    make
```

Output: `bin/default.xbe` (~143 KB).

## Run on the real Xbox via the orchestrator

```sh
# 1. Deploy
curl -u xbox:xbox -T bin/default.xbe \
    ftp://192.168.0.200/E/XBMC4Gamers/Apps/mirror/default.xbe

# 2. Chainload + collect (orchestrator handles agent ↔ XBE handover)
python3 ../../oracle-orchestrator.py run-diag \
    --xbe 'E:\XBMC4Gamers\Apps\mirror\default.xbe' \
    --ftp-collect /E/XBMC4Gamers/Apps/mirror \
    --out /tmp/mirror-real-xbox

# 3. Decode the captured XOSS to PNG
python3 -c "
import importlib.util as i, pathlib as p
oc = i.module_from_spec(s := i.spec_from_file_location('oc', '../../oracle-client.py')); s.loader.exec_module(oc)
data = open('/tmp/mirror-real-xbox/artifacts/mirror-capture.bin','rb').read()
import struct
w,h,stride = struct.unpack_from('<III', data, 4)
rgba = oc.bgrx_to_rgba(data[16:], w, h, stride)
oc.save_screenshot_png(rgba, w, h, '/tmp/mirror-real-xbox/mirror.png')
print('wrote /tmp/mirror-real-xbox/mirror.png')
"
```

## Run on xemu

The `xbe-harness` orchestration layer (sibling to this directory)
provides the per-renderer driver. Manual single-run:

```sh
# Generate the math-derived expected PNG
python3 expected.py /tmp/mirror-expected.png

# (xemu-side capture pipeline) — runs the iso under run-benchmark.sh
# with --xbe-mode and the F1 flip-stall trigger; produces a captured
# PNG that the harness diffs against /tmp/mirror-expected.png.
```

## Math derivation

See the source-file header in `main.c` for the line-by-line
derivation of the expected pattern (no hand-waving). The paired
`expected.py:default()` encodes the same math; a reviewer can read
both side-by-side and confirm "yes, the math says this should
output that."
