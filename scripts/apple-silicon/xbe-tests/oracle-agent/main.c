/*
 * xbox-oracle-agent v0.4 (Phase 2 + controller.* + smc.*)
 *
 * A network-listening XBE that exposes the Xbox console as an oracle
 * for xemu correctness validation. Speaks a simple text-line RPC
 * protocol (with optional 202- BINARY length-prefixed payloads) on
 * TCP port 9001.
 *
 * Phase 1 commands (always present):
 *   info, eeprom, reboot, bye, help
 *
 * Phase 2 commands:
 *   mem.read / mem.write (gated) / nv2a.read / nv2a.write (gated) /
 *   vram.read / screenshot / runxbe / unsafe.enable
 *
 * v0.3 controller.* commands (2026-05-07):
 *   controller.set / controller.get / controller.button /
 *   controller.axis / controller.clear / controller.buffer-info
 *
 * v0.4 smc.* commands (2026-05-12):
 *   smc.read  — allowlisted SMC register read
 *   smc.write — allowlisted SMC register write (gated)
 *   smc.temps — convenience cpu+board+fan readout
 *   smc.fan   — set fan curve (auto or 0..100 percent; gated)
 *
 * Source layout:
 *   main.c            — entry point, network bring-up, listener, dispatch
 *   protocol.{h,c}    — wire-protocol helpers (line + binary writers, parsers)
 *   commands.{h,c}    — Phase 1 + Phase 2 command implementations
 *   controller.{h,c}  — v0.3 synthetic-input state + RPCs
 *   tier2.{h,c}       — Tier-2 kernel hook research (research-only,
 *                       install commands crash this Xbox)
 *   smc.{h,c}         — v0.4 SMC sensor + fan-control RPCs
 *
 * Built with nxdk; lwIP TCP via the same pattern as the httpd sample.
 */
#include "commands.h"
#include "controller.h"
#include "protocol.h"
#include "smc.h"
#include "tier2.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>
#include <nxdk/net.h>
#include <lwip/api.h>
#include <lwip/tcpip.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define ORACLE_PORT 9001

extern struct netif *g_pnetif;

typedef int (*cmd_fn)(struct netconn *c, const char *args);

struct cmd_entry {
    const char *verb;
    cmd_fn      fn;
};

static const struct cmd_entry s_cmds[] = {
    { "info",                    cmd_info                    },
    { "eeprom",                  cmd_eeprom                  },
    { "mem.read",                cmd_mem_read                },
    { "mem.write",               cmd_mem_write               },
    { "nv2a.read",               cmd_nv2a_read               },
    { "nv2a.write",              cmd_nv2a_write              },
    { "vram.read",               cmd_vram_read               },
    { "screenshot",              cmd_screenshot              },
    { "runxbe",                  cmd_runxbe                  },
    { "unsafe.enable",           cmd_unsafe_enable           },
    { "controller.set",          cmd_controller_set          },
    { "controller.get",          cmd_controller_get          },
    { "controller.button",       cmd_controller_button       },
    { "controller.axis",         cmd_controller_axis         },
    { "controller.clear",        cmd_controller_clear        },
    { "controller.buffer-info",  cmd_controller_buffer_info  },
    { "tier2.preflight",         cmd_tier2_preflight         },
    { "tier2.install-jump-only", cmd_tier2_install_jump_only },
    { "tier2.install-noop",      cmd_tier2_install_noop      },
    { "tier2.uninstall",         cmd_tier2_uninstall         },
    { "tier2.status",            cmd_tier2_status            },
    { "smc.read",                cmd_smc_read                },
    { "smc.write",               cmd_smc_write               },
    { "smc.temps",               cmd_smc_temps               },
    { "smc.fan",                 cmd_smc_fan                 },
    { "reboot",                  cmd_reboot                  },
    { "bye",                     cmd_bye                     },
    { "help",                    cmd_help                    },
    { NULL,                      NULL                        },
};

static int dispatch(struct netconn *c, char *line)
{
    int n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' ||
                     line[n - 1] == ' '  || line[n - 1] == '\t')) {
        line[--n] = 0;
    }
    if (n == 0) return 0;

    debugPrint("rcv: %s\n", line);

    /* Split verb / args at first whitespace. */
    char *args = strpbrk(line, " \t");
    if (args) {
        *args++ = 0;
        while (*args == ' ' || *args == '\t') args++;
    } else {
        args = "";
    }

    for (const struct cmd_entry *e = s_cmds; e->verb; e++) {
        if (strcmp(line, e->verb) == 0) {
            return e->fn(c, args);
        }
    }
    op_send_errf(c, "unknown command: %s", line);
    return 0;
}

/* Per-connection line buffer. Sized to fit the largest legitimate
 * command line: `mem.write addr=0xHHHHHHHH data=<hex>\r\n` with the
 * 1024-byte max payload encodes to 2 chars/byte = 2048 chars hex,
 * plus ~32 chars verb + key + addr + delimiters. Round up to 4 KiB
 * so the agent never silently truncates a max-sized write. Static
 * because handle_client runs on a single thread (the main accept
 * loop) — only one connection is being serviced at a time. */
static char s_line_buf[4096];

static void handle_client(struct netconn *c)
{
    debugPrint("client connected\n");
    op_send_okf(c, "xbox-oracle-agent ready");

    int blen = 0;
    int overflowed = 0;

    for (;;) {
        struct netbuf *inbuf = NULL;
        if (netconn_recv(c, &inbuf) != ERR_OK) break;

        do {
            char *data = NULL;
            u16_t len = 0;
            if (netbuf_data(inbuf, (void **)&data, &len) != ERR_OK) break;

            for (u16_t i = 0; i < len; i++) {
                char ch = data[i];
                if (ch == '\n') {
                    s_line_buf[blen] = 0;
                    if (overflowed) {
                        op_send_errf(c, "command line too long (max %u bytes)",
                                     (unsigned)(sizeof(s_line_buf) - 1));
                        overflowed = 0;
                        blen = 0;
                        continue;
                    }
                    int rc = dispatch(c, s_line_buf);
                    blen = 0;
                    if (rc != 0) {
                        netbuf_delete(inbuf);
                        netconn_close(c);
                        netconn_delete(c);
                        if (rc == 2) {
                            /* Caller (runxbe / reboot) is shutting us down. */
                        }
                        return;
                    }
                } else if (blen < (int)sizeof(s_line_buf) - 1) {
                    s_line_buf[blen++] = ch;
                } else {
                    /* Line too long; mark and continue swallowing
                     * bytes until the next \n so we can report the
                     * overflow as a 500- instead of silently
                     * dropping. */
                    overflowed = 1;
                }
            }
        } while (netbuf_next(inbuf) >= 0);
        netbuf_delete(inbuf);
    }
    netconn_close(c);
    netconn_delete(c);
    debugPrint("client disconnected\n");
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("\nxbox-oracle-agent v0.4 (Phase 2 + controller.* + smc.*)\n");
    debugPrint("Bringing up network...\n");

    /* Initialize the synthetic controller-state buffer up front so the
     * `controller.buffer-info` and `controller.get` commands return
     * sane values even before any client has called `controller.set`. */
    oracle_ctrl_init();
    oracle_tier2_init();

    nxNetInit(NULL);

    debugPrint("\nIP address.. %s\n", ip4addr_ntoa(netif_ip4_addr(g_pnetif)));
    debugPrint("Mask........ %s\n", ip4addr_ntoa(netif_ip4_netmask(g_pnetif)));
    debugPrint("Gateway..... %s\n", ip4addr_ntoa(netif_ip4_gw(g_pnetif)));
    debugPrint("\nListening on TCP port %d ...\n", ORACLE_PORT);

    struct netconn *listener = netconn_new(NETCONN_TCP);
    if (!listener) {
        debugPrint("netconn_new failed\n");
        return 1;
    }
    if (netconn_bind(listener, IP_ADDR_ANY, ORACLE_PORT) != ERR_OK) {
        debugPrint("bind failed\n");
        return 1;
    }
    if (netconn_listen(listener) != ERR_OK) {
        debugPrint("listen failed\n");
        return 1;
    }

    for (;;) {
        struct netconn *client = NULL;
        if (netconn_accept(listener, &client) == ERR_OK) {
            handle_client(client);
        }
    }

    netconn_delete(listener);
    return 0;
}
