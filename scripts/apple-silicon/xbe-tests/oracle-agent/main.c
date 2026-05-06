/*
 * xbox-oracle-agent v0.1
 *
 * A network-listening XBE that exposes the Xbox console as an oracle
 * for xemu correctness validation. Speaks a simple text-line RPC
 * protocol on TCP port 9001.
 *
 * Phase 1 commands (this build):
 *   info                   - agent + Xbox info (single-line)
 *   eeprom                 - 256-byte EEPROM as hex (multi-line)
 *   reboot                 - reboot Xbox (no response, conn drops)
 *   bye                    - close connection
 *
 * Response format (XBDM/SMTP-inspired):
 *   200- <text>\r\n        single-line success
 *   201- OK\r\n            multi-line begin; lines follow; ends with .\r\n
 *   500- <text>\r\n        error
 *
 * Built with nxdk; lwip TCP via the same pattern as the httpd sample.
 * Source lives at scripts/apple-silicon/xbe-tests/oracle-agent/ in
 * the xemu-fork tree (per project rule #5: in-tree tooling).
 */
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

#define ORACLE_PORT       9001
#define EEPROM_SMBUS_ADDR 0xA8
#define EEPROM_SIZE       256
#define VERSION_STR       "xbox-oracle-agent v0.1 (Phase 1: info/eeprom/reboot/bye)"

extern struct netif *g_pnetif;

static int read_eeprom(unsigned char *out)
{
    for (unsigned int i = 0; i < EEPROM_SIZE; i++) {
        ULONG val = 0;
        NTSTATUS s = HalReadSMBusValue(EEPROM_SMBUS_ADDR, (UCHAR)i, FALSE, &val);
        if (!NT_SUCCESS(s)) return -1;
        out[i] = (unsigned char)(val & 0xFF);
    }
    return 0;
}

static void send_line(struct netconn *c, const char *s)
{
    netconn_write(c, s, strlen(s), NETCONN_COPY);
    netconn_write(c, "\r\n", 2, NETCONN_NOCOPY);
}

static void cmd_info(struct netconn *c)
{
    char line[256];
    snprintf(line, sizeof(line),
             "200- %s; ip=%s",
             VERSION_STR,
             ip4addr_ntoa(netif_ip4_addr(g_pnetif)));
    send_line(c, line);
}

static void cmd_eeprom(struct netconn *c)
{
    unsigned char eeprom[EEPROM_SIZE];
    memset(eeprom, 0, sizeof(eeprom));
    if (read_eeprom(eeprom) != 0) {
        send_line(c, "500- HalReadSMBusValue failed");
        return;
    }
    send_line(c, "201- OK 256");
    /* Hex-encode 32 bytes per line for readability */
    char hex[64 * 2 + 4];
    for (int row = 0; row < EEPROM_SIZE; row += 32) {
        int p = 0;
        for (int j = 0; j < 32 && (row + j) < EEPROM_SIZE; j++) {
            p += snprintf(hex + p, sizeof(hex) - p, "%02X", eeprom[row + j]);
        }
        send_line(c, hex);
    }
    send_line(c, ".");
}

static int cmd_dispatch(struct netconn *c, char *cmd)
{
    /* Trim trailing whitespace */
    int n = (int)strlen(cmd);
    while (n > 0 && (cmd[n - 1] == '\r' || cmd[n - 1] == '\n' ||
                     cmd[n - 1] == ' '  || cmd[n - 1] == '\t')) {
        cmd[--n] = 0;
    }

    debugPrint("rcv: %s\n", cmd);

    if (strcmp(cmd, "info") == 0) {
        cmd_info(c);
        return 0;
    }
    if (strcmp(cmd, "eeprom") == 0) {
        cmd_eeprom(c);
        return 0;
    }
    if (strcmp(cmd, "bye") == 0) {
        send_line(c, "200- bye");
        return 1; /* drop connection */
    }
    if (strcmp(cmd, "reboot") == 0) {
        send_line(c, "200- rebooting");
        netconn_close(c);
        Sleep(500);
        HalReturnToFirmware(HalRebootRoutine);
        /* not reached */
        return 1;
    }
    if (strcmp(cmd, "") == 0) {
        return 0;
    }
    char err[128];
    snprintf(err, sizeof(err), "500- unknown command: %s", cmd);
    send_line(c, err);
    return 0;
}

static void handle_client(struct netconn *c)
{
    debugPrint("client connected\n");
    send_line(c, "200- xbox-oracle-agent ready");

    char buf[1024];
    int blen = 0;

    while (1) {
        struct netbuf *inbuf;
        if (netconn_recv(c, &inbuf) != ERR_OK) break;

        char *data;
        u16_t len;
        netbuf_data(inbuf, (void **)&data, &len);

        for (u16_t i = 0; i < len; i++) {
            char ch = data[i];
            if (ch == '\n') {
                buf[blen] = 0;
                if (cmd_dispatch(c, buf)) {
                    netbuf_delete(inbuf);
                    netconn_close(c);
                    netconn_delete(c);
                    return;
                }
                blen = 0;
            } else if (blen < (int)sizeof(buf) - 1) {
                buf[blen++] = ch;
            } else {
                /* line too long; reset */
                blen = 0;
            }
        }
        netbuf_delete(inbuf);
    }
    netconn_close(c);
    netconn_delete(c);
    debugPrint("client disconnected\n");
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("\n%s\n", VERSION_STR);
    debugPrint("Bringing up network...\n");

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

    while (1) {
        struct netconn *client;
        if (netconn_accept(listener, &client) == ERR_OK) {
            handle_client(client);
        }
    }

    netconn_delete(listener);
    return 0;
}
