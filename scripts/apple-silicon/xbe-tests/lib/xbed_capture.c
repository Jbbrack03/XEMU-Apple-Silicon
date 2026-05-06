#include "xbed_capture.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

struct xoss_header {
    uint8_t  magic[4];      /* 'X','O','S','S' */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

xbed_status_t xbed_capture_front_to_xoss(const char *xoss_path)
{
    /* Use kernel-reported video mode and front-buffer pointer; mirrors
     * the agent's `cmd_screenshot` and pipeline-smoke. */
    VIDEO_MODE vm = XVideoGetMode();
    if (vm.width <= 0 || vm.height <= 0 || vm.bpp <= 0) {
        debugPrint("xbed_capture: bad video mode w=%d h=%d bpp=%d\n",
                   vm.width, vm.height, vm.bpp);
        return XBED_FAIL_CAPTURE;
    }
    uint32_t bytes_per_pixel = (uint32_t)((vm.bpp + 7) / 8);
    uint32_t stride = (uint32_t)vm.width * bytes_per_pixel;
    uint64_t pixel_bytes = (uint64_t)stride * (uint64_t)vm.height;
    if (pixel_bytes == 0 || pixel_bytes > 0x01000000ull) {
        debugPrint("xbed_capture: framebuffer too big: %llu\n",
                   (unsigned long long)pixel_bytes);
        return XBED_FAIL_CAPTURE;
    }
    uint8_t *fb = XVideoGetFB();
    if (!fb) {
        debugPrint("xbed_capture: XVideoGetFB returned NULL\n");
        return XBED_FAIL_CAPTURE;
    }

    /* Wait for vblank so the front-buffer scan-out doesn't tear our
     * capture across two presented frames. */
    XVideoWaitForVBlank();

    FILE *f = fopen(xoss_path, "wb");
    if (!f) {
        debugPrint("xbed_capture: fopen %s failed\n", xoss_path);
        return XBED_FAIL_CAPTURE;
    }
    struct xoss_header hdr;
    hdr.magic[0] = 'X'; hdr.magic[1] = 'O';
    hdr.magic[2] = 'S'; hdr.magic[3] = 'S';
    hdr.width  = (uint32_t)vm.width;
    hdr.height = (uint32_t)vm.height;
    hdr.stride = stride;
    size_t hn = fwrite(&hdr, 1, sizeof(hdr), f);
    size_t pn = fwrite(fb,  1, (size_t)pixel_bytes, f);
    int rc = (hn == sizeof(hdr) && pn == (size_t)pixel_bytes) ? 0 : -1;
    if (rc != 0) {
        debugPrint("xbed_capture: short write hdr=%u/%u px=%u/%u\n",
                   (unsigned)hn, (unsigned)sizeof(hdr),
                   (unsigned)pn, (unsigned)pixel_bytes);
    }
    if (fclose(f) != 0) {
        debugPrint("xbed_capture: fclose %s failed\n", xoss_path);
        rc = -1;
    }
    return rc == 0 ? XBED_OK : XBED_FAIL_CAPTURE;
}

void xbed_render_loop_then_capture(xbed_frame_fn fn, void *ctx,
                                   uint32_t n_frames,
                                   const char *xoss_path,
                                   const char *done_path,
                                   const char *xbe_id)
{
    /* Pre-loop: caller has already done lib init + shader load. We
     * render the same pattern n_frames times to give the host
     * (xemu's in-renderer screenshot path) plenty of opportunities
     * to land on the post-flip drawable. */
    extern void xbed_frame_begin(void);
    extern void xbed_frame_end_and_swap(void);
    if (fn == NULL || n_frames == 0) {
        xbed_capture_and_reboot(xoss_path, done_path, xbe_id);
        return;
    }
    for (uint32_t i = 0; i < n_frames; i++) {
        xbed_frame_begin();
        fn(i, ctx);
        xbed_frame_end_and_swap();
    }
    xbed_capture_and_reboot(xoss_path, done_path, xbe_id);
}

void xbed_capture_and_reboot(const char *xoss_path,
                             const char *done_path,
                             const char *xbe_id)
{
    VIDEO_MODE vm = XVideoGetMode();
    uint32_t stride = (uint32_t)vm.width * (uint32_t)((vm.bpp + 7) / 8);

    /* Hold pattern visible briefly so a human watching the TV can
     * spot-check before the reboot. Same 1.5 s pipeline-smoke uses. */
    Sleep(1500);

    xbed_status_t cs = xbed_capture_front_to_xoss(xoss_path);
    if (cs != XBED_OK) {
        debugPrint("xbed_capture_and_reboot: capture failed (%d)\n", (int)cs);
    } else {
        debugPrint("wrote %s\n", xoss_path);
    }

    FILE *f = fopen(done_path, "wb");
    if (f) {
        fprintf(f,
                "%s\n"
                "width=%d height=%d bpp=%d stride=%u\n"
                "capture=%s\n"
                "capture_status=%d\n",
                xbe_id ? xbe_id : "diag",
                vm.width, vm.height, vm.bpp, (unsigned)stride,
                xoss_path, (int)cs);
        fclose(f);
    } else {
        debugPrint("xbed_capture_and_reboot: fopen %s failed\n", done_path);
    }

    Sleep(500);
    debugPrint("rebooting\n");
    HalReturnToFirmware(HalRebootRoutine);
    /* not reached */
}
