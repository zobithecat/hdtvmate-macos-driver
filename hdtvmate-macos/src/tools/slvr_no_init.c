/*
 * slvr_no_init — open USB, do NOTHING, attempt 0x98 immediately.
 *
 * Tests hypothesis: chip retains SLVR-unlocked state from prior session
 * (e.g. Sony's app on UTM). If macOS USB enum doesn't reset chip, our
 * 0x98 access should ACK without any init.
 *
 * If still NACK → chip IS being reset by macOS USB enum.
 * If ACK → chip retains state; our it9300_init RESETS it incorrectly.
 */
#include "usb_device.h"
#include "it9300.h"
#include <stdio.h>
#include <string.h>

extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);

int main(void)
{
    usb_device_t usb;
    it9300_device_t bridge;

    g_log_level = LOG_INFO;

    if (usb_init(&usb) != HDTVMATE_OK) return 1;
    if (usb_auto_detect(&usb) != HDTVMATE_OK) { fprintf(stderr, "no dev\n"); return 1; }
    if (usb_discover_endpoints(&usb) != HDTVMATE_OK) return 1;

    /* DO NOT call it9300_initialize. Just set up minimal struct. */
    memset(&bridge, 0, sizeof(bridge));
    bridge.usb = &usb;
    bridge.cmd_seq = 0;

    printf("=== SLVR no-init test (chip state from previous session) ===\n");
    printf("USB opened, NO chip-init. Attempting 0x98 write directly.\n\n");

    /* Step 1: writes to 0x98 directly */
    {
        uint8_t tx[] = { 2, 0x03, 0x98, 0x00, 0x01 };
        hdtvmate_error_t r = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("[1] write 0x98 reg 0x00 = 0x01: ret=%d\n", r);
    }

    {
        uint8_t tx[] = { 2, 0x03, 0x98, 0x48, 0x01 };
        hdtvmate_error_t r = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, tx, sizeof(tx), NULL, 0);
        printf("[2] write 0x98 reg 0x48 = 0x01: ret=%d\n", r);
    }

    /* Same-reg-twice on read */
    printf("\n[3] read 0x98 (1B) twice:\n");
    {
        uint8_t set_ptr[] = { 1, 0x03, 0x98, 0x0A };
        br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, set_ptr, sizeof(set_ptr), NULL, 0);
        uint8_t rd[] = { 1, 0x03, 0x98 };
        uint8_t r1 = 0, r2 = 0;
        br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_RD, rd, sizeof(rd), &r1, 1);
        br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_RD, rd, sizeof(rd), &r2, 1);
        printf("  read1: 0x%02x  read2: 0x%02x  %s\n", r1, r2,
               (r1 == r2) ? "(same)" : "(different)");
    }

    /* And test SLVT for comparison */
    printf("\n[4] SLVT (0xD8) read for comparison:\n");
    {
        uint8_t bk[] = { 2, 0x03, 0xD8, 0x00, 0x00 };  /* bank 0 */
        br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, bk, sizeof(bk), NULL, 0);
        uint8_t set_ptr[] = { 1, 0x03, 0xD8, 0xFB };
        br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_WR, set_ptr, sizeof(set_ptr), NULL, 0);
        uint8_t rd[] = { 1, 0x03, 0xD8 };
        uint8_t v = 0;
        hdtvmate_error_t r = br_cmd_send(&bridge, IT9300_CMD_GENERIC_I2C_RD, rd, sizeof(rd), &v, 1);
        printf("  SLVT 0xFB: ret=%d val=0x%02x %s\n", r, v,
               (v == 0xDC || v == 0x6e || v == 0x39) ? "(known chip-id pattern)" : "");
    }

    usb_close(&usb);
    return 0;
}
