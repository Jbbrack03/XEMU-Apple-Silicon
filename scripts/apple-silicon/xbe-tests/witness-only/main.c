/*
 * witness-only — cycle-25 minimal A.4 witness-mechanism viability
 * discriminator + cycle-29 option (c) self-allocated witness + cycle-31
 * option (d) on-screen visual breadcrumb. NO pbkit, NO NV2A
 * class-object setup, NO xbed_init, NO file I/O. Cycle 31 ADDED a
 * single pbkit-free XVideoSetMode + framebuffer-paint call path
 * (used purely for breadcrumb display); the cycle-25 "NO
 * XVideoSetMode" invariant therefore no longer holds. The remaining
 * cycle-25 invariants (no pbkit, no NV2A class objects, no
 * xbed_init, no file I/O) are intact.
 *
 * Why this XBE exists
 * -------------------
 * Cycle 24 (2026-05-22) ran the cycle-23 A.4 witness on real Xbox by
 * deploying the cycle-23 image-blit binary (`xbed_a4_witness.{c,h}`
 * linked + 2 call sites in `image-blit/main.c` bracketing
 * `image_blit_marker(0, ...)`). The chainload hung the Xbox for
 * 928.3 s of continuous polling on FTP/21 + agent/9001 + ICMP ping —
 * a NEW failure mode that did NOT appear in cycles 19/20/21 (which
 * reproduced a stable 22.3..22.4 s chainload→FTP-back gap across 5
 * attempts with the pre-witness image-blit binary). The only change
 * between cycle-21 image-blit and cycle-23 image-blit is ~196 LOC of
 * witness instrumentation, which is itself weak-but-real evidence
 * that *something* in that instrumentation is executing in cycle-23
 * image-blit that did not execute in cycle-21 image-blit.
 *
 * Cycle 24 promoted NEW hypothesis #5 as top-priority:
 *   "The kseg0-scan witness mechanism may be real-Xbox-unsafe from a
 *    non-agent process context (different load address, different
 *    process, different RPC state). The MmGetPhysicalAddress per-page
 *    gate guards against page-fault on unmapped pages but does NOT
 *    guard against returning non-zero for a mapped-but-MMIO-aliased
 *    page whose read hangs the bus."
 *
 * Cycle 25 (this XBE) is the cheapest discriminator. It strips
 * image-blit's `main()` down to ONLY the witness calls + sleeps +
 * reboot. If THIS XBE reboots cleanly on real Xbox and leaves an
 * orphan `oracle_ctrl_buffer` with `reserved[0] == 0xA4000003`
 * (POST_MARKER0 = last stage stamped), the witness mechanism IS
 * real-Xbox-safe IN THIS MINIMAL XBE — image-blit's hang is in
 * code that is ABSENT from witness-only. That ABSENT set is
 * pbkit + NV2A + xbed_init + xbed_render_loop_then_capture AND
 * the `image_blit_marker(0, ...)` helper itself (cycle-25
 * substitutes a passive `Sleep(500)` for it). Independently
 * excluding the marker helper requires a follow-on cycle that
 * actually runs the marker between the two fires. If it hangs
 * the Xbox identically to cycle 24, the witness mechanism itself
 * is real-Xbox-incompatible and needs a redesign (EEPROM scratchpad
 * / non-MMIO-aliased RAM / abandon in-XBE witness).
 *
 * Discriminator semantics (cycle-26 real-Xbox run, NOT this session)
 * ------------------------------------------------------------------
 * NOTE on what this XBE actually isolates. Cycle-25 replaces
 * image-blit's `image_blit_marker(0, ...)` fopen call between the
 * two witness fires with a passive `Sleep(500)`. A successful cycle-
 * 26 run therefore proves the witness mechanism is real-Xbox-safe in
 * THIS minimal context, but it does NOT exercise the marker helper
 * and so does NOT independently exclude the marker helper as a
 * contributor to image-blit's hang. Any "marker helper is safe"
 * claim requires a separate cycle that runs the actual marker
 * helper between the two fires (cycle 26.5 / cycle 27 candidate).
 *
 *   Reboot ~5..15 s + orphan reserved[0] == 0xA4000003
 *       → witness mechanism IS real-Xbox-safe in this minimal XBE
 *       → image-blit's hang is in code that is ABSENT from
 *         witness-only — which includes pbkit / NV2A / xbed_init /
 *         xbed_render_loop_then_capture AND the marker helper
 *         `image_blit_marker(0, ...)` itself. Cycle-22 "pre-main
 *         crash" leading hypothesis is INVALIDATED. Marker-helper
 *         exclusion requires a follow-on cycle.
 *       → cycle 27 splits image-blit's instrumentation set across
 *         multiple smaller discriminator XBEs to localize.
 *
 *   Hangs identically to cycle 24 (no FTP/21 / agent/9001 / ping)
 *       → witness mechanism itself is real-Xbox-incompatible from a
 *         non-agent process context
 *       → redesign required.
 *
 *   Reboot + orphan reserved[0] == 0xA4000001 (MAIN_ENTERED only)
 *       → witness fires once but second fire hangs the Xbox
 *       → CPU-state corruption between fires
 *       → less likely; worth surfacing.
 *
 * Hard precondition (cycle-26): baseline `witness.scan` BEFORE
 * chainload must show exactly ONE live `oracle_ctrl_buffer` with
 * `reserved[0] == 0`. If multiple orphans pre-exist (e.g. from a
 * prior cycle-26 attempt in the same power session), Hermes must
 * power-cycle the Xbox first — the persistent buffer is
 * `MmPersistContiguousMemory`-tagged and survives soft reset but NOT
 * power-off.
 *
 * Why match image-blit's linking pattern exactly
 * ----------------------------------------------
 * lib.mk pulls `xbed_a4_witness.c` AND `xbed_runtime.c` (host-log
 * channel) AND `xbed_capture.c`, `xbed_input_synth.c`, `xbed_texture.c`
 * into SRCS. We could trim that list for a strictly smaller binary,
 * but we deliberately do NOT — keeping the linking pattern identical
 * to image-blit isolates the cycle-25-vs-cycle-21-image-blit delta
 * to (a) the absence of NV2A class-object setup / pbkit init / draw
 * loop and (b) main()'s body shrinking to ~5 statements. If
 * witness-only reboots cleanly while image-blit hangs, the delta is
 * the absent code — pbkit / NV2A / xbed_init / draw AND the
 * `image_blit_marker(0, ...)` helper itself — not the witness. The
 * marker helper is in the absent set because cycle-25 substitutes a
 * passive `Sleep(500)` for it; independently excluding it as a
 * contributor requires a follow-on cycle. If witness-only hangs, the
 * delta is the witness itself (or pre-main paths sensitive to
 * xbed_a4_witness.c's linked .text size, which is identical here to
 * image-blit). The linked-but-unused functions (xbed_init et al.)
 * only execute if called; their static .text cost is the controlled
 * invariant.
 *
 * Cycle-29 addendum (option (c), 2026-05-23)
 * ------------------------------------------
 * Cycle 28 closure (commit c77b509149) observed
 * `count=1 live=1 reserved0=0 reserved1=0` after a cycle-25
 * `witness-only` chainload + cycle-27 preserve-branch oracle-agent
 * re-launch. The cycle-27 preserve gate is correctly wired (Codex
 * 3-round green); a landed A.4 stamp would survive it. Therefore
 * "no A.4 stamp landed on the agent's XCTR buffer." Three live
 * causes (carried from cycle-28 closure):
 *   (α) `xbed_a4_witness::a4_candidate_ok` kseg0 scan from this
 *       non-agent process context does not find the agent's XCTR
 *       buffer.
 *   (β) Scan finds it but the write faults silently.
 *   (γ) `main()` never reaches the fire calls (cycle-22 leading
 *       hypothesis).
 *
 * Cycle 29 adopts option (c) from the cycle-27 closure catalog:
 * `main()` additionally calls `xbed_self_witness_fire(stage)` (new
 * shared lib `lib/xbed_self_witness.{c,h}`) which on its first call
 * allocates the diag XBE's OWN persistent contiguous page via
 * `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory` and
 * stamps a unique 'WTNS' magic + stage + counter at known offsets
 * in that page. The relaunched agent's new `witness.scan-self`
 * verb scans kseg0 for the 'WTNS' magic.
 *
 * Ordering decision (Codex round-1 high finding #1, adopted): the
 * cycle-29 self-witness fires execute AFTER the cycle-23 XCTR
 * fires. The cycle-23 path therefore executes under conditions
 * bit-identical to cycle 25 (same instruction sequence + same
 * kernel state on entry), so the cycle-30 XCTR readback is
 * properly comparable to cycle 28's D-cycle-27 result. The
 * cycle-29 binary is therefore NOT a strict superset of cycle 25
 * for the post-cycle-23-fires-to-reboot window — that window
 * gains new kernel-allocator activity — but IS bit-identical to
 * cycle 25 up to and including the second cycle-23 fire.
 *
 * Discriminator semantics (Codex round-1 high finding #2,
 * adopted): a cycle-30 `witness.scan-self` hit proves only that
 * `main()` ran AND that writes to a SELF-OWNED persistent page
 * survive the chainload. It does NOT exercise the failing write
 * into the agent's XCTR page; cause (β) "scan finds the agent
 * buffer but the write faults silently" therefore REMAINS LIVE
 * even on a successful cycle-29 readback. A cycle-31+ option (b)
 * (agent-side prior-phys dump + read-only kseg0 dump verb) is
 * required to break α-vs-β. Cycle 29 is positioned narrowly as a
 * γ-only discriminator:
 *
 *   - witness.scan-self finds a stamped page →
 *     `main()` ran AND the self-allocated page is findable from
 *     non-agent context → (γ) INVALIDATED. (α) and (β) BOTH
 *     remain live (the cycle-23 scan-from-non-agent-context path
 *     was not exercised by this witness — it stamped its own
 *     page, not the agent's XCTR page — so α "scan can't find
 *     XCTR" and β "scan finds XCTR but write faults" cannot be
 *     distinguished from this readback alone).
 *   - witness.scan-self finds nothing AND witness.scan still
 *     shows `count=1 live=1 reserved0=0` → no stamp anywhere →
 *     (γ) leading; cycle 31+ should pursue option (d)
 *     (on-screen breadcrumb) for an independent main()-runs
 *     verification.
 *
 * Cycle-31 addendum (option (d), 2026-05-23)
 * ------------------------------------------
 * Cycle 30 (closure commit dfe1480cba) ran the cycle-29 build on real
 * Xbox; outcome E2 = `witness.scan = D-cycle-27` AND
 * `witness.scan-self = count=0`. Per the cycle-30 discriminator table
 * this makes (γ) "main() never reaches the fire calls" LEADING and
 * re-strengthens the cycle-22 pre-main-crash hypothesis, but does NOT
 * fully corroborate it — main() could equally crash AFTER entering
 * but BEFORE the first fire (between cycle-25's host-log breadcrumb
 * write and `xbed_a4_witness_fire(MAIN_ENTERED)`). Cycle 31 adds the
 * cheapest independent answer to "did main() execute at all on real
 * Xbox?": a pbkit-free `XVideoSetMode(640, 480, 32, REFRESH_DEFAULT)`
 * + a direct CPU paint of distinguishable horizontal stripes into the
 * resulting linear framebuffer, captured during a follow-on cycle by
 * the existing composite-capture leg (MS2109 USB stick + ffmpeg
 * AVFoundation). The breadcrumb runs BEFORE the cycle-23 fires; if
 * stripe 0 is visible on composite, main() reached its first executable
 * instruction and (γ) is INVALIDATED. Absence of stripe 0 narrows the
 * live causes to (γ.0) execution never entered `main()` at all (CRT
 * `_start` / `__security_init_cookie` / static-init crash), or (γ.1)
 * `XVideoSetMode` itself crashed before painting completed (kernel
 * display init not safe in this context). Either γ.0 or γ.1
 * strengthens cycle-22 further than cycle 30 could; a γ-invalidating
 * stripe-0 presence redirects the next cycle toward (α)/(β) and
 * re-elevates option (b) from the cycle-29 closure catalog.
 *
 * Cycle 31 also paints additional stripes AFTER each subsequent
 * checkpoint (after `xbed_a4_witness_fire(MAIN_ENTERED)` returns,
 * after `xbed_a4_witness_fire(POST_MARKER0)` returns, after
 * `xbed_self_witness_fire(MAIN_ENTERED)` returns, after
 * `xbed_self_witness_fire(POST_MARKER0)` returns). Each stripe uses a
 * distinct ARGB8888 color so a single captured frame near reboot time
 * directly indicates the deepest checkpoint reached. Each paint is
 * just memory writes into a contiguous-allocated, write-combined
 * framebuffer page — the marginal risk surface is concentrated in the
 * single `XVideoSetMode` call. The final `Sleep(2000)` extends cycle
 * 25's 500 ms drain to ~60 captured frames at 30 fps composite so the
 * cycle-32 analyzer has redundant samples in case of capture-card
 * frame drops.
 *
 * Cycle 31 is therefore additive but NOT a strict superset of cycle 25
 * OR cycle 29 — the new `XVideoSetMode` path runs BEFORE the cycle-23
 * fires, so a cycle-22-style crash that happens to fall inside
 * `XVideoSetMode` would prevent BOTH the cycle-23 XCTR fires AND the
 * cycle-29 WTNS fires from running. Cycle-31 ordering is required by
 * the discriminator's purpose ("did main() execute past XVideoSetMode
 * BEFORE we even attempt the fires?"). The trade-off is acceptable:
 * if a cycle-32 readback shows `WTNS count=0` AND stripe 0 absent, the
 * crash site is at-or-before XVideoSetMode (which is strictly earlier
 * than cycle 30's narrowing); if `WTNS count=0` AND stripe 0 present,
 * the crash is between XVideoSetMode return and the first fire (a NEW
 * window cycle 30 could not isolate).
 *
 * Cycle-35 addendum (pre-main breadcrumb via .CRT$X* slots, 2026-05-23)
 * --------------------------------------------------------------------
 * Cycle 34 (cycle-32 redo on real Xbox vs cycle-31 binary; closure
 * commit `b5327d4d17`) observed OUTCOME F4 = zero stripes visible
 * across 22 NTSC-correct composite snapshots over t+0.07s..t+24.17s
 * post-runxbe + `witness.scan = D-cycle-27` + `witness.scan-self =
 * count=0`. Per the cycle-31 cycle-32 9-row discriminator table this
 * collapses to (γ.0) "execution never entered `main()` AT ALL" OR
 * (γ.1) "`XVideoSetMode` itself faulted hard before returning." The
 * cycle-22 pre-main-crash hypothesis is now FULLY CORROBORATED in its
 * strongest form, but cycle 34 alone cannot tell γ.0 from γ.1.
 *
 * Cycle 35 adds the cheapest discriminator that runs strictly BEFORE
 * `main()`: nxdk's CRT registers two function-pointer-table sections
 * (`crt_initializers.c` lines 8-13) — `.CRT$XI*` (C initializers,
 * `_PIFV = int(*)(void)`) and `.CRT$XC*` (C++ initializers,
 * `_PVFV = void(*)(void)`) both walked by `_PDCLIB_xbox_run_crt_initi-
 * alizers()` from the `main_wrapper` thread, plus `.CRT$XX*` (pre-
 * initializers, `_PVFV`) walked by `_PDCLIB_xbox_run_pre_initializers()`
 * from `WinMainCRTStartup` BEFORE `thrd_create(main_wrapper)`. Cycle 35
 * registers two slots that each call `xbed_self_witness_fire(stage)`
 * with new pre-main stage codes:
 *
 *   - .CRT$XXC slot — stage WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XX = 4.
 *     Runs in the entry thread after `__security_init_cookie` + TLS
 *     setup + `_PDCLIB_xbox_libc_init` but BEFORE `thrd_create`. This
 *     is the EARLIEST point in process lifetime where straight-line C
 *     code can run.
 *   - .CRT$XCU slot — stage WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XC = 5.
 *     Runs in `main_wrapper`'s thread immediately before `main()`
 *     after `_PDCLIB_xbox_run_crt_initializers()`'s XI pass returns
 *     zero.
 *
 * Each fire increments the WTNS counter (`reserved1`) by 1 and writes
 * its stage byte into `reserved0`'s low 24 bits with the 0xA4 tag in
 * the high byte. The fires run in lockstep order (XX → XC →
 * main()'s WTNS MAIN_ENTERED → main()'s WTNS POST_MARKER0) so the
 * final `reserved1` counter == number of WTNS fires that landed:
 *
 *   (count, reserved1)    | meaning (cycle-36 readback)
 *   ------------------    | -----------------------------------------
 *   (0, n/a)              | NO WTNS page allocated → not even the
 *                           .CRT$XX slot ran (OR MmAllocateContiguous-
 *                           MemoryEx itself returned NULL from
 *                           .CRT$XX — edge case in
 *                           lib/xbed_self_witness.c:54-74) → γ.0
 *                           NARROWED to "_start / __security_init_cookie
 *                           / TLS setup / libc_init crash" — strictly
 *                           EARLIER than anything cycle 34 could
 *                           distinguish.
 *   (1, 1) stage=4        | .CRT$XX slot ran; .CRT$XC slot did NOT →
 *                           either `thrd_create(main_wrapper)` failed
 *                           OR `main_wrapper`'s
 *                           `_PDCLIB_xbox_run_crt_initializers` XI pass
 *                           faulted. γ.0 NARROWED to "in
 *                           thread-create or in XI initializer pass."
 *   (1, 2) stage=5        | both pre-main slots ran but no in-main
 *                           WTNS fire landed. γ.1 *candidate* window
 *                           (NOT corroborated): TWO sub-cases share
 *                           this shape and cycle-35 evidence CANNOT
 *                           distinguish them — (γ.0 sub) main() never
 *                           entered after .CRT$XC return OR (γ.1)
 *                           main() entered AND crashed inside paint(0)
 *                           = XVideoSetMode before any in-main WTNS
 *                           fire could tick the counter higher.
 *                           Cycle 37 can separate by adding a .CRT$XCV
 *                           slot inside main_wrapper that fires AFTER
 *                           .CRT$XCU but BEFORE main()'s first
 *                           instruction.
 *   (1, 3..4) no stripes  | cycle-35 analogue of cycle-32 F4':
 *                           graceful XVideoSetMode FALSE return
 *                           latched xbed_breadcrumb_init FAILED so
 *                           paint(0..4) became no-ops; main()
 *                           continued through cycle-23 XCTR + cycle-29
 *                           in-main WTNS fires. γ INVALIDATED via WTNS
 *                           path; cycle 37 investigates AV-encoder
 *                           rejection.
 *   (1, 3..4) stripes vis | main() entered AND paint(0) ran
 *   stage in {1,3}          (XVideoSetMode succeeded) AND reached at
 *                           least one in-main WTNS fire. γ.0
 *                           INVALIDATED. Apply cycle-32 F-row rules
 *                           for the in-main half.
 *
 * The fires use the existing `xbed_self_witness_fire(stage)` shim
 * (cycle-29 lib, unchanged) so ZERO new shared-lib code is needed and
 * the cycle-23 lockstep contract + cycle-29 self-witness shim remain
 * bit-identical to their last Codex-validated state. The new code is
 * confined to `witness-only/main.c`. The pre-main fires use a brand-
 * new stage-code namespace (4, 5) that does NOT overlap the existing
 * XBED_A4_STAGE_* values (1, 2, 3) so the agent's `witness.scan-self`
 * reader (which only tag-byte-filters with `(reserved0 >> 24) ==
 * 0xA4`) accepts them without modification.
 *
 * Why this design over the other cycle-34 F4 "Next"-column candidates
 * ------------------------------------------------------------------
 * 1) `.CRT$X*` static-init slot stamp (CHOSEN). nxdk's CRT walks these
 *    sections unconditionally; the mechanism is documented in
 *    `nxdk/lib/pdclib/platform/xbox/crt_initializers.c` and exercised
 *    every time any nxdk-built XBE runs (nxdk's own crt registers
 *    `__xc_a[] / __xc_z[]` etc. as section terminators in the same
 *    file). ZERO nxdk source, linker, or XBE-header schema changes are
 *    required. ZERO new shared-lib code is required because the fires
 *    reuse `xbed_self_witness_fire`. Scope = `witness-only/main.c`
 *    only.
 * 2) Custom XBE-header callback (REJECTED). nxdk's XBE header generator
 *    does not expose a documented "pre-CRT entry slot" mechanism; an
 *    implementation would have to modify nxdk's PE / XBE-header
 *    generation (`nxdk/tools/cxbe/` or the linker script), widening
 *    scope to nxdk itself AND making the new mechanism untested on the
 *    Xbox kernel-entry path. Strictly earlier than `.CRT$XX*` but the
 *    EARLIEST `.CRT$XX*` slot is already after only `_start` +
 *    `__security_init_cookie` + a TLS-size computation + a libc_init
 *    mutex setup, so the γ.0 sub-windows it could uniquely distinguish
 *    (crash inside `_start` / inside `__security_init_cookie` / inside
 *    TLS setup) are vanishingly unlikely to be the cycle-22 hang site.
 *    The marginal discriminator value does NOT justify modifying nxdk.
 * 3) Thinner alternative to `XVideoSetMode` (e.g. direct NV2A CRTC
 *    register writes that bypass kernel display init; REJECTED). Does
 *    not address the γ.0 vs γ.1 question — if `main()` does not enter
 *    at all, no `main()`-body code runs whether it touches CRTC
 *    registers or `XVideoSetMode`. Also widens the NV2A surface
 *    (cycle-23 lockstep contract + cycle-29 self-witness shim would
 *    have to coexist with direct register pokes), which violates the
 *    cycle-34 prompt's "tightly scoped" guardrail. Filed for cycle-36+
 *    only IF cycle-36 narrows the crash site to γ.1 AND a less-invasive
 *    paint mechanism becomes useful.
 *
 * Cycle-35 ordering rationale
 * ---------------------------
 * The two pre-main fires execute in nxdk's natural CRT order
 * (`.CRT$XX*` from entry thread BEFORE `.CRT$XC*` from `main_wrapper`),
 * which is bit-identical to every other nxdk-built XBE's startup
 * sequence. No new ordering invariants are introduced. The four in-
 * `main()` fires (cycle-23 XCTR ×2 + cycle-29 WTNS ×2) AND the cycle-
 * 31 visual breadcrumb sequence are unchanged. The WTNS page is now
 * allocated by the FIRST `.CRT$XX` fire (instead of by the first in-
 * `main()` WTNS fire as in cycle 29); subsequent fires (CRT$XC + both
 * in-main WTNS) reuse the same page via the shim's idempotent
 * `s_witness_page != 0` early return. The cycle-23 XCTR fires are
 * unchanged and continue to target the agent's persistent XCTR buffer
 * (an entirely separate kseg0 page); no interaction with the cycle-35
 * additions.
 *
 * Cycle 35 is therefore additive but NOT a strict superset of cycle 25
 * OR cycle 29 OR cycle 31 — the WTNS page now gets allocated EARLIER
 * in the process lifetime (from `.CRT$XX*` instead of from main()'s
 * first WTNS fire). The cycle-26-style "WTNS allocator interacts with
 * unmapped kseg0 read" hypothesis is NOT a concern because the
 * allocator path doesn't sweep kseg0; it only calls
 * `MmAllocateContiguousMemoryEx`, which is known-safe at every point
 * post-`_start` (the agent itself calls it during its own startup
 * from its own `main()`'s first instruction). The cycle-35 win is
 * granularity, not a new failure mode.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xbed_a4_witness.h"
#include "xbed_runtime.h"
#include "xbed_self_witness.h"

/* Cycle-31 option (d) on-screen breadcrumb.
 *
 * 5 horizontal stripes painted top-to-bottom on a 640x480x32 framebuffer,
 * each 96 rows tall (480 / 5 = 96). Composite-capture interpretation:
 *
 *   Stripe  Color (ARGB8888)  What it proves
 *   ------  ----------------  --------------
 *   0       RED    0xFFFF0000  main() reached its first observable
 *                              instruction (XVideoSetMode + paint of
 *                              stripe 0 completed before the first fire)
 *   1       ORANGE 0xFFFF7F00  xbed_a4_witness_fire(MAIN_ENTERED) returned
 *   2       YELLOW 0xFFFFFF00  xbed_a4_witness_fire(POST_MARKER0) returned
 *   3       GREEN  0xFF00FF00  xbed_self_witness_fire(MAIN_ENTERED) returned
 *   4       BLUE   0xFF0000FF  xbed_self_witness_fire(POST_MARKER0) returned
 *
 * A cycle-32 composite capture takes one or more frames near reboot
 * time. The deepest stripe whose color matches the table above
 * indicates the deepest checkpoint reached. Stripes missing above
 * the deepest visible one would indicate a write that landed in the
 * framebuffer but did not survive cache eviction; the FB is
 * MmAllocateContiguousMemoryEx-allocated with PAGE_WRITECOMBINE and
 * XVideoFlushFB issues an sfence after each paint, so missing
 * intermediate stripes should be vanishingly rare.
 */
#define XBED_BREADCRUMB_W      640
#define XBED_BREADCRUMB_H      480
#define XBED_BREADCRUMB_BANDS  5

static const uint32_t s_breadcrumb_colors[XBED_BREADCRUMB_BANDS] = {
    0xFFFF0000u,  /* stripe 0: main() entered */
    0xFFFF7F00u,  /* stripe 1: XCTR fire1 (MAIN_ENTERED) returned */
    0xFFFFFF00u,  /* stripe 2: XCTR fire2 (POST_MARKER0) returned */
    0xFF00FF00u,  /* stripe 3: WTNS fire1 (MAIN_ENTERED) returned */
    0xFF0000FFu,  /* stripe 4: WTNS fire2 (POST_MARKER0) returned */
};

/* Breadcrumb init state machine.
 *
 *   STATE  Meaning
 *   -----  -------
 *   0      Untried: the next paint will attempt XVideoSetMode.
 *   1      Succeeded: XVideoSetMode returned TRUE; paints are live.
 *   2      Failed (latched): XVideoSetMode returned FALSE on a prior
 *          attempt; subsequent paints are permanent no-ops AND the
 *          XVideoSetMode call is NOT retried.
 *
 * The latched failure state is load-bearing for cycle-32 outcome F4
 * interpretation (Codex cycle-31 round-1 high finding adopted). If
 * XVideoSetMode were re-invoked from every paint(N) after a prior
 * failure, then a "no stripes visible" composite reading could equally
 * mean "main() never ran past paint(0)" OR "XVideoSetMode failed
 * gracefully then crashed on a later retry." Latching ensures the
 * single XVideoSetMode call is concentrated at paint(0)'s invocation;
 * paint(1..4) cannot re-enter the kernel display init path. */
#define XBED_BREADCRUMB_INIT_UNTRIED 0
#define XBED_BREADCRUMB_INIT_OK      1
#define XBED_BREADCRUMB_INIT_FAILED  2

static int s_breadcrumb_init_state = XBED_BREADCRUMB_INIT_UNTRIED;

/* Bring up a 640x480x32 framebuffer ONCE. Returns nonzero on success.
 * On a graceful FALSE return from XVideoSetMode (exotic AV
 * configuration), latches the failure into XBED_BREADCRUMB_INIT_FAILED
 * so subsequent paint calls become permanent no-ops AND the
 * XVideoSetMode call is NOT retried (Codex cycle-31 round-1 high
 * finding adopted: see state-machine doc above). XVideoSetMode
 * internally calls AvGetSavedDataAddress + MmAllocateContiguousMemoryEx
 * (with PAGE_WRITECOMBINE) + AvSetDisplayMode + XVideoSetGammaRamp —
 * all well-trodden nxdk paths used by `xbed_init` and every diag XBE
 * that draws anything (see `lib/xbed_runtime.c:45`). pbkit is NOT
 * called — that is the cycle-31 design point. */
static int xbed_breadcrumb_init(void)
{
    if (s_breadcrumb_init_state == XBED_BREADCRUMB_INIT_OK) {
        return 1;
    }
    if (s_breadcrumb_init_state == XBED_BREADCRUMB_INIT_FAILED) {
        return 0;
    }
    if (!XVideoSetMode(XBED_BREADCRUMB_W, XBED_BREADCRUMB_H, 32,
                       REFRESH_DEFAULT)) {
        s_breadcrumb_init_state = XBED_BREADCRUMB_INIT_FAILED;
        return 0;
    }
    s_breadcrumb_init_state = XBED_BREADCRUMB_INIT_OK;
    /* Clear to opaque black so any unpainted stripe is clearly
     * distinct from a "no signal" composite reading. */
    unsigned char *fb = XVideoGetFB();
    if (fb) {
        memset(fb, 0,
               XBED_BREADCRUMB_W * XBED_BREADCRUMB_H * 4);
        XVideoFlushFB();
    }
    return 1;
}

/* Paint stripe `stage` (0..XBED_BREADCRUMB_BANDS-1) with its assigned
 * color. Each stripe is 96 rows tall on a 640-wide 32bpp framebuffer.
 * Idempotent. No-op if display init failed or stage is out of range. */
static void xbed_breadcrumb_paint(int stage)
{
    if (!xbed_breadcrumb_init()) {
        return;
    }
    if (stage < 0 || stage >= XBED_BREADCRUMB_BANDS) {
        return;
    }
    unsigned char *fb = XVideoGetFB();
    if (!fb) {
        return;
    }
    const int band_h = XBED_BREADCRUMB_H / XBED_BREADCRUMB_BANDS;
    uint32_t *row = (uint32_t *)fb + stage * band_h * XBED_BREADCRUMB_W;
    uint32_t color = s_breadcrumb_colors[stage];
    for (int y = 0; y < band_h; y++) {
        for (int x = 0; x < XBED_BREADCRUMB_W; x++) {
            row[x] = color;
        }
        row += XBED_BREADCRUMB_W;
    }
    XVideoFlushFB();
}

/* Cycle-35 pre-main breadcrumb stage codes.
 *
 * Local to this XBE so the cycle-23 lockstep contract on `xbed_a4_witness.h`
 * (which owns stage codes 1, 2, 3 = MAIN_ENTERED / PRE_MARKER0 /
 * POST_MARKER0) stays intact and the cycle-35 namespace cannot
 * accidentally collide with future cycle-23 stage additions. The
 * `xbed_self_witness_fire` shim accepts any uint32_t in the low 24
 * bits of `reserved0`; the agent's `witness.scan-self` reader filters
 * only on the 0xA4 tag in the high byte. */
#define WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XX  4u  /* .CRT$XXC slot fired */
#define WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XC  5u  /* .CRT$XCU slot fired */

/* Pre-main breadcrumb runners. Each is a `_PVFV = void(*)(void)`-shaped
 * function whose address lives in a `.CRT$X*` section so nxdk's CRT
 * walks it automatically. The body calls `xbed_self_witness_fire` with
 * the corresponding stage code; the shim is idempotent (first call
 * allocates + zeros + stamps magic/version + stamps stage; subsequent
 * calls just stamp + tick counter), so this is safe to invoke from
 * both pre-main slots AND from main()'s WTNS fires later.
 *
 * Side effects intentionally minimized: no kseg0 sweep (the WTNS path
 * touches only its own page); no XCTR fire (the cycle-23 lockstep
 * contract stays intact); no framebuffer paint (XVideoSetMode is the
 * γ.1 candidate-crash-site so we DELIBERATELY do not touch it from
 * pre-main slots — that would defeat the cycle-35 discriminator). */
static void witness_only_pre_main_crt_xx(void)
{
    xbed_host_log_write(
        "witness-only: .CRT$XXC pre-main breadcrumb running (cycle 35)");
    uintptr_t rpm1 = xbed_self_witness_fire(WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XX);
    xbed_host_log_writef(
        "witness-only: pre-main-xx fire returned phys=0x%08lx",
        (unsigned long)rpm1);
}

static void witness_only_pre_main_crt_xc(void)
{
    xbed_host_log_write(
        "witness-only: .CRT$XCU pre-main breadcrumb running (cycle 35)");
    uintptr_t rpm2 = xbed_self_witness_fire(WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XC);
    xbed_host_log_writef(
        "witness-only: pre-main-xc fire returned phys=0x%08lx",
        (unsigned long)rpm2);
}

/* nxdk CRT initializer-table function-pointer type. Mirrors the
 * `_PVFV` typedef in `nxdk/lib/pdclib/platform/xbox/crt_initializers.c`
 * (which is file-local there, so we re-declare ours locally too). */
typedef void (__cdecl *witness_only_pvfv_t)(void);

/* Slot pointer in .CRT$XXC — walked by `_PDCLIB_xbox_run_pre_initi-
 * alizers()` from `WinMainCRTStartup` BEFORE `thrd_create(main_wrapper)`.
 * The `used` attribute prevents dead-code-elimination from dropping
 * the symbol since nothing in this translation unit otherwise
 * references it. nxdk's `crt_initializers.c` provides the `.CRT$XXA`
 * (start sentinel) and `.CRT$XXZ` (end sentinel) terminators; our
 * `.CRT$XXC` slot sorts alphabetically between them. */
__attribute__((section(".CRT$XXC"), used))
static witness_only_pvfv_t s_witness_only_pre_main_crt_xx_slot =
    witness_only_pre_main_crt_xx;

/* Slot pointer in .CRT$XCU — walked by `_PDCLIB_xbox_run_crt_initi-
 * alizers()` from `main_wrapper` AFTER the .CRT$XI* C-initializer pass
 * AND immediately BEFORE the user's `main()`. Same `_PVFV` shape +
 * `used` attribute pattern as the XX slot. */
__attribute__((section(".CRT$XCU"), used))
static witness_only_pvfv_t s_witness_only_pre_main_crt_xc_slot =
    witness_only_pre_main_crt_xc;

int main(void)
{
    /* CYCLE-31 OPTION (d) BREADCRUMB #0: paint the top stripe RED
     * BEFORE any other observable side effect of main(). If composite
     * capture during a cycle-32 real-Xbox run records this stripe,
     * main() ran past its first executable instruction — γ
     * ("main() never reached the fire calls") is INVALIDATED. Absence
     * narrows the live causes to (γ.0) execution never entered main()
     * AT ALL (CRT _start / __security_init_cookie / static-init
     * crash), or (γ.1) XVideoSetMode itself crashed before painting
     * completed (kernel display init not safe in this context).
     * Cycle-31 ordering rationale: this call MUST precede the
     * cycle-25 host-log anchor and the cycle-23 fires because its
     * value proposition is exactly to discriminate "did main() run
     * at all?" — running it AFTER another visible side effect would
     * defeat the purpose. */
    xbed_breadcrumb_paint(0);

    /* Host-log anchor BEFORE the first witness fire. Inert on real
     * Xbox and on stock xemu (the OUT to port 0xE9 is a silent
     * no-op without `XEMU_GUEST_LOG=1`). On xemu-Metal local
     * validation, this line + the witness "enter stage=*" lines + the
     * tail "rebooting" line provide a complete control-flow trace.
     *
     * Cycle 29 option (c): the cycle-25 line is preserved verbatim.
     * The cycle-29 self-witness fires execute AFTER the cycle-23
     * XCTR fires (Codex round-1 high finding #1 adopted) so the
     * cycle-23 path runs under conditions bit-identical to cycle 25.
     * Cycle 29 is therefore additive but NOT a strict superset of
     * cycle 25: the post-cycle-23-fires-to-reboot window gains new
     * kernel-allocator activity (one MmAllocateContiguousMemoryEx
     * + MmPersistContiguousMemory + page wipe + four 32-bit writes
     * per fire). Up to and including the second cycle-23 fire,
     * cycle 29 is bit-identical to cycle 25.
     *
     * Cycle 31 option (d): the cycle-25 line is preserved verbatim
     * AND is now itself preceded by a cycle-31 visual breadcrumb
     * paint (see xbed_breadcrumb_paint(0) above). Cycle 31 is
     * therefore additive but NOT a strict superset of cycle 25 OR
     * cycle 29: a single XVideoSetMode call runs BEFORE all prior
     * cycles' instructions. Bit-identicality with cycle 29 holds
     * only from this xbed_host_log_write line through the second
     * cycle-29 self-witness fire — the only new instructions
     * relative to cycle 29 are concentrated in the four extra
     * xbed_breadcrumb_paint calls between checkpoints (each of which
     * is a few hundred memory writes into a write-combined linear
     * framebuffer) plus the one XVideoSetMode kernel call. */
    xbed_host_log_write("witness-only: main() entered (cycle 25)");

    /* Fire 1: MAIN_ENTERED.
     *
     * Mirrors image-blit/main.c:789 (the absolute first instruction
     * of image-blit's main() body). If witness-only succeeds where
     * image-blit hangs, the cycle-25 conclusion is that image-blit's
     * hang is from code AFTER this point in main(), not from the
     * witness call or from pre-main paths sensitive to the linked
     * xbed_a4_witness.c .text. */
    uintptr_t r1 = xbed_a4_witness_fire(XBED_A4_STAGE_MAIN_ENTERED);
    xbed_host_log_writef("witness-only: fire1 returned phys=0x%08lx",
                         (unsigned long)r1);
    /* CYCLE-31 BREADCRUMB #1: cycle-23 XCTR fire1 returned. */
    xbed_breadcrumb_paint(1);

    /* Sleep between fires.
     *
     * Cycle-23 image-blit bracketed an intermediate call (the
     * `image_blit_marker(0, ...)` fopen) between the two witness
     * fires. Cycle-25 deliberately substitutes a passive `Sleep`
     * so any post-fire CPU-state perturbation from the witness
     * itself (e.g. a delayed bus hang from a faulty kseg0 read)
     * has room to surface without confounding the second fire's
     * behavior with marker-helper code. The marker helper is NOT
     * exercised here — so a clean cycle-26 outcome does NOT
     * independently exclude the marker helper as a contributor
     * to image-blit's hang; that requires a separate cycle. */
    Sleep(500);

    /* Fire 2: POST_MARKER0.
     *
     * Mirrors image-blit/main.c:806. If both fires complete and the
     * Xbox reboots cleanly, the agent's `witness.scan` (after
     * cycle-26 re-launch) will show an orphan with `reserved[0] ==
     * 0xA4000003` (= 0xA4 << 24 | 3 = (0xA4 << 24) | POST_MARKER0)
     * because `xbed_a4_witness_fire` targets the HIGHEST-phys
     * candidate each call — the second stamp overwrites the first
     * on the same buffer, and the counter (`reserved[1]`) ticks
     * twice. */
    uintptr_t r2 = xbed_a4_witness_fire(XBED_A4_STAGE_POST_MARKER0);
    xbed_host_log_writef("witness-only: fire2 returned phys=0x%08lx",
                         (unsigned long)r2);
    /* CYCLE-31 BREADCRUMB #2: cycle-23 XCTR fire2 returned. */
    xbed_breadcrumb_paint(2);

    /* Cycle 29 option (c): self-allocated witness fires AFTER the
     * cycle-23 XCTR fires.
     *
     * Ordering rationale (Codex round-1 high finding #1): if the
     * cycle-29 self-witness allocation / cache flush / writes ran
     * BEFORE the cycle-23 fires, any new failure mode in the
     * cycle-23 path would be confounded with the new
     * kernel-allocator activity. By running self-witness AFTER
     * the cycle-23 fires, the cycle-23 XCTR-scan path executes
     * under conditions bit-identical to cycle-25 (same instruction
     * sequence, same kernel state on entry), so a cycle-30 `(D-cycle-27
     * + WTNS success)` shape is properly comparable to cycle 28's
     * D-cycle-27 result. The trade-off: a cycle-22-style pre-main
     * crash that happens to fall BEFORE the cycle-23 fires would
     * also fall BEFORE the cycle-29 fires (acceptable — γ would
     * still surface as `WTNS count=0`); a crash specifically
     * between the cycle-23 fires and the cycle-29 fires would yield
     * `XCTR observation == cycle-25` AND `WTNS count=0`, which is a
     * NEW shape this ordering can produce but cycle 25 could not
     * (acceptable — informative about the cycle-23-fires-to-reboot
     * window).
     *
     * Two fires mirror the cycle-23 pattern (MAIN_ENTERED then
     * POST_MARKER0). First call allocates the persistent page;
     * second call reuses it. After both, the self-witness page
     * carries `reserved0 == 0xA4000003` (POST_MARKER0 = 3) and
     * `reserved1 == 2` (counter ticked twice). The relaunched
     * cycle-29 agent's `witness.scan-self` reports exactly that
     * shape on a fully-successful run. */
    uintptr_t rs1 = xbed_self_witness_fire(XBED_A4_STAGE_MAIN_ENTERED);
    xbed_host_log_writef("witness-only: self-fire1 returned phys=0x%08lx",
                         (unsigned long)rs1);
    /* CYCLE-31 BREADCRUMB #3: cycle-29 WTNS self-fire1 returned. */
    xbed_breadcrumb_paint(3);
    uintptr_t rs2 = xbed_self_witness_fire(XBED_A4_STAGE_POST_MARKER0);
    xbed_host_log_writef("witness-only: self-fire2 returned phys=0x%08lx",
                         (unsigned long)rs2);
    /* CYCLE-31 BREADCRUMB #4: cycle-29 WTNS self-fire2 returned. */
    xbed_breadcrumb_paint(4);

    /* Settle period before reboot.
     *
     * Cycle-19+20+21 measured ~22.4 s chainload→FTP-back gap for
     * pre-witness image-blit (post-fault auto-reboot). Cycle-25's
     * intentional reboot path should be much faster:
     * HalReturnToFirmware(HalRebootRoutine) initiates a soft reset
     * that reaches the dashboard FTP server in ~5..15 s on a clean
     * exit (cycles 26 + 28 reproduced ~70 s; cycle 30 reproduced
     * ~39 s on the cycle-29 build — both consistent with an
     * unhandled-fault auto-reboot path rather than the clean
     * HalReturnToFirmware path actually being reached).
     * Cycle 25's original 500 ms gave the host-log writes time to
     * drain the port-0xE9 emit before the reboot kills the OUT
     * instruction pipeline.
     *
     * CYCLE-31 OPTION (d): extend cycle-25's 500 ms to 2000 ms so
     * a composite-capture stream running at ~30 fps captures the
     * final painted stripe state across ≥60 frames. That gives the
     * cycle-32 analyzer redundant samples in case of capture-card
     * frame drops AND provides headroom for the BIOS to start
     * reasserting its own video state during the soft reset (the
     * AV encoder may take a few frames after HalReturnToFirmware to
     * latch the dashboard's mode again). 2000 ms stays well under
     * any plausible Xbox watchdog window AND still preserves the
     * cycle-25 host-log drain guarantee (the drain completes in
     * the first few ms; the remaining time is composite-headroom).
     * The 500 → 2000 ms change cannot itself trigger a new failure
     * mode (it is a passive Sleep on the same code path cycle 29
     * already exercises). */
    Sleep(2000);

    xbed_host_log_write("witness-only: rebooting via "
                        "HalReturnToFirmware(HalRebootRoutine)");
    /* On xemu-Metal validation, debugPrint is captured to the
     * standard xemu log (separate from the XEMU_GUEST_LOG=1
     * host-log channel). Provides a redundant breadcrumb when
     * xemu is run without XEMU_GUEST_LOG=1. */
    debugPrint("witness-only: rebooting\n");

    HalReturnToFirmware(HalRebootRoutine);
    /* not reached */
    return 0;
}
