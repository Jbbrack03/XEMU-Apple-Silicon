# Validation Status

- Active slice: cycle 41b `PAGE_WRITECOMBINE` allocation-flag variation — bounded implementation+run slice executing the cycle-41a closure's recommended NEXT cycle-41 candidate. Changes `MmAllocateContiguousMemoryEx`'s `Protect` argument in `lib/xbed_self_witness.c:158` from `PAGE_READWRITE | PAGE_NOCACHE` (cycle-41a) to `PAGE_READWRITE | PAGE_WRITECOMBINE`. Codex-validates, rebuilds witness-only XBE, deploys to physical Xbox, executes cycle-40-shape runbook, recovers post-run evidence, classifies against cycle-40 G0(c) regression gate.
- Validation state: **Rule #15 SATISFIED via Codex round-1 v3 `changes`-mode review.** Verdict=**GREEN**; 3 P3 findings, all confirmations (no hard-rule conflicts; flag-value verification; precedent verification; no new coherency hazard). Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed. **OUTCOME: G0(c) PERSISTS under `PAGE_WRITECOMBINE`** per the cycle-40 G-row discriminator table. **Cache-policy variations EXHAUSTED.**

## Rule #15 applicability (cycle 41b)

Cycle 41b is an IMPLEMENTATION slice (rule #15 trigger #2 fires — uncommitted code in `lib/xbed_self_witness.{c,h}` is renderer-adjacent + apple-silicon-scripts scope). Even though the aggregate diff is well under the 30-LOC trivial-work threshold (1-line `Protect` change + paired comment + header doc updates), the prompt explicitly mandated Codex validation for this slice ("Treat Codex validation as REQUIRED for this implementation slice even if the diff is small; adopt or explicitly deflect findings in the docs before closing."). Codex round-1 executed accordingly.

Codex round-1 summary (mode=`changes`):
- v1 attempt: `codex-prompt.md` / `codex-output.md` got stuck in a `web_search` loop the read-only sandbox cannot service (codex tried to verify wbinvd-vs-WC Intel SDM semantics via web search; all calls failed silently; output truncated at ~1225 lines).
- v3 retry (load-bearing): `codex-prompt-v2.md` / `codex-output-v3.md` with explicit "DO NOT use web search; nxdk lives at `../nxdk/...`" guidance + concise output budget. Verdict=**GREEN** in 41,456 tokens.
- P3.1 No hard-rule conflicts. The diff is confined to the `Protect` swap and matching comment/header text in `xbed_self_witness.c:128` and `xbed_self_witness.h:103`; does not touch PR #2240 guidance, add any new `XEMU_*` runtime flags, or blur the diagnostic/stable flag split.
- P3.2 Flag values verified: `PAGE_NOCACHE = 0x200` and `PAGE_WRITECOMBINE = 0x400` are distinct non-overlapping cache-policy bits per `nxdk/lib/xboxkrnl/xboxkrnl.h:3294`. Precedent verified: `nxdk/lib/hal/video.c:363` calls `MmAllocateContiguousMemoryEx(..., PAGE_READWRITE | PAGE_WRITECOMBINE)` for the framebuffer.
- P3.3 No new coherency hazard introduced by switching `p` from `PAGE_NOCACHE` to `PAGE_WRITECOMBINE`. After allocation, the code gets `phys`, marks persistence on `p`, then canonicalizes to `vp = (phys | 0x80000000)`; all page zeroing, header writes, stage/counter stamping, and `wbinvd` operate through `vp`, not through `p` (`xbed_self_witness.c:165`). The consumer side also scans and reads WTNS pages via kseg0 virtual addresses (`oracle-agent/commands.c:726`). Since the diff leaves that cached-alias producer/consumer path unchanged, this is the same cycle-41a alias separation, not a new WC-specific hazard.

Hard rule conflicts: NONE.

## Cycle-41b outcome — G0(c) PERSISTS under `PAGE_WRITECOMBINE`

Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)`:

| Signal | Observed (cycle 41b) | Cycle-41a | Cycle-40 expected for G0(c) | Match |
|---|---|---|---|---|
| `eeprom.scratch.read` byte at 0xFF | `0xA4` (tag=0xA, stage_nib=0x4) | `0xA4` | `0xA4` | ✓ G0(c) PERSISTS |
| `witness.scan-self` count | `0` | `0` | `0` | ✓ |
| `witness.scan-self` reserved1 | n/a (count=0) | n/a | n/a | ✓ |
| `witness.scan` shape | D-cycle-27 (count=1 phys=0x03eb3000 reserved=0) | D-cycle-27 | D-cycle-27 | ✓ |
| Dashboard FTP recovery | t+24s | t+8s | (no specific table expectation) | +16s vs cycle 41a — NOT dismissable as sampling variance; still faster than cycle-36 t+38s graceful; recovery timing NOT load-bearing for G-row classification |

**G0(c) is uniquely selected (same as cycles 40 + 41a).** `PAGE_WRITECOMBINE` cache-policy bit did NOT shift the outcome to G1..G4 (which would require `witness.scan-self count >= 1`). The cycle-41b hypothesis "WRITECOMBINE is the cache-policy bit the kernel demanded" is REJECTED.

## What cycle-41b discriminates

- ELIMINATES candidate "cache-policy WRITECOMBINE recovers the allocation". Combined with cycle 41a's elimination of NOCACHE + cycle 40's elimination of bare RW, **CACHE-POLICY VARIATIONS ARE EXHAUSTED**.
- DOES NOT ELIMINATE: broader address range (cycle 41c); lower / different alignment (cycle 41d); fallback to non-`-Ex` variant `MmAllocateContiguousMemory` (cycle 41e). At least one of these is the failing constraint.

## Hypothesis state after cycle 41b

The cycle-22 leading hypothesis is FURTHER NARROWED beyond cycle-41a's narrowing: the failing constraint is NOT a cache-policy bit at all. It is one of:

1. **Address-range floor** `0x00010000` — nxdk's framebuffer allocator uses `lowest=0x00000000` and is known-good. Could the kernel demand "lowest=0" for this kind of allocation?
2. **Address-range ceiling** `0x03FFFFFF` — nxdk's framebuffer allocator uses `highest=0x7FFFFFFF`. Could a tighter ceiling be tripping a kernel-internal validator?
3. **Alignment requirement** `0x1000` — the framebuffer allocator uses `0x1000` too, so this seems less likely the failing axis, but cycle 41d will test `alignment=0` if needed.
4. **The `-Ex` variant itself** — non-`-Ex` `MmAllocateContiguousMemory(0x1000)` (cycle 41e) is the ultimate fallback.

Reproducibility shape continues:
- `phys=0x03eb3000` deterministic kernel-pool reuse: 22+ consecutive observations across cycles 26..41b.
- `mapped_pages_seen=419` at every readback.
- EEPROM non-volatility across the reboot 1 (pre-deployment) confirmed.

## What this validates / what is still in flight

- Rule #15 (Codex validation for non-trivial implementation): **SATISFIED** via Codex round-1 v3 GREEN.
- Cycle-40 EEPROM regression gate (`byte = 0xA4` after any cycle-39+ chainload): **HELD** by cycle-41b (sticky `s_eeprom_scratch_attempted` flag from cycle-39 / Codex round-2 P1 fix continues to fire correctly).
- Cycle-23 lockstep + cycle-29 self-witness + cycle-31 paint + cycle-35 `.CRT$X*` slot + cycle-39 EEPROM-scratchpad mechanism + cycle-41a Codex-adopted comment tightening: **ALL preserved**.
- Cycle-22 leading hypothesis: FURTHER NARROWED. The failing constraint is in {address-range floor, address-range ceiling, alignment, `-Ex` variant}. Cycle 41c+ will discriminate.
- M15 default-on shape: **still blocked** (cycle 41c+ address-range variations + downstream cycles).
- Eight default-on Apple Silicon flags: untouched. Cycle-41b changes no host xemu source. Project rule #11 not in play.

## Closing checklist

- [x] Source diff applied and matches the prompt's intent (1-line `Protect` change + paired comment + header doc updates).
- [x] Clean nxdk rebuild produced new witness-only artifact (SHA `fbd828a2…`).
- [x] Codex round-1 v3 executed; GREEN verdict; 3 P3 confirming findings; Codex marker refreshed.
- [x] Real-Xbox runbook executed end-to-end (cycle-40 shape).
- [x] G0(c) PERSISTS classification confirmed via three primary signals + full EEPROM hex dump cross-check.
- [x] Evidence directory written with SUMMARY.md + 18 step-numbered logs + Codex artifacts.
- [x] handoff.md / decision-log.md / orchestration-state quartet updated; prior cycles preserved unchanged below.
- [ ] Closure commit on `apple-silicon-performance` (next step in this session).
