/*
 * xbox-oracle-agent — Phase 2 command implementations.
 *
 * Memory read/write, NV2A register access, front-buffer screenshot,
 * VRAM window read, XBE chainload.
 *
 * Safety model:
 *   - Reads from a curated allowlist of address ranges (see
 *     op_addr_range_ok) are always allowed.
 *   - Writes (mem.write, nv2a.write) require an explicit
 *     `unsafe.enable` arming command per session. The arm flag is
 *     process-global and resets on agent restart (i.e. on reboot or
 *     any runxbe chainload).
 *   - All read lengths are capped at ORACLE_MAX_READ_LEN (1 MiB).
 *     Larger payloads should use multiple calls.
 */
#include "commands.h"
#include "protocol.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>
#include <nxdk/net.h>
#include <lwip/api.h>
#include <lwip/netif.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define EEPROM_SMBUS_ADDR  0xA8
#define EEPROM_SIZE        256
#define VERSION_STR        "xbox-oracle-agent v0.3 (Phase 2 + controller.*)"
#define ORACLE_MAX_READ_LEN (1u * 1024u * 1024u) /* 1 MiB */
#define ORACLE_MAX_WRITE_LEN 1024u
#define NV2A_BAR0_BASE     0xFD000000u
#define NV2A_BAR0_SIZE     0x01000000u

/* PCRTC_START offset in NV2A BAR0; same constant pbkit and the kernel use. */
#define NV_PCRTC_START_OFF 0x00600800u

extern struct netif *g_pnetif;

/* Process-global "writes are armed for this session" flag.
 * Only mem.write and nv2a.write check this. Resets on agent restart. */
static int s_unsafe_writes_enabled = 0;

/* Borrowed from Phase 1 — the EEPROM dump path. */
static int read_eeprom(unsigned char *out)
{
    for (unsigned int i = 0; i < EEPROM_SIZE; i++) {
        ULONG val = 0;
        NTSTATUS s = HalReadSMBusValue(EEPROM_SMBUS_ADDR, (UCHAR)i, FALSE, &val);
        if (!NT_SUCCESS(s)) return -1;
        out[i] = (unsigned char)(val & 0xFF);
    }
    return 0;
}

int cmd_info(struct netconn *c, const char *args)
{
    (void)args;
    VIDEO_MODE vm = XVideoGetMode();
    op_send_okf(c,
                "%s; ip=%s; mode=%dx%d@%dbpp; writes_enabled=%d",
                VERSION_STR,
                ip4addr_ntoa(netif_ip4_addr(g_pnetif)),
                vm.width, vm.height, vm.bpp,
                s_unsafe_writes_enabled);
    return 0;
}

int cmd_eeprom(struct netconn *c, const char *args)
{
    (void)args;
    unsigned char eeprom[EEPROM_SIZE];
    memset(eeprom, 0, sizeof(eeprom));
    if (read_eeprom(eeprom) != 0) {
        op_send_errf(c, "HalReadSMBusValue failed");
        return 0;
    }
    op_send_text_begin(c, EEPROM_SIZE);
    char hex[64 + 4];
    for (int row = 0; row < EEPROM_SIZE; row += 32) {
        int p = 0;
        for (int j = 0; j < 32 && (row + j) < EEPROM_SIZE; j++) {
            p += snprintf(hex + p, sizeof(hex) - p, "%02X", eeprom[row + j]);
        }
        op_send_line(c, hex);
    }
    op_send_text_end(c);
    return 0;
}

int cmd_unsafe_enable(struct netconn *c, const char *args)
{
    (void)args;
    s_unsafe_writes_enabled = 1;
    op_send_okf(c, "writes enabled for this session");
    return 0;
}

/* mem.read addr=0xHHHH len=N
 *   Reads up to len bytes from one of the four Xbox RAM aliases
 *   (low alias / kseg0 / kseg1 / VRAM aperture). Replies with a
 *   202- BINARY <len> header followed by exactly len raw bytes.
 *   Out-of-allowlist addresses (including all MMIO ranges) are
 *   refused with 500-; use `nv2a.read` for NV2A BAR0 registers.
 */
int cmd_mem_read(struct netconn *c, const char *args)
{
    uint32_t addr = 0, len = 0;
    if (op_parse_kv_u32(args, "addr", &addr) != 0 ||
        op_parse_kv_u32(args, "len", &len) != 0) {
        op_send_errf(c, "usage: mem.read addr=0xHEX len=N");
        return 0;
    }
    if (len == 0 || len > ORACLE_MAX_READ_LEN) {
        op_send_errf(c, "len out of range (1..%u)", ORACLE_MAX_READ_LEN);
        return 0;
    }
    if (!op_addr_range_ok(addr, len)) {
        op_send_errf(c, "addr range 0x%08lx+%lu not in allowlist",
                     (unsigned long)addr, (unsigned long)len);
        return 0;
    }
    /* On the Xbox the kernel virtual address space contains a 1:1
     * mapping for the four RAM-alias windows (low alias / kseg0 /
     * kseg1 / VRAM aperture), so a direct pointer dereference works
     * for every allowlisted range. */
    op_send_binary_header(c, len);
    op_send_binary_payload(c, (const void *)(uintptr_t)addr, (size_t)len);
    return 0;
}

/* mem.write addr=0xHHHH data=<hex>
 *   Writes hex-encoded bytes. Requires unsafe.enable.
 */
int cmd_mem_write(struct netconn *c, const char *args)
{
    /* Static scratch (moved off the stack to avoid blowing nxdk's
     * default thread stack budget when this command is dispatched
     * back-to-back with screenshot-sized binary writes elsewhere). */
    static char    data_hex[ORACLE_MAX_WRITE_LEN * 2 + 4];
    static uint8_t buf[ORACLE_MAX_WRITE_LEN];

    if (!s_unsafe_writes_enabled) {
        op_send_errf(c, "writes disabled — call unsafe.enable first");
        return 0;
    }
    uint32_t addr = 0;
    if (op_parse_kv_u32(args, "addr", &addr) != 0 ||
        op_parse_kv_str(args, "data", data_hex, sizeof(data_hex)) != 0) {
        op_send_errf(c, "usage: mem.write addr=0xHEX data=<hex>");
        return 0;
    }
    size_t blen = 0;
    if (op_parse_hex_buf(data_hex, buf, sizeof(buf), &blen) != 0) {
        op_send_errf(c, "data hex invalid or too long (max %u bytes)",
                     ORACLE_MAX_WRITE_LEN);
        return 0;
    }
    if (!op_addr_range_ok(addr, (uint32_t)blen)) {
        op_send_errf(c, "addr range 0x%08lx+%lu not in allowlist",
                     (unsigned long)addr, (unsigned long)blen);
        return 0;
    }
    memcpy((void *)(uintptr_t)addr, buf, blen);
    op_send_okf(c, "wrote %u bytes at 0x%08lx", (unsigned)blen,
                (unsigned long)addr);
    return 0;
}

int cmd_nv2a_read(struct netconn *c, const char *args)
{
    uint32_t off = 0;
    if (op_parse_kv_u32(args, "off", &off) != 0) {
        op_send_errf(c, "usage: nv2a.read off=0xHEX");
        return 0;
    }
    if ((off & 3u) != 0) {
        op_send_errf(c, "off must be 4-byte aligned");
        return 0;
    }
    if (off >= NV2A_BAR0_SIZE) {
        op_send_errf(c, "off 0x%08lx outside BAR0", (unsigned long)off);
        return 0;
    }
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(NV2A_BAR0_BASE + off);
    uint32_t val = *p;
    op_send_okf(c, "0x%08lx", (unsigned long)val);
    return 0;
}

int cmd_nv2a_write(struct netconn *c, const char *args)
{
    if (!s_unsafe_writes_enabled) {
        op_send_errf(c, "writes disabled — call unsafe.enable first");
        return 0;
    }
    uint32_t off = 0, val = 0;
    if (op_parse_kv_u32(args, "off", &off) != 0 ||
        op_parse_kv_u32(args, "val", &val) != 0) {
        op_send_errf(c, "usage: nv2a.write off=0xHEX val=0xHEX");
        return 0;
    }
    if ((off & 3u) != 0) {
        op_send_errf(c, "off must be 4-byte aligned");
        return 0;
    }
    if (off >= NV2A_BAR0_SIZE) {
        op_send_errf(c, "off 0x%08lx outside BAR0", (unsigned long)off);
        return 0;
    }
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(NV2A_BAR0_BASE + off);
    *p = val;
    op_send_okf(c, "wrote 0x%08lx at off 0x%08lx",
                (unsigned long)val, (unsigned long)off);
    return 0;
}

/* vram.read off=0xHHHH len=N
 *   Reads N bytes from the NV2A "VRAM" window starting at the
 *   write-combined aperture base 0xF0000000. On Xbox, "VRAM" is
 *   just the 64 MB system RAM, accessed through a different cache
 *   attribute window. Use this command when the caller has a
 *   physical offset into the framebuffer/texture pool and wants the
 *   pre-flush contents (avoiding the cached kseg0 alias). */
int cmd_vram_read(struct netconn *c, const char *args)
{
    uint32_t off = 0, len = 0;
    if (op_parse_kv_u32(args, "off", &off) != 0 ||
        op_parse_kv_u32(args, "len", &len) != 0) {
        op_send_errf(c, "usage: vram.read off=0xHEX len=N");
        return 0;
    }
    if (len == 0 || len > ORACLE_MAX_READ_LEN) {
        op_send_errf(c, "len out of range (1..%u)", ORACLE_MAX_READ_LEN);
        return 0;
    }
    if ((uint64_t)off + (uint64_t)len > 0x04000000ull) {
        op_send_errf(c, "off+len exceeds 64 MiB VRAM window");
        return 0;
    }
    uint32_t addr = 0xF0000000u + off;
    op_send_binary_header(c, len);
    op_send_binary_payload(c, (const void *)(uintptr_t)addr, (size_t)len);
    return 0;
}

/* screenshot
 *   Captures the current front buffer. Returns:
 *     202- BINARY <total>
 *     <16-byte header><pixels>
 *
 *   Header (little-endian):
 *     u32 magic     = 'XOSS' (0x53534F58)
 *     u32 width
 *     u32 height
 *     u32 stride    in bytes per row
 *
 *   Pixel format is the raw front-buffer layout reported by
 *   XVideoGetMode(): typically 32 bpp X8R8G8B8 (BGRA in memory).
 *   The Mac client converts to RGBA / PNG.
 *
 *   The kernel uses XVideoGetFB() -> _fb (the active front-buffer
 *   pointer it published to PCRTC_START). This is the same memory
 *   the GPU display engine is scanning out at vblank, so the read
 *   may show a tear at the active scan line — mostly negligible at
 *   60 Hz and a single-frame snapshot. To minimize tearing, we wait
 *   for vblank before sampling.
 */
int cmd_screenshot(struct netconn *c, const char *args)
{
    (void)args;
    VIDEO_MODE vm = XVideoGetMode();
    if (vm.width <= 0 || vm.height <= 0 || vm.bpp <= 0) {
        op_send_errf(c, "no active video mode (w=%d h=%d bpp=%d)",
                     vm.width, vm.height, vm.bpp);
        return 0;
    }
    uint32_t bytes_per_pixel = (uint32_t)((vm.bpp + 7) / 8);
    uint32_t stride = (uint32_t)vm.width * bytes_per_pixel;
    uint64_t pixel_bytes = (uint64_t)stride * (uint64_t)vm.height;
    if (pixel_bytes == 0 || pixel_bytes > 0x01000000ull) {
        op_send_errf(c, "framebuffer size %llu out of range",
                     (unsigned long long)pixel_bytes);
        return 0;
    }
    uint8_t *fb = XVideoGetFB();
    if (!fb) {
        op_send_errf(c, "XVideoGetFB returned NULL");
        return 0;
    }

    /* Wait for vblank to reduce tearing on the captured frame. */
    XVideoWaitForVBlank();

    uint8_t header[16];
    header[0]  = 'X'; header[1]  = 'O'; header[2]  = 'S'; header[3]  = 'S';
    header[4]  = (uint8_t)(vm.width >> 0);
    header[5]  = (uint8_t)(vm.width >> 8);
    header[6]  = (uint8_t)(vm.width >> 16);
    header[7]  = (uint8_t)(vm.width >> 24);
    header[8]  = (uint8_t)(vm.height >> 0);
    header[9]  = (uint8_t)(vm.height >> 8);
    header[10] = (uint8_t)(vm.height >> 16);
    header[11] = (uint8_t)(vm.height >> 24);
    header[12] = (uint8_t)(stride >> 0);
    header[13] = (uint8_t)(stride >> 8);
    header[14] = (uint8_t)(stride >> 16);
    header[15] = (uint8_t)(stride >> 24);

    size_t total = sizeof(header) + (size_t)pixel_bytes;
    op_send_binary_header(c, total);
    op_send_binary_payload(c, header, sizeof(header));
    op_send_binary_payload(c, fb, (size_t)pixel_bytes);
    return 0;
}

/* runxbe path=<xbox-path>
 *   Chainloads the named XBE. The kernel performs an in-place
 *   image swap: this oracle terminates and the named XBE takes
 *   over. The agent therefore does NOT auto-relaunch — the
 *   dashboard has no startup-app concept, so the orchestrator on
 *   the Mac side must FTP-launch the agent again after the
 *   chainloaded XBE reboots back to the dashboard.
 *
 *   Path examples:
 *     C:\\xboxdash.xbe
 *     E:\\Apps\\diag-mirror\\default.xbe
 *
 *   Caller must escape backslashes in JSON / shell layers.
 */
int cmd_runxbe(struct netconn *c, const char *args)
{
    char path[260];
    if (op_parse_kv_str(args, "path", path, sizeof(path)) != 0) {
        op_send_errf(c, "usage: runxbe path=<xbox-path>");
        return 0;
    }
    op_send_okf(c, "launching %s", path);
    /* Best effort: drain the netconn and close the listener before
     * blowing away our own image. */
    netconn_close(c);
    Sleep(500);
    XLaunchXBE(path);
    /* Should not return. If it does, the path was invalid; reboot
     * back to dashboard so we don't strand the Xbox. */
    Sleep(2000);
    HalReturnToFirmware(HalRebootRoutine);
    return 2;
}

int cmd_reboot(struct netconn *c, const char *args)
{
    (void)args;
    op_send_okf(c, "rebooting");
    netconn_close(c);
    Sleep(500);
    HalReturnToFirmware(HalRebootRoutine);
    return 2;
}

int cmd_bye(struct netconn *c, const char *args)
{
    (void)args;
    op_send_okf(c, "bye");
    return 1;
}

int cmd_help(struct netconn *c, const char *args)
{
    (void)args;
    op_send_text_begin(c, 0);
    op_send_line(c, "info                                  agent + console info");
    op_send_line(c, "eeprom                                256-byte EEPROM hex dump");
    op_send_line(c, "mem.read addr=0xHEX len=N             binary read of memory");
    op_send_line(c, "mem.write addr=0xHEX data=<hex>       binary write (needs unsafe.enable)");
    op_send_line(c, "nv2a.read off=0xHEX                   read NV2A BAR0 register");
    op_send_line(c, "nv2a.write off=0xHEX val=0xHEX        write NV2A reg (needs unsafe.enable)");
    op_send_line(c, "vram.read off=0xHEX len=N             read NV2A VRAM aperture");
    op_send_line(c, "screenshot                            capture front buffer (XOSS+pixels)");
    op_send_line(c, "runxbe path=<xbox-path>               chainload another XBE");
    op_send_line(c, "unsafe.enable                         arm mem.write + nv2a.write");
    op_send_line(c, "controller.set port=N [...]           update synthetic input state");
    op_send_line(c, "controller.button port=N name=X value=V edit one button by xemu name");
    op_send_line(c, "controller.axis   port=N name=X value=V edit one axis by xemu name");
    op_send_line(c, "controller.get [port=N]               read back synthetic input state");
    op_send_line(c, "controller.clear [port=N]             zero one or all ports");
    op_send_line(c, "controller.buffer-info                buffer addr/size for shim hooks");
    op_send_line(c, "reboot                                reboot to dashboard");
    op_send_line(c, "bye                                   close connection");
    op_send_line(c, "help                                  this list");
    op_send_text_end(c);
    return 0;
}
