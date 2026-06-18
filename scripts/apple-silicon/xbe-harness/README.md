# xbe-harness — diag-XBE production correctness gate

Top-level driver for the diagnostic-XBE pipeline (per
`docs/apple-silicon/diagnostic-xbe-plan.md` v2). Given the set of
Tier-1 diag XBEs in `../xbe-tests/`, runs each XBE on every available
renderer (xemu-GL, xemu-Metal, real Xbox) and emits a per-cell
PASS/FAIL matrix.

When the matrix is all-PASS for the active diag-XBE library, we have
empirical evidence the renderer is correct on the feature surface
those XBEs cover. When a real game then misrenders, the bug is
*outside* that feature surface — narrowing the search.

## Quick start

```sh
# 1. List discovered XBEs
python3 xbe_orchestrator.py list

# 2. Probe renderer availability (xemu binary present, real Xbox alive)
python3 xbe_orchestrator.py probe

# 3. Generate a math-derived expected PNG (audit oracle)
python3 xbe_orchestrator.py expected --xbe mirror --out /tmp/mirror-expected.png

# 4. Capture canonical real-Xbox reference (one-shot per XBE per console)
python3 xbe_orchestrator.py capture-reference --xbe mirror

# 5. Run the full matrix (all Tier-1 XBEs × all available renderers)
python3 xbe_orchestrator.py run \
    --out /tmp/xbe-matrix \
    --threshold 16 --max-changed-pct 1.0
# → /tmp/xbe-matrix/report.md  (markdown matrix)
# → /tmp/xbe-matrix/summary.json  (machine-readable)
# → /tmp/xbe-matrix/<xbe>/<renderer>/  (per-cell artifacts)
```

## Layout

```
scripts/apple-silicon/
├── xbe-tests/          ← diagnostic XBEs (one dir per XBE)
│   ├── lib/            ← shared XBE-side runtime (xbed_*)
│   ├── mirror/
│   ├── color-channel/
│   ├── depth-floor/
│   ├── pipeline-smoke/ ← Tier-4 plumbing test (predates Tier-1)
│   └── flat-tri-depth/ ← legacy XBE — UNTOUCHED (project rule #11)
└── xbe-harness/        ← Mac-side orchestration
    ├── xbe_discover.py    ← find XBEs from manifest.json
    ├── xbe_renderers.py   ← per-renderer drivers (gl/metal/real-xbox)
    ├── xbe_compare.py     ← reference + comparison primitives
    └── xbe_orchestrator.py ← top-level CLI + matrix runner
```

## What each renderer driver does

### xemu-GL / xemu-Metal

1. Clone `Xbox-Emulator-Files/hdd/xbox_hdd.qcow2` (APFS `cp -c`) into a
   per-run scratch HDD so the source HDD isn't modified.
2. Write a per-run `xemu.toml` pointing at MCPX/BIOS/scratch HDD/the
   diag XBE iso (`scripts/apple-silicon/xbe-tests/<id>/<id>.iso`).
3. Launch xemu with the chosen renderer flags + the canonical recipe:
   - **Metal**: `XEMU_RENDERER=METAL`, `XEMU_METAL_TRANSLATED_PIPELINE=1`,
     `XEMU_METAL_FRONT_FB_FALLBACK=1`, `XEMU_METAL_HUD=0`,
     `XEMU_METAL_VALIDATION=1`, screenshot path/at-frame/interval set
     so a sequence of post-flip drawables lands as PNGs.
   - **GL**: `XEMU_RENDERER=GL`, `XEMU_GL_MSAA=0`, sidecar
     `macos-capture.sh` window screencapture every 2 s (no in-renderer
     PNG writer for GL).
4. Wait `timeout_seconds` (default 35 s) — long enough for Xbox boot
   (~10 s) + diag XBE render loop (300 frames ≈ 5 s) + a margin. Set
   `XBE_HARNESS_TIMEOUT_SECONDS=<int>` (cycle 17, 2026-05-22) to
   override the default when running diag flags that add per-call
   wait latency — notably `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1`, where
   the cycle-15 GL leg needs ~120 s to complete one XBE
   boot-to-tally pass instead of the usual ~25 s.
5. QMP-quit cleanly; SIGTERM/KILL fallback.
6. Return the directory of captured PNGs to the harness.

### real-xbox

1. Probe the oracle agent (`scripts/apple-silicon/oracle-orchestrator.py
   status`); if it's up, `reboot` it via the agent so dashboard FTP
   server comes back.
2. FTP-upload the diag XBE binary to
   `E:\Apps\<id>\default.xbe` (creates the dir if needed).
3. Invoke `oracle-orchestrator.py run-diag --xbe ... --ftp-collect ...
   --out ...` which:
   a. Re-launches the agent if needed (auto-detected FTP launch verb:
      `SITE EXEC` on UnleashX, `SITE RunXBE` fallback on XBMC4Gamers).
   b. Sends `runxbe` → agent chainloads the diag XBE.
   c. Waits for FTP to come back after the diag XBE reboots.
   d. FTP-pulls `<id>-capture.bin` + `<id>-done.txt` from
      `/E/Apps/<id>/`.
   e. Re-launches the agent for post-state inspection.
4. Decode the pulled XOSS blob to PNG via the same
   `oracle-client.bgrx_to_rgba` + stdlib PNG encoder used by
   `oracle-client.py screenshot`.

## How the comparison gate works

Each diag XBE's `manifest.json` declares one or more
`expected_results` entries keyed by
`<renderer>/<scale>/<msaa>[/fallback=N][/translated=N]` (with fallback
`any/any/any`). Recipe-aware keys let one XBE assert different
expected outputs per `XEMU_METAL_FRONT_FB_FALLBACK` /
`XEMU_METAL_TRANSLATED_PIPELINE` setting — see `crtc-publish` for
the canonical example. The harness picks the most-specific match
(see `xbe_compare.select_reference_key`), materializes the reference
PNG (either `real-xbox-capture` from
`docs/apple-silicon/xbox-real-references/<id>/<label>.png` or
`math-derived` from the XBE's `expected.py:<fn>()`), then:

1. For each captured PNG (xemu records many because the XBE renders
   in a loop), runs `xbe_compare.frame_quality_score` to compute
   per-frame `signal_match_pct` (fraction of non-black-in-reference
   pixels that match in the captured frame) and `total_match_pct`
   (fraction of all pixels matching).
2. Picks the screenshot with the **highest composite score
   `signal × total`**. Ties are broken by raw signal, then by
   first-seen. This separates real diag-render frames from
   happenstance-signal-matching post-reboot dashboard frames (a
   dashboard frame can score `signal=100, total=0.01 → score=1`
   while the real render scores `signal=75, total=99.99 → score=7500`).
   The composite-score selector replaced the older lowest-
   `changed_pixels_pct` / first-tie selector after the 2026-05-12 T2
   host-refresh publish exposed the dashboard-frame collision (see
   decision-log "2026-05-20 evening: xbe-harness frame selector").
3. Re-runs `compare-screenshots.py` on the chosen PNG to produce
   the canonical compare artifacts at
   `<out>/<xbe>/<renderer>[/<variant>]/compare/`. The final
   PASS/FAIL gate is `changed_pixels_pct ≤ --max-changed-pct` AND
   `signal_match_pct ≥ 99.0` (with a wider per-channel threshold of
   140 to tolerate BOX-downsample boundary AA). See
   `xbe_compare.compare`.

### Tier-4 `capture_blob` XBEs skip the drawable board

A Tier-4 visual-only XBE that declares an `artifacts.capture_blob`
oracle (`self_validation_tier == 4` + `capture_blob` in `artifacts`,
e.g. `pipeline-smoke`) is **skipped on the xemu GL/Metal drawable
board** (`status=skip`) and validated only via the real-xbox
`run-diag`/XOSS path. The xemu drawable board cannot validate it:
the XBE CPU-paints the front buffer and reboots before the
`at-frame=30` capture fires (so the frame selector lands on a
post-reboot dashboard frame and FAILs spuriously), and the XOSS
blob it writes to `D:\` is unreachable on xemu (`D:\` is the
read-only DVD). The authoritative oracle is the real-xbox leg, which
FTP-pulls + decodes the XOSS blob and compares it byte-exact against
`expected.py` (`xbe_renderers.run_real_xbox` → `decode_xoss_to_png`
→ `compare`). Gated by `XbeManifest.is_tier4_capture_blob`.
Non-Tier-4 (screenshot-validated) XBEs are unaffected. Deferred
follow-up: tiny-signal frame-selector hardening so a happenstance
single-white-pixel dashboard frame can't score `signal=100`.

### Counter-based path-activation assertion (required_counters_min)

A manifest may declare per-renderer min-counter thresholds. Choose
counters that uniquely correspond to the code paths under test —
aggregate `NATIVE_TRI_DEPTH_DRAW` / `METAL_NATIVE_TRI_DEPTH_DRAWS` are
**insufficient** for multi-path gates (e.g. `native-quad-tri-depth`'s
SMOOTH and FLAT_FIRST stripes) because one stripe alone would trivially
satisfy the aggregate min and mask a regression in the other path. The
GL counter set + the shared `NV2A_PROF_NATIVE_TRI_DEPTH_DRAW_SMOOTH` /
`NV2A_PROF_NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST` profile counters (also
incremented by the Metal renderer since 2026-05-20 evening) let a
manifest gate each provoking-vertex variant independently:

```json
"required_counters_min": {
  "gl": {
    "NATIVE_QUAD_DRAW": 100,
    "NATIVE_TRI_DEPTH_DRAW_SMOOTH": 100,
    "NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST": 100
  },
  "metal": {
    "METAL_NATIVE_QUAD_DRAWS": 100,
    "NATIVE_TRI_DEPTH_DRAW_SMOOTH": 100,
    "NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST": 100
  }
}
```

For a single-path XBE the aggregate counters are fine; the rule of
thumb is "every distinct path the XBE exercises gets its own counter
key."

After each cell runs, the harness reads `xemu-perf:` interval lines
from `cell_dir/xemu.log`, sums every named counter across all
intervals, and gates the cell PASS on every counter meeting its
declared min in addition to the pixel oracle. real-Xbox cells skip
this assertion (xemu counters don't apply on real hardware) and
manifests that don't declare a per-renderer block skip silently.

Used by `native-quad-tri-depth` to prove that the `XEMU_NATIVE_QUAD`
and `XEMU_NATIVE_TRI_DEPTH` bypass paths actually engaged. The
pixel oracle alone cannot distinguish "bypass engaged correctly"
from "bypass silently fell back to the geometry shader" — both
paint the same uniform-color cells. The counter assertion closes
that loophole (Codex 2026-05-20 evening finding).

Each cell's verdict in `summary.json` carries a `counter_assertion`
key with the observed sums, the declared mins, the number of
intervals parsed, and a one-line note explaining any shortfall.
The markdown report also surfaces a compact `counters: pass/fail
(KEY=sum/req, ...)` line per non-skipped cell.

### Per-XBE compare overrides (compare_overrides)

A manifest may relax (or tighten) the pixel-compare gate per XBE:

```json
"compare_overrides": {
  "threshold": 8,
  "max_changed_pct": 5.0,
  "min_signal_match_pct": 95.0
}
```

The override applies to BOTH the candidate-frame selection
(`frame_quality_score` uses the overridden threshold) and the final
PASS/FAIL gate (`compare(...)` uses all three). This keeps the
"best frame" definition consistent with the gate that ultimately
accepts or rejects it.

Used by grid-pattern XBEs (e.g. `native-quad-tri-depth`'s 4×3-cell
layout per half) whose many internal cell boundaries produce more
retina-downsample AA boundary pixels than the strict defaults tuned
for sparse-signal XBEs (`mirror`, `crtc-publish`, `depth-floor`,
`color-channel`). When overrides apply, the report's per-cell
artifacts line surfaces them explicitly so reviewers don't read the
header's CLI threshold as the active gate.

### Multi-recipe cells (additional_metal_recipes)

A manifest can declare `additional_metal_recipes`:

```json
"additional_metal_recipes": [
  {"name": "fallback0",
   "env": {"XEMU_METAL_FRONT_FB_FALLBACK": "0"}}
]
```

The orchestrator runs the canonical Metal cell AND one extra cell
per entry, applying the listed env overrides on top of the
canonical recipe. Each cell appears in the report's
`recipe_variant` field and nested under
`<out>/<xbe>/metal/<variant>/`. Real-Xbox cells ignore variants
(real HW publishes CRTC regardless of xemu flags). Used by
`crtc-publish` to gate both publish-path legs from a single matrix
run (Codex review, 2026-05-20).

### Per-XBE canonical-recipe overrides (metal_canonical_overrides)

A manifest can also declare `metal_canonical_overrides` (2026-05-21):

```json
"metal_canonical_overrides": {
  "XEMU_METAL_SCREENSHOT_SOURCE": "nv2a"
}
```

The orchestrator merges these env-var entries into the Metal
canonical recipe — i.e., they apply to the SINGLE canonical cell
(and propagate to any `additional_metal_recipes` variants, where
the per-variant `env` entries still win on key collision). Use
this when the XBE needs to deviate from the default Metal recipe
for a correctness reason that applies to every renderer-leg of
the cell, not for an extra-cell variant.

Concrete use case: `combiner-basic` and `swizzle-mipmap` pin
`XEMU_METAL_SCREENSHOT_SOURCE=nv2a` so the captured frame is the
linear NV2A surface (not the BGRA8Unorm_sRGB drawable, which
gamma-encodes non-saturated cell values and diverges from both
the math-derived oracle and the real-Xbox agent screenshot).
The drawable source stays the default for XBEs whose cells use
only 0/255 endpoints (gamma neutral: gamma(0)=0, gamma(1)=1).

## Per-XBE render loop pattern

Each diag XBE's `main()` follows this shape (see `mirror/main.c` for
the full example):

```c
static void render_one(uint32_t frame_idx, void *ctx) {
    /* clear, set state, draw the test pattern */
}

int main(void) {
    if (xbed_init(640, 480) != XBED_OK) return 1;
    xbed_set_default_render_state();
    xbed_load_default_shaders();
    /* allocate vertex buffer once */
    xbed_render_loop_then_capture(
        render_one, NULL, /*n_frames=*/300,
        "D:\\<id>-capture.bin",
        "D:\\<id>-done.txt",
        "<id>");
    return 0;
}
```

The render-loop pattern is critical: on xemu, only a subset of the
captured drawable PNGs land during the diag's render window (the
rest catch Xbox boot or post-reboot dashboard). 300 frames at 60 Hz
= 5 s, which gives the screenshot path plenty of opportunities. On
real Xbox the loop just wastes 5 s before the FTP-collect path runs;
acceptable.

## Adding a new XBE

1. `mkdir scripts/apple-silicon/xbe-tests/<id>/`
2. Copy `mirror/Makefile` and edit `XBE_TITLE`.
3. Write `main.c` following the render-loop pattern.
4. Author `expected.py` — math derivation in pure Python that mirrors
   the source-file header.
5. Author `manifest.json` — declare `self_validation_tier`,
   `oracle_priority`, `expected_results`, `artifacts`.
6. Run `make` (with the nxdk activate environment).
7. Run `python3 xbe-harness/xbe_orchestrator.py run --xbe <id>` to
   exercise it on the available renderers.

When real-Xbox is available, also capture the canonical reference:

```sh
python3 xbe-harness/xbe_orchestrator.py capture-reference --xbe <id>
git add docs/apple-silicon/xbox-real-references/<id>/real-xbox.png
```

## CLI reference

```text
xbe_orchestrator.py [--host HOST] CMD [args...]

CMD = list                       discover XBEs from xbe-tests/
    | probe                      probe renderer availability
    | expected --xbe ID --out P  generate math-derived expected PNG
    | capture-reference --xbe ID [--label L] [--no-upload]
                                 pull canonical real-Xbox reference
    | run [--xbe ID]... [--renderer R]...
          [--out DIR] [--surface-scale N]
          [--threshold N] [--max-changed-pct F] [--crop x,y,w,h]
          [--no-upload]          run the matrix
```

Default surface scale is 1 (forces guest 640×480 = host 640×480 per
diag-XBE-plan §2.6, resolves coordinate confusion).

Exit codes: 0 if every cell passes, 1 if any cell fails or
infra-errors.
