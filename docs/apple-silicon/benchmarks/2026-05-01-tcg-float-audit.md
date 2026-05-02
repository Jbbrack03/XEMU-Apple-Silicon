# 2026-05-01 — TCG SSE/x87 hardfloat audit

## Question

The PGR2 mid-route sample profile after the three opt-in flags
(`docs/apple-silicon/benchmarks/2026-05-01-pgr2-bottleneck-postfast.md`)
showed prominent CPU time in `helper_mulss`, `helper_mulps_xmm`,
`helper_fmul_ST0_FT0`, `floatx80_mul`, `float32_mul`, `soft_f32_mul`, and
`parts64_uncanon_normal`. The handoff lists this as the largest potential
TCG win on Apple Silicon if SSE float32 ops are unnecessarily going through
softfloat when NEON has native float32 (Prioritized Next Tasks #3, "Open
investigation"). This audit is source-only: does the SSE hardfloat fast
path actually exist, is it active on aarch64 hosts, does it benefit
`helper_mulss`, and is x87 80-bit irreducibly soft.

## Findings

### Does QEMU have a hardfloat path for float32 ops?

Yes. `fpu/softfloat.c` defines a generic two-input dispatcher
`float32_gen2`/`float64_gen2` that conditionally takes a hardfloat
shortcut.

- Definition: `fpu/softfloat.c:337-366` (`float32_gen2`).
- Hardfloat op for mul: `fpu/softfloat.c:2159-2162`:

  ```c
  static float hard_f32_mul(float a, float b) { return a * b; }
  ```

- Wiring: `fpu/softfloat.c:2169-2174`:

  ```c
  float32 QEMU_FLATTEN
  float32_mul(float32 a, float32 b, float_status *s)
  {
      return float32_gen2(a, b, s, hard_f32_mul, soft_f32_mul,
                          f32_is_zon2, f32_addsubmul_post);
  }
  ```

- Soft fallback: `soft_f32_mul` at `fpu/softfloat.c:2135-2145` →
  `parts_mul` → `parts64_uncanon_normal` (the symbols seen in the sample
  profile).

The shortcut gate is `can_use_fpu(s)` at `fpu/softfloat.c:230-237`:

```c
static inline bool can_use_fpu(const float_status *s)
{
    if (QEMU_NO_HARDFLOAT) {
        return false;
    }
    return likely(s->float_exception_flags & float_flag_inexact &&
                  s->float_rounding_mode == float_round_nearest_even);
}
```

`QEMU_NO_HARDFLOAT` is only forced on under `-ffast-math`
(`fpu/softfloat.c:220-228`) — it is 0 in a stock build. So the hardfloat
path is compiled in on every host that QEMU supports.

### Is the hardfloat path active on aarch64 hosts?

Yes — and there is no aarch64-specific gate that disables it.

- `fpu/softfloat.c:187-201` only conditionally enables `*_USE_FP`
  (fpclassify-based zero/normal checks) on `__x86_64__`, but those macros
  control _how_ the pre-checks are written, not whether the hardfloat
  shortcut is taken at all. The fast path exists on all hosts.
- `fpu/softfloat.c:209-213` enables `QEMU_HARDFLOAT_USE_ISINF` for both
  `__x86_64__` and `__aarch64__`, confirming the codebase explicitly
  considers aarch64 a first-class hardfloat host.
- No `host/include/aarch64/` header disables hardfloat. Searched
  `host/include/` for `HARDFLOAT|hardfloat` — only the `fpu/softfloat.c`
  references appear.
- `meson.build:295` sets `host_arch = cpu` for `aarch64`; nothing in the
  build system sets `-DQEMU_NO_HARDFLOAT` or `-ffast-math` for arm64
  hosts. (`build.sh -a arm64` only sets `-mmacosx-version-min` and CMake
  arch; it does not pass `-ffast-math`.)

So on Apple Silicon, `can_use_fpu(s)` is reachable. Whether it returns
`true` at runtime depends on the float_status state — see the gating
section below.

### What does `helper_mulss` actually call at runtime?

Trace:

1. `target/i386/ops_sse.h:516`:
   `#define FPU_MUL(size, a, b) float ## size ## _mul(a, b, &env->sse_status)`.
2. `target/i386/ops_sse.h:530`: `SSE_HELPER_S(mul, FPU_MUL)` instantiates
   both `helper_mulps`/`helper_mulps_xmm` (via `SSE_HELPER_P` at
   `ops_sse.h:466-483`) and `helper_mulss`/`helper_mulsd` (via
   `SSE_HELPER_S` at `ops_sse.h:487-506`). The body for `mulss` is:

   ```c
   void helper_mulss(CPUX86State *env, Reg *d, Reg *v, Reg *s)
   {
       d->ZMM_S(0) = float32_mul(v->ZMM_S(0), s->ZMM_S(0), &env->sse_status);
       /* lanes 1..3 copied from v */
   }
   ```

3. `helper_mulps_xmm` is the `SHIFT == 1` instantiation looping
   `float32_mul` over four lanes (`ops_sse.h:471-473`).
4. The instantiation site is `target/i386/tcg/fpu_helper.c:3509-3517`,
   which `#include`s `ops_sse.h` three times with `SHIFT 0/1/2`.
5. Both ultimately call `float32_mul(...)` → `float32_gen2(...)` →
   either `hard_f32_mul` (a single arm64 `fmul`) or
   `soft_f32_mul` → `parts_mul` → `parts64_uncanon_normal`.

So the SSE arithmetic helpers go through the shortcut-capable dispatcher.
**Whether the shortcut is taken depends on `can_use_fpu(&env->sse_status)`
at the moment of the call.**

The relevant `sse_status` state is set by guest `LDMXCSR` /
`FXRSTOR` / `XRSTOR` and never cleared by SSE arithmetic helpers
themselves. The standard SSE arithmetic helpers (`mulss`, `mulps`,
`mulsd`, `mulpd`, `addss`, `subss`, `divss`, `min/max`, `sqrt`) do **not**
call `set_float_exception_flags(0, ...)` before each op — only the
`WRAP_FLOATCONV` macro for cvt-to-int operations
(`target/i386/ops_sse.h:703-718`) clears flags, and even that one ORs
them back in. So the inexact flag accumulates naturally:

- `cpu_set_mxcsr` (`target/i386/cpu.h:2804-2810`) → `update_mxcsr_status`
  (`target/i386/tcg/fpu_helper.c:3447-3470`) → `set_float_exception_flags`
  with `float_flag_inexact` iff guest MXCSR has the PE bit set
  (`fpu_helper.c:3457-3463`).
- After the first inexact softfloat op, `float_flag_inexact` becomes
  sticky until guest writes MXCSR.

For Xbox titles whose inner loops do not repeatedly clear MXCSR (the
common case), the steady-state behavior is: first op of a sequence is
soft (and sets PE), subsequent ops can take the hard shortcut **provided
both inputs are normal/zero (post-FTZ flush) and rounding mode is
nearest-even** (typical SSE state).

### What does `helper_fmul_ST0_FT0` call?

Trace:

1. `target/i386/tcg/fpu_helper.c:780-785` (soft path body):

   ```c
   void helper_fmul_ST0_FT0(CPUX86State *env)
   {
       int old_flags = save_exception_flags(env);
       ST0 = floatx80_mul(ST0, FT0, &env->fp_status);
       merge_exception_flags(env, old_flags);
   }
   ```

2. `floatx80_mul` is at `fpu/softfloat.c:2218-2230`:

   ```c
   floatx80 QEMU_FLATTEN
   floatx80_mul(floatx80 a, floatx80 b, float_status *status)
   {
       FloatParts128 pa, pb, *pr;
       if (!floatx80_unpack_canonical(&pa, a, status) ||
           !floatx80_unpack_canonical(&pb, b, status)) {
           return floatx80_default_nan(status);
       }
       pr = parts_mul(&pa, &pb, status);
       return floatx80_round_pack_canonical(pr, status);
   }
   ```

   There is **no** `floatx80_gen2` dispatcher and **no** `hard_floatx80_mul`
   in `fpu/softfloat.c`. 80-bit ops are unconditionally soft.

3. The fork ships an Xbox-specific 80-bit hardfloat shim
   (`floatx80_mul__hard`) in `target/i386/tcg/fpu_helper.c:104-108` and
   `target/i386/tcg/fpu_helper_hard.c:1-4`, but it is **gated on
   `XBOX && __x86_64__`**. Confirmed at:

   - `target/i386/helper.h:104` — `HS_DEF_HELPER_*` only emits
     `__soft`/`__hard` pairs when `XBOX && __x86_64__`.
   - `target/i386/tcg/fpu_helper.c:76` — same gate around the hard impl.
   - `target/i386/tcg/fpu_helper.c:267` — closing `#endif`.
   - `target/i386/tcg/translate.c:40-124` — the `MAP_GEN_HELPER_SOFT_HARD`
     dispatch macros are only defined under `XBOX && __x86_64__`; outside
     that gate the helper names resolve directly to the soft helpers.
   - `target/i386/tcg/fpu_helper_hard.c:1-4` — the hard implementation
     translation unit is empty on aarch64.
   - `target/i386/tcg/translate.c:4230-4232` — `g_use_hard_fpu =
     g_config.perf.hard_fpu;` is only assigned under the same gate.
   - `ui/xui/main-menu.cc:62-66` — the "Hard FPU emulation" UI toggle is
     `#if defined(__x86_64__)` only, so it is not even visible in the
     Apple Silicon build.

   The reason the gate is correct: `floatx80_mul__hard` uses `long
   double` arithmetic; on x86-64 SysV ABI `long double` is 80-bit
   (matching x87), but on Apple Silicon `long double == double` (verified
   `sizeof(long double) == 8` with the system clang). There is no native
   80-bit float on aarch64, so a "hard" floatx80 path is impossible
   without precision loss.

4. Independent of the missing fast path, `helper_fmul_ST0_FT0` calls
   `save_exception_flags(env)` (`fpu_helper.c:396-401`) which
   `set_float_exception_flags(0, &env->fp_status)` immediately before
   every call. Even if a `floatx80_gen2`-style dispatcher existed, that
   clear would defeat `can_use_fpu`'s `float_flag_inexact` precondition
   on every call. (This is why the existing `__hard` path bypasses
   `save_exception_flags` entirely and just packs the result via
   precision rounding, accepting the FIXME on flags/exceptions noted at
   `fpu_helper.c:79`.)

**Confirmed**: `helper_fmul_ST0_FT0` on aarch64 always goes through
`floatx80_mul` → `parts_mul` → `parts64_uncanon_normal`. There is no
hardfloat shortcut available, and the cost is irreducible without either
(a) emulating 80-bit precision in software (what we already do), (b)
truncating to 64-bit double precision (precision loss the FPU control
word's PC field actually allows for some games — see below), or (c)
a NEON/SVE 80-bit emulation kernel optimized for the specific recurring
operation pattern.

### What rounding/exception conditions gate the hardfloat shortcut?

From `fpu/softfloat.c:230-237`, the shortcut is taken only when **all**
of the following hold for `&env->sse_status`:

1. `QEMU_NO_HARDFLOAT == 0` — true unless built with `-ffast-math`.
2. `s->float_exception_flags & float_flag_inexact` is set. Sticky in
   normal use; cleared by `LDMXCSR`/`FXRSTOR` of an MXCSR with PE clear.
3. `s->float_rounding_mode == float_round_nearest_even` — i.e. the
   guest's MXCSR RC field is `00`. SSE in retail games is almost always
   round-to-nearest; non-default rounding modes (round-down/up/zero) take
   the soft path automatically.

Within the shortcut, additional per-call gates from
`float32_gen2`/`float64_gen2` (`fpu/softfloat.c:337-397`):

4. `pre(ua, ub)` — `f32_is_zon2`/`f64_is_zon2`
   (`fpu/softfloat.c:270-292`): both inputs are zero-or-normal after FTZ
   input flush. NaN/Inf inputs fall to soft.
5. After the hard op, if the result is `+/-Inf`, `float_flag_overflow`
   is raised and the result is still returned.
6. If `fabsf(ur.h) <= FLT_MIN` and the post-check `f32_addsubmul_post`
   (`fpu/softfloat.c:1975-1981`) signals the result is not `0 * 0`, fall
   to soft (denormal handling).

For Xbox-game gameplay code these gates are usually satisfied: MXCSR =
`0x1F80` (mask all, RC=00), inputs are normal, results are normal-range
floats. The first op in a guest-initiated reset bursts a soft op to set
PE; subsequent ops fast-path.

## Bottom line

**Partial opportunity, smaller than initially hypothesized — and most of
it is already enabled on aarch64.**

- The hardfloat fast path exists (`fpu/softfloat.c:337-397`) and is
  compiled in on Apple Silicon. There is no `__x86_64__` gate around the
  shortcut itself; the only `__x86_64__` gate is on the
  `*_USE_FP`/fpclassify pre-check style, which is a micro-optimization
  not a fast-path on/off switch. So `helper_mulss`/`helper_mulps_xmm`
  already get the hard `a * b` (single arm64 `fmul`) call when the
  guest's MXCSR is in its overwhelmingly common state.
- The `parts64_uncanon_normal` time visible in the profile is the cost
  of the **soft fallback paths that are still hit**: (a) the first op
  after any MXCSR clear of PE; (b) any op with NaN/Inf/denormal inputs
  or denormal results; (c) any op while the guest is using a non-default
  rounding mode. PGR2 will hit (a) on every context switch that restores
  MXCSR with PE clear, and (b) on common cases like vertex-blend code
  that produces near-zero deltas. None of these are a software defect.
- `helper_fmul_ST0_FT0` and the rest of the x87 surface are
  **irreducibly soft** on aarch64. The fork's existing `__hard` x87 path
  is correctly gated to `XBOX && __x86_64__` because Apple Silicon's
  `long double` is 64-bit; there is no native 80-bit float to dispatch
  to. PGR2 still uses x87 in some math routines (the
  `helper_fmul_ST0_FT0` time in the profile is real x87 use, not SSE),
  and that cost cannot be eliminated without sacrificing 80-bit
  precision or hand-writing a NEON 80-bit kernel.

### Recommendation

1. **Drop the framing of "SSE float ops unnecessarily going through
   softfloat" from Prioritized Next Tasks #3.** The premise is
   incorrect for the SSE float32/float64 path: the hardfloat fast path
   is already on, and the visible `parts64_*` time is the necessary cost
   of softfloat's correctness fallback for edge cases plus the first-op
   cost after MXCSR resets — not an unconditional softfloat trip.

2. **Cheap measurement to quantify what is left, before any code work**
   (this is the targeted experiment that resolves remaining uncertainty
   without guessing): add a counter pair around `float32_gen2`/
   `float64_gen2` that increments `sse_hard_taken` when the shortcut is
   used and `sse_soft_fallback` when `goto soft` fires (with one
   counter per fall-through reason: `!can_use_fpu`, `!pre`, `denormal
   result`). Aggregate per `XEMU_PERF_LOG_INTERVAL_MS`. Run on the PGR2
   mid-route snapshot for 30 s. If `sse_hard_taken/(hard+soft) > 0.9`,
   then the visible `parts64_uncanon_normal` time is irreducible
   correctness work and SSE further work is not warranted; redirect to
   the next dominant subsystem identified by Instruments. If the ratio
   is unexpectedly low, the counter breakdown identifies the dominant
   fall-through reason and the next investigation target.

3. **For x87 specifically (`helper_fmul_ST0_FT0`, `floatx80_mul`, the
   `parts128_*` tail), the only realistic Apple-Silicon-specific wins
   are**:
   - Implement a NEON-based `floatx80_mul`/`floatx80_add` kernel that
     produces the bit-exact 80-bit result via two double-precision
     multiplies + correction (Dekker / TwoProduct). High effort, real
     win only if x87 is dominant for a tracked title.
   - Add an opt-in `XEMU_X87_RELAXED_PRECISION=1` (off by default) that
     follows the FPU control word's `PC` (precision control) field: when
     the guest sets PC=double or PC=single, dispatch to `double` /
     `float` arithmetic via NEON instead of going through softfloat.
     This is **correctness-relaxing** if the guest ever uses PC=extended
     while expecting bit-exact results; but a number of titles set PC=
     double and never reset it. Gate it behind a per-title opt-in. Do
     **not** add this without first validating that PGR2/Crimson/R6 do
     not require PC=extended for their math.

   Either of these is a Phase-2-scope effort, not a quick win. They
   should be deferred until Instruments / a measurement confirms x87 is
   dominant in real-game inner loops, not just visible in the profile.

4. **Update the handoff** to reflect: SSE hardfloat shortcut already
   active on aarch64; remaining float-helper time is partly irreducible
   (x87 80-bit) and partly bounded by softfloat's NaN/denormal fallback
   path. The next slice should be selected from whichever non-float TCG
   subsystem Instruments identifies as dominant after this finding lands
   — likely TLB / memory-op helpers, NV2A PGRAPH command parsing, or
   surface/texture upload, per handoff Prioritized Next Tasks #4-#6.

## Source references

- `fpu/softfloat.c:184-201` — `QEMU_HARDFLOAT_*F{32,64}_USE_FP` macros
  (x86_64-only style choice; not a fast-path on/off).
- `fpu/softfloat.c:209-213` — `QEMU_HARDFLOAT_USE_ISINF` enabled for
  both `__x86_64__` and `__aarch64__`.
- `fpu/softfloat.c:220-228` — `QEMU_NO_HARDFLOAT` only set under
  `-ffast-math`.
- `fpu/softfloat.c:230-237` — `can_use_fpu` rounding-mode and
  inexact-flag preconditions.
- `fpu/softfloat.c:270-292` — `f32_is_zon2` / `f64_is_zon2` pre-check
  (input must be zero or normal).
- `fpu/softfloat.c:337-366` — `float32_gen2` dispatcher.
- `fpu/softfloat.c:368-397` — `float64_gen2` dispatcher.
- `fpu/softfloat.c:1975-1990` — `f32_addsubmul_post` / `f64_addsubmul_post`
  denormal-result fallback gate.
- `fpu/softfloat.c:2135-2174` — `soft_f32_mul`, `hard_f32_mul`,
  `float32_mul` wiring.
- `fpu/softfloat.c:2218-2230` — `floatx80_mul` (no hardfloat shortcut).
- `fpu/softfloat-parts.c.inc:549-601` — `partsN(mul)` body (the
  `parts_mul` template that produces `parts64_mul`/`parts128_mul`).
- `fpu/softfloat-parts.c.inc:429` — `parts_uncanon_normal` site (the
  symbol seen in the sample profile).
- `target/i386/cpu.h:1906` — `float_status sse_status;` member.
- `target/i386/cpu.h:2804-2810` — `cpu_set_mxcsr` triggers
  `update_mxcsr_status` under TCG.
- `target/i386/cpu.h:2812+` — `cpu_set_fpuc` analogue for x87 control
  word.
- `target/i386/helper.h:104-121` — `HS_DEF_HELPER_*` gated to
  `XBOX && __x86_64__`.
- `target/i386/ops_sse.h:466-512` — `SSE_HELPER_P` and `SSE_HELPER_S`
  macros generating `mulps`/`mulss` etc.
- `target/i386/ops_sse.h:514-533` — `FPU_MUL` macro and
  `SSE_HELPER_S(mul, FPU_MUL)`.
- `target/i386/ops_sse.h:703-718` — `WRAP_FLOATCONV` (only place SSE
  helpers manipulate exception flags around a softfloat call; arithmetic
  helpers do not).
- `target/i386/tcg/fpu_helper.c:60` — `FPUS_PE` definition.
- `target/i386/tcg/fpu_helper.c:76-267` — `XBOX && __x86_64__` gated
  `__hard` x87 implementations.
- `target/i386/tcg/fpu_helper.c:323-393` — soft-only init paths and
  `set_float_*` for `sse_status`.
- `target/i386/tcg/fpu_helper.c:396-416` — `save_exception_flags` clears
  `fp_status.float_exception_flags = 0` before every x87 helper.
- `target/i386/tcg/fpu_helper.c:780-785` — `helper_fmul_ST0_FT0` body.
- `target/i386/tcg/fpu_helper.c:957-968` — `set_x86_rounding_mode`
  mapping.
- `target/i386/tcg/fpu_helper.c:3041-3045` — `do_xrstor_mxcsr` →
  `cpu_set_mxcsr`.
- `target/i386/tcg/fpu_helper.c:3447-3493` — `update_mxcsr_status`,
  `update_mxcsr_from_sse_status`, `helper_ldmxcsr`, `cpu_set_mxcsr`
  flow.
- `target/i386/tcg/fpu_helper.c:3509-3517` — `ops_sse.h` instantiation
  with `SHIFT 0/1/2`.
- `target/i386/tcg/fpu_helper_hard.c:1-4` — empty on non-`XBOX
  && __x86_64__`.
- `target/i386/tcg/translate.c:38-124` — `g_use_hard_fpu` and
  `MAP_GEN_HELPER_SOFT_HARD` (gated).
- `target/i386/tcg/translate.c:4230-4232` — `g_use_hard_fpu` assignment
  site.
- `include/fpu/softfloat-types.h:101-117` — `floatx80` struct definition
  (Xbox x86_64 packed `long double` variant vs default hi/lo struct).
- `host/include/aarch64/host/` — directory contents listed; no
  hardfloat-related override file.
- `meson.build:55,287-296` — `host_arch` mapping for `aarch64`; no
  hardfloat-affecting flags applied.
- `ui/xui/main-menu.cc:62-66` — `__x86_64__`-only "Hard FPU emulation"
  UI toggle.
- `build/xemu-config.h:290,791-792` — `perf.hard_fpu` config field.
