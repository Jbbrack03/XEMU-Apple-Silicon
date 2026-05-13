/*
 * xbox-oracle-agent — SMC RPC implementation.
 *
 * Register map and SMBus address are sourced from xemu's own SMC
 * emulation at hw/xbox/smbus_xbox_smc.c:74-93 and the public
 * xboxdevwiki PIC documentation. nxdk's hal/led.c:16-29 confirms the
 * post-boot HalWriteSMBusValue(0x20, ...) calling convention works
 * from a chainloaded XBE without re-issuing the PIC challenge.
 */
#include "smc.h"
#include "commands.h"
#include "protocol.h"

#include <xboxkrnl/xboxkrnl.h>
#include <lwip/api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* SMC SMBus slave address in 8-bit HAL form (7-bit 0x10 << 1). */
#define SMC_SMBUS_ADDR        0x20

/* Register offsets. Names match xemu's hw/xbox/smbus_xbox_smc.c. */
#define SMC_REG_VER           0x01
#define SMC_REG_TRAYSTATE     0x03
#define SMC_REG_AVPACK        0x04
#define SMC_REG_FANMODE       0x05
#define SMC_REG_FANSPEED      0x06
#define SMC_REG_CPUTEMP       0x09
#define SMC_REG_BOARDTEMP     0x0a
#define SMC_REG_FANSPEED_RB   0x10  /* public PIC docs; not in xemu enum */
#define SMC_REG_SCRATCH       0x1b

#define SMC_FANMODE_AUTO      0x00
#define SMC_FANMODE_MANUAL    0x01
#define SMC_FANSPEED_RAW_MAX  50    /* 0..50 maps to 0..100% PWM duty */

/* Session-scoped state for cleanup + smc.temps reporting. Cleared on
 * agent restart. */
static int s_smc_manual_active = 0;
static int s_smc_last_percent  = -1;  /* -1 = never written; 0..100 otherwise */

static int smc_read_allowed(uint8_t off)
{
    switch (off) {
    case SMC_REG_VER:
    case SMC_REG_TRAYSTATE:
    case SMC_REG_AVPACK:
    case SMC_REG_CPUTEMP:
    case SMC_REG_BOARDTEMP:
    case SMC_REG_FANSPEED_RB:
    case SMC_REG_SCRATCH:
        return 1;
    default:
        return 0;
    }
}

static int smc_write_allowed(uint8_t off)
{
    return (off == SMC_REG_FANMODE) || (off == SMC_REG_FANSPEED);
}

/* Single-byte read with NT status check. Returns 0 on success and
 * writes the byte to *out; -1 on failure. */
static int smc_read_byte(uint8_t reg, uint8_t *out)
{
    ULONG val = 0;
    NTSTATUS s = HalReadSMBusValue(SMC_SMBUS_ADDR, reg, FALSE, &val);
    if (!NT_SUCCESS(s)) {
        return -1;
    }
    *out = (uint8_t)(val & 0xFFu);
    return 0;
}

/* Single-byte write with NT status check. Returns 0 on success, -1 on
 * failure. */
static int smc_write_byte(uint8_t reg, uint8_t val)
{
    NTSTATUS s = HalWriteSMBusValue(SMC_SMBUS_ADDR, reg, FALSE, val);
    return NT_SUCCESS(s) ? 0 : -1;
}

void oracle_smc_cleanup_if_manual(void)
{
    if (!s_smc_manual_active) return;
    /* Best effort — caller is exiting anyway. */
    if (smc_write_byte(SMC_REG_FANMODE, SMC_FANMODE_AUTO) == 0) {
        s_smc_manual_active = 0;
    }
}

/* smc.read off=0xNN
 *   Reads a single byte from an allowlisted SMC register. */
int cmd_smc_read(struct netconn *c, const char *args)
{
    uint32_t off = 0;
    if (op_parse_kv_u32(args, "off", &off) != 0) {
        op_send_errf(c, "usage: smc.read off=0xHEX");
        return 0;
    }
    if (off > 0xFF) {
        op_send_errf(c, "off 0x%08lx out of byte range",
                     (unsigned long)off);
        return 0;
    }
    uint8_t reg = (uint8_t)off;
    if (!smc_read_allowed(reg)) {
        op_send_errf(c,
                     "smc.read not allowed for off=0x%02x "
                     "(side-effecting or dangerous)",
                     reg);
        return 0;
    }
    uint8_t v = 0;
    if (smc_read_byte(reg, &v) != 0) {
        op_send_errf(c, "HalReadSMBusValue failed for off=0x%02x", reg);
        return 0;
    }
    op_send_okf(c, "off=0x%02x val=0x%02x dec=%u", reg, v, (unsigned)v);
    return 0;
}

/* smc.write off=0xNN val=0xVV  (gated by unsafe.enable, register
 *   allowlist = {0x05 FANMODE, 0x06 FANSPEED}). */
int cmd_smc_write(struct netconn *c, const char *args)
{
    if (!oracle_writes_enabled()) {
        op_send_errf(c, "writes disabled — call unsafe.enable first");
        return 0;
    }
    uint32_t off = 0, val = 0;
    if (op_parse_kv_u32(args, "off", &off) != 0 ||
        op_parse_kv_u32(args, "val", &val) != 0) {
        op_send_errf(c, "usage: smc.write off=0xHEX val=0xHEX");
        return 0;
    }
    if (off > 0xFF || val > 0xFF) {
        op_send_errf(c, "off and val must be 0..0xff");
        return 0;
    }
    uint8_t reg = (uint8_t)off;
    uint8_t v   = (uint8_t)val;
    if (!smc_write_allowed(reg)) {
        op_send_errf(c,
                     "smc.write not allowed for off=0x%02x "
                     "(allowlist: 0x05 FANMODE, 0x06 FANSPEED)",
                     reg);
        return 0;
    }
    /* Per-register value-domain check. xemu's PIC16LC model only
     * defines FANMODE 0/1 and FANSPEED 0..50; sending out-of-range
     * values to real hardware is undefined behavior. Callers that
     * want the high-level percent mapping should use smc.fan. */
    if (reg == SMC_REG_FANMODE && v > 1) {
        op_send_errf(c, "FANMODE value must be 0 (auto) or 1 (manual)");
        return 0;
    }
    if (reg == SMC_REG_FANSPEED && v > SMC_FANSPEED_RAW_MAX) {
        op_send_errf(c, "FANSPEED raw value must be 0..%u",
                     SMC_FANSPEED_RAW_MAX);
        return 0;
    }
    if (smc_write_byte(reg, v) != 0) {
        op_send_errf(c, "HalWriteSMBusValue failed for off=0x%02x", reg);
        return 0;
    }
    /* Keep cleanup state coherent with raw writes. */
    if (reg == SMC_REG_FANMODE) {
        s_smc_manual_active = (v != SMC_FANMODE_AUTO) ? 1 : 0;
        if (v == SMC_FANMODE_AUTO) {
            s_smc_last_percent = -1;
        }
    }
    op_send_okf(c, "wrote off=0x%02x val=0x%02x", reg, v);
    return 0;
}

/* smc.temps  (always safe; no writes). */
int cmd_smc_temps(struct netconn *c, const char *args)
{
    (void)args;
    uint8_t cpu_c = 0, board_c = 0, avpack = 0, fanspeed_rb = 0;
    int rb_ok = 0;

    if (smc_read_byte(SMC_REG_CPUTEMP, &cpu_c) != 0) {
        op_send_errf(c, "HalReadSMBusValue failed for cputemp");
        return 0;
    }
    if (smc_read_byte(SMC_REG_BOARDTEMP, &board_c) != 0) {
        op_send_errf(c, "HalReadSMBusValue failed for boardtemp");
        return 0;
    }
    /* Best-effort: avpack should always succeed but don't fail the
     * whole command if it doesn't. */
    if (smc_read_byte(SMC_REG_AVPACK, &avpack) != 0) {
        avpack = 0xFF;
    }
    /* Best-effort: fan-speed readback may not be implemented on every
     * SMC revision (xemu's enum doesn't define 0x10, public PIC docs
     * do). Report only when the read succeeds with a non-trivial value
     * that would be plausible for the fan controller. */
    if (smc_read_byte(SMC_REG_FANSPEED_RB, &fanspeed_rb) == 0) {
        rb_ok = 1;
    }

    const char *mode = s_smc_manual_active ? "manual" : "auto";
    if (rb_ok) {
        op_send_okf(c,
                    "cpu_c=%u board_c=%u avpack=0x%02x "
                    "fan_mode=%s fan_percent=%d fan_raw_rb=%u",
                    (unsigned)cpu_c, (unsigned)board_c, (unsigned)avpack,
                    mode, s_smc_last_percent, (unsigned)fanspeed_rb);
    } else {
        op_send_okf(c,
                    "cpu_c=%u board_c=%u avpack=0x%02x "
                    "fan_mode=%s fan_percent=%d",
                    (unsigned)cpu_c, (unsigned)board_c, (unsigned)avpack,
                    mode, s_smc_last_percent);
    }
    return 0;
}

/* smc.fan val=auto|0-100  (gated by unsafe.enable). */
int cmd_smc_fan(struct netconn *c, const char *args)
{
    if (!oracle_writes_enabled()) {
        op_send_errf(c, "writes disabled — call unsafe.enable first");
        return 0;
    }

    /* Look for val=<token>. We accept either the literal "auto" or a
     * decimal/0xHEX percent. op_parse_kv_str gives us the raw token. */
    char tok[16];
    if (op_parse_kv_str(args, "val", tok, sizeof(tok)) != 0) {
        op_send_errf(c, "usage: smc.fan val=auto|0-100");
        return 0;
    }
    /* Trim trailing whitespace just in case. */
    size_t n = strlen(tok);
    while (n > 0 && (tok[n - 1] == ' ' || tok[n - 1] == '\t' ||
                     tok[n - 1] == '\r' || tok[n - 1] == '\n')) {
        tok[--n] = 0;
    }

    if (strcmp(tok, "auto") == 0) {
        if (smc_write_byte(SMC_REG_FANMODE, SMC_FANMODE_AUTO) != 0) {
            op_send_errf(c, "HalWriteSMBusValue failed for fanmode=auto");
            return 0;
        }
        s_smc_manual_active = 0;
        s_smc_last_percent  = -1;
        op_send_okf(c, "fan mode=auto");
        return 0;
    }

    /* Numeric percentage. Accept decimal or 0x-prefixed hex via
     * strtoul. */
    char *end = NULL;
    unsigned long pct = strtoul(tok, &end, 0);
    if (end == tok || *end != 0 || pct > 100) {
        op_send_errf(c, "usage: smc.fan val=auto|0-100 (got %s)", tok);
        return 0;
    }

    /* Map 0..100 percent to 0..50 raw. Round to nearest to avoid
     * underbiasing on odd percentages. */
    unsigned raw = (unsigned)((pct * SMC_FANSPEED_RAW_MAX + 50) / 100);
    if (raw > SMC_FANSPEED_RAW_MAX) raw = SMC_FANSPEED_RAW_MAX;

    /* Order matters: switch to manual first, then set the speed. If
     * the second write fails we still revert to auto so we don't
     * leave the system in manual with a stale speed. */
    if (smc_write_byte(SMC_REG_FANMODE, SMC_FANMODE_MANUAL) != 0) {
        op_send_errf(c, "HalWriteSMBusValue failed for fanmode=manual");
        return 0;
    }
    if (smc_write_byte(SMC_REG_FANSPEED, (uint8_t)raw) != 0) {
        /* Best-effort revert. */
        (void)smc_write_byte(SMC_REG_FANMODE, SMC_FANMODE_AUTO);
        op_send_errf(c, "HalWriteSMBusValue failed for fanspeed=%u", raw);
        return 0;
    }

    s_smc_manual_active = 1;
    s_smc_last_percent  = (int)pct;
    op_send_okf(c, "fan mode=manual percent=%lu raw=%u",
                (unsigned long)pct, raw);
    return 0;
}
