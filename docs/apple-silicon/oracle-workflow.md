# Oracle workflow — driving Metal-renderer development with the real Xbox

This document describes how the real-Xbox oracle pipeline plugs into
day-to-day Metal-renderer correctness work. It is **the** integration
guide for the oracle and supersedes the per-script READMEs as the
single source of truth for "what do I run when".

## TL;DR

```sh
# 1. Health check the oracle (run before anything else)
./scripts/apple-silicon/oracle-smoke.sh

# 2. Run the full Tier-1 visual gate (real Xbox + xemu Metal)
python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
    --renderer metal --renderer real-xbox --max-changed-pct 1.0

# 3. Smoke-validate one diag XBE end-to-end on real Xbox + compare math
./scripts/apple-silicon/oracle-smoke.sh --tier1 mirror

# 4. Validate synthetic-input pipeline (M15 controller-injection check)
./scripts/apple-silicon/oracle-smoke.sh --tier1 controller-roundtrip \
    --buttons 0xA5A5 --lt 16384 --lx 12345 --ly -12345 \
    --rx -32768 --ry 32767
```

If steps 1–4 are all green, the oracle is ready to drive an M15
default-on visual gate.

## Architecture diagram

```
+-------------------------- Mac (this repo) --------------------------+
|                                                                     |
|  oracle-smoke.sh ── runs all 12 health-check layers + Tier-1 diags. |
|       │                                                             |
|       ├── oracle-orchestrator.py                                    |
|       │       ├── ensure-agent (FTP SITE EXEC the agent XBE)        |
|       │       ├── run-diag (chainload diag XBE + FTP-pull capture)  |
|       │       └── capture (screenshot front buffer via agent RPC)   |
|       │                                                             |
|       ├── oracle-client.py                                          |
|       │       ├── info / eeprom / mem.read / nv2a.read / vram.read  |
|       │       ├── controller.set/get/button/axis/clear/buffer-info  |
|       │       ├── screenshot / runxbe / decode-xoss                 |
|       │       └── reboot / bye / help                               |
|       │                                                             |
|       └── xbe-harness/xbe_orchestrator.py                           |
|               ├── list / probe / expected / capture-reference / run |
|               └── per-cell drives xemu (GL / Metal) + real-Xbox     |
|                   and byte-compares against expected.py math oracle |
|                                                                     |
+-------------------- TCP 9001 + FTP 21 to 192.168.0.200 -------------+
                                  │
+-------------------------- Real Xbox --------------------------------+
|                                                                     |
|  iND-BiOS → /C/evoxdash.xbe → UnleashX (chainloader)                |
|                                                                     |
|  oracle-agent (E:\Apps\oracle-agent\default.xbe)                    |
|       ├── lwIP TCP listener on 9001 (text-line RPC + XOSS binary)   |
|       └── Persistent kernel-pool controller buffer                  |
|             via MmAllocateContiguousMemoryEx +                      |
|             MmPersistContiguousMemory; anchor at                    |
|             E:\Apps\oracle-agent\state\ctrl-addr.txt                |
|                                                                     |
|  Tier-1 diag XBEs (E:\Apps\<id>\default.xbe)                        |
|       ├── mirror / color-channel / depth-floor (NV2A correctness)   |
|       ├── controller-roundtrip (synthetic-input integration)        |
|       └── pipeline-smoke (Tier-4 plumbing oracle)                   |
|       Each diag uses xbed_runtime + xbed_capture (PCRTC-based       |
|       front-buffer read) + (optionally) xbed_input_synth (kseg0     |
|       map of the agent's persistent controller buffer).             |
|                                                                     |
|  Composite output → MS2109 USB stick → Mac:                         |
|       composite-record.sh / extract-keyframes.py / audio-waveform.py|
|                                                                     |
+---------------------------------------------------------------------+
```

## When to use each tool

| Situation | Tool | Why |
|---|---|---|
| Starting a Metal session, first thing | `oracle-smoke.sh` | 30 s health check; fails fast if Xbox is offline / agent broken / kernel-pool alloc broke |
| New Metal renderer change touching pgraph | `xbe-harness run --renderer metal --renderer real-xbox` | Catches divergence between Metal and real Xbox on the documented NV2A surface |
| Specific diag XBE failing | `oracle-smoke.sh --tier1 <id>` | Runs THAT diag on real Xbox + math-compares; isolates whether the bug is on real Xbox or xemu |
| Want canonical real-Xbox reference frame for new diag XBE | `xbe_orchestrator.py capture-reference --xbe <id>` | Stashes real Xbox PNG under `docs/apple-silicon/xbox-real-references/<id>/` |
| Need to drive a synthetic gameplay scenario | `controller-replay.py <csv>` | Replays a XEMU_RECORD_INPUT CSV into the agent's controller buffer (Tier-1 integration validated 2026-05-07) |
| Recording real Xbox output for offline analysis | `composite-record.sh` | NTSC composite + audio capture via MS2109 stick |
| Diagnosing a single visual failure | `oracle-client.py screenshot` then compare manually | Grabs the agent's debug-print console + dashboard; useful for "what is the Xbox actually showing right now?" |

## How the M15 visual gate uses the oracle

Per `metal-renderer-plan.md` §M15, the default-on flip requires:
- **5 distinct titles render at ≥ console-native FPS via Metal**
- **≤ 1% per-pixel diff vs GL on PGR2/Rainbow/Crimson/SC2 + 1 sweep title**
- **Cold-launch shader compile total < 5 s**
- **p99 mspf jitter ≥ 20% improvement vs GL on PGR2/Rainbow/Crimson**
- **No correctness bug open ≥ 30 days**

Adding the oracle adds a **sixth** mandatory exit-gate criterion:

> **All Tier-1 diag XBEs PASS on real Xbox AND on xemu Metal**, with
> per-pixel `changed_pixels_pct < 1.0` against either the math-derived
> oracle (`expected.py:default`) or the canonical real-Xbox reference
> (`docs/apple-silicon/xbox-real-references/<id>/real-xbox.png`).

This is enforced by:

```sh
python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
    --renderer metal --renderer real-xbox \
    --max-changed-pct 1.0 \
    --threshold 8
```

A non-zero exit code blocks the M15 flip. The harness writes a
matrix report at `benchmark-runs/xbe-harness-<UTC>/report.md` and
machine-readable `summary.json`.

The Tier-1 set as of 2026-05-07:

| XBE | Catalog ref | Tests |
|---|---|---|
| `mirror` | §A.6, §C.8, §J.1, §J.2 | Y-mirror / front-buffer orientation |
| `color-channel` | NV2A RT format | B/R channel ordering |
| `depth-floor` | depth test + native_tri_depth | LEQUAL depth + Z perspective |
| `controller-roundtrip` | Tier-1 controller injection | Synth state → kernel pool → diag XBE roundtrip |

Validation evidence (2026-05-07):
- All 4 PASS byte-exact on real Xbox (`max_abs_error=0`,
  `changed_pixels_pct=0.0000`).
- All 4 PASS on xemu Metal at `--max-changed-pct 1.0` against the
  same math-derived oracles.

## How synthetic input drives the M15 gate

Pre-2026-05-07, paired Metal-vs-GL diff harness (`metal-gl-compare.sh`)
required input-script CSVs replayed via `XEMU_SCRIPTED_INPUT` — a
xemu-only mechanism that doesn't apply to real Xbox. The M15 gate
needed real-Xbox parity captures of the SAME gameplay scenes.

Tier-1 controller injection (shipped 2026-05-07) closes that loop:

1. Record gameplay on xemu via `XEMU_RECORD_INPUT=<game>.csv`.
2. Replay on real Xbox via `controller-replay.py <game>.csv` —
   the same CSV, no transcoding.
3. The agent's persistent kernel-pool controller buffer (anchored
   at `E:\Apps\oracle-agent\state\ctrl-addr.txt`) holds the
   synthetic state; the chainloaded retail-or-diag XBE reads it via
   the `xbed_input_synth_*` shim.
4. Capture composite output via `composite-record.sh` for visual
   comparison or via the agent's `screenshot` RPC for byte-exact
   compare.

**Limitation as of 2026-05-07:** retail games do NOT yet read from
the synthetic buffer — the Tier-1 shim only attaches diag XBEs that
explicitly call `xbed_input_synth_attach()`. Tier-2 (kernel-mode
XInputGetState hook) is the path to retail-game gameplay validation;
designed in `controller-injection-research.md`, not yet implemented.

## Known gaps (close before declaring production-grade)

As of 2026-05-07 (post-recovery) the **oracle pipeline's mainline
visual gate is live-green** (m15-visual-gate.sh 5/5 PASS) but
**FOUR named blockers (B1-B4) remain open** before the oracle can
be declared "ready for production use". The full discussion +
investigation directions + resume recipe live in `handoff.md`
under "OPEN BLOCKERS"; the short version:

- **B1**: `controller-roundtrip` non-zero pre-set state intermittent
  stale-state read across `XLaunchXBE` chainload boundary (the
  diag's read of `phys = anchor_recorded_phys` sometimes returns
  a previous session's values). Tested PAGE_NOCACHE, atomic
  anchor rename + NtFlushBuffersFile, `-DORACLE_CTRL_ALLOW_REATTACH`
  — none restored consistency. Root cause not yet identified.
  Smoke + harness pass because they use zero-state mode.
- **B2**: `oracle-stress.sh` only ran 3 iterations; spec was 10.
- **B3**: `oracle-seqlock-test.py` live mode never run; only the
  offline `--selftest` (5/5 predicate cases PASS).
- **B4**: `bin-reattach/default.xbe` built but never deployed-
  and-run on the Xbox. Gap-7 exit criterion ("opt-in flag works
  OR is removed") not satisfied.

Until all 4 close, treat the oracle as **"ready for development
use, not yet ready for production"** — you can use it to
validate Metal-renderer changes and catch regressions today (the
mainline gate works), but do NOT cite a green oracle run as the
unconditional M15 default-on go-ahead criterion until B1-B4 are
resolved.

The 8 prior-session gaps from 2026-05-07 evening are all closed
code-side and the mainline validation paths are live-green; the
4 blockers above are sub-items that surfaced during live
validation.

## Failure recovery playbook

| Symptom | Likely cause | Recovery |
|---|---|---|
| `01 ping: host not responding` | Xbox powered off / unplugged / network unreachable | Power-cycle Xbox + check LAN cable + `arp -a` for MAC `00:12:5A:00:5B:CF` |
| `02 ensure-agent: did not come up` | Agent XBE corrupted / missing / wrong path | Reupload via FTP: `curl -u xbox:xbox -T scripts/apple-silicon/xbe-tests/oracle-agent/bin/default.xbe ftp://192.168.0.200/E/Apps/oracle-agent/default.xbe` |
| `04 eeprom: wrong size` | Agent compiled against wrong nxdk / lwIP buffer too small | Rebuild agent under nxdk; verify `bin/default.xbe` size ≈ 405 KB |
| `06 nv2a.read PMC_BOOT_0 wrong value` | Agent → NV2A MMIO mapping broken (kernel revision mismatch?) | Sanity-check via `oracle-client.py raw mem.read 0xfd000000 16` — should return non-zero NV2A signature |
| `08b controller.buffer-info: phys=0x00000000` | Kernel-pool allocation failed; agent fell back to BSS | E: drive not mountable from agent? Check `nxIsDriveMounted('E')` returns true. Re-deploy agent. |
| `11.<id>: capture file missing` | pbkit + D:\\ fopen path broken (regression) — should not happen post-2026-05-07 PCRTC fix | Verify `xbed_capture.c` includes the PCRTC_START path; rebuild diag XBEs |
| `11.<id>: changed_pixels_pct > 1.0` | Real divergence between real Xbox and math-derived oracle | This is an actionable finding — investigate the diag XBE's NV2A method coverage in `nv2a-feature-surface-research.md` |
| Agent listens (port 9001 alive, `info` succeeds) but binary-payload commands (`mem.read`, `nv2a.read`, `screenshot`) return empty payload | lwIP PCB pool exhaustion under tight diag-upload + chainload + relaunch loops (observed once 2026-05-07 evening; not yet root-caused) | `python3 oracle-client.py reboot` → wait 35 s → re-run smoke. Investigate via `oracle-stress.sh` next session (gap-closure item #5). |

## Adding a new Tier-1 diag XBE

The cookie-cutter recipe (mirrors `mirror/`, `color-channel/`,
`depth-floor/`):

1. Create `scripts/apple-silicon/xbe-tests/<id>/` directory.
2. `Makefile`:
   ```
   XBE_TITLE       = <id>
   GEN_XISO        = $(XBE_TITLE).iso
   SRCS            = $(CURDIR)/main.c
   XBED_LIB_DIR   ?= $(CURDIR)/../lib
   include $(XBED_LIB_DIR)/lib.mk
   NXDK_DIR       ?= /Users/jbbrack03/XEMU_MacOS/nxdk
   include $(NXDK_DIR)/Makefile
   ```
3. `main.c`: model after `mirror/main.c`. Use `xbed_init`, `xbed_load_default_shaders`,
   render via `xbed_set_attrib_pointer` + `xbed_draw_arrays`, finish with
   `xbed_render_loop_then_capture(render_one, NULL, 300, "D:\\<id>-capture.bin", "D:\\<id>-done.txt", "<id>")`.
4. `expected.py`: math-derived 640×480 RGBA buffer the diag should
   produce. Provide `default()` for zero-state and (optional) parameterized
   functions for variant inputs.
5. `manifest.json`: `id`, `title`, `purpose`, `self_validation_tier`,
   `oracle_priority` (start with `["math-derived"]` for Tier-1), and an
   `expected_results` table keyed by `<renderer>/<flag-recipe>/<scale>`.
6. Build: `eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && make`.
7. Validate locally on xemu Metal: `python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run --xbe <id> --renderer metal --max-changed-pct 1.0`.
8. Capture canonical real-Xbox reference once: `python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py capture-reference --xbe <id>`.
9. Run cross-renderer matrix: `python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run --xbe <id> --renderer metal --renderer real-xbox`.

If the new XBE renders synthetic input, also include
`#include "xbed_input_synth.h"` and call `xbed_input_synth_attach()` /
`xbed_input_synth_read()` per `controller-roundtrip/main.c`.

## Files of interest

| Path | Purpose |
|---|---|
| `scripts/apple-silicon/oracle-smoke.sh` | Single-command pipeline health check |
| `scripts/apple-silicon/oracle-client.py` | Mac-side wrapper around the agent's TCP/9001 protocol |
| `scripts/apple-silicon/oracle-orchestrator.py` | Higher-level driver: ensure-agent / capture / run-diag / validate |
| `scripts/apple-silicon/controller-replay.py` | Mac-side replay of a `XEMU_RECORD_INPUT` CSV into the agent |
| `scripts/apple-silicon/composite-record.sh` | ffmpeg AV recorder for the MS2109 capture stick |
| `scripts/apple-silicon/xbe-harness/xbe_orchestrator.py` | Matrix runner: per-(XBE × renderer) PASS/FAIL |
| `scripts/apple-silicon/xbe-tests/oracle-agent/` | nxdk source for the agent (controller buffer + RPCs) |
| `scripts/apple-silicon/xbe-tests/lib/xbed_input_synth.{h,c}` | Diag-XBE shim that reads the agent's persistent controller buffer |
| `scripts/apple-silicon/xbe-tests/lib/xbed_capture.{h,c}` | Diag-XBE shim that reads the displayed front buffer (PCRTC_START path) |
| `scripts/apple-silicon/xbe-tests/lib/xbed_runtime.{h,c}` | Common pbkit init / render / shader load |
| `scripts/apple-silicon/xbe-tests/<id>/` | One Tier-1 diag XBE per directory |
| `docs/apple-silicon/xbox-real-references/<id>/real-xbox.png` | Canonical real-Xbox reference frame for diag `<id>` |
| `docs/apple-silicon/controller-injection-research.md` | Tier-1 (shipped) + Tier-2/3 (designed) controller injection plan |
| `docs/apple-silicon/diagnostic-xbe-plan.md` | Diag-XBE library architecture + per-XBE specifications |
| `docs/apple-silicon/real-xbox-oracle-feasibility.md` | Original feasibility study (superseded same-day by Phase 1+2+3.0) |

## Versioning

| Component | Version | Notes |
|---|---|---|
| Oracle agent | v0.3 + persistent controller buffer | Shipped 2026-05-07 |
| `xbed_capture` PCRTC path | 1.0 | Shipped 2026-05-07; unblocked all Tier-1 diags |
| `xbed_input_synth` shim | 1.0 | Shipped 2026-05-07 |
| Persistence anchor file format | v1 | "XCTR\n0x<phys>\n0x<virt>\n0x<size>\n" |

If you bump the agent's protocol or the buffer's struct layout,
update `XBED_INPUT_SYNTH_VERSION` in
`scripts/apple-silicon/xbe-tests/lib/xbed_input_synth.h` AND the
agent's `ORACLE_CTRL_VERSION` in
`scripts/apple-silicon/xbe-tests/oracle-agent/controller.h`. The
shim's attach() rejects mismatched versions so an old diag XBE will
fail-loud against a new agent rather than reading garbage.
