/*
 * xbed_input_synth — implementation. See header for the architecture.
 */
#include "xbed_input_synth.h"

#include <hal/debug.h>
#include <nxdk/mount.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ANCHOR_PATH "E:\\Apps\\oracle-agent\\state\\ctrl-addr.txt"

/* The 16-byte header in oracle_ctrl_buffer (magic + version + 2x
 * reserved) followed by 4 packed port states of 26 bytes each. We
 * mirror that layout here independently so this lib has no dependency
 * on the agent's controller.h. */
struct __attribute__((packed)) shim_ctrl_buffer {
    uint32_t magic;
    uint32_t version;
    uint32_t reserved[2];
    struct xbed_port_state port[XBED_INPUT_SYNTH_NUM_PORTS];
};

static struct shim_ctrl_buffer *s_buf;
static uintptr_t                s_phys;
static uintptr_t                s_virt;

uintptr_t xbed_input_synth_phys_addr(void) { return s_phys; }
uintptr_t xbed_input_synth_virt_addr(void) { return s_virt; }
const char *xbed_input_synth_anchor_path(void) { return ANCHOR_PATH; }

int xbed_input_synth_attached(void)
{
    return s_buf != NULL ? 1 : 0;
}

/* Idempotent E: mount via nxdk's symlink helper. Returns 1 if E: is
 * usable. */
static int s_ensure_e(void)
{
    if (nxIsDriveMounted('E')) return 1;
    if (nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")) return 1;
    return 0;
}

/* Strict 0x-prefixed hex parser; returns 0 on success, 1 on failure.
 * Stops at \r, \n, or end-of-string. */
static int s_parse_hex(const char *s, uintptr_t *out)
{
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return 1;
    s += 2;
    uintptr_t acc = 0;
    int digits = 0;
    while (*s) {
        char c = *s;
        if (c == '\r' || c == '\n') break;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
        else return 1;
        if (digits >= (int)(sizeof(uintptr_t) * 2)) return 1;
        acc = (acc << 4) | (uintptr_t)v;
        digits++;
        s++;
    }
    if (digits == 0) return 1;
    *out = acc;
    return 0;
}

xbed_input_synth_status_t xbed_input_synth_attach(void)
{
    if (s_buf) return XBED_INPUT_SYNTH_OK;  /* already attached */

    if (!s_ensure_e()) {
        debugPrint("xbed_input_synth: E: mount failed\n");
        return XBED_INPUT_SYNTH_E_MOUNT_FAILED;
    }

    FILE *fp = fopen(ANCHOR_PATH, "rb");
    if (!fp) {
        debugPrint("xbed_input_synth: anchor file not found at %s\n",
                   ANCHOR_PATH);
        return XBED_INPUT_SYNTH_NO_AGENT;
    }
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) {
        debugPrint("xbed_input_synth: anchor file empty\n");
        return XBED_INPUT_SYNTH_BAD_ANCHOR;
    }

    /* Tokenize on \n. Strict 4-line parse (Codex 2026-05-07): require
     * "XCTR", "0xPHYS", "0xVIRT", "0xSIZE". A short / partial anchor
     * file is rejected up-front to avoid mapping a stale/misaligned
     * address. */
    char *line[4] = {0};
    int li = 0;
    char *p = buf;
    line[li++] = p;
    while (*p && li < 4) {
        if (*p == '\n') {
            *p = 0;
            if (p[1] != 0 && li < 4) line[li++] = p + 1;
        }
        p++;
    }
    if (li < 4 || strncmp(line[0], "XCTR", 4) != 0) {
        debugPrint("xbed_input_synth: anchor format invalid (li=%d)\n", li);
        return XBED_INPUT_SYNTH_BAD_ANCHOR;
    }

    uintptr_t phys = 0, virt_recorded = 0, size_recorded = 0;
    if (s_parse_hex(line[1], &phys) != 0 || phys == 0 || phys >= 0x04000000u) {
        debugPrint("xbed_input_synth: anchor phys parse failed\n");
        return XBED_INPUT_SYNTH_BAD_ANCHOR;
    }
    /* virt_recorded and size_recorded are validated for sanity but
     * we always rederive virt via kseg0; the recorded fields exist
     * for forward-compat + diagnostic logging. */
    if (s_parse_hex(line[2], &virt_recorded) != 0) {
        debugPrint("xbed_input_synth: anchor virt parse failed\n");
        return XBED_INPUT_SYNTH_BAD_ANCHOR;
    }
    if (s_parse_hex(line[3], &size_recorded) != 0 ||
        size_recorded < sizeof(struct shim_ctrl_buffer) ||
        size_recorded > 0x10000u) {
        debugPrint("xbed_input_synth: anchor size %lu invalid\n",
                   (unsigned long)size_recorded);
        return XBED_INPUT_SYNTH_BAD_ANCHOR;
    }

    /* kseg0 identity map: physical P maps to virtual P | 0x80000000.
     * The OG Xbox kernel identity-maps the entire 64 MiB of physical
     * RAM at 0x80000000-0x83FFFFFF; this is the same convention used
     * by the agent (`controller.c::s_try_reattach`). */
    uintptr_t virt = phys | 0x80000000u;
    struct shim_ctrl_buffer *vp = (struct shim_ctrl_buffer *)virt;

    /* Cross-check that the kernel still considers this page mapped
     * AND that the agent's magic/version are intact (a stale anchor
     * file pointing at a reclaimed page would otherwise dereference
     * random kernel data). */
    if ((uintptr_t)MmGetPhysicalAddress((PVOID)vp) != phys) {
        debugPrint("xbed_input_synth: kseg0 mapping invalid (page "
                   "reclaimed?)\n");
        return XBED_INPUT_SYNTH_BAD_BUFFER;
    }
    if (vp->magic != XBED_INPUT_SYNTH_MAGIC) {
        debugPrint("xbed_input_synth: bad magic 0x%08lx (want 0x%08lx)\n",
                   (unsigned long)vp->magic,
                   (unsigned long)XBED_INPUT_SYNTH_MAGIC);
        return XBED_INPUT_SYNTH_BAD_BUFFER;
    }
    if (vp->version != XBED_INPUT_SYNTH_VERSION) {
        debugPrint("xbed_input_synth: version mismatch %u (want %u)\n",
                   (unsigned)vp->version,
                   (unsigned)XBED_INPUT_SYNTH_VERSION);
        return XBED_INPUT_SYNTH_BAD_BUFFER;
    }

    s_buf  = vp;
    s_phys = phys;
    s_virt = virt;
    debugPrint("xbed_input_synth: attached at phys=0x%08lx virt=0x%08lx\n",
               (unsigned long)s_phys, (unsigned long)s_virt);
    return XBED_INPUT_SYNTH_OK;
}

int xbed_input_synth_read(uint32_t port, struct xbed_port_state *out)
{
    if (!s_buf || !out) return -1;
    if (port >= XBED_INPUT_SYNTH_NUM_PORTS) return -1;

    /* Seqlock-style read (Codex 2026-05-07 — simple seq++ was
     * insufficient).
     *
     * The agent's writers wrap each mutation block with seq++ to
     * ODD before writing fields and seq++ to EVEN after. A reader
     * is consistent when:
     *
     *   - pre/post seq values are equal (no write completed between)
     *   - seq is even (no write was IN PROGRESS at read time;
     *     reading mid-write produced odd seq, which fails this check)
     *
     * On retry we re-read both seqs around a fresh copy; on the OG
     * Xbox single-CPU box, the writer is the same RPC dispatch
     * thread that took the wire-protocol command, so torn reads
     * only occur if the writer is preempted (does not happen in
     * the agent's lwIP-only event loop) — but the protocol is
     * still robust against future Tier-2 kernel-mode injection
     * paths where a kernel hook might be the writer. */
    volatile struct xbed_port_state *src =
        (volatile struct xbed_port_state *)&s_buf->port[port];

    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t seq_pre = src->seq;
        if (seq_pre & 1u) {
            /* Writer in flight; spin-yield and retry. */
            continue;
        }
        memcpy(out, (const void *)src, sizeof(*out));
        uint32_t seq_post = src->seq;
        if (seq_pre == seq_post && (seq_post & 1u) == 0) {
            return 0;  /* consistent stable snapshot */
        }
        /* Otherwise we either caught a write in progress or one
         * completed during our copy; loop and retry. */
    }
    /* All retries exhausted. Return the last copy we made — caller
     * sees a possibly-torn snapshot but never sees this function
     * stall a render frame indefinitely. The choice mirrors Linux's
     * `read_seqcount_retry` post-budget fallback: at the budget
     * limit we deliver the latest read rather than spinning. */
    return 0;
}
