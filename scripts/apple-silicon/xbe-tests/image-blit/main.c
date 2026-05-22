/*
 * image-blit — NV_IMAGE_BLIT 2D blit guest-VRAM oracle (Tier-2 NV2A
 * diag XBE; covers §H.6 in
 * `docs/apple-silicon/nv2a-feature-surface-research.md`).
 *
 * NV2A feature exercised: §H.6 — `NV_IMAGE_BLIT` (class 0x9F) +
 * `NV_CONTEXT_SURFACES_2D` (class 0x62), `OPERATION = SRCCOPY`,
 * `COLOR_FORMAT = LE_A8R8G8B8`. Eight independent blits sweep the
 * (rect dimension, source rect, destination rect) space across a
 * single source buffer into 8 independent destination buffers.
 *
 * Per-cell GPU pipeline (one cell = one IMAGE_BLIT call):
 *
 *     NV062 SET_OBJECT                 = 17  (pbkit sGrObject17, class 0x62)
 *     NV062 SET_CONTEXT_DMA_IMAGE_SOURCE = 3  (pbkit sDmaObject3; Base=0, Limit=MAXRAM)
 *     NV062 SET_CONTEXT_DMA_IMAGE_DESTIN = 4  (pbkit sDmaObject4; Base=0, Limit=MAXRAM)
 *     NV062 SET_COLOR_FORMAT           = LE_A8R8G8B8 (0x0A)
 *     NV062 SET_PITCH                  = (src_pitch | (dst_pitch << 16))
 *     NV062 SET_OFFSET_SOURCE          = phys(source_buffer)
 *     NV062 SET_OFFSET_DESTIN          = phys(dest_buffer[cell])
 *     NV09F SET_OBJECT                 = 16  (pbkit sGrObject16, class 0x9F)
 *     NV09F SET_CONTEXT_SURFACES       = 17  (resolves to same instance as NV062 SET_OBJECT)
 *     NV09F SET_OPERATION              = SRCCOPY (3)
 *     NV09F CONTROL_POINT_IN           = (in_x | (in_y  << 16))
 *     NV09F CONTROL_POINT_OUT          = (out_x | (out_y << 16))
 *     NV09F SIZE                       = (width  | (height << 16))   <-- fires
 *
 * Every method in the `[0x000, 0x180)` and `[0x180, 0x200)` ranges
 * (SET_OBJECT + the context-DMA methods) is RAMHT-resolved by PFIFO
 * (`pfifo.c:572`). pbkit defaults `NV_PFIFO_RAMHT_SIZE = 0` (4 KiB
 * table) so the lookup hash must satisfy `hash * 8 < 4096`. Large
 * D3D-runtime handles like `0x14d00 / 0x11120` overflow that bound
 * and trip the `pfifo.c:578` assertion. We instead point at pbkit's
 * pre-registered small ChannelIDs (3/4/16/17), which hash within
 * range; channels 3/4 resolve to base=0, Limit=MAXRAM DMA contexts
 * that pbkit leaves UNTOUCHED after `pb_init`, and 16/17 to the
 * matching graphics objects (class 0x9F / class 0x62). Channels 9
 * and 11 are NOT used here: `pb_init` calls
 * `pb_target_back_buffer() → set_draw_buffer()` (pbkit.c:1611-1668),
 * which reprograms PRAMIN for channels 9 and 11 to point at the
 * back-buffer with a limit of `height*pitch-1` (= 0x0012BFFF for
 * 640x480 LE_A8R8G8B8) — see `nv2a_regs_image_blit.h` for the full
 * rationale and the assertion that empirically caught this.
 *
 * `pgraph_mtl_image_blit` asserts `context_surfaces->object_instance
 * == image_blit->context_surfaces` (`mtl/blit.c:144`). Both fields
 * receive `entry.instance` after the PFIFO RAMHT lookup; routing both
 * NV062_SET_OBJECT and NV09F_SET_CONTEXT_SURFACES through handle 17
 * makes them equal. See `nv2a_regs_image_blit.h` for full rationale.
 *
 * --- Source buffer ----------------------------------------------------
 *
 * Single 32x32 LE_A8R8G8B8 buffer in VRAM, CPU-painted with four
 * pure colors arranged in 16x16 quadrants:
 *
 *     +--------+--------+
 *     |  RED   | GREEN  |    x=0..15      x=16..31
 *     |        |        |    y=0..15      y=0..15
 *     +--------+--------+
 *     |  BLUE  | WHITE  |    x=0..15      x=16..31
 *     |        |        |    y=16..31     y=16..31
 *     +--------+--------+
 *
 * Source pitch is 32 * 4 = 128 bytes/row. Quadrant boundaries fall
 * on pixel coords (16, 16), which is well inside the largest blit
 * rect (32x32, cell 3).
 *
 * --- Destination buffers ----------------------------------------------
 *
 * Eight independent destination buffers, one per cell. Each is
 * 64x64 (covers max blit size + max out-offset + comfortable safety
 * margin) with pitch=256 bytes/row (= 64 * 4). The full
 * `pitch * 64` allocation is pre-filled with sentinel gray
 * `0xFF808080` BEFORE the blit fires so that any blit that touches
 * pixels outside the declared dst rect leaves a visibly-degraded
 * cell.
 *
 * --- Cell layout (per `scripts/apple-silicon/xbe-tests/image-blit/expected.py`) ---
 *
 *   Cell  In(x,y)  Out(x,y)  WxH    Source quadrant region
 *   ----  -------  --------  -----  ------------------------
 *   0     0,0      0,0       8x8    RED only
 *   1     0,0      0,0       16x16  RED only (full quadrant)
 *   2     0,0      0,0       32x32  All 4 quadrants
 *   3     8,8      0,0       8x8    RED only (offset src)
 *   4     0,0      4,4       8x8    RED only (offset dst)
 *   5     4,4      8,8       8x8    RED only (both offsets)
 *   6     0,0      0,0       1x16   Single-pixel-wide column (RED)
 *   7     0,0      0,0       16x1   Single-pixel-high row (RED)
 *
 * --- Oracle ----------------------------------------------------------
 *
 * Tier-2 guest-VRAM oracle per `diagnostic-xbe-plan.md` §2.1: the
 * XBE waits for GPU idle (`pb_wait_until_gr_not_busy()`), reads the
 * destination VRAM via `pb_agp_access()` (cache-coherent linear
 * read), and CPU-verifies each destination pixel against the
 * math-derived expected pattern. The per-cell PASS/FAIL verdict is
 * encoded as a solid-color rectangle in the 4x2 dashboard:
 *
 *   - PASS: solid green   (0xFF00FF00)
 *   - FAIL: solid red     (0xFFFF0000)
 *
 * The host-side capture picks up the dashboard via PCRTC_START. The
 * math-derived oracle in `expected.py` is therefore 8 solid green
 * cells on a 4x2 grid (one green frame, byte-exact). A single failed
 * blit will turn one cell red and trip the harness's strict
 * byte-exact compare.
 *
 * The CPU-side oracle is the load-bearing one (per Tier-2 contract):
 * the green/red dashboard is just the visible projection of the
 * CPU-side compare result so the host can capture and compare.
 *
 * --- Reproducibility -------------------------------------------------
 *
 * Pure deterministic pattern: no banner, no counter, no per-frame
 * variation. Byte-identical across two cold runs after the masked
 * dashboard region is applied (there is no mask; the whole frame
 * is deterministic).
 *
 * Pixel oracle per cell verifies:
 *   - In(x,y) honored: src-offset reads correct quadrant.
 *   - Out(x,y) honored: dst-offset writes correct pixel positions.
 *   - Width/Height honored: declared rect pixels copied,
 *     surrounding sentinel remains.
 *   - SRCCOPY semantics: every dst pixel inside the rect == the
 *     corresponding src pixel (no blend, no key, no patch).
 *   - LE_A8R8G8B8 byte layout: pre-existing alpha bits in the
 *     source survive the copy.
 *
 * --- Does NOT catch (deferred / separate XBEs) -----------------------
 *
 *   - BLEND_AND / non-SRCCOPY operations (Beta channel).
 *   - LE_R5G6B5 / LE_Y8 / LE_X8R8G8B8_Z8R8G8B8 color formats.
 *   - Source/destination overlap, intra-buffer blits.
 *   - GPU-to-GPU MTLTexture propagation correctness (this XBE
 *     intentionally uses never-rendered VRAM buffers so the
 *     CPU-memcpy fast path in `pgraph_mtl_image_blit` is the path
 *     under test; a separate slice would cover the surface-cache
 *     propagation path).
 */
#include "xbed_capture.h"
#include "xbed_runtime.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <pbkit/pbkit.h>
#include <pbkit/pbkit_dma.h>
#include <pbkit/pbkit_pushbuffer.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "nv2a_regs_image_blit.h"

/* Cycle 20 Path A (2026-05-22): always-on, FTP-collectable progress
 * markers. Cycle 21 Path A.2 (2026-05-22): markers re-routed from
 * `D:\image-blit-marker-NN-STAGE.txt` (proven blocked on real Xbox
 * across three cycle-19+cycle-20 runs) to
 * `E:\Apps\image-blit\image-blit-marker-NN-STAGE.txt` — the harness's
 * existing FTP-collect target. Every staged checkpoint writes a tiny
 * text file there; the harness pulls everything under `/E/Apps/<id>`
 * after the chainloaded XBE reboots, so the highest-numbered marker
 * file present after a real-Xbox run is the furthest stage the XBE
 * actually reached.
 *
 * Why D:\ was the cycle-20 witness path: cycle 19 attempted a real-Xbox
 * parity check by chainloading the existing v0.4 image-blit XBE via the
 * oracle agent's `runxbe` RPC. Two independent attempts produced clean
 * reboots back to FTP but NO `D:\image-blit-capture.bin` and NO
 * `D:\image-blit-done.txt`. The single binary witness "either
 * everything ran or nothing did" was the entire problem — we could not
 * tell whether the XBE crashed at `xbed_init`, mid-blit, during oracle
 * compare, in shader load, or whether it ran to completion and the
 * final `fopen("D:\\…","wb")` for the capture/done files failed
 * silently. Cycle 20 added 13 staged D:\ marker writes; three
 * independent real-Xbox runs returned ZERO marker files, while xemu
 * local proved every `fopen("D:\\…","wb")` returns NULL on the ISO
 * mount path (CD-ROM, read-only). That elevated cycle-19 hypothesis
 * #1 (D:\ remap mismatch under `runxbe` SITE-EXEC chainload) to the
 * leading hypothesis.
 *
 * Why E:\Apps\image-blit\ for cycle 21: the xbe-harness
 * (`scripts/apple-silicon/xbe-harness/xbe_renderers.py::run_real_xbox`)
 * uploads the diag XBE to `E:\\Apps\\<id>\\default.xbe` BEFORE
 * chainload AND runs `--ftp-collect /E/Apps/<id>` AFTER chainload.
 * That directory therefore (a) provably exists at run time (the
 * upload echo is what every cycle-20 real-Xbox run retrieved), (b)
 * lives on the persistent FATX utility partition
 * `\Device\Harddisk0\Partition1` (E:), (c) needs only an idempotent
 * `nxIsDriveMounted('E')` + `nxMountDrive('E', …)` shim because
 * nxdk's automount-d mounts D:\ for the launched XBE but NOT E:\.
 * The same shim is shipped in `oracle-agent/controller.c`,
 * `oracle-agent/tier2.c`, `lib/xbed_input_synth.c`, and
 * `controller-readback/main.c`. T:\ was rejected: the harness
 * FTP-collects from `/E/Apps/<id>` only, so a T:\ write would not be
 * retrievable without expanding harness scope.
 *
 * The cycle-21 question is binary and conservative: if E:\ markers
 * land, cycle-19 hypothesis #1 (witness-path partition mismatch under
 * `runxbe`) is corroborated AND the highest-numbered file tells us
 * the actual real-Xbox execution stage. If E:\ markers DO NOT land,
 * the failure mode is bigger than a partition mismatch (XBE crashes
 * very early, or the chainload itself has a different runtime
 * environment than the upload context) and a different next slice is
 * required. The bounded scope does NOT relocate the
 * `xbed_render_loop_then_capture` capture/done writes (still under
 * `D:\`); that's a separate slice if cycle-21 evidence motivates it.
 *
 * Each marker is independently small (a few hundred bytes max), is
 * written via `fopen / fprintf / fclose`, and is mirrored through the
 * existing `xbed_host_log_writef` channel so xemu logs see the
 * progression even when the FS write fails. A failed marker write is
 * silently ignored — we want maximum forward progress, not extra
 * exit paths.
 *
 * Markers MUST be numerically ordered. Adding a new one between two
 * existing markers requires renumbering everything after it. The
 * mapping (number -> stage) is the authoritative documentation; the
 * stage label string in the filename is for human readability. */

/* Cycle 21 Path A.2 (2026-05-22): idempotent E:\ mount used by the
 * marker helper. Same pattern as `oracle-agent/controller.c::
 * s_ensure_e_drive_mounted` and `controller-readback/main.c::
 * ensure_e_drive_mounted`. Cached after first success so we avoid
 * re-mounting on every marker. Returns 1 iff E: is usable after the
 * call; 0 on hard failure. Safe to call before any other init —
 * `nxIsDriveMounted` / `nxMountDrive` only touch the NT symbolic-link
 * table, no PFIFO / NV2A / pbkit dependency. */
static int s_image_blit_e_mount_cached = 0;

static int image_blit_ensure_e_mount(void)
{
    if (s_image_blit_e_mount_cached) return 1;
    if (!nxIsDriveMounted('E')) {
        /* OG Xbox standard FATX layout: Partition1 = E: (utility,
         * ~5 GiB, persistent). See `oracle-agent/controller.c` for
         * the full partition map. */
        if (!nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")) {
            return 0;
        }
    }
    /* On real Xbox the harness FTP-uploads `default.xbe` to
     * `E:\Apps\image-blit\` BEFORE chainload, so the target dir
     * already exists; CreateDirectoryA is a no-op (returns FALSE on
     * already-exists). On xemu local — where there is no harness
     * upload step — the dir does not exist and CreateDirectoryA
     * creates it so marker fopens land on the scratch HDD image
     * instead of silently fopen-failing. Idempotent. Same pattern
     * as `oracle-agent/controller.c::s_write_anchor_file`. */
    CreateDirectoryA("E:\\Apps", NULL);
    CreateDirectoryA("E:\\Apps\\image-blit", NULL);
    s_image_blit_e_mount_cached = 1;
    return 1;
}

static void image_blit_marker(unsigned idx, const char *stage)
{
    /* Filename budget: FATX caps basenames at 42 characters. The
     * fixed prefix `image-blit-marker-` (18) + `NN-` (3) + `.txt` (4)
     * costs 25 chars, leaving 17 for the stage label. ALL labels
     * below are kept ≤ 14 chars so every marker basename stays at
     * 39 chars or under, well clear of the limit. (Codex 2026-05-22
     * cycle 20 review flagged the original `default_state_set` /
     * `before_capture_loop` labels for being at-limit / over-limit;
     * they were shortened to `state_set` / `pre_capture`.) The
     * `%02u` keeps lexical and numeric ordering identical so a
     * sorted FTP listing reflects the stage-execution order.
     *
     * Cycle 21 path-length math: directory prefix
     * `E:\Apps\image-blit\` is 19 chars; basename ≤ 39 chars; null
     * terminator 1 char ⇒ worst case 59 chars. Buffer is 96 for
     * comfortable safety margin. The basename invariant the FATX
     * 42-char limit cares about is unchanged from cycle 20 — only
     * the directory prefix grew. */
    char path[96];
    int n = snprintf(path, sizeof(path),
                     "E:\\Apps\\image-blit\\image-blit-marker-%02u-%s.txt",
                     idx, stage);
    if (n <= 0 || n >= (int)sizeof(path)) {
        /* Bad format / overflow: skip the FS write but still surface
         * through the host-log channel so xemu sees the stage tag. */
        xbed_host_log_writef("image-blit: marker %02u %s (path-overflow)",
                             idx, stage);
        return;
    }
    /* Cycle 21: ensure E: is mounted before each fopen. Idempotent
     * after first success (cached). Failure to mount is surfaced
     * through the host-log channel and the FS write below is
     * attempted anyway — `fopen` on an unmounted drive will simply
     * return NULL and take the existing fopen-failed branch. */
    int e_mounted = image_blit_ensure_e_mount();
    /* Mirror through the host-log channel FIRST so the marker shows up
     * in xemu logs even if the file open fails. The host-log channel
     * is inert on real Xbox / stock xemu without XEMU_GUEST_LOG; on
     * xemu with XEMU_GUEST_LOG=1 it surfaces every staged marker
     * regardless of fopen outcome. */
    xbed_host_log_writef("image-blit: marker %02u %s e_mount=%d",
                         idx, stage, e_mounted);
    /* Then write the FTP-collectable file. Best-effort: ignore errors
     * silently so a single failing write can't strand the test. */
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "image-blit marker %02u stage=%s\n", idx, stage);
        fclose(f);
    } else {
        /* fopen failed: also surface this through the host-log channel
         * so the failure mode is at least observable under xemu. */
        xbed_host_log_writef("image-blit: marker %02u %s fopen-failed",
                             idx, stage);
    }
}

#define WIN_W 640
#define WIN_H 480

#define GRID_COLS 4
#define GRID_ROWS 2
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define CELL_W (WIN_W / GRID_COLS)   /* 160 */
#define CELL_H (WIN_H / GRID_ROWS)   /* 240 */

#define BPP 4 /* LE_A8R8G8B8 */

/* Source surface dimensions. 32x32 covers the largest blit (cell 2)
 * and the offset-source case (cell 3 in=(8,8)+8x8 → reads through
 * (15,15) inside the BLUE quadrant -- still valid for testing
 * because the quadrant midpoint matches (15,15)? No: we want
 * cell 3 to read RED only, so the in_x/in_y must put the rect
 * entirely inside the RED quadrant. Cell 3 in=(8,8) size=8x8 reads
 * x=8..15, y=8..15 -- still RED. cell 5 in=(4,4) size=8x8 reads
 * x=4..11, y=4..11 -- still RED. Good.) */
#define SRC_W      32
#define SRC_H      32
#define SRC_PITCH  (SRC_W * BPP)    /* 128 bytes/row */
#define SRC_BYTES  (SRC_PITCH * SRC_H)

/* Destination surface dimensions, identical per cell. 64x64 with
 * 256-byte pitch covers max(out_x + width) = 8 + 8 = 16 and
 * max(out_y + height) = 8 + 8 = 16, with a comfortable safety
 * margin of 48 trailing rows/columns sentinel-filled. */
#define DST_W      64
#define DST_H      64
#define DST_PITCH  (DST_W * BPP)    /* 256 bytes/row */
#define DST_BYTES  (DST_PITCH * DST_H)

/* Sentinel pixel pre-painted into the entire dst allocation before
 * each blit fires. Chosen NOT to match any source-quadrant color so
 * the CPU-side oracle can tell "dst pixel was overwritten by blit"
 * from "dst pixel still has sentinel."
 *
 * Memory bytes for 0xFF808080 in A8R8G8B8 layout (low addr -> high):
 *   B=0x80, G=0x80, R=0x80, A=0xFF.
 * That's the same gray `texture-pitch-alignment` uses, for consistency. */
#define SENTINEL_PIXEL 0xFF808080u

/* Source quadrant colors as 0xAARRGGBB. */
#define SRC_RED    0xFFFF0000u
#define SRC_GREEN  0xFF00FF00u
#define SRC_BLUE   0xFF0000FFu
#define SRC_WHITE  0xFFFFFFFFu

/* PASS/FAIL cell colors. Pure 0/255 channels survive display gamma
 * byte-exact, same property `texture-pitch-alignment` relies on. */
#define DASH_PASS  0xFF00FF00u /* green */
#define DASH_FAIL  0xFFFF0000u /* red   */

typedef struct {
    uint16_t in_x;
    uint16_t in_y;
    uint16_t out_x;
    uint16_t out_y;
    uint16_t width;
    uint16_t height;
} BlitCell;

static const BlitCell k_cells[GRID_CELLS] = {
    /* Row 0 -- baseline / size sweep / src-offset sweep. */
    { 0, 0,  0, 0,   8,  8 }, /* 0  smallest contiguous rect, RED only      */
    { 0, 0,  0, 0,  16, 16 }, /* 1  full RED quadrant                       */
    { 0, 0,  0, 0,  32, 32 }, /* 2  all 4 quadrants                         */
    { 8, 8,  0, 0,   8,  8 }, /* 3  in_x/in_y honored: RED only             */
    /* Row 1 -- dst-offset + degenerate-dim sweep. */
    { 0, 0,  4, 4,   8,  8 }, /* 4  out_x/out_y honored                     */
    { 4, 4,  8, 8,   8,  8 }, /* 5  both offsets non-zero, RED only         */
    { 0, 0,  0, 0,   1, 16 }, /* 6  degenerate width (single-pixel column)  */
    { 0, 0,  0, 0,  16,  1 }, /* 7  degenerate height (single-pixel row)    */
};

/* Per-cell verdict from CPU-side oracle: 1 on PASS, 0 on FAIL. */
static int s_cell_pass[GRID_CELLS];

/* Per-cell first-mismatch diagnostic (cycle 12, v0.3): when the
 * oracle finds a mismatched pixel inside the 64x64 dst, capture
 * (mx, my) and the got / expected pixel values so the failing
 * sub-rect can encode them visually for host-screenshot decode.
 *
 * Visual encoding (FAIL cells, v0.3):
 *   - TL 80x120: solid red (FAIL banner).
 *   - TR 80x120: GOT pixel color (what the dst actually held).
 *   - BL 80x120: EXPECTED pixel color (math-derived oracle).
 *   - BR 80x120: pure-color (mx, my) encoding:
 *       R = (mx % 8) * 32  (3 LSB of mx, scaled into bucket-of-32)
 *       G = (my % 8) * 32  (3 LSB of my)
 *       B = ((mx / 8) << 4) | (my / 8)  (3 MSB each packed)
 *     R and G are bucket-of-32 values in {0, 32, 64, ..., 224},
 *     which on Apple's GL-on-Metal display path stay
 *     visually-decodable even under gamma. B packs the upper
 *     three bits of mx and my into a nibble pair (values up
 *     to 0x33 for mx, my < 32 — narrower range, but mx/my are
 *     bounded by the largest blit rect, 32x32 in this XBE).
 *     The exact (mx, my) can be read by dividing the R/G
 *     channel value by 32 and reading the B channel
 *     high-nibble / low-nibble.
 *
 * PASS cells continue to render solid green (all 4 sub-rects). */
typedef struct {
    int has_mismatch;
    uint32_t mx, my;
    uint32_t got, expected;
} CellDiag;
static CellDiag s_cell_diag[GRID_CELLS];

/* VRAM allocations: one source surface + 8 dst surfaces. */
static void *s_src_vram   = NULL;
static void *s_dst_vram[GRID_CELLS];

/* Dashboard geometry: one solid-color quad per cell (6 verts/quad,
 * 8 cells = 48 vertices). DIFFUSE is the per-cell verdict color
 * (green PASS or red FAIL); POSITION is window-space (NDC mapped
 * via the viewport matrix uploaded by `xbed_load_viewport_matrix`).
 * Matches the texture-pitch-alignment pattern: passthrough VS/PS
 * with attribute-array binding via xbed_set_attrib_pointer. */
typedef struct {
    float pos[3];
    float col[4];
} __attribute__((packed)) DashVertex;

#define VERTS_PER_QUAD 6
/* Cycle 12 (v0.3): each cell now renders 4 sub-quads (2x2 layout)
 * carrying the per-cell diagnostic info; total verts = 8 cells x
 * 4 sub-quads x 6 verts = 192. */
#define SUBQUADS_PER_CELL 4
#define DASH_VERTS_TOTAL (GRID_CELLS * SUBQUADS_PER_CELL * VERTS_PER_QUAD)

static DashVertex s_verts[DASH_VERTS_TOTAL];
static DashVertex *s_alloc_verts = NULL;

/* Compute the source pixel value at (sx, sy) per the four-quadrant
 * pattern. Used by both `fill_source_buffer()` (CPU paint) and
 * `oracle_check_cell()` (CPU verify). Pure function — no
 * side-effects. */
static uint32_t source_pixel_at(uint32_t sx, uint32_t sy)
{
    const uint32_t mid = SRC_W / 2; /* SRC_W == SRC_H */
    if (sx < mid && sy < mid)         return SRC_RED;
    else if (sx >= mid && sy < mid)   return SRC_GREEN;
    else if (sx < mid && sy >= mid)   return SRC_BLUE;
    else                              return SRC_WHITE;
}

static void fill_source_buffer(void)
{
    uint32_t *p = (uint32_t *)s_src_vram;
    for (uint32_t y = 0; y < SRC_H; y++) {
        for (uint32_t x = 0; x < SRC_W; x++) {
            p[y * SRC_W + x] = source_pixel_at(x, y);
        }
    }
}

static void fill_dest_sentinel(void *dst)
{
    uint32_t *p = (uint32_t *)dst;
    size_t count = DST_BYTES / sizeof(uint32_t);
    for (size_t i = 0; i < count; i++) {
        p[i] = SENTINEL_PIXEL;
    }
}

/* Push the per-cell IMAGE_BLIT setup + fire on the PFIFO. pbkit
 * pre-binds SUBCH_3 to ChannelID 16 (class 0x9F = NV_IMAGE_BLIT) and
 * SUBCH_4 to ChannelID 17 (class 0x62 = NV_CONTEXT_SURFACES_2D) in
 * `pb_init`, so subchannel routing is already correct. */
static void push_image_blit(const BlitCell *cell,
                            void *src_vram, void *dst_vram)
{
    const uint32_t src_phys = (uint32_t)src_vram & 0x03FFFFFFu;
    const uint32_t dst_phys = (uint32_t)dst_vram & 0x03FFFFFFu;

    uint32_t *p = pb_begin();

    /* CONTEXT_SURFACES_2D programming on SUBCH_4. */
    p = pb_push1_to(SUBCH_4, p, NV062_SET_OBJECT, IMAGE_BLIT_NV062_OBJECT_HANDLE);
    p = pb_push1_to(SUBCH_4, p, NV062_SET_CONTEXT_DMA_IMAGE_SOURCE,
                    IMAGE_BLIT_DMA_HANDLE_SRC);
    p = pb_push1_to(SUBCH_4, p, NV062_SET_CONTEXT_DMA_IMAGE_DESTIN,
                    IMAGE_BLIT_DMA_HANDLE_DST);
    p = pb_push1_to(SUBCH_4, p, NV062_SET_COLOR_FORMAT,
                    NV062_SET_COLOR_FORMAT_LE_A8R8G8B8);
    p = pb_push1_to(SUBCH_4, p, NV062_SET_PITCH,
                    (uint32_t)SRC_PITCH | ((uint32_t)DST_PITCH << 16));
    p = pb_push1_to(SUBCH_4, p, NV062_SET_OFFSET_SOURCE, src_phys);
    p = pb_push1_to(SUBCH_4, p, NV062_SET_OFFSET_DESTIN, dst_phys);

    /* IMAGE_BLIT programming on SUBCH_3. */
    p = pb_push1_to(SUBCH_3, p, NV09F_SET_OBJECT, IMAGE_BLIT_NV09F_OBJECT_HANDLE);
    p = pb_push1_to(SUBCH_3, p, NV09F_SET_CONTEXT_SURFACES,
                    IMAGE_BLIT_NV062_OBJECT_HANDLE);
    p = pb_push1_to(SUBCH_3, p, NV09F_SET_OPERATION,
                    NV09F_SET_OPERATION_SRCCOPY);
    p = pb_push1_to(SUBCH_3, p, NV09F_CONTROL_POINT_IN,
                    (uint32_t)cell->in_x | ((uint32_t)cell->in_y << 16));
    p = pb_push1_to(SUBCH_3, p, NV09F_CONTROL_POINT_OUT,
                    (uint32_t)cell->out_x | ((uint32_t)cell->out_y << 16));
    p = pb_push1_to(SUBCH_3, p, NV09F_SIZE,
                    (uint32_t)cell->width | ((uint32_t)cell->height << 16));

    pb_end(p);
}

/* CPU-side oracle: every pixel inside the declared dst rect must
 * equal the corresponding source pixel; every pixel outside the
 * dst rect (but still inside the 64x64 allocation) must remain
 * sentinel. Returns 1 on PASS, 0 on FAIL.
 *
 * Cycle 12 (v0.3): on the FIRST mismatch, populate s_cell_diag[idx]
 * with (mx, my, got, expected) so the dashboard can encode this
 * info as a 2x2 sub-rect layout (see CellDiag comment above). */
static int oracle_check_cell(int idx,
                             const BlitCell *cell, const void *dst_vram)
{
    /* Cycle 12 finding (decision-log 2026-05-22): use `pb_agp_access`
     * to read dst via the AGP-aliased UNCACHED view. The control
     * experiment (a cycle-12 v0.3 variant that read through the
     * cached pointer) made the residual WORSE: only cell 4 PASSed
     * instead of 0/4/5. Combined with the host-side fprintf evidence
     * that the renderer memcpy writes correct RED bytes for all 8
     * cells, this proves the residual 5-cell mismatch is a guest
     * CPU cache-coherency issue on the read-back path, not a
     * renderer bug. AGP-aliased read is strictly less stale than
     * cached read and is the right view for this test. */
    const uint32_t *dst = (const uint32_t *)pb_agp_access((void *)dst_vram);
    for (uint32_t y = 0; y < DST_H; y++) {
        for (uint32_t x = 0; x < DST_W; x++) {
            const uint32_t got = dst[y * DST_W + x];
            uint32_t expected;
            const int inside_x =
                (x >= cell->out_x) &&
                (x <  (uint32_t)cell->out_x + (uint32_t)cell->width);
            const int inside_y =
                (y >= cell->out_y) &&
                (y <  (uint32_t)cell->out_y + (uint32_t)cell->height);
            if (inside_x && inside_y) {
                const uint32_t sx = (uint32_t)cell->in_x +
                                    (x - (uint32_t)cell->out_x);
                const uint32_t sy = (uint32_t)cell->in_y +
                                    (y - (uint32_t)cell->out_y);
                expected = source_pixel_at(sx, sy);
            } else {
                expected = SENTINEL_PIXEL;
            }
            if (got != expected) {
                s_cell_diag[idx].has_mismatch = 1;
                s_cell_diag[idx].mx = x;
                s_cell_diag[idx].my = y;
                s_cell_diag[idx].got = got;
                s_cell_diag[idx].expected = expected;
                debugPrint(
                    "image-blit: cell %d mismatch dst@(%u,%u) got=0x%08lx "
                    "expected=0x%08lx in=(%u,%u) out=(%u,%u) wxh=%ux%u\n",
                    idx, (unsigned)x, (unsigned)y,
                    (unsigned long)got, (unsigned long)expected,
                    (unsigned)cell->in_x, (unsigned)cell->in_y,
                    (unsigned)cell->out_x, (unsigned)cell->out_y,
                    (unsigned)cell->width, (unsigned)cell->height);
                /* Cycle 15: mirror the per-cell first-mismatch line through
                 * the host-visible log channel so it survives a broken
                 * screenshot path. Inert if XEMU_GUEST_LOG is unset. */
                xbed_host_log_writef(
                    "image-blit: cell %d mismatch dst@(%u,%u) got=0x%08lx "
                    "expected=0x%08lx in=(%u,%u) out=(%u,%u) wxh=%ux%u",
                    idx, (unsigned)x, (unsigned)y,
                    (unsigned long)got, (unsigned long)expected,
                    (unsigned)cell->in_x, (unsigned)cell->in_y,
                    (unsigned)cell->out_x, (unsigned)cell->out_y,
                    (unsigned)cell->width, (unsigned)cell->height);
                return 0;
            }
        }
    }
    return 1;
}

static void run_one_blit_cell(int idx)
{
    fill_dest_sentinel(s_dst_vram[idx]);
    push_image_blit(&k_cells[idx], s_src_vram, s_dst_vram[idx]);
    pb_wait_until_gr_not_busy();
    s_cell_pass[idx] = oracle_check_cell(idx, &k_cells[idx], s_dst_vram[idx]);
    debugPrint("image-blit: cell %d %s\n", idx,
               s_cell_pass[idx] ? "PASS" : "FAIL");
    /* Cycle 15: mirror the per-cell verdict through the host-visible log
     * channel. Renderer-agnostic: this is the path that lets the §H.6
     * race hypothesis be evaluated under both GL and Metal without
     * depending on screenshot capture. Inert if XEMU_GUEST_LOG is
     * unset (the OUT instruction is then a silent no-op). */
    xbed_host_log_writef("image-blit: cell %d %s", idx,
                         s_cell_pass[idx] ? "PASS" : "FAIL");
}

static inline void dash_vert(DashVertex *v, int x_w, int y_w,
                             const float rgba[4])
{
    /* Window-space POSITION; viewport-matrix uniform maps clip-space
     * [-1,1] back to window coords, so emit NDC. Same math as the
     * texture-pitch-alignment XBE's mk_vert. */
    v->pos[0] = (float)x_w / (float)(WIN_W / 2) - 1.0f;
    v->pos[1] = 1.0f - (float)y_w / (float)(WIN_H / 2);
    v->pos[2] = 0.5f;
    v->col[0] = rgba[0];
    v->col[1] = rgba[1];
    v->col[2] = rgba[2];
    v->col[3] = rgba[3];
}

static void emit_cell_quad(DashVertex *out, int x0, int y0, int x1, int y1,
                           const float rgba[4])
{
    dash_vert(&out[0], x0, y0, rgba);
    dash_vert(&out[1], x1, y0, rgba);
    dash_vert(&out[2], x1, y1, rgba);
    dash_vert(&out[3], x0, y0, rgba);
    dash_vert(&out[4], x1, y1, rgba);
    dash_vert(&out[5], x0, y1, rgba);
}

/* Cycle 12 (v0.3): unpack an ARGB pixel into a normalized float4
 * suitable for emitting as a sub-rect DIFFUSE. The XBE source
 * paint and oracle use 0xAARRGGBB layout. */
static void argb_to_float4(uint32_t argb, float out[4])
{
    out[0] = (float)((argb >> 16) & 0xFF) / 255.0f; /* R */
    out[1] = (float)((argb >>  8) & 0xFF) / 255.0f; /* G */
    out[2] = (float)((argb >>  0) & 0xFF) / 255.0f; /* B */
    out[3] = (float)((argb >> 24) & 0xFF) / 255.0f; /* A */
}

/* Cycle 12 (v0.3): pack (mx, my) into a pure ARGB color via the
 * bucket-of-32 scheme documented at the CellDiag declaration so
 * the host screenshot can decode the exact first-mismatch
 * coordinates without relying on continuous gradients. */
static uint32_t pos_color_argb(uint32_t mx, uint32_t my)
{
    uint8_t r = (uint8_t)((mx & 0x07u) * 32u);
    uint8_t g = (uint8_t)((my & 0x07u) * 32u);
    uint8_t b = (uint8_t)((((mx >> 3) & 0x07u) << 4) |
                          ((my >> 3) & 0x07u));
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void build_dashboard_geometry(void)
{
    /* Per-cell DIFFUSE: PASS (green) or FAIL with diagnostic 2x2
     * sub-rect encoding. Stored as float4 in [0,1] for the
     * passthrough PS. */
    const float pass_rgba[4] = { 0.0f, 1.0f, 0.0f, 1.0f }; /* green */
    const float fail_rgba[4] = { 1.0f, 0.0f, 0.0f, 1.0f }; /* red   */

    const int SUBCELL_W = CELL_W / 2; /* 80 */
    const int SUBCELL_H = CELL_H / 2; /* 120 */

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            const int cx0 = col * CELL_W;
            const int cy0 = row * CELL_H;

            float tl_rgba[4], tr_rgba[4], bl_rgba[4], br_rgba[4];
            if (s_cell_pass[idx] || !s_cell_diag[idx].has_mismatch) {
                /* PASS or unexpected no-mismatch FAIL: all green. */
                memcpy(tl_rgba, pass_rgba, sizeof(pass_rgba));
                memcpy(tr_rgba, pass_rgba, sizeof(pass_rgba));
                memcpy(bl_rgba, pass_rgba, sizeof(pass_rgba));
                memcpy(br_rgba, pass_rgba, sizeof(pass_rgba));
            } else {
                /* FAIL with diag info: encode 2x2 layout per docs. */
                memcpy(tl_rgba, fail_rgba, sizeof(fail_rgba));
                argb_to_float4(s_cell_diag[idx].got, tr_rgba);
                argb_to_float4(s_cell_diag[idx].expected, bl_rgba);
                argb_to_float4(pos_color_argb(s_cell_diag[idx].mx,
                                              s_cell_diag[idx].my),
                               br_rgba);
            }

            const int base = idx * SUBQUADS_PER_CELL * VERTS_PER_QUAD;
            /* TL */
            emit_cell_quad(&s_verts[base + 0 * VERTS_PER_QUAD],
                           cx0, cy0,
                           cx0 + SUBCELL_W, cy0 + SUBCELL_H,
                           tl_rgba);
            /* TR */
            emit_cell_quad(&s_verts[base + 1 * VERTS_PER_QUAD],
                           cx0 + SUBCELL_W, cy0,
                           cx0 + CELL_W, cy0 + SUBCELL_H,
                           tr_rgba);
            /* BL */
            emit_cell_quad(&s_verts[base + 2 * VERTS_PER_QUAD],
                           cx0, cy0 + SUBCELL_H,
                           cx0 + SUBCELL_W, cy0 + CELL_H,
                           bl_rgba);
            /* BR */
            emit_cell_quad(&s_verts[base + 3 * VERTS_PER_QUAD],
                           cx0 + SUBCELL_W, cy0 + SUBCELL_H,
                           cx0 + CELL_W, cy0 + CELL_H,
                           br_rgba);
        }
    }
    memcpy(s_alloc_verts, s_verts, sizeof(s_verts));
}

static void enforce_dashboard_state(void)
{
    /* Match texture-pitch-alignment's enforce_common_state: no blend,
     * no depth, no cull, fill mode, smooth shading -- a solid quad
     * goes from the VS through the passthrough PS unmodified. */
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    p = pb_push1(p, NV097_SET_FRONT_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_BACK_POLYGON_MODE,
                 NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    pb_end(p);
}

static void bind_dashboard_attribs(void)
{
    xbed_clear_all_attribs_to_float();
    /* POSITION (slot 0): Float3. */
    xbed_set_attrib_pointer(
        0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3,
        sizeof(DashVertex), &s_alloc_verts[0].pos[0]);
    /* DIFFUSE (slot 3): Float4. */
    xbed_set_attrib_pointer(
        3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
        sizeof(DashVertex), &s_alloc_verts[0].col[0]);
}

static void render_dashboard_frame(uint32_t frame_idx, void *ctx)
{
    (void)frame_idx;
    (void)ctx;
    /* Clear back buffer to black so anywhere a cell quad fails to
     * cover stays black (visibly distinct from PASS green and FAIL
     * red). */
    xbed_clear_color_argb(0xFF000000);
    xbed_load_viewport_matrix();
    enforce_dashboard_state();
    bind_dashboard_attribs();
    xbed_draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, DASH_VERTS_TOTAL);
}

int main(void)
{
    /* Cycle 20 Path A: marker 00 fires BEFORE anything else — no
     * pbkit, no XVideoSetMode, no VRAM alloc. If real Xbox produces
     * this file but no later markers, the failure is in xbed_init or
     * pbkit init. If real Xbox produces NO markers at all, either
     * D:\ doesn't fopen on this chainload path or the XBE crashes
     * before main() body runs. */
    image_blit_marker(0, "program_entered");

    if (xbed_init(WIN_W, WIN_H) != XBED_OK) return 1;
    image_blit_marker(1, "xbed_init_ok");
    debugPrint("image-blit v0.4 (cycle 15 host-visible oracle channel)\n");
    /* Cycle 15: anchor line so harness/grep can confirm the
     * host-visible log channel reached at least once during this run.
     * Inert if XEMU_GUEST_LOG is unset. */
    xbed_host_log_write("image-blit: begin v0.4 host-log channel "
                        "(cycle 15; cells=0..7)");

    /* Vertex storage for the 8-cell dashboard quads. Allocated up
     * front but only populated after the per-cell verdicts land.
     *
     * HighestAcceptableAddress = MAXRAM (= 0x03FFAFFF), matching pbkit's
     * own contiguous allocs (`nxdk/lib/pbkit/pbkit.c:2297-2305`). Other
     * diag XBEs use `0x3FFB000` (= MAXRAM + 1), which is unsafe for
     * NV062/NV09F buffers: pbkit's `sDmaObject9` / `sDmaObject11` (used
     * by IMAGE_BLIT as source/destin DMA contexts) carry
     * `Limit = MAXRAM`, and `pgraph_mtl_image_blit` (also gl/vk siblings)
     * strictly asserts `source_offset < source_dma_len` /
     * `dest_offset < dest_dma_len`. A buffer landing in the top page
     * (`[MAXRAM+1, MAXRAM+0x1000]`) trips the assertion before
     * `METAL_IMAGE_BLITS` ever increments. Textures via NV097 don't
     * assert, which is why the existing first-wave XBEs survive
     * `0x3FFB000`; IMAGE_BLIT is the first XBE-library NV062/NV09F use. */
    s_alloc_verts = MmAllocateContiguousMemoryEx(
        sizeof(s_verts), 0, MAXRAM, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_alloc_verts) {
        debugPrint("image-blit: dashboard verts VRAM alloc failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    image_blit_marker(2, "verts_alloc_ok");

    /* Allocate VRAM for source + 8 dst surfaces. PAGE_WRITECOMBINE
     * matches what every other diag XBE uses for CPU-paint VRAM. */
    s_src_vram = MmAllocateContiguousMemoryEx(
        SRC_BYTES, 0, MAXRAM, 0,
        PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!s_src_vram) {
        debugPrint("image-blit: src VRAM alloc failed\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    image_blit_marker(3, "src_alloc_ok");
    fill_source_buffer();
    image_blit_marker(4, "src_filled");

    for (int idx = 0; idx < GRID_CELLS; idx++) {
        s_dst_vram[idx] = MmAllocateContiguousMemoryEx(
            DST_BYTES, 0, MAXRAM, 0,
            PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!s_dst_vram[idx]) {
            debugPrint("image-blit: dst[%d] VRAM alloc failed\n", idx);
            Sleep(2000);
            HalReturnToFirmware(HalRebootRoutine);
            return 1;
        }
        fill_dest_sentinel(s_dst_vram[idx]);
    }
    image_blit_marker(5, "dst_alloc_ok");

    /* Phase 1: execute all 8 IMAGE_BLITs + verify CPU-side. We do
     * this BEFORE loading the 3D shaders so the IMAGE_BLIT pushes
     * (which touch subchannels 3 and 4) cannot interact with any
     * partially-initialized 3D pipeline state on subchannel 0. */
    image_blit_marker(6, "before_blits");
    for (int idx = 0; idx < GRID_CELLS; idx++) {
        run_one_blit_cell(idx);
        if (idx == 0) {
            /* Marker 7 fires AFTER the first blit + oracle compare
             * specifically — this is the cycle-19 hypothesis-3 surface
             * (`pb_agp_access` divergence between xemu's emulated AGP
             * aperture and real-NV2A behavior). If marker 6 lands but
             * 7 does not, real-Xbox died inside the first blit fire
             * or the first oracle read-back. */
            image_blit_marker(7, "after_cell0");
        }
    }
    image_blit_marker(8, "after_all_blits");

    /* Cycle 15: emit a single-line tally through the host channel so
     * the harness can read the verdict total even if the per-cell
     * lines are filtered. Format `image-blit: tally pass=N/8 mask=0xXX`
     * with the mask bit-N set iff cell N passed. */
    {
        unsigned pass_count = 0;
        unsigned mask = 0;
        for (int idx = 0; idx < GRID_CELLS; idx++) {
            if (s_cell_pass[idx]) {
                pass_count++;
                mask |= (1u << (unsigned)idx);
            }
        }
        xbed_host_log_writef(
            "image-blit: tally pass=%u/%d mask=0x%02x",
            pass_count, GRID_CELLS, mask);
    }

    /* Phase 2: now set up the 3D pipeline for the dashboard. Default
     * render state + passthrough VS/PS (POSITION x viewport matrix
     * -> window coords; DIFFUSE -> COLOR). Matches the triangle
     * sample / texture-pitch-alignment patterns. */
    xbed_set_default_render_state();
    image_blit_marker(9, "state_set");
    xbed_load_default_shaders();
    image_blit_marker(10, "shaders_loaded");

    /* Build the dashboard geometry once with per-cell verdict
     * colors, then render it for n_frames so the host capture has
     * plenty of opportunities to land on a fully-rendered frame. */
    build_dashboard_geometry();
    image_blit_marker(11, "geometry_built");

    /* Marker 12 is the last checkpoint BEFORE the render loop and
     * the final `xbed_capture_front_to_xoss(D:\\image-blit-capture.bin)
     * + fopen(D:\\image-blit-done.txt)` tail. If marker 12 lands on
     * real Xbox but neither `image-blit-capture.bin` nor
     * `image-blit-done.txt` does, the failure is specifically in the
     * render-loop / PCRTC-publish / final capture+done fopen, NOT in
     * any of the earlier init/blit stages. Label kept short
     * (`pre_capture`) to stay clear of the FATX 42-char basename
     * limit — see image_blit_marker() comment. */
    image_blit_marker(12, "pre_capture");

    xbed_render_loop_then_capture(
        render_dashboard_frame, NULL, /*n_frames=*/300,
        "D:\\image-blit-capture.bin",
        "D:\\image-blit-done.txt",
        "image-blit");
    return 0;
}
