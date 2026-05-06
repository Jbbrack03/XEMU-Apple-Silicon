/*
 * xbox-oracle-agent — wire protocol helpers.
 *
 * Response codes (XBDM/SMTP-inspired so a streaming line parser works):
 *   200- <text>            single-line success
 *   201- OK [<count>]      multi-line text begin; lines follow; ends with .\r\n
 *   202- BINARY <length>   binary payload begin; exactly <length> raw bytes follow
 *   500- <text>            error
 *
 * Lines are CRLF-terminated. Binary payloads have no trailer — the
 * client reads exactly the declared byte count.
 */
#ifndef ORACLE_PROTOCOL_H
#define ORACLE_PROTOCOL_H

#include <lwip/api.h>
#include <stddef.h>
#include <stdint.h>

/* Single-line writers. */
void op_send_line(struct netconn *c, const char *s);
void op_send_okf(struct netconn *c, const char *fmt, ...);
void op_send_errf(struct netconn *c, const char *fmt, ...);

/* 201- multi-line text payload. Caller emits each line via op_send_line()
 * and finishes with op_send_text_end(). */
void op_send_text_begin(struct netconn *c, size_t n_or_zero);
void op_send_text_end(struct netconn *c);

/* 202- binary payload. Send the header line, then exactly len bytes. */
void op_send_binary_header(struct netconn *c, size_t len);
void op_send_binary_payload(struct netconn *c, const void *buf, size_t len);

/* Parsers. Return 0 on success, -1 on failure. */
int op_parse_kv_u32(const char *args, const char *key, uint32_t *out);
int op_parse_kv_str(const char *args, const char *key, char *out, size_t outsz);
int op_parse_hex_buf(const char *hex, uint8_t *out, size_t outcap, size_t *outlen);

/* Range checks for memory commands. Returns 1 if [addr, addr+len) lies
 * inside a known-safe Xbox memory window. */
int op_addr_range_ok(uint32_t addr, uint32_t len);

#endif /* ORACLE_PROTOCOL_H */
