/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "renderer.h"

#ifdef __ANDROID__
#include <android/log.h>
#define REPORT_DIAG_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "xemu-report-diag", __VA_ARGS__)
#else
#define REPORT_DIAG_LOG(...) fprintf(stderr, "xemu-report-diag: " __VA_ARGS__)
#endif

/* s42 diagnostic (env XEMU_REPORT_DIAG=1, cold when unset): trace queued-report
 * starvation — a report sitting in report_queue while no drain path fires
 * leaves the guest polling its report address forever (vCPU tight-spin). */
static bool report_diag_enabled(void)
{
    static bool initialized = false;
    static bool enabled = false;
    if (!initialized) {
        const char *env = getenv("XEMU_REPORT_DIAG");
        enabled = env && env[0] == '1';
        initialized = true;
    }
    return enabled;
}

/* s42 fix (default on; XEMU_REPORT_STARVATION_FIX=0 = exact rollback): a
 * queued report must always have a drain path once the FIFO idles, or the
 * guest polls its report address forever. */
static bool report_starvation_fix_enabled(void)
{
    static bool initialized = false;
    static bool enabled = true;
    if (!initialized) {
        const char *env = getenv("XEMU_REPORT_STARVATION_FIX");
        if (env && env[0] == '0') {
            enabled = false;
        }
        initialized = true;
    }
    return enabled;
}

void pgraph_vk_init_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_LOG("init_reports: begin");

    QSIMPLEQ_INIT(&r->report_queue);
    QSIMPLEQ_INIT(&r->report_pool);
    r->num_queries_in_flight = 0;
    r->max_queries_in_flight = 1024;
    r->new_query_needed = false;
    r->query_in_flight = false;
    r->zpass_pixel_count_result = 0;

    r->report_pool_entries =
        g_malloc_n(r->max_queries_in_flight, sizeof(QueryReport));
    for (int i = 0; i < r->max_queries_in_flight; i++) {
        QSIMPLEQ_INSERT_TAIL(&r->report_pool,
                              &r->report_pool_entries[i], entry);
    }

    r->query_results =
        g_malloc_n(r->max_queries_in_flight, sizeof(uint64_t));

    VkQueryPoolCreateInfo pool_create_info = (VkQueryPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_OCCLUSION,
        .queryCount = r->max_queries_in_flight,
    };
    VK_CHECK(
        vkCreateQueryPool(r->device, &pool_create_info, NULL, &r->query_pool));
}

void pgraph_vk_finalize_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QSIMPLEQ_INIT(&r->report_queue);
    QSIMPLEQ_INIT(&r->report_pool);

    g_free(r->report_pool_entries);
    r->report_pool_entries = NULL;
    g_free(r->query_results);
    r->query_results = NULL;

    vkDestroyQueryPool(r->device, r->query_pool, NULL);
}

/* Candidate (XEMU_EARLY_REPORT_SUBMIT): when the guest enqueues a zpass
 * report and the current command buffer already carries substantial work,
 * submit it immediately as a deferred finish (no fence wait, no report
 * drain). The GPU starts on the batched work at enqueue time instead of at
 * the eventual pfifo stall, so the stall's blocking
 * vkGetQueryPoolResults(WAIT) sees mostly-complete queries. */
static bool early_report_submit_enabled(void)
{
    static bool initialized;
    static bool enabled;

    if (!initialized) {
        const char *value = getenv("XEMU_EARLY_REPORT_SUBMIT");
        if (value && value[0]) {
            enabled = strcmp(value, "0") != 0;
        }
        initialized = true;
    }
    return enabled;
}

static int early_report_submit_min_draws(void)
{
    static bool initialized;
    static int min_draws = 24;

    if (!initialized) {
        const char *value = getenv("XEMU_EARLY_REPORT_SUBMIT_MIN_DRAWS");
        if (value && value[0]) {
            int parsed = atoi(value);
            if (parsed > 0) {
                min_draws = parsed;
            }
        }
        initialized = true;
    }
    return min_draws;
}

static QueryReport *alloc_report(PGRAPHVkState *r)
{
    QueryReport *report = QSIMPLEQ_FIRST(&r->report_pool);
    if (report) {
        QSIMPLEQ_REMOVE_HEAD(&r->report_pool, entry);
    } else {
        report = g_malloc(sizeof(QueryReport));
    }
    return report;
}

static void free_report(PGRAPHVkState *r, QueryReport *report)
{
    QSIMPLEQ_INSERT_TAIL(&r->report_pool, report, entry);
}

void pgraph_vk_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueryReport *report = alloc_report(r);
    report->clear = true;
    report->parameter = 0;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

void pgraph_vk_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);
    assert(type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);

    QueryReport *report = alloc_report(r);
    report->clear = false;
    report->parameter = parameter;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;

    if (report_diag_enabled()) {
        static unsigned enq_count;
        enq_count++;
        if (enq_count <= 4 || (enq_count % 256) == 0) {
            REPORT_DIAG_LOG("get_report #%u qc=%d in_cb=%d nqif=%d draws=%d",
                            enq_count, report->query_count,
                            r->in_command_buffer, r->num_queries_in_flight,
                            r->draws_in_cb);
        }
    }

    if (early_report_submit_enabled()) {
        OPT_STAT_INC(ers_seen);
        /* The query-pool guard mirrors the stall-skip guard below: an early
         * submit skips the report drain (the only reset of
         * num_queries_in_flight) while its staging reset suppresses the
         * NEED_BUFFER_SPACE finishes that would otherwise drain. Once the
         * pool is half full, fall back to baseline dynamics so a draining
         * finish happens before begin_query's live pool-limit assert. */
        /* pending_post_fence_cb would convert this finish into one whose
         * render-thread handler fence-waits; this path spin-waits while
         * holding pgraph.lock (locked method dispatch), so never arm the
         * early submit with a callback pending. */
        if (!r->is_render_thread_context && r->in_command_buffer &&
            !r->in_draw && !r->pending_post_fence_cb &&
            r->num_queries_in_flight > 0 &&
            r->num_queries_in_flight < (r->max_queries_in_flight / 2) &&
            r->draws_in_cb >= early_report_submit_min_draws()) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_EARLY_REPORT);
        }
    }
}

void pgraph_vk_process_pending_reports_internal(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Processing queries");

    assert(!r->in_command_buffer);

    uint64_t *query_results = r->query_results;

    if (r->num_queries_in_flight > 0) {
        size_t size_of_results = r->num_queries_in_flight * sizeof(uint64_t);
        VkResult result;
        do {
            result = vkGetQueryPoolResults(
                r->device, r->query_pool, 0, r->num_queries_in_flight,
                size_of_results, query_results, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        } while (result == VK_NOT_READY);
    }

    // Write out queries
    int num_results_counted = 0;
    const int result_divisor =
        pg->surface_scale_factor * pg->surface_scale_factor;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        assert(report->query_count >= num_results_counted);
        assert(report->query_count <= r->num_queries_in_flight);

        while (num_results_counted < report->query_count) {
            r->zpass_pixel_count_result +=
                query_results[num_results_counted++];
        }

        if (report->clear) {
            NV2A_VK_DPRINTF("Cleared");
            r->zpass_pixel_count_result = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(
                d, report->parameter,
                r->zpass_pixel_count_result / result_divisor);
        }

        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        free_report(r, report);
    }

    // Add remaining results
    while (num_results_counted < r->num_queries_in_flight) {
        r->zpass_pixel_count_result += query_results[num_results_counted++];
    }

    r->num_queries_in_flight = 0;
    NV2A_VK_DGROUP_END();
}

void pgraph_vk_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    /* If an early report submit already flushed the command buffer, the
     * STALLED drain below can never fire (it requires an open CB) and the
     * queued reports would starve while the guest polls. Every query
     * belonging to a queued report has been submitted in that case, so
     * drain directly. Gated by the same flag as the early submit so
     * XEMU_EARLY_REPORT_SUBMIT=0 restores baseline behavior exactly. */
    if (early_report_submit_enabled() &&
        *dma_get == *dma_put && !r->in_command_buffer &&
        !QSIMPLEQ_EMPTY(&r->report_queue)) {
        OPT_STAT_INC(ers_nocb_drains);
        pgraph_vk_process_pending_reports_internal(d);
        return;
    }

    /* s42 starvation guard: with the FIFO idle (get==put) and a report
     * queued but NO open CB, nothing below can ever drain it — the STALLED
     * finish requires an open CB, and a polling guest issues no further
     * methods to open one. Every query belonging to the queued reports was
     * submitted with a previous finish (baseline drains on every finish, so
     * nqif==0 here and the report only needs the accumulated zpass result
     * written out). Same shape as the early-submit no-CB drain above, made
     * unconditional. Without this the guest spins on the report address
     * forever (s42 disc-swap hang, 4/15 tours). */
    if (report_starvation_fix_enabled() &&
        *dma_get == *dma_put && !r->in_command_buffer &&
        !QSIMPLEQ_EMPTY(&r->report_queue)) {
        pgraph_vk_process_pending_reports_internal(d);
        return;
    }

    if (*dma_get == *dma_put && r->in_command_buffer) {
        /* This preemptive drain-finish exists only to service pending
         * occlusion reports: every finish runs process_pending_reports_
         * internal(), and that is the only consumer that needs the GPU
         * flushed here. When no report is queued the finish does no report
         * work and is pure latency-flush overhead -- Halo's opening hits this
         * ~60x/frame. Backend-semaphore release, WAIT_FOR_IDLE, and surface
         * reads all self-finish (see pgraph.c BACK_END_WRITE_SEMAPHORE_RELEASE
         * / WAIT_FOR_IDLE), and the per-frame flip (PRESENTING) submits any
         * accumulated work regardless, so skipping is safe when no report is
         * pending. The query-count guard keeps the occlusion pool from
         * approaching its limit while finishes are deferred. */
        if (xemu_get_skip_empty_report_stalls() &&
            QSIMPLEQ_EMPTY(&r->report_queue) &&
            r->num_queries_in_flight < (r->max_queries_in_flight / 2)) {
            OPT_STAT_INC(stall_skipped_empty);
            return;
        }
        /* s42 starvation guard, open-CB variant: stall-batching exists to
         * skip redundant EMPTY-queue stall finishes; it must never gate the
         * drain of an actually-queued report (the guest may already be
         * polling it, issuing no further draws to advance draw_time). */
        if (pg->draw_time != r->last_stall_draw_time ||
            (report_starvation_fix_enabled() &&
             !QSIMPLEQ_EMPTY(&r->report_queue))) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_STALLED);
            r->last_stall_draw_time = pg->draw_time;
        } else {
            OPT_STAT_INC(stall_batched);
        }
    }

    if (report_diag_enabled()) {
        /* Any report still queued when this pass ends took a non-draining
         * path; if the guest is polling that report, no further drain can
         * ever fire (no new methods -> no finish) = permanent starvation. */
        static unsigned starve_count;
        if (!QSIMPLEQ_EMPTY(&r->report_queue)) {
            starve_count++;
            if (starve_count <= 8 || (starve_count % 512) == 0) {
                int depth = 0;
                QueryReport *qr;
                QSIMPLEQ_FOREACH(qr, &r->report_queue, entry) { depth++; }
                REPORT_DIAG_LOG(
                    "starved pass #%u depth=%d in_cb=%d get=%08x put=%08x "
                    "draw_time=%" PRId64 " last_stall=%" PRId64 " nqif=%d",
                    starve_count, depth, r->in_command_buffer, *dma_get,
                    *dma_put, (int64_t)pg->draw_time,
                    (int64_t)r->last_stall_draw_time,
                    r->num_queries_in_flight);
            }
        } else if (starve_count) {
            REPORT_DIAG_LOG("starvation cleared after %u passes", starve_count);
            starve_count = 0;
        }
    }
}
