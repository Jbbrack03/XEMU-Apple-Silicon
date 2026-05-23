/*
 * xbed_a4_witness — implementation. See header for the architecture +
 * cycle-23 Path A.4 discriminator semantics.
 */
#include "xbed_a4_witness.h"
#include "xbed_runtime.h"

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <stdint.h>

/* Mirrors `ORACLE_CTRL_MAGIC` / `ORACLE_CTRL_VERSION` in
 * `scripts/apple-silicon/xbe-tests/oracle-agent/controller.h`. The
 * shim deliberately does NOT include the agent's controller.h to keep
 * the witness independent of the agent's lwIP / RPC headers (same
 * separation as `xbed_input_synth.{c,h}`); the constants are tiny and
 * load-bearing for the wire ABI either way. */
#define XCTR_MAGIC    0x58435452u   /* 'XCTR' little-endian */
#define XCTR_VERSION  1u

/* kseg0 identity-maps phys [0x00000000, 0x04000000] to virt
 * [0x80000000, 0x84000000]. The agent's `MmAllocateContiguousMemoryEx`
 * (`oracle-agent/controller.c:153-158`) constrains phys to
 * [0x00010000, 0x03ffffff] aligned to 4 KiB; scan that virtual range
 * in 4 KiB strides for the agent's buffer header. The first 64 KiB
 * (phys [0, 0x10000]) is skipped to match the agent's allocation
 * floor and to avoid touching kernel-data low memory. */
#define KSEG0_SCAN_START 0x80010000u
#define KSEG0_SCAN_END   0x84000000u
#define PAGE_STRIDE      0x1000u

/* Soft bound: refuse to treat any page whose `reserved[1]` (witness
 * counter) is implausibly large as a candidate. Each agent restart
 * leaks at most one persistent page, so even an Xbox session with
 * hundreds of agent restarts won't push the counter past a few
 * thousand. A random false-positive page whose first two uint32
 * happen to match 'XCTR' + version 1 will almost certainly have a
 * wildly different `reserved[1]`; reject those defensively. */
#define A4_MAX_PLAUSIBLE_COUNTER 4096u

/* `wbinvd` (Write-Back + Invalidate cache) — same primitive used by
 * `oracle-agent/controller.c::cache_writeback_invalidate`. The OG
 * Xbox is single-CPU + uniprocessor i386; no MFENCE is needed. The
 * witness writes must be visible to any subsequent reader (including
 * the agent's restarted `witness.scan` after this XBE reboots back
 * to FTP), so a strong cache flush is the conservative choice. */
static inline void cache_writeback_invalidate(void)
{
    __asm__ __volatile__("wbinvd" ::: "memory");
}

/* Shared candidate predicate (Codex cycle-23 finding #2 — keep
 * writer and reader in lockstep). Returns 1 iff the page at `va`
 * passes ALL filters and is a credible A.4-candidate
 * `oracle_ctrl_buffer`. The agent's reader (`commands.c::
 * cmd_witness_scan`) MUST apply the same filter set so both sides
 * agree on what counts as a real buffer vs. a false-positive page.
 *
 * Filters, in order:
 *   1. MmGetPhysicalAddress(va) != 0 — kernel page tables actually
 *      cover this page (else dereference faults; see header).
 *   2. p[0] == XCTR_MAGIC, p[1] == XCTR_VERSION — agent header.
 *   3. p[2] (reserved[0]) is either 0 (fresh agent buffer) or already
 *      tagged with the A.4 witness pattern in its top byte.
 *   4. p[3] (reserved[1] / counter) is within A4_MAX_PLAUSIBLE_COUNTER.
 */
static int a4_candidate_ok(uintptr_t va)
{
    if ((uintptr_t)MmGetPhysicalAddress((PVOID)va) == 0u) return 0;
    volatile uint32_t *p = (volatile uint32_t *)va;
    if (p[0] != XCTR_MAGIC) return 0;
    if (p[1] != XCTR_VERSION) return 0;
    uint32_t r0 = p[2];
    if (r0 != 0u && ((r0 >> 24) != XBED_A4_WITNESS_TAG)) return 0;
    uint32_t r1 = p[3];
    if (r1 > A4_MAX_PLAUSIBLE_COUNTER) return 0;
    return 1;
}

uintptr_t xbed_a4_witness_fire(uint32_t stage)
{
    /* Emit ENTRY breadcrumb FIRST so the host-log channel reflects
     * that the witness call site executed even if the scan below
     * trips on some unanticipated kernel-state interaction. */
    xbed_host_log_writef("xbed_a4_witness: enter stage=%u", (unsigned)stage);

    /* Scan kseg0 in 4 KiB strides looking for the HIGHEST-phys
     * candidate that passes `a4_candidate_ok`. Targeting the highest
     * phys (not the first) is intentional (Codex cycle-23 finding #3):
     * the agent's persistent allocator grows monotonically per
     * restart, so the highest-phys passing page is the most recent
     * agent allocation. On a fresh power-cycle that IS the live agent
     * buffer; on repeated runs within one power session it is the
     * most recent orphan, which keeps cycle-24 readback attribution
     * unambiguous run-to-run.
     *
     * Safety gate (cycle 23, learned from xemu-Metal local
     * validation): blindly dereferencing every page in [0x80010000,
     * 0x84000000] crashes the XBE on xemu and would page-fault on
     * real Xbox for any unmapped page in the 64 MiB physical RAM
     * window. `a4_candidate_ok` calls `MmGetPhysicalAddress` first;
     * the kernel returns 0 for unmapped pages and the expected
     * phys-mod address for mapped pages. Same safety pattern
     * oracle-agent/tier2.c:159 uses against a stale persistent-pool
     * pointer.
     *
     * False-positive defense: magic + version alone matched random
     * pages in kseg0 and a stray write to one such page corrupted
     * enough kernel state to silence subsequent xbed_host_log
     * output. The agent's `oracle_ctrl_buffer` is `memset(0)` to its
     * full sizeof at allocation (controller.c:181), so the canonical
     * shape is `magic=XCTR, version=1, reserved[0]=0, reserved[1]=0`
     * with a subsequent A.4-tagged buffer satisfying
     * `(reserved[0] >> 24) == 0xA4` and `reserved[1] < 4096`. The two
     * extra filters in `a4_candidate_ok` reject random memory whose
     * first two uint32 happen to spell XCTR+1 but whose next 8 bytes
     * don't. */
    uintptr_t best_va = 0;
    uint32_t mapped_pages_seen = 0;
    uint32_t passing_candidates = 0;
    for (uintptr_t va = KSEG0_SCAN_START; va < KSEG0_SCAN_END; va += PAGE_STRIDE) {
        if ((uintptr_t)MmGetPhysicalAddress((PVOID)va) == 0u) continue;
        mapped_pages_seen++;
        if (!a4_candidate_ok(va)) continue;
        passing_candidates++;
        if (va > best_va) best_va = va;
    }

    if (best_va == 0) {
        xbed_host_log_writef(
            "xbed_a4_witness: no XCTR buffer found at stage=%u "
            "(mapped_pages_seen=%u; no agent ran before this XBE this "
            "boot, buffer was reclaimed, or filters rejected all "
            "false-positive matches)",
            (unsigned)stage, (unsigned)mapped_pages_seen);
        return 0;
    }

    /* Best candidate found — apply the witness stamp + bump the
     * counter. Preserve magic + version so subsequent scans by this
     * XBE OR by the restarted agent's `witness.scan` still match
     * this buffer. Stage is masked to 24 bits so the tag byte is
     * always 0xA4. */
    volatile uint32_t *p = (volatile uint32_t *)best_va;
    uint32_t prev_counter = p[3];
    p[2] = ((uint32_t)XBED_A4_WITNESS_TAG << 24) |
           (stage & 0x00FFFFFFu);
    p[3] = prev_counter + 1u;
    cache_writeback_invalidate();

    /* `phys = va & 0x03FFFFFF` undoes the kseg0 mask. */
    uintptr_t phys = (uintptr_t)best_va & 0x03FFFFFFu;
    xbed_host_log_writef(
        "xbed_a4_witness: fired stage=%u at phys=0x%08lx "
        "virt=0x%08lx counter=%u (mapped_pages_seen=%u "
        "candidates_passing=%u; chose HIGHEST-phys match)",
        (unsigned)stage, (unsigned long)phys, (unsigned long)best_va,
        (unsigned)(prev_counter + 1u),
        (unsigned)mapped_pages_seen,
        (unsigned)passing_candidates);
    return phys;
}
