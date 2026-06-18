#include "xbed_capture.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <nxdk/mount.h>
#include <xboxkrnl/xboxkrnl.h>

struct xoss_header {
    uint8_t  magic[4];      /* 'X','O','S','S' */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

/* PCRTC_START register: the physical address the NV2A CRTC is currently
 * scanning out from. When pbkit owns the screen (via pb_init +
 * pb_show_front_screen), this is the pbkit-managed front buffer, NOT
 * the kernel-managed `XVideoGetFB()` framebuffer.
 *
 * Reading PCRTC_START gives us the address of whatever pixels the user
 * is actually seeing on the TV — which is what we want to capture.
 *
 * MMIO base for the NV2A is at virtual 0xFD000000 (a fixed Xbox kernel
 * mapping); PCRTC_START lives at offset 0x600800 inside that. */
#define NV2A_MMIO_BASE      0xFD000000u
#define NV2A_PCRTC_START    0x00600800u

static uint32_t s_read_pcrtc_start(void)
{
    volatile uint32_t *reg =
        (volatile uint32_t *)(uintptr_t)(NV2A_MMIO_BASE + NV2A_PCRTC_START);
    return *reg;
}

xbed_status_t xbed_capture_front_to_xoss(const char *xoss_path)
{
    /* Use kernel-reported video mode for dimensions; pbkit's swap chain
     * uses the same width/height/bpp as XVideoSetMode established. */
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

    /* Resolve the actual currently-displayed front-buffer address.
     *
     * Strategy: read PCRTC_START (the physical scan-out address the
     * NV2A is currently reading from); kseg0-map it to a virtual
     * pointer (`virt = phys | 0x80000000`); copy `pixel_bytes` from
     * there into the XOSS payload.
     *
     * Why this matters: pbkit's swap chain (pb_show_front_screen +
     * pb_finished + back-buffer rotation) uses pbkit-allocated
     * framebuffers, NOT the kernel's `XVideoGetFB()` framebuffer.
     * Diag XBEs that render via pbkit therefore have their pixels in
     * a different page than `XVideoGetFB()` returns, and an
     * `XVideoGetFB()`-based capture would grab a stale or
     * unrelated buffer.
     *
     * Fallback: if PCRTC_START reads as 0 (pbkit not initialized / no
     * CRTC programming yet), fall back to `XVideoGetFB()` so
     * CPU-painted XBEs (pipeline-smoke style) still capture
     * correctly. */
    uint32_t pcrtc = s_read_pcrtc_start();
    uint8_t *fb;
    const char *src_label;
    /* PCRTC_START holds a non-zero physical address in the 64 MiB
     * RAM window any time pbkit (or kernel D3D) has programmed the
     * CRTC. Use it as long as it points anywhere into RAM (not just
     * the upper 48 MiB, as a previous overly-restrictive check
     * implied — Codex 2026-05-07). Fall back to XVideoGetFB() only
     * when PCRTC is zero (CPU-paint scenarios like pipeline-smoke
     * that never call pb_init / pb_show_front_screen). */
    if (pcrtc != 0 && pcrtc < 0x04000000u) {
        /* kseg0 identity-map: phys P → virtual P | 0x80000000. */
        fb = (uint8_t *)(uintptr_t)(0x80000000u | (pcrtc & 0x03FFFFFFu));
        src_label = "pcrtc";
    } else {
        fb = XVideoGetFB();
        src_label = (pcrtc == 0) ? "kfb-pcrtc-zero" : "kfb-pcrtc-out-of-range";
    }
    if (!fb) {
        debugPrint("xbed_capture: no framebuffer pointer (pcrtc=0x%08lx)\n",
                   (unsigned long)pcrtc);
        return XBED_FAIL_CAPTURE;
    }
    debugPrint("xbed_capture: src=%s addr=%p pcrtc=0x%08lx\n",
               src_label, (void *)fb, (unsigned long)pcrtc);

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

/* Mount E: (FATX Partition1 — persistent and FTP-accessible) and ensure
 * E:\Apps\<xbe_id>\ exists, then build the capture + done-marker paths
 * there. The legacy callers pass D:\ paths, but under the UnleashX
 * `SITE EXEC` chainload D:\ is the read-only launch mount, so
 * fopen(...,"wb") fails and no capture blob lands for the orchestrator to
 * FTP-pull from /E/Apps/<id>/ (the real-Xbox Tier-1 capture blocker,
 * 2026-06-15). Mirrors the proven idiom in image-blit/main.c and
 * oracle-agent/controller.c (verified writable on this console under
 * chainload). Returns 1 on success (paths written), 0 if xbe_id is
 * NULL/empty or E: is unavailable, in which case the caller keeps its
 * passed-in (legacy D:\) paths. */
static int xbed_ensure_e_capture_paths(const char *xbe_id,
                                       char *xoss_out, size_t xoss_n,
                                       char *done_out, size_t done_n)
{
    if (!xbe_id || !xbe_id[0]) {
        return 0;
    }
    if (!nxIsDriveMounted('E')) {
        if (!nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")) {
            return 0;
        }
    }
    /* On real Xbox the harness FTP-uploads default.xbe to E:\Apps\<id>\
     * before chainload, so the dir already exists (CreateDirectoryA is a
     * no-op). On local xemu the scratch HDD has no such dir, so create it.
     * Idempotent. */
    char dir[96];
    CreateDirectoryA("E:\\Apps", NULL);
    snprintf(dir, sizeof dir, "E:\\Apps\\%s", xbe_id);
    CreateDirectoryA(dir, NULL);
    snprintf(xoss_out, xoss_n, "%s\\%s-capture.bin", dir, xbe_id);
    snprintf(done_out, done_n, "%s\\%s-done.txt", dir, xbe_id);
    return 1;
}

void xbed_capture_and_reboot(const char *xoss_path,
                             const char *done_path,
                             const char *xbe_id)
{
    VIDEO_MODE vm = XVideoGetMode();
    uint32_t stride = (uint32_t)vm.width * (uint32_t)((vm.bpp + 7) / 8);

    /* Redirect the capture + done-marker from the caller's legacy D:\
     * paths to the writable, FTP-collected E:\Apps\<id>\ partition. */
    char e_xoss[128], e_done[128];
    if (xbed_ensure_e_capture_paths(xbe_id, e_xoss, sizeof e_xoss,
                                    e_done, sizeof e_done)) {
        xoss_path = e_xoss;
        done_path = e_done;
    }

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
