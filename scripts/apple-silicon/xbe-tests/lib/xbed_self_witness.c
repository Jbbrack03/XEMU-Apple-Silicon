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
         * CYCLE-41D BOUNDED VARIATION (alignment-drop): drop the
         * `alignment` argument from `0x1000u` (cycles 29..41c) to
         * `0u` so the kernel picks its own alignment. All other
         * cycle-41c arguments (matched-tuple address range +
         * `PAGE_READWRITE | PAGE_WRITECOMBINE` protect bits) are
         * preserved:
         *   - 0x1000 size (one page; unchanged from cycles 29..41c)
         *   - 0x00000000 lowest phys (cycle-41c matched-tuple; unchanged)
         *   - 0x7FFFFFFF highest phys (cycle-41c matched-tuple; unchanged)
         *   - 0u alignment (cycle-41d: WAS 0x1000u in cycles 29..41c;
         *     `0u` is in-tree usage precedent against
         *     `MmAllocateContiguousMemoryEx` — call sites that pass
         *     `0u`: `nxdk/lib/hal/video.c:363` does NOT (it pins
         *     `0x1000` for the framebuffer ABI), but
         *     `nxdk/lib/pbkit/pbkit.c:2297`,
         *     `nxdk/samples/{triangle,mesh,xaudio}/main.c`, and
         *     `xbe-tests/flat-tri-depth/main.c:77` all do. The nxdk
         *     header at `nxdk/lib/xboxkrnl/xboxkrnl.h:3464` is a bare
         *     prototype with no `Alignment=0` doc text. What these
         *     sites prove is that the API accepts `0u`; they do NOT
         *     prove what alignment the kernel returns — Codex
         *     round-1 + round-2 P2 findings adopted; the page-alignment
         *     guard added below is the defense-in-depth that closes
         *     the consumer-side stride blind spot regardless)
         *   - PAGE_READWRITE | PAGE_WRITECOMBINE protect (unchanged
         *     from cycles 41b..41c)
         *
         * Rationale: cycles 40 + 41a + 41b exhausted cache-policy
         * (bare RW / NOCACHE / WRITECOMBINE all G0(c)); cycle 41c
         * eliminated the "kernel demands a specific non-cycle-29-tuple
         * address range" sub-hypothesis (matched-tuple to nxdk's
         * framebuffer allocator at `nxdk/lib/hal/video.c:363-367`
         * also G0(c) — kernel rejects the request even with the
         * exact known-good address range). The cycle-22 leading
         * hypothesis is now narrowed to {(a) alignment requirement
         * `0x1000`, (b) the `-Ex` variant itself, (c) a `size=0x1000`-
         * specific interaction}. Cycle 41d is the cheapest
         * discriminator for branch (a): a single-literal drop of the
         * alignment argument to `0u`. If the kernel rejects the
         * cycle-29 call solely because it cannot or will not honor
         * the requested 0x1000 alignment for a single-page allocation
         * inside the matched-tuple address range, cycle 41d should
         * advance the outcome past G0(c) (i.e. `witness.scan-self
         * count >= 1` with `(reserved0 >> 24) == 0xA4`). If cycle
         * 41d still yields G0(c), branch (a) is ELIMINATED and only
         * non-`-Ex` fallback to plain `MmAllocateContiguousMemory`
         * (cycle 41e) remains in cycle-41 scope before forcing a
         * fundamentally new approach (custom XBE-header callback
         * before `_start` — high scope; requires `nxdk/tools/cxbe/`
         * changes) or a redesign of the cycle-29 self-witness
         * as multi-page (high scope; would change the WTNS layout
         * contract).
         *
         * Precedent strength: dropping `alignment` to `0u` against
         * `MmAllocateContiguousMemoryEx` is an in-tree usage pattern
         * — `nxdk/lib/hal/video.c:363` pins `0x1000` but a number of
         * other call sites pass `0u` (see the citation list below).
         * The nxdk header at `xboxkrnl.h:3464` is a bare prototype
         * with no `Alignment=0` doc, so we cannot point to a
         * documented "kernel picks natural page alignment" guarantee;
         * what the in-tree calls demonstrate is only that the API
         * accepts `0u` as an argument, not what alignment the kernel
         * returns. The framebuffer's `0x1000` is an ABI requirement
         * on the consumer side (framebuffer scanout), not a
         * kernel-acceptance signal — so a successful framebuffer
         * call with `0x1000` does not prove the kernel demands
         * `0x1000`, and a hypothetical `0u` framebuffer call would
         * not prove the kernel returns page-aligned phys. The
         * `oracle-agent/controller.c:217-226` agent buffer also
         * uses `0x1000` — but the agent allocation actually
         * SUCCEEDS, so it does not constrain what alignment values
         * the kernel rejects. Cycle 41d is therefore not testing
         * "the kernel hates 0x1000 alignment" (the agent disproves
         * that) but "the kernel hates 0x1000 alignment specifically
         * for the cycle-29 tuple inside the matched-tuple address
         * range" — a much narrower claim that the cycle-29-vs-agent
         * delta (size, persistence flag, calling context) leaves
         * open.
         *
         * Defensive phys-range guards (cycle-41c Codex-R1+R2 P1)
         * are PRESERVED unchanged at xbed_self_witness.c:254-273.
         * They reject any returned `phys` outside the cycle-29
         * consumer's scan window `[0x00010000, 0x04000000)` so a
         * `witness.scan-self count=0` post-run remains STRONGLY
         * suggestive of allocation failure — but NOT unambiguous,
         * because either guard firing also yields `count=0` (with
         * the page allocated, freed by the guard, and never stamped).
         * The full real-Xbox interpretation lives in the guard-block
         * comment below. Within these guards the cycle-41d claim is
         * narrow but well-defined: a cycle-41d outcome of `count>=1`
         * (with the cycle-39 EEPROM byte at `0xA4`) would invalidate
         * branch (a) only with respect to `0u`-vs-`0x1000` alignment;
         * a cycle-41d outcome of `count=0` further narrows toward
         * branches (b) and (c).
         *
         * Scope of this variation: ALLOCATOR-ACCEPTANCE triage only,
         * identical to cycles 41a/b/c. The producer/consumer readback
         * path is unchanged — both the stamp below and
         * `witness.scan-self` still use the `phys | 0x80000000`
         * cached-RAM mirror. */
        PVOID p = MmAllocateContiguousMemoryEx(
            0x1000u,             /* size: 1 page */
            0x00000000u,         /* lowest phys: match nxdk fb (cycle-41c) */
            0x7FFFFFFFu,         /* highest phys: match nxdk fb (cycle-41c) */
            0u,                  /* alignment: kernel-picked (cycle-41d) */
            PAGE_READWRITE | PAGE_WRITECOMBINE);
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

        /* CYCLE-41C DEFENSIVE PHYS-RANGE GUARDS (Codex round-1 P1 +
         * round-2 P1 adopted; symmetric pair). PRESERVED unchanged
         * in cycle-41d — alignment-drop does not affect the consumer
         * scan-window contract; the matched-tuple address range
         * (`lowest=0x00000000, highest=0x7FFFFFFF`) is also preserved
         * from cycle-41c, so the guards remain load-bearing for the
         * cycle-41d real-Xbox interpretation.
         *
         * The cycle-29 consumer at
         * `oracle-agent/commands.c::cmd_witness_scan_self` only scans
         * the kseg0 window [0x80010000, 0x84000000] and reconstructs
         * phys as `va & 0x03FFFFFF`. In cycles 29..41b the producer's
         * allocation tuple (`lowest=0x00010000, highest=0x03FFFFFF`)
         * matched that window exactly, so any returned `phys` was
         * guaranteed to be in [0x00010000, 0x04000000) — fully visible
         * to the consumer.
         *
         * Cycle 41c widened BOTH ends of the address range to match
         * nxdk's framebuffer allocator (`lowest=0x00000000,
         * highest=0x7FFFFFFF`). The kernel could now in principle
         * return any `phys` in [0x00000000, 0x80000000). Any phys
         * outside [0x00010000, 0x04000000) would be stamped by the
         * producer but INVISIBLE to the consumer's scan — yielding
         * `witness.scan-self count=0` post-run, which would be
         * indistinguishable from the cycle-40 G0(c) "allocation
         * returned NULL or crashed" shape and would BREAK the
         * cycle-40 G-row discriminator. Cycle 41d preserves the same
         * widened address range, so the same blind spot exists; the
         * symmetric guards continue to close it.
         *
         * Two defensive guards close the interpretation gap so a
         * `count=0` observation can only mean "allocation failed":
         *
         *   (i)  phys < 0x00010000  — symmetric lower-bound guard;
         *        cycle-29 reader scan window starts at 0x80010000
         *        (i.e. phys-floor 0x00010000). Subsumes the
         *        `phys == 0` invalid-sentinel check above (kept for
         *        defense in depth as the canonical "MmGetPhysicalAddress
         *        failed" signal — different host-log line).
         *   (ii) phys >= 0x04000000 — symmetric upper-bound guard;
         *        cycle-29 reader scan window ends at 0x84000000
         *        (i.e. phys-ceiling 0x04000000). On retail Original
         *        Xbox (64 MiB physical RAM) the kernel cannot return
         *        a phys it does not have, so this branch is a no-op
         *        on target hardware; guard exists for defense in
         *        depth (debug-Xbox 128 MiB) and to formally close the
         *        Codex round-1 finding.
         *
         * Both branches free the still-unpersisted page (no
         * `MmPersistContiguousMemory` has run yet at this point) and
         * return 0. The EEPROM byte stays at 0xA4 (already stamped
         * pre-MmAlloc), so the OBSERVABLE shape on real Xbox is
         * still `byte=0xA4 + count=0` — IDENTICAL to G0(c). The
         * guards do not give us a real-Xbox-distinguishable signal;
         * they ensure that the cycle-41c interpretation "address
         * range is eliminated as failing constraint (within retail
         * 64 MiB scope)" is only claimable when the kernel DEMONSTRABLY
         * cannot return an in-range phys. If retail hardware ever did
         * return phys in [0x00010000, 0x04000000), the unguarded
         * cycle-29 path runs and yields a normal cycle-29/30 readback
         * shape — no behavioral change vs cycles 29..41b. */
        if (phys < 0x00010000u) {
            xbed_host_log_writef(
                "xbed_self_witness: MmGetPhysicalAddress returned "
                "phys=0x%08lx < 0x00010000; cycle-41d phys is below "
                "the agent reader's kseg0 scan window "
                "[0x80010000..0x84000000]; freeing self-witness page "
                "(defensive — symmetric lower-bound guard)",
                (unsigned long)phys);
            MmFreeContiguousMemory(p);
            return 0;
        }
        if (phys >= 0x04000000u) {
            xbed_host_log_writef(
                "xbed_self_witness: MmGetPhysicalAddress returned "
                "phys=0x%08lx >= 0x04000000; cycle-41d phys is above "
                "the agent reader's kseg0 scan window "
                "[0x80010000..0x84000000]; freeing self-witness page "
                "(defensive — symmetric upper-bound guard; should not "
                "fire on retail 64 MiB hardware)",
                (unsigned long)phys);
            MmFreeContiguousMemory(p);
            return 0;
        }

        /* CYCLE-41D PAGE-ALIGNMENT GUARD (Codex round-1 P2 adopted).
         *
         * Cycle 41d drops `alignment` to `0u` (let the kernel pick).
         * The cycle-29 consumer at
         * `oracle-agent/commands.c:cmd_witness_scan_self` scans on a
         * fixed `0x1000` page stride starting from `0x80010000`, so a
         * returned `phys` that is not 0x1000-aligned would be stamped
         * by the producer but invisible to the consumer's stride —
         * yielding `witness.scan-self count=0` post-run,
         * indistinguishable from G0(c) "allocation failed (NULL or
         * crash)". The cited in-tree `Alignment=0` call sites
         * (`nxdk/lib/hal/video.c:363`, `pbkit/pbkit.c:2297`,
         * `samples/{triangle,mesh,xaudio}`, `flat-tri-depth/main.c:77`)
         * are usage precedent only — they prove the API accepts
         * `0u`, NOT that the kernel returns page-aligned phys; we
         * have not measured returned alignment from any of those
         * sites. This guard is the cycle-41d defense-in-depth: it
         * closes the consumer-stride blind spot specifically. It
         * does NOT make `count=0` uniquely imply allocation failure
         * — any of the three guards firing (lower phys-range, upper
         * phys-range, this alignment guard) also yields `count=0`
         * with the page allocated then freed pre-stamp. The full
         * real-Xbox interpretation lives in the surrounding
         * cycle-41d comment block; the value of this guard is solely
         * that it removes "sub-page-aligned allocation hides from
         * the consumer" from the list of post-run ambiguities. */
        if ((phys & 0xFFFu) != 0u) {
            xbed_host_log_writef(
                "xbed_self_witness: MmGetPhysicalAddress returned "
                "phys=0x%08lx not 0x1000-aligned; cycle-41d phys "
                "would be invisible to the agent reader's 0x1000-stride "
                "scan; freeing self-witness page (defensive — "
                "alignment guard; in-tree precedent only proves the "
                "API accepts Alignment=0, NOT that returned phys is "
                "page-aligned; guard closes the consumer-stride blind "
                "spot regardless)",
                (unsigned long)phys);
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
