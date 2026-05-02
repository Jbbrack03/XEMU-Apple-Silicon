/*
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2019, 2024 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "system/whpx.h"
#include "system/cpu-timers.h"
#include "trace.h"

#include "hw/i386/x86.h"
#include "target/i386/cpu.h"
#include "hw/intc/i8259.h"
#include "hw/irq.h"
#include "system/kvm.h"

#if defined(XBOX) && defined(__APPLE__)
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#endif

/*
 * Apple Silicon performance fork: per-interval RDTSC call counter.
 * Always-on atomic; emitted on the xemu-perf interval line via the
 * NV2A profile module's flush path. Surfaced in extract-perf-summary.sh
 * so V9 can validate worst-frame call rate against per-frame RDTSC
 * dominance hypothesis.
 */
#ifdef XBOX
static uint64_t xemu_helper_rdtsc_calls;

void xemu_helper_rdtsc_inc(void);
void xemu_rdtsc_perf_emit_and_reset(FILE *out);

void xemu_helper_rdtsc_inc(void)
{
    qatomic_inc(&xemu_helper_rdtsc_calls);
}

void xemu_rdtsc_perf_emit_and_reset(FILE *out)
{
    if (out == NULL) {
        return;
    }
    uint64_t calls = qatomic_xchg(&xemu_helper_rdtsc_calls, 0);
    if (calls == 0) {
        return;
    }
    fprintf(out, " HELPER_RDTSC_CALLS=%llu", (unsigned long long)calls);
}
#endif

/*
 * Apple Silicon RDTSC fast-path. Bypasses the QEMU clock abstraction
 * entirely (avoids cpu_get_clock seqlock + cpu_get_clock_locked +
 * get_clock + clock_gettime + libsystem internals + mach_absolute_time
 * — a 7-9 function deep call chain that costs ~80-100 ns per RDTSC on
 * M3 Ultra).
 *
 * On Apple Silicon, mach_timebase_info is {1,1} (mach_absolute_time
 * returns nanoseconds directly); the cached check at startup verifies
 * this and falls back to the QEMU path if not. Cost in the common
 * case: one mach_absolute_time call (~10-15 ns) + muldiv64 (~3 ns) =
 * ~15-20 ns per RDTSC, vs ~80-100 ns through the QEMU clock path.
 *
 * Estimated ~70 % overhead reduction per call. Used to attribute
 * worst-frame stutter on titles whose Xbox kernel busy-waits on
 * RDTSC deadline checks (Crimson Skies, suspected); see
 * benchmarks/2026-05-02-v7-cumulative-phase-attribution.md
 * Mission 4 for the V8 sample-profile evidence.
 *
 * The vm_clock_offset (added by cpu_get_clock_locked when the VM is
 * paused) is omitted from the fast path. xemu does not pause/resume
 * the VM mid-execution, so this is safe in practice. The guest only
 * cares about TSC deltas (KeQueryPerformanceCounter), not absolute
 * value, so a small one-time offset at boot is invisible to the
 * guest.
 *
 * Gated on XEMU_FAST_RDTSC={0,1} env var; default ON for Apple
 * Silicon system builds. Set XEMU_FAST_RDTSC=0 to fall back to the
 * legacy qemu_clock_get_ns path (rollback for A/B testing or
 * correctness triage).
 */
#if defined(XBOX) && defined(__APPLE__)
static bool xemu_fast_rdtsc_enabled;
static uint64_t xemu_mach_timebase_numer = 1;
static uint64_t xemu_mach_timebase_denom = 1;
static bool xemu_fast_rdtsc_init_done;

static void xemu_fast_rdtsc_init(void)
{
    if (xemu_fast_rdtsc_init_done) {
        return;
    }

    /* Default ON for Apple Silicon system builds. */
    bool enabled = true;
    const char *env = getenv("XEMU_FAST_RDTSC");
    if (env && env[0]) {
        if (strcmp(env, "0") == 0) {
            enabled = false;
        } else if (strcmp(env, "1") == 0) {
            enabled = true;
        }
        /* Other values: silently keep default. */
    }

    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) == KERN_SUCCESS && tb.denom != 0) {
        xemu_mach_timebase_numer = tb.numer;
        xemu_mach_timebase_denom = tb.denom;
    } else {
        /* mach_timebase_info failed; force fallback. */
        enabled = false;
    }

    xemu_fast_rdtsc_enabled = enabled;
    xemu_fast_rdtsc_init_done = true;

    /* Log once at first call so the active path is visible in
     * benchmark logs. Format mirrors other XEMU_TCG_* / XEMU_APU_* /
     * XEMU_GL_* startup log lines. */
    fprintf(stderr,
            "xemu-perf: fast_rdtsc=%d source=%s tb_numer=%llu tb_denom=%llu\n",
            (int)xemu_fast_rdtsc_enabled,
            (env && env[0]) ? "env" : "auto-default",
            (unsigned long long)xemu_mach_timebase_numer,
            (unsigned long long)xemu_mach_timebase_denom);
}

static inline uint64_t xemu_fast_rdtsc_ns(void)
{
    uint64_t mach = mach_absolute_time();
    /* On M-series Apple Silicon, mach_timebase_info is {1, 1} so this
     * reduces to `return mach;` — the multiply/divide is constant-
     * folded. The full form preserves correctness on any host where
     * the timebase ratio is non-trivial. */
    if (xemu_mach_timebase_numer == 1 && xemu_mach_timebase_denom == 1) {
        return mach;
    }
    return mach * xemu_mach_timebase_numer / xemu_mach_timebase_denom;
}
#endif /* XBOX && __APPLE__ */

/* TSC handling */
uint64_t cpu_get_tsc(CPUX86State *env)
{
#ifdef XBOX
    xemu_helper_rdtsc_inc();
# if defined(__APPLE__)
    if (!xemu_fast_rdtsc_init_done) {
        xemu_fast_rdtsc_init();
    }
    if (xemu_fast_rdtsc_enabled) {
        return muldiv64(xemu_fast_rdtsc_ns(), 733333333,
                        NANOSECONDS_PER_SECOND);
    }
# endif
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 733333333,
                    NANOSECONDS_PER_SECOND);
#else
    return cpus_get_elapsed_ticks();
#endif
}

/* IRQ handling */
static void pic_irq_request(void *opaque, int irq, int level)
{
    CPUState *cs = first_cpu;
    X86CPU *cpu = X86_CPU(cs);

    trace_x86_pic_interrupt(irq, level);
    if (cpu_is_apic_enabled(cpu->apic_state) && !kvm_irqchip_in_kernel() &&
        !whpx_apic_in_platform()) {
        CPU_FOREACH(cs) {
            cpu = X86_CPU(cs);
            if (apic_accept_pic_intr(cpu->apic_state)) {
                apic_deliver_pic_intr(cpu->apic_state, level);
            }
        }
    } else {
        if (level) {
            cpu_interrupt(cs, CPU_INTERRUPT_HARD);
        } else {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        }
    }
}

qemu_irq x86_allocate_cpu_irq(void)
{
    return qemu_allocate_irq(pic_irq_request, NULL, 0);
}

int cpu_get_pic_interrupt(CPUX86State *env)
{
    X86CPU *cpu = env_archcpu(env);
    int intno;

    if (!kvm_irqchip_in_kernel() && !whpx_apic_in_platform()) {
        intno = apic_get_interrupt(cpu->apic_state);
        if (intno >= 0) {
            return intno;
        }
        /* read the irq from the PIC */
        if (!apic_accept_pic_intr(cpu->apic_state)) {
            return -1;
        }
    }

    intno = pic_read_irq(isa_pic);
    return intno;
}

APICCommonState *cpu_get_current_apic(void)
{
    if (current_cpu) {
        X86CPU *cpu = X86_CPU(current_cpu);
        return cpu->apic_state;
    } else {
        return NULL;
    }
}
