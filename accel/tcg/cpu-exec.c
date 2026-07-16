/*
 *  emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "qapi/type-helpers.h"
#include "hw/core/cpu.h"
#include "accel/tcg/cpu-ops.h"
#include "accel/tcg/helper-retaddr.h"
#include "trace.h"
#include "disas/disas.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/page-protection.h"
#include "exec/mmap-lock.h"
#include "exec/translation-block.h"
#include "tcg/tcg.h"
#include "qemu/atomic.h"
#include "qemu/rcu.h"
#include "exec/log.h"
#include "qemu/main-loop.h"
#include "exec/icount.h"
#include "exec/replay-core.h"
#include "system/tcg.h"
#include "exec/helper-proto-common.h"
#include "tcg-accel-ops.h"
#include "tb-jmp-cache.h"
#include "tb-hash.h"
#include "tb-code-hash.h"
#include "tb-context.h"
#include "tb-internal.h"
#include "internal-common.h"
#include "tb-cache-hints.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

/* ------------------------------------------------------------------ */
/*  Tier 1 promotion mechanism                                         */
/* ------------------------------------------------------------------ */

#ifdef XBOX

/*
 * Diagnostic-only exact TB edge capture for the multicore trace feasibility
 * study.  Direct chaining normally hides intermediate TBs from cpu_exec_loop,
 * so an active capture window forces CF_NO_GOTO_* and records one row per TB.
 * This intentionally changes performance while capturing and MUST NOT be used
 * for FPS measurements.  With XEMU_TB_TRACE unset there is one cold unlikely
 * check per outer dispatch and no allocation, clock read, or code-gen change.
 */
typedef struct QEMU_PACKED XemuTbTraceRecord {
    uint64_t pc;
    uint64_t phys_page;
    uint32_t flags;
    uint32_t cflags;
    uint16_t size;
    uint16_t icount;
    uint8_t exit;
    uint8_t tier;
    uint16_t host_size;
} XemuTbTraceRecord;

typedef struct QEMU_PACKED XemuTbTraceHeader {
    char magic[8];              /* XQ3TBTR1 */
    uint32_t header_size;
    uint32_t record_size;
    uint64_t record_count;
    uint64_t reserved;
} XemuTbTraceHeader;

/*
 * Optional sidecar for an offline translation-quality oracle.
 *
 * The compact XQ3TBTR1 stream deliberately contains just enough data for
 * control-flow analysis.  It does not retain cs_base or immutable guest code
 * bytes, so it cannot by itself reproduce a TCG translation.  When explicitly
 * requested, retain one deduplicated code snapshot for every executed TB
 * identity/version.  This is diagnostic-only and intentionally bounded; it is
 * never consulted by normal emulation.
 */
typedef struct QEMU_PACKED XemuTbTraceSnapshotHeader {
    char magic[8];              /* XQ3TBS1 */
    uint32_t header_size;
    uint32_t record_size;
    uint64_t record_count;
    uint64_t code_capacity;
} XemuTbTraceSnapshotHeader;

typedef struct QEMU_PACKED XemuTbTraceSnapshot {
    uint64_t pc;
    uint64_t phys_pc;
    uint64_t cs_base;
    uint64_t ihash;
    uint32_t flags;
    uint32_t trace_cflags;
    uint32_t normal_cflags;
    uint16_t guest_size;
    uint16_t guest_icount;
    uint16_t host_size;
    uint16_t code_len;
    uint8_t code[TARGET_PAGE_SIZE];
} XemuTbTraceSnapshot;

static int xemu_tb_trace_state = -1; /* -1 unknown, 0 waiting, 1 active, 2 done */
static int64_t xemu_tb_trace_deadline_ms;
static uint64_t xemu_tb_trace_poll;
static uint64_t xemu_tb_trace_count;
static uint64_t xemu_tb_trace_capacity;
static XemuTbTraceRecord *xemu_tb_trace_records;
static char *xemu_tb_trace_path;
static char *xemu_tb_trace_guest_dump_path;
static vaddr xemu_tb_trace_guest_dump_addr;
static size_t xemu_tb_trace_guest_dump_size = 4096;
static char *xemu_tb_trace_snapshot_path;
static uint64_t xemu_tb_trace_snapshot_capacity;
static uint64_t xemu_tb_trace_snapshot_skipped;
static GPtrArray *xemu_tb_trace_snapshots;
static GHashTable *xemu_tb_trace_snapshot_index;

static int xemu_vcpu_thread_id;

static guint xemu_tb_trace_snapshot_hash(gconstpointer opaque);
static gboolean xemu_tb_trace_snapshot_equal(gconstpointer a,
                                              gconstpointer b);

/* Read by the OpenXR shell through dlsym so it can identify the latency-
 * critical emulator thread to XR_KHR_android_thread_settings. */
__attribute__((visibility("default"))) int xemu_get_vcpu_thread_id(void)
{
    return qatomic_read(&xemu_vcpu_thread_id);
}

static void xemu_tb_trace_init(void)
{
    const char *guest_dump;
    const char *guest_addr;
    const char *guest_size;
    const char *snapshot;
    const char *snapshot_max;
    const char *path = getenv("XEMU_TB_TRACE");
    if (!path || !path[0]) {
        xemu_tb_trace_state = 2;
        return;
    }

    uint64_t capacity = 1000000;
    const char *max_env = getenv("XEMU_TB_TRACE_MAX");
    if (max_env && max_env[0]) {
        capacity = g_ascii_strtoull(max_env, NULL, 10);
    }
    capacity = MAX(UINT64_C(1024), MIN(capacity, UINT64_C(5000000)));

    int64_t delay_ms = 0;
    const char *delay_env = getenv("XEMU_TB_TRACE_DELAY_MS");
    if (delay_env && delay_env[0]) {
        delay_ms = g_ascii_strtoll(delay_env, NULL, 10);
    }
    delay_ms = MAX(INT64_C(0), MIN(delay_ms, INT64_C(600000)));

    xemu_tb_trace_records = g_try_new(XemuTbTraceRecord, capacity);
    if (!xemu_tb_trace_records) {
        error_report("tb-trace: allocation failed for %lu records",
                     (unsigned long)capacity);
        xemu_tb_trace_state = 2;
        return;
    }

    xemu_tb_trace_path = g_strdup(path);
    guest_dump = getenv("XEMU_TB_TRACE_GUEST_DUMP");
    guest_addr = getenv("XEMU_TB_TRACE_GUEST_ADDR");
    guest_size = getenv("XEMU_TB_TRACE_GUEST_SIZE");
    if (guest_dump && guest_dump[0] && guest_addr && guest_addr[0]) {
        char *end = NULL;
        uint64_t addr = g_ascii_strtoull(guest_addr, &end, 0);
        if (end && *end == '\0') {
            xemu_tb_trace_guest_dump_path = g_strdup(guest_dump);
            xemu_tb_trace_guest_dump_addr = addr;
            if (guest_size && guest_size[0]) {
                uint64_t size;

                end = NULL;
                size = g_ascii_strtoull(guest_size, &end, 0);
                if (end && *end == '\0') {
                    xemu_tb_trace_guest_dump_size = (size_t)
                        MIN(MAX(size, UINT64_C(4096)), UINT64_C(4194304));
                }
            }
        }
    }

    snapshot = getenv("XEMU_TB_TRACE_SNAPSHOT");
    if (snapshot && snapshot[0]) {
        uint64_t capacity = 16384;

        snapshot_max = getenv("XEMU_TB_TRACE_SNAPSHOT_MAX");
        if (snapshot_max && snapshot_max[0]) {
            capacity = g_ascii_strtoull(snapshot_max, NULL, 10);
        }
        xemu_tb_trace_snapshot_capacity =
            MAX(UINT64_C(1), MIN(capacity, UINT64_C(65536)));
        xemu_tb_trace_snapshot_path = g_strdup(snapshot);
        xemu_tb_trace_snapshots = g_ptr_array_new_with_free_func(g_free);
        xemu_tb_trace_snapshot_index =
            g_hash_table_new(xemu_tb_trace_snapshot_hash,
                             xemu_tb_trace_snapshot_equal);
        if (!xemu_tb_trace_snapshots || !xemu_tb_trace_snapshot_index) {
            error_report("tb-trace: snapshot allocation failed");
            g_clear_pointer(&xemu_tb_trace_snapshots, g_ptr_array_unref);
            g_clear_pointer(&xemu_tb_trace_snapshot_index,
                            g_hash_table_unref);
            g_clear_pointer(&xemu_tb_trace_snapshot_path, g_free);
        }
    }
    xemu_tb_trace_capacity = capacity;
    xemu_tb_trace_deadline_ms =
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + delay_ms;
    xemu_tb_trace_state = delay_ms ? 0 : 1;
    error_report("tb-trace: %s, max=%lu path=%s",
                 delay_ms ? "armed" : "active",
                 (unsigned long)capacity, path);
}

static guint xemu_tb_trace_snapshot_hash(gconstpointer opaque)
{
    const XemuTbTraceSnapshot *s = opaque;
    uint64_t mix = s->pc ^ s->phys_pc ^ s->cs_base ^ s->ihash;

    mix ^= ((uint64_t)s->flags << 32) | s->normal_cflags;
    mix ^= ((uint64_t)s->guest_size << 16) | s->guest_icount;
    return (guint)(mix ^ (mix >> 32));
}

static gboolean xemu_tb_trace_snapshot_equal(gconstpointer a,
                                              gconstpointer b)
{
    const XemuTbTraceSnapshot *left = a;
    const XemuTbTraceSnapshot *right = b;

    return left->pc == right->pc &&
           left->phys_pc == right->phys_pc &&
           left->cs_base == right->cs_base &&
           left->ihash == right->ihash &&
           left->flags == right->flags &&
           left->normal_cflags == right->normal_cflags &&
           left->guest_size == right->guest_size &&
           left->guest_icount == right->guest_icount;
}

static void xemu_tb_trace_snapshot(CPUState *cpu, vaddr pc,
                                   TranslationBlock *tb)
{
    XemuTbTraceSnapshot *snapshot;

    if (!xemu_tb_trace_snapshot_path || !xemu_tb_trace_snapshots ||
        !xemu_tb_trace_snapshot_index) {
        return;
    }
    if (xemu_tb_trace_snapshots->len >= xemu_tb_trace_snapshot_capacity) {
        xemu_tb_trace_snapshot_skipped++;
        return;
    }
    if (tb->size == 0 || tb->size > TARGET_PAGE_SIZE) {
        xemu_tb_trace_snapshot_skipped++;
        return;
    }

    snapshot = g_try_malloc0(sizeof(*snapshot));
    if (!snapshot) {
        xemu_tb_trace_snapshot_skipped++;
        return;
    }
    snapshot->pc = pc;
    snapshot->phys_pc = tb_page_addr0(tb);
    snapshot->cs_base = tb->cs_base;
    snapshot->ihash = tb->ihash;
    snapshot->flags = tb->flags;
    snapshot->trace_cflags = tb->cflags;
    /* Trace mode forces these two flags; omit them for a production replay. */
    snapshot->normal_cflags = tb->cflags &
        ~(CF_NO_GOTO_TB | CF_NO_GOTO_PTR | CF_INVALID |
          CF_TIER1 | CF_SUPERBLOCK);
    snapshot->guest_size = tb->size;
    snapshot->guest_icount = tb->icount;
    snapshot->host_size = MIN(tb->tc.size, UINT16_MAX);
    snapshot->code_len = tb->size;

    if (g_hash_table_contains(xemu_tb_trace_snapshot_index, snapshot)) {
        g_free(snapshot);
        return;
    }
    /*
     * Capture while this validated TB is executing.  Xbox page-wide code
     * invalidation means a write to this code page has already discarded the
     * TB, so these bytes correspond to the current version identified by
     * ihash.  cpu_memory_rw_debug also handles the rare virtual page crossing.
     */
    if (cpu_memory_rw_debug(cpu, pc, snapshot->code, snapshot->code_len,
                            false) != 0) {
        xemu_tb_trace_snapshot_skipped++;
        g_free(snapshot);
        return;
    }
    g_hash_table_add(xemu_tb_trace_snapshot_index, snapshot);
    g_ptr_array_add(xemu_tb_trace_snapshots, snapshot);
}

static void xemu_tb_trace_snapshot_finish(void)
{
    XemuTbTraceSnapshotHeader header = {
        .magic = { 'X', 'Q', '3', 'T', 'B', 'S', '1', '\0' },
        .header_size = sizeof(XemuTbTraceSnapshotHeader),
        .record_size = sizeof(XemuTbTraceSnapshot),
        .record_count = xemu_tb_trace_snapshots ?
                        xemu_tb_trace_snapshots->len : 0,
        .code_capacity = TARGET_PAGE_SIZE,
    };
    FILE *f;
    bool ok = true;

    if (!xemu_tb_trace_snapshot_path) {
        return;
    }
    f = fopen(xemu_tb_trace_snapshot_path, "wb");
    if (!f || fwrite(&header, sizeof(header), 1, f) != 1) {
        ok = false;
    }
    for (guint i = 0; ok && xemu_tb_trace_snapshots &&
                     i < xemu_tb_trace_snapshots->len; i++) {
        const XemuTbTraceSnapshot *snapshot =
            g_ptr_array_index(xemu_tb_trace_snapshots, i);
        ok = fwrite(snapshot, sizeof(*snapshot), 1, f) == 1;
    }
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    error_report("tb-trace: wrote %u snapshots to %s (%s, skipped=%lu)",
                 xemu_tb_trace_snapshots ? xemu_tb_trace_snapshots->len : 0,
                 xemu_tb_trace_snapshot_path, ok ? "ok" : "FAILED",
                 (unsigned long)xemu_tb_trace_snapshot_skipped);
    g_clear_pointer(&xemu_tb_trace_snapshot_index, g_hash_table_unref);
    g_clear_pointer(&xemu_tb_trace_snapshots, g_ptr_array_unref);
    g_clear_pointer(&xemu_tb_trace_snapshot_path, g_free);
}

static bool xemu_tb_trace_active(void)
{
    if (unlikely(xemu_tb_trace_state < 0)) {
        xemu_tb_trace_init();
    }
    if (likely(xemu_tb_trace_state != 0)) {
        return xemu_tb_trace_state == 1;
    }

    /* Avoid a clock syscall on every dispatch before the requested window. */
    if ((++xemu_tb_trace_poll & 0xfff) == 0 &&
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= xemu_tb_trace_deadline_ms) {
        xemu_tb_trace_state = 1;
        error_report("tb-trace: capture window active");
    }
    return xemu_tb_trace_state == 1;
}

static void xemu_tb_trace_finish(void)
{
    XemuTbTraceHeader header = {
        .magic = { 'X', 'Q', '3', 'T', 'B', 'T', 'R', '1' },
        .header_size = sizeof(XemuTbTraceHeader),
        .record_size = sizeof(XemuTbTraceRecord),
        .record_count = xemu_tb_trace_count,
    };
    FILE *f = fopen(xemu_tb_trace_path, "wb");
    bool ok = f && fwrite(&header, sizeof(header), 1, f) == 1 &&
              fwrite(xemu_tb_trace_records, sizeof(XemuTbTraceRecord),
                     xemu_tb_trace_count, f) == xemu_tb_trace_count;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    error_report("tb-trace: wrote %lu records to %s (%s)",
                 (unsigned long)xemu_tb_trace_count, xemu_tb_trace_path,
                 ok ? "ok" : "FAILED");
    xemu_tb_trace_snapshot_finish();
    g_clear_pointer(&xemu_tb_trace_records, g_free);
    xemu_tb_trace_state = 2;
}

static void xemu_tb_trace_dump_guest(CPUState *cpu)
{
    uint8_t *data;
    FILE *f;
    bool ok;

    if (!xemu_tb_trace_guest_dump_path) {
        return;
    }
    data = g_try_malloc(xemu_tb_trace_guest_dump_size);
    if (!data) {
        error_report("tb-trace: guest dump allocation failed");
        return;
    }
    if (cpu_memory_rw_debug(cpu, xemu_tb_trace_guest_dump_addr, data,
                            xemu_tb_trace_guest_dump_size, false) != 0) {
        error_report("tb-trace: guest dump read failed at 0x%" PRIx64,
                     (uint64_t)xemu_tb_trace_guest_dump_addr);
        g_free(data);
        return;
    }

    f = fopen(xemu_tb_trace_guest_dump_path, "wb");
    ok = f && fwrite(data, xemu_tb_trace_guest_dump_size, 1, f) == 1;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    error_report("tb-trace: guest dump 0x%" PRIx64 " to %s (%s)",
                 (uint64_t)xemu_tb_trace_guest_dump_addr,
                 xemu_tb_trace_guest_dump_path, ok ? "ok" : "FAILED");
    g_free(data);
}

static inline void xemu_tb_trace_record(CPUState *cpu, vaddr pc,
                                        TranslationBlock *tb,
                                        int tb_exit)
{
    XemuTbTraceRecord *r = &xemu_tb_trace_records[xemu_tb_trace_count++];
    r->pc = pc;
    r->phys_page = tb_page_addr0(tb);
    r->flags = tb->flags;
    r->cflags = tb->cflags;
    r->size = tb->size;
    r->icount = tb->icount;
    r->exit = tb_exit;
    r->tier = tb->tier;
    /* Existing v1 readers treated this trailing field as reserved. */
    r->host_size = MIN(tb->tc.size, UINT16_MAX);
    xemu_tb_trace_snapshot(cpu, pc, tb);
    if (unlikely(xemu_tb_trace_count == xemu_tb_trace_capacity)) {
        xemu_tb_trace_dump_guest(cpu);
        xemu_tb_trace_finish();
    }
}

#define TIER1_PROMOTION_BUDGET   32   /* Max promotions per budget window */
#define TIER1_BUDGET_INTERVAL_MS 10   /* Reset budget every N ms */

static int tier1_promotion_budget = TIER1_PROMOTION_BUDGET;

static int g_tier1_threshold = TB_TIER1_THRESHOLD;
static uint64_t g_tier1_promotions_total;
static uint64_t g_tier1_promotions_dropped;

void xemu_set_tier1_threshold(int value)
{
    if (value < 8) value = 8;
    if (value > 512) value = 512;
    g_tier1_threshold = value;
}

int xemu_get_tier1_threshold(void)
{
    return g_tier1_threshold;
}

void xemu_get_tier1_stats(uint64_t *promoted, uint64_t *dropped)
{
    if (promoted) *promoted = g_tier1_promotions_total;
    if (dropped) *dropped = g_tier1_promotions_dropped;
}

/*
 * Deferred tier-1 promotion request table.
 *
 * Calling tb_gen_code from within the post-execution handler is unsafe
 * (it breaks rendering).  Instead, promotion only invalidates the old
 * TB and records the request here.  The natural tb_gen_code path
 * (called from tb_find on the next cache miss) checks this table and
 * sets CF_TIER1 on the new TB so the tier-1 optimisation passes fire.
 */
#define TIER1_REQUEST_SLOTS 64

typedef struct {
    vaddr    pc;
    uint64_t cs_base;
    uint32_t flags;
    uint32_t exec_count;
    bool     valid;
} Tier1Request;

static Tier1Request tier1_requests[TIER1_REQUEST_SLOTS];

static inline bool tier1_enabled(void);

/*
 * Called from tb_gen_code (translate-all.c) to check whether a
 * freshly translated TB should use tier-1 optimisations.
 * Returns the saved exec_count if a request matches, or -1.
 */
/*
 * Peek: returns true if there is a pending tier-1 request for (pc,
 * cs_base, flags) without consuming it.
 */
bool tier1_has_pending_request(vaddr pc, uint64_t cs_base, uint32_t flags)
{
    if (!tier1_enabled()) {
        return false;
    }

    for (int i = 0; i < TIER1_REQUEST_SLOTS; i++) {
        if (tier1_requests[i].valid &&
            tier1_requests[i].pc == pc &&
            tier1_requests[i].cs_base == cs_base &&
            tier1_requests[i].flags == flags) {
            return true;
        }
    }
    return false;
}

static uint64_t g_tier1_consumed;  /* diagnostics: requests consumed */

int tier1_consume_request(vaddr pc, uint64_t cs_base, uint32_t flags,
                          uint32_t *cflags_out)
{
    if (!tier1_enabled()) {
        return -1;
    }

    for (int i = 0; i < TIER1_REQUEST_SLOTS; i++) {
        if (tier1_requests[i].valid &&
            tier1_requests[i].pc == pc &&
            tier1_requests[i].cs_base == cs_base &&
            tier1_requests[i].flags == flags) {
            tier1_requests[i].valid = false;
            if (cflags_out) {
                *cflags_out |= CF_TIER1;
            }
            g_tier1_consumed++;
            return (int)tier1_requests[i].exec_count;
        }
    }
    return -1;
}

static void tb_request_tier1_promotion(CPUState *cpu, TranslationBlock *tb,
                                       vaddr pc)
{
    /*
     * Record the request for deferred tier-1 retranslation.
     *
     * The key must be the REAL guest pc passed in from the exec loop
     * (s.pc), NOT tb->pc: x86 system-mode always sets CF_PCREL
     * (target/i386/cpu.c), and under CF_PCREL tb->pc is never written
     * (tb_gen_code skips it), so keying on tb->pc recorded garbage and
     * tier1_consume_request never matched — no tier-1 TB was ever
     * created, while the invalidation below still churned hot TBs.
     */
    int slot = -1;
    for (int i = 0; i < TIER1_REQUEST_SLOTS; i++) {
        if (!tier1_requests[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /*
         * Request table full: do NOT invalidate — the old code kept
         * invalidating the hot TB anyway, churning retranslation with
         * no promotion possible.
         */
        return;
    }
    tier1_requests[slot].pc         = pc;
    tier1_requests[slot].cs_base    = tb->cs_base;
    tier1_requests[slot].flags      = tb->flags;
    tier1_requests[slot].exec_count = tb->exec_count;
    tier1_requests[slot].valid      = true;

    /*
     * Invalidate the old TB.  Pass -1 so tb_phys_invalidate removes
     * it from the page list (standalone invalidation path).
     */
    mmap_lock();
    tb_phys_invalidate(tb, -1);
    mmap_unlock();
}

/*
 * Check if a TB should be promoted to Tier 1 and do so if budget allows.
 * Called from cpu_exec_loop after execution counting.
 */
/*
 * Runtime gate for tier-1 promotion (XEMU_TIER1=1 to enable, default
 * OFF). Historically promotion was always-on but silently broken: it
 * keyed requests on tb->pc, which is never written under CF_PCREL
 * (always set for system-mode x86), so requests never matched and no
 * tier-1 TB was ever created — promotion only invalidate-churned hot
 * TBs. Now that the keying is fixed the tier-1 codegen path actually
 * runs for the first time, so it must be opt-in until validated.
 * OFF means no promotions at all (less churn than the old behavior).
 */
static int g_tier1_enabled = -1;

static inline bool tier1_enabled(void)
{
    if (g_tier1_enabled < 0) {
        const char *env = getenv("XEMU_TIER1");
        g_tier1_enabled = (env && env[0] && strcmp(env, "0") != 0) ? 1 : 0;
    }
    return g_tier1_enabled != 0;
}

static inline void tier1_maybe_promote(CPUState *cpu, TranslationBlock *tb,
                                       vaddr pc)
{
    if (!tier1_enabled()) {
        return;
    }
    if (tb->tier == 0 && tb->exec_count >= (uint32_t)g_tier1_threshold) {
        if (tier1_promotion_budget > 0) {
            tier1_promotion_budget--;
            g_tier1_promotions_total++;
            tb_request_tier1_promotion(cpu, tb, pc);
        } else {
            g_tier1_promotions_dropped++;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Superblock detection                                               */
/* ------------------------------------------------------------------ */

/*
 * Superblock candidacy (adversarial-review hardened, 2026-07-13).
 *
 * The original chain_count dominance test was dead code: chain_count
 * increments only when tb_add_jump claims a previously-NULL jmp_dest
 * slot, i.e. once per relink cycle — it counts LINK events, not exit
 * traversals, so stable hot code never reached MIN_CHAINS=128.
 *
 * Replacement heuristic (observable at loop-surfacing time):
 *  - only exit slot 0. In this front-end slot 0 is the LAST-emitted
 *    exit (unconditional jumps, straight-line continuation TBs, and
 *    the TAKEN side of conditionals — see gen_conditional_jump_labels
 *    in target/i386/tcg/translate.c). The IR surgery reaches B by
 *    physical fall-through at the op-list tail, which is only correct
 *    when the removed exit is physically last. Merging on slot 1
 *    (fall-through-dominant conditionals) would misroute the hot path
 *    into the taken exit — silent wrong control flow. Never do it.
 *  - dominance proxy: if a slot-1 exit exists, it must never have been
 *    chained (jmp_dest[1] NULL ⇒ that path never surfaced in the loop).
 *    Dominance is a perf heuristic only — a mispredicted merge still
 *    executes correctly via the retained side-exit.
 *  - hotness: exec_count at its cap (counts loop surfacings, which are
 *    interrupt-sampled and thus biased toward genuinely hot blocks).
 */

/*
 * Forward-declare the superblock formation function (defined in
 * translate-all.c).  Returns the new superblock TB or NULL on failure.
 * pc_a/pc_b are the REAL guest pcs of A and B (tb->pc is unset under
 * CF_PCREL); both are re-validated against the TBs' physical identity
 * inside before anything destructive happens.
 */
TranslationBlock *tb_gen_superblock(CPUState *cpu,
                                     TranslationBlock *tb_a,
                                     int dominant_exit_a,
                                     int dominant_exit_b,
                                     vaddr pc_a, vaddr pc_b);

#define SUPERBLOCK_BUDGET 32  /* Max superblock formations per budget cycle */
static int superblock_budget = SUPERBLOCK_BUDGET;

/*
 * Runtime gate for superblock (2-block trace) formation.
 *
 * Experimental — default OFF. Set XEMU_SUPERBLOCK=1 (or any non-"0" value)
 * to enable; XEMU_SUPERBLOCK=0 or unset keeps it disabled. Consulted once
 * at first use and cached, so it is a predictable hot-path branch. Keeping
 * the formation body compiled (not #ifdef'd out) means the build always
 * validates it. A/B on device by launching with/without the env var.
 */
static int g_superblock_enabled = -1;

/*
 * Diagnostic targeted mode: form exactly one observed A->B pair without
 * enabling global tier-1 promotion/scanning.  This isolates the runtime value
 * of merged code from the previously measured global promotion tax.
 * Format: XEMU_SUPERBLOCK_TARGET=<pc-a>,<pc-b>[,<b-dominant-exit>].
 */
static int g_superblock_target_state = -1;
static vaddr g_superblock_target_a;
static vaddr g_superblock_target_b;
static int g_superblock_target_b_exit;
static TranslationBlock *g_superblock_target_tb;

static void __attribute__((noinline)) superblock_target_resolve(void)
{
    const char *env = getenv("XEMU_SUPERBLOCK_TARGET");
    char *end_a = NULL;
    char *end_b = NULL;
    char *end_exit = NULL;
    uint64_t a = 0;
    uint64_t b = 0;

    if (env && env[0]) {
        a = g_ascii_strtoull(env, &end_a, 0);
        if (end_a && *end_a == ',') {
            b = g_ascii_strtoull(end_a + 1, &end_b, 0);
        }
    }
    if (a && b && end_b && *end_b == ',') {
        uint64_t b_exit = g_ascii_strtoull(end_b + 1, &end_exit, 0);
        if (b_exit <= 1 && end_exit && *end_exit == '\0') {
            g_superblock_target_b_exit = b_exit;
        } else {
            a = 0;
        }
    }
    if (a && b && end_b &&
        (*end_b == '\0' || (end_exit && *end_exit == '\0'))) {
        g_superblock_target_a = a;
        g_superblock_target_b = b;
        g_superblock_target_state = 1;
        error_report("superblock: targeted A=0x%" PRIx64
                     " B=0x%" PRIx64 " B-exit=%d",
                     a, b, g_superblock_target_b_exit);
    } else {
        g_superblock_target_state = 0;
    }
}

static inline bool superblock_target_enabled(void)
{
    if (unlikely(g_superblock_target_state < 0)) {
        superblock_target_resolve();
    }
    return g_superblock_target_state != 0;
}

static inline bool superblock_enabled(void)
{
    if (g_superblock_enabled < 0) {
        const char *env = getenv("XEMU_SUPERBLOCK");
        g_superblock_enabled = (env && env[0] && strcmp(env, "0") != 0) ? 1 : 0;
    }
    return g_superblock_enabled != 0;
}

/*
 * Check if a Tier 1 TB is a superblock candidate and attempt formation.
 * Called from cpu_exec_loop after tier1 promotion, with budget rate limiting.
 */
/* Rejection-reason diagnostics, logged periodically when enabled. */
enum {
    SB_REJ_TIER, SB_REJ_HOT, SB_REJ_SLOT1, SB_REJ_BUDGET, SB_REJ_DEST,
    SB_REJ_B, SB_REJ_PAGE, SB_REJ_COPAGE, SB_REJ_ICOUNT,
    SB_FORMED, SB_FAILED, SB_STAT_MAX
};
static uint32_t sb_stats[SB_STAT_MAX];

static inline void tier1_maybe_form_superblock(CPUState *cpu,
                                                TranslationBlock *tb,
                                                vaddr pc)
{
    bool targeted = superblock_target_enabled();

    if (!superblock_enabled() && !targeted) {
        return;
    }

    if (targeted && pc != g_superblock_target_a) {
        return;
    }

    /* Global mode requires Tier 1; targeted mode deliberately isolates it. */
    if ((!targeted && tb->tier < 1) || tb->superblock != NULL) {
        sb_stats[SB_REJ_TIER]++;
        return;
    }
    if (tb->cflags & CF_SUPERBLOCK) {
        sb_stats[SB_REJ_TIER]++;
        return;
    }

    /* Hotness: exec_count must have reached its cap. */
    if (tb->exec_count < (uint32_t)g_tier1_threshold * 2) {
        sb_stats[SB_REJ_HOT]++;
        return;
    }

    /*
     * Slot 0 only (see block comment above). If a slot-1 exit exists
     * and has ever been chained, both paths are live — skip.
     */
    if (!targeted &&
        tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID &&
        qatomic_read(&tb->jmp_dest[1]) != (uintptr_t)NULL) {
        sb_stats[SB_REJ_SLOT1]++;
        return;
    }

    /* Check budget. */
    if (superblock_budget <= 0) {
        sb_stats[SB_REJ_BUDGET]++;
        return;
    }

    /* Verify successor exists and is valid. */
    uintptr_t dest = qatomic_read(&tb->jmp_dest[0]);
    if (dest == (uintptr_t)NULL || (dest & 1)) {
        sb_stats[SB_REJ_DEST]++;
        return;
    }
    TranslationBlock *tb_b = (TranslationBlock *)dest;
    if (tb_b->cflags & (CF_INVALID | CF_SUPERBLOCK)) {
        sb_stats[SB_REJ_B]++;
        return;
    }
    if (tb_b->superblock != NULL) {
        sb_stats[SB_REJ_B]++;
        return;
    }

    /* Both must be single-page TBs. */
    if (tb_page_addr1(tb) != -1 || tb_page_addr1(tb_b) != -1) {
        sb_stats[SB_REJ_PAGE]++;
        return;
    }

    /*
     * B's guest pc: tb->pc is unset under CF_PCREL, so use the pc
     * stamped at B's last dispatch (any chained-into TB was a dispatch
     * entry at chain time, so a live B is always stamped). 0 = never
     * dispatched — refuse. Also refuse virtual-page mismatch with A.
     */
    vaddr pc_b = targeted ? g_superblock_target_b : tb_b->entry_pc;
    if (pc_b == 0 || ((pc ^ pc_b) & TARGET_PAGE_MASK)) {
        sb_stats[SB_REJ_DEST]++;
        return;
    }

    /*
     * Co-page only: A and B must live on the same physical page.
     * A cross-page superblock records page_addr1 = B's page, which
     * tb_lookup_cmp validates as if it were the page virtually
     * following A (contiguous-TB assumption) — the merged block would
     * be unreachable AND every hot lookup of A would walk the guest
     * page tables for a page the guest never touched (spurious A-bit,
     * possible spurious #PF). Co-page keeps page_addr1 == -1 so the
     * normal lookup and SMC invalidation paths apply unchanged.
     */
    if ((tb_page_addr0(tb) & TARGET_PAGE_MASK) !=
        (tb_page_addr0(tb_b) & TARGET_PAGE_MASK)) {
        sb_stats[SB_REJ_COPAGE]++;
        return;
    }

    /*
     * Bound the merged instruction count: tcg_gen_code writes
     * gen_insn_end_off[0 .. icount-1], a fixed uint16_t[TCG_MAX_INSNS]
     * array. An unchecked a+b (each up to 512) overflows it, corrupting
     * TCGContext and the fault-unwind search data. Also refuse icount
     * of 0 — formation would fall back to TCG_MAX_INSNS per side.
     */
    if (tb->icount == 0 || tb_b->icount == 0 ||
        tb->icount + tb_b->icount > TCG_MAX_INSNS) {
        sb_stats[SB_REJ_ICOUNT]++;
        return;
    }

    superblock_budget--;
    mmap_lock();
    TranslationBlock *formed = tb_gen_superblock(
        cpu, tb, 0, targeted ? g_superblock_target_b_exit : 0, pc, pc_b);
    if (formed) {
        if (targeted) {
            g_superblock_target_tb = formed;
        }
        sb_stats[SB_FORMED]++;
    } else {
        sb_stats[SB_FAILED]++;
    }
    mmap_unlock();
}

/*
 * Reset the promotion budget periodically.  Called from cpu_exec_loop.
 * Uses a simple call counter rather than real time to avoid clock overhead.
 */
#define TIER1_BUDGET_RESET_INTERVAL 100000
static uint32_t tier1_budget_counter;

static uint32_t tier1_log_counter;
#define TIER1_LOG_INTERVAL 5  /* TEMP: 50→5 for superblock diagnosis */

static inline void tier1_maybe_reset_budget(void)
{
    if (++tier1_budget_counter >= TIER1_BUDGET_RESET_INTERVAL) {
        tier1_budget_counter = 0;
        tier1_promotion_budget = TIER1_PROMOTION_BUDGET;
        superblock_budget = SUPERBLOCK_BUDGET;
        if (++tier1_log_counter >= TIER1_LOG_INTERVAL) {
            tier1_log_counter = 0;
            qemu_printf("[tier1] threshold=%d promoted=%lu dropped=%lu\n",
                        g_tier1_threshold,
                        (unsigned long)g_tier1_promotions_total,
                        (unsigned long)g_tier1_promotions_dropped);
#if defined(__ANDROID__)
            if (superblock_enabled() || superblock_target_enabled() ||
                tier1_enabled()) {
                __android_log_print(ANDROID_LOG_INFO, "superblock",
                    "stats: promo=%lu drop=%lu consumed=%lu "
                    "tier=%u hot=%u slot1=%u budget=%u dest=%u "
                    "b=%u page=%u copage=%u icount=%u FORMED=%u failed=%u "
                    "target=%p off=%u/%u dest=%p/%p",
                    (unsigned long)g_tier1_promotions_total,
                    (unsigned long)g_tier1_promotions_dropped,
                    (unsigned long)g_tier1_consumed,
                    sb_stats[SB_REJ_TIER], sb_stats[SB_REJ_HOT],
                    sb_stats[SB_REJ_SLOT1], sb_stats[SB_REJ_BUDGET],
                    sb_stats[SB_REJ_DEST], sb_stats[SB_REJ_B],
                    sb_stats[SB_REJ_PAGE], sb_stats[SB_REJ_COPAGE],
                    sb_stats[SB_REJ_ICOUNT], sb_stats[SB_FORMED],
                    sb_stats[SB_FAILED], (void *)g_superblock_target_tb,
                    g_superblock_target_tb ?
                        g_superblock_target_tb->jmp_reset_offset[0] : 0,
                    g_superblock_target_tb ?
                        g_superblock_target_tb->jmp_reset_offset[1] : 0,
                    g_superblock_target_tb ? (void *)qatomic_read(
                        &g_superblock_target_tb->jmp_dest[0]) : NULL,
                    g_superblock_target_tb ? (void *)qatomic_read(
                        &g_superblock_target_tb->jmp_dest[1]) : NULL);
            }
#endif
        }
    }
}

#endif /* XBOX */

/* -icount align implementation. */

typedef struct SyncClocks {
    int64_t diff_clk;
    int64_t last_cpu_icount;
    int64_t realtime_clock;
} SyncClocks;

#if !defined(CONFIG_USER_ONLY)
/* Allow the guest to have a max 3ms advance.
 * The difference between the 2 clocks could therefore
 * oscillate around 0.
 */
#define VM_CLOCK_ADVANCE 3000000
#define THRESHOLD_REDUCE 1.5
#define MAX_DELAY_PRINT_RATE 2000000000LL
#define MAX_NB_PRINTS 100

int64_t max_delay;
int64_t max_advance;

static void align_clocks(SyncClocks *sc, CPUState *cpu)
{
    int64_t cpu_icount;

    if (!icount_align_option) {
        return;
    }

    cpu_icount = cpu->icount_extra + cpu->neg.icount_decr.u16.low;
    sc->diff_clk += icount_to_ns(sc->last_cpu_icount - cpu_icount);
    sc->last_cpu_icount = cpu_icount;

    if (sc->diff_clk > VM_CLOCK_ADVANCE) {
#ifndef _WIN32
        struct timespec sleep_delay, rem_delay;
        sleep_delay.tv_sec = sc->diff_clk / 1000000000LL;
        sleep_delay.tv_nsec = sc->diff_clk % 1000000000LL;
        if (nanosleep(&sleep_delay, &rem_delay) < 0) {
            sc->diff_clk = rem_delay.tv_sec * 1000000000LL + rem_delay.tv_nsec;
        } else {
            sc->diff_clk = 0;
        }
#else
        Sleep(sc->diff_clk / SCALE_MS);
        sc->diff_clk = 0;
#endif
    }
}

static void print_delay(const SyncClocks *sc)
{
    static float threshold_delay;
    static int64_t last_realtime_clock;
    static int nb_prints;

    if (icount_align_option &&
        sc->realtime_clock - last_realtime_clock >= MAX_DELAY_PRINT_RATE &&
        nb_prints < MAX_NB_PRINTS) {
        if ((-sc->diff_clk / (float)1000000000LL > threshold_delay) ||
            (-sc->diff_clk / (float)1000000000LL <
             (threshold_delay - THRESHOLD_REDUCE))) {
            threshold_delay = (-sc->diff_clk / 1000000000LL) + 1;
            qemu_printf("Warning: The guest is now late by %.1f to %.1f seconds\n",
                        threshold_delay - 1,
                        threshold_delay);
            nb_prints++;
            last_realtime_clock = sc->realtime_clock;
        }
    }
}

static void init_delay_params(SyncClocks *sc, CPUState *cpu)
{
    if (!icount_align_option) {
        return;
    }
    sc->realtime_clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    sc->diff_clk = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - sc->realtime_clock;
    sc->last_cpu_icount
        = cpu->icount_extra + cpu->neg.icount_decr.u16.low;
    if (sc->diff_clk < max_delay) {
        max_delay = sc->diff_clk;
    }
    if (sc->diff_clk > max_advance) {
        max_advance = sc->diff_clk;
    }

    /* Print every 2s max if the guest is late. We limit the number
       of printed messages to NB_PRINT_MAX(currently 100) */
    print_delay(sc);
}
#else
static void align_clocks(SyncClocks *sc, const CPUState *cpu)
{
}

static void init_delay_params(SyncClocks *sc, const CPUState *cpu)
{
}
#endif /* CONFIG USER ONLY */

struct tb_desc {
    TCGTBCPUState s;
    CPUArchState *env;
    tb_page_addr_t page_addr0;
};

static bool tb_lookup_cmp(const void *p, const void *d)
{
    const TranslationBlock *tb = p;
    const struct tb_desc *desc = d;

    if ((tb_cflags(tb) & CF_PCREL || tb->pc == desc->s.pc) &&
        tb_page_addr0(tb) == desc->page_addr0 &&
        tb->cs_base == desc->s.cs_base &&
        tb->flags == desc->s.flags &&
        (tb_cflags(tb) & ~CF_INVALID) == desc->s.cflags) {
        /* check next page if needed */
        tb_page_addr_t tb_phys_page1 = tb_page_addr1(tb);
        if (tb_phys_page1 == -1) {
            return true;
        } else {
            tb_page_addr_t phys_page1;
            vaddr virt_page1;

            /*
             * We know that the first page matched, and an otherwise valid TB
             * encountered an incomplete instruction at the end of that page,
             * therefore we know that generating a new TB from the current PC
             * must also require reading from the next page -- even if the
             * second pages do not match, and therefore the resulting insn
             * is different for the new TB.  Therefore any exception raised
             * here by the faulting lookup is not premature.
             */
            virt_page1 = TARGET_PAGE_ALIGN(desc->s.pc);
            phys_page1 = get_page_addr_code(desc->env, virt_page1);
            if (tb_phys_page1 == phys_page1) {
                return true;
            }
        }
    }
    return false;
}

static TranslationBlock *
tb_htable_lookup_common(CPUState *cpu, TCGTBCPUState s, const struct qht *ht,
                        qht_lookup_func_t func)
{
    tb_page_addr_t phys_pc;
    struct tb_desc desc;
    uint32_t h;

    desc.s = s;
    desc.env = cpu_env(cpu);
    phys_pc = get_page_addr_code(desc.env, s.pc);
    if (phys_pc == -1) {
        return NULL;
    }
    desc.page_addr0 = phys_pc;
    h = tb_hash_func(phys_pc, (s.cflags & CF_PCREL ? 0 : s.pc),
                     s.flags, s.cs_base, s.cflags);
    return qht_lookup_custom(ht, &desc, h, func);
}

static TranslationBlock *tb_htable_lookup(CPUState *cpu, TCGTBCPUState s)
{
    return tb_htable_lookup_common(cpu, s, &tb_ctx.htable, tb_lookup_cmp);
}

static bool inv_tb_lookup_cmp(const void *p, const void *d)
{
    const TranslationBlock *tb = p;
    const struct tb_desc *desc = d;

    return tb_lookup_cmp(p, d) &&
           tb->ihash == tb_code_hash_func(desc->env, desc->s.pc, tb->size);
}

TranslationBlock *inv_tb_htable_lookup(CPUState *cpu, TCGTBCPUState s)
{
    return tb_htable_lookup_common(cpu, s, &tb_ctx.inv_htable, inv_tb_lookup_cmp);
}

/**
 * tb_lookup:
 * @cpu: CPU that will execute the returned translation block
 * @pc: guest PC
 * @cs_base: arch-specific value associated with translation block
 * @flags: arch-specific translation block flags
 * @cflags: CF_* flags
 *
 * Look up a translation block inside the QHT using @pc, @cs_base, @flags and
 * @cflags. Uses @cpu's tb_jmp_cache. Might cause an exception, so have a
 * longjmp destination ready.
 *
 * Returns: an existing translation block or NULL.
 */
static inline TranslationBlock *tb_lookup(CPUState *cpu, TCGTBCPUState s)
{
    TranslationBlock *tb;
    CPUJumpCache *jc;
    uint32_t hash;

    /* we should never be trying to look up an INVALID tb */
    tcg_debug_assert(!(s.cflags & CF_INVALID));

    hash = tb_jmp_cache_hash_func(s.pc);
    jc = cpu->tb_jmp_cache;

    tb = qatomic_read(&jc->array[hash].tb);
    if (likely(tb &&
               jc->array[hash].pc == s.pc &&
               tb->cs_base == s.cs_base &&
               tb->flags == s.flags &&
               tb_cflags(tb) == s.cflags)) {
        goto hit;
    }

    tb = tb_htable_lookup(cpu, s);
    if (tb == NULL) {
        return NULL;
    }

    jc->array[hash].pc = s.pc;
    qatomic_set(&jc->array[hash].tb, tb);

hit:
    /*
     * As long as tb is not NULL, the contents are consistent.  Therefore,
     * the virtual PC has to match for non-CF_PCREL translations.
     */
    assert((tb_cflags(tb) & CF_PCREL) || tb->pc == s.pc);
    return tb;
}

static void log_cpu_exec(vaddr pc, CPUState *cpu,
                         const TranslationBlock *tb)
{
    if (qemu_log_in_addr_range(pc)) {
        qemu_log_mask(CPU_LOG_EXEC,
                      "Trace %d: %p [%08" PRIx64
                      "/%016" VADDR_PRIx "/%08x/%08x] %s\n",
                      cpu->cpu_index, tb->tc.ptr, tb->cs_base, pc,
                      tb->flags, tb->cflags, lookup_symbol(pc));

        if (qemu_loglevel_mask(CPU_LOG_TB_CPU)) {
            FILE *logfile = qemu_log_trylock();
            if (logfile) {
                int flags = CPU_DUMP_CCOP;

                if (qemu_loglevel_mask(CPU_LOG_TB_FPU)) {
                    flags |= CPU_DUMP_FPU;
                }
                if (qemu_loglevel_mask(CPU_LOG_TB_VPU)) {
                    flags |= CPU_DUMP_VPU;
                }
                cpu_dump_state(cpu, logfile, flags);
                qemu_log_unlock(logfile);
            }
        }
    }
}

static bool check_for_breakpoints_slow(CPUState *cpu, vaddr pc,
                                       uint32_t *cflags)
{
    CPUBreakpoint *bp;
    bool match_page = false;

    /*
     * Singlestep overrides breakpoints.
     * This requirement is visible in the record-replay tests, where
     * we would fail to make forward progress in reverse-continue.
     *
     * TODO: gdb singlestep should only override gdb breakpoints,
     * so that one could (gdb) singlestep into the guest kernel's
     * architectural breakpoint handler.
     */
    if (cpu->singlestep_enabled) {
        return false;
    }

    QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
        /*
         * If we have an exact pc match, trigger the breakpoint.
         * Otherwise, note matches within the page.
         */
        if (pc == bp->pc) {
            bool match_bp = false;

            if (bp->flags & BP_GDB) {
                match_bp = true;
            } else if (bp->flags & BP_CPU) {
#ifdef CONFIG_USER_ONLY
                g_assert_not_reached();
#else
                const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
                assert(tcg_ops->debug_check_breakpoint);
                match_bp = tcg_ops->debug_check_breakpoint(cpu);
#endif
            }

            if (match_bp) {
                cpu->exception_index = EXCP_DEBUG;
                return true;
            }
        } else if (((pc ^ bp->pc) & TARGET_PAGE_MASK) == 0) {
            match_page = true;
        }
    }

    /*
     * Within the same page as a breakpoint, single-step,
     * returning to helper_lookup_tb_ptr after each insn looking
     * for the actual breakpoint.
     *
     * TODO: Perhaps better to record all of the TBs associated
     * with a given virtual page that contains a breakpoint, and
     * then invalidate them when a new overlapping breakpoint is
     * set on the page.  Non-overlapping TBs would not be
     * invalidated, nor would any TB need to be invalidated as
     * breakpoints are removed.
     */
    if (match_page) {
        *cflags = (*cflags & ~CF_COUNT_MASK) | CF_NO_GOTO_TB | CF_BP_PAGE | 1;
    }
    return false;
}

static inline bool check_for_breakpoints(CPUState *cpu, vaddr pc,
                                         uint32_t *cflags)
{
    return unlikely(!QTAILQ_EMPTY(&cpu->breakpoints)) &&
        check_for_breakpoints_slow(cpu, pc, cflags);
}

/**
 * helper_lookup_tb_ptr: quick check for next tb
 * @env: current cpu state
 *
 * Look for an existing TB matching the current cpu state.
 * If found, return the code pointer.  If not found, return
 * the tcg epilogue so that we return into cpu_tb_exec.
 */
const void *HELPER(lookup_tb_ptr)(CPUArchState *env)
{
    CPUState *cpu = env_cpu(env);
    TranslationBlock *tb;

    /*
     * By definition we've just finished a TB, so I/O is OK.
     * Avoid the possibility of calling cpu_io_recompile() if
     * a page table walk triggered by tb_lookup() calling
     * probe_access_internal() happens to touch an MMIO device.
     * The next TB, if we chain to it, will clear the flag again.
     */
    cpu->neg.can_do_io = true;

    TCGTBCPUState s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
    s.cflags = curr_cflags(cpu);

    if (check_for_breakpoints(cpu, s.pc, &s.cflags)) {
        cpu_loop_exit(cpu);
    }

    tb = tb_lookup(cpu, s);
    if (tb == NULL) {
        return tcg_code_gen_epilogue;
    }

    if (qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC)) {
        log_cpu_exec(s.pc, cpu, tb);
    }

    return tb->tc.ptr;
}

/* Return the current PC from CPU, which may be cached in TB. */
static vaddr log_pc(CPUState *cpu, const TranslationBlock *tb)
{
    if (tb_cflags(tb) & CF_PCREL) {
        return cpu->cc->get_pc(cpu);
    } else {
        return tb->pc;
    }
}

/* Execute a TB, and fix up the CPU state afterwards if necessary */
/*
 * Disable CFI checks.
 * TCG creates binary blobs at runtime, with the transformed code.
 * A TB is a blob of binary code, created at runtime and called with an
 * indirect function call. Since such function did not exist at compile time,
 * the CFI runtime has no way to verify its signature and would fail.
 * TCG is not considered a security-sensitive part of QEMU so this does not
 * affect the impact of CFI in environment with high security requirements
 */
static inline TranslationBlock * QEMU_DISABLE_CFI
cpu_tb_exec(CPUState *cpu, TranslationBlock *itb, int *tb_exit)
{
    uintptr_t ret;
    TranslationBlock *last_tb;
    const void *tb_ptr = itb->tc.ptr;

    if (qemu_loglevel_mask(CPU_LOG_TB_CPU | CPU_LOG_EXEC)) {
        log_cpu_exec(log_pc(cpu, itb), cpu, itb);
    }

    qemu_thread_jit_execute();
    ret = tcg_qemu_tb_exec(cpu_env(cpu), tb_ptr);
    cpu->neg.can_do_io = true;
    qemu_plugin_disable_mem_helpers(cpu);
    /*
     * TODO: Delay swapping back to the read-write region of the TB
     * until we actually need to modify the TB.  The read-only copy,
     * coming from the rx region, shares the same host TLB entry as
     * the code that executed the exit_tb opcode that arrived here.
     * If we insist on touching both the RX and the RW pages, we
     * double the host TLB pressure.
     */
    last_tb = tcg_splitwx_to_rw((void *)(ret & ~TB_EXIT_MASK));
    *tb_exit = ret & TB_EXIT_MASK;

    trace_exec_tb_exit(last_tb, *tb_exit);

    if (*tb_exit > TB_EXIT_IDX1) {
        /* We didn't start executing this TB (eg because the instruction
         * counter hit zero); we must restore the guest PC to the address
         * of the start of the TB.
         */
        CPUClass *cc = cpu->cc;
        const TCGCPUOps *tcg_ops = cc->tcg_ops;

        if (tcg_ops->synchronize_from_tb) {
            tcg_ops->synchronize_from_tb(cpu, last_tb);
        } else {
            tcg_debug_assert(!(tb_cflags(last_tb) & CF_PCREL));
            assert(cc->set_pc);
            cc->set_pc(cpu, last_tb->pc);
        }
        if (qemu_loglevel_mask(CPU_LOG_EXEC)) {
            vaddr pc = log_pc(cpu, last_tb);
            if (qemu_log_in_addr_range(pc)) {
                qemu_log("Stopped execution of TB chain before %p [%016"
                         VADDR_PRIx "] %s\n",
                         last_tb->tc.ptr, pc, lookup_symbol(pc));
            }
        }
    }

    /*
     * If gdb single-step, and we haven't raised another exception,
     * raise a debug exception.  Single-step with another exception
     * is handled in cpu_handle_exception.
     */
    if (unlikely(cpu->singlestep_enabled) && cpu->exception_index == -1) {
        cpu->exception_index = EXCP_DEBUG;
        cpu_loop_exit(cpu);
    }

    return last_tb;
}


static void cpu_exec_enter(CPUState *cpu)
{
    const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;

    if (tcg_ops->cpu_exec_enter) {
        tcg_ops->cpu_exec_enter(cpu);
    }
}

static void cpu_exec_exit(CPUState *cpu)
{
    const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;

    if (tcg_ops->cpu_exec_exit) {
        tcg_ops->cpu_exec_exit(cpu);
    }
}

static void cpu_exec_longjmp_cleanup(CPUState *cpu)
{
    /* Non-buggy compilers preserve this; assert the correct value. */
    g_assert(cpu == current_cpu);

#ifdef CONFIG_USER_ONLY
    clear_helper_retaddr();
    if (have_mmap_lock()) {
        mmap_unlock();
    }
#else
    /*
     * For softmmu, a tlb_fill fault during translation will land here,
     * and we need to release any page locks held.  In system mode we
     * have one tcg_ctx per thread, so we know it was this cpu doing
     * the translation.
     *
     * Alternative 1: Install a cleanup to be called via an exception
     * handling safe longjmp.  It seems plausible that all our hosts
     * support such a thing.  We'd have to properly register unwind info
     * for the JIT for EH, rather that just for GDB.
     *
     * Alternative 2: Set and restore cpu->jmp_env in tb_gen_code to
     * capture the cpu_loop_exit longjmp, perform the cleanup, and
     * jump again to arrive here.
     */
    if (tcg_ctx->gen_tb) {
        tb_unlock_pages(tcg_ctx->gen_tb);
        tcg_ctx->gen_tb = NULL;
    }
#ifdef XBOX
    /*
     * A translation fault can longjmp out of tb_gen_code while the
     * hot-arena code-buffer swap is active; without this restore the
     * tiny hot arena would silently become the global code-gen buffer
     * (constant flush/retranslate thrash, overlapping host-PC ranges).
     */
    tcg_hot_swap_restore(tcg_ctx);
    tcg_ctx->superblock_append = false;
#endif
#endif
    if (bql_locked()) {
        bql_unlock();
    }
    assert_no_pages_locked();
}

void cpu_exec_step_atomic(CPUState *cpu)
{
    TranslationBlock *tb;
    int tb_exit;

    if (sigsetjmp(cpu->jmp_env, 0) == 0) {
        start_exclusive();
        g_assert(cpu == current_cpu);
        g_assert(!cpu->running);
        cpu->running = true;

        TCGTBCPUState s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
        s.cflags = curr_cflags(cpu);

        /* Execute in a serial context. */
        s.cflags &= ~CF_PARALLEL;
        /* After 1 insn, return and release the exclusive lock. */
        s.cflags |= CF_NO_GOTO_TB | CF_NO_GOTO_PTR | 1;
        /*
         * No need to check_for_breakpoints here.
         * We only arrive in cpu_exec_step_atomic after beginning execution
         * of an insn that includes an atomic operation we can't handle.
         * Any breakpoint for this insn will have been recognized earlier.
         */

        tb = tb_lookup(cpu, s);
        if (tb == NULL) {
            mmap_lock();
            tb = tb_gen_code(cpu, s);
            mmap_unlock();
        }

        cpu_exec_enter(cpu);
        /* execute the generated code */
        trace_exec_tb(tb, s.pc);
        cpu_tb_exec(cpu, tb, &tb_exit);
        cpu_exec_exit(cpu);
    } else {
        cpu_exec_longjmp_cleanup(cpu);
    }

    /*
     * As we start the exclusive region before codegen we must still
     * be in the region if we longjump out of either the codegen or
     * the execution.
     */
    g_assert(cpu_in_exclusive_context(cpu));
    cpu->running = false;
    end_exclusive();
}

void tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr)
{
    /*
     * Get the rx view of the structure, from which we find the
     * executable code address, and tb_target_set_jmp_target can
     * produce a pc-relative displacement to jmp_target_addr[n].
     */
    const TranslationBlock *c_tb = tcg_splitwx_to_rx(tb);
    uintptr_t offset = tb->jmp_insn_offset[n];
    uintptr_t jmp_rx = (uintptr_t)tb->tc.ptr + offset;
    uintptr_t jmp_rw = jmp_rx - tcg_splitwx_diff;

    if (tcg_use_indirect_chaining) {
        /*
         * The generated LDR reads this slot directly.  Publish the complete
         * pointer atomically before a subsequent chained execution can use
         * it.  This is the current equivalent of QEMU's pre-8.0 indirect
         * goto_tb mode, which updated only the data slot.
         */
        qatomic_store_release(&tb->jmp_target_addr[n], addr);
    } else {
        tb->jmp_target_addr[n] = addr;
    }
    tb_target_set_jmp_target(c_tb, n, jmp_rx, jmp_rw);
}

static inline void tb_add_jump(TranslationBlock *tb, int n,
                               TranslationBlock *tb_next)
{
    uintptr_t old;

    qemu_thread_jit_write();
    assert(n < ARRAY_SIZE(tb->jmp_list_next));
    qemu_spin_lock(&tb_next->jmp_lock);

    /* make sure the destination TB is valid */
    if (tb_next->cflags & CF_INVALID) {
        goto out_unlock_next;
    }
    /* Atomically claim the jump destination slot only if it was NULL */
    old = qatomic_cmpxchg(&tb->jmp_dest[n], (uintptr_t)NULL,
                          (uintptr_t)tb_next);
    if (old) {
        goto out_unlock_next;
    }

    /* patch the native jump address */
    tb_set_jmp_target(tb, n, (uintptr_t)tb_next->tc.ptr);

    /* add in TB jmp list */
    tb->jmp_list_next[n] = tb_next->jmp_list_head;
    tb_next->jmp_list_head = (uintptr_t)tb | n;

#ifdef XBOX
    {
        uint32_t cnt = tb->chain_count[n];
        if (cnt < UINT32_MAX) {
            tb->chain_count[n] = cnt + 1;
        }
    }
#endif

    qemu_spin_unlock(&tb_next->jmp_lock);

    qemu_log_mask(CPU_LOG_EXEC, "Linking TBs %p index %d -> %p\n",
                  tb->tc.ptr, n, tb_next->tc.ptr);
    return;

 out_unlock_next:
    qemu_spin_unlock(&tb_next->jmp_lock);
}

static inline bool cpu_handle_halt(CPUState *cpu)
{
#ifndef CONFIG_USER_ONLY
    if (cpu->halted) {
        const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
        bool leave_halt = tcg_ops->cpu_exec_halt(cpu);

        if (!leave_halt) {
            return true;
        }

        cpu->halted = 0;
    }
#endif /* !CONFIG_USER_ONLY */

    return false;
}

static inline void cpu_handle_debug_exception(CPUState *cpu)
{
    const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
    CPUWatchpoint *wp;

    if (!cpu->watchpoint_hit) {
        QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
            wp->flags &= ~BP_WATCHPOINT_HIT;
        }
    }

    if (tcg_ops->debug_excp_handler) {
        tcg_ops->debug_excp_handler(cpu);
    }
}

static inline bool cpu_handle_exception(CPUState *cpu, int *ret)
{
    if (cpu->exception_index < 0) {
#ifndef CONFIG_USER_ONLY
        if (replay_has_exception()
            && cpu->neg.icount_decr.u16.low + cpu->icount_extra == 0) {
            /* Execute just one insn to trigger exception pending in the log */
            cpu->cflags_next_tb = (curr_cflags(cpu) & ~CF_USE_ICOUNT)
                | CF_NOIRQ | 1;
        }
#endif
        return false;
    }

    if (cpu->exception_index >= EXCP_INTERRUPT) {
        /* exit request from the cpu execution loop */
        *ret = cpu->exception_index;
        if (*ret == EXCP_DEBUG) {
            cpu_handle_debug_exception(cpu);
        }
        cpu->exception_index = -1;
        return true;
    }

#if defined(CONFIG_USER_ONLY)
    /*
     * If user mode only, we simulate a fake exception which will be
     * handled outside the cpu execution loop.
     */
    const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
    if (tcg_ops->fake_user_interrupt) {
        tcg_ops->fake_user_interrupt(cpu);
    }
    *ret = cpu->exception_index;
    cpu->exception_index = -1;
    return true;
#else
    if (replay_exception()) {
        const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;

        bql_lock();
        tcg_ops->do_interrupt(cpu);
        bql_unlock();
        cpu->exception_index = -1;

        if (unlikely(cpu->singlestep_enabled)) {
            /*
             * After processing the exception, ensure an EXCP_DEBUG is
             * raised when single-stepping so that GDB doesn't miss the
             * next instruction.
             */
            *ret = EXCP_DEBUG;
            cpu_handle_debug_exception(cpu);
            return true;
        }
    } else if (!replay_has_interrupt()) {
        /* give a chance to iothread in replay mode */
        *ret = EXCP_INTERRUPT;
        return true;
    }
#endif

    return false;
}

void tcg_kick_vcpu_thread(CPUState *cpu)
{
#ifndef CONFIG_USER_ONLY
    /*
     * Ensure cpu_exec will see the reason why the exit request was set.
     * FIXME: this is not always needed.  Other accelerators instead
     * read interrupt_request and set exit_request on demand from the
     * CPU thread; see kvm_arch_pre_run() for example.
     */
    qatomic_store_release(&cpu->exit_request, true);
#endif

    /* Ensure cpu_exec will see the exit request after TCG has exited.  */
    qatomic_store_release(&cpu->neg.icount_decr.u16.high, -1);
}

static inline bool icount_exit_request(CPUState *cpu)
{
    if (!icount_enabled()) {
        return false;
    }
    if (cpu->cflags_next_tb != -1 && !(cpu->cflags_next_tb & CF_USE_ICOUNT)) {
        return false;
    }
    return cpu->neg.icount_decr.u16.low + cpu->icount_extra == 0;
}

static inline bool cpu_handle_interrupt(CPUState *cpu,
                                        TranslationBlock **last_tb)
{
    /*
     * If we have requested custom cflags with CF_NOIRQ we should
     * skip checking here. Any pending interrupts will get picked up
     * by the next TB we execute under normal cflags.
     */
    if (cpu->cflags_next_tb != -1 && cpu->cflags_next_tb & CF_NOIRQ) {
        return false;
    }

    /* Clear the interrupt flag now since we're processing
     * cpu->interrupt_request and cpu->exit_request.
     * Ensure zeroing happens before reading cpu->exit_request or
     * cpu->interrupt_request (see also store-release in
     * tcg_kick_vcpu_thread())
     */
    qatomic_set_mb(&cpu->neg.icount_decr.u16.high, 0);

#ifdef CONFIG_USER_ONLY
    assert(!cpu_test_interrupt(cpu, ~0));
#else
    if (unlikely(cpu_test_interrupt(cpu, ~0))) {
        bql_lock();
        if (cpu_test_interrupt(cpu, CPU_INTERRUPT_DEBUG)) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_DEBUG);
            cpu->exception_index = EXCP_DEBUG;
            bql_unlock();
            return true;
        }
        if (replay_mode == REPLAY_MODE_PLAY && !replay_has_interrupt()) {
            /* Do nothing */
        } else if (cpu_test_interrupt(cpu, CPU_INTERRUPT_HALT)) {
            replay_interrupt();
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_HALT);
            cpu->halted = 1;
            cpu->exception_index = EXCP_HLT;
            bql_unlock();
            return true;
        } else {
            const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
            int interrupt_request = cpu->interrupt_request;

            if (cpu_test_interrupt(cpu, CPU_INTERRUPT_RESET)) {
                replay_interrupt();
                tcg_ops->cpu_exec_reset(cpu);
                bql_unlock();
                return true;
            }

            if (unlikely(cpu->singlestep_enabled & SSTEP_NOIRQ)) {
                /* Mask out external interrupts for this step. */
                interrupt_request &= ~CPU_INTERRUPT_SSTEP_MASK;
            }

            /*
             * The target hook has 3 exit conditions:
             * False when the interrupt isn't processed,
             * True when it is, and we should restart on a new TB,
             * and via longjmp via cpu_loop_exit.
             */
            if (tcg_ops->cpu_exec_interrupt(cpu, interrupt_request)) {
                if (!tcg_ops->need_replay_interrupt ||
                    tcg_ops->need_replay_interrupt(interrupt_request)) {
                    replay_interrupt();
                }
                /*
                 * After processing the interrupt, ensure an EXCP_DEBUG is
                 * raised when single-stepping so that GDB doesn't miss the
                 * next instruction.
                 */
                if (unlikely(cpu->singlestep_enabled)) {
                    cpu->exception_index = EXCP_DEBUG;
                    bql_unlock();
                    return true;
                }
                cpu->exception_index = -1;
                *last_tb = NULL;
            }
        }
        if (cpu_test_interrupt(cpu, CPU_INTERRUPT_EXITTB)) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_EXITTB);
            /* ensure that no TB jump will be modified as
               the program flow was changed */
            *last_tb = NULL;
        }

        /* If we exit via cpu_loop_exit/longjmp it is reset in cpu_exec */
        bql_unlock();
    }
#endif /* !CONFIG_USER_ONLY */

    /*
     * Finally, check if we need to exit to the main loop.
     * The corresponding store-release is in cpu_exit.
     */
    if (unlikely(qatomic_load_acquire(&cpu->exit_request)) || icount_exit_request(cpu)) {
        if (cpu->exception_index == -1) {
            cpu->exception_index = EXCP_INTERRUPT;
        }
        return true;
    }

    return false;
}

static inline void cpu_loop_exec_tb(CPUState *cpu, TranslationBlock *tb,
                                    vaddr pc, TranslationBlock **last_tb,
                                    int *tb_exit)
{
    trace_exec_tb(tb, pc);
    tb = cpu_tb_exec(cpu, tb, tb_exit);
    if (*tb_exit != TB_EXIT_REQUESTED) {
        *last_tb = tb;
        return;
    }

    *last_tb = NULL;
    if (cpu_loop_exit_requested(cpu)) {
        /* Something asked us to stop executing chained TBs; just
         * continue round the main loop. Whatever requested the exit
         * will also have set something else (eg exit_request or
         * interrupt_request) which will be handled by
         * cpu_handle_interrupt.  cpu_handle_interrupt will also
         * clear cpu->icount_decr.u16.high.
         */
        return;
    }

    /* Instruction counter expired.  */
    assert(icount_enabled());
#ifndef CONFIG_USER_ONLY
    /* Ensure global icount has gone forward */
    icount_update(cpu);
    /* Refill decrementer and continue execution.  */
    int32_t insns_left = MIN(0xffff, cpu->icount_budget);
    cpu->neg.icount_decr.u16.low = insns_left;
    cpu->icount_extra = cpu->icount_budget - insns_left;

    /*
     * If the next tb has more instructions than we have left to
     * execute we need to ensure we find/generate a TB with exactly
     * insns_left instructions in it.
     */
    if (insns_left > 0 && insns_left < tb->icount)  {
        assert(insns_left <= CF_COUNT_MASK);
        assert(cpu->icount_extra == 0);
        cpu->cflags_next_tb = (tb->cflags & ~CF_COUNT_MASK) | insns_left;
    }
#endif
}

/* main execution loop */

static int __attribute__((noinline))
cpu_exec_loop(CPUState *cpu, SyncClocks *sc)
{
    int ret;

    /* if an exception is pending, we execute it here */
    while (!cpu_handle_exception(cpu, &ret)) {
        TranslationBlock *last_tb = NULL;
        int tb_exit = 0;

        while (!cpu_handle_interrupt(cpu, &last_tb)) {
            TranslationBlock *tb;
            TCGTBCPUState s = cpu->cc->tcg_ops->get_tb_cpu_state(cpu);
            s.cflags = cpu->cflags_next_tb;

            /*
             * When requested, use an exact setting for cflags for the next
             * execution.  This is used for icount, precise smc, and stop-
             * after-access watchpoints.  Since this request should never
             * have CF_INVALID set, -1 is a convenient invalid value that
             * does not require tcg headers for cpu_common_reset.
             */
            if (s.cflags == -1) {
                s.cflags = curr_cflags(cpu);
            } else {
                cpu->cflags_next_tb = -1;
            }

#ifdef XBOX
            bool tb_trace_this = unlikely(xemu_tb_trace_active());
            if (tb_trace_this) {
                /* Exact diagnostic edges: make this dispatch execute one TB. */
                s.cflags |= CF_NO_GOTO_TB | CF_NO_GOTO_PTR;
                last_tb = NULL;
            }
#endif

            if (check_for_breakpoints(cpu, s.pc, &s.cflags)) {
                break;
            }

            tb = tb_lookup(cpu, s);
            if (tb == NULL) {
                CPUJumpCache *jc;
                uint32_t h;

                tb_cache_notify_lookup_miss();
                tb_cache_maybe_log_stats();

                mmap_lock();
                tb = tb_gen_code(cpu, s);
                mmap_unlock();

                /*
                 * We add the TB in the virtual pc hash table
                 * for the fast lookup
                 */
                h = tb_jmp_cache_hash_func(s.pc);
                jc = cpu->tb_jmp_cache;
                jc->array[h].pc = s.pc;
                qatomic_set(&jc->array[h].tb, tb);
            } else {
                tb_cache_notify_lookup_hit();
            }

#ifndef CONFIG_USER_ONLY
            /*
             * We don't take care of direct jumps when address mapping
             * changes in system emulation.  So it's not safe to make a
             * direct jump to a TB spanning two pages because the mapping
             * for the second page can change.
             */
            if (tb_page_addr1(tb) != -1) {
                last_tb = NULL;
            }
#endif
            /* See if we can patch the calling TB. */
            if (last_tb) {
                tb_add_jump(last_tb, tb_exit, tb);
            }


#ifdef XBOX
            {
                static uint64_t cpu_heartbeat = 0;
                if (unlikely(cpu_heartbeat < 20)) {
                    cpu_heartbeat++;
                    error_report("[CPU-PRE]  tb#%lu pc=0x%lx size=%d",
                                 (unsigned long)cpu_heartbeat,
                                 (unsigned long)s.pc, tb->size);

                    cpu_loop_exec_tb(cpu, tb, s.pc, &last_tb, &tb_exit);

                    error_report("[CPU-POST] tb#%lu exit=%d last_tb=%p",
                                 (unsigned long)cpu_heartbeat,
                                 tb_exit, last_tb);
                } else {
                    cpu_loop_exec_tb(cpu, tb, s.pc, &last_tb, &tb_exit);
                }
            }
#else
            cpu_loop_exec_tb(cpu, tb, s.pc, &last_tb, &tb_exit);
#endif

#ifdef XBOX
            if (unlikely(tb_trace_this)) {
                xemu_tb_trace_record(cpu, s.pc, tb, tb_exit);
                last_tb = NULL;
            }
            /*
             * Tier-1/superblock bookkeeping only when the machinery is
             * live: with both flags off the stores below (exec_count,
             * entry_pc) would dirty the TB cacheline on every loop
             * surfacing for nothing.
             */
            if (unlikely(tier1_enabled() || superblock_enabled() ||
                         superblock_target_enabled())) {
#if defined(__ANDROID__)
                if (unlikely(tb->superblock && superblock_target_enabled())) {
                    static unsigned target_exec_logs;
                    if (target_exec_logs < 12) {
                        __android_log_print(ANDROID_LOG_INFO, "superblock",
                            "target-exec pc=0x%" PRIx64 " exit=%d last=%p "
                            "tb=%p off=%u/%u dest=%p/%p",
                            (uint64_t)s.pc, tb_exit, last_tb, tb,
                            tb->jmp_reset_offset[0], tb->jmp_reset_offset[1],
                            (void *)qatomic_read(&tb->jmp_dest[0]),
                            (void *)qatomic_read(&tb->jmp_dest[1]));
                        target_exec_logs++;
                    }
                }
#endif
                uint32_t c = tb->exec_count;
                if (c < (uint32_t)g_tier1_threshold * 2) {
                    tb->exec_count = c + 1;
                }
                /* Stamp the real guest pc (tb->pc is unset under CF_PCREL). */
                tb->entry_pc = s.pc;
                tier1_maybe_promote(cpu, tb, s.pc);
                tier1_maybe_form_superblock(cpu, tb, s.pc);
                tier1_maybe_reset_budget();
            }
            /*
             * Unconditional: promotion invalidates TBs mid-loop, but SMC
             * can too — never chain into a CF_INVALID TB.
             */
            if (tb->cflags & CF_INVALID) {
                last_tb = NULL;
            }
#endif

            /* Try to align the host and virtual clocks
               if the guest is in advance */
            align_clocks(sc, cpu);
        }
    }
    return ret;
}

static int cpu_exec_setjmp(CPUState *cpu, SyncClocks *sc)
{
    /* Prepare setjmp context for exception handling. */
    if (unlikely(sigsetjmp(cpu->jmp_env, 0) != 0)) {
        cpu_exec_longjmp_cleanup(cpu);
    }

    return cpu_exec_loop(cpu, sc);
}

int cpu_exec(CPUState *cpu)
{
    int ret;
    SyncClocks sc = { 0 };

#ifdef XBOX
    if (unlikely(qatomic_read(&xemu_vcpu_thread_id) == 0)) {
        qatomic_set(&xemu_vcpu_thread_id, qemu_get_thread_id());
    }
    static bool tb_cache_warmed = false;
    if (!tb_cache_warmed) {
        tb_cache_warmed = true;
        tb_cache_prewarm(cpu);
    }
#endif

    /* replay_interrupt may need current_cpu */
    current_cpu = cpu;

    if (cpu_handle_halt(cpu)) {
        return EXCP_HALTED;
    }

    RCU_READ_LOCK_GUARD();
    cpu_exec_enter(cpu);

    /*
     * Calculate difference between guest clock and host clock.
     * This delay includes the delay of the last cycle, so
     * what we have to do is sleep until it is 0. As for the
     * advance/delay we gain here, we try to fix it next time.
     */
    init_delay_params(&sc, cpu);

    ret = cpu_exec_setjmp(cpu, &sc);

    cpu_exec_exit(cpu);
    return ret;
}

/*
 * Jump-cache geometry, latched once before the first vCPU's cache is
 * allocated (single vCPU on Xbox). XEMU_JMP_CACHE_BITS overrides the
 * upstream default of TB_JMP_CACHE_BITS (12); clamped so that
 * page_bits never exceeds TARGET_PAGE_BITS (hash shift stays positive).
 */
XemuJmpCacheGeom xemu_jc_geom = {
    .bits      = TB_JMP_CACHE_BITS,
    .page_bits = TB_JMP_CACHE_BITS / 2,
    .size      = 1u << TB_JMP_CACHE_BITS,
    .page_size = 1u << (TB_JMP_CACHE_BITS / 2),
    .addr_mask = (1u << (TB_JMP_CACHE_BITS / 2)) - 1,
    .page_mask = (1u << TB_JMP_CACHE_BITS) - (1u << (TB_JMP_CACHE_BITS / 2)),
    .fixed_default_hash = true,
};

static void xemu_jc_geom_latch(void)
{
    static bool latched;
    unsigned bits = TB_JMP_CACHE_BITS;
    const char *env;
    const char *fixed_hash_env;

    if (latched) {
        return;
    }
    latched = true;

    env = getenv("XEMU_JMP_CACHE_BITS");
    if (env && env[0]) {
        long v = strtol(env, NULL, 10);
        if (v >= 10 && v <= 18) {
            bits = (unsigned)v;
        }
    }
    xemu_jc_geom.bits      = bits;
    xemu_jc_geom.page_bits = bits / 2;
    xemu_jc_geom.size      = 1u << bits;
    xemu_jc_geom.page_size = 1u << (bits / 2);
    xemu_jc_geom.addr_mask = (1u << (bits / 2)) - 1;
    xemu_jc_geom.page_mask = (1u << bits) - (1u << (bits / 2));
    fixed_hash_env = getenv("XEMU_JMP_CACHE_CONST_HASH");
    xemu_jc_geom.fixed_default_hash =
        bits == TB_JMP_CACHE_BITS &&
        !(fixed_hash_env && strcmp(fixed_hash_env, "0") == 0);
    if (bits != TB_JMP_CACHE_BITS) {
        qemu_printf("jmp-cache: %u bits (%u entries)\n", bits,
                    xemu_jc_geom.size);
    }
}

bool tcg_exec_realizefn(CPUState *cpu, Error **errp)
{
    static bool tcg_target_initialized;

    xemu_jc_geom_latch();

    if (!tcg_target_initialized) {
        /* Check mandatory TCGCPUOps handlers */
        const TCGCPUOps *tcg_ops = cpu->cc->tcg_ops;
#ifndef CONFIG_USER_ONLY
        assert(tcg_ops->cpu_exec_halt);
        assert(tcg_ops->cpu_exec_interrupt);
        assert(tcg_ops->cpu_exec_reset);
        assert(tcg_ops->pointer_wrap);
#endif /* !CONFIG_USER_ONLY */
        assert(tcg_ops->translate_code);
        assert(tcg_ops->get_tb_cpu_state);
        assert(tcg_ops->mmu_index);
        tcg_ops->initialize();
        tcg_target_initialized = true;
    }

    cpu->tb_jmp_cache = g_malloc0(sizeof(CPUJumpCache) +
                                  (size_t)xemu_jc_geom.size *
                                  sizeof(cpu->tb_jmp_cache->array[0]));
    tlb_init(cpu);
#ifndef CONFIG_USER_ONLY
    tcg_iommu_init_notifier_list(cpu);
#endif /* !CONFIG_USER_ONLY */
    /* qemu_plugin_vcpu_init_hook delayed until cpu_index assigned. */

    return true;
}

/* undo the initializations in reverse order */
void tcg_exec_unrealizefn(CPUState *cpu)
{
#ifndef CONFIG_USER_ONLY
    tcg_iommu_free_notifier_list(cpu);
#endif /* !CONFIG_USER_ONLY */

    tlb_destroy(cpu);
    g_free_rcu(cpu->tb_jmp_cache, rcu);
}
