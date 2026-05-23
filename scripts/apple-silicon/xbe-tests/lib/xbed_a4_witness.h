/*
 * xbed_a4_witness — non-fopen kernel-pool controller-buffer witness for
 * the cycle-23 image-blit Path A.4 discriminator (see
 * `docs/apple-silicon/decision-log.md` cycle-23 entry +
 * `docs/apple-silicon/handoff.md` cycle-23 entry).
 *
 * Architecture
 * ------------
 * The oracle agent (`scripts/apple-silicon/xbe-tests/oracle-agent/`)
 * allocates a persistent kernel-pool buffer at startup (see
 * `oracle-agent/controller.c::s_allocate_fresh`) that carries a
 * `oracle_ctrl_buffer` header (`'XCTR'` magic + version 1 + 8 reserved
 * bytes + 4 port states). The allocation uses
 * `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory` so the
 * physical page survives the agent's own process death across an
 * `XLaunchXBE` chainload.
 *
 * This shim provides a fopen-FREE witness mechanism for a chainloaded
 * diag XBE (image-blit at cycle 23) to record that it reached a given
 * execution stage. It works by:
 *
 *   1. Scanning kseg0 RAM [0x80010000, 0x84000000] in 4 KiB strides for
 *      EVERY page that passes the agent's `oracle_ctrl_buffer` header
 *      filter (`magic == 'XCTR' && version == 1`, plus the
 *      `reserved[0]/reserved[1]` plausibility filters in
 *      `a4_candidate_ok`). The HIGHEST-phys passing candidate is the
 *      stamp target (Codex cycle-23 review finding #3): the agent's
 *      persistent allocator grows monotonically per restart, so the
 *      highest-phys passing page is the most recent agent allocation,
 *      which keeps cycle-24 readback attribution unambiguous across
 *      repeated runs within one power session.
 *   2. On stamp: writing `(0xA4 << 24) | stage` into `reserved[0]`
 *      (offset 8) and incrementing `reserved[1]` (offset 12) as a
 *      call counter. `wbinvd` for cache coherence.
 *   3. Leaving the `'XCTR'` magic + version 1 fields intact so
 *      subsequent scans (from a re-entered XBE or a restarted agent's
 *      `witness.scan` RPC) still recognize the buffer.
 *
 * No `fopen` is involved anywhere in the witness path. The host-log
 * channel (`xbed_host_log_write`) is the ONLY diagnostic output and is
 * itself an OUT instruction that is inert on real Xbox / stock xemu
 * without `XEMU_GUEST_LOG=1`.
 *
 * Discriminator semantics for cycle-23 Path A.4
 * ---------------------------------------------
 * After image-blit chainloads, runs, and reboots back to FTP, the
 * agent is re-launched and queried via the new `witness.scan` RPC on
 * the agent (see `oracle-agent/commands.c::cmd_witness_scan`). The
 * scan enumerates ALL `oracle_ctrl_buffer` instances in kseg0 — the
 * NEW live buffer (allocated by the restarted agent, `reserved[0]=0`)
 * plus any ORPHAN persistent pages from previous agent runs (the
 * agent leaks one 4 KiB page per restart until the Xbox is power-
 * cycled, per `oracle-agent/controller.c:207`). If image-blit reached
 * a stage-N witness call, the orphan from the agent run that preceded
 * image-blit will have `reserved[0] == (0xA4 << 24) | N`.
 *
 *   - Witness fires (any orphan has `reserved[0] == 0xA4xxxxxx`)
 *     → image-blit DID reach main()'s first instruction
 *     → cycle-22 leading hypothesis (pre-main crash) INVALIDATED.
 *   - Witness does NOT fire → cycle-22 leading hypothesis CORROBORATED
 *     → CRT/static-init/XBE-thunking analysis required.
 *   - Witness fires with stage MAIN_ENTERED but not POST_MARKER0
 *     → main() entered but `image_blit_marker(0, ...)` itself crashed.
 *
 * Safety notes
 * ------------
 * - Scan range is kseg0 identity-mapped RAM only ([0x80010000,
 *   0x84000000]). NOT every page in that range is mapped by the OG
 *   Xbox kernel — only pages the kernel's MMU page tables actually
 *   cover. Blindly dereferencing the full range CRASHES the XBE
 *   (confirmed locally on xemu-Metal, 2026-05-22 cycle-23 validation).
 *   Every candidate page must therefore be gated with
 *   `MmGetPhysicalAddress` first; unmapped pages return 0 and are
 *   skipped. Same safety pattern as
 *   `oracle-agent/tier2.c:159` and `oracle-agent/controller.c:163`.
 * - The scan caps at 16 384 iterations (== 64 MiB / 4 KiB). It targets
 *   the HIGHEST-phys buffer that passes magic + version +
 *   reserved-field filters, which corresponds to the most recently
 *   allocated agent buffer (the agent's allocator grows monotonically
 *   per restart per `controller.c:207-208`). On a fresh power-cycle
 *   that is the live agent buffer; on repeated runs within one power
 *   session it is the most recent orphan, which keeps cycle-24
 *   readback attribution unambiguous run-to-run.
 * - Hard precondition for an UNAMBIGUOUS cycle-24 readback:
 *   `witness.scan` before chainload must show exactly one live buffer
 *   with reserved[0] == 0. If multiple orphans pre-exist (e.g., from
 *   a prior cycle-24 run earlier in the same power session), Hermes
 *   must power-cycle the Xbox first.
 * - The function takes no locks and does no allocation; it is safe to
 *   call before any C runtime or graphics init is up.
 */
#ifndef XBED_A4_WITNESS_H
#define XBED_A4_WITNESS_H

#include <stdint.h>

/* Stage codes for A.4 witness writes. Each call site uses a unique
 * stage byte so the host-side `witness.scan` reader can tell which
 * source-line the chainloaded XBE last reached. Stage 0 is reserved
 * for "agent allocated but no witness yet" (the agent's fresh buffers
 * have `reserved[0] == 0` by construction). */
#define XBED_A4_STAGE_MAIN_ENTERED   1u  /* first instruction of main() */
#define XBED_A4_STAGE_PRE_MARKER0    2u  /* just before image_blit_marker(0, ...) */
#define XBED_A4_STAGE_POST_MARKER0   3u  /* immediately after image_blit_marker(0, ...) returns */

/* High byte of `reserved[0]` whenever the A.4 witness has fired.
 * Cycle-23 picked 0xA4 to mirror the Path A.4 label. */
#define XBED_A4_WITNESS_TAG          0xA4u

/* Fire the A.4 witness for the given stage. Scans kseg0 RAM for the
 * oracle-agent's `oracle_ctrl_buffer` ('XCTR' magic + version 1, plus
 * the reserved-field plausibility filters); picks the HIGHEST-phys
 * passing candidate (= the most recent agent allocation; see body
 * comment in the .c file for rationale), writes
 * `(XBED_A4_WITNESS_TAG << 24) | stage` to its `reserved[0]` and
 * increments `reserved[1]`. Emits one host-log line via
 * `xbed_host_log_write` so xemu logs with `XEMU_GUEST_LOG=1` see
 * each witness firing. Returns the physical address of the buffer that
 * was written, or 0 if no buffer was found.
 *
 * Safe to call before any other init — uses only kseg0 reads/writes
 * plus the host-log OUT instruction; no fopen, no allocation, no
 * pbkit/NV2A dependency. */
uintptr_t xbed_a4_witness_fire(uint32_t stage);

#endif /* XBED_A4_WITNESS_H */
