/*
 * scan_full — full ATSC scan, channels 2-51, both ATSC 1.0 and ATSC 3.0
 *
 * For each channel & mode:
 *   1. atsc1_tune or atsc3_tune
 *   2. wait 800ms for acquisition
 *   3. read lock register (bank 0x90 reg 0x10 for ATSC3, bank 0x20 reg 0x10 for ATSC1)
 *   4. report sync_stat + SNR
 */

#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include "channel_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
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

    int locked_count = 0;
    int total_attempts = 0;

    printf("=== Full ATSC Scan: US channels 2-51 + Korean UHF (701-803 MHz), ATSC 1.0 + 3.0 ===\n");
    printf("Ch | Freq    | Mode    | LockReg | sync | SNR (x100) | RF (dBm) | Result\n");
    printf("---+---------+---------+---------+------+------------+----------+--------\n");

    /* Build list of frequencies to scan: US channels 2-51 plus Korean UHF
     * (701, 707, ..., 803 MHz) which falls outside the US channel plan but
     * is where Korean ATSC 3.0 broadcasts live. */
    uint32_t freqs[100];
    int freq_count = 0;
    for (int ch = 2; ch <= 51; ch++) {
        uint32_t f = frequency_for_channel((uint8_t)ch);
        if (f != 0) freqs[freq_count++] = f;
    }
    /* Korean ATSC 3.0 UHF (channels 52-69 in old US plan = 698-806 MHz) */
    for (uint32_t f = 701000; f <= 803000 && freq_count < 100; f += 6000) {
        freqs[freq_count++] = f;
    }

    for (int idx = 0; idx < freq_count; idx++) {
        uint32_t freq = freqs[idx];
        int ch = idx + 2;  /* synthetic channel number for output */

        for (int mode = 0; mode < 2; mode++) {
            const char *mode_str = (mode == 0) ? "ATSC1.0" : "ATSC3.0";
            total_attempts++;

            hdtvmate_error_t ret;
            if (mode == 0) {
                ret = cxd6801_atsc1_tune(&demod, freq);
            } else {
                ret = cxd6801_atsc3_tune(&demod, freq, CXD6801_BW_6MHZ);
            }
            if (ret != HDTVMATE_OK) {
                printf("%2d | %7d | %s | TUNE FAIL\n", ch, freq, mode_str);
                continue;
            }

            usleep(800000);  /* 800ms for acquisition */

            uint8_t lock_reg = 0, alp_reg = 0;
            if (mode == 0) {
                cxd6801_i2c_read(&demod.i2c_demod, 0x20, 0x10, &lock_reg, 1);  /* ATSC1 lock */
            } else {
                cxd6801_i2c_read(&demod.i2c_demod, 0x90, 0x10, &lock_reg, 1);  /* ATSC3 sync_stat */
                cxd6801_i2c_read(&demod.i2c_demod, 0x95, 0x40, &alp_reg, 1);   /* ATSC3 ALP lock */
            }

            int32_t snr_x100 = 0, rf_dbm = 0;
            cxd6801_monitor_snr(&demod, &snr_x100);
            cxd6801_monitor_rf_level(&demod, &rf_dbm);

            uint8_t sync_stat = lock_reg & 0x07;
            uint8_t unlock_det = (lock_reg >> 4) & 0x01;

            const char *result = "no signal";
            if (mode == 1) {
                if (sync_stat >= 6 && !unlock_det) {
                    result = "*** LOCKED ***";
                    locked_count++;
                } else if (sync_stat >= 2) {
                    result = "acquiring";
                } else if (sync_stat == 1) {
                    result = "bootstrap";
                }
            } else {
                /* ATSC1 lock semantics differ */
                if (lock_reg & 0x80) {
                    result = "*** LOCKED ***";
                    locked_count++;
                }
            }

            printf("%2d | %7d | %s | %02x      | %d    | %4d.%02d    | %4d     | %s\n",
                   ch, freq, mode_str, lock_reg, sync_stat,
                   snr_x100 / 100, abs(snr_x100) % 100, rf_dbm, result);
            fflush(stdout);
        }
    }

    printf("---+---------+---------+---------+------+------------+----------+--------\n");
    printf("\n=== Summary: %d/%d locked ===\n", locked_count, total_attempts);

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
