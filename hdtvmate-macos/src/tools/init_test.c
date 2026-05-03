#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * hdtvmate_init - Hardware initialization test
 *
 * Tests:
 * 1. USB device open
 * 2. IT9300 bridge initialization
 * 3. CXD6801 demodulator initialization
 * 4. ASCOT3 tuner initialization
 *
 * Usage:
 *   ./hdtvmate_init [VID PID]
 */

int main(int argc, char *argv[])
{
    usb_device_t usb;
    it9300_device_t bridge;
    cxd6801_device_t demod;
    hdtvmate_error_t ret;

    g_log_level = LOG_DEBUG;

    uint16_t vid = 0, pid = 0;
    if (argc >= 3) {
        vid = (uint16_t)strtol(argv[1], NULL, 16);
        pid = (uint16_t)strtol(argv[2], NULL, 16);
    }

    printf("=== GTMedia HDTV Mate Initialization Test ===\n\n");

    /* Step 1: USB */
    printf("[1/4] Opening USB device...\n");
    ret = usb_init(&usb);
    if (ret != HDTVMATE_OK) { fprintf(stderr, "USB init failed\n"); return 1; }

    if (vid && pid) {
        ret = usb_open(&usb, vid, pid);
    } else {
        ret = usb_auto_detect(&usb);
    }
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "USB device not found. Run hdtvmate_detect first.\n");
        usb_close(&usb);
        return 1;
    }

    ret = usb_discover_endpoints(&usb);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "Endpoint discovery failed\n");
        usb_close(&usb);
        return 1;
    }
    printf("  USB OK: %04x:%04x\n\n", usb.vid, usb.pid);

    /* Step 2: IT9300 bridge */
    printf("[2/4] Initializing IT9300 bridge...\n");
    ret = it9300_initialize(&bridge, &usb);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "IT9300 init failed: %d\n", ret);
        fprintf(stderr, "Check: Is firmware data loaded? Run extract_firmware.py\n");
        usb_close(&usb);
        return 1;
    }
    printf("  IT9300 OK: FW v%u.%u.%u.%u\n\n",
           (bridge.fw_version >> 24) & 0xFF,
           (bridge.fw_version >> 16) & 0xFF,
           (bridge.fw_version >> 8) & 0xFF,
           bridge.fw_version & 0xFF);

    /* Step 3: CXD6801 demodulator */
    printf("[3/4] Initializing CXD6801 demodulator...\n");
    ret = cxd6801_create(&demod, &bridge, 0);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "CXD6801 create failed: %d\n", ret);
        it9300_deinit(&bridge);
        usb_close(&usb);
        return 1;
    }

    ret = cxd6801_initialize(&demod);
    if (ret != HDTVMATE_OK) {
        fprintf(stderr, "CXD6801 init failed: %d\n", ret);
        fprintf(stderr, "Check I2C addresses in EEPROM config output above.\n");
        it9300_deinit(&bridge);
        usb_close(&usb);
        return 1;
    }
    printf("  CXD6801 OK: Chip ID=0x%04x\n\n", demod.chip_id);

    /* Step 4: Summary */
    printf("[4/4] Initialization complete!\n\n");
    printf("=== Hardware Summary ===\n");
    printf("  USB:        %04x:%04x\n", usb.vid, usb.pid);
    printf("  Bridge:     IT9300 (FW v%u.%u.%u.%u)\n",
           (bridge.fw_version >> 24) & 0xFF,
           (bridge.fw_version >> 16) & 0xFF,
           (bridge.fw_version >> 8) & 0xFF,
           bridge.fw_version & 0xFF);
    printf("  Demod:      0x%04x\n", demod.chip_id);
    printf("  State:      %s\n",
           demod.state == CXD6801_STATE_SLEEP ? "SLEEP (ready)" : "other");
    printf("  EEPROM:     board=0x%04x, rx=%d\n",
           bridge.eeprom.board_id, bridge.eeprom.rx_device_count);
    printf("\nReady for channel scan (hdtvmate_scan) or tuning (hdtvmate_tune).\n");

    /* Cleanup */
    cxd6801_deinit(&demod);
    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
