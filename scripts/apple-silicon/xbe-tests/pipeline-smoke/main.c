/*
 * pipeline-smoke — diag-XBE pipeline validator
 *
 * Tier classification: 4 (visual-only). This XBE does NOT exercise
 * the NV2A pgraph pipeline — pixels are CPU-painted directly into
 * the front buffer. Its purpose is to exercise the orchestrator's
 * run-diag chainload-and-back cycle end-to-end with a Tier-1
 * NV2A-based diag XBE pending. See `docs/apple-silicon/diagnostic-xbe-plan.md`.
 *
 * Math derivation (single-pixel oracle):
 *   - Surface: 640x480, 32-bit X8R8G8B8 (B G R X bytes in memory).
 *   - Background: all pixels exactly 0xFF000000 (opaque black).
 *   - Foreground: pixel at guest coord (320, 50) exactly 0xFFFFFFFF
 *     (opaque white). Coordinate is (col, row) with row 0 at the
 *     top of the screen.
 *   - Mirror-bug detection (informational): pixel at (320, 429) =
 *     (320, 480-1-50) should be 0xFF000000. A mirroring renderer
 *     would paint white there too.
 *
 * What this XBE does at runtime:
 *   1. XVideoSetMode(640, 480, 32) and grab the front-buffer pointer
 *      via XVideoGetFB(). The framebuffer is in physical RAM;
 *      writes are directly visible to the display engine after
 *      XVideoFlushFB() (sfence).
 *   2. CPU-paint the pattern.
 *   3. Sleep briefly so a human watching the TV can spot-check.
 *   4. Write the painted framebuffer to D:\pipeline-smoke-capture.bin
 *      in XOSS format (4-byte magic + u32 width + u32 height + u32
 *      stride + raw pixels) — same encoding the agent's `screenshot`
 *      command uses, so oracle-client.py's bgrx_to_rgba decoder
 *      handles it directly.
 *   5. Write D:\pipeline-smoke-done.txt as a liveness marker.
 *   6. HalReturnToFirmware(HalRebootRoutine) to warm-reset back to
 *      the dashboard so the orchestrator can relaunch the agent
 *      and pull the artifacts via FTP.
 *
 * D:\ resolves to the launched-XBE's parent directory at runtime
 * (kernel auto-mapping). With this XBE deployed at
 * E:\XBMC4Gamers\Apps\pipeline-smoke\default.xbe, D:\* lands at
 * E:\XBMC4Gamers\Apps\pipeline-smoke\* — FTP-accessible.
 *
 * Reproducibility: byte-identical-after-mask across runs (no banner,
 * no timestamps, no frame counter; pure deterministic pattern).
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define FB_W       640
#define FB_H       480
#define FB_BPP     32
#define FB_BYTES   ((uint32_t)FB_W * FB_H * (FB_BPP / 8))
#define WHITE_X    320
#define WHITE_Y    50

#define CAPTURE_BIN  "D:\\pipeline-smoke-capture.bin"
#define DONE_TXT     "D:\\pipeline-smoke-done.txt"

/* XOSS header — exact same layout as the oracle agent's screenshot
 * payload, so the same Python decoder handles both. */
struct xoss_header {
    uint8_t  magic[4];   /* 'X','O','S','S' */
    uint32_t width;
    uint32_t height;
    uint32_t stride;     /* bytes per scan line */
};

static void paint_pattern(uint8_t *fb, uint32_t stride)
{
    /* Clear: Xbox front-buffer at 32 bpp is X8R8G8B8 little-endian
     * (B G R X bytes in memory). Opaque black = 0xFF000000 = bytes
     * 00 00 00 FF (little-endian on x86). For "all black + alpha
     * 0xFF" we need bytes B=0 G=0 R=0 X=0xFF. */
    for (uint32_t y = 0; y < FB_H; y++) {
        uint32_t *row = (uint32_t *)(fb + (size_t)y * stride);
        for (uint32_t x = 0; x < FB_W; x++) {
            row[x] = 0xFF000000u;
        }
    }
    /* Single white pixel at (WHITE_X, WHITE_Y). 0xFFFFFFFF. */
    uint32_t *row = (uint32_t *)(fb + (size_t)WHITE_Y * stride);
    row[WHITE_X] = 0xFFFFFFFFu;
}

static int write_capture(const uint8_t *fb, uint32_t stride)
{
    FILE *f = fopen(CAPTURE_BIN, "wb");
    if (!f) {
        debugPrint("fopen %s failed\n", CAPTURE_BIN);
        return -1;
    }
    struct xoss_header hdr;
    hdr.magic[0] = 'X';
    hdr.magic[1] = 'O';
    hdr.magic[2] = 'S';
    hdr.magic[3] = 'S';
    hdr.width  = FB_W;
    hdr.height = FB_H;
    hdr.stride = stride;
    /* Verify partial writes don't slip by silently — a truncated
     * D:\pipeline-smoke-capture.bin would FTP-pull cleanly but
     * decode wrong on the host side. */
    size_t hdr_n = fwrite(&hdr, 1, sizeof(hdr), f);
    /* Write the pixels exactly as they appear in front-buffer
     * memory. Caller's stride may exceed FB_W*4 if the kernel
     * round-up'd the row pitch; we still copy `stride * height`
     * bytes so the manifest math matches. */
    size_t want_pixels = (size_t)stride * FB_H;
    size_t got_pixels  = fwrite(fb, 1, want_pixels, f);
    int rc = 0;
    if (hdr_n != sizeof(hdr) || got_pixels != want_pixels) {
        debugPrint("write_capture short write: hdr=%u/%u pixels=%u/%u\n",
                   (unsigned)hdr_n, (unsigned)sizeof(hdr),
                   (unsigned)got_pixels, (unsigned)want_pixels);
        rc = -1;
    }
    if (fclose(f) != 0) {
        debugPrint("fclose %s failed\n", CAPTURE_BIN);
        rc = -1;
    }
    return rc;
}

static void write_done_marker(uint32_t stride)
{
    FILE *f = fopen(DONE_TXT, "wb");
    if (!f) return;
    fprintf(f,
            "pipeline-smoke v0.1\n"
            "width=%d height=%d bpp=%d stride=%u\n"
            "capture=%s\n"
            "expected_white_pixel=(%d,%d)\n",
            FB_W, FB_H, FB_BPP, (unsigned)stride,
            CAPTURE_BIN, WHITE_X, WHITE_Y);
    fclose(f);
}

int main(void)
{
    XVideoSetMode(FB_W, FB_H, FB_BPP, REFRESH_DEFAULT);
    debugPrint("\npipeline-smoke v0.1\n");
    debugPrint("FB %dx%d@%dbpp\n", FB_W, FB_H, FB_BPP);

    uint8_t *fb = XVideoGetFB();
    if (!fb) {
        debugPrint("XVideoGetFB returned NULL\n");
        Sleep(2000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    /* The kernel doesn't expose the row stride directly; for the
     * standard 640x480x32 NTSC mode it's exactly width*4 = 2560.
     * Hard-code that and document it. If the kernel ever pads,
     * the host-side decoder will see the discrepancy via the XOSS
     * header (stride field) and adjust. */
    uint32_t stride = (uint32_t)FB_W * (FB_BPP / 8);

    paint_pattern(fb, stride);
    XVideoFlushFB();

    /* Hold pattern visible briefly so a human at the TV can spot-
     * check before the reboot. */
    Sleep(1500);

    if (write_capture(fb, stride) != 0) {
        debugPrint("write_capture failed\n");
    } else {
        debugPrint("wrote %s (%u bytes)\n",
                   CAPTURE_BIN,
                   (unsigned)(sizeof(struct xoss_header) + (size_t)stride * FB_H));
    }
    write_done_marker(stride);

    Sleep(500);
    debugPrint("rebooting\n");
    HalReturnToFirmware(HalRebootRoutine);
    /* not reached */
    return 0;
}
