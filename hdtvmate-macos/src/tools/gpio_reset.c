#include "usb_device.h"
#include "it9300.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * gpio_reset - Bruteforce search for CXD6801 reset GPIO on IT9300
 *
 * Toggles each GPIO register LOW→HIGH and checks if CXD6801
 * starts responding on I2C (error changes from 0x17/NACK to 0x00).
 */

extern hdtvmate_error_t br_user_bus_tx(usb_device_t *usb, uint8_t *data, int len);
extern hdtvmate_error_t br_user_bus_rx(usb_device_t *usb, uint8_t *data, int len);
extern void br_user_delay(uint32_t ms);

static int check_i2c(it9300_device_t *b)
{
    uint8_t tx[16] = {0}, rx[16] = {0};
    int seq = b->cmd_seq++;
    int il = 7;

    tx[1] = 0x00; tx[2] = 0x2A; tx[3] = (uint8_t)seq;
    tx[4] = 0x01; tx[5] = 0xD8; tx[6] = 0xFD;

    uint16_t c = 0;
    for (int j = 0; j < 3; j++)
        c += ((uint16_t)tx[2*j + 1] << 8) | tx[2*j + 2];
    c = ~c;
    tx[il] = (uint8_t)(c >> 8);
    tx[il + 1] = (uint8_t)(c & 0xFF);
    tx[0] = (uint8_t)(il + 1);

    br_user_bus_tx(b->usb, tx, il + 2);
    br_user_bus_rx(b->usb, rx, 16);
    return rx[2]; /* 0x00=success/echo, 0x17=NACK */
}

int main(void)
{
    usb_device_t usb;
    it9300_device_t bridge;
    int err;

    g_log_level = LOG_ERROR;

    if (usb_init(&usb) || usb_auto_detect(&usb) || usb_discover_endpoints(&usb)) {
        fprintf(stderr, "USB device not found\n");
        return 1;
    }
    memset(&bridge, 0, sizeof(bridge));
    bridge.usb = &usb;

    printf("=== GPIO Reset Pin Search for CXD6801 ===\n\n");

    /* Apply IT9300 init sequence to enable I2C master */
    static const struct { uint32_t a; uint8_t v; } ini[] = {
        {0x4976, 0x00}, {0x4BFB, 0x00}, {0x4978, 0x00}, {0x4977, 0x00},
        {0xF6A7, 0x07}, {0xF103, 0x07},
        {0xD8D8, 0x01}, {0xD8D9, 0x01}, {0xD8D7, 0x00}, {0xD8D7, 0x01},
        {0xDC00, 0x21}, {0xDC00, 0x21}, {0xDC00, 0x21},
        {0xDA1A, 0x00}, {0xD833, 0x01}, {0xD830, 0x00}, {0xD831, 0x01}, {0xD832, 0x00},
        {0x4976, 0x01}, {0xD8D4, 0x01}, {0xD8D5, 0x01}, {0xD8D3, 0x01},
        {0xD8B8, 0x01}, {0xD8B9, 0x01}, {0xD8B7, 0x00}, {0xD8B7, 0x01},
        {0xDA5A, 0x1F}, {0xD820, 0x01},
    };
    for (size_t i = 0; i < sizeof(ini) / sizeof(ini[0]); i++)
        it9300_write_register(&bridge, 0, ini[i].a, ini[i].v);

    err = check_i2c(&bridge);
    printf("Initial I2C: error=0x%02X (%s)\n\n", err,
           err == 0x17 ? "NACK - CXD6801 not responding" : "other");

    if (err != 0x17) {
        printf("I2C is not NACK - unexpected state. Exiting.\n");
        usb_close(&usb);
        return 0;
    }

    /* Scan GPIO registers: 0xD880 - 0xD960 (4 regs per GPIO: EN, ON, O, I) */
    printf("Scanning GPIO 0xD880 - 0xD960 (toggle LOW→HIGH)...\n");
    for (uint32_t base = 0xD880; base < 0xD960; base += 4) {
        uint8_t orig[4];
        for (int i = 0; i < 4; i++)
            it9300_read_register(&bridge, 0, base + i, &orig[i]);

        it9300_write_register(&bridge, 0, base,     0x01); /* EN = 1 */
        it9300_write_register(&bridge, 0, base + 1, 0x01); /* ON = 1 (output) */
        it9300_write_register(&bridge, 0, base + 2, 0x00); /* O = LOW (reset assert) */
        br_user_delay(200);
        it9300_write_register(&bridge, 0, base + 2, 0x01); /* O = HIGH (reset release) */
        br_user_delay(200);

        err = check_i2c(&bridge);
        if (err != 0x17) {
            printf("  >>> GPIO 0x%04X: error=0x%02X *** FOUND! ***\n", base, err);
        }

        /* Restore */
        for (int i = 0; i < 4; i++)
            it9300_write_register(&bridge, 0, base + i, orig[i]);
    }

    /* Also try some power management registers */
    printf("\nScanning power regs 0xD800 - 0xD880...\n");
    for (uint32_t addr = 0xD800; addr < 0xD880; addr++) {
        uint8_t orig = 0;
        it9300_read_register(&bridge, 0, addr, &orig);

        /* Try setting bit 0 */
        it9300_write_register(&bridge, 0, addr, orig | 0x01);
        br_user_delay(100);
        err = check_i2c(&bridge);
        if (err != 0x17) {
            printf("  >>> 0x%04X |= 0x01: error=0x%02X *** FOUND! *** (was 0x%02X)\n",
                   addr, err, orig);
        }
        it9300_write_register(&bridge, 0, addr, orig);

        /* Try setting bit 7 */
        it9300_write_register(&bridge, 0, addr, orig | 0x80);
        br_user_delay(100);
        err = check_i2c(&bridge);
        if (err != 0x17) {
            printf("  >>> 0x%04X |= 0x80: error=0x%02X *** FOUND! *** (was 0x%02X)\n",
                   addr, err, orig);
        }
        it9300_write_register(&bridge, 0, addr, orig);
    }

    printf("\nDone scanning.\n");
    usb_close(&usb);
    return 0;
}
