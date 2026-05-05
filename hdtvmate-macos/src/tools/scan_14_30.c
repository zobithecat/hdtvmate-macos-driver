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

    int use_atsc1 = 1;  /* default to ATSC 1.0 sweep */
    printf("=== Channel sweep 14-30 (ATSC %s mode) ===\n\n",
           use_atsc1 ? "1.0" : "3.0");

    for (int ch = 14; ch <= 30; ch++) {
        uint32_t freq = frequency_for_channel((uint8_t)ch);
        if (freq == 0) continue;

        /* Tune */
        hdtvmate_error_t ret = use_atsc1
            ? cxd6801_atsc1_tune(&demod, freq)
            : cxd6801_atsc3_tune(&demod, freq, CXD6801_BW_6MHZ);
        if (ret != HDTVMATE_OK) {
            printf("%2d | %7d   | TUNE FAIL\n", ch, freq);
            continue;
        }

        /* Give chip 1s to attempt acquisition */
        usleep(1000000);

        /* Read several lock-status registers — values vary by mode */
        uint8_t r20 = 0, r91 = 0, r95 = 0;
        cxd6801_i2c_read(&demod.i2c_demod, 0x20, 0x10, &r20, 1);  /* ATSC1 lock */
        cxd6801_i2c_read(&demod.i2c_demod, 0x91, 0x10, &r91, 1);  /* ATSC3 lock */
        cxd6801_i2c_read(&demod.i2c_demod, 0x95, 0x40, &r95, 1);  /* ATSC3 ALP */

        /* SNR + RF level (works even without full lock) */
        int32_t snr_x100 = 0, rf_dbm = 0;
        cxd6801_monitor_snr(&demod, &snr_x100);
        cxd6801_monitor_rf_level(&demod, &rf_dbm);

        uint8_t sync = use_atsc1 ? (r20 & 0x07) : (r91 & 0x07);

        printf("%2d | %7d | r20=%02x r91=%02x r95=%02x | sync=%d | SNR=%2d.%02d dB | RF=%d dBm\n",
               ch, freq, r20, r91, r95, sync,
               snr_x100 / 100, abs(snr_x100) % 100, rf_dbm);
    }

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
