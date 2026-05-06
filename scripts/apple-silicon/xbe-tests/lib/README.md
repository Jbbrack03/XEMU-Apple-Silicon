# xbed — shared diagnostic-XBE runtime

Shared C / shader / build snippets for the Tier-1 NV2A diag XBEs
(`mirror`, `color-channel`, `depth-floor`, ...) per
`docs/apple-silicon/diagnostic-xbe-plan.md` v2 §3.1.

## Files

- `xbed_runtime.{h,c}` — pbkit init, default render state, viewport
  matrix, attribute helpers, frame-loop boilerplate. Mirrors the
  pattern in `flat-tri-depth/main.c` and `nxdk/samples/triangle/main.c`.
- `xbed_capture.{h,c}` — write the post-flip front buffer to D:\ as
  XOSS (same format the oracle agent's `screenshot` uses) + reboot
  via `HalReturnToFirmware(HalRebootRoutine)`. Mirrors the
  `pipeline-smoke` capture-and-reboot tail.
- `vs.vs.cg` / `ps.ps.cg` — passthrough VS/PS used by every diag XBE.
  VS multiplies clip-space POSITION by the runtime's viewport matrix
  and forwards DIFFUSE → COLOR. PS returns COLOR.
- `lib.mk` — Makefile snippet each diag XBE includes to pull these
  sources into its build.

## Adding a new diag XBE

1. Create `xbe-tests/<id>/` with `main.c`, `manifest.json`, `expected.py`,
   `README.md`, and a Makefile shaped like:

   ```make
   XBE_TITLE       = <id>
   GEN_XISO        = $(XBE_TITLE).iso
   SRCS            = $(CURDIR)/main.c
   XBED_LIB_DIR   ?= $(CURDIR)/../lib
   include $(XBED_LIB_DIR)/lib.mk
   NXDK_DIR       ?= /Users/jbbrack03/XEMU_MacOS/nxdk
   include $(NXDK_DIR)/Makefile
   ```

2. The XBE source is small. Typical body:

   ```c
   #include "xbed_runtime.h"
   #include "xbed_capture.h"

   int main(void) {
       if (xbed_init(640, 480) != XBED_OK) return 1;
       xbed_set_default_render_state();
       xbed_load_default_shaders();
       xbed_load_viewport_matrix();
       xbed_clear_all_attribs_to_float();

       xbed_frame_begin();
       xbed_clear_color_argb(0xFF000000);
       /* ... issue draws ... */
       xbed_frame_end_and_swap();

       xbed_capture_and_reboot(
           "D:\\<id>-capture.bin", "D:\\<id>-done.txt", "<id>");
       return 0;
   }
   ```

3. Math-derive the expected output in the source-file header and in
   the paired `expected.py:default()` (or per-flag-recipe generator).

4. Add a manifest matching the `pipeline-smoke/manifest.json` schema
   plus `capture_at_flip_stall_ordinal: null` if you reboot with
   capture (Tier-1 host-side capture path).

## Capture model

Tier-1 diag XBEs render with the NV2A pgraph, then capture the
post-flip front buffer to disk and reboot. The orchestrator
(`oracle-orchestrator.py run-diag --xbe ... --ftp-collect ... --out
...`) chainloads the XBE, waits for FTP to come back after reboot,
pulls the artifacts, then relaunches the agent.

For xemu-side comparison (no real Xbox needed), the same XBE runs
through `run-benchmark.sh --xbe-mode` using
`XEMU_METAL_SCREENSHOT_PATH` + the F1 flip-stall trigger; the
captured PNG is compared to either the real-Xbox reference or the
`expected.py` math-derived buffer.
