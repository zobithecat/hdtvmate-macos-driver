/*
 * snr_monitor — real-time SNR / sync_stat monitor for antenna tuning
 *
 * Tunes once, then polls every 500ms forever. Prints a single line
 * per poll: sync_stat, SNR raw + dB-ish, lock indicator.
 *
 * Usage:
 *   ./snr_monitor 707000        # 707 MHz
 *   ./snr_monitor 701000        # 701 MHz
 *   Ctrl+C to quit.
 *
 * What to look for while moving the antenna:
 *   - sync_stat 6 = OFDM lock acquired
 *   - SNR raw bigger = stronger signal (uncalibrated; relative comparison only)
 *   - "ALP" indicator = ATSC 3.0 ALP-layer lock (rare; only with strong RF)
 */
#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile int g_running = 1;
static void sigint_handler(int sig) { (void)sig; g_running = 0; }

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <freq_khz>\n", argv[0]);
        return 1;
    }
    uint32_t freq = (uint32_t)atol(argv[1]);

    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;

    g_log_level = LOG_ERROR;  /* quiet — we print our own status line */

    if (usb_init(&usb) != HDTVMATE_OK) { fprintf(stderr, "usb_init failed\n"); return 1; }
    if (usb_auto_detect(&usb) != HDTVMATE_OK) { fprintf(stderr, "device not found\n"); return 1; }
    if (usb_discover_endpoints(&usb) != HDTVMATE_OK) return 1;
    if (it9300_initialize(&bridge, &usb) != HDTVMATE_OK) return 1;
    cxd6801_create(&demod, &bridge, 0);
    cxd6801_initialize(&demod);

    signal(SIGINT, sigint_handler);

    fprintf(stderr, "Tuning to %u kHz... (Ctrl+C to quit)\n", freq);
    if (cxd6801_atsc3_tune(&demod, freq, CXD6801_BW_6MHZ) != HDTVMATE_OK) {
        fprintf(stderr, "tune failed\n");
        return 1;
    }

    fprintf(stderr,
        "%-8s | %-8s | %-12s | %-8s | %s\n",
        "time(s)", "syncStat", "SNRraw(0x)", "SNR~dB", "status");
    fprintf(stderr,
        "---------+----------+--------------+----------+------------\n");

    int t = 0;
    while (g_running) {
        uint8_t lock = 0;
        cxd6801_i2c_read(&demod.i2c_demod, 0x90, 0x10, &lock, 1);
        uint8_t sync = lock & 0x07;
        uint8_t unlock_det = (lock >> 4) & 0x01;

        /* Read raw 24-bit SNR (bank 0x90 reg 0x28) */
        uint8_t snr_data[3] = {0};
        cxd6801_i2c_read(&demod.i2c_demod, 0x90, 0x28, snr_data, 3);
        uint32_t snr_raw = ((uint32_t)snr_data[0] << 16) |
                           ((uint32_t)snr_data[1] << 8)  |
                           snr_data[2];
        /* Rough dB scale: log2(raw)*3 — purely relative, NOT calibrated. */
        int snr_log = 0;
        for (uint32_t v = snr_raw; v > 1; v >>= 1) snr_log += 3;

        const char *status;
        if (unlock_det)        status = "UNLOCK";
        else if (sync == 6)    status = "*** LOCKED ***";
        else if (sync >= 4)    status = "near-lock";
        else if (sync >= 2)    status = "acquiring";
        else                   status = "no signal";

        fprintf(stderr, "%-8d | %-8d | 0x%08x   | %-8d | %s\n",
                t, sync, snr_raw, snr_log, status);
        fflush(stderr);

        sleep(1);
        t++;
    }

    fprintf(stderr, "\nStopped.\n");
    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
