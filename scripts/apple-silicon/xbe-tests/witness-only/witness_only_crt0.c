/*
 * witness-only — cycle-42B candidate B pre-WinMainCRTStartup thunk.
 *
 * Purpose (cycle-42B calling-context discriminator)
 * --------------------------------------------------
 * Cycles 40 + 41a + 41b + 41c + 41d + 41e + 42A varied the five
 * cycle-22 axes that are reachable from inside the cycle-29 .CRT$XXC
 * slot (cache policy, address range, alignment, -Ex-vs-non-Ex ABI,
 * single-vs-2-page size). Every variation produced OUTCOME G0(c):
 * the kernel allocator rejected the request and witness.scan-self
 * count stayed at 0. The cycle-22 candidate set inside the in-XBE /
 * lib-only oracle workflow scope is therefore EXHAUSTED.
 *
 * The remaining live cycle-22 axis is calling-context: maybe the
 * kernel allocator behaves differently when called from a context
 * that is NOT the .CRT$XXC slot. The strictest pre-CRT context we can
 * reach from user code is BEFORE __security_init_cookie / TLS size
 * computation / _PDCLIB_xbox_libc_init / _PDCLIB_xbox_run_pre_initi-
 * alizers — i.e. AS THE ABSOLUTE FIRST USER-CODE INSTRUCTION the XBE
 * runs, executed before nxdk's WinMainCRTStartup begins its own
 * sequence.
 *
 * Mechanism: redirect the XBE entry point from nxdk's
 * `WinMainCRTStartup` to `witness_only_pre_winmain_crt_startup` (this
 * file) via `-entry:witness_only_pre_winmain_crt_startup` in the
 * witness-only Makefile's NXDK_LDFLAGS. This function performs the
 * cycle-42B EEPROM-byte breadcrumb + the cycle-29 self-witness fire,
 * then calls nxdk's `WinMainCRTStartup`. The rest of the XBE's
 * runtime (security cookie / TLS / libc / .CRT$XX slots / main thread
 * spawn / main()) is bit-identical to a standard nxdk XBE because
 * nxdk's crt0.obj is still linked in and `WinMainCRTStartup` still
 * runs unmodified after our thunk returns control to it.
 *
 * Why this is the "candidate B equivalent" smaller-blast-radius path
 * ------------------------------------------------------------------
 * Cycle-41e closure framed candidate B as "custom XBE-header callback
 * before _start via nxdk/tools/cxbe/ modifications." Inspection of
 * `nxdk/tools/cxbe/Xbe.cpp:222` shows cxbe does not invent an entry
 * point — it propagates the entry-point virtual address from the input
 * PE's optional header. Therefore changing what code runs before
 * `WinMainCRTStartup` reduces to "change what symbol is at the PE
 * entry point" — a per-XBE link-flag concern, NOT a cxbe-source-edit
 * concern. The `-entry:` flag in nxdk-link / lld-link does exactly
 * that: it picks which symbol becomes the PE entry-point address. So
 * the smallest meaningful implementation of candidate B is a single
 * thunk function defined locally in witness-only, plus a one-line
 * NXDK_LDFLAGS addition. ZERO nxdk source edits required; ZERO
 * lib edits required (the existing cycle-29 self-witness shim from
 * `lib/xbed_self_witness.c` is reused verbatim).
 *
 * What is strictly EARLIER than .CRT$XXC (where cycle 29 fires today)
 * -------------------------------------------------------------------
 * nxdk's standard startup sequence (`nxdk/lib/pdclib/platform/xbox/
 * crt0.c:52-71`):
 *
 *   WinMainCRTStartup:                 <-- kernel jumps here at XBE entry
 *     __security_init_cookie()
 *     [TLS size computation -> _tls_index]
 *     _PDCLIB_xbox_libc_init()
 *     _PDCLIB_xbox_run_pre_initializers()   <-- walks .CRT$XX*
 *                                               (cycle-29 fires here)
 *     thrd_create(main_thread, main_wrapper, NULL)
 *     thrd_detach(...)
 *
 *   main_wrapper (separate thread):
 *     _PDCLIB_xbox_run_crt_initializers()   <-- walks .CRT$XI* + .CRT$XC*
 *                                               (cycle-29's XC slot fires here)
 *     main(0, ...)
 *
 * Our `witness_only_pre_winmain_crt_startup` runs BEFORE the very
 * first line of `WinMainCRTStartup` — i.e. before the security cookie
 * exists, before TLS is sized, before libc mutexes are init'd, before
 * any .CRT$X* slot runs, before any thread is created. This is the
 * absolute earliest legitimate user-code execution context inside the
 * XBE.
 *
 * What this fire does that cycle 29's .CRT$XXC fire does NOT
 * ----------------------------------------------------------
 * The cycle-29 self-witness shim
 * (`scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c`) is
 * idempotent: on first call it allocates a persistent contiguous
 * region and stamps the WTNS magic + initializes reserved1=1; on
 * subsequent calls (after a successful first call) it ticks
 * reserved1 by 1 and overwrites reserved0's stage byte. Today (cycle
 * 42A) the "first call" is the .CRT$XXC slot's fire (stage 4); the
 * .CRT$XCU slot and the in-main cycle-29 WTNS fires would reuse the
 * page IF the .CRT$XXC fire succeeded. The cycle-23 XCTR fires use
 * the SEPARATE `xbed_a4_witness_fire` shim and target the agent's
 * persistent XCTR buffer; they do NOT touch the WTNS page and do NOT
 * affect WTNS count / reserved0 / reserved1 (Codex R4.LOW adopted
 * 2026-05-24: prior wording erroneously listed XCTR fires as WTNS-
 * page consumers). Cycle 42B inserts a NEW first WTNS-shim call into
 * a strictly earlier calling context (the
 * `witness_only_pre_winmain_crt_startup` thunk, stage 6). If the
 * pre-WinMain fire successfully allocates, the subsequent four
 * WTNS-shim fires (.CRT$XXC + .CRT$XCU + in-main WTNS1 + WTNS2) each
 * tick reserved1 in lockstep order. The full WTNS-shim ordering
 * becomes:
 *
 *   i=1: PRE_WINMAIN_CRT (this file, stage 6)        <-- NEW
 *   i=2: .CRT$XXC        (witness-only/main.c, stage 4)
 *   i=3: .CRT$XCU        (witness-only/main.c, stage 5)
 *   i=4: in-main WTNS1   (witness-only/main.c, stage 1=MAIN_ENTERED)
 *   i=5: in-main WTNS2   (witness-only/main.c, stage 3=POST_MARKER0)
 *
 * The cycle-23 in-main XCTR fires (XCTR1 stage 1, XCTR2 stage 3) run
 * between .CRT$XCU and the in-main WTNS fires but are an independent
 * witness path; they do not interact with WTNS count / reserved1.
 *
 * Discriminator semantics (cycle-42B real-Xbox readback)
 * ------------------------------------------------------
 * The cycle-42B value proposition is the EEPROM byte + count split.
 * Honest framing (Codex round-1 R1.HIGH + R1.MED adopted, plus
 * Codex round-2 MED adopted 2026-05-24): the defensive pre-write of
 * EEPROM=0xA6 happens BEFORE the call to xbed_self_witness_fire and
 * proves only "the thunk executed far enough to hit the SMBus write."
 * It does NOT prove the allocator was reached, AND it does NOT prove
 * the allocator returned a usable page. A 0xA6+count=0 outcome
 * collapses three distinct failure modes that cycle 42B alone cannot
 * separate:
 *   (a) PRE-ALLOCATOR — a fault or early return inside
 *       `xbed_self_witness_fire` BEFORE `MmAllocateContiguousMemory`.
 *       The cycle-29 shim's only known pre-allocator fault source is
 *       vsnprintf inside `xbed_host_log_writef("enter stage=...")`
 *       at the function head AND inside the cycle-39 sticky-flag
 *       block's own host-log lines (both pre-libc-init in the
 *       cycle-42B calling context — `WinMainCRTStartup`'s
 *       `_PDCLIB_xbox_libc_init()` has not run yet — and therefore
 *       potentially unsafe to call). If vsnprintf faults, count=0
 *       not because the allocator failed but because we never tried.
 *   (b) ALLOCATOR-THEN-POST-GUARD — `MmAllocateContiguousMemory`
 *       successfully returned a page AND `MmGetPhysicalAddress`
 *       succeeded, but then either the cycle-41c symmetric
 *       phys-range guard (lower < 0x10000 / upper >= 0x04000000)
 *       OR the cycle-41d page-alignment guard ((phys & 0xFFFu) != 0)
 *       rejected the returned phys; the shim then calls
 *       `MmFreeContiguousMemory(p)` and returns 0 leaving
 *       `s_witness_page == 0` and the WTNS magic never stamped. The
 *       allocator DID run, but the kernel returned a phys outside the
 *       cycle-29 consumer's window or with sub-page alignment.
 *   (c) ALLOCATOR-REJECTED — `MmAllocateContiguousMemory` returned
 *       NULL silently or crashed. This is the true G0(c) shape from
 *       the pre-WinMainCRT calling context: the kernel allocator
 *       refuses the cycle-29 tuple even from the strictest pre-CRT
 *       caller.
 *
 * Without per-fault breadcrumbs that survive the pre-libc context,
 * cycle 42B cannot distinguish (a) / (b) / (c) from a single
 * 0xA6+count=0 reading. A future cycle 42C could split them by
 * adding stage-6-specific bypass behavior inside the shim (skip the
 * host-log calls + skip the post-allocation guards) OR by writing
 * additional EEPROM marker bytes at each pre-allocator and
 * post-allocator decision point. Until then, 0xA6+count=0 is
 * "thunk runs, downstream signal lost" — neither calling-context
 * confirmation nor elimination.
 *
 * The cycle-42B agent-side readback returns (count, reserved0,
 * reserved1) from `witness.scan-self`. To identify which fire was
 * the FIRST successful allocator (call it index k in the i=1..5
 * sequence above) we need BOTH reserved0_last_stage (the stage byte
 * of the MOST RECENT successful fire — the shim overwrites reserved0
 * on every successful call so this tells us only the latest stage,
 * not the earliest) AND reserved1 (the count of successful fires).
 * Codex R4.HIGH adopted 2026-05-24: reserved1 alone is insufficient
 * because the cycle-22-leading hypothesis is that execution dies
 * partway through startup — a crash between fires can produce the
 * same reserved1 value as a successful run that started at a later
 * fire index.
 *
 * Given (reserved0_last_stage, reserved1) when count >= 1:
 *
 *   Let M = the i-index of the i-th fire whose stage matches
 *           reserved0_last_stage. M is the LAST fire that reached
 *           the WTNS stamping path; execution either died after M
 *           (no fire i > M ran) or i > M ran but their allocations
 *           failed without touching reserved0 (impossible here since
 *           after the first success the shim's reuse branch always
 *           ticks reserved0 + reserved1).
 *   Then k = M - reserved1 + 1 = i-index of the first successful
 *           allocator.
 *
 *   Stage byte -> M:  6 -> 1   4 -> 2   5 -> 3   1 -> 4   3 -> 5
 *
 *   reserved0 low byte | M | reserved1 | k | meaning
 *   -------------------|---|-----------|---|------------------------
 *   tag|6 = 0xA6       | 1 | 1         | 1 | only pre-WinMain reached; pre-WinMain succeeded; execution died before .CRT$XXC. STRONG signal: pre-WinMain calling context yielded the first usable allocation; cycle-22 hypothesis NARROWS to "pre-WinMain calling context works; .CRT$XX* contexts fail" — but ALSO implies a new failure mode after the WinMainCRTStartup() call returns from our thunk (since the cycle-35 .CRT$XXC slot inside _PDCLIB_xbox_run_pre_initializers is reached by every nxdk-built XBE that runs at all).
 *   tag|4 = 0xA4       | 2 | 2         | 1 | pre-WinMain + .CRT$XXC both succeeded; execution died before .CRT$XCU. pre-WinMain was first allocator. (Note: this row's stage byte coincidentally matches cycle 42A's G0(c) EEPROM byte but the count>=1 distinguishes it.)
 *   tag|4             | 2 | 1         | 2 | only .CRT$XXC succeeded (pre-WinMain failed). Cycle 22 hypothesis: .CRT$XX* context works; pre-WinMain context doesn't. SURPRISING given pre-WinMain is the strictly earlier context, but possible if e.g. HalWriteSMBusValue's NTSTATUS path is unsafe pre-libc and aborts the thread.
 *   tag|5 = 0xA5       | 3 | 3         | 1 | pre-WinMain + .CRT$XXC + .CRT$XCU all succeeded; execution died before in-main WTNS1.
 *   tag|5             | 3 | 2         | 2 | .CRT$XXC + .CRT$XCU succeeded; pre-WinMain failed.
 *   tag|5             | 3 | 1         | 3 | only .CRT$XCU succeeded.
 *   tag|1 = 0xA1       | 4 | 4         | 1 | pre-WinMain + .CRT$XXC + .CRT$XCU + WTNS1 succeeded; execution died before WTNS2.
 *   tag|1             | 4 | 3         | 2 | .CRT$XXC + .CRT$XCU + WTNS1 succeeded.
 *   tag|1             | 4 | 2         | 3 | .CRT$XCU + WTNS1 succeeded.
 *   tag|1             | 4 | 1         | 4 | only WTNS1 succeeded.
 *   tag|3 = 0xA3       | 5 | 5         | 1 | ALL FIVE WTNS-shim fires reached AND pre-WinMain was the first successful allocator. CLEANEST cycle-42B success signal — STRONG evidence FOR calling-context being the failing axis in cycles 35..42A. Not formally conclusive on its own (the pre-WinMain calling context differs from .CRT$X* on multiple axes simultaneously: no security cookie / no TLS / no libc / no thread / different return-address-on-stack); a single observation does not enumerate which property is the operative one. But a definitive demonstration that ONE calling context produced a usable allocation when seven prior cycles' .CRT$XX* + in-main calling contexts did not.
 *   tag|3             | 5 | 4         | 2 | .CRT$XXC was the first successful allocator; pre-WinMain failed. Gradient signal: pre-WinMain context worse than .CRT$XXC.
 *   tag|3             | 5 | 3         | 3 | .CRT$XCU first allocator.
 *   tag|3             | 5 | 2         | 4 | WTNS1 first allocator.
 *   tag|3             | 5 | 1         | 5 | WTNS2 first allocator.
 *
 * The EEPROM byte at 0xFF is 0xA6 (the thunk's defensive pre-write)
 * regardless of any of these scenarios as long as the thunk reached
 * its `HalWriteSMBusValue` line. The shim's own cycle-39 sticky-flag
 * EEPROM write was already gated off by the thunk's pre-write (the
 * shim writes 0xA0 | first-call-stage, which for the pre-WinMain
 * fire is also 0xA0 | 6 = 0xA6 — so the byte stays 0xA6 whether one
 * or both writes succeed).
 *   0xA6        | 0                       | INCONCLUSIVE for the allocator question. The thunk reached the SMBus write (so the entry-point override took AND the SMBus path is usable from pre-libc context), but the cycle-29 shim could have stopped at vsnprintf, at the sticky-flag block, at a phys-range guard, at the alignment guard, OR at the allocator itself. Without further instrumentation cycle 42B alone cannot tell "calling-context ELIMINATED as axis" from "vsnprintf or some other pre-allocator guard faulted in pre-libc context." Cycle 42C+ would need a stage-6-specific bypass path inside the shim (or a duplicate, log-free, guard-free, allocator-only variant of the shim invoked from this thunk) to break the ambiguity. Until then, this outcome should NOT be treated as elimination of the calling-context axis; it is "thunk runs, downstream signal lost."
 *   0x00        | 0                       | regression — our defensive SMBus write did NOT land. Either (i) the entry-point override did not take (Makefile / link-flag issue); (ii) HalWriteSMBusValue itself faulted from pre-libc context (would be a NEW failure mode worth surfacing); (iii) the thunk faulted before the SMBus write (cookie/TLS interaction not predicted by the risk analysis below). NOT expected; would force a build artifact / link-flag re-validation against the PE AddressOfEntryPoint disassembly. In this regression case the standard cycle-35 .CRT$XXC fire would also never run, so no 0xA4 outcome is reachable from the cycle-42B build.
 *
 * Notably absent: a 0xA4 + count=0 row. Cycle 42A's `0xA4 + count=0`
 * outcome is unreachable in the cycle-42B build (Codex round-1 R1.MED
 * adopted) because if the thunk reaches the cycle-35 .CRT$XXC fire,
 * our defensive write at the head of the thunk has already landed
 * 0xA6; the cycle-39 sticky-flag inside xbed_self_witness.c is set on
 * the FIRST call (the pre-WinMainCRT call) so the .CRT$XXC fire's
 * EEPROM write site short-circuits and cannot overwrite 0xA6 with
 * 0xA4. Conversely, if the thunk faults before our defensive write,
 * the .CRT$XXC fire never runs either (control never reaches
 * WinMainCRTStartup) — so a post-run 0xA4 reading would be impossible
 * regardless of when in the thunk the fault occurred.
 *
 * Defensive double-write design (vsnprintf risk mitigation)
 * ---------------------------------------------------------
 * `xbed_self_witness_fire` (lib/xbed_self_witness.c:52) calls
 * `xbed_host_log_writef("xbed_self_witness: enter stage=%u", ...)` as
 * its first instruction. That uses vsnprintf, which lives in libpdclib
 * and may depend on libc init having run. Cycle 35's .CRT$XXC slot
 * fires AFTER libc_init so this works there; cycle 42B's thunk fires
 * BEFORE libc_init so vsnprintf could fault.
 *
 * To make the EEPROM-byte discriminator robust against a vsnprintf
 * fault in xbed_self_witness_fire, this thunk writes the EEPROM byte
 * directly via `HalWriteSMBusValue` BEFORE calling
 * xbed_self_witness_fire. The cycle-39 sticky-flag (
 * `s_eeprom_scratch_attempted` inside lib/xbed_self_witness.c) is a
 * SEPARATE flag from ours; both writes target offset 0xFF with the
 * SAME byte value (0xA0 | 6 = 0xA6) so they converge — whether
 * vsnprintf faults or not, the post-run EEPROM byte is deterministic.
 *
 * Risk surface of the pre-WinMainCRT call
 * ---------------------------------------
 * 1. Stack-protector cookie: `__security_init_cookie` has not run
 *    yet, so `__security_cookie` holds whatever value the linker
 *    placed there (typically zero). Function prologues store
 *    `cookie XOR esp` in the frame and epilogues compare on exit. If
 *    the cookie is unchanged between prolog and epilog (which it is
 *    in our short witness path — nothing touches `__security_cookie`)
 *    AND esp matches, the comparison passes trivially. No stack-
 *    overflow detection happens during the cycle-42B fire, but no
 *    false-positive crash either. The thunk itself uses
 *    `__attribute__((no_stack_protector))` for belt-and-suspenders.
 *
 * 2. TLS access: not used. `xbed_self_witness_fire` reads/writes
 *    only file-static globals and the allocated contiguous page.
 *    `HalWriteSMBusValue` is a kernel export with no TLS dependency.
 *
 * 3. Libc dependence: minimal. `HalWriteSMBusValue` is a bare kernel
 *    export. `xbed_self_witness_fire`'s body uses kernel exports
 *    (`MmAllocateContiguousMemory`, `MmPersistContiguousMemory`,
 *    `MmGetPhysicalAddress`, `MmFreeContiguousMemory`, `memset`
 *    intrinsic, `wbinvd` inline asm) — none of these need libc init.
 *    The ONLY libc-touching call is the `xbed_host_log_writef` at
 *    the head of xbed_self_witness_fire which uses vsnprintf. If
 *    that faults, the EEPROM byte is still 0xA6 from our defensive
 *    pre-call write, so the discriminator answer survives.
 *
 * 4. Call to WinMainCRTStartup: nxdk's standard
 *    `WinMainCRTStartup` is still linked in (since we did not
 *    override the symbol — we just renamed the entry point). After
 *    our thunk's witness work completes, control jumps into nxdk's
 *    standard startup sequence which is bit-identical to a non-
 *    cycle-42B XBE. From that point on, the runtime is unaltered:
 *    `__security_init_cookie` runs, TLS is sized, libc is init'd,
 *    `.CRT$XX*` slots fire (including cycle-35's stage-4 fire which
 *    is now a SUBSEQUENT call to the idempotent shim), the main
 *    thread is spawned, main() runs the cycle-23 + cycle-29 + cycle-
 *    31 instrumentation as in cycles 35..42A.
 *
 * Preserved invariants
 * --------------------
 * - cycle-23 lockstep contract (`lib/xbed_a4_witness.{c,h}`): UNCHANGED.
 * - cycle-29 self-witness shim (`lib/xbed_self_witness.{c,h}`): UNCHANGED;
 *   cycle-42A 2-page allocation + cycle-41c phys-range guards +
 *   cycle-41d alignment guard + cycle-39 EEPROM sticky-flag gate ALL
 *   preserved in their cycle-42A state.
 * - cycle-35 .CRT$XXC + .CRT$XCU slot fires in `witness-only/main.c`:
 *   UNCHANGED; they now run as 2nd/3rd calls to the idempotent shim
 *   instead of 1st/2nd as in cycle 42A.
 * - cycle-31 visual breadcrumb path: UNCHANGED.
 * - cycle-39 EEPROM scratchpad mechanism: UNCHANGED in the shim; this
 *   file adds a SEPARATE defensive pre-write to the same byte with
 *   the same encoding (0xA0 | stage) so post-run EEPROM read is
 *   deterministic regardless of vsnprintf behavior in pre-libc
 *   context.
 * - oracle-agent (`oracle-agent` tree): UNCHANGED; existing
 *   `witness.scan-self` + `eeprom.scratch.read` verbs already report
 *   the cycle-42B signal shape without modification.
 * - All eight default-on Apple Silicon flags (project rule #11):
 *   UNCHANGED (this file is XBE-side instrumentation only, no host
 *   xemu source touched).
 *
 * Why a separate file (not adding to witness-only/main.c)
 * -------------------------------------------------------
 * The `-entry:` link flag picks a symbol from any linked object. Keep
 * the thunk + its rationale in its own file so `main.c`'s rationale
 * block (which is already large — cycles 25/29/31/35) does NOT have
 * to absorb the cycle-42B history; the cycle-29 .CRT$XX/XCU slots in
 * main.c remain undisturbed; and the file boundary makes the cycle-
 * 42B addition trivially identifiable in `git log --follow` for
 * future-cycle work that needs to roll back to a non-cycle-42B
 * baseline.
 */

#include <stdint.h>
#include <winnt.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xbed_self_witness.h"

/* Stage code for the cycle-42B pre-WinMainCRT fire. Extends the
 * cycle-35 stage-code namespace (4 = .CRT$XXC, 5 = .CRT$XCU) without
 * colliding with cycle-23's stage codes (1, 2, 3). Local to this XBE
 * (matching the cycle-35 pattern in witness-only/main.c) so the
 * cycle-23 lockstep contract on `xbed_a4_witness.h` stage codes
 * stays intact. The `xbed_self_witness_fire` shim accepts any
 * uint32_t in the low 24 bits of `reserved0`; the agent's
 * `witness.scan-self` reader filters only on the 0xA4 tag in the
 * high byte. */
#define WITNESS_ONLY_STAGE_PRE_WINMAIN_CRT  6u

/* nxdk's standard PE entry-point symbol. Declared extern so this
 * file can call it after the cycle-42B witness fire completes.
 * Defined in `nxdk/lib/pdclib/platform/xbox/crt0.c:52`. */
extern void WinMainCRTStartup(void);

/* Cycle-42B-local sticky flag for the defensive EEPROM write.
 * Independent of `lib/xbed_self_witness.c`'s `s_eeprom_scratch_attempted`
 * flag. Both flags gate writes to the same EEPROM byte (offset 0xFF)
 * with the same encoded value (0xA0 | stage); the writes converge so
 * the post-run EEPROM byte is deterministic whether vsnprintf inside
 * xbed_self_witness_fire faults from pre-libc context or not. */
static int s_witness_only_42b_eeprom_attempted = 0;

/* Cycle-42B custom XBE entry-point thunk. Selected via
 * `-entry:witness_only_pre_winmain_crt_startup` in the witness-only
 * Makefile's NXDK_LDFLAGS. Runs BEFORE nxdk's standard
 * `WinMainCRTStartup` — i.e. before `__security_init_cookie`, TLS
 * size computation, `_PDCLIB_xbox_libc_init`, `_PDCLIB_xbox_run_pre_-
 * initializers`, and any `.CRT$X*` slot fire. Calls nxdk's
 * standard `WinMainCRTStartup` after the cycle-42B witness work
 * completes so the rest of the runtime is bit-identical to a
 * standard nxdk XBE.
 *
 * `no_stack_protector`: matches nxdk crt0's attribute on
 * `WinMainCRTStartup` for the same reason — `__security_init_cookie`
 * has not run, so the compiler-inserted cookie check must not fire.
 *
 * Falls off the end after `WinMainCRTStartup` returns. nxdk's
 * `WinMainCRTStartup` itself falls off after `thrd_detach`; control
 * exits the entry thread, but the new main_wrapper thread keeps the
 * process alive until `main()` returns (which call `exit`). Same
 * shape as the standard nxdk XBE entry, no behavioral divergence. */
void __attribute__((no_stack_protector))
witness_only_pre_winmain_crt_startup(void)
{
    /* Defensive EEPROM write — fires BEFORE xbed_self_witness_fire so
     * the cycle-42B discriminator byte (0xA6) lands regardless of
     * whether vsnprintf inside the shim faults from pre-libc context.
     * Bit-identical encoding to the cycle-39 sticky-flag write inside
     * lib/xbed_self_witness.c: same EEPROM SMBus address, same byte
     * offset, same `(0xA0 | stage)` tag pattern. Both writes converge
     * on 0xA6 (= 0xA0 | 6) so the post-run EEPROM byte is the same
     * whether one or both writes succeed. */
    if (!s_witness_only_42b_eeprom_attempted) {
        s_witness_only_42b_eeprom_attempted = 1;
        (void)HalWriteSMBusValue(
            (UCHAR)XBED_SELF_WITNESS_EEPROM_SMBUS_ADDR,
            (UCHAR)XBED_SELF_WITNESS_EEPROM_SCRATCH_OFF,
            FALSE,
            (UCHAR)(XBED_SELF_WITNESS_EEPROM_TAG_NIB |
                    (WITNESS_ONLY_STAGE_PRE_WINMAIN_CRT & 0x0Fu)));
    }

    /* Cycle-42B pre-WinMainCRT witness fire. On real Xbox this is the
     * FIRST call to the cycle-29 self-witness shim — the cycle-35
     * .CRT$XXC + .CRT$XCU slots and the in-main cycle-23 + cycle-29
     * fires will reuse the page this call allocates (via the shim's
     * idempotent `s_witness_page != 0` early return). Return value
     * intentionally discarded — the post-run readback recovers it
     * via the agent's `witness.scan-self` verb instead of a host-log
     * that might not survive a vsnprintf fault. */
    (void)xbed_self_witness_fire(WITNESS_ONLY_STAGE_PRE_WINMAIN_CRT);

    /* Call into nxdk's standard PE entry-point. From this point
     * the runtime is bit-identical to a non-cycle-42B nxdk XBE:
     * security cookie + TLS + libc + .CRT$X* + main thread + main(). */
    WinMainCRTStartup();
}
