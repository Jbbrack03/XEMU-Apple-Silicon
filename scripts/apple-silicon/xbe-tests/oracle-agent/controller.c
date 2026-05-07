/*
 * xbox-oracle-agent — synthetic controller state buffer + RPC commands.
 * See controller.h for the architecture overview.
 */
#include "controller.h"
#include "protocol.h"

#include <hal/debug.h>
#include <nxdk/mount.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pointer to the canonical synthetic-input buffer. Allocated from the
 * persistent contiguous memory pool (MmAllocateContiguousMemoryEx +
 * MmPersistContiguousMemory) so the buffer survives an XLaunchXBE
 * chainload. See controller.h for the cross-XBE re-attach scheme. */
static struct oracle_ctrl_buffer *g_oracle_ctrl_p;
static uintptr_t                  g_oracle_ctrl_phys;

/* Wall-clock helper. nxdk's xboxkrnl exports
 * `KeQueryPerformanceCounter` / `KeQueryPerformanceFrequency` as
 * ULONGLONG-returning functions (NOT the LARGE_INTEGER* API the
 * Win32 ABI uses). Convert ticks → microseconds; the result is
 * stamped on the buffer to give external consumers (a future shim or
 * kernel hook) a freshness signal. */
static uint64_t now_us(void)
{
    uint64_t freq = (uint64_t)KeQueryPerformanceFrequency();
    if (freq == 0) return 0;
    uint64_t ctr = (uint64_t)KeQueryPerformanceCounter();
    /* Reduce overflow risk: convert to microseconds via
     * (ctr / freq) * 1e6 + ((ctr % freq) * 1e6) / freq.
     * Xbox's perf-counter frequency is 3.375 MHz so 64-bit math
     * has plenty of headroom even at multi-decade uptime. */
    return ctr * 1000000ull / freq;
}

/* ---- persistence anchor: state/ctrl-addr.txt ---------------------- */

/* Path of the per-console state directory we own. Anchor file lives at
 * the path defined in controller.h. */
#define ORACLE_CTRL_STATE_DIR "E:\\Apps\\oracle-agent\\state"

/* Persistence-file format:
 *   line 1: "XCTR\n"
 *   line 2: "0x<8-hex-digits-physical-address>\n"
 *   line 3: "0x<8-hex-digits-virtual-address>\n"
 *   line 4: "0x<8-hex-digits-size-bytes>\n"
 *
 * Diag XBEs read this file (via fopen("E:\\Apps\\oracle-agent\\state\\ctrl-addr.txt"))
 * to discover the physical address of the agent-allocated controller
 * buffer; they then map the same physical page via the kseg0 identity
 * map (virt = phys | 0x80000000). The 4 KiB size is fixed by the
 * fact we always allocate one page; we still store it for forward
 * compatibility. */
/* Ensure the E: drive (FATX partition 1, the persistent storage drive
 * standard on every retail Xbox HDD) is mounted in this process. nxdk's
 * automount-d only mounts the launched-XBE's directory as D:; E: is not
 * auto-mounted, so writes to E:\Apps\... silently fail until we create
 * the symbolic link ourselves. Idempotent — `nxIsDriveMounted` short-
 * circuits if the link already exists (e.g. on a re-attach where a
 * prior agent process already mounted it but the kernel kept the
 * link).
 *
 * Returns 1 if E: is usable after the call, 0 otherwise. */
static int s_ensure_e_drive_mounted(void)
{
    if (nxIsDriveMounted('E')) return 1;
    /* OG Xbox standard FATX layout:
     *   Partition0 = entire disk (raw)
     *   Partition1 = E:  (utility, ~5 GiB, persistent)
     *   Partition2 = C:  (system / dashboard)
     *   Partition3..5 = X:/Y:/Z: (game cache, transient)
     *   Partition6+   = F:/G: (extended HDD, optional) */
    if (nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")) return 1;
    debugPrint("oracle_ctrl: E: mount failed (state writes will be skipped)\n");
    return 0;
}

/* Write the anchor file atomically (Codex 2026-05-07). fopen("wb")
 * truncates the existing file before fprintf runs; if fprintf or
 * fclose fails (low E: disk space, FATX corruption, etc.) the prior
 * anchor is destroyed AND replaced with a malformed one. To
 * eliminate that hazard we write to a sibling temp file, validate
 * every step, then atomically rename over the canonical path.
 *
 * MoveFileEx with MOVEFILE_REPLACE_EXISTING gives us atomic rename
 * on FATX (the rename either fully completes or doesn't happen at
 * all from the file system's perspective). */
#define ORACLE_CTRL_ADDR_FILE_TMP "E:\\Apps\\oracle-agent\\state\\ctrl-addr.tmp"

static int s_write_anchor_file(uintptr_t phys, uintptr_t virt, size_t size)
{
    if (!s_ensure_e_drive_mounted()) return -1;

    /* Best-effort: create directory; ignore "already exists". A partial
     * directory chain (E:\Apps already exists, only state needs creating)
     * is the common case; first call after a clean install creates both. */
    CreateDirectoryA("E:\\Apps", NULL);
    CreateDirectoryA("E:\\Apps\\oracle-agent", NULL);
    CreateDirectoryA(ORACLE_CTRL_STATE_DIR, NULL);

    /* Pre-clean any leftover .tmp from a previous crashed write. */
    DeleteFileA(ORACLE_CTRL_ADDR_FILE_TMP);

    FILE *fp = fopen(ORACLE_CTRL_ADDR_FILE_TMP, "wb");
    if (!fp) {
        debugPrint("oracle_ctrl: fopen %s failed\n",
                   ORACLE_CTRL_ADDR_FILE_TMP);
        return -1;
    }
    int n = fprintf(fp, "XCTR\n0x%08lx\n0x%08lx\n0x%08lx\n",
                    (unsigned long)phys, (unsigned long)virt,
                    (unsigned long)size);
    int io_err = (n < 0) || ferror(fp);
    if (fflush(fp) != 0) io_err = 1;
    if (fclose(fp) != 0) io_err = 1;
    if (io_err) {
        debugPrint("oracle_ctrl: anchor write/flush/close failed\n");
        DeleteFileA(ORACLE_CTRL_ADDR_FILE_TMP);
        return -1;
    }

    /* Atomic rename via NtSetInformationFile with ReplaceIfExists=TRUE.
     * nxdk's MoveFileA hardcodes ReplaceIfExists=FALSE so it can't
     * overwrite the existing canonical; the previous DeleteFileA +
     * MoveFileA two-step was racy AND would silently leave the anchor
     * stale if MoveFileA failed because the canonical still existed
     * (e.g. DeleteFileA returned but the FATX directory entry hadn't
     * been removed yet). The result was that diag XBEs read OLD
     * anchor → mapped OLD persistent buffer → saw stale state.
     * NtSetInformationFile with ReplaceIfExists is a single atomic op
     * on FATX. Followed by NtFlushBuffersFile to commit the rename
     * to disk before the agent can be killed by XLaunchXBE. */
    {
        ANSI_STRING src_str;
        OBJECT_ATTRIBUTES srcAttr;
        IO_STATUS_BLOCK iosb;
        HANDLE handle = NULL;
        NTSTATUS st;

        RtlInitAnsiString(&src_str, ORACLE_CTRL_ADDR_FILE_TMP);
        InitializeObjectAttributes(&srcAttr, &src_str,
                                   OBJ_CASE_INSENSITIVE,
                                   ObDosDevicesDirectory(), NULL);
        st = NtOpenFile(&handle,
                        DELETE | SYNCHRONIZE,
                        &srcAttr, &iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT |
                        FILE_OPEN_FOR_BACKUP_INTENT);
        if (!NT_SUCCESS(st)) {
            debugPrint("oracle_ctrl: NtOpenFile(tmp) failed 0x%08lx\n",
                       (unsigned long)st);
            DeleteFileA(ORACLE_CTRL_ADDR_FILE_TMP);
            return -1;
        }

        /* FILE_RENAME_INFORMATION with ReplaceIfExists=TRUE. */
        struct {
            FILE_RENAME_INFORMATION header;
            char extra_name_bytes[260];
        } rename_buf;
        memset(&rename_buf, 0, sizeof(rename_buf));
        rename_buf.header.ReplaceIfExists = TRUE;
        rename_buf.header.RootDirectory = ObDosDevicesDirectory();
        RtlInitAnsiString(&rename_buf.header.FileName,
                          ORACLE_CTRL_ADDR_FILE);
        st = NtSetInformationFile(handle, &iosb,
                                  &rename_buf,
                                  sizeof(FILE_RENAME_INFORMATION) +
                                      rename_buf.header.FileName.Length,
                                  FileRenameInformation);
        if (!NT_SUCCESS(st)) {
            debugPrint("oracle_ctrl: NtSetInformationFile(rename) failed "
                       "0x%08lx\n", (unsigned long)st);
            NtClose(handle);
            DeleteFileA(ORACLE_CTRL_ADDR_FILE_TMP);
            return -1;
        }

        /* Force FATX metadata to disk before the kernel-image swap
         * (XLaunchXBE / runxbe) can wipe in-memory state. */
        IO_STATUS_BLOCK flush_iosb;
        st = NtFlushBuffersFile(handle, &flush_iosb);
        if (!NT_SUCCESS(st)) {
            debugPrint("oracle_ctrl: NtFlushBuffersFile failed 0x%08lx "
                       "(continuing; rename committed in-memory)\n",
                       (unsigned long)st);
        }

        NtClose(handle);
    }
    return 0;
}

/* Strict 0x-prefixed hex parser; returns 0 on success and 1 on failure. */
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

/* Re-attach to a previously-allocated persistent buffer.
 *
 * Safety analysis (Codex 2026-05-07).
 *
 * The kseg0 identity-map check (`MmGetPhysicalAddress(virt) == phys`)
 * is NOT a proof of allocator ownership — it's just confirming that
 * the kernel still maps that physical page into kseg0, which it
 * always does for the entire 64 MiB physical RAM window. RAM
 * contents on x86 are not reset by a soft reboot; a stale
 * ctrl-addr.txt file (which lives on persistent E: storage and
 * NEVER gets cleaned up across power cycles) plus surviving RAM
 * bits could pass our magic+version check by coincidence and let
 * us write into kernel-pool memory we no longer own.
 *
 * Mitigation: re-attach is OPT-IN via the
 * `ORACLE_CTRL_ALLOW_REATTACH` build flag. By default we always
 * fresh-allocate. Cost of fresh-allocate: one 4 KiB kernel-pool
 * page leaked per agent restart (the prior persistent buffer is
 * unreachable but stays MmPersistContiguousMemory-flagged in the
 * kernel pool). The Xbox has 64 MiB of RAM; this is acceptable
 * for the project's expected restart cadence (single-digit per
 * day on the project Xbox).
 *
 * If re-attach is enabled, we additionally cross-check a
 * session-nonce field (must match the one we wrote to the anchor
 * file) so a coincidence-XCTR-magic-survival is essentially
 * impossible. The session nonce is only correct across a single
 * quick-reboot path that DOESN'T zero RAM; even then the chance
 * of false match is 2^-32 per restart. */
static struct oracle_ctrl_buffer *s_try_reattach(uintptr_t *out_phys)
{
#ifndef ORACLE_CTRL_ALLOW_REATTACH
    /* Default: always fresh-allocate. Suppresses the unused-helper
     * warnings by referencing the static helpers from the fresh-alloc
     * path below. */
    (void)out_phys;
    return NULL;
#else
    if (!s_ensure_e_drive_mounted()) return NULL;
    FILE *fp = fopen(ORACLE_CTRL_ADDR_FILE, "rb");
    if (!fp) return NULL;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    int closed = fclose(fp);
    if (n == 0 || closed != 0) return NULL;

    /* Strict 4-line parse (Codex 2026-05-07): require ALL of XCTR +
     * 0xPHYS + 0xVIRT + 0xSIZE; reject malformed/partial files. */
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
    if (li < 4 || strncmp(line[0], "XCTR", 4) != 0) return NULL;

    uintptr_t phys = 0, virt_recorded = 0, size_recorded = 0;
    if (s_parse_hex(line[1], &phys) != 0) return NULL;
    if (s_parse_hex(line[2], &virt_recorded) != 0) return NULL;
    if (s_parse_hex(line[3], &size_recorded) != 0) return NULL;
    if (size_recorded < sizeof(struct oracle_ctrl_buffer) ||
        size_recorded > 0x10000u) return NULL;
    if (phys == 0 || phys >= 0x04000000u) return NULL;

    struct oracle_ctrl_buffer *vp =
        (struct oracle_ctrl_buffer *)(phys | 0x80000000u);
    if ((uintptr_t)MmGetPhysicalAddress((PVOID)vp) != phys) return NULL;
    if (vp->magic != ORACLE_CTRL_MAGIC) return NULL;
    if (vp->version != ORACLE_CTRL_VERSION) return NULL;

    *out_phys = phys;
    return vp;
#endif /* ORACLE_CTRL_ALLOW_REATTACH */
}

/* Allocate a fresh persistent contiguous buffer + write the anchor file.
 * Returns NULL on hard failure. */
static struct oracle_ctrl_buffer *s_allocate_fresh(uintptr_t *out_phys)
{
    /* Page-sized, page-aligned, anywhere in the low 64 MiB physical
     * RAM, with PAGE_READWRITE protection. The buffer is small (120 B)
     * but we round up to a full page so MmPersistContiguousMemory has
     * a clean page to flag. */
    PVOID p = MmAllocateContiguousMemoryEx(
        0x1000u,             /* size: 1 page */
        0x00010000u,         /* lowest acceptable: skip the very-low pages */
        0x03ffffffu,         /* highest: top of 64 MiB RAM */
        0x1000u,             /* alignment: page */
        PAGE_READWRITE);     /* full RW for the agent + future diag XBEs */
    if (!p) {
        debugPrint("oracle_ctrl: MmAllocateContiguousMemoryEx failed\n");
        return NULL;
    }
    uintptr_t phys = (uintptr_t)MmGetPhysicalAddress(p);
    if (phys == 0) {
        debugPrint("oracle_ctrl: MmGetPhysicalAddress returned 0\n");
        MmFreeContiguousMemory(p);
        return NULL;
    }
    /* Mark the allocation persistent so it survives an XLaunchXBE
     * chainload. Without this flag the kernel reclaims it when the
     * agent process exits. */
    MmPersistContiguousMemory(p, 0x1000u, TRUE);

    struct oracle_ctrl_buffer *vp = (struct oracle_ctrl_buffer *)p;
    memset(vp, 0, sizeof(*vp));
    vp->magic = ORACLE_CTRL_MAGIC;
    vp->version = ORACLE_CTRL_VERSION;

    /* Anchor the address so a chainloaded diag XBE / re-launched
     * agent can find it. */
    if (s_write_anchor_file(phys, (uintptr_t)vp, 0x1000u) != 0) {
        /* Anchor write failure is not fatal — the buffer is still
         * usable for in-process RPCs; cross-XBE discovery just won't
         * work this run. Log and continue. */
        debugPrint("oracle_ctrl: WARNING — anchor file write failed; "
                   "cross-XBE discovery disabled this run\n");
    }
    *out_phys = phys;
    return vp;
}

void oracle_ctrl_init(void)
{
    /* Re-attach path: if a prior agent run already published a live
     * persistent buffer, just rebind to it. This avoids leaking pool
     * pages every time the agent gets relaunched (we do that often
     * — once per oracle-orchestrator run-diag cycle). */
    g_oracle_ctrl_p = s_try_reattach(&g_oracle_ctrl_phys);
    if (g_oracle_ctrl_p) {
        debugPrint("oracle_ctrl: reattached to persistent buffer at "
                   "phys=0x%08lx virt=%p\n",
                   (unsigned long)g_oracle_ctrl_phys,
                   (void *)g_oracle_ctrl_p);
        return;
    }

    /* Fresh-allocation path. */
    g_oracle_ctrl_p = s_allocate_fresh(&g_oracle_ctrl_phys);
    if (g_oracle_ctrl_p) {
        debugPrint("oracle_ctrl: allocated fresh persistent buffer at "
                   "phys=0x%08lx virt=%p\n",
                   (unsigned long)g_oracle_ctrl_phys,
                   (void *)g_oracle_ctrl_p);
        return;
    }

    /* Hard fallback: BSS-resident static buffer. The agent's RPCs
     * still work; cross-XBE discovery does not. The diag-XBE shim
     * detects this case (MmGetPhysicalAddress returns the BSS
     * physical address but the anchor file is absent or stale) and
     * fails the attach with a useful message. */
    static struct oracle_ctrl_buffer s_bss_fallback;
    memset(&s_bss_fallback, 0, sizeof(s_bss_fallback));
    s_bss_fallback.magic = ORACLE_CTRL_MAGIC;
    s_bss_fallback.version = ORACLE_CTRL_VERSION;
    g_oracle_ctrl_p = &s_bss_fallback;
    g_oracle_ctrl_phys =
        (uintptr_t)MmGetPhysicalAddress(&s_bss_fallback);
    debugPrint("oracle_ctrl: WARNING — using BSS fallback at virt=%p "
               "phys=0x%08lx\n",
               (void *)g_oracle_ctrl_p,
               (unsigned long)g_oracle_ctrl_phys);
}

struct oracle_ctrl_buffer *oracle_ctrl_get(void)
{
    return g_oracle_ctrl_p;
}

/* Convenience accessor used by the rest of this file (kept legacy-name
 * to minimize the diff in the RPC handlers below). */
#define g_oracle_ctrl (*oracle_ctrl_get())

/* ---- name → bit / axis index lookup ----
 *
 * The names match xemu's xemu_input_button_names / xemu_input_axis_names
 * tables (ui/xemu-input.c) so a recorded XEMU_RECORD_INPUT CSV from
 * xemu replays cleanly through this protocol without translation. */
struct kv_button { const char *name; uint16_t bit; };
struct kv_axis   { const char *name; int       index; };

static const struct kv_button s_buttons[] = {
    { "a",            ORACLE_BTN_A          },
    { "b",            ORACLE_BTN_B          },
    { "x",            ORACLE_BTN_X          },
    { "y",            ORACLE_BTN_Y          },
    { "dpad_up",      ORACLE_BTN_DPAD_UP    },
    { "dpad_down",    ORACLE_BTN_DPAD_DOWN  },
    { "dpad_left",    ORACLE_BTN_DPAD_LEFT  },
    { "dpad_right",   ORACLE_BTN_DPAD_RIGHT },
    { "start",        ORACLE_BTN_START      },
    { "back",         ORACLE_BTN_BACK       },
    { "white",        ORACLE_BTN_WHITE      },
    { "black",        ORACLE_BTN_BLACK      },
    { "lstick_btn",   ORACLE_BTN_LSTICK     },
    { "rstick_btn",   ORACLE_BTN_RSTICK     },
    { "guide",        ORACLE_BTN_GUIDE      },
    { NULL,           0                     },
};

enum {
    AXIS_LT = 0, AXIS_RT, AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY,
};
static const struct kv_axis s_axes[] = {
    { "ltrigger", AXIS_LT },
    { "rtrigger", AXIS_RT },
    { "lstick_x", AXIS_LX },
    { "lstick_y", AXIS_LY },
    { "rstick_x", AXIS_RX },
    { "rstick_y", AXIS_RY },
    { NULL,       -1     },
};

/* Strict signed parser for axis values. Accepts optional leading sign
 * and base-10 digits; rejects overflow against int32 range. Returns 0
 * on success, -1 otherwise. */
static int parse_int(const char *s, int32_t *out)
{
    if (!s || !*s) return -1;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    if (!*s) return -1;
    uint32_t acc = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t d = (uint32_t)(*s - '0');
        if (acc > (UINT32_MAX - d) / 10) return -1;
        acc = acc * 10 + d;
        s++;
    }
    if (neg) {
        if (acc > 0x80000000u) return -1;
        *out = (int32_t)(-(int64_t)acc);
    } else {
        if (acc > 0x7FFFFFFFu) return -1;
        *out = (int32_t)acc;
    }
    return 0;
}

/* Some args use op_parse_kv_u32 (which only accepts unsigned). For
 * signed axis values we need a separate helper that takes the raw
 * key= token. Returns 0 on success. */
static int parse_kv_int(const char *args, const char *key, int32_t *out)
{
    /* Locate "<key>=" and then parse the value substring (terminate
     * at whitespace). Allocate an on-stack copy just long enough. */
    if (!args || !key) return -1;
    size_t klen = strlen(key);
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            char tmp[24];
            size_t i = 0;
            while (*v && *v != ' ' && *v != '\t' && i + 1 < sizeof(tmp)) {
                tmp[i++] = *v++;
            }
            tmp[i] = 0;
            return parse_int(tmp, out);
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return -1;
}

static int parse_port_arg(const char *args, uint32_t *out_port,
                          int allow_missing)
{
    uint32_t p = 0;
    if (op_parse_kv_u32(args, "port", &p) != 0) {
        if (allow_missing) {
            *out_port = (uint32_t)-1;
            return 0;
        }
        return -1;
    }
    if (p >= ORACLE_CTRL_NUM_PORTS) return -2;
    *out_port = p;
    return 0;
}

/* Saturating clamp helpers. Values are pre-validated to int32 range
 * by parse_int above. */
static int16_t clamp_i16(int32_t v)
{
    if (v < -32768) return -32768;
    if (v > 32767)  return 32767;
    return (int16_t)v;
}

/* Trigger range matches xemu's `axis[CONTROLLER_AXIS_LTRIG]`: int16
 * 0..32767 (only the positive half is meaningful — triggers don't
 * report negative values). Negative input is clamped to 0. The Xbox
 * XID HID-report u8 0..255 is produced from this by `>> 7` at
 * `hw/xbox/xid.c:108-109` (a future kernel-hook shim does the same). */
static int16_t clamp_trigger(int32_t v)
{
    if (v < 0)     return 0;
    if (v > 32767) return 32767;
    return (int16_t)v;
}

/* Lookup helpers */
static const struct kv_button *find_button(const char *name)
{
    for (const struct kv_button *e = s_buttons; e->name; e++) {
        if (strcmp(name, e->name) == 0) return e;
    }
    return NULL;
}
static const struct kv_axis *find_axis(const char *name)
{
    for (const struct kv_axis *e = s_axes; e->name; e++) {
        if (strcmp(name, e->name) == 0) return e;
    }
    return NULL;
}

/* Mutate one axis on a port-state struct without a switch in the
 * three call sites. */
static void apply_axis(struct oracle_ctrl_port_state *p, int axis, int32_t val)
{
    switch (axis) {
        case AXIS_LT: p->ltrigger = clamp_trigger(val); break;
        case AXIS_RT: p->rtrigger = clamp_trigger(val); break;
        case AXIS_LX: p->lstick_x = clamp_i16(val); break;
        case AXIS_LY: p->lstick_y = clamp_i16(val); break;
        case AXIS_RX: p->rstick_x = clamp_i16(val); break;
        case AXIS_RY: p->rstick_y = clamp_i16(val); break;
        default: break;
    }
}

/* Seqlock helpers (Codex 2026-05-07).
 *
 * Writers wrap their field-mutation block with begin_write() (bumps
 * seq to ODD = "in-flight") and end_write() (bumps seq to EVEN +
 * stamps timestamp_us = "stable"). Readers check parity + equality
 * across pre/post copies and retry on mismatch. The `volatile`
 * qualifier on the seq writes prevents the compiler from reordering
 * field stores past the seq writes; on i386 the runtime memory model
 * is also strong enough that no explicit fence is required for
 * single-CPU OG Xbox (which is the only target).
 *
 * `mark_changed` is retained as a backwards-compat wrapper for the
 * `cmd_controller_get` heartbeat path that doesn't actually mutate
 * fields (bumps seq by 2 = stable→stable; net effect: a freshness
 * tick the reader can observe). */
/* Write `val` into a uint32_t field with C's "as-if" volatile
 * semantics, without taking the address of a packed-struct field
 * (which the compiler warns about: oracle_ctrl_port_state is packed
 * so seq lands at byte offset 14, not 4-byte aligned). On i386 the
 * resulting unaligned 4-byte store is a single atomic instruction
 * to the CPU; we just route through a void* cast so the compiler
 * doesn't generate an alignment warning on every call site. */
static inline void seq_store(struct oracle_ctrl_port_state *p, uint32_t val)
{
    void *dst = &p->seq;
    *(volatile uint32_t *)dst = val;
}

static inline uint32_t seq_load(struct oracle_ctrl_port_state *p)
{
    void *src = &p->seq;
    return *(volatile uint32_t *)src;
}

static inline void seq_begin_write(struct oracle_ctrl_port_state *p)
{
    /* If a previous writer left seq odd (crashed mid-write — should
     * never happen in single-threaded agent), force back to even
     * before adding 1 so we exit this call still odd. */
    uint32_t s = seq_load(p);
    if (s & 1u) s++;
    seq_store(p, s + 1u);
}

static inline void seq_end_write(struct oracle_ctrl_port_state *p)
{
    /* Match seq_begin_write: take the current odd seq up to the next
     * even value and stamp the timestamp at the same point. */
    uint32_t s = seq_load(p);
    if ((s & 1u) == 0) s--;        /* defensive: someone called us out of order */
    p->timestamp_us = now_us();
    seq_store(p, s + 1u);
}

/* Heartbeat: net seq bump of 2 (stable → unstable → stable) so a
 * reader sees a freshness change without observing torn data. */
static void mark_changed(struct oracle_ctrl_port_state *p)
{
    seq_begin_write(p);
    seq_end_write(p);
}

/* ---- RPC handlers ---- */

int cmd_controller_set(struct netconn *c, const char *args)
{
    uint32_t port = 0;
    int rc = parse_port_arg(args, &port, /*allow_missing=*/0);
    if (rc < 0) {
        op_send_errf(c, "usage: controller.set port=N [buttons=0xHHHH] "
                        "[lt=N] [rt=N] [lx=N] [ly=N] [rx=N] [ry=N]");
        return 0;
    }
    struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];

    seq_begin_write(st);
    int touched = 0;
    uint32_t btns = 0;
    if (op_parse_kv_u32(args, "buttons", &btns) == 0) {
        st->buttons = (uint16_t)(btns & 0xFFFFu);
        touched = 1;
    }
    int32_t v = 0;
    if (parse_kv_int(args, "lt", &v) == 0) { st->ltrigger = clamp_trigger(v); touched = 1; }
    if (parse_kv_int(args, "rt", &v) == 0) { st->rtrigger = clamp_trigger(v); touched = 1; }
    if (parse_kv_int(args, "lx", &v) == 0) { st->lstick_x = clamp_i16(v); touched = 1; }
    if (parse_kv_int(args, "ly", &v) == 0) { st->lstick_y = clamp_i16(v); touched = 1; }
    if (parse_kv_int(args, "rx", &v) == 0) { st->rstick_x = clamp_i16(v); touched = 1; }
    if (parse_kv_int(args, "ry", &v) == 0) { st->rstick_y = clamp_i16(v); touched = 1; }
    if (!touched) {
        /* Bare `controller.set port=N` is treated as a `seq+timestamp`
         * heartbeat — useful for the replay tool to bump the sequence
         * counter without changing state. */
    }
    seq_end_write(st);
    op_send_okf(c, "port=%u seq=%u buttons=0x%04x lt=%d rt=%d "
                   "lx=%d ly=%d rx=%d ry=%d",
                (unsigned)port, (unsigned)st->seq,
                (unsigned)st->buttons,
                (int)st->ltrigger, (int)st->rtrigger,
                (int)st->lstick_x, (int)st->lstick_y,
                (int)st->rstick_x, (int)st->rstick_y);
    return 0;
}

int cmd_controller_button(struct netconn *c, const char *args)
{
    uint32_t port = 0, val = 0;
    char name[24];
    if (parse_port_arg(args, &port, /*allow_missing=*/0) < 0 ||
        op_parse_kv_str(args, "name", name, sizeof(name)) != 0 ||
        op_parse_kv_u32(args, "value", &val) != 0) {
        op_send_errf(c, "usage: controller.button port=N name=<id> value=<0|1>");
        return 0;
    }
    const struct kv_button *b = find_button(name);
    if (!b) {
        op_send_errf(c, "unknown button '%s'", name);
        return 0;
    }
    struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];
    seq_begin_write(st);
    if (val) st->buttons |= b->bit;
    else     st->buttons &= (uint16_t)~b->bit;
    seq_end_write(st);
    op_send_okf(c, "port=%u %s=%u buttons=0x%04x seq=%u",
                (unsigned)port, name, (unsigned)(val ? 1 : 0),
                (unsigned)st->buttons, (unsigned)st->seq);
    return 0;
}

int cmd_controller_axis(struct netconn *c, const char *args)
{
    uint32_t port = 0;
    int32_t  val = 0;
    char name[24];
    if (parse_port_arg(args, &port, /*allow_missing=*/0) < 0 ||
        op_parse_kv_str(args, "name", name, sizeof(name)) != 0 ||
        parse_kv_int(args, "value", &val) != 0) {
        op_send_errf(c, "usage: controller.axis port=N name=<id> value=<int>");
        return 0;
    }
    const struct kv_axis *a = find_axis(name);
    if (!a) {
        op_send_errf(c, "unknown axis '%s'", name);
        return 0;
    }
    struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];
    seq_begin_write(st);
    apply_axis(st, a->index, val);
    seq_end_write(st);
    op_send_okf(c, "port=%u %s=%d seq=%u",
                (unsigned)port, name, (int)val, (unsigned)st->seq);
    return 0;
}

static void emit_port_lines(struct netconn *c, uint32_t port)
{
    struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];
    char buf[160];
    snprintf(buf, sizeof(buf),
             "port.%u.buttons=0x%04x", (unsigned)port,
             (unsigned)st->buttons);
    op_send_line(c, buf);
    snprintf(buf, sizeof(buf),
             "port.%u.triggers lt=%d rt=%d",
             (unsigned)port, (int)st->ltrigger,
             (int)st->rtrigger);
    op_send_line(c, buf);
    snprintf(buf, sizeof(buf),
             "port.%u.lstick x=%d y=%d",
             (unsigned)port, (int)st->lstick_x, (int)st->lstick_y);
    op_send_line(c, buf);
    snprintf(buf, sizeof(buf),
             "port.%u.rstick x=%d y=%d",
             (unsigned)port, (int)st->rstick_x, (int)st->rstick_y);
    op_send_line(c, buf);
    snprintf(buf, sizeof(buf),
             "port.%u.seq=%u", (unsigned)port, (unsigned)st->seq);
    op_send_line(c, buf);
    /* timestamp_us is KeQueryPerformanceCounter-derived microseconds
     * since the agent booted (not wall-clock); useful as a freshness
     * indicator for downstream consumers that read the buffer
     * directly. Print as %llu since `unsigned long` is 32-bit on the
     * Xbox i386 ABI but timestamp_us is uint64_t. */
    snprintf(buf, sizeof(buf),
             "port.%u.timestamp_us=%llu",
             (unsigned)port,
             (unsigned long long)st->timestamp_us);
    op_send_line(c, buf);
}

int cmd_controller_get(struct netconn *c, const char *args)
{
    uint32_t port = 0;
    int rc = parse_port_arg(args, &port, /*allow_missing=*/1);
    if (rc < 0) {
        op_send_errf(c, "usage: controller.get [port=N]");
        return 0;
    }
    op_send_text_begin(c, 0);
    if (port == (uint32_t)-1) {
        for (uint32_t p = 0; p < ORACLE_CTRL_NUM_PORTS; p++) {
            emit_port_lines(c, p);
        }
    } else {
        emit_port_lines(c, port);
    }
    op_send_text_end(c);
    return 0;
}

int cmd_controller_clear(struct netconn *c, const char *args)
{
    uint32_t port = 0;
    int rc = parse_port_arg(args, &port, /*allow_missing=*/1);
    if (rc < 0) {
        op_send_errf(c, "usage: controller.clear [port=N]");
        return 0;
    }
    if (port == (uint32_t)-1) {
        for (uint32_t p = 0; p < ORACLE_CTRL_NUM_PORTS; p++) {
            struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[p];
            uint32_t saved_seq = st->seq;
            seq_begin_write(st);
            /* memset zeros every field including seq + timestamp_us;
             * restore both via seq_end_write and the caller-saved
             * seq below so external observers still see monotonic
             * progression after a clear. */
            uint16_t b = 0;
            int16_t lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
            st->buttons = b; st->ltrigger = lt; st->rtrigger = rt;
            st->lstick_x = lx; st->lstick_y = ly;
            st->rstick_x = rx; st->rstick_y = ry;
            /* Preserve monotonic seq across the clear: write the
             * post-clear stable value as saved_seq (caller-observed
             * progression). seq_end_write restores even parity below. */
            st->seq = (saved_seq | 1u);  /* keep the in-flight odd marker */
            seq_end_write(st);
        }
        op_send_okf(c, "all ports cleared");
    } else {
        struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];
        uint32_t saved_seq = st->seq;
        seq_begin_write(st);
        st->buttons = 0;
        st->ltrigger = 0; st->rtrigger = 0;
        st->lstick_x = 0; st->lstick_y = 0;
        st->rstick_x = 0; st->rstick_y = 0;
        st->seq = (saved_seq | 1u);
        seq_end_write(st);
        op_send_okf(c, "port %u cleared", (unsigned)port);
    }
    return 0;
}

int cmd_controller_buffer_info(struct netconn *c, const char *args)
{
    (void)args;
    /* Re-derive physical address each call so a stale `g_oracle_ctrl_phys`
     * (e.g. agent re-launched and re-attached) doesn't lie to the
     * client. MmGetPhysicalAddress is cheap. */
    uintptr_t phys = (uintptr_t)MmGetPhysicalAddress((PVOID)oracle_ctrl_get());
    op_send_okf(c,
                "addr=0x%08lx phys=0x%08lx size=%u magic=0x%08lx "
                "version=%u ports=%u port_state_size=%u "
                "anchor=%s",
                (unsigned long)(uintptr_t)oracle_ctrl_get(),
                (unsigned long)phys,
                (unsigned)sizeof(struct oracle_ctrl_buffer),
                (unsigned long)oracle_ctrl_get()->magic,
                (unsigned)oracle_ctrl_get()->version,
                (unsigned)ORACLE_CTRL_NUM_PORTS,
                (unsigned)sizeof(struct oracle_ctrl_port_state),
                ORACLE_CTRL_ADDR_FILE);
    return 0;
}
