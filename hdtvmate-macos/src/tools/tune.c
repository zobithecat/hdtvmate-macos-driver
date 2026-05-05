#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include "channel_scan.h"
#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/*
 * hdtvmate_tune - Tune to a channel and capture/play TS data
 *
 * Usage:
 *   ./hdtvmate_tune <freq_khz>                         # Capture to stdout
 *   ./hdtvmate_tune <freq_khz> -o output.ts            # Save to file
 *   ./hdtvmate_tune <freq_khz> --udp 127.0.0.1:1234    # UDP output for VLC
 *   ./hdtvmate_tune --channel 14                        # By channel number
 *   ./hdtvmate_tune --channel 14 --atsc1                # ATSC 1.0 mode
 *
 * Playback with VLC:
 *   vlc udp://@:1234
 *
 * Playback with mpv:
 *   mpv udp://127.0.0.1:1234
 */

static volatile bool g_running = true;

static void signal_handler(int sig)
{
    (void)sig;
    printf("\nStopping...\n");
    g_running = false;
}

static void stdout_callback(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;
    capture_context_t capture;
    hdtvmate_error_t ret;

    g_log_level = LOG_INFO;

    uint32_t frequency_khz = 0;
    int channel_num = -1;
    const char *output_file = NULL;
    const char *udp_addr = NULL;
    uint16_t udp_port = 1234;
    broadcast_standard_t std = BROADCAST_ATSC3;
    uint16_t vid = 0, pid = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i+1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--udp") == 0 && i+1 < argc) {
            char *colon = strchr(argv[++i], ':');
            if (colon) {
                *colon = '\0';
                udp_addr = argv[i];
                udp_port = atoi(colon + 1);
            } else {
                udp_addr = argv[i];
            }
        } else if (strcmp(argv[i], "--channel") == 0 && i+1 < argc) {
            channel_num = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--atsc1") == 0) {
            std = BROADCAST_ATSC1;
        } else if (strcmp(argv[i], "--atsc3") == 0) {
            std = BROADCAST_ATSC3;
        } else if (strcmp(argv[i], "-v") == 0) {
            g_log_level = LOG_DEBUG;
        } else if (strcmp(argv[i], "-vv") == 0) {
            g_log_level = LOG_TRACE;
        } else if (strcmp(argv[i], "-q") == 0) {
            g_log_level = LOG_ERROR;
        } else if (argv[i][0] != '-') {
            if (vid == 0 && frequency_khz == 0) {
                /* Could be frequency or VID */
                uint32_t val = strtoul(argv[i], NULL, 0);
                if (val > 0xFFFF) {
                    frequency_khz = val;
                } else if (vid == 0) {
                    vid = (uint16_t)val;
                }
            } else if (pid == 0 && frequency_khz == 0) {
                pid = (uint16_t)strtol(argv[i], NULL, 16);
            }
        }
    }

    /* Resolve channel to frequency */
    if (channel_num >= 0) {
        frequency_khz = frequency_for_channel(channel_num);
        if (frequency_khz == 0) {
            fprintf(stderr, "Invalid channel: %d\n", channel_num);
            return 1;
        }
    }

    if (frequency_khz == 0) {
        fprintf(stderr, "Usage: %s <freq_khz> [options]\n", argv[0]);
        fprintf(stderr, "       %s --channel <num> [options]\n", argv[0]);
        fprintf(stderr, "\nOptions:\n");
        fprintf(stderr, "  -o <file>              Save TS to file\n");
        fprintf(stderr, "  --udp <addr>:<port>    Stream via UDP (for VLC)\n");
        fprintf(stderr, "  --channel <num>        Channel number (14-51)\n");
        fprintf(stderr, "  --atsc1                Use ATSC 1.0 mode\n");
        fprintf(stderr, "  --atsc3                Use ATSC 3.0 mode (default)\n");
        fprintf(stderr, "  -v                     Verbose logging\n");
        fprintf(stderr, "  -q                     Quiet (errors only)\n");
        return 1;
    }

    /* Setup signal handler for clean shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fprintf(stderr, "=== GTMedia HDTV Mate Tuner ===\n\n");
    fprintf(stderr, "Frequency: %u kHz (CH %d)\n", frequency_khz,
            channel_num >= 0 ? channel_num : 0);
    fprintf(stderr, "Standard:  %s\n", std == BROADCAST_ATSC3 ? "ATSC 3.0" : "ATSC 1.0");
    if (output_file) fprintf(stderr, "Output:    %s\n", output_file);
    if (udp_addr) fprintf(stderr, "UDP:       %s:%d\n", udp_addr, udp_port);
    if (!output_file && !udp_addr) fprintf(stderr, "Output:    stdout (pipe to player)\n");
    fprintf(stderr, "\n");

    /* Initialize hardware */
    ret = usb_init(&usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "USB init failed\n"); return 1; }

    ret = (vid && pid) ? usb_open(&usb, vid, pid) : usb_auto_detect(&usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "Device not found\n"); usb_close(&usb); return 1; }

    ret = usb_discover_endpoints(&usb);
    if (ret != HDTVMATE_OK) { usb_close(&usb); return 1; }

    ret = it9300_initialize(&bridge, &usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "Bridge init failed\n"); usb_close(&usb); return 1; }

    ret = cxd6801_create(&demod, &bridge, 0);
    if (ret != HDTVMATE_OK) goto cleanup;

    ret = cxd6801_initialize(&demod);
    if (ret != HDTVMATE_OK) goto cleanup;

    /* Tune to channel */
    fprintf(stderr, "Tuning to %u kHz...\n", frequency_khz);
    ret = cxd6801_acquire_channel(&demod, frequency_khz, std);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "Channel acquisition failed: no signal at %u kHz\n", frequency_khz);
        goto cleanup;
    }

    /* Get signal info */
    signal_stats_t stats;
    cxd6801_get_stats(&demod, &stats);
    fprintf(stderr, "Locked! SNR=%d.%02d dB, RF=%d dBm\n",
            stats.snr_db_x100 / 100, stats.snr_db_x100 % 100, stats.rf_level_dbm);

    /* Setup output */
    capture_callback_t cb = stdout_callback;
    void *cb_data = NULL;

    /* Start capture */
    memset(&capture, 0, sizeof(capture));

    if (output_file) {
        ret = output_to_file(output_file, &capture);
        if (ret != HDTVMATE_OK) goto cleanup;
    } else if (udp_addr) {
        ret = output_to_udp(udp_addr, udp_port, &capture);
        if (ret != HDTVMATE_OK) goto cleanup;
    } else {
        capture.callback = cb;
        capture.callback_data = cb_data;
    }

    ret = capture_start(&capture, &usb, &bridge,
                         capture.callback, capture.callback_data);
    if (ret != HDTVMATE_OK) goto cleanup;

    fprintf(stderr, "Capturing... Press Ctrl+C to stop.\n");

    /* Main loop - just wait */
    while (g_running) {
        usleep(500000);

        /* Periodic signal check */
        cxd6801_get_stats(&demod, &stats);
        if (!stats.locked) {
            fprintf(stderr, "WARNING: Signal lost!\n");
        }
    }

    /* Cleanup */
    capture_stop(&capture);
    fprintf(stderr, "\nCapture stats: %llu bytes received\n",
            (unsigned long long)capture.bytes_received);

cleanup:
    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);

    fprintf(stderr, "Done.\n");
    return (ret == HDTVMATE_OK) ? 0 : 1;
}
