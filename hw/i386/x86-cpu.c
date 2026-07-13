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
#include "system/whpx.h"
#include "system/cpu-timers.h"
#include "trace.h"

#include "hw/i386/x86.h"
#include "target/i386/cpu.h"
#include "hw/intc/i8259.h"
#include "hw/irq.h"
#include "system/kvm.h"
#include "system/runstate.h"

/*
 * Ported perf fork (V9), Android/aarch64 variant: RDTSC fast-path.
 * Reads the ARM generic timer (CNTVCT_EL0) directly instead of going
 * through the QEMU clock abstraction (cpu_get_clock seqlock +
 * cpu_get_clock_locked + get_clock + clock_gettime), a deep call chain
 * that dominates RDTSC cost on titles whose Xbox kernel busy-waits on
 * RDTSC deadline checks (Crimson Skies, suspected).
 *
 * The counter is converted straight into the Xbox 733.33 MHz TSC domain.
 * The two-step conversion
 *   ns  = muldiv64(v, NANOSECONDS_PER_SECOND, cntfrq)
 *   tsc = muldiv64(ns, 733333333, NANOSECONDS_PER_SECOND)
 * is folded into a single muldiv64(v, 733333333, cntfrq) to avoid
 * double rounding.
 *
 * Semantics (accepted trade, same as the source fork): the raw generic
 * timer does not pause when the VM stops, so a vm_stop/vm_start pair
 * produces a one-time forward jump in the emulated TSC. xemu does not
 * pause/resume the VM mid-execution in normal use, and the guest only
 * cares about TSC deltas (KeQueryPerformanceCounter), not the absolute
 * value, so a small one-time offset at boot is invisible to the guest.
 *
 * Gated on XEMU_FAST_RDTSC; default ON for Android. Set
 * XEMU_FAST_RDTSC=0 to fall back to the legacy qemu_clock_get_ns path
 * (rollback for A/B testing or correctness triage).
 */
#if defined(XBOX) && defined(__ANDROID__) && defined(__aarch64__)
#include <android/log.h>

static bool xemu_fast_rdtsc_enabled;
static bool xemu_fast_rdtsc_init_done;
static uint64_t xemu_cntfrq;

/*
 * Android correction to the source fork's "VM never pauses mid-run"
 * assumption: on Quest/Android the app vm_stops on EVERY panel focus
 * loss, so an uncorrected raw counter would jump the guest TSC forward
 * by the full wall-clock pause (minutes/hours), which guest frame-delta
 * and network-timeout logic can observe. Track cumulative paused ticks
 * via a VM change-state handler and subtract them, restoring
 * QEMU_CLOCK_VIRTUAL pause semantics at the cost of one atomic load.
 * The handler runs in the main loop while vCPUs are quiesced on both
 * edges, so plain atomics are sufficient.
 */
static uint64_t xemu_tsc_pause_offset;  /* cntvct ticks spent paused */
static uint64_t xemu_tsc_paused_at;
static bool xemu_tsc_paused;

static inline uint64_t cntvct_now(void);

static void xemu_fast_rdtsc_vm_state_change(void *opaque, bool running,
                                            RunState state)
{
    if (!running && !xemu_tsc_paused) {
        xemu_tsc_paused_at = cntvct_now();
        xemu_tsc_paused = true;
    } else if (running && xemu_tsc_paused) {
        qatomic_set(&xemu_tsc_pause_offset,
                    xemu_tsc_pause_offset +
                        (cntvct_now() - xemu_tsc_paused_at));
        xemu_tsc_paused = false;
    }
}

static inline uint64_t cntvct_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t cntfrq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

static void xemu_fast_rdtsc_init(void)
{
    if (xemu_fast_rdtsc_init_done) {
        return;
    }

    /* Default ON for Android; only "0" disables. */
    bool enabled = true;
    const char *env = getenv("XEMU_FAST_RDTSC");
    if (env && env[0] && strcmp(env, "0") == 0) {
        enabled = false;
    }

    xemu_cntfrq = cntfrq();
    if (xemu_cntfrq == 0) {
        /* cntfrq_el0 unreadable/zero; force fallback to the QEMU path. */
        enabled = false;
    }

    if (enabled) {
        qemu_add_vm_change_state_handler(xemu_fast_rdtsc_vm_state_change,
                                         NULL);
    }
    xemu_fast_rdtsc_enabled = enabled;
    xemu_fast_rdtsc_init_done = true;

    __android_log_print(ANDROID_LOG_INFO, "xemu-perf",
                        "fast_rdtsc=%d source=%s cntfrq=%llu",
                        (int)xemu_fast_rdtsc_enabled,
                        (env && env[0]) ? "env" : "auto-default",
                        (unsigned long long)xemu_cntfrq);
}
#endif /* XBOX && __ANDROID__ && __aarch64__ */

/* TSC handling */
uint64_t cpu_get_tsc(CPUX86State *env)
{
#ifdef XBOX
# if defined(__ANDROID__) && defined(__aarch64__)
    if (!xemu_fast_rdtsc_init_done) {
        xemu_fast_rdtsc_init();
    }
    if (xemu_fast_rdtsc_enabled) {
        uint64_t v = cntvct_now() - qatomic_read(&xemu_tsc_pause_offset);
        return muldiv64(v, 733333333, xemu_cntfrq);
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
#if defined(XBOX) && defined(__ANDROID__) && defined(__aarch64__)
    /* Main-thread, board-init-time hook: resolve the fast-RDTSC gate and
     * register the pause-offset VM state handler here rather than lazily
     * from a vCPU thread (handler-list append assumes BQL). */
    xemu_fast_rdtsc_init();
#endif
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
