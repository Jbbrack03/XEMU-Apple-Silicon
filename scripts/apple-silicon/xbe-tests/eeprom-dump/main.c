/*
 * xbox-oracle eeprom-dump
 *
 * Reads the Original Xbox EEPROM and writes (via D:\ — the only drive
 * letter the kernel auto-maps for a launched XBE; D:\ resolves to the
 * XBE's parent directory, e.g. E:\XBMC4Gamers\Apps\eeprom-dump\):
 *
 *   D:\eeprom-fresh.bin   raw 256 bytes (RC4-encrypted region preserved)
 *   D:\eeprom-info.txt    decrypted identifying info + hex dump
 *   D:\xbe-ran-marker.txt liveness marker (proves the XBE actually ran)
 *
 * On completion (success or read failure), reboots back into the dashboard
 * boot chain so we can FTP the files off and continue.
 *
 * EEPROM is on the SMBus at 7-bit address 0x54 (0xA8 in 8-bit form). 256
 * single-byte reads via HalReadSMBusValue produce the raw image. The kernel
 * also exposes per-field decrypted access through ExQueryNonVolatileSetting,
 * which we use to write a human-readable info file alongside the raw blob.
 *
 * No SDK, no XBDM, no controller input required. Runs headless to completion.
 *
 * Source lives at scripts/apple-silicon/xbe-tests/eeprom-dump/ in the
 * xemu-fork tree.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define EEPROM_SMBUS_ADDR 0xA8
#define EEPROM_SIZE 256
/*
 * Use D:\ — when the kernel launches an XBE it auto-maps D: to the XBE's
 * parent directory. No symlink-creation gymnastics needed. For an XBE at
 * E:\XBMC4Gamers\Apps\eeprom-dump\default.xbe, D:\eeprom-fresh.bin lands
 * at E:\XBMC4Gamers\Apps\eeprom-dump\eeprom-fresh.bin — FTP-accessible.
 */
#define EEPROM_BIN "D:\\eeprom-fresh.bin"
#define EEPROM_TXT "D:\\eeprom-info.txt"
#define MARKER_TXT "D:\\xbe-ran-marker.txt"

static int read_eeprom(unsigned char *buf)
{
    for (unsigned int i = 0; i < EEPROM_SIZE; i++) {
        ULONG val = 0;
        NTSTATUS s = HalReadSMBusValue(EEPROM_SMBUS_ADDR, (UCHAR)i, FALSE, &val);
        if (!NT_SUCCESS(s)) {
            debugPrint("HalReadSMBusValue offset 0x%02x failed: 0x%08lx\n",
                       i, (unsigned long)s);
            return -1;
        }
        buf[i] = (unsigned char)(val & 0xFF);
    }
    return 0;
}

static void write_bin(const unsigned char *eeprom)
{
    FILE *f = fopen(EEPROM_BIN, "wb");
    if (!f) {
        debugPrint("fopen %s failed\n", EEPROM_BIN);
        return;
    }
    fwrite(eeprom, 1, EEPROM_SIZE, f);
    fclose(f);
    debugPrint("wrote %s (%d bytes)\n", EEPROM_BIN, EEPROM_SIZE);
}

static void query_hex(FILE *f, const char *label, ULONG idx, ULONG length)
{
    ULONG type = 0, result_len = 0;
    unsigned char value[64] = {0};
    if (length > sizeof(value)) length = sizeof(value);
    NTSTATUS s = ExQueryNonVolatileSetting(idx, &type, value, length, &result_len);
    fprintf(f, "%-16s ", label);
    if (NT_SUCCESS(s)) {
        for (ULONG i = 0; i < result_len; i++) fprintf(f, "%02X", value[i]);
        fprintf(f, "  [type=0x%lx, len=%lu]\r\n", type, result_len);
    } else {
        fprintf(f, "(query failed: 0x%08lx)\r\n", (unsigned long)s);
    }
}

static void query_ascii(FILE *f, const char *label, ULONG idx, ULONG length)
{
    ULONG type = 0, result_len = 0;
    char value[64] = {0};
    if (length > sizeof(value) - 1) length = sizeof(value) - 1;
    NTSTATUS s = ExQueryNonVolatileSetting(idx, &type, value, length, &result_len);
    fprintf(f, "%-16s ", label);
    if (NT_SUCCESS(s)) {
        if (result_len >= sizeof(value)) result_len = sizeof(value) - 1;
        value[result_len] = 0;
        fprintf(f, "%s\r\n", value);
    } else {
        fprintf(f, "(query failed: 0x%08lx)\r\n", (unsigned long)s);
    }
}

static void write_info(const unsigned char *eeprom)
{
    FILE *f = fopen(EEPROM_TXT, "w");
    if (!f) {
        debugPrint("fopen %s failed\n", EEPROM_TXT);
        return;
    }

    fprintf(f, "Xbox EEPROM dump\r\n");
    fprintf(f, "Source: xbox-oracle eeprom-dump XBE\r\n");
    fprintf(f, "EEPROM size: %d bytes (raw, encrypted region preserved)\r\n",
            EEPROM_SIZE);
    fprintf(f, "\r\n");
    fprintf(f, "Decrypted factory section (via ExQueryNonVolatileSetting):\r\n");
    query_ascii(f, "Serial number:",  XC_FACTORY_SERIAL_NUMBER, 16);
    query_hex  (f, "MAC address:",    XC_FACTORY_ETHERNET_ADDR, 6);
    query_hex  (f, "Online key:",     XC_FACTORY_ONLINE_KEY, 16);
    query_hex  (f, "AV region:",      XC_FACTORY_AV_REGION, 4);
    query_hex  (f, "Game region:",    XC_FACTORY_GAME_REGION, 4);

    fprintf(f, "\r\nDecrypted user section:\r\n");
    query_hex  (f, "Video setting:",  XC_VIDEO, 4);
    query_hex  (f, "Audio setting:",  XC_AUDIO, 4);
    query_hex  (f, "DVD region:",     XC_DVD_REGION, 4);
    query_hex  (f, "Language:",       XC_LANGUAGE, 4);

    fprintf(f, "\r\nRaw EEPROM (256 bytes, hex dump):\r\n");
    for (int i = 0; i < EEPROM_SIZE; i += 16) {
        fprintf(f, "%04X: ", i);
        for (int j = 0; j < 16; j++) fprintf(f, "%02X ", eeprom[i + j]);
        fprintf(f, " | ");
        for (int j = 0; j < 16; j++) {
            unsigned char c = eeprom[i + j];
            fprintf(f, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        fprintf(f, "\r\n");
    }
    fprintf(f, "\r\n# end\r\n");
    fclose(f);
    debugPrint("wrote %s\n", EEPROM_TXT);
}

int main(void)
{
    /* First action: drop a marker so we can prove the XBE ran even if later
     * steps fail. D: is auto-mapped to our XBE's directory by the kernel. */
    {
        FILE *m = fopen(MARKER_TXT, "w");
        if (m) {
            fprintf(m, "xbox-oracle eeprom-dump XBE started\r\n");
            fclose(m);
        }
    }

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("xbox-oracle eeprom-dump\n");

    unsigned char eeprom[EEPROM_SIZE];
    memset(eeprom, 0, sizeof(eeprom));
    if (read_eeprom(eeprom) != 0) {
        debugPrint("EEPROM read failed; rebooting in 30s\n");
        Sleep(30000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }
    write_bin(eeprom);
    write_info(eeprom);

    debugPrint("done. rebooting in 5s...\n");
    Sleep(5000);
    HalReturnToFirmware(HalRebootRoutine);
    return 0;
}
