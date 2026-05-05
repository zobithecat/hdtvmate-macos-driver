/*
 * no_reset_probe - probe chip *without* triggering GPIO reset
 *
 * Hypothesis: our it9300_initialize toggles D8B7 (demodulator GPIO reset),
 * which puts a freshly-Android-woken chip back into a stuck state. This
 * tool skips that toggle entirely — only sets I2C clock + power-enable
 * GPIOs + reads firmware version, then probes 0xC0.
 *
 * Run *immediately* after detaching the device from UTM/Android.
 */

#include "usb_device.h"
#include "it9300.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern hdtvmate_error_t br_cmd_send(it9300_device_t *dev, uint16_t cmd,
                                     const uint8_t *tx_data, uint8_t tx_len,
                                     uint8_t *rx_data, uint8_t rx_len);
extern hdtvmate_error_t br_cmd_write_registers(it9300_device_t *dev,
                                                uint8_t processor,
                                                uint32_t addr,
                                                const uint8_t *values, uint8_t len);

static uint8_t read_tuner_7f(it9300_device_t *bridge)
{
    uint8_t tx[8], rx = 0xC1;
    tx[0] = 1; tx[1] = 3; tx[2] = 0xC0; tx[3] = 0x7F;
    br_cmd_send(bridge, 0x002B, tx, 4, NULL, 0);
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

    /* Bare-bones IT9300 setup — NO GPIO reset, NO I2C clock, NO power
     * toggling. Just enough to be able to issue I2C commands. */
    memset(&bridge, 0, sizeof(bridge));
    bridge.usb = &usb;
    bridge.cmd_seq = 0;
    bridge.initialized = true;

    printf("=== No-Reset Probe ===\n");
    printf("(skipping GPIO reset — chip should still be in Android-init state)\n\n");

    /* Step 1: chip ID via SLVX direct */
    uint8_t tx[8];
    uint8_t fb = 0, fd = 0;
    tx[0] = 1; tx[1] = 3; tx[2] = 0xDC; tx[3] = 0xFB;
    br_cmd_send(&bridge, 0x002B, tx, 4, NULL, 0);
    tx[0] = 1; tx[1] = 3; tx[2] = 0xDC;
    br_cmd_send(&bridge, 0x002A, tx, 3, &fb, 1);
    tx[0] = 1; tx[1] = 3; tx[2] = 0xDC; tx[3] = 0xFD;
    br_cmd_send(&bridge, 0x002B, tx, 4, NULL, 0);
    tx[0] = 1; tx[1] = 3; tx[2] = 0xDC;
    br_cmd_send(&bridge, 0x002A, tx, 3, &fd, 1);
    uint16_t chip_id = ((uint16_t)(fb & 0x03) << 8) | fd;
    printf("Chip ID: 0x%04x %s\n", chip_id,
           chip_id == 0x0396 ? "(CXD6801 alive)" : "(unexpected!)");

    /* Step 2: read SLVX 0x1A — see what state Android left */
    uint8_t v = 0xFF;
    tx[0] = 1; tx[1] = 3; tx[2] = 0xDC; tx[3] = 0x1A;
    br_cmd_send(&bridge, 0x002B, tx, 4, NULL, 0);
    tx[0] = 1; tx[1] = 3; tx[2] = 0xDC;
    br_cmd_send(&bridge, 0x002A, tx, 3, &v, 1);
    printf("SLVX reg 0x1A (current value): 0x%02x\n", v);

    /* Step 3: read SLVT bank 0 reg 0x1A */
    tx[0] = 2; tx[1] = 3; tx[2] = 0xD8; tx[3] = 0x00; tx[4] = 0x00;
    br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);  /* bank select */
    tx[0] = 1; tx[1] = 3; tx[2] = 0xD8; tx[3] = 0x1A;
    br_cmd_send(&bridge, 0x002B, tx, 4, NULL, 0);
    tx[0] = 1; tx[1] = 3; tx[2] = 0xD8;
    v = 0xFF;
    br_cmd_send(&bridge, 0x002A, tx, 3, &v, 1);
    printf("SLVT bank 0 reg 0x1A (current value): 0x%02x\n\n", v);

    /* Step 4: try 0xC0 directly without changing any state */
    printf("Step A: 0xC0 reg 0x7F (no enable touched):\n");
    v = read_tuner_7f(&bridge);
    printf("  = 0x%02x %s\n\n", v,
           v == 0xE1 ? "*** ALIVE! ***" : v == 0xC1 ? "(NACK)" : "(other)");

    if (v != 0xE1) {
        printf("Step B: SLVX 0x1A = 1, then 0xC0:\n");
        tx[0] = 2; tx[1] = 3; tx[2] = 0xDC; tx[3] = 0x1A; tx[4] = 0x01;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
        v = read_tuner_7f(&bridge);
        printf("  = 0x%02x %s\n\n", v,
               v == 0xE1 ? "*** ALIVE! ***" : v == 0xC1 ? "(NACK)" : "(other)");
    }

    if (v != 0xE1) {
        printf("Step C: SLVT 0x1A = 1 + SLVX 0x1A = 1, then 0xC0:\n");
        tx[0] = 2; tx[1] = 3; tx[2] = 0xD8; tx[3] = 0x00; tx[4] = 0x00;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
        tx[0] = 2; tx[1] = 3; tx[2] = 0xD8; tx[3] = 0x1A; tx[4] = 0x01;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
        tx[0] = 2; tx[1] = 3; tx[2] = 0xDC; tx[3] = 0x1A; tx[4] = 0x01;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
        v = read_tuner_7f(&bridge);
        printf("  = 0x%02x %s\n\n", v,
               v == 0xE1 ? "*** ALIVE! ***" : v == 0xC1 ? "(NACK)" : "(other)");
    }

    if (v != 0xE1) {
        printf("Step D: SLVT 0x1A=1 + SLVX 0x08=1, then 0xC0:\n");
        tx[0] = 2; tx[1] = 3; tx[2] = 0xD8; tx[3] = 0x00; tx[4] = 0x00;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
        tx[0] = 2; tx[1] = 3; tx[2] = 0xD8; tx[3] = 0x1A; tx[4] = 0x01;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
        tx[0] = 2; tx[1] = 3; tx[2] = 0xDC; tx[3] = 0x08; tx[4] = 0x01;
        br_cmd_send(&bridge, 0x002B, tx, 5, NULL, 0);
        v = read_tuner_7f(&bridge);
        printf("  = 0x%02x %s\n\n", v,
               v == 0xE1 ? "*** ALIVE! ***" : v == 0xC1 ? "(NACK)" : "(other)");
    }

    usb_close(&usb);
    return 0;
}
