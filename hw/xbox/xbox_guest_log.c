/*
 * Xbox guest-log host sink — minimal IO-port channel that forwards
 * bytes written by the guest to xemu stderr with an `xemu-guest-log:`
 * prefix. Renderer-agnostic; intended for Tier-2 diagnostic XBEs
 * (`docs/apple-silicon/diagnostic-xbe-plan.md`) whose per-cell oracle
 * verdicts otherwise live only in the on-screen framebuffer and
 * therefore depend on screenshot capture.
 *
 * Opt-in via env var `XEMU_GUEST_LOG=1`. Fixed IO port 0xE9 (the
 * Bochs/QEMU debugcon convention; Xbox does not use this port).
 * The port is intentionally fixed end-to-end: the guest-side helper
 * in `scripts/apple-silicon/xbe-tests/lib/xbed_runtime.c` writes to
 * a compile-time constant, so a runtime host-side override would
 * silently disconnect the channel. If you ever need to move it,
 * change both `XBOX_GUEST_LOG_IOPORT` here and `XBED_HOST_LOG_PORT`
 * in `lib/xbed_runtime.h`, and rebuild the XBE library.
 *
 * Behavior: byte writes accumulate into a per-instance line buffer.
 * Writing `'\n'` or `'\0'` flushes the buffer to stderr; the buffer
 * also auto-flushes when full (512 bytes). Reads return the iobase
 * (same convention as `hw/char/debugcon.c`) so the guest can confirm
 * the device is present.
 *
 * Added 2026-05-22 (cycle 15) to surface the `image-blit` XBE's
 * existing per-cell PASS|FAIL verdicts directly to xemu.log without
 * depending on GL screenshot capture.
 */

#include "qemu/osdep.h"
#include "qemu/typedefs.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "exec/hwaddr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declared in hw/xbox/xbox.h. Forward-declared here so this TU does
 * not need to drag in hw/i386/pc.h (which xbox.h transitively requires
 * for the XboxMachineState struct). */
void xbox_guest_log_init(void);

/* Fixed IO port; must match XBED_HOST_LOG_PORT in
 * scripts/apple-silicon/xbe-tests/lib/xbed_runtime.h. */
#define XBOX_GUEST_LOG_IOPORT         0xe9
#define XBOX_GUEST_LOG_LINE_BUF       512

typedef struct XboxGuestLogState {
    MemoryRegion io;
    char         buf[XBOX_GUEST_LOG_LINE_BUF];
    size_t       len;
    uint32_t     readback;
    uint64_t     bytes_written;
    uint64_t     lines_flushed;
} XboxGuestLogState;

static void xbox_guest_log_flush(XboxGuestLogState *s)
{
    if (s->len == 0) {
        return;
    }
    s->buf[s->len] = '\0';
    fprintf(stderr, "xemu-guest-log: %s\n", s->buf);
    fflush(stderr);
    s->len = 0;
    s->lines_flushed++;
}

static void xbox_guest_log_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    XboxGuestLogState *s = opaque;
    unsigned char ch = (unsigned char)(val & 0xff);

    s->bytes_written++;

    if (ch == '\n' || ch == '\0') {
        xbox_guest_log_flush(s);
        return;
    }

    /* Strip carriage returns; treat \r as a no-op so guest CRLF
     * sequences collapse to a single line. */
    if (ch == '\r') {
        return;
    }

    if (s->len >= XBOX_GUEST_LOG_LINE_BUF - 1) {
        xbox_guest_log_flush(s);
    }
    s->buf[s->len++] = (char)ch;
}

static uint64_t xbox_guest_log_read(void *opaque, hwaddr addr, unsigned size)
{
    XboxGuestLogState *s = opaque;
    return s->readback;
}

static const MemoryRegionOps xbox_guest_log_ops = {
    .read = xbox_guest_log_read,
    .write = xbox_guest_log_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void xbox_guest_log_init(void)
{
    static bool initialized;

    if (initialized) {
        return;
    }
    initialized = true;

    const char *enable_env = getenv("XEMU_GUEST_LOG");
    bool enabled = enable_env && enable_env[0] && strcmp(enable_env, "0") != 0;
    if (!enabled) {
        return;
    }

    XboxGuestLogState *s = g_new0(XboxGuestLogState, 1);
    s->readback = XBOX_GUEST_LOG_IOPORT & 0xff;
    memory_region_init_io(&s->io, NULL, &xbox_guest_log_ops, s,
                          "xbox-guest-log", 1);
    memory_region_add_subregion(get_system_io(),
                                XBOX_GUEST_LOG_IOPORT, &s->io);

    fprintf(stderr,
            "xemu-guest-log: enabled on IO port 0x%04x "
            "(fixed; guest-side helper writes the same port; "
            "lines flushed to stderr on '\\n')\n",
            XBOX_GUEST_LOG_IOPORT);
    fflush(stderr);
}
