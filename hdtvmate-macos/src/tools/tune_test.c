/*
 * tune_test.c - Minimal ASCOT3 tune + demod lock test
 *
 * Tests the tuning sequence without capture:
 *   1. USB init → IT9300 init → CXD6801 init
 *   2. ASCOT3 tune to specified frequency
 *   3. Poll demod lock for 5 seconds
 *   4. Report result
 *
 * Usage:
 *   ./hdtvmate_tune_test 473000          # UHF ch14, ATSC 3.0
 *   ./hdtvmate_tune_test 473000 --atsc1  # UHF ch14, ATSC 1.0
 *   ./hdtvmate_tune_test --channel 14    # Same as 473000
 */

#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include "channel_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;
    hdtvmate_error_t ret;

    g_log_level = LOG_DEBUG;  /* Verbose by default for testing */

    uint32_t frequency_khz = 0;
    broadcast_standard_t std = BROADCAST_ATSC3;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            int ch = atoi(argv[++i]);
            frequency_khz = frequency_for_channel(ch);
        } else if (strcmp(argv[i], "--atsc1") == 0) {
            std = BROADCAST_ATSC1;
        } else if (strcmp(argv[i], "-v") == 0) {
            g_log_level = LOG_TRACE;
        } else if (argv[i][0] != '-') {
            frequency_khz = (uint32_t)atol(argv[i]);
        }
    }

    if (frequency_khz == 0) {
        frequency_khz = 473000;  /* Default: UHF ch14 */
        fprintf(stderr, "No frequency specified, using default 473000 kHz (ch14)\n");
    }

    fprintf(stderr, "=== ASCOT3 Tune Test ===\n");
    fprintf(stderr, "Frequency: %u kHz\n", frequency_khz);
    fprintf(stderr, "Standard:  %s\n", std == BROADCAST_ATSC3 ? "ATSC 3.0" : "ATSC 1.0");
    fprintf(stderr, "\n");

    /* ---- Step 1: USB ---- */
    fprintf(stderr, "[1/5] USB init...\n");
    ret = usb_init(&usb);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "FAIL: USB init (err=%d)\n", ret);
        return 1;
    }

    ret = usb_auto_detect(&usb);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "FAIL: Device not found. Is HDTV Mate plugged in?\n");
        usb_close(&usb);
        return 1;
    }

    ret = usb_discover_endpoints(&usb);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "FAIL: Endpoint discovery\n");
        usb_close(&usb);
        return 1;
    }
    fprintf(stderr, "  OK: USB device opened\n");

    /* ---- Step 2: IT9300 bridge ---- */
    fprintf(stderr, "[2/5] IT9300 bridge init...\n");
    ret = it9300_initialize(&bridge, &usb);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "FAIL: Bridge init (err=%d)\n", ret);
        usb_close(&usb);
        return 1;
    }
    fprintf(stderr, "  OK: IT9300 bridge ready\n");

    /* ---- Step 3: CXD6801 demod ---- */
    fprintf(stderr, "[3/5] CXD6801 demod init...\n");
    ret = cxd6801_create(&demod, &bridge, 0);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "FAIL: demod create (err=%d)\n", ret);
        goto cleanup;
    }

    ret = cxd6801_initialize(&demod);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "FAIL: demod init (err=%d)\n", ret);
        goto cleanup;
    }
    fprintf(stderr, "  OK: CXD6801 chip_id=0x%04x\n", demod.chip_id);

    /* ---- Step 4: Tune ---- */
    fprintf(stderr, "[4/5] Tuning to %u kHz...\n", frequency_khz);

    if (std == BROADCAST_ATSC3) {
        ret = cxd6801_atsc3_tune(&demod, frequency_khz, CXD6801_BW_6MHZ);
    } else {
        ret = cxd6801_atsc1_tune(&demod, frequency_khz);
    }

    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "FAIL: Tune failed (err=%d)\n", ret);
        goto cleanup;
    }
    fprintf(stderr, "  OK: Tune command sent\n");

    /* ---- Step 5: Wait for lock ---- */
    fprintf(stderr, "[5/5] Waiting for demod lock (5s timeout)...\n");

    bool locked = false;
    for (int i = 0; i < 100; i++) {  /* 100 × 50ms = 5s */
        if (std == BROADCAST_ATSC3) {
            cxd6801_atsc3_check_demod_lock(&demod, &locked);
        } else {
            cxd6801_atsc1_check_lock(&demod, &locked);
        }

        if (locked) {
            fprintf(stderr, "  *** LOCKED at %d ms! ***\n", (i + 1) * 50);
            break;
        }

        if (i % 20 == 19) {
            fprintf(stderr, "  ... still waiting (%d ms)\n", (i + 1) * 50);
        }
        usleep(50000);
    }

    if (!locked) {
        fprintf(stderr, "  NO LOCK after 5 seconds.\n");
        fprintf(stderr, "\n  This is expected if:\n");
        fprintf(stderr, "    - No antenna connected\n");
        fprintf(stderr, "    - No signal at %u kHz in your area\n", frequency_khz);
        fprintf(stderr, "    - Demod register init sequence incomplete (TODO)\n");
        fprintf(stderr, "\n  Next debug step: read lock status register raw values\n");

        /* Read some diagnostic registers */
        uint8_t status = 0;
        cxd6801_i2c_read(&demod.i2c_demod, 0x91, 0x10, &status, 1);
        fprintf(stderr, "  Lock reg (bank=0x91, reg=0x10): 0x%02x\n", status);

        cxd6801_i2c_read(&demod.i2c_demod, 0x00, 0x10, &status, 1);
        fprintf(stderr, "  State reg (bank=0x00, reg=0x10): 0x%02x\n", status);
    } else {
        /* Read signal stats */
        signal_stats_t stats;
        cxd6801_get_stats(&demod, &stats);
        fprintf(stderr, "\n  === Signal Info ===\n");
        fprintf(stderr, "  SNR: %d.%02d dB\n",
                stats.snr_db_x100 / 100, abs(stats.snr_db_x100) % 100);
        fprintf(stderr, "  RF Level: %d dBm\n", stats.rf_level_dbm);

        /* Check ALP lock for ATSC 3.0 */
        if (std == BROADCAST_ATSC3) {
            bool alp_locked = false;
            for (int i = 0; i < 40; i++) {
                cxd6801_atsc3_check_alp_lock(&demod, &alp_locked);
                if (alp_locked) {
                    fprintf(stderr, "  ALP locked at %d ms after demod lock\n", (i + 1) * 50);
                    break;
                }
                usleep(50000);
            }
            if (!alp_locked) {
                fprintf(stderr, "  ALP not locked (2s timeout)\n");
            }
        }
    }

    fprintf(stderr, "\n=== Test complete ===\n");

cleanup:
    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return (locked) ? 0 : 1;
}
