/*
 * xbed_tier2_hook — shared ABI for the retail-input Tier-2 hook probe.
 *
 * The oracle agent allocates one persistent page for a resident hook blob,
 * writes this header at the start of the page, and stores the page address in
 * XBED_TIER2_HOOK_ANCHOR_PATH. Chainloaded diagnostic XBEs can read the same
 * page before rebooting and report whether the hook was actually hit while
 * their title-facing input stack ran.
 */
#ifndef XBED_TIER2_HOOK_H
#define XBED_TIER2_HOOK_H

#include <stddef.h>
#include <stdint.h>

#define XBED_TIER2_HOOK_MAGIC      0x48325458u  /* 'XT2H' little-endian */
#define XBED_TIER2_HOOK_VERSION    1u
#define XBED_TIER2_HOOK_PAGE_SIZE  0x1000u
#define XBED_TIER2_HOOK_CODE_SIZE  64u

#define XBED_TIER2_HOOK_ANCHOR_PATH \
    "E:\\Apps\\oracle-agent\\state\\tier2-hook-addr.txt"

/* Kernel 5838 recipe matched by tier2-shim-analyze.py. The export table slot
 * stores RVAs, not absolute VAs. */
#define XBED_TIER2_KERNEL_BASE             0x80010000u
#define XBED_TIER2_KE_RAISE_SLOT_VA        0x800104e8u
#define XBED_TIER2_KE_RAISE_ORIGINAL_RVA   0x00003d04u
#define XBED_TIER2_KE_RAISE_ORIGINAL_VA \
    (XBED_TIER2_KERNEL_BASE + XBED_TIER2_KE_RAISE_ORIGINAL_RVA)

struct __attribute__((packed)) xbed_tier2_hook_page {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t flags;

    uint32_t hook_slot_va;
    uint32_t original_rva;
    uint32_t original_va;
    uint32_t hook_va;
    uint32_t hook_rva;
    uint32_t code_off;
    uint32_t code_size;

    volatile uint32_t calls;
    volatile uint32_t installs;
    volatile uint32_t uninstalls;
    volatile uint32_t last_error;

    uint32_t reserved[17];
    uint8_t code[XBED_TIER2_HOOK_CODE_SIZE];
};

#define XBED_TIER2_HOOK_CODE_OFF \
    ((uint32_t)offsetof(struct xbed_tier2_hook_page, code))

#endif /* XBED_TIER2_HOOK_H */
