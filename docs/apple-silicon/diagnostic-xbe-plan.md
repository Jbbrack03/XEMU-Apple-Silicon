# Diagnostic XBE Library — Implementation Plan (v2)

> **2026-05-21 (morning, XBE wave expansion) — this plan is the binding
> Metal-renderer development driver.** Per decision-log "2026-05-20
> (evening): XBE-first development loop is binding for the Metal renderer"
> and workspace `CLAUDE.md` rule #17, per-feature XBE correctness against
> this library is the primary loop. The §7 Phase 5 framing ("All N XBEs
> PASS on Metal" replaces "≤1% per-pixel diff vs GL") is now the M15
> default-on gate.
>
> **15 of 17 first-wave XBEs PASS on Metal + 2 expected_fail. 1
> unstarted.** Four new XBEs shipped 2026-05-21: §4.12 `combiner-basic`
> (PASS, byte-exact), §4.8 `swizzle-mipmap` v0.2 (expected_fail Metal+GL;
> SPEC oracle for tasks #16 + #17), §4.16 `texture-dma-ab` v0.1
> (pbkit-aliased smoke; PASS), §4.15 `msaa-aa-factor` v0.1 (path-
> activation + edge-AA-band SMOKE; PASS on canonical msaa=2 +
> msaa4 variant).
>
> **PASS on Metal (15):** `pipeline-smoke` Tier-4 plus §4.1 `mirror`,
> §4.2 `color-channel`, §4.3 `depth-floor`, §4.4 `crtc-publish`,
> §4.5 `native-quad-tri-depth`, §4.6 `cmp-vertex-format`,
> §4.7 `texture-format-sweep` v0.1, §4.9 `blend-matrix`, §4.10
> `stencil-ops` (task #14 closed via cross-queue sync fix in
> `mtl/surface.mm`), §4.11 `texture-filter-wrap` v0.1 (task #15
> closed -- was a test authoring bug, not a Metal gap), `flat-quad-
> propagation` regression gate that validates the task #13 CPU-side
> flat-color propagation fix in `mtl/vertex.c`, §4.12 `combiner-basic`
> v0.1 (4×4 mapping × scale grid), §4.15 `msaa-aa-factor` v0.1
> (MSAA path-activation + edge-AA-band SMOKE under canonical msaa=2
> + msaa4 variant; per-mode profile + AA-band lower-bound deferred
> to v0.2 per Codex 2026-05-21 finding, NEW), §4.16 `texture-dma-ab` v0.1
> (DMA-A vs DMA-B selector smoke under pbkit channel aliasing).
>
> **expected_fail (2):** §4.14 `logic-ops` -- neither GL nor Metal
> implements NV2A logic-ops; XBE serves as the SPEC for what each
> renderer needs. Feature work, not a Metal-only gap. §4.8
> `swizzle-mipmap` v0.2 (NEW) -- catches REAL renderer correctness
> gaps in BOTH renderers (task #16 Metal intra-mip swizzle collapses
> all UVs to texel 0; task #17 GL renders BLACK for MIN_LOD_CLAMP =
> MAX_LOD_CLAMP > 0). Ships as SPEC oracle; flips to PASS once
> tasks #16 + #17 close.
>
> **Unstarted (1):** §4.13 `texture-shader-stages` (19 NV2A texture
> shader modes; needs combiner-helper + texture-shader-stage
> infrastructure; significant scope deferred). §4.15 `msaa-aa-factor`
> v0.1 shipped 2026-05-21; v0.2 per-mode keyed expected_results + AA-
> band lower-bound is queued as a second-wave follow-up. See handoff.md
> for the priority order.
>
> Renderer + harness changes shipped 2026-05-21:
> - **Metal LOD-clamp + LOD-bias renderer fix** in
>   `mtl/texture_pg.c::build_sampler_desc_from_pg` + `mtl/texture.mm`.
>   Was hardcoded; now honors guest `SET_TEXTURE_CONTROL0` MIN/MAX_LOD_CLAMP
>   and `MIPMAP_LOD_BIAS`. Per-cell mip ramp in swizzle-mipmap proves
>   the fix.
> - **xbe-harness `metal_canonical_overrides`** manifest field. Per-XBE
>   env-var overrides merged into the Metal canonical recipe; used by
>   combiner-basic + swizzle-mipmap to pin
>   `XEMU_METAL_SCREENSHOT_SOURCE=nv2a` for linear capture.
> - **`XEMU_METAL_DIAG_ATTRIB_DUMP`** env-gated diagnostic that
>   narrowed task #16 to a pipeline-key issue (see handoff.md +
>   automation.md).
>
> The §4.7 v0.1 covers 4 linear 32-bit format codes (A8R8G8B8,
> X8R8G8B8, A8B8G8R8, B8G8R8A8); second wave will expand to the full
> 42-code surface. The retail-title oracle is the final acceptance
> gate, not a development driver.

Last updated: 2026-05-21 (morning, XBE wave expansion) —
combiner-basic / swizzle-mipmap v0.2 / texture-dma-ab shipped +
Metal LOD-clamp + LOD-bias renderer fix + xbe-harness
`metal_canonical_overrides` field + `XEMU_METAL_DIAG_ATTRIB_DUMP`
diagnostic narrowed task #16 to a pipeline-key /
vertex-descriptor issue (slot 9 attribute selectively omitted by
spirv-cross MSL emission). Previous banner:
2026-05-20 (evening, +3 XBEs) — §4.5
`native-quad-tri-depth` shipped (three-pass design + new manifest
fields `required_counters_min` + `compare_overrides` + Metal-side
per-mode native-tri counter increments in `mtl/renderer.c`); §4.6
`cmp-vertex-format` shipped (endpoint-only ±1 corners, narrowed
from the original (normal+1)*0.5 spec); §4.10 `stencil-ops`
shipped as `expected_fail` on Metal with documented 4-of-8
stencil-op gap (task #14); §4.14 `logic-ops` shipped as
`expected_fail` on both Metal and GL (neither implements logic
ops). New harness wiring `expected_fail_renderers` so the
rotation distinguishes manifest-declared known regressions from
real failures. Header status counter: 9 of 17 first-wave PASS,
2 of 17 expected_fail (renderer regression targets documented).
Status: SHIPPING. Plan was originally PLANNING (Codex-revalidated
post-v2 2026-05-06); 9 of 17 first-wave XBEs are now green on
xemu-Metal and feeding the regression rotation; 2 more are in
the rotation as expected_fail spec oracles.

This plan supersedes v1 (committed d57742ef47) which Codex flagged
BLOCKING because the v1 self-validation contract assumed CPU-side
VRAM readback worked on Metal — it does not without
`XEMU_METAL_FRONT_FB_DOWNLOAD=1`, and even then it tests an
intermediate state rather than the visible output. v2 inverts the
self-validation tier order: host-side capture is now primary, with
real-Xbox-captured frames as the canonical reference oracle when
available.

This plan is the implementation companion to:

- `nv2a-feature-surface-research.md` — what the NV2A pipeline is.
- `real-xbox-oracle-feasibility.md` — how the real-Xbox oracle path
  works. **Note (2026-05-06 evening):** that doc's original
  XBDM-based architecture was superseded by a custom nxdk-built
  oracle agent that ships at
  `scripts/apple-silicon/xbe-tests/oracle-agent/`. The
  capability surface (memory R/W, register access, framebuffer
  capture, XBE launch) is preserved; only the wire protocol
  changed. See decision-log "2026-05-06: Real Xbox oracle Phase 1
  — custom oracle agent supersedes XBDM" for full context. The
  diagnostic-XBE harness should target the oracle agent's TCP-9001
  protocol, not XBDM's port 731.

## 1. Purpose

Build a library of diagnostic Xbox homebrew test programs (XBEs)
that exercise the NV2A rendering pipeline feature-by-feature, with
each XBE's correct output validated by host-side framebuffer capture
and compared either against:

1. **A real-Xbox-captured reference frame** (canonical oracle, when
   real Xbox is available), OR
2. **A math-derived expected pixel buffer** (audit material; same
   math the XBE source-file header derives) when real Xbox is not
   available.

The library replaces three failure modes already observed:

1. Counter-only validation says "renderer healthy" while rendering
   is visually broken (2026-05-05 SC2 Metal canonical-recipe replay).
2. xemu-GL is ~85 % correct so paired Metal-vs-GL diff is a
   divergence detector, not a correctness oracle.
3. YouTube reference + structural sanity oracles spot-check but
   cannot catch NV2A-semantics bugs below the level of "scene
   composition looks right."

When all priority XBEs pass on Metal against either oracle, we have
actual evidence the renderer is correct on the feature surface they
cover. When a real game then misrenders, the bug is *outside* that
feature surface — narrowing the search.

## 2. Design principles

### 2.1 Self-validation tiers (v2 — inverted from v1)

Each XBE picks one mechanism. The XBE's source-file header MUST
declare which it uses and why.

**Tier 1 — Host-side capture (primary, ~95 % of XBEs).** The XBE
renders its test pattern at guest 640×480 (forced; see §2.6). The
host captures the post-HUD, pre-present drawable byte-identical to
what's displayed:

- On **xemu**, via the existing `XEMU_METAL_SCREENSHOT_PATH` and
  the F1 deterministic flip-stall trigger
  (`XEMU_CAPTURE_AT_FLIP_STALL=N`). Captures the actual visible
  output — no surface-download dependency, no
  Metal-private-texture coherency issue.
- On **real Xbox** (when available), via XBDM's `screenshot`
  command on TCP/731 (debug-kernel boot path), OR via the XBE
  itself sending the framebuffer through `libnxdk_net` (Cerbios
  boot path).

The harness compares the captured frame to either the real-Xbox
reference (canonical) or the math-derived expected pixel buffer.

**Tier 2 — Guest-side VRAM readback (escape hatch, ~5 % of XBEs).**
Only for XBEs that legitimately test guest-visible state, not
host-rendered output. Examples: "does `NV097_GET_REPORT` write the
right Z-pass count to the report DMA?", "does `NV097_IMAGE_BLIT`
preserve guest VRAM?". The XBE waits for GPU completion (`pb_finished()`,
`pb_wait_until_gr_not_busy()`), reads VRAM via `pb_agp_access()`,
verifies CPU-side, encodes the verdict into the rendered output for
host capture.

Caveat: Tier 2 on Metal **requires either `XEMU_METAL_FRONT_FB_DOWNLOAD=1`
or an explicit IMAGE_BLIT to a CPU-readable scratch buffer** for
any value the renderer wrote. Manifest declares which prerequisites
each XBE needs.

**Tier 3 — `NV097_GET_REPORT` Z-pass count (rarely useful).**
Z-pass-only, renderer-dependent. Metal currently writes 0
unconditionally (`mtl/renderer.c:1917-1920`) — so any Tier-3 XBE
will FAIL on Metal until that's implemented. Use only for guest-
state probes that explicitly test report-DMA writeback (one or two
XBEs total).

**Tier 4 — Visual-only operator confirmation (last resort).** For
XBEs where neither host capture nor guest readback is feasible
(display-side gamma / post-process tests). Source-file header must
explain why no on-GPU oracle is possible. Manifest tags `tier4`.

### 2.2 Reference oracle hierarchy

For Tier 1 host-side capture, the comparison reference is selected
in priority order:

1. **Real-Xbox capture** — canonical. Captured once per XBE per
   `(renderer, flag-recipe)` tuple via XBDM `screenshot` or XBE-
   side `libnxdk_net` upload. Stored as
   `docs/apple-silicon/xbox-real-references/<xbe-id>/<frame-id>.png`
   plus a manifest entry. The XBE's math derivation must agree with
   the captured pixels; if they disagree, either the math is wrong
   (we update the XBE) or NV2A has undocumented behavior (we update
   the catalog).
2. **Math-derived pixel buffer** — generated by a per-XBE
   `expected.py` Python module that encodes the same math as the
   XBE source-file header. Used when no real-Xbox capture is
   available. Audit-friendly: a reviewer can read the XBE's header,
   read `expected.py`, and confirm they say the same thing.
3. **Cross-renderer divergence detection** — running the XBE on
   xemu-GL and xemu-Metal and diffing the captures. Useful as a
   "look here" signal when neither real-Xbox nor math reference is
   available, but never a PASS/FAIL gate by itself (consistent with
   the project's "GL is ~85 % correct" framing).

### 2.3 PASS/FAIL banner format (advisory; harness is the authority)

The XBE renders a banner using `pb_print` for human spot-checking
during development:

```
Line 0: <XBE-ID>: PASS (or FAIL or PENDING)
Line 1: <feature> | <renderer-detected> | <frame>
Line 2-N: <diagnostic-X> on FAIL
```

For Tier 1 XBEs, the banner is informational — the host harness
makes the authoritative PASS/FAIL determination by comparing the
captured frame to the reference. For Tier 2 XBEs, the banner is
load-bearing because the verdict comes from guest-side VRAM
inspection.

### 2.4 Reproducibility (revised)

Every XBE produces **byte-identical-after-applying-mask** captured
output across two cold runs on the same renderer. The masked
regions (frame counters, banners with timestamps, etc.) are
declared in `manifest.json::non_deterministic_regions` and
subtracted from the comparison. All other pixels must match
byte-exactly across runs.

For first-wave XBEs, all rendered content during the captured frame
is fully deterministic — banners and counters render only on
debug/visual-inspection frames captured at a separate ordinal.

### 2.5 Math derivation as audit material

The XBE source-file header derives the expected output from first
principles (NV2A method semantics + math + W1 source line citations
from the catalog). The paired `expected.py` encodes the same math
in Python form. A reviewer auditing the XBE without running it can
confirm "yes, the math says this should output that." If the
derivation can't fit in the header without hand-waving, the XBE
isn't isolated enough yet — split it.

When real-Xbox captures are available, **math and real-Xbox must
agree.** Disagreement is an actionable finding: either the math is
wrong (we update) or NV2A has undocumented behavior (we update the
catalog).

### 2.6 Diagnostic-mode prerequisites

All diagnostic XBEs run with these settings, applied via a single
`--xbe-mode` switch in `run-benchmark.sh`:

- `XEMU_DISPLAY_SCALE=1` (forces guest 640×480 = host 640×480 ;
  resolves Codex finding #3 about coordinate confusion).
- `XEMU_METAL_HUD=0` (already exposed via `--metal-no-hud`; ensures
  clean drawable for capture).
- `XEMU_METAL_VALIDATION=1` (catches API misuse during development).
- `XEMU_CAPTURE_AT_FLIP_STALL=<N>` (deterministic frame trigger;
  the XBE issues exactly one `NV097_FLIP_STALL` at the validation
  point).
- `XEMU_CAPTURE_FLIP_STALL_SENTINEL=<path>` (sentinel for cross-leg
  sync; harness watches for it).
- `XEMU_PERF_LOG=0` (no perf-log noise during diagnostic runs).

For Tier-2 XBEs that need surface download, the manifest declares
`requires_flags: ["XEMU_METAL_FRONT_FB_DOWNLOAD=1"]` and the
harness sets it.

### 2.7 No reliance on external oracles

The XBE's correctness argument is encoded in (a) its source header,
(b) its paired `expected.py`, and (c) a real-Xbox capture if
available. YouTube footage, GL-as-reference, and "vague mental
model of how the game looks" are explicitly NOT reference oracles
for the diagnostic-XBE library.

### 2.8 Project-rule alignment

- **No guessing (rule #1):** every XBE source claim cites the
  catalog or a specific NV097 method dispatch in `pgraph.c`.
- **Build tools when stuck (rule #5):** the diagnostic-XBE library
  IS the tool for "I can't tell whether a renderer is correct."
- **Don't re-validate closed default-on flags (rule #11):** the
  existing `flat-tri-depth` XBE is **left untouched**. New XBEs
  go in new directories. Resolves Codex finding #10.
- **Codex-validate the plan and the changes (rule #15):** this v2
  plan goes through `/codex-validate plan` before any new nxdk
  source is written; every batch of new XBEs goes through
  `/codex-validate changes` before commit.

## 3. Shared infrastructure

Split into XBE-side (runs on the Xbox guest) and harness-side
(runs on the Mac).

### 3.1 XBE-side library (`xbe-tests/lib/`)

Pure rendering and minimal frame-loop helpers. Banner is a nice-to-
have for human spot-check, not load-bearing.

```
xbe-tests/
├── lib/
│   ├── xbed_runtime.h            // frame-loop, init, present
│   ├── xbed_runtime.c
│   ├── xbed_vertex.h             // attribute-array setup
│   ├── xbed_vertex.c
│   ├── xbed_banner.h             // pb_print-based PASS/FAIL banner
│   ├── xbed_banner.c
│   ├── xbed_net.h                // libnxdk_net wrapper for Xbox-real
│   ├── xbed_net.c                //   framebuffer-to-Mac TCP upload
│   ├── xbed_capture.h            // FLIP_STALL trigger + sentinel
│   ├── xbed_capture.c
│   └── xbed_readback.h           // Tier-2 helpers: pb_agp_access decode
│       xbed_readback.c
├── lib-smoke/                    // minimum-viable XBE that exercises
│   ├── main.c                    //   the full lib API as proof-of-API
│   ├── manifest.json
│   ├── expected.py
│   ├── README.md
│   └── Makefile
├── flat-tri-depth/               // EXISTING — UNTOUCHED
│   └── ...                       // (resolves Codex finding #10)
├── mirror/                       // first new XBE
├── color-channel/
└── ...
```

**`xbed_runtime` API (skeleton):**

```c
typedef enum {
    XBED_OK = 0, XBED_FAIL_INIT, XBED_FAIL_VRAM,
    XBED_FAIL_NET, XBED_FAIL_CAPTURE,
} xbed_status_t;

xbed_status_t xbed_init(int width, int height);  // wraps pb_init + XVideoSetMode
void          xbed_shutdown(void);

typedef void (*xbed_frame_fn)(uint32_t frame_idx, void *user_ctx);
void xbed_run_frames(xbed_frame_fn fn, uint32_t total_frames, void *ctx);

// Trigger host capture by issuing an extra NV097_FLIP_STALL.
// On xemu, F1 traps it. On Xbox-real, xbed_net captures via TCP.
void xbed_capture_now(uint32_t capture_id);
```

**`xbed_net` API (Xbox-real path; no-op on xemu):**

```c
xbed_status_t xbed_net_init(uint32_t mac_ip, uint16_t port);
xbed_status_t xbed_net_send_frame(uint32_t test_id, uint32_t frame_id,
                                  const void *fb_ptr, int w, int h,
                                  uint32_t format);
void          xbed_net_shutdown(void);
```

When built without network code (xemu-only), `xbed_net_*` are
empty inline functions.

**`xbed_readback` API (Tier 2 only):**

```c
// Read a single pixel from VRAM into linear A8R8G8B8 (host-endian).
// Uses pb_agp_access for cache-coherent CPU read.
// Caveat: on Metal, requires XEMU_METAL_FRONT_FB_DOWNLOAD=1 OR
// an explicit IMAGE_BLIT to a CPU-readable scratch first.
uint32_t xbed_decode_pixel_a8r8g8b8(uint32_t vram_addr, uint32_t pitch,
                                    int x, int y);

// Decode a single texel from a specific NV097 RT color format.
// Format dispatch table covers §F.7 of the catalog (10 RT formats).
typedef struct {
    uint32_t format_code;     // NV097_SET_SURFACE_FORMAT_COLOR_*
    uint32_t (*decoder)(const uint8_t *bytes, int x, int y, uint32_t pitch);
    int      bytes_per_pixel;
} xbed_rt_format_decoder_t;
extern const xbed_rt_format_decoder_t xbed_rt_format_decoders[10];

// Decode a single texel from a specific NV097 texture format.
// Format dispatch table covers §E.1 of the catalog (42 codes).
typedef struct {
    uint32_t format_code;     // NV097_SET_TEXTURE_FORMAT_COLOR_*
    uint32_t (*decoder)(const uint8_t *bytes, int x, int y,
                        int swizzled, uint32_t pitch);
    int      bytes_per_pixel;
} xbed_tex_format_decoder_t;
extern const xbed_tex_format_decoder_t xbed_tex_format_decoders[42];
```

(Resolves Codex finding #5: `xbed_decode_pixel` underspecified.
Per-namespace dispatch tables with explicit metadata.)

### 3.2 Harness-side library (`scripts/apple-silicon/xbe-harness/`)

Mac-side Python tooling.

```
scripts/apple-silicon/xbe-harness/
├── xbe_orchestrator.py     // top-level "run XBE on renderer X"
├── xbe_capture.py          // F1-trigger + screenshot capture
├── xbe_compare.py          // pixel comparison primitives
├── xbe_discover.py         // find XBEs from xbe-tests/*/manifest.json
├── xbe_renderers.py        // GL / Metal / xbox-real backends
├── xbox_real/
│   ├── xbdm_client.py      // XBDM TCP/731 protocol
│   ├── ftp_deploy.py       // FTP XBE to Xbox HDD
│   ├── xbmc4xbox_api.py    // XBMC4Xbox HTTP API client
│   ├── prometheos_api.py   // PrometheOS REST API client
│   └── frame_server.py     // TCP listener for libnxdk_net uploads
└── manifest_schema.json    // JSON schema for per-XBE manifest
```

The orchestrator's job:

1. Discover XBE manifests.
2. For each XBE × renderer × flag-recipe combination:
   a. Start the renderer (xemu with the right flags, or boot the
      Xbox into the right BIOS bank).
   b. Deploy the XBE if needed (FTP).
   c. Trigger XBE launch (XBMC4Xbox HTTP API or xemu CLI).
   d. Wait for the deterministic capture trigger (F1 on xemu;
      libnxdk_net handshake on Xbox-real-Cerbios; XBDM screenshot
      timing on Xbox-real-debug).
   e. Capture the frame.
   f. Compare to reference oracle (real-Xbox gold, math-derived,
      or both).
   g. Emit a per-(XBE, renderer, flag-recipe) PASS/FAIL.
3. Aggregate into a regression matrix.

### 3.3 Comparison primitives (`xbe_compare.py`)

```python
def compare_exact(captured, expected, mask=None) -> CompareResult: ...
def compare_epsilon(captured, expected, eps=1, mask=None) -> CompareResult: ...
def compare_masked_hash(captured, expected, mask) -> CompareResult: ...
```

`CompareResult` includes `passed: bool`, `n_mismatch: int`,
`first_mismatch: (x, y, expected_argb, got_argb)`, `mask_applied:
bool`, plus a side-by-side PNG output for human review.

### 3.4 Standard XBE skeleton

```c
/*
 * <XBE-ID>: <one-line purpose>
 *
 * NV2A feature exercised: <catalog section, e.g. §A.6 Z perspective>
 * NV097 methods:          <comma-separated symbolic names>
 * Self-validation tier:   <1 / 2 / 3 / 4>
 * Oracle priority:        <real-xbox / math-derived / both>
 *
 * Math derivation:
 *   <line-by-line, no hand-waving>
 *
 * Reproducibility: byte-identical-after-mask across two cold runs.
 *   Non-deterministic regions: <list, with rationale>
 */

#include "../lib/xbed_runtime.h"
#include "../lib/xbed_capture.h"
#include "../lib/xbed_banner.h"
#include "../lib/xbed_net.h"

static void render_test_frame(uint32_t frame, void *ctx) { ... }

int main(void) {
    if (xbed_init(640, 480) != XBED_OK) return 1;

    xbed_banner_set_id("<XBE-ID>");
    xbed_banner_set_feature("<catalog section>");

    // Render warm-up frames (renderer reaches steady state),
    // then render the validation frame and trigger capture.
    xbed_run_frames(render_test_frame, /*warm_up*/ 30, NULL);
    render_test_frame(/*frame*/ 0xCAFE, /*ctx*/ NULL);
    xbed_capture_now(/*capture_id*/ 1);

    // Hold banner indefinitely for human spot-check.
    for (;;) { xbed_run_frames(NULL, 1, NULL); xbed_banner_render(); }
}
```

### 3.5 Manifest schema (revised, per-flag-keyed expected results)

```json
{
  "id": "mirror",
  "title": "Pixel mirror / viewport / scissor",
  "catalog_ref": "§A.6, §C.8, §J.1, §J.2",
  "self_validation_tier": 1,
  "oracle_priority": ["real-xbox", "math-derived"],
  "duration_seconds": 5,
  "capture_at_flip_stall_ordinal": 30,
  "non_deterministic_regions": [],
  "requires_flags": [
    "XEMU_DISPLAY_SCALE=1",
    "XEMU_METAL_HUD=0"
  ],
  "expected_results": {
    "real-xbox/cerbios/default": {
      "kind": "real-xbox-capture",
      "ref": "xbox-real-references/mirror/cerbios-default.png",
      "tolerance": "exact"
    },
    "real-xbox/debug/xbdm": {
      "kind": "real-xbox-capture",
      "ref": "xbox-real-references/mirror/debug-xbdm.png",
      "tolerance": "exact"
    },
    "xemu/gl/scale=1/msaa=0": {
      "kind": "math-derived",
      "generator": "expected.py:gl_no_msaa",
      "tolerance": "exact"
    },
    "xemu/metal/scale=1/msaa=0/fallback=0": {
      "kind": "math-derived",
      "generator": "expected.py:metal_no_msaa_no_fallback",
      "tolerance": "exact"
    },
    "xemu/metal/scale=1/msaa=0/fallback=1": {
      "kind": "math-derived",
      "generator": "expected.py:metal_no_msaa_fallback",
      "tolerance": "exact"
    }
  },
  "expected_fail_renderers": []
}
```

(Resolves Codex finding #4 / §9.4: per-(renderer, flag-recipe) keyed
expected results. Resolves §9.7: each XBE explicitly declares which
oracle for which configuration.)

`expected.py` exposes one Python function per `generator` key. Each
function takes `(width, height, **flags)` and returns a `bytes` /
`numpy.ndarray` of expected ARGB pixels.

## 4. First wave — 16 priority XBEs (revised)

Each spec is concise; full math derivation lives in the per-XBE
source-file header that gets reviewed pre-build.

### 4.1 `mirror` — pixel-position oracle (Tier 1)

**Catalog refs:** §A.6, §C.8, §J.1, §J.2.

**Math (revised per Codex finding #6):** uses programmable VS path
(matches existing flat-tri-depth pattern). VS computes screen-space
output via `surfaceSize` per `glsl/vsh-prog.c:753`; we don't touch
`SET_VIEWPORT_SCALE/OFFSET`. Forced 640×480 surface scale. Render
single-pixel triangle at guest coord `(320, 50)` on opaque-black
back-buffer. Expected: pixel `(320, 50)` exactly `0xFFFFFFFF`, all
others exactly `0xFF000000`. Mirror-bug detection: pixel at
`(320, H-1-50) = (320, 429)` should be `0xFF000000` but FAIL
case shows `0xFFFFFFFF` there.

**Oracle:** real-xbox (canonical) + math-derived (audit).

**Catches:** SC2 top-mirrored-to-bottom symptom.

### 4.2 `color-channel` — RT format and channel ordering (Tier 1)

**Catalog refs:** §F.7, §F.3, §A.3.

Render four screen-space quadrants with known `_TYPE_F` and
`_TYPE_UB_D3D` vertex DIFFUSE colors. Expected: TL `0xFFFF0000`,
TR `0xFF00FF00`, BL `0xFF0000FF`, BR `0xFFFFFFFF`. Test runs both
attribute formats in successive frames; both must produce same
final pixel.

**Catches:** SC2 wrong-colors symptom.

### 4.3 `depth-floor` — depth test + native_tri_depth (Tier 1)

**Catalog refs:** §G.1, §G.6, §K.2, §H.5.

Camera at `(0, 1.6, 0)` looking at `(0, 0, 4)`. 8×8 floor grid in
XZ plane, each cell colored `0xFF<row><col>00`. Wall at z=2
`0xFF808080` occluding back rows. Sample center pixel of each cell;
expected per-cell color matches depth-test visibility.

**Catches:** SC2 floor-disappearing symptom.

### 4.4 `crtc-publish` — front-fb publish policy oracle (Tier 1, SHIPPED 2026-05-20 evening)

**Catalog refs:** §H.7, §3b.4, §K.2, §H.5.

**Status:** PASS on xemu-Metal both legs (2026-05-20 evening). The
canonical Metal recipe + `additional_metal_recipes: [{name:
"fallback0", env: {XEMU_METAL_FRONT_FB_FALLBACK: "0"}}]` give two
cells per matrix run, both 0.0% changed at 100% signal + total
match.

**Design (final shipped version):**

Surface A is the pbkit back buffer (CRTC-pointed after pbkit's
triple-buffer swap rotates this buffer to front). Surfaces B and C
are pbkit extra buffers requested via `pb_extra_buffers(2)` before
`xbed_init`; `pb_target_extra_buffer(0/1)` switches the rendering
target by reprogramming DMA channel 9 base AND pushing
`NV097_SET_SURFACE_PITCH` (`NV20_TCL_PRIMITIVE_3D_BUFFER_PITCH` =
`0x20c`, same numeric value as the NV097 method dispatch), which
reaches xemu's `surface_update` so the Metal renderer re-evaluates
the bound surface and the new VRAM addr lands in the per-VRAM cache.

Per frame:

1. Clear A to **RED** (0xFFFF0000). 0 marker draws — A.frame_draw_count
   stays at 0.
2. Switch to B; clear to **GREEN** (0xFF00FF00); 1 marker draw.
   B.frame_draw_count = 1.
3. Switch to C; clear to **BLUE** (0xFF0000FF); 3 marker draws.
   C.frame_draw_count = 3 — highest count, so C wins
   `s_fallback_draw_candidate` per `surface.mm:1944-1949`.
4. `pb_target_back_buffer()` + a no-op `xbed_clear_color_argb(COL_A)`
   to actually rebind A as `s_color_binding` via
   `mtl_bind_current_surfaces` (Codex 2026-05-20 review: target_back_buffer
   alone doesn't trigger a bind; the bind happens lazily on the next
   clear/draw). The extra clear keeps A.frame_draw_count at 0.
5. Manually push `NV097_FLIP_STALL` so `pgraph_mtl_flip_stall` fires.
   pbkit's `pb_finished` does NOT push this method — it uses a
   `PB_FINISHED` subprog + DPC + direct PCRTC_START write. Without
   the manual push, the renderer's flip-stall publish path never
   runs and the fallback policy can't be tested.

The captured front buffer reflects whichever surface the renderer's
flip-stall handler published. Manifest declares per-flag-setting
expected reference (math-derived; canonical real-Xbox reference
pending Tier-1 XBE D:\ fopen fix):

- `real-xbox/any/any`        → `expected.py:default`        (RED — real HW scans CRTC).
- `gl/any/any`               → `expected.py:default`        (RED — GL has no fallback).
- `metal/scale=1/msaa=0/fallback=0` → `expected.py:default`        (RED — `publish_display_front_fb` publishes CRTC).
- `metal/scale=1/msaa=0/fallback=1` → `expected.py:metal_with_fallback` (BLUE — `publish_latest_draw_fallback` publishes candidate C).

**Catches:**

- Stale fallback candidate after rebind. If `publish_latest_draw_fallback`
  fell through to `s_color_binding` (A, last bound) instead of using
  `s_fallback_draw_candidate` (C, highest count), the test would
  publish RED on fallback=1 instead of BLUE.
- Surface cache cross-contamination — A's binding reused for B/C
  would show wrong color.
- `pb_target_extra_buffer`'s `SET_SURFACE_PITCH` push not triggering
  `surface_update` — would keep drawing into A for all 3 steps and
  let A win the candidate race.
- `publish_display_front_fb` resolving `crtc_addr` to the wrong
  cache entry — fallback=0 leg would show GREEN or BLUE instead of
  RED.

(Resolves v1 Codex finding #4: was a category error — guest-side
observation can't classify host-side fallback behavior. v2 / shipped
uses host capture which sees the actual published frame.)

### 4.5 `native-quad-tri-depth` — GS-bypass regression gate (Tier 1)

**Catalog refs:** §B.1, §C.5, §K.1, §K.2.

Render three stripe-passes per frame:

- **PASS 1** — top half, OP_QUADS, `SHADE_MODEL_SMOOTH`, 12 cells.
  Engages `NATIVE_QUAD` (per `glsl/geom.c:162-197` — eligible for
  smooth + FILL on both faces + QUADS/QUAD_STRIP). Every cell's 4
  verts carry the same expected color so smooth interpolation
  produces a uniform cell.
- **PASS 2** — bottom half rows 0-1, OP_TRIANGLES,
  `SHADE_MODEL_SMOOTH`, 8 cells. Engages `NATIVE_TRI_DEPTH`'s
  smooth path (per `glsl/geom.c:135-160`). All 6 verts per cell
  carry the same expected color.
- **PASS 3** — bottom half row 2, OP_TRIANGLES, `SHADE_MODEL_FLAT`
  + `FLAT_SHADE_OP=VERTEX_FIRST`, 4 cells. Engages
  `NATIVE_TRI_DEPTH`'s first-provoking branch (per
  `glsl/geom.c:156` — flat is eligible only when
  `first_vertex_is_provoking`). Per-cell: TL=EXPECTED,
  TR/BR/BL=BLACK distractor. Each emitted triangle's vertex 0 = TL
  must propagate via the renderer's manual flat-color path.

The math-derived expected output is the SAME 4×3 grid in both
halves: each cell takes the same saturated 0/255-RGB color
regardless of which pass produced it. Top-vs-bottom byte-equality
is the pixel gate.

**Not exercised** (deferred to a follow-up XBE
`flat-quad-propagation` + a tracked Metal-renderer slice):
FLAT-shaded OP_QUADS, where NV2A's quad rule fixes vertex 3 as
the provoking vertex. `NATIVE_QUAD` is NOT eligible for flat-
shaded quads (`glsl/geom.c:186`), and Apple Silicon Metal has no
native geometry-shader stage (`shader_validation.c:206-228`,
`state.h:29-32`). The first run of this XBE on Metal
(2026-05-20 evening, late) included a FLAT-quad stripe and
exposed Metal rendering it all-BLACK — a real correctness gap.
That stripe has been removed from this XBE so it gates only what
the renderer claims to support today, and the gap is filed as a
tracked follow-up (decision-log "2026-05-20 (evening, late):
native-quad-tri-depth XBE caught Metal FLAT-quad gap").

The two halves are arranged so the math-derived expected output is
the SAME 4×3 grid in both halves: each stripe's cells take the same
saturated 0/255 RGB color in the same position, regardless of which
path produced them. Top-vs-bottom byte-equality after the harness
crops out the non-relevant non-cell pixels.

**Path-activation assertion (mandatory, not just pixel match).**
Pixel equivalence alone cannot prove the bypass actually engaged —
a silent fall-back to the geometry shader can produce the same
pixels. The manifest declares `required_counters_min` so the
harness reads `xemu-perf:` interval lines from `xemu.log`, sums
per-counter across intervals, and asserts BOTH a per-stripe-
specific counter for the native_tri_depth path (so PASS 3 SMOOTH
and PASS 4 FLAT_FIRST each contribute and a regression in only
one path is caught):
  - GL renderer: `NATIVE_QUAD_DRAW >= 100`,
    `NATIVE_TRI_DEPTH_DRAW_SMOOTH >= 100`, AND
    `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST >= 100`.
  - Metal renderer: `METAL_NATIVE_QUAD_DRAWS >= 100`,
    `NATIVE_TRI_DEPTH_DRAW_SMOOTH >= 100`, AND
    `NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST >= 100`.

  (The two `NATIVE_TRI_DEPTH_DRAW_{SMOOTH,FLAT_FIRST}` counters are
  shared `NV2A_PROF_*` profile counters incremented by both the GL
  and Metal renderers; the Metal renderer increment was added
  2026-05-20 evening (late) to `mtl/renderer.c` mirroring
  `gl/draw.c:422-428` so this XBE could discriminate the SMOOTH and
  FLAT_FIRST native_tri paths via xemu-perf alone, without a Metal-
  specific counter. The aggregate `METAL_NATIVE_TRI_DEPTH_DRAWS`
  counter is intentionally NOT asserted: PASS 3 SMOOTH alone would
  satisfy any aggregate threshold and mask a PASS 4 FLAT_FIRST
  regression.)

  - real-Xbox cell: no counter assertion (xemu counters don't apply).

The 100-min thresholds are well below what the XBE produces over its
300-frame render loop (one draw call per stripe per frame: 300
`NATIVE_QUAD_DRAW`, ~300 `NATIVE_TRI_DEPTH_DRAW_SMOOTH`, ~300
`NATIVE_TRI_DEPTH_DRAW_FLAT_FIRST` per renderer at 30+ fps). 100
gives ~3× cushion against frame-rate slowdowns and harness early-
termination.

**Closed default-on flag regression gate:** `XEMU_NATIVE_QUAD`,
`XEMU_NATIVE_TRI_DEPTH`.

**Codex-validated 2026-05-20 evening (late).** Initial v0.1 spec
("4×3 grid of quads with mixed flat-shade provoking-vertex
selection; same grid via OP_TRIANGLES reference; pairwise center-
pixel match") was Codex-flagged BLOCKING for two reasons: (a)
uniform-per-cell vertex colors with SMOOTH shading make
diagonal/provoking-vertex choice invisible, and (b) pixel-only
oracle cannot distinguish "bypass engaged correctly" from "bypass
silently fell back to GS." The spec above resolves both: the FLAT
stripe makes provoking-vertex selection visible per primitive, and
the `required_counters_min` mandate gates path activation
quantitatively rather than only by visual equivalence.

### 4.6 `cmp-vertex-format` — packed (11,11,10) decoder (Tier 1)

**Catalog refs:** §A.3, §3b.5.

CMP layout (Codex-resolved per `vertex.c:56`): X bits 0-10, Y bits
11-21, Z bits 22-31, signed normalized by 1023/1023/511.

**Shipped 2026-05-20 evening (+3 XBEs).** v1 specified a custom VS
projecting normal to color via `(normal+1)*0.5` with ±1 LSB
tolerance to cover intermediate normals. The shipped XBE
deliberately narrows scope to the 8 ±1-corner CMP encodings —
their decoded normals land at saturated [0, 1] after the
framebuffer's natural clamp, producing the 8 corners of the RGB
cube (RED / GREEN / BLUE / WHITE / YELLOW / CYAN / MAGENTA /
BLACK) byte-exact across renderers with no display-gamma artifact.

What this catches: bitfield-range / shift-offset bugs, sign-
extension bugs (the BLACK -1/-1/-1 cell would decode positively
without sign-extend and saturate to non-BLACK), component-ordering
bugs (RED ↔ BLUE swap at asymmetric corners), and renderer-path
divergence between GL's GLSL `bitfieldExtract` decoder
(`vsh.c:203-208`) and Metal's CPU-side decoder in
`mtl/vertex.c:130-157`.

What this does NOT catch (fundamental byte-quantization limits,
documented in the XBE source-file header): sub-LSB divisor errors
(1023 vs 1024 at the max-positive encoding is ~0.25 LSB,
invisible at 8-bit quantization) and missing decoder-side clamp of
the slightly-out-of-range -1024/1023 ≈ -1.001 (the framebuffer's
[0, 1] clamp subsumes it). Mid-range coverage with a custom
`(normal+1)*0.5` VS — the original v1 design — remains queued as
a second-wave follow-up. PASS on Metal verified at
`/tmp/cmp-vertex-format-firstrun/`.

### 4.7 `texture-format-sweep` — full 42-code coverage (Tier 1)

**Catalog refs:** §E.1.

Tile 7×6 quads on 640×480; each samples one format from §E.1
(includes W1-only `0x2F` and W2-only `0x16`). Sample center pixel
per cell; expected ARGB from per-format static table.

### 4.8 `swizzle-mipmap` — Z-order tile + mip-chain (Tier 1)

**Catalog refs:** §E.5, §E.9.

64×64 base + 7-level chain; each mip a unique solid color;
swizzled vs linear layouts; force LOD via `MIPMAP_LOD_BIAS`;
sample per-mip expected color.

### 4.9 `blend-matrix` — 16 sfactors × 8 dfactor/equation rows (Tier 1)

**Catalog refs:** §F.1.

128 cells on 640×480; each cell = (sfactor, dfactor, equation)
with known src and dst colors; expected per-cell pixel computed
from blend math.

### 4.10 `stencil-ops` — 8 stencil ops via color probe (Tier 1, redesigned per Codex finding #7)

**Catalog refs:** §G.4.

Z24S8 zeta; clear stencil to known value; render 8 quads each
performing one of the 8 stencil ops; render follow-up quads whose
**color** is gated by stencil-test pass/fail at known reference
values. Captured frame encodes post-op stencil state into color.

(Resolves Codex finding #7: v1 proposed Tier-2 RT-as-texture for
stencil readback, but Metal download skips depth/stencil. v2 uses
color-via-stencil-compare probes; harness decodes from captured
color frame.)

### 4.11 `texture-filter-wrap` — filter + wrap modes (Tier 1)

**Catalog refs:** §E.3, §E.4.

4×4 texture; UV-overrun quads with each (filter, wrap) combination;
expected per-cell pixel from filter math (with epsilon for linear/
trilinear).

### 4.12 `combiner-basic` — single-stage combiner ops (Tier 1)

**Catalog refs:** §D.1-D.7.

16-cell matrix of (input mapping × output scale modifier) at fixed
input/op; expected color per cell from combiner equation.

### 4.13 `texture-shader-stages` — 19 modes (Tier 1)

**Catalog refs:** §D.8.

Iterate each valid (stage, mode) pair from the 19-mode enum; per-
mode signature pattern.

### 4.14 `logic-ops` — 16 ops (Tier 1, expected_fail Metal+GL per Codex finding #9)

**Catalog refs:** §F.4, §3b.3.

Render with each logic op against known dst; expected = bitwise op
result.

**Manifest:** `expected_fail_renderers: ["xemu/gl", "xemu/metal"]`
(Codex confirmed Metal also lacks logic-op support per
`mtl/shaders.mm:451`). The XBE still runs and reports per-op
failure list — provides the spec for what each renderer needs to
implement.

### 4.15 `msaa-aa-factor` — MSAA gradient profile (Tier 1, epsilon)

**Catalog refs:** §C.4, §K.4.

High-contrast diagonal edge at sub-pixel angle; per AA mode
(none/2×/4×); sample edge perpendicular; expected gradient
profile per mode.

**v0.1 shipped 2026-05-21 (NARROWED SCOPE per Codex 2026-05-21
finding).** v0.1 renders one solid-WHITE triangle (60,60)-(60,420)-
(580,240) on solid-BLACK; the two diagonals advance ~0.346 px/px so
every column inside [60,580] places the edge at a distinct sub-pixel
position. The XBE is MSAA-agnostic; `XEMU_METAL_MSAA` on the host
renderer governs whether the edge resolves to a hard step or per-
coverage gradient. v0.1 is structured as a **MSAA path-activation +
edge-AA-band PRESENT smoke test**, NOT the full per-mode gradient-
profile test the spec ultimately calls for. Two Metal cells per matrix
run: canonical (`XEMU_METAL_MSAA=2`) + `msaa4` variant
(`additional_metal_recipes`). Hard-step math oracle with
`compare_overrides.max_changed_pct=3.0` absorbs the ~0.7-0.9% AA band.
Counter gate: `METAL_MSAA_RESOLVE_COUNT >= 100` AND
`METAL_MSAA_SAMPLE_COUNT >= 12` (sum across intervals; rules out the
"sample-count silently coerced to 1" regression class). Validation:
canonical PASS changed_pct=0.7855%, msaa4 PASS changed_pct=0.8626%
(monotonically wider band with more samples — the expected signature).

**v0.2 deferred follow-up** (queued as second-wave): (a) per-mode
keyed `expected_results` so a 4× → 2× collapse fails; (b) positive
lower-bound assertion on AA-band pixel count (rejects a pure hard-step
output even when the upper compare bound is met); (c) optional edge-
perpendicular probe lines with renderer-tolerant gradient-shape
oracle. Both Codex MAJOR-severity findings from 2026-05-21 adopted as
v0.2 work (see decision-log entry for the slice).

### 4.16 `texture-dma-ab` — DMA channel A vs B (Tier 1, redesigned per Codex finding #8)

**Catalog refs:** §E.14.

Allocate two copies of identical texture content at **DIFFERENT
VRAM base addresses** addr_A and addr_B (NOT identical bases as
v1 proposed). Configure DMA channel A's base to addr_A; DMA channel
B's base to addr_B. Render two side-by-side quads; left binds
texture via `NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA=0` (channel A);
right via `=2` (channel B). Expected: pixel-identical halves
(both channels resolve correct texture content from their
respective bases).

(Resolves Codex finding #8: v1 used identical bases; that only
tested selector decode. v2 uses different bases to actually test
DMA-channel address translation.)

## 5. Second wave — feature-coverage XBEs (catalog reference)

50-60 additional XBEs covering remaining catalog sections. Each
follows the same template and validation tier. Build queue
ordered by `(priority, complexity)`:

- §A.1 vertex shader instruction set — one XBE per MAC + ILU op
  (~20 XBEs total).
- §A.2 fixed-function: 8-light, fog modes, texgen, skinning,
  material-source toggles.
- §B.4 ingestion paths — one per (inline buffer, inline elements
  16/32-bit, inline arrays, draw_arrays).
- §C.1 polygon mode (point/line/fill per face).
- §C.6 edge flags + line stipple — confirms whether xemu silently
  no-ops these (§3a.2/§3a.3 catalog disagreement resolution).
- §C.7.4 point sprites — experimental probe; iterates flag
  combinations to identify true Xbox enable source.
- §D.13 bumpenvmap — full Bm00-11 + scale + offset sweep.
- §D.11 shadow / depth-shadow comparison.
- §D.12 color key / alpha kill.
- §E.6 cube map.
- §E.7 3D textures.
- §E.8 palettized.
- §E.10 texgen-driven coords.
- §E.13 per-format pitch + image rect alignment.
- §G.5 Z compression boundary cases.
- §H.6 NV_IMAGE_BLIT 2D blit (Tier 2 — guest VRAM oracle).
- §3a.5 ARL bias — boundary-case sweep; identifies xemu issue
  #2362 over-correction directly.

Approximate total: ~70 XBEs.

## 6. Validation procedure (per-XBE)

7 gates before joining the regression suite:

1. **Design review.** Source-file header completed (math, citations,
   tier, oracle priority). Manifest validated against schema. Diff
   passes `/codex-validate plan` (XBE source header + manifest
   are the inline plan).
2. **Build.** `make` produces `bin/default.xbe` + `<xbe>.iso`.
   Build order: `lib/` first, then `lib-smoke/`, then per-XBE.
3. **Self-test on xemu-GL.** PASS or matches manifest's
   `expected_fail` entry. Two consecutive runs: byte-identical
   after applying mask.
4. **Self-test on xemu-Metal.** Same criteria.
5. **Math audit.** Independent reviewer (or Codex) reads the XBE
   header derivation against the catalog; reads paired `expected.py`;
   confirms they say the same thing. This is the step that prevents
   "XBE built against a guess."
6. **Real-Xbox capture (when hardware available).** Run XBE on
   Xbox-real (Cerbios bank for Tier 1 self-instrumented; debug
   bank + XBDM for Tier 1 host-capture); save reference frame to
   `docs/apple-silicon/xbox-real-references/<xbe-id>/<config>.png`;
   manifest entry promoted from `math-derived` to `real-xbox-capture`.
7. **Regression integration.** Add to `metal-canary-regress.sh`
   `--mode counters` rotation; XBEs that PASS on Metal become
   automatic post-change smoke. `expected_fail` renderers skip
   that renderer in the regression sweep.

## 7. Build sequence

**Phase 0 — Mac-side prep + xbe-tests/lib/ (Day 1-3, Mac-only).**

- Build `xbe-tests/lib/` (xbed_runtime, xbed_vertex, xbed_banner,
  xbed_capture, xbed_net stubs, xbed_readback dispatch tables).
- Build `lib-smoke` XBE — minimum-viable test that exercises every
  lib helper. Acts as "did the lib build correctly?" gate. Does
  NOT touch `flat-tri-depth/` (Codex finding #10).
- Build harness skeleton (`xbe-orchestrator.py` + per-renderer
  backends). xemu-GL and xemu-Metal backends first.
- Codex-validate the harness + lib API as `changes` mode batch.

**Phase 1 — first 3 priority XBEs (Day 4-7).**

- `mirror`, `color-channel`, `depth-floor`.
- Codex-validate as `changes` batch.
- Run on xemu-GL + xemu-Metal; confirm SC2-bug-class detection
  works as designed.
- If catalog or contract needs revision based on what the first 3
  reveal, revise + re-Codex-validate before continuing.

**Phase 2 — rest of first wave (Week 2).**

- XBEs 4-16 in priority order.
- Codex-validate in batches of 3-4.
- Each addition runs the regression rotation immediately to catch
  coupling bugs early.

**Phase 3 — Real-Xbox bring-up + reference captures (Week 3, IF
user retrieves Xbox).**

- Phase 0 of `real-xbox-oracle-feasibility.md` (Mac-side prep
  already done in this plan's Phase 0).
- Phase 1-2 of feasibility doc: hardware bring-up + resolve
  unknowns.
- Capture real-Xbox reference frames for first-wave XBEs;
  promote manifest entries from `math-derived` to `real-xbox-capture`.

**Phase 4 — second wave (Week 4-6).**

- ~50 XBEs covering remaining catalog sections.
- Codex-validate per batch.
- Real-Xbox reference captures as XBEs come online (if hardware
  is up).

**Phase 5 — use the library (ongoing).**

- Run full library on Metal + GL; produce per-(XBE, renderer,
  flag-recipe) PASS/FAIL matrix.
- Failures localize broken NV2A-pipeline classes per renderer.
- Fork work (Metal renderer fixes, GL gap closures) proceeds
  against this matrix.
- "All N XBEs PASS on Metal" replaces the (broken) "≤1 % per-pixel
  diff vs GL" criterion as the new M15 default-on prerequisite.

## 8. Open questions (revised, post-Codex)

Codex resolved most v1 open questions; new ones for v2:

1. **`xbed_capture_now()` mechanism on Xbox-real-Cerbios path.** No
   XBDM available on Cerbios. The XBE itself must do the capture
   via `libnxdk_net` upload. Verify the existing xemu-side
   `XEMU_CAPTURE_AT_FLIP_STALL` logic doesn't interfere when
   running on real hardware (the renderer-side hook is in xemu's
   Metal renderer; on real Xbox it's a no-op, so the XBE-side
   `libnxdk_net` upload is the only path).
2. **xbox-real-references manifest schema.** Naming convention
   for per-config reference frames (`<bank>-<flag-recipe>.png`).
   Should we hash flag recipes to keep filenames bounded? Or
   rely on the manifest JSON to map?
3. **Cross-renderer divergence detection report (no explicit
   gate).** Even though it's not a PASS/FAIL signal, surfacing
   "Metal and GL disagree on this XBE's pixels" is useful triage.
   Where does it live in the orchestrator output?
4. **128 MB RAM upgrade decision.** If Phase 3 reveals OOM on
   PGR2/heavy titles, the user has to physically solder. Workflow
   needs a "hardware pause" gate that pauses orchestrator runs
   pending user action.
5. **`autoinput` on OG Xbox.** If unverified at Phase 3 turns into
   "missing on OG Xbox," we add ~1-2 weeks for nxdk_dyndxt
   implementation. Is that acceptable to the user, or do they
   want to ship without controller-injection automation and just
   manually drive games to canonical states?

## 9. Codex-validation cadence

Per project rule #15 + catalog §8.1 + this plan §2.8:

- **This v2 plan** goes through `/codex-validate plan
  docs/apple-silicon/diagnostic-xbe-plan.md` BEFORE any new nxdk
  source is written. (Pending; v1 Codex-validation returned
  BLOCKING; v2 should resolve all 12 findings; re-validation
  gates the next step.)
- **`xbe-tests/lib/` API** goes through `/codex-validate changes`
  after Phase 0 build.
- **Each XBE batch** (typically 3-4) goes through
  `/codex-validate changes` before commit.
- **Catalog updates** that motivate XBE additions go through
  `/codex-validate plan` per catalog §8.1.

## 10. Summary of what changed from v1 (Codex BLOCKING resolution)

| Codex finding | v1 issue | v2 resolution |
|---|---|---|
| #1 Critical: Tier 1 CPU-side VRAM readback fails on Metal | Self-validation contract assumed CPU-side readback worked | §2.1 inverted tier order; host-side capture is now Tier 1 |
| #2 Critical: pb_agp_access not coherent with Metal cache | Same root cause as #1 | Tier 2 (escape hatch) declares its surface-download prerequisite in manifest |
| #3 High: Mirror coords mixed host/guest scaling | XBE rendered at host-scaled coords | §2.6 forces `XEMU_DISPLAY_SCALE=1` for all XBEs; §4.1 spec rewritten in guest space |
| #4 High: CRTC-publish XBE category error | Guest can't observe host-side fallback | §4.4 redesigned for host-side capture with per-flag-recipe expected references |
| #5 Medium: xbed_decode_pixel underspecified | One signature for two namespaces | §3.1 split into `xbed_rt_format_decoders[10]` + `xbed_tex_format_decoders[42]` dispatch tables |
| #6 Medium: Mirror VS path conflated FFP/programmable | Header math wrong for the actual code path | §4.1 explicitly programmable VS path; math derived from `glsl/vsh-prog.c:753` |
| #7 Medium: Stencil readback via Tier 2 not feasible on Metal | Metal download skips depth/stencil | §4.10 redesigned for color-via-stencil-compare probe (Tier 1) |
| #8 Medium: DMA A/B used identical bases | Only tested selector decode | §4.16 uses different bases per channel; tests actual translation |
| #9 Medium: Logic ops Metal also lacks impl | Manifest only marked GL expected_fail | §4.14 manifest marks both GL and Metal expected_fail |
| #10 Medium: Phase 0 churned flat-tri-depth | Re-validation forbidden by project rule #11 | §3.1 + §7 build new `lib-smoke` XBE; flat-tri-depth untouched |
| #11 Low: byte-identical vs non_deterministic_regions contradicted | Schema didn't reconcile | §2.4 defines reproducibility as "byte-identical-after-mask" |
| #12 Low: run-benchmark.sh hardcodes title aliases | Generic XBE path mode missing | §3.2 `xbe_discover.py` + harness manages discovery; run-benchmark.sh integration is downstream |

Plus one architectural addition not on Codex's list:

| Architectural | What changed |
|---|---|
| **Real-Xbox oracle path** | New §2.2 reference oracle hierarchy puts real-Xbox-captured frames as canonical when available; math-derived as audit material; cross-renderer divergence as triage signal. Architecture details in `real-xbox-oracle-feasibility.md`. |

## Appendix A — Worked example: `mirror` XBE (revised)

```c
/*
 * mirror — pixel-position oracle.
 *
 * NV2A feature exercised: §A.6 Z perspective, §C.8 window clip,
 *                         §J.1 viewport, §J.2 scissor.
 * NV097 methods:          SET_TRANSFORM_PROGRAM (0x0B00),
 *                         SET_TRANSFORM_PROGRAM_LOAD (0x1E9C),
 *                         SET_TRANSFORM_PROGRAM_START (0x1EA0),
 *                         SET_BEGIN_END (0x17FC),
 *                         DRAW_ARRAYS (0x1810).
 * Self-validation tier:   1 (host-side capture).
 * Oracle priority:        real-xbox > math-derived.
 *
 * Math derivation:
 *   Back-buffer 640x480 A8R8G8B8 (XEMU_DISPLAY_SCALE=1 forced
 *   per diagnostic-mode prereq §2.6).
 *   Clear color: opaque black 0xFF000000.
 *
 *   Programmable VS path: VS computes screen-space position
 *   directly via surfaceSize uniform (per glsl/vsh-prog.c:753).
 *   For target guest pixel (320, 50):
 *     screen_x = 320, screen_y = 50.
 *     o[POSITION] = vec4(screen_x, screen_y, 0, 1).
 *   The pixel shader emits opaque white 0xFFFFFFFF.
 *
 *   Triangle: 1-pixel triangle vertices arranged so coverage
 *   collapses to exactly host pixel (320, 50).
 *
 *   Expected back-buffer:
 *     pixel(320, 50)  == 0xFFFFFFFF
 *     all other pixels == 0xFF000000
 *
 *   Mirror-bug detection: a renderer that flips top-half to
 *   bottom-half produces additional white at (320, H-1-50) =
 *   (320, 429). The XBE's harness reports any non-(320,50)
 *   white pixel by location.
 *
 * Reproducibility: deterministic. No banner / counter on the
 *   captured frame. (Banner renders only on later visual-debug
 *   frames after the capture trigger fires.)
 */
```

The `expected.py` for this XBE encodes the same math:

```python
def gl_no_msaa(width, height, **flags):
    fb = numpy.full((height, width), 0xFF000000, dtype=numpy.uint32)
    fb[50, 320] = 0xFFFFFFFF
    return fb

def metal_no_msaa_no_fallback(width, height, **flags):
    return gl_no_msaa(width, height, **flags)

def metal_no_msaa_fallback(width, height, **flags):
    return gl_no_msaa(width, height, **flags)
```

Reviewer audit: read the XBE header, read expected.py, confirm
they match. If they don't, the XBE source is wrong (or the math
derivation is wrong). Either way, the disagreement is actionable
before a single byte of nxdk source is written.
