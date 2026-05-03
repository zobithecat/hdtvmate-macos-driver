/* Quick diagnostic: read multiple status registers after tune */
#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include "channel_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;
    g_log_level = LOG_WARN;  /* Quiet mode */

    uint32_t freq = argc > 1 ? atol(argv[1]) : 701000;
    
    usb_init(&usb);
    if (usb_auto_detect(&usb) != HDTVMATE_OK) { fprintf(stderr, "No device\n"); return 1; }
    usb_discover_endpoints(&usb);
    it9300_initialize(&bridge, &usb);
    cxd6801_create(&demod, &bridge, 0);
    cxd6801_initialize(&demod);
    
    /* Tune */
    cxd6801_atsc3_tune(&demod, freq, CXD6801_BW_6MHZ);
    
    /* Wait a bit */
    usleep(2000000);
    
    /* Read diagnostic registers */
    fprintf(stderr, "=== Diagnostics at %u kHz ===\n", freq);
    
    uint8_t val;
    struct { uint8_t bank; uint8_t reg; const char *name; } regs[] = {
        {0x00, 0x10, "SW_RST"},
        {0x00, 0x17, "SLVX_0x17"},
        {0x00, 0x2C, "SYS_MODE"},
        {0x00, 0x49, "reg_0x49"},
        {0x00, 0x4B, "reg_0x4B"},
        {0x00, 0xA9, "OUTPUT_MODE"},
        {0x00, 0xFE, "SOFT_RST"},
        {0x10, 0xA5, "IQ_INV"},
        {0x10, 0xD7, "reg_10_D7"},
        {0x11, 0x6A, "reg_11_6A"},
        {0x90, 0x10, "SYNC_STAT"},
        {0x90, 0x9F, "NOM_RATE_0"},
        {0x91, 0x10, "LOCK_91"},
        {0x95, 0x40, "ALP_LOCK"},
        {0x95, 0x79, "reg_95_79"},
        {0x9C, 0xFC, "reg_9C_FC"},
    };
    
    for (int i = 0; i < (int)(sizeof(regs)/sizeof(regs[0])); i++) {
        val = 0xFF;
        cxd6801_i2c_read(&demod.i2c_demod, regs[i].bank, regs[i].reg, &val, 1);
        fprintf(stderr, "  [%02X:%02X] %-12s = 0x%02X\n", regs[i].bank, regs[i].reg, regs[i].name, val);
    }
    
    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
