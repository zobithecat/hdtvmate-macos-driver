/*
 * extended_repeater_brute - exhaustive search for the I2C repeater register
 *
 * The original repeater_brute tried 8 hand-picked (reg, val) pairs.
 * After the chip got stuck (our 0xF424 wrap experiments) those don't work
 * anymore. This sweeps every (reg, val) in bank 0 of SLVT/SLVX looking
 * for any combination that makes 0xC0 respond with something other than
 * 0xC1 (NACK echo).
 *
 * For each (reg, val) we:
 *   1. SLVT bank 0, reg = val
 *   2. SLVX reg = val
 *   3. Read 1 byte from 0xC0 reg 0x7F (tuner ID)
 *   4. If response != 0xC1: log as HIT
 *   5. SLVT reg = 0, SLVX reg = 0
 */

#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);

int main(void)
{
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;

    g_log_level = LOG_WARN;

    if (usb_init(&usb) != HDTVMATE_OK) return 1;
    if (usb_auto_detect(&usb) != HDTVMATE_OK) {
        fprintf(stderr, "device not found\n");
        return 1;
    }
    if (usb_discover_endpoints(&usb) != HDTVMATE_OK) return 1;
    if (it9300_initialize(&bridge, &usb) != HDTVMATE_OK) return 1;
    cxd6801_create(&demod, &bridge, 0);
    cxd6801_initialize(&demod);

    printf("=== Extended Repeater Brute Force ===\n");
    printf("Sweeping SLVT bank 0 reg 0x00..0xFF, val 0x01\n");

    int total_hits = 0;
    for (int reg = 0; reg <= 0xFF; reg++) {
        uint8_t val = 0x01;
        uint8_t tx[8], rx;

        /* SLVT bank 0, reg = val */
        cxd6801_i2c_write_one(&demod.i2c_demod, 0x00, (uint8_t)reg, val);

        /* SLVX reg = val */
        tx[0] = 2; tx[1] = 3; tx[2] = 0xDC; tx[3] = (uint8_t)reg; tx[4] = val;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);

        /* Read 0xC0 reg 0x7F */
        tx[0] = 1; tx[1] = 3; tx[2] = 0xC0; tx[3] = 0x7F;
        br_cmd_send(&bridge, 0x002B, tx, 4, NULL, 0);
        rx = 0xC1;
        tx[0] = 1; tx[1] = 3; tx[2] = 0xC0;
        br_cmd_send(&bridge, 0x002A, tx, 3, &rx, 1);

        if (rx != 0xC1 && rx != 0x00) {
            printf("  *** HIT! reg 0x%02x = 0x%02x → 0xC0 = 0x%02x ***\n",
                   reg, val, rx);
            total_hits++;
        }

        /* Disable */
        cxd6801_i2c_write_one(&demod.i2c_demod, 0x00, (uint8_t)reg, 0x00);
        tx[0] = 2; tx[1] = 3; tx[2] = 0xDC; tx[3] = (uint8_t)reg; tx[4] = 0x00;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
    }

    printf("\nTotal hits: %d\n", total_hits);

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
