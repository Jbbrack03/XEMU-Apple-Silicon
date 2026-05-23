/*
 * witness-only — cycle-25 minimal A.4 witness-mechanism viability
 * discriminator. NO pbkit, NO NV2A, NO XVideoSetMode, NO file I/O,
 * NO xbed_init.
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
 */
#include <hal/debug.h>
#include <hal/xbox.h>
#include <stdint.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xbed_a4_witness.h"
#include "xbed_runtime.h"

int main(void)
{
    /* Host-log anchor BEFORE the first witness fire. Inert on real
     * Xbox and on stock xemu (the OUT to port 0xE9 is a silent
     * no-op without `XEMU_GUEST_LOG=1`). On xemu-Metal local
     * validation, this line + the witness "enter stage=*" lines + the
     * tail "rebooting" line provide a complete control-flow trace. */
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

    /* Settle period before reboot.
     *
     * Cycle-19+20+21 measured ~22.4 s chainload→FTP-back gap for
     * pre-witness image-blit (post-fault auto-reboot). Cycle-25's
     * intentional reboot path should be much faster:
     * HalReturnToFirmware(HalRebootRoutine) initiates a soft reset
     * that reaches the dashboard FTP server in ~5..15 s.
     * 500 ms here gives the host-log writes time to drain the
     * port-0xE9 emit before the reboot kills the OUT instruction
     * pipeline. Also matches the cycle-24 handoff recommendation
     * verbatim. */
    Sleep(500);

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
