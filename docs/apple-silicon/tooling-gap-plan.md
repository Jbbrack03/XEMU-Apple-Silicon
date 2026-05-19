# Tooling gap plan

Last updated: 2026-05-19 (oracle-independent measurement closure — three
gaps closed: surface-graph dump backing M5.12/M17 PGR2 multi-RT
investigation; gameplay-route temporal capture via
`capture-gameplay-temporal.sh` for the per-tracked-title temporal
re-validation required by the 2026-05-12 evening methodology; LLDB-
attached GL leg via `lldb-gl-launch.sh` + `metal-gl-compare.sh
--gl-attach-lldb` for the Halo cold-launch segfault. See
`benchmarks/2026-05-19-tooling-gap-closure.md` and decision-log
"2026-05-19"). Prior 2026-05-11: M15 checklist + paired-capture
finding.

This note records the feedback gaps that matter for the Metal backend and how
to close them without turning every Codex session into a pile of background
tools. The rule of thumb is simple: prefer tools that produce repeatable
artifacts under `benchmark-runs/`, and keep interactive tools opt-in.

## Xcode MCP

Status:

- Xcode's `mcpbridge` is installed at
  `/Applications/Xcode.app/Contents/Developer/usr/bin/mcpbridge`.
- Codex has an `[mcp_servers.xcode]` entry, but it is globally disabled.
- The main xemu tree is not an Xcode project. It builds through the existing
  Meson/CMake/app-bundle path (`./build.sh -a arm64`).
- The in-tree Swift component is `tools/xemu-capture/Package.swift`; that is
  the only local code that naturally fits Xcode/Swift-package tooling.

Policy:

- Do not enable Xcode MCP globally for this project. It previously caused
  unwanted Xcode activity at every Codex start.
- Use Xcode MCP only for a targeted task that genuinely needs Xcode project
  semantics: Swift Package work in `tools/xemu-capture/`, Xcode previews in a
  future UI project, or explicit GPU trace inspection.
- Continue using shell tools for the main xemu build, code signing, benchmark
  harnesses, oracle gates, and diag-XBE builds.

Useful non-MCP Xcode hooks already in the project:

- `XEMU_METAL_CAPTURE=/tmp/name.gputrace` and
  `XEMU_METAL_CAPTURE_FRAMES=N` produce Xcode-openable GPU traces through
  `MTLCaptureManager`.
- Apple Instruments "Metal System Trace" remains the manual whole-system GPU
  profiler when per-stage counters are not enough.

## Gap 1: automated `.gputrace` interpretation

Current state: programmatic capture exists and benchmark runs now emit a
machine-readable sidecar. Reading GPU draw/resource detail still means opening
the trace in Xcode, but the capture no longer floats around without provenance.

Closed pieces:

- `scripts/apple-silicon/metal-capture-manifest.py` writes
  `metal-capture-manifest.json` and `.md` next to any `run-benchmark.sh
  --metal-capture` run.
- `run-benchmark.sh` invokes the manifest generator automatically when
  `--metal-capture` is passed.
- The manifest records run metadata, capture path/size, capture start/stop
  log evidence, Metal validation/HUD state, renderer flags, and a ready-to-open
  Xcode command.

Remaining manual piece:

- If Xcode MCP exposes GPU-frame metadata without launching an editor window,
  wrap it in a targeted script. If it only helps with Xcode projects, leave it
  disabled and keep manual Xcode trace inspection as an explicit step.

## Gap 2: retail-game real-Xbox input

Current state:

- Tier-1 real-Xbox controller injection is shipped for diag XBEs.
- The Tier-2 readback/preflight tools are now present.
- Retail games still do not consume the synthetic controller buffer.
- The OGX360 hardware bridge is now the production retail-game input path.
- The end-to-end retail workflow is live-proven on Crimson Skies through
  dashboard FTP launch, OGX360 replay, composite capture, controller IGR, and
  dashboard FTP return.
- The same end-to-end retail workflow is now also live-proven on Rainbow Six 3
  at `benchmark-runs/retail-oracle-workflow-rainbow-20260510T214732Z/`.
- The same end-to-end retail workflow is now also live-proven on PGR2 at
  `benchmark-runs/retail-oracle-workflow-pgr2-20260510T231608Z/`.
- The production retail-oracle title set is now the stable trio:
  Crimson Skies, Rainbow Six 3, and PGR2.
- `retail-oracle-workflow.py` can now list installed retail titles and
  auto-resolve tracked titles against the Xbox's current FTP inventory instead
  of assuming one fixed install path.
- The wrapper also now accepts title-specific IGR-proof input routes plus
  explicit exit timing knobs (`--igr-proof-input-csv`, `--exit-delay-ms`,
  `--exit-hold-ms`, `--exit-attempts`, `--exit-repeat-gap-ms`).

Closed pieces:

- `scripts/apple-silicon/xbe-tests/controller-readback/` builds an nxdk SDL
  XBE that records the title-facing controller state to
  `D:\controller-readback.txt`.
- `scripts/apple-silicon/controller-readback-validate.py` runs the XBE through
  the oracle orchestrator, parses the pulled text artifact, and optionally
  enforces explicit `--expect key=value` checks.
- `scripts/apple-silicon/xbox-kernel-symbol-dump.py` safely dumps the running
  kernel PE export table through the oracle agent, probing only known kernel
  base candidates and export-table RVAs.
- `scripts/apple-silicon/xbox-kernel-export-annotate.py` joins the live
  ordinal-only dump with nxdk's `xboxkrnl.exe.def` so Tier-2 work can refer to
  concrete export names and addresses. On 2026-05-07 it named 366/366 exports
  for the project Xbox's `0x80010000` kernel base.
- `scripts/apple-silicon/retail-oracle-smoke.py` is the retail-game
  production gate. It validates the requested route and capture stack, then
  blocks the run unless a proven title-facing input backend and a proven
  autonomous dashboard-return backend are both supplied as evidence. This is
  intentional: an emulator-only smoke or an agent-buffer replay is not a
  retail-game oracle proof.
- `scripts/apple-silicon/retail-gameplay-oracle.py` is the guarded execution
  wrapper for when that evidence exists. It appends the softmod IGR combo to
  the route, records composite A/V, launches the retail XBE, runs the supplied
  input backend command, waits for dashboard FTP recovery, and extracts
  keyframes plus audio artifacts.
- `scripts/apple-silicon/retail-oracle-workflow.py` is the production wrapper
  around the bridge proof, IGR proof, capture preflight, and gameplay run. The
  2026-05-10 Crimson run at
  `benchmark-runs/retail-oracle-workflow-crimson-routeoffset-20260510T183546Z/`
  is the first accepted end-to-end retail-game oracle proof.
- `scripts/apple-silicon/ogx360-bridge/` is now the shipped hardware backend
  for retail-title control. Bench validation and Xbox-side readback validation
  both passed on 2026-05-09/10, and the workflow reuses those proofs.
- `scripts/apple-silicon/xbe-inspect.py` plus
  `docs/apple-silicon/retail-gameplay-software-paths.md` record the
  software-only verdict: agent RPC after launch and LaunchData-only preload
  remain ruled out. One generic title patch is not production-generic, but
  per-title XBE patching is now the accepted production path for the fixed
  5-6 game oracle scope.
- `scripts/apple-silicon/tier2-shim-analyze.py` plus
  `docs/apple-silicon/tier2-kernel-shim-viability.md` identify the primary
  generic Tier-2 software path. This Xbox matches NKPatcher `patcher_5838`;
  the first hook candidate is the `KeRaiseIrqlToDpcLevel` export slot at
  `0x800104e8`, but the 2026-05-08 live install crashed and this path is now
  research-only.
- `scripts/apple-silicon/tier2-shim-preflight.py` is the first live read-only
  gate. It verifies the export slot still contains `0x00003d04` before any
  future unsafe installer may patch it.

Remaining implementation piece:

- Soul Calibur 2 is now installed and its gameplay route is live-proven to
  reach character-select and active combat, but controller IGR from that title
  still black-screens/hangs the Xbox before dashboard FTP returns. The current
  failure artifact is
  `benchmark-runs/retail-oracle-workflow-sc2-20260510T232019Z/`.
- A second retry using the wrapper-level exit-timing knobs
  (`benchmark-runs/retail-oracle-workflow-sc2-20260511T005611Z/`,
  early single-attempt IGR from the round-end loss screen) failed the same
  way, so SC2 should now be treated as a title-specific hard blocker for the
  generic controller-IGR exit path, not as a blocker for the stable retail
  workflow trio.
- A third return-only retry under a live BIOS change from
  `iND-BiOS IGRMODE=2` to `IGRMODE=1` (compatible) also failed:
  `benchmark-runs/sc2-compatible-igr-proof-20260511T012513Z/` still
  ended with `dashboard_returned=false` and the Xbox off-network during
  both the primary and fallback recovery waits. The public iND-BiOS
  warning about SC2 quick-IGR lockups explains part of the symptom, but
  on this console compatible IGR was not sufficient to make SC2 return
  production-safe.
- A fourth return-only retry with `IGRMODE=1` still live but the legacy
  `E:\\x2config.ini` IGR layer disabled (`igrEnabled = 0`) also failed:
  `benchmark-runs/sc2-compatible-igr-x2off-proof-20260511T022925Z/`
  still ended with `dashboard_returned=false`. The failure shape changed
  slightly during fallback recovery, so the stack configuration matters,
  but removing the second IGR layer still did not make SC2 return
  production-safe.
- The immediate next production path is now patch-based rather than more
  generic-IGR tuning. Offline prep succeeded on 2026-05-11:
  `benchmark-runs/sc2-offline-xbe/Default.xbe` matches the configured
  SC2 SHA-256 exactly, a return-only patch artifact exists at
  `benchmark-runs/retail-title-patches/return-only-20260511T023825Z/sc2/default.xbe`,
  and a full route-driven patch artifact exists at
  `benchmark-runs/retail-title-patches/route-20260511T023825Z/sc2/default.xbe`.
  The route patch resolved unique hooks for `xinputgetcaps`,
  `xinputgetstate`, and `xinputsetstate`, with a direct
  `HalReturnToFirmware(reboot)` exit after the route.
- On 2026-05-11 the SC2 return-only patch was re-proved live on the
  current Xbox image at
  `benchmark-runs/retail-return-proof-sc2-20260511T0916-localreturn/`
  (`status=ok`, `dashboard_ftp_returned=true`).
- The same day, the full SC2 route patch was launched live at
  `benchmark-runs/retail-automation-proof-sc2-route-20260511T0920/`.
  It got past upload/launch but still failed to return:
  `status=fail`, `dashboard_ftp_returned=false`, `capture_rc=137`,
  `video.mp4 missing`, and the Xbox ended fully down (`ping=false`,
  `ftp=false`, `agent=false`). So the title-local direct reboot path is
  good in isolation, but the full embedded SC2 route patch is not yet
  production-safe.
- A smaller intermediate SC2 physical-device input-proof patch now
  exists at
  `benchmark-runs/retail-title-patches/input-proof-20260511T132911Z/sc2/default.xbe`.
  It was then live-tested at
  `benchmark-runs/retail-automation-proof-sc2-input-20260511T0832/`
  and also failed to return (`status=fail`, `dashboard_ftp_returned=false`,
  `capture_rc=137`, `video.mp4 missing`, Xbox fully down afterward).
  That result matters: the breakage is not specific to the huge embedded
  SC2 gameplay route. Even a tiny 8-event proof pulse on the current
  XInput-hook patch set is enough to strand SC2.
- `scripts/apple-silicon/retail-title-patcher.py` now supports
  `--physical-hook-profile {full,state-only}` so SC2 hook narrowing can
  be tested directly.
- `benchmark-runs/retail-title-patches/input-proof-20260511T141421Z/sc2/default.xbe`
  is the first SC2 `state-only` physical patch (hooking only
  `xinputgetstate`).
- `benchmark-runs/retail-automation-proof-sc2-input-stateonly-20260511T1415/`
  live-tested that `state-only` patch on 2026-05-11 and still failed to
  return (`status=fail`, `dashboard_ftp_returned=false`,
  `capture_rc=137`, `video.mp4 missing`, Xbox fully down through the
  full recovery window).
- `scripts/apple-silicon/retail-title-patcher.py` also now supports
  `--proof-style {pulse,idle}` for `--mode input-proof`; an SC2 idle
  state-only artifact is staged at
  `benchmark-runs/retail-title-patches/input-proof-20260511T142237Z/sc2/default.xbe`
  for the next live reboot window.
- `benchmark-runs/retail-automation-proof-sc2-input-stateonly-idle-20260511T1503/`
  then live-tested that idle state-only patch on 2026-05-11 and it still
  failed to return (`status=fail`, `dashboard_ftp_returned=false`,
  `capture_rc=137`, `video.mp4 missing`, Xbox down through the full
  recovery window).
- A title-owned SC2 wrapper-bypass artifact now exists at
  `benchmark-runs/retail-title-patches/sc2-local-zero-20260511T175812Z/sc2/default.xbe`.
  It redirects the SC2-local wrapper entry at `0x001c410` to the sibling
  helper at `0x001c4c0`, which zeroes the same analog output fields
  without touching `XInputGetState`.
- A second title-owned artifact now exists at
  `benchmark-runs/retail-title-patches/sc2-local-zero-return-20260511T175916Z/sc2/default.xbe`,
  which detours that same SC2-local wrapper to a stub that bypasses
  `XInputGetState`, zeroes the same analog output fields, and calls
  `HalReturnToFirmware(reboot)` after a 25 s dwell.
- `benchmark-runs/retail-automation-proof-sc2-local-zero-return-20260511T1800/`
  then live-tested that title-owned wrapper detour on 2026-05-11 and it
  still failed to return (`status=fail`, `dashboard_ftp_returned=false`,
  `capture_rc=137`, `video.mp4 missing`, Xbox down through the full
  recovery window).
- Immediate next work should move from rung-climbing to deeper patch
  surgery: intercepting `xinputgetstate` alone is enough to break SC2, and
  the first title-owned wrapper detour at `0x001c410` is still not enough
  to restore dashboard FTP. The next experiment should patch a later
  title-owned input consumer farther downstream than that wrapper, or add
  breadcrumb/marker output to the current title-owned stub before another
  live proof. This is now deferred work, not a blocker for the production
  retail oracle.
- Decide whether Halo or another sixth title should join the retail-gameplay
  oracle set for the broader M15 evidence bundle.
- Keep the per-title patching and Tier-2 kernel-hook material as research or
  fallback paths, not as blockers for the hardware-backed retail workflow.

## Gap 3: session-start state visibility

Current state: evidence exists but is spread across many run directories and
docs.

Closure path:

- Use `scripts/apple-silicon/metal-feedback-dashboard.py` at the start of a
  Metal session. It reports the latest M15 gate, oracle validation, canary
  regression, XBE matrix, paired diff, Xcode MCP state, and the remaining
  feedback gaps.
- Use `scripts/apple-silicon/metal-tools-readiness.sh --quick` when you need
  the toolchain itself checked before a renderer/debugging session. Use
  `--full` only when the Xbox is online and the long oracle + M15 gates are
  desired.

Example:

```sh
./scripts/apple-silicon/metal-feedback-dashboard.py
```

Optionally persist a snapshot:

```sh
./scripts/apple-silicon/metal-feedback-dashboard.py \
  --out benchmark-runs/metal-feedback-dashboard.md
```

## Gap 4: full M15 evidence bundle

Current state: the oracle-side gate is green, but the complete default-on
decision still needs the title-level visual/perf bundle.

Status command:

```sh
./scripts/apple-silicon/m15-bundle-status.py
```

This reads existing `benchmark-runs/` artifacts and reports which M15
preconditions are green, failed, or missing without launching xemu or touching
the Xbox. As of 2026-05-11 it reports:

- Oracle production gate: green via
  `benchmark-runs/oracle-validate-m15-20260511Ttargeted/summary.json`
  (`pass=4`, `fail=0`; targeted run skipped stress).
- Stable retail oracle trio: green for Crimson Skies, Rainbow Six 3, and PGR2.
- Paired visual/perf bundle: not green. PGR2/Rainbow latest passes are
  capture/static-canary evidence only, Crimson paired diff fails, and SC2/Halo
  gameplay diffs are still missing.
- P99 jitter: PGR2/Rainbow/Crimson fail the current bundle criteria.
- Cold shader compile proof is still open. Front-fb fallback policy is
  resolved as of 2026-05-11 evening (opt-in stays; multi-RT compositing
  fix deferred — see decision-log "2026-05-11 (evening 2)").

Tooling note: attempting to move the GL leg of `metal-gl-compare.sh` to QMP
framebuffer capture on 2026-05-11 proved that this app build does not expose
`screendump` through QMP or HMP. `qmp-capture.py` now has flip-stall sentinel
support and an HMP/PPM fallback for builds that do expose screendump. The
paired diff harness has since moved the `--trigger flip` GL leg to
`XEMU_GL_SCREENSHOT_PATH` and the Metal leg to
`XEMU_METAL_SCREENSHOT_SOURCE=nv2a`; the remaining tooling gap is gameplay
sequence capture, keyframe selection, and content-aligned GL/Metal/oracle
comparison.

Closure path:

1. Run `oracle-validate.sh` before any long Metal route if the real Xbox is in
   the loop.
2. Run `m15-visual-gate.sh --paired` to exercise build, oracle, canary, XBE
   matrix, and paired static-canary diff composition.
3. Complete five-title paired Metal-vs-GL route coverage:
   PGR2, Rainbow Six 3, Crimson Skies, Soul Calibur 2, and one broader-sweep
   title.
4. Record p99 mspf jitter deltas for PGR2/Rainbow/Crimson and cold shader
   compile time with a fresh shader cache.
5. Decide the front-fb fallback policy from title coverage evidence, not from
   intuition.

## Gap 5: oracle-independent measurement closure — **CLOSED 2026-05-19**

Three measurement gaps were blocking the next round of M15 default-on
evidence work while the retail Xbox oracle is offline (thermal repaste
pending). The 2026-05-19 slice closed all three with oracle-independent
tools (decision-log "2026-05-19";
`benchmarks/2026-05-19-tooling-gap-closure.md`).

Closed pieces:

- **Surface-graph dump.** `XEMU_METAL_SURFACE_GRAPH_DUMP=path` +
  `XEMU_METAL_SURFACE_GRAPH_AT_FLIP_STALL=N` /
  `XEMU_METAL_SURFACE_GRAPH_INTERVAL=N` emit per-flip JSONL of every
  cached `MtlSurfaceBinding`. Analyzer
  `scripts/apple-silicon/surface-graph-analyze.py` ranks final-composite
  candidates by recency (`last_color_draw_seq`) with explicit
  publish-source attribution (`s_last_publish_source_*` statics
  recorded under `s_front_framebuffer_lock`). New counter
  `METAL_SURFACE_GRAPH_DUMPS`. Compresses the three-diagnostic-runs-
  with-different-`XEMU_METAL_SCREENSHOT_SOURCE=vram:0x…`-overrides
  workflow from 2026-05-11 into one xemu run + one analyzer pass.
  Smoke evidence: 25 flips × 17 bindings on flat-tri-depth produced
  7 candidate addresses, with the display-compose publish path's
  `source_texture ≠ published_texture` divergence captured cleanly
  (Codex finding #1 about pointer-match being unreliable proven correct).
- **Gameplay-route temporal capture.**
  `scripts/apple-silicon/capture-gameplay-temporal.sh` is a thin
  orchestrator over `run-benchmark.sh`. New
  `XEMU_BENCH_TEMPORAL_CAPTURE=1` mode in the launcher forces PNG-
  every-frame output: Metal via the existing renderer-native
  `XEMU_METAL_SCREENSHOT_*` infrastructure (every-frame from frame 1,
  `_SOURCE=nv2a`), GL via a parallel `ffmpeg -f avfoundation` capture
  decomposed post-run. Pairs directly with
  `temporal-flicker-analyze.py`. Smoke evidence: 691 frames at
  ~59.6 fps over 12 s under METAL on flat-tri-depth.
- **LLDB-attached GL leg.**
  `scripts/apple-silicon/lldb-gl-launch.sh` writes a one-shot wrapper
  script and exports `XEMU_BENCH_LAUNCHER_PREFIX=<wrapper-path>`,
  which `run-benchmark.sh` prepends to the xemu launch in all three
  launch branches. The wrapper invokes `lldb --batch --source-on-crash
  <cmds>` so on crash the file (`thread list`, `thread backtrace all`,
  `process status`, `register read`, `image list`) fires and the
  backtrace lands in `<run-dir>/crash.lldb.log`.
  `metal-gl-compare.sh --gl-attach-lldb` routes the GL leg through
  the wrapper. Smoke evidence: wrapper invocation OK on flat-tri-depth;
  crash path not exercised (no segfault on flat-tri-depth) but
  `--source-on-crash` is standard LLDB behavior. Known limitation:
  the inferior's normal `fprintf(stderr,…)` output is full-buffered
  under LLDB's batch redirection — irrelevant for crash capture (the
  crash path uses `-o` commands which flow into LLDB's own stdout)
  but visible as missing perf intervals in a non-crashing LLDB run.

Codex review applied to plan + changes; 4 of 5 plan findings adopted,
2 of 2 changes findings adopted, 1 plan finding deflected (prompt
overstated rule #4 — project rule mandates `automation.md` +
`.claude/rules/flags-*.md`, both already present). Marker at
`.claude/state/codex-validate-last-run`.

## Readiness gate

`scripts/apple-silicon/metal-tools-readiness.sh` is the single "are my tools
ready?" command.

Quick mode checks:

- xemu app binary and Swift capture package presence.
- Xcode `mcpbridge` availability and the intentional global-disabled Codex
  MCP policy.
- Python compile health for the dashboard, capture manifest, kernel symbol
  dump, controller readback validator, oracle client, and orchestrator.
- Shell syntax for benchmark and gate scripts.
- Controller-readback source/manifest and built-XBE presence.
- Whether previous capture-manifest and kernel-export artifacts exist.
- Whether a successful retail-game real-Xbox workflow or smoke proof exists.

Full mode additionally runs `oracle-validate.sh` and
`m15-visual-gate.sh --paired`.
