/*
 * xbox-oracle-agent — Tier-2 retail-input kernel hook probe commands.
 *
 * These commands implement the first rung of the software-only retail input
 * proof ladder: resident tail-jump/counter hooks at the NKPatcher-matched
 * KeRaiseIrqlToDpcLevel export slot. It does not mutate controller state.
 */
#ifndef ORACLE_TIER2_H
#define ORACLE_TIER2_H

#include <lwip/api.h>

void oracle_tier2_init(void);

int cmd_tier2_preflight(struct netconn *c, const char *args);
int cmd_tier2_install_jump_only(struct netconn *c, const char *args);
int cmd_tier2_install_noop(struct netconn *c, const char *args);
int cmd_tier2_uninstall(struct netconn *c, const char *args);
int cmd_tier2_status(struct netconn *c, const char *args);

#endif /* ORACLE_TIER2_H */
