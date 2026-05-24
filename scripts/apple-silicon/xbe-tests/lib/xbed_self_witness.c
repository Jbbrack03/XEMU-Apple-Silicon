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

/* CYCLE-42D milestone marker writer — direct, host-log-free,
 * single-byte EEPROM write via `HalWriteSMBusValue` to the
 * cycle-39 scratch offset (0xFF). Used ONLY by the stage==6
 * bypass below. Encoding: high nibble = 0xB0 (cycle-42D tag,
 * distinct from cycle-39 0xA0..0xAF stage byte namespace); low
 * nibble = milestone index 0..0xB. See `xbed_self_witness.h`
 * "Cycle-42D" subsection for the full milestone table.
 *
 * NTSTATUS is deliberately discarded — there is no host-log path
 * safe to call from pre-WinMainCRT context, and an error-return
 * path that itself depends on additional kernel-export calls
 * would just re-introduce the fault surface this bypass was built
 * to escape. CONSEQUENCE (Codex round-1 R1.HIGH adopted
 * 2026-05-24): the post-run EEPROM byte is a LOWER BOUND on
 * milestones reached, NOT an exact "execution died here"
 * boundary. If any later marker write fails silently (kernel
 * pathological state, SMBus arbitration loss, retry exhaustion
 * inside HalWriteSMBusValue), the byte stays at the previous
 * successful marker even when the body code continued executing
 * past it. The cycle-42D real-Xbox interpretation must therefore
 * enumerate ALL the possibilities each (byte, count) shape
 * collapses to and use the witness.scan-self (reserved0,
 * reserved1) joint readback as the disambiguator where possible.
 * See `xbed_self_witness.h` "Cycle-42D" Honest framing subsection
 * for the full interpretation matrix.
 *
 * `__attribute__((no_stack_protector))` matches the cycle-42B
 * thunk's attribute on `witness_only_pre_winmain_crt_startup` for
 * the same reason — `__security_init_cookie` has not run yet when
 * this helper is called from the stage==6 bypass, so the
 * compiler-inserted cookie check must not fire. Inlining is also
 * fine for cookie correctness (no separate frame), but the
 * attribute guards against future changes where the compiler
 * chooses to spill or out-of-line. */
static inline __attribute__((no_stack_protector))
void self_witness_cycle42d_marker(uint8_t milestone)
{
    (void)HalWriteSMBusValue(
        (UCHAR)XBED_SELF_WITNESS_EEPROM_SMBUS_ADDR,
        (UCHAR)XBED_SELF_WITNESS_EEPROM_SCRATCH_OFF,
        FALSE,
        (ULONG)((UCHAR)(XBED_SELF_WITNESS_EEPROM_CYCLE42D_TAG_NIB |
                        (milestone & 0x0Fu))));
}

uintptr_t __attribute__((no_stack_protector))
xbed_self_witness_fire(uint32_t stage)
{
    /* CYCLE-42D STAGE-6 PRE-LIBC-SAFE MILESTONE MARKER BYPASS
     * (2026-05-24). See `xbed_self_witness.h` "Cycle-42D" Safety-
     * notes subsection for the full rationale + marker table +
     * post-run interpretation matrix.
     *
     * Cycle 42C real-Xbox run produced the INCONCLUSIVE signal
     * `(eeprom.scratch.read=0xA6, witness.scan-self count=0)`.
     * The byte-stayed-at-0xA6 evidence RULED OUT hypothesis (b)
     * "allocator-then-post-guard rejection" (the cycle-41c/d
     * guards preserve the cycle-39 sticky-flag setter, which
     * would have flipped EEPROM to 0xA4 from at least one of the
     * five WTNS-shim fires; the byte staying at 0xA6 means no
     * fire reached the sticky-flag setter line). The remaining
     * live cycle-42B failure modes narrow from {a, b, c} to {a,
     * "post-cycle-39-write-site fault that prevents the sticky
     * flag from being set on entry"}, dominated by leading
     * hypothesis (a) **pre-allocator fault inside the shim's
     * pre-libc context — most likely
     * `xbed_host_log_writef → vsnprintf`** at the function's
     * first host-log line (which assumes
     * `_PDCLIB_xbox_libc_init` has run, but the cycle-42B thunk
     * fires before WinMainCRTStartup runs libc init).
     *
     * Cycle 42D adds this stage==6 fast path BEFORE the existing
     * `xbed_host_log_writef("enter stage=%u", ...)` call. The
     * bypass:
     *   - sets `s_eeprom_scratch_attempted = 1` FIRST (1-instruction
     *     mov to a static int; commits us to the cycle-42D marker
     *     scheme by locking out the cycle-39 EEPROM write block in
     *     any subsequent stages-!=6 fire — otherwise a later
     *     .CRT$XXC stage=4 fire would overwrite our 0xBn marker
     *     with 0xA4);
     *   - writes EEPROM marker 0xB0 to confirm the bypass was
     *     entered (no later fire can overwrite this byte because
     *     of the sticky-flag set above);
     *   - skips every `xbed_host_log_writef` / `xbed_host_log_write`
     *     call (the suspected fault site);
     *   - replicates the cycle-42A 2-page allocation contract +
     *     cycle-41c symmetric phys-range guards + cycle-41d
     *     alignment guard + cycle-29 WTNS layout (magic + version +
     *     reserved0 + reserved1 at offset 0 of the FIRST page) +
     *     the existing stage-stamp + wbinvd sequence;
     *   - emits an EEPROM milestone marker after each major step
     *     (0xB1 = pre-alloc, 0xB2 = post-alloc, 0xB3 = post-phys,
     *     0xB4 = post-lower-guard, 0xB5 = post-upper-guard, 0xB6 =
     *     post-align-guard, 0xB7 = post-persist, 0xB8 = post-wipe,
     *     0xB9 = post-WTNS-stamp, 0xBA = full-completion plus 0xBB
     *     for the not-expected idempotent-reuse case);
     *   - returns early so control does NOT fall through to the
     *     existing stages-!=6 host-log tail.
     *
     * Stages != 6 (cycle-29 .CRT$XXC stage=4, .CRT$XCU stage=5,
     * in-main WTNS1 stage=1, WTNS2 stage=3 — and cycle-23 stages
     * are an unrelated shim, not this file) continue to execute
     * the unchanged cycle-42A code path BELOW. Their call sites
     * are all AFTER `WinMainCRTStartup → _PDCLIB_xbox_libc_init`,
     * so vsnprintf via `xbed_host_log_writef` is safe.
     *
     * The cycle-42B thunk's defensive pre-write of 0xA6 still
     * runs FIRST (before this function is called from the thunk).
     * The cycle-42D 0xB0 marker overwrites that 0xA6 on bypass
     * entry. If the bypass is never entered for any reason, the
     * EEPROM byte stays at 0xA6 — same INCONCLUSIVE shape as
     * cycle 42C, but distinguishable from cycle-42A (0xA4) and
     * from cycle-42D-success (0xBA) shapes.
     *
     * `__attribute__((no_stack_protector))` on this function: the
     * stage==6 dispatch may run before `__security_init_cookie`
     * (when called from the cycle-42B thunk in the cycle-42B+
     * build); the standard compiler-inserted stack-cookie check
     * would compare against `__security_cookie` which still holds
     * its linker-initialized value at that point. Mirrors the
     * cycle-42B thunk's attribute on
     * `witness_only_pre_winmain_crt_startup` for the same reason. */
    if (stage == XBED_SELF_WITNESS_STAGE_PRE_WINMAIN_CRT) {
        /* Lock out the cycle-39 EEPROM-write block for any
         * subsequent stages-!=6 fire. Single i386 mov to a static
         * int — atomic on uniprocessor OG Xbox, no observable
         * failure mode. Ordering: set the sticky flag BEFORE any
         * HalWriteSMBusValue so even if the very first marker
         * write fails, no later .CRT$XXC fire can overwrite the
         * cycle-42B thunk's 0xA6 pre-write with 0xA4 (which would
         * collapse the cycle-42D signal to a cycle-42A-shaped
         * INCONCLUSIVE outcome). */
        s_eeprom_scratch_attempted = 1;

        /* Marker 0xB0 — bypass entered. From here the post-run
         * EEPROM byte identifies the highest milestone whose marker
         * write SUCCEEDED — a LOWER BOUND on body progress (per
         * Codex R1.HIGH and R2.MED — silent failure of any later
         * 0xB? marker write leaves the byte at the previous
         * successful marker even if execution continued past it;
         * see `self_witness_cycle42d_marker` docstring above and
         * `xbed_self_witness.h` "Cycle-42D" Honest-framing
         * subsection). */
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_ENTRY);

        if (s_witness_page != 0) {
            /* Idempotent reuse path — not expected in the cycle-
             * 42B sequence (the thunk fires stage=6 exactly once
             * before WinMainCRTStartup; later .CRT$XXC/.CRT$XCU/
             * in-main fires use stages 4/5/1/3, not 6). Kept
             * defensive so a future caller that legitimately
             * fires stage=6 twice does not silently bypass the
             * stamp tick. */
            volatile uint32_t *vp_r = s_witness_page;
            uint32_t prev_counter_r = vp_r[3];
            vp_r[2] = ((uint32_t)XBED_SELF_WITNESS_TAG << 24) |
                      (stage & 0x00FFFFFFu);
            vp_r[3] = prev_counter_r + 1u;
            self_witness_wbinvd();
            self_witness_cycle42d_marker(
                XBED_SELF_WITNESS_C42D_M_REUSE_COMPLETION);
            return s_witness_phys;
        }

        /* First-call path — equivalent to the cycle-42A
         * stages-!=6 first-call block below but with all
         * `xbed_host_log_writef` calls REMOVED and EEPROM
         * milestone markers INSERTED after each major step.
         * Numeric thresholds (allocation size 0x2000, lower
         * phys-range 0x00010000, upper phys-range 0x04000000,
         * page-alignment mask 0xFFF, persist size 0x2000, page-
         * wipe span 0x2000) are bit-identical to the cycle-42A
         * stages-!=6 path. */
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_PRE_ALLOC);

        PVOID p_c42d = MmAllocateContiguousMemory(0x2000u);
        if (!p_c42d) {
            /* Alloc returned NULL. EEPROM is AT MOST 0xB1 (it
             * holds whatever the highest-numbered successful
             * marker write left there; could be 0xA6 if the 0xB0
             * write itself returned non-success). Per Codex R1
             * HIGH adopted, the EEPROM byte is a LOWER BOUND on
             * milestones reached, NOT an exact step boundary.
             * The shape `EEPROM in {0xA6, 0xB0, 0xB1} + count=0`
             * narrows the failure mode to "stage=6 calling
             * context reached at most the allocator call AND the
             * allocator rejected the request" — the cycle-22
             * hypothesis (c) "true allocator rejection" failing
             * AT the call site rather than mid-call. */
            return 0;
        }
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_POST_ALLOC);

        uintptr_t phys_c42d = (uintptr_t)MmGetPhysicalAddress(p_c42d);
        if (phys_c42d == 0) {
            MmFreeContiguousMemory(p_c42d);
            return 0;  /* EEPROM AT MOST 0xB2 (lower bound) */
        }
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_POST_PHYS);

        /* Cycle-41c symmetric lower phys-range guard — replicated
         * inline. Phys outside [0x00010000, 0x04000000) would be
         * invisible to the cycle-29 consumer scan in
         * `oracle-agent/commands.c::cmd_witness_scan_self`. */
        if (phys_c42d < 0x00010000u) {
            MmFreeContiguousMemory(p_c42d);
            return 0;  /* EEPROM AT MOST 0xB3 (lower bound) */
        }
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_POST_LOWER_GUARD);

        /* Cycle-41c symmetric upper phys-range guard. On retail
         * 64 MiB hardware this branch is a no-op (kernel cannot
         * return phys it does not have); preserved for symmetry
         * with the stages-!=6 path. */
        if (phys_c42d >= 0x04000000u) {
            MmFreeContiguousMemory(p_c42d);
            return 0;  /* EEPROM AT MOST 0xB4 (lower bound) */
        }
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_POST_UPPER_GUARD);

        /* Cycle-41d page-alignment guard. The non-`-Ex` ABI
         * documents no alignment guarantee; the cycle-29 consumer
         * scans on a fixed 0x1000 stride, so a sub-page-aligned
         * phys would be invisible. Replicated inline. */
        if ((phys_c42d & 0xFFFu) != 0u) {
            MmFreeContiguousMemory(p_c42d);
            return 0;  /* EEPROM AT MOST 0xB5 (lower bound) */
        }
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_POST_ALIGN_GUARD);

        /* Mark persistent so the allocation survives this XBE's
         * `HalReturnToFirmware(HalRebootRoutine)` exit + dashboard
         * chainload + relaunched agent process death. Persist size
         * matches the 0x2000u multi-page allocation above
         * (cycle-42A invariant). */
        MmPersistContiguousMemory(p_c42d, 0x2000u, TRUE);
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_POST_PERSIST);

        /* Canonicalize the virtual alias to kseg0 (virt = phys |
         * 0x80000000) — same pattern as the stages-!=6 path and
         * `oracle-agent/controller.c::s_allocate_fresh`. */
        volatile uint32_t *vp_c42d =
            (volatile uint32_t *)(phys_c42d | 0x80000000u);

        /* Zero the full 0x2000 allocation so the second page does
         * not accidentally match the WTNS magic predicate at the
         * consumer's next 0x1000-stride read (cycle-42A invariant). */
        for (uint32_t i = 0; i < 0x2000u / sizeof(uint32_t); i++) {
            vp_c42d[i] = 0;
        }
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_POST_WIPE);

        /* Stamp the cycle-29 WTNS header at offset 0 of the FIRST
         * page (cycle-42A invariant: magic + version only — the
         * reserved0/reserved1 stamp follows below). */
        vp_c42d[0] = XBED_SELF_WITNESS_MAGIC;
        vp_c42d[1] = XBED_SELF_WITNESS_VERSION;
        /* vp_c42d[2] (reserved0) and vp_c42d[3] (reserved1)
         * already zero from the page wipe above; the stamp below
         * sets them. */

        s_witness_page = vp_c42d;
        s_witness_phys = phys_c42d;
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_POST_WTNS_STAMP);

        /* Stamp `stage` into `reserved0` (with the 0xA4 tag byte)
         * and tick `reserved1` from 0 to 1 — bit-identical to the
         * fall-through stamp at the bottom of the stages-!=6
         * path. Stage is masked to 24 bits so the tag byte is
         * always 0xA4. The wbinvd flush guarantees visibility to
         * the relaunched agent's `witness.scan-self` reader
         * across the chainload. */
        uint32_t prev_counter_c42d = vp_c42d[3];
        vp_c42d[2] = ((uint32_t)XBED_SELF_WITNESS_TAG << 24) |
                     (stage & 0x00FFFFFFu);
        vp_c42d[3] = prev_counter_c42d + 1u;
        self_witness_wbinvd();
        self_witness_cycle42d_marker(
            XBED_SELF_WITNESS_C42D_M_FULL_COMPLETION);

        return s_witness_phys;
    }
    /* END CYCLE-42D STAGE-6 BYPASS — control falls through to the
     * unchanged cycle-42A stages-!=6 path below. */

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
         * CYCLE-42A BOUNDED VARIATION (multi-page redesign — branch
         * (c) discriminator). After cycle-41 scope was exhausted
         * (G0(c) PERSISTS across cache-policy 40+41a+41b, address-
         * range 41c, alignment 41d, and `-Ex`-vs-non-`-Ex` 41e), the
         * remaining live cycle-22 candidate is branch (c) "a
         * `size=0x1000`-specific interaction" — the ONE axis the
         * cycle-41 series could not vary inside the cycle-29 single-
         * page WTNS layout contract. Cycle 42 candidate A redesigns
         * the cycle-29 self-witness as a MULTI-PAGE allocation: this
         * call site now requests `0x2000u` bytes (= 2 pages, 8 KiB)
         * instead of cycle-41e's `0x1000u` (1 page). All other
         * cycle-41e invariants are preserved: non-`-Ex` ABI; cycle-39
         * EEPROM scratchpad sticky-flag breadcrumb; cycle-41c
         * symmetric phys-range guards; cycle-41d page-alignment
         * guard; `MmPersistContiguousMemory` + `wbinvd` stamp +
         * `phys | 0x80000000` cached-mirror alias. The WTNS magic +
         * version + reserved0 + reserved1 header still lives at
         * offset 0 of the FIRST page of the allocation; the second
         * page is zero-filled. The cycle-29 consumer
         * (`oracle-agent/commands.c::cmd_witness_scan_self`) scans
         * every 0x1000 page in the kseg0 window
         * `[0x80010000, 0x84000000]` for the WTNS magic — since only
         * the first page carries the magic, the consumer still
         * reports `count=1` per cycle-42A allocation (unchanged
         * count semantics vs cycles 29..41e); no consumer code
         * change is required.
         *
         * Why 0x2000u (NOT 0x4000u, NOT 0x8000u): smallest multi-
         * page size that actually tests branch (c). Larger sizes
         * would (i) increase the contiguous-RAM pressure on the
         * real-Xbox kernel allocator and risk a NEW failure mode
         * (allocator-pool exhaustion) confounding the size-axis
         * signal, and (ii) widen the blast radius of any future
         * recovery work without adding information. 0x2000u is the
         * minimal increment that genuinely varies the size axis
         * while keeping every other cycle-41e invariant intact.
         *
         * Discriminator semantics (cycle-42A real-Xbox readback):
         *   - EEPROM byte=0xA4 + `witness.scan-self count >= 1`
         *     with `(reserved0 >> 24) == 0xA4` and `reserved1 >= 1`
         *     → multi-page allocation succeeded → STRONG evidence
         *     FOR branch (c) being the failing constraint (single-
         *     page allocation from this calling context is rejected
         *     by the kernel; multi-page is accepted). Cycle-22
         *     leading hypothesis is FURTHER NARROWED toward branch
         *     (c). Not conclusive on its own: the kernel allocator
         *     may route single-page and multi-page contiguous
         *     requests through DIFFERENT internal code paths (e.g.
         *     size-bucketed free lists, separate pool arenas, or
         *     distinct minimum-size policies for contiguous-memory
         *     allocations from a pre-`main()` calling context). A
         *     2-page success could therefore reflect that internal
         *     code-path divergence rather than a real
         *     "kernel-validation rejects size=0x1000 specifically"
         *     rule. Codex round-2 finding adopted; the prior
         *     "2-page free run when no 1-page hole was available"
         *     example was logically impossible (any free
         *     2-page contiguous run trivially contains a free
         *     1-page hole), and the replacement covers the
         *     internally-distinct-code-path failure mode that the
         *     fragmentation example tried to gesture at.
         *   - EEPROM byte=0xA4 + `witness.scan-self count=0` →
         *     G0(c) PERSISTS at 0x2000u as well → STRONG evidence
         *     AGAINST size being the failing axis at the smallest
         *     multi-page step. The cycle-22 candidate set then
         *     forces a move to candidate B (custom XBE-header
         *     callback before `_start`) for the calling-context
         *     axis, since size has been varied as far as the
         *     cycle-29 layout reasonably permits without
         *     introducing confounds.
         *   - EEPROM byte=0x00 + count=0 → cycle-39 G0(a)+(b)
         *     regression (sticky-flag did not preserve byte) — NOT
         *     expected from cycle-42A's bounded diff; would
         *     indicate a build artifact problem, NOT a cycle-42A
         *     signal. Re-baseline EEPROM and re-run.
         *
         * Honest framing carried over from cycle-41e: cycle-42A is
         * NOT a pure single-axis discriminator either. Bumping
         * `size` from `0x1000u` to `0x2000u` while continuing to
         * call the non-`-Ex` ABI means the kernel still picks
         * Protect / placement / Alignment internally; the new
         * 2-page request may interact differently with the kernel's
         * pool-search policy (it has to find 2 contiguous physical
         * pages) and that interaction is itself uncharacterized on
         * this hardware. The cycle-42A SUCCESS interpretation is
         * therefore "STRONG-but-not-conclusive evidence FOR branch
         * (c)"; the FAILURE interpretation is "STRONG evidence
         * AGAINST size being the operative axis at the smallest
         * multi-page step." Neither outcome closes branch (c)
         * formally on its own — that would require sweeping size
         * across multiple multi-page steps OR cross-validating with
         * candidate B. Same envelope structure as cycle 41e's
         * Honest-framing paragraph below.
         *
         * The cycle-41E historical comment block is preserved
         * verbatim below for the cycle-29..41e history. The
         * load-bearing source delta for cycle-42A is the size
         * literal here (and at the matching `MmPersistContiguousMemory`
         * + page-wipe-loop sites below). Failure log line + persist
         * size + wipe-loop bound are updated to match.
         *
         * --- cycle-41e historical comment block (preserved) ---
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
        PVOID p = MmAllocateContiguousMemory(0x2000u);
        if (!p) {
            xbed_host_log_write(
                "xbed_self_witness: MmAllocateContiguousMemory "
                "(non-Ex; cycle-42A 0x2000 multi-page) failed; "
                "cycle-29 option (c) page not allocated");
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
                "phys=0x%08lx < 0x00010000; cycle-42A phys is below "
                "the agent reader's kseg0 scan window "
                "[0x80010000..0x84000000]; freeing self-witness "
                "allocation (defensive — symmetric lower-bound guard; "
                "guards the FIRST page of the 0x2000 multi-page "
                "allocation — the only page that carries the WTNS "
                "magic)",
                (unsigned long)phys);
            MmFreeContiguousMemory(p);
            return 0;
        }
        if (phys >= 0x04000000u) {
            xbed_host_log_writef(
                "xbed_self_witness: MmGetPhysicalAddress returned "
                "phys=0x%08lx >= 0x04000000; cycle-42A phys is above "
                "the agent reader's kseg0 scan window "
                "[0x80010000..0x84000000]; freeing self-witness "
                "allocation (defensive — symmetric upper-bound guard; "
                "should not fire on retail 64 MiB hardware; checks "
                "FIRST-page start phys only because the WTNS magic "
                "lives at offset 0 of the first page, so first-page "
                "visibility is sufficient for consumer scan-self to "
                "find the stamp)",
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
                "phys=0x%08lx not 0x1000-aligned; cycle-42A phys "
                "would be invisible to the agent reader's 0x1000-stride "
                "scan; freeing self-witness allocation (defensive — "
                "alignment guard; non-`-Ex` API has no documented "
                "returned-alignment guarantee; guard closes the "
                "consumer-stride blind spot for the FIRST page of "
                "the 0x2000 multi-page allocation regardless)",
                (unsigned long)phys);
            MmFreeContiguousMemory(p);
            return 0;
        }

        /* Mark persistent so the allocation survives this XBE's
         * `HalReturnToFirmware(HalRebootRoutine)` exit + the
         * dashboard chainload + the relaunched agent process death
         * boundary. Same flag the agent uses to keep XCTR alive.
         * Cycle-42A: persist size matches the 0x2000u multi-page
         * allocation above so BOTH pages survive the reboot; the
         * cycle-29 consumer only reads the first page (where the
         * WTNS magic + header live), but persisting the full
         * allocation matches the kernel's expectation that
         * MmFreeContiguousMemory will eventually release the same
         * range MmPersistContiguousMemory pinned. */
        MmPersistContiguousMemory(p, 0x2000u, TRUE);

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

        /* Zero the full allocation then stamp the 16-byte header
         * at the first page's offset 0. We deliberately do NOT
         * preserve any pre-existing contents on the returned phys:
         * the cycle-29 discriminator wants an unambiguous "this XBE
         * allocated this page" signal, and the agent's preserve-
         * branch concerns (cycle 27) don't apply here because no
         * one else is sharing this magic. Cycle-42A: wipe spans
         * BOTH pages (0x2000 bytes total) so the second page's
         * contents do not accidentally match the WTNS magic
         * predicate at the consumer's next 0x1000-stride read; the
         * first page receives the magic + version + reserved0 +
         * reserved1 stamp below. The consumer reports `count=1`
         * per cycle-42A allocation (single magic match per
         * allocation; second page is solid zero and fails the
         * `p[0] != SELF_WTNS_MAGIC` predicate). */
        for (uint32_t i = 0; i < 0x2000u / sizeof(uint32_t); i++) {
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
