#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
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

    fprintf(stderr, "Tuning to 701 MHz...\n");
    if (cxd6801_atsc3_tune(&demod, 701000, CXD6801_BW_6MHZ) != HDTVMATE_OK) return 1;

    fprintf(stderr, "Polling lock register, ALP register, SNR, sync_stat for 30s\n");
    fprintf(stderr, "%-7s %-6s %-6s %-6s %-6s %-6s %-6s\n",
            "ms", "ld90.10", "alp95.40", "9d", "10[6]", "snr", "rf");

    for (int i = 0; i < 60; i++) {
        uint8_t lock=0, alp=0, reg9d=0;
        uint8_t buf80[6] = {0};
        cxd6801_i2c_read(&demod.i2c_demod, 0x90, 0x10, &lock, 1);
        cxd6801_i2c_read(&demod.i2c_demod, 0x95, 0x40, &alp, 1);
        cxd6801_i2c_read(&demod.i2c_demod, 0x93, 0x9d, &reg9d, 1);
        cxd6801_i2c_read(&demod.i2c_demod, 0x93, 0x80, buf80, 6);

        int32_t snr_x100=0, rf_dbm=0;
        /* Skip monitor reads to isolate any chip-disturb effects */
        (void)snr_x100; (void)rf_dbm;

        fprintf(stderr, "%-7d 0x%02x   0x%02x   0x%02x   %02x%02x%02x %4d.%02d %d\n",
                i*500, lock, alp, reg9d, buf80[0], buf80[1], buf80[2],
                snr_x100/100, abs(snr_x100)%100, rf_dbm);
        usleep(500000);
    }

    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
