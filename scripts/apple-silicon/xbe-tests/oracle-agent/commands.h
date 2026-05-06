/*
 * xbox-oracle-agent — command handlers.
 *
 * Each cmd_* takes the netconn and any trailing argument string
 * (everything after the command verb and the first space, untrimmed
 * past leading whitespace).
 *
 * Returns 0 normally, 1 to drop the connection and exit handle_client,
 * 2 to drop the connection AND stop the listener (oracle agent dies,
 * the kernel chainloads the next image — e.g. runxbe, reboot).
 */
#ifndef ORACLE_COMMANDS_H
#define ORACLE_COMMANDS_H

#include <lwip/api.h>

int cmd_info(struct netconn *c, const char *args);
int cmd_eeprom(struct netconn *c, const char *args);
int cmd_mem_read(struct netconn *c, const char *args);
int cmd_mem_write(struct netconn *c, const char *args);
int cmd_nv2a_read(struct netconn *c, const char *args);
int cmd_nv2a_write(struct netconn *c, const char *args);
int cmd_vram_read(struct netconn *c, const char *args);
int cmd_screenshot(struct netconn *c, const char *args);
int cmd_runxbe(struct netconn *c, const char *args);
int cmd_unsafe_enable(struct netconn *c, const char *args);
int cmd_reboot(struct netconn *c, const char *args);
int cmd_bye(struct netconn *c, const char *args);
int cmd_help(struct netconn *c, const char *args);

#endif /* ORACLE_COMMANDS_H */
