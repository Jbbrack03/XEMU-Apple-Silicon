/*
 * xbox-oracle-agent — SMC (System Management Controller) RPC commands.
 *
 * The Xbox SMC (PIC16LC on v1.0-1.5, Xyclops on v1.6) sits on the
 * SMBus at 7-bit address 0x10 (HAL 8-bit form 0x20) and owns the
 * thermal sensors, fan control, AV-pack ID, traystate, LEDs, and
 * power/reset commands. We expose a tightly-allowlisted subset so a
 * headless Xbox can report temperatures and accept fan-curve
 * overrides over TCP 9001.
 *
 * Allowlists (intentionally narrow for first deploy):
 *
 *   smc.read off=0xNN allowlist:
 *     0x01 SMC_REG_VER         version byte (multi-byte string;
 *                              each successive read returns the next
 *                              byte of a 3-char identifier — caller
 *                              must read three times to get the full
 *                              string)
 *     0x03 SMC_REG_TRAYSTATE   tray state
 *     0x04 SMC_REG_AVPACK      AV-pack identifier
 *     0x09 SMC_REG_CPUTEMP     CPU temperature, °C
 *     0x0a SMC_REG_BOARDTEMP   motherboard temperature, °C
 *     0x10 SMC_REG_FANSPEED_RB current commanded fan speed readback
 *                              (per public PIC docs; not all SMC
 *                              revisions implement it — call may
 *                              return zero on unsupported hardware)
 *     0x1b SMC_REG_SCRATCH     boot-scratch flags (RW)
 *
 *   smc.write off=0xNN val=0xVV allowlist (gated by unsafe.enable):
 *     0x05 SMC_REG_FANMODE     0=auto thermal algorithm, non-zero=manual
 *     0x06 SMC_REG_FANSPEED    0..50 mapping to 0..100% PWM duty
 *
 * Side-effect notes (why other registers are NOT in the read allowlist):
 *   0x11 SMC_REG_INTSTATUS — clear-on-read; would mask pending kernel
 *                            interrupts
 *   0x18                   — xboxdevwiki documents this as a
 *                            dangerous read that can drive the SMC
 *                            into an overheated state
 *
 * Cleanup contract:
 *   `cmd_reboot` and `cmd_runxbe` MUST call
 *   oracle_smc_cleanup_if_manual() before exiting — both replace the
 *   agent image, so a stranded manual-mode FANSPEED would persist
 *   until the next dashboard interaction. `cmd_bye` MUST NOT call
 *   cleanup: oracle-client.py's polite-close sends `bye` after every
 *   command, so cleanup-on-bye would silently revert every caller's
 *   `smc.fan` set on the very next disconnect. This was a real bug
 *   caught mid-session during agent-v0.4 bring-up (2026-05-12).
 */
#ifndef ORACLE_SMC_H
#define ORACLE_SMC_H

#include <lwip/api.h>

/* Reverts FANMODE to auto if the agent set manual mode this session.
 * Safe to call unconditionally; no-op when manual mode was never set
 * or when a previous cleanup already ran. */
void oracle_smc_cleanup_if_manual(void);

int cmd_smc_read(struct netconn *c, const char *args);
int cmd_smc_write(struct netconn *c, const char *args);
int cmd_smc_temps(struct netconn *c, const char *args);
int cmd_smc_fan(struct netconn *c, const char *args);

#endif /* ORACLE_SMC_H */
