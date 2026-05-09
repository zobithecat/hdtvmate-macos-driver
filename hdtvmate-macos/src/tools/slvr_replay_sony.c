/*
 * slvr_replay_sony — replay Sony's EXACT i2c=0x98 sequence + verify chip
 * actually responds vs bridge fake-ACK.
 *
 * Sony's pattern (from /tmp/sony_brcmd.log):
 *   1. write [02 03 98 00 01]      (proxy reg 0x00 = 0x01)
 *   2. write [02 03 98 48 01]      (proxy reg 0x48 = 0x01)
 *   3. write [07 03 98 0a c5 a3 a1 77 ff 00]  (CMD 0xC5 packet to reg 0x0A)
 *   4. write [01 03 98 0a]         (set read pointer to 0x0A)
 *   5. read 6B from 0x98           (expect [30 00 00 00 00 c5] from chip)
 *
 * If write NACK BUT read returns valid Sony-pattern, chip is actually
 * unlocked. If read returns bridge fake-ACK (varies between calls), chip
 * is truly locked.
 */
#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <string.h>

extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);

int main(void)
{
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;
    hdtvmate_error_t ret;

    g_log_level = LOG_INFO;

    if (usb_init(&usb) != HDTVMATE_OK) return 1;
    if (usb_auto_detect(&usb) != HDTVMATE_OK) { fprintf(stderr, "no dev\n"); return 1; }
    if (usb_discover_endpoints(&usb) != HDTVMATE_OK) return 1;
    if (it9300_initialize(&bridge, &usb) != HDTVMATE_OK) return 1;
    if (cxd6801_create(&demod, &bridge, 0) != HDTVMATE_OK) return 1;
    if (cxd6801_initialize(&demod) != HDTVMATE_OK) return 1;

    printf("=== SLVR replay test ===\n");
    printf("Replay Sony's exact byte sequence to i2c=0x98 + verify chip response.\n\n");

    /* Step 1: proxy_init reg 0x00 = 0x01 */
    {
        uint8_t tx[] = { 2, 0x03, 0x98, 0x00, 0x01 };
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("[1] write 0x98 reg 0x00 = 0x01: ret=%d\n", ret);
    }
    /* Step 2: proxy_init reg 0x48 = 0x01 */
    {
        uint8_t tx[] = { 2, 0x03, 0x98, 0x48, 0x01 };
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("[2] write 0x98 reg 0x48 = 0x01: ret=%d\n", ret);
    }
    /* Step 3: 6-byte CMD 0xC5 packet to reg 0x0A */
    {
        uint8_t tx[] = { 7, 0x03, 0x98, 0x0A, 0xC5, 0xA3, 0xA1, 0x77, 0xFF, 0x00 };
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("[3] write 0x98 reg 0x0A 6-byte CMD: ret=%d\n", ret);
    }
    /* Step 4: set read pointer to 0x0A */
    {
        uint8_t tx[] = { 1, 0x03, 0x98, 0x0A };
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("[4] write 0x98 reg 0x0A (set ptr): ret=%d\n", ret);
    }
    /* Step 5: read 6 bytes from 0x98 */
    {
        uint8_t tx[] = { 6, 0x03, 0x98 };
        uint8_t resp[6] = {0};
        ret = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_RD, tx, sizeof(tx), resp, 6);
        printf("[5] read 0x98 (6 bytes): ret=%d data=[%02x %02x %02x %02x %02x %02x]\n",
               ret, resp[0], resp[1], resp[2], resp[3], resp[4], resp[5]);
        printf("    Sony got: [30 00 00 00 00 c5]\n");
        printf("    %s\n",
               (resp[0] == 0x30 && resp[5] == 0xC5) ? "✓ CHIP RESPONDED CORRECTLY"
                                                    : "✗ chip silent (bridge fake-ACK)");
    }

    /* Same-reg-twice: read 6 bytes again and check if same value */
    printf("\n[6] verify chip vs bridge fake-ACK — read 6B twice:\n");
    {
        uint8_t tx[] = { 6, 0x03, 0x98 };
        uint8_t r1[6] = {0}, r2[6] = {0}, r3[6] = {0};
        br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_RD, tx, sizeof(tx), r1, 6);
        br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_RD, tx, sizeof(tx), r2, 6);
        br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_RD, tx, sizeof(tx), r3, 6);
        printf("  read1: %02x %02x %02x %02x %02x %02x\n", r1[0],r1[1],r1[2],r1[3],r1[4],r1[5]);
        printf("  read2: %02x %02x %02x %02x %02x %02x\n", r2[0],r2[1],r2[2],r2[3],r2[4],r2[5]);
        printf("  read3: %02x %02x %02x %02x %02x %02x\n", r3[0],r3[1],r3[2],r3[3],r3[4],r3[5]);
        bool same = (memcmp(r1, r2, 6) == 0) && (memcmp(r2, r3, 6) == 0);
        printf("  %s\n", same ? "✓ SAME (real chip data)" : "✗ DIFFERENT (bridge seq echo)");
    }

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
