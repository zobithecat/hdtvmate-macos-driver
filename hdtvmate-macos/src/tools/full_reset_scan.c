#include "usb_device.h"
#include "it9300.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * full_reset_scan - Exhaustive search for CXD6801 reset mechanism
 *
 * Strategy: For each register in the IT9300 GPIO/power range,
 * try toggling it and check if CXD6801 starts responding on I2C.
 *
 * After init, CMD 0x2A returns error=0x17 (NACK).
 * If any register toggle changes this to error=0x00, we found the reset pin.
 */

extern hdtvmate_error_t br_user_bus_tx(usb_device_t *usb, uint8_t *data, int len);
extern hdtvmate_error_t br_user_bus_rx(usb_device_t *usb, uint8_t *data, int len);
extern void br_user_delay(uint32_t ms);

/* Send CMD 0x2A and return error code from byte[2] */
static uint8_t i2c_probe(it9300_device_t *b)
{
    uint8_t tx[16] = {0}, rx[16] = {0};
    int s = b->cmd_seq++;
    int il = 7;
    tx[1] = 0; tx[2] = 0x2A; tx[3] = (uint8_t)s;
    tx[4] = 1; tx[5] = 0xD8; tx[6] = 0xFD;
    uint16_t c = 0;
    for (int j = 0; j < 3; j++)
        c += ((uint16_t)tx[2*j+1] << 8) | tx[2*j+2];
    c = ~c;
    tx[il] = (uint8_t)(c >> 8);
    tx[il+1] = (uint8_t)(c & 0xFF);
    tx[0] = (uint8_t)(il + 1);
    br_user_bus_tx(b->usb, tx, il + 2);
    br_user_bus_rx(b->usb, rx, 16);
    return rx[2];
}

int main(void)
{
    usb_device_t usb;
    it9300_device_t bridge;

    g_log_level = LOG_ERROR;

    if (usb_init(&usb) || usb_auto_detect(&usb) || usb_discover_endpoints(&usb)) {
        fprintf(stderr, "USB device not found\n");
        return 1;
    }
    memset(&bridge, 0, sizeof(bridge));
    bridge.usb = &usb;

    printf("=== Exhaustive CXD6801 Reset Search ===\n\n");

    /* Apply IT9300 init to enable I2C master */
    static const struct { uint32_t a; uint8_t v; } ini[] = {
        {0x4976,0},{0x4BFB,0},{0x4978,0},{0x4977,0},
        {0xF6A7,7},{0xF103,7},{0xD8D8,1},{0xD8D9,1},{0xD8D7,0},{0xD8D7,1},
        {0xDC00,0x21},{0xDC00,0x21},{0xDC00,0x21},
        {0xDA1A,0},{0xD833,1},{0xD830,0},{0xD831,1},{0xD832,0},
        {0x4976,1},{0xD8D4,1},{0xD8D5,1},{0xD8D3,1},
        {0xD8B8,1},{0xD8B9,1},{0xD8B7,0},{0xD8B7,1},
        {0xDA5A,0x1F},{0xD820,1},
    };
    for (size_t i = 0; i < sizeof(ini)/sizeof(ini[0]); i++)
        it9300_write_register(&bridge, 0, ini[i].a, ini[i].v);

    /* Verify NACK baseline */
    uint8_t baseline = i2c_probe(&bridge);
    printf("Baseline I2C error: 0x%02X (%s)\n\n",
           baseline, baseline == 0x17 ? "NACK" : "other");

    /*
     * SCAN 1: Toggle each register bit-by-bit in GPIO/power range
     * Ranges: 0xD800-0xDA00 (GPIO, TS, power), 0x4970-0x4990 (EEPROM/config)
     */
    printf("--- SCAN: Write 0x01 to registers 0xD800-0xDA80 ---\n");
    int found = 0;
    for (uint32_t addr = 0xD800; addr < 0xDA80; addr++) {
        uint8_t orig = 0;
        it9300_read_register(&bridge, 0, addr, &orig);

        /* Skip registers that are already being used for I2C master */
        if (addr >= 0xD8D7 && addr <= 0xD8D9) continue;
        if (addr == 0xDA5A || addr == 0xD820) continue;

        /* Try writing 0x01 (enable/high) */
        it9300_write_register(&bridge, 0, addr, 0x01);
        br_user_delay(50);

        uint8_t err = i2c_probe(&bridge);
        if (err != baseline) {
            printf("  0x%04X = 0x01 (was 0x%02X): error changed to 0x%02X !!!\n",
                   addr, orig, err);
            found++;
        }

        /* Restore */
        it9300_write_register(&bridge, 0, addr, orig);
    }

    if (!found) {
        printf("  (no changes found in 0xD800-0xDA80)\n");
    }

    /*
     * SCAN 2: Try toggling 0→1 for registers that were 0
     */
    printf("\n--- SCAN: Toggle 0→1→0 for zero-valued regs 0xD800-0xDA80 ---\n");
    for (uint32_t addr = 0xD800; addr < 0xDA80; addr++) {
        uint8_t orig = 0;
        it9300_read_register(&bridge, 0, addr, &orig);
        if (orig != 0) continue;  /* only test zero registers */
        if (addr >= 0xD8D7 && addr <= 0xD8D9) continue;

        /* Toggle: write 1, wait, write 0, wait, check */
        it9300_write_register(&bridge, 0, addr, 0x01);
        br_user_delay(100);
        it9300_write_register(&bridge, 0, addr, 0x00);
        br_user_delay(100);

        uint8_t err = i2c_probe(&bridge);
        if (err != baseline) {
            printf("  0x%04X toggle 0→1→0: error changed to 0x%02X !!!\n", addr, err);
            found++;
        }
    }

    /*
     * SCAN 3: Write 0xFF (all bits set) to each register
     */
    printf("\n--- SCAN: Write 0xFF to regs 0xD800-0xDA80 ---\n");
    for (uint32_t addr = 0xD800; addr < 0xDA80; addr++) {
        uint8_t orig = 0;
        it9300_read_register(&bridge, 0, addr, &orig);
        if (addr >= 0xD8D7 && addr <= 0xD8D9) continue;
        if (addr == 0xDA5A || addr == 0xD820) continue;

        it9300_write_register(&bridge, 0, addr, 0xFF);
        br_user_delay(50);

        uint8_t err = i2c_probe(&bridge);
        if (err != baseline) {
            printf("  0x%04X = 0xFF (was 0x%02X): error changed to 0x%02X !!!\n",
                   addr, orig, err);
            found++;
        }

        it9300_write_register(&bridge, 0, addr, orig);
    }

    if (!found) {
        printf("\n*** NO RESET REGISTER FOUND in GPIO/power range ***\n");
        printf("CXD6801 reset is likely external (not via IT9300 registers)\n");
    }

    printf("\nDone. Total hits: %d\n", found);
    usb_close(&usb);
    return 0;
}
