/*
 * atsc1_long_test - 30s lock register polling for ATSC 1.0 channels.
 *
 * Usage: ./atsc1_long_test <freq_khz>
 *
 * Tunes once, then polls F.11, 9.62, D.86 every 200ms for 30s.
 * Shows histogram + reports any lock event.
 */
#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <freq_khz> [duration_sec=30] [retune_interval=0]\n", argv[0]);
        return 1;
    }
    uint32_t freq = (uint32_t)strtoul(argv[1], NULL, 10);
    int duration_sec = (argc >= 3) ? atoi(argv[2]) : 30;
    int retune_interval = (argc >= 4) ? atoi(argv[3]) : 0;

    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;

    g_log_level = LOG_WARN;

    if (usb_init(&usb) != HDTVMATE_OK) return 1;
    if (usb_auto_detect(&usb) != HDTVMATE_OK) return 1;
    if (usb_discover_endpoints(&usb) != HDTVMATE_OK) return 1;
    if (it9300_initialize(&bridge, &usb) != HDTVMATE_OK) return 1;
    cxd6801_create(&demod, &bridge, 0);
    cxd6801_initialize(&demod);

    printf("Tuning to %u kHz (ATSC 1.0)...\n", freq);
    if (cxd6801_atsc1_tune(&demod, freq) != HDTVMATE_OK) {
        fprintf(stderr, "atsc1_tune failed\n");
        return 1;
    }

    int n_samples = duration_sec * 5;  /* 200ms interval = 5 per sec */
    printf("\nPolling lock registers for %ds (200ms interval, %d samples", duration_sec, n_samples);
    if (retune_interval > 0) printf(", retune every %ds", retune_interval);
    printf(")...\n");
    printf("%-6s | %-6s %-7s %-7s | %-12s\n",
           "ms", "F.11", "9.62", "D.86", "interpretation");

    int locked_count = 0;
    int unlock0_count = 0;  /* unlock_det=0 = closer to lock */
    int total = 0;
    uint8_t hist_962[256] = {0};

    cxd6801_i2c_t slvr;
    cxd6801_i2c_init(&slvr, demod.bridge, demod.i2c_demod.chip_idx,
                     0x30, demod.i2c_demod.i2c_bus);

    int retune_at_sample = retune_interval * 5;
    for (int i = 0; i < n_samples; i++) {
        /* Periodic retune to kick demod state machine */
        if (retune_interval > 0 && i > 0 && (i % retune_at_sample) == 0) {
            printf("%-6d | --- RETUNE ---\n", i * 200);
            cxd6801_atsc1_tune(&demod, freq);
        }
        uint8_t r_F11 = 0, r_962 = 0, r_D86 = 0;

        cxd6801_i2c_read(&slvr, 0x0F, 0x11, &r_F11, 1);
        cxd6801_i2c_read(&slvr, 0x09, 0x62, &r_962, 1);
        cxd6801_i2c_read(&slvr, 0x0D, 0x86, &r_D86, 1);

        uint8_t ts_valid   = r_F11 & 0x01;
        uint8_t unlock_det = (r_962 >> 4) & 0x01;
        uint8_t ts_lock    = r_D86 & 0x01;
        bool locked = (unlock_det == 0) && (ts_lock != 0);

        const char *interp;
        if (locked) interp = "*** LOCKED ***";
        else if (unlock_det == 0) interp = "potential";
        else interp = "no signal";

        if (locked) locked_count++;
        if (unlock_det == 0) unlock0_count++;
        hist_962[r_962]++;
        total++;

        if (i < 10 || locked || (i % 25 == 0)) {
            printf("%-6d | 0x%02x   0x%02x    0x%02x    | %s%s%s%s\n",
                   i * 200, r_F11, r_962, r_D86,
                   ts_valid ? "ts_valid " : "",
                   ts_lock ? "ts_lock " : "",
                   unlock_det ? "unlock " : "lock-ok ",
                   interp);
        }

        usleep(200000);
    }

    printf("\n=== Summary ===\n");
    printf("Total samples: %d\n", total);
    printf("Locked: %d (%.1f%%)\n", locked_count, 100.0*locked_count/total);
    printf("unlock_det=0 (potential): %d (%.1f%%)\n",
           unlock0_count, 100.0*unlock0_count/total);
    printf("\n9.62 register histogram (top 5):\n");
    for (int top = 0; top < 5; top++) {
        int max_v = 0, max_i = 0;
        for (int j = 0; j < 256; j++) {
            if (hist_962[j] > max_v) { max_v = hist_962[j]; max_i = j; }
        }
        if (max_v == 0) break;
        printf("  0x%02x: %d (%.1f%%)\n", max_i, max_v, 100.0*max_v/total);
        hist_962[max_i] = 0;
    }

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return locked_count > 0 ? 0 : 2;
}
