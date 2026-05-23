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
#include "controller.h"
#include "protocol.h"
#include "smc.h"

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
#define VERSION_STR        "xbox-oracle-agent v0.4 (Phase 2 + controller.* + smc.*)"
#define ORACLE_MAX_READ_LEN (1u * 1024u * 1024u) /* 1 MiB */
#define ORACLE_MAX_WRITE_LEN 1024u
#define NV2A_BAR0_BASE     0xFD000000u
#define NV2A_BAR0_SIZE     0x01000000u

/* PCRTC_START offset in NV2A BAR0; same constant pbkit and the kernel use. */
#define NV_PCRTC_START_OFF 0x00600800u

extern struct netif *g_pnetif;

/* Process-global "writes are armed for this session" flag.
 * Checked by all mutating commands: mem.write, nv2a.write, smc.write,
 * smc.fan. Resets on agent restart. */
static int s_unsafe_writes_enabled = 0;

int oracle_writes_enabled(void)
{
    return s_unsafe_writes_enabled;
}

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
    const char *p = strstr(args, "path=");
    if (!p) {
        op_send_errf(c, "usage: runxbe path=<xbox-path>");
        return 0;
    }
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (p[n] && n + 1 < sizeof(path)) n++;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' ||
                     p[n - 1] == '\r' || p[n - 1] == '\n')) {
        n--;
    }
    if (n == 0) {
        op_send_errf(c, "usage: runxbe path=<xbox-path>");
        return 0;
    }
    memcpy(path, p, n);
    path[n] = 0;
    /* This call replaces the agent image, so any session-set fan
     * curve would be stranded. Same rationale as cmd_reboot. */
    oracle_smc_cleanup_if_manual();
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
    /* If this session put the SMC into manual fan mode, restore auto
     * before the kernel reboots. We don't know whether Xyclops/PIC
     * retains FANMODE across a soft reboot — fail-safe to auto. */
    oracle_smc_cleanup_if_manual();
    op_send_okf(c, "rebooting");
    netconn_close(c);
    Sleep(500);
    HalReturnToFirmware(HalRebootRoutine);
    return 2;
}

int cmd_bye(struct netconn *c, const char *args)
{
    (void)args;
    /* `bye` only closes the connection — the agent stays alive.
     * Do NOT touch fan state here; oracle-client.py's polite-close
     * sends `bye` after every command, which would silently revert
     * any caller-set manual fan curve. Cleanup happens only on
     * agent-exit paths (cmd_reboot, cmd_runxbe). */
    op_send_okf(c, "bye");
    return 1;
}

/* Cycle 23 Path A.4 — non-fopen kernel-pool controller-buffer witness
 * readback. Scans kseg0 [0x80010000, 0x84000000] in 4 KiB strides for
 * every `oracle_ctrl_buffer` header instance that passes the SAME
 * candidate filter set the writer uses in
 * `lib/xbed_a4_witness.c::a4_candidate_ok`. For each match, reports
 * `(phys_addr, virt_addr, live, reserved[0], reserved[1])`.
 *
 * Why this exists (cycle-23 design — see
 * `docs/apple-silicon/decision-log.md` cycle-23 entry +
 * `lib/xbed_a4_witness.h` body comment):
 *
 *   - The agent allocates a FRESH persistent buffer on every restart
 *     (`controller.c::s_allocate_fresh` is unconditional; previous
 *     opt-in reattach build was removed for production). It leaks a
 *     4 KiB persistent page per restart until the Xbox is power-
 *     cycled (`controller.c:207-208`).
 *   - Image-blit's A.4 witness writes into the HIGHEST-phys candidate
 *     it finds (the most recent agent allocation — the agent's
 *     allocator grows monotonically per restart). The witness leaves
 *     the magic + version intact, so subsequent scans still recognize
 *     the buffer.
 *   - After image-blit reboots back to FTP and the agent restarts,
 *     calling `witness.scan` enumerates the live buffer + any orphan
 *     persistent pages from earlier agent runs. The orphan that
 *     image-blit stamped will appear with `reserved[0] = 0xA4xxxxxx`.
 *
 * Cycle-23 expected output shapes (from a CLEAN power-cycled state;
 * see the "Hard precondition" line in `lib/xbed_a4_witness.h` —
 * Hermes must power-cycle the Xbox if multiple orphans already
 * exist before the cycle-24 run):
 *
 *   count=1 with reserved0=0x00000000        → no orphans; agent has
 *                                               just started for the
 *                                               first time this
 *                                               power-cycle (witness
 *                                               can't fire yet).
 *   count=2 with orphan reserved0=0x00000000 → image-blit ran but did
 *                                               NOT reach main()'s
 *                                               first instruction
 *                                               (cycle-22 leading
 *                                               hypothesis CORROBORATED).
 *   count=2 with orphan reserved0=0xA4000001 → image-blit reached
 *                                               MAIN_ENTERED only
 *                                               (marker_00 itself
 *                                               crashed).
 *   count=2 with orphan reserved0=0xA4000003 → image-blit reached
 *                                               POST_MARKER0 (cycle-22
 *                                               leading hypothesis
 *                                               INVALIDATED; marker_00
 *                                               fopen-fails silently
 *                                               and code continues).
 *
 * Cycle-27 additional success shape (`controller.c::s_allocate_fresh`
 * preserve branch): real-Xbox cycle 26 observed the kernel pool
 * deterministically returns the same persistent phys (e.g.
 * phys=0x03eb3000) across agent re-launches in one power session.
 * When that happens AND the relaunched agent's `s_allocate_fresh` now
 * detects a plausible A.4-tagged header on the returned page, the
 * preserve branch keeps `reserved[0,1]` intact. Therefore the LIVE
 * buffer itself can now carry the stamp:
 *
 *   count=1 with live=1 reserved0=0xA4xxxxxx → witness fire DID land
 *                                               AND the kernel pool
 *                                               returned the same
 *                                               persistent page on
 *                                               agent re-launch; the
 *                                               preserve branch
 *                                               retained the stamp on
 *                                               the live buffer (no
 *                                               separate orphan in
 *                                               this case).
 *
 * Operationally: with cycle-27 in effect, EITHER the legacy orphan
 * shape (count>=2 with a stamped orphan) OR the new live-buffer shape
 * (count=1 with live=1 and a stamped reserved0) is a positive
 * "witness landed" outcome; `count=1 live=1 reserved0=0` remains the
 * "no stamp landed" baseline.
 *
 * Safety (Codex cycle-23 finding #1): kseg0 [0x80010000, 0x84000000]
 * is NOT fully identity-mapped on the OG Xbox; only pages the kernel
 * has actually allocated are valid. The writer's xemu-Metal local
 * validation crashed on blind dereference (see
 * `lib/xbed_a4_witness.c::xbed_a4_witness_fire` body comment) and
 * required `MmGetPhysicalAddress` gating. The reader MUST use the
 * same gate; this implementation does.
 *
 * Filter parity (Codex cycle-23 finding #2): magic + version alone
 * matched random pages in the writer's local run. The reader applies
 * the SAME extra filters the writer uses: `reserved[0]` must be 0 or
 * A.4-tagged, `reserved[1]` must be < A4_MAX_PLAUSIBLE_COUNTER. Both
 * sides MUST stay in lockstep on this filter set; if you change one,
 * change the other in the same commit. */
#define A4_RDR_KSEG0_SCAN_START  0x80010000u
#define A4_RDR_KSEG0_SCAN_END    0x84000000u
#define A4_RDR_PAGE_STRIDE       0x1000u
#define A4_RDR_WITNESS_TAG       0xA4u
#define A4_RDR_MAX_COUNTER       4096u

static int a4_reader_candidate_ok(uintptr_t va)
{
    if ((uintptr_t)MmGetPhysicalAddress((PVOID)va) == 0u) return 0;
    volatile uint32_t *p = (volatile uint32_t *)va;
    if (p[0] != ORACLE_CTRL_MAGIC) return 0;
    if (p[1] != ORACLE_CTRL_VERSION) return 0;
    uint32_t r0 = p[2];
    if (r0 != 0u && ((r0 >> 24) != A4_RDR_WITNESS_TAG)) return 0;
    uint32_t r1 = p[3];
    if (r1 > A4_RDR_MAX_COUNTER) return 0;
    return 1;
}

int cmd_witness_scan(struct netconn *c, const char *args)
{
    (void)args;
    op_send_text_begin(c, 0);
    int reported = 0;
    uint32_t mapped_pages_seen = 0;
    for (uintptr_t va = A4_RDR_KSEG0_SCAN_START;
         va < A4_RDR_KSEG0_SCAN_END;
         va += A4_RDR_PAGE_STRIDE) {
        if ((uintptr_t)MmGetPhysicalAddress((PVOID)va) == 0u) continue;
        mapped_pages_seen++;
        if (!a4_reader_candidate_ok(va)) continue;

        volatile uint32_t *p = (volatile uint32_t *)va;
        char buf[200];
        uintptr_t phys = (uintptr_t)va & 0x03FFFFFFu;
        /* Mark the live buffer (the one this agent allocated this boot
         * and pointed `oracle_ctrl_get()` at) so the host side doesn't
         * have to cross-reference `controller.buffer-info`. */
        const int is_live =
            ((uintptr_t)oracle_ctrl_get() == va) ? 1 : 0;
        snprintf(buf, sizeof(buf),
                 "buf.%d phys=0x%08lx virt=0x%08lx live=%d "
                 "reserved0=0x%08lx reserved1=0x%08lx",
                 reported, (unsigned long)phys, (unsigned long)va,
                 is_live,
                 (unsigned long)p[2], (unsigned long)p[3]);
        op_send_line(c, buf);
        reported++;
        if (reported >= 256) break;
    }
    char summary[96];
    snprintf(summary, sizeof(summary),
             "count=%d mapped_pages_seen=%u",
             reported, (unsigned)mapped_pages_seen);
    op_send_line(c, summary);
    op_send_text_end(c);
    return 0;
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
    op_send_line(c, "unsafe.enable                         arm mem.write / nv2a.write / smc.write / smc.fan");
    op_send_line(c, "controller.set port=N [...]           update synthetic input state");
    op_send_line(c, "controller.button port=N name=X value=V edit one button by xemu name");
    op_send_line(c, "controller.axis   port=N name=X value=V edit one axis by xemu name");
    op_send_line(c, "controller.get [port=N]               read back synthetic input state");
    op_send_line(c, "controller.clear [port=N]             zero one or all ports");
    op_send_line(c, "controller.buffer-info                buffer addr/size for shim hooks");
    op_send_line(c, "witness.scan                          enumerate kseg0 oracle_ctrl_buffer instances + reserved[0,1] (cycle-23 A.4 readback)");
    op_send_line(c, "tier2.preflight                       read-only Tier-2 hook slot check");
    op_send_line(c, "tier2.install-jump-only confirm=...   install resident tail-jump hook (crashes)");
    op_send_line(c, "tier2.install-noop confirm=...        install resident no-op counter hook (crashes)");
    op_send_line(c, "tier2.uninstall                       restore Tier-2 hook slot (unsafe)");
    op_send_line(c, "tier2.status                          read Tier-2 hook page + counters");
    op_send_line(c, "smc.read off=0xHEX                    read one SMC register (allowlisted)");
    op_send_line(c, "smc.write off=0xHEX val=0xHEX         write SMC reg (needs unsafe.enable; allowlist 0x05,0x06)");
    op_send_line(c, "smc.temps                             cpu_c/board_c/avpack + last-set fan");
    op_send_line(c, "smc.fan val=auto|0-100                set fan curve floor (needs unsafe.enable)");
    op_send_line(c, "reboot                                reboot to dashboard");
    op_send_line(c, "bye                                   close connection");
    op_send_line(c, "help                                  this list");
    op_send_text_end(c);
    return 0;
}
