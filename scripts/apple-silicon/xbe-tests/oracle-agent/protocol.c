/*
 * xbox-oracle-agent — wire protocol implementation.
 */
#include "protocol.h"

#include <lwip/api.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void op_send_line(struct netconn *c, const char *s)
{
    netconn_write(c, s, strlen(s), NETCONN_COPY);
    netconn_write(c, "\r\n", 2, NETCONN_NOCOPY);
}

static void op_vprintln(struct netconn *c, const char *prefix, const char *fmt, va_list ap)
{
    char buf[512];
    int n = 0;
    if (prefix && *prefix) {
        n = (int)strlen(prefix);
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        memcpy(buf, prefix, (size_t)n);
    }
    int wrote = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    if (wrote < 0) wrote = 0;
    if ((size_t)(n + wrote) >= sizeof(buf)) {
        buf[sizeof(buf) - 1] = 0;
    }
    op_send_line(c, buf);
}

void op_send_okf(struct netconn *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    op_vprintln(c, "200- ", fmt, ap);
    va_end(ap);
}

void op_send_errf(struct netconn *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    op_vprintln(c, "500- ", fmt, ap);
    va_end(ap);
}

void op_send_text_begin(struct netconn *c, size_t n_or_zero)
{
    char hdr[64];
    if (n_or_zero) {
        snprintf(hdr, sizeof(hdr), "201- OK %u", (unsigned)n_or_zero);
    } else {
        snprintf(hdr, sizeof(hdr), "201- OK");
    }
    op_send_line(c, hdr);
}

void op_send_text_end(struct netconn *c)
{
    op_send_line(c, ".");
}

void op_send_binary_header(struct netconn *c, size_t len)
{
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "202- BINARY %u", (unsigned)len);
    op_send_line(c, hdr);
}

void op_send_binary_payload(struct netconn *c, const void *buf, size_t len)
{
    /* lwIP netconn_write can split into multiple TCP segments internally;
     * the caller hands us the full payload, lwIP handles fragmentation. */
    if (len == 0) return;
    netconn_write(c, buf, len, NETCONN_COPY);
}

/* ---- parsers ---- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Extract token "key=" from args. Returns pointer to first byte AFTER '='
 * or NULL if not found. Tokens are separated by whitespace; the value
 * runs until the next whitespace or end of string. */
static const char *find_kv(const char *args, const char *key)
{
    if (!args || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            return p + klen + 1;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return NULL;
}

int op_parse_kv_u32(const char *args, const char *key, uint32_t *out)
{
    const char *v = find_kv(args, key);
    if (!v) return -1;
    int base = 10;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        base = 16;
        v += 2;
    }
    uint32_t acc = 0;
    int any = 0;
    while (*v && *v != ' ' && *v != '\t') {
        int d;
        if (base == 16) {
            d = hex_nibble(*v);
            if (d < 0) return -1;
        } else {
            if (*v < '0' || *v > '9') return -1;
            d = *v - '0';
        }
        /* Reject values that don't fit in uint32_t. Without this,
         * callers' downstream range checks (e.g. `val > 0xFF` in
         * smc.write) can be bypassed by oversized tokens that
         * silently wrap during accumulation. */
        if (acc > (UINT32_MAX - (uint32_t)d) / (uint32_t)base) {
            return -1;
        }
        acc = acc * (uint32_t)base + (uint32_t)d;
        v++;
        any = 1;
    }
    if (!any) return -1;
    *out = acc;
    return 0;
}

int op_parse_kv_str(const char *args, const char *key, char *out, size_t outsz)
{
    const char *v = find_kv(args, key);
    if (!v || outsz == 0) return -1;
    size_t i = 0;
    while (*v && *v != ' ' && *v != '\t' && i + 1 < outsz) {
        out[i++] = *v++;
    }
    out[i] = 0;
    return (i > 0) ? 0 : -1;
}

int op_parse_hex_buf(const char *hex, uint8_t *out, size_t outcap, size_t *outlen)
{
    if (!hex) return -1;
    size_t n = strlen(hex);
    if (n & 1u) return -1;
    n /= 2;
    if (n > outcap) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *outlen = n;
    return 0;
}

/* Allow generic-byte mem.read / mem.write only against the four
 * Xbox RAM aliases (cached and uncached views of the same 64 MB
 * of system DRAM):
 *   0x00000000 .. 0x03FFFFFF — physical RAM (low alias)
 *   0x80000000 .. 0x83FFFFFF — kseg0 RAM mirror (cached)
 *   0xB0000000 .. 0xB3FFFFFF — kseg1 RAM mirror (uncached / AGP)
 *   0xF0000000 .. 0xF3FFFFFF — write-combined VRAM aperture
 *
 * MMIO regions (NV2A BAR0, APU, ACI, USB) are intentionally NOT
 * in this allowlist. Byte-wide memcpy / netconn_write into MMIO
 * generates unaligned and split-bus accesses with side effects
 * that the device side may not tolerate (Codex 2026-05-06). For
 * NV2A BAR0 use the typed `nv2a.read` / `nv2a.write` commands,
 * which always do 32-bit aligned access. APU/ACI/USB are
 * out-of-scope for the oracle today. */
int op_addr_range_ok(uint32_t addr, uint32_t len)
{
    if (len == 0) return 0;
    /* Overflow check */
    uint64_t end = (uint64_t)addr + (uint64_t)len;
    if (end > 0xFFFFFFFFull) return 0;

    static const struct { uint32_t base; uint32_t size; } regions[] = {
        { 0x00000000, 0x04000000 },
        { 0x80000000, 0x04000000 },
        { 0xB0000000, 0x04000000 },
        { 0xF0000000, 0x04000000 },
    };
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        uint64_t r_end = (uint64_t)regions[i].base + (uint64_t)regions[i].size;
        if ((uint64_t)addr >= regions[i].base && end <= r_end) {
            return 1;
        }
    }
    return 0;
}
