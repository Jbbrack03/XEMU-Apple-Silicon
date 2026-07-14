/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

#define TB_JMP_CACHE_BITS 12

/*
 * Runtime-sized jump cache (XEMU_JMP_CACHE_BITS, default
 * TB_JMP_CACHE_BITS). The Xbox UMA workload generates hundreds of
 * thousands of TBs; a fixed 4096-entry cache thrashes and every miss
 * pays a qht walk in helper_lookup_tb_ptr. Geometry is latched once in
 * tcg_exec_realizefn() before the (single) vCPU's cache is allocated
 * and is read-only afterwards.
 */
typedef struct XemuJmpCacheGeom {
    unsigned bits;       /* log2 total entries */
    unsigned page_bits;  /* bits/2: low-order intra-page hash bits */
    unsigned size;       /* 1 << bits */
    unsigned page_size;  /* 1 << page_bits */
    unsigned addr_mask;  /* page_size - 1 */
    unsigned page_mask;  /* size - page_size */
} XemuJmpCacheGeom;
extern XemuJmpCacheGeom xemu_jc_geom;

#define TB_JMP_CACHE_SIZE (xemu_jc_geom.size)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[];
} CPUJumpCache;

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
