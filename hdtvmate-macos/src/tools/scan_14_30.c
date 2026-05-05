/*
 * scan_14_30 — quick UHF sweep ch14..ch30 with current ATSC3+ATSC1-AGC config.
 *
 * For each channel:
 *   1. atsc3_tune (uses ATSC1 g_param_table entry [9] currently)
 *   2. wait 1s
 *   3. read Lock reg + sync_stat + ALP lock
 *   4. log result
 *
 * Helps find which channel(s) have strongest signal at this antenna.
 */

#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include "channel_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;

    g_log_level = LOG_WARN;  /* quiet */

    if (usb_init(&usb) != HDTVMATE_OK) return 1;
    if (usb_auto_detect(&usb) != HDTVMATE_OK) {
        fprintf(stderr, "device not found\n");
        return 1;
    }
    if (usb_discover_endpoints(&usb) != HDTVMATE_OK) return 1;
    if (it9300_initialize(&bridge, &usb) != HDTVMATE_OK) return 1;
    cxd6801_create(&demod, &bridge, 0);
    cxd6801_initialize(&demod);

    printf("=== Channel sweep 14-30 (ATSC 3.0 mode + current g_param_table) ===\n\n");
    printf("ch | freq(kHz) | Lock reg | sync | unlock | ALP\n");
    printf("---+-----------+----------+------+--------+----\n");

    for (int ch = 14; ch <= 30; ch++) {
        uint32_t freq = frequency_for_channel((uint8_t)ch);
        if (freq == 0) continue;

        /* Tune */
        hdtvmate_error_t ret = cxd6801_atsc3_tune(&demod, freq, CXD6801_BW_6MHZ);
        if (ret != HDTVMATE_OK) {
            printf("%2d | %7d   | TUNE FAIL\n", ch, freq);
            continue;
        }

        /* Give chip 1s to attempt acquisition */
        usleep(1000000);

        /* Read status registers */
        uint8_t lock90 = 0, lock91 = 0, alp = 0;
        cxd6801_i2c_read(&demod.i2c_demod, 0x90, 0x10, &lock90, 1);
        cxd6801_i2c_read(&demod.i2c_demod, 0x91, 0x10, &lock91, 1);
        cxd6801_i2c_read(&demod.i2c_demod, 0x95, 0x40, &alp, 1);

        uint8_t sync_stat = lock90 & 0x07;
        uint8_t unlock = (lock90 >> 4) & 0x01;
        uint8_t alp_locked = (alp >> 4) & 0x01;

        const char *flag = "";
        if (sync_stat >= 6) flag = " ★ LOCKED!";
        else if (sync_stat >= 3) flag = " ← partial";
        else if (sync_stat >= 1) flag = " (bootstrap)";

        printf("%2d | %7d   | 90:%02x 91:%02x | %d    | %d      | %d%s\n",
               ch, freq, lock90, lock91, sync_stat, unlock, alp_locked, flag);
    }

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
