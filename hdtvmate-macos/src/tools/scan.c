#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include "channel_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * hdtvmate_scan - ATSC 3.0 / 1.0 channel scanner
 *
 * Usage:
 *   ./hdtvmate_scan [VID PID]           # Scan all (ATSC 3.0 + 1.0)
 *   ./hdtvmate_scan --atsc3             # ATSC 3.0 only
 *   ./hdtvmate_scan --atsc1             # ATSC 1.0 only
 *   ./hdtvmate_scan --channel 14        # Scan single channel
 */

static void on_channel_found(const channel_info_t *ch, void *user_data)
{
    (void)user_data;
    printf("  >>> CHANNEL FOUND: CH %d (%u kHz) %s",
           ch->channel_number, ch->frequency_khz,
           ch->standard == BROADCAST_ATSC3 ? "ATSC3" : "ATSC1");
    if (ch->stats.snr_db_x100 > 0) {
        printf(" SNR=%d.%02d dB", ch->stats.snr_db_x100 / 100,
               ch->stats.snr_db_x100 % 100);
    }
    if (ch->num_plps > 0) {
        printf(" PLPs=%d", ch->num_plps);
    }
    printf("\n");
}

static void on_progress(uint32_t current, uint32_t total, uint32_t freq_khz, void *user_data)
{
    (void)user_data;
    printf("  [%3u/%u] Scanning %u kHz...\r", current, total, freq_khz);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;
    hdtvmate_error_t ret;

    g_log_level = LOG_WARN;  /* Less verbose for scan */

    bool scan_atsc3 = true, scan_atsc1 = true;
    int single_channel = -1;
    uint16_t vid = 0, pid = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--atsc3") == 0) { scan_atsc3 = true; scan_atsc1 = false; }
        else if (strcmp(argv[i], "--atsc1") == 0) { scan_atsc1 = true; scan_atsc3 = false; }
        else if (strcmp(argv[i], "--channel") == 0 && i+1 < argc) { single_channel = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-v") == 0) { g_log_level = LOG_DEBUG; }
        else if (vid == 0 && argv[i][0] != '-') { vid = (uint16_t)strtol(argv[i], NULL, 16); }
        else if (pid == 0 && argv[i][0] != '-') { pid = (uint16_t)strtol(argv[i], NULL, 16); }
    }

    printf("=== GTMedia HDTV Mate Channel Scanner ===\n\n");

    /* Init hardware */
    ret = usb_init(&usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "USB init failed\n"); return 1; }

    ret = (vid && pid) ? usb_open(&usb, vid, pid) : usb_auto_detect(&usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "Device not found\n"); usb_close(&usb); return 1; }

    ret = usb_discover_endpoints(&usb);
    if (ret != HDTVMATE_OK) { usb_close(&usb); return 1; }

    ret = it9300_initialize(&bridge, &usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "Bridge init failed\n"); usb_close(&usb); return 1; }

    ret = cxd6801_create(&demod, &bridge, 0);
    if (ret != HDTVMATE_OK) { it9300_deinit(&bridge); usb_close(&usb); return 1; }

    ret = cxd6801_initialize(&demod);
    if (ret != HDTVMATE_OK) { it9300_deinit(&bridge); usb_close(&usb); return 1; }

    printf("Hardware initialized. Starting scan...\n");
    printf("Scanning: %s%s%s\n\n",
           scan_atsc3 ? "ATSC 3.0" : "",
           (scan_atsc3 && scan_atsc1) ? " + " : "",
           scan_atsc1 ? "ATSC 1.0" : "");

    if (single_channel >= 0) {
        /* Scan single channel */
        uint32_t freq = frequency_for_channel(single_channel);
        if (freq == 0) {
            fprintf(stderr, "Invalid channel number: %d\n", single_channel);
        } else {
            channel_info_t info;
            if (scan_atsc3) {
                printf("Scanning CH %d (%u kHz) ATSC 3.0...\n", single_channel, freq);
                ret = channel_scan_single(&demod, freq, BROADCAST_ATSC3, &info);
                if (ret == HDTVMATE_OK && info.stats.locked) {
                    on_channel_found(&info, NULL);
                } else {
                    printf("  No ATSC 3.0 signal\n");
                }
            }
            if (scan_atsc1) {
                printf("Scanning CH %d (%u kHz) ATSC 1.0...\n", single_channel, freq);
                ret = channel_scan_single(&demod, freq, BROADCAST_ATSC1, &info);
                if (ret == HDTVMATE_OK && info.stats.locked) {
                    on_channel_found(&info, NULL);
                } else {
                    printf("  No ATSC 1.0 signal\n");
                }
            }
        }
    } else {
        /* Full scan */
        scan_config_t config = {
            .scan_atsc3 = scan_atsc3,
            .scan_atsc1 = scan_atsc1,
            .lock_timeout_ms = 2000,
            .on_channel_found = on_channel_found,
            .on_progress = on_progress,
            .user_data = NULL,
        };

        scan_results_t results;
        channel_scan(&demod, &config, &results);

        printf("\n\n=== Scan Results ===\n");
        printf("Channels found: %u\n\n", results.count);
        for (uint32_t i = 0; i < results.count; i++) {
            channel_info_t *ch = &results.channels[i];
            printf("  CH %-3d  %6u kHz  %-5s  SNR=%d.%02d dB",
                   ch->channel_number, ch->frequency_khz,
                   ch->standard == BROADCAST_ATSC3 ? "ATSC3" : "ATSC1",
                   ch->stats.snr_db_x100 / 100, ch->stats.snr_db_x100 % 100);
            if (ch->num_plps > 0) {
                printf("  PLPs=%d", ch->num_plps);
            }
            printf("\n");
        }
    }

    /* Cleanup */
    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);

    printf("\nDone.\n");
    return 0;
}
