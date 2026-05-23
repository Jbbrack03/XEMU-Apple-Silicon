/*
 * xbed_self_witness — cycle-29 option (c) "self-allocated witness" shim
 * for diagnostic XBEs. Companion to (NOT replacement for)
 * `xbed_a4_witness.{c,h}` (cycle-23 XCTR-scan witness).
 *
 * Purpose (cycle-29 discriminator, see decision-log cycle-29 entry +
 * handoff cycle-29 entry):
 * -----------------------------------------------------------------
 * Cycle 28 closure observed `count=1 live=1 reserved0=0 reserved1=0`
 * after a cycle-25 `witness-only` chainload + cycle-27 preserve-branch
 * oracle-agent re-launch. The cycle-27 preserve gate is correctly
 * wired (Codex 3-round green); a landed A.4 stamp would survive it.
 * Therefore "no A.4 stamp landed on the agent's XCTR buffer." Three
 * live causes for that:
 *
 *   (α) `xbed_a4_witness::a4_candidate_ok` kseg0 scan from
 *       `witness-only`'s non-agent process context does not find the
 *       agent's persistent XCTR buffer at all (filter rejects it OR
 *       MmGetPhysicalAddress gates it out OR the agent's
 *       MmPersistContiguousMemory tagging is not visible to a foreign
 *       process's kseg0 walk).
 *   (β) Scan finds the buffer but the resulting write faults silently
 *       (PAT/WC/WB attribute divergence, or cache line never drains).
 *   (γ) `witness-only`'s `main()` never reaches the fire calls
 *       (cycle-22 leading "pre-main crash" hypothesis re-strengthens).
 *
 * Option (c) breaks the (α) vs (γ) ambiguity. Instead of writing into
 * the agent's persistent buffer (which requires the kseg0 scan to
 * find it), the diag XBE allocates ITS OWN persistent contiguous
 * page via `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory`,
 * stamps a unique magic tag + the witness stage + a counter into
 * known offsets within that page, and reboots. After the chainload,
 * the relaunched agent runs the new `witness.scan-self` verb, which
 * scans kseg0 for the unique 'WTNS' magic and reports every match.
 *
 * Discriminator semantics (cycle-30 real-Xbox readback, NOT this
 * cycle). Cycle 29 is positioned narrowly as a **(γ)-only**
 * discriminator (Codex round-1 high finding #2 adopted):
 *
 *   - `witness.scan-self count >= 1` AND a match has
 *     `(reserved0 >> 24) == 0xA4` with `reserved1` matching the
 *     number of fires the XBE issued
 *       → `main()` ran AND the self-allocated persistent page is
 *         findable by a non-agent-context kseg0 scan
 *       → (γ) INVALIDATED
 *       → (α) AND (β) BOTH REMAIN LIVE. This witness stamps a
 *         SELF-OWNED page; it does not exercise the failing write
 *         into the agent's XCTR page. So a successful readback
 *         cannot distinguish "scan can't find the agent's XCTR
 *         buffer" (α) from "scan finds it but the write faults
 *         silently" (β). Breaking α-vs-β requires cycle 31+
 *         option (b) (agent-side prior-phys dump + read-only
 *         kseg0 dump verb).
 *   - `witness.scan-self count == 0`
 *       → no self-witness magic anywhere in kseg0
 *       → `main()` did not reach the self-witness call (γ leading)
 *       → cycle 31+ should pursue option (d) (on-screen breadcrumb).
 *
 * The two diagnoses are mutually exclusive for THIS XBE in cycle 30.
 *
 * Why a separate file (not adding to xbed_a4_witness)
 * ---------------------------------------------------
 * Project rule #4 (no doc drift) + cycle-25's deliberate decision to
 * keep cycle-23 instrumentation as a controlled invariant: cycle-25
 * `witness-only` still fires the cycle-23 XCTR-scan witness, with
 * the cycle-29 self-witness fires running AFTER the cycle-23 fires
 * (Codex round-1 high finding #1 adopted). Cycle 29 is therefore
 * additive but NOT a strict superset of cycle 25 — the
 * post-cycle-23-fires-to-reboot window gains new kernel-allocator
 * activity; up to and including the second cycle-23 fire, cycle 29
 * is bit-identical to cycle 25. Adding the self-witness as a sibling
 * lib keeps the cycle-23 instruction sequence + linked .text
 * intact, isolates the cycle-29 delta to the new file + two call
 * sites in `witness-only/main.c` + the new agent verb, and
 * minimizes the risk surface of introducing a regression in the
 * cycle-25 instrumentation. The cycle-23 lockstep contract between
 * writer (`xbed_a4_witness.c`), scan reader (`oracle-agent/
 * commands.c::cmd_witness_scan`), and preserve gate (`oracle-agent/
 * controller.c::s_page_has_plausible_witness_header`) is therefore
 * unaffected by cycle 29.
 *
 * Layout (16-byte header — symmetric with `oracle_ctrl_buffer` so
 * the agent's scanner can reuse the same `reserved0`/`reserved1`
 * extraction pattern)
 * --------------------------------------------------------------
 *   offset 0  uint32 magic     = XBED_SELF_WITNESS_MAGIC ('WTNS')
 *   offset 4  uint32 version   = XBED_SELF_WITNESS_VERSION (1)
 *   offset 8  uint32 reserved0 = (0xA4 << 24) | (stage & 0x00FFFFFF)
 *   offset 12 uint32 reserved1 = call counter (starts at 0; ticks per fire)
 *   offset 16..PAGE_SIZE = zeroed at init (room for future fields)
 *
 * The HIGH byte of `reserved0` is deliberately the same 0xA4 tag
 * cycle 23 picked, so a downstream reader can use the same tag
 * predicate to confirm "yes this is a witness fire, not an
 * accidental magic match." The agent's `witness.scan-self`
 * reader applies the same plausibility filter set
 * (`(reserved0==0,reserved1==0) || ((reserved0>>24)==0xA4,
 * 1<=reserved1<=4096)`) the cycle-27 preserve gate uses, so a
 * partially-corrupted false-positive page is rejected.
 *
 * Safety notes
 * ------------
 * - Allocation uses the SAME `MmAllocateContiguousMemoryEx` floor
 *   (0x00010000) / ceiling (0x03ffffff) / page alignment / RW
 *   protection the agent uses (`oracle-agent/controller.c:217-226`).
 *   `MmPersistContiguousMemory` is then applied. This is bit-for-bit
 *   the same allocation pattern that survives an XLaunchXBE chainload
 *   on real Xbox (cycle 23 designed it; cycle 24 confirmed survival
 *   for the agent's buffer; cycle 28 reconfirmed deterministic phys
 *   reuse across re-launches).
 * - The shim writes ONLY to its own allocated page. No kseg0 sweep.
 *   No MMIO touch. No write to the agent's XCTR page. The cycle-24
 *   "kseg0-scan-from-non-agent-context hangs the bus" hypothesis #5
 *   therefore cannot fire from this code path even if it is real for
 *   the cycle-23 witness.
 * - The shim is single-process, single-threaded; no locks; safe to
 *   call before any C runtime or graphics init (no fopen, no
 *   pbkit, no NV2A).
 * - On allocation failure the shim returns 0 and emits a host-log
 *   line via `xbed_host_log_write` (inert without `XEMU_GUEST_LOG=1`).
 *   No fallback path — option (c)'s entire value proposition is that
 *   it allocates its own page; a fallback would defeat the
 *   discriminator semantics.
 */
#ifndef XBED_SELF_WITNESS_H
#define XBED_SELF_WITNESS_H

#include <stdint.h>

/* 'WTNS' little-endian — distinct from oracle-agent's 'XCTR' magic
 * (0x58435452). Memory order: 'W','T','N','S'. */
#define XBED_SELF_WITNESS_MAGIC    0x534E5457u
#define XBED_SELF_WITNESS_VERSION  1u

/* High byte of `reserved0` whenever the self-witness has fired.
 * Same 0xA4 Path-A.4 tag the cycle-23 XCTR witness picked, so the
 * tag predicate `(reserved0 >> 24) == 0xA4` is shared across both
 * witnesses (cycle-23 scan reader + cycle-27 preserve gate + this
 * shim's `witness.scan-self` reader). */
#define XBED_SELF_WITNESS_TAG      0xA4u

/* Stage codes mirror `xbed_a4_witness.h` (XBED_A4_STAGE_*). Adding
 * new stage codes only requires updating xbed_a4_witness.h; this
 * shim treats the stage as an opaque 24-bit integer. */

/* Initialize the self-witness if not already initialized, then stamp
 * the given `stage` into `reserved0` and increment `reserved1`.
 * Idempotent — repeated calls reuse the same allocated page; only
 * the first call performs `MmAllocateContiguousMemoryEx +
 * MmPersistContiguousMemory + memset + magic/version init`.
 *
 * Returns the physical address of the self-witness page on success,
 * or 0 on hard failure (allocation failed; no fallback). Caller
 * (e.g. `witness-only/main.c`) is expected to host-log the result
 * via `xbed_host_log_writef` for visibility under
 * `XEMU_GUEST_LOG=1`.
 *
 * Safe to call before any other init — uses only one kernel
 * allocation per process plus straight-line writes; no fopen, no
 * pbkit/NV2A dependency, no kseg0 sweep. */
uintptr_t xbed_self_witness_fire(uint32_t stage);

#endif /* XBED_SELF_WITNESS_H */
