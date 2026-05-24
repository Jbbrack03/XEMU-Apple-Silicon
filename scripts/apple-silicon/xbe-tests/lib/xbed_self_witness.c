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

/* Cycle-39 EEPROM scratchpad write — fired AT MOST ONCE per process,
 * regardless of MmAllocateContiguousMemoryEx outcome. Separate from
 * `s_witness_page` because the cycle-39 G0(c) sub-case is exactly the
 * branch where the allocation fails (so `s_witness_page` stays NULL);
 * if the EEPROM write were re-gated on `s_witness_page == 0`, later
 * `.CRT$XCU` and in-main fires would re-enter the EEPROM-write block
 * and OVERWRITE the breadcrumb byte from 0xA4 (stage 4 = first fire)
 * to 0xA5 / 0xA1 / 0xA3 (later stages), destroying the cycle-39
 * discriminator value. The sticky flag preserves the first-stage
 * breadcrumb regardless of allocation outcome. Set to 1 the FIRST
 * time `xbed_self_witness_fire` enters the first-call branch — both
 * on success (NTSTATUS NT_SUCCESS) AND on failure paths — so we
 * never re-attempt the EEPROM write either. */
static int                s_eeprom_scratch_attempted = 0;

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
        /* CYCLE-39 EEPROM SCRATCHPAD WRITE — durable real-Xbox
         * breadcrumb that proves the `.CRT$XXC` slot's first call
         * reached the pre-allocation point. Fires AT MOST ONCE per
         * process, regardless of MmAllocateContiguousMemoryEx
         * outcome. The `s_eeprom_scratch_attempted` sticky flag is
         * checked BEFORE entering the write block — without it, a
         * cycle-39 G0(c) outcome (allocation returns NULL on first
         * call → `s_witness_page` stays 0 → later .CRT$XCU and
         * in-main fires re-enter this first-call branch) would
         * OVERWRITE the cycle-39 first-fire breadcrumb byte from
         * 0xA4 (stage 4 = .CRT$XXC) to 0xA5 (stage 5 = .CRT$XCU) or
         * 0xA1/0xA3 (in-main MAIN_ENTERED/POST_MARKER0 if main()
         * ran), destroying the cycle-39 discriminator value (Codex
         * round-2 P1 finding adopted). Position: AS THE LAST
         * INSTRUCTION before `MmAllocateContiguousMemoryEx`. See
         * `xbed_self_witness.h` cycle-39 addendum for the full
         * scratchpad contract + discriminator table.
         *
         * Encoding: byte = 0xA0 | (stage & 0x0F). Real-Xbox first
         * call is the `.CRT$XXC` slot stage=4 fire → byte=0xA4.
         * Post-run readback via the agent's `eeprom.scratch.read`
         * (or full `eeprom` dump, offset 0xFF):
         *   - byte=0x00 + WTNS count=0 → sub-case (a) or (b) (crash
         *     before this write executed)
         *   - byte=0xA4 + WTNS count=0 → sub-case (c) (write landed
         *     but allocation returned NULL silently or crashed)
         *   - byte=0xA4 + WTNS count>=1 → full pre-main path landed
         *
         * NTSTATUS is checked + host-logged but does NOT short-
         * circuit the function. The allocation attempt below
         * proceeds regardless — the EEPROM byte is purely additive
         * instrumentation. The sticky flag is set BEFORE the write
         * (rather than only on NT_SUCCESS) so that even a write
         * failure does not cause a later fire to re-attempt the
         * write — the failure host-log line is itself diagnostic
         * (visible on xemu; on real Xbox the EEPROM byte stays
         * unchanged from its pre-run baseline value). */
        if (!s_eeprom_scratch_attempted) {
            s_eeprom_scratch_attempted = 1;
            UCHAR scratch_byte = (UCHAR)(
                XBED_SELF_WITNESS_EEPROM_TAG_NIB |
                (UCHAR)(stage & 0x0Fu));
            NTSTATUS eep_s = HalWriteSMBusValue(
                XBED_SELF_WITNESS_EEPROM_SMBUS_ADDR,
                XBED_SELF_WITNESS_EEPROM_SCRATCH_OFF,
                FALSE,                  /* byte mode (1 byte payload) */
                (ULONG)scratch_byte);
            if (NT_SUCCESS(eep_s)) {
                xbed_host_log_writef(
                    "xbed_self_witness: cycle-39 EEPROM scratchpad "
                    "byte=0x%02X written at off=0x%02X (pre-MmAlloc "
                    "breadcrumb; sticky, at-most-once per process)",
                    (unsigned)scratch_byte,
                    (unsigned)XBED_SELF_WITNESS_EEPROM_SCRATCH_OFF);
            } else {
                xbed_host_log_writef(
                    "xbed_self_witness: cycle-39 EEPROM scratchpad "
                    "HalWriteSMBusValue failed status=0x%08lx "
                    "(continuing to MmAllocateContiguousMemoryEx; "
                    "write will NOT be re-attempted)",
                    (unsigned long)eep_s);
            }
        }

        /* First call this process — allocate + init.
         *
         * Match the agent's allocation pattern exactly
         * (`oracle-agent/controller.c::s_allocate_fresh`):
         *   - 1 page (0x1000) size
         *   - 0x00010000 lowest acceptable phys (skip very-low pages)
         *   - 0x03ffffff highest (top of 64 MiB RAM)
         *   - 0x1000 page alignment
         *   - PAGE_READWRITE | PAGE_NOCACHE protection (cycle-41a:
         *     cache policy variation #1; was bare PAGE_READWRITE in
         *     cycles 29..40). Cycle-40 outcome G0(c) confirmed the
         *     EEPROM-write breadcrumb landed but the bare-RW
         *     `MmAllocateContiguousMemoryEx` call did not yield a
         *     usable allocation on real-Xbox kernel. Cycle-41a
         *     adopts the lowest-scope cycle-39-closeout candidate:
         *     pair PAGE_READWRITE with PAGE_NOCACHE (0x200, defined
         *     in nxdk's `xboxkrnl.h`). Precedent: nxdk's own
         *     `libusbohci_xbox/usbh_xbox.c:41` uses
         *     `PAGE_READWRITE | PAGE_NOCACHE` against
         *     `MmAllocateContiguousMemoryEx` for the USB OHCI
         *     controller's DMA-coherent ring buffers — known-good
         *     pattern on this kernel. Scope of this variation:
         *     ALLOCATOR-ACCEPTANCE triage only. The producer/consumer
         *     readback path is unchanged — both the stamp below and
         *     `witness.scan-self` still use the `phys | 0x80000000`
         *     cached-RAM mirror (Codex 2026-05-24 P1 finding adopted
         *     in spirit: the cycle-41a goal is to discriminate
         *     "kernel rejects bare-RW protect" from "kernel rejects
         *     this allocation tuple regardless of protect"; an
         *     end-to-end "uncached-alias visibility" experiment via
         *     `phys | 0xB0000000` is intentionally deferred to a
         *     later bounded slice if cycle-41a flips the regression
         *     gate). */
        PVOID p = MmAllocateContiguousMemoryEx(
            0x1000u,             /* size: 1 page */
            0x00010000u,         /* lowest phys: skip low pages */
            0x03ffffffu,         /* highest phys: top of 64 MiB RAM */
            0x1000u,             /* alignment: page */
            PAGE_READWRITE | PAGE_NOCACHE);
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
