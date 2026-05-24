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

/* Cycle 39 EEPROM scratchpad (2026-05-24).
 *
 * Companion read/reset surface for the cycle-39 self-witness
 * pre-allocation breadcrumb. See
 * `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.h` cycle-39
 * addendum for the full scratchpad contract.
 *
 * Offset 0xFF is the highest byte of the 256-byte Xbox EEPROM image
 * and is documented as part of the 0xC0..0xFF reserved/unused tail
 * (consistently zero on stock OEM consoles). The cycle-39
 * self-witness shim writes a single byte = 0xA0 | (stage & 0x0F)
 * there ONLY on the first call of `xbed_self_witness_fire`, AS THE
 * LAST INSTRUCTION before `MmAllocateContiguousMemoryEx`.
 *
 * Read-back: returns the single byte at offset 0xFF along with the
 * decoded stage nibble + the cycle-39 G-row sub-case interpretation.
 *
 * Reset: writes 0x00 to offset 0xFF (gated by `unsafe.enable`).
 * Hermes calls this BEFORE the cycle-39 real-Xbox run to establish
 * a known clean baseline.
 *
 * Why a dedicated verb when `cmd_eeprom` already dumps all 256 bytes:
 * (1) compact + scriptable single-byte readout (no hex-row parsing);
 * (2) explicit tag/sub-case interpretation in the agent response
 * line so Hermes does not have to duplicate the decode logic.
 * The full `eeprom` dump remains the authoritative reference (e.g.
 * for cross-checking that no other EEPROM byte mutated). */
#define EEPROM_SCRATCH_OFF       0xFFu
#define EEPROM_SCRATCH_TAG_NIB   0xA0u

int cmd_eeprom_scratch_read(struct netconn *c, const char *args)
{
    (void)args;
    ULONG val = 0;
    NTSTATUS s = HalReadSMBusValue(EEPROM_SMBUS_ADDR, EEPROM_SCRATCH_OFF,
                                   FALSE, &val);
    if (!NT_SUCCESS(s)) {
        op_send_errf(c, "HalReadSMBusValue failed status=0x%08lx",
                     (unsigned long)s);
        return 0;
    }
    unsigned byte = (unsigned)(val & 0xFFu);
    const char *interp;
    /* Sub-case interpretation matches xbed_self_witness.h cycle-39
     * discriminator table + witness-only/README.md cycle-39 addendum
     * G-row table. Stage nibble is the low 4 bits of the cycle-29
     * self-witness stage code passed to the first
     * `xbed_self_witness_fire` call this XBE made; on real-Xbox
     * witness-only the first call is the .CRT$XXC slot stage=4 fire
     * → byte=0xA4 is the ONLY value that maps to a defined G-row in
     * the cycle-40 readback. Other 0xA? values (0xA1/0xA3/0xA5 etc.)
     * are TAG-shaped but stage-nibble-mismatched — per the cycle-39
     * G-row table's "any other" row they are indeterminate and
     * should force a rerun with an explicit eeprom.scratch.reset
     * baseline. The readback verb must NOT alias them onto
     * sub-case (c). */
    if (byte == 0x00u) {
        interp = "cleared (cycle-39 baseline; sub-case (a)+(b) if "
                 "this is a POST-run reading)";
    } else if (byte == 0xA4u) {
        interp = "cycle-39 self-witness pre-MmAlloc breadcrumb "
                 "(sub-case (c) if WTNS count=0; G1..G4 if WTNS count>=1)";
    } else if ((byte & 0xF0u) == EEPROM_SCRATCH_TAG_NIB) {
        interp = "indeterminate (TAG nibble matches but stage_nib != 4; "
                 "rerun with eeprom.scratch.reset baseline)";
    } else {
        interp = "indeterminate (not a cycle-39 breadcrumb; possibly "
                 "stale pre-baseline value or foreign write — rerun with "
                 "eeprom.scratch.reset baseline)";
    }
    op_send_okf(c, "off=0x%02X byte=0x%02X tag=0x%01X stage_nib=0x%01X "
                   "interp=\"%s\"",
                (unsigned)EEPROM_SCRATCH_OFF,
                byte,
                (unsigned)((byte & 0xF0u) >> 4),
                (unsigned)(byte & 0x0Fu),
                interp);
    return 0;
}

int cmd_eeprom_scratch_reset(struct netconn *c, const char *args)
{
    (void)args;
    if (!s_unsafe_writes_enabled) {
        op_send_errf(c, "writes disabled — call unsafe.enable first");
        return 0;
    }
    NTSTATUS s = HalWriteSMBusValue(EEPROM_SMBUS_ADDR, EEPROM_SCRATCH_OFF,
                                    FALSE, 0x00u);
    if (!NT_SUCCESS(s)) {
        op_send_errf(c, "HalWriteSMBusValue failed status=0x%08lx",
                     (unsigned long)s);
        return 0;
    }
    op_send_okf(c, "off=0x%02X byte=0x00 (cycle-39 scratchpad cleared)",
                (unsigned)EEPROM_SCRATCH_OFF);
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

/* Cycle 29 option (c) — read-only enumerator for the self-allocated
 * witness pages stamped by `lib/xbed_self_witness.c::xbed_self_witness_fire`.
 *
 * Why a separate verb (not folded into cmd_witness_scan):
 * - The cycle-23 scan filter (`a4_reader_candidate_ok`) is keyed to
 *   the agent's XCTR magic. Cycle-29 self-witness pages use the
 *   distinct 'WTNS' magic so they neither inflate the XCTR count
 *   (preserves cycle-26/28 readback semantics) nor get rejected by
 *   the XCTR-only filter. Keeping the two verbs separate also lets
 *   future diag XBEs share the self-witness shim without polluting
 *   cycle-23 readback output.
 * - Read-only by design — same safety profile as cmd_witness_scan
 *   (MmGetPhysicalAddress gate per page, kseg0 scan range capped to
 *   the agent's 64 MiB-RAM allocation window, no writes).
 *
 * Cycle-30 expected output shapes (run AFTER the cycle-29 witness-only
 * chainload + relaunched cycle-27/29 oracle-agent):
 *
 *   count=1 reserved0=0xA4000003 reserved1=2
 *       → witness-only's main() ran AND both self-witness fires
 *         (MAIN_ENTERED stage=1 then POST_MARKER0 stage=3) landed
 *         AND the self-allocated persistent page is findable from a
 *         non-agent process context (the relaunched agent IS a new
 *         process from witness-only's perspective).
 *       → cause (γ) "witness-only never reaches main()" INVALIDATED.
 *       → causes (α) "agent XCTR buffer not findable from non-agent
 *         context" AND (β) "scan finds XCTR but write faults silently"
 *         BOTH REMAIN LIVE. Cycle-29 stamps a SELF-OWNED page, not
 *         the agent's XCTR page; the cycle-23 scan-from-non-agent-
 *         context path that targets the agent's XCTR buffer is NOT
 *         exercised by witness.scan-self, so this readback alone
 *         cannot distinguish α from β. The narrower (correct) claim
 *         is "main() reached the call sites." Codex round-1 high
 *         finding #2 (adopted). Cycle 31+ should pursue option (b)
 *         (agent-side prior-phys dump + read-only kseg0 dump verb)
 *         to break α-vs-β.
 *   count=1 reserved0=0xA4000001 reserved1=1
 *       → only the first self-witness fire landed; second fire
 *         perturbed CPU state or hung the box and a watchdog
 *         soft-reset eventually fired. Less likely; worth
 *         surfacing.
 *   count=0
 *       → No self-witness magic anywhere in kseg0.
 *       → cause (γ) leading; cycle 31+ should pursue option (d)
 *         (on-screen breadcrumb via debugPrint + pbkit-init OR
 *         a minimal NV097 single-poke) for an independent
 *         main()-runs verification.
 *   count>=2
 *       → Multiple self-witness allocations accumulated across
 *         repeated cycle-30+ chainloads within the same physical
 *         power session. The persistent contiguous-memory pool kept
 *         the older allocation(s) alive (each diag-XBE run leaks
 *         ONE persistent allocation until power-off, mirroring the
 *         agent's own leak pattern). Cycle-42A note: each
 *         allocation is now 2 pages (0x2000 B) instead of cycle-29..
 *         41e's 1 page (0x1000 B); `count` reported here still grows
 *         by exactly 1 per cycle-42A allocation (only the first page
 *         of each allocation carries the WTNS magic; the second
 *         page is zero-filled and fails the magic predicate at the
 *         consumer's next 0x1000-stride read). `mapped_pages_seen`
 *         in the summary line is a separate survey counter — it
 *         counts every kseg0 page in `[0x80010000, 0x84000000]` for
 *         which `MmGetPhysicalAddress` returns non-zero, NOT the
 *         number of WTNS allocations; on retail Original Xbox the
 *         kseg0 identity mapping for physical RAM is generally
 *         persistent across reboots, so `mapped_pages_seen` is
 *         typically stable across runs (cycles 41a..41e all reported
 *         419) and is not a load-bearing signal for the cycle-42A
 *         allocation-shape interpretation. Hermes should
 *         power-cycle the Xbox between attempts if precondition
 *         cleanliness is required.
 *
 * Filter parity (lockstep with the cycle-29 writer
 * `lib/xbed_self_witness.c`): magic == XBED_SELF_WITNESS_MAGIC,
 * version == XBED_SELF_WITNESS_VERSION, reserved0 is either 0
 * (allocated but not yet fired — should not occur because the
 * writer always fires immediately after allocation, but tolerate
 * it as a soft success) OR ((reserved0 >> 24) == 0xA4) AND
 * reserved1 in [1, 4096]. Tightened (cycle-27-style) version of the
 * cycle-23 filter, applied here because the writer is more
 * restricted than xbed_a4_witness (which had to tolerate arbitrary
 * pre-existing XCTR pages allocated by the agent).
 *
 * Cycle-42A producer-side change (2026-05-24; consumer note only,
 * NO code change required): the cycle-42A producer
 * (`lib/xbed_self_witness.c`) now requests a 0x2000-byte (two-page)
 * contiguous allocation instead of cycle-41e's 0x1000-byte (one-
 * page) request, as the branch-(c) "`size=0x1000`-specific
 * interaction" discriminator under the post-cycle-41 hypothesis-
 * narrowing ledger. The WTNS magic + version + reserved0 +
 * reserved1 header still lives ONLY at offset 0 of the FIRST page
 * of the multi-page allocation; the second page is zero-filled by
 * the producer (page-wipe loop now covers the full 0x2000 bytes).
 * This consumer's per-page 0x1000-stride scan therefore matches
 * the WTNS magic predicate on the first page only and reports
 * `count=1` per cycle-42A allocation — same count semantics as
 * cycles 29..41e. NO consumer code change is required. The
 * `SELF_WTNS_PAGE_STRIDE` (0x1000) is unchanged because the
 * producer's first-page placement is page-aligned and the
 * cycle-29 layout contract for stamp visibility is preserved.
 * Multiple cycle-42A allocations across re-runs within the same
 * physical power session would accumulate as multiple count-1
 * matches (one per allocation) — same accumulation pattern as
 * cycles 29..41e. */
/* Cycle-42H phys-range widening (2026-05-24; oracle-agent source
 * change; rule #15 re-triggers a full Codex pass).
 *
 * Motivation (direct response to cycle-42G real-Xbox outcome
 * `(eeprom.scratch.read=0xBC, witness.scan-self count=0,
 * mapped_pages_seen=419)`): the cycle-42F in-process readback at
 * milestone 10 observed the WTNS magic on the cached kseg0 alias
 * `(phys | 0x80000000u)` post-`wbinvd` and the subsequent 0xBC SMBus
 * marker write succeeded, BUT the agent's post-chainload
 * `witness.scan-self` still reports `count=0`. Cycle 42G PROVES the
 * bypass body + stamp-observability axis but leaves a residual
 * discoverability gap with three live sub-causes:
 *
 *   (1) The WTNS page lands at a physical address whose cached kseg0
 *       alias falls outside the cycle-29..42G scan window
 *       `[0x80010000, 0x84000000)`. The original lower bound
 *       `0x80010000` was a defensive skip of the bottom 64 KiB
 *       (rationale: kernel-pool allocations were not expected at
 *       phys < 0x10000); the upper bound `0x84000000` was a defensive
 *       skip of phys >= 64 MiB which the Original Xbox does not
 *       address. Variant (1) is unlikely for a normal
 *       `MmAllocateContiguousMemory(0x2000)` return (kernel pools grow
 *       downward from the top of RAM; cycle-23 reported phys=0x03eb3000
 *       D-cycle-28 for 30+ consecutive observations cycles 26..42G —
 *       well inside the existing window), but cycle 42H still drops
 *       the lower bound to `0x80000000` so [0, 0x10000) becomes
 *       observable rather than silently skipped. This is a tooling
 *       hardening, not a hypothesis-driven probe.
 *
 *   (2) The page lives inside the cached kseg0 window during the
 *       in-process readback but is torn down by the BIOS / kernel
 *       between the post-`runxbe` chainload return to dashboard and
 *       the agent's post-chainload `witness.scan-self` invocation.
 *       This is the leading sub-hypothesis under the cycle-42E
 *       discoverability framing — `MmPersistContiguousMemory`
 *       retention semantics for a pre-WinMain allocation may not
 *       extend across a chainload return on this iND-BiOS revision.
 *       Cycle-42H cannot prove this alone in a single subsequent
 *       real-Xbox run, but the per-window breakdown emitted by this
 *       widened verb lets the next slice's outcome shape distinguish
 *       (2) from (3) — see "Interpretation table" below.
 *
 *   (3) The page survives but is reachable only through the uncached
 *       kseg1 alias `(phys | 0xA0000000u)`, not through the cached
 *       kseg0 alias `(phys | 0x80000000u)`. The Original Xbox MIPS-
 *       style memory map exposes RAM through both aliases; the
 *       producer (`lib/xbed_self_witness.c`) intentionally writes
 *       through the cached kseg0 alias because it's the same alias a
 *       consumer would later walk for the typical "find a tagged
 *       page" flow. If for any reason this BIOS revision's
 *       `MmAllocateContiguousMemory(0x2000)` path leaves a survivor
 *       only at the uncached alias (e.g. via a write-combined buffer
 *       commit path that pins the uncached mirror but lets the cache
 *       line drop), the cycle-29..42G cached-only scan would miss it.
 *       Cycle 42H adds the symmetric kseg1 window
 *       `[0xA0000000, 0xA4000000)` so (3) becomes observable.
 *
 * Width contract (preserves cycle-22 lockout):
 *
 *   - kseg0 cached window:   [0x80000000, 0x84000000)  (lower bound
 *                            dropped from cycle-29..42G's 0x80010000
 *                            to expose the bottom 64 KiB)
 *   - kseg1 uncached window: [0xA0000000, 0xA4000000)  (NEW; same
 *                            64 MiB Original Xbox RAM range, uncached
 *                            mirror)
 *   - Stride unchanged:      0x1000 (page-aligned, matches producer's
 *                            page-aligned 0x2000 allocation; the WTNS
 *                            magic + version + reserved0 + reserved1
 *                            header lives at offset 0 of the FIRST
 *                            page only — cycle-42A producer-side
 *                            shape, unchanged)
 *   - Filter unchanged:      `self_wtns_reader_candidate_ok` —
 *                            cycle-23-style tight filter (magic +
 *                            version + reserved0 either 0 or A.4-
 *                            tagged + reserved1 in [1, 4096]). Both
 *                            windows apply the SAME filter so a
 *                            cache-line stale read at the kseg0 alias
 *                            wouldn't accidentally promote noise into
 *                            a false match.
 *   - Reporting cap:         total reported entries capped at 256
 *                            across BOTH windows, same as cycle-29..
 *                            42G. The first window scanned is kseg0
 *                            (the producer's canonical alias);
 *                            kseg1 is scanned second so a kseg0 match
 *                            is reported first when both aliases hit
 *                            for the same physical page (which is the
 *                            normal expected pattern if the page
 *                            survives at all).
 *
 * Interpretation table (cycle 42I or later — the next real-Xbox
 * slice that runs this widened agent post-chainload):
 *
 *   count=0 kseg0_count=0 kseg1_count=0
 *       → Sub-cause (2) "page torn down between chainload return
 *         and agent scan" is the leading remaining hypothesis on the
 *         discoverability axis. Variants (1) and (3) are RULED OUT
 *         on this run. Next: pursue option 3 of the cycle-42E menu
 *         (a marker emitted by xbed_self_witness immediately before
 *         and after the chainload syscall to bracket the teardown
 *         window) OR the cycle-42F alternative "EEPROM-stamped
 *         pre-teardown phys" so the agent has a phys to dereference
 *         instead of enumerate.
 *   count=N kseg0_count=N kseg1_count=0
 *       → Page survives + is reachable through the cached kseg0
 *         alias. If `N=1` with `reserved0=0xA4xxxxxx reserved1=1`,
 *         the cycle-42D bypass body's WTNS stamp survived the
 *         chainload AND the cycle-39 sticky-flag setter did NOT fire
 *         on a later stages-!=6 fire (consistent with the cycle-42D
 *         sticky-flag-first invariant). Cycle-22 axis fully
 *         CONFIRMED on the discoverability sub-axis; the cycle-29..
 *         42G "page not discoverable" framing was a phys-range
 *         enumeration gap, not a true survival gap.
 *   count=N kseg0_count=0 kseg1_count=N
 *       → Sub-cause (3) "page survives only at the uncached kseg1
 *         alias" CONFIRMED. The producer's cached-alias-only write
 *         visibility model needs a cycle-43x adjustment OR the
 *         agent's downstream readers need a kseg1 fallback. Less
 *         likely on a stable BIOS, but the per-window breakdown is
 *         what lets us see it.
 *   count=N kseg0_count=K0 kseg1_count=K1 with K0>0 AND K1>0
 *       → Page is mapped through BOTH aliases (the normal MIPS-
 *         style identity-mapped RAM case). Stamp survived; (3)
 *         RULED OUT; cycle-22 axis fully CONFIRMED. By construction
 *         `N == K0 + K1` (the reporter increments `reported` for
 *         every emitted hit on either alias, so the leading
 *         `count=` integer is the exact sum of the two per-window
 *         counts modulo the cap-hit case described under
 *         "Cap-truncation observability" below). The number of
 *         *distinct physical survivors* visible through the dual-
 *         alias scan can be smaller than `K0 + K1` because the
 *         reporter does NOT deduplicate by phys — the same
 *         physical page matched at both aliases counts twice
 *         (once per alias). That's deliberate so the operator sees
 *         the raw dual-alias visibility shape; the per-buf
 *         `phys=0x...` field is the disambiguator. Per-buf lines
 *         are emitted in scan order (kseg0 first, then kseg1).
 *
 * Cap-truncation observability:
 *
 *   The 256-entry global report cap (`SELF_WTNS_REPORT_CAP`) is
 *   shared across both windows so the response message stays
 *   bounded in the cycle-23 lwIP-netconn-on-coroutine context. If
 *   the kseg0 pass alone fills the cap, the kseg1 window is NOT
 *   scanned (a cap-overflow in kseg0 is itself a strong anomaly
 *   signal: cycle-29..42G observed at most `count=1` across many
 *   runs, so a kseg0 256-match flood is more interesting than a
 *   second window's noise). If the cap is reached during the
 *   kseg1 pass instead, that pass returns early with whatever it
 *   has accumulated so far. To prevent silent conflation of
 *   "no kseg1 evidence", "kseg1 was never scanned", and "kseg1
 *   was scanned but the cap was reached before the window
 *   completed", the summary line APPENDS:
 *
 *     truncated_at_cap=<0|1>   1 iff the global 256-entry cap was
 *                              reached during the kseg0 pass OR
 *                              the kseg1 pass (Codex cycle-42H R2
 *                              finding adopted — was previously
 *                              kseg0-only). Acts as a true global
 *                              truncation signal.
 *     kseg1_scanned=<0|1>      1 iff the kseg1 pass was executed;
 *                              0 iff the kseg1 pass was skipped
 *                              because the kseg0 pass exhausted
 *                              the cap. Combined with
 *                              `truncated_at_cap=`, the operator
 *                              can pinpoint which window saw the
 *                              truncation: kseg0 truncation maps
 *                              to (truncated_at_cap=1,
 *                              kseg1_scanned=0); kseg1 truncation
 *                              maps to (truncated_at_cap=1,
 *                              kseg1_scanned=1); no truncation
 *                              maps to (truncated_at_cap=0,
 *                              kseg1_scanned=1). The fourth shape
 *                              (truncated_at_cap=0,
 *                              kseg1_scanned=0) is unreachable by
 *                              construction.
 *
 *   These two booleans together exhaust the cap-truncation
 *   disambiguation space. Under normal operation (cycle-29..42G
 *   has only ever observed `count=0` or `count=1`)
 *   `truncated_at_cap=0 kseg1_scanned=1` is the expected shape and
 *   the existing `count=N` field is sufficient on its own.
 *
 * Scope-preservation contract:
 *
 *   - ZERO change to the cycle-23 `cmd_witness_scan` verb (XCTR
 *     scanner; cycle-23 lockstep contract intact).
 *   - ZERO change to the cycle-29 `self_wtns_reader_candidate_ok`
 *     filter (the producer's stamp shape and the consumer's match
 *     predicate stay in lockstep).
 *   - ZERO change to host xemu source, `lib/xbed_self_witness.{c,h}`,
 *     `lib/xbed_a4_witness.{c,h}`, `witness-only/`, or any nxdk
 *     source. Only `oracle-agent/commands.c` `cmd_witness_scan_self`
 *     enumeration body + its documentation block.
 *   - The existing `count=N mapped_pages_seen=M` summary contract is
 *     PRESERVED: the leading `count=` integer is the total reported
 *     across both windows (parsers keying on the first token stay
 *     happy); `mapped_pages_seen=` is the sum of kseg0 and kseg1
 *     mapped-page counts (an aggregate survey counter — same role as
 *     cycle-29..42G but now reflects both aliases). The new fields
 *     `kseg0_count=`, `kseg1_count=`, `kseg0_mapped=`,
 *     `kseg1_mapped=`, `truncated_at_cap=`, `kseg1_scanned=` are
 *     APPENDED to the same line so the runbook's `grep '^count='`
 *     invocations stay compatible. The cap-handling booleans
 *     `truncated_at_cap=` + `kseg1_scanned=` were added in response
 *     to Codex 2026-05-24 cycle-42H R1 medium finding (silent cap-
 *     truncation of kseg1 evidence would otherwise conflate "no
 *     kseg1 evidence" with "kseg1 was never scanned").
 *
 * Honest-framing what cycle-42H does NOT prove (per project rule #1
 * + #3):
 *
 *   - It does NOT itself run anything on real hardware. It only
 *     widens the observation aperture. The next bounded slice (cycle
 *     42I; Hermes's call) must deploy the widened agent + re-run the
 *     cycle-42G 18-step runbook to actually classify the
 *     discoverability question.
 *   - The kseg1 uncached read goes around the L1 + L2 caches; on
 *     consumer-style hardware this can be slower than a cached read,
 *     but the agent is not on a tight loop and the 4 KiB stride is
 *     identical to the existing cached scan. No performance
 *     regression expected; if the post-deployment readback adds
 *     measurable wall time, the cycle-42I runbook will report it.
 *   - The cycle-42H reporter does NOT deduplicate by phys (see
 *     "Interpretation table" K0>0 AND K1>0 row). An operator reading
 *     `count=2` with two `buf.*` lines that report the same
 *     `phys=0x...` field at different `alias=` values is looking at
 *     the SAME physical survivor seen through two aliases, not two
 *     distinct allocations. Cycle-22 lockout interpretation must
 *     account for this. */
#define SELF_WTNS_KSEG0_SCAN_START  0x80000000u
#define SELF_WTNS_KSEG0_SCAN_END    0x84000000u
#define SELF_WTNS_KSEG1_SCAN_START  0xA0000000u
#define SELF_WTNS_KSEG1_SCAN_END    0xA4000000u
#define SELF_WTNS_PAGE_STRIDE       0x1000u
#define SELF_WTNS_MAGIC             0x534E5457u  /* 'WTNS' little-endian */
#define SELF_WTNS_VERSION           1u
#define SELF_WTNS_TAG               0xA4u
#define SELF_WTNS_MAX_COUNTER       4096u
#define SELF_WTNS_REPORT_CAP        256

static int self_wtns_reader_candidate_ok(uintptr_t va)
{
    if ((uintptr_t)MmGetPhysicalAddress((PVOID)va) == 0u) return 0;
    volatile uint32_t *p = (volatile uint32_t *)va;
    if (p[0] != SELF_WTNS_MAGIC) return 0;
    if (p[1] != SELF_WTNS_VERSION) return 0;
    uint32_t r0 = p[2];
    uint32_t r1 = p[3];
    if (r0 == 0u && r1 == 0u) return 1;                 /* freshly allocated */
    if ((r0 >> 24) == SELF_WTNS_TAG &&
        r1 >= 1u && r1 <= SELF_WTNS_MAX_COUNTER) {
        return 1;                                       /* fired at least once */
    }
    return 0;
}

/* Cycle-42H helper — scan one alias window, append matches to the
 * netconn output, accumulate per-window counters. Caller owns the
 * running `reported` total (used for the 256-match global cap and
 * the `buf.<idx>` line-prefix index) so kseg0 and kseg1 share a
 * single reporter sequence. Returns 1 if the global cap was hit (so
 * the caller should NOT scan the next window), 0 otherwise. */
static int self_wtns_scan_window(struct netconn *c,
                                 uintptr_t va_start,
                                 uintptr_t va_end,
                                 const char *alias_tag,
                                 int *reported,
                                 int *window_count,
                                 uint32_t *window_mapped)
{
    *window_count = 0;
    *window_mapped = 0;
    for (uintptr_t va = va_start; va < va_end; va += SELF_WTNS_PAGE_STRIDE) {
        if ((uintptr_t)MmGetPhysicalAddress((PVOID)va) == 0u) continue;
        (*window_mapped)++;
        if (!self_wtns_reader_candidate_ok(va)) continue;

        volatile uint32_t *p = (volatile uint32_t *)va;
        char buf[224];
        uintptr_t phys = (uintptr_t)va & 0x03FFFFFFu;
        snprintf(buf, sizeof(buf),
                 "buf.%d alias=%s phys=0x%08lx virt=0x%08lx "
                 "reserved0=0x%08lx reserved1=0x%08lx",
                 *reported, alias_tag,
                 (unsigned long)phys, (unsigned long)va,
                 (unsigned long)p[2], (unsigned long)p[3]);
        op_send_line(c, buf);
        (*reported)++;
        (*window_count)++;
        if (*reported >= SELF_WTNS_REPORT_CAP) return 1;
    }
    return 0;
}

int cmd_witness_scan_self(struct netconn *c, const char *args)
{
    (void)args;
    op_send_text_begin(c, 0);
    int reported = 0;
    int kseg0_count = 0;
    int kseg1_count = 0;
    uint32_t kseg0_mapped = 0;
    uint32_t kseg1_mapped = 0;
    int kseg1_scanned = 0;

    /* Cached kseg0 first — producer's canonical alias. */
    int cap_hit = self_wtns_scan_window(c,
                                        SELF_WTNS_KSEG0_SCAN_START,
                                        SELF_WTNS_KSEG0_SCAN_END,
                                        "kseg0",
                                        &reported,
                                        &kseg0_count,
                                        &kseg0_mapped);
    /* Uncached kseg1 second — cycle-42H phys-range widening. Only
     * scan if the global 256-entry report cap has not yet been hit
     * during the kseg0 pass; otherwise we'd silently truncate the
     * kseg1 evidence without a chance to count its mapped pages. The
     * cap is generous (cycle-29..42G observed at most count=1 across
     * many runs); a kseg0-overflow into the cap is itself a strong
     * anomaly signal worth surfacing without a second window's
     * noise. The `truncated_at_cap=` / `kseg1_scanned=` fields in
     * the summary line below disambiguate the "no kseg1 evidence"
     * shape from the "kseg1 not scanned because cap-truncated"
     * shape (Codex 2026-05-24 cycle-42H R1 finding adopted).
     *
     * The `truncated_at_cap=` flag below is set iff EITHER window
     * hit the global cap during its pass — that's a true global
     * truncation signal (Codex cycle-42H R2 finding adopted). The
     * companion `kseg1_scanned=` boolean then tells the operator
     * which window the truncation occurred in: kseg0 truncation
     * implies kseg1 was not scanned at all (`kseg1_scanned=0`),
     * while a kseg1 truncation means kseg1 WAS scanned but the
     * 256-entry cap was reached before its window completed
     * (`kseg1_scanned=1`). Both cases together exhaust the "cap
     * was hit" disambiguation space. */
    int kseg1_cap_hit = 0;
    if (!cap_hit) {
        kseg1_cap_hit = self_wtns_scan_window(c,
                                              SELF_WTNS_KSEG1_SCAN_START,
                                              SELF_WTNS_KSEG1_SCAN_END,
                                              "kseg1",
                                              &reported,
                                              &kseg1_count,
                                              &kseg1_mapped);
        kseg1_scanned = 1;
    }

    int truncated_at_cap = (cap_hit || kseg1_cap_hit) ? 1 : 0;
    uint32_t mapped_pages_seen = kseg0_mapped + kseg1_mapped;
    char summary[256];
    snprintf(summary, sizeof(summary),
             "count=%d mapped_pages_seen=%u "
             "kseg0_count=%d kseg1_count=%d "
             "kseg0_mapped=%u kseg1_mapped=%u "
             "truncated_at_cap=%d kseg1_scanned=%d",
             reported, (unsigned)mapped_pages_seen,
             kseg0_count, kseg1_count,
             (unsigned)kseg0_mapped, (unsigned)kseg1_mapped,
             truncated_at_cap, kseg1_scanned);
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
    op_send_line(c, "eeprom.scratch.read                   read cycle-39 EEPROM scratchpad byte at off=0xFF + sub-case interp");
    op_send_line(c, "eeprom.scratch.reset                  clear cycle-39 EEPROM scratchpad (needs unsafe.enable; writes 0x00 at off=0xFF)");
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
    op_send_line(c, "witness.scan-self                     enumerate kseg0+kseg1 xbed_self_witness 'WTNS' pages + reserved[0,1] (cycle-29 option (c) readback; cycle-42H phys-range widening — emits per-buf alias=kseg0|kseg1 + kseg0_count/kseg1_count/kseg0_mapped/kseg1_mapped/truncated_at_cap/kseg1_scanned summary fields)");
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
