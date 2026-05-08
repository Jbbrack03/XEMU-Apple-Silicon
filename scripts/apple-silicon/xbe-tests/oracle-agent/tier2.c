/*
 * xbox-oracle-agent — Tier-2 resident hook probes.
 *
 * The first hook shape is intentionally tiny:
 *
 *   jmp KeRaiseIrqlToDpcLevel_original
 *
 * The counter variant adds only:
 *
 *   pushfd
 *   inc dword ptr [hook_page.calls]
 *   popfd
 *
 * It proves that a persistent blob can survive XLaunchXBE and sit on the
 * same kernel boundary NKPatcher uses for IGR, without attempting any
 * controller-state mutation yet.
 */
#include "tier2.h"
#include "commands.h"
#include "protocol.h"
#include "../lib/xbed_tier2_hook.h"

#include <hal/debug.h>
#include <nxdk/mount.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIER2_STATE_DIR "E:\\Apps\\oracle-agent\\state"

static struct xbed_tier2_hook_page *g_hook_page;
static uintptr_t                    g_hook_phys;
static int                          g_hook_anchor_ok;

enum tier2_hook_mode {
    TIER2_HOOK_JUMP_ONLY = 0,
    TIER2_HOOK_COUNTER = 1,
};

static inline void cache_writeback_invalidate(void)
{
    __asm__ __volatile__("wbinvd" ::: "memory");
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 0);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int ensure_e_drive_mounted(void)
{
    if (nxIsDriveMounted('E')) return 1;
    if (nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")) return 1;
    debugPrint("tier2: E: mount failed\n");
    return 0;
}

static int parse_hex(const char *s, uintptr_t *out)
{
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return -1;
    s += 2;
    uintptr_t acc = 0;
    int digits = 0;
    while (*s && *s != '\r' && *s != '\n') {
        int v = -1;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = 10 + (*s - 'a');
        else if (*s >= 'A' && *s <= 'F') v = 10 + (*s - 'A');
        else return -1;
        if (digits >= (int)(sizeof(uintptr_t) * 2)) return -1;
        acc = (acc << 4) | (uintptr_t)v;
        digits++;
        s++;
    }
    if (digits == 0) return -1;
    *out = acc;
    return 0;
}

static int write_anchor_file(uintptr_t phys, uintptr_t virt, size_t size)
{
    if (!ensure_e_drive_mounted()) return -1;
    CreateDirectoryA("E:\\Apps", NULL);
    CreateDirectoryA("E:\\Apps\\oracle-agent", NULL);
    CreateDirectoryA(TIER2_STATE_DIR, NULL);

    char expected[96];
    int expected_n = snprintf(expected, sizeof(expected),
                              "XT2H\n0x%08lx\n0x%08lx\n0x%08lx\n",
                              (unsigned long)phys, (unsigned long)virt,
                              (unsigned long)size);
    if (expected_n <= 0 || expected_n >= (int)sizeof(expected)) return -1;

    FILE *fp = fopen(XBED_TIER2_HOOK_ANCHOR_PATH, "wb");
    if (!fp) return -1;
    size_t n = fwrite(expected, 1, (size_t)expected_n, fp);
    int io_err = (n != (size_t)expected_n) || ferror(fp);
    if (fflush(fp) != 0) io_err = 1;
    if (fclose(fp) != 0) io_err = 1;
    if (io_err) return -1;

    char verify[sizeof(expected)] = {0};
    fp = fopen(XBED_TIER2_HOOK_ANCHOR_PATH, "rb");
    if (!fp) return -1;
    size_t got = fread(verify, 1, (size_t)expected_n, fp);
    int close_err = fclose(fp) != 0;
    if (got != (size_t)expected_n || close_err ||
        memcmp(verify, expected, (size_t)expected_n) != 0) {
        return -1;
    }
    return 0;
}

static struct xbed_tier2_hook_page *attach_anchor(uintptr_t *out_phys)
{
    if (!ensure_e_drive_mounted()) return NULL;

    FILE *fp = fopen(XBED_TIER2_HOOK_ANCHOR_PATH, "rb");
    if (!fp) return NULL;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return NULL;

    char *line[4] = {0};
    int li = 0;
    char *p = buf;
    line[li++] = p;
    while (*p && li < 4) {
        if (*p == '\n') {
            *p = 0;
            if (p[1] != 0) line[li++] = p + 1;
        }
        p++;
    }
    if (li < 4 || strncmp(line[0], "XT2H", 4) != 0) return NULL;

    uintptr_t phys = 0, recorded_virt = 0, recorded_size = 0;
    if (parse_hex(line[1], &phys) != 0 ||
        parse_hex(line[2], &recorded_virt) != 0 ||
        parse_hex(line[3], &recorded_size) != 0) {
        return NULL;
    }
    if (phys == 0 || phys >= 0x04000000u ||
        recorded_size < sizeof(struct xbed_tier2_hook_page) ||
        recorded_size > XBED_TIER2_HOOK_PAGE_SIZE) {
        return NULL;
    }

    uintptr_t virt = phys | 0x80000000u;
    struct xbed_tier2_hook_page *page =
        (struct xbed_tier2_hook_page *)virt;
    if ((uintptr_t)MmGetPhysicalAddress((PVOID)page) != phys) return NULL;
    if (page->magic != XBED_TIER2_HOOK_MAGIC ||
        page->version != XBED_TIER2_HOOK_VERSION ||
        page->size < sizeof(*page)) {
        return NULL;
    }
    *out_phys = phys;
    (void)recorded_virt;
    return page;
}

static void emit_hook_code(struct xbed_tier2_hook_page *page,
                           enum tier2_hook_mode mode)
{
    uint8_t *code = page->code;
    uint32_t hook_va = (uint32_t)(uintptr_t)&page->code[0];
    uint32_t counter_va = (uint32_t)(uintptr_t)&page->calls;
    uint32_t original_va = XBED_TIER2_KE_RAISE_ORIGINAL_VA;
    uint32_t off = 0;

    memset(code, 0x90, XBED_TIER2_HOOK_CODE_SIZE);

    if (mode == TIER2_HOOK_COUNTER) {
        code[off++] = 0x9C;          /* pushfd */
        code[off++] = 0xFF;          /* inc dword ptr [abs32] */
        code[off++] = 0x05;
        put32(&code[off], counter_va);
        off += 4;
        code[off++] = 0x9D;          /* popfd */
    }
    code[off++] = 0xE9;              /* jmp rel32 */
    put32(&code[off], original_va - (hook_va + off + 4u));
    off += 4;

    page->hook_va = hook_va;
    page->hook_rva = hook_va - XBED_TIER2_KERNEL_BASE;
    page->code_off = XBED_TIER2_HOOK_CODE_OFF;
    page->code_size = off;
}

static struct xbed_tier2_hook_page *allocate_page(uintptr_t *out_phys)
{
    PVOID p = MmAllocateContiguousMemoryEx(
        XBED_TIER2_HOOK_PAGE_SIZE,
        0x00020000u,
        0x03ffffffu,
        0x1000u,
        PAGE_READWRITE);
    if (!p) {
        debugPrint("tier2: MmAllocateContiguousMemoryEx failed\n");
        return NULL;
    }
    uintptr_t phys = (uintptr_t)MmGetPhysicalAddress(p);
    if (phys == 0) {
        MmFreeContiguousMemory(p);
        return NULL;
    }
    MmPersistContiguousMemory(p, XBED_TIER2_HOOK_PAGE_SIZE, TRUE);

    struct xbed_tier2_hook_page *page =
        (struct xbed_tier2_hook_page *)(phys | 0x80000000u);
    memset(page, 0, XBED_TIER2_HOOK_PAGE_SIZE);
    page->magic = XBED_TIER2_HOOK_MAGIC;
    page->version = XBED_TIER2_HOOK_VERSION;
    page->size = sizeof(*page);
    page->hook_slot_va = XBED_TIER2_KE_RAISE_SLOT_VA;
    page->original_rva = XBED_TIER2_KE_RAISE_ORIGINAL_RVA;
    page->original_va = XBED_TIER2_KE_RAISE_ORIGINAL_VA;
    emit_hook_code(page, TIER2_HOOK_COUNTER);
    cache_writeback_invalidate();

    g_hook_anchor_ok =
        (write_anchor_file(phys, (uintptr_t)page,
                           XBED_TIER2_HOOK_PAGE_SIZE) == 0);
    if (!g_hook_anchor_ok) {
        debugPrint("tier2: WARNING anchor write failed\n");
    }
    *out_phys = phys;
    return page;
}

static struct xbed_tier2_hook_page *ensure_page(void)
{
    if (g_hook_page) return g_hook_page;
    g_hook_page = attach_anchor(&g_hook_phys);
    if (g_hook_page) return g_hook_page;
    g_hook_page = allocate_page(&g_hook_phys);
    return g_hook_page;
}

static uint32_t slot_read(void)
{
    volatile uint32_t *slot =
        (volatile uint32_t *)(uintptr_t)XBED_TIER2_KE_RAISE_SLOT_VA;
    return *slot;
}

static void slot_write(uint32_t value)
{
    volatile uint32_t *slot =
        (volatile uint32_t *)(uintptr_t)XBED_TIER2_KE_RAISE_SLOT_VA;
    *slot = value;
    cache_writeback_invalidate();
}

void oracle_tier2_init(void)
{
    uintptr_t phys = 0;
    struct xbed_tier2_hook_page *page = attach_anchor(&phys);
    if (page) {
        g_hook_page = page;
        g_hook_phys = phys;
        g_hook_anchor_ok = 1;
        debugPrint("tier2: attached existing hook page phys=0x%08lx "
                   "virt=%p calls=%lu\n",
                   (unsigned long)phys, (void *)page,
                   (unsigned long)page->calls);
    }
}

int cmd_tier2_preflight(struct netconn *c, const char *args)
{
    (void)args;
    uint32_t observed = slot_read();
    const char *verdict =
        observed == XBED_TIER2_KE_RAISE_ORIGINAL_RVA ? "ok" :
        (g_hook_page && observed == g_hook_page->hook_rva) ? "already-hooked" :
        "fail";
    op_send_okf(c, "verdict=%s slot=0x%08lx observed_rva=0x%08lx "
                   "expected_rva=0x%08lx",
                verdict,
                (unsigned long)XBED_TIER2_KE_RAISE_SLOT_VA,
                (unsigned long)observed,
                (unsigned long)XBED_TIER2_KE_RAISE_ORIGINAL_RVA);
    return 0;
}

static int install_hook(struct netconn *c, enum tier2_hook_mode mode)
{
    if (!oracle_writes_enabled()) {
        op_send_errf(c, "writes disabled — call unsafe.enable first");
        return 0;
    }
    struct xbed_tier2_hook_page *page = ensure_page();
    if (!page) {
        op_send_errf(c, "failed to allocate/attach Tier-2 hook page");
        return 0;
    }
    uint32_t observed = slot_read();
    if (observed != XBED_TIER2_KE_RAISE_ORIGINAL_RVA &&
        observed != page->hook_rva) {
        page->last_error = observed;
        op_send_errf(c, "slot owned by another hook: observed_rva=0x%08lx",
                     (unsigned long)observed);
        return 0;
    }
    if (observed != page->hook_rva) {
        emit_hook_code(page, mode);
        page->calls = 0;
        cache_writeback_invalidate();
        slot_write(page->hook_rva);
        page->installs++;
    }
    page->flags |= 1u;
    if (mode == TIER2_HOOK_COUNTER) {
        page->flags |= 2u;
    } else {
        page->flags &= ~2u;
    }
    cache_writeback_invalidate();
    op_send_okf(c, "installed mode=%s slot=0x%08lx hook_rva=0x%08lx "
                   "hook_va=0x%08lx code_size=%lu calls=%lu anchor_ok=%d",
                mode == TIER2_HOOK_COUNTER ? "counter" : "jump-only",
                (unsigned long)XBED_TIER2_KE_RAISE_SLOT_VA,
                (unsigned long)page->hook_rva,
                (unsigned long)page->hook_va,
                (unsigned long)page->code_size,
                (unsigned long)page->calls,
                g_hook_anchor_ok);
    return 0;
}

int cmd_tier2_install_jump_only(struct netconn *c, const char *args)
{
    if (!args || !strstr(args, "confirm=crash-risk-20260508")) {
        op_send_errf(c, "Tier-2 install crashed the Xbox on 2026-05-08; "
                        "rerun with confirm=crash-risk-20260508");
        return 0;
    }
    return install_hook(c, TIER2_HOOK_JUMP_ONLY);
}

int cmd_tier2_install_noop(struct netconn *c, const char *args)
{
    if (!args || !strstr(args, "confirm=crash-risk-20260508")) {
        op_send_errf(c, "Tier-2 install crashed the Xbox on 2026-05-08; "
                        "rerun with confirm=crash-risk-20260508");
        return 0;
    }
    return install_hook(c, TIER2_HOOK_COUNTER);
}

int cmd_tier2_uninstall(struct netconn *c, const char *args)
{
    (void)args;
    if (!oracle_writes_enabled()) {
        op_send_errf(c, "writes disabled — call unsafe.enable first");
        return 0;
    }
    struct xbed_tier2_hook_page *page = ensure_page();
    if (!page) {
        op_send_errf(c, "failed to allocate/attach Tier-2 hook page");
        return 0;
    }
    uint32_t observed = slot_read();
    if (observed == page->hook_rva) {
        slot_write(XBED_TIER2_KE_RAISE_ORIGINAL_RVA);
        page->uninstalls++;
        page->flags &= ~1u;
        cache_writeback_invalidate();
        op_send_okf(c, "uninstalled restored_rva=0x%08lx calls=%lu",
                    (unsigned long)XBED_TIER2_KE_RAISE_ORIGINAL_RVA,
                    (unsigned long)page->calls);
        return 0;
    }
    if (observed == XBED_TIER2_KE_RAISE_ORIGINAL_RVA) {
        page->flags &= ~1u;
        op_send_okf(c, "already-original calls=%lu",
                    (unsigned long)page->calls);
        return 0;
    }
    page->last_error = observed;
    op_send_errf(c, "slot owned by another hook: observed_rva=0x%08lx",
                 (unsigned long)observed);
    return 0;
}

int cmd_tier2_status(struct netconn *c, const char *args)
{
    (void)args;
    if (!g_hook_page) {
        uintptr_t phys = 0;
        g_hook_page = attach_anchor(&phys);
        if (g_hook_page) {
            g_hook_phys = phys;
            g_hook_anchor_ok = 1;
        }
    }
    uint32_t observed = slot_read();
    op_send_text_begin(c, 0);
    char line[160];
    snprintf(line, sizeof(line), "slot_va=0x%08lx",
             (unsigned long)XBED_TIER2_KE_RAISE_SLOT_VA);
    op_send_line(c, line);
    snprintf(line, sizeof(line), "observed_rva=0x%08lx",
             (unsigned long)observed);
    op_send_line(c, line);
    snprintf(line, sizeof(line), "expected_original_rva=0x%08lx",
             (unsigned long)XBED_TIER2_KE_RAISE_ORIGINAL_RVA);
    op_send_line(c, line);
    if (g_hook_page) {
        snprintf(line, sizeof(line), "page_phys=0x%08lx",
                 (unsigned long)g_hook_phys);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "page_virt=0x%08lx",
                 (unsigned long)(uintptr_t)g_hook_page);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "hook_va=0x%08lx",
                 (unsigned long)g_hook_page->hook_va);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "hook_rva=0x%08lx",
                 (unsigned long)g_hook_page->hook_rva);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "code_size=%lu",
                 (unsigned long)g_hook_page->code_size);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "calls=%lu",
                 (unsigned long)g_hook_page->calls);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "installs=%lu",
                 (unsigned long)g_hook_page->installs);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "uninstalls=%lu",
                 (unsigned long)g_hook_page->uninstalls);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "flags=0x%08lx",
                 (unsigned long)g_hook_page->flags);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "anchor_ok=%d", g_hook_anchor_ok);
        op_send_line(c, line);
        snprintf(line, sizeof(line), "installed=%d",
                 observed == g_hook_page->hook_rva ? 1 : 0);
        op_send_line(c, line);
    } else {
        op_send_line(c, "page=absent");
        snprintf(line, sizeof(line), "installed=%d", 0);
        op_send_line(c, line);
    }
    op_send_text_end(c);
    return 0;
}
