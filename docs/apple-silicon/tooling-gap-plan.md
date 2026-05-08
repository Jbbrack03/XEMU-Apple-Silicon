# Tooling gap plan

Last updated: 2026-05-07.

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

- Build the first per-title patcher, starting with PGR2.
- Prove autonomous dashboard return from the patched title before gameplay
  input.
- Prove one visible patched input event.
- Run the existing PGR2 route through `retail-gameplay-oracle.py` with
  title-patch input and autonomous-exit evidence.
- Add successful `retail-oracle-smoke.py` evidence for at least one installed
  retail title before using real-Xbox footage as a gameplay oracle for Metal.
- Keep the hardware-controller-emulator path as the fallback if per-title
  patching stalls or broader generic title coverage becomes necessary.

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

Closure path:

1. Run `oracle-validate.sh` before any long Metal route if the real Xbox is in
   the loop.
2. Run `m15-visual-gate.sh --paired` to exercise build, oracle, canary, XBE
   matrix, and paired canary diff composition.
3. Complete five-title paired Metal-vs-GL route coverage:
   PGR2, Rainbow Six 3, Crimson Skies, Soul Calibur 2, and one broader-sweep
   title.
4. Record p99 mspf jitter deltas for PGR2/Rainbow/Crimson and cold shader
   compile time with a fresh shader cache.
5. Decide the front-fb fallback policy from title coverage evidence, not from
   intuition.

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
- Whether a successful retail-game real-Xbox smoke proof exists. Today this is
  expected to warn until Tier-2 input/exit is closed.

Full mode additionally runs `oracle-validate.sh` and
`m15-visual-gate.sh --paired`.
