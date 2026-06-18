# Handoff

## Current state

- Canonical direction: see `metal-parity-roadmap.md`.
- Live progress / coverage: see `xbe-coverage-matrix.md`.
- Binding decisions: see `decision-log.md`.
- Older handoff history (pre-2026-05-21 cycle-1-closure and the interleaved
  banner/Metal-slice/back-history sections) was relocated verbatim to
  `_archive/handoff-history/handoff-pre-2026-05-21.md`. Nothing was summarized
  or deleted.
- The 2026-06-15 workflow change (Claude Code operating solo; Codex external
  validation decommissioned and Hermes orchestration archived) is recorded in
  `decision-log.md`.

### Session 2026-06-18 (late) — §C.3 polygon-offset XBE shipped; CONFIRMED Metal depth-bias gap; doc reconciliation

**This is the head of the next-session reading order. Read the ordered NEXT-SESSION list below FIRST.**

**Done this session (all data-verified):**
- **§C.3 `polygon-offset` XBE authored + built (v0.2)** — Tier-1, 8-cell 4×2
  grid isolating the NV2A polygon-offset / depth-bias path
  (`NV097_SET_POLY_OFFSET_FILL_ENABLE` 0x0338 +
  `NV097_SET_POLYGON_OFFSET_SCALE_FACTOR` 0x0384 →`NV_PGRAPH_ZOFFSETFACTOR`
  + `NV097_SET_POLYGON_OFFSET_BIAS` 0x0388 → `NV_PGRAPH_ZOFFSETBIAS`).
  Per cell a GREEN base quad at clip z=0.5 is overdrawn by a RED quad with
  the cell's offset config under LEQUAL; RED survives iff its biased depth
  wins. In this fork (PR #2240 / `XEMU_NATIVE_TRI_DEPTH`) polygon offset is
  applied in the **fragment shader** (`glsl/psh.c`: `zvalue += depthOffset;
  zvalue += depthFactor*nativeTriMZ`), NOT via `glPolygonOffset`/`setDepthBias`;
  Metal consumes the same generated GLSL so GL and Metal MUST agree.
- **CONFIRMED Metal renderer gap (the whole point of the XBE).** Metal renders
  only the 5 offset-independent cells correctly (signal_match 62.5% = 5/8) and
  gets the 3 offset-dependent cells wrong, across **2 cold runs**
  (`benchmark-runs/polygon-offset-metal-run{1,2}`, 136/137 frames each). The
  symptom is `depthOffset` / `depthFactor` reaching the Metal fragment shader as
  **0 at runtime** while GL applies them. The XBE manifest declares
  `expected_fail_renderers: ["xemu/metal"]`; the two Metal run summaries are
  recorded as `expected_fail` (manifest was marked after the runs, so the
  harness's own fail→expected_fail downgrade is reproduced in the run records).
  GL run `polygon-offset-gl-run1` reads FAIL too — that is a **GL screencapture
  FLIP_STALL / resolution-scale artifact** (candidate captured at 1416×1160, no
  clean flip-stall frame), NOT a GL correctness problem; tracked as task #11.
- **Metal gap narrowed to 2 hypotheses** (NOT yet fixed — this is the
  recommended next action): (a) the offset uniforms are gated/zeroed at the PSH
  emission gate `hw/xbox/nv2a/pgraph/glsl/psh.c:1764-1778`, vs (b) they are
  computed correctly but the Metal uniform **staging** path
  `hw/xbox/nv2a/pgraph/mtl/uniform.c:348-416` never writes them into the buffer
  the fragment shader reads. The decisive next diagnostic is below.
- **Coverage matrix regenerated** (generator, not hand-edited) including the
  polygon-offset run dirs: **Metal PASS 15, xfail 2 (`logic-ops`,
  `polygon-offset`), FAIL 0, skip 1 (`pipeline-smoke`)**; feature surfaces
  **40/91 covered, 35 green on Metal, 51 uncovered** (§C.3 now counts as
  covered-but-Metal-xfail; was 52 uncovered after alpha-test).
- **Tooling (this session):** Tier-4 `capture_blob` board-scoping fix
  (pipeline-smoke → XOSS oracle, task #7); a **CORR-1** MSAA sub-rect
  color-clear latent hazard documented inline in `mtl/surface.mm` and filed
  (task #9).

**NEXT SESSION — do these IN ORDER (the §C.3 Metal fix is the recommended next action):**

1. **FIX the Metal polygon-offset / depth-bias gap (§C.3) — task #10.** (The
   live task board's #10 now points at this fix; the earlier "task #10 =
   stencil-ops" in the session entry below is CLOSED and the number was reused.)
   This is
   a LIVE retail-affecting renderer-correctness bug (any title using polygon
   offset / depth bias — decals, shadow-acne avoidance — misrenders on Metal).
   - **Decisive diagnostic FIRST (cheap, no guessing):** add `depthOffset` and
     `depthFactor` to the `metal_psh_uniform_diag` print at
     `hw/xbox/nv2a/pgraph/mtl/uniform.c:374-391` and re-run
     `polygon-offset` on Metal via `xbe-harness`. If the diag prints the
     correct non-zero values, the gate at `glsl/psh.c:1764-1778` is fine and the
     bug is downstream of staging (shader-side / buffer-layout); if it prints 0,
     the bug is at the gate or upstream uniform computation. This single print
     distinguishes the two hypotheses (gate vs staging).
   - **Rule #6 caution:** the fix MUST preserve the PR #2240 fragment-shader
     depth path (`zvalue += depthOffset; zvalue += depthFactor*nativeTriMZ`).
     Do NOT strip it or revert to `glPolygonOffset`-style host-API bias — replace
     the broken plumbing, keep the behavior.
   - After the fix, re-run `polygon-offset` on Metal (expect 8/8 PASS), flip the
     manifest's `expected_fail_renderers` to `[]`, regenerate the matrix, and run
     `metal-canary-regress.sh --mode counters` as the regression gate.
2. **polygon-offset XBE v0.3 redesign — task #11.** Cells 3 and 5 are currently
   clip-cull (geometry off-screen) rather than true bias discriminators; redesign
   so all 8 cells isolate the bias/factor terms. Also close the GL FLIP_STALL
   capture gap so `polygon-offset-gl-run1` produces a clean reference frame
   (GL is correct; the harness just can't capture its flip-stall frame at scale).
3. **Continue M-I feature saturation — task #6.** Attack the remaining 51
   uncovered NV2A surfaces in `xbe-coverage-matrix.md`, one isolated feature at a
   time per `diagnostic-xbe-plan.md` §5. Next clean candidates after
   polygon-offset: A.1 vertex shader, A.2 FFP, E.8 palettized, F.5 dithering —
   pick by the plan's `(priority, complexity)` order. No retail-title tuning
   (rule #17).
4. **Real-Xbox Tier-4 XOSS fresh-capture infra — task #8.** `D:\` write not
   landing from the `E:\Apps` deploy; plus `xbe_compare` frame-selector
   hardening for tiny-signal references. Non-blocking.
5. **MSAA + SET_CLEAR_RECT sub-rect color-clear isolating XBE + the CORR-1
   verified fix — task #9.** Build the XBE that isolates the documented
   `mtl/surface.mm` color-clear sub-rect hazard, then land + verify the fix.

**Do NOT redo / known limits:** the 8 closed Apple-Silicon flags stay closed
(rule #11). GL ships as user-facing default until M15. The GL `polygon-offset`
FAIL is a capture artifact, not a correctness bug — do not "fix" the GL renderer
for it. Do not re-run the full XBE board to reconcile §C.3 — the board +
alpha-test + polygon-offset run dirs are the authoritative inputs to the matrix
generator.

---

### Session 2026-06-18 — task #10 CLOSED (stencil-ops PASS); root cause was a guest XBE vertex race, not a renderer clear bug

**Done this session (all data-verified):**
- **Task #10 root cause was MISDIAGNOSED in the Jun-15 handoff and is now
  corrected.** `stencil-ops` failed on Metal because of a **guest-side XBE
  vertex-buffer-reuse race in `stencil-ops/main.c`**, NOT a renderer
  clear-rect bug. The XBE reused a single 6-vertex VRAM buffer, overwriting it
  in place per cell and drawing at `start=0` with no inter-cell GR drain. The
  host renderer reads vertices lazily from guest VRAM at flush, so TCG raced
  ahead of PGRAPH and every op/probe draw decoded the **last** cell's geometry
  → only the last cell rendered. This is the **same renderer-agnostic
  PFIFO/vCPU vertex-race class already confirmed for `image-blit`** (cycle-15).
  Cross-renderer proof: GL renders the last 2 cells, Metal the last 1 — same
  mechanism, pure timing.
- **The Metal `SET_CLEAR_RECT` sub-rect clear path is CORRECT** (instrumented
  proof: scissor is exactly the per-cell 160×240 rect across 3184 clears; the
  clear is stencil/depth-only and never touches color). It did NOT cause the
  bug; its per-clear sync only widened the timing window and exposed the latent
  XBE bug. The scaffold is retained; renderer unchanged by the fix.
- **Fix (landed):** write-once vertex buffer in `stencil-ops/main.c` (mirrors
  `blend-matrix`; 96 verts, op at `idx*12`, probe at `idx*12+6`, built +
  memcpy'd once before the cell loop). `stencil-ops` now PASSes **deterministic
  8/8 on Metal** across 3 boots + an `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1`
  cross-check.
- **Renderer cleanups landed (validated behavior-preserving):** removed the
  dead `write_mask` generality from the clear-quad pipeline cache (only ever
  `MTLColorWriteMaskAll`) + corrected the overclaiming `k_clear_quad_msl`
  comment; removed leftover `metal_clear_subrect` instrumentation. Clean build:
  shader-validation 7/7 PASS, codesign valid, canary 4/4 green.
- **`pipeline-smoke` board-scoping fix (task #7):** the Tier-4 `capture_blob`
  XBE is now correctly **SKIPPED** on the xemu drawable board and routed to its
  real-Xbox XOSS oracle (`xbe_orchestrator.py` gates on
  `XbeManifest.is_tier4_capture_blob`). The drawable board structurally can't
  validate a CPU-paint-and-reboot XBE — its prior `FAIL` was a FALSE fail, not
  a regression.
- **Coverage matrix regenerated** from the post-cleanup board run +
  fresh pipeline-smoke (skip) and stencil-ops (PASS) cells. Board at the
  stencil-ops close (before the alpha-test slice below):
  **14 PASS / 0 FAIL / 1 xfail (`logic-ops`) / 1 skip (`pipeline-smoke`,
  XOSS-routed) / 1 not-built (`msaa-aa-factor`)** on Metal across the
  first-wave priority XBEs; 38/91 surfaces covered, 53 uncovered.
  (Later this session the §F.2 `alpha-test` second-wave slice moved this to
  **15 PASS** / 39 covered / 52 uncovered — see the alpha-test bullet below.)
  `swizzle-mipmap`'s `expected_fail` is GL-only (Metal expected-pass) per its
  manifest, so it reads PASS on the Metal column. `logic-ops` is xfail on both
  renderers.
- **M-I second-wave slice 1 — `alpha-test` (§F.2) authored + PASS 8/8 on Metal.**
  New §F.2 alpha-test isolation XBE (8-cell func/ref grid:
  NV097_SET_ALPHA_TEST_ENABLE + ALPHA_FUNC + ALPHA_REF; 0-indexed PshAlphaFunc
  decode, A==ref boundary discriminators). **PASS 8/8 on Metal across 2 cold
  runs** (`benchmark-runs/alpha-test-metal-run{1,2}`, signal_match 100%); the
  math-derived `expected.py` oracle was independently validated as correct;
  **no renderer change involved** — a genuine coverage win and the first M-I
  second-wave slice. Matrix regenerated: §F.2 now covered, **coverage 53→52
  uncovered** (39/91 surfaces have ≥1 XBE; Metal PASS cells 14→15). The M-I
  burndown continues; next natural M-I candidate per `diagnostic-xbe-plan.md`
  §5 is **§C.3 polygon offset / depth bias**.

**Open follow-ups (non-blocking, task #8):**
- Real-Xbox Tier-4 XOSS fresh-capture infra (`D:\` write not landing from the
  `E:\Apps` deploy).
- `xbe_compare` frame-selector hardening for tiny-signal references (so a
  happenstance single-white-pixel dashboard frame can't score signal=100).

---

### Session 2026-06-15 — M0 COMPLETE; M-I in progress

**Done this session (all data-verified):**
- **M0 foundations + visibility complete.** New `scripts/apple-silicon/xbe-coverage-matrix.py`
  generates the regenerable NV2A feature-surface board → `xbe-coverage-matrix.md`
  (re-run it after any XBE harness run; do not hand-edit the .md). New
  `METAL_CLEAR_SYNC_US_TOTAL`/`_COUNT` counters (`surface.mm`, `xemu-metal-perf.c`,
  `extract-perf-summary.sh`, `automation.md`) — measured **~1.08 ms CPU stall per
  synchronous clear**, the lead suspect for Metal's worse p99 jitter (Workstream C).
- **Docs restructured losslessly:** `handoff.md` 13.2k→3.8k lines, `decision-log.md`
  15.5k→89 lines; pre-2026-06 history verbatim under `_archive/` (heading-multiset proven).
- **Real Metal XBE baseline (Jun-9 binary):** 15 PASS / 1 FAIL (`stencil-ops`) / 1 xfail
  (`logic-ops`) / 1 not-built (`msaa-aa-factor`); 38/91 surfaces covered, **53 uncovered**.
  See `benchmarks/2026-06-15-m0-baseline-and-clear-sync.md`.
- **Real-Xbox oracle access restored:** `oracle-orchestrator.py ensure-agent` (agent does
  NOT auto-launch). Capture-path bug fixed (`xbed_capture.c`: D:\→`E:\Apps\<id>\`).
  **Finding:** diagnostic XBEs CRASH on real hardware during render (stencil-ops dies
  pre-capture; image-blit dies at its blit) → real-Xbox diag-XBE goldens BLOCKED (task #12,
  not critical path). Real-Xbox oracle's working use is RETAIL validation. See
  `benchmarks/2026-06-15-real-xbox-oracle-access-and-xbe-crash.md`.

**NEXT SESSION — SUPERSEDED by the `Session 2026-06-18 (late)` head at the top of this file. Read that ordered list first; the list below is retained for continuity.**
1. **Task #10 CLOSED 2026-06-18 — see the `Session 2026-06-18` entry above.** The
   root cause was NOT a renderer clear-rect bug: it was a guest-side XBE
   vertex-buffer-reuse race in `stencil-ops/main.c`. The `SET_CLEAR_RECT`
   sub-rect clear path is correct and retained. Fix was a write-once vertex
   buffer in the XBE; `stencil-ops` now PASSes deterministic 8/8 on Metal.
   The "scissor Y-origin flip" hypothesis and the "do NOT regenerate the
   matrix from the defective build" warning are both withdrawn (no defective
   build exists). No further action here.
2. **Continue M-I feature saturation** (task #5): attack the 53 uncovered surfaces in
   `xbe-coverage-matrix.md`, build `xbed_lib` infra as needed. One feature at a time,
   isolated; no retail-title tuning (rule #17).
3. **Real-Xbox = retail acceptance only for now** (M-IV / task #8). ALWAYS `ensure-agent`
   first. NEVER eyeball a scene — use the deterministic comparison tools
   (`xemu_frame_correctness_oracle.py` / `compare-screenshots.py` / harness real-Xbox golden).
   GL and math `expected.py` are NOT oracle-grade; the real Xbox is the only truth, but
   math oracle is spec-authoritative for well-specified features (stencil/blend/depth/logic).
4. **Metal jitter (Workstream C / task #7):** the `METAL_CLEAR_SYNC_US_TOTAL` counter is the
   lead — measure it on a tracked retail title, evaluate the `XEMU_METAL_NO_CLEAR_SYNC=1`
   fence-only path.

**Do NOT redo / known limits:** rebuild-all-XBEs is deferred until the hardware-crash issue
(task #12) is addressed; the capture-path fix is ready for then. The 8 closed Apple-Silicon
flags stay closed (rule #11). GL ships as the user-facing default until M15 (task #9).

---

## 2026-05-24 (cycle 42A multi-page cycle-29 self-witness redesign — bounded implementation+run slice CLOSED on `apple-silicon-performance`) — OUTCOME G0(c) PERSISTS under 0x2000 multi-page allocation; STRONG evidence AGAINST cycle-22 branch (c) being the failing axis at the smallest multi-page step; cycle-22 candidate set within in-XBE/lib-only scope now EXHAUSTED; cycle 42 candidate B (pre-`_start` callback via `nxdk/tools/cxbe/`) is the necessary follow-up

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "redesign the cycle-29 self-witness allocation so the PRODUCER requests a multi-page contiguous region instead of a single 0x1000 page, with the smallest scope that genuinely tests the remaining branch-(c) hypothesis. Update the CONSUMER side only as needed so witness.scan-self can still find and classify the producer stamp unambiguously after the size change. Preserve the cycle-23 lockstep contract, cycle-39 EEPROM scratchpad discriminator path, sticky gate, cycle-41c symmetric phys-range protections, and cycle-41d alignment-interpretation protections. Keep the experiment tightly scoped to the size axis."

**Outcome G0(c) PERSISTS under 0x2000 multi-page allocation.** Post-chainload signals identical to cycles 40 + 41a..41e: EEPROM byte at 0xFF = `0xA4` + `witness.scan-self count=0 mapped_pages_seen=419` + `witness.scan = D-cycle-27` shape (phys=0x03eb3000). `MmAllocateContiguousMemory(0x2000u)` STILL did NOT yield a usable allocation on this real-Xbox kernel from the cycle-29 `.CRT$XXC` slot calling context. Dashboard FTP recovery at t+36s post-chainload (slower than cycles 41a..41e t+6..24s but consistent with cycle 36 t+38s graceful HalReturnToFirmware shape; recovery timing NOT load-bearing for G-row classification).

**Source change (size literal at 3 sites + matched persist + comment/header doc + 3 guard log relabels + consumer doc note).** `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c`: allocator request bumped `0x1000u → 0x2000u` (2 pages, 8 KiB); `MmPersistContiguousMemory(p, 0x2000u, TRUE)` bumped to match; page-wipe loop bound bumped to `0x2000u / sizeof(uint32_t)` so neither page accidentally matches the WTNS magic predicate; allocator-failure host-log line reworded "MmAllocateContiguousMemory (non-Ex; cycle-42A 0x2000 multi-page) failed"; cycle-42A bounded-variation comment block prepended above the preserved cycle-41E historical block in the first-call branch, documenting the multi-page rationale + the discriminator semantics + the Honest-framing envelope (success → STRONG-but-not-conclusive evidence FOR branch (c); failure → STRONG evidence AGAINST size being the operative axis at the smallest multi-page step). Persist comment notes the 0x2000 match. Wipe-loop comment notes the full-allocation wipe ensures the second page fails the magic predicate at the consumer's next 0x1000-stride read. Cycle-41c symmetric phys-range guards + cycle-41d page-alignment guard PRESERVED unchanged with log strings relabeled `cycle-41e → cycle-42A` and updated to note the first-page-only nature of the check (the magic + header live exclusively on the first page, so first-page visibility is sufficient for consumer scan-self to find the stamp). Cycle-29 first-call branch / cycle-39 EEPROM-write breadcrumb + sticky `s_eeprom_scratch_attempted` gate / `wbinvd` stamp / `phys | 0x80000000` cached-mirror alias for the readback path: ALL UNCHANGED. `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.h`: top-line allocation size description updated `0x1000 → 0x2000` with forward pointer to the new Cycle-42A subsection; cycle-29 option-(c) overview paragraph updated to distinguish cycle-29 original design (1-page, `-Ex`) from cycle-42A current live behavior (2-page, non-`-Ex`); ~95-LOC new "Cycle-42A" subsection appended to the Safety-notes block (authoritative current-behavior description + discriminator semantics + Honest-framing envelope + consumer-side impact + guard-adjustment rationale); function-contract docstring rewritten with explicit "Current live behavior (CYCLE-42A multi-page redesign; supersedes the cycles-29..41e single-page behavior described in the Safety-notes block above)" paragraph + history-fence pointer to the Cycle-42A subsection. `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c::cmd_witness_scan_self` block COMMENT-ONLY updated (NO logic change): added a cycle-42A producer-side-change note clarifying that the producer now allocates 2 pages but the consumer's per-page 0x1000-stride scan still reports `count=1` per allocation (only first page carries WTNS magic; second page is zero-filled and fails the magic predicate). The `count>=2` paragraph in the same comment block updated to say "ONE persistent allocation" per run (not "one persistent page"), with a cycle-42A note clarifying that `count` grows by exactly 1 per cycle-42A allocation while `mapped_pages_seen` is the kseg0 survey counter (NOT a per-allocation counter; ~419 stable across runs on retail Xbox).

**Codex 4-round review summary** (mode=`changes`).
- **Round 1: MINOR ISSUES.** MED "header docstring + several comments still describe the live implementation as one-page `MmAllocateContiguousMemoryEx` even though the code now uses two-page non-`-Ex`" ADOPTED via function-contract docstring rewrite + cycle-29 option-(c) overview history-fence. LOW "consumer `count>=2` comment said each run leaks 1 page (now 2 pages per allocation)" ADOPTED via consumer-comment rewrite.
- **Round 2: MINOR ISSUES.** R1 findings confirmed addressed. 1 new MED "cycle-42A 'not conclusive' example is logically impossible in both `.c` and `.h` copies: it says a shared allocator artifact could leave 'a 2-page contiguous run free when no 1-page hole was available,' but any free 2-page contiguous run necessarily contains a free 1-page hole — the example cannot explain '0x2000 succeeds after 0x1000 failed'." ADOPTED in both `.c` and `.h` via replacement with internally-distinct-code-path rationale: the kernel allocator may route single-page and multi-page contiguous requests through DIFFERENT internal code paths (size-bucketed free lists, separate pool arenas, distinct minimum-size policies for contiguous-memory allocations from a pre-`main()` calling context); a 2-page success could reflect that internal code-path divergence rather than a "kernel-validation rejects size=0x1000 specifically" rule. Each replacement notes the Codex R2 adoption in-source.
- **Round 3: NOT-GREEN.** R2 MED confirmed addressed; replacement allocator-rationale is logically sound. 1 new MED "new `commands.c` note says `mapped_pages_seen` 'grows by 2 per retained cycle-42A allocation,' but the implementation increments `mapped_pages_seen` for every mapped scan page before the WTNS filter, then reports it separately from `count` — that counter is not an allocation counter in this function, so the new interpretation text is inaccurate and can mislead real-run readback analysis." ADOPTED via rewrite of the consumer comment to reaffirm `count` grows by exactly 1 per cycle-42A allocation (only first page carries WTNS magic), clarify `mapped_pages_seen` is the kseg0 survey counter (every page in scan window with non-zero MmGetPhysicalAddress), note retail Xbox kseg0 identity mapping for physical RAM is generally persistent across reboots so the counter is typically stable (cycles 41a..41e all reported 419), and explicitly state `mapped_pages_seen` is NOT a load-bearing signal for the cycle-42A allocation-shape interpretation.
- **Round 4: GREEN — deploy-ready.** R3 MED confirmed addressed in `commands.c`. Codex explicit "GREEN — deploy-ready. The R3 MED is addressed, and the revised `cmd_witness_scan_self` wording now matches the actual producer/consumer behavior for `count` vs `mapped_pages_seen`." No new findings; no open questions; no out-of-scope issues.

Codex marker at `.claude/state/codex-validate-last-run` refreshed.

**Build.** Incremental nxdk rebuilds after each Codex round (R1/R2/R3 producer object recompiled; R3-R4 consumer-only changes left producer .obj unchanged). Cycle-42A witness-only deployed-build SHA = `698e6fefeff916daf287cdff02f8bda72167687025aa57cd0d51568e1a1d054f` (155 648 B; same nxdk XBE page boundary as cycles 31..41e; SHA varies between rebuilds of identical source because XBE/COFF format embeds build timestamps; source bit-identical to codex-R4-confirmed state). Oracle-agent unchanged from cycle-39 v0.5 SHA `d419b452…`.

**Deployment + evidence.** Cycle-40-shape runbook, 17 logged steps under `benchmark-runs/cycle42a-multipage-20260524T103826Z/`: 00 baseline status (ping=true, agent=true v0.5 resident from cycle 41e); 01a pre-baseline `witness.scan` D-cycle-27 phys=0x03eb3000; 01b `witness.scan-self count=0 mapped_pages_seen=419`; 01c `eeprom.scratch.read byte=0xA4` (cycle-41e leftover); 02 reboot at 2026-05-24T10:39:14Z → dashboard FTP back at t+35s; 03 cycle-42A SHA captured; 04 FTP-upload witness-only with `--overwrite` (uploaded=1); 05 ensure-agent (v0.5 re-launched); 06 unsafe.enable; 07 eeprom.scratch.reset → byte=0x00 baseline; 08 eeprom.scratch.read confirmed byte=0x00; 09 witness.scan-self count=0; 10 witness.scan D-cycle-27 phys=0x03eb3000; 11 runxbe at 2026-05-24T10:40:34Z; 12 dashboard FTP back at t+36s (agent=false at t+36s); 13 ensure-agent post-chainload (v0.5 re-launched); 14 final witness.scan D-cycle-27 phys=0x03eb3000; 15 final witness.scan-self count=0 mapped_pages_seen=419; 16 final eeprom.scratch.read byte=0xA4; 17 full eeprom hex dump cross-check last byte = A4 (line ends `…0900A4`). Composite capture SKIPPED with cycle-34..41e silent-stall rationale; cycle-42A primary + secondary signals are fully agent-side.

**Hypothesis-state update.** Cache-policy variations EXHAUSTED (cycles 40+41a+41b). Address-range matched-tuple ELIMINATED (cycle 41c). Page-alignment requirement ELIMINATED (cycle 41d). Non-`-Ex` ABI fallback at single-page eliminated the simpler "`-Ex` validation logic specifically rejects this tuple" hypothesis (cycle 41e — STRONG evidence AGAINST branch (b) being SOLE failing constraint). Cycle-42A is STRONG evidence AGAINST branch (c) "`size=0x1000`-specific interaction" being the failing axis at the smallest multi-page step. The cycle-22 candidate set within the in-XBE / lib-only oracle workflow scope is now exhausted — the remaining axis is calling-context.

Caveats (per cycle-42A Honest-framing paragraph): cycle-42A is NOT a pure single-axis discriminator. Bumping `size` from `0x1000` to `0x2000` may interact with the kernel's pool-search policy through internal code-path divergence (size-bucketed free lists / separate pool arenas / distinct minimum-size policies) rather than the size-as-validation-axis being what was tested. A formal closure of branch (c) would require sweeping size across multiple multi-page steps OR cross-validation with candidate B.

**Recommended cycle 42 candidate B (Hermes's call).** Custom XBE-header callback before `_start` via `nxdk/tools/cxbe/` modifications — separates the calling-context axis from the size axis; strictest pre-CRT context distinguishes "calling-context-specific failure inside the cycle-29 `.CRT$XXC` slot" from "shared-upstream failure independent of calling context". Larger blast radius than candidate A (requires modifying nxdk's XBE-header generator), but at this point the calling-context axis is the only remaining live cycle-22 candidate that can be tested inside the in-XBE oracle workflow. If candidate B also yields G0(c), the cycle-22 hypothesis has been varied across every axis the in-XBE / lib-only workflow exposes and the next step would necessarily move outside that scope (e.g. host-xemu instrumentation of the guest allocator path; high scope; violates the cycle-23 lockstep contract).

**No scope drift.** ZERO host xemu source touched. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO `lib/lib.mk` touched. ZERO `oracle-agent/*` LOGIC touched (cycle-39 v0.5 verbs intact; cycle-42A producer change is allocator-size only; consumer `cmd_witness_scan_self` is comment-only updated). ZERO `xbed_runtime.{c,h}` touched. ZERO `witness-only/main.c` / Makefile / manifest.json touched. ZERO `nxdk/` source touched (read-only inspected only for the multi-page allocator confirmation). ZERO `tools/xemu-capture/` source touched. ZERO `composite-record.sh` / `composite-preflight.sh` source touched. Pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` + 2 `scripts/apple-silicon/xbe-tests/lib/*.inl` PRESERVED unstaged per the rolling cycle-34+ Hermes-supervision pattern (carried forward through cycles 35..42A); pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged; 24+ pre-existing untracked `.hermes_*` files at repo root preserved un-staged per the same pattern.

**M15 overall still NOT MET** pending §H.6 default-on shape (now blocked on cycle-42B+ calling-context-axis work + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

**Files.** 3 source files (`lib/xbed_self_witness.{c,h}` + `oracle-agent/commands.c`) + 2 rebuilt artifacts (`witness-only/{bin/default.xbe, witness-only.iso}`) + 6 canonical-doc updates (handoff.md cycle-42A entry on top above cycle-41d; decision-log.md cycle-42A entry above cycle-41e; orchestration-state quartet — `current-cycle.md` + `claude-status.md` + `validation-status.md` + `handoff-summary.md` — synced to cycle-42A closure). Evidence-only files at `benchmark-runs/cycle42a-multipage-20260524T103826Z/{SUMMARY.md, codex-output-r1..r4.md, codex-prompt-r1..r4.md, 00..17-*.{txt,json}}` are gitignored per project convention.

## 2026-05-24 (cycle 41d alignment-drop variation — bounded implementation+run slice CLOSED on `apple-silicon-performance`) — OUTCOME G0(c) PERSISTS under alignment-drop; cycle-22 narrowed to {the `-Ex` variant itself, `size=0x1000`-specific interaction}; cycle 41e explores non-`-Ex` fallback

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "change the witness-only `MmAllocateContiguousMemoryEx` alignment argument in `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c` from `0x1000u` to `0u` (let the real kernel pick alignment). Preserve everything else from cycle 41c unless required by Codex findings. In particular preserve the cycle-39 EEPROM scratchpad breadcrumb + sticky gate, cycle-29 self-witness structure, cycle-41c widened address range, `PAGE_READWRITE | PAGE_WRITECOMBINE` protect bits, the symmetric phys-range guards, and all existing docs/history below the new top entries. Rebuild the affected XBE; run Codex validation; deploy + run cycle-40-shape runbook; classify against existing cycle-40 G0(c) regression gate; sync canonical docs/state."

**Outcome G0(c) PERSISTS under alignment-drop.** Post-chainload signals identical to cycles 40 + 41a + 41b + 41c: EEPROM byte at 0xFF = `0xA4` + `witness.scan-self count=0 mapped_pages_seen=419` + `witness.scan = D-cycle-27` shape (phys=0x03eb3000). `MmAllocateContiguousMemoryEx(0x1000, 0x00000000, 0x7FFFFFFF, 0u, PAGE_READWRITE | PAGE_WRITECOMBINE)` STILL did NOT yield a usable allocation on this real-Xbox kernel — the kernel rejects (NULL-return or internal crash) the cycle-29 allocation tuple even with the explicit page-alignment requirement dropped. Dashboard FTP recovery at t+19s post-chainload (within sampling variance vs cycle 41c t+24s; recovery timing NOT load-bearing for G-row classification).

**Source change (1 literal + new page-alignment guard + comment + header doc + cycle-41c→cycle-41d log relabel).** `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c:214` — `alignment` argument changed from `0x1000u` to `0u` (let the kernel pick natural page-granular alignment for contiguous memory). Cycle-41c matched-tuple address range (`lowest=0x00000000u, highest=0x7FFFFFFFu`) + `Protect = PAGE_READWRITE | PAGE_WRITECOMBINE` (cycle 41b) UNCHANGED. Cycle-29 first-call branch / EEPROM-write breadcrumb / sticky `s_eeprom_scratch_attempted` gate / `MmPersistContiguousMemory` / `wbinvd` stamp / `phys | 0x80000000` cached-mirror alias for the readback path: ALL UNCHANGED. Cycle-41c symmetric phys-range guards at `xbed_self_witness.c:293-315` PRESERVED unchanged (alignment-drop does not affect the consumer scan-window contract; the matched-tuple address range is also preserved). Plus NEW Codex-round-1 P1-adopted page-alignment guard at `xbed_self_witness.c:317-346` — `if ((phys & 0xFFFu) != 0u) { host-log + MmFreeContiguousMemory + return 0 }` — that rejects any returned `phys` not 0x1000-aligned, closing the cycle-41d-specific "kernel could in principle return sub-page-aligned phys that the cycle-29 consumer's 4 KiB stride cannot see" interpretation gap. In-tree usage precedent for `Alignment=0u` exists at `nxdk/lib/pbkit/pbkit.c:2297`, `nxdk/samples/{triangle,mesh,xaudio}/main.c`, and `xbe-tests/flat-tri-depth/main.c:77`, but the nxdk header at `xboxkrnl.h:3464` is a bare prototype with no `Alignment=0` doc text — comment/header explicitly note "we cannot point to a documented 'kernel picks natural page alignment' guarantee; what the in-tree calls demonstrate is only that the API accepts `0u` as an argument, not what alignment the kernel returns." Codex round-1 P3 low "stale cycle-41c guard log strings" ADOPTED via cycle-41d relabel at `xbed_self_witness.c:296` + `:307`. Comment block at `xbed_self_witness.c:120-220` rewritten to document the cycle-41d divergence + the in-tree precedent + the new alignment guard + the narrowed elimination claim (the alignment branch is ELIMINATED, leaving only {`-Ex` variant, `size`-interaction} candidates). Header `xbed_self_witness.h:101-148` Safety-notes block updated correspondingly.

**Codex 3-round review summary** (mode=`changes`).
- **Round 1: MAJOR ISSUES.** P1 high "alignment-drop blind spot on consumer 4 KiB stride — `Alignment=0u` could let kernel return sub-page-aligned phys that the producer stamps but the consumer can't see via 0x1000-stride scan; needs evidence for page alignment OR a compensating guard." ADOPTED via 5-LOC `(phys & 0xFFFu) != 0u` guard at `xbed_self_witness.c:317-346`. P3 low "guard log strings still say `cycle-41c`" ADOPTED via cycle-41d relabel.
- **Round 2: MINOR ISSUES.** R1.P1+P3 confirmed addressed via in-diff guard + log relabel. One low comment-precision finding on "documented" wording (already satisfied by in-source soft framing at `xbed_self_witness.c:175-189` + `xbed_self_witness.h:107-119`).
- **Round 3: GREEN.** Codex explicit: "Current source already satisfies the R2 low finding. The block at `xbed_self_witness.c:175` explicitly limits the claim to 'in-tree usage pattern' and then disclaims any documented `Alignment=0` guarantee at lines 179-184; the matching header block uses the same soft framing. No further wording softening is needed. This slice is deploy-ready for the real-Xbox run."

Codex marker at `.claude/state/codex-validate-last-run` refreshed.

**Build.** Clean nxdk rebuild via `make NXDK_DIR=...` clean + rebuild. Cycle-41d witness-only deployed-build SHA = `c49ca0ad2a3f968ecb0ea02bf16a7117da614b912ff98feb58ed925ee387f589` (155 648 B; SHA varies between rebuilds of identical source because XBE/COFF format embeds build timestamps in three locations of `default.xbe` + one location of `main.exe`; source bit-identical to codex-validated GREEN state). Oracle-agent binary unchanged from cycle-39 v0.5 SHA `d419b452…`.

**Deployment + evidence.** Cycle-40 runbook, 18 logged steps under `benchmark-runs/cycle41d-alignment-20260524T054838Z/`: 00 baseline scans MET (ping=true, agent=true v0.5 resident from cycle 41c); 01 pre-baseline `eeprom.scratch.read` byte=0xA4 (cycle-41c leftover) + `witness.scan-self count=0` + `witness.scan D-cycle-27`; 02 reboot 1 at 06:00:02Z → 03 dashboard FTP back at t+20s; 04 cycle-41d witness-only SHA captured; 05 FTP-upload witness-only with `--overwrite` (uploaded=1); 06 ensure-agent (v0.5 re-launched); 07 unsafe.enable; 08 eeprom.scratch.reset → byte=0x00 confirmed; 09 eeprom.scratch.read confirmed byte=0x00 baseline; 10 witness.scan-self count=0; 11 witness.scan D-cycle-27 phys=0x03eb3000; 12 `runxbe path=E:\Apps\witness-only\default.xbe` at 06:03:09Z; 13 dashboard FTP back at t+19s; 14 ensure-agent post-chainload (v0.5 re-launched); 15 final witness.scan D-cycle-27 phys=0x03eb3000; 16 final witness.scan-self count=0 mapped_pages_seen=419; 17 final eeprom.scratch.read byte=0xA4; 18 full eeprom hex dump cross-check last byte = A4. Composite capture SKIPPED with cycle-34+36+41a+41b+41c silent-stall rationale; cycle-41d primary + secondary signals are fully agent-side.

**Hypothesis-state update.** Cache-policy variations EXHAUSTED (cycles 40+41a+41b: bare RW / NC / WC). Address-range matched-tuple sub-hypothesis ELIMINATED (cycle 41c). Page-alignment requirement ELIMINATED (cycle 41d: `Alignment=0u` also fails identically). Cycle-22 leading hypothesis is FURTHER NARROWED — the failing constraint is one of:
1. **The `-Ex` variant itself** — fall back to non-`-Ex` `MmAllocateContiguousMemory(0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE)` (cycle 41e).
2. **A `size=0x1000`-specific interaction** — only `size` still differs from nxdk's framebuffer allocator (their `screenSize` is multi-page; ours is single-page); out-of-cycle-41-scope (would require redesigning the cycle-29 self-witness as multi-page; changes the WTNS layout contract).

**Recommended cycle 41e (Hermes's call).** Non-`-Ex` fallback variation: in `xbed_self_witness.c`, replace `MmAllocateContiguousMemoryEx(0x1000u, 0x00000000u, 0x7FFFFFFFu, 0u, PAGE_READWRITE | PAGE_WRITECOMBINE)` with the standard 2-arg `MmAllocateContiguousMemory(0x1000u, PAGE_READWRITE | PAGE_WRITECOMBINE)` (non-`-Ex` variant). Bounded ~3-line source change. All other cycle-41d preserved invariants stay UNCHANGED — the cycle-41c address-range guards and cycle-41d alignment guard remain valid because the consumer scan window is still `[0x80010000, 0x84000000]` regardless of which variant produces the phys. Cycle-40 G0(c) is the regression gate. If cycle 41e also fails G0(c), only fundamentally different approaches remain (custom XBE-header callback before `_start` — high scope; requires `nxdk/tools/cxbe/` changes; OR redesign of cycle-29 self-witness as multi-page).

**No scope drift.** ZERO host xemu source touched. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO `lib/lib.mk` touched. ZERO `oracle-agent/*` touched (cycle-39 v0.5 verbs intact; cycle-41d alignment-drop is producer-side only — consumer scan window unchanged). ZERO `xbed_runtime.{c,h}` touched. ZERO `witness-only/main.c` / Makefile / manifest.json touched. ZERO `nxdk/` source touched (`../nxdk/lib/pbkit/pbkit.c` + `../nxdk/samples/{triangle,mesh,xaudio}/main.c` + `../nxdk/lib/hal/video.c` + `../nxdk/lib/xboxkrnl/xboxkrnl.h` were read-only inspected by Codex for the `Alignment=0u` precedent search). ZERO `tools/xemu-capture/` source touched. ZERO `composite-record.sh` / `composite-preflight.sh` source touched. Pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` PRESERVED unstaged per the rolling cycle-34+ Hermes-supervision pattern (carried forward through cycles 35..41d); pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged; 22+ pre-existing untracked `.hermes_*` files at repo root preserved un-staged per the same pattern (`.hermes_cycle41d_alignment_*` from this cycle's Hermes supervisor in that set).

**M15 overall still NOT MET** pending §H.6 default-on shape (now blocked on cycle-41e+ non-`-Ex` fallback work + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

**Files.** 2 source files (`xbed_self_witness.{c,h}`) + 2 rebuilt artifacts (`witness-only/{bin/default.xbe, witness-only.iso}`) + 6 canonical-doc updates (handoff.md cycle-41d entry on top above cycle-41c; decision-log.md cycle-41d entry above cycle-41c; orchestration-state quartet — `current-cycle.md` + `claude-status.md` + `validation-status.md` + `handoff-summary.md` — synced to cycle-41d closure). Evidence-only files at `benchmark-runs/cycle41d-alignment-20260524T054838Z/{SUMMARY.md, codex-output.md, codex-output-r2.md, codex-output-r3.md, codex-prompt.md, codex-prompt-r2.md, codex-prompt-r3.md, 00..18-*.{txt,json}}` are gitignored per project convention.

## 2026-05-24 (cycle 41c combined address-range variation — bounded implementation+run slice CLOSED on `apple-silicon-performance`) — OUTCOME G0(c) PERSISTS under matched-tuple address range; cycle-22 narrowed to {alignment, `-Ex` variant, or `size`-specific interaction}; cycle 41d explores alignment-drop

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "change the witness-only allocation tuple to the combined address-range candidate that matches nxdk's framebuffer allocator range as closely as possible (`lowest=0x00000000, highest=0x7FFFFFFF, alignment=0x1000, Protect = PAGE_READWRITE | PAGE_WRITECOMBINE`), keep the cycle-39 EEPROM breadcrumb / sticky gate / cached-mirror alias / all other witness behavior intact, rebuild the affected XBE, run Codex validation, deploy to the physical Xbox, execute the documented witness-only chainload runbook, classify against the existing cycle-40 G0(c) regression gate, and sync canonical docs/state."

**Outcome G0(c) PERSISTS under matched-tuple address range.** Post-chainload signals identical to cycles 40 + 41a + 41b: EEPROM byte at 0xFF = `0xA4` + `witness.scan-self count=0` + `witness.scan = D-cycle-27` shape. `MmAllocateContiguousMemoryEx(0x1000, 0x00000000, 0x7FFFFFFF, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE)` — the EXACT nxdk framebuffer allocator tuple modulo `size` — STILL did NOT yield a usable allocation. Dashboard FTP recovery at t+24s post-chainload — IDENTICAL to cycle 41b (+18s vs cycle 40 baseline; recovery timing NOT load-bearing for G-row classification).

**Source change (2 literals + symmetric guards + comment + header doc).** `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c:155-156` — `lowest` argument changed from `0x00010000u` to `0x00000000u`, `highest` argument changed from `0x03FFFFFFu` to `0x7FFFFFFFu`. Codex round-1 P1 + round-2 P1 ADOPTED via NEW symmetric phys-range guards at `xbed_self_witness.c:254-273` that reject any returned `phys` outside the cycle-29 consumer's scan window `[0x00010000, 0x04000000)` — lower guard `phys < 0x00010000u` + upper guard `phys >= 0x04000000u`, each with a distinct host-log line, `MmFreeContiguousMemory(p)`, and `return 0`. On retail Original Xbox (64 MiB physical RAM) the upper guard is a no-op (kernel cannot return phys it does not have); guards exist for defense in depth and to formally close the Codex-flagged interpretation gap. Comment block at `xbed_self_witness.c:120-180` rewritten to document the cycle-41c divergence + the matched-tuple precedent + the symmetric guards + the narrowed elimination claim ("kernel demands specific non-cycle-29-tuple address range" only, NOT address-range as a whole). Header `xbed_self_witness.h:101-130` Safety-notes block updated correspondingly. Cycle-29 first-call branch / EEPROM-write breadcrumb / sticky `s_eeprom_scratch_attempted` gate / `MmPersistContiguousMemory` / `wbinvd` stamp / `phys | 0x80000000` cached-mirror alias for the readback path: ALL UNCHANGED.

**Codex 3-round review summary** (mode=`changes`).
- **Round 1: MAJOR ISSUES.** P1 high "cycle-29 consumer at `oracle-agent/commands.c:726` only scans `0x80010000..0x84000000` (64 MiB kseg0 window); cycle-41c's `highest=0x7FFFFFFF` could let kernel return a phys above 64 MiB that the producer stamps but the consumer cannot see, turning `count=0` into a false-negative G0(c)-shape." ADOPTED via 5-LOC upper-bound guard. P2 medium "elimination claim overstates" ADOPTED via retail-64MiB-scope qualification. P2 medium "doc-rule-#4 mid-slice state" DEFLECTED (transient; closure commit syncs). P3s confirming.
- **Round 2: MAJOR ISSUES.** P1 high "symmetric low-phys blind spot — cycle-41c's `lowest=0x00000000` could let kernel return phys < 0x00010000 (cycle-29 reader's lower bound) equally invisible to the consumer." ADOPTED via 5-LOC symmetric lower-bound guard. P2 medium "elimination claim still too strong" ADOPTED via further-narrowed wording: cycle 41c eliminates only the "kernel demands specific non-cycle-29-tuple address range" sub-hypothesis (because the matched-tuple is known-good against the same `-Ex` entry point for nxdk's framebuffer allocator on this kernel). P3 no new hazards from guards.
- **Round 3: MINOR ISSUES.** P2 medium "count=0 comment overstates as unambiguously 'allocation failed'" ADOPTED via wording softening — count=0 is STRONGLY suggestive of allocation-failure but not unambiguous because either guard firing also yields count=0 (page allocated, then freed by guard, never stamped); the guard-block comment provides full interpretation. Codex explicit: "Nothing else looks load-bearing for the cycle-41c real-Xbox deploy."

Codex marker at `.claude/state/codex-validate-last-run` refreshed (fingerprint `cc437e2b`).

**Build.** Clean nxdk rebuild via `make NXDK_DIR=...` clean + rebuild. New witness-only artifact SHA = `cc437e2b7da250fe18be120da168e973a72a3f81559a60d99d5e2e7bc6034175` (size 155 648 B — same as cycle-39/40/41a/41b builds; delta is 2-literal source change + 2× 5-LOC symmetric guards + comment block + header doc updates). Oracle-agent binary unchanged from cycle-39 v0.5 SHA `d419b452…`.

**Deployment + evidence.** Cycle-40 runbook, 18 logged steps under `benchmark-runs/cycle41c-addressrange-20260524T051200Z/`: 00 baseline scans MET (ping=true, agent=true v0.5 resident from cycle 41b); 01 pre-baseline `eeprom.scratch.read` byte=0xA4 (cycle-41b leftover) + `witness.scan-self count=0` + `witness.scan D-cycle-27`; 02 reboot 1 at 05:28:17Z → 03 dashboard FTP back at t+24s; 04 cycle-41c witness-only SHA captured; 05 FTP-upload witness-only with `--overwrite` (uploaded=1); 06 ensure-agent (v0.5 re-launched); 07 unsafe.enable; 08 eeprom.scratch.reset; 09 eeprom.scratch.read confirmed byte=0x00; 10 witness.scan-self count=0; 11 witness.scan D-cycle-27; 12 `runxbe path=E:\Apps\witness-only\default.xbe` at 05:29:28Z; 13 dashboard FTP back at t+24s; 14 ensure-agent post-chainload; 15 final witness.scan D-cycle-27 (phys=0x03eb3000); 16 final witness.scan-self count=0; 17 final eeprom.scratch.read byte=0xA4; 18 full eeprom hex dump cross-check last byte = A4. Composite capture SKIPPED with cycle-34+36+41a+41b silent-stall rationale; cycle-41c primary + secondary signals are fully agent-side.

**Hypothesis-state update.** Cache-policy variations EXHAUSTED (cycle 40 bare RW + cycle 41a NC + cycle 41b WC). Cycle 41c eliminates the "kernel demands specific non-cycle-29-tuple address range" sub-hypothesis: the EXACT nxdk framebuffer allocator tuple modulo `size` was also rejected. Cycle-22 leading hypothesis is FURTHER NARROWED — the failing constraint is one of:
1. **Alignment `0x1000`** — drop to 0 (cycle 41d).
2. **The `-Ex` variant itself** — fall back to non-`-Ex` `MmAllocateContiguousMemory(0x1000)` (cycle 41e).
3. **A `size=0x1000`-specific interaction** — only `size` still differs from nxdk's framebuffer allocator (their `screenSize` is multi-page; ours is single-page); out-of-cycle-41-scope (size cannot meaningfully vary for a single-page witness; would require redesigning the cycle-29 self-witness as multi-page).

**Recommended cycle 41d (Hermes's call).** Alignment-drop variation: change `xbed_self_witness.c` `alignment` argument from `0x1000u` to `0u` (let the kernel pick alignment). Bounded single-literal change. If cycle 41d still fails G0(c), only non-`-Ex` fallback to plain `MmAllocateContiguousMemory(0x1000)` (cycle 41e) remains in cycle-41 scope. Cycle-40 G0(c) is the cycle-41 regression gate: EEPROM byte at 0xFF must stay 0xA4 (sticky `s_eeprom_scratch_attempted` Codex-round-2 P1 fix ensures preservation); a SUCCESSFUL variation should advance `witness.scan-self count >= 1`.

**No scope drift.** ZERO host xemu source touched. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO `lib/lib.mk` touched. ZERO `oracle-agent/*` touched (cycle-39 v0.5 verbs intact). ZERO `xbed_runtime.{c,h}` touched. ZERO `witness-only/main.c` / Makefile / manifest.json touched. ZERO `nxdk/` source touched (`../nxdk/lib/hal/video.c:363-367` was read-only inspected for the matched-tuple precedent). ZERO `tools/xemu-capture/` source touched. ZERO `composite-record.sh` / `composite-preflight.sh` source touched. Pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` PRESERVED unstaged per the rolling cycle-34+ Hermes-supervision pattern (carried forward through cycles 35..41c); pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged; 22+ pre-existing untracked `.hermes_*` files at repo root preserved un-staged per the same pattern (`.hermes_cycle41c_address_range_20260524T045532Z_prompt.txt` from this cycle's Hermes supervisor in that set).

**M15 overall still NOT MET** pending §H.6 default-on shape (now blocked on cycle-41d+ alignment / `-Ex`-fallback work + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

**Files.** 2 source files (`xbed_self_witness.{c,h}`) + 2 rebuilt artifacts (`witness-only/{bin/default.xbe, witness-only.iso}`) + 6 canonical-doc updates (handoff.md cycle-41c entry on top above cycle-41b; decision-log.md cycle-41c entry above cycle-41b; orchestration-state quartet — `current-cycle.md` + `claude-status.md` + `validation-status.md` + `handoff-summary.md` — synced to cycle-41c closure). Evidence-only files at `benchmark-runs/cycle41c-addressrange-20260524T051200Z/{SUMMARY.md, codex-output.md, codex-output-r2.md, codex-output-r3.md, codex-prompt.md, codex-prompt-r2.md, codex-prompt-r3.md, 00..18-*.{txt,json}}` are gitignored per project convention.

## 2026-05-24 (cycle 41b `PAGE_WRITECOMBINE` allocation-flag variation — bounded implementation+run slice CLOSED on `apple-silicon-performance`) — OUTCOME G0(c) PERSISTS under `PAGE_WRITECOMBINE`; CACHE-POLICY VARIATIONS EXHAUSTED; cycle 41c broadens to address-range

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "try the next-highest-value cycle-41 allocation variation by changing the `MmAllocateContiguousMemoryEx` protect flags in `lib/xbed_self_witness.c` from the cycle-41a `PAGE_READWRITE | PAGE_NOCACHE` experiment to `PAGE_READWRITE | PAGE_WRITECOMBINE`, then rebuild the affected XBE(s), run the required validation, deploy to the physical Xbox, execute the documented witness-only chainload sequence, classify the result against the existing cycle-40 G0(c) regression gate, and sync durable docs/state."

**Outcome G0(c) PERSISTS under `PAGE_WRITECOMBINE`.** Post-chainload signals identical to cycles 40 + 41a: EEPROM byte at 0xFF = `0xA4` + `witness.scan-self count=0` + `witness.scan = D-cycle-27` shape. `MmAllocateContiguousMemoryEx(0x1000, 0x10000, 0x3ffffff, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE)` STILL did NOT yield a usable allocation. Dashboard FTP recovery at t+24s post-chainload (cycle-41a was t+8s; +16s shift NOT dismissable as sampling variance; still faster than cycle-36 t+38s graceful; recovery timing NOT load-bearing for G-row classification — full interpretation in cycle-41b SUMMARY).

**Source change (1 line + paired comment + header doc).** `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c:158` — `Protect` argument flipped from `PAGE_READWRITE | PAGE_NOCACHE` to `PAGE_READWRITE | PAGE_WRITECOMBINE`. Comment block at `xbed_self_witness.c:125-152` updated to document the cycle-41b divergence + the new precedent (nxdk's framebuffer allocator at `nxdk/lib/hal/video.c:363-367` uses `PAGE_READWRITE | PAGE_WRITECOMBINE` against `MmAllocateContiguousMemoryEx` — symmetric known-good site to cycle-41a's OHCI precedent). Header `xbed_self_witness.h:101-118` Safety-notes block updated correspondingly. Cycle-29 first-call branch / EEPROM-write breadcrumb / sticky `s_eeprom_scratch_attempted` gate / `MmPersistContiguousMemory` / `wbinvd` stamp / `phys | 0x80000000` cached-mirror alias for the readback path: ALL UNCHANGED.

**Codex round-1 review summary.** v1 prompt (`codex-prompt.md` / `codex-output.md`) got stuck in a web-search loop the read-only sandbox cannot service (codex called `web_search` to verify wbinvd-vs-WC Intel SDM semantics; all calls failed silently; output truncated mid-investigation). v3 prompt (`codex-prompt-v2.md` / `codex-output-v3.md`) replaced it with explicit "DO NOT use web search; nxdk lives at `../nxdk/...`" guidance and produced a clean **GREEN** verdict in 41k tokens with 3 P3 findings, all confirmations: (P3.1) no hard-rule conflicts; (P3.2) `PAGE_NOCACHE=0x200` + `PAGE_WRITECOMBINE=0x400` are distinct non-overlapping cache-policy bits in `nxdk/lib/xboxkrnl/xboxkrnl.h:3294`; precedent claim verified at `nxdk/lib/hal/video.c:363`; (P3.3) no new coherency hazard introduced by WC vs NC since the producer-side writes go through the kseg0 cached-mirror alias `phys | 0x80000000` (canonicalized at `xbed_self_witness.c:165`) not through the kernel-returned `p`; consumer scans WTNS pages via kseg0 virtual addresses in `oracle-agent/commands.c:726` — the diff leaves that cached-alias producer/consumer path unchanged, so this is the same cycle-41a alias separation, not a new WC-specific hazard. Codex marker at `.claude/state/codex-validate-last-run` refreshed.

**Build.** Clean nxdk rebuild via `eval $(... activate -s) && make NXDK_DIR=...` clean + rebuild. New witness-only artifact SHA = `fbd828a24edcac95278fb62d486a951a0c5ba9c48454a27c32032ef3959f8404` (size 155 648 B — same as cycle-39 / cycle-40 / cycle-41a builds; delta is the 1-line code change + paired comment updates only). Oracle-agent binary unchanged from cycle-39 v0.5 SHA `d419b452…`.

**Deployment + evidence.** Cycle-40 runbook (18 logged steps under `benchmark-runs/cycle41b-pagewritecombine-20260524T043727Z/`): 00 baseline scans MET (ping=true, agent=true v0.5 resident from cycle 41a); 01 pre-baseline `eeprom.scratch.read` byte=0xA4 (cycle-41a leftover); 02 reboot 1 → 03 dashboard FTP back at t+18s; 04 new cycle-41b witness-only SHA captured; 05 FTP-upload witness-only with `--overwrite`; 06 ensure-agent (v0.5 re-launched); 07 unsafe.enable; 08 eeprom.scratch.reset; 09 eeprom.scratch.read confirmed byte=0x00; 10 witness.scan-self count=0; 11 witness.scan D-cycle-27; 12 runxbe at 044438Z; 13 dashboard FTP back at t+24s (NOTE: +16s shift vs cycle 41a); 14 ensure-agent post-chainload; 15 final witness.scan D-cycle-27; 16 final witness.scan-self count=0; 17 final eeprom.scratch.read byte=0xA4; 18 full eeprom hex dump cross-check last byte = A4. Composite capture SKIPPED with cycle-34+36+41a silent-stall rationale; cycle-41b primary + secondary signals are fully agent-side.

**Hypothesis-state update.** Cache-policy variations are EXHAUSTED (bare RW + NC + WC all fail identically). The cycle-22 leading hypothesis is FURTHER NARROWED: the failing constraint is NOT a cache-policy bit. It is one of {(i) address-range floor `0x00010000`, (ii) address-range ceiling `0x03FFFFFF`, (iii) alignment requirement `0x1000`, (iv) the `-Ex` variant itself}. Cycle 41c is the next bounded slice.

**Recommended cycle 41c (Hermes's call).** Combine the lowest-scope remaining candidates into ONE variation that matches the nxdk framebuffer-allocator address range exactly: `lowest=0x00000000, highest=0x7FFFFFFF, alignment=0x1000, Protect = PAGE_READWRITE | PAGE_WRITECOMBINE`. This is the closest possible match to a known-good nxdk-side call (`nxdk/lib/hal/video.c:363-367` parameters exactly modulo `size`) and represents the lowest-information-cost test of "is the address range the failing constraint." If cycle 41c still fails G0(c), only alignment-drop (cycle 41d) and non-`-Ex` fallback to plain `MmAllocateContiguousMemory(0x1000)` (cycle 41e) remain in scope. Cycle-40 G0(c) is the cycle-41 regression gate: EEPROM byte at 0xFF must stay 0xA4 (sticky `s_eeprom_scratch_attempted` Codex-round-2 P1 fix ensures preservation across later fires); a SUCCESSFUL variation should advance `witness.scan-self count >= 1` with `reserved1` matching fires landed.

**No scope drift.** ZERO host xemu source touched. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO `lib/lib.mk` touched. ZERO `oracle-agent/*` touched (cycle-39 v0.5 verbs intact). ZERO `xbed_runtime.{c,h}` touched. ZERO `witness-only/main.c` / Makefile / manifest.json touched. ZERO `nxdk/` source touched (`../nxdk/lib/hal/video.c` and `../nxdk/lib/xboxkrnl/xboxkrnl.h` were read-only inspected for the WRITECOMBINE precedent + flag-value verification). ZERO `tools/xemu-capture/` source touched. ZERO `composite-record.sh` / `composite-preflight.sh` source touched. Pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` PRESERVED unstaged per the rolling cycle-34+ Hermes-supervision pattern (carried forward through cycles 35..41b); pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged; 21+ pre-existing untracked `.hermes_*.{txt,sh,log}` files at repo root preserved un-staged per the same pattern (`.hermes_cycle41b_writecombine_20260524T043351Z_prompt.txt` + `.log` + `.hermes_launch_cycle41b_*.sh` from this cycle's Hermes supervisor in that set).

**M15 overall still NOT MET** pending §H.6 default-on shape (now blocked on cycle-41c+ address-range variation work + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

**Files.** 2 source files (`xbed_self_witness.{c,h}`) + 2 rebuilt artifacts (`witness-only/{bin/default.xbe, witness-only.iso}`) + 6 canonical-doc updates (handoff.md cycle-41b entry on top above cycle-41a; decision-log.md cycle-41b entry above cycle-41a; orchestration-state quartet — `current-cycle.md` + `claude-status.md` + `validation-status.md` + `handoff-summary.md` — synced to cycle-41b closure). Evidence-only files at `benchmark-runs/cycle41b-pagewritecombine-20260524T043727Z/{SUMMARY.md, codex-output.md, codex-prompt.md, codex-prompt-v2.md, codex-output-v2.md, codex-output-v3.md, 00..18-*.{txt,json}}` are gitignored per project convention.

## 2026-05-24 (cycle 41a `PAGE_NOCACHE` allocation-flag variation — bounded implementation+run slice CLOSED on `apple-silicon-performance`) — OUTCOME G0(c) PERSISTS under `PAGE_NOCACHE`; cycle 41b explores `PAGE_WRITECOMBINE`

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "try the LOWEST-SCOPE cycle-41 allocation variation by changing the `MmAllocateContiguousMemoryEx` protect flags in `lib/xbed_self_witness.c` from plain `PAGE_READWRITE` to `PAGE_READWRITE | PAGE_NOCACHE`, then rebuild the affected XBE(s), run the required validation, deploy to the physical Xbox, execute the documented witness-only chainload sequence, classify the result against the existing cycle-40 G0(c) regression gate, and sync durable docs/state."

**Outcome G0(c) PERSISTS.** Post-chainload signals identical to cycle 40: EEPROM byte at 0xFF = `0xA4` + `witness.scan-self count=0` + `witness.scan = D-cycle-27` shape. `MmAllocateContiguousMemoryEx(0x1000, 0x10000, 0x3ffffff, 0x1000, PAGE_READWRITE | PAGE_NOCACHE)` still did NOT yield a usable allocation on this real-Xbox kernel. Dashboard FTP recovery at t+8s post-chainload (vs cycle-40 t+6s; +2s within sampling variance; still consistent with watchdog hardware reset rather than `HalReturnToFirmware` graceful exit).

**Source change (1 line + Codex-adopted comment updates).** `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c:138` — `Protect` argument flipped from `PAGE_READWRITE` to `PAGE_READWRITE | PAGE_NOCACHE`. Comment block at `xbed_self_witness.c:127-145` tightened per Codex P1 medium finding to explicitly frame cycle-41a as ALLOCATOR-ACCEPTANCE triage (the `phys | 0x80000000` cached-mirror alias for the stamp/readback is unchanged; the `phys | 0xB0000000` end-to-end uncached-alias experiment is DEFLECTED as out-of-slice scope). Header `xbed_self_witness.h:101-118` Safety-notes block rewritten per Codex P2 low finding to document the cycle-41a NOCACHE divergence from the agent's plain-RW pattern + the nxdk OHCI `MmAllocateContiguousMemoryEx(..., PAGE_READWRITE | PAGE_NOCACHE)` precedent at `nxdk/lib/usb/libusbohci_xbox/usbh_xbox.c:35-41`. Cycle-29 first-call branch / EEPROM-write breadcrumb / sticky `s_eeprom_scratch_attempted` gate / `MmPersistContiguousMemory` / `wbinvd` stamp / cached-alias readback path: ALL UNCHANGED.

**Codex round-1 review summary.** mode=`changes`; verdict=MINOR ISSUES. P1 medium ("comment overstates kseg0+NOCACHE alias semantics") ADOPTED via in-code comment tightening only — alias-change suggestion DEFLECTED as out-of-slice. P2 low ("header doc still says RW protection matches agent") ADOPTED via header Safety-notes rewrite. Codex open question about `PAGE_WRITECOMBINE` vs `PAGE_NOCACHE` resolved by Codex itself ("WRITECOMBINE is the more natural later experiment for NV2A-facing buffers, not an obvious requirement for this specific witness page") — filed as cycle-41b candidate in cycle-41a SUMMARY ranked-low-scope-first list. Out-of-scope finding ("agent's own allocator at `oracle-agent/controller.c:221-248` still uses plain `PAGE_READWRITE`") NOTED for future follow-up only — the agent's buffer empirically works on this kernel-state across many chainloads, so it is not in the failing kernel-state region. Codex marker at `.claude/state/codex-validate-last-run` refreshed.

**Build.** Clean nxdk rebuild via `eval $(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s) && make NXDK_DIR=/Users/jbbrack03/XEMU_MacOS/nxdk`. New witness-only artifact SHA = `7dae8cf9cc08c60f699628eb284a0b1e93b7d8ac580af6851b55b3ed6bb08f78` (size 155 648 B — same size as cycle-39/cycle-40 builds; the delta is the 1-line code change + comment updates). Oracle-agent binary unchanged from cycle-39 v0.5 (SHA `d419b452…`).

**Deployment runbook executed.** Identical shape to cycle 40 (reboot 1 → wait dashboard FTP → FTP-upload witness-only XBE with `--overwrite` → `ensure-agent` → `unsafe.enable` + `eeprom.scratch.reset` → confirm `eeprom.scratch.read=0x00` baseline + `witness.scan-self count=0` + `witness.scan = D-cycle-27` → `runxbe path=E:\Apps\witness-only\default.xbe` → poll for dashboard FTP recovery → `ensure-agent` again → final scans + EEPROM dump cross-check). Composite capture SKIPPED with cycle-34 / cycle-36 ffmpeg silent-stall rationale; cycle-41a primary + secondary signals are fully agent-side and reliable.

**What cycle-41a discriminates.** ELIMINATES the cycle-39-closeout candidate #1 sub-option "cache-policy bare-RW alone fails because the kernel rejects bare-RW for `MmAllocateContiguousMemoryEx` and demands an explicit cache-policy bit" — the kernel did NOT accept the call with `PAGE_NOCACHE` either. DOES NOT eliminate: `PAGE_WRITECOMBINE` (cycle 41b); broader address range (cycle 41c); lower alignment (cycle 41d); fallback to non-`-Ex` `MmAllocateContiguousMemory` (cycle 41e). Cycle-22 leading hypothesis FURTHER STRENGTHENED but not yet narrowed beyond cycle-40's narrowing.

**Reproducibility wins.** `phys=0x03eb3000` deterministic kernel-pool reuse REPRODUCED across baseline + post-deployment + final readbacks (21+ consecutive observations across cycles 26..41a). `mapped_pages_seen=419` REPRODUCED at every readback. EEPROM non-volatility across the pre-chainload reboot 1 confirmed (post-reset byte=0x00 survived to runxbe; only the chainloaded XBE's `xbed_self_witness_fire` wrote 0xA4).

**Recommended cycle 41b (Hermes's call).** Lowest-scope: `PAGE_WRITECOMBINE` cache-policy candidate (symmetric one-line change in `xbed_self_witness.c:138`). If WRITECOMBINE also fails identically (`byte=0xA4 + count=0 + fast watchdog reset`), cache-policy variations are exhausted → cycle 41c broadens to address-range / alignment / non-`-Ex` fallback per the ranked-low-scope-first list in cycle-41a SUMMARY.md. Cycle-40 G0(c) signal remains the regression gate (EEPROM byte must stay 0xA4 + watchdog-reset-shape recovery; a SUCCESSFUL variation should advance `witness.scan-self count >= 1`).

**Files.** Tracked source delta: 2 files (`scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c` + `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.h`). Canonical doc updates: handoff.md cycle-41a entry on top above cycle-40; decision-log.md cycle-41a entry above cycle-40; orchestration-state quartet — current-cycle.md + claude-status.md + validation-status.md + handoff-summary.md — synced to cycle-41a closure. Evidence-only files at `benchmark-runs/cycle41a-pagenocache-20260524T040940Z/{SUMMARY.md, codex-output.md, codex-prompt.md, codex-login-status.txt, 00..18-*.{txt,json}}` are gitignored per project convention.

---

## 2026-05-24 (cycle 40 real-Xbox EEPROM scratchpad discriminator run — bounded run-only slice CLOSED on `apple-silicon-performance`) — OUTCOME G0(c); cycle 22 narrowed to `MmAllocateContiguousMemoryEx` returns NULL silently; cycle 41 explores allocation-flag variations

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "execute the cycle-39 recommended real-Xbox deployment and recover the EEPROM scratchpad discriminator so we can classify the remaining G0 sub-cases. Use the existing repo tooling and documented runbook to deploy the cycle-39 oracle-agent + witness-only artifacts to the physical Xbox, establish the required clean baseline (`unsafe.enable` + `eeprom.scratch.reset`), run the chainload sequence, recover the post-run `eeprom.scratch.read` + `witness.scan-self` + `witness.scan` evidence, and update the durable docs/state with the actual outcome." ZERO source/script edits; ZERO host xemu source touched.

**Outcome: G0(c)** per the cycle-40 G-row discriminator table in `witness-only/README.md` cycle-39 addendum. Reads as `(EEPROM 0xFF = 0xA4, witness.scan-self count = 0, witness.scan = D-cycle-27)`:

- `witness.scan` = `count=1 phys=0x03eb3000 reserved0=0 reserved1=0` (D-cycle-27 shape — kernel-pool deterministic reuse continues; reproduced across cycles 26..36..40)
- `witness.scan-self` = `count=0 mapped_pages_seen=419` (no WTNS page allocated; the cycle-29 in-`main()` fires never ran)
- `eeprom.scratch.read` = `off=0xFF byte=0xA4 tag=0xA stage_nib=0x4 interp="cycle-39 self-witness pre-MmAlloc breadcrumb (sub-case (c) if WTNS count=0; G1..G4 if WTNS count>=1)"` — cross-confirmed by full `eeprom` dump (last byte of 256-byte image = `A4`)
- Dashboard FTP recovery: **t+6s** post-runxbe (4 seconds of ping=false at t+2/t+4, then ping+ftp+auth=OK at t+6s) — anomalously fast vs cycle-36's t+38s clean recovery, consistent with kernel-detected allocation crash triggering a watchdog hardware reset rather than `HalReturnToFirmware` graceful exit

G0(c) means: the cycle-29 `.CRT$XXC` stage-4 fire DID execute up to AND including the `HalWriteSMBusValue(0xA8, 0xFF, FALSE, 0xA4)` call (positioned in `lib/xbed_self_witness.c` AS THE LAST INSTRUCTION before `MmAllocateContiguousMemoryEx`). The subsequent `MmAllocateContiguousMemoryEx(0x1000u, 0x00010000u, 0x03ffffffu, 0x1000u, PAGE_READWRITE)` call returned NULL silently OR crashed inside the kernel call, leaving `s_witness_page` NULL and the second-call no-op path never reached. **Sub-cases G0(a) "crash inside nxdk pre-`.CRT$X*` startup (`_start` / `__security_init_cookie` / TLS-size / `_PDCLIB_xbox_libc_init`)" and G0(b) "crash inside `_witness_only_pre_main_crt_xx`'s body BEFORE the pre-MmAlloc instruction" are ELIMINATED** by the EEPROM byte landing at `0xA4` (both (a) and (b) would have left byte = `0x00` — the cycle-39 reset baseline).

**Hypothesis state update vs cycle 39 closure.** The cycle-22 leading hypothesis "pre-main crash, anywhere" is NARROWED from a 3-sub-case G0 row to specifically sub-case (c): `MmAllocateContiguousMemoryEx` returns NULL silently on the real-Xbox kernel for the cycle-29 allocation tuple (size=0x1000, lowest=0x00010000, highest=0x03ffffff, alignment=0x1000, protect=PAGE_READWRITE). γ.0 narrowing is now finer-grained but still strictly pre-`main()`-completion. γ.1 (`XVideoSetMode` faulting) remains INVALIDATED (cycle 36 G0 outcome did not change). No new hypothesis introduced — cycle 40 is a clean confirmation of the cycle-39 discriminator's intended signal path.

**Sequence executed** (15 logged steps under `benchmark-runs/cycle40-real-xbox-eeprom-discriminator-20260524T021727Z/`, gitignored per project convention; full SUMMARY.md in that directory):

1. Reachability probe (ping=true, agent=true; v0.4 resident from cycle 39 closure) + baseline both-scans MET.
2. Reboot to dashboard (1st) — FTP back t+~14s, auth OK.
3. FTP-upload cycle-39 oracle-agent v0.5 (SHA-256 `d419b452f127e5a065a51b2d5bba49b6f2ec1f1826cdec57845793c34440cb53`, 417 792 B) to `/E/Apps/oracle-agent/default.xbe`.
4. `ensure-agent` → v0.5 listening; verbs `eeprom.scratch.read` + `eeprom.scratch.reset` present in help; banner still reads "v0.4" because cycle 39 only updated the source banner-comment, NOT the runtime `cmd_info` string — cosmetic-only deviation; verbs work.
5. `unsafe.enable` + `eeprom.scratch.reset` + verify → EEPROM 0xFF = `0x00` (baseline armed).
6. Reboot to dashboard (2nd, deviation from runbook step order to release dashboard FTP for witness-only upload) — FTP back t+8s, auth OK; EEPROM non-volatile so baseline preserved across reboot.
7. FTP-upload cycle-39 witness-only XBE (SHA-256 `7528bb5bf4c9cc8f00c934c38e015166de7a6f89236ddc8e2ad2195a47ce4e0d`, 155 648 B) `--overwrite` to `/E/Apps/witness-only/default.xbe`.
8. `ensure-agent` (relaunch v0.5) + re-verify preconditions: EEPROM 0xFF still `0x00`; `witness.scan` D-cycle-27 (count=1 phys=0x03eb3000); `witness.scan-self` count=0.
9. Composite capture **SKIPPED** — cycle-34 + cycle-36 reproduced ffmpeg silent-stall; cycle-40 primary signal (EEPROM byte) is agent-side and reliable; secondary signal (`witness.scan-self`) is also fully agent-side; composite stripes would only matter for G1..G4 outcomes which require count>=1 (cycles 26..36 all show count=0).
10. First `runxbe` attempt — wrong syntax (`runxbe E:\…`) → agent rejected: `oracle error: usage: runxbe path=<xbox-path>`. Xbox state unchanged.
11. Second `runxbe` attempt — correct syntax (`runxbe path=E:\Apps\witness-only\default.xbe`) → `200 launching` at 022351Z.
12. Poll dashboard FTP recovery: ping=false at t+2s/t+4s (Xbox went down for ~4s); ping+FTP=true at t+6s; auth OK at t+6s. **RECOVERED_AT=t+6s**.
13. `ensure-agent` (post-chainload, dashboard side) → v0.5 re-launched.
14. Final witness scans → `witness.scan` D-cycle-27 (count=1 phys=0x03eb3000 reserved0=0 reserved1=0); `witness.scan-self` count=0.
15. `eeprom.scratch.read` → `off=0xFF byte=0xA4 tag=0xA stage_nib=0x4 interp="cycle-39 self-witness pre-MmAlloc breadcrumb (sub-case (c) if WTNS count=0; G1..G4 if WTNS count>=1)"`. Full `eeprom` hex dump cross-checked — last byte of 256-byte image at offset 0xFF = `A4`.

**Reproducibility wins this cycle**: `phys=0x03eb3000` deterministic kernel-pool reuse REPRODUCED across cycle-40 baseline + post-deploy + post-witness-upload + post-chainload (4 readbacks this session) + 20+ consecutive observations across cycles 26..40; `mapped_pages_seen=419` REPRODUCED at every readback this cycle; `witness.scan-self count=0` REPRODUCED across cycle-40 baseline + post-chainload (matching cycle 36 G0 evidence + cycle-29..36 consistent pattern); EEPROM non-volatility across two soft reboots within this session CONFIRMED (baseline byte `0x00` survived reboot 2; post-chainload byte `0xA4` survived chainload-induced reset).

**Recommended cycle 41 next step (bounded; cycle-39 closure's pre-recorded contingent path).** With G0(c) confirmed, cycle 41 explores `MmAllocateContiguousMemoryEx` allocation-flag variations in `lib/xbed_self_witness.c:133-138`. Current call signature:

```c
MmAllocateContiguousMemoryEx(0x1000u,             /* size: 1 page */
                              0x00010000u,         /* lowest phys */
                              0x03ffffffu,         /* highest phys */
                              0x1000u,             /* alignment */
                              PAGE_READWRITE);
```

Bounded variations (low scope → higher scope) to try in cycle 41 (probably split across cycles 41a/41b if multiple attempts needed):

1. **Cache policy** — try `PAGE_READWRITE | PAGE_NOCACHE` and `PAGE_READWRITE | PAGE_WRITECOMBINE`. The cycle-29 design comment notes the allocation pattern is "known-safe and known-findable" but did not specify a cache policy beyond plain `PAGE_READWRITE`; the real-Xbox kernel may reject the specific cache policy combination on this kernel/BIOS revision.
2. **Address-floor / highest-phys** — try `highest=0xFFFFFFFFu` (no upper limit) and `lowest=0x00000000u` (no lower limit) to broaden the search.
3. **Alignment** — try `alignment=0` (no specific alignment requirement).
4. **Fall back to `MmAllocateContiguousMemory` (no -Ex)** — simpler kernel surface; may dodge a real-Xbox quirk specific to the -Ex variant on this kernel revision.

Cycle-40 G0(c) is the cycle-41 regression gate: EEPROM byte at 0xFF must remain `0xA4` (the sticky `s_eeprom_scratch_attempted` Codex-round-2 P1 fix ensures the byte is preserved across later fires); a successful variation should advance `witness.scan-self count >= 1` with `reserved1` matching the count of fires that landed (cycle-35 design ordering: stage-4 .CRT$XXC + stage-5 .CRT$XCU + cycle-29 in-main stages).

DEFERRED beyond cycle 41 (only if multiple allocation-flag variations all fail):
- Custom XBE-header callback that runs before nxdk's `_start` — RULED OUT for the G0(c) sub-case by cycle 40, but may resurface if cycle-41 variations all fail in ways suggesting the kernel surface is fundamentally unusable from this pre-main context.
- Bypass `MmAllocateContiguousMemoryEx` entirely + use raw `MmAllocatePhysicalMemoryEx` or static `.bss` pre-allocated buffer + explicit `MmGetPhysicalAddress` lookup at runtime — wider kernel surface change.

**Secondary findings filed for cycle 41+ consideration (NOT cycle-40 scope).** (i) The cycle-39 oracle-agent's `cmd_info` runtime banner string was NOT updated to "v0.5" alongside the source banner-comment addendum — `info` still reports "v0.4 (Phase 2 + controller.* + smc.*)". The new verbs work correctly; only the banner is stale. A trivial 1-line fix in `oracle-agent/main.c::cmd_info` (or `oracle-agent/protocol.c::cmd_info` depending on where the banner string lives) would address this. Out-of-scope for cycle 40 (run-only slice). (ii) The agent `runxbe` verb requires explicit `path=` named-argument syntax (`runxbe path=E:\…`) — passing the path positionally (`runxbe E:\…`) returns `usage: runxbe path=<xbox-path>`. The cycle-39 README runbook's bare-bash example `runxbe path=E:\…` is correct; the agent's help-text response `runxbe path=<xbox-path>` already documents this clearly; no doc change needed but worth flagging because the analogous `mem.read`/`mem.write`/`nv2a.read`/`nv2a.write` verbs all use positional+named hybrid argument parsing.

**ZERO source/script code edits this cycle; ZERO XBE rebuilds; ZERO host xemu source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO `lib/xbed_self_witness.{c,h}` touched (cycle-39 EEPROM-scratchpad write + Codex round-2 sticky-flag gate intact); ZERO `lib/lib.mk` touched; ZERO `oracle-agent/*` touched (cycle-39 v0.5 verbs intact); ZERO `xbed_runtime.{c,h}` touched; ZERO `witness-only/main.c` / `Makefile` / `manifest.json` touched; ZERO `tools/xemu-capture/` source touched; ZERO `scripts/apple-silicon/composite-*.sh` touched; ZERO `nxdk/` source touched.**

**Codex SKIPPED per rule #15 run-only / doc-only carve-out** (no source diff; the deployed XBE binaries are exactly the cycle-39 builds — witness-only SHA `7528bb5bf4c9cc8f00c934c38e015166de7a6f89236ddc8e2ad2195a47ce4e0d`, oracle-agent SHA `d419b452f127e5a065a51b2d5bba49b6f2ec1f1826cdec57845793c34440cb53` — that passed Codex 3-round review at cycle-39 closure commit `33fb5b7e34`; `.claude/state/codex-validate-last-run` marker from cycle 39 remains the relevant marker for the deployed artifacts).

Pre-existing tracked-but-uncommitted modifications to `scripts/apple-silicon/capture-composite-reference.sh` + `retail-gameplay-oracle.py` + `retail-oracle-workflow.py` + `retail-title-automation-proof.py` PRESERVED unstaged per the rolling Hermes-supervision pattern (carried forward from cycles 34..39); pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged; 18+ pre-existing untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files at repo root preserved un-staged per the same pattern (`.hermes_cycle40_real_xbox_prompt.txt` + `.hermes_cycle40_real_xbox.log` + `.hermes_launch_cycle40.sh` from this cycle's Hermes supervisor in that set).

**Evidence-only files** (gitignored per `/benchmark-runs/` rule): `benchmark-runs/cycle40-real-xbox-eeprom-discriminator-20260524T021727Z/{SUMMARY.md, 00-baseline-pre-deploy.txt, 01-reboot.txt, 02-ftp-recovery.txt, 02b-ftp-auth.txt, 03-deploy-agent.txt, 03c-new-verbs-probe.txt, 04-eeprom-baseline.txt, 05-pre-witness-upload-status.txt, 06-pre-witness-upload-reboot.txt, 07-witness-upload-and-baseline-recheck.txt, 08-chainload-and-recovery.txt, 09-runxbe-issue.txt, 10-chainload-recovery.txt, 11-post-run-evidence.txt}`.

M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-41+ allocation-flag variation work + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

Cycle 40 closes with the G0(c) discriminator answer landed via the cycle-39 EEPROM scratchpad mechanism working as designed on real Xbox; cycle 41 allocation-flag variation design is Hermes's call.

## 2026-05-23 (cycle 39 EEPROM scratchpad pre-`MmAllocateContiguousMemoryEx` discriminator — bounded implementation slice CLOSED on `apple-silicon-performance`) — Codex-validated; ships cycle-38-recommended Option C; cycle-40 real-Xbox run is Hermes's call

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "implement the cycle-38 recommended next step — add an EEPROM scratchpad discriminator that writes a durable breadcrumb inside `xbed_self_witness_fire` BEFORE `MmAllocateContiguousMemoryEx`, and add the paired oracle-agent read/write surface needed to recover that breadcrumb on real Xbox; lowest-cost highest-information discriminator for the remaining G0 sub-cases by letting a future real-Xbox run distinguish (c) `MmAllocateContiguousMemoryEx` returned NULL silently from (a) pre-`.CRT$X*` startup crash and (b) crash inside `_witness_only_pre_main_crt_xx` before the allocation call". ZERO host xemu source touched. Cycle-40 real-Xbox deployment is NOT in this slice.

**Design — EEPROM scratchpad contract.** Single byte at EEPROM offset `0xFF` (last byte of the 256-byte image; documented as part of the 0xC0..0xFF reserved/unused tail in the standard Xbox EEPROM layout — consistently zero on stock OEM consoles). Encoding: `0xA0 | (stage & 0x0F)` — high nibble `0xA` reuses the existing "Path-A.4 tag" semantics; low nibble = stage. On real-Xbox witness-only the first call into `xbed_self_witness_fire` is always the `.CRT$XXC` slot's stage=4 fire (cycle 35 ordering) → expected post-run byte = `0xA4`. Mechanism: `HalWriteSMBusValue(0xA8, 0xFF, FALSE /* byte mode */, encoded_byte)` — same SMBus address the existing `cmd_eeprom` 256-byte dump uses for the 24LC02-class EEPROM. Write endurance: ≥1M cycles per byte; one write per cycle-39+ run is bounded across hundreds of debugging sessions.

**Fire-path gate — AT MOST ONCE per process (Codex round-2 P1 adopted).** Initial design gated the EEPROM write on the existing `s_witness_page == 0` first-call branch. Codex round-2 caught a P1 bug: in the exact G0(c) sub-case this discriminator targets, the allocation fails on the first call → `s_witness_page` stays NULL → later `.CRT$XCU` (stage=5) and in-main (stage=1/3) fires re-enter the first-call branch and OVERWRITE the breadcrumb byte from `0xA4` to `0xA5`/`0xA1`/`0xA3`, destroying the cycle-39 discriminator value. Fix: a NEW sticky `s_eeprom_scratch_attempted` static flag is set BEFORE the write (rather than only on `NT_SUCCESS`) so the byte is preserved regardless of allocation outcome and the write is never re-attempted. The host-log line distinguishes "wrote" vs "failed" for the xemu-visible diagnostic path.

**Agent surface — read + reset verbs.** Two new verbs in `oracle-agent/{commands.c,commands.h,main.c}`:
- `eeprom.scratch.read` — reads byte at offset `0xFF` via existing `HalReadSMBusValue` path; returns `off=0xFF byte=0x.. tag=0x. stage_nib=0x. interp="..."`. Interpretation table (Codex round-1 P2 adopted — only byte=`0xA4` is a valid cycle-39 breadcrumb; other `0xA?` values are explicitly "indeterminate" rather than aliased onto sub-case (c)):
  - `0x00` → "cleared (cycle-39 baseline; sub-case (a)+(b) if this is a POST-run reading)"
  - `0xA4` → "cycle-39 self-witness pre-MmAlloc breadcrumb (sub-case (c) if WTNS count=0; G1..G4 if WTNS count>=1)"
  - other `0xA?` → "indeterminate (TAG nibble matches but stage_nib != 4; rerun with eeprom.scratch.reset baseline)"
  - else → "indeterminate (not a cycle-39 breadcrumb; possibly stale pre-baseline value or foreign write — rerun with eeprom.scratch.reset baseline)"
- `eeprom.scratch.reset` — writes `0x00` to offset `0xFF` via `HalWriteSMBusValue`; gated by the existing `s_unsafe_writes_enabled` flag (requires `unsafe.enable` first). Hermes calls this BEFORE each cycle-40 real-Xbox run to establish a known clean baseline.

Both verbs registered in the `s_cmds[]` dispatch table; help-text rows added; banner-comment addendum at top of `oracle-agent/main.c` marking the `v0.5 eeprom.scratch.*` surface.

**Cycle-40 discriminator table (extends cycle-35 G-rows).** Reads as `(EEPROM byte at 0xFF, witness.scan-self count, witness.scan-self reserved1, witness.scan shape)` (full table in `witness-only/README.md` cycle-39 addendum):

| EEPROM 0xFF | scan-self count | scan-self reserved1 | scan shape | G-row | Interpretation |
|---|---|---|---|---|---|
| `0x00` | 0 | n/a | `D-cycle-27` | **G0(a)+(b)** | EEPROM write never executed → crash strictly before `xbed_self_witness_fire` reached its pre-`MmAlloc` instruction. cycle-39 cannot distinguish (a) from (b); cycle 40 would need a pre-`.CRT$X*` callback (high scope) only if forced. |
| `0xA4` | 0 | n/a | `D-cycle-27` | **G0(c)** | EEPROM write landed → `MmAllocateContiguousMemoryEx` returned NULL silently OR crashed. Sub-cases (a)+(b) ELIMINATED → cycle 40 explores allocation-flag variations (`PAGE_WRITECOMBINE` vs `PAGE_NOCACHE`; address-floor / alignment). |
| `0xA4` | ≥1 | 1..4 | `D-cycle-27` or `A1/A2` | **G1..G4** | Reverts to the cycle-35 G1..G4 interpretations on the WTNS path. |
| any other | n/a | n/a | n/a | indeterminate | Force rerun with explicit `eeprom.scratch.reset`. |

**Implementation surface (4 source files + 1 doc/manifest).**

1. `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.h` — 3 new `#define`s (`XBED_SELF_WITNESS_EEPROM_SMBUS_ADDR = 0xA8`, `XBED_SELF_WITNESS_EEPROM_SCRATCH_OFF = 0xFF`, `XBED_SELF_WITNESS_EEPROM_TAG_NIB = 0xA0`) + ~85-line cycle-39 head-comment addendum explaining the scratchpad contract, discriminator semantics, fire-path gate rationale (Codex P1 callout in the comment), reset/read semantics, and the EEPROM offset selection rationale.
2. `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c` — 1 new sticky-flag static (`s_eeprom_scratch_attempted`) + ~50-LOC code block inside the existing first-call branch (positioned BEFORE `MmAllocateContiguousMemoryEx`; gated by `!s_eeprom_scratch_attempted` AT MOST ONCE per process; sets the flag BEFORE the write so failure does not cause re-attempt). ZERO change to the public API. ZERO change to the existing allocation / `MmGetPhysicalAddress` / `MmPersistContiguousMemory` / page-wipe / `wbinvd` flush / second-call no-op paths.
3. `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c` — 2 new command implementations (`cmd_eeprom_scratch_read` + `cmd_eeprom_scratch_reset`) + 2 new help-text lines + 2 new `#define`s (`EEPROM_SCRATCH_OFF`, `EEPROM_SCRATCH_TAG_NIB`). Read uses the existing `HalReadSMBusValue` + 4-branch decode-and-format pattern. Reset uses `HalWriteSMBusValue` + the existing `s_unsafe_writes_enabled` gate (same pattern `cmd_mem_write` / `cmd_nv2a_write` / `cmd_smc_write` use). ZERO change to existing verbs.
4. `scripts/apple-silicon/xbe-tests/oracle-agent/commands.h` — 2 new function-declaration lines (with cycle-39 head comment).
5. `scripts/apple-silicon/xbe-tests/oracle-agent/main.c` — 2 new lines in the `s_cmds[]` dispatch table + 1 new banner-comment addendum (`v0.5 eeprom.scratch.*` 2026-05-24).
6. `scripts/apple-silicon/xbe-tests/witness-only/main.c` — UNCHANGED. The cycle-29 shim picks up the new EEPROM write code automatically because `main.c` already calls `xbed_self_witness_fire` from cycle-35's `.CRT$XXC` + `.CRT$XCU` slots and cycle-29's in-`main()` fires.
7. `scripts/apple-silicon/xbe-tests/witness-only/Makefile` — UNCHANGED.
8. `scripts/apple-silicon/xbe-tests/witness-only/manifest.json` — title extended (`+ cycle-39 EEPROM scratchpad pre-MmAlloc discriminator`); new `real-xbox/physical/cycle-40` expected_results section enumerating the G0(a)+(b) / G0(c) / G1..G4 / indeterminate rows + cycle-40 deployment runbook delta vs cycle 36.
9. `scripts/apple-silicon/xbe-tests/witness-only/README.md` — cycle-39 addendum (~120 LOC) with scratchpad contract table + cycle-40 G-row discriminator table + cycle-40 deployment runbook (11-step sequence extending cycle-36's with the 3a `eeprom.scratch.reset` baseline step + 11 `eeprom.scratch.read` recovery step) + design-choice rationale table (chosen vs rejected candidates per cycle 38 closeout) + cycle-39 source impact + local validation evidence note.

**Build.** Both XBEs rebuilt cleanly via `eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && make`. ZERO new warnings (benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles).

- `witness-only/bin/default.xbe` — 155 648 B (UNCHANGED size from cycle 35; the new SMBus-write code + sticky flag + 3 host-log lines fit within the existing nxdk XBE page boundary). SHA-256 = `7528bb5bf4c9cc8f00c934c38e015166de7a6f89236ddc8e2ad2195a47ce4e0d` (changed from cycle-35's `ab52df8d…` since the static `.text` now contains the `HalWriteSMBusValue` call + sticky-flag check + cycle-39 host-log format strings; main() unchanged).
- `witness-only/witness-only.iso` — 720 896 B (unchanged size — same ISO sector boundary). SHA-256 = `d2c2089913267215f51d5fb24d050c20482e859cda72cf6a7c9370fb7e626bd4`.
- `oracle-agent/bin/default.xbe` — 417 792 B (unchanged size). SHA-256 = `d419b452f127e5a065a51b2d5bba49b6f2ec1f1826cdec57845793c34440cb53`.
- `oracle-agent/oracle-agent.iso` — 983 040 B (unchanged size). SHA-256 = `283b11d3b201eb8ff1480e602c2701314fdd3ea7d6163792a7d3dbeeaa2d9cca`.

**Local validation.** Cold-boot xemu smoke (`dist/xemu.app/Contents/MacOS/xemu -config_path /tmp/cycle39-smoke/xemu.toml -display none -nographic` against `witness-only.iso` with `XEMU_GUEST_LOG=1`) did NOT reach the XBE within the bounded slice's wait budget — cold boot through BIOS to DVD-launch via the .app wrapper exceeds the ~120 s envelope used in this session; the cycle-35 closure's claimed "12 s spawn" almost certainly depended on a pre-warmed snapshot path not available in this bounded session. **Structural correctness coverage** therefore rests on: (i) clean nxdk lld link of both XBEs with ZERO new warnings; (ii) the cycle-29 shim's existing Codex-validated first-call branch (where the new EEPROM write is inserted as a sticky-flag-gated additive instruction sequence); (iii) xemu's QEMU `smbus-eeprom` device implementing both `eeprom_receive_byte` and `eeprom_write_data` per `hw/i2c/smbus_eeprom.c:52-83` — so the SMBus write call IS honored in emulation when reached, ruling out a "broken in xemu" failure mode; (iv) Codex 3-round source review (see below). The real discriminator answer lives in cycle 40 (real-Xbox run + post-run `eeprom.scratch.read`).

**Codex validation per rule #15 (mandatory — non-trivial diff > 30 LOC in apple-silicon scripts; ~975 lines aggregate including ~85 LOC `lib/` source + ~90 LOC agent source + ~118 LOC README + ~6 LOC manifest + ~120 LOC headers/comments):**

- **Round 1 = MAJOR ISSUES (1 P2 finding adopted).** `cmd_eeprom_scratch_read` aliased ALL `0xA?` values onto sub-case (c), but the cycle-40 G-row table explicitly classifies non-`0xA4` TAG-nibble values as "indeterminate" (rerun with `eeprom.scratch.reset` baseline). FIX: split the middle branch into `byte == 0xA4` (sub-case (c)) vs `(byte & 0xF0) == 0xA0` but ≠ `0xA4` (indeterminate TAG-nibble-match) vs else (indeterminate). Rebuilt oracle-agent.
- **Round 2 = MAJOR ISSUES (1 P1 finding adopted).** EEPROM write was gated on the existing `s_witness_page == 0` first-call branch; in the exact G0(c) sub-case this discriminator targets, allocation fails → `s_witness_page` stays NULL → later `.CRT$XCU`/in-main fires re-enter the block and OVERWRITE the `0xA4` breadcrumb with `0xA5`/`0xA1`/`0xA3`, destroying the discriminator value. FIX: added a NEW sticky `s_eeprom_scratch_attempted` static flag set BEFORE the write (so failure does not cause re-attempt either); updated the cycle-39 head comment in `xbed_self_witness.h` + README scratchpad-contract table row to document the at-most-once semantics. Rebuilt witness-only.
- **Round 3 = MAJOR ISSUES (1 P1 finding DEFLECTED — out of cycle-39 scope).** Codex flagged that `retail-gameplay-oracle.py:49` (and 2 sibling `retail-*.py` files) import `composite_preflight` which is currently only an UNTRACKED file at `scripts/apple-silicon/composite_preflight.py`. This finding is correct on its merits BUT is about pre-existing tracked drift that the cycle-34..38 prompts explicitly told me to PRESERVE unstaged per the rolling Hermes-supervision guardrail. The cycle-39 closing commit deliberately does NOT stage either the 4 tracked `retail-*` / `capture-composite-reference.sh` drift files OR the untracked `composite_preflight.py` file. The cycle-39 source surface (`lib/`, `oracle-agent/`, `witness-only/`) has no findings in round 3. DEFLECTED with documented reason; the responsibility for resolving the tracked-drift dependency belongs to whichever future cycle commits those 4 files (currently held back per the Hermes guardrail).
- Codex marker written at `.claude/state/codex-validate-last-run` with the round-3 fingerprint.

**Hypothesis state update (vs cycle 38 closure).** Three live G0 sub-cases remain (carried over from cycle 38): (a) pre-`.CRT$X*` startup crash; (b) helper-body crash before `xbed_self_witness_fire`'s pre-MmAlloc instruction; (c) `MmAllocateContiguousMemoryEx` returns NULL silently. Cycle 39 ships the discriminator that uniquely splits (c) from (a)+(b) on a future real-Xbox run; cycle-22 pre-main-crash hypothesis remains FULLY CORROBORATED; γ.1 remains INVALIDATED (cycle 36 G0 outcome). No new hypothesis introduced this cycle — the slice is pure implementation of the cycle-38 recommendation.

**Recommended cycle-40 next step (Hermes's call; NOT this session).** Deploy the cycle-39 oracle-agent (v0.5) + cycle-39 witness-only XBE on real Xbox using the cycle-40 11-step runbook in `witness-only/README.md`. Steps 3a (`unsafe.enable` + `eeprom.scratch.reset`) and 11 (`eeprom.scratch.read`) bracket the existing cycle-36 chainload window. The post-run EEPROM byte at offset `0xFF` is the primary cycle-40 signal; combine with `witness.scan-self count`/`reserved1` + composite stripes per the cycle-40 G-row table. If outcome = G0(c) (`byte=0xA4 + count=0`), cycle 41 explores `MmAllocateContiguousMemoryEx` allocation-flag variations (cache policy `PAGE_WRITECOMBINE` vs `PAGE_NOCACHE`; tighter / looser address floor; alignment changes). If outcome = G0(a)+(b) (`byte=0x00 + count=0`), cycle 41 needs a custom XBE-header callback that runs BEFORE nxdk's `_start` (significantly higher scope — requires modifying `nxdk/tools/cxbe/`; only fund if forced).

**Evidence-only files** (none this cycle — the source diff IS the evidence; the cycle-39 build artifacts replace the cycle-35 deployed binaries in the working tree).

**ZERO host xemu source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO `lib/lib.mk` touched; ZERO `oracle-agent/{controller,smc,tier2,protocol}.{c,h}` touched; ZERO `lib/xbed_runtime.{c,h}` touched; ZERO image-blit touched; ZERO `witness-only/main.c` touched (cycle-35 source intact; the new EEPROM-write code is picked up automatically through the cycle-35 `.CRT$X*` slot path → cycle-29 shim); ZERO `witness-only/Makefile` touched; ZERO `tools/xemu-capture/` source touched; ZERO `scripts/apple-silicon/composite-record.sh` / `composite-preflight.sh` source touched; ZERO `nxdk/` source touched.**

Pre-existing tracked-but-uncommitted modifications to `scripts/apple-silicon/capture-composite-reference.sh` + `retail-gameplay-oracle.py` + `retail-oracle-workflow.py` + `retail-title-automation-proof.py` PRESERVED unstaged per the rolling Hermes-supervision pattern (carried forward from cycles 34..38); pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged; 17+ pre-existing untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files at repo root preserved un-staged per the same pattern.

M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-40+ real-Xbox EEPROM-readback discrimination + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

Cycle 39 closes with the EEPROM-scratchpad discriminator implementation landed + Codex-validated (2 rounds of findings adopted, 1 round of out-of-scope finding deflected); cycle-40 real-Xbox deployment is Hermes's call.

## 2026-05-23 (cycle 38 lld link-map symbol resolution for the cycle-37 unexplained `.text 0x16720` walker fn-ptr — bounded tooling-only slice CLOSED on `apple-silicon-performance`) — concrete positive result; cycle-37 F5 REVISED; cycle-39 recommendation filed

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "execute the cycle-37 recommended next step by regenerating / extracting link-map evidence for the existing `witness-only` build so we can resolve which symbol corresponds to the unexplained `.text 0x16720` singleton function pointer in the extra `.CRT$X*` walker group; determine whether that extra walker-group entry is a benign nxdk default initializer or a substantive pre-main path unique to `witness-only`." ZERO source / script / nxdk / host xemu / lib edits; ZERO real-Xbox run. ONE bounded rebuild (witness-only only, `LDFLAGS=-map:<path>` added; identical `.obj` inputs; resulting binary structurally bit-identical to cycle-35 modulo 8 timestamp bytes total; tracked artifacts restored to cycle-35 deployed bytes from git HEAD post-rebuild). Pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` + 14 untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files + untracked `composite_preflight.py` preserved unstaged per cycle-34+ prompt guardrail (carried forward).

**Methodology.** Tooling-only: re-link the cycle-35 `witness-only` build with one additional lld flag (`-map:<path>`; lld-link PE/COFF syntax — not GNU ld's `-Map=`) and read the resulting linker map. Reproducibility verified: `cmp -l` of cycle-35 vs cycle-38 `default.xbe` shows ONLY 6 bytes differ across 3 separate 2-byte spans (file offsets 277/278, 329/330, 381/382 — three XBE timestamp embeddings); `main.exe` shows ONLY 2 bytes differ at offsets 129/130 (one COFF timestamp). `.text` / `.rdata` / `.data` / `.tls` content is bit-identical between cycle-35 deployed and cycle-38 rebuild — the cycle-38 map is therefore authoritative for the cycle-35 deployed binary's symbol layout. Post-rebuild restoration: tracked artifacts (`bin/default.xbe` + `witness-only.iso`) restored via `git checkout --`; gitignored `main.exe` restored from local backup. Post-restore SHA-256s match cycle-35 originals exactly (`ab52df8d…` / `a8033fef…` / `6f4ecd59…`).

**Findings (numbered F38.1..F38.5 — full detail under `benchmark-runs/cycle38-link-map-symbol-resolution-20260524T000443Z/SUMMARY.md`):**

- **F38.1 — `.text 0x16720` resolved.** `_automount_d_drive` in `libnxdk_automount_d:automount_d.obj`. Map excerpt: `0001:00005720 _automount_d_drive 0000000000016720 libnxdk_automount_d:automount_d.obj`. Source: `nxdk/lib/nxdk/automount_d.c:35` registers `automount_d_drive_p` in `.CRT$XIT` via `__attribute__((section(".CRT$XIT"), used))`.
- **F38.2 — `.CRT$XIT` is an nxdk DEFAULT, NOT a witness-only opt-in.** `nxdk/lib/nxdk/Makefile:38-41` force-includes `_automount_d_drive` and links `libnxdk_automount_d.lib` unless `NXDK_DISABLE_AUTOMOUNT_D=y`. Neither witness-only's Makefile, lib.mk, mirror's Makefile, nor pipeline-smoke's Makefile sets that flag. Therefore all three nxdk XBEs include `_automount_d_drive`. Witness-only does NOT pull in any extra `.CRT$XIT` contributor that mirror/pipeline-smoke lack.
- **F38.3 — Full CRT walker subsection layout enumerated.** Witness-only's three semantic CRT walker passes (per `nxdk/lib/pdclib/platform/xbox/crt_initializers.c`): `_PDCLIB_xbox_run_pre_initializers` walks `.CRT$XXA..XXZ` invoking 3 functions in order — `_witness_only_pre_main_crt_xx` (cycle-35; at `.text 0x11000`), `_fls_init` (nxdk default `libwinapi:fiber.obj`; at `.text 0x2bd00`), `_tsc_freq_init` (nxdk default `libwinapi:profiling.obj`; at `.text 0x2d950`). `_PDCLIB_xbox_run_crt_initializers` walks `.CRT$XIA..XIZ` invoking 1 function — `_automount_d_drive` (nxdk default; at `.text 0x16720`) — then walks `.CRT$XCA..XCZ` invoking 1 function — `_witness_only_pre_main_crt_xc` (cycle-35; at `.text 0x11040`). Total: 5 walker invocations. Net delta vs a baseline non-witness nxdk XBE: exactly +2 walker entries (the two cycle-35 slots), matching cycle 35's design.
- **F38.4 — Cycle-37 dump entries past `.CRT$XXZ` are NOT walker entries.** VAs `0x3303c` onward in the cycle-37 dump resolve to `.text` addresses inside `libpdclib:malloc.obj` (`_dlmalloc_set_footprint_limit + 100`, `_internal_mallinfo + 256`, `_internal_mallinfo + 572`). These are most likely `.rdata`-resident pdclib malloc constants / vtables that the cycle-37 heuristic mis-classified as function pointers because they happen to fall in the `.text` VA range. They do NOT execute as part of any pre-main walker.
- **F38.5 — Cycle-37 F5 hypothesis REVISED.** The cycle-37 "+1 unexplained walker group" was a layout/heuristic artifact of counting zero-delimited fn-ptr runs in the merged `.CRT=.rdata` region. The semantic CRT walker entry count delta vs mirror is exactly +2 (the two cycle-35 slots; XCU + XXC) — NOT +1 walker group + +2 fn-ptrs. Cycle-37's candidate-source conjecture about `automount_d.obj` was directionally correct (the symbol IS `_automount_d_drive`) but the underlying assumption that this would represent a witness-only-unique drag-in was FALSE — it's an nxdk default present in all three nxdk XBEs (witness-only, mirror, pipeline-smoke).

**Net verdict.** The lld link-map analysis yields a **concrete positive symbol-resolution result** that REVISES (NOT confirms) the cycle-37 F5 hypothesis. The static binary surface of `witness-only` is now confirmed structurally identical to mirror modulo the 2 cycle-35 walker entries; there is **NO witness-only-unique pre-`.CRT$X*` code path** that mirror's pre-main slate does not exercise. The G0 real-Xbox crash narrows further to one of three sub-cases:

1. (a) crash inside nxdk's pre-`.CRT$X*` startup (`_start` / `__security_init_cookie` / TLS-size computation / `_PDCLIB_xbox_libc_init`) — the only fully-pre-CRT alternative;
2. (b) crash inside `_witness_only_pre_main_crt_xx`'s body BEFORE `MmAllocateContiguousMemoryEx` is reached (cycle-29 self-witness shim path);
3. (c) `MmAllocateContiguousMemoryEx` returns NULL silently from `.CRT$XXC` (edge case in `lib/xbed_self_witness.c:54-74`).

The cycle-37 "F5 +1 walker group" alternative (a witness-only-unique pre-main code path drag-in) is **ELIMINATED**. γ.0 narrowing is finer-grained but still pre-`main()`; cycle-22 leading hypothesis remains FULLY CORROBORATED; γ.1 remains INVALIDATED (cycle 36 G0 outcome).

**Recommended cycle-39 next step (lowest-cost, highest-information).** The static surface has now been exhausted as a discriminator for the 3 remaining G0 sub-cases. The next genuine information requires dynamic instrumentation on real Xbox. The cycle-36 Option C (EEPROM scratchpad write inside `xbed_self_witness_fire` BEFORE `MmAllocateContiguousMemoryEx`, paired with an agent EEPROM read-back via the `unsafe.enable` + EEPROM-write surface) uniquely discriminates sub-case (c) from (a)+(b). If the EEPROM scratchpad tick survives (helper body ran but allocation returned NULL), G0 narrows to (c) and cycle 40 can attempt allocation-flag variations. If the tick does NOT land, G0 narrows to (a) — strictly pre-`.CRT$X*` — and cycle 40 needs a custom XBE-header callback mechanism (significantly higher scope, requires nxdk modification). Other candidates considered and rejected: an `out 0xe9` host-log breadcrumb (provides no real-Xbox visibility; SKIP); a `.CRT$XCV` slot between `.CRT$XCU` and `main()` (adds no new information beyond cycle-35's stage-4 fire — cycle 36 G0 outcome already shows the earlier `.CRT$XXC` slot didn't fire; SKIP).

**Evidence-only files** (gitignored per `/benchmark-runs/` rule): `benchmark-runs/cycle38-link-map-symbol-resolution-20260524T000443Z/{SUMMARY.md, witness-only.map, 04-crt-walker-symbol-resolution.txt, 05-reproducibility-evidence.txt, main.exe.cycle35-original, default.xbe.cycle35-original}`.

**ZERO source/script code edits this cycle; ZERO XBE rebuilds that altered the deployed binary (cycle-38 rebuild restored to cycle-35 bytes); ZERO host xemu source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact); ZERO `lib/lib.mk` touched; ZERO `oracle-agent/*` touched; ZERO `xbed_runtime.{c,h}` touched; ZERO `witness-only/main.c` / `Makefile` / `manifest.json` touched; ZERO `tools/xemu-capture/` source touched; ZERO `composite-record.sh` / `composite-preflight.sh` touched; ZERO `nxdk/` source touched.**

**Codex SKIPPED per rule #15 trivial-work carve-out** (slice did not produce any non-trivial code change; the one rebuild was a tooling-only `-map`-flag add and the resulting deployed-binary bytes were restored to the cycle-35 Codex-validated artifact; no source diff in xemu-fork apple-silicon scripts beyond doc/state updates). The cycle-35 binary observed bit-identically is the cycle-35 build that passed 4-round Codex green at cycle-35 closure commit `515e03f4e7`; cycle-35 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed artifact.

M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-39+ EEPROM-scratchpad work OR alternative pre-`.CRT$X*` discriminator + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

Cycle 38 closes with the `.text 0x16720` symbol question definitively answered; cycle-39 EEPROM-scratchpad-write design is Hermes's call.

## 2026-05-23 (cycle 37 static XBE binary diff — `witness-only` vs `pipeline-smoke` + `mirror` — bounded analysis-only slice CLOSED on `apple-silicon-performance`) — narrowed-but-not-conclusive negative result; cycle-38 recommendation filed

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "execute the lowest-risk highest-value G0 follow-up from the cycle-36 closeout by performing a static binary-diff / structural-comparison investigation between the failing cycle-35 `witness-only` XBE and at least one known-good nxdk XBE that boots and paints on real Xbox (`pipeline-smoke` and/or `mirror`)." ZERO XBE rebuilds; ZERO source / script / nxdk / host xemu / lib edits; ZERO real-Xbox run; ZERO Codex (analysis/doc-only per the cycle-37 prompt). Pre-existing tracked drift in 4 `scripts/apple-silicon/*.{sh,py}` + 13 untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files + untracked `composite_preflight.py` preserved unstaged per cycle-34 prompt guardrail (carried forward).

**Comparison targets.** Both `pipeline-smoke` (110 592 B; pure nxdk, no `lib/lib.mk`) AND `mirror` (147 456 B; uses `lib/lib.mk` — same shared runtime as witness-only). Justification per `.claude/rules/oracle-and-xbe.md`: pipeline-smoke's PNG SHA-256 matches `expected.py:default()` byte-for-byte (Phase 3.0 Tier-4 known-good); mirror PASSes on xemu-Metal vs math-derived oracle and has been demonstrated boot-functional in prior oracle-pipeline runs. Mirror is the strictest comparison because the source-side delta vs witness-only is small: witness-only's `main.c` is different + witness-only opts in `lib/xbed_self_witness.c` + witness-only adds the cycle-35 `.CRT$XCU` + `.CRT$XXC` slots.

**Methodology.** Purpose-built Python XBE parser (`benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/xbe_parse.py`; field offsets cross-checked against Caustik's XBE specification + nxdk `tools/cxbe` source — project rule #5 "build tools rather than substitute weaker evidence"). Three diff axes: (1) XBE header / section / TLS structure; (2) kernel thunk-table imports (decoded with debug XOR key `0xEFB1F152` / retail key `0x5B6D40B6` per XBE spec); (3) `.CRT$X*` static-initializer walker arrays in `.rdata` (merged from `.CRT*` subsections via the `#pragma comment(linker, "/merge:.CRT=.rdata")` in `nxdk/lib/pdclib/platform/xbox/crt_initializers.c`).

**Findings (numbered F1..F6 — full detail under `benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/SUMMARY.md`):**

- **F1 — XBE-format-level: witness-only is well-formed.** All three XBEs share identical `base_vaddr=0x10000`, `size_of_headers=376`, `num_sections=4` (`.text` / `.rdata` / `.data` / `.tls` in that order), `init_flags=0x05`, `pe_stack_commit=65 536`, `pe_heap_reserve=1 048 576`, library count = 1, entry-point XOR key = retail, and bitwise-identical section flag bitmaps per section. Witness-only is NOT malformed at the XBE-header level.
- **F2 — TLS layout is IDENTICAL across all three XBEs.** Per-XBE `.tls` section vsize=276 raw_size=4; TLS directory `size_of_zero_fill=0`, `tls_callback_va=0x00000000` (no TLS callback functions registered), `raw_data_end - raw_data_start = 0x110` bytes. **Sub-case (a) "TLS-size computation crash" candidate from the cycle-36 G0 enumeration is UNSUPPORTED by witness-only's TLS surface** — it is bit-identical to mirror's, which boots fine on real Xbox.
- **F3 — Kernel imports: witness-only ≡ mirror.** Witness-only imports 78 kernel ordinals; mirror imports the EXACT same 78 ordinals; pipeline-smoke imports a 72-ordinal subset (missing {47, 100, 137, 156, 168, 173} — fully accounted for by the shared `lib/` stack that mirror also links). **NO kernel function imported by witness-only is missing from mirror.** The "`xbed_self_witness.c` introduces a new kernel call that crashes pre-main" hypothesis is UNSUPPORTED — all four `Mm*` calls in `lib/xbed_self_witness.c` (`MmAllocateContiguousMemoryEx`, `MmPersistContiguousMemory`, `MmGetPhysicalAddress`, `MmFreeContiguousMemory`) are already imported and used by `lib/xbed_runtime.c` in mirror.
- **F4 — Cycle-35 slots physically present in witness-only binary.** `strings -a -t x` recovers both cycle-35 host-log strings (`witness-only: .CRT$XXC pre-main breadcrumb running (cycle 35)` at file offset `0x22237`; `witness-only: .CRT$XCU pre-main breadcrumb running (cycle 35)` at `0x221f9`); `objdump -h main.obj` confirms `.CRT$XXC` (4 bytes) + `.CRT$XCU` (4 bytes) sections exist; `nm main.obj` confirms the helper-function bodies + slot variables. **The cycle-35 slots did NOT get dead-code-eliminated.**
- **F5 — CRT walker layout: witness-only has +1 walker group + +2 extra fn-ptrs vs mirror (only structural drift surfaced).** Walker-group counts (each group = a `0, fn1, fn2, ..., 0` array between sentinels): pipeline-smoke and mirror both have 4 groups (`[1, 2, 1, 2]` fn-ptrs); witness-only has 5 groups (`[1, 1, 3, 1, 2]` fn-ptrs). Net delta witness-only vs mirror: +1 walker group AND +2 fn-ptrs in another group. Cycle 35 added EXACTLY 2 new slots (`.CRT$XCU` + `.CRT$XXC`); the +2 fn-ptr increase matches that. The +1 EXTRA WALKER GROUP is NOT directly accounted for by cycle 35. Candidate sources: nxdk auto-link of an additional CRT-subsection contributor — `nxdk/lib/winapi/profiling.obj` (`.CRT$XXT`), `nxdk/lib/winapi/fiber.obj` (`.CRT$XXT`), `nxdk/lib/nxdk/automount_d.obj` (`.CRT$XIT`). `xbed_self_witness.c` is the only source-side delta between mirror and witness-only besides main.c text + the cycle-35 slots; it is plausible (but UNVERIFIED in this slice) that a symbol referenced from `xbed_self_witness.c` drags in one of these contributors that mirror's symbol set does not pull. The singleton fn-ptr in witness-only's extra walker group points at `.text 0x16720`; identifying the source symbol at that address would discriminate "benign additional nxdk-default initializer" from a genuinely-new pre-main code path that mirror's pre-main slate does not exercise.
- **F6 — Negative result: cycle-35 binary DOES boot on xemu.** Cycle-35 closure commit (`515e03f4e7`) records local-xemu smoke validation (`XEMU_GUEST_LOG=1`, 12 s spawn) emitted ALL FOUR expected WTNS-fire host-log lines in correct order — nxdk's CRT walkers DO call the cycle-35 slots successfully on xemu. The G0 crash is REAL-XBOX-ONLY. Static XBE bytes load identically on both hosts (XBEs have no load-time relocations), so the real-Xbox divergence is either (i) in nxdk's pre-`.CRT$XX*` startup code (`_start`, `__security_init_cookie`, TLS setup, `_PDCLIB_xbox_libc_init`) doing something the real-Xbox BIOS / kernel context disagrees with but xemu tolerates, OR (ii) in `MmAllocateContiguousMemoryEx`'s real-Xbox behavior diverging from xemu's emulation.

**Net verdict.** The static binary diff yields a **narrowed-but-not-conclusive negative result**: it RULES OUT four pre-`.CRT$XXC` failure modes that the cycle-36 closeout listed as live candidates (malformed XBE header per F1, TLS-size computation crash per F2, kernel-import surface mismatch per F3, "cycle-35 slots got dead-code-eliminated" per F4); it SURFACES one new narrowing question (the +1 unexplained CRT walker group per F5). The static surface alone cannot tell which sub-case of the cycle-36 G0 row is the actual hang site — that requires either dynamic instrumentation on real Xbox (cycle-36 Option C-style EEPROM scratchpad write) or a symbol-resolution step on the existing build.

**Recommended cycle-38 next step (lowest-cost, highest-information).** Re-link the existing witness-only build with lld's `--print-map` output (`-Wl,-Map=witness-only.map`) and resolve which symbol corresponds to the `.text 0x16720` singleton fn-ptr in witness-only's extra CRT walker group. This is a tooling-only change (no source edits, no slot additions; just one extra linker flag); it would either identify a benign nxdk-default initializer (eliminating the +1 walker-group hypothesis) OR identify a non-trivial pre-main code path unique to witness-only that becomes the next instrumentation target. If the latter, cycle-39 could place an `out 0xe9` host-log breadcrumb inside that symbol's entry to discriminate "this path executes on xemu but faults on real Xbox" via standalone xemu validation. If the map-file analysis returns negative, cycle-39 falls back to the cycle-36-listed Option C (EEPROM scratchpad write inside `xbed_self_witness_fire` before `MmAllocateContiguousMemoryEx`) which uniquely discriminates sub-case (c) from (a)/(b) but adds substantial scope.

**Evidence-only files** (gitignored per `/benchmark-runs/` rule): `benchmark-runs/cycle37-static-xbe-diff-20260523T232828Z/{xbe_parse.py, 01-xbe-headers.json, 02-crt-region-dump.txt, 03-evidence.txt, SUMMARY.md}`.

**ZERO source/script code edits this cycle; ZERO XBE rebuilds; ZERO host xemu source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact); ZERO `lib/lib.mk` touched; ZERO `oracle-agent/*` touched; ZERO `xbed_runtime.{c,h}` touched; ZERO `witness-only/main.c` / `Makefile` / `manifest.json` touched; ZERO `tools/xemu-capture/` source touched; ZERO `composite-record.sh` / `composite-preflight.sh` touched; ZERO `nxdk/` source touched.**

**Codex SKIPPED per the cycle-37 prompt's analysis/doc-only carve-out** (slice did not produce any non-trivial code change; no rule-#15 trigger fired). The cycle-35 binary observed this cycle is bit-identical to the cycle-35 build that passed 4-round Codex green at cycle-35 closure commit `515e03f4e7`; cycle-35 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed artifact.

M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-38+ map-file analysis OR fallback EEPROM scratchpad work + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

Cycle 37 closes with the static-diff investigation landed; cycle-38 map-file regeneration is Hermes's call.

## 2026-05-23 (cycle 36 Path A.4 cycle-35 pre-main breadcrumb on real Xbox — bounded run-only slice CLOSED on `apple-silicon-performance`) — OUTCOME G0; cycle-37+ scope depends on Hermes's decision

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "execute cycle 36 only — the real-Xbox discriminator run for the cycle-35 witness-only pre-main breadcrumb build, using the canonical cycle-36 runbook from `witness-only/README.md`." ZERO source/script/XBE edits; ZERO host xemu source touched.

**Outcome: G0** per the cycle-35 G-row discriminator table (`witness-only/README.md` cycle-35 addendum). Reads as `(stripes visible = none, witness.scan-self count = 0, witness.scan = D-cycle-27)`:

- `witness.scan` = `count=1 phys=0x03eb3000 reserved0=0 reserved1=0 mapped_pages_seen=419` — identical to cycles 28 / 30 / 32 / 34 (D-cycle-27 shape)
- `witness.scan-self` = `count=0` — **NO WTNS page allocated → not even `.CRT$XXC` slot (stage=4) ran**
- 26-snap NTSC composite burst over t+0..t+29.5s of the second runxbe: **ZERO stripe colors** (no RED / ORANGE / YELLOW / GREEN / BLUE band detected in any band position; 13/26 pure black, 13/26 dashboard transition/return frames including pre-runxbe snap_00 = 25 874 unique colors and dashboard-return snaps 20-22 = 27k..40k unique colors)

G0 means the crash occurred BEFORE the `.CRT$XXC` slot fired its `xbed_self_witness_fire(stage=4)` call. Three pre-`.CRT$XXC`-fire sub-cases share this G0 shape and cycle-35 evidence CANNOT distinguish them from each other without further instrumentation (real Xbox has no host-log breadcrumb channel):

1. Crash inside `_start` / `__security_init_cookie` / TLS-size computation / `_PDCLIB_xbox_libc_init` — i.e. strictly BEFORE `_PDCLIB_xbox_run_pre_initializers()` walked `.CRT$XX*` slots. **STRICTLY EARLIER than anything cycle 34 could distinguish.**
2. Walker invoked the `.CRT$XXC` slot but the helper function body crashed BEFORE reaching `MmAllocateContiguousMemoryEx` inside `xbed_self_witness_fire`.
3. `MmAllocateContiguousMemoryEx` returned NULL silently from `.CRT$XXC` (edge case in `lib/xbed_self_witness.c:54-74`; only distinguishable on standalone xemu via host-log line).

G1 / G2 / G2' / G3 / G4 ALL require `count >= 1` → G0 ELIMINATES all of them. F-row interpretation from the cycle-32 F-table is moot here because every F row assumes `main()` was at least attempted (G0 says even the pre-main slots didn't run, which is strictly earlier than any F-row precondition).

**Hypothesis state update:**

- (γ) "main() never reaches the fire calls" **FURTHER STRENGTHENED.** Cycle 30 / 34 left γ as LEADING; cycle 35 ADDED the pre-`main()` slot fire which would have INVALIDATED γ (count >= 1 while main() body still failed); cycle-36 G0 outcome shows the crash is so early that even `.CRT$XXC` — the FIRST user-C code the nxdk CRT runs in this XBE — never executed.
- Cycle-22 pre-main-crash hypothesis is **NARROWED FURTHER** beyond cycle-34's F4 to a strictly earlier pre-`.CRT$XXC` window. The cycle-32 F4 (γ.0 OR γ.1) is now refined to "γ.0 sub-narrowed to pre-`.CRT$XXC` crash" (γ.1 — `XVideoSetMode` faulting before returning — is INVALIDATED because `main()` is not even reached, so `XVideoSetMode` cannot be the failure mode).
- (α) "kseg0 scan can't find XCTR from non-agent context" and (β) "scan finds XCTR but write faults silently": REMAIN LIVE but FURTHER DEPRIORITIZED — both presuppose `main()` ran the cycle-23 fires, which the cycle-36 G0 outcome strongly refutes.
- Hypothesis #5 (cycle 24 "kseg0-scan witness real-Xbox-unsafe from non-agent context"): UNCHANGED PARTIALLY INVALIDATED in the catastrophic-hang sense; cycle 36 reproduced clean dashboard recovery in t+~26..38s across both runxbe attempts.

**Reproducibility wins this cycle:** `witness.scan-self count=0` REPRODUCED across both runxbe attempts in same physical power session; `witness.scan = D-cycle-27` reproduced across 5 readbacks this session (baseline + post-relaunch + post-first-runxbe + post-second-runxbe + post-burst-window) and ≥6 cycles (26 / 28 / 30 / 32 / 34 / 36) of consistent D-cycle-27 shape; kernel-pool deterministic `phys=0x03eb3000` reuse ≥5 readbacks this session + ≥20+ consecutive observations across cycles 26..36 + now across at least two physical power sessions (cycle 34 = first fresh-power session post Josh's restart; cycle 36 = same physical-power continuation); `mapped_pages_seen=419` REPRODUCED at every readback.

**Sequence executed** (16 logged steps under `benchmark-runs/cycle36-real-xbox-witness-only-pre-main-discriminator-20260523T225306Z/`, gitignored per project convention): 00 reachability (ping=true ftp=false agent=true — cycle-29 agent resident from cycle 34); 01 ensure-agent baseline (already listening); 02 baseline both scans MET (D-cycle-27 + count=0); 03 reboot 200; 04 poll dashboard FTP return (verbose curl LIST 226 confirms ready; first auth probe in a too-tight 4 s window had silently false-negatived, all subsequent probes succeed); 05 FTP-upload cycle-35 `default.xbe` (SHA-256 `ab52df8dee32c24b857b3df749e3b8fc0a5a7e8f0949e06ec5c7e4d82aaef5bd`, 155 648 B) `--overwrite` to `/E/Apps/witness-only/default.xbe` (uploaded=1; remote mtime advanced Dec 10 17:17 → Dec 11 02:20 = cycle-31 → cycle-35 file actually replaced on disk); 06 ensure-agent + recheck scans (preconditions still MET); 07 composite-preflight (`status=ok` via xemu-capture in 1.759 s); 08 composite-record.sh `--duration 80` (preflight OK; ffmpeg launched; SILENT-STALLED for 103 s, `ffmpeg_rc=137 capture_timed_out=true`, ZERO bytes stderr / video.mp4 — cycle-34 finding (i) reproduced in a SEPARATE physical power session, strengthening it from "one-off" to "reproducible"); 09 first `runxbe` issued 23:02:38Z; 10 dashboard FTP back at 23:03:16Z = t+38 s (clean recovery); 11 ensure-agent + final scans = (D-cycle-27, count=0) — already enough to land G0; 12 xemu-capture snap_00 pre-runxbe (capture path healthy, 25 874 unique colors); 13 second `runxbe` issued 23:06:43Z (cycle-34-style snapshot-burst substitution because composite-record.sh failed at step 8); 14 25-snap burst over t+0..t+29.5s with explicit `--width 720 --height 480`; 15 stripe analysis = ZERO stripe colors detected across all 26 snaps; 16 ensure-agent + final scans = (D-cycle-27, count=0) — REPRODUCED.

**Secondary findings filed for cycle 37+ consideration (NOT cycle-36 scope):** (i) **composite-record.sh ffmpeg silent-stall REPRODUCED in cycle 36** (already-filed cycle-34 secondary finding (i) is upgraded from "one-off cycle-32 + cycle-34" to "reproducible across power sessions"; the cycle-33 preflight correctly reports ok via xemu-capture but does NOT protect the subsequent ffmpeg long-form recording from the TCC inheritance asymmetry; cycle 37 candidate: add `--require-both-detectors` to composite-preflight.sh OR have composite-record.sh always run a brief ffmpeg liveness check before arming the full duration). (ii) **G0-vs-edge-case-(c) (allocation-failure) ambiguity** is documented in the G0 row itself (real Xbox lacks the host-log breadcrumb channel that would discriminate); cycle 37 candidate: instrument `xbed_self_witness.c` with a 4-byte EEPROM scratchpad write BEFORE the `MmAllocateContiguousMemoryEx` call (using the agent's `unsafe.enable` + EEPROM-write path); a successful EEPROM tick would discriminate (c) from (a)/(b). Adds substantial scope.

**ZERO source/script code edits this cycle; ZERO XBE rebuilds; ZERO host source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact); ZERO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact); ZERO `xbed_runtime.{c,h}` touched; ZERO image-blit touched; ZERO `witness-only/main.c` touched (cycle-35 source intact); ZERO `tools/xemu-capture/` source touched; ZERO `scripts/apple-silicon/composite-record.sh` source touched (cycle-33 implementation intact); ZERO `scripts/apple-silicon/composite-preflight.sh` source touched (cycle-33 implementation intact).**

**Codex SKIPPED under rule #15 doc-only / run-only carve-out** (same path as cycles 26 / 28 / 30 / 32 / 34); the cycle-35 binary observed this cycle is bit-identical to the cycle-35 build that passed 4-round Codex green at cycle-35 closure commit `515e03f4e7`; the cycle-35 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed artifact.

Pre-existing tracked-but-uncommitted modifications to `scripts/apple-silicon/capture-composite-reference.sh` + `retail-gameplay-oracle.py` + `retail-oracle-workflow.py` + `retail-title-automation-proof.py` PRESERVED unstaged per the rolling Hermes-supervision pattern (carried forward from cycle 35); pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged; 12+ pre-existing untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files at repo root preserved un-staged per the same pattern.

M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-37+ pre-`.CRT$XXC` discriminator work + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF.

Cycle 36 closes with the G0 discriminator answer landed; cycle-37+ pre-`.CRT$XXC` discriminator design (custom XBE-header callback that runs before nxdk's `_start`; OR static binary diff against a known-good nxdk XBE like `pipeline-smoke` or `mirror`; OR EEPROM-scratchpad write inside `xbed_self_witness_fire` before `MmAllocateContiguousMemoryEx`) is Hermes's call.

## 2026-05-23 (cycle 35 Path A.4 pre-main breadcrumb — bounded code slice CLOSED on `apple-silicon-performance`) — implementation + local-xemu smoke + Codex-validated; cycle-36 real-Xbox discriminator run is Hermes's call

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "implement the highest-value cycle-35+ follow-up to the cycle-34 F4 closeout: a pre-main breadcrumb mechanism in witness-only that can distinguish γ.0 (`main()` never entered AT ALL) from γ.1 (`XVideoSetMode` itself faulted before returning), preferring the least-invasive candidate that runs before `main()` while preserving the cycle-23 / cycle-29 / cycle-31 discriminator contracts." Cycle-36 real-Xbox deployment is NOT in this slice.

**Design — option (1) `.CRT$X*` static-init slot stamp CHOSEN.** Cycle 34 closure (`b5327d4d17`) recorded OUTCOME F4 = zero stripes + `witness.scan = D-cycle-27` + `witness.scan-self = count=0` → cycle-31 cycle-32 9-row discriminator table row F4 (γ.0 OR γ.1). The cycle-22 pre-main-crash hypothesis is FULLY CORROBORATED in its strongest form but cycle 34 cannot tell γ.0 from γ.1. Cycle 35 adds the cheapest mechanism that runs strictly before `main()`: two new function-pointer slots in nxdk's CRT-initializer sections that each fire `xbed_self_witness_fire(stage)` with brand-new pre-main stage codes (4, 5) before `main()` enters. The two slots are:

- `.CRT$XXC` (stage 4) — walked by `_PDCLIB_xbox_run_pre_initializers()` from `WinMainCRTStartup` AFTER `__security_init_cookie` + TLS setup + `_PDCLIB_xbox_libc_init` but BEFORE `thrd_create(main_wrapper)`. Earliest straight-line user-C point in process lifetime.
- `.CRT$XCU` (stage 5) — walked by `_PDCLIB_xbox_run_crt_initializers()` from `main_wrapper`'s thread AFTER the `.CRT$XI*` C-initializer pass succeeds, immediately BEFORE `main()`.

Both slots reuse the existing cycle-29 `xbed_self_witness_fire` shim (idempotent; first call allocates + zeroes + stamps magic/version + stamps stage byte; subsequent calls stamp + tick counter). After a fully successful run the WTNS page carries `reserved0=0xA4000003` (last stamp = in-`main()` POST_MARKER0) AND `reserved1=4` (2 pre-main + 2 in-`main()` WTNS fires).

**Why option (1) over the other cycle-34 F4 "Next"-column candidates** (full table in `witness-only/README.md` cycle-35 addendum):
1. `.CRT$X*` static-init slot stamp (CHOSEN). Documented + exercised on every nxdk-built XBE (`nxdk/lib/pdclib/platform/xbox/crt_initializers.c` registers `__xc_a[] / __xc_z[]` etc. as sentinel terminators using the same `__attribute__((section(".CRT$X*")))` mechanism). ZERO nxdk / linker / XBE-header changes. ZERO new shared-lib code (reuses cycle-29 `xbed_self_witness_fire`). Scope = `witness-only/main.c` only. Local xemu smoke confirms both slots fire BEFORE `main()` in the expected order with the WTNS counter ticking to 2 before main() enters.
2. Custom XBE-header callback (REJECTED). No documented "pre-CRT entry slot" in nxdk's `tools/cxbe/` XBE-header generator; implementation would have to modify nxdk itself, widening scope beyond `witness-only` + paired docs. Strictly earlier than `.CRT$XX*` but the γ.0 sub-windows it could uniquely distinguish (crash inside `_start` / `__security_init_cookie` / TLS-size computation) are vanishingly unlikely cycle-22 hang sites. Marginal value does not justify modifying nxdk.
3. Thinner alternative to `XVideoSetMode` via direct NV2A CRTC register writes (REJECTED). Does not address the γ.0-vs-γ.1 question — if `main()` does not enter at all, no in-`main()` code runs regardless. Also widens NV2A surface (cycle-23 lockstep + cycle-29 self-witness shim would have to coexist with direct register pokes), violating the cycle-34 prompt's "tightly scoped" guardrail. Filed for cycle-36+ only IF cycle 36 narrows the crash site to γ.1 AND a less-invasive paint mechanism becomes useful.

**Implementation surface.** ENTIRELY contained in `scripts/apple-silicon/xbe-tests/witness-only/`:

1. `main.c` modified. Cycle-35 head-comment addendum (~100 LOC) explaining design + ordering rationale + γ.0 / γ.1 sub-cases + why each candidate was chosen / rejected. Two new locally-defined stage constants (`WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XX = 4`, `WITNESS_ONLY_STAGE_PRE_MAIN_CRT_XC = 5`) — defined locally (NOT in `lib/xbed_a4_witness.h`) so the cycle-23 lockstep contract stays intact and the cycle-35 namespace cannot accidentally collide with future cycle-23 stage additions. Two static `_PVFV`-shaped runner functions (`witness_only_pre_main_crt_xx` / `witness_only_pre_main_crt_xc`) each emitting a host-log breadcrumb line + calling `xbed_self_witness_fire(stage)` + host-logging the returned phys. Two `__attribute__((section(".CRT$XXC"), used))` / `__attribute__((section(".CRT$XCU"), used))` static function-pointer slots that nxdk's CRT walker picks up between its `.CRT$XXA`/`XXZ` and `.CRT$XCA`/`XCZ` sentinels respectively. ZERO changes to the existing `main()` body: cycle-31 stripe paint sequence + cycle-25 host-log line + cycle-23 XCTR fires + cycle-29 in-`main()` WTNS fires + cycle-31 final `Sleep(2000)` + `HalReturnToFirmware(HalRebootRoutine)` all unchanged. Single new dependency is the `__attribute__((section(...), used))` linker hint syntax (clang/lld, already used in nxdk's own `crt_initializers.c`).

2. `README.md` cycle-35 addendum. Design rationale (option 1 chosen, options 2/3 rejected) with cycle-34 candidate comparison table; expanded WTNS counter encoding (count=0 / count=1 reserved1=1 / count=1 reserved1=2 / count=1 reserved1=3..4); new cycle-36 G-row discriminator table (G0 / G1 / G2 / G2' / G3 / G4 — G2' is the cycle-32 F4'-analogue for graceful XVideoSetMode FALSE) extending the cycle-32 F-rows for the γ.0 sub-windows; cycle-35 build artifacts + local validation evidence (xemu smoke output); cycle-36 deployment runbook (11-step sequence extending the cycle-32 runbook with the FTP `--overwrite` upload step because the cycle-35 XBE size matches cycle 31's).

3. `manifest.json` updated. Title extended ("+ cycle-35 .CRT$X* pre-main breadcrumb"); purpose paragraph extended with cycle-35 design summary; new `real-xbox/physical/cycle-36` expected_results section enumerating G0..G4 outcomes.

**Build.** `witness-only/bin/default.xbe` 155 648 B (unchanged from cycle 31's 155 648 B; the new ~200 bytes of pre-main breadcrumb code + 2 `.CRT$X*` slot pointers fit within the existing nxdk XBE page boundary). `witness-only.iso` 720 896 B (unchanged — same ISO sector boundary). Rebuilt cleanly via `eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && make`. Benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles; ZERO new warnings.

**Local validation (added value over cycle 31, which had no useful standalone-xemu signal).** Spawned `dist/xemu.app/Contents/MacOS/xemu` with a 12 s timeout against `witness-only.iso` with `XEMU_GUEST_LOG=1`. Stderr captured the following ordered host-log emission (full transcript in cycle-35 session evidence):

```
witness-only: .CRT$XXC pre-main breadcrumb running (cycle 35)
xbed_self_witness: enter stage=4
xbed_self_witness: allocated self-witness page phys=0x03fdf000 virt=0x83fdf000 magic='WTNS' version=1
xbed_self_witness: fired stage=4 at phys=0x03fdf000 virt=0x83fdf000 counter=1
witness-only: pre-main-xx fire returned phys=0x03fdf000
witness-only: .CRT$XCU pre-main breadcrumb running (cycle 35)
xbed_self_witness: enter stage=5
xbed_self_witness: fired stage=5 at phys=0x03fdf000 virt=0x83fdf000 counter=2
witness-only: pre-main-xc fire returned phys=0x03fdf000
witness-only: main() entered (cycle 25)
...
xbed_self_witness: fired stage=1 ... counter=3
xbed_self_witness: fired stage=3 ... counter=4
```

This proves: (i) `.CRT$XXC` slot fires; (ii) `.CRT$XCU` slot fires AFTER XXC but BEFORE `main()`; (iii) the cycle-29 self-witness shim's idempotent same-page reuse holds across both pre-main fires AND both in-`main()` fires (single WTNS page; counter ticks 1→2→3→4 with the correct stage bytes at each fire). Standalone xemu cannot exercise the real-Xbox-only failure modes (the agent isn't running, the cycle-23 XCTR scan correctly reports "no XCTR buffer found"), but local smoke is now meaningful for cycle 35 because it directly validates that the new `.CRT$X*` slot mechanism actually runs the slots in the expected order — a concern that does NOT have a cycle-31-style "no analog in xemu" excuse.

**Codex validation (rule #15, mandatory — non-trivial diff in xemu-fork apple-silicon scripts; main.c head comment + new helpers + slot declarations + paired docs = ~200 lines source + ~150 lines docs).**

- **Round 1 (changes mode) = MAJOR ISSUES** with 1 high + 1 medium + 1 low finding.
  - HIGH #1: G2 row in README.md + manifest.json + current-cycle.md overstated what `reserved1=2` proves — said "CORROBORATES γ.1" when in fact G2 is a *candidate* window where two sub-cases share the same shape (γ.0-sub "main() never entered after .CRT$XCU" AND γ.1 "main() entered and crashed inside paint(0) = XVideoSetMode"); cycle-35 evidence cannot distinguish them. **Adopted:** rewrote G2 in all four surfaces (README G-row table + manifest cycle-36 expected_results + current-cycle.md + handoff.md cycle-35 entry + main.c head-comment table) as a candidate-only window with explicit two-sub-case enumeration and a cycle-37 `.CRT$XCV`-slot follow-up to separate γ.0-sub from γ.1.
  - MEDIUM #2: cycle-36 G-row table did not explicitly map the cycle-32 F4'-analogue shape (no stripes + `reserved1=3..4` + `D-cycle-27`) — that shape would surface on real Xbox if `XVideoSetMode` returns FALSE gracefully (latching `xbed_breadcrumb_init` FAILED) and `main()` continues through the cycle-29 in-`main()` WTNS fires. **Adopted:** added explicit **G2'** row to the README G-row table + manifest cycle-36 expected_results + current-cycle.md G-row summary + main.c head-comment table covering this case (γ INVALIDATED via WTNS path; cycle 37 investigates AV-encoder rejection).
  - LOW #3: `handoff.md` cycle-35 entry + `decision-log.md` cycle-35 entry + `validation-status.md` had stale "TODO: round-by-round Codex disposition will be appended" / "marker will be written" wording while `claude-status.md` already said Codex was complete and the marker updated — canonical-state drift inside the cycle-35 closeout docs. **Adopted:** this Codex-validation block rewritten to reflect the actual landed disposition + marker write; `decision-log.md` + `validation-status.md` synced to match.
  - Open question from Codex: pre-main `MmAllocateContiguousMemoryEx` can return NULL from `.CRT$XXC` (allocation-failure edge case in `lib/xbed_self_witness.c:54-74`); the docs treat early allocation as guaranteed. **Adopted:** G0 row now explicitly notes this edge case in README + manifest + main.c head-comment table.
- **Round 2 (changes mode) = MINOR ISSUES** with 1 medium + 1 low finding (all 3 round-1 findings RESOLVED).
  - MEDIUM #4: `handoff-summary.md` collapsed `reserved1=3..4` directly into G3 and dropped the G2' (no-stripes graceful-XVideoSetMode-FALSE) sub-case. **Adopted:** `handoff-summary.md` G-row block now explicitly enumerates G2' alongside G2 + G3.
  - LOW #5: `handoff.md` line 74 still said the validation marker "will be written" while line 60 said it was written. **Adopted:** line 74 rewritten to "written at Codex round-2 LOOKS GOOD"; subsequently updated again at round 4 to "written at Codex round-4 LOOKS GOOD."
- **Round 3 (changes mode) = MINOR ISSUES** with 1 low (all round-2 findings RESOLVED).
  - LOW #6: abbreviated shorthand surfaces (README cycle-36 runbook step 10; `manifest.json` purpose; `handoff.md`; `decision-log.md`; `current-cycle.md`; `claude-status.md`) collapsed `reserved1=3..4 → G3` without distinguishing G2' (no-stripes) from G3 (stripes-visible). **Adopted:** README runbook step 10 expanded to a 5-row variant explicitly enumerating G0 / G1 / G2 / G2' / G3; `manifest.json` purpose adds explicit G2'; `handoff.md` + `decision-log.md` + `current-cycle.md` + `claude-status.md` shorthand updated to enumerate G2' alongside the other rows.
- **Round 4 (changes mode) = LOOKS GOOD** (all round-3 findings RESOLVED; no new findings).

Validation marker written at `.claude/state/codex-validate-last-run` on round-4 LOOKS GOOD.

**Hypothesis status after cycle 35 (implementation-only; cycle 36 will move).** Cycle-31's "cycle 31 ships the discriminator tool; cycle 32 will move hypotheses" pattern repeats: cycle 35 ships the γ.0-vs-γ.1 discriminator; cycle 36 will narrow the crash site. Specifically:

- Cycle-22 leading hypothesis ("pre-main crash"): STILL FULLY CORROBORATED (unchanged from cycle 34). Cycle-36 readback will refine which sub-window of pre-main (G0 vs G1 vs G2) the crash falls in.
- (γ.0) "execution never entered `main()` AT ALL": cycle 36 will narrow this into 3 sub-windows: G0 (`_start` / `__security_init_cookie` / TLS / `_PDCLIB_xbox_libc_init`); G1 (`thrd_create` failed OR `.CRT$XI*` faulted); G2 (between `.CRT$XCU` return and first in-`main()` WTNS fire; this is also the γ.1 candidate window).
- (γ.1) "`XVideoSetMode` itself faulted hard before returning": cycle 36 G2 row is a γ.1 **candidate** window if the WTNS counter lands at 2 (both pre-main slots ran but no in-`main()` WTNS fire landed). G2 narrows the crash site to between `.CRT$XCU` return and the first in-`main()` WTNS fire but CANNOT distinguish (γ.0 sub) `main()` never entered after `.CRT$XCU` from (γ.1) `main()` entered and crashed inside paint(0); a cycle-37 `.CRT$XCV` slot (between `.CRT$XCU` and `main()`'s first instruction) would separate the two.
- (α) "kseg0 scan can't find XCTR from non-agent context": STILL LIVE but DEPRIORITIZED (γ remains LEADING regardless of cycle-36 outcome).
- (β) "scan finds XCTR but write faults silently": same as (α).

**Cycle-36 candidate scope (NOT promoted by this session — Hermes's call).** FTP-deploy cycle-35 `witness-only/bin/default.xbe` (155 648 B; SAME path `/E/Apps/witness-only/default.xbe`; cycle-29 oracle-agent stays in place from cycle 32 / 34) USING `--overwrite` because the cycle-35 size matches cycle 31's exactly and the FTP uploader's default size-only diff would otherwise skip the upload (cycle-30 methodology lesson); ARM composite-capture leg via `scripts/apple-silicon/composite-record.sh cycle36-witness-only-pre-main` BEFORE issuing `runxbe`; run the cycle-32 canonical sequence (baseline both scans → reboot to dashboard → FTP-upload with `--overwrite` → relaunch agent → baseline both scans → composite-capture ARM → runxbe → poll FTP/21 + 9001 + ICMP → composite-capture STOP → ensure-agent → final both scans). The KEY new signal at step 10 is `witness.scan-self`'s `reserved1` counter, which now counts WTNS fires that landed (with the pre-main fires the count goes 0 → 1 → 2 → 3 → 4 instead of cycle 29's 0 → 1 → 2). Classification per the G-row table.

**Out of scope (kept bounded for cycle 35).** ZERO xemu-fork host source touched. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact). ZERO `lib/lib.mk` touched (cycle-29 opt-in policy intact). ZERO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 witness.scan-self verb intact). ZERO `xbed_runtime.{c,h}` touched. ZERO image-blit touched. ZERO `nxdk/` source touched (the `.CRT$X*` mechanism is consumed from nxdk's stable CRT API, NOT modified). ZERO XBE rebuilds beyond `witness-only` itself. ZERO retail-title / §G.5 / RT-as-texture / second-wave-XBE work. ZERO flag default flips. ZERO PushNotification — bounded implementation slice, not blocker / milestone. ZERO composite-preflight / ffmpeg TCC gap fix (filed as cycle-34 secondary finding for separate cycle; cycle-34 prompt explicitly constrained this slice). ZERO touch of pre-existing tracked drift in `capture-composite-reference.sh` + 3 `retail-*.py` scripts AND ZERO touch of pre-existing untracked `composite_preflight.py` (preserved per cycle-34 prompt guardrail). 12 pre-existing untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files at repo root preserved un-staged per the rolling Hermes-supervision pattern.

**Files touched (cycle 35).** `scripts/apple-silicon/xbe-tests/witness-only/main.c` (head-comment addendum + 2 new helper functions + 2 `.CRT$X*` slot declarations); `scripts/apple-silicon/xbe-tests/witness-only/README.md` (cycle-35 addendum + cycle-36 G-row discriminator table + cycle-36 deployment runbook + cross-references to nxdk CRT source); `scripts/apple-silicon/xbe-tests/witness-only/manifest.json` (title + purpose + new `real-xbox/physical/cycle-36` expected_results); `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` + `witness-only.iso` (rebuilt artifacts); `docs/apple-silicon/handoff.md` (this entry on top; cycle-34 entry preserved unchanged below); `docs/apple-silicon/decision-log.md` (cycle-35 entry on top; cycle-34 preserved unchanged); orchestration-state quartet closure pass (current-cycle.md + claude-status.md + validation-status.md + handoff-summary.md). Validation marker at `.claude/state/codex-validate-last-run` written at Codex round-4 LOOKS GOOD.

## 2026-05-23 (cycle 34 cycle-32 redo on real Xbox vs cycle-31 visual-breadcrumb build — **CLOSED on `apple-silicon-performance`**; **OUTCOME F4** = zero stripes visible across 22 NTSC-correct composite snapshots over t+0.07s..t+24.17s after `runxbe` + `witness.scan = D-cycle-27 (count=1 phys=0x03eb3000 reserved0=0 reserved1=0)` + `witness.scan-self = count=0` per `witness-only/README.md` cycle-32 9-row discriminator table; γ.0 OR γ.1 → **cycle-22 pre-main-crash hypothesis FULLY CORROBORATED in its strongest form**; cycle-32 redo finally moved from F8 (no-capture-procedural-failure recorded at cycle-32 closure `ac515383bb`) to F4 (genuine discriminator answer). Hardware-side blocker cleared by Josh's physical Xbox restart + Mac Studio QuickTime composite-capture verification reported at session start (Xbox at dashboard with `ping=true, ftp=true, agent=false`; composite-preflight `--mode auto` xemu-capture detector returned `status=ok` in 1.671 s with a real 720x480 NTSC dashboard frame carrying 652 unique colors). Canonical cycle-31 cycle-32 deployment runbook (`witness-only/README.md` lines 398-469) executed in 9 numbered evidence steps with one bounded substitution: `composite-record.sh` (ffmpeg-AVFoundation-based) cannot run in this bash session because ffmpeg is unavailable on the inherited PATH (PATH=/usr/bin:/bin:/usr/sbin:/sbin:/Users/jbbrack03/.claude/plugins/cache/...; brew ffmpeg lives at /opt/homebrew/bin/ffmpeg) AND because ffmpeg from this terminal silent-stalls AVFoundation device open even when explicit `FFMPEG=/opt/homebrew/bin/ffmpeg` is set (cycle-33 preflight `--mode ffmpeg` correctly classifies as `no_signal` after 8.93 s; xemu-capture against the same device at the same time returns `status=ok` in <2 s — hypothesized TCC camera-access permission inheritance gap inside the bash subprocess launched via `/Users/jbbrack03/.local/bin/claude-max-bypass` since xemu-capture has its own `com.xemu-macos.capture` bundle TCC grant). Substituted the ffmpeg leg with an xemu-capture snapshot burst over the witness-only execution window: 22 frames at ~1.2 s cadence over 25 s, each via `scripts/apple-silicon/bin/xemu-capture --wait-s 4 snapshot USB2 --out <path> --warmup-frames 1 --timeout 3 --width 720 --height 480`. ZERO source changes — only different invocations of already-shipped tools — and preserves the discriminator semantics because the cycle-31 final `Sleep(2000)` design holds the deepest stable paint state for ~60 frames at 30 fps which is well above the burst's 1.2 s cadence. Sequence executed (9 logged steps under `benchmark-runs/cycle34-cycle32-redo-real-xbox-witness-only-visual-20260523T203702Z/`, gitignored per project convention): 00 reachability + composite preflight (`status=ok` 1.671 s); 01 ftp-list confirms cycle-31 witness-only (155 648 B) + cycle-29 oracle-agent (417 792 B) still resident at expected paths; 02 ensure-agent SITE EXEC OK; 03 baseline both scans MET (`witness.scan = count=1 phys=0x03eb3000 reserved0=0 reserved1=0 mapped_pages_seen=419` AND `witness.scan-self = count=0`); 04 composite-record.sh attempted, aborted at preflight gate (`status=no_backend` because ffmpeg missing on bash PATH) — switched to xemu-capture burst; 05 first `runxbe` issued at t=1779568884.10 (2026-05-23T20:41:24Z); FIRST burst into `snapshots/` mistakenly used xemu-capture's 720x576 PAL default dimensions because the bare `snapshot DEVICE --out PATH` invocation does NOT pass `--width`/`--height` — every PAL frame returned RGB(0,0,0) with all 19 frames having identical SHA `ef23436c5368...` (Claude-side procedural error; NOT XBE evidence; PRESERVED for audit and as the secondary cycle-33-preflight-gap finding); 06 first runxbe completed (`ping=true, ftp=true, agent=false` afterward); 07 post-first-runxbe scans = `witness.scan = D-cycle-27` AND `witness.scan-self = count=0` (already enough to land F4 if no-stripe finding holds); 08 second `runxbe` issued at t=1779568974 with format mismatch corrected (`--width 720 --height 480`); SECOND burst into `snapshots-runxbe2-ntsc/` shows pre-runxbe snap_00 with REAL dashboard signal (25 357 unique colors, max=(255,255,255)) AND 22 post-runxbe snaps over t+0.07s..t+24.17s ALL RGB(0,0,0) pure-black unique=1 (zero stripes visible across the entire window where the cycle-31 design says paint(0) RED through paint(4) BLUE should be observable — a single-stripe-only landing or any-band-of-color landing should have shown given the final `Sleep(2000)` settle); 09 final post-burst-15s status = dashboard FTP returned, ensure-agent OK, final `witness.scan = D-cycle-27` AND `witness.scan-self = count=0`. Classification reads `(deepest visible stripe = none, witness.scan-self count = 0, witness.scan = D-cycle-27)` against the 9-row discriminator table: F1/F2/F3/F5/F6 require >=1 visible stripe (DOES NOT MATCH); F4' requires `witness.scan-self count>=1` from cycle-29 self-witness fires running after the graceful `XVideoSetMode` FALSE-return latches FAILED — the observed `count=0` REFUTES the continuation path and therefore eliminates F4'; F7 requires partial intermediate band-gap shape (not observed); F8 ("no composite capture available") DOES NOT APPLY because the capture leg ran end-to-end and the pre-runxbe dashboard snap proves the capture pipeline healthy. **Only F4 matches all three axes simultaneously.** F4 maps to γ.0 ("execution never entered `main()` AT ALL") OR γ.1 ("`XVideoSetMode` itself faulted hard before returning") per the cycle-31 cycle-32 discriminator table row 4. Hypothesis state: (γ) "main() never reaches the fire calls" promoted from LEADING (cycle 30 E2 / cycle 32 F8 inconclusive on stripes) to LEADING at STRONGEST FORM; cycle-22 pre-main-crash hypothesis promoted from RE-STRENGTHENED-toward-leading-but-not-fully-corroborated to FULLY CORROBORATED in its strongest form; (α) "kseg0 scan can't find XCTR from non-agent context" + (β) "scan finds XCTR but write faults silently" BOTH STILL LIVE but FURTHER DEPRIORITIZED (any successor design must still account for them, but neither is the LEADING cause); hypothesis #5 catastrophic-hang sense UNCHANGED (cycle 34 reproduced clean dashboard recovery, not a 928 s+ hang). Reproducibility wins: kernel-pool deterministic `phys=0x03eb3000` REPRODUCED across cycle-34 baseline + post-first-runxbe + post-second-runxbe = 3 readbacks this session; combined with prior cycles ≥18 consecutive observations across cycles 26 / 28 / 30 / 32 / 34 now across TWO physical power sessions (cycle 34 fresh post Josh's restart — confirms the kernel pool's reuse pattern survives across physical power cycles, stronger than prior single-power-session claim); `mapped_pages_seen=419` REPRODUCED at every readback (matches cycles 28 / 30 / 32 exactly). Secondary findings filed for cycle-35+ consideration (NOT cycle-34 fix scope): (i) cycle-33 preflight `--mode auto` does not protect `composite-record.sh` (ffmpeg-based) from the TCC-permission-inherited silent-ffmpeg-stall failure mode because preflight stops at the first detector that succeeds (xemu-capture path) and never tries ffmpeg in this asymmetric case — cycle-35+ could add `--require-both-detectors` OR have `composite-record.sh` always run an ffmpeg preflight regardless of xemu-capture outcome; (ii) `scripts/apple-silicon/bin/xemu-capture snapshot USB2 --out ...` without explicit `--width`/`--height` silently defaults to 720x576 PAL and returns pure-zero pixels on the NTSC composite signal while still reporting `status=ok` (cycle-33 preflight is unaffected because it passes `--width 720 --height 480` explicitly; the `witness-only/README.md` line-180 step-11 example invocation does NOT pass dimensions and could mislead operators into a false F4-shape reading; cycle-35+ could either default xemu-capture snapshot dimensions to NTSC when device matches MS2109, OR update the example invocation in README.md to include dimensions). ZERO source/script code edits this cycle; ZERO XBE rebuilds; ZERO host source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact); ZERO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact); ZERO `xbed_runtime.{c,h}` touched; ZERO image-blit touched; ZERO `witness-only/main.c` touched (cycle-31 source intact); ZERO `tools/xemu-capture/` source touched; ZERO `scripts/apple-silicon/composite-record.sh` source touched (cycle-33 implementation intact); ZERO `scripts/apple-silicon/composite-preflight.sh` source touched (cycle-33 implementation intact); Codex SKIPPED under rule #15 doc-only / run-only carve-out (same path as cycles 26 / 28 / 30 / 32) — the cycle-31 binary observed this cycle is bit-identical to the cycle-31 build that passed 3-round Codex green at cycle-31 closure (`41f350c174`); cycle-31 marker at `.claude/state/codex-validate-last-run` remains the relevant marker for the deployed artifact. Pre-existing tracked-but-uncommitted modifications to `scripts/apple-silicon/capture-composite-reference.sh` + `retail-gameplay-oracle.py` + `retail-oracle-workflow.py` + `retail-title-automation-proof.py` PRESERVED unstaged per the cycle-34 prompt guardrail. Pre-existing untracked `scripts/apple-silicon/composite_preflight.py` PRESERVED unstaged. 11 pre-existing untracked `.hermes_*.txt` + `.hermes_launch_*.sh` files at repo root preserved un-staged per the rolling Hermes-supervision pattern (consistent with cycles 26 / 27 / 28 / 29 / 30 / 31 / 32 / 33 handling). M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-35+ pre-main breadcrumb implementation + downstream cycles), §G.5, RT-as-texture; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF. Cycle-35+ pre-main breadcrumb design is Hermes's call: candidates per cycle-31 cycle-32 discriminator table F4 "Next" column include (1) nxdk `.CRT$XCU` static-init slot stamp that runs after PE-load but before `main()`; (2) custom XBE-header callback (kernel-controlled entry slot, runs before `.CRT$*`); (3) thinner alternative to `XVideoSetMode` (e.g. direct NV2A CRTC register writes that bypass the kernel display init path). Cycle 33 entry preserved unchanged below. Originally tagged for cycle 33 composite-capture fail-fast preflight SHIPPED on `apple-silicon-performance`; new `scripts/apple-silicon/composite-preflight.sh` + default-on integration into `scripts/apple-silicon/composite-record.sh` so the cycle-32 OUTCOME F8 silent-stall failure mode aborts in ~8 s instead of ~93 s; no source/host-renderer changes; paired automation.md + flags-bench.md doc updates; Codex validation per rule #15 closed at 6 rounds (round 1 MINOR ISSUES 4 findings, round 2 MINOR ISSUES 2 findings, round 3 MINOR ISSUES 1 finding, round 4 MINOR ISSUES 1 finding, round 5 MINOR ISSUES 1 finding, round 6 LOOKS GOOD); cycle-32 redo remains Hermes's call after physical-side composite-cable / capture-input verification; cycle-32 entry preserved unchanged below). Originally tagged for cycle 32 Path A.4 real-Xbox deployment of the cycle-31 visual-breadcrumb build — **CLOSED on `apple-silicon-performance`**; **OUTCOME F8** = cycle-32 procedural failure because the MS2109 composite-capture leg never recorded a frame; post-run witness-side two-tuple `(witness.scan, witness.scan-self) = (D-cycle-27, count=0)` functionally IDENTICAL to cycle 30's E2; visible-stripe count was the SOLE remaining discriminator among F2/F3/F4/F5 and is missing; F4' (graceful XVideoSetMode FALSE return) RULED OUT by the WTNS `count=0` readback; cycle-32 redo is Hermes's call after physical-side composite-cable / capture-input verification; hypothesis state UNCHANGED from cycle 30; cycle-22 leading hypothesis STILL RE-STRENGTHENED, not yet fully corroborated). Originally tagged for cycle 31 (preserved unchanged below). Cycle 31 modified `scripts/apple-silicon/xbe-tests/witness-only/main.c` to add a pbkit-free `XVideoSetMode(640, 480, 32, REFRESH_DEFAULT)` + 5 distinguishable horizontal-stripe paint sites (RED / ORANGE / YELLOW / GREEN / BLUE; ARGB8888 0xFFFF0000 / 0xFFFF7F00 / 0xFFFFFF00 / 0xFF00FF00 / 0xFF0000FF) interleaved between cycle-25/29 checkpoints in `main()`: paint(0) BEFORE the cycle-25 host-log line (the FIRST observable side effect of `main()`), paint(1) after cycle-23 fire1 return, paint(2) after cycle-23 fire2 return, paint(3) after cycle-29 self-fire1 return, paint(4) after cycle-29 self-fire2 return. Pre-reboot Sleep extended from cycle 25's 500 ms to 2 000 ms so a composite-capture stream at ~30 fps records ≥60 frames of the deepest-painted state. Helpers (`xbed_breadcrumb_init` 3-state machine + `xbed_breadcrumb_paint`) live entirely in `witness-only/main.c` — NO shared-lib changes (no edits to `lib/xbed_a4_witness.{c,h}` / `lib/xbed_self_witness.{c,h}` / `lib/lib.mk`); NO oracle-agent changes; NO `xbed_runtime.{c,h}` changes; NO image-blit changes. Single new include is `<hal/video.h>` (+`<string.h>` for `memset`). The XVideoSetMode call exercises the same kernel paths `lib/xbed_runtime.c:43-60`'s `xbed_init` already uses for every diag XBE that draws anything (AvGetSavedDataAddress + MmAllocateContiguousMemoryEx with PAGE_WRITECOMBINE + AvSetDisplayMode + XVideoSetGammaRamp); pbkit is NOT called — that is the cycle-31 design point. Build: `witness-only/bin/default.xbe` 155 648 B (+4 096 B from cycle 29's 151 552 B; the new code fits in one nxdk XBE page boundary); `witness-only.iso` 720 896 B (unchanged — same ISO sector boundary). Codex validation 3 rounds per rule #15 (non-trivial diff ~250 lines C source + paired docs): Round 1 = MAJOR ISSUES with 3 high + 1 low findings (HIGH #1 canonical docs not synced; HIGH #2 F1 outcome row contradictory — said "both mechanisms landed" but `witness.scan` still D-cycle-27 means XCTR did NOT land; HIGH #3 init retried XVideoSetMode after failure, broadening the risk surface beyond "single call" claim; LOW #4 file banner still said "NO XVideoSetMode"). All 4 adopted: canonical docs actually synced; F1 wording rewritten to "WTNS landed; XCTR still D-cycle-27" with F6 reserved for "both mechanisms succeeded"; `xbed_breadcrumb_init` rewritten as 3-state machine (UNTRIED / OK / FAILED) so a graceful FALSE return latches FAILED and `paint(1..4)` cannot re-enter the kernel display init path — concentrating the risk surface strictly in the single XVideoSetMode call invoked from paint(0); banner header rewritten. Round 2 = MAJOR ISSUES with 1 new HIGH + 1 new LOW (all 4 round-1 findings RESOLVED): new HIGH #5 F4 was self-contradictory — claimed it included "graceful XVideoSetMode FALSE return" alongside (γ.0)/(γ.1) AND claimed "cycle-22 pre-main FULLY CORROBORATED" (a graceful FALSE means main() DID execute past its first instruction, so it does NOT corroborate "pre-main" anything); new LOW #6 stripe-map prose stale ("All 5 stripes = E1 shape"). Both adopted: F4 split into F4 (only γ.0 / γ.1) and a new F4' row (graceful XVideoSetMode FALSE return = main() executed past paint(0) which became no-op when latch FAILED + cycle-29 self-witness fires still run + stamp WTNS page = γ INVALIDATED via WTNS path); stripe-map prose rewritten to reference the F1 / F5 / F6 disambiguation; discriminator-table row count went from 8 to 9. Round 3 = LOOKS GOOD (round-2 findings RESOLVED; no new findings). Validation marker written at `.claude/state/codex-validate-last-run`. **Cycle 31 does NOT include a real-Xbox run — the bounded assignment was "ship the option-(d) infrastructure so Hermes can later schedule the cycle-32 real-Xbox discriminator run from durable docs." Cycle 32 (Hermes-scheduled) will FTP-deploy the cycle-31 `witness-only/bin/default.xbe` and run the cycle-30 canonical sequence WITH the composite-capture leg ARMED via `scripts/apple-silicon/composite-record.sh` (MS2109 USB stick + ffmpeg AVFoundation) BEFORE issuing `runxbe`.** Cycle-32 hard preconditions in addition to cycle 30's: composite-capture leg ARMED + MS2109 USB stick recognized via `tools/xemu-capture list`. Cycle-32 expected outcomes (full 8-row F1..F8 table in `witness-only/README.md` + cycle-32 entry in `manifest.json`): F1 (all 5 stripes + WTNS success + XCTR D-cycle-27) — γ INVALIDATED, α/β remain live on XCTR side → cycle 33 option (b); F2 (stripes 0..2 only + count=0) — main() ran cycle-23 fires but crashed before self-fire1 site → instrument the gap; F3 (stripe 0 only + count=0) — strong cycle-23 mechanism failure on real Xbox in this minimal XBE → redesign; F4 (no stripes + count=0) — γ.0 OR γ.1 — cycle-22 pre-main hypothesis FULLY CORROBORATED → pre-main breadcrumbs; F4' (no stripes + count=1 WTNS success) — graceful XVideoSetMode FALSE return; main() DID execute → γ INVALIDATED (Codex round-2 high adopted); F5 (all 5 stripes + count=0) — exotic, cache-attribute divergence → re-elevate option (b); F6 (all 5 stripes + WTNS success + XCTR A1/A2 success) — full success across BOTH mechanisms → consider declaring discriminator track CLOSED; F7 (partial intermediate band missing) — re-run; F8 (no composite capture) — procedural failure, re-run. ZERO xemu-fork host source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO `lib/xbed_self_witness.{c,h}` touched; ZERO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 witness.scan-self verb intact); ZERO image-blit touched. Three pre-existing untracked `.hermes_cycle*.txt` prompt files at repo root preserved un-staged per the rolling Hermes-supervision pattern. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-32 real-Xbox run with the cycle-31 visual-breadcrumb build), §G.5, RT-as-texture. Cycle 30 entry preserved unchanged below. Originally tagged for cycle 30 Path A.4 real-Xbox deployment of the cycle-29 self-allocated-witness build — **CLOSED on `apple-silicon-performance`; OUTCOME E2**). Cycle 30 FTP-deployed the cycle-29 `oracle-agent/bin/default.xbe` (forced re-upload via `--overwrite` because the same-size cycle-27 binary already on disk was skipped by `xbox-ftp-upload.py`'s default size-only diff) + cycle-29 `witness-only/bin/default.xbe` (151 552 B vs cycle-25's 147 456 B — size mismatch triggered overwrite without `--overwrite`). Canonical cycle-26/28-style sequence extended with `witness.scan-self` queries at baseline + post-run. **Hard preconditions MET:** baseline `witness.scan count=1 buf.0 phys=0x03eb3000 live=1 reserved0=0 reserved1=0 mapped_pages_seen=419` AND baseline `witness.scan-self count=0 mapped_pages_seen=419`. **Final readbacks: `witness.scan count=1 phys=0x03eb3000 reserved0=0 reserved1=0` (D-cycle-27, identical to cycle 28) AND `witness.scan-self count=0` (no WTNS page allocated/found anywhere in scanned kseg0).** This is outcome **E2** per the cycle-30 discriminator table (`witness-only/README.md` row "D-cycle-27 | `count=0`"): **(γ) "main() never reaches the fire calls" is now LEADING**; the cycle-22 "pre-main crash" hypothesis is **RE-STRENGTHENED** from "weakened" toward "leading." Because the cycle-29 self-witness fires execute AFTER the cycle-23 XCTR fires in `witness-only/main.c` (Codex round-1 high finding #1 ordering), E2 also rules out the sub-case "main() reached cycle-23 fire #2 but crashed before the cycle-29 fires" — both fire pairs (cycle-23 XCTR and cycle-29 WTNS) are equally invisible, and the cycle-29 path has no XCTR-side dependency (it allocates its own page with `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory` using the same primitive `oracle-agent/controller.c::s_allocate_fresh` uses successfully). **(α) "kseg0 scan can't find XCTR from non-agent context" and (β) "scan finds XCTR but write faults silently" both remain LIVE on the XCTR-side D-cycle-27 readback but are DEPRIORITIZED — they are XCTR-specific failure causes, moot for explaining the WTNS `count=0` (γ-leading) observation.** New timing observation: dashboard FTP recovery at **t+39s** (a NEW shape) vs cycle 26's 70.17 s + cycle 28's 70 s (the previously reproduced witness-only recovery shape). The 39 s shape sits between mirror control (~36 s) and the 70 s prior witness-only shape; possible readings (single-sample, not discriminated): a faster early crash that bypasses the witness lib `.text` entirely, variance, or shifted crash site from the +4 096 B `xbed_self_witness.c` `.text` linkage. Recorded as a tracked open observation, NOT load-bearing for the E2 conclusion (the WTNS `count=0` readback is). Methodology note (preserved for future cycles): the cycle-30 dashboard-recovery poll initially used the same anonymous `curl --max-time 2 ftp://...` probe as cycle 26/28; the Xbox responded `530` (login-required) immediately after reboot (= FTP service alive, anonymous denied), which the unauthenticated probe scored as CLOSED. **Switched to authenticated `curl -u xbox:xbox ...` which returns `226` on a successful LIST; that is the new ground-truth probe.** Reproducibility wins this cycle: kernel-pool deterministic phys=0x03eb3000 REPRODUCED across cycle-27 agent re-launch + cycle-29 first launch + cycle-29 post-chainload launch = **≥9 consecutive observations in same physical power session** (cycle 26: 3 + cycle 28: 3 + cycle 30: 3); `mapped_pages_seen=419` REPRODUCED at baseline + post-launch + post-run (5th, 6th, 7th observations across cycles 26/28/30). **ZERO source/script code edits this cycle**, ZERO XBE rebuilds, ZERO host-source touched — run-only / doc-only slice; rule #15 carve-out applies (same path as cycles 26 / 28). Evidence preserved on disk: `benchmark-runs/cycle30-real-xbox-witness-only-self-20260523T103624Z/{00..09-*.log, SUMMARY.md}` (gitignored per project convention). **Cycle 31 candidate scope (NOT promoted by this session — Hermes's call):** option (d) — on-screen visual breadcrumb captured via composite capture during `witness-only` execution. Option (b) (agent-side prior-phys dump + read-only kseg0 dump verb) was the α-vs-β discriminator; cycle 30 makes α-vs-β moot for now, so option (b) is DEMOTED. Option (d) is the cycle-31 leading candidate. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF. M15 overall still **NOT MET** pending §H.6 default-on shape (now blocked on cycle-31 option (d) on-screen breadcrumb discriminator), §G.5, RT-as-texture. Two pre-existing untracked `.hermes_cycle*.txt` prompt files at repo root preserved un-staged (consistent with cycles 26 / 27 / 28 / 29 handling). Cycle 29 entry preserved unchanged below. Originally tagged for cycle 29 Path A.4 option (c) self-allocated witness — **IMPLEMENTATION + CODEX-VALIDATED BOUNDED CODE SLICE CLOSED on `apple-silicon-performance`**; cycle-30 real-Xbox discriminator run is Hermes's call (now executed this session, OUTCOME E2 above). The cycle-28 closure (commit `c77b509149`) collapsed the cycle-26 ambiguity to "no A.4 stamp landed on the agent's XCTR buffer" with three live causes: (α) `xbed_a4_witness::a4_candidate_ok` kseg0 scan doesn't find the agent's XCTR buffer from a non-agent process context, (β) scan finds it but the write faults silently, (γ) `witness-only`'s `main()` never reaches the fire calls (consistent with cycle-22 leading hypothesis). Cycle 29 ships **option (c)** from the cycle-27 closure catalog: a new shared diag-XBE lib `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.{h,c}` that on its first call allocates the diag XBE's OWN persistent contiguous page via `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory` and stamps a unique `'WTNS'` magic (0x534E5457) + version 1 + reserved0 = `(0xA4 << 24) | stage` + reserved1 = call counter, plus a new read-only oracle-agent verb `witness.scan-self` that enumerates `'WTNS'` pages in kseg0 [0x80010000, 0x84000000] using the same `MmGetPhysicalAddress`-gated safety pattern as `cmd_witness_scan`. `witness-only/main.c` calls `xbed_self_witness_fire(MAIN_ENTERED)` + `xbed_self_witness_fire(POST_MARKER0)` AFTER the existing cycle-23 XCTR fires (Codex round-1 high finding #1 adopted — ordering keeps the cycle-23 path bit-identical to cycle 25 up to and including the second cycle-23 fire, so the cycle-30 XCTR readback is properly comparable to cycle 28's D-cycle-27). Cycle 29 is therefore additive but NOT a strict superset of cycle 25: the post-cycle-23-fires-to-reboot window gains new kernel-allocator activity. The shim is opted in only by `witness-only/Makefile` (Codex round-1 low finding #3 adopted — NOT added to `lib.mk` default SRCS, so the rest of the diag-XBE corpus is unaffected). Agent registration: new entry `{ "witness.scan-self", cmd_witness_scan_self }` in `oracle-agent/main.c`'s `s_cmds[]` + new `cmd_help` line. **Discriminator scope (Codex round-1 high finding #2 adopted): cycle 29 is positioned narrowly as a (γ)-only discriminator.** A cycle-30 `witness.scan-self` hit INVALIDATES (γ) "main() never reached" but (α) AND (β) BOTH REMAIN LIVE — the cycle-29 self-witness stamps a SELF-OWNED page, not the agent's XCTR page; it does not exercise the failing write into the agent's XCTR buffer, so it cannot distinguish "scan can't find XCTR" (α) from "scan finds XCTR but write faults silently" (β). Breaking α-vs-β requires cycle 31+ option (b) (agent-side prior-phys dump + read-only kseg0 dump verb). All docs (commands.c body comment, manifest.json, witness-only README.md, xbed_self_witness.h, main.c head comment) consistently encode the narrowed scope. Build: `oracle-agent/bin/default.xbe` 417 792 B rebuilt (size unchanged from cycle 28 — new verb fits in existing XBE page boundary; benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles); `witness-only/bin/default.xbe` 151 552 B rebuilt (+4 096 B = +1 page from cycle 25's 147 456 B — the new `xbed_self_witness.c` linked-in code). Both ISOs reproduce cleanly (`oracle-agent.iso` 983 040 B unchanged; `witness-only.iso` 720 896 B unchanged — same ISO sector boundary). Codex validation 4 rounds per rule #15 (non-trivial diff = ~340 lines across the new shim + agent verb + witness-only main.c + docs): Round 1 = MAJOR ISSUES with 3 findings (HIGH #1 "strictly additive" claim was misleading because pre-cycle-23 self-witness activity confounds the cycle-25 baseline; HIGH #2 discriminator over-claimed by saying WTNS hit makes α leading when in fact β remains live too; LOW #3 lib.mk pulled the new SRCS into every diag XBE). All 3 adopted: ordering swapped so cycle-29 fires run AFTER cycle-23 fires; all five doc surfaces rewritten to position cycle 29 as a (γ)-only discriminator with α+β remaining live; new SRCS opted in only by witness-only/Makefile. Round 2 = MINOR ISSUES (round-1 HIGH #1 PARTIAL — xbed_self_witness.h still said "strict superset"; round-1 LOW #2 — operator-facing tables missed the partial-success shapes the reader tolerates: `0xA4000001/1` first-self-fire-only, `(0,0)` allocated-but-not-stamped, `count>=2` accumulated orphans; round-1 LOW #3 RESOLVED; round-1 HIGH #2 RESOLVED). Both adopted: header doc text harmonized with the rest; README + manifest cycle-30 tables expanded to 7 rows (E1/E1'/E1''/E2/E3/E4/E5). Round 3 = MINOR ISSUES (round-2 LOW #1 RESOLVED; round-2 LOW #2 PARTIAL — the tolerated `reserved0=0xA4000003 reserved1=1` second-self-fire-only edge case still wasn't called out). Adopted: README + manifest now explicitly say the table is "representative not exhaustive" and document the tolerated shape. Round 4 = **LOOKS GOOD** with round-3 LOW RESOLVED and no new findings. Validation marker written at `.claude/state/codex-validate-last-run`. **Cycle 29 does NOT include a real-Xbox run — the bounded assignment was "ship the self-allocated witness infrastructure," not "run it." Cycle 30 (Hermes-scheduled) will FTP-deploy both rebuilt XBEs and execute the canonical cycle-26-style sequence extended with `witness.scan-self` queries at baseline + post-run.** Cycle-30 hard preconditions: baseline `witness.scan count=1 live=1 reserved0=0` AND baseline `witness.scan-self count=0` (power-cycle Xbox if multiple persistent pages of either kind pre-exist). Cycle-30 expected outcomes (full table in `witness-only/README.md` + `manifest.json`): E1 (full success, D-cycle-27 + `WTNS count=1 0xA4000003/2`) — γ INVALIDATED, α+β remain live → cycle 31 option (b); E1' (partial, first-self-fire-only) — γ INVALIDATED in the partial sense; same α-vs-β follow-up; E1'' (allocated-but-not-stamped) — very unlikely, instrument writer in cycle 31; E2 (D-cycle-27 + `WTNS count=0`) — γ leading; cycle 31+ promotes option (d) on-screen breadcrumb; E3 (A1/A2 success + WTNS success) — both mechanisms work; full re-validation required; E4 (cycle-24-style hang) — cycle-29 ordering or own-page allocation is a new failure mode; redesign required; E5 (`WTNS count>=2`) — accumulated orphans; power-cycle between attempts. ZERO xemu-fork host source touched; ZERO image-blit source touched; ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact); ZERO cycle-22/preserve-branch source touched (cycle-27 preserve gate intact). Two pre-existing untracked `.hermes_cycle*.txt` prompt files at repo root preserved un-staged. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-30 real-Xbox run with the cycle-29 self-witness build), §G.5, RT-as-texture. Cycle 28 entry preserved unchanged below. Originally tagged for cycle 28 Path A.4 real-Xbox deployment of cycle-27 preserve-branch oracle-agent vs cycle-25 witness-only — **OUTCOME D-cycle-27**. Cycle-27 option (a) DEMONSTRATED INSUFFICIENT to break the cycle-26 stamp-vs-no-stamp ambiguity to A1/A2 — the post-run `witness.scan` after the cycle-27 preserve-branch agent re-allocated the deterministic kernel-pool buffer phys=0x03eb3000 showed `count=1 live=1 reserved0=0x00000000 reserved1=0x00000000`, meaning no A.4-tagged stamp existed on the page at re-allocation time. The cycle-27 preserve gate's strict predicate (`(reserved0>>24)==0xA4 AND 1<=reserved1<=4096`) would have preserved any header `xbed_a4_witness.c` writes (MAIN_ENTERED → 0xA4000001/1; POST_MARKER0 → 0xA4000003/2); a `(0,0)` readback unambiguously means the stamp never landed, not that the agent wiped it. This RESOLVES the cycle-26 "stamp landed but agent wiped it" hypothesis as **INVALIDATED**; the cycle-26 reading collapses to "stamp never landed." LEAVES OPEN three live causes: (α) `xbed_a4_witness::a4_candidate_ok` kseg0 scan doesn't find agent's XCTR buffer from non-agent process context, (β) scan finds it but write faults silently (PAT/WC/WB attribute divergence), (γ) witness-only's main() never reaches the fire calls (consistent with cycle-22 leading hypothesis). Cycle-22 leading hypothesis status: still **WEAKENED** — cycle 28 evidence is equally consistent with "main() runs but fires no-op silently" and "main() never reached." Hypothesis #5 status: catastrophic-hang sense remains **PARTIALLY INVALIDATED**; subtler "kseg0-scan witness mechanism silently no-ops from non-agent context" is now consistent with cycle-28 evidence but indistinguishable from (γ). Reproducibility wins this cycle: (i) 70 s chainload→dashboard-ready for witness-only reproduced TWICE (cycle 26 ~70.17 s + cycle 28 ~70 s — real Xbox behavior for this XBE), (ii) kernel-pool deterministic reuse of phys=0x03eb3000 reproduced across at least 3 more agent re-launches this session (cycle-23 resident → cycle-27 first launch → cycle-27 post-chainload launch) on top of cycle-26's 3 observations (≥6 consecutive reuses in same physical power session), (iii) `mapped_pages_seen=419` reproduced at baseline + post-launch + post-run. Sequence executed: reachability probe (08:44:29Z; agent on 9001, FTP closed → cycle-23 agent foreground) → baseline `witness.scan` (count=1 phys=0x03eb3000 reserved0=0 mapped_pages_seen=419; precondition MET) → `reboot` → dashboard FTP-LIST at t+27s → FTP-upload cycle-27 oracle-agent (417 792 B; verified) → confirm cycle-25 witness-only XBE still resident (147 456 B) → `ensure-agent` launches cycle-27 build (banner unchanged as expected — only internal allocator logic changed) → post-launch rescan identical to baseline → `runxbe E:\\Apps\\witness-only\\default.xbe` → FTP-LIST poll → dashboard ready at t+70s → post-run `ensure-agent` + final `witness.scan` = **count=1 buf.0 phys=0x03eb3000 live=1 reserved0=0x00000000 reserved1=0x00000000**. **ZERO source/script code edits, ZERO XBE rebuilds** — all operations used existing agent verbs and existing Mac-side tooling; rule #15 carve-out for run-only/doc-only slices applies (same path as cycle 26). Evidence preserved on disk: `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/{00..08-*.log, SUMMARY.md}` (gitignored per project convention). Cycle 29 candidate scope (NOT promoted this session — Hermes's call): **option (c) [recommended]** — witness-only allocates its OWN persistent page via `MmAllocateContiguousMemoryEx` with a unique magic tag, eliminating reliance on kseg0 scan finding the agent's buffer (discriminates α from γ: if (c) lands a stamp visible to agent's known-good `witness.scan` scanner, then α was the blocker; if (c) lands nothing, γ becomes leading); plus **option (d)** — on-screen visual breadcrumb via composite capture during witness-only execution (specifically discriminates γ: if a known-pattern breadcrumb appears, main() did execute). Less promising option (b) (agent-side prior-phys dump + read-only kseg0 dump verb) is on the table but lower-priority. Cycle-22 leading hypothesis (image-blit crashes BEFORE main()'s first instruction) remains UNRESOLVED; cycle-29 option (c) or (d) is the path to break it. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-29 design + run), §G.5, RT-as-texture. Two pre-existing untracked `.hermes_cycle*.txt` prompt files at repo root preserved un-staged per the rolling Hermes-supervision pattern. Cycle 27 entry preserved unchanged below — the cycle-27 preserve branch is correctly built and deployed; the cycle-28 outcome is "preserve branch had nothing to preserve," not "preserve branch defective." Originally tagged for cycle 27 Path A.4 option (a) — `oracle-agent/controller.c::s_allocate_fresh` now PRESERVES an existing plausible `oracle_ctrl_buffer` witness header across agent restart instead of unconditionally `memset`-wiping it. Bounded XBE-source slice CLOSED on `apple-silicon-performance`; cycle-28 real-Xbox deployment is Hermes's call. The change directly addresses the cycle-26 stamp-vs-no-stamp ambiguity (cycle 26 commit `a31e061144` observed the kernel pool deterministically returns phys=0x03eb3000 across 3 consecutive agent re-launches and the unconditional `memset` in the old `s_allocate_fresh` would silently wipe any cycle-25 `witness-only` stamp before `witness.scan` could observe it). New `s_page_has_plausible_witness_header` helper in `oracle-agent/controller.c` accepts ONLY two header shapes (Codex round-1 medium adopted): `(reserved0==0, reserved1==0)` for a fresh-init buffer, OR `((reserved0 >> 24) == 0xA4, 1 <= reserved1 <= 4096)` for an A.4-stamped buffer. The preserve gate is a strict subset of the cycle-23 scan filter shared by `lib/xbed_a4_witness.c::a4_candidate_ok` + `oracle-agent/commands.c::a4_reader_candidate_ok`; lockstep documented in three comment blocks. Preserve branch keeps magic/version/reserved[0,1] intact and clears only `port[]` so the new agent session is usable. Non-preserve branch keeps legacy full-zero behavior. One debugPrint breadcrumb when preserve fires reports `phys`, `reserved0`, `reserved1` so an on-screen verification (composite capture) can confirm the branch activated without needing `witness.scan`. With cycle-27 active the cycle-26 readback acquires a NEW positive success shape — `count=1 live=1 reserved0=0xA4xxxxxx` on the cycle-26-reused phys — alongside the legacy orphan shape; documented in `oracle-agent/commands.c::cmd_witness_scan` body comment, `witness-only/README.md` discriminator table, and `witness-only/manifest.json` `artifacts.witness_readback.notes` + `expected_results.real-xbox/physical/cycle-26.notes`. Build: `oracle-agent/bin/default.xbe` 417 792 B rebuilt (size unchanged from cycle-26; benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles). Codex validation 3 rounds per rule #15 (non-trivial diff = 109+ lines across renderer-adjacent oracle-agent C source): Round 1 = MINOR ISSUES (medium #1 predicate too loose — preserve gate accepted `(0, small_nonzero)` which no writer produces; low #2 witness-only docs only described orphan-shape success). Both adopted: predicate tightened to two-shape acceptance; commands.c body comment + witness-only README + manifest extended with the cycle-27 live-buffer success shape. Round 2 = MINOR ISSUES (round-1 MEDIUM RESOLVED; round-1 LOW PARTIAL — manifest `expected_results.real-xbox/physical/cycle-26.notes` retained legacy-only wording). Adopted: that field now describes both shapes. Round 3 = LOOKS GOOD (round-2 PARTIAL RESOLVED; round-1 MEDIUM still RESOLVED; no new round-3 findings). Validation marker written at `.claude/state/codex-validate-last-run`. Cycle-22 leading hypothesis status: still WEAKENED (cycle 27 ships the discriminator-sharpening preserve branch; cycle 28 runs it). Hypothesis #5 status: still PARTIALLY INVALIDATED in the catastrophic-hang sense from cycle 26 (cycle-27 does not run on real Xbox so cannot move that further). Cycle-28 candidate scope (NOT promoted by this session — Hermes's call): re-deploy the cycle-23 oracle-agent (containing this preserve branch) + the cycle-25 witness-only XBE; baseline `witness.scan` precondition (count=1 live=1 reserved0=0); `runxbe witness-only`; poll FTP/21 + agent/9001 + ICMP ping; restart agent; query `witness.scan`. New expected positive outcome shapes: (A1) count=1 live=1 reserved0=0xA4000003 (preserve branch caught the live buffer carrying both stamps), (A2) count>=2 with a stamped orphan reserved0=0xA4000003 (legacy orphan shape if the kernel pool happened to return a different phys this time). Either A1 or A2 means cycle-22 leading hypothesis INVALIDATED and the witness mechanism is real-Xbox-safe in this minimal XBE. Outcome (D-cycle-27) = count=1 live=1 reserved0=0 (preserve branch never matched because no fire landed): cycle-22 leading hypothesis status unchanged, witness mechanism's real-Xbox behavior in witness-only context still ambiguous, escalate to cycle-27 candidate option (c) or (d). Outcome (B) = identical hang to cycle 24: hypothesis #5 PROMOTED back to "real-Xbox-incompatible from non-agent context in some contexts"; redesign required. Also: cycle-28 should still discriminate the cycle-26 ~70 s recovery shape (slow kseg0 scan vs delayed-fault watchdog vs slow BIOS POST) — preserve branch alone does not address that. ZERO host-source touched; ZERO image-blit / witness-only / xbed_a4_witness / xbed_runtime source touched; ZERO XBE rebuilds beyond `oracle-agent` itself. Two pre-existing untracked prompt files at repo root (`.hermes_cycle22_path_a3_prompt.txt`, `.hermes_cycle23_docsync_prompt.txt`) are preserved un-staged per Hermes pre-session instruction. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-28 real-Xbox discriminator run with cycle-27 oracle-agent), §G.5, RT-as-texture. Cycle 26 entry preserved unchanged below — its description of `controller.c:181` as "wiping the stamp" is now obsolete code-wise but accurate as the historical observation that motivated this cycle. Originally tagged for cycle 26 Path A.4 real-Xbox witness-only deployment — **CLOSED with PARTIAL DISCRIMINATOR OUTCOME (outcome shape D, not in cycle-25 design table)**. Claude Code worker autonomously executed the cycle-25-recommended cycle-26 slice from this Mac. Xbox @ 192.168.0.200 was in dashboard state at session start (Hermes had physically power-cycled post cycle-24). Baseline `witness.scan` precondition MET (count=1, phys=0x03eb3000, reserved[0]=0, mapped_pages_seen=419). FTP-uploaded `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` (147 456 B) to `/E/Apps/witness-only/default.xbe`. Chainloaded witness-only 4× plus 2 controls (invalid-path + mirror). **Definitive chainload→dashboard-fully-ready timing via `curl FTP LIST` poll (the `nc -z -w 1` early-positive readings turned out to be polling artifacts during Xbox network-stack transitions): ~70.17 s for witness-only vs 20.67 s for invalid-path control vs ~36 s for known-good mirror control.** Post-run `witness.scan` after EVERY chainload returned IDENTICAL state to baseline: count=1, phys=0x03eb3000, reserved[0]=0, mapped_pages_seen=419 — **no orphan ever observed across 4 chainloads.** Cycle-25 design table expected outcomes (A=clean reboot + orphan 0xA4000003; B=hang like cycle 24; C=clean reboot + orphan 0xA4000001) did NOT anticipate the observed shape (D = ~70 s recovery, no orphan). **Hypothesis #5 PARTIALLY INVALIDATED in the catastrophic-hang sense:** the kseg0-scan witness mechanism from a non-agent process context, in this minimal XBE, does NOT hard-hang the real Xbox — the console fully recovered in 70 s without a physical power-cycle (compare cycle 24's 928 s+ silent). **Cycle-22 leading hypothesis ("image-blit crashes BEFORE main()'s first instruction") remains WEAKENED** — the 70 s gap is consistent with witness-only's main() running to HalReturnToFirmware AND with witness-only never reaching its witness fires; session evidence cannot resolve. **Stamp-vs-no-stamp ambiguity** unresolvable from session data: either witness fire never landed a stamp OR stamp landed but the relaunched agent's `s_allocate_fresh::memset(vp, 0, ...)` (`oracle-agent/controller.c:181`) wiped it after the kernel pool deterministically returned phys=0x03eb3000 across 3 observed agent re-launches (despite `MmPersistContiguousMemory`-tagged prior pages supposedly being excluded). The cycle-25 design table's orphan-survival assumption is invalid on this Xbox. **Cycle 27 candidate options recorded** (NOT promoted by this session — Hermes's call): (a) modify `s_allocate_fresh` to preserve an existing `XCTR + version==1` buffer instead of memset; (b) agent-side dump of prior controller-buffer phys to a known file before chainload + a read-only kseg0 dump verb; (c) witness-only allocates its own page via `MmAllocateContiguousMemoryEx` with a unique magic tag; (d) on-screen visual breadcrumb captured mid-run. Cycle 27 should also discriminate the ~70 s delay (slow kseg0 scan vs delayed-fault watchdog vs slow BIOS POST). Codex validation SKIPPED under rule #15's doc-only / ≤30-line uncommitted source diff carve-out — ZERO source/script code edits this session, ZERO XBE rebuilds; all Xbox-side operations used existing agent verbs and existing Mac-side tooling. Evidence preserved on disk: `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/{00..20-*.log, SUMMARY.md}` (gitignored per project convention). `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-27 stamp-vs-no-stamp discriminator + ~70 s delay discriminator), §G.5, RT-as-texture. Methodology lesson encoded: `nc -z -w 1` produces spurious port=open readings during Xbox network-stack transitions; use `curl --max-time 2 ftp://...` issuing a real FTP LIST as the dashboard-ready ground truth in future cycle-26-style poll loops. Cycle 25 entry preserved unchanged below. Originally tagged for cycle 25 Path A.4 witness-mechanism viability discriminator XBE — **`witness-only` diag XBE SHIPPED** under `scripts/apple-silicon/xbe-tests/witness-only/` exactly per the cycle-24 handoff recommendation. Files: `main.c` (~10-statement `main()`: `xbed_a4_witness_fire(MAIN_ENTERED)` → `Sleep(500)` → `xbed_a4_witness_fire(POST_MARKER0)` → `Sleep(500)` → `HalReturnToFirmware(HalRebootRoutine)` plus three `xbed_host_log_write*` anchor lines for `XEMU_GUEST_LOG=1` visibility), `Makefile` (lib.mk pattern identical to image-blit's), `manifest.json` (`real_xbox_only:true`, `oracle_priority:["real-xbox"]`, record-only `expected_results` for the cycle-26 real-Xbox run + the cycle-25 local xemu-Metal smoke entry), `README.md` (build / deploy / cycle-26 sequence + full discriminator-semantics table), `.gitignore` (peer convention). Build outputs: `bin/default.xbe` 147 456 B + `witness-only.iso` 720 896 B via the project nxdk flow. Local xemu-Metal smoke validation green: 9× `witness-only: main() entered`, 9× `xbed_a4_witness: enter stage=1`, 9× `fire1 returned`, 9× `xbed_a4_witness: enter stage=3`, 9× `fire2 returned`, 8× `rebooting via HalReturnToFirmware` lines across a 25 s timeout window under `XEMU_GUEST_LOG=1` — the XBE boots, fires both stages, reboots, and loops (because the iso is the DVD; on real Xbox the chainload exits to dashboard); preserved at `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-20260523T034519Z/`. Local validation only proves the mechanism's logic-level correctness in emulation; xemu's `MmGetPhysicalAddress` emulation cannot reproduce real-Xbox MMIO-aliasing failure modes, so the real-Xbox discriminator answer remains cycle-26 scope. Codex validation round 1 = MAJOR ISSUES with 4 findings (HIGH #1 discriminator overclaim, HIGH #2 state-file overstatement, MEDIUM #3 missing .gitignore, LOW #4 manifest count); all 4 adopted. Round 2 = BLOCK on residual #1 PARTIAL (main.c retained 2 leftover overclaim sites) + new LOW (current-cycle.md exit-checkbox/claude-status.md disagreement); both adopted. Round 3 = **PASS_WITH_FINDINGS** with round-2 #1 PARTIAL RESOLVED + round-2 new LOW PARTIAL (claude-status.md residual stale "round 2 pending" wording; fixed before docs sync). Validation marker recorded. **The bounded assignment was "ship the cycle-25 witness-only XBE so Hermes can later schedule the real-Xbox deployment slice from durable docs"; cycle 25 does NOT include the real-Xbox run — that is Hermes's call for cycle 26.** Discriminator semantics for the cycle-26 run (Hermes-scheduled): hard precondition = baseline `witness.scan` shows exactly 1 live `oracle_ctrl_buffer` with `reserved[0]==0` (power-cycle Xbox first if multiple A.4-tagged orphans pre-exist); deploy → baseline scan → `runxbe E:\Apps\witness-only\default.xbe` → poll FTP/21+agent/9001+ICMP ping for dashboard return → restart agent → query `witness.scan`. Branch results: reboots in ~5..15 s + orphan with `reserved[0]==0xA4000003` → witness mechanism IS real-Xbox-safe in this minimal XBE (image-blit's hang is in code ABSENT from witness-only = pbkit / NV2A / xbed_init / draw AND the `image_blit_marker(0,...)` helper itself, because cycle-25 substitutes a passive `Sleep(500)` for it; independently excluding the marker helper requires a follow-on cycle), cycle-22 "pre-main crash" hypothesis INVALIDATED, cycle 27 splits image-blit's instrumentation across multiple smaller discriminator XBEs to localize; hangs Xbox identically to cycle 24 → witness mechanism itself is real-Xbox-incompatible from a non-agent process context (redesign required — EEPROM scratchpad / non-MMIO-aliased RAM / abandon in-XBE witness); reboots cleanly but orphan has `reserved[0]==0xA4000001` (MAIN_ENTERED but not POST_MARKER0) → witness fires once but second fire hangs the box (less likely; worth surfacing). `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF. M15 overall still **NOT MET** pending §H.6 default-on shape (still blocked on cycle-26 witness-mechanism viability discrimination), §G.5, RT-as-texture. Cycle 24 entry preserved unchanged below. Originally tagged for cycle 24 Path A.4 real-Xbox discriminator run — **CONCRETE BLOCKER PROVEN; cycle-22 leading hypothesis "pre-main crash" WEAKENED (not corroborated; not invalidated)**. Cycle-23 binaries (oracle-agent + image-blit) deployed to real Xbox via FTP; baseline `witness.scan` precondition MET (exactly 1 live `oracle_ctrl_buffer` at phys=0x03eb3000 / virt=0x83eb3000 / reserved0=0 / reserved1=0; `mapped_pages_seen=419`; magic `XCTR`); `runxbe E:\Apps\image-blit\default.xbe` issued at 2026-05-23T02:34:34Z; **Xbox went fully silent (no FTP/21, no agent/9001, no ICMP ping) and stayed silent for 928.3 s (≈15.5 min) — measurement aborted at 2026-05-23T02:50:02Z**. This is a NEW failure mode that did NOT occur in cycle 19/20/21 (which reproduced a stable 22.4 s chainload→FTP-back gap across 4 attempts with the pre-witness image-blit binary). The change between cycle-21 image-blit and cycle-23 image-blit is ~196 LOC: 2 witness call sites in `main.c` + `xbed_a4_witness.{h,c}` (linked but only called from those 2 sites). The "consistent 22.4 s auto-reboot" regressing to "indefinite hard hang" is weak-but-real evidence that something inside `main()` (most parsimonious candidate: the witness call itself or the lib code now linked into the binary) is being executed and is destabilizing the OG Xbox CPU in a way the watchdog does not recover from. Empirical consequence: the persistent kernel-pool witness buffer (and any `reserved[0]/reserved[1]` writes that landed before the hang) is UNREADABLE without rebooting; the only way to reboot from a hard hang is a physical power cycle; power-cycling erases `MmPersistContiguousMemory` pages; therefore the cycle-24 discriminator answer is UNRECOVERABLE from this run. Cycle 24 closes with the concrete-blocker outcome documented and the cycle-22 leading hypothesis WEAKENED rather than corroborated or invalidated. Recommended cycle-25 bounded slice: build a minimal "witness-only" diag XBE (no pbkit / no NV2A / no rendering) that fires `xbed_a4_witness_fire(MAIN_ENTERED)`, sleeps briefly, then `HalReturnToFirmware(HalRebootRoutine)` — if THAT hangs the Xbox too, the witness mechanism itself is real-Xbox-incompatible and needs a redesign without kseg0 scanning; if THAT reboots cleanly in ~5 s, the hang is from somewhere AFTER the witness call in image-blit's `main()` (pbkit / NV2A / xbed_init) and the witness DID land its byte before the hang — meaning a re-run with a cycle-25 "soft-reboot-on-fault" mechanism could read the persistent buffer post-fault. Evidence: `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/{01-deploy.log, 02-baseline-witness-scan.log, 03-chainload-image-blit.log}`. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-25 witness-mechanism viability), §G.5, RT-as-texture. Cycle 23 entry preserved unchanged below. Originally tagged for cycle 23 Path A.4 — **non-fopen kernel-pool controller-buffer witness instrumentation for image-blit SHIPPED + agent-side `witness.scan` RPC SHIPPED. Bounded code slice CLOSED; cycle-24 real-Xbox discriminator run pending (Hermes's call to schedule). Implementation: new `scripts/apple-silicon/xbe-tests/lib/xbed_a4_witness.{h,c}` (~170 lines) + 2 call sites in `scripts/apple-silicon/xbe-tests/image-blit/main.c` (+26 lines) bracketing the existing cycle-20 `image_blit_marker(0, "program_entered")` call + 1 new agent RPC `witness.scan` in `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c` (+127 lines). Both writer (image-blit) and reader (agent `witness.scan`) scan kseg0 [0x80010000, 0x84000000] in 4 KiB strides using `MmGetPhysicalAddress` per-page safety gates (Codex round-1 BLOCKING #1: blind dereference faults on unmapped kseg0 pages — confirmed locally) plus a SHARED candidate filter set ('XCTR' magic + version 1 + `reserved[0]` either 0 or A.4-tagged + `reserved[1] < 4096`; Codex round-1 BLOCKING #2: magic+version-only matched false positives). Writer targets HIGHEST-phys passing candidate (Codex round-1 MEDIUM #3: targeting first/oldest match was attribution-ambiguous across repeated runs in one power session). Stamps `(0xA4 << 24) | stage` to `reserved[0]` + bumps `reserved[1]` counter; preserves magic + version so successive scans match the same buffer. Local xemu-Metal validation (4 boots): witness call sites fire on every boot (`xbed_a4_witness: enter stage=1` + `enter stage=3` lines under `XEMU_GUEST_LOG=1`), correctly reports "no XCTR buffer found" on standalone xemu (no agent ran), image-blit pass=3/8 mask=0x31 tally UNCHANGED on first boot (matches cycle-21 baseline; no instrumentation regression). Codex round 1 returned BLOCK with 4 findings; all adopted; Codex round 2 returned PASS_WITH_FINDINGS with all 3 BLOCKING + 1 MEDIUM RESOLVED + 1 MINOR PARTIAL (header doc drift); MINOR resolved post-round-2 via direct comment sync. Validation marker written at `.claude/state/codex-validate-last-run`. Cycle 23 does NOT include a real-Xbox run — the bounded assignment was "add the witness," not "run it." Cycle 24 (Hermes-scheduled) will do the real-Xbox discriminator run: baseline `witness.scan` (expect 1 live buffer reserved=0) → chainload image-blit via `runxbe` → wait for FTP-back → restart agent → query `witness.scan` (expect 2 buffers; if orphan's reserved[0] == 0xA4000003 → cycle-22 leading hypothesis INVALIDATED; if 0xA4000001 → main() entered but marker_00 helper crashed; if 0 → cycle-22 leading hypothesis CORROBORATED).** `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision remains DEFERRED. Cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; the flag continues to ship opt-in, default OFF. M15 overall still **NOT MET** pending §H.6 default-on shape (now blocked on cycle-24 result), §G.5, RT-as-texture. Cycle 22 entry preserved unchanged below.

## 2026-05-23 (cycle 33 composite-capture fail-fast preflight slice) — SHIPPED `composite-preflight.sh` + default-on integration into `composite-record.sh` to convert cycle-32 OUTCOME F8 silent-stall into a fast actionable abort

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "ship a fail-fast composite-capture preflight so the cycle-32 OUTCOME F8 failure mode (MS2109 connected, no live signal → ffmpeg silent for ~93 s before SIGKILL) becomes a few-second abort with an actionable physical-side checklist, and wire it into `composite-record.sh` by default." Scope contained strictly inside composite tooling + directly paired docs/rules/state updates. NO host renderer changes; NO XBE rebuilds; NO oracle-agent changes; NO real-Xbox run; NO cycle-32 redo.

**Why this cycle ran.** Cycle 32 closure (commit `ac515383bb`) recorded OUTCOME F8 = "cycle-32 procedural failure — MS2109 composite-capture leg recorded zero frames." Root cause was hardware-side (no live composite signal at MS2109 input), but the procedural cost was painful: `composite-record.sh --duration 70` plus the 20 s watchdog ran to wall-elapsed ~93 s before SIGKILL, with zero bytes of ffmpeg stderr. A re-attempt of the cycle-32 sequence without operator intervention would silently repeat the same stall during the witness-only chainload window, burning ~90 s of test time and producing zero evidence. Cycle 33 ships the cheapest tool that closes that gap: a few-second pre-arm probe that detects "device opens, no frames arrive" against the same MS2109 and aborts BEFORE arming the long capture. This follows workspace `CLAUDE.md` rule #5 (build/extend tools when the existing toolset is the limit) — `composite-record.sh` alone could not distinguish a hardware-side no-signal from a transient encoder warmup without burning a full capture window.

**Implementation.** Two files modified, one new file. No source-tree changes outside `scripts/apple-silicon/`.

1. **New `scripts/apple-silicon/composite-preflight.sh` (~310 lines, executable bash + python3 inline).** Standalone diagnostic + library-of-one for `composite-record.sh`. Accepts the same device / width / height / fps / pixel-format / no-audio flags as `composite-record.sh` so the preflight runs against the same AVFoundation device the long capture will use. Default `--mode auto` prefers `scripts/apple-silicon/bin/xemu-capture snapshot` (the TCC-approved app-bundle path the retail oracle already authenticates against) and falls back to `ffmpeg -f avfoundation -frames:v 1` against the resolved numeric AVFoundation index when xemu-capture is unavailable or returns a non-ok JSON. The ffmpeg fallback uses the same composite-record.sh substring-resolver awk routine + a python3 deadline timer that SIGKILLs ffmpeg if no frame arrives within `--timeout SECONDS` (default 8). Output always goes to a `preflight-meta.json` (schema `composite-preflight/v1`) capturing status / exit_code / detector / elapsed_s / detail + every input parameter + backend availability flags + probe-image path when present. Status codes: `ok` (rc 0), `no_signal` (rc 2 — cycle-32 F8 shape), `device_not_found` (rc 3 — substring didn't match an AVFoundation video device), `no_backend` (rc 4 — no xemu-capture AND no ffmpeg), invalid CLI (rc 5), unexpected (rc 1). On any non-zero exit the script prints a 5-line physical-side checklist drawn from cycle-32 evidence (composite cable seating, MS2109 input selector, Xbox AV output mode, USB-port stability — Mac Studio M2 Ultra front USB-C ASMedia 3142 brown-outs are a documented historical failure mode — plus the only xemu-capture verb that actually proves live frames: `xemu-capture snapshot DEVICE --out /tmp/probe.png`; `inputs` / `set-input` reserved for confirming the active input source AFTER snapshot succeeds — Codex round-2 low #3 adopted). On any non-zero exit the probe PNG is deleted so callers never pick up a stale frame. `--json` echoes the final JSON to stdout for piping; `--quiet` suppresses the human-readable banner.

2. **`scripts/apple-silicon/composite-record.sh` (+~140 lines).** New flags: `--skip-preflight`, `--preflight-timeout SECONDS` (default 8), `--preflight-mode auto|xemu-capture|ffmpeg`. Two env-var overrides: `COMPOSITE_PREFLIGHT_TIMEOUT` and `COMPOSITE_PREFLIGHT_MODE`. By default `composite-record.sh` now runs `composite-preflight.sh` first against the same device parameters, with its `--out-dir` pointed at `$OUT_DIR/preflight/`. On preflight failure, composite-record.sh writes a stub `capture-meta.json` with schema `composite-record/v1`, `status="preflight-failed"`, `ffmpeg_invoked=false`, and a `preflight` summary object (status / exit_code / detector / elapsed_s / meta_path), then exits with the preflight's own rc (2 cycle-32 F8 no_signal / 3 device_not_found / 4 no_backend / 1 host-side backend error / 5 invalid CLI — Codex cycle-33-closeout round-2 LOW adopted to keep the rc enumeration in sync with the rc=1 vs rc=2 contract the preflight now ships) WITHOUT touching ffmpeg. On preflight success the long ffmpeg capture proceeds unchanged and the post-run `capture-meta.json` is patched with the same `preflight` object alongside the existing ffmpeg_rc / wall_elapsed_s / video_duration_s fields. The `--skip-preflight` path is explicitly preserved so operators who have already physically verified the capture chain can bypass the probe (e.g. when xemu-capture / ffmpeg one-shot probes are known to interact badly with the stick, or when running the preflight separately for diagnostic purposes).

3. **Paired doc updates.** `docs/apple-silicon/automation.md` gains a new "Composite capture preflight — `composite-preflight.sh` (cycle 33)" section directly above the existing "Composite A/V recording" section, documenting flags / exit codes / outputs / physical-side checklist semantics. The existing `composite-record.sh` section is extended with the three new flag rows + a new "Preflight default (cycle 33)" paragraph documenting the default-on behavior, the `--skip-preflight` opt-out, and the env-var overrides. `.claude/rules/flags-bench.md` gains a new "Composite-capture preflight (cycle 33)" subsection listing `COMPOSITE_PREFLIGHT_TIMEOUT`, `COMPOSITE_PREFLIGHT_MODE`, and the `--skip-preflight` operator switch.

**Validation.** Local script validation green: `bash -n` passes for both modified scripts; happy-path no-signal probe against the live MS2109 (current physical state per cycle 32: device enumerated, zero signal) returns rc 2 + status `no_signal` in ~3.6 s via the ffmpeg fallback (vs cycle 32's ~93 s); end-to-end `composite-record.sh --duration 2 --preflight-mode ffmpeg --preflight-timeout 3` aborts in ~3.6 s with a structured `capture-meta.json` containing `status=preflight-failed` + `ffmpeg_invoked=false`; `--skip-preflight` end-to-end test reproduces the legacy cycle-32 silent-stall behavior unchanged (preserves the escape hatch); `device_not_found` (rc 3) and `--mode ffmpeg` paths both produce valid JSON. The xemu-capture primary detector path is exercised by `--mode auto` when the device is enumerated; on this host it currently times out the same way ffmpeg does because the underlying AVFoundation session sees the same no-signal MS2109, and the script then falls back to ffmpeg cleanly. Codex validation per rule #15 (closeout session): 6 rounds. Round 1 = MINOR ISSUES with 4 findings (M1 detector field comma-joined in failure path overrode the singular-enum schema → ADOPTED, both no_signal/rc=2 and error/rc=1 paths now emit `detector="none"` while detectors_attempted/detector_reasons/detector_details carry the multi-backend detail; M2 `COMPOSITE_PREFLIGHT_*` env vars not in xemu-fork/CLAUDE.md → DEFLECTED because xemu-fork/CLAUDE.md enumerates only the meta-rule for runtime flags and individual `XEMU_BENCH_*` / `XEMU_PERF_*` / `XEMU_DIAG_*` knobs follow the same automation.md + flags-*.md pattern, not CLAUDE.md enumeration; L3 state-file inconsistency about validation status → DEFERRED to closeout sync; L4 stale `xemu-capture probe` + `set-input` forward-looking redo guidance in handoff-summary.md + decision-log.md → ADOPTED, replaced with `xemu-capture snapshot USB2 --out /tmp/probe.png` as the live-frame proof). Round 2 = MINOR ISSUES with 2 findings (Minor #1 post-preflight device-resolution failures `exit 1`-ed with no top-level capture-meta.json — audio path reachable by design because preflight intentionally doesn't open audio → ADOPTED, new `emit_device_not_found_meta` helper emits structured `capture-meta.json` (schema composite-record/v1, status="device-not-found", ffmpeg_invoked=false, embedded preflight summary) before exiting with rc=3 matching the preflight's documented `device_not_found` exit code; Minor #2 = round-1 L3 still unresolved in diff → DEFERRED to closeout sync). Round 3 = MINOR ISSUES with 1 finding (bad `FFMPEG=/bad/path` override was misclassified as device-not-found rc=3 because the early no-backend check only tested for empty FFMPEG → ADOPTED, the early check now also tests `[ ! -x "$FFMPEG" ]` so non-executable overrides produce `status="preflight-failed"` with `preflight.status="no_backend"` rc=4). Round 4 = MINOR ISSUES with 1 finding (broken-but-executable FFMPEG e.g. `/usr/bin/true` or an ffmpeg build without AVFoundation still misclassified as device-not-found because `ffmpeg -list_devices ... || true` swallowed the failure → ADOPTED, after the `-list_devices` call composite-record.sh now greps for the canonical "AVFoundation ... devices:" header and emits a structured `status="preflight-failed"` / `preflight.status="no_backend"` rc=4 stub with the broken-backend reason embedded if the header is missing; legitimate `--device NoSuchDeviceZZZ` still routes to rc=3 device-not-found). Round 5 = MINOR ISSUES with 1 finding (`ffmpeg -list_devices` + `xemu-capture list` enumeration calls lack a hard wall-clock deadline → DEFLECTED with rationale: cycle-33 bounded goal is "convert cycle-32 F8 silent-stall into a fast actionable abort"; F8 is the AVFoundation-opens-cleanly-but-frames-never-arrive failure mode, NOT the AVFoundation-system-extension-wedged failure mode; enumeration has never been observed to exceed <1 s across cycles 26 / 28 / 30 / 32 evidence; the per-detector budget arithmetic already subtracts elapsed-since-start so the detect phase is bounded even if enumeration overshoots; bounding enumeration is a clean future hardening slice if AVFoundation system-extension hangs are ever observed). Round 6 = **LOOKS GOOD** ("No findings"; round-5 deflect confirmed reasonable for this bounded slice; explicit Codex agreement that `xemu-capture list` is a light `AVCaptureDevice.DiscoverySession(...).devices` enumeration, not a live capture-session startup, so the remaining hang class is "AVFoundation/device-discovery itself wedges at the host level" — plausible in theory but not the cycle-32 F8 failure mode). Validation marker written at `.claude/state/codex-validate-last-run`.

**What this slice does NOT do.**

- NO real-Xbox cycle-32 redo (out-of-scope per session prompt; Hermes's call after physical-side composite-cable / capture-input verification).
- NO host xemu source touched; NO renderer / TCG / NV2A / build changes.
- NO XBE rebuilds; NO `oracle-agent/*` / `lib/xbed_a4_witness.{c,h}` / `lib/xbed_self_witness.{c,h}` / `lib/xbed_runtime.{c,h}` / image-blit / `witness-only/main.c` touched.
- NO cleanup or commit of the intentional untracked `.hermes_cycle*.txt` + `.hermes_launch_cycle*.sh` files at repo root (preserved un-staged per the rolling Hermes-supervision pattern, consistent with cycles 26 / 27 / 28 / 29 / 30 / 31 / 32 handling).
- NO PushNotification — bounded tooling slice, not a milestone.
- NO M15 default-on movement; `XEMU_DIAG_PGRAPH_STATUS_DRAIN` long-term decision unchanged; M15 overall still NOT MET pending §H.6 default-on shape (still blocked on cycle-32 redo with composite capture confirmed armed + downstream cycle-33-onward investigation per cycle-31/32 hypothesis state), §G.5, RT-as-texture.

**Operator workflow change (binding for unattended cycles).** From cycle 33 onward, `composite-record.sh` aborts BEFORE arming ffmpeg whenever the MS2109 is not producing frames. Pass `--skip-preflight` explicitly to bypass; operators who have already verified the capture chain visually can also pass `COMPOSITE_PREFLIGHT_MODE=xemu-capture` to force the TCC-approved snapshot path or `COMPOSITE_PREFLIGHT_MODE=ffmpeg` to force the bare-ffmpeg path. The cycle-32 redo runbook (`witness-only/README.md` cycle-31 addendum §"Cycle-32 deployment runbook") becomes safer to run unattended because step 8 (`composite-record.sh ARM`) will refuse to proceed if the physical-side problem from cycle 32 is still present, returning a structured `preflight-failed` capture-meta.json instead of recording zero frames during the witness-only chainload window.

**Cycle 34 candidate scope (NOT promoted by this session — Hermes's call).** Cycle 33 closes the unattended-orchestration gap surfaced by cycle 32 for the `composite-record.sh` direct-invocation path (the path the cycle-32 redo runbook in `witness-only/README.md` cycle-31 addendum §"Cycle-32 deployment runbook" step 8 actually uses). The substantive next slice remains the cycle-32 redo (deploy cycle-31 witness-only XBE + run the canonical sequence + classify F1..F8 per the cycle-31 discriminator table) — that requires physical-side composite-cable / capture-input verification first. Cycle 34 might also opt to power-cycle the Xbox if Hermes prefers a fresh cold-boot state for the redo. The hypothesis-investigation roadmap remains as recorded in cycle 32's closure: on F4 outcome → cycle-22 pre-main hypothesis FULLY CORROBORATED → cycle 34+ ships pre-main breadcrumbs; on F1/F2/F3/F5 → γ INVALIDATED via stripe 0 visible → cycle 34+ re-elevates option (b) for α-vs-β.

**Known follow-up (intentionally OUT-OF-SCOPE for cycle 33 — Codex round-3 medium #1 deflected to a future cycle).** `scripts/apple-silicon/capture-composite-reference.sh`, `scripts/apple-silicon/retail-title-automation-proof.py`, and `scripts/apple-silicon/retail-gameplay-oracle.py` background `composite-record.sh` and then issue `runxbe` (or the equivalent retail launch) unconditionally after a small sleep. The cycle-33 preflight runs INSIDE that backgrounded composite-record.sh, so on failure the recorder leg dies but the Xbox-side leg still fires — for those wrappers a preflight failure shortens the dead recorder leg but does not gate the Xbox-side action. The cycle-32 redo path uses `composite-record.sh` directly (per the cycle-31 deployment runbook §step 8) and is fully protected. Wiring the preflight as a synchronous pre-launch gate into the 3 backgrounding wrappers is a separate slice (each has different recorder-launch semantics, signal-readiness contracts, and TCC behavior) and is deferred to a future cycle.

## 2026-05-23 (cycle 32 Path A.4 real-Xbox deployment of the cycle-31 visual-breadcrumb build vs the cycle-31 witness-only XBE) — OUTCOME F8 (cycle-32 procedural failure); hypothesis state UNCHANGED from cycle 30; cycle-22 leading hypothesis STILL RE-STRENGTHENED but not yet fully corroborated

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "deploy the cycle-31 `witness-only/bin/default.xbe` (155 648 B; SHA-256 `c00c726c96f2172badbe0dcd20c111ab89eee95960b8ce43d03c472db4e09edb`) to real Xbox + run the cycle-30 canonical sequence with the composite-capture leg ARMED via `scripts/apple-silicon/composite-record.sh` + classify the outcome per the cycle-31 8-row F1..F8 + F4' discriminator table." The Xbox-side leg completed successfully (FTP-upload OK, runxbe issued, dashboard returned at t+30 s, post-run scans observed). The composite-capture leg failed at the hardware level: the MS2109 stick is connected and AVFoundation enumerates it (`AV TO USB2.0` at video=[0] + audio=[3]), but ffmpeg never received any frames across 70 s of `-t` plus the 20 s composite-record watchdog grace (rc=137 SIGKILL after wall-elapsed 93 s; stderr log 0 bytes; `capture_timed_out=true`). A follow-up 4 s standalone ffmpeg probe against the same device (no audio mux, no encoder choice difference) produced the same shape — zero bytes of stderr across 60+ s before being killed manually. This is the canonical MS2109 "device connected but receiving no signal" failure mode (composite cable not seated at Xbox AV port OR MS2109 input selector on S-Video instead of composite OR Xbox AV output not on composite). Per the cycle-31 cycle-32 discriminator table, **outcome F8 = no composite capture available → procedural failure, NOT a discriminator answer → cycle-32 redo with composite capture confirmed armed.**

**Why this cycle ran.** Cycle 31 closure (commit `41f350c174`) shipped the option-(d) visual-breadcrumb infrastructure but explicitly left the real-Xbox deployment as Hermes's call. The cycle-31 closure motivation stands: cycle 30 (commit `dfe1480cba`) observed E2 = `witness.scan = D-cycle-27` AND `witness.scan-self = count=0`; (γ) "main() never reaches the fire calls" is LEADING; the cycle-22 pre-main-crash hypothesis is RE-STRENGTHENED toward leading but not fully corroborated. A successful cycle-32 capture would have discriminated stripe 0 visible (γ INVALIDATED) from no stripes visible AND `witness.scan-self count=0` (F4 = γ FULLY CORROBORATED) at coarser granularity than cycle 30's two-tuple alone.

**Witness-side readback (still high-value, two-tuple matches cycle 30's E2).**

```
baseline witness.scan         = 201 buf.0 phys=0x03eb3000 virt=0x83eb3000 live=1
                                reserved0=0x00000000 reserved1=0x00000000
                                count=1 mapped_pages_seen=419   (D-cycle-27)
baseline witness.scan-self    = 201 count=0 mapped_pages_seen=419
pre-run   witness.scan        = (identical to baseline)
pre-run   witness.scan-self   = 201 count=0 mapped_pages_seen=419
post-run  witness.scan        = (identical to baseline; D-cycle-27)
post-run  witness.scan-self   = 201 count=0 mapped_pages_seen=419
```

The two-tuple `(witness.scan, witness.scan-self) = (D-cycle-27, count=0)` is functionally IDENTICAL to cycle 30's E2 readback. Per the cycle-31 F1..F8 + F4' discriminator table:

- **F4' RULED OUT.** F4' requires `witness.scan-self count >= 1` (graceful XVideoSetMode FALSE return = main() executed past paint(0) AND ran cycle-29 self-witness fires which still stamp the WTNS page). cycle-32 observed `count=0`; F4' is incompatible with this readback.
- **F6 / E1 / E1' / E1'' RULED OUT.** All require either WTNS stamping success OR XCTR success shape on `witness.scan`; cycle-32 observed neither.
- **F2 / F3 / F4 / F5 all CONSISTENT** with the observed two-tuple `(D-cycle-27, count=0)`. The deepest-visible-stripe count is the SOLE remaining discriminator among those four, and that data was never captured.

**Capture failure root cause (diagnosed in-session).** AVFoundation opens the MS2109 successfully on both ffmpeg-launch attempts (composite-record.sh's substring resolver picks video=[0] + audio=[3]; the device-list enumeration prints `[0] AV TO USB2.0`). ffmpeg produces no `Stream #0:0` / `Stream #0:1` lines, no `frame=` progress lines, and no `error opening input` / `Input/output error`; it sits silently consuming wall-clock until the composite-record watchdog (`--duration 70` + `CAPTURE_TIMEOUT_EXTRA=20`) sends SIGKILL at wall-elapsed 93 s (`rc=137`, `capture_timed_out=true`). Re-running with a minimal 4 s ffmpeg probe (no audio, video-only) reproduced the same silent-no-frames behavior across 60+ s before manual SIGKILL. Both attempts are well documented in `benchmark-runs/cycle32-real-xbox-witness-only-visual-20260523T135646Z/{08-composite-record.log, 12-ms2109-probe.log, composite-cycle32/{capture-meta.json, capture-stderr.log}}`. The canonical reading of "AVFoundation opens device, no frames arrive, no error" is **no live composite signal at the MS2109 input** — physical-side action required (cannot be remediated from a Claude session): verify composite cable seated at Xbox AV port; verify MS2109 input selector is composite (not S-Video); optionally build `tools/xemu-capture/` and use `xemu-capture snapshot USB2 --out /tmp/probe.png` (the only xemu-capture verb that actually proves live frames are arriving) — `inputs` / `set-input` only AFTER snapshot succeeds, to confirm the active input is composite vs S-Video.

**Timing observation (weak signal; single-sample; explicitly NOT load-bearing).** Cycle 32 dashboard-recovery returned at t+30 s after runxbe — 9 s FASTER than cycle 30's t+39 s. The cycle-31 build adds ~2 s of additional expected work over cycle 29 (one extra `XVideoSetMode` kernel call + Sleep extension 500 → 2 000 ms). A smooth "main() ran past every checkpoint with the extended 2 000 ms settle Sleep" interpretation predicts ~t+41 s. The observed t+30 s is therefore mildly inconsistent with the full-success interpretation and mildly consistent with an early crash before the extended settle (γ-ish). Cycle-31 closure notes explicitly flagged timing as single-sample and not load-bearing; cycle 32 carries that flag forward. The recovery-time series across the four real-Xbox witness-only runs so far is: cycle 26 = ~70.17 s, cycle 28 = ~70 s, cycle 30 = t+39 s, cycle 32 = t+30 s; the cycle 26/28 vs cycle 30/32 split correlates with the cycle-29 self-witness `MmAllocateContiguousMemoryEx` activity being present in the latter pair, but timing alone cannot distinguish "earlier crash" from "faster successful main() path."

**Reproducibility wins this cycle.** Kernel-pool deterministic `phys=0x03eb3000` reuse REPRODUCED across pre-reboot baseline + post-relaunch baseline + post-run readback = **≥12 consecutive observations across cycles 26 / 28 / 30 / 32** in the same physical power session (the cycle-32 readbacks confirm `phys=0x03eb3000` for the 10th / 11th / 12th time). `mapped_pages_seen=419` REPRODUCED at baseline + pre-run + post-run (8th / 9th / 10th observations across the four cycles). The cycle-31 build chainloads cleanly (no 928 s+ cycle-24-style hang); the FTP upload (size mismatch 151 552 → 155 648 B; `--overwrite` passed for safety per the cycle-30 methodology lesson) succeeded with correct post-upload size + mtime advance.

**Sequence executed.** 14 logged steps in `benchmark-runs/cycle32-real-xbox-witness-only-visual-20260523T135646Z/{00..12-*.log, SUMMARY.md, composite-cycle32/{capture-meta.json, capture-stderr.log}}` (gitignored per project convention). 00 reachability (ping=true, ftp=false, agent=true; cycle-29 oracle-agent resident from cycle 30); 01 agent info (`v0.4 (Phase 2 + controller.* + smc.*)`; `mode=640x480@32bpp`); 02 baseline both scans (preconditions MET); 03 reboot; 04 dashboard FTP-LIST at t+12 s (authenticated `curl -u xbox:xbox` returning `226`; cycle-30 methodology preserved); 05a pre-upload list (cycle-29 build still resident: 151 552 B); 05 `--overwrite` FTP upload of cycle-31 binary (155 648 B; post-upload list verified `155648 Dec 10 17:17`); 06 ensure-agent (cycle-29 build re-launched; banner unchanged as expected); 07 pre-run both scans (preconditions still MET); 08 composite-record.sh `--duration 70` armed in background (resolved video=[0] audio=[3]; ffmpeg launched but produced no frames); 09 runxbe at `2026-05-23T13:59:46Z`; 10 dashboard FTP `226` at t+30 s after runxbe; 11 ensure-agent + post-run scans (`witness.scan` = D-cycle-27; `witness.scan-self` = count=0; both identical to cycle 30 E2); 12 standalone 4 s MS2109 probe (silent no-frames across 60+ s; killed manually; confirms hardware-side capture failure, not a cycle-32 procedural bug in `composite-record.sh`).

**Hypothesis status after cycle 32 (UNCHANGED from cycle 30; only F4' eliminated from cycle-32-redo candidate space).**

- (γ) "main() never reaches the fire calls" STILL LEADING.
- (α) "scan can't find XCTR from non-agent context" STILL LIVE but DEPRIORITIZED.
- (β) "scan finds XCTR but write faults silently" STILL LIVE but DEPRIORITIZED.
- Cycle-22 pre-main-crash hypothesis STILL RE-STRENGTHENED toward leading but NOT fully corroborated. Cycle-32 redo with composite capture confirmed armed will move it (F4 → fully corroborated in its strongest form; F1/F2/F3/F5 → strong INVALIDATION via stripe 0 visible).
- F4' (graceful `XVideoSetMode` FALSE return) RULED OUT by cycle-32's WTNS `count=0` readback — eliminated from the cycle-32-redo candidate space.
- Hypotheses #1 / #2 / #3 unchanged. Hypothesis #5 catastrophic-hang sense unchanged (cycle 32 reproduced clean ~30 s dashboard recovery, NOT a 928 s+ hang).

**Cycle 32 redo scope (NOT promoted this session — Hermes's call).** (1) Hardware-side: verify composite cable seated at Xbox AV port; verify MS2109 input selector is composite (not S-Video); optionally `cd tools/xemu-capture && make` + use `xemu-capture snapshot USB2 --out /tmp/probe.png` (the only xemu-capture verb that actually proves live frames are arriving — `inputs` / `set-input` only AFTER snapshot succeeds). (2) Smoke-test capture before re-arming the cycle-32 sequence: `composite-record.sh --duration 4 --label smoke` should produce a non-empty `video.mp4`; inspect first frame to visually confirm an Xbox-dashboard frame. (3) OPTIONAL: power-cycle the Xbox (kernel-pool `phys=0x03eb3000` deterministic reuse has reproduced ≥12 consecutive observations in this physical power session; a cold reboot is not strictly required for the cycle-32 redo because cycle 32 + cycle 30 both left `reserved0=0 reserved1=0` on the persistent buffer, but the side-channel "fresh power-on" interpretation is cleaner with a power-cycle — Hermes's call). (4) Run the canonical cycle-32 sequence per `scripts/apple-silicon/xbe-tests/witness-only/README.md` cycle-31 addendum §"Cycle-32 deployment runbook" steps 1-11 verbatim. The cycle-31 `bin/default.xbe` is already deployed at `/E/Apps/witness-only/default.xbe` (155 648 B).

**Codex SKIPPED.** Rule #15 doc-only / run-only carve-out applies — ZERO source/script code edits this cycle, ZERO XBE rebuilds. Same path as cycles 26 / 28 / 30. The cycle-31 binary deployed this cycle is the same binary cycle 31 Codex-validated (3-round green); the canonical-doc updates this cycle are the only doc surfaces touched and are bounded reconciliations to the cycle-32 outcome.

**Scope discipline (this slice).** ZERO xemu-fork host source touched. ZERO XBE rebuilds. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact). ZERO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 `witness.scan-self` verb intact). ZERO `xbed_runtime.{c,h}` touched. ZERO image-blit touched. ZERO cycle-33 implementation work. ZERO `PushNotification` — bounded blocker closeout, not a milestone. Five pre-existing untracked `.hermes_cycle*.txt` + `.hermes_launch_cycle*.sh` files at repo root preserved un-staged per the rolling Hermes-supervision pattern (consistent with cycles 26 / 27 / 28 / 29 / 30 / 31 handling).

**Files touched (cycle 32).** Evidence-only: `benchmark-runs/cycle32-real-xbox-witness-only-visual-20260523T135646Z/{00..12-*.log, SUMMARY.md, composite-cycle32/{capture-meta.json, capture-stderr.log}}` (gitignored per project convention; `composite-cycle32/video.mp4` was never produced). Canonical-doc updates: `docs/apple-silicon/handoff.md` (this entry on top; cycle-31 + cycle-30 entries preserved unchanged below); `docs/apple-silicon/decision-log.md` (cycle-32 entry on top; cycle-31 preserved unchanged); orchestration-state quartet closure pass. `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on / long-term-fix decision REMAINS DEFERRED; cycle-17's xemu-side `8/8 mask=0xff` finding is NOT invalidated; flag continues to ship opt-in, default OFF. M15 overall still NOT MET pending §H.6 default-on shape (now blocked on cycle-32 redo with composite capture confirmed armed + downstream cycle 33 work), §G.5, RT-as-texture.

## 2026-05-23 (cycle 31 Path A.4 option (d) on-screen visual breadcrumb — bounded code slice CLOSED on `apple-silicon-performance`) — implementation + Codex-validated; cycle-32 real-Xbox run is Hermes's call

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "ship the cycle-31 option (d) on-screen visual breadcrumb infrastructure so Hermes can later schedule the cycle-32 real-Xbox discriminator run from durable docs." Real-Xbox deployment is NOT in this slice.

**Why this cycle ran.** Cycle 30 closure (commit `dfe1480cba`) observed outcome E2 (`witness.scan = D-cycle-27` AND `witness.scan-self = count=0`); (γ) "main() never reaches the fire calls" is LEADING; cycle-22 pre-main-crash hypothesis re-strengthened toward leading but not fully corroborated — `main()` could equally well crash AFTER entering but BEFORE the first fire (between cycle-25's host-log breadcrumb write and `xbed_a4_witness_fire(MAIN_ENTERED)`). The cycle-29 closure's option catalog promoted option (d) on-screen visual breadcrumb as the cycle-31 leading candidate (option (b) DEMOTED because α-vs-β is moot given γ leading). Cycle 31 ships option (d).

**What this slice contains (all under `scripts/apple-silicon/xbe-tests/witness-only/`).**

1. **`main.c` modified.** Cycle-31 head-comment addendum (~50 LOC) explaining the design + ordering rationale + γ-discriminator scope + γ.0/γ.1 sub-cases. Two new static helpers — `xbed_breadcrumb_init` (3-state machine UNTRIED / OK / FAILED; on first call attempts `XVideoSetMode(640, 480, 32, REFRESH_DEFAULT)`; on graceful FALSE return latches FAILED so `paint(1..4)` cannot re-enter the kernel display init path — Codex round-1 high finding adopted; on success clears the FB to opaque black via `memset` + `XVideoFlushFB`) and `xbed_breadcrumb_paint(stage)` (fills a 96-row band with the stage's color, then `XVideoFlushFB`). Five `xbed_breadcrumb_paint(N)` call sites in `main()`: paint(0) BEFORE the cycle-25 host-log line; paint(1) after `xbed_a4_witness_fire(MAIN_ENTERED)` return; paint(2) after `xbed_a4_witness_fire(POST_MARKER0)` return; paint(3) after `xbed_self_witness_fire(MAIN_ENTERED)` return; paint(4) after `xbed_self_witness_fire(POST_MARKER0)` return. Pre-reboot Sleep extended from cycle 25's 500 ms to 2 000 ms (cycle-31 rationale block: composite stream at ~30 fps captures ≥60 frames of deepest-painted state). Single new include is `<hal/video.h>` (+`<string.h>` for `memset`).

2. **`README.md` cycle-31 addendum.** 5-stripe color map; 8-row cycle-32 discriminator table (F1 / F2 / F3 / F4 / F5 / F6 / F7 / F8) keyed on the deepest visible stripe × `(witness.scan, witness.scan-self)` two-tuple; cycle-32 deployment runbook (10-step sequence covering composite-capture arm + cycle-30 canonical sequence + analyze step); cycle-31 build artifact sizes; cross-references updated.

3. **`manifest.json` updated.** Title extended ("+ cycle-31 option (d) on-screen visual breadcrumb"); purpose paragraph extended with cycle-31 design summary; new `real-xbox/physical/cycle-32` `expected_results` section enumerating F1..F8 with shape notes.

4. **Paired doc explicit invariants.**
   - F1 row says "main() ran past every checkpoint AND WTNS landed AND XCTR did NOT land (witness.scan still D-cycle-27)" — NOT "both mechanisms landed" (Codex round-1 high finding #2 adopted: F6 is the only outcome that means both succeeded; F1 means γ INVALIDATED + α/β remain live on the XCTR side).
   - F4 row says "γ.0 OR γ.1 OR graceful XVideoSetMode FALSE return" — all three land at F4 because the cycle-31 init-failed latch suppresses paint(0) on a graceful FALSE return (Codex round-1 high finding #3 adopted).
   - File banner header rewritten to acknowledge `XVideoSetMode` is now in scope (cycle-25 "NO XVideoSetMode" invariant no longer holds; remaining cycle-25 invariants — no pbkit / no NV2A class objects / no xbed_init / no file I/O — are intact; Codex round-1 low finding adopted).

**Build.** `witness-only/bin/default.xbe` 155 648 B (+4 096 B from cycle 29's 151 552 B; new code fits in one nxdk XBE page boundary). `witness-only.iso` 720 896 B (unchanged — same ISO sector boundary as cycle 29). Rebuilt cleanly via `eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && make`. Benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles.

**Local validation.** Build success + 2-round Codex green is the high-confidence verification appropriate for this slice. Standalone xemu cannot meaningfully exercise the cycle-31 discriminator question ("does `main()` execute far enough to emit a synchronous visible breadcrumb on REAL Xbox after an `XLaunchXBE` chainload?") — that question has no analog in xemu's `XLaunchXBE` emulation surface, where main() always runs. Real-Xbox deployment is cycle 32 scope.

**Codex validation (rule #15, mandatory — non-trivial diff ~250 lines C source + paired docs).**

- **Round 1 (changes mode) = MAJOR ISSUES** with 3 high + 1 low findings.
  - HIGH #1: orchestration-state files claimed cycle-31 canonical docs were synced, but `handoff.md` / `decision-log.md` / `validation-status.md` / `handoff-summary.md` still described cycle 30. **Adopted:** all four files actually synced this round (this entry is part of that fix).
  - HIGH #2: the F1 outcome row in `README.md` + `manifest.json` said "both witness mechanisms landed" when `witness.scan` stays D-cycle-27 — which means the XCTR witness did NOT land. **Adopted:** F1 wording rewritten to "WTNS landed; XCTR still D-cycle-27" with F6 reserved for "both mechanisms succeeded"; both surfaces updated.
  - HIGH #3: `xbed_breadcrumb_init` re-attempted `XVideoSetMode` on every paint call after a prior failure, broadening the risk surface beyond the "single XVideoSetMode call" claim and weakening the γ.1 interpretation. **Adopted:** init rewritten as 3-state machine (UNTRIED / OK / FAILED) so a graceful FALSE return latches FAILED and `paint(1..4)` cannot re-enter the kernel display init path; F4 wording updated to include graceful mode rejection.
  - LOW #4: file banner header still said "NO XVideoSetMode," which became false in cycle 31. **Adopted:** banner rewritten to acknowledge XVideoSetMode is in scope and enumerate the remaining cycle-25 invariants that still hold.

- **Round 2 = MAJOR ISSUES** with 1 new HIGH + 1 new LOW (all 4 round-1 findings RESOLVED).
  - HIGH #5: F4 row was self-contradictory — claimed it included "graceful XVideoSetMode FALSE return" alongside (γ.0)/(γ.1) AND claimed "cycle-22 pre-main FULLY CORROBORATED." A graceful FALSE return means `main()` DID execute past its first instruction, so it does NOT corroborate "pre-main" anything. **Adopted:** F4 rewritten to cover only γ.0 (`main()` never entered) and γ.1 (`XVideoSetMode` crash); new row F4' added for graceful-FALSE case (no stripes + `witness.scan-self count=1` + `witness.scan D-cycle-27`) — `main()` ran past paint(0) (no-op after latch FAILED) and through the cycle-29 self-witness fires, which still stamp the WTNS page. F4' INVALIDATES γ via the WTNS path; cycle 33 investigates AV-encoder rejection cause. Updated in README + manifest + handoff + decision-log + current-cycle + handoff-summary. Discriminator-table row count went from 8 to 9.
  - LOW #6: stripe-map prose stale ("All 5 stripes visible = the full witness path executed and the cycle-32 readback should be E1 shape") — but F1/F5/F6 give three different interpretations for "all 5 stripes visible" combined with different two-tuple shapes. **Adopted:** rewritten to "All 5 stripes visible = the full witness path through `main()` executed; the cycle-32 readback shape is then disambiguated by combining stripe count with the (witness.scan, witness.scan-self) two-tuple per the F1 / F5 / F6 rows of the cycle-32 discriminator table."

- **Round 3 = LOOKS GOOD.** Round-1 + round-2 findings all RESOLVED; no new findings. Validation marker written at `.claude/state/codex-validate-last-run`.

**Hypothesis status after cycle 31.**

- Hypotheses #1 / #2 / #3 unchanged (cycle 22 / 22 / 22 outcomes preserved).
- Cycle-22 leading hypothesis ("`witness-only`'s / `image-blit`'s `main()` does not execute its first fire-call instruction"): STILL RE-STRENGTHENED from cycle 30 toward LEADING; cycle 31 implementation-only does not change this. Cycle 32 readback (F4 vs F1..F3 + F5..F7) will move it.
- Hypothesis #5: PARTIALLY INVALIDATED catastrophic-hang sense unchanged. Subsumed-by-γ subtler sense remains.
- (α) "kseg0 scan can't find XCTR from non-agent context": STILL LIVE but DEPRIORITIZED unchanged (cycle-32 F1 outcome would re-elevate to LEADING).
- (β) "scan finds XCTR but write faults silently": same as (α).
- NEW (cycle 31, conditional on cycle 32): if cycle-32 readback shows stripe 0 visible OR is F4' (no stripes + `witness.scan-self count=1` = graceful XVideoSetMode FALSE + main() continued), γ is INVALIDATED and α-vs-β becomes the live discriminator question — re-elevating option (b) from the cycle-29 closure catalog. If cycle-32 readback is F4 (no stripes AND count=0), γ.0 OR γ.1 is the live cause and cycle-22 leading hypothesis is FULLY CORROBORATED in its strongest form.

**Hypothesis status conditional on cycle 32 (cycle-31 implementation-only does not change current state).** Cycle 31 ships the cycle-32 discriminator tool; cycle 32 will move hypotheses. If F1 / F2 / F3 / F4' / F5 / F6 / F7 (any stripe-0 visible OR F4' WTNS-success-without-stripe-0) → γ INVALIDATED → cycle-22 leading hypothesis WEAKENED (no longer the leading explanation); cycle 33 re-elevates option (b) for α-vs-β on the XCTR side. If F4 (no stripes AND count=0) → γ.0 OR γ.1 → cycle-22 leading hypothesis FULLY CORROBORATED in its strongest form; cycle 33 ships pre-main breadcrumbs.

**Cycle-32 candidate scope (NOT promoted by this session — Hermes's call).** FTP-deploy cycle-31 `witness-only/bin/default.xbe` (155 648 B; same path `/E/Apps/witness-only/default.xbe`; cycle-29 oracle-agent stays in place from cycle 30); ARM composite-capture leg via `scripts/apple-silicon/composite-record.sh cycle32-witness-only-screen` BEFORE issuing `runxbe`; run the cycle-30 canonical sequence (baseline both scans → reboot to dashboard → FTP-upload → relaunch agent → baseline both scans → composite-capture ARM → runxbe → poll FTP/21 + 9001 + ICMP → composite-capture STOP → ensure-agent → final both scans). Analyze composite recording with `scripts/apple-silicon/extract-keyframes.py` sampling near `t = runxbe_issued + 2s` (just before the 2 000 ms settle Sleep begins releasing for the reboot); classify deepest visible stripe per the cycle-31 stripe map; combine with `(witness.scan, witness.scan-self)` two-tuple per the 8-row F1..F8 table.

**Out of scope (kept bounded for cycle 31).** ZERO xemu-fork host source touched. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO `lib/xbed_self_witness.{c,h}` touched (cycle-29 self-witness shim intact). ZERO `lib/lib.mk` touched (cycle-29 opt-in policy intact). ZERO `oracle-agent/*` touched (cycle-27 preserve gate + cycle-29 witness.scan-self verb intact). ZERO `xbed_runtime.{c,h}` touched. ZERO image-blit touched. ZERO XBE rebuilds beyond `witness-only` itself. ZERO retail-title / §G.5 / RT-as-texture / second-wave-XBE work. ZERO flag default flips. ZERO PushNotification — bounded implementation slice, not blocker / milestone.

**Files touched (cycle 31).** `scripts/apple-silicon/xbe-tests/witness-only/main.c` (head-comment addendum + 2 static helpers + 5 paint sites + Sleep extension); `scripts/apple-silicon/xbe-tests/witness-only/README.md` (cycle-31 addendum + 5-stripe map + 8-row F1..F8 cycle-32 table + 10-step runbook + cross-references); `scripts/apple-silicon/xbe-tests/witness-only/manifest.json` (title + purpose + cycle-32 expected_results); `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` + `witness-only.iso` (rebuilt artifacts); `docs/apple-silicon/handoff.md` (this entry on top; cycle-30 entry preserved unchanged below); `docs/apple-silicon/decision-log.md` (cycle-31 entry on top; cycle-30 preserved unchanged); orchestration-state quartet closure pass. Three pre-existing untracked `.hermes_cycle*.txt` prompt files at repo root preserved un-staged per the rolling Hermes-supervision pattern.

## 2026-05-23 (cycle 30 Path A.4 real-Xbox deployment of the cycle-29 self-allocated-witness build vs the cycle-29 witness-only XBE) — OUTCOME E2; (γ) leading; cycle-22 pre-main-crash hypothesis re-strengthened

**Status: CLOSED on `apple-silicon-performance`. Bounded slice was "FTP-deploy cycle-29 oracle-agent + cycle-29 witness-only; run canonical cycle-26/28-style sequence extended with `witness.scan-self`; record discriminator readback." Result: SLICE RAN; outcome shape E2 as defined in cycle-29 closure docs and `witness-only/README.md` cycle-30 discriminator table. ZERO source/script code edits; ZERO XBE rebuilds; doc + evidence-file edits only.**

**Why this cycle ran.** Cycle 29 (closure commit `725bc97bbd`) shipped the cycle-29 self-allocated-witness infrastructure: new shared diag-XBE lib `lib/xbed_self_witness.{h,c}` allocating its own persistent `'WTNS'`-tagged page, plus a new read-only oracle-agent verb `witness.scan-self`. Cycle 29 was implementation-only; the Hermes-scheduled real-Xbox deployment slice is cycle 30. Cycle-29 closure laid out the outcome table: E1 (full success, D-cycle-27 + `WTNS count=1 0xA4000003 reserved1=2`), E1' (first-self-fire-only), E1'' (allocated-but-not-stamped), E2 (D-cycle-27 + `WTNS count=0`), E3 (A1/A2 + WTNS), E4 (hang), E5 (`WTNS count>=2`).

**What ran (chronological, file-backed in `benchmark-runs/cycle30-real-xbox-witness-only-self-20260523T103624Z/`).**

1. **Reachability + state probe (2026-05-23T10:36:29Z).** Xbox @ 192.168.0.200: ping 0% loss, FTP/21 CLOSED to anonymous probe (cycle-23/27 oracle-agent foreground holds the FTP server's anonymous slot via in-XBE listener), 9001 OPEN → cycle-27 oracle-agent (resident since cycle 28) still foreground.
2. **Agent banner + `help` (10:36:36Z).** `v0.4 (Phase 2 + controller.* + smc.*)`; only `witness.scan` registered (no `witness.scan-self`) — confirms resident agent is pre-cycle-29 (cycle-27 build).
3. **Baseline `witness.scan` against cycle-27 agent (10:36:45Z).** `count=1 buf.0 phys=0x03eb3000 virt=0x83eb3000 live=1 reserved0=0x00000000 reserved1=0x00000000 mapped_pages_seen=419`. `controller.buffer-info` cross-check: `addr=0x83eb3000 phys=0x03eb3000 size=120 magic=0x58435452 version=1 ports=4 anchor_ok=1`. **Precondition MET, matches cycle-26 + cycle-28 baselines exactly** (7th consecutive observation of this persistent phys in the same physical power session).
4. **Reboot to dashboard (10:36:53Z → confirmed up at 10:41:39Z by authenticated probe).** `reboot` RPC OK. Anonymous `curl --max-time 2 ftp://...` poll (cycle-26/28 methodology) timed out at +90s. Authenticated `curl -u xbox:xbox ftp://192.168.0.200/` confirmed dashboard at +218s — but FTP was likely up much earlier; the anonymous probe scored `530 login-required` as CLOSED. **Methodology lesson encoded for future cycles: use `-u xbox:xbox` and treat `226` as the dashboard-ready ground truth; `530` already means FTP service is alive.**
5. **FTP-upload cycle-29 oracle-agent (10:41:50Z → 10:42:12Z).** First `xbox-ftp-upload.py` STOR skipped with "same-size" reason — local + remote both 417 792 B (cycle-29 oracle-agent is the same size as cycle-27 because the new verb fits in the existing XBE page boundary). Forced re-upload with `--overwrite`; local SHA-256 `6fd215fff4f826b64bb3add84717d7d5e5d934aa6c9eb831b16130bdeb0a0395` landed; remote `default.xbe` mtime advanced. **Project-rule-#15-applicable gotcha:** `xbox-ftp-upload.py`'s default size-only diff cannot distinguish cycle-27 vs cycle-29 oracle-agent binaries. Future cycle redeploys MUST pass `--overwrite` when the size is unchanged.
6. **FTP-upload cycle-29 witness-only (10:42:22Z).** Single STOR succeeded (cycle-29 witness-only is 151 552 B vs cycle-25's 147 456 B — size mismatch is enough for the uploader to overwrite without `--overwrite`). Local SHA-256 `297f6eb90d1945090905b261eb22e70e181bee65fe200d9f2f843eb7d2504114`. Remote verified.
7. **`ensure-agent` launches cycle-29 build + verifies `witness.scan-self` registered (10:42:41Z → 10:42:55Z).** `SITE EXEC E:\Apps\oracle-agent\default.xbe → 200 EXEC command succeeded`; agent ready at 9001 within ~10 s. Banner unchanged (`v0.4 (Phase 2 + controller.* + smc.*)`) — expected (cycle-29 adds a verb, not a banner update). **`help` now lists `witness.scan-self                     enumerate kseg0 xbed_self_witness 'WTNS' pages + reserved[0,1] (cycle-29 option (c) readback)` → confirms cycle-29 build is foreground.**
8. **Baseline `witness.scan` AND `witness.scan-self` against cycle-29 agent (10:43:06Z).** `witness.scan`: `count=1 phys=0x03eb3000 reserved0=0 reserved1=0` (matches baseline; persistent agent buffer preserved across reboot + relaunch). `witness.scan-self`: `count=0 mapped_pages_seen=419` (no WTNS pages yet, expected — fresh state for cycle-29 self-witness). `controller.buffer-info` identical. **BOTH hard preconditions MET.**
9. **`runxbe E:\Apps\witness-only\default.xbe` + multi-probe poll (10:43:25Z, t+0).** `200 launching E:\Apps\witness-only\default.xbe`; agent died on chainload. Polled FTP/21 (authenticated `curl -u xbox:xbox`) + port 9001 (nc -z -w 1) + ICMP ping every ~1 s for 180 s timeout. **Dashboard FTP/21 returned `226` at t+39s.** 9001 never re-opened on its own (expected — chainload exits to dashboard, agent must be re-launched).
10. **Post-run: `ensure-agent` (cycle-29 build) + final both scans (10:44:31Z).** `SITE EXEC` OK; agent ready ~10 s. **Final `witness.scan`: `count=1 buf.0 phys=0x03eb3000 reserved0=0x00000000 reserved1=0x00000000`** — identical to baseline; no XCTR A.4 stamp landed (D-cycle-27, same shape as cycle 28). **Final `witness.scan-self`: `count=0 mapped_pages_seen=419`** — no WTNS page allocated/found anywhere in scanned kseg0 [0x80010000, 0x84000000]. `controller.buffer-info` identical to baseline. **Outcome E2 confirmed.**

**Outcome.**

**E2** per cycle-29 closure and `witness-only/README.md` cycle-30 discriminator table: `witness.scan = D-cycle-27 (count=1 reserved0=0 reserved1=0)` AND `witness.scan-self = count=0`. **(γ) "main() never reaches the fire calls" is LEADING.** The cycle-29 self-witness has no XCTR dependency — it calls `MmAllocateContiguousMemoryEx` directly with parameters bit-identical to the agent's known-good `s_allocate_fresh`. If `xbed_self_witness_fire` had been called at all, one of E1 / E1' / E1'' would have shown up (full stamp / first-fire-only / allocated-but-not-stamped). `count=0` instead means **no `MmAllocateContiguousMemoryEx` from the cycle-29 self-witness was ever called from `witness-only`'s `main()`**. The cycle-22 leading hypothesis (`witness-only`'s / `image-blit`'s `main()` does not execute its first fire-call instruction) is **RE-STRENGTHENED from "weakened" toward "leading"**, but not fully corroborated — `main()` could equally well crash AFTER its first instruction but BEFORE either fire site (between the host-log breadcrumb writes and `xbed_a4_witness_fire`). Cycle-31 option (d) (on-screen visual breadcrumb) is the next discriminator.

**Hypothesis status after cycle 30.**

- Hypothesis #1 (D:\\ remap mismatch): INVALIDATED cycle 22 — unchanged.
- Hypothesis #2 (NV2A early-init failure): STILL OPEN — unchanged.
- Hypothesis #3 (FATX/NT-mount divergence): INVALIDATED for D:\\ cycle 22 — unchanged.
- Cycle-22 leading hypothesis: **RE-STRENGTHENED** from weakened toward leading; not fully corroborated.
- Hypothesis #5: PARTIALLY INVALIDATED in catastrophic-hang sense (unchanged). The subtler "silently no-ops" sub-hypothesis is now subsumed by (γ) leading — moot.
- (α) (kseg0 scan can't find XCTR from non-agent context): STILL LIVE but DEPRIORITIZED — moot given (γ) leading.
- (β) (scan finds XCTR but write faults silently): STILL LIVE but DEPRIORITIZED — same reason.

**Cycle-30 timing observation (preserved as tracked open question, NOT load-bearing for the E2 conclusion).** Dashboard FTP recovery at **t+39s** is a NEW shape, distinct from cycle 26's 70.17 s + cycle 28's 70 s reproduced witness-only shape. Sits between mirror control (~36 s) and the 70 s prior shape. Possible readings (not discriminated by this single-sample run): faster early-crash path bypassing the witness lib `.text`, normal variance, or shifted crash site from +4 096 B of new linked-in code.

**Reproducibility checkpoints.** Kernel-pool deterministic phys=0x03eb3000 REPRODUCED across cycle-27 agent re-launch (start) + cycle-29 first launch + cycle-29 post-chainload launch = **≥9 consecutive observations** of this persistent phys in same physical power session (cycle 26: 3 + cycle 28: 3 + cycle 30: 3). `mapped_pages_seen=419` REPRODUCED at baseline + post-launch + post-run = 5th-7th observations across cycles 26 / 28 / 30 — kseg0 scan range covers the same physical pages each time. No physical power-cycle was needed at any point in cycles 26 / 28 / 30; the persistent buffer survives reboot-to-dashboard + chainload + relaunch across all three cycles.

**Cycle-31 candidate scope (NOT promoted by this session — Hermes's call).** **Option (d)** is the leading candidate: composite-capture during `witness-only` execution OR re-architect `witness-only` to emit a synchronous visual marker (pbkit-free `XVideoSetMode` + framebuffer-write breadcrumb) — would discriminate γ at the "did `main()` execute at all?" granularity. Option (b) (agent-side prior-phys dump + read-only kseg0 dump verb) was the α-vs-β discriminator; cycle 30 makes α-vs-β moot for now, so option (b) is DEMOTED. Cycle-22 leading hypothesis remains UNRESOLVED; cycle-31 option (d) is the path to break it.

**Out of scope (kept bounded for cycle 30).** ZERO xemu-fork host source touched. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO image-blit source touched. ZERO oracle-agent source touched. ZERO `lib/xbed_self_witness.{c,h}` touched. ZERO XBE rebuilds. ZERO retail-title / §G.5 / RT-as-texture / second-wave-XBE work. ZERO flag default flips. ZERO PushNotification — bounded run/doc slice, not blocker / milestone.

**Files touched / added (cycle 30).** Evidence directory `benchmark-runs/cycle30-real-xbox-witness-only-self-20260523T103624Z/` with 10 step logs + `SUMMARY.md` (gitignored per project convention); paired-doc updates to `docs/apple-silicon/handoff.md` cycle-30 entry on top with cycle-29 preserved unchanged, `docs/apple-silicon/decision-log.md` cycle-30 entry above cycle-29 (no supersession), and orchestration-state quartet closure pass. Two pre-existing untracked `.hermes_cycle22_path_a3_prompt.txt` + `.hermes_cycle23_docsync_prompt.txt` files at repo root preserved un-staged per the rolling Hermes-supervision pattern.

## 2026-05-23 (cycle 29 Path A.4 option (c) self-allocated witness — bounded code slice CLOSED) — implementation + Codex-validated; cycle-30 real-Xbox run is Hermes's call

**Status: CLOSED on `apple-silicon-performance`.** Bounded slice was "ship the cycle-29 option (c) self-allocated witness infrastructure so Hermes can later schedule the cycle-30 real-Xbox discriminator run from durable docs." Real-Xbox deployment is NOT in this slice.

**Why this cycle ran.** Cycle 28 closure (commit `c77b509149`) collapsed the cycle-26 ambiguity to "no A.4 stamp landed on the agent's XCTR buffer" with three live causes:
- (α) `xbed_a4_witness::a4_candidate_ok` kseg0 scan doesn't find the agent's XCTR buffer from a non-agent process context.
- (β) Scan finds it but the write faults silently (PAT/WC/WB attribute divergence, cache line never drains).
- (γ) `witness-only`'s `main()` never reaches the fire calls (cycle-22 leading hypothesis re-strengthens).

Cycle-28 closure recommended option (c) from the cycle-27 catalog as the cheapest γ-discriminator. Cycle 29 ships it.

**What this slice contains (all under `scripts/apple-silicon/xbe-tests/`).**

1. **New shared diag-XBE lib `lib/xbed_self_witness.{h,c}`** — `xbed_self_witness_fire(stage)`. First call allocates a persistent contiguous page via `MmAllocateContiguousMemoryEx(0x1000, 0x00010000, 0x03ffffff, 0x1000, PAGE_READWRITE)` + `MmPersistContiguousMemory(p, 0x1000, TRUE)` (identical allocator parameters to `oracle-agent/controller.c::s_allocate_fresh`), zeroes the full page, stamps magic = `'WTNS'` (0x534E5457) at offset 0 + version 1 at offset 4. Then (idempotent on subsequent calls) writes `reserved0 = (0xA4 << 24) | (stage & 0x00FFFFFF)` at offset 8 + increments `reserved1` (call counter) at offset 12. `wbinvd` after every stamp. Returns phys on success, 0 on hard failure (no fallback path — the cycle-29 value prop is that it allocates its own page). Header doc encodes: 16-byte layout symmetric with `oracle_ctrl_buffer` so the agent's scanner can reuse `reserved0`/`reserved1` extraction; safety pattern lifted from cycle 23; cycle-29 positioned narrowly as a (γ)-only discriminator (α+β remain live on a successful readback because this witness stamps a self-owned page, not the agent's XCTR page).

2. **`witness-only/main.c`** — adds two `xbed_self_witness_fire` calls AFTER the existing cycle-23 fires (ordering decision, Codex round-1 high finding #1 adopted: the cycle-23 path therefore runs under conditions bit-identical to cycle 25 up to and including the second cycle-23 fire, so the cycle-30 XCTR readback is properly comparable to cycle 28's D-cycle-27 result). Calls: `xbed_self_witness_fire(XBED_A4_STAGE_MAIN_ENTERED)` then `xbed_self_witness_fire(XBED_A4_STAGE_POST_MARKER0)`. Host-log breadcrumbs `witness-only: self-fire1 returned phys=…` + `witness-only: self-fire2 returned phys=…`. Existing cycle-23 fires + cycle-25 host-log lines + 500 ms inter-fire `Sleep` are PRESERVED VERBATIM.

3. **`witness-only/Makefile`** — adds `SRCS += $(XBED_LIB_DIR)/xbed_self_witness.c` opt-in line. The new file is INTENTIONALLY NOT added to `lib/lib.mk`'s default SRCS (Codex round-1 low finding #3 adopted), so the rest of the diag-XBE corpus is unaffected by cycle 29.

4. **`oracle-agent/commands.{c,h}` + `oracle-agent/main.c`** — new read-only verb `witness.scan-self` (mirror of `cmd_witness_scan` gated to `'WTNS'` magic). Uses the same `MmGetPhysicalAddress` per-page safety gate, the same kseg0 scan range [0x80010000, 0x84000000], the same 4 KiB stride, the same plausibility predicate `(reserved0==0 && reserved1==0) || ((reserved0>>24)==0xA4 && 1<=reserved1<=4096)`. Emits `buf.N phys=… virt=… reserved0=… reserved1=…` lines + a `count=N mapped_pages_seen=M` summary. Registered in `s_cmds[]` and added to `cmd_help` output.

5. **Paired doc edits.** `witness-only/README.md` adds a cycle-29 addendum + a 7-row cycle-30 discriminator table covering E1 (full success), E1' (first-self-fire-only partial), E1'' (allocated-but-not-stamped), E2 (γ leading), E3 (both mechanisms work), E4 (hang), E5 (accumulated orphans); the table is explicitly labelled "representative, NOT exhaustive" with the reader's `(0xA4-tagged, 1≤r1≤4096)` acceptance rule called out so operators can interpret novel shapes (e.g. `0xA4000003 reserved1=1`) correctly. `witness-only/manifest.json` adds a `real-xbox/physical/cycle-30` expected_results section enumerating E1/E1'/E1''/E2/E3/E4/E5 with shape notes; the title + purpose + `artifacts.witness_readback.notes` are extended to describe the cycle-29 readback path. `oracle-agent/commands.c::cmd_witness_scan_self` body comment encodes the full cycle-30 expected-shape table inline. `lib/lib.mk` carries a comment explaining why xbed_self_witness.c is intentionally NOT in default SRCS. The cycle-23 lockstep contract between `lib/xbed_a4_witness.c::a4_candidate_ok`, `oracle-agent/commands.c::a4_reader_candidate_ok`, and `oracle-agent/controller.c::s_page_has_plausible_witness_header` is UNCHANGED.

**Build.** `oracle-agent/bin/default.xbe` 417 792 B (size unchanged from cycle 28; new verb fits in existing XBE page boundary). `oracle-agent.iso` 983 040 B (unchanged). `witness-only/bin/default.xbe` 151 552 B (+4 096 B = +1 page from cycle 25's 147 456 B — the new `xbed_self_witness.c` linked-in code). `witness-only.iso` 720 896 B (unchanged — same ISO sector boundary). Both rebuilt cleanly via `eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && make`. Benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats prior cycles.

**Local validation.** Build success + 4-round Codex green is the high-confidence verification appropriate for this slice. Standalone xemu does not deterministically reproduce real-Xbox kernel-pool reuse, so a local xemu-Metal run can only exercise the allocation path (which is the same primitive cycle 23's oracle-agent uses and which is known-good there); it cannot answer the cycle-30 discriminator question. Real-Xbox deployment is cycle 30 scope.

**Codex validation (rule #15, mandatory — non-trivial diff = ~340 lines across the new shim + new agent verb + witness-only main.c + paired docs).**

- **Round 1 (changes mode) = MAJOR ISSUES** with 3 findings.
  - HIGH #1: "strictly additive" claim was misleading because the original cycle-29 ordering placed the self-witness fires BEFORE the cycle-23 fires, introducing kernel-allocator activity that confounded the cycle-25 baseline.
  - HIGH #2: the discriminator over-claimed by saying a `witness.scan-self` hit makes (α) the leading explanation; in fact (β) "scan finds XCTR but write faults silently" REMAINS LIVE because the self-witness stamps a self-owned page, not the agent's XCTR page.
  - LOW #3: `xbed_self_witness.c` was added to `lib/lib.mk` default SRCS, which would have pulled the new code into every diag XBE rebuild and widened controlled-delta drift across the test corpus.
  - **All 3 adopted in full.** Ordering swapped so cycle-29 fires run AFTER cycle-23 fires (the cycle-23 path is now bit-identical to cycle 25 through the second cycle-23 fire). All five doc surfaces (commands.c body comment, manifest.json, README.md, xbed_self_witness.h, main.c head comment) rewritten to position cycle 29 as a (γ)-only discriminator with α+β remaining live and the cycle-30 follow-up route to option (b) for α-vs-β discrimination. `xbed_self_witness.c` opted in only by `witness-only/Makefile`.

- **Round 2 = MINOR ISSUES.**
  - LOW #1 (round-1 HIGH #1 PARTIAL): `xbed_self_witness.h` still carried the "strict superset" language. **Adopted:** header doc text harmonized with the rest (additive but NOT a strict superset; bit-identical only through the second cycle-23 fire).
  - LOW #2 (new): operator-facing tables (README + manifest) missed partial-success shapes the reader tolerates: `0xA4000001/1` (first-self-fire-only), `(0,0)` (allocated-but-not-stamped), `count>=2` (accumulated orphans). **Adopted:** cycle-30 table expanded to 7 rows (E1/E1'/E1''/E2/E3/E4/E5) in both surfaces.
  - Round-1 LOW #3 confirmed RESOLVED. Round-1 HIGH #2 confirmed RESOLVED.

- **Round 3 = MINOR ISSUES.**
  - LOW (round-2 LOW #2 PARTIAL): the tolerated `reserved0=0xA4000003 reserved1=1` (second-self-fire-only) edge case still wasn't called out. **Adopted:** README + manifest now explicitly say the table is "representative not exhaustive" and document the tolerated shape + the reader's acceptance rule.
  - Round-2 LOW #1 confirmed RESOLVED.

- **Round 4 (confirmation) = LOOKS GOOD.** Round-3 LOW RESOLVED. No new findings. Validation marker written at `.claude/state/codex-validate-last-run`.

**Hypothesis status after cycle 29.**
- (α) STILL LIVE — cycle-30 (E1) will keep α live but invalidate γ; cycle-31 option (b) is the discriminator.
- (β) STILL LIVE — cycle-30 cannot move β; cycle-31 option (b) is the discriminator.
- (γ) STILL LIVE this session — cycle-30 will move it (INVALIDATED on any WTNS hit; LEADING on `count=0`).
- Cycle-22 leading hypothesis: STILL WEAKENED (unchanged from cycle 28).
- Hypothesis #5: STILL PARTIALLY INVALIDATED in the catastrophic-hang sense (unchanged from cycle 28; cycle 29 does not run on real Xbox so cannot move it).

**Cycle-30 candidate scope (NOT promoted by this session — Hermes's call).** Re-deploy the cycle-29 `oracle-agent/bin/default.xbe` + cycle-29 `witness-only/bin/default.xbe` via FTP. Canonical cycle-26-style sequence extended with `witness.scan-self` at baseline + post-run. Hard preconditions: baseline `witness.scan count=1 live=1 reserved0=0 reserved1=0` AND baseline `witness.scan-self count=0` (power-cycle Xbox first if either condition fails). Expected outcomes E1/E1'/E1''/E2/E3/E4/E5 enumerated in `witness-only/README.md` cycle-30 discriminator table and `manifest.json` `expected_results.real-xbox/physical/cycle-30.notes`. On E1: γ INVALIDATED → cycle 31 option (b) for α-vs-β. On E2: γ LEADING → cycle 31+ option (d) on-screen breadcrumb. On E3: full re-validation. On E4: redesign. On E5: power-cycle between attempts.

**Out of scope (kept bounded for cycle 29).** ZERO xemu-fork host source touched. ZERO `lib/xbed_a4_witness.{c,h}` touched (cycle-23 lockstep contract intact). ZERO image-blit source touched. ZERO oracle-agent allocator/preserve-gate source touched (cycle-27 preserve gate intact; the new agent verb is a new function next to `cmd_witness_scan`, not a modification of it). ZERO retail-title / §G.5 / RT-as-texture / second-wave-XBE work. ZERO flag default flips. ZERO cycle-30 deployment. ZERO PushNotification — this is a bounded implementation slice, not a milestone / blocker.

**Files touched / added.**
- M `scripts/apple-silicon/xbe-tests/lib/lib.mk`
- A `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.h`
- A `scripts/apple-silicon/xbe-tests/lib/xbed_self_witness.c`
- M `scripts/apple-silicon/xbe-tests/oracle-agent/commands.h`
- M `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c`
- M `scripts/apple-silicon/xbe-tests/oracle-agent/main.c`
- M `scripts/apple-silicon/xbe-tests/oracle-agent/bin/default.xbe` (rebuild; size unchanged)
- M `scripts/apple-silicon/xbe-tests/oracle-agent/oracle-agent.iso` (rebuild; size unchanged)
- M `scripts/apple-silicon/xbe-tests/witness-only/Makefile`
- M `scripts/apple-silicon/xbe-tests/witness-only/main.c`
- M `scripts/apple-silicon/xbe-tests/witness-only/README.md`
- M `scripts/apple-silicon/xbe-tests/witness-only/manifest.json`
- M `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` (rebuild; +4 096 B)
- M `scripts/apple-silicon/xbe-tests/witness-only/witness-only.iso` (rebuild; size unchanged)

Two pre-existing untracked `.hermes_cycle22_path_a3_prompt.txt` + `.hermes_cycle23_docsync_prompt.txt` files at repo root preserved un-staged per the rolling Hermes-supervision pattern (consistent with cycles 26 / 27 / 28 handling).

## 2026-05-23 (cycle 28 Path A.4 real-Xbox deployment of the cycle-27 preserve-branch oracle-agent vs cycle-25 witness-only) — OUTCOME D-cycle-27; cycle-27 option (a) demonstrated insufficient; cycle-26 "wiped after landing" hypothesis INVALIDATED; "stamp never landed" conclusion now isolated

**Status: CLOSED. Bounded slice was "deploy cycle-27 oracle-agent + run canonical cycle-26-style sequence vs cycle-25 witness-only; record discriminator readback." Result: SLICE RAN; outcome shape **D-cycle-27** as defined in the cycle-27 closure's outcome table. ZERO source/script code edits; ZERO XBE rebuilds; doc + evidence-file edits only.**

**Why this cycle ran.** Cycle 27 (commit `df999e41ea`, closure `291b607a46`) shipped a `s_allocate_fresh` preserve-branch in the oracle-agent: if the kernel-pool buffer page returned to a relaunched agent already carries a plausible header (either the fresh-init shape `(0,0)` or the A.4-tagged shape `((reserved0>>24)==0xA4, 1<=reserved1<=4096)`), the agent keeps the header intact and clears only `port[]`. Cycle 28 was the Hermes-scheduled real-Xbox deployment slice for that build. Cycle-27 closure laid out the outcome semantics: A1 (`count=1 live=1 reserved0=0xA4xxxxxx`) = preserve branch retained live-buffer stamp on reused phys; A2 (`count>=2` stamped orphan) = legacy orphan shape on a different phys; D-cycle-27 (`count=1 live=1 reserved0=0`) = stamp never landed; B (cycle-24-style 928 s+ hang) = real-Xbox-incompatible.

**What ran (chronological, file-backed in `benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/`).**

1. **Reachability + state probe (2026-05-23T08:44:29Z).** Xbox @ 192.168.0.200: ping 0% loss, FTP/21 CLOSED, agent/9001 OPEN → cycle-23 oracle-agent (resident since cycle 26's chainload) was foreground XBE.
2. **Agent banner + `help` (08:44:43Z).** `v0.4 (Phase 2 + controller.* + smc.*)`; `witness.scan` verb registered.
3. **Baseline witness.scan (08:44:48Z).** `count=1 buf.0 phys=0x03eb3000 virt=0x83eb3000 live=1 reserved0=0x00000000 reserved1=0x00000000 mapped_pages_seen=419`. `controller.buffer-info` cross-check: `addr=0x83eb3000 phys=0x03eb3000 size=120 magic=0x58435452 version=1 ports=4 anchor_ok=1`. **Precondition MET; matches cycle-26 baseline exactly** (same phys, same mapped_pages_seen, same reserved bytes).
4. **Reboot to dashboard (08:45:04Z → t+27s).** `reboot` RPC → dashboard FTP-LIST OK at t+27s.
5. **FTP-upload cycle-27 oracle-agent (08:45:39Z..08:45:51Z).** Local `scripts/apple-silicon/xbe-tests/oracle-agent/bin/default.xbe` (417 792 B, mtime 2026-05-23T00:31:28Z) → `/E/Apps/oracle-agent/default.xbe` via FTP STOR; FTP LIST verifies 417 792 B remote.
6. **Confirm cycle-25 witness-only XBE still present (08:45:51Z).** FTP LIST `/E/Apps/witness-only/`: `default.xbe 147 456 B` — unchanged since cycle-26 deploy.
7. **ensure-agent launches cycle-27 build + post-launch rescan (08:46:17Z..08:46:35Z).** `SITE EXEC E:\Apps\oracle-agent\default.xbe → 200 EXEC command succeeded`; agent ready at 9001 within ~10 s. Banner unchanged (`v0.4 (Phase 2 + controller.* + smc.*)`) — expected, the cycle-27 change is internal allocator logic only. **Post-launch `witness.scan`: identical to baseline (`count=1 phys=0x03eb3000 reserved0=0 reserved1=0 mapped_pages_seen=419`)** — kernel pool returned the same persistent phys to the relaunched cycle-27 agent; preserve branch saw `(0,0)` predicate match (no observable difference from legacy branch on this case).
8. **runxbe witness-only + FTP-LIST poll (08:47:07Z).** `runxbe path=E:\Apps\witness-only\default.xbe → 200 launching`; agent died on chainload. Polled FTP/21 (via `curl --max-time 2 ftp://...`) + port 9001 (via `nc -z -w 1`) + ICMP ping every 1 s. **Dashboard fully ready at t+70s** — virtually identical to cycle-26's measured 70.17 s (two-cycle reproducibility of this 70-second timing shape).
9. **Post-run: ensure-agent (cycle-27 build) + FINAL `witness.scan` (08:48:34Z).** `SITE EXEC E:\Apps\oracle-agent\default.xbe → 200 EXEC command succeeded`; agent ready ~10 s. **Final `witness.scan`: `count=1 buf.0 phys=0x03eb3000 virt=0x83eb3000 live=1 reserved0=0x00000000 reserved1=0x00000000`.** `controller.buffer-info` cross-check identical to baseline. **Outcome D-cycle-27 confirmed.**

**Outcome.**

**D-cycle-27** (as defined in cycle-27 closure): `count=1 live=1 reserved0=0 reserved1=0` after the cycle-27 preserve-branch agent re-allocates the deterministic kernel-pool page phys=0x03eb3000. The cycle-27 preserve gate's STRICT predicate (`(reserved0==0, reserved1==0)` OR `((reserved0>>24)==0xA4, 1<=reserved1<=4096)`) would have preserved any header that `xbed_a4_witness.c` actually writes — `MAIN_ENTERED → reserved0=0xA4000001 reserved1=1`; `POST_MARKER0 → reserved0=0xA4000003 reserved1=2`. Both fall inside the predicate's second branch; if either landed, the bytes would have survived `s_allocate_fresh`'s preserve branch and shown up in the post-run `witness.scan` readback. The observed `(0,0)` therefore unambiguously means **no A.4-tagged stamp existed on the kernel-pool page at the time the cycle-27 agent re-allocated it.**

**What outcome D-cycle-27 RESOLVES.**

- The cycle-26 indistinguishable-causes pair "stamp landed and got wiped vs stamp never landed" collapses on the wiped side: **stamp-landed-then-wiped hypothesis INVALIDATED**. The cycle-27 preserve branch is correctly wired up (Codex 3-round green at cycle-27 closure), agent restarts cleanly across the chainload, kernel-pool reuse is deterministic and the same phys=0x03eb3000 is returned every time — all preconditions for the cycle-27 discriminator to surface a preserved stamp are met. The stamp simply was never there.
- Cycle-27 option (a) is therefore **demonstrated insufficient** for breaking the cycle-26 ambiguity to A1/A2. Option (a) was the correct LOW-risk LOW-LOC first pick (per the cycle-26 closure's design-candidate ranking), but the underlying failure mode is upstream of `s_allocate_fresh`.
- Cycle-22 leading hypothesis ("image-blit crashes BEFORE main()'s first instruction") status: still **WEAKENED**. Cycle 28 evidence is equally consistent with "main() runs and fires no-op silently" and "main() never reached." Cycle-29 instrumentation is needed to discriminate.

**What outcome D-cycle-27 LEAVES OPEN ("stamp never landed" branches).**

- **(α)** `xbed_a4_witness.c::a4_candidate_ok`'s kseg0 scan does not find the agent's XCTR buffer on real Xbox from a non-agent process context. The buffer is at virt=0x83eb3000 / phys=0x03eb3000 with the right magic/version + zero reserved fields (matches the cycle-23 scan filter on paper). Real-Xbox `MmGetPhysicalAddress` behavior on a `MmPersistContiguousMemory`-tagged page from a different process is the unknown.
- **(β)** Scan succeeds but the write itself faults silently (PAT/WC/WB attribute divergence between the agent's mapping of the page and the non-agent process's mapping; or kseg0-direct write going to a stale cache line that never gets flushed before the kernel pool reuses the page).
- **(γ)** `witness-only`'s `main()` never reaches the fire calls. The linked `xbed_a4_witness.c` `.text` could itself trigger a slow pre-main fault. Both the cycle 26 (~70.17 s) and cycle 28 (~70 s) recovery shapes are consistent with this and with (α)/(β); none of those reads imply main() actually ran.

**Reproducibility checkpoints.**

- **70 s chainload→dashboard-ready for witness-only:** reproduced TWICE now (cycle 26 ~70.17 s + cycle 28 ~70 s). Confirmed real Xbox behavior for this XBE, not a one-off measurement artifact.
- **Kernel-pool deterministic reuse of phys=0x03eb3000:** at least 3 reuses observed in this cycle-28 session alone (cycle-23 agent resident → cycle-27 first launch → cycle-27 post-chainload launch), on top of cycle-26's 3 observations. Combined ≥6 consecutive reuses in the same physical power session. The cycle-25/26 design assumption "different phys per agent restart" continues to be empirically false on this Xbox.
- **`mapped_pages_seen=419`:** reproduced at baseline + post-launch + post-run (cycle 28) and at baseline + post-launch (cycle 26). The kseg0 scan range covers the same physical pages each time — argues against (α) in the "scan range too narrow" sub-form.

**Hypothesis status after cycle 28.**

1. Hypothesis #1 (D:\\ remap mismatch under `runxbe` chainload): INVALIDATED cycle 22 — unchanged.
2. Hypothesis #2 (NV2A class-object instantiation mismatch / `pb_agp_access` divergence / generic early-init failure): STILL OPEN.
3. Hypothesis #3 (FATX/NT-mount divergence): INVALIDATED for D:\\ cycle 22 — unchanged.
4. Cycle-22 leading hypothesis (image-blit crashes BEFORE main()'s first instruction): **still WEAKENED**. Cycle 28 cannot move it — equally consistent with "main() runs and fires no-op silently" (which would be a `xbed_a4_witness`-specific issue + would also apply to image-blit's witness calls) and with "main() never reached" (which would corroborate cycle 22).
5. NEW (cycle 24) hypothesis #5 (kseg0-scan witness mechanism real-Xbox-unsafe from non-agent process context): **REFINED**. Catastrophic-hang sense remains PARTIALLY INVALIDATED (cycle 28 reproduces the 70 s recovery, not the 928 s+ hang). The subtler "silently no-ops" sense is now consistent with cycle-28 evidence but cannot be distinguished from (γ) without further instrumentation.

**Cycle 29 candidate scope (NOT executed this session — Hermes's call).** Cycle-27 option (a) is DEMONSTRATED INSUFFICIENT. Adopt one of:

- **(c) [recommended]** Witness-only allocates its OWN page via `MmAllocateContiguousMemoryEx` with a unique magic tag (separate from XCTR). No reliance on kseg0 scan finding the agent's buffer. Discriminates (α) from (γ): the agent's `witness.scan` scanner (which is the same code as the writer scanner) IS known-good at finding XCTR buffers (we use it every session). If a unique-magic page is findable by an analogous read-only scanner after witness-only runs, then witness-only DID reach main() and DID successfully write to a self-allocated page — collapsing (α) into "agent buffer not findable by witness-only's scan (from non-agent context)" as the cycle-26/28 blocker. If even (c) lands nothing, (γ) ("witness-only never reaches main()") becomes leading.
- **(d)** On-screen visual breadcrumb captured via `oracle-orchestrator.py capture` mid-run OR composite capture during witness-only execution. Specifically discriminates (γ): if a known-pattern breadcrumb appears on screen, witness-only's main() did execute. Requires re-architecting witness-only to emit a synchronous visual marker without pulling in pbkit (or accepting a minimal pbkit dependency). On real Xbox there is no host-log channel readback equivalent to `XEMU_GUEST_LOG=1`, so on-screen is the only mid-run signal available.
- **(b)** Agent-side dump of prior controller-buffer phys+reserved[] to a known file before chainload + read-only kseg0 dump verb. Less promising than (c) because the cycle-26/28 evidence already shows the agent's own scanner finds the buffer reliably; the question is what the non-agent process sees.

Cycle 29 should also attempt to specifically discriminate the cycle-26/28 ~70 s recovery shape (slow kseg0 scan vs delayed-fault watchdog window vs slow BIOS POST). Option (c)'s own-page approach by itself may collapse that ambiguity if a successful (c) run shows BIOS-POST-like timing (~17 s) rather than 70 s — that would imply 70 s is specifically caused by the kseg0-scan path itself or by a delayed-fault watchdog activated by that path.

**Codex validation (rule #15).** Cycle 28 is **run-only / doc-only** — ZERO source/script code edits, ZERO XBE rebuilds, all Xbox-side operations used existing agent verbs and existing Mac-side tooling (`oracle-client.py`, `oracle-orchestrator.py`, `curl FTP`). Rule #15 trigger #2 (non-trivial uncommitted code in xemu-fork/) does NOT fire. The rule #15 doc-only / ≤30-line uncommitted source diff carve-out applies. Same path as cycle 26.

**Cycle 28 closure.** Closure commit (this commit) covers: this handoff.md entry (cycle-28 on top, cycle-27 preserved unchanged below), `decision-log.md` cycle-28 entry above cycle-27 (no supersession), orchestration-state quartet refresh (current-cycle.md, claude-status.md, validation-status.md, handoff-summary.md). NO source/script files touched.

**Cycle 28 evidence preserved on disk** (`benchmark-runs/cycle28-real-xbox-witness-only-preserve-20260523T084331Z/`, gitignored per project convention):

- `00-reachability.log` — initial Xbox probe.
- `01-agent-info.log` — agent banner + `help` (verifies `witness.scan` registered on resident cycle-23 build).
- `02-baseline-witness-scan.log` — baseline + `controller.buffer-info` (precondition MET).
- `03-reboot-to-dashboard.log` — `reboot` + FTP-LIST poll (t+27s).
- `04-ftp-upload-oracle-agent.log` — STOR cycle-27 oracle-agent + FTP LIST verify.
- `05-ftp-list-witness-only.log` — confirm cycle-25 witness-only XBE still resident.
- `06-relaunch-cycle27-agent.log` — `ensure-agent` (cycle-27 build) + post-launch rescan.
- `07-chainload-witness-only-ftp-list.log` — `runxbe witness-only` + FTP-LIST poll (t+70s).
- `08-postrun-witness-scan.log` — **FINAL discriminator readback (D-cycle-27).**
- `SUMMARY.md` — full evidence-summary writeup with hypothesis analysis.

**Out-of-scope (cycle 28 kept bounded).**

- Did NOT modify `oracle-agent/` source.
- Did NOT modify `lib/xbed_a4_witness.{c,h}`.
- Did NOT modify witness-only XBE source.
- Did NOT modify image-blit.
- Did NOT rebuild any XBE.
- Did NOT pursue cycle-29 options (b)/(c)/(d).
- Did NOT re-run cycle-26's invalid-path or mirror controls (already characterized at 20.67 s and ~36 s respectively).
- Did NOT issue a PushNotification — outcome is a definitive partial-discriminator result, not a blocker / not a milestone; standard handoff via state-file review.
- Two pre-existing untracked `.hermes_cycle*.txt` prompt files at repo root preserved un-staged (consistent with cycle 26 + cycle 27 handling).

## 2026-05-23 (cycle 27 Path A.4 option (a) — `oracle-agent/s_allocate_fresh` preserves an existing witness header instead of memset-wiping it) — XBE source slice CLOSED on `apple-silicon-performance`

**Status: CLOSED. Bounded slice was "implement cycle-27 option (a): preserve an existing oracle controller buffer witness stamp across agent restart by modifying `oracle-agent/controller.c::s_allocate_fresh`." Result: SHIPPED with Codex 3-round validation green (round 3 = LOOKS GOOD). No real-Xbox run this session — cycle 28 is Hermes's call.**

**Why this cycle ran.** Cycle 26 (commit `a31e061144`) closed with a CONCRETE partial-discriminator outcome (shape D, not in the cycle-25 design table): chainload→dashboard-ready in ~70 s (vs ~36 s for known-good mirror chainload and ~20.67 s for invalid-path), no orphan observable across 4 chainloads. The cycle-26 closure documented 4 candidate cycle-27 designs to break the stamp-vs-no-stamp ambiguity; option (a) was the LOW-risk LOW-LOC pick. The other options remain on the table but are not exercised here.

**Slice.** Three source files modified, no host-source / image-blit / witness-only-source touched:

1. `scripts/apple-silicon/xbe-tests/oracle-agent/controller.c` (+97 lines including comments): new static helper `s_page_has_plausible_witness_header(vp)` mirrors the cycle-23 lockstep plausibility filter shared by `lib/xbed_a4_witness.c::a4_candidate_ok` + `oracle-agent/commands.c::a4_reader_candidate_ok`, but tightened to accept ONLY the two header shapes that the agent's writer paths actually produce (Codex round-1 medium): `(reserved0==0, reserved1==0)` for a freshly initialized buffer, OR `((reserved0 >> 24) == 0xA4, 1 <= reserved1 <= 4096)` for a cycle-23 A.4-stamped buffer. The cycle-23 scan filter intentionally stays wider; the preserve gate is a strict subset because a false negative here just falls back to legacy full-zero behavior (safe), whereas a false positive would silently retain garbage as if it were a real witness header. `s_allocate_fresh` then branches: if the page already carries a plausible header, `memset(&vp->port[0], 0, sizeof(vp->port))` clears only the synthetic-input payload while leaving magic/version/reserved[0,1] intact; otherwise legacy full-zero + re-stamp magic/version. `cache_writeback_invalidate()` after either branch. One conditional `debugPrint("oracle_ctrl: preserved existing witness header at phys=… reserved0=… reserved1=…\n")` when the preserve branch fires, so an on-screen (composite-capture or debug-overlay) verification can confirm the branch activated without `witness.scan`.

2. `scripts/apple-silicon/xbe-tests/oracle-agent/controller.h` (+17 lines doc only): the `oracle_ctrl_init` doc comment now explains the cycle-27 preserve refinement so a future maintainer reading the header sees the contract.

3. `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c` (+26 lines doc only): `cmd_witness_scan` body comment extended with a "Cycle-27 additional success shape" block. With cycle 27 active, EITHER the legacy orphan shape (count>=2 with a stamped orphan) OR the new live-buffer shape (`count=1 live=1 reserved0=0xA4xxxxxx` on the reused phys) is a positive "witness landed" outcome; `count=1 live=1 reserved0=0` is the "no stamp landed" baseline.

Plus two paired doc updates in `scripts/apple-silicon/xbe-tests/witness-only/`:

4. `README.md` discriminator-semantics table now lists both success surfaces (orphan or live preserve) and clarifies "cycle 28" is the follow-on real-Xbox run.

5. `manifest.json` `artifacts.witness_readback.notes` + `expected_results.real-xbox/physical/cycle-26.notes` both extended to describe both success shapes.

`oracle-agent/bin/default.xbe` rebuilt: 417 792 B (size unchanged from cycle-26). `oracle-agent.iso` rebuilt: 983 040 B. Benign `lld: warning: .edata=.rdata: already merged into .edataxb` repeats from prior cycles. No xemu-fork host source touched. No image-blit / witness-only source touched. No `lib/xbed_a4_witness.{c,h}` touched (cycle-23 scan filter intentionally stays wider than the cycle-27 preserve gate — the comment block in controller.c spells out why and points at the lockstep contract).

**Local validation.** Build green. Local xemu agent run was NOT performed this session — the agent's preserve branch can only be meaningfully exercised when it boots into a kernel-pool page that previously held a stamped buffer, which requires the cycle-25 `witness-only` XBE to have run first AND the kernel pool to have returned the same phys to the relaunched agent. Standalone xemu's `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory` semantics do not deterministically reproduce the real-Xbox kernel-pool reuse pattern observed in cycle 26, so a local validation run would prove only "agent still boots cleanly on a never-stamped page" (the non-preserve branch — already the path it's been exercising for every prior cycle). Build success + source review + Codex 3-round validation green is the high-confidence verification appropriate for this slice; the real-Xbox preserve-branch path is cycle-28 scope.

**Codex validation (rule #15, mandatory — non-trivial diff = 109+ lines on renderer-adjacent oracle-agent C source).** Three rounds:

- Round 1 (changes mode, diff ≈109 lines) = **MINOR ISSUES** with 1 medium + 1 low finding:
  - MEDIUM #1: `s_page_has_plausible_witness_header()` predicate was too loose — it accepted `(reserved0==0, reserved1>0)`, a header shape that the agent's writer / init paths never actually produce; accepting it would let a coincidental kernel-pool page mimic a "fresh buffer with a nonzero counter" and trigger the preserve branch on garbage. **Adopted in full:** predicate tightened to accept exactly `(0,0)` or `((reserved0 >> 24) == 0xA4, 1 <= reserved1 <= 4096)`.
  - LOW #2: `cmd_witness_scan` body comment, `witness-only/README.md` discriminator table, and `witness-only/manifest.json` expected_results all described only the legacy orphan-shape success surface — but cycle 27 makes the live-buffer shape (`count=1 live=1 reserved0=0xA4...`) a valid success outcome on the cycle-26-reused phys. An operator following the legacy guidance could misread that result as "no stamp landed." **Adopted in full:** cmd_witness_scan comment + README + manifest `artifacts.witness_readback.notes` extended with the cycle-27 live-buffer shape.

- Round 2 (changes mode, post-adoption diff) = **MINOR ISSUES**:
  - Round-1 MEDIUM: RESOLVED — predicate verified at `controller.c::s_page_has_plausible_witness_header` line 100.
  - Round-1 LOW: PARTIAL — `cmd_witness_scan` body comment, README, and manifest `artifacts.witness_readback.notes` all updated; HOWEVER manifest `expected_results.real-xbox/physical/cycle-26.notes` (separate field) still hard-coded the legacy "exactly 2 total instances / 1 NEW orphan" success shape. **Adopted:** that field now describes both shapes ("either (i) LEGACY ORPHAN SHAPE … OR (ii) CYCLE-27 PRESERVE-BRANCH SHAPE …").

- Round 3 (changes mode, post-PARTIAL-fix diff) = **LOOKS GOOD**:
  - Round-2 PARTIAL: RESOLVED — manifest `expected_results.real-xbox/physical/cycle-26.notes` verified at line 23.
  - Round-1 MEDIUM: STILL RESOLVED — preserve gate predicate verified intact.
  - No new round-3 findings.
  - Out-of-scope note (Codex): `docs/apple-silicon/decision-log.md` cycle-26 entry still describes the pre-cycle-27 orphan-only success table. That file is updated in this same commit (cycle-27 entry added above cycle-26, no supersession needed for cycle-26 entry which accurately describes the cycle-26 state of the world).

Validation marker written at `.claude/state/codex-validate-last-run`.

**Hypothesis status after cycle 27.**

1. Hypothesis #1 (D:\ remap mismatch): INVALIDATED cycle 22 — unchanged.
2. Hypothesis #2 (NV2A early-init failure): STILL OPEN — unchanged.
3. Hypothesis #3 (FATX/NT-mount divergence): INVALIDATED for D:\ cycle 22 — unchanged.
4. Cycle-22 leading hypothesis (image-blit crashes BEFORE main()'s first instruction): WEAKENED (carried from cycle 24+26; cycle 27 ships a discriminator-sharpening tool, cycle 28 runs it).
5. NEW (cycle 24) hypothesis #5: kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context. PARTIALLY INVALIDATED in the catastrophic-hang sense (carried from cycle 26); cycle 28 may further refine if the preserve branch surfaces a clear stamped-buffer readback.

**Out-of-scope (cycle 27 kept bounded).**

- NO host-source touched (no xemu-fork/{ui,hw,target,include,...}).
- NO image-blit source touched.
- NO witness-only source touched.
- NO `lib/xbed_a4_witness.{c,h}` touched.
- NO XBE rebuilds beyond `oracle-agent`.
- NO real-Xbox deployment — cycle 28 is Hermes's call.
- NO §H.6 / §G.5 / RT-as-texture / second-wave-XBE work.
- NO flag default flips.
- Did NOT delete or commit the two pre-existing untracked prompt files (`.hermes_cycle22_path_a3_prompt.txt`, `.hermes_cycle23_docsync_prompt.txt`).
- Did NOT pursue cycle-27 options (b), (c), or (d). They remain on the table for cycle 29+ if option (a) does not resolve the ambiguity.
- Did NOT issue a PushNotification — outcome is "tool shipped, ready for Hermes to schedule next real-Xbox slice"; standard handoff via state-file review.

**Next bounded slice (cycle 28 — Hermes's call).** Re-deploy the cycle-27 `oracle-agent/bin/default.xbe` (size 417 792 B; same as cycle-26 but the preserve branch is now live). Cycle-25 `witness-only/bin/default.xbe` UNCHANGED — reuse as-is. Sequence: power-cycle Xbox if multiple A.4-tagged buffers pre-exist; ensure-agent; baseline witness.scan precondition (count=1 live=1 reserved0=0); FTP-upload new oracle-agent; relaunch agent; runxbe witness-only; poll FTP/21 + agent/9001 + ICMP ping via `curl FTP LIST` (the cycle-26 ground-truth poll); on dashboard return, restart agent + query witness.scan. Cycle-28 expected positive shapes: (A1) count=1 live=1 reserved0=0xA4000003 — preserve branch retained the live-buffer stamp on the reused phys; (A2) count>=2 with a stamped orphan reserved0=0xA4000003 — legacy orphan shape if the kernel pool returned a different phys this time. Either A1 or A2 means cycle-22 leading hypothesis INVALIDATED. Outcome (D-cycle-27) = count=1 live=1 reserved0=0 — preserve branch never matched, the stamp never landed in the first place; cycle 29 promotes one of options (c) or (d). Outcome (B) = identical 928s+ hang to cycle 24 — hypothesis #5 promoted back; redesign required.

**Cycle 27 closure.** Closure commit (this commit) covers: 3 modified `oracle-agent/` source files (controller.c, controller.h, commands.c), 1 rebuilt `oracle-agent/bin/default.xbe` + 1 rebuilt `oracle-agent.iso`, 2 paired-doc edits under `witness-only/` (README.md, manifest.json), 6 canonical-doc edits (handoff.md cycle-27 entry on top, decision-log.md cycle-27 entry above cycle-26, orchestration-state quartet refresh).

## 2026-05-22 (cycle 26 Path A.4 real-Xbox witness-only deployment) — PARTIAL DISCRIMINATOR (outcome shape D); hypothesis #5 partially invalidated in the catastrophic-hang sense; stamp-vs-no-stamp ambiguity left open for cycle 27

**Status: CLOSED. Bounded slice was "Determine cycle-26 feasibility autonomously; if feasible, execute the canonical real-Xbox sequence; document outcome." Result: SLICE RAN with concrete partial-discriminator evidence. ZERO source/script code edits; ZERO XBE rebuilds; doc + evidence-file edits only.**

**Slice.** Cycle 25 shipped the `witness-only` diagnostic XBE (5 new files + 2 built artifacts; cycle-23 lib + agent + image-blit UNTOUCHED). Cycle 26 was the Hermes-scheduled real-Xbox deployment slice — Hermes left the Xbox in dashboard state (post-cycle-24 physical power-cycle confirmed by the `mapped_pages_seen=419` matching cycle-24 baseline exactly). Claude Code worker (this session) ran the canonical cycle-26 sequence autonomously from this Mac.

**What ran (chronological, file-backed in `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/`).**

1. **Reachability + state probe (2026-05-23T04:13:56Z).** Xbox @ 192.168.0.200: ICMP 0% loss, FTP/21 OPEN, agent/9001 CLOSED → dashboard state. Hermes had clearly physically power-cycled post cycle-24 (otherwise the Xbox would still be hung from cycle-24's 928 s+ silent state).
2. **ensure-agent + baseline witness.scan (04:14:31Z).** Agent up, banner `v0.4 (Phase 2 + controller.* + smc.*)`, `witness.scan` verb registered. Baseline scan: `count=1 buf.0 phys=0x03eb3000 virt=0x83eb3000 live=1 reserved0=0x00000000 reserved1=0x00000000 mapped_pages_seen=419`. **Hard precondition MET** (exactly 1 live `oracle_ctrl_buffer` with reserved[0]==0). `controller.buffer-info`: `addr=0x83eb3000 phys=0x03eb3000 size=120 magic=0x58435452 version=1 ports=4 port_state_size=26 anchor_ok=1`. Cross-check perfect.
3. **Reboot + FTP upload (04:15:40Z..04:15:45Z).** Agent reboot returned FTP/21 at +35 s. Uploaded `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` (147 456 B local) → `/E/Apps/witness-only/default.xbe`; FTP LIST confirms remote size 147 456 B.
4. **ensure-agent + baseline rescan (04:15:53Z..04:16:08Z).** Identical state to step 2 (consistent; soft reboot preserved persistent allocator state).
5. **runxbe witness-only #1 + post-run scan (04:16:30Z..04:17:45Z).** Post-run: identical to baseline (count=1 phys=0x03eb3000 reserved[0]=0 mapped_pages_seen=419). NO orphan.
6. **runxbe witness-only #2 + post-run scan (04:20:58Z..04:21:48Z).** Identical to baseline. NO orphan.
7. **Polling methodology investigation (04:22:29Z..04:24:45Z).** Tightened polling to 0.5 s and then 0.2 s intervals with `nc -z -w 1` on both port 21 and 9001. Observed apparent fast transitions (t+0.60 s ftp=Y 9001=N) that proved to be artifacts.
8. **CONTROL #1 (04:30:18Z..04:30:58Z): invalid path chainload.** `runxbe 'E:\Apps\does-not-exist\nope.xbe'` → dashboard-ready at t+20.67 s. Matches the agent's `XLaunchXBE`-failure fallback path: `op_send_okf + netconn_close + Sleep(500) + XLaunchXBE(fail) + Sleep(2000) + HalReturnToFirmware(HalRebootRoutine)` → BIOS POST ~17 s → dashboard. Baseline for "chainload didn't actually launch anything".
9. **CONTROL #2 (04:32:57Z..04:33:48Z): mirror chainload.** Known-good Tier-1 NV2A diag XBE. Apparent dashboard-ready at t+0.07 s (spurious `nc -z` artifact); FTP LIST confirmed dashboard actually ready ~36 s after chainload (04:34:24Z verified via real FTP LIST poll).
10. **Definitive timing via `curl FTP LIST` poll (04:34:55Z..04:36:23Z, log 20-).** `runxbe witness-only` + `for i ...; FTPOK=$(curl -sS --max-time 2 ftp://.../ >/dev/null && echo Y || echo N); ...` — first iteration t+0.07 s ftp_list=N 9001=N (Xbox in transition); transition to ftp_list=Y at **t+70.17 s**. Post-run ensure-agent + witness.scan returned identical state to baseline. **Confirms witness-only chainload→dashboard-fully-ready gap is ~70 s on this Xbox.**

**Outcome (cycle 26).**

**Outcome D** (not in cycle-25 design table):
- Chainload→dashboard-ready: **~70.17 s** (witness-only) vs **20.67 s** (invalid-path control) vs **~36 s** (mirror known-good control).
- Post-run `witness.scan` after EVERY chainload: identical to baseline (count=1, phys=0x03eb3000, reserved[0]=0). **No orphan observed in any of 4 chainloads.**

**What outcome D resolves.**

- **Outcome B (cycle-24-like hard hang) RULED OUT.** Xbox fully recovered to dashboard in 70 s without a physical power-cycle. Hypothesis #5 ("kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context") is **PARTIALLY INVALIDATED in the catastrophic-hang sense**: the mechanism does not hard-hang the Xbox in this minimal XBE.
- Witness-only's ~70 s is FUNDAMENTALLY DIFFERENT from invalid-path's ~20.67 s, ruling out the trivial "XLaunchXBE failed instantly" explanation. Witness-only IS being loaded by the kernel — there is real work being done that delays recovery beyond the normal reboot cycle.

**What outcome D leaves open.**

- **Stamp-vs-no-stamp ambiguity.** No orphan observed → either (i) witness fire never landed a stamp, OR (ii) stamp landed but the relaunched agent's `s_allocate_fresh::memset(vp, 0, sizeof(*vp))` at `oracle-agent/controller.c:181` wiped it after the kernel pool deterministically returned phys=0x03eb3000 across all 3 observed agent re-launches in the session (despite `MmPersistContiguousMemory`-tagged prior pages supposedly being excluded from re-allocation by the controller.c:201-208 comment "one persistent 4 KiB page per agent restart until the Xbox is power-cycled"). Session evidence cannot discriminate.
- **Cycle-22 leading hypothesis status: still WEAKENED.** The 70 s recovery is consistent with witness-only's main() running to HalReturnToFirmware AND consistent with witness-only never reaching its witness fires (the linked `xbed_a4_witness.c` `.text` could itself cause a slow pre-main fault). Session evidence cannot resolve.
- **~70 s delay shape.** Could reflect a slow kseg0 scan on real Xbox, a delayed-fault watchdog recovery window, or a slow BIOS POST after an exception. Cycle 27 must add an observable mid-run signal (visual breadcrumb / new agent verb / non-shared phys page) to discriminate.

**Hypothesis status after cycle 26.**

1. Cycle-19 hypothesis #1 (D:\\ remap mismatch under `runxbe` chainload): FULLY INVALIDATED (unchanged from cycle 22).
2. Cycle-21 hypothesis #2 (NV2A class-object instantiation mismatch / `pb_agp_access` divergence / generic early-init failure): STILL OPEN.
3. Cycle-21 hypothesis #3 (FATX-driver / NT-mount state divergence): INVALIDATED for D:\\.
4. Cycle-22 leading hypothesis (image-blit crashes BEFORE main()'s first instruction): WEAKENED (carried from cycle 24; cycle 26 does not resolve).
5. **NEW (cycle 24) hypothesis #5: the kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context. PARTIALLY INVALIDATED in the catastrophic-hang sense by cycle 26 — the mechanism does not hard-hang. The subtler "stamp landed vs didn't land" question is the cycle-27 target.**

**Methodology lesson (encoded for future cycle-26-style sessions).**

`nc -z -w 1 <host> <port>` returns spurious "port=open" readings during Xbox network-stack transitions (the agent dying / dashboard's lwIP listener half-rebinding). The reliable ground-truth signal for "dashboard fully ready" is `curl --max-time 2 ftp://xbox:xbox@<host>/` issuing a real FTP LIST and checking exit code. The earlier readings in this session (t+0.6 s, t+0.07 s) were artifacts; the FTP-LIST poll measured the actual recovery at ~70 s.

**Cycle 27 candidate scope (NOT executed this session; Hermes's call).**

To break the stamp-vs-no-stamp ambiguity, pick ONE:
- (a) Modify `oracle-agent/controller.c::s_allocate_fresh` to NOT `memset` when it finds an existing `XCTR + version==1` buffer at the returned phys (preserve any landed witness stamp).
- (b) Add an agent verb that dumps prior controller-buffer phys+reserved[] to a known file BEFORE chainloading; add a read-only kseg0 dump verb that doesn't re-initialize the buffer.
- (c) Use a fresh `MmAllocateContiguousMemoryEx` page from witness-only itself (separate magic tag, separate page) so it doesn't collide with the agent's allocator pattern.
- (d) Add an on-screen visual breadcrumb captured via `oracle-orchestrator.py capture` mid-run.

Cycle 27 should also discriminate the ~70 s delay: slow kseg0 scan vs delayed-fault watchdog vs slow BIOS POST. Adding host-log channel readback after `runxbe` (since `XEMU_GUEST_LOG=1` writes go to port 0xE9 which is host-only) is also worth considering — but that requires xemu host-log readback infrastructure that doesn't exist on real Xbox.

**Cycle 26 closure.** Closure commit (this commit) covers: orchestration-state quartet refresh (current-cycle.md, claude-status.md, validation-status.md, handoff-summary.md), this handoff.md entry, decision-log.md cycle-26 entry. NO source/script files touched.

**Cycle 26 evidence preserved on disk.**

- `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/00-reachability.log` — initial Xbox probe.
- `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/{01..09}-*.log` — first two chainload cycles, baseline scans, post-run scans.
- `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/{10..15}-*.log` — agent state, additional chainloads, polling-methodology investigation.
- `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/{16,17}-*.log` — invalid-path control + FTP listing.
- `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/{18,19}-*.log` — mirror control + post-mirror scan.
- `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/20-chainload-ftp-list-timing.log` — DEFINITIVE TIMING (FTP LIST poll, ~70 s).
- `benchmark-runs/cycle26-real-xbox-witness-only-20260523T041356Z/SUMMARY.md` — full evidence-summary writeup.

(`benchmark-runs/` is gitignored per project convention; consistent with cycles 19/20/21/22/24/25.)

**Out-of-scope (cycle 26 kept bounded).**

- Did NOT modify `oracle-agent/` source.
- Did NOT modify `lib/xbed_a4_witness.{c,h}`.
- Did NOT modify witness-only XBE source.
- Did NOT modify image-blit.
- Did NOT touch xemu-fork host source.
- Did NOT rebuild any XBE.
- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on or any other flag.
- Did NOT touch retail-title metrics, §G.5, RT-as-texture, or second-wave XBE work.
- Did NOT promote cycle 27 to "shall run." Cycle 27 is Hermes's call.
- Did NOT issue a PushNotification — outcome is concrete-but-partial; Hermes will see this on next state-file review and decide cycle-27 scope.

Cycle 25 entry preserved unchanged below.

---

## 2026-05-22 (cycle 25 Path A.4 witness-mechanism viability discriminator XBE) — `witness-only` diag XBE SHIPPED + Codex round-3 PASS_WITH_FINDINGS; cycle-26 real-Xbox deployment deferred to Hermes

**Status: SHIPPED (XBE source slice; cycle-23 lib + agent + image-blit untouched; no xemu-fork host source touched). Bounded slice CLOSED — implementation green, local xemu-Metal smoke green, Codex 3-round validation green. The cycle-26 real-Xbox deployment slice is explicitly Hermes's call.**

**Slice.** Cycle 24 promoted NEW hypothesis #5 ("the kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context") as top-priority discriminator candidate after cycle-23 image-blit hard-hung the Xbox for 928.3 s during the cycle-24 real-Xbox run. The cycle-24 handoff recommended a minimal "witness-only" diag XBE under `scripts/apple-silicon/xbe-tests/witness-only/` that fires the cycle-23 witness twice with sleep gaps and `HalReturnToFirmware(HalRebootRoutine)`s, with NO pbkit / NV2A / file I/O / xbed_init, so the discriminator answer would isolate the witness mechanism itself from the rest of image-blit's `main()` body. Cycle 25 ships exactly that. The bounded assignment was "ship the XBE so Hermes can later schedule the real-Xbox deployment slice from durable docs"; cycle 25 does NOT include the real-Xbox run.

**Design (locked 2026-05-22 22:42 CDT).** Five files under `scripts/apple-silicon/xbe-tests/witness-only/`:

1. `main.c` — ~10-statement `main()`: `xbed_host_log_write` anchor → `xbed_a4_witness_fire(XBED_A4_STAGE_MAIN_ENTERED)` → log fire1 return value → `Sleep(500)` → `xbed_a4_witness_fire(XBED_A4_STAGE_POST_MARKER0)` → log fire2 return value → `Sleep(500)` → anchor + `debugPrint` → `HalReturnToFirmware(HalRebootRoutine)`. Header documents the cycle-25 substitution: image-blit's intermediate `image_blit_marker(0, ...)` fopen is REPLACED with a passive `Sleep(500)`, so a clean cycle-26 outcome proves the witness mechanism is real-Xbox-safe IN THIS MINIMAL XBE but does NOT independently exclude the marker helper as a contributor to image-blit's hang. NO `XVideoSetMode`, NO `pb_init`, NO `xbed_init`, NO `fopen`, NO `image_blit_marker_*`. Includes `xbed_a4_witness.h` (cycle-23 witness header) and `xbed_runtime.h` (host-log channel) only.
2. `Makefile` — `lib.mk` include pattern identical to `image-blit/Makefile` so cycle-25 links the SAME `xbed_a4_witness.c` + `xbed_runtime.c` + sibling lib code that image-blit links. The linked-but-unused helpers (`xbed_init`, `xbed_capture_*`, `xbed_input_synth_*`, `xbed_texture_*`) only execute if called; their static `.text` cost is the controlled invariant between cycle-25 and cycle-21-image-blit binaries.
3. `manifest.json` — `real_xbox_only:true`, `oracle_priority:["real-xbox"]`, two record-only `expected_results` keys: one for the cycle-26 real-Xbox discriminator semantics (outcome A = `0xA4000003` orphan + clean reboot; outcome B = identical hang; outcome C = `0xA4000001` orphan + clean reboot), one for the cycle-25 local xemu-Metal smoke (no agent present → "no XCTR buffer found" returns + 0 phys; expected). Preconditions block lists the cycle-26 hard precondition (baseline `witness.scan` shows exactly 1 live buffer with `reserved[0]==0`) and the deployment path (`E:\Apps\witness-only\default.xbe`).
4. `README.md` — purpose / background / build / local smoke validation / cycle-26 deployment sequence / full discriminator-semantics table cross-referencing handoff.md cycle-24 + decision-log cycle-24 + cycle-23 lib files.
5. `.gitignore` — per peer-XBE convention (`*.obj`, `*.exe`, `*.c.d`, `*.cpp.d`, `__pycache__/`).

**Build.** `make` from `xbe-tests/witness-only/` with `eval "$(nxdk/bin/activate -s)"`. Outputs: `bin/default.xbe` = 147 456 B (compare: image-blit's `bin/default.xbe` = 159 744 B; pipeline-smoke's = 110 592 B), `witness-only.iso` = 720 896 B.

**Local xemu-Metal smoke validation.** `XEMU_GUEST_LOG=1 XEMU_RENDERER=METAL XEMU_PERF_LOG=1 XEMU_SNAPSHOT_NO_THUMBNAIL=1 XEMU_METAL_HUD=0 XEMU_METAL_VALIDATION=1` with a 25 s timeout. Counts from `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-20260523T034519Z/`:

- `witness-only: main() entered` × 9
- `xbed_a4_witness: enter stage=1` × 9
- `witness-only: fire1 returned phys=0x00000000` × 9
- `xbed_a4_witness: enter stage=3` × 9
- `witness-only: fire2 returned phys=0x00000000` × 9
- `witness-only: rebooting via HalReturnToFirmware(HalRebootRoutine)` × 8
- `xbed_a4_witness: no XCTR buffer found` × 18 (= 9 fires × 2 stages)

Cold-boot mapped_pages_seen=378; warm-reboot mapped_pages_seen=77 — matches cycle-23 image-blit numbers exactly. The fire1/fire2 `phys=0x00000000` outcome reflects "no agent running" — expected on standalone xemu (the agent would have to be running for there to be an `oracle_ctrl_buffer` to find). Loop count (9 vs 8 reboots, with the 9th main() entry cut off by timeout mid-iteration) confirms `HalReturnToFirmware(HalRebootRoutine)` works correctly on every iteration. Local validation only proves the mechanism's logic-level correctness in emulation; xemu cannot reproduce real-Xbox MMIO-aliasing failure modes.

**Codex validation (3 rounds).** Per rule #15, mandatory (cycle 25 ships non-trivial XBE source, NOT a doc-only carve-out).

- **Round 1 (changes mode):** MAJOR ISSUES, 4 findings:
  - HIGH #1 — Outcome-A discriminator overclaim: witness-only's `Sleep(500)` substitution for the marker helper means a clean `0xA4000003` outcome does NOT prove the marker helper is safe; Hermes could prematurely narrow cycle 27 to later graphics init.
  - HIGH #2 — Orchestration-state files (`claude-status.md`, `current-cycle.md`) overstated completion vs the actual working tree.
  - MEDIUM #3 — Missing `.gitignore` for build artifacts (peer XBEs have one).
  - LOW #4 — Manifest success-case count inconsistent ("N+1" vs "1 live + 1 new orphan + N prior orphans = N+2"; clean baseline = exactly 2 buffers).

  All 4 adopted in full: every Outcome-A statement narrowed across `README.md`, `manifest.json`, `current-cycle.md`; `claude-status.md` rewritten as strict in-progress receipt; `.gitignore` added; manifest count restated as "exactly 2 total buffers".

- **Round 2:** BLOCK on residual #1 PARTIAL — `main.c` retained 2 leftover overclaim sites at the cycle-25 motivation paragraph and the "Why match image-blit's linking pattern exactly" paragraph; both still attributed Outcome A to post-witness code only. Plus new LOW finding on `current-cycle.md` (exit checkboxes unchecked) vs `claude-status.md` (steps marked complete) disagreement. Both adopted: main.c overclaim sites rewritten; current-cycle.md exit checkboxes flipped to reflect actual on-disk state.

- **Round 3: PASS_WITH_FINDINGS.** Round-2 #1 PARTIAL RESOLVED (`main.c:47-55, 95-100`). Round-2 new LOW PARTIAL (claude-status.md still said "Codex round 2 pending"; addressed in this update before final docs sync). Round-1 #2/#3/#4 CARRIED. No new issues. No open questions. Validation marker written to `.claude/state/codex-validate-last-run`.

**Hypothesis status after cycle 25 (UNCHANGED from cycle 24 — cycle 25 ships the discriminator; cycle 26 runs it).**

1. Cycle-19 hypothesis #1 (D:\\ remap mismatch under `runxbe` chainload): FULLY INVALIDATED (unchanged from cycle 22).
2. Cycle-21 hypothesis #2 (NV2A class-object instantiation mismatch / `pb_agp_access` divergence / generic early-init failure): STILL OPEN.
3. Cycle-21 hypothesis #3 (FATX-driver / NT-mount state divergence): INVALIDATED for D:\\.
4. Cycle-22 leading hypothesis (image-blit crashes BEFORE main()'s first instruction): WEAKENED (carried from cycle 24); cycle 26 will discriminate.
5. NEW (cycle 24) hypothesis #5: the kseg0-scan witness mechanism may be real-Xbox-unsafe from a non-agent process context. STILL TOP-PRIORITY; cycle 26 will discriminate via the cycle-25 witness-only XBE.

**Cycle 25 closure.** Closure commit (this commit) covers: 5 NEW files under `scripts/apple-silicon/xbe-tests/witness-only/` (main.c, Makefile, manifest.json, README.md, .gitignore); built artifacts (`bin/default.xbe`, `witness-only.iso`); doc updates (this handoff entry, decision-log cycle-25 entry, orchestration-state quartet closure pass); `.claude/state/codex-validate-last-run` marker bump. NO xemu-fork host source touched. NO changes to `lib/xbed_a4_witness.{c,h}`, `oracle-agent/`, or `image-blit/`.

**Cycle 25 evidence preserved on disk.**

- `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-20260523T034519Z/xemu.log` — full xemu-Metal stderr/stdout across the smoke run.
- `benchmark-runs/cycle25-witness-only-xemu-metal-smoke-20260523T034519Z/summary.txt` — anchor-line counts.

(`benchmark-runs/` is gitignored per project convention; consistent with cycles 19/20/21/22/24.)

**Next bounded slice (cycle 26 — Hermes's call).**

Hard precondition: physically power-cycle the Xbox if multiple A.4-tagged orphans pre-exist (cycle-24 left a stale persistent buffer; even though it's lost on power-off, if Hermes ran additional cycle-26 attempts in the same power session, prior orphans would accumulate). Then:

1. `oracle-orchestrator.py ensure-agent` (cycle-23 build of oracle-agent must be deployed; provides `witness.scan` verb).
2. Baseline `oracle-client.py raw witness.scan` — must show exactly 1 live `oracle_ctrl_buffer` with `reserved[0]==0`.
3. FTP-upload `scripts/apple-silicon/xbe-tests/witness-only/bin/default.xbe` to `/E/Apps/witness-only/default.xbe`.
4. `oracle-client.py runxbe 'E:\Apps\witness-only\default.xbe'`.
5. Poll FTP/21 + agent/9001 + ICMP ping (cycle-24 poll pattern from `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-*/03-chainload-image-blit.log` is appropriate).
6. On dashboard return: `ensure-agent` again + `witness.scan`. Inspect highest-phys orphan.

Branch results enumerated above (outcome A / B / C). Cycle 26 closure should record the elapsed time, the `witness.scan` output, and the conclusion (witness-mechanism viability discriminated).

**Out-of-scope (cycle 25 kept bounded).**

- Did NOT run the witness-only XBE on real Xbox — that's cycle 26.
- Did NOT modify cycle-23 `lib/xbed_a4_witness.{c,h}` (Codex-validated PASS_WITH_FINDINGS round 2 in cycle 23).
- Did NOT modify oracle-agent's `witness.scan` RPC.
- Did NOT modify image-blit.
- Did NOT touch xemu-fork host source.
- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on or any other flag.
- Did NOT touch retail-title metrics, §G.5, RT-as-texture, or second-wave XBE work.
- Did NOT promote cycle 26 to "shall run." Cycle 26 is Hermes's call.
- Did NOT issue a PushNotification — Hermes will see this on next state-file review.

Cycle 24 details preserved unchanged below.

---

## 2026-05-22 (cycle 24 Path A.4 real-Xbox discriminator run) — CONCRETE BLOCKER: cycle-23 image-blit hard-hangs real Xbox; cycle-22 leading hypothesis WEAKENED but not corroborated/invalidated

**Status: CLOSED with a CONCRETE BLOCKER (no destructive operations; no source/script code edits; docs/evidence + binary-deploy slice). The bounded scope of cycle 24 was "run the cycle-23-shipped A.4 witness on real Xbox, interpret conservatively, sync docs, stop cleanly." Outcome shape: the witness readback is UNRECOVERABLE because the cycle-23 image-blit binary appears to hard-hang the OG Xbox (no auto-reboot to dashboard) — but the failure-mode delta from cycle 19/20/21 is itself a weak signal about how far into `main()` the binary now reaches.**

**Slice.** Cycle 23 shipped the writer (`image-blit/main.c` call sites bracketing `image_blit_marker(0, ...)`) + reader (`oracle-agent/commands.c::cmd_witness_scan`) for the non-fopen kernel-pool controller-buffer witness, plus the rebuilt ISOs/XBEs. The bounded scope was "add the witness, not run it." Cycle 24 ran it on real Xbox: deploy cycle-23 binaries via FTP, baseline `witness.scan`, chainload image-blit, wait for FTP-back, restart agent, query `witness.scan` again, interpret per the cycle-23 discriminator semantics table.

**What ran (chronological, file-backed in `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/`).**

1. **Reachability + agent build check (2026-05-23T02:30:50Z).** Xbox reachable at 192.168.0.200: `ping` 0% loss / ~0.5 ms RTT; `nc -z 192.168.0.200 9001` → port open. Deployed agent was a cycle-22 build (`oracle-client.py raw witness.scan` → `500- unknown command: witness.scan`). Conclusion: deploy cycle-23 build first.
2. **Reboot Xbox → dashboard FTP (2026-05-23T02:32:57Z).** `oracle-client.py reboot` issued; dashboard FTP/21 came back at +12 s (within the cycle-21 baseline range; agent's `reboot` verb behaves identically to prior cycles).
3. **FTP upload (2026-05-23T02:33:32Z → 02:33:34Z).** `curl --upload-file` of both cycle-23-built XBEs:
    - `scripts/apple-silicon/xbe-tests/oracle-agent/bin/default.xbe` (417 792 B local) → `/E/Apps/oracle-agent/default.xbe`; FTP `LIST` confirms remote size 417 792 B.
    - `scripts/apple-silicon/xbe-tests/image-blit/bin/default.xbe` (159 744 B local) → `/E/Apps/image-blit/default.xbe`; FTP `LIST` confirms remote size 159 744 B.
4. **Re-launch agent (2026-05-23T02:34:03Z).** `oracle-orchestrator.py ensure-agent` → `SITE EXEC E:\Apps\oracle-agent\default.xbe → 200 EXEC command succeeded` → `agent ready at 192.168.0.200:9001`. `info` returns the same `v0.4 (Phase 2 + controller.* + smc.*)` banner (the cycle-23 build did not bump the banner string but DID add the `witness.scan` verb).
5. **Witness verb recognition check (2026-05-23T02:34:16Z).** `oracle-client.py raw help | grep witness` returns `witness.scan  enumerate kseg0 oracle_ctrl_buffer instances + reserved[0,1] (cycle-23 A.4 readback)` — verb is now registered. `oracle-client.py raw witness.scan` returns:
    ```
    201 buf.0 phys=0x03eb3000 virt=0x83eb3000 live=1 reserved0=0x00000000 reserved1=0x00000000
    count=1 mapped_pages_seen=419
    ```
    **Baseline precondition MET** per the `lib/xbed_a4_witness.h` documented requirement: exactly ONE live `oracle_ctrl_buffer` with `reserved[0] == 0`. No power-cycle needed. Cross-check via `controller.buffer-info`: `addr=0x83eb3000 phys=0x03eb3000 size=120 magic=0x58435452 version=1 ports=4 port_state_size=26 anchor_ok=1` (magic = `XCTR` little-endian = 0x58435452; `anchor_ok=1` means `E:\Apps\oracle-agent\state\ctrl-addr.txt` matches the live buffer; perfect agreement with the `witness.scan` row).
6. **Chainload image-blit (2026-05-23T02:34:34Z).** `oracle-client.py runxbe 'E:\Apps\image-blit\default.xbe'` → `launching E:\Apps\image-blit\default.xbe`. Chainload epoch recorded for elapsed-time computation.
7. **Wait for FTP-back / agent / ping — FAILED.** Polling pattern (file-backed in `03-chainload-image-blit.log`):
    - 0..156 s after chainload: dashboard FTP/21 not responding; agent/9001 not responding.
    - 196 s after chainload: ICMP ping started failing 100% (Xbox network stack effectively offline).
    - 196..928 s: continuous polling on ping + FTP/21 + agent/9001. NO response of any kind.
    - **Measurement aborted at 2026-05-23T02:50:02Z, TOTAL_ELAPSED_FROM_CHAINLOAD=928.3 s.**
8. **Post-chainload `witness.scan` — UNRECOVERABLE.** Without network reachability there is no way to read the persistent kernel-pool buffer. The persistent buffer is `MmAllocateContiguousMemoryEx + MmPersistContiguousMemory`-backed; it survives soft reboots within a single power session but does NOT survive a power-off. A hard hang has no auto-reboot path on this Xbox revision (iND-BiOS without BFM 5004.67; no XBDM). Recovery requires Hermes to physically power-cycle the console, which erases the witness state.

**Comparison vs cycle 19/20/21 baseline.** Across cycle 19 attempts (×2), cycle 20 attempts (×2), and cycle 21 attempt (×1), the chainload→FTP-back gap was a remarkably stable 22.3..22.4 s (5 reproductions; max-min = 0.1 s). The 22.4 s gap is consistent with the OG Xbox kernel exception handler auto-rebooting on a critical fault — cycle 22's interpretation. Cycle 24 broke that pattern: the chainload→FTP-back gap is **NOT 22.4 s; it is at least 928.3 s and still hung at measurement-abort**. This is a 6th cycle 19/20/21/22-class real-Xbox run for image-blit; first-of-its-kind divergence; the only change between cycle 21's last-tested image-blit binary and cycle 24's image-blit binary is the cycle-23 witness code path (~196 LOC including helper + 2 call sites).

**Conservative reading — what cycle 24's outcome ACTUALLY tells us.**

1. **Cycle-22 leading hypothesis (image-blit dies BEFORE main()'s first instruction) is WEAKENED but neither corroborated nor invalidated.** Reasoning: if main() never ran in cycles 19/20/21 *and* doesn't run in cycle 24, the cycle-23 instrumentation would never be reached, the binary should fail identically to cycle 19/20/21 (22.4 s reboot), and we would see a 22.4 s gap. We do NOT see a 22.4 s gap. So at least *something* about the cycle-23 binary executes that did not execute (or executed differently) in the cycle-21 binary. The most parsimonious candidate is the witness call site at `image-blit/main.c:789` (absolute first instruction of `main()`); the second candidate is the additional linked `.text` from `xbed_a4_witness.c` shifting the binary's pre-main behavior (DllCharacteristics / XBE thunking / CRT init paths shift slightly when the binary grows ~6 KB). Either way, the cycle-22 framing "the XBE never reaches main()'s first instruction" is in tension with the observed delta — but it is NOT falsified, because the additional code could be hitting BEFORE `main()` (i.e. in CRT init via static initializers, or in the XBE header thunking) without main() ever being entered. The witness uses no global constructors; the only mechanism for it to run pre-main is the loader itself, which is unlikely. WEAKENED, not invalidated.
2. **The kseg0-scan witness mechanism may itself be real-Xbox-unsafe.** The same `xbed_a4_witness_fire` code runs cleanly on xemu-Metal (4 boots in cycle 23, all green). It runs cleanly on real Xbox from the agent context (cycle 24's baseline `witness.scan` returned correctly with `mapped_pages_seen=419`). What's new in cycle 24 is firing it from image-blit's `main()` context (a different XBE, different process, different load address, different RPC context). The kseg0 scan uses `MmGetPhysicalAddress` per-page gating — that's the correct pattern (Codex round-1 BLOCKING #1; xemu-validated). If real-Xbox kseg0 page-table state at image-blit's `main()` entry has a page that `MmGetPhysicalAddress` returns non-zero for but that physically backs to a non-existent BAR / MMIO / aliased region that hangs on read, the witness's identity-aliased read `*magic_ptr` at `xbed_a4_witness.c::xbed_a4_witness_fire` body could hang the CPU. xemu emulation of `MmGetPhysicalAddress` cannot reproduce that class of hang because xemu's RAM map is contiguous and well-defined. This is a new failure mode that cycle 23's local validation could not have caught.
3. **The witness MAY have written its byte before the hang.** The `xbed_a4_witness_fire` body writes `(0xA4 << 24) | stage` to `reserved[0]` *after* finding a candidate. If a candidate was found and the write succeeded, then `MmPersistContiguousMemory` guarantees the byte survives to next power session start — except this Xbox does NOT have a sustained power session; recovery requires power-cycle. So even if the byte landed, we cannot read it. The persistent semantic is "survives chainload AND survives soft reset," not "survives power off."

**Hypothesis status after cycle 24 (delta from cycle 23).**

1. **Cycle-19 hypothesis #1 (D:\ remap mismatch under `runxbe` chainload):** FULLY INVALIDATED (unchanged from cycle 22).
2. **Cycle-21 hypothesis #2 (NV2A class-object instantiation mismatch / `pb_agp_access` divergence / generic early-init failure):** STILL OPEN. Cycle 24 neither confirms nor denies this directly.
3. **Cycle-21 hypothesis #3 (FATX-driver / NT-mount state diverges between FTP-server-time and chainloaded-XBE-time):** INVALIDATED for D:\ (unchanged from cycle 22).
4. **Cycle-22 leading hypothesis (image-blit crashes BEFORE main()'s first instruction):** WEAKENED. Cycle 24's failure-mode delta (auto-reboot at 22.4 s regressed to indefinite hang) is best explained by *something* in cycle-23's instrumentation executing where nothing in cycle-21 image-blit executed. Most parsimonious explanation: the witness call IS firing inside main() and the resulting CPU state hangs the box. Less parsimonious explanations exist (pre-main side-effects of the additional `.text`) and are not falsified.
5. **NEW (cycle 24): the witness mechanism's kseg0-scan-from-non-agent-context may be real-Xbox-unsafe even with the `MmGetPhysicalAddress` gate.** Promote to top-priority candidate to discriminate before any further A.4 readback attempt.

**Codex validation.** Skipped under rule #15's "doc-only / ≤30-line uncommitted diff" carve-out. Cycle 24 ships ZERO source/script code edits, ZERO XBE rebuilds, and only doc + evidence-file edits across `docs/apple-silicon/handoff.md`, `docs/apple-silicon/decision-log.md`, `docs/apple-silicon/orchestration-state/*`, and the new `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/` evidence directory. All Xbox-side operations were evidence-gathering (FTP upload of cycle-23-built binaries; `witness.scan` reads; `runxbe` chainload via existing verb; `reboot` via existing verb). Per-slice justification recorded in `orchestration-state/validation-status.md`. Validation marker NOT written. If any future cycle needs to fix the witness mechanism (cycle 25 candidate), Codex validation becomes mandatory before re-running.

**Files produced (evidence preserved on disk).**

- `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/01-deploy.log` — FTP-upload of cycle-23 oracle-agent + image-blit XBEs (remote sizes confirmed via `LIST`), `ensure-agent` `SITE EXEC` output, info/help/`witness.scan` verb-recognition probe.
- `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/02-baseline-witness-scan.log` — clean baseline (1 live buffer, reserved[0]=0, reserved[1]=0, magic XCTR, anchor_ok=1).
- `benchmark-runs/cycle24-real-xbox-image-blit-a4-witness-20260523T023225Z/03-chainload-image-blit.log` — chainload timing + 928.3 s of polling on FTP/21 + agent/9001 + ICMP ping with full silence; concrete-blocker outcome appended.
- `docs/apple-silicon/handoff.md` — this entry (cycle-23 entry preserved unchanged below).
- `docs/apple-silicon/decision-log.md` — cycle-24 entry above cycle-23; no supersession of cycle 23, 22, 21, or 17 entries.
- `docs/apple-silicon/orchestration-state/{current-cycle.md, claude-status.md, validation-status.md, handoff-summary.md}` — all four files updated for cycle-24 closure.
- ZERO source/script files touched.
- ZERO XBE rebuilds.

**Recovery action required (Hermes-side).**

- Physically power-cycle the Xbox before any next real-Xbox attempt. The witness state is lost on power-off (persistent buffer is `MmPersistContiguousMemory`-tagged, which survives soft reset but not power loss). Therefore the cycle-24 byte is unrecoverable from this run.

**Next bounded slice (cycle 25 recommendation, NOT promoted in this cycle).**

Build a minimal "witness-only" diag XBE that contains:

1. `main()` body: `xbed_a4_witness_fire(XBED_A4_STAGE_MAIN_ENTERED)` → `Sleep(500)` → `xbed_a4_witness_fire(XBED_A4_STAGE_POST_MARKER0)` → `Sleep(500)` → `HalReturnToFirmware(HalRebootRoutine)`.
2. NO `xbed_init`, NO pbkit, NO NV2A class-object instantiation, NO `image_blit_marker_*`, NO drawing, NO file I/O.
3. Built as a sibling XBE under `scripts/apple-silicon/xbe-tests/witness-only/` using `lib/lib.mk` (so it links `xbed_a4_witness.c` the same way image-blit does).

Cycle-25 real-Xbox run shape: deploy → baseline `witness.scan` → `runxbe E:\Apps\witness-only\default.xbe` → wait → expected behavior is `HalRebootRoutine` → dashboard FTP back within ~5..15 s (the same range as cycle-19/20/21's 22.4 s, possibly faster because there's no XBE-side render loop). Then post-chainload `witness.scan` should show 2 buffers with one orphan having `reserved[0] == 0xA4000003` (last fire = POST_MARKER0 since we re-target highest-phys candidate each time).

**Branch results in cycle 25.**

- Witness-only XBE reboots in ~5..15 s + orphan with `reserved[0] == 0xA4000003` → the witness mechanism IS real-Xbox-safe; image-blit's hang is from somewhere AFTER the witness call site (pbkit / NV2A / xbed_init / xbed_render_loop_then_capture) and the cycle-22 "pre-main crash" hypothesis is **INVALIDATED**. Cycle 26 can then add finer-grained witness call sites (post-pbkit, post-xbed_init, pre-first-NV2A-draw, etc.) inside image-blit to localize the failure point.
- Witness-only XBE hangs the Xbox identically to cycle 24 → the witness mechanism itself is real-Xbox-incompatible; redesign required. Candidates: (a) write to EEPROM scratchpad instead of kseg0 (EEPROM survives power-cycle, but writes require unsafe.enable + careful timing); (b) write to a fixed FATX file BEFORE pbkit init (but cycles 19/20/21 already proved fopen blocks under `runxbe` chainload); (c) abandon the in-XBE witness approach entirely and pivot to XBE-level static analysis of cycle-23 vs cycle-21 image-blit binary deltas.
- Witness-only XBE reboots in ~5..15 s + orphan with `reserved[0] == 0xA4000001` (MAIN_ENTERED but NOT POST_MARKER0) → the witness mechanism is real-Xbox-safe for ONE fire but the second `xbed_a4_witness_fire` in quick succession somehow hangs. Less likely but worth surfacing.

**Out-of-scope (cycle 24 kept bounded).**

- Did NOT rebuild any XBE.
- Did NOT touch xemu-fork host source.
- Did NOT modify `xbed_a4_witness.{h,c}` or `image-blit/main.c` or `oracle-agent/commands.c`.
- Did NOT promote a cycle-25 plan beyond a single-paragraph recommendation.
- Did NOT touch retail-title metrics, §G.5, RT-as-texture, or any second-wave XBE work.
- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on.
- Did NOT issue a PushNotification — Hermes will see this on next state-file review and decide whether to schedule cycle 25 immediately or queue it.

Cycle 23 details preserved unchanged below.

---

## 2026-05-22 (cycle 23 Path A.4) — non-fopen kernel-pool controller-buffer witness for image-blit SHIPPED; cycle-24 real-Xbox discriminator run pending

**Status: SHIPPED (code slice; XBE-only + oracle-agent RPC extension; no xemu-fork host source touched). Bounded slice CLOSED — implementation + local xemu-Metal validation green; Codex round-2 PASS_WITH_FINDINGS (all BLOCKING + MEDIUM resolved; MINOR resolved post-round-2). Real-Xbox discriminator run deferred to cycle 24 per bounded assignment scope.**

**Slice.** Cycle 22 promoted A.4 to top-priority by invalidating cycle-19 hypothesis #1 (launch-path blocker) via comparator evidence (five reference captures wrote `D:\\<id>-capture.bin` successfully via the same `XLaunchXBE` chainload path). Cycle-22 leading hypothesis after that: image-blit crashes BEFORE its main() body executes the first `image_blit_marker(0, "program_entered")` call (`image-blit/main.c:780`) — i.e. in CRT init, static-init, DllCharacteristics, or pre-main XBE thunking. Cycle 23 implements the cycle-22-identified discriminator: a witness mechanism that does NOT use fopen (the suspect path) and fires from inside main()'s first instruction. Bounded scope was "add the witness," NOT "run it on real Xbox and report results" — the real-Xbox run is cycle 24's job.

**Design (locked 2026-05-22 19:24 CDT).** The agent's `oracle_ctrl_buffer` (`oracle-agent/controller.h:153-158`) is allocated via `MmAllocateContiguousMemoryEx` + `MmPersistContiguousMemory` so the kernel-pool page survives the agent's process death across `XLaunchXBE` chainload (`controller.c:153-198`). Per `controller.c:207-208` the agent leaks one persistent page per restart until the Xbox is power-cycled. A.4 writes are routed to that buffer header's `reserved[0]/reserved[1]` fields (offsets 8/12) found via kseg0 [0x80010000, 0x84000000] scan in 4 KiB strides — no fopen anywhere in the witness path. Two image-blit call sites: `XBED_A4_STAGE_MAIN_ENTERED` (1) as the absolute first instruction of `main()` at `image-blit/main.c:789`; `XBED_A4_STAGE_POST_MARKER0` (3) immediately AFTER `image_blit_marker(0, "program_entered")` returns at `image-blit/main.c:806`. New oracle-agent RPC `witness.scan` enumerates ALL `oracle_ctrl_buffer` instances in kseg0 with their `reserved[0]/reserved[1]` values so the host side can read the witness back across the chainload + agent-restart boundary.

**Implementation.** Files:
- NEW: `scripts/apple-silicon/xbe-tests/lib/xbed_a4_witness.h` — public declarations + stage codes + tag byte + safety/discriminator semantics doc.
- NEW: `scripts/apple-silicon/xbe-tests/lib/xbed_a4_witness.c` — `xbed_a4_witness_fire(stage)` implementation. Scans kseg0; uses `MmGetPhysicalAddress` per-page safety gate (skip unmapped pages without dereferencing — pattern from `oracle-agent/tier2.c:159` and `controller.c:163`); applies shared `a4_candidate_ok` filter (magic+version+`reserved[0]` ∈ {0, 0xA4xxxxxx}+`reserved[1] < 4096`); targets HIGHEST-phys passing candidate (most recent agent allocation; deterministic across repeated runs in one power session); writes `(0xA4 << 24) | stage` to `reserved[0]` + bumps `reserved[1]`; wbinvd. Emits ENTRY + RESULT host-log lines so xemu logs with `XEMU_GUEST_LOG=1` see every call.
- EDIT: `scripts/apple-silicon/xbe-tests/lib/lib.mk` — adds `xbed_a4_witness.c` to SRCS for every XBE that uses `lib/lib.mk` (currently all of them; image-blit is the first call-site caller this cycle; sibling XBEs link the symbol but do not call it).
- EDIT: `scripts/apple-silicon/xbe-tests/image-blit/main.c` (+26 lines) — `#include "xbed_a4_witness.h"`; 2 call sites at the absolute first instruction of `main()` and immediately after `image_blit_marker(0, ...)`. Cycle-20+21 marker_00..12 fopen-based instrumentation LEFT IN PLACE — A.4 is a strict ADD-ONLY discriminator that runs in parallel.
- EDIT: `scripts/apple-silicon/xbe-tests/oracle-agent/commands.{h,c}` (+127 lines) — declares + implements `cmd_witness_scan`. Mirror image-blit's writer-side safety + filters via parallel `a4_reader_candidate_ok` predicate (Codex round-2 lockstep requirement); reports `buf.N phys=… virt=… live=N reserved0=… reserved1=…` lines + a `count=N mapped_pages_seen=M` summary. The `live` flag cross-references `oracle_ctrl_get()` so the host side immediately sees which buffer is the current agent allocation vs. orphans.
- EDIT: `scripts/apple-silicon/xbe-tests/oracle-agent/main.c` — register `"witness.scan"` in the dispatch table.

**Local xemu-Metal validation (4 boots, /tmp/cycle23-final).** Witness call sites BOTH fire on every boot (`xbed_a4_witness: enter stage=1` + `xbed_a4_witness: enter stage=3` lines appear via the `XEMU_GUEST_LOG=1` channel). Standalone xemu has no agent running, so the scan correctly reports "no XCTR buffer found" with `mapped_pages_seen=378` (cold-boot) or `mapped_pages_seen=77` (warm reboot) — meaning the kernel has ~300 KiB to ~1.5 MiB of mapped kseg0 RAM at image-blit's main() entry, well covered by the scan. Image-blit's first-boot tally is `pass=3/8 mask=0x31` — UNCHANGED from the cycle-21 baseline; no instrumentation regression. All cycle-20+21 markers 00..12 still fire in xemu logs.

**Failure mode discovered + fixed during local validation.** Initial implementation blindly dereferenced every page in [0x80010000, 0x84000000] without an MmGetPhysicalAddress gate. xemu-Metal local run showed the witness ENTRY breadcrumb fired but NOTHING after it (no marker_00 host-log, no cell PASS/FAIL lines) — image-blit kept rendering frames but the host-log channel was effectively silent. Root cause: on the OG Xbox kernel (and xemu's emulation), only pages the MMU page tables actually cover are valid kseg0 reads; the rest fault on dereference. The `MmGetPhysicalAddress` gate (returns 0 for unmapped pages) eliminates the crash; with the gate, all four boots are clean. Codex round-1 review picked this up as a BLOCKING finding for the agent-side `witness.scan` RPC (same blind-dereference pattern) and a MINOR finding for the writer's header comment claiming "reads never fault"; both fixed before round 2.

**Codex validation.** Round 1: BLOCK with 4 findings — 2 BLOCKING (agent reader needs `MmGetPhysicalAddress` gate + filter parity with writer), 1 MEDIUM (writer-side first-match attribution ambiguous on repeated runs), 1 MINOR (header doc drift). All 4 findings adopted in full:
- BLOCKING #1: `cmd_witness_scan` gains `MmGetPhysicalAddress` per-page gate via new `a4_reader_candidate_ok` helper.
- BLOCKING #2: `cmd_witness_scan` applies the SAME `reserved[0]/reserved[1]` filters as the writer (lockstep documented in commands.c body comment + filter helpers extracted on both sides).
- MEDIUM #3: writer changed to scan full kseg0 once, pick HIGHEST-phys passing candidate (= most recent agent allocation; agent allocator grows monotonically per restart).
- MINOR #4: header `xbed_a4_witness.h` Safety notes rewritten + "first occurrence/first match" wording replaced with "HIGHEST-phys passing candidate" wording.

Round 2: PASS_WITH_FINDINGS — all 3 BLOCKING + MEDIUM RESOLVED, MINOR PARTIAL (residual header "first match" wording in two places). MINOR PARTIAL closed post-round-2 via direct comment sync. Validation marker written to `.claude/state/codex-validate-last-run`.

**Hypothesis status after cycle 23 (UNCHANGED FROM CYCLE 22 — A.4 only ships the discriminator; cycle 24 runs it).**

1. **Cycle-19 hypothesis #1 (D:\\ remap mismatch under `runxbe` chainload):** FULLY INVALIDATED in cycle 22.
2. **Cycle-21 hypothesis #2 (NV2A class-object instantiation mismatch / `pb_agp_access` divergence / generic early-init failure):** STILL OPEN; A.4 readback in cycle 24 will discriminate.
3. **Cycle-21 hypothesis #3 (FATX-driver / NT-mount state diverges between FTP-server-time and chainloaded-XBE-time):** INVALIDATED for D:\\ in cycle 22; residual narrow uncertainty for E:\\.
4. **Cycle-22 leading hypothesis (image-blit crashes before main()'s first instruction):** STILL LEADING; cycle 24 will discriminate.

**Cycle 23 closure.** Closure commit `5fce3b14e4` (landed on `apple-silicon-performance`) covers: lib/xbed_a4_witness.{h,c} (new), lib/lib.mk (SRCS add), image-blit/main.c (2 witness call sites + comments), image-blit/{bin/default.xbe,image-blit.iso} (rebuilt), oracle-agent/commands.{h,c} (cmd_witness_scan + helper), oracle-agent/main.c (register witness.scan verb), oracle-agent/{bin/default.xbe,oracle-agent.iso} (rebuilt), this handoff entry, decision-log cycle-23 entry, orchestration-state quartet, `.claude/state/codex-validate-last-run` marker. No xemu-fork host source files touched; no flag default flips.

**Next bounded slice (NOT promoted in this cycle).**
- **Cycle 24.** Real-Xbox run of the patched image-blit + oracle-agent. Hard precondition (per `lib/xbed_a4_witness.h` doc): baseline `witness.scan` before chainload must show exactly ONE live buffer with `reserved[0] == 0`; if multiple orphans exist from a prior cycle-24 attempt in the same power session, Hermes must power-cycle the Xbox first. Then `runxbe E:\\Apps\\image-blit\\default.xbe`. Wait for FTP-back. Restart agent. Query `witness.scan` again. Interpretation per the discriminator semantics enumerated above.

**Out-of-scope (cycle-23 kept bounded).**
- Did NOT run image-blit on real Xbox — that's cycle 24.
- Did NOT modify the cycle-20+21 fopen-based marker mechanism — the A.4 witness is a strict ADD-ONLY discriminator that runs in parallel; cycle 20+21 markers still fire to disk in cycle 24 (and will continue to produce ZERO files if the cycle-19+20+21 pattern persists).
- Did NOT touch xemu-fork host source (no `hw/`, `ui/`, `accel/`, `target/` edits).
- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on or any other flag.
- Did NOT touch retail-title metrics, §G.5, RT-as-texture, or any second-wave XBE work.
- Did NOT start Path B (smaller PFIFO-race-only Tier-1 diag XBE — still on the table per cycle-19 recommendation list; better suited if A.4 lands inconclusively).

Cycle 22 details preserved below.

---

## 2026-05-22 (cycle 22 Path A.3) — `xbox-real-references/` provenance audit CLOSED; cycle-19 launch-path-blocker hypothesis fully INVALIDATED; image-blit failure re-classified as image-blit-specific

**Status: CLOSED (doc/evidence-only audit; no code, no XBE rebuild, no real-Xbox runs). Bounded slice CLOSED — provenance question answered with HIGH confidence across all five reference sets; cycle-21 interpretation materially sharpened.**

**Slice.** Cycle 21 left three candidate next slices (A.3 provenance audit, A.4 non-fopen kernel-pool buffer witness, B smaller PFIFO-race-only Tier-1 XBE). Cycle 22 took A.3 — the cheapest single follow-up per rule #1 — because it required only inspecting existing artifacts + tooling history: no new code, no XBE rebuild, no real-Xbox run. Binary question: did the existing reference captures use the same `runxbe` SITE-EXEC chainload path image-blit currently uses, or a meaningfully different launch path?

**Method.** Five steps, all file-backed:
1. Inventory every file under `docs/apple-silicon/xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor,controller-roundtrip}/`. Result: 6 PNG files, **zero** embedded README/metadata/provenance docs. Audit must rely on git history + harness/agent source lineage.
2. `git log --diff-filter=A` (no `--follow`) on each capture file to find the introducing commit.
3. Read the orchestrator (`scripts/apple-silicon/oracle-orchestrator.py`) and xbe-harness (`scripts/apple-silicon/xbe-harness/xbe_renderers.py`) source at each introducing commit, and at HEAD, and diff them to verify the chainload mechanism didn't change.
4. Read the oracle-agent (`scripts/apple-silicon/xbe-tests/oracle-agent/commands.c`) `cmd_runxbe` handler at the earliest introducing commit and at HEAD to verify the kernel-call primitive (`XLaunchXBE(path)`) is unchanged.
5. Cross-reference with the dashboard-transition timeline (`e74715cd71` "Composite-capture leg + UnleashX dashboard switch + iND-BiOS findings" at 2026-05-06 19:21 CDT) to determine which dashboard each capture was taken under.

**Provenance evidence (per-set verdicts).**

| Reference set | Capture file | Intro commit | Capture date (commit time / file mtime) | Dashboard at capture | Launch path | Verdict |
|---|---|---|---|---|---|---|
| pipeline-smoke | `pipeline-smoke/real-xbox.png` (2 320 B) | `aae0138565` | 2026-05-06 15:32 CDT / file mtime 15:19 (pre-19:21 UnleashX switch) | **XBMC4Gamers** | orchestrator `run_diag` → `client.runxbe()` RPC → agent `cmd_runxbe` → `XLaunchXBE("E:\\Apps\\pipeline-smoke\\default.xbe")`. AGENT FTP-launched via `SITE RunXBE`. | **runxbe path (HIGH)** — same `XLaunchXBE` chainload as cycle-21; only the AGENT-launch verb differs (SITE RunXBE vs. SITE EXEC), and that difference is upstream of the diag XBE's process environment. |
| color-channel | `color-channel/real-xbox.png` (2 319 B) | `823733f2e6` | 2026-05-06 21:48 CDT (file mtime; post-switch) | **UnleashX** | same chainload; AGENT FTP-launched via `SITE EXEC` (orchestrator now SITE HELP-auto-detects). | **runxbe path (HIGH)** — identical dashboard + same chainload as cycle-21. |
| depth-floor | `depth-floor/real-xbox.png` (2 313 B) | `823733f2e6` | 2026-05-06 21:48 CDT (post-switch) | **UnleashX** | same. | **runxbe path (HIGH)** — identical dashboard + same chainload as cycle-21. |
| mirror | `mirror/real-xbox.png` (2 330 B) | `823733f2e6` | 2026-05-06 21:48 CDT (post-switch) | **UnleashX** | same. | **runxbe path (HIGH)** — identical dashboard + same chainload as cycle-21. |
| mirror | `mirror/composite.png` (2 191 B) | `58bf218838` | 2026-05-07 09:55 CDT | **UnleashX** | `scripts/apple-silicon/capture-composite-reference.sh` runs the same `oracle-orchestrator.py run-diag` chainload, then PICKS the best-matching frame from a parallel MS2109 HDMI-capture recording. The XBE-side chainload is identical to the other sets. | **runxbe path (HIGH)** — XBE chainload identical; the only "different" thing is the host-side capture leg (HDMI grab vs. FTP-pull of XOSS blob), which doesn't affect the diag XBE's execution environment. |
| controller-roundtrip | `controller-roundtrip/real-xbox-zero.png` (2 319 B) | `58bf218838` | 2026-05-07 09:53 CDT | **UnleashX** | same chainload; set additionally requires a pre-run `controller.set` zero-state write via the agent (`_pre_run_setup_real_xbox` in `xbe_renderers.py:343-385`) but the diag-XBE launch itself is `XLaunchXBE(path)` exactly as in every other set. | **runxbe path (HIGH)** — identical dashboard + same chainload as cycle-21. |

**Source-lineage diff summary (capture-time → HEAD).**

- `scripts/apple-silicon/xbe-tests/oracle-agent/commands.c::cmd_runxbe` — 2 commits between `c2274310fc` (first introduction) and HEAD: (i) `81e36900ef` 2026-05-10 reworked path argument parsing from `op_parse_kv_str` to manual `strstr("path=") + trim`, fixing whitespace/CR handling; (ii) `959d24acb8` 2026-05-12 added one line of SMC fan-curve cleanup (`oracle_smc_cleanup_if_manual()`). Neither edit touches the `XLaunchXBE(path)` call or the kernel-side chainload primitive. **The chainload primitive is invariant across all reference captures and cycle-21.**
- `scripts/apple-silicon/oracle-orchestrator.py::run_diag` — diff between `823733f2e6` (color-channel/depth-floor/mirror intro commit) and HEAD is 49 lines, **all comment-only** (rephrases "SITE RunXBE" → "SITE EXEC / SITE RunXBE (auto-detected)" in docstrings; rephrases "XBMC4Gamers' FTP server" → "dashboard FTP"; updates the documented agent path from `/E/XBMC4Gamers/Apps/oracle-agent/...` to `/E/Apps/oracle-agent/...`). The `client.runxbe(xbe_path)` chainload call inside `run_diag` is unchanged.
- `scripts/apple-silicon/xbe-harness/xbe_renderers.py::run_real_xbox` — diff between `823733f2e6` and HEAD adds (a) QMP-socket-in-/tmp for the xemu-side path-length safety (doesn't touch real-Xbox flow), (b) `XBE_HARNESS_TIMEOUT_SECONDS` env knob, (c) `_ensure_agent_via_orch` + `_pre_run_setup_real_xbox` helpers (cycle-17 additions for controller-roundtrip), (d) QMP socket cleanup. The actual real-Xbox upload + chainload path (`oracle-orchestrator.py run-diag --xbe E:\\Apps\\<id>\\default.xbe --ftp-collect /E/Apps/<id>`) is unchanged.

**What this proves and what it doesn't.**

PROVES (file-backed):
1. The current `XLaunchXBE`-based chainload mechanism IS capable of running a diag XBE on real Xbox to the point where it writes a multi-KB `D:\<id>-capture.bin` + `D:\<id>-done.txt` to FATX, reboots back to dashboard, and has those files FTP-retrievable. Provable five times across pipeline-smoke + mirror + color-channel + depth-floor + controller-roundtrip.
2. Cycle-19 hypothesis #1 (the `runxbe` SITE-EXEC chainload path itself blocks witness-path file writes) is **FALSIFIED** by the mere existence of five working reference captures produced via that exact path. Cycle 21 had this hypothesis "demoted to insufficient as sole explanation"; cycle 22 promotes it to **fully invalidated**.
3. Cycle-21 hypothesis #3 (FATX-driver / NT-mount state diverges between FTP-server-time and chainloaded-XBE-time) is **FALSIFIED for D:\\** by mirror/color-channel/depth-floor having all written D:\\ captures successfully via the same chainload under UnleashX. The most natural reading is also falsified for E:\\ (same FATX driver, same NT mount semantics), though no existing reference capture writes to E:\\ from a chainloaded XBE — that's a residual narrow uncertainty.

DOES NOT PROVE:
- That ANY chainloaded diag XBE will succeed on this path. Tier-2+ diag XBEs (image-blit at Tier-2; anything Tier-3) exercise more NV2A state and may hit failures unrelated to the launch path.
- That the byte-exact SHA match recorded at capture time still holds today (the SHA was matched at the moment of commit; the references are static PNGs since).
- That pipeline-smoke (Tier-4, CPU-painted, no NV2A) is a comparator for image-blit (Tier-2, NV_IMAGE_BLIT 0x9F + NV062 + pb_agp_access). The Tier-1 references (mirror/color-channel/depth-floor) are the relevant comparator — they use pbkit + NV2A and succeed.

**Re-interpretation of cycle-21's NEGATIVE witness result against A.3 evidence.**

Five reproductions of the chainload→FTP-back gap (22.4 s, 22.3 s, 22.4 s, 22.4 s, 22.4 s across cycle 19+20+21) with zero retrieved markers from image-blit, set against five successful reference captures via the same launch path, narrows the leading explanation to **image-blit-specific failure**. The strongest candidates after A.3:

1. **image-blit crashes BEFORE its main() body executes the first `image_blit_marker(0, "program_entered")` call.** Marker-00 sits as the literal first line of `main()` per cycle-20's placement — before xbed_init, before pbkit, before any NV2A. If marker-00's fopen never runs, the crash is in CRT init, static-init, DllCharacteristics, or pre-main XBE thunking. **LEADING candidate**: predictive power matches every cycle 19/20/21 observation including the consistent 22.4 s chainload→FTP-back gap (XBE early-exit reboot pattern).
2. **image-blit reaches main() but `nxMountDrive('E', …)` semantics in the chainloaded-XBE runtime environment fail in a way nxIsDriveMounted does not detect AND the D:\\ default-mount also fails.** Less likely after A.3 because mirror/color-channel/depth-floor's D:\\ writes worked from the same chainload — but their XBEs may differ in CRT/static-init behavior or DllCharacteristics.
3. **image-blit's specific NV2A usage (NV_IMAGE_BLIT 0x9F + NV062 + 8-cell sweep + pb_agp_access readback) triggers a crash inside `xbed_init` / pbkit-init on real hardware that doesn't on xemu.** Possible but doesn't explain marker-00 (pre-init) not landing.
4. **image-blit binary size (159 744 B vs. mirror's 147 456 B vs. pipeline-smoke's 110 592 B) crosses some FATX/loader threshold.** Speculative; no evidence to support.

The leading candidate (#1) is what cycle-21's proposed Path A.4 (non-fopen kernel-pool controller-buffer witness) is specifically designed to discriminate. A.3 makes A.4 the right next bounded slice.

**Hypothesis status after cycle 22.**

1. **Cycle-19 hypothesis #1 (D:\\ remap mismatch under `runxbe` chainload):** **FULLY INVALIDATED.** Cycle-21 demoted; cycle-22 falsifies via comparator evidence.
2. **Cycle-21 hypothesis #2 (NV2A class-object instantiation mismatch / `pb_agp_access` divergence / generic early-init failure):** **STILL OPEN** but partially narrowed — if marker-00 (pre-NV2A) doesn't fire, the NV2A-instantiation framing alone can't explain it. Generic CRT/static-init early-crash is the more parsimonious explanation. The narrower NV2A-specific framing is reserved for "image-blit reaches xbed_init's NV2A calls but crashes there"; A.4 discriminates.
3. **Cycle-21 hypothesis #3 (FATX-driver / NT-mount state diverges between FTP-server-time and chainloaded-XBE-time):** **INVALIDATED for D:\\** by reference captures. Residual narrow uncertainty for E:\\ — no existing reference capture writes from a chainloaded XBE to E:\\; the speculation is weakened but not strictly falsified.
4. **Cycle-22 new framing (image-blit crashes before main()'s first instruction completes):** **LEADING.** Best fit to every cycle 19/20/21 observation when read against A.3 comparator evidence.

**Codex validation.** **Skipped under rule #15's "doc-only changes" carve-out.** No code, no XBE rebuild, no real-Xbox runs. Aggregate diff is markdown-only across `docs/apple-silicon/handoff.md`, `docs/apple-silicon/decision-log.md`, and `docs/apple-silicon/orchestration-state/*`. Rule #15 explicitly: "Trivial work skips automatically (≤30-line uncommitted diff, doc-only changes, single-line fixes)." Full justification at the end of the cycle-22 decision-log entry. Validation marker NOT written; this paragraph is the per-slice justification record per `orchestration-state/validation-status.md`.

**Files produced (evidence preserved on disk).**

- `docs/apple-silicon/handoff.md` — this entry.
- `docs/apple-silicon/decision-log.md` — cycle-22 entry above cycle-21; cycle-17/19/20/21 NOT superseded.
- `docs/apple-silicon/orchestration-state/{current-cycle.md, claude-status.md, validation-status.md, handoff-summary.md}` — all four files updated for cycle 22.
- No new files under `docs/apple-silicon/xbox-real-references/` — the audit deliberately did not generate new captures.

**Cycle 11 follow-up list — updated.**

- ✅ #1 (Re-run image-blit on GL) — CLOSED cycle 15.
- ✅ #2 (`XEMU_DIAG_PGRAPH_STATUS_DRAIN`) — CLOSED cycle 17.
- 🟡 #3 (Real-Xbox parity check) — **REFRAMED AGAIN cycle 22.** Cycle 21 demonstrated re-routing to E:\\ doesn't unblock the witness; cycle 22 demonstrated the launch path itself is not the blocker (five reference captures work via the same path). Remaining bounded slices:
  - **A.4 (now top-priority).** Add a non-fopen witness to image-blit: write a few bytes into the oracle-agent's persistent kernel-pool controller buffer (`oracle_ctrl_buffer` at the agent-published phys address) BEFORE attempting any marker fopen. Next agent boot reads the buffer via `controller.buffer-info` / `controller.get`. Bypasses every partition-mount + FATX-driver-state concern. **Discriminates the cycle-22 leading hypothesis (#1) from the cycle-21 #2 framing.**
  - **B.** Smaller PFIFO-race-only Tier-1 diag XBE that captures via PCRTC. Still on the table per cycle 19's recommendation list. Heavier than A.4; better suited if A.4 lands inconclusively.
  - **C (new, cycle 22).** If A.4's witness fires (i.e. image-blit DOES reach first-instruction execution), shift focus to xbed_init / pbkit-init / first NV2A call on real Xbox via per-section instrumentation. If A.4's witness does NOT fire, the crash is in CRT/static-init/XBE thunking — out of in-XBE-source reach; would need either a smaller image-blit variant or XBE-file-level analysis (DllCharacteristics, kernel imports, section layout).

**Out-of-scope (cycle-22 kept bounded).**

- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on.
- Did NOT start Path A.4 (oracle-agent kernel-pool buffer witness) — that's now top-priority for the next bounded slice; Hermes chooses.
- Did NOT start Path B (smaller PFIFO-race-only Tier-1 diag XBE).
- Did NOT modify any code (XBE OR host).
- Did NOT rebuild any binaries.
- Did NOT run any real-Xbox attempts.
- Did NOT verify Hermes-side `.hermes_cycle22_path_a3_prompt.txt` scratch file (it's untracked and out of scope).
- Did NOT touch retail-title metrics, §G.5, RT-as-texture, or any second-wave XBE work.

Cycle 21 details preserved below.

---

## 2026-05-22 (cycle 21 Path A.2) — image-blit markers re-routed to `E:\Apps\image-blit\`; real-Xbox witness still BLOCKED (4th reproduction of the 22.4 s zero-output pattern); D:\-only hypothesis INVALIDATED

**Status: SHIPPED (XBE-only marker-path slice; xemu host source untouched). Bounded slice CLOSED — local validation green, real-Xbox returned a clear NEGATIVE answer, cycle-19 hypothesis #1 demoted from leading to insufficient.**

**Slice.** Cycle 20 closed with "Path A.2 (route markers to a known-writeable partition)" listed as the cheapest single follow-up per rule #1. Cycle 21 executed exactly that bounded slice: re-route the existing 13 staged markers in `scripts/apple-silicon/xbe-tests/image-blit/main.c` from `D:\image-blit-marker-NN-STAGE.txt` to `E:\Apps\image-blit\image-blit-marker-NN-STAGE.txt` — the partition+directory that the xbe-harness already (a) FTP-uploads `default.xbe` to BEFORE chainload and (b) `--ftp-collect`s recursively AFTER chainload (`scripts/apple-silicon/xbe-harness/xbe_renderers.py::run_real_xbox` lines 450-485). The cycle-21 question is binary: if E:\ markers land, cycle-19 hypothesis #1 (D:\ remap mismatch under `runxbe` SITE-EXEC chainload) is corroborated AND the highest-numbered file tells us the actual real-Xbox execution stage. If E:\ markers don't land either, the failure mode is bigger than a partition mismatch and a different next slice is required.

**Why E:\ specifically (precise, conservative choice).** The harness's `_run_real_xbox` uploads to `E:\Apps\<id>\default.xbe` and ftp-collects `/E/Apps/<id>` — so writing to `E:\Apps\image-blit\…` is the SMALLEST possible re-route that's guaranteed FTP-retrievable. nxdk's automount-d only mounts D:\ for the launched XBE; E:\ requires explicit `nxIsDriveMounted('E')` + `nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")` (the standard FATX utility partition). The exact same shim is already shipped in `oracle-agent/controller.c::s_ensure_e_drive_mounted` lines 74-86, `oracle-agent/tier2.c` lines 56-60, `lib/xbed_input_synth.c` lines 43-47, and `controller-readback/main.c` lines 50-55. T:\ was rejected for cycle 21 because the harness FTP-collects from `/E/Apps/<id>` only — a T:\ write would not be retrievable without expanding harness scope. The cycle-21 slice deliberately did NOT relocate `xbed_render_loop_then_capture`'s `D:\image-blit-capture.bin` / `D:\image-blit-done.txt` writes (still under D:\); that's a separate decision pending the cycle-21 evidence.

**Diff (uncommitted in xemu-fork/ at session close).**

- `scripts/apple-silicon/xbe-tests/image-blit/main.c` — re-routes the marker helper to `E:\Apps\image-blit\…`, adds a cached idempotent E:\ mount + `CreateDirectoryA` shim (`image_blit_ensure_e_mount`), bumps the path buffer from 64 to 96 bytes for the longer prefix (basenames stay ≤39 chars; FATX 42-char invariant unchanged from cycle 20), preserves the host-log mirror, preserves the path-overflow safety branch, and adds an explicit `e_mount=N` field to every marker host-log line so xemu logs can correlate mount success with fopen outcome. Helper-comment block updated with cycle-21 rationale, the rejected-T:\ explanation, and the path-length math. Bounded reviewable diff ~120 net lines added (~100 helper / comment, ~20 new mount fn), main.c only.
- `scripts/apple-silicon/xbe-tests/image-blit/bin/default.xbe` + `image-blit.iso` — rebuilt via `eval "$(nxdk/bin/activate -s)" && make`. By-products of the build; staged with this slice's commit.
- `docs/apple-silicon/orchestration-state/{current-cycle,claude-status,validation-status,handoff-summary}.md` — all four files updated for cycle 21.
- `docs/apple-silicon/handoff.md` — this entry (cycle-20 entry preserved below).
- `docs/apple-silicon/decision-log.md` — cycle-21 entry appended above cycle-20; cycle-17 / cycle-19 / cycle-20 NOT superseded.

**Local validation (xemu-Metal, `XEMU_GUEST_LOG=1`).** Two local runs:

- `benchmark-runs/cycle21-image-blit-markers-local-metal-guestlog-20260522T231623Z/` — first build, only-E:\-marker variant (no `CreateDirectoryA` yet). All 13 markers fire per boot, `e_mount=1` reported, BUT every fopen returns `fopen-failed` because the scratch xemu HDD image does NOT contain `E:\Apps\image-blit\` (only the real Xbox does, via the harness FTP-upload step). Surfaced an oversight that would mask local regression checks; fix landed immediately.
- `benchmark-runs/cycle21-image-blit-markers-local-metal-guestlog-20260522T231823Z/` — second build, with `CreateDirectoryA("E:\\Apps", NULL); CreateDirectoryA("E:\\Apps\\image-blit", NULL)` (same pattern as `oracle-agent/controller.c::s_write_anchor_file` lines 95-97). All 13 markers fire per boot × 4 boots = 52 marker lines, `e_mount=1` everywhere, **zero `fopen-failed` lines** (compare cycle 20's 52 fopen-failed lines for the D:\ baseline; the count flip from 52→0 is the local-side proof that the new code path is correct). The v0.4 tally drift across the 4 boots is `3/8 mask=0x31, 3/8 mask=0x31, 2/8 mask=0x30, 2/8 mask=0x30` — BYTE-IDENTICAL to cycle 20's distribution, confirming the marker re-route did not perturb the race the cycle-17 diag flag addresses.

**Real-Xbox run.**

- Oracle smoke: 12/12 PASS at 2026-05-22 18:20 CDT (`/tmp/oracle-smoke-20260522T232023Z`). EEPROM sha256=871ed8a9…, controller buffer kernel-pool `anchor_ok=1`, PMC_BOOT_0=0x02a000e1.
- `benchmark-runs/cycle21-real-xbox-image-blit-markers-20260522T232031Z/` — single attempt per the bounded assignment.
- `chainload_at = 1779492077.015844`, `ftp_back_at = 1779492099.4170449`, gap = **22.40 s exactly** — matches cycle-19 attempt 1 (22.4 s), cycle-19 attempt 2 (22.3 s), and cycle-20 post-Codex attempt (22.4 s) to within 0.1 s. **Fourth independent reproduction.**
- `verdict.json status: ok` for the chainload-and-collect cycle (the orchestrator path itself is healthy; this is not an infra failure).
- FTP-collect from `/E/Apps/image-blit/` returned exactly **1 file**: `default.xbe` (the upload echo — same byte count as the local build, 159 744 bytes). **Zero `image-blit-marker-*` files retrieved.**

**Yes/no answer to the cycle-21 question.** **NO.** Re-routing the marker writes from `D:\` to `E:\Apps\image-blit\` (a path provably writeable AND retrievable from xemu locally, and provably retrievable from real Xbox for any pre-existing file in the directory) does NOT make the markers observable on real Xbox under the current `runxbe`/oracle workflow. The cycle-21 evidence shows the failure mode is upstream of any in-XBE `fopen("E:\\…","wb")` call — either the XBE never reaches its first marker call, or the chainload environment under `SITE EXEC` does not allow this XBE's file writes to land on the FATX partition. The local-side success (52→0 fopen-failed flip) proves the code path is correct in principle; the real-Xbox failure proves something about the SITE-EXEC chainload context blocks the same code from executing the way it does on xemu local.

**Hypothesis status after cycle 21.**

1. **D:\ remap mismatch under `runxbe` chainload** — was promoted to leading hypothesis by cycle 20. Cycle 21 demotes it from "leading" to "INSUFFICIENT as a sole explanation." It may still be one factor, but it cannot account for the symmetric blockage of E:\ writes when the harness has visibly demonstrated that `E:\Apps\image-blit\` is fully writeable from the harness's own FTP upload context AND that the oracle agent (also launched via `SITE EXEC`) writes to `E:\Apps\oracle-agent\state\` successfully.
2. **NV2A class-object instantiation mismatch / `pb_agp_access` divergence / generic early-init failure** — were "not discriminated" by cycle 20. Cycle 21 doesn't directly discriminate them either, but their relative weight goes UP because the partition-mismatch framing is no longer sufficient. The most consistent unifying explanation across cycles 19+20+21 is now: **the XBE either crashes very early (before marker 00's fopen attempt completes) OR the SITE-EXEC `runxbe` chainload runs the XBE in an environment where any in-XBE filesystem write fails silently**. Discriminating those two needs a non-fopen witness (e.g. writing to the oracle agent's persistent kernel-pool controller buffer — a path that is known-functional from `controller-readback` evidence) or a known-good comparator XBE that DOES survive `runxbe` chainload (Path A.3 provenance audit of the existing `xbox-real-references/` captures).
3. **NEW (cycle 21):** The harness's UPLOAD step is what makes `E:\Apps\image-blit\` writeable — the file system semantics during chainloaded-XBE execution may be different from the FTP-server-time semantics. nxdk's `nxMountDrive` may report success but the FATX driver may be in a different state than a freshly-rebooted UnleashX provides for the FTP server. This is speculative — needs evidence.

**Codex validation.** Mode: `changes`. Verdict: **MINOR ISSUES**. Two findings, both adopted in full:

1. *(medium severity, doc sync)* "Canonical state/docs are still materially behind the recorded cycle-21 outcome at the moment Codex ran" — addressed by this very doc-sync pass.
2. *(low severity, doc consistency)* "`claude-status.md` said all four orchestration-state files are updated but `handoff-summary.md` was not yet touched" — also addressed in this doc-sync pass.

Codex also flagged a genuine open question worth recording: "with E:\ markers still absent on real Xbox, is the remaining discriminator a trivial write-only XBE on the same `runxbe` path, or an A.3 provenance check against the older real-Xbox references?" — this is the right framing for the next bounded slice; Hermes should choose. Validation marker written at `.claude/state/codex-validate-last-run` per rule #15. Prompt + last-message tempfiles removed per skill §6.

**Files produced (evidence preserved on disk).**

- `scripts/apple-silicon/xbe-tests/image-blit/main.c` — cycle-21 instrumentation diff (uncommitted at session close).
- `scripts/apple-silicon/xbe-tests/image-blit/bin/default.xbe` + `image-blit.iso` — rebuilt; ~159 744 B XBE / 720 896 B ISO.
- `benchmark-runs/cycle21-image-blit-markers-local-metal-guestlog-20260522T231623Z/` — first local xemu run (no dir-create; surfaced the local-validation oversight).
- `benchmark-runs/cycle21-image-blit-markers-local-metal-guestlog-20260522T231823Z/` — second local xemu run with `CreateDirectoryA`; 52/52 markers fire, 0/52 fopen-failed, byte-identical v0.4 tally drift to cycle 20.
- `benchmark-runs/cycle21-real-xbox-image-blit-markers-20260522T232031Z/` — single real-Xbox run; 22.4 s gap, 1 file retrieved (`default.xbe` upload echo), 0 marker files.

**Cycle 11 follow-up list — updated.**

- ✅ #1 (Re-run image-blit on GL) — CLOSED cycle 15.
- ✅ #2 (`XEMU_DIAG_PGRAPH_STATUS_DRAIN`) — CLOSED cycle 17.
- 🟡 #3 (Real-Xbox parity check) — **REFRAMED AGAIN cycle 21.** Cycle 20 sharpened the framing from "which stage fails" to "D:\ write-back is blocked." Cycle 21 demonstrates that re-routing to E:\ does NOT unblock the witness, so the blocker is NOT (only) a partition mismatch. Remaining bounded slices:
  - **A.3 (now top-priority).** Cross-check whether the existing `xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor}` captures came from `runxbe` SITE-EXEC chainload or from a different launch path. If from `runxbe`, the cycle-21 result is image-blit-specific and the failure is in the XBE itself (early crash, NV2A init divergence). If from a different launch path, the oracle pipeline needs a non-`runxbe` chainload mode for any XBE that depends on observable file writes. This is the cheapest single follow-up per rule #1 because it requires only inspecting existing artifacts + tooling history.
  - **A.4 (new).** Add a non-fopen witness to image-blit: write a few bytes into the oracle-agent's persistent kernel-pool controller buffer (`oracle_ctrl_buffer` at the agent-published phys address) BEFORE attempting any marker fopen. The next agent boot reads the buffer via `controller.buffer-info` / `controller.get` (known-functional from cycle-17 oracle-smoke evidence). If the buffer changes, the XBE ran for at least that long; if it does not, the XBE crashes before reaching that point. This requires zero filesystem activity and bypasses every partition-mount + FATX-driver-state concern.
  - **B.** Smaller PFIFO-race-only Tier-1 diag XBE that captures via PCRTC. Still on the table per cycle 19's recommendation list. Heavier than A.3 / A.4.

**Out-of-scope (cycle-21 kept bounded).**

- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on.
- Did NOT start Path A.3 (`xbox-real-references/` provenance audit) — that's the next bounded slice; Hermes chooses.
- Did NOT start Path A.4 (oracle-agent kernel-pool buffer witness) — also pending Hermes selection.
- Did NOT start Path B (smaller PFIFO-race-only Tier-1 diag XBE).
- Did NOT modify xemu-fork host source (instrumentation slice is XBE-only).
- Did NOT relocate `xbed_render_loop_then_capture` capture/done writes (still under D:\); cycle-21 scope was markers only.
- Did NOT touch retail-title metrics, §G.5, RT-as-texture, or any second-wave XBE work.

Cycle 20 details preserved below.

---

## 2026-05-22 (cycle 20 Path A) — image-blit progress-marker instrumentation; D:\ witness path confirmed BLOCKED on real Xbox under `runxbe` chainload

**Status: SHIPPED (XBE-only instrumentation slice; xemu source untouched). Bounded slice CLOSED — markers landed, local validation green, real-Xbox witness re-confirmed BLOCKED.**

**Slice.** Cycle-19 closure left two candidate next slices: Path A
(make `image-blit.iso` real-Xbox-witnessable enough to identify
which stage fails) and Path B (build a smaller PFIFO-race-only
Tier-1 diag XBE that captures via the proven `xbed_capture`
PCRTC path). Cycle-20 took Path A: add early, always-on,
FTP-collectable progress markers to
`scripts/apple-silicon/xbe-tests/image-blit/main.c` so the
existing oracle pipeline's FTP-collect step can distinguish
"crashed at xbed_init", "crashed mid-blit", "crashed during
oracle compare", "crashed during shader load", "completed but
D:\ fopens silently fail", etc.

**Diff (uncommitted in xemu-fork/ at session close).** Only
`scripts/apple-silicon/xbe-tests/image-blit/main.c` — adds a
single helper `image_blit_marker(unsigned idx, const char *stage)`
that writes a tiny text file to
`D:\image-blit-marker-NN-STAGE.txt` and ALSO mirrors the marker
line through the existing `xbed_host_log_writef` channel
(inert on real Xbox / stock xemu without `XEMU_GUEST_LOG=1`).
Markers fire at 13 staged points: 00 program_entered (literal
first line of `main()`, before `xbed_init`), 01 xbed_init_ok,
02 verts_alloc_ok, 03 src_alloc_ok, 04 src_filled, 05
dst_alloc_ok, 06 before_blits, 07 after_cell0 (specifically
isolates the first IMAGE_BLIT fire + first `pb_agp_access`
oracle readback), 08 after_all_blits, 09 state_set, 10
shaders_loaded, 11 geometry_built, 12 pre_capture (last
checkpoint BEFORE `xbed_render_loop_then_capture`). Markers
09 and 12 were shortened from `default_state_set` /
`before_capture_loop` to `state_set` / `pre_capture` after
Codex flagged the originals against the FATX 42-char basename
limit (marker-12 was 44 chars, marker-09 exactly at 42). Helper
is best-effort: failed `fopen` returns silently after surfacing
an `image-blit: marker NN STAGE fopen-failed` line through the
host-log channel; no exit paths added. Bounded reviewable diff
~101 net lines added.

**Local validation (xemu-Metal, `XEMU_GUEST_LOG=1`).** All 13
markers fire in order via the `xemu-guest-log:` host channel.
EVERY `fopen("D:\\image-blit-marker-NN-STAGE.txt","wb")` returns
NULL on the ISO-mount path — `image-blit.iso` is a CD-ROM mount
which is read-only, so `D:\` (mapped to the chainloaded XBE's
parent directory) is also read-only. This was previously masked
because xemu's renderer-side screenshot hook
(`XEMU_METAL_SCREENSHOT_PATH`) writes screenshots to a HOST
filesystem path, NOT through `D:\` — the harness has been
producing PNGs all along despite the XBE's own `D:\` writes
silently failing. The v0.4 3/8 `mask=0x31` tally is unchanged
(same expected_fail verdict as v0.4 baseline; no
instrumentation-induced regression). First local run:
`benchmark-runs/cycle20-image-blit-markers-local-metal-guestlog-20260522T221035Z/`.
Post-Codex re-validate (with shortened labels):
`benchmark-runs/cycle20-image-blit-markers-local-metal-postcodex-20260522T221959Z/`.

**Real-Xbox runs (two, with the post-Codex shortened-label
binary AND the pre-Codex over-limit-label binary).** Both
produced IDENTICAL outcomes: chainload→FTP-back gap 22.4 s
(matches cycle-19 attempts of 22.4 s + 22.3 s; third
reproducibility confirmation), only `default.xbe` retrieved
from `/E/Apps/image-blit/`, ZERO marker files present. Pixel
oracle correctly classifies as `fail: no-xoss-blob-pulled`.

| Attempt | Run dir | Chainload→FTP-back | Marker files retrieved |
|---:|---|---:|---|
| 1 (pre-Codex labels) | `cycle20-real-xbox-image-blit-markers-20260522T221224Z/` | 22.4 s | 0 |
| 2 (post-Codex labels) | `cycle20-real-xbox-image-blit-markers-postcodex-20260522T222048Z/` | 22.4 s | 0 |

**Why marker-00 producing zero files is decision-relevant.**
Marker-00 fires as the literal first line of `main()`, before
`xbed_init()`, before `XVideoSetMode`, before pbkit, before any
NV2A interaction. The marker writes a 40-character basename
(`image-blit-marker-00-program_entered.txt`), well under the
FATX 42-char limit. If `fopen("D:\\…","wb")` worked at all on
the `runxbe` chainload path, marker-00 would be the highest-
likelihood-success file in the entire ladder. Its absence
combined with the absence of marker 01..12 makes the
discriminating signal strong: **`D:\` fopen for write is
blocked on the `runxbe` SITE-EXEC chainload path on this
console, regardless of XBE-side stage.** This is consistent
with cycle-19 hypothesis #1 (D:\ remap mismatch under `runxbe`
chainload).

**Conservative reading — what this DOES NOT prove.**

1. We have NOT proven the XBE crashes at any specific stage. We
   have only proven that no `D:\…` file written by the XBE
   reaches FTP. The XBE may run to completion with every fopen
   failing, OR it may crash at any point along the way; the
   surface evidence is identical in either case.
2. We have NOT proven that the existing pipeline-smoke / mirror /
   color-channel / depth-floor real-Xbox references (in
   `docs/apple-silicon/xbox-real-references/`) were captured via
   the same `runxbe` SITE-EXEC chainload path. If those were
   captured by a different launch path (e.g. via `XLaunchXBE`
   from an ISO mount through a different shell), the D:\
   behavior may genuinely differ; that needs cross-checking
   before the leading hypothesis is treated as a single
   universal fact.
3. Path A as originally framed ("identify which stage fails on
   real Xbox") is NOT directly delivered — markers couldn't get
   off-board on real Xbox. What IS delivered is sharper framing:
   the witness-mechanism blocker is `D:\` write-back under
   `runxbe`, not stage-specific XBE failure modes.

**Codex validation.** Mode: `changes`. Verdict: **MINOR ISSUES**
(strengths cited; one medium-severity finding flagged the FATX
42-char basename overflow for marker 12 + at-limit marker 09).
Findings adopted in full: both labels shortened, comment block
re-grounded against the FATX limit with an explicit ≤14-char
label budget, binary rebuilt, validation re-run on real Xbox
with the corrected binary. Out-of-scope finding (orchestration
`claude-status.md` mentioned a 9-marker ladder when the code
ships 13) is addressed in this same doc-sync pass. Validation
marker written at `.claude/state/codex-validate-last-run` per
rule #15. Codex stream + last-message files removed at close
per skill §6.

**Files produced.**

- `scripts/apple-silicon/xbe-tests/image-blit/main.c` —
  instrumentation diff (uncommitted at session close).
- `scripts/apple-silicon/xbe-tests/image-blit/bin/default.xbe`
  + `image-blit.iso` — rebuilt (post-Codex labels). Both are
  uncommitted by-products of the build; will be staged as part
  of the cycle-20 commit.
- `benchmark-runs/cycle20-image-blit-markers-local-metal-20260522T220904Z/`
  — first local xemu run (no `XEMU_GUEST_LOG`); confirms pixel
  oracle behavior unchanged.
- `benchmark-runs/cycle20-image-blit-markers-local-metal-guestlog-20260522T221035Z/`
  — local xemu run with host-log channel on; confirms 13
  markers fire and `fopen` fails on ISO mount.
- `benchmark-runs/cycle20-real-xbox-image-blit-markers-20260522T221224Z/`
  — first real-Xbox run (pre-Codex labels); zero markers
  retrieved.
- `benchmark-runs/cycle20-image-blit-markers-local-metal-postcodex-20260522T221959Z/`
  — local xemu re-validate after Codex-driven label shortening;
  same v0.4 tally.
- `benchmark-runs/cycle20-real-xbox-image-blit-markers-postcodex-20260522T222048Z/`
  — second real-Xbox run (post-Codex labels); zero markers
  retrieved (third reproducibility confirmation overall).

**Cycle 11 follow-up list — updated.**

- ✅ #1 (Re-run image-blit on GL) — CLOSED cycle 15.
- ✅ #2 (`XEMU_DIAG_PGRAPH_STATUS_DRAIN`) — CLOSED cycle 17.
- 🟡 #3 (Real-Xbox parity check) — **REFRAMED cycle 20.** No
  longer "which stage fails"; the bounded answer is "D:\
  write-back is blocked on the `runxbe` chainload path." Next
  bounded slices (NOT promoted in this session per the cycle-20
  scope rule):
  - **A.2.** Route the next image-blit instrumentation pass to
    a known-writeable partition (e.g. `T:\` title-data or
    direct paths under `E:\Apps\image-blit\`) and re-run. If
    markers land there, we'll learn the XBE's execution stage.
  - **A.3.** Cross-check the existing
    `xbox-real-references/{pipeline-smoke,mirror,color-channel,depth-floor}`
    captures: confirm whether they were captured via `runxbe`
    chainload (in which case `D:\` writes work for THOSE XBEs
    and image-blit fails for a different reason) or via a
    different launch path (in which case the leading hypothesis
    is universal).
  - **B.** Smaller PFIFO-race-only Tier-1 diag XBE that
    captures via PCRTC. Still on the table per cycle 19's
    recommendation list.

**Out-of-scope (cycle-20 kept bounded).**

- Did NOT flip `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on.
- Did NOT start Path B.
- Did NOT modify xemu-fork host source.
- Did NOT touch retail-title metrics or §G.5 / RT-as-texture
  work.
- Did NOT investigate alternative write-back partitions in this
  cycle — that is Path A.2 and belongs to the next Hermes pass.

Cycle 19 details preserved below.

---

## 2026-05-22 (cycle 19) — real-Xbox image-blit parity check attempted; witness path BLOCKED

**Status: ATTEMPTED — capture reproducibly FAILED on real Xbox
across two bounded attempts; long-term-fix decision deferred.**

**Slice.** Cycle-11 follow-up item #3 promoted by cycle-17 closure:
take the cycle-17 `XEMU_DIAG_PGRAPH_STATUS_DRAIN` result
(`pass=8/8 mask=0xff` on Metal AND GL under the flag) and witness
the same XBE on real Xbox via the existing oracle pipeline. The
parity outcome would inform the long-term-fix shape — match implies
"diagnostic correctly models real-hardware NV_PGRAPH_STATUS"
(justifies default-on path); divergence implies "diagnostic is a
xemu-side overshoot" (different fix needed).

**Pre-conditions verified.**

- `oracle-smoke.sh` 12/12 PASS at 2026-05-22 15:36 CDT (agent up,
  EEPROM sha256=871ed8a9…, controller buffer kernel-pool allocated
  with `anchor_ok=1`, screenshot RPC working).
- Working tree clean on `apple-silicon-performance` except the
  cycle-19 doc updates; HEAD = `b2e4af913f` (cycle-18 packaging).

**What was run.**

```
python3 scripts/apple-silicon/xbe-harness/xbe_orchestrator.py run \
  --xbe image-blit --renderer real-xbox --out <bench-dir>
```

Twice, ~3 minutes apart. Each run:
1. Reboots Xbox to release FTP, uploads `bin/default.xbe` (155 648 B)
   to `/E/Apps/image-blit/`.
2. Re-launches the oracle agent via `SITE EXEC`.
3. Captures `pre.png` (agent splash visible).
4. Chainloads `E:\Apps\image-blit\default.xbe` via the agent's
   `runxbe` RPC.
5. Waits for the Xbox to come back to FTP after the chainloaded XBE
   reboots.
6. FTP-pulls everything under `/E/Apps/image-blit/`.
7. Re-launches the agent, captures `post.png`.

**Result — reproducible across two attempts.**

| Attempt | Run dir | Chainload→FTP-back gap | Files in `/E/Apps/image-blit/` after run |
|--------:|---|---:|---|
| 1 | `benchmark-runs/cycle19-real-xbox-parity-image-blit-20260522T203718Z/` | 22.4 s | `default.xbe` (the upload) only |
| 2 | `benchmark-runs/cycle19-real-xbox-parity-image-blit-retry-20260522T204139Z/` | 22.3 s | `default.xbe` (the upload) only |

Neither attempt produced `D:\image-blit-capture.bin` or
`D:\image-blit-done.txt`. The orchestrator's `verdict.json`
reports `status: ok` for the chainload-and-collect cycle itself —
the Xbox successfully rebooted, FTP came back inside the 120-retry
window, and the post-run agent splash matches the pre-run splash.
The harness's pixel oracle correctly flips the cell to `fail` with
`notes: no-xoss-blob-pulled` (the XOSS decoder has nothing to
decode).

`pre.png` and `post.png` for both attempts show only the
oracle-agent boot splash — the agent re-launched cleanly after
the XBE's reboot, so the screenshot capture sees the agent's
fresh init, not whatever the XBE rendered. Without composite
capture (MS2109) we cannot determine from these screenshots alone
whether the XBE crashed during init / class-object instantiation /
IMAGE_BLIT submission / `pb_agp_access()` VRAM readback /
`xbed_capture_front_to_xoss()`'s `fopen("D:\\…","wb")`, or whether
it completed and the D:\ fopens silently failed because real
Xbox's chainload-launched-from-`E:\Apps\image-blit\` does not
remap D:\ the way `XLaunchXBE` does for ISO-mounted launches.

**Why the parity check is decision-relevant, and why it cannot
land today.** The cycle-17 conclusion — "publishing a meaningful
`NV_PGRAPH_STATUS` busy bit gated on PFIFO drain eligibility flips
image-blit from `3/8 mask=0x31` to `8/8 mask=0xff` on both Metal
and GL with zero changes to renderer code" — is a xemu-side
finding. To justify a default-on flip, we need real-Xbox evidence
that hardware ALSO reports `8/8` under the same workload, i.e.
that the diagnostic faithfully models real-NV2A behavior rather
than over-correcting. Without that, defaulting the flag on risks
"fixed on xemu but diverges from real hardware" — exactly the
failure mode rule #17 (XBE-first development loop is binding for
the Metal renderer; the retail-game oracle is the final acceptance
gate) was adopted to prevent.

**Decision.** The `XEMU_DIAG_PGRAPH_STATUS_DRAIN` default-on /
long-term-fix shape decision is **DEFERRED**. The flag continues
to ship opt-in, default OFF; nothing about cycle-17's local
finding is invalidated.

**Hypotheses for the witness-path failure (NOT decided today —
listed only to bound the next cycle).**

1. **D:\ remap mismatch.** `XLaunchXBE` on real Xbox, when chained
   from the oracle agent (`SITE EXEC` launch from FTP), may not
   remap D:\ to point at the chainloaded XBE's directory the way
   `-dvd_path image-blit.iso` does on xemu. If true, both
   `xbed_capture_front_to_xoss(D:\\…)` and the done-marker fopen
   silently fail, but the orderly reboot still happens. Other
   real-Xbox Tier-1 XBEs (`mirror`, `color-channel`, `depth-floor`)
   captured byte-exact references via this same harness on
   2026-05-07, which is some evidence against a generic D:\ failure
   — but image-blit is a Tier-2 XBE with additional D:\ usage
   patterns that may differ.
2. **NV2A class-object instantiation mismatch.** image-blit
   pre-binds channel 3 (NV_09F IMAGE_BLIT) and channel 4 (NV_062
   CONTEXT_SURFACES_2D) via pbkit handles 3/4 to avoid the
   `set_draw_buffer()` reprogramming of channels 9/11 (see
   manifest.json). Real NV2A may enforce stricter instantiation
   rules that xemu doesn't, triggering an NV2A exception that the
   pbkit pre-bind doesn't recover from.
3. **`pb_agp_access()` divergence.** image-blit reads dst VRAM
   via the cache-coherent linear AGP remap. Real NV2A's AGP
   aperture behavior may not match xemu's emulated linear path.
4. **Some other early-init failure.** Memory alignment, PFIFO
   programming order, or a pbkit init step that's safe on xemu
   but not on real Xbox.

**Next bounded slice (cycle 20+).** Restore the witness path. Two
candidate paths, both legitimate per `diagnostic-xbe-plan.md` v2
and rule #5 (build tools when the toolset is the limit):

- **A. Make image-blit real-Xbox-witnessable.** Add an
  always-on, FTP-collectable diagnostic file the XBE writes
  EARLY (e.g. before pbkit init), and stage subsequent progress
  markers through additional fopens. Verify which stage fails on
  real Xbox by which marker files exist after the chainload+reboot
  cycle. This is a cheap, ladder-style targeted-test slice per
  rule #1.
- **B. Build a smaller PFIFO-race-only XBE.** A new Tier-1 diag
  that exercises ONLY the `pb_wait_until_gr_not_busy()` semantics
  against a workload that races against NV_PGRAPH_STATUS — no
  IMAGE_BLIT, no pb_agp_access, no per-cell dashboard encoding.
  Capture via the existing PCRTC-based xbed_capture path that's
  already proven on real Xbox. Pass/fail signal is just a binary
  "tally drained before timeout / didn't."

**A** is the cheapest, **B** is the most robust. The cycle-20
scope choice belongs to the next Hermes pass.

**Codex validation.** Cycle 19 is doc-only + evidence-preservation
(no xemu-fork code changes; aggregate uncommitted diff is the
four orchestration-state files + this handoff entry + the
decision-log cycle-19 entry). Rule #15's `/codex-validate changes`
trigger (>30-line uncommitted diff on renderer / TCG / NV2A /
build / apple-silicon scripts) does not fire on doc-only work.

**Files produced.**

- `benchmark-runs/cycle19-real-xbox-parity-image-blit-20260522T203718Z/`
  (attempt 1; report.md + summary.json + image-blit/real-xbox/
  real-xbox.log + orch/{pre,post}.png + orch/verdict.json +
  orch/artifacts/default.xbe).
- `benchmark-runs/cycle19-real-xbox-parity-image-blit-retry-20260522T204139Z/`
  (attempt 2; same shape).

**Cycle 11 follow-up list — updated.**

- ✅ #1 (Re-run image-blit on GL) — CLOSED cycle 15.
- ✅ #2 (`XEMU_DIAG_PGRAPH_STATUS_DRAIN`) — CLOSED cycle 17.
- 🟡 #3 (Real-Xbox parity check) — **ATTEMPTED cycle 19, BLOCKED
  on the XBE's real-Xbox witness mechanism, NOT on the diagnostic
  itself.** Promoted to cycle 20 with two candidate paths
  (witness-on-image-blit vs smaller-PFIFO-race-XBE).

**M15 default-on Gate 2 status — UNCHANGED from cycle 17.**

- §E.13 per-format pitch + image-rect alignment — MET (cycle 10).
- §H.6 IMAGE_BLIT — **MET under the flag locally (cycle 17);
  default-off pending the real-Xbox witness path (cycle 19's
  finding).**
- §G.5 Z compression boundary — still unstarted.
- RT-as-texture sampling XBE — still unstarted.

Cycle 17 details preserved below.

---

## 2026-05-22 (cycle 17) — `XEMU_DIAG_PGRAPH_STATUS_DRAIN` lands; §H.6 race window closed renderer-agnostically

**Status: SHIPPED (opt-in diagnostic only; default OFF).**

**Slice.** Implement the cycle-13 follow-up item #2 promised by
both cycle 13 and cycle 15: an opt-in diagnostic flag that
publishes a meaningful `NV_PGRAPH_STATUS` busy bit so the Xbox
guest's `pb_wait_until_gr_not_busy()` actually waits until PFIFO
has drained the pushbuffer, instead of exiting on the first
iteration against a permanently-zero register.

**What landed.**

- `hw/xbox/nv2a/pgraph/pgraph.c` — new `pgraph_status_drain_enabled()`
  cached env-var helper (mirrors `pgraph_fast_read_enabled()` at
  `pgraph.c:97-113`) and an early-return branch at the top of
  `pgraph_read()`. When the flag is on AND `addr == NV_PGRAPH_STATUS`,
  returns `STATE_BUSY` (bit 0) iff `dma_put != dma_get` **and** the
  PFIFO pusher is currently eligible to drain. The eligibility gate
  mirrors `pfifo_run_pusher`'s top-of-function guard
  (`hw/xbox/nv2a/pfifo.c:309-313`) plus its inner stall set
  (`pfifo_pusher_stall_reasons`, `pfifo.c:283-294`): `PUSH0_ACCESS` /
  `DMA_PUSH_ACCESS` set, `DMA_PUSH_STATUS` (suspended) clear,
  `NV_PGRAPH_FIFO_ACCESS` set, `pgraph.waiting_for_nop` and
  `pgraph.waiting_for_context_switch` both clear.
- `hw/xbox/nv2a/nv2a_regs.h` — adds `NV_PGRAPH_STATUS = 0x00000700`
  and `NV_PGRAPH_STATUS_STATE_BUSY = (1<<0)`.
- `scripts/apple-silicon/xbe-harness/xbe_renderers.py` — opt-in
  `XBE_HARNESS_TIMEOUT_SECONDS` env-var override for the
  `timeout_seconds` parameter (default 35 s unchanged). Necessary
  because the flag adds latency to every `pb_wait_until_gr_not_busy()`
  call and the GL leg's BIOS/pbkit boot needs ~120 s to complete one
  XBE pass under the flag.
- `scripts/apple-silicon/xbe-harness/README.md` — documents the
  override (Codex finding #2).
- `docs/apple-silicon/automation.md` — full flag description +
  validation-evidence subsection.
- `.claude/rules/flags-renderer.md` + `.claude/rules/flags-bench.md`
  — 1-line index entries.

**Validation evidence.**

| Run | Boots | Per-boot tally | Cycle-15 baseline |
|---|---:|:---:|:---:|
| Metal (35 s harness window, flag ON) | 4 | `pass=8/8 mask=0xff` | `pass=3/8 mask=0x31` |
| GL (120 s harness window, flag ON) | 15 | `pass=8/8 mask=0xff` | `pass=3/8 mask=0x31` |
| Metal (baseline, flag OFF) | 2 | `pass=3/8 mask=0x31` | — |
| GL (baseline, flag OFF) | 2 | `pass=3/8 mask=0x31` | — |

- Metal flag-on: `benchmark-runs/cycle17-status-drain-metal-PASS-gl-timeout-20260522/image-blit/metal/xemu.log`.
- GL flag-on: `benchmark-runs/cycle17-status-drain-gl-long-timeout-20260522/image-blit/gl/xemu.log`.
- Metal baseline: `benchmark-runs/cycle17-baseline-no-drain-metal-20260522/image-blit/metal/xemu.log`.
- GL baseline: `benchmark-runs/cycle17-baseline-gl-only-20260522/image-blit/gl/xemu.log`.
- Two failed-iteration evidence dirs preserved for the
  eligibility-gate lesson:
  `benchmark-runs/cycle17-status-drain-FIRST-ATTEMPT-too-aggressive-20260522/`
  (no gate — guest hung at BIOS) and
  `benchmark-runs/cycle17-status-drain-2nd-attempt-also-hung-20260522/`
  (PUSH0/DMA_PUSH gates only — still hung; needed
  `NV_PGRAPH_FIFO_ACCESS` + `waiting_for_nop` +
  `waiting_for_context_switch`).

**Cycle 13 race hypothesis — DEFINITIVELY CONFIRMED.** The §H.6
IMAGE_BLIT 5/8 FAIL residual was the predicted PFIFO ↔ vCPU
dispatch race against missing `NV_PGRAPH_STATUS` publication.
Publishing a meaningful busy bit (gated on actual drain eligibility)
flips every previously-FAIL cell to PASS on both renderers, with
zero changes to `gl/blit.c` / `mtl/blit.c` / `vk/blit.c`. This
also closes cycle-11 follow-up item #2.

**Codex validation.** `/codex-validate changes` returned MINOR
ISSUES (one medium, one low). Both adopted before close (see
`decision-log.md` 2026-05-22 cycle 17 entry).

**Cycle 11 follow-up list status update.**

- ✅ #1 (Re-run image-blit on GL) — CLOSED cycle 15.
- ✅ #2 (`XEMU_DIAG_PGRAPH_STATUS_DRAIN`) — **CLOSED by this slice.**
  Diagnostic ships opt-in; default-on / long-term fix shape pending.
- ⏭ #3 (Real-Xbox oracle parity check) — promoted to next bounded
  slice. Gates the default-on / busy-bit-vs-PFIFO-barrier decision.

**M15 default-on Gate 2 status — IMPROVED.**

- §E.13 per-format pitch + image-rect alignment — MET (cycle 10).
- §H.6 IMAGE_BLIT — **PARTIAL → MET under the flag (cycle 17),
  default-off pending real-Xbox parity.**
- §G.5 Z compression boundary — still unstarted.
- RT-as-texture sampling XBE — still unstarted.

XBE first-wave Metal count unchanged at **17 of 18 PASS on Metal
+ 1 expected_fail SPEC** (`logic-ops`). Second-wave coverage now
**2 MET + 1 PARTIAL out of 4** (with image-blit MET conditional on
the flag).

Cycle 15 details preserved below.

---

## 2026-05-22 (cycle 15) — §H.6 renderer-agnostic confirmation + reusable host-visible guest-log channel

**Status: SHIPPED — host-visible Tier-2 oracle output channel + first-adopter retargeted (image-blit v0.4).**

**Slice.** Smallest durable host-visible output path for Tier-2
diagnostic XBEs whose per-cell oracle verdicts otherwise depend on
GL/Metal screenshot capture. Renderer-agnostic by construction;
opt-in by env var so retail runs are unaffected.

**What landed.**

- `hw/xbox/xbox_guest_log.c` (NEW, ~110 lines): opt-in IO-port sink
  at port 0xE9. Bytes accumulate into a 512-byte line buffer; `\n`
  / `\0` / buffer-full flushes to `stderr` with `xemu-guest-log:`
  prefix. Activation gated by `XEMU_GUEST_LOG=1`. Wired into
  `xbox_init_common` (`hw/xbox/xbox.c:344`) and declared in
  `hw/xbox/xbox.h`. Build-system entry in `hw/xbox/meson.build`.
- `scripts/apple-silicon/xbe-tests/lib/xbed_runtime.{h,c}`: shared
  `xbed_host_log_write[f]()` helpers using GCC inline `outb` to a
  fixed compile-time port `XBED_HOST_LOG_PORT = 0xE9`. Helpers
  safe to call unconditionally — silently absorbed by unmapped IO
  on real Xbox / stock upstream xemu.
- `scripts/apple-silicon/xbe-tests/image-blit/main.c`: mirrors the
  existing per-cell PASS|FAIL + first-mismatch lines through the
  new channel alongside `debugPrint`; adds session-begin anchor
  line and post-Phase-1 tally line
  `image-blit: tally pass=N/8 mask=0xXX`.
- `scripts/apple-silicon/xbe-tests/image-blit/manifest.json`:
  bumped to v0.4; adds `"gl"` to `expected_fail_renderers` to
  record the renderer-agnostic truth uncovered by the new channel.

**Cycle 13 race hypothesis — CONFIRMED renderer-agnostic.**

Captured 2026-05-22 12:31 CDT under
`benchmark-runs/xbe-cycle15-host-log-20260522-123038/`:

| Run # | Metal tally        | GL tally           |
|------:|:------------------:|:------------------:|
| 1     | 3/8 (mask=0x31)    | 3/8 (mask=0x31)    |
| 2     | 3/8 (mask=0x31)    | 3/8 (mask=0x31)    |
| 3     | 2/8 (mask=0x30)    | 2/8 (mask=0x30)    |
| 4     | 2/8 (mask=0x30)    | 3/8 (mask=0x31)    |

mask=0x31 ↔ cells 0, 4, 5 PASS (the same triplet cycle-11/12/13
identified on Metal v0.2/v0.3). First-mismatch records also match
across renderers: every FAIL cell shows `got=0xff808080`
(sentinel), `expected=0xffff0000` (RED), at `(mx=0, my=0)`. The
small per-boot mask drift is expected under the cycle-13
PFIFO/vCPU race hypothesis (small race window → small variance).
This is the renderer-agnostic confirmation cycle 14 could not
produce because the GL screenshot path is upstream-blocked.

**Codex validation.** `/codex-validate changes` returned MAJOR
ISSUES with three actionable findings; all three adopted before
close (see decision-log cycle-15 entry). Notable adoption: dropped
the runtime `XEMU_GUEST_LOG_PORT` host override so the port is
fixed end-to-end on both sides (Codex finding #2 — a runtime
override without a matching XBE rebuild would silently disconnect
the channel).

**Cycle 11 follow-up list status update.**

- ✅ #1 (Re-run image-blit on GL) — CLOSED by this slice.
  Renderer-agnostic verdict confirmed; bug is in shared
  PFIFO/PGRAPH machinery, NOT in `mtl/blit.c`.
- ⏭ #2 (`XEMU_DIAG_PGRAPH_STATUS_DRAIN`) — promoted to next
  bounded slice. Codex mandatory.
- ⏳ #3 (Real-Xbox oracle parity check) — deferred until #2 lands
  and flips all 8 cells green locally.

**M15 default-on Gate 2 status — UNCHANGED from cycle 13.**

- §E.13 per-format pitch + image-rect alignment — MET (cycle 10).
- §H.6 IMAGE_BLIT — PARTIAL (3/8 cells green first-run; residual
  now CONFIRMED renderer-agnostic by cycle 15; expected to close
  via `XEMU_DIAG_PGRAPH_STATUS_DRAIN`).
- §G.5 Z compression boundary — still unstarted.
- RT-as-texture sampling XBE — still unstarted.

XBE first-wave Metal count unchanged at **17 of 18 PASS on Metal
+ 1 expected_fail SPEC** (`logic-ops`). Second-wave coverage now
**1 MET + 1 PARTIAL out of 4**.

Cycle 13 details preserved below.

---

## 2026-05-22 (cycle 13) — §H.6 `image-blit` residual REFRAMED — PFIFO ↔ vCPU dispatch race against missing NV_PGRAPH_STATUS publication

**Status: AUDIT-ONLY (doc-only slice; zero `xemu-fork/hw/` or
`xemu-fork/scripts/apple-silicon/` diff).**

**Slice goal.** Take ONE bounded diagnostic step toward the
cycle-12 "guest CPU / TCG VRAM read-back coherency" hypothesis
for §H.6 `image-blit`. Outcome: hypothesis sharpened with
file/line evidence; mechanism reframed from "TLB / page-
attribute coherency" to "missing PGRAPH busy publication +
relaxed-atomic fast read + async PFIFO kick → vCPU reads
VRAM before PFIFO has processed the IMAGE_BLIT push."

**Concrete evidence chain.** (Full citations in
`decision-log.md` 2026-05-22 cycle-13 entry.)

- PFIFO thread runs `pgraph_mtl_image_blit`
  (`hw/xbox/nv2a/nv2a.c:248`,
  `hw/xbox/nv2a/pfifo.c:226-272`,
  `hw/xbox/nv2a/pgraph/mtl/renderer.c:2525`,
  `hw/xbox/nv2a/pgraph/mtl/blit.c:215-221`).
- xemu never writes `NV_PGRAPH_STATUS` (0x400700). Repo-wide
  `grep -rn '0x400700\|PGRAPH_STATUS' hw/xbox/nv2a/` returns
  zero hits. `pg->regs_[0x400700]` is permanently `0 =
  NV_PGRAPH_STATUS_NOT_BUSY`
  (`nxdk/lib/pbkit/outer.h:461-462`).
- Default-on fast path returns it with no acquire
  (`hw/xbox/nv2a/pgraph/pgraph.c:115-150`,
  `include/qemu/atomic.h:77-84`).
- `pb_wait_until_gr_not_busy` exits on the first iteration
  (`nxdk/lib/pbkit/pbkit.c:486-494`).
- PFIFO kick is pure async signal
  (`hw/xbox/nv2a/pfifo.c:85-116`).

**Why the 3/8 PASS / 5/8 FAIL pattern is consistent.** The
race window between vCPU's IMAGE_BLIT push and vCPU's read is
small but non-zero. Early cells sometimes win the race; later
cells lose more often. The cached-vs-AGP asymmetry (cycle-12)
is naturally explained by the racing PFIFO memcpy interacting
with the different mappings' read serialization differently.
Cycle-12's "renderer memcpy is byte-correct" fprintf
evidence is preserved — those logs fire on the PFIFO thread
and run *eventually*, just not necessarily *before* the per-
cell oracle.

**Renderer-agnostic prediction.** The gap is in PFIFO/PGRAPH
machinery shared by GL, Vulkan and Metal. The same XBE should
exhibit a similar PASS/FAIL split under `XEMU_RENDERER=GL`.
The cycle-11 follow-up item #1 ("Re-run image-blit on GL")
is now the sharpest single confirmation experiment.

**Cycle-14 entry plan (NOT started this slice).**

1. GL leg of `image-blit.iso` through
   `scripts/apple-silicon/xbe-harness`. Race hypothesis
   predicts GL fails the same cells. Confirms/disproves
   renderer-agnostically.
2. If confirmed: add `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1`
   diagnostic env flag wired into
   `pgraph_read(NV_PGRAPH_STATUS)` to return non-zero while
   `pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] !=
   pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]`. Forces
   `pb_wait_until_gr_not_busy` to spin until PFIFO has
   drained the pushbuffer. If all 8 cells PASS, the §H.6
   residual closes and the path to a default-on barrier or
   a properly published busy bit is clear. Codex MANDATORY
   for that slice (non-trivial `hw/xbox/nv2a/` change).
3. Real-Xbox oracle parity check on the SAME XBE after the
   local fix flips all 8 cells green.

**Broader implication.** Every retail title that uses
`pb_wait_until_gr_not_busy` as a software fence between an
IMAGE_BLIT (or other PGRAPH-resident op) and a CPU read of
VRAM almost certainly hits the same race silently. The §H.6
XBE made it visible because the oracle is byte-exact.

**M15 default-on Gate 2 status — UNCHANGED from cycle 12.**

- §E.13 per-format pitch + image-rect alignment — MET (cycle 10).
- §H.6 IMAGE_BLIT — PARTIAL (3/8 cells green; residual reframed
  but not closed).
- §G.5 Z compression boundary — still unstarted.
- RT-as-texture sampling XBE — still unstarted.

XBE first-wave Metal count unchanged at **17 of 18 PASS on Metal
+ 1 expected_fail SPEC** (`logic-ops`). Second-wave coverage
still **1 MET + 1 PARTIAL out of 4**.

Cycle 12 details preserved below.

---

## 2026-05-22 (cycle 12) — §H.6 `image-blit` v0.3 BOUNDED PARTIAL — root cause materially narrowed

**Status: OPTION B — bounded partial closed cleanly.**

**Slice:** `xbe-tests/image-blit/` — same Tier-2 XBE, bumped to
v0.3. Cycle 12 ships item #4 of the cycle-11 residual
investigation list (per-cell first-mismatch debug encode) and
uses two further data-driven control experiments to materially
narrow the residual root cause.

**Per-cell verdict on Metal (unchanged from v0.2): 3/8 PASS
(cells 0, 4, 5), 5/8 FAIL (cells 1, 2, 3, 6, 7).** First
mismatch for every FAIL cell is at `(mx=0, my=0)`, `got =
0xff808080` (sentinel), `expected = 0xffff0000` (RED). Evidence:
`benchmark-runs/xbe-harness-20260522-090124/image-blit/metal/
screenshots/image-blit.0124.png` — visually decodes via the
v0.3 2×2 sub-rect encoding (TL=red banner, TR=got, BL=expected,
BR=`pos_color_argb(mx, my)` — R/G are bucket-of-32 in
`{0, 32, 64, …, 224}`, B nibble-packs the upper 3 bits of mx/my).

### Evidence + narrowing

1. **Renderer memcpy IS byte-correct for all 8 cells.** A
   transient `fprintf(stderr, "xemu-perf: image_blit_cell …")`
   in `hw/xbox/nv2a/pgraph/mtl/blit.c` (capped at 32
   invocations, since reverted) logged per-blit
   `source_offset`, `dest_offset`, `dest_size`,
   `clipped_dest_size`, `adjusted_height`, `leftover_bytes`,
   `row_pixels`, and the pre/post first-pixel dword of
   `dest_row`. Captured in
   `benchmark-runs/xbe-harness-20260522-090729/image-blit/
   metal/xemu.log`. For all 8 cells in the first XBE
   invocation: `dst_pre=0xff808080 dst_post=0xffff0000`. No
   tile clipping engages (`clipped == dest_size` for every
   cell). The renderer is innocent.
2. **Cached-read control disproves a renderer fault.** A v0.3
   variant flipped `oracle_check_cell` to read via the cached
   guest virtual pointer (`s_dst_vram[idx]`) instead of
   `pb_agp_access(s_dst_vram[idx])`. Frame 0124 of
   `benchmark-runs/xbe-harness-20260522-091452/` shows only
   cell 4 PASSes — strictly WORSE than v0.3 (which had 0/4/5
   PASS via AGP read). The cached read is less fresh than the
   AGP-aliased read. Both views see the same backing memory in
   principle; the difference proves the guest CPU is hitting a
   stale read path.
3. **PASS/FAIL asymmetry under the read-back-bug hypothesis.**
   Cells 4/5 PASS via AGP read because their oracle's first
   mismatch would be INSIDE the rect (`out=(4,4)` /
   `out=(8,8)`); the outside-rect pixels at `dst[0,0]` are
   EXPECTED to be sentinel, so a stale-sentinel read still
   matches the oracle. The remaining asymmetry — cell 0 PASS
   with AGP vs cell 3 FAIL with AGP, both `out=(0,0)` — is the
   surviving puzzle. Working sub-hypothesis: cold TCG TB cache
   on the first iteration, or per-page TLB invalidation
   semantics that the cycle-13 follow-up must ground.

### Disproved cycle-11 hypotheses

- Shared blit math bug (`mtl/blit.c:181-233`, mirrored in
  gl/vk) — DISPROVED by the per-blit fprintf evidence.
- Tile-limit clipping via `nv_clip_gpu_tile_blit`
  (`nv2a.c:89-107`) — DISPROVED (`clipped == dest_size`
  always).
- Surface-cache download corruption — DISPROVED (no
  cache entries for the XBE's never-rendered VRAM buffers).
- XBE-side oracle bug — DISPROVED (math symmetric across
  cells; cycle 11 inspection still holds).

### Cycle-12 v0.3 ships

- `scripts/apple-silicon/xbe-tests/image-blit/main.c`:
  - `CellDiag s_cell_diag[GRID_CELLS]` (has_mismatch, mx, my,
    got, expected).
  - `oracle_check_cell(idx, ...)` populates the diag on the
    first mismatched pixel.
  - `build_dashboard_geometry()` emits 4 sub-quads per cell:
    TL=red, TR=got color, BL=expected color, BR=
    `pos_color_argb(mx, my)` (R = `(mx & 7) * 32`, G =
    `(my & 7) * 32` — bucket-of-32 values in `{0, 32, 64, …,
    224}` so the low 3 bits survive Apple gamma; B =
    `((mx >> 3) << 4) | (my >> 3)` nibble-packs the upper 3
    bits, narrower range but mx/my are bounded by the 32×32
    max blit rect).
  - Banner: `image-blit v0.3 (cycle 12 first-mismatch diag
    encoding)`.
- `manifest.json`: title bumped to v0.3,
  `expected_fail_notes` rewritten to reflect the narrowed
  root cause + cited evidence.
- `README.md`: cycle-12 status section + cited evidence +
  disproved-hypothesis list + decoder for the BR sub-rect.
- `hw/xbox/nv2a/pgraph/mtl/blit.c` — diag fprintf REVERTED
  (the evidence is durable in
  `xbe-harness-20260522-090729/.../xemu.log`).

### Cycle-13 follow-up (not started, NOT shipping this cycle)

Investigate guest-CPU cache-coherency between xemu's host
pgraph memcpy writes (to `d->vram_ptr + phys`) and the guest
TCG vCPU reads through (a) the cached kernel virtual mapping
`0x80000000 + phys` and (b) the AGP-aliased mapping
`0xF0000000 + phys`. Suspect either a TLB / dirty-bit
interaction in QEMU softmmu or a page-attribute discrepancy
between the cached / AGP-aliased mappings on Apple-Silicon
TCG. Possible diagnostic tools: temporary print in
softmmu_template's `MMU_HELPER_LD` hot path, or a Tracer
hook around `memory_region_dispatch_read` for the RAM region.

### M15 default-on Gate 2 status update

- §E.13 per-format pitch + image-rect alignment — **MET** (cycle 10).
- §H.6 IMAGE_BLIT — **PARTIAL** (cycle 12; 3/8 cells green,
  residual now grounded to guest read-back coherency, not
  renderer).
- §G.5 Z compression boundary — still unstarted.
- RT-as-texture sampling XBE — still unstarted.

XBE first-wave Metal count unchanged at **17 of 18 PASS on Metal +
1 expected_fail SPEC** (`logic-ops`). Second-wave coverage now
reads **1 MET + 1 PARTIAL (root cause narrowed) out of 4**.

Cycle 11 details preserved below.

---

## 2026-05-22 (cycle 11) — §H.6 `image-blit` v0.2 BOUNDED PARTIAL on Metal

**Status: OPTION B — bounded partial closed cleanly per cycle exit option B.**

**Slice:** `xbe-tests/image-blit/` — Tier-2 NV2A diag XBE covering
§H.6 (`NV_IMAGE_BLIT` class 0x9F + `NV_CONTEXT_SURFACES_2D`
class 0x62, SRCCOPY only, single LE_A8R8G8B8 format).

**Original failure (v0.1, prior session).** v0.1 routed NV062
source/destin DMA through pbkit handles 9 and 11. On Metal,
the FIRST IMAGE_BLIT asserted at
`hw/xbox/nv2a/pgraph/mtl/blit.c:170`
(`source_offset < source_dma_len`) and aborted xemu.
`METAL_IMAGE_BLITS = 0`. Crash artifact:
`benchmark-runs/xbe-harness-20260522-071449/`.

**Root cause grounded via qemu trace.** `pb_init` calls
`pb_target_back_buffer() → set_draw_buffer()`
(`lib/pbkit/pbkit.c:1611-1668`, called at `pbkit.c:3260`), which
**reprograms PRAMIN for channels 9 and 11**:
`addr = framebuffer_base`, `limit = height*pitch-1`
(= 0x0012BFFF for 640×480 LE_A8R8G8B8). Verified by running xemu
with `-trace events=nv2a_dma_map`: sDmaObject9's instance entry
shows `addr=0x03BD4000 limit=0x0012BFFF` just before the
assertion. Channels 9 and 11 are pbkit-reserved scratch DMA
contexts for the back/front buffer aperture; NOT general-purpose
RAM channels after pb_init returns.

**Fix (v0.2).** Switch `IMAGE_BLIT_DMA_HANDLE_SRC = 3` /
`IMAGE_BLIT_DMA_HANDLE_DST = 4` in `nv2a_regs_image_blit.h`.
Channels 3 and 4 are created with `base=0, Limit=MAXRAM`
(`pbkit.c:2643,2645`) and pbkit never reprograms them after
`pb_init`. NV062 does not validate the DMA channel class, so
3 (CLASS_3D) and 4 (CLASS_3) are both legal NV062 source/dest
channels. v0.2 also replaces `HighestAcceptableAddress=0x3FFB000`
with `MAXRAM` in three `MmAllocateContiguousMemoryEx` sites
(conservative; matches pbkit pattern at `pbkit.c:2297-2305`).

**Result on Metal (v0.2).** No more assertion crash.
`benchmark-runs/xbe-harness-20260522-075241/image-blit/metal/`:
- `signal_match_pct = 37.5000` (3 of 8 cells PASS byte-correct).
- `changed_pixels_pct = 62.7083`.
- Best frame `image-blit.0124.png`.

Per-cell verdict:

| Cell | In(x,y) | Out(x,y) | W×H   | Verdict     |
|------|---------|----------|-------|-------------|
| 0    | (0,0)   | (0,0)    | 8×8   | **PASS**    |
| 1    | (0,0)   | (0,0)    | 16×16 | FAIL        |
| 2    | (0,0)   | (0,0)    | 32×32 | FAIL        |
| 3    | (8,8)   | (0,0)    | 8×8   | FAIL        |
| 4    | (0,0)   | (4,4)    | 8×8   | **PASS**    |
| 5    | (4,4)   | (8,8)    | 8×8   | **PASS**    |
| 6    | (0,0)   | (0,0)    | 1×16  | FAIL        |
| 7    | (0,0)   | (0,0)    | 16×1  | FAIL        |

PASS cells share `width == height == 8 AND (in_x,in_y) ≤ (4,4)`.

**METAL_IMAGE_BLITS=0 is the expected steady-state** for this XBE
(uses never-rendered VRAM, so the surface cache stays empty and
`pgraph_mtl_surface_blit_copy` takes Path C without incrementing;
the CPU memcpy at `mtl/blit.c:215-221` is the load-bearing path
and runs unconditionally). README + decision-log document this so
the next session doesn't chase the wrong suspect.

**Manifest.** v0.2 declares `expected_fail_renderers=["metal"]` with
`expected_fail_notes` citing the decision-log entry. Matrix runner
treats v0.2 as known-not-green on Metal (not a regression).

**Codex validation.** v0.2 → MAJOR ISSUES (README/manifest stale,
no `expected_fail`, claude-status misleads on METAL_IMAGE_BLITS=0).
All three findings adopted in this cycle.

**Residual hypothesis (next bounded slice).** The 5/8 failure
pattern looks like a shared-blit-path issue
(`mtl/blit.c:181-233` mirrored in `gl/blit.c:123-187` and
`vk/blit.c:127-191`) or tile-limit clipping via
`nv_clip_gpu_tile_blit` (`nv2a.c:89-107`) against PFB tile
registers inherited from the chainloading UnleashX dashboard. The
XBE-side oracle (`main.c:262-378`) was inspected and looks
internally consistent. Next session investigation list (per
decision-log cycle 11):
1. Re-run image-blit on GL (proves shared-vs-Metal-specific).
2. Inspect `nv_clip_gpu_tile_blit` against runtime PFB tile state.
3. Diff `mtl/blit.c` against `gl/`+`vk/` siblings.
4. Add a per-cell first-mismatch debug encode in the FAIL cell
   color so the residual pixel reveals itself in the captured PNG.

**M15 default-on Gate 2 status update.**
- §E.13 per-format pitch + image-rect alignment — **MET** (cycle 10).
- §H.6 IMAGE_BLIT — **PARTIAL** (this cycle; 3/8 cells green).
- §G.5 Z compression boundary — still unstarted.
- RT-as-texture sampling XBE — still unstarted.

XBE first-wave Metal count unchanged at **17 of 18 PASS on Metal +
1 expected_fail SPEC** (`logic-ops`). Second-wave coverage now
reads **1 MET + 1 PARTIAL out of 4**.

Cycle 10 details preserved below.

---
**§4.13 now PASSES Metal (all 16 cells byte-exact; harness:
1 pass, 0 fail, 2026-05-22T04:29:05Z). Two root causes found and
fixed: (1) XBE combiner D_SOURCE=0x0C→0x04 bug (v0.2 ALL-BLACK
was an XBE code bug, NOT a Metal renderer bug); (2)
`pgraph_is_texture_stage_active()` incorrectly excluded
PASS_THROUGH mode 4 from active-stage detection (pgraph.h:331,
removed `mode != 4`). XBE first-wave PASS count: 17 of 18 PASS
on Metal + 1 expected_fail (logic-ops SPEC).** §4.13 v0.2 expands the 4x2 grid
to a 4x3 grid by adding row 2 —
`SHADER_STAGE_PROGRAM=PROGRAM_NONE` (same as row 1) + combiner
ICW `A_SOURCE=V0` (DIFFUSE) with per-cell DIFFUSE = (R,G,B,1),
bypassing the `t0`/`pT0` chain entirely. **Row 2 also produces
pure (0,0,0,0) on Metal.** All 8 XBE-active capture frames
across 3 render-and-reboot cycles are pure BLACK with zero
non-zero pixels (per-row stats archived at
`benchmark-runs/20260522T075639Z-task18-texture-shader-stages-metal-v0.2-baseline/key-evidence/per-row-stats.md`).
Because row 2 keeps `SHADER_STAGE_PROGRAM` constant across all 4
cells, a `NV_PGRAPH_SHADERPROG` dirty-state propagation issue
**alone** is ruled out, but the broader "override path itself
not honored under textured-shader state" interpretation
survives. The surviving candidate root causes for task #18
(refined per Codex finding 2): (a, formerly v0.1 candidate 3)
state-machine interaction between `xbed_load_textured_shaders`
Cg-emitted setup and the XBE's per-cell overrides on Metal;
(b, NEW per Codex 2026-05-22) the row-2 combiner rewrite
(`A_SOURCE=V0`) may not be honored either — if the combiner
update is silently dropped under textured-shader state, row 2's
BLACK output is equally explained by row 1's residual
`A_SOURCE=T0` config still being in effect at draw time. The
dashboard renders correctly and `combiner-basic` (uses
`xbed_load_default_shaders`, no texturing) PASSes in the same
harness session.
v0.2 also identified a SEPARATE bug while reading the source —
`psh.c:142-148` + `pgraph.h:330` (`pgraph_is_texture_stage_active`)
shows that the fork-local gate added in commit `046160d04d`
("Fix Metal boot and texture stability canaries", 2026-05-04)
degrades PASS_THROUGH (mode 4) to NONE in
`state->shader_stage_program` because `pgraph_is_texture_stage_active`
returns false for mode 4. This explains the v0.1 shader-dump
finding (no `vec4 t0 = pT0;` signature) for row 0 but does not
explain row 2's all-BLACK output, so it is queued as a
SEPARATE fix slice rather than task #18 itself. First-wave XBE
rotation unchanged: **16 of 18 PASS on Metal + 2 expected_fail**
(logic-ops neither-renderer SPEC; texture-shader-stages still
tracks task #18) + 1 expected_fail GL-only (swizzle-mipmap task
#17). 0 unstarted of the §4 first-wave priority list. Cycle 7
banner appended; cycle 6 preserved below for continuity.


## 2026-05-22 (cycle 10) — §E.13 `texture-pitch-alignment` v0.2 PASS on Metal

**Status: OPTION A — clean close.** First second-wave Gate 2 XBE shipped and
PASSes the math-derived oracle on Metal byte-correctly.

**Slice:** `xbe-tests/texture-pitch-alignment/` — Tier-1 NV2A diag
XBE covering §E.13 (linear-texture row pitch + IMAGE_RECT
width/height) in `nv2a-feature-surface-research.md`. 4x2 grid, 8
cells, single LU_IMAGE_A8R8G8B8 format, sweeps
`(IMAGE_RECT.width, IMAGE_RECT.height, TEXCTL1.IMAGE_PITCH)` across
baseline / oversized / odd-dimension combinations. v0.2 sizes each
cell's allocation as `pitch * (height + EXTRA_PAD_ROWS)`
(EXTRA_PAD_ROWS = 8) so trailing physical rows beneath the active
rectangle stay sentinel-grey — a renderer that silently rounds
`IMAGE_RECT.height` up to a power of two reads sentinel rather
than uninitialised memory. The full allocation is sentinel-filled
with 0xFF808080 BEFORE the first `height` rows have their leading
`width * bpp` bytes overwritten with the cell's target cube-corner
color, so any pitch-ignored / image-rect-rounded sampling path
visibly leaks sentinel through to the framebuffer.

**Result on Metal (v0.2):** **PASS** byte-correct against the
math-derived oracle. Harness verdict `1 pass, 0 fail` with:
- `changed_pixels_pct = 0.9919` (≪ 3.0 gate)
- `signal_match_pct = 100.0000` (≥ 97 gate)
- captured frame: `texture-pitch-alignment.0124.png`

Durable evidence:
`benchmark-runs/20260522T055517Z-texture-pitch-alignment-metal-v0.2-PASS/`
(report.md, summary.json, key-evidence/{reference.png,
texture-pitch-alignment.0124.png, baseline-crop.png,
candidate-crop.png, diff-amplified.png, per-cell-stats.md}).

**Reuse of existing infrastructure:** copies the
`texture-format-sweep` 4x2 grid + xbed_texture-lib bind path
verbatim; the only divergences are the per-cell
`(width, height, pitch_bytes)` parameterization and the
sentinel-then-overwrite VRAM fill in `fill_pitch_texture()`. No
xbed_lib changes; no renderer changes.

**Codex review (rule #15):** v0.1 raised MAJOR ISSUES — (a) height
oracle gap (allocation was exactly `pitch * height`, so a
height-ignored regression would have hit uninitialised memory
rather than sentinel); (b) cell 4 mislabeled as a baseline (its
pitch=20 carries 4 bytes of padding per row — cell 0 is the only
true `pitch == w * bpp` baseline). v0.2 addresses both: added
EXTRA_PAD_ROWS sentinel rows + corrected the labeling across
main.c / expected.py / manifest / README / docs.

**M15 default-on Gate 2 status update:**
- §E.13 per-format pitch + image-rect alignment — **MET** (this cycle).
- §H.6 IMAGE_BLIT — still unstarted.
- §G.5 Z compression boundary — still unstarted.
- RT-as-texture sampling XBE (PGR2 late-stage-0 class) — still unstarted.

XBE first-wave Metal count unchanged at **17 of 18 PASS on Metal + 1
expected_fail SPEC** (`logic-ops`). Second-wave coverage now reads
**1 of 4 MET**.

**Highest-value next bounded slice:** §H.6 IMAGE_BLIT XBE (NV2A
`NV_IMAGE_BLIT` 2D blit; Tier 2 — guest VRAM oracle per
`diagnostic-xbe-plan.md` §5).

**Codex validation:** v0.1 → MAJOR ISSUES, v0.2 addresses both
findings; see decision-log cycle-10 entry.

Cycle 9 details preserved below.

---

## 2026-05-22 (cycle 9) — M15 default-on gate check after task #18 closure

**Status: OPTION A — gate verdict captured; next slice identified.**

Task #18 closure cleared the final first-wave Metal blocker, so this cycle
re-ran the documented M15 default-on gate using canonical docs,
`diagnostic-xbe-plan.md` §4/§5/§7, `renderer-metal.md`, and the 18 first-wave
XBE manifests.

**Verdict: M15 default-on is NOT MET.**

- **Gate 1 — first-wave XBE saturation:** **MET.** The §4 library now stands at
  **17 of 18 PASS on Metal + 1 expected_fail SPEC** (`logic-ops`, neither
  renderer implements the NV2A logic-op feature).
- **Gate 2 — second-wave coverage of retail-implicated feature surfaces:**
  **NOT MET.** The minimum set remains unstarted: **§E.13 per-format pitch +
  image-rect alignment, §H.6 IMAGE_BLIT, §G.5 Z compression boundary, and an
  RT-as-texture sampling XBE for the late-stage-0 PGR2 class.**
- **Gate 3 — retail-title canary re-verification after XBE-library green:**
  **NOT MET, blocked on Gate 2.**
- **Gate 4 — no correctness bug ≥30 days:** **MET.**

**Highest-value next bounded slice:** **§E.13 per-format pitch + image-rect
alignment XBE** (Tier 1, math-derived oracle). It reuses existing
`texture-format-sweep` / `crtc-publish` infrastructure and directly targets the
surface-shape/alignment class implicated by late PGR2 behavior. After §E.13, do
§H.6 IMAGE_BLIT next, then re-open Gate 3 with paired retail-title validation.

Cycle 8 details preserved below.

---

## 2026-05-22 (cycle 8) — §4.13 `texture-shader-stages` v0.3 sentinel+control-row bisect (task #18 CLOSED)

**Status: OPTION A — CLEAN CLOSE. Task #18 fully resolved.**

v0.3 expands the 4x3 grid to a 4x4 grid (16 cells, CELL_H=120px).
Row 2 is a sentinel combiner (PROGRAM_NONE +
A=B=INVERT(ZERO)=1.0, textured shaders; expected WHITE×4 —
independent of T0/V0). Row 3 is a control row (PROGRAM_NONE +
A=V0/DIFFUSE, DEFAULT shaders; expected RED/GREEN/BLUE/WHITE).

**Root cause 1 (XBE code bug):** FINAL CW0 `D_SOURCE=0x0C`
(PS_REGISTER_R0, never written) should have been `0x04`
(PS_REGISTER_V0). OCW `AB_DST=0x4` writes to PS_REGISTER_V0
(per `psh.c::parse_combiner_output` + `get_var` case
PS_REGISTER_V0). R0 was never written → FINAL read 0 → BLACK.
This was the root cause of v0.2 ALL-BLACK — NOT a Metal renderer
bug. Fixed in `main.c` (`program_combiners_with_a_source` and
`program_combiners_sentinel`).

**Root cause 2 (renderer psh.c gate bug):** After fixing the
D_SOURCE bug, row 0 (PASS_THROUGH + T0) still produced BLACK.
`pgraph_is_texture_stage_active()` (`pgraph.h:331`) returned false
for mode 4 (PASS_THROUGH) due to `mode != 4` exclusion. psh.c:145
then cleared the stage program bits → PASS_THROUGH demoted to NONE
→ T0=0. Fixed by removing `mode != 4`: only PROGRAM_NONE (0) is
inactive. Affects both GL and Metal (shared code path).

**Final result:** all 16 cells byte-exact on Metal (harness:
1 pass, 0 fail, 2026-05-22T04:29:05Z). `expected_fail_renderers: []`.
Task #18 CLOSED. XBE first-wave count: **17 of 18 PASS on Metal
+ 1 expected_fail** (logic-ops SPEC). Codex validation: PASS (no findings).
Cycle 7 preserved below.

---

## 2026-05-22 (Hermes cycle 7) — §4.13 `texture-shader-stages` v0.2 DIFFUSE-source bisect (task #18 investigation bounded partial)

**Status: BOUNDED PARTIAL closed cleanly per cycle exit option B.**
v0.2 lands the DIFFUSE-source bisect row described in v0.1's
`expected_fail_notes` as the next-session-actionable next step;
the bisect ran on Metal and **invalidates v0.1's revised
hypothesis 1** (NV_PGRAPH_SHADERPROG dirty-state propagation).
The fix attempt is **NOT** landed in this session because the
hypothesis it would have addressed is now ruled out.

**Slice scope (v0.2):** 4x3 grid (12 cells), 3 rows; rows 0+1
unchanged from v0.1; row 2 is the new bisect:

- Row 0 (4 cells, y=0..159): SHADER_STAGE_PROGRAM stage0 =
  PASS_THROUGH (0x04); combiner ICW A_SOURCE = T0; per-cell
  TEXCOORD0 = (R,G,B,1) → t0 = pT0 → R0 → fragColor. Expected
  R/G/B/W.
- Row 1 (4 cells, y=160..319): SHADER_STAGE_PROGRAM stage0 =
  PROGRAM_NONE (0x00); combiner ICW A_SOURCE = T0; TEXCOORD0
  mirrors row 0 → t0 = (0,0,0,1) → R0=0 → fragColor=BLACK.
- Row 2 (4 cells, y=320..479, NEW bisect): SHADER_STAGE_PROGRAM
  stage0 = PROGRAM_NONE (0x00); combiner ICW A_SOURCE = V0
  (PS_REGISTER_V0 = 0x4, DIFFUSE); per-cell DIFFUSE =
  (R,G,B,1) → v0 → R0 → fragColor. Bypasses the t0/pT0 chain.
  Expected R/G/B/W identical to row 0 if the combiner+vertex
  attribute path is sound and only the t0/PASS_THROUGH chain is
  broken.

**v0.2 bisect result (Metal):**

Row 0 BLACK + row 1 BLACK + **row 2 BLACK**. Per-row stats in
`benchmark-runs/20260522T075639Z-task18-texture-shader-stages-metal-v0.2-baseline/key-evidence/per-row-stats.md`.
Frames 0116-0118, 0183-0184, 0248-0250 are unique-color = 1 with
RGB max = 0 per channel (zero non-zero pixels anywhere). The XBE
clears to BLACK successfully (8 captured BLACK frames across 3
render-and-reboot cycles matching the n_frames=300 cycle), but
none of the 12 per-cell draws produce any visible output.

**Files touched (4 modified, 1 baseline created):**

- `scripts/apple-silicon/xbe-tests/texture-shader-stages/main.c`
  — v0.2 expansion (4x3 grid, per-row combiner switch,
  per-cell DIFFUSE attribute).
- `scripts/apple-silicon/xbe-tests/texture-shader-stages/expected.py`
  — 12-cell layout with row 2 R/G/B/W from DIFFUSE.
- `scripts/apple-silicon/xbe-tests/texture-shader-stages/manifest.json`
  — v0.2 title + updated `expected_fail_notes` with the bisect
  verdict.
- `scripts/apple-silicon/xbe-tests/texture-shader-stages/{bin/default.xbe,
  texture-shader-stages.iso, main.exe, main.obj}`
  — rebuilt artifacts.
- `benchmark-runs/20260522T075639Z-task18-texture-shader-stages-metal-v0.2-baseline/`
  — durable evidence: full screenshot sequence (256 PNGs) +
  `key-evidence/` directory with the dashboard frame + 3 distinct
  XBE-active BLACK frames + per-row-stats.md narrative.

**v0.1 candidate root causes — post-v0.2 status (refined per
Codex 2026-05-22):**

1. **NV_PGRAPH_SHADERPROG dirty-state propagation
   (PASS_THROUGH-specific reading):** RULED OUT by v0.2 for the
   PASS_THROUGH-specific interpretation only. Row 2 keeps
   SHADER_STAGE_PROGRAM at PROGRAM_NONE for all 4 cells (no
   change across cells), so a SHADER_STAGE_PROGRAM-mode-specific
   dirty-state bug cannot explain row 2's failure. The BROADER
   reading — "any SHADER_STAGE_PROGRAM override under
   xbed_load_textured_shaders() is not being honored" —
   remains live and needs the v0.3 control-row bisect or a
   per-draw pipeline-key dump to confirm or rule out.
2. **XBE-side combiner setup wrong:** PARTIALLY RULED OUT. Row 2
   uses a different combiner ICW_A_SOURCE (V0 vs T0) and also
   fails; the combiner config encodings mirror combiner-basic
   (which PASSes on Metal), so this is unlikely.
3. **State-machine interaction with xbed_load_textured_shaders:**
   **SURVIVING CANDIDATE.** The dashboard renders correctly and
   `combiner-basic` (uses `xbed_load_default_shaders`, no
   texturing) PASSes in the same harness session. Likely
   interaction points: (a) Cg's pre-set
   `SHADER_STAGE_PROGRAM=2D_PROJECTIVE` for stage 0 in
   `xbed_tex_ps.inl` overridden per-cell but Metal's
   texture-state cache / dirty-bit chain may not invalidate the
   right things; (b) textured VS has 3 input attributes
   (POSITION+DIFFUSE+TEXCOORD0) vs default VS's 2 — vertex
   descriptor / `uniform_attrs` recomputation may not be picking
   up TEXCOORD0 (slot 9) correctly; (c) `bind_dummy_stage0`'s
   tex bind interacts with per-cell SHADER_STAGE_PROGRAM
   override in a way that silently drops the draw on Metal.
4. **(NEW per Codex 2026-05-22) Combiner-rewrite ignored under
   textured-shader state:** SURVIVING CANDIDATE. The only
   intentional row1→row2 delta is the second
   `program_combiners_with_a_source()` call switching
   `ICW_A_SOURCE` from `T0` to `V0`. If that combiner update is
   silently dropped under textured-shader state, row 2's BLACK
   output is equally explained by row 1's residual
   `A_SOURCE=T0` config still being in effect at draw time
   (which combined with `t0=(0,0,0,1)` from NONE yields
   `R0 = T0 * 1 = 0` → BLACK). This is distinct from
   candidate (3): even if SHADER_STAGE_PROGRAM overrides ARE
   honored, the combiner override may not be.

**SEPARATE bug identified while authoring v0.2 (not task #18
itself but real and worth a future fix slice):**
`hw/xbox/nv2a/pgraph/glsl/psh.c:142-148` (shared GL+Metal code,
added by commit `046160d04d`) combined with
`hw/xbox/nv2a/pgraph/pgraph.h:330`
(`pgraph_is_texture_stage_active`) degrades PASS_THROUGH (mode
0x04) to NONE in `state->shader_stage_program`. The helper
returns false for mode 0 (NONE) and mode 4 (PASS_THROUGH), and
the gate `if (!enabled) state->shader_stage_program &= ~(0x1f
<< (i * 5));` clears the per-stage mode bits for PASS_THROUGH
even though PASS_THROUGH does not sample any texture and does
not require an active texture binding. tex_modes[0] becomes 0
in the shader generator and the emitted PSH contains `vec4 t0 =
vec4(0.0, 0.0, 0.0, 1.0);` instead of `vec4 t0 = pT0;`. This
matches v0.1's shader-dump finding (no PASSTHRU signature
observed) for row 0. **The minimal proposed fix** would exempt
NONE and PASS_THROUGH from the zeroing gate (they do not need
texture sampling infrastructure; PASS_THROUGH just routes pT0).
This is in shared GL+Metal code so the fix would apply to both
renderers. The fix is NOT landed in this cycle because: (a) the
v0.2 evidence shows row 2 also fails, so this PASS_THROUGH bug
alone is insufficient to explain task #18 — fixing it would not
make texture-shader-stages PASS on Metal; (b) commit
`046160d04d` was specifically added to fix "Metal boot and
texture stability canaries", so naïvely reverting/relaxing the
gate risks regressing Metal boot animation. The future fix
slice should bisect Metal boot stability with and without the
NONE/PASS_THROUGH exemption before landing.

**Validation evidence (durable, all under `benchmark-runs/`):**

| Run dir | Purpose | Result |
|---|---|---|
| `20260522T075639Z-task18-texture-shader-stages-metal-v0.2-baseline/` | v0.2 Metal canonical baseline (nv2a source pinned via metal_canonical_overrides) | `expected_fail` per manifest; row 2 also pure BLACK; 8 XBE-active BLACK frames across 3 cycles |

Harness sanity from same session: combiner-basic PASS (in v0.1
cycle's `/tmp/combiner-basic-sanity/`); no new harness sanity
needed since v0.2 reuses the same harness machinery.

**Codex review state (cycle 7):** COMPLETED. Verdict: **MINOR
ISSUES** (3 findings, all adopted in cycle 7).

- MEDIUM (adopted): Codex pointed out the docs overstated what
  v0.2 ruled out. Row 2 is authored correctly and shows the
  failure is not PASS_THROUGH-only, but it does not eliminate
  the broader "any SHADER_STAGE_PROGRAM override under
  xbed_load_textured_shaders() is not being honored" case.
  Softened the verdict accordingly: now says
  "PASS_THROUGH-only explanation ruled out" rather than
  "hypothesis 1 ruled out".
- MEDIUM (adopted): Codex flagged a third live interpretation
  missing from the verdict — row 2 black is also consistent
  with the row-2 COMBINER REWRITE never taking effect under
  textured-shader state. Added candidate (4) to the surviving
  list above.
- LOW (adopted): Codex noted the orchestration docs were
  initially inconsistent about review state (one said
  completed, others said pending). Synced all 6 docs to
  "COMPLETED, MINOR ISSUES" before commit.
- OUT OF SCOPE (Codex confirmed): row-2 XBE wiring is correct
  for the stated bisect; PASS_THROUGH-degraded-to-NONE
  analysis is correct; queueing the psh.c fix as a separate
  slice is reasonable.

**Next-session-actionable bisect (v0.3 proposed, not committed
— Codex finding 2 informs the design):**

1. Add a v0.3 control row that uses `xbed_load_default_shaders`
   (no texturing) but still issues per-cell SHADER_STAGE_PROGRAM
   writes. If that row renders correctly, the bug is isolated to
   the textured-shader state machine — candidate (3a)/(3b)/(3c).
   If it also fails, the bug is in the SHADER_STAGE_PROGRAM
   override path itself, independent of textured shaders.
2. **NEW (per Codex finding 2):** Instrument the row1→row2
   combiner `A_SOURCE` switch directly (not just the stage-
   program writes) — e.g., add a sentinel combiner config
   that would produce a deterministic non-black output IF the
   combiner update is honored, and capture whether the
   pipeline cache hits a new MSL after the switch.
3. Alternatively / additionally: dump the Metal pipeline cache
   key per draw via `XEMU_METAL_DIAG_ATTRIB_DUMP` (already wired
   for task #16 investigation) to see whether the XBE's
   per-cell pipelines are even being built, and if not, why the
   cache key isn't changing.
4. Separately (not task #18 itself): land a `psh.c` PASS_THROUGH
   gate fix slice that exempts mode 0 and mode 4 from the
   zeroing gate, with a Metal boot animation regression test
   before flipping.

**M15 default-on prerequisite status:** unchanged from cycle 6
— `texture-shader-stages` remains an expected_fail SPEC oracle
tracking task #18. First-wave PASS count stays **16 of 18**.

**Doc / instrumentation deltas landed this slice:**

- 4 modified files under
  `scripts/apple-silicon/xbe-tests/texture-shader-stages/` +
  rebuilt artifacts.
- `benchmark-runs/20260522T075639Z-task18-texture-shader-stages-metal-v0.2-baseline/`
  — full evidence directory.
- `docs/apple-silicon/handoff.md` — cycle 7 banner appended.
- `docs/apple-silicon/decision-log.md` — cycle 7 entry to be
  appended.
- `docs/apple-silicon/orchestration-state/*.md` — refreshed.

**Previous cycle 6 closure banner preserved below for continuity.**

---



## 2026-05-22 (Hermes cycle 6) — §4.13 `texture-shader-stages` v0.1 (last unstarted first-wave XBE; ships as expected_fail Metal pending task #18)

**Status: BOUNDED PARTIAL closed cleanly.** v0.1 lands the
infrastructure + a math-derived spec oracle for the
SHADER_STAGE_PROGRAM 5-bit-per-stage register dispatch path. The
XBE is design-verified (Codex MINOR ISSUES adopted) but FAILs on
Metal with a documented signature; the FAIL is queued as task #18
for the next session. Per workspace rule #17 (XBE-first
methodology), this is the correct shape: ship the feature-
isolating XBE as a SPEC ORACLE first; investigate the renderer-
side root cause in a separate dedicated slice.

**Slice scope (v0.1):** 4x2 grid, 8 cells, 2 of 19 modes.

- Row 0 (4 cells): SHADER_STAGE_PROGRAM stage0 = PASS_THROUGH
  (0x04); per-cell `TEXCOORD0 = (R,G,B,1)` → t0 = pT0 → R0 →
  fragColor. Expected: RED / GREEN / BLUE / WHITE.
- Row 1 (4 cells): SHADER_STAGE_PROGRAM stage0 = PROGRAM_NONE
  (0x00); identical per-cell TEXCOORD0 input → t0 = (0,0,0,1)
  regardless of input → BLACK x 4. Identical input across both
  rows isolates per-row delta to the SHADER_STAGE_PROGRAM
  dispatch path itself.
- Shared combiner override (mirrors combiner-basic v0.1
  topology): COLOR ICW stage 0 A_SOURCE=T0, B_SOURCE=ZERO
  (UNSIGNED_INVERT → B=1), C=D=0; OCW AB_DST=R0; alpha
  zeroed; final-combiner D=R0, G=DIFFUSE.a.
- Stage 0 bound to a 4x4 magenta dummy texture (so renderer
  does NOT force stage_program=NONE due to a disabled stage per
  `glsl/psh.c:142-148`); texture content is never sampled by
  either mode, but a leak would render row 0 MAGENTA.

**Files added (5 new):**

- `scripts/apple-silicon/xbe-tests/texture-shader-stages/main.c`
- `scripts/apple-silicon/xbe-tests/texture-shader-stages/expected.py`
- `scripts/apple-silicon/xbe-tests/texture-shader-stages/manifest.json`
- `scripts/apple-silicon/xbe-tests/texture-shader-stages/Makefile`
- build artifacts (`bin/default.xbe`, `texture-shader-stages.iso`,
  `main.{exe,obj,c.d}`)

**Validation evidence (durable, under `benchmark-runs/`):**

| Run dir | Purpose | Result |
|---|---|---|
| `20260522T064552Z-task18-texture-shader-stages-metal/` | initial Metal run (drawable source) | FAIL pure-BLACK |
| `20260522T065933Z-task18-ts-shader-dump/` | XEMU_METAL_DUMP_TARGET_SHADER=all | 1024 .glsl; no PASSTHRU PSH variant emitted for the front buffer |
| `20260522T070256Z-task18-texture-shader-stages-metal-v0.1-baseline/` | nv2a source pinned via metal_canonical_overrides; canonical baseline | `expected_fail` recognized by harness; `signal_match_pct=19.3` confirms no per-cell color content |

Combiner-basic sanity check on the same harness setup PASSed in
the same session (`/tmp/combiner-basic-sanity/`), so the harness
itself is sound — the FAIL is specific to this XBE's renderer
interaction.

**Codex review state (cycle 6):** COMPLETED. Verdict: **MINOR
ISSUES** (2 findings, both adopted).

- MEDIUM (adopted): cycle 6 v0.1 manifest's candidate root
  cause (1) said the Metal pipeline cache key may omit
  SHADER_STAGE_PROGRAM. Codex verified via
  `hw/xbox/nv2a/pgraph/glsl/psh.h:37-40` →
  `hw/xbox/nv2a/pgraph/glsl/shaders.h:27-31` →
  `hw/xbox/nv2a/pgraph/mtl/shaderstate.h:59-67` that the key
  DOES include `shader_stage_program`. Manifest rewritten:
  revised hypothesis (1) now points at pipeline-rebuild dirty-
  state propagation around `NV_PGRAPH_SHADERPROG` writes
  instead.
- LOW (adopted): main.c header text "No `compare_overrides`
  needed" + `expected.py` "byte-exact" claim were inconsistent
  with the manifest's `max_changed_pct=3.0 / min_signal=97.0`
  budget. Both header texts updated to clarify that the budget
  absorbs only inter-cell rasterizer edges + harness frame-
  selection slack; per-channel threshold remains harness default
  (16); cell interiors are byte-exact.
- OPEN question Codex raised: GL not in `expected_fail_renderers`
  even though the harness GL screencap path is unreliable. The
  manifest now documents the GL exclusion explicitly in
  `expected_fail_notes` (the harness will report
  `no-screenshot-captured` for the GL leg rather than a
  meaningful diff; treat any GL run as smoke until GL renderer-
  native screenshot lands).
- OUT OF SCOPE Codex noted: no obvious authoring bug in main.c
  explains the all-BLACK Metal result. The combiner / source
  encodings and PASSTHRU/NONE derivation are internally
  consistent — strengthening the case that the FAIL is renderer-
  side.

**M15 default-on prerequisite status (per `metal-renderer-plan.md`
§M15 + 2026-05-20 evening XBE-first methodology pivot):** the
first-wave XBE PASS count is **16 of 18** with this slice (was 16
of 17 before §4.13 entered the rotation; §4.13 enters as a
documented expected_fail SPEC oracle for task #18, mirroring
`swizzle-mipmap` v0.2's role for tasks #16/#17 before they
closed). All §4 priority XBEs now have at least a v0.1; only
v0.2+ expansions and the second wave remain queued.

**Net next-highest-value actions (not binding):**

1. **Investigate task #18 (§4.13 Metal silent-fail root cause).**
   Three remaining candidate root causes (Codex ruled out the
   original "cache key omits SHADER_STAGE_PROGRAM" hypothesis):
   (1) `NV_PGRAPH_SHADERPROG` may not trigger pipeline dirty-
   state / rebuild on Metal despite being in the key; (2) my
   XBE's combiner setup may cause a silent failure (e.g. wrong
   ICW_A_SOURCE encoding for T0, even though combiner-basic
   uses the same encoding); (3) state-machine interaction
   between xbed_load_textured_shaders Cg setup and per-cell
   override. Next concrete step: author the v0.2 DIFFUSE-source
   experiment cell (described in expected_fail_notes) to bisect
   between (1) renderer-side vs (2)/(3) XBE-side.
2. **Investigate task #17 (GL LOD-clamp regression)** — separate
   slice. Cycle 3 evidence: GL renders swizzle-mipmap cell 0
   correctly but cells 1..6 BLACK when MIN_LOD_CLAMP =
   MAX_LOD_CLAMP > 0. Likely lives in `gl/texture.c` per-mip
   upload when `s.levels < 7`.
3. **v0.2 expansion of `texture-shader-stages` after task #18
   closure.** Add the remaining 17 modes (PROJECT2D, PROJECT3D,
   CUBEMAP, CLIPPLANE, BUMPENVMAP*, BRDF, DOT_*, DPNDNT_*,
   DOTPRODUCT, DOT_RFLCT_SPEC_CONST) with multi-stage chaining
   infrastructure.
4. **Investigate the pipeline-smoke `surface_scale=2` leak**
   — harness / xemu.toml interaction; pre-existing, documented
   under cycle 5.

**Doc / instrumentation deltas landed this slice:**

- 5 new files under
  `scripts/apple-silicon/xbe-tests/texture-shader-stages/`.
- `docs/apple-silicon/handoff.md` — cycle 6 banner appended.
- `docs/apple-silicon/decision-log.md` — cycle 6 entry appended.
- `docs/apple-silicon/orchestration-state/*.md` — 3
  orchestration-state files refreshed to slice-complete state.
- Codex marker at
  `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run`.

**Previous cycle 5 closure banner preserved below for continuity.**

---



## 2026-05-22 (Hermes cycle 5) — Task #16 closure: Metal bordered-texture fix + xbed_texture library BORDER_SOURCE_COLOR default

(Cycle-5 closure narrative preserved verbatim below.)
**Original 2026-05-22 cycle 5 header text:** Last updated: 2026-05-22 (Hermes-supervised cycle 5 — task #16 closure
slice). **Task #16 CLOSED.** `swizzle-mipmap` v0.2 now PASSes byte-exact
on Metal (changed_pixels_pct=0.0000, signal_match_pct=100.0000) via
the two-part fix Cycle 4 identified end-to-end. XBE rotation now stood
at 16 of 17 first-wave XBEs PASS on Metal + 1 expected_fail (`logic-ops`,
NV2A feature in neither renderer) + 1 expected_fail GL-only
(`swizzle-mipmap` v0.2 still tracked task #17 GL LOD-clamp regression).
1 unstarted (`texture-shader-stages` — closed by cycle 6, this banner).

## 2026-05-22 (Hermes cycle 5) — Task #16 closure: Metal bordered-texture fix + xbed_texture library BORDER_SOURCE_COLOR default

**Status: CLOSED.** Implements the fix scope identified in Cycle 4
(2026-05-21 evening). Single bounded slice landed; build clean;
swizzle-mipmap byte-exact PASS on Metal; 0 regressions across 11 other
XBEs exercised; Codex MINOR ISSUES (one LOW finding adopted, one open
question deferred to a documented follow-up slice).

**Code change set (3 files modified; aggregate ~58 LOC):**

1. `hw/xbox/nv2a/pgraph/mtl/texture_pg.c::decode_face_levels` (~12 LOC):
   adds `border_2d = !s.cubemap && s.border && !f.linear` and doubles
   both `src_*` and `dst_*` dims when set, mirroring `gl/texture.c:451-456`
   and `vk/texture.c:111`. Cubemap+border path (`crop_cubemap_border`)
   keeps its existing crop-after-double behavior because the cube
   sampler cannot reference border texels.
2. `hw/xbox/nv2a/pgraph/mtl/texture_pg.c::pgraph_mtl_texture_bind_from_pg`
   (~22 LOC): computes `adjusted_width/height/texture_length` once for
   `border_2d_double = !s.cubemap && !f.linear && s.border`; propagates
   to (a) `texture_length` (used for dirty-range download, invalidate,
   surface-overlap download, and `bind_slot_full` byte_length), (b) the
   `pgraph_mtl_texture_bind_slot_cached_full` cache lookup (now uses
   adjusted dims to match the dims `bind_slot_full` will insert), and
   (c) the surface fast-path guard: `has_compatible_surface = !border_2d_double && ...`
   so bordered textures never alias a flat RT (surfaces don't have the
   doubled-with-border VRAM layout).
3. `scripts/apple-silicon/xbe-tests/lib/xbed_texture.c::xbed_texture_bind_stage0`
   (~14 LOC including comment): adds `fmt |= XBED_FMT_BORDER_SOURCE_BIT;`
   so the composed format word sets `BORDER_SOURCE = COLOR` (bit 3 = 1),
   matching the nxdk `samples/mesh/main.c:145` reference `0x0001122a`.
   Previously bit 3 was 0 = `BORDER_SOURCE_TEXTURE`, which caused
   `s.border = true` in xemu's `pgraph_get_texture_shape` and tickled
   the Metal-renderer bordered-texture upload gap that Bug #1 above now
   fixes. With the library fix, the four current xbed_texture users
   (swizzle-mipmap + three LU_IMAGE_ XBEs) no longer trip the
   bordered-UV transform in `psh.c::apply_border_adjustment`.

**XBE binaries rebuilt (4):** swizzle-mipmap, texture-format-sweep,
texture-filter-wrap, texture-dma-ab. All four `*.iso` + `bin/default.xbe`
+ `main.exe` + `main.obj` files refreshed via `make clean && make` in
each test dir against the nxdk toolchain (the shared `xbed_texture.c`
is included via `lib/lib.mk` so all four needed a rebuild).

**Manifest update:** `swizzle-mipmap/manifest.json` flips
`expected_fail_renderers` from `["xemu/gl", "xemu/metal"]` to
`["xemu/gl"]`. GL leg still expected_fail under task #17 (LOD-clamp
regression — `pgraph_get_texture_shape` truncates `levels` and uploads
wrong data for cells 1-6).

**Validation evidence (durable):**

| Run dir | XBE(s) | Result |
|---|---|---|
| `benchmark-runs/20260522T054316Z-task16-swizzle-mipmap-validation/` | swizzle-mipmap | **PASS byte-exact** (changed_pixels=0, mean_abs_error=0.0000, signal_match=100.0000%) |
| `benchmark-runs/20260522T054422Z-task16-xbed-texture-regress/` | texture-format-sweep, texture-filter-wrap, texture-dma-ab | 3/3 PASS (LU_IMAGE_ linear formats — `psh.c:179` `if (!f.linear && !cubemap)` skips bordered-UV transform regardless of `s.border` so the library fix is a no-op for them; verifies no regression) |
| `benchmark-runs/20260522T055631Z-task16-wider-regress/` | depth-floor, stencil-ops, native-quad-tri-depth, cmp-vertex-format, flat-quad-propagation, crtc-publish ×2 variants | 7/7 PASS (msaa-aa-factor reports `not-built`, pre-existing — not in this slice's rebuild scope) |
| `benchmark-runs/20260522T054657Z-task16-renderer-regress-smoke/` | pipeline-smoke, mirror, color-channel, combiner-basic, blend-matrix | 4/5 PASS; pipeline-smoke FAIL (see "Known pre-existing issue" below) |
| `benchmark-runs/20260522T055508Z-task16-pipeline-smoke-recheck/` | pipeline-smoke | FAIL — same deterministic mode as previous run |

Net: 14 PASS (incl. the originally-targeted swizzle-mipmap flip from
expected_fail to PASS), 1 deterministic pre-existing FAIL
(pipeline-smoke), 0 newly-introduced regressions.

**Known pre-existing issue (NOT caused by this slice):** `pipeline-smoke`
FAILs on Metal across two consecutive runs with `changed_pixels_pct=99.9997 > 0.5`.
Root cause: the captured screenshot is `1280x960` while the math-derived
oracle reference is `640x480` — `surface_scale=2` is leaking from xemu's
mutated `xemu.toml` despite the harness writing `[display.quality] surface_scale = 1`
into the fresh per-cell toml (`scripts/apple-silicon/xbe-harness/xbe_renderers.py:78-79`).
The xemu.toml dumped from the cell dir has no `[display.quality]` block
at all post-run, suggesting xemu strips it on toml-rewrite when it
matches "the default" and then applies the Apple Silicon first-launch
default = 2 on the next load. `swizzle-mipmap` survives this because
its manifest declares `metal_canonical_overrides: {"XEMU_METAL_SCREENSHOT_SOURCE": "nv2a"}`
which captures the unscaled NV2A surface; PGRAPH-rendered XBEs survive
because their content downsamples cleanly; pipeline-smoke (Tier-4,
CPU-paints a single white pixel) does not. **Out of scope for Task #16;**
queued as a separate harness/config issue.

**Codex review state (cycle 5):** COMPLETED. Verdict: MINOR ISSUES.
Strengths: confirms all three sync points are updated correctly
(decode dims, dirty-range byte length, cache lookup dims) and praises
the surface fast-path guard for not aliasing RT-sized textures.
Finding (LOW, adopted): `scripts/apple-silicon/xbe-tests/lib/vs.inl`
and `xbed_tex_vs.inl` had workstation-absolute path comments from the
rebuild; reverted those two cosmetic changes (`git checkout --` the
two files; the shader bytecode is identical). Open question (deferred):
whether to expose `BORDER_SOURCE` as an explicit field on
`XbedTextureStage0` for a future dedicated bordered-texture XBE — yes,
but tracked as a follow-up; for now hardcoding COLOR matches the nxdk
samples/mesh reference and unblocks the four current users. Codex's
"out of scope" note about pipeline-smoke attribution is addressed
above by our own re-run confirmation.

**Cycle 4 narrative resolution.** Cycle 4 identified the two-bug root
cause end-to-end:
- **Bug 1 (Metal renderer):** `decode_face_levels` did not double for
  `s.border` in the non-cubemap-2D path. → Fixed by change set #1 + #2.
- **Bug 2 (XBE library):** `xbed_texture_bind_stage0` never set
  `BORDER_SOURCE_COLOR`. → Fixed by change set #3.

Both fixes shipped together (Cycle 4's "preferred option (c)").

**M15 default-on prerequisite status (per `metal-renderer-plan.md` §M15 +
2026-05-20 evening XBE-first methodology pivot):** the first-wave XBE
PASS count goes from **15 of 17** to **16 of 17** with this slice.
`logic-ops` remains expected_fail (NV2A feature in neither renderer;
SPEC oracle). `texture-shader-stages` (§4.13) is the only unstarted
first-wave XBE. `swizzle-mipmap` PASSes on Metal but is still
expected_fail on GL (task #17, separate fix slice).

**Net next-highest-value actions (not binding):**

1. **Author §4.13 `texture-shader-stages`** — the last unstarted
   first-wave XBE. 19 NV2A texture-shader modes; needs combiner-helper
   + texture-shader-stage infra. Significant scope.
2. **Author a dedicated `swizzle-bordered` XBE** that intentionally
   sets `BORDER_SOURCE = TEXTURE` and provides 128x128 swizzled VRAM
   data so the bordered-UV transform produces correct per-quadrant
   samples. Guards the Metal renderer's just-fixed bordered-texture
   path against future regression. Per Codex's open question, also
   consider exposing `BORDER_SOURCE` as a field on `XbedTextureStage0`
   in this slice rather than hardcoding COLOR in `xbed_texture_bind_stage0`.
3. **Investigate Task #17 (GL LOD-clamp regression)** — separate
   slice. Cycle 3 evidence: GL renders cell 0 correctly but cells 1..6
   BLACK when MIN_LOD_CLAMP == MAX_LOD_CLAMP > 0. Likely lives in
   `gl/texture.c` per-mip upload when `s.levels < 7` due to the same
   `pgraph_get_texture_shape::levels = MIN(levels, max + 1)` clamp.
4. **Investigate the pipeline-smoke `surface_scale=2` leak** —
   harness / xemu.toml interaction. Pre-existing; documented above.

**Doc / instrumentation deltas landed this slice:**

- `hw/xbox/nv2a/pgraph/mtl/texture_pg.c` — renderer fix (functional;
  removes the bordered-texture upload gap end-to-end).
- `scripts/apple-silicon/xbe-tests/lib/xbed_texture.c` — library fix
  (functional; nxdk-aligned `BORDER_SOURCE_COLOR` default).
- 4 XBE binaries rebuilt (`swizzle-mipmap.iso` + 3 LU_IMAGE_ XBEs).
- `scripts/apple-silicon/xbe-tests/swizzle-mipmap/manifest.json` —
  flips `expected_fail_renderers` to GL-only with cycle 5 notes.
- `docs/apple-silicon/handoff.md` — cycle 5 banner appended.
- `docs/apple-silicon/decision-log.md` — cycle 5 entry appended.
- `docs/apple-silicon/orchestration-state/*.md` — 4 orchestration-state
  files refreshed to closure state.
- Codex marker at `/Users/jbbrack03/XEMU_MacOS/.claude/state/codex-validate-last-run`.

**Previous cycle 4 evening banner preserved below for continuity.**

---



## 2026-05-21 (evening, Hermes cycle 4) — Task #16 sampler attribution + bordered-texture diagnosis (durable, no fix landed)

**Status: still expected_fail on Metal.** This cycle closes the
cycle-3 open question ("is the sampler per-cell correct?") and traces
the intra-mip Q0 collapse symptom to its end-to-end root cause. The
bug is now fully explained: a Metal-renderer missing-feature
(`pgraph_mtl_texture_bind_from_pg` / `decode_face_levels` do not honor
`s.border` to upload textures at 2x size with a 4-texel border, the way
`gl/texture.c:451-456` does) interacting with an XBE library bug
(`scripts/apple-silicon/xbe-tests/lib/xbed_texture.c::xbed_texture_bind_stage0`
forgets to set `BORDER_SOURCE_COLOR` in the composed format word,
defaulting it to BORDER_SOURCE_TEXTURE). Cycle 3's "sampler /
fragment-shader UV-to-texel path" framing is partially superseded:
sampler is correct, fragment-shader UV transform is correct (under
the GL bordered-texture convention), but the Metal texture upload does
not honor the convention. No bounded fix landed this cycle; the renderer
fix touches `decode_face_levels` + `pgraph_mtl_texture_bind_from_pg` +
the texture cache key (multi-file, needs its own validation slice). The
XBE-library fix is a one-liner but requires a nxdk rebuild and is
deferred to the same next slice for cohesion. Tree left clean (one
env-gated diag added + one tiny new helper in mtl/draw.{h,mm}; zero
behavior change with env unset).

**Tooling delta this cycle (env-gated, zero impact when env unset).**
`hw/xbox/nv2a/pgraph/mtl/texture_pg.c::pgraph_mtl_texture_bind_from_pg`
gains one env-gated `metal_tex_bind_attrib` diag line per bind when
`XEMU_METAL_DIAG_ATTRIB_DUMP=1` AND
`pg->vertex_attributes[9].stride == 44` AND
`s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8`. Cap
32 lines; matches the existing diag-stream cap family. The line shape:

```
xemu-perf: metal_tex_bind_attrib stage=N tex_addr=0xAAAA color_target=0xCCCC \
  nv2a_fmt=0xFF mtl_fmt=M w=W h=H levels=L s_levels=S \
  shape_min_lvl=MN shape_max_lvl=MX \
  min_lod=X.XXX max_lod=Y.YYY lod_bias=B.BBB \
  min_f=N mag_f=N mip_f=N addr_u=U addr_v=V \
  has_surf=B self_sample=B linear=B tex_dirty=B \
  next_dump_idx=IDX dump_active=B
```

The `next_dump_idx` field is the upcoming `XEMU_METAL_DUMP_DRAW_RT`
PNG filename, plumbed via a new tiny helper
`pgraph_mtl_draw_dump_rt_peek_index()` in `mtl/draw.{h,mm}` (returns 0
when DUMP is inactive; atomic-load otherwise). Lets the caller match
a stride==44 bind to the eventual post-flush_draw PNG when DUMP is
active. The gate intentionally omits `s.levels == 7` because per-cell
`MAX_LOD_CLAMP` writes cause `pgraph_get_texture_shape` to clamp
`s.levels` down to `max_mipmap_level + 1` (so the same XBE-bound
texture is reported as `s.levels = 1..7` across the 7 per-cell binds).

Documented in `automation.md` "Diagnostic Toggles" and
`.claude/rules/flags-renderer.md`.

**Decisive findings this cycle (durable evidence under
`docs/apple-silicon/task-16-evidence-2026-05-21/cycle4-sampler-rt/`).**

1. **Per-cell sampler state IS correct on Metal.** The 32 captured
   stride==44 binds show exact per-cell discrimination matching the
   XBE's `set_lod_clamp(mip, mip)` writes:
   - cell 0: `levels=1, shape_min/max=0/0, min_lod/max_lod=0/0, mip_f=0`
   - cell 1: `levels=2, shape_min/max=1/1, min_lod/max_lod=1/1, mip_f=1`
   - ...
   - cell 6: `levels=7, shape_min/max=6/6, min_lod/max_lod=6/6, mip_f=1`

   Evidence: `cycle4-sampler-rt/logs/sampler-attrib-per-cell.log`.
   The Metal sampler descriptor cache produces a distinct
   `MTLSamplerState` per cell because the `PgraphMtlSamplerDesc`
   memcmp key differs. The cycle-3 LOD-clamp morning fix is plumbed
   end-to-end through `build_sampler_desc_from_pg`.

2. **The bug is NOT in the sampler.** Each cell's sampler clamps
   correctly to its target mip. The intra-mip Q0 collapse must come
   from upstream of the sampler (the texture upload / fragment-shader
   UV transform).

3. **`pgraph_get_texture_shape` clamps `s.levels` per cell.** The
   widened-gate run histogram
   (`cycle4-sampler-rt/logs/sampler-attrib-widened-gate-histogram.log`)
   shows ~9-10 binds each across `s_levels=1..7` over 64 captured
   lines. This is `texture.c:304`'s
   `levels = MIN(levels, max_mipmap_level + 1)` clamp. Per cell, a
   distinct MTLTexture is allocated (cache key includes `levels`).
   Cell 0's MTLTexture has 1 mip level = 64x64; cell 6's has 7 mips
   = 64x64...1x1. Each MTLTexture's mip data is the correct
   unswizzled per-mip swizzle-mipmap data (cycle-2 unswizzle dump
   already proved CPU-side decode is correct).

4. **The fragment shader uses the bordered-texture UV transform.**
   Cycle 3's preserved GLSL dump
   (`../cycle3-replay/glsl-dumps/xemu-metal-target-0x03aa8000.glsl`)
   emits the
   `apply_border_adjustment` block from `psh.c:794-810`:

   ```glsl
   vec3 t0LogicalSize = vec3(64.000000, 64.000000, 1.000000);
   pT0.xyz = (pT0.xyz * t0LogicalSize + vec3(4, 4, 4))
               * vec3(0.007812, 0.007812, 0.062500);
   vec4 t0 = textureProj(texSamp0, (pT0.xyw));
   ```

   `0.007812 = 1/128`, `0.062500 = 1/16`. Per xemu's convention
   (`psh.c:180-185` comment): when `BORDER_SOURCE != COLOR` the
   actual texture in memory is **2x the reported logical size with a
   4-texel border**. The GLSL transforms input UVs (e.g. 0.75) to land
   on the correct texel of the **128-wide physical** texture:
   `(0.75 * 64 + 4) / 128 = 52/128`, hitting texel 52 of the 128-wide
   bordered texture = correctly the Q1 region of the inner logical
   64x64.

5. **The GL renderer uploads the 2x bordered texture.**
   `gl/texture.c:451-456`:
   ```c
   if (!f.linear && s.border) {
       adjusted_width  = MAX(16, adjusted_width  * 2);
       adjusted_height = MAX(16, adjusted_height * 2);
       adjusted_pitch  = adjusted_width * (s.pitch / s.width);
       adjusted_depth  = MAX(16, s.depth * 2);
   }
   ```
   GL allocates a 128x128 GL texture and uploads the doubled swizzled
   VRAM contents. The GLSL `(uv*64+4)/128` transform produces correct
   sample coordinates against this 128x128 texture — cell 0 renders
   the 4-quadrant pattern correctly on GL.

6. **The Metal renderer does NOT double for borders.**
   `mtl/texture_pg.c::decode_face_levels` (lines 856-958) decodes
   `s.width × s.height` with no `s.border` adjustment;
   `pgraph_mtl_texture_bind_slot_full` (`texture.mm:907-912`)
   allocates the MTLTexture at `l0->width × l0->height`. Result for
   the swizzle-mipmap XBE: the 64x64-uploaded texture is sampled with
   the bordered-UV transform that expects 128x128, so all four sub-quad
   UVs (0.25/0.75 × 0.25/0.75) land in the left half of the 64x64
   texture's normalized [0..1] range, specifically in Q0:
   - Q0 (TL) UV=(0.25, 0.25) → texel (10, 10) → Q0 → RED ✓
   - Q1 (TR) UV=(0.75, 0.25) → texel (26, 10) → still Q0 → RED ✗
   - Q2 (BL) UV=(0.25, 0.75) → texel (10, 26) → still Q0 → RED ✗
   - Q3 (BR) UV=(0.75, 0.75) → texel (26, 26) → still Q0 → RED ✗

   This is the intra-mip "Q0 collapse" symptom exactly as observed in
   the cycle 3 + cycle 4 screenshots. Evidence:
   `cycle4-sampler-rt/screenshots/cycle4-best-frame-0257-q0-collapse.png`.

7. **The XBE's texture format word never sets `BORDER_SOURCE_COLOR`.**
   `xbed_texture.c::xbed_texture_bind_stage0` (lines 87-93) composes
   the format from `fmt = 0` and OR-ins `XBED_FMT_CONTEXT_DMA_MASK`,
   `XBED_FMT_DIMENSIONALITY`, color/mipmap/base-size — but never the
   `XBED_FMT_BORDER_SOURCE_BIT` it defines at line 27. So bit 3 is
   left at 0 = `NV_PGRAPH_TEXFMT0_BORDER_SOURCE_TEXTURE` (NOT COLOR).
   The nxdk `samples/mesh` reference at
   `/Users/jbbrack03/XEMU_MacOS/nxdk/samples/mesh/main.c:145` pushes
   `0x0001122a` whose bit 3 IS set:
   `0x2a = 0b00101010` → bit 3 = 1 = `BORDER_SOURCE_COLOR`. xbed
   library was authored to mirror that sample but missed this bit.

**Root cause: two interacting bugs.**

- **Bug 1 (Metal renderer, real-game impact):**
  `pgraph_mtl_texture_bind_from_pg` + `decode_face_levels` do not
  honor `s.border`. When `s.border == true` (BORDER_SOURCE != COLOR)
  the renderer should double the texture upload dimensions and apply
  the 4-texel border, mirroring `gl/texture.c:451-456`. Currently it
  uploads only the reported size, so the GLSL bordered-UV transform
  produces wrong sample coordinates. Real-Xbox games using bordered
  textures will misrender on Metal.
- **Bug 2 (XBE library, accidental tickle):**
  `xbed_texture_bind_stage0` forgets to set `BORDER_SOURCE_COLOR`,
  so all four library users bind their textures as "TEXTURE has its
  own border" — accidentally triggering bug #1 in the Metal renderer.
  Only the SZ_*-format users hit it; LU_IMAGE_-format users
  (`texture-format-sweep`, `texture-dma-ab`, `texture-filter-wrap`)
  are unaffected because `psh.c:179`'s `if (!f.linear && !cubemap)`
  guard skips the bordered UV transform for linear textures.

**Cycle 3 narrative resolution.** Cycle 3 narrowed the surface to
"Metal texture sampler / fragment-shader UV-to-texel path". Cycle 4
proves the sampler is correct (#1, #2 above) and the
**fragment-shader UV transform is also correct** under the GL
convention. The bug is in the **texture upload** path
(`decode_face_levels` / `bind_slot_full` not honoring `s.border`).
Cycle 3's instinct was off by one layer; cycle 4's per-bind sampler
attribution diag closed that gap by proving the sampler is per-cell
correct, forcing the search back upstream to the texture upload.

**Code locations validated correct this cycle (cumulative; do NOT
re-investigate unless code changes):**

- `hw/xbox/nv2a/pgraph/mtl/texture_pg.c::build_sampler_desc_from_pg`
  — per-cell sampler descriptor (incl. LOD clamps + filter mode) is
  derived correctly from pg state.
- `hw/xbox/nv2a/pgraph/mtl/texture.mm::get_sampler` /
  `sampler_desc_equal` — sampler-cache memcmp produces distinct
  MTLSamplerStates per cell.
- `hw/xbox/nv2a/pgraph/glsl/psh.c::apply_border_adjustment` — the
  `(uv*size+4)/(size*2)` bordered UV transform is correct under the
  documented convention (GL renderer's bordered upload satisfies it).
- All cycle 2 + cycle 3 code locations remain validated correct.

**Implicated files / lines for the next slice (FIX surface):**

- `hw/xbox/nv2a/pgraph/mtl/texture_pg.c::decode_face_levels`
  (lines 718-959) — needs `s.border` adjust:
  `if (!f.linear && s.border) { width *= 2; height *= 2; pitch *= 2; ... }`
  for the swizzled / compressed paths (mirror
  `gl/texture.c:451-456`).
- `hw/xbox/nv2a/pgraph/mtl/texture_pg.c::pgraph_mtl_texture_bind_from_pg`
  (lines 1025-1320) — the cache lookup + `bind_slot_full` call must
  pass the doubled dimensions when `s.border`. Cache key currently
  uses (vram_addr, length, w, h, fmt, cube, levels); if doubled w/h
  is passed it'll naturally evict the old non-bordered entries.
- `scripts/apple-silicon/xbe-tests/lib/xbed_texture.c::xbed_texture_bind_stage0`
  (lines 87-93) — add `fmt |= XBED_FMT_BORDER_SOURCE_BIT;`. Rebuild
  the four library users (`swizzle-mipmap`, `texture-format-sweep`,
  `texture-dma-ab`, `texture-filter-wrap`) via nxdk. The three
  LU_IMAGE_ users won't change behavior (they're linear); only
  `swizzle-mipmap` will flip its bordered-path behavior.

**Net next-highest-value actions when work resumes (suggested
ordering; not binding):**

1. **Decide fix scope.** Either (a) ship the Metal renderer
   `s.border` 2x-upload fix and keep the XBE library tickling it as
   a regression gate; or (b) ship the XBE-library fix alone (one
   line, makes swizzle-mipmap pass on Metal but doesn't catch the
   real-game Metal bug); or (c) ship both (preferred; principled).
2. **Author a dedicated diag XBE for bordered textures** if option
   (c) — `swizzle-bordered` or similar — that intentionally sets
   `BORDER_SOURCE = TEXTURE` and provides 128x128 swizzled data so the
   bordered-UV transform produces correct per-quadrant samples. This
   guards against the Metal renderer's bordered-texture path
   regressing in the future once the library helper is fixed.
3. **Investigate Task #17 separately.** Cycle 3 evidence says GL
   renders cell 0 correctly but cells 1..6 BLACK when MIN_LOD_CLAMP ==
   MAX_LOD_CLAMP > 0. That is unrelated to the bordered-texture bug
   (GL handles borders correctly). The GL bug likely lives in
   `gl/texture.c` per-mip upload when `s.levels < 7` due to the same
   `pgraph_get_texture_shape::levels = MIN(levels, max + 1)` clamp.
4. **Audit other `psh.c` UV-emission sites** for consistency with the
   bordered-texture convention (textureProj variants, convolution
   filter, etc.) so the Metal renderer fix satisfies them all.

**Doc / instrumentation deltas landed this slice:**

- `hw/xbox/nv2a/pgraph/mtl/texture_pg.c::pgraph_mtl_texture_bind_from_pg`:
  added env-gated `metal_tex_bind_attrib` diag line (~50 LOC,
  32-line cap, zero impact when env unset).
- `hw/xbox/nv2a/pgraph/mtl/draw.{h,mm}`: added
  `pgraph_mtl_draw_dump_rt_peek_index()` helper (~8 LOC; returns the
  upcoming `XEMU_METAL_DUMP_DRAW_RT` PNG filename index when DUMP is
  active, 0 otherwise).
- `automation.md` "Diagnostic Toggles": added stream-5 description
  to `XEMU_METAL_DIAG_ATTRIB_DUMP`.
- `.claude/rules/flags-renderer.md`: updated
  `XEMU_METAL_DIAG_ATTRIB_DUMP` summary to mention the new line.
- `docs/apple-silicon/task-16-evidence-2026-05-21/cycle4-sampler-rt/`:
  new evidence directory with README + 4 log files + 1 screenshot.
  Total ~600KB durable evidence; raw harness dirs (~3.5GB of
  PNG/log churn) cleaned up.

**Previous cycle 3 evening banner preserved below for continuity.**

---


## 2026-05-21 (evening, Hermes cycle 3) — Task #16 render-target attribution (durable, no fix landed)

**Status: still expected_fail on Metal.** This cycle resolves the
top open ambiguity from cycle 2 ("which VRAM target do the XBE's
stride==44 draws actually render to?") and replays the
`0x032a4000` front-buffer GLSL dump cycle 2 lost. Both questions are
now answered decisively; the open Task #16 surface area moves
DOWNSTREAM of the vertex / pipeline-key / render-target paths
(those are now all proven correct).

**Tooling delta this cycle (env-gated, zero impact when env unset).**
`hw/xbox/nv2a/pgraph/mtl/renderer.c::mtl_dispatch_decoded_draw`
gains one new diag line per dispatch when
`XEMU_METAL_DIAG_ATTRIB_DUMP=1` AND `pg->vertex_attributes[9].stride
== 44` (same gate the cycle-2 `metal_set_attr_masks` diag uses; cap
32 lines so the two diag streams interleave 1:1 across the same
budget). Line shape:

```
xemu-perf: metal_dispatch_draw_target color_addr=0x.. depth_addr=0x.. \
  uniform_attrs=0xfdf6 vcount=N icount=M prim=P color_fmt=0xF depth_fmt=0xF \
  v0=1 v3=1 v9=1 native_tri=1 native_quad=0
```

Documented in `automation.md` "Diagnostic Toggles" and
`.claude/rules/flags-renderer.md`.

**Decisive findings this cycle (durable evidence under
`docs/apple-silicon/task-16-evidence-2026-05-21/cycle3-replay/`; the
parent cycle-2 directory still holds cycle 2's evidence).**

1. **The XBE's stride==44 `xbed_draw_arrays(TRIANGLES, cell*24, 24)`
   draws all render to back-buffer-class color targets `0x03aa8000`,
   `0x03bd4000`, `0x03d00000` cycling round-robin per frame** (one
   back buffer per frame, 7 cells per back buffer). Depth target is
   uniformly `0x0397c000`. Color format `0x50`, depth format `0x104`.
   prim=5 (TRIANGLES), vcount=24, native_tri=1. Evidence: 32
   consecutive `metal_dispatch_draw_target color_addr=0x03{aa8|bd4|
   d00}000 ... uniform_attrs=0xfdf6 v0=1 v3=1 v9=1` lines in
   `cycle3-replay/logs/dispatch-draw-target-stride44.log`, each
   interleaved 1:1 with the cycle-2 `metal_set_attr_masks
   uniform_attrs=0xfdf6 [9]c=4,s=44` line in `set-attr-masks-
   stride44.log`. Same dispatch, two diag streams, identical cap.

2. **The XBE NEVER renders to `0x032a4000`.** Zero of the 32
   stride==44 dispatches hit it. Per-interval
   `metal_draw_target vram_addr=0x32a4000` shows ~30-35 flush_draws/
   sec — flat dashboard-class cadence, not the XBE's 7-cells-per-frame
   burst rhythm. The XBE's 3 back buffers each receive 700 flush_draws/
   interval once the XBE is running. Evidence:
   `cycle3-replay/logs/draw-target-aggregates.log`.

3. **The replayed `0x032a4000` GLSL dump is a uniform-only draw.**
   Captured this cycle via `XEMU_METAL_DUMP_TARGET_SHADER=0x032a4000`.
   Every slot 0..15 reads from `inlineValue[N]`; NO `layout(location
   = N) in vec4 vN` for any slot. I.e. `uniform_attrs == 0xFFFF`,
   regs `0x...3f430700` (vs the back-buffer pipelines' `0x...3e830700`
   from cycle 2). This is a non-XBE blit/publish pipeline. The cycle-2
   narrative's `0x032a4000` pipeline with `v3 = inlineValue[2]` while
   `v9` streaming (uniform_attrs=0xFDFE) was a different non-XBE draw
   captured at a different moment (the `0xADDR`-mode "latest wins"
   filter overwrites per-target). Either way, the XBE does not own
   `0x032a4000`. Evidence:
   `cycle3-replay/glsl-dumps/xemu-metal-target-0x032a4000.glsl`.

4. **Front-buffer publish path under
   `XEMU_METAL_FRONT_FB_FALLBACK=1`** (the xbe-harness canonical
   Metal recipe via `METAL_CANONICAL_RECIPE` in `xbe_orchestrator.py:
   55`) **overwhelmingly selects dashboard surfaces, NOT the XBE's
   back buffer**. 427 publishes observed (`XEMU_METAL_DIAG_PUBLISH=1`):
   376 → `0x3628000`, 26 → `0x2c06000`, 18 → `0x2e06000`, 3 →
   `0x03aa8000` (an XBE back buffer), 3 → `0x2994000`, 1 →
   `0x2454000`. Reason breakdown: 426 `fallback-dominant-draw` + 1
   `fallback-current-binding` (the first publish in the run, to
   `0x3628000`). The dashboard wins on dominance because it
   accumulates ~10000 draws/interval to a single buffer while the
   XBE splits its ~2100 draws/interval across 3 buffers; the
   `0x2454000` / `0x2994000` outliers (4/427 ≈ 0.9%) are transient
   non-XBE pipelines. Evidence: `cycle3-replay/logs/front-fb-
   publish.log`.

5. **The captured "best frame" screenshot IS the XBE's actual
   output.** Frame 0124 (in cycle 3B run) shows a 4-column × 2-row
   grid with per-mip RED tint ramp (cell 0 → cell 6 dark; cell 7
   black) and each cell rendered as a SINGLE color (intra-mip Q0
   collapse). This is exactly the manifest's documented
   `expected_fail_notes` symptom. The cycle-2 banner's "smooth
   corner-tinted gradient covering the full surface" description for
   `symptom-corner-gradient-f0138.png` **does not match the XBE's
   render** — it captured a different surface, likely the BIOS
   animation routed through the M5.13 VGA-direct deferred path
   (cycle 3's frame 0138 captures green-on-black dashboard noise —
   same M5.13 noise class). Evidence:
   `cycle3-replay/screenshots/cycle3-best-frame-0124-xbe-per-mip-tint-
   ramp.png` versus `cycle3-frame-0138-dashboard-noise.png`.

6. **CPU-side unswizzled texture buffer is correct for every mip.**
   `metal_unswizzle_dump w=64 Q0=(B00 G00 Rff Aff) Q1=(B00 Gff R00
   Aff) Q2=(Bff G00 R00 Aff) Q3=(B00 Gff Rff Aff)` and the same
   per-quadrant 4-color pattern with per-mip-tinted intensity down to
   `w=2 h=2`. Combined with cycle 2's per-vertex slot-9 stream
   correctness, this localizes the bug to the **Metal texture sampler /
   fragment-shader UV-to-texel path**, NOT the vertex pipeline,
   pipeline-key, render-target selection, or unswizzle decode.
   Evidence: `cycle3-replay/logs/unswizzle-dump-quadrants.log`.

**Cycle 2 narrative resolution.** Cycle 2 left two competing
hypotheses open: (a) XBE renders to back buffer; the screenshot
samples a stale/wrong front-buffer pipeline; (b) XBE renders to front
buffer through a path the diagnostics didn't catch. This cycle
**confirms (a) and rules out (b)**. The screenshot is sampling the
publish path's surface — usually dashboard, occasionally the XBE's
back buffer — and the XBE's true Task #16 symptom (intra-mip Q0
collapse + per-mip ramp working) is visible whenever the
`fallback-dominant-draw` path happens to publish a back-buffer
surface. The harness's best-frame selector lands those frames.

**Code locations validated correct (cycle 2 + cycle 3 combined; do
NOT re-investigate these unless code changes):**

- `hw/xbox/nv2a/pgraph/mtl/vertex.c::pgraph_mtl_collect_all_vertex_streams`
  — slot 9 stride=44 stream collected correctly per cell.
- `hw/xbox/nv2a/pgraph/mtl/vertex.c::pgraph_mtl_set_attr_masks` —
  recomputes `uniform_attrs=0xFDF6` correctly.
- `hw/xbox/nv2a/pgraph/mtl/state.c::pgraph_mtl_build_pipeline_key` —
  uses the correct uniform_attrs in the cache key.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c::pgraph_mtl_flush_draw_inner`
  draw_arrays branch — dispatches to the correct color/depth VRAM
  target per subrange.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c::mtl_dispatch_decoded_draw` —
  receives correct `draw_target_vram_addr` and passes it into
  `mtl_dump_target_shader_once` correctly.
- GLSL `vertex_main0_in` for back-buffer-class targets — emits
  `layout(location = 0) in vec4 v0;`, `layout(location = 3) in vec4
  v3;`, `layout(location = 9) in vec4 v9;` correctly (per cycle 2's
  staged dumps; cycle 3 replay produces identical files).
- CPU-side unswizzle of SZ_A8R8G8B8 mip chain — produces 4-quadrant
  distinct colors per mip (cycle 2 + cycle 3 unswizzle log).

**Implicated areas for the NEXT slice (downstream of everything
above):**

- `hw/xbox/nv2a/pgraph/mtl/texture_pg.c` and `mtl/texture.mm` —
  sampler descriptor build, mip-chain texture upload, sampler
  state binding. The cycle-2-morning LOD-clamp fix
  (`build_sampler_desc_from_pg` honoring MIN_LOD_CLAMP /
  MAX_LOD_CLAMP / MIPMAP_LOD_BIAS) closed the per-cell mip discrimination
  half of the bug; the intra-mip Q0 collapse is the remaining half.
- Fragment-shader UV path: GLSL emits `pT0.xyz = (pT0.xyz *
  t0LogicalSize + vec3(4,4,4)) * vec3(0.007812, ...)` then
  `textureProj(texSamp0, pT0.xyw)`. Verify per-vertex `pT0.xy`
  is preserved through rasterization.

**Net next-highest-value action when work resumes (suggested
ordering; not binding):**

1. Add a per-cell sampler-state attribution diag in
   `pgraph_mtl_texture_bind_from_pg` (or
   `build_sampler_desc_from_pg`) gated on the same stride==44
   heuristic, logging resolved `min_lod_clamp` / `max_lod_clamp` /
   `mip_filter` / `mag_filter` / `mip_levels` /
   `texture_base_vram_addr` per draw. Cross-reference with the
   cycle-3 dispatch-draw-target log.
2. Use `XEMU_METAL_DUMP_DRAW_RT` to dump the post-draw color RT
   per-cell. If the RT contains the 4-quadrant pattern but the
   published front-fb does not, the bug is in publish/compose. If
   the RT itself is uniform per cell, the bug is in the sampler.
3. Consider an XBE-specific `metal_canonical_overrides` setting
   `XEMU_METAL_FRONT_FB_FALLBACK=0` so the swizzle-mipmap publish
   goes through CRTC resolution instead of the dominant-draw
   fallback, removing a 99.3%-of-publishes confound from the
   capture path.

**Doc / instrumentation deltas landed this slice:**

- `hw/xbox/nv2a/pgraph/mtl/renderer.c::mtl_dispatch_decoded_draw`:
  added env-gated `metal_dispatch_draw_target` diag line (32-line
  cap, gated on `XEMU_METAL_DIAG_ATTRIB_DUMP` + slot-9 stride==44;
  same gate as cycle-2 `metal_set_attr_masks` so the two streams
  interleave 1:1).
- `automation.md` "Diagnostic Toggles": updated `XEMU_METAL_DIAG_
  ATTRIB_DUMP` entry to describe the fourth stream.
- `.claude/rules/flags-renderer.md`: updated `XEMU_METAL_DIAG_
  ATTRIB_DUMP` summary to mention the dispatch-draw-target line.
- `docs/apple-silicon/task-16-evidence-2026-05-21/cycle3-replay/`:
  new evidence directory with replay README, 4 GLSL dumps (3
  back-buffer + 1 front-buffer), 6 log files (per-dispatch
  attribution, set_attr_masks, attrib_stream, publish stream,
  draw-target aggregates, unswizzle quadrants), and 2 representative
  screenshot frames (XBE actual output + dashboard noise contrast).

**Previous cycle 2 evening banner preserved below for continuity.**

---

## 2026-05-21 (evening, Hermes cycle 2) — Task #16 deeper diagnosis (durable, no fix landed)

**Status: still expected_fail on Metal.** The handoff's earlier task
#16 narrative ("many compiled pipelines declare ONLY `float4 v0
[[attribute(0)]]`, no `v9 [[attribute(9)]]`, slot 9 read from
`inlineValue[8]`") is **partially superseded by this evening's
diagnosis**: with the current source tree (after the morning's
LOD-clamp fix + commit `498bdfd57e` adding
`XEMU_METAL_DIAG_ATTRIB_DUMP`), the bug is NOT a wholesale "slot 9
collapsed to a uniform" — slot 9 IS routed through the vertex
descriptor in every dumped pipeline that matched the XBE's
draw-target. The newly-isolated symptom is finer-grained and lives
**downstream of `pgraph_mtl_set_attr_masks` / `pipeline_key_build`**.
A bounded fix did NOT fit this slice; tree left clean (diag-only code
changes documented below). Next-highest-value action is to confirm
which render target the XBE actually draws to and whether the
captured front-buffer screenshot path samples a stale `0x032a4000`
binding produced by a non-XBE pipeline.

**What was proven this slice (decisive evidence; not speculation).
Durable evidence copied into
`docs/apple-silicon/task-16-evidence-2026-05-21/` so the supersession
narrative is replayable across hosts; the original `/tmp/task16-*`
captures are ephemeral.**

1. **The CPU-side per-vertex slot-9 stream is correct at collect
   time.** `XEMU_METAL_DIAG_ATTRIB_DUMP=1` fires the
   `metal_attrib_stream slot=9 ... count=4 stride=44 src=0` log line
   16× per run for the swizzle-mipmap XBE's draws (7 cells × ~2
   frames). Stride=44 matches `sizeof(TexVertex) = pos[3]+tex[4]+
   col[4]` floats packed. Evidence:
   `docs/apple-silicon/task-16-evidence-2026-05-21/logs/
   collect-stream-and-vsh-diag.log`,
   `benchmark-runs/xbe-harness-20260521-110722/swizzle-mipmap/metal/
   xemu.log` (earlier preserved capture).

2. **`pgraph_mtl_set_attr_masks` computes the CORRECT
   `uniform_attrs=0xFDF6` for the XBE's full-bind state**
   (bits 0/3/9 clear → POSITION/DIFFUSE/TEX0 streaming;
   bits 1/2/4-8/10-15 set → uniform). The new
   `metal_set_attr_masks uniform_attrs=0xfdf6 [0]c=3,s=44 [1]c=0,s=0
   [2]c=0,s=0 [3]c=4,s=44 [4]c=0,s=0 [9]c=4,s=44` diag line confirms
   each XBE draw observes the expected per-slot count/stride state
   inside the same dispatch as the collect-stream diag. Evidence:
   `docs/apple-silicon/task-16-evidence-2026-05-21/logs/
   set-attr-masks-stride44.log`.

3. **Pipelines whose `attrs[3]` AND `attrs[9]` are both populated DO
   exist** — `XEMU_METAL_DUMP_TARGET_SHADER=stride44` (a heuristic
   noise filter — NOT proof of XBE provenance; see automation.md
   caveats) captures 3 such pipelines targeting back-buffer-class
   VRAM addresses (`0x03aa8000`, `0x03bd4000`, `0x03d00000`). Their
   GLSL correctly emits `layout(location = 0) in vec4 v0;`,
   `layout(location = 3) in vec4 v3;`, and
   `layout(location = 9) in vec4 v9;`. These are strong candidates
   for the XBE's own pipelines (matching the bit-3/bit-9 invariant
   of `uniform_attrs=0xFDF6`) but the filter alone does not prove
   draw provenance. Evidence:
   `docs/apple-silicon/task-16-evidence-2026-05-21/glsl-dumps/
   xemu-metal-target-0x03aa8000.glsl` (plus two siblings).

4. **At least one pipeline compiled for the front buffer
   (`0x032a4000`) has slot 3 marked uniform while slot 9 is still
   streaming** (`uniform_attrs = 0xFDFE`, GLSL emits
   `vec4 v3 = inlineValue[2]` while keeping
   `layout(location = 9) in vec4 v9`). This corresponds to a state
   where bit 3 of `uniform_attrs` is set but bit 9 is clear — which
   the XBE's `bind_attribs()` ordering (clear → bind 0 → bind 9 →
   bind 3) cannot legitimately produce mid-bind, since slot 3 is
   bound LAST. Two plausible mechanisms remain to investigate when
   work resumes: (a) a non-XBE draw (BIOS / dashboard / pbkit
   publish path) hits the same VRAM target with this attribute
   layout; or (b) the XBE's draws actually go to a different target
   and front-buffer composition uses a different (stale) pipeline.
   Evidence (note: the `0x032a4000` dump was captured during a
   `stride44`-filtered run earlier in this slice but was overwritten
   by the final 3-dump run; the original 6-dump set was not staged
   into the repo. A replay capture is the first step when resuming).

5. **The visual symptom in the captured screenshots is a smooth
   2-D corner-tinted gradient covering the entire 640×480 surface,
   NOT the expected 8-cell × 4-quadrant mosaic.** Reference (math-
   derived oracle) shows 8 cells with sharp per-mip tint and per-
   quadrant RGBY pattern. Captured output shows top-right red,
   bottom-left green, bottom-right yellow, top-left black — i.e. one
   large quad spanning the full surface with linearly-interpolated
   UV-sampled colors. This is **inconsistent with the previously
   stated "Q0 collapse"** symptom and indicates the actual rendered
   geometry on the displayed surface is one big screen-aligned quad,
   not 7×6 small per-cell triangles. Evidence:
   `docs/apple-silicon/task-16-evidence-2026-05-21/screenshots/
   symptom-corner-gradient-f0138.png` versus
   `docs/apple-silicon/task-16-evidence-2026-05-21/reference/
   math-derived-expected.png`.

**Implicated files / lines (read these next):**

- `hw/xbox/nv2a/pgraph/mtl/state.c:189-280`
  (`pgraph_mtl_build_pipeline_key`) — reads `pg->uniform_attrs` via
  `pgraph_glsl_get_shader_state(pg)`. Confirmed reads correct value
  immediately after `set_attr_masks`.
- `hw/xbox/nv2a/pgraph/mtl/vertex.c:287-359`
  (`pgraph_mtl_collect_all_vertex_streams`) — collects per-vertex
  streams. Diag confirms stride=44, count=4 for slot 9 at the XBE's
  draws.
- `hw/xbox/nv2a/pgraph/mtl/vertex.c:435-478`
  (`pgraph_mtl_set_attr_masks`) — diag confirms `uniform_attrs =
  0xFDF6` for the XBE's draws.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c:1843-1985`
  (`pgraph_mtl_flush_draw_inner`) — branches over inline_elements /
  draw_arrays / inline_array / inline_buffer; the swizzle-mipmap
  XBE goes through `draw_arrays` (line 1936) at `min_element=0..144`
  with `count=24` per cell (matches 7 cells × 4 quads × 6 verts).
  **Open question**: does the draw_arrays branch in the Metal
  renderer correctly honor `start_index` per-subrange? The CPU
  collect routine passes `start, count` to
  `collect_all_vertex_streams` but the bound buffer-offset for slot
  N is unconditionally `attr_offs[i]` (which `state.c::
  pipeline_key_build` hardcodes to 0) — so all 7 cells may be
  reading the SAME first-24 vertices, with the position stream
  re-mapped per call. This is consistent with the observed
  "one-big-gradient-quad" output if the position stream is being
  reused across cells.
- `hw/xbox/nv2a/pgraph/mtl/draw.mm:1107-1114`
  (`pgraph_mtl_draw_translated`) — binds each non-NULL stream at
  `MTL_ATTR_BUFFER_INDEX_BASE + i` with `attr_offs[i]`. If
  `attr_offs` is always 0 and the streams[] buffer holds only the
  current subrange's decoded verts, that's actually correct (the
  decoder already offsets by `start`). But verify against the cell-
  to-cell sequence to be sure.

**What was ruled out:**

- "slot 9 dropped wholesale from the vertex descriptor" — false in
  current tree; all observed pipelines for the XBE-bind-state have
  `layout(location = 9) in vec4 v9` in the compiled GLSL.
- "`set_attr_masks` computes wrong `uniform_attrs`" — false; diag
  confirms `0xFDF6` for stride=44 draws.
- "shader cache aliasing across pipeline keys" — false; cache
  compare is full-struct memcmp; different `uniform_attrs` values
  produce different keys and were observed to produce distinct
  cached entries.
- "`pgraph_mtl_texture_bind_from_pg` modifies `pg->uniform_attrs`
  between `set_attr_masks` and `pipeline_key_build`" — false;
  inspected source, only `pg->texture_dirty[stage]` is touched.

**Net next-highest-value action when work resumes:**

(a) Confirm which VRAM target the XBE's `xbed_draw_arrays(TRIANGLES,
    cell*24, 24)` actually renders to (instrument
    `mtl_dispatch_decoded_draw` to log `draw_target_vram_addr` for
    stride==44 draws). If it's a back buffer with a correct full-
    bind pipeline (`0x03aa8000` class), the front-buffer
    `0x032a4000` content shown in the screenshot is a copy/blit
    that's using a stale/wrong pipeline. (b) Examine the per-cell
    position stream values — the corner-gradient visual implies
    that 6 of 7 cells contribute zero pixels and 1 cell (or one
    aggregated quad) covers the whole screen. (c) Re-test under
    `XEMU_METAL_FRONT_FB_FALLBACK=0` to remove the post-flip
    publish leg as a confound.

**Doc / instrumentation deltas landed this slice (kept as opt-in
diagnostics, no behavior change with env unset):**

- `hw/xbox/nv2a/pgraph/mtl/vertex.c::pgraph_mtl_set_attr_masks`:
  added env-gated diag dump (`XEMU_METAL_DIAG_ATTRIB_DUMP`, slot 9
  stride==44 filter, 32-line cap) of the recomputed
  `uniform_attrs` plus per-slot count/stride for slots 0-4 and 9.
- `hw/xbox/nv2a/pgraph/mtl/renderer.c::mtl_dump_target_shader_once`:
  added `stride44` mode (filter: `attrs[3].format != 0 AND
  attrs[9].format != 0`, 1024-dump cap) so a run can dump only
  XBE-full-bind pipelines without `all`'s noise drowning the
  diagnostic.
- `automation.md` "Diagnostic Toggles": updated `XEMU_METAL_DIAG_
  ATTRIB_DUMP` entry to describe the third stream, and added
  `XEMU_METAL_DUMP_TARGET_SHADER` entry covering `all` / `stride44`
  / `0xADDR` modes.
- `.claude/rules/flags-renderer.md`: updated both flag entries to
  match.

**Previous mid-day banner preserved below for diagnostic continuity.**

---

## 2026-05-21 (mid-day, Hermes-supervised cycle 1 closure)

**15 of 17 first-wave XBEs PASS on Metal + 2 expected_fail
(logic-ops + swizzle-mipmap). 1 unstarted (§4.13). §4.15
`msaa-aa-factor` v0.1 SHIPPED this slice (Hermes-supervised
single-cycle); Codex MAJOR findings adopted as narrowed-v0.1 +
v0.2 deferral; both Metal cells PASS with strengthened counter gate
(METAL_MSAA_RESOLVE_COUNT >= 100 AND METAL_MSAA_SAMPLE_COUNT >= 12,
proving sample count >= 2 across intervals, not just resolve plumbing).
Carries forward the morning's three XBEs (combiner-basic,
swizzle-mipmap v0.2, texture-dma-ab v0.1) + Metal LOD-clamp + LOD-bias
renderer fix + xbe-harness `metal_canonical_overrides` field.**

This session (2026-05-21 mid-day, Hermes-supervised cycle 1):

- **§4.15 `msaa-aa-factor` v0.1 SHIPPED PASS on Metal** as a
  MSAA path-activation + edge-AA-band PRESENT smoke test. Single
  high-contrast WHITE triangle (60,60)-(60,420)-(580,240) on BLACK;
  diagonals slope 180/520 ≈ 0.346 px/px so every column places the
  edge at a distinct sub-pixel position. XBE is MSAA-agnostic;
  `XEMU_METAL_MSAA` governs hard-step vs per-coverage gradient. Two
  Metal cells per matrix run: canonical (`XEMU_METAL_MSAA=2` via
  `metal_canonical_overrides`) + `msaa4` variant
  (`additional_metal_recipes`). Hard-step math oracle with
  `compare_overrides.max_changed_pct=3.0` absorbs the ~0.7-0.9% AA
  band. Counter gate: `METAL_MSAA_RESOLVE_COUNT >= 100` AND
  `METAL_MSAA_SAMPLE_COUNT >= 12` (sum across intervals → proves
  sample count >= 2 across the run). Validation
  (`benchmark-runs/msaa-aa-factor-20260521-v2/`): canonical PASS
  changed_pct=0.7855%, SAMPLE_COUNT=24; msaa4 PASS changed_pct=
  0.8626%, SAMPLE_COUNT=48 — monotonic widening with more samples
  is the expected signature. Codex review returned MAJOR ISSUES;
  all 4 findings adopted in-session (README "three"→"two", main.c
  triangle-area comment 117k→93,600, v0.1 title/purpose/README
  narrowed to "MSAA path-activation + edge-AA-band SMOKE" with v0.2
  follow-up explicitly queued, required_counters_min strengthened
  from RESOLVE>=1 to RESOLVE>=100 AND SAMPLE_COUNT>=12). v0.2
  deferred: per-mode keyed expected_results + AA-band lower-bound
  rejection (second-wave follow-up).

- **`docs/apple-silicon/orchestration-workflow.md` SHIPPED** as the
  canonical workflow for supervising Claude Code on this repo without
  context-window collapse. Defines Hermes (orchestrator) / Claude
  (worker) / Codex (validator) / real-Xbox-oracle (hardware witness)
  roles, the artifact-over-transcript rule, required orchestration-state
  files, supervised-cycle vs unattended-cycle loops, validation gates,
  permission-bypass policy, anti-drift rules, and Telegram escalation
  triggers. Linked from `README.md` Documentation Map and added to the
  workspace `CLAUDE.md` reference list. Formalizes the Hermes
  supervision model already in use this 2026-05-21 cycle. No code
  change; pairs with decision-log "2026-05-21 (mid-day, late):
  formalize Hermes/Claude/Codex orchestration workflow."

**Previous morning session (2026-05-21) closures preserved below
for diagnostic continuity.**

Morning of 2026-05-21:

- **§4.12 `combiner-basic` v0.1 SHIPPED PASS on Metal** byte-exact.
  4x4 grid of (input mapping × output scale modifier) at fixed
  DIFFUSE=(0.25, 0.5, 0.75, 1.0). Uses single-stage combiner with
  SUM=R0 → final D=R0 → fragColor. Math-derived oracle agrees with
  the XBE-side combiner config; mid-tones (64/128/191) byte-exact in
  raw NV2A surface. Required a new harness feature
  `metal_canonical_overrides` so the manifest can pin
  `XEMU_METAL_SCREENSHOT_SOURCE=nv2a` (linear capture) rather than
  the drawable (BGRA8Unorm_sRGB, gamma-encoded). Codex review
  returned MINOR ISSUES; both findings adopted in-session (narrowed
  SUM-vs-MUX claim; manifest no longer overclaims [-1,1] clamp
  coverage).

- **§4.16 `texture-dma-ab` v0.1 SHIPPED PASS on Metal** as a narrow
  smoke test under pbkit's default DMA aliasing. Renders the same
  1x1 RED texture via CONTEXT_DMA=0 (channel A) and CONTEXT_DMA=2
  (channel B per `pgraph.c:2679-2680`; the field encoding is 0=A,
  2=B, not 0/1). Under pbkit defaults both channels alias the same
  RAMIN object (#3) → same VRAM base → v0.1 cannot distinguish a
  routing regression. Documented as such. v0.2 needs xbed_dma helper
  for proper per-channel base-address testing (task #18). Codex
  review returned BLOCKING on initial cut (selector value 1→2,
  pbkit aliasing, unneeded screenshot-source override). All three
  findings adopted in-session.

- **§4.8 `swizzle-mipmap` v0.2 SHIPPED EXPECTED_FAIL Metal+GL.** The
  XBE catches REAL renderer correctness gaps in BOTH renderers and
  ships as a SPEC oracle for them:
  - **Task #16 (Metal):** SZ_A8R8G8B8 swizzled-texture sampling
    collapses all UVs to texel (0, 0) within the selected mip.
    Verified: per-cell tints (255→223→191→159→127→95→63) correctly
    discriminate by mip (proving the new LOD-clamp fix works), but
    cell 0's 4 sub-quads sampling Q0..Q3 all return Q0's color
    instead of the 4 distinct quadrant colors.
  - **Task #17 (GL):** cell 0 renders the 4-quadrant pattern
    correctly (proves the XBE design is valid), but cells 1..6
    render BLACK when MIN_LOD_CLAMP == MAX_LOD_CLAMP > 0. Needs
    deeper diagnosis of GL texture upload + sampler state path.
  Codex review returned BLOCKING on v0.1 (per-cell rebind with
  MIPMAP_LEVELS=1 sidestepped xemu's mip-chain traversal); BLOCKING
  retained on v0.2 (uniform mip data didn't discriminate intra-mip
  swizzle) — both findings adopted: v0.2 single-bind with
  MIPMAP_LEVELS=7 and per-mip non-uniform pre-swizzled patterns.

- **Metal renderer fix:** `mtl/texture_pg.c::build_sampler_desc_from_pg`
  now honors `SET_TEXTURE_CONTROL0` MIN_LOD_CLAMP / MAX_LOD_CLAMP
  (was hardcoded 0 / levels-1) and `NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS`
  (was computed but never written to MTLSamplerDescriptor). Also
  removed a `(max_lod > 0.0f) ? max_lod : FLT_MAX` overload in
  `texture.mm::build_sampler` that masked guest `MAX_LOD_CLAMP = 0`
  writes; prewarm path updated to pass FLT_MAX explicitly.
  Per-cell mip-ramp on swizzle-mipmap is the regression gate.

- **xbe-harness `metal_canonical_overrides`** field shipped in
  `xbe_discover.py` + `xbe_orchestrator.py`. Per-XBE env-var
  overrides merged into the canonical Metal recipe so XBEs that need
  `XEMU_METAL_SCREENSHOT_SOURCE=nv2a` (for byte-exact non-saturated
  pixel checks) can request it without abusing
  `additional_metal_recipes`. Used by combiner-basic + swizzle-mipmap.

**Previous session (2026-05-20 late evening) closures preserved
below for diagnostic continuity.**

- **Task #15 — `texture-filter-wrap` "Metal TEX0 propagation" was a
  test authoring bug, NOT a Metal renderer gap.** The XBE used
  normalized [0..1] UVs but the PSH's `norm0()` divides UVs by
  `textureSize / texScale[0]`, expecting TEXEL-UNIT [0..TEX_W] UVs.
  All UVs in [0..1] mapped to texel 0 = RED. Fix: rewrite UVs in
  texel-unit space (0.5..6.5 for the 4x4 texture). Removed `metal`
  from `expected_fail_renderers`. PASS on Metal. The nxdk mesh
  sample confirms texel-unit UVs as the linear-texture convention.

- **Task #13 — Metal FLAT-shaded OP_QUADS (real renderer gap).**
  Added CPU-side flat-color propagation in
  `mtl/vertex.c::pgraph_mtl_propagate_flat_quad_colors`. For
  `PRIM_TYPE_QUADS` with `!smooth_shading && !first_vertex_is_provoking`,
  replicates vertex 3's DIFFUSE / SPECULAR / BACK_DIFFUSE /
  BACK_SPECULAR across vertices 0/1/2. Then sets
  `pg->smooth_shading=true` temporarily so the native_quad fast
  path accepts the draw (no GS needed). New counter
  `METAL_FLAT_QUAD_PROPAGATIONS` validates path activation. New
  `flat-quad-propagation` XBE (BLACK distractors on v0/v1/v2,
  expected color on v3) PASSes with counter assertion confirming
  the propagation engaged. **QUAD_STRIP intentionally excluded** --
  vertex sharing makes single-pass CPU propagation incorrect
  (Codex 2026-05-20 review); deferred to follow-up second-wave XBE
  + vertex-duplication path.

- **Task #14 residual — `stencil-ops` "first cells BLACK" — root
  cause CORRECTED 2026-06-18 (see the `Session 2026-06-18` entry at
  the top of this file; supersedes the cross-queue-race / clear-sync
  framing that previously sat here).** The real cause was a
  **guest-side XBE vertex-buffer-reuse race in `stencil-ops/main.c`**,
  not a renderer cross-queue race and not the `SET_CLEAR_RECT`
  clear path. The XBE reused one 6-vertex VRAM buffer in place per
  cell; the host renderer reads vertices lazily at flush, so TCG
  raced ahead and every draw decoded the last cell's geometry. The
  task #14 `s_clear_done_event` fence + `[cmd waitUntilCompleted]`
  clear-sync (opt-out `XEMU_METAL_NO_CLEAR_SYNC=1`, default OFF) and
  the `SET_CLEAR_RECT` sub-rect path are CORRECT and retained — the
  per-clear sync merely widened the timing window that exposed the
  latent XBE bug. **Fix:** a write-once vertex buffer in the XBE
  (96 verts, op at `idx*12`, probe at `idx*12+6`, built once before
  the cell loop, mirroring `blend-matrix`). `metal` removed from
  `expected_fail_renderers`. **stencil-ops PASS deterministic 8/8 on
  Metal** (3 boots + `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` cross-check);
  renderer unchanged by the fix. Same renderer-agnostic vertex-race
  class previously confirmed for `image-blit` (cycle-15).

**Current first-wave XBE status (per `diagnostic-xbe-plan.md` §4):**
> NOTE (2026-06-18): the canonical Metal board is the regenerated
> `xbe-coverage-matrix.md` + the `Session 2026-06-18` entry at the top
> of this file — **14 PASS / 0 FAIL / 1 xfail (`logic-ops`) / 1 skip
> (`pipeline-smoke`, Tier-4 XOSS-routed) / 1 not-built
> (`msaa-aa-factor`)**. The list below is the May-20/21-era slice
> snapshot retained for continuity; where it conflicts with the dated
> board (it counts `pipeline-smoke` as a board PASS and `msaa-aa-factor`
> v0.1 as PASS), **the board wins.**
- **PASS on Metal (15):** pipeline-smoke, mirror, color-channel,
  depth-floor, crtc-publish, native-quad-tri-depth, cmp-vertex-format,
  blend-matrix, texture-format-sweep, texture-filter-wrap, stencil-ops,
  flat-quad-propagation, combiner-basic, msaa-aa-factor v0.1 (path-
  activation + edge-AA-band SMOKE; per-mode profile + AA-band lower-
  bound queued for v0.2 per Codex 2026-05-21, NEW), texture-dma-ab
  (pbkit-aliased smoke).
- **expected_fail (2):** logic-ops (NV2A logic-op feature not
  implemented in either renderer; serves as SPEC oracle) and
  swizzle-mipmap (catches task #16 Metal swizzle position decode +
  task #17 GL LOD-clamp regression; serves as SPEC oracle for both).
- **Unstarted (1):** §4.13 texture-shader-stages (19 NV2A texture
  shader modes; needs combiner-helper + texture-shader-stage
  infrastructure -- significant scope; deferred).

**Tracked follow-ups (not blocking the bulk of M15 prep but
required for full XBE saturation):**
- Task #16: Metal SZ_A8R8G8B8 sampling collapses to texel (0, 0).
  Captured by swizzle-mipmap. Manifests as cell-level mip ramp
  working (LOD clamp now honored, per the 2026-05-21 renderer fix)
  but intra-mip 2x2 quadrant sampling returning Q0 only. **DEEPER
  DIAGNOSIS 2026-05-21 (research pass):** CPU data ARRIVING at
  the Metal renderer is correct (per
  `XEMU_METAL_DIAG_ATTRIB_DUMP=1` instrumentation): per-vertex
  slot-9 stream has 4 distinct UV groups per cell; per-mip
  unswizzled texture buffer has 4 distinct quadrant colors. The
  bug is in the **pipeline-key / vertex-descriptor** path: when
  the GLSL→SPIR-V→MSL pipeline emits its `vertex_main0_in` struct,
  many compiled pipelines declare ONLY `float4 v0
  [[attribute(0)]];` — no `v9 [[attribute(9)]]`. The vertex
  shader then reads slot 9 as `float4 v9 = _60.inlineValue[8]`
  (a CONSTANT uniform value, not a per-vertex stream). A subset of
  pipelines (e.g. pipeline-0013 in the dump) DO include the v9
  attribute and read `in.v9` correctly — so the bug is selective.
  Mechanism likely: pipeline-key state-snapshot timing, where the
  shader gets compiled before slot 9 is flagged as "streaming" (or
  the `uniform_attrs` mask bit-9 is set spuriously). PT0-override
  diagnostic (now reverted) showed `pT0 = (x_screen/640,
  y_screen/480, ...)` linear gradient instead of per-sub-quad
  constant, consistent with the slot 9 attribute not flowing
  through the vertex stream. **Fix candidate:** investigate
  `mtl/state.c::pipeline_key_build` around line 258-280 — verify
  slot 9 is included in `out_key->attrs[i]` when the XBE binds
  it. Also `pgraph_mtl_collect_all_vertex_streams` flagging logic
  in `vertex.c`. The env-gated diagnostic
  `XEMU_METAL_DIAG_ATTRIB_DUMP=1` is the regression gate (now
  documented in `automation.md`).
- Task #17: GL renderer renders BLACK when MIN_LOD_CLAMP =
  MAX_LOD_CLAMP > 0. xemu's `pgraph_get_texture_shape` truncates
  `levels = MIN(levels, max_mipmap_level + 1)` and the GL upload
  uses GL_TEXTURE_BASE_LEVEL = min_mipmap_level, which should work
  -- but mip > 0 data renders as BLACK. Needs deeper diagnosis.
- Task #18: §4.16 texture-dma-ab v0.2 needs guest-side RAMIN/DMA
  object setup (a new `xbed_dma` helper) so DMA A and DMA B
  resolve to distinct VRAM bases. Without this, v0.1's smoke
  cannot regress-gate per-channel base-address translation.

**M15 default-on prerequisite:** "all priority XBEs PASS on Metal"
per `metal-renderer-plan.md` §4 + decision-log 2026-05-20 evening.
Today: 15 PASS + 2 expected_fail + 1 unstarted of 17. The 2
expected_fail are documented SPEC oracles (one feature work, one
regression target for already-tracked bugs) -- whether they count
toward the PASS gate is a decision-log question. Conservatively
they do NOT count, so the gate needs:
- §4.13 texture-shader-stages SHIPPED + PASSING on Metal
- Tasks #16 + #17 resolved → swizzle-mipmap flips to PASS
- §4.15 msaa-aa-factor v0.2 second-wave follow-up (per-mode keyed
  oracle + AA-band lower-bound) — gates the "per-mode AA factor
  profile" portion of §4.15 that v0.1 explicitly defers

Then re-evaluate per `m15-bundle-status.py` and run the tracked-title
oracle (PGR2 / Rainbow / Crimson / Halo / SC2) as the final
acceptance gate.

**Codex review.** `/codex-validate changes` returned MAJOR ISSUES
on the initial slice; adopted all 3 findings in-session:
QUAD_STRIP narrowed to QUADS only; counter+flag documentation
added; this banner is the sync. See decision-log
"2026-05-20 (late evening, +3 closures)".

Previous banner (texture-filter-wrap initial expected_fail) is
superseded; older banners preserved below for diagnostic
continuity.

---

