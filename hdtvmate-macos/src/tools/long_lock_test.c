#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "usage: %s <freq_khz>\n", argv[0]); return 1; }
    uint32_t freq = (uint32_t)atol(argv[1]);

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

    fprintf(stderr, "Tuning to %u kHz...\n", freq);
    if (cxd6801_atsc3_tune(&demod, freq, CXD6801_BW_6MHZ) != HDTVMATE_OK) return 1;

    /* Poll every 20ms for 30 seconds, count states */
    int counts[8] = {0};
    int unlock_count = 0;
    int locked_total_ms = 0;
    int max_streak_ms = 0;
    int cur_streak = 0;

    for (int i = 0; i < 1500; i++) {
        uint8_t lock = 0;
        cxd6801_i2c_read(&demod.i2c_demod, 0x90, 0x10, &lock, 1);
        uint8_t sync = lock & 0x07;
        uint8_t un = (lock >> 4) & 0x01;
        counts[sync]++;
        if (un) unlock_count++;
        if (sync >= 6 && !un) {
            locked_total_ms += 20;
            cur_streak += 20;
            if (cur_streak > max_streak_ms) max_streak_ms = cur_streak;
        } else {
            cur_streak = 0;
        }
        usleep(20000);
    }

    fprintf(stderr, "30s polling at 20ms intervals:\n");
    for (int s = 0; s < 8; s++) if (counts[s]) fprintf(stderr, "  sync_stat=%d: %d hits\n", s, counts[s]);
    fprintf(stderr, "  unlock_det set: %d hits\n", unlock_count);
    fprintf(stderr, "  total LOCKED time: %d ms\n", locked_total_ms);
    fprintf(stderr, "  max consecutive lock streak: %d ms\n", max_streak_ms);

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
