/*
 * internal execution defines for qemu
 *
 *  Copyright (c) 2003 Fabrice Bellard
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

#ifndef EXEC_TB_HASH_H
#define EXEC_TB_HASH_H

#include "exec/vaddr.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "qemu/xxhash.h"
#include "tb-jmp-cache.h"

#ifdef CONFIG_SOFTMMU

/* Only the bottom xemu_jc_geom.page_bits of the jump cache hash bits vary
   for addresses on the same page.  The top bits are the same.  This allows
   TLB invalidation to quickly clear a subset of the hash table.  Geometry
   is runtime-latched (see tb-jmp-cache.h); the derived values below keep
   the upstream structure with the constants replaced by fields. */
#define TB_JMP_PAGE_SIZE (xemu_jc_geom.page_size)

static inline unsigned int tb_jmp_cache_hash_page(vaddr pc)
{
    if (likely(xemu_jc_geom.fixed_default_hash)) {
        const unsigned shift =
            TARGET_PAGE_BITS - TB_JMP_CACHE_DEFAULT_PAGE_BITS;
        vaddr tmp = pc ^ (pc >> shift);

        return (tmp >> shift) & TB_JMP_CACHE_DEFAULT_PAGE_MASK;
    }

    unsigned shift = TARGET_PAGE_BITS - xemu_jc_geom.page_bits;
    vaddr tmp = pc ^ (pc >> shift);
    return (tmp >> shift) & xemu_jc_geom.page_mask;
}

static inline unsigned int tb_jmp_cache_hash_func(vaddr pc)
{
    if (likely(xemu_jc_geom.fixed_default_hash)) {
        const unsigned shift =
            TARGET_PAGE_BITS - TB_JMP_CACHE_DEFAULT_PAGE_BITS;
        vaddr tmp = pc ^ (pc >> shift);

        return (((tmp >> shift) & TB_JMP_CACHE_DEFAULT_PAGE_MASK) |
                (tmp & TB_JMP_CACHE_DEFAULT_ADDR_MASK));
    }

    unsigned shift = TARGET_PAGE_BITS - xemu_jc_geom.page_bits;
    vaddr tmp = pc ^ (pc >> shift);
    return (((tmp >> shift) & xemu_jc_geom.page_mask)
           | (tmp & xemu_jc_geom.addr_mask));
}

#else

/* In user-mode we can get better hashing because we do not have a TLB */
static inline unsigned int tb_jmp_cache_hash_func(vaddr pc)
{
    if (likely(xemu_jc_geom.fixed_default_hash)) {
        return (pc ^ (pc >> TB_JMP_CACHE_BITS)) &
               ((1u << TB_JMP_CACHE_BITS) - 1);
    }

    return (pc ^ (pc >> xemu_jc_geom.bits)) & (TB_JMP_CACHE_SIZE - 1);
}

#endif /* CONFIG_SOFTMMU */

static inline
uint32_t tb_hash_func(tb_page_addr_t phys_pc, vaddr pc,
                      uint32_t flags, uint64_t flags2, uint32_t cf_mask)
{
    return qemu_xxhash8(phys_pc, pc, flags2, flags, cf_mask);
}

#endif
