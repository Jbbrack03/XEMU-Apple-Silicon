# 2026-05-19 — Tooling-gap closure: three oracle-independent measurement tools

> Postscript, later on 2026-05-19: the retail Xbox oracle returned to
> service after the repaste validation documented in
> `2026-05-19-retail-oracle-post-repaste-thermal-check.md`. The
> "oracle is offline" framing below is historical context for why this
> tooling slice was started.

## Motivation

Three measurement gaps were blocking the next round of M15 default-on
evidence work while the retail Xbox oracle is offline (thermal repaste
pending; see `benchmarks/2026-05-12-noctua-fan-validation.md`). Project
rule #1 forbids guessing; rule #5 demands building tools when the
existing toolset cannot answer the question. This slice closes all
three gaps:

1. PGR2 multi-RT compositing investigation (M5.12/M17) needed structured
   per-flip dumps of every cached `MtlSurfaceBinding` to identify the
   final-composite surface by elimination — today this requires three
   separate xemu runs with `XEMU_METAL_SCREENSHOT_SOURCE=vram:0x…` per
   `benchmarks/2026-05-11-pgr2-metal-render-path-diagnostic.md`.
2. Per-tracked-title temporal re-validation (decision-log 2026-05-12
   evening) needed a gameplay analogue of `capture-boot-temporal.sh`.
3. Halo paired-gameplay infra-block at
   `benchmark-runs/20260511-153638-metal-gl-compare-halo/` had zero
   backtrace evidence for the GL cold-launch segfault.

## Plan validation

`/codex-validate plan` (model: Codex CLI 0.16.0, read-only, ChatGPT
subscription mode) returned a MAJOR-ISSUES verdict with five findings;
four were adopted and one deflected (the deflected one was based on an
overstated rule in the prompt itself — project rule #4 does not require
`xemu-fork/CLAUDE.md` updates for new runtime flags). Full
adopt/deflect rationale lives in the decision-log entry for
2026-05-19.

## Tool 1 — Surface-graph dump

### Renderer-side

- `hw/xbox/nv2a/pgraph/mtl/surface.h` — declares
  `pgraph_mtl_surface_dump_graph_jsonl(FILE*, const char*, uint64_t)`
  and `pgraph_mtl_surface_graph_dumps()` accessor.
- `hw/xbox/nv2a/pgraph/mtl/surface.mm`:
  - Added `last_color_draw_seq` field to `MtlSurfaceBinding`
    (bumped in `pgraph_mtl_surface_note_color_draw` when
    `color_write=true`). Codex finding #2 fix — `frame_draw_count`
    alone is cumulative and only resets on the fallback publish path.
  - Added `s_last_publish_*` statics + `record_publish_source_locked()`
    helper. The four publish paths (`publish_front_texture` non-
    snapshot branch, `publish_front_texture` snapshot branch,
    `publish_front_fb_pointer_only`, `publish_display_binding_front_fb`)
    record the SELECTED source binding under
    `s_front_framebuffer_lock` alongside the published texture
    pointer store. Codex finding #1 fix — pointer-match against
    `s_front_framebuffer_texture` fails on the display-compose path
    where `dst` is a composed texture, not the source binding's
    `e->texture`.
  - Implemented `pgraph_mtl_surface_dump_graph_jsonl` — walks
    `s_cache_head` (caller must hold `pg->lock`), snapshots the
    publish-source statics under `s_front_framebuffer_lock`, emits one
    flip header + one binding object per cache entry, flushes the FILE*.
  - New counter `s_graph_dumps` + `pgraph_mtl_surface_graph_dumps()`
    accessor.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c`:
  - New env-cache + lazy-open machinery
    (`s_surface_graph_*`, `mtl_surface_graph_dump_if_enabled`).
    Three env vars: `XEMU_METAL_SURFACE_GRAPH_DUMP=path` (activates),
    `XEMU_METAL_SURFACE_GRAPH_AT_FLIP_STALL=N` (one-shot at Nth flip),
    `XEMU_METAL_SURFACE_GRAPH_INTERVAL=N` (every-Nth flip). Default
    when only path is set is every-flip.
  - Hook fires from `pgraph_mtl_flip_stall` AFTER both publish paths
    complete so the dump sees the final state.
- `util/xemu-metal-perf.c`:
  - New weak symbol stub for `pgraph_mtl_surface_graph_dumps`.
  - New baseline static, value-read, delta, mask-or, format-string
    entry, and printf arg for `METAL_SURFACE_GRAPH_DUMPS`.
- `scripts/apple-silicon/extract-perf-summary.sh` — registers
  `METAL_SURFACE_GRAPH_DUMPS` in the counter list.

### Analyzer

`scripts/apple-silicon/surface-graph-analyze.py`. Reads the JSONL
stream, emits:
- `summary.json` — per-flip counts + per-vram_addr aggregates.
- `report.md` — first/middle/last flip tables with ranked rows + a
  candidates section per flip.
- `candidates.md` — standalone candidate writeup.
- `per-flip.csv` — spreadsheet-friendly long form.

Candidate heuristic (configurable via `--target-width`, `--target-height`,
`--target-format`, `--scale-tolerance`):
- `is_color = true`
- `last_color_draw_seq > 0` (drew color in this run)
- NOT the publish source for the surrounding flip
- Guest dims within tolerance × target
- Optional `nv097_format` match

Ranking: `last_color_draw_seq` desc (recency) then `frame_draw_count`
desc (cumulative activity).

### Build evidence

`ninja -C build qemu-system-i386` — 18 targets, no new warnings.
Pre-existing `-Wmissing-prototypes` warnings unrelated.

## Tool 2 — Gameplay-route temporal capture

### Launcher change

`scripts/apple-silicon/run-benchmark.sh`:
- New env vars `XEMU_BENCH_TEMPORAL_CAPTURE=1` and
  `XEMU_BENCH_TEMPORAL_FPS=N`. When TEMPORAL_CAPTURE is on:
  - Metal: exports `XEMU_METAL_SCREENSHOT_PATH=$RUN_DIR/frames/metal-gameplay.png`,
    `_AT_FRAME=1`, `_INTERVAL=1`, `_SOURCE=nv2a`. The renderer-native
    every-frame screenshot path handles it.
  - GL: spawns `ffmpeg -f avfoundation -framerate <fps> -i 1:none`
    in parallel, recording to `$RUN_DIR/frames/gameplay.mov`,
    decomposed post-run via `ffmpeg -vf fps=<fps>` to
    `gameplay-NNNN.png`.
- New env var `XEMU_BENCH_LAUNCHER_PREFIX="..."` — prepended to the
  xemu launch in all three launch branches (record/live/scripted).
  Used by Tool 3.
- ffmpeg lifecycle integrated into `cleanup()` — SIGINT first for
  clean container finalize, SIGKILL after 2.5 s grace.

### Wrapper

`scripts/apple-silicon/capture-gameplay-temporal.sh`. Thin orchestrator
(~210 lines including header comments). Resolves input CSV
(<game>-gameplay.csv preferred over <game>-smoke.csv), sets the
canonical M15 recipe env (XEMU_NATIVE_TRI_DEPTH=1, XEMU_NATIVE_QUAD=1,
XEMU_PGRAPH_FAST_READ=1, XEMU_GL_MSAA=4 or XEMU_METAL_* equivalents),
calls `run-benchmark.sh`, parses the run-dir from launcher output,
prints a `temporal-summary.json` + the next-step
`temporal-flicker-analyze.py` invocation.

## Tool 3 — LLDB-attached GL leg

### Wrapper

`scripts/apple-silicon/lldb-gl-launch.sh`. Sets
`XEMU_BENCH_LAUNCHER_PREFIX="lldb --batch -o run -k 'thread list' -k 'thread backtrace all' -k 'process status' -o quit --output-path <tmp> --error-path <tmp> --"`
and invokes `run-benchmark.sh`. After the run finishes, copies the
captured LLDB output into `<run-dir>/crash.lldb.log`. Forces
`XEMU_RENDERER=GL`. All other env passes through. Pre-flight check
for `lldb` in PATH.

### metal-gl-compare integration

`scripts/apple-silicon/metal-gl-compare.sh` gains `--gl-attach-lldb`.
When set, the `run_gl()` function uses `$LLDB_GL_LAUNCH` instead of
`$RUN_BENCHMARK` as the launcher. Metal leg is unaffected. Operator
help updated to document the flag.

## Smoke evidence

_To be filled in by the smoke-test task. Each tool gets one short
run with the resulting artifact paths committed alongside this note._

### Tool 1 + Tool 2 (METAL) combined smoke: flat-tri-depth

```sh
XEMU_METAL_SURFACE_GRAPH_DUMP=/tmp/smoke-graph.jsonl \
XEMU_METAL_SURFACE_GRAPH_INTERVAL=10 \
./scripts/apple-silicon/capture-gameplay-temporal.sh \
    --renderer METAL --game flat-tri-depth --duration 12
```

Result: 691 PNG frames over 12 s (~57.6 fps; `temporal-summary.json`
`effective_fps_estimate=59.58`).
Run dir: `benchmark-runs/20260519-145729-flat-tri-depth/`.

JSONL: 224 292 bytes, 25 flip headers × ~17 bindings each ≈ 450
binding objects. Decisive evidence the publish-source fix
(Codex finding #1) is necessary — first flip header shows:

```json
"last_publish":{
  "kind":"display-compose",
  "reason":"fallback-dominant-draw",
  "source_vram_addr":"0x2e06000",
  "source_texture":"0xb3468d4c0",       // ← source binding's e->texture
  "published_texture":"0xb3468fb00"     // ← different pointer (composed dst)
}
```

A pointer-match heuristic over `s_front_framebuffer_texture` would
have failed to identify `0x2e06000` as the publish source. With the
explicit `source_vram_addr` field, the analyzer correctly flags it.

Analyzer output:
- `surface-graph/summary.json`: `flip_count=25`,
  `candidate_addrs_overall=[0x2854000, 0x2894000, 0x28d4000,
  0x2914000, 0x2954000, 0x2994000, 0x32a4000]` (7 candidates per flip).
- `surface-graph/report.md`, `surface-graph/candidates.md`,
  `surface-graph/per-flip.csv` all generated.

### Tool 2 (GL) smoke

Not run in this session. GL temporal capture invokes
`ffmpeg -f avfoundation -i "1:none"` which requires macOS Screen
Recording TCC permission for the calling terminal. Verifying it
requires a user-interactive permission grant; deferred. The script
path is exercised by `bash -n` syntax-check and the run-benchmark.sh
ffmpeg branch is identical to the well-validated path in
`capture-boot-temporal.sh` (in production since 2026-05-12).

### Tool 3 smoke: flat-tri-depth GL under LLDB

```sh
./scripts/apple-silicon/lldb-gl-launch.sh flat-tri-depth \
    scripts/apple-silicon/input-scripts/noop.csv 8
```

Run dir: `benchmark-runs/20260519-150349-flat-tri-depth/`. The wrapper:
- Wrote `crash.lldb.log` with structured "LLDB session output" +
  "Inferior stdout/stderr" sections.
- Correctly invoked LLDB with `--source-on-crash <file>` referencing
  a temp file containing `thread list`, `thread backtrace all`,
  `process status`, `register read`, `image list`.
- Used a wrapper-shell file as the launcher prefix to avoid
  bash-word-splitting on multi-word LLDB commands (see "Limitations"
  below).

Crash path NOT exercised in this smoke (no segfault on flat-tri-depth).
The mechanism is `--source-on-crash` standard LLDB behavior — on the
Halo cold-launch segfault the file will be sourced, the five commands
will run, and their output will flow into the run dir.

### LLDB output buffering limitation (Tool 3 honesty)

Even under `process launch -o <file> -e <file>` redirection, the
inferior's `fprintf(stderr, ...)` perf-interval lines do not appear in
the redirected file during normal (non-crashing) runs. xemu uses
`stderr` for perf logs; when `stderr` connects to a file instead of a
terminal it switches to full buffering, and our QMP-quit / SIGTERM
exit paths don't always flush before LLDB tears down the process.
For the CRASH use case (Tool 3's actual goal) this is irrelevant —
the `--source-on-crash` file fires while xemu is still stopped at the
crash site, producing the backtrace to LLDB's own stdout which
run-benchmark.sh has already redirected to xemu.log.

## Cross-references

- `decision-log.md` entry for 2026-05-19 — adopt/deflect rationale.
- `automation.md` — three new flags + three new scripts canonical
  descriptions.
- `.claude/rules/flags-renderer.md` + `flags-bench.md` — one-line
  index entries.
- `handoff.md` — next session should pick up "Tier 1" work
  (T2 paired reruns) now backed by the temporal capture tool, and
  the M5.12/M17 PGR2 multi-RT investigation backed by the
  surface-graph dump.

## Footprint

- `hw/xbox/nv2a/pgraph/mtl/surface.h`: +25 lines
- `hw/xbox/nv2a/pgraph/mtl/surface.mm`: ~140 lines
- `hw/xbox/nv2a/pgraph/mtl/renderer.c`: ~90 lines
- `util/xemu-metal-perf.c`: ~15 lines spread across baseline / value
  / delta / mask / format / args
- `scripts/apple-silicon/extract-perf-summary.sh`: +1 line
- `scripts/apple-silicon/run-benchmark.sh`: ~80 lines
- `scripts/apple-silicon/metal-gl-compare.sh`: ~30 lines
- `scripts/apple-silicon/capture-gameplay-temporal.sh`: 210 lines new
- `scripts/apple-silicon/lldb-gl-launch.sh`: 105 lines new
- `scripts/apple-silicon/surface-graph-analyze.py`: 310 lines new
- `docs/apple-silicon/automation.md`: ~110 lines
- `docs/apple-silicon/decision-log.md`: ~75 lines
- `docs/apple-silicon/benchmarks/2026-05-19-tooling-gap-closure.md`:
  this file
- `.claude/rules/flags-renderer.md`: +3 lines
- `.claude/rules/flags-bench.md`: +3 lines

Total ~1170 lines added; ~95 lines of renderer C/.mm changes.
Well above the rule #15 codex-validate-changes threshold (30 lines).

## Limitations and follow-ups

- The Metal renderer-native every-frame screenshot writes one PNG per
  presented frame, not per flip-stall. Under T2 the publish cadence
  is per-host-refresh (~60 Hz) while flip-stall is per-guest-flip
  (30 Hz on tracked titles). Temporal analysis sees the higher
  cadence, which is appropriate for the "missing-between-flips"
  failure class but can amplify VRAM-side flicker.
- The GL temporal capture uses AVFoundation primary-display capture
  with no built-in xemu window targeting — downstream
  `temporal-flicker-analyze.py` must `--gl-crop` to the xemu region.
  Future refinement: extend `pgraph_gl_capture_display_if_requested`
  to interval mode for renderer-native GL every-frame.
- The LLDB wrapper's `--output-path` captures lldb's stdout/stderr but
  not the xemu inferior's. The inferior's output already goes to
  `xemu.log` via run-benchmark.sh's redirection. On segfault the
  thread backtrace ends up in `crash.lldb.log`; the xemu stderr
  remains in `xemu.log` per the normal pattern.
- `XEMU_METAL_SURFACE_GRAPH_DUMP` opens the file in append mode and
  never closes it explicitly — the OS reclaims the descriptor on
  process exit. Fine for a diagnostic flag; would need atexit
  registration if the file became long-lived state.
