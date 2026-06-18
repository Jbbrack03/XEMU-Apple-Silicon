# Metal Parity Roadmap

> **Created 2026-06-15.** This is the canonical *direction* doc: the path from
> today's state to the project goal. Read it after `handoff.md` (current state)
> and `decision-log.md` (binding decisions). It supersedes the scattered
> "next-actions" lists in `handoff.md` and `.claude/rules/renderer-state.md`
> as the single source of strategic structure. Keep it short and parseable —
> if it grows past ~250 lines, move detail into the workstream docs it points to.

## North-star goal (verbatim)

Most Xbox titles playing at **1080p**, **native framerate (30/60)**, with
**high-quality anti-aliasing**, **no framerate jitter**, and **no artifacting**.
The **native Metal backend's output must visually match the real-Xbox Oracle**.

**Acceptance title set** (decided 2026-06-15): the current tracked set —
PGR2, Rainbow Six 3, Crimson Skies, Halo CE, Soul Calibur 2, plus the broader
sweep (Burnout 3, Splinter Cell, Ninja Gaiden Black, OutRun 2). "Most titles"
generalizes from NV2A-feature-surface coverage, *validated* on this ~10-title
set against the Oracle.

## Where we are (honest summary, 2026-06-15)

- **GL renderer meets the goal today** and is the shipping default: 1080p
  (`surface_scale=2`), console-native FPS, MSAA 4× (`XEMU_GL_MSAA=4`). Keep it
  the default until Metal earns the flip; GL stays as permanent fallback.
- **Native Metal renderer (M0–M14 shipped) is fast but not correct or shippable:**
  - Boot/BIOS animation renders solid magenta / green-blob (VGA-direct path has
    no Metal equivalent — a *presentation-layer* bug, **not** PGRAPH/geometry).
  - PGR2 gameplay: multi-RT compositing bug (white HUD bars, corrupt reflections,
    RTT sampling of `0x3c84000`). Crimson paired diff fails `changed_pct≈14.76`.
  - **Frame pacing is worse on Metal than GL**: PGR2 p99 jitter Metal ≈ 300 ms
    vs GL ≈ 41 ms. The "no jitter" goal is currently *violated* by Metal —
    prime suspect is the synchronous `[cmd waitUntilCompleted]` clear-sync hammer.
  - ~15/17 first-wave diagnostic XBEs PASS, but the **NV2A feature surface is
    mostly uncovered** (second-wave ~50–60 XBEs barely started).
- **Process lesson (ground-truthed from git):** a *binding* XBE-first decision
  was made 2026-05-20 (rule #17), then the next three weeks of commits
  (2026-05-30 → 2026-06-09 "sibling-sync", "codex-takeover-frontfb") relapsed
  into PGR2-metric RTT tuning — the exact "fixated on one title" anti-pattern.
  **This roadmap exists to make the discipline enforceable and visible.**
- **No persisted XBE results exist on disk** — "15/17 PASS" is prose in docs,
  not a regenerable artifact. That is how a project loses track. M0 fixes it.

## Operating principles (anti-churn — these are the structure)

1. **XBE-first is binding** (rule #17). No retail-title-metric tuning until the
   coverage board (Workstream A) is green. The 2026-05/06 sibling-sync relapse
   is the cautionary tale: a PGR2-tuned fix regressed boot/Halo/Crimson.
2. **One feature surface at a time, isolated.** A feature is "done" only when its
   XBE passes GL == Metal == Oracle. Then move on. No batching half-fixes.
3. **Every renderer change passes the full regression rotation** (entire XBE board
   + temporal-flicker gate) before it's considered landed. Counter-mode is a
   smoke check only — it has passed while pixels were wrong.
4. **The coverage board is the single source of truth for progress** — generated
   from harness results, never hand-asserted prose. See `xbe-coverage-matrix.md`.
5. **Three things XBEs cannot cover get their own tracks** with their own evidence:
   presentation/VGA-direct (boot), frame-pacing/jitter, and retail-Oracle
   acceptance. Do not try to force them through the XBE loop.
6. **Docs stay parseable.** Current-state docs target < ~200 lines; history is
   archived, not accreted. No canonical doc grows unbounded (Workstream E).
7. **No guessing** (rule #1). Every change is justified by an XBE result, a perf
   counter, source reading, or research — at high confidence — before it lands.

## Workstreams

- **A — Geometry & feature correctness (XBE saturation). PRIMARY.** Build the
  remaining `xbed_lib` infra, then one XBE per uncovered NV2A feature surface;
  when an XBE fails on Metal, fix the renderer *at that feature in isolation*.
  Drives toward an all-green coverage board. (`diagnostic-xbe-plan.md`,
  `nv2a-feature-surface-research.md`, `xbe-coverage-matrix.md`.)
- **B — Presentation layer.** VGA-direct boot path Metal equivalent (the boot
  magenta/green-blob), CRTC publish / front-fb policy. Evidence: boot temporal
  capture, **not** XBE. (M5.13/M18 in `metal-renderer-plan.md`.)
- **C — Performance & frame pacing.** Bring Metal p99 jitter into parity with GL
  across tracked titles; remove synchronous stalls (clear-sync hammer) and
  shader-compile stalls. Evidence: jitter counters + stall-attribution counters
  (M0 shipped `METAL_CLEAR_SYNC_US_TOTAL`). **Lead finding 2026-06-15:** the
  synchronous clear-sync `[cmd waitUntilCompleted]` costs ~1.08 ms CPU stall *per
  clear* (measured) — a primary suspect for the GL≈41 ms vs Metal≈300 ms PGR2 p99
  jitter gap. Confirm on a tracked retail title; evaluate the fence-only path.
- **D — Retail Oracle acceptance. FINAL GATE.** Tracked ~10 titles' gameplay vs
  real Xbox, visual + temporal within tolerance, no artifacting.
  (`oracle-workflow.md`.)
- **E — Documentation & memory health. ENABLER (do early).** Restructure the
  1 MB `handoff.md` / `decision-log.md` into parseable current-state + archived
  history; reconcile auto-memory; enforce Anthropic memory/CLAUDE standards.

## Milestones (gated)

Each milestone has a hard **exit gate**. Do not advance a dependent milestone
until the gate is green. A → B → C may run in parallel after M0; D gates on
A+B+C; M-V gates on D.

- **M0 — Foundations & visibility** *(this + next sessions)*
  - Build the **coverage-matrix generator** (reads XBE manifests + harness
    results → regenerable green/red board).
  - Build the **per-frame Metal stall-attribution** tool (Workstream C input).
  - Complete the **doc/memory health pass** (Workstream E first slice).
  - **Baseline** the full XBE board + Metal/GL jitter on the current build.
  - *Exit:* the green/red board is regenerable from disk; current truth captured
    and replaces all prose counts.
  - **STATUS 2026-06-15 — M0 substantially COMPLETE** (see
    `benchmarks/2026-06-15-m0-baseline-and-clear-sync.md`): coverage generator
    shipped + validated; clear-sync counter shipped + validated; docs/memory
    health done; Metal XBE board baselined (15 PASS / 1 FAIL `stencil-ops` →
    task #10 / 1 xfail / 1 not-built; 53/91 surfaces uncovered).
    **(task #10 CLOSED 2026-06-18: `stencil-ops` PASSes — the failure was a
    guest XBE vertex-buffer-reuse race, not a renderer clear bug. Since then the
    §F.2 `alpha-test` (PASS) and §C.3 `polygon-offset` (xfail — CONFIRMED Metal
    depth-bias gap, fix tracked as the NEW task #10) M-I second-wave slices
    shipped; current board per `xbe-coverage-matrix.md` is **15 PASS / 0 FAIL /
    2 xfail (`logic-ops`, `polygon-offset`) / 1 skip / 1 not-built; 40/91
    covered, 51 uncovered**.)** Remaining:
    GL + real-Xbox board legs (real-Xbox Tier-4 capture is task #8, needs
    user/hardware) and the retail jitter baseline (folds into M-III).
- **M-I — Feature-surface saturation** *(Workstream A)*
  - Every enumerated NV2A feature surface has an XBE; each PASSes on Metal
    (== GL == Oracle) or is a documented `expected_fail` / `N-A` with rationale.
  - *Exit:* coverage board all-green (no untracked surface, no unexplained red).
- **M-II — Presentation parity** *(Workstream B)*
  - Boot animation + CRTC publish correct on Metal; boot temporal capture
    Metal == GL (`solid_frame_count == 0`, blink rate within 2× of GL).
  - *Exit:* boot + menu sequences render clean on Metal, temporally stable.
- **M-III — Performance parity** *(Workstream C)*
  - Metal p99 jitter within band of GL on every tracked title; sustained
    native FPS; no synchronous-wait stalls in the steady-state frame.
  - *Exit:* jitter gate green across the tracked set.
- **M-IV — Retail Oracle acceptance** *(Workstream D)*
  - All tracked titles' gameplay matches the Oracle within tolerance, no
    artifacting, temporally clean (paired GL/Metal/Oracle, snapshot-anchored).
  - *Exit:* Oracle gate green on the full tracked set.
- **M-V — Metal default-on flip (M15)**
  - M-I…M-IV all green → flip Metal default-on for Apple Silicon; GL retained
    as fallback. Append decision-log entry; flip the flag.

## Tooling to build (visibility gaps)

Ranked. Each unblocks a workstream; build before substituting weaker evidence (rule #5).

1. **Coverage-matrix generator** — no persisted XBE results exist today. Without
   this, progress is prose and drifts. *(M0, unblocks A.)*
2. **Per-frame Metal stall-attribution** — attribute worst-frame time to
   shader-compile / clear-sync wait / texture upload / present wait / guest CPU
   (extend the TCG perf spike-log to the Metal path). *(M0, unblocks C.)*
3. **`xbed_lib` infra slices** — combiner-stage builders, fixed-function pipeline
   helpers, Tier-2 format-decoder dispatch tables (`rt[10]`, `tex[42]`). Prereqs
   for whole XBE classes (combiner, FFP, palettized, deep texture). *(M-I, A.)*
4. **XBE-on-real-Xbox Tier-1 capture** — fix the pbkit `D:\` fopen issue so
   diagnostic XBEs validate against *hardware*, not just the math-derived oracle
   (triangulate GL == Metal == real-HW). Raises confidence of the whole board. *(A/D.)*
5. *(nice-to-have)* headless `.gputrace` summary for Metal multi-RT debugging.

## Definition of done

All five milestones green; Metal is default-on on Apple Silicon for the tracked
set with GL retained as fallback; the coverage board is all-green and
regenerable; canonical docs are parseable and auto-memory is accurate.

## Cross-references

- Current state: `handoff.md`. Binding decisions: `decision-log.md`.
- Feature surface: `nv2a-feature-surface-research.md`. XBE plan: `diagnostic-xbe-plan.md`.
- Live coverage board: `xbe-coverage-matrix.md`. Metal slices: `metal-renderer-plan.md`.
- Tools/flags: `automation.md`. Oracle: `oracle-workflow.md`.
- Doc/memory health plan: Workstream E below.

## Workstream E detail — Documentation & memory health

Enforces Anthropic's memory standard (https://code.claude.com/docs/en/memory)
and project rule #4 (no doc drift). Findings 2026-06-15:

- **CLAUDE.md files: compliant.** Workspace 134 lines, fork 38 lines (both < 200).
  No action beyond periodic conflict review.
- **`handoff.md` (1.05 MB / 13.2k lines): the core problem.** It conflates a
  small "current state / next actions" with 13k lines of chronological history.
  *Plan:* extract current-state into a short living head (target < 250 lines)
  that points to this roadmap + the coverage board; move historical cycle
  entries into `_archive/handoff-history/` (dated chunks) with an index. The
  current-state head becomes the read-first artifact.
- **`decision-log.md` (1.03 MB / 15.5k lines): append-only, unparseable.** Keep
  append-only discipline but chunk by period into `_archive/decision-log/<period>.md`
  with an index, leaving only recent + still-binding decisions in the main file.
  Never lose a binding decision or supersession marker (rules #4, #6).
- **`automation.md` (281 KB): large but legitimate reference.** Lower priority;
  optionally split by flag family to mirror `.claude/rules/flags-*.md`.
- **Auto-memory: stale.** `project_state.md` (9.7 KB) was a detailed snapshot
  duplicating `handoff.md` — rewritten 2026-06-15 to a concise durable-priors
  pointer. `MEMORY.md` index lines trimmed to true one-liners.
- **Sequencing:** do the memory reconciliation immediately (low-risk, done
  2026-06-15); the `handoff.md` / `decision-log.md` surgery is a *reviewed* M0
  task (large, touches canonical append-only docs — execute deliberately, not
  hastily). Re-point `README.md` + `handoff.md` head at this roadmap during that pass.
