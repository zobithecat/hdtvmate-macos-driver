/*
 * post_android_test - probe chip state right after Android wakes it up
 *
 * Run this immediately after closing the Android HDTV Player app in UTM
 * and detaching the USB device back to macOS. This skips most of our
 * init (no XtoSL, no TunerI2cEnable, no warm-up) and just tries to
 * read 0xC0 reg 0x7F directly. If the Android app left the chip in
 * a state where the repeater is already armed, we should see 0xE1.
 *
 * Then we can incrementally re-add init steps and pinpoint which one
 * pushes the chip back into a stuck state.
 */

#include "usb_device.h"
#include "it9300.h"
#include "cxd6801.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);

static uint8_t read_tuner_7f(it9300_device_t *bridge)
{
    uint8_t tx[8], rx = 0xC1;
    /* Set reg ptr to 0x7F at 0xC0 */
    tx[0] = 1; tx[1] = 3; tx[2] = 0xC0; tx[3] = 0x7F;
    br_cmd_send(bridge, 0x002B, tx, 4, NULL, 0);
    /* Read 1 byte from 0xC0 */
    tx[0] = 1; tx[1] = 3; tx[2] = 0xC0;
    br_cmd_send(bridge, 0x002A, tx, 3, &rx, 1);
    return rx;
}

int main(void)
{
    usb_device_t usb;
    it9300_device_t bridge;
    g_log_level = LOG_WARN;

    if (usb_init(&usb) != HDTVMATE_OK) return 1;
    if (usb_auto_detect(&usb) != HDTVMATE_OK) {
        fprintf(stderr, "device not found\n");
        return 1;
    }
    if (usb_discover_endpoints(&usb) != HDTVMATE_OK) return 1;
    if (it9300_initialize(&bridge, &usb) != HDTVMATE_OK) return 1;

    printf("=== Post-Android probe ===\n\n");
    printf("Step 1: 0xC0 read right after IT9300 init (no chip touch yet):\n");
    uint8_t v = read_tuner_7f(&bridge);
    printf("  0xC0 reg 0x7F = 0x%02x %s\n\n", v,
           v == 0xE1 ? "*** ALIVE! ***" : v == 0xC1 ? "(NACK echo - dead)" : "(unexpected)");

    /* Try various tuner registers in case 0x7F is masked by Android state */
    printf("Step 2: Sweep 0xC0 reg 0x00..0x20 (just read, no enable):\n");
    for (int r = 0; r <= 0x20; r++) {
        uint8_t tx[8], rx = 0xC1;
        tx[0] = 1; tx[1] = 3; tx[2] = 0xC0; tx[3] = (uint8_t)r;
        br_cmd_send(&bridge, 0x002B, tx, 4, NULL, 0);
        tx[0] = 1; tx[1] = 3; tx[2] = 0xC0;
        br_cmd_send(&bridge, 0x002A, tx, 3, &rx, 1);
        if (rx != 0xC1) {
            printf("  reg 0x%02x = 0x%02x %s\n", r, rx,
                   rx != 0x00 ? "*** non-NACK ***" : "");
        }
    }
    printf("\n");

    printf("Step 3: Try SLVX reg 0x1A read (Android may have left it set):\n");
    uint8_t tx[8], rx = 0xFF;
    tx[0] = 1; tx[1] = 3; tx[2] = 0xDC; tx[3] = 0x1A;
    br_cmd_send(&bridge, 0x002B, tx, 4, NULL, 0);
    tx[0] = 1; tx[1] = 3; tx[2] = 0xDC;
    br_cmd_send(&bridge, 0x002A, tx, 3, &rx, 1);
    printf("  SLVX reg 0x1A = 0x%02x\n\n", rx);

    printf("Step 4: After SLVX 0x1A=1 (Sony's TunerI2cEnable):\n");
    tx[0] = 2; tx[1] = 3; tx[2] = 0xDC; tx[3] = 0x1A; tx[4] = 0x01;
    br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
    v = read_tuner_7f(&bridge);
    printf("  0xC0 reg 0x7F = 0x%02x %s\n\n", v,
           v == 0xE1 ? "*** ALIVE! ***" : v == 0xC1 ? "(NACK)" : "(other)");

    it9300_deinit(&bridge);
    usb_close(&usb);
    return 0;
}
