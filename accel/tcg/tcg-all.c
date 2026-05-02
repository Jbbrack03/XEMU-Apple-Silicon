/*
 * QEMU System Emulator, accelerator interfaces
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
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
#include "system/tcg.h"
#include "exec/replay-core.h"
#include "exec/icount.h"
#include "tcg/startup.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/accel.h"
#include "qemu/atomic.h"
#include "qapi/qapi-types-common.h"
#include "qapi/qapi-builtin-visit.h"
#include "qemu/units.h"
#include "qemu/target-info.h"
#ifndef CONFIG_USER_ONLY
#include "hw/boards.h"
#include "exec/tb-flush.h"
#include "system/runstate.h"
#endif
#include "accel/accel-ops.h"
#include "accel/accel-cpu-ops.h"
#include "accel/tcg/cpu-ops.h"
#include "qemu/xemu-spike-log.h"
#include "internal-common.h"


struct TCGState {
    AccelState parent_obj;

    OnOffAuto mttcg_enabled;
    bool one_insn_per_tb;
    int splitwx_enabled;
    unsigned long tb_size;
};
typedef struct TCGState TCGState;

#define TYPE_TCG_ACCEL ACCEL_CLASS_NAME("tcg")

DECLARE_INSTANCE_CHECKER(TCGState, TCG_STATE,
                         TYPE_TCG_ACCEL)

#ifndef CONFIG_USER_ONLY
bool qemu_tcg_mttcg_enabled(void)
{
    TCGState *s = TCG_STATE(current_accel());
    return s->mttcg_enabled == ON_OFF_AUTO_ON;
}
#endif /* !CONFIG_USER_ONLY */

/*
 * Apple Silicon performance fork: resolve the splitwx default.
 *
 * Upstream defaults splitwx to off in non-debug builds because the
 * MAP_JIT + per-TB pthread_jit_write_protect_np() path is correct
 * everywhere. On Apple Silicon that toggle is the most expensive
 * single operation on the TCG vCPU thread (measured 16% of on-CPU
 * time during the Crimson Skies 1.35-second worst-frame stutter on
 * 2026-05-01). The mach_vm_remap-based splitwx path
 * (`alloc_code_gen_buffer_splitwx_vmremap` in tcg/region.c) eliminates
 * that toggle entirely by maintaining two VA aliases (PROT_RX and
 * PROT_RW) of the same physical pages, so we default it on.
 *
 * Precedence: explicit `-accel tcg,split-wx=...` wins over the env
 * var, env var wins over the auto default. The env var is opt-in
 * tuning surface for benchmarks; the property remains the official
 * QEMU-style override.
 */
static int tcg_resolve_splitwx_default(void)
{
    const char *env = getenv("XEMU_TCG_SPLITWX");
    if (env && env[0]) {
        if (strcmp(env, "0") == 0) {
            return 0;
        }
        if (strcmp(env, "1") == 0) {
            return 1;
        }
        /* Anything else: ignore, fall through to compile-time default. */
    }

#if defined(CONFIG_DARWIN) && defined(__aarch64__) && !defined(CONFIG_USER_ONLY)
    /* Auto-on for Apple Silicon system emulation: try splitwx, fall
     * back to MAP_JIT if the dual mapping fails. */
    return -1;
#elif defined(CONFIG_DEBUG_TCG) && !defined(CONFIG_USER_ONLY)
    return -1;
#else
    return 0;
#endif
}

/*
 * Apple Silicon performance fork (I2): resolve XEMU_TCG_JMP_CACHE_TARGETED.
 *
 * Default-on for Apple Silicon system emulation: replace the
 * unconditional 4096-entry jmp-cache zero (CF_PCREL path in
 * tb_jmp_cache_inval_tb) with one bucket per invalidated TB inside an
 * SMC-driven burst. Correctness rests on the CF_INVALID + cflags-
 * equality check in cpu-exec.c::tb_lookup; see the comment on
 * tcg_jmp_cache_targeted_enabled in accel/tcg/tb-maint.c.
 *
 * Precedence: XEMU_TCG_JMP_CACHE_TARGETED=0 forces off (rollback for
 * A/B testing or correctness regression triage); =1 forces on
 * (covers builds where the auto-default is off); unset uses the
 * compile-time default (Apple Silicon system → on, others → off).
 *
 * The env var is consulted once at startup. There is no QOM property
 * for this slice (it is a fork-local steady-state perf gate, not a
 * user-facing knob like split-wx).
 */
extern bool tcg_jmp_cache_targeted_enabled;

static bool tcg_resolve_jmp_cache_targeted_default(void)
{
    const char *env = getenv("XEMU_TCG_JMP_CACHE_TARGETED");
    if (env && env[0]) {
        if (strcmp(env, "0") == 0) {
            return false;
        }
        if (strcmp(env, "1") == 0) {
            return true;
        }
        /* Anything else: ignore, fall through to compile-time default. */
    }

#if defined(CONFIG_DARWIN) && defined(__aarch64__) && !defined(CONFIG_USER_ONLY)
    return true;
#else
    return false;
#endif
}

static void tcg_accel_instance_init(Object *obj)
{
    TCGState *s = TCG_STATE(obj);

    s->splitwx_enabled = tcg_resolve_splitwx_default();
    tcg_jmp_cache_targeted_enabled = tcg_resolve_jmp_cache_targeted_default();

    /* V3: ensure the shared spike-log enables are read from the
     * environment before any TCG hot-path site checks them. This is
     * idempotent and runs once at TCG accel instance init (single-
     * threaded, before any vCPU thread starts). */
    xemu_spike_log_init();
}

bool one_insn_per_tb;

#ifndef CONFIG_USER_ONLY
static void tcg_vm_change_state(void *opaque, bool running, RunState state)
{
    if (state == RUN_STATE_RESTORE_VM) {
        /*
         * loadvm will update the content of RAM, bypassing the usual
         * mechanisms that ensure we flush TBs for writes to memory
         * we've translated code from, so we must flush all TBs.
         *
         * vm_stop() has just stopped all cpus, so we are exclusive.
         */
        assert(!running);
        tb_flush__exclusive_or_serial();
    }
}
#endif

static int tcg_init_machine(AccelState *as, MachineState *ms)
{
    TCGState *s = TCG_STATE(as);
    unsigned max_threads = 1;

#ifndef CONFIG_USER_ONLY
    CPUClass *cc = CPU_CLASS(object_class_by_name(target_cpu_type()));
    bool mttcg_supported = cc->tcg_ops->mttcg_supported;

    switch (s->mttcg_enabled) {
    case ON_OFF_AUTO_AUTO:
        /*
         * We default to false if we know other options have been enabled
         * which are currently incompatible with MTTCG. Otherwise when each
         * guest (target) has been updated to support:
         *   - atomic instructions
         *   - memory ordering primitives (barriers)
         * they can set the appropriate CONFIG flags in ${target}-softmmu.mak
         *
         * Once a guest architecture has been converted to the new primitives
         * there is one remaining limitation to check:
         *   - The guest can't be oversized (e.g. 64 bit guest on 32 bit host)
         */
        if (mttcg_supported && !icount_enabled()) {
            s->mttcg_enabled = ON_OFF_AUTO_ON;
            max_threads = ms->smp.max_cpus;
        } else {
            s->mttcg_enabled = ON_OFF_AUTO_OFF;
        }
        break;
    case ON_OFF_AUTO_ON:
        if (!mttcg_supported) {
            warn_report("Guest not yet converted to MTTCG - "
                        "you may get unexpected results");
        }
        max_threads = ms->smp.max_cpus;
        break;
    case ON_OFF_AUTO_OFF:
        break;
    default:
        g_assert_not_reached();
    }

    qemu_add_vm_change_state_handler(tcg_vm_change_state, NULL);
#endif

    tcg_allowed = true;

    page_init();
    tb_htable_init();
    tcg_init(s->tb_size * MiB, s->splitwx_enabled, max_threads);

#if defined(CONFIG_SOFTMMU)
    /*
     * There's no guest base to take into account, so go ahead and
     * initialize the prologue now.
     */
    tcg_prologue_init();
#endif

#ifdef CONFIG_USER_ONLY
    qdev_create_fake_machine();
#endif

    return 0;
}

static char *tcg_get_thread(Object *obj, Error **errp)
{
    TCGState *s = TCG_STATE(obj);

    return g_strdup(s->mttcg_enabled == ON_OFF_AUTO_ON ? "multi" : "single");
}

static void tcg_set_thread(Object *obj, const char *value, Error **errp)
{
    TCGState *s = TCG_STATE(obj);

    if (strcmp(value, "multi") == 0) {
        if (icount_enabled()) {
            error_setg(errp, "No MTTCG when icount is enabled");
        } else {
            s->mttcg_enabled = ON_OFF_AUTO_ON;
        }
    } else if (strcmp(value, "single") == 0) {
        s->mttcg_enabled = ON_OFF_AUTO_OFF;
    } else {
        error_setg(errp, "Invalid 'thread' setting %s", value);
    }
}

static void tcg_get_tb_size(Object *obj, Visitor *v,
                            const char *name, void *opaque,
                            Error **errp)
{
    TCGState *s = TCG_STATE(obj);
    uint32_t value = s->tb_size;

    visit_type_uint32(v, name, &value, errp);
}

static void tcg_set_tb_size(Object *obj, Visitor *v,
                            const char *name, void *opaque,
                            Error **errp)
{
    TCGState *s = TCG_STATE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    s->tb_size = value;
}

static bool tcg_get_splitwx(Object *obj, Error **errp)
{
    TCGState *s = TCG_STATE(obj);
    return s->splitwx_enabled;
}

static void tcg_set_splitwx(Object *obj, bool value, Error **errp)
{
    TCGState *s = TCG_STATE(obj);
    s->splitwx_enabled = value;
}

static bool tcg_get_one_insn_per_tb(Object *obj, Error **errp)
{
    TCGState *s = TCG_STATE(obj);
    return s->one_insn_per_tb;
}

static void tcg_set_one_insn_per_tb(Object *obj, bool value, Error **errp)
{
    TCGState *s = TCG_STATE(obj);
    s->one_insn_per_tb = value;
    /* Set the global also: this changes the behaviour */
    qatomic_set(&one_insn_per_tb, value);
}

static int tcg_gdbstub_supported_sstep_flags(AccelState *as)
{
    /*
     * In replay mode all events will come from the log and can't be
     * suppressed otherwise we would break determinism. However as those
     * events are tied to the number of executed instructions we won't see
     * them occurring every time we single step.
     */
    if (replay_mode != REPLAY_MODE_NONE) {
        return SSTEP_ENABLE;
    } else {
        return SSTEP_ENABLE | SSTEP_NOIRQ | SSTEP_NOTIMER;
    }
}

static void tcg_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "tcg";
    ac->init_machine = tcg_init_machine;
    ac->cpu_common_realize = tcg_exec_realizefn;
    ac->cpu_common_unrealize = tcg_exec_unrealizefn;
    ac->get_stats = tcg_get_stats;
    ac->allowed = &tcg_allowed;
    ac->gdbstub_supported_sstep_flags = tcg_gdbstub_supported_sstep_flags;

    object_class_property_add_str(oc, "thread",
                                  tcg_get_thread,
                                  tcg_set_thread);

    object_class_property_add(oc, "tb-size", "int",
        tcg_get_tb_size, tcg_set_tb_size,
        NULL, NULL);
    object_class_property_set_description(oc, "tb-size",
        "TCG translation block cache size");

    object_class_property_add_bool(oc, "split-wx",
        tcg_get_splitwx, tcg_set_splitwx);
    object_class_property_set_description(oc, "split-wx",
        "Map jit pages into separate RW and RX regions");

    object_class_property_add_bool(oc, "one-insn-per-tb",
                                   tcg_get_one_insn_per_tb,
                                   tcg_set_one_insn_per_tb);
    object_class_property_set_description(oc, "one-insn-per-tb",
        "Only put one guest insn in each translation block");
}

static const TypeInfo tcg_accel_type = {
    .name = TYPE_TCG_ACCEL,
    .parent = TYPE_ACCEL,
    .instance_init = tcg_accel_instance_init,
    .class_init = tcg_accel_class_init,
    .instance_size = sizeof(TCGState),
};
module_obj(TYPE_TCG_ACCEL);

static void register_accel_types(void)
{
    type_register_static(&tcg_accel_type);
}

type_init(register_accel_types);
