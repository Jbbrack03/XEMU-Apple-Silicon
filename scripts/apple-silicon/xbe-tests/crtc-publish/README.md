# crtc-publish — front-buffer publish policy oracle (Tier-1)

Diagnostic XBE for `diagnostic-xbe-plan.md` v2 §4.4. Tests whether
the renderer publishes the CRTC-pointed front buffer (canonical real
Xbox semantics) vs the fallback dominant-draw target (xemu-Metal
`XEMU_METAL_FRONT_FB_FALLBACK=1` path).

## What it does

Every render frame:

1. Bind surface A (pbkit back buffer); clear to RED 0xFFFF0000; 0
   marker draws.
2. Bind surface B (pbkit extra buffer 0) via `pb_target_extra_buffer(0)`;
   clear to GREEN 0xFF00FF00; 1 marker draw.
3. Bind surface C (pbkit extra buffer 1); clear to BLUE 0xFF0000FF;
   3 marker draws.
4. Rebind A via `pb_target_back_buffer()` so pbkit's triple-buffer
   swap chain stays consistent.
5. Manually push `NV097_FLIP_STALL` so xemu's
   `pgraph_mtl_flip_stall` handler fires (pbkit's own swap mechanism
   does NOT push this method — see comment in `main.c::s_push_flip_stall`).

Runs the pattern 300× (~5 s at 60 Hz), then writes the captured
front buffer (via `PCRTC_START`-resolved kseg0 read) to
`D:\crtc-publish-capture.bin` and reboots back to the dashboard.

## Per-recipe expected output

| Renderer / recipe | Publish path                                    | Expected color |
|---|---|---|
| Real Xbox (any)               | NV2A CRTC physically scans front          | RED  |
| xemu-GL (any)                 | flip_stall publishes CRTC-pointed         | RED  |
| xemu-Metal `fallback=0`       | `publish_display_front_fb` (CRTC)         | RED  |
| xemu-Metal `fallback=1`       | `publish_latest_draw_fallback` (cand=C)   | BLUE |

`expected.py` exposes two math-derived generators:
- `default()`        → RED   (canonical real Xbox + GL + Metal fb=0)
- `metal_with_fallback()` → BLUE (Metal fb=1)

## Build

```sh
eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && \
    make
```

Output: `bin/default.xbe` and `crtc-publish.iso`.

## Run on the real Xbox via the orchestrator

```sh
# 1. Deploy
curl -u xbox:xbox -T bin/default.xbe \
    ftp://192.168.0.200/E/XBMC4Gamers/Apps/crtc-publish/default.xbe

# 2. Chainload + collect
python3 ../../oracle-orchestrator.py run-diag \
    --xbe 'E:\XBMC4Gamers\Apps\crtc-publish\default.xbe' \
    --ftp-collect /E/XBMC4Gamers/Apps/crtc-publish \
    --out /tmp/crtc-publish-real-xbox

# 3. Decode XOSS → PNG
python3 -c "
import importlib.util as i, pathlib as p, struct
spec = i.spec_from_file_location('oc', '../../oracle-client.py')
oc = i.module_from_spec(spec); spec.loader.exec_module(oc)
data = open('/tmp/crtc-publish-real-xbox/artifacts/crtc-publish-capture.bin','rb').read()
w,h,stride = struct.unpack_from('<III', data, 4)
rgba = oc.bgrx_to_rgba(data[16:], w, h, stride)
oc.save_screenshot_png(rgba, w, h, '/tmp/crtc-publish-real-xbox/frame.png')
print('wrote /tmp/crtc-publish-real-xbox/frame.png')
"
```

## Run on xemu via the harness

This XBE's `manifest.json` declares `additional_metal_recipes` so the
standard orchestrator runs BOTH publish-path legs in one matrix
invocation (canonical + `fallback0` variant). No per-XBE sidecar
script is required.

```sh
# Runs Metal twice (canonical fallback=1 → BLUE; fallback=0 → RED)
# plus GL once (→ RED). Both Metal cells must PASS for this XBE to
# be considered green.
python3 ../../xbe-harness/xbe_orchestrator.py run \
    --xbe crtc-publish --renderer metal --renderer gl \
    --max-changed-pct 1.0 --threshold 8 \
    --out /tmp/crtc-publish-xemu
```

The orchestrator emits one cell per `(xbe, renderer, recipe_variant)`
tuple. For `crtc-publish` the Metal renderer produces two cells:

| `recipe_variant` | env overrides applied             | expected color |
|------------------|-----------------------------------|----------------|
| `canonical`      | (none — pure canonical recipe)    | BLUE  (C)      |
| `fallback0`      | `XEMU_METAL_FRONT_FB_FALLBACK=0`  | RED   (A)      |

Manual single-config generator preview:

```sh
python3 expected.py /tmp/expected-red.png  default
python3 expected.py /tmp/expected-blue.png metal_with_fallback
```

## What this catches

- **Stale fallback candidate after rebind.** If
  `publish_latest_draw_fallback` followed `s_color_binding` (A) instead
  of `s_fallback_draw_candidate` (C) when both are non-NULL, the
  fallback path would publish RED instead of BLUE — caught.
- **Surface cache cross-contamination.** If the cache reuses A's
  binding for B or C (mismatched VRAM addresses), the captured frame
  would show the wrong color or a mix — caught.
- **DMA channel reprogram not picked up by surface_update.** If
  `pb_target_extra_buffer`'s `SET_SURFACE_PITCH` push did not trigger
  `surface_update`, the renderer would keep drawing into A's binding
  for the B and C steps. In Metal+fb=1, A.frame_draw_count would
  hit 4 and outrank a (would-be) C.count of 0, publishing RED on the
  fallback path — caught by the BLUE expectation.
- **CRTC publish path republishes the wrong surface.** If
  `publish_display_front_fb` resolved `crtc_addr` to B's or C's
  binding (bug in `cache_get_within`), the fallback=0 frame would be
  GREEN or BLUE — caught.

## Math derivation

See the source-file header in `main.c` for the line-by-line
derivation (cleared color = pb_fill ARGB → captured pixel; per-recipe
publish-path mapping). The paired `expected.py` encodes the same
math in two generators. A reviewer can read both side-by-side and
confirm "yes, the math says this should output that."
