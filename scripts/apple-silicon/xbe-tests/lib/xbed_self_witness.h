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
 * - Allocation uses `MmAllocateContiguousMemory` (non-`-Ex` variant;
 *   cycle-41e bounded variation) with size = 0x1000 (one page).
 *   The 1-arg call is the canonical kernel prototype declared at
 *   `nxdk/lib/xboxkrnl/xboxkrnl.h:3473-3476` and exported as
 *   `MmAllocateContiguousMemory@4` (`xboxkrnl.exe.def:172`); the
 *   `-Ex` 5-arg sibling at `xboxkrnl.h:3464-3471` is its
 *   superset. In-tree precedent for the 1-arg form is at
 *   `nxdk/lib/hal/xbox.c:34` + `:80`
 *   (`MmAllocateContiguousMemory(LaunchDataPageSize)`); the
 *   launch-data page is a per-boot kernel-managed allocation, so
 *   its known-good status proves the non-`-Ex` entry point is
 *   reachable on this kernel — it does NOT prove what alignment,
 *   cache policy, or phys-range the kernel returns. The kernel
 *   picks all of those internally; the exact kernel-default
 *   Protect / placement / Alignment for this console + build are
 *   NOT measured on this hardware (no in-tree documented
 *   page-alignment guarantee from the non-`-Ex` API either —
 *   Codex round-3 finding #3 adopted).
 *
 *   Cycles 40 (bare RW) + 41a (NC) + 41b (WC) all produced G0(c)
 *   against the `-Ex` variant — cache-policy variations EXHAUSTED
 *   under `-Ex`. Cycle 41c eliminated the "kernel demands a
 *   specific non-cycle-29-tuple address range" sub-hypothesis under
 *   `-Ex` (matched-tuple to `nxdk/lib/hal/video.c:363-367` also
 *   G0(c)). Cycle 41d eliminated the alignment-requirement branch
 *   under `-Ex` (`Alignment=0u` also G0(c)). The remaining
 *   cycle-22 candidates are {(b) the `-Ex` variant itself, (c) a
 *   `size=0x1000`-specific interaction}. Cycle 41e is the cheapest
 *   cycle-41-scope discriminator pointed at branch (b): swap to
 *   the non-`-Ex` entry point with the same size. Honest framing
 *   (Codex round-1 finding #2 adopted): cycle 41e is NOT a pure
 *   single-axis discriminator. Dropping to non-`-Ex` simultaneously
 *   cedes caller control over Protect, placement, and Alignment to
 *   whatever defaults the kernel picks for the non-`-Ex` ABI; those
 *   defaults are not measured on this hardware. A cycle-41e
 *   `witness.scan-self count >= 1` (with `(reserved0 >> 24) == 0xA4`)
 *   is STRONG but not conclusive evidence FOR branch (b) being the
 *   failing constraint — strong because the entry-point swap is
 *   the load-bearing delta vs cycle 41d, not conclusive because
 *   the swap also cedes Protect / placement / Alignment to whatever
 *   defaults the kernel picks (success may instead reflect those
 *   defaults landing on an as-yet-unmeasured working tuple). A
 *   cycle-41e G0(c) PERSISTS is STRONG evidence AGAINST branch (b)
 *   being the sole failing constraint, narrowing toward branch
 *   (c), but does not formally eliminate branch (b) on its own.
 *   (Codex round-2 finding adopted; the prior wording sign-flipped
 *   the success/failure interpretations vs the `xbed_self_witness.c`
 *   Honest-framing paragraph.) Cycle-41 scope ends after 41e either
 *   way. The only forward paths are (i) a custom XBE-header callback
 *   before `_start` (requires `nxdk/tools/cxbe/` changes), or
 *   (ii) redesigning the cycle-29 self-witness as multi-page
 *   (changes the WTNS layout contract).
 *
 *   Cycle 41c's symmetric defensive phys-range guards at the
 *   `MmGetPhysicalAddress` site (lower: `phys < 0x00010000u`;
 *   upper: `phys >= 0x04000000u`; Codex round-1 P1 + round-2 P1
 *   adopted) are PRESERVED unchanged in cycle 41e because the
 *   non-`-Ex` call has no address-range argument at all — the
 *   kernel has full discretion over the returned phys, so the
 *   consumer-window blind spot exists even more strongly than in
 *   cycle 41c/41d. On retail Xbox the kernel can only return phys
 *   within physical RAM (≤ 64 MiB), so the upper guard is a no-op
 *   on target hardware and the lower guard fires only on the
 *   (vanishingly unlikely) sub-64 KiB return. The cycle-41d
 *   page-alignment guard at `xbed_self_witness.c` is ALSO
 *   PRESERVED unchanged for the same defense-in-depth reason: the
 *   non-`-Ex` variant does not document its returned alignment;
 *   the cycle-29 consumer scan is fixed at a 0x1000 stride.
 *   `MmPersistContiguousMemory` is then applied. Cycle 23 designed
 *   the chainload-survival pattern; cycle 24 confirmed survival for
 *   the agent's buffer; cycle 28 reconfirmed deterministic phys
 *   reuse across re-launches.
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

/* Cycle-39 EEPROM scratchpad discriminator (2026-05-24).
 *
 * Cycle 36 outcome G0 on real Xbox = (stripes=none, witness.scan-self
 * count=0, witness.scan=D-cycle-27) → no WTNS page allocated, not even
 * the `.CRT$XXC` slot ran. Cycle 38 lld link-map analysis (commit
 * `1127cafa0d`) confirmed witness-only's static binary surface is
 * structurally identical to the known-boot-functional `mirror` XBE
 * modulo the two cycle-35 `.CRT$X*` slot entries — there is NO
 * witness-only-unique pre-`.CRT$X*` code path. G0 therefore narrows to
 * three live sub-cases:
 *
 *   (a) crash inside nxdk's pre-`.CRT$X*` startup (`_start` /
 *       `__security_init_cookie` / TLS-size computation /
 *       `_PDCLIB_xbox_libc_init`) — strictly earlier than any user-C.
 *   (b) crash inside `_witness_only_pre_main_crt_xx`'s body BEFORE
 *       `xbed_self_witness_fire` reaches the `MmAllocateContiguousMemoryEx`
 *       call.
 *   (c) `MmAllocateContiguousMemoryEx` returns NULL silently (or
 *       crashes) from the `.CRT$XXC` slot.
 *
 * The static binary surface has been exhausted as a discriminator
 * (cycle 37 + cycle 38). The cycle-38 recommendation was to add a
 * dynamic durable breadcrumb inside `xbed_self_witness_fire` BEFORE
 * `MmAllocateContiguousMemoryEx`. Cycle 39 implements that as a
 * single-byte EEPROM scratchpad write at offset 0xFF — the highest
 * EEPROM byte, which the 256-byte Xbox EEPROM layout reserves as
 * unused/reserved (the documented user-settings region ends at 0x96
 * and the factory-encrypted region ends at 0x5F; the 0xC0..0xFF tail
 * is consistently zero on stock OEM consoles). Write endurance for the
 * 24LC02-class EEPROM is ≥1 M cycles; a single byte write per
 * cycle-39+ run is bounded even across hundreds of debugging sessions.
 *
 * The write fires AT MOST ONCE per process — gated by a separate
 * `s_eeprom_scratch_attempted` sticky flag, NOT by the
 * `s_witness_page == 0` check on the existing first-call branch.
 * (Codex round-2 P1 finding 2026-05-24): if the allocation fails on
 * the first call — the exact G0(c) sub-case this discriminator
 * targets — `s_witness_page` stays NULL, so later `.CRT$XCU` and
 * in-main fires would re-enter the first-call branch and overwrite
 * the breadcrumb byte from 0xA4 (stage 4 = first fire) to 0xA5 /
 * 0xA1 / 0xA3 (later stages), destroying the discriminator value.
 * The sticky flag preserves the first-stage breadcrumb regardless
 * of allocation outcome. The flag is set BEFORE the write attempt
 * (rather than only on `NT_SUCCESS`) so that even a write failure
 * does not cause a later fire to re-attempt the write — the
 * failure host-log line is itself diagnostic (visible on xemu; on
 * real Xbox the EEPROM byte stays unchanged from its pre-run
 * baseline value).
 *
 * Position: AS THE LAST INSTRUCTION before
 * `MmAllocateContiguousMemoryEx`. On real Xbox the first call is
 * always the `.CRT$XXC` slot's stage=4 fire (cycle 35 ordering); a
 * post-run EEPROM byte of 0xA4 therefore proves the function body
 * reached the pre-allocation point.
 *
 * Discriminator semantics (cycle-40 real-Xbox readback table):
 *
 *   Post-run byte at 0xFF | witness.scan-self count | G-row     | Sub-case
 *   ----------------------|-------------------------|-----------|-------------
 *   0x00                  | 0                       | G0(a)+(b) | crash before EEPROM write — either pre-`.CRT$X*` startup OR helper body crashed before reaching `xbed_self_witness_fire`'s pre-allocation point
 *   0xA4                  | 0                       | G0(c)     | EEPROM write landed → `MmAllocateContiguousMemoryEx` returned NULL silently OR crashed; sub-case (a) and (b) ELIMINATED
 *   0xA4                  | 1+                      | G2..G4    | full pre-main path landed; same shapes as cycle-35 G2..G4
 *
 * The G0(c) vs G0(a)+(b) split is the cycle-39 value proposition.
 *
 * Reset semantics: the EEPROM byte is NOT auto-reset. Hermes must
 * write 0x00 to offset 0xFF BEFORE the cycle-39 real-Xbox run to
 * establish a known clean baseline. The companion oracle-agent verb
 * `eeprom.scratch.reset` (gated by `unsafe.enable`) provides this.
 * Read-back uses either the existing `cmd_eeprom` 256-byte dump
 * (offset 0xFF) or the new `eeprom.scratch.read` convenience verb.
 *
 * Why a single byte, single offset (not a two-byte function-entry +
 * pre-alloc encoding): the cycle-38 recommendation was singular —
 * "write a durable breadcrumb inside `xbed_self_witness_fire` BEFORE
 * `MmAllocateContiguousMemoryEx`". The cycle-39 value-add is solely
 * the (a)+(b)-vs-(c) split. Further discriminating (a) vs (b) requires
 * a pre-`.CRT$X*` callback (deferred to cycle-40+ if cycle-39 forces
 * that branch by leaving the byte cleared).
 *
 * Why offset 0xFF (not 0x5F+, 0x96+, or one of the SMC scratch
 * registers): offset 0xFF is the highest single byte of the 256-byte
 * EEPROM image — easiest to identify in a hex dump, no risk of
 * stepping on the factory/encrypted region (0x00..0x5F), the user
 * settings region (0x60..0x96), or the user-settings-checksum region
 * (0x96..0xBF). The 0xC0..0xFF tail is consistently zero on stock
 * OEM consoles. SMC scratch registers were considered and rejected:
 * the SMC has no documented RAM-backed scratch register that survives
 * a reboot, and the agent's smc.* allowlist intentionally excludes
 * write access to non-{0x05,0x06} registers. EEPROM is the only
 * durable cross-reboot single-byte channel.
 *
 * Failure handling: the EEPROM write's NTSTATUS is checked and
 * host-logged but does NOT short-circuit the function. The cycle-29
 * self-witness allocation path continues to attempt
 * `MmAllocateContiguousMemoryEx` regardless of EEPROM write outcome —
 * the EEPROM byte is purely additive instrumentation; a hard-error
 * fallback would defeat the discriminator semantics. */
#define XBED_SELF_WITNESS_EEPROM_SMBUS_ADDR    0xA8u    /* 24LC02 EEPROM SMBus address; same as oracle-agent/commands.c */
#define XBED_SELF_WITNESS_EEPROM_SCRATCH_OFF   0xFFu    /* last byte of 256-byte EEPROM image; reserved/unused on stock OEM consoles */
#define XBED_SELF_WITNESS_EEPROM_TAG_NIB       0xA0u    /* high nibble = 0xA "Path-A.4 tag"; low nibble = (stage & 0x0F) */


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
