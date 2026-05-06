# Diagnostic XBE Library — Implementation Plan

Last updated: 2026-05-05.
Status: PLANNING. Codex-validation pending before any nxdk source is
written (project rule #15).

This document translates the
[NV2A feature surface research catalog](nv2a-feature-surface-research.md)
into a concrete per-XBE design. The catalog answers "what is in the
NV2A rendering pipeline." This plan answers "what XBEs do we build,
in what order, with what self-validation, and how do we know each
one is correct."

## 1. Purpose

Build a library of self-validating Xbox homebrew test programs (XBEs)
that exercise the NV2A rendering pipeline feature-by-feature. Each
XBE is its own oracle — its correct visual output is mathematically
derivable and (where possible) verified at runtime via CPU-side VRAM
readback, with `PASS`/`FAIL` reported on-screen via `pb_print` and
to stderr via the existing xemu logging path.

The library replaces three failure modes already observed on this
project:

1. **Counter-only validation says "renderer healthy" while the
   rendering is visually broken** (2026-05-05 SC2 Metal canonical-
   recipe replay: `METAL_PIPELINE_TRANSLATED_FAILED=0`,
   `METAL_PIPELINE_FALLBACKS=0`, but the user observed top-mirrored-
   to-bottom, missing floor, wrong colors live).
2. **xemu-GL is not a correctness oracle** — per the user, GL
   renders games ~85 % correctly. A paired Metal-vs-GL diff is at
   most a divergence detector; agreement just inherits GL's bugs.
3. **YouTube reference + structural sanity oracles** are useful for
   spot-checks but cannot catch the specific NV2A semantics bugs
   that live below the level of "scene composition looks right."

The diagnostic-XBE library replaces all three with mathematically-
derivable correctness claims. When all priority XBEs pass on Metal,
we have actual evidence that the renderer is correct on the feature
surface they cover. When a real game then misrenders, we know the
bug is *outside* the feature surface tested — narrowing the search
space substantially.

## 2. Design principles

### 2.1 Self-validation tiers (priority order)

Each XBE picks one mechanism from this priority order. The XBE's
source-file header MUST declare which it uses and why others were
not suitable.

**Tier 1 — CPU-side VRAM readback.** The XBE runs in the Xbox
guest. After issuing `pb_finished()` and waiting for GPU completion
via `pb_wait_until_gr_not_busy()`, the XBE reads pixels straight
out of the back-buffer (or a render-target VRAM offset) via plain
pointer dereference, decodes the expected color/depth/stencil per
the test's math, and renders `PASS` or `FAIL [diagnostic info]` via
`pb_print` text overlay.

This is the **default**. Works on any renderer; mathematically
deterministic; independent of any external oracle.

**Tier 2 — RT-as-texture sampling.** When CPU-side decode is
awkward (MSAA-resolved output, stencil readback, palette-decoded
texel), the XBE binds the just-rendered RT as a texture in a
follow-up draw, samples the relevant texel, and outputs it as a
solid-color block at a known screen position. Tier-1 readback
then verifies that block.

**Tier 3 — `NV097_GET_REPORT` Z-pass count.** Z-pass-only,
renderer-dependent (Metal currently writes 0 unconditionally —
`mtl/renderer.c:1917-1920`). Use as a **secondary** liveness
counter alongside Tier 1, never as the primary mechanism.

**Tier 4 — Visual-only reference.** Where neither readback nor
texture sampling is feasible (gamma test, display-side post-process
test), the XBE renders side-by-side expected vs actual reference
patterns and asks the operator to visually confirm. The XBE's
header must explicitly note that no on-GPU self-check is possible
and explain why. The XBE manifest tags it `tier4`. These are last-
resort.

### 2.2 PASS/FAIL banner format

Every XBE renders a top-of-screen banner:

```
Line 0: <XBE-ID>: PASS
Line 1: <feature> | <renderer-detected> | <frame-count>
```

or on failure:

```
Line 0: <XBE-ID>: FAIL
Line 1: <feature> | <renderer-detected> | <frame-count>
Line 2: <diagnostic-1>
Line 3: <diagnostic-2>
...
```

The banner is rendered via `pb_print` after `pb_draw_text_screen()`
so it composites over the test's draw output. `<XBE-ID>` is the
test's short slug (e.g., `mirror`, `color-channel`); `<feature>` is
the catalog section reference (e.g., `§A.6 Z perspective`);
`<renderer-detected>` is best-effort detected at runtime by reading
GPU register signatures the renderer exposes (W1 source patterns
under `pgraph_query_gpu_props`).

The diagnostic lines are the XBE's chance to say *why* it failed —
e.g., `expected pixel (640,100)=0xFFFFFFFF, got 0xFF00FFFF`,
`mirror at (640,860)`, `mip-3 base offset 0xC000 != 0xA000`.

### 2.3 Reproducibility

Every XBE must produce **byte-identical** rendered output across
two cold runs on the same renderer. No timing leaks, no
uninitialized memory, no non-deterministic input. The XBE harness
in `run-benchmark.sh` will run each XBE twice in succession and
fail if the captured outputs diverge.

XBEs that have legitimate non-determinism (e.g., a frame-count
display ticking) must isolate it to a specific sub-region of the
frame and exclude that region from the readback comparison; the
XBE manifest declares the excluded region.

### 2.4 No reliance on external oracles

The XBE's correctness argument is encoded in its source. The
header comment derives the expected output from first principles
(NV2A method semantics + math + W1 source line citations). A
reviewer auditing the XBE without running it can confirm "yes,
the math says this should output that." If the derivation can't
fit in the header without hand-waving, the XBE isn't isolated
enough yet — split it.

### 2.5 Project-rule alignment

- **No guessing (rule #1):** every claim in the XBE header cites
  the catalog or a specific NV097 method dispatch in `pgraph.c`.
- **Build tools when stuck (rule #5):** the diagnostic-XBE library
  IS the tool for "I can't tell whether a renderer is correct."
- **Don't re-validate closed default-on flags (rule #11):** XBEs
  that cover the geometry-shader-bypass paths (`XEMU_NATIVE_QUAD`,
  `XEMU_NATIVE_TRI_DEPTH`, `XEMU_PGRAPH_FAST_READ`) become the
  regression gates; existing `validate-native-tri-depth.sh`
  becomes a wrapper that runs the relevant XBEs.
- **Codex-validate the plan and the changes (rule #15):** this
  plan goes through `/codex-validate plan` before any nxdk
  source is written; every batch of new XBEs goes through
  `/codex-validate changes` before commit.

## 3. Shared infrastructure

Building 60-80 XBEs against raw pbkit will produce ~60 % copy-
pasted code. Factor out shared pieces into a small library
under `xbe-tests/lib/` (referenced by each XBE's Makefile).

### 3.1 Library layout

```
xbe-tests/
├── lib/
│   ├── xbed_runtime.h          // shared types, frame-loop helpers
│   ├── xbed_runtime.c          // pb_init wrapper, vbl loop, clear
│   ├── xbed_readback.h         // CPU-side VRAM readback decoders
│   ├── xbed_readback.c         // per-format pixel decoders
│   ├── xbed_banner.h           // PASS/FAIL banner API
│   ├── xbed_banner.c           // pb_print-based banner renderer
│   ├── xbed_vertex.h           // vertex/index buffer helpers
│   ├── xbed_vertex.c           // common attribute-array setup
│   ├── xbed_compare.h          // per-pixel comparison primitives
│   └── xbed_compare.c          // exact / epsilon / region-mask compare
├── mirror/                     // first XBE
│   ├── main.c
│   ├── vs.vs.cg
│   ├── ps.ps.cg
│   ├── Makefile
│   ├── manifest.json           // expected results per renderer
│   └── README.md
├── color-channel/
├── depth-floor/
├── ...
└── shared.mk                   // common Makefile fragment
```

### 3.2 `xbed_runtime` API (skeleton)

```c
// Return codes
typedef enum {
    XBED_OK = 0,
    XBED_FAIL_INIT,
    XBED_FAIL_READBACK,
    XBED_FAIL_VRAM_LOCK,
} xbed_status_t;

// Lifecycle
xbed_status_t xbed_init(int width, int height);  // wraps pb_init + XVideoSetMode
void          xbed_shutdown(void);

// Frame loop — each XBE calls this in main()
typedef void (*xbed_frame_fn)(uint32_t frame_idx, void *user_ctx);
void xbed_run_frames(xbed_frame_fn fn, uint32_t total_frames, void *ctx);

// Get back-buffer addr + dims for readback
uint32_t  xbed_back_buffer_phys_addr(void);   // GPU VRAM address
uint32_t  xbed_back_buffer_pitch(void);       // bytes per row
int       xbed_back_buffer_width(void);
int       xbed_back_buffer_height(void);
uint32_t  xbed_back_buffer_format(void);      // NV097 format code

// VRAM coherency: CPU sees what GPU last wrote
void xbed_sync_for_readback(void);  // pb_finished() + wait + cache invalidate
```

### 3.3 `xbed_readback` API

```c
// Decode a single pixel from VRAM into linear A8R8G8B8 (host-endian).
// Handles all NV097 RT color formats from §F.7 of the catalog.
uint32_t xbed_decode_pixel(uint32_t vram_addr,
                           uint32_t pitch,
                           int x, int y,
                           uint32_t nv097_format);

// Read an entire region into a host-allocated buffer (always returned
// as A8R8G8B8 little-endian for ease of comparison).
xbed_status_t xbed_read_region(uint32_t vram_addr,
                               uint32_t pitch,
                               int x, int y, int w, int h,
                               uint32_t nv097_format,
                               uint32_t *out_buf);  // out_buf = w*h dwords

// Decode depth from zeta surface.
float xbed_decode_depth(uint32_t zeta_addr,
                        uint32_t pitch,
                        int x, int y,
                        uint32_t nv097_zeta_format);

// Decode stencil byte from a Z24S8 zeta surface.
uint8_t xbed_decode_stencil(uint32_t zeta_addr,
                            uint32_t pitch,
                            int x, int y);
```

Implementation reads VRAM via the AGP-aliased view (`pb_agp_access`,
nxdk's `pbkit_dma.c:55-58`) so the GPU's tile cache doesn't return
stale data.

### 3.4 `xbed_banner` API

```c
// Set after init; decides default banner verdict.
typedef enum { XBED_VERDICT_PENDING, XBED_VERDICT_PASS,
               XBED_VERDICT_FAIL } xbed_verdict_t;

void xbed_banner_set_id(const char *xbe_id);
void xbed_banner_set_feature(const char *catalog_section);
void xbed_banner_set_verdict(xbed_verdict_t v);
void xbed_banner_add_diag(const char *fmt, ...);  // up to 8 lines
void xbed_banner_render(void);  // pb_erase_text_screen + pb_print + pb_draw_text_screen
```

Plus stderr/log emission via `DbgPrint`-style trace so the xemu
process log captures verdicts even without a screenshot.

### 3.5 `xbed_compare` API

```c
// Exact equality. Returns first mismatch position; -1 on full match.
int xbed_compare_exact_a8r8g8b8(const uint32_t *a, const uint32_t *b,
                                int count);

// Epsilon-tolerant comparison for filtered/AA outputs.
int xbed_compare_epsilon_a8r8g8b8(const uint32_t *a, const uint32_t *b,
                                  int count, uint8_t per_channel_eps);

// Region-mask: ignore pixels in masked region (for non-deterministic areas).
int xbed_compare_masked(const uint32_t *a, const uint32_t *b,
                        const uint8_t *mask, int count);
```

### 3.6 Standard XBE skeleton

Every XBE follows this template:

```c
/*
 * <XBE-ID>: <one-line purpose>
 *
 * NV2A feature exercised: <catalog section, e.g. §A.6 Z perspective>
 * NV097 methods:          <comma-separated symbolic names>
 * Self-validation tier:   <1 / 2 / 3 / 4>
 * Expected output:        <derived from first principles, with citations>
 *
 * Math:
 *   <derivation goes here, line-by-line, no hand-waving>
 *
 * Reproducibility: byte-identical across two cold runs (no timing
 * leaks, no uninitialized memory).
 */

#include "../lib/xbed_runtime.h"
#include "../lib/xbed_readback.h"
#include "../lib/xbed_banner.h"
#include "../lib/xbed_compare.h"

static void render_test_frame(uint32_t frame, void *ctx) { ... }
static void verify_and_report(void) { ... }

int main(void)
{
    if (xbed_init(640, 480) != XBED_OK) {
        // best-effort error report
        return 1;
    }

    xbed_banner_set_id("<XBE-ID>");
    xbed_banner_set_feature("<catalog section>");

    // Run a few warm-up frames so the renderer reaches steady state,
    // then capture-and-verify on a known frame index.
    xbed_run_frames(render_test_frame, /*total*/ 60, /*ctx*/ NULL);
    verify_and_report();

    // Hold the banner on screen indefinitely (xemu harness captures
    // a screenshot once banner is stable).
    for (;;) {
        xbed_run_frames(NULL, 1, NULL);  // null fn = just present
        xbed_banner_render();
    }
}
```

## 4. Manifest schema

Each XBE has a `manifest.json` that declares expected behavior per
renderer. Used by the xemu-side run harness to mark known-renderer
failures as expected (not gating).

```json
{
  "id": "mirror",
  "title": "Pixel mirror / viewport / scissor",
  "catalog_ref": "§A.6, §C.8, §J.1, §J.2",
  "self_validation_tier": 1,
  "primary_oracle": "cpu_vram_readback",
  "expected_pass_renderers": ["xbox-real", "gl", "metal"],
  "expected_fail_renderers": [],
  "tolerance": {
    "kind": "exact",
    "epsilon_per_channel": 0
  },
  "non_deterministic_regions": [
    {"x": 0, "y": 0, "w": 320, "h": 32, "reason": "frame counter"}
  ],
  "duration_seconds": 5,
  "screenshot_at_frame": 30,
  "depends_on_flags": [
    "XEMU_NATIVE_TRI_DEPTH=1",
    "XEMU_NATIVE_QUAD=1"
  ]
}
```

`expected_fail_renderers` is the place to record known-renderer
gaps (e.g., GL logic-op map commented out — `gl/constants.h:86`).
A test failing on a renderer in `expected_fail_renderers` is
treated as an *expected* failure and does not block promotion.
Failing on a renderer NOT in either list is a regression.

## 5. First wave — 16 priority XBEs

Each XBE below has a full spec. Build order is the list order; later
XBEs may depend on earlier ones (e.g., `color-channel` depends on
`mirror` having validated viewport correctness).

### 5.1 `mirror` — pixel-position oracle

**Catalog refs:** §A.6 Z perspective; §C.8 window clip; §J.1
viewport; §J.2 scissor.

**NV097 methods:** `SET_VIEWPORT_OFFSET`, `SET_VIEWPORT_SCALE`,
`SET_SURFACE_CLIP_HORIZONTAL/VERTICAL`, `SET_TRANSFORM_PROGRAM`,
`SET_BEGIN_END`, `DRAW_ARRAYS`, `SET_VERTEX_DATA_ARRAY_FORMAT`.

**xemu touchpoints:** `pgraph.c:2083, 2133` (viewport), `gl/draw.c:734-737`
(GL viewport apply), `pgraph.h:266` (surface_scale_factor).

**Math:**
- Back-buffer is 1280×960 (surface_scale=2 default).
- Clear to opaque black `0xFF000000`.
- Render exactly one triangle that covers exactly one pixel at
  guest coordinate `(640, 100)` after the modelview→projection→
  viewport pipeline. Triangle is sized to be sub-pixel except at
  the target.
- Vertex shader applies a known projection matrix; viewport
  scales NDC to surface coordinates with `surface_scale_factor`
  factored in.
- Expected back-buffer: pixel at host coord `(640*scale,
  100*scale) = (1280, 200)` is white `0xFFFFFFFF`; every other
  pixel is exactly `0xFF000000`.
- A correct renderer produces this. A renderer that mirrors
  top-half to bottom-half produces a second white pixel at
  `(1280, 760)` (= `100 + 480 = 580` flipped to `960-100-1 = 859`,
  scaled to `1718`, etc — the XBE computes the expected mirror
  coordinate explicitly and reports it on FAIL).

**Self-validation (Tier 1):** read entire back-buffer via
`xbed_read_region`; count non-black pixels; assert exactly one,
at the expected coordinate.

**PASS criteria:** 1 non-black pixel, color exactly `0xFFFFFFFF`,
location `(1280, 200)`. All other pixels exactly `0xFF000000`.

**FAIL diagnostics:**
- `count=N` non-black pixels (expected 1).
- `pixel-i at (x,y)=0xVALUE` for each non-black pixel.
- `mirror_suspected` if a non-black pixel exists at the predicted
  Y-flip coordinate.
- `viewport_skew` if the lone non-black pixel is at the right
  color but wrong coordinate.

**Cross-renderer notes:** all renderers expected PASS. If GL
fails, regression in `pgraph_apply_scaling_factor`. If Metal
fails, look at `MtlSurfaceBinding` / front-fb fallback path.

**Reproducibility:** banner has frame counter excluded via
non-deterministic-region mask.

**Estimated complexity:** 200 lines C + 30 lines Cg shaders.

---

### 5.2 `color-channel` — RT format and channel ordering oracle

**Catalog refs:** §F.7 RT color formats; §F.3 color masks; §A.3
vertex attribute UB_D3D vs UB_OGL byte ordering.

**NV097 methods:** `SET_SURFACE_FORMAT`, `SET_VERTEX_DATA_ARRAY_FORMAT`,
`SET_DIFFUSE_COLOR4F` / `4UB`, combiner setup for "pass diffuse to
output."

**xemu touchpoints:** `gl/constants.h:287-302` (RT format map),
`pgraph.c:2565-2568` (UB_D3D BGRA byte order),
`vertex.c:42` (S1 normalization).

**Math:**
- Render four screen-space quadrants, each filled with a known
  vertex color via DIFFUSE attribute slot 3.
- TL: `(R,G,B,A) = (1,0,0,1)` → expected back-buffer dword
  `0xFFFF0000` (A8R8G8B8 ARGB).
- TR: `(0,1,0,1)` → `0xFF00FF00`.
- BL: `(0,0,1,1)` → `0xFF0000FF`.
- BR: `(1,1,1,1)` → `0xFFFFFFFF`.
- Test runs **twice** in successive frames: once with vertex
  attribute format `_TYPE_F` (3 floats, expand_normal mapping),
  once with `_TYPE_UB_D3D` (BGRA encoding) — verifies both paths
  produce same final pixel.

**Self-validation (Tier 1):** read 1 pixel from the center of
each quadrant, assert exact match.

**PASS criteria:** all four center pixels exactly match expected
ARGB.

**FAIL diagnostics:**
- `quadrant=TL got=0xVALUE expected=0xFFFF0000` etc.
- `channel_swap_suspected: red=blue?` if TL produces `0xFF0000FF`.

**Cross-renderer notes:** the `LE_X1A7R8G8B8_*` and `Z*`/`O*`
variants in §F.7 (catalog) are not separately implemented in xemu
GL; an extended sweep XBE (priority 7) covers those.

**Estimated complexity:** 250 lines C + minimal Cg.

---

### 5.3 `depth-floor` — depth test and floor coverage oracle

**Catalog refs:** §G.1 depth test; §G.6 depth-only RT; §K.2
native_tri_depth; §H.5 surface-to-texture.

**NV097 methods:** `SET_DEPTH_TEST_ENABLE`, `SET_DEPTH_FUNC`,
`SET_DEPTH_MASK`, `SET_SURFACE_FORMAT_ZETA`, `CLEAR_SURFACE` with
Z bit, `SET_TRANSFORM_PROGRAM` (perspective projection).

**xemu touchpoints:** `glsl/psh.c` native_tri_depth path,
`gl/draw.c:648-651` (polygon offset disabled, depth in fragment
shader), `pgraph_zeta_write_enabled` (`pgraph.h:357-362`).

**Math:**
- Camera at `(0, 1.6, 0)` looking at `(0, 0, 4)` with 60° vertical
  FOV.
- Render an 8×8 grid of unit-quads in the XZ plane, each labeled
  with a unique solid color encoding `(row, column)` as
  `0xFF<row><col>00` (e.g., `(0,0) → 0xFF000000`, `(7,7) →
  0xFF707000`).
- Render a vertical wall at z=2 with color `0xFF808080` that
  occludes the back half of the floor.
- After draw: read row 4 column 4 (should be visible floor color
  `0xFF404000`); read row 7 column 0 (should be wall `0xFF808080`
  if wall occludes that cell, else floor color).

**Self-validation (Tier 1):** sample 8 known pixels from floor +
wall regions; assert depth-test produced the correct visibility
ordering.

**PASS criteria:** 8/8 sampled pixels match expected color from
the depth-buffer-projected scene.

**FAIL diagnostics:**
- `floor_disappeared: row=R col=C got=0x808080` (wall color where
  floor should be visible — depth-test inverted or write-disabled).
- `floor_through_wall: row=R col=C got=floor_color` (depth-write
  off so wall doesn't occlude).

**Cross-renderer notes:** with `XEMU_NATIVE_TRI_DEPTH=1`, fragment
shader derives depth from `gl_FragCoord`. Without it, geometry-
shader path. Both must produce the same pixel output.

**Estimated complexity:** 350 lines C + 40 lines Cg (perspective
matrix, per-cell color attribute setup).

---

### 5.4 `crtc-publish` — front-fb fallback / CRTC publish path oracle

**Catalog refs:** §H.7 CRTC publish; §3b.4 (xemu GS expansion);
§K.2 native_quad; §H.5 surface-to-texture.

**NV097 methods:** `SET_SURFACE_COLOR_OFFSET`, `SET_FLIP_READ`,
`SET_FLIP_WRITE`, `SET_FLIP_MODULO`, `FLIP_INCREMENT_WRITE`,
`FLIP_STALL`, `CLEAR_SURFACE`.

**xemu touchpoints:** `pgraph.c:989-1024, 1026-1047`,
`mtl/surface.{h,mm}` `MtlSurfaceBinding` cache,
`mtl/renderer.c::pgraph_mtl_flip_stall` (front-fb fallback path).

**Math:**
- Allocate three VRAM color surfaces at distinct addresses
  `addr_A`, `addr_B`, `addr_C` (each 1280×960 A8R8G8B8 = 4.7 MB).
- Frame 0: bind `addr_A` as RT, draw a known checkerboard
  pattern, `FLIP_STALL` with `addr_A` as front-fb.
- Frame 1: bind `addr_B` as RT, draw a different pattern (solid
  red), do NOT update CRTC publish — `addr_A` stays as front-fb.
- Frame 2: bind `addr_C` as RT, draw a third pattern (solid
  green), do NOT update CRTC publish.
- Frame 3: read the CRTC-published surface
  (`NV_PCRTC_START`) and verify what it points to.

**Self-validation (Tier 1):** read VRAM at the CRTC-published
addr; assert content matches `addr_A`'s checkerboard. If front-
fb fallback is on (default `XEMU_METAL_FRONT_FB_FALLBACK=1`)
the addr may instead be `addr_C` (the most-recent draw target);
the XBE reports which behavior occurred rather than PASS/FAIL —
this XBE is **observational** about which policy is in effect.

**PASS criteria:** XBE reports a verdict consistent with the
flag setting:
- `XEMU_METAL_FRONT_FB_FALLBACK=0`: CRTC publish == `addr_A`
  (faithful publish).
- `XEMU_METAL_FRONT_FB_FALLBACK=1`: CRTC publish == `addr_C`
  (most-recent fallback) — this is what current PGR2/Crimson
  rely on.

**FAIL diagnostics:**
- `crtc_addr=0xADDR doesn't match A or C; possibly stale
  drawable`.
- `published_content=incorrect: expected_pattern_X got_pattern_Y`.

**Cross-renderer notes:** GL renderer's CRTC publish goes through
`get_framebuffer_surface` (display-side). Metal goes through
`pgraph_mtl_surface_publish_front_fb`. Both must produce
consistent results given the same flag setting.

**Estimated complexity:** 400 lines C + minimal Cg (this XBE is
mostly state machinery and VRAM bookkeeping).

---

### 5.5 `native-quad-tri-depth` — geometry-shader-bypass regression gate

**Catalog refs:** §B.1 primitives; §C.5 smooth/flat shading;
§K.1 GS expansion; §K.2 native_tri_depth / native_quad.

**NV097 methods:** `SET_BEGIN_END_OP_QUADS`, `_OP_TRIANGLES`,
`SET_FLAT_SHADE_OP_VERTEX_FIRST/_LAST`, `SET_SHADE_MODEL_FLAT`.

**xemu touchpoints:** `gl/shaders.c:34-70` (primitive map),
`gl/vertex.c:275` (native_quad index buffer),
`glsl/geom.h:52-60` (eligibility predicates),
`glsl/psh.h:72-73` (`native_tri_depth`, `native_quad` PshState).

**Math:**
- Render a 4×3 grid of quads (12 total) using `OP_QUADS`, each
  with 4 vertices at known positions and a flat-shaded color
  derived from the provoking vertex.
- Half the grid uses `_VERTEX_FIRST` provoking; half uses
  `_VERTEX_LAST` (mixed within the same draw via state changes
  between sub-batches).
- Render the same grid via `OP_TRIANGLES` with explicit triangle
  expansion.
- Both must produce pixel-identical output.

**Self-validation (Tier 1):** read center pixel of each grid
cell from both renders; assert pairwise exact match (modulo
known per-cell expected colors).

**PASS criteria:** all 12 quad-rendered cells match the
triangle-rendered reference exactly.

**FAIL diagnostics:**
- `quad_color_wrong: cell=(r,c) tri=0xV1 quad=0xV2`
  (provoking-vertex selection bug).
- `quad_winding_diagonal_swapped` if cell colors form a checkerboard
  pattern of off-by-one (GS diagonal selection regression).

**Cross-renderer notes:** with `XEMU_NATIVE_QUAD=0`, GL renderer
uses the geometry-shader path; with `=1`, CPU expansion. Metal
always uses CPU expansion (M5.8 per-element decoder). All paths
must match this XBE's reference.

**Estimated complexity:** 300 lines C + 50 lines Cg.

---

### 5.6 `cmp-vertex-format` — packed (11,11,10) decoder oracle

**Catalog refs:** §A.3 vertex formats; §3b.5 fork-specific (M5.8
Metal CPU decoder).

**NV097 methods:** `SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP`.

**xemu touchpoints:** `vertex.c:56-75` (CMP decode),
`gl/vertex.c:124-130` (`needs_conversion` integer-attrib path),
`mtl/vertex.{c,h}` (M5.8 CPU decoder).

**Math:**
- Construct vertex data with CMP-encoded normals at known values.
  Each component is a signed 11/11/10 fixed-point in `[-1, 1]`
  range.
- Vertex shader projects the normal directly to color via:
  `color.xyz = (normal.xyz + 1.0) * 0.5`. Output is the encoded
  normal as a color.
- For each test vertex, compute `expected_color = (decoded_n + 1) * 0.5`
  via the CMP encoding math in the source comment.

**Self-validation (Tier 1):** sample one pixel per test vertex's
quad; assert color matches expected to within 1 LSB per channel
(8-bit RT after 11/10-bit decode).

**PASS criteria:** 6/6 (or however many test vertices) within
tolerance.

**FAIL diagnostics:**
- `cmp_decode_off: vertex=N expected=0xV1 got=0xV2`.
- `cmp_endian_swapped: components in wrong order`.

**Cross-renderer notes:** Metal M5.8 CPU-decoder vs GL
`needs_conversion` — both must agree.

**Estimated complexity:** 200 lines C + 30 lines Cg.

---

### 5.7 `texture-format-sweep` — full 42-code coverage

**Catalog refs:** §E.1 texture formats (full table).

**NV097 methods:** `SET_TEXTURE_FORMAT_COLOR` for each of the 42
codes; `SET_TEXTURE_OFFSET`; `SET_TEXTURE_CONTROL0_ENABLE`;
`SET_TEXTURE_FILTER_MIN/_MAG=NEAREST` (so we test format decode,
not filtering).

**xemu touchpoints:** `texture.c:26-79` (`kelvin_color_format_info_map`),
swizzle table at `swizzle.c:180-181`, S3TC decode at `s3tc.c`.

**Math:**
- For each format code, allocate a 4×4-pixel texture in VRAM with
  a known input bit pattern that decodes to a known
  expected ARGB color.
- Render a screen-aligned quad sampled from that texture.
- Tile 7×6 = 42 quads on a 1280×960 framebuffer (each ~180×160).
- Each quad's expected center pixel is computed in a static
  array indexed by format code.

**Self-validation (Tier 1):** sample center pixel of each quad;
assert color matches the format-decode expectation.

**PASS criteria:** all 42 quads match expected.

**FAIL diagnostics:** per-format failure list with input bytes,
expected ARGB, actual ARGB. Especially flag:
- `0x16 LU_IMAGE_R8B8` failing on xemu (W2-only — likely not
  handled).
- `0x2F LU_IMAGE_DEPTH_X8_Y24_FLOAT` failing on nxdk-driven
  paths (W1-only).
- The xemu-issue-#320 historic offenders.

**Cross-renderer notes:** the swizzled `SZ_*` family stresses
`unswizzle_box`; the `LC_*` YUV family stresses YUV→RGB conversion;
the `L_*` family stresses S3TC decode. Each may fail on one
renderer and pass on another — manifest declares per-renderer
expected results.

**Estimated complexity:** 600 lines C (mostly the per-format
test-vector tables), 30 lines Cg.

---

### 5.8 `swizzle-mipmap` — Z-order tile + mip-chain offset oracle

**Catalog refs:** §E.5 LOD/mipmap; §E.9 swizzled vs linear.

**NV097 methods:** `SET_TEXTURE_FORMAT_BASE_SIZE_U/V`,
`_MIPMAP_LEVELS`, `_DIMENSIONALITY=2`, `SET_TEXTURE_OFFSET`,
`SET_TEXTURE_FILTER_MIPMAP_LOD_BIAS`.

**xemu touchpoints:** `swizzle.{h,c}` `swizzle_box` /
`unswizzle_box`, `texture.c:166-219` (mip-level offset math).

**Math:**
- Allocate a 64×64 base texture with a 7-level mipmap chain (mips
  1..6 are 32×32, 16×16, 8×8, 4×4, 2×2, 1×1).
- Each mip level filled with a unique solid color: mip0=red,
  mip1=green, mip2=blue, mip3=yellow, mip4=cyan, mip5=magenta,
  mip6=white.
- Per the swizzle math (Z-order, level-N base = `level0_base +
  swizzle(level_offset)` per W3 pitfall), compute the VRAM offset
  for each level and write the solid color in swizzled order.
- Render 7 screen-aligned quads, each forcing a specific LOD via
  `MIPMAP_LOD_BIAS`. Each quad's pixel is the corresponding mip's
  solid color.
- Repeat for a linear-layout variant (different format code).

**Self-validation (Tier 1):** sample center pixel of each
mip-bias quad; assert exact match for swizzled and linear cases.

**PASS criteria:** 7+7 = 14 sampled pixels match.

**FAIL diagnostics:**
- `swizzle_mip_offset_wrong: mip=N expected_color=0xV got=0xV`
  (level-N base computed via linear offset instead of swizzle).
- `lod_bias_off_by_one: mip=N produced mip=N+1's color`.

**Cross-renderer notes:** swizzled mip math is the W3 pitfall
("level-N base is `level0_base + swizzle(level_offset)`, not
`level0_base + sum(level_sizes)`"). xemu commit `f0abe3c4`
historically fixed this.

**Estimated complexity:** 400 lines C + 30 lines Cg.

---

### 5.9 `blend-matrix` — blend factors and equations oracle

**Catalog refs:** §F.1 blend.

**NV097 methods:** `SET_BLEND_ENABLE`, `SET_BLEND_FUNC_SFACTOR/_DFACTOR`,
`SET_BLEND_EQUATION`, `SET_BLEND_COLOR`.

**xemu touchpoints:** `gl/constants.h:57-74` (blend factor map),
`gl/constants.h:76-84` (blend equation map).

**Math:**
- Render a 16×8 matrix of overlapping quad pairs (16 src factors,
  8 row-per-factor variations including each blend equation).
- Each cell: clear to a known dst color, then render a known src
  color with the cell's (sfactor, dfactor, equation).
- Expected pixel for each cell is computed by the blend math:
  `result = sfactor*src OP dfactor*dst` per the catalog tables.
- Tile 128 cells onto 1280×960 framebuffer (~80×60 each).

**Self-validation (Tier 1):** sample center pixel of each cell;
assert exact match (8-bit per channel, within 1 LSB).

**PASS criteria:** 128/128 cells match.

**FAIL diagnostics:** per-cell sfactor/dfactor/equation +
expected/actual; likely groups together if a single factor or
equation is broken.

**Cross-renderer notes:** the 7 equations include 2 Xbox-specific
`*_SIGNED` variants; emulators often skip these.

**Estimated complexity:** 500 lines C (mostly the test matrix
tables), 30 lines Cg.

---

### 5.10 `stencil-ops` — 8 stencil ops oracle

**Catalog refs:** §G.4 stencil.

**NV097 methods:** `SET_STENCIL_TEST_ENABLE`, `SET_STENCIL_FUNC`,
`SET_STENCIL_OP_FAIL/_ZFAIL/_ZPASS`, `SET_STENCIL_MASK`,
`SET_STENCIL_FUNC_REF/_MASK`.

**xemu touchpoints:** `gl/constants.h:136-146` (stencil-op map).

**Math:**
- Z24S8 zeta surface; clear stencil to a known starting value.
- Render 8 quads in sequence, each configured to perform one of
  the 8 stencil ops on a specific stencil region.
- After each op: render a follow-up quad whose color depends on
  stencil-test pass/fail, exposing the post-op stencil value to
  the color RT.
- Expected color per quad position is computed from the op math.

**Self-validation (Tier 2 — RT-as-texture):** stencil isn't
directly memcpy-readable, so use the follow-up quad to read out
stencil into color via stencil-test pass/fail; then Tier-1 read
the color RT.

**PASS criteria:** 8/8 ops produce expected post-op stencil
values.

**FAIL diagnostics:** per-op stencil expected vs actual, plus
pre-op state.

**Estimated complexity:** 400 lines C + 30 lines Cg.

---

### 5.11 `texture-filter-wrap` — filter modes and wrap modes oracle

**Catalog refs:** §E.3 filter modes; §E.4 wrap modes.

**NV097 methods:** `SET_TEXTURE_FILTER_MIN/_MAG`,
`SET_TEXTURE_ADDRESS_ADDRU/_V/_P`.

**xemu touchpoints:** `gl/constants.h:40-46` (mag-filter map),
`gl/constants.h:48-55` (wrap map — note GL_CLAMP→GL_CLAMP_TO_EDGE
approximation flagged in source comment).

**Math:**
- Allocate a 4×4 texture with each texel a unique color.
- Render quads with UV coordinates exceeding [0, 1] range; per
  wrap mode, the out-of-range pixels decode to known texels:
  - `WRAP=1`: UV 1.5 → texel(0.5*4) = texel 2.
  - `MIRROR=2`: UV 1.5 → texel(2 - 0.5)*4 = texel 6 ≡ 2 mod 4.
  - `CLAMP_TO_EDGE=3`: UV 1.5 → texel(3) (last column).
  - `BORDER=4`: UV 1.5 → border color.
  - `CLAMP_OGL=5`: UV 1.5 → similar to CLAMP_TO_EDGE in xemu's
    GL backend per the FIXME comment.
- For each filter mode, sample at sub-texel UV; expected output
  is computed per the filter math (nearest = single texel,
  linear = bilinear blend, trilinear = mipmap-blended bilinear).

**Self-validation (Tier 1):** sample center pixels of each
wrap×filter cell.

**PASS criteria:** all wrap×filter combinations within 1 LSB
of expected.

**FAIL diagnostics:** per-cell wrap, filter, expected, actual.
Specifically flags the `CLAMP_OGL` vs `CLAMP_TO_EDGE` xemu
approximation.

**Estimated complexity:** 500 lines C + 30 lines Cg.

---

### 5.12 `combiner-basic` — single-stage register combiner oracle

**Catalog refs:** §D.1-D.7 register combiners.

**NV097 methods:** `SET_COMBINER_CONTROL`,
`SET_COMBINER_COLOR_ICW/_OCW`, `SET_COMBINER_ALPHA_ICW/_OCW`,
`SET_COMBINER_FACTOR0/_FACTOR1`,
`SET_COMBINER_SPECULAR_FOG_CW0/_CW1`.

**xemu touchpoints:** `glsl/psh.c` (combiner translator),
`psh_regs.h:31-141` (full state-machine enums).

**Math:**
- Configure a single combiner stage with a known input (DIFFUSE),
  input mapping (e.g., `EXPAND_NORMAL` = `2x-1`), output operation
  (multiply by C0 constant), output scale modifier (`SHIFTLEFT_1`
  = `2x`), and channel selector.
- Render a quad with a known DIFFUSE color; expected output is
  computed by the combiner equation.
- Test a matrix of (input, mapping, op, scale) — limited subset
  to keep the XBE bounded (e.g., 16 cells = 4×4 mapping×scale,
  with one fixed input/op).

**Self-validation (Tier 1):** sample center pixel of each cell.

**PASS criteria:** 16/16 cells match.

**FAIL diagnostics:** per-cell (mapping, scale, expected, actual).

**Cross-renderer notes:** xemu translates combiners to GLSL/MSL
on both backends. Bugs in the translator surface here, not in
fixture validation.

**Estimated complexity:** 450 lines C + 50 lines Cg (combiner
setup is verbose).

---

### 5.13 `texture-shader-stages` — 19-mode texture shader oracle

**Catalog refs:** §D.8 texture stage program.

**NV097 methods:** `SET_SHADER_STAGE_PROGRAM`,
`SET_SHADER_OTHER_STAGE_INPUT`, plus per-mode supporting state
(BUMPENV registers for modes 6/7, etc.).

**xemu touchpoints:** `psh_regs.h:31-53` (mode enum).

**Math:**
- Iterate each valid (stage, mode) pair. Per-mode expected output
  is documented in `psh_regs.h:31-53` and in W3 NV_texture_shader
  spec.
- For modes that need supporting input (BUMPENV, DOT_*), provide
  pre-computed expected outputs.
- Render N quads per mode (N = number of valid stages for that
  mode); each quad's color encodes the mode index plus a per-mode
  signature.

**Self-validation (Tier 1):** sample per-quad signature pixel.

**PASS criteria:** all 19 modes produce per-mode signatures.

**FAIL diagnostics:** per-mode (stage, expected, actual). Mode
0x12 `DOT_RFLCT_SPEC_CONST` is rare and may surface bugs.

**Estimated complexity:** 700 lines C (the supporting state per
mode is verbose), 60 lines Cg.

---

### 5.14 `logic-ops` — 16 logic ops oracle (will likely flag GL regression)

**Catalog refs:** §F.4 logic ops; §3b.3 known xemu GL impl gap.

**NV097 methods:** `SET_LOGIC_OP_ENABLE`, `SET_LOGIC_OP`.

**xemu touchpoints:** `gl/constants.h:86-105` — **commented out**.

**Math:**
- 16 logic ops (CLEAR, AND, AND_REVERSE, COPY, AND_INVERTED,
  NOOP, XOR, OR, NOR, EQUIV, INVERT, OR_REVERSE, COPY_INVERTED,
  OR_INVERTED, NAND, SET).
- Render src and dst with known colors; the logic op produces
  an expected result computed bit-by-bit.

**Self-validation (Tier 1):** sample one pixel per op cell.

**PASS criteria:** all 16 ops match.

**FAIL diagnostics:** per-op (src, dst, expected, actual). On
GL, ALL 16 likely fail because the GL map is commented out;
manifest marks this as `expected_fail_renderers: ["gl"]` and
the regression gate accepts that. On Metal, this is a true
test — if Metal also fails all 16, that's a real bug to fix.

**Cross-renderer notes:** **THIS XBE IS LIKELY THE FIRST TO
FLAG A REAL METAL GAP.** xemu's Metal renderer state.c maps
similar enums. If logic ops aren't mapped on Metal either, the
XBE will FAIL on Metal too.

**Estimated complexity:** 250 lines C.

---

### 5.15 `msaa-aa-factor` — multisample AA mode oracle

**Catalog refs:** §C.4 multisampling; §K.4 anti-aliasing factor.

**NV097 methods:** `SET_ANTI_ALIASING_CONTROL`,
`SET_SURFACE_FORMAT_ANTI_ALIASING`.

**xemu touchpoints:** `surface.h:32` (`anti_aliasing` mode),
`pgraph.h:364-382` (`pgraph_apply_anti_aliasing_factor`),
`mtl/heap.h` MSAA companion textures, `mtl/draw.mm` MSAA store.

**Math:**
- Render a high-contrast diagonal edge (black-to-white) at known
  sub-pixel angle.
- For each AA mode (none / 2× / 4×), the resolved-output edge
  pixels show specific gradient values.
- Sample several pixels along the edge perpendicular; expected
  is per-mode AA gradient.

**Self-validation (Tier 1, with epsilon):** epsilon-tolerant
comparison — exact AA reconstruction varies per-sample-pattern,
but the gradient *direction* and *step count* must match.

**PASS criteria:** edge gradient profile within tolerance.

**FAIL diagnostics:** per-pixel along edge (expected gradient,
actual). Specifically flags "all-or-nothing" cases (no AA when
mode > 0).

**Cross-renderer notes:** Metal MSAA store is recent (2026-05-04
fix); regression-gates that work.

**Estimated complexity:** 300 lines C + 30 lines Cg.

---

### 5.16 `texture-dma-ab` — DMA channel A vs B parity oracle

**Catalog refs:** §E.14 texture DMA selector.

**NV097 methods:** `SET_CONTEXT_DMA_A`, `SET_CONTEXT_DMA_B`,
`SET_TEXTURE_FORMAT_CONTEXT_DMA` (low 2 bits of
`SET_TEXTURE_FORMAT`).

**xemu touchpoints:** `pgraph.c:1056` (DMA A/B handlers),
`pgraph.c:2675` (CONTEXT_DMA selector decode), `texture.c:92,
153` (DMA-translation in image and palette lookup).

**Math:**
- Allocate the same 4×4 texture data twice in VRAM, at addresses
  reachable by DMA channel A and DMA channel B respectively.
  (Practically: use a single VRAM offset but bind it through both
  DMA channels in successive draws.)
- Render two side-by-side quads; left uses DMA A, right uses
  DMA B.
- Expected output: pixel-identical halves.

**Self-validation (Tier 1):** read both quads' center pixels;
assert exact equality.

**PASS criteria:** left and right halves identical.

**FAIL diagnostics:**
- `dma_b_address_translation_off: A=0xVA B=0xVB` if the two halves
  produce different colors (one path resolved a different VRAM
  source).
- `dma_b_not_handled` if the right half is solid black or
  uninitialized memory.

**Estimated complexity:** 250 lines C + minimal Cg.

---

## 6. Second wave (catalog reference, no per-XBE spec yet)

The first wave covers:
- The 3 SC2 bug classes (mirror, color, depth/floor).
- The closed default-on flag regression gates (native_quad,
  native_tri_depth).
- The xemu GL impl gap most likely to surface (logic ops).
- The fork-specific risks (CMP, surface cache, MSAA).
- Major coverage classes (texture format sweep, blend matrix,
  stencil ops, combiner basics, texture shader, filter/wrap,
  DMA selector).

The second wave expands feature coverage. For each, the catalog
section provides the math; the per-XBE spec is straightforward
extrapolation of the first-wave pattern. Listed for build queue:

- §A.1 vertex shader instruction set — one XBE per MAC op + ILU
  op (~20 XBEs; each tests one operation against a known-input
  vector and verifies output via fragment-shader-passthrough).
- §A.2 fixed-function: 8-light combinations, fog modes (6 modes),
  texgen modes (6 modes), skinning modes (7 modes), material
  source toggles.
- §B.4 ingestion paths — one XBE per (inline buffer, inline
  elements 16-bit, inline elements 32-bit, inline arrays,
  draw_arrays); same scene, different ingestion.
- §C.1 polygon mode (point / line / fill per-face).
- §C.6 edge flags + line stipple — confirms whether xemu
  silently no-ops these (§3a.2 / §3a.3 disagreement resolution).
- §C.7.4 point sprites — experimental probe; iterates flag
  combinations to find which triggers texture-coord replacement.
- §D.13 bumpenvmap — full sweep (Bm00-11 matrix, scale, offset).
- §D.11 shadow / depth-shadow comparison — 8 compare funcs.
- §D.12 color key / alpha kill — kill modes with known patterns.
- §E.6 cube map — 6 face content known; sample expected face
  per direction.
- §E.7 3D textures — 8×8×8 volume with known voxel pattern.
- §E.8 palettized — 256-entry palette with known indices.
- §E.10 texgen-driven texture coord generation.
- §E.13 per-format pitch + image rect alignment.
- §G.5 Z compression — boundary cases (4:1 compress, 2:1 partial).
- §H.6 NV_IMAGE_BLIT 2D blit context.
- §3a.5 ARL bias — boundary-case sweep, identifies xemu issue
  #2362 over-correction directly.

Approximate total: 50-60 XBEs in the second wave plus the 16 in
the first wave = 65-75 total.

## 7. Validation procedure (per-XBE)

Each XBE proceeds through these gates before joining the regression
suite:

1. **Design review.** XBE source-file header completed (math
   derivation, citations, tier declaration). Diff goes through
   `/codex-validate plan` before any nxdk source is written —
   inline plan = the XBE header comment + manifest.json.
2. **Build.** `make` in the XBE's directory produces
   `bin/default.xbe` + `<xbe>.iso`. Build order: `lib/` first,
   then per-XBE.
3. **Self-test on xemu-GL.** Run via `run-benchmark.sh <xbe>`.
   XBE renders banner. Expected: PASS, or FAIL matching the
   manifest's `expected_fail_renderers` entry. Two consecutive
   runs: byte-identical output.
4. **Self-test on xemu-Metal.** Run via `XEMU_RENDERER=METAL
   run-benchmark.sh <xbe>`. Same pass criteria.
5. **Math audit.** Independent reviewer (or Codex) reads the
   XBE header derivation against the catalog and confirms it's
   sound. This is the step that prevents "XBE built against a
   guess" — the audit re-derives the expected output from
   first principles before trusting it.
6. **Cross-emulator (advisory).** Run on Cxbx-Reloaded if
   feasible. Disagreements logged; not blocking.
7. **Regression integration.** Add to `metal-canary-regress.sh`
   `--mode counters` rotation — XBEs that pass on Metal become
   automatic post-change smoke. XBEs marked `expected_fail` on
   a renderer skip that renderer in the regression sweep.

## 8. Build sequence

Phase 0 (infrastructure):
- `xbe-tests/lib/` shared library + `shared.mk`.
- Codex-validate the lib API.
- Build + smoke-test against the existing `flat-tri-depth/`
  XBE (refactor it to use the new lib as proof of API).

Phase 1 (first 3 priority XBEs — proof of contract):
- `mirror`, `color-channel`, `depth-floor`.
- Codex-validate as a batch (plan + first changes).
- Run on GL + Metal; confirm SC2-bug-class detection works as
  designed.
- If the catalog or contract needs revision based on what the
  first 3 reveal, revise, re-Codex-validate, then continue.

Phase 2 (rest of first wave — 13 XBEs):
- Build XBEs 4-16 in priority order.
- Codex-validate in batches of 3-4.
- Each addition runs the regression rotation immediately so we
  catch coupling bugs early.

Phase 3 (second wave — feature coverage):
- ~50 XBEs covering remaining catalog sections.
- Codex-validate per batch.
- Goal: full pipeline coverage by end of phase.

Phase 4 (use the library):
- Run full library on Metal + GL; produce a per-XBE pass/fail
  matrix.
- Failures localize the actual broken NV2A-pipeline classes per
  renderer. Fork work (Metal renderer fixes, GL gap closures)
  proceeds against this matrix.
- The goal "all 60+ XBEs PASS on Metal" is the new M15
  default-on prerequisite, replacing the (broken) "≤1 % per-pixel
  diff vs GL" criterion.

## 9. Open questions for Codex review

1. **Self-validation tier ordering** — is Tier 1 (CPU-side VRAM
   readback) actually feasible for every XBE on Metal? The Metal
   renderer's surface cache (`MtlSurfaceBinding`) may keep the
   GPU-side texture out of VRAM if the surface is private/shared.
   The XBE infrastructure needs to force a surface download
   (`pgraph_mtl_surface_download_if_dirty_at`) before CPU
   readback. Verify this path works without `XEMU_METAL_FRONT_FB_DOWNLOAD=1`.

2. **AGP-aliased VRAM access** — `pb_agp_access(ptr)` returns
   `(ptr | 0xF0000000)` to bypass the GPU tile cache for CPU reads.
   Confirm this works for color RTs (not just plain VRAM
   buffers), and confirm it interacts correctly with the Metal
   renderer's swizzled surface cache.

3. **Single-XBE total runtime** — the first-wave XBEs target ~5
   seconds of runtime each. Total first-wave runtime ≈ 80 seconds
   sequential. Second-wave ≈ 250 seconds. Is this acceptable for
   regression-gate rotation, or do we need to parallelize via
   ISO-pre-built and run all in one xemu boot?

4. **Manifest evolution** — `expected_fail_renderers` is
   per-renderer-name. As fork branches (or renderer sub-modes
   like `XEMU_METAL_FRONT_FB_FALLBACK=0`/=1) proliferate, the
   manifest may need a more structured key. Defer until needed
   or design upfront?

5. **Reproducibility enforcement** — should `run-benchmark.sh`
   wrap each XBE run in an automatic two-cold-runs-and-diff
   verification, or is that opt-in per-test? The cost is
   doubling regression-gate runtime.

6. **CMP format encoding** — §A.3 says CMP is "(11,11,10) packed
   signed normalized." Verify the exact bit layout against
   `vertex.c:56-75` before writing the cmp-vertex-format XBE
   (§5.6) test vectors. xemu source is the authority here, not
   the catalog summary.

7. **Cross-renderer expected behavior for §3a items** — the XBEs
   that test cross-witness disagreements (edge flags, line
   stipple, ARL bias) don't have a clear pre-existing expected
   PASS verdict. The XBE itself becomes the experimental probe
   that *defines* the expected behavior. Manifest's
   `expected_pass_renderers` for these XBEs starts empty and
   gets populated after the first run; is this appropriate, or
   should we write down the "real Xbox would say X" answer
   somewhere first?

## 10. Codex-validation cadence (reminder)

Per project rule #15 + catalog §8.1:

- This plan goes through `/codex-validate plan
  docs/apple-silicon/diagnostic-xbe-plan.md` BEFORE any nxdk
  source is written.
- The shared `xbe-tests/lib/` API goes through
  `/codex-validate changes` after build.
- Each XBE batch (typically 3-4 at a time) goes through
  `/codex-validate changes` before commit.
- Catalog updates that motivate XBE additions go through
  `/codex-validate plan` per catalog §8.1.
- Substantial revisions to this plan re-trigger
  `/codex-validate plan`.

## Appendix A — Worked example: `mirror` XBE source skeleton

For reviewer audit, here is the XBE source-file header that
would land for §5.1 `mirror`:

```c
/*
 * mirror — pixel-position oracle for vertex transform / viewport.
 *
 * NV2A feature exercised: §A.6 Z perspective, §C.8 window clip,
 *                         §J.1 viewport, §J.2 scissor.
 * NV097 methods:          SET_VIEWPORT_OFFSET (0x0A20),
 *                         SET_VIEWPORT_SCALE (0x0AF0),
 *                         SET_SURFACE_CLIP_HORIZONTAL/VERTICAL
 *                         (0x0200, 0x0204), SET_TRANSFORM_PROGRAM
 *                         (0x0B00), SET_BEGIN_END (0x17FC),
 *                         DRAW_ARRAYS (0x1810).
 * Self-validation tier:   1 (CPU-side VRAM readback).
 *
 * Math:
 *   Back-buffer is W=1280 H=960 A8R8G8B8 (surface_scale=2 default).
 *   Clear color: opaque black 0xFF000000.
 *
 *   Vertex shader: project NDC [-1, 1] to surface space via the
 *   identity orthographic matrix combined with the viewport
 *   transform pos = pos*scale + offset. With offset = (W/2, H/2,
 *   0, 0) and scale = (W/2, -H/2, 1, 1), NDC (0, 0) maps to
 *   surface (640, 480). To target guest coord (640, 100), the
 *   vertex's NDC is (0, (480-100)/480) = (0, 0.79166...).
 *   Surface-scaled to (1280*0, 200) = (1280, 200).
 *
 *   Triangle: render a single 1-pixel-tall, 1-pixel-wide triangle
 *   centered at NDC (0, 0.79166). Pixel coverage: exactly the host
 *   pixel (1280, 200). Color: opaque white 0xFFFFFFFF passed
 *   through a passthrough combiner.
 *
 *   Expected back-buffer:
 *     pixel(1280, 200) == 0xFFFFFFFF
 *     all other pixels == 0xFF000000
 *
 *   A renderer that mirrors top-half to bottom-half produces a
 *   second white pixel at the Y-flip coordinate:
 *     mirror_y = H - 1 - 200 = 759    (host pixel)
 *     mirror_y = (1920 - 1) - 200 ... (alternative interpretations)
 *   The XBE computes both candidate mirror coordinates and
 *   reports them on FAIL.
 *
 * Reproducibility: deterministic. Frame counter rendered in
 * top-left 64x16 region (excluded via non_deterministic_regions
 * mask in manifest.json).
 */
```

This is what the reviewer audits before approving the XBE for
build. If the reviewer can't follow the math from the header to
"yes, the expected back-buffer is correct," the XBE goes back for
clarification.
