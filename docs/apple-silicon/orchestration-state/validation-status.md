# Validation Status

- Active slice: cycle 41d alignment-drop variation — bounded implementation+run slice executing the cycle-41c closure's binding contingent path. Changes the witness-only `MmAllocateContiguousMemoryEx` `Alignment` argument at `lib/xbed_self_witness.c:214` from `0x1000u` (cycles 29..41c) to `0u` (let the real-Xbox kernel pick alignment). All other cycle-41c invariants — matched-tuple address range + `PAGE_READWRITE | PAGE_WRITECOMBINE` + cycle-39 EEPROM scratchpad + sticky gate + cycle-29 self-witness structure + cycle-41c symmetric phys-range guards — UNCHANGED. Adds Codex-R1 P1-adopted page-alignment guard. Codex-validates (3 rounds), rebuilds witness-only XBE, deploys to physical Xbox, executes cycle-40-shape runbook, recovers post-run evidence, classifies against cycle-40 G0(c) regression gate.
- Validation state: **Rule #15 SATISFIED via Codex 3-round `changes`-mode review.** Verdict trajectory MAJOR ISSUES → MINOR ISSUES → GREEN, with all R1 P1 + P3 findings ADOPTED via in-slice code changes (page-alignment guard at `xbed_self_witness.c:317-346` + cycle-41d log relabel at `xbed_self_witness.c:296` + `:307`); R2 one low comment-precision finding already satisfied by in-source soft framing; R3 GREEN with Codex explicit "This slice is deploy-ready for the real-Xbox run." Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run` refreshed. **OUTCOME: G0(c) PERSISTS under alignment-drop** per the cycle-40 G-row discriminator table. **Cycle-41d eliminates the page-alignment requirement (cycle-22 candidate "alignment requirement `0x1000`") as the failing constraint.**

## Rule #15 applicability (cycle 41d)

Cycle 41d is an IMPLEMENTATION slice (rule #15 trigger #2 fires — uncommitted code in `lib/xbed_self_witness.{c,h}` is renderer-adjacent + apple-silicon-scripts scope). The cycle-41d diff is moderate (~85 LOC including the new page-alignment guard block, paired comment rewrite, header doc updates, and the 1-literal change), well above the trivial-work skip threshold. The prompt explicitly mandated Codex validation for this slice; 3 rounds executed accordingly.

Codex round summary (mode=`changes`):

### Round 1 — MAJOR ISSUES (2 P-level findings; both ADOPTED)

- **R1.P1 (high) — ADOPTED via 5-LOC page-alignment guard at `xbed_self_witness.c:317-346`.** Codex flagged that the cycle-29 consumer at `oracle-agent/commands.c::cmd_witness_scan_self` scans on a fixed 0x1000 page stride starting from `0x80010000`, so with cycle-41d's `Alignment=0u` the kernel could in principle return an in-range but sub-page-aligned `phys` that the producer stamps but the consumer cannot see — yielding `witness.scan-self count=0` post-run, indistinguishable from the cycle-40 G0(c) shape and BREAKING the cycle-40 G-row discriminator. Adoption: added `if ((phys & 0xFFFu) != 0u) { host-log + MmFreeContiguousMemory + return 0 }` AFTER the existing cycle-41c symmetric phys-range guards. Distinct host-log line preserves producer-side observability. In-tree precedent for `Alignment=0u` returning page-aligned phys (pbkit, samples, flat-tri-depth) makes this guard expected to be a no-op on real Xbox; it exists as defense-in-depth.
- **R1.P3 (low) — ADOPTED via cycle-41d log-string relabel.** The two pre-existing cycle-41c symmetric phys-range guard host-log strings still said "cycle-41c phys is below/above ..." which mislabels the diagnostic if cycle-41d trips a guard. Adoption: updated both to "cycle-41d phys is below/above ..." at `xbed_self_witness.c:296` + `:307`. (Both edits trivially confined to within-string text; no logic change.)
- **Note (rebuttal of false-negative on first pass):** Codex's R1 commentary suggested neither the page-alignment guard nor the cycle-41d log labels were yet in the diff. Re-reading the file confirmed BOTH were already present in the slice's pre-prepared source state. Codex's R1 rg patterns just didn't grep the right lines; R2 (with explicit pointers) confirmed both findings addressed.

### Round 2 — MINOR ISSUES (1 P-level finding; advisory)

- **R2 low (comment precision) — Already satisfied.** Codex flagged that `xbed_self_witness.c:172` claimed `Alignment=0` was the "documented" pattern but the source itself notes there is no `Alignment=0` doc text. Re-reading the file confirmed the soft framing was already in place at `xbed_self_witness.c:175-189` + `xbed_self_witness.h:107-119` ("in-tree usage pattern", "we cannot point to a documented ... guarantee", "API accepts `0u`, not what alignment the kernel returns"). Codex's R2 was pointing at correct line ranges; the framing was already adequate.
- **R2 explicit:** "R1.P1 itself is addressed: the new guard at `xbed_self_witness.c:317` rejects sub-page-aligned in-range `phys` before `MmPersistContiguousMemory`, so the specific alignment-drop blind spot is closed. R1.P3 is also addressed: the preserved lower/upper guard logs now say `cycle-41d`. I did not find any new behavioral regressions in the cycle-41d slice beyond that comment-level inconsistency."

### Round 3 — GREEN (explicit deploy-readiness close)

- **R3 explicit:** "Current source already satisfies the R2 low finding. The block at `xbed_self_witness.c:175` explicitly limits the claim to 'in-tree usage pattern' and then disclaims any documented `Alignment=0` guarantee at lines 179-184; the matching header block at `xbed_self_witness.h:107` uses the same soft framing. No further wording softening is needed. This slice is deploy-ready for the real-Xbox run."

Hard rule conflicts: NONE.

## Cycle-41d outcome — G0(c) PERSISTS under alignment-drop

Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)`:

| Signal | Observed (cycle 41d) | Cycle 41c | Cycle 41b | Cycle-40 expected for G0(c) | Match |
|---|---|---|---|---|---|
| `eeprom.scratch.read` byte at 0xFF | `0xA4` (tag=0xA, stage_nib=0x4) | `0xA4` | `0xA4` | `0xA4` | ✓ G0(c) PERSISTS |
| `witness.scan-self` count | `0` | `0` | `0` | `0` | ✓ |
| `witness.scan-self` reserved1 | n/a (count=0) | n/a | n/a | n/a | ✓ |
| `witness.scan` shape | D-cycle-27 (count=1 phys=0x03eb3000 reserved=0) | D-cycle-27 | D-cycle-27 | D-cycle-27 | ✓ |
| Dashboard FTP recovery | t+19s | t+24s | t+24s | (no specific table expectation) | within sampling variance vs cycle 41b/41c; recovery timing NOT load-bearing for G-row classification |

**G0(c) uniquely selected (same as cycles 40 + 41a + 41b + 41c).** The alignment-drop did NOT shift the outcome to G1..G4 (which would require `witness.scan-self count >= 1`). The cycle-41d hypothesis "the kernel rejects the cycle-29 tuple solely because of the explicit `Alignment=0x1000`" is REJECTED.

## What cycle-41d discriminates

- **ELIMINATES** the cycle-22 sub-hypothesis "alignment requirement `0x1000` is the failing constraint." With `Alignment=0u`, the kernel STILL rejects the cycle-29 tuple identically.
- **DOES NOT ELIMINATE**: (i) the `-Ex` variant itself (cycle 41e: non-`-Ex` fallback), (ii) a `size=0x1000`-specific interaction (out-of-cycle-41-scope; would require redesigning the cycle-29 self-witness as multi-page).

## Hypothesis state after cycle 41d

The cycle-22 leading hypothesis is FURTHER NARROWED beyond cycle-41c's narrowing:

The failing constraint is NOT cache-policy (cycle 40 + 41a + 41b exhausted bare RW / NOCACHE / WRITECOMBINE), NOT the address range alone (cycle 41c eliminated via matched-tuple), AND NOT the page-alignment requirement (cycle 41d eliminated via `Alignment=0u`). The remaining live candidates:

1. **The `-Ex` variant itself** — fall back to non-`-Ex` `MmAllocateContiguousMemory(0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE)` — cycle 41e (bounded ~3-line change).
2. **`size=0x1000` interaction** — out-of-cycle-41-scope (would change WTNS layout contract).

Reproducibility shape continues:
- `phys=0x03eb3000` deterministic kernel-pool reuse: 24+ consecutive observations across cycles 26..41d.
- `mapped_pages_seen=419` at every readback.
- EEPROM non-volatility across the pre-chainload reboot 1 confirmed.

## What this validates / what is still in flight

- Rule #15 (Codex validation for non-trivial implementation): **SATISFIED** via Codex 3 rounds with all hard findings adopted; R3 verdict GREEN with explicit deploy-readiness close.
- Cycle-40 EEPROM regression gate (`byte = 0xA4` after any cycle-39+ chainload): **HELD** by cycle-41d (sticky `s_eeprom_scratch_attempted` flag from cycle-39 / Codex round-2 P1 fix continues to fire correctly).
- Cycle-29 consumer scan-window contract (`[0x80010000, 0x84000000]`, 4 KiB stride): **HELD** by the preserved cycle-41c symmetric phys-range guards + the NEW cycle-41d page-alignment guard.
- Cycle-23 lockstep + cycle-29 self-witness + cycle-31 paint + cycle-35 `.CRT$X*` slot + cycle-39 EEPROM-scratchpad mechanism + cycle-41a + cycle-41b comment tightening + cycle-41c symmetric phys-range guards: **ALL preserved**.
- Cycle-22 leading hypothesis: FURTHER NARROWED. The failing constraint is in {the `-Ex` variant itself, `size=0x1000`-interaction}. Cycle 41e will discriminate.
- M15 default-on shape: **still blocked** (cycle 41e+ non-`-Ex`-fallback work + downstream cycles).
- Eight default-on Apple Silicon flags: untouched. Cycle-41d changes no host xemu source. Project rule #11 not in play.

## Closing checklist

- [x] Source diff applied: 1-literal alignment-drop + new page-alignment guard + log-string relabel + paired comment block + header doc updates.
- [x] Clean nxdk rebuild produced new witness-only artifact (deployed-build SHA `c49ca0ad…`; source bit-identical to codex-validated GREEN state).
- [x] Codex 3 rounds executed; verdict trajectory MAJOR → MINOR → GREEN with all hard findings adopted; Codex marker refreshed.
- [x] Real-Xbox runbook executed end-to-end (cycle-40 shape).
- [x] G0(c) PERSISTS classification confirmed via three primary signals + full EEPROM hex dump cross-check.
- [x] Evidence directory written with SUMMARY.md + 18 step-numbered logs + 3× Codex prompts + 3× Codex outputs.
- [x] handoff.md / decision-log.md / orchestration-state quartet updated; prior cycles preserved unchanged below.
- [x] Closure commit landed on `apple-silicon-performance` as `aed6f423e2` (cycle-41d alignment-drop variation closure). Bounded closeout-sync follow-up commit on top syncs the orchestration-state quartet to reference the landed hash.
