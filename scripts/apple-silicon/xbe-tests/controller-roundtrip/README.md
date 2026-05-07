# controller-roundtrip — Tier-1 synthetic-input integration oracle

Validates the full Tier-1 controller-injection path:

1. The oracle agent allocates a persistent kernel-pool controller
   buffer at startup and writes the buffer's physical address to
   `E:\Apps\oracle-agent\state\ctrl-addr.txt`.
2. Mac-side `controller.set` RPCs write a known synthetic state
   into the buffer.
3. `runxbe` chainloads this XBE; the kernel destroys the agent
   process but the kernel-pool buffer (flagged
   `MmPersistContiguousMemory`) survives.
4. This XBE links the `xbed_input_synth_*` shim, which mounts E:,
   reads the anchor file, validates the buffer's magic+version, and
   exposes a `xbed_input_synth_read(port, &state)` accessor.
5. This XBE renders a deterministic pattern from the observed state
   (a 4×4 button grid in the top half + 6 axis-fraction stripes in
   the bottom half).
6. Capture + reboot via `xbed_render_loop_then_capture()` at the
   standard `D:\\controller-roundtrip-capture.bin` path.
7. Mac-side comparison: the orchestrator computes the same pattern
   via `expected.py:from_state(...)` using the synthetic state it
   set in step 2, then byte-compares.

A byte-exact match proves both:
- The kernel-pool buffer survived chainload byte-for-byte.
- The shim layer read it correctly via the kseg0 identity map.

## Failure modes the diag surfaces

- **All-cyan capture (`(0, 255, 255, 255)` everywhere)**: shim-attach
  failed. Root cause is one of: (a) agent didn't allocate a
  persistent buffer, (b) anchor file missing or malformed, (c)
  `MmGetPhysicalAddress` mismatch (kernel reclaimed the page), (d)
  buffer's magic/version corrupted, (e) E: drive could not be
  mounted from the diag-XBE process.
- **All-black capture (zero-state pattern)**: shim attached, but
  port-0 state is zero. Root cause is one of: (a) orchestrator
  didn't call `controller.set` before chainload, (b) `controller.set`
  was for a different port, (c) state was overwritten by a
  `controller.clear` between the set and the chainload.
- **Pattern mismatch (some cells lit but wrong, partial stripes)**:
  shim attached + state non-zero, but the bytes the shim observed
  differ from what the orchestrator set. Root cause is most likely
  a torn read (a concurrent `controller.set` during the diag's
  per-frame read) — the shim's seq-stamped retry loop should make
  this rare. Could also indicate a pool eviction during chainload
  (the kernel reclaimed the persistent page despite the
  `MmPersistContiguousMemory` flag).

## Build

```sh
eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)"
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon/xbe-tests/controller-roundtrip
make
```

Outputs `bin/default.xbe` (~150 KB) and `controller-roundtrip.iso`.

## Run via the orchestrator (preferred)

```sh
cd /Users/jbbrack03/XEMU_MacOS/xemu-fork
python3 scripts/apple-silicon/oracle-smoke.sh --tier1 controller-roundtrip
```

The smoke harness handles agent ensure-up, the pre-set
`controller.set port=0 buttons=… lt=… …`, the `runxbe` chainload,
the FTP-pull of the capture, and the byte-exact comparison against
`expected.py:from_state(...)`.

## Manual run (for debugging)

```sh
# 1. Bring the agent up
python3 scripts/apple-silicon/oracle-orchestrator.py ensure-agent

# 2. Pre-set the synthetic state we want to validate
python3 scripts/apple-silicon/oracle-client.py raw \
    "controller.set port=0 buttons=0x0001 lt=16384 lx=8192"

# 3. Upload + chainload via run-diag (ftp_collect = the XBE's E: dir)
python3 scripts/apple-silicon/oracle-orchestrator.py run-diag \
    --xbe 'E:\\Apps\\controller-roundtrip\\default.xbe' \
    --ftp-collect /E/Apps/controller-roundtrip \
    --out /tmp/controller-roundtrip-run

# 4. Decode XOSS to PNG via oracle-client helpers
python3 -c "
import importlib.util, pathlib
spec = importlib.util.spec_from_file_location(
    'oc', 'scripts/apple-silicon/oracle-client.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.decode_xoss_file('/tmp/controller-roundtrip-run/artifacts/controller-roundtrip-capture.bin',
                   '/tmp/controller-roundtrip.png')
"

# 5. Compare against the math-derived expected
python3 -c "
from pathlib import Path
import sys; sys.path.insert(0, 'scripts/apple-silicon/xbe-tests/controller-roundtrip')
import expected as e
b = e.from_state(buttons=0x0001, ltrigger=16384, lstick_x=8192)
import importlib.util
spec = importlib.util.spec_from_file_location(
    'oc', 'scripts/apple-silicon/oracle-client.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.save_screenshot_png(b, e.WIDTH, e.HEIGHT, '/tmp/controller-roundtrip-expected.png')
"

python3 scripts/apple-silicon/compare-screenshots.py \
    /tmp/controller-roundtrip.png /tmp/controller-roundtrip-expected.png \
    --crop 0,0,640,480 --threshold 0 --out-dir /tmp/cr-cmp
```
