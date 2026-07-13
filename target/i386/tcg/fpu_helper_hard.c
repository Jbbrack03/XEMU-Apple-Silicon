/*
 * Dual-compile of fpu_helper.c with USE_HARD_FPU: produces helper_*__hard
 * variants selected at translation time via g_use_fp_jit (default OFF on
 * Android). On AArch64 the hard path uses native double storage/arithmetic,
 * which does not reproduce x87 extended-precision (64-bit mantissa)
 * compare/equality semantics — known to break e.g. Fable: The Lost Chapters.
 * D3D8-era titles set x87 precision control to single, for which double
 * arithmetic is exact, so the flag is a per-game opt-in, not a default.
 */
#if defined(XBOX) && (defined(__x86_64__) || defined(__aarch64__))
#define USE_HARD_FPU 1
#include "fpu_helper.c"
#endif
