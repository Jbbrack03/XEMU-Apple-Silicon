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
   (~10 s) + diag XBE render loop (300 frames ≈ 5 s) + a margin.
5. QMP-quit cleanly; SIGTERM/KILL fallback.
6. Return the directory of captured PNGs to the harness.

### real-xbox

1. Probe the oracle agent (`scripts/apple-silicon/oracle-orchestrator.py
   status`); if it's up, `reboot` it via the agent so XBMC's FTP
   server comes back.
2. FTP-upload the diag XBE binary to
   `E:\XBMC4Gamers\Apps\<id>\default.xbe` (creates the dir if needed).
3. Invoke `oracle-orchestrator.py run-diag --xbe ... --ftp-collect ...
   --out ...` which:
   a. Re-launches the agent if needed (`SITE RunXBE`).
   b. Sends `runxbe` → agent chainloads the diag XBE.
   c. Waits for FTP to come back after the diag XBE reboots.
   d. FTP-pulls `<id>-capture.bin` + `<id>-done.txt` from
      `/E/XBMC4Gamers/Apps/<id>/`.
   e. Re-launches the agent for post-state inspection.
4. Decode the pulled XOSS blob to PNG via the same
   `oracle-client.bgrx_to_rgba` + stdlib PNG encoder used by
   `oracle-client.py screenshot`.

## How the comparison gate works

Each diag XBE's `manifest.json` declares one or more
`expected_results` entries keyed by `<renderer>/<scale>/<msaa>` (with
fallback `any/any/any`). The harness picks the most-specific match,
materializes the reference PNG (either `real-xbox-capture` from
`docs/apple-silicon/xbox-real-references/<id>/<label>.png` or
`math-derived` from the XBE's `expected.py:<fn>()`), then:

1. For each captured PNG (xemu records many because the XBE renders
   in a loop), runs `compare-screenshots.py` and parses
   `changed_pixels_pct` from stdout.
2. Picks the screenshot with the lowest `changed_pixels_pct` (i.e.
   the one that landed during the diag's render window, not during
   Xbox boot or the post-reboot dashboard).
3. Re-runs the compare on the chosen PNG and writes the artifacts to
   `<out>/<xbe>/<renderer>/compare/`. Verdict is **pass** if
   `changed_pixels_pct ≤ --max-changed-pct`, else **fail**.

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
