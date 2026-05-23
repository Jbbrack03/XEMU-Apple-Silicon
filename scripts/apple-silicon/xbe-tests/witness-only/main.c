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
 */
#include <hal/debug.h>
#include <hal/xbox.h>
#include <stdint.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xbed_a4_witness.h"
#include "xbed_runtime.h"
#include "xbed_self_witness.h"

int main(void)
{
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
     * cycle 29 is bit-identical to cycle 25. */
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
    uintptr_t rs2 = xbed_self_witness_fire(XBED_A4_STAGE_POST_MARKER0);
    xbed_host_log_writef("witness-only: self-fire2 returned phys=0x%08lx",
                         (unsigned long)rs2);

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
