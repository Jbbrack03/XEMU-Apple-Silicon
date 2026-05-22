# texture-pitch-alignment

Tier-1 diagnostic XBE covering §E.13 (linear-texture row pitch +
IMAGE_RECT alignment) in
`docs/apple-silicon/nv2a-feature-surface-research.md`. Sister to
`texture-format-sweep` (which sweeps the COLOR format enum at
baseline pitch); this XBE pins the format to `LU_IMAGE_A8R8G8B8`
and sweeps the pitch / image-rect register pair instead.

## What it tests

8 cells in a 4x2 grid (160x240 each, 640x480 frame). Every cell
binds a uniform-target-color `LU_IMAGE_A8R8G8B8` texture under a
different `(IMAGE_RECT.width, IMAGE_RECT.height,
TEXCTL1.IMAGE_PITCH)` combination. Per-cell VRAM allocation is
`pitch * (height + EXTRA_PAD_ROWS)` bytes (8 sentinel rows beneath
the active rectangle so a wrong-height sampler reads sentinel,
not uninitialised memory). The entire allocation is sentinel-filled
with gray `0xFF808080` BEFORE the active rectangle's leading
`width * bpp` bytes are overwritten with the cell's target color.
NEAREST filter + CLAMP_TO_EDGE wrap.

| Cell | Color   | w × h  | pitch | variation                                  |
|------|---------|--------|-------|--------------------------------------------|
| 0    | RED     | 4 × 4  | 16    | only true baseline: pitch == w * bpp       |
| 1    | GREEN   | 4 × 4  | 64    | pitch 4x oversized                         |
| 2    | BLUE    | 8 × 4  | 64    | wider active, same pitch                   |
| 3    | WHITE   | 5 × 3  | 32    | non-power-of-two dims (small)              |
| 4    | YELLOW  | 4 × 4  | 20    | smallest non-baseline pitch (w * bpp + 4)  |
| 5    | CYAN    | 4 × 4  | 128   | pitch 8x oversized                         |
| 6    | MAGENTA | 7 × 5  | 64    | odd width + non-aligned pitch              |
| 7    | RED     | 3 × 3  | 32    | tiny active + padded pitch                 |

## Expected output

Math-derived (cube-corner-only target colors survive display gamma
byte-exact):

```
Row 0 (y 0..239):    RED      GREEN    BLUE     WHITE
Row 1 (y 240..479):  YELLOW   CYAN     MAGENTA  RED
```

Identical to `texture-format-sweep` -- intentionally, because the
math-derived oracle here is what the FRAMEBUFFER should look like,
and the mechanism under test (per-cell pitch + image-rect) does not
change the visible target color.

## What it catches

- Row-stride bug: any cell whose `pitch > width * bpp` picks up
  sentinel-gray in rows 1+ if the renderer assumes `width * bpp`
  row stride.
- IMAGE_RECT.width ignored: cells with non-power-of-two width
  (3 / 5 / 7) read past the active span on the u-axis and pick up
  sentinel.
- IMAGE_RECT.height ignored (e.g. silently rounded to next_pow2):
  sampling at v in [0,1] with a wrong larger height steps into the
  EXTRA_PAD_ROWS sentinel rows beneath the active rectangle.
- Complete linear-texture binding failure: all 8 cells render as
  black or sentinel.

## What it does NOT catch

- Swizzled (SZ_) format pitch (no row pitch register; uses Morton
  curve) — covered by `swizzle-mipmap`.
- Channel-decode bugs across the LU_IMAGE_ family — covered by
  `texture-format-sweep`.
- DMA-channel-selector bugs (A vs B) — covered by `texture-dma-ab`.
- Border-source / wrap mode bugs — covered by
  `texture-filter-wrap`.

## Build + run

```sh
# build (Mac-side):
cd scripts/apple-silicon/xbe-tests/texture-pitch-alignment
make

# run on xemu-Metal via the production harness:
scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
    --xbe texture-pitch-alignment --renderer metal
```

## Status

v0.2 -- Tier-1 math-derived oracle. Shipped 2026-05-22 as the first
slice of the second-wave M15 default-on coverage list. v0.2 adds
`EXTRA_PAD_ROWS = 8` sentinel rows beneath each cell's active
rectangle so `IMAGE_RECT.height` has a grounded oracle (v0.1
allocated exactly `pitch * height` bytes so a wrong-height read
would have hit uninitialised memory rather than a deliberate
sentinel; v0.2 fixes that per Codex 2026-05-22 finding). Real-Xbox reference promotion deferred to the same hardware
bring-up window as `texture-format-sweep` -- both share the same
build / capture mechanism (lib.mk + xbed_render_loop_then_capture
to a per-XBE `D:\\<xbe-id>-*.bin` artifact) and will be captured
together.
