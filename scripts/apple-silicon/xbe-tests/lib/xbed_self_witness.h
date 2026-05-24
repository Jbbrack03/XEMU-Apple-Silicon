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
 * region (cycle-29 original design used `MmAllocateContiguousMemoryEx`
 * for a one-page allocation; current cycle-42A live behavior uses
 * the non-`-Ex` `MmAllocateContiguousMemory` for a two-page
 * allocation — see the "Cycle-42A" subsection at the end of the
 * Safety notes block below for the authoritative current-behavior
 * description) via `MmPersistContiguousMemory`, stamps a unique
 * magic tag + the witness stage + a counter into known offsets
 * within that region (specifically: offset 0 of the FIRST page in
 * the current multi-page layout), and reboots. After the
 * chainload, the relaunched agent runs the new `witness.scan-self`
 * verb, which scans kseg0 for the unique 'WTNS' magic and reports
 * every match.
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
 *   cycle-41e bounded variation) with size = `0x2000` (two pages,
 *   8 KiB; cycle-42A bounded variation — see cycle-42A section at
 *   the end of this Safety-notes block for the rationale + the
 *   discriminator semantics + the consumer-side impact).
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
 *
 * Cycle-42A (multi-page redesign — branch (c) discriminator,
 * 2026-05-24)
 * -------------------------------------------------------------
 * Cycle-41 scope is now EXHAUSTED. G0(c) PERSISTED across cycles
 * 40+41a+41b (cache-policy variations under `-Ex`), 41c (matched-
 * tuple address range under `-Ex`), 41d (alignment-drop under
 * `-Ex`), and 41e (non-`-Ex` ABI fallback). The remaining live
 * cycle-22 candidate is branch (c) "a `size=0x1000`-specific
 * interaction" — the ONE axis the cycle-41 series could not vary
 * inside the cycle-29 single-page WTNS layout contract.
 *
 * Cycle 42 candidate A redesigns the allocation as MULTI-PAGE.
 * The producer now requests `0x2000` bytes (= 2 pages, 8 KiB)
 * instead of cycle-41e's `0x1000` (1 page). `MmPersistContiguous-
 * Memory` is called with the matching `0x2000` size so both
 * pages survive the chainload. The page-wipe loop also covers
 * `0x2000` bytes so neither page accidentally matches the WTNS
 * magic predicate at the consumer's next 0x1000-stride read.
 * The WTNS magic + version + reserved0 + reserved1 header still
 * lives at offset 0 of the FIRST page of the allocation; the
 * second page is solid zero.
 *
 * Why `0x2000` and not `0x4000` / `0x8000`: smallest multi-page
 * size that actually tests branch (c). Larger sizes would
 * (i) increase contiguous-RAM pressure on the real-Xbox kernel
 * allocator and risk a NEW failure mode (allocator-pool
 * exhaustion) confounding the size-axis signal, and (ii) widen
 * blast radius without adding information. `0x2000` is the
 * minimal increment that genuinely varies the size axis while
 * keeping every other cycle-41e invariant intact.
 *
 * Consumer impact: the cycle-29 consumer at
 * `oracle-agent/commands.c::cmd_witness_scan_self` scans every
 * 0x1000 page in the kseg0 window `[0x80010000, 0x84000000]`
 * for the WTNS magic. Since only the first page of a cycle-42A
 * allocation carries the magic, the consumer still reports
 * `count=1` per cycle-42A allocation — same count semantics as
 * cycles 29..41e. NO consumer code change is required; only a
 * documentation note in the `cmd_witness_scan_self` comment
 * block flagging that the producer now allocates multi-page.
 *
 * Guard adjustment (cycle-41c phys-range + cycle-41d page-
 * alignment): the existing guards check the FIRST page of the
 * allocation (which carries the WTNS magic and is the only page
 * the consumer scan needs to be visible). This is sufficient
 * because the magic stamp + header live exclusively on the first
 * page; whether the second page falls inside or outside the
 * consumer scan window is irrelevant — it's zero-filled and
 * would fail the magic predicate even if scanned. On retail
 * 64 MiB hardware the upper guard never fires regardless; on the
 * theoretical 128 MiB debug-Xbox case, a multi-page allocation
 * straddling the upper end would still have its first page
 * visible to the consumer as long as the first page's phys is
 * in `[0x00010000, 0x04000000)`. Both guards are preserved
 * unchanged in the conditional; only the relevant log strings
 * are relabeled cycle-41e → cycle-42A and updated to note the
 * first-page-only nature of the check.
 *
 * Discriminator semantics (cycle-42A real-Xbox readback):
 *   - EEPROM byte=0xA4 + `witness.scan-self count >= 1` with
 *     `(reserved0 >> 24) == 0xA4` and `reserved1 >= 1`
 *     → multi-page allocation succeeded → STRONG evidence FOR
 *     branch (c) being the failing constraint. Not conclusive
 *     on its own: the kernel allocator may route single-page
 *     and multi-page contiguous requests through DIFFERENT
 *     internal code paths (e.g. size-bucketed free lists,
 *     separate pool arenas, or distinct minimum-size policies
 *     for contiguous-memory allocations from a pre-`main()`
 *     calling context). A 2-page success could reflect that
 *     internal code-path divergence rather than a real
 *     "kernel-validation rejects size=0x1000 specifically"
 *     rule. (Codex round-2 finding adopted; the earlier
 *     "2-page free run without a free 1-page hole" example was
 *     logically impossible — any free 2-page run trivially
 *     contains a free 1-page hole; the replacement covers the
 *     distinct-internal-code-path mode the prior example tried
 *     to gesture at.)
 *   - EEPROM byte=0xA4 + `witness.scan-self count=0` → G0(c)
 *     PERSISTS at 0x2000u → STRONG evidence AGAINST size being
 *     the failing axis at the smallest multi-page step. The
 *     cycle-22 candidate set then forces a move to cycle 42
 *     candidate B (custom XBE-header callback before `_start`
 *     via `nxdk/tools/cxbe/` modifications) for the calling-
 *     context axis.
 *   - EEPROM byte=0x00 + count=0 → cycle-39 G0(a)+(b)
 *     regression. NOT expected from cycle-42A's bounded diff;
 *     would indicate a build artifact problem, NOT a cycle-42A
 *     signal. Re-baseline EEPROM and re-run.
 *
 * Honest framing carried over from cycle-41e: cycle-42A is NOT a
 * pure single-axis discriminator either. Bumping `size` from
 * `0x1000` to `0x2000` while continuing to call the non-`-Ex`
 * ABI means the kernel still picks Protect / placement /
 * Alignment internally; the new 2-page request may interact
 * differently with the kernel's pool-search policy (it has to
 * find 2 contiguous physical pages) and that interaction is
 * itself uncharacterized on this hardware. Cycle-42A SUCCESS
 * is therefore "STRONG-but-not-conclusive evidence FOR branch
 * (c)"; FAILURE is "STRONG evidence AGAINST size being the
 * operative axis at the smallest multi-page step." Neither
 * outcome closes branch (c) formally on its own — that would
 * require sweeping size across multiple multi-page steps OR
 * cross-validating with candidate B. Same envelope structure
 * as cycle 41e's Honest-framing paragraph in the .c file.
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
 * fallback would defeat the discriminator semantics.
 *
 * Cycle-42D (stage-6 pre-libc-safe milestone marker bypass,
 * 2026-05-24)
 * -------------------------------------------------------------
 * Cycle 42C real-Xbox deployment of the cycle-42B pre-WinMainCRT
 * thunk produced the INCONCLUSIVE shape `(eeprom.scratch.read=0xA6,
 * witness.scan-self count=0)` — see cycle-42C SUMMARY.md, cycle-42C
 * handoff entry, decision-log cycle-42C entry. The byte-stayed-at-0xA6
 * evidence RULED OUT hypothesis (b) "allocator-then-post-guard
 * rejection" (the cycle-41c/d post-allocation guards preserve the
 * cycle-39 sticky-flag setter sequence, which would have flipped
 * EEPROM to 0xA4 from at least one of the five WTNS-shim fires; the
 * fact that no fire flipped the byte means none reached the sticky-
 * flag setter line). The live cycle-42B failure modes narrowed from
 * {a, b, c} to {a, "post-cycle-39-write-site fault that prevents the
 * sticky flag from being set on entry"}, dominated by leading
 * hypothesis (a) **pre-allocator fault inside the shim's pre-libc
 * context — most likely `xbed_host_log_writef → vsnprintf`** (the
 * shim's `xbed_host_log_writef("xbed_self_witness: enter stage=%u",
 * ...)` at function head invokes vsnprintf which assumes
 * `_PDCLIB_xbox_libc_init` has run; from the pre-WinMain calling
 * context introduced by the cycle-42B thunk, libc init has NOT run
 * yet, so vsnprintf can fault).
 *
 * Cycle 42D adds a stage==6 conditional fast path at the top of
 * `xbed_self_witness_fire` that:
 *
 *   1. Sets `s_eeprom_scratch_attempted = 1` as the FIRST shim
 *      action (lock-out of the cycle-39 EEPROM-write block so any
 *      later .CRT$XXC stage=4 fire does NOT overwrite our markers
 *      with 0xA4);
 *   2. Skips every `xbed_host_log_writef` / `xbed_host_log_write`
 *      call (the suspected fault site);
 *   3. Emits distinct EEPROM marker bytes via direct
 *      `HalWriteSMBusValue` at each shim-internal milestone (entry /
 *      pre-alloc / post-alloc / post-phys / post-lower-guard /
 *      post-upper-guard / post-align-guard / post-persist /
 *      post-wipe / post-WTNS-stamp / full-completion plus 0xBB for
 *      the not-expected idempotent-reuse case);
 *   4. Preserves the cycle-42A 2-page allocation contract, the
 *      cycle-41c symmetric phys-range guards, the cycle-41d page-
 *      alignment guard, the cycle-29 WTNS layout (magic + version +
 *      reserved0 + reserved1 at offset 0 of the first page), and
 *      the existing stage-stamp + wbinvd sequence;
 *   5. Returns early so it does NOT fall through to the existing
 *      `xbed_host_log_writef("fired stage=...")` tail.
 *
 * Stages != 6 (i.e. cycle-23 in-main XCTR-shim stages 1+3 are
 * untouched by this file; cycle-29 .CRT$XXC stage=4, .CRT$XCU stage
 * =5, in-main WTNS1 stage=1, WTNS2 stage=3) continue to execute the
 * cycle-42A unchanged code path — those calls run AFTER
 * `WinMainCRTStartup → _PDCLIB_xbox_libc_init`, so `vsnprintf` is
 * safe at their call sites.
 *
 * Marker encoding: high-nibble = `XBED_SELF_WITNESS_EEPROM_CYCLE42D_TAG_NIB`
 * (= 0xB0), distinct from cycle-39 stage byte 0xA0..0xAF; low-nibble
 * = milestone index 0..0xB. Each marker overwrites the previous, so
 * the post-run EEPROM byte identifies the highest milestone whose
 * marker write SUCCEEDED — a LOWER BOUND on body progress, NOT an
 * exact "execution died here" boundary (see Honest framing below).
 *
 *   Byte | Milestone (stage==6 fast path)
 *   -----|--------------------------------------------------------
 *   0xB0 | shim entered from stage=6 (sticky flag set, marker
 *        | written; cycle-42D bypass committed)
 *   0xB1 | about to call MmAllocateContiguousMemory(0x2000)
 *   0xB2 | MmAllocateContiguousMemory returned non-NULL
 *   0xB3 | MmGetPhysicalAddress returned non-zero
 *   0xB4 | passed lower phys-range guard (phys >= 0x00010000)
 *   0xB5 | passed upper phys-range guard (phys < 0x04000000)
 *   0xB6 | passed page-alignment guard ((phys & 0xFFF) == 0)
 *   0xB7 | MmPersistContiguousMemory returned
 *   0xB8 | page wipe loop complete (0x2000 bytes zeroed via kseg0)
 *   0xB9 | WTNS magic + version stamped; s_witness_page registered
 *   0xBA | reserved0/reserved1 stamped + wbinvd done (full first-
 *        | call completion for stage=6 — best success signal)
 *   0xBB | idempotent reuse path (s_witness_page != 0 on entry; not
 *        | expected in the cycle-42B sequence since the thunk fires
 *        | stage=6 exactly once — defensive; would only fire if
 *        | xbed_self_witness_fire(6) is somehow called twice)
 *
 * Cycle-42D post-run EEPROM-byte interpretation table (combined
 * with `witness.scan-self count` and the cycle-42B joint
 * `(reserved0_last_stage, reserved1)` table).
 *
 * **Honest framing (Codex R1 HIGH adopted 2026-05-24):** every
 * `self_witness_cycle42d_marker` call discards `HalWriteSMBusValue`'s
 * `NTSTATUS` (there is no host-log path safe to call from pre-libc
 * context, and an error-return path that itself depends on
 * additional kernel-export calls would just re-introduce the fault
 * surface this bypass was built to escape). That means the EEPROM
 * byte observed post-run is the highest milestone whose SMBus write
 * SUCCEEDED — it is a LOWER BOUND on milestones reached, NOT an
 * exact "execution died here" boundary. Concretely, if any later
 * marker write fails silently (kernel pathological state, SMBus
 * arbitration loss, retry exhaustion inside `HalWriteSMBusValue`),
 * the byte stays at the previous successful marker even when the
 * body code continued executing past it. The table below therefore
 * lists the JOINT possible meanings for each (byte, count) shape;
 * the cycle-42D real-Xbox interpretation must enumerate ALL the
 * possibilities a given shape collapses to, then use additional
 * signals (witness.scan-self `reserved0` low byte, `reserved1`,
 * future cycle markers) to disambiguate. Single-byte uniqueness was
 * an over-claim; the bypass's true value is that the byte STRICTLY
 * INCREASES across cycle 42C's signal (0xA6) so any observed
 * `0xBn` proves that at LEAST `n+1` milestones executed, even if
 * we cannot exactly localize the failure within the post-`n`
 * remainder.
 *
 *   EEPROM | count | possible meanings (all must be enumerated)
 *   -------|-------|---------------------------------------------
 *   0xA6   | 0     | bypass NEVER landed marker 0xB0 — could be
 *          |       | (i) xbed_self_witness_fire(6) was never
 *          |       | called (thunk → shim call site faulted),
 *          |       | (ii) the function prologue / stage==6
 *          |       | dispatch / `s_eeprom_scratch_attempted = 1`
 *          |       | sequence faulted before the first
 *          |       | HalWriteSMBusValue (very unlikely — bare
 *          |       | i386 mov/cmp/jne/mov), (iii) the marker
 *          |       | helper at `xbed_self_witness.c:`
 *          |       | `self_witness_cycle42d_marker` was entered
 *          |       | but the SMBus write itself returned a
 *          |       | non-success NTSTATUS or hardware-NAK and the
 *          |       | EEPROM byte stayed at the cycle-42B thunk's
 *          |       | defensive 0xA6 pre-write, OR (iv) the marker
 *          |       | write SUCCEEDED but execution continued past
 *          |       | it and the NEXT marker write (or any later
 *          |       | marker write whose value falls in 0xB1..0xBA)
 *          |       | ALSO failed in a way that ended up restoring
 *          |       | the byte to 0xA6 — not possible with a single
 *          |       | marker call per milestone (each call writes a
 *          |       | strictly increasing low-nibble value, so the
 *          |       | byte never decreases) so this collapses to
 *          |       | (iii) for the post-0xB0 path. Sticky-flag
 *          |       | armed-ness is INDETERMINATE from this shape
 *          |       | alone (the sticky-flag set happens BEFORE the
 *          |       | marker write, so the flag may be set even
 *          |       | with EEPROM still at 0xA6). The fact that no
 *          |       | later .CRT$XXC fire wrote 0xA4 to EEPROM is
 *          |       | consistent with either (sticky-flag set,
 *          |       | later fires short-circuit) or (sticky-flag
 *          |       | not set AND .CRT$XXC fire itself never
 *          |       | reached the cycle-39 EEPROM write site — same
 *          |       | cycle-42C explanation). SAME INCONCLUSIVE
 *          |       | shape as cycle 42C, narrower residual
 *          |       | hypothesis set but not RULE-OUT.
 *   0xA4   | 0..1+ | bypass was NOT effective: sticky flag never
 *          |       | got set AND a later .CRT$XXC stage=4 fire
 *          |       | reached the cycle-39 EEPROM write site. Would
 *          |       | imply the stage==6 dispatch check faulted OR
 *          |       | the `s_eeprom_scratch_attempted = 1` mov
 *          |       | instruction faulted — both near-impossibilities
 *          |       | given the thunk's successful execution of the
 *          |       | preceding i386 instructions. NOT expected.
 *          |       | If observed, re-validate the build artifact +
 *          |       | PE entry-point disassembly.
 *   0xBn   | 0     | (where n in 0..8) AT LEAST milestone n
 *   for    |       | landed its SMBus write successfully; the body
 *   n=0..8 |       | continued executing UP TO AT LEAST that
 *          |       | point. Possible meanings: (i) execution died
 *          |       | between milestone n's body-after-marker
 *          |       | instruction and milestone n+1's body-before-
 *          |       | marker instruction (the "exact-step-boundary"
 *          |       | reading); (ii) execution continued past
 *          |       | milestone n+1's body-before-marker but the
 *          |       | marker write for n+1 returned a non-success
 *          |       | NTSTATUS (and zero or more later milestones
 *          |       | also fell to the same failure mode); (iii)
 *          |       | the body ran to a milestone in (n+1)..9 but
 *          |       | every subsequent marker write returned
 *          |       | non-success. To rule out (ii)+(iii) we would
 *          |       | need EITHER a fresh `witness.scan-self` read
 *          |       | confirming `count==0` AND `reserved0==0` AND
 *          |       | `reserved1==0` (which rules out reaching the
 *          |       | WTNS-stamp body at milestone 9 only — see
 *          |       | 0xB9 row below for the count=1 ambiguity), OR
 *          |       | a future cycle that adds a non-EEPROM
 *          |       | side-channel for milestone confirmation. For
 *          |       | the cycle-42D first run the conservative
 *          |       | reading is "AT LEAST milestone n's marker
 *          |       | write succeeded; the body executed AT LEAST
 *          |       | the pre-n-marker instructions; nothing past
 *          |       | that is proved by EEPROM alone." See the
 *          |       | per-milestone semantic notes in the milestone
 *          |       | byte table above for what each marker N
 *          |       | brackets.
 *   0xB9   | 1     | AT LEAST milestone 9 (WTNS magic stamped,
 *          |       | s_witness_page registered) landed its SMBus
 *          |       | write successfully — the WTNS page is visible
 *          |       | to the consumer scan (count=1). Possible
 *          |       | refinements: (i) execution died between
 *          |       | milestone 9's body-after-marker and the
 *          |       | reserved0/reserved1 stamp at milestone 10's
 *          |       | body-before-marker, AND `reserved0`,
 *          |       | `reserved1` stay 0 from the page wipe
 *          |       | (matches the D-cycle-27 reserved-zero shape
 *          |       | but on a SELF-allocated page, not the agent's
 *          |       | XCTR page); (ii) the reserved0/reserved1
 *          |       | stamp + wbinvd actually ran to completion AND
 *          |       | the milestone 10 (0xBA) marker write failed
 *          |       | — in which case `reserved0` shows the stage
 *          |       | byte of the most recent successful fire and
 *          |       | `reserved1` shows the count of fires reached
 *          |       | (including any later stages-!=6 reuse fires);
 *          |       | (iii) milestone 10 (0xBA) succeeded BUT a
 *          |       | hardware-level EEPROM corruption demoted the
 *          |       | byte back to 0xB9 — vanishingly unlikely on
 *          |       | the 24LC02. Disambiguating (i) vs (ii) needs
 *          |       | the `reserved0`,`reserved1` joint readback
 *          |       | from `witness.scan-self`: `(0, 0)` ⇒ (i);
 *          |       | non-zero ⇒ (ii). Either way the WTNS-stamp
 *          |       | body landed and the cycle-22 calling-context
 *          |       | hypothesis is STRONGLY supported.
 *   0xBA   | 1+    | full first-call completion for stage=6 —
 *          |       | best success signal. If subsequent
 *          |       | .CRT$XXC + .CRT$XCU + in-main WTNS1 + WTNS2
 *          |       | fires also reach the shim, count stays 1
 *          |       | (single allocation, reused page), reserved0
 *          |       | tracks the last fire's stage byte (e.g.
 *          |       | 0xA4000003 if stage=3 was last), and reserved1
 *          |       | counts total fires (5 if all reached). The
 *          |       | cycle-42B joint (reserved0_last_stage,
 *          |       | reserved1) table applies for k=1 (pre-WinMain
 *          |       | was the first successful allocator). STRONG
 *          |       | cycle-22 calling-context-CONFIRMED signal.
 *          |       | Cannot RULE OUT a later marker-write failure
 *          |       | path scenario (since 0xBA is the LAST marker
 *          |       | the bypass writes) but the bypass also
 *          |       | returns immediately after the 0xBA marker, so
 *          |       | once the byte reaches 0xBA there is no
 *          |       | further body code that could fail silently;
 *          |       | the 0xBA + count>=1 shape is unambiguous.
 *   0xBB   | 1+    | NOT expected (would only fire if
 *          |       | xbed_self_witness_fire(6) was called twice).
 *          |       | If observed, indicates a logic error in the
 *          |       | thunk or the bypass path; investigate.
 *
 * Why high-nibble 0xB0 (not extending 0xA0..0xAF): the cycle-39
 * stage byte uses high-nibble 0xA with low-nibble = stage code. We
 * already use stages 1, 3, 4, 5, 6 (and the cycle-42B thunk uses 6
 * as 0xA6). To avoid colliding with the stage-encoding namespace,
 * the cycle-42D milestone marker uses a distinct high-nibble. 0xB
 * is the next adjacent nibble; reserves room for future cycle-42E+
 * markers (0xC0..0xFF) without colliding.
 *
 * Why NOT a new EEPROM offset: offset 0xFF is the single byte the
 * agent's `eeprom.scratch.read` verb is wired to (cycle-39 design);
 * adding a new offset would require oracle-agent verb changes. The
 * 1-byte resolution is sufficient for the 12-milestone marker set.
 *
 * Why NOT add a host-log channel that doesn't depend on vsnprintf:
 * the cycle-15 `xbed_host_log_write` (non-formatting) already exists
 * and DOES NOT call vsnprintf — it just does `outb` of each byte to
 * port 0xE9. We could call that instead of skipping host-log
 * entirely. We chose to skip ALL host-log calls in cycle 42D for
 * three reasons: (i) the host-log channel only surfaces on xemu
 * (not real Xbox); cycle-42D's target is the real-Xbox calling-
 * context discriminator, and EEPROM markers ARE durable across the
 * chainload; (ii) calling outb from pre-libc context is plausibly
 * safe but unverified on this hardware, and EEPROM via
 * HalWriteSMBusValue has been EMPIRICALLY validated to work from
 * pre-WinMain context (cycle-42C thunk pre-write landed and
 * persisted); (iii) keeping the bypass minimal (zero non-kernel-
 * export calls) reduces the surface area where a pre-libc fault
 * could mask the diagnostic signal. The cycle-29 stages-!=6 path
 * still emits the existing host-log lines for xemu-side debugging.
 *
 * Preservation contract:
 *   - cycle-23 lockstep (`lib/xbed_a4_witness.{c,h}`): UNTOUCHED.
 *   - cycle-29 stages !=6 path: UNTOUCHED (same code emits same
 *     host-log lines + same allocator call + same guards + same
 *     stamp).
 *   - cycle-39 sticky-flag semantics for stages !=6: PRESERVED
 *     (the unchanged path still checks `!s_eeprom_scratch_attempted`
 *     before its EEPROM write; cycle-42D pre-sets the flag for
 *     stages !=6 ONLY when stage==6 ran first, which suppresses
 *     the cycle-39 EEPROM write — by design, since cycle-42D
 *     markers replace the cycle-39 signal when stage==6 runs).
 *   - cycle-41c/41d guards: REPLICATED inside the stage==6 bypass
 *     (same numeric thresholds; same MmFreeContiguousMemory + return
 *     0 on guard fire) AND PRESERVED unchanged in the stages-!=6
 *     path.
 *   - cycle-42A multi-page (0x2000) allocation: REPLICATED inside
 *     the stage==6 bypass AND PRESERVED unchanged in the stages-
 *     !=6 path.
 *   - cycle-42B pre-WinMainCRT thunk
 *     (`witness-only/witness_only_crt0.c`): UNTOUCHED. The thunk's
 *     defensive 0xA6 pre-write still runs FIRST (before
 *     xbed_self_witness_fire is called); cycle-42D's 0xB0 marker
 *     overwrites that pre-write on entry to the bypass. If the
 *     bypass is never entered for any reason, the EEPROM byte
 *     stays at 0xA6 — same INCONCLUSIVE shape as cycle 42C, but
 *     now distinguishable from cycle-42A (0xA4) and from cycle-
 *     42D-success (0xBA) signals. */
#define XBED_SELF_WITNESS_EEPROM_SMBUS_ADDR    0xA8u    /* 24LC02 EEPROM SMBus address; same as oracle-agent/commands.c */
#define XBED_SELF_WITNESS_EEPROM_SCRATCH_OFF   0xFFu    /* last byte of 256-byte EEPROM image; reserved/unused on stock OEM consoles */
#define XBED_SELF_WITNESS_EEPROM_TAG_NIB       0xA0u    /* high nibble = 0xA "Path-A.4 tag"; low nibble = (stage & 0x0F) */

/* Cycle-42D stage-6 milestone marker tag (high nibble = 0xB,
 * distinct from cycle-39 0xA stage byte namespace; low nibble =
 * milestone index 0..B per the table above). Written ONLY by the
 * stage==6 fast path in `xbed_self_witness_fire`. */
#define XBED_SELF_WITNESS_EEPROM_CYCLE42D_TAG_NIB  0xB0u

/* Cycle-42D stage-6 fast-path stage code. Must match
 * `witness_only/witness_only_crt0.c::WITNESS_ONLY_STAGE_PRE_WINMAIN_CRT`
 * (= 6). Defined here so the shim's dispatch check at the top of
 * `xbed_self_witness_fire` does not depend on a magic literal. */
#define XBED_SELF_WITNESS_STAGE_PRE_WINMAIN_CRT   6u

/* Cycle-42D milestone indices (low nibble of the marker byte). The
 * comments alongside the assignments inside `xbed_self_witness.c`'s
 * stage-6 bypass are the authoritative semantics; these defines
 * exist so the source uses named constants instead of magic 0..0xB
 * integers. Numbered in execution order, so the highest marker
 * observed post-run is the highest milestone whose marker write
 * SUCCEEDED — a LOWER BOUND on body progress, NOT an exact
 * "execution died here" boundary (per Codex R1.HIGH and R2.MED and
 * R3.MED — silent failure of any later 0xB? marker write leaves the
 * byte at the previous successful marker even when the body code
 * continued executing past it; see the "Cycle-42D" Honest-framing
 * subsection above for the full interpretation matrix). */
#define XBED_SELF_WITNESS_C42D_M_ENTRY                0x0u
#define XBED_SELF_WITNESS_C42D_M_PRE_ALLOC            0x1u
#define XBED_SELF_WITNESS_C42D_M_POST_ALLOC           0x2u
#define XBED_SELF_WITNESS_C42D_M_POST_PHYS            0x3u
#define XBED_SELF_WITNESS_C42D_M_POST_LOWER_GUARD     0x4u
#define XBED_SELF_WITNESS_C42D_M_POST_UPPER_GUARD     0x5u
#define XBED_SELF_WITNESS_C42D_M_POST_ALIGN_GUARD     0x6u
#define XBED_SELF_WITNESS_C42D_M_POST_PERSIST         0x7u
#define XBED_SELF_WITNESS_C42D_M_POST_WIPE            0x8u
#define XBED_SELF_WITNESS_C42D_M_POST_WTNS_STAMP      0x9u
#define XBED_SELF_WITNESS_C42D_M_FULL_COMPLETION      0xAu
#define XBED_SELF_WITNESS_C42D_M_REUSE_COMPLETION     0xBu


/* Initialize the self-witness if not already initialized, then stamp
 * the given `stage` into `reserved0` and increment `reserved1`.
 * Idempotent — repeated calls reuse the same allocated region; only
 * the first call performs `MmAllocateContiguousMemory +
 * MmPersistContiguousMemory + memset (full allocation) + first-page
 * magic/version init`.
 *
 * Current live behavior (CYCLE-42D stage-6 milestone marker bypass
 * layered on top of CYCLE-42A multi-page redesign; supersedes the
 * cycles-29..42C single-path behavior described in the Safety-notes
 * blocks above):
 *
 *   - If `stage == XBED_SELF_WITNESS_STAGE_PRE_WINMAIN_CRT` (= 6),
 *     run the cycle-42D pre-libc-safe milestone marker bypass: set
 *     `s_eeprom_scratch_attempted = 1` as the first action (lock
 *     out the cycle-39 EEPROM write for any subsequent fire);
 *     write EEPROM marker 0xB0 (entry); skip every
 *     `xbed_host_log_writef` / `xbed_host_log_write` call (the
 *     suspected vsnprintf-pre-libc fault site identified by cycle
 *     42C); execute the same cycle-42A 2-page allocation + cycle-
 *     41c phys-range guards + cycle-41d alignment guard +
 *     `MmPersistContiguousMemory` + page wipe + WTNS magic stamp
 *     + reserved0/reserved1 stamp + wbinvd sequence, emitting an
 *     EEPROM milestone marker (0xB1..0xBA) after each major step;
 *     return early so control does NOT fall through to the
 *     existing stages-!=6 host-log tail.
 *   - Otherwise (stages 1, 3, 4, 5 — the cycle-29 .CRT$XXC,
 *     .CRT$XCU, in-main WTNS1/WTNS2 fires), run the unchanged
 *     cycle-42A code path. The first call allocates a 2-page
 *     (0x2000 B, 8 KiB) contiguous region via the non-`-Ex` 1-arg
 *     `MmAllocateContiguousMemory`, persists the full 0x2000 range
 *     via `MmPersistContiguousMemory`, zeroes the full 0x2000
 *     range via the page-wipe loop, then stamps the WTNS magic +
 *     version + reserved0 + reserved1 header at offset 0 of the
 *     FIRST page. Subsequent stages-!=6 calls reuse the same
 *     first-page header. The cycle-39 EEPROM sticky-flag block
 *     still gates a 1-byte 0xA4 write at the pre-allocation point
 *     of the FIRST stages-!=6 call IF the cycle-42D stage-6
 *     bypass did NOT pre-set the sticky flag (i.e. the cycle-42B
 *     thunk was not in play, or the stage-6 bypass was never
 *     entered).
 *
 * Historical references to `MmAllocateContiguousMemoryEx` or to
 * "one page" elsewhere in this header are preserved as cycle-history
 * context (cycles 29..41d used the `-Ex` variant; cycle 41e moved
 * to the non-`-Ex` variant at 0x1000 size; cycle 42A bumped size
 * to 0x2000 while keeping the non-`-Ex` ABI; cycle 42D added the
 * stage-6 milestone marker bypass without changing the underlying
 * allocator call sequence). The most recent authoritative
 * descriptions are the "Cycle-42D" subsection above + the
 * "Cycle-42A" subsection above + this paragraph.
 *
 * Returns the physical address of the first page of the self-
 * witness allocation on success, or 0 on hard failure (allocation
 * failed OR phys-range guard fired OR alignment guard fired; no
 * fallback). Caller (e.g. `witness-only/main.c`) is expected to
 * host-log the result via `xbed_host_log_writef` for visibility
 * under `XEMU_GUEST_LOG=1`.
 *
 * Safe to call before any other init — uses only one kernel
 * allocation per process plus straight-line writes; no fopen, no
 * pbkit/NV2A dependency, no kseg0 sweep. */
uintptr_t xbed_self_witness_fire(uint32_t stage);

#endif /* XBED_SELF_WITNESS_H */
