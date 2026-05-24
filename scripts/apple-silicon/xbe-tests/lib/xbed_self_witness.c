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
         * process, regardless of the allocator outcome (the
         * allocator entry point itself varies per cycle: cycles
         * 29..41d called `MmAllocateContiguousMemoryEx`; cycle 41e
         * onward calls the non-`-Ex` `MmAllocateContiguousMemory`).
         * The `s_eeprom_scratch_attempted` sticky flag is
         * checked BEFORE entering the write block — without it, a
         * cycle-39 G0(c) outcome (allocation returns NULL on first
         * call → `s_witness_page` stays 0 → later .CRT$XCU and
         * in-main fires re-enter this first-call branch) would
         * OVERWRITE the cycle-39 first-fire breadcrumb byte from
         * 0xA4 (stage 4 = .CRT$XXC) to 0xA5 (stage 5 = .CRT$XCU) or
         * 0xA1/0xA3 (in-main MAIN_ENTERED/POST_MARKER0 if main()
         * ran), destroying the cycle-39 discriminator value (Codex
         * round-2 P1 finding adopted). Position: AS THE LAST
         * INSTRUCTION before the allocator call. See
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
                    "(continuing to the allocator call; "
                    "write will NOT be re-attempted)",
                    (unsigned long)eep_s);
            }
        }

        /* First call this process — allocate + init.
         *
         * CYCLE-41E BOUNDED VARIATION (non-`-Ex` fallback): replace
         * the cycle-29..41d `MmAllocateContiguousMemoryEx(size,
         * lowest, highest, alignment, protect)` 5-arg call with the
         * plain 1-arg `MmAllocateContiguousMemory(size)` call. This
         * is the last bounded cycle-41-scope variation: it isolates
         * the `-Ex` variant itself as the candidate failing
         * constraint by switching to the simpler non-`-Ex` entry
         * point with the SAME requested page size.
         *
         * API note (prompt-vs-header reconciliation): the cycle-41d
         * orchestration prompt for cycle 41e described the
         * substitution as a "2-arg" call
         * `MmAllocateContiguousMemory(size, protect)`. The actual
         * nxdk header at `nxdk/lib/xboxkrnl/xboxkrnl.h:3473-3476`
         * declares the function as a SINGLE-arg `(IN SIZE_T
         * NumberOfBytes)` prototype, and the in-tree call sites at
         * `nxdk/lib/hal/xbox.c:34` + `:80` use the 1-arg form
         * (`MmAllocateContiguousMemory(LaunchDataPageSize)`). The
         * exported kernel ordinal `MmAllocateContiguousMemory@4`
         * (`nxdk/lib/xboxkrnl/xboxkrnl.exe.def:172`) confirms the
         * stdcall stack argument size is 4 bytes = 1 ULONG argument.
         * A 2-arg call would be a compile error. We therefore follow
         * the actual API and pass only `size`; the `Protect`
         * argument has no place in the non-`-Ex` ABI — the kernel
         * decides the protection bits internally. The exact
         * kernel-default Protect / placement / Alignment for this
         * console + build are NOT measured on this hardware; see
         * the Honest-framing paragraph below + the cycle-41c+41d
         * defensive guards that close consumer-visibility blind
         * spots regardless of what the kernel returns. (Codex
         * round-3 finding #3 adopted — earlier claim "historically
         * PAGE_READWRITE, cacheable write-back" had no local
         * citation and was inconsistent with the surrounding
         * unmeasured-defaults framing.)
         *
         * Rationale: cycles 40 + 41a + 41b exhausted cache-policy
         * (bare RW / NOCACHE / WRITECOMBINE all G0(c) against the
         * `-Ex` variant); cycle 41c eliminated the "kernel demands
         * a specific non-cycle-29-tuple address range" sub-hypothesis
         * (matched-tuple to nxdk's framebuffer allocator at
         * `nxdk/lib/hal/video.c:363-367` also G0(c)); cycle 41d
         * eliminated the alignment-requirement branch (`Alignment=0u`
         * also G0(c)). The cycle-22 leading hypothesis is now
         * narrowed to {(b) the `-Ex` variant itself,
         * (c) a `size=0x1000`-specific interaction}. Cycle 41e is
         * the cheapest cycle-41-scope discriminator pointed at
         * branch (b): swap to the non-`-Ex` entry point with the
         * SAME 1-page size. If the kernel-side `-Ex` implementation
         * is the failing surface (for example, the address-range /
         * alignment validation logic of `-Ex` rejects the cycle-29
         * calling-context state even when ALL `-Ex` args have been
         * individually shown to be acceptable to nxdk-internal
         * callers), then bypassing `-Ex` entirely should advance
         * the outcome past G0(c) (`witness.scan-self count >= 1`
         * with `(reserved0 >> 24) == 0xA4`).
         *
         * Honest framing (Codex round-1 finding #2 adopted):
         * cycle 41e changes more than just the entry-point symbol.
         * Dropping to the non-`-Ex` variant simultaneously cedes
         * caller control over Protect (cache policy), placement
         * (LowestAcceptableAddress / HighestAcceptableAddress), and
         * Alignment to whatever defaults the kernel picks for the
         * non-`-Ex` ABI. Those defaults are not measured on this
         * hardware. So a cycle-41e SUCCESS (`count >= 1`) is
         * strong but NOT conclusive evidence that "branch (b) was
         * the failing constraint" — it is consistent with EITHER
         * "the `-Ex` entry-point logic was the problem" OR "the
         * non-`-Ex` defaults happen to land on an as-yet-unmeasured
         * combination that the `-Ex` variations did not visit"
         * (e.g. the kernel-default Protect bits + the kernel-default
         * placement + the kernel-default alignment happens to be a
         * working tuple the cycle-41a/b/c/d sweeps missed). A
         * cycle-41e FAILURE (`count = 0` with EEPROM `byte=0xA4`)
         * is STRONG evidence against branch (b) being the SOLE
         * failing constraint, but does not formally eliminate
         * branch (b) on its own — it shows the non-`-Ex` defaults
         * also fail, narrowing toward branch (c) (`size=0x1000`-
         * specific interaction) but leaving open the possibility
         * that both entry points share an unrelated failure mode.
         * Cycle-41 scope ends here either way: changing `size`
         * would change the WTNS layout contract and is out of
         * scope. After cycle 41e the only paths forward are
         * (i) custom XBE-header callback before `_start` (high
         * scope; requires `nxdk/tools/cxbe/` changes), or
         * (ii) redesign of the cycle-29 self-witness as multi-page
         * (high scope; changes WTNS layout contract).
         *
         * Precedent strength: dropping to the non-`-Ex` variant is
         * an in-tree usage pattern — `nxdk/lib/hal/xbox.c:34` + `:80`
         * use `MmAllocateContiguousMemory(LaunchDataPageSize)`
         * unconditionally for the launch-data page. The launch-data
         * page is a per-boot kernel-managed allocation, so its
         * known-good status proves the non-`-Ex` entry point is
         * reachable on this kernel — it does NOT prove what
         * alignment, cache policy, address range, or phys-range
         * the kernel returns. We have no measured comparison
         * between non-`-Ex` and `-Ex` returns for the same size on
         * this hardware. The cycle-41c symmetric phys-range guards
         * and cycle-41d page-alignment guard below are therefore
         * STILL load-bearing for the cycle-41e real-Xbox
         * interpretation: any of the three guards firing yields the
         * same observable `count=0` shape as G0(c), so a cycle-41e
         * outcome of `count=0` is "allocation failed OR returned
         * out-of-window OR returned sub-page-aligned phys" — same
         * G0(c) interpretation envelope as cycles 41c+41d.
         *
         * What cycle 41e does NOT directly test: it does not
         * isolate the cache-policy axis (already exhausted across
         * cycles 40+41a+41b under the `-Ex` variant only). It does
         * not isolate address range or alignment (cycles 41c+41d
         * exhausted those under the `-Ex` variant only). What it
         * tests is the joint question: does swapping the `-Ex`
         * entry point for the non-`-Ex` entry point — and with
         * that swap also accepting whatever Protect, placement,
         * and alignment defaults the kernel applies for the non-
         * `-Ex` ABI — yield a usable allocation on this real-Xbox
         * kernel from this calling context. That joint signal is
         * the right cycle-41-scope question to ask after the four
         * one-axis sweeps but does not on its own decompose into a
         * pure single-axis result; see the honest-framing paragraph
         * above.
         *
         * Defensive phys-range guards (cycle-41c Codex-R1+R2 P1)
         * are PRESERVED unchanged. They reject any returned `phys`
         * outside the cycle-29 consumer's scan window
         * `[0x00010000, 0x04000000)`. The non-`-Ex` variant gives
         * us no address-range control, so the kernel could in
         * principle return a phys outside the consumer scan window
         * — the guards close that interpretation gap. The
         * cycle-41d page-alignment guard is also PRESERVED unchanged
         * for the same defense-in-depth reason: the non-`-Ex`
         * variant does not document its returned alignment.
         *
         * Scope of this variation: ALLOCATOR-ACCEPTANCE triage only,
         * identical to cycles 41a/b/c/d. The producer/consumer
         * readback path is unchanged — both the stamp below and
         * `witness.scan-self` still use the `phys | 0x80000000`
         * cached-RAM mirror. */
        PVOID p = MmAllocateContiguousMemory(0x1000u);
        if (!p) {
            xbed_host_log_write(
                "xbed_self_witness: MmAllocateContiguousMemory "
                "(non-Ex; cycle-41e) failed; cycle-29 option (c) "
                "page not allocated");
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
         * in cycle-41e — the non-`-Ex` fallback gives us no
         * address-range control at all (the kernel picks the phys
         * unilaterally), so the same consumer-scan-window blind
         * spot exists and the guards remain load-bearing.
         *
         * The cycle-29 consumer at
         * `oracle-agent/commands.c::cmd_witness_scan_self` only scans
         * the kseg0 window [0x80010000, 0x84000000] and reconstructs
         * phys as `va & 0x03FFFFFF`. In cycles 29..41b the producer's
         * `-Ex` allocation tuple (`lowest=0x00010000,
         * highest=0x03FFFFFF`) matched that window exactly, so any
         * returned `phys` was guaranteed to be in
         * [0x00010000, 0x04000000) — fully visible to the consumer.
         *
         * Cycle 41c widened BOTH ends of the address range to match
         * nxdk's framebuffer allocator (`lowest=0x00000000,
         * highest=0x7FFFFFFF`); cycle 41d preserved that widened
         * range. The kernel could now in principle return any `phys`
         * in [0x00000000, 0x80000000). Cycle 41e drops the `-Ex`
         * entry point entirely and calls `MmAllocateContiguousMemory`
         * with size only — the kernel has full discretion over the
         * returned `phys`. On retail 64 MiB hardware the kernel
         * cannot return phys outside [0, 0x04000000), but the upper
         * guard is preserved for symmetry with cycles 41c+41d and
         * for the (vanishingly unlikely) debug-Xbox case. The lower
         * guard remains load-bearing on every cycle. Any phys
         * outside [0x00010000, 0x04000000) would be stamped by the
         * producer but INVISIBLE to the consumer's scan — yielding
         * `witness.scan-self count=0` post-run, which would be
         * indistinguishable from the cycle-40 G0(c) "allocation
         * returned NULL or crashed" shape and would BREAK the
         * cycle-40 G-row discriminator.
         *
         * Two defensive guards close the consumer-visibility blind
         * spot so that a `count=0` observation falls in the bounded
         * envelope "allocation failed (NULL) OR allocator returned
         * out-of-window phys OR allocator returned sub-page-aligned
         * phys" (see the closing paragraph below for the full
         * envelope statement; cycle-41e Codex round-1 finding #3
         * adopted — softened from the prior "only allocation
         * failure" wording, which was internally inconsistent with
         * the later envelope text):
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
         * they ensure that a cycle-41e `count=0` outcome can be
         * read as "allocation failed (NULL) OR allocator returned
         * out-of-window phys OR allocator returned sub-page-aligned
         * phys", same interpretation envelope as cycles 41c+41d. A
         * cycle-41e outcome of `count>=1` (with the cycle-39 EEPROM
         * byte at `0xA4`) would prove the non-`-Ex` variant succeeds
         * from this calling context and would be STRONG-but-not-
         * conclusive evidence FOR branch (b) "the `-Ex` variant
         * itself is the failing constraint" — strong because the
         * entry-point swap is the load-bearing delta vs cycle 41d,
         * not conclusive because the swap also cedes Protect /
         * placement / Alignment to kernel defaults (see the
         * Honest-framing paragraph above for the full envelope —
         * cycle-41e Codex round-2 finding adopted; the prior
         * "AND eliminate branch (b)" wording sign-flipped vs the
         * Honest-framing paragraph and is corrected here). If
         * retail hardware ever did return an in-window page-aligned
         * phys to the non-`-Ex` call, the unguarded cycle-29 path
         * runs and yields a normal cycle-29/30 readback shape — no
         * behavioral change vs cycles 29..41d. */
        if (phys < 0x00010000u) {
            xbed_host_log_writef(
                "xbed_self_witness: MmGetPhysicalAddress returned "
                "phys=0x%08lx < 0x00010000; cycle-41e phys is below "
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
                "phys=0x%08lx >= 0x04000000; cycle-41e phys is above "
                "the agent reader's kseg0 scan window "
                "[0x80010000..0x84000000]; freeing self-witness page "
                "(defensive — symmetric upper-bound guard; should not "
                "fire on retail 64 MiB hardware)",
                (unsigned long)phys);
            MmFreeContiguousMemory(p);
            return 0;
        }

        /* CYCLE-41D PAGE-ALIGNMENT GUARD (Codex round-1 P2 adopted).
         * PRESERVED unchanged in cycle-41e.
         *
         * Cycle 41e calls the non-`-Ex` `MmAllocateContiguousMemory`
         * variant with size only; no documented guarantee on returned
         * alignment. The cycle-29 consumer at
         * `oracle-agent/commands.c:cmd_witness_scan_self` scans on a
         * fixed `0x1000` page stride starting from `0x80010000`, so a
         * returned `phys` that is not 0x1000-aligned would be stamped
         * by the producer but invisible to the consumer's stride —
         * yielding `witness.scan-self count=0` post-run,
         * indistinguishable from G0(c). The in-tree non-`-Ex` call
         * sites at `nxdk/lib/hal/xbox.c:34` + `:80`
         * (`MmAllocateContiguousMemory(LaunchDataPageSize)`) prove
         * the API is reachable on this kernel, NOT that the returned
         * phys is page-aligned for the cycle-29 size/context. The
         * guard is the defense-in-depth that closes the consumer-
         * stride blind spot. It does NOT make `count=0` uniquely
         * imply allocation failure — any of the three guards firing
         * (lower phys-range, upper phys-range, this alignment guard)
         * also yields `count=0` with the page allocated then freed
         * pre-stamp. The full real-Xbox interpretation lives in the
         * surrounding cycle-41e comment block. */
        if ((phys & 0xFFFu) != 0u) {
            xbed_host_log_writef(
                "xbed_self_witness: MmGetPhysicalAddress returned "
                "phys=0x%08lx not 0x1000-aligned; cycle-41e phys "
                "would be invisible to the agent reader's 0x1000-stride "
                "scan; freeing self-witness page (defensive — "
                "alignment guard; non-`-Ex` API has no documented "
                "returned-alignment guarantee; guard closes the "
                "consumer-stride blind spot regardless)",
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
