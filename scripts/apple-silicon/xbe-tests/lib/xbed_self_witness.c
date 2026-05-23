/*
 * xbed_self_witness — implementation. See header for cycle-29 option
 * (c) discriminator design + layout + safety notes.
 */
#include "xbed_self_witness.h"
#include "xbed_runtime.h"

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>

/* Process-global pointer to the self-witness page. NULL until the
 * first `xbed_self_witness_fire` succeeds; sticky thereafter. The
 * shim is single-threaded; no locking required (real OG Xbox is
 * single-CPU). */
static volatile uint32_t *s_witness_page = 0;
static uintptr_t          s_witness_phys = 0;

/* `wbinvd` (Write-Back + Invalidate cache) — same primitive
 * `oracle-agent/controller.c::cache_writeback_invalidate` and
 * `lib/xbed_a4_witness.c::cache_writeback_invalidate` use. OG Xbox
 * is single-CPU uniprocessor i386; no MFENCE needed. The stamp must
 * be visible to the relaunched agent's `witness.scan-self` reader
 * across the chainload, so we flush after every write. */
static inline void self_witness_wbinvd(void)
{
    __asm__ __volatile__("wbinvd" ::: "memory");
}

uintptr_t xbed_self_witness_fire(uint32_t stage)
{
    /* Emit ENTRY breadcrumb FIRST so the host-log channel reflects
     * that this call site executed even if the allocation below
     * trips on some unanticipated kernel-state interaction (same
     * pattern as `xbed_a4_witness_fire`). */
    xbed_host_log_writef("xbed_self_witness: enter stage=%u",
                         (unsigned)stage);

    if (s_witness_page == 0) {
        /* First call this process — allocate + init.
         *
         * Match the agent's allocation pattern exactly
         * (`oracle-agent/controller.c::s_allocate_fresh`):
         *   - 1 page (0x1000) size
         *   - 0x00010000 lowest acceptable phys (skip very-low pages)
         *   - 0x03ffffff highest (top of 64 MiB RAM)
         *   - 0x1000 page alignment
         *   - PAGE_READWRITE protection
         * The relaunched agent's `witness.scan-self` reader walks
         * the SAME kseg0 [0x80010000, 0x84000000] range the agent
         * itself searches for XCTR buffers; this allocation pattern
         * is known-safe and known-findable in that range. */
        PVOID p = MmAllocateContiguousMemoryEx(
            0x1000u,             /* size: 1 page */
            0x00010000u,         /* lowest phys: skip low pages */
            0x03ffffffu,         /* highest phys: top of 64 MiB RAM */
            0x1000u,             /* alignment: page */
            PAGE_READWRITE);
        if (!p) {
            xbed_host_log_write(
                "xbed_self_witness: MmAllocateContiguousMemoryEx "
                "failed; cycle-29 option (c) page not allocated");
            return 0;
        }

        uintptr_t phys = (uintptr_t)MmGetPhysicalAddress(p);
        if (phys == 0) {
            xbed_host_log_write(
                "xbed_self_witness: MmGetPhysicalAddress returned 0; "
                "freeing self-witness page");
            MmFreeContiguousMemory(p);
            return 0;
        }

        /* Mark persistent so the page survives this XBE's
         * `HalReturnToFirmware(HalRebootRoutine)` exit + the
         * dashboard chainload + the relaunched agent process death
         * boundary. Same flag the agent uses to keep XCTR alive. */
        MmPersistContiguousMemory(p, 0x1000u, TRUE);

        /* Canonicalize the virtual alias to kseg0 (virt = phys |
         * 0x80000000) so the agent's downstream reader and any
         * future diag-XBE reader walk the same page-table path.
         * Matches `oracle-agent/controller.c::s_allocate_fresh`
         * which deliberately uses the kseg0 alias rather than the
         * pointer `MmAllocateContiguousMemoryEx` returned, citing
         * the same cross-XBE visibility concern (see
         * `controller.c` lines 247-249). */
        volatile uint32_t *vp =
            (volatile uint32_t *)(phys | 0x80000000u);

        /* Zero the full page then stamp the 16-byte header. We
         * deliberately do NOT preserve any pre-existing contents on
         * the returned phys: the cycle-29 discriminator wants an
         * unambiguous "this XBE allocated this page" signal, and
         * the agent's preserve-branch concerns (cycle 27) don't
         * apply here because no one else is sharing this magic. */
        for (uint32_t i = 0; i < 0x1000u / sizeof(uint32_t); i++) {
            vp[i] = 0;
        }
        vp[0] = XBED_SELF_WITNESS_MAGIC;
        vp[1] = XBED_SELF_WITNESS_VERSION;
        /* vp[2] (reserved0) and vp[3] (reserved1) already zero from
         * the page wipe above; the stamp below sets them. */

        s_witness_page = vp;
        s_witness_phys = phys;

        xbed_host_log_writef(
            "xbed_self_witness: allocated self-witness page phys=0x%08lx "
            "virt=0x%08lx magic='WTNS' version=1",
            (unsigned long)phys, (unsigned long)(uintptr_t)vp);
    }

    /* Stamp `stage` into `reserved0` (with the 0xA4 tag byte) and
     * tick the counter at `reserved1`. Stage is masked to 24 bits
     * so the tag byte is always 0xA4. Mirrors
     * `xbed_a4_witness_fire`'s stamp pattern. */
    volatile uint32_t *vp = s_witness_page;
    uint32_t prev_counter = vp[3];
    vp[2] = ((uint32_t)XBED_SELF_WITNESS_TAG << 24) |
            (stage & 0x00FFFFFFu);
    vp[3] = prev_counter + 1u;
    self_witness_wbinvd();

    xbed_host_log_writef(
        "xbed_self_witness: fired stage=%u at phys=0x%08lx "
        "virt=0x%08lx counter=%u (self-allocated page; no kseg0 "
        "scan)",
        (unsigned)stage,
        (unsigned long)s_witness_phys,
        (unsigned long)(uintptr_t)vp,
        (unsigned)(prev_counter + 1u));
    return s_witness_phys;
}
